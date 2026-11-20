#pragma once

#include <QString>

// Values shared by the QML boundary, the CLI and the study scheduler. QString
// remains the public representation for deck files and QML properties; these
// types keep the internal branches from drifting apart.
enum class StudyMode { Review, Cram };
enum class Rating { Invalid = 0, Again = 1, Good = 3 };
enum class CardType { Invalid, Basic, Cloze };

// A typed decision plus the exact text seen at the boundary. Unknown file
// types stay Invalid but keep their text so load -> save and load -> JSON do
// not silently rewrite the deck file.
struct CardTypeValue {
    CardType kind = CardType::Basic;
    QString text = QStringLiteral("basic");
};

namespace Domain {
StudyMode modeFromString(const QString &text);
QString toString(StudyMode mode);

Rating ratingFromInt(int value);
int toInt(Rating rating);

CardTypeValue parseCardType(const QString &text);
QString toString(const CardTypeValue &value);

CardType cardTypeFromString(const QString &text);
QString toString(CardType type);
} // namespace Domain
