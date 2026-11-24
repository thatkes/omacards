QT += core gui qml quick quickcontrols2 testlib dbus sql
CONFIG += c++17

INCLUDEPATH += $$PWD/../src
DEFINES += OMACARDS_SOURCE_DIR=\\\"$$PWD/../src\\\"

# The CLI reads its command-path registry from the Qt resource system, so the
# test binary must embed the same resource as the app.
RESOURCES += $$PWD/../src/resources.qrc
