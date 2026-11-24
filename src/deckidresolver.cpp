#include "deckidresolver.h"

#include <QDir>
#include <QFileInfo>

DeckIdResolver::Result DeckIdResolver::resolve(const QVector<Deck> &decks, const QString &decksDirectory,
                                               const QString &requestedDeck) {
    if (requestedDeck.isEmpty())
        return {std::nullopt, FailureReason::EmptyRequest, {}};
    for (const Deck &deck : decks) {
        if (deck.name == requestedDeck)
            return {deck.name, FailureReason::None, deck.path};
    }

    const QDir dir(decksDirectory);
    const QString base = DeckIO::safeFileName(requestedDeck);
    for (const QString &suffix : {QStringLiteral(".txt"), QStringLiteral(".deck")}) {
        const QString candidate = dir.filePath(base + suffix);
        for (const Deck &deck : decks) {
            if (deck.path == candidate)
                return {deck.name, FailureReason::None, deck.path};
        }
        // A Deck file the loader skipped still has a path to lint, but it
        // cannot name a Deck id. Callers report that case from the reason.
        if (QFileInfo::exists(candidate))
            return {std::nullopt, FailureReason::UnloadedDeckFile, candidate};
    }
    return {std::nullopt, FailureReason::UnknownDeck, {}};
}
