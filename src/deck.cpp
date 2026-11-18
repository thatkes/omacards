// Deck files are the source of truth: plain text an agent can edit with sed,
// and a sidecar SQLite database holds only scheduling state.
#include "deck.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    }
    return out.join('\n') + "\n";
}

bool save(const Deck &deck, QString *error) {
    QString path = deck.path;
    if (path.isEmpty()) {
        const QDir dir(QFileInfo(deck.path).absolutePath());
        path = dir.filePath(deck.name + ".txt");
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QObject::tr("Cannot write deck file: %1").arg(path);
        return false;
    }
    file.write(serialize(deck).toUtf8());
    if (!file.commit()) {
        if (error)
            *error = QObject::tr("Cannot save deck file: %1").arg(path);
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
