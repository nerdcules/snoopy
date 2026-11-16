# Every source the app and the tests both compile. main.cpp stays out.
HEADERS += \
    $$PWD/adb.h \
    $$PWD/apk.h \
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
    $$PWD/apk.cpp \
    $$PWD/capture.cpp \
    $$PWD/core_stubs.cpp \
    $$PWD/icons.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp

# Raw deflate for APK entries (icons). Qt itself links the same zlib.
LIBS += -lz
