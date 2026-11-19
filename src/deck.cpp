// Deck files are the source of truth: plain text an agent can edit with sed,
// and a sidecar SQLite database holds only scheduling state.
#include "deck.h"
#include "mathrender.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QRegularExpression>

namespace DeckIO {

static QString fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return {};
}

template <typename Replacement>
static QString replaceMatches(const QString &text, const QRegularExpression &expression, Replacement replacement) {
    QString out;
    int last = 0;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        out += text.mid(last, match.capturedStart() - last) + replacement(match);
        last = match.capturedEnd();
    }
    return out + text.mid(last);
}

static void appendLine(QString &field, const QString &line) {
    if (field.isEmpty())
        field = line;
    else
        field += "\n" + line;
}

QString cardId(const Card &card) {
    const QString source = card.deck + "\x1f" + card.question + "\x1f" + card.answer;
    return QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
}

Deck load(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(error, QObject::tr("Cannot read deck file: %1").arg(path)), Deck{};

    const QString text = QString::fromUtf8(file.readAll());
    const QStringList lines = text.split('\n');
    Deck deck;
    deck.path = path;
    deck.name = QFileInfo(path).completeBaseName();

    static const QRegularExpression header(R"re(^#\s*(Deck|Description)\s*:\s*(.*)$)re",
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression keyLine(R"re(^(Q|A|Answer|tags|type|id)\s*:\s*(.*)$)re",
                                             QRegularExpression::CaseInsensitiveOption);

    QStringList block;
    int startLine = 0;
    auto parseBlock = [&](const QStringList &body, int lineNumber) {
        Card card;
        card.deck = deck.name;
        QString currentKey;
        for (int i = 0; i < body.size(); ++i) {
            const QString raw = body.at(i);
            if (raw.trimmed().isEmpty())
                continue;
            const auto match = keyLine.match(raw);
            if (match.hasMatch()) {
                const QString key = match.captured(1).toLower();
                const QString value = match.captured(2).trimmed();
                if (key == "q") {
                    currentKey = "q";
                    appendLine(card.question, value);
                } else if (key == "a" || key == "answer") {
                    currentKey = "a";
                    appendLine(card.answer, value);
                } else if (key == "tags") {
                    currentKey = "tags";
                    card.tags << value.split(',', Qt::SkipEmptyParts);
                } else if (key == "type") {
                    card.type = value.isEmpty() ? "basic" : value.toLower();
                    currentKey.clear();
                } else if (key == "id") {
                    card.id = value;
                    card.explicitId = !value.isEmpty();
                    currentKey.clear();
                }
            } else if (raw.startsWith(QLatin1String("  ")) && !currentKey.isEmpty()) {
                const QString continued = raw.trimmed();
                if (currentKey == "tags") {
                    // Tags continue as a comma-separated line.
                    card.tags << continued.split(',', Qt::SkipEmptyParts);
                } else {
                    appendLine(currentKey == "q" ? card.question : card.answer, continued);
                }
            } else if (!raw.trimmed().startsWith('#')) {
                if (error)
                    *error = QObject::tr("Unexpected line %1 in %2: %3")
                                 .arg(lineNumber + i).arg(path, raw);
            }
        }
        card.question = card.question.trimmed();
        card.answer = card.answer.trimmed();
        for (QString &tag : card.tags)
            tag = tag.trimmed();
        card.tags.removeAll(QString());
        if (card.question.isEmpty())
            card.question = displayQuestion(card);
        if (card.question.isEmpty()) {
            if (error)
                *error = QObject::tr("Card without a question in %1 near line %2").arg(path).arg(lineNumber);
            return;
        }
        if (card.id.isEmpty())
            card.id = cardId(card);
        deck.cards << card;
    };

    bool inHeader = true;
    for (int i = 0; i < lines.size(); ++i) {
        const QString raw = lines.at(i);
        if (raw.trimmed() == QLatin1String("---")) {
            if (!block.isEmpty()) {
                parseBlock(block, startLine);
                block.clear();
            }
            inHeader = false;
            startLine = i + 1;
            continue;
        }
        if (inHeader) {
            const auto match = header.match(raw);
            if (match.hasMatch()) {
                const QString value = match.captured(2).trimmed();
                if (match.captured(1).compare("Deck", Qt::CaseInsensitive) == 0 && !value.isEmpty())
                    deck.name = value;
                else if (match.captured(1).compare("Description", Qt::CaseInsensitive) == 0)
                    deck.description = value;
            }
            continue;
        }
        block << raw;
    }
    if (!block.isEmpty())
        parseBlock(block, startLine);
    return deck;
}

QVector<Deck> loadAll(const QString &directory, QString *error) {
    QVector<Deck> decks;
    const QDir dir(directory);
    if (!dir.exists()) {
        if (error)
            *error = QObject::tr("Deck directory does not exist: %1").arg(directory);
        return decks;
    }
    const auto files = dir.entryList({"*.txt", "*.deck"}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        QString deckError;
        Deck deck = load(dir.filePath(name), &deckError);
        if (!deckError.isEmpty()) {
            if (error)
                *error = deckError;
            continue;
        }
        decks << deck;
    }
    return decks;
}

QVector<DeckIssue> lint(const QString &directory) {
    QVector<DeckIssue> issues;
    const QDir dir(directory);
    if (!dir.exists())
        return issues;

    static const QRegularExpression keyLine(R"re(^(Q|A|Answer|tags|type|id)\s*:\s*(.*)$)re",
                                            QRegularExpression::CaseInsensitiveOption);
    MathRenderer math;
    const auto questionLine = [](const QStringList &lines, const QString &question, int occurrence = 1) {
        const QString expected = QStringLiteral("Q: ") + question.trimmed();
        int seen = 0;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).trimmed() == expected && ++seen == occurrence)
                return i + 1;
        }
        return 0;
    };

    const QStringList files = dir.entryList({"*.txt", "*.deck"}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        const QString path = dir.filePath(name);
        QString error;
        const Deck deck = load(path, &error);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            DeckIssue issue;
            issue.path = path;
            issue.severity = QStringLiteral("error");
            issue.message = QStringLiteral("Cannot read deck file");
            issue.hint = QStringLiteral("Check file permissions.");
            issues << issue;
            continue;
        }
        const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');

        bool inBlock = false;
        QString currentKey;
        for (int i = 0; i < lines.size(); ++i) {
            const QString raw = lines.at(i);
            if (!inBlock) {
                if (raw.trimmed() == QLatin1String("---"))
                    inBlock = true;
                continue;
            }
            if (raw.trimmed() == QLatin1String("---")) {
                currentKey.clear();
                continue;
            }
            if (raw.trimmed().isEmpty())
                continue;
            if (keyLine.match(raw).hasMatch()) {
                currentKey = keyLine.match(raw).captured(1).toLower();
                continue;
            }
            if (raw.startsWith(QLatin1String("  "))) {
                if (raw.startsWith(QLatin1String("   "))) {
                    DeckIssue issue;
                    issue.path = path;
                    issue.line = i + 1;
                    issue.message = QStringLiteral("Continuation line has more than two leading spaces");
                    issue.hint = QStringLiteral("Start multiline Q: or A: with exactly two spaces.");
                    issues << issue;
                }
                continue;
            }
            if (raw.trimmed().startsWith('#'))
                continue;
            DeckIssue issue;
            issue.path = path;
            issue.line = i + 1;
            issue.message = QStringLiteral("Unexpected line in deck file");
            issue.hint = QStringLiteral("Use Q:, A:, tags:, type:, id:, or indent with exactly two spaces.");
            issues << issue;
        }

        if (!error.isEmpty() && !error.startsWith(QStringLiteral("Unexpected line"))) {
            DeckIssue issue;
            issue.path = path;
            issue.severity = QStringLiteral("error");
            issue.message = error;
            issue.hint = QStringLiteral("Fix the format above and run omacards check --json again.");
            issues << issue;
        }

        QHash<QString, int> seenQuestions;
        for (const Card &card : deck.cards) {
            const QString key = card.question.trimmed().toCaseFolded();
            const int occurrence = key.isEmpty() ? 1 : seenQuestions.value(key, 0) + 1;
            if (!key.isEmpty())
                seenQuestions.insert(key, occurrence);
            const int line = questionLine(lines, card.question, occurrence);
            if (occurrence > 1) {
                DeckIssue issue;
                issue.path = path;
                issue.card = card.id;
                issue.line = line;
                issue.message = QStringLiteral("Duplicate question in deck");
                issue.hint = QStringLiteral("Question already used on line %1; make it distinct.")
                    .arg(questionLine(lines, card.question, 1));
                issues << issue;
            }
            const auto addMathWarnings = [&](const QString &field, const QString &text) {
                for (const QString &message : math.warnings(text)) {
                    DeckIssue issue;
                    issue.path = path;
                    issue.card = card.id;
                    issue.line = line;
                    issue.message = QStringLiteral("Math warning in %1: %2").arg(field, message);
                    issue.hint = QStringLiteral("Use supported macros, or install tectonic for LaTeX rendering.");
                    issues << issue;
                }
            };
            addMathWarnings(QStringLiteral("question"), card.question);
            addMathWarnings(QStringLiteral("answer"), card.answer);
        }
    }
    return issues;
}

QString serialize(const Deck &deck) {
    QStringList out;
    out << "# Deck: " + deck.name;
    if (!deck.description.isEmpty())
        out << "# Description: " + deck.description;
    for (const Card &card : deck.cards) {
        out << "---";
        out << "Q: " + card.question;
        out << "A: " + card.answer;
        if (card.type != "basic")
            out << "type: " + card.type;
        if (!card.tags.isEmpty())
            out << "tags: " + card.tags.join(", ");
        // Only write the id when it pins the card; hashes stay implicit so the
        // file keeps reading like plain notes.
        if (card.explicitId || (!card.id.isEmpty() && card.id != cardId(card)))
            out << "id: " + card.id;
    }
    return out.join('\n') + "\n";
}

bool save(const Deck &deck, QString *error) {
    if (deck.path.isEmpty()) {
        if (error)
            *error = QObject::tr("Deck %1 has no file path").arg(deck.name);
        return false;
    }
    QSaveFile file(deck.path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QObject::tr("Cannot write deck file: %1").arg(deck.path);
        return false;
    }
    file.write(serialize(deck).toUtf8());
    if (!file.commit()) {
        if (error)
            *error = QObject::tr("Cannot save deck file: %1").arg(deck.path);
        return false;
    }
    return true;
}

// Cloze cards keep the full sentence in Q and the answer is derived from the
// {{c1::hidden}} markers, so agents can write them by hand.
static QString stripCloze(const QString &text, bool reveal) {
    static const QRegularExpression cloze(R"re(\{\{c\d+::(.*?)\}\})re");
    if (reveal) {
        return replaceMatches(text, cloze, [](const QRegularExpressionMatch &m) { return m.captured(1); });
    }
    QString hidden = replaceMatches(text, cloze, [](const QRegularExpressionMatch &m) {
        return QString("[ %1 ]").arg(QString(m.captured(1).length(), QChar(0x00B7)));
    });
    hidden.remove(QRegularExpression(R"re(\{\{c\d+::(.*?)::.*?\}\})re"));
    return hidden;
}

QString contentHash(const Card &card) {
    const QString source = card.question + "\x1f" + card.answer + "\x1f" + card.type;
    return QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString safeFileName(const QString &name) {
    QString result = name.trimmed();
    result.replace(QRegularExpression(R"re([^\w.-]+)re"), "-");
    result.remove(QRegularExpression(R"re(^-+|-+$)re"));
    return result.isEmpty() ? QStringLiteral("Deck") : result;
}

QString displayQuestion(const Card &card) {
    if (card.type == "cloze")
        return stripCloze(card.question, false);
    return card.question;
}

QString displayAnswer(const Card &card) {
    if (card.type == "cloze")
        return stripCloze(card.question, true);
    return card.answer;
}

} // namespace DeckIO
