#pragma once

#include <QObject>

class TestFsrs : public QObject {
    Q_OBJECT

private slots:
    void fsrsInitialState();
    void fsrsReviewMovesDueForward();
    void fsrsRetentionShapesIntervals();
    void fsrsInitialGradesClampAndPinWeights();
    void fsrsIntervalClampsRetentionAndMaximum();
    void fsrsRetrievabilityHandlesMissingAndFutureReviews();
    void fsrsReviewClampsRatingAndRetention();
};
