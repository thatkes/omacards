#include <QtTest>
#include <QDir>
#include <QFile>
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

void TestFileHistory::fileHistoryRestorePreWriteState() {
    QTemporaryDir dir;
    const QString existing = dir.filePath("existing.txt");
    QVERIFY(writeFile(existing, "before"));
    const QByteArray before = FileHistory::readBytes(existing);
    QVERIFY(writeFile(existing, "after"));

    QString error;
    QVERIFY(FileHistory::restore(existing, before, true, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(FileHistory::readBytes(existing), QByteArray("before"));

    const QString created = dir.filePath("created.txt");
    QVERIFY(writeFile(created, "new"));
    QVERIFY(FileHistory::restore(created, {}, false, &error));
    QVERIFY(!QFileInfo::exists(created));
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

void TestFileHistory::fileHistoryUnchangedRecordIsNoop() {
    QTemporaryDir dir;
    const QString path = dir.filePath("note.txt");
    QVERIFY(writeFile(path, "same"));
    FileHistory history;

    const QByteArray before = FileHistory::readBytes(path);
    QVERIFY(!history.record(path, before, true, QStringLiteral("no change")));
    QVERIFY(!history.canUndo());
    QVERIFY(!history.canRedo());

    // Neither side existing with empty bytes is also a no-op.
    QVERIFY(!history.record(dir.filePath("absent.txt"), {}, false, QStringLiteral("nothing")));
    QVERIFY(!history.canUndo());
    QVERIFY(!history.canRedo());
}

void TestFileHistory::fileHistoryUndoRedoDeletedFile() {
    QTemporaryDir dir;
    const QString path = dir.filePath("deleted.txt");
    QVERIFY(writeFile(path, "original"));
    const QByteArray before = FileHistory::readBytes(path);

    QVERIFY(QFile::remove(path));
    QVERIFY(!QFileInfo::exists(path));

    FileHistory history;
    QVERIFY(history.record(path, before, true, QStringLiteral("deleting")));

    QString error;
    QString description;
    QVERIFY(history.undo(&error, &description));
    QCOMPARE(description, QStringLiteral("deleting"));
    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(FileHistory::readBytes(path), QByteArray("original"));

    QVERIFY(history.redo(&error, &description));
    QCOMPARE(description, QStringLiteral("deleting"));
    QVERIFY(!QFileInfo::exists(path));
}

void TestFileHistory::fileHistoryRecordClearsRedo() {
    QTemporaryDir dir;
    const QString path = dir.filePath("note.txt");
    QVERIFY(writeFile(path, "a"));
    FileHistory history;

    QByteArray before = FileHistory::readBytes(path);
    QVERIFY(writeFile(path, "b"));
    QVERIFY(history.record(path, before, true, QStringLiteral("first")));

    QString error;
    QString description;
    QVERIFY(history.undo(&error, &description));
    QVERIFY(history.canRedo());

    before = FileHistory::readBytes(path);
    QVERIFY(writeFile(path, "c"));
    QVERIFY(history.record(path, before, true, QStringLiteral("second")));
    QVERIFY(!history.canRedo());

    QVERIFY(history.undo(&error, &description));
    QCOMPARE(description, QStringLiteral("second"));
    // Recording "second" cleared the redo stack, so the earlier "first"
    // snapshot (which had been undone) is gone for good.
    QVERIFY(!history.undo(&error, &description));
    QVERIFY(!history.canUndo());
    QVERIFY(history.canRedo());
    QCOMPARE(FileHistory::readBytes(path), QByteArray("a"));
}

void TestFileHistory::fileHistoryEmptyStacksAreNoops() {
    QTemporaryDir dir;
    const QString path = dir.filePath("note.txt");
    QVERIFY(writeFile(path, "untouched"));

    FileHistory history;
    QString error = QStringLiteral("unchanged-error");
    QString description = QStringLiteral("unchanged-description");

    QVERIFY(!history.undo(&error, &description));
    QCOMPARE(error, QStringLiteral("unchanged-error"));
    QCOMPARE(description, QStringLiteral("unchanged-description"));
    QVERIFY(!history.redo(&error, &description));
    QCOMPARE(error, QStringLiteral("unchanged-error"));
    QCOMPARE(description, QStringLiteral("unchanged-description"));
    QVERIFY(!history.undo(nullptr, nullptr));
    QVERIFY(!history.redo(nullptr, nullptr));

    QVERIFY(!history.canUndo());
    QVERIFY(!history.canRedo());
    QCOMPARE(FileHistory::readBytes(path), QByteArray("untouched"));
}

void TestFileHistory::fileHistoryKeepsFiftySnapshots() {
    QTemporaryDir dir;
    const QString path = dir.filePath("note.txt");
    QVERIFY(writeFile(path, QStringLiteral("state-0").toUtf8()));
    FileHistory history;

    for (int i = 1; i <= 51; ++i) {
        const QByteArray before = FileHistory::readBytes(path);
        QVERIFY(writeFile(path, QStringLiteral("state-%1").arg(i).toUtf8()));
        QVERIFY(history.record(path, before, true, QStringLiteral("change %1").arg(i)));
    }
    QCOMPARE(FileHistory::readBytes(path), QByteArray("state-51"));

    QString error;
    QString description;
    for (int i = 0; i < 50; ++i)
        QVERIFY(history.undo(&error, &description));

    QVERIFY(!history.canUndo());
    QVERIFY(history.canRedo());
    // The oldest snapshot (before the first change) was dropped, so the
    // earliest restorable state is the result of the first change.
    QCOMPARE(FileHistory::readBytes(path), QByteArray("state-1"));
}

void TestFileHistory::fileHistoryUndoFailureKeepsSnapshot() {
    QTemporaryDir dir;
    const QString path = dir.filePath("note.txt");
    QVERIFY(writeFile(path, "original"));
    const QByteArray before = FileHistory::readBytes(path);
    QVERIFY(writeFile(path, "edited"));

    FileHistory history;
    QVERIFY(history.record(path, before, true, QStringLiteral("edit")));

    // A directory at the target path makes QSaveFile unable to replace it.
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().mkpath(path));

    QString error;
    QString description = QStringLiteral("untouched");
    QVERIFY(!history.undo(&error, &description));
    QVERIFY(!error.isEmpty());
    QCOMPARE(description, QStringLiteral("untouched"));
    QVERIFY(history.canUndo());
    QVERIFY(!history.canRedo());

    QVERIFY(QDir().rmdir(path));
    error.clear();
    QVERIFY(history.undo(&error, &description));
    QCOMPARE(description, QStringLiteral("edit"));
    QCOMPARE(FileHistory::readBytes(path), QByteArray("original"));
}

void TestFileHistory::fileHistoryDescriptionOutParams() {
    QTemporaryDir dir;
    const QString path = dir.filePath("note.txt");
    QVERIFY(writeFile(path, "a"));
    FileHistory history;

    QByteArray before = FileHistory::readBytes(path);
    QVERIFY(writeFile(path, "b"));
    QVERIFY(history.record(path, before, true, QStringLiteral("first")));

    before = FileHistory::readBytes(path);
    QVERIFY(writeFile(path, "c"));
    QVERIFY(history.record(path, before, true, QStringLiteral("second")));

    QString error = QStringLiteral("sentinel");
    QString description;
    QVERIFY(history.undo(&error, &description));
    QCOMPARE(description, QStringLiteral("second"));
    QVERIFY(history.undo(&error, &description));
    QCOMPARE(description, QStringLiteral("first"));

    QVERIFY(history.redo(&error, &description));
    QCOMPARE(description, QStringLiteral("first"));
    QVERIFY(history.redo(&error, &description));
    QCOMPARE(description, QStringLiteral("second"));
    QCOMPARE(FileHistory::readBytes(path), QByteArray("c"));
}
