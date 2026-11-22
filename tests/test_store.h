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
    void migratesLegacyNewCardAllowanceKey();
    void summaryStreakBreaksOnMissingDay();
    void summaryStreakSurvivesDayWithoutReviews();
    void summaryCountsOnlyGood();
    void summaryUsesStudyDayForReviewDates();
    void settingFallbackAndUpsert();
};
