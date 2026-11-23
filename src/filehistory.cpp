#include "filehistory.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

QByteArray FileHistory::readBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void FileHistory::pushSnapshot(const Snapshot &snapshot) {
    m_undoStack.append(snapshot);
    if (m_undoStack.size() > 50)
        m_undoStack.removeFirst();
    m_redoStack.clear();
}

bool FileHistory::record(const QString &path, const QByteArray &before, bool existedBefore,
                         const QString &description) {
    const bool existsAfter = QFileInfo::exists(path);
    const QByteArray after = existsAfter ? readBytes(path) : QByteArray();
    if (existedBefore == existsAfter && before == after)
        return false;
    pushSnapshot(Snapshot{path, before, after, existedBefore, existsAfter, description, {}});
    return true;
}

bool FileHistory::recordRename(const QString &from, const QByteArray &before, const QString &to,
                               const QByteArray &after, const QString &description) {
    if (from == to && before == after)
        return false;
    pushSnapshot(Snapshot{from, before, after, true, true, description, to});
    return true;
}

bool FileHistory::restore(const QString &path, const QByteArray &bytes, bool existed, QString *error) {
    if (!existed) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            if (error)
                *error = QCoreApplication::translate("FileHistory", "Cannot remove %1").arg(path);
            return false;
        }
        return true;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QCoreApplication::translate("FileHistory", "Cannot write %1").arg(path);
        return false;
    }
    file.write(bytes);
    if (!file.commit()) {
        if (error)
            *error = QCoreApplication::translate("FileHistory", "Cannot write %1").arg(path);
        return false;
    }
    return true;
}

bool FileHistory::apply(const Snapshot &snapshot, bool restoreBefore, QString *error) {
    if (snapshot.movedTo.isEmpty()) {
        const bool exists = restoreBefore ? snapshot.existedBefore : snapshot.existsAfter;
        const QByteArray &bytes = restoreBefore ? snapshot.before : snapshot.after;
        return restore(snapshot.path, bytes, exists, error);
    }

    // Undo puts the original file back and clears the renamed copy; redo does
    // the reverse. A failed removal leaves the snapshot on the stack.
    const QString &removePath = restoreBefore ? snapshot.movedTo : snapshot.path;
    if (QFileInfo::exists(removePath) && !QFile::remove(removePath)) {
        if (error)
            *error = QCoreApplication::translate("FileHistory", "Cannot remove %1").arg(removePath);
        return false;
    }
    const QString &writePath = restoreBefore ? snapshot.path : snapshot.movedTo;
    const QByteArray &bytes = restoreBefore ? snapshot.before : snapshot.after;
    return restore(writePath, bytes, true, error);
}

bool FileHistory::undo(QString *error, QString *description) {
    if (m_undoStack.isEmpty())
        return false;
    const Snapshot snapshot = m_undoStack.takeLast();
    if (!apply(snapshot, true, error)) {
        m_undoStack.append(snapshot);
        return false;
    }
    m_redoStack.append(snapshot);
    if (description)
        *description = snapshot.description;
    return true;
}

bool FileHistory::redo(QString *error, QString *description) {
    if (m_redoStack.isEmpty())
        return false;
    const Snapshot snapshot = m_redoStack.takeLast();
    if (!apply(snapshot, false, error)) {
        m_redoStack.append(snapshot);
        return false;
    }
    m_undoStack.append(snapshot);
    if (description)
        *description = snapshot.description;
    return true;
}
