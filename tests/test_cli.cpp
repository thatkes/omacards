#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "test_cli.h"
#include "test_support.h"
#include "backend.h"
#include "cli.h"
#include "config.h"
#include "deck.h"
#include "domain.h"
#include "mathrender.h"
#include "theme.h"

using namespace TestSupport;

namespace {
QStringList sortedKeys(const QJsonObject &object) {
    QStringList keys = object.keys();
    keys.sort();
    return keys;
}

const QString kCliTinySvg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" "
                                           "viewBox=\"0 0 10 5\"><g fill=\"rgb(0%, 0%, 0%)\"/></svg>");

QString checkDeckIssueLine(const QJsonObject &issue) {
    return QStringLiteral("%1:%2 %3: %4 (%5)")
        .arg(issue.value(QStringLiteral("path")).toString())
        .arg(issue.value(QStringLiteral("line")).toInt())
        .arg(issue.value(QStringLiteral("severity")).toString(), issue.value(QStringLiteral("message")).toString(),
             issue.value(QStringLiteral("hint")).toString());
}

} // namespace

void TestCli::helpIsHeadless() {
    const QStringList paths = Cli::commandPaths();
    QVERIFY2(!paths.isEmpty(), "The command-path registry must be available");
    for (const QString &path : paths) {
        const QString first = path.section(' ', 0, 0);
        if (Cli::isHelpCommand(first))
            QVERIFY2(Cli::needsOffscreen({first}), qPrintable(first));
    }
    QVERIFY(!Cli::needsOffscreen({"open"}));

    if (!QFileInfo::exists(appBinary()))
        QSKIP("Build the app before running CLI integration tests.");
    QProcess process;
    process.start(appBinary(), {"--help"});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    const QByteArray help = process.readAllStandardOutput();
    QVERIFY(help.contains("Usage:"));
    // Every documented command path comes from the same registry the CLI uses.
    // `open` is shown as [open] and help flags are covered above.
    for (const QString &path : paths) {
        if (path == QLatin1String("open") || Cli::isHelpCommand(path.section(' ', 0, 0)))
            continue;
        QVERIFY2(help.contains(QStringLiteral("omacards %1").arg(path).toUtf8()), qPrintable(path));
    }
    // Runtime still accepts the legacy aliases; only the registry omits them.
    QVERIFY(help.contains("(alias: active)"));
    QVERIFY(help.contains("--new-card-allowance"));
    QVERIFY(help.contains("--new-per-day"));
    QVERIFY(help.contains("--day-start-hour"));
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
    QCOMPARE(Domain::toString(deck.cards.at(1).type), QString("cloze"));
    QCOMPARE(deck.cards.at(1).tags, QStringList({"z", "w"}));
}

void TestCli::addCardsJsonMetadataContract() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"({"cards":[{"question":"Clean Q1?","answer":"A1"},
                                          {"question":"Clean Q2?","answer":"A2"}]})"));

    int code = -1;
    const QJsonObject added = runCli({"add-cards", "--deck", "Calculus Series", "--description",
                                      "Maclaurin and Taylor series", "--input", input, "--json"},
                                     decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(added.value(QStringLiteral("ok")).toBool());

    QStringList successKeys{"created", "deck", "ids", "issueCounts", "ok", "path"};
    successKeys.sort();
    QCOMPARE(sortedKeys(added), successKeys);
    QCOMPARE(added.value(QStringLiteral("deck")).toString(), QString("Calculus Series"));
    QCOMPARE(added.value(QStringLiteral("created")).toInt(), 2);
    QCOMPARE(added.value(QStringLiteral("ids")).toArray().size(), 2);

    const QJsonObject listed = runCli({"decks", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QString listedPath;
    const QJsonArray listedDecks = listed.value(QStringLiteral("decks")).toArray();
    for (const QJsonValue &value : listedDecks) {
        if (value.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Calculus Series"))
            listedPath = value.toObject().value(QStringLiteral("path")).toString();
    }
    const QString path = added.value(QStringLiteral("path")).toString();
    QVERIFY(!path.isEmpty());
    QCOMPARE(path, listedPath);
    QCOMPARE(path, QDir(decks).filePath(QStringLiteral("Calculus-Series.txt")));
    QVERIFY(QFileInfo::exists(path));

    const QJsonObject counts = added.value(QStringLiteral("issueCounts")).toObject();
    QStringList countKeys{"error", "warning"};
    countKeys.sort();
    QCOMPARE(sortedKeys(counts), countKeys);
    QVERIFY(counts.value(QStringLiteral("error")).isDouble());
    QVERIFY(counts.value(QStringLiteral("warning")).isDouble());
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 0);

    const QByteArray piped = R"([{"question":"Piped Q?","answer":"Piped A"}])";
    const QJsonObject throughStdin = runCli({"add-cards", "--deck", "Piped Series", "--input", "-", "--json"}, decks,
                                            database, &code, nullptr, &piped);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(throughStdin), successKeys);
    QCOMPARE(throughStdin.value(QStringLiteral("deck")).toString(), QString("Piped Series"));
    QCOMPARE(throughStdin.value(QStringLiteral("created")).toInt(), 1);
    QVERIFY(!throughStdin.value(QStringLiteral("path")).toString().isEmpty());
    QCOMPARE(sortedKeys(throughStdin.value(QStringLiteral("issueCounts")).toObject()), countKeys);

    const QString human =
        runCliText({"add-cards", "--deck", "Calculus Series", "--input", input}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(human, QStringLiteral("Added 2 cards to Calculus Series\n"));
}

void TestCli::addCardsPathKeepsRelativeDecksDir() {
    QTemporaryDir work;
    QVERIFY(work.isValid());
    QTemporaryDir relativeDecks(QDir::current().filePath(QStringLiteral("add-cards-relative-XXXXXX")));
    QVERIFY(relativeDecks.isValid());
    const QString decks = QDir::current().relativeFilePath(relativeDecks.path());
    QVERIFY(!QDir::isAbsolutePath(decks));
    const QString database = work.filePath("state.db");
    EnvGuard env(decks, database);

    const QString input = work.filePath("cards.json");
    QVERIFY(writeFile(input, R"([{"question":"Q1?","answer":"A1"}])"));

    int code = -1;
    const QJsonObject added =
        runCli({"add-cards", "--deck", "Calculus Series", "--input", input, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QString expectedPath = decks + QStringLiteral("/Calculus-Series.txt");
    QCOMPARE(added.value(QStringLiteral("path")).toString(), expectedPath);

    const QJsonObject listed = runCli({"decks", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray listedDecks = listed.value(QStringLiteral("decks")).toArray();
    QCOMPARE(listedDecks.size(), 1);
    QCOMPARE(listedDecks.at(0).toObject().value(QStringLiteral("path")).toString(), expectedPath);
}

void TestCli::addCardsJsonCountsOnlyWrittenDeck() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    QVERIFY(writeFile(QDir(decks).filePath(QStringLiteral("Target.txt")),
                      "# Deck: Target\n---\nQ: same\nA: one\n---\nQ: same\nA: two\n"));
    QVERIFY(writeFile(QDir(decks).filePath(QStringLiteral("Other.txt")),
                      "# Deck: Other\n---\nQ: other\nA: one\n---\nQ: other\nA: two\n---\n"
                      "Q: Bad \\(\\foobar{x}\\)\nA: ok\n"));

    int code = -1;
    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"([{"question":"Fresh question","answer":"Fresh answer"}])"));
    const QJsonObject added =
        runCli({"add-cards", "--deck", "Target", "--input", input, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject checked = runCli({"check", "--deck", "Target", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(added.value(QStringLiteral("issueCounts")).toObject()).toJson(),
             QJsonDocument(checked.value(QStringLiteral("issueCounts")).toObject()).toJson());
    const QJsonObject counts = added.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 1);

    const QJsonObject all = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(all.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("warning")).toInt() > 1);

    const QString duplicateInput = dir.filePath("duplicate.json");
    QVERIFY(writeFile(duplicateInput, R"([{"question":"same","answer":"three"}])"));
    const QJsonObject duplicate =
        runCli({"add-cards", "--deck", "Target", "--input", duplicateInput, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject checkedAgain = runCli({"check", "--deck", "Target", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(duplicate.value(QStringLiteral("issueCounts")).toObject()).toJson(),
             QJsonDocument(checkedAgain.value(QStringLiteral("issueCounts")).toObject()).toJson());
    QCOMPARE(duplicate.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("warning")).toInt(), 2);

    const QString mathInput = dir.filePath("math.json");
    QVERIFY(writeFile(mathInput, R"json([{"question":"Bad \\(\\foobar{x}\\)",
                                          "answer":"ok"}])json"));
    const QJsonObject mathAdded =
        runCli({"add-cards", "--deck", "Mathy", "--input", mathInput, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject mathChecked = runCli({"check", "--deck", "Mathy", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(mathAdded.value(QStringLiteral("issueCounts")).toObject()).toJson(),
             QJsonDocument(mathChecked.value(QStringLiteral("issueCounts")).toObject()).toJson());
    QCOMPARE(mathAdded.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("warning")).toInt(), 1);
    QCOMPARE(mathAdded.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("error")).toInt(), 0);
}

void TestCli::addCardsCountsOnlyWrittenDeckWhenReloadSkipsIt() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    // Other has a duplicate-question Warning. The newline in the new Card's
    // tag makes load() skip the written Deck on reload, so the written path
    // cannot come from allDecks(); lint must still stay on that one Deck.
    QVERIFY(writeFile(QDir(decks).filePath(QStringLiteral("Other.txt")),
                      "# Deck: Other\n---\nQ: same\nA: one\n---\nQ: same\nA: two\n"));
    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"([{"question":"Fresh question","answer":"Fresh answer","tags":["x\nevil"]}])"));

    int code = -1;
    const QJsonObject added =
        runCli({"add-cards", "--deck", "Weird", "--input", input, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList successKeys{"created", "deck", "ids", "issueCounts", "ok", "path"};
    successKeys.sort();
    QCOMPARE(sortedKeys(added), successKeys);
    QCOMPARE(added.value(QStringLiteral("path")).toString(), QDir(decks).filePath(QStringLiteral("Weird.txt")));

    const QJsonObject checked = runCli({"check", "--deck", "Weird", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(added.value(QStringLiteral("issueCounts")).toObject()).toJson(),
             QJsonDocument(checked.value(QStringLiteral("issueCounts")).toObject()).toJson());
    const QJsonObject counts = added.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 1);

    const QJsonObject other = runCli({"check", "--deck", "Other", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(other.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("warning")).toInt(), 1);
}

void TestCli::addCardsIgnoresUnreadableOtherDeck() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    const QString other = QDir(decks).filePath(QStringLiteral("Other.txt"));
    QVERIFY(writeFile(other, "# Deck: Other\n---\nQ: other\nA: one\n"));
    QVERIFY(QFile::setPermissions(other, QFile::Permission(0)));
    if (QFileInfo(other).isReadable()) {
        QFile::setPermissions(other, QFile::ReadOwner | QFile::WriteOwner);
        QSKIP("file permissions are ignored for this user");
    }

    int code = -1;
    const QJsonObject unreadable = runCli({"check", "--deck", "Other", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(unreadable.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("error")).toInt(), 1);

    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"([{"question":"Fresh question","answer":"Fresh answer","tags":["x\nevil"]}])"));
    const QJsonObject added =
        runCli({"add-cards", "--deck", "Weird", "--input", input, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList successKeys{"created", "deck", "ids", "issueCounts", "ok", "path"};
    successKeys.sort();
    QCOMPARE(sortedKeys(added), successKeys);
    QCOMPARE(added.value(QStringLiteral("path")).toString(), QDir(decks).filePath(QStringLiteral("Weird.txt")));

    const QJsonObject checked = runCli({"check", "--deck", "Weird", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(added.value(QStringLiteral("issueCounts")).toObject()).toJson(),
             QJsonDocument(checked.value(QStringLiteral("issueCounts")).toObject()).toJson());
    const QJsonObject counts = added.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 1);

    QFile::setPermissions(other, QFile::ReadOwner | QFile::WriteOwner);
}

void TestCli::addCardsMultilineRoundTrips() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    const QString input = dir.filePath("worked.json");
    QVERIFY(writeFile(input, R"({"cards":[{"question":"Work this out",
                                          "answer":"First step\nSecond step"}]})"));

    int code = -1;
    const QJsonObject added = runCli({"add-cards", "-d", "Worked", "-i", input, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(added.value(QStringLiteral("ok")).toBool());

    QFile file(QDir(decks).filePath("Worked.txt"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString written = QString::fromUtf8(file.readAll());
    file.close();
    QVERIFY(written.contains(QStringLiteral("A: First step\n  Second step")));

    const QJsonObject check = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject counts = check.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 0);
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

void TestCli::writeCommandsValidateSerializedDeck() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    const QString input = dir.filePath("cards.json");
    QVERIFY(writeFile(input, R"([{"question":"Q","answer":"A"}])"));
    const QString badDeck = QStringLiteral("Broken\n---\nA: orphan");

    int code = -1;
    const QJsonObject result = runCli({"add-cards", "-d", badDeck, "-i", input, "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QVERIFY(!result.value(QStringLiteral("ok")).toBool(true));
    QStringList failureKeys{"error", "ok"};
    failureKeys.sort();
    QCOMPARE(sortedKeys(result), failureKeys);
    const QString error = result.value(QStringLiteral("error")).toString();
    QVERIFY(error.contains(QStringLiteral("Deck would be invalid after write")));
    QVERIFY(error.contains(QStringLiteral("Hint:")));
    QVERIFY(!QFileInfo::exists(QDir(decks).filePath(DeckIO::safeFileName(badDeck) + ".txt")));
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
    QCOMPARE(Cli::run({"config", "--new-card-allowance", "5", "--json"}), 1);
    QFile::setPermissions(database, QFile::ReadOwner | QFile::WriteOwner);
}

void TestCli::cliEditsAndDeletesCardsAcrossDecks() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    EnvGuard env(decks, database);
    int code = -1;

    const QJsonObject first =
        runCli({"add-card", "-d", "First", "-q", "Q1", "-a", "A1", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(!first.value(QStringLiteral("id")).toString().isEmpty());
    const QJsonObject added =
        runCli({"add-card", "-d", "Second", "-q", "Q2", "-a", "A2", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QString cardId = added.value(QStringLiteral("id")).toString();
    QVERIFY(!cardId.isEmpty());
    runCli({"use-deck", "First", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);

    // With no --deck, edit-card must follow the card's own deck ("Second"),
    // not whichever deck happens to be current ("First").
    const QJsonObject edited = runCli({"edit-card", "-c", cardId, "-q", "Q2b", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(edited.value(QStringLiteral("id")).toString(), cardId);

    // Rating then renaming the Deck keeps the same id, and the next Rating
    // continues the same Scheduling state instead of starting a new card.
    const QJsonObject rated = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);

    const QJsonObject renamed = runCli({"rename-deck", "Second", "Renamed", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(renamed.value(QStringLiteral("ok")).toBool());
    QCOMPARE(renamed.value(QStringLiteral("deck")).toString(), QString("Renamed"));
    QCOMPARE(renamed.value(QStringLiteral("path")).toString(), QDir(decks).filePath("Renamed.txt"));
    QCOMPARE(renamed.value(QStringLiteral("renamedCards")).toInt(), 1);
    // edit-card above already pinned this id in the deck file, so the rename
    // preserves it without adding another id line.
    QCOMPARE(renamed.value(QStringLiteral("preservedIds")).toInt(), 0);
    QVERIFY(!QFileInfo::exists(QDir(decks).filePath("Second.txt")));
    QVERIFY(QFileInfo::exists(QDir(decks).filePath("Renamed.txt")));

    const QString confirmation = runCliText({"rename-deck", "Renamed", "Renamed Again"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(confirmation.contains(QStringLiteral("Renamed Again")));
    QCOMPARE(confirmation.trimmed().count('\n'), 0);

    const QJsonObject continued = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(continued.value(QStringLiteral("id")).toString(), cardId);
    QVERIFY(continued.value(QStringLiteral("stability")).toDouble() >
            rated.value(QStringLiteral("stability")).toDouble());

    const QJsonObject conflict = runCli({"rename-deck", "Renamed Again", "First", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(conflict), QStringList({"error", "ok"}));
    QCOMPARE(conflict.value(QStringLiteral("error")).toString(), QString("Deck already exists: First"));
    QVERIFY(!conflict.value(QStringLiteral("ok")).toBool());

    // The same id keeps resolving for a later command.
    const QJsonObject deleted = runCli({"delete-card", "-c", cardId, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(deleted.value(QStringLiteral("id")).toString(), cardId);
    const QJsonObject remaining = runCli({"cards", "-d", "Renamed Again", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(remaining.value(QStringLiteral("cards")).toArray().size(), 0);
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

void TestCli::checkReportsLineNumberedDeckIssues() {
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
    const QStringList commands = Cli::headlessCommands();
    QVERIFY2(!commands.isEmpty(), "The command-path registry must be available");
    for (const QString &command : commands)
        QVERIFY2(Cli::needsOffscreen({command}), qPrintable(command));

    // Every registry path's first token is either headless or the GUI command.
    for (const QString &path : Cli::commandPaths()) {
        const QString first = path.section(' ', 0, 0);
        QVERIFY2(commands.contains(first) || first == QLatin1String("open"), qPrintable(path));
    }
    // The legacy aliases stay accepted at runtime but are not registry paths.
    QVERIFY(!Cli::commandPaths().contains(QStringLiteral("decks active")));
    QVERIFY(!Cli::commandPaths().contains(QStringLiteral("--new-per-day")));

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
    QStringList expected{"ok",
                         "version",
                         "decks",
                         "cards",
                         "due",
                         "totalDue",
                         "newCardsOnStudyDay",
                         "newCardAllowance",
                         "retention",
                         "dayStartHour",
                         "math",
                         "decksDir",
                         "database",
                         "issues",
                         "issueCounts"};
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

void TestCli::checkDeckScoping() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    QVERIFY(writeFile(QDir(decks).filePath("Clean.txt"), "# Deck: Clean\n---\nQ: Clean question\nA: ok\n"));
    QVERIFY(writeFile(QDir(decks).filePath("Broken.txt"),
                      "# Deck: Broken\n---\nQ: Same\nA: ok\n---\nQ: Same\nA: \\(\\foobar{x}\\)\n"));

    int code = -1;
    const QJsonObject clean = runCli({"check", "--deck", "Clean", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(clean.value(QStringLiteral("ok")).toBool());
    QCOMPARE(clean.value(QStringLiteral("deck")).toString(), QString("Clean"));
    const QJsonArray cleanDecks = clean.value(QStringLiteral("decks")).toArray();
    QCOMPARE(cleanDecks.size(), 1);
    QCOMPARE(cleanDecks.at(0).toObject().value(QStringLiteral("name")).toString(), QString("Clean"));
    QCOMPARE(clean.value(QStringLiteral("cards")).toInt(), 1);
    QVERIFY(clean.value(QStringLiteral("issues")).toArray().isEmpty());
    const QJsonObject cleanCounts = clean.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(cleanCounts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(cleanCounts.value(QStringLiteral("warning")).toInt(), 0);

    const QJsonObject broken = runCli({"check", "--deck", "Broken", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray issues = broken.value(QStringLiteral("issues")).toArray();
    QVERIFY(issues.size() >= 2);
    bool sawDuplicate = false;
    bool sawMath = false;
    for (const QJsonValue &value : issues) {
        const QJsonObject issue = value.toObject();
        QVERIFY2(issue.value(QStringLiteral("path")).toString().endsWith(QStringLiteral("Broken.txt")),
                 qPrintable(issue.value(QStringLiteral("path")).toString()));
        sawDuplicate |=
            issue.value(QStringLiteral("message")).toString() == QStringLiteral("Duplicate question in deck");
        sawMath |=
            issue.value(QStringLiteral("message")).toString().startsWith(QStringLiteral("Math warning in answer"));
    }
    QVERIFY(sawDuplicate);
    QVERIFY(sawMath);

    const QJsonObject math = runCli({"check", "--deck", "Broken", "--math", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray warnings = math.value(QStringLiteral("mathWarnings")).toArray();
    QCOMPARE(warnings.size(), 1);
    QCOMPARE(warnings.at(0).toObject().value(QStringLiteral("deck")).toString(), QString("Broken"));

    const QJsonObject missing = runCli({"check", "--deck", "Unknown", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(missing.value(QStringLiteral("error")).toString(), QString("No such deck: Unknown"));

    const QJsonObject all = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(all.value(QStringLiteral("decks")).toArray().size(), 2);
    QVERIFY(!all.contains(QStringLiteral("deck")));
    QVERIFY(all.value(QStringLiteral("issues")).toArray().size() >= 2);
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
    QStringList expectedStats{
        "ok",  "totalRatings", "studyDayRatings", "studyDayGood", "streak", "newCardsOnStudyDay", "newCardAllowance",
        "due", "totalDue",     "cards",           "decks"};
    expectedStats.sort();
    QCOMPARE(sortedKeys(stats), expectedStats);
    QCOMPARE(stats.value(QStringLiteral("studyDayRatings")).toInt(), 1);
    QCOMPARE(stats.value(QStringLiteral("studyDayGood")).toInt(), 1);
    const QString statsHuman = runCliText({"stats"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY2(statsHuman.contains(QStringLiteral("Ratings on Study day: 1")), qPrintable(statsHuman));
    QVERIFY2(statsHuman.contains(QStringLiteral("100% Good")), qPrintable(statsHuman));
    QVERIFY2(statsHuman.contains(QStringLiteral("1-day Streak")), qPrintable(statsHuman));
    QVERIFY2(!statsHuman.contains(QStringLiteral("correct")), qPrintable(statsHuman));
    QCOMPARE(stats.value(QStringLiteral("cards")).toInt(), 3);
    QCOMPARE(stats.value(QStringLiteral("decks")).toInt(), 2);

    const QJsonObject config = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expectedConfig{"ok",           "newCardAllowance",   "retention",
                               "dayStartHour", "newCardsOnStudyDay", "newRemaining"};
    expectedConfig.sort();
    QCOMPARE(sortedKeys(config), expectedConfig);
    QCOMPARE(config.value(QStringLiteral("newCardsOnStudyDay")).toInt(), 1);
    QCOMPARE(config.value(QStringLiteral("newRemaining")).toInt(),
             config.value(QStringLiteral("newCardAllowance")).toInt() - 1);
}

void TestCli::useDeckCommandContract() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Alpha", &error));
        QVERIFY(owned->createDeck("Beta", &error));
    }

    int code = -1;
    const QJsonObject used = runCli({"use-deck", "Beta", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(used), QStringList({"deck", "ok"}));
    QVERIFY(used.value(QStringLiteral("ok")).toBool());
    QCOMPARE(used.value(QStringLiteral("deck")).toString(), QString("Beta"));
    QCOMPARE(runCliText({"use-deck", "Beta"}, decks, database, &code).trimmed(), QString("Using deck Beta"));
    QCOMPARE(code, 0);

    const QJsonObject current = runCli({"decks", "current", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(current.value(QStringLiteral("deck")).toString(), QString("Beta"));

    const QJsonObject unknown = runCli({"use-deck", "Missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(unknown), QStringList({"error", "ok"}));
    QVERIFY(!unknown.value(QStringLiteral("ok")).toBool());
    QCOMPARE(unknown.value(QStringLiteral("error")).toString(), QString("No such deck: Missing"));

    QByteArray standardError;
    const QJsonObject missing = runCli({"use-deck", "--json"}, decks, database, &code, &standardError);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(missing), QStringList({"error", "ok"}));
    QVERIFY(!missing.value(QStringLiteral("ok")).toBool());
    QVERIFY(missing.value(QStringLiteral("error")).toString().contains(QStringLiteral("use-deck NAME")));

    // A per-invocation --deck flag must not change the Current deck.
    runCli({"stats", "--deck", "Alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(runCliText({"decks", "current"}, decks, database, &code).trimmed(), QString("Beta"));
    QCOMPARE(code, 0);
}

void TestCli::decksCurrentCommandContract() {
    QTemporaryDir emptyDir;
    const QString emptyDecks = emptyDir.filePath("decks");
    const QString emptyDatabase = emptyDir.filePath("state.db");
    QVERIFY(QDir().mkpath(emptyDecks));

    int code = -1;
    QByteArray standardError;
    const QJsonObject none = runCli({"decks", "current", "--json"}, emptyDecks, emptyDatabase, &code, &standardError);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(none), QStringList({"error", "ok"}));
    QVERIFY(!none.value(QStringLiteral("ok")).toBool());
    QCOMPARE(none.value(QStringLiteral("error")).toString(), QString("No current deck"));
    runCli({"decks", "current"}, emptyDecks, emptyDatabase, &code, &standardError);
    QCOMPARE(code, 1);
    QVERIFY(standardError.contains("No current deck"));

    // `active` stays an accepted alias with the same output in both modes.
    const QJsonObject emptyAlias =
        runCli({"decks", "active", "--json"}, emptyDecks, emptyDatabase, &code, &standardError);
    QCOMPARE(code, 1);
    QCOMPARE(QJsonDocument(emptyAlias).toJson(), QJsonDocument(none).toJson());
    runCli({"decks", "active"}, emptyDecks, emptyDatabase, &code, &standardError);
    QCOMPARE(code, 1);
    QVERIFY(standardError.contains("No current deck"));

    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Only", &error));
    }
    QCOMPARE(runCliText({"decks", "current"}, decks, database, &code).trimmed(), QString("Only"));
    QCOMPARE(code, 0);
    const QJsonObject current = runCli({"decks", "current", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(current), QStringList({"deck", "ok"}));
    QCOMPARE(current.value(QStringLiteral("deck")).toString(), QString("Only"));
    const QJsonObject alias = runCli({"decks", "active", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(alias).toJson(), QJsonDocument(current).toJson());
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

    const QStringList expected{"difficulty", "due", "id", "intervalDays", "ok", "rating", "stability"};

    int code = -1;
    const QJsonObject good = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(good), expected);
    QVERIFY(good.value(QStringLiteral("ok")).toBool());
    QCOMPARE(good.value(QStringLiteral("id")).toString(), cardId);
    QCOMPARE(good.value(QStringLiteral("rating")).toInt(), 3);
    QVERIFY(good.value(QStringLiteral("intervalDays")).toInt() >= 0);

    const QJsonObject second = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(second), expected);
    QCOMPARE(second.value(QStringLiteral("id")).toString(), cardId);
    QCOMPARE(second.value(QStringLiteral("rating")).toInt(), 3);

    const QJsonObject again = runCli({"review", "-c", cardId, "-r", "1", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(again), expected);
    QCOMPARE(again.value(QStringLiteral("rating")).toInt(), 1);

    auto expectBadRating = [&](const QStringList &args) {
        const QJsonObject bad = runCli(args, decks, database, &code);
        QCOMPARE(code, 1);
        QCOMPARE(sortedKeys(bad), QStringList({"error", "ok"}));
        QVERIFY(!bad.value(QStringLiteral("ok")).toBool());
        QCOMPARE(bad.value(QStringLiteral("error")).toString(), QStringLiteral("Rating must be 1 (Again) or 3 (Good)"));
    };
    expectBadRating({"review", "-c", cardId, "-r", "0", "--json"});
    expectBadRating({"review", "-c", cardId, "-r", "2", "--json"});
    expectBadRating({"review", "-c", cardId, "--json"});

    const QJsonObject missing = runCli({"review", "-c", "missing", "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(missing), QStringList({"error", "ok"}));
    QCOMPARE(missing.value(QStringLiteral("error")).toString(), QStringLiteral("No such card: missing"));

    const QJsonObject change = runCli({"edit-card", "-c", cardId, "-a", "changed", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(change.value(QStringLiteral("id")).toString(), cardId);

    // The edited Answer keeps the same id, which still resolves for review.
    // Scheduling is proved in TestRatingRecording.
    const QJsonObject edited = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(edited), expected);
    QCOMPARE(edited.value(QStringLiteral("id")).toString(), cardId);
    QCOMPARE(edited.value(QStringLiteral("rating")).toInt(), 3);
}

void TestCli::reviewWrapsStoreErrors() {
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
        QVERIFY(owned->createDeck("Failure", &error));
        QVERIFY(owned->createCard("Failure", "Q", "A", "basic", {}, &error));
        cardId = owned->allDecks().first().cards.first().id;
    }
    QVERIFY(!cardId.isEmpty());

    // Leave the database openable but make the scheduling read fail, so the
    // raw Store error has to travel back through the CLI's translated message.
    const QString connection = QStringLiteral("cli-broken-state");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(database);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("DROP TABLE cards")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE cards (id TEXT)")));
    }
    QSqlDatabase::removeDatabase(connection);

    int code = -1;
    const QJsonObject failure = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(failure), QStringList({"error", "ok"}));
    QVERIFY(!failure.value(QStringLiteral("ok")).toBool());
    const QString message = failure.value(QStringLiteral("error")).toString();
    QVERIFY2(message.startsWith(QStringLiteral("Cannot read scheduling state: ")), qPrintable(message));
    QVERIFY(message.size() > QStringLiteral("Cannot read scheduling state: ").size());
}

void TestCli::reviewWrapsWriteStoreErrors() {
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
        QVERIFY(owned->createDeck("Failure", &error));
        QVERIFY(owned->createCard("Failure", "Q", "A", "basic", {}, &error));
        cardId = owned->allDecks().first().cards.first().id;
    }
    QVERIFY(!cardId.isEmpty());

    // Keep the scheduling read working, but leave the history table unable
    // to accept the append so only the write fails.
    const QString connection = QStringLiteral("cli-broken-history");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(database);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("DROP TABLE reviews")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE reviews (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                          "card_id TEXT NOT NULL, new_card INTEGER NOT NULL DEFAULT 0)")));
    }
    QSqlDatabase::removeDatabase(connection);

    int code = -1;
    const QJsonObject failure = runCli({"review", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(failure), QStringList({"error", "ok"}));
    QVERIFY(!failure.value(QStringLiteral("ok")).toBool());
    const QString message = failure.value(QStringLiteral("error")).toString();
    QVERIFY2(message.startsWith(QStringLiteral("Cannot record rating: ")), qPrintable(message));
    QVERIFY(message.size() > QStringLiteral("Cannot record rating: ").size());
}

void TestCli::cramCommandContract() {
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
    QCOMPARE(sortedKeys(four), QStringList({"good", "leitnerBox", "ok", "ratings"}));
    QCOMPARE(four.value(QStringLiteral("ratings")).toInt(), 4);
    QCOMPARE(four.value(QStringLiteral("good")).toInt(), 3);
    QVERIFY(!four.contains(QStringLiteral("correct")));
    QVERIFY(!four.contains(QStringLiteral("reviews")));
    QVERIFY(!four.contains(QStringLiteral("box")));
    // Exact Leitner box values pin the CLI output contract; progression
    // rules stay in the InSessionProgression suite.
    QCOMPARE(four.value(QStringLiteral("leitnerBox")).toInt(), 1);

    const QJsonObject two = runCli({"cram", "--deck", "Cram", "--steps", "2", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(two.value(QStringLiteral("ratings")).toInt(), 2);
    QCOMPARE(two.value(QStringLiteral("good")).toInt(), 2);
    QCOMPARE(two.value(QStringLiteral("leitnerBox")).toInt(), 3);

    const QJsonObject eight = runCli({"cram", "--deck", "Cram", "--steps", "8", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(eight.value(QStringLiteral("ratings")).toInt(), 8);
    QCOMPARE(eight.value(QStringLiteral("good")).toInt(), 6);
    QCOMPARE(eight.value(QStringLiteral("leitnerBox")).toInt(), 1);

    // Repeating step counts in a different order must reproduce each result.
    const QJsonObject repeatedEight =
        runCli({"cram", "--deck", "Cram", "--steps", "8", "--json"}, decks, database, &code);
    const QJsonObject repeatedFour =
        runCli({"cram", "--deck", "Cram", "--steps", "4", "--json"}, decks, database, &code);
    QCOMPARE(QJsonDocument(repeatedEight).toJson(), QJsonDocument(eight).toJson());
    QCOMPARE(QJsonDocument(repeatedFour).toJson(), QJsonDocument(four).toJson());

    const QString human = runCliText({"cram", "--deck", "Cram", "--steps", "4"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(human.trimmed(), QStringLiteral("4 reviews · 3 Good · box 1"));
    QVERIFY2(!human.contains(QStringLiteral("correct")), qPrintable(human));

    const QJsonObject clamped = runCli({"cram", "--deck", "Cram", "--steps", "0", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(clamped.value(QStringLiteral("ratings")).toInt(), 1);

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

void TestCli::cardsJsonPreservesUnknownFileType() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    const QString path = QDir(decks).filePath("Types.txt");
    QVERIFY(writeFile(path, "# Deck: Types\n---\nQ: Unknown\nA: answer\ntype: FUTURE\n"));

    QString error;
    const Deck deck = DeckIO::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(deck.cards.size(), 1);

    int code = -1;
    const QJsonObject result = runCli({"cards", "--deck", "Types", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray cards = result.value(QStringLiteral("cards")).toArray();
    QCOMPARE(cards.size(), 1);
    const QJsonObject card = cards.at(0).toObject();
    QCOMPARE(card.value(QStringLiteral("id")).toString(), deck.cards.first().id);
    QCOMPARE(card.value(QStringLiteral("type")).toString(), QString("future"));
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

    // HTML preview has no theme, so math must be emitted in black regardless
    // of the colors the app happens to use.
    const QString bin = dir.filePath("bin");
    QVERIFY(writeFakePdfToolchain(bin, kCliTinySvg));
    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", dir.filePath("args.log"));
    PathPrependGuard pathGuard(bin);
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createCard("P", "\\(z^2\\)", "plain", "basic", {}, &error));
    }

    const QJsonObject html = runCli({"preview", "--deck", "P", "--format", "html", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray htmlItems = html.value(QStringLiteral("items")).toArray();
    QCOMPARE(htmlItems.size(), 3);
    const QJsonObject item = htmlItems.at(0).toObject();
    QStringList itemKeys{"id", "questionHtml", "answerHtml"};
    itemKeys.sort();
    QCOMPARE(sortedKeys(item), itemKeys);

    QString mathHtml;
    for (const QJsonValue &value : htmlItems) {
        const QString question = value.toObject().value(QStringLiteral("questionHtml")).toString();
        if (question.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")))
            mathHtml = question;
    }
    QVERIFY(!mathHtml.isEmpty());
    QVERIFY(svgFromMathHtml(mathHtml).contains("fill=\"#000000\""));
}

void TestCli::previewZeroCardDeckIssues() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    const QDir deckDir(decks);
    QVERIFY(writeFile(deckDir.filePath("Broken.txt"), R"deck(# Deck: Broken
---
oops: not a key
   too much indent
)deck"));
    QVERIFY(writeFile(deckDir.filePath("Empty.txt"), "# Deck: Empty\n"));
    // The supplied name differs from both the filename and the header name.
    QVERIFY(writeFile(deckDir.filePath("Quoted-Name.txt"), "# Deck: Different Name\n"));
    QVERIFY(writeFile(deckDir.filePath("Good.txt"), "# Deck: Good\n---\nQ: Q?\nA: A\n"));

    int code = -1;
    QByteArray stderrOutput;
    const QStringList failureKeys{"deck", "error", "hint", "issueCounts", "issues", "ok"};

    // A malformed deck file with zero Cards: the JSON failure keeps ok/error
    // and adds the same Deck issues that check reports for that Deck.
    const QJsonObject check = runCli({"check", "--deck", "Broken", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray checkIssues = check.value(QStringLiteral("issues")).toArray();
    QVERIFY(checkIssues.size() >= 2);

    const QJsonObject broken = runCli({"preview", "--deck", "Broken", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(broken), failureKeys);
    QVERIFY(!broken.value(QStringLiteral("ok")).toBool());
    QCOMPARE(broken.value(QStringLiteral("error")).toString(), QStringLiteral("Deck Broken has no Cards"));
    QCOMPARE(broken.value(QStringLiteral("deck")).toString(), QStringLiteral("Broken"));
    QCOMPARE(broken.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck Broken"));
    QCOMPARE(QJsonDocument(broken.value(QStringLiteral("issues")).toArray()).toJson(QJsonDocument::Compact),
             QJsonDocument(checkIssues).toJson(QJsonDocument::Compact));
    const QJsonObject counts = broken.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(sortedKeys(counts), QStringList({"error", "warning"}));
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(),
             check.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("error")).toInt());
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(),
             check.value(QStringLiteral("issueCounts")).toObject().value(QStringLiteral("warning")).toInt());
    bool sawError = false;
    bool sawWarning = false;
    bool sawLine = false;
    for (const QJsonValue &value : broken.value(QStringLiteral("issues")).toArray()) {
        const QJsonObject issue = value.toObject();
        QVERIFY(issue.contains(QStringLiteral("path")));
        QVERIFY(!issue.value(QStringLiteral("severity")).toString().isEmpty());
        QVERIFY(!issue.value(QStringLiteral("message")).toString().isEmpty());
        QVERIFY(!issue.value(QStringLiteral("hint")).toString().isEmpty());
        sawError |= issue.value(QStringLiteral("severity")).toString() == QStringLiteral("error");
        sawWarning |= issue.value(QStringLiteral("severity")).toString() == QStringLiteral("warning");
        sawLine |= issue.value(QStringLiteral("line")).toInt() > 0;
    }
    QVERIFY(sawError);
    QVERIFY(sawWarning);
    QVERIFY(sawLine);

    // Text mode puts the whole Deck issue report on stderr and nothing on stdout.
    const QString brokenText = runCliText({"preview", "--deck", "Broken"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(brokenText.isEmpty());
    QStringList expectedLines{"Deck Broken has no Cards"};
    for (const QJsonValue &value : checkIssues)
        expectedLines << checkDeckIssueLine(value.toObject());
    expectedLines << QStringLiteral("omacards check --deck Broken");
    QCOMPARE(QString::fromUtf8(stderrOutput), expectedLines.join('\n') + '\n');

    // A valid empty Deck has no Deck issues but still gets the hint.
    const QJsonObject empty = runCli({"preview", "--deck", "Empty", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(empty), failureKeys);
    QCOMPARE(empty.value(QStringLiteral("error")).toString(), QStringLiteral("Deck Empty has no Cards"));
    QCOMPARE(empty.value(QStringLiteral("deck")).toString(), QStringLiteral("Empty"));
    QVERIFY(empty.value(QStringLiteral("issues")).toArray().isEmpty());
    const QJsonObject emptyCounts = empty.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(emptyCounts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(emptyCounts.value(QStringLiteral("warning")).toInt(), 0);
    QCOMPARE(empty.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck Empty"));

    const QString emptyText = runCliText({"preview", "--deck", "Empty"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(emptyText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput), QStringLiteral("Deck Empty has no Cards\nomacards check --deck Empty\n"));

    // A --card filter does not hide an existing zero-Card Deck's Deck issues.
    const QJsonObject emptyFiltered =
        runCli({"preview", "--deck", "Empty", "--card", "nope", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(emptyFiltered), failureKeys);
    QVERIFY(!emptyFiltered.value(QStringLiteral("ok")).toBool());
    QCOMPARE(emptyFiltered.value(QStringLiteral("error")).toString(), QStringLiteral("Deck Empty has no Cards"));
    QCOMPARE(emptyFiltered.value(QStringLiteral("deck")).toString(), QStringLiteral("Empty"));
    QVERIFY(emptyFiltered.value(QStringLiteral("issues")).toArray().isEmpty());
    const QJsonObject emptyFilteredCounts = emptyFiltered.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(sortedKeys(emptyFilteredCounts), QStringList({"error", "warning"}));
    QCOMPARE(emptyFilteredCounts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(emptyFilteredCounts.value(QStringLiteral("warning")).toInt(), 0);
    QCOMPARE(emptyFiltered.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck Empty"));
    const QString emptyFilteredText =
        runCliText({"preview", "--deck", "Empty", "--card", "nope"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(emptyFilteredText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput), QStringLiteral("Deck Empty has no Cards\nomacards check --deck Empty\n"));

    // The hint uses the supplied name, quoted only when it has whitespace,
    // even when the Deck file and header use different names.
    const QJsonObject quoted =
        runCli({"preview", "--deck", "Quoted Name", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QCOMPARE(quoted.value(QStringLiteral("deck")).toString(), QStringLiteral("Quoted Name"));
    QCOMPARE(quoted.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck 'Quoted Name'"));
    const QString quotedText = runCliText({"preview", "--deck", "Quoted Name"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(quotedText.isEmpty());
    QVERIFY(QString::fromUtf8(stderrOutput).endsWith("omacards check --deck 'Quoted Name'\n"));

    // A missing Deck keeps the old message and its old two-key JSON shape.
    const QJsonObject missing =
        runCli({"preview", "--deck", "Missing", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(missing), QStringList({"error", "ok"}));
    QVERIFY(!missing.value(QStringLiteral("ok")).toBool());
    QCOMPARE(missing.value(QStringLiteral("error")).toString(), QStringLiteral("No cards to preview"));
    const QString missingText = runCliText({"preview", "--deck", "Missing"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(missingText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput), QStringLiteral("No cards to preview\n"));

    // A --card miss also keeps the old message even though the Deck file exists.
    const QJsonObject filtered =
        runCli({"preview", "--deck", "Good", "--card", "nope", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(filtered), QStringList({"error", "ok"}));
    QCOMPARE(filtered.value(QStringLiteral("error")).toString(), QStringLiteral("No cards to preview"));
    const QString filteredText =
        runCliText({"preview", "--deck", "Good", "--card", "nope"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(filteredText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput), QStringLiteral("No cards to preview\n"));

    // A preview with Cards is unchanged in both output modes.
    const QJsonObject good = runCli({"preview", "--deck", "Good", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 0);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(good), QStringList({"deck", "format", "items", "ok"}));
    QVERIFY(good.value(QStringLiteral("ok")).toBool());
    const QJsonArray items = good.value(QStringLiteral("items")).toArray();
    QCOMPARE(items.size(), 1);
    const QJsonObject item = items.at(0).toObject();
    QCOMPARE(item.value(QStringLiteral("question")).toString(), QStringLiteral("Q?"));
    QCOMPARE(item.value(QStringLiteral("answer")).toString(), QStringLiteral("A"));
    const QString goodText = runCliText({"preview", "--deck", "Good"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 0);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(goodText, QStringLiteral("[%1]\nQ: Q?\nA: A\n").arg(item.value(QStringLiteral("id")).toString()));
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
    expectError({"config", "--new-card-allowance", "10000", "--json"}, "New-card allowance must be 9999 or less");
    expectError({"config", "--day-start-hour", "-2", "--json"}, "Day start hour must be between 0 and 23");
    expectError({"config", "--day-start-hour", "24", "--json"}, "Day start hour must be between 0 and 23");

    const QJsonObject boundaries =
        runCli({"config", "--new-card-allowance", "0", "--retention", "0.99", "--day-start-hour", "23", "--json"},
               decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(boundaries.value(QStringLiteral("newCardAllowance")).toInt(), 0);
    QCOMPARE(boundaries.value(QStringLiteral("retention")).toDouble(), 0.99);
    QCOMPARE(boundaries.value(QStringLiteral("dayStartHour")).toInt(), 23);

    runCli({"config", "--new-card-allowance", "5", "--retention", "0.70", "--day-start-hour", "0", "--json"}, decks,
           database, &code);
    QCOMPARE(code, 0);
    const QJsonObject reread = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(reread.value(QStringLiteral("newCardAllowance")).toInt(), 5);
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
    QCOMPARE(budget.value(QStringLiteral("newCardsOnStudyDay")).toInt(), 1);
    QCOMPARE(budget.value(QStringLiteral("newRemaining")).toInt(), 4);

    // The legacy flag stays accepted and writes the canonical setting.
    const QJsonObject alias = runCli({"config", "--new-per-day", "7", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(alias.value(QStringLiteral("newCardAllowance")).toInt(), 7);
    const QString configText = runCliText({"config"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(configText.contains(QStringLiteral("New-card allowance: 7")));
    QVERIFY(configText.contains(QStringLiteral("new cards on Study day: ")));
    QVERIFY(configText.contains(QStringLiteral("day-start hour: ")));
}

void TestCli::warmReportsRenderedAndCached() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    const QString bin = dir.filePath("bin");
    const QString cacheHome = dir.filePath("cache");
    const QString cache = cacheHome + "/omacards/math";
    const QString argsLog = dir.filePath("args.log");
    const QString themeDir = dir.filePath("theme");
    QVERIFY(QDir().mkpath(decks));
    QVERIFY(QDir().mkpath(themeDir + "/theme"));
    QVERIFY(writeFile(decks + "/Warm.txt", R"deck(# Deck: Warm
---
Q: What is \(x^2\) and \(x^2\)?
A: It is \(y^2\).
)deck"));
    QVERIFY(writeFile(themeDir + "/theme/colors.toml", R"theme(foreground = "#112233"
accent = "#445566"
)theme"));
    QVERIFY(writeFakePdfToolchain(bin, kCliTinySvg));

    EnvGuard env(decks, database);
    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", cacheHome);
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", argsLog);
    TestSupport::ScopedEnv themeEnv("OMACARDS_THEME_DIR", themeDir);
    TestSupport::ScopedEnv hyprEnv("HYPRLAND_INSTANCE_SIGNATURE", {});
    PathPrependGuard pathGuard(bin);

    int code = -1;
    const QJsonObject first = runCli({"warm", "--deck", "Warm", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(first), QStringList({"cached", "deck", "ok", "rendered"}));
    QVERIFY(first.value(QStringLiteral("ok")).toBool());
    QCOMPARE(first.value(QStringLiteral("deck")).toString(), QStringLiteral("Warm"));
    QCOMPARE(first.value(QStringLiteral("rendered")).toInt(), 2);
    QCOMPARE(first.value(QStringLiteral("cached")).toInt(), 0);
    QCOMPARE(toolchainRenders(argsLog), 2);

    MathRenderer verifier(cache);
    const QString xPath = verifier.mathSvgPath("x^2");
    QVERIFY(QFileInfo::exists(xPath));
    QVERIFY(QFileInfo::exists(verifier.mathSvgPath("y^2")));
    QCOMPARE(QDir(cache).entryList({"*.svg"}).size(), 2);
    QFile neutral(xPath);
    QVERIFY(neutral.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(neutral.readAll()).contains("rgb(0%, 0%, 0%)"));
    // Warming must not create or select study state as a side effect.
    QVERIFY(!QFileInfo::exists(database));

    // A different active theme must not re-render or duplicate anything.
    QVERIFY(writeFile(themeDir + "/theme/colors.toml", R"theme(foreground = "#ff0000"
accent = "#00ff00"
)theme"));
    QFile::remove(argsLog);
    const QString text = runCliText({"warm", "--deck", "Warm"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(text, QStringLiteral("Warmed Warm: 0 rendered · 2 cached\n"));
    QCOMPARE(toolchainRenders(argsLog), 0);

    const QJsonObject second = runCli({"warm", "--deck", "Warm", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(second.value(QStringLiteral("rendered")).toInt(), 0);
    QCOMPARE(second.value(QStringLiteral("cached")).toInt(), 2);
}

void TestCli::warmRejectsUnknownAndEmptyDecks() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    QVERIFY(writeFile(decks + "/Empty.txt", "# Deck: Empty\n"));
    EnvGuard env(decks, database);

    int code = -1;
    QByteArray stderrOutput;
    const QJsonObject missing = runCli({"warm", "--deck", "Missing", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(missing), QStringList({"error", "ok"}));
    QCOMPARE(missing.value(QStringLiteral("error")).toString(), QStringLiteral("No such deck: Missing"));

    const QString missingText = runCliText({"warm", "--deck", "Missing"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(missingText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput), QStringLiteral("No such deck: Missing\n"));

    const QJsonObject empty = runCli({"warm", "--deck", "Empty", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(empty), QStringList({"deck", "error", "hint", "issueCounts", "issues", "ok"}));
    QCOMPARE(empty.value(QStringLiteral("error")).toString(), QStringLiteral("Deck Empty has no Cards"));
    QCOMPARE(empty.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck Empty"));

    const QString emptyText = runCliText({"warm", "--deck", "Empty"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(emptyText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput), QStringLiteral("Deck Empty has no Cards\nomacards check --deck Empty\n"));
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
    // The two isolated decks directories give different absolute paths, so
    // compare everything except the path and pin each path to its own root.
    QJsonObject bulkSeparateWithoutPath = bulkSeparate;
    QJsonObject bulkEqualsWithoutPath = bulkEquals;
    bulkSeparateWithoutPath.remove(QStringLiteral("path"));
    bulkEqualsWithoutPath.remove(QStringLiteral("path"));
    QCOMPARE(QJsonDocument(bulkEqualsWithoutPath).toJson(), QJsonDocument(bulkSeparateWithoutPath).toJson());
    QCOMPARE(bulkSeparate.value(QStringLiteral("path")).toString(), QDir(separateDecks).filePath("Bulk.txt"));
    QCOMPARE(bulkEquals.value(QStringLiteral("path")).toString(), QDir(equalsDecks).filePath("Bulk.txt"));

    const QJsonObject previewSeparate =
        runCli({"preview", "--deck", "D", "--format", "html", "--json"}, separateDecks, separateDb, &codeSeparate);
    const QJsonObject previewEquals =
        runCli({"preview", "--deck=D", "--format=html", "--json"}, equalsDecks, equalsDb, &codeEquals);
    QCOMPARE(codeSeparate, 0);
    QCOMPARE(codeEquals, 0);
    QCOMPARE(QJsonDocument(previewEquals).toJson(), QJsonDocument(previewSeparate).toJson());

    const QJsonObject allowanceSeparate =
        runCli({"config", "--new-card-allowance", "8", "--json"}, separateDecks, separateDb, &codeSeparate);
    const QJsonObject allowanceEquals =
        runCli({"config", "--new-card-allowance=8", "--json"}, equalsDecks, equalsDb, &codeEquals);
    QCOMPARE(codeSeparate, 0);
    QCOMPARE(codeEquals, 0);
    QCOMPARE(QJsonDocument(allowanceEquals).toJson(), QJsonDocument(allowanceSeparate).toJson());
}
