#include "studyqueue.h"

#include <QRandomGenerator>
#include <algorithm>

namespace {
constexpr int kCramIntervals[] = {1, 2, 4, 8, 16};
}

void StudyQueue::clear() {
    m_cramQueue.clear();
    m_reviewQueue.clear();
    m_reviewAgainNotBefore.clear();
    m_cramReviews = 0;
}

void StudyQueue::buildReview(const QStringList &dueIds, const QStringList &freshIds,
                             const QHash<QString, qint64> &pending) {
    clear();
    m_reviewQueue = dueIds;
    m_reviewQueue << freshIds;
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        // A repeat's FSRS due date is in the future, so it is not part of the
        // database-derived queue; keep it as session-only work.
        if (!m_reviewQueue.contains(it.key()))
            m_reviewQueue << it.key();
        m_reviewAgainNotBefore.insert(it.key(), it.value());
    }
}

void StudyQueue::buildCram(const QVector<Card> &cards) {
    clear();
    for (int i = 0; i < cards.size(); ++i)
        m_cramQueue.append(CramEntry{cards.at(i).id, 0, i});
    std::shuffle(m_cramQueue.begin(), m_cramQueue.end(), *QRandomGenerator::global());
}

StudyQueue::Next StudyQueue::next(qint64 now, QString *id, qint64 *waitMs, int *box) {
    if (waitMs)
        *waitMs = 0;
    if (!m_cramQueue.isEmpty()) {
        int best = 0;
        for (int i = 1; i < m_cramQueue.size(); ++i)
            if (m_cramQueue.at(i).next < m_cramQueue.at(best).next)
                best = i;
        if (id)
            *id = m_cramQueue.at(best).cardId;
        if (box)
            *box = m_cramQueue.at(best).box + 1;
        return Next::Ready;
    }
    if (m_reviewQueue.isEmpty())
        return Next::Finished;

    int ready = -1;
    for (int i = 0; i < m_reviewQueue.size(); ++i) {
        const auto it = m_reviewAgainNotBefore.constFind(m_reviewQueue.at(i));
        if (it == m_reviewAgainNotBefore.cend() || it.value() <= now) {
            ready = i;
            break;
        }
    }
    if (ready < 0) {
        if (waitMs)
            *waitMs = qMax<qint64>(1, nextWait(now));
        return Next::Waiting;
    }

    const QString chosen = m_reviewQueue.takeAt(ready);
    m_reviewAgainNotBefore.remove(chosen);
    if (id)
        *id = chosen;
    if (waitMs)
        *waitMs = nextWait(now);
    return Next::Ready;
}

qint64 StudyQueue::nextWait(qint64 now) const {
    qint64 next = 0;
    bool haveNext = false;
    for (const QString &id : m_reviewQueue) {
        const auto it = m_reviewAgainNotBefore.constFind(id);
        if (it != m_reviewAgainNotBefore.cend() && (!haveNext || it.value() < next)) {
            next = it.value();
            haveNext = true;
        }
    }
    return haveNext ? qMax<qint64>(1, next - now) : 0;
}

void StudyQueue::remove(const QString &id) {
    m_reviewQueue.removeAll(id);
    m_reviewAgainNotBefore.remove(id);
    m_cramQueue.erase(std::remove_if(m_cramQueue.begin(), m_cramQueue.end(), [&](const CramEntry &entry) {
        return entry.cardId == id;
    }), m_cramQueue.end());
}

void StudyQueue::repeatAfter(const QString &id, qint64 notBefore) {
    if (!m_reviewQueue.contains(id))
        m_reviewQueue << id;
    m_reviewAgainNotBefore.insert(id, notBefore);
}

void StudyQueue::advanceCram(const QString &id, Rating rating, int *box) {
    for (int i = 0; i < m_cramQueue.size(); ++i) {
        if (m_cramQueue.at(i).cardId != id)
            continue;
        CramEntry entry = m_cramQueue.takeAt(i);
        entry.box = rating == Rating::Again ? 0 : qMin(4, entry.box + 1);
        entry.next = ++m_cramReviews + kCramIntervals[entry.box];
        m_cramQueue.append(entry);
        if (box)
            *box = entry.box + 1;
        return;
    }
}
