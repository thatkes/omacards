#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Undo/redo for whole-file edits. Deck files are the source of truth, so every
// edit records the bytes before and after; restoring either is atomic.
class FileHistory {
public:
    struct Snapshot {
        QString path;
        QByteArray before;
        QByteArray after;
        bool existedBefore = false;
        bool existsAfter = true;
        QString description;
        QString movedTo; // rename only: path -> movedTo, with the bytes at movedTo in after
    };

    static QByteArray readBytes(const QString &path);
    static bool restore(const QString &path, const QByteArray &bytes, bool existed, QString *error = nullptr);

    // Returns false when the file did not actually change.
    bool record(const QString &path, const QByteArray &before, bool existedBefore, const QString &description);
    // A rename is one undo step across both paths; `after` is the bytes at `to`.
    bool recordRename(const QString &from, const QByteArray &before, const QString &to, const QByteArray &after,
                      const QString &description);
    bool undo(QString *error, QString *description);
    bool redo(QString *error, QString *description);

    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

private:
    static bool apply(const Snapshot &snapshot, bool restoreBefore, QString *error);
    void pushSnapshot(const Snapshot &snapshot);

    QVector<Snapshot> m_undoStack;
    QVector<Snapshot> m_redoStack;
};
