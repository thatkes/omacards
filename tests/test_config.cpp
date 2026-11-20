#include <QtTest>
#include <QDateTime>

#include "test_config.h"
#include "test_support.h"
#include "config.h"

using namespace TestSupport;

void TestConfig::configDayStartHelpers() {
    const qint64 dueNight = QDateTime(QDate(2026, 11, 19), QTime(22, 49)).toMSecsSinceEpoch();
    const qint64 beforeRollover = QDateTime(QDate(2026, 11, 19), QTime(1, 0)).toMSecsSinceEpoch();
    const qint64 atRollover = QDateTime(QDate(2026, 11, 19), QTime(3, 0)).toMSecsSinceEpoch();
    const qint64 previousDay = QDateTime(QDate(2026, 11, 18), QTime(23, 59)).toMSecsSinceEpoch();

    // Midnight rollover: due at any time on the 19th means due all day.
    QVERIFY(Config::isDueByDay(dueNight, QDateTime(QDate(2026, 11, 19), QTime(0, 1)).toMSecsSinceEpoch(), 0));
    QVERIFY(!Config::isDueByDay(dueNight, previousDay, 0));
    // A 3 AM rollover holds that card until 3 AM on the 19th.
    QVERIFY(!Config::isDueByDay(dueNight, beforeRollover, 3));
    QVERIFY(Config::isDueByDay(dueNight, atRollover, 3));
    // Unscheduled cards (due == 0) must stay due at any rollover.
    QVERIFY(Config::isDueByDay(0, beforeRollover, 3));
    QCOMPARE(Config::studyDayStartMs(beforeRollover, 3),
             QDateTime(QDate(2026, 11, 18), QTime(3, 0)).toMSecsSinceEpoch());
    QCOMPARE(Config::studyDayStartMs(beforeRollover, 0),
             QDateTime(QDate(2026, 11, 19), QTime(0, 0)).toMSecsSinceEpoch());
}
