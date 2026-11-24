#pragma once

#include "deck.h"

#include <QString>
#include <QVector>

#include <optional>

// Answers one Deck id for a requested Deck. A failure carries a reason only;
// the callers own their error text. No Backend or QObject dependency.
class DeckIdResolver {
public:
    enum class FailureReason { None, EmptyRequest, UnknownDeck };

    struct Result {
        std::optional<QString> deckId;
        FailureReason failureReason = FailureReason::None;
    };

    static Result resolve(const QVector<Deck> &decks, const QString &decksDirectory, const QString &requestedDeck);
};
