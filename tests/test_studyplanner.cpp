#include <QtTest>

#include <QDate>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include "test_studyplanner.h"
#include "test_support.h"
#include "config.h"
#include "deck.h"
#include "domain.h"
#include "fsrs.h"
#include "store.h"
#include "studyplanner.h"

namespace {

constexpr const char *kCreateCardsSql =
    "CREATE TABLE cards (id TEXT PRIMARY KEY, hash TEXT NOT NULL, stability REAL NOT NULL, "
    "difficulty REAL NOT NULL, due INTEGER NOT NULL, last_review INTEGER NOT NULL, "
    "reps INTEGER NOT NULL, lapses INTEGER NOT NULL)";

Card makeCard(const QString &id, const QString &question, const QString &answer, const QString &deckName) {
    Card card;
    card.id = id;
    card.question = question;
    card.answer = answer;
    card.deck = deckName;
    return card;
}

// Run DDL through a second connection so the Store's sticky error is the only
// signal the planner sees; the Store connection itself stays open.
bool runSql(const QString &database, const QString &connection, const QString &sql) {
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(database);
        if (!db.open())
            return false;
        QSqlQuery query(db);
        if (!query.exec(sql))
            return false;
        query.finish();
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return true;
}

void saveState(Store &store, const Card &card, qint64 due, int reps) {
    FsrsState state;
    state.due = due;
    state.reps = reps;
    state.lapses = 0;
    state.lastReview = reps > 0 ? due - 86400000LL : 0;
    state.stability = reps > 0 ? 5.0 : 0.0;
    state.difficulty = reps > 0 ? 5.0 : 0.0;
    store.save(card.id, DeckIO::contentHash(card), state);
}

void verifyEmptyState(const FsrsState &state) {
    QCOMPARE(state.stability, 0.0);
    QCOMPARE(state.difficulty, 0.0);
    QCOMPARE(state.due, qint64(0));
    QCOMPARE(state.lastReview, qint64(0));
    QCOMPARE(state.reps, 0);
    QCOMPARE(state.lapses, 0);
}

QStringList idsOf(const QVector<Card> &cards) {
    QStringList ids;
    for (const Card &card : cards)
        ids << card.id;
    return ids;
}

// StudyPlanner holds its StoreAdapter by reference. This owns the real adapter
// beside the planner so a test only writes one setup line; the injectable stub
// in test_support.h stays the seam for failed-refresh tests.
struct PlannerAdapterOwner {
    explicit PlannerAdapterOwner(Store &store) : adapter(store) {}
    StorePlannerAdapter adapter;
};

class PlannerWithStore : private PlannerAdapterOwner, public StudyPlanner {
public:
    explicit PlannerWithStore(Store &store) : PlannerAdapterOwner(store), StudyPlanner(PlannerAdapterOwner::adapter) {}
};

} // namespace

void TestStudyPlanner::noDecksIsSafe() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(error.isEmpty());

    PlannerWithStore planner(store);
    planner.configure(5, 3); // setDecks() is deliberately never called

    const StudyPlanner::Availability all = planner.availability();
    QVERIFY(all.due.isEmpty());
    QVERIFY(all.newCards.isEmpty());
    QVERIFY(all.duePerDeck.isEmpty());
    QVERIFY(all.newPerDeck.isEmpty());
    QVERIFY(all.dueIncludingNewPerDeck.isEmpty());
    const StudyPlanner::Availability filtered = planner.availability("Missing");
    QVERIFY(filtered.due.isEmpty());
    QVERIFY(filtered.newCards.isEmpty());

    QCOMPARE(planner.countDue(), 0);
    QCOMPARE(planner.countDue("Missing"), 0);
    QCOMPARE(planner.availableCount(), 0);
    QCOMPARE(planner.newCount(), 0);
    QCOMPARE(planner.deckDue(-1), 0);
    QCOMPARE(planner.deckNew(-1), 0);
    QCOMPARE(planner.deckDue(0), 0);
    QCOMPARE(planner.deckNew(99), 0);
    QVERIFY(planner.dueCards().isEmpty());
    QVERIFY(planner.dueCards("Missing").isEmpty());

    Card card = makeCard("absent", "Q", "A", "Missing");
    verifyEmptyState(planner.stateFor(card));

    QStringList dueIds;
    QStringList newIds;
    planner.reviewIds("Missing", &dueIds, &newIds);
    QVERIFY(dueIds.isEmpty());
    QVERIFY(newIds.isEmpty());

    QVERIFY(planner.deckSummaries(QString()).isEmpty());
    QVERIFY(planner.cardList(QString(), "needle", "new", "due").isEmpty());

    const QVariantMap stats = planner.statsSummary({store.summary(0), 0, 0, 0, 0, 0, Config::kDefaultRetention});
    QCOMPARE(stats.value("accuracy").toInt(), 0);
    QCOMPARE(stats.value("newAvailable").toInt(), 0);
    QCOMPARE(stats.value("streak").toInt(), 0);
    QCOMPARE(stats.value("newCardAllowance").toInt(), 5);
    QCOMPARE(stats.value("dayStartHour").toInt(), 3);
}

void TestStudyPlanner::refreshReadsStatesAndHashes() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    Deck deck;
    deck.name = "Sidecar";
    const Card stored = makeCard("stored", "Stored?", "Yes", "Sidecar");
    const Card stale = makeCard("stale", "Stale?", "No", "Sidecar");
    const Card absent = makeCard("absent", "Absent?", "No", "Sidecar");
    deck.cards << stored << stale << absent;
    const QVector<Deck> decks{deck};

    FsrsState saved;
    saved.reps = 2;
    saved.lapses = 1;
    saved.stability = 4.5;
    saved.difficulty = 6.5;
    saved.lastReview = QDateTime::currentMSecsSinceEpoch() - 2 * 86400000LL;
    saved.due = QDateTime::currentMSecsSinceEpoch() - 86400000LL;
    store.save(stored.id, DeckIO::contentHash(stored), saved);
    store.save(stale.id, QStringLiteral("wrong-hash"), saved); // state with a mismatched hash

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(20, 0);

    QString storeError = QStringLiteral("sentinel");
    QVERIFY(planner.refresh(&storeError));
    // refresh() only writes storeError on failure; a pre-existing value is
    // left untouched when the refresh succeeds.
    QCOMPARE(storeError, QStringLiteral("sentinel"));

    const FsrsState found = planner.stateFor(stored);
    QCOMPARE(found.reps, saved.reps);
    QCOMPARE(found.lapses, saved.lapses);
    QCOMPARE(found.stability, saved.stability);
    QCOMPARE(found.difficulty, saved.difficulty);
    QCOMPARE(found.lastReview, saved.lastReview);
    QCOMPARE(found.due, saved.due);

    Card edited = stored;
    edited.answer = QStringLiteral("Changed"); // same id, different content hash
    verifyEmptyState(planner.stateFor(edited));
    verifyEmptyState(planner.stateFor(absent)); // id has no sidecar row

    QCOMPARE(planner.availability().due.size(), 1);
    QCOMPARE(planner.newCount(), 2);
    QCOMPARE(planner.availableCount(), 3);
}

void TestStudyPlanner::refreshReadFailureCanBeInjectedAtStoreAdapter() {
    TestSupport::PlannerStoreStub stub;
    stub.failReads(QStringLiteral("planner read boom"));
    StudyPlanner planner(stub);

    QString storeError;
    QVERIFY(!planner.refresh(&storeError));
    QCOMPARE(storeError, QStringLiteral("planner read boom"));
}

void TestStudyPlanner::refreshBrokenStoreInvalidatesSnapshot() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    Store store(database);
    QString error;
    QVERIFY(store.open(&error));

    Deck deck;
    deck.name = "Broken";
    const Card card = makeCard("card", "Q", "A", "Broken");
    deck.cards << card;
    const QVector<Deck> decks{deck};
    saveState(store, card, QDateTime::currentMSecsSinceEpoch() - 86400000LL, 2);

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(5, 0);
    QString storeError;
    QVERIFY(planner.refresh(&storeError));
    QVERIFY(storeError.isEmpty());
    QCOMPARE(planner.availableCount(), 1);

    QVERIFY(runSql(database, QStringLiteral("planner-drop-cards"), QStringLiteral("DROP TABLE cards")));

    storeError.clear();
    QVERIFY(!planner.refresh(&storeError));
    QVERIFY(!storeError.isEmpty());
    const QString failedRefreshError = storeError;
    QVERIFY(!store.lastError().isEmpty()); // sticky until clearError()

    const StudyPlanner::Availability available = planner.availability();
    QVERIFY(available.due.isEmpty());
    QVERIFY(available.newCards.isEmpty());
    QCOMPARE(available.duePerDeck.size(), decks.size()); // sized before the validity check
    QCOMPARE(available.newPerDeck.size(), decks.size());
    QCOMPARE(available.dueIncludingNewPerDeck.size(), decks.size());
    QCOMPARE(available.duePerDeck.at(0), 0);
    QCOMPARE(available.newPerDeck.at(0), 0);
    QCOMPARE(planner.countDue(), 0);
    QCOMPARE(planner.availableCount(), 0);
    QCOMPARE(planner.newCount(), 0);
    QCOMPARE(planner.deckDue(0), 0);
    QCOMPARE(planner.deckNew(0), 0);
    QVERIFY(planner.dueCards().isEmpty());
    verifyEmptyState(planner.stateFor(card));

    store.clearError();
    QVERIFY(store.lastError().isEmpty());
    QCOMPARE(planner.newCount(), 0); // clearing the error does not restore the snapshot

    QVERIFY(runSql(database, QStringLiteral("planner-restore-cards"), QString::fromLatin1(kCreateCardsSql)));
    QVERIFY(planner.refresh(&storeError));
    QCOMPARE(storeError, failedRefreshError); // still not cleared on success
    QCOMPARE(planner.newCount(), 1);          // the dropped sidecar row makes the card new again
}

void TestStudyPlanner::configureLimitsAndCounts() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const qint64 base = QDateTime::currentMSecsSinceEpoch() - 3 * 86400000LL;
    const Card newA1 = makeCard("new-a1", "New A1", "a", "Alpha");
    const Card dueB = makeCard("due-b", "Due B", "b", "Alpha");
    const Card newA2 = makeCard("new-a2", "New A2", "a", "Alpha");
    const Card dueA = makeCard("due-a", "Due A", "a", "Alpha");
    const Card future = makeCard("future", "Future", "f", "Alpha");
    const Card newB1 = makeCard("new-b1", "New B1", "b", "Beta");
    const Card dueC = makeCard("due-c", "Due C", "c", "Beta");
    const Card newB2 = makeCard("new-b2", "New B2", "b", "Beta");

    Deck alpha;
    alpha.name = "Alpha";
    alpha.cards << newA1 << dueB << newA2 << dueA << future;
    Deck beta;
    beta.name = "Beta";
    beta.cards << newB1 << dueC << newB2;
    const QVector<Deck> decks{alpha, beta};

    saveState(store, dueC, base, 1);
    saveState(store, dueA, base + 1000, 2);
    saveState(store, dueB, base + 1000, 2); // same due as due-a: id breaks the tie
    saveState(store, future, QDateTime::currentMSecsSinceEpoch() + 3 * 86400000LL, 1);

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(3, 0);
    QVERIFY(planner.refresh(&error));
    QVERIFY(error.isEmpty());

    const StudyPlanner::Availability all = planner.availability();
    QCOMPARE(idsOf(all.due), QStringList({"due-c", "due-a", "due-b"}));
    QCOMPARE(idsOf(all.newCards), QStringList({"new-a1", "new-a2", "new-b1"}));
    QCOMPARE(all.duePerDeck.size(), 2);
    QCOMPARE(all.duePerDeck.at(0), 2);
    QCOMPARE(all.duePerDeck.at(1), 1);
    QCOMPARE(all.newPerDeck.size(), 2);
    QCOMPARE(all.newPerDeck.at(0), 2);
    QCOMPARE(all.newPerDeck.at(1), 1);
    QCOMPARE(all.dueIncludingNewPerDeck.size(), 2);
    QCOMPARE(all.dueIncludingNewPerDeck.at(0), 4); // new cards count as due=0 too
    QCOMPARE(all.dueIncludingNewPerDeck.at(1), 3);

    // countDue() counts new cards and is not clamped by the new budget.
    QCOMPARE(planner.countDue(), 7);
    QCOMPARE(planner.availableCount(), 6);
    QCOMPARE(planner.newCount(), 3);

    QCOMPARE(planner.deckDue(0), 4);
    QCOMPARE(planner.deckDue(1), 2);
    QCOMPARE(planner.deckNew(0), 2);
    QCOMPARE(planner.deckNew(1), 1);
    QCOMPARE(planner.deckDue(-1), 0);
    QCOMPARE(planner.deckNew(-1), 0);
    QCOMPARE(planner.deckDue(2), 0);
    QCOMPARE(planner.deckNew(99), 0);

    // A filter still reports one entry per deck, with only the selected deck
    // filled in. The new-card budget restarts per availability() call, so both
    // Beta new cards are available even though the unfiltered view spent its
    // budget on Alpha's new cards first.
    const StudyPlanner::Availability betaOnly = planner.availability("Beta");
    QCOMPARE(idsOf(betaOnly.due), QStringList({"due-c"}));
    QCOMPARE(idsOf(betaOnly.newCards), QStringList({"new-b1", "new-b2"}));
    QCOMPARE(betaOnly.duePerDeck.size(), 2);
    QCOMPARE(betaOnly.duePerDeck.at(0), 0);
    QCOMPARE(betaOnly.duePerDeck.at(1), 1);
    QCOMPARE(betaOnly.newPerDeck.size(), 2);
    QCOMPARE(betaOnly.newPerDeck.at(0), 0);
    QCOMPARE(betaOnly.newPerDeck.at(1), 2);
    QCOMPARE(betaOnly.dueIncludingNewPerDeck.size(), 2);
    QCOMPARE(betaOnly.dueIncludingNewPerDeck.at(0), 0);
    QCOMPARE(betaOnly.dueIncludingNewPerDeck.at(1), 3);
    QCOMPARE(planner.countDue("Beta"), 3);
    QCOMPARE(planner.availableCount("Beta"), 3);

    QCOMPARE(idsOf(planner.dueCards()), QStringList({"due-c", "due-a", "due-b", "new-a1", "new-a2", "new-b1"}));
}

void TestStudyPlanner::futureReviewsAreNotAvailable() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const Card past = makeCard("past", "Past", "p", "Timing");
    const Card future = makeCard("future", "Future", "f", "Timing");
    const Card zero = makeCard("zero", "Zero", "z", "Timing");
    Deck deck;
    deck.name = "Timing";
    deck.cards << past << future << zero;
    const QVector<Deck> decks{deck};

    const QDate today = QDate::currentDate();
    const qint64 pastDue = QDateTime(today.addDays(-3), QTime(12, 0)).toMSecsSinceEpoch();
    const qint64 futureDue = QDateTime(today.addDays(3), QTime(12, 0)).toMSecsSinceEpoch();
    saveState(store, past, pastDue, 1);
    saveState(store, future, futureDue, 1);
    saveState(store, zero, 0, 1); // a reviewed card pinned to the epoch

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(10, 3);
    QVERIFY(planner.refresh(&error));

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVERIFY(Config::isDueByDay(pastDue, now, 3));
    QVERIFY(!Config::isDueByDay(futureDue, now, 3));
    QVERIFY(Config::isDueByDay(0, now, 3));

    QCOMPARE(idsOf(planner.availability().due), QStringList({"zero", "past"}));
    QCOMPARE(planner.countDue(), 2);
    QCOMPARE(planner.availableCount(), 2);
    QCOMPARE(planner.newCount(), 0);

    const QVariantList cards = planner.cardList("Timing", QString(), "new", "due");
    QCOMPARE(cards.size(), 3);
    for (const QVariant &entry : cards) {
        const QVariantMap map = entry.toMap();
        const QString id = map.value("id").toString();
        const bool due = Config::isDueByDay(map.value("due").toLongLong(), now, 3);
        QCOMPARE(map.value("isDue").toBool(), due);
        if (id == "future")
            QVERIFY(!map.value("isDue").toBool());
        else
            QVERIFY(map.value("isDue").toBool());
    }
}

void TestStudyPlanner::refreshRecomputesNewAllowanceFromReviewHistory() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    Deck deck;
    deck.name = "Allowance";
    const Card first = makeCard("first", "First", "f", "Allowance");
    const Card second = makeCard("second", "Second", "s", "Allowance");
    const Card third = makeCard("third", "Third", "t", "Allowance");
    deck.cards << first << second << third;
    const QVector<Deck> decks{deck};

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(3, 0);
    QVERIFY(planner.refresh(&error));
    QCOMPARE(planner.newCount(), 3);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const FsrsState reviewed = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.recordRating(first.id, 3, now, reviewed, 3, true);
    QVERIFY(planner.refresh(&error));
    QCOMPARE(planner.newCount(), 2);

    store.recordRating(second.id, 3, now, reviewed, 3, true);
    QVERIFY(planner.refresh(&error));
    QCOMPARE(planner.newCount(), 1);

    // A later review of an already rated Card does not spend the allowance.
    store.recordRating(first.id, 3, now + 1000, reviewed, 3, false);
    QVERIFY(planner.refresh(&error));
    QCOMPARE(planner.newCount(), 1);
}

void TestStudyPlanner::updateNewBudgetTracksRecordedRatings() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    Store store(database);
    QString error;
    QVERIFY(store.open(&error));

    Deck deck;
    deck.name = "Budget";
    deck.cards << makeCard("new-a", "A", "a", "Budget") << makeCard("new-b", "B", "b", "Budget")
               << makeCard("new-c", "C", "c", "Budget");
    const QVector<Deck> decks{deck};

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(3, 0);
    QVERIFY(planner.refresh(&error));
    QCOMPARE(planner.newCount(), 3);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.recordRating("new-a", 3, now, state, 1, true);
    store.recordRating("new-a", 3, now + 1, state, 1, true); // DISTINCT: same card twice
    planner.updateNewBudget(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(planner.newCount(), 2);

    store.recordRating("new-b", 3, now, state, 1, true);
    planner.updateNewBudget(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(planner.newCount(), 1);

    // A failed refresh invalidates the snapshot; updateNewBudget must stay a no-op.
    QVERIFY(runSql(database, QStringLiteral("planner-drop-for-budget"), QStringLiteral("DROP TABLE cards")));
    error.clear();
    QVERIFY(!planner.refresh(&error));
    QCOMPARE(planner.newCount(), 0);
    QVERIFY(!store.lastError().isEmpty());

    QVERIFY(runSql(database, QStringLiteral("planner-restore-for-budget"), QString::fromLatin1(kCreateCardsSql)));
    error.clear();
    planner.updateNewBudget(&error);
    QVERIFY(error.isEmpty()); // no store call while the snapshot is invalid
    QVERIFY(!store.lastError().isEmpty());
    QCOMPARE(planner.newCount(), 0);

    QVERIFY(planner.refresh(&error));
    QVERIFY(error.isEmpty());
    QCOMPARE(planner.newCount(), 1); // budget recomputed from the two logged new cards
}

void TestStudyPlanner::reviewIdsFollowAvailabilityOrder() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    Deck deck;
    deck.name = "Order";
    deck.cards << makeCard("f1", "F1", "f", "Order") << makeCard("d2", "D2", "d", "Order")
               << makeCard("f2", "F2", "f", "Order") << makeCard("d1", "D1", "d", "Order")
               << makeCard("f3", "F3", "f", "Order");
    const QVector<Deck> decks{deck};
    const qint64 base = QDateTime::currentMSecsSinceEpoch() - 86400000LL;
    saveState(store, deck.cards.at(1), base, 1);
    saveState(store, deck.cards.at(3), base, 1); // same due, lower id comes first

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(2, 0);
    QVERIFY(planner.refresh(&error));

    QStringList dueIds;
    QStringList newIds;
    planner.reviewIds("Order", &dueIds, &newIds);
    QCOMPARE(dueIds, QStringList({"d1", "d2"}));
    QCOMPARE(newIds, QStringList({"f1", "f2"}));

    dueIds.clear();
    newIds.clear();
    planner.reviewIds(QString(), &dueIds, &newIds);
    QCOMPARE(dueIds, QStringList({"d1", "d2"}));
    QCOMPARE(newIds, QStringList({"f1", "f2"}));

    dueIds.clear();
    newIds.clear();
    planner.reviewIds("NoSuchDeck", &dueIds, &newIds);
    QVERIFY(dueIds.isEmpty());
    QVERIFY(newIds.isEmpty());
}

void TestStudyPlanner::cardListFiltersAndFormats() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    Card alpha = makeCard("alpha", "alpha question", "Beta answer", "List");
    alpha.type = Domain::parseCardType("cloze");
    alpha.tags << "Geometry";
    Card gamma = makeCard("gamma", "gamma question", "delta answer", "List");
    gamma.tags << "blue";
    const Card reviewed = makeCard("reviewed", "Reviewed", "r", "List");
    const Card future = makeCard("future", "Future", "f", "List");
    const Card other = makeCard("other", "ALPHA in other deck", "o", "Other");

    Deck list;
    list.name = "List";
    list.cards << alpha << gamma << reviewed << future;
    Deck otherDeck;
    otherDeck.name = "Other";
    otherDeck.cards << other;
    const QVector<Deck> decks{list, otherDeck};

    const qint64 dueMs = QDateTime::currentMSecsSinceEpoch() - 86400000LL;
    const qint64 futureDue = QDateTime::currentMSecsSinceEpoch() + 3 * 86400000LL;
    saveState(store, reviewed, dueMs, 1);
    saveState(store, future, futureDue, 1);

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(20, 0);
    QVERIFY(planner.refresh(&error));

    const QVariantList cards = planner.cardList("List", QString(), "NEW", "DUE");
    QCOMPARE(cards.size(), 4);
    QStringList expectedKeys{"answer", "deck",     "due",  "dueText", "id",  "isDue",
                             "new",    "question", "reps", "tags",    "type"};
    expectedKeys.sort();
    for (const QVariant &entry : cards) {
        QStringList keys = entry.toMap().keys();
        keys.sort();
        QCOMPARE(keys, expectedKeys);
    }

    QCOMPARE(cards.at(0).toMap().value("dueText").toString(), QString("NEW"));
    QCOMPARE(cards.at(0).toMap().value("type").toString(), QString("cloze"));
    QCOMPARE(cards.at(1).toMap().value("type").toString(), QString("basic"));
    QCOMPARE(cards.at(0).toMap().value("tags").toString(), QString("Geometry"));
    QCOMPARE(cards.at(1).toMap().value("tags").toString(), QString("blue"));
    QCOMPARE(cards.at(2).toMap().value("dueText").toString(), QString("DUE"));
    QVERIFY(cards.at(2).toMap().value("isDue").toBool());
    QCOMPARE(cards.at(2).toMap().value("reps").toInt(), 1);
    QVERIFY(!cards.at(2).toMap().value("new").toBool());
    QCOMPARE(cards.at(3).toMap().value("dueText").toString(),
             QDateTime::fromMSecsSinceEpoch(futureDue).toString("yyyy-MM-dd"));
    QVERIFY(!cards.at(3).toMap().value("isDue").toBool());

    // Filter is trimmed and case-insensitive across Q, A and tags.
    QCOMPARE(planner.cardList("List", "  ALPHA  ", "n", "d").size(), 1);
    QCOMPARE(planner.cardList("List", "beta", "n", "d").first().toMap().value("id").toString(), QString("alpha"));
    QCOMPARE(planner.cardList("List", "GEOMETRY", "n", "d").first().toMap().value("id").toString(), QString("alpha"));
    QCOMPARE(planner.cardList("List", "blue", "n", "d").first().toMap().value("id").toString(), QString("gamma"));
    QCOMPARE(planner.cardList("List", "missing", "n", "d").size(), 0);

    // Without a deck filter the same text also matches in the other deck.
    QCOMPARE(planner.cardList(QString(), "alpha", "n", "d").size(), 2);
    QCOMPARE(planner.cardList("Other", "alpha", "n", "d").size(), 1);
    QCOMPARE(planner.cardList("Other", "alpha", "n", "d").first().toMap().value("deck").toString(), QString("Other"));
}

void TestStudyPlanner::deckSummariesReportCounts() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    Deck alpha;
    alpha.name = "Alpha";
    alpha.description = "First deck";
    alpha.cards << makeCard("a-new1", "A1", "a", "Alpha") << makeCard("a-due", "AD", "a", "Alpha")
                << makeCard("a-new2", "A2", "a", "Alpha");
    Deck beta;
    beta.name = "Beta";
    beta.description = "Second deck";
    beta.cards << makeCard("b-new", "B1", "b", "Beta");
    const QVector<Deck> decks{alpha, beta};
    saveState(store, alpha.cards.at(1), QDateTime::currentMSecsSinceEpoch() - 86400000LL, 1);

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(1, 0);
    QVERIFY(planner.refresh(&error));

    const QVariantList summaries = planner.deckSummaries("Alpha");
    QCOMPARE(summaries.size(), 2);
    const QVariantMap first = summaries.at(0).toMap();
    QCOMPARE(first.value("name").toString(), QString("Alpha"));
    QCOMPARE(first.value("description").toString(), QString("First deck"));
    QCOMPARE(first.value("cards").toInt(), 3);
    QCOMPARE(first.value("due").toInt(), 1);
    QCOMPARE(first.value("new").toInt(), 1);
    QVERIFY(first.value("isCurrent").toBool());

    const QVariantMap second = summaries.at(1).toMap();
    QCOMPARE(second.value("name").toString(), QString("Beta"));
    QCOMPARE(second.value("description").toString(), QString("Second deck"));
    QCOMPARE(second.value("cards").toInt(), 1);
    QCOMPARE(second.value("due").toInt(), 0);
    QCOMPARE(second.value("new").toInt(), 0);
    QVERIFY(!second.value("isCurrent").toBool());

    QStringList expectedKeys{"cards", "description", "due", "isCurrent", "name", "new"};
    expectedKeys.sort();
    QStringList keys = first.keys();
    keys.sort();
    QCOMPARE(keys, expectedKeys);

    const QVariantList selected = planner.deckSummaries("Beta");
    QVERIFY(!selected.at(0).toMap().value("isCurrent").toBool());
    QVERIFY(selected.at(1).toMap().value("isCurrent").toBool());
    const QVariantList none = planner.deckSummaries("Missing");
    QVERIFY(!none.at(0).toMap().value("isCurrent").toBool());
    QVERIFY(!none.at(1).toMap().value("isCurrent").toBool());
}

void TestStudyPlanner::statsSummaryNumericContract() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    Deck deck;
    deck.name = "Stats";
    deck.cards << makeCard("a", "A", "a", "Stats") << makeCard("b", "B", "b", "Stats")
               << makeCard("c", "C", "c", "Stats");
    const QVector<Deck> decks{deck};

    const qint64 todayStart = QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch() + 3600000;
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, todayStart).state;
    store.recordRating("today-good", 3, todayStart, state, 3);
    store.recordRating("today-wrong", 1, todayStart + 1000, state, 1);
    store.recordRating("yesterday", 3, todayStart - 86400000LL, state, 3);
    store.recordRating("logged-new", 3, todayStart + 2000, state, 1, true);

    PlannerWithStore planner(store);
    planner.setDecks(decks);
    planner.configure(2, 0);
    QVERIFY(planner.refresh(&error));

    const QVariantMap summary = planner.statsSummary({store.summary(0), 7, 3, 1, 4, 3, 0.85});
    QStringList keys = summary.keys();
    QStringList expected{"accuracy",
                         "cards",
                         "dayStartHour",
                         "decks",
                         "due",
                         "newAvailable",
                         "newCardAllowance",
                         "newCardsOnStudyDay",
                         "retention",
                         "sessionGood",
                         "sessionReviewed",
                         "streak",
                         "studyDayRatings",
                         "studyDayGood",
                         "totalRatings"};
    keys.sort();
    expected.sort();
    QCOMPARE(keys, expected);

    QCOMPARE(summary.value("totalRatings").toInt(), 4);
    QCOMPARE(summary.value("studyDayRatings").toInt(), 3);
    QCOMPARE(summary.value("studyDayGood").toInt(), 2);
    QCOMPARE(summary.value("streak").toInt(), 2);
    QCOMPARE(summary.value("accuracy").toInt(), 67); // round(2/3 * 100)
    QCOMPARE(summary.value("due").toInt(), 7);
    QCOMPARE(summary.value("cards").toInt(), 3);
    QCOMPARE(summary.value("decks").toInt(), 1);
    QCOMPARE(summary.value("newCardsOnStudyDay").toInt(), 1);
    QCOMPARE(summary.value("newAvailable").toInt(), 1); // 2 per day minus the one logged new card
    QCOMPARE(summary.value("newCardAllowance").toInt(), 2);
    QCOMPARE(summary.value("retention").toInt(), 85);
    QCOMPARE(summary.value("dayStartHour").toInt(), 0);
    QCOMPARE(summary.value("sessionReviewed").toInt(), 4);
    QCOMPARE(summary.value("sessionGood").toInt(), 3);

    // No reviews at all: accuracy falls back to zero instead of dividing by zero.
    QTemporaryDir emptyDir;
    Store emptyStore(emptyDir.filePath("state.db"));
    QVERIFY(emptyStore.open(&error));
    PlannerWithStore emptyPlanner(emptyStore);
    emptyPlanner.configure(2, 0);
    const QVariantMap empty = emptyPlanner.statsSummary({emptyStore.summary(0), 0, 0, 0, 0, 0, 0.9});
    QCOMPARE(empty.value("accuracy").toInt(), 0);
    QCOMPARE(empty.value("streak").toInt(), 0);
    QCOMPARE(empty.value("totalRatings").toInt(), 0);
    QCOMPARE(empty.value("retention").toInt(), 90);
}
