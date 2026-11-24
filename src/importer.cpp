#include "importer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

namespace {

QString tr(const QString &text) {
    return QCoreApplication::translate("DeckImport", text.toUtf8().constData());
}

template <typename Replacement>
QString replaceRegex(const QString &text, const QRegularExpression &expression, Replacement replacement) {
    QString out;
    qsizetype last = 0;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        out += text.mid(last, match.capturedStart() - last) + replacement(match);
        last = match.capturedEnd();
    }
    return out + text.mid(last);
}

QStringList splitDelimited(const QString &line, QChar delimiter) {
    QStringList cells;
    QString cell;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line.at(i + 1) == '"') {
                cell += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == delimiter && !quoted) {
            cells << cell;
            cell.clear();
        } else {
            cell += ch;
        }
    }
    cells << cell;
    return cells;
}

QString decodeEntities(const QString &text) {
    QString out = text;
    out.replace("&nbsp;", QChar(0x00A0));
    out.replace("&amp;", "&");
    out.replace("&lt;", "<");
    out.replace("&gt;", ">");
    out.replace("&quot;", "\"");
    out.replace("&apos;", "'");
    out.replace("&#39;", "'");
    static const QRegularExpression numeric(R"re(&#(?:x([0-9a-fA-F]+)|([0-9]+));)re");
    out = replaceRegex(out, numeric, [](const QRegularExpressionMatch &match) {
        bool ok = false;
        const int code =
            match.captured(1).isEmpty() ? match.captured(2).toInt(&ok, 10) : match.captured(1).toInt(&ok, 16);
        return ok && code > 0 && code <= 0x10ffff ? QString(QChar(code)) : match.captured();
    });
    // Anki cloze hints are written as {{c1::answer::hint}}; our deck format
    // keeps just the answer so the plain-text source stays easy to edit.
    static const QRegularExpression clozeHint(R"re(\{\{(c\d+::[^:]+)::[^}]*\}\})re");
    out = replaceRegex(out, clozeHint, [](const QRegularExpressionMatch &match) {
        return QStringLiteral("{{%1}}").arg(match.captured(1));
    });
    return out;
}

QString stripHtml(const QString &html) {
    QString text = html;
    text.replace(QRegularExpression(R"re(<br\s*/?>)re"), "\n");
    // Keep image tags so imported media can still be shown in the RichText
    // study view; replace them with markers while the other tags are removed.
    QVector<QString> images;
    static const QRegularExpression imageTag(R"re(<img\b[^>]*>)re", QRegularExpression::CaseInsensitiveOption);
    QString marked;
    int last = 0;
    auto matches = imageTag.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        marked += text.mid(last, match.capturedStart() - last);
        images << match.captured();
        marked += QString("\x01%1\x02").arg(images.size() - 1);
        last = match.capturedEnd();
    }
    marked += text.mid(last);
    marked.replace(QRegularExpression(R"re(<[^>]+>)re"), "");
    text = decodeEntities(marked);
    for (int i = 0; i < images.size(); ++i)
        text.replace(QString("\x01%1\x02").arg(i), images.at(i));
    return text.trimmed();
}

using MediaPaths = QHash<QString, QString>;

// Fold the keys once per import so each field can match them with a hash
// lookup instead of a scan per media entry.
MediaPaths foldMediaKeys(const MediaPaths &media) {
    MediaPaths folded;
    for (auto it = media.constBegin(); it != media.constEnd(); ++it)
        folded.insert(it.key().toCaseFolded(), it.value());
    return folded;
}

QString rewriteAnkiMedia(const QString &html, const MediaPaths &folded) {
    static const QRegularExpression expression(R"re(src=(["'])([^"']+)\1)re",
                                               QRegularExpression::CaseInsensitiveOption);
    return replaceRegex(html, expression, [&](const QRegularExpressionMatch &match) {
        const auto found = folded.constFind(match.captured(2).toCaseFolded());
        if (found == folded.constEnd())
            return match.captured();
        return QStringLiteral("src=%1%2%1").arg(match.captured(1), found.value());
    });
}

MediaPaths apkgMedia(const QString &extracted, const QString &directory, QString *error) {
    MediaPaths media;
    QFile mapping(QDir(extracted).filePath(QStringLiteral("media")));
    if (!mapping.open(QIODevice::ReadOnly))
        return media; // packages without media are common
    const QJsonDocument document = QJsonDocument::fromJson(mapping.readAll());
    if (!document.isObject())
        return media;

    const QDir target(directory);
    if (!target.mkpath(QStringLiteral(".")))
        return media;
    const QJsonObject entries = document.object();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const QString name = QFileInfo(it.value().toString()).fileName();
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;
        const QString source = QDir(extracted).filePath(QFileInfo(it.key()).fileName());
        const QString destination = target.filePath(name);
        QFile::remove(destination);
        if (!QFile::exists(source) || !QFile::copy(source, destination)) {
            QFile::remove(destination);
            if (error)
                *error = tr("Could not extract media %1").arg(name);
            continue;
        }
        media.insert(it.key(), QUrl::fromLocalFile(destination).toString());
    }
    return media;
}

} // namespace

namespace DeckImport {

bool csv(const QString &path, const QString &deckName, Result *result, QString *error) {
    if (!result)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = tr("Cannot read: %1").arg(path);
        return false;
    }
    const QChar delimiter = path.endsWith(".tsv", Qt::CaseInsensitive) ? '\t' : ',';
    QStringList rows = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    if (rows.isEmpty()) {
        if (error)
            *error = tr("No rows in %1").arg(path);
        return false;
    }
    const auto first = splitDelimited(rows.first(), delimiter);
    // "front" is an external Anki CSV header alias; keep accepting it.
    const bool hasHeader = !first.isEmpty() && (first.first().contains("question", Qt::CaseInsensitive) ||
                                                first.first().contains("front", Qt::CaseInsensitive));
    if (hasHeader)
        rows.removeFirst();

    result->deckName = deckName.isEmpty() ? QFileInfo(path).completeBaseName() : deckName;
    for (const QString &row : rows) {
        const QStringList cells = splitDelimited(row, delimiter);
        if (cells.size() < 2)
            continue;
        const QString question = cells.at(0).trimmed();
        const QString answer = cells.at(1).trimmed();
        if (question.isEmpty() || answer.isEmpty())
            continue;
        CardDraft draft;
        draft.question = question;
        draft.answer = answer;
        draft.tags = cells.size() > 2 ? splitDelimited(cells.at(2), ',') : QStringList();
        result->cards << draft;
    }
    return true;
}

bool apkg(const QString &path, const QString &deckName, const QString &decksDir, Result *result, QString *error) {
    if (!result)
        return false;
    QTemporaryDir temp;
    if (!temp.isValid()) {
        if (error)
            *error = tr("Cannot create a temporary directory");
        return false;
    }
    const auto unzip = QStandardPaths::findExecutable("unzip");
    if (unzip.isEmpty()) {
        if (error)
            *error = tr("Importing .apkg needs the unzip command");
        return false;
    }

    // The collection and the media members share one unzip run. Anki stores
    // them flat, so -j gives the temporary directory a predictable layout.
    QProcess archive;
    archive.start(unzip, {QStringLiteral("-o"), QStringLiteral("-j"), QStringLiteral("-d"), temp.path(), path});
    if (!archive.waitForFinished(30000) || archive.exitCode() != 0) {
        if (error)
            *error = tr("Not a readable Anki package: %1").arg(path);
        return false;
    }
    const QDir extracted(temp.path());
    QString database = extracted.filePath(QStringLiteral("collection.anki2"));
    if (!QFile::exists(database))
        database = extracted.filePath(QStringLiteral("collection.anki21"));
    if (!QFile::exists(database)) {
        // Anki 21b is a gzip-compressed SQLite file inside the archive.
        const QString compressed = extracted.filePath(QStringLiteral("collection.anki21b"));
        const QString decompressed = extracted.filePath(QStringLiteral("collection.db"));
        QFile input(compressed);
        QProcess gzip;
        gzip.setStandardOutputFile(decompressed);
        gzip.start(QStandardPaths::findExecutable("gzip"), {"-dc"});
        if (input.open(QIODevice::ReadOnly))
            gzip.write(input.readAll());
        gzip.closeWriteChannel();
        database = gzip.waitForFinished(10000) && gzip.exitCode() == 0 && QFile::exists(decompressed) ? decompressed
                                                                                                      : QString();
    }
    if (database.isEmpty()) {
        if (error)
            *error = tr("Not a readable Anki package: %1").arg(path);
        return false;
    }

    const QString connection = "omacards-apkg-" + QString::number(QRandomGenerator::global()->generate());
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(database);
    QSqlQuery query;
    // Every exit path must release the named connection; leaks accumulate
    // across imports and keep the temporary database file alive.
    auto closeCollection = [&] {
        query.finish();
        query.clear();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    };
    if (!db.open()) {
        if (error)
            *error = tr("Cannot read the Anki collection");
        closeCollection();
        return false;
    }
    // Cloze notes use model type 1; their text lives in a single field with
    // {{c1::...}} markers. Older collections may lack the type column.
    query = QSqlQuery(db);
    bool clozeColumn = query.exec("SELECT type FROM models LIMIT 1");
    if (clozeColumn)
        clozeColumn = query.exec("SELECT n.flds, n.tags, COALESCE(m.type, 0) "
                                 "FROM notes n LEFT JOIN models m ON n.mid = m.id");
    if (!clozeColumn && !query.exec("SELECT flds, tags FROM notes")) {
        if (error)
            *error = tr("The Anki collection has no notes table");
        closeCollection();
        return false;
    }

    result->deckName = deckName.isEmpty() ? QFileInfo(path).completeBaseName() : deckName;
    const MediaPaths media =
        apkgMedia(temp.path(), decksDir + "/media/" + DeckIO::safeFileName(result->deckName), error);
    const MediaPaths folded = foldMediaKeys(media);
    while (query.next()) {
        const QStringList fields = query.value(0).toString().split(QChar(0x1f));
        if (fields.isEmpty())
            continue;
        const bool cloze = clozeColumn && query.value(2).toInt() == 1;
        QString question = stripHtml(fields.at(0));
        QString answer = !cloze && fields.size() > 1 ? stripHtml(fields.at(1)) : QString();
        if (cloze) {
            static const QRegularExpression clozeNumber(R"re(\{\{c\d+::(.*?)::[^}]*\}\})re");
            question = replaceRegex(question, clozeNumber, [](const QRegularExpressionMatch &match) {
                return QStringLiteral("{{c1::%1}}").arg(match.captured(1));
            });
        }
        question = rewriteAnkiMedia(question, folded);
        if (!cloze) {
            static const QRegularExpression imageSrc(R"re(<img\b[^>]*\bsrc="([^"]+)"[^>]*>)re");
            answer = replaceRegex(answer, imageSrc, [&](const QRegularExpressionMatch &match) {
                return QStringLiteral("<img src=\"%1\">").arg(media.value(match.captured(1), match.captured(1)));
            });
        }
        if (question.isEmpty())
            continue;
        CardDraft draft;
        draft.question = question;
        draft.answer = answer;
        draft.type = cloze ? QStringLiteral("cloze") : QStringLiteral("basic");
        draft.tags = query.value(1).toString().simplified().split(' ', Qt::SkipEmptyParts);
        result->cards << draft;
    }
    closeCollection();
    return true;
}

bool file(const QString &path, const QString &deckName, const QString &decksDir, Result *result, QString *error) {
    const QString lower = path.toLower();
    if (lower.endsWith(".apkg"))
        return apkg(path, deckName, decksDir, result, error);
    if (lower.endsWith(".csv") || lower.endsWith(".tsv") || lower.endsWith(".txt"))
        return csv(path, deckName, result, error);
    if (error)
        *error = QCoreApplication::translate("DeckImport", "Unsupported import format: %1").arg(path);
    return false;
}

} // namespace DeckImport
