#include "watch.h"

#include "graph.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace {

// "u0a305" is app 305 of user 0 (uid 10305); "1000" is itself.
qint64 uidOf(const QString &token) {
    static const QRegularExpression app(QStringLiteral("^u(\\d+)a(\\d+)$"));
    const QRegularExpressionMatch match = app.match(token);
    if (match.hasMatch())
        return match.captured(1).toLongLong() * 100000 + 10000 + match.captured(2).toLongLong();
    bool ok = false;
    const qint64 value = token.toLongLong(&ok);
    return ok ? value : -1;
}

qint64 durationMs(const QString &text) {
    static const QRegularExpression part(QStringLiteral("(\\d+)(ms|d|h|m|s)"));
    qint64 total = 0;
    for (auto it = part.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const qint64 n = m.captured(1).toLongLong();
        const QString unit = m.captured(2);
        total += unit == "ms" ? n : unit == "s" ? n * 1000 : unit == "m" ? n * 60000 : unit == "h" ? n * 3600000 : n * 86400000;
    }
    return total;
}

QString componentPackage(const QString &component) {
    return component.section('/', 0, 0).trimmed();
}

QStringList components(const QString &value) {
    if (value.isEmpty() || value == "null")
        return {};
    QStringList list;
    for (const QString &item : value.split(':', Qt::SkipEmptyParts))
        if (item.contains('/'))
            list.append(item.trimmed());
    return list;
}

// What reading each op means, and how much it matters when the app wasn't
// on screen.
struct Sensitive {
    const char *op;
    const char *what;
    const char *level;
};
const Sensitive kSensitive[] = {
    {"RECORD_AUDIO", QT_TR_NOOP("the microphone"), "alert"},
    {"CAMERA", QT_TR_NOOP("the camera"), "alert"},
    {"FINE_LOCATION", QT_TR_NOOP("precise location"), "warn"},
    {"COARSE_LOCATION", QT_TR_NOOP("approximate location"), "warn"},
    {"READ_CONTACTS", QT_TR_NOOP("contacts"), "warn"},
    {"READ_CALL_LOG", QT_TR_NOOP("call history"), "warn"},
    {"READ_SMS", QT_TR_NOOP("text messages"), "warn"},
    {"READ_CLIPBOARD", QT_TR_NOOP("the clipboard"), "warn"},
    {"READ_CALENDAR", QT_TR_NOOP("the calendar"), "warn"},
    {"READ_MEDIA_IMAGES", QT_TR_NOOP("photos"), "warn"},
    {"READ_MEDIA_VIDEO", QT_TR_NOOP("videos"), "warn"},
    {"BODY_SENSORS", QT_TR_NOOP("body sensors"), "warn"},
    {"ACTIVITY_RECOGNITION", QT_TR_NOOP("physical activity"), "warn"},
};

const Sensitive *sensitive(const QString &op) {
    for (const Sensitive &item : kSensitive)
        if (op == QLatin1String(item.op))
            return &item;
    return nullptr;
}

QString agoText(qint64 seconds) {
    if (seconds < 60)
        return QObject::tr("%n seconds ago", "", int(seconds));
    if (seconds < 3600)
        return QObject::tr("%n minutes ago", "", int(seconds / 60));
    return QObject::tr("%n hours ago", "", int(seconds / 3600));
}

QString stateText(const QString &state) {
    if (state == "cch")
        return QObject::tr("cached: not on screen and not running anything the user can see");
    if (state == "bg")
        return QObject::tr("in the background: not on screen");
    if (state == "fgsvc")
        return QObject::tr("running a foreground service, which shows a notification");
    return state;
}

QString appName(const QString &package) {
    return friendlyName(package);
}

QJsonObject finding(const QString &key, const QString &level, const QString &group, const QString &package,
                    const QString &title, const QString &detail) {
    return {{"key", key}, {"level", level}, {"group", group}, {"package", package}, {"title", title}, {"detail", detail}};
}

int levelRank(const QString &level) {
    return level == "alert" ? 0 : level == "warn" ? 1 : 2;
}

struct OpGrant {
    QString package;
    QString op;
    QString mode;
};

// "Package x:" then "      OP (mode):" or "      OP: mode=allow".
QList<OpGrant> parseOpGrants(const QString &appops) {
    static const QRegularExpression packageLine(QStringLiteral("^\\s+Package\\s+([^:\\s]+):\\s*$"));
    static const QRegularExpression opMode(QStringLiteral("^\\s+([A-Z][A-Z0-9_]+)\\s*(?:\\(([a-z]+)[^)]*\\)|:\\s*mode=([a-z]+))"));
    QList<OpGrant> grants;
    QString package;
    for (const QString &line : appops.split('\n')) {
        QRegularExpressionMatch m = packageLine.match(line);
        if (m.hasMatch()) {
            package = m.captured(1);
            continue;
        }
        if (line.startsWith("  Uid ") || (!line.isEmpty() && !line.at(0).isSpace())) {
            package.clear();
            continue;
        }
        if (package.isEmpty())
            continue;
        m = opMode.match(line);
        if (m.hasMatch())
            grants.append({package, m.captured(1), m.captured(2).isEmpty() ? m.captured(3) : m.captured(2)});
    }
    return grants;
}

} // namespace

qint64 agoSeconds(const QString &text) {
    if (!text.startsWith('-'))
        return -1;
    const qint64 ms = durationMs(text);
    return ms / 1000;
}

QList<OpAccess> parseOpAccesses(const QString &appops) {
    static const QRegularExpression uidLine(QStringLiteral("^\\s*Uid\\s+([^:\\s]+):\\s*$"));
    static const QRegularExpression packageLine(QStringLiteral("^\\s+Package\\s+([^:\\s]+):\\s*$"));
    static const QRegularExpression opLine(QStringLiteral("^\\s+([A-Z][A-Z0-9_]+)\\s*(?:\\([^)]*\\))?:"));
    static const QRegularExpression tagLine(QStringLiteral("^\\s+([^\\s=]+)=\\[\\s*$"));
    static const QRegularExpression access(QStringLiteral(
        "^\\s+(Access|Reject):\\s*\\[([a-z]+)-([a-z]+)\\][^(]*\\((-[0-9dhms]+)\\)(?:\\s+duration=\\+?([0-9dhms]+))?"));
    QList<OpAccess> found;
    qint64 uid = -1;
    QString package, op, attribution;
    for (const QString &line : appops.split('\n')) {
        QRegularExpressionMatch m = uidLine.match(line);
        if (m.hasMatch()) {
            uid = uidOf(m.captured(1));
            package.clear();
            op.clear();
            continue;
        }
        m = packageLine.match(line);
        if (m.hasMatch()) {
            package = m.captured(1);
            op.clear();
            continue;
        }
        m = access.match(line);
        if (m.hasMatch()) {
            if (package.isEmpty() || op.isEmpty())
                continue;
            OpAccess entry;
            entry.package = package;
            entry.uid = uid;
            entry.op = op;
            entry.attribution = attribution == "null" ? QString() : attribution;
            entry.rejected = m.captured(1) == "Reject";
            entry.state = m.captured(2);
            entry.flags = m.captured(3);
            entry.ago = agoSeconds(m.captured(4));
            entry.durationMs = m.captured(5).isEmpty() ? 0 : durationMs(m.captured(5));
            found.append(entry);
            continue;
        }
        m = tagLine.match(line);
        if (m.hasMatch()) {
            attribution = m.captured(1);
            continue;
        }
        m = opLine.match(line);
        if (m.hasMatch()) {
            op = m.captured(1);
            attribution.clear();
        }
    }
    return found;
}

QHash<QString, QString> parseSecurity(const QString &text) {
    QHash<QString, QString> values;
    for (const QString &line : text.split('\n')) {
        const int at = int(line.indexOf('='));
        if (at > 0)
            values.insert(line.left(at).trimmed(), line.mid(at + 1).trimmed());
    }
    return values;
}

QHash<QString, QString> parseInstallers(const QString &text) {
    static const QRegularExpression line(QStringLiteral("^package:(\\S+)\\s+installer=(\\S+)"));
    QHash<QString, QString> installers;
    for (const QString &raw : text.split('\n')) {
        const QRegularExpressionMatch m = line.match(raw.trimmed());
        if (m.hasMatch())
            installers.insert(m.captured(1), m.captured(2));
    }
    return installers;
}

QSet<QString> parseLaunchers(const QString &text) {
    static const QRegularExpression component(QStringLiteral("^\\s*([A-Za-z][\\w.]*)/\\S+\\s*$"));
    QSet<QString> packages;
    for (const QString &raw : text.split('\n')) {
        const QRegularExpressionMatch m = component.match(raw);
        if (m.hasMatch())
            packages.insert(m.captured(1));
    }
    return packages;
}

QJsonArray watchFindings(const Capture &capture, const QDate &today) {
    QList<QJsonObject> out;
    const QHash<QString, QString> security = parseSecurity(capture.text("security"));
    const QHash<QString, QString> installers = parseInstallers(capture.text("installers"));
    const bool knowsThirdParty = !installers.isEmpty();
    auto thirdParty = [&](const QString &package) { return installers.contains(package); };
    auto systemNote = [&](const QString &package) {
        return knowsThirdParty && !thirdParty(package) ? QObject::tr(" It is part of the phone's system software.")
                                                       : QString();
    };

    // ---- Background use: the last day of sensitive access while the app
    // wasn't on screen.
    struct Summary {
        OpAccess latest;
        int times = 0;
        qint64 totalMs = 0;
    };
    QHash<QString, Summary> summaries;
    QStringList order;
    for (const OpAccess &entry : parseOpAccesses(capture.text("appops"))) {
        if (entry.rejected || entry.ago < 0 || entry.ago > 86400 || !sensitive(entry.op))
            continue;
        const bool hidden = entry.state == "bg" || entry.state == "cch";
        const bool service = entry.state == "fgsvc" &&
                             (entry.op == "RECORD_AUDIO" || entry.op == "CAMERA" || entry.op == "FINE_LOCATION");
        if (!hidden && !service)
            continue;
        // One finding per app and kind of data; a foreground service (which
        // shows a notification) is kept apart from use with nothing shown.
        const QString key = QStringList{service ? "fgsvc" : "bg", entry.package, entry.op}.join('|');
        if (!summaries.contains(key))
            order.append(key);
        Summary &summary = summaries[key];
        if (summary.times == 0 || entry.ago < summary.latest.ago)
            summary.latest = entry;
        ++summary.times;
        summary.totalMs += entry.durationMs;
    }
    for (const QString &key : order) {
        const Summary &summary = summaries[key];
        const OpAccess &entry = summary.latest;
        const Sensitive *what = sensitive(entry.op);
        const bool service = entry.state == "fgsvc";
        // Microphone and camera out of sight are alerts, location a warning,
        // for any app. Other data read in the background is routine for the
        // phone's own sync and backup (24 of 33 such findings on a Pixel 8
        // Pro, 2026-11-16), so for system apps it is listed, not flagged.
        const bool system = knowsThirdParty && !thirdParty(entry.package);
        QString level = QString::fromLatin1(what->level);
        if (service)
            level = QStringLiteral("info");
        else if (level == "warn" && system && entry.op != "FINE_LOCATION" && entry.op != "COARSE_LOCATION")
            level = QStringLiteral("info");
        QString detail = QObject::tr("Last %1; %n times in the last day.", "", summary.times).arg(agoText(entry.ago));
        if (summary.totalMs >= 1000)
            detail += QObject::tr(" %1 s in all.").arg(summary.totalMs / 1000);
        detail += QObject::tr(" The app was %1.").arg(stateText(entry.state));
        if (!entry.attribution.isEmpty())
            detail += QObject::tr(" Its own label for the use: %1.").arg(entry.attribution);
        if (entry.flags.contains('p'))
            detail += QObject::tr(" It went through another app on its behalf.");
        detail += systemNote(entry.package);
        QJsonObject item = finding(key, level, "background", entry.package,
                                   QObject::tr("%1 accessed %2 while not on screen")
                                       .arg(appName(entry.package), QObject::tr(what->what)),
                                   detail);
        item.insert("ago", entry.ago);
        out.append(item);
    }

    // ---- Apps that can watch or control the phone.
    for (const QString &component : components(security.value("accessibility"))) {
        const QString package = componentPackage(component);
        out.append(finding("a11y|" + component, thirdParty(package) ? "warn" : "info", "control", package,
                           QObject::tr("%1 can read and control the screen").arg(appName(package)),
                           QObject::tr("An accessibility service is on: it sees everything on screen and can tap and "
                                       "type. Screen readers and password managers use this, and so does stalkerware. "
                                       "If you didn't turn it on, turn it off under Settings > Accessibility.") +
                               systemNote(package)));
    }
    for (const QString &component : components(security.value("listeners"))) {
        const QString package = componentPackage(component);
        out.append(finding("listener|" + component, thirdParty(package) ? "warn" : "info", "control", package,
                           QObject::tr("%1 can read every notification").arg(appName(package)),
                           QObject::tr("Notification access shows the app the text of every notification, messages "
                                       "included. Watches and car apps use it. Turn it off under Settings > "
                                       "Notifications > Device & app notifications if you don't know why it's on.") +
                               systemNote(package)));
    }
    {
        // "Enabled Device Admins (User 0, ...):" then one "pkg/.Cls:" per admin.
        const QString policy = capture.text("device_policy");
        bool admins = false;
        for (const QString &line : policy.split('\n')) {
            if (line.trimmed().startsWith("Enabled Device Admins")) {
                admins = true;
                continue;
            }
            if (!admins)
                continue;
            const int indent = int(line.size() - line.trimmed().size());
            const QString text = line.trimmed();
            if (indent <= 2 && !text.isEmpty()) {
                admins = false;
                continue;
            }
            if (indent == 4 && text.endsWith(':') && text.contains('/')) {
                const QString component = text.chopped(1);
                const QString package = componentPackage(component);
                out.append(finding("admin|" + component, thirdParty(package) ? "warn" : "info", "control", package,
                                   QObject::tr("%1 is a device administrator").arg(appName(package)),
                                   QObject::tr("A device administrator can lock the phone, change the screen lock "
                                               "and sometimes wipe it, and can't be uninstalled until it is turned "
                                               "off under Settings > Security > Device admin apps.") +
                                       systemNote(package)));
            }
        }
        static const QRegularExpression owner(
            QStringLiteral("(Device Owner|Profile Owner[^:]*):[\\s\\S]{0,400}?admin=ComponentInfo\\{([^/}]+)/"));
        for (auto it = owner.globalMatch(policy); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const bool device = m.captured(1) == "Device Owner";
            const QString package = m.captured(2);
            out.append(finding(QString(device ? "owner|" : "profile|") + package, device ? "alert" : "warn", "control",
                               package,
                               device ? QObject::tr("%1 manages this phone (device owner)").arg(appName(package))
                                      : QObject::tr("%1 manages a work profile").arg(appName(package)),
                               device ? QObject::tr("A device owner (company or school management) can see and control "
                                                    "almost everything. Expected on a work phone; on a personal phone, "
                                                    "find out who set it up.")
                                      : QObject::tr("A profile owner controls the apps and data in its profile.")));
        }
    }
    for (const OpGrant &grant : parseOpGrants(capture.text("appops"))) {
        if (grant.mode != "allow" || !thirdParty(grant.package))
            continue;
        QString title, detail, level = "info";
        if (grant.op == "GET_USAGE_STATS") {
            title = QObject::tr("%1 can see which apps you use and when").arg(appName(grant.package));
            detail = QObject::tr("Usage access. Battery and screen-time apps use it.");
            level = "warn";
        } else if (grant.op == "REQUEST_INSTALL_PACKAGES") {
            title = QObject::tr("%1 can install other apps").arg(appName(grant.package));
            detail = QObject::tr("Allowed to install unknown apps. Browsers and app stores ask for it.");
            level = "warn";
        } else if (grant.op == "SYSTEM_ALERT_WINDOW") {
            title = QObject::tr("%1 can draw over other apps").arg(appName(grant.package));
            detail = QObject::tr("Chat heads and screen filters use it; it can also cover what another app shows.");
        } else if (grant.op == "MANAGE_EXTERNAL_STORAGE") {
            title = QObject::tr("%1 can read all files").arg(appName(grant.package));
            detail = QObject::tr("All-files access. File managers and backup apps use it.");
        } else {
            continue;
        }
        out.append(finding("grant|" + grant.package + "|" + grant.op, level, "control", grant.package, title, detail));
    }
    {
        // Default apps: who handles texts, calls, the assistant, the home
        // screen and the browser. Listed so a change shows up as new.
        static const QHash<QString, QString> roles = {
            {"android.app.role.SMS", QT_TR_NOOP("texts")},
            {"android.app.role.DIALER", QT_TR_NOOP("phone calls")},
            {"android.app.role.ASSISTANT", QT_TR_NOOP("the assistant")},
            {"android.app.role.HOME", QT_TR_NOOP("the home screen")},
            {"android.app.role.BROWSER", QT_TR_NOOP("web links")},
            {"android.app.role.CALL_SCREENING", QT_TR_NOOP("call screening")},
        };
        QString role;
        for (const QString &raw : capture.text("roles").split('\n')) {
            const QString line = raw.trimmed();
            if (line.startsWith("name="))
                role = line.mid(5);
            else if (line.startsWith("holders=") && roles.contains(role)) {
                for (const QString &holder : line.mid(8).split(',', Qt::SkipEmptyParts)) {
                    const QString package = holder.trimmed();
                    out.append(finding("role|" + role + "|" + package, "info", "control", package,
                                       QObject::tr("%1 is the default app for %2")
                                           .arg(appName(package), QObject::tr(roles.value(role).toUtf8().constData())),
                                       QObject::tr("Listed so that a change of default app shows up as new.")));
                }
            }
        }
    }

    // ---- Network: where traffic can be redirected or read.
    const QString proxy = security.value("proxy");
    if (!proxy.isEmpty() && proxy != "null" && proxy != ":0")
        out.append(finding("proxy|" + proxy, "alert", "network", QString(),
                           QObject::tr("Web traffic goes through a proxy: %1").arg(proxy),
                           QObject::tr("A global HTTP proxy sees every unencrypted request and the names of encrypted "
                                       "ones. Set by hand, by a work profile, or by something that wants to watch.")));
    {
        static const QRegularExpression active(QStringLiteral("Active package name:\\s*(\\S[^\\n]*)"));
        const QString vpnText = capture.text("vpn");
        for (auto it = active.globalMatch(vpnText); it.hasNext();) {
            const QString package = it.next().captured(1).trimmed();
            if (package.isEmpty() || package.startsWith('['))
                continue;
            out.append(finding("vpn|" + package, "warn", "network", package,
                               QObject::tr("%1 is the active VPN").arg(appName(package)),
                               QObject::tr("A VPN app carries every connection the phone makes. Expected if you chose "
                                           "one (a privacy VPN, a firewall, an ad blocker).")));
        }
        static const QRegularExpression lockdown(QStringLiteral("lockdown=(true|false) alwaysOn=(true|false)"));
        const QRegularExpressionMatch m = lockdown.match(vpnText);
        if (m.hasMatch() && m.captured(2) == "true")
            out.append(finding("vpn-always-on", "info", "network", QString(), QObject::tr("A VPN is set to always on"),
                               m.captured(1) == "true" ? QObject::tr("With lockdown: no traffic leaves without it.")
                                                       : QObject::tr("Without lockdown.")));
    }
    {
        const QString mode = security.value("private_dns");
        if (mode == "hostname")
            out.append(finding("dns|" + security.value("private_dns_host"), "info", "network", QString(),
                               QObject::tr("Private DNS: %1").arg(security.value("private_dns_host")),
                               QObject::tr("Every name the phone looks up goes to this server, encrypted.")));
        else if (mode == "off")
            out.append(finding("dns|off", "info", "network", QString(), QObject::tr("Private DNS is off"),
                               QObject::tr("Name lookups go to the network's DNS server unencrypted.")));
    }

    // ---- Apps from outside an app store, and apps without an icon.
    static const QHash<QString, QString> stores = {
        {"com.android.vending", QString()},
        {"org.fdroid.fdroid", "F-Droid"},
        {"com.aurora.store", "Aurora Store"},
        {"com.amazon.venezia", "Amazon Appstore"},
        {"com.sec.android.app.samsungapps", "Galaxy Store"},
        {"com.huawei.appmarket", "AppGallery"},
    };
    const QSet<QString> launchers = parseLaunchers(capture.text("launchers"));
    QStringList thirdPartyNames = installers.keys();
    thirdPartyNames.sort();
    for (const QString &package : thirdPartyNames) {
        const QString installer = installers.value(package);
        if (stores.contains(installer)) {
            if (!stores.value(installer).isEmpty())
                out.append(finding("store|" + package + "|" + installer, "info", "outside", package,
                                   QObject::tr("%1 came from %2").arg(appName(package), stores.value(installer)),
                                   QObject::tr("Installed from an app store other than Google Play.")));
        } else {
            const QString how = installer == "com.google.android.packageinstaller" ||
                                        installer == "com.android.packageinstaller"
                                    ? QObject::tr("from an APK file opened on the phone")
                                : installer == "com.android.shell" ? QObject::tr("over adb from a computer")
                                : installer == "null"              ? QObject::tr("by an installer that didn't say")
                                                                   : QObject::tr("by %1").arg(installer);
            out.append(finding("installed|" + package + "|" + installer,
                               installer == "com.android.shell" ? "info" : "warn", "outside", package,
                               QObject::tr("%1 was installed %2").arg(appName(package), how),
                               QObject::tr("Apps from outside an app store skip its checks. Fine if you installed it "
                                           "yourself.")));
        }
        if (!launchers.isEmpty() && !launchers.contains(package))
            out.append(finding("noicon|" + package, "info", "outside", package,
                               QObject::tr("%1 has no icon in the app list").arg(appName(package)),
                               QObject::tr("Plug-ins, keyboards and companion services often have none; an app that "
                                           "hides itself is worth a look.")));
    }

    // ---- The phone itself.
    {
        const QDate patch = QDate::fromString(security.value("patch"), "yyyy-MM-dd");
        if (patch.isValid()) {
            const qint64 days = patch.daysTo(today);
            out.append(finding("patch", days > 90 ? "warn" : "info", "device", QString(),
                               QObject::tr("Security patch %1, %n days old", "", int(days)).arg(security.value("patch")),
                               days > 90 ? QObject::tr("More than three months without security fixes. Check for a system "
                                                       "update under Settings > System.")
                                         : QObject::tr("Recent enough.")));
        }
        const QString boot = security.value("verified_boot");
        if (!boot.isEmpty() && boot != "green")
            out.append(finding("boot|" + boot, "alert", "device", QString(),
                               QObject::tr("Verified boot is %1, not green").arg(boot),
                               QObject::tr("The phone can't vouch for its system software: the bootloader is unlocked "
                                           "or the system was changed. Expected only if you did that yourself.")));
        if (security.value("flash_locked") == "0")
            out.append(finding("unlocked", "alert", "device", QString(), QObject::tr("The bootloader is unlocked"),
                               QObject::tr("Anyone with the phone in hand can install other system software.")));
        if (security.value("adb_wifi") == "1")
            out.append(finding("adb-wifi", "info", "device", QString(), QObject::tr("Wireless debugging is on"),
                               QObject::tr("Paired computers on this network can control the phone over adb. Turn it "
                                           "off under Developer options when you're done.")));
    }

    std::stable_sort(out.begin(), out.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const int ra = levelRank(a.value("level").toString()), rb = levelRank(b.value("level").toString());
        if (ra != rb)
            return ra < rb;
        return a.value("ago").toDouble(1e12) < b.value("ago").toDouble(1e12);
    });
    QJsonArray array;
    for (const QJsonObject &item : out)
        array.append(item);
    return array;
}

WatchBaseline::WatchBaseline(const QString &directory)
    : m_directory(directory.isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/snoopy/watch"
                      : directory) {}

QString WatchBaseline::path(const QString &phone) const {
    QString safe = phone;
    safe.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    return m_directory + "/" + safe + ".json";
}

QJsonObject WatchBaseline::load(const QString &phone) const {
    QFile file(path(phone));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void WatchBaseline::save(const QString &phone, const QJsonObject &state) const {
    QDir().mkpath(m_directory);
    QSaveFile file(path(phone));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

QJsonArray WatchBaseline::apply(const QString &phone, const QJsonArray &findings, const QDateTime &now, bool write) {
    QJsonObject state = load(phone);
    const QString stamp = now.toUTC().toString(Qt::ISODate);
    if (state.isEmpty() && !write)
        return findings;
    if (state.isEmpty()) {
        // The first look is the baseline: nothing in it is new, whatever the
        // findings passed in say.
        QJsonArray known, plain;
        for (const QJsonValue &value : findings) {
            QJsonObject item = value.toObject();
            item.remove("new");
            item.remove("since");
            known.append(item.value("key"));
            plain.append(item);
        }
        save(phone, {{"firstLook", stamp}, {"known", known}, {"pending", QJsonObject()}});
        return plain;
    }
    QSet<QString> known;
    for (const QJsonValue &key : state.value("known").toArray())
        known.insert(key.toString());
    QJsonObject pending = state.value("pending").toObject();
    bool changed = false;
    QJsonArray marked;
    for (const QJsonValue &value : findings) {
        QJsonObject item = value.toObject();
        item.remove("new");
        item.remove("since");
        const QString key = item.value("key").toString();
        if (!known.contains(key)) {
            if (!pending.contains(key)) {
                pending.insert(key, stamp);
                changed = true;
            }
            item.insert("new", true);
            item.insert("since", pending.value(key));
        }
        marked.append(item);
    }
    if (changed && write) {
        state.insert("pending", pending);
        save(phone, state);
    }
    return marked;
}

void WatchBaseline::acknowledge(const QString &phone) {
    QJsonObject state = load(phone);
    if (state.isEmpty())
        return;
    QJsonArray known = state.value("known").toArray();
    const QJsonObject pending = state.value("pending").toObject();
    for (auto it = pending.begin(); it != pending.end(); ++it)
        known.append(it.key());
    state.insert("known", known);
    state.insert("pending", QJsonObject());
    save(phone, state);
}
