#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "cli.h"
#include "deck.h"
#include "domain.h"
#include "fsrs.h"
#include "mathrender.h"
#include "store.h"
#include "theme.h"
#include "translator.h"

namespace {
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
        if (!decks.isNull()) qputenv("OMACARDS_DECKS_DIR", decks);
        else qunsetenv("OMACARDS_DECKS_DIR");
        if (!state.isNull()) qputenv("OMACARDS_STATE_DB", state);
        else qunsetenv("OMACARDS_STATE_DB");
    }
};
}

class OmacardsTests : public QObject {
    Q_OBJECT

private slots:
    void helpIsHeadless() {
        QVERIFY(Cli::needsOffscreen({"--help"}));
        QVERIFY(Cli::needsOffscreen({"-h"}));
        QVERIFY(Cli::needsOffscreen({"help"}));
        QVERIFY(!Cli::needsOffscreen({"open"}));

        if (!QFileInfo::exists(appBinary()))
            QSKIP("Build the app before running CLI integration tests.");
        QProcess process;
        process.start(appBinary(), {"--help"});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains("Usage:"));
    }

    void addCardsCreatesDeckBulk() {
        QTemporaryDir dir;
        EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
        const QString input = dir.filePath("cards.json");
        QVERIFY(writeFile(input, R"({"description":"Bulk math","cards":[
            {"question":"Q1?","answer":"A1","tags":["x","y"]},
            {"question":"Q2?","answer":"A2","type":"cloze","tags":"z, w"}]})"));

        QCOMPARE(Cli::run({"add-cards", "-d", "Bulk", "-i", input, "--json"}), 0);

        QString error;
        const Deck deck = DeckIO::load(QDir(dir.filePath("decks")).filePath("Bulk.txt"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(deck.description, QString("Bulk math"));
        QCOMPARE(deck.cards.size(), 2);
        QCOMPARE(deck.cards.at(0).tags, QStringList({"x", "y"}));
        QCOMPARE(deck.cards.at(1).type, QString("cloze"));
        QCOMPARE(deck.cards.at(1).tags, QStringList({"z", "w"}));
    }

    void addCardsRejectsInvalidInputWithoutWriting() {
        QTemporaryDir dir;
        EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
        const QString input = dir.filePath("bad.json");
        QVERIFY(writeFile(input, R"({"cards":[
            {"question":"ok","answer":"a"},{"answer":"no question"}]})"));

        QCOMPARE(Cli::run({"add-cards", "-d", "Bad", "-i", input, "--json"}), 1);
        QVERIFY(!QFileInfo::exists(QDir(dir.filePath("decks")).filePath("Bad.txt")));
    }

    void addDeckDescriptionAndJsonImport() {
        QTemporaryDir dir;
        EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
        QCOMPARE(Cli::run({"add-deck", "Empty", "--description", "No cards", "--json"}), 0);
        QString error;
        const Deck empty = DeckIO::load(QDir(dir.filePath("decks")).filePath("Empty.txt"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(empty.description, QString("No cards"));

        const QString input = dir.filePath("Imported.json");
        QVERIFY(writeFile(input, R"({"description":"from import","cards":[
            {"question":"j1","answer":"a1"}]})"));
        QCOMPARE(Cli::run({"import", input, "--json"}), 0);
        const Deck imported = DeckIO::load(QDir(dir.filePath("decks")).filePath("Imported.txt"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(imported.description, QString("from import"));
        QCOMPARE(imported.cards.size(), 1);
    }

    void previewRendersMathAsText() {
        QTemporaryDir dir;
        EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
        const QString input = dir.filePath("cards.json");
        QVERIFY(writeFile(input, R"json({"cards":[{"question":"Expand \\(x^2\\)",
                                                  "answer":"\\(\\frac{1}{2}\\)"}]})json"));
        int code = -1;
        runCli({"add-cards", "-d", "Math", "-i", input, "--json"}, dir.filePath("decks"),
               dir.filePath("state.db"), &code);
        QCOMPARE(code, 0);

        const QJsonObject result = runCli({"preview", "--deck", "Math", "--json"},
                                          dir.filePath("decks"), dir.filePath("state.db"), &code);
        QCOMPARE(code, 0);
        const QJsonArray items = result.value(QStringLiteral("items")).toArray();
        QCOMPARE(items.size(), 1);
        const QJsonObject item = items.at(0).toObject();
        QCOMPARE(item.value(QStringLiteral("question")).toString(), QString("Expand x²"));
        QCOMPARE(item.value(QStringLiteral("answer")).toString(), QString("(1)/(2)"));
    }

    void checkMathReportsUnsupportedCommands() {
        QTemporaryDir dir;
        EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
        const QString input = dir.filePath("cards.json");
        QVERIFY(writeFile(input, R"json({"cards":[{"question":"Bad \\(\\foobar{x}\\)",
                                                  "answer":"ok"}]})json"));
        int code = -1;
        runCli({"add-cards", "-d", "Warn", "-i", input, "--json"}, dir.filePath("decks"),
               dir.filePath("state.db"), &code);
        QCOMPARE(code, 0);

        const QJsonObject result = runCli({"check", "--math", "--json"}, dir.filePath("decks"),
                                          dir.filePath("state.db"), &code);
        QCOMPARE(code, 0);
        const QJsonArray warnings = result.value(QStringLiteral("mathWarnings")).toArray();
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(warnings.at(0).toObject().value(QStringLiteral("message")).toString(),
                 QString("Unsupported math command: \\foobar"));
    }

    void checkReportsLineNumberedIssues() {
        QTemporaryDir dir;
        EnvGuard env(dir.filePath("decks"), dir.filePath("state.db"));
        QVERIFY(QDir().mkpath(dir.filePath("decks")));
        const QString path = dir.filePath("decks/Broken.txt");
        QVERIFY(writeFile(path, R"deck(# Deck: Broken
---
Q: Same question
A: one
---
Q: Same question
A: two
---
Q: Good question
A: ok
oops: not a key
   too much indent
)deck"));

        int code = -1;
        const QJsonObject result = runCli({"check", "--json"}, dir.filePath("decks"),
                                          dir.filePath("state.db"), &code);
        QCOMPARE(code, 0);
        QVERIFY(result.value(QStringLiteral("ok")).toBool());

        const QJsonArray issues = result.value(QStringLiteral("issues")).toArray();
        const QJsonObject counts = result.value(QStringLiteral("issueCounts")).toObject();
        QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 0);
        QVERIFY(counts.value(QStringLiteral("warning")).toInt() >= 3);

        bool sawDuplicate = false;
        bool sawUnexpected = false;
        bool sawIndent = false;
        for (const QJsonValue &value : issues) {
            const QJsonObject issue = value.toObject();
            QVERIFY(!issue.value(QStringLiteral("hint")).toString().isEmpty());
            const QString message = issue.value(QStringLiteral("message")).toString();
            if (message == QStringLiteral("Duplicate question in deck")) {
                sawDuplicate = true;
                QCOMPARE(issue.value(QStringLiteral("line")).toInt(), 6);
                QVERIFY(issue.value(QStringLiteral("hint")).toString().contains(QStringLiteral("line 3")));
            } else if (message == QStringLiteral("Unexpected line in deck file")) {
                sawUnexpected = true;
                QCOMPARE(issue.value(QStringLiteral("line")).toInt(), 11);
            } else if (message == QStringLiteral("Continuation line has more than two leading spaces")) {
                sawIndent = true;
                QCOMPARE(issue.value(QStringLiteral("line")).toInt(), 12);
            }
        }
        QVERIFY(sawDuplicate);
        QVERIFY(sawUnexpected);
        QVERIFY(sawIndent);

        runCli({"check", "--strict", "--json"}, dir.filePath("decks"), dir.filePath("state.db"), &code);
        QCOMPARE(code, 1);
    }

    void defaultPathsIgnoreOrganizationName() {
        const QByteArray oldDecks = qgetenv("OMACARDS_DECKS_DIR");
        const QByteArray oldState = qgetenv("OMACARDS_STATE_DB");
        qunsetenv("OMACARDS_DECKS_DIR");
        qunsetenv("OMACARDS_STATE_DB");

        const QString oldOrg = QCoreApplication::organizationName();
        Backend withoutOrg;
        const QString first = withoutOrg.decksDirectory();
        QCoreApplication::setOrganizationName("omarchy");
        Backend withOrg;
        const QString second = withOrg.decksDirectory();
        QCoreApplication::setOrganizationName(oldOrg);

        QCOMPARE(first, second);
        QVERIFY(!first.contains(QStringLiteral("omarchy")));

        if (!oldDecks.isNull()) qputenv("OMACARDS_DECKS_DIR", oldDecks);
        if (!oldState.isNull()) qputenv("OMACARDS_STATE_DB", oldState);
    }

    void domainParsesSharedValues() {
        QCOMPARE(Domain::modeFromString("cram"), StudyMode::Cram);
        QCOMPARE(Domain::modeFromString("anything"), StudyMode::Review);
        QCOMPARE(Domain::toString(StudyMode::Review), QString("review"));
        QCOMPARE(Domain::ratingFromInt(1), Rating::Again);
        QCOMPARE(Domain::ratingFromInt(3), Rating::Good);
        QCOMPARE(Domain::ratingFromInt(2), Rating::Invalid);
        QCOMPARE(Domain::cardTypeFromString("cloze"), CardType::Cloze);
        QCOMPARE(Domain::cardTypeFromString(""), CardType::Basic);
        QCOMPARE(Domain::cardTypeFromString("other"), CardType::Invalid);
    }

    void fsrsInitialState() {
        QCOMPARE(Fsrs::initial().reps, 0);
        QVERIFY(Fsrs::initialStability(3) > Fsrs::initialStability(1));
        QVERIFY(Fsrs::initialDifficulty(1) > Fsrs::initialDifficulty(3));
        const double recall = Fsrs::retrievability(Fsrs::initial(), QDateTime::currentMSecsSinceEpoch());
        QCOMPARE(recall, 0.0);
    }

    void fsrsReviewMovesDueForward() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const ReviewResult first = Fsrs::review(Fsrs::initial(), 3, now);
        QVERIFY(first.state.stability > 0.0);
        QVERIFY(first.intervalDays >= 1);
        QVERIFY(first.state.due > now);
        QCOMPARE(first.state.reps, 1);

        // A same-day repeat grows stability without resetting the memory state.
        const ReviewResult repeat = Fsrs::review(first.state, 3, now);
        QVERIFY(repeat.state.stability >= first.state.stability);
        QCOMPARE(repeat.state.reps, 2);

        // A lapse after a long delay lowers stability and raises difficulty.
        const qint64 later = now + 30LL * 86400000LL;
        const ReviewResult lapse = Fsrs::review(repeat.state, 1, later);
        QVERIFY(lapse.state.stability < repeat.state.stability);
        QCOMPARE(lapse.state.lapses, 1);
        QVERIFY(lapse.retrievability < 1.0);
    }

    void deckParsesAndSerializes() {
        QTemporaryDir dir;
        const QString path = dir.filePath("Spanish.txt");
        const QString text = QStringLiteral(
            "# Deck: Spanish\n"
            "# Description: Words and phrases\n"
            "---\n"
            "Q: What is \"gato\"?\n"
            "A: cat\n"
            "tags: vocab, animals\n"
            "---\n"
            "Q: Solve \\(x^2+1=0\\)\n"
            "A: \\(x=\\pm i\\)\n"
            "id: fixed-id\n");
        QVERIFY(writeFile(path, text.toUtf8()));

        QString error;
        Deck deck = DeckIO::load(path, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(deck.name, QString("Spanish"));
        QCOMPARE(deck.description, QString("Words and phrases"));
        QCOMPARE(deck.cards.size(), 2);
        QCOMPARE(deck.cards.first().question, QString("What is \"gato\"?"));
        QCOMPARE(deck.cards.first().tags, QStringList({"vocab", "animals"}));
        QCOMPARE(deck.cards.at(1).id, QString("fixed-id"));
        QVERIFY(!deck.cards.first().id.isEmpty());

        const QString roundTrip = DeckIO::serialize(deck);
        QVERIFY(roundTrip.contains("# Deck: Spanish"));
        QVERIFY(roundTrip.contains("Q: What is \"gato\"?"));
        QVERIFY(roundTrip.contains("tags: vocab, animals"));
        QVERIFY(roundTrip.contains("id: fixed-id"));

        // Generated ids stay implicit so edited files remain readable.
        QVERIFY(!roundTrip.contains("id: " + deck.cards.first().id));
    }

    void clozeCardsHideAndReveal() {
        Card card;
        card.type = "cloze";
        card.question = "The {{c1::cat}} sleeps.";
        QVERIFY(DeckIO::displayQuestion(card).contains('['));
        QVERIFY(!DeckIO::displayQuestion(card).contains("cat"));
        QCOMPARE(DeckIO::displayAnswer(card), QString("The cat sleeps."));
    }

    void mathFallsBackToUnicode() {
        MathRenderer renderer(QDir::tempPath() + "/omacards-math-test");
        if (renderer.hasRenderer())
            QVERIFY(!renderer.renderExpression("x^2").isEmpty());
        QCOMPARE(unicodeMath("\\alpha + \\beta"), QString("α + β"));
        QCOMPARE(unicodeMath("\\pm i"), QString("± i"));
        QCOMPARE(unicodeMath("\\frac{1}{2}"), QString("(1)/(2)"));
        QCOMPARE(unicodeMath("\\frac{f^{(n)}(a)}{n!}"), QString("(f^(n)(a))/(n!)"));
        QCOMPARE(unicodeMath("\\binom{r}{n}"), QString("C(r,n)"));
        QCOMPARE(unicodeMath("\\sqrt{1+x}"), QString("√(1+x)"));
        QCOMPARE(unicodeMath("x^{2n+1}"), QString("x^(2n+1)"));
        QCOMPARE(unicodeMath("x \\le 2"), QString("x ≤ 2"));
        QCOMPARE(unicodeMath("a \\cdots b"), QString("a ⋯ b"));
        QVERIFY(renderer.warnings("\\(\\frac{1}{2}\\)").isEmpty());
        QCOMPARE(renderer.warnings("\\(\\foobar{x}\\)").size(), 1);
    }

    void tectonicRendererUsesSupportedFlags() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString bin = dir.filePath("bin");
        const QString cache = dir.filePath("cache");
        const QString argsLog = dir.filePath("args.log");
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(QDir().mkpath(cache));

        const auto makeTool = [&](const QString &name, const QByteArray &script) {
            const QString path = bin + "/" + name;
            QVERIFY(writeFile(path, script));
            QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                           | QFile::ReadGroup | QFile::ExeGroup
                                           | QFile::ReadOther | QFile::ExeOther);
        };
        const auto script = [](const QStringList &lines) {
            return (lines.join('\n') + '\n').toUtf8();
        };
        makeTool("tectonic", script({
            "#!/bin/sh",
            "printf 'tectonic' >> \"$OMACARDS_TEST_ARGS\"",
            "for arg do printf ' %s' \"$arg\" >> \"$OMACARDS_TEST_ARGS\"; done",
            "printf '\\n' >> \"$OMACARDS_TEST_ARGS\"",
            "cached=0",
            "for arg do if [ \"$arg\" = \"--only-cached\" ]; then cached=1; fi; done",
            "if [ \"$cached\" = \"1\" ] && [ \"$OMACARDS_TEST_MISSING_BUNDLE\" = \"1\" ]; then exit 1; fi",
            "out=",
            "tex=",
            "for arg do",
            "    case \"$arg\" in --outdir=*) out=${arg#--outdir=} ;; esac",
            "    tex=\"$arg\"",
            "done",
            "base=$(basename \"$tex\" .tex)",
            "printf '%%PDF-1.4\\n' > \"$out/$base.pdf\"",
        }));
        makeTool("pdftocairo", script({
            "#!/bin/sh",
            "printf 'pdftocairo' >> \"$OMACARDS_TEST_ARGS\"",
            "for arg do printf ' %s' \"$arg\" >> \"$OMACARDS_TEST_ARGS\"; done",
            "printf '\\n' >> \"$OMACARDS_TEST_ARGS\"",
            "printf '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" viewBox=\"0 0 10 5\"><g fill=\"rgb(0%%, 0%%, 0%%)\"/></svg>\\n' > \"$3\"",
        }));

        struct PathGuard {
            QByteArray original;
            explicit PathGuard(const QString &binDir)
                : original(qgetenv("PATH")) {
                qputenv("PATH", (binDir + ":" + QString::fromLocal8Bit(original)).toLocal8Bit());
            }
            ~PathGuard() { qputenv("PATH", original); }
        } pathGuard(bin);
        qputenv("OMACARDS_TEST_ARGS", argsLog.toUtf8());
        qputenv("OMACARDS_TEST_MISSING_BUNDLE", "1");
        struct TestVarGuard {
            ~TestVarGuard() {
                qunsetenv("OMACARDS_TEST_ARGS");
                qunsetenv("OMACARDS_TEST_MISSING_BUNDLE");
            }
        } varGuard;

        MathRenderer renderer(cache);
        QVERIFY(renderer.hasRenderer());
        QVERIFY(renderer.renderExpression("x^2").startsWith("<img "));
        QVERIFY(!QDir(cache).entryList({"*.svg"}).isEmpty());

        const QStringList texFiles = QDir(cache + "/tex").entryList({"*.tex"});
        QVERIFY(!texFiles.isEmpty());
        QFile texFile(cache + "/tex/" + texFiles.first());
        QVERIFY(texFile.open(QIODevice::ReadOnly));
        const QString tex = QString::fromUtf8(texFile.readAll());
        QVERIFY(tex.contains("\\hoffset=-1in"));
        QVERIFY(tex.contains("\\pdfpagewidth=\\wd0"));
        QVERIFY(tex.contains("\\shipout\\vbox"));

        // Ink is part of the cache key so themed equations do not reuse black SVGs.
        MathRenderer themed(cache);
        QVERIFY(themed.renderExpression("x^2", "#c0caf5").startsWith("<img "));
        bool foundThemedSvg = false;
        for (const QString &name : QDir(cache).entryList({"*.svg"})) {
            QFile svg(cache + "/" + name);
            if (!svg.open(QIODevice::ReadOnly))
                continue;
            const QString content = QString::fromUtf8(svg.readAll());
            if (content.contains("#c0caf5") && !content.contains("rgb(0%, 0%, 0%)"))
                foundThemedSvg = true;
        }
        QVERIFY(foundThemedSvg);

        // Scale enlarges the SVG itself and the <img> pixel dimensions so the
        // 12pt TeX glyphs stay readable beside 24-30px QML labels.
        const QString scaledHtml = themed.renderExpression("x^2", "#70d6ff", 2.0);
        QVERIFY(scaledHtml.contains("width='27'"));
        QVERIFY(scaledHtml.contains("height='13'"));
        bool foundScaledSvg = false;
        for (const QString &name : QDir(cache).entryList({"*.svg"})) {
            QFile svg(cache + "/" + name);
            if (!svg.open(QIODevice::ReadOnly))
                continue;
            const QString content = QString::fromUtf8(svg.readAll());
            if (content.contains("#70d6ff") && content.contains("width=\"54px\" height=\"26px\""))
                foundScaledSvg = true;
        }
        QVERIFY(foundScaledSvg);

        QFile logFile(argsLog);
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(logFile.readAll());
        QVERIFY(log.contains("tectonic --untrusted --only-cached --outdir="));
        QVERIFY(log.contains("tectonic --untrusted --outdir="));
        QVERIFY(!log.contains("-no-shell-escape"));
        QVERIFY(log.contains("pdftocairo -svg "));
    }

    void storeRoundTripsState() {
        QTemporaryDir dir;
        Store store(dir.filePath("state.db"));
        QString error;
        QVERIFY(store.open(&error));
        QVERIFY(error.isEmpty());

        FsrsState state = Fsrs::review(Fsrs::initial(), 3, QDateTime::currentMSecsSinceEpoch()).state;
        store.save("card-1", "hash-1", state);
        store.logReview("card-1", 3, state.lastReview, state, 3);

        QCOMPARE(store.states().value("card-1").reps, 1);
        QCOMPARE(store.hashes().value("card-1"), QString("hash-1"));
        QCOMPARE(store.states().value("card-1").stability, state.stability);

        store.removeExcept({});
        QVERIFY(store.states().isEmpty());

        const qint64 today = QDateTime::currentMSecsSinceEpoch();
        store.logReview("card-2", 3, today, state, 3);
        store.logReview("card-2", 1, today - 86400000LL, state, 1);
        const ReviewSummary summary = store.summary();
        QCOMPARE(summary.total, 3);
        QCOMPARE(summary.today, 2);
        QCOMPARE(summary.todayCorrect, 2);
        QCOMPARE(summary.streakDays, 2);
    }

    void backendCreatesCardsAndCramsForever() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(error.isEmpty());

        QVERIFY(backend.createDeck("Test", &error));
        QVERIFY(backend.createCard("Test", "Question?", "Answer", "basic", "tag", &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(backend.totalCards(), 1);
        QCOMPARE(backend.decks(), QStringList({"Test"}));

        backend.selectDeck("Test");
        backend.setMode("cram");
        QVERIFY(backend.hasCard());
        for (int i = 0; i < 12; ++i) {
            backend.flip();
            QVERIFY(backend.showingAnswer());
            backend.rate(i % 3 == 0 ? 1 : 3);
            QVERIFY(backend.hasCard()); // endless: the card comes back on every box interval
        }
        QCOMPARE(backend.reviewed(), 12);
        QVERIFY(backend.correct() > 0);
    }

    void qmlCanRateCards() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Test", &error));
        QVERIFY(backend.createCard("Test", "Question?", "Answer", "basic", {}, &error));
        backend.selectDeck("Test");
        backend.setMode("cram");
        backend.flip();

        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick 2.15
            QtObject {
                Component.onCompleted: backend.rate(3)
            }
        )QML", QUrl("qrc:/rate-test.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *object = component.create();
        QVERIFY2(object, qPrintable(component.errorString()));
        delete object;

        QCOMPARE(backend.reviewed(), 1);
        QCOMPARE(backend.correct(), 1);
    }

    void backendEditsCardsWithUndo() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Test", &error));
        QVERIFY(backend.createCard("Test", "Question?", "Answer", "basic", "tag", &error));

        const Card original = backend.allDecks().last().cards.first();
        const QString changedId = original.id;
        QVERIFY(backend.modifyCard("Test", changedId, "Edited?", "Better", "cloze", "a, b", &error));
        QVERIFY(error.isEmpty());
        const Card edited = backend.allDecks().last().cards.first();
        QCOMPARE(edited.question, QString("Edited?"));
        QCOMPARE(edited.answer, QString("Better"));
        QCOMPARE(edited.type, QString("cloze"));
        QCOMPARE(edited.tags, QStringList({"a", "b"}));
        QVERIFY(edited.id != changedId); // content hash follows edits
        QVERIFY(backend.canUndo());

        backend.undo();
        QCOMPARE(backend.allDecks().last().cards.first().question, QString("Question?"));
        QVERIFY(backend.canRedo());

        backend.redo();
        QCOMPARE(backend.allDecks().last().cards.first().question, QString("Edited?"));

        QVERIFY(backend.deleteCard("Test", backend.allDecks().last().cards.first().id, &error));
        QVERIFY(backend.allDecks().last().cards.isEmpty());
        backend.undo();
        QCOMPARE(backend.allDecks().last().cards.size(), 1);
    }

    void backendDeletesDeckAndUndoes() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Temporary", &error));
        QVERIFY(backend.decks().contains("Temporary"));

        QVERIFY(backend.removeDeck("Temporary", &error));
        QVERIFY(!backend.decks().contains("Temporary"));
        QVERIFY(!QFileInfo::exists(dir.filePath("Temporary.txt")));

        backend.undo();
        QVERIFY(backend.decks().contains("Temporary"));
        QVERIFY(QFileInfo::exists(dir.filePath("Temporary.txt")));
    }

    void backendFiltersCardList() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Filter", &error));
        QVERIFY(backend.createCard("Filter", "alpha question", "one", "basic", "red", &error));
        QVERIFY(backend.createCard("Filter", "beta question", "two", "basic", "blue", &error));
        backend.selectDeck("Filter");
        QCOMPARE(backend.cardList().size(), 2);

        backend.setCardFilter("blue");
        QCOMPARE(backend.cardList().size(), 1);
        QCOMPARE(backend.cardList().first().toMap().value("question").toString(), QString("beta question"));

        backend.setCardFilter("alpha");
        QCOMPARE(backend.cardList().size(), 1);
        QCOMPARE(backend.searchCards("alpha").size(), 1);
    }

    void storeRemembersSettings() {
        QTemporaryDir dir;
        const QString path = dir.filePath("state.db");
        {
            Store store(path);
            QString error;
            QVERIFY(store.open(&error));
            QCOMPARE(store.setting("new_per_day", "20"), QString("20"));
            QVERIFY(store.setSetting("new_per_day", "12"));
            QVERIFY(store.setSetting("desired_retention", "0.85"));
        }
        Store reopened(path);
        QString error;
        QVERIFY(reopened.open(&error));
        QCOMPARE(reopened.setting("new_per_day", "20"), QString("12"));
        QCOMPARE(reopened.setting("desired_retention", "0.90"), QString("0.85"));
    }

    void backendImportsAnkiClozeAndMedia() {
        QTemporaryDir dir;
        const QString package = dir.filePath("sample.apkg");
        const QString database = dir.filePath("collection.anki2");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "apkg-fixture");
            db.setDatabaseName(database);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TABLE models (id INTEGER, type INTEGER)"));
            QVERIFY(query.exec("INSERT INTO models VALUES (1, 0), (2, 1)"));
            QVERIFY(query.exec("CREATE TABLE notes (flds TEXT, mid INTEGER, tags TEXT)"));
            const QString sep(1, QChar(0x1f));
            QVERIFY(query.exec(QString("INSERT INTO notes VALUES ('Basic &amp; <b>bold</b>%1back with <img src=\"0\">', 1, ' tag1 tag2 ')").arg(sep)));
            QVERIFY(query.exec("INSERT INTO notes VALUES ('The {{c1::cat::animal}} sleeps', 2, 'cloze')"));
            db.close();
        }
        QSqlDatabase::removeDatabase("apkg-fixture");
        QFile mediaMap(dir.filePath("media"));
        QVERIFY(mediaMap.open(QIODevice::WriteOnly));
        mediaMap.write(R"({"0":"image.png"})");
        mediaMap.close();
        QFile image(dir.filePath("0"));
        QVERIFY(image.open(QIODevice::WriteOnly));
        image.write("png-bytes");
        image.close();

        QProcess zip;
        zip.setWorkingDirectory(dir.path());
        zip.start("zip", {"-q", package, "collection.anki2", "media", "0"});
        QVERIFY(zip.waitForFinished(10000));
        QCOMPARE(zip.exitCode(), 0);

        const QString decks = dir.filePath("decks");
        Backend backend(decks, dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY2(backend.importApkg(package, "Anki", &error), qPrintable(error));
        QVERIFY(backend.decks().contains("Anki"));
        QVector<Card> cards;
        for (const Deck &deck : backend.allDecks())
            if (deck.name == "Anki") cards = deck.cards;
        QCOMPARE(cards.size(), 2);
        const Card *basic = nullptr;
        const Card *cloze = nullptr;
        for (const Card &card : cards) {
            if (card.type == "cloze") cloze = &card;
            else basic = &card;
        }
        QVERIFY(basic && cloze);
        QCOMPARE(basic->question, QString("Basic & bold"));
        QVERIFY(basic->answer.contains("<img src=\"file://"));
        QCOMPARE(basic->tags, QStringList({"tag1", "tag2"}));
        QCOMPARE(cloze->question, QString("The {{c1::cat}} sleeps"));
        QVERIFY(QFile::exists(decks + "/media/Anki/image.png"));
    }

    void storeCountsNewCardsToday() {
        QTemporaryDir dir;
        Store store(dir.filePath("state.db"));
        QString error;
        QVERIFY(store.open(&error));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
        store.logReview("new-1", 3, now, state, 1, true);
        store.logReview("new-1", 3, now, state, 1, true); // same card twice
        store.logReview("old-1", 3, now, state, 1, false);
        QCOMPARE(store.newCardsToday(), 1);
        QCOMPARE(store.summary().newToday, 1);

        store.logReview("new-2", 1, now, state, 1, true);
        QCOMPARE(store.newCardsToday(), 2);
    }

    void fsrsRetentionShapesIntervals() {
        QVERIFY(Fsrs::interval(10.0, 0.80) > Fsrs::interval(10.0, 0.90));
        QVERIFY(Fsrs::interval(10.0, 0.98) < Fsrs::interval(10.0, 0.90));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const ReviewResult relaxed = Fsrs::review(Fsrs::initial(), 3, now, 0.80);
        const ReviewResult strict = Fsrs::review(Fsrs::initial(), 3, now, 0.95);
        QVERIFY(relaxed.intervalDays >= strict.intervalDays);
    }

    void backendReportsNewCountAndDeckSummaries() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Alpha", &error));
        for (int i = 0; i < 4; ++i)
            QVERIFY(backend.createCard("Alpha", QString("A%1").arg(i), "a", "basic", {}, &error));
        QVERIFY(backend.createDeck("Beta", &error));
        QVERIFY(backend.createCard("Beta", "B0", "b", "basic", {}, &error));

        QCOMPARE(backend.totalCards(), 5);
        QCOMPARE(backend.newCount(), 5);

        const QVariantList summaries = backend.deckSummaries();
        QCOMPARE(summaries.size(), 2);
        QCOMPARE(summaries.at(0).toMap().value("name").toString(), QString("Alpha"));
        QCOMPARE(summaries.at(0).toMap().value("cards").toInt(), 4);
        QCOMPARE(summaries.at(0).toMap().value("new").toInt(), 4);
        QCOMPARE(summaries.at(1).toMap().value("name").toString(), QString("Beta"));

        backend.setNewPerDay(3);
        QCOMPARE(backend.newCount(), 3);
        const QVariantList capped = backend.deckSummaries();
        QCOMPARE(capped.at(0).toMap().value("new").toInt(), 3);
        QCOMPARE(capped.at(1).toMap().value("new").toInt(), 0);
    }

    void qmlCanNavigateLibraryCards() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Test", &error));
        for (int i = 0; i < 3; ++i)
            QVERIFY(backend.createCard("Test", QString("Q%1").arg(i), "A", "basic", {}, &error));
        backend.selectDeck("Test");

        QQmlEngine engine;
        Theme theme;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("theme", &theme);
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QStringLiteral(OMACARDS_SOURCE_DIR) + "/Main.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *window = component.create();
        QVERIFY2(window, qPrintable(component.errorString()));

        QCOMPARE(window->property("cardIndex").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(window, "moveCardSelection", Q_ARG(QVariant, 1)));
        QCOMPARE(window->property("cardIndex").toInt(), 1);
        QVERIFY(QMetaObject::invokeMethod(window, "moveCardSelection", Q_ARG(QVariant, -1)));
        QCOMPARE(window->property("cardIndex").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(window, "moveCardSelection", Q_ARG(QVariant, -1)));
        QCOMPARE(window->property("cardIndex").toInt(), 2); // wraps around

        QVariant selected;
        QVERIFY(QMetaObject::invokeMethod(window, "selectedCard", Q_RETURN_ARG(QVariant, selected)));
        QCOMPARE(selected.toMap().value("question").toString(), QString("Q2"));

        QVERIFY(QMetaObject::invokeMethod(window, "removeSelectedCard"));
        QCOMPARE(backend.totalCards(), 2);
        QCOMPARE(backend.cardList().size(), 2);
        delete window;
    }

    void backendAppliesNewCardLimit() {
        QTemporaryDir dir;
        const QString database = dir.filePath("state.db");
        {
            Backend backend(dir.path(), database);
            QString error;
            QVERIFY(backend.initialize(&error));
            QVERIFY(backend.createDeck("Limit", &error));
            for (int i = 0; i < 5; ++i)
                QVERIFY(backend.createCard("Limit", QString("Q%1").arg(i), "A", "basic", {}, &error));
            backend.selectDeck("Limit");
            QCOMPARE(backend.countDue(), 5);
            QCOMPARE(backend.availableCount(), 5);

            backend.setNewPerDay(2);
            QCOMPARE(backend.availableCount(), 2);
            QCOMPARE(backend.dueCount(), 2);
            QCOMPARE(backend.reviewSummary().newToday, 0);

            backend.flip();
            backend.rate(3);
            backend.flip();
            backend.rate(3);
            QCOMPARE(backend.reviewSummary().newToday, 2);
            QCOMPARE(backend.availableCount(), 0);
            QCOMPARE(backend.dueCount(), 0);

            backend.setNewPerDay(3);
            QCOMPARE(backend.availableCount(), 1);
            backend.setDesiredRetention(0.85);
        }
        Backend reopened(dir.path(), database);
        QString error;
        QVERIFY(reopened.initialize(&error));
        QCOMPARE(reopened.newPerDay(), 3);
        QCOMPARE(reopened.desiredRetention(), 0.85);
    }

    void backendRequeuesAgainInSameSession() {
        QTemporaryDir dir;
        const QByteArray previousDelay = qgetenv("OMACARDS_AGAIN_DELAY_MS");
        qputenv("OMACARDS_AGAIN_DELAY_MS", "20");
        {
            Backend backend(dir.path(), dir.filePath("state.db"));
            QString error;
            QVERIFY(backend.initialize(&error));
            QVERIFY(backend.createDeck("Repeats", &error));
            QVERIFY(backend.createCard("Repeats", "Q1", "A1", "basic", {}, &error));
            QVERIFY(backend.createCard("Repeats", "Q2", "A2", "basic", {}, &error));
            backend.selectDeck("Repeats");
            QVERIFY(backend.question().contains("Q1"));

            backend.flip();
            backend.rate(1); // Again: Q1 waits, Q2 is shown next.
            QVERIFY(backend.hasCard());
            QVERIFY(backend.question().contains("Q2"));

            backend.flip();
            backend.rate(3); // Good: the waiting Q1 comes back after the break.
            QVERIFY(backend.waitingForRepeat());
            QTRY_VERIFY_WITH_TIMEOUT(backend.question().contains("Q1"), 2000);
            QVERIFY(!backend.waitingForRepeat());

            const QVariantList cards = backend.cardList();
            QVariantMap againCard;
            for (const QVariant &entry : cards) {
                const QVariantMap map = entry.toMap();
                if (map.value("question").toString() == "Q1")
                    againCard = map;
            }
            QVERIFY(!againCard.isEmpty());
            QCOMPARE(againCard.value("reps").toInt(), 1);
            QVERIFY(againCard.value("due").toLongLong() > QDateTime::currentMSecsSinceEpoch());

            backend.flip();
            backend.rate(3);
            QVERIFY(!backend.hasCard());
        }
        if (previousDelay.isNull())
            qunsetenv("OMACARDS_AGAIN_DELAY_MS");
        else
            qputenv("OMACARDS_AGAIN_DELAY_MS", previousDelay);
    }

    void unreadableDeckKeepsSchedulingState() {
        QTemporaryDir dir;
        const QString database = dir.filePath("state.db");
        const QString deckPath = QDir(dir.path()).filePath("Keep.txt");
        {
            Backend backend(dir.path(), database);
            QString error;
            QVERIFY(backend.initialize(&error));
            QVERIFY(backend.createDeck("Keep", &error));
            QVERIFY(backend.createCard("Keep", "Q1", "A1", "basic", {}, &error));
            backend.selectDeck("Keep");
            backend.flip();
            backend.rate(3);

            Store store(database);
            QVERIFY(store.open(&error));
            const QString cardId = backend.cardList().first().toMap().value("id").toString();
            QVERIFY(store.states().contains(cardId));

            QVERIFY(QFile::setPermissions(deckPath, QFile::Permissions{}));
            if (QFileInfo(deckPath).isReadable()) {
                QFile::setPermissions(deckPath, QFile::ReadOwner | QFile::WriteOwner);
                QSKIP("file permissions are ignored for this user");
            }
            backend.refresh();
            QVERIFY2(store.states().contains(cardId), "an unreadable deck must not erase its FSRS state");
            QFile::setPermissions(deckPath, QFile::ReadOwner | QFile::WriteOwner);
        }
    }

    void failedCardWriteDoesNotMutateBackend() {
        QTemporaryDir dir;
        {
            Backend backend(dir.path(), dir.filePath("state.db"));
            QString error;
            QVERIFY(backend.initialize(&error));
            QVERIFY(backend.createDeck("Files", &error));
            QVERIFY(backend.createCard("Files", "Q1", "A1", "basic", {}, &error));
            QCOMPARE(backend.cardList().size(), 1);

            QVERIFY(QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::ExeOwner));
            if (QFileInfo(dir.path()).isWritable()) {
                QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                QSKIP("directory permissions are ignored for this user");
            }
            QVERIFY(!backend.createCard("Files", "Q2", "A2", "basic", {}, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(backend.cardList().size(), 1);
            QVERIFY(backend.question().contains("Q1"));
            QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
    }

    void unreadableStateDoesNotTreatEveryCardAsNew() {
        QTemporaryDir dir;
        const QString database = dir.filePath("state.db");
        Backend backend(dir.path(), database);
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Deck", &error));
        QVERIFY(backend.createCard("Deck", "Q1", "A1", "basic", {}, &error));
        QCOMPARE(backend.newCount(), 1);

        // Simulate a failed query after startup (schema drift, damaged DB).
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "omacards-unreadable-test");
        db.setDatabaseName(database);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("DROP TABLE cards"));
        query.finish();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("omacards-unreadable-test");

        backend.refresh();
        QCOMPARE(backend.totalCards(), 1);
        QCOMPARE(backend.availableCount(), 0);
        QCOMPARE(backend.newCount(), 0);
        QCOMPARE(backend.dueCount(), 0);
        QVERIFY(!backend.hasCard());
    }

    void deckCountsMatchLibrarySummaries() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("First", &error));
        for (int i = 0; i < 2; ++i)
            QVERIFY(backend.createCard("First", QString("F%1").arg(i), "a", "basic", {}, &error));
        QVERIFY(backend.createDeck("Second", &error));
        for (int i = 0; i < 2; ++i)
            QVERIFY(backend.createCard("Second", QString("S%1").arg(i), "b", "basic", {}, &error));

        backend.setNewPerDay(3);
        backend.selectDeck("Second");
        const QVariantList summaries = backend.deckSummaries();
        QCOMPARE(summaries.at(0).toMap().value("new").toInt(), 2);
        QCOMPARE(summaries.at(1).toMap().value("new").toInt(), 1);
        QCOMPARE(backend.deckNew(), 1);
        QCOMPARE(backend.deckDue(), 1);
        QCOMPARE(backend.newCount(), 3);
        QCOMPARE(backend.availableCount(), 3);
    }

    void deckCreationRejectsSanitizedPathCollision() {
        QTemporaryDir dir;
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("A/B", &error));
        QVERIFY(!backend.createDeck("A-B", &error));
        QCOMPARE(backend.decks().size(), 1);
        QCOMPARE(backend.decks().first(), QString("A/B"));
    }

    void refreshKeepsPendingAgainRepeat() {
        QTemporaryDir dir;
        const QByteArray previousDelay = qgetenv("OMACARDS_AGAIN_DELAY_MS");
        qputenv("OMACARDS_AGAIN_DELAY_MS", "50");
        {
            Backend backend(dir.path(), dir.filePath("state.db"));
            QString error;
            QVERIFY(backend.initialize(&error));
            QVERIFY(backend.createDeck("Repeats", &error));
            QVERIFY(backend.createCard("Repeats", "Q1", "A1", "basic", {}, &error));
            QVERIFY(backend.createCard("Repeats", "Q2", "A2", "basic", {}, &error));
            backend.selectDeck("Repeats");
            backend.flip();
            backend.rate(1);
            QVERIFY(backend.question().contains("Q2"));

            backend.refresh(); // same path the file watcher uses
            QVERIFY(backend.question().contains("Q2"));
            backend.flip();
            backend.rate(3);
            QTRY_VERIFY_WITH_TIMEOUT(backend.question().contains("Q1"), 2000);
        }
        if (previousDelay.isNull())
            qunsetenv("OMACARDS_AGAIN_DELAY_MS");
        else
            qputenv("OMACARDS_AGAIN_DELAY_MS", previousDelay);
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QVERIFY(writeFile(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void translatorFollowsLanguage() {
        QTemporaryDir dir;
        writeFile(dir.path() + "/language", "pt-BR\n");
        writeFile(dir.path() + "/pt.json", R"({"Save": "Guardar"})");
        writeFile(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

private:
    static QString appBinary() {
        return QDir(QString(OMACARDS_SOURCE_DIR)).filePath("../build/omacards");
    }

    static QJsonObject runCli(const QStringList &arguments, const QString &decksDir,
                              const QString &stateDb, int *exitCode = nullptr) {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("OMACARDS_DECKS_DIR"), decksDir);
        environment.insert(QStringLiteral("OMACARDS_STATE_DB"), stateDb);
        process.setProcessEnvironment(environment);
        process.start(appBinary(), arguments);
        if (!process.waitForFinished(10000)) {
            if (exitCode) *exitCode = -1;
            return {};
        }
        if (exitCode) *exitCode = process.exitCode();
        return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    }

    static bool writeFile(const QString &path, const QByteArray &contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    }
};

QTEST_MAIN(OmacardsTests)
#include "omacards_tests.moc"
