#include <QtTest>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVector>

#include "test_importer.h"
#include "test_support.h"
#include "deck.h"
#include "importer.h"

using namespace TestSupport;

namespace {

struct AnkiNote {
    QString fields;
    QString tags;
    int model = 1;
};

bool createAnkiDatabase(const QString &database, const QVector<AnkiNote> &notes) {
    const QString connection = QStringLiteral("omacards-importer-fixture");
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(database);
    if (!db.open()) {
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
        return false;
    }
    bool ok = true;
    {
        QSqlQuery query(db);
        ok = query.exec("CREATE TABLE models (id INTEGER, type INTEGER)") &&
             query.exec("INSERT INTO models VALUES (1, 0), (2, 1)") &&
             query.exec("CREATE TABLE notes (flds TEXT, mid INTEGER, tags TEXT)");
        for (const AnkiNote &note : notes) {
            if (!ok)
                break;
            ok = query.prepare("INSERT INTO notes (flds, mid, tags) VALUES (:f, :m, :t)");
            if (!ok)
                break;
            query.bindValue(":f", note.fields);
            query.bindValue(":m", note.model);
            query.bindValue(":t", note.tags);
            ok = query.exec();
        }
    }
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool zipPackage(const QString &package, const QString &workingDir, const QStringList &entries) {
    QProcess zip;
    zip.setWorkingDirectory(workingDir);
    zip.start(QStringLiteral("zip"), QStringList{QStringLiteral("-q"), package} + entries);
    return zip.waitForFinished(10000) && zip.exitCode() == 0;
}

} // namespace

void TestImporter::csvSelectsDelimiterByExtension() {
    QTemporaryDir dir;
    const QString commas = dir.filePath("commas.csv");
    const QString tabs = dir.filePath("tabs.tsv");
    const QString tabInCsv = dir.filePath("tab.csv");
    const QString commaInTsv = dir.filePath("comma.tsv");
    QVERIFY(writeFile(commas, "qC,aC\n"));
    QVERIFY(writeFile(tabs, "qT\taT\n"));
    QVERIFY(writeFile(tabInCsv, "qW\taW\n"));
    QVERIFY(writeFile(commaInTsv, "qW,aW\n"));

    QString error;
    DeckImport::Result result;
    QVERIFY(DeckImport::csv(commas, {}, &result, &error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().question, QString("qC"));
    QCOMPARE(result.cards.first().answer, QString("aC"));

    result = DeckImport::Result{};
    QVERIFY(DeckImport::csv(tabs, {}, &result, &error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().question, QString("qT"));
    QCOMPARE(result.cards.first().answer, QString("aT"));

    // A single cell is never a card, so the wrong delimiter yields no cards
    // but is still a successful parse.
    result = DeckImport::Result{};
    QVERIFY(DeckImport::csv(tabInCsv, {}, &result, &error));
    QVERIFY(result.cards.isEmpty());
    result = DeckImport::Result{};
    QVERIFY(DeckImport::csv(commaInTsv, {}, &result, &error));
    QVERIFY(result.cards.isEmpty());
}

void TestImporter::csvDetectsHeaderCaseInsensitively() {
    QTemporaryDir dir;
    const QString questionHeader = dir.filePath("question.csv");
    const QString questionAliasHeader = dir.filePath("question-alias.csv");
    const QString noHeader = dir.filePath("noheader.csv");
    QVERIFY(writeFile(questionHeader, "QUESTION,ANSWER\nq,a\n"));
    // "Front,Back" is an external Anki alias, so the literal stays as-is.
    QVERIFY(writeFile(questionAliasHeader, "Front,Back\nf,b\n"));
    QVERIFY(writeFile(noHeader, "Name,Value\nx,y\n"));

    QString error;
    DeckImport::Result result;
    QVERIFY(DeckImport::csv(questionHeader, {}, &result, &error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().question, QString("q"));

    result = DeckImport::Result{};
    QVERIFY(DeckImport::csv(questionAliasHeader, {}, &result, &error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().question, QString("f"));

    // Without a recognized header even the first row becomes a card.
    result = DeckImport::Result{};
    QVERIFY(DeckImport::csv(noHeader, {}, &result, &error));
    QCOMPARE(result.cards.size(), 2);
    QCOMPARE(result.cards.first().question, QString("Name"));
    QCOMPARE(result.cards.last().question, QString("x"));
}

void TestImporter::csvSkipsShortAndEmptyRows() {
    QTemporaryDir dir;
    const QString path = dir.filePath("rows.csv");
    QVERIFY(writeFile(path, "single\n,empty question\nquestion,\n   ,   \nok,fine\nx,y,z\n"));

    DeckImport::Result result;
    QString error;
    QVERIFY(DeckImport::csv(path, {}, &result, &error));
    QCOMPARE(result.cards.size(), 2);
    QCOMPARE(result.cards.at(0).question, QString("ok"));
    QCOMPARE(result.cards.at(0).answer, QString("fine"));
    QCOMPARE(result.cards.at(1).question, QString("x"));
    QCOMPARE(result.cards.at(1).answer, QString("y"));
}

void TestImporter::csvSplitsTagsFromThirdColumn() {
    QTemporaryDir dir;
    const QString path = dir.filePath("tags.csv");
    QVERIFY(writeFile(path, "q1,a1,\"one,two\"\nq2,a2,solo\nq3,a3\n"));

    DeckImport::Result result;
    QString error;
    QVERIFY(DeckImport::csv(path, {}, &result, &error));
    QCOMPARE(result.cards.size(), 3);
    QCOMPARE(result.cards.at(0).tags, QStringList({"one", "two"}));
    QCOMPARE(result.cards.at(1).tags, QStringList({"solo"}));
    QVERIFY(result.cards.at(2).tags.isEmpty());
}

void TestImporter::csvParsesQuotedFieldsWithEscapedQuotes() {
    QTemporaryDir dir;
    const QString path = dir.filePath("quotes.csv");
    QVERIFY(writeFile(path, "\"Say \"\"hello\"\"\",Answer,plain\n"
                            "Q,\"a \"\"quoted\"\" answer\"\n"
                            "\"q, with comma\",last\n"));

    DeckImport::Result result;
    QString error;
    QVERIFY(DeckImport::csv(path, {}, &result, &error));
    QCOMPARE(result.cards.size(), 3);
    QCOMPARE(result.cards.at(0).question, QString("Say \"hello\""));
    QCOMPARE(result.cards.at(0).answer, QString("Answer"));
    QCOMPARE(result.cards.at(0).tags, QStringList({"plain"}));
    QCOMPARE(result.cards.at(1).question, QString("Q"));
    QCOMPARE(result.cards.at(1).answer, QString("a \"quoted\" answer"));
    QCOMPARE(result.cards.at(2).question, QString("q, with comma"));
    QCOMPARE(result.cards.at(2).answer, QString("last"));
}

void TestImporter::csvHandlesCrLf() {
    QTemporaryDir dir;
    const QString path = dir.filePath("crlf.csv");
    QVERIFY(writeFile(path, "Question,Answer\r\nq1,a1\r\nq2,a2\r\n"));

    DeckImport::Result result;
    QString error;
    QVERIFY(DeckImport::csv(path, {}, &result, &error));
    QCOMPARE(result.cards.size(), 2);
    QCOMPARE(result.cards.at(0).question, QString("q1"));
    QCOMPARE(result.cards.at(0).answer, QString("a1"));
    QCOMPARE(result.cards.at(1).question, QString("q2"));
    QCOMPARE(result.cards.at(1).answer, QString("a2"));
}

void TestImporter::csvNamesDeckFromFileNameUnlessOverridden() {
    QTemporaryDir dir;
    const QString path = dir.filePath("My Deck.csv");
    QVERIFY(writeFile(path, "q,a\n"));

    QString error;
    DeckImport::Result fallback;
    QVERIFY(DeckImport::csv(path, {}, &fallback, &error));
    QCOMPARE(fallback.deckName, QString("My Deck"));

    DeckImport::Result explicitName;
    QVERIFY(DeckImport::csv(path, "Chosen", &explicitName, &error));
    QCOMPARE(explicitName.deckName, QString("Chosen"));
}

void TestImporter::csvReportsEmptyAndUnreadableFiles() {
    QTemporaryDir dir;
    const QString empty = dir.filePath("empty.csv");
    const QString blank = dir.filePath("blank.csv");
    QVERIFY(writeFile(empty, ""));
    QVERIFY(writeFile(blank, "\n\n"));

    DeckImport::Result result;
    QString error;
    QVERIFY(!DeckImport::csv(empty, {}, &result, &error));
    QVERIFY(error.contains(QStringLiteral("No rows")));

    error.clear();
    QVERIFY(!DeckImport::csv(blank, {}, &result, &error));
    QVERIFY(error.contains(QStringLiteral("No rows")));

    error.clear();
    QVERIFY(!DeckImport::csv(dir.filePath("does-not-exist.csv"), {}, &result, &error));
    QVERIFY(error.contains(QStringLiteral("Cannot read")));
}

void TestImporter::nullResultReturnsFalse() {
    QTemporaryDir dir;
    const QString csvPath = dir.filePath("cards.csv");
    QVERIFY(writeFile(csvPath, "q,a\n"));
    QVERIFY(!DeckImport::csv(csvPath, {}, nullptr, nullptr));

    const QString package = dir.filePath("sample.apkg");
    QVERIFY(writeFile(package, "junk"));
    // apkg() rejects a null result before looking for unzip.
    QVERIFY(!DeckImport::apkg(package, {}, dir.path(), nullptr, nullptr));
}

void TestImporter::fileDispatchesExtensionsCaseInsensitively() {
    QTemporaryDir dir;
    const QString csvUp = dir.filePath("data.CSV");
    const QString txtUp = dir.filePath("notes.TXT");
    const QString tsvUp = dir.filePath("table.TSV");
    const QString apkgUp = dir.filePath("pack.ApKg");
    QVERIFY(writeFile(csvUp, "qC,aC\n"));
    QVERIFY(writeFile(txtUp, "qT,aT\n"));
    QVERIFY(writeFile(tsvUp, "qS\taS\n"));
    QVERIFY(writeFile(apkgUp, "not a zip"));

    QString error;
    DeckImport::Result result;
    QVERIFY(DeckImport::file(csvUp, {}, dir.path(), &result, &error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().answer, QString("aC"));

    result = DeckImport::Result{};
    QVERIFY(DeckImport::file(txtUp, {}, dir.path(), &result, &error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().answer, QString("aT"));

    result = DeckImport::Result{};
    QVERIFY(DeckImport::file(tsvUp, {}, dir.path(), &result, &error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().answer, QString("aS"));

    // A broken .apkg must reach apkg(), not the unsupported-format branch.
    result = DeckImport::Result{};
    error.clear();
    QVERIFY(!DeckImport::file(apkgUp, {}, dir.path(), &result, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(QStringLiteral("Unsupported")));
}

void TestImporter::fileRejectsUnsupportedExtension() {
    QTemporaryDir dir;
    DeckImport::Result result;
    QString error;
    QVERIFY(!DeckImport::file(dir.filePath("notes.md"), {}, dir.path(), &result, &error));
    QVERIFY(error.contains(QStringLiteral("Unsupported import format")));
}

void TestImporter::apkgRejectsInvalidArchive() {
    if (!hasTool(QStringLiteral("unzip")))
        QSKIP("unzip is required for .apkg tests");

    QTemporaryDir dir;
    const QString package = dir.filePath("broken.apkg");
    QVERIFY(writeFile(package, "not a zip file"));

    DeckImport::Result result;
    QString error;
    QVERIFY(!DeckImport::apkg(package, "Broken", dir.path(), &result, &error));
    QVERIFY(error.contains(QStringLiteral("Not a readable Anki package")));

    error.clear();
    QVERIFY(!DeckImport::file(package, "Broken", dir.path(), &result, &error));
    QVERIFY(error.contains(QStringLiteral("Not a readable Anki package")));
}

void TestImporter::apkgReportsMissingUnzip() {
    // QStandardPaths reads PATH on demand, so an empty PATH characterizes the
    // "unzip is missing" branch. The shared guard restores the original PATH.
    PathGuard guard(QString(), PathGuard::Mode::Replace);
    if (hasTool(QStringLiteral("unzip")))
        QSKIP("Cannot hide unzip from QStandardPaths by clearing PATH");

    QTemporaryDir dir;
    const QString package = dir.filePath("sample.apkg");
    QVERIFY(writeFile(package, "junk"));

    DeckImport::Result result;
    QString error;
    QVERIFY(!DeckImport::apkg(package, {}, dir.path(), &result, &error));
    QVERIFY(error.contains(QStringLiteral("unzip")));
}

void TestImporter::apkgImportsHtmlClozeAndMedia() {
    if (!hasTool(QStringLiteral("unzip")) || !hasTool(QStringLiteral("zip")))
        QSKIP("zip and unzip are required for .apkg tests");

    QTemporaryDir dir;
    const QString package = dir.filePath("sample.apkg");
    const QString sep(1, QChar(0x1f));
    const QVector<AnkiNote> notes{
        {QStringLiteral("Basic &amp; <b>bold</b>") + sep + QStringLiteral("back with <img src=\"0\">"),
         QStringLiteral(" tag1 tag2 "), 1},
        {QStringLiteral("The {{c1::cat::animal}} sleeps"), QStringLiteral("cloze"), 2},
        {QStringLiteral("Quote &#39;x&#39; &quot;y&quot; and &#x41;") + sep + QStringLiteral("tail"),
         QStringLiteral(" misc "), 1},
    };
    QVERIFY(createAnkiDatabase(dir.filePath("collection.anki2"), notes));
    QVERIFY(writeFile(dir.filePath("media"), R"({"0":"image.png"})"));
    QVERIFY(writeFile(dir.filePath("0"), "png-bytes"));
    QVERIFY(zipPackage(package, dir.path(), {"collection.anki2", "media", "0"}));

    const QString decks = dir.filePath("decks");
    DeckImport::Result result;
    QString error;
    QVERIFY2(DeckImport::apkg(package, "Anki", decks, &result, &error), qPrintable(error));
    QCOMPARE(result.deckName, QString("Anki"));
    QCOMPARE(result.cards.size(), 3);

    const CardDraft *basic = nullptr;
    const CardDraft *cloze = nullptr;
    const CardDraft *entities = nullptr;
    for (const CardDraft &draft : result.cards) {
        if (draft.question.startsWith(QStringLiteral("Basic")))
            basic = &draft;
        else if (draft.question.startsWith(QStringLiteral("The {{c1")))
            cloze = &draft;
        else if (draft.question.startsWith(QStringLiteral("Quote")))
            entities = &draft;
    }
    QVERIFY(basic && cloze && entities);

    QCOMPARE(basic->question, QString("Basic & bold"));
    QCOMPARE(basic->type, QString("basic"));
    QVERIFY(basic->answer.contains(QStringLiteral("<img src=\"file://")));
    QVERIFY(basic->answer.endsWith(QStringLiteral("image.png\">")));
    QCOMPARE(basic->tags, QStringList({"tag1", "tag2"}));

    QCOMPARE(cloze->question, QString("The {{c1::cat}} sleeps"));
    QCOMPARE(cloze->type, QString("cloze"));
    QVERIFY(cloze->answer.isEmpty());
    QCOMPARE(cloze->tags, QStringList({"cloze"}));

    QCOMPARE(entities->question, QString("Quote 'x' \"y\" and A"));
    QVERIFY(QFile::exists(decks + "/media/Anki/image.png"));

    // Without an explicit name the archive's base name is used.
    DeckImport::Result fallback;
    error.clear();
    QVERIFY2(DeckImport::apkg(package, {}, decks, &fallback, &error), qPrintable(error));
    QCOMPARE(fallback.deckName, QString("sample"));
    QCOMPARE(fallback.cards.size(), 3);
}

void TestImporter::apkgImportsWithoutMedia() {
    if (!hasTool(QStringLiteral("unzip")) || !hasTool(QStringLiteral("zip")))
        QSKIP("zip and unzip are required for .apkg tests");

    QTemporaryDir dir;
    const QString package = dir.filePath("plain.apkg");
    const QString sep(1, QChar(0x1f));
    QVERIFY(createAnkiDatabase(
        dir.filePath("collection.anki2"),
        {{QStringLiteral("Question") + sep + QStringLiteral("back with <img src=\"0\">"), QStringLiteral("tag"), 1}}));
    QVERIFY(zipPackage(package, dir.path(), {"collection.anki2"}));

    DeckImport::Result result;
    QString error;
    QVERIFY2(DeckImport::apkg(package, "Plain", dir.filePath("decks"), &result, &error), qPrintable(error));
    QCOMPARE(result.cards.size(), 1);
    QCOMPARE(result.cards.first().question, QString("Question"));
    QCOMPARE(result.cards.first().answer, QString("back with <img src=\"0\">"));
    QVERIFY(!QFile::exists(dir.filePath("decks/media")));
}
