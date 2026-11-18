#include "store.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

Store::Store(const QString &path)
    : m_path(path.isEmpty()
                 ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/state.db"
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
        "interval INTEGER NOT NULL)",
        "CREATE INDEX IF NOT EXISTS reviews_card ON reviews(card_id)",
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            if (error)
                *error = query.lastError().text();
            return false;
        }
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
                      const FsrsState &state, int intervalDays) {
    if (!m_db.isOpen())
        return;
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO reviews (card_id, rating, at, stability, difficulty, interval) "
                  "VALUES (:id, :rating, :at, :stability, :difficulty, :interval)");
    query.bindValue(":id", cardId);
    query.bindValue(":rating", rating);
    query.bindValue(":at", at);
    query.bindValue(":stability", state.stability);
    query.bindValue(":difficulty", state.difficulty);
    query.bindValue(":interval", intervalDays);
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
        query.addBindValue(id);
        query.exec();
    }
}
