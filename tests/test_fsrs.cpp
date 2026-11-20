#include <QtTest>
#include <QDateTime>
#include <QtMath>

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

void TestFsrs::fsrsInitialGradesClampAndPinWeights() {
    // The model accepts all four internal grades even though the UI only
    // exposes Again (1) and Good (3). Out-of-range grades clamp to that range.
    QVERIFY(qFuzzyCompare(Fsrs::initialStability(1), Fsrs::weights[0]));
    QVERIFY(qFuzzyCompare(Fsrs::initialStability(2), Fsrs::weights[1]));
    QVERIFY(qFuzzyCompare(Fsrs::initialStability(3), Fsrs::weights[2]));
    QVERIFY(qFuzzyCompare(Fsrs::initialStability(4), Fsrs::weights[3]));
    QVERIFY(qFuzzyCompare(Fsrs::initialStability(0), Fsrs::weights[0]));
    QVERIFY(qFuzzyCompare(Fsrs::initialStability(-5), Fsrs::weights[0]));
    QVERIFY(qFuzzyCompare(Fsrs::initialStability(5), Fsrs::weights[3]));

    // Grade 1 keeps the whole 7.1434 weight; later grades subtract an exp
    // term. Difficulty stays inside 1..10 for every clamped grade.
    QVERIFY(qFuzzyCompare(Fsrs::initialDifficulty(1), 7.1434));
    for (int grade = 1; grade <= 4; ++grade) {
        const double difficulty = Fsrs::initialDifficulty(grade);
        QVERIFY(difficulty >= 1.0);
        QVERIFY(difficulty <= 10.0);
    }
    QVERIFY(qFuzzyCompare(Fsrs::initialDifficulty(0), Fsrs::initialDifficulty(1)));
    QVERIFY(qFuzzyCompare(Fsrs::initialDifficulty(99), Fsrs::initialDifficulty(4)));
}

void TestFsrs::fsrsIntervalClampsRetentionAndMaximum() {
    // At stability 1, 90% retention resolves to exactly one day; lower
    // retention lengthens the interval and higher retention shortens it.
    QCOMPARE(Fsrs::interval(1.0, 0.90), 1);
    QCOMPARE(Fsrs::interval(1.0, 0.80), 2);
    QCOMPARE(Fsrs::interval(1.0, 0.99), 1); // rounds to zero, clamped to one

    // The floor applies to zero/negative stability and rounding to zero.
    QCOMPARE(Fsrs::interval(0.0), 1);
    QCOMPARE(Fsrs::interval(-10.0), 1);

    // Retention outside the configured window is clamped, not rejected.
    QCOMPARE(Fsrs::interval(20.0, -1.0), Fsrs::interval(20.0, Config::kMinRetention));
    QCOMPARE(Fsrs::interval(20.0, 2.0), Fsrs::interval(20.0, Config::kMaxRetention));

    // A huge stability clamps to the configured maximum when the intermediate
    // days value still fits in a 32-bit int (default retention has a modifier
    // of ~1). A lower retention gives a larger modifier, so stability*modifier
    // overflows the int cast and the negative result clamps to the floor.
    QCOMPARE(Fsrs::interval(1e9), Config::kMaxIntervalDays);
    QCOMPARE(Fsrs::interval(1e9, 0.70), 1);
}

void TestFsrs::fsrsRetrievabilityHandlesMissingAndFutureReviews() {
    const qint64 now = 1'700'000'000'000LL;
    QCOMPARE(Fsrs::retrievability(Fsrs::initial(), now), 0.0);

    FsrsState state = Fsrs::initial();
    state.lastReview = now;
    state.stability = 1.0;
    QVERIFY(qFuzzyCompare(Fsrs::retrievability(state, now), 1.0));

    // Time before the last review is treated as no elapsed time, so recall
    // stays at 1 rather than being extrapolated above 1.
    QVERIFY(qFuzzyCompare(Fsrs::retrievability(state, now - 86'400'000LL), 1.0));

    state.lastReview = now - 86'400'000LL;
    const double expected = qPow(1.0 + Fsrs::factor, Fsrs::decay);
    QVERIFY(qFuzzyCompare(Fsrs::retrievability(state, now), expected));

    state.stability = 0.0;
    QCOMPARE(Fsrs::retrievability(state, now), 0.0);
}

void TestFsrs::fsrsReviewClampsRatingAndRetention() {
    const qint64 now = 1'700'000'000'000LL;

    // Invalid boundary ratings use the same branch as the nearest valid grade.
    const ReviewResult invalidLow = Fsrs::review(Fsrs::initial(), 0, now);
    const ReviewResult again = Fsrs::review(Fsrs::initial(), 1, now);
    QVERIFY(qFuzzyCompare(invalidLow.state.stability, again.state.stability));
    QVERIFY(qFuzzyCompare(invalidLow.state.difficulty, again.state.difficulty));
    QCOMPARE(invalidLow.state.lapses, 1);

    const ReviewResult invalidHigh = Fsrs::review(Fsrs::initial(), 99, now);
    const ReviewResult easy = Fsrs::review(Fsrs::initial(), 4, now);
    QVERIFY(qFuzzyCompare(invalidHigh.state.stability, easy.state.stability));
    QCOMPARE(invalidHigh.state.lapses, 0);

    // Retention is clamped to the Config window in both directions.
    QCOMPARE(Fsrs::review(Fsrs::initial(), 3, now, -5.0).intervalDays,
             Fsrs::review(Fsrs::initial(), 3, now, Config::kMinRetention).intervalDays);
    QCOMPARE(Fsrs::review(Fsrs::initial(), 3, now, 5.0).intervalDays,
             Fsrs::review(Fsrs::initial(), 3, now, Config::kMaxRetention).intervalDays);

    // The sidecar due time always matches the reported interval in whole days.
    const ReviewResult result = Fsrs::review(Fsrs::initial(), 3, now);
    QCOMPARE(result.state.due, now + static_cast<qint64>(result.intervalDays) * 86'400'000LL);
}
