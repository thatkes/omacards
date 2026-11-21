#include <QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSignalSpy>
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
#include "domain.h"
#include "config.h"
#include "deck.h"
#include "fsrs.h"
#include "store.h"
#include "theme.h"

using namespace TestSupport;

namespace {

// The single seam for loading Main.qml in a test: callers verify view.window
// before using it, and the harness owns the engine and window for them.
struct MainWindowHarness {
    explicit MainWindowHarness(Backend &backend) {
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(OMACARDS_SOURCE_DIR) + "/Main.qml"));
        if (!component.isReady()) {
            qWarning() << component.errorString();
            return;
        }
        window = component.create();
        if (!window)
            qWarning() << component.errorString();
    }

    ~MainWindowHarness() { delete window; }

    QQuickItem *item(const QString &objectName) const {
        return window ? window->findChild<QQuickItem *>(objectName) : nullptr;
    }

    QQmlEngine engine;
    Theme theme;
    QObject *window = nullptr;
};

} // namespace

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

void TestBackend::backendCreatesCardAndRatesInCram() {
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
    backend.flip();
    QVERIFY(backend.showingAnswer());
    backend.rate(3);
    QCOMPARE(backend.reviewed(), 1);
    QCOMPARE(backend.good(), 1);
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
    QCOMPARE(backend.good(), 1);
}

void TestBackend::createCardRejectsInvalidType() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Test", &error));
    QVERIFY(!backend.createCard("Test", "Q", "A", "nonsense", {}, &error));
    QCOMPARE(error, QString("cards[0] has an invalid type: nonsense"));
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
    QCOMPARE(edited.type.kind, CardType::Cloze);
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
        if (card.type.kind == CardType::Cloze)
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

    MainWindowHarness view(backend);
    QVERIFY(view.window);

    QCOMPARE(view.window->property("cardIndex").toInt(), 0);
    QVERIFY(QMetaObject::invokeMethod(view.window, "moveCardSelection", Q_ARG(QVariant, 1)));
    QCOMPARE(view.window->property("cardIndex").toInt(), 1);
    QVERIFY(QMetaObject::invokeMethod(view.window, "moveCardSelection", Q_ARG(QVariant, -1)));
    QCOMPARE(view.window->property("cardIndex").toInt(), 0);
    QVERIFY(QMetaObject::invokeMethod(view.window, "moveCardSelection", Q_ARG(QVariant, -1)));
    QCOMPARE(view.window->property("cardIndex").toInt(), 2); // wraps around

    QVariant selected;
    QVERIFY(QMetaObject::invokeMethod(view.window, "selectedCard", Q_RETURN_ARG(QVariant, selected)));
    QCOMPARE(selected.toMap().value("question").toString(), QString("Q2"));

    QVERIFY(QMetaObject::invokeMethod(view.window, "removeSelectedCard"));
    QCOMPARE(backend.totalCards(), 2);
    QCOMPARE(backend.cardList().size(), 2);
}

void TestBackend::qmlCramLabelUsesCurrentBox() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Test", &error));
    QVERIFY(backend.createCard("Test", "Question?", "Answer", "basic", {}, &error));
    backend.selectDeck("Test");
    backend.setMode("cram");
    backend.refresh(); // startStudy() in Main.qml sets the mode, then refreshes

    MainWindowHarness view(backend);
    QVERIFY(view.window);

    QVERIFY(!backend.property("box").isValid());
    QQuickItem *label = view.item(QStringLiteral("cramBoxLabel"));
    QVERIFY(label);
    const int current = backend.currentBox();
    QVERIFY(current >= 1);
    QVERIFY(label->property("text").toString().contains(QStringLiteral("BOX %1 OF 5").arg(current)));
}

void TestBackend::qmlGoodLabelUsesGoodCount() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Test", &error));
    QVERIFY(backend.createCard("Test", "Question?", "Answer", "basic", {}, &error));
    backend.selectDeck("Test");
    backend.refresh();

    MainWindowHarness view(backend);
    QVERIFY(view.window);

    // The facade exposes only the Good property; the label forwards it.
    QVERIFY(!backend.property("correct").isValid());
    QCOMPARE(backend.property("good").toInt(), 0);
    QQuickItem *label = view.item(QStringLiteral("reviewGoodLabel"));
    QVERIFY(label);
    QCOMPARE(label->property("text").toString(), QString("0 reviewed · 0 Good"));

    backend.flip();
    backend.rate(3);
    QCOMPARE(backend.property("good").toInt(), 1);
    QCOMPARE(label->property("text").toString(), QString("1 reviewed · 1 Good"));

    QVERIFY(QMetaObject::invokeMethod(view.window, "openStats"));
    const QVariantMap stats = view.window->property("statsData").toMap();
    QCOMPARE(stats.value("sessionGood").toInt(), 1);
    QVERIFY(!stats.contains("sessionCorrect"));
}

void TestBackend::qmlSettingsRowsFitTheirContent() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));

    MainWindowHarness view(backend);
    QVERIFY(view.window);
    auto *window = qobject_cast<QQuickWindow *>(view.window);
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));

    auto settle = [] {
        QCoreApplication::processEvents();
        QTest::qWait(30);
    };

    window->resize(window->minimumWidth(), window->minimumHeight());
    settle();
    QVERIFY(QMetaObject::invokeMethod(view.window, "openSettings"));
    settle();
    QCOMPARE(view.window->property("screen").toString(), QString("settings"));

    auto visualRect = [](QQuickItem *item) { return item->mapRectToScene(item->boundingRect()); };
    auto isInside = [visualRect](QQuickItem *outer, QQuickItem *inner) {
        return visualRect(outer).contains(visualRect(inner));
    };
    auto leftOf = [visualRect](QQuickItem *left, QQuickItem *right) {
        return visualRect(left).right() <= visualRect(right).left() + 1.0;
    };
    auto above = [visualRect](QQuickItem *top, QQuickItem *bottom) {
        return visualRect(top).bottom() <= visualRect(bottom).top() + 1.0;
    };
    auto nameOf = [](QQuickItem *item) { return item ? item->objectName() : QStringLiteral("<missing>"); };

    QQuickItem *allowancePanel = view.item(QStringLiteral("settingsNewCardAllowancePanel"));
    QQuickItem *allowanceTitle = view.item(QStringLiteral("settingsNewCardAllowanceTitle"));
    QQuickItem *allowanceDescription = view.item(QStringLiteral("settingsNewCardAllowanceDescription"));
    QQuickItem *allowanceMinus = view.item(QStringLiteral("settingsNewCardAllowanceMinus"));
    QQuickItem *allowanceValue = view.item(QStringLiteral("settingsNewCardAllowanceValue"));
    QQuickItem *allowancePlus = view.item(QStringLiteral("settingsNewCardAllowancePlus"));
    QQuickItem *dayPanel = view.item(QStringLiteral("settingsDayStartHourPanel"));
    QQuickItem *dayTitle = view.item(QStringLiteral("settingsDayStartHourTitle"));
    QQuickItem *dayDescription = view.item(QStringLiteral("settingsDayStartHourDescription"));
    QQuickItem *dayMinus = view.item(QStringLiteral("settingsDayStartHourMinus"));
    QQuickItem *dayValue = view.item(QStringLiteral("settingsDayStartHourValue"));
    QQuickItem *dayPlus = view.item(QStringLiteral("settingsDayStartHourPlus"));
    QQuickItem *retentionPanel = view.item(QStringLiteral("settingsRetentionPanel"));
    QQuickItem *retentionTitle = view.item(QStringLiteral("settingsRetentionTitle"));
    QQuickItem *retentionValue = view.item(QStringLiteral("settingsRetentionValue"));
    QQuickItem *retentionSlider = view.item(QStringLiteral("settingsRetentionSlider"));
    QQuickItem *retentionDescription = view.item(QStringLiteral("settingsRetentionDescription"));
    QQuickItem *mathPanel = view.item(QStringLiteral("settingsMathPanel"));
    QQuickItem *mathTitle = view.item(QStringLiteral("settingsMathTitle"));
    QQuickItem *mathDescription = view.item(QStringLiteral("settingsMathDescription"));
    QQuickItem *mathEngine = view.item(QStringLiteral("settingsMathEngine"));
    QQuickItem *deckPanel = view.item(QStringLiteral("settingsDeckFilesPanel"));
    QQuickItem *deckTitle = view.item(QStringLiteral("settingsDeckFilesTitle"));
    QQuickItem *deckPath = view.item(QStringLiteral("settingsDeckFilesPath"));
    QQuickItem *deckEdit = view.item(QStringLiteral("settingsDeckFilesEditButton"));
    QQuickItem *scroll = view.item(QStringLiteral("settingsScrollView"));
    QQuickItem *back = view.item(QStringLiteral("settingsBack"));
    QVERIFY(allowancePanel && allowanceTitle && allowanceDescription && allowanceMinus);
    QVERIFY(allowanceValue && allowancePlus && dayPanel && dayTitle && dayDescription);
    QVERIFY(dayMinus && dayValue && dayPlus && retentionPanel && retentionTitle);
    QVERIFY(retentionValue && retentionSlider && retentionDescription && mathPanel);
    QVERIFY(mathTitle && mathDescription && mathEngine && deckPanel && deckTitle);
    QVERIFY(deckPath && deckEdit && scroll && back);

    QCOMPARE(allowanceTitle->property("text").toString(), QString("New-card allowance"));
    QCOMPARE(dayTitle->property("text").toString(), QString("Day-start hour"));

    auto checkLayout = [&]() -> bool {
        bool ok = true;
        auto inside = [&](QQuickItem *outer, QQuickItem *inner) {
            if (!isInside(outer, inner)) {
                qWarning() << nameOf(inner) << "paints outside" << nameOf(outer) << visualRect(inner)
                           << visualRect(outer);
                ok = false;
            }
        };
        auto separated = [&](QQuickItem *text, QQuickItem *control) {
            if (!leftOf(text, control)) {
                qWarning() << nameOf(text) << "overlaps" << nameOf(control) << visualRect(text) << visualRect(control);
                ok = false;
            }
        };
        auto fixedWidth = [&](QQuickItem *item, qreal expected) {
            if (qAbs(item->width() - expected) > 1.0) {
                qWarning() << nameOf(item) << "width" << item->width() << "expected" << expected;
                ok = false;
            }
        };

        inside(allowancePanel, allowanceTitle);
        inside(allowancePanel, allowanceDescription);
        inside(allowancePanel, allowanceMinus);
        inside(allowancePanel, allowanceValue);
        inside(allowancePanel, allowancePlus);
        separated(allowanceTitle, allowanceMinus);
        separated(allowanceDescription, allowanceMinus);
        fixedWidth(allowanceMinus, 38.0);
        fixedWidth(allowanceValue, 34.0);
        fixedWidth(allowancePlus, 38.0);

        inside(dayPanel, dayTitle);
        inside(dayPanel, dayDescription);
        inside(dayPanel, dayMinus);
        inside(dayPanel, dayValue);
        inside(dayPanel, dayPlus);
        separated(dayTitle, dayMinus);
        separated(dayDescription, dayMinus);
        fixedWidth(dayMinus, 38.0);
        fixedWidth(dayValue, 46.0);
        fixedWidth(dayPlus, 38.0);

        inside(retentionPanel, retentionTitle);
        inside(retentionPanel, retentionValue);
        inside(retentionPanel, retentionSlider);
        inside(retentionPanel, retentionDescription);
        separated(retentionTitle, retentionValue);
        if (!above(retentionSlider, retentionDescription)) {
            qWarning() << "retention slider overlaps description" << visualRect(retentionSlider)
                       << visualRect(retentionDescription);
            ok = false;
        }

        inside(mathPanel, mathTitle);
        inside(mathPanel, mathDescription);
        inside(mathPanel, mathEngine);
        separated(mathTitle, mathEngine);
        separated(mathDescription, mathEngine);

        inside(deckPanel, deckTitle);
        inside(deckPanel, deckPath);
        inside(deckPanel, deckEdit);
        separated(deckTitle, deckEdit);
        separated(deckPath, deckEdit);

        return ok;
    };

    QVERIFY2(checkLayout(), "Settings geometry failed with English descriptions");

    const qreal ordinaryAllowanceHeight = allowancePanel->height();
    const qreal ordinaryRetentionHeight = retentionPanel->height();
    const QString longDescription =
        QStringLiteral("Keeps a large import manageable. ") +
        QStringLiteral("A long translated description can wrap across many lines in this row. ").repeated(5);
    allowanceDescription->setProperty("text", longDescription);
    retentionDescription->setProperty("text", longDescription);
    settle();

    QVERIFY2(allowancePanel->height() > ordinaryAllowanceHeight + 1.0,
             "New-card allowance box did not grow with its description");
    QVERIFY2(retentionPanel->height() > ordinaryRetentionHeight + 1.0,
             "Desired retention box did not grow with its description");
    QVERIFY2(checkLayout(), "Settings geometry failed with long descriptions");

    const QString longPath =
        QStringLiteral("/home/learner/decks/") +
        QStringLiteral("a-very-long-decks-directory-name-that-must-not-stretch-the-row/").repeated(6);
    deckPath->setProperty("text", longPath);
    settle();

    const qreal deckPanelWidth = deckPanel->width();
    const qreal deckPanelHeight = deckPanel->height();
    QVERIFY2(checkLayout(), "Settings geometry failed with a long decks directory path");
    QCOMPARE(deckPanel->width(), deckPanelWidth);
    QCOMPARE(deckPanel->height(), deckPanelHeight);

    QQuickItem *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    const qreal maxContentY = qMax<qreal>(0.0, flickable->property("contentHeight").toReal() - flickable->height());
    QVERIFY2(maxContentY > 0.0, "Settings content should scroll after boxes grow");
    flickable->setProperty("contentY", maxContentY);
    settle();
    QVERIFY2(isInside(flickable, back), "Back button is not reachable by scrolling");
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

void TestBackend::deckSwitchKeepsPendingAgainRepeat() {
    QTemporaryDir dir;
    const QByteArray previousDelay = qgetenv("OMACARDS_AGAIN_DELAY_MS");
    qputenv("OMACARDS_AGAIN_DELAY_MS", "500");
    {
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("First", &error));
        QVERIFY(backend.createCard("First", "Q1", "A1", "basic", {}, &error));
        QVERIFY(backend.createCard("First", "Q2", "A2", "basic", {}, &error));
        QVERIFY(backend.createDeck("Second", &error));
        QVERIFY(backend.createCard("Second", "Q3", "A3", "basic", {}, &error));

        backend.selectDeck("First");
        backend.flip();
        backend.rate(1); // Q1 waits; Q2 is current.
        QVERIFY(backend.question().contains("Q2"));

        backend.selectDeck("Second");
        QVERIFY(backend.question().contains("Q3"));
        backend.selectDeck("First");
        QVERIFY(backend.question().contains("Q2"));

        backend.flip();
        backend.rate(3); // Q2 is done; Q1's pending repeat comes back.
        QTRY_VERIFY_WITH_TIMEOUT(backend.question().contains("Q1"), 2000);
    }
    if (previousDelay.isNull())
        qunsetenv("OMACARDS_AGAIN_DELAY_MS");
    else
        qputenv("OMACARDS_AGAIN_DELAY_MS", previousDelay);
}

void TestBackend::setMathColorsNotifiesColorChange() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Math", &error));
    QVERIFY(backend.createCard("Math", "What is \\(x^2\\)?", "A", "basic", {}, &error));
    backend.selectDeck("Math");
    QVERIFY(backend.hasCard());
    QVERIFY(!backend.question().isEmpty());

    int cardChanges = 0;
    QObject::connect(&backend, &Backend::cardChanged, &backend, [&cardChanges] { ++cardChanges; });
    backend.setMathColors("#112233", "#445566");
    QCOMPARE(cardChanges, 1);
    backend.setMathColors("#112233", "#445566"); // unchanged colors are a no-op
    QCOMPARE(cardChanges, 1);
    backend.setMathColors("#000000", "#000000");
    QCOMPARE(cardChanges, 2);
}

void TestBackend::statsSummaryKeepsContract() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Stats", &error));
    QVERIFY(backend.createCard("Stats", "Q1", "A1", "basic", {}, &error));
    backend.selectDeck("Stats");
    backend.flip();
    backend.rate(3);

    const QVariantMap summary = backend.statsSummary();
    QStringList keys = summary.keys();
    keys.sort();
    QStringList expected{"accuracy",        "cards",      "dayStartHour", "decks",     "due",
                         "newAvailable",    "newPerDay",  "newToday",     "retention", "sessionGood",
                         "sessionReviewed", "streakDays", "today",        "todayGood", "totalReviews"};
    expected.sort();
    QCOMPARE(keys, expected);
    QCOMPARE(summary.value("cards").toInt(), backend.totalCards());
    QCOMPARE(summary.value("decks").toInt(), backend.decks().size());
    QCOMPARE(summary.value("due").toInt(), backend.dueCount());
    QCOMPARE(summary.value("sessionReviewed").toInt(), backend.reviewed());
    QCOMPARE(summary.value("sessionGood").toInt(), backend.good());
    QCOMPARE(summary.value("todayGood").toInt(), 1);
}

void TestBackend::setModeBoundarySemantics() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Modes", &error));
    QVERIFY(backend.createCard("Modes", "Q1", "A1", "basic", {}, &error));
    QVERIFY(backend.createCard("Modes", "Q2", "A2", "basic", {}, &error));
    QVERIFY(backend.createCard("Modes", "Q3", "A3", "basic", {}, &error));
    backend.selectDeck("Modes");
    QVERIFY(backend.hasCard());
    QCOMPARE(backend.mode(), QString("review"));

    QSignalSpy modeChanged(&backend, &Backend::modeChanged);
    QSignalSpy statsChanged(&backend, &Backend::statsChanged);
    QSignalSpy cardChanged(&backend, &Backend::cardChanged);
    QStringList order;
    QObject::connect(&backend, &Backend::modeChanged, &backend, [&order] { order << QStringLiteral("mode"); });
    QObject::connect(&backend, &Backend::statsChanged, &backend, [&order] { order << QStringLiteral("stats"); });

    // Unknown text normalizes to review, but the raw argument differs from the
    // canonical mode, so both boundaries run: the session resets counters and
    // emits modeChanged before statsChanged, then the facade rebuilds.
    backend.flip();
    QVERIFY(backend.showingAnswer());
    backend.rate(3);
    QCOMPARE(backend.reviewed(), 1);
    modeChanged.clear();
    statsChanged.clear();
    cardChanged.clear();
    order.clear();
    backend.setMode("banana");
    QCOMPARE(backend.mode(), QString("review"));
    QCOMPARE(backend.reviewed(), 0);
    QCOMPARE(backend.good(), 0);
    QCOMPARE(modeChanged.count(), 1);
    QVERIFY(statsChanged.count() >= 1);
    QVERIFY(cardChanged.count() >= 1);
    QVERIFY(!backend.showingAnswer()); // rebuild re-presented the card
    QCOMPARE(order.first(), QStringLiteral("mode"));
    QVERIFY(order.contains(QStringLiteral("stats")));

    backend.flip();
    backend.rate(3);
    QCOMPARE(backend.reviewed(), 1);
    modeChanged.clear();
    statsChanged.clear();
    cardChanged.clear();
    order.clear();
    backend.setMode("CRAM");
    QCOMPARE(backend.mode(), QString("review"));
    QCOMPARE(backend.reviewed(), 0);
    QCOMPARE(backend.good(), 0);
    QCOMPARE(modeChanged.count(), 1);
    QVERIFY(statsChanged.count() >= 1);
    QVERIFY(cardChanged.count() >= 1);
    QVERIFY(!backend.showingAnswer());
    QCOMPARE(order.first(), QStringLiteral("mode"));

    // Canonical input in the current mode is a complete no-op.
    modeChanged.clear();
    statsChanged.clear();
    cardChanged.clear();
    order.clear();
    backend.setMode("review");
    QCOMPARE(modeChanged.count(), 0);
    QCOMPARE(statsChanged.count(), 0);
    QCOMPARE(cardChanged.count(), 0);
    QVERIFY(order.isEmpty());

    // A real switch resets the session counters and emits modeChanged first.
    backend.flip();
    backend.rate(3);
    QCOMPARE(backend.reviewed(), 1);
    QCOMPARE(backend.good(), 1);
    modeChanged.clear();
    statsChanged.clear();
    order.clear();
    backend.setMode("cram");
    QCOMPARE(backend.mode(), QString("cram"));
    QCOMPARE(backend.reviewed(), 0);
    QCOMPARE(backend.good(), 0);
    QCOMPARE(modeChanged.count(), 1);
    QVERIFY(statsChanged.count() >= 1);
    QVERIFY(order.size() >= 2);
    QCOMPARE(order.at(0), QStringLiteral("mode"));
    QCOMPARE(order.at(1), QStringLiteral("stats"));
}

void TestBackend::invalidRatingsAreNoOps() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Ratings", &error));
    QVERIFY(backend.createCard("Ratings", "Q1", "A1", "basic", {}, &error));
    backend.selectDeck("Ratings");
    QVERIFY(backend.hasCard());

    QSignalSpy statsChanged(&backend, &Backend::statsChanged);
    QSignalSpy cardChanged(&backend, &Backend::cardChanged);
    QSignalSpy statusChanged(&backend, &Backend::statusChanged);
    QSignalSpy decksChanged(&backend, &Backend::decksChanged);
    const QString status = backend.status();
    for (int rating : {0, 2, 4}) {
        backend.rate(rating);
        QCOMPARE(backend.reviewed(), 0);
        QCOMPARE(backend.good(), 0);
        QCOMPARE(backend.status(), status);
    }
    QCOMPARE(statsChanged.count(), 0);
    QCOMPARE(cardChanged.count(), 0);
    QCOMPARE(statusChanged.count(), 0);
    QCOMPARE(decksChanged.count(), 0);

    QTemporaryDir emptyRoot;
    Backend empty(emptyRoot.path(), emptyRoot.filePath("state.db"));
    QVERIFY(empty.initialize(&error));
    QVERIFY(!empty.hasCard());
    QSignalSpy emptyStats(&empty, &Backend::statsChanged);
    QSignalSpy emptyCards(&empty, &Backend::cardChanged);
    empty.rate(3);
    QCOMPARE(empty.reviewed(), 0);
    QCOMPARE(emptyStats.count(), 0);
    QCOMPARE(emptyCards.count(), 0);
}

void TestBackend::settingsClampAndPersist() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    {
        Backend backend(dir.path(), database);
        QString error;
        QVERIFY(backend.initialize(&error));
        QSignalSpy changed(&backend, &Backend::settingsChanged);

        backend.setNewPerDay(-5);
        QCOMPARE(backend.newPerDay(), 0);
        QCOMPARE(changed.count(), 1);
        backend.setNewPerDay(10000);
        QCOMPARE(backend.newPerDay(), Config::kMaxNewPerDay);
        QCOMPARE(changed.count(), 2);

        backend.setDesiredRetention(0.1);
        QCOMPARE(backend.desiredRetention(), Config::kMinRetention);
        QCOMPARE(changed.count(), 3);
        backend.setDesiredRetention(2.0);
        QCOMPARE(backend.desiredRetention(), Config::kMaxRetention);
        QCOMPARE(changed.count(), 4);

        backend.setDayStartHour(12);
        QCOMPARE(changed.count(), 5);
        backend.setDayStartHour(-3);
        QCOMPARE(backend.dayStartHour(), 0);
        QCOMPARE(changed.count(), 6);
        backend.setDayStartHour(99);
        QCOMPARE(backend.dayStartHour(), Config::kMaxDayStartHour);
        QCOMPARE(changed.count(), 7);
    }
    {
        Backend reopened(dir.path(), database);
        QString error;
        QVERIFY(reopened.initialize(&error));
        QCOMPARE(reopened.newPerDay(), Config::kMaxNewPerDay);
        QCOMPARE(reopened.desiredRetention(), Config::kMaxRetention);
        QCOMPARE(reopened.dayStartHour(), Config::kMaxDayStartHour);
    }

    QTemporaryDir rawDir;
    const QString rawDatabase = rawDir.filePath("state.db");
    {
        Store store(rawDatabase);
        QString error;
        QVERIFY(store.open(&error));
        QVERIFY(store.setSetting("new_per_day", "100000"));
        QVERIFY(store.setSetting("desired_retention", "7.5"));
        QVERIFY(store.setSetting("day_start_hour", "77"));
    }
    Backend clamped(rawDir.path(), rawDatabase);
    QString error;
    QVERIFY(clamped.initialize(&error));
    QCOMPARE(clamped.newPerDay(), Config::kMaxNewPerDay);
    QCOMPARE(clamped.desiredRetention(), Config::kMaxRetention);
    QCOMPARE(clamped.dayStartHour(), Config::kMaxDayStartHour);
}

void TestBackend::signalContractsForRefreshAndNavigation() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Nav", &error));
    QVERIFY(backend.createCard("Nav", "Q1", "A1", "basic", {}, &error));
    backend.selectDeck("Nav");

    QSignalSpy decks(&backend, &Backend::decksChanged);
    QSignalSpy current(&backend, &Backend::currentDeckChanged);
    QSignalSpy cards(&backend, &Backend::cardsChanged);
    QSignalSpy filter(&backend, &Backend::cardFilterChanged);

    backend.refresh();
    QCOMPARE(decks.count(), 1);
    QCOMPARE(current.count(), 1);
    QCOMPARE(cards.count(), 1);

    decks.clear();
    current.clear();
    cards.clear();
    backend.selectDeck("");
    backend.selectDeck("NoSuchDeck");
    QCOMPARE(decks.count(), 0);
    QCOMPARE(current.count(), 0);
    QCOMPARE(cards.count(), 0);

    backend.setCardFilter("Q1");
    QCOMPARE(filter.count(), 1);
    QCOMPARE(cards.count(), 1);
    filter.clear();
    cards.clear();
    backend.setCardFilter("Q1");
    QCOMPARE(filter.count(), 0);
    QCOMPARE(cards.count(), 0);

    QTemporaryDir emptyRoot;
    Backend empty(emptyRoot.path(), emptyRoot.filePath("state.db"));
    QVERIFY(empty.initialize(&error));
    QSignalSpy status(&empty, &Backend::statusChanged);
    QVERIFY(!empty.canUndo());
    QVERIFY(!empty.canRedo());
    empty.undo();
    QCOMPARE(empty.status(), QString("Nothing to undo"));
    QCOMPARE(status.count(), 1);
    empty.redo();
    QCOMPARE(empty.status(), QString("Nothing to redo"));
    QCOMPARE(status.count(), 2);
}

void TestBackend::modifyCardInvalidTypeNormalizesToBasic() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Types", &error));
    QVERIFY(backend.createCard("Types", "Q", "A", "basic", {}, &error));
    const QString id = backend.allDecks().first().cards.first().id;
    QString finalId;
    QVERIFY(backend.modifyCard("Types", id, "Q", "A", "not-a-type", {}, &error, &finalId));
    QVERIFY(error.isEmpty());
    QVERIFY(!finalId.isEmpty());
    QCOMPARE(backend.allDecks().first().cards.first().type.kind, CardType::Basic);

    QVERIFY(backend.modifyCard("Types", finalId, "Q", "A", "cloze", {}, &error, &finalId));
    QCOMPARE(backend.allDecks().first().cards.first().type.kind, CardType::Cloze);
}

void TestBackend::deckSummariesAndSearchFilterByDeck() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Alpha", &error));
    QVERIFY(backend.createCard("Alpha", "alpha question", "one", "basic", {}, &error));
    QVERIFY(backend.createCard("Alpha", "other card", "two", "basic", {}, &error));
    QVERIFY(backend.createDeck("Beta", &error));
    QVERIFY(backend.createCard("Beta", "beta question", "three", "basic", {}, &error));

    backend.selectDeck("Alpha");
    QVariantList summaries = backend.deckSummaries();
    QCOMPARE(summaries.size(), 2);
    QVERIFY(summaries.at(0).toMap().value("active").toBool());
    QVERIFY(!summaries.at(1).toMap().value("active").toBool());

    backend.selectDeck("Beta");
    summaries = backend.deckSummaries();
    QVERIFY(!summaries.at(0).toMap().value("active").toBool());
    QVERIFY(summaries.at(1).toMap().value("active").toBool());

    QCOMPARE(backend.availableCount("Alpha"), 2);
    QCOMPARE(backend.availableCount("Beta"), 1);
    QCOMPARE(backend.availableCount(), 3);

    QCOMPARE(backend.searchCards("question").size(), 2);
    QCOMPARE(backend.searchCards("question", "Alpha").size(), 1);
    QCOMPARE(backend.searchCards("").size(), 3);
    QCOMPARE(backend.searchCards("", "Beta").size(), 1);
}

void TestBackend::emptySessionHelpersAndStatsText() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(!backend.hasCard());

    QSignalSpy cardChanged(&backend, &Backend::cardChanged);
    QSignalSpy statusChanged(&backend, &Backend::statusChanged);
    backend.flip();
    backend.showNext();
    QCOMPARE(cardChanged.count(), 0);
    QCOMPARE(statusChanged.count(), 0);

    QSignalSpy shown(&backend, &Backend::showWindowRequested);
    backend.requestShow();
    QCOMPARE(shown.count(), 1);

    QVERIFY(backend.createDeck("Stats", &error));
    QVERIFY(backend.createCard("Stats", "Q1", "A1", "basic", {}, &error));
    backend.selectDeck("Stats");
    backend.setMode("cram");
    backend.flip();
    backend.rate(3);
    QCOMPARE(backend.reviewed(), 1);
    QCOMPARE(backend.good(), 1);
    QCOMPARE(backend.currentBox(), 2);
    QCOMPARE(backend.statsText(), QString("1 reviewed · 100% Good · box 2"));
}

void TestBackend::setDeckDescriptionAndQmlErrorSignals() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Described", &error, "Original"));

    QSignalSpy decks(&backend, &Backend::decksChanged);
    QVERIFY(backend.setDeckDescription("Described", " Original "));
    QCOMPARE(decks.count(), 0);
    QVERIFY(backend.setDeckDescription("Described", "Updated"));
    QCOMPARE(decks.count(), 1);
    QCOMPARE(backend.allDecks().first().description, QString("Updated"));

    QSignalSpy errors(&backend, &Backend::errorOccurred);
    backend.newDeck("");
    QCOMPARE(errors.count(), 1);
    QVERIFY(!backend.status().isEmpty());

    QTemporaryDir emptyRoot;
    Backend empty(emptyRoot.path(), emptyRoot.filePath("state.db"));
    QVERIFY(empty.initialize(&error));
    QSignalSpy emptyErrors(&empty, &Backend::errorOccurred);
    empty.addCard("Q", "A");
    QCOMPARE(emptyErrors.count(), 1);
    QVERIFY(!empty.status().isEmpty());
}
