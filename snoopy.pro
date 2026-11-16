QT += core gui qml quick quickcontrols2 svg concurrent

CONFIG += c++17 release
TARGET = snoopy
TEMPLATE = app

# Shared with tests/snoopy_tests.pro.
include(src/sources.pri)

SOURCES += src/main.cpp
RESOURCES += src/resources.qrc
