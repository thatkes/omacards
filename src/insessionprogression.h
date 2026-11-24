#pragma once

#include "config.h"
#include "domain.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <map>

#include "deck.h"

struct CramEntry {
    QString cardId;
    int leitnerBox = 0;     // 0..Config::kLeitnerBoxes-1, delays in Config::kLeitnerIntervals
    int nextShowReview = 0; // review count at which this Card is shown again
    quint64 sequence = 0;   // insertion order, used to break deadline ties
};

// Owns the in-session order of cards, Cram Leitner boxes and Review Pending
// repeat timing. It knows nothing about rendering, SQLite or QML; callers
// supply the policy (ordered Cards and Ratings) and present the results.
class InSessionProgression {
public:
    enum class Next { Ready, Finished };

    struct NextResult {
        Next kind = Next::Finished;
        QString cardId;
        qint64 waitMs = 0;
        int leitnerBox = 0; // 1-based Leitner box of the current Card in Cram
    };

    void clear();

    void buildReview(const QStringList &dueIds, const QStringList &newIds = {},
                     const QHash<QString, qint64> &pending = {});
    // Session-only cards join the live Review queue without a deadline.
    void appendReview(const QStringList &ids);
    void buildCram(const QVector<Card> &orderedCards);
    bool contains(const QString &id) const;
    // A learner asking R during a Pending repeat wants that card now.
    bool presentPendingNow(qint64 now);
    // The repeat timer only makes an elapsed Pending repeat available for the
    // next card transition; it never selects or presents a Flashcard.
    void makePendingAvailable(qint64 now);

    NextResult next(qint64 now);
    // Read-only look at the next cards; used for prefetching only. It never
    // consumes a Card, changes a deadline or advances a Leitner box.
    QStringList upcoming(int limit) const;
    int currentLeitnerBox() const { return m_currentLeitnerBox; }
    // Box distribution of the live Cram queue, in Box 1 to Box 5 order.
    QVector<int> boxDistribution() const;

    void remove(const QString &id);
    void repeatAfter(const QString &id, qint64 notBefore);
    void advanceCram(const QString &id, Rating rating);

    int pendingRepeatCount() const { return m_reviewAgainNotBefore.size(); }
    QHash<QString, qint64> pendingRepeats() const { return m_reviewAgainNotBefore; }
    bool empty() const { return m_reviewQueue.isEmpty() && m_cramById.isEmpty(); }

private:
    // Deadline first, then insertion sequence: this is the order the old
    // vector's minimum scan produced, including first-in-queue tie-breaking.
    struct CramOrderKey {
        int nextShowReview = 0;
        quint64 sequence = 0;

        bool operator<(const CramOrderKey &other) const {
            if (nextShowReview != other.nextShowReview)
                return nextShowReview < other.nextShowReview;
            return sequence < other.sequence;
        }
    };

    void detachCramEntry(const CramEntry &entry);
    void eraseCramEntry(const QString &id);

    qint64 pendingWait(qint64 now) const;

    // Card ids are unique in a built queue: the hash answers id lookups and
    // the map holds the same entries in presentation order.
    std::map<CramOrderKey, QString> m_cramOrder;
    QHash<QString, CramEntry> m_cramById;
    QVector<int> m_cramBoxCounts = QVector<int>(Config::kLeitnerBoxes, 0);
    quint64 m_cramSequence = 0;
    QStringList m_reviewQueue;
    QSet<QString> m_reviewQueueIds;
    QHash<QString, qint64> m_reviewAgainNotBefore;
    qint64 m_cramReviews = 0;
    int m_currentLeitnerBox = 0;
};

Q_DECLARE_METATYPE(InSessionProgression::Next)
