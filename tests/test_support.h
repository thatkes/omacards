#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace TestSupport {

struct EnvGuard {
    QByteArray decks;
    QByteArray state;

    EnvGuard(const QString &decksDir, const QString &stateDb) {
        decks = qgetenv("OMACARDS_DECKS_DIR");
        state = qgetenv("OMACARDS_STATE_DB");
        qputenv("OMACARDS_DECKS_DIR", decksDir.toUtf8());
        qputenv("OMACARDS_STATE_DB", stateDb.toUtf8());
    }

    ~EnvGuard() {
        if (!decks.isNull())
            qputenv("OMACARDS_DECKS_DIR", decks);
        else
            qunsetenv("OMACARDS_DECKS_DIR");
        if (!state.isNull())
            qputenv("OMACARDS_STATE_DB", state);
        else
            qunsetenv("OMACARDS_STATE_DB");
    }
};

inline QString appBinary() {
    return QDir(QString(OMACARDS_SOURCE_DIR)).filePath("../build/omacards");
}

inline QJsonObject runCli(const QStringList &arguments, const QString &decksDir, const QString &stateDb,
                          int *exitCode = nullptr) {
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("OMACARDS_DECKS_DIR"), decksDir);
    environment.insert(QStringLiteral("OMACARDS_STATE_DB"), stateDb);
    process.setProcessEnvironment(environment);
    process.start(appBinary(), arguments);
    if (!process.waitForFinished(10000)) {
        if (exitCode)
            *exitCode = -1;
        return {};
    }
    if (exitCode)
        *exitCode = process.exitCode();
    return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
}

inline bool writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

} // namespace TestSupport
