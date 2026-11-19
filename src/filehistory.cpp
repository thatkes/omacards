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

bool FileHistory::record(const QString &path, const QByteArray &before, bool existedBefore,
                         const QString &description) {
    const bool existsAfter = QFileInfo::exists(path);
    const QByteArray after = existsAfter ? readBytes(path) : QByteArray();
    if (existedBefore == existsAfter && before == after)
        return false;
    m_undoStack.append(Snapshot{path, before, after, existedBefore, existsAfter, description});
    if (m_undoStack.size() > 50)
        m_undoStack.removeFirst();
    m_redoStack.clear();
    return true;
}

bool FileHistory::apply(const Snapshot &snapshot, bool restoreBefore, QString *error) {
    const bool exists = restoreBefore ? snapshot.existedBefore : snapshot.existsAfter;
    if (!exists) {
        if (QFileInfo::exists(snapshot.path) && !QFile::remove(snapshot.path)) {
            if (error)
                *error = QCoreApplication::translate("FileHistory", "Cannot remove %1").arg(snapshot.path);
            return false;
        }
        return true;
    }

    QSaveFile file(snapshot.path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QCoreApplication::translate("FileHistory", "Cannot write %1").arg(snapshot.path);
        return false;
    }
    file.write(restoreBefore ? snapshot.before : snapshot.after);
    if (!file.commit()) {
        if (error)
            *error = QCoreApplication::translate("FileHistory", "Cannot write %1").arg(snapshot.path);
        return false;
    }
    return true;
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
