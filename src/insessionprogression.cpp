#include "insessionprogression.h"
#include "config.h"

#include <algorithm>

void InSessionProgression::clear() {
    m_cramQueue.clear();
    m_reviewQueue.clear();
    m_reviewAgainNotBefore.clear();
    m_cramReviews = 0;
    m_currentBox = 0;
}

void InSessionProgression::buildReview(const QStringList &dueIds, const QStringList &freshIds,
                                       const QHash<QString, qint64> &pending) {
    clear();
    m_reviewQueue = dueIds;
    m_reviewQueue << freshIds;
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        // A repeat's FSRS due date is in the future, so it is not part of the
        // database-derived queue; keep it as session-only work with its
        // absolute deadline intact.
        if (!m_reviewQueue.contains(it.key()))
            m_reviewQueue << it.key();
        m_reviewAgainNotBefore.insert(it.key(), it.value());
    }
}

void InSessionProgression::buildCram(const QVector<Card> &orderedCards) {
    clear();
    for (int i = 0; i < orderedCards.size(); ++i)
        m_cramQueue.append(CramEntry{orderedCards.at(i).id, 0, i});
}

InSessionProgression::NextResult InSessionProgression::next(qint64 now) {
    NextResult result;
    if (!m_cramQueue.isEmpty()) {
        int best = 0;
        for (int i = 1; i < m_cramQueue.size(); ++i)
            if (m_cramQueue.at(i).next < m_cramQueue.at(best).next)
                best = i;
        result.kind = Next::Ready;
        result.cardId = m_cramQueue.at(best).cardId;
        m_currentBox = m_cramQueue.at(best).box + 1;
        result.box = m_currentBox;
        return result;
    }
    if (m_reviewQueue.isEmpty())
        return result;

    int ready = -1;
    for (int i = 0; i < m_reviewQueue.size(); ++i) {
        const auto it = m_reviewAgainNotBefore.constFind(m_reviewQueue.at(i));
        if (it == m_reviewAgainNotBefore.cend() || it.value() <= now) {
            ready = i;
            break;
        }
    }
    if (ready < 0) {
        result.kind = Next::Waiting;
        result.waitMs = qMax<qint64>(1, pendingWait(now));
        return result;
    }

    const QString chosen = m_reviewQueue.takeAt(ready);
    m_reviewAgainNotBefore.remove(chosen);
    result.kind = Next::Ready;
    result.cardId = chosen;
    result.waitMs = pendingWait(now); // computed after the ready card was taken
    return result;
}

qint64 InSessionProgression::pendingWait(qint64 now) const {
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

void InSessionProgression::remove(const QString &id) {
    m_reviewQueue.removeAll(id);
    m_reviewAgainNotBefore.remove(id);
    m_cramQueue.erase(std::remove_if(m_cramQueue.begin(), m_cramQueue.end(),
                                     [&](const CramEntry &entry) { return entry.cardId == id; }),
                      m_cramQueue.end());
}

void InSessionProgression::repeatAfter(const QString &id, qint64 notBefore) {
    if (!m_reviewQueue.contains(id))
        m_reviewQueue << id;
    m_reviewAgainNotBefore.insert(id, notBefore);
}

void InSessionProgression::advanceCram(const QString &id, Rating rating) {
    for (int i = 0; i < m_cramQueue.size(); ++i) {
        if (m_cramQueue.at(i).cardId != id)
            continue;
        CramEntry entry = m_cramQueue.takeAt(i);
        entry.box = rating == Rating::Again ? 0 : qMin(Config::kLeitnerBoxes - 1, entry.box + 1);
        entry.next = ++m_cramReviews + Config::kLeitnerIntervals[entry.box];
        m_cramQueue.append(entry);
        m_currentBox = entry.box + 1;
        return;
    }
}
