include(sources.pri)

CONFIG += testcase
TARGET = omacards_tests
TEMPLATE = app

HEADERS += test_support.h test_cli.h test_backend.h test_insessionprogression.h test_store.h test_fsrs.h test_theme.h test_deck.h test_mathrender.h test_filehistory.h test_config.h test_domain.h \
           test_studyplanner.h test_studysession.h test_ratingrecording.h test_importer.h test_deckrepository.h test_translator.h
SOURCES += test_main.cpp test_cli.cpp test_backend.cpp test_insessionprogression.cpp test_store.cpp test_fsrs.cpp test_theme.cpp test_deck.cpp test_mathrender.cpp test_filehistory.cpp test_config.cpp test_domain.cpp \
           test_studyplanner.cpp test_studysession.cpp test_ratingrecording.cpp test_importer.cpp test_deckrepository.cpp test_translator.cpp
