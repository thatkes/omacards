#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "deck.h"
#include "fsrs.h"
#include "mathrender.h"
#include "store.h"
#include "theme.h"
#include "translator.h"

class OmacardsTests : public QObject {
    Q_OBJECT

private slots:
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
    static bool writeFile(const QString &path, const QByteArray &contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    }
};

QTEST_MAIN(OmacardsTests)
#include "omacards_tests.moc"
