#pragma once

#include "deck.h"

#include <QString>
#include <QVector>

#include <optional>

// Answers one Deck id for a requested Deck. A failure carries a reason only;
// the callers own their error text. The Deck file path is for file-level
// linting, never a second Deck identity. No Backend or QObject dependency.
class DeckIdResolver {
public:
    enum class FailureReason { None, EmptyRequest, UnknownDeck, UnloadedDeckFile };

    struct Result {
        std::optional<QString> deckId;
        FailureReason failureReason = FailureReason::None;
        QString deckPath;
    };

    static Result resolve(const QVector<Deck> &decks, const QString &decksDirectory, const QString &requestedDeck);
};
