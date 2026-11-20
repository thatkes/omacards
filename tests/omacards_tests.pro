QT += core gui qml quick quickcontrols2 testlib dbus sql
CONFIG += c++17 testcase
TARGET = omacards_tests
TEMPLATE = app

INCLUDEPATH += ../src
DEFINES += OMACARDS_SOURCE_DIR=\\\"$$PWD/../src\\\"

HEADERS += test_support.h test_cli.h test_backend.h test_studyqueue.h test_store.h test_fsrs.h test_theme.h test_deck.h test_mathrender.h test_filehistory.h test_config.h test_domain.h \
           ../src/backend.h ../src/cli.h ../src/config.h ../src/deck.h ../src/deckrepository.h ../src/domain.h ../src/filehistory.h ../src/fsrs.h \
           ../src/importer.h ../src/mathrender.h ../src/store.h ../src/studyqueue.h \
           ../src/theme.h \
           ../src/translator.h
SOURCES += test_main.cpp test_cli.cpp test_backend.cpp test_studyqueue.cpp test_store.cpp test_fsrs.cpp test_theme.cpp test_deck.cpp test_mathrender.cpp test_filehistory.cpp test_config.cpp test_domain.cpp \
           ../src/backend.cpp ../src/cli.cpp ../src/deck.cpp ../src/deckrepository.cpp ../src/domain.cpp ../src/filehistory.cpp \
           ../src/fsrs.cpp ../src/importer.cpp ../src/mathrender.cpp ../src/store.cpp \
           ../src/studyqueue.cpp ../src/theme.cpp ../src/translator.cpp
