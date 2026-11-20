#include <QtTest>
#include <QDir>
#include <QGuiApplication>
#include <QLocale>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "test_translator.h"
#include "test_support.h"
#include "translator.h"

using namespace TestSupport;

namespace {

// Translator::reload() changes process-wide state, so every test that builds
// one puts it back when the test ends.
struct LocaleGuard {
    QLocale locale;
    Qt::LayoutDirection direction;

    LocaleGuard() : locale(QLocale()), direction(QGuiApplication::layoutDirection()) {}

    ~LocaleGuard() {
        QLocale::setDefault(locale);
        QGuiApplication::setLayoutDirection(direction);
    }
};

struct EnvVarGuard {
    QByteArray name;
    QByteArray original;
    bool present;

    explicit EnvVarGuard(const char *variable)
        : name(variable), original(qgetenv(variable)), present(!original.isNull()) {}

    ~EnvVarGuard() {
        if (present)
            qputenv(name.constData(), original);
        else
            qunsetenv(name.constData());
    }
};

} // namespace

void TestTranslator::readLanguageFallbacksAndFormats() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("language");

    QCOMPARE(Translator::readLanguage(path), QString("en"));

    QVERIFY(writeFile(path, "hello world\n"));
    QCOMPARE(Translator::readLanguage(path), QString("en"));

    QVERIFY(writeFile(path, "pt\n"));
    QCOMPARE(Translator::readLanguage(path), QString("pt"));
    QVERIFY(writeFile(path, "fil\n"));
    QCOMPARE(Translator::readLanguage(path), QString("fil"));

    QVERIFY(writeFile(path, "pt-BR\n"));
    QCOMPARE(Translator::readLanguage(path), QString("pt-BR"));
    QVERIFY(writeFile(path, "pt-br\n"));
    QCOMPARE(Translator::readLanguage(path), QString("pt-br"));
    QVERIFY(writeFile(path, "zh-cn\n"));
    QCOMPARE(Translator::readLanguage(path), QString("zh-cn"));

    QVERIFY(writeFile(path, "de\nfr\n"));
    QCOMPARE(Translator::readLanguage(path), QString("de"));

    QVERIFY(writeFile(path, "  es  \n"));
    QCOMPARE(Translator::readLanguage(path), QString("es"));
    QVERIFY(writeFile(path, "\tja\t\n"));
    QCOMPARE(Translator::readLanguage(path), QString("ja"));

    QVERIFY(writeFile(path, "english\n"));
    QCOMPARE(Translator::readLanguage(path), QString("en"));
    QVERIFY(writeFile(path, "abcd\n"));
    QCOMPARE(Translator::readLanguage(path), QString("en"));
    QVERIFY(writeFile(path, "e\n"));
    QCOMPARE(Translator::readLanguage(path), QString("en"));
    QVERIFY(writeFile(path, "EN\n"));
    QCOMPARE(Translator::readLanguage(path), QString("en"));
    QVERIFY(writeFile(path, "pt-BRA\n"));
    QCOMPARE(Translator::readLanguage(path), QString("en"));
    QVERIFY(writeFile(path, "pt-B\n"));
    QCOMPARE(Translator::readLanguage(path), QString("en"));
}

void TestTranslator::pluralCategories() {
    struct Case {
        const char *language;
        long long count;
        const char *category;
    };
    const Case cases[] = {
        // Languages with a single, invariant form.
        {"ja", 0, "other"},
        {"ja", 1, "other"},
        {"ja", 2, "other"},
        {"ko", 1, "other"},
        {"ko", 5, "other"},
        {"zh-CN", 1, "other"},
        {"zh-TW", 2, "other"},
        {"th", 1, "other"},
        {"vi", 1, "other"},
        {"id", 1, "other"},
        {"ms", 1, "other"},

        // n <= 1 is "one".
        {"hi", 0, "one"},
        {"hi", 1, "one"},
        {"hi", 2, "other"},
        {"hi", 5, "other"},
        {"bn", 1, "one"},
        {"bn", 2, "other"},
        {"fa", 1, "one"},
        {"fa", 3, "other"},
        {"si", 0, "one"},
        {"si", 1, "one"},
        {"si", 2, "other"},

        // n <= 1 is "one", exact millions are "many".
        {"fr", 0, "one"},
        {"fr", 1, "one"},
        {"fr", 2, "other"},
        {"fr", 1000000, "many"},
        {"fr", 2000000, "many"},
        {"pt-BR", 0, "one"},
        {"pt-BR", 1, "one"},
        {"pt-BR", 2, "other"},
        {"pt-BR", 1000000, "many"},

        // Only n == 1 is "one", exact millions are "many" (0 is "other").
        {"ca", 0, "other"},
        {"ca", 1, "one"},
        {"ca", 2, "other"},
        {"ca", 1000000, "many"},
        {"es-MX", 1, "one"},
        {"es-MX", 2, "other"},
        {"es-MX", 1000000, "many"},
        {"it", 1, "one"},
        {"it", 1000000, "many"},
        {"pt", 0, "other"},
        {"pt", 1, "one"},
        {"pt", 1000000, "many"},

        // fil, is.
        {"fil", 0, "one"},
        {"fil", 1, "one"},
        {"fil", 4, "other"},
        {"fil", 6, "other"},
        {"fil", 9, "other"},
        {"fil", 11, "one"},
        {"fil", 14, "other"},
        {"is", 0, "other"},
        {"is", 1, "one"},
        {"is", 2, "other"},
        {"is", 11, "other"},
        {"is", 21, "one"},

        // cs/sk.
        {"cs", 0, "other"},
        {"cs", 1, "one"},
        {"cs", 2, "few"},
        {"cs", 4, "few"},
        {"cs", 5, "other"},
        {"sk", 1, "one"},
        {"sk", 3, "few"},
        {"sk", 5, "other"},

        // pl: teens force "many" even though the last digit is 2-4.
        {"pl", 0, "many"},
        {"pl", 1, "one"},
        {"pl", 2, "few"},
        {"pl", 4, "few"},
        {"pl", 5, "many"},
        {"pl", 12, "many"},
        {"pl", 14, "many"},
        {"pl", 22, "few"},

        // ru/uk; negative counts are absolutized.
        {"ru", 0, "many"},
        {"ru", 1, "one"},
        {"ru", 2, "few"},
        {"ru", 4, "few"},
        {"ru", 5, "many"},
        {"ru", 11, "many"},
        {"ru", 12, "many"},
        {"ru", 21, "one"},
        {"ru", 22, "few"},
        {"ru", -1, "one"},
        {"ru", -3, "few"},
        {"ru", -11, "many"},
        {"uk", 1, "one"},
        {"uk", 2, "few"},
        {"uk", 11, "many"},

        // hr/sr differ from ru/uk by using "other" instead of "many".
        {"hr", 0, "other"},
        {"hr", 1, "one"},
        {"hr", 2, "few"},
        {"hr", 4, "few"},
        {"hr", 5, "other"},
        {"hr", 11, "other"},
        {"hr", 21, "one"},
        {"sr", 1, "one"},
        {"sr", 3, "few"},
        {"sr", 12, "other"},

        // sl, lt, lv.
        {"sl", 0, "other"},
        {"sl", 1, "one"},
        {"sl", 2, "two"},
        {"sl", 3, "few"},
        {"sl", 4, "few"},
        {"sl", 5, "other"},
        {"sl", 11, "other"},
        {"sl", 101, "one"},
        {"sl", 102, "two"},
        {"lt", 0, "other"},
        {"lt", 1, "one"},
        {"lt", 2, "few"},
        {"lt", 9, "few"},
        {"lt", 10, "other"},
        {"lt", 11, "other"},
        {"lt", 15, "other"},
        {"lt", 21, "one"},
        {"lt", 22, "few"},
        {"lv", 0, "zero"},
        {"lv", 1, "one"},
        {"lv", 2, "other"},
        {"lv", 10, "zero"},
        {"lv", 11, "zero"},
        {"lv", 21, "one"},

        // ro, ar, he, ga.
        {"ro", 0, "few"},
        {"ro", 1, "one"},
        {"ro", 2, "few"},
        {"ro", 19, "few"},
        {"ro", 20, "other"},
        {"ro", 100, "other"},
        {"ro", 101, "few"},
        {"ar", 0, "zero"},
        {"ar", 1, "one"},
        {"ar", 2, "two"},
        {"ar", -2, "two"},
        {"ar", 3, "few"},
        {"ar", 10, "few"},
        {"ar", 11, "many"},
        {"ar", 99, "many"},
        {"ar", 100, "other"},
        {"ar", 103, "few"},
        {"ar", 111, "many"},
        {"he", 0, "other"},
        {"he", 1, "one"},
        {"he", 2, "two"},
        {"he", 3, "other"},
        {"ga", 0, "other"},
        {"ga", 1, "one"},
        {"ga", 2, "two"},
        {"ga", 3, "few"},
        {"ga", 6, "few"},
        {"ga", 7, "many"},
        {"ga", 10, "many"},
        {"ga", 11, "other"},

        // Default (English and anything unrecognized).
        {"en", 0, "other"},
        {"en", 1, "one"},
        {"en", 2, "other"},
        {"en", -1, "one"},
        {"de", 1, "one"},
        {"de", 5, "other"},
        {"xx", 1, "one"},
    };
    for (const Case &item : cases) {
        const QString actual = Translator::pluralCategory(item.language, item.count);
        QVERIFY2(actual == QLatin1String(item.category),
                 qPrintable(QString("%1 n=%2: expected %3, got %4")
                                .arg(QString::fromLatin1(item.language))
                                .arg(item.count)
                                .arg(QString::fromLatin1(item.category), actual)));
    }

    QCOMPARE(Translator::pluralCategory("ar", -11), Translator::pluralCategory("ar", 11));
    QCOMPARE(Translator::pluralCategory("pl", -22), Translator::pluralCategory("pl", 22));
    QCOMPARE(Translator::pluralCategory("en", -5), Translator::pluralCategory("en", 5));
}

void TestTranslator::formatLocaleRegistryAndFallback() {
    EnvVarGuard omarchy("OMARCHY_PATH");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    qunsetenv("OMARCHY_PATH");
    QCOMPARE(Translator::formatLocale("pt-BR").name(), QLocale("pt-BR").name());
    QCOMPARE(Translator::formatLocale("de").name(), QLocale("de").name());

    const QString registryDir = dir.path() + "/default/i18n";
    QVERIFY(QDir().mkpath(registryDir));
    const QString registry = registryDir + "/locales.json";
    qputenv("OMARCHY_PATH", dir.path().toUtf8());

    QVERIFY(writeFile(registry, R"({"pt-BR": {"formatLocale": "de_DE"}})"));
    QCOMPARE(Translator::formatLocale("pt-BR").name(), QLocale("de_DE").name());
    QCOMPARE(Translator::formatLocale("fr").name(), QLocale("fr").name());

    QVERIFY(writeFile(registry, R"({"pt-BR": {"formatLocale": ""}})"));
    QCOMPARE(Translator::formatLocale("pt-BR").name(), QLocale("pt-BR").name());

    QVERIFY(writeFile(registry, R"({"pt-BR": {}})"));
    QCOMPARE(Translator::formatLocale("pt-BR").name(), QLocale("pt-BR").name());

    QVERIFY(writeFile(registry, "not json"));
    QCOMPARE(Translator::formatLocale("pt-BR").name(), QLocale("pt-BR").name());
}

void TestTranslator::translateStringAndPluralBasics() {
    LocaleGuard locale;
    EnvVarGuard omarchy("OMARCHY_PATH");
    qunsetenv("OMARCHY_PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath("language"), "en\n"));
    QVERIFY(writeFile(dir.filePath("en.json"), R"({
        "Save": "Save!",
        "Items": {"one": "%n item", "other": "%n items"},
        "Blank": "",
        "Number": 42,
        "Flag": true,
        "Nothing": null
    })"));

    Translator translator(dir.filePath("language"), dir.path());

    QCOMPARE(translator.translate("Main", "Save"), QString("Save!"));
    QCOMPARE(translator.translate("Main", "Items", nullptr, -1), QString("%n items"));
    QCOMPARE(translator.translate("Main", "Items", nullptr, 1), QString("%n item"));
    QCOMPARE(translator.translate("Main", "Items", nullptr, 5), QString("%n items"));
    QCOMPARE(translator.translate("Main", "Missing"), QString());
    QCOMPARE(translator.translate("Main", "Blank"), QString());
    QCOMPARE(translator.translate("Main", "Number"), QString());
    QCOMPARE(translator.translate("Main", "Flag"), QString());
    QCOMPARE(translator.translate("Main", "Nothing"), QString());
}

void TestTranslator::translateCatalogFallbackChain() {
    LocaleGuard locale;
    EnvVarGuard omarchy("OMARCHY_PATH");
    qunsetenv("OMARCHY_PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath("language"), "pt-BR\n"));
    QVERIFY(writeFile(dir.filePath("en.json"), R"({
        "Save": "Save",
        "OnlyInEn": "English only",
        "Override": "en value",
        "Items": {"one": "one item", "other": "many items"}
    })"));
    QVERIFY(writeFile(dir.filePath("pt.json"), R"({
        "Save": "Guardar",
        "OnlyInPt": "Portuguese only",
        "Override": "pt value",
        "Items": {"one": "um item", "other": "muitos itens"}
    })"));
    QVERIFY(writeFile(dir.filePath("pt-BR.json"), R"({
        "Override": "pt-BR value",
        "Items": {"one": "um item BR", "other": "muitos itens BR"}
    })"));

    Translator translator(dir.filePath("language"), dir.path());
    QCOMPARE(translator.language(), QString("pt-BR"));

    QCOMPARE(translator.translate("Main", "Save"), QString("Guardar"));
    QCOMPARE(translator.translate("Main", "OnlyInEn"), QString("English only"));
    QCOMPARE(translator.translate("Main", "OnlyInPt"), QString("Portuguese only"));
    QCOMPARE(translator.translate("Main", "Override"), QString("pt-BR value"));
    QCOMPARE(translator.translate("Main", "Items", nullptr, 1), QString("um item BR"));
    QCOMPARE(translator.translate("Main", "Items", nullptr, 5), QString("muitos itens BR"));
}

void TestTranslator::translatePluralFallsBackToOther() {
    LocaleGuard locale;
    EnvVarGuard omarchy("OMARCHY_PATH");
    qunsetenv("OMARCHY_PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath("language"), "ru\n"));
    QVERIFY(writeFile(dir.filePath("ru.json"), R"({"Items": {"one": "one", "few": "few", "other": "other"}})"));

    Translator translator(dir.filePath("language"), dir.path());

    QCOMPARE(translator.translate("Main", "Items", nullptr, 1), QString("one"));
    QCOMPARE(translator.translate("Main", "Items", nullptr, 3), QString("few"));
    // ru/5 selects "many", which is absent, so the "other" form is used.
    QCOMPARE(translator.translate("Main", "Items", nullptr, 5), QString("other"));
    QCOMPARE(translator.translate("Main", "Items", nullptr, -1), QString("other"));
}

void TestTranslator::isEmptyReflectsCatalog() {
    LocaleGuard locale;
    EnvVarGuard omarchy("OMARCHY_PATH");
    qunsetenv("OMARCHY_PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString language = dir.filePath("language");
    QVERIFY(writeFile(language, "en\n"));

    Translator empty(language, dir.path());
    QVERIFY(empty.isEmpty());

    QVERIFY(writeFile(dir.filePath("en.json"), R"({"Blank": ""})"));
    Translator onlyEmptyStrings(language, dir.path());
    QVERIFY(onlyEmptyStrings.isEmpty());

    QVERIFY(writeFile(dir.filePath("en.json"), R"({"Hello": "Hi"})"));
    Translator loaded(language, dir.path());
    QVERIFY(!loaded.isEmpty());
    QCOMPARE(loaded.translate("Main", "Hello"), QString("Hi"));
}

void TestTranslator::changedOnLanguageFileChange() {
    LocaleGuard locale;
    EnvVarGuard omarchy("OMARCHY_PATH");
    qunsetenv("OMARCHY_PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString language = dir.filePath("language");
    QVERIFY(writeFile(language, "en\n"));
    QVERIFY(writeFile(dir.filePath("en.json"), R"({"Save": "Save"})"));
    QVERIFY(writeFile(dir.filePath("de.json"), R"({"Save": "Speichern"})"));

    Translator translator(language, dir.path());
    QCOMPARE(translator.language(), QString("en"));

    QSignalSpy spy(&translator, &Translator::changed);
    QVERIFY(spy.isValid());

    QVERIFY(writeFile(language, "de\n"));
    QTRY_VERIFY_WITH_TIMEOUT(translator.language() == QString("de"), 2000);
    QCOMPARE(translator.translate("Main", "Save"), QString("Speichern"));
    QVERIFY(spy.count() >= 1);
}

void TestTranslator::changedOnCatalogChangeWithLanguageSwitch() {
    LocaleGuard locale;
    EnvVarGuard omarchy("OMARCHY_PATH");
    qunsetenv("OMARCHY_PATH");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString language = dir.filePath("language");
    QVERIFY(writeFile(language, "en\n"));
    QVERIFY(writeFile(dir.filePath("en.json"), R"({"Save": "Save"})"));

    Translator translator(language, dir.path());
    QSignalSpy spy(&translator, &Translator::changed);
    QVERIFY(spy.isValid());

    // A new catalog alone cannot emit changed() because reload() short-circuits
    // when the language code is unchanged. Switching the language file in the
    // same debounce window is what makes the new catalog observable.
    QVERIFY(writeFile(dir.filePath("de.json"), R"({"Save": "Speichern"})"));
    QVERIFY(writeFile(language, "de\n"));
    QTRY_VERIFY_WITH_TIMEOUT(translator.language() == QString("de"), 2000);
    QCOMPARE(translator.translate("Main", "Save"), QString("Speichern"));
    QVERIFY(spy.count() >= 1);
}
