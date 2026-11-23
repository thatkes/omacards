QT += core gui qml quick quickcontrols2 testlib dbus sql
CONFIG += c++17

INCLUDEPATH += $$PWD/../src
DEFINES += OMACARDS_SOURCE_DIR=\\\"$$PWD/../src\\\"

HEADERS += \
    $$PWD/../src/backend.h \
    $$PWD/../src/cli.h \
    $$PWD/../src/config.h \
    $$PWD/../src/deck.h \
    $$PWD/../src/deckrepository.h \
    $$PWD/../src/domain.h \
    $$PWD/../src/filehistory.h \
    $$PWD/../src/fsrs.h \
    $$PWD/../src/importer.h \
    $$PWD/../src/mathrender.h \
    $$PWD/../src/store.h \
    $$PWD/../src/studyplanner.h \
    $$PWD/../src/insessionprogression.h \
    $$PWD/../src/ratingrecording.h \
    $$PWD/../src/studysession.h \
    $$PWD/../src/theme.h \
    $$PWD/../src/translator.h

SOURCES += \
    $$PWD/../src/backend.cpp \
    $$PWD/../src/cli.cpp \
    $$PWD/../src/deck.cpp \
    $$PWD/../src/deckrepository.cpp \
    $$PWD/../src/domain.cpp \
    $$PWD/../src/filehistory.cpp \
    $$PWD/../src/fsrs.cpp \
    $$PWD/../src/importer.cpp \
    $$PWD/../src/mathrender.cpp \
    $$PWD/../src/store.cpp \
    $$PWD/../src/studyplanner.cpp \
    $$PWD/../src/insessionprogression.cpp \
    $$PWD/../src/ratingrecording.cpp \
    $$PWD/../src/studysession.cpp \
    $$PWD/../src/theme.cpp \
    $$PWD/../src/translator.cpp

# The CLI reads its command-path registry from the Qt resource system, so the
# test binary must embed the same resource as the app.
RESOURCES += $$PWD/../src/resources.qrc
