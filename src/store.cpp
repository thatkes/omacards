#include "store.h"

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
      m_connection("omacards-" + QUuid::createUuid().toString(QUuid::WithoutBraces)) {
}

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
        if (error)
            *error = m_db.lastError().text();
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
            if (error)
                *error = query.lastError().text();
            return false;
        }
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
        if (error)
            *error = query.lastError().text();
        return false;
    }
    return true;
}

QHash<QString, FsrsState> Store::states() const {
    QHash<QString, FsrsState> result;
    if (!m_db.isOpen())
        return result;
    QSqlQuery query(m_db);
    if (!query.exec("SELECT id, stability, difficulty, due, last_review, reps, lapses FROM cards"))
        return result;
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
    if (!query.exec("SELECT id, hash FROM cards"))
        return result;
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
    query.exec();
}

void Store::logReview(const QString &cardId, int rating, qint64 at,
                      const FsrsState &state, int intervalDays, bool wasNew) {
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
    query.exec();
}

void Store::removeExcept(const QSet<QString> &ids) {
    if (!m_db.isOpen())
        return;
    QSqlQuery query(m_db);
    query.exec("SELECT id FROM cards");
    QStringList stale;
    while (query.next()) {
        const QString id = query.value(0).toString();
        if (!ids.contains(id))
            stale << id;
    }
    query.prepare("DELETE FROM cards WHERE id = ?");
    for (const QString &id : stale) {
        query.bindValue(0, id);
        query.exec();
    }
}

ReviewSummary Store::summary() const {
    ReviewSummary result;
    if (!m_db.isOpen())
        return result;
    QSqlQuery query(m_db);
    if (!query.exec("SELECT at, rating FROM reviews ORDER BY at"))
        return result;

    const QDate today = QDate::currentDate();
    QSet<QDate> days;
    while (query.next()) {
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(query.value(0).toLongLong());
        const QDate day = at.date();
        days.insert(day);
        ++result.total;
        if (day == today) {
            ++result.today;
            if (query.value(1).toInt() == 3)
                ++result.todayCorrect;
        }
    }
    result.newToday = newCardsToday();

    // A streak survives until midnight after a review day, so a user who has
    // not reviewed yet today still sees yesterday's total.
    QDate cursor = today;
    if (!days.contains(cursor))
        cursor = cursor.addDays(-1);
    while (days.contains(cursor)) {
        ++result.streakDays;
        cursor = cursor.addDays(-1);
    }
    return result;
}

int Store::newCardsToday() const {
    if (!m_db.isOpen())
        return 0;
    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(DISTINCT card_id) FROM reviews "
                  "WHERE new_card = 1 AND at >= :start");
    query.bindValue(":start", QDate::currentDate().startOfDay().toMSecsSinceEpoch());
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toInt();
}

QString Store::setting(const QString &key, const QString &fallback) const {
    if (!m_db.isOpen())
        return fallback;
    QSqlQuery query(m_db);
    query.prepare("SELECT value FROM settings WHERE key = :key");
    query.bindValue(":key", key);
    if (!query.exec() || !query.next())
        return fallback;
    return query.value(0).toString();
}

bool Store::setSetting(const QString &key, const QString &value) {
    if (!m_db.isOpen())
        return false;
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO settings (key, value) VALUES (:key, :value) "
                  "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    query.bindValue(":key", key);
    query.bindValue(":value", value);
    return query.exec();
}
