#pragma once

#include "domain.h"

#include <QHash>
#include <QStringList>
#include <QVector>

#include "deck.h"

struct CramEntry {
    QString cardId;
    int box = 0; // 0..Config::kLeitnerBoxes-1, delays in Config::kLeitnerIntervals
    int next = 0;
};

// The in-session order of cards. It knows nothing about rendering, SQLite or
// QML; Backend decides when to call it and owns the retry timer.
class StudyQueue {
public:
    enum class Next { Ready, Waiting, Finished };

    void clear();

    void buildReview(const QStringList &dueIds, const QStringList &freshIds = {},
                     const QHash<QString, qint64> &pending = {});
    void buildCram(const QVector<Card> &cards);

    Next next(qint64 now, QString *id, qint64 *waitMs, int *box);
    qint64 nextWait(qint64 now) const;

    void remove(const QString &id);
    void repeatAfter(const QString &id, qint64 notBefore);
    void advanceCram(const QString &id, Rating rating, int *box);

    int pendingRepeatCount() const { return m_reviewAgainNotBefore.size(); }
    QHash<QString, qint64> pendingRepeats() const { return m_reviewAgainNotBefore; }
    bool empty() const { return m_reviewQueue.isEmpty() && m_cramQueue.isEmpty(); }

private:
    QVector<CramEntry> m_cramQueue;
    QStringList m_reviewQueue;
    QHash<QString, qint64> m_reviewAgainNotBefore;
    int m_cramReviews = 0;
};

Q_DECLARE_METATYPE(StudyQueue::Next)
