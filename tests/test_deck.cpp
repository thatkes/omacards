#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "test_deck.h"
#include "test_support.h"
#include "deck.h"
#include "domain.h"

using namespace TestSupport;

namespace {
bool sameIssues(const QVector<DeckIssue> &a, const QVector<DeckIssue> &b) {
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a.at(i).path != b.at(i).path || a.at(i).card != b.at(i).card || a.at(i).line != b.at(i).line ||
            a.at(i).severity != b.at(i).severity || a.at(i).message != b.at(i).message || a.at(i).hint != b.at(i).hint)
            return false;
    }
    return true;
}
} // namespace

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

void TestDeck::multilineFieldsRoundTrip() {
    QTemporaryDir dir;
    const QString decksDir = dir.filePath("decks");
    QVERIFY(QDir().mkpath(decksDir));
    const QString path = QDir(decksDir).filePath("Multi.txt");

    Deck deck;
    deck.name = "Multi";
    deck.path = path;

    Card worked;
    worked.deck = deck.name;
    worked.question = "First line\nSecond line";
    worked.answer = "Step 1\nStep 2\n\nStep 4";
    worked.type = Domain::parseCardType("basic");
    deck.cards << worked;

    Card windows;
    windows.deck = deck.name;
    windows.question = "Windows\r\nline";
    windows.answer = "ok";
    windows.type = Domain::parseCardType("basic");
    deck.cards << windows;

    QString error;
    QVERIFY(DeckIO::save(deck, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString written = QString::fromUtf8(file.readAll());
    file.close();
    QVERIFY(written.contains(QStringLiteral("Q: First line\n  Second line")));
    QVERIFY(written.contains(QStringLiteral("A: Step 1\n  Step 2\n  \n  Step 4")));
    QVERIFY(written.contains(QStringLiteral("Q: Windows\n  line")));
    QVERIFY(!written.contains('\r'));

    // The written file must be clean according to the same linter used by check.
    QVERIFY2(DeckIO::lint(decksDir).isEmpty(), "serialized continuations should be lint-clean");

    error.clear();
    const Deck loaded = DeckIO::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.cards.size(), 2);
    QCOMPARE(loaded.cards.at(0).question, worked.question);
    QCOMPARE(loaded.cards.at(0).answer, worked.answer);
    QCOMPARE(loaded.cards.at(1).question, QStringLiteral("Windows\nline"));
    QCOMPARE(loaded.cards.at(1).answer, windows.answer);
}

void TestDeck::clozeCardsHideAndReveal() {
    Card card;
    card.type = Domain::parseCardType("cloze");
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
    basic.type = Domain::parseCardType("basic");

    Card cloze = basic;
    cloze.type = Domain::parseCardType("cloze");
    Card unknown = basic;
    unknown.type = Domain::parseCardType("future");
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
    // make every existing card look edited and reset its Scheduling state.
    Card card;
    card.question = "Q";
    card.answer = "A";

    card.type = Domain::parseCardType("basic");
    QCOMPARE(DeckIO::contentHash(card), QString("8cad942f67dcac72"));
    // A card built directly can still carry empty boundary text; file parsing
    // canonicalizes an omitted type to "basic" before it reaches the card.
    card.type = {CardType::Basic, QString()};
    QCOMPARE(DeckIO::contentHash(card), QString("0f6bb18bba5bb6f0"));
    card.type = Domain::parseCardType("cloze");
    QCOMPARE(DeckIO::contentHash(card), QString("145a501a9cf1dfe5"));
    card.type = Domain::parseCardType("future"); // unknown file types are preserved verbatim
    QCOMPARE(DeckIO::contentHash(card), QString("25036129cb0549b6"));
    card.type = Domain::parseCardType("CLOZE"); // hashing is byte-exact, not case-folded
    QCOMPARE(DeckIO::contentHash(card), QString("965e4671d78fa6bd"));
    QCOMPARE(DeckIO::contentHash(card).size(), 16);
}

void TestDeck::deckLoadPreservesUnknownTypeText() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Types.txt");
    QVERIFY(writeFile(path, "# Deck: Types\n"
                            "---\n"
                            "Q: The {{c1::cat}} sleeps.\n"
                            "A: cat\n"
                            "type: CLOZE\n"
                            "---\n"
                            "Q: Unknown\n"
                            "A: answer\n"
                            "type: FUTURE\n"));

    QString error;
    const Deck deck = DeckIO::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(deck.cards.size(), 2);

    const Card &cloze = deck.cards.first();
    QCOMPARE(Domain::toString(cloze.type), QString("cloze"));
    QVERIFY(DeckIO::displayQuestion(cloze).contains('['));
    QCOMPARE(DeckIO::displayAnswer(cloze), QString("The cat sleeps."));

    const Card &unknown = deck.cards.at(1);
    QCOMPARE(Domain::toString(unknown.type), QString("future"));
    QCOMPARE(DeckIO::displayQuestion(unknown), QString("Unknown"));
    QCOMPARE(DeckIO::displayAnswer(unknown), QString("answer"));
    Card basic = unknown;
    basic.type = Domain::parseCardType("basic");
    QCOMPARE(DeckIO::cardId(unknown), DeckIO::cardId(basic));

    const QString text = DeckIO::serialize(deck);
    QVERIFY(!text.contains("type: basic"));
    QVERIFY(text.contains("type: cloze"));
    QVERIFY(text.contains("type: future"));

    const QString roundTripPath = dir.filePath("RoundTrip.txt");
    QVERIFY(writeFile(roundTripPath, text.toUtf8()));
    error.clear();
    const Deck roundTrip = DeckIO::load(roundTripPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(roundTrip.cards.size(), 2);
    QCOMPARE(Domain::toString(roundTrip.cards.first().type), QString("cloze"));
    QCOMPARE(Domain::toString(roundTrip.cards.at(1).type), QString("future"));
    QCOMPARE(roundTrip.cards.at(1).id, unknown.id);
    QCOMPARE(DeckIO::contentHash(roundTrip.cards.at(1)), DeckIO::contentHash(unknown));
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
    QCOMPARE(Domain::toString(first.type), QString("cloze")); // type values are lowercased
    QCOMPARE(first.id, QString("pinned"));
    QVERIFY(first.explicitId);

    const Card &second = deck.cards.at(1);
    QCOMPARE(Domain::toString(second.type), QString("basic"));
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
    card.type = Domain::parseCardType("cloze");
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
    QCOMPARE(Domain::toString(loaded.cards.first().type), QString("cloze"));
    QCOMPARE(loaded.cards.first().tags, QStringList({"one", "two"}));
    QCOMPARE(loaded.cards.first().id, QString("pinned-id"));
    QVERIFY(loaded.cards.first().explicitId);

    Deck pathless;
    error.clear();
    QVERIFY(!DeckIO::save(pathless, &error));
    QVERIFY(error.contains(QStringLiteral("has no file path")));
}

void TestDeck::deckLintReportsDeckIssues() {
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

void TestDeck::deckLintWarnsWhenClozeHasNoMarker() {
    QTemporaryDir dir;
    const QString cleanPath = dir.filePath("Marked.txt");
    const QString warningPath = dir.filePath("Unmarked.txt");
    QVERIFY(writeFile(cleanPath, "# Deck: Marked\n---\nQ: A {{c1::cat}} naps.\nA:\ntype: cloze\n"));
    QVERIFY(writeFile(warningPath, "# Deck: Unmarked\n---\nQ: Plain question\nA:\ntype: cloze\n"));

    QVERIFY2(DeckIO::lintFile(cleanPath).isEmpty(), "a marked Cloze card must stay lint-clean");

    const QVector<DeckIssue> issues = DeckIO::lintFile(warningPath);
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues.first().path, warningPath);
    QCOMPARE(issues.first().line, 3);
    QCOMPARE(issues.first().severity, QStringLiteral("warning"));
    QVERIFY(issues.first().message.contains(QStringLiteral("marker")));
    QVERIFY(issues.first().hint.contains(QStringLiteral("{{c1::answer}}")));

    // A Warning must not stop the Deck from loading or writing.
    QString error;
    const Deck deck = DeckIO::load(warningPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(deck.cards.size(), 1);
    QCOMPARE(Domain::toString(deck.cards.first().type), QStringLiteral("cloze"));
    QVERIFY(sameIssues(DeckIO::lintBytes(warningPath, "# Deck: Unmarked\n---\nQ: Plain question\nA:\ntype: cloze\n"),
                       issues));
}

void TestDeck::deckLintFileMatchesDirectoryLint() {
    QTemporaryDir dir;
    const QString cleanPath = dir.filePath("Clean.txt");
    const QString brokenPath = dir.filePath("Broken.txt");
    QVERIFY(writeFile(cleanPath, "# Deck: Clean\n---\nQ: Fine\nA: ok\n"));
    QVERIFY(writeFile(brokenPath, "# Deck: Broken\n---\nA: orphan\n"));

    QVERIFY(DeckIO::lintFile(cleanPath).isEmpty());

    const QVector<DeckIssue> single = DeckIO::lintFile(brokenPath);
    QCOMPARE(single.size(), 1);
    QCOMPARE(single.first().path, brokenPath);
    QCOMPARE(single.first().severity, QStringLiteral("error"));
    QVERIFY(single.first().message.contains(QStringLiteral("Card without a question")));
    QVERIFY(single.first().hint.contains(QStringLiteral("Fix the format")));

    const QVector<DeckIssue> directory = DeckIO::lint(dir.path());
    QCOMPARE(directory.size(), single.size());
    QCOMPARE(directory.first().path, single.first().path);
    QCOMPARE(directory.first().severity, single.first().severity);
    QCOMPARE(directory.first().message, single.first().message);
    QCOMPARE(directory.first().hint, single.first().hint);
}

void TestDeck::deckLintFlagsMalformedContinuationAsError() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Hidden.txt");
    // One leading space is not a continuation, so load() rejects the line and
    // loadAll() drops the whole Deck. Lint has to treat that as an Error.
    QVERIFY(writeFile(path, "# Deck: Hidden\n"
                            "---\n"
                            "Q: First line\n"
                            " second line\n"
                            "A: answer\n"));

    const QVector<DeckIssue> issues = DeckIO::lintFile(path);
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues.first().path, path);
    QCOMPARE(issues.first().line, 4);
    QCOMPARE(issues.first().severity, QStringLiteral("error"));
    QCOMPARE(issues.first().message, QStringLiteral("Continuation line has one leading space"));
    QCOMPARE(issues.first().hint, QStringLiteral("Start multiline Q: or A: with exactly two spaces."));

    const QVector<DeckIssue> directory = DeckIO::lint(dir.path());
    QCOMPARE(directory.size(), issues.size());
    QCOMPARE(directory.first().severity, issues.first().severity);
    QCOMPARE(directory.first().message, issues.first().message);

    QString error;
    QVERIFY(DeckIO::loadAll(dir.path(), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("Unexpected line")));

    // A one-space comment is still a comment to load(), so it stays clean.
    const QString commented = dir.filePath("Commented.txt");
    QVERIFY(writeFile(commented, "# Deck: Commented\n---\nQ: First\n # note\nA: ok\n"));
    QVERIFY2(DeckIO::lintFile(commented).isEmpty(), "one-space comment must stay lint-clean");
    QString commentError;
    QVERIFY(!DeckIO::load(commented, &commentError).cards.isEmpty());
    QVERIFY2(commentError.isEmpty(), qPrintable(commentError));
}

void TestDeck::deckLintBytesMatchesLintFileWithoutReading() {
    QTemporaryDir dir;
    const QString path = dir.filePath("Bytes.txt");
    const QByteArray bytes = "# Deck: Bytes\n"
                             "---\n"
                             "Q: Same\n"
                             "A: one\n"
                             "---\n"
                             "Q: Same\n"
                             "A: \\(\\foo{x}\\)\n"
                             "oops: bad\n"
                             "   too much indent\n";
    QVERIFY(writeFile(path, bytes));
    const QVector<DeckIssue> fromFile = DeckIO::lintFile(path);
    QVERIFY(!fromFile.isEmpty());
    QVERIFY(sameIssues(DeckIO::lintBytes(path, bytes), fromFile));

    // lintBytes() uses only the bytes it was given: it produces the same
    // issues after the file has gone away.
    QVERIFY(QFile::remove(path));
    QVERIFY(sameIssues(DeckIO::lintBytes(path, bytes), fromFile));
}

void TestDeck::deckSaveReportsCommittedBytes() {
    QTemporaryDir dir;
    Deck deck;
    deck.name = QStringLiteral("Bytes");
    deck.path = dir.filePath(QStringLiteral("Bytes.txt"));
    deck.description = QStringLiteral("Desc");
    Card card;
    card.deck = deck.name;
    card.question = QStringLiteral("First line\nSecond line");
    card.answer = QStringLiteral("Answer");
    card.type = Domain::parseCardType(QStringLiteral("basic"));
    card.id = DeckIO::cardId(card);
    deck.cards << card;

    QByteArray written;
    QString error;
    QVERIFY(DeckIO::save(deck, &written, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QFile file(deck.path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), written);
    QVERIFY(written.contains("  Second line"));
}

void TestDeck::deckClozeEdgeCases() {
    Card card;
    card.type = Domain::parseCardType("cloze");

    card.question = "The {{c1::cat}} and {{c2::dog}} sleep.";
    const QString hidden = DeckIO::displayQuestion(card);
    const QString dots(3, QChar(0x00B7));
    QCOMPARE(hidden, QString("The [ %1 ] and [ %2 ] sleep.").arg(dots, dots));
    QCOMPARE(DeckIO::displayAnswer(card), QString("The cat and dog sleep."));

    // Hand-written hints are stripped like importer hints: the reveal shows
    // only the answer and the blank is sized to the answer, not the hint.
    card.question = "A {{c2::cat::animal}} naps.";
    card.answer.clear();                      // Cloze answers are derived from Q, not A.
    const QString hintDots(3, QChar(0x00B7)); // length of "cat"
    QCOMPARE(DeckIO::displayQuestion(card), QString("A [ %1 ] naps.").arg(hintDots));
    QCOMPARE(DeckIO::displayAnswer(card), QString("A cat naps."));

    card.question = "A {{c1::cat naps.";
    QCOMPARE(DeckIO::displayQuestion(card), card.question);
    QCOMPARE(DeckIO::displayAnswer(card), card.question);

    // Basic cards do not interpret cloze markers at all.
    card.type = Domain::parseCardType("basic");
    card.question = "The {{c1::cat}} sleeps.";
    QCOMPARE(DeckIO::displayQuestion(card), card.question);
    QCOMPARE(DeckIO::displayAnswer(card), card.answer);
}
