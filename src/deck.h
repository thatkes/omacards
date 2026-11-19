#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

struct Card {
    QString id;      // stable hash of deck + question + answer, or an explicit id
    QString question;
    QString answer;
    QString deck;
    QString type = "basic"; // basic | cloze
    QStringList tags;
    bool explicitId = false; // an `id:` line pins identity across edits
};

struct Deck {
    QString name;
    QString path;
    QString description;
    QVector<Card> cards;
};

Q_DECLARE_METATYPE(Card)
Q_DECLARE_METATYPE(Deck)

namespace DeckIO {
    QVector<Deck> loadAll(const QString &directory, QString *error = nullptr);
    Deck load(const QString &path, QString *error = nullptr);
    QString serialize(const Deck &deck);
    bool save(const Deck &deck, QString *error = nullptr);
    QString cardId(const Card &card);
    QString contentHash(const Card &card);
    QString displayQuestion(const Card &card);
    QString displayAnswer(const Card &card);
}
