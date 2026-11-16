# Every source the app and the tests both compile. main.cpp stays out.
HEADERS += \
    $$PWD/adb.h \
    $$PWD/apk.h \
    $$PWD/capture.h \
    $$PWD/catalog.h \
    $$PWD/cli.h \
    $$PWD/flows.h \
    $$PWD/graph.h \
    $$PWD/iconprovider.h \
    $$PWD/icons.h \
    $$PWD/monitor.h \
    $$PWD/names.h \
    $$PWD/parsers.h \
    $$PWD/render.h \
    $$PWD/report.h \
    $$PWD/theme.h \
    $$PWD/translator.h

SOURCES += \
    $$PWD/adb.cpp \
    $$PWD/apk.cpp \
    $$PWD/capture.cpp \
    $$PWD/catalog.cpp \
    $$PWD/cli.cpp \
    $$PWD/flows.cpp \
    $$PWD/graph.cpp \
    $$PWD/iconprovider.cpp \
    $$PWD/icons.cpp \
    $$PWD/monitor.cpp \
    $$PWD/names.cpp \
    $$PWD/parsers.cpp \
    $$PWD/render.cpp \
    $$PWD/report.cpp \
    $$PWD/theme.cpp \
    $$PWD/translator.cpp

# Raw deflate for APK entries (icons). Qt itself links the same zlib.
LIBS += -lz
