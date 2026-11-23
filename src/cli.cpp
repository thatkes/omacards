// Agent-facing commands are deterministic, take --json, and never open a GUI.
// Deck files stay the source of truth; these commands only edit those files
// and the SQLite sidecar.
#include "cli.h"

#include "backend.h"
#include "config.h"
#include "deck.h"
#include "deckrepository.h"
#include "domain.h"
#include "insessionprogression.h"
#include "mathrender.h"
#include "ratingrecording.h"
#include "store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <algorithm>
#include <cstdio>
#include <unistd.h>

namespace {
struct Options {
    QString command;
    QString deck;
    QString question;
    QString answer;
    QString type = "basic";
    QString tags;
    QString card;
    QString path;
    QString newName;
    QString input;
    QString description;
    QString format = "text";
    QString unknownFlag;
    int rating = 0;
    int steps = 10;
    int newCardAllowance = -1;
    int dayStartHour = -1;
    double retention = -1.0;
    bool json = false;
    bool math = false;
    bool strict = false;
    bool questionSet = false;
    bool answerSet = false;
    bool typeSet = false;
    bool tagsSet = false;
    bool help = false;
};

// The one GUI command path. Every other first token is an attempted CLI
// command, including unknown ones, so typos fail fast instead of opening a GUI
// event loop that never returns.
const QString &guiCommand() {
    static const QString command = QStringLiteral("open");
    return command;
}

// Help is a request for syntax, never a positional value or a flag argument.
bool hasHelpFlag(const QStringList &arguments) {
    return arguments.contains(QStringLiteral("--help")) || arguments.contains(QStringLiteral("-h"));
}

// The registry is a plain text file embedded as a Qt resource. Reading it (not
// a compiled constant) lets tooling inspect the same command paths before the
// app is built.
QStringList readCommandPaths() {
    QStringList paths;
    QFile registry(QStringLiteral(":/command-paths.txt"));
    if (!registry.open(QIODevice::ReadOnly | QIODevice::Text))
        return paths;
    while (!registry.atEnd()) {
        const QString line = QString::fromUtf8(registry.readLine()).trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#')))
            paths << line;
    }
    return paths;
}

QString commandToken(const QString &path) {
    return path.section(QLatin1Char(' '), 0, 0);
}

bool isHeadlessCommand(const QString &command) {
    return Cli::headlessCommands().contains(command);
}

Options parse(const QStringList &arguments) {
    Options options;
    QStringList rest = arguments;
    rest.removeAll(QString("--json"));
    rest.removeAll(QString("--math"));
    rest.removeAll(QString("--strict"));
    options.help = hasHelpFlag(arguments);
    rest.removeAll(QStringLiteral("--help"));
    rest.removeAll(QStringLiteral("-h"));
    if (!rest.isEmpty())
        options.command = rest.takeFirst();
    for (int i = 0; i < rest.size(); ++i) {
        const QString arg = rest.at(i);
        auto value = [&]() { return i + 1 < rest.size() ? rest.at(++i) : QString(); };
        if (arg == "--deck" || arg == "-d")
            options.deck = value();
        else if (arg == "--question" || arg == "-q") {
            options.question = value();
            options.questionSet = true;
        } else if (arg == "--answer" || arg == "-a") {
            options.answer = value();
            options.answerSet = true;
        } else if (arg == "--type") {
            options.type = value();
            options.typeSet = true;
        } else if (arg == "--tags") {
            options.tags = value();
            options.tagsSet = true;
        } else if (arg == "--card" || arg == "-c")
            options.card = value();
        else if (arg == "--input" || arg == "-i")
            options.input = value();
        else if (arg == "--description")
            options.description = value();
        else if (arg == "--format")
            options.format = value();
        else if (arg == "--rating" || arg == "-r")
            options.rating = value().toInt();
        else if (arg == "--steps")
            options.steps = value().toInt();
        else if (arg == "--new-card-allowance")
            options.newCardAllowance = value().toInt();
        else if (arg == "--day-start-hour")
            options.dayStartHour = value().toInt();
        else if (arg == "--retention")
            options.retention = value().toDouble();
        else if (arg.startsWith("--deck="))
            options.deck = arg.section('=', 1);
        else if (arg.startsWith("--question=")) {
            options.question = arg.section('=', 1);
            options.questionSet = true;
        } else if (arg.startsWith("--answer=")) {
            options.answer = arg.section('=', 1);
            options.answerSet = true;
        } else if (arg.startsWith("--input="))
            options.input = arg.section('=', 1);
        else if (arg.startsWith("--description="))
            options.description = arg.section('=', 1);
        else if (arg.startsWith("--format="))
            options.format = arg.section('=', 1);
        else if (arg.startsWith("--new-card-allowance="))
            options.newCardAllowance = arg.section('=', 1).toInt();
        else if (options.command == "rename-deck") {
            if (options.path.isEmpty())
                options.path = arg;
            else if (options.newName.isEmpty())
                options.newName = arg;
        } else if (arg.startsWith("--")) {
            if (options.unknownFlag.isEmpty())
                options.unknownFlag = arg;
        } else if (options.path.isEmpty())
            options.path = arg;
    }
    options.json = arguments.contains(QString("--json"));
    options.math = arguments.contains(QString("--math"));
    options.strict = arguments.contains(QString("--strict"));
    return options;
}

void write(const QJsonObject &object, bool json, const QString &human) {
    if (json) {
        const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
        fputc('\n', stdout);
    } else {
        fprintf(stdout, "%s\n", qPrintable(human));
    }
}

int fail(const QString &message, bool json) {
    if (json) {
        QJsonObject object;
        object.insert("ok", false);
        object.insert("error", message);
        write(object, true, {});
    } else {
        fprintf(stderr, "%s\n", qPrintable(message));
    }
    return 1;
}

// HOME is the only input to the install target. The bare `skill` command and
// the `skill status` subcommand share this read-only snapshot.
QString skillTargetPath() {
    return QDir::homePath() + QStringLiteral("/.agents/skills/omacards/SKILL.md");
}

struct SkillStatus {
    QString path;
    bool installed = false;
    bool matches = false;
};

SkillStatus readSkillStatus() {
    SkillStatus status;
    status.path = skillTargetPath();
    status.installed = QFileInfo::exists(status.path);
    if (!status.installed)
        return status;
    QFile bundled(QStringLiteral(":/skill/SKILL.md"));
    const QByteArray source = bundled.open(QIODevice::ReadOnly) ? bundled.readAll() : QByteArray();
    QFile installed(status.path);
    status.matches = installed.open(QIODevice::ReadOnly) && installed.readAll() == source;
    return status;
}

QJsonObject skillStatusJson(const SkillStatus &status) {
    QJsonObject object;
    object.insert("ok", true);
    object.insert("installed", status.installed);
    object.insert("path", status.path);
    object.insert("matches", status.matches);
    return object;
}

QString skillStatusText(const SkillStatus &status) {
    QStringList lines;
    lines << QStringLiteral("Skill installed: %1").arg(status.installed ? QStringLiteral("yes") : QStringLiteral("no"));
    if (status.installed) {
        lines << QStringLiteral("Installed copy matches bundled source: %1")
                     .arg(status.matches ? QStringLiteral("yes") : QStringLiteral("no"));
    }
    lines << QStringLiteral("Skill target: %1").arg(status.path);
    return lines.join(QLatin1Char('\n'));
}

QString skillHelpNotes() {
    return QStringLiteral("Target: %1\n"
                          "Installs are idempotent: an unchanged generated copy is left alone; a changed copy is "
                          "overwritten.\n"
                          "Restart the agent to load a new skill.\n")
        .arg(skillTargetPath());
}

struct DeckIssueReport {
    QJsonArray entries;
    QStringList lines;
    int errors = 0;
    int warnings = 0;
};

// Same resolution order as check: a loaded Deck name first, then the deck
// filename fallback for the supported suffixes.
QString resolveDeckPath(const QVector<Deck> &decks, const QString &decksDirectory, const QString &deckName) {
    for (const Deck &deck : decks) {
        if (deck.name == deckName)
            return deck.path;
    }
    if (!deckName.isEmpty()) {
        const QDir dir(decksDirectory);
        const QString base = DeckIO::safeFileName(deckName);
        for (const QString &suffix : {QStringLiteral(".txt"), QStringLiteral(".deck")}) {
            const QString candidate = dir.filePath(base + suffix);
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }
    return {};
}

void appendDeckIssue(DeckIssueReport *report, const DeckIssue &issue) {
    QJsonObject entry;
    entry.insert("path", issue.path);
    if (!issue.card.isEmpty())
        entry.insert("card", issue.card);
    if (issue.line > 0)
        entry.insert("line", issue.line);
    entry.insert("severity", issue.severity);
    entry.insert("message", issue.message);
    entry.insert("hint", issue.hint);
    report->entries << entry;
    if (issue.severity == QLatin1String("error"))
        ++report->errors;
    else
        ++report->warnings;
    report->lines << QStringLiteral("%1:%2 %3: %4 (%5)")
                         .arg(issue.path)
                         .arg(issue.line)
                         .arg(issue.severity, issue.message, issue.hint);
}

DeckIssueReport deckIssueReport(const QString &decksDirectory, const QString &targetPath) {
    DeckIssueReport report;
    for (const DeckIssue &issue : DeckIO::lint(decksDirectory)) {
        if (!targetPath.isEmpty() && issue.path != targetPath)
            continue;
        appendDeckIssue(&report, issue);
    }
    return report;
}

DeckIssueReport deckIssueReportForFile(const QString &path) {
    DeckIssueReport report;
    for (const DeckIssue &issue : DeckIO::lintFile(path))
        appendDeckIssue(&report, issue);
    return report;
}

QJsonObject issueCountsJson(const DeckIssueReport &report) {
    QJsonObject counts;
    counts.insert("error", report.errors);
    counts.insert("warning", report.warnings);
    return counts;
}

QString checkCommandHint(const QString &deckName) {
    bool needsQuotes = false;
    for (const QChar ch : deckName) {
        if (ch.isSpace()) {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes)
        return QStringLiteral("omacards check --deck %1").arg(deckName);
    QString escaped = deckName;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("omacards check --deck '%1'").arg(escaped);
}

int failEmptyDeck(const QString &deckName, const DeckIssueReport &report, bool json) {
    const QString message = QObject::tr("Deck %1 has no Cards").arg(deckName);
    const QString hint = checkCommandHint(deckName);
    if (json) {
        QJsonObject object;
        object.insert("ok", false);
        object.insert("error", message);
        object.insert("deck", deckName);
        object.insert("issues", report.entries);
        object.insert("issueCounts", issueCountsJson(report));
        object.insert("hint", hint);
        write(object, true, {});
    } else {
        fprintf(stderr, "%s\n", qPrintable(message));
        for (const QString &line : report.lines)
            fprintf(stderr, "%s\n", qPrintable(line));
        fprintf(stderr, "%s\n", qPrintable(hint));
    }
    return 1;
}

QVector<Card> allCards(const QVector<Deck> &decks, const QString &deckName) {
    QVector<Card> cards;
    for (const Deck &deck : decks) {
        if (!deckName.isEmpty() && deck.name != deckName)
            continue;
        cards << deck.cards;
    }
    return cards;
}

QJsonObject cardJson(const Card &card) {
    QJsonObject object;
    object.insert("id", card.id);
    object.insert("deck", card.deck);
    object.insert("question", card.question);
    object.insert("answer", card.answer);
    object.insert("type", Domain::toString(card.type));
    QJsonArray tags;
    for (const QString &tag : card.tags)
        tags << tag;
    object.insert("tags", tags);
    return object;
}

bool readCardInput(const Options &options, QByteArray *bytes, QString *error) {
    const QString source = options.input.isEmpty() ? options.path : options.input;
    if (source.isEmpty() || source == QLatin1String("-")) {
        if (::isatty(fileno(stdin))) {
            if (error)
                *error = QObject::tr("Use --input FILE or pipe JSON to stdin");
            return false;
        }
        char buffer[4096];
        size_t count = 0;
        while ((count = fread(buffer, 1, sizeof(buffer), stdin)) > 0)
            bytes->append(buffer, static_cast<int>(count));
        return true;
    }
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot read: %1").arg(source);
        return false;
    }
    *bytes = file.readAll();
    return true;
}

bool parseCardDrafts(const QByteArray &data, const QString &descriptionFlag, QVector<CardDraft> *drafts,
                     QString *description, QString *error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QObject::tr("Invalid JSON: %1").arg(parseError.errorString());
        return false;
    }

    QJsonArray cards;
    *description = descriptionFlag;
    if (document.isArray()) {
        cards = document.array();
    } else if (document.isObject()) {
        const QJsonObject root = document.object();
        cards = root.value(QStringLiteral("cards")).toArray();
        if (descriptionFlag.isEmpty())
            *description = root.value(QStringLiteral("description")).toString();
    } else {
        if (error)
            *error = QObject::tr("Expected a JSON array or an object with a cards array");
        return false;
    }
    if (cards.isEmpty()) {
        if (error)
            *error = QObject::tr("No cards in JSON input");
        return false;
    }

    for (int i = 0; i < cards.size(); ++i) {
        if (!cards.at(i).isObject()) {
            if (error)
                *error = QObject::tr("cards[%1] must be an object").arg(i);
            return false;
        }
        const QJsonObject entry = cards.at(i).toObject();
        CardDraft draft;
        draft.question = entry.value(QStringLiteral("question")).toString();
        draft.answer = entry.value(QStringLiteral("answer")).toString();
        draft.type = entry.value(QStringLiteral("type")).toString(QStringLiteral("basic"));
        const QJsonValue tags = entry.value(QStringLiteral("tags"));
        if (tags.isArray()) {
            for (const QJsonValue &tag : tags.toArray())
                draft.tags << tag.toString();
        } else if (!tags.isUndefined() && !tags.isNull()) {
            draft.tags = tags.toString().split(',', Qt::SkipEmptyParts);
        }
        if (draft.question.trimmed().isEmpty()) {
            if (error)
                *error = QObject::tr("cards[%1] needs a question").arg(i);
            return false;
        }
        if (Domain::cardTypeFromString(draft.type) == CardType::Invalid) {
            if (error)
                *error = QObject::tr("cards[%1] has an invalid type: %2").arg(i).arg(draft.type);
            return false;
        }
        drafts->append(draft);
    }
    return true;
}

QString usage() {
    return QStringLiteral("omacards — keyboard flashcards for Omarchy\n\n"
                          "Usage:\n"
                          "  omacards [open]                  Start the GUI\n"
                          "  omacards check [--deck NAME] [--math] [--strict] [--json]\n"
                          "                                   Show decks, cards, due count, math and issues\n"
                          "  omacards decks [--json]          List decks\n"
                          "  omacards decks current [--json]  Show the Current deck\n"
                          "  omacards use-deck NAME [--json] Choose the Current deck\n"
                          "  omacards cards [--deck D] [--json]\n"
                          "  omacards search TERM [--deck D] [--json]\n"
                          "  omacards preview --deck D [--card ID] [--format text|html] [--json]\n"
                          "  omacards warm --deck D [--json]          Render a Deck's math SVGs ahead of study\n"
                          "  omacards add-deck NAME [--description TEXT] [--json]\n"
                          "  omacards add-cards --deck NAME [--description TEXT] [--input FILE|-] [--json]\n"
                          "  omacards add-card -q Q -a A [-d D] [--type cloze] [--tags a,b] [--json]\n"
                          "  omacards edit-card -c ID [-q Q] [-a A] [--type T] [--tags a,b] [--json]\n"
                          "  omacards delete-card -c ID [-d D] [--json]\n"
                          "  omacards delete-deck NAME [--json]\n"
                          "  omacards rename-deck OLD NEW [--json]\n"
                          "  omacards import PATH [-d D] [--json]\n"
                          "  omacards due [-d D] [--json]\n"
                          "  omacards review -c ID -r 1|3 [--json]\n"
                          "  omacards cram [-d D] [--steps N] [--json]\n"
                          "  omacards stats [--json]\n"
                          "  omacards config [--new-card-allowance N] [--day-start-hour H]\n"
                          "                       [--retention 0.70-0.99] [--json]\n"
                          "  omacards skill install [--json]\n"
                          "\n"
                          "Agent integration: run 'omacards skill install' to install the skill to %1.\n")
        .arg(skillTargetPath());
}

struct CommandHelpText {
    QString purpose;
    QString flags;
    QString example;
};

// Help prose for the registry's command paths. Command discovery still comes
// from commandPaths(); this table only supplies the wording for each path.
QString commandHelp(const QString &command) {
    if (command.isEmpty() || Cli::isHelpCommand(command.section(QLatin1Char(' '), 0, 0)) ||
        !Cli::commandPaths().contains(command))
        return usage();

    static const QHash<QString, CommandHelpText> texts = {
        {QStringLiteral("add-card"),
         {QStringLiteral("Add a Card to a Deck."),
          QStringLiteral("-q Q, -a A, [-d D], [--type T], [--tags a,b], [--json], --help, -h"),
          QStringLiteral("omacards add-card -q \"Two plus two?\" -a four --json")}},
        {QStringLiteral("add-cards"),
         {QStringLiteral("Add Cards from JSON or stdin."),
          QStringLiteral("--deck D, [--description TEXT], [--input FILE|-], [--json], --help, -h"),
          QStringLiteral("omacards add-cards --deck Calculus --input cards.json --json")}},
        {QStringLiteral("add-deck"),
         {QStringLiteral("Create a Deck."), QStringLiteral("NAME, [--description TEXT], [--json], --help, -h"),
          QStringLiteral("omacards add-deck Calculus --json")}},
        {QStringLiteral("cards"),
         {QStringLiteral("List Cards, optionally in one Deck."), QStringLiteral("[-d D], [--json], --help, -h"),
          QStringLiteral("omacards cards -d Calculus --json")}},
        {QStringLiteral("check"),
         {QStringLiteral("Show decks, Cards, due count, math and Deck issues."),
          QStringLiteral("[--deck NAME], [--math], [--strict], [--json], --help, -h"),
          QStringLiteral("omacards check --deck Calculus --json")}},
        {QStringLiteral("config"),
         {QStringLiteral("Show or change Settings."),
          QStringLiteral("[--new-card-allowance N], [--day-start-hour H], [--retention 0.70-0.99], "
                         "[--json], --help, -h"),
          QStringLiteral("omacards config --new-card-allowance 7 --json")}},
        {QStringLiteral("cram"),
         {QStringLiteral("Simulate a Cram session."), QStringLiteral("[-d D], [--steps N], [--json], --help, -h"),
          QStringLiteral("omacards cram -d Calculus --steps 4 --json")}},
        {QStringLiteral("decks"),
         {QStringLiteral("List decks."), QStringLiteral("[--json], --help, -h"),
          QStringLiteral("omacards decks --json")}},
        {QStringLiteral("decks current"),
         {QStringLiteral("Show the Current deck."), QStringLiteral("[--json], --help, -h"),
          QStringLiteral("omacards decks current --json")}},
        {QStringLiteral("delete-card"),
         {QStringLiteral("Delete a Card."), QStringLiteral("-c ID, [-d D], [--json], --help, -h"),
          QStringLiteral("omacards delete-card -c cdbec825f20d --json")}},
        {QStringLiteral("delete-deck"),
         {QStringLiteral("Delete a Deck."), QStringLiteral("NAME, [--json], --help, -h"),
          QStringLiteral("omacards delete-deck Calculus --json")}},
        {QStringLiteral("due"),
         {QStringLiteral("List Cards due on the current Study day."), QStringLiteral("[-d D], [--json], --help, -h"),
          QStringLiteral("omacards due -d Calculus --json")}},
        {QStringLiteral("edit-card"),
         {QStringLiteral("Edit a Card's Question, Answer, type or Tags."),
          QStringLiteral("-c ID, [-q Q], [-a A], [--type T], [--tags a,b], [--json], --help, -h"),
          QStringLiteral("omacards edit-card -c cdbec825f20d -a five --json")}},
        {QStringLiteral("import"),
         {QStringLiteral("Import Cards from a file."), QStringLiteral("PATH, [-d D], [--json], --help, -h"),
          QStringLiteral("omacards import cards.json -d Calculus --json")}},
        {QStringLiteral("open"),
         {QStringLiteral("Start the GUI."), QStringLiteral("--help, -h"), QStringLiteral("omacards open")}},
        {QStringLiteral("preview"),
         {QStringLiteral("Preview a Deck's Cards."),
          QStringLiteral("--deck D, [--card ID], [--format text|html], [--json], --help, -h"),
          QStringLiteral("omacards preview --deck Calculus --json")}},
        {QStringLiteral("review"),
         {QStringLiteral("Record a Rating for a Card."), QStringLiteral("-c ID, -r 1|3, [--json], --help, -h"),
          QStringLiteral("omacards review -c cdbec825f20d -r 3 --json")}},
        {QStringLiteral("search"),
         {QStringLiteral("Search Cards."), QStringLiteral("TERM, [--deck D], [--json], --help, -h"),
          QStringLiteral("omacards search Taylor --json")}},
        {QStringLiteral("skill"),
         {QStringLiteral("Manage the agent skill. Bare `omacards skill` shows status and help without installing."),
          QStringLiteral("[install], [--json], --help, -h"), QStringLiteral("omacards skill --help")}},
        {QStringLiteral("skill install"),
         {QStringLiteral("Install the agent skill."), QStringLiteral("[--json], --help, -h"),
          QStringLiteral("omacards skill install --json")}},
        {QStringLiteral("stats"),
         {QStringLiteral("Show study statistics."), QStringLiteral("[--json], --help, -h"),
          QStringLiteral("omacards stats --json")}},
        {QStringLiteral("use-deck"),
         {QStringLiteral("Choose the Current deck."), QStringLiteral("NAME, [--json], --help, -h"),
          QStringLiteral("omacards use-deck Calculus --json")}},
        {QStringLiteral("warm"),
         {QStringLiteral("Render a Deck's math SVGs ahead of study."), QStringLiteral("--deck D, [--json], --help, -h"),
          QStringLiteral("omacards warm --deck Calculus --json")}},
    };
    const CommandHelpText text = texts.value(command);
    const QString purpose = text.purpose.isEmpty() ? QObject::tr("Run the %1 command.").arg(command) : text.purpose;
    const QString flags = text.flags.isEmpty() ? QStringLiteral("--help, -h") : text.flags;
    const QString example = text.example.isEmpty() ? QStringLiteral("omacards %1 --help").arg(command) : text.example;
    QString help = QStringLiteral("omacards %1 --help\n\nPurpose: %2\nFlags: %3\nExample: %4\n")
                       .arg(command, purpose, flags, example);
    if (command == QLatin1String("skill") || command == QLatin1String("skill install"))
        help += skillHelpNotes();
    return help;
}
} // namespace

namespace Cli {

QStringList commandPaths() {
    static const QStringList paths = readCommandPaths();
    return paths;
}

QStringList headlessCommands() {
    static const QStringList commands = [] {
        QStringList result;
        for (const QString &path : commandPaths()) {
            const QString token = commandToken(path);
            if (token != guiCommand() && !result.contains(token))
                result << token;
        }
        return result;
    }();
    return commands;
}

bool isHelpCommand(const QString &command) {
    static const QStringList helpCommands = [] {
        QStringList result;
        for (const QString &path : commandPaths()) {
            const QString token = commandToken(path);
            const bool isHelp = token == QLatin1String("help") || token.startsWith(QLatin1Char('-'));
            if (isHelp && !result.contains(token))
                result << token;
        }
        return result;
    }();
    return helpCommands.contains(command);
}

bool needsOffscreen(const QStringList &arguments) {
    if (arguments.isEmpty() || arguments.first().isEmpty())
        return false; // no command: start the GUI
    if (hasHelpFlag(arguments))
        return true; // probing syntax must never start the GUI event loop
    return arguments.first() != guiCommand();
}

int run(const QStringList &arguments) {
    const Options options = parse(arguments);
    // Help is answered before positional parsing reaches a command body, so
    // a probe cannot create a Deck, install the skill or run a command.
    if (options.help) {
        const QString key =
            options.path.isEmpty() ? options.command : options.command + QLatin1Char(' ') + options.path;
        fprintf(stdout, "%s", qPrintable(commandHelp(commandPaths().contains(key) ? key : options.command)));
        return 0;
    }
    if (options.command.isEmpty() || isHelpCommand(options.command)) {
        fprintf(stdout, "%s", qPrintable(usage()));
        // `help` is the one text-mode entry point that nudges an agent towards
        // the skill. Help flags and JSON probes stay silent so they keep their
        // existing clean contracts.
        if (options.command == QLatin1String("help") && !options.json && !readSkillStatus().installed) {
            const QString nudge =
                QObject::tr("Agent skill not installed; run 'omacards skill install' to install it to %1")
                    .arg(skillTargetPath());
            fprintf(stderr, "%s\n", qPrintable(nudge));
        }
        return 0;
    }
    if (!isHeadlessCommand(options.command))
        return fail(QObject::tr("Unknown command: %1").arg(options.command), options.json);
    if (!options.unknownFlag.isEmpty())
        return fail(QObject::tr("Unknown flag: %1").arg(options.unknownFlag), options.json);

    // Bare `skill` is a read-only probe: report status and help, and skip
    // backend initialization so neither the state database nor the skill is
    // written. `skill status` will reuse this same snapshot.
    if (options.command == "skill" && options.path.isEmpty()) {
        const SkillStatus status = readSkillStatus();
        if (options.json) {
            write(skillStatusJson(status), true, {});
        } else {
            fprintf(stdout, "%s\n\n%s", qPrintable(skillStatusText(status)),
                    qPrintable(commandHelp(QStringLiteral("skill"))));
        }
        return 0;
    }

    auto ownedBackend = Backend::fromEnvironment();
    Backend &backend = *ownedBackend;

    // `warm` reads Deck files only. Skipping initialize() keeps the study
    // session from presenting a card (and queueing its math) in a headless
    // process, and leaves the selected Deck and study order untouched.
    if (options.command == "warm") {
        const bool json = options.json;
        if (options.deck.isEmpty())
            return fail(QObject::tr("Use --deck / -d"), json);
        const QVector<Deck> warmDecks = DeckIO::loadAll(backend.decksDirectory());
        const QString targetPath = resolveDeckPath(warmDecks, backend.decksDirectory(), options.deck);
        if (targetPath.isEmpty())
            return fail(QObject::tr("No such deck: %1").arg(options.deck), json);

        QVector<Deck> targets;
        for (const Deck &deck : warmDecks) {
            if (deck.path == targetPath)
                targets << deck;
        }
        if (targets.isEmpty()) {
            QString loadError;
            targets << DeckIO::load(targetPath, &loadError);
        }
        const QVector<Card> cards = allCards(targets, {});
        if (cards.isEmpty())
            return failEmptyDeck(options.deck, deckIssueReport(backend.decksDirectory(), targetPath), json);

        MathRenderer renderer;
        int rendered = 0;
        int cached = 0;
        for (const Card &card : cards) {
            int fieldCached = 0;
            rendered += renderer.warm(DeckIO::displayQuestion(card), &fieldCached);
            cached += fieldCached;
            fieldCached = 0;
            rendered += renderer.warm(DeckIO::displayAnswer(card), &fieldCached);
            cached += fieldCached;
        }

        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", options.deck);
        object.insert("rendered", rendered);
        object.insert("cached", cached);
        const QString human =
            QObject::tr("Warmed %1: %2 rendered · %3 cached").arg(options.deck).arg(rendered).arg(cached);
        write(object, json, human);
        return 0;
    }

    QString error;
    if (!backend.initialize(&error))
        return fail(error, options.json);
    const QVector<Deck> decks = backend.allDecks();
    const bool json = options.json;

    if (options.command == "check") {
        const QString targetPath =
            options.deck.isEmpty() ? QString() : resolveDeckPath(decks, backend.decksDirectory(), options.deck);
        if (!options.deck.isEmpty() && targetPath.isEmpty())
            return fail(QObject::tr("No such deck: %1").arg(options.deck), json);

        QVector<Deck> checkedDecks;
        if (targetPath.isEmpty()) {
            checkedDecks = decks;
        } else {
            for (const Deck &deck : decks) {
                if (deck.path == targetPath)
                    checkedDecks << deck;
            }
            if (checkedDecks.isEmpty()) {
                QString loadError;
                checkedDecks << DeckIO::load(targetPath, &loadError);
            }
        }

        const int total = allCards(checkedDecks, {}).size();
        const int due = backend.availableCount(options.deck);
        QJsonArray deckArray;
        for (const Deck &deck : checkedDecks) {
            QJsonObject object;
            object.insert("name", deck.name);
            object.insert("cards", deck.cards.size());
            deckArray << object;
        }
        QJsonArray mathWarnings;
        QStringList warningLines;
        if (options.math) {
            MathRenderer renderer;
            for (const Deck &deck : checkedDecks) {
                for (const Card &card : deck.cards) {
                    const auto checkField = [&](const QString &field, const QString &text) {
                        for (const QString &message : renderer.warnings(text)) {
                            QJsonObject warning;
                            warning.insert("card", card.id);
                            warning.insert("deck", deck.name);
                            warning.insert("field", field);
                            warning.insert("message", message);
                            mathWarnings << warning;
                            warningLines << QStringLiteral("%1 (%2.%3): %4").arg(card.id, deck.name, field, message);
                        }
                    };
                    checkField(QStringLiteral("question"), card.question);
                    checkField(QStringLiteral("answer"), card.answer);
                }
            }
        }

        const DeckIssueReport report = deckIssueReport(backend.decksDirectory(), targetPath);
        const bool strictPassed = !options.strict || report.entries.isEmpty();

        QJsonObject object;
        object.insert("ok", report.errors == 0);
        if (options.strict)
            object.insert("strictPassed", strictPassed);
        object.insert("version", QString("0.1.0"));
        if (!options.deck.isEmpty())
            object.insert("deck", options.deck);
        object.insert("decks", deckArray);
        object.insert("cards", total);
        object.insert("due", due);
        object.insert("totalDue", backend.countDue(options.deck));
        object.insert("newCardsOnStudyDay", backend.reviewSummary().newCardsOnStudyDay);
        object.insert("newCardAllowance", backend.newCardAllowance());
        object.insert("retention", backend.desiredRetention());
        object.insert("dayStartHour", backend.dayStartHour());
        object.insert("math", backend.mathEngine());
        object.insert("decksDir", backend.decksDirectory());
        object.insert("database", backend.databasePath());
        object.insert("issues", report.entries);
        object.insert("issueCounts", issueCountsJson(report));
        if (options.math)
            object.insert("mathWarnings", mathWarnings);
        QString human = QObject::tr("%1 decks · %2 cards · %3 due · math: %4")
                            .arg(checkedDecks.size())
                            .arg(total)
                            .arg(due)
                            .arg(backend.mathEngine());
        human += QStringLiteral("\n%1 issues · %2 errors · %3 warnings")
                     .arg(report.entries.size())
                     .arg(report.errors)
                     .arg(report.warnings);
        if (!report.lines.isEmpty())
            human += QLatin1String("\n") + report.lines.join(QLatin1String("\n"));
        if (options.math) {
            human += QLatin1String("\n");
            human += warningLines.isEmpty() ? QObject::tr("No math warnings") : warningLines.join('\n');
        }
        write(object, json, human);
        return strictPassed ? 0 : 1;
    }

    if (options.command == "preview") {
        if (options.deck.isEmpty())
            return fail(QObject::tr("Use --deck / -d"), json);
        if (options.format != QLatin1String("text") && options.format != QLatin1String("html"))
            return fail(QObject::tr("--format must be text or html"), json);
        QVector<Card> cards = allCards(decks, options.deck);
        const bool deckHasNoCards = cards.isEmpty();
        if (!options.card.isEmpty()) {
            QVector<Card> filtered;
            for (const Card &card : cards)
                if (card.id == options.card)
                    filtered << card;
            cards = filtered;
        }
        if (cards.isEmpty()) {
            // A --card miss keeps the old message; only an existing Deck file
            // that yields no Cards gets the empty-Deck diagnostic.
            if (deckHasNoCards) {
                const QString targetPath = resolveDeckPath(decks, backend.decksDirectory(), options.deck);
                if (!targetPath.isEmpty() && QFileInfo::exists(targetPath))
                    return failEmptyDeck(options.deck, deckIssueReport(backend.decksDirectory(), targetPath), json);
            }
            return fail(QObject::tr("No cards to preview"), json);
        }

        constexpr double previewMathScale = 1.6;              // Match the review answer label size.
        const QString previewInk = QStringLiteral("#000000"); // HTML stays readable on light backgrounds.
        MathRenderer renderer;
        QJsonArray items;
        QStringList lines;
        for (const Card &card : cards) {
            const bool html = options.format == QLatin1String("html");
            const QString question = html ? renderer.render(card.question, previewInk, previewMathScale)
                                          : renderer.renderPlain(card.question);
            const QString answer =
                html ? renderer.render(card.answer, previewInk, previewMathScale) : renderer.renderPlain(card.answer);
            QJsonObject item;
            item.insert("id", card.id);
            item.insert(html ? QStringLiteral("questionHtml") : QStringLiteral("question"), question);
            item.insert(html ? QStringLiteral("answerHtml") : QStringLiteral("answer"), answer);
            items << item;
            lines << QStringLiteral("[%1]\nQ: %2\nA: %3").arg(card.id, question, answer);
        }
        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", options.deck);
        object.insert("format", options.format);
        object.insert("items", items);
        write(object, json, lines.join(QStringLiteral("\n\n")));
        return 0;
    }

    if (options.command == "config") {
        if (options.retention > 0 &&
            (options.retention < Config::kMinRetention || options.retention > Config::kMaxRetention))
            return fail(QObject::tr("Retention must be between 0.70 and 0.99"), json);
        if (options.newCardAllowance > Config::kMaxNewPerDay)
            return fail(QObject::tr("New-card allowance must be 9999 or less"), json);
        if (options.dayStartHour < -1 || options.dayStartHour > Config::kMaxDayStartHour)
            return fail(QObject::tr("Day start hour must be between 0 and 23"), json);
        // The QML setters report write failures as signals; the CLI must turn
        // them into a non-zero exit instead of claiming success.
        QString settingError;
        const QMetaObject::Connection connection =
            QObject::connect(&backend, &Backend::errorOccurred, &backend, [&](const QString &message) {
                if (settingError.isEmpty())
                    settingError = message;
            });
        if (options.newCardAllowance >= 0)
            backend.setNewCardAllowance(options.newCardAllowance);
        if (options.dayStartHour >= 0)
            backend.setDayStartHour(options.dayStartHour);
        if (options.retention > 0)
            backend.setDesiredRetention(options.retention);
        QObject::disconnect(connection);
        if (!settingError.isEmpty())
            return fail(settingError, json);

        const ReviewSummary summary = backend.reviewSummary();
        QJsonObject object;
        object.insert("ok", true);
        object.insert("newCardAllowance", backend.newCardAllowance());
        object.insert("retention", backend.desiredRetention());
        object.insert("dayStartHour", backend.dayStartHour());
        object.insert("newCardsOnStudyDay", summary.newCardsOnStudyDay);
        object.insert("newRemaining", qMax(0, backend.newCardAllowance() - summary.newCardsOnStudyDay));
        write(object, json,
              QObject::tr("New-card allowance: %1 · new cards on Study day: %2 · desired retention: %3% · "
                          "day-start hour: %4")
                  .arg(backend.newCardAllowance())
                  .arg(summary.newCardsOnStudyDay)
                  .arg(qRound(backend.desiredRetention() * 100))
                  .arg(QStringLiteral("%1:00").arg(backend.dayStartHour(), 2, 10, QLatin1Char('0'))));
        return 0;
    }

    if (options.command == "stats") {
        const ReviewSummary summary = backend.reviewSummary();
        QJsonObject object;
        object.insert("ok", true);
        object.insert("totalRatings", summary.total);
        object.insert("studyDayRatings", summary.studyDayRatings);
        object.insert("studyDayGood", summary.studyDayGood);
        object.insert("streak", summary.streak);
        object.insert("newCardsOnStudyDay", summary.newCardsOnStudyDay);
        object.insert("newCardAllowance", backend.newCardAllowance());
        object.insert("due", backend.availableCount());
        object.insert("totalDue", backend.countDue());
        object.insert("cards", allCards(decks, {}).size());
        object.insert("decks", decks.size());
        const int accuracy =
            summary.studyDayRatings > 0 ? qRound(100.0 * summary.studyDayGood / summary.studyDayRatings) : 0;
        write(object, json,
              QObject::tr("Ratings on Study day: %1 · %2% Good · %3-day Streak · %4 due")
                  .arg(summary.studyDayRatings)
                  .arg(accuracy)
                  .arg(summary.streak)
                  .arg(backend.availableCount()));
        return 0;
    }

    if (options.command == "use-deck") {
        if (options.path.isEmpty())
            return fail(QObject::tr("Usage: omacards use-deck NAME"), json);
        const QString path = resolveDeckPath(decks, backend.decksDirectory(), options.path);
        QString selected;
        for (const Deck &deck : decks) {
            if (deck.path == path) {
                selected = deck.name;
                break;
            }
        }
        if (selected.isEmpty())
            return fail(QObject::tr("No such deck: %1").arg(options.path), json);
        backend.selectDeck(selected);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", selected);
        write(object, json, QObject::tr("Using deck %1").arg(selected));
        return 0;
    }

    if (options.command == "decks" && options.path == "current") {
        const QString selected = backend.currentDeck();
        if (selected.isEmpty())
            return fail(QObject::tr("No current deck"), json);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", selected);
        write(object, json, selected);
        return 0;
    }

    if (options.command == "decks") {
        if (!options.path.isEmpty())
            return fail(QObject::tr("Unknown command: decks %1").arg(options.path), json);
        QJsonArray array;
        for (const Deck &deck : decks) {
            QJsonObject object;
            object.insert("name", deck.name);
            object.insert("path", deck.path);
            object.insert("cards", deck.cards.size());
            object.insert("due", backend.countDue(deck.name));
            array << object;
        }
        QJsonObject object;
        object.insert("ok", true);
        object.insert("decks", array);
        QStringList lines;
        for (const Deck &deck : decks)
            lines << QString("%1 — %2 cards, %3 due")
                         .arg(deck.name)
                         .arg(deck.cards.size())
                         .arg(backend.countDue(deck.name));
        write(object, json, lines.join('\n'));
        return 0;
    }

    if (options.command == "cards") {
        if (!options.deck.isEmpty() && resolveDeckPath(decks, backend.decksDirectory(), options.deck).isEmpty())
            return fail(QObject::tr("No such deck: %1").arg(options.deck), json);
        QJsonArray array;
        for (const Card &card : allCards(decks, options.deck))
            array << cardJson(card);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("cards", array);
        QStringList lines;
        for (const Card &card : allCards(decks, options.deck))
            lines << QString("[%1] %2 → %3").arg(card.id, card.question, card.answer);
        write(object, json, lines.join('\n'));
        return 0;
    }

    if (options.command == "search") {
        if (!options.deck.isEmpty() && resolveDeckPath(decks, backend.decksDirectory(), options.deck).isEmpty())
            return fail(QObject::tr("No such deck: %1").arg(options.deck), json);
        const QVector<Card> matches = backend.searchCards(options.path, options.deck);
        QJsonArray array;
        for (const Card &card : matches)
            array << cardJson(card);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("count", matches.size());
        object.insert("cards", array);
        QStringList lines;
        for (const Card &card : matches)
            lines << QString("[%1] %2 → %3").arg(card.id, card.question, card.answer);
        write(object, json, lines.join('\n'));
        return 0;
    }

    if (options.command == "add-deck") {
        if (options.path.isEmpty())
            return fail(QObject::tr("Give a deck name"), json);
        if (!backend.createDeck(options.path, &error, options.description))
            return fail(error, json);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", options.path);
        write(object, json, QObject::tr("Created deck %1").arg(options.path));
        return 0;
    }

    if (options.command == "add-cards") {
        if (options.deck.isEmpty())
            return fail(QObject::tr("Use --deck / -d"), json);
        QByteArray bytes;
        if (!readCardInput(options, &bytes, &error))
            return fail(error, json);
        QVector<CardDraft> drafts;
        QString description;
        if (!parseCardDrafts(bytes, options.description, &drafts, &description, &error))
            return fail(error, json);
        QStringList ids;
        if (!backend.createCards(options.deck, drafts, &description, &ids, &error))
            return fail(error, json);

        // Reloaded after the write so an implicitly created Deck has the same
        // stored path that decks --json reports. resolveDeckPath's filename
        // fallback covers a Deck that load() skips after the write; only the
        // written Deck is linted.
        const QString writtenPath =
            resolveDeckPath(backend.allDecks(), backend.decksDirectory(), options.deck.trimmed());
        const DeckIssueReport report = deckIssueReportForFile(writtenPath);

        QJsonArray idArray;
        for (const QString &id : ids)
            idArray << id;
        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", options.deck);
        object.insert("created", drafts.size());
        object.insert("ids", idArray);
        object.insert("path", writtenPath);
        object.insert("issueCounts", issueCountsJson(report));
        write(object, json, QObject::tr("Added %1 cards to %2").arg(drafts.size()).arg(options.deck));
        return 0;
    }

    if (options.command == "add-card") {
        if (options.question.isEmpty())
            return fail(QObject::tr("Use --question / -q"), json);
        CardDraft draft;
        draft.question = options.question;
        draft.answer = options.answer;
        draft.type = options.type;
        draft.tags = options.tags.split(',', Qt::SkipEmptyParts);
        QStringList ids;
        if (!backend.createCards(options.deck, {draft}, nullptr, &ids, &error))
            return fail(error, json);
        const Card *created = nullptr;
        for (const Deck &deck : backend.allDecks())
            for (const Card &card : deck.cards)
                if (card.id == ids.value(0)) {
                    created = &card;
                    break;
                }
        QJsonObject object;
        object.insert("ok", true);
        if (created) {
            object.insert("id", created->id);
            object.insert("card", cardJson(*created));
        }
        write(object, json, QObject::tr("Added card"));
        return 0;
    }

    if (options.command == "edit-card") {
        if (options.card.isEmpty())
            return fail(QObject::tr("Use --card / -c"), json);
        const QVector<Card> cards = allCards(decks, options.deck);
        const Card *target = nullptr;
        for (const Card &card : cards)
            if (card.id == options.card)
                target = &card;
        if (!target)
            return fail(QObject::tr("No such card: %1").arg(options.card), json);

        const QString question = options.questionSet ? options.question : target->question;
        const QString answer = options.answerSet ? options.answer : target->answer;
        const QString type = options.typeSet ? options.type : Domain::toString(target->type);
        QString tags;
        if (options.tagsSet) {
            tags = options.tags;
        } else {
            tags = target->tags.join(", ");
        }
        const QString deckName = options.deck.isEmpty() ? target->deck : options.deck;
        QString finalId;
        if (!backend.modifyCard(deckName, options.card, question, answer, type, tags, &error, &finalId))
            return fail(error, json);

        const Card *updated = nullptr;
        for (const Deck &deck : backend.allDecks())
            for (const Card &card : deck.cards)
                if (card.id == finalId) {
                    updated = &card;
                    break;
                }

        QJsonObject object;
        object.insert("ok", true);
        object.insert("id", finalId.isEmpty() ? options.card : finalId);
        if (updated)
            object.insert("card", cardJson(*updated));
        write(object, json, QObject::tr("Updated card %1").arg(options.card));
        return 0;
    }

    if (options.command == "delete-card") {
        if (options.card.isEmpty())
            return fail(QObject::tr("Use --card / -c"), json);
        QString deckName = options.deck;
        if (deckName.isEmpty()) {
            for (const Deck &deck : decks)
                for (const Card &card : deck.cards)
                    if (card.id == options.card)
                        deckName = deck.name;
            if (deckName.isEmpty())
                return fail(QObject::tr("No such card: %1").arg(options.card), json);
        }
        if (!backend.deleteCard(deckName, options.card, &error))
            return fail(error, json);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("id", options.card);
        write(object, json, QObject::tr("Removed card %1").arg(options.card));
        return 0;
    }

    if (options.command == "delete-deck") {
        const QString name = options.path.isEmpty() ? options.deck : options.path;
        if (name.isEmpty())
            return fail(QObject::tr("Give a deck name"), json);
        if (!backend.removeDeck(name, &error))
            return fail(error, json);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", name);
        write(object, json, QObject::tr("Removed deck %1").arg(name));
        return 0;
    }

    if (options.command == "rename-deck") {
        if (options.path.isEmpty() || options.newName.isEmpty())
            return fail(QObject::tr("Give the old and new deck names"), json);
        DeckRename::Result result;
        if (!backend.renameDeck(options.path, options.newName, &result, &error))
            return fail(error, json);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("deck", result.name);
        object.insert("path", result.path);
        object.insert("renamedCards", result.cards);
        object.insert("preservedIds", result.preservedIds);
        write(object, json, QObject::tr("Renamed deck %1 to %2").arg(options.path, result.name));
        return 0;
    }

    if (options.command == "import") {
        if (options.path.isEmpty())
            return fail(QObject::tr("Give a file to import"), json);
        if (options.path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            QFile file(options.path);
            if (!file.open(QIODevice::ReadOnly))
                return fail(QObject::tr("Cannot read: %1").arg(options.path), json);
            QVector<CardDraft> drafts;
            QString description;
            if (!parseCardDrafts(file.readAll(), options.description, &drafts, &description, &error))
                return fail(error, json);
            const QString target = options.deck.isEmpty() ? QFileInfo(options.path).completeBaseName() : options.deck;
            QStringList ids;
            if (!backend.createCards(target, drafts, &description, &ids, &error))
                return fail(error, json);
            QJsonObject object;
            object.insert("ok", true);
            object.insert("file", options.path);
            object.insert("deck", target);
            object.insert("cards", drafts.size());
            write(object, json, QObject::tr("Imported %1 cards from %2").arg(drafts.size()).arg(options.path));
            return 0;
        }
        if (!backend.importFile(options.path, options.deck, &error))
            return fail(error, json);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("file", options.path);
        write(object, json, QObject::tr("Imported %1").arg(options.path));
        return 0;
    }

    if (options.command == "due") {
        if (!options.deck.isEmpty() && resolveDeckPath(decks, backend.decksDirectory(), options.deck).isEmpty())
            return fail(QObject::tr("No such deck: %1").arg(options.deck), json);
        const QVector<Card> cards = backend.dueCards(options.deck);
        QJsonArray array;
        for (const Card &card : cards)
            array << cardJson(card);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("count", cards.size());
        object.insert("totalDue", backend.countDue(options.deck));
        object.insert("cards", array);
        write(object, json, QString::number(cards.size()));
        return 0;
    }

    if (options.command == "review") {
        const QVector<Card> cards = allCards(decks, options.deck);
        const Card *target = nullptr;
        for (const Card &card : cards)
            if (card.id == options.card)
                target = &card;
        if (!target)
            return fail(QObject::tr("No such card: %1").arg(options.card), json);
        const Rating rating = Domain::ratingFromInt(options.rating);
        if (rating == Rating::Invalid)
            return fail(QObject::tr("Rating must be 1 (Again) or 3 (Good)"), json);

        Store store(backend.databasePath());
        if (!store.open(&error))
            return fail(error, json);
        StoreRatingAdapter storeAdapter(store);
        RatingRecording recording(storeAdapter);
        const RatingRecording::Result result =
            recording.record(*target, rating, backend.desiredRetention(), QDateTime::currentMSecsSinceEpoch());
        if (!result.ok) {
            const QString message = result.failurePhase == RatingRecording::FailurePhase::SchedulingRead
                                        ? QObject::tr("Cannot read scheduling state: %1")
                                        : QObject::tr("Cannot record rating: %1");
            return fail(message.arg(result.storeError), json);
        }

        QJsonObject object;
        object.insert("ok", true);
        object.insert("id", target->id);
        object.insert("rating", Domain::toInt(rating));
        object.insert("intervalDays", result.intervalDays);
        object.insert("due", result.state.due);
        object.insert("stability", result.state.stability);
        object.insert("difficulty", result.state.difficulty);
        write(object, json, QObject::tr("Next due in %1 day(s)").arg(result.intervalDays));
        return 0;
    }

    if (options.command == "cram") {
        const QVector<Card> cards = allCards(decks, options.deck);
        if (cards.isEmpty())
            return fail(QObject::tr("No cards in this deck"), json);
        QVector<Card> ordered = cards;
        std::sort(ordered.begin(), ordered.end(), [](const Card &a, const Card &b) { return a.id < b.id; });
        InSessionProgression progression;
        progression.buildCram(ordered);

        const int steps = qMax(1, options.steps);
        int good = 0;
        for (int step = 0; step < steps; ++step) {
            const InSessionProgression::NextResult current = progression.next(0);
            const Rating rating = (step % 4 == 3) ? Rating::Again : Rating::Good;
            progression.advanceCram(current.cardId, rating);
            if (rating == Rating::Good)
                ++good;
        }
        QJsonObject object;
        object.insert("ok", true);
        object.insert("ratings", steps);
        object.insert("good", good);
        object.insert("leitnerBox", progression.currentLeitnerBox()); // already 1-based
        write(object, json,
              QObject::tr("%1 reviews · %2 Good · box %3").arg(steps).arg(good).arg(progression.currentLeitnerBox()));
        return 0;
    }

    if (options.command == "skill") {
        const QString action = options.path.isEmpty() ? QString("install") : options.path;
        if (action != "install")
            return fail(QObject::tr("Usage: omacards skill install"), json);
        QFile bundled(QStringLiteral(":/skill/SKILL.md"));
        if (!bundled.open(QIODevice::ReadOnly))
            return fail(QObject::tr("The bundled agent skill is missing"), json);
        const QString path = skillTargetPath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate) || output.write(bundled.readAll()) < 0)
            return fail(QObject::tr("Cannot write %1").arg(path), json);
        QJsonObject object;
        object.insert("ok", true);
        object.insert("path", path);
        write(object, json, QObject::tr("Installed agent skill to %1").arg(path));
        return 0;
    }

    return fail(QObject::tr("Unknown command: %1").arg(options.command), json);
}

} // namespace Cli
