#pragma once

#include <QObject>

class TestTheme : public QObject {
    Q_OBJECT

private slots:
    void themeFallsBack();
    void themeReadsAccent();
    void translatorFollowsLanguage();
};
