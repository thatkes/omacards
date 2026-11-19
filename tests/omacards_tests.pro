QT += core gui qml quick quickcontrols2 testlib dbus sql
CONFIG += c++17 testcase
TARGET = omacards_tests
TEMPLATE = app

INCLUDEPATH += ../src
DEFINES += OMACARDS_SOURCE_DIR=\\\"$$PWD/../src\\\"

HEADERS += ../src/backend.h ../src/cli.h ../src/deck.h ../src/domain.h ../src/fsrs.h \
           ../src/importer.h ../src/mathrender.h ../src/store.h ../src/theme.h \
           ../src/translator.h
SOURCES += omacards_tests.cpp ../src/backend.cpp ../src/cli.cpp ../src/deck.cpp ../src/domain.cpp \
           ../src/fsrs.cpp ../src/importer.cpp ../src/mathrender.cpp ../src/store.cpp \
           ../src/theme.cpp ../src/translator.cpp
