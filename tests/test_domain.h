#pragma once

#include <QObject>

class TestDomain : public QObject {
    Q_OBJECT

private slots:
    void domainParsesSharedValues();
};
