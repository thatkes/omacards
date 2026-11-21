#include <QGuiApplication>
#include <QTextStream>
#include <QVector>
#include <QtTest>

#include <cstring>

#include "test_cli.h"
#include "test_backend.h"
#include "test_insessionprogression.h"
#include "test_store.h"
#include "test_fsrs.h"
#include "test_theme.h"
#include "test_deck.h"
#include "test_mathrender.h"
#include "test_filehistory.h"
#include "test_config.h"
#include "test_domain.h"
#include "test_studyplanner.h"
#include "test_studysession.h"
#include "test_importer.h"
#include "test_deckrepository.h"
#include "test_translator.h"

namespace {

struct Suite {
    QString name;
    QObject *object;
};

bool takeSuite(int &argc, char **argv, QString *name, bool *present) {
    *present = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--suite") != 0)
            continue;
        *present = true;
        if (i + 1 >= argc)
            return false;
        *name = QString::fromLocal8Bit(argv[i + 1]);
        for (int j = i + 2; j < argc; ++j)
            argv[j - 2] = argv[j];
        argc -= 2;
        return true;
    }
    return true;
}

void printSuites(const QVector<Suite> &suites) {
    QTextStream err(stderr);
    err << "Available suites:\n";
    for (const Suite &suite : suites)
        err << "  " << suite.name << '\n';
}

} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    Q_UNUSED(app);

    QString selected;
    bool hasSuite = false;
    if (!takeSuite(argc, argv, &selected, &hasSuite)) {
        QTextStream err(stderr);
        err << "Usage: omacards_tests [--suite <name>] [QtTest options]\n";
        return 2;
    }

    TestCli cli;
    TestBackend backend;
    TestInSessionProgression progression;
    TestStore store;
    TestFsrs fsrs;
    TestTheme theme;
    TestDeck deck;
    TestMathRender mathrender;
    TestFileHistory filehistory;
    TestConfig config;
    TestDomain domain;
    TestStudyPlanner studyplanner;
    TestStudySession studysession;
    TestImporter importer;
    TestDeckRepository deckrepository;
    TestTranslator translator;

    const QVector<Suite> suites = {
        {QStringLiteral("TestCli"), &cli},
        {QStringLiteral("TestBackend"), &backend},
        {QStringLiteral("TestInSessionProgression"), &progression},
        {QStringLiteral("TestStore"), &store},
        {QStringLiteral("TestFsrs"), &fsrs},
        {QStringLiteral("TestTheme"), &theme},
        {QStringLiteral("TestDeck"), &deck},
        {QStringLiteral("TestMathRender"), &mathrender},
        {QStringLiteral("TestFileHistory"), &filehistory},
        {QStringLiteral("TestConfig"), &config},
        {QStringLiteral("TestDomain"), &domain},
        {QStringLiteral("TestStudyPlanner"), &studyplanner},
        {QStringLiteral("TestStudySession"), &studysession},
        {QStringLiteral("TestImporter"), &importer},
        {QStringLiteral("TestDeckRepository"), &deckrepository},
        {QStringLiteral("TestTranslator"), &translator},
    };

    if (hasSuite) {
        for (const Suite &suite : suites) {
            if (suite.name != selected)
                continue;
            return QTest::qExec(suite.object, argc, argv) == 0 ? 0 : 1;
        }
        QTextStream err(stderr);
        err << "Unknown suite: " << selected << '\n';
        printSuites(suites);
        return 2;
    }

    int failed = 0;
    for (const Suite &suite : suites)
        failed += QTest::qExec(suite.object, argc, argv);
    return failed == 0 ? 0 : 1;
}
