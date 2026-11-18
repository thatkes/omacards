#pragma once

#include <QtGlobal>

// FSRS-5 memory state for one card. Times are Unix milliseconds so the SQLite
// sidecar and QML can both use them without conversion.
struct FsrsState {
    double stability = 0.0;   // days at which recall is 90%
    double difficulty = 0.0;  // 1 (easy) .. 10 (hard)
    qint64 due = 0;           // next review time
    qint64 lastReview = 0;
    int reps = 0;
    int lapses = 0;
};

struct ReviewResult {
    FsrsState state;
    int intervalDays = 1;
    double retrievability = 0.0;
};

// A small, self-contained port of the FSRS-5 model (19 weights, fixed decay
// -0.5). Only Again (1) and Good (3) are exposed in omacards, but the model
// accepts all four grades so it stays faithful to the reference algorithm.
class Fsrs {
public:
    static constexpr double decay = -0.5;
    static constexpr double factor = 19.0 / 81.0;
    static constexpr int maximumInterval = 36500;
    static constexpr double desiredRetention = 0.9;

    // Default FSRS-5 weights from ts-fsrs v4 (the FSRS V5 preset).
    static const double weights[19];

    static FsrsState initial();
    static ReviewResult review(const FsrsState &state, int rating, qint64 nowMs);
    static double retrievability(const FsrsState &state, qint64 nowMs);
    static int interval(double stability);
    static double initialStability(int rating);
    static double initialDifficulty(int rating);

private:
    static double clamp(double value, double low, double high);
};
