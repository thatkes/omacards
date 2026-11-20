#pragma once

#include <QObject>

class TestCli : public QObject {
    Q_OBJECT

private slots:
    void helpIsHeadless();
    void addCardsCreatesDeckBulk();
    void addCardsKeepsExistingDescription();
    void addCardsRejectsInvalidInputWithoutWriting();
    void addDeckDescriptionAndJsonImport();
    void cliConfigReportsWriteFailure();
    void cliEditsAndDeletesCardsAcrossDecks();
    void previewRendersMathAsText();
    void checkMathReportsUnsupportedCommands();
    void checkReportsLineNumberedIssues();
    void headlessCommandDetectionIsComplete();
    void unknownCommandJsonError();
    void checkJsonKeyContract();
    void readCommandJsonContracts();
    void reviewCommandContract();
    void cramCommandDeterminism();
    void addCardJsonContracts();
    void addCardsInputValidationErrors();
    void editDeleteErrorContracts();
    void importJsonAndCsvContracts();
    void previewJsonContracts();
    void configValidationAndPersistence();
    void flagEqualsFormsMatchSeparateArgs();
};
