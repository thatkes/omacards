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
    // Only 1 and 3 are valid Ratings at the boundary; no other integer
    // (including negatives and the FSRS algorithm inputs 2 and 4) maps to
    // a Rating.
    for (int value : {-100, -1, 0, 2, 4, 5, 100})
        QCOMPARE(Domain::ratingFromInt(value), Rating::Invalid);

    QCOMPARE(Domain::toInt(Rating::Invalid), 0);
    QCOMPARE(Domain::toInt(Rating::Again), 1);
    QCOMPARE(Domain::toInt(Rating::Good), 3);

    for (Rating rating : {Rating::Again, Rating::Good})
        QCOMPARE(Domain::ratingFromInt(Domain::toInt(rating)), rating);
    // Invalid is a sentinel, not a Rating: it does not round-trip through int.
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

void TestDomain::domainCardTypeValueRetainsBoundaryText() {
    const CardTypeValue canonicalBasic = Domain::parseCardType(QStringLiteral("basic"));
    QCOMPARE(canonicalBasic.kind, CardType::Basic);
    QCOMPARE(Domain::toString(canonicalBasic), QString("basic"));

    const CardTypeValue empty = Domain::parseCardType(QString());
    QCOMPARE(empty.kind, CardType::Basic);
    QCOMPARE(Domain::toString(empty), QString("basic"));

    const CardTypeValue cloze = Domain::parseCardType(QStringLiteral("cloze"));
    QCOMPARE(cloze.kind, CardType::Cloze);
    QCOMPARE(Domain::toString(cloze), QString("cloze"));

    // Unknown and noncanonical text is retained verbatim; only file parsing
    // decides whether to trim or lowercase before reaching this parser.
    const CardTypeValue unknown = Domain::parseCardType(QStringLiteral("future"));
    QCOMPARE(unknown.kind, CardType::Invalid);
    QCOMPARE(Domain::toString(unknown), QString("future"));
    const CardTypeValue upper = Domain::parseCardType(QStringLiteral("CLOZE"));
    QCOMPARE(upper.kind, CardType::Invalid);
    QCOMPARE(Domain::toString(upper), QString("CLOZE"));

    for (const QString &text : {QStringLiteral("basic"), QStringLiteral("cloze"), QStringLiteral("future"),
                                QStringLiteral("CLOZE"), QStringLiteral(" cloze "), QString()}) {
        const CardTypeValue value = Domain::parseCardType(text);
        QCOMPARE(Domain::cardTypeFromString(text), value.kind);
        QCOMPARE(Domain::toString(value), text.isEmpty() ? QStringLiteral("basic") : text);
    }
}
