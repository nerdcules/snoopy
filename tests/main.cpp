#include <QGuiApplication>

#include "tests.h"

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    int failed = 0;
    failed += runAppTests(argc, argv);
    failed += runReportTests(argc, argv);
    failed += runIconTests(argc, argv);
    failed += runWatchTests(argc, argv);
    return failed == 0 ? 0 : 1;
}
