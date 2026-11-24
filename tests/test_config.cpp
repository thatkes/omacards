#include <QtTest>
#include <QByteArray>
#include <QDateTime>

#include "test_config.h"
#include "test_support.h"
#include "config.h"

using namespace TestSupport;

void TestConfig::configDayStartHourHelpers() {
    const qint64 dueNight = QDateTime(QDate(2026, 11, 19), QTime(22, 49)).toMSecsSinceEpoch();
    const qint64 beforeDayStart = QDateTime(QDate(2026, 11, 19), QTime(1, 0)).toMSecsSinceEpoch();
    const qint64 atDayStart = QDateTime(QDate(2026, 11, 19), QTime(3, 0)).toMSecsSinceEpoch();
    const qint64 previousDay = QDateTime(QDate(2026, 11, 18), QTime(23, 59)).toMSecsSinceEpoch();

    // A midnight day-start hour means due at any time on the 19th is due
    // for the whole day.
    QVERIFY(Config::isDueByDay(dueNight, QDateTime(QDate(2026, 11, 19), QTime(0, 1)).toMSecsSinceEpoch(), 0));
    QVERIFY(!Config::isDueByDay(dueNight, previousDay, 0));
    // A 3 AM day-start hour holds that card until 3 AM on the 19th.
    QVERIFY(!Config::isDueByDay(dueNight, beforeDayStart, 3));
    QVERIFY(Config::isDueByDay(dueNight, atDayStart, 3));
    // Unscheduled cards (due == 0) must stay due at any day-start hour.
    QVERIFY(Config::isDueByDay(0, beforeDayStart, 3));
    QCOMPARE(Config::studyDayStartMs(beforeDayStart, 3),
             QDateTime(QDate(2026, 11, 18), QTime(3, 0)).toMSecsSinceEpoch());
    QCOMPARE(Config::studyDayStartMs(beforeDayStart, 0),
             QDateTime(QDate(2026, 11, 19), QTime(0, 0)).toMSecsSinceEpoch());
}

void TestConfig::configAgainDelayFallsBackAndClamps() {
    ScopedEnv guard("OMACARDS_AGAIN_DELAY_MS");

    // No override, non-numeric text, and an empty value all use the default.
    QCOMPARE(Config::againDelayMs(), Config::kAgainDelayMs);
    guard.set("not-a-number");
    QCOMPARE(Config::againDelayMs(), Config::kAgainDelayMs);
    guard.set("");
    QCOMPARE(Config::againDelayMs(), Config::kAgainDelayMs);

    // Valid values pass through unchanged; the supported window is 0..10 min.
    guard.set("0");
    QCOMPARE(Config::againDelayMs(), 0);
    guard.set("12345");
    QCOMPARE(Config::againDelayMs(), 12345);
    guard.set("600000");
    QCOMPARE(Config::againDelayMs(), Config::kMaxAgainDelayMs);

    guard.set("-1");
    QCOMPARE(Config::againDelayMs(), 0);
    guard.set("999999999");
    QCOMPARE(Config::againDelayMs(), Config::kMaxAgainDelayMs);

    // A value with trailing non-digits is rejected as a whole, not parsed as
    // a prefix.
    guard.set("250ms");
    QCOMPARE(Config::againDelayMs(), Config::kAgainDelayMs);
}
