#include "render.h"

#include "catalog.h"
#include "parsers.h"

#include <QJsonArray>

#include <algorithm>

// render.py, function for function.

namespace {

QString field(const QJsonObject &item, const char *key) {
    return py::str(item.value(QLatin1String(key)));
}

QStringList strings(const QJsonValue &value) {
    QStringList out;
    for (const QJsonValue &item : value.toArray())
        out.append(py::str(item));
    return out;
}

std::optional<QString> mode(const QJsonObject &item) {
    const QJsonValue value = item.value("mode");
    if (!py::truthy(value))
        return std::nullopt;
    return py::str(value);
}

QString formatBytes(qint64 size) {
    if (size < 1024)
        return QString::number(size) + QStringLiteral(" B");
    double value = double(size);
    for (const char *unit : {"KB", "MB", "GB", "TB"}) {
        value /= 1024;
        if (value < 1024 || QLatin1String(unit) == QLatin1String("TB"))
            return QString::asprintf("%.1f %s", value, unit);
    }
    return QString::number(size) + QStringLiteral(" B");
}

QString dnsText(const QJsonObject &item) {
    if (py::truthy(item.value("tracker")))
        return field(item, "host") + QStringLiteral(" — tracker ") + field(item, "tracker");
    return field(item, "host");
}

QString opText(const QJsonObject &item) {
    QStringList parts = {field(item, "category") + QStringLiteral(" — ") + field(item, "op") + QStringLiteral(", ") +
                         catalog::modePhrase(mode(item))};
    if (py::truthy(item.value("last_access")))
        parts.append(QStringLiteral("last access ") + field(item, "last_access"));
    if (py::truthy(item.value("last_reject")))
        parts.append(QStringLiteral("last reject ") + field(item, "last_reject"));
    QString text = parts.join(QStringLiteral(", "));
    if (py::truthy(item.value("detail")))
        text = text + QStringLiteral(". ") + field(item, "detail");
    return text;
}

QStringList renderTransmitted(const QJsonObject &card) {
    const QJsonObject transmitted = card.value("transmitted").toObject();
    QStringList lines = {QStringLiteral("Transmitted:")};
    QStringList owners = strings(card.value("uid_packages"));
    if (owners.isEmpty())
        owners = strings(card.value("packages"));
    if (owners.size() > 1)
        lines.append(QStringLiteral("  Sockets and byte counts belong to uid ") + field(card, "uid") + QStringLiteral(" (") +
                     owners.join(QStringLiteral(", ")) + QStringLiteral(")."));
    lines.append(QStringLiteral("  Sent ") + formatBytes(transmitted.value("tx_bytes").toInteger()) + QStringLiteral(", received ") +
                 formatBytes(transmitted.value("rx_bytes").toInteger()) + u'.');
    if (py::truthy(transmitted.value("network_permission")))
        lines.append(QStringLiteral("  Network permission: granted."));
    const QJsonArray connections = transmitted.value("connections").toArray();
    if (!connections.isEmpty()) {
        const qsizetype shown = std::min<qsizetype>(connections.size(), 40);
        for (qsizetype i = 0; i < shown; ++i) {
            const QJsonObject item = connections[i].toObject();
            lines.append(QStringLiteral("  - ") + field(item, "proto") + u' ' + field(item, "state") + u' ' + field(item, "local") +
                         QStringLiteral(" -> ") + field(item, "remote"));
        }
        const qsizetype extra = connections.size() - shown;
        if (extra)
            lines.append(QStringLiteral("  - ") + QString::number(extra) + QStringLiteral(" more sockets"));
    } else {
        lines.append(QStringLiteral("  - No active socket with a remote address."));
    }
    const QJsonArray dns = transmitted.value("dns").toArray();
    for (qsizetype i = 0; i < dns.size() && i < 40; ++i)
        lines.append(QStringLiteral("  - DNS ") + dnsText(dns[i].toObject()));
    const qsizetype extraDns = dns.size() - 40;
    if (extraDns > 0)
        lines.append(QStringLiteral("  - ") + QString::number(extraDns) + QStringLiteral(" more DNS names"));
    return lines;
}

QStringList renderCard(const QJsonObject &card) {
    const QStringList packages = strings(card.value("packages"));
    const QString names = packages.isEmpty() ? QStringLiteral("(no package mapped to this uid)") : packages.join(QStringLiteral(", "));
    QStringList lines = {QString(), QStringLiteral("### uid ") + field(card, "uid") + QStringLiteral("  ") + names};

    const QJsonObject versionMap = card.value("versions").toObject();
    QStringList versionKeys = versionMap.keys();
    std::stable_sort(versionKeys.begin(), versionKeys.end(), py::less);
    QStringList versions;
    for (const QString &package : versionKeys)
        versions.append(package + u' ' + py::str(versionMap.value(package)));
    if (!versions.isEmpty())
        lines.append(QStringLiteral("Version: ") + versions.join(QStringLiteral(", ")));

    QStringList shared;
    for (const QString &name : strings(card.value("uid_packages")))
        if (!packages.contains(name))
            shared.append(name);
    if (!shared.isEmpty())
        lines.append(QStringLiteral("This uid also contains: ") + shared.join(QStringLiteral(", ")));

    lines.append(QStringLiteral("Doing:"));
    const QStringList doing = strings(card.value("doing"));
    if (!doing.isEmpty()) {
        for (const QString &item : doing)
            lines.append(QStringLiteral("  - ") + item);
    } else {
        lines.append(QStringLiteral("  - Nothing running, scheduled, or holding a sensor, location request, or the clipboard."));
    }

    lines.append(QStringLiteral("Information tracked:"));
    bool wrote = false;
    const QJsonArray observed = card.value("observed").toArray();
    if (!observed.isEmpty()) {
        wrote = true;
        lines.append(QStringLiteral("  Observed by the OS:"));
        for (const QJsonValue &item : observed)
            lines.append(QStringLiteral("    - ") + opText(item.toObject()));
    }
    const QJsonArray attempted = card.value("attempted").toArray();
    if (!attempted.isEmpty()) {
        wrote = true;
        lines.append(QStringLiteral("  Attempted and denied:"));
        for (const QJsonValue &item : attempted)
            lines.append(QStringLiteral("    - ") + opText(item.toObject()));
    }
    const QJsonArray permitted = card.value("permitted_unseen").toArray();
    if (!permitted.isEmpty()) {
        wrote = true;
        lines.append(QStringLiteral("  Permitted, with no access time recorded:"));
        for (const QJsonValue &value : permitted) {
            const QJsonObject item = value.toObject();
            lines.append(QStringLiteral("    - ") + field(item, "category") + QStringLiteral(" — ") + field(item, "op") +
                         QStringLiteral(", ") + catalog::modePhrase(mode(item)));
        }
    }
    const QJsonArray declared = card.value("declared").toArray();
    if (!declared.isEmpty()) {
        wrote = true;
        lines.append(QStringLiteral("  Granted permissions:"));
        for (const QJsonValue &value : declared) {
            const QJsonObject item = value.toObject();
            lines.append(QStringLiteral("    - ") + field(item, "category") + QStringLiteral(" — ") + field(item, "permission") +
                         QStringLiteral(". ") + field(item, "detail"));
        }
    }
    const QJsonArray trackers = card.value("trackers").toArray();
    if (!trackers.isEmpty()) {
        wrote = true;
        lines.append(QStringLiteral("  Tracker components shipped in the app:"));
        for (const QJsonValue &value : trackers) {
            const QJsonObject item = value.toObject();
            lines.append(QStringLiteral("    - ") + field(item, "name") + QStringLiteral(" (") + field(item, "prefix") +
                         QStringLiteral("). ") + field(item, "collects"));
        }
    }
    if (!wrote)
        lines.append(QStringLiteral("  - No sensitive grant, app-op, or tracker component in this snapshot."));
    lines += renderTransmitted(card);
    return lines;
}

} // namespace

QString renderText(const QJsonObject &report) {
    QStringList lines = {QStringLiteral("Android diagnoser")};
    const QJsonObject device = report.value("device").toObject();
    QStringList bits;
    if (py::truthy(device.value("model")))
        bits.append(field(device, "model"));
    if (py::truthy(device.value("android"))) {
        const QString sdk = py::truthy(device.value("sdk")) ? QStringLiteral(" (SDK ") + field(device, "sdk") + u')' : QString();
        bits.append(QStringLiteral("Android ") + field(device, "android") + sdk);
    }
    if (py::truthy(device.value("serial")))
        bits.append(QStringLiteral("serial ") + field(device, "serial"));
    if (!bits.isEmpty())
        lines.append(QStringLiteral("Device: ") + bits.join(QStringLiteral(", ")));
    if (py::truthy(device.value("captured_at")))
        lines.append(QStringLiteral("Captured: ") + field(device, "captured_at"));
    lines.append(QStringLiteral("Sources: ") + strings(report.value("sources")).join(QStringLiteral(", ")));
    lines.append(QString());
    lines += strings(report.value("limits"));
    lines.append(QString());
    lines.append(QStringLiteral("## What each app is doing, tracking, and transmitting"));
    const QJsonArray live = report.value("live").toArray();
    if (live.isEmpty())
        lines.append(QStringLiteral(
            "No app showed live activity, sensitive access, tracker components, or network traffic in this snapshot."));
    for (const QJsonValue &card : live)
        lines += renderCard(card.toObject());
    lines.append(QString());
    lines.append(QStringLiteral("## Declared sensitive access with no live evidence in this snapshot"));
    const QJsonArray declared = report.value("declared_only").toArray();
    if (!declared.isEmpty()) {
        for (const QJsonValue &value : declared) {
            const QJsonObject item = value.toObject();
            lines.append(QStringLiteral("- ") + field(item, "package") + QStringLiteral(" (uid ") + field(item, "uid") +
                         QStringLiteral("): ") + strings(item.value("categories")).join(QStringLiteral(", ")));
        }
    } else {
        lines.append(QStringLiteral("None."));
    }
    const QJsonArray unattributed = report.value("unattributed_dns").toArray();
    if (!unattributed.isEmpty()) {
        lines.append(QString());
        lines.append(QStringLiteral("## DNS names not tied to an app uid"));
        for (const QJsonValue &item : unattributed)
            lines.append(QStringLiteral("- ") + dnsText(item.toObject()));
    }
    const QJsonObject errors = report.value("errors").toObject();
    if (!errors.isEmpty()) {
        lines.append(QString());
        lines.append(QStringLiteral("## Capture gaps"));
        // A QJsonObject keeps its keys sorted; Python keeps meta.json's order.
        for (auto it = errors.begin(); it != errors.end(); ++it)
            lines.append(QStringLiteral("- ") + it.key() + QStringLiteral(": ") + py::str(it.value()));
    }
    lines.append(QString());
    return lines.join(u'\n');
}
