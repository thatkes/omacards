#pragma once

#include "deck.h"

#include <QString>

// Parsers for external card formats. They only produce card drafts; applying
// them to decks and scheduling state stays in Backend.
namespace DeckImport {

struct Result {
    QString deckName;
    QVector<CardDraft> cards;
};

bool file(const QString &path, const QString &deckName, const QString &decksDir, Result *result,
          QString *error = nullptr);
bool csv(const QString &path, const QString &deckName, Result *result, QString *error = nullptr);
bool apkg(const QString &path, const QString &deckName, const QString &decksDir, Result *result,
          QString *error = nullptr);

} // namespace DeckImport
