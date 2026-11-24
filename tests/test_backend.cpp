#include <QtTest>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QMouseEvent>
#include <QPoint>
#include <QWheelEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSize>
#include <QSignalSpy>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <memory>
#include <utility>

#include "test_backend.h"
#include "test_support.h"
#include "backend.h"
#include "domain.h"
#include "config.h"
#include "deck.h"
#include "fsrs.h"
#include "mathrender.h"
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

    QString screenName() const { return window ? window->property("screen").toString() : QString(); }

    // Repeater delegates are visual children but not QObject children of the
    // window, so findChild() cannot see help-overlay rows.
    QQuickItem *visualItem(const QString &objectName) const {
        auto *qwindow = qobject_cast<QQuickWindow *>(window);
        if (!qwindow)
            return nullptr;
        QList<QQuickItem *> pending{qwindow->contentItem()};
        while (!pending.isEmpty()) {
            QQuickItem *current = pending.takeLast();
            if (current->objectName() == objectName)
                return current;
            pending.append(current->childItems());
        }
        return nullptr;
    }

    // Geometry-only facts for the loaded study panel. All rects are mapped
    // through real QML items; text measured inside the scroll content, and
    // gaps measured against the clipped viewport, so a long answer that is
    // still scrollable does not look like an overlap with the fixed controls.
    QVariantMap visualFacts() const {
        QVariantMap facts;
        auto *scroll = item(QStringLiteral("flashcardViewport"));
        auto *content = scroll ? scroll->property("contentItem").value<QQuickItem *>() : nullptr;
        auto *question = item(QStringLiteral("studyQuestionLabel"));
        auto *answer = item(QStringLiteral("studyAnswerLabel"));
        auto *hint = item(QStringLiteral("studyHint"));
        auto *again = item(QStringLiteral("studyAgainButton"));
        auto *good = item(QStringLiteral("studyGoodButton"));
        if (!scroll || !content || !question || !answer || !hint || !again || !good)
            return facts;

        const qreal tolerance = 1.5;
        const qreal contentWidth = content->property("contentWidth").toReal();
        const qreal contentHeight = content->property("contentHeight").toReal();
        const QRectF contentRect(0.0, 0.0, contentWidth, contentHeight);
        const QRectF viewportScene = scroll->mapRectToScene(scroll->boundingRect());
        QRectF textInContent;
        QRectF visibleTextScene;
        for (QQuickItem *label : {question, answer}) {
            if (!label->isVisible())
                continue;
            const QRectF labelContent = label->mapRectToItem(content, label->boundingRect());
            textInContent = textInContent.isEmpty() ? labelContent : textInContent.united(labelContent);
            const QRectF labelScene = label->mapRectToScene(label->boundingRect()).intersected(viewportScene);
            visibleTextScene = visibleTextScene.isEmpty() ? labelScene : visibleTextScene.united(labelScene);
        }
        if (textInContent.isEmpty() || visibleTextScene.isEmpty())
            return facts;

        facts.insert(QStringLiteral("textInsideContent"),
                     contentRect.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(textInContent));
        const auto gapBelowText = [&](QQuickItem *control) {
            if (!control->isVisible())
                return -1.0;
            return control->mapRectToScene(control->boundingRect()).top() - visibleTextScene.bottom();
        };
        facts.insert(QStringLiteral("gapToHint"), gapBelowText(hint));
        facts.insert(QStringLiteral("gapToRatings"), qMin(gapBelowText(again), gapBelowText(good)));
        facts.insert(QStringLiteral("scrollRange"), qMax<qreal>(0.0, contentHeight - content->height()));
        facts.insert(QStringLiteral("lastLineBottomGap"), contentHeight - textInContent.bottom());
        facts.insert(QStringLiteral("horizontalOverflow"), contentWidth - content->width());
        facts.insert(QStringLiteral("answerContentWidth"), answer->property("contentWidth").toReal());
        facts.insert(QStringLiteral("textWidth"), answer->width());
        const auto fontPixels = [](QQuickItem *label) {
            return qvariant_cast<QFont>(label->property("font")).pixelSize();
        };
        facts.insert(QStringLiteral("answerFontPixels"), fontPixels(answer));
        facts.insert(QStringLiteral("questionFontPixels"), fontPixels(question));
        return facts;
    }

    void settle() const {
        QCoreApplication::processEvents();
        QTest::qWait(30);
    }

    // DeckRepository debounces watcher signals for 120 ms; let one reload
    // finish before asserting on study state after creating cards.
    void settleFileWatcher() const {
        QTest::qWait(200);
        QCoreApplication::processEvents();
    }

    QQmlEngine engine;
    Theme theme;
    QObject *window = nullptr;
};

// Shared setup for the study-view tests. The window is still the real Main.qml
// loaded by MainWindowHarness; this only owns the temp dir, Backend and card
// setup that every study test used to repeat.
struct StudyViewFixture {
    enum class Sizing { Minimum, KeepWindowSize };

    explicit StudyViewFixture(Backend::CramOrder cramOrder = {})
        : backend(dir.path(), dir.filePath("state.db"), std::move(cramOrder)) {}

    bool initialize() { return backend.initialize(&error); }

    bool createDeck(const QString &name = QStringLiteral("Study")) { return backend.createDeck(name, &error); }

    bool createCard(const QString &question, const QString &answer, const QString &type = QStringLiteral("basic"),
                    const QString &tags = {}) {
        return backend.createCard(QStringLiteral("Study"), question, answer, type, tags, &error);
    }

    void selectDeck(const QString &name = QStringLiteral("Study")) { backend.selectDeck(name); }

    void prepareMode(const QString &mode) {
        backend.setMode(mode);
        backend.refresh();
    }

    bool loadWindow() {
        view = std::make_unique<MainWindowHarness>(backend);
        return view->window != nullptr;
    }

    QQuickWindow *window() const { return view ? qobject_cast<QQuickWindow *>(view->window) : nullptr; }

    QString screenName() const { return view ? view->screenName() : QString(); }

    QQuickItem *item(const QString &objectName) const { return view ? view->item(objectName) : nullptr; }

    QVariantMap visualFacts() const { return view ? view->visualFacts() : QVariantMap(); }

    bool revealAndResetFlashcardViewport() const {
        return view && QMetaObject::invokeMethod(view->window, "revealAndResetFlashcardViewport");
    }

    bool scaleImagesToWidth(const QString &html, double width, QString *out) const {
        QVariant result;
        const bool invoked =
            view && QMetaObject::invokeMethod(view->window, "scaleImagesToWidth", Q_RETURN_ARG(QVariant, result),
                                              Q_ARG(QVariant, html), Q_ARG(QVariant, width));
        *out = result.toString();
        return invoked;
    }

    bool enterStudy(const QString &mode, Sizing sizing = Sizing::Minimum) {
        if (!window() || !QTest::qWaitForWindowExposed(window()))
            return false;
        if (sizing == Sizing::Minimum)
            window()->resize(window()->minimumWidth(), window()->minimumHeight());
        return startStudy(mode);
    }

    bool enterStudy(const QString &mode, const QSize &size) {
        if (!window() || !QTest::qWaitForWindowExposed(window()))
            return false;
        window()->resize(size);
        return startStudy(mode);
    }

    void settle() const { view->settle(); }

    QTemporaryDir dir;
    Backend backend;
    QString error;
    std::unique_ptr<MainWindowHarness> view;

private:
    bool startStudy(const QString &mode) {
        if (!QMetaObject::invokeMethod(view->window, "startStudy", Q_ARG(QVariant, mode)))
            return false;
        view->settleFileWatcher();
        return true;
    }
};

QString manyCardLines(int count) {
    QString text;
    for (int i = 0; i < count; ++i)
        text += QStringLiteral("<div>line %1 stays on one line</div>").arg(i);
    return text;
}

} // namespace

void TestBackend::defaultPathsIgnoreOrganizationName() {
    // Scoped guards restore both variables even when an assertion below fails;
    // a manual restore after QVERIFY would leak state into later suites.
    ScopedEnv noDecks("OMACARDS_DECKS_DIR");
    ScopedEnv noState("OMACARDS_STATE_DB");

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
    backend.reveal();
    QVERIFY(backend.showingAnswer());
    backend.rate(3);
    QCOMPARE(backend.reviewed(), 1);
    QCOMPARE(backend.good(), 1);
    QCOMPARE(backend.reviewSummary().total, 0); // Cram keeps its Leitner-only path
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
    backend.reveal();

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
    QCOMPARE(edited.id, changedId); // an edit keeps the id
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

    backend.setNewCardAllowance(3);
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

namespace {
// Three decks with a known selector order: Alpha is Current, Beta was selected
// before it, and Gamma was added straight to disk so it has no recency record.
void prepareSelectorDecks(Backend &backend, QString *error) {
    QVERIFY(backend.createDeck("Alpha", error));
    QVERIFY(backend.createCard("Alpha", "alpha one", "A1", "basic", {}, error));
    QVERIFY(backend.createCard("Alpha", "alpha two", "A2", "basic", {}, error));
    QVERIFY(backend.createDeck("Beta", error));
    QVERIFY(backend.createCard("Beta", "beta one", "B1", "basic", {}, error));
    const QDir dir(backend.decksDirectory());
    QVERIFY(TestSupport::writeFile(dir.filePath("Gamma.txt"), "# Deck: Gamma\n"));
    backend.refresh();
    backend.selectDeck("Beta");
    backend.selectDeck("Alpha");
}
// Popup delegates can be reachable through visual children rather than QObject
// children, so selector tests accept either traversal.
QQuickItem *pickerItem(const MainWindowHarness &view, const QString &objectName) {
    if (QQuickItem *found = view.item(objectName))
        return found;
    return view.visualItem(objectName);
}

QQuickItem *popupRowInViewport(const MainWindowHarness &view, QQuickItem *list, const QString &name) {
    if (!list)
        return nullptr;
    const qreal top = list->property("contentY").toReal();
    const qreal bottom = top + list->height();
    for (int i = 0; i < 12; ++i) {
        QQuickItem *row = pickerItem(view, QStringLiteral("homeDeckPopupRow%1").arg(i));
        if (!row)
            return nullptr;
        if (row->property("deckName").toString() != name)
            continue;
        const qreal y = row->mapToItem(list, QPointF(0, 0)).y();
        if (y >= top && y + row->height() <= bottom)
            return row;
        return nullptr;
    }
    return nullptr;
}

void scrollPopupListTo(QQuickItem *list, const QString &name) {
    QVERIFY(list);
    QVERIFY(QMetaObject::invokeMethod(list, "scrollToRow", Q_ARG(QVariant, name)));
}

QObject *pickerObject(const MainWindowHarness &view, const QString &objectName) {
    return view.window ? view.window->findChild<QObject *>(objectName) : nullptr;
}

QPoint centerInWindow(QQuickWindow *window, QQuickItem *item) {
    return window->contentItem()->mapFromScene(item->mapToScene(item->boundingRect().center())).toPoint();
}

} // namespace

void TestBackend::qmlDeckSelectorShowsCurrentDeckAndCounts() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    prepareSelectorDecks(backend, &error);

    MainWindowHarness view(backend);
    QVERIFY(view.window);
    auto *window = qobject_cast<QQuickWindow *>(view.window);
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));

    QQuickItem *homeSelector = view.item(QStringLiteral("homeDeckSelector"));
    QVERIFY(homeSelector);
    const QString selectorClass = QString::fromLatin1(homeSelector->metaObject()->className());
    QVERIFY2(!selectorClass.contains("ComboBox"), qPrintable(selectorClass));
    QVERIFY(!homeSelector->property("open").toBool());
    QCOMPARE(homeSelector->property("candidateName").toString(), QString("Alpha"));

    QQuickItem *homeSummary = view.item(QStringLiteral("homeDeckLabel"));
    QVERIFY(homeSummary);
    const QString homeText = homeSummary->property("text").toString();
    QVERIFY2(homeText.contains("Alpha"), qPrintable(homeText));
    QVERIFY2(homeText.contains("2 cards"), qPrintable(homeText));
    QVERIFY2(homeText.contains("0 due"), qPrintable(homeText));
    QQuickItem *chevron = view.item(QStringLiteral("homeDeckSelectorChevron"));
    QVERIFY(chevron);
    QVERIFY(!chevron->property("text").toString().isEmpty());
    QQuickItem *hint = view.item(QStringLiteral("homeDeckSelectorHint"));
    QVERIFY(hint);
    QVERIFY2(hint->property("text").toString().contains("[ / ]"), qPrintable(hint->property("text").toString()));

    QVERIFY(QMetaObject::invokeMethod(homeSelector, "openAtCurrent"));
    view.settle();
    QObject *popup = pickerObject(view, QStringLiteral("homeDeckPopup"));
    QVERIFY(popup);
    QVERIFY(popup->property("visible").toBool());
    QCOMPARE(popup->property("width").toInt(), 320);

    QQuickItem *list = pickerItem(view, QStringLiteral("homeDeckPopupList"));
    QVERIFY(list);
    QCOMPARE(list->property("height").toInt(), 264); // five rows plus four gaps
    QQuickItem *footer = pickerItem(view, QStringLiteral("homeDeckPopupFooter"));
    QVERIFY(footer);
    const QString footerText = footer->property("text").toString();
    QVERIFY2(footerText.contains("Enter switch"), qPrintable(footerText));
    QVERIFY2(footerText.contains("Esc cancel"), qPrintable(footerText));

    QQuickItem *row = pickerItem(view, QStringLiteral("homeDeckPopupRow0"));
    QVERIFY(row);
    QCOMPARE(row->property("deckName").toString(), QString("Alpha"));
    QVERIFY(row->property("candidate").toBool());
    QVERIFY(row->property("current").toBool());
    Theme expectedTheme;
    QCOMPARE(qvariant_cast<QColor>(row->property("color")), QColor(expectedTheme.accent()));
    QQuickItem *counts = pickerItem(view, QStringLiteral("homeDeckPopupRowCounts0"));
    QVERIFY(counts);
    const QString countsText = counts->property("text").toString();
    QVERIFY2(countsText.contains("2 cards"), qPrintable(countsText));
    QVERIFY2(countsText.contains("0 due"), qPrintable(countsText));
    QQuickItem *tag = pickerItem(view, QStringLiteral("homeDeckPopupRowTag0"));
    QVERIFY(tag);
    QVERIFY(tag->isVisible());
    QVERIFY2(tag->property("tagText").toString().contains("Current"), qPrintable(tag->property("tagText").toString()));
    QQuickItem *otherTag = pickerItem(view, QStringLiteral("homeDeckPopupRowTag1"));
    QVERIFY(otherTag);
    QVERIFY(!otherTag->isVisible());

    // Study now shares this picker, with its own trigger and popup names.
    QVERIFY(QMetaObject::invokeMethod(view.window, "startStudy", Q_ARG(QVariant, QString("review"))));
    view.settle();
    QQuickItem *studySelector = view.item(QStringLiteral("studyDeckSelector"));
    QVERIFY(studySelector);
    QCOMPARE(QString::fromLatin1(studySelector->metaObject()->className()), selectorClass);
    QVERIFY(!studySelector->property("open").toBool());
    QCOMPARE(studySelector->property("candidateName").toString(), QString("Alpha"));
    QQuickItem *studySummary = view.item(QStringLiteral("studyDeckLabel"));
    QVERIFY(studySummary);
    const QString studyText = studySummary->property("text").toString();
    QVERIFY2(studyText.contains("Alpha"), qPrintable(studyText));
    QVERIFY2(studyText.contains("2 cards"), qPrintable(studyText));
}

void TestBackend::qmlDeckCycleKeysWrapAndAreScoped() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    prepareSelectorDecks(backend, &error);

    MainWindowHarness view(backend);
    QVERIFY(view.window);
    auto *window = qobject_cast<QQuickWindow *>(view.window);
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QCOMPARE(backend.currentDeck(), QString("Alpha"));
    QQuickItem *picker = view.item(QStringLiteral("homeDeckSelector"));
    QVERIFY(picker);

    const auto press = [&](Qt::Key key) {
        QTest::keyClick(window, key);
        view.settle();
    };
    const auto candidate = [&]() { return picker->property("candidateName").toString(); };
    const auto isOpen = [&]() { return picker->property("open").toBool(); };

    // Opening never switches. Candidates move through the same ordered list in
    // both directions and wrap at each end.
    press(Qt::Key_BracketRight);
    QCOMPARE(backend.currentDeck(), QString("Alpha"));
    QVERIFY(isOpen());
    QCOMPARE(candidate(), QString("Alpha"));
    press(Qt::Key_BracketRight);
    QCOMPARE(candidate(), QString("Beta"));
    QCOMPARE(backend.currentDeck(), QString("Alpha"));
    QVERIFY(pickerItem(view, QStringLiteral("homeDeckPopupRow1"))->property("candidate").toBool());
    press(Qt::Key_BracketRight);
    QCOMPARE(candidate(), QString("Gamma"));
    press(Qt::Key_BracketRight);
    QCOMPARE(candidate(), QString("Alpha"));
    press(Qt::Key_BracketLeft);
    QCOMPARE(candidate(), QString("Gamma"));

    // Unrelated keys are swallowed while the popup is open.
    press(Qt::Key_B);
    QCOMPARE(view.window->property("screen").toString(), QString("home"));
    press(Qt::Key_S);
    QVERIFY(!view.window->property("statsVisible").toBool());
    press(Qt::Key_Question);
    QVERIFY(!view.window->property("helpVisible").toBool());
    QCOMPARE(candidate(), QString("Gamma"));
    QVERIFY(isOpen());

    // Escape discards the Candidate deck.
    press(Qt::Key_Escape);
    QVERIFY(!isOpen());
    QCOMPARE(backend.currentDeck(), QString("Alpha"));
    QCOMPARE(candidate(), QString("Alpha"));

    // Enter commits the Candidate deck; committing Current only closes.
    press(Qt::Key_BracketLeft);
    QCOMPARE(candidate(), QString("Alpha"));
    press(Qt::Key_BracketLeft);
    QCOMPARE(candidate(), QString("Gamma"));
    press(Qt::Key_Enter);
    QVERIFY(!isOpen());
    QCOMPARE(backend.currentDeck(), QString("Gamma"));
    press(Qt::Key_BracketRight);
    QCOMPARE(candidate(), QString("Gamma"));
    press(Qt::Key_Return);
    QVERIFY(!isOpen());
    QCOMPARE(backend.currentDeck(), QString("Gamma"));

    // Brackets stay scoped to Home and never open when a dialog owns keys.
    QVERIFY(QMetaObject::invokeMethod(view.window, "openLibrary"));
    press(Qt::Key_BracketRight);
    QVERIFY(!isOpen());
    QCOMPARE(backend.currentDeck(), QString("Gamma"));
    QVERIFY(QMetaObject::invokeMethod(view.window, "openSettings"));
    press(Qt::Key_BracketLeft);
    QVERIFY(!isOpen());
    QCOMPARE(backend.currentDeck(), QString("Gamma"));

    QVERIFY(QMetaObject::invokeMethod(view.window, "goHome"));
    window->setProperty("helpVisible", true);
    press(Qt::Key_BracketRight);
    QVERIFY(!isOpen());
    window->setProperty("helpVisible", false);

    // The Library shortcut is unchanged.
    QVERIFY(QMetaObject::invokeMethod(view.window, "goHome"));
    press(Qt::Key_B);
    QCOMPARE(view.window->property("screen").toString(), QString("library"));
}

void TestBackend::qmlDeckPickerClickCommitsAndLongNamesElide() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    prepareSelectorDecks(backend, &error);
    const QString longName = QStringLiteral("Molecular Biology — Lecture 12 (Cell Signalling and membrane transport)");
    QVERIFY(backend.createDeck(longName, &error));
    QVERIFY(backend.createCard(longName, "long one", "L1", "basic", {}, &error));
    for (const QString &name : {QStringLiteral("Delta"), QStringLiteral("Epsilon"), QStringLiteral("Zeta")})
        QVERIFY(backend.createDeck(name, &error));
    backend.selectDeck(QStringLiteral("Alpha"));

    MainWindowHarness view(backend);
    QVERIFY(view.window);
    auto *window = qobject_cast<QQuickWindow *>(view.window);
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    // At the minimum window size the 65% cap forces long names to elide.
    window->resize(window->minimumWidth(), window->minimumHeight());
    QQuickItem *picker = view.item(QStringLiteral("homeDeckSelector"));
    QVERIFY(picker);
    QVERIFY(QMetaObject::invokeMethod(picker, "openAtCurrent"));
    view.settle();

    QQuickItem *list = pickerItem(view, QStringLiteral("homeDeckPopupList"));
    QVERIFY(list);
    QVERIFY(list->property("contentHeight").toReal() > list->property("height").toReal());

    scrollPopupListTo(list, QStringLiteral("Beta"));
    QQuickItem *betaRow = popupRowInViewport(view, list, QStringLiteral("Beta"));
    QVERIFY(betaRow);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(window, betaRow));
    view.settle();
    QCOMPARE(backend.currentDeck(), QString("Beta"));
    QVERIFY(!picker->property("open").toBool());

    QVERIFY(QMetaObject::invokeMethod(picker, "openAtCurrent"));
    view.settle();
    scrollPopupListTo(list, longName);
    QQuickItem *longRow = popupRowInViewport(view, list, longName);
    QVERIFY(longRow);
    QQuickItem *name = pickerItem(view, longRow->property("nameObjectName").toString());
    QVERIFY(name);
    QVERIFY2(name->property("truncated").toBool(), qPrintable(name->property("text").toString()));
    QCOMPARE(longRow->property("fullName").toString(), longName);
    QCOMPARE(longRow->property("tooltipText").toString(), longName);
    QVERIFY(name->property("text").toString().contains("Molecular Biology"));

    // A click outside discards a moved Candidate and never switches decks.
    QVERIFY(QMetaObject::invokeMethod(picker, "moveCandidate", Q_ARG(QVariant, 1)));
    QVERIFY(picker->property("candidateName").toString() != backend.currentDeck());
    QCOMPARE(backend.currentDeck(), QString("Beta"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
    view.settle();
    QVERIFY(!picker->property("open").toBool());
    QCOMPARE(picker->property("candidateName").toString(), QString("Beta"));
    QCOMPARE(backend.currentDeck(), QString("Beta"));
}

void TestBackend::qmlHelpOverlayNamesDeckSelector() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Alpha", &error));
    backend.selectDeck("Alpha");

    MainWindowHarness view(backend);
    QVERIFY(view.window);

    QQuickItem *row = view.visualItem(QStringLiteral("helpDeckSelectorRow"));
    QVERIFY(row);
    QVERIFY(!row->isVisible());

    view.window->setProperty("helpVisible", true);
    view.settle();
    QVERIFY(row->isVisible());

    QQuickItem *line = view.visualItem(QStringLiteral("helpDeckSelectorRowText"));
    QVERIFY(line);
    const QString text = line->property("text").toString();
    QVERIFY2(text.contains(QStringLiteral("Deck selector")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("Home")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("Study header")), qPrintable(text));

    const QStringList steps{QStringLiteral("open"),  QStringLiteral("move"),   QStringLiteral("candidate deck"),
                            QStringLiteral("enter"), QStringLiteral("commit"), QStringLiteral("esc"),
                            QStringLiteral("cancel")};
    const QString lower = text.toLower();
    for (const QString &step : steps) {
        QVERIFY2(lower.contains(step), qPrintable(QStringLiteral("missing '%1' in: %2").arg(step, text)));
    }

    QQuickItem *keysRow = view.visualItem(QStringLiteral("helpDeckSelectorKeysRow"));
    QVERIFY(keysRow);
    QVERIFY(keysRow->isVisible());
    QQuickItem *keysLine = view.visualItem(QStringLiteral("helpDeckSelectorKeysRowText"));
    QVERIFY(keysLine);
    const QString keysText = keysLine->property("text").toString();
    const QString keysLower = keysText.toLower();
    for (const QString &step : steps) {
        QVERIFY2(keysLower.contains(step), qPrintable(QStringLiteral("missing '%1' in: %2").arg(step, keysText)));
    }
}

void TestBackend::qmlStudySelectorSwitchesDeckFresh() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.backend.createDeck("Alpha", &fixture.error));
    QVERIFY(fixture.backend.createCard("Alpha", "Alpha question", manyCardLines(40), "basic", {}, &fixture.error));
    QVERIFY(fixture.backend.createCard("Alpha", "Alpha second", "A2", "basic", {}, &fixture.error));
    QVERIFY(fixture.backend.createDeck("Beta", &fixture.error));
    QVERIFY(fixture.backend.createCard("Beta", "Beta question", "B", "basic", {}, &fixture.error));
    QVERIFY(fixture.backend.createCard("Beta", "Beta second", "B2", "basic", {}, &fixture.error));
    fixture.selectDeck("Alpha");
    fixture.view->settleFileWatcher();

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));
    auto *selector = fixture.item(QStringLiteral("studyDeckSelector"));
    QVERIFY(selector);
    const QString selectorClass = QString::fromLatin1(selector->metaObject()->className());
    QVERIFY2(!selectorClass.contains("ComboBox"), qPrintable(selectorClass));
    QVERIFY(!selector->property("open").toBool());
    QCOMPARE(selector->property("candidateName").toString(), QString("Alpha"));
    auto *summary = fixture.item(QStringLiteral("studyDeckLabel"));
    QVERIFY(summary);
    QVERIFY2(summary->property("text").toString().contains("Alpha"), qPrintable(summary->property("text").toString()));

    QTest::keyClick(window, Qt::Key_Space);
    fixture.settle();
    QTest::keyClick(window, Qt::Key_2);
    fixture.settle();
    QCOMPARE(fixture.backend.reviewed(), 1);
    QCOMPARE(fixture.backend.property("boxDistribution").toList(), QVariantList({1, 1, 0, 0, 0}));

    // Reveal the long answer so a stray scroll key would move the viewport.
    QTest::keyClick(window, Qt::Key_Space);
    fixture.settle();
    QVERIFY(fixture.backend.showingAnswer());
    auto *scroll = fixture.item(QStringLiteral("flashcardViewport"));
    QVERIFY(scroll);
    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    flickable->setProperty("contentY", 48.0);
    fixture.settle();

    // Brackets open the shared picker; opening never switches decks.
    QTest::keyClick(window, Qt::Key_BracketRight);
    fixture.settle();
    QVERIFY(selector->property("open").toBool());
    QCOMPARE(selector->property("candidateName").toString(), QString("Alpha"));
    QCOMPARE(fixture.backend.currentDeck(), QString("Alpha"));

    // Every other Study key is inert while the picker owns the keyboard.
    for (Qt::Key key : {Qt::Key_J, Qt::Key_K, Qt::Key_Down, Qt::Key_Up, Qt::Key_Space, Qt::Key_1, Qt::Key_2, Qt::Key_A,
                        Qt::Key_G, Qt::Key_S, Qt::Key_B, Qt::Key_R, Qt::Key_C, Qt::Key_Question}) {
        QTest::keyClick(window, key);
        fixture.settle();
    }
    QVERIFY(selector->property("open").toBool());
    QCOMPARE(selector->property("candidateName").toString(), QString("Alpha"));
    QCOMPARE(fixture.backend.currentDeck(), QString("Alpha"));
    QCOMPARE(fixture.backend.reviewed(), 1);
    QCOMPARE(fixture.backend.property("boxDistribution").toList(), QVariantList({1, 1, 0, 0, 0}));
    QVERIFY(fixture.backend.showingAnswer());
    QCOMPARE(flickable->property("contentY").toReal(), qreal(48));
    QCOMPARE(fixture.screenName(), QStringLiteral("study"));
    QVERIFY(!fixture.view->window->property("statsVisible").toBool());
    QVERIFY(!fixture.view->window->property("helpVisible").toBool());

    // Moving the Candidate highlights a row without changing the sitting.
    QTest::keyClick(window, Qt::Key_BracketRight);
    fixture.settle();
    QCOMPARE(selector->property("candidateName").toString(), QString("Beta"));
    QVERIFY(pickerItem(*fixture.view, QStringLiteral("studyDeckPopupRow1"))->property("candidate").toBool());
    QCOMPARE(fixture.backend.currentDeck(), QString("Alpha"));
    QCOMPARE(fixture.backend.reviewed(), 1);

    // Escape discards the Candidate and leaves the sitting untouched.
    QTest::keyClick(window, Qt::Key_Escape);
    fixture.settle();
    QVERIFY(!selector->property("open").toBool());
    QCOMPARE(selector->property("candidateName").toString(), QString("Alpha"));
    QCOMPARE(fixture.backend.currentDeck(), QString("Alpha"));
    QCOMPARE(fixture.backend.reviewed(), 1);

    // Rate Again so the sitting is dirty, then commit a different deck.
    QTest::keyClick(window, Qt::Key_1);
    fixture.settle();
    QCOMPARE(fixture.backend.reviewed(), 2);
    QTest::keyClick(window, Qt::Key_BracketRight);
    fixture.settle();
    QTest::keyClick(window, Qt::Key_BracketRight);
    fixture.settle();
    QCOMPARE(selector->property("candidateName").toString(), QString("Beta"));
    QTest::keyClick(window, Qt::Key_Return);
    fixture.settle();
    QVERIFY(!selector->property("open").toBool());
    QCOMPARE(fixture.backend.currentDeck(), QString("Beta"));
    QCOMPARE(fixture.backend.mode(), QString("cram"));
    QVERIFY2(fixture.backend.question().contains("Beta"), qPrintable(fixture.backend.question()));
    QCOMPARE(fixture.backend.reviewed(), 0);
    QCOMPARE(fixture.backend.currentLeitnerBox(), 1);
    QCOMPARE(fixture.backend.property("boxDistribution").toList(), QVariantList({2, 0, 0, 0, 0}));

    const auto names = [](const QVariantList &entries) {
        QStringList result;
        for (const QVariant &entry : entries)
            result << entry.toMap().value(QStringLiteral("name")).toString();
        return result;
    };
    QCOMPARE(names(fixture.backend.deckSelector()).first(), QString("Beta"));

    // Enter on the Current deck closes without a recency write or a rebuild.
    QTest::keyClick(window, Qt::Key_Space);
    fixture.settle();
    QTest::keyClick(window, Qt::Key_2);
    fixture.settle();
    QCOMPARE(fixture.backend.reviewed(), 1);
    const QStringList order = names(fixture.backend.deckSelector());
    QTest::keyClick(window, Qt::Key_BracketRight);
    fixture.settle();
    QCOMPARE(selector->property("candidateName").toString(), QString("Beta"));
    QTest::keyClick(window, Qt::Key_Return);
    fixture.settle();
    QVERIFY(!selector->property("open").toBool());
    QCOMPARE(names(fixture.backend.deckSelector()), order);
    QCOMPARE(fixture.backend.reviewed(), 1);
    QCOMPARE(fixture.backend.mode(), QString("cram"));

    // A long deck name grows the popup up to 65% of the window, then elides.
    const int reviewedBeforeLong = fixture.backend.reviewed();
    const QString longName =
        QStringLiteral("Molecular Biology — Lecture 12 (Cell Signalling and membrane transport, with extended notes)");
    QVERIFY(fixture.backend.createDeck(longName, &fixture.error));
    QVERIFY(fixture.backend.createCard(longName, "Long question", "L", "basic", {}, &fixture.error));
    fixture.view->settleFileWatcher();
    QTest::keyClick(window, Qt::Key_BracketRight);
    fixture.settle();
    QObject *popup = window->findChild<QObject *>(QStringLiteral("studyDeckPopup"));
    QVERIFY(popup);
    QVERIFY(popup->property("visible").toBool());
    const qreal popupWidth = popup->property("width").toReal();
    const qreal cap = window->width() * 0.65;
    QVERIFY2(popupWidth > 260.0, qPrintable(QString::number(popupWidth)));
    QVERIFY2(popupWidth <= cap + 1.0, qPrintable(QString::number(popupWidth)));
    auto *list = pickerItem(*fixture.view, QStringLiteral("studyDeckPopupList"));
    QVERIFY(list);
    QCOMPARE(list->property("width").toReal(), popupWidth - 16.0);
    scrollPopupListTo(list, longName);
    QQuickItem *longRow = nullptr;
    for (int i = 0; i < 12; ++i) {
        auto *row = pickerItem(*fixture.view, QStringLiteral("studyDeckPopupRow%1").arg(i));
        if (row && row->property("deckName").toString() == longName) {
            longRow = row;
            break;
        }
    }
    QVERIFY(longRow);
    auto *name = pickerItem(*fixture.view, longRow->property("nameObjectName").toString());
    QVERIFY(name);
    QVERIFY(name->property("truncated").toBool());
    QCOMPARE(longRow->property("fullName").toString(), longName);

    // The capped popup stays inside the window and overlaps the flashcard,
    // as in the approved Study prototype.
    const QRectF popupRect(popup->property("x").toReal(), popup->property("y").toReal(), popupWidth,
                           popup->property("height").toReal());
    const QRectF scrollRect = scroll->mapRectToScene(scroll->boundingRect());
    QVERIFY(popupRect.intersects(scrollRect));
    QVERIFY(QRectF(0, 0, window->width(), window->height()).contains(popupRect));

    // Click-outside discards a moved Candidate as it does on Home.
    QTest::keyClick(window, Qt::Key_BracketRight);
    fixture.settle();
    QVERIFY(selector->property("candidateName").toString() != fixture.backend.currentDeck());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
    fixture.settle();
    QVERIFY(!selector->property("open").toBool());
    QCOMPARE(selector->property("candidateName").toString(), longName);
    QCOMPARE(fixture.backend.currentDeck(), longName);
    QCOMPARE(fixture.backend.reviewed(), reviewedBeforeLong);
}

void TestBackend::qmlStudyFlashcardViewportClipsAndSizes() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Short question", manyCardLines(40)));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));
    QCOMPARE(fixture.screenName(), QStringLiteral("study"));

    auto *scroll = fixture.item(QStringLiteral("flashcardViewport"));
    auto *verticalBar = fixture.item(QStringLiteral("studyVerticalScrollBar"));
    auto *horizontalBar = fixture.item(QStringLiteral("studyHorizontalScrollBar"));
    auto *question = fixture.item(QStringLiteral("studyQuestionLabel"));
    auto *answer = fixture.item(QStringLiteral("studyAnswerLabel"));
    QVERIFY(scroll && verticalBar && horizontalBar && question && answer);
    QVERIFY(scroll->findChild<QQuickItem *>(QStringLiteral("studyQuestionLabel")));
    QVERIFY(scroll->findChild<QQuickItem *>(QStringLiteral("studyAnswerLabel")));
    QVERIFY(scroll->property("clip").toBool());
    QCOMPARE(horizontalBar->property("policy").toInt(), static_cast<int>(Qt::ScrollBarAlwaysOff));

    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    QVERIFY(flickable->property("contentHeight").toReal() <= flickable->height() + 1.0);
    QVERIFY(verticalBar->property("size").toDouble() >= 0.99);

    auto visualRect = [](QQuickItem *item) { return item->mapRectToScene(item->boundingRect()); };
    const QStringList fixedNames{"studyDeckLabel",   "studyModeLabel",  "studyShowAnswer", "studyHint",
                                 "studyAgainButton", "studyGoodButton", "reviewGoodLabel"};
    QList<QQuickItem *> fixed;
    for (const QString &name : fixedNames) {
        QQuickItem *item = fixture.item(name);
        QVERIFY2(item, qPrintable(name + QStringLiteral(" is missing")));
        QVERIFY2(!scroll->isAncestorOf(item), qPrintable(name + QStringLiteral(" is inside the viewport")));
        fixed << item;
    }
    QList<QRectF> places;
    for (QQuickItem *item : fixed)
        places << visualRect(item);

    QVERIFY(fixture.revealAndResetFlashcardViewport());
    fixture.settle();
    QVERIFY(fixture.backend.showingAnswer());

    const qreal maxContentY = qMax<qreal>(0.0, flickable->property("contentHeight").toReal() - flickable->height());
    QVERIFY2(maxContentY > 50.0, "long card content should overflow the viewport");
    QVERIFY(verticalBar->property("size").toDouble() < 0.99);
    flickable->setProperty("contentY", maxContentY / 2);
    fixture.settle();

    for (int i = 0; i < fixed.size(); ++i)
        QCOMPARE(visualRect(fixed.at(i)), places.at(i));

    const qreal shortViewportHeight = scroll->height();
    const qreal heldPosition = flickable->property("contentY").toReal();
    window->resize(1200, 900);
    fixture.settle();
    QVERIFY2(scroll->height() > shortViewportHeight + 100.0, "viewport did not grow with the window");
    const qreal grownMax = qMax<qreal>(0.0, flickable->property("contentHeight").toReal() - flickable->height());
    QVERIFY(qAbs(flickable->property("contentY").toReal() - qMin(heldPosition, grownMax)) < 1.0);
}

void TestBackend::qmlStudyScrollFirstKeysAndRevealFallbacks() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Long card", manyCardLines(40)));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));

    auto *scroll = fixture.item(QStringLiteral("flashcardViewport"));
    QVERIFY(scroll);
    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    auto press = [&](Qt::Key key) {
        QTest::keyClick(window, key);
        fixture.settle();
    };
    auto position = [&] { return flickable->property("contentY").toReal(); };
    auto maxPosition = [&] {
        return qMax<qreal>(0.0, flickable->property("contentHeight").toReal() - flickable->height());
    };

    QVERIFY(!fixture.backend.showingAnswer());
    press(Qt::Key_Down); // the short question cannot scroll, so it reveals
    QVERIFY(fixture.backend.showingAnswer());
    QVERIFY2(maxPosition() > 50.0, "the long answer should scroll");

    press(Qt::Key_Down);
    QVERIFY(fixture.backend.showingAnswer());
    QVERIFY(position() > 0.0);
    const qreal afterDown = position();
    press(Qt::Key_Up);
    QVERIFY(fixture.backend.showingAnswer());
    QVERIFY(position() < afterDown);

    press(Qt::Key_J);
    QVERIFY(fixture.backend.showingAnswer());
    QVERIFY(position() > 0.0);
    const qreal afterJ = position();
    press(Qt::Key_K);
    QVERIFY(fixture.backend.showingAnswer());
    QVERIFY(position() < afterJ);

    flickable->setProperty("contentY", 0.0);
    fixture.settle();
    press(Qt::Key_K); // K only scrolls, so it cannot reveal at the top
    QVERIFY(fixture.backend.showingAnswer());
    QCOMPARE(position(), qreal(0));
    flickable->setProperty("contentY", maxPosition());
    fixture.settle();
    press(Qt::Key_J); // J only scrolls, so it cannot reveal at the bottom
    QVERIFY(fixture.backend.showingAnswer());
    QCOMPARE(position(), maxPosition());
    press(Qt::Key_Down); // at the bottom, Down reveals
    QVERIFY(!fixture.backend.showingAnswer());
    press(Qt::Key_Up); // at the top, Up reveals
    QVERIFY(fixture.backend.showingAnswer());

    flickable->setProperty("contentY", 0.0);
    press(Qt::Key_Space);
    QVERIFY(!fixture.backend.showingAnswer());
    press(Qt::Key_Return);
    QVERIFY(fixture.backend.showingAnswer());
    press(Qt::Key_Enter);
    QVERIFY(!fixture.backend.showingAnswer());
    QCOMPARE(position(), qreal(0));
}

void TestBackend::qmlStudyJKScrollOnlyWithoutOverflow() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Short question", "Short answer"));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));

    auto *scroll = fixture.item(QStringLiteral("flashcardViewport"));
    auto *verticalBar = fixture.item(QStringLiteral("studyVerticalScrollBar"));
    auto *topFade = fixture.item(QStringLiteral("studyTopFade"));
    auto *bottomFade = fixture.item(QStringLiteral("studyBottomFade"));
    auto *endHint = fixture.item(QStringLiteral("studyEndOfScrollHint"));
    QVERIFY(scroll && verticalBar && topFade && bottomFade && endHint);
    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    QVERIFY(flickable->property("contentHeight").toReal() <= flickable->height() + 1.0);

    auto noCues = [&] {
        fixture.settle();
        QVERIFY(verticalBar->property("size").toDouble() >= 0.99);
        QVERIFY(!topFade->property("visible").toBool());
        QVERIFY(!bottomFade->property("visible").toBool());
        QVERIFY(!endHint->property("visible").toBool());
    };
    noCues();

    auto press = [&](Qt::Key key) {
        QTest::keyClick(window, key);
        fixture.settle();
    };
    press(Qt::Key_J);
    QVERIFY(!fixture.backend.showingAnswer());
    press(Qt::Key_K);
    QVERIFY(!fixture.backend.showingAnswer());
    QCOMPARE(flickable->property("contentY").toReal(), qreal(0));

    press(Qt::Key_Space);
    QVERIFY(fixture.backend.showingAnswer());
    noCues();

    press(Qt::Key_J);
    press(Qt::Key_K);
    QVERIFY(fixture.backend.showingAnswer());
    QCOMPARE(flickable->property("contentY").toReal(), qreal(0));
}

void TestBackend::qmlStudyScrollCuesFollowPosition() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard(manyCardLines(30), manyCardLines(40)));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));

    auto *scroll = fixture.item(QStringLiteral("flashcardViewport"));
    auto *verticalBar = fixture.item(QStringLiteral("studyVerticalScrollBar"));
    auto *horizontalBar = fixture.item(QStringLiteral("studyHorizontalScrollBar"));
    auto *topFade = fixture.item(QStringLiteral("studyTopFade"));
    auto *bottomFade = fixture.item(QStringLiteral("studyBottomFade"));
    auto *endHint = fixture.item(QStringLiteral("studyEndOfScrollHint"));
    QVERIFY(scroll && verticalBar && horizontalBar && topFade && bottomFade && endHint);
    QCOMPARE(verticalBar->property("policy").toInt(), static_cast<int>(Qt::ScrollBarAsNeeded));
    QCOMPARE(horizontalBar->property("policy").toInt(), static_cast<int>(Qt::ScrollBarAlwaysOff));
    QCOMPARE(endHint->property("text").toString(), QStringLiteral("At end — ↑/↓ flips card"));

    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    auto maxPosition = [&] {
        return qMax<qreal>(0.0, flickable->property("contentHeight").toReal() - flickable->height());
    };
    auto expectCues = [&](bool topFadeVisible, bool bottomFadeVisible, bool hintVisible, bool barHasOverflow) {
        fixture.settle();
        QCOMPARE(topFade->property("visible").toBool(), topFadeVisible);
        QCOMPARE(bottomFade->property("visible").toBool(), bottomFadeVisible);
        QCOMPARE(endHint->property("visible").toBool(), hintVisible);
        QCOMPARE(verticalBar->property("size").toDouble() < 0.99, barHasOverflow);
    };

    expectCues(false, true, false, true); // the first question overflows at the top

    flickable->setProperty("contentY", maxPosition());
    expectCues(true, false, false, true); // no hint before Reveal, even at the bottom

    QTest::keyClick(window, Qt::Key_Space);
    expectCues(false, true, false, true); // Reveal resets scroll to the top

    flickable->setProperty("contentY", maxPosition() / 2);
    expectCues(true, true, false, true);

    flickable->setProperty("contentY", maxPosition());
    expectCues(true, false, true, true); // the hint appears only at the bottom of revealed content

    QTest::keyClick(window, Qt::Key_Space);
    expectCues(false, true, false, true); // reveal resets scroll to the top of the question
}

void TestBackend::qmlStudyRatingKeysKeepTheirMeaning() {
    const QList<QPair<Qt::Key, int>> cases{{Qt::Key_Left, 0}, {Qt::Key_Right, 1}, {Qt::Key_A, 0}, {Qt::Key_G, 1}};
    for (const auto &entry : cases) {
        StudyViewFixture fixture;
        QVERIFY(fixture.initialize());
        QVERIFY(fixture.createDeck());
        QVERIFY(fixture.createCard("Question?", "Answer"));
        fixture.selectDeck();

        QVERIFY(fixture.loadWindow());
        auto *window = fixture.window();
        QVERIFY(window);
        QVERIFY(fixture.enterStudy(QStringLiteral("review"), StudyViewFixture::Sizing::KeepWindowSize));

        QTest::keyClick(window, Qt::Key_Space);
        fixture.settle();
        QVERIFY(fixture.backend.showingAnswer());
        QTest::keyClick(window, entry.first);
        fixture.settle();
        QCOMPARE(fixture.backend.reviewed(), 1);
        QCOMPARE(fixture.backend.good(), entry.second);
    }
}

void TestBackend::qmlStudyResetsScrollOnRevealAndNewCard() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard(manyCardLines(30), manyCardLines(30)));
    QVERIFY(fixture.createCard(manyCardLines(30), "short"));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("review"));

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("review")));

    auto *scroll = fixture.item(QStringLiteral("flashcardViewport"));
    QVERIFY(scroll);
    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    auto position = [&] { return flickable->property("contentY").toReal(); };
    auto scrollDown = [&] {
        flickable->setProperty("contentY", 90.0);
        fixture.settle();
        QVERIFY(position() > 0.0);
    };

    QTest::keyClick(window, Qt::Key_Space);
    fixture.settle();
    QVERIFY(fixture.backend.showingAnswer());
    scrollDown();
    QTest::keyClick(window, Qt::Key_Space);
    fixture.settle();
    QVERIFY(!fixture.backend.showingAnswer());
    QCOMPARE(position(), qreal(0));

    scrollDown();
    QTest::keyClick(window, Qt::Key_Space);
    fixture.settle();
    QVERIFY(fixture.backend.showingAnswer());
    const QString firstCard = fixture.backend.currentCardId();
    QTest::keyClick(window, Qt::Key_Right);
    fixture.settle();
    QVERIFY(fixture.backend.currentCardId() != firstCard);
    QCOMPARE(position(), qreal(0));
}

void TestBackend::qmlStudyPointerScrollAndClickReveal() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Question?", manyCardLines(40)));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));
    QTest::keyClick(window, Qt::Key_Space);
    fixture.settle();
    QVERIFY(fixture.backend.showingAnswer());

    auto *scroll = fixture.item(QStringLiteral("flashcardViewport"));
    QVERIFY(scroll);
    auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
    QVERIFY(flickable);
    const QRectF viewportRect = scroll->mapRectToScene(scroll->boundingRect());
    const QPoint center = viewportRect.center().toPoint();
    auto *clickArea = fixture.item(QStringLiteral("studyClickArea"));
    QVERIFY(clickArea);
    const QPoint panelCorner = clickArea->mapRectToScene(clickArea->boundingRect()).topLeft().toPoint() + QPoint(8, 8);
    QVERIFY(window->geometry().contains(center));

    int cardChanges = 0;
    QObject::connect(&fixture.backend, &Backend::cardChanged, &fixture.backend, [&] { ++cardChanges; });
    const QPointF local = flickable->mapFromScene(QPointF(center));
    QMouseEvent press(QEvent::MouseButtonPress, local, QPointF(center), QPointF(center), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(flickable, &press);
    for (int step = 1; step <= 8; ++step) {
        const QPointF at = local + QPointF(0, -step * 16);
        QMouseEvent move(QEvent::MouseMove, at, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(flickable, &move);
        fixture.settle();
    }
    QMouseEvent release(QEvent::MouseButtonRelease, local + QPointF(0, -128), local + QPointF(0, -128),
                        local + QPointF(0, -128), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(flickable, &release);
    fixture.settle();
    QVERIFY2(flickable->property("contentY").toReal() > 10.0, "drag should pan the answer");
    QVERIFY(fixture.backend.showingAnswer());
    QCOMPARE(cardChanges, 0);

    flickable->setProperty("contentY", 0.0);
    fixture.settle();

    QWheelEvent wheel(center, center, QPoint(), QPoint(0, -240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                      false);
    QCoreApplication::sendEvent(window, &wheel);
    fixture.settle();
    QVERIFY2(flickable->property("contentY").toReal() > 10.0, "wheel should pan the answer");
    QVERIFY(fixture.backend.showingAnswer());
    QCOMPARE(cardChanges, 0);

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, panelCorner);
    fixture.settle();
    QVERIFY(!fixture.backend.showingAnswer());
}

void TestBackend::qmlStudyScalesPicturesToWidth() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.loadWindow());
    fixture.settle();

    auto scale = [&](const QString &html, double width, QString *out) {
        return fixture.scaleImagesToWidth(html, width, out);
    };
    auto dimensions = [](const QString &tag) {
        static const QRegularExpression widthPattern(QStringLiteral("\\bwidth\\s*=\\s*[\"']?([0-9.]+)[\"']?"));
        static const QRegularExpression heightPattern(QStringLiteral("\\bheight\\s*=\\s*[\"']?([0-9.]+)[\"']?"));
        const QRegularExpressionMatch width = widthPattern.match(tag);
        const QRegularExpressionMatch height = heightPattern.match(tag);
        if (!width.hasMatch() || !height.hasMatch())
            return QPair<qreal, qreal>(-1, -1);
        return QPair<qreal, qreal>(width.captured(1).toDouble(), height.captured(1).toDouble());
    };

    const QString svgUrl = QUrl::fromLocalFile(fixture.dir.filePath("math.svg")).toString();
    const QString svgTag = QStringLiteral("<img src='%1' alt='x' width='600' height='200' align='middle' "
                                          "style='vertical-align: middle;'>")
                               .arg(svgUrl);
    QString scaled;
    QVERIFY(scale(svgTag, 300.0, &scaled));
    QVERIFY(scaled.contains(QStringLiteral("width=\"300\"")));
    QVERIFY(scaled.contains(QStringLiteral("height=\"100\"")));
    QVERIFY(scaled.contains(QStringLiteral("alt='x'")));
    QVERIFY(scaled.contains(QStringLiteral("align='middle'")));
    QVERIFY(!scaled.contains(QStringLiteral("font-size")));

    const QString pngPath = fixture.dir.filePath("wide.png");
    QImage wide(600, 200, QImage::Format_ARGB32);
    wide.fill(Qt::white);
    QVERIFY(wide.save(pngPath));
    const QString html =
        QStringLiteral("<p>before</p><img src=\"%1\"><p>after</p>").arg(QUrl::fromLocalFile(pngPath).toString());
    QVERIFY(scale(html, 300.0, &scaled));
    QVERIFY(scaled.contains(QStringLiteral("<p>before</p>")));
    QVERIFY(scaled.contains(QStringLiteral("<p>after</p>")));
    const int imageStart = scaled.indexOf(QStringLiteral("<img"));
    QVERIFY(imageStart >= 0);
    const int imageEnd = scaled.indexOf('>', imageStart);
    QVERIFY(imageEnd > imageStart);
    const QPair<qreal, qreal> pngSize = dimensions(scaled.mid(imageStart, imageEnd - imageStart + 1));
    QVERIFY(pngSize.first > 0);
    QVERIFY(pngSize.second > 0);
    QCOMPARE(pngSize.first, qreal(300));
    QCOMPARE(pngSize.second, qreal(100));

    const QString narrow = QStringLiteral("<img src='%1' width='120' height='40'>").arg(svgUrl);
    QVERIFY(scale(narrow, 300.0, &scaled));
    const QPair<qreal, qreal> narrowSize = dimensions(scaled);
    QVERIFY(narrowSize.first > 0);
    QVERIFY(narrowSize.second > 0);
    QCOMPARE(narrowSize.first, qreal(120));
    QCOMPARE(narrowSize.second, qreal(40));
}

void TestBackend::qmlStudyVisualFactsPinLayoutAtMinimumSize() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QImage wide(900, 300, QImage::Format_ARGB32);
    wide.fill(Qt::white);
    const QString imagePath = fixture.dir.filePath("wide.png");
    QVERIFY(wide.save(imagePath));
    const QString answer = QStringLiteral("<img src=\"%1\" width=\"900\" height=\"300\">%2")
                               .arg(QUrl::fromLocalFile(imagePath).toString(), manyCardLines(30));
    QVERIFY(fixture.createCard("Short question", answer));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    auto *window = fixture.window();
    QVERIFY(window);
    QVERIFY(fixture.enterStudy(QStringLiteral("cram"), QSize(760, 540)));
    QCOMPARE(window->width(), 760);
    QCOMPARE(window->height(), 540);
    QCOMPARE(fixture.screenName(), QStringLiteral("study"));

    const QStringList names{"textInsideContent", "gapToHint",          "gapToRatings",       "scrollRange",
                            "lastLineBottomGap", "horizontalOverflow", "answerContentWidth", "textWidth",
                            "answerFontPixels",  "questionFontPixels"};
    const QVariantMap questionFacts = fixture.visualFacts();
    for (const QString &name : names)
        QVERIFY2(questionFacts.contains(name), qPrintable(name + QStringLiteral(" missing in question facts")));

    QVERIFY(questionFacts.value("textInsideContent").toBool());
    QVERIFY2(questionFacts.value("gapToHint").toDouble() >= 8.0, "question text touches the hint");
    QVERIFY2(questionFacts.value("horizontalOverflow").toDouble() <= 1.0, "horizontal overflow in question state");

    QVERIFY(fixture.revealAndResetFlashcardViewport());
    fixture.settle();
    QVERIFY(fixture.backend.showingAnswer());

    const QVariantMap answerFacts = fixture.visualFacts();
    for (const QString &name : names)
        QVERIFY2(answerFacts.contains(name), qPrintable(name + QStringLiteral(" missing in answer facts")));

    QVERIFY(answerFacts.value("textInsideContent").toBool());
    QVERIFY2(answerFacts.value("gapToRatings").toDouble() >= 8.0, "answer text touches the Again/Good row");
    QVERIFY2(answerFacts.value("scrollRange").toDouble() > 50.0, "long answer should scroll");
    QVERIFY2(qAbs(answerFacts.value("lastLineBottomGap").toDouble()) <= 1.0,
             "last answer line should lie at the scrollable content bottom");
    QVERIFY2(answerFacts.value("horizontalOverflow").toDouble() <= 1.0, "horizontal overflow in answer state");
    QVERIFY2(answerFacts.value("answerContentWidth").toDouble() <= answerFacts.value("textWidth").toDouble() + 1.0,
             "wide image should fit inside the text column");
    QCOMPARE(answerFacts.value("answerFontPixels").toInt(), 24);
    QCOMPARE(answerFacts.value("questionFontPixels").toInt(), 30);
}

void TestBackend::qmlCramLabelUsesCurrentLeitnerBox() {
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
    const QVariant distribution = backend.property("boxDistribution");
    QVERIFY(distribution.isValid());
    QCOMPARE(distribution.toList(), QVariantList({1, 0, 0, 0, 0}));
    QQuickItem *label = view.item(QStringLiteral("cramLeitnerBoxLabel"));
    QVERIFY(label);
    const int current = backend.currentLeitnerBox();
    QVERIFY(current >= 1);
    QVERIFY(label->property("text").toString().contains(QStringLiteral("BOX %1 OF 5").arg(current)));
}

void TestBackend::qmlCramBoxDistributionShowsCountsTimingAndHighlight() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Q1", "A1"));
    QVERIFY(fixture.createCard("Q2", "A2"));
    QVERIFY(fixture.createCard("Q3", "A3"));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));

    const QVariantList counts = fixture.backend.property("boxDistribution").toList();
    QCOMPARE(counts, QVariantList({3, 0, 0, 0, 0}));
    QCOMPARE(fixture.backend.property("cramNextInCards").toInt(), 1);

    QQuickItem *display = fixture.view->item(QStringLiteral("boxDistributionDisplay"));
    QVERIFY(display);
    QVERIFY(display->isVisible());
    QQuickItem *status = fixture.view->item(QStringLiteral("studyStatusLabel"));
    QVERIFY(status);
    QVERIFY(!status->isVisible()); // only one status format appears at a time

    const QColor green(fixture.view->theme.green());
    const QColor muted(fixture.view->theme.muted());
    for (int leitnerBox = 1; leitnerBox <= Config::kLeitnerBoxes; ++leitnerBox) {
        QQuickItem *label = fixture.view->item(QStringLiteral("boxDistributionCount%1").arg(leitnerBox));
        QVERIFY2(label, qPrintable(QStringLiteral("boxDistributionCount%1 is missing").arg(leitnerBox)));
        QCOMPARE(label->property("text").toString(),
                 QStringLiteral("Box %1: %2").arg(leitnerBox).arg(counts.at(leitnerBox - 1).toInt()));
        QCOMPARE(label->property("color").value<QColor>(), leitnerBox == 1 ? green : muted);
    }

    QQuickItem *timing = fixture.view->item(QStringLiteral("cramNextInLabel"));
    QVERIFY(timing);
    QCOMPARE(timing->property("text").toString(), QStringLiteral("next in 1 cards"));
    QCOMPARE(timing->property("color").value<QColor>(), muted);

    QQuickItem *dot = fixture.view->item(QStringLiteral("studyFooterDot"));
    QVERIFY(dot);
    QCOMPARE(dot->property("color").value<QColor>(), green);
}

void TestBackend::qmlCramBoxDistributionFollowsRatings() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Q1", "A1"));
    QVERIFY(fixture.createCard("Q2", "A2"));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));

    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));

    QQuickItem *box1 = fixture.view->item(QStringLiteral("boxDistributionCount1"));
    QQuickItem *box2 = fixture.view->item(QStringLiteral("boxDistributionCount2"));
    QQuickItem *timing = fixture.view->item(QStringLiteral("cramNextInLabel"));
    QVERIFY(box1 && box2 && timing);
    const QColor green(fixture.view->theme.green());
    const QColor muted(fixture.view->theme.muted());

    QCOMPARE(box1->property("text").toString(), QStringLiteral("Box 1: 2"));
    QCOMPARE(box2->property("text").toString(), QStringLiteral("Box 2: 0"));
    QCOMPARE(box1->property("color").value<QColor>(), green);
    QCOMPARE(box2->property("color").value<QColor>(), muted);
    QCOMPARE(timing->property("text").toString(), QStringLiteral("next in 1 cards"));

    fixture.backend.reveal();
    fixture.backend.rate(3);
    QCOMPARE(box1->property("text").toString(), QStringLiteral("Box 1: 1"));
    QCOMPARE(box2->property("text").toString(), QStringLiteral("Box 2: 1"));
    QCOMPARE(box1->property("color").value<QColor>(), green);
    QCOMPARE(timing->property("text").toString(), QStringLiteral("next in 1 cards"));

    fixture.backend.reveal();
    fixture.backend.rate(3);
    QCOMPARE(box1->property("text").toString(), QStringLiteral("Box 1: 0"));
    QCOMPARE(box2->property("text").toString(), QStringLiteral("Box 2: 2"));
    QCOMPARE(box2->property("color").value<QColor>(), green);
    QCOMPARE(box1->property("color").value<QColor>(), muted);
    QCOMPARE(timing->property("text").toString(), QStringLiteral("next in 2 cards"));

    fixture.backend.reveal();
    fixture.backend.rate(1);
    QCOMPARE(box1->property("text").toString(), QStringLiteral("Box 1: 1"));
    QCOMPARE(box2->property("text").toString(), QStringLiteral("Box 2: 1"));
    QCOMPARE(box2->property("color").value<QColor>(), green); // the next Flashcard, not the reset one
    QCOMPARE(timing->property("text").toString(), QStringLiteral("next in 2 cards"));

    // A Deck change rebuilds the queue; no stale counts or highlight linger.
    QVERIFY(fixture.backend.createCard("Study", "Q3", "A3"));
    fixture.view->settleFileWatcher();
    QCOMPARE(fixture.backend.property("boxDistribution").toList(), QVariantList({3, 0, 0, 0, 0}));
    QCOMPARE(box1->property("text").toString(), QStringLiteral("Box 1: 3"));
    QCOMPARE(box2->property("text").toString(), QStringLiteral("Box 2: 0"));
    QCOMPARE(box1->property("color").value<QColor>(), green);
    QCOMPARE(timing->property("text").toString(), QStringLiteral("next in 1 cards"));
}

void TestBackend::qmlCramBoxDistributionIsCramOnlyAndHidesWhenEmpty() {
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Q1", "A1"));
    QVERIFY(fixture.createCard("Q2", "A2"));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("review"));

    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("review")));
    QQuickItem *display = fixture.view->item(QStringLiteral("boxDistributionDisplay"));
    QQuickItem *status = fixture.view->item(QStringLiteral("studyStatusLabel"));
    QVERIFY(display && status);
    QVERIFY(!display->isVisible());
    QVERIFY(status->isVisible());

    auto *window = fixture.window();
    QVERIFY(QMetaObject::invokeMethod(window, "startStudy", Q_ARG(QVariant, QStringLiteral("cram"))));
    fixture.settle();
    QVERIFY(display->isVisible());
    QVERIFY(!status->isVisible());

    QVERIFY(QMetaObject::invokeMethod(window, "startStudy", Q_ARG(QVariant, QStringLiteral("review"))));
    fixture.settle();
    QVERIFY(!display->isVisible());
    QVERIFY(status->isVisible());

    StudyViewFixture empty;
    QVERIFY(empty.initialize());
    QVERIFY(empty.createDeck());
    empty.selectDeck();
    empty.prepareMode(QStringLiteral("cram"));
    QVERIFY(empty.loadWindow());
    QVERIFY(empty.enterStudy(QStringLiteral("cram")));
    QVERIFY(!empty.backend.hasCard());
    QCOMPARE(empty.backend.property("boxDistribution").toList(), QVariantList({0, 0, 0, 0, 0}));
    QQuickItem *emptyDisplay = empty.view->item(QStringLiteral("boxDistributionDisplay"));
    QQuickItem *emptyStatus = empty.view->item(QStringLiteral("studyStatusLabel"));
    QVERIFY(emptyDisplay && emptyStatus);
    QVERIFY(!emptyDisplay->isVisible());
    QVERIFY(emptyStatus->isVisible());
    QVERIFY(emptyStatus->property("text").toString().contains(QStringLiteral("No cards")));
}

void TestBackend::qmlCramLeitnerBoxHighlightFollowsThemeChange() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("theme")));
    const QString colors = dir.filePath(QStringLiteral("theme/colors.toml"));
    QVERIFY(writeFile(colors, "green = \"#112233\"\n"));
    ScopedEnv guard("OMACARDS_THEME_DIR", dir.path());

    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Q1", "A1"));
    fixture.selectDeck();
    fixture.prepareMode(QStringLiteral("cram"));
    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));

    QCOMPARE(fixture.view->theme.green(), QString("#112233"));
    QQuickItem *highlight = fixture.view->item(QStringLiteral("boxDistributionCount1"));
    QQuickItem *dot = fixture.view->item(QStringLiteral("studyFooterDot"));
    QQuickItem *mutedCount = fixture.view->item(QStringLiteral("boxDistributionCount2"));
    QVERIFY(highlight && dot && mutedCount);
    QCOMPARE(highlight->property("color").value<QColor>(), QColor(QStringLiteral("#112233")));
    QCOMPARE(dot->property("color").value<QColor>(), QColor(QStringLiteral("#112233")));

    QVERIFY(writeFile(colors, "green = \"#445566\"\n"));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.view->theme.green() == QStringLiteral("#445566"), 3000);
    QCOMPARE(highlight->property("color").value<QColor>(), QColor(QStringLiteral("#445566")));
    QCOMPARE(dot->property("color").value<QColor>(), QColor(QStringLiteral("#445566")));
    QCOMPARE(mutedCount->property("color").value<QColor>(), QColor(fixture.view->theme.muted()));
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

    backend.reveal();
    backend.rate(3);
    QCOMPARE(backend.property("good").toInt(), 1);
    QCOMPARE(label->property("text").toString(), QString("1 reviewed · 1 Good"));

    QVERIFY(QMetaObject::invokeMethod(view.window, "openStats"));
    const QVariantMap stats = view.window->property("statsData").toMap();
    QCOMPARE(stats.value("sessionGood").toInt(), 1);
    QVERIFY(!stats.contains("sessionCorrect"));
    QCOMPARE(stats.value("studyDayRatings").toInt(), 1);
    QCOMPARE(stats.value("studyDayGood").toInt(), 1);
    QCOMPARE(stats.value("totalRatings").toInt(), 1);
    QVERIFY(!stats.contains("today"));
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

        backend.setNewCardAllowance(2);
        QCOMPARE(backend.availableCount(), 2);
        QCOMPARE(backend.dueCount(), 2);
        QCOMPARE(backend.reviewSummary().newCardsOnStudyDay, 0);

        backend.reveal();
        backend.rate(3);
        backend.reveal();
        backend.rate(3);
        QCOMPARE(backend.reviewSummary().newCardsOnStudyDay, 2);
        QCOMPARE(backend.availableCount(), 0);
        QCOMPARE(backend.dueCount(), 0);

        backend.setNewCardAllowance(3);
        QCOMPARE(backend.availableCount(), 1);
        backend.setDesiredRetention(0.85);
    }
    Backend reopened(dir.path(), database);
    QString error;
    QVERIFY(reopened.initialize(&error));
    QCOMPARE(reopened.newCardAllowance(), 3);
    QCOMPARE(reopened.desiredRetention(), 0.85);
}

void TestBackend::backendKeepsPendingRepeatInSameSession() {
    QTemporaryDir dir;
    ScopedEnv delay("OMACARDS_AGAIN_DELAY_MS", "20");
    {
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Repeats", &error));
        QVERIFY(backend.createCard("Repeats", "Q1", "A1", "basic", {}, &error));
        QVERIFY(backend.createCard("Repeats", "Q2", "A2", "basic", {}, &error));
        backend.selectDeck("Repeats");
        QVERIFY(backend.question().contains("Q1"));

        backend.reveal();
        backend.rate(1); // Again: Q1 waits, Q2 is shown next.
        QVERIFY(backend.hasCard());
        QVERIFY(backend.question().contains("Q2"));

        backend.reveal();
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

        backend.reveal();
        backend.rate(3);
        QVERIFY(!backend.hasCard());
    }
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
        backend.reveal();
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

    backend.setNewCardAllowance(3);
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
    ScopedEnv delay("OMACARDS_AGAIN_DELAY_MS", "50");
    {
        Backend backend(dir.path(), dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Repeats", &error));
        QVERIFY(backend.createCard("Repeats", "Q1", "A1", "basic", {}, &error));
        QVERIFY(backend.createCard("Repeats", "Q2", "A2", "basic", {}, &error));
        backend.selectDeck("Repeats");
        backend.reveal();
        backend.rate(1);
        QVERIFY(backend.question().contains("Q2"));

        backend.refresh(); // same path the file watcher uses
        QVERIFY(backend.question().contains("Q2"));
        backend.reveal();
        backend.rate(3);
        QTRY_VERIFY_WITH_TIMEOUT(backend.question().contains("Q1"), 2000);
    }
}

void TestBackend::deckSwitchStartsFreshSession() {
    QTemporaryDir dir;
    ScopedEnv delay("OMACARDS_AGAIN_DELAY_MS", "500");
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
        backend.reveal();
        backend.rate(1); // Q1 waits; Q2 is current.
        QVERIFY(backend.question().contains("Q2"));

        backend.selectDeck("Second");
        QVERIFY(backend.question().contains("Q3"));
        backend.selectDeck("First");
        // Switching back starts over: the Pending repeat and the counters from
        // the old sitting do not follow the learner to the new one.
        const QString freshQuestion = backend.question();
        QVERIFY(!freshQuestion.isEmpty());
        QCOMPARE(backend.reviewed(), 0);
        QCOMPARE(backend.good(), 0);
        QTest::qWait(700);
        QCOMPARE(backend.question(), freshQuestion);
    }
}

void TestBackend::deckSwitchKeepsModeAndResetsLeitnerBox() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("First", &error));
    QVERIFY(backend.createCard("First", "Q1", "A1", "basic", {}, &error));
    QVERIFY(backend.createDeck("Second", &error));
    QVERIFY(backend.createCard("Second", "Q3", "A3", "basic", {}, &error));

    backend.selectDeck("First");
    backend.setMode("cram");
    backend.reveal();
    backend.rate(3);
    QCOMPARE(backend.currentLeitnerBox(), 2);
    QCOMPARE(backend.reviewed(), 1);

    backend.selectDeck("Second");
    QCOMPARE(backend.mode(), QString("cram"));
    QCOMPARE(backend.currentLeitnerBox(), 1);
    QCOMPARE(backend.reviewed(), 0);
    QVERIFY(backend.question().contains("Q3"));
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
    backend.reveal();
    backend.rate(3);

    const QVariantMap summary = backend.statsSummary();
    QStringList keys = summary.keys();
    keys.sort();
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
    expected.sort();
    QCOMPARE(keys, expected);
    QCOMPARE(summary.value("cards").toInt(), backend.totalCards());
    QCOMPARE(summary.value("decks").toInt(), backend.decks().size());
    QCOMPARE(summary.value("due").toInt(), backend.dueCount());
    QCOMPARE(summary.value("sessionReviewed").toInt(), backend.reviewed());
    QCOMPARE(summary.value("sessionGood").toInt(), backend.good());
    QCOMPARE(backend.reviewSummary().total, 1); // one GUI rating appends one review
    QCOMPARE(summary.value("studyDayGood").toInt(), 1);
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
    backend.reveal();
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

    backend.reveal();
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
    backend.reveal();
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

void TestBackend::modeSwitchStopsRepeatTimerAndClearsPending() {
    QTemporaryDir dir;
    ScopedEnv delay("OMACARDS_AGAIN_DELAY_MS", "200");
    {
        // Keep deck files out of the state-db directory so a SQLite write
        // cannot trigger the deck watcher's debounced reload mid-test. Create
        // it first so initialize() does not seed the two-card Welcome deck.
        const QString decksDir = dir.filePath("decks");
        QVERIFY(QDir().mkpath(decksDir));
        Backend backend(decksDir, dir.filePath("state.db"));
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Atomic", &error));
        QVERIFY(backend.createCard("Atomic", "Q1", "A1", "basic", {}, &error));
        QVERIFY(backend.createCard("Atomic", "Q2", "A2", "basic", {}, &error));
        // Flush the watcher's 120 ms debounce before the repeat timer is armed.
        QTest::qWait(250);
        backend.selectDeck("Atomic");

        backend.reveal();
        backend.rate(1); // Again: Q1 Pending, Q2 presented
        QVERIFY(backend.question().contains("Q2"));
        QCOMPARE(backend.dueCount(), 2);

        // Backend no longer pairs setMode() with rebuild(); the switch alone
        // stops the timer, drops the Pending repeat and presents the new queue.
        backend.setMode("cram");
        QCOMPARE(backend.mode(), QString("cram"));
        QCOMPARE(backend.reviewed(), 0);
        QCOMPARE(backend.good(), 0);
        QVERIFY(!backend.waitingForRepeat());
        QCOMPARE(backend.dueCount(), 1);
        QVERIFY(backend.hasCard());
        QVERIFY(backend.status().contains("Cram"));

        QSignalSpy cardSpy(&backend, &Backend::cardChanged);
        QTest::qWait(400); // the old Review timer would have fired by now
        QCOMPARE(cardSpy.count(), 0);
    }
}

void TestBackend::qmlCramInjectedOrderPresentsExactNextFlashcard() {
    StudyViewFixture fixture([](const QVector<Card> &cards) { return cards; });
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard("Alpha", "A"));
    QVERIFY(fixture.createCard("Bravo", "B"));
    QVERIFY(fixture.createCard("Charlie", "C"));
    fixture.selectDeck();

    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("cram")));

    const QVector<Card> deckCards = fixture.backend.searchCards({}, QStringLiteral("Study"));
    QCOMPARE(deckCards.size(), 3);
    QCOMPARE(fixture.backend.mode(), QStringLiteral("cram"));
    QVERIFY(fixture.backend.status().contains(QStringLiteral("Cram")));
    QCOMPARE(fixture.backend.currentCardId(), deckCards.at(0).id);

    const auto rateGood = [&fixture] {
        QTest::keyClick(fixture.window(), Qt::Key_Space);
        fixture.settle();
        QTest::keyClick(fixture.window(), Qt::Key_2);
        fixture.settle();
    };
    rateGood();
    QCOMPARE(fixture.backend.currentCardId(), deckCards.at(1).id);
    rateGood();
    QCOMPARE(fixture.backend.currentCardId(), deckCards.at(2).id);
    rateGood();
    QCOMPARE(fixture.backend.currentCardId(), deckCards.at(0).id);
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

        backend.setNewCardAllowance(-5);
        QCOMPARE(backend.newCardAllowance(), 0);
        QCOMPARE(changed.count(), 1);
        backend.setNewCardAllowance(10000);
        QCOMPARE(backend.newCardAllowance(), Config::kMaxNewPerDay);
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
        QCOMPARE(reopened.newCardAllowance(), Config::kMaxNewPerDay);
        QCOMPARE(reopened.desiredRetention(), Config::kMaxRetention);
        QCOMPARE(reopened.dayStartHour(), Config::kMaxDayStartHour);
    }

    QTemporaryDir rawDir;
    const QString rawDatabase = rawDir.filePath("state.db");
    {
        Store store(rawDatabase);
        QString error;
        QVERIFY(store.open(&error));
        QVERIFY(store.setSetting("new_card_allowance", "100000"));
        QVERIFY(store.setSetting("desired_retention", "7.5"));
        QVERIFY(store.setSetting("day_start_hour", "77"));
    }
    Backend clamped(rawDir.path(), rawDatabase);
    QString error;
    QVERIFY(clamped.initialize(&error));
    QCOMPARE(clamped.newCardAllowance(), Config::kMaxNewPerDay);
    QCOMPARE(clamped.desiredRetention(), Config::kMaxRetention);
    QCOMPARE(clamped.dayStartHour(), Config::kMaxDayStartHour);
}

void TestBackend::legacyAllowanceSettingMigratesAndMirrorsWrites() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    const QString connection = QStringLiteral("omacards-backend-allowance-setup");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(database);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO settings (key, value) VALUES ('new_per_day', '4')")));
        db.close();
        db = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connection);

    Backend backend(dir.path(), database);
    QString error;
    QVERIFY(backend.initialize(&error));
    QCOMPARE(backend.newCardAllowance(), 4);

    backend.setNewCardAllowance(6);
    {
        Store reader(database);
        QVERIFY(reader.open(&error));
        QCOMPARE(reader.setting(QStringLiteral("new_card_allowance")), QStringLiteral("6"));
        QCOMPARE(reader.setting(QStringLiteral("new_per_day")), QStringLiteral("6"));
    }
    Backend reopened(dir.path(), database);
    QVERIFY(reopened.initialize(&error));
    QCOMPARE(reopened.newCardAllowance(), 6);
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
    QVERIFY(summaries.at(0).toMap().value("isCurrent").toBool());
    QVERIFY(!summaries.at(1).toMap().value("isCurrent").toBool());

    backend.selectDeck("Beta");
    summaries = backend.deckSummaries();
    QVERIFY(!summaries.at(0).toMap().value("isCurrent").toBool());
    QVERIFY(summaries.at(1).toMap().value("isCurrent").toBool());

    QCOMPARE(backend.availableCount("Alpha"), 2);
    QCOMPARE(backend.availableCount("Beta"), 1);
    QCOMPARE(backend.availableCount(), 3);

    QCOMPARE(backend.searchCards("question").size(), 2);
    QCOMPARE(backend.searchCards("question", "Alpha").size(), 1);
    QCOMPARE(backend.searchCards("").size(), 3);
    QCOMPARE(backend.searchCards("", "Beta").size(), 1);

    // The selector reorders the same summaries; the Library list keeps file order.
    const auto namesOf = [](const QVariantList &entries) {
        QStringList names;
        for (const QVariant &entry : entries)
            names << entry.toMap().value("name").toString();
        return names;
    };
    QCOMPARE(namesOf(backend.deckSummaries()), QStringList({"Alpha", "Beta"}));
    QCOMPARE(namesOf(backend.deckSelector()), QStringList({"Beta", "Alpha"}));
    const QVariantMap first = backend.deckSelector().first().toMap();
    QCOMPARE(first.value("cards").toInt(), 1);
    QCOMPARE(first.value("due").toInt(), 0); // a New card is not Due by day
}

void TestBackend::currentDeckAndRecencyPersistAcrossBackends() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    {
        Backend backend(dir.path(), database);
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Alpha", &error));
        QVERIFY(backend.createCard("Alpha", "Q1", "A1", "basic", {}, &error));
        QVERIFY(backend.createDeck("Beta", &error));
        QVERIFY(backend.createCard("Beta", "Q2", "A2", "basic", {}, &error));

        // Gamma is added behind the Backend so it has no last-studied record.
        const QString gamma = dir.filePath("Gamma.txt");
        QVERIFY(TestSupport::writeFile(gamma, "# Deck: Gamma\n"));
        backend.refresh();
        QCOMPARE(backend.decks(), QStringList({"Alpha", "Beta", "Gamma"}));

        backend.selectDeck("Beta");
        backend.selectDeck("Alpha");
        const auto namesOf = [](const QVariantList &entries) {
            QStringList names;
            for (const QVariant &entry : entries)
                names << entry.toMap().value("name").toString();
            return names;
        };
        QCOMPARE(namesOf(backend.deckSelector()), QStringList({"Alpha", "Beta", "Gamma"}));
    }
    {
        Backend reopened(dir.path(), database);
        QString error;
        QVERIFY(reopened.initialize(&error));
        QCOMPARE(reopened.currentDeck(), QString("Alpha"));
        const auto namesOf = [](const QVariantList &entries) {
            QStringList names;
            for (const QVariant &entry : entries)
                names << entry.toMap().value("name").toString();
            return names;
        };
        QCOMPARE(namesOf(reopened.deckSelector()), QStringList({"Alpha", "Beta", "Gamma"}));
    }
}

void TestBackend::selectionUpdatesRecencyOnlyOnExplicitChoice() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(backend.createDeck("Alpha", &error));
    QVERIFY(backend.createCard("Alpha", "Q1", "A1", "basic", {}, &error));
    QVERIFY(backend.createDeck("Beta", &error));
    QVERIFY(backend.createCard("Beta", "Q2", "A2", "basic", {}, &error));

    const auto namesOf = [&]() {
        QStringList names;
        for (const QVariant &entry : backend.deckSelector())
            names << entry.toMap().value("name").toString();
        return names;
    };
    backend.selectDeck("Alpha");
    backend.selectDeck("Beta");
    const QStringList afterSelection{"Beta", "Alpha"};
    QCOMPARE(namesOf(), afterSelection);

    backend.setMode("cram");
    backend.reveal();
    backend.rate(3);
    backend.refresh();
    QCOMPARE(namesOf(), afterSelection);
}

void TestBackend::missingDeckFallbackPersistsAndSelectorAgrees() {
    QTemporaryDir dir;
    const QString database = dir.filePath("state.db");
    {
        Backend backend(dir.path(), database);
        QString error;
        QVERIFY(backend.initialize(&error));
        QVERIFY(backend.createDeck("Alpha", &error));
        QVERIFY(backend.createDeck("Beta", &error));
        backend.selectDeck("Beta");
        QCOMPARE(backend.currentDeck(), QString("Beta"));

        QString betaPath;
        for (const Deck &deck : backend.allDecks())
            if (deck.name == QStringLiteral("Beta"))
                betaPath = deck.path;
        QVERIFY(!betaPath.isEmpty());
        QVERIFY(QFile::remove(betaPath));
        backend.refresh();
        QCOMPARE(backend.currentDeck(), QString("Alpha"));
        QCOMPARE(backend.deckSelector().first().toMap().value("name").toString(), QString("Alpha"));
    }
    {
        Backend reopened(dir.path(), database);
        QString error;
        QVERIFY(reopened.initialize(&error));
        QCOMPARE(reopened.currentDeck(), QString("Alpha"));
    }
}

void TestBackend::emptySessionHelpersAndStatsText() {
    QTemporaryDir dir;
    Backend backend(dir.path(), dir.filePath("state.db"));
    QString error;
    QVERIFY(backend.initialize(&error));
    QVERIFY(!backend.hasCard());

    QSignalSpy cardChanged(&backend, &Backend::cardChanged);
    QSignalSpy statusChanged(&backend, &Backend::statusChanged);
    backend.reveal();
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
    backend.reveal();
    backend.rate(3);
    QCOMPARE(backend.reviewed(), 1);
    QCOMPARE(backend.good(), 1);
    QCOMPARE(backend.currentLeitnerBox(), 2);
    QCOMPARE(backend.statsText(), QString("1 reviewed · 100% Good"));
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

void TestBackend::qmlStudyShowsMathPlaceholderUntilSvgArrives() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    // normalizeCachedSvg() supersamples the root to 120px x 48px and
    // svgPixelSize() halves it again, for a 60 x 24 logical size. At the
    // question scale the finished image would be 120 x 48 if it were sized
    // from the SVG instead of the reserved box.
    const QSize svgImageSize(120, 48);
    const QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"60px\" height=\"24px\" "
                                       "viewBox=\"0 0 60 24\"><g fill=\"rgb(0%, 0%, 0%)\"/></svg>");
    QVERIFY(writeFakePdfToolchain(bin, svg, QStringLiteral("sleep 1\n")));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    PathGuard pathGuard(bin);
    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard(QStringLiteral("What is \\(f'''(x)\\)?"), QStringLiteral("plain answer")));
    fixture.selectDeck();
    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("review")));

    auto *question = fixture.item(QStringLiteral("studyQuestionLabel"));
    auto *answer = fixture.item(QStringLiteral("studyAnswerLabel"));
    QVERIFY(question);
    QVERIFY(answer);
    fixture.backend.reveal();
    fixture.settle();
    QVERIFY(answer->isVisible());

    const QString text = question->property("text").toString();
    QVERIFY2(text.contains(QStringLiteral("<!--omacards-math:")), qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    QVERIFY(!text.contains(QStringLiteral("font-family: monospace")));
    // The font-metric reservation must differ from the SVG's eventual size;
    // otherwise the fixture would hide the bug again.
    const QSize reserved = parsedMathImageSize(text);
    QVERIFY(!reserved.isEmpty());
    QVERIFY(reserved != svgImageSize);

    const QSizeF questionSizeBefore(question->property("contentWidth").toReal(),
                                    question->property("contentHeight").toReal());
    const qreal answerTopBefore = answer->mapRectToScene(answer->boundingRect()).top();

    QTRY_VERIFY_WITH_TIMEOUT(!question->property("text").toString().contains(QStringLiteral("<!--omacards-math:")),
                             5000);
    const QString swapped = question->property("text").toString();
    QVERIFY(swapped.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    QVERIFY(svgFromMathHtml(swapped).contains(QStringLiteral("width=\"120px\" height=\"48px\"")));
    // The apostrophes used to close the alt attribute early, so Qt parsed the
    // finished image as 0x0 and the SVG took its intrinsic size. It must parse
    // back to the box the placeholder reserved.
    QCOMPARE(parsedMathImageSize(swapped), reserved);

    const QSizeF questionSizeAfter(question->property("contentWidth").toReal(),
                                   question->property("contentHeight").toReal());
    QCOMPARE(questionSizeAfter, questionSizeBefore);
    QVERIFY(qAbs(answer->mapRectToScene(answer->boundingRect()).top() - answerTopBefore) < 1.0);
}

void TestBackend::qmlStudyRecolorsCachedSvgWhenThemeChanges() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache/omacards/math");
    const QString argsLog = dir.filePath("args.log");
    const QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" "
                                       "viewBox=\"0 0 10 5\"><g fill=\"rgb(0%, 0%, 0%)\"/></svg>");
    QVERIFY(writeFakePdfToolchain(bin, svg));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    TestSupport::ScopedEnv argsEnv("OMACARDS_TEST_ARGS", argsLog);
    PathGuard pathGuard(bin);
    MathRenderer prewarm(cache);
    QVERIFY(prewarm.hasRenderer());
    QVERIFY(prewarm.renderExpression("x^2", QStringLiteral("#112233"), MathScale::kQuestion)
                .startsWith(QStringLiteral("<img ")));
    QVERIFY(prewarm.renderExpression("y^2", QStringLiteral("#112233"), MathScale::kQuestion)
                .startsWith(QStringLiteral("<img ")));
    const QStringList filesBefore = QDir(cache).entryList({"*.svg"});

    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard(QStringLiteral("What is \\(x^2\\)?"), QStringLiteral("plain answer")));
    QVERIFY(fixture.createCard(QStringLiteral("Next \\(y^2\\)"), QStringLiteral("plain answer")));
    fixture.backend.setMathColors(QStringLiteral("#112233"), QStringLiteral("#445566"));
    fixture.selectDeck();
    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("review")));

    auto *question = fixture.item(QStringLiteral("studyQuestionLabel"));
    QVERIFY(question);
    const QString before = question->property("text").toString();
    QVERIFY(before.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    QVERIFY(svgFromMathHtml(before).contains(QStringLiteral("fill=\"#112233\"")));
    const int rendersBefore = toolchainRenders(argsLog);

    fixture.backend.setMathColors(QStringLiteral("#abcdef"), QStringLiteral("#123456"));
    fixture.settle();
    const QString after = question->property("text").toString();
    QVERIFY(after.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    QVERIFY(!after.contains(QStringLiteral("<!--omacards-math:")));
    QVERIFY(svgFromMathHtml(after).contains(QStringLiteral("fill=\"#abcdef\"")));
    QVERIFY(!svgFromMathHtml(after).contains(QStringLiteral("fill=\"#112233\"")));

    QCOMPARE(toolchainRenders(argsLog), rendersBefore);
    QCOMPARE(QDir(cache).entryList({"*.svg"}), filesBefore);
}

void TestBackend::qmlStudyUsesCachedSvgWithoutPlaceholder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bin = dir.filePath("bin");
    const QString cache = dir.filePath("cache/omacards/math");
    const QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10pt\" height=\"5pt\" "
                                       "viewBox=\"0 0 10 5\"><g fill=\"rgb(0%, 0%, 0%)\"/></svg>");
    QVERIFY(writeFakePdfToolchain(bin, svg));

    TestSupport::ScopedEnv cacheEnv("XDG_CACHE_HOME", dir.filePath("cache"));
    PathGuard pathGuard(bin);
    MathRenderer prewarm(cache);
    QVERIFY(prewarm.hasRenderer());
    QVERIFY(prewarm.renderExpression("x^2", QStringLiteral("#000000"), MathScale::kQuestion)
                .startsWith(QStringLiteral("<img ")));

    StudyViewFixture fixture;
    QVERIFY(fixture.initialize());
    QVERIFY(fixture.createDeck());
    QVERIFY(fixture.createCard(QStringLiteral("What is \\(x^2\\)?"), QStringLiteral("plain answer")));
    fixture.selectDeck();
    QVERIFY(fixture.loadWindow());
    QVERIFY(fixture.enterStudy(QStringLiteral("review")));

    auto *question = fixture.item(QStringLiteral("studyQuestionLabel"));
    QVERIFY(question);
    const QString text = question->property("text").toString();
    QVERIFY(text.contains(QStringLiteral("<img src='data:image/svg+xml;base64,")));
    QVERIFY(!text.contains(QStringLiteral("<!--omacards-math:")));
}
