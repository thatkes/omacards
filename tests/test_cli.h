#pragma once

#include <QObject>

class TestCli : public QObject {
    Q_OBJECT

private slots:
    void helpIsHeadless();
    void missingSkillNudge();
    void skillStatusReportsInstallState();
    void addCardsCreatesDeckBulk();
    void addCardsJsonMetadataContract();
    void addCardsPathKeepsRelativeDecksDir();
    void addCardsJsonCountsOnlyWrittenDeck();
    void addCardsCountsOnlyWrittenDeckWhenReloadSkipsIt();
    void addCardsIgnoresUnreadableOtherDeck();
    void addCardsMultilineRoundTrips();
    void addCardsKeepsExistingDescription();
    void addCardsRejectsInvalidInputWithoutWriting();
    void writeCommandsValidateSerializedDeck();
    void addDeckDescriptionAndJsonImport();
    void cliConfigReportsWriteFailure();
    void cliEditsAndDeletesCardsAcrossDecks();
    void previewRendersMathAsText();
    void checkMathReportsUnsupportedCommands();
    void checkReportsLineNumberedDeckIssues();
    void headlessCommandDetectionIsComplete();
    void unknownCommandJsonError();
    void checkJsonKeyContract();
    void checkDeckScoping();
    void readCommandJsonContracts();
    void useDeckCommandContract();
    void decksCurrentCommandContract();
    void reviewCommandContract();
    void reviewWrapsStoreErrors();
    void reviewWrapsWriteStoreErrors();
    void cramCommandContract();
    void addCardJsonContracts();
    void cardsJsonPreservesUnknownFileType();
    void addCardsInputValidationErrors();
    void editDeleteErrorContracts();
    void importJsonAndCsvContracts();
    void previewJsonContracts();
    void previewZeroCardDeckIssues();
    void configValidationAndPersistence();
    void flagEqualsFormsMatchSeparateArgs();
    void warmReportsRenderedAndCached();
    void warmRejectsUnknownAndEmptyDecks();
};
