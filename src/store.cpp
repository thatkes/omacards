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

namespace {

// Column 1 in states(), column 2 in the combined read: the six Fsrs fields
// always follow the id and (where present) the content hash.
FsrsState stateFromQuery(const QSqlQuery &query, int firstColumn) {
    FsrsState state;
    state.stability = query.value(firstColumn).toDouble();
    state.difficulty = query.value(firstColumn + 1).toDouble();
    state.due = query.value(firstColumn + 2).toLongLong();
    state.lastReview = query.value(firstColumn + 3).toLongLong();
    state.reps = query.value(firstColumn + 4).toInt();
    state.lapses = query.value(firstColumn + 5).toInt();
    return state;
}

} // namespace

Store::Store(const QString &path)
    : m_path(path.isEmpty()
                 ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/omacards/state.db"
                 : path),
      m_connection("omacards-" + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

Store::~Store() {
    if (m_db.isValid()) {
        // Drop the cached queries before removing their connection.
        m_schedulingQuery = QSqlQuery();
        m_cardQuery = QSqlQuery();
        m_schedulingPrepared = false;
        m_cardPrepared = false;
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
        "interval INTEGER NOT NULL, new_card INTEGER NOT NULL DEFAULT 0, "
        "deck_id TEXT NOT NULL DEFAULT '')",
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
    bool hasAt = false;
    bool hasDeckId = false;
    if (query.exec("PRAGMA table_info(reviews)")) {
        while (query.next()) {
            const QString column = query.value(1).toString();
            if (column == "new_card")
                hasNewCard = true;
            else if (column == "at")
                hasAt = true;
            else if (column == "deck_id")
                hasDeckId = true;
        }
    }
    if (!hasNewCard && !query.exec("ALTER TABLE reviews ADD COLUMN new_card INTEGER NOT NULL DEFAULT 0")) {
        m_lastError = query.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }
    hasNewCard = true;
    // Older reviews predate Deck-scoped counts. Their blank Deck id is read as
    // "all Decks" by the unscoped summary; no history is lost on migration.
    if (!hasDeckId && !query.exec("ALTER TABLE reviews ADD COLUMN deck_id TEXT NOT NULL DEFAULT ''")) {
        m_lastError = query.lastError().text();
        if (error)
            *error = m_lastError;
        return false;
    }
    hasDeckId = true;

    // No production query filters reviews by card id. The new-card count
    // filters new_card, at and (for a Deck) deck_id, so that trio gets the
    // covering index. Skip the index for a damaged reviews table; opening it
    // must stay as forgiving as the legacy migrations above.
    if (hasNewCard && hasAt && hasDeckId &&
        (!query.exec("DROP INDEX IF EXISTS reviews_card") ||
         !query.exec("CREATE INDEX IF NOT EXISTS reviews_new_card_at_deck ON reviews(new_card, at, deck_id)"))) {
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
    while (query.next())
        result.insert(query.value(0).toString(), stateFromQuery(query, 1));
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

QHash<QString, SchedulingRecord> Store::schedulingRecords() const {
    QHash<QString, SchedulingRecord> result;
    if (!m_db.isOpen())
        return result;

    // Prepared once per Store; repeated refreshes reuse the same statement.
    if (!m_schedulingPrepared) {
        m_schedulingQuery = QSqlQuery(m_db);
        if (!m_schedulingQuery.prepare(
                "SELECT id, hash, stability, difficulty, due, last_review, reps, lapses FROM cards")) {
            m_lastError = m_schedulingQuery.lastError().text();
            return result;
        }
        m_schedulingPrepared = true;
    }
    if (!m_schedulingQuery.exec()) {
        m_lastError = m_schedulingQuery.lastError().text();
        // A schema change can invalidate a prepared statement; reprepare on the
        // next read instead of serving an empty snapshot forever.
        m_schedulingQuery = QSqlQuery();
        m_schedulingPrepared = false;
        return result;
    }
    while (m_schedulingQuery.next()) {
        result.insert(m_schedulingQuery.value(0).toString(),
                      SchedulingRecord{stateFromQuery(m_schedulingQuery, 2), m_schedulingQuery.value(1).toString()});
    }
    m_schedulingQuery.finish();
    return result;
}

SchedulingRecord Store::schedulingRecord(const QString &cardId) const {
    if (!m_db.isOpen())
        return {};

    // Prepared once per Store; one rating binds its Card id and reads at most
    // that one cards row.
    if (!m_cardPrepared) {
        m_cardQuery = QSqlQuery(m_db);
        if (!m_cardQuery.prepare("SELECT id, hash, stability, difficulty, due, last_review, reps, lapses "
                                 "FROM cards WHERE id = :id")) {
            m_lastError = m_cardQuery.lastError().text();
            return {};
        }
        m_cardPrepared = true;
    }
    m_cardQuery.bindValue(QStringLiteral(":id"), cardId);
    if (!m_cardQuery.exec()) {
        m_lastError = m_cardQuery.lastError().text();
        // Schema drift can invalidate a prepared statement; reprepare next time.
        m_cardQuery = QSqlQuery();
        m_cardPrepared = false;
        return {};
    }

    SchedulingRecord result;
    if (m_cardQuery.next())
        result = SchedulingRecord{stateFromQuery(m_cardQuery, 2), m_cardQuery.value(1).toString()};
    m_cardQuery.finish();
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
                         bool wasNew, const QString &deckId) {
    if (!m_db.isOpen())
        return;
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO reviews (card_id, rating, at, stability, difficulty, interval, new_card, deck_id) "
                  "VALUES (:id, :rating, :at, :stability, :difficulty, :interval, :new_card, :deck_id)");
    query.bindValue(":id", cardId);
    query.bindValue(":rating", rating);
    query.bindValue(":at", at);
    query.bindValue(":stability", state.stability);
    query.bindValue(":difficulty", state.difficulty);
    query.bindValue(":interval", intervalDays);
    query.bindValue(":new_card", wasNew ? 1 : 0);
    // A default-constructed QString binds as SQL NULL; the column is NOT NULL.
    query.bindValue(":deck_id", deckId.isNull() ? QStringLiteral("") : deckId);
    if (!query.exec())
        m_lastError = query.lastError().text();
}

bool Store::beginRatingWrite() {
    if (!m_db.isOpen()) {
        if (m_lastError.isEmpty())
            m_lastError = QStringLiteral("State database is not open");
        return false;
    }
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    return true;
}

bool Store::finishRatingWrite() {
    if (!m_db.isOpen())
        return false;
    if (!m_lastError.isEmpty()) {
        // Keep the raw write error; a failed rollback must not mask it.
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

void Store::removeExcept(const QSet<QString> &ids) {
    if (!m_db.isOpen())
        return;
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return;
    }

    // One statement for the whole prune: a failure cannot leave part of the
    // stale set deleted. Empty sets still prune every sidecar row.
    const QStringList kept = ids.values();
    QString sql = QStringLiteral("DELETE FROM cards");
    if (!kept.isEmpty())
        sql += QStringLiteral(" WHERE id NOT IN (%1)")
                   .arg(QStringList(kept.size(), QStringLiteral("?")).join(QStringLiteral(",")));
    QSqlQuery query(m_db);
    bool ok = query.prepare(sql);
    if (ok) {
        for (int i = 0; i < kept.size(); ++i)
            query.bindValue(i, kept.at(i));
        ok = query.exec();
    }
    if (!ok) {
        m_lastError = query.lastError().text();
        m_db.rollback();
        return;
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
    }
}

ReviewSummary Store::summary(int dayStartHour) const {
    ReviewSummary result;
    if (!m_db.isOpen())
        return result;
    QSqlQuery query(m_db);
    // Only the per-day QSet and counters need these rows; their order is
    // never observed, so avoid SQLite's sort entirely.
    if (!query.exec("SELECT at, rating FROM reviews")) {
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

int Store::newCardsOnStudyDay(int dayStartHour, const QString &deckId) const {
    const int count = countNewCardsOnStudyDay(dayStartHour, deckId);
    if (m_lastError.isEmpty() && !m_db.isOpen())
        m_lastError = QStringLiteral("State database is not open");
    return count;
}

int Store::countNewCardsOnStudyDay(int dayStartHour, const QString &deckId) const {
    if (!m_db.isOpen())
        return 0;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral("SELECT COUNT(DISTINCT card_id) FROM reviews "
                                 "WHERE new_card = 1 AND at >= :start");
    if (!deckId.isEmpty())
        sql += QStringLiteral(" AND deck_id = :deck");
    if (!query.prepare(sql)) {
        m_lastError = query.lastError().text();
        return 0;
    }
    query.bindValue(":start", Config::studyDayStartMs(QDateTime::currentMSecsSinceEpoch(), dayStartHour));
    if (!deckId.isEmpty())
        query.bindValue(":deck", deckId);
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

    // Study-day usage is recomputed from review history, so the Deck id on
    // those rows has to follow the same rename as the settings below.
    QSqlQuery reviews(m_db);
    if (!reviews.prepare(QStringLiteral("UPDATE reviews SET deck_id = :to WHERE deck_id = :from"))) {
        m_lastError = reviews.lastError().text();
        return false;
    }
    reviews.bindValue(":to", to);
    reviews.bindValue(":from", from);
    if (!reviews.exec()) {
        m_lastError = reviews.lastError().text();
        return false;
    }

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
