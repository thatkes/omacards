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
#include "test_ratingrecording.h"
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

// Update this when registering a suite; the runtime check keeps the vector
// and the announced suite count from drifting apart.
constexpr int kExpectedSuiteCount = 17;

bool isInformationalMode(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-functions") == 0 || std::strcmp(argv[i], "-help") == 0 ||
            std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "-?") == 0)
            return true;
    }
    return false;
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
    TestRatingRecording ratingrecording;
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
        {QStringLiteral("TestRatingRecording"), &ratingrecording},
        {QStringLiteral("TestImporter"), &importer},
        {QStringLiteral("TestDeckRepository"), &deckrepository},
        {QStringLiteral("TestTranslator"), &translator},
    };

    if (suites.size() != kExpectedSuiteCount) {
        QTextStream err(stderr);
        err << "ERROR: expected " << kExpectedSuiteCount << " registered suites, saw " << suites.size() << '\n';
        return 2;
    }

    // bin/check-test-summary compares this with the number of Totals lines.
    // Announce the plan before any suite runs so a truncated run cannot pass.
    if (isInformationalMode(argc, argv)) {
        QTextStream out(stdout);
        out << "OMACARDS_TEST_MODE: info\n";
        out.flush();
    } else {
        const int plannedSuites = hasSuite ? 1 : kExpectedSuiteCount;
        QTextStream out(stdout);
        out << "OMACARDS_TEST_SUITES: " << plannedSuites << '\n';
        out.flush();
    }

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
