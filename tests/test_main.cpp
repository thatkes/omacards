#include <QGuiApplication>
#include <QtTest>

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

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    Q_UNUSED(app);

    int failed = 0;
    TestCli cli;
    failed += QTest::qExec(&cli, argc, argv);
    TestBackend backend;
    failed += QTest::qExec(&backend, argc, argv);
    TestInSessionProgression progression;
    failed += QTest::qExec(&progression, argc, argv);
    TestStore store;
    failed += QTest::qExec(&store, argc, argv);
    TestFsrs fsrs;
    failed += QTest::qExec(&fsrs, argc, argv);
    TestTheme theme;
    failed += QTest::qExec(&theme, argc, argv);
    TestDeck deck;
    failed += QTest::qExec(&deck, argc, argv);
    TestMathRender mathrender;
    failed += QTest::qExec(&mathrender, argc, argv);
    TestFileHistory filehistory;
    failed += QTest::qExec(&filehistory, argc, argv);
    TestConfig config;
    failed += QTest::qExec(&config, argc, argv);
    TestDomain domain;
    failed += QTest::qExec(&domain, argc, argv);
    TestStudyPlanner studyplanner;
    failed += QTest::qExec(&studyplanner, argc, argv);
    TestStudySession studysession;
    failed += QTest::qExec(&studysession, argc, argv);
    TestImporter importer;
    failed += QTest::qExec(&importer, argc, argv);
    TestDeckRepository deckrepository;
    failed += QTest::qExec(&deckrepository, argc, argv);
    TestTranslator translator;
    failed += QTest::qExec(&translator, argc, argv);

    return failed == 0 ? 0 : 1;
}
