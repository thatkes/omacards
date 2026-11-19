#include "domain.h"

namespace Domain {

StudyMode modeFromString(const QString &text) {
    return text == QLatin1String("cram") ? StudyMode::Cram : StudyMode::Review;
}

QString toString(StudyMode mode) {
    return mode == StudyMode::Cram ? QStringLiteral("cram") : QStringLiteral("review");
}

Rating ratingFromInt(int value) {
    switch (value) {
    case static_cast<int>(Rating::Again):
        return Rating::Again;
    case static_cast<int>(Rating::Good):
        return Rating::Good;
    default:
        return Rating::Invalid;
    }
}

int toInt(Rating rating) {
    return static_cast<int>(rating);
}

CardType cardTypeFromString(const QString &text) {
    if (text == QLatin1String("cloze"))
        return CardType::Cloze;
    if (text.isEmpty() || text == QLatin1String("basic"))
        return CardType::Basic;
    return CardType::Invalid;
}

QString toString(CardType type) {
    return type == CardType::Cloze ? QStringLiteral("cloze") : QStringLiteral("basic");
}

} // namespace Domain
