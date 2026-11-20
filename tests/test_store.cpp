#include <QtTest>
#include <QDateTime>
#include <QTemporaryDir>

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
    store.logReview("card-1", 3, state.lastReview, state, 3);

    QCOMPARE(store.states().value("card-1").reps, 1);
    QCOMPARE(store.hashes().value("card-1"), QString("hash-1"));
    QCOMPARE(store.states().value("card-1").stability, state.stability);

    store.removeExcept({});
    QVERIFY(store.states().isEmpty());

    const qint64 today = QDateTime::currentMSecsSinceEpoch();
    store.logReview("card-2", 3, today, state, 3);
    store.logReview("card-2", 1, today - 86400000LL, state, 1);
    const ReviewSummary summary = store.summary();
    QCOMPARE(summary.total, 3);
    QCOMPARE(summary.today, 2);
    QCOMPARE(summary.todayCorrect, 2);
    QCOMPARE(summary.streakDays, 2);
}

void TestStore::storeRemembersSettings() {
    QTemporaryDir dir;
    const QString path = dir.filePath("state.db");
    {
        Store store(path);
        QString error;
        QVERIFY(store.open(&error));
        QCOMPARE(store.setting("new_per_day", "20"), QString("20"));
        QVERIFY(store.setSetting("new_per_day", "12"));
        QVERIFY(store.setSetting("desired_retention", "0.85"));
    }
    Store reopened(path);
    QString error;
    QVERIFY(reopened.open(&error));
    QCOMPARE(reopened.setting("new_per_day", "20"), QString("12"));
    QCOMPARE(reopened.setting("desired_retention", "0.90"), QString("0.85"));
}

void TestStore::storeCountsNewCardsToday() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.logReview("new-1", 3, now, state, 1, true);
    store.logReview("new-1", 3, now, state, 1, true); // same card twice
    store.logReview("old-1", 3, now, state, 1, false);
    QCOMPARE(store.newCardsToday(), 1);
    QCOMPARE(store.summary().newToday, 1);

    store.logReview("new-2", 1, now, state, 1, true);
    QCOMPARE(store.newCardsToday(), 2);
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
    store.logReview("edge", 3, probe, state, 1, true);
    const int midnight = store.newCardsToday(0);
    const int atThree = store.newCardsToday(3);
    QCOMPARE(midnight + atThree, 1);
    QCOMPARE(store.summary(0).newToday, midnight);
    QCOMPARE(store.summary(3).newToday, atThree);
}
