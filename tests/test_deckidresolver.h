#pragma once

#include <QObject>

class TestDeckIdResolver : public QObject {
    Q_OBJECT

private slots:
    void exactDeckNameWins();
    void fileNameFallbackReturnsWrittenName();
    void deckFileExtensionFallback();
    void textExtensionIsCheckedBeforeDeckExtension();
    void emptyRequestReturnsNoId();
    void unknownRequestReturnsStructuredReason();
};
