#pragma once

#include <QObject>

class TestStudyQueue : public QObject {
    Q_OBJECT

private slots:
    void studyQueueReviewOrder();
    void studyQueueRepeatWaitsThenReturns();
    void studyQueueRebuildKeepsPending();
    void studyQueueCramBoxes();
    void studyQueueRemove();
    void studyQueueNextWaitEdges();
    void studyQueueNextWaitUsesSmallestPending();
    void studyQueueNextReturnsPostTakeWait();
    void studyQueueBuildReviewPendingAlreadyDue();
    void studyQueueRepeatAfterOverwritesDeadline();
    void studyQueueClearEmptiesEverything();
    void studyQueueCramEdges();
    void studyQueueCramOrderingByNextCounter();
    void studyQueueRemovePendingAndUnknown();
    void studyQueuePendingRepeatsIsCopy();
};
