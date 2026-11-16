#include "flows.h"

#include "parsers.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

// flows.py, function for function.

namespace {

QString s(const QJsonObject &item, const char *key) {
    return item.value(QLatin1String(key)).toString();
}

// item.get(a) or item.get(b) or ... or fallback, for string fields.
QString either(const QJsonObject &item, std::initializer_list<const char *> keys, const QString &fallback = QString()) {
    for (const char *key : keys) {
        const QJsonValue value = item.value(QLatin1String(key));
        if (py::truthy(value))
            return py::str(value);
    }
    return fallback;
}

qint64 intOf(const QJsonValue &value, qint64 fallback) {
    if (!py::truthy(value))
        return fallback;
    return value.toInteger(qint64(value.toDouble()));
}

bool framework(const QString &name) {
    return name == QLatin1String("android") || name.startsWith(QLatin1String("android.")) ||
           name.startsWith(QLatin1String("com.android.")) || name.startsWith(QLatin1String("com.google.android."));
}

QString appName(const QJsonObject &card) {
    QStringList packages;
    for (const QJsonValue &value : card.value("packages").toArray())
        if (py::truthy(value))
            packages.append(value.toString());
    if (packages.isEmpty())
        return QStringLiteral("uid ") + py::str(card.value("uid"));
    if (packages.size() == 1)
        return packages.first();
    QStringList notable;
    for (const QString &name : packages)
        if (!framework(name))
            notable.append(name);
    if (notable.size() == 1)
        return notable.first();
    if (notable.size() > 1 && notable.size() <= 3)
        return notable.join(QStringLiteral(", "));
    if (!notable.isEmpty())
        return notable.first() + QStringLiteral(" and ") + QString::number(notable.size() - 1) + QStringLiteral(" other packages");
    return QStringLiteral("Android system (") + QString::number(packages.size()) + QStringLiteral(" packages)");
}

QSet<QString> coveredCategories(const QJsonObject &card) {
    QSet<QString> covered;
    for (const char *bucket : {"observed", "attempted"})
        for (const QJsonValue &value : card.value(QLatin1String(bucket)).toArray()) {
            const QJsonValue category = value.toObject().value("category");
            if (py::truthy(category))
                covered.insert(category.toString());
        }
    return covered;
}

QJsonObject collection(const QJsonObject &item, const QString &app, const QString &via) {
    QStringList parts;
    if (py::truthy(item.value("last_access")))
        parts.append(QStringLiteral("last access ") + py::str(item.value("last_access")));
    if (py::truthy(item.value("last_reject")))
        parts.append(QStringLiteral("last reject ") + py::str(item.value("last_reject")));
    if (py::truthy(item.value("detail")))
        parts.append(py::str(item.value("detail")));
    return {
        {"kind", "collection"},
        {"source", either(item, {"category", "op"}, QStringLiteral("Sensitive data"))},
        {"information", either(item, {"detail", "op"})},
        {"destination", either(item, {"package"}, app)},
        {"via", via},
        {"detail", parts.join(QStringLiteral(". "))},
    };
}

QJsonObject declared(const QString &category, const QString &app, const QString &package) {
    return {
        {"kind", "declared"},
        {"source", category},
        {"information", "Granted, with no use in this sample"},
        {"destination", package.isEmpty() ? app : package},
        {"via", "granted"},
        {"detail", ""},
    };
}

QJsonObject dns(const QString &source, const QJsonObject &query) {
    const QString tracker = either(query, {"tracker"});
    return {
        {"kind", "transmission"},
        {"source", source},
        {"information", "DNS lookup"},
        {"destination", either(query, {"host"})},
        {"via", tracker},
        {"detail", tracker.isEmpty() ? QString() : QStringLiteral("Known tracker: ") + tracker},
    };
}

QString prettyEndpoint(const QString &value) {
    static const QRegularExpression mappedV4 =
        py::re(QStringLiteral(R"re(^\[::ffff:(\d+\.\d+\.\d+\.\d+)\](?::(\d+))?$)re"), true);
    const QRegularExpressionMatch m = py::match(mappedV4, value);
    if (!m.hasMatch())
        return value;
    const QString port = m.captured(2);
    return port.isEmpty() ? m.captured(1) : m.captured(1) + u':' + port;
}

QString port(const QString &endpoint) {
    if (endpoint.startsWith(u'[') && endpoint.contains(QLatin1String("]:")))
        return endpoint.mid(endpoint.lastIndexOf(QLatin1String("]:")) + 2);
    if (endpoint.count(u':') == 1)
        return endpoint.mid(endpoint.lastIndexOf(u':') + 1);
    return QString();
}

std::optional<QString> sensorName(const QString &line) {
    if (!line.startsWith(QLatin1String("Sensor ")))
        return std::nullopt;
    QString rest = line.mid(7);
    const qsizetype cut = rest.indexOf(QLatin1String(" (pid"));
    if (cut >= 0)
        rest.truncate(cut);
    rest = py::strip(rest);
    if (rest.isEmpty())
        return std::nullopt;
    return rest;
}

QList<QJsonObject> dedupeTransmissions(const QList<QJsonObject> &flows) {
    QList<QJsonObject> merged;
    QHash<QString, qsizetype> seen;
    for (QJsonObject flow : flows) {
        if (flow.value("kind").toString() != QLatin1String("transmission")) {
            merged.append(flow);
            continue;
        }
        const QString key = py::str(flow.value("source")) + QChar(0) + py::str(flow.value("destination")) + QChar(0) +
                            py::str(flow.value("information"));
        auto existing = seen.constFind(key);
        if (existing == seen.cend()) {
            flow.insert("count", intOf(flow.value("count"), 1));
            seen.insert(key, merged.size());
            merged.append(flow);
            continue;
        }
        QJsonObject &old = merged[*existing];
        old.insert("count", intOf(old.value("count"), 1) + intOf(flow.value("count"), 1));
    }
    return merged;
}

} // namespace

QJsonObject projectReport(const QJsonObject &report) {
    QList<QJsonObject> flows;
    QJsonArray apps;
    for (const QJsonValue &cardValue : report.value("live").toArray()) {
        const QJsonObject card = cardValue.toObject();
        const QString app = appName(card);
        QSet<QString> covered = coveredCategories(card);
        for (const QJsonValue &item : card.value("observed").toArray())
            flows.append(collection(item.toObject(), app, QStringLiteral("read")));
        for (const QJsonValue &item : card.value("attempted").toArray())
            flows.append(collection(item.toObject(), app, QStringLiteral("denied")));
        for (const QJsonValue &value : card.value("permitted_unseen").toArray()) {
            const QJsonObject item = value.toObject();
            const QString category = s(item, "category");
            if (covered.contains(category))
                continue;
            flows.append(declared(either(item, {"category", "op"}, QStringLiteral("Permission")), app, either(item, {"package"}, app)));
            covered.insert(category);
        }
        for (const QJsonValue &value : card.value("declared").toArray()) {
            const QJsonObject item = value.toObject();
            const QString category = s(item, "category");
            if (covered.contains(category))
                continue;
            flows.append(declared(category, app, either(item, {"package"}, app)));
            covered.insert(category);
        }
        for (const QJsonValue &value : card.value("doing").toArray()) {
            const QString line = value.toString();
            if (const std::optional<QString> sensor = sensorName(line)) {
                flows.append({
                    {"kind", "collection"},
                    {"source", *sensor},
                    {"information", "Sensor reading"},
                    {"destination", app},
                    {"via", "read"},
                    {"detail", line},
                });
            }
        }
        for (const QJsonValue &value : card.value("trackers").toArray()) {
            const QJsonObject tracker = value.toObject();
            flows.append({
                {"kind", "tracker"},
                {"source", either(tracker, {"package"}, app)},
                {"information", either(tracker, {"collects"}, QStringLiteral("Tracker component shipped in the app"))},
                {"destination", either(tracker, {"name", "prefix"}, QStringLiteral("Tracker"))},
                {"via", "SDK in the app"},
                {"detail", either(tracker, {"prefix"})},
            });
        }
        const QJsonObject transmitted = card.value("transmitted").toObject();
        for (const QJsonValue &value : transmitted.value("connections").toArray()) {
            const QJsonObject connection = value.toObject();
            const QString remote = prettyEndpoint(either(connection, {"remote"}));
            const QString proto = either(connection, {"proto"}, QStringLiteral("TCP"));
            const QString state = either(connection, {"state"});
            QString information = py::strip(proto + u' ' + state);
            if (proto == QLatin1String("UDP") && port(remote) == QLatin1String("443"))
                information = QStringLiteral("QUIC");
            flows.append({
                {"kind", "transmission"},
                {"source", app},
                {"information", information},
                {"destination", remote},
                {"via", prettyEndpoint(either(connection, {"local"}))},
                {"detail", ""},
                {"count", 1},
            });
        }
        for (const QJsonValue &query : transmitted.value("dns").toArray())
            flows.append(dns(app, query.toObject()));
        QJsonArray packages;
        for (const QJsonValue &name : card.value("packages").toArray())
            packages.append(name);
        apps.append(QJsonObject{
            {"name", app},
            {"packages", packages},
            {"tx_bytes", intOf(transmitted.value("tx_bytes"), 0)},
            {"rx_bytes", intOf(transmitted.value("rx_bytes"), 0)},
            {"doing", card.value("doing").toArray()},
        });
    }
    for (const QJsonValue &value : report.value("declared_only").toArray()) {
        const QJsonObject item = value.toObject();
        const QString package = either(item, {"package"}, QStringLiteral("uid ") + py::str(item.value("uid")));
        for (const QJsonValue &category : item.value("categories").toArray())
            flows.append(declared(category.toString(), package, package));
    }
    for (const QJsonValue &query : report.value("unattributed_dns").toArray())
        flows.append(dns(QStringLiteral("Unknown app"), query.toObject()));

    flows = dedupeTransmissions(flows);
    std::stable_sort(flows.begin(), flows.end(), [](const QJsonObject &a, const QJsonObject &b) {
        for (const char *key : {"kind", "source", "destination", "information"}) {
            const int order = py::compare(s(a, key), s(b, key));
            if (order != 0)
                return order < 0;
        }
        return false;
    });
    QJsonArray flowArray;
    for (const QJsonObject &flow : flows)
        flowArray.append(flow);
    return {{"flows", flowArray}, {"apps", apps}};
}
