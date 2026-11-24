#pragma once

#include <QObject>

class TestDeck : public QObject {
    Q_OBJECT

private slots:
    void deckParsesAndSerializes();
    void multilineFieldsRoundTrip();
    void clozeCardsHideAndReveal();
    void deckCardIdIsStableAndIgnoresType();
    void deckContentHashPinsHashBytes();
    void deckLoadPreservesUnknownTypeText();
    void deckLoadAcceptsCaseInsensitiveKeysAndContinuations();
    void deckLoadReportsBrokenCardsAndSkipsThem();
    void deckLoadAllFiltersSortsAndReportsErrors();
    void deckSafeFileNameSanitizes();
    void deckSaveRoundTripsAndRejectsEmptyPath();
    void deckLintReportsDeckIssues();
    void deckLintFileMatchesDirectoryLint();
    void deckLintFlagsMalformedContinuationAsError();
    void deckClozeEdgeCases();
};
