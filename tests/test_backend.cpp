#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include "test_backend.h"
#include "test_support.h"
#include "backend.h"
#include "config.h"
#include "deck.h"
#include "fsrs.h"
#include "store.h"
#include "theme.h"

using namespace TestSupport;

void TestBackend::defaultPathsIgnoreOrganizationName() {
    const QByteArray oldDecks = qgetenv("OMACARDS_DECKS_DIR");
    const QByteArray oldState = qgetenv("OMACARDS_STATE_DB");
    qunsetenv("OMACARDS_DECKS_DIR");
    qunsetenv("OMACARDS_STATE_DB");

    const QString oldOrg = QCoreApplication::organizationName();
    auto ownedWithoutOrg = Backend::fromEnvironment();
    Backend &withoutOrg = *ownedWithoutOrg;
    const QString first = withoutOrg.decksDirectory();
    QCoreApplication::setOrganizationName("omarchy");
    auto ownedWithOrg = Backend::fromEnvironment();
    Backend &withOrg = *ownedWithOrg;
    const QString second = withOrg.decksDirectory();
    QCoreApplication::setOrganizationName(oldOrg);

    QCOMPARE(first, second);
    QVERIFY(!first.contains(QStringLiteral("omarchy")));

    if (!oldDecks.isNull())
        qputenv("OMACARDS_DECKS_DIR", oldDecks);
    if (!oldState.isNull())
        qputenv("OMACARDS_STATE_DB", oldState);
}

void TestBackend::backendCreatesCardsAndCramsForever() {
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

void TestBackend::qmlCanRateCards() {
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
    )QML",
                      QUrl("qrc:/rate-test.qml"));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QObject *object = component.create();
    QVERIFY2(object, qPrintable(component.errorString()));
    delete object;

    QCOMPARE(backend.reviewed(), 1);
    QCOMPARE(backend.correct(), 1);
}

void TestBackend::createCardRejectsInvalidType() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Test", &error));
    QVERIFY(!backend.createCard("Test", "Q", "A", "nonsense", {}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(backend.allDecks().first().cards.isEmpty());
}

void TestBackend::backendEditsCardsWithUndo() {
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

void TestBackend::backendDeletesDeckAndUndoes() {
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

void TestBackend::backendFiltersCardList() {
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

void TestBackend::backendImportsAnkiClozeAndMedia() {
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
        QVERIFY(query.exec(
            QString("INSERT INTO notes VALUES ('Basic &amp; <b>bold</b>%1back with <img src=\"0\">', 1, ' tag1 tag2 ')")
                .arg(sep)));
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
        if (deck.name == "Anki")
            cards = deck.cards;
    QCOMPARE(cards.size(), 2);
    const Card *basic = nullptr;
    const Card *cloze = nullptr;
    for (const Card &card : cards) {
        if (card.type == "cloze")
            cloze = &card;
        else
            basic = &card;
    }
    QVERIFY(basic && cloze);
    QCOMPARE(basic->question, QString("Basic & bold"));
    QVERIFY(basic->answer.contains("<img src=\"file://"));
    QCOMPARE(basic->tags, QStringList({"tag1", "tag2"}));
    QCOMPARE(cloze->question, QString("The {{c1::cat}} sleeps"));
    QVERIFY(QFile::exists(decks + "/media/Anki/image.png"));
}

void TestBackend::backendAppliesDayStartToDueCards() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    {
        Backend backend(dir.path(), database);
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Timing", &error));
        QVERIFY(backend.createCard("Timing", "Late due", "A", "basic", {}, &error));
        backend.selectDeck("Timing");

        const Card *target = nullptr;
        for (const Deck &deck : backend.allDecks())
            for (const Card &card : deck.cards)
                if (card.question == "Late due")
                    target = &card;
        QVERIFY(target);

        // Due late today: available all today under a midnight rollover.
        FsrsState state;
        state.due = QDateTime(QDate::currentDate(), QTime(22, 0)).toMSecsSinceEpoch();
        state.lastReview = QDateTime(QDate::currentDate().addDays(-1), QTime(22, 0)).toMSecsSinceEpoch();
        state.reps = 1;
        Store store(database);
        QVERIFY(store.open(&error));
        store.save(target->id, DeckIO::contentHash(*target), state);
        backend.refresh();

        const QVariantList midnightList = backend.cardList();
        QCOMPARE(midnightList.size(), 1);
        QVERIFY(Config::isDueByDay(state.due, QDateTime::currentMSecsSinceEpoch(), 0));
        QVERIFY(midnightList.first().toMap().value("isDue").toBool());
        QCOMPARE(midnightList.first().toMap().value("dueText").toString(), QString("due now"));
        QCOMPARE(backend.availableCount(), 1);

        backend.setDayStartHour(3);
        QCOMPARE(backend.dayStartHour(), 3);
        const bool dueAtThree = Config::isDueByDay(state.due, QDateTime::currentMSecsSinceEpoch(), 3);
        const QVariantList shiftedList = backend.cardList();
        QCOMPARE(shiftedList.size(), 1);
        QCOMPARE(shiftedList.first().toMap().value("isDue").toBool(), dueAtThree);
        QCOMPARE(backend.availableCount(), dueAtThree ? 1 : 0);
    }
    Backend reopened(dir.path(), database);
    QString error;
    QVERIFY(reopened.initialize(&error));
    QCOMPARE(reopened.dayStartHour(), 3);
}

void TestBackend::backendReportsNewCountAndDeckSummaries() {
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

void TestBackend::qmlCanNavigateLibraryCards() {
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
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(OMACARDS_SOURCE_DIR) + "/Main.qml"));
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

void TestBackend::backendAppliesNewCardLimit() {
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

void TestBackend::backendRequeuesAgainInSameSession() {
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

void TestBackend::unreadableDeckKeepsSchedulingState() {
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

void TestBackend::failedCardWriteDoesNotMutateBackend() {
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

void TestBackend::unreadableStateDoesNotTreatEveryCardAsNew() {
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

void TestBackend::deckCountsMatchLibrarySummaries() {
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

void TestBackend::deckCreationRejectsSanitizedPathCollision() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("A/B", &error));
    QVERIFY(!backend.createDeck("A-B", &error));
    QCOMPARE(backend.decks().size(), 1);
    QCOMPARE(backend.decks().first(), QString("A/B"));
}

void TestBackend::refreshKeepsPendingAgainRepeat() {
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
