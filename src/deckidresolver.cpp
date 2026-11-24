#include "deckidresolver.h"

#include <QDir>

DeckIdResolver::Result DeckIdResolver::resolve(const QVector<Deck> &decks, const QString &decksDirectory,
                                               const QString &requestedDeck) {
    if (requestedDeck.isEmpty())
        return {std::nullopt, FailureReason::EmptyRequest};
    for (const Deck &deck : decks) {
        if (deck.name == requestedDeck)
            return {deck.name, FailureReason::None};
    }

    const QDir dir(decksDirectory);
    const QString base = DeckIO::safeFileName(requestedDeck);
    for (const QString &suffix : {QStringLiteral(".txt"), QStringLiteral(".deck")}) {
        const QString candidate = dir.filePath(base + suffix);
        for (const Deck &deck : decks) {
            if (deck.path == candidate)
                return {deck.name, FailureReason::None};
        }
    }
    return {std::nullopt, FailureReason::UnknownDeck};
}
