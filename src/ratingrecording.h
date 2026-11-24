#pragma once

#include "deck.h"
#include "domain.h"
#include "fsrs.h"
#include "store.h"

#include <QString>

// Owns the hash guard, Fsrs call and both writes that make up Rating
// recording. Both adapters call record(); the nested Store adapter is the only
// injected dependency, so a failing write is testable at this seam.
class RatingRecording {
public:
    class StoreAdapter {
    public:
        virtual ~StoreAdapter() = default;

        virtual void clearError() = 0;
        virtual SchedulingRecord schedulingRecord(const QString &cardId) const = 0;
        virtual void save(const QString &cardId, const QString &hash, const FsrsState &state) = 0;
        virtual void recordRating(const QString &cardId, int rating, qint64 at, const FsrsState &state,
                                  int intervalDays, bool wasNew) = 0;
        virtual bool beginRatingWrite() = 0;
        virtual bool finishRatingWrite() = 0;
        virtual QString lastError() const = 0;
    };

    enum class FailurePhase { None, SchedulingRead, StoreWrite };

    struct Result {
        bool ok = false;
        FsrsState state; // starting state on failure, updated state on success
        int intervalDays = 0;
        QString storeError;
        FailurePhase failurePhase = FailurePhase::None;
    };

    explicit RatingRecording(StoreAdapter &store);

    Result record(const Card &card, Rating rating, double desiredRetention, qint64 now);

private:
    StoreAdapter &m_store;
};

// Binds the module to the existing Store API without changing that API.
class StoreRatingAdapter : public RatingRecording::StoreAdapter {
public:
    explicit StoreRatingAdapter(Store &store);

    void clearError() override;
    SchedulingRecord schedulingRecord(const QString &cardId) const override;
    void save(const QString &cardId, const QString &hash, const FsrsState &state) override;
    void recordRating(const QString &cardId, int rating, qint64 at, const FsrsState &state, int intervalDays,
                      bool wasNew) override;
    bool beginRatingWrite() override;
    bool finishRatingWrite() override;
    QString lastError() const override;

private:
    Store &m_store;
};
