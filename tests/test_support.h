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

#include "studyplanner.h"

namespace TestSupport {

// Stub at the planner's Store seam: a failing read is visible only through
// lastError(), exactly as it is with the real Store.
class PlannerStoreStub : public StudyPlanner::StoreAdapter {
public:
    void clearError() override { m_error.clear(); }
    QHash<QString, FsrsState> states() const override {
        if (!m_readError.isEmpty())
            m_error = m_readError;
        return {};
    }
    QHash<QString, QString> hashes() const override { return {}; }
    int newCardsToday(int) const override { return 0; }
    QString lastError() const override { return m_error; }

    void failReads(const QString &error) { m_readError = error; }

private:
    QString m_readError;
    mutable QString m_error;
};

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

inline QByteArray runCliRaw(const QStringList &arguments, const QString &decksDir, const QString &stateDb,
                            int *exitCode = nullptr, QByteArray *standardError = nullptr,
                            const QByteArray *standardInput = nullptr) {
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("OMACARDS_DECKS_DIR"), decksDir);
    environment.insert(QStringLiteral("OMACARDS_STATE_DB"), stateDb);
    process.setProcessEnvironment(environment);
    process.start(appBinary(), arguments);
    if (standardInput) {
        process.write(*standardInput);
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(10000)) {
        if (exitCode)
            *exitCode = -1;
        if (standardError)
            *standardError = process.readAllStandardError();
        return {};
    }
    if (exitCode)
        *exitCode = process.exitCode();
    if (standardError)
        *standardError = process.readAllStandardError();
    return process.readAllStandardOutput();
}

inline QJsonObject runCli(const QStringList &arguments, const QString &decksDir, const QString &stateDb,
                          int *exitCode = nullptr, QByteArray *standardError = nullptr,
                          const QByteArray *standardInput = nullptr) {
    return QJsonDocument::fromJson(runCliRaw(arguments, decksDir, stateDb, exitCode, standardError, standardInput))
        .object();
}

inline QString runCliText(const QStringList &arguments, const QString &decksDir, const QString &stateDb,
                          int *exitCode = nullptr, QByteArray *standardError = nullptr) {
    return QString::fromUtf8(runCliRaw(arguments, decksDir, stateDb, exitCode, standardError));
}

inline bool writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

} // namespace TestSupport
