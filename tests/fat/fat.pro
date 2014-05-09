QT += testlib
QT -= gui
CONFIG += debug
TARGET = test-fat
DEPENDPATH += ../..
INCLUDEPATH += ../..
QMAKE_LFLAGS += -fprofile-arcs -ftest-coverage
QMAKE_CXXFLAGS += -fprofile-arcs -ftest-coverage
QMAKE_CXXFLAGS_DEBUG += -O0

# Input
SOURCES += tst_fat.cpp
SOURCES += ../../fat.cpp
