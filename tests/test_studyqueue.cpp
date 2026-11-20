#include <QtTest>

#include "test_studyqueue.h"
#include "test_support.h"
#include "config.h"
#include "deck.h"
#include "domain.h"
#include "studyqueue.h"

using namespace TestSupport;

void TestStudyQueue::studyQueueReviewOrder() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("due1"), QStringLiteral("due2")}, {QStringLiteral("fresh1")});
    QString id;
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("due1"));
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("due2"));
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("fresh1"));
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Finished);
}

void TestStudyQueue::studyQueueRepeatWaitsThenReturns() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a"), QStringLiteral("b")});
    QString id;
    qint64 wait = 0;
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("a"));

    queue.repeatAfter(QStringLiteral("a"), 1500);
    QCOMPARE(queue.pendingRepeatCount(), 1);
    QCOMPARE(queue.next(1000, &id, &wait, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("b"));
    QCOMPARE(wait, qint64(500));

    QCOMPARE(queue.next(1200, &id, &wait, nullptr), StudyQueue::Next::Waiting);
    QCOMPARE(wait, qint64(300));
    QCOMPARE(queue.next(1500, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("a"));
    QCOMPARE(queue.pendingRepeatCount(), 0);
}

void TestStudyQueue::studyQueueRebuildKeepsPending() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a"), QStringLiteral("b")});
    QString id;
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    queue.repeatAfter(QStringLiteral("a"), 1500);
    const QHash<QString, qint64> pending = queue.pendingRepeats();

    StudyQueue rebuilt;
    rebuilt.buildReview({QStringLiteral("b")}, {}, pending);
    QCOMPARE(rebuilt.pendingRepeatCount(), 1);
    QCOMPARE(rebuilt.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("b"));
    QCOMPARE(rebuilt.next(1200, &id, nullptr, nullptr), StudyQueue::Next::Waiting);
    QCOMPARE(rebuilt.next(1500, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("a"));
}

void TestStudyQueue::studyQueueCramBoxes() {
    QVector<Card> cards;
    for (int i = 0; i < 3; ++i) {
        Card card;
        card.id = QStringLiteral("c%1").arg(i);
        cards << card;
    }
    StudyQueue queue;
    queue.buildCram(cards);
    QString id;
    int box = 0;
    QCOMPARE(queue.next(0, &id, nullptr, &box), StudyQueue::Next::Ready);
    const QString first = id;
    QCOMPARE(box, 1);
    queue.advanceCram(first, Rating::Good, &box);
    QCOMPARE(box, 2);
    queue.advanceCram(first, Rating::Again, &box);
    QCOMPARE(box, 1);
    QString second;
    QCOMPARE(queue.next(0, &second, nullptr, &box), StudyQueue::Next::Ready);
    QVERIFY(second != first);
}

void TestStudyQueue::studyQueueRemove() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a"), QStringLiteral("b")});
    queue.remove(QStringLiteral("a"));
    QString id;
    QCOMPARE(queue.next(0, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QString("b"));
    QCOMPARE(queue.next(0, &id, nullptr, nullptr), StudyQueue::Next::Finished);

    Card card;
    card.id = QStringLiteral("only");
    StudyQueue cram;
    cram.buildCram({card});
    cram.remove(QStringLiteral("only"));
    QCOMPARE(cram.next(0, &id, nullptr, nullptr), StudyQueue::Next::Finished);
}

void TestStudyQueue::studyQueueNextWaitEdges() {
    StudyQueue queue;
    QCOMPARE(queue.nextWait(1000), qint64(0));

    queue.buildReview({QStringLiteral("a"), QStringLiteral("b")});
    QCOMPARE(queue.nextWait(1000), qint64(0));

    queue.repeatAfter(QStringLiteral("a"), 500);
    QCOMPARE(queue.nextWait(1000), qint64(1)); // past deadline is clamped
    queue.repeatAfter(QStringLiteral("a"), 1000);
    QCOMPARE(queue.nextWait(1000), qint64(1)); // deadline now is clamped too
    queue.repeatAfter(QStringLiteral("a"), 1500);
    QCOMPARE(queue.nextWait(1000), qint64(500));
}

void TestStudyQueue::studyQueueNextWaitUsesSmallestPending() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}, {},
                      {{QStringLiteral("a"), 3500}, {QStringLiteral("b"), 1500}, {QStringLiteral("c"), 2500}});
    QCOMPARE(queue.pendingRepeatCount(), 3);
    QCOMPARE(queue.nextWait(1000), qint64(500));
}

void TestStudyQueue::studyQueueNextReturnsPostTakeWait() {
    StudyQueue plain;
    plain.buildReview({QStringLiteral("a"), QStringLiteral("b")});
    QString id;
    qint64 wait = -1;
    QCOMPARE(plain.next(1000, &id, &wait, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("a"));
    QCOMPARE(wait, qint64(0));
    QCOMPARE(plain.nextWait(1000), wait);

    StudyQueue queue;
    queue.buildReview({QStringLiteral("a"), QStringLiteral("b")}, {}, {{QStringLiteral("b"), 1700}});
    QCOMPARE(queue.next(1000, &id, &wait, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("a"));
    // waitMs is computed after the ready card has been taken.
    QCOMPARE(wait, qint64(700));
    QCOMPARE(queue.nextWait(1000), wait);
}

void TestStudyQueue::studyQueueBuildReviewPendingAlreadyDue() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a"), QStringLiteral("b")}, {}, {{QStringLiteral("a"), 1500}});

    QString id;
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("b"));
    QCOMPARE(queue.next(1200, &id, nullptr, nullptr), StudyQueue::Next::Waiting);
    QCOMPARE(queue.next(1500, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("a"));
    // No duplicate "a" was appended for the pending id.
    QCOMPARE(queue.next(1500, &id, nullptr, nullptr), StudyQueue::Next::Finished);
}

void TestStudyQueue::studyQueueRepeatAfterOverwritesDeadline() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a")});
    queue.repeatAfter(QStringLiteral("a"), 1500);
    QCOMPARE(queue.pendingRepeatCount(), 1);
    queue.repeatAfter(QStringLiteral("a"), 2000);
    QCOMPARE(queue.pendingRepeatCount(), 1);
    QCOMPARE(queue.pendingRepeats().value(QStringLiteral("a")), qint64(2000));

    QString id;
    QCOMPARE(queue.next(1500, &id, nullptr, nullptr), StudyQueue::Next::Waiting);
    QCOMPARE(queue.next(2000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("a"));
}

void TestStudyQueue::studyQueueClearEmptiesEverything() {
    StudyQueue reviews;
    reviews.buildReview({QStringLiteral("a")}, {}, {{QStringLiteral("a"), 5000}});
    QVERIFY(!reviews.empty());
    reviews.clear();
    QVERIFY(reviews.empty());
    QCOMPARE(reviews.pendingRepeatCount(), 0);
    QCOMPARE(reviews.nextWait(0), qint64(0));
    QString id;
    QCOMPARE(reviews.next(0, &id, nullptr, nullptr), StudyQueue::Next::Finished);

    Card card;
    card.id = QStringLiteral("cram");
    StudyQueue cram;
    cram.buildCram({card});
    cram.repeatAfter(QStringLiteral("pending"), 5000); // combines all three containers
    QVERIFY(!cram.empty());
    QCOMPARE(cram.pendingRepeatCount(), 1);
    cram.clear();
    QVERIFY(cram.empty());
    QCOMPARE(cram.pendingRepeatCount(), 0);
    QCOMPARE(cram.next(0, &id, nullptr, nullptr), StudyQueue::Next::Finished);
}

void TestStudyQueue::studyQueueCramEdges() {
    QCOMPARE(Config::kLeitnerBoxes, 5);

    StudyQueue empty;
    empty.buildCram({});
    QString id;
    int box = -1;
    QCOMPARE(empty.next(0, &id, nullptr, &box), StudyQueue::Next::Finished);
    QVERIFY(empty.empty());
    empty.advanceCram(QStringLiteral("unknown"), Rating::Good, &box);
    QCOMPARE(box, -1); // no-op leaves the out-param untouched
    QCOMPARE(empty.next(0, &id, nullptr, &box), StudyQueue::Next::Finished);

    Card card;
    card.id = QStringLiteral("c1");
    const QVector<Card> cards{card};
    StudyQueue queue;
    queue.buildCram(cards);
    queue.advanceCram(QStringLiteral("c1"), Rating::Good, &box);
    QCOMPARE(box, 2);
    queue.advanceCram(QStringLiteral("c1"), Rating::Good, &box);
    QCOMPARE(box, 3);
    queue.advanceCram(QStringLiteral("c1"), Rating::Good, &box);
    QCOMPARE(box, 4);
    queue.advanceCram(QStringLiteral("c1"), Rating::Good, &box);
    QCOMPARE(box, Config::kLeitnerBoxes);
    queue.advanceCram(QStringLiteral("c1"), Rating::Good, &box);
    QCOMPARE(box, Config::kLeitnerBoxes);
    queue.advanceCram(QStringLiteral("c1"), Rating::Again, &box);
    QCOMPARE(box, 1);

    // Switching to cram also drops session-only review repeats.
    StudyQueue switched;
    switched.buildReview({QStringLiteral("a")});
    switched.repeatAfter(QStringLiteral("a"), 5000);
    QCOMPARE(switched.pendingRepeatCount(), 1);
    switched.buildCram(cards);
    QCOMPARE(switched.pendingRepeatCount(), 0);
    QVERIFY(switched.pendingRepeats().isEmpty());
    QCOMPARE(switched.next(0, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("c1"));
}

void TestStudyQueue::studyQueueCramOrderingByNextCounter() {
    Card first;
    first.id = QStringLiteral("c1");
    Card second;
    second.id = QStringLiteral("c2");
    const QVector<Card> cards{first, second};

    StudyQueue queue;
    queue.buildCram(cards);
    QString id;
    QCOMPARE(queue.next(0, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    const QString served = id;
    QVERIFY(served == QStringLiteral("c1") || served == QStringLiteral("c2"));

    queue.advanceCram(served, Rating::Good, nullptr);
    QCOMPARE(queue.next(0, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QVERIFY(id != served);
    QCOMPARE(id, served == QStringLiteral("c1") ? QStringLiteral("c2") : QStringLiteral("c1"));
}

void TestStudyQueue::studyQueueRemovePendingAndUnknown() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a")});
    queue.remove(QStringLiteral("unknown"));
    QString id;
    QCOMPARE(queue.next(0, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("a"));
    QCOMPARE(queue.next(0, &id, nullptr, nullptr), StudyQueue::Next::Finished);

    StudyQueue pending;
    pending.buildReview({QStringLiteral("a")}, {}, {{QStringLiteral("a"), 5000}});
    QCOMPARE(pending.pendingRepeatCount(), 1);
    pending.remove(QStringLiteral("a"));
    QCOMPARE(pending.pendingRepeatCount(), 0);
    QVERIFY(pending.empty());
    QCOMPARE(pending.nextWait(0), qint64(0));
    QCOMPARE(pending.next(0, &id, nullptr, nullptr), StudyQueue::Next::Finished);

    StudyQueue repeated;
    repeated.buildReview({QStringLiteral("x")});
    repeated.repeatAfter(QStringLiteral("x"), 5000);
    repeated.remove(QStringLiteral("x"));
    QCOMPARE(repeated.pendingRepeatCount(), 0);
}

void TestStudyQueue::studyQueuePendingRepeatsIsCopy() {
    StudyQueue queue;
    queue.buildReview({QStringLiteral("a"), QStringLiteral("b")}, {}, {{QStringLiteral("a"), 5000}});

    QHash<QString, qint64> pending = queue.pendingRepeats();
    pending.insert(QStringLiteral("b"), 10);
    pending.insert(QStringLiteral("a"), 10);
    QCOMPARE(queue.pendingRepeatCount(), 1);
    QCOMPARE(queue.pendingRepeats().value(QStringLiteral("a")), qint64(5000));
    QVERIFY(!queue.pendingRepeats().contains(QStringLiteral("b")));

    QString id;
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Ready);
    QCOMPARE(id, QStringLiteral("b"));
    QCOMPARE(queue.next(1000, &id, nullptr, nullptr), StudyQueue::Next::Waiting);
}
