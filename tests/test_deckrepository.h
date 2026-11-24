#pragma once

#include <QObject>

class TestDeckRepository : public QObject {
    Q_OBJECT

private slots:
    void initializeCreatesWelcomeDeckOnFreshDirectory();
    void initializeDoesNotOverwriteExistingDeckDirectory();
    void createDeckRejectsEmptyAndDuplicateNames();
    void createDeckRejectsSanitizedPathCollision();
    void createDeckRejectsExistingUntrackedFile();
    void createDeckRejectsInvalidSerializedDeck();
    void createDeckTrimsDescription();
    void createCardsRejectsEmptyTargetAndDrafts();
    void createCardsReportsInvalidDraftIndex();
    void createCardsRejectsEmptyAnswers();
    void createCardsKeepsClozeAnswersDerivedFromQuestion();
    void createCardsCreatesDeckWithDescription();
    void createCardsKeepsExistingDescription();
    void createCardsDeduplicatesTagsAndGeneratesIds();
    void createCardsCanonicalizesAndRejectsTypes();
    void createCardRequiresExistingDeckAndSplitsTags();
    void modifyCardRejectsUnknownDeckAndCard();
    void modifyCardRejectsEmptyQuestionAndNormalizesType();
    void modifyCardHandlesExplicitAndGeneratedIds();
    void modifyCardTrimsAndDeduplicatesTags();
    void deleteAndRemoveRejectUnknownNames();
    void removeDeckClearsCurrentAndPrunesOnReload();
    void removeDeckKeepsUnrelatedCurrent();
    void setDeckDescriptionTrimsAndSkipsNoOp();
    void setDeckDescriptionRejectsInvalidSerializedDeck();
    void writeAllowsPreExistingWarnings();
    void cardByIdKeepsFirstDuplicateExplicitId();
    void loadPicksFirstDeckWhenCurrentIsInvalid();
    void loadPrunesStaleSidecarRows();
    void loadKeepsStateWhenDeckFileUnreadable();
    void searchCardsMatchesFieldsAndFiltersDeck();
    void emptyHistoryUndoRedoReportErrors();
    void undoRedoEmitHistoryChanged();
    void watchDirectoryRequestsReloadAfterFileChange();
};
