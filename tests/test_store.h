#pragma once

#include <QObject>

class TestStore : public QObject {
    Q_OBJECT

private slots:
    void storeRoundTripsState();
    void storeRemembersSettings();
    void storeCountsNewCardsToday();
    void storeUsesConfiguredDayStart();
};
