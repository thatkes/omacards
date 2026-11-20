// Backend owns cards, scheduling and files. QML only binds to properties and
// calls the small set of Q_INVOKABLE actions below.
#include "backend.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

namespace {
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

} // namespace

Backend::Backend(const QString &decksDirectory, const QString &databasePath, QObject *parent)
    : QObject(parent), m_store(databasePath.isEmpty() ? defaultDatabasePath() : databasePath),
      m_deckRepo(decksDirectory.isEmpty() ? defaultDecksDirectory() : decksDirectory, m_store, m_history, this) {
    connect(&m_deckRepo, &DeckRepository::reloadRequested, this, &Backend::refresh);
    connect(&m_deckRepo, &DeckRepository::errorOccurred, this, &Backend::reportError);
    connect(&m_deckRepo, &DeckRepository::historyChanged, this, &Backend::historyChanged);
    connect(&m_deckRepo, &DeckRepository::statusMessage, this, &Backend::setStatus);

    m_reviewAgainDelayMs = Config::againDelayMs();
    m_reviewAgainTimer.setSingleShot(true);
    connect(&m_reviewAgainTimer, &QTimer::timeout, this, &Backend::presentCard);
}

std::unique_ptr<Backend> Backend::fromEnvironment(QObject *parent) {
    return std::make_unique<Backend>(defaultDecksDirectory(), defaultDatabasePath(), parent);
}

bool Backend::initialize(QString *error) {
    if (!m_deckRepo.initialize(error))
        return false;

    if (!m_store.open(error)) {
        reportError(tr("Cannot open state database: %1").arg(m_store.path()));
        return false;
    }
    m_newPerDay = qBound(0, m_store.setting("new_per_day", QString::number(Config::kDefaultNewPerDay)).toInt(),
                         Config::kMaxNewPerDay);
    m_desiredRetention =
        qBound(Config::kMinRetention,
               m_store.setting("desired_retention", QString::number(Config::kDefaultRetention)).toDouble(),
               Config::kMaxRetention);
    m_dayStartHour = qBound(0, m_store.setting("day_start_hour", QString::number(Config::kDefaultDayStartHour)).toInt(),
                            Config::kMaxDayStartHour);
    m_deckRepo.watchDirectory();
    refresh();
    return true;
}

void Backend::setMathColors(const QString &questionColor, const QString &answerColor) {
    if (m_questionInk == questionColor && m_answerInk == answerColor)
        return;
    m_questionInk = questionColor;
    m_answerInk = answerColor;
    const Card *card = m_deckRepo.cardById(m_currentId);
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
    next.newBudget = qMax(0, m_newPerDay - m_store.newCardsToday(m_dayStartHour));
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
    const QVector<Deck> &decks = m_deckRepo.deckData();
    result.duePerDeck.resize(decks.size());
    result.freshPerDeck.resize(decks.size());
    result.dueIncludingNewPerDeck.resize(decks.size());
    if (!m_snapshotValid)
        return result;

    struct DueEntry {
        qint64 due;
        int deck;
        Card card;
    };
    QVector<DueEntry> dueEntries;
    int remainingNew = m_snapshot.newBudget;
    for (int deckIndex = 0; deckIndex < decks.size(); ++deckIndex) {
        const Deck &deck = decks.at(deckIndex);
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            const FsrsState state = effectiveState(card);
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

void Backend::load() {
    m_deckRepo.load(&m_currentDeck);
    refreshSnapshot();

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
    const int index = m_deckRepo.deckIndex(m_currentDeck);
    if (index < 0)
        return 0;
    return available.duePerDeck.value(index) + available.freshPerDeck.value(index);
}

int Backend::deckNew() const {
    const Availability available = availability();
    const int index = m_deckRepo.deckIndex(m_currentDeck);
    return index < 0 ? 0 : available.freshPerDeck.value(index);
}

QVariantList Backend::deckSummaries() const {
    const Availability available = availability();
    QVariantList result;
    const QVector<Deck> &decks = m_deckRepo.deckData();
    for (int i = 0; i < decks.size(); ++i) {
        const Deck &deck = decks.at(i);
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

    const Deck *deck = m_deckRepo.deckByName(m_currentDeck);
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
    const StudyQueue::Next next = m_queue.next(QDateTime::currentMSecsSinceEpoch(), &nextId, nullptr, &nextBox);

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

    const Card *card = m_deckRepo.cardById(m_currentId);
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
        const double recall = state.reps > 0 ? Fsrs::retrievability(state, QDateTime::currentMSecsSinceEpoch()) : 1.0;
        setStatus(
            tr("Review · predicted recall %1% · FSRS box %2").arg(qRound(recall * 100)).arg(qMax(1, m_reviewed + 1)));
    } else {
        setStatus(tr("Cram · box %1 of %2 · next in %3 cards")
                      .arg(m_box)
                      .arg(Config::kLeitnerBoxes)
                      .arg(Config::kLeitnerIntervals[m_box - 1]));
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

void Backend::undo() {
    if (!m_history.canUndo()) {
        setStatus(tr("Nothing to undo"));
        return;
    }
    QString error;
    QString description;
    if (!m_deckRepo.undo(&error, &description))
        return;
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
    if (!m_deckRepo.redo(&error, &description))
        return;
    refresh();
    setStatus(tr("Redid %1").arg(description));
}

QVariantList Backend::cardList() const {
    QVariantList list;
    const QString needle = m_cardFilter.trimmed().toLower();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (const Deck &deck : m_deckRepo.deckData()) {
        if (!m_currentDeck.isEmpty() && deck.name != m_currentDeck)
            continue;
        for (const Card &card : deck.cards) {
            const QString haystack = (card.question + " " + card.answer + " " + card.tags.join(' ')).toLower();
            if (!needle.isEmpty() && !haystack.contains(needle))
                continue;
            const FsrsState state = effectiveState(card);
            const bool due = Config::isDueByDay(state.due, now, m_dayStartHour);

            QVariantMap entry;
            entry.insert("id", card.id);
            entry.insert("deck", deck.name);
            entry.insert("question", card.question);
            entry.insert("answer", card.answer);
            entry.insert("type", card.type);
            entry.insert("tags", card.tags.join(", "));
            entry.insert("due", state.due);
            entry.insert("isDue", due);
            entry.insert("new", state.reps == 0);
            entry.insert("reps", state.reps);
            entry.insert("dueText", state.reps == 0 ? tr("new")
                                    : due           ? tr("due now")
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
    return m_deckRepo.searchCards(text, deckName);
}

void Backend::setCardFilter(const QString &filter) {
    if (m_cardFilter == filter)
        return;
    m_cardFilter = filter;
    emit cardFilterChanged();
    emit cardsChanged();
}

void Backend::setNewPerDay(int count) {
    const int clean = qBound(0, count, Config::kMaxNewPerDay);
    if (m_newPerDay == clean)
        return;
    m_newPerDay = clean;
    saveSetting(QStringLiteral("new_per_day"), QString::number(clean));
    if (m_snapshotValid) {
        m_store.clearError();
        m_snapshot.newBudget = qMax(0, clean - m_store.newCardsToday(m_dayStartHour));
        if (!m_store.lastError().isEmpty())
            reportError(tr("Cannot read scheduling state: %1").arg(m_store.lastError()));
    }
    emit settingsChanged();
    rebuildQueue();
}

void Backend::setDesiredRetention(double retention) {
    const double clean = qBound(Config::kMinRetention, retention, Config::kMaxRetention);
    if (qFuzzyCompare(m_desiredRetention, clean))
        return;
    m_desiredRetention = clean;
    saveSetting(QStringLiteral("desired_retention"), QString::number(clean, 'f', 2));
    emit settingsChanged();
}

void Backend::setDayStartHour(int hour) {
    const int clean = qBound(0, hour, Config::kMaxDayStartHour);
    if (m_dayStartHour == clean)
        return;
    m_dayStartHour = clean;
    saveSetting(QStringLiteral("day_start_hour"), QString::number(clean));
    emit settingsChanged();
    // The rollover changes which cards are due and which reviews count toward
    // today, so rebuild the derived counts and the current card as well.
    refresh();
    emit cardsChanged();
}

void Backend::saveSetting(const QString &key, const QString &value) {
    m_store.setSetting(key, value);
    if (!m_store.lastError().isEmpty())
        reportError(tr("Cannot save settings: %1").arg(m_store.lastError()));
}

QString Backend::mathEngine() const {
    switch (m_math.detect()) {
    case MathRenderer::Engine::LatexSvg:
        return tr("LaTeX (SVG)");
    case MathRenderer::Engine::Unicode:
    case MathRenderer::Engine::None:
        return tr("Unicode fallback");
    }
    return tr("Unicode fallback");
}

int Backend::totalCards() const {
    return m_deckRepo.totalCards();
}

void Backend::selectDeck(const QString &name) {
    if (name.isEmpty() || name == m_currentDeck || !m_deckRepo.deckByName(name))
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
    Card *card = m_deckRepo.cardById(m_currentId);
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
    if (!m_deckRepo.createDeck(name, &m_currentDeck, error, description))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::setDeckDescription(const QString &deckName, const QString &description, QString *error) {
    const QString target = deckName.isEmpty() ? m_currentDeck : deckName;
    const Deck *deck = m_deckRepo.deckByName(target);
    if (deck && deck->description == description.trimmed())
        return true;
    if (!m_deckRepo.setDeckDescription(target, description, error))
        return false;
    refresh();
    return true;
}

bool Backend::createCards(const QString &deckName, const QVector<CardDraft> &drafts, const QString *description,
                          QStringList *createdIds, QString *error) {
    if (!m_deckRepo.createCards(deckName, &m_currentDeck, drafts, description, createdIds, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::createCard(const QString &deckName, const QString &question, const QString &answer, const QString &type,
                         const QString &tags, QString *error) {
    if (!m_deckRepo.createCard(deckName, &m_currentDeck, question, answer, type, tags, error))
        return false;
    refresh();
    return true;
}

bool Backend::modifyCard(const QString &deckName, const QString &cardId, const QString &question, const QString &answer,
                         const QString &type, const QString &tags, QString *error, QString *finalId) {
    if (!m_deckRepo.modifyCard(deckName.isEmpty() ? m_currentDeck : deckName, cardId, question, answer, type, tags,
                               error, finalId))
        return false;
    refresh();
    return true;
}

bool Backend::deleteCard(const QString &deckName, const QString &cardId, QString *error) {
    if (!m_deckRepo.deleteCard(deckName.isEmpty() ? m_currentDeck : deckName, cardId, error))
        return false;
    refresh();
    return true;
}

bool Backend::removeDeck(const QString &name, QString *error) {
    if (!m_deckRepo.removeDeck(name, &m_currentDeck, error))
        return false;
    refresh();
    return true;
}

void Backend::addCard(const QString &question, const QString &answer) {
    QString error;
    if (!createCard(m_currentDeck, question, answer, Domain::toString(CardType::Basic), {}, &error))
        reportError(error);
    else
        setStatus(tr("Card added"));
}

void Backend::addCardDetails(const QString &question, const QString &answer, const QString &type, const QString &tags) {
    QString error;
    if (!createCard(m_currentDeck, question, answer, type, tags, &error))
        reportError(error);
    else
        setStatus(tr("Card added"));
}

void Backend::updateCard(const QString &cardId, const QString &question, const QString &answer, const QString &type,
                         const QString &tags) {
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

bool Backend::importCsv(const QString &path, const QString &deckName, QString *error) {
    if (!m_deckRepo.importCsv(path, deckName, &m_currentDeck, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::importApkg(const QString &path, const QString &deckName, QString *error) {
    if (!m_deckRepo.importApkg(path, deckName, &m_currentDeck, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

bool Backend::importFile(const QString &path, const QString &deckName, QString *error) {
    if (!m_deckRepo.importFile(path, deckName, &m_currentDeck, error))
        return false;
    refresh();
    selectDeck(m_currentDeck);
    return true;
}

void Backend::editDeckFile() {
    const QString path = m_deckRepo.deckPath(m_currentDeck);
    if (!path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Backend::requestShow() {
    emit showWindowRequested();
}

QString Backend::statsText() const {
    const double accuracy = m_reviewed > 0 ? 100.0 * m_correct / m_reviewed : 0.0;
    return tr("%1 reviewed · %2% correct · box %3").arg(m_reviewed).arg(qRound(accuracy)).arg(m_box);
}

QVariantMap Backend::statsSummary() const {
    const ReviewSummary summary = m_store.summary(m_dayStartHour);
    QVariantMap result;
    result.insert("totalReviews", summary.total);
    result.insert("today", summary.today);
    result.insert("todayCorrect", summary.todayCorrect);
    result.insert("streakDays", summary.streakDays);
    result.insert("accuracy", summary.today > 0 ? qRound(100.0 * summary.todayCorrect / summary.today) : 0);
    result.insert("due", m_dueCount);
    result.insert("cards", totalCards());
    result.insert("decks", m_deckRepo.deckNames().size());
    result.insert("newToday", summary.newToday);
    result.insert("newAvailable", newCount());
    result.insert("newPerDay", m_newPerDay);
    result.insert("retention", qRound(m_desiredRetention * 100));
    result.insert("dayStartHour", m_dayStartHour);
    result.insert("sessionReviewed", m_reviewed);
    result.insert("sessionCorrect", m_correct);
    return result;
}
