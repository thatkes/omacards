#pragma once

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QTime>
#include <QtGlobal>

// Study defaults and bounds shared by the GUI, CLI and scheduler. Keep the
// optional environment overrides here too, so a literal changed in one path
// cannot silently diverge from the others.
namespace Config {
inline constexpr int kDefaultNewPerDay = 20;
inline constexpr int kMinNewPerDay = 0;
inline constexpr int kMaxNewPerDay = 9999;

inline constexpr int kDefaultDayStartHour = 0;
inline constexpr int kMinDayStartHour = 0;
inline constexpr int kMaxDayStartHour = 23;

inline constexpr double kDefaultRetention = 0.9;
inline constexpr double kMinRetention = 0.70;
inline constexpr double kMaxRetention = 0.99;

inline constexpr qint64 kAgainDelayMs = 60000;
inline constexpr qint64 kMaxAgainDelayMs = 600000;

inline constexpr int kMaxIntervalDays = 36500;

inline constexpr int kLeitnerBoxes = 5;
inline constexpr int kLeitnerIntervals[kLeitnerBoxes] = {1, 2, 4, 8, 16};

// Study days roll over at dayStartHour instead of at midnight. Everything that
// calls itself "due" or "Study day" uses these helpers so the library label, the
// due counts and the New-card allowance cannot disagree about the boundary.
// Compare local calendar days, not raw epoch days: dividing by 86400000 would
// put the boundary at UTC midnight and shift every user by their timezone.
inline QDate studyDay(qint64 atMs, int dayStartHour) {
    return QDateTime::fromMSecsSinceEpoch(atMs).addSecs(-static_cast<qint64>(dayStartHour) * 3600).date();
}

inline bool isDueByDay(qint64 dueMs, qint64 nowMs, int dayStartHour) {
    return studyDay(dueMs, dayStartHour) <= studyDay(nowMs, dayStartHour);
}

// First instant of the study day containing `atMs`. With a Day-start hour of 3 AM, a
// clock of 01:00 on Nov 19 starts the Nov 18 study day at 03:00 on Nov 18.
inline qint64 studyDayStartMs(qint64 atMs, int dayStartHour) {
    const QDateTime shifted = QDateTime::fromMSecsSinceEpoch(atMs).addSecs(-static_cast<qint64>(dayStartHour) * 3600);
    return QDateTime(shifted.date(), QTime(dayStartHour, 0)).toMSecsSinceEpoch();
}

// The only read of OMACARDS_AGAIN_DELAY_MS; an invalid value falls back to the
// default and an out-of-range one is clamped to the supported window.
inline qint64 againDelayMs() {
    const QByteArray override = qgetenv("OMACARDS_AGAIN_DELAY_MS");
    bool ok = false;
    const qint64 value = override.toLongLong(&ok);
    return ok ? qBound<qint64>(0, value, kMaxAgainDelayMs) : kAgainDelayMs;
}
} // namespace Config
