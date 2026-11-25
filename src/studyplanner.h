#pragma once

#include "deck.h"
#include "fsrs.h"
#include "store.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

// Reads the scheduling sidecar once into a snapshot and answers every
// availability/count question from it. Backend supplies the decks, limits and
// translated labels; this class never talks to QML.
class StudyPlanner {
public:
    // The only Store operations refresh needs. A stub can fail a read here
    // without degrading the real database.
    class StoreAdapter {
    public:
        virtual ~StoreAdapter() = default;

        virtual void clearError() = 0;
        virtual QHash<QString, FsrsState> states() const = 0;
        virtual QHash<QString, QString> hashes() const = 0;
        // Older stubs still answer the two reads separately; the real adapter
        // overrides this with one cards query.
        virtual QHash<QString, SchedulingRecord> schedulingRecords() const {
            QHash<QString, SchedulingRecord> combined;
            const QHash<QString, FsrsState> allStates = states();
            const QHash<QString, QString> allHashes = hashes();
            for (auto it = allStates.cbegin(); it != allStates.cend(); ++it)
                combined.insert(it.key(), SchedulingRecord{it.value(), allHashes.value(it.key())});
            return combined;
        }
        virtual int newCardsOnStudyDay(int dayStartHour, const QString &deckId = {}) const = 0;
        virtual QString lastError() const = 0;
    };

    struct Availability {
        QVector<Card> due;      // ordered by due date, then id
        QVector<Card> newCards; // deck-file order, up to the daily cap
        QVector<int> duePerDeck;
        QVector<int> newPerDeck;
        QVector<int> dueIncludingNewPerDeck; // totalDue in CLI stats includes new cards
    };

    explicit StudyPlanner(StoreAdapter &store);

    void setDecks(const QVector<Deck> &decks) {
        m_decks = &decks;
        ++m_decksGeneration;
    }
    void configure(int newCardAllowance, int dayStartHour);

    bool refresh(QString *storeError = nullptr);
    void updateNewBudget(QString *storeError = nullptr);

    FsrsState stateFor(const Card &card) const;
    Availability availability(const QString &deckName = {}) const;
    int countDue(const QString &deckName = {}) const;
    int availableCount(const QString &deckName = {}) const;
    int newCount(const QString &deckName = {}) const;
    int deckDue(int deckIndex) const;
    int deckNew(int deckIndex) const;
    QVector<Card> dueCards(const QString &deckName = {}) const;
    void reviewIds(const QString &deckName, QStringList *dueIds, QStringList *newIds) const;
    QVariantList deckSummaries(const QString &currentDeck) const;
    QVariantList cardList(const QString &currentDeck, const QString &filter, const QString &newText,
                          const QString &dueText) const;
    // Inputs to the stats summary: the review history plus the counts and
    // settings measured when the summary is requested.
    struct StatsSummaryInputs {
        ReviewSummary summary;
        int dueCount;
        int totalCards;
        int deckCount;
        int sessionReviewed;
        int sessionGood;
        double desiredRetention;
    };
    QVariantMap statsSummary(const StatsSummaryInputs &inputs, const QString &deckName = {}) const;
    // Read-only distribution of Cards by next scheduled return. An empty deck
    // name means all Decks, matching availability() and the Stats panel.
    QVariantMap forecast(const QString &deckName = {}) const;

private:
    struct StudySnapshot {
        QHash<QString, FsrsState> states;
        QHash<QString, QString> hashes;
        // Content hashes are computed once at refresh(); cards are kept so an
        // edited Card with the same id still fails the stale-hash guard.
        QHash<QString, Card> hashedCards;
        QSet<QString> validIds;
        // One allowance per Deck name. A future per-Deck setting only has to
        // replace the value each entry is seeded with.
        QHash<QString, int> newBudgetByDeck;
        qint64 now = 0;
    };

    struct CachedAvailability {
        quint64 snapshotGeneration = 0;
        quint64 decksGeneration = 0;
        QHash<QString, Availability> byDeckName;
    };

    const QVector<Deck> &decks() const;

    StoreAdapter &m_store;
    const QVector<Deck> *m_decks = nullptr;
    StudySnapshot m_snapshot;
    bool m_snapshotValid = false;
    int m_newCardAllowance = 0;
    int m_dayStartHour = 0;
    quint64 m_snapshotGeneration = 0;
    quint64 m_decksGeneration = 0;
    mutable CachedAvailability m_availabilityCache;
};

// Binds the planner to the existing Store API without changing that API.
class StorePlannerAdapter : public StudyPlanner::StoreAdapter {
public:
    explicit StorePlannerAdapter(Store &store);

    void clearError() override;
    QHash<QString, FsrsState> states() const override;
    QHash<QString, QString> hashes() const override;
    QHash<QString, SchedulingRecord> schedulingRecords() const override;
    int newCardsOnStudyDay(int dayStartHour, const QString &deckId = {}) const override;
    QString lastError() const override;

private:
    Store &m_store;
};
