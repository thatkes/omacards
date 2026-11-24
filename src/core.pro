QT += core gui widgets qml quick quickcontrols2 dbus sql

TEMPLATE = lib
CONFIG += staticlib c++17 release
TARGET = omacardscore

INCLUDEPATH += $$PWD

HEADERS += \
    backend.h \
    cli.h \
    config.h \
    deck.h \
    deckidresolver.h \
    deckrepository.h \
    domain.h \
    filehistory.h \
    fsrs.h \
    importer.h \
    mathrender.h \
    store.h \
    studyplanner.h \
    insessionprogression.h \
    ratingrecording.h \
    studysession.h \
    theme.h \
    translator.h

SOURCES += \
    backend.cpp \
    cli.cpp \
    deck.cpp \
    deckidresolver.cpp \
    deckrepository.cpp \
    domain.cpp \
    filehistory.cpp \
    fsrs.cpp \
    importer.cpp \
    mathrender.cpp \
    store.cpp \
    studyplanner.cpp \
    insessionprogression.cpp \
    ratingrecording.cpp \
    studysession.cpp \
    theme.cpp \
    translator.cpp
