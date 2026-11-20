#include <QtTest>
#include <QDir>
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
    QCOMPARE(theme.accentForeground(), QString("black"));
}

void TestTheme::themeReadsAccent() {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("theme");
    QVERIFY(writeFile(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
    Theme theme(dir.path());
    QCOMPARE(theme.accent(), QString("#112233"));
    QCOMPARE(theme.accentForeground(), QString("white"));
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
