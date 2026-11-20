#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QTemporaryDir>

#include "test_cli.h"
#include "test_support.h"
#include "backend.h"
#include "cli.h"
#include "deck.h"
#include "store.h"

using namespace TestSupport;

namespace {
QStringList sortedKeys(const QJsonObject &object) {
    QStringList keys = object.keys();
    keys.sort();
    return keys;
}
} // namespace

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

void TestCli::headlessCommandDetectionIsComplete() {
    const QStringList commands{
        "check",     "decks",       "cards",       "search", "add-deck", "add-cards", "add-card",
        "edit-card", "delete-card", "delete-deck", "import", "due",      "review",    "cram",
        "preview",   "skill",       "stats",       "config", "help",     "--help",    "-h",
    };
    for (const QString &command : commands)
        QVERIFY2(Cli::needsOffscreen({command}), qPrintable(command));

    QVERIFY(!Cli::needsOffscreen({}));
    QVERIFY(!Cli::needsOffscreen({""}));
    QVERIFY(!Cli::needsOffscreen({"open"}));
    QVERIFY(!Cli::needsOffscreen({"open", "check"}));
    // Unknown commands are headless too, so they fail fast instead of opening
    // a GUI event loop that never returns.
    QVERIFY(Cli::needsOffscreen({"banana"}));
    QVERIFY(Cli::needsOffscreen({"banana", "--json"}));
    QVERIFY(Cli::needsOffscreen({"check", "open"}));
}

void TestCli::unknownCommandJsonError() {
    // Regression test: spawning the binary with an unknown command used to
    // launch the GUI instead of returning from Cli::run, so it never exited.
    QVERIFY(Cli::needsOffscreen({"banana", "--json"}));

    QTemporaryDir dir;
    int code = -1;
    const QJsonObject result = runCli({"banana", "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 1);
    QVERIFY(!result.value(QStringLiteral("ok")).toBool());
    QCOMPARE(result.value(QStringLiteral("error")).toString(), QString("Unknown command: banana"));
}

void TestCli::checkJsonKeyContract() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Clean", &error));
        QVERIFY(owned->createCard("Clean", "Q", "A", "basic", {}, &error));
    }

    int code = -1;
    const QJsonObject result = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(result.value(QStringLiteral("ok")).toBool());
    QStringList expected{"ok",       "version",  "decks",     "cards",     "due",
                         "totalDue", "newToday", "newPerDay", "retention", "dayStartHour",
                         "math",     "decksDir", "database",  "issues",    "issueCounts"};
    expected.sort();
    QCOMPARE(sortedKeys(result), expected);
    QCOMPARE(result.value(QStringLiteral("version")).toString(), QString("0.1.0"));
    QVERIFY(!result.contains(QStringLiteral("mathWarnings")));

    const QJsonObject withMath = runCli({"check", "--math", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(withMath.contains(QStringLiteral("mathWarnings")));
    QVERIFY(withMath.value(QStringLiteral("mathWarnings")).isArray());

    runCli({"check", "--strict", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);

    // A duplicate question is a warning, not a hard error: ok stays true but
    // --strict still exits 1.
    QVERIFY(writeFile(QDir(decks).filePath("Warn.txt"), R"deck(# Deck: Warn
---
Q: same
A: one
---
Q: same
A: two
)deck"));
    const QJsonObject warned = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(warned.value(QStringLiteral("ok")).toBool());
    const QJsonObject counts = warned.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QVERIFY(counts.value(QStringLiteral("warning")).toInt() >= 1);
    runCli({"check", "--strict", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
}

void TestCli::readCommandJsonContracts() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    QString cardId;
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Alpha", &error));
        QVERIFY(owned->createCard("Alpha", "alpha one", "A1", "basic", {}, &error));
        QVERIFY(owned->createCard("Alpha", "alpha two", "A2", "basic", {}, &error));
        QVERIFY(owned->createDeck("Beta", &error));
        QVERIFY(owned->createCard("Beta", "beta three", "A3", "basic", {}, &error));
        for (const Deck &deck : owned->allDecks())
            for (const Card &card : deck.cards)
                if (card.question == QStringLiteral("alpha two"))
                    cardId = card.id;
    }
    QVERIFY(!cardId.isEmpty());

    int code = -1;
    const QJsonObject decksJson = runCli({"decks", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(decksJson), QStringList({"decks", "ok"}));
    const QJsonArray deckArray = decksJson.value(QStringLiteral("decks")).toArray();
    QCOMPARE(deckArray.size(), 2);
    QCOMPARE(sortedKeys(deckArray.at(0).toObject()), QStringList({"cards", "due", "name", "path"}));

    const QJsonObject cardsJson = runCli({"cards", "--deck", "Alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(cardsJson), QStringList({"cards", "ok"}));
    const QJsonArray cardArray = cardsJson.value(QStringLiteral("cards")).toArray();
    QCOMPARE(cardArray.size(), 2);
    QCOMPARE(sortedKeys(cardArray.at(0).toObject()), QStringList({"answer", "deck", "id", "question", "tags", "type"}));

    const QJsonObject search = runCli({"search", "alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(search), QStringList({"cards", "count", "ok"}));
    QCOMPARE(search.value(QStringLiteral("count")).toInt(), 2);
    const QJsonObject scoped = runCli({"search", "alpha", "--deck", "Beta", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(scoped.value(QStringLiteral("count")).toInt(), 0);

    const QJsonObject due = runCli({"due", "--deck", "Alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(due), QStringList({"cards", "count", "ok", "totalDue"}));
    QCOMPARE(due.value(QStringLiteral("count")).toInt(), 2);
    QCOMPARE(due.value(QStringLiteral("totalDue")).toInt(), 2);

    runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);

    const QJsonObject stats = runCli({"stats", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expectedStats{"ok",        "totalReviews", "today",    "todayCorrect", "streakDays", "newToday",
                              "newPerDay", "due",          "totalDue", "cards",        "decks"};
    expectedStats.sort();
    QCOMPARE(sortedKeys(stats), expectedStats);
    QCOMPARE(stats.value(QStringLiteral("today")).toInt(), 1);
    QCOMPARE(stats.value(QStringLiteral("todayCorrect")).toInt(), 1);
    QCOMPARE(stats.value(QStringLiteral("cards")).toInt(), 3);
    QCOMPARE(stats.value(QStringLiteral("decks")).toInt(), 2);

    const QJsonObject config = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expectedConfig{"ok", "newPerDay", "retention", "dayStartHour", "newToday", "newRemaining"};
    expectedConfig.sort();
    QCOMPARE(sortedKeys(config), expectedConfig);
    QCOMPARE(config.value(QStringLiteral("newToday")).toInt(), 1);
    QCOMPARE(config.value(QStringLiteral("newRemaining")).toInt(),
             config.value(QStringLiteral("newPerDay")).toInt() - 1);
}

void TestCli::reviewCommandContract() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    QString cardId;
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Review", &error));
        QVERIFY(owned->createCard("Review", "Q", "A", "basic", {}, &error));
        cardId = owned->allDecks().first().cards.first().id;
    }
    QVERIFY(!cardId.isEmpty());

    int code = -1;
    const QJsonObject good = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expected{"ok", "id", "rating", "intervalDays", "due", "stability", "difficulty"};
    expected.sort();
    QCOMPARE(sortedKeys(good), expected);
    QCOMPARE(good.value(QStringLiteral("id")).toString(), cardId);
    QCOMPARE(good.value(QStringLiteral("rating")).toInt(), 3);
    QVERIFY(good.value(QStringLiteral("intervalDays")).toInt() >= 0);

    QString error;
    qint64 firstDue = 0;
    {
        Store state(database);
        QVERIFY(state.open(&error));
        QCOMPARE(state.states().value(cardId).reps, 1);
        firstDue = state.states().value(cardId).due;
    }

    const QJsonObject second = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    {
        Store state(database);
        QVERIFY(state.open(&error));
        QCOMPARE(state.states().value(cardId).reps, 2);
        QVERIFY(state.states().value(cardId).due > firstDue);
    }

    const QJsonObject again = runCli({"review", "-c", cardId, "-r", "1", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(again.value(QStringLiteral("rating")).toInt(), 1);

    auto expectBadRating = [&](const QStringList &args) {
        const QJsonObject bad = runCli(args, decks, database, &code);
        QCOMPARE(code, 1);
        QVERIFY(!bad.value(QStringLiteral("ok")).toBool());
        QCOMPARE(bad.value(QStringLiteral("error")).toString(), QString("Rating must be 1 (Again) or 3 (Good)"));
    };
    expectBadRating({"review", "-c", cardId, "-r", "0", "--json"});
    expectBadRating({"review", "-c", cardId, "-r", "2", "--json"});
    expectBadRating({"review", "-c", cardId, "--json"});

    const QJsonObject missing = runCli({"review", "-c", "missing", "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(missing.value(QStringLiteral("error")).toString(), QString("No such card: missing"));

    runCli({"edit-card", "-c", cardId, "-a", "changed", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QString editedId;
    {
        auto owned = Backend::fromEnvironment();
        QString loadError;
        QVERIFY(owned->initialize(&loadError));
        for (const Deck &deck : owned->allDecks())
            for (const Card &card : deck.cards)
                if (card.answer == QStringLiteral("changed"))
                    editedId = card.id;
    }
    QVERIFY(!editedId.isEmpty());
    QVERIFY(editedId != cardId);
    Store after(database);
    QVERIFY(after.open(&error));
    QVERIFY(!after.states().contains(editedId));
}

void TestCli::cramCommandDeterminism() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Cram", &error));
        QVERIFY(owned->createCard("Cram", "Q", "A", "basic", {}, &error));
    }

    int code = -1;
    const QJsonObject four = runCli({"cram", "--deck", "Cram", "--steps", "4", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(four), QStringList({"box", "correct", "ok", "reviews"}));
    QCOMPARE(four.value(QStringLiteral("reviews")).toInt(), 4);
    QCOMPARE(four.value(QStringLiteral("correct")).toInt(), 3);
    QCOMPARE(four.value(QStringLiteral("box")).toInt(), 1); // 1-based; step 4 is Again

    const QJsonObject two = runCli({"cram", "--deck", "Cram", "--steps", "2", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(two.value(QStringLiteral("reviews")).toInt(), 2);
    QCOMPARE(two.value(QStringLiteral("correct")).toInt(), 2);
    QCOMPARE(two.value(QStringLiteral("box")).toInt(), 3);

    const QJsonObject clamped = runCli({"cram", "--deck", "Cram", "--steps", "0", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(clamped.value(QStringLiteral("reviews")).toInt(), 1);
    QCOMPARE(clamped.value(QStringLiteral("box")).toInt(), 2);

    const QJsonObject none = runCli({"cram", "--deck", "Unknown", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(none.value(QStringLiteral("error")).toString(), QString("No cards in this deck"));
}

void TestCli::addCardJsonContracts() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));

    int code = -1;
    const QJsonObject noQuestion = runCli({"add-card", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(noQuestion.value(QStringLiteral("error")).toString(), QString("Use --question / -q"));

    const QJsonObject added =
        runCli({"add-card", "--deck", "Fresh", "-q", "Q", "-a", "A", "--type", "cloze", "--tags", "x,y", "--json"},
               decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expected{"ok", "id", "card"};
    expected.sort();
    QCOMPARE(sortedKeys(added), expected);
    const QJsonObject card = added.value(QStringLiteral("card")).toObject();
    QStringList cardKeys{"id", "deck", "question", "answer", "type", "tags"};
    cardKeys.sort();
    QCOMPARE(sortedKeys(card), cardKeys);
    QCOMPARE(card.value(QStringLiteral("deck")).toString(), QString("Fresh"));
    QCOMPARE(card.value(QStringLiteral("type")).toString(), QString("cloze"));
    QCOMPARE(card.value(QStringLiteral("tags")).toArray().size(), 2);
    QCOMPARE(added.value(QStringLiteral("id")).toString(), card.value(QStringLiteral("id")).toString());
    QVERIFY(QFileInfo::exists(QDir(decks).filePath("Fresh.txt")));
}

void TestCli::addCardsInputValidationErrors() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));

    struct Case {
        const char *name;
        const char *contents;
    };
    const QVector<Case> cases{
        {"Malformed", "{"},
        {"NonRoot", "null"},
        {"Empty", R"({"cards":[]})"},
        {"ScalarEntry", R"(["oops"])"},
        {"BadType", R"([{"question":"Q","answer":"A","type":"weird"}])"},
    };
    for (const Case &item : cases) {
        const QString input = dir.filePath(QString("%1.json").arg(item.name));
        QVERIFY(writeFile(input, item.contents));
        int code = -1;
        const QJsonObject result =
            runCli({"add-cards", "--deck", item.name, "--input", input, "--json"}, decks, database, &code);
        QCOMPARE(code, 1);
        QVERIFY(!result.value(QStringLiteral("ok")).toBool());
        QVERIFY(!result.value(QStringLiteral("error")).toString().isEmpty());
        QVERIFY2(!QFileInfo::exists(QDir(decks).filePath(QString("%1.txt").arg(item.name))), item.name);
    }
}

void TestCli::editDeleteErrorContracts() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    QString cardId;
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Edit", &error));
        QVERIFY(owned->createCard("Edit", "Q", "A", "basic", "one,two", &error));
        cardId = owned->allDecks().first().cards.first().id;
    }

    int code = -1;
    const QJsonObject unknownCard = runCli({"edit-card", "-c", "missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(unknownCard.value(QStringLiteral("error")).toString(), QString("No such card: missing"));

    const QJsonObject partial = runCli({"edit-card", "-c", cardId, "-q", "NewQ", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject card = partial.value(QStringLiteral("card")).toObject();
    QCOMPARE(card.value(QStringLiteral("question")).toString(), QString("NewQ"));
    QCOMPARE(card.value(QStringLiteral("answer")).toString(), QString("A"));
    QCOMPARE(card.value(QStringLiteral("type")).toString(), QString("basic"));
    QCOMPARE(card.value(QStringLiteral("tags")).toArray().size(), 2);

    // modifyCard has no type validation: unknown text normalizes to basic.
    const QString editedId = partial.value(QStringLiteral("id")).toString();
    const QJsonObject normalized =
        runCli({"edit-card", "-c", editedId, "--type", "weird", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(normalized.value(QStringLiteral("card")).toObject().value(QStringLiteral("type")).toString(),
             QString("basic"));

    const QJsonObject noCard = runCli({"delete-card", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(noCard.value(QStringLiteral("error")).toString(), QString("Use --card / -c"));
    const QJsonObject missingCard = runCli({"delete-card", "-c", "missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(missingCard.value(QStringLiteral("error")).toString(), QString("No such card: missing"));

    const QJsonObject noDeck = runCli({"delete-deck", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(noDeck.value(QStringLiteral("error")).toString(), QString("Give a deck name"));
    const QJsonObject missingDeck = runCli({"delete-deck", "Unknown", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(missingDeck.value(QStringLiteral("error")).toString(), QString("No such deck: Unknown"));
}

void TestCli::importJsonAndCsvContracts() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));

    int code = -1;
    const QString unsupported = dir.filePath("notes.md");
    QVERIFY(writeFile(unsupported, "hello"));
    const QJsonObject bad = runCli({"import", unsupported, "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(bad.value(QStringLiteral("error")).toString(), QString("Unsupported import format: %1").arg(unsupported));

    const QString csv = dir.filePath("vocab.csv");
    QVERIFY(writeFile(csv, "question,answer,tags\none,uno,vocab\ntwo,dos,vocab\n"));
    const QJsonObject importedCsv = runCli({"import", csv, "--deck", "Vocab", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(importedCsv.value(QStringLiteral("ok")).toBool());
    QString error;
    const Deck vocabDeck = DeckIO::load(QDir(decks).filePath("Vocab.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(vocabDeck.cards.size(), 2);

    const QString arrayJson = dir.filePath("Array.json");
    QVERIFY(writeFile(arrayJson, R"([{"question":"j1","answer":"a1"}])"));
    const QJsonObject importedArray = runCli({"import", arrayJson, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(importedArray.value(QStringLiteral("deck")).toString(), QString("Array"));
    const Deck arrayDeck = DeckIO::load(QDir(decks).filePath("Array.txt"), &error);
    QCOMPARE(arrayDeck.cards.size(), 1);

    const QString objectJson = dir.filePath("Object.json");
    QVERIFY(writeFile(objectJson, R"({"description":"json description","cards":[{"question":"j2","answer":"a2"}]})"));
    const QJsonObject importedObject =
        runCli({"import", objectJson, "--description", "flag description", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(importedObject.value(QStringLiteral("deck")).toString(), QString("Object"));
    const Deck objectDeck = DeckIO::load(QDir(decks).filePath("Object.txt"), &error);
    QCOMPARE(objectDeck.description, QString("flag description"));
    QCOMPARE(objectDeck.cards.size(), 1);
}

void TestCli::previewJsonContracts() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    int code = -1;
    const QJsonObject noDeck = runCli({"preview", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(noDeck.value(QStringLiteral("error")).toString(), QString("Use --deck / -d"));

    const QJsonObject badFormat =
        runCli({"preview", "--deck", "P", "--format", "xml", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(badFormat.value(QStringLiteral("error")).toString(), QString("--format must be text or html"));

    const QJsonObject noCards = runCli({"preview", "--deck", "Missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(noCards.value(QStringLiteral("error")).toString(), QString("No cards to preview"));

    QString firstId;
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("P", &error));
        QVERIFY(owned->createCard("P", "Q1", "A1", "basic", {}, &error));
        QVERIFY(owned->createCard("P", "Q2", "A2", "basic", {}, &error));
        firstId = owned->allDecks().first().cards.first().id;
    }
    QVERIFY(!firstId.isEmpty());

    const QJsonObject all = runCli({"preview", "--deck", "P", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(all), QStringList({"deck", "format", "items", "ok"}));
    QCOMPARE(all.value(QStringLiteral("items")).toArray().size(), 2);

    const QJsonObject one = runCli({"preview", "--deck", "P", "--card", firstId, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(one.value(QStringLiteral("items")).toArray().size(), 1);

    const QJsonObject html = runCli({"preview", "--deck", "P", "--format", "html", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject item = html.value(QStringLiteral("items")).toArray().at(0).toObject();
    QStringList itemKeys{"id", "questionHtml", "answerHtml"};
    itemKeys.sort();
    QCOMPARE(sortedKeys(item), itemKeys);
}

void TestCli::configValidationAndPersistence() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    int code = -1;
    auto expectError = [&](const QStringList &args, const QString &message) {
        const QJsonObject result = runCli(args, decks, database, &code);
        QCOMPARE(code, 1);
        QVERIFY(!result.value(QStringLiteral("ok")).toBool());
        QCOMPARE(result.value(QStringLiteral("error")).toString(), message);
    };
    expectError({"config", "--retention", "0.69", "--json"}, "Retention must be between 0.70 and 0.99");
    expectError({"config", "--retention", "1.0", "--json"}, "Retention must be between 0.70 and 0.99");
    expectError({"config", "--new-per-day", "10000", "--json"}, "New cards per day must be 9999 or less");
    expectError({"config", "--day-start-hour", "-2", "--json"}, "Day start hour must be between 0 and 23");
    expectError({"config", "--day-start-hour", "24", "--json"}, "Day start hour must be between 0 and 23");

    const QJsonObject boundaries =
        runCli({"config", "--new-per-day", "0", "--retention", "0.99", "--day-start-hour", "23", "--json"}, decks,
               database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(boundaries.value(QStringLiteral("newPerDay")).toInt(), 0);
    QCOMPARE(boundaries.value(QStringLiteral("retention")).toDouble(), 0.99);
    QCOMPARE(boundaries.value(QStringLiteral("dayStartHour")).toInt(), 23);

    runCli({"config", "--new-per-day", "5", "--retention", "0.70", "--day-start-hour", "0", "--json"}, decks, database,
           &code);
    QCOMPARE(code, 0);
    const QJsonObject reread = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(reread.value(QStringLiteral("newPerDay")).toInt(), 5);
    QCOMPARE(reread.value(QStringLiteral("retention")).toDouble(), 0.70);
    QCOMPARE(reread.value(QStringLiteral("dayStartHour")).toInt(), 0);

    // -1 is the "leave unchanged" sentinel and stays accepted.
    runCli({"config", "--day-start-hour", "-1", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);

    QString cardId;
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Budget", &error));
        QVERIFY(owned->createCard("Budget", "Q", "A", "basic", {}, &error));
        cardId = owned->allDecks().first().cards.first().id;
    }
    runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject budget = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(budget.value(QStringLiteral("newToday")).toInt(), 1);
    QCOMPARE(budget.value(QStringLiteral("newRemaining")).toInt(), 4);
}

void TestCli::flagEqualsFormsMatchSeparateArgs() {
    QTemporaryDir separateRoot;
    QTemporaryDir equalsRoot;
    const QString separateDecks = separateRoot.filePath("decks");
    const QString equalsDecks = equalsRoot.filePath("decks");
    const QString separateDb = separateRoot.filePath("state.db");
    const QString equalsDb = equalsRoot.filePath("state.db");
    QVERIFY(QDir().mkpath(separateDecks));
    QVERIFY(QDir().mkpath(equalsDecks));

    const QString inputSeparate = separateRoot.filePath("cards.json");
    const QString inputEquals = equalsRoot.filePath("cards.json");
    QVERIFY(writeFile(inputSeparate, R"([{"question":"Q","answer":"A"}])"));
    QVERIFY(writeFile(inputEquals, R"([{"question":"Q","answer":"A"}])"));

    int codeSeparate = -1;
    int codeEquals = -1;

    const QJsonObject cardSeparate = runCli({"add-card", "--deck", "D", "--question", "Q", "--answer", "A", "--json"},
                                            separateDecks, separateDb, &codeSeparate);
    const QJsonObject cardEquals =
        runCli({"add-card", "--deck=D", "--question=Q", "--answer=A", "--json"}, equalsDecks, equalsDb, &codeEquals);
    QCOMPARE(codeSeparate, 0);
    QCOMPARE(codeEquals, 0);
    QCOMPARE(QJsonDocument(cardEquals).toJson(), QJsonDocument(cardSeparate).toJson());

    const QJsonObject descSeparate =
        runCli({"add-deck", "Extra", "--description", "Desc", "--json"}, separateDecks, separateDb, &codeSeparate);
    const QJsonObject descEquals =
        runCli({"add-deck", "Extra", "--description=Desc", "--json"}, equalsDecks, equalsDb, &codeEquals);
    QCOMPARE(codeSeparate, 0);
    QCOMPARE(codeEquals, 0);
    QCOMPARE(QJsonDocument(descEquals).toJson(), QJsonDocument(descSeparate).toJson());

    const QJsonObject bulkSeparate = runCli({"add-cards", "--deck", "Bulk", "--input", inputSeparate, "--json"},
                                            separateDecks, separateDb, &codeSeparate);
    const QJsonObject bulkEquals =
        runCli({"add-cards", "--deck=Bulk", "--input=" + inputEquals, "--json"}, equalsDecks, equalsDb, &codeEquals);
    QCOMPARE(codeSeparate, 0);
    QCOMPARE(codeEquals, 0);
    QCOMPARE(QJsonDocument(bulkEquals).toJson(), QJsonDocument(bulkSeparate).toJson());

    const QJsonObject previewSeparate =
        runCli({"preview", "--deck", "D", "--format", "html", "--json"}, separateDecks, separateDb, &codeSeparate);
    const QJsonObject previewEquals =
        runCli({"preview", "--deck=D", "--format=html", "--json"}, equalsDecks, equalsDb, &codeEquals);
    QCOMPARE(codeSeparate, 0);
    QCOMPARE(codeEquals, 0);
    QCOMPARE(QJsonDocument(previewEquals).toJson(), QJsonDocument(previewSeparate).toJson());
}
