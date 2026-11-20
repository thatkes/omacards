#pragma once

#include <QObject>

class TestDomain : public QObject {
    Q_OBJECT

private slots:
    void domainParsesSharedValues();
    void domainModeIsTotalAndCaseSensitive();
    void domainModeRoundTrips();
    void domainRatingConversionsAreExact();
    void domainCardTypeIsCaseSensitive();
    void domainCardTypeRoundTrips();
};
