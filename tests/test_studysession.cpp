#include <QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantMap>

#include "test_studysession.h"
#include "test_support.h"
#include "config.h"
#include "deck.h"
#include "deckrepository.h"
#include "domain.h"
#include "filehistory.h"
#include "mathrender.h"
#include "ratingrecording.h"
#include "store.h"
#include "studyplanner.h"
#include "studysession.h"

using namespace TestSupport;

namespace {

struct SessionFixture {
    QTemporaryDir dir;
    Store store;
    FileHistory history;
    DeckRepository repo;
    StorePlannerAdapter plannerStore;
    StudyPlanner planner;
    StoreRatingAdapter ratingStore;
    RatingRecording ratings;
    StudySession session;
    QString current;

    SessionFixture()
        : store(dir.filePath("state.db")), repo(dir.filePath("decks"), store, history), plannerStore(store),
          planner(plannerStore), ratingStore(store), ratings(ratingStore), session(repo, planner, ratings) {}
};

// Counts what the session hands to Rating recording; scheduling semantics stay
// in TestRatingRecording.
class RecordingStoreStub : public RatingRecording::StoreAdapter {
public:
    void clearError() override { m_error.clear(); }
    QHash<QString, FsrsState> states() const override {
        if (!m_readError.isEmpty())
            m_error = m_readError;
        return {};
    }
    QHash<QString, QString> hashes() const override { return {}; }
    void save(const QString &, const QString &, const FsrsState &) override {}
    void recordRating(const QString &, int rating, qint64, const FsrsState &, int, bool) override {
        ++m_logCount;
        m_lastRating = rating;
        if (!m_writeError.isEmpty())
            m_error = m_writeError;
    }
    QString lastError() const override { return m_error; }

    void failReads(const QString &error) { m_readError = error; }
    void failWrites(const QString &error) { m_writeError = error; }
    int logCount() const { return m_logCount; }
    int lastRating() const { return m_lastRating; }

private:
    QString m_readError;
    QString m_writeError;
    mutable QString m_error;
    int m_logCount = 0;
    int m_lastRating = 0;
};

// The planner Store stub comes from #18; the recording stub stays local so
// each side can fail independently at the StudySession rating seam.
struct StubSessionFixture {
    QTemporaryDir dir;
    Store store;
    FileHistory history;
    DeckRepository repo;
    TestSupport::PlannerStoreStub plannerStore;
    StudyPlanner planner;
    RecordingStoreStub ratingStore;
    RatingRecording ratings;
    StudySession session;
    QString current;

    StubSessionFixture()
        : store(dir.filePath("state.db")), repo(dir.filePath("decks"), store, history), planner(plannerStore),
          ratings(ratingStore), session(repo, planner, ratings) {}
};

bool prepareSessionParts(DeckRepository &repo, Store &store, StudyPlanner &planner, StudySession &session,
                         QString *loadedDeck, const QString &deckName, const QStringList &questions, QString *error,
                         const QStringList &answers = {}) {
    if (!repo.initialize(error))
        return false;
    if (!store.open(error))
        return false;
    repo.load(loadedDeck);
    if (repo.deckByName("Welcome")) {
        if (!repo.removeDeck("Welcome", loadedDeck, error))
            return false;
    }
    if (!repo.createDeck(deckName, loadedDeck, error))
        return false;
    for (int i = 0; i < questions.size(); ++i) {
        const QString answer = i < answers.size() ? answers.at(i) : "A " + questions.at(i);
        if (!repo.createCard(deckName, loadedDeck, questions.at(i), answer, "basic", QString(), error))
            return false;
    }
    repo.load(loadedDeck);
    planner.setDecks(repo.deckData());
    planner.configure(Config::kDefaultNewPerDay, 0);
    QString storeError;
    if (!planner.refresh(&storeError)) {
        if (error)
            *error = storeError;
        return false;
    }
    session.rebuild(deckName);
    return true;
}

bool prepareSessionWith(SessionFixture &fixture, StudySession &session, const QString &deckName,
                        const QStringList &questions, QString *error) {
    return prepareSessionParts(fixture.repo, fixture.store, fixture.planner, session, &fixture.current, deckName,
                               questions, error);
}

bool prepareSession(SessionFixture &fixture, const QString &deckName, const QStringList &questions, QString *error) {
    return prepareSessionWith(fixture, fixture.session, deckName, questions, error);
}

// StudySession reads the delay in its constructor, so the variable must be set
// before the fixture is built. RAII keeps it safe even if an assertion fails.
const QString kTinySvg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" "
                                        "viewBox=\"0 0 10 5\"><g fill=\"rgb(0%, 0%, 0%)\"/></svg>");

QString toolchainLog(const QString &logPath) {
    QFile log(logPath);
    return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
}

int tectonicRenders(const QString &logPath) {
    // Two workers can interleave their appends, so count the .tex argument
    // rather than a whole command line.
    return toolchainLog(logPath).count(QStringLiteral(".tex"));
}

QString cacheKey(const QString &path) {
    return QFileInfo(path).completeBaseName();
}

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
    QCOMPARE(fixture.session.good(), 1);

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
    QCOMPARE(fixture.session.good(), 0);
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
    QCOMPARE(fixture.session.good(), 0);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(statsSpy.count(), 1);
    QCOMPARE(order, QStringList({"mode", "stats"}));

    fixture.session.rebuild("Modes");
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentLeitnerBox(), 0); // remained in review mode
}

void TestStudySession::setModeCanonicalInputIsNoOp() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Modes", {"Q1", "Q2"}, &error));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.reviewed(), 1);
    QCOMPARE(fixture.session.good(), 1);

    QSignalSpy modeSpy(&fixture.session, &StudySession::modeChanged);
    QSignalSpy statsSpy(&fixture.session, &StudySession::statsChanged);

    fixture.session.setMode("review");
    QCOMPARE(fixture.session.mode(), StudyMode::Review);
    QCOMPARE(fixture.session.reviewed(), 1);
    QCOMPARE(fixture.session.good(), 1);
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
    QCOMPARE(fixture.session.good(), 1);

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
    QCOMPARE(fixture.session.good(), 0);
    QCOMPARE(modeSpy.count(), 1);
    QCOMPARE(statsSpy.count(), 1);
    QCOMPARE(order, QStringList({"mode", "stats"}));

    fixture.session.rebuild("Modes");
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);
    QVERIFY(fixture.session.status().contains("Cram"));
    QCOMPARE(fixture.session.boxDistribution(), QVector<int>({2, 0, 0, 0, 0}));
    QCOMPARE(fixture.session.cramNextInCards(), 1);

    fixture.session.reveal();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.boxDistribution(), QVector<int>({1, 1, 0, 0, 0}));
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);

    fixture.session.reveal();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.boxDistribution(), QVector<int>({0, 2, 0, 0, 0}));
    QCOMPARE(fixture.session.currentLeitnerBox(), 2); // the next Flashcard is in Box 2
    QCOMPARE(fixture.session.cramNextInCards(), 2);

    fixture.session.reveal();
    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QCOMPARE(fixture.session.boxDistribution(), QVector<int>({1, 1, 0, 0, 0}));
    QCOMPARE(fixture.session.currentLeitnerBox(), 2); // the reset card waits; the other Box 2 card is next
    QCOMPARE(fixture.session.cramNextInCards(), 2);
}

void TestStudySession::statsChangedSeesPresentedLeitnerBox() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "SignalOrder", {"Q1", "Q2"}, &error));
    QVERIFY(error.isEmpty());
    fixture.session.setMode("cram");
    fixture.session.rebuild("SignalOrder");
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);

    QVector<int> boxesAtSignal;
    QObject::connect(&fixture.session, &StudySession::statsChanged, &fixture.session,
                     [&fixture, &boxesAtSignal] { boxesAtSignal << fixture.session.currentLeitnerBox(); });

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));

    // A subscriber must never see the just-rated Flashcard's box. Every
    // statsChanged from rate() carries the next Flashcard's Leitner box.
    QVERIFY(!boxesAtSignal.isEmpty());
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);
    for (int box : boxesAtSignal)
        QCOMPARE(box, fixture.session.currentLeitnerBox());
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
    QCOMPARE(fixture.session.good(), 0);
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
    QCOMPARE(fixture.session.good(), 2);
    QCOMPARE(statsSpy.count(), 0);
    QCOMPARE(cardSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
}

void TestStudySession::reviewPassesTypedRatingToRecording() {
    SessionFixture fixture;
    RecordingStoreStub stub;
    RatingRecording recording(stub);
    StudySession session(fixture.repo, fixture.planner, recording);
    QString error;
    QVERIFY(prepareSessionWith(fixture, session, "Ratings", {"Q1", "Q2", "Q3"}, &error));
    QVERIFY(error.isEmpty());

    QVERIFY(session.rate(Rating::Again, Config::kDefaultRetention));
    QCOMPARE(stub.logCount(), 1);
    QCOMPARE(stub.lastRating(), 1); // Again is the typed 1 at the module seam
    QVERIFY(session.question().contains("Q2"));

    QVERIFY(session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(stub.logCount(), 2);
    QCOMPARE(stub.lastRating(), 3); // Good is the typed 3 at the module seam
    QCOMPARE(session.reviewed(), 2);
    QCOMPARE(session.good(), 1);
}

void TestStudySession::reviewFailuresKeepTranslatedText() {
    SessionFixture fixture;
    RecordingStoreStub stub;
    RatingRecording recording(stub);
    StudySession session(fixture.repo, fixture.planner, recording);
    QString error;
    QVERIFY(prepareSessionWith(fixture, session, "Errors", {"Q1", "Q2"}, &error));
    QSignalSpy errorSpy(&session, &StudySession::errorOccurred);

    // presentCard() replaces the transient status after a rating, so the
    // frozen text is asserted on the forwarded error signal.
    stub.failReads(QStringLiteral("read boom"));
    QVERIFY(session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toString(), QString("Cannot read scheduling state: read boom"));

    stub.failReads({});
    stub.failWrites(QStringLiteral("write boom"));
    QVERIFY(session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(errorSpy.count(), 2);
    QCOMPARE(errorSpy.at(1).at(0).toString(), QString("Cannot save review: write boom"));
}

void TestStudySession::successfulReviewRefreshesPlanner() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Refresh", {"Q1", "Q2", "Q3"}, &error));
    const Card card = fixture.repo.deckByName("Refresh")->cards.first();
    QCOMPARE(fixture.planner.stateFor(card).reps, 0);
    QCOMPARE(fixture.planner.newCount(), 3);

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));

    // The module wrote straight to the Store; only a refresh can make the
    // planner's snapshot see the new Scheduling state and its New-card budget.
    QCOMPARE(fixture.planner.stateFor(card).reps, 1);
    QCOMPARE(fixture.planner.newCount(), 2);
    QCOMPARE(fixture.session.dueCount(), 2);
    const QVariantMap row = fixture.planner.cardList("Refresh", "Q1", "new", "due").first().toMap();
    QCOMPARE(row.value("reps").toInt(), 1);
    QVERIFY(!row.value("isDue").toBool());
}

void TestStudySession::goodRatingWithFailedRefreshPinsErrorAndDegradedValues() {
    StubSessionFixture fixture;
    QString error;
    QVERIFY(prepareSessionParts(fixture.repo, fixture.store, fixture.planner, fixture.session, &fixture.current,
                                "RefreshFailure", {"Q1", "Q2"}, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.dueCount() > 0);

    QSignalSpy errorSpy(&fixture.session, &StudySession::errorOccurred);
    const QString rawStoreError = QStringLiteral("refresh read boom");
    fixture.plannerStore.failReads(rawStoreError);

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.ratingStore.logCount(), 1); // the Rating itself recorded
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toString(), QStringLiteral("Cannot read scheduling state: %1").arg(rawStoreError));

    // A failed refresh invalidates the snapshot: the successful write cannot
    // leave a usable plan behind, so Q2 shows the degraded values.
    QCOMPARE(fixture.planner.availableCount(), 0);
    QCOMPARE(fixture.session.dueCount(), 0);
    QVERIFY(fixture.session.status().contains(QStringLiteral("predicted recall 100%")));
}

void TestStudySession::againRatingWithFailedRefreshKeepsPendingRepeat() {
    AgainDelayGuard guard;
    qputenv("OMACARDS_AGAIN_DELAY_MS", "1000");
    StubSessionFixture fixture;
    QString error;
    QVERIFY(prepareSessionParts(fixture.repo, fixture.store, fixture.planner, fixture.session, &fixture.current,
                                "RepeatFailure", {"Q1", "Q2"}, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q1")));

    QSignalSpy errorSpy(&fixture.session, &StudySession::errorOccurred);
    fixture.plannerStore.failReads(QStringLiteral("refresh read boom"));

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QCOMPARE(fixture.ratingStore.logCount(), 1);
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q2")));

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.ratingStore.logCount(), 2);
    QCOMPARE(errorSpy.count(), 2); // both refreshes after recording fail
    QCOMPARE(fixture.planner.availableCount(), 0);
    QCOMPARE(fixture.session.dueCount(), 1); // only the Pending repeat remains
    QVERIFY(fixture.session.waitingForRepeat());
    QVERIFY(!fixture.session.hasCard());

    QTRY_VERIFY_WITH_TIMEOUT(fixture.session.question().contains(QStringLiteral("Q1")), 5000);
    QVERIFY(!fixture.session.waitingForRepeat());
}

void TestStudySession::reviewStatusOmitsSessionPositionBox() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Status", {"Q1"}, &error));

    QVERIFY(fixture.session.status().contains(QStringLiteral("predicted recall")));
    QVERIFY(!fixture.session.status().contains(QStringLiteral("FSRS box")));
}

void TestStudySession::revealAndShowNextToggleAnswer() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Reveal", {"Q1"}, &error));
    QSignalSpy cardSpy(&fixture.session, &StudySession::cardChanged);

    QVERIFY(!fixture.session.showingAnswer());
    fixture.session.reveal();
    QVERIFY(fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 1);

    fixture.session.reveal();
    QVERIFY(!fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 2);

    fixture.session.reveal();
    QVERIFY(fixture.session.showingAnswer());
    fixture.session.showNext(); // only reveals when the answer is showing
    QVERIFY(!fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 4);

    fixture.session.showNext();
    QVERIFY(!fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 4);

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(!fixture.session.hasCard());
    cardSpy.clear();
    fixture.session.reveal();
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
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);
    QVERIFY(!fixture.session.waitingForRepeat());
    fixture.session.reveal();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.hasCard());
    QVERIFY(!fixture.session.waitingForRepeat());
}

void TestStudySession::statsTextAndMathEngine() {
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Stats", {"Q1", "Q2"}, &error));

    QCOMPARE(fixture.session.boxDistribution(), QVector<int>({0, 0, 0, 0, 0}));
    QCOMPARE(fixture.session.statsText(), QString("0 reviewed · 0% Good"));

    fixture.session.setMode("cram");
    fixture.session.rebuild("Stats");
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);
    QCOMPARE(fixture.session.boxDistribution(), QVector<int>({2, 0, 0, 0, 0}));
    fixture.session.reveal();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.boxDistribution(), QVector<int>({1, 1, 0, 0, 0}));
    QCOMPARE(fixture.session.statsText(), QString("1 reviewed · 100% Good"));

    fixture.session.reveal();
    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QCOMPARE(fixture.session.statsText(), QString("2 reviewed · 50% Good"));

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

void TestStudySession::prefetchQueuesTheNextThreeCardsWithoutChangingOrder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache/omacards/math");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(writeFakePdfToolchain(bin, kTinySvg));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", argsLog);
    TestSupport::PathPrependGuard pathGuard(bin);
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Prefetch", {"plain", "\\(x1\\)", "\\(x2\\)", "\\(x3\\)", "\\(x4\\)"}, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(fixture.session.question(), QStringLiteral("plain"));

    // Only the next three cards are warmed; the first card has no math and x4
    // is too far away. Question and Answer share one neutral SVG per expression
    // even though they display at different scales.
    QTRY_VERIFY_WITH_TIMEOUT(tectonicRenders(argsLog) >= 3, 5000);
    QCOMPARE(tectonicRenders(argsLog), 3);
    MathRenderer renderer(cache);
    for (const QString expression : {"x1", "x2", "x3"})
        QVERIFY(QFileInfo::exists(renderer.mathSvgPath(expression)));
    QVERIFY(!QFileInfo::exists(renderer.mathSvgPath("x4")));

    // Warming is a read-only peek: ratings still meet the cards in order.
    for (const QString expression : {"x1", "x2", "x3", "x4"}) {
        QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.session.question().contains(expression) &&
                                     !fixture.session.question().contains(QStringLiteral("<!--omacards-math:")) &&
                                     !fixture.session.question().contains(QStringLiteral("font-family: monospace")),
                                 5000);
    }
}

void TestStudySession::prefetchResultIsIgnoredForCardsAlreadyLeftBehind() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache/omacards/math");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(writeFakePdfToolchain(bin, kTinySvg));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", argsLog);
    TestSupport::PathPrependGuard pathGuard(bin);
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Stale", {"\\(x^2\\)", "plain"}, &error));
    MathRenderer renderer(cache);
    const QString key = cacheKey(renderer.mathSvgPath("x^2"));
    QVERIFY(fixture.session.question().contains(QStringLiteral("<!--omacards-math:%1-->").arg(key)));

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(fixture.session.question(), QStringLiteral("plain"));
    const QString questionBefore = fixture.session.question();

    fixture.session.applyMathRenderResult(key, QStringLiteral("<img src='stale' width='1' height='1'>"));
    QCOMPARE(fixture.session.question(), questionBefore);

    // The worker's real result is equally stale once the card has changed.
    QCoreApplication::processEvents();
    QCOMPARE(fixture.session.question(), questionBefore);
}

void TestStudySession::themeChangeRecolorsFromCacheWithoutPrefetch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache/omacards/math");
    const QString argsLog = dir.filePath("args.log");
    QVERIFY(writeFakePdfToolchain(bin, kTinySvg));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", argsLog);
    TestSupport::PathPrependGuard pathGuard(bin);
    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSession(fixture, "Theme", {"\\(x^2\\)", "plain", "\\(y^2\\)"}, &error));
    QTRY_VERIFY_WITH_TIMEOUT(tectonicRenders(argsLog) >= 2, 5000);

    MathRenderer renderer(cache);
    QVERIFY(QFileInfo::exists(renderer.mathSvgPath("x^2")));
    QVERIFY(QFileInfo::exists(renderer.mathSvgPath("y^2")));
    const QStringList filesBefore = QDir(cache).entryList({"*.svg"});
    const QString questionBefore = fixture.session.question();

    QSignalSpy cardSpy(&fixture.session, &StudySession::cardChanged);
    fixture.session.setMathColors("#112233", "#445566");
    QCOMPARE(cardSpy.count(), 1);

    // New inks only change the data URIs. No upcoming card is re-rendered and
    // no cache file is added, so a theme switch stays instant.
    QCOMPARE(tectonicRenders(argsLog), 2);
    QCOMPARE(QDir(cache).entryList({"*.svg"}), filesBefore);
    const QString questionAfter = fixture.session.question();
    QVERIFY(questionAfter != questionBefore);
    QVERIFY(questionAfter.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    QVERIFY(!questionAfter.contains(QStringLiteral("<!--omacards-math:")));
}

void TestStudySession::themeChangeDuringRenderUsesNewInk() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache/omacards/math");
    const QString argsLog = dir.filePath("args.log");
    const QString started = dir.filePath("started");
    const QString release = dir.filePath("release");
    // pdftocairo announces the in-flight render and waits for the test, so the
    // theme switch cannot race past the worker into an already cached SVG.
    const QString gate = QStringLiteral("printf 'started' > \"$OMACARDS_TEST_STARTED\"\n"
                                        "i=0\n"
                                        "while [ ! -e \"$OMACARDS_TEST_RELEASE\" ] && [ $i -lt 200 ]; do\n"
                                        "    sleep 0.05\n"
                                        "    i=$((i+1))\n"
                                        "done\n");
    QVERIFY(writeFakePdfToolchain(bin, kTinySvg, gate));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", argsLog);
    TestSupport::ScopedEnv startedEnv("OMACARDS_TEST_STARTED", started);
    TestSupport::ScopedEnv releaseEnv("OMACARDS_TEST_RELEASE", release);
    TestSupport::PathPrependGuard pathGuard(bin);

    SessionFixture fixture;
    QString error;
    QVERIFY(prepareSessionParts(fixture.repo, fixture.store, fixture.planner, fixture.session, &fixture.current,
                                "InFlight", {"\\(x^2\\)"}, &error, {"plain"}));
    QVERIFY(error.isEmpty());

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 5000);
    QVERIFY(fixture.session.question().contains(QStringLiteral("<!--omacards-math:")));
    QCOMPARE(tectonicRenders(argsLog), 1);

    fixture.session.setMathColors("#112233", "#445566");
    QVERIFY(writeFile(release, ""));

    QTRY_VERIFY_WITH_TIMEOUT(!fixture.session.question().contains(QStringLiteral("<!--omacards-math:")), 5000);
    const QString question = fixture.session.question();
    QVERIFY(question.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    const QString svg = svgFromMathHtml(question);
    QVERIFY(svg.contains(QStringLiteral("fill=\"#112233\"")));
    QVERIFY(!svg.contains(QStringLiteral("fill=\"#000000\"")));
    // Switching ink reused the in-flight neutral render; it never started a
    // second toolchain run or changed the cached SVG.
    QCOMPARE(tectonicRenders(argsLog), 1);
}
