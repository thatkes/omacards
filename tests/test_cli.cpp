#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTime>

#include <memory>
#include <vector>

#include "test_cli.h"
#include "test_support.h"
#include "backend.h"
#include "cli.h"
#include "config.h"
#include "deck.h"
#include "domain.h"
#include "fsrs.h"
#include "mathrender.h"
#include "store.h"
#include "theme.h"

using namespace TestSupport;

namespace {
QStringList sortedKeys(const QJsonObject &object) {
    QStringList keys = object.keys();
    keys.sort();
    return keys;
}

QString expectedUnreadableDeckError(const QString &deckName, const QString &path) {
    return QStringLiteral("Cannot read the Deck file at %1. Run 'omacards check --deck %2' to see Deck issues.")
        .arg(path, deckName);
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

std::unique_ptr<QProcess> startCliProcess(const QStringList &arguments, const QString &decks, const QString &database,
                                          const QString &cache) {
    auto process = std::make_unique<QProcess>();
    process->setProcessEnvironment(cliProcessEnvironment(decks, database, cache));
    process->start(appBinary(), arguments);
    return process;
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
    // Help documents the canonical spellings and no alias notes.
    QVERIFY(!help.contains("alias"));
    QVERIFY(help.contains("omacards decks current [--json]  Show the Current deck\n"));
    QVERIFY(help.contains("--new-card-allowance"));
    QVERIFY(help.contains("--day-start-hour"));

    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    EnvGuard env(decks, database);

    // Every real command path accepts both help flags through the process
    // boundary: purpose, flags and an example, exit 0, and no state write.
    for (const QString &path : paths) {
        const QString token = path.section(' ', 0, 0);
        if (Cli::isHelpCommand(token))
            continue; // top-level help flags keep their usage() contract
        for (const QString &flag : {QStringLiteral("--help"), QStringLiteral("-h")}) {
            int code = -1;
            QByteArray errors;
            const QByteArray output = runCliProcessRaw({token, flag}, decks, database, &code, &errors);
            const QString text = QString::fromUtf8(output);
            QVERIFY2(code == 0, qPrintable(token + " " + flag + " exited " + QString::number(code)));
            QVERIFY2(text.contains(token), qPrintable(token));
            QVERIFY2(text.contains("Purpose:"), qPrintable(token));
            QVERIFY2(text.contains("Flags:"), qPrintable(token));
            QVERIFY2(text.contains("Example:"), qPrintable(token));
            QVERIFY2(errors.isEmpty(), qPrintable(errors));
        }
    }

    // rename-deck has its own wording rather than the generic fallback.
    int renameCode = -1;
    const QString renameHelp =
        QString::fromUtf8(runCliProcessRaw({"rename-deck", "--help"}, decks, database, &renameCode));
    QCOMPARE(renameCode, 0);
    QVERIFY2(renameHelp.contains(QStringLiteral("Purpose: Rename a Deck.")), qPrintable(renameHelp));
    QVERIFY2(renameHelp.contains(QStringLiteral("Flags: OLD, NEW, [--json], --help, -h")), qPrintable(renameHelp));
    QVERIFY2(renameHelp.contains(QStringLiteral("Example: omacards rename-deck Calculus Analysis --json")),
             qPrintable(renameHelp));
    QVERIFY2(!renameHelp.contains(QStringLiteral("Run the rename-deck command.")), qPrintable(renameHelp));

    // add-cards help shows the minimal JSON input shape without the full schema.
    int addCardsCode = -1;
    const QString addCardsHelp =
        QString::fromUtf8(runCliProcessRaw({"add-cards", "--help"}, decks, database, &addCardsCode));
    QCOMPARE(addCardsCode, 0);
    QVERIFY2(addCardsHelp.contains(QStringLiteral("[{\"question\":\"...\",\"answer\":\"...\"}]")),
             qPrintable(addCardsHelp));

    // Preview help names the supported math delimiters before a user guesses
    // Markdown-style single-dollar math.
    int previewCode = -1;
    const QString previewHelp =
        QString::fromUtf8(runCliProcessRaw({"preview", "--help"}, decks, database, &previewCode));
    QCOMPARE(previewCode, 0);
    QVERIFY2(previewHelp.contains(QStringLiteral("Math: supported $$...$$ and \\(...\\); $...$ stays literal.")),
             qPrintable(previewHelp));

    QVERIFY2(!QFileInfo::exists(decks) || QDir(decks).entryList(QDir::NoDotAndDotDot).isEmpty(),
             "Help probes must not create a Deck");
    QVERIFY2(!QFileInfo::exists(database), "Help probes must not create the state database");

    // HOME alone controls the skill target. An isolated HOME keeps every probe
    // below away from the agent's real installed copy.
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    HomeGuard home(homeDir.path());
    const QString skillPath = homeDir.filePath(QStringLiteral(".agents/skills/omacards/SKILL.md"));
    QVERIFY2(!QFileInfo::exists(skillPath), "The isolated HOME must start without an installed skill");

    // Bare `skill` is a probe: it reports status and help, and installs nothing.
    int skillCode = -1;
    QByteArray skillErrors;
    const QByteArray bareSkill = runCliProcessRaw({"skill"}, decks, database, &skillCode, &skillErrors);
    QCOMPARE(skillCode, 0);
    QVERIFY2(skillErrors.isEmpty(), qPrintable(skillErrors));
    const QString bareText = QString::fromUtf8(bareSkill);
    QVERIFY2(bareText.contains(QStringLiteral("Skill installed: no")), qPrintable(bareText));
    QVERIFY2(bareText.contains(skillPath), qPrintable(bareText));
    QVERIFY2(bareText.contains(QStringLiteral("Purpose:")), qPrintable(bareText));
    QVERIFY2(bareText.contains(QStringLiteral("Flags:")), qPrintable(bareText));
    QVERIFY2(bareText.contains(QStringLiteral("Example:")), qPrintable(bareText));
    QVERIFY2(!QFileInfo::exists(skillPath), "Bare skill must not install the skill");

    // JSON status stays parseable and names the same target.
    const QJsonObject status = runCliProcess({"skill", "--json"}, decks, database, &skillCode, &skillErrors);
    QCOMPARE(skillCode, 0);
    QCOMPARE(sortedKeys(status), QStringList({"installed", "matches", "ok", "path"}));
    QVERIFY(status.value(QStringLiteral("ok")).toBool());
    QCOMPARE(status.value(QStringLiteral("path")).toString(), skillPath);
    QVERIFY(!status.value(QStringLiteral("installed")).toBool());
    QVERIFY(!QFileInfo::exists(skillPath));
    QVERIFY2(!QFileInfo::exists(database), "Skill probes must not initialize the state database");

    // Skill help explains where the copy goes, that installs are idempotent,
    // how a changed generated copy is treated, and when the agent reloads it.
    for (const QString &command : {QStringLiteral("skill"), QStringLiteral("skill install")}) {
        for (const QString &flag : {QStringLiteral("--help"), QStringLiteral("-h")}) {
            QStringList arguments = command.split(QLatin1Char(' '));
            arguments << flag;
            int code = -1;
            QByteArray errors;
            const QString text = QString::fromUtf8(runCliProcessRaw(arguments, decks, database, &code, &errors));
            QCOMPARE(code, 0);
            QVERIFY2(errors.isEmpty(), qPrintable(errors));
            QVERIFY2(text.contains(skillPath), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("idempotent"), Qt::CaseInsensitive), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("overwritten"), Qt::CaseInsensitive), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("Restart the agent"), Qt::CaseInsensitive), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("`ok`, `installed`, `path` and `changed`")), qPrintable(text));
            QVERIFY2(text.contains(QStringLiteral("`changed` is false when the installed copy already matches")),
                     qPrintable(text));
        }
    }
    QVERIFY2(!QFileInfo::exists(skillPath), "Help probes must not install the skill");
    QVERIFY2(!QFileInfo::exists(database), "Skill help probes must not initialize the state database");

    // A blocked target keeps the two-key failure shape from issue #107.
    const QString agentsDir = homeDir.filePath(QStringLiteral(".agents"));
    QVERIFY(writeFile(agentsDir, "not a directory"));
    const QJsonObject failure =
        runCliProcess({"skill", "install", "--json"}, decks, database, &skillCode, &skillErrors);
    QCOMPARE(skillCode, 1);
    QCOMPARE(sortedKeys(failure), QStringList({"error", "ok"}));
    QVERIFY(!failure.value(QStringLiteral("ok")).toBool());
    QVERIFY2(failure.value(QStringLiteral("error")).toString().contains(skillPath),
             qPrintable(failure.value(QStringLiteral("error")).toString()));
    QVERIFY2(skillErrors.isEmpty(), qPrintable(skillErrors));
    QVERIFY2(!QFileInfo::exists(skillPath), "A failed install must not create the skill");

    QVERIFY(QFile::remove(agentsDir));
    const QJsonObject installed =
        runCliProcess({"skill", "install", "--json"}, decks, database, &skillCode, &skillErrors);
    QCOMPARE(skillCode, 0);
    QCOMPARE(sortedKeys(installed), QStringList({"changed", "installed", "ok", "path"}));
    QVERIFY(installed.value(QStringLiteral("ok")).toBool());
    QVERIFY(installed.value(QStringLiteral("installed")).toBool());
    QCOMPARE(installed.value(QStringLiteral("path")).toString(), skillPath);
    QVERIFY(installed.value(QStringLiteral("changed")).toBool());
    QVERIFY(QFileInfo::exists(skillPath));
    QFile installedSkill(skillPath);
    QFile bundledSkill(QStringLiteral(":/skill/SKILL.md"));
    QVERIFY(installedSkill.open(QIODevice::ReadOnly));
    QVERIFY(bundledSkill.open(QIODevice::ReadOnly));
    QCOMPARE(installedSkill.readAll(), bundledSkill.readAll());

    // Reinstalling a matching copy reports it as installed but not changed.
    const QJsonObject unchanged =
        runCliProcess({"skill", "install", "--json"}, decks, database, &skillCode, &skillErrors);
    QCOMPARE(skillCode, 0);
    QCOMPARE(sortedKeys(unchanged), QStringList({"changed", "installed", "ok", "path"}));
    QVERIFY(unchanged.value(QStringLiteral("ok")).toBool());
    QVERIFY(unchanged.value(QStringLiteral("installed")).toBool());
    QCOMPARE(unchanged.value(QStringLiteral("path")).toString(), skillPath);
    QVERIFY(!unchanged.value(QStringLiteral("changed")).toBool());

    // A help flag is never a positional name and never runs the command.
    int code = -1;
    const QByteArray checkHelp = runCliProcessRaw({"check", "--help", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(checkHelp.contains("Purpose:"));
    QVERIFY(!checkHelp.contains("\"decks\""));

    const QByteArray topLevel = runCliProcessRaw({"help"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QByteArray topLevelWithFlag = runCliProcessRaw({"help", "--help"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(topLevelWithFlag, topLevel);
}

void TestCli::helpCommandDispatchesToCommandHelp() {
    if (!QFileInfo::exists(appBinary()))
        QSKIP("Build the app before running CLI integration tests.");

    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");

    int code = -1;
    QByteArray errors;
    const QString addCardHelp = QString::fromUtf8(runCliRaw({"help", "add-card"}, decks, database, &code, &errors));
    QCOMPARE(code, 0);
    QVERIFY2(errors.isEmpty(), qPrintable(errors));
    QVERIFY2(addCardHelp.contains(QStringLiteral("Purpose: Add a Card to a Deck.")), qPrintable(addCardHelp));
    QVERIFY2(!addCardHelp.contains(QStringLiteral("Usage:")), qPrintable(addCardHelp));

    // Every registry path reached through `help` matches its own help flag.
    for (const QString &path : Cli::commandPaths()) {
        if (Cli::isHelpCommand(path.section(QLatin1Char(' '), 0, 0)))
            continue;
        QStringList helpArguments = {QStringLiteral("help")};
        helpArguments << path.split(QLatin1Char(' '));
        QStringList flagArguments = path.split(QLatin1Char(' '));
        flagArguments << QStringLiteral("--help");
        int helpCode = -1;
        int flagCode = -1;
        QByteArray helpErrors;
        const QByteArray viaHelp = runCliRaw(helpArguments, decks, database, &helpCode, &helpErrors);
        const QByteArray viaFlag = runCliRaw(flagArguments, decks, database, &flagCode);
        QCOMPARE(helpCode, 0);
        QVERIFY2(helpErrors.isEmpty(), qPrintable(path + ": " + QString::fromUtf8(helpErrors)));
        QCOMPARE(flagCode, 0);
        QCOMPARE(QString::fromUtf8(viaHelp), QString::fromUtf8(viaFlag));
    }
    QVERIFY2(!QFileInfo::exists(database), "Help dispatch must not create the state database");
}

void TestCli::skillUnknownActionMentionsBothPaths() {
    if (!QFileInfo::exists(appBinary()))
        QSKIP("Build the app before running CLI integration tests.");

    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");

    int code = -1;
    QByteArray errors;
    runCliProcessRaw({"skill", "bogus"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QVERIFY2(errors.contains("Usage: omacards skill install"), qPrintable(errors));
    QVERIFY2(errors.contains("omacards skill status"), qPrintable(errors));

    const QJsonObject failure = runCliProcess({"skill", "bogus", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    const QString message = failure.value(QStringLiteral("error")).toString();
    QVERIFY2(message.contains("skill install"), qPrintable(message));
    QVERIFY2(message.contains("skill status"), qPrintable(message));
}

void TestCli::missingSkillNudge() {
    if (!QFileInfo::exists(appBinary()))
        QSKIP("Build the app before running CLI integration tests.");

    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    HomeGuard home(homeDir.path());
    const QString skillPath = homeDir.filePath(QStringLiteral(".agents/skills/omacards/SKILL.md"));
    QVERIFY2(!QFileInfo::exists(skillPath), "The isolated HOME must start without an installed skill");

    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");

    // `omacards help` advertises the skill and nudges on stderr while missing.
    int code = -1;
    QByteArray errors;
    const QByteArray help = runCliProcessRaw({"help"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    const QString helpText = QString::fromUtf8(help);
    QVERIFY2(helpText.contains(QStringLiteral("Agent integration")), qPrintable(helpText));
    QVERIFY2(helpText.contains(QStringLiteral("omacards skill install")), qPrintable(helpText));
    QVERIFY2(helpText.contains(skillPath), qPrintable(helpText));

    const QString nudge = QString::fromUtf8(errors).trimmed();
    QVERIFY2(!nudge.isEmpty(), "A missing skill must produce a stderr nudge");
    QVERIFY2(!nudge.contains(QLatin1Char('\n')), qPrintable(nudge));
    QVERIFY2(nudge.contains(QStringLiteral("omacards skill install")), qPrintable(nudge));
    QVERIFY2(nudge.contains(skillPath), qPrintable(nudge));

    // Help flags and JSON invocations stay clean: neither ever nudges.
    for (const QString &flag : {QStringLiteral("--help"), QStringLiteral("-h")}) {
        int flagCode = -1;
        QByteArray flagErrors;
        runCliProcessRaw({flag}, decks, database, &flagCode, &flagErrors);
        QCOMPARE(flagCode, 0);
        QVERIFY2(flagErrors.isEmpty(), qPrintable(flagErrors));
    }
    int jsonCode = -1;
    QByteArray jsonErrors;
    runCliProcessRaw({"help", "--json"}, decks, database, &jsonCode, &jsonErrors);
    QCOMPARE(jsonCode, 0);
    QVERIFY2(jsonErrors.isEmpty(), qPrintable(jsonErrors));

    // Once installed, the same help invocation stays quiet but keeps the pointer.
    int installCode = -1;
    QByteArray installErrors;
    const QJsonObject installed =
        runCliProcess({"skill", "install", "--json"}, decks, database, &installCode, &installErrors);
    QCOMPARE(installCode, 0);
    QVERIFY(installed.value(QStringLiteral("ok")).toBool());
    QVERIFY(QFileInfo::exists(skillPath));

    int afterCode = -1;
    QByteArray afterErrors;
    const QByteArray helpAfter = runCliProcessRaw({"help"}, decks, database, &afterCode, &afterErrors);
    QCOMPARE(afterCode, 0);
    QCOMPARE(QString::fromUtf8(helpAfter), helpText);
    QVERIFY2(afterErrors.isEmpty(), qPrintable(afterErrors));
}

void TestCli::skillStatusReportsInstallState() {
    if (!QFileInfo::exists(appBinary()))
        QSKIP("Build the app before running CLI integration tests.");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    EnvGuard env(decks, database);
    HomeGuard home(homeDir.path());
    const QString skillPath = homeDir.filePath(QStringLiteral(".agents/skills/omacards/SKILL.md"));
    QVERIFY2(!QFileInfo::exists(skillPath), "The isolated HOME must start without an installed skill");

    int code = -1;
    QByteArray errors;

    // Before install: absent, no match, the isolated target, four keys, no writes.
    const QJsonObject absent = runCli({"skill", "status", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QVERIFY2(errors.isEmpty(), qPrintable(errors));
    QCOMPARE(sortedKeys(absent), QStringList({"installed", "matches", "ok", "path"}));
    QVERIFY(absent.value(QStringLiteral("ok")).toBool());
    QCOMPARE(absent.value(QStringLiteral("path")).toString(), skillPath);
    QVERIFY(!absent.value(QStringLiteral("installed")).toBool());
    QVERIFY(!absent.value(QStringLiteral("matches")).toBool());
    QVERIFY2(!QFileInfo::exists(skillPath), "Status must not install the skill");
    QVERIFY2(!QFileInfo::exists(database), "Status must not initialize the state database");

    // Text mode names the install state and the target path without installing.
    const QString absentText = QString::fromUtf8(runCliRaw({"skill", "status"}, decks, database, &code, &errors));
    QCOMPARE(code, 0);
    QVERIFY2(errors.isEmpty(), qPrintable(errors));
    QVERIFY2(absentText.contains(QStringLiteral("Skill installed: no")), qPrintable(absentText));
    QVERIFY2(absentText.contains(skillPath), qPrintable(absentText));
    QVERIFY2(!QFileInfo::exists(skillPath), "Status must not install the skill");

    // After install: installed and matching, with the same four keys.
    runCli({"skill", "install", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QVERIFY(QFileInfo::exists(skillPath));
    const QJsonObject present = runCli({"skill", "status", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QVERIFY2(errors.isEmpty(), qPrintable(errors));
    QCOMPARE(sortedKeys(present), QStringList({"installed", "matches", "ok", "path"}));
    QVERIFY(present.value(QStringLiteral("ok")).toBool());
    QCOMPARE(present.value(QStringLiteral("path")).toString(), skillPath);
    QVERIFY(present.value(QStringLiteral("installed")).toBool());
    QVERIFY(present.value(QStringLiteral("matches")).toBool());

    // A changed installed copy is reported as installed but not matching.
    QVERIFY(writeFile(skillPath, "not the bundled skill"));
    const QJsonObject changed = runCli({"skill", "status", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QVERIFY2(errors.isEmpty(), qPrintable(errors));
    QVERIFY(changed.value(QStringLiteral("installed")).toBool());
    QVERIFY(!changed.value(QStringLiteral("matches")).toBool());

    // JSON failures around the new path keep the two-key error/ok shape.
    const QJsonObject failed = runCli({"skill", "status", "--unknown-flag", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(failed), QStringList({"error", "ok"}));
    QVERIFY(!failed.value(QStringLiteral("ok")).toBool());
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
    const QJsonObject throughStdin = runCliProcess({"add-cards", "--deck", "Piped Series", "--input", "-", "--json"},
                                                   decks, database, &code, nullptr, &piped);
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

    // edit-deck changes the description through the process boundary without
    // adding, removing or re-id'ing a Card.
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    int code = -1;
    const QJsonObject added =
        runCli({"add-card", "--deck", "Imported", "-q", "Q?", "-a", "A", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QString cardId = added.value(QStringLiteral("id")).toString();
    QVERIFY(!cardId.isEmpty());
    error.clear();
    const Deck beforeEdit = DeckIO::load(QDir(decks).filePath("Imported.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QStringList idsBefore;
    for (const Card &card : beforeEdit.cards)
        idsBefore << card.id;
    idsBefore.sort();

    const QJsonObject edited =
        runCli({"edit-deck", "Imported", "--description", "Fresh description", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(edited.value(QStringLiteral("ok")).toBool());
    QCOMPARE(sortedKeys(edited), QStringList({"deck", "description", "ok", "path"}));
    QCOMPARE(edited.value(QStringLiteral("deck")).toString(), QString("Imported"));
    QCOMPARE(edited.value(QStringLiteral("description")).toString(), QString("Fresh description"));
    QCOMPARE(edited.value(QStringLiteral("path")).toString(), QDir(decks).filePath("Imported.txt"));

    error.clear();
    const Deck reread = DeckIO::load(QDir(decks).filePath("Imported.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(reread.description, QString("Fresh description"));
    QStringList idsAfter;
    for (const Card &card : reread.cards)
        idsAfter << card.id;
    idsAfter.sort();
    QCOMPARE(idsAfter, idsBefore);
    QVERIFY(idsAfter.contains(cardId));
    const QJsonObject cards = runCli({"cards", "--deck", "Imported", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray items = cards.value(QStringLiteral("cards")).toArray();
    QCOMPARE(items.size(), idsBefore.size());

    const QString confirmation =
        runCliText({"edit-deck", "Imported", "--description", "Terse"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(confirmation.contains(QStringLiteral("Imported")));
    QCOMPARE(confirmation.trimmed().count('\n'), 0);

    QFile deckFile(QDir(decks).filePath("Imported.txt"));
    QVERIFY(deckFile.open(QIODevice::ReadOnly));
    const QByteArray before = deckFile.readAll();
    deckFile.close();
    const QJsonObject missing = runCli({"edit-deck", "Ghost", "--description", "x", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(missing), QStringList({"error", "ok"}));
    QCOMPARE(missing.value(QStringLiteral("error")).toString(),
             QString("No Deck named Ghost. Run 'omacards decks' to see available Decks."));
    QVERIFY(!missing.value(QStringLiteral("ok")).toBool());
    QVERIFY(deckFile.open(QIODevice::ReadOnly));
    QCOMPARE(deckFile.readAll(), before);
    deckFile.close();

    const QJsonObject noText = runCli({"edit-deck", "Imported", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QVERIFY(noText.value(QStringLiteral("error")).toString().contains(QStringLiteral("--description")));
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
    // preserves it without adding another id line; it still counts as preserved.
    QCOMPARE(renamed.value(QStringLiteral("preservedIds")).toInt(), 1);
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

    const QString markdownInput = dir.filePath("markdown.json");
    QVERIFY(writeFile(markdownInput, R"json({"cards":[{"question":"Expand \\(x^2\\)",
                                                       "answer":"line one\n$$y$$ and \\(z\\) and $x$"}]})json"));
    runCli({"add-cards", "-d", "Markdown", "-i", markdownInput, "--json"}, dir.filePath("decks"),
           dir.filePath("state.db"), &code);
    QCOMPARE(code, 0);

    const QJsonObject markdown = runCli({"preview", "--deck", "Markdown", "--format", "markdown", "--json"},
                                        dir.filePath("decks"), dir.filePath("state.db"), &code);
    QCOMPARE(code, 0);
    QCOMPARE(markdown.value(QStringLiteral("format")).toString(), QString("markdown"));
    const QJsonArray markdownItems = markdown.value(QStringLiteral("items")).toArray();
    QCOMPARE(markdownItems.size(), 1);
    const QJsonObject markdownItem = markdownItems.at(0).toObject();
    QCOMPARE(sortedKeys(markdownItem), QStringList({"answer", "id", "question"}));
    QCOMPARE(markdownItem.value(QStringLiteral("question")).toString(), QString("Expand \\(x^2\\)"));
    QCOMPARE(markdownItem.value(QStringLiteral("answer")).toString(), QString("line one\n$$y$$ and \\(z\\) and $x$"));
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

void TestCli::checkMalformedContinuationFails() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    // One leading space makes load() reject the line, so the Deck disappears
    // from decks --json. check must call that out as an Error, not a Warning.
    const QString path = QDir(decks).filePath("Hidden.txt");
    QVERIFY(writeFile(path, R"deck(# Deck: Hidden
---
Q: First line
 second line
A: answer
)deck"));

    int code = -1;
    const QJsonObject result = runCli({"check", "--json"}, decks, database, &code);
    QVERIFY(!result.value(QStringLiteral("ok")).toBool());
    const QJsonObject counts = result.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 1);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 0);

    const QJsonArray issues = result.value(QStringLiteral("issues")).toArray();
    QCOMPARE(issues.size(), 1);
    const QJsonObject issue = issues.first().toObject();
    QCOMPARE(issue.value(QStringLiteral("path")).toString(), path);
    QCOMPARE(issue.value(QStringLiteral("line")).toInt(), 4);
    QCOMPARE(issue.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QCOMPARE(issue.value(QStringLiteral("message")).toString(),
             QStringLiteral("Continuation line has one leading space"));

    const QJsonObject strict = runCli({"check", "--strict", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QVERIFY(!strict.value(QStringLiteral("ok")).toBool());
    QCOMPARE(strict.value(QStringLiteral("strictPassed")), QJsonValue(false));

    const QJsonObject listed = runCli({"decks", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(listed.value(QStringLiteral("decks")).toArray().size(), 0);
}

void TestCli::checkDuplicateQuestionLinesStayPinned() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    const QString path = QDir(decks).filePath("Pinned.txt");
    QVERIFY(writeFile(path, R"deck(# Deck: Pinned
---
id: alpha-first
Q: alpha
A: one
---
id: beta-first
Q: beta \(\foo{x}\)
A: two
---
id: alpha-second
Q: alpha
A: three
---
id: beta-second
Q: beta
A: four \(\foo{y}\)
---
id: beta-third
Q: beta
A: five
)deck"));

    int code = -1;
    const QJsonObject result = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(result.value(QStringLiteral("ok")).toBool());

    const QString mathHint = QStringLiteral("Use supported macros, or install tectonic for LaTeX rendering.");
    const auto issue = [&path, &mathHint](const QString &card, int line, const QString &message, const QString &hint) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), path);
        object.insert(QStringLiteral("card"), card);
        object.insert(QStringLiteral("line"), line);
        object.insert(QStringLiteral("severity"), QStringLiteral("warning"));
        object.insert(QStringLiteral("message"), message);
        object.insert(QStringLiteral("hint"), hint);
        return object;
    };
    QJsonArray expected;
    expected << issue(QStringLiteral("beta-first"), 8,
                      QStringLiteral("Math warning in question: Unsupported math command: \\foo"), mathHint);
    expected << issue(QStringLiteral("alpha-second"), 12, QStringLiteral("Duplicate question in deck"),
                      QStringLiteral("Question already used on line 4; make it distinct."));
    expected << issue(QStringLiteral("beta-second"), 16,
                      QStringLiteral("Math warning in answer: Unsupported math command: \\foo"), mathHint);
    expected << issue(QStringLiteral("beta-third"), 20, QStringLiteral("Duplicate question in deck"),
                      QStringLiteral("Question already used on line 16; make it distinct."));

    const QJsonArray issues = result.value(QStringLiteral("issues")).toArray();
    QCOMPARE(QJsonDocument(issues).toJson(QJsonDocument::Compact),
             QJsonDocument(expected).toJson(QJsonDocument::Compact));
    const QJsonObject counts = result.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(sortedKeys(counts), QStringList({"error", "warning"}));
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 4);

    const QByteArray human = runCliRaw({"check"}, decks, database, &code);
    QCOMPARE(code, 0);
    for (const QJsonValue &value : expected) {
        const QByteArray line = checkDeckIssueLine(value.toObject()).toUtf8();
        QVERIFY2(human.contains(line), line.constData());
    }
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
    QVERIFY(Cli::commandPaths().contains(QStringLiteral("decks current")));

    QVERIFY(!Cli::needsOffscreen({}));
    QVERIFY(!Cli::needsOffscreen({""}));
    QVERIFY(!Cli::needsOffscreen({"open"}));
    QVERIFY(!Cli::needsOffscreen({"open", "check"}));
    // Unknown commands are headless too, so they fail fast instead of opening
    // a GUI event loop that never returns.
    QVERIFY(Cli::needsOffscreen({"banana"}));
    QVERIFY(Cli::needsOffscreen({"banana", "--json"}));
    QVERIFY(Cli::needsOffscreen({"check", "open"}));

    // A help flag routes every registry path through the CLI, including the
    // GUI command, so probing syntax cannot start an event loop.
    for (const QString &path : Cli::commandPaths()) {
        const QString token = path.section(' ', 0, 0);
        QVERIFY2(Cli::needsOffscreen({token, "--help"}), qPrintable(path));
        QVERIFY2(Cli::needsOffscreen({token, "-h"}), qPrintable(path));
    }
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

    const QJsonObject strictClean = runCli({"check", "--strict", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(strictClean.value(QStringLiteral("strictPassed")), QJsonValue(true));

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
    const QJsonObject strictWarned = runCli({"check", "--strict", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(strictWarned.value(QStringLiteral("strictPassed")), QJsonValue(false));
    QVERIFY(strictWarned.value(QStringLiteral("ok")).toBool());
}

void TestCli::summaryContractAcrossThreeStudyDays() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    const int dayStart = 4;
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck("Summary", &error));
        QVERIFY(owned->createCard("Summary", "Q", "A", "basic", {}, &error));
        owned->setDayStartHour(dayStart);
    }

    const QDate today = Config::studyDay(QDateTime::currentMSecsSinceEpoch(), dayStart);
    const auto atOnDay = [&today](int offset) {
        return QDateTime(today.addDays(offset), QTime(12, 0)).toMSecsSinceEpoch();
    };
    {
        Store store(database);
        QString error;
        QVERIFY(store.open(&error));
        const FsrsState state = Fsrs::review(Fsrs::initial(), 3, atOnDay(0)).state;
        store.recordRating(QStringLiteral("today-good"), 3, atOnDay(0), state, 1, true, QStringLiteral("Summary"));
        store.recordRating(QStringLiteral("today-again"), 1, atOnDay(0) + 1000, state, 1, false,
                           QStringLiteral("Summary"));
        store.recordRating(QStringLiteral("today-easy"), 4, atOnDay(0) + 2000, state, 1, false,
                           QStringLiteral("Summary"));
        store.recordRating(QStringLiteral("yesterday-good"), 3, atOnDay(-1), state, 1, true, QStringLiteral("Summary"));
        store.recordRating(QStringLiteral("older-again"), 1, atOnDay(-2), state, 1, false, QStringLiteral("Summary"));
        QVERIFY(store.lastError().isEmpty());
    }

    auto backend = Backend::fromEnvironment();
    QString error;
    QVERIFY(backend->initialize(&error));
    QCOMPARE(backend->dayStartHour(), dayStart);
    const ReviewSummary summary = backend->reviewSummary();
    QCOMPARE(summary.total, 5);
    QCOMPARE(summary.studyDayRatings, 3);
    QCOMPARE(summary.studyDayGood, 1);
    QCOMPARE(summary.streak, 3);
    QCOMPARE(summary.newCardsOnStudyDay, 1);
    QCOMPARE(backend->streak(), summary.streak);
    const QVariantMap qmlStats = backend->statsSummary();
    QCOMPARE(qmlStats.value(QStringLiteral("totalRatings")).toInt(), summary.total);
    QCOMPARE(qmlStats.value(QStringLiteral("studyDayRatings")).toInt(), summary.studyDayRatings);
    QCOMPARE(qmlStats.value(QStringLiteral("streak")).toInt(), summary.streak);
    QCOMPARE(qmlStats.value(QStringLiteral("newCardsOnStudyDay")).toInt(), summary.newCardsOnStudyDay);
    QCOMPARE(qmlStats.value(QStringLiteral("dayStartHour")).toInt(), dayStart);
    backend.reset();

    int code = -1;
    const QJsonObject stats = runCli({"stats", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expectedStats{
        "ok",  "totalRatings", "studyDayRatings", "studyDayGood", "streak", "newCardsOnStudyDay", "newCardAllowance",
        "due", "totalDue",     "cards",           "decks"};
    expectedStats.sort();
    QCOMPARE(sortedKeys(stats), expectedStats);
    QCOMPARE(stats.value(QStringLiteral("totalRatings")).toInt(), summary.total);
    QCOMPARE(stats.value(QStringLiteral("studyDayRatings")).toInt(), summary.studyDayRatings);
    QCOMPARE(stats.value(QStringLiteral("studyDayGood")).toInt(), summary.studyDayGood);
    QCOMPARE(stats.value(QStringLiteral("streak")).toInt(), summary.streak);
    QCOMPARE(stats.value(QStringLiteral("newCardsOnStudyDay")).toInt(), summary.newCardsOnStudyDay);

    const QJsonObject check = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expectedCheck{"ok",
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
    expectedCheck.sort();
    QCOMPARE(sortedKeys(check), expectedCheck);
    QCOMPARE(check.value(QStringLiteral("newCardsOnStudyDay")).toInt(), summary.newCardsOnStudyDay);
    QCOMPARE(check.value(QStringLiteral("dayStartHour")).toInt(), dayStart);
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
    QCOMPARE(missing.value(QStringLiteral("error")).toString(),
             QString("No Deck named Unknown. Run 'omacards decks' to see available Decks."));

    const QJsonObject all = runCli({"check", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(all.value(QStringLiteral("decks")).toArray().size(), 2);
    QVERIFY(!all.contains(QStringLiteral("deck")));
    QVERIFY(all.value(QStringLiteral("issues")).toArray().size() >= 2);
}

void TestCli::checkPreviewWarmIssueReportsOnlyTargetDeck() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);

    const QDir deckDir(decks);
    const QString duplicate = QStringLiteral("# Deck: %1\n---\nQ: same\nA: one\n---\nQ: same\nA: two\n");
    const QString emptyWithIssues = QStringLiteral("# Deck: %1\n---\noops: not a key\n   too much indent\n");
    QVERIFY(writeFile(deckDir.filePath("Target.txt"), duplicate.arg("Target").toUtf8()));
    QVERIFY(writeFile(deckDir.filePath("Other.txt"), duplicate.arg("Other").toUtf8()));
    QVERIFY(writeFile(deckDir.filePath("EmptyTarget.txt"), emptyWithIssues.arg("EmptyTarget").toUtf8()));
    QVERIFY(writeFile(deckDir.filePath("EmptyOther.txt"), emptyWithIssues.arg("EmptyOther").toUtf8()));
    QVERIFY(writeFile(deckDir.filePath("Clean.txt"), "# Deck: Clean\n---\nQ: clean\nA: ok\n"));
    // The header name differs from the filename, so this Deck resolves by the
    // .deck fallback rather than through the loaded Deck list.
    QVERIFY(writeFile(deckDir.filePath("Fallback.deck"), duplicate.arg("Hidden").toUtf8()));

    int code = -1;
    const QJsonObject target = runCli({"check", "--deck", "Target", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(target.value(QStringLiteral("ok")).toBool());
    const QJsonArray targetIssues = target.value(QStringLiteral("issues")).toArray();
    QCOMPARE(targetIssues.size(), 1);
    const QJsonObject targetIssue = targetIssues.at(0).toObject();
    QCOMPARE(targetIssue.value(QStringLiteral("path")).toString(), deckDir.filePath("Target.txt"));
    QCOMPARE(targetIssue.value(QStringLiteral("message")).toString(), QStringLiteral("Duplicate question in deck"));
    const QJsonObject targetCounts = target.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(targetCounts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(targetCounts.value(QStringLiteral("warning")).toInt(), 1);

    const QJsonObject strictTarget =
        runCli({"check", "--deck", "Target", "--strict", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(strictTarget.value(QStringLiteral("strictPassed")), QJsonValue(false));
    const QJsonObject strictClean = runCli({"check", "--deck", "Clean", "--strict", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(strictClean.value(QStringLiteral("strictPassed")), QJsonValue(true));
    QVERIFY(strictClean.value(QStringLiteral("issues")).toArray().isEmpty());

    QByteArray errors;
    const QString targetText = runCliText({"check", "--deck", "Target"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QVERIFY(errors.isEmpty());
    QVERIFY(targetText.contains(checkDeckIssueLine(targetIssue)));
    QVERIFY(!targetText.contains(QStringLiteral("Other.txt")));
    QVERIFY(!targetText.contains(QStringLiteral("EmptyTarget.txt")));

    // The empty-Deck reports from preview and warm keep the check report's
    // fields, counts, text lines and hint, without other Decks' issues.
    const QJsonObject emptyCheck = runCli({"check", "--deck", "EmptyTarget", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray emptyIssues = emptyCheck.value(QStringLiteral("issues")).toArray();
    QVERIFY(emptyIssues.size() >= 2);
    for (const QJsonValue &value : emptyIssues) {
        const QJsonObject issue = value.toObject();
        QCOMPARE(issue.value(QStringLiteral("path")).toString(), deckDir.filePath("EmptyTarget.txt"));
    }

    const QStringList failureKeys{"deck", "error", "hint", "issueCounts", "issues", "ok"};
    const QJsonObject preview = runCli({"preview", "--deck", "EmptyTarget", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QVERIFY(errors.isEmpty());
    QCOMPARE(sortedKeys(preview), failureKeys);
    QCOMPARE(preview.value(QStringLiteral("error")).toString(),
             expectedUnreadableDeckError(QStringLiteral("EmptyTarget"), deckDir.filePath("EmptyTarget.txt")));
    QCOMPARE(preview.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck EmptyTarget"));
    QCOMPARE(QJsonDocument(preview.value(QStringLiteral("issues")).toArray()).toJson(QJsonDocument::Compact),
             QJsonDocument(emptyIssues).toJson(QJsonDocument::Compact));
    QCOMPARE(QJsonDocument(preview.value(QStringLiteral("issueCounts")).toObject()).toJson(),
             QJsonDocument(emptyCheck.value(QStringLiteral("issueCounts")).toObject()).toJson());

    QStringList emptyLines{
        expectedUnreadableDeckError(QStringLiteral("EmptyTarget"), deckDir.filePath("EmptyTarget.txt"))};
    for (const QJsonValue &value : emptyIssues)
        emptyLines << checkDeckIssueLine(value.toObject());
    emptyLines << QStringLiteral("omacards check --deck EmptyTarget");
    const QString previewText = runCliText({"preview", "--deck", "EmptyTarget"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QVERIFY(previewText.isEmpty());
    QCOMPARE(QString::fromUtf8(errors), emptyLines.join('\n') + '\n');

    const QJsonObject warm = runCli({"warm", "--deck", "EmptyTarget", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QVERIFY(errors.isEmpty());
    QCOMPARE(sortedKeys(warm), failureKeys);
    QCOMPARE(warm.value(QStringLiteral("error")).toString(),
             expectedUnreadableDeckError(QStringLiteral("EmptyTarget"), deckDir.filePath("EmptyTarget.txt")));
    QCOMPARE(QJsonDocument(warm.value(QStringLiteral("issues")).toArray()).toJson(QJsonDocument::Compact),
             QJsonDocument(emptyIssues).toJson(QJsonDocument::Compact));
    const QString warmText = runCliText({"warm", "--deck", "EmptyTarget"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QVERIFY(warmText.isEmpty());
    QCOMPARE(QString::fromUtf8(errors), emptyLines.join('\n') + '\n');

    // An existing empty Deck still wins over a stale --card id, while a miss
    // on a Deck with Cards and a missing Deck keep their old messages.
    const QJsonObject filtered =
        runCli({"preview", "--deck", "EmptyTarget", "--card", "nope", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QCOMPARE(filtered.value(QStringLiteral("error")).toString(),
             expectedUnreadableDeckError(QStringLiteral("EmptyTarget"), deckDir.filePath("EmptyTarget.txt")));
    QCOMPARE(QJsonDocument(filtered.value(QStringLiteral("issues")).toArray()).toJson(QJsonDocument::Compact),
             QJsonDocument(emptyIssues).toJson(QJsonDocument::Compact));
    const QJsonObject cardMiss =
        runCli({"preview", "--deck", "Target", "--card", "nope", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(cardMiss), QStringList({"error", "ok"}));
    QCOMPARE(cardMiss.value(QStringLiteral("error")).toString(), QStringLiteral("No such card: nope"));
    const QJsonObject missing = runCli({"preview", "--deck", "Missing", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(missing), QStringList({"error", "ok"}));
    QCOMPARE(missing.value(QStringLiteral("error")).toString(),
             QStringLiteral("No Deck named Missing. Run 'omacards decks' to see available Decks."));

    const QJsonObject warmed = runCli({"warm", "--deck", "Target", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QCOMPARE(warmed.value(QStringLiteral("deck")).toString(), QStringLiteral("Target"));
    const QString warmSuccess = runCliText({"warm", "--deck", "Target"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QCOMPARE(warmSuccess, QStringLiteral("Warmed Target: 0 rendered · 0 cached\n"));

    // The resolved .deck path is the path lintFile reports, even when the
    // header name does not match the filename.
    const QJsonObject fallback = runCli({"check", "--deck", "Fallback", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray fallbackIssues = fallback.value(QStringLiteral("issues")).toArray();
    QCOMPARE(fallbackIssues.size(), 1);
    QCOMPARE(fallbackIssues.at(0).toObject().value(QStringLiteral("path")).toString(),
             deckDir.filePath("Fallback.deck"));

    // A relative decks directory resolves to the same relative issue path.
    QTemporaryDir relative(QDir::current().filePath(QStringLiteral("check-scope-relative-XXXXXX")));
    QVERIFY(relative.isValid());
    const QString relativeDir = QDir::current().relativeFilePath(relative.path());
    QVERIFY(!QDir::isAbsolutePath(relativeDir));
    QVERIFY(writeFile(QDir(relativeDir).filePath("Relative.txt"), duplicate.arg("Relative").toUtf8()));
    const QJsonObject relativeReport = runCli({"check", "--deck", "Relative", "--json"}, relativeDir, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray relativeIssues = relativeReport.value(QStringLiteral("issues")).toArray();
    QCOMPARE(relativeIssues.size(), 1);
    QCOMPARE(relativeIssues.at(0).toObject().value(QStringLiteral("path")).toString(),
             QDir(relativeDir).filePath("Relative.txt"));
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
    runCli({"use-deck", "Alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
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
    QCOMPARE(sortedKeys(scoped), QStringList({"cards", "count", "ok"}));
    QVERIFY(scoped.value(QStringLiteral("ok")).toBool());
    QCOMPARE(scoped.value(QStringLiteral("count")).toInt(), 0);

    const QJsonObject due = runCli({"due", "--deck", "Alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(due), QStringList({"cards", "count", "ok", "totalDue"}));
    QCOMPARE(due.value(QStringLiteral("count")).toInt(), 2);
    QCOMPARE(due.value(QStringLiteral("totalDue")).toInt(), 2);

    const QJsonObject missingCards = runCli({"cards", "--deck", "Missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(missingCards), QStringList({"error", "ok"}));
    QVERIFY(!missingCards.value(QStringLiteral("ok")).toBool());
    QCOMPARE(missingCards.value(QStringLiteral("error")).toString(),
             QString("No Deck named Missing. Run 'omacards decks' to see available Decks."));

    const QJsonObject missingSearch =
        runCli({"search", "alpha", "--deck", "Missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(missingSearch), QStringList({"error", "ok"}));
    QVERIFY(!missingSearch.value(QStringLiteral("ok")).toBool());
    QCOMPARE(missingSearch.value(QStringLiteral("error")).toString(),
             QString("No Deck named Missing. Run 'omacards decks' to see available Decks."));

    const QJsonObject missingDue = runCli({"due", "--deck", "Missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(missingDue), QStringList({"error", "ok"}));
    QVERIFY(!missingDue.value(QStringLiteral("ok")).toBool());
    QCOMPARE(missingDue.value(QStringLiteral("error")).toString(),
             QString("No Deck named Missing. Run 'omacards decks' to see available Decks."));

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

    // The daily keys stay frozen while their values follow --deck. Alpha spent
    // its one New card; Beta has never started one today.
    const QJsonObject statsAlpha = runCli({"stats", "--deck", "Alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(statsAlpha), expectedStats);
    QCOMPARE(statsAlpha.value(QStringLiteral("newCardsOnStudyDay")).toInt(), 1);
    const QJsonObject statsBeta = runCli({"stats", "--deck", "Beta", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(statsBeta), expectedStats);
    QCOMPARE(statsBeta.value(QStringLiteral("newCardsOnStudyDay")).toInt(), 0);
    const QJsonObject checkBeta = runCli({"check", "--deck", "Beta", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(checkBeta.value(QStringLiteral("newCardsOnStudyDay")).toInt(), 0);

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

void TestCli::forecastReportsJsonTextAndDeckScope() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);
    const int dayStart = 4;
    const QDate today = Config::studyDay(QDateTime::currentMSecsSinceEpoch(), dayStart);
    const auto at = [&today](int offset) { return QDateTime(today.addDays(offset), QTime(12, 0)).toMSecsSinceEpoch(); };

    QHash<QString, QString> ids;
    QHash<QString, qint64> expectedDue;
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createDeck(QStringLiteral("Alpha"), &error));
        QVERIFY(owned->createDeck(QStringLiteral("Beta"), &error));
        for (const QString &question :
             {QStringLiteral("overdue"), QStringLiteral("today"), QStringLiteral("plus1"), QStringLiteral("plus20"),
              QStringLiteral("plus21"), QStringLiteral("plus95"), QStringLiteral("new")})
            QVERIFY(owned->createCard(QStringLiteral("Alpha"), question, QStringLiteral("A"), QStringLiteral("basic"),
                                      {}, &error));
        QVERIFY(owned->createCard(QStringLiteral("Beta"), QStringLiteral("beta-plus5"), QStringLiteral("A"),
                                  QStringLiteral("basic"), {}, &error));
        owned->setDayStartHour(dayStart);

        QHash<QString, Card> cards;
        for (const Deck &deck : owned->allDecks())
            for (const Card &card : deck.cards) {
                cards.insert(card.question, card);
                ids.insert(card.question, card.id);
            }
        const auto seed = [&](const QString &question, int offset, int reps) {
            const Card card = cards.value(question);
            QVERIFY(!card.id.isEmpty());
            FsrsState state;
            state.due = at(offset);
            state.reps = reps;
            state.stability = 2.0;
            state.difficulty = 5.0;
            state.lastReview = at(-1);
            owned->store().save(card.id, DeckIO::contentHash(card), state);
            expectedDue.insert(question, state.due);
        };
        seed(QStringLiteral("overdue"), -1, 2);
        seed(QStringLiteral("today"), 0, 3);
        seed(QStringLiteral("plus1"), 1, 2);
        seed(QStringLiteral("plus20"), 20, 3);
        seed(QStringLiteral("plus21"), 21, 3);
        seed(QStringLiteral("plus95"), 95, 3);
        seed(QStringLiteral("beta-plus5"), 5, 2);
        QVERIFY(owned->store().lastError().isEmpty());
        owned->refresh();
    }

    int code = -1;
    const QJsonObject forecast = runCli({"forecast", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(forecast), QStringList({"days", "deck", "ok", "slices", "windowDays"}));
    QVERIFY(forecast.value(QStringLiteral("ok")).toBool());
    QCOMPARE(forecast.value(QStringLiteral("deck")).toString(), QString());
    QCOMPARE(forecast.value(QStringLiteral("windowDays")).toInt(), 90);

    const QJsonArray days = forecast.value(QStringLiteral("days")).toArray();
    QCOMPARE(days.size(), 90);
    for (const QJsonValue &day : days)
        QCOMPARE(sortedKeys(day.toObject()), QStringList({"count", "date"}));
    QCOMPARE(days.at(0).toObject().value(QStringLiteral("date")).toString(),
             today.toString(QStringLiteral("yyyy-MM-dd")));
    QCOMPARE(days.at(89).toObject().value(QStringLiteral("date")).toString(),
             today.addDays(89).toString(QStringLiteral("yyyy-MM-dd")));
    const auto dayCount = [&days](int index) {
        return days.at(index).toObject().value(QStringLiteral("count")).toInt();
    };
    QCOMPARE(dayCount(0), 2); // overdue and current-day returns share the first cell
    QCOMPARE(dayCount(1), 1);
    QCOMPARE(dayCount(5), 1); // Beta is included by the all-Decks default
    QCOMPARE(dayCount(20), 1);
    QCOMPARE(dayCount(21), 1);
    QCOMPARE(dayCount(89), 0);

    const QJsonObject slices = forecast.value(QStringLiteral("slices")).toObject();
    QCOMPARE(sortedKeys(slices), QStringList({"learning", "mature", "new", "young"}));
    QCOMPARE(slices.value(QStringLiteral("new")).toInt(), 1);
    QCOMPARE(slices.value(QStringLiteral("learning")).toInt(), 2);
    QCOMPARE(slices.value(QStringLiteral("young")).toInt(), 3);
    QCOMPARE(slices.value(QStringLiteral("mature")).toInt(), 2);
    QCOMPARE(runCliText({"forecast"}, decks, database, &code).trimmed(),
             QStringLiteral("90-day forecast: 1 new · 2 learning · 3 young · 2 mature"));
    QCOMPARE(code, 0);

    const QJsonObject alpha = runCli({"forecast", "--deck", "Alpha", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(alpha), sortedKeys(forecast));
    QCOMPARE(alpha.value(QStringLiteral("deck")).toString(), QStringLiteral("Alpha"));
    const QJsonArray alphaDays = alpha.value(QStringLiteral("days")).toArray();
    QCOMPARE(alphaDays.at(0).toObject().value(QStringLiteral("count")).toInt(), 2);
    QCOMPARE(alphaDays.at(5).toObject().value(QStringLiteral("count")).toInt(), 0);
    const QJsonObject alphaSlices = alpha.value(QStringLiteral("slices")).toObject();
    QCOMPARE(alphaSlices.value(QStringLiteral("new")).toInt(), 1);
    QCOMPARE(alphaSlices.value(QStringLiteral("learning")).toInt(), 2);
    QCOMPARE(alphaSlices.value(QStringLiteral("young")).toInt(), 2);
    QCOMPARE(alphaSlices.value(QStringLiteral("mature")).toInt(), 2);

    const QJsonObject beta = runCli({"forecast", "--deck", "Beta", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(beta.value(QStringLiteral("deck")).toString(), QStringLiteral("Beta"));
    QCOMPARE(beta.value(QStringLiteral("days")).toArray().at(5).toObject().value(QStringLiteral("count")).toInt(), 1);
    const QJsonObject betaSlices = beta.value(QStringLiteral("slices")).toObject();
    QCOMPARE(betaSlices.value(QStringLiteral("young")).toInt(), 1);
    QCOMPARE(betaSlices.value(QStringLiteral("new")).toInt(), 0);

    // Forecast reads state only: no Scheduling write, Review entry or new Setting.
    Store stored(database);
    QVERIFY(stored.open());
    for (auto it = expectedDue.constBegin(); it != expectedDue.constEnd(); ++it)
        QCOMPARE(stored.schedulingRecord(ids.value(it.key())).state.due, it.value());
    QCOMPARE(stored.summary(dayStart).total, 0);

    // The frozen stats contract keeps its eleven keys beside the new command.
    const QJsonObject stats = runCli({"stats", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList expectedStats{
        "ok",  "totalRatings", "studyDayRatings", "studyDayGood", "streak", "newCardsOnStudyDay", "newCardAllowance",
        "due", "totalDue",     "cards",           "decks"};
    expectedStats.sort();
    QCOMPARE(sortedKeys(stats), expectedStats);
    QCOMPARE(stats.value(QStringLiteral("totalRatings")).toInt(), 0);
}

void TestCli::cardsResolvesDeckFileNameWithDifferentWrittenName() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    const QString path = QDir(decks).filePath("Lesson-Notes.txt");
    QVERIFY(writeFile(path, "# Deck: Written Name\n---\nQ: One?\nA: A\n"));

    int code = -1;
    const QJsonObject byFile = runCli({"cards", "--deck", "Lesson Notes", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(byFile), QStringList({"cards", "ok"}));
    QVERIFY(byFile.value(QStringLiteral("ok")).toBool());
    const QJsonArray cards = byFile.value(QStringLiteral("cards")).toArray();
    QCOMPARE(cards.size(), 1);
    QCOMPARE(cards.at(0).toObject().value(QStringLiteral("deck")).toString(), QStringLiteral("Written Name"));

    const QJsonObject byName = runCli({"cards", "--deck", "Written Name", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(byName.value(QStringLiteral("cards")).toArray(), cards);

    const QString text = runCliText({"cards", "--deck", "Lesson Notes"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(text.contains(QStringLiteral("One?")));

    const QJsonObject missing = runCli({"cards", "--deck", "Unknown", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(missing.value(QStringLiteral("error")).toString(),
             QStringLiteral("No Deck named Unknown. Run 'omacards decks' to see available Decks."));
}

void TestCli::searchAndDueResolveDeckFileNameWithDifferentWrittenName() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    QVERIFY(writeFile(QDir(decks).filePath("Lesson-Notes.txt"), "# Deck: Written Name\n---\nQ: lesson only\nA: A\n"));
    QVERIFY(writeFile(QDir(decks).filePath("Other.txt"), "# Deck: Other\n---\nQ: other only\nA: A\n"));

    int code = -1;
    // The child process has only the Deck file name and the environment to go
    // on, so it proves the fallback at the real CLI boundary.
    const QJsonObject searchFile =
        runCliProcess({"search", "only", "--deck", "Lesson Notes", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(searchFile), QStringList({"cards", "count", "ok"}));
    QCOMPARE(searchFile.value(QStringLiteral("count")).toInt(), 1);
    const QJsonArray searchCards = searchFile.value(QStringLiteral("cards")).toArray();
    QCOMPARE(searchCards.size(), 1);
    QCOMPARE(searchCards.at(0).toObject().value(QStringLiteral("question")).toString(), QStringLiteral("lesson only"));

    const QJsonObject searchName =
        runCliProcess({"search", "only", "--deck", "Written Name", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(searchFile).toJson(QJsonDocument::Compact),
             QJsonDocument(searchName).toJson(QJsonDocument::Compact));

    const QJsonObject dueFile = runCliProcess({"due", "--deck", "Lesson Notes", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(dueFile), QStringList({"cards", "count", "ok", "totalDue"}));
    QCOMPARE(dueFile.value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(dueFile.value(QStringLiteral("totalDue")).toInt(), 1);
    const QJsonArray dueCards = dueFile.value(QStringLiteral("cards")).toArray();
    QCOMPARE(dueCards.size(), 1);
    QCOMPARE(dueCards.at(0).toObject().value(QStringLiteral("question")).toString(), QStringLiteral("lesson only"));

    const QJsonObject dueName = runCliProcess({"due", "--deck", "Written Name", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(dueFile).toJson(QJsonDocument::Compact),
             QJsonDocument(dueName).toJson(QJsonDocument::Compact));

    QByteArray errors;
    const QString dueText = runCliProcessText({"due", "--deck", "Lesson Notes"}, decks, database, &code, &errors);
    QCOMPARE(code, 0);
    QCOMPARE(dueText, QStringLiteral("1\n"));
    QVERIFY(errors.isEmpty());
}

void TestCli::checkPreviewWarmReviewCramResolveDeckFileName() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    EnvGuard env(decks, database);
    const QString lessonPath = QDir(decks).filePath("Lesson-Notes.txt");
    QVERIFY(writeFile(lessonPath, "# Deck: Written Name\n---\nQ: lesson only\nA: A\n"));
    QVERIFY(writeFile(QDir(decks).filePath("Other.txt"),
                      "# Deck: Other\n---\nQ: other one\nA: A\n---\nQ: other two\nA: A\n"));

    QString error;
    const Deck lesson = DeckIO::load(lessonPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(lesson.name, QStringLiteral("Written Name"));
    QCOMPARE(lesson.cards.size(), 1);
    const QString cardId = lesson.cards.first().id;

    int code = -1;
    const QJsonObject check = runCli({"check", "--deck", "Lesson Notes", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(check.value(QStringLiteral("deck")).toString(), QStringLiteral("Lesson Notes"));
    const QJsonArray checkedDecks = check.value(QStringLiteral("decks")).toArray();
    QCOMPARE(checkedDecks.size(), 1);
    QCOMPARE(checkedDecks.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Written Name"));
    QCOMPARE(check.value(QStringLiteral("cards")).toInt(), 1);
    QCOMPARE(check.value(QStringLiteral("due")).toInt(), 1);
    QCOMPARE(check.value(QStringLiteral("totalDue")).toInt(), 1);
    const QJsonObject checkName = runCli({"check", "--deck", "Written Name", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    for (const char *key : {"cards", "decks", "due", "issueCounts", "issues", "totalDue"}) {
        const QString name = QString::fromLatin1(key);
        QCOMPARE(check.value(name), checkName.value(name));
    }

    const QJsonObject preview = runCli({"preview", "--deck", "Lesson Notes", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(preview.value(QStringLiteral("deck")).toString(), QStringLiteral("Lesson Notes"));
    const QJsonArray previewItems = preview.value(QStringLiteral("items")).toArray();
    QCOMPARE(previewItems.size(), 1);
    QCOMPARE(previewItems.at(0).toObject().value(QStringLiteral("question")).toString(), QStringLiteral("lesson only"));

    const QJsonObject warm = runCli({"warm", "--deck", "Lesson Notes", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(warm.value(QStringLiteral("deck")).toString(), QStringLiteral("Lesson Notes"));
    QCOMPARE(warm.value(QStringLiteral("rendered")).toInt(), 0);
    QCOMPARE(runCliText({"warm", "--deck", "Lesson Notes"}, decks, database, &code),
             QStringLiteral("Warmed Lesson Notes: 0 rendered · 0 cached\n"));

    const QJsonObject reviewed =
        runCli({"review", "--deck", "Lesson Notes", "-c", cardId, "-r", "3", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(reviewed.value(QStringLiteral("id")).toString(), cardId);
    QVERIFY(reviewed.value(QStringLiteral("intervalDays")).toInt() >= 0);

    const QJsonObject cram =
        runCli({"cram", "--deck", "Lesson Notes", "--steps", "2", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(cram.value(QStringLiteral("ratings")).toInt(), 2);
    QCOMPARE(cram.value(QStringLiteral("good")).toInt(), 2);
    const QJsonObject cramName =
        runCli({"cram", "--deck", "Written Name", "--steps", "2", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(cram).toJson(QJsonDocument::Compact),
             QJsonDocument(cramName).toJson(QJsonDocument::Compact));
}

void TestCli::writeCommandsResolveDeckFileNameWithDifferentWrittenName() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    const QString path = QDir(decks).filePath("Lesson-Notes.txt");
    QVERIFY(writeFile(path, "# Deck: Written Name\n---\nQ: Existing?\nA: A\n"));

    QString error;
    const Deck initial = DeckIO::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(initial.cards.size(), 1);
    const QString originalId = initial.cards.first().id;

    int code = -1;
    const QJsonObject single = runCli(
        {"add-card", "--deck", "Lesson Notes", "-q", "Single Q?", "-a", "Single A", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(single.value(QStringLiteral("ok")).toBool());
    QCOMPARE(single.value(QStringLiteral("card")).toObject().value(QStringLiteral("deck")).toString(),
             QStringLiteral("Written Name"));
    const QString singleId = single.value(QStringLiteral("id")).toString();
    QVERIFY(!singleId.isEmpty());

    const QString batchInput = dir.filePath("batch.json");
    QVERIFY(writeFile(batchInput, R"([{"question":"Batch Q?","answer":"Batch A"}])"));
    const QJsonObject batch =
        runCli({"add-cards", "--deck", "Lesson Notes", "--input", batchInput, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(batch), QStringList({"created", "deck", "ids", "issueCounts", "ok", "path"}));
    QCOMPARE(batch.value(QStringLiteral("deck")).toString(), QStringLiteral("Lesson Notes"));
    QCOMPARE(batch.value(QStringLiteral("created")).toInt(), 1);
    QCOMPARE(batch.value(QStringLiteral("path")).toString(), path);

    const QString textInput = dir.filePath("batch-text.json");
    QVERIFY(writeFile(textInput, R"([{"question":"Text Q?","answer":"Text A"}])"));
    const QString batchText =
        runCliText({"add-cards", "--deck", "Lesson Notes", "--input", textInput}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(batchText.contains(QStringLiteral("Added 1 cards to Lesson Notes")));

    const QJsonObject edited = runCli(
        {"edit-card", "--deck", "Lesson Notes", "-c", originalId, "-q", "Edited Q?", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(edited), QStringList({"card", "id", "ok"}));
    const QJsonObject editedCard = edited.value(QStringLiteral("card")).toObject();
    QCOMPARE(editedCard.value(QStringLiteral("deck")).toString(), QStringLiteral("Written Name"));
    QCOMPARE(editedCard.value(QStringLiteral("question")).toString(), QStringLiteral("Edited Q?"));

    const QJsonObject deleted =
        runCli({"delete-card", "--deck", "Lesson Notes", "-c", singleId, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(deleted), QStringList({"id", "ok"}));
    QCOMPARE(deleted.value(QStringLiteral("id")).toString(), singleId);

    const QJsonObject described =
        runCli({"edit-deck", "Lesson Notes", "--description", "Fresh description", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(described), QStringList({"deck", "description", "ok", "path"}));
    QCOMPARE(described.value(QStringLiteral("deck")).toString(), QStringLiteral("Lesson Notes"));
    QCOMPARE(described.value(QStringLiteral("description")).toString(), QStringLiteral("Fresh description"));
    QCOMPARE(described.value(QStringLiteral("path")).toString(), path);
    const QString describedText =
        runCliText({"edit-deck", "Lesson Notes", "--description", "Terse"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(describedText.contains(QStringLiteral("Updated description of deck Lesson Notes")));

    error.clear();
    const Deck written = DeckIO::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(written.name, QStringLiteral("Written Name"));
    QCOMPARE(written.description, QStringLiteral("Terse"));
    QCOMPARE(written.cards.size(), 3);
    for (const Card &card : written.cards) {
        QCOMPARE(card.deck, QStringLiteral("Written Name"));
        QVERIFY(card.id != singleId);
    }
    const QSet<QString> questions{written.cards.at(0).question, written.cards.at(1).question,
                                  written.cards.at(2).question};
    QCOMPARE(questions, QSet<QString>({"Edited Q?", "Batch Q?", "Text Q?"}));

    // A name that is not an existing Deck id or file still defines a new Deck.
    const QJsonObject fresh = runCli({"add-card", "--deck", "Brand New", "-q", "Fresh Q?", "-a", "Fresh A", "--json"},
                                     decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(fresh.value(QStringLiteral("card")).toObject().value(QStringLiteral("deck")).toString(),
             QStringLiteral("Brand New"));
    error.clear();
    const Deck freshDeck = DeckIO::load(QDir(decks).filePath("Brand-New.txt"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(freshDeck.name, QStringLiteral("Brand New"));
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
    QCOMPARE(unknown.value(QStringLiteral("error")).toString(),
             QString("No Deck named Missing. Run 'omacards decks' to see available Decks."));

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

    // An unknown positional subcommand is an error, not the bare deck list.
    const QJsonObject unknown = runCli({"decks", "active", "--json"}, emptyDecks, emptyDatabase, &code, &standardError);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(unknown), QStringList({"error", "ok"}));
    QVERIFY(!unknown.value(QStringLiteral("ok")).toBool());
    QCOMPARE(unknown.value(QStringLiteral("error")).toString(), QString("Unknown command: decks active"));
    runCli({"decks", "active"}, emptyDecks, emptyDatabase, &code, &standardError);
    QCOMPARE(code, 1);
    QVERIFY(standardError.contains("Unknown command: decks active"));

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
    const QJsonObject unknownSubcommand = runCli({"decks", "active", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(unknownSubcommand), QStringList({"error", "ok"}));
    QVERIFY(!unknownSubcommand.contains(QStringLiteral("decks")));
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
    QCOMPARE(none.value(QStringLiteral("error")).toString(),
             QString("No Deck named Unknown. Run 'omacards decks' to see available Decks."));
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

    const QJsonObject noAnswer = runCli({"add-card", "--deck", "Fresh", "-q", "Q", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QStringList noAnswerKeys{"error", "ok"};
    noAnswerKeys.sort();
    QCOMPARE(sortedKeys(noAnswer), noAnswerKeys);
    QCOMPARE(noAnswer.value(QStringLiteral("error")).toString(), QString("Use --answer / -a"));
    QVERIFY(!QFileInfo::exists(QDir(decks).filePath("Fresh.txt")));

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

void TestCli::concurrentAddCardCommandsKeepEveryCard() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    const QString cache = dir.filePath("cache");
    QVERIFY(QDir().mkpath(decks));
    QVERIFY(QDir().mkpath(cache));
    const QString path = QDir(decks).filePath("Parallel.txt");
    QVERIFY(writeFile(path, "# Deck: Parallel\n"));

    constexpr int added = 6;
    std::vector<std::unique_ptr<QProcess>> processes;
    processes.reserve(added);
    for (int i = 0; i < added; ++i) {
        processes.push_back(
            startCliProcess({"add-card", "--deck", "Parallel", "-q", QStringLiteral("Concurrent Q%1?").arg(i), "-a",
                             QStringLiteral("Concurrent A%1").arg(i), "--json"},
                            decks, database, cache));
    }

    QSet<QString> ids;
    for (int i = 0; i < added; ++i) {
        QProcess &process = *processes.at(i);
        QVERIFY2(process.waitForFinished(20000), qPrintable(process.errorString()));
        const QByteArray output = process.readAllStandardOutput();
        const QByteArray errors = process.readAllStandardError();
        QCOMPARE(process.exitCode(), 0);
        const QJsonObject response = QJsonDocument::fromJson(output).object();
        QVERIFY2(response.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QString::fromUtf8(output) + QString::fromUtf8(errors)));
        const QString id = response.value(QStringLiteral("id")).toString();
        QVERIFY2(!id.isEmpty(), qPrintable(QString::fromUtf8(output)));
        QCOMPARE(response.value(QStringLiteral("card")).toObject().value(QStringLiteral("id")).toString(), id);
        ids.insert(id);
    }
    QCOMPARE(ids.size(), added);

    QString error;
    const Deck deck = DeckIO::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(deck.cards.size(), added);
    QSet<QString> storedIds;
    for (const Card &card : deck.cards)
        storedIds.insert(card.id);
    QCOMPARE(storedIds.size(), added);
    for (const QString &id : ids)
        QVERIFY(storedIds.contains(id));
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
    // delete-deck's error is owned by DeckRepository, outside this ticket.
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

void TestCli::importJsonKeepsExactContractKeys() {
    QTemporaryDir dir;
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));

    const QString input = dir.filePath("Contract.json");
    QVERIFY(writeFile(input, R"([{"question":"q","answer":"a"},{"question":"q2","answer":"a2"}])"));

    int code = -1;
    const QJsonObject imported = runCli({"import", input, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QStringList keys = imported.keys();
    keys.sort();
    QCOMPARE(keys, QStringList({"cards", "deck", "file", "ok"}));
    QVERIFY(imported.value(QStringLiteral("ok")).toBool());
    QCOMPARE(imported.value(QStringLiteral("file")).toString(), input);
    QCOMPARE(imported.value(QStringLiteral("deck")).toString(), QString("Contract"));
    QCOMPARE(imported.value(QStringLiteral("cards")).toInt(), 2);
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
    QCOMPARE(noDeck.value(QStringLiteral("error")).toString(), QString("Name a Deck with --deck NAME (or -d NAME)."));

    const QJsonObject badFormat =
        runCli({"preview", "--deck", "P", "--format", "xml", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(badFormat.value(QStringLiteral("error")).toString(), QString("--format must be text, html or markdown"));

    const QJsonObject noCards = runCli({"preview", "--deck", "Missing", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(noCards.value(QStringLiteral("error")).toString(),
             QString("No Deck named Missing. Run 'omacards decks' to see available Decks."));

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

    // Preview reads Deck files only. A state database that cannot open must not
    // block it or start a study session that claims the preview's math renders.
    const QJsonObject brokenState = runCli({"preview", "--deck", "P", "--json"}, decks, decks, &code);
    QCOMPARE(code, 0);
    QCOMPARE(brokenState.value(QStringLiteral("items")).toArray().size(), 2);

    const QJsonObject all = runCli({"preview", "--deck", "P", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(sortedKeys(all), QStringList({"deck", "format", "items", "ok"}));
    QCOMPARE(all.value(QStringLiteral("items")).toArray().size(), 2);

    const QJsonObject one = runCli({"preview", "--deck", "P", "--card", firstId, "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(one.value(QStringLiteral("items")).toArray().size(), 1);

    // A stale id names itself in the error; the failure keeps only error/ok.
    const QString staleId = QStringLiteral("deadbeef");
    int staleCode = -1;
    QByteArray staleErrors;
    const QJsonObject stale =
        runCli({"preview", "--deck", "P", "--card", staleId, "--json"}, decks, database, &staleCode, &staleErrors);
    QCOMPARE(staleCode, 1);
    QVERIFY(staleErrors.isEmpty());
    QCOMPARE(sortedKeys(stale), QStringList({"error", "ok"}));
    QVERIFY(!stale.value(QStringLiteral("ok")).toBool());
    QCOMPARE(stale.value(QStringLiteral("error")).toString(), QStringLiteral("No such card: %1").arg(staleId));

    // HTML preview has no theme, so math must be emitted in black regardless
    // of the colors the app happens to use.
    const QString bin = dir.filePath("bin");
    // A slow toolchain keeps any validation Backend's async math claim active
    // while preview renders, so a preview that competes for that claim fails.
    QVERIFY(writeFakePdfToolchain(bin, kCliTinySvg, QStringLiteral("sleep 0.2\n")));
    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", dir.filePath("args.log"));
    PathGuard pathGuard(bin);
    {
        auto owned = Backend::fromEnvironment();
        QString error;
        QVERIFY(owned->initialize(&error));
        QVERIFY(owned->createCard("P", "\\(z^2\\)", "plain", "basic", {}, &error));
        QVERIFY(owned->createCard("P", "plain question", "\\(f'''(x)\\)", "basic", {}, &error));
    }

    const QJsonObject html = runCli({"preview", "--deck", "P", "--format", "html", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonArray htmlItems = html.value(QStringLiteral("items")).toArray();
    QCOMPARE(htmlItems.size(), 4);
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

    QString answerHtml;
    for (const QJsonValue &value : htmlItems) {
        const QString answer = value.toObject().value(QStringLiteral("answerHtml")).toString();
        if (answer.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")))
            answerHtml = answer;
    }
    QVERIFY(!answerHtml.isEmpty());
    // kCliTinySvg normalizes to 26 x 14 px; its logical size is half that and
    // preview scale 1.6 gives 21 x 11. Apostrophes must not change what Qt
    // parses back from the Answer's image markup.
    QCOMPARE(parsedMathImageSize(answerHtml), QSize(21, 11));

    // Markdown keeps the source notation instead of images or the Unicode
    // fallback, even while the same fake LaTeX toolchain is available.
    const QJsonObject markdown =
        runCli({"preview", "--deck", "P", "--format", "markdown", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(markdown.value(QStringLiteral("format")).toString(), QString("markdown"));
    QCOMPARE(sortedKeys(markdown), QStringList({"deck", "format", "items", "ok"}));
    const QJsonArray markdownItems = markdown.value(QStringLiteral("items")).toArray();
    QCOMPARE(markdownItems.size(), 4);
    bool markdownQuestionFound = false;
    bool markdownAnswerFound = false;
    for (const QJsonValue &value : markdownItems) {
        const QJsonObject item = value.toObject();
        QCOMPARE(sortedKeys(item), QStringList({"answer", "id", "question"}));
        const QString question = item.value(QStringLiteral("question")).toString();
        const QString answer = item.value(QStringLiteral("answer")).toString();
        QVERIFY(!question.contains(QStringLiteral("data:image/svg+xml;base64,")));
        QVERIFY(!answer.contains(QStringLiteral("data:image/svg+xml;base64,")));
        markdownQuestionFound |= question.contains(QStringLiteral("\\(z^2\\)"));
        markdownAnswerFound |= answer.contains(QStringLiteral("\\(f'''(x)\\)"));
    }
    QVERIFY(markdownQuestionFound);
    QVERIFY(markdownAnswerFound);
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
    QCOMPARE(broken.value(QStringLiteral("error")).toString(),
             expectedUnreadableDeckError(QStringLiteral("Broken"), deckDir.filePath("Broken.txt")));
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
    QStringList expectedLines{expectedUnreadableDeckError(QStringLiteral("Broken"), deckDir.filePath("Broken.txt"))};
    for (const QJsonValue &value : checkIssues)
        expectedLines << checkDeckIssueLine(value.toObject());
    expectedLines << QStringLiteral("omacards check --deck Broken");
    QCOMPARE(QString::fromUtf8(stderrOutput), expectedLines.join('\n') + '\n');

    // A valid empty Deck has no Deck issues but still gets the hint.
    const QJsonObject empty = runCli({"preview", "--deck", "Empty", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(empty), failureKeys);
    QCOMPARE(empty.value(QStringLiteral("error")).toString(),
             QStringLiteral("Deck Empty has no Cards. Add a Card to it before using this command."));
    QCOMPARE(empty.value(QStringLiteral("deck")).toString(), QStringLiteral("Empty"));
    QVERIFY(empty.value(QStringLiteral("issues")).toArray().isEmpty());
    const QJsonObject emptyCounts = empty.value(QStringLiteral("issueCounts")).toObject();
    QCOMPARE(emptyCounts.value(QStringLiteral("error")).toInt(), 0);
    QCOMPARE(emptyCounts.value(QStringLiteral("warning")).toInt(), 0);
    QCOMPARE(empty.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck Empty"));

    const QString emptyText = runCliText({"preview", "--deck", "Empty"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(emptyText.isEmpty());
    QCOMPARE(
        QString::fromUtf8(stderrOutput),
        QStringLiteral(
            "Deck Empty has no Cards. Add a Card to it before using this command.\nomacards check --deck Empty\n"));

    // A --card filter does not hide an existing zero-Card Deck's Deck issues.
    const QJsonObject emptyFiltered =
        runCli({"preview", "--deck", "Empty", "--card", "nope", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(emptyFiltered), failureKeys);
    QVERIFY(!emptyFiltered.value(QStringLiteral("ok")).toBool());
    QCOMPARE(emptyFiltered.value(QStringLiteral("error")).toString(),
             QStringLiteral("Deck Empty has no Cards. Add a Card to it before using this command."));
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
    QCOMPARE(
        QString::fromUtf8(stderrOutput),
        QStringLiteral(
            "Deck Empty has no Cards. Add a Card to it before using this command.\nomacards check --deck Empty\n"));

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
    QCOMPARE(missing.value(QStringLiteral("error")).toString(),
             QStringLiteral("No Deck named Missing. Run 'omacards decks' to see available Decks."));
    const QString missingText = runCliText({"preview", "--deck", "Missing"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(missingText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput),
             QStringLiteral("No Deck named Missing. Run 'omacards decks' to see available Decks.\n"));

    // A --card miss on a Deck with Cards names the stale id in both modes.
    const QJsonObject filtered =
        runCli({"preview", "--deck", "Good", "--card", "nope", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(filtered), QStringList({"error", "ok"}));
    QVERIFY(!filtered.value(QStringLiteral("ok")).toBool());
    QCOMPARE(filtered.value(QStringLiteral("error")).toString(), QStringLiteral("No such card: nope"));
    const QString filteredText =
        runCliText({"preview", "--deck", "Good", "--card", "nope"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(filteredText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput), QStringLiteral("No such card: nope\n"));

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
        QCOMPARE(sortedKeys(result), QStringList({"error", "ok"}));
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

    // An unknown flag is an error and leaves the stored allowance untouched.
    const QJsonObject beforeUnknown = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    const QJsonObject rejected = runCli({"config", "--new-per-day", "7", "--json"}, decks, database, &code);
    QCOMPARE(code, 1);
    QCOMPARE(sortedKeys(rejected), QStringList({"error", "ok"}));
    QVERIFY(!rejected.value(QStringLiteral("ok")).toBool());
    QCOMPARE(rejected.value(QStringLiteral("error")).toString(), QString("Unknown flag: --new-per-day"));
    const QJsonObject afterUnknown = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(afterUnknown).toJson(), QJsonDocument(beforeUnknown).toJson());
    QByteArray rejectedStderr;
    QCOMPARE(runCliText({"config", "--new-per-day", "7"}, decks, database, &code, &rejectedStderr), QString());
    QCOMPARE(code, 1);
    QVERIFY(rejectedStderr.contains("Unknown flag: --new-per-day"));

    // --deck has no meaning for config: reject both spellings before a Setting
    // write, even when the named Deck exists and other flags are valid.
    const QJsonObject beforeDeck = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    expectError({"config", "--deck", "Budget", "--new-card-allowance", "7", "--json"}, "Unknown flag: --deck");
    expectError({"config", "--deck=Missing", "--new-card-allowance", "9", "--json"}, "Unknown flag: --deck");
    const QJsonObject afterDeck = runCli({"config", "--json"}, decks, database, &code);
    QCOMPARE(code, 0);
    QCOMPARE(QJsonDocument(afterDeck).toJson(), QJsonDocument(beforeDeck).toJson());

    const QString configText = runCliText({"config"}, decks, database, &code);
    QCOMPARE(code, 0);
    QVERIFY(configText.contains(QStringLiteral("New-card allowance: 5")));
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
    PathGuard pathGuard(bin);

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
    QCOMPARE(missing.value(QStringLiteral("error")).toString(),
             QStringLiteral("No Deck named Missing. Run 'omacards decks' to see available Decks."));

    const QString missingText = runCliText({"warm", "--deck", "Missing"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(missingText.isEmpty());
    QCOMPARE(QString::fromUtf8(stderrOutput),
             QStringLiteral("No Deck named Missing. Run 'omacards decks' to see available Decks.\n"));

    const QJsonObject empty = runCli({"warm", "--deck", "Empty", "--json"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(stderrOutput.isEmpty());
    QCOMPARE(sortedKeys(empty), QStringList({"deck", "error", "hint", "issueCounts", "issues", "ok"}));
    QCOMPARE(empty.value(QStringLiteral("error")).toString(),
             QStringLiteral("Deck Empty has no Cards. Add a Card to it before using this command."));
    QCOMPARE(empty.value(QStringLiteral("hint")).toString(), QStringLiteral("omacards check --deck Empty"));

    const QString emptyText = runCliText({"warm", "--deck", "Empty"}, decks, database, &code, &stderrOutput);
    QCOMPARE(code, 1);
    QVERIFY(emptyText.isEmpty());
    QCOMPARE(
        QString::fromUtf8(stderrOutput),
        QStringLiteral(
            "Deck Empty has no Cards. Add a Card to it before using this command.\nomacards check --deck Empty\n"));
}

void TestCli::deckFailureMessagesNameCauseAndNextStep() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString decks = dir.filePath("decks");
    const QString database = dir.filePath("state.db");
    QVERIFY(QDir().mkpath(decks));
    // One leading space makes loadAll() skip this Deck file. The resolver
    // still sees the file, so this is an unreadable Deck file, not a typo.
    const QString skipped = QDir(decks).filePath("Skipped.txt");
    QVERIFY(writeFile(skipped, R"deck(# Deck: Skipped
---
Q: First line
 second line
A: answer
)deck"));
    QVERIFY(writeFile(QDir(decks).filePath("Empty.txt"), "# Deck: Empty\n"));

    int code = -1;
    QByteArray errors;
    const QJsonObject unknown =
        runCliProcess({"cards", "--deck", "Missing", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QVERIFY(errors.isEmpty());
    QCOMPARE(sortedKeys(unknown), QStringList({"error", "ok"}));
    QCOMPARE(unknown.value(QStringLiteral("error")).toString(),
             QStringLiteral("No Deck named Missing. Run 'omacards decks' to see available Decks."));

    const QJsonObject missing = runCliProcess({"warm", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QCOMPARE(missing.value(QStringLiteral("error")).toString(),
             QStringLiteral("Name a Deck with --deck NAME (or -d NAME)."));

    const QJsonObject unreadable =
        runCliProcess({"cards", "--deck", "Skipped", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QCOMPARE(unreadable.value(QStringLiteral("error")).toString(),
             QStringLiteral("Cannot read the Deck file at %1. Run 'omacards check --deck Skipped' to see Deck issues.")
                 .arg(skipped));

    const QJsonObject empty = runCliProcess({"cram", "--deck", "Empty", "--json"}, decks, database, &code, &errors);
    QCOMPARE(code, 1);
    QCOMPARE(empty.value(QStringLiteral("error")).toString(),
             QStringLiteral("Deck Empty has no Cards. Add a Card to it before using this command."));

    const auto expectTextError = [&](const QStringList &arguments, const QString &expected) {
        QByteArray textErrors;
        const QString text = runCliProcessText(arguments, decks, database, &code, &textErrors);
        QCOMPARE(code, 1);
        QVERIFY(text.isEmpty());
        QCOMPARE(QString::fromUtf8(textErrors), expected + QStringLiteral("\n"));
    };
    expectTextError({"cards", "--deck", "Missing"},
                    QStringLiteral("No Deck named Missing. Run 'omacards decks' to see available Decks."));
    expectTextError({"warm"}, QStringLiteral("Name a Deck with --deck NAME (or -d NAME)."));
    expectTextError({"cards", "--deck", "Skipped"},
                    QStringLiteral("Cannot read the Deck file at %1. Run 'omacards check --deck Skipped' "
                                   "to see Deck issues.")
                        .arg(skipped));
    expectTextError({"cram", "--deck", "Empty"},
                    QStringLiteral("Deck Empty has no Cards. Add a Card to it before using this command."));
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
