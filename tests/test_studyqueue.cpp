#include <QtTest>

#include "test_studyqueue.h"
#include "test_support.h"
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
