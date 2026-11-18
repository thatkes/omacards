QT += core gui widgets qml quick quickcontrols2 dbus sql

CONFIG += c++17 release
TARGET = omacards
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/deck.h \
    src/fsrs.h \
    src/mathrender.h \
    src/store.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/deck.cpp \
    src/fsrs.cpp \
    src/mathrender.cpp \
    src/store.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
