#include "ratingrecording.h"

RatingRecording::RatingRecording(StoreAdapter &store) : m_store(store) {}

RatingRecording::Result RatingRecording::record(const Card &card, Rating rating, double desiredRetention, qint64 now) {
    const QString hash = DeckIO::contentHash(card);
    m_store.clearError();
    FsrsState state = m_store.states().value(card.id);
    const QString storedHash = m_store.hashes().value(card.id);
    if (!m_store.lastError().isEmpty())
        return Result{false, state, 0, m_store.lastError()};
    if (storedHash != hash)
        state = FsrsState{};
    const bool wasNew = state.reps == 0;
    const ReviewResult reviewed = Fsrs::review(state, Domain::toInt(rating), now, desiredRetention);

    m_store.clearError();
    m_store.save(card.id, hash, reviewed.state);
    m_store.logReview(card.id, Domain::toInt(rating), now, reviewed.state, reviewed.intervalDays, wasNew);
    if (!m_store.lastError().isEmpty())
        return Result{false, state, 0, m_store.lastError()};

    return Result{true, reviewed.state, reviewed.intervalDays, {}};
}

StoreRatingAdapter::StoreRatingAdapter(Store &store) : m_store(store) {}

void StoreRatingAdapter::clearError() {
    m_store.clearError();
}

QHash<QString, FsrsState> StoreRatingAdapter::states() const {
    return m_store.states();
}

QHash<QString, QString> StoreRatingAdapter::hashes() const {
    return m_store.hashes();
}

void StoreRatingAdapter::save(const QString &cardId, const QString &hash, const FsrsState &state) {
    m_store.save(cardId, hash, state);
}

void StoreRatingAdapter::logReview(const QString &cardId, int rating, qint64 at, const FsrsState &state,
                                   int intervalDays, bool wasNew) {
    m_store.logReview(cardId, rating, at, state, intervalDays, wasNew);
}

QString StoreRatingAdapter::lastError() const {
    return m_store.lastError();
}
