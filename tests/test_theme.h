#pragma once

#include <QObject>

class TestTheme : public QObject {
    Q_OBJECT

private slots:
    void themeFallsBack();
    void themeReadsAccent();
    void translatorFollowsLanguage();
    void themeReadColorFallbacks();
    void themeReadColorsAllKeys();
    void themeAccentForegroundLuminance();
    void themeReloadMapsPalette();
    void themeReloadEmitsChangedOnceOnPaletteChange();
    void themeRadiusFromHyprctl();
    void themeRadiusIgnoresInvalidValues();
};
