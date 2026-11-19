// Backend owns cards, scheduling and files. QML only binds to properties and
// calls the small set of Q_INVOKABLE actions below.
#include "backend.h"
#include "importer.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>
#include <algorithm>

namespace {
qint64 reviewAgainDelayMs() {
    const QByteArray override = qgetenv("OMACARDS_AGAIN_DELAY_MS");
    bool ok = false;
    const qint64 value = override.toLongLong(&ok);
    return ok ? qBound<qint64>(qint64(0), value, qint64(600000)) : qint64(60000);
}

QString defaultDataDirectory() {
    // Use a deterministic base directory that does not depend on whether
    // QCoreApplication::setOrganizationName() has been called yet: the GUI
    // sets it to "omarchy", while the headless CLI path returns before that,
    // which previously made the two resolve different decks directories.
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/omacards";
}

QString defaultDecksDirectory() {
    const QString fromEnv = qEnvironmentVariable("OMACARDS_DECKS_DIR");
    return fromEnv.isEmpty() ? defaultDataDirectory() + "/decks" : fromEnv;
}

QString defaultDatabasePath() {
    const QString fromEnv = qEnvironmentVariable("OMACARDS_STATE_DB");
    return fromEnv.isEmpty() ? defaultDataDirectory() + "/state.db" : fromEnv;
}

// TeX typesets at 12pt (~16px at 96dpi) while the QML labels use 30px/24px.
// Scaling the SVG keeps inline math legible next to the surrounding text.
constexpr double kQuestionMathScale = 2.0;
constexpr double kAnswerMathScale = 1.6;

}

Backend::Backend(const QString &decksDirectory, const QString &databasePath, QObject *parent)
    : QObject(parent),
      m_decksDir(decksDirectory.isEmpty() ? defaultDecksDirectory() : decksDirectory),
      m_store(databasePath.isEmpty() ? defaultDatabasePath() : databasePath),
      m_math() {
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(120);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reloadTimer.start(); });
    connect(&m_reloadTimer, &QTimer::timeout, this, &Backend::refresh);

    m_reviewAgainDelayMs = reviewAgainDelayMs();
    m_reviewAgainTimer.setSingleShot(true);
    connect(&m_reviewAgainTimer, &QTimer::timeout, this, &Backend::presentCard);
}

bool Backend::initialize(QString *error) {
    const bool fresh = !QFileInfo::exists(m_decksDir);
    if (!QDir().mkpath(m_decksDir)) {
        reportError(tr("Cannot create deck directory: %1").arg(m_decksDir));
        if (error) *error = tr("Cannot create deck directory: %1").arg(m_decksDir);
        return false;
    }
    if (fresh) {
        Deck welcome;
        welcome.name = tr("Welcome");
        welcome.path = QDir(m_decksDir).filePath("Welcome.txt");
        Card card;
        card.deck = welcome.name;
        card.question = tr("What are the two study modes in omacards?");
        card.answer = tr("FSRS review (long-term) and cram (endless Leitner).");
        card.id = DeckIO::cardId(card);
        welcome.cards << card;
        Card mathCard;
        mathCard.deck = welcome.name;
        mathCard.question = tr("Use a keyboard shortcut to flip a card. Math like \\(x^2\\) works too.");
        mathCard.answer = tr("Press Space or the right arrow. Every shortcut is in the ? overlay.");
        mathCard.id = DeckIO::cardId(mathCard);
        welcome.cards << mathCard;
        DeckIO::save(welcome);
    }

    if (!m_store.open(error)) {
        reportError(tr("Cannot open state database: %1").arg(m_store.path()));
        return false;
    }
    m_newPerDay = qBound(0, m_store.setting("new_per_day", "20").toInt(), 9999);
    m_desiredRetention = qBound(0.70, m_store.setting("desired_retention", "0.9").toDouble(), 0.99);
    m_watcher.addPath(m_decksDir);
    refresh();
    return true;
}

void Backend::setMathColors(const QString &questionColor, const QString &answerColor) {
    if (m_questionInk == questionColor && m_answerInk == answerColor)
        return;
    m_questionInk = questionColor;
    m_answerInk = answerColor;
    const Card *card = cardById(m_currentId);
    if (!card)
        return;
    m_question = m_math.render(DeckIO::displayQuestion(*card), m_questionInk, kQuestionMathScale);
    m_answer = m_math.render(DeckIO::displayAnswer(*card), m_answerInk, kAnswerMathScale);
    emit cardChanged();
}

bool Backend::refreshSnapshot() {
    m_store.clearError();
    StudySnapshot next;
    next.states = m_store.states();
    next.hashes = m_store.hashes();
    next.newBudget = qMax(0, m_newPerDay - m_store.newCardsToday());
    if (!m_store.lastError().isEmpty()) {
        // Never present a card with scheduling data we could not verify.
        m_snapshot = StudySnapshot{};
        m_snapshotValid = false;
        reportError(tr("Cannot read scheduling state: %1").arg(m_store.lastError()));
        return false;
    }
    next.now = QDateTime::currentMSecsSinceEpoch();
    m_snapshot = next;
    m_snapshotValid = true;
    return true;
}

FsrsState Backend::effectiveState(const Card &card) const {
    if (!m_snapshotValid || m_snapshot.hashes.value(card.id) != DeckIO::contentHash(card))
        return FsrsState{};
    return m_snapshot.states.value(card.id);
}

Backend::Availability Backend::availability(const QString &deckName) const {
    Availability result;
    result.duePerDeck.resize(m_decks.size());
    result.freshPerDeck.resize(m_decks.size());
    result.dueIncludingNewPerDeck.resize(m_decks.size());
    if (!m_snapshotValid)
        return result;

    struct DueEntry {
        qint64 due;
        int deck;
        Card card;
    };
    QVector<DueEntry> dueEntries;
    int remainingNew = m_snapshot.newBudget;
    for (int deckIndex = 0; deckIndex < m_decks.size(); ++deckIndex) {
        const Deck &deck = m_decks.at(deckIndex);
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            const FsrsState state = effectiveState(card);
            if (state.due <= m_snapshot.now)
                ++result.dueIncludingNewPerDeck[deckIndex];
            if (state.reps == 0) {
                if (remainingNew > 0) {
                    result.fresh << card;
                    ++result.freshPerDeck[deckIndex];
                    --remainingNew;
                }
            } else if (state.due <= m_snapshot.now) {
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

void Backend::load() {
    QString error;
    const bool hadDecks = !m_deckNames.isEmpty();
    const auto decks = DeckIO::loadAll(m_decksDir, &error);
    if (!error.isEmpty())
        reportError(error);
    m_decks = decks;
    m_deckNames.clear();
    for (const Deck &deck : m_decks)
        m_deckNames << deck.name;
    if (!m_deckNames.contains(m_currentDeck))
        m_currentDeck = m_deckNames.isEmpty() ? QString() : m_deckNames.first();

    refreshSnapshot();

    // Only prune sidecar rows after a clean, conclusive load. loadAll() skips
    // deck files it cannot read; pruning then would silently erase their FSRS
    // progress. An intentional last-deck deletion prunes in removeDeck().
    if (error.isEmpty() && (!m_decks.isEmpty() || !hadDecks)) {
        QSet<QString> ids;
        for (const Deck &deck : m_decks)
            for (const Card &card : deck.cards)
                ids << card.id;
        m_store.removeExcept(ids);
        if (!m_store.lastError().isEmpty())
            reportError(tr("Cannot clean scheduling state: %1").arg(m_store.lastError()));
    }

    emit decksChanged();
    emit currentDeckChanged();
    emit cardsChanged();
    dueCountChanged();
}

int Backend::countDue(const QString &deckName) const {
    const Availability available = availability(deckName);
    int count = 0;
    for (int due : available.dueIncludingNewPerDeck)
        count += due;
    return count;
}

int Backend::availableCount(const QString &deckName) const {
    const Availability available = availability(deckName);
    return available.due.size() + available.fresh.size();
}

void Backend::refreshDueCount() {
    // Pending Again repeats are not "due" in the database yet, but they are
    // still work waiting in this session.
    m_dueCount = availableCount() + m_queue.pendingRepeatCount();
    emit dueCountChanged();
}

void Backend::refresh() {
    load();
    rebuildQueue();
}

int Backend::newCount() const {
    return availability().fresh.size();
}

int Backend::deckDue() const {
    const Availability available = availability();
    const int index = deckIndex(m_currentDeck);
    if (index < 0)
        return 0;
    return available.duePerDeck.value(index) + available.freshPerDeck.value(index);
}

int Backend::deckNew() const {
    const Availability available = availability();
    const int index = deckIndex(m_currentDeck);
    return index < 0 ? 0 : available.freshPerDeck.value(index);
}

QVariantList Backend::deckSummaries() const {
    const Availability available = availability();
    QVariantList result;
    for (int i = 0; i < m_decks.size(); ++i) {
        const Deck &deck = m_decks.at(i);
        QVariantMap entry;
        entry.insert("name", deck.name);
        entry.insert("description", deck.description);
        entry.insert("cards", deck.cards.size());
        entry.insert("due", available.duePerDeck.value(i));
        entry.insert("new", available.freshPerDeck.value(i));
        entry.insert("active", deck.name == m_currentDeck);
        result << entry;
    }
    return result;
}

void Backend::rebuildQueue() {
    // A watcher refresh can rebuild the queue while a repeat is pending. Keep
    // those deadlines for cards that are still available; explicit deck and
    // mode changes clear them instead.
    const QHash<QString, qint64> pending = m_queue.pendingRepeats();
    m_reviewAgainTimer.stop();
    m_currentId.clear();

    const Deck *deck = deckByName(m_currentDeck);
    if (deck && Domain::modeFromString(m_mode) == StudyMode::Cram) {
        m_queue.buildCram(deck->cards);
    } else if (deck) {
        const Availability available = availability(m_currentDeck);
        // Due reviews come first so lapses are not hidden behind new material;
        // new cards then follow in deck-file order up to the daily limit.
        QStringList dueIds;
        QStringList freshIds;
        for (const Card &card : available.due)
            dueIds << card.id;
        for (const Card &card : available.fresh)
            freshIds << card.id;
        m_queue.buildReview(dueIds, freshIds, pending);
    } else {
        m_queue.clear();
    }

    refreshDueCount();
    presentCard();
}

void Backend::scheduleNextRepeat() {
    m_reviewAgainTimer.stop();
    if (Domain::modeFromString(m_mode) != StudyMode::Review)
        return;
    const qint64 wait = m_queue.nextWait(QDateTime::currentMSecsSinceEpoch());
    if (wait > 0)
        m_reviewAgainTimer.start(wait);
}

void Backend::presentCard() {
    m_showingAnswer = false;
    m_waitingForRepeat = false;
    const bool cram = Domain::modeFromString(m_mode) == StudyMode::Cram;
    QString nextId;
    int nextBox = 0;
    const StudyQueue::Next next = m_queue.next(
        QDateTime::currentMSecsSinceEpoch(), &nextId, nullptr, &nextBox);

    if (next == StudyQueue::Next::Finished) {
        m_currentId.clear();
        m_question.clear();
        m_answer.clear();
        m_reviewAgainTimer.stop();
        setStatus(cram ? tr("No cards yet. Press N to add one.")
                       : tr("All caught up. Press C to cram or N to add a card."));
        emit cardChanged();
        return;
    }
    if (next == StudyQueue::Next::Waiting) {
        scheduleNextRepeat();
        m_waitingForRepeat = true;
        m_currentId.clear();
        m_question.clear();
        m_answer.clear();
        setStatus(tr("Repeating a card in a moment…"));
        emit cardChanged();
        return;
    }

    m_currentId = nextId;
    if (cram)
        m_box = nextBox;
    scheduleNextRepeat();

    const Card *card = cardById(m_currentId);
    if (!card) {
        m_queue.remove(m_currentId);
        m_currentId.clear();
        presentCard();
        return;
    }
    m_question = m_math.render(DeckIO::displayQuestion(*card), m_questionInk, kQuestionMathScale);
    m_answer = m_math.render(DeckIO::displayAnswer(*card), m_answerInk, kAnswerMathScale);

    if (Domain::modeFromString(m_mode) == StudyMode::Review) {
        const FsrsState state = effectiveState(*card);
        const double recall = state.reps > 0
            ? Fsrs::retrievability(state, QDateTime::currentMSecsSinceEpoch())
            : 1.0;
        setStatus(tr("Review · predicted recall %1% · FSRS box %2")
                      .arg(qRound(recall * 100))
                      .arg(qMax(1, m_reviewed + 1)));
    } else {
        static const int intervals[] = {1, 2, 4, 8, 16};
        setStatus(tr("Cram · box %1 of 5 · next in %2 cards")
                      .arg(m_box).arg(intervals[m_box - 1]));
    }
    emit statsChanged();
    emit cardChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void Backend::reportError(const QString &message) {
    setStatus(message);
    emit errorOccurred(message);
}

void Backend::recordHistory(const QString &path, const QByteArray &before, bool existedBefore,
                            const QString &description) {
    if (m_history.record(path, before, existedBefore, description))
        emit historyChanged();
}

bool Backend::commitDeckEdit(Deck *deck, const Deck &original, const QByteArray &before,
                             const QString &description, QString *error) {
    if (!DeckIO::save(*deck, error)) {
        *deck = original;
        return false;
    }
    recordHistory(deck->path, before, true, description);
    refresh();
    return true;
}

void Backend::undo() {
    if (!m_history.canUndo()) {
        setStatus(tr("Nothing to undo"));
        return;
    }
    QString error;
    QString description;
    if (!m_history.undo(&error, &description)) {
        reportError(error);
        return;
    }
    emit historyChanged();
    refresh();
    setStatus(tr("Undid %1").arg(description));
}

void Backend::redo() {
    if (!m_history.canRedo()) {
        setStatus(tr("Nothing to redo"));
        return;
    }
    QString error;
    QString description;
    if (!m_history.redo(&error, &description)) {
        reportError(error);
        return;
    }
    emit historyChanged();
    refresh();
    setStatus(tr("Redid %1").arg(description));
}

QVariantList Backend::cardList() const {
    QVariantList list;
    const QString needle = m_cardFilter.trimmed().toLower();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (const Deck &deck : m_decks) {
        if (!m_currentDeck.isEmpty() && deck.name != m_currentDeck)
            continue;
        for (const Card &card : deck.cards) {
            const QString haystack = (card.question + " " + card.answer + " " + card.tags.join(' ')).toLower();
            if (!needle.isEmpty() && !haystack.contains(needle))
                continue;
            const FsrsState state = effectiveState(card);

            QVariantMap entry;
            entry.insert("id", card.id);
            entry.insert("deck", deck.name);
            entry.insert("question", card.question);
            entry.insert("answer", card.answer);
            entry.insert("type", card.type);
            entry.insert("tags", card.tags.join(", "));
            entry.insert("due", state.due);
            entry.insert("isDue", state.due <= now);
            entry.insert("new", state.reps == 0);
            entry.insert("reps", state.reps);
            entry.insert("dueText", state.reps == 0
                             ? tr("new")
                             : state.due <= now
                                 ? tr("due now")
                                 : QDateTime::fromMSecsSinceEpoch(state.due).toString("yyyy-MM-dd"));
            list << entry;
        }
    }
    return list;
}

QVector<Card> Backend::dueCards(const QString &deckName) const {
    const Availability available = availability(deckName);
    QVector<Card> cards = available.due;
    cards << available.fresh;
    return cards;
}

QVector<Card> Backend::searchCards(const QString &text, const QString &deckName) const {
    QVector<Card> result;
    const QString needle = text.trimmed().toLower();
    for (const Deck &deck : m_decks) {
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            const QString haystack = (card.question + " " + card.answer + " " + card.tags.join(' ')).toLower();
            if (needle.isEmpty() || haystack.contains(needle))
                result << card;
        }
    }
    return result;
}

void Backend::setCardFilter(const QString &filter) {
    if (m_cardFilter == filter)
        return;
    m_cardFilter = filter;
    emit cardFilterChanged();
    emit cardsChanged();
}

void Backend::setNewPerDay(int count) {
    const int clean = qBound(0, count, 9999);
    if (m_newPerDay == clean)
        return;
    m_newPerDay = clean;
    saveSetting(QStringLiteral("new_per_day"), QString::number(clean));
    if (m_snapshotValid) {
        m_store.clearError();
        m_snapshot.newBudget = qMax(0, clean - m_store.newCardsToday());
        if (!m_store.lastError().isEmpty())
            reportError(tr("Cannot read scheduling state: %1").arg(m_store.lastError()));
    }
    emit settingsChanged();
    rebuildQueue();
}

void Backend::setDesiredRetention(double retention) {
    const double clean = qBound(0.70, retention, 0.99);
    if (qFuzzyCompare(m_desiredRetention, clean))
        return;
    m_desiredRetention = clean;
    saveSetting(QStringLiteral("desired_retention"), QString::number(clean, 'f', 2));
    emit settingsChanged();
}

void Backend::saveSetting(const QString &key, const QString &value) {
    m_store.setSetting(key, value);
    if (!m_store.lastError().isEmpty())
        reportError(tr("Cannot save settings: %1").arg(m_store.lastError()));
}

QString Backend::mathEngine() const {
    return m_math.hasRenderer() ? tr("LaTeX (SVG)") : tr("Unicode fallback");
}

int Backend::totalCards() const {
    int total = 0;
    for (const Deck &deck : m_decks)
        total += deck.cards.size();
    return total;
}

Deck *Backend::deckByName(const QString &name) {
    for (Deck &deck : m_decks)
        if (deck.name == name)
            return &deck;
    return nullptr;
}

const Deck *Backend::deckByName(const QString &name) const {
    for (const Deck &deck : m_decks)
        if (deck.name == name)
            return &deck;
    return nullptr;
}

int Backend::deckIndex(const QString &name) const {
    for (int i = 0; i < m_decks.size(); ++i)
        if (m_decks.at(i).name == name)
            return i;
    return -1;
}

Card *Backend::cardById(const QString &id) {
    for (Deck &deck : m_decks)
        for (Card &card : deck.cards)
            if (card.id == id)
                return &card;
    return nullptr;
}

void Backend::selectDeck(const QString &name) {
    if (name.isEmpty() || name == m_currentDeck || !deckByName(name))
        return;
    m_currentDeck = name;
    emit currentDeckChanged();
    emit cardsChanged();
    emit decksChanged();
    rebuildQueue();
}

void Backend::setMode(const QString &mode) {
    if (mode == m_mode)
        return;
    m_mode = Domain::toString(Domain::modeFromString(mode));
    m_reviewed = 0;
    m_correct = 0;
    emit modeChanged();
    emit statsChanged();
    rebuildQueue();
}

void Backend::flip() {
    if (!hasCard())
        return;
    m_showingAnswer = !m_showingAnswer;
    emit cardChanged();
}

void Backend::rate(int rating) {
    if (!hasCard())
        return;
    const Rating parsed = Domain::ratingFromInt(rating);
    if (parsed == Rating::Invalid)
        return;
    ++m_reviewed;
    if (parsed == Rating::Good)
        ++m_correct;
    if (Domain::modeFromString(m_mode) == StudyMode::Cram)
        answerCram(parsed);
    else
        answerReview(parsed);
    emit statsChanged();
    presentCard();
    emit cardsChanged();
    emit decksChanged();
}

void Backend::answerReview(Rating rating) {
    Card *card = cardById(m_currentId);
    if (!card)
        return;
    const QString hash = DeckIO::contentHash(*card);
    const FsrsState state = effectiveState(*card);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool wasNew = state.reps == 0;
    const ReviewResult result = Fsrs::review(state, Domain::toInt(rating), now, m_desiredRetention);
    m_store.clearError();
    m_store.save(card->id, hash, result.state);
    m_store.logReview(card->id, Domain::toInt(rating), now, result.state, result.intervalDays, wasNew);
    if (!m_store.lastError().isEmpty()) {
        reportError(tr("Cannot save review: %1").arg(m_store.lastError()));
    } else if (m_snapshotValid) {
        // Keep the in-memory snapshot in step with the write so later counts
        // do not have to re-read the whole database.
        m_snapshot.states.insert(card->id, result.state);
        m_snapshot.hashes.insert(card->id, hash);
        m_snapshot.newBudget = qMax(0, m_snapshot.newBudget - (wasNew ? 1 : 0));
        m_snapshot.now = now;
    }
    m_queue.remove(card->id);
    if (rating == Rating::Again) {
        // Keep the FSRS due date above, but give the card one more appearance
        // in this sitting after a short break.
        m_queue.repeatAfter(card->id, now + m_reviewAgainDelayMs);
    }
    refreshDueCount();
}

void Backend::answerCram(Rating rating) {
    m_queue.advanceCram(m_currentId, rating, &m_box);
}

void Backend::showNext() {
    if (m_showingAnswer)
        flip();
}

void Backend::newDeck(const QString &name) {
    QString error;
    if (!createDeck(name, &error))
        reportError(error);
}

bool Backend::createDeck(const QString &name, QString *error, const QString &description) {
    const QString clean = name.trimmed();
    if (clean.isEmpty()) {
        if (error) *error = tr("Deck name cannot be empty");
        return false;
    }
    if (m_deckNames.contains(clean)) {
        if (error) *error = tr("Deck already exists: %1").arg(clean);
        return false;
    }
    Deck deck;
    deck.name = clean;
    deck.description = description.trimmed();
    deck.path = QDir(m_decksDir).filePath(DeckIO::safeFileName(clean) + ".txt");
    for (const Deck &existing : m_decks) {
        if (QFileInfo(existing.path).absoluteFilePath() == QFileInfo(deck.path).absoluteFilePath()) {
            if (error) *error = tr("Deck file already used by %1").arg(existing.name);
            return false;
        }
    }
    // An unreadable file is not in m_decks; never silently overwrite it.
    if (QFileInfo::exists(deck.path)) {
        if (error) *error = tr("A deck file already exists at %1").arg(deck.path);
        return false;
    }
    const bool existed = false;
    const QByteArray before = existed ? FileHistory::readBytes(deck.path) : QByteArray();
    if (!DeckIO::save(deck, error))
        return false;
    recordHistory(deck.path, before, existed, tr("creating deck %1").arg(clean));
    refresh();
    selectDeck(clean);
    return true;
}

bool Backend::setDeckDescription(const QString &deckName, const QString &description, QString *error) {
    Deck *deck = deckByName(deckName.isEmpty() ? m_currentDeck : deckName);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(deckName);
        return false;
    }
    const QString clean = description.trimmed();
    if (deck->description == clean)
        return true;
    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(deck->path);
    deck->description = clean;
    return commitDeckEdit(deck, original, before, tr("editing deck description"), error);
}

bool Backend::createCards(const QString &deckName, const QVector<CardDraft> &drafts,
                          const QString *description, QStringList *createdIds, QString *error) {
    const QString target = deckName.isEmpty() ? m_currentDeck : deckName.trimmed();
    if (target.isEmpty()) {
        if (error) *error = tr("Give a deck name");
        return false;
    }
    if (drafts.isEmpty()) {
        if (error) *error = tr("No cards to add");
        return false;
    }
    for (int i = 0; i < drafts.size(); ++i) {
        const CardDraft &draft = drafts.at(i);
        if (draft.question.trimmed().isEmpty()) {
            if (error) *error = tr("cards[%1] needs a question").arg(i);
            return false;
        }
        if (Domain::cardTypeFromString(draft.type) == CardType::Invalid) {
            if (error) *error = tr("cards[%1] has an invalid type: %2").arg(i).arg(draft.type);
            return false;
        }
    }

    if (!deckByName(target) && !createDeck(target, error, description ? *description : QString()))
        return false;
    Deck *deck = deckByName(target);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(target);
        return false;
    }

    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(deck->path);
    // An absent or empty description must not erase one already on the deck.
    if (description && !description->trimmed().isEmpty())
        deck->description = description->trimmed();

    QVector<Card> cards;
    cards.reserve(drafts.size());
    for (const CardDraft &draft : drafts) {
        Card card;
        card.deck = deck->name;
        card.question = draft.question.trimmed();
        card.answer = draft.answer.trimmed();
        card.type = Domain::toString(Domain::cardTypeFromString(draft.type));
        for (const QString &tag : draft.tags) {
            const QString clean = tag.trimmed();
            if (!clean.isEmpty() && !card.tags.contains(clean))
                card.tags << clean;
        }
        card.id = DeckIO::cardId(card);
        cards << card;
    }

    deck->cards << cards;
    if (!commitDeckEdit(deck, original, before, tr("adding %1 cards").arg(cards.size()), error))
        return false;
    if (createdIds)
        for (const Card &card : cards)
            createdIds->append(card.id);
    return true;
}

bool Backend::createCard(const QString &deckName, const QString &question, const QString &answer,
                         const QString &type, const QString &tags, QString *error) {
    Deck *deck = deckByName(deckName.isEmpty() ? m_currentDeck : deckName);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(deckName);
        return false;
    }
    if (question.trimmed().isEmpty()) {
        if (error) *error = tr("A card needs a question");
        return false;
    }
    Card card;
    card.deck = deck->name;
    card.question = question.trimmed();
    card.answer = answer.trimmed();
    card.type = Domain::toString(Domain::cardTypeFromString(type));
    for (const QString &tag : tags.split(',', Qt::SkipEmptyParts)) {
        const QString clean = tag.trimmed();
        if (!clean.isEmpty() && !card.tags.contains(clean))
            card.tags << clean;
    }
    card.id = DeckIO::cardId(card);
    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(deck->path);
    deck->cards << card;
    return commitDeckEdit(deck, original, before, tr("adding a card"), error);
}

bool Backend::modifyCard(const QString &deckName, const QString &cardId, const QString &question,
                         const QString &answer, const QString &type, const QString &tags,
                         QString *error, QString *finalId) {
    Deck *deck = deckByName(deckName.isEmpty() ? m_currentDeck : deckName);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(deckName);
        return false;
    }
    Card *card = nullptr;
    for (Card &candidate : deck->cards)
        if (candidate.id == cardId)
            card = &candidate;
    if (!card) {
        if (error) *error = tr("No such card: %1").arg(cardId);
        return false;
    }
    if (question.trimmed().isEmpty()) {
        if (error) *error = tr("A card needs a question");
        return false;
    }

    const Deck original = *deck;
    card->question = question.trimmed();
    card->answer = answer.trimmed();
    card->type = Domain::toString(Domain::cardTypeFromString(type));
    card->tags.clear();
    for (const QString &tag : tags.split(',', Qt::SkipEmptyParts)) {
        const QString clean = tag.trimmed();
        if (!clean.isEmpty() && !card->tags.contains(clean))
            card->tags << clean;
    }
    // An explicit id pinned in the file survives edits; a generated id follows
    // the content so the card resets its schedule when it changes.
    if (!card->explicitId)
        card->id = DeckIO::cardId(*card);
    if (finalId)
        *finalId = card->id;

    const QByteArray before = FileHistory::readBytes(deck->path);
    return commitDeckEdit(deck, original, before, tr("editing a card"), error);
}

bool Backend::deleteCard(const QString &deckName, const QString &cardId, QString *error) {
    Deck *deck = deckByName(deckName.isEmpty() ? m_currentDeck : deckName);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(deckName);
        return false;
    }
    const auto found = std::find_if(deck->cards.begin(), deck->cards.end(), [&](const Card &card) {
        return card.id == cardId;
    });
    if (found == deck->cards.end()) {
        if (error) *error = tr("No such card: %1").arg(cardId);
        return false;
    }
    const Deck original = *deck;
    const QByteArray before = FileHistory::readBytes(deck->path);
    deck->cards.erase(found);
    return commitDeckEdit(deck, original, before, tr("removing a card"), error);
}

bool Backend::removeDeck(const QString &name, QString *error) {
    const Deck *deck = deckByName(name);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(name);
        return false;
    }
    const QString path = deck->path;
    const QByteArray before = FileHistory::readBytes(path);
    if (!QFile::remove(path)) {
        if (error) *error = tr("Cannot remove deck file: %1").arg(path);
        return false;
    }
    recordHistory(path, before, true, tr("deleting deck %1").arg(name));
    if (m_currentDeck == name)
        m_currentDeck.clear();
    refresh();
    // An intentional deletion may remove the last deck; load() deliberately
    // skips pruning in that case, so do it here instead.
    QSet<QString> ids;
    for (const Deck &remaining : m_decks)
        for (const Card &card : remaining.cards)
            ids << card.id;
    m_store.removeExcept(ids);
    if (!m_store.lastError().isEmpty())
        reportError(tr("Cannot clean scheduling state: %1").arg(m_store.lastError()));
    return true;
}

void Backend::addCard(const QString &question, const QString &answer) {
    QString error;
    if (!createCard(m_currentDeck, question, answer, Domain::toString(CardType::Basic), {}, &error))
        reportError(error);
    else
        setStatus(tr("Card added"));
}

void Backend::addCardDetails(const QString &question, const QString &answer, const QString &type,
                             const QString &tags) {
    QString error;
    if (!createCard(m_currentDeck, question, answer, type, tags, &error))
        reportError(error);
    else
        setStatus(tr("Card added"));
}

void Backend::updateCard(const QString &cardId, const QString &question, const QString &answer,
                         const QString &type, const QString &tags) {
    QString error;
    if (!modifyCard(m_currentDeck, cardId, question, answer, type, tags, &error))
        reportError(error);
    else
        setStatus(tr("Card updated"));
}

void Backend::removeCurrentCard() {
    if (m_currentId.isEmpty())
        return;
    removeCard(m_currentId);
}

void Backend::removeCard(const QString &cardId) {
    QString error;
    if (!deleteCard(m_currentDeck, cardId, &error))
        reportError(error);
    else
        setStatus(tr("Card removed"));
}

void Backend::deleteDeck(const QString &name) {
    QString error;
    if (!removeDeck(name, &error))
        reportError(error);
    else
        setStatus(tr("Deck removed"));
}

void Backend::deleteCurrentDeck() {
    if (m_currentDeck.isEmpty())
        return;
    deleteDeck(m_currentDeck);
}

void Backend::importFrom(const QString &path, const QString &deckName) {
    QString error;
    if (!importFile(path, deckName.isEmpty() ? m_currentDeck : deckName, &error))
        reportError(error);
    else
        setStatus(tr("Imported %1").arg(QFileInfo(path).fileName()));
}

bool Backend::commitImport(const DeckImport::Result &result, QString *error) {
    if (result.cards.isEmpty()) {
        setStatus(tr("Imported 0 cards"));
        return true;
    }
    // Only a new deck receives an imported description; an existing deck keeps
    // the description its owner wrote.
    QString description;
    const QString *descriptionPtr = nullptr;
    if (!deckByName(result.deckName) && !result.description.isEmpty()) {
        description = result.description;
        descriptionPtr = &description;
    }
    if (!createCards(result.deckName, result.cards, descriptionPtr, nullptr, error))
        return false;
    setStatus(tr("Imported %1 cards").arg(result.cards.size()));
    return true;
}

bool Backend::importCsv(const QString &path, const QString &deckName, QString *error) {
    DeckImport::Result result;
    return DeckImport::csv(path, deckName, &result, error) && commitImport(result, error);
}

bool Backend::importApkg(const QString &path, const QString &deckName, QString *error) {
    DeckImport::Result result;
    return DeckImport::apkg(path, deckName, m_decksDir, &result, error) && commitImport(result, error);
}

bool Backend::importFile(const QString &path, const QString &deckName, QString *error) {
    DeckImport::Result result;
    return DeckImport::file(path, deckName, m_decksDir, &result, error) && commitImport(result, error);
}

void Backend::editDeckFile() {
    const Deck *deck = deckByName(m_currentDeck);
    if (deck)
        QDesktopServices::openUrl(QUrl::fromLocalFile(deck->path));
}

void Backend::requestShow() {
    emit showWindowRequested();
}

QString Backend::statsText() const {
    const double accuracy = m_reviewed > 0 ? 100.0 * m_correct / m_reviewed : 0.0;
    return tr("%1 reviewed · %2% correct · box %3")
        .arg(m_reviewed).arg(qRound(accuracy)).arg(m_box);
}

QVariantMap Backend::statsSummary() const {
    const ReviewSummary summary = m_store.summary();
    QVariantMap result;
    result.insert("totalReviews", summary.total);
    result.insert("today", summary.today);
    result.insert("todayCorrect", summary.todayCorrect);
    result.insert("streakDays", summary.streakDays);
    result.insert("accuracy", summary.today > 0
                      ? qRound(100.0 * summary.todayCorrect / summary.today)
                      : 0);
    result.insert("due", m_dueCount);
    result.insert("cards", totalCards());
    result.insert("decks", m_deckNames.size());
    result.insert("newToday", summary.newToday);
    result.insert("newAvailable", newCount());
    result.insert("newPerDay", m_newPerDay);
    result.insert("retention", qRound(m_desiredRetention * 100));
    result.insert("sessionReviewed", m_reviewed);
    result.insert("sessionCorrect", m_correct);
    return result;
}
