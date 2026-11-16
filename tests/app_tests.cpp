#include <QtTest>

#include "adb.h"
#include "capture.h"
#include "monitor.h"
#include "names.h"
#include "qrimage.h"
#include "wireless.h"
#include "flows.h"
#include "graph.h"
#include "report.h"

#include <QJSEngine>
#include <QSignalSpy>
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

// A fake phone for pairing: it announces the pairing service under
// whatever name `announced` holds, accepts `password`, then announces
// its connect service.
struct FakeWirelessPhone {
    QMutex mutex;
    QString announced;
    QString password;
    bool paired = false;
    bool refuse = false;
    QStringList calls;

    Runner runner() {
        return [this](const QString &, const QStringList &arguments, int) {
            QMutexLocker lock(&mutex);
            const QString command = arguments.join(' ');
            calls.append(command);
            ProcessResult result;
            result.started = true;
            result.code = 0;
            if (command == "mdns services") {
                QString out = "List of discovered mdns services\n";
                if (!announced.isEmpty() && !paired)
                    out += announced + "\t_adb-tls-pairing._tcp\t192.0.2.5:41231\n";
                if (paired)
                    out += "adb-PHONE1-ab12\t_adb-tls-connect._tcp\t192.0.2.5:36115\n";
                result.out = out.toUtf8();
            } else if (arguments.value(0) == "pair") {
                paired = !refuse && arguments.value(2) == password;
                if (paired) {
                    result.out = "Successfully paired to 192.0.2.5:41231 [guid=adb-PHONE1-ab12]\n";
                } else {
                    result.code = 1;
                    result.err = "Failed: Wrong password or connection was dropped.\n";
                }
            } else if (command == "devices -l") {
                result.out = "List of devices attached\n";
            } else if (arguments.value(0) == "connect") {
                result.out = "connected to 192.0.2.5:36115\n";
            }
            return result;
        };
    }

    // What the phone reads from the QR code.
    void scan(const QString &qr) {
        QMutexLocker lock(&mutex);
        announced = qr.section("S:", 1).section(';', 0, 0);
        if (password.isEmpty())
            password = qr.section("P:", 1).section(';', 0, 0);
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
    // graph.js is the window's layout. It runs here in a plain JS engine.
    QJSValue graphScript(QJSEngine &engine) {
        QFile file(":/graph.js");
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QString code = QString::fromUtf8(file.readAll());
        code.remove(".pragma library");
        engine.evaluate(code);
        return engine.globalObject();
    }

    void graphSharesNodesAndOrdersByLinks() {
        QJSEngine engine;
        QJSValue g = graphScript(engine);
        engine.evaluate(R"(
            var cards = [
              {package: "a.one", name: "One", reads: [{label: "Location", detail: "x"}],
               destinations: [{label: "192.0.2.1", kind: "Encrypted", endpoint: "192.0.2.1:443", relation: "connects", detail: "d1"}]},
              {package: "a.two", name: "Two", reads: [{label: "Location", detail: "y"}, {label: "Camera", detail: "z"}],
               destinations: [{label: "192.0.2.1", kind: "Encrypted", endpoint: "192.0.2.1:443", relation: "connects", detail: "d2"},
                              {label: "ads.example", kind: "DNS", endpoint: "ads.example", relation: "lookup", detail: "d3"}]}
            ]
            var built = build(cards)
            var height = layout(built, 1000, {gap: 100, minGap: 80, pad: 50, left: 100, right: 120})
        )");
        QCOMPARE(engine.evaluate("built.apps.length").toInt(), 2);
        // Location is read by both apps: one node with two lines.
        QCOMPARE(engine.evaluate("built.infos.length").toInt(), 2);
        QCOMPARE(engine.evaluate("built.infos.filter(n => n.label === 'Location')[0].lines.length").toInt(), 2);
        // The same endpoint reached the same way is one destination.
        QCOMPARE(engine.evaluate("built.dests.length").toInt(), 2);
        QCOMPARE(engine.evaluate("built.edges.filter(e => e.relation === 'LOOKUP').length").toInt(), 1);
        // Columns at their x, apps in the middle.
        QCOMPARE(engine.evaluate("built.infos[0].x").toNumber(), 100.0);
        QCOMPARE(engine.evaluate("built.apps[0].x").toNumber(), 500.0);
        QCOMPARE(engine.evaluate("built.dests[0].x").toNumber(), 880.0);
        // Location links to both apps (y 0 and 100 before padding), so it
        // sits between them; Camera only to the second.
        QCOMPARE(engine.evaluate("built.infos.filter(n => n.label === 'Location')[0].y").toNumber(), 50.0 + 50.0);
        // Nodes in a column are at least minGap apart.
        QVERIFY(engine.evaluate("built.infos[1].y - built.infos[0].y >= 80").toBool());
        QVERIFY(engine.evaluate("height").toNumber() >= 100 + 100);
    }

    void graphSpreadsAppsOverATallerColumn() {
        QJSEngine engine;
        graphScript(engine);
        engine.evaluate(R"(
            var reads = ["Location","Camera","Microphone","Contacts","Clipboard"].map(l => ({label: l, detail: ""}))
            var built = build([{package: "a.one", name: "One", reads: reads, destinations: []},
                               {package: "a.two", name: "Two", reads: [], destinations: []}])
            layout(built, 1000, {gap: 100, minGap: 80, pad: 0, left: 100, right: 120})
        )");
        // Five information nodes 80 apart span 320, so the two apps do too.
        QCOMPARE(engine.evaluate("built.apps[1].y - built.apps[0].y").toNumber(), 320.0);
    }

    void graphFiltersAndFindsNeighbours() {
        QJSEngine engine;
        graphScript(engine);
        engine.evaluate(R"(
            var cards = [{package: "a.maps", name: "Maps", reads: [], destinations: [{label: "tiles.example", kind: "DNS", relation: "lookup", detail: ""}]},
                         {package: "a.chat", name: "Chat", reads: [], destinations: []}]
        )");
        QCOMPARE(engine.evaluate("filterCards(cards, '  TILES ').length").toInt(), 1);
        QCOMPARE(engine.evaluate("filterCards(cards, '').length").toInt(), 2);
        QCOMPARE(engine.evaluate("filterCards(cards, 'nothing').length").toInt(), 0);
        engine.evaluate("var built = build(cards); var hot = neighbors(built, 'app:a.maps')");
        QCOMPARE(engine.evaluate("Object.keys(hot).length").toInt(), 3); // the app, its destination, the edge
        QCOMPARE(engine.evaluate("prettyName('a-very-long-host-name-that-goes-on.edge.region.cdn.example.com')").toString(),
                 QString("a-very-long-host-name-that….example.com"));
    }

    void namesLookUpAddressesOnce() {
        QStringList asked;
        QList<std::function<void(QString)>> pending;
        Names names([&](const QString &address, QObject *, std::function<void(QString)> done) {
            asked.append(address);
            pending.append(done);
        });
        QSignalSpy resolved(&names, &Names::resolved);
        names.warm({"192.0.2.1", "[2001:db8::1]", "example.com", "192.0.2.1", ""});
        // Names from DNS lookups are not reverse-resolved; each address once.
        QCOMPARE(asked, QStringList({"192.0.2.1", "2001:db8::1"}));
        QCOMPARE(names.cached("192.0.2.1"), QString());
        pending.at(0)("edge.example.net");
        pending.at(1)("");
        QCOMPARE(names.cached("192.0.2.1"), QString("edge.example.net"));
        QCOMPARE(names.cached("[2001:db8::1]"), QString());
        QCOMPARE(resolved.count(), 1); // only a found name redraws
        names.warm({"192.0.2.1", "2001:db8::1"});
        QCOMPARE(asked.size(), 2); // a miss is remembered too
    }

    void monitorRecordsWhatAppearedAndStopped() {
        Capture first, second;
        QString error;
        QVERIFY(loadCapture(SNOOPY_FIXTURES "/watch-before", &first, &error));
        QVERIFY(loadCapture(SNOOPY_FIXTURES "/watch-after", &second, &error));
        const QJsonObject before = buildReport(first), after = buildReport(second);

        Monitor monitor;
        QSignalSpy events(&monitor, &Monitor::eventsChanged);
        monitor.relay(before);
        // The first sample is the baseline.
        QCOMPARE(monitor.status(), QString("live"));
        QCOMPARE(monitor.samples(), 1);
        QVERIFY(monitor.events().isEmpty());
        QCOMPARE(QJsonArray::fromVariantList(monitor.graph()),
                 graphFrom(projectReport(before).value("flows").toArray(), projectReport(before).value("apps").toArray()));

        // What the second sample should record, worked out here from the flows.
        auto keys = [](const QJsonObject &report) {
            QStringList list;
            for (const QJsonValue &flow : projectReport(report).value("flows").toArray()) {
                const QJsonObject f = flow.toObject();
                list.append(QStringList{f.value("kind").toString(), f.value("source").toString(), f.value("destination").toString(),
                                        f.value("information").toString(), f.value("via").toString()}.join('|'));
            }
            return QSet<QString>(list.begin(), list.end());
        };
        const QSet<QString> a = keys(before), b = keys(after);
        const int appeared = int((b - a).size()), stopped = int((a - b).size());
        QVERIFY2(appeared > 0 && stopped > 0, "the watch fixtures should differ both ways");

        monitor.relay(after);
        QCOMPARE(monitor.samples(), 2);
        int seenAppeared = 0, seenStopped = 0;
        for (const QVariant &event : monitor.events())
            (event.toMap().value("change") == "appeared" ? seenAppeared : seenStopped)++;
        QCOMPARE(seenAppeared, appeared);
        QCOMPARE(seenStopped, stopped);
        // Newest first; within a sample, appeared before stopped.
        QCOMPARE(monitor.events().first().toMap().value("change").toString(), QString("appeared"));

        // The same sample again changes nothing.
        const int count = int(monitor.events().size());
        monitor.relay(after);
        QCOMPARE(monitor.events().size(), count);
    }

    void monitorKeepsTheLastSampleWhileWaiting() {
        Capture capture;
        QString error;
        QVERIFY(loadCapture(SNOOPY_FIXTURES "/demo", &capture, &error));
        Monitor monitor;
        monitor.relay(buildReport(capture));
        const QVariantList graph = monitor.graph();
        QVERIFY(!graph.isEmpty());
        monitor.setWaiting("No Android device is connected.");
        QCOMPARE(monitor.status(), QString("waiting"));
        QCOMPARE(monitor.graph(), graph);
        QCOMPARE(monitor.device().value("model").toString(), QString("Pixel 8"));
    }

    void readsAdbMdnsServices() {
        const QList<MdnsService> found = parseMdnsServices(
            "List of discovered mdns services\n"
            "adb-0A1B2C3D-Xk4Q2z\t_adb-tls-connect._tcp\t192.168.8.83:36115\n"
            "snoopy-abc123\t_adb-tls-pairing._tcp\t192.168.8.83:41231\n"
            "garbage line\n");
        QCOMPARE(found.size(), 2);
        QCOMPARE(found.at(0).instance, QString("adb-0A1B2C3D-Xk4Q2z"));
        QCOMPARE(found.at(1).type, QString("_adb-tls-pairing._tcp"));
        QCOMPARE(found.at(1).address, QString("192.168.8.83:41231"));
    }

    void namesPhonesBehindSerials() {
        QCOMPARE(phoneOf("adb-0A1B2C3D-Xk4Q2z._adb-tls-connect._tcp"), QString("0A1B2C3D"));
        QCOMPARE(phoneOf("0A1B2C3D"), QString("0A1B2C3D"));
        QVERIFY(isWireless("adb-0A1B2C3D-Xk4Q2z._adb-tls-connect._tcp"));
        QVERIFY(isWireless("192.168.8.83:36115"));
        QVERIFY(!isWireless("0A1B2C3D"));
        QVERIFY(!isWireless("emulator-5554"));
    }

    void pairingCodeScans() {
        QCOMPARE(pairingQrText("snoopy-abc123", "pw"), QString("WIFI:T:ADB;S:snoopy-abc123;P:pw;;"));
        const QImage code = qrImage(pairingQrText("snoopy-abc123", "k3y9mx2pqa"), 4);
        QVERIFY(!code.isNull());
        QCOMPARE(code.width(), code.height());
        // Quiet zone light, the finder pattern's corner dark.
        QCOMPARE(code.pixel(0, 0), qRgb(255, 255, 255));
        QCOMPARE(code.pixel(4 * 4, 4 * 4), qRgb(0, 0, 0));
    }

    void cablePreferredForOnePhone() {
        FakeAdb fake;
        fake.answer("devices -l", "List of devices attached\n"
                                  "adb-0A1B2C3D-Xk4Q2z._adb-tls-connect._tcp device product:husky\n"
                                  "0A1B2C3D device usb:2-1\n");
        Adb adb({}, fake.runner(), "/usr/bin/adb");
        QCOMPARE(adb.resolveSerial(), QString("0A1B2C3D"));
        // Two different phones are still two.
        FakeAdb two;
        two.answer("devices -l", "List of devices attached\nAAA device\nadb-BBB-x1._adb-tls-connect._tcp device\n");
        Adb both({}, two.runner(), "/usr/bin/adb");
        QVERIFY_THROWS_EXCEPTION(AdbError, both.resolveSerial());
    }

    void pairsAndConnectsOverWifi() {
        FakeWirelessPhone phone;
        Pairing pairing(phone.runner(), "/usr/bin/adb");
        pairing.setPollInterval(20);
        QSignalSpy connected(&pairing, &Pairing::connected);
        pairing.start();
        QCOMPARE(pairing.state(), QString("waiting"));
        const QString qr = pairing.qrText();
        QVERIFY(qr.section("S:", 1).startsWith("snoopy-"));
        QCOMPARE(qr.section("P:", 1).section(';', 0, 0).size(), 10);
        // Nothing happens until the phone scans the code.
        QTest::qWait(100);
        QCOMPARE(pairing.state(), QString("waiting"));
        phone.scan(qr);
        QVERIFY(connected.wait(5000));
        QCOMPARE(pairing.state(), QString("connected"));
        QCOMPARE(pairing.serial(), QString("192.0.2.5:36115"));
        QMutexLocker lock(&phone.mutex);
        QVERIFY(phone.calls.contains("pair 192.0.2.5:41231 " + phone.password));
        QVERIFY(phone.calls.contains("connect 192.0.2.5:36115"));
    }

    void pairingReportsARefusal() {
        FakeWirelessPhone phone;
        phone.refuse = true;
        Pairing pairing(phone.runner(), "/usr/bin/adb");
        pairing.setPollInterval(20);
        pairing.start();
        phone.scan(pairing.qrText());
        QTRY_COMPARE_WITH_TIMEOUT(pairing.state(), QString("failed"), 5000);
        QVERIFY2(pairing.message().contains("Wrong password"), qPrintable(pairing.message()));
        // Starting again makes a new name and password.
        const QString before = pairing.qrText();
        pairing.start();
        QVERIFY(pairing.qrText() != before);
        pairing.cancel();
        QCOMPARE(pairing.state(), QString("idle"));
    }

    void reconnectsAnnouncedPhones() {
        FakeAdb fake;
        fake.answer("devices -l", "List of devices attached\n");
        fake.answer("mdns services", "List of discovered mdns services\n"
                                     "adb-PHONE1-ab12\t_adb-tls-connect._tcp\t192.0.2.5:36115\n"
                                     "other-thing\t_adb-tls-pairing._tcp\t192.0.2.9:4000\n");
        fake.answer("connect 192.0.2.5:36115", "connected to 192.0.2.5:36115\n");
        Adb adb({}, fake.runner(), "/usr/bin/adb");
        QCOMPARE(connectAnnounced(adb), 1);
        QVERIFY(fake.calls.contains("connect 192.0.2.5:36115"));
        QVERIFY(!fake.calls.contains("connect 192.0.2.9:4000"));
    }

    void monitorWaitsWhenThePhoneCantBeRead() {
        Monitor monitor({}, [](const QString &) {
            Sample sample;
            sample.error = "No Android device is connected. Enable USB debugging and plug the phone in.";
            return sample;
        });
        QSignalSpy sampled(&monitor, &Monitor::sampled);
        monitor.setPaused(true); // one sample, no timer after it
        monitor.sampleNow();
        QVERIFY(sampled.wait(5000));
        QCOMPARE(monitor.status(), QString("waiting"));
        QVERIFY(monitor.message().startsWith("No Android device"));
        QCOMPARE(monitor.samples(), 0);
    }

    void monitorIntervalIsBounded() {
        Monitor monitor({}, [](const QString &) { return Sample(); });
        monitor.setInterval(1);
        QCOMPARE(monitor.interval(), 5);
        monitor.setInterval(1000);
        QCOMPARE(monitor.interval(), 300);
    }
};

} // namespace

int runAppTests(int argc, char **argv) {
    AppTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "app_tests.moc"
