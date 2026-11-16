# Every source the app and the tests both compile. main.cpp stays out.
HEADERS += \
    $$PWD/adb.h \
    $$PWD/capture.h \
    $$PWD/cli.h \
    $$PWD/flows.h \
    $$PWD/graph.h \
    $$PWD/iconprovider.h \
    $$PWD/icons.h \
    $$PWD/monitor.h \
    $$PWD/names.h \
    $$PWD/render.h \
    $$PWD/report.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/adb.cpp \
    $$PWD/capture.cpp \
    $$PWD/cli.cpp \
    $$PWD/core_stubs.cpp \
    $$PWD/iconprovider.cpp \
    $$PWD/icons_stub.cpp \
    $$PWD/monitor.cpp \
    $$PWD/names.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp
