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

} // namespace

void TestStudyPlanner::noDecksIsSafe() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));
    QVERIFY(error.isEmpty());

    StudyPlanner planner(store);
    planner.configure(5, 3); // setDecks() is deliberately never called

    const StudyPlanner::Availability all = planner.availability();
    QVERIFY(all.due.isEmpty());
    QVERIFY(all.fresh.isEmpty());
    QVERIFY(all.duePerDeck.isEmpty());
    QVERIFY(all.freshPerDeck.isEmpty());
    QVERIFY(all.dueIncludingNewPerDeck.isEmpty());
    const StudyPlanner::Availability filtered = planner.availability("Missing");
    QVERIFY(filtered.due.isEmpty());
    QVERIFY(filtered.fresh.isEmpty());

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
    QStringList freshIds;
    planner.reviewIds("Missing", &dueIds, &freshIds);
    QVERIFY(dueIds.isEmpty());
    QVERIFY(freshIds.isEmpty());

    QVERIFY(planner.deckSummaries(QString()).isEmpty());
    QVERIFY(planner.cardList(QString(), "needle", "new", "due").isEmpty());

    const QVariantMap stats = planner.statsSummary(0, 0, 0, 0, 0, Config::kDefaultRetention);
    QCOMPARE(stats.value("accuracy").toInt(), 0);
    QCOMPARE(stats.value("newAvailable").toInt(), 0);
    QCOMPARE(stats.value("streakDays").toInt(), 0);
    QCOMPARE(stats.value("newPerDay").toInt(), 5);
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

    StudyPlanner planner(store);
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

    StudyPlanner planner(store);
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
    QVERIFY(available.fresh.isEmpty());
    QCOMPARE(available.duePerDeck.size(), decks.size()); // sized before the validity check
    QCOMPARE(available.freshPerDeck.size(), decks.size());
    QCOMPARE(available.dueIncludingNewPerDeck.size(), decks.size());
    QCOMPARE(available.duePerDeck.at(0), 0);
    QCOMPARE(available.freshPerDeck.at(0), 0);
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
    QCOMPARE(planner.newCount(), 1);          // the dropped sidecar row makes the card fresh again
}

void TestStudyPlanner::configureLimitsAndCounts() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const qint64 base = QDateTime::currentMSecsSinceEpoch() - 3 * 86400000LL;
    const Card freshA1 = makeCard("fresh-a1", "Fresh A1", "a", "Alpha");
    const Card dueB = makeCard("due-b", "Due B", "b", "Alpha");
    const Card freshA2 = makeCard("fresh-a2", "Fresh A2", "a", "Alpha");
    const Card dueA = makeCard("due-a", "Due A", "a", "Alpha");
    const Card future = makeCard("future", "Future", "f", "Alpha");
    const Card freshB1 = makeCard("fresh-b1", "Fresh B1", "b", "Beta");
    const Card dueC = makeCard("due-c", "Due C", "c", "Beta");
    const Card freshB2 = makeCard("fresh-b2", "Fresh B2", "b", "Beta");

    Deck alpha;
    alpha.name = "Alpha";
    alpha.cards << freshA1 << dueB << freshA2 << dueA << future;
    Deck beta;
    beta.name = "Beta";
    beta.cards << freshB1 << dueC << freshB2;
    const QVector<Deck> decks{alpha, beta};

    saveState(store, dueC, base, 1);
    saveState(store, dueA, base + 1000, 2);
    saveState(store, dueB, base + 1000, 2); // same due as due-a: id breaks the tie
    saveState(store, future, QDateTime::currentMSecsSinceEpoch() + 3 * 86400000LL, 1);

    StudyPlanner planner(store);
    planner.setDecks(decks);
    planner.configure(3, 0);
    QVERIFY(planner.refresh(&error));
    QVERIFY(error.isEmpty());

    const StudyPlanner::Availability all = planner.availability();
    QCOMPARE(idsOf(all.due), QStringList({"due-c", "due-a", "due-b"}));
    QCOMPARE(idsOf(all.fresh), QStringList({"fresh-a1", "fresh-a2", "fresh-b1"}));
    QCOMPARE(all.duePerDeck.size(), 2);
    QCOMPARE(all.duePerDeck.at(0), 2);
    QCOMPARE(all.duePerDeck.at(1), 1);
    QCOMPARE(all.freshPerDeck.size(), 2);
    QCOMPARE(all.freshPerDeck.at(0), 2);
    QCOMPARE(all.freshPerDeck.at(1), 1);
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
    // Beta fresh cards are available even though the unfiltered view spent its
    // budget on Alpha's fresh cards first.
    const StudyPlanner::Availability betaOnly = planner.availability("Beta");
    QCOMPARE(idsOf(betaOnly.due), QStringList({"due-c"}));
    QCOMPARE(idsOf(betaOnly.fresh), QStringList({"fresh-b1", "fresh-b2"}));
    QCOMPARE(betaOnly.duePerDeck.size(), 2);
    QCOMPARE(betaOnly.duePerDeck.at(0), 0);
    QCOMPARE(betaOnly.duePerDeck.at(1), 1);
    QCOMPARE(betaOnly.freshPerDeck.size(), 2);
    QCOMPARE(betaOnly.freshPerDeck.at(0), 0);
    QCOMPARE(betaOnly.freshPerDeck.at(1), 2);
    QCOMPARE(betaOnly.dueIncludingNewPerDeck.size(), 2);
    QCOMPARE(betaOnly.dueIncludingNewPerDeck.at(0), 0);
    QCOMPARE(betaOnly.dueIncludingNewPerDeck.at(1), 3);
    QCOMPARE(planner.countDue("Beta"), 3);
    QCOMPARE(planner.availableCount("Beta"), 3);

    QCOMPARE(idsOf(planner.dueCards()), QStringList({"due-c", "due-a", "due-b", "fresh-a1", "fresh-a2", "fresh-b1"}));
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

    StudyPlanner planner(store);
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

void TestStudyPlanner::recordReviewUpdatesStoreAndSnapshot() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    const Card newA = makeCard("new-a", "New A", "a", "Reviews");
    const Card newB = makeCard("new-b", "New B", "b", "Reviews");
    const Card old = makeCard("old", "Old", "o", "Reviews");
    Deck deck;
    deck.name = "Reviews";
    deck.cards << newA << newB << old;
    const QVector<Deck> decks{deck};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    saveState(store, old, now - 86400000LL, 1);

    StudyPlanner planner(store);
    planner.setDecks(decks);
    planner.configure(2, 0);
    QVERIFY(planner.refresh(&error));
    QCOMPARE(planner.newCount(), 2);

    QVERIFY(planner.recordReview(newA, Rating::Good, Config::kDefaultRetention, now, &error));
    QVERIFY(error.isEmpty());
    const FsrsState written = planner.stateFor(newA);
    QCOMPARE(written.reps, 1);
    QVERIFY(written.due > now);
    QVERIFY(written.stability > 0.0);
    QCOMPARE(store.states().value(newA.id).reps, 1);
    QCOMPARE(store.hashes().value(newA.id), DeckIO::contentHash(newA));
    QCOMPARE(planner.newCount(), 1); // new budget went 2 -> 1 in memory too

    QVERIFY(planner.recordReview(old, Rating::Good, Config::kDefaultRetention, now, &error));
    QCOMPARE(planner.stateFor(old).reps, 2);
    QCOMPARE(store.hashes().value(old.id), DeckIO::contentHash(old));
    QCOMPARE(planner.newCount(), 1); // a review with reps > 0 must not spend the new budget

    QVERIFY(runSql(store.path(), QStringLiteral("planner-drop-for-review"), QStringLiteral("DROP TABLE cards")));
    error.clear();
    QVERIFY(!planner.recordReview(newB, Rating::Good, Config::kDefaultRetention, now, &error));
    QVERIFY(!error.isEmpty());
    verifyEmptyState(planner.stateFor(newB)); // failed write leaves the snapshot alone
    QCOMPARE(planner.stateFor(newA).reps, 1);
}

void TestStudyPlanner::updateNewBudgetTracksLoggedReviews() {
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

    StudyPlanner planner(store);
    planner.setDecks(decks);
    planner.configure(3, 0);
    QVERIFY(planner.refresh(&error));
    QCOMPARE(planner.newCount(), 3);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const FsrsState state = Fsrs::review(Fsrs::initial(), 3, now).state;
    store.logReview("new-a", 3, now, state, 1, true);
    store.logReview("new-a", 3, now + 1, state, 1, true); // DISTINCT: same card twice
    planner.updateNewBudget(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(planner.newCount(), 2);

    store.logReview("new-b", 3, now, state, 1, true);
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

    StudyPlanner planner(store);
    planner.setDecks(decks);
    planner.configure(2, 0);
    QVERIFY(planner.refresh(&error));

    QStringList dueIds;
    QStringList freshIds;
    planner.reviewIds("Order", &dueIds, &freshIds);
    QCOMPARE(dueIds, QStringList({"d1", "d2"}));
    QCOMPARE(freshIds, QStringList({"f1", "f2"}));

    dueIds.clear();
    freshIds.clear();
    planner.reviewIds(QString(), &dueIds, &freshIds);
    QCOMPARE(dueIds, QStringList({"d1", "d2"}));
    QCOMPARE(freshIds, QStringList({"f1", "f2"}));

    dueIds.clear();
    freshIds.clear();
    planner.reviewIds("NoSuchDeck", &dueIds, &freshIds);
    QVERIFY(dueIds.isEmpty());
    QVERIFY(freshIds.isEmpty());
}

void TestStudyPlanner::cardListFiltersAndFormats() {
    QTemporaryDir dir;
    Store store(dir.filePath("state.db"));
    QString error;
    QVERIFY(store.open(&error));

    Card alpha = makeCard("alpha", "alpha question", "Beta answer", "List");
    alpha.type = "basic";
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

    StudyPlanner planner(store);
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

    StudyPlanner planner(store);
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
    QVERIFY(first.value("active").toBool());

    const QVariantMap second = summaries.at(1).toMap();
    QCOMPARE(second.value("name").toString(), QString("Beta"));
    QCOMPARE(second.value("description").toString(), QString("Second deck"));
    QCOMPARE(second.value("cards").toInt(), 1);
    QCOMPARE(second.value("due").toInt(), 0);
    QCOMPARE(second.value("new").toInt(), 0);
    QVERIFY(!second.value("active").toBool());

    QStringList expectedKeys{"active", "cards", "description", "due", "name", "new"};
    expectedKeys.sort();
    QStringList keys = first.keys();
    keys.sort();
    QCOMPARE(keys, expectedKeys);

    const QVariantList selected = planner.deckSummaries("Beta");
    QVERIFY(!selected.at(0).toMap().value("active").toBool());
    QVERIFY(selected.at(1).toMap().value("active").toBool());
    const QVariantList none = planner.deckSummaries("Missing");
    QVERIFY(!none.at(0).toMap().value("active").toBool());
    QVERIFY(!none.at(1).toMap().value("active").toBool());
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
    store.logReview("today-correct", 3, todayStart, state, 3);
    store.logReview("today-wrong", 1, todayStart + 1000, state, 1);
    store.logReview("yesterday", 3, todayStart - 86400000LL, state, 3);
    store.logReview("logged-new", 3, todayStart + 2000, state, 1, true);

    StudyPlanner planner(store);
    planner.setDecks(decks);
    planner.configure(2, 0);
    QVERIFY(planner.refresh(&error));

    const QVariantMap summary = planner.statsSummary(7, 3, 1, 4, 3, 0.85);
    QStringList keys = summary.keys();
    QStringList expected{"accuracy",        "cards",      "dayStartHour", "decks",        "due",
                         "newAvailable",    "newPerDay",  "newToday",     "retention",    "sessionCorrect",
                         "sessionReviewed", "streakDays", "today",        "todayCorrect", "totalReviews"};
    keys.sort();
    expected.sort();
    QCOMPARE(keys, expected);

    QCOMPARE(summary.value("totalReviews").toInt(), 4);
    QCOMPARE(summary.value("today").toInt(), 3);
    QCOMPARE(summary.value("todayCorrect").toInt(), 2);
    QCOMPARE(summary.value("streakDays").toInt(), 2);
    QCOMPARE(summary.value("accuracy").toInt(), 67); // round(2/3 * 100)
    QCOMPARE(summary.value("due").toInt(), 7);
    QCOMPARE(summary.value("cards").toInt(), 3);
    QCOMPARE(summary.value("decks").toInt(), 1);
    QCOMPARE(summary.value("newToday").toInt(), 1);
    QCOMPARE(summary.value("newAvailable").toInt(), 1); // 2 per day minus the one logged new card
    QCOMPARE(summary.value("newPerDay").toInt(), 2);
    QCOMPARE(summary.value("retention").toInt(), 85);
    QCOMPARE(summary.value("dayStartHour").toInt(), 0);
    QCOMPARE(summary.value("sessionReviewed").toInt(), 4);
    QCOMPARE(summary.value("sessionCorrect").toInt(), 3);

    // No reviews at all: accuracy falls back to zero instead of dividing by zero.
    QTemporaryDir emptyDir;
    Store emptyStore(emptyDir.filePath("state.db"));
    QVERIFY(emptyStore.open(&error));
    StudyPlanner emptyPlanner(emptyStore);
    emptyPlanner.configure(2, 0);
    const QVariantMap empty = emptyPlanner.statsSummary(0, 0, 0, 0, 0, 0.9);
    QCOMPARE(empty.value("accuracy").toInt(), 0);
    QCOMPARE(empty.value("streakDays").toInt(), 0);
    QCOMPARE(empty.value("totalReviews").toInt(), 0);
    QCOMPARE(empty.value("retention").toInt(), 90);
}
