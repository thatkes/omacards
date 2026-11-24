#include "ratingrecording.h"

RatingRecording::RatingRecording(StoreAdapter &store) : m_store(store) {}

RatingRecording::Result RatingRecording::record(const Card &card, Rating rating, double desiredRetention, qint64 now) {
    const QString hash = DeckIO::contentHash(card);
    m_store.clearError();
    const SchedulingRecord stored = m_store.schedulingRecord(card.id);
    if (!m_store.lastError().isEmpty())
        return Result{false, {}, 0, m_store.lastError(), FailurePhase::SchedulingRead};
    FsrsState state = stored.state;
    if (stored.hash != hash)
        state = FsrsState{};
    const bool wasNew = state.reps == 0;
    const ReviewResult reviewed = Fsrs::review(state, Domain::toInt(rating), now, desiredRetention);

    m_store.clearError();
    if (!m_store.beginRatingWrite())
        return Result{false, state, 0, m_store.lastError(), FailurePhase::StoreWrite};
    m_store.save(card.id, hash, reviewed.state);
    m_store.recordRating(card.id, Domain::toInt(rating), now, reviewed.state, reviewed.intervalDays, wasNew);
    if (!m_store.finishRatingWrite() || !m_store.lastError().isEmpty())
        return Result{false, state, 0, m_store.lastError(), FailurePhase::StoreWrite};

    return Result{true, reviewed.state, reviewed.intervalDays, {}, FailurePhase::None};
}

StoreRatingAdapter::StoreRatingAdapter(Store &store) : m_store(store) {}

void StoreRatingAdapter::clearError() {
    m_store.clearError();
}

SchedulingRecord StoreRatingAdapter::schedulingRecord(const QString &cardId) const {
    return m_store.schedulingRecord(cardId);
}

void StoreRatingAdapter::save(const QString &cardId, const QString &hash, const FsrsState &state) {
    m_store.save(cardId, hash, state);
}

void StoreRatingAdapter::recordRating(const QString &cardId, int rating, qint64 at, const FsrsState &state,
                                      int intervalDays, bool wasNew) {
    m_store.recordRating(cardId, rating, at, state, intervalDays, wasNew);
}

bool StoreRatingAdapter::beginRatingWrite() {
    return m_store.beginRatingWrite();
}

bool StoreRatingAdapter::finishRatingWrite() {
    return m_store.finishRatingWrite();
}

QString StoreRatingAdapter::lastError() const {
    return m_store.lastError();
}
