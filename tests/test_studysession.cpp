#include <QtTest>

#include <QByteArray>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>

#include "test_studysession.h"
#include "config.h"
#include "deck.h"
#include "deckrepository.h"
#include "domain.h"
#include "filehistory.h"
#include "mathrender.h"
#include "store.h"
#include "studyplanner.h"
#include "studysession.h"

namespace {

struct SessionFixture {
    QTemporaryDir dir;
    Store store;
    FileHistory history;
    DeckRepository repo;
    StudyPlanner planner;
    StudySession session;
    QString current;

    SessionFixture()
        : store(dir.filePath("state.db")), repo(dir.filePath("decks"), store, history), planner(store),
          session(repo, planner) {}
};

bool prepareSession(SessionFixture &fixture, const QString &deckName, const QStringList &questions, QString *error) {
    if (!fixture.repo.initialize(error))
        return false;
    if (!fixture.store.open(error))
        return false;
    fixture.repo.load(&fixture.current);
    if (fixture.repo.deckByName("Welcome")) {
        if (!fixture.repo.removeDeck("Welcome", &fixture.current, error))
            return false;
    }
    if (!fixture.repo.createDeck(deckName, &fixture.current, error))
        return false;
    for (const QString &question : questions) {
        if (!fixture.repo.createCard(deckName, &fixture.current, question, "A " + question, "basic", QString(), error))
            return false;
    }
    fixture.repo.load(&fixture.current);
    fixture.planner.setDecks(fixture.repo.deckData());
    fixture.planner.configure(Config::kDefaultNewPerDay, 0);
    QString storeError;
    if (!fixture.planner.refresh(&storeError)) {
        if (error)
            *error = storeError;
        return false;
    }
    fixture.session.rebuild(deckName);
    return true;
}

// StudySession reads the delay in its constructor, so the variable must be set
// before the fixture is built. RAII keeps it safe even if an assertion fails.
class AgainDelayGuard {
public:
    AgainDelayGuard() : m_previous(qgetenv("OMACARDS_AGAIN_DELAY_MS")) {}
    ~AgainDelayGuard() {
        if (m_previous.isNull())
            qunsetenv("OMACARDS_AGAIN_DELAY_MS");
        else
            qputenv("OMACARDS_AGAIN_DELAY_MS", m_previous);
    }

private:
    QByteArray m_previous;
};

} // namespace

void TestStudySession::setModeNormalizesInvalidInput() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Modes", {"Q1", "Q2"}, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(fixture.session.hasCard());

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.reviewed(), 1);
    QCOMPARE(fixture.session.correct(), 1);

    QSignalSpy modeSpy(&fixture.session, &StudySession::modeChanged);
    QSignalSpy statsSpy(&fixture.session, &StudySession::statsChanged);
    QStringList order;
    QObject::connect(&fixture.session, &StudySession::modeChanged, &fixture.session,
                     [&order] { order << QStringLiteral("mode"); });
    QObject::connect(&fixture.session, &StudySession::statsChanged, &fixture.session,
                     [&order] { order << QStringLiteral("stats"); });

    fixture.session.setMode("banana");
    QCOMPARE(fixture.session.mode(), StudyMode::Review);
    QCOMPARE(fixture.session.reviewed(), 0);
    QCOMPARE(fixture.session.correct(), 0);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(statsSpy.count(), 1);
    QCOMPARE(order, QStringList({"mode", "stats"}));

    // Backend forwards setMode() and then rebuilds; the session itself only
    // resets counters and emits.
    fixture.session.rebuild("Modes");
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q"));
}

void TestStudySession::setModeUppercaseCramNormalizesToReview() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Modes", {"Q1", "Q2"}, &error));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.reviewed(), 1);

    QSignalSpy modeSpy(&fixture.session, &StudySession::modeChanged);
    QSignalSpy statsSpy(&fixture.session, &StudySession::statsChanged);
    QStringList order;
    QObject::connect(&fixture.session, &StudySession::modeChanged, &fixture.session,
                     [&order] { order << QStringLiteral("mode"); });
    QObject::connect(&fixture.session, &StudySession::statsChanged, &fixture.session,
                     [&order] { order << QStringLiteral("stats"); });

    fixture.session.setMode("CRAM");
    QCOMPARE(fixture.session.mode(), StudyMode::Review);
    QCOMPARE(fixture.session.reviewed(), 0);
    QCOMPARE(fixture.session.correct(), 0);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(statsSpy.count(), 1);
    QCOMPARE(order, QStringList({"mode", "stats"}));

    fixture.session.rebuild("Modes");
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentBox(), 0); // remained in review mode
}

void TestStudySession::setModeCanonicalInputIsNoOp() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Modes", {"Q1", "Q2"}, &error));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.reviewed(), 1);
    QCOMPARE(fixture.session.correct(), 1);

    QSignalSpy modeSpy(&fixture.session, &StudySession::modeChanged);
    QSignalSpy statsSpy(&fixture.session, &StudySession::statsChanged);

    fixture.session.setMode("review");
    QCOMPARE(fixture.session.mode(), StudyMode::Review);
    QCOMPARE(fixture.session.reviewed(), 1);
    QCOMPARE(fixture.session.correct(), 1);
    QCOMPARE(modeSpy.count(), 0);
    QCOMPARE(statsSpy.count(), 0);

    fixture.session.setMode("cram");
    QCOMPARE(fixture.session.mode(), StudyMode::Cram);
    QCOMPARE(modeSpy.count(), 1);
    modeSpy.clear();
    statsSpy.clear();

    fixture.session.setMode("cram"); // canonical cram is also a no-op
    QCOMPARE(fixture.session.mode(), StudyMode::Cram);
    QCOMPARE(fixture.session.reviewed(), 0);
    QCOMPARE(modeSpy.count(), 0);
    QCOMPARE(statsSpy.count(), 0);
}

void TestStudySession::setModeCramSwitchesAndResets() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Modes", {"Q1", "Q2"}, &error));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.reviewed(), 1);
    QCOMPARE(fixture.session.correct(), 1);

    QSignalSpy modeSpy(&fixture.session, &StudySession::modeChanged);
    QSignalSpy statsSpy(&fixture.session, &StudySession::statsChanged);
    QStringList order;
    QObject::connect(&fixture.session, &StudySession::modeChanged, &fixture.session,
                     [&order] { order << QStringLiteral("mode"); });
    QObject::connect(&fixture.session, &StudySession::statsChanged, &fixture.session,
                     [&order] { order << QStringLiteral("stats"); });

    fixture.session.setMode("cram");
    QCOMPARE(fixture.session.mode(), StudyMode::Cram);
    QCOMPARE(fixture.session.reviewed(), 0);
    QCOMPARE(fixture.session.correct(), 0);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(statsSpy.count(), 1);
    QCOMPARE(order, QStringList({"mode", "stats"}));

    fixture.session.rebuild("Modes");
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentBox(), 1);
    QVERIFY(fixture.session.status().contains("Cram"));
}

void TestStudySession::invalidRatingsAreNoOps() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Ratings", {"Q1", "Q2"}, &error));

    QSignalSpy statsSpy(&fixture.session, &StudySession::statsChanged);
    QSignalSpy cardSpy(&fixture.session, &StudySession::cardChanged);
    QSignalSpy statusSpy(&fixture.session, &StudySession::statusChanged);
    QSignalSpy errorSpy(&fixture.session, &StudySession::errorOccurred);
    const QString statusBefore = fixture.session.status();
    const QString idBefore = fixture.session.currentCardId();

    // Raw ints are rejected at the Backend boundary; the session only has to
    // refuse the Invalid sentinel it can still receive from a direct caller.
    QVERIFY(!fixture.session.rate(Rating::Invalid, Config::kDefaultRetention));
    QCOMPARE(fixture.session.reviewed(), 0);
    QCOMPARE(fixture.session.correct(), 0);
    QCOMPARE(fixture.session.status(), statusBefore);
    QCOMPARE(fixture.session.currentCardId(), idBefore);
    QCOMPARE(statsSpy.count(), 0);
    QCOMPARE(cardSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);

    // An empty queue rejects even a valid rating without emitting anything.
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(!fixture.session.hasCard());
    statsSpy.clear();
    cardSpy.clear();
    statusSpy.clear();
    errorSpy.clear();

    QVERIFY(!fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.reviewed(), 2);
    QCOMPARE(fixture.session.correct(), 2);
    QCOMPARE(statsSpy.count(), 0);
    QCOMPARE(cardSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
}

void TestStudySession::reviewStatusOmitsSessionPositionBox() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Status", {"Q1"}, &error));

    QVERIFY(fixture.session.status().contains(QStringLiteral("predicted recall")));
    QVERIFY(!fixture.session.status().contains(QStringLiteral("FSRS box")));
}

void TestStudySession::flipAndShowNextToggleAnswer() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Flip", {"Q1"}, &error));
    QSignalSpy cardSpy(&fixture.session, &StudySession::cardChanged);

    QVERIFY(!fixture.session.showingAnswer());
    fixture.session.flip();
    QVERIFY(fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 1);

    fixture.session.flip();
    QVERIFY(!fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 2);

    fixture.session.flip();
    QVERIFY(fixture.session.showingAnswer());
    fixture.session.showNext(); // only flips when the answer is showing
    QVERIFY(!fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 4);

    fixture.session.showNext();
    QVERIFY(!fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 4);

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(!fixture.session.hasCard());
    cardSpy.clear();
    fixture.session.flip();
    fixture.session.showNext();
    QCOMPARE(cardSpy.count(), 0);
    QVERIFY(!fixture.session.showingAnswer());
}

void TestStudySession::setStatusDedupesAndReportErrorSignals() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Status", {"Q1"}, &error));

    QSignalSpy statusSpy(&fixture.session, &StudySession::statusChanged);
    QSignalSpy errorSpy(&fixture.session, &StudySession::errorOccurred);
    QStringList order;
    QObject::connect(&fixture.session, &StudySession::statusChanged, &fixture.session,
                     [&order] { order << QStringLiteral("status"); });
    QObject::connect(&fixture.session, &StudySession::errorOccurred, &fixture.session,
                     [&order](const QString &) { order << QStringLiteral("error"); });

    fixture.session.setStatus("ready");
    QCOMPARE(fixture.session.status(), QString("ready"));
    QCOMPARE(statusSpy.count(), 1);
    fixture.session.setStatus("ready");
    QCOMPARE(statusSpy.count(), 1); // identical status is deduped

    fixture.session.reportError("boom");
    QCOMPARE(fixture.session.status(), QString("boom"));
    QCOMPARE(statusSpy.count(), 2);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toString(), QString("boom"));
    QCOMPARE(order, QStringList({"status", "status", "error"}));

    fixture.session.reportError("boom");
    QCOMPARE(statusSpy.count(), 2); // deduped, but errorOccurred is still emitted
    QCOMPARE(errorSpy.count(), 2);
    QCOMPARE(order, QStringList({"status", "status", "error", "error"}));
}

void TestStudySession::rebuildKeepsPendingRepeatInReview() {
    AgainDelayGuard guard;
    qputenv("OMACARDS_AGAIN_DELAY_MS", "2000");
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Repeats", {"Q1", "Q2"}, &error));
    QVERIFY(fixture.session.question().contains("Q1"));

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention)); // Again: Q1 waits, Q2 is shown
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q2"));

    fixture.session.rebuild("Repeats"); // the watcher refresh path keeps the deadline
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q2"));

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention)); // Q2 done, Q1 still waiting
    QVERIFY(fixture.session.waitingForRepeat());
    QVERIFY(!fixture.session.hasCard());
    QTRY_VERIFY_WITH_TIMEOUT(fixture.session.question().contains("Q1"), 5000);
    QVERIFY(!fixture.session.waitingForRepeat());
}

void TestStudySession::switchingToCramClearsPendingRepeat() {
    AgainDelayGuard guard;
    qputenv("OMACARDS_AGAIN_DELAY_MS", "20");
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Switch", {"Q1", "Q2"}, &error));

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention)); // a pending Again in review mode
    fixture.session.setMode("cram");
    QCOMPARE(fixture.session.mode(), StudyMode::Cram);
    fixture.session.rebuild("Switch"); // a cram queue is built from scratch

    // The module suite owns the Cram ordering and endlessness assertions; here
    // we only check that the session forwards the rebuilt state.
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentBox(), 1);
    QVERIFY(!fixture.session.waitingForRepeat());
    fixture.session.flip();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.hasCard());
    QVERIFY(!fixture.session.waitingForRepeat());
}

void TestStudySession::statsTextAndMathEngine() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Stats", {"Q1", "Q2"}, &error));

    QCOMPARE(fixture.session.statsText(), QString("0 reviewed · 0% correct · box 0"));

    fixture.session.setMode("cram");
    fixture.session.rebuild("Stats");
    QCOMPARE(fixture.session.currentBox(), 1);
    fixture.session.flip();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.statsText(), QString("1 reviewed · 100% correct · box 1"));

    fixture.session.flip();
    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QCOMPARE(fixture.session.statsText(), QString("2 reviewed · 50% correct · box 2"));

    const MathRenderer::Engine engine = fixture.session.mathEngine();
    QVERIFY(engine == MathRenderer::Engine::Unicode || engine == MathRenderer::Engine::LatexSvg);
    MathRenderer renderer;
    QCOMPARE(engine, renderer.detect());
}

void TestStudySession::setMathColorsEmitsOnlyOnChange() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Math", {"What is \\(x^2\\)?"}, &error));
    QVERIFY(fixture.session.hasCard());
    QVERIFY(!fixture.session.question().isEmpty());

    QSignalSpy cardSpy(&fixture.session, &StudySession::cardChanged);
    fixture.session.setMathColors("#112233", "#445566");
    QCOMPARE(cardSpy.count(), 1);
    QVERIFY(!fixture.session.question().isEmpty());
    QVERIFY(!fixture.session.answer().isEmpty());

    fixture.session.setMathColors("#112233", "#445566"); // unchanged colors are a no-op
    QCOMPARE(cardSpy.count(), 1);

    fixture.session.setMathColors("#000000", "#000000"); // back to the defaults
    QCOMPARE(cardSpy.count(), 2);
}
