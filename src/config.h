#pragma once

#include <QByteArray>
#include <QtGlobal>

// Study defaults and bounds shared by the GUI, CLI and scheduler. Keep the
// optional environment overrides here too, so a literal changed in one path
// cannot silently diverge from the others.
namespace Config {
inline constexpr int kDefaultNewPerDay = 20;
inline constexpr int kMaxNewPerDay = 9999;

inline constexpr double kDefaultRetention = 0.9;
inline constexpr double kMinRetention = 0.70;
inline constexpr double kMaxRetention = 0.99;

inline constexpr qint64 kAgainDelayMs = 60000;
inline constexpr qint64 kMaxAgainDelayMs = 600000;

inline constexpr int kMaxIntervalDays = 36500;

inline constexpr int kLeitnerBoxes = 5;
inline constexpr int kLeitnerIntervals[kLeitnerBoxes] = {1, 2, 4, 8, 16};

// The only read of OMACARDS_AGAIN_DELAY_MS; an invalid value falls back to the
// default and an out-of-range one is clamped to the supported window.
inline qint64 againDelayMs() {
    const QByteArray override = qgetenv("OMACARDS_AGAIN_DELAY_MS");
    bool ok = false;
    const qint64 value = override.toLongLong(&ok);
    return ok ? qBound<qint64>(0, value, kMaxAgainDelayMs) : kAgainDelayMs;
}
} // namespace Config
