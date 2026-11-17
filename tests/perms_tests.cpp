#include <QtTest>

#include "adb.h"
#include "perms.h"
#include "tests.h"

namespace {

// A phone with state, answering in the shapes Android 17 prints (Pixel 8
// Pro, 2026-11-16), that really applies the commands Snoopy sends: pm
// grant/revoke/suspend/disable-user/enable, cmd appops set, cmd
// notification allow/disallow_listener, settings get/put/delete.
struct StatefulPhone {
    struct App {
        QMap<QString, bool> runtime; // permission -> granted
        QSet<QString> fixedPermissions;
        QMap<QString, QString> ops; // package modes
        bool system = false;
        bool suspended = false;
        int enabled = 0;
        bool installed = true;
    };
    QMap<QString, App> apps;
    QString listeners = "null";
    QString accessibility = "null";
    QString admins;
    QStringList calls;
    bool failNext = false;

    QString packageDump(const QString &package) const {
        if (!apps.contains(package) || !apps[package].installed)
            return "Unable to find package: " + package + "\n";
        const App &app = apps[package];
        QString out = "Packages:\n  Package [" + package + "] (1a2b3c):\n    appId=10305\n";
        out += QString("    pkgFlags=[ %1HAS_CODE ALLOW_BACKUP ]\n").arg(app.system ? "SYSTEM " : "");
        out += "    requested permissions:\n";
        for (auto it = app.runtime.begin(); it != app.runtime.end(); ++it)
            out += "      " + it.key() + "\n";
        for (auto it = app.ops.begin(); it != app.ops.end(); ++it)
            if (it.key() == "REQUEST_INSTALL_PACKAGES")
                out += "      android.permission.REQUEST_INSTALL_PACKAGES\n";
        out += "    install permissions:\n      android.permission.INTERNET: granted=true\n";
        out += QString("    User 0: ceDataInode=1 installed=true hidden=false suspended=%1 stopped=false enabled=%2\n")
                   .arg(app.suspended ? "true" : "false")
                   .arg(app.enabled);
        out += "      runtime permissions:\n";
        for (auto it = app.runtime.begin(); it != app.runtime.end(); ++it)
            out += "        " + it.key() + ": granted=" + (it.value() ? "true" : "false") + ", flags=[ USER_SENSITIVE_WHEN_GRANTED" +
                   (app.fixedPermissions.contains(it.key()) ? "|SYSTEM_FIXED" : "") + "]\n";
        return out;
    }

    QString appops(const QString &package) const {
        QString out = "Uid mode: CAMERA: foreground\n";
        // A const map's [] returns a copy; keep it alive for the loop.
        const App app = apps.value(package);
        for (auto it = app.ops.begin(); it != app.ops.end(); ++it)
            out += it.key() + ": " + it.value() + "; time=+3d0h0m0s0ms ago\n";
        return out;
    }

    Runner runner() {
        return [this](const QString &, const QStringList &arguments, int) {
            ProcessResult result;
            result.started = true;
            result.code = 0;
            const QString command = arguments.value(arguments.indexOf("shell") + 1);
            calls.append(command);
            auto answer = [&](const QString &text) { result.out = text.toUtf8(); };
            const QStringList w = command.split(' ', Qt::SkipEmptyParts);
            auto unquote = [](QString s) {
                if (s.startsWith('\'') && s.endsWith('\''))
                    s = s.mid(1, s.size() - 2);
                return s;
            };
            if (failNext && (w.value(0) == "pm" || w.value(0) == "cmd")) {
                failNext = false;
                answer("Exception occurred while executing 'grant':\njava.lang.SecurityException: not allowed\n");
                return result;
            }
            if (w.value(0) == "dumpsys" && w.value(1) == "package")
                answer(packageDump(unquote(w.value(2))));
            else if (w.value(0) == "dumpsys" && w.value(1) == "device_policy")
                answer("  Enabled Device Admins (User 0, provisioningState: 0):\n" + admins + "  Other:\n");
            else if (w.value(0) == "cmd" && w.value(1) == "appops" && w.value(2) == "get")
                answer(appops(unquote(w.value(3))));
            else if (w.value(0) == "cmd" && w.value(1) == "appops" && w.value(2) == "set")
                apps[w.value(3)].ops[w.value(4)] = w.value(5);
            else if (w.value(0) == "pm" && (w.value(1) == "grant" || w.value(1) == "revoke"))
                apps[w.value(4)].runtime[w.value(5)] = w.value(1) == "grant";
            else if (w.value(0) == "pm" && (w.value(1) == "suspend" || w.value(1) == "unsuspend"))
                apps[w.value(4)].suspended = w.value(1) == "suspend";
            else if (w.value(0) == "pm" && w.value(1) == "disable-user")
                apps[w.value(4)].enabled = 3;
            else if (w.value(0) == "pm" && w.value(1) == "enable")
                apps[w.value(4)].enabled = 1;
            else if (w.value(0) == "cmd" && w.value(1) == "notification") {
                const QString component = w.value(3);
                QStringList list = listeners == "null" ? QStringList() : listeners.split(':');
                list.removeAll(component);
                if (w.value(2) == "allow_listener")
                    list.append(component);
                listeners = list.isEmpty() ? "null" : list.join(':');
            } else if (command.startsWith("echo listeners="))
                answer("listeners=" + listeners + "\naccessibility=" + accessibility + "\n");
            else if (command == "settings get secure enabled_accessibility_services")
                answer(accessibility + "\n");
            else if (w.value(0) == "settings" && w.value(1) == "put")
                accessibility = unquote(w.value(4));
            else if (w.value(0) == "settings" && w.value(1) == "delete")
                accessibility = "null";
            return result;
        };
    }
};

const QString P = "android.permission.";

class PermsTests : public QObject {
    Q_OBJECT

    QTemporaryDir *m_journal = nullptr;

    StatefulPhone phone() {
        StatefulPhone p;
        StatefulPhone::App chat;
        chat.runtime = {{P + "ACCESS_FINE_LOCATION", true}, {P + "ACCESS_COARSE_LOCATION", true},
                        {P + "ACCESS_BACKGROUND_LOCATION", true}, {P + "RECORD_AUDIO", true},
                        {P + "CAMERA", false}, {P + "READ_CONTACTS", true}};
        chat.ops = {{"READ_CLIPBOARD", "allow"}, {"RUN_ANY_IN_BACKGROUND", "allow"}};
        p.apps["com.example.chat"] = chat;
        StatefulPhone::App dialer;
        dialer.system = true;
        dialer.runtime = {{P + "READ_CALL_LOG", true}};
        dialer.fixedPermissions = {P + "READ_CALL_LOG"};
        p.apps["com.android.dialer"] = dialer;
        p.listeners = "com.example.chat/.Listener:com.example.watch/.Bridge";
        p.accessibility = "com.example.chat/com.example.chat.Svc$Inner:com.android.talkback/.TalkBack";
        p.admins = "    com.example.chat/.Admin:\n      uid=10305\n";
        return p;
    }

    QString control(const perms::AppState &app, const QString &id) {
        for (const perms::Control &c : perms::controls(app))
            if (c.id == id)
                return c.state;
        return {};
    }

private slots:
    void init() { m_journal = new QTemporaryDir; }
    void cleanup() { delete m_journal; }

    void readsAnAppsControls() {
        StatefulPhone fake = phone();
        Adb adb("SERIAL", fake.runner(), "/usr/bin/adb");
        perms::Changer changer(adb, "PHONE1", m_journal->path());
        const perms::AppState app = changer.read("com.example.chat");
        QVERIFY(app.found);
        QVERIFY(!app.system);
        QCOMPARE(control(app, "perm:location"), QString("always"));
        QCOMPARE(control(app, "perm:microphone"), QString("allow"));
        QCOMPARE(control(app, "perm:camera"), QString("deny"));
        QCOMPARE(control(app, "op:clipboard"), QString("allow"));
        QCOMPARE(control(app, "listener:com.example.chat/.Listener"), QString("on"));
        QVERIFY(control(app, "listener:com.example.watch/.Bridge").isEmpty()); // another app's
        QCOMPARE(control(app, "a11y:com.example.chat/com.example.chat.Svc$Inner"), QString("on"));
        QCOMPARE(control(app, "admin:com.example.chat/.Admin"), QString("on"));
        // Location offers all three when the app asks for the background.
        for (const perms::Control &c : perms::controls(app))
            if (c.id == "perm:location")
                QCOMPARE(c.options, (QStringList{"always", "while_using", "deny"}));
    }

    void plansExactCommandsAndTheirUndo() {
        StatefulPhone fake = phone();
        Adb adb("SERIAL", fake.runner(), "/usr/bin/adb");
        perms::Changer changer(adb, "PHONE1", m_journal->path());
        const perms::AppState app = changer.read("com.example.chat");
        const perms::Plan whileUsing = perms::plan(app, "perm:location", "while_using");
        QCOMPARE(whileUsing.commands.size(), 1);
        QCOMPARE(whileUsing.commands.first(), (QStringList{"pm", "revoke", "--user", "0", "com.example.chat", P + "ACCESS_BACKGROUND_LOCATION"}));
        QCOMPARE(whileUsing.undo.first(), (QStringList{"pm", "grant", "--user", "0", "com.example.chat", P + "ACCESS_BACKGROUND_LOCATION"}));
        QCOMPARE(perms::plan(app, "perm:location", "deny").commands.size(), 3);
        QCOMPARE(perms::plan(app, "op:clipboard", "deny").commands.first(),
                 (QStringList{"cmd", "appops", "set", "com.example.chat", "READ_CLIPBOARD", "ignore"}));
        // Already so: nothing to run.
        const perms::Plan same = perms::plan(app, "perm:camera", "deny");
        QVERIFY(same.commands.isEmpty());
        QVERIFY(!same.blocked);
        // Not a state it has, or not something Android allows here.
        QVERIFY(perms::plan(app, "perm:camera", "always").blocked);
        QVERIFY(perms::plan(app, "admin:com.example.chat/.Admin", "off").blocked);
        const perms::AppState dialer = changer.read("com.android.dialer");
        const perms::Plan fixedPlan = perms::plan(dialer, "perm:calllog", "deny");
        QVERIFY(fixedPlan.blocked);
        QVERIFY(fixedPlan.reason.contains("system"));
    }

    void essentialAppsAreNeverStopped() {
        QVERIFY(perms::essential("com.android.systemui"));
        QVERIFY(perms::essential("com.android.providers.calendar"));
        QVERIFY(!perms::essential("com.example.chat"));
        perms::AppState settings;
        settings.package = "com.android.settings";
        settings.found = true;
        QVERIFY(perms::plan(settings, "app:disable", "disabled").blocked);
        QVERIFY(perms::plan(settings, "app:remove", "removed").blocked);
    }

    void appliesChecksRecordsAndUndoes() {
        StatefulPhone fake = phone();
        Adb adb("SERIAL", fake.runner(), "/usr/bin/adb");
        perms::Changer changer(adb, "PHONE1", m_journal->path());
        const QJsonObject entry = changer.apply(perms::plan(changer.read("com.example.chat"), "perm:location", "while_using"));
        QVERIFY(entry.value("ok").toBool());
        QVERIFY(entry.value("verified").toBool());
        QCOMPARE(entry.value("now").toString(), QString("while_using"));
        QVERIFY(!fake.apps["com.example.chat"].runtime[P + "ACCESS_BACKGROUND_LOCATION"]);
        QCOMPARE(changer.journal().size(), 1);

        const QJsonObject undone = changer.undo(entry.value("id").toInt());
        QVERIFY(undone.value("undone").toBool());
        QCOMPARE(undone.value("undoNow").toString(), QString("always"));
        QVERIFY(fake.apps["com.example.chat"].runtime[P + "ACCESS_BACKGROUND_LOCATION"]);
        QVERIFY(changer.journal().first().toObject().value("undone").toBool());
    }

    void specialAccessAndAppStates() {
        StatefulPhone fake = phone();
        Adb adb("SERIAL", fake.runner(), "/usr/bin/adb");
        perms::Changer changer(adb, "PHONE1", m_journal->path());
        // Notification access off, then back.
        QJsonObject entry = changer.apply(perms::plan(changer.read("com.example.chat"), "listener:com.example.chat/.Listener", "off"));
        QVERIFY(entry.value("ok").toBool());
        QCOMPARE(fake.listeners, QString("com.example.watch/.Bridge"));
        QVERIFY(changer.undo(entry.value("id").toInt()).value("undone").toBool());
        QVERIFY(fake.listeners.contains("com.example.chat/.Listener"));
        // Accessibility: the list is rewritten without the one service, and
        // a class name with $ survives the shell's quoting.
        entry = changer.apply(perms::plan(changer.read("com.example.chat"), "a11y:com.example.chat/com.example.chat.Svc$Inner", "off"));
        QVERIFY(entry.value("verified").toBool());
        QCOMPARE(fake.accessibility, QString("com.android.talkback/.TalkBack"));
        changer.undo(entry.value("id").toInt());
        QCOMPARE(fake.accessibility, QString("com.android.talkback/.TalkBack:com.example.chat/com.example.chat.Svc$Inner"));
        // Pause and disable.
        entry = changer.apply(perms::plan(changer.read("com.example.chat"), "app:pause", "paused"));
        QVERIFY(entry.value("verified").toBool());
        QVERIFY(fake.apps["com.example.chat"].suspended);
        entry = changer.apply(perms::plan(changer.read("com.example.chat"), "app:disable", "disabled"));
        QVERIFY(entry.value("verified").toBool());
        QCOMPARE(fake.apps["com.example.chat"].enabled, 3);
        QCOMPARE(changer.journal().size(), 4);
    }

    void aRefusedChangeIsReportedNotTrusted() {
        StatefulPhone fake = phone();
        Adb adb("SERIAL", fake.runner(), "/usr/bin/adb");
        perms::Changer changer(adb, "PHONE1", m_journal->path());
        const perms::Plan plan = perms::plan(changer.read("com.example.chat"), "perm:microphone", "deny");
        fake.failNext = true;
        const QJsonObject entry = changer.apply(plan);
        QVERIFY(!entry.value("ok").toBool());
        QVERIFY(!entry.value("verified").toBool());
        QCOMPARE(entry.value("now").toString(), QString("allow"));
        QVERIFY(entry.value("output").toString().contains("SecurityException"));
    }

    void watchFindingsGetAFix() {
        QCOMPARE(perms::fixFor({{"key", "bg|com.x|RECORD_AUDIO"}}).value("control").toString(), QString("perm:microphone"));
        QCOMPARE(perms::fixFor({{"key", "bg|com.x|FINE_LOCATION"}}).value("to").toString(), QString("while_using"));
        QCOMPARE(perms::fixFor({{"key", "listener|com.x/.L"}}).value("control").toString(), QString("listener:com.x/.L"));
        QCOMPARE(perms::fixFor({{"key", "grant|com.x|GET_USAGE_STATS"}}).value("control").toString(), QString("special:usage"));
        QVERIFY(perms::fixFor({{"key", "admin|com.x/.A"}}).isEmpty()); // only the phone can
        QVERIFY(perms::fixFor({{"key", "patch"}}).isEmpty());
    }

    void reviewAsksEachThingOnceInOrder() {
        Capture capture;
        capture.texts["installers"] = "package:com.example.chat  installer=com.android.vending uid:10305\n"
                                      "package:com.example.maps  installer=com.android.vending uid:10410\n";
        capture.texts["package_dump"] =
            "Packages:\n"
            "  Package [com.example.chat] (1):\n"
            "    appId=10305\n"
            "    install permissions:\n"
            "      android.permission.ACCESS_BACKGROUND_LOCATION: granted=true\n"
            "      android.permission.RECORD_AUDIO: granted=true\n"
            "  Package [com.example.maps] (2):\n"
            "    appId=10410\n"
            "    install permissions:\n"
            "      android.permission.ACCESS_FINE_LOCATION: granted=true\n";
        const QJsonArray findings{
            QJsonObject{{"key", "bg|com.example.maps|FINE_LOCATION"}, {"level", "warn"}, {"package", "com.example.maps"},
                        {"title", "Maps accessed precise location while not on screen"}},
            QJsonObject{{"key", "bg|com.example.chat|RECORD_AUDIO"}, {"level", "alert"}, {"package", "com.example.chat"},
                        {"title", "Chat accessed the microphone while not on screen"}},
        };
        const QList<perms::Question> questions = perms::review(capture, findings, {});
        // The alert first; Maps' location isn't asked (it isn't "all the
        // time", so there is nothing to narrow); Chat's microphone isn't
        // asked twice (Watch and the unused-microphone preset).
        QCOMPARE(questions.size(), 2);
        QCOMPARE(questions.at(0).control, QString("perm:microphone"));
        QCOMPARE(questions.at(0).title, QString("Chat accessed the microphone while not on screen"));
        QCOMPARE(questions.at(0).suggested, QString("deny"));
        QCOMPARE(questions.at(0).options.first().state, QString("keep"));
        QCOMPARE(questions.at(1).control, QString("perm:location"));
        QCOMPARE(questions.at(1).package, QString("com.example.chat"));
        QCOMPARE(questions.at(1).suggested, QString("while_using"));
        QCOMPARE(questions.at(1).options.size(), 3);
        QCOMPARE(questions.at(0).round, 1); // from Watch
        QCOMPARE(questions.at(1).round, 1); // location all the time
        // What was kept isn't asked again.
        const auto fewer = perms::review(capture, findings, {perms::questionKey("com.example.chat", "perm:microphone")});
        QCOMPARE(fewer.size(), 1);
        QCOMPARE(fewer.first().control, QString("perm:location"));
    }

    void keptAnswersAreRemembered() {
        perms::Decisions decisions("PHONE1", m_journal->path());
        QVERIFY(decisions.kept().isEmpty());
        decisions.keep("com.example.chat|perm:camera");
        QVERIFY(perms::Decisions("PHONE1", m_journal->path()).kept().contains("com.example.chat|perm:camera"));
        QVERIFY(perms::Decisions("PHONE2", m_journal->path()).kept().isEmpty());
        decisions.forget();
        QVERIFY(decisions.kept().isEmpty());
    }

    void presetsListOnlyWhatApplies() {
        Capture capture;
        capture.texts["installers"] = "package:com.example.chat  installer=com.android.vending uid:10305\n"
                                      "package:com.example.maps  installer=com.android.vending uid:10410\n";
        capture.texts["package_dump"] =
            "Packages:\n"
            "  Package [com.example.chat] (1):\n"
            "    appId=10305\n"
            "    install permissions:\n"
            "      android.permission.ACCESS_BACKGROUND_LOCATION: granted=true\n"
            "      android.permission.RECORD_AUDIO: granted=true\n"
            "      android.permission.CAMERA: granted=true\n"
            "  Package [com.example.maps] (2):\n"
            "    appId=10410\n"
            "    install permissions:\n"
            "      android.permission.RECORD_AUDIO: granted=true\n"
            "  Package [com.android.system] (3):\n"
            "    appId=1000\n"
            "    install permissions:\n"
            "      android.permission.ACCESS_BACKGROUND_LOCATION: granted=true\n";
        // Maps used the microphone 2 days ago; chat never.
        capture.texts["appops"] = "  Uid u0a410:\n    Package com.example.maps:\n      RECORD_AUDIO (allow):\n        null=[\n"
                                  "          Access: [top-s] 2026-11-14 10:00:00.000 (-2d0h0m0s0ms)\n        ]\n";
        const auto background = perms::preset("background-location", capture, {});
        QCOMPARE(background.size(), 1); // not the system package
        QCOMPARE(background.first().package, QString("com.example.chat"));
        QCOMPARE(background.first().to, QString("while_using"));
        const auto unused = perms::preset("unused-mic-camera", capture, {});
        QCOMPARE(unused.size(), 2); // chat's microphone and camera, not maps'
        for (const perms::Step &step : unused)
            QCOMPARE(step.package, QString("com.example.chat"));
        const QJsonArray findings{QJsonObject{{"key", "bg|com.example.maps|RECORD_AUDIO"}, {"level", "alert"},
                                              {"package", "com.example.maps"}, {"title", "Maps accessed the microphone"}}};
        const auto watch = perms::preset("watch", capture, findings);
        QCOMPARE(watch.size(), 1);
        QCOMPARE(watch.first().control, QString("perm:microphone"));
    }
};

} // namespace

int runPermsTests(int argc, char **argv) {
    PermsTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "perms_tests.moc"
