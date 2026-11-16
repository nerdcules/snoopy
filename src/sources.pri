# Every source the app and the tests both compile. main.cpp stays out.
HEADERS += \
    $$PWD/adb.h \
    $$PWD/capture.h \
    $$PWD/catalog.h \
    $$PWD/flows.h \
    $$PWD/graph.h \
    $$PWD/icons.h \
    $$PWD/parsers.h \
    $$PWD/render.h \
    $$PWD/report.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/adb.cpp \
    $$PWD/capture.cpp \
    $$PWD/catalog.cpp \
    $$PWD/flows.cpp \
    $$PWD/graph.cpp \
    $$PWD/icons_stub.cpp \
    $$PWD/parsers.cpp \
    $$PWD/render.cpp \
    $$PWD/report.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp
