#include "studyplanner.h"
#include "config.h"
#include "domain.h"

#include <QDateTime>
#include <QVariantMap>
#include <algorithm>

StudyPlanner::StudyPlanner(StoreAdapter &store) : m_store(store) {}

void StudyPlanner::configure(int newPerDay, int dayStartHour) {
    m_newPerDay = newPerDay;
    m_dayStartHour = dayStartHour;
}

const QVector<Deck> &StudyPlanner::decks() const {
    static const QVector<Deck> empty;
    return m_decks ? *m_decks : empty;
}

bool StudyPlanner::refresh(QString *storeError) {
    m_store.clearError();
    StudySnapshot next;
    next.states = m_store.states();
    next.hashes = m_store.hashes();
    next.newBudget = qMax(0, m_newPerDay - m_store.newCardsToday(m_dayStartHour));
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
    m_store.clearError();
    m_snapshot.newBudget = qMax(0, m_newPerDay - m_store.newCardsToday(m_dayStartHour));
    if (!m_store.lastError().isEmpty() && storeError)
        *storeError = m_store.lastError();
}

FsrsState StudyPlanner::stateFor(const Card &card) const {
    if (!m_snapshotValid || m_snapshot.hashes.value(card.id) != DeckIO::contentHash(card))
        return FsrsState{};
    return m_snapshot.states.value(card.id);
}

StudyPlanner::Availability StudyPlanner::availability(const QString &deckName) const {
    Availability result;
    const QVector<Deck> &allDecks = decks();
    result.duePerDeck.resize(allDecks.size());
    result.freshPerDeck.resize(allDecks.size());
    result.dueIncludingNewPerDeck.resize(allDecks.size());
    if (!m_snapshotValid)
        return result;

    struct DueEntry {
        qint64 due;
        int deck;
        Card card;
    };
    QVector<DueEntry> dueEntries;
    int remainingNew = m_snapshot.newBudget;
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
                if (remainingNew > 0) {
                    result.fresh << card;
                    ++result.freshPerDeck[deckIndex];
                    --remainingNew;
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
    return result;
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
    return available.due.size() + available.fresh.size();
}

int StudyPlanner::newCount() const {
    return availability().fresh.size();
}

int StudyPlanner::deckDue(int deckIndex) const {
    if (deckIndex < 0)
        return 0;
    const Availability available = availability();
    return available.duePerDeck.value(deckIndex) + available.freshPerDeck.value(deckIndex);
}

int StudyPlanner::deckNew(int deckIndex) const {
    if (deckIndex < 0)
        return 0;
    return availability().freshPerDeck.value(deckIndex);
}

QVector<Card> StudyPlanner::dueCards(const QString &deckName) const {
    const Availability available = availability(deckName);
    QVector<Card> cards = available.due;
    cards << available.fresh;
    return cards;
}

void StudyPlanner::reviewIds(const QString &deckName, QStringList *dueIds, QStringList *newIds) const {
    const Availability available = availability(deckName);
    // Due reviews come first so lapses are not hidden behind new material;
    // new cards then follow in deck-file order up to the daily limit.
    for (const Card &card : available.due)
        dueIds->append(card.id);
    for (const Card &card : available.fresh)
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
        entry.insert("new", available.freshPerDeck.value(i));
        entry.insert("active", deck.name == currentDeck);
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

QVariantMap StudyPlanner::statsSummary(const ReviewSummary &summary, int dueCount, int totalCards, int deckCount,
                                       int sessionReviewed, int sessionGood, double desiredRetention) const {
    QVariantMap result;
    result.insert("totalReviews", summary.total);
    result.insert("today", summary.today);
    result.insert("todayGood", summary.todayGood);
    result.insert("streakDays", summary.streakDays);
    result.insert("accuracy", summary.today > 0 ? qRound(100.0 * summary.todayGood / summary.today) : 0);
    result.insert("due", dueCount);
    result.insert("cards", totalCards);
    result.insert("decks", deckCount);
    result.insert("newToday", summary.newToday);
    result.insert("newAvailable", newCount());
    result.insert("newPerDay", m_newPerDay);
    result.insert("retention", qRound(desiredRetention * 100));
    result.insert("dayStartHour", m_dayStartHour);
    result.insert("sessionReviewed", sessionReviewed);
    result.insert("sessionGood", sessionGood);
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

int StorePlannerAdapter::newCardsToday(int dayStartHour) const {
    return m_store.newCardsToday(dayStartHour);
}

QString StorePlannerAdapter::lastError() const {
    return m_store.lastError();
}
