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
    void createDeckTrimsDescription();
    void createCardsRejectsEmptyTargetAndDrafts();
    void createCardsReportsInvalidDraftIndex();
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
    void loadPicksFirstDeckWhenCurrentIsInvalid();
    void loadPrunesStaleSidecarRows();
    void loadKeepsStateWhenDeckFileUnreadable();
    void searchCardsMatchesFieldsAndFiltersDeck();
    void emptyHistoryUndoRedoReportErrors();
    void undoRedoEmitHistoryChanged();
    void watchDirectoryRequestsReloadAfterFileChange();
};
