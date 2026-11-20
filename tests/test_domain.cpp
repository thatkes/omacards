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
