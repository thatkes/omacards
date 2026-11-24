#include <QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantMap>
#include <type_traits>

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

template <typename Adapter> Adapter makeStoreAdapter(Store &store) {
    if constexpr (std::is_constructible_v<Adapter, Store &>)
        return Adapter(store);
    else
        return Adapter{};
}

// One fixture owns the whole session graph and its setup. The adapter types
// vary: the stub fixture fails each seam independently at the rating path.
template <typename PlannerStore, typename RatingStore> struct SessionFixtureBase {
    QTemporaryDir dir;
    Store store;
    FileHistory history;
    DeckRepository repo;
    PlannerStore plannerStore;
    StudyPlanner planner;
    RatingStore ratingStore;
    RatingRecording ratings;
    StudySession session;
    QString current;

    SessionFixtureBase()
        : store(dir.filePath("state.db")), repo(dir.filePath("decks"), store, history),
          plannerStore(makeStoreAdapter<PlannerStore>(store)), planner(plannerStore),
          ratingStore(makeStoreAdapter<RatingStore>(store)), ratings(ratingStore), session(repo, planner, ratings) {}

    bool prepare(const QString &deckName, const QStringList &questions, QString *error, const QStringList &answers = {},
                 StudySession *target = nullptr) {
        if (!repo.initialize(error))
            return false;
        if (!store.open(error))
            return false;
        repo.load(&current);
        if (repo.deckByName("Welcome")) {
            if (!repo.removeDeck("Welcome", &current, error))
                return false;
        }
        if (!repo.createDeck(deckName, &current, error))
            return false;
        for (int i = 0; i < questions.size(); ++i) {
            const QString answer = i < answers.size() ? answers.at(i) : "A " + questions.at(i);
            if (!repo.createCard(deckName, &current, questions.at(i), answer, "basic", QString(), error))
                return false;
        }
        repo.load(&current);
        planner.setDecks(repo.deckData());
        planner.configure(Config::kDefaultNewPerDay, 0);
        QString storeError;
        if (!planner.refresh(&storeError)) {
            if (error)
                *error = storeError;
            return false;
        }
        StudySession &study = target ? *target : session;
        study.rebuild(deckName);
        return true;
    }
};

using SessionFixture = SessionFixtureBase<StorePlannerAdapter, StoreRatingAdapter>;
using StubSessionFixture = SessionFixtureBase<PlannerStoreStub, RatingStoreStub>;

// StudySession reads the delay in its constructor, so the variable must be set
// before the fixture is built. RAII keeps it safe even if an assertion fails.
const QString kTinySvg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" "
                                        "viewBox=\"0 0 10 5\"><g fill=\"rgb(0%, 0%, 0%)\"/></svg>");

QString cacheKey(const QString &path) {
    return QFileInfo(path).completeBaseName();
}

} // namespace

void TestStudySession::setModeNormalizesInvalidInput() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Modes", {"Q1", "Q2"}, &error));
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

    // The invalid mode still settles a Review Flashcard without a paired
    // rebuild from the caller.
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q"));
}

void TestStudySession::setModeUppercaseCramNormalizesToReview() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Modes", {"Q1", "Q2"}, &error));
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

    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentLeitnerBox(), 0); // remained in review mode
}

void TestStudySession::setModeCanonicalInputIsNoOp() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Modes", {"Q1", "Q2"}, &error));
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
    QVERIFY(fixture.prepare("Modes", {"Q1", "Q2"}, &error));
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

void TestStudySession::modeSwitchStopsTimerAndClearsPendingRepeat() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "10");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("AtomicMode", {"Q1", "Q2"}, &error));
    QVERIFY(error.isEmpty());

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QVERIFY(fixture.session.question().contains("Q2"));
    QCOMPARE(fixture.session.dueCount(), 2); // Q2 available, Q1 Pending

    fixture.session.setMode("cram"); // no paired rebuild: the switch is atomic
    QCOMPARE(fixture.session.mode(), StudyMode::Cram);
    QCOMPARE(fixture.session.reviewed(), 0);
    QCOMPARE(fixture.session.good(), 0);
    QVERIFY(!fixture.session.status().contains(QStringLiteral("moment")));
    QCOMPARE(fixture.session.dueCount(), 1); // the Pending repeat did not follow
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.status().contains("Cram"));
}

void TestStudySession::injectedCramOrderPresentsExactFlashcards() {
    SessionFixture fixture;
    StudySession ordered(fixture.repo, fixture.planner, fixture.ratings,
                         [](const QVector<Card> &cards) { return cards; });
    QString error;
    QVERIFY(fixture.prepare("CramOrder", {"Q1", "Q2", "Q3"}, &error, {}, &ordered));
    QVERIFY(error.isEmpty());
    const QVector<Card> cards = fixture.repo.deckByName("CramOrder")->cards;
    QCOMPARE(cards.size(), 3);

    ordered.setMode("cram"); // one operation, no rebuild afterwards
    QCOMPARE(ordered.mode(), StudyMode::Cram);
    QCOMPARE(ordered.currentCardId(), cards.at(0).id);
    QVERIFY(ordered.status().contains("Cram"));

    for (int i = 1; i < cards.size(); ++i) {
        ordered.reveal();
        QVERIFY(ordered.rate(Rating::Good, Config::kDefaultRetention));
        QCOMPARE(ordered.currentCardId(), cards.at(i).id);
    }
    // Cram is endless: the first Flashcard returns once the others have gone round.
    ordered.reveal();
    QVERIFY(ordered.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(ordered.currentCardId(), cards.at(0).id);
}

void TestStudySession::statsChangedSeesPresentedLeitnerBox() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("SignalOrder", {"Q1", "Q2"}, &error));
    QVERIFY(error.isEmpty());
    fixture.session.setMode("cram");
    fixture.session.rebuild("SignalOrder");
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);

    QVector<int> boxesAtSignal;
    QObject::connect(&fixture.session, &StudySession::statsChanged, &fixture.session,
                     [&fixture, &boxesAtSignal] { boxesAtSignal << fixture.session.currentLeitnerBox(); });

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));

    // A subscriber must never see the just-rated Flashcard's box. The one
    // statsChanged from rate() carries the next Flashcard's settled box.
    QCOMPARE(boxesAtSignal.size(), 1);
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);
    for (int box : boxesAtSignal)
        QCOMPARE(box, fixture.session.currentLeitnerBox());
}

void TestStudySession::endRepeatExitEmitsOneSettledStatsNotification() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "2000");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Waiting", {"Q1", "Q2"}, &error));
    QVERIFY(error.isEmpty());

    QVector<int> reviewedAtSignal;
    QVector<int> goodAtSignal;
    QStringList statusAtSignal;
    QObject::connect(&fixture.session, &StudySession::statsChanged, &fixture.session,
                     [&fixture, &reviewedAtSignal, &goodAtSignal, &statusAtSignal] {
                         reviewedAtSignal << fixture.session.reviewed();
                         goodAtSignal << fixture.session.good();
                         statusAtSignal << fixture.session.status();
                     });

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention)); // Q1 waits; Q2 is shown
    QCOMPARE(reviewedAtSignal.size(), 1);
    QCOMPARE(reviewedAtSignal.last(), 1);
    QCOMPARE(goodAtSignal.last(), 0);
    QVERIFY(fixture.session.hasCard());

    reviewedAtSignal.clear();
    goodAtSignal.clear();
    statusAtSignal.clear();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention)); // Q2 done; Q1 returns at once
    QCOMPARE(reviewedAtSignal.size(), 1);
    QCOMPARE(reviewedAtSignal.last(), 2);
    QCOMPARE(goodAtSignal.last(), 1);
    QCOMPARE(statusAtSignal.last(), fixture.session.status());
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q1")));
    QVERIFY(!fixture.session.status().contains(QStringLiteral("moment")));
}

void TestStudySession::endOfSessionRepeatIsPresentedWithoutWaiting() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "2000");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("EndRepeat", {"Q1"}, &error));
    QVERIFY(error.isEmpty());
    const QString firstCard = fixture.session.currentCardId();
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q1")));

    QSignalSpy cardSpy(&fixture.session, &StudySession::cardChanged);
    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentCardId(), firstCard);
    QVERIFY(!fixture.session.showingAnswer());
    QCOMPARE(fixture.session.dueCount(), 1);
    QVERIFY(fixture.session.status().contains(QStringLiteral("predicted recall")));
    QVERIFY(!fixture.session.status().contains(QStringLiteral("wait")));
    QCOMPARE(cardSpy.count(), 1);
}

void TestStudySession::repeatTimerDoesNotSwapTheUnratedFlashcard() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "150");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Timer", {"Q1", "Q2"}, &error));
    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q2")));
    const QString onScreen = fixture.session.currentCardId();
    fixture.session.reveal();

    QSignalSpy cardSpy(&fixture.session, &StudySession::cardChanged);
    // Q1's Pending deadline must not displace the unrated Q2 before it passes.
    QTest::qWait(75);
    QCOMPARE(fixture.session.currentCardId(), onScreen);
    QVERIFY(fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 0);

    // The expired timer still leaves the card in place; the next transition is
    // driven by rating Q2, which presents the Pending Q1.
    QTest::qWait(125);
    QCOMPARE(fixture.session.currentCardId(), onScreen);
    QVERIFY(fixture.session.showingAnswer());
    QCOMPARE(cardSpy.count(), 0);
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q1")));
    QCOMPARE(cardSpy.count(), 1);
}

void TestStudySession::oldestPendingDeadlineIsPresentedFirst() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "5000");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Deadlines", {"Q1", "Q2", "Q3"}, &error));

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention)); // Q1 Pending, Q2 shown
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q2")));
    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention)); // Q2 Pending, Q3 shown
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q3")));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q1"))); // older deadline first

    // Rating that repeat Again gives it a fresh deadline, so Q2 takes its turn.
    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q2")));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q1")));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(!fixture.session.hasCard());
    QVERIFY(fixture.session.status().contains(QStringLiteral("All caught up")));
}

void TestStudySession::repeatedAgainLoopsUntilGoodThenFinishes() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "5000");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Loop", {"Q1"}, &error));

    for (int attempt = 0; attempt < 4; ++attempt) {
        QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention));
        QVERIFY(fixture.session.hasCard());
        QVERIFY(fixture.session.question().contains(QStringLiteral("Q1")));
        QCOMPARE(fixture.session.dueCount(), 1);
    }
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(!fixture.session.hasCard());
    QVERIFY(fixture.session.status().contains(QStringLiteral("All caught up")));
    QCOMPARE(fixture.session.dueCount(), 0);
}

void TestStudySession::finishedExitEmitsOneSettledStatsNotification() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Finished", {"Q1"}, &error));
    QVERIFY(error.isEmpty());

    QVector<int> reviewedAtSignal;
    QVector<int> goodAtSignal;
    QStringList statusAtSignal;
    QObject::connect(&fixture.session, &StudySession::statsChanged, &fixture.session,
                     [&fixture, &reviewedAtSignal, &goodAtSignal, &statusAtSignal] {
                         reviewedAtSignal << fixture.session.reviewed();
                         goodAtSignal << fixture.session.good();
                         statusAtSignal << fixture.session.status();
                     });

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QCOMPARE(reviewedAtSignal.size(), 1);
    QCOMPARE(reviewedAtSignal.last(), 1);
    QCOMPARE(goodAtSignal.last(), 1);
    QCOMPARE(statusAtSignal.last(), fixture.session.status());
    QVERIFY(fixture.session.status().contains(QStringLiteral("All caught up")));
    QVERIFY(!fixture.session.hasCard());
}

void TestStudySession::invalidRatingsAreNoOps() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Ratings", {"Q1", "Q2"}, &error));

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
    RatingStoreStub stub;
    RatingRecording recording(stub);
    StudySession session(fixture.repo, fixture.planner, recording);
    QString error;
    QVERIFY(fixture.prepare("Ratings", {"Q1", "Q2", "Q3"}, &error, {}, &session));
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
    RatingStoreStub stub;
    RatingRecording recording(stub);
    StudySession session(fixture.repo, fixture.planner, recording);
    QString error;
    QVERIFY(fixture.prepare("Errors", {"Q1", "Q2"}, &error, {}, &session));
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
    QVERIFY(fixture.prepare("Refresh", {"Q1", "Q2", "Q3"}, &error));
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
    QVERIFY(fixture.prepare("RefreshFailure", {"Q1", "Q2"}, &error));
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
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "10");
    StubSessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("RepeatFailure", {"Q1", "Q2"}, &error));
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
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains(QStringLiteral("Q1")));
    QVERIFY(!fixture.session.status().contains(QStringLiteral("moment")));
}

void TestStudySession::reviewStatusOmitsSessionPositionBox() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Status", {"Q1"}, &error));

    QVERIFY(fixture.session.status().contains(QStringLiteral("predicted recall")));
    QVERIFY(!fixture.session.status().contains(QStringLiteral("FSRS box")));
}

void TestStudySession::revealAndShowNextToggleAnswer() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Reveal", {"Q1"}, &error));
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
    QVERIFY(fixture.prepare("Status", {"Q1"}, &error));

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
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "10");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Repeats", {"Q1", "Q2"}, &error));
    QVERIFY(fixture.session.question().contains("Q1"));

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention)); // Again: Q1 waits, Q2 is shown
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q2"));

    fixture.session.rebuild("Repeats"); // the watcher refresh path keeps the deadline
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q2"));

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention)); // Q2 done, Q1 returns at once
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q1"));
    QVERIFY(!fixture.session.status().contains(QStringLiteral("moment")));
}

void TestStudySession::rebuildKeepsUnratedAdditionalNewCards() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Study", {"Q1", "Q2", "Q3", "Q4"}, &error));
    fixture.planner.configure(1, 0);
    QString storeError;
    fixture.planner.updateNewBudget(&storeError);
    fixture.session.rebuild("Study");
    QVERIFY(fixture.session.question().contains("Q1"));

    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(!fixture.session.hasCard());
    QVERIFY(fixture.session.reviewWhenEmpty(2));
    QVERIFY(fixture.session.question().contains("Q2"));

    // A watcher refresh rebuilds from the Store; the session-only cards must
    // survive in their original order until they are rated.
    fixture.session.rebuild("Study");
    QVERIFY(fixture.session.hasCard());
    QVERIFY(fixture.session.question().contains("Q2"));
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.question().contains("Q3"));
    fixture.session.rebuild("Study");
    QVERIFY(fixture.session.question().contains("Q3"));

    fixture.session.startFresh("Study");
    QVERIFY(!fixture.session.hasCard());
}

void TestStudySession::switchingToCramClearsPendingRepeat() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS", "20");
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Switch", {"Q1", "Q2"}, &error));

    QVERIFY(fixture.session.rate(Rating::Again, Config::kDefaultRetention)); // a pending Again in review mode
    fixture.session.setMode("cram");
    QCOMPARE(fixture.session.mode(), StudyMode::Cram);
    fixture.session.rebuild("Switch"); // a cram queue is built from scratch

    // The module suite owns the Cram ordering and endlessness assertions; here
    // we only check that the session forwards the rebuilt state.
    QVERIFY(fixture.session.hasCard());
    QCOMPARE(fixture.session.currentLeitnerBox(), 1);
    QVERIFY(!fixture.session.status().contains(QStringLiteral("moment")));
    fixture.session.reveal();
    QVERIFY(fixture.session.rate(Rating::Good, Config::kDefaultRetention));
    QVERIFY(fixture.session.hasCard());
    QVERIFY(!fixture.session.status().contains(QStringLiteral("moment")));
}

void TestStudySession::statsTextAndMathEngine() {
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Stats", {"Q1", "Q2"}, &error));

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
    QVERIFY(fixture.prepare("Math", {"What is \\(x^2\\)?"}, &error));
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
    TestSupport::PathGuard pathGuard(bin);
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Prefetch", {"plain", "\\(x1\\)", "\\(x2\\)", "\\(x3\\)", "\\(x4\\)"}, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(fixture.session.question(), QStringLiteral("plain"));

    // Only the next three cards are warmed; the first card has no math and x4
    // is too far away. Question and Answer share one neutral SVG per expression
    // even though they display at different scales.
    QTRY_VERIFY_WITH_TIMEOUT(toolchainRenders(argsLog) >= 3, 5000);
    QCOMPARE(toolchainRenders(argsLog), 3);
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
    TestSupport::PathGuard pathGuard(bin);
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Stale", {"\\(x^2\\)", "plain"}, &error));
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
    TestSupport::PathGuard pathGuard(bin);
    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("Theme", {"\\(x^2\\)", "plain", "\\(y^2\\)"}, &error));
    QTRY_VERIFY_WITH_TIMEOUT(toolchainRenders(argsLog) >= 2, 5000);

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
    QCOMPARE(toolchainRenders(argsLog), 2);
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
    TestSupport::PathGuard pathGuard(bin);

    SessionFixture fixture;
    QString error;
    QVERIFY(fixture.prepare("InFlight", {"\\(x^2\\)"}, &error, {"plain"}));
    QVERIFY(error.isEmpty());

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 5000);
    QVERIFY(fixture.session.question().contains(QStringLiteral("<!--omacards-math:")));
    QCOMPARE(toolchainRenders(argsLog), 1);

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
    QCOMPARE(toolchainRenders(argsLog), 1);
}

void TestStudySession::themeChangeAfterSvgIsCachedUsesNewInk() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache/omacards/math");
    const QString argsLog = dir.filePath("args.log");
    const QString cached = dir.filePath("cached");
    const QString release = dir.filePath("release");
    // The fake converter writes the neutral SVG, announces it, then waits.
    // Both theme switches therefore take the cached path while the worker still
    // holds the active request, just before it posts its queued completion.
    const QString gate = QStringLiteral("printf '%s\\n' '") + kTinySvg +
                         QStringLiteral("' > \"$svg\"\n"
                                        "printf 'cached' > \"$OMACARDS_TEST_CACHED\"\n"
                                        "i=0\n"
                                        "while [ ! -e \"$OMACARDS_TEST_RELEASE\" ] && [ $i -lt 200 ]; do\n"
                                        "    sleep 0.05\n"
                                        "    i=$((i+1))\n"
                                        "done\n");
    QVERIFY(writeFakePdfToolchain(bin, kTinySvg, gate));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", argsLog);
    TestSupport::ScopedEnv cachedEnv("OMACARDS_TEST_CACHED", cached);
    TestSupport::ScopedEnv releaseEnv("OMACARDS_TEST_RELEASE", release);
    TestSupport::PathGuard pathGuard(bin);

    MathRenderer renderer(cache);
    QSignalSpy rendered(&renderer, &MathRenderer::mathRendered);
    const QString expression = QStringLiteral("\\(x^2\\)");
    const QString key = cacheKey(renderer.mathSvgPath("x^2"));
    const QString placeholder = renderer.renderWithPlaceholders(expression, "#000000", MathScale::kQuestion);
    QVERIFY(placeholder.contains(QStringLiteral("<!--omacards-math:%1-->").arg(key)));

    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(cached), 5000);
    QCOMPARE(toolchainRenders(argsLog), 1);

    renderer.clearPendingMath();
    const QString first = renderer.renderWithPlaceholders(expression, "#112233", MathScale::kQuestion);
    renderer.clearPendingMath();
    const QString live = renderer.renderWithPlaceholders(expression, "#abcdef", MathScale::kQuestion);
    QVERIFY(!live.contains(QStringLiteral("<!--omacards-math:")));
    QVERIFY(svgFromMathHtml(live).contains(QStringLiteral("fill=\"#abcdef\"")));
    QVERIFY(!svgFromMathHtml(live).contains(QStringLiteral("fill=\"#112233\"")));
    // The intermediate ink is not left on screen either.
    QVERIFY(!svgFromMathHtml(first).contains(QStringLiteral("fill=\"#000000\"")));

    QVERIFY(writeFile(release, ""));
    QTRY_VERIFY_WITH_TIMEOUT(rendered.count() >= 1, 5000);
    QCOMPARE(rendered.count(), 1);
    const QString emitted = rendered.at(0).at(1).toString();
    QVERIFY(emitted.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    const QString emittedSvg = svgFromMathHtml(emitted);
    QVERIFY(!emittedSvg.contains(QStringLiteral("fill=\"#112233\"")));
    QVERIFY(!emittedSvg.contains(QStringLiteral("fill=\"#000000\"")));
    QVERIFY(emittedSvg.contains(QStringLiteral("fill=\"#abcdef\"")));
    // Recoloring from the neutral cache never starts a second toolchain run.
    QCOMPARE(toolchainRenders(argsLog), 1);
}
