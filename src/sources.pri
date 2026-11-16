# Every source the app and the tests both compile. main.cpp stays out.
HEADERS += \
    $$PWD/adb.h \
    $$PWD/capture.h \
    $$PWD/flows.h \
    $$PWD/graph.h \
    $$PWD/icons.h \
    $$PWD/render.h \
    $$PWD/report.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/adb.cpp \
    $$PWD/capture.cpp \
    $$PWD/core_stubs.cpp \
    $$PWD/icons_stub.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp
