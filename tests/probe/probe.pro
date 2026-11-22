include(../sources.pri)

TEMPLATE = app
TARGET = omacards_probe

isEmpty(PROBE_SOURCE) {
    error("Pass PROBE_SOURCE=<absolute-path-to-probe.cpp> to qmake")
}
SOURCES += $$PROBE_SOURCE
