#pragma once

#include <QObject>

class TestDeck : public QObject {
    Q_OBJECT

private slots:
    void deckParsesAndSerializes();
    void clozeCardsHideAndReveal();
};
