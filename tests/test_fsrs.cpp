#include <QtTest>
#include <QDateTime>

#include "test_fsrs.h"
#include "test_support.h"
#include "fsrs.h"

using namespace TestSupport;

void TestFsrs::fsrsInitialState() {
    QCOMPARE(Fsrs::initial().reps, 0);
    QVERIFY(Fsrs::initialStability(3) > Fsrs::initialStability(1));
    QVERIFY(Fsrs::initialDifficulty(1) > Fsrs::initialDifficulty(3));
    const double recall = Fsrs::retrievability(Fsrs::initial(), QDateTime::currentMSecsSinceEpoch());
    QCOMPARE(recall, 0.0);
}

void TestFsrs::fsrsReviewMovesDueForward() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const ReviewResult first = Fsrs::review(Fsrs::initial(), 3, now);
    QVERIFY(first.state.stability > 0.0);
    QVERIFY(first.intervalDays >= 1);
    QVERIFY(first.state.due > now);
    QCOMPARE(first.state.reps, 1);

    // A same-day repeat grows stability without resetting the memory state.
    const ReviewResult repeat = Fsrs::review(first.state, 3, now);
    QVERIFY(repeat.state.stability >= first.state.stability);
    QCOMPARE(repeat.state.reps, 2);

    // A lapse after a long delay lowers stability and raises difficulty.
    const qint64 later = now + 30LL * 86400000LL;
    const ReviewResult lapse = Fsrs::review(repeat.state, 1, later);
    QVERIFY(lapse.state.stability < repeat.state.stability);
    QCOMPARE(lapse.state.lapses, 1);
    QVERIFY(lapse.retrievability < 1.0);
}

void TestFsrs::fsrsRetentionShapesIntervals() {
    QVERIFY(Fsrs::interval(10.0, 0.80) > Fsrs::interval(10.0, 0.90));
    QVERIFY(Fsrs::interval(10.0, 0.98) < Fsrs::interval(10.0, 0.90));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const ReviewResult relaxed = Fsrs::review(Fsrs::initial(), 3, now, 0.80);
    const ReviewResult strict = Fsrs::review(Fsrs::initial(), 3, now, 0.95);
    QVERIFY(relaxed.intervalDays >= strict.intervalDays);
}
