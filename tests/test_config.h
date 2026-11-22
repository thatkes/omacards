#pragma once

#include <QObject>

class TestConfig : public QObject {
    Q_OBJECT

private slots:
    void configDayStartHourHelpers();
    void configAgainDelayFallsBackAndClamps();
};
