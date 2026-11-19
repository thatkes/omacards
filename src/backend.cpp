// Backend owns cards, scheduling and files. QML only binds to properties and
// calls the small set of Q_INVOKABLE actions below.
#include "backend.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QCryptographicHash>
#include <algorithm>

namespace {
template <typename Replacement>
QString replaceRegex(const QString &text, const QRegularExpression &expression, Replacement replacement) {
    QString out;
    qsizetype last = 0;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        out += text.mid(last, match.capturedStart() - last) + replacement(match);
        last = match.capturedEnd();
    }
    return out + text.mid(last);
}

QString defaultDecksDirectory() {
    const QString fromEnv = qEnvironmentVariable("OMACARDS_DECKS_DIR");
    return fromEnv.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/decks" : fromEnv;
}

QString defaultDatabasePath() {
    const QString fromEnv = qEnvironmentVariable("OMACARDS_STATE_DB");
    return fromEnv.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/state.db" : fromEnv;
}

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

void Backend::load() {
    QString error;
    const auto decks = DeckIO::loadAll(m_decksDir, &error);
    if (!error.isEmpty())
        reportError(error);
    m_decks = decks;
    m_deckNames.clear();
    for (const Deck &deck : m_decks)
        m_deckNames << deck.name;
    if (!m_deckNames.contains(m_currentDeck))
        m_currentDeck = m_deckNames.isEmpty() ? QString() : m_deckNames.first();

    QSet<QString> ids;
    for (const Deck &deck : m_decks)
        for (const Card &card : deck.cards)
            ids << card.id;
    m_store.removeExcept(ids);

    emit decksChanged();
    emit currentDeckChanged();
    emit cardsChanged();
    dueCountChanged();
}

int Backend::countDue(const QString &deckName) const {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto states = m_store.states();
    const auto hashes = m_store.hashes();
    int count = 0;
    for (const Deck &deck : m_decks) {
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};
            if (state.due <= now)
                ++count;
        }
    }
    return count;
}

int Backend::availableCount(const QString &deckName) const {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto states = m_store.states();
    const auto hashes = m_store.hashes();
    int remainingNew = qMax(0, m_newPerDay - m_store.newCardsToday());
    int count = 0;
    for (const Deck &deck : m_decks) {
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};
            if (state.reps == 0) {
                if (remainingNew > 0) {
                    ++count;
                    --remainingNew;
                }
            } else if (state.due <= now) {
                ++count;
            }
        }
    }
    return count;
}

void Backend::refresh() {
    load();
    rebuildQueue();
}

int Backend::newCount() const {
    const auto states = m_store.states();
    const auto hashes = m_store.hashes();
    int remaining = qMax(0, m_newPerDay - m_store.newCardsToday());
    int count = 0;
    for (const Deck &deck : m_decks) {
        for (const Card &card : deck.cards) {
            if (remaining <= 0)
                return count;
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};
            if (state.reps == 0) {
                ++count;
                --remaining;
            }
        }
    }
    return count;
}

int Backend::deckDue() const {
    return availableCount(m_currentDeck);
}

int Backend::deckNew() const {
    const Deck *deck = deckByName(m_currentDeck);
    if (!deck)
        return 0;
    const auto states = m_store.states();
    const auto hashes = m_store.hashes();
    int remaining = qMax(0, m_newPerDay - m_store.newCardsToday());
    int count = 0;
    for (const Card &card : deck->cards) {
        FsrsState state = states.value(card.id);
        if (hashes.value(card.id) != DeckIO::contentHash(card))
            state = FsrsState{};
        if (state.reps == 0 && count < remaining)
            ++count;
    }
    return count;
}

QVariantList Backend::deckSummaries() const {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto states = m_store.states();
    const auto hashes = m_store.hashes();
    int remainingNew = qMax(0, m_newPerDay - m_store.newCardsToday());

    QVariantList result;
    for (const Deck &deck : m_decks) {
        int due = 0;
        int fresh = 0;
        for (const Card &card : deck.cards) {
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};
            if (state.reps == 0) {
                if (remainingNew > 0) {
                    ++fresh;
                    --remainingNew;
                }
            } else if (state.due <= now) {
                ++due;
            }
        }
        QVariantMap entry;
        entry.insert("name", deck.name);
        entry.insert("description", deck.description);
        entry.insert("cards", deck.cards.size());
        entry.insert("due", due);
        entry.insert("new", fresh);
        entry.insert("active", deck.name == m_currentDeck);
        result << entry;
    }
    return result;
}

void Backend::rebuildQueue() {
    m_reviewQueue.clear();
    m_cramQueue.clear();
    m_currentId.clear();

    const Deck *deck = deckByName(m_currentDeck);
    if (!deck) {
        presentCard();
        return;
    }

    if (m_mode == "cram") {
        for (int i = 0; i < deck->cards.size(); ++i)
            m_cramQueue.append(CramEntry{deck->cards.at(i).id, 0, i});
        std::shuffle(m_cramQueue.begin(), m_cramQueue.end(), *QRandomGenerator::global());
    } else {
        const auto states = m_store.states();
        const auto hashes = m_store.hashes();
        QVector<QPair<qint64, QString>> due;
        QStringList fresh;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        int remainingNew = qMax(0, m_newPerDay - m_store.newCardsToday());
        for (const Card &card : deck->cards) {
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};
            if (state.reps == 0) {
                if (remainingNew > 0) {
                    fresh << card.id;
                    --remainingNew;
                }
            } else if (state.due <= now) {
                due.append({state.due, card.id});
            }
        }
        std::sort(due.begin(), due.end(), [](const auto &a, const auto &b) {
            return a.first == b.first ? a.second < b.second : a.first < b.first;
        });
        for (const auto &item : due)
            m_reviewQueue << item.second;
        // Due reviews come first so lapses are not hidden behind new material;
        // new cards then follow in deck-file order up to the daily limit.
        for (const QString &id : fresh)
            m_reviewQueue << id;
    }

    m_dueCount = availableCount();
    emit dueCountChanged();
    presentCard();
}

void Backend::presentCard() {
    m_showingAnswer = false;
    if (m_mode == "cram") {
        if (m_cramQueue.isEmpty()) {
            m_currentId.clear();
            m_question.clear();
            m_answer.clear();
            setStatus(tr("No cards yet. Press N to add one."));
            emit cardChanged();
            return;
        }
        int best = 0;
        for (int i = 1; i < m_cramQueue.size(); ++i)
            if (m_cramQueue.at(i).next < m_cramQueue.at(best).next)
                best = i;
        m_currentId = m_cramQueue.at(best).cardId;
        m_box = m_cramQueue.at(best).box + 1;
    } else {
        if (m_reviewQueue.isEmpty()) {
            m_currentId.clear();
            m_question.clear();
            m_answer.clear();
            setStatus(tr("All caught up. Press C to cram or N to add a card."));
            emit cardChanged();
            return;
        }
        m_currentId = m_reviewQueue.first();
    }

    const Card *card = cardById(m_currentId);
    if (!card) {
        if (m_mode == "cram") {
            m_cramQueue.erase(std::remove_if(m_cramQueue.begin(), m_cramQueue.end(), [this](const CramEntry &entry) {
                return entry.cardId == m_currentId;
            }), m_cramQueue.end());
        } else {
            m_reviewQueue.removeAll(m_currentId);
        }
        m_currentId.clear();
        presentCard();
        return;
    }
    m_question = m_math.render(DeckIO::displayQuestion(*card));
    m_answer = m_math.render(DeckIO::displayAnswer(*card));

    if (m_mode == "review") {
        const auto state = m_store.states().value(card->id);
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

QByteArray Backend::readFileBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void Backend::recordHistory(const QString &path, const QByteArray &before, bool existedBefore,
                            const QString &description) {
    const bool existsAfter = QFileInfo::exists(path);
    const QByteArray after = existsAfter ? readFileBytes(path) : QByteArray();
    if (existedBefore == existsAfter && before == after)
        return;
    m_undoStack.append(FileSnapshot{path, before, after, existedBefore, existsAfter, description});
    if (m_undoStack.size() > 50)
        m_undoStack.removeFirst();
    m_redoStack.clear();
    emit historyChanged();
}

bool Backend::applySnapshot(const FileSnapshot &snapshot, bool restoreBefore, QString *error) {
    const bool exists = restoreBefore ? snapshot.existedBefore : snapshot.existsAfter;
    if (!exists) {
        if (QFileInfo::exists(snapshot.path) && !QFile::remove(snapshot.path)) {
            if (error) *error = tr("Cannot remove %1").arg(snapshot.path);
            return false;
        }
        return true;
    }

    QSaveFile file(snapshot.path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("Cannot write %1").arg(snapshot.path);
        return false;
    }
    file.write(restoreBefore ? snapshot.before : snapshot.after);
    if (!file.commit()) {
        if (error) *error = tr("Cannot write %1").arg(snapshot.path);
        return false;
    }
    return true;
}

void Backend::undo() {
    if (m_undoStack.isEmpty()) {
        setStatus(tr("Nothing to undo"));
        return;
    }
    const FileSnapshot snapshot = m_undoStack.takeLast();
    QString error;
    if (!applySnapshot(snapshot, true, &error)) {
        m_undoStack.append(snapshot);
        reportError(error);
        return;
    }
    m_redoStack.append(snapshot);
    emit historyChanged();
    refresh();
    setStatus(tr("Undid %1").arg(snapshot.description));
}

void Backend::redo() {
    if (m_redoStack.isEmpty()) {
        setStatus(tr("Nothing to redo"));
        return;
    }
    const FileSnapshot snapshot = m_redoStack.takeLast();
    QString error;
    if (!applySnapshot(snapshot, false, &error)) {
        m_redoStack.append(snapshot);
        reportError(error);
        return;
    }
    m_undoStack.append(snapshot);
    emit historyChanged();
    refresh();
    setStatus(tr("Redid %1").arg(snapshot.description));
}

QVariantList Backend::cardList() const {
    QVariantList list;
    const QString needle = m_cardFilter.trimmed().toLower();
    const auto states = m_store.states();
    const auto hashes = m_store.hashes();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (const Deck &deck : m_decks) {
        if (!m_currentDeck.isEmpty() && deck.name != m_currentDeck)
            continue;
        for (const Card &card : deck.cards) {
            const QString haystack = (card.question + " " + card.answer + " " + card.tags.join(' ')).toLower();
            if (!needle.isEmpty() && !haystack.contains(needle))
                continue;
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};

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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto states = m_store.states();
    const auto hashes = m_store.hashes();
    int remainingNew = qMax(0, m_newPerDay - m_store.newCardsToday());
    QVector<Card> reviews;
    QVector<Card> fresh;
    for (const Deck &deck : m_decks) {
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        for (const Card &card : deck.cards) {
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};
            if (state.reps == 0) {
                if (remainingNew > 0) {
                    fresh << card;
                    --remainingNew;
                }
            } else if (state.due <= now) {
                reviews << card;
            }
        }
    }
    reviews << fresh;
    return reviews;
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
    m_store.setSetting("new_per_day", QString::number(clean));
    emit settingsChanged();
    rebuildQueue();
}

void Backend::setDesiredRetention(double retention) {
    const double clean = qBound(0.70, retention, 0.99);
    if (qFuzzyCompare(m_desiredRetention, clean))
        return;
    m_desiredRetention = clean;
    m_store.setSetting("desired_retention", QString::number(clean, 'f', 2));
    emit settingsChanged();
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
    m_mode = mode == "cram" ? "cram" : "review";
    m_reviewed = 0;
    m_correct = 0;
    m_cramReviews = 0;
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
    if (rating != 1 && rating != 3)
        return;
    ++m_reviewed;
    if (rating == 3)
        ++m_correct;
    if (m_mode == "cram")
        answerCram(rating);
    else
        answerReview(rating);
    emit statsChanged();
    presentCard();
    emit cardsChanged();
    emit decksChanged();
}

void Backend::answerReview(int rating) {
    Card *card = cardById(m_currentId);
    if (!card)
        return;
    const QString hash = DeckIO::contentHash(*card);
    FsrsState state = m_store.states().value(card->id);
    if (m_store.hashes().value(card->id) != hash)
        state = FsrsState{};

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool wasNew = state.reps == 0;
    const ReviewResult result = Fsrs::review(state, rating, now, m_desiredRetention);
    m_store.save(card->id, hash, result.state);
    m_store.logReview(card->id, rating, now, result.state, result.intervalDays, wasNew);
    if (!m_reviewQueue.isEmpty() && m_reviewQueue.first() == card->id)
        m_reviewQueue.removeFirst();
    else
        m_reviewQueue.removeAll(card->id);
    m_dueCount = availableCount();
    emit dueCountChanged();
}

void Backend::answerCram(int rating) {
    static const int intervals[] = {1, 2, 4, 8, 16};
    for (int i = 0; i < m_cramQueue.size(); ++i) {
        if (m_cramQueue.at(i).cardId != m_currentId)
            continue;
        CramEntry entry = m_cramQueue.takeAt(i);
        entry.box = rating == 1 ? 0 : qMin(4, entry.box + 1);
        entry.next = ++m_cramReviews + intervals[entry.box];
        m_cramQueue.append(entry);
        m_box = entry.box + 1;
        break;
    }
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

QString Backend::safeFileName(const QString &name) {
    QString result = name.trimmed();
    result.replace(QRegularExpression(R"re([^\w.-]+)re"), "-");
    result.remove(QRegularExpression(R"re(^-+|-+$)re"));
    return result.isEmpty() ? "Deck" : result;
}

bool Backend::createDeck(const QString &name, QString *error) {
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
    deck.path = QDir(m_decksDir).filePath(safeFileName(clean) + ".txt");
    const bool existed = QFileInfo::exists(deck.path);
    const QByteArray before = existed ? readFileBytes(deck.path) : QByteArray();
    if (!DeckIO::save(deck, error))
        return false;
    recordHistory(deck.path, before, existed, tr("creating deck %1").arg(clean));
    refresh();
    selectDeck(clean);
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
    card.type = type == "cloze" ? "cloze" : "basic";
    for (const QString &tag : tags.split(',', Qt::SkipEmptyParts)) {
        const QString clean = tag.trimmed();
        if (!clean.isEmpty() && !card.tags.contains(clean))
            card.tags << clean;
    }
    card.id = DeckIO::cardId(card);
    deck->cards << card;
    const QByteArray before = readFileBytes(deck->path);
    if (!DeckIO::save(*deck, error))
        return false;
    recordHistory(deck->path, before, true, tr("adding a card"));
    refresh();
    return true;
}

bool Backend::modifyCard(const QString &deckName, const QString &cardId, const QString &question,
                         const QString &answer, const QString &type, const QString &tags,
                         QString *error) {
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

    card->question = question.trimmed();
    card->answer = answer.trimmed();
    card->type = type == "cloze" ? "cloze" : "basic";
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

    const QByteArray before = readFileBytes(deck->path);
    if (!DeckIO::save(*deck, error))
        return false;
    recordHistory(deck->path, before, true, tr("editing a card"));
    refresh();
    return true;
}

bool Backend::deleteCard(const QString &deckName, const QString &cardId, QString *error) {
    Deck *deck = deckByName(deckName.isEmpty() ? m_currentDeck : deckName);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(deckName);
        return false;
    }
    const int before = deck->cards.size();
    deck->cards.erase(std::remove_if(deck->cards.begin(), deck->cards.end(), [&](const Card &card) {
        return card.id == cardId;
    }), deck->cards.end());
    if (deck->cards.size() == before) {
        if (error) *error = tr("No such card: %1").arg(cardId);
        return false;
    }
    const QByteArray contents = readFileBytes(deck->path);
    if (!DeckIO::save(*deck, error))
        return false;
    recordHistory(deck->path, contents, true, tr("removing a card"));
    refresh();
    return true;
}

bool Backend::removeDeck(const QString &name, QString *error) {
    const Deck *deck = deckByName(name);
    if (!deck) {
        if (error) *error = tr("No such deck: %1").arg(name);
        return false;
    }
    const QString path = deck->path;
    const QByteArray before = readFileBytes(path);
    if (!QFile::remove(path)) {
        if (error) *error = tr("Cannot remove deck file: %1").arg(path);
        return false;
    }
    recordHistory(path, before, true, tr("deleting deck %1").arg(name));
    if (m_currentDeck == name)
        m_currentDeck.clear();
    refresh();
    return true;
}

void Backend::addCard(const QString &question, const QString &answer) {
    QString error;
    if (!createCard(m_currentDeck, question, answer, "basic", {}, &error))
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

QStringList Backend::splitDelimited(const QString &line, QChar delimiter) {
    QStringList cells;
    QString cell;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line.at(i + 1) == '"') {
                cell += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == delimiter && !quoted) {
            cells << cell;
            cell.clear();
        } else {
            cell += ch;
        }
    }
    cells << cell;
    return cells;
}

bool Backend::importCsv(const QString &path, const QString &deckName, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = tr("Cannot read: %1").arg(path);
        return false;
    }
    const QChar delimiter = path.endsWith(".tsv", Qt::CaseInsensitive) ? '\t' : ',';
    QStringList rows = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    if (rows.isEmpty()) {
        if (error) *error = tr("No rows in %1").arg(path);
        return false;
    }
    const auto first = splitDelimited(rows.first(), delimiter);
    const bool hasHeader = !first.isEmpty()
        && (first.first().contains("question", Qt::CaseInsensitive)
            || first.first().contains("front", Qt::CaseInsensitive));
    if (hasHeader)
        rows.removeFirst();

    QString target = deckName;
    if (target.isEmpty())
        target = QFileInfo(path).completeBaseName();
    if (!deckByName(target) && !createDeck(target, error))
        return false;

    int imported = 0;
    for (const QString &row : rows) {
        const QStringList cells = splitDelimited(row, delimiter);
        if (cells.size() < 2)
            continue;
        const QString question = cells.at(0).trimmed();
        const QString answer = cells.at(1).trimmed();
        if (question.isEmpty() || answer.isEmpty())
            continue;
        const QString tags = cells.size() > 2 ? cells.at(2) : QString();
        if (!createCard(target, question, answer, "basic", tags, error))
            return false;
        ++imported;
    }
    setStatus(tr("Imported %1 cards").arg(imported));
    return true;
}

QString Backend::decodeEntities(const QString &text) {
    QString out = text;
    out.replace("&nbsp;", QChar(0x00A0));
    out.replace("&amp;", "&");
    out.replace("&lt;", "<");
    out.replace("&gt;", ">");
    out.replace("&quot;", "\"");
    out.replace("&apos;", "'");
    out.replace("&#39;", "'");
    static const QRegularExpression numeric(R"re(&#(?:x([0-9a-fA-F]+)|([0-9]+));)re");
    out = replaceRegex(out, numeric, [](const QRegularExpressionMatch &match) {
        bool ok = false;
        const int code = match.captured(1).isEmpty()
                             ? match.captured(2).toInt(&ok, 10)
                             : match.captured(1).toInt(&ok, 16);
        return ok && code > 0 && code <= 0x10ffff ? QString(QChar(code)) : match.captured();
    });
    // Anki cloze hints are written as {{c1::answer::hint}}; our deck format
    // keeps just the answer so the plain-text source stays easy to edit.
    static const QRegularExpression clozeHint(R"re(\{\{(c\d+::[^:]+)::[^}]*\}\})re");
    out = replaceRegex(out, clozeHint, [](const QRegularExpressionMatch &match) {
        return QStringLiteral("{{%1}}").arg(match.captured(1));
    });
    return out;
}

QString Backend::stripHtml(const QString &html) {
    QString text = html;
    text.replace(QRegularExpression(R"re(<br\s*/?>)re"), "\n");
    // Keep image tags so imported media can still be shown in the RichText
    // study view; replace them with markers while the other tags are removed.
    QVector<QString> images;
    static const QRegularExpression imageTag(R"re(<img\b[^>]*>)re",
                                             QRegularExpression::CaseInsensitiveOption);
    QString marked;
    int last = 0;
    auto matches = imageTag.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        marked += text.mid(last, match.capturedStart() - last);
        images << match.captured();
        marked += QString("\x01%1\x02").arg(images.size() - 1);
        last = match.capturedEnd();
    }
    marked += text.mid(last);
    marked.replace(QRegularExpression(R"re(<[^>]+>)re"), "");
    text = decodeEntities(marked);
    for (int i = 0; i < images.size(); ++i)
        text.replace(QString("\x01%1\x02").arg(i), images.at(i));
    return text.trimmed();
}

QString Backend::rewriteAnkiMedia(const QString &html, const QHash<QString, QString> &media) {
    QString text = html;
    for (auto it = media.constBegin(); it != media.constEnd(); ++it) {
        for (const QString &quote : {QStringLiteral("\""), QStringLiteral("'")}) {
            const QString from = QStringLiteral("src=%1%2%1").arg(quote, it.key());
            const QString to = QStringLiteral("src=%1%2%1").arg(quote, it.value());
            text.replace(from, to, Qt::CaseInsensitive);
        }
    }
    return text;
}

QHash<QString, QString> Backend::apkgMedia(const QString &path, const QString &directory,
                                            const QString &unzip, QString *error) {
    QHash<QString, QString> media;
    QProcess mapping;
    mapping.start(unzip, {"-p", path, "media"});
    if (!mapping.waitForFinished(10000) || mapping.exitCode() != 0)
        return media; // packages without media are common
    const QJsonDocument document = QJsonDocument::fromJson(mapping.readAllStandardOutput());
    if (!document.isObject())
        return media;

    const QDir target(directory);
    if (!target.mkpath(QStringLiteral(".")))
        return media;
    const QJsonObject entries = document.object();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QString name = QFileInfo(it.value().toString()).fileName();
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;
        const QString destination = target.filePath(name);
        QProcess extract;
        extract.setStandardOutputFile(destination);
        extract.start(unzip, {"-p", path, it.key()});
        if (!extract.waitForFinished(10000) || extract.exitCode() != 0) {
            QFile::remove(destination);
            if (error)
                *error = tr("Could not extract media %1").arg(name);
            continue;
        }
        media.insert(it.key(), QUrl::fromLocalFile(destination).toString());
    }
    return media;
}

bool Backend::importApkg(const QString &path, const QString &deckName, QString *error) {
    QTemporaryDir temp;
    if (!temp.isValid()) {
        if (error) *error = tr("Cannot create a temporary directory");
        return false;
    }
    const QString database = temp.filePath("collection.db");
    const auto unzip = QStandardPaths::findExecutable("unzip");
    if (unzip.isEmpty()) {
        if (error) *error = tr("Importing .apkg needs the unzip command");
        return false;
    }
    auto extract = [&](const QString &entry) {
        QProcess process;
        process.setStandardOutputFile(database);
        process.start(unzip, {"-p", path, entry});
        return process.waitForFinished(10000) && process.exitCode() == 0 && QFileInfo::exists(database);
    };

    bool ok = extract("collection.anki2") || extract("collection.anki21");
    if (!ok) {
        // Anki 21b is a gzip-compressed SQLite file inside the archive.
        QProcess unzipper;
        unzipper.start(unzip, {"-p", path, "collection.anki21b"});
        if (unzipper.waitForFinished(10000) && unzipper.exitCode() == 0) {
            QProcess gzip;
            gzip.setStandardOutputFile(database);
            gzip.start(QStandardPaths::findExecutable("gzip"), {"-dc"});
            gzip.write(unzipper.readAllStandardOutput());
            gzip.closeWriteChannel();
            ok = gzip.waitForFinished(10000) && gzip.exitCode() == 0 && QFileInfo::exists(database);
        }
    }
    if (!ok) {
        if (error) *error = tr("Not a readable Anki package: %1").arg(path);
        return false;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "omacards-apkg-" + QString::number(QRandomGenerator::global()->generate()));
    db.setDatabaseName(database);
    if (!db.open()) {
        if (error) *error = tr("Cannot read the Anki collection");
        return false;
    }
    // Cloze notes use model type 1; their text lives in a single field with
    // {{c1::...}} markers. Older collections may lack the type column.
    QSqlQuery query(db);
    bool clozeColumn = query.exec("SELECT type FROM models LIMIT 1");
    if (clozeColumn)
        clozeColumn = query.exec("SELECT n.flds, n.tags, COALESCE(m.type, 0) "
                                 "FROM notes n LEFT JOIN models m ON n.mid = m.id");
    if (!clozeColumn && !query.exec("SELECT flds, tags FROM notes")) {
        if (error) *error = tr("The Anki collection has no notes table");
        return false;
    }
    QString target = deckName.isEmpty() ? QFileInfo(path).completeBaseName() : deckName;
    if (!deckByName(target) && !createDeck(target, error))
        return false;
    const QHash<QString, QString> media = apkgMedia(
        path, m_decksDir + "/media/" + safeFileName(target), unzip, error);
    int imported = 0;
    while (query.next()) {
        const QStringList fields = query.value(0).toString().split(QChar(0x1f));
        if (fields.isEmpty())
            continue;
        const bool cloze = clozeColumn && query.value(2).toInt() == 1;
        QString front = stripHtml(fields.at(0));
        QString back = !cloze && fields.size() > 1 ? stripHtml(fields.at(1)) : QString();
        if (cloze) {
            static const QRegularExpression clozeNumber(R"re(\{\{c\d+::(.*?)::[^}]*\}\})re");
            front = replaceRegex(front, clozeNumber, [](const QRegularExpressionMatch &match) {
                return QStringLiteral("{{c1::%1}}").arg(match.captured(1));
            });
        }
        front = rewriteAnkiMedia(front, media);
        if (!cloze) {
            static const QRegularExpression imageSrc(R"re(<img\b[^>]*\bsrc="([^"]+)"[^>]*>)re");
            back = replaceRegex(back, imageSrc, [&](const QRegularExpressionMatch &match) {
                return QStringLiteral("<img src=\"%1\">").arg(media.value(match.captured(1), match.captured(1)));
            });
        }
        if (front.isEmpty())
            continue;
        QString tags = query.value(1).toString().simplified();
        tags.replace(' ', ',');
        if (!createCard(target, front, back, cloze ? "cloze" : "basic", tags, error))
            return false;
        ++imported;
    }
    query.finish();
    query.clear();
    db.close();
    const QString connection = db.connectionName();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    setStatus(tr("Imported %1 cards").arg(imported));
    return true;
}

bool Backend::importFile(const QString &path, const QString &deckName, QString *error) {
    const QString lower = path.toLower();
    if (lower.endsWith(".apkg"))
        return importApkg(path, deckName, error);
    if (lower.endsWith(".csv") || lower.endsWith(".tsv") || lower.endsWith(".txt"))
        return importCsv(path, deckName, error);
    if (error) *error = tr("Unsupported import format: %1").arg(path);
    return false;
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
