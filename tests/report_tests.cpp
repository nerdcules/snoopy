#include <QtTest>

#include "tests.h"

namespace {
class ReportTests : public QObject {
    Q_OBJECT
private slots:
    void placeholder() {}
};
} // namespace

int runReportTests(int argc, char **argv) {
    ReportTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "report_tests.moc"
