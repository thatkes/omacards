#pragma once

#include <QObject>

class TestStudyPlanner : public QObject {
    Q_OBJECT

private slots:
    void noDecksIsSafe();
    void refreshReadsStatesAndHashes();
    void refreshUsesOneCombinedSchedulingRead();
    void refreshReadFailureCanBeInjectedAtStoreAdapter();
    void refreshBrokenStoreInvalidatesSnapshot();
    void configureLimitsAndCounts();
    void futureReviewsAreNotAvailable();
    void refreshRecomputesNewAllowanceFromReviewHistory();
    void updateNewBudgetTracksRecordedRatings();
    void reviewIdsFollowAvailabilityOrder();
    void cardListFiltersAndFormats();
    void deckSummariesReportCounts();
    void statsSummaryNumericContract();
    void availabilityMemoisesPerDeckNameAndGeneration();
    void stateForKeepsStaleHashGuardWithCachedHashes();
};
