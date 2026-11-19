QT += core gui widgets qml quick quickcontrols2 dbus sql

CONFIG += c++17 release
TARGET = omacards
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/config.h \
    src/deck.h \
    src/domain.h \
    src/filehistory.h \
    src/fsrs.h \
    src/importer.h \
    src/mathrender.h \
    src/store.h \
    src/studyqueue.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/deck.cpp \
    src/domain.cpp \
    src/filehistory.cpp \
    src/fsrs.cpp \
    src/importer.cpp \
    src/mathrender.cpp \
    src/store.cpp \
    src/studyqueue.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
