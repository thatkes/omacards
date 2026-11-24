#include <QtTest>
#include <QDate>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTime>

#include "test_store.h"
#include "test_support.h"
#include "config.h"
#include "fsrs.h"
#include "store.h"

using namespace TestSupport;

void TestStore::storeRoundTripsState() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(error.isEmpty());

    FsrsState state = Fsrs::review(Fsrs::initial(), 3, QDateTime::currentMSecsSinceEpoch()).state;
    store.save("card-1", "hash-1", state);
    store.recordRating("card-1", 3, state.lastReview, state, 3);

    QCOMPARE(store.states().value("card-1").reps, 1);
    QCOMPARE(store.hashes().value("card-1"), QString("hash-1"));
    QCOMPARE(store.states().value("card-1").stability, state.stability);

    store.removeExcept({});
    QVERIFY(store.states().isEmpty());

    const qint64 today = QDateTime::currentMSecsSinceEpoch();
    store.recordRating("card-2", 3, today, state, 3);
    store.recordRating("card-2", 1, today - 86400000LL, state, 1);
    const ReviewSummary summary = store.summary();
    QCOMPARE(summary.total, 3);
    QCOMPARE(summary.studyDayRatings, 2);
    QCOMPARE(summary.studyDayGood, 2);
    QCOMPARE(summary.streak, 2);
}

void TestStore::storeRemembersSettings() {
    QTemporaryDir dir;
    const QString path = dir.filePath("state.db");
    {
        Store store(path);
        QString error;
        QVERIFY(store.open(&error));
        QCOMPARE(store.setting("new_card_allowance", "20"), QString("20"));
        QVERIFY(store.setSetting("new_card_allowance", "12"));
        QVERIFY(store.setSetting("desired_retention", "0.85"));
    }
    Store reopened(path);
    QString error;
    QVERIFY(reopened.open(&error));
    QCOMPARE(reopened.setting("new_card_allowance", "20"), QString("12"));
    QCOMPARE(reopened.setting("desired_retention", "0.90"), QString("0.85"));
}

void TestStore::storeCountsNewCardsToday() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.recordRating("new-1", 3, now, state, 1, true, QStringLiteral("Alpha"));
    store.recordRating("new-1", 3, now, state, 1, true, QStringLiteral("Alpha")); // same card twice
    store.recordRating("old-1", 3, now, state, 1, false, QStringLiteral("Alpha"));
    QCOMPARE(store.newCardsOnStudyDay(), 1);
    QCOMPARE(store.summary().newCardsOnStudyDay, 1);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Alpha")), 1);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Beta")), 0);

    store.recordRating("new-2", 1, now, state, 1, true, QStringLiteral("Beta"));
    QCOMPARE(store.newCardsOnStudyDay(), 2);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Alpha")), 1);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Beta")), 1);
}

void TestStore::storeUsesConfiguredDayStart() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 start0 = Config::studyDayStartMs(now, 0);
    const qint64 start3 = Config::studyDayStartMs(now, 3);
    // This review lands in exactly one of the two study-day windows,
    // whichever side of the 3 AM boundary the clock is currently on.
    const qint64 probe = start3 > start0 ? start0 + 60000 : start0 - 60000;
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, probe).state;
    store.recordRating("edge", 3, probe, state, 1, true, QStringLiteral("Edge"));
    const int midnight = store.newCardsOnStudyDay(0);
    const int atThree = store.newCardsOnStudyDay(3);
    QCOMPARE(midnight + atThree, 1);
    QCOMPARE(store.summary(0).newCardsOnStudyDay, midnight);
    QCOMPARE(store.summary(3).newCardsOnStudyDay, atThree);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Edge")), midnight);
    QCOMPARE(store.newCardsOnStudyDay(3, QStringLiteral("Edge")), atThree);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Other")), 0);
}

void TestStore::openInvalidPathSetsError() {
    QTemporaryDir dir;
    // An existing directory cannot be used as a SQLite database file.
    Store store(dir.path());
    QString error;
    QVERIFY(!store.open(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.lastError(), error);
}

void TestStore::openTwiceIsIdempotent() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(error.isEmpty());

    const FsrsState state = Fsrs::initial();
    store.save("card", "hash", state);

    QVERIFY(store.open(&error));
    QVERIFY(error.isEmpty());
    QCOMPARE(store.hashes().value(QStringLiteral("card")), QStringLiteral("hash"));
}

void TestStore::stickyErrorsFromFailedQueries() {
    QTemporaryDir dir;
    const QString path = dir.filePath("state.db");
    Store store(path);
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(store.lastError().isEmpty());

    // Simulate schema drift through a second connection: every later cards
    // query fails, and the error stays until clearError() is called.
    const QString connection = QStringLiteral("omacards-store-error-test");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("DROP TABLE cards")));
        }
        db.close();
        db = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connection);

    QVERIFY(store.states().isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    const QString sticky = store.lastError();

    // A successful query does not clear a previous error.
    QCOMPARE(store.setting(QStringLiteral("anything"), QStringLiteral("fallback")), QStringLiteral("fallback"));
    QCOMPARE(store.lastError(), sticky);
    QCOMPARE(store.newCardsOnStudyDay(), 0);
    QCOMPARE(store.lastError(), sticky);

    store.clearError();
    QVERIFY(store.lastError().isEmpty());
    QVERIFY(store.hashes().isEmpty());
    QVERIFY(!store.lastError().isEmpty());
    store.clearError();
    QVERIFY(store.lastError().isEmpty());
}

void TestStore::unopenedStoreIsInert() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));

    QVERIFY(store.states().isEmpty());
    QVERIFY(store.hashes().isEmpty());
    QVERIFY(store.lastError().isEmpty());

    const ReviewSummary summary = store.summary();
    QCOMPARE(summary.total, 0);
    QCOMPARE(summary.studyDayRatings, 0);
    QCOMPARE(summary.studyDayGood, 0);
    QCOMPARE(summary.streak, 0);
    QCOMPARE(summary.newCardsOnStudyDay, 0);
    QVERIFY(store.lastError().isEmpty());

    // Pin the current behaviour: only newCardsOnStudyDay() reports a closed DB.
    QCOMPARE(store.newCardsOnStudyDay(), 0);
    QCOMPARE(store.lastError(), QStringLiteral("State database is not open"));
    store.clearError();

    QCOMPARE(store.setting(QStringLiteral("missing"), QStringLiteral("fallback")), QStringLiteral("fallback"));
    QVERIFY(store.setting(QStringLiteral("missing")).isEmpty());
    QVERIFY(!store.setSetting(QStringLiteral("missing"), QStringLiteral("value")));
    QVERIFY(store.lastError().isEmpty());

    const FsrsState state = Fsrs::initial();
    store.save(QStringLiteral("card"), QStringLiteral("hash"), state);
    store.recordRating(QStringLiteral("card"), 3, 1000, state, 1, true);
    store.removeExcept({});
    QVERIFY(store.lastError().isEmpty());

    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(store.states().isEmpty());
    QVERIFY(store.hashes().isEmpty());
    QCOMPARE(store.summary().total, 0);
}

void TestStore::saveUpsertsByCardId() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    FsrsState first;
    first.stability = 1.5;
    first.difficulty = 2.5;
    first.due = 1000;
    first.lastReview = 500;
    first.reps = 1;
    first.lapses = 0;

    FsrsState second = first;
    second.stability = 3.5;
    second.difficulty = 4.5;
    second.due = 2000;
    second.lastReview = 1500;
    second.reps = 7;
    second.lapses = 2;

    store.save(QStringLiteral("same"), QStringLiteral("hash-1"), first);
    store.save(QStringLiteral("same"), QStringLiteral("hash-2"), second);

    QCOMPARE(store.states().size(), 1);
    QCOMPARE(store.hashes().size(), 1);
    QCOMPARE(store.hashes().value(QStringLiteral("same")), QStringLiteral("hash-2"));

    const FsrsState stored = store.states().value(QStringLiteral("same"));
    QCOMPARE(stored.stability, second.stability);
    QCOMPARE(stored.difficulty, second.difficulty);
    QCOMPARE(stored.due, second.due);
    QCOMPARE(stored.lastReview, second.lastReview);
    QCOMPARE(stored.reps, second.reps);
    QCOMPARE(stored.lapses, second.lapses);
}

void TestStore::removeExceptKeepsReviewsLog() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, QDateTime::currentMSecsSinceEpoch()).state;
    store.save(QStringLiteral("keep-1"), QStringLiteral("h1"), state);
    store.save(QStringLiteral("keep-2"), QStringLiteral("h2"), state);
    store.save(QStringLiteral("gone"), QStringLiteral("hg"), state);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    store.recordRating(QStringLiteral("keep-1"), 3, now, state, 1);
    store.recordRating(QStringLiteral("gone"), 1, now, state, 1);
    store.recordRating(QStringLiteral("gone"), 3, now, state, 1);

    store.removeExcept({QStringLiteral("keep-1"), QStringLiteral("keep-2")});

    QCOMPARE(store.states().size(), 2);
    QVERIFY(store.states().contains(QStringLiteral("keep-1")));
    QVERIFY(store.states().contains(QStringLiteral("keep-2")));
    QVERIFY(!store.states().contains(QStringLiteral("gone")));
    QCOMPARE(store.hashes().size(), 2);
    QVERIFY(!store.hashes().contains(QStringLiteral("gone")));

    // Reviews are intentionally not pruned with the cards.
    const ReviewSummary summary = store.summary();
    QCOMPARE(summary.total, 3);
    QCOMPARE(summary.studyDayRatings, 3);
    QCOMPARE(summary.studyDayGood, 2);
}

void TestStore::removeExceptPruneIsAtomicOnDeleteFailure() {
    QTemporaryDir dir;
    const QString path = dir.filePath("state.db");
    Store store(path);
    QString error;
    QVERIFY(store.open(&error));

    const FsrsState state = Fsrs::initial();
    store.save(QStringLiteral("keep"), QStringLiteral("h-keep"), state);
    store.save(QStringLiteral("stale-a"), QStringLiteral("h-a"), state);
    store.save(QStringLiteral("stale-b"), QStringLiteral("h-b"), state);

    // Fail the second DELETE of a prune. A per-id loop leaves the first stale
    // Card gone; one set-based statement rolls every stale Card back.
    const QString connection = QStringLiteral("omacards-store-prune-trigger");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE delete_count (n INTEGER NOT NULL)")));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO delete_count (n) VALUES (0)")));
            QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER count_card_deletes AFTER DELETE ON cards BEGIN "
                                              "UPDATE delete_count SET n = n + 1; END")));
            QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER abort_second_card_delete BEFORE DELETE ON cards "
                                              "WHEN (SELECT n FROM delete_count) >= 1 BEGIN "
                                              "SELECT RAISE(ABORT, 'second delete blocked'); END")));
        }
        db.close();
        db = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connection);
    store.clearError();

    store.removeExcept({QStringLiteral("keep")});
    QVERIFY2(!store.lastError().isEmpty(), "A failed prune must set the sticky error");
    QVERIFY2(store.lastError().contains(QStringLiteral("second delete blocked")), qPrintable(store.lastError()));

    const QHash<QString, FsrsState> states = store.states();
    QCOMPARE(states.size(), 3);
    QVERIFY(states.contains(QStringLiteral("keep")));
    QVERIFY(states.contains(QStringLiteral("stale-a")));
    QVERIFY(states.contains(QStringLiteral("stale-b")));
}

void TestStore::migratesLegacyReviewsSchema() {
    QTemporaryDir dir;
    const QString path = dir.filePath("legacy.db");
    const QString connection = QStringLiteral("omacards-store-legacy-setup");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec(
                QStringLiteral("CREATE TABLE reviews (id INTEGER PRIMARY KEY AUTOINCREMENT, card_id TEXT NOT NULL, "
                               "rating INTEGER NOT NULL, at INTEGER NOT NULL, stability REAL NOT NULL, "
                               "difficulty REAL NOT NULL, interval INTEGER NOT NULL)")));
            QVERIFY(query.prepare(
                QStringLiteral("INSERT INTO reviews (card_id, rating, at, stability, difficulty, interval) "
                               "VALUES ('legacy', 3, :at, 1.5, 2.5, 1)")));
            query.bindValue(QStringLiteral(":at"), QDateTime::currentMSecsSinceEpoch());
            QVERIFY(query.exec());
        }
        db.close();
        db = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connection);

    Store store(path);
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(store.lastError().isEmpty());

    QCOMPARE(store.newCardsOnStudyDay(), 0);
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(store.summary().total, 1);

    const QString checkConnection = QStringLiteral("omacards-store-legacy-check");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), checkConnection);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("PRAGMA table_info(reviews)")));
            bool hasNewCard = false;
            bool hasDeckId = false;
            while (query.next()) {
                if (query.value(1).toString() == QStringLiteral("new_card"))
                    hasNewCard = true;
                if (query.value(1).toString() == QStringLiteral("deck_id"))
                    hasDeckId = true;
            }
            QVERIFY(hasNewCard);
            QVERIFY(hasDeckId);
            QVERIFY(query.exec(QStringLiteral("SELECT deck_id FROM reviews")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QString());
        }
        db.close();
        db = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(checkConnection);
}

void TestStore::summaryStreakBreaksOnMissingDay() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QDate today = Config::studyDay(now, 0);
    const qint64 todayMs = now;
    const qint64 yesterdayMs = QDateTime(today.addDays(-1), QTime(12, 0)).toMSecsSinceEpoch();
    const qint64 threeDaysAgoMs = QDateTime(today.addDays(-3), QTime(12, 0)).toMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;

    store.recordRating(QStringLiteral("today"), 3, todayMs, state, 1);
    store.recordRating(QStringLiteral("yesterday"), 3, yesterdayMs, state, 1);
    store.recordRating(QStringLiteral("old"), 1, threeDaysAgoMs, state, 1);

    const ReviewSummary summary = store.summary();
    QCOMPARE(summary.total, 3);
    QCOMPARE(summary.studyDayRatings, 1);
    QCOMPARE(summary.studyDayGood, 1);
    // The two-day-old gap stops the streak before the third review.
    QCOMPARE(summary.streak, 2);
}

void TestStore::summaryStreakSurvivesDayWithoutReviews() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QDate today = Config::studyDay(now, 0);
    const qint64 yesterdayMs = QDateTime(today.addDays(-1), QTime(12, 0)).toMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.recordRating(QStringLiteral("yesterday"), 3, yesterdayMs, state, 1);

    const ReviewSummary summary = store.summary();
    QCOMPARE(summary.total, 1);
    QCOMPARE(summary.studyDayRatings, 0);
    QCOMPARE(summary.studyDayGood, 0);
    QCOMPARE(summary.streak, 1);
}

void TestStore::summaryCountsOnlyGood() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.recordRating(QStringLiteral("good"), 3, now, state, 1);
    store.recordRating(QStringLiteral("again"), 1, now, state, 1);
    store.recordRating(QStringLiteral("hard"), 2, now, state, 1);
    store.recordRating(QStringLiteral("easy"), 4, now, state, 1);

    const ReviewSummary summary = store.summary();
    QCOMPARE(summary.total, 4);
    QCOMPARE(summary.studyDayRatings, 4);
    QCOMPARE(summary.studyDayGood, 1);
}

void TestStore::summaryUsesStudyDayForReviewDates() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const int dayStart = 4;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 start = Config::studyDayStartMs(now, dayStart);
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;

    // One millisecond on either side of the study-day boundary: raw time is
    // almost equal, but the two reviews belong to consecutive study days.
    store.recordRating(QStringLiteral("before"), 3, start - 1, state, 1);
    store.recordRating(QStringLiteral("after"), 3, start + 1, state, 1);

    const ReviewSummary summary = store.summary(dayStart);
    QCOMPARE(summary.total, 2);
    QCOMPARE(summary.studyDayRatings, 1);
    QCOMPARE(summary.studyDayGood, 1);
    QCOMPARE(summary.streak, 2);
}

void TestStore::summaryKeepsThreeStudyDaysWithNonDefaultStart() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const int dayStart = 4;
    const QDate today = Config::studyDay(QDateTime::currentMSecsSinceEpoch(), dayStart);
    const auto atOnDay = [&today](int offset) {
        return QDateTime(today.addDays(offset), QTime(12, 0)).toMSecsSinceEpoch();
    };
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, atOnDay(0)).state;

    store.recordRating(QStringLiteral("today-good"), 3, atOnDay(0), state, 1, true);
    store.recordRating(QStringLiteral("today-again"), 1, atOnDay(0) + 1000, state, 1);
    store.recordRating(QStringLiteral("today-easy"), 4, atOnDay(0) + 2000, state, 1);
    store.recordRating(QStringLiteral("yesterday-good"), 3, atOnDay(-1), state, 1, true);
    store.recordRating(QStringLiteral("older-again"), 1, atOnDay(-2), state, 1);
    QVERIFY(store.lastError().isEmpty());

    const ReviewSummary summary = store.summary(dayStart);
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(summary.total, 5);
    QCOMPARE(summary.studyDayRatings, 3);
    QCOMPARE(summary.studyDayGood, 1);
    QCOMPARE(summary.streak, 3);
    QCOMPARE(summary.newCardsOnStudyDay, 1);
}

void TestStore::settingFallbackAndUpsert() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    QCOMPARE(store.setting(QStringLiteral("absent"), QStringLiteral("fallback")), QStringLiteral("fallback"));
    QVERIFY(store.setSetting(QStringLiteral("theme"), QStringLiteral("one")));
    QCOMPARE(store.setting(QStringLiteral("theme"), QStringLiteral("fallback")), QStringLiteral("one"));
    QVERIFY(store.setSetting(QStringLiteral("theme"), QStringLiteral("two")));
    QCOMPARE(store.setting(QStringLiteral("theme"), QStringLiteral("fallback")), QStringLiteral("two"));
}

void TestStore::renameMovesCurrentAndLastStudiedSettings() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    QVERIFY(store.setCurrentDeck(QStringLiteral("Old")));
    QVERIFY(store.setDeckLastStudied(QStringLiteral("Earlier"), 100));
    QVERIFY(store.setDeckLastStudied(QStringLiteral("Old"), 200));
    QVERIFY(store.setDeckLastStudied(QStringLiteral("Later"), 300));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.recordRating(QStringLiteral("old-card"), 3, now, state, 1, true, QStringLiteral("Old"));
    store.recordRating(QStringLiteral("other-card"), 3, now, state, 1, true, QStringLiteral("Earlier"));

    QVERIFY(store.moveDeckSettings(QStringLiteral("Old"), QStringLiteral("New")));

    QCOMPARE(store.currentDeck(), QStringLiteral("New"));
    const QHash<QString, qint64> studied = store.deckLastStudied();
    QCOMPARE(studied.size(), 3);
    QVERIFY(!studied.contains(QStringLiteral("Old")));
    // The unchanged stamp keeps New in Old's position in the last-studied order.
    QCOMPARE(studied.value(QStringLiteral("New")), qint64(200));
    QCOMPARE(studied.value(QStringLiteral("Earlier")), qint64(100));
    QCOMPARE(studied.value(QStringLiteral("Later")), qint64(300));
    // The Study-day usage follows the renamed Deck, as its settings do.
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("New")), 1);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Old")), 0);
    QCOMPARE(store.newCardsOnStudyDay(0, QStringLiteral("Earlier")), 1);
}

void TestStore::migratesLegacyNewCardAllowanceKey() {
    QTemporaryDir dir;
    const QString path = dir.filePath("legacy.db");
    const QString connection = QStringLiteral("omacards-store-allowance-setup");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO settings (key, value) VALUES ('new_per_day', '12')")));
        }
        db.close();
        db = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connection);

    Store store(path);
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(store.setting(QStringLiteral("new_card_allowance"), QStringLiteral("-1")), QStringLiteral("12"));
    // The old key stays readable for consumers that have not migrated yet.
    QCOMPARE(store.setting(QStringLiteral("new_per_day"), QStringLiteral("-1")), QStringLiteral("12"));

    // Once both keys exist the canonical one wins: migration never overwrites it.
    QVERIFY(store.setSetting(QStringLiteral("new_card_allowance"), QStringLiteral("7")));
    Store reopened(path);
    QVERIFY(reopened.open(&error));
    QVERIFY(error.isEmpty());
    QCOMPARE(reopened.setting(QStringLiteral("new_card_allowance"), QStringLiteral("-1")), QStringLiteral("7"));
    QCOMPARE(reopened.setting(QStringLiteral("new_per_day"), QStringLiteral("-1")), QStringLiteral("12"));
}

void TestStore::schedulingRecordFindsOnlyOneCard() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    FsrsState first = Fsrs::review(Fsrs::initial(), 3, now).state;
    first.reps = 2;
    first.lapses = 1;
    const FsrsState second = Fsrs::review(Fsrs::initial(), 1, now).state;
    store.save(QStringLiteral("one"), QStringLiteral("hash-one"), first);
    store.save(QStringLiteral("two"), QStringLiteral("hash-two"), second);
    store.clearError();

    const SchedulingRecord found = store.schedulingRecord(QStringLiteral("one"));
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(found.hash, QStringLiteral("hash-one"));
    QCOMPARE(found.state.reps, 2);
    QCOMPARE(found.state.lapses, 1);
    QCOMPARE(found.state.due, first.due);
    QCOMPARE(found.state.stability, first.stability);
    QCOMPARE(found.state.difficulty, first.difficulty);
    QCOMPARE(found.state.lastReview, first.lastReview);

    const SchedulingRecord missing = store.schedulingRecord(QStringLiteral("missing"));
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(missing.hash, QString());
    QCOMPARE(missing.state.reps, 0);
}

void TestStore::ratingWriteRollsBackBothTablesOnFailure() {
    QTemporaryDir dir;
    const QString path = dir.filePath("state.db");
    Store store(path);
    QString error;
    QVERIFY(store.open(&error));

    // Schema drift through a second connection: the cards write succeeds but
    // the history append cannot, so only a real rollback can undo the first.
    const QString connection = QStringLiteral("omacards-store-rollback-setup");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("DROP TABLE reviews")));
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE reviews (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                              "card_id TEXT NOT NULL, new_card INTEGER NOT NULL DEFAULT 0)")));
        }
        db.close();
        db = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connection);

    const FsrsState state = Fsrs::initial();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVERIFY(store.beginRatingWrite());
    store.save(QStringLiteral("rolled-back"), QStringLiteral("hash"), state);
    store.recordRating(QStringLiteral("rolled-back"), 3, now, state, 3, true);
    QVERIFY(!store.finishRatingWrite());
    // SQLite reports the schema drift as a parameter mismatch; the raw store
    // error must survive the rollback unchanged.
    QVERIFY2(!store.lastError().isEmpty(), qPrintable(store.lastError()));

    store.clearError();
    QVERIFY(store.states().isEmpty());
    QVERIFY(store.hashes().isEmpty());
    QVERIFY(store.lastError().isEmpty());
}
