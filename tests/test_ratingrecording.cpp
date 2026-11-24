#include <QtTest>

#include <QHash>
#include <QVector>

#include "test_ratingrecording.h"
#include "test_support.h"
#include "deck.h"
#include "domain.h"
#include "fsrs.h"
#include "ratingrecording.h"

using namespace TestSupport;

namespace {

constexpr qint64 kNow = 1700000000000LL;
constexpr qint64 kDayMs = 86400000LL;

void verifyState(const FsrsState &state, double stability, double difficulty, qint64 due, int reps, int lapses,
                 qint64 lastReview = kNow) {
    QVERIFY(qAbs(state.stability - stability) < 1e-9);
    QVERIFY(qAbs(state.difficulty - difficulty) < 1e-9);
    QCOMPARE(state.due, due);
    QCOMPARE(state.lastReview, lastReview);
    QCOMPARE(state.reps, reps);
    QCOMPARE(state.lapses, lapses);
}

} // namespace

void TestRatingRecording::goodNewCardRecordsStateAndReturnsIntervalAndDue() {
    const Card card = makeCard(QStringLiteral("good"), QStringLiteral("Q?"), QStringLiteral("A"));
    RatingStoreStub store;
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(result.ok);
    QVERIFY(result.storeError.isEmpty());
    QCOMPARE(result.intervalDays, 3);
    verifyState(result.state, 3.0412, 4.490943335350446, kNow + 3 * kDayMs, 1, 0);
    verifyState(store.savedState(card.id), 3.0412, 4.490943335350446, kNow + 3 * kDayMs, 1, 0);

    const QVector<RecordedRating> reviews = store.reviews();
    QCOMPARE(reviews.size(), 1);
    QCOMPARE(reviews.first().cardId, card.id);
    QCOMPARE(reviews.first().rating, 3);
    QCOMPARE(reviews.first().at, kNow);
    QCOMPARE(reviews.first().intervalDays, 3);
}

void TestRatingRecording::againUsesStoredSchedulingState() {
    const Card card = makeCard(QStringLiteral("again"), QStringLiteral("Again?"), QStringLiteral("A"));
    FsrsState starting;
    starting.stability = 5.0;
    starting.difficulty = 5.0;
    starting.lastReview = kNow - 2 * kDayMs;
    starting.due = kNow - kDayMs;
    starting.reps = 2;
    starting.lapses = 1;

    RatingStoreStub store;
    store.seed(card, DeckIO::contentHash(card), starting);
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Again, 0.9, kNow);

    QVERIFY(result.ok);
    QVERIFY(result.storeError.isEmpty());
    QCOMPARE(result.intervalDays, 1);
    // Worked FSRS lapse branch: recall 0.9561491614605041 from a five-day
    // stability reviewed two days ago, then lapse stability 1.380719...
    verifyState(result.state, 1.3807194683897863, 6.607894767180462, kNow + kDayMs, 3, 2);
    verifyState(store.savedState(card.id), 1.3807194683897863, 6.607894767180462, kNow + kDayMs, 3, 2);

    const QVector<RecordedRating> reviews = store.reviews();
    QCOMPARE(reviews.size(), 1);
    QCOMPARE(reviews.first().cardId, card.id);
    QCOMPARE(reviews.first().rating, 1);
    QCOMPARE(reviews.first().at, kNow);
    QCOMPARE(reviews.first().intervalDays, 1);
}

void TestRatingRecording::contentHashMismatchStartsFromEmptySchedulingState() {
    const Card card = makeCard(QStringLiteral("edited"), QStringLiteral("Edited?"), QStringLiteral("New answer"));
    FsrsState stale;
    stale.stability = 9.0;
    stale.difficulty = 8.0;
    stale.lastReview = kNow - 10 * kDayMs;
    stale.due = kNow - 5 * kDayMs;
    stale.reps = 4;
    stale.lapses = 2;

    RatingStoreStub store;
    store.seed(card, QStringLiteral("old-hash"), stale);
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(result.ok);
    QVERIFY(result.storeError.isEmpty());
    QCOMPARE(result.intervalDays, 3);
    verifyState(result.state, 3.0412, 4.490943335350446, kNow + 3 * kDayMs, 1, 0);
    verifyState(store.savedState(card.id), 3.0412, 4.490943335350446, kNow + 3 * kDayMs, 1, 0);
    QCOMPARE(store.reviews().size(), 1);
}

void TestRatingRecording::reviewHistoryRecordsWhetherCardWasNew() {
    const Card existing = makeCard(QStringLiteral("existing"), QStringLiteral("Seen?"), QStringLiteral("A"));
    const Card fresh = makeCard(QStringLiteral("fresh"), QStringLiteral("New?"), QStringLiteral("A"));
    FsrsState starting;
    starting.stability = 4.0;
    starting.difficulty = 5.0;
    starting.lastReview = kNow - 3 * kDayMs;
    starting.due = kNow - 2 * kDayMs;
    starting.reps = 2;
    starting.lapses = 0;

    RatingStoreStub store;
    store.seed(existing, DeckIO::contentHash(existing), starting);
    RatingRecording recording(store);

    QVERIFY(recording.record(existing, Rating::Good, 0.9, kNow).ok);
    QVERIFY(recording.record(fresh, Rating::Good, 0.9, kNow).ok);

    const QVector<RecordedRating> reviews = store.reviews();
    QCOMPARE(reviews.size(), 2);
    QCOMPARE(reviews.at(0).cardId, existing.id);
    QVERIFY(!reviews.at(0).wasNew);
    QCOMPARE(reviews.at(1).cardId, fresh.id);
    QVERIFY(reviews.at(1).wasNew);
}

void TestRatingRecording::storeWriteFailureReturnsRawErrorAndKeepsStartingState() {
    const Card card = makeCard(QStringLiteral("write-fail"), QStringLiteral("Save?"), QStringLiteral("A"));
    FsrsState starting;
    starting.stability = 6.0;
    starting.difficulty = 5.0;
    starting.lastReview = kNow - 4 * kDayMs;
    starting.due = kNow - 2 * kDayMs;
    starting.reps = 3;
    starting.lapses = 1;

    RatingStoreStub store;
    store.seed(card, DeckIO::contentHash(card), starting);
    store.failWrites(QStringLiteral("disk full"));
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(!result.ok);
    QCOMPARE(result.failurePhase, RatingRecording::FailurePhase::StoreWrite);
    QCOMPARE(result.storeError, QStringLiteral("disk full"));
    QCOMPARE(result.intervalDays, 0);
    QCOMPARE(result.state.stability, 6.0);
    QCOMPARE(result.state.difficulty, 5.0);
    QCOMPARE(result.state.lastReview, kNow - 4 * kDayMs);
    QCOMPARE(result.state.due, kNow - 2 * kDayMs);
    QCOMPARE(result.state.reps, 3);
    QCOMPARE(result.state.lapses, 1);

    const FsrsState saved = store.savedState(card.id);
    QCOMPARE(saved.stability, 6.0);
    QCOMPARE(saved.due, kNow - 2 * kDayMs);
    QCOMPARE(saved.reps, 3);
}

void TestRatingRecording::storeReadFailureReturnsRawErrorAndReadPhase() {
    const Card card = makeCard(QStringLiteral("read-fail"), QStringLiteral("Read?"), QStringLiteral("A"));
    RatingStoreStub store;
    store.failReads(QStringLiteral("no such column: stability"));
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(!result.ok);
    QCOMPARE(result.failurePhase, RatingRecording::FailurePhase::SchedulingRead);
    QCOMPARE(result.storeError, QStringLiteral("no such column: stability"));
    QVERIFY(store.reviews().isEmpty());
}

void TestRatingRecording::recordUsesOneCardSchedulingLookup() {
    const Card card = makeCard(QStringLiteral("lookup"), QStringLiteral("Lookup?"), QStringLiteral("A"));
    const Card other = makeCard(QStringLiteral("bystander"), QStringLiteral("Other?"), QStringLiteral("A"));
    FsrsState seeded;
    seeded.stability = 4.0;
    seeded.difficulty = 5.0;
    seeded.lastReview = kNow - 2 * kDayMs;
    seeded.due = kNow - kDayMs;
    seeded.reps = 2;
    seeded.lapses = 1;

    RatingStoreStub store;
    store.seed(card, DeckIO::contentHash(card), seeded);
    store.seed(other, QStringLiteral("other-hash"), FsrsState{});
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(result.ok);
    QCOMPARE(store.lookupCount(), 1);
    QCOMPARE(result.state.reps, 3);
    QCOMPARE(result.state.lapses, 1);
    verifyState(store.savedState(card.id), result.state.stability, result.state.difficulty, result.state.due, 3, 1);
    QCOMPARE(store.savedHash(card.id), DeckIO::contentHash(card));
    verifyState(store.savedState(other.id), 0.0, 0.0, 0, 0, 0, 0);
}

void TestRatingRecording::bothWritesShareOneCommittedTransaction() {
    const Card card = makeCard(QStringLiteral("batch"), QStringLiteral("Batch?"), QStringLiteral("A"));
    RatingStoreStub store;
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(result.ok);
    QCOMPARE(store.beginCount(), 1);
    QCOMPARE(store.finishCount(), 1);
    QCOMPARE(store.commitCount(), 1);
    QCOMPARE(store.savedState(card.id).reps, 1);
    QCOMPARE(store.reviews().size(), 1);
}

void TestRatingRecording::failedWriteRollsBackBothWritesInOneTransaction() {
    const Card card = makeCard(QStringLiteral("rollback"), QStringLiteral("Rollback?"), QStringLiteral("A"));
    FsrsState seeded;
    seeded.stability = 5.0;
    seeded.difficulty = 6.0;
    seeded.lastReview = kNow - kDayMs;
    seeded.due = kNow;
    seeded.reps = 4;
    seeded.lapses = 2;

    RatingStoreStub store;
    store.seed(card, DeckIO::contentHash(card), seeded);
    store.failWrites(QStringLiteral("write boom"));
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(!result.ok);
    QCOMPARE(result.failurePhase, RatingRecording::FailurePhase::StoreWrite);
    QCOMPARE(result.storeError, QStringLiteral("write boom"));
    QCOMPARE(store.beginCount(), 1);
    QCOMPARE(store.finishCount(), 1);
    QCOMPARE(store.commitCount(), 0);
    QVERIFY(store.reviews().isEmpty());
    verifyState(store.savedState(card.id), 5.0, 6.0, kNow, 4, 2, kNow - kDayMs);
    QCOMPARE(store.savedHash(card.id), DeckIO::contentHash(card));
}

void TestRatingRecording::failedTransactionStartWritesNothing() {
    const Card card = makeCard(QStringLiteral("no-begin"), QStringLiteral("No begin?"), QStringLiteral("A"));
    RatingStoreStub store;
    store.failWritesToBegin(QStringLiteral("begin boom"));
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(!result.ok);
    QCOMPARE(result.failurePhase, RatingRecording::FailurePhase::StoreWrite);
    QCOMPARE(result.storeError, QStringLiteral("begin boom"));
    QCOMPARE(store.beginCount(), 1);
    QCOMPARE(store.finishCount(), 0);
    QVERIFY(store.reviews().isEmpty());
    verifyState(store.savedState(card.id), 0.0, 0.0, 0, 0, 0, 0);
}
