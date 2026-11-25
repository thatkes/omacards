#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "test_theme.h"
#include "test_support.h"
#include "theme.h"
#include "translator.h"

using namespace TestSupport;

void TestTheme::themeFallsBack() {
    QTemporaryDir dir;
    Theme theme(dir.path());
    QCOMPARE(theme.accent(), QString("#FFD60A"));
    QCOMPARE(theme.accentForeground(), QString("#0e0e10"));
}

void TestTheme::themeReadsAccent() {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("theme");
    QVERIFY(writeFile(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
    Theme theme(dir.path());
    QCOMPARE(theme.accent(), QString("#112233"));
    QCOMPARE(theme.accentForeground(), QString("#ffffff"));
}

void TestTheme::translatorFollowsLanguage() {
    QTemporaryDir dir;
    writeFile(dir.path() + "/language", "pt-BR\n");
    writeFile(dir.path() + "/pt.json", R"({"Save": "Guardar"})");
    writeFile(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");
    Translator translator(dir.path() + "/language", dir.path());
    QCOMPARE(translator.language(), QString("pt-BR"));
    QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
    QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
    QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
}

void TestTheme::themeReadColorFallbacks() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("colors.toml");

    QCOMPARE(Theme::readColor(path, "accent"), QString());
    QCOMPARE(Theme::readColor(path, "accent", "#FFD60A"), QString("#FFD60A"));

    QVERIFY(writeFile(path, "accent = \"#123456\"\n"));
    QCOMPARE(Theme::readColor(path, "missing", "#abc"), QString("#abc"));
    QCOMPARE(Theme::readColor(path, "accent", "#abc"), QString("#123456"));
}

void TestTheme::themeReadColorsAllKeys() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("colors.toml");
    const QStringList keys{"mode",
                           "accent",
                           "selection",
                           "muted",
                           "background",
                           "dark_background",
                           "darker_background",
                           "lighter_background",
                           "foreground",
                           "dark_foreground",
                           "light_foreground",
                           "bright_foreground",
                           "red",
                           "yellow",
                           "green",
                           "orange",
                           "blue",
                           "cyan",
                           "magenta"};

    QString contents;
    for (const auto &key : keys)
        contents += key + " = \"v-" + key + "\"\n";
    contents += "empty = \"\"\n";
    contents += "unsupported = \"ignored\"\n";
    QVERIFY(writeFile(path, contents.toUtf8()));

    const QHash<QString, QString> colors = Theme::readColors(path);
    QCOMPARE(colors.size(), keys.size());
    for (const auto &key : keys)
        QCOMPARE(colors.value(key), QString("v-" + key));
    QVERIFY(!colors.contains("empty"));
    QVERIFY(!colors.contains("unsupported"));
}

void TestTheme::themeAccentForegroundLuminance() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir(dir.path()).mkpath("theme");
    const QString colors = dir.path() + "/theme/colors.toml";

    QVERIFY(writeFile(colors, "accent = \"#ffffff\"\n"));
    Theme white(dir.path());
    QCOMPARE(white.accent(), QString("#ffffff"));
    QCOMPARE(white.accentForeground(), QString("#0e0e10"));

    QVERIFY(writeFile(colors, "accent = \"#000000\"\n"));
    Theme black(dir.path());
    QCOMPARE(black.accent(), QString("#000000"));
    QCOMPARE(black.accentForeground(), QString("#ffffff"));
}

void TestTheme::themeReloadMapsPalette() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir(dir.path()).mkpath("theme");
    const QString colors = dir.path() + "/theme/colors.toml";

    {
        QVERIFY(writeFile(colors, "mode = \"dark\"\n"));
        Theme theme(dir.path());
        QVERIFY(theme.dark());
        QCOMPARE(theme.background(), QString("#0e0e10"));
        QCOMPARE(theme.foreground(), QString("#c0caf5"));
        QCOMPARE(theme.soft(), QString("#a9b1d6"));
        QCOMPARE(theme.muted(), QString("#565f89"));
        QCOMPARE(theme.panel(), QString("#13141c"));
        QCOMPARE(theme.raised(), QString("#1b1d28"));
        QCOMPARE(theme.border(), QString("#232636"));
        QCOMPARE(theme.red(), QString("#f7768e"));
        QCOMPARE(theme.yellow(), QString("#e0af68"));
        QCOMPARE(theme.green(), QString("#9ece6a"));
    }

    {
        QVERIFY(writeFile(colors, "mode = \"light\"\n"));
        Theme theme(dir.path());
        QVERIFY(!theme.dark());
        QCOMPARE(theme.background(), QString("#ffffff"));
        QCOMPARE(theme.foreground(), QString("#100F0F"));
        QCOMPARE(theme.soft(), QString("#403E3C"));
        QCOMPARE(theme.muted(), QString("#878580"));
        QCOMPARE(theme.panel(), QString("#f5f5f5"));
        QCOMPARE(theme.raised(), QString("#e8e8e8"));
        QCOMPARE(theme.border(), QString("#c0c0c0"));
        QCOMPARE(theme.red(), QString("#d20f39"));
        QCOMPARE(theme.yellow(), QString("#df8e1d"));
        QCOMPARE(theme.green(), QString("#40a02b"));
    }

    {
        QVERIFY(writeFile(colors, "mode = \"dark\"\n"
                                  "accent = \"#abcdef\"\n"
                                  "background = \"#010101\"\n"
                                  "foreground = \"#020202\"\n"
                                  "light_foreground = \"#030303\"\n"
                                  "muted = \"#040404\"\n"
                                  "dark_background = \"#050505\"\n"
                                  "lighter_background = \"#060606\"\n"
                                  "selection = \"#070707\"\n"
                                  "red = \"#080808\"\n"
                                  "yellow = \"#090909\"\n"
                                  "green = \"#0a0a0a\"\n"));
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#abcdef"));
        QCOMPARE(theme.background(), QString("#010101"));
        QCOMPARE(theme.foreground(), QString("#020202"));
        QCOMPARE(theme.soft(), QString("#030303"));
        QCOMPARE(theme.muted(), QString("#040404"));
        QCOMPARE(theme.panel(), QString("#050505"));
        QCOMPARE(theme.raised(), QString("#060606"));
        QCOMPARE(theme.border(), QString("#070707"));
        QCOMPARE(theme.red(), QString("#080808"));
        QCOMPARE(theme.yellow(), QString("#090909"));
        QCOMPARE(theme.green(), QString("#0a0a0a"));
    }
}

void TestTheme::themeReloadEmitsChangedOnceOnPaletteChange() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir(dir.path()).mkpath("theme");
    const QString colors = dir.path() + "/theme/colors.toml";
    QVERIFY(writeFile(colors, "accent = \"#111111\"\n"));

    Theme theme(dir.path());
    QCOMPARE(theme.accent(), QString("#111111"));

    QSignalSpy spy(&theme, &Theme::changed);
    QVERIFY(spy.isValid());

    QVERIFY(writeFile(colors, "accent = \"#222222\"\n"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 2000);
    QCOMPARE(theme.accent(), QString("#222222"));
    QCOMPARE(spy.count(), 1);

    // An identical reload leaves the palette alone and must not emit again.
    QVERIFY(writeFile(colors, "accent = \"#222222\"\n"));
    // The palette debounce is 80 ms; a second emit would arrive in this window.
    QVERIFY(!spy.wait(120));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(theme.accent(), QString("#222222"));
}

void TestTheme::themeRadiusFromHyprctl() {
    if (!hasPosixShell())
        QSKIP("A POSIX shell is required for the fake hyprctl script");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tool = dir.filePath("hyprctl");
    const QByteArray script =
        "#!/bin/sh\n"
        "if [ \"$1\" = \"-j\" ] && [ \"$2\" = \"getoption\" ] && [ \"$3\" = \"decoration:rounding\" ]; then\n"
        "    printf '{\"int\":12}\\n'\n"
        "else\n"
        "    printf '{}\\n'\n"
        "fi\n";
    QVERIFY(writeExecutable(tool, script));

    Theme theme(dir.path(), nullptr, tool);
    QSignalSpy spy(&theme, &Theme::changed);
    QVERIFY(spy.isValid());

    QTRY_VERIFY_WITH_TIMEOUT(theme.radius() == 12, 3000);
    QVERIFY(spy.count() >= 1);
}

void TestTheme::themeRadiusIgnoresInvalidValues() {
    if (!hasPosixShell())
        QSKIP("A POSIX shell is required for the fake hyprctl script");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tool = dir.filePath("hyprctl");

    const QString negativeMarker = dir.filePath("rounding-negative-ran");
    QVERIFY(writeExecutable(tool, QByteArray("#!/bin/sh\nprintf '{\"int\":-5}\\n'\nprintf 'x' > '") +
                                      negativeMarker.toUtf8() + "'\n"));
    {
        Theme theme(dir.path(), nullptr, tool);
        QSignalSpy spy(&theme, &Theme::changed);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(negativeMarker), 2000);
        QCoreApplication::processEvents();
        QCOMPARE(theme.radius(), 0);
        QCOMPARE(spy.count(), 0);
    }

    const QString invalidMarker = dir.filePath("rounding-invalid-ran");
    QVERIFY(writeExecutable(tool, QByteArray("#!/bin/sh\nprintf 'not json\\n'\nprintf 'x' > '") +
                                      invalidMarker.toUtf8() + "'\n"));
    {
        Theme theme(dir.path(), nullptr, tool);
        QSignalSpy spy(&theme, &Theme::changed);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(invalidMarker), 2000);
        QCoreApplication::processEvents();
        QCOMPARE(theme.radius(), 0);
        QCOMPARE(spy.count(), 0);
    }
}
