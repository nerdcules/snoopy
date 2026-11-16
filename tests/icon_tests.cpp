#include <QtTest>

#include "tests.h"

namespace {
class IconTests : public QObject {
    Q_OBJECT
private slots:
    void placeholder() {}
};
} // namespace

int runIconTests(int argc, char **argv) {
    IconTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "icon_tests.moc"
