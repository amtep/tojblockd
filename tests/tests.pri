QT += testlib
QT -= gui
CONFIG += debug
DEPENDPATH += ../..
INCLUDEPATH += ../..
QMAKE_LFLAGS += -fprofile-arcs -ftest-coverage
QMAKE_CXXFLAGS += -fprofile-arcs -ftest-coverage
QMAKE_CXXFLAGS_DEBUG += -O0
DEFINES += TESTING

SOURCES += ../helpers.cpp
HEADERS += ../helpers.h
