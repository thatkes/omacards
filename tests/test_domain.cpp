#include <QtTest>

#include "test_domain.h"
#include "test_support.h"
#include "domain.h"

using namespace TestSupport;

void TestDomain::domainParsesSharedValues() {
    QCOMPARE(Domain::modeFromString("cram"), StudyMode::Cram);
    QCOMPARE(Domain::modeFromString("anything"), StudyMode::Review);
    QCOMPARE(Domain::toString(StudyMode::Review), QString("review"));
    QCOMPARE(Domain::ratingFromInt(1), Rating::Again);
    QCOMPARE(Domain::ratingFromInt(3), Rating::Good);
    QCOMPARE(Domain::ratingFromInt(2), Rating::Invalid);
    QCOMPARE(Domain::cardTypeFromString("cloze"), CardType::Cloze);
    QCOMPARE(Domain::cardTypeFromString(""), CardType::Basic);
    QCOMPARE(Domain::cardTypeFromString("other"), CardType::Invalid);
}

void TestDomain::domainModeIsTotalAndCaseSensitive() {
    // Every string other than the exact lowercase "cram" is Review. This is
    // the boundary rule documented in TYPE_SAFETY.md; callers such as deck
    // parsing decide separately whether to normalize the text first.
    QCOMPARE(Domain::modeFromString("review"), StudyMode::Review);
    QCOMPARE(Domain::modeFromString("CRAM"), StudyMode::Review);
    QCOMPARE(Domain::modeFromString("Cram"), StudyMode::Review);
    QCOMPARE(Domain::modeFromString(" cram"), StudyMode::Review);
    QCOMPARE(Domain::modeFromString("cram "), StudyMode::Review);
    QCOMPARE(Domain::modeFromString(""), StudyMode::Review);
}

void TestDomain::domainModeRoundTrips() {
    QCOMPARE(Domain::toString(StudyMode::Cram), QString("cram"));
    for (StudyMode mode : {StudyMode::Review, StudyMode::Cram}) {
        const QString text = Domain::toString(mode);
        QCOMPARE(Domain::modeFromString(text), mode);
    }
}

void TestDomain::domainRatingConversionsAreExact() {
    // Only 1 and 3 are valid at the boundary; no other integer (including
    // negatives and the FSRS-internal grades 2 and 4) maps to a Rating.
    for (int value : {-100, -1, 0, 2, 4, 5, 100})
        QCOMPARE(Domain::ratingFromInt(value), Rating::Invalid);

    QCOMPARE(Domain::toInt(Rating::Invalid), 0);
    QCOMPARE(Domain::toInt(Rating::Again), 1);
    QCOMPARE(Domain::toInt(Rating::Good), 3);

    for (Rating rating : {Rating::Again, Rating::Good})
        QCOMPARE(Domain::ratingFromInt(Domain::toInt(rating)), rating);
    // Invalid is a sentinel, not a grade: it does not round-trip through int.
    QCOMPARE(Domain::ratingFromInt(Domain::toInt(Rating::Invalid)), Rating::Invalid);

    // toInt() is a raw cast, not a validating conversion: it preserves even
    // values that no boundary parser would accept.
    QCOMPARE(Domain::toInt(static_cast<Rating>(99)), 99);
}

void TestDomain::domainCardTypeIsCaseSensitive() {
    QCOMPARE(Domain::cardTypeFromString("basic"), CardType::Basic);
    QCOMPARE(Domain::cardTypeFromString("Basic"), CardType::Invalid);
    QCOMPARE(Domain::cardTypeFromString("CLOZE"), CardType::Invalid);
    QCOMPARE(Domain::cardTypeFromString(" cloze"), CardType::Invalid);
    QCOMPARE(Domain::cardTypeFromString("cloze "), CardType::Invalid);
    QCOMPARE(Domain::cardTypeFromString("future"), CardType::Invalid);
}

void TestDomain::domainCardTypeRoundTrips() {
    QCOMPARE(Domain::toString(CardType::Basic), QString("basic"));
    QCOMPARE(Domain::toString(CardType::Cloze), QString("cloze"));
    // Invalid deliberately collapses to the canonical basic text.
    QCOMPARE(Domain::toString(CardType::Invalid), QString("basic"));

    QCOMPARE(Domain::cardTypeFromString(Domain::toString(CardType::Basic)), CardType::Basic);
    QCOMPARE(Domain::cardTypeFromString(Domain::toString(CardType::Cloze)), CardType::Cloze);
    QCOMPARE(Domain::cardTypeFromString(Domain::toString(CardType::Invalid)), CardType::Basic);
}
