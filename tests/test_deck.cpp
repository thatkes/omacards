#include <QtTest>
#include <QDir>
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

void TestDeck::deckCardIdIsStableAndIgnoresType() {
    Card basic;
    basic.deck = "Deck";
    basic.question = "Q";
    basic.answer = "A";
    basic.type = "basic";

    Card cloze = basic;
    cloze.type = "cloze";
    Card unknown = basic;
    unknown.type = "future";
    Card otherDeck = basic;
    otherDeck.deck = "Other";
    Card otherAnswer = basic;
    otherAnswer.answer = "B";

    // Identity is deck + question + answer only; type and content hash are not
    // part of it, so changing a card's type keeps its scheduling state.
    QCOMPARE(DeckIO::cardId(basic), QString("fdafe1523b72"));
    QCOMPARE(DeckIO::cardId(cloze), DeckIO::cardId(basic));
    QCOMPARE(DeckIO::cardId(unknown), DeckIO::cardId(basic));
    QVERIFY(!DeckIO::cardId(otherDeck).isEmpty());
    QVERIFY(DeckIO::cardId(otherDeck) != DeckIO::cardId(basic));
    QVERIFY(DeckIO::cardId(otherAnswer) != DeckIO::cardId(basic));
    QCOMPARE(DeckIO::cardId(basic).size(), 12);

    // contentHash does the opposite: it includes the exact type text so a
    // type edit invalidates the sidecar state, and ignores the deck name.
    QVERIFY(DeckIO::contentHash(cloze) != DeckIO::contentHash(basic));
    QCOMPARE(DeckIO::contentHash(otherDeck), DeckIO::contentHash(basic));
    QVERIFY(DeckIO::contentHash(otherAnswer) != DeckIO::contentHash(basic));
}

void TestDeck::deckContentHashPinsHashBytes() {
    // Pin the SHA-256 prefix so a change to the hash formula cannot silently
    // make every existing card look edited and reset its FSRS progress.
    Card card;
    card.question = "Q";
    card.answer = "A";

    card.type = "basic";
    QCOMPARE(DeckIO::contentHash(card), QString("8cad942f67dcac72"));
    card.type.clear(); // omitted type in a file is the canonical "basic"
    QCOMPARE(DeckIO::contentHash(card), QString("0f6bb18bba5bb6f0"));
    card.type = "cloze";
    QCOMPARE(DeckIO::contentHash(card), QString("145a501a9cf1dfe5"));
    card.type = "future"; // unknown file types are preserved verbatim
    QCOMPARE(DeckIO::contentHash(card), QString("25036129cb0549b6"));
    card.type = "CLOZE"; // hashing is byte-exact, not case-folded
    QCOMPARE(DeckIO::contentHash(card), QString("965e4671d78fa6bd"));
    QCOMPARE(DeckIO::contentHash(card).size(), 16);
}

void TestDeck::deckLoadAcceptsCaseInsensitiveKeysAndContinuations() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Mixed.txt");
    QVERIFY(writeFile(path, "# deck: Mixed\n"
                            "# description: Desc\n"
                            "---\n"
                            "q: First\n"
                            "  continued question\n"
                            "A: answer\n"
                            "  continued answer\n"
                            "ANSWER: alias\n"
                            "TAGS: one, two,\n"
                            "  three\n"
                            "TYPE: CLOZE\n"
                            "ID: pinned\n"
                            "---\n"
                            "---\n"
                            "Q: The {{c1::cat}} sleeps.\n"));

    QString error;
    const Deck deck = DeckIO::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(deck.name, QString("Mixed"));
    QCOMPARE(deck.description, QString("Desc"));
    QCOMPARE(deck.cards.size(), 2); // consecutive separators do not create a card

    const Card &first = deck.cards.first();
    QCOMPARE(first.question, QString("First\ncontinued question"));
    QCOMPARE(first.answer, QString("answer\ncontinued answer\nalias"));
    QCOMPARE(first.tags, QStringList({"one", "two", "three"}));
    QCOMPARE(first.type, QString("cloze")); // type values are lowercased
    QCOMPARE(first.id, QString("pinned"));
    QVERIFY(first.explicitId);

    const Card &second = deck.cards.at(1);
    QCOMPARE(second.type, QString("basic"));
    QVERIFY(!second.explicitId);
    QCOMPARE(second.question, QString("The {{c1::cat}} sleeps."));
}

void TestDeck::deckLoadReportsBrokenCardsAndSkipsThem() {
    QTemporaryDir dir;
    const QString noQuestion = dir.filePath("NoQuestion.txt");
    QVERIFY(writeFile(noQuestion, "---\nA: answer only\n"));
    QString error;
    const Deck empty = DeckIO::load(noQuestion, &error);
    QVERIFY(empty.cards.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Card without a question")));

    const QString unexpected = dir.filePath("Unexpected.txt");
    QVERIFY(writeFile(unexpected, "---\nQ: valid\nA: ok\nnot-a-key\n"));
    error.clear();
    const Deck partial = DeckIO::load(unexpected, &error);
    QCOMPARE(partial.cards.size(), 1);
    QCOMPARE(partial.cards.first().question, QString("valid"));
    QVERIFY(error.contains(QStringLiteral("Unexpected line 3 in")));

    error.clear();
    const Deck missing = DeckIO::load(dir.filePath("nope.txt"), &error);
    QVERIFY(missing.cards.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Cannot read deck file")));
}

void TestDeck::deckLoadAllFiltersSortsAndReportsErrors() {
    QTemporaryDir dir;
    const QString decksDir = dir.filePath("decks");
    QVERIFY(QDir().mkpath(decksDir));
    QVERIFY(writeFile(decksDir + "/b.txt", "---\nQ: B\nA: b\n"));
    QVERIFY(writeFile(decksDir + "/a.deck", "# Deck: Named A\n---\nQ: A\nA: a\n"));
    QVERIFY(writeFile(decksDir + "/c.md", "---\nQ: ignored\nA: x\n"));
    QVERIFY(writeFile(decksDir + "/d.csv", "---\nQ: ignored\nA: x\n"));

    QString error;
    const QVector<Deck> decks = DeckIO::loadAll(decksDir, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(decks.size(), 2);
    QCOMPARE(decks.at(0).name, QString("Named A")); // # Deck: overrides the filename
    QCOMPARE(decks.at(1).name, QString("b"));

    error.clear();
    QVERIFY(DeckIO::loadAll(dir.filePath("missing-dir"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("Deck directory does not exist")));

    // A file with a parse error is skipped, but the remaining decks still load
    // and the error is reported once to the caller.
    QVERIFY(writeFile(decksDir + "/zz_bad.txt", "---\nQ: ok\nA: ok\nbroken line\n"));
    error.clear();
    const QVector<Deck> withBad = DeckIO::loadAll(decksDir, &error);
    QCOMPARE(withBad.size(), 2);
    QVERIFY(error.contains(QStringLiteral("Unexpected line")));
}

void TestDeck::deckSafeFileNameSanitizes() {
    QCOMPARE(DeckIO::safeFileName("  A/B  "), QString("A-B"));
    QCOMPARE(DeckIO::safeFileName("A/B"), QString("A-B"));
    QCOMPARE(DeckIO::safeFileName("---trim---"), QString("trim"));
    QCOMPARE(DeckIO::safeFileName("a  b!!c"), QString("a-b-c"));
    QCOMPARE(DeckIO::safeFileName("a.b-c"), QString("a.b-c"));
    QCOMPARE(DeckIO::safeFileName("  ///  "), QString("Deck"));
    QCOMPARE(DeckIO::safeFileName(""), QString("Deck"));
}

void TestDeck::deckSaveRoundTripsAndRejectsEmptyPath() {
    QTemporaryDir dir;
    Deck deck;
    deck.name = "Round";
    deck.description = "Saved deck";
    deck.path = dir.filePath("Round.txt");
    Card card;
    card.deck = deck.name;
    card.question = "Q?";
    card.answer = "A";
    card.type = "cloze";
    card.tags = {"one", "two"};
    card.id = "pinned-id";
    card.explicitId = true;
    deck.cards << card;

    QString error;
    QVERIFY(DeckIO::save(deck, &error));
    QVERIFY(error.isEmpty());

    const Deck loaded = DeckIO::load(deck.path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.name, QString("Round"));
    QCOMPARE(loaded.description, QString("Saved deck"));
    QCOMPARE(loaded.cards.size(), 1);
    QCOMPARE(loaded.cards.first().type, QString("cloze"));
    QCOMPARE(loaded.cards.first().tags, QStringList({"one", "two"}));
    QCOMPARE(loaded.cards.first().id, QString("pinned-id"));
    QVERIFY(loaded.cards.first().explicitId);

    Deck pathless;
    error.clear();
    QVERIFY(!DeckIO::save(pathless, &error));
    QVERIFY(error.contains(QStringLiteral("has no file path")));
}

void TestDeck::deckLintReportsIssues() {
    QTemporaryDir dir;
    QVERIFY(DeckIO::lint(dir.filePath("nonexistent")).isEmpty());

    const QString path = dir.filePath("Lint.txt");
    QVERIFY(writeFile(path, "# Deck: Lint\n"
                            "---\n"
                            "Q: Same\n"
                            "A: one\n"
                            "---\n"
                            "Q: Same\n"
                            "A: \\(\\foo{x}\\)\n"
                            "oops: bad\n"
                            "   too much indent\n"));

    const QVector<DeckIssue> issues = DeckIO::lint(dir.path());
    int duplicates = 0, unexpected = 0, indent = 0, math = 0;
    for (const DeckIssue &issue : issues) {
        QCOMPARE(issue.path, path);
        QVERIFY(!issue.hint.isEmpty());
        if (issue.message == QStringLiteral("Duplicate question in deck")) {
            ++duplicates;
            QCOMPARE(issue.line, 6);
            QVERIFY(issue.hint.contains(QStringLiteral("line 3")));
        } else if (issue.message == QStringLiteral("Unexpected line in deck file")) {
            ++unexpected;
            QCOMPARE(issue.line, 8);
        } else if (issue.message == QStringLiteral("Continuation line has more than two leading spaces")) {
            ++indent;
            QCOMPARE(issue.line, 9);
        } else if (issue.message.startsWith(QStringLiteral("Math warning in answer:"))) {
            ++math;
            QCOMPARE(issue.line, 6);
            QVERIFY(issue.message.contains(QStringLiteral("Unsupported math command: \\foo")));
        }
    }
    QCOMPARE(duplicates, 1);
    QCOMPARE(unexpected, 1);
    QCOMPARE(indent, 1);
    QCOMPARE(math, 1);
    // Warnings are the default severity; load-level errors are "error".
    QCOMPARE(issues.first().severity, QString("warning"));
}

void TestDeck::deckClozeEdgeCases() {
    Card card;
    card.type = "cloze";

    card.question = "The {{c1::cat}} and {{c2::dog}} sleep.";
    const QString hidden = DeckIO::displayQuestion(card);
    const QString dots(3, QChar(0x00B7));
    QCOMPARE(hidden, QString("The [ %1 ] and [ %2 ] sleep.").arg(dots, dots));
    QCOMPARE(DeckIO::displayAnswer(card), QString("The cat and dog sleep."));

    // Hints written by hand in a deck file are not stripped by DeckIO; the
    // Anki importer is the layer that removes them.
    card.question = "A {{c1::cat::animal}} naps.";
    const QString hintDots(11, QChar(0x00B7)); // length of "cat::animal"
    QCOMPARE(DeckIO::displayQuestion(card), QString("A [ %1 ] naps.").arg(hintDots));
    QCOMPARE(DeckIO::displayAnswer(card), QString("A cat::animal naps."));

    card.question = "A {{c1::cat naps.";
    QCOMPARE(DeckIO::displayQuestion(card), card.question);
    QCOMPARE(DeckIO::displayAnswer(card), card.question);

    // Basic cards do not interpret cloze markers at all.
    card.type = "basic";
    card.question = "The {{c1::cat}} sleeps.";
    QCOMPARE(DeckIO::displayQuestion(card), card.question);
    QCOMPARE(DeckIO::displayAnswer(card), card.answer);
}
