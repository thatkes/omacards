#include "studyplanner.h"
#include "config.h"
#include "domain.h"

#include <QDateTime>
#include <QVariantMap>
#include <algorithm>

namespace {

// DeckIO::contentHash() hashes Question, Answer and Card type. A card with
// those fields unchanged can reuse the hash computed at refresh; anything
// else goes through the stored-hash guard in stateFor().
bool sameContent(const Card &card, const Card &hashed) {
    return card.question == hashed.question && card.answer == hashed.answer && card.type.text == hashed.type.text;
}

// The configured number is the allowance for each Deck. Keeping the result keyed
// by name leaves a seam for a future per-Deck value without changing callers.
QHash<QString, int> newBudgetsFor(StudyPlanner::StoreAdapter &store, int allowance, int dayStartHour,
                                  const QVector<Deck> &decks) {
    QHash<QString, int> budgets;
    for (const Deck &deck : decks) {
        const int used = store.newCardsOnStudyDay(dayStartHour, deck.name);
        budgets.insert(deck.name, qMax(0, allowance - used));
    }
    return budgets;
}

} // namespace

StudyPlanner::StudyPlanner(StoreAdapter &store) : m_store(store) {}

void StudyPlanner::configure(int newCardAllowance, int dayStartHour) {
    m_newCardAllowance = newCardAllowance;
    m_dayStartHour = dayStartHour;
    ++m_snapshotGeneration;
}

const QVector<Deck> &StudyPlanner::decks() const {
    static const QVector<Deck> empty;
    return m_decks ? *m_decks : empty;
}

bool StudyPlanner::refresh(QString *storeError) {
    ++m_snapshotGeneration;
    m_store.clearError();
    StudySnapshot next;
    const QHash<QString, SchedulingRecord> records = m_store.schedulingRecords();
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        next.states.insert(it.key(), it.value().state);
        next.hashes.insert(it.key(), it.value().hash);
    }
    // One hash per Deck card per snapshot generation; stateFor() and the
    // availability scan reuse that decision instead of rehashing every card.
    for (const Deck &deck : decks()) {
        for (const Card &card : deck.cards) {
            next.hashedCards.insert(card.id, card);
            if (next.hashes.value(card.id) == DeckIO::contentHash(card))
                next.validIds.insert(card.id);
        }
    }
    next.newBudgetByDeck = newBudgetsFor(m_store, m_newCardAllowance, m_dayStartHour, decks());
    if (!m_store.lastError().isEmpty()) {
        // Never present a card with scheduling data we could not verify.
        m_snapshot = StudySnapshot{};
        m_snapshotValid = false;
        if (storeError)
            *storeError = m_store.lastError();
        return false;
    }
    next.now = QDateTime::currentMSecsSinceEpoch();
    m_snapshot = next;
    m_snapshotValid = true;
    return true;
}

void StudyPlanner::updateNewBudget(QString *storeError) {
    if (!m_snapshotValid)
        return;
    ++m_snapshotGeneration; // the New-card allowance is part of the memo key
    m_store.clearError();
    m_snapshot.newBudgetByDeck = newBudgetsFor(m_store, m_newCardAllowance, m_dayStartHour, decks());
    if (!m_store.lastError().isEmpty() && storeError)
        *storeError = m_store.lastError();
}

FsrsState StudyPlanner::stateFor(const Card &card) const {
    if (!m_snapshotValid)
        return FsrsState{};
    const auto stored = m_snapshot.hashes.constFind(card.id);
    if (stored == m_snapshot.hashes.cend() || stored->isEmpty())
        return FsrsState{}; // no stored hash: no need to hash the Card either
    const auto hashed = m_snapshot.hashedCards.constFind(card.id);
    if (hashed != m_snapshot.hashedCards.cend() && sameContent(card, hashed.value()))
        return m_snapshot.validIds.contains(card.id) ? m_snapshot.states.value(card.id) : FsrsState{};
    return DeckIO::contentHash(card) == *stored ? m_snapshot.states.value(card.id) : FsrsState{};
}

StudyPlanner::Availability StudyPlanner::availability(const QString &deckName) const {
    if (m_availabilityCache.snapshotGeneration == m_snapshotGeneration &&
        m_availabilityCache.decksGeneration == m_decksGeneration) {
        const auto cached = m_availabilityCache.byDeckName.constFind(deckName);
        if (cached != m_availabilityCache.byDeckName.cend())
            return *cached;
    }

    auto remember = [this, &deckName](const Availability &value) -> Availability {
        if (m_availabilityCache.snapshotGeneration != m_snapshotGeneration ||
            m_availabilityCache.decksGeneration != m_decksGeneration) {
            m_availabilityCache.byDeckName.clear();
            m_availabilityCache.snapshotGeneration = m_snapshotGeneration;
            m_availabilityCache.decksGeneration = m_decksGeneration;
        }
        m_availabilityCache.byDeckName.insert(deckName, value);
        return value;
    };

    Availability result;
    const QVector<Deck> &allDecks = decks();
    result.duePerDeck.resize(allDecks.size());
    result.newPerDeck.resize(allDecks.size());
    result.dueIncludingNewPerDeck.resize(allDecks.size());
    if (!m_snapshotValid)
        return remember(result);

    struct DueEntry {
        qint64 due;
        int deck;
        Card card;
    };
    QVector<DueEntry> dueEntries;
    QHash<QString, int> remainingNew = m_snapshot.newBudgetByDeck;
    for (int deckIndex = 0; deckIndex < allDecks.size(); ++deckIndex) {
        const Deck &deck = allDecks.at(deckIndex);
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            const FsrsState state = stateFor(card);
            // "Due" means the study day containing the FSRS time has started,
            // not that the exact millisecond has passed. The library uses the
            // same rule, so a card labelled due today is always available.
            const bool due = Config::isDueByDay(state.due, m_snapshot.now, m_dayStartHour);
            if (due)
                ++result.dueIncludingNewPerDeck[deckIndex];
            if (state.reps == 0) {
                int &deckRemaining = remainingNew[deck.name];
                if (deckRemaining > 0) {
                    result.newCards << card;
                    ++result.newPerDeck[deckIndex];
                    --deckRemaining;
                }
            } else if (due) {
                dueEntries.append({state.due, deckIndex, card});
            }
        }
    }
    std::sort(dueEntries.begin(), dueEntries.end(), [](const DueEntry &a, const DueEntry &b) {
        return a.due == b.due ? a.card.id < b.card.id : a.due < b.due;
    });
    for (const DueEntry &entry : dueEntries) {
        result.due << entry.card;
        ++result.duePerDeck[entry.deck];
    }
    return remember(result);
}

int StudyPlanner::countDue(const QString &deckName) const {
    const Availability available = availability(deckName);
    int count = 0;
    for (int due : available.dueIncludingNewPerDeck)
        count += due;
    return count;
}

int StudyPlanner::availableCount(const QString &deckName) const {
    const Availability available = availability(deckName);
    return available.due.size() + available.newCards.size();
}

int StudyPlanner::newCount(const QString &deckName) const {
    return availability(deckName).newCards.size();
}

int StudyPlanner::deckDue(int deckIndex) const {
    if (deckIndex < 0)
        return 0;
    const Availability available = availability();
    return available.duePerDeck.value(deckIndex) + available.newPerDeck.value(deckIndex);
}

int StudyPlanner::deckNew(int deckIndex) const {
    if (deckIndex < 0)
        return 0;
    return availability().newPerDeck.value(deckIndex);
}

QVector<Card> StudyPlanner::dueCards(const QString &deckName) const {
    const Availability available = availability(deckName);
    QVector<Card> cards = available.due;
    cards << available.newCards;
    return cards;
}

void StudyPlanner::reviewIds(const QString &deckName, QStringList *dueIds, QStringList *newIds) const {
    const Availability available = availability(deckName);
    // Due reviews come first so lapses are not hidden behind new material;
    // new cards then follow in deck-file order up to the daily limit.
    for (const Card &card : available.due)
        dueIds->append(card.id);
    for (const Card &card : available.newCards)
        newIds->append(card.id);
}

QVariantList StudyPlanner::deckSummaries(const QString &currentDeck) const {
    const Availability available = availability();
    QVariantList result;
    const QVector<Deck> &allDecks = decks();
    for (int i = 0; i < allDecks.size(); ++i) {
        const Deck &deck = allDecks.at(i);
        QVariantMap entry;
        entry.insert("name", deck.name);
        entry.insert("description", deck.description);
        entry.insert("cards", deck.cards.size());
        entry.insert("due", available.duePerDeck.value(i));
        entry.insert("new", available.newPerDeck.value(i));
        entry.insert("isCurrent", deck.name == currentDeck);
        result << entry;
    }
    return result;
}

QVariantList StudyPlanner::cardList(const QString &currentDeck, const QString &filter, const QString &newText,
                                    const QString &dueText) const {
    QVariantList list;
    const QString needle = filter.trimmed().toLower();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (const Deck &deck : decks()) {
        if (!currentDeck.isEmpty() && deck.name != currentDeck)
            continue;
        for (const Card &card : deck.cards) {
            const QString haystack = (card.question + " " + card.answer + " " + card.tags.join(' ')).toLower();
            if (!needle.isEmpty() && !haystack.contains(needle))
                continue;
            const FsrsState state = stateFor(card);
            const bool due = Config::isDueByDay(state.due, now, m_dayStartHour);

            QVariantMap entry;
            entry.insert("id", card.id);
            entry.insert("deck", deck.name);
            entry.insert("question", card.question);
            entry.insert("answer", card.answer);
            entry.insert("type", Domain::toString(card.type));
            entry.insert("tags", card.tags.join(", "));
            entry.insert("due", state.due);
            entry.insert("isDue", due);
            entry.insert("new", state.reps == 0);
            entry.insert("reps", state.reps);
            entry.insert("dueText", state.reps == 0 ? newText
                                    : due           ? dueText
                                                    : QDateTime::fromMSecsSinceEpoch(state.due).toString("yyyy-MM-dd"));
            list << entry;
        }
    }
    return list;
}

QVariantMap StudyPlanner::statsSummary(const StatsSummaryInputs &inputs, const QString &deckName) const {
    QVariantMap result;
    result.insert("totalRatings", inputs.summary.total);
    result.insert("studyDayRatings", inputs.summary.studyDayRatings);
    result.insert("studyDayGood", inputs.summary.studyDayGood);
    result.insert("streak", inputs.summary.streak);
    result.insert("accuracy", inputs.summary.studyDayRatings > 0
                                  ? qRound(100.0 * inputs.summary.studyDayGood / inputs.summary.studyDayRatings)
                                  : 0);
    result.insert("due", inputs.dueCount);
    result.insert("cards", inputs.totalCards);
    result.insert("decks", inputs.deckCount);
    // Daily New numbers follow the Deck, like the Home view and CLI do. The
    // other summary fields keep their whole-collection meaning.
    result.insert("newCardsOnStudyDay", m_store.newCardsOnStudyDay(m_dayStartHour, deckName));
    result.insert("newAvailable", newCount(deckName));
    result.insert("newCardAllowance", m_newCardAllowance);
    result.insert("retention", qRound(inputs.desiredRetention * 100));
    result.insert("dayStartHour", m_dayStartHour);
    result.insert("sessionReviewed", inputs.sessionReviewed);
    result.insert("sessionGood", inputs.sessionGood);
    return result;
}

StorePlannerAdapter::StorePlannerAdapter(Store &store) : m_store(store) {}

void StorePlannerAdapter::clearError() {
    m_store.clearError();
}

QHash<QString, FsrsState> StorePlannerAdapter::states() const {
    return m_store.states();
}

QHash<QString, QString> StorePlannerAdapter::hashes() const {
    return m_store.hashes();
}

QHash<QString, SchedulingRecord> StorePlannerAdapter::schedulingRecords() const {
    return m_store.schedulingRecords();
}

int StorePlannerAdapter::newCardsOnStudyDay(int dayStartHour, const QString &deckId) const {
    return m_store.newCardsOnStudyDay(dayStartHour, deckId);
}

QString StorePlannerAdapter::lastError() const {
    return m_store.lastError();
}
