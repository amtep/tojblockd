QT += testlib
QT -= gui
TARGET = test-fat
DEPENDPATH += ../..
INCLUDEPATH += ../..

# Input
SOURCES += tst_fat.cpp
SOURCES += ../../fat.cpp
HEADERS += ../../fat.h
SOURCES += ../../dir.cpp  # for dir_fill
HEADERS += ../../dir.h
SOURCES += ../../vfat.cpp  # for filemap_fill
HEADERS += ../../fat.h
SOURCES += ../../ConvertUTF.cpp  # for vfat.cpp
HEADERS += ../../ConvertUTF.h
