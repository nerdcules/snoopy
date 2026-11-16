#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "capture.h"
#include "flows.h"
#include "graph.h"
#include "parsers.h"
#include "render.h"
#include "report.h"
#include "tests.h"

// The C++ port against android-diagnoser (7b2d820): every capture under
// tests/fixtures is run through each function and compared with the output
// tests/make-golden.py wrote from the Python program, then the Python unit
// tests are repeated case by case.

namespace {

QString fixtures() { return QStringLiteral(SNOOPY_FIXTURES); }

QString golden(const QString &capture, const QString &file) {
    return QDir(fixtures()).filePath(QStringLiteral("golden/%1/%2").arg(capture, file));
}

QStringList captureNames() {
    QStringList names = QDir(fixtures()).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    names.removeAll(QStringLiteral("golden"));
    return names;
}

Capture load(const QString &name) {
    Capture capture;
    QString error;
    if (!loadCapture(QDir(fixtures()).filePath(name), &capture, &error))
        qFatal("%s", qPrintable(error));
    return capture;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

QJsonValue readJson(const QString &path) {
    const QJsonDocument document = QJsonDocument::fromJson(readFile(path));
    if (document.isArray())
        return document.array();
    return document.object();
}

// The value as it reads back from JSON text, so a C++ result and a golden
// file are compared in the same representation.
QJsonValue roundTrip(const QJsonValue &value) {
    const QJsonDocument document = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    const QJsonDocument back = QJsonDocument::fromJson(document.toJson(QJsonDocument::Compact));
    return back.isArray() ? QJsonValue(back.array()) : QJsonValue(back.object());
}

QString shortJson(const QJsonValue &value) {
    QJsonDocument document;
    if (value.isArray())
        document = QJsonDocument(value.toArray());
    else if (value.isObject())
        document = QJsonDocument(value.toObject());
    else
        document = QJsonDocument(QJsonArray{value});
    QString text = QString::fromUtf8(document.toJson(QJsonDocument::Compact));
    if (!value.isArray() && !value.isObject())
        text = text.mid(1, text.size() - 2);
    return text.size() > 300 ? text.left(300) + QStringLiteral("...") : text;
}

// "" when equal, else the first path where they part and both values there.
QString firstDifference(const QJsonValue &got, const QJsonValue &want, const QString &path = QStringLiteral("$")) {
    if (got.type() != want.type())
        return QStringLiteral("%1: type %2, want %3 (got %4, want %5)")
            .arg(path)
            .arg(int(got.type()))
            .arg(int(want.type()))
            .arg(shortJson(got), shortJson(want));
    if (got.isObject()) {
        const QJsonObject a = got.toObject();
        const QJsonObject b = want.toObject();
        for (auto it = b.begin(); it != b.end(); ++it)
            if (!a.contains(it.key()))
                return QStringLiteral("%1.%2: missing, want %3").arg(path, it.key(), shortJson(it.value()));
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (!b.contains(it.key()))
                return QStringLiteral("%1.%2: unexpected %3").arg(path, it.key(), shortJson(it.value()));
            const QString inner = firstDifference(it.value(), b.value(it.key()), path + u'.' + it.key());
            if (!inner.isEmpty())
                return inner;
        }
        return QString();
    }
    if (got.isArray()) {
        const QJsonArray a = got.toArray();
        const QJsonArray b = want.toArray();
        for (qsizetype i = 0; i < std::min(a.size(), b.size()); ++i) {
            const QString inner = firstDifference(a[i], b[i], QStringLiteral("%1[%2]").arg(path).arg(i));
            if (!inner.isEmpty())
                return inner;
        }
        if (a.size() != b.size())
            return QStringLiteral("%1: %2 items, want %3").arg(path).arg(a.size()).arg(b.size());
        return QString();
    }
    if (got != want)
        return QStringLiteral("%1: got %2, want %3").arg(path, shortJson(got), shortJson(want));
    return QString();
}

QString firstLineDifference(const QString &got, const QString &want) {
    const QStringList a = got.split(u'\n');
    const QStringList b = want.split(u'\n');
    for (qsizetype i = 0; i < std::max(a.size(), b.size()); ++i) {
        const QString x = a.value(i, QStringLiteral("<end>"));
        const QString y = b.value(i, QStringLiteral("<end>"));
        if (x != y)
            return QStringLiteral("line %1:\n  got:  %2\n  want: %3").arg(i + 1).arg(x, y);
    }
    return QString();
}

#define COMPARE_JSON(got, wantPath)                                                                          \
    do {                                                                                                     \
        QVERIFY2(QFile::exists(wantPath), qPrintable(QStringLiteral("no golden file ") + (wantPath)));       \
        const QString difference = firstDifference(roundTrip(got), readJson(wantPath));                      \
        QVERIFY2(difference.isEmpty(), qPrintable(QStringLiteral("%1\n  %2").arg(wantPath, difference)));     \
    } while (false)

QJsonObject projected(const QJsonObject &report) { return projectReport(report); }

QJsonArray graphOf(const QJsonObject &report) {
    const QJsonObject flows = projectReport(report);
    return graphFrom(flows.value("flows").toArray(), flows.value("apps").toArray());
}

QSet<QString> fieldSet(const QJsonArray &rows, const char *key) {
    QSet<QString> out;
    for (const QJsonValue &row : rows)
        out.insert(row.toObject().value(QLatin1String(key)).toString());
    return out;
}

using Triple = QString;

Triple triple(const QString &a, const QString &b, const QString &c) { return a + u'\x1f' + b + u'\x1f' + c; }

QSet<Triple> triples(const QJsonArray &flows, const QString &kind, const char *a, const char *b, const char *c) {
    QSet<Triple> out;
    for (const QJsonValue &value : flows) {
        const QJsonObject flow = value.toObject();
        if (flow.value("kind").toString() == kind)
            out.insert(triple(flow.value(QLatin1String(a)).toString(), flow.value(QLatin1String(b)).toString(),
                              c ? flow.value(QLatin1String(c)).toString() : QString()));
    }
    return out;
}

} // namespace

class ReportTests : public QObject {
    Q_OBJECT

private:
    void captureRows() {
        QTest::addColumn<QString>("capture");
        for (const QString &name : captureNames())
            QTest::newRow(qPrintable(name)) << name;
    }

private slots:
    // ------------------------------------------------------------ golden

    void fixturesAndGoldenFilesExist() {
        const QStringList names = captureNames();
        QVERIFY2(names.size() >= 8, qPrintable(names.join(", ")));
        for (const QString &name : names)
            QVERIFY2(QFile::exists(golden(name, "report.json")), qPrintable(name));
    }

    void goldenReport_data() { captureRows(); }
    void goldenReport() {
        QFETCH(QString, capture);
        COMPARE_JSON(buildReport(load(capture)), golden(capture, "report.json"));
    }

    void goldenReportAll_data() { captureRows(); }
    void goldenReportAll() {
        QFETCH(QString, capture);
        COMPARE_JSON(buildReport(load(capture), true), golden(capture, "report-all.json"));
    }

    void goldenReportFilter_data() { captureRows(); }
    void goldenReportFilter() {
        QFETCH(QString, capture);
        const QString wanted = readJson(golden(capture, "manifest.json")).toObject().value("filter").toString();
        const QSet<QString> filter = {wanted};
        COMPARE_JSON(buildReport(load(capture), false, &filter), golden(capture, "report-filter.json"));
    }

    void goldenFlows_data() { captureRows(); }
    void goldenFlows() {
        QFETCH(QString, capture);
        COMPARE_JSON(projected(buildReport(load(capture))), golden(capture, "flows.json"));
    }

    void goldenGraph_data() { captureRows(); }
    void goldenGraph() {
        QFETCH(QString, capture);
        COMPARE_JSON(graphOf(buildReport(load(capture))), golden(capture, "graph.json"));
    }

    // The window passes the report it got from the C++ side; the graph from
    // Python's own report must come out the same.
    void goldenGraphFromPythonFlows_data() { captureRows(); }
    void goldenGraphFromPythonFlows() {
        QFETCH(QString, capture);
        const QJsonObject flows = readJson(golden(capture, "flows.json")).toObject();
        COMPARE_JSON(graphFrom(flows.value("flows").toArray(), flows.value("apps").toArray()), golden(capture, "graph.json"));
        COMPARE_JSON(projectReport(readJson(golden(capture, "report.json")).toObject()), golden(capture, "flows.json"));
    }

    void goldenText_data() { captureRows(); }
    void goldenText() {
        QFETCH(QString, capture);
        const QString want = QString::fromUtf8(readFile(golden(capture, "text.txt")));
        QVERIFY(!want.isEmpty());
        const QString got = renderText(buildReport(load(capture)));
        QVERIFY2(got == want, qPrintable(firstLineDifference(got, want)));
        // And from Python's report, so render alone is checked too.
        const QString fromPython = renderText(readJson(golden(capture, "report.json")).toObject());
        QVERIFY2(fromPython == want, qPrintable(firstLineDifference(fromPython, want)));
    }

    void goldenTextAllAndFiltered_data() { captureRows(); }
    void goldenTextAllAndFiltered() {
        QFETCH(QString, capture);
        const Capture loaded = load(capture);
        const QString wanted = readJson(golden(capture, "manifest.json")).toObject().value("filter").toString();
        const QSet<QString> filter = {wanted};
        const QString all = renderText(buildReport(loaded, true));
        const QString wantAll = QString::fromUtf8(readFile(golden(capture, "text-all.txt")));
        QVERIFY2(all == wantAll, qPrintable(firstLineDifference(all, wantAll)));
        const QString filtered = renderText(buildReport(loaded, false, &filter));
        const QString wantFiltered = QString::fromUtf8(readFile(golden(capture, "text-filter.txt")));
        QVERIFY2(filtered == wantFiltered, qPrintable(firstLineDifference(filtered, wantFiltered)));
    }

    void goldenWatchSnapshot_data() { captureRows(); }
    void goldenWatchSnapshot() {
        QFETCH(QString, capture);
        COMPARE_JSON(watchSnapshot(load(capture)), golden(capture, "watch.json"));
    }

    void goldenWatchDiff_data() {
        QTest::addColumn<QString>("pair");
        const QStringList pairs = QDir(QDir(fixtures()).filePath("golden/watch")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY(pairs.size() >= 2);
        for (const QString &pair : pairs)
            QTest::newRow(qPrintable(pair)) << pair;
    }
    void goldenWatchDiff() {
        QFETCH(QString, pair);
        const QStringList names = pair.split(QStringLiteral("--"));
        QCOMPARE(names.size(), 2);
        const QJsonObject before = watchSnapshot(load(names[0]));
        const QJsonObject after = watchSnapshot(load(names[1]));
        const QString dir = QStringLiteral("watch/") + pair;
        COMPARE_JSON(before, golden(dir, "before.json"));
        COMPARE_JSON(after, golden(dir, "after.json"));
        auto lines = [](const QStringList &events) {
            QString text;
            for (const QString &event : events)
                text += event + u'\n';
            return text;
        };
        const QString forward = lines(diffWatch(before, after));
        const QString wantForward = QString::fromUtf8(readFile(golden(dir, "diff.txt")));
        QVERIFY2(forward == wantForward, qPrintable(firstLineDifference(forward, wantForward)));
        const QString backward = lines(diffWatch(after, before));
        const QString wantBackward = QString::fromUtf8(readFile(golden(dir, "diff-reverse.txt")));
        QVERIFY2(backward == wantBackward, qPrintable(firstLineDifference(backward, wantBackward)));
        // From the snapshots as Python wrote them, too.
        const QJsonObject pyBefore = readJson(golden(dir, "before.json")).toObject();
        const QJsonObject pyAfter = readJson(golden(dir, "after.json")).toObject();
        QCOMPARE(lines(diffWatch(pyBefore, pyAfter)), wantForward);
    }

    // ------------------------------------------------- test_diagnoser.py

    // UidTests.test_formatted_uids
    void formattedUids() {
        QCOMPARE(parseUid("10245"), std::optional<qint64>(10245));
        QCOMPARE(parseUid("u0a245"), std::optional<qint64>(10245));
        QCOMPARE(parseUid("u0s1000"), std::optional<qint64>(1000));
        QCOMPARE(parseUid("u10a1"), std::optional<qint64>(1010001));
        QVERIFY(!parseUid("nope"));
    }

    // ParserTests.test_pm_list_keeps_the_package_after_equals_signs_in_the_path
    void pmListKeepsThePackageAfterEqualsSignsInThePath() {
        const QList<PmPackage> listed = parsePmList(load("demo").text("packages"));
        const PmPackage *demo = nullptr;
        for (const PmPackage &item : listed)
            if (item.name == QLatin1String("com.tracker.demo"))
                demo = &item;
        QVERIFY(demo);
        QCOMPARE(demo->uid, 10245);
        QVERIFY(demo->apkPath && demo->apkPath->contains("base.apk"));
        const QList<PmPackage> simple = parsePmList("package:com.example uid:10001\n");
        QCOMPARE(simple.size(), 1);
        QCOMPARE(simple[0].name, QStringLiteral("com.example"));
        QCOMPARE(simple[0].uid, 10001);
    }

    // ParserTests.test_appops_block_and_inline_formats
    void appopsBlockAndInlineFormats() {
        QHash<QPair<QString, QString>, OpEntry> ops;
        for (const OpEntry &entry : parseAppops(load("demo").text("appops")))
            ops.insert({entry.package, entry.op}, entry);
        const OpEntry fine = ops.value({"com.tracker.demo", "FINE_LOCATION"});
        QCOMPARE(fine.uid, std::optional<qint64>(10245));
        QCOMPARE(fine.mode, std::optional<QString>("foreground"));
        QVERIFY(fine.lastAccess && fine.lastAccess->contains("(-5m ago)"));
        QVERIFY(fine.lastReject && fine.lastReject->contains("(-1h ago)"));
        const OpEntry clip = ops.value({"com.tracker.demo", "READ_CLIPBOARD"});
        QCOMPARE(clip.mode, std::optional<QString>("allow"));
        QCOMPARE(clip.lastAccess, std::optional<QString>("+30s ago"));
        QCOMPARE(ops.value({"com.quiet.notes", "READ_CONTACTS"}).uid, std::optional<qint64>(10299));

        const QList<OpEntry> inl = parseAppops("Uid 10245:\n  Package com.tracker.demo:\n"
                                               "    FINE_LOCATION: foreground; time=+5m ago; rejectTime=+1h ago\n");
        QCOMPARE(inl.size(), 1);
        QCOMPARE(inl[0].mode, std::optional<QString>("foreground"));
        QCOMPARE(inl[0].lastAccess, std::optional<QString>("+5m ago"));
        QCOMPARE(inl[0].lastReject, std::optional<QString>("+1h ago"));
    }

    // ParserTests.test_proc_net_addresses_and_listen_sockets
    void procNetAddressesAndListenSockets() {
        QCOMPARE(parseIp("0100007F"), std::optional<QString>("127.0.0.1"));
        QCOMPARE(parseIp("00000000000000000000000001000000"), std::optional<QString>("::1"));
        const Capture demo = load("demo");
        const QList<SocketInfo> sockets = parseProcNet(demo.text("tcp"), "TCP");
        QCOMPARE(sockets.size(), 1);
        QCOMPARE(sockets[0].state, QStringLiteral("ESTABLISHED"));
        QCOMPARE(sockets[0].remoteIp, QStringLiteral("192.168.8.8"));
        QCOMPARE(sockets[0].remotePort, 443);
        QCOMPARE(sockets[0].uid, 10245);
        const QList<SocketInfo> udp = parseProcNet(demo.text("udp"), "UDP");
        QCOMPARE(udp[0].remoteIp, QStringLiteral("8.8.8.8"));
        QCOMPARE(udp[0].remotePort, 53);
    }

    // ParserTests.test_netstats_ignores_uid_tag_section
    void netstatsIgnoresUidTagSection() {
        const QList<UidTraffic> totals = parseNetstats(load("demo").text("netstats"));
        QCOMPARE(totals.size(), 1);
        QCOMPARE(totals[0].uid, 10245);
        QCOMPARE(totals[0].rxBytes, 81536);
        QCOMPARE(totals[0].txBytes, 12512);
    }

    // ParserTests.test_netstats_reads_the_bpf_uid_table
    void netstatsReadsTheBpfUidTable() {
        const QString text = "\nmAppUidStatsMap:\n"
                             "    uid rxBytes rxPackets txBytes txPackets\n"
                             "    10144 1000 4 50 2\n"
                             "    10491 9 1 8 1\n"
                             "  mStatsMapA:\n"
                             "    ifaceIndex ifaceName tag_hex uid_int cnt_set rxBytes rxPackets txBytes txPackets\n"
                             "    47 wlan0 0x0 1000 1 99999 4 88888 3\n";
        const QList<UidTraffic> totals = parseNetstats(text);
        QCOMPARE(totals.size(), 2);
        QCOMPARE(totals[0].uid, 10144);
        QCOMPARE(totals[0].rxBytes, 1000);
        QCOMPARE(totals[0].txBytes, 50);
        QCOMPARE(totals[1].uid, 10491);
        QCOMPARE(totals[1].rxBytes, 9);
        QCOMPARE(totals[1].txBytes, 8);
    }

    // ParserTests.test_garbage_lines_are_skipped
    void garbageLinesAreSkipped() {
        QVERIFY(parseProcNet("this is not a socket table\n", "TCP").isEmpty());
        QVERIFY(parseAppops("nothing here\n").isEmpty());
        QVERIFY(parseNetstats("Dev stats:\n  rxBytes=5 txBytes=5\n").isEmpty());
    }

    // ReportTests.test_live_app_covers_activity_tracking_and_transmission
    void liveAppCoversActivityTrackingAndTransmission() {
        const QJsonObject report = buildReport(load("demo"));
        const QJsonArray live = report.value("live").toArray();
        QCOMPARE(live.size(), 1);
        const QJsonObject card = live[0].toObject();
        QCOMPARE(card.value("packages").toArray(), QJsonArray{"com.tracker.demo"});
        QCOMPARE(card.value("versions").toObject().value("com.tracker.demo").toString(), QStringLiteral("1.2.3"));
        QStringList doingLines;
        for (const QJsonValue &line : card.value("doing").toArray())
            doingLines.append(line.toString());
        const QString doing = doingLines.join(u'\n');
        for (const char *expected : {"On screen: com.tracker.demo/.Main",
                                     "Process com.tracker.demo pid 4321 (on screen, foreground service)",
                                     "Foreground service com.tracker.demo/.UploadService",
                                     "Scheduled job com.tracker.demo/.sync.SyncJob", "Wakelock UploadLock",
                                     "Sensor android.sensor.accelerometer", "HIGH_ACCURACY",
                                     "Owns the current clipboard contents"})
            QVERIFY2(doing.contains(QLatin1String(expected)), expected);

        const QSet<QString> observed = fieldSet(card.value("observed").toArray(), "op");
        QCOMPARE(observed, (QSet<QString>{"FINE_LOCATION", "COARSE_LOCATION", "CAMERA", "READ_CLIPBOARD"}));
        QVERIFY(!observed.contains("WAKE_LOCK"));
        QCOMPARE(fieldSet(card.value("attempted").toArray(), "op"), QSet<QString>{"RECORD_AUDIO"});
        QVERIFY(fieldSet(card.value("permitted_unseen").toArray(), "op").contains("READ_CONTACTS"));

        const QSet<QString> granted = fieldSet(card.value("declared").toArray(), "permission");
        QVERIFY(granted.contains("android.permission.READ_CONTACTS"));
        QVERIFY(granted.contains("com.google.android.gms.permission.AD_ID"));
        QVERIFY(!granted.contains("android.permission.RECORD_AUDIO"));

        QCOMPARE(fieldSet(card.value("trackers").toArray(), "name"),
                 (QSet<QString>{"Facebook Audience Network", "Firebase Analytics"}));

        const QJsonObject transmitted = card.value("transmitted").toObject();
        QCOMPARE(transmitted.value("rx_bytes").toInteger(), 81536);
        QCOMPARE(transmitted.value("tx_bytes").toInteger(), 12512);
        QVERIFY(transmitted.value("network_permission").toBool());
        QCOMPARE(fieldSet(transmitted.value("connections").toArray(), "remote"),
                 (QSet<QString>{"192.168.8.8:443", "8.8.8.8:53"}));
        QHash<QString, QJsonValue> dns;
        for (const QJsonValue &row : transmitted.value("dns").toArray())
            dns.insert(row.toObject().value("host").toString(), row.toObject().value("tracker"));
        QCOMPARE(dns.value("graph.facebook.com").toString(), QStringLiteral("Facebook"));
        QVERIFY(dns.contains("example.com") && dns.value("example.com").isNull());
    }

    // ReportTests.test_quiet_app_stays_in_the_declared_section
    void quietAppStaysInTheDeclaredSection() {
        const QJsonObject report = buildReport(load("demo"));
        const QString text = renderText(report);
        QVERIFY(!text.contains("### uid 10299"));
        QVERIFY(text.contains("com.quiet.notes (uid 10299): Contacts"));
        QVERIFY(text.contains("HTTPS message bodies stay encrypted."));
        QVERIFY(text.contains("Sent 12.2 KB, received 79.6 KB."));
        QVERIFY(text.contains("TCP ESTABLISHED 127.0.0.1:54321 -> 192.168.8.8:443"));
        QVERIFY(text.contains(QStringLiteral("DNS graph.facebook.com — tracker Facebook")));
        const QJsonObject first = report.value("unattributed_dns").toArray()[0].toObject();
        QCOMPARE(first.value("host").toString(), QStringLiteral("app-measurement.com"));
        QCOMPARE(first.value("tracker").toString(), QStringLiteral("Firebase Analytics"));
        QVERIFY(text.contains("Pixel 8"));
    }

    // ReportTests.test_package_filter_and_capture_gaps
    void packageFilterAndCaptureGaps() {
        Capture capture = load("demo");
        const QSet<QString> filter = {"com.quiet.notes"};
        const QJsonObject filtered = buildReport(capture, false, &filter);
        QCOMPARE(filtered.value("live").toArray(), QJsonArray());
        QCOMPARE(filtered.value("declared_only").toArray()[0].toObject().value("package").toString(),
                 QStringLiteral("com.quiet.notes"));
        capture.meta.insert("errors", QJsonObject{{"location", "dumpsys failed"}});
        QVERIFY(renderText(buildReport(capture)).contains("location: dumpsys failed"));
    }

    // ReportTests.test_watch_diff_emits_new_socket_focus_and_access
    void watchDiffEmitsNewSocketFocusAndAccess() {
        const QJsonObject first = watchSnapshot(load("demo"));
        QJsonObject second = first;
        second.insert("focus", QJsonArray());
        second.insert("connections", QJsonArray());
        second.insert("ops", QJsonArray());
        const QStringList events = diffWatch(second, first);
        QVERIFY(events.contains("On screen: com.tracker.demo/.Main"));
        QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const QString &e) { return e.contains("192.168.8.8:443"); }));
        QVERIFY(std::any_of(events.cbegin(), events.cend(),
                            [](const QString &e) { return e.startsWith("com.tracker.demo FINE_LOCATION"); }));
        QCOMPARE(diffWatch(first, first), QStringList());
    }

    // ---------------------------------------------------- test_serve.py

    // FlowTests.test_fixture_splits_collection_from_transmission
    void fixtureSplitsCollectionFromTransmission() {
        const QJsonObject result = projectReport(buildReport(load("demo")));
        const QJsonArray flows = result.value("flows").toArray();
        const QSet<Triple> collections = triples(flows, "collection", "source", "destination", "via");
        QVERIFY(collections.contains(triple("Precise location", "com.tracker.demo", "read")));
        QVERIFY(collections.contains(triple("Camera", "com.tracker.demo", "read")));
        QVERIFY(collections.contains(triple("Microphone", "com.tracker.demo", "denied")));
        QVERIFY(collections.contains(triple("android.sensor.accelerometer", "com.tracker.demo", "read")));
        const QSet<Triple> transmissions = triples(flows, "transmission", "source", "destination", "via");
        QVERIFY(transmissions.contains(triple("com.tracker.demo", "graph.facebook.com", "Facebook")));
        QVERIFY(transmissions.contains(triple("com.tracker.demo", "192.168.8.8:443", "127.0.0.1:54321")));
        QVERIFY(transmissions.contains(triple("Unknown app", "app-measurement.com", "Firebase Analytics")));
        QVERIFY(triples(flows, "declared", "source", "destination", nullptr).contains(triple("Contacts", "com.quiet.notes", "")));
        QSet<QString> trackers;
        for (const QJsonValue &flow : flows)
            if (flow.toObject().value("kind").toString() == QLatin1String("tracker"))
                trackers.insert(flow.toObject().value("destination").toString());
        QVERIFY(trackers.contains("Firebase Analytics"));
        QVERIFY(trackers.contains("Facebook Audience Network"));
        QCOMPARE(result.value("apps").toArray()[0].toObject().value("tx_bytes").toInteger(), 12512);
    }

    // RelayTests.test_post_relay_shows_on_the_dashboard_state, the graph
    // part only: the HTTP relay and the icon route are not ported here.
    void dashboardGraphForTheDemo() {
        const QJsonArray graph = graphOf(buildReport(load("demo")));
        QJsonObject demo;
        for (const QJsonValue &card : graph)
            if (card.toObject().value("package").toString() == QLatin1String("com.tracker.demo"))
                demo = card.toObject();
        QVERIFY(!demo.isEmpty());
        QHash<QString, QJsonObject> reads;
        for (const QJsonValue &item : demo.value("reads").toArray())
            reads.insert(item.toObject().value("label").toString(), item.toObject());
        QVERIFY(reads.contains("Location"));
        QVERIFY(reads.contains("Camera"));
        QVERIFY(reads.value("Location").value("detail").toString().contains("Precise location"));
        QVERIFY(reads.value("Location").value("detail").toString().contains("5 minutes ago"));
        QHash<QString, QJsonObject> destinations;
        for (const QJsonValue &item : demo.value("destinations").toArray())
            destinations.insert(item.toObject().value("label").toString(), item.toObject());
        QVERIFY(destinations.value("192.168.8.8").value("detail").toString().contains("Encrypted connection"));
        QVERIFY(destinations.value("graph.facebook.com").value("detail").toString().contains("DNS lookup"));
        QCOMPARE(destinations.value("graph.facebook.com").value("relation").toString(), QStringLiteral("lookup"));
        QCOMPARE(QSet<QString>(destinations.keyBegin(), destinations.keyEnd()),
                 (QSet<QString>{"192.168.8.8", "8.8.8.8", "graph.facebook.com", "example.com"}));
        QCOMPARE(destinations.value("graph.facebook.com").value("kind").toString(), QStringLiteral("Facebook"));
        QCOMPARE(destinations.value("192.168.8.8").value("kind").toString(), QStringLiteral("Encrypted"));
    }

    // SummaryTests.test_old_reads_are_left_off_the_graph
    void oldReadsAreLeftOffTheGraph() {
        const QJsonArray flows = {
            QJsonObject{{"kind", "transmission"}, {"source", "com.tracker.demo"}, {"destination", "192.0.2.1:443"},
                        {"information", "TCP ESTABLISHED"}},
            QJsonObject{{"kind", "collection"}, {"source", "Precise location"}, {"destination", "com.tracker.demo"},
                        {"via", "read"}, {"detail", "last access 2020-01-01 (-400d ago)"}},
            QJsonObject{{"kind", "collection"}, {"source", "Camera"}, {"destination", "com.tracker.demo"},
                        {"via", "read"}, {"detail", "last access 2026-11-14 (-5m ago)"}},
        };
        const QJsonArray apps = {QJsonObject{
            {"name", "com.tracker.demo"}, {"packages", QJsonArray{"com.tracker.demo"}}, {"tx_bytes", 10}, {"rx_bytes", 0}}};
        const QJsonObject card = graphFrom(flows, apps)[0].toObject();
        QCOMPARE(card.value("name").toString(), QStringLiteral("Demo"));
        const QJsonArray reads = card.value("reads").toArray();
        QCOMPARE(reads.size(), 1);
        QCOMPARE(reads[0].toObject().value("label").toString(), QStringLiteral("Camera"));
        QVERIFY(reads[0].toObject().value("detail").toString().contains("5 minutes ago"));
        const QJsonObject destination = card.value("destinations").toArray()[0].toObject();
        QCOMPARE(destination.value("kind").toString(), QStringLiteral("Encrypted"));
        QVERIFY(destination.value("detail").toString().contains("192.0.2.1:443"));
    }

    // SummaryTests.test_a_resolved_address_is_shown_by_name, with nameFor in
    // place of names.remember.
    void aResolvedAddressIsShownByName() {
        const QJsonArray flows = {QJsonObject{{"kind", "transmission"}, {"source", "com.whatsapp"},
                                              {"destination", "192.0.2.9:443"}, {"information", "TCP ESTABLISHED"}}};
        const QJsonArray apps = {QJsonObject{
            {"name", "com.whatsapp"}, {"packages", QJsonArray{"com.whatsapp"}}, {"tx_bytes", 1}, {"rx_bytes", 1}}};
        QStringList asked;
        const auto names = [&](const QString &host) {
            asked.append(host);
            return host == QLatin1String("192.0.2.9") ? QStringLiteral("mail.example") : QString();
        };
        const QJsonObject destination = graphFrom(flows, apps, names)[0].toObject().value("destinations").toArray()[0].toObject();
        QCOMPARE(destination.value("name").toString(), QStringLiteral("mail.example"));
        QVERIFY(destination.value("detail").toString().contains("mail.example"));
        QCOMPARE(asked, QStringList{"192.0.2.9"});
    }

    // SummaryTests.test_a_recent_read_keeps_an_app_with_no_socket
    void aRecentReadKeepsAnAppWithNoSocket() {
        const QJsonArray flows = {
            QJsonObject{{"kind", "collection"}, {"source", "Camera"}, {"destination", "com.whatsapp"}, {"via", "read"},
                        {"detail", "last access 2026-11-14 (-20m ago)"}},
            QJsonObject{{"kind", "collection"}, {"source", "Camera"}, {"destination", "com.android.systemui"},
                        {"via", "read"}, {"detail", "last access 2026-11-14 (-2m ago)"}},
        };
        const QJsonArray apps = {
            QJsonObject{{"name", "com.whatsapp"}, {"packages", QJsonArray{"com.whatsapp"}}, {"tx_bytes", 100}, {"rx_bytes", 100}},
            QJsonObject{{"name", "com.android.systemui"}, {"packages", QJsonArray{"com.android.systemui"}},
                        {"tx_bytes", 100}, {"rx_bytes", 0}},
        };
        const QJsonArray cards = graphFrom(flows, apps);
        QCOMPARE(cards.size(), 1);
        QCOMPARE(cards[0].toObject().value("name").toString(), QStringLiteral("WhatsApp"));
    }

    // ------------------------------------------------ port-specific checks

    // set(a) | set(b) iteration order for two defaultdicts, as CPython
    // 3.14.8 printed list(set(a) | set(b)) for these keys.
    void setUnionOrderMatchesCPython() {
        QCOMPARE(py::setUnionOrder({10304, 10301, 10306, 0, 1073, 1000, 10120, 1035, 1043}, {10303, 10306, 1051, 10011, 10019, 10027}),
                 (QList<qint64>{10304, 0, 10306, 10019, 10011, 1000, 10120, 1035, 10027, 1073, 1043, 1051, 10301, 10303}));
        QCOMPARE(py::setUnionOrder({}, {8, 0, 16}), (QList<qint64>{8, 0, 16}));
        QCOMPARE(py::setUnionOrder({5, 13, 21, 29}, {}), (QList<qint64>{29, 13, 21, 5}));
        QList<qint64> a;
        for (qint64 uid = 10000; uid < 10040; uid += 3)
            a.append(uid);
        QCOMPARE(py::setUnionOrder(a, {1000, 0, 1073, 10003}),
                 (QList<qint64>{0, 10018, 10021, 10024, 1000, 10027, 10030, 10000, 10033, 1073, 10003, 10036, 10006, 10039, 10009, 10012, 10015}));
    }

    // Each expected value is what parse_ip returned under Python 3.14.8.
    void ipv6FormatsLikeInetNtop() {
        QCOMPARE(parseIp("0000000000000000FFFF00000100007F"), std::optional<QString>("::ffff:127.0.0.1"));
        QCOMPARE(parseIp("00000000000000000000000004030201"), std::optional<QString>("::1.2.3.4"));
        QCOMPARE(parseIp("00000000000000000000000000000000"), std::optional<QString>("::"));
        QCOMPARE(parseIp("B80D0120000000000000000001000000"), std::optional<QString>("2001:db8::1"));
        QCOMPARE(parseIp("B80D0120000000000000010001000000"), std::optional<QString>("2001:db8::1:0:0:1"));
        QCOMPARE(parseIp("0100007"), std::nullopt);
        QCOMPARE(parseIp("0100007F00"), std::nullopt);
    }

    void friendlyNames() {
        QCOMPARE(friendlyName("com.whatsapp"), QStringLiteral("WhatsApp"));
        QCOMPARE(friendlyName("com.life360.android.safetymapd"), QStringLiteral("Life360"));
        QCOMPARE(friendlyName("com.example.myCoolApp_beta"), QStringLiteral("My Cool App beta"));
        QCOMPARE(friendlyName(""), QStringLiteral("App"));
        QCOMPARE(friendlyName("uid 10245"), QStringLiteral("uid 10245"));
        QCOMPARE(friendlyName("com.example."), QStringLiteral("com.example."));
    }

    // dumpsys package runs to several MB on a real phone. A synthetic dump of
    // that size must stay well clear of quadratic time.
    void largePackageDumpStaysLinear() {
        Capture capture = load("android14");
        const QString base = capture.text("package_dump");
        QString big;
        int copies = 0;
        while (big.size() < 6 * 1024 * 1024) {
            QString copy = base;
            copy.replace(QStringLiteral("Package [com."), QStringLiteral("Package [com.copy%1.").arg(copies++));
            big += copy;
        }
        capture.texts.insert("package_dump", big);
        QElapsedTimer timer;
        timer.start();
        const QJsonObject report = buildReport(capture);
        const qint64 elapsed = timer.elapsed();
        qInfo("buildReport on a %lld-character package dump (%d copies of android14): %lld ms", qint64(big.size()), copies,
              elapsed);
        QVERIFY(!report.value("live").toArray().isEmpty());
        QVERIFY2(elapsed < 20000, qPrintable(QString::number(elapsed)));
    }
};

int runReportTests(int argc, char **argv) {
    ReportTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "report_tests.moc"
