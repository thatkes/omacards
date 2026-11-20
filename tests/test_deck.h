#pragma once

#include <QObject>

class TestDeck : public QObject {
    Q_OBJECT

private slots:
    void deckParsesAndSerializes();
    void clozeCardsHideAndReveal();
    void deckCardIdIsStableAndIgnoresType();
    void deckContentHashPinsHashBytes();
    void deckLoadAcceptsCaseInsensitiveKeysAndContinuations();
    void deckLoadReportsBrokenCardsAndSkipsThem();
    void deckLoadAllFiltersSortsAndReportsErrors();
    void deckSafeFileNameSanitizes();
    void deckSaveRoundTripsAndRejectsEmptyPath();
    void deckLintReportsIssues();
    void deckClozeEdgeCases();
};
