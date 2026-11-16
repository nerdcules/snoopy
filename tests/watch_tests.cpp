#include <QtTest>

#include "capture.h"
#include "tests.h"
#include "watch.h"

namespace {

// The shapes of the dumps on Android 17 (Pixel 8 Pro, 2026-11-16), with
// made-up packages.
const char *kAppops =
    "Current AppOps Service state:\n"
    "  Uid 1000:\n"
    "    state=pers\n"
    "    Package android:\n"
    "      READ_CONTACTS (allow): \n"
    "        null=[\n"
    "          Access: [pers-s] 2026-11-16 07:00:00.000 (-1h32m12s593ms)\n"
    "        ]\n"
    "  Uid u0a305:\n"
    "    state=cch\n"
    "    Package com.example.spy:\n"
    "      RECORD_AUDIO (allow): \n"
    "        recorder=[\n"
    "          Access: [cch-s] 2026-11-16 08:00:00.000 (-12m3s4ms) duration=+41s200ms\n"
    "          Access: [bg-s] 2026-11-16 07:00:00.000 (-1h2m3s4ms) duration=+2s\n"
    "          Access: [top-s] 2026-11-16 07:30:00.000 (-30m0s0ms) duration=+5s\n"
    "        ]\n"
    "      CAMERA (allow): \n"
    "        null=[\n"
    "          Access: [bg-s] 2026-11-14 08:00:00.000 (-2d0h0m0s0ms)\n"
    "        ]\n"
    "      READ_CONTACTS (allow): \n"
    "        null=[\n"
    "          Reject: [bg-s] 2026-11-16 08:00:00.000 (-5m0s0ms)\n"
    "        ]\n"
    "      GET_USAGE_STATS (allow): \n"
    "      SYSTEM_ALERT_WINDOW (default): \n"
    "  Uid u0a410:\n"
    "    Package com.example.maps:\n"
    "      FINE_LOCATION (allow): \n"
    "        null=[\n"
    "          Access: [fgsvc-s] 2026-11-16 08:10:00.000 (-2m0s0ms) duration=+1m0s0ms\n"
    "          Access: [bg-tp] 2026-11-16 08:00:00.000 (-9m0s0ms)\n"
    "        ]\n"
    "  Uid 10088:\n"
    "    Package com.android.contacts:\n"
    "      READ_CONTACTS (allow): \n"
    "        sync=[\n"
    "          Access: [bg-s] 2026-11-16 08:00:00.000 (-10m0s0ms)\n"
    "        ]\n";

const char *kSecurity =
    "accessibility=com.example.spy/.Watcher:com.android.talkback/.TalkBack\n"
    "listeners=com.example.spy/.Listener\n"
    "proxy=null\n"
    "private_dns=hostname\n"
    "private_dns_host=dns.example.net\n"
    "adb_wifi=1\n"
    "patch=2026-05-01\n"
    "verified_boot=green\n"
    "flash_locked=1\n"
    "hardware_serial=PHONE1\n";

const char *kInstallers =
    "package:com.example.spy  installer=com.google.android.packageinstaller uid:10305\n"
    "package:com.example.maps  installer=com.android.vending uid:10410\n"
    "package:com.example.free  installer=org.fdroid.fdroid uid:10500\n";

const char *kLaunchers =
    "2 activities found:\n"
    "  Activity #0:\n"
    "    priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\n"
    "    com.example.maps/.Main\n"
    "  Activity #1:\n"
    "    priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\n"
    "    com.example.free/.Main\n";

const char *kPolicy =
    "Current Device Policy Manager state:\n"
    "  Immutable state:\n"
    "    mHasFeature=true\n"
    "  Enabled Device Admins (User 0, provisioningState: 0):\n"
    "    com.example.spy/.receiver.Admin:\n"
    "      uid=10305\n"
    "      testOnlyAdmin=false\n"
    "  Other:\n";

const char *kVpn =
    "VPNs:\n"
    "  0: [Legacy VPN]\n"
    "    Active package name: com.example.vpn\n"
    "    mEventChanges (most recent first):\n"
    "      2026-11-16T06:56:41.815312 - [LockdownAlwaysOn] Mode changed: lockdown=false alwaysOn=true calling from x\n";

Capture capture() {
    Capture c;
    c.texts.insert("appops", kAppops);
    c.texts.insert("security", kSecurity);
    c.texts.insert("installers", kInstallers);
    c.texts.insert("launchers", kLaunchers);
    c.texts.insert("device_policy", kPolicy);
    c.texts.insert("vpn", kVpn);
    c.texts.insert("roles", "    roles=[\n      {\n        name=android.app.role.SMS\n        holders=com.example.sms\n      }\n");
    return c;
}

QJsonObject byKey(const QJsonArray &findings, const QString &key) {
    for (const QJsonValue &item : findings)
        if (item.toObject().value("key").toString() == key)
            return item.toObject();
    return {};
}

class WatchTests : public QObject {
    Q_OBJECT

private slots:
    void readsEveryAccessLine() {
        const QList<OpAccess> all = parseOpAccesses(kAppops);
        QCOMPARE(all.size(), 9);
        const OpAccess first = all.at(1);
        QCOMPARE(first.package, QString("com.example.spy"));
        QCOMPARE(first.uid, qint64(10305));
        QCOMPARE(first.op, QString("RECORD_AUDIO"));
        QCOMPARE(first.attribution, QString("recorder"));
        QCOMPARE(first.state, QString("cch"));
        QCOMPARE(first.flags, QString("s"));
        QCOMPARE(first.ago, qint64(12 * 60 + 3));
        QCOMPARE(first.durationMs, qint64(41200));
        QVERIFY(all.at(5).rejected);
        QCOMPARE(agoSeconds("-1d2h3m4s5ms"), qint64(86400 + 7200 + 180 + 4));
        QCOMPARE(agoSeconds("1d"), qint64(-1));
    }

    void backgroundUseIsRankedByWhatAndWho() {
        const QJsonArray findings = watchFindings(capture(), QDate(2026, 11, 16));
        // Microphone out of sight: an alert, two lines merged, the top one left out.
        const QJsonObject mic = byKey(findings, "bg|com.example.spy|RECORD_AUDIO");
        QCOMPARE(mic.value("level").toString(), QString("alert"));
        QCOMPARE(mic.value("ago").toInt(), 12 * 60 + 3);
        QVERIFY2(mic.value("detail").toString().contains("2 times"), qPrintable(mic.value("detail").toString()));
        QVERIFY(mic.value("detail").toString().contains("43 s in all"));
        // Older than a day, or rejected: nothing.
        QVERIFY(byKey(findings, "bg|com.example.spy|CAMERA").isEmpty());
        QVERIFY(byKey(findings, "bg|com.example.spy|READ_CONTACTS").isEmpty());
        // Location in the background warns, through a proxy says so; in a
        // foreground service it is only listed.
        const QJsonObject location = byKey(findings, "bg|com.example.maps|FINE_LOCATION");
        QCOMPARE(location.value("level").toString(), QString("warn"));
        QVERIFY(location.value("detail").toString().contains("another app on its behalf"));
        QCOMPARE(byKey(findings, "fgsvc|com.example.maps|FINE_LOCATION").value("level").toString(), QString("info"));
        // The system's own contacts sync is listed, not flagged; a
        // persistent system process isn't listed at all.
        QCOMPARE(byKey(findings, "bg|com.android.contacts|READ_CONTACTS").value("level").toString(), QString("info"));
        QVERIFY(byKey(findings, "bg|android|READ_CONTACTS").isEmpty());
        // Alerts first.
        QCOMPARE(findings.first().toObject().value("level").toString(), QString("alert"));
    }

    void appsThatCanWatchOrControl() {
        const QJsonArray findings = watchFindings(capture(), QDate(2026, 11, 16));
        QCOMPARE(byKey(findings, "a11y|com.example.spy/.Watcher").value("level").toString(), QString("warn"));
        QCOMPARE(byKey(findings, "a11y|com.android.talkback/.TalkBack").value("level").toString(), QString("info"));
        QCOMPARE(byKey(findings, "listener|com.example.spy/.Listener").value("level").toString(), QString("warn"));
        QCOMPARE(byKey(findings, "admin|com.example.spy/.receiver.Admin").value("level").toString(), QString("warn"));
        QCOMPARE(byKey(findings, "grant|com.example.spy|GET_USAGE_STATS").value("level").toString(), QString("warn"));
        // "default" is not "allow".
        QVERIFY(byKey(findings, "grant|com.example.spy|SYSTEM_ALERT_WINDOW").isEmpty());
        QCOMPARE(byKey(findings, "role|android.app.role.SMS|com.example.sms").value("level").toString(), QString("info"));
    }

    void networkOutsideAndDevice() {
        const QJsonArray findings = watchFindings(capture(), QDate(2026, 11, 16));
        QVERIFY(byKey(findings, "proxy|null").isEmpty());
        QCOMPARE(byKey(findings, "vpn|com.example.vpn").value("level").toString(), QString("warn"));
        QVERIFY(!byKey(findings, "vpn-always-on").isEmpty());
        QVERIFY(byKey(findings, "dns|dns.example.net").value("title").toString().contains("dns.example.net"));
        QCOMPARE(byKey(findings, "installed|com.example.spy|com.google.android.packageinstaller").value("level").toString(),
                 QString("warn"));
        QCOMPARE(byKey(findings, "store|com.example.free|org.fdroid.fdroid").value("level").toString(), QString("info"));
        QVERIFY(byKey(findings, "installed|com.example.maps|com.android.vending").isEmpty());
        QVERIFY(!byKey(findings, "noicon|com.example.spy").isEmpty());
        QVERIFY(byKey(findings, "noicon|com.example.maps").isEmpty());
        // 199 days since the patch.
        QCOMPARE(byKey(findings, "patch").value("level").toString(), QString("warn"));
        QVERIFY(byKey(findings, "patch").value("title").toString().contains("199 days"));
        QVERIFY(byKey(findings, "unlocked").isEmpty());

        Capture worse = capture();
        worse.texts["security"] = QString(kSecurity).replace("proxy=null", "proxy=10.0.0.9:8080")
                                      .replace("verified_boot=green", "verified_boot=orange")
                                      .replace("flash_locked=1", "flash_locked=0");
        const QJsonArray bad = watchFindings(worse, QDate(2026, 11, 16));
        QCOMPARE(byKey(bad, "proxy|10.0.0.9:8080").value("level").toString(), QString("alert"));
        QCOMPARE(byKey(bad, "boot|orange").value("level").toString(), QString("alert"));
        QCOMPARE(byKey(bad, "unlocked").value("level").toString(), QString("alert"));
    }

    void baselineMarksWhatIsNew() {
        QTemporaryDir directory;
        WatchBaseline baseline(directory.path());
        const QDateTime now(QDate(2026, 11, 16), QTime(9, 0), QTimeZone::UTC);
        const QJsonArray first = watchFindings(capture(), QDate(2026, 11, 16));
        // Looking only, before any baseline: nothing new, nothing stored.
        QVERIFY(!baseline.apply("PHONE1", first, now, false).first().toObject().contains("new"));
        QVERIFY(QDir(directory.path()).isEmpty());
        // The first look is the baseline.
        for (const QJsonValue &item : baseline.apply("PHONE1", first, now))
            QVERIFY(!item.toObject().value("new").toBool());
        // A new accessibility service later is new, and stays new.
        Capture later = capture();
        later.texts["security"] = QString(kSecurity).replace("accessibility=", "accessibility=com.example.new/.Svc:");
        const QJsonArray second = baseline.apply("PHONE1", watchFindings(later, QDate(2026, 11, 16)), now.addSecs(600));
        const QJsonObject added = byKey(second, "a11y|com.example.new/.Svc");
        QVERIFY(added.value("new").toBool());
        QCOMPARE(added.value("since").toString(), QString("2026-11-16T09:10:00Z"));
        const QJsonArray third = baseline.apply("PHONE1", watchFindings(later, QDate(2026, 11, 16)), now.addSecs(1200));
        QCOMPARE(byKey(third, "a11y|com.example.new/.Svc").value("since").toString(), QString("2026-11-16T09:10:00Z"));
        QCOMPARE(byKey(third, "a11y|com.example.spy/.Watcher").value("new").toBool(), false);
        // Once seen, it isn't new.
        baseline.acknowledge("PHONE1");
        const QJsonArray fourth = baseline.apply("PHONE1", watchFindings(later, QDate(2026, 11, 16)), now.addSecs(1800));
        QVERIFY(!byKey(fourth, "a11y|com.example.new/.Svc").value("new").toBool());
        // Another phone has its own baseline.
        for (const QJsonValue &item : baseline.apply("PHONE2", second, now))
            QVERIFY(!item.toObject().value("new").toBool());
    }

    void emptySampleFindsNothing() {
        QCOMPARE(watchFindings(Capture(), QDate(2026, 11, 16)).size(), 0);
    }
};

} // namespace

int runWatchTests(int argc, char **argv) {
    WatchTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "watch_tests.moc"
