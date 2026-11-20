#include "fsrs.h"

#include <QtMath>

const double Fsrs::weights[19] = {
    0.4197, 1.1869, 3.0412, 15.2441, 7.1434, 0.6477, 1.0007, 0.0674, 1.6597, 0.1712,
    1.1178, 2.0225, 0.0904, 0.3025,  2.1214, 0.2498, 2.9466, 0.4891, 0.6468,
};

double Fsrs::clamp(double value, double low, double high) {
    return qBound(low, value, high);
}

FsrsState Fsrs::initial() {
    return FsrsState{};
}

double Fsrs::initialStability(int rating) {
    const int index = clamp(rating, 1, 4) - 1;
    return qMax(0.1, weights[index]);
}

double Fsrs::initialDifficulty(int rating) {
    const int grade = static_cast<int>(clamp(rating, 1, 4));
    return clamp(weights[4] - qExp(weights[5] * (grade - 1)) + 1.0, 1.0, 10.0);
}

int Fsrs::interval(double stability, double retention) {
    const double desired = qBound(Config::kMinRetention, retention, Config::kMaxRetention);
    const double modifier = (qPow(desired, 1.0 / decay) - 1.0) / factor;
    // Clamp before converting to int: a large stability at low retention can
    // otherwise overflow the int cast and wrap around to the one-day floor.
    const double days = clamp(stability * modifier, 1.0, static_cast<double>(maximumInterval));
    return qRound(days);
}

double Fsrs::retrievability(const FsrsState &state, qint64 nowMs) {
    if (state.lastReview <= 0 || state.stability <= 0.0)
        return 0.0;
    const double elapsed = qMax(0.0, (nowMs - state.lastReview) / 86400000.0);
    return qPow(1.0 + factor * elapsed / state.stability, decay);
}

ReviewResult Fsrs::review(const FsrsState &state, int rating, qint64 nowMs, double retention) {
    const int grade = static_cast<int>(clamp(rating, 1, 4));
    const double elapsedDays = state.lastReview > 0 ? qMax(0.0, (nowMs - state.lastReview) / 86400000.0) : 0.0;

    FsrsState next = state;
    double stability = state.stability;
    double difficulty = state.difficulty;
    double recall = retrievability(state, nowMs);

    if (state.reps == 0 || stability <= 0.0 || difficulty <= 0.0) {
        // First contact with the card: the grade sets both memory dimensions.
        stability = initialStability(grade);
        difficulty = initialDifficulty(grade);
        recall = 1.0;
    } else if (elapsedDays < 1.0) {
        // Same-day repeats use the FSRS short-term branch; they do not reset
        // the whole memory state and keep intervals sane for rapid cramming.
        const double increase = qExp(weights[17] * (grade - 3 + weights[18]));
        stability = qMax(0.1, stability * (grade == 1 ? increase : qMax(1.0, increase)));
        difficulty =
            clamp(weights[7] * initialDifficulty(4) + (1.0 - weights[7]) * (difficulty - weights[6] * (grade - 3)), 1.0,
                  10.0);
    } else {
        if (grade == 1) {
            // Forget: post-lapse stability is capped at the short-term branch.
            const double longTerm = weights[11] * qPow(difficulty, -weights[12]) *
                                    (qPow(stability + 1.0, weights[13]) - 1.0) * qExp((1.0 - recall) * weights[14]);
            const double cap = stability / qExp(weights[17] * weights[18]);
            stability = qMin(longTerm, cap);
        } else {
            const double growth = qExp(weights[8]) * (11.0 - difficulty) * qPow(stability, -weights[9]) *
                                  (qExp((1.0 - recall) * weights[10]) - 1.0);
            stability = stability * (1.0 + growth);
        }
        difficulty =
            clamp(weights[7] * initialDifficulty(4) + (1.0 - weights[7]) * (difficulty - weights[6] * (grade - 3)), 1.0,
                  10.0);
    }

    next.stability = qMax(0.1, stability);
    next.difficulty = difficulty;
    next.lastReview = nowMs;
    ++next.reps;
    if (grade == 1)
        ++next.lapses;

    const int days = interval(next.stability, retention);
    next.due = nowMs + static_cast<qint64>(days) * 86400000LL;

    return ReviewResult{next, days, recall};
}
