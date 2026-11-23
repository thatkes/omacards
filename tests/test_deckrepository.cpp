#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <algorithm>

#include "test_deckrepository.h"
#include "deck.h"
#include "deckrepository.h"
#include "domain.h"
#include "filehistory.h"
#include "fsrs.h"
#include "store.h"

namespace {

bool writeText(const QString &path, const QByteArray &contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

struct RepoFixture {
    QTemporaryDir dir;
    Store store;
    FileHistory history;
    DeckRepository repo;
    QString current;

    RepoFixture() : store(dir.filePath("state.db")), repo(dir.filePath("decks"), store, history) {}
};

bool setup(RepoFixture &f) {
    QString error;
    if (!f.repo.initialize(&error))
        return false;
    if (!f.store.open(&error))
        return false;
    f.repo.load(&f.current);
    return true;
}

} // namespace

void TestDeckRepository::initializeCreatesWelcomeDeckOnFreshDirectory() {
    RepoFixture f;
    QString error;
    QVERIFY(f.repo.initialize(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo::exists(f.dir.filePath("decks/Welcome.txt")));

    QString loadError;
    const Deck welcome = DeckIO::load(f.dir.filePath("decks/Welcome.txt"), &loadError);
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
    QCOMPARE(welcome.name, QString("Welcome"));
    QCOMPARE(welcome.cards.size(), 2);
    QVERIFY(welcome.cards.at(0).question.contains(QStringLiteral("study modes")));
    QVERIFY(welcome.cards.at(0).answer.contains(QStringLiteral("FSRS review")));
    QVERIFY(welcome.cards.at(1).question.contains(QStringLiteral("keyboard shortcut")));
    QVERIFY(welcome.cards.at(1).answer.contains(QStringLiteral("Press Space")));
    for (const Card &card : welcome.cards) {
        QCOMPARE(card.deck, QString("Welcome"));
        QVERIFY(!card.id.isEmpty());
    }

    // initialize() only writes the file; the in-memory view arrives via load().
    QVERIFY(f.repo.deckNames().isEmpty());
    f.repo.load(&f.current);
    QCOMPARE(f.repo.deckNames(), QStringList({"Welcome"}));
    QCOMPARE(f.current, QString("Welcome"));
}

void TestDeckRepository::initializeDoesNotOverwriteExistingDeckDirectory() {
    RepoFixture f;
    const QString decks = f.dir.filePath("decks");
    QVERIFY(QDir().mkpath(decks));
    const QString path = QDir(decks).filePath("Custom.txt");
    const QByteArray contents = "# Deck: Custom\n---\nQ: Keep me\nA: Please\n";
    QVERIFY(writeText(path, contents));

    QString error;
    QVERIFY(f.repo.initialize(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFileInfo::exists(QDir(decks).filePath("Welcome.txt")));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
}

void TestDeckRepository::createDeckRejectsEmptyAndDuplicateNames() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QString current = f.current;

    QVERIFY(!f.repo.createDeck("", &current, &error));
    QVERIFY(error.contains(QStringLiteral("cannot be empty")));
    error.clear();
    QVERIFY(!f.repo.createDeck("   \t ", &current, &error));
    QVERIFY(error.contains(QStringLiteral("cannot be empty")));

    error.clear();
    QVERIFY(f.repo.createDeck("Dup", &current, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(current, QString("Dup"));
    QVERIFY(!f.repo.createDeck("Dup", &current, &error));
    QVERIFY(error.contains(QStringLiteral("already exists")));
    QCOMPARE(f.repo.deckNames().count(QString("Dup")), 1);
}

void TestDeckRepository::createDeckRejectsSanitizedPathCollision() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("A/B", &f.current, &error));
    QVERIFY(!f.repo.createDeck("A-B", &f.current, &error));
    QVERIFY(error.contains(QStringLiteral("Deck file already used by A/B")));
    QCOMPARE(f.repo.deckNames().count(QString("A-B")), 0);
    QCOMPARE(f.repo.deckNames().count(QString("A/B")), 1);
}

void TestDeckRepository::createDeckRejectsExistingUntrackedFile() {
    RepoFixture f;
    QVERIFY(setup(f));
    const QString path = f.dir.filePath("decks/Taken.txt");
    const QByteArray contents = "not a deck\n";
    QVERIFY(writeText(path, contents));

    QString error;
    QVERIFY(!f.repo.createDeck("Taken", &f.current, &error));
    QVERIFY(error.contains(QStringLiteral("A deck file already exists at")));
    QVERIFY(!f.repo.deckByName("Taken"));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
}

void TestDeckRepository::createDeckRejectsInvalidSerializedDeck() {
    RepoFixture f;
    QVERIFY(setup(f));
    const QString badName = QStringLiteral("Broken\n---\nA: orphan");
    const QString path = QDir(f.dir.filePath("decks")).filePath(DeckIO::safeFileName(badName) + ".txt");

    QString error;
    QVERIFY(!f.repo.createDeck(badName, &f.current, &error));
    QVERIFY(error.contains(QStringLiteral("Deck would be invalid after write")));
    QVERIFY(error.contains(QStringLiteral("Hint:")));
    QVERIFY(error.contains(QStringLiteral("Fix the format")));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(!f.repo.deckByName(badName));
    QVERIFY(!f.history.canUndo());
}

void TestDeckRepository::createDeckTrimsDescription() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Trim", &f.current, &error, "  spaced description  "));
    const Deck *deck = f.repo.deckByName("Trim");
    QVERIFY(deck);
    QCOMPARE(deck->description, QString("spaced description"));
}

void TestDeckRepository::createCardsRejectsEmptyTargetAndDrafts() {
    RepoFixture f;
    QVERIFY(setup(f));
    CardDraft draft;
    draft.question = "Q";
    draft.answer = "A";
    QString error;
    QString current = f.current;

    // An empty name falls back to currentDeck; only when both are empty is
    // the operation rejected.
    QVERIFY(f.repo.createCards("", &current, {draft}, nullptr, nullptr, &error));
    error.clear();
    current.clear();
    QVERIFY(!f.repo.createCards("", &current, {draft}, nullptr, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("Give a deck name")));

    error.clear();
    QVERIFY(!f.repo.createCards("New", &current, {}, nullptr, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("No cards to add")));
    QVERIFY(!f.repo.deckByName("New"));
    QVERIFY(!QFileInfo::exists(f.dir.filePath("decks/New.txt")));
}

void TestDeckRepository::createCardsReportsInvalidDraftIndex() {
    RepoFixture f;
    QVERIFY(setup(f));
    QVector<CardDraft> drafts(3);
    drafts[0].question = "ok";
    drafts[0].answer = "a";
    drafts[1].question = "  ";
    drafts[1].answer = "a";
    drafts[2].question = "q";
    drafts[2].answer = "a";
    drafts[2].type = "bogus";

    QString error;
    QVERIFY(!f.repo.createCards("Invalid", &f.current, drafts, nullptr, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("cards[1] needs a question")));
    QVERIFY(!QFileInfo::exists(f.dir.filePath("decks/Invalid.txt")));

    error.clear();
    drafts[1].question = "fixed";
    QVERIFY(!f.repo.createCards("Invalid", &f.current, drafts, nullptr, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("cards[2] has an invalid type: bogus")));
    QVERIFY(!QFileInfo::exists(f.dir.filePath("decks/Invalid.txt")));
}

void TestDeckRepository::createCardsCreatesDeckWithDescription() {
    RepoFixture f;
    QVERIFY(setup(f));
    CardDraft draft;
    draft.question = "Q";
    draft.answer = "A";
    QString description = "  bulk description  ";
    QStringList createdIds;
    QString error;

    QVERIFY(f.repo.createCards("Bulk", &f.current, {draft}, &description, &createdIds, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(f.current, QString("Bulk"));
    const Deck *deck = f.repo.deckByName("Bulk");
    QVERIFY(deck);
    QCOMPARE(deck->description, QString("bulk description"));
    QCOMPARE(deck->cards.size(), 1);
    QCOMPARE(createdIds.size(), 1);
}

void TestDeckRepository::createCardsKeepsExistingDescription() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Keep", &f.current, &error, "Original"));

    CardDraft first;
    first.question = "Q1";
    first.answer = "A1";
    QString emptyDescription;
    QVERIFY(f.repo.createCards("Keep", &f.current, {first}, &emptyDescription, nullptr, &error));
    QCOMPARE(f.repo.deckByName("Keep")->description, QString("Original"));

    CardDraft second;
    second.question = "Q2";
    second.answer = "A2";
    QVERIFY(f.repo.createCards("Keep", &f.current, {second}, nullptr, nullptr, &error));
    QCOMPARE(f.repo.deckByName("Keep")->description, QString("Original"));
    QCOMPARE(f.repo.deckByName("Keep")->cards.size(), 2);
}

void TestDeckRepository::createCardsDeduplicatesTagsAndGeneratesIds() {
    RepoFixture f;
    QVERIFY(setup(f));
    CardDraft draft;
    draft.question = "Q";
    draft.answer = "A";
    draft.tags = {"x", " x ", "x", "", "y"};
    QStringList createdIds;
    QString error;

    QVERIFY(f.repo.createCards("Tags", &f.current, {draft}, nullptr, &createdIds, &error));
    const Deck *deck = f.repo.deckByName("Tags");
    QVERIFY(deck);
    QCOMPARE(deck->cards.size(), 1);
    const Card &card = deck->cards.first();
    QCOMPARE(card.tags, QStringList({"x", "y"}));
    QVERIFY(!card.explicitId);
    QCOMPARE(card.id, DeckIO::cardId(card));
    QCOMPARE(createdIds, QStringList({card.id}));
}

void TestDeckRepository::createCardsCanonicalizesAndRejectsTypes() {
    RepoFixture f;
    QVERIFY(setup(f));
    CardDraft basic;
    basic.question = "B";
    basic.answer = "A";
    basic.type = "basic";
    CardDraft empty;
    empty.question = "E";
    empty.answer = "A";
    empty.type = "";
    CardDraft cloze;
    cloze.question = "C";
    cloze.answer = "A";
    cloze.type = "cloze";

    QString error;
    QVERIFY(f.repo.createCards("Types", &f.current, {basic, empty, cloze}, nullptr, nullptr, &error));
    const Deck *deck = f.repo.deckByName("Types");
    QVERIFY(deck);
    QCOMPARE(deck->cards.size(), 3);
    QCOMPARE(deck->cards.at(0).type.kind, CardType::Basic);
    QCOMPARE(deck->cards.at(1).type.kind, CardType::Basic);
    QCOMPARE(deck->cards.at(2).type.kind, CardType::Cloze);
}

void TestDeckRepository::createCardRequiresExistingDeckAndSplitsTags() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(!f.repo.createCard("Ghost", &f.current, "Q", "A", "basic", "a,b", &error));
    QVERIFY(error.contains(QStringLiteral("No such deck: Ghost")));
    QVERIFY(!QFileInfo::exists(f.dir.filePath("decks/Ghost.txt")));

    error.clear();
    QVERIFY(f.repo.createDeck("Real", &f.current, &error));
    QVERIFY(f.repo.createCard("Real", &f.current, "Q", "A", "basic", " a, b ,,c ", &error));
    QVERIFY(error.isEmpty());
    const Deck *deck = f.repo.deckByName("Real");
    QVERIFY(deck);
    QCOMPARE(deck->cards.size(), 1);
    QCOMPARE(deck->cards.first().tags, QStringList({"a", "b", "c"}));
}

void TestDeckRepository::modifyCardRejectsUnknownDeckAndCard() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("D", &f.current, &error));
    QVERIFY(f.repo.createCard("D", &f.current, "Q", "A", "basic", {}, &error));
    QString finalId;

    QVERIFY(!f.repo.modifyCard("Ghost", "id", "Q", "A", "basic", {}, &error, &finalId));
    QVERIFY(error.contains(QStringLiteral("No such deck: Ghost")));
    error.clear();
    QVERIFY(!f.repo.modifyCard("D", "missing-id", "Q", "A", "basic", {}, &error, &finalId));
    QVERIFY(error.contains(QStringLiteral("No such card: missing-id")));
}

void TestDeckRepository::modifyCardRejectsEmptyQuestionAndNormalizesType() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("D", &f.current, &error));
    QVERIFY(f.repo.createCard("D", &f.current, "Q", "A", "basic", {}, &error));
    const QString id = f.repo.deckByName("D")->cards.first().id;

    QVERIFY(!f.repo.modifyCard("D", id, "  ", "A", "basic", {}, &error));
    QVERIFY(error.contains(QStringLiteral("A card needs a question")));
    QCOMPARE(f.repo.deckByName("D")->cards.first().question, QString("Q"));

    error.clear();
    QString finalId;
    QVERIFY(f.repo.modifyCard("D", id, "New", "A", "nonsense", {}, &error, &finalId));
    QVERIFY(error.isEmpty());
    QCOMPARE(f.repo.deckByName("D")->cards.first().type.kind, CardType::Basic);
    QVERIFY(!finalId.isEmpty());
}

void TestDeckRepository::modifyCardHandlesExplicitAndGeneratedIds() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("D", &f.current, &error));
    QVERIFY(f.repo.createCard("D", &f.current, "Original", "A", "basic", {}, &error));
    const QString oldId = f.repo.deckByName("D")->cards.first().id;

    QString finalId;
    QVERIFY(f.repo.modifyCard("D", oldId, "Changed", "A", "basic", {}, &error, &finalId));
    QCOMPARE(finalId, oldId);
    QCOMPARE(f.repo.deckByName("D")->cards.first().id, oldId);

    // The id written on edit survives a reload and still resolves later.
    f.repo.load(&f.current);
    QCOMPARE(f.repo.deckByName("D")->cards.first().id, oldId);
    finalId.clear();
    QVERIFY(f.repo.modifyCard("D", oldId, "Changed again", "A", "basic", {}, &error, &finalId));
    QCOMPARE(finalId, oldId);

    const QString pinnedPath = QDir(f.dir.filePath("decks")).filePath("Pinned.txt");
    QVERIFY(writeText(pinnedPath, "# Deck: Pinned\n---\nQ: Fixed\nA: Answer\nid: pinned-123\n"));
    f.repo.load(&f.current);
    const Deck *pinned = f.repo.deckByName("Pinned");
    QVERIFY(pinned);
    QCOMPARE(pinned->cards.size(), 1);
    QVERIFY(pinned->cards.first().explicitId);
    QCOMPARE(pinned->cards.first().id, QString("pinned-123"));

    finalId.clear();
    QVERIFY(f.repo.modifyCard("Pinned", "pinned-123", "Fixed edited", "Answer", "basic", {}, &error, &finalId));
    const Card &pinnedAfter = f.repo.deckByName("Pinned")->cards.first();
    QCOMPARE(pinnedAfter.id, QString("pinned-123"));
    QVERIFY(pinnedAfter.explicitId);
    QCOMPARE(finalId, QString("pinned-123"));

    // Renaming a Deck changes the name that generated ids hash. The rename
    // pins generated ids, so Scheduling state under the old ids still resolves.
    QVERIFY(f.repo.createDeck("Old", &f.current, &error));
    QVERIFY(f.repo.createCard("Old", &f.current, "Keep Q", "Keep A", "basic", {}, &error));
    const Card generated = f.repo.deckByName("Old")->cards.first();
    const QString renamedDeckId = generated.id;
    FsrsState state;
    state.reps = 3;
    f.store.save(renamedDeckId, DeckIO::contentHash(generated), state);
    QVERIFY(f.store.states().contains(renamedDeckId));

    // Settings are keyed by Deck name, so they must follow the rename too.
    QVERIFY(f.store.setCurrentDeck(QStringLiteral("Old")));
    QVERIFY(f.store.setDeckLastStudied(QStringLiteral("Welcome"), 100));
    QVERIFY(f.store.setDeckLastStudied(QStringLiteral("Old"), 200));

    DeckRename::Result renamed;
    QVERIFY(f.repo.renameDeck("Old", "New", &f.current, &renamed, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(renamed.name, QString("New"));
    QCOMPARE(renamed.path, QDir(f.dir.filePath("decks")).filePath("New.txt"));
    QCOMPARE(renamed.cards, 1);
    QCOMPARE(renamed.preservedIds, 1);
    QVERIFY(!QFileInfo::exists(f.dir.filePath("decks/Old.txt")));
    QVERIFY(QFileInfo::exists(renamed.path));
    QCOMPARE(f.current, QString("New"));
    QCOMPARE(f.store.currentDeck(), QString("New"));
    const QHash<QString, qint64> studied = f.store.deckLastStudied();
    QVERIFY(!studied.contains(QStringLiteral("Old")));
    QCOMPARE(studied.value(QStringLiteral("New")), qint64(200));
    QCOMPARE(studied.value(QStringLiteral("Welcome")), qint64(100));

    const Deck *moved = f.repo.deckByName("New");
    QVERIFY(moved);
    QCOMPARE(moved->name, QString("New"));
    QCOMPARE(moved->cards.first().deck, QString("New"));
    QCOMPARE(moved->cards.first().id, renamedDeckId);
    QVERIFY(moved->cards.first().explicitId);

    f.repo.load(&f.current);
    moved = f.repo.deckByName("New");
    QVERIFY(moved);
    QCOMPARE(moved->cards.first().id, renamedDeckId);
    QVERIFY(moved->cards.first().explicitId);
    QVERIFY(f.store.states().contains(renamedDeckId));
    QCOMPARE(f.store.states().value(renamedDeckId).reps, 3);

    // An id already pinned in the file is preserved without a second pin.
    DeckRename::Result pinnedRename;
    QVERIFY(f.repo.renameDeck("Pinned", "Pinned New", &f.current, &pinnedRename, &error));
    QCOMPARE(pinnedRename.cards, 1);
    QCOMPARE(pinnedRename.preservedIds, 0);
    QCOMPARE(f.repo.deckByName("Pinned New")->cards.first().id, QString("pinned-123"));

    error.clear();
    QVERIFY(!f.repo.renameDeck("New", "Pinned New", &f.current, &renamed, &error));
    QCOMPARE(error, QString("Deck already exists: Pinned New"));
    QVERIFY(f.repo.deckByName("New"));

    // Undo and redo cover both files as one rename step.
    QString description;
    QVERIFY(f.repo.undo(&error, &description));
    f.repo.load(&f.current);
    QVERIFY(f.repo.deckByName("New"));
    QVERIFY(!f.repo.deckByName("Pinned New"));
    QVERIFY(QFileInfo::exists(f.dir.filePath("decks/Pinned.txt")));
    QVERIFY(f.repo.redo(&error, &description));
    f.repo.load(&f.current);
    QVERIFY(f.repo.deckByName("Pinned New"));
    QVERIFY(!f.repo.deckByName("Pinned"));
}

void TestDeckRepository::modifyCardTrimsAndDeduplicatesTags() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("D", &f.current, &error));
    QVERIFY(f.repo.createCard("D", &f.current, "Q", "A", "basic", "old", &error));
    const QString id = f.repo.deckByName("D")->cards.first().id;

    QVERIFY(f.repo.modifyCard("D", id, " Q ", " A ", "basic", " a, a ,, b ", &error));
    const Card &card = f.repo.deckByName("D")->cards.first();
    QCOMPARE(card.question, QString("Q"));
    QCOMPARE(card.answer, QString("A"));
    QCOMPARE(card.tags, QStringList({"a", "b"}));
}

void TestDeckRepository::deleteAndRemoveRejectUnknownNames() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("D", &f.current, &error));

    QVERIFY(!f.repo.deleteCard("Ghost", "x", &error));
    QVERIFY(error.contains(QStringLiteral("No such deck: Ghost")));
    error.clear();
    QVERIFY(!f.repo.deleteCard("D", "x", &error));
    QVERIFY(error.contains(QStringLiteral("No such card: x")));
    error.clear();
    QVERIFY(!f.repo.removeDeck("Ghost", &f.current, &error));
    QVERIFY(error.contains(QStringLiteral("No such deck: Ghost")));
}

void TestDeckRepository::removeDeckClearsCurrentAndPrunesOnReload() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Gone", &f.current, &error));
    QVERIFY(f.repo.createDeck("Kept", &f.current, &error));
    QVERIFY(f.repo.createCard("Gone", &f.current, "QG", "AG", "basic", {}, &error));
    QVERIFY(f.repo.createCard("Kept", &f.current, "QK", "AK", "basic", {}, &error));
    const Card gone = f.repo.deckByName("Gone")->cards.first();
    const Card kept = f.repo.deckByName("Kept")->cards.first();
    FsrsState state;
    state.reps = 3;
    f.store.save(gone.id, DeckIO::contentHash(gone), state);
    f.store.save(kept.id, DeckIO::contentHash(kept), state);
    QVERIFY(f.store.states().contains(gone.id));

    f.current = "Gone";
    QVERIFY(f.repo.removeDeck("Gone", &f.current, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(f.current.isEmpty());
    QVERIFY(!QFileInfo::exists(f.dir.filePath("decks/Gone.txt")));

    // removeDeck() still sees the old in-memory deck list, so its own pruning
    // keeps the removed card's sidecar row until the following clean load.
    QVERIFY(f.store.states().contains(gone.id));

    f.repo.load(&f.current);
    QCOMPARE(f.current, QString("Kept"));
    const auto states = f.store.states();
    QVERIFY(!states.contains(gone.id));
    QVERIFY(states.contains(kept.id));
}

void TestDeckRepository::removeDeckKeepsUnrelatedCurrent() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("A", &f.current, &error));
    QVERIFY(f.repo.createDeck("B", &f.current, &error));
    f.current = "B";
    QVERIFY(f.repo.removeDeck("A", &f.current, &error));
    QCOMPARE(f.current, QString("B"));
    QVERIFY(!QFileInfo::exists(f.dir.filePath("decks/A.txt")));
}

void TestDeckRepository::setDeckDescriptionTrimsAndSkipsNoOp() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Desc", &f.current, &error, "Original"));
    QVERIFY(f.repo.setDeckDescription("Desc", "  Updated  ", &error));
    QCOMPARE(f.repo.deckByName("Desc")->description, QString("Updated"));

    QSignalSpy historySpy(&f.repo, &DeckRepository::historyChanged);
    QVERIFY(f.repo.setDeckDescription("Desc", " Updated ", &error));
    QCOMPARE(historySpy.count(), 0);

    error.clear();
    QVERIFY(!f.repo.setDeckDescription("Ghost", "x", &error));
    QVERIFY(error.contains(QStringLiteral("No such deck: Ghost")));
}

void TestDeckRepository::setDeckDescriptionRejectsInvalidSerializedDeck() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Desc", &f.current, &error, "Original"));
    const QString path = f.repo.deckPath("Desc");
    QVERIFY(!path.isEmpty());
    const QByteArray before = FileHistory::readBytes(path);
    QVERIFY(DeckIO::lintFile(path).isEmpty());

    QSignalSpy historySpy(&f.repo, &DeckRepository::historyChanged);
    QVERIFY(!f.repo.setDeckDescription("Desc", "Original\n---\nA: orphan", &error));
    QVERIFY(error.contains(QStringLiteral("Deck would be invalid after write")));
    QVERIFY(error.contains(QStringLiteral("Hint:")));
    QVERIFY(error.contains(QStringLiteral("Fix the format")));
    QCOMPARE(FileHistory::readBytes(path), before);
    QCOMPARE(f.repo.deckByName("Desc")->description, QStringLiteral("Original"));
    QVERIFY(DeckIO::lintFile(path).isEmpty());
    QCOMPARE(historySpy.count(), 0);
}

void TestDeckRepository::writeAllowsPreExistingWarnings() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Warnings", &f.current, &error));

    CardDraft duplicate;
    duplicate.question = "Same";
    duplicate.answer = "A";
    QVector<CardDraft> drafts{duplicate, duplicate};
    QVERIFY(f.repo.createCards("Warnings", &f.current, drafts, nullptr, nullptr, &error));
    const QString path = f.repo.deckPath("Warnings");
    const QVector<DeckIssue> before = DeckIO::lintFile(path);
    QVERIFY(!before.isEmpty());
    QVERIFY(std::any_of(before.cbegin(), before.cend(), [](const DeckIssue &issue) {
        return issue.message == QStringLiteral("Duplicate question in deck");
    }));
    const QByteArray bytesBefore = FileHistory::readBytes(path);

    CardDraft distinct;
    distinct.question = "Different";
    distinct.answer = "B";
    QVERIFY(f.repo.createCards("Warnings", &f.current, {distinct}, nullptr, nullptr, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(f.repo.deckByName("Warnings")->cards.size(), 3);
    QVERIFY(FileHistory::readBytes(path) != bytesBefore);
    const QVector<DeckIssue> after = DeckIO::lintFile(path);
    QVERIFY(std::any_of(after.cbegin(), after.cend(), [](const DeckIssue &issue) {
        return issue.message == QStringLiteral("Duplicate question in deck");
    }));
}

void TestDeckRepository::loadPicksFirstDeckWhenCurrentIsInvalid() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Alpha", &f.current, &error));
    QVERIFY(f.repo.createDeck("Beta", &f.current, &error));

    f.current = "Not a deck";
    f.repo.load(&f.current);
    QCOMPARE(f.current, QString("Alpha"));
}

void TestDeckRepository::loadPrunesStaleSidecarRows() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("D", &f.current, &error));
    QVERIFY(f.repo.createCard("D", &f.current, "Q", "A", "basic", {}, &error));
    const Card card = f.repo.deckByName("D")->cards.first();
    FsrsState state;
    state.reps = 2;
    f.store.save(card.id, DeckIO::contentHash(card), state);
    QVERIFY(f.store.states().contains(card.id));

    Deck onDisk = DeckIO::load(f.repo.deckPath("D"));
    onDisk.cards.clear();
    QVERIFY(DeckIO::save(onDisk));
    f.repo.load(&f.current);

    QVERIFY(!f.store.states().contains(card.id));
}

void TestDeckRepository::loadKeepsStateWhenDeckFileUnreadable() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("Keep", &f.current, &error));
    QVERIFY(f.repo.createCard("Keep", &f.current, "Q", "A", "basic", {}, &error));
    const Card card = f.repo.deckByName("Keep")->cards.first();
    FsrsState state;
    state.reps = 4;
    f.store.save(card.id, DeckIO::contentHash(card), state);

    const QString path = f.repo.deckPath("Keep");
    QVERIFY(QFile::setPermissions(path, QFile::Permissions{}));
    if (QFileInfo(path).isReadable()) {
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
        QSKIP("file permissions are ignored for this user");
    }

    QSignalSpy errorSpy(&f.repo, &DeckRepository::errorOccurred);
    f.repo.load(&f.current);
    QVERIFY(errorSpy.count() > 0);
    QVERIFY(f.store.states().contains(card.id));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
}

void TestDeckRepository::searchCardsMatchesFieldsAndFiltersDeck() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("DeckA", &f.current, &error));
    QVERIFY(f.repo.createCard("DeckA", &f.current, "Alpha Question", "Beta Answer", "basic", "RedTag", &error));
    QVERIFY(f.repo.createDeck("DeckB", &f.current, &error));
    QVERIFY(f.repo.createCard("DeckB", &f.current, "Gamma Question", "Delta Answer", "basic", "BlueTag", &error));

    QCOMPARE(f.repo.searchCards("").size(), f.repo.totalCards());
    QCOMPARE(f.repo.searchCards("   ").size(), f.repo.totalCards());
    QCOMPARE(f.repo.searchCards("alpha").size(), 1);
    QCOMPARE(f.repo.searchCards("BETA").size(), 1);
    QCOMPARE(f.repo.searchCards("redtag").size(), 1);
    QCOMPARE(f.repo.searchCards("  REDTAG  ").size(), 1);
    QCOMPARE(f.repo.searchCards("gamma", "DeckA").size(), 0);
    QCOMPARE(f.repo.searchCards("Question", "DeckA").size(), 1);
    QCOMPARE(f.repo.searchCards("Question", "DeckB").size(), 1);
}

void TestDeckRepository::emptyHistoryUndoRedoReportErrors() {
    RepoFixture f;
    QString error;
    QString description;
    QSignalSpy errorSpy(&f.repo, &DeckRepository::errorOccurred);

    QVERIFY(!f.repo.undo(&error, &description));
    QCOMPARE(errorSpy.count(), 1);
    errorSpy.clear();
    QVERIFY(!f.repo.redo(&error, &description));
    QCOMPARE(errorSpy.count(), 1);
}

void TestDeckRepository::undoRedoEmitHistoryChanged() {
    RepoFixture f;
    QVERIFY(setup(f));
    QString error;
    QVERIFY(f.repo.createDeck("D", &f.current, &error));

    QSignalSpy historySpy(&f.repo, &DeckRepository::historyChanged);
    QString description;
    QVERIFY(f.repo.undo(&error, &description));
    QCOMPARE(historySpy.count(), 1);
    QVERIFY(description.contains(QStringLiteral("creating deck D")));

    historySpy.clear();
    QVERIFY(f.repo.redo(&error, &description));
    QCOMPARE(historySpy.count(), 1);
    QVERIFY(description.contains(QStringLiteral("creating deck D")));
}

void TestDeckRepository::watchDirectoryRequestsReloadAfterFileChange() {
    RepoFixture f;
    QVERIFY(setup(f));
    f.repo.watchDirectory();
    QSignalSpy reloadSpy(&f.repo, &DeckRepository::reloadRequested);

    QVERIFY(writeText(QDir(f.dir.filePath("decks")).filePath("Watched.txt"), "# Deck: Watched\n---\nQ: q\nA: a\n"));
    QTRY_VERIFY_WITH_TIMEOUT(reloadSpy.count() > 0, 2000);
}
