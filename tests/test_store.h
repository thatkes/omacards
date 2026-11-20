#pragma once

#include <QObject>

class TestStore : public QObject {
    Q_OBJECT

private slots:
    void storeRoundTripsState();
    void storeRemembersSettings();
    void storeCountsNewCardsToday();
    void storeUsesConfiguredDayStart();
    void openInvalidPathSetsError();
    void openTwiceIsIdempotent();
    void stickyErrorsFromFailedQueries();
    void unopenedStoreIsInert();
    void saveUpsertsByCardId();
    void removeExceptKeepsReviewsLog();
    void migratesLegacyReviewsSchema();
    void summaryStreakBreaksOnMissingDay();
    void summaryStreakSurvivesDayWithoutReviews();
    void summaryCountsOnlyGoodAsCorrect();
    void summaryUsesStudyDayForReviewDates();
    void settingFallbackAndUpsert();
};
