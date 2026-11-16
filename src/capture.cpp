#include "capture.h"

#include "adb.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace {

struct Command {
    const char *name;
    const char *command;
};

// The same dumps, in the same order, as android-diagnoser's collect.py.
const Command kSnapshot[] = {
    {"packages", "pm list packages -U -f"},
    {"appops", "dumpsys appops"},
    {"package_dump", "dumpsys package"},
    {"activities", "dumpsys activity activities"},
    {"services", "dumpsys activity services"},
    {"processes", "dumpsys activity processes"},
    {"jobs", "dumpsys jobscheduler"},
    {"power", "dumpsys power"},
    {"netstats", "dumpsys netstats"},
    {"location", "dumpsys location"},
    {"sensors", "dumpsys sensorservice"},
    {"clipboard", "dumpsys clipboard"},
    {"tcp", "cat /proc/net/tcp"},
    {"tcp6", "cat /proc/net/tcp6"},
    {"udp", "cat /proc/net/udp"},
    {"udp6", "cat /proc/net/udp6"},
    {"dns", "logcat -d -t 3000 | grep -Ei 'getaddrinfo|DnsResolver|libnetd_resolv|qname=|host='"},
    // For the Watch page (not in android-diagnoser): settings and build
    // properties as key=value lines, third-party packages with their
    // installer, apps with a launcher icon, device admins and owners,
    // default-app roles, and VPN state.
    {"security", "echo accessibility=$(settings get secure enabled_accessibility_services); "
                 "echo listeners=$(settings get secure enabled_notification_listeners); "
                 "echo proxy=$(settings get global http_proxy); "
                 "echo private_dns=$(settings get global private_dns_mode); "
                 "echo private_dns_host=$(settings get global private_dns_specifier); "
                 "echo adb_wifi=$(settings get global adb_wifi_enabled); "
                 "echo patch=$(getprop ro.build.version.security_patch); "
                 "echo verified_boot=$(getprop ro.boot.verifiedbootstate); "
                 "echo flash_locked=$(getprop ro.boot.flash.locked); "
                 "echo hardware_serial=$(getprop ro.serialno)"},
    {"installers", "pm list packages -i -U -3"},
    {"launchers", "cmd package query-activities --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER"},
    {"device_policy", "dumpsys device_policy"},
    {"roles", "dumpsys role"},
    {"vpn", "dumpsys vpn_management"},
};

const Command kWatch[] = {
    {"tcp", "cat /proc/net/tcp"},
    {"tcp6", "cat /proc/net/tcp6"},
    {"udp", "cat /proc/net/udp"},
    {"udp6", "cat /proc/net/udp6"},
    {"activities", "dumpsys activity activities"},
    {"appops", "dumpsys appops"},
    {"packages", "pm list packages -U"},
};

template <size_t N>
Capture runCommands(Adb &adb, const Command (&commands)[N]) {
    Capture capture;
    for (const QString &field : Capture::fields)
        capture.texts.insert(field, QString());
    QJsonObject errors;
    for (const Command &command : commands) {
        const Adb::Shell result = adb.shell(QString::fromLatin1(command.command));
        capture.texts.insert(command.name, result.out);
        if (result.code != 0 && result.out.trimmed().isEmpty()) {
            // grep exits 1 when the log holds no lookups; that is not a failure.
            if (QLatin1String(command.name) == QLatin1String("dns") && result.err.trimmed().isEmpty())
                continue;
            const QString message = result.err.trimmed().isEmpty()
                                        ? QString("command exited %1").arg(result.code)
                                        : result.err.trimmed();
            errors.insert(command.name, message.left(500));
        }
    }
    capture.meta.insert("errors", errors);
    capture.meta.insert("captured_at", utcNow());
    return capture;
}

} // namespace

const QStringList Capture::fields = [] {
    QStringList names;
    for (const Command &command : kSnapshot)
        names.append(QString::fromLatin1(command.name));
    return names;
}();

QString utcNow() {
    return QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ssZ");
}

Capture collectSnapshot(Adb &adb) {
    const QString serial = adb.resolveSerial();
    Capture capture = runCommands(adb, kSnapshot);
    const Adb::Shell props = adb.shell("getprop ro.product.model; getprop ro.build.version.release; getprop ro.build.version.sdk");
    const QStringList lines = props.out.split('\n');
    capture.meta.insert("serial", serial);
    capture.meta.insert("model", lines.value(0).trimmed());
    capture.meta.insert("android", lines.value(1).trimmed());
    capture.meta.insert("sdk", lines.value(2).trimmed());
    return capture;
}

Capture collectWatch(Adb &adb) {
    adb.resolveSerial();
    return runCommands(adb, kWatch);
}

bool saveCapture(const Capture &capture, const QString &directory, QString *error) {
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QObject::tr("Could not create %1").arg(directory);
        return false;
    }
    auto write = [&](const QString &name, const QByteArray &bytes) {
        QSaveFile file(QDir(directory).filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            if (error)
                *error = QObject::tr("Could not write %1").arg(file.fileName());
            return false;
        }
        return true;
    };
    for (const QString &field : Capture::fields)
        if (!write(field + ".txt", capture.text(field).toUtf8()))
            return false;
    return write("meta.json", QJsonDocument(capture.meta).toJson(QJsonDocument::Indented));
}

bool loadCapture(const QString &directory, Capture *capture, QString *error) {
    if (!QDir(directory).exists()) {
        if (error)
            *error = QObject::tr("Capture directory does not exist: %1").arg(directory);
        return false;
    }
    Capture loaded;
    for (const QString &field : Capture::fields) {
        QFile file(QDir(directory).filePath(field + ".txt"));
        loaded.texts.insert(field, file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString());
    }
    QFile meta(QDir(directory).filePath("meta.json"));
    if (meta.open(QIODevice::ReadOnly))
        loaded.meta = QJsonDocument::fromJson(meta.readAll()).object();
    *capture = loaded;
    return true;
}
