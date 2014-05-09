QT += testlib
QT -= gui
TARGET = test-fat
DEPENDPATH += ../..
INCLUDEPATH += ../..

# Input
SOURCES += tst_fat.cpp
SOURCES += ../../fat.cpp
