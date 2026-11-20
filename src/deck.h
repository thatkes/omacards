#pragma once

#include "domain.h"

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

struct Card {
    QString id; // stable hash of deck + question + answer, or an explicit id
    QString question;
    QString answer;
    QString deck;
    CardTypeValue type; // typed kind plus the exact boundary text
    QStringList tags;
    bool explicitId = false; // an `id:` line pins identity across edits
};

struct Deck {
    QString name;
    QString path;
    QString description;
    QVector<Card> cards;
};

struct CardDraft {
    QString question;
    QString answer;
    QString type = "basic"; // basic | cloze
    QStringList tags;
};

struct DeckIssue {
    QString path;                                 // deck file the issue belongs to
    QString card;                                 // card id, when the issue is about a card
    int line = 0;                                 // 1-based line in the deck file, when known
    QString severity = QStringLiteral("warning"); // error | warning
    QString message;
    QString hint;
};

Q_DECLARE_METATYPE(Card)
Q_DECLARE_METATYPE(Deck)

namespace DeckIO {
QVector<Deck> loadAll(const QString &directory, QString *error = nullptr);
QVector<DeckIssue> lint(const QString &directory);
QVector<DeckIssue> lintFile(const QString &path);
Deck load(const QString &path, QString *error = nullptr);
QString serialize(const Deck &deck);
bool save(const Deck &deck, QString *error = nullptr);
QString cardId(const Card &card);
QString contentHash(const Card &card);
QString safeFileName(const QString &name);
QString displayQuestion(const Card &card);
QString displayAnswer(const Card &card);
} // namespace DeckIO
