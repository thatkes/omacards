#include <QtTest>

#include <QHash>
#include <QVector>

#include "test_ratingrecording.h"
#include "deck.h"
#include "domain.h"
#include "fsrs.h"
#include "ratingrecording.h"

namespace {

constexpr qint64 kNow = 1700000000000LL;
constexpr qint64 kDayMs = 86400000LL;

Card makeCard(const QString &id, const QString &question, const QString &answer) {
    Card card;
    card.id = id;
    card.question = question;
    card.answer = answer;
    card.deck = QStringLiteral("Deck");
    return card;
}

struct RecordedRating {
    QString cardId;
    int rating = 0;
    qint64 at = 0;
    FsrsState state;
    int intervalDays = 0;
    bool wasNew = false;
};

class StubStore : public RatingRecording::StoreAdapter {
public:
    void clearError() override { m_error.clear(); }
    QHash<QString, FsrsState> states() const override {
        if (!m_readError.isEmpty())
            m_error = m_readError;
        return m_states;
    }
    QHash<QString, QString> hashes() const override { return m_hashes; }

    void save(const QString &cardId, const QString &hash, const FsrsState &state) override {
        if (!m_writeError.isEmpty()) {
            m_error = m_writeError;
            return;
        }
        m_states.insert(cardId, state);
        m_hashes.insert(cardId, hash);
    }

    void recordRating(const QString &cardId, int rating, qint64 at, const FsrsState &state, int intervalDays,
                      bool wasNew) override {
        m_reviews.append({cardId, rating, at, state, intervalDays, wasNew});
        if (!m_writeError.isEmpty())
            m_error = m_writeError;
    }

    QString lastError() const override { return m_error; }

    void seed(const Card &card, const QString &hash, const FsrsState &state) {
        m_states.insert(card.id, state);
        m_hashes.insert(card.id, hash);
    }

    void failWrites(const QString &error) { m_writeError = error; }
    void failReads(const QString &error) { m_readError = error; }

    FsrsState savedState(const QString &cardId) const { return m_states.value(cardId); }
    QVector<RecordedRating> reviews() const { return m_reviews; }

private:
    QHash<QString, FsrsState> m_states;
    QHash<QString, QString> m_hashes;
    QVector<RecordedRating> m_reviews;
    QString m_writeError;
    QString m_readError;
    mutable QString m_error;
};

void verifyState(const FsrsState &state, double stability, double difficulty, qint64 due, int reps, int lapses) {
    QVERIFY(qAbs(state.stability - stability) < 1e-9);
    QVERIFY(qAbs(state.difficulty - difficulty) < 1e-9);
    QCOMPARE(state.due, due);
    QCOMPARE(state.lastReview, kNow);
    QCOMPARE(state.reps, reps);
    QCOMPARE(state.lapses, lapses);
}

} // namespace

void TestRatingRecording::goodNewCardRecordsStateAndReturnsIntervalAndDue() {
    const Card card = makeCard(QStringLiteral("good"), QStringLiteral("Q?"), QStringLiteral("A"));
    StubStore store;
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

    StubStore store;
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

    StubStore store;
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

    StubStore store;
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

    StubStore store;
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
    StubStore store;
    store.failReads(QStringLiteral("no such column: stability"));
    RatingRecording recording(store);

    const RatingRecording::Result result = recording.record(card, Rating::Good, 0.9, kNow);

    QVERIFY(!result.ok);
    QCOMPARE(result.failurePhase, RatingRecording::FailurePhase::SchedulingRead);
    QCOMPARE(result.storeError, QStringLiteral("no such column: stability"));
    QVERIFY(store.reviews().isEmpty());
}
