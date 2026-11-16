QT += core gui qml quick quickcontrols2 svg concurrent dbus network testlib
CONFIG += c++17 testcase
TARGET = snoopy_tests
TEMPLATE = app

INCLUDEPATH += ../src
include(../src/sources.pri)

HEADERS += tests.h
SOURCES += main.cpp app_tests.cpp report_tests.cpp icon_tests.cpp watch_tests.cpp
RESOURCES += ../src/resources.qrc
DEFINES += SNOOPY_FIXTURES=\\\"$$PWD/fixtures\\\"
