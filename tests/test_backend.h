#pragma once

#include <QObject>

class TestBackend : public QObject {
    Q_OBJECT

private slots:
    void defaultPathsIgnoreOrganizationName();
    void backendCreatesCardsAndCramsForever();
    void qmlCanRateCards();
    void createCardRejectsInvalidType();
    void backendEditsCardsWithUndo();
    void backendDeletesDeckAndUndoes();
    void backendFiltersCardList();
    void backendImportsAnkiClozeAndMedia();
    void backendAppliesDayStartToDueCards();
    void backendReportsNewCountAndDeckSummaries();
    void qmlCanNavigateLibraryCards();
    void backendAppliesNewCardLimit();
    void backendRequeuesAgainInSameSession();
    void unreadableDeckKeepsSchedulingState();
    void failedCardWriteDoesNotMutateBackend();
    void unreadableStateDoesNotTreatEveryCardAsNew();
    void deckCountsMatchLibrarySummaries();
    void deckCreationRejectsSanitizedPathCollision();
    void refreshKeepsPendingAgainRepeat();
    void deckSwitchKeepsPendingAgainRepeat();
    void setMathColorsNotifiesColorChange();
    void statsSummaryKeepsContract();
};
