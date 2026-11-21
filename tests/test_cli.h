#pragma once

#include <QObject>

class TestCli : public QObject {
    Q_OBJECT

private slots:
    void helpIsHeadless();
    void addCardsCreatesDeckBulk();
    void addCardsMultilineRoundTrips();
    void addCardsKeepsExistingDescription();
    void addCardsRejectsInvalidInputWithoutWriting();
    void writeCommandsValidateSerializedDeck();
    void addDeckDescriptionAndJsonImport();
    void cliConfigReportsWriteFailure();
    void cliEditsAndDeletesCardsAcrossDecks();
    void previewRendersMathAsText();
    void checkMathReportsUnsupportedCommands();
    void checkReportsLineNumberedIssues();
    void headlessCommandDetectionIsComplete();
    void unknownCommandJsonError();
    void checkJsonKeyContract();
    void checkDeckScoping();
    void readCommandJsonContracts();
    void reviewCommandContract();
    void reviewWrapsStoreErrors();
    void cramCommandContract();
    void addCardJsonContracts();
    void cardsJsonPreservesUnknownFileType();
    void addCardsInputValidationErrors();
    void editDeleteErrorContracts();
    void importJsonAndCsvContracts();
    void previewJsonContracts();
    void configValidationAndPersistence();
    void flagEqualsFormsMatchSeparateArgs();
};
