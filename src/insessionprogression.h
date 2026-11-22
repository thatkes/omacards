#pragma once

#include "domain.h"

#include <QHash>
#include <QStringList>
#include <QVector>

#include "deck.h"

struct CramEntry {
    QString cardId;
    int box = 0;            // 0..Config::kLeitnerBoxes-1, delays in Config::kLeitnerIntervals
    int nextShowReview = 0; // review count at which this Card is shown again
};

// Owns the in-session order of cards, Cram Leitner boxes and Review Pending
// repeat timing. It knows nothing about rendering, SQLite or QML; callers
// supply the policy (ordered Cards and Ratings) and present the results.
class InSessionProgression {
public:
    enum class Next { Ready, Waiting, Finished };

    struct NextResult {
        Next kind = Next::Finished;
        QString cardId;
        qint64 waitMs = 0;
        int box = 0; // 1-based Leitner box of the current Card in Cram
    };

    void clear();

    void buildReview(const QStringList &dueIds, const QStringList &newIds = {},
                     const QHash<QString, qint64> &pending = {});
    void buildCram(const QVector<Card> &orderedCards);

    NextResult next(qint64 now);
    // Read-only look at the next cards; used for prefetching only. It never
    // consumes a Card, changes a deadline or advances a Leitner box.
    QStringList upcoming(int limit) const;
    int currentBox() const { return m_currentBox; }

    void remove(const QString &id);
    void repeatAfter(const QString &id, qint64 notBefore);
    void advanceCram(const QString &id, Rating rating);

    int pendingRepeatCount() const { return m_reviewAgainNotBefore.size(); }
    QHash<QString, qint64> pendingRepeats() const { return m_reviewAgainNotBefore; }
    bool empty() const { return m_reviewQueue.isEmpty() && m_cramQueue.isEmpty(); }

private:
    qint64 pendingWait(qint64 now) const;

    QVector<CramEntry> m_cramQueue;
    QStringList m_reviewQueue;
    QHash<QString, qint64> m_reviewAgainNotBefore;
    qint64 m_cramReviews = 0;
    int m_currentBox = 0;
};

Q_DECLARE_METATYPE(InSessionProgression::Next)
