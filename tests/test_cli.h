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
};
