#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>

#include "deck.h"
#include "studyplanner.h"

namespace TestSupport {

inline Card makeCard(const QString &id, const QString &question, const QString &answer,
                     const QString &deckName = QStringLiteral("Deck")) {
    Card card;
    card.id = id;
    card.question = question;
    card.answer = answer;
    card.deck = deckName;
    return card;
}

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
    int newCardsOnStudyDay(int) const override { return 0; }
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

// Isolates the home directory that QDir::homePath() resolves for the bundled
// agent skill. No other environment variable controls the install target, and
// runCli*() inherits this HOME through the child process environment.
class HomeGuard {
public:
    explicit HomeGuard(const QString &home) : m_previous(qgetenv("HOME")) { qputenv("HOME", home.toUtf8()); }

    ~HomeGuard() {
        if (!m_previous.isNull())
            qputenv("HOME", m_previous);
        else
            qunsetenv("HOME");
    }

private:
    QByteArray m_previous;
};

inline QString appBinary() {
    return QDir(QString(OMACARDS_SOURCE_DIR)).filePath("../build/omacards");
}

inline QProcessEnvironment cliProcessEnvironment(const QString &decksDir, const QString &stateDb,
                                                 const QString &cacheDir = {}) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("OMACARDS_DECKS_DIR"), decksDir);
    environment.insert(QStringLiteral("OMACARDS_STATE_DB"), stateDb);
    if (!cacheDir.isEmpty())
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), cacheDir);
    return environment;
}

inline QByteArray runCliRaw(const QStringList &arguments, const QString &decksDir, const QString &stateDb,
                            int *exitCode = nullptr, QByteArray *standardError = nullptr,
                            const QByteArray *standardInput = nullptr) {
    QProcess process;
    process.setProcessEnvironment(cliProcessEnvironment(decksDir, stateDb));
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

// Saves and restores any environment variable a test changes. Math tests use
// it for PATH, XDG_CACHE_HOME and the fake toolchain's argument log.
class ScopedEnv {
public:
    ScopedEnv(const char *name, const QString &value) : m_name(name) {
        m_previous = qgetenv(name);
        m_present = !m_previous.isNull();
        qputenv(name, value.toUtf8());
    }

    ~ScopedEnv() {
        if (m_present)
            qputenv(m_name.constData(), m_previous);
        else
            qunsetenv(m_name.constData());
    }

private:
    QByteArray m_name;
    QByteArray m_previous;
    bool m_present = false;
};

class PathPrependGuard {
public:
    explicit PathPrependGuard(const QString &directory) : m_previous(qgetenv("PATH")) {
        qputenv("PATH", (directory + ":" + QString::fromLocal8Bit(m_previous)).toLocal8Bit());
    }

    ~PathPrependGuard() { qputenv("PATH", m_previous); }

private:
    QByteArray m_previous;
};

// A deterministic TeX toolchain: tectonic writes a PDF, pdftocairo writes the
// supplied SVG. Each invocation appends its argv to $OMACARDS_TEST_ARGS so a
// test can count renders instead of measuring milliseconds.
inline bool writeFakePdfToolchain(const QString &binDir, const QString &svg, const QString &delayScript = {}) {
    if (!QDir().mkpath(binDir))
        return false;
    const auto writeTool = [&](const QString &name, const QString &body) {
        const QString path = QDir(binDir).filePath(name);
        const QString script = QStringLiteral("#!/bin/sh\n"
                                              "printf '%s' '") +
                               name +
                               QStringLiteral("' >> \"$OMACARDS_TEST_ARGS\"\n"
                                              "for arg do printf ' %s' \"$arg\" >> \"$OMACARDS_TEST_ARGS\"; done\n"
                                              "printf '\\n' >> \"$OMACARDS_TEST_ARGS\"\n") +
                               body;
        return writeFile(path, script.toUtf8()) &&
               QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup |
                                               QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther);
    };
    const QString tectonic = QStringLiteral(R"sh(
out=
tex=
for arg do
    case "$arg" in --outdir=*) out=${arg#--outdir=} ;; esac
    tex="$arg"
done
base=${tex##*/}
base=${base%.tex}
printf '%%PDF-1.4\n' > "$out/$base.pdf"
)sh");
    const QString cairo = QStringLiteral("svg=\nfor arg do svg=\"$arg\"; done\n") + delayScript +
                          QStringLiteral("printf '%s\\n' '") + svg + QStringLiteral("' > \"$svg\"\n");
    return writeTool(QStringLiteral("tectonic"), tectonic) && writeTool(QStringLiteral("pdftocairo"), cairo);
}

inline int toolchainRenders(const QString &logPath) {
    QFile log(logPath);
    if (!log.open(QIODevice::ReadOnly))
        return 0;
    return QString::fromUtf8(log.readAll()).count(QStringLiteral(".tex"));
}

// The renderer colors the cached master by emitting a data URI. Tests read the
// SVG back out of that URI instead of inspecting rendered pixels.
inline QString svgFromMathHtml(const QString &html) {
    static const QString prefix = QStringLiteral("<img src='data:image/svg+xml;base64,");
    const int start = html.indexOf(prefix);
    if (start < 0)
        return {};
    const int end = html.indexOf('\'', start + prefix.size());
    if (end < 0)
        return {};
    const QByteArray encoded = html.mid(start + prefix.size(), end - start - prefix.size()).toLocal8Bit();
    return QString::fromUtf8(QByteArray::fromBase64(encoded));
}

// QML's Text.RichText reads <img> size through QTextDocument. Parsing the
// markup back catches malformed attributes: a broken alt leaves width and
// height at zero even though the emitted tag still contains them.
inline QSize parsedMathImageSize(const QString &html) {
    QTextDocument document;
    document.setHtml(html);
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const QTextImageFormat image = fragment.charFormat().toImageFormat();
            if (image.isValid())
                return QSize(qRound(image.width()), qRound(image.height()));
        }
    }
    return {};
}

} // namespace TestSupport
