#include "insessionprogression.h"
#include "config.h"

void InSessionProgression::clear() {
    m_cramOrder.clear();
    m_cramById.clear();
    m_cramBoxCounts = QVector<int>(Config::kLeitnerBoxes, 0);
    m_cramSequence = 0;
    m_reviewQueue.clear();
    m_reviewQueueIds.clear();
    m_reviewAgainNotBefore.clear();
    m_cramReviews = 0;
    m_currentLeitnerBox = 0;
}

void InSessionProgression::buildReview(const QStringList &dueIds, const QStringList &newIds,
                                       const QHash<QString, qint64> &pending) {
    clear();
    m_reviewQueue = dueIds;
    m_reviewQueue << newIds;
    m_reviewQueueIds = QSet<QString>(m_reviewQueue.cbegin(), m_reviewQueue.cend());
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        // A repeat's FSRS due date is in the future, so it is not part of the
        // database-derived queue; keep it as session-only work with its
        // absolute deadline intact.
        repeatAfter(it.key(), it.value());
    }
}

void InSessionProgression::appendReview(const QStringList &ids) {
    for (const QString &id : ids) {
        if (m_reviewQueueIds.contains(id))
            continue;
        m_reviewQueue << id;
        m_reviewQueueIds.insert(id);
    }
}

bool InSessionProgression::contains(const QString &id) const {
    return m_reviewQueueIds.contains(id) || m_cramById.contains(id);
}

bool InSessionProgression::presentPendingNow(qint64 now) {
    for (const QString &id : m_reviewQueue) {
        auto pending = m_reviewAgainNotBefore.find(id);
        if (pending == m_reviewAgainNotBefore.cend())
            continue;
        pending.value() = now;
        return true;
    }
    return false;
}

void InSessionProgression::buildCram(const QVector<Card> &orderedCards) {
    clear();
    for (int i = 0; i < orderedCards.size(); ++i) {
        const CramEntry entry{orderedCards.at(i).id, 0, i, m_cramSequence++};
        m_cramById.insert(entry.cardId, entry);
        m_cramOrder.emplace(CramOrderKey{entry.nextShowReview, entry.sequence}, entry.cardId);
        ++m_cramBoxCounts[entry.leitnerBox];
    }
}

InSessionProgression::NextResult InSessionProgression::next(qint64 now) {
    NextResult result;
    if (!m_cramOrder.empty()) {
        const auto first = m_cramOrder.cbegin();
        const auto entry = m_cramById.constFind(first->second);
        result.kind = Next::Ready;
        result.cardId = first->second;
        m_currentLeitnerBox = entry.value().leitnerBox + 1;
        result.leitnerBox = m_currentLeitnerBox;
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
    m_reviewQueueIds.remove(chosen);
    m_reviewAgainNotBefore.remove(chosen);
    result.kind = Next::Ready;
    result.cardId = chosen;
    result.waitMs = pendingWait(now); // computed after the ready card was taken
    return result;
}

QVector<int> InSessionProgression::boxDistribution() const {
    return m_cramBoxCounts;
}

QStringList InSessionProgression::upcoming(int limit) const {
    QStringList result;
    if (limit <= 0)
        return result;

    if (!m_cramOrder.empty()) {
        // Walking the deadline index gives the same order as repeatedly
        // picking the minimum from the old vector, including ties.
        for (auto it = m_cramOrder.cbegin(); it != m_cramOrder.cend() && result.size() < limit; ++it)
            result << it->second;
        return result;
    }

    // Pending repeats stay in the queue, so they are part of the peek even
    // while their deadline is in the future.
    for (const QString &id : m_reviewQueue) {
        if (result.size() >= limit)
            break;
        result << id;
    }
    return result;
}

qint64 InSessionProgression::pendingWait(qint64 now) const {
    qint64 next = 0;
    bool haveNext = false;
    for (auto it = m_reviewAgainNotBefore.cbegin(); it != m_reviewAgainNotBefore.cend(); ++it) {
        if (!m_reviewQueueIds.contains(it.key()))
            continue;
        if (!haveNext || it.value() < next) {
            next = it.value();
            haveNext = true;
        }
    }
    return haveNext ? qMax<qint64>(1, next - now) : 0;
}

void InSessionProgression::detachCramEntry(const CramEntry &entry) {
    const QString id = entry.cardId;
    m_cramOrder.erase(CramOrderKey{entry.nextShowReview, entry.sequence});
    --m_cramBoxCounts[entry.leitnerBox];
    m_cramById.remove(id);
}

void InSessionProgression::eraseCramEntry(const QString &id) {
    const auto found = m_cramById.constFind(id);
    if (found != m_cramById.cend())
        detachCramEntry(found.value());
}

void InSessionProgression::remove(const QString &id) {
    m_reviewQueue.removeAll(id);
    m_reviewQueueIds.remove(id);
    m_reviewAgainNotBefore.remove(id);
    eraseCramEntry(id);
}

void InSessionProgression::repeatAfter(const QString &id, qint64 notBefore) {
    if (!m_reviewQueueIds.contains(id)) {
        m_reviewQueue << id;
        m_reviewQueueIds.insert(id);
    }
    m_reviewAgainNotBefore.insert(id, notBefore);
}

void InSessionProgression::advanceCram(const QString &id, Rating rating) {
    const auto found = m_cramById.constFind(id);
    if (found == m_cramById.cend())
        return;

    CramEntry entry = found.value();
    detachCramEntry(entry);

    entry.leitnerBox = rating == Rating::Again ? 0 : qMin(Config::kLeitnerBoxes - 1, entry.leitnerBox + 1);
    entry.nextShowReview = ++m_cramReviews + Config::kLeitnerIntervals[entry.leitnerBox];
    entry.sequence = m_cramSequence++;
    ++m_cramBoxCounts[entry.leitnerBox];
    m_cramById.insert(id, entry);
    m_cramOrder.emplace(CramOrderKey{entry.nextShowReview, entry.sequence}, id);
    m_currentLeitnerBox = entry.leitnerBox + 1;
}
