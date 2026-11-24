QT += core gui widgets qml quick quickcontrols2 dbus sql

CONFIG += c++17 release
TARGET = omacards
TEMPLATE = app

INCLUDEPATH += $$PWD/src

CORE_BUILD_DIR = $$OUT_PWD/core
CORE_PROJECT = $$PWD/src/core.pro
include(src/corelink.pri)

SOURCES += \
    src/main.cpp \
    src/traycontroller.cpp

RESOURCES += src/resources.qrc
