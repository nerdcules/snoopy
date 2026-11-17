#include "perms.h"

#include "graph.h"
#include "parsers.h"
#include "watch.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace perms {

namespace {

const QString P = QStringLiteral("android.permission.");

// A control's state as the phone reports it now. Notification access and
// accessibility rows exist only while they are on, so a missing one is off.
QString stateNow(const AppState &app, const QString &id);

struct Kind {
    const char *id;
    const char *label;
    QStringList permissions;
};

const QList<Kind> &kinds() {
    static const QList<Kind> list = {
        {"location", QT_TR_NOOP("Location"), {P + "ACCESS_FINE_LOCATION", P + "ACCESS_COARSE_LOCATION", P + "ACCESS_BACKGROUND_LOCATION"}},
        {"microphone", QT_TR_NOOP("Microphone"), {P + "RECORD_AUDIO"}},
        {"camera", QT_TR_NOOP("Camera"), {P + "CAMERA"}},
        {"contacts", QT_TR_NOOP("Contacts"), {P + "READ_CONTACTS", P + "WRITE_CONTACTS", P + "GET_ACCOUNTS"}},
        {"sms", QT_TR_NOOP("Text messages"), {P + "READ_SMS", P + "RECEIVE_SMS", P + "SEND_SMS", P + "RECEIVE_MMS", P + "RECEIVE_WAP_PUSH"}},
        {"calllog", QT_TR_NOOP("Call history"), {P + "READ_CALL_LOG", P + "WRITE_CALL_LOG", P + "PROCESS_OUTGOING_CALLS"}},
        {"phone", QT_TR_NOOP("Phone"), {P + "READ_PHONE_STATE", P + "READ_PHONE_NUMBERS", P + "CALL_PHONE", P + "ANSWER_PHONE_CALLS", P + "ADD_VOICEMAIL", P + "USE_SIP"}},
        {"calendar", QT_TR_NOOP("Calendar"), {P + "READ_CALENDAR", P + "WRITE_CALENDAR"}},
        {"photos", QT_TR_NOOP("Photos and videos"), {P + "READ_MEDIA_IMAGES", P + "READ_MEDIA_VIDEO", P + "READ_MEDIA_VISUAL_USER_SELECTED", P + "READ_EXTERNAL_STORAGE", P + "WRITE_EXTERNAL_STORAGE"}},
        {"audio", QT_TR_NOOP("Music and audio"), {P + "READ_MEDIA_AUDIO"}},
        {"sensors", QT_TR_NOOP("Body sensors"), {P + "BODY_SENSORS", P + "BODY_SENSORS_BACKGROUND"}},
        {"activity", QT_TR_NOOP("Physical activity"), {P + "ACTIVITY_RECOGNITION"}},
        {"nearby", QT_TR_NOOP("Nearby devices"), {P + "BLUETOOTH_SCAN", P + "BLUETOOTH_CONNECT", P + "BLUETOOTH_ADVERTISE", P + "NEARBY_WIFI_DEVICES", P + "UWB_RANGING"}},
        {"notifications", QT_TR_NOOP("Notifications"), {P + "POST_NOTIFICATIONS"}},
    };
    return list;
}

const Kind *kind(const QString &id) {
    for (const Kind &k : kinds())
        if (id == QLatin1String(k.id))
            return &k;
    return nullptr;
}

// App ops the panel switches directly, with the permission an app requests
// for them (empty: shown for every third-party app that has a mode).
struct OpControl {
    const char *id;
    const char *op;
    const char *permission;
    const char *label;
    const char *group;
};
const OpControl kOps[] = {
    {"op:clipboard", "READ_CLIPBOARD", "", QT_TR_NOOP("Read the clipboard"), "permission"},
    {"op:background", "RUN_ANY_IN_BACKGROUND", "", QT_TR_NOOP("Run in the background"), "permission"},
    {"special:usage", "GET_USAGE_STATS", "android.permission.PACKAGE_USAGE_STATS", QT_TR_NOOP("Usage access (which apps you use)"), "special"},
    {"special:install", "REQUEST_INSTALL_PACKAGES", "android.permission.REQUEST_INSTALL_PACKAGES", QT_TR_NOOP("Install other apps"), "special"},
    {"special:overlay", "SYSTEM_ALERT_WINDOW", "android.permission.SYSTEM_ALERT_WINDOW", QT_TR_NOOP("Display over other apps"), "special"},
    {"special:files", "MANAGE_EXTERNAL_STORAGE", "android.permission.MANAGE_EXTERNAL_STORAGE", QT_TR_NOOP("All-files access"), "special"},
};

const OpControl *opControl(const QString &id) {
    for (const OpControl &c : kOps)
        if (id == QLatin1String(c.id))
            return &c;
    return nullptr;
}

bool fixed(const Permission &permission) {
    return permission.flags.contains("SYSTEM_FIXED") || permission.flags.contains("POLICY_FIXED");
}

QString modeOf(const AppState &app, const QString &op) {
    return app.packageModes.value(op, app.uidModes.value(op));
}

bool opOn(const QString &mode) {
    return mode == "allow" || mode == "foreground";
}

QStringList pmPermission(const QString &verb, const QString &package, const QString &permission) {
    return {"pm", verb, "--user", "0", package, permission};
}

QStringList appopsSet(const QString &package, const QString &op, const QString &mode) {
    return {"cmd", "appops", "set", package, op, mode.isEmpty() ? QStringLiteral("default") : mode};
}

// Single-quoted for the phone's sh; adb joins shell arguments with spaces.
QString quoted(const QString &argument) {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_./:=,@+-]+$"));
    if (plain.match(argument).hasMatch())
        return argument;
    QString escaped = argument;
    escaped.replace('\'', "'\\''");
    return '\'' + escaped + '\'';
}

QString kindState(const AppState &app, const Kind &k) {
    bool any = false;
    for (const QString &permission : k.permissions)
        any = any || app.permissions.value(permission).granted;
    if (QLatin1String(k.id) != QLatin1String("location"))
        return any ? QStringLiteral("allow") : QStringLiteral("deny");
    if (app.permissions.value(P + "ACCESS_BACKGROUND_LOCATION").granted)
        return QStringLiteral("always");
    return app.permissions.value(P + "ACCESS_FINE_LOCATION").granted ||
                   app.permissions.value(P + "ACCESS_COARSE_LOCATION").granted
               ? QStringLiteral("while_using")
               : QStringLiteral("deny");
}

// The settings list without (or with) one component.
QString withoutComponent(const QString &list, const QString &component) {
    QStringList items;
    for (const QString &item : list.split(':', Qt::SkipEmptyParts))
        if (item.trimmed() != component && item.trimmed() != "null")
            items.append(item.trimmed());
    return items.join(':');
}

} // namespace

AppState parseApp(const QString &package, const QString &packageDump, const QString &appops, const QString &security,
                  const QString &devicePolicy) {
    AppState app;
    app.package = package;
    // Only this package's block: from its header to the next one.
    const QString header = QStringLiteral("Package [%1]").arg(package);
    const int start = int(packageDump.indexOf(header));
    if (start >= 0) {
        app.found = true;
        int end = int(packageDump.indexOf(QStringLiteral("Package ["), start + header.size()));
        const int hidden = int(packageDump.indexOf(QStringLiteral("Hidden system packages"), start));
        if (hidden >= 0 && (end < 0 || hidden < end))
            end = hidden;
        const QString block = packageDump.mid(start, end < 0 ? -1 : end - start);
        static const QRegularExpression flagsLine(QStringLiteral("^\\s+pkgFlags=\\[([^\\]]*)\\]"));
        static const QRegularExpression userLine(QStringLiteral("^\\s+User 0:.*"));
        static const QRegularExpression permLine(QStringLiteral("^\\s+(android\\.permission\\.[A-Z0-9_]+|[a-z][\\w.]*\\.permission\\.[\\w.]+): granted=(true|false)(?:, flags=\\[\\s*([^\\]]*)\\])?"));
        QString section;
        bool inUser = false, runtime = false;
        for (const QString &line : block.split('\n')) {
            const QString text = line.trimmed();
            QRegularExpressionMatch m = flagsLine.match(line);
            if (m.hasMatch()) {
                app.system = m.captured(1).split(' ', Qt::SkipEmptyParts).contains("SYSTEM");
                continue;
            }
            if (userLine.match(line).hasMatch()) {
                if (inUser) // a second User 0 belongs to another listing
                    break;
                inUser = true;
                app.suspended = line.contains("suspended=true");
                static const QRegularExpression enabled(QStringLiteral("enabled=(\\d)"));
                const QRegularExpressionMatch e = enabled.match(line);
                if (e.hasMatch())
                    app.enabled = e.captured(1).toInt();
                continue;
            }
            // A section header: "requested permissions:", "runtime permissions:".
            if (text.endsWith(':') && !text.contains('=') && !text.chopped(1).contains(':')) {
                section = text.chopped(1);
                runtime = inUser && section == "runtime permissions";
                continue;
            }
            if (section == "requested permissions" && !text.isEmpty() && !text.contains(' ')) {
                app.requested.insert(text.section(':', 0, 0));
                continue;
            }
            m = permLine.match(line);
            if (m.hasMatch() && runtime) {
                Permission permission;
                permission.granted = m.captured(2) == "true";
                permission.flags = m.captured(3).split('|', Qt::SkipEmptyParts);
                for (QString &flag : permission.flags)
                    flag = flag.trimmed();
                app.permissions.insert(m.captured(1), permission);
            }
        }
    }
    // App ops: the uid's modes come first, then the package's; the last line
    // for an op is the one in force.
    static const QRegularExpression opLine(QStringLiteral("^(?:Uid mode: )?([A-Z][A-Z0-9_]+): ([a-z]+)(.*)$"));
    static const QRegularExpression time(QStringLiteral("time=\\+([0-9dhms]+) ago"));
    bool uidBlock = true;
    for (const QString &raw : appops.split('\n')) {
        const QString line = raw.trimmed();
        const QRegularExpressionMatch m = opLine.match(line);
        if (!m.hasMatch())
            continue;
        if (m.captured(3).contains(';'))
            uidBlock = false;
        if (uidBlock && !app.uidModes.contains(m.captured(1)))
            app.uidModes.insert(m.captured(1), m.captured(2));
        else
            app.packageModes.insert(m.captured(1), m.captured(2));
        const QRegularExpressionMatch t = time.match(m.captured(3));
        if (t.hasMatch())
            app.lastAccess.insert(m.captured(1), agoSeconds("-" + t.captured(1)));
    }
    const QHash<QString, QString> values = parseSecurity(security);
    for (const QString &component : values.value("listeners").split(':', Qt::SkipEmptyParts))
        if (component.section('/', 0, 0) == package)
            app.listeners.append(component.trimmed());
    for (const QString &component : values.value("accessibility").split(':', Qt::SkipEmptyParts))
        if (component.section('/', 0, 0) == package)
            app.accessibility.append(component.trimmed());
    static const QRegularExpression admin(QStringLiteral("^\\s{4}(\\S+/\\S+):\\s*$"));
    bool admins = false;
    for (const QString &line : devicePolicy.split('\n')) {
        if (line.trimmed().startsWith("Enabled Device Admins")) {
            admins = true;
            continue;
        }
        const int indent = int(line.size() - line.trimmed().size());
        if (admins && indent <= 2 && !line.trimmed().isEmpty())
            admins = false;
        const QRegularExpressionMatch m = admin.match(line);
        if (admins && m.hasMatch() && m.captured(1).section('/', 0, 0) == package)
            app.admins.append(m.captured(1));
    }
    return app;
}

QList<Control> controls(const AppState &app) {
    QList<Control> out;
    for (const Kind &k : kinds()) {
        bool present = false, allFixed = true, system = false;
        for (const QString &permission : k.permissions) {
            if (!app.permissions.contains(permission))
                continue;
            present = true;
            const Permission p = app.permissions.value(permission);
            allFixed = allFixed && fixed(p);
            system = system || p.flags.contains("SYSTEM_FIXED");
        }
        if (!present)
            continue;
        Control control;
        control.id = QStringLiteral("perm:") + k.id;
        control.group = "permission";
        control.label = QObject::tr(k.label);
        control.state = kindState(app, k);
        if (QLatin1String(k.id) == QLatin1String("location"))
            control.options = app.permissions.contains(P + "ACCESS_BACKGROUND_LOCATION")
                                  ? QStringList{"always", "while_using", "deny"}
                                  : QStringList{"while_using", "deny"};
        else
            control.options = {"allow", "deny"};
        if (allFixed) {
            control.changeable = false;
            control.reason = system ? QObject::tr("Set by the system; Android doesn't let it be changed.")
                                    : QObject::tr("Set by a device policy (work profile or management).");
        }
        out.append(control);
    }
    for (const OpControl &c : kOps) {
        const QString mode = modeOf(app, c.op);
        const bool requested = QLatin1String(c.permission).isEmpty() ? !app.system
                                                                     : app.requested.contains(QString::fromLatin1(c.permission));
        if (!requested && !opOn(mode))
            continue;
        if (QLatin1String(c.id) == QLatin1String("op:clipboard") && mode.isEmpty())
            continue;
        Control control;
        control.id = c.id;
        control.group = c.group;
        control.label = QObject::tr(c.label);
        const bool on = QLatin1String(c.group) == QLatin1String("special") ? opOn(mode)
                                                                         : mode != "ignore" && mode != "deny";
        control.state = on ? (QLatin1String(c.group) == QLatin1String("special") ? "on" : "allow")
                           : (QLatin1String(c.group) == QLatin1String("special") ? "off" : "deny");
        control.options = QLatin1String(c.group) == QLatin1String("special") ? QStringList{"on", "off"}
                                                                            : QStringList{"allow", "deny"};
        out.append(control);
    }
    for (const QString &component : app.listeners)
        out.append({"listener:" + component, "special", QObject::tr("Notification access"), "on", {"on", "off"}, true, {}});
    for (const QString &component : app.accessibility)
        out.append({"a11y:" + component, "special", QObject::tr("Accessibility service (reads and controls the screen)"),
                    "on", {"on", "off"}, true, {}});
    for (const QString &component : app.admins)
        out.append({"admin:" + component, "special", QObject::tr("Device administrator"), "on", {"on"}, false,
                    QObject::tr("Android only lets you turn this off on the phone: Settings > Security > Device admin apps.")});
    const bool keep = essential(app.package);
    const QString why = QObject::tr("The phone needs this app; Snoopy won't stop it.");
    out.append({"app:pause", "app", QObject::tr("Pause the app"), app.suspended ? "paused" : "active", {"active", "paused"},
                !keep, keep ? why : QString()});
    out.append({"app:disable", "app", QObject::tr("Disable the app"), app.enabled >= 2 ? "disabled" : "enabled",
                {"enabled", "disabled"}, !keep, keep ? why : QString()});
    out.append({"app:remove", "app", QObject::tr("Remove for this user (it can be put back)"), app.found ? "installed" : "removed",
                {"installed", "removed"}, !keep, keep ? why : QString()});
    return out;
}

bool essential(const QString &package) {
    static const QSet<QString> list = {
        "android", "com.android.systemui", "com.android.phone", "com.android.settings", "com.android.shell",
        "com.android.permissioncontroller", "com.google.android.permissioncontroller", "com.google.android.gms",
        "com.android.providers.settings", "com.android.providers.telephony", "com.android.providers.contacts",
        "com.android.providers.media", "com.android.providers.downloads", "com.android.packageinstaller",
        "com.google.android.packageinstaller", "com.android.vending", "com.google.android.apps.nexuslauncher",
        "com.android.launcher3", "com.android.server.telecom", "com.android.bluetooth", "com.android.networkstack",
        "com.google.android.networkstack", "com.android.inputmethod.latin", "com.google.android.inputmethod.latin",
        "com.android.nfc", "com.google.android.ext.services", "com.android.se",
    };
    return list.contains(package) || package.startsWith("com.android.providers.");
}

Plan plan(const AppState &app, const QString &id, const QString &to) {
    Plan p;
    p.package = app.package;
    p.control = id;
    p.to = to;
    const QString name = friendlyName(app.package);
    Control current;
    for (const Control &c : controls(app))
        if (c.id == id)
            current = c;
    if (current.id.isEmpty()) {
        p.blocked = true;
        p.reason = QObject::tr("%1 has no such control.").arg(name);
        return p;
    }
    p.label = current.label;
    p.from = current.state;
    if (!current.changeable) {
        p.blocked = true;
        p.reason = current.reason;
        return p;
    }
    if (!current.options.contains(to)) {
        p.blocked = true;
        p.reason = QObject::tr("%1 can't be set to %2.").arg(current.label, to);
        return p;
    }
    if (id.startsWith("perm:")) {
        const Kind *k = kind(id.mid(5));
        for (const QString &permission : k->permissions) {
            if (!app.permissions.contains(permission))
                continue;
            const Permission now = app.permissions.value(permission);
            if (fixed(now))
                continue;
            bool want = to == "allow" || to == "always";
            if (to == "while_using")
                want = permission != P + "ACCESS_BACKGROUND_LOCATION";
            if (want == now.granted)
                continue;
            p.commands.append(pmPermission(want ? "grant" : "revoke", app.package, permission));
            p.undo.append(pmPermission(want ? "revoke" : "grant", app.package, permission));
        }
        p.detail = to == "deny"          ? QObject::tr("%1 loses %2. It can ask again; Android will show you the prompt.").arg(name, current.label)
                   : to == "while_using" ? QObject::tr("%1 keeps %2 only while you use it.").arg(name, current.label)
                                         : QObject::tr("%1 gets %2.").arg(name, current.label);
    } else if (const OpControl *c = opControl(id)) {
        const QString before = app.packageModes.value(c->op);
        const QString mode = to == "allow" || to == "on" ? QStringLiteral("allow") : QStringLiteral("ignore");
        if (mode != modeOf(app, c->op)) {
            p.commands.append(appopsSet(app.package, c->op, mode));
            p.undo.append(appopsSet(app.package, c->op, before));
        }
        p.detail = (to == "allow" || to == "on") ? QObject::tr("%1 is allowed: %2.").arg(name, current.label)
                                                 : QObject::tr("%1 is no longer allowed: %2.").arg(name, current.label);
    } else if (id.startsWith("listener:")) {
        const QString component = id.mid(9);
        p.commands.append({"cmd", "notification", to == "off" ? "disallow_listener" : "allow_listener", component});
        p.undo.append({"cmd", "notification", to == "off" ? "allow_listener" : "disallow_listener", component});
        p.detail = QObject::tr("%1 stops seeing your notifications.").arg(name);
    } else if (id.startsWith("a11y:")) {
        // Written whole at apply time from the phone's current list; the
        // undo puts the list back as it was.
        p.commands.append({"a11y-remove", id.mid(5)});
        p.undo.append({"a11y-add", id.mid(5)});
        p.detail = QObject::tr("%1's accessibility service is turned off: it no longer sees or controls the screen.").arg(name);
    } else if (id == "app:pause") {
        p.commands.append({"pm", to == "paused" ? "suspend" : "unsuspend", "--user", "0", app.package});
        p.undo.append({"pm", to == "paused" ? "unsuspend" : "suspend", "--user", "0", app.package});
        p.detail = QObject::tr("%1 is paused: its icon goes grey, it can't run, and tapping it says it's paused. Unpause here or on the phone.").arg(name);
    } else if (id == "app:disable") {
        p.commands.append(to == "disabled" ? QStringList{"pm", "disable-user", "--user", "0", app.package}
                                           : QStringList{"pm", "enable", "--user", "0", app.package});
        p.undo.append(to == "disabled" ? QStringList{"pm", "enable", "--user", "0", app.package}
                                       : QStringList{"pm", "disable-user", "--user", "0", app.package});
        p.detail = QObject::tr("%1 is disabled: it disappears from the app list and doesn't run until it is enabled again.").arg(name);
    } else if (id == "app:remove") {
        p.commands.append(to == "removed" ? QStringList{"pm", "uninstall", "-k", "--user", "0", app.package}
                                          : QStringList{"cmd", "package", "install-existing", "--user", "0", app.package});
        p.undo.append(to == "removed" ? QStringList{"cmd", "package", "install-existing", "--user", "0", app.package}
                                      : QStringList{"pm", "uninstall", "-k", "--user", "0", app.package});
        p.detail = app.system ? QObject::tr("%1 is removed for you; the phone keeps a copy, so it can be put back.").arg(name)
                              : QObject::tr("%1 is removed for you. Its data is kept, and it can be put back while the phone still has the app.").arg(name);
    }
    if (p.commands.isEmpty() && !p.blocked)
        p.reason = QObject::tr("Already %1.").arg(to);
    return p;
}

QJsonObject fixFor(const QJsonObject &finding) {
    const QString key = finding.value("key").toString();
    const QStringList parts = key.split('|');
    auto fix = [](const QString &control, const QString &to, const QString &label) {
        return QJsonObject{{"control", control}, {"to", to}, {"label", label}};
    };
    if (parts.value(0) == "bg" && parts.size() >= 3) {
        static const QHash<QString, QString> byOp = {
            {"RECORD_AUDIO", "microphone"}, {"CAMERA", "camera"}, {"READ_CONTACTS", "contacts"},
            {"READ_CALL_LOG", "calllog"}, {"READ_SMS", "sms"}, {"READ_CALENDAR", "calendar"},
            {"READ_MEDIA_IMAGES", "photos"}, {"READ_MEDIA_VIDEO", "photos"}, {"BODY_SENSORS", "sensors"},
            {"ACTIVITY_RECOGNITION", "activity"},
        };
        const QString op = parts.value(2);
        if (op == "FINE_LOCATION" || op == "COARSE_LOCATION")
            return fix("perm:location", "while_using", QObject::tr("Location only while using"));
        if (op == "READ_CLIPBOARD")
            return fix("op:clipboard", "deny", QObject::tr("Deny the clipboard"));
        if (byOp.contains(op))
            return fix("perm:" + byOp.value(op), "deny", QObject::tr("Deny"));
    }
    if (parts.value(0) == "listener")
        return fix("listener:" + parts.value(1), "off", QObject::tr("Turn off notification access"));
    if (parts.value(0) == "a11y")
        return fix("a11y:" + parts.value(1), "off", QObject::tr("Turn off the accessibility service"));
    if (parts.value(0) == "grant" && parts.size() >= 3) {
        static const QHash<QString, QString> byOp = {
            {"GET_USAGE_STATS", "special:usage"}, {"REQUEST_INSTALL_PACKAGES", "special:install"},
            {"SYSTEM_ALERT_WINDOW", "special:overlay"}, {"MANAGE_EXTERNAL_STORAGE", "special:files"},
        };
        if (byOp.contains(parts.value(2)))
            return fix(byOp.value(parts.value(2)), "off", QObject::tr("Turn off"));
    }
    return {};
}

Changer::Changer(Adb &adb, const QString &phone, const QString &directory)
    : m_adb(adb), m_phone(phone),
      m_directory(directory.isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/snoopy/changes"
                      : directory) {}

QString Changer::path() const {
    QString safe = m_phone;
    safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    return m_directory + "/" + (safe.isEmpty() ? QStringLiteral("phone") : safe) + ".json";
}

QJsonArray Changer::journal() const {
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).array();
}

void Changer::save(const QJsonArray &entries) const {
    QDir().mkpath(m_directory);
    QSaveFile file(path());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

AppState Changer::read(const QString &package) {
    const QString dump = m_adb.shell("dumpsys package " + quoted(package)).out;
    const QString ops = m_adb.shell("cmd appops get " + quoted(package)).out;
    const QString security = m_adb.shell("echo listeners=$(settings get secure enabled_notification_listeners); "
                                         "echo accessibility=$(settings get secure enabled_accessibility_services)").out;
    const QString policy = m_adb.shell("dumpsys device_policy").out;
    return parseApp(package, dump, ops, security, policy);
}

QString Changer::run(const QStringList &command, bool *ok) {
    // Accessibility: the setting is one list; change it as it is now.
    if (command.value(0) == "a11y-remove" || command.value(0) == "a11y-add") {
        const QString component = command.value(1);
        const QString now = m_adb.shell("settings get secure enabled_accessibility_services").out.trimmed();
        QString next = withoutComponent(now, component);
        if (command.value(0) == "a11y-add")
            next = next.isEmpty() ? component : next + ':' + component;
        const Adb::Shell result = next.isEmpty()
                                      ? m_adb.shell("settings delete secure enabled_accessibility_services")
                                      : m_adb.shell("settings put secure enabled_accessibility_services " + quoted(next));
        *ok = result.code == 0;
        return (result.out + result.err).trimmed();
    }
    QStringList words;
    for (const QString &word : command)
        words.append(quoted(word));
    const Adb::Shell result = m_adb.shell(words.join(' '));
    const QString output = (result.out + result.err).trimmed();
    // pm and cmd print an exception or "Error" and may still exit 0.
    *ok = result.code == 0 && !output.contains("Exception") && !output.startsWith("Error") &&
          !output.contains("Failure") && !output.contains("Unknown");
    return output;
}

QJsonObject Changer::apply(const Plan &plan) {
    QJsonArray entries = journal();
    int id = 0;
    for (const QJsonValue &entry : entries)
        id = qMax(id, entry.toObject().value("id").toInt());
    QJsonArray commands, undo;
    for (const QStringList &command : plan.commands)
        commands.append(QJsonArray::fromStringList(command));
    for (const QStringList &command : plan.undo)
        undo.append(QJsonArray::fromStringList(command));
    QJsonObject entry{{"id", id + 1},
                      {"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                      {"package", plan.package},
                      {"control", plan.control},
                      {"label", plan.label},
                      {"from", plan.from},
                      {"to", plan.to},
                      {"commands", commands},
                      {"undo", undo}};
    if (plan.blocked || plan.commands.isEmpty()) {
        entry.insert("ok", false);
        entry.insert("output", plan.blocked ? plan.reason : QObject::tr("Nothing to change."));
        return entry;
    }
    bool ok = true;
    QStringList output;
    for (const QStringList &command : plan.commands) {
        bool step = false;
        const QString said = run(command, &step);
        if (!said.isEmpty())
            output.append(said);
        ok = ok && step;
        if (!step)
            break;
    }
    // Read the app back: the change counts when the phone says so.
    const QString now = stateNow(read(plan.package), plan.control);
    entry.insert("ok", ok);
    entry.insert("now", now);
    entry.insert("verified", now == plan.to);
    entry.insert("output", output.join('\n'));
    entries.append(entry);
    save(entries);
    return entry;
}

QJsonObject Changer::undo(int id) {
    QJsonArray entries = journal();
    for (int i = 0; i < entries.size(); ++i) {
        QJsonObject entry = entries.at(i).toObject();
        if (entry.value("id").toInt() != id)
            continue;
        if (entry.value("undone").toBool())
            return entry;
        bool ok = true;
        QStringList output;
        for (const QJsonValue &value : entry.value("undo").toArray()) {
            QStringList command;
            for (const QJsonValue &word : value.toArray())
                command.append(word.toString());
            bool step = false;
            const QString said = run(command, &step);
            if (!said.isEmpty())
                output.append(said);
            ok = ok && step;
        }
        const QString now = stateNow(read(entry.value("package").toString()), entry.value("control").toString());
        entry.insert("undone", ok);
        entry.insert("undoneAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        entry.insert("undoNow", now);
        entry.insert("undoOutput", output.join('\n'));
        entries[i] = entry;
        save(entries);
        return entry;
    }
    return {{"ok", false}, {"output", QObject::tr("No change %1 in the journal.").arg(id)}};
}

namespace {

QString stateNow(const AppState &app, const QString &id) {
    for (const Control &c : controls(app))
        if (c.id == id)
            return c.state;
    if (id.startsWith("listener:") || id.startsWith("a11y:"))
        return QStringLiteral("off");
    if (id == "app:remove" && !app.found)
        return QStringLiteral("removed");
    return {};
}

} // namespace

QStringList presetIds() {
    return {"background-location", "unused-mic-camera", "unused-personal", "watch"};
}

QString presetTitle(const QString &id) {
    if (id == "background-location")
        return QObject::tr("Location only while using, for apps that have it all the time");
    if (id == "unused-mic-camera")
        return QObject::tr("Microphone and camera off for apps that haven't used them in 30 days");
    if (id == "unused-personal")
        return QObject::tr("Contacts, texts, call history and calendar off for apps that haven't used them in 30 days");
    if (id == "watch")
        return QObject::tr("Fix what the Watch page warns about");
    return id;
}

QList<Step> preset(const QString &id, const Capture &capture, const QJsonArray &findings) {
    QList<Step> steps;
    const QHash<QString, QString> installers = parseInstallers(capture.text("installers"));
    if (id == "watch") {
        for (const QJsonValue &value : findings) {
            const QJsonObject item = value.toObject();
            if (item.value("level").toString() == "info")
                continue;
            const QJsonObject fix = fixFor(item);
            const QString package = item.value("package").toString();
            if (fix.isEmpty() || package.isEmpty() || !installers.contains(package))
                continue;
            steps.append({package, fix.value("control").toString(), fix.value("to").toString(),
                          fix.value("label").toString() + ": " + friendlyName(package), item.value("title").toString()});
        }
        return steps;
    }
    // Granted runtime permissions per package, and the last use of each op.
    const auto details = parsePackageDump(capture.text("package_dump"));
    QHash<QString, qint64> lastUse;
    for (const OpAccess &access : parseOpAccesses(capture.text("appops"))) {
        if (access.rejected || access.ago < 0)
            continue;
        const QString key = access.package + '|' + access.op;
        if (!lastUse.contains(key) || access.ago < lastUse.value(key))
            lastUse.insert(key, access.ago);
    }
    auto unused = [&](const QString &package, const QStringList &ops) {
        for (const QString &op : ops)
            if (lastUse.contains(package + '|' + op) && lastUse.value(package + '|' + op) < 30 * 86400)
                return false;
        return true;
    };
    QStringList packages = installers.keys();
    packages.sort();
    for (const QString &package : packages) {
        const PackageDetails *detail = details.find(package);
        if (!detail)
            continue;
        const QStringList &granted = detail->granted;
        const QString name = friendlyName(package);
        if (id == "background-location" && granted.contains(P + "ACCESS_BACKGROUND_LOCATION"))
            steps.append({package, "perm:location", "while_using", QObject::tr("%1: location only while using").arg(name),
                          QObject::tr("Can use your location all the time.")});
        if (id == "unused-mic-camera") {
            if (granted.contains(P + "RECORD_AUDIO") && unused(package, {"RECORD_AUDIO"}))
                steps.append({package, "perm:microphone", "deny", QObject::tr("%1: microphone off").arg(name),
                              QObject::tr("Allowed, and no use recorded in 30 days.")});
            if (granted.contains(P + "CAMERA") && unused(package, {"CAMERA"}))
                steps.append({package, "perm:camera", "deny", QObject::tr("%1: camera off").arg(name),
                              QObject::tr("Allowed, and no use recorded in 30 days.")});
        }
        if (id == "unused-personal") {
            const struct {
                const char *kind;
                const char *permission;
                QStringList ops;
                const char *label;
            } personal[] = {
                {"contacts", "READ_CONTACTS", {"READ_CONTACTS", "WRITE_CONTACTS"}, QT_TR_NOOP("contacts")},
                {"sms", "READ_SMS", {"READ_SMS", "RECEIVE_SMS", "SEND_SMS"}, QT_TR_NOOP("text messages")},
                {"calllog", "READ_CALL_LOG", {"READ_CALL_LOG", "WRITE_CALL_LOG"}, QT_TR_NOOP("call history")},
                {"calendar", "READ_CALENDAR", {"READ_CALENDAR", "WRITE_CALENDAR"}, QT_TR_NOOP("the calendar")},
            };
            for (const auto &item : personal)
                if (granted.contains(P + item.permission) && unused(package, item.ops))
                    steps.append({package, QStringLiteral("perm:") + item.kind, "deny",
                                  QObject::tr("%1: %2 off").arg(name, QObject::tr(item.label)),
                                  QObject::tr("Allowed, and no use recorded in 30 days.")});
        }
    }
    return steps;
}

QString questionKey(const QString &package, const QString &control) {
    return package + '|' + control;
}

namespace {

// The words and choices for one control.
Question question(const QString &package, const QString &control, const QString &title, const QString &why) {
    Question q{package, control, title, why, QString(), {}};
    if (control == "perm:location") {
        q.options = {{"keep", QObject::tr("Keep all the time")}, {"while_using", QObject::tr("Only while using")},
                     {"deny", QObject::tr("Turn off")}};
        q.suggested = "while_using";
    } else {
        const bool onOff = control.startsWith("special:") || control.startsWith("listener:") || control.startsWith("a11y:");
        q.options = {{"keep", QObject::tr("Keep")}, {onOff ? "off" : "deny", QObject::tr("Turn off")}};
        q.suggested = onOff ? "off" : "deny";
    }
    return q;
}

QString whyFor(const QString &control) {
    if (control.startsWith("listener:"))
        return QObject::tr("It can read the text of every notification, messages included.");
    if (control.startsWith("a11y:"))
        return QObject::tr("It can see everything on screen and tap and type for you.");
    if (control == "special:usage")
        return QObject::tr("It can see which apps you use, and when.");
    if (control == "special:install")
        return QObject::tr("It can install other apps.");
    if (control == "special:overlay")
        return QObject::tr("It can draw on top of other apps.");
    if (control == "special:files")
        return QObject::tr("It can read every file on the phone.");
    if (control == "perm:location")
        return QObject::tr("Most apps only need your location while you use them.");
    return QObject::tr("It did this while you weren't using it. If you can't think why, turn it off; it will ask again if it needs it.");
}

} // namespace

QList<Question> review(const Capture &capture, const QJsonArray &findings, const QSet<QString> &kept) {
    QList<Question> out;
    QSet<QString> asked;
    const QHash<QString, QString> installers = parseInstallers(capture.text("installers"));
    const auto details = parsePackageDump(capture.text("package_dump"));
    auto grantedBackground = [&](const QString &package) {
        const PackageDetails *detail = details.find(package);
        return detail && detail->granted.contains(P + "ACCESS_BACKGROUND_LOCATION");
    };
    auto add = [&](const Question &q) {
        const QString key = questionKey(q.package, q.control);
        if (asked.contains(key) || kept.contains(key) || !installers.contains(q.package))
            return;
        // Location can only be narrowed when the app has it all the time.
        if (q.control == "perm:location" && !grantedBackground(q.package))
            return;
        asked.insert(key);
        out.append(q);
    };
    // Watch first: alerts, then warnings, in the page's order.
    for (const QString level : {QStringLiteral("alert"), QStringLiteral("warn")}) {
        for (const QJsonValue &value : findings) {
            const QJsonObject item = value.toObject();
            if (item.value("level").toString() != level)
                continue;
            const QJsonObject fix = fixFor(item);
            if (fix.isEmpty())
                continue;
            const QString control = fix.value("control").toString();
            add(question(item.value("package").toString(), control, item.value("title").toString(), whyFor(control)));
        }
    }
    for (const QString &id : {QStringLiteral("background-location"), QStringLiteral("unused-mic-camera"), QStringLiteral("unused-personal")}) {
        for (const Step &step : preset(id, capture, findings)) {
            const QString name = friendlyName(step.package);
            QString title;
            QString why = QObject::tr("No use in 30 days. Turning it off costs nothing; it will ask again if it needs it.");
            if (step.control == "perm:location") {
                title = QObject::tr("%1 can use your location all the time").arg(name);
                why = whyFor(step.control);
            } else if (step.control == "perm:microphone")
                title = QObject::tr("%1 can use the microphone").arg(name);
            else if (step.control == "perm:camera")
                title = QObject::tr("%1 can use the camera").arg(name);
            else if (step.control == "perm:contacts")
                title = QObject::tr("%1 can read your contacts").arg(name);
            else if (step.control == "perm:sms")
                title = QObject::tr("%1 can read your text messages").arg(name);
            else if (step.control == "perm:calllog")
                title = QObject::tr("%1 can read your call history").arg(name);
            else if (step.control == "perm:calendar")
                title = QObject::tr("%1 can read your calendar").arg(name);
            else
                title = step.title;
            Question q = question(step.package, step.control, title, why);
            q.round = id == "background-location" ? 1 : 2;
            add(q);
        }
    }
    // Round 1 before round 2, each in the order found.
    std::stable_sort(out.begin(), out.end(), [](const Question &a, const Question &b) { return a.round < b.round; });
    return out;
}

Decisions::Decisions(const QString &phone, const QString &directory)
    : m_phone(phone),
      m_directory(directory.isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/snoopy/review"
                      : directory) {}

QString Decisions::path() const {
    QString safe = m_phone;
    safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    return m_directory + "/" + (safe.isEmpty() ? QStringLiteral("phone") : safe) + ".json";
}

QSet<QString> Decisions::kept() const {
    QSet<QString> keys;
    QFile file(path());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object().value("kept").toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            keys.insert(it.key());
    }
    return keys;
}

void Decisions::keep(const QString &key) {
    QJsonObject root;
    QFile file(path());
    if (file.open(QIODevice::ReadOnly))
        root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonObject kept = root.value("kept").toObject();
    kept.insert(key, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert("kept", kept);
    QDir().mkpath(m_directory);
    QSaveFile out(path());
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out.commit();
    }
}

void Decisions::forget() {
    QFile::remove(path());
}

} // namespace perms
