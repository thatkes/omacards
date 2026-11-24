#include <QtTest>

#include "test_insessionprogression.h"
#include "config.h"
#include "deck.h"
#include "domain.h"
#include "insessionprogression.h"

namespace {

Card cardWithId(const QString &id) {
    Card card;
    card.id = id;
    return card;
}

QVector<Card> cardsWithIds(const QStringList &ids) {
    QVector<Card> cards;
    for (const QString &id : ids)
        cards << cardWithId(id);
    return cards;
}

} // namespace

void TestInSessionProgression::nextResultCarriesKindCardWaitAndLeitnerBox() {
    InSessionProgression progression;
    progression.buildCram(cardsWithIds({"c1", "c2"}));

    const InSessionProgression::NextResult first = progression.next(0);
    QCOMPARE(first.kind, InSessionProgression::Next::Ready);
    QCOMPARE(first.cardId, QStringLiteral("c1"));
    QCOMPARE(first.waitMs, qint64(0));
    QCOMPARE(first.leitnerBox, 1);
    QCOMPARE(progression.currentLeitnerBox(), 1);
}

void TestInSessionProgression::reviewOrderComesFromNextResults() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("due1"), QStringLiteral("due2")}, {QStringLiteral("new1")});

    const InSessionProgression::NextResult first = progression.next(1000);
    QCOMPARE(first.kind, InSessionProgression::Next::Ready);
    QCOMPARE(first.cardId, QStringLiteral("due1"));
    QCOMPARE(first.waitMs, qint64(0));
    QCOMPARE(first.leitnerBox, 0); // Review has no Leitner box
    QCOMPARE(progression.next(1000).cardId, QStringLiteral("due2"));
    QCOMPARE(progression.next(1000).cardId, QStringLiteral("new1"));

    const InSessionProgression::NextResult done = progression.next(1000);
    QCOMPARE(done.kind, InSessionProgression::Next::Finished);
    QVERIFY(done.cardId.isEmpty());
    QCOMPARE(done.waitMs, qint64(0));
    QCOMPARE(done.leitnerBox, 0);
    QCOMPARE(progression.currentLeitnerBox(), 0);
}

void TestInSessionProgression::upcomingPeeksWithoutConsuming() {
    InSessionProgression review;
    review.buildReview({"a", "b", "c"}, {"new"}, {{"b", 4000}});
    const QHash<QString, qint64> pendingBefore = review.pendingRepeats();

    QCOMPARE(review.upcoming(2), QStringList({"a", "b"}));
    QCOMPARE(review.upcoming(0), QStringList());
    QCOMPARE(review.pendingRepeats(), pendingBefore);
    QCOMPARE(review.next(1000).cardId, QStringLiteral("a"));
    QCOMPARE(review.upcoming(2), QStringList({"b", "c"}));

    InSessionProgression cram;
    cram.buildCram(cardsWithIds({"c1", "c2", "c3"}));
    QCOMPARE(cram.upcoming(2), QStringList({"c1", "c2"}));
    QCOMPARE(cram.next(0).cardId, QStringLiteral("c1"));
    cram.advanceCram(QStringLiteral("c1"), Rating::Good);
    QCOMPARE(cram.upcoming(2), QStringList({"c2", "c3"}));
}

void TestInSessionProgression::pendingRepeatWaitComesFromNextResult() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("a"), QStringLiteral("b")});

    const InSessionProgression::NextResult taken = progression.next(1000);
    QCOMPARE(taken.cardId, QStringLiteral("a"));
    progression.repeatAfter(QStringLiteral("a"), 1500);
    QCOMPARE(progression.pendingRepeatCount(), 1);

    const InSessionProgression::NextResult other = progression.next(1000);
    QCOMPARE(other.kind, InSessionProgression::Next::Ready);
    QCOMPARE(other.cardId, QStringLiteral("b"));
    QCOMPARE(other.waitMs, qint64(500)); // the Pending repeat left after taking b

    // No other card is ready, so the Pending repeat is shown before its
    // original deadline instead of ending in a wait.
    const InSessionProgression::NextResult repeated = progression.next(1200);
    QCOMPARE(repeated.kind, InSessionProgression::Next::Ready);
    QCOMPARE(repeated.cardId, QStringLiteral("a"));
    QCOMPARE(repeated.waitMs, qint64(0));
    QCOMPARE(repeated.leitnerBox, 0);
    QCOMPARE(progression.pendingRepeatCount(), 0);
    QCOMPARE(progression.next(1500).kind, InSessionProgression::Next::Finished);

    InSessionProgression clamped;
    clamped.buildReview({"ready", "overdue", "later"}, {}, {{"overdue", 500}, {"later", 1500}});
    const InSessionProgression::NextResult readyWithOverdue = clamped.next(1200);
    QCOMPARE(readyWithOverdue.kind, InSessionProgression::Next::Ready);
    QCOMPARE(readyWithOverdue.cardId, QStringLiteral("ready"));
    QCOMPARE(readyWithOverdue.waitMs, qint64(1)); // an elapsed deadline is clamped

    const InSessionProgression::NextResult overdue = clamped.next(1200);
    QCOMPARE(overdue.cardId, QStringLiteral("overdue"));
    QCOMPARE(overdue.waitMs, qint64(300));

    const InSessionProgression::NextResult later = clamped.next(1200);
    QCOMPARE(later.kind, InSessionProgression::Next::Ready);
    QCOMPARE(later.cardId, QStringLiteral("later"));
    QCOMPARE(later.waitMs, qint64(0));

    InSessionProgression now;
    now.buildReview({"ready", "repeat"}, {}, {{"repeat", 1200}});
    QCOMPARE(now.next(1200).waitMs, qint64(1)); // a deadline of "now" is clamped too
}

void TestInSessionProgression::nextResultUsesSmallestPendingDeadline() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}, {},
                            {{QStringLiteral("b"), 1500}, {QStringLiteral("c"), 2500}});

    const InSessionProgression::NextResult ready = progression.next(1000);
    QCOMPARE(ready.cardId, QStringLiteral("a"));
    QCOMPARE(ready.waitMs, qint64(500));

    // With no queued card ready, the oldest deadline is shown first even
    // though its delay has not expired.
    const InSessionProgression::NextResult oldest = progression.next(1000);
    QCOMPARE(oldest.kind, InSessionProgression::Next::Ready);
    QCOMPARE(oldest.cardId, QStringLiteral("b"));
    QCOMPARE(oldest.waitMs, qint64(1500)); // the remaining repeat's deadline
    QCOMPARE(progression.next(1000).cardId, QStringLiteral("c"));
    QCOMPARE(progression.next(1000).kind, InSessionProgression::Next::Finished);
}

void TestInSessionProgression::rebuildPreservesAbsolutePendingDeadlines() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("a"), QStringLiteral("b")});
    QCOMPARE(progression.next(1000).cardId, QStringLiteral("a"));
    progression.repeatAfter(QStringLiteral("a"), 1500);

    const QHash<QString, qint64> pending = progression.pendingRepeats();
    InSessionProgression rebuilt;
    rebuilt.buildReview({QStringLiteral("b")}, {}, pending);

    QCOMPARE(rebuilt.pendingRepeats().value(QStringLiteral("a")), qint64(1500));
    QCOMPARE(rebuilt.next(1000).cardId, QStringLiteral("b"));
    QCOMPARE(rebuilt.next(1200).cardId, QStringLiteral("a"));
    QCOMPARE(rebuilt.next(1500).kind, InSessionProgression::Next::Finished);
}

void TestInSessionProgression::pendingAlreadyDueIsNotDuplicated() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("a"), QStringLiteral("b")}, {}, {{QStringLiteral("a"), 1500}});

    QCOMPARE(progression.next(1000).cardId, QStringLiteral("b"));
    QCOMPARE(progression.next(1200).cardId, QStringLiteral("a"));
    QCOMPARE(progression.next(1500).kind, InSessionProgression::Next::Finished);
}

void TestInSessionProgression::repeatAfterOverwritesDeadline() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("a")});
    progression.repeatAfter(QStringLiteral("a"), 1500);
    progression.repeatAfter(QStringLiteral("a"), 2000);
    QCOMPARE(progression.pendingRepeatCount(), 1);
    QCOMPARE(progression.pendingRepeats().value(QStringLiteral("a")), qint64(2000));

    QCOMPARE(progression.next(1500).cardId, QStringLiteral("a"));
    QCOMPARE(progression.next(2000).kind, InSessionProgression::Next::Finished);
}

void TestInSessionProgression::pendingRepeatsIsACopy() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("a"), QStringLiteral("b")}, {}, {{QStringLiteral("a"), 5000}});

    QHash<QString, qint64> pending = progression.pendingRepeats();
    pending.insert(QStringLiteral("b"), 10);
    pending.insert(QStringLiteral("a"), 10);
    QCOMPARE(progression.pendingRepeatCount(), 1);
    QCOMPARE(progression.pendingRepeats().value(QStringLiteral("a")), qint64(5000));
    QVERIFY(!progression.pendingRepeats().contains(QStringLiteral("b")));

    QCOMPARE(progression.next(1000).cardId, QStringLiteral("b"));
    QCOMPARE(progression.next(1000).cardId, QStringLiteral("a"));
}

void TestInSessionProgression::buildCramKeepsTheGivenOrder() {
    InSessionProgression progression;
    progression.buildCram(cardsWithIds({"c1", "c2", "c3"}));

    QCOMPARE(progression.boxDistribution(), QVector<int>({3, 0, 0, 0, 0}));

    QCOMPARE(progression.next(0).cardId, QStringLiteral("c1"));
    progression.advanceCram(QStringLiteral("c1"), Rating::Good);
    QCOMPARE(progression.boxDistribution(), QVector<int>({2, 1, 0, 0, 0}));
    QCOMPARE(progression.next(0).cardId, QStringLiteral("c2"));
    progression.advanceCram(QStringLiteral("c2"), Rating::Good);
    QCOMPARE(progression.boxDistribution(), QVector<int>({1, 2, 0, 0, 0}));
    QCOMPARE(progression.next(0).cardId, QStringLiteral("c3"));
}

void TestInSessionProgression::advanceCramPromotesAndResetsTheCurrentLeitnerBox() {
    QCOMPARE(Config::kLeitnerBoxes, 5);

    InSessionProgression progression;
    progression.buildCram(cardsWithIds({"c1"}));
    QCOMPARE(progression.next(0).leitnerBox, 1);

    QCOMPARE(progression.boxDistribution(), QVector<int>({1, 0, 0, 0, 0}));
    for (int expected = 2; expected <= Config::kLeitnerBoxes; ++expected) {
        progression.advanceCram(QStringLiteral("c1"), Rating::Good);
        QCOMPARE(progression.currentLeitnerBox(), expected); // no box out-parameter
        QCOMPARE(progression.next(0).leitnerBox, expected);
        QVector<int> counts(Config::kLeitnerBoxes, 0);
        counts[expected - 1] = 1;
        QCOMPARE(progression.boxDistribution(), counts);
    }
    progression.advanceCram(QStringLiteral("c1"), Rating::Good);
    QCOMPARE(progression.currentLeitnerBox(), Config::kLeitnerBoxes);
    progression.advanceCram(QStringLiteral("c1"), Rating::Again);
    QCOMPARE(progression.currentLeitnerBox(), 1);
    QCOMPARE(progression.next(0).leitnerBox, 1);
    QCOMPARE(progression.boxDistribution(), QVector<int>({1, 0, 0, 0, 0}));
}

void TestInSessionProgression::cramIntervalsCountOtherCards() {
    InSessionProgression progression;
    progression.buildCram(cardsWithIds({"c1", "c2", "c3"}));

    QCOMPARE(progression.next(0).cardId, QStringLiteral("c1"));
    progression.advanceCram(QStringLiteral("c1"), Rating::Good);
    QCOMPARE(progression.next(0).cardId, QStringLiteral("c2")); // Good promotes by one Leitner box
    progression.advanceCram(QStringLiteral("c2"), Rating::Good);
    QCOMPARE(progression.next(0).cardId, QStringLiteral("c3"));
    progression.advanceCram(QStringLiteral("c3"), Rating::Good);

    const InSessionProgression::NextResult returnee = progression.next(0);
    QCOMPARE(returnee.cardId, QStringLiteral("c1")); // its interval counted the two other Cards
    QCOMPARE(returnee.leitnerBox, 2);
    progression.advanceCram(QStringLiteral("c1"), Rating::Again);
    QCOMPARE(progression.currentLeitnerBox(), 1); // Again reset the rated Card to box 1
    const InSessionProgression::NextResult next = progression.next(0);
    QCOMPARE(next.cardId, QStringLiteral("c2"));
    QCOMPARE(next.leitnerBox, 2); // the next Card keeps the box it earned with Good
}

void TestInSessionProgression::cramQueueNeverEndsForOneCard() {
    InSessionProgression progression;
    progression.buildCram(cardsWithIds({"c1"}));

    for (int step = 0; step < 12; ++step) {
        const InSessionProgression::NextResult next = progression.next(0);
        QCOMPARE(next.kind, InSessionProgression::Next::Ready);
        QCOMPARE(next.cardId, QStringLiteral("c1"));
        QVERIFY(next.leitnerBox >= 1 && next.leitnerBox <= Config::kLeitnerBoxes);
        progression.advanceCram(next.cardId, step % 4 == 3 ? Rating::Again : Rating::Good);
    }
}

void TestInSessionProgression::emptyCramHasNoCurrentLeitnerBox() {
    InSessionProgression empty;
    empty.buildCram({});

    const InSessionProgression::NextResult done = empty.next(0);
    QCOMPARE(done.kind, InSessionProgression::Next::Finished);
    QCOMPARE(done.leitnerBox, 0);
    QCOMPARE(empty.currentLeitnerBox(), 0);
    QVERIFY(empty.empty());

    empty.advanceCram(QStringLiteral("unknown"), Rating::Good);
    QCOMPARE(empty.currentLeitnerBox(), 0);
    QCOMPARE(empty.next(0).kind, InSessionProgression::Next::Finished);

    InSessionProgression progression;
    progression.buildCram(cardsWithIds({"c1"}));
    QCOMPARE(progression.next(0).leitnerBox, 1);
    progression.advanceCram(QStringLiteral("unknown"), Rating::Good);
    QCOMPARE(progression.currentLeitnerBox(), 1);
}

void TestInSessionProgression::removeDropsReviewAndCramCards() {
    InSessionProgression reviews;
    reviews.buildReview({QStringLiteral("a"), QStringLiteral("b")});
    reviews.remove(QStringLiteral("a"));
    QCOMPARE(reviews.next(0).cardId, QStringLiteral("b"));
    QCOMPARE(reviews.next(0).kind, InSessionProgression::Next::Finished);
    reviews.remove(QStringLiteral("unknown"));

    InSessionProgression repeated;
    repeated.buildReview({QStringLiteral("a")});
    repeated.repeatAfter(QStringLiteral("a"), 5000);
    repeated.remove(QStringLiteral("a"));
    QCOMPARE(repeated.pendingRepeatCount(), 0);
    QVERIFY(repeated.empty());
    QCOMPARE(repeated.next(0).kind, InSessionProgression::Next::Finished);

    InSessionProgression cram;
    cram.buildCram(cardsWithIds({"c1"}));
    cram.remove(QStringLiteral("c1"));
    QCOMPARE(cram.next(0).kind, InSessionProgression::Next::Finished);
}

void TestInSessionProgression::clearDropsRepeatsAndLeitnerBox() {
    InSessionProgression progression;
    progression.buildCram(cardsWithIds({"c1"}));
    QCOMPARE(progression.next(0).leitnerBox, 1);
    progression.repeatAfter(QStringLiteral("pending"), 5000); // a combined session is cleared wholesale
    QVERIFY(!progression.empty());

    progression.clear();
    QVERIFY(progression.empty());
    QCOMPARE(progression.pendingRepeatCount(), 0);
    QCOMPARE(progression.currentLeitnerBox(), 0);
    QCOMPARE(progression.next(0).kind, InSessionProgression::Next::Finished);
}

void TestInSessionProgression::switchingToCramDropsPendingRepeats() {
    InSessionProgression progression;
    progression.buildReview({QStringLiteral("a")});
    progression.repeatAfter(QStringLiteral("a"), 5000);
    QCOMPARE(progression.pendingRepeatCount(), 1);

    progression.buildCram(cardsWithIds({"c1"}));
    QCOMPARE(progression.pendingRepeatCount(), 0);
    QVERIFY(progression.pendingRepeats().isEmpty());
    QCOMPARE(progression.next(0).cardId, QStringLiteral("c1"));
}
