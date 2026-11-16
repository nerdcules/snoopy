#include <QtTest>

#include "adb.h"
#include "capture.h"
#include "tests.h"

namespace {

// A runner that answers from a table of "arguments joined by spaces" to
// what the process printed, and records every call.
struct FakeAdb {
    QHash<QString, ProcessResult> answers;
    QStringList calls;

    Runner runner() {
        return [this](const QString &, const QStringList &arguments, int) {
            const QString key = arguments.join(' ');
            calls.append(key);
            ProcessResult result = answers.value(key);
            if (!answers.contains(key)) {
                result.code = 1;
                result.err = "unexpected: " + key.toUtf8();
            }
            result.started = true;
            return result;
        };
    }

    void answer(const QString &key, const QByteArray &out, int code = 0) {
        ProcessResult result;
        result.out = out;
        result.code = code;
        answers.insert(key, result);
    }
};

class AppTests : public QObject {
    Q_OBJECT

private slots:
    void parsesDevices() {
        const auto devices = parseDevices("List of devices attached\n"
                                          "R58M123 device usb:1-1 product:x model:Pixel_8 transport_id:3\r\n"
                                          "emulator-5554\tunauthorized\n\n");
        QCOMPARE(devices.size(), 2);
        QCOMPARE(devices.at(0).first, QString("R58M123"));
        QCOMPARE(devices.at(0).second, QString("device"));
        QCOMPARE(devices.at(1).second, QString("unauthorized"));
    }

    void resolvesTheOneReadyPhone() {
        FakeAdb fake;
        fake.answer("devices -l", "List of devices attached\nR58M123 device usb:1-1\nOTHER offline\n");
        Adb adb({}, fake.runner(), "/usr/bin/adb");
        QCOMPARE(adb.resolveSerial(), QString("R58M123"));
        QCOMPARE(adb.serial(), QString("R58M123"));
    }

    void saysWhatIsWrongWithThePhone_data() {
        QTest::addColumn<QByteArray>("listing");
        QTest::addColumn<QString>("expected");
        QTest::newRow("none") << QByteArray("List of devices attached\n\n") << QString("No Android device is connected");
        QTest::newRow("unauthorized") << QByteArray("List of devices attached\nR58 unauthorized\n")
                                      << QString("A phone is connected but not ready: R58 (unauthorized)");
        QTest::newRow("two") << QByteArray("List of devices attached\nA device\nB device\n")
                             << QString("More than one device is connected");
    }

    void saysWhatIsWrongWithThePhone() {
        QFETCH(QByteArray, listing);
        QFETCH(QString, expected);
        FakeAdb fake;
        fake.answer("devices -l", listing);
        Adb adb({}, fake.runner(), "/usr/bin/adb");
        try {
            adb.resolveSerial();
            QFAIL("resolveSerial should have thrown");
        } catch (const AdbError &error) {
            QVERIFY2(error.message().startsWith(expected), qPrintable(error.message()));
        }
    }

    void missingAdbIsReported() {
        Adb adb({}, FakeAdb().runner(), QString());
        try {
            adb.devices();
            QFAIL("devices should have thrown");
        } catch (const AdbError &error) {
            QVERIFY(error.message().contains("android-tools"));
        }
    }

    void shellPassesTheSerialAndNormalizesLineEnds() {
        FakeAdb fake;
        fake.answer("-s R58 shell cat /proc/net/tcp", "a\r\nb\rc\n");
        Adb adb("R58", fake.runner(), "/usr/bin/adb");
        const Adb::Shell result = adb.shell("cat /proc/net/tcp");
        QCOMPARE(result.out, QString("a\nb\nc\n"));
        QCOMPARE(result.code, 0);
    }

    void collectsEveryDumpAndRecordsFailures() {
        FakeAdb fake;
        fake.answer("devices -l", "List of devices attached\nR58 device\n");
        const QString prefix = "-s R58 shell ";
        fake.answer(prefix + "pm list packages -U -f", "package:/data/app/x/base.apk=com.example uid:10123\n");
        fake.answer(prefix + "getprop ro.product.model; getprop ro.build.version.release; getprop ro.build.version.sdk",
                    "Pixel 8\n15\n35\n");
        // grep finding no DNS lines exits 1 with nothing on stderr: not an error.
        fake.answer(prefix + "logcat -d -t 3000 | grep -Ei 'getaddrinfo|DnsResolver|libnetd_resolv|qname=|host='", "", 1);
        Adb adb({}, fake.runner(), "/usr/bin/adb");
        const Capture capture = collectSnapshot(adb);

        QCOMPARE(capture.text("packages"), QString("package:/data/app/x/base.apk=com.example uid:10123\n"));
        QCOMPARE(capture.meta.value("model").toString(), QString("Pixel 8"));
        QCOMPARE(capture.meta.value("android").toString(), QString("15"));
        QCOMPARE(capture.meta.value("sdk").toString(), QString("35"));
        QCOMPARE(capture.meta.value("serial").toString(), QString("R58"));
        const QJsonObject errors = capture.meta.value("errors").toObject();
        QVERIFY(!errors.contains("dns"));
        QVERIFY(!errors.contains("packages"));
        // Every other dump answered "unexpected" with exit 1, so each is recorded.
        QVERIFY(errors.value("appops").toString().startsWith("unexpected"));
        QCOMPARE(errors.size(), Capture::fields.size() - 2);
    }

    void savesAndLoadsACapture() {
        QTemporaryDir directory;
        Capture capture;
        for (const QString &field : Capture::fields)
            capture.texts.insert(field, field + " text\n");
        capture.meta.insert("model", "Pixel 8");
        QString error;
        QVERIFY2(saveCapture(capture, directory.filePath("cap"), &error), qPrintable(error));

        Capture loaded;
        QVERIFY(loadCapture(directory.filePath("cap"), &loaded, &error));
        QCOMPARE(loaded.texts, capture.texts);
        QCOMPARE(loaded.meta.value("model").toString(), QString("Pixel 8"));
        QVERIFY(!loadCapture(directory.filePath("missing"), &loaded, &error));
        QVERIFY(error.contains("does not exist"));
    }

    void loadsAnAndroidDiagnoserCapture() {
        Capture capture;
        QString error;
        QVERIFY(loadCapture(SNOOPY_FIXTURES "/demo", &capture, &error));
        QVERIFY(!capture.text("appops").isEmpty());
        QVERIFY(!capture.text("package_dump").isEmpty());
        QVERIFY(!capture.meta.isEmpty());
    }
};

} // namespace

int runAppTests(int argc, char **argv) {
    AppTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "app_tests.moc"
