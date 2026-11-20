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

CardTypeValue parseCardType(const QString &text) {
    if (text == QLatin1String("cloze"))
        return {CardType::Cloze, QStringLiteral("cloze")};
    if (text.isEmpty() || text == QLatin1String("basic"))
        return {CardType::Basic, QStringLiteral("basic")};
    return {CardType::Invalid, text};
}

QString toString(const CardTypeValue &value) {
    return value.text;
}

CardType cardTypeFromString(const QString &text) {
    return parseCardType(text).kind;
}

QString toString(CardType type) {
    return type == CardType::Cloze ? QStringLiteral("cloze") : QStringLiteral("basic");
}

} // namespace Domain
