#pragma once

#include <QObject>

class TestFsrs : public QObject {
    Q_OBJECT

private slots:
    void fsrsInitialSchedulingState();
    void fsrsReviewMovesDueForward();
    void fsrsRetentionShapesIntervals();
    void fsrsInitialInputsClampAndPinWeights();
    void fsrsIntervalClampsRetentionAndMaximum();
    void fsrsRetrievabilityHandlesMissingAndFutureReviews();
    void fsrsReviewClampsRatingAndRetention();
};
