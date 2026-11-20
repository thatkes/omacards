#include <QtTest>
#include <QTemporaryDir>

#include "test_deck.h"
#include "test_support.h"
#include "deck.h"

using namespace TestSupport;

void TestDeck::deckParsesAndSerializes() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Spanish.txt");
    const QString text = QStringLiteral("# Deck: Spanish\n"
                                        "# Description: Words and phrases\n"
                                        "---\n"
                                        "Q: What is \"gato\"?\n"
                                        "A: cat\n"
                                        "tags: vocab, animals\n"
                                        "---\n"
                                        "Q: Solve \\(x^2+1=0\\)\n"
                                        "A: \\(x=\\pm i\\)\n"
                                        "id: fixed-id\n");
    QVERIFY(writeFile(path, text.toUtf8()));

    QString error;
    Deck deck = DeckIO::load(path, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(deck.name, QString("Spanish"));
    QCOMPARE(deck.description, QString("Words and phrases"));
    QCOMPARE(deck.cards.size(), 2);
    QCOMPARE(deck.cards.first().question, QString("What is \"gato\"?"));
    QCOMPARE(deck.cards.first().tags, QStringList({"vocab", "animals"}));
    QCOMPARE(deck.cards.at(1).id, QString("fixed-id"));
    QVERIFY(!deck.cards.first().id.isEmpty());

    const QString roundTrip = DeckIO::serialize(deck);
    QVERIFY(roundTrip.contains("# Deck: Spanish"));
    QVERIFY(roundTrip.contains("Q: What is \"gato\"?"));
    QVERIFY(roundTrip.contains("tags: vocab, animals"));
    QVERIFY(roundTrip.contains("id: fixed-id"));

    // Generated ids stay implicit so edited files remain readable.
    QVERIFY(!roundTrip.contains("id: " + deck.cards.first().id));
}

void TestDeck::clozeCardsHideAndReveal() {
    Card card;
    card.type = "cloze";
    card.question = "The {{c1::cat}} sleeps.";
    QVERIFY(DeckIO::displayQuestion(card).contains('['));
    QVERIFY(!DeckIO::displayQuestion(card).contains("cat"));
    QCOMPARE(DeckIO::displayAnswer(card), QString("The cat sleeps."));
}
