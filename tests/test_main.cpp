#include <QGuiApplication>
#include <QStringList>
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

// Strip suite selectors before QtTest sees argv: it would read unknown
// positional arguments as test function names. "--suite" keeps its single-suite
// contract; plain suite class names let bin/test run several suites in one
// process. Anything that is not a registered suite name is left for QtTest.
bool takeSelectedSuites(int &argc, char **argv, const QVector<Suite> &suites, QStringList *selected) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--suite") != 0)
            continue;
        if (i + 1 >= argc)
            return false;
        *selected << QString::fromLocal8Bit(argv[i + 1]);
        for (int j = i + 2; j < argc; ++j)
            argv[j - 2] = argv[j];
        argc -= 2;
        break;
    }

    for (int i = 1; i < argc; ++i) {
        const QString candidate = QString::fromLocal8Bit(argv[i]);
        bool known = false;
        for (const Suite &suite : suites) {
            if (suite.name == candidate) {
                known = true;
                break;
            }
        }
        if (!known)
            continue;
        if (!selected->contains(candidate))
            *selected << candidate;
        for (int j = i + 1; j < argc; ++j)
            argv[j - 1] = argv[j];
        --argc;
        --i;
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
        if (std::strcmp(argv[i], "-functions") == 0 || std::strcmp(argv[i], "-datatags") == 0 ||
            std::strcmp(argv[i], "-perfcounterlist") == 0 || std::strcmp(argv[i], "-help") == 0 ||
            std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "-?") == 0)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    Q_UNUSED(app);

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

    QStringList selected;
    if (!takeSelectedSuites(argc, argv, suites, &selected)) {
        QTextStream err(stderr);
        err << "Usage: omacards_tests [--suite <name>] [suite names] [QtTest options]\n";
        return 2;
    }
    for (const QString &name : selected) {
        bool known = false;
        for (const Suite &suite : suites) {
            if (suite.name == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            QTextStream err(stderr);
            err << "Unknown suite: " << name << '\n';
            printSuites(suites);
            return 2;
        }
    }

    // bin/check-test-summary compares this with the number of Totals lines.
    // Announce the plan before any suite runs so a truncated run cannot pass.
    if (isInformationalMode(argc, argv)) {
        QTextStream out(stdout);
        out << "OMACARDS_TEST_MODE: info\n";
        out.flush();
    } else {
        const int plannedSuites = selected.isEmpty() ? kExpectedSuiteCount : selected.size();
        QTextStream out(stdout);
        out << "OMACARDS_TEST_SUITES: " << plannedSuites << '\n';
        out.flush();
    }

    int failed = 0;
    for (const Suite &suite : suites) {
        if (!selected.isEmpty() && !selected.contains(suite.name))
            continue;
        failed += QTest::qExec(suite.object, argc, argv);
    }
    return failed == 0 ? 0 : 1;
}
