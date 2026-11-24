#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QVector>

#include "deck.h"
#include "deckidresolver.h"
#include "test_deckidresolver.h"

namespace {

Deck deckWith(const QString &name, const QString &path) {
    Deck deck;
    deck.name = name;
    deck.path = path;
    return deck;
}

} // namespace

void TestDeckIdResolver::exactDeckNameWins() {
    const QVector<Deck> decks = {deckWith(QStringLiteral("Filename Deck"), QStringLiteral("/decks/Notes.txt")),
                                 deckWith(QStringLiteral("Notes"), QStringLiteral("/decks/Other.txt"))};

    const DeckIdResolver::Result result =
        DeckIdResolver::resolve(decks, QStringLiteral("/decks"), QStringLiteral("Notes"));

    QVERIFY(result.deckId.has_value());
    QCOMPARE(*result.deckId, QStringLiteral("Notes"));
    QCOMPARE(result.failureReason, DeckIdResolver::FailureReason::None);
    // The exact name wins over the file-name fallback, so the path belongs
    // to the chosen Deck, not to the file the request happened to name.
    QCOMPARE(result.deckPath, QStringLiteral("/decks/Other.txt"));
}

void TestDeckIdResolver::fileNameFallbackReturnsWrittenName() {
    const QVector<Deck> decks = {deckWith(QStringLiteral("Written Name"), QStringLiteral("/decks/Study-Guide.txt"))};

    const DeckIdResolver::Result result =
        DeckIdResolver::resolve(decks, QStringLiteral("/decks"), QStringLiteral("Study Guide"));

    QVERIFY(result.deckId.has_value());
    QCOMPARE(*result.deckId, QStringLiteral("Written Name"));
    QCOMPARE(result.failureReason, DeckIdResolver::FailureReason::None);
    QCOMPARE(result.deckPath, QStringLiteral("/decks/Study-Guide.txt"));
}

void TestDeckIdResolver::deckFileExtensionFallback() {
    const QVector<Deck> decks = {deckWith(QStringLiteral("Archived"), QStringLiteral("/decks/Old.deck"))};

    const DeckIdResolver::Result result =
        DeckIdResolver::resolve(decks, QStringLiteral("/decks"), QStringLiteral("Old"));

    QVERIFY(result.deckId.has_value());
    QCOMPARE(*result.deckId, QStringLiteral("Archived"));
    QCOMPARE(result.failureReason, DeckIdResolver::FailureReason::None);
}

void TestDeckIdResolver::textExtensionIsCheckedBeforeDeckExtension() {
    const QVector<Deck> decks = {deckWith(QStringLiteral("Text Deck"), QStringLiteral("/decks/Shared.txt")),
                                 deckWith(QStringLiteral("Deck File"), QStringLiteral("/decks/Shared.deck"))};

    const DeckIdResolver::Result result =
        DeckIdResolver::resolve(decks, QStringLiteral("/decks"), QStringLiteral("Shared"));

    QVERIFY(result.deckId.has_value());
    QCOMPARE(*result.deckId, QStringLiteral("Text Deck"));
    QCOMPARE(result.failureReason, DeckIdResolver::FailureReason::None);
}

void TestDeckIdResolver::emptyRequestReturnsNoId() {
    const QVector<Deck> decks = {deckWith(QStringLiteral("Deck"), QStringLiteral("/decks/Deck.txt"))};

    const DeckIdResolver::Result result = DeckIdResolver::resolve(decks, QStringLiteral("/decks"), QString());

    QVERIFY(!result.deckId.has_value());
    QCOMPARE(result.failureReason, DeckIdResolver::FailureReason::EmptyRequest);
}

void TestDeckIdResolver::unknownRequestReturnsStructuredReason() {
    const QVector<Deck> decks = {deckWith(QStringLiteral("Deck"), QStringLiteral("/decks/Deck.txt"))};

    const DeckIdResolver::Result result =
        DeckIdResolver::resolve(decks, QStringLiteral("/decks"), QStringLiteral("None"));

    QVERIFY(!result.deckId.has_value());
    QCOMPARE(result.failureReason, DeckIdResolver::FailureReason::UnknownDeck);
    QVERIFY(result.deckPath.isEmpty());
}

void TestDeckIdResolver::unloadedDeckFileCarriesCandidatePath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("Skipped.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const DeckIdResolver::Result result = DeckIdResolver::resolve({}, dir.path(), QStringLiteral("Skipped"));

    QVERIFY(!result.deckId.has_value());
    QCOMPARE(result.failureReason, DeckIdResolver::FailureReason::UnloadedDeckFile);
    QCOMPARE(result.deckPath, path);
}
