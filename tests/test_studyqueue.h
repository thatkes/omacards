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
};
