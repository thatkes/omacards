// Backend owns cards, scheduling and files. QML only binds to properties and
// calls the small set of Q_INVOKABLE actions below.
#include "backend.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>
#include <QCryptographicHash>
#include <algorithm>

namespace {
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

void Backend::refresh() {
    load();
    rebuildQueue();
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
        for (const Card &card : deck->cards) {
            FsrsState state = states.value(card.id);
            if (hashes.value(card.id) != DeckIO::contentHash(card))
                state = FsrsState{};
            if (state.due <= QDateTime::currentMSecsSinceEpoch())
                due.append({state.due, card.id});
        }
        std::sort(due.begin(), due.end(), [](const auto &a, const auto &b) {
            return a.first == b.first ? a.second < b.second : a.first < b.first;
        });
        for (const auto &item : due)
            m_reviewQueue << item.second;
    }

    m_dueCount = countDue();
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

void Backend::answer(int rating) {
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
    const ReviewResult result = Fsrs::review(state, rating, now);
    m_store.save(card->id, hash, result.state);
    m_store.logReview(card->id, rating, now, result.state, result.intervalDays);
    if (!m_reviewQueue.isEmpty() && m_reviewQueue.first() == card->id)
        m_reviewQueue.removeFirst();
    else
        m_reviewQueue.removeAll(card->id);
    m_dueCount = countDue();
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
    if (!DeckIO::save(deck, error))
        return false;
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
    for (const QString &tag : tags.split(',', Qt::SkipEmptyParts))
        card.tags << tag.trimmed();
    card.id = DeckIO::cardId(card);
    deck->cards << card;
    if (!DeckIO::save(*deck, error))
        return false;
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
    if (!DeckIO::save(*deck, error))
        return false;
    refresh();
    return true;
}

void Backend::removeCurrentCard() {
    if (m_currentId.isEmpty())
        return;
    QString error;
    if (!deleteCard(m_currentDeck, m_currentId, &error))
        reportError(error);
    else
        setStatus(tr("Card removed"));
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

QString Backend::stripHtml(const QString &html) {
    QString text = html;
    text.replace(QRegularExpression(R"re(<br\s*/?>)re"), "\n");
    text.replace(QRegularExpression(R"re(<[^>]+>)re"), "");
    return text.trimmed();
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
    QSqlQuery query(db);
    if (!query.exec("SELECT flds FROM notes")) {
        if (error) *error = tr("The Anki collection has no notes table");
        return false;
    }
    QString target = deckName.isEmpty() ? QFileInfo(path).completeBaseName() : deckName;
    if (!deckByName(target) && !createDeck(target, error))
        return false;
    int imported = 0;
    while (query.next()) {
        const QStringList fields = query.value(0).toString().split(QChar(0x1f));
        if (fields.isEmpty())
            continue;
        const QString front = stripHtml(fields.at(0));
        const QString back = fields.size() > 1 ? stripHtml(fields.at(1)) : QString();
        if (front.isEmpty())
            continue;
        if (!createCard(target, front, back, "basic", {}, error))
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
