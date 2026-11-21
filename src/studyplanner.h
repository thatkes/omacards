#pragma once

#include "deck.h"
#include "fsrs.h"
#include "store.h"

#include <QHash>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

// Reads the scheduling sidecar once into a snapshot and answers every
// availability/count question from it. Backend supplies the decks, limits and
// translated labels; this class never talks to QML.
class StudyPlanner {
public:
    struct Availability {
        QVector<Card> due;   // ordered by due date, then id
        QVector<Card> fresh; // deck-file order, up to the daily cap
        QVector<int> duePerDeck;
        QVector<int> freshPerDeck;
        QVector<int> dueIncludingNewPerDeck; // totalDue in CLI stats includes new cards
    };

    explicit StudyPlanner(Store &store);

    void setDecks(const QVector<Deck> &decks) { m_decks = &decks; }
    void configure(int newPerDay, int dayStartHour);

    bool refresh(QString *storeError = nullptr);
    void updateNewBudget(QString *storeError = nullptr);

    FsrsState stateFor(const Card &card) const;
    Availability availability(const QString &deckName = {}) const;
    int countDue(const QString &deckName = {}) const;
    int availableCount(const QString &deckName = {}) const;
    int newCount() const;
    int deckDue(int deckIndex) const;
    int deckNew(int deckIndex) const;
    QVector<Card> dueCards(const QString &deckName = {}) const;
    void reviewIds(const QString &deckName, QStringList *dueIds, QStringList *freshIds) const;
    QVariantList deckSummaries(const QString &currentDeck) const;
    QVariantList cardList(const QString &currentDeck, const QString &filter, const QString &newText,
                          const QString &dueText) const;
    QVariantMap statsSummary(int dueCount, int totalCards, int deckCount, int sessionReviewed, int sessionGood,
                             double desiredRetention) const;

private:
    struct StudySnapshot {
        QHash<QString, FsrsState> states;
        QHash<QString, QString> hashes;
        int newBudget = 0;
        qint64 now = 0;
    };

    const QVector<Deck> &decks() const;

    Store &m_store;
    const QVector<Deck> *m_decks = nullptr;
    StudySnapshot m_snapshot;
    bool m_snapshotValid = false;
    int m_newPerDay = 0;
    int m_dayStartHour = 0;
};
