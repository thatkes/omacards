#include <QtTest>
#include <QFileInfo>
#include <QTemporaryDir>

#include "test_filehistory.h"
#include "test_support.h"
#include "filehistory.h"

using namespace TestSupport;

void TestFileHistory::fileHistoryUndoRedoText() {
    QTemporaryDir dir;
    const QString path = dir.filePath("note.txt");
    QVERIFY(writeFile(path, "first"));
    FileHistory history;
    const QByteArray before = FileHistory::readBytes(path);
    QVERIFY(writeFile(path, "second"));
    QVERIFY(history.record(path, before, true, QStringLiteral("editing")));
    QVERIFY(history.canUndo());
    QVERIFY(!history.canRedo());

    QString error;
    QString description;
    QVERIFY(history.undo(&error, &description));
    QCOMPARE(description, QString("editing"));
    QCOMPARE(FileHistory::readBytes(path), QByteArray("first"));
    QVERIFY(history.canRedo());

    QVERIFY(history.redo(&error, &description));
    QCOMPARE(FileHistory::readBytes(path), QByteArray("second"));
    QVERIFY(!history.canRedo());
}

void TestFileHistory::fileHistoryUndoRemovesCreatedFile() {
    QTemporaryDir dir;
    const QString path = dir.filePath("created.txt");
    FileHistory history;
    QVERIFY(writeFile(path, "new"));
    QVERIFY(history.record(path, {}, false, QStringLiteral("creating")));
    QString error;
    QString description;
    QVERIFY(history.undo(&error, &description));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(history.redo(&error, &description));
    QCOMPARE(FileHistory::readBytes(path), QByteArray("new"));
}
