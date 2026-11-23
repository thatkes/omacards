#include "store.h"
#include "config.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

Store::Store(const QString &path)
    : m_path(path.isEmpty()
                 ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/omacards/state.db"
                 : path),
      m_connection("omacards-" + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

Store::~Store() {
    if (m_db.isValid()) {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool Store::open(QString *error) {
    if (m_db.isOpen())
        return true;
    QFileInfo(m_path).dir().mkpath(".");
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    m_db.setDatabaseName(m_path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }

    QSqlQuery query(m_db);
    const QStringList statements{
        "CREATE TABLE IF NOT EXISTS cards ("
        "id TEXT PRIMARY KEY, hash TEXT NOT NULL, stability REAL NOT NULL, "
        "difficulty REAL NOT NULL, due INTEGER NOT NULL, last_review INTEGER NOT NULL, "
        "reps INTEGER NOT NULL, lapses INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS reviews ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, card_id TEXT NOT NULL, rating INTEGER NOT NULL, "
        "at INTEGER NOT NULL, stability REAL NOT NULL, difficulty REAL NOT NULL, "
        "interval INTEGER NOT NULL, new_card INTEGER NOT NULL DEFAULT 0)",
        "CREATE INDEX IF NOT EXISTS reviews_card ON reviews(card_id)",
        "CREATE TABLE IF NOT EXISTS settings ("
        "key TEXT PRIMARY KEY, value TEXT NOT NULL)",
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            if (error)
                *error = m_lastError;
            return false;
        }
    }

    // Older databases stored the new-card allowance under new_per_day. Copy
    // the value forward on load, without replacing a canonical key that is
    // already there, and keep the old key for not-yet-migrated consumers.
    if (!query.exec(QStringLiteral("INSERT INTO settings (key, value) SELECT 'new_card_allowance', value FROM settings "
                                   "WHERE key = 'new_per_day' AND NOT EXISTS "
                                   "(SELECT 1 FROM settings WHERE key = 'new_card_allowance')"))) {
        m_lastError = query.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }

    // Older databases predate the new_card column. Keep them readable instead
    // of dropping the review history.
    bool hasNewCard = false;
    if (query.exec("PRAGMA table_info(reviews)")) {
        while (query.next())
            if (query.value(1).toString() == "new_card")
                hasNewCard = true;
    }
    if (!hasNewCard && !query.exec("ALTER TABLE reviews ADD COLUMN new_card INTEGER NOT NULL DEFAULT 0")) {
        m_lastError = query.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }
    return true;
}

QHash<QString, FsrsState> Store::states() const {
    QHash<QString, FsrsState> result;
    if (!m_db.isOpen())
        return result;
    QSqlQuery query(m_db);
    if (!query.exec("SELECT id, stability, difficulty, due, last_review, reps, lapses FROM cards")) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next()) {
        FsrsState state;
        state.stability = query.value(1).toDouble();
        state.difficulty = query.value(2).toDouble();
        state.due = query.value(3).toLongLong();
        state.lastReview = query.value(4).toLongLong();
        state.reps = query.value(5).toInt();
        state.lapses = query.value(6).toInt();
        result.insert(query.value(0).toString(), state);
    }
    return result;
}

QHash<QString, QString> Store::hashes() const {
    QHash<QString, QString> result;
    if (!m_db.isOpen())
        return result;
    QSqlQuery query(m_db);
    if (!query.exec("SELECT id, hash FROM cards")) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next())
        result.insert(query.value(0).toString(), query.value(1).toString());
    return result;
}

void Store::save(const QString &cardId, const QString &hash, const FsrsState &state) {
    if (!m_db.isOpen())
        return;
    QSqlQuery query(m_db);
    query.prepare(R"sql(
        INSERT INTO cards (id, hash, stability, difficulty, due, last_review, reps, lapses)
        VALUES (:id, :hash, :stability, :difficulty, :due, :last_review, :reps, :lapses)
        ON CONFLICT(id) DO UPDATE SET
            hash=excluded.hash, stability=excluded.stability, difficulty=excluded.difficulty,
            due=excluded.due, last_review=excluded.last_review, reps=excluded.reps,
            lapses=excluded.lapses
    )sql");
    query.bindValue(":id", cardId);
    query.bindValue(":hash", hash);
    query.bindValue(":stability", state.stability);
    query.bindValue(":difficulty", state.difficulty);
    query.bindValue(":due", state.due);
    query.bindValue(":last_review", state.lastReview);
    query.bindValue(":reps", state.reps);
    query.bindValue(":lapses", state.lapses);
    if (!query.exec())
        m_lastError = query.lastError().text();
}

void Store::recordRating(const QString &cardId, int rating, qint64 at, const FsrsState &state, int intervalDays,
                         bool wasNew) {
    if (!m_db.isOpen())
        return;
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO reviews (card_id, rating, at, stability, difficulty, interval, new_card) "
                  "VALUES (:id, :rating, :at, :stability, :difficulty, :interval, :new_card)");
    query.bindValue(":id", cardId);
    query.bindValue(":rating", rating);
    query.bindValue(":at", at);
    query.bindValue(":stability", state.stability);
    query.bindValue(":difficulty", state.difficulty);
    query.bindValue(":interval", intervalDays);
    query.bindValue(":new_card", wasNew ? 1 : 0);
    if (!query.exec())
        m_lastError = query.lastError().text();
}

void Store::removeExcept(const QSet<QString> &ids) {
    if (!m_db.isOpen())
        return;
    QSqlQuery query(m_db);
    if (!query.exec("SELECT id FROM cards")) {
        m_lastError = query.lastError().text();
        return;
    }
    QStringList stale;
    while (query.next()) {
        const QString id = query.value(0).toString();
        if (!ids.contains(id))
            stale << id;
    }
    if (!query.prepare("DELETE FROM cards WHERE id = ?")) {
        m_lastError = query.lastError().text();
        return;
    }
    for (const QString &id : stale) {
        query.bindValue(0, id);
        if (!query.exec())
            m_lastError = query.lastError().text();
    }
}

ReviewSummary Store::summary(int dayStartHour) const {
    ReviewSummary result;
    if (!m_db.isOpen())
        return result;
    QSqlQuery query(m_db);
    if (!query.exec("SELECT at, rating FROM reviews ORDER BY at")) {
        m_lastError = query.lastError().text();
        return result;
    }

    const QDate today = Config::studyDay(QDateTime::currentMSecsSinceEpoch(), dayStartHour);
    QSet<QDate> days;
    while (query.next()) {
        const qint64 atMs = query.value(0).toLongLong();
        const QDate day = Config::studyDay(atMs, dayStartHour);
        days.insert(day);
        ++result.total;
        if (day == today) {
            ++result.studyDayRatings;
            if (query.value(1).toInt() == 3)
                ++result.studyDayGood;
        }
    }
    result.newCardsOnStudyDay = countNewCardsOnStudyDay(dayStartHour);

    // A streak survives until the next study day begins, so a user who has
    // not reviewed yet still sees the previous day's total.
    QDate cursor = today;
    if (!days.contains(cursor))
        cursor = cursor.addDays(-1);
    while (days.contains(cursor)) {
        ++result.streak;
        cursor = cursor.addDays(-1);
    }
    return result;
}

int Store::newCardsOnStudyDay(int dayStartHour) const {
    const int count = countNewCardsOnStudyDay(dayStartHour);
    if (m_lastError.isEmpty() && !m_db.isOpen())
        m_lastError = QStringLiteral("State database is not open");
    return count;
}

int Store::countNewCardsOnStudyDay(int dayStartHour) const {
    if (!m_db.isOpen())
        return 0;
    QSqlQuery query(m_db);
    if (!query.prepare("SELECT COUNT(DISTINCT card_id) FROM reviews "
                       "WHERE new_card = 1 AND at >= :start")) {
        m_lastError = query.lastError().text();
        return 0;
    }
    query.bindValue(":start", Config::studyDayStartMs(QDateTime::currentMSecsSinceEpoch(), dayStartHour));
    if (!query.exec() || !query.next()) {
        m_lastError = query.lastError().text();
        return 0;
    }
    return query.value(0).toInt();
}

QString Store::setting(const QString &key, const QString &fallback) const {
    if (!m_db.isOpen())
        return fallback;
    QSqlQuery query(m_db);
    if (!query.prepare("SELECT value FROM settings WHERE key = :key")) {
        m_lastError = query.lastError().text();
        return fallback;
    }
    query.bindValue(":key", key);
    if (!query.exec())
        m_lastError = query.lastError().text();
    return query.next() ? query.value(0).toString() : fallback;
}

bool Store::setSetting(const QString &key, const QString &value) {
    if (!m_db.isOpen())
        return false;
    QSqlQuery query(m_db);
    if (!query.prepare("INSERT INTO settings (key, value) VALUES (:key, :value) "
                       "ON CONFLICT(key) DO UPDATE SET value = excluded.value")) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.bindValue(":key", key);
    query.bindValue(":value", value);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QString Store::currentDeck() const {
    return setting(QStringLiteral("current_deck"));
}

bool Store::setCurrentDeck(const QString &name) {
    return setSetting(QStringLiteral("current_deck"), name);
}

QHash<QString, qint64> Store::deckLastStudied() const {
    QHash<QString, qint64> result;
    if (!m_db.isOpen())
        return result;
    const QString prefix = QStringLiteral("deck_last_studied/");
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT key, value FROM settings"))) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next()) {
        const QString key = query.value(0).toString();
        if (key.startsWith(prefix))
            result.insert(key.mid(prefix.size()), query.value(1).toString().toLongLong());
    }
    return result;
}

bool Store::setDeckLastStudied(const QString &name, qint64 at) {
    return setSetting(QStringLiteral("deck_last_studied/") + name, QString::number(at));
}

bool Store::moveDeckSettings(const QString &from, const QString &to) {
    if (from == to)
        return true;
    if (currentDeck() == from && !setCurrentDeck(to))
        return false;

    const QHash<QString, qint64> studied = deckLastStudied();
    if (!studied.contains(from))
        return true;
    // Carrying the stamp across keeps the renamed Deck in its old position in
    // the last-studied order; only its name changes.
    return setDeckLastStudied(to, studied.value(from)) && removeSetting(QStringLiteral("deck_last_studied/") + from);
}

bool Store::removeSetting(const QString &key) {
    if (!m_db.isOpen())
        return false;
    QSqlQuery query(m_db);
    if (!query.prepare(QStringLiteral("DELETE FROM settings WHERE key = :key"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    query.bindValue(":key", key);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}
