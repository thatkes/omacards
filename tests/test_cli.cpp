#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

#include "test_cli.h"
#include "test_support.h"
#include "backend.h"
#include "cli.h"
#include "deck.h"

using namespace TestSupport;

void TestCli::helpIsHeadless() {
    QVERIFY(Cli::needsOffscreen({"--help"}));
    QVERIFY(Cli::needsOffscreen({"-h"}));
    QVERIFY(Cli::needsOffscreen({"help"}));
    QVERIFY(!Cli::needsOffscreen({"open"}));

    if (!QFileInfo::exists(appBinary()))
        QSKIP("Build the app before running CLI integration tests.");
    QProcess process;
    process.start(appBinary(), {"--help"});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(process.readAllStandardOutput().contains("Usage:"));
}

void TestCli::addCardsCreatesDeckBulk() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"({"description":"Bulk math","cards":[
        {"question":"Q1?","answer":"A1","tags":["x","y"]},
        {"question":"Q2?","answer":"A2","type":"cloze","tags":"z, w"}]})"));

    QCOMPARE(Cli::run({"add-cards", "-d", "Bulk", "-i", input, "--json"}), 0);

    QString error;
    const Deck deck = DeckIO::load(QDir(dir.filePath("decks")).filePath("Bulk.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(deck.description, QString("Bulk math"));
    QCOMPARE(deck.cards.size(), 2);
    QCOMPARE(deck.cards.at(0).tags, QStringList({"x", "y"}));
    QCOMPARE(deck.cards.at(1).type, QString("cloze"));
    QCOMPARE(deck.cards.at(1).tags, QStringList({"z", "w"}));
}

void TestCli::addCardsKeepsExistingDescription() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    QCOMPARE(Cli::run({"add-deck", "Keep", "--description", "Original", "--json"}), 0);
    QVERIFY(writeFile(dir.filePath("cards.json"), R"([{"question":"Q","answer":"A"}])"));
    QCOMPARE(Cli::run({"add-cards", "-d", "Keep", "-i", dir.filePath("cards.json"), "--json"}), 0);

    QString error;
    const Deck deck = DeckIO::load(QDir(dir.filePath("decks")).filePath("Keep.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(deck.description, QString("Original"));
    QCOMPARE(deck.cards.size(), 1);
}

void TestCli::addCardsRejectsInvalidInputWithoutWriting() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    const QString input = dir.filePath("bad.json");
    QVERIFY(writeFile(input, R"({"cards":[
        {"question":"ok","answer":"a"},{"answer":"no question"}]})"));

    QCOMPARE(Cli::run({"add-cards", "-d", "Bad", "-i", input, "--json"}), 1);
    QVERIFY(!QFileInfo::exists(QDir(dir.filePath("decks")).filePath("Bad.txt")));
}

void TestCli::addDeckDescriptionAndJsonImport() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    QCOMPARE(Cli::run({"add-deck", "Empty", "--description", "No cards", "--json"}), 0);
    QString error;
    const Deck empty = DeckIO::load(QDir(dir.filePath("decks")).filePath("Empty.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(empty.description, QString("No cards"));

    const QString input = dir.filePath("Imported.json");
    QVERIFY(writeFile(input, R"({"description":"from import","cards":[
        {"question":"j1","answer":"a1"}]})"));
    QCOMPARE(Cli::run({"import", input, "--json"}), 0);
    const Deck imported = DeckIO::load(QDir(dir.filePath("decks")).filePath("Imported.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(imported.description, QString("from import"));
    QCOMPARE(imported.cards.size(), 1);
}

void TestCli::cliConfigReportsWriteFailure() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    EnvGuard env(dir.filePath("decks"), database);
    {
        Backend backend(dir.filePath("decks"), database);
        QString error;
        QVERIFY(backend.initialize(&error));
    }
    QVERIFY(QFile::setPermissions(database, QFile::ReadOwner));
    if (QFileInfo(database).isWritable()) {
        QFile::setPermissions(database, QFile::ReadOwner | QFile::WriteOwner);
        QSKIP("file permissions are ignored for this user");
    }
    QCOMPARE(Cli::run({"config", "--new-per-day", "5", "--json"}), 1);
    QFile::setPermissions(database, QFile::ReadOwner | QFile::WriteOwner);
}

void TestCli::cliEditsAndDeletesCardsAcrossDecks() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    QString cardId;
    {
        auto ownedBackend = Backend::fromEnvironment();
        Backend &backend = *ownedBackend;
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("First", &error));
        QVERIFY(backend.createCard("First", "Q1", "A1", "basic", {}, &error));
        QVERIFY(backend.createDeck("Second", &error));
        QVERIFY(backend.createCard("Second", "Q2", "A2", "basic", {}, &error));
        for (const Deck &deck : backend.allDecks())
            for (const Card &card : deck.cards)
                if (card.question == QStringLiteral("Q2"))
                    cardId = card.id;
    }
    QVERIFY(!cardId.isEmpty());

    // With no --deck, both commands must follow the card's own deck
    // ("Second"), not whichever deck happens to be current ("First").
    QCOMPARE(Cli::run({"edit-card", "-c", cardId, "-q", "Q2b", "--json"}), 0);

    // A generated id follows the content, so the edit gave the card a new
    // id; look it up again before deleting.
    auto ownedEdited = Backend::fromEnvironment();
    Backend &edited = *ownedEdited;
    QString error;
    QVERIFY(edited.initialize(&error));
    QString updatedId;
    for (const Deck &deck : edited.allDecks())
        for (const Card &card : deck.cards)
            if (card.question == QStringLiteral("Q2b"))
                updatedId = card.id;
    QVERIFY(!updatedId.isEmpty());
    QCOMPARE(Cli::run({"delete-card", "-c", updatedId, "--json"}), 0);

    auto ownedCheck = Backend::fromEnvironment();
    Backend &check = *ownedCheck;
    QVERIFY(check.initialize(&error));
    bool found = false;
    for (const Deck &deck : check.allDecks())
        for (const Card &card : deck.cards)
            found |= card.id == updatedId;
    QVERIFY(!found);
}

void TestCli::previewRendersMathAsText() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"json({"cards":[{"question":"Expand \\(x^2\\)",
                                              "answer":"\\(\\frac{1}{2}\\)"}]})json"));
    int code = -1;
    runCli({"add-cards", "-d", "Math", "-i", input, "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 0);

    const QJsonObject result =
        runCli({"preview", "--deck", "Math", "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 0);
    const QJsonArray items = result.value(QStringLiteral("items")).toArray();
    QCOMPARE(items.size(), 1);
    const QJsonObject item = items.at(0).toObject();
    QCOMPARE(item.value(QStringLiteral("question")).toString(), QString("Expand x²"));
    QCOMPARE(item.value(QStringLiteral("answer")).toString(), QString("(1)/(2)"));
}

void TestCli::checkMathReportsUnsupportedCommands() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"json({"cards":[{"question":"Bad \\(\\foobar{x}\\)",
                                              "answer":"ok"}]})json"));
    int code = -1;
    runCli({"add-cards", "-d", "Warn", "-i", input, "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 0);

    const QJsonObject result =
        runCli({"check", "--math", "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 0);
    const QJsonArray warnings = result.value(QStringLiteral("mathWarnings")).toArray();
    QCOMPARE(warnings.size(), 1);
    QCOMPARE(warnings.at(0).toObject().value(QStringLiteral("message")).toString(),
             QString("Unsupported math command: \\foobar"));
}

void TestCli::checkReportsLineNumberedIssues() {
    QTemporaryDir dir;
    EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
    QVERIFY(QDir().mkpath(dir.filePath("decks")));
    const QString path = dir.filePath("decks/Broken.txt");
    QVERIFY(writeFile(path, R"deck(# Deck: Broken
---
Q: Same question
A: one
---
Q: Same question
A: two
---
Q: Good question
A: ok
oops: not a key
   too much indent
)deck"));

    int code = -1;
    const QJsonObject result = runCli({"check", "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 0);
    QVERIFY(result.value(QStringLiteral("ok")).toBool());

    const QJsonArray issues = result.value(QStringLiteral("issues")).toArray();
    const QJsonObject counts = result.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QVERIFY(counts.value(QStringLiteral("warning")).toInt() >= 3);

    bool sawDuplicate = false;
    bool sawUnexpected = false;
    bool sawIndent = false;
    for (const QJsonValue &value : issues) {
        const QJsonObject issue = value.toObject();
        QVERIFY(!issue.value(QStringLiteral("hint")).toString().isEmpty());
        const QString message = issue.value(QStringLiteral("message")).toString();
        if (message == QStringLiteral("Duplicate question in deck")) {
            sawDuplicate = true;
            QCOMPARE(issue.value(QStringLiteral("line")).toInt(), 6);
            QVERIFY(issue.value(QStringLiteral("hint")).toString().contains(QStringLiteral("line 3")));
        } else if (message == QStringLiteral("Unexpected line in deck file")) {
            sawUnexpected = true;
            QCOMPARE(issue.value(QStringLiteral("line")).toInt(), 11);
        } else if (message == QStringLiteral("Continuation line has more than two leading spaces")) {
            sawIndent = true;
            QCOMPARE(issue.value(QStringLiteral("line")).toInt(), 12);
        }
    }
    QVERIFY(sawDuplicate);
    QVERIFY(sawUnexpected);
    QVERIFY(sawIndent);

    runCli({"check", "--strict", "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 1);
}
