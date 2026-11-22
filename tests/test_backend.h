#pragma once

#include <QObject>

class TestBackend : public QObject {
    Q_OBJECT

private slots:
    void defaultPathsIgnoreOrganizationName();
    void backendCreatesCardAndRatesInCram();
    void qmlCanRateCards();
    void createCardRejectsInvalidType();
    void backendEditsCardsWithUndo();
    void backendDeletesDeckAndUndoes();
    void backendFiltersCardList();
    void backendImportsAnkiClozeAndMedia();
    void backendAppliesDayStartToDueCards();
    void backendReportsNewCountAndDeckSummaries();
    void qmlCanNavigateLibraryCards();
    void qmlDeckSelectorShowsCurrentDeckAndCounts();
    void qmlDeckCycleKeysWrapAndAreScoped();
    void qmlHelpOverlayNamesDeckSelector();
    void qmlStudySelectorSwitchesDeckFresh();
    void qmlStudyScrollViewClipsAndSizes();
    void qmlStudyScrollFirstKeysAndFlipFallbacks();
    void qmlStudyJKScrollOnlyWithoutOverflow();
    void qmlStudyScrollCuesFollowPosition();
    void qmlStudyRatingKeysKeepTheirMeaning();
    void qmlStudyResetsScrollOnFlipAndNewCard();
    void qmlStudyPointerScrollAndClickFlip();
    void qmlStudyScalesPicturesToWidth();
    void qmlStudyVisualFactsPinLayoutAtMinimumSize();
    void qmlCramLabelUsesCurrentBox();
    void qmlCramBoxDistributionShowsCountsTimingAndHighlight();
    void qmlCramBoxDistributionFollowsRatings();
    void qmlCramBoxDistributionIsCramOnlyAndHidesWhenEmpty();
    void qmlCramBoxHighlightFollowsThemeChange();
    void qmlGoodLabelUsesGoodCount();
    void qmlSettingsRowsFitTheirContent();
    void backendAppliesNewCardLimit();
    void backendRequeuesAgainInSameSession();
    void unreadableDeckKeepsSchedulingState();
    void failedCardWriteDoesNotMutateBackend();
    void unreadableStateDoesNotTreatEveryCardAsNew();
    void deckCountsMatchLibrarySummaries();
    void deckCreationRejectsSanitizedPathCollision();
    void refreshKeepsPendingAgainRepeat();
    void deckSwitchStartsFreshSession();
    void deckSwitchKeepsModeAndResetsCramBox();
    void setMathColorsNotifiesColorChange();
    void statsSummaryKeepsContract();
    void setModeBoundarySemantics();
    void invalidRatingsAreNoOps();
    void settingsClampAndPersist();
    void legacyAllowanceSettingMigratesAndMirrorsWrites();
    void signalContractsForRefreshAndNavigation();
    void modifyCardInvalidTypeNormalizesToBasic();
    void deckSummariesAndSearchFilterByDeck();
    void currentDeckAndRecencyPersistAcrossBackends();
    void selectionUpdatesRecencyOnlyOnExplicitChoice();
    void missingDeckFallbackPersistsAndSelectorAgrees();
    void emptySessionHelpersAndStatsText();
    void setDeckDescriptionAndQmlErrorSignals();
    void qmlStudyShowsMathPlaceholderUntilSvgArrives();
    void qmlStudyRecolorsCachedSvgWhenThemeChanges();
    void qmlStudyUsesCachedSvgWithoutPlaceholder();
};
