#pragma once

#include <QString>

// Values shared by the QML boundary, the CLI and the study scheduler. QString
// remains the public representation for deck files and QML properties; these
// types keep the internal branches from drifting apart.
enum class StudyMode { Review, Cram };
enum class Rating { Invalid = 0, Again = 1, Good = 3 };
enum class CardType { Invalid, Basic, Cloze };

namespace Domain {
StudyMode modeFromString(const QString &text);
QString toString(StudyMode mode);

Rating ratingFromInt(int value);
int toInt(Rating rating);

CardType cardTypeFromString(const QString &text);
QString toString(CardType type);
} // namespace Domain
