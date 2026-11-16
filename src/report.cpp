#include "report.h"

#include "catalog.h"
#include "parsers.h"

#include <QJsonDocument>

#include <algorithm>

// analyze.py, function for function. Python's dicts keep insertion order and
// its sorts are stable; OrderedMap and std::stable_sort keep both, because
// the order of equal cards is part of the output.

namespace {

struct PackageInfo {
    std::optional<qint64> uid;
    std::optional<QString> apkPath;
    std::optional<QString> version;
    QStringList granted;
    QStringList requested;
    QString body;
    bool internet = false;
};

using Packages = OrderedMap<QString, PackageInfo>;

struct Card {
    qint64 uid = 0;
    QStringList packages;
    QJsonObject versions;
    QStringList doing;
    QList<QJsonObject> observed;
    QList<QJsonObject> attempted;
    QList<QJsonObject> permittedUnseen;
    QList<QJsonObject> declared;
    QList<QJsonObject> trackers;
    qint64 rxBytes = 0;
    qint64 txBytes = 0;
    bool networkPermission = false;
    QList<QJsonObject> connections;
    QList<QJsonObject> dns;
    std::optional<QStringList> uidPackages;
};

QJsonValue optional(const std::optional<QString> &value) {
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}

QJsonValue optional(const std::optional<qint64> &value) {
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}

// row.get(a) or row.get(b) or ... or "".
QString firstTruthy(const QJsonObject &row, std::initializer_list<const char *> keys) {
    for (const char *key : keys) {
        const QJsonValue value = row.value(QLatin1String(key));
        if (py::truthy(value))
            return value.toString();
    }
    return QString();
}

QString frozen(const QJsonObject &row) {
    return QString::fromUtf8(QJsonDocument(row).toJson(QJsonDocument::Compact));
}

QString uidKey(const QJsonValue &uid) {
    return py::str(uid);
}

QStringList sortedStrings(QStringList list) {
    std::stable_sort(list.begin(), list.end(), py::less);
    return list;
}

QStringList dedupe(const QStringList &items) {
    QSet<QString> seen;
    QStringList result;
    for (const QString &item : items) {
        if (seen.contains(item))
            continue;
        seen.insert(item);
        result.append(item);
    }
    return result;
}

QList<QJsonObject> dedupeDicts(const QList<QJsonObject> &items) {
    QSet<QString> seen;
    QList<QJsonObject> result;
    for (const QJsonObject &item : items) {
        const QString key = frozen(item);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        result.append(item);
    }
    return result;
}

QList<QJsonObject> dedupeDns(QList<QJsonObject> items) {
    std::stable_sort(items.begin(), items.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return py::less(a.value("host").toString(), b.value("host").toString());
    });
    QSet<QPair<QString, QString>> seen;
    QList<QJsonObject> result;
    for (const QJsonObject &item : items) {
        const QPair<QString, QString> key(item.value("host").toString(), uidKey(item.value("uid")));
        if (seen.contains(key))
            continue;
        seen.insert(key);
        result.append(item);
    }
    return result;
}

QStringList sortByRank(QStringList categories) {
    std::stable_sort(categories.begin(), categories.end(), [](const QString &a, const QString &b) {
        return catalog::categoryRank(a) < catalog::categoryRank(b);
    });
    return categories;
}

QString endpoint(const QString &ip, qint64 port) {
    if (ip.contains(u':'))
        return u'[' + ip + QStringLiteral("]:") + QString::number(port);
    return ip + u':' + QString::number(port);
}

QList<SocketInfo> sockets(const Capture &capture) {
    QList<SocketInfo> all = parseProcNet(py::universalNewlines(capture.text("tcp")), "TCP");
    all += parseProcNet(py::universalNewlines(capture.text("tcp6")), "TCP");
    all += parseProcNet(py::universalNewlines(capture.text("udp")), "UDP");
    all += parseProcNet(py::universalNewlines(capture.text("udp6")), "UDP");
    return all;
}

QString text(const Capture &capture, const char *field) {
    return py::universalNewlines(capture.text(QLatin1String(field)));
}

Packages packagesOf(const Capture &capture) {
    Packages found;
    for (const PmPackage &package : parsePmList(text(capture, "packages"))) {
        PackageInfo info;
        info.uid = package.uid;
        info.apkPath = package.apkPath;
        found.insert(package.name, info);
    }
    const auto details = parsePackageDump(text(capture, "package_dump"));
    for (const auto &[name, detail] : details) {
        if (!found.contains(name)) {
            PackageInfo blank;
            blank.uid = detail.userId;
            found.insert(name, blank);
        }
        PackageInfo &info = found[name];
        if (!info.uid)
            info.uid = detail.userId;
        info.version = detail.versionName;
        info.granted = detail.granted;
        info.requested = detail.requested;
        info.body = detail.body;
        info.internet = detail.granted.contains(QStringLiteral("android.permission.INTERNET"));
    }
    return found;
}

OrderedMap<qint64, QStringList> doingOf(const Capture &capture, const Packages &packages) {
    OrderedMap<qint64, QStringList> lines;
    QHash<QString, qint64> known;
    for (const auto &[name, info] : packages)
        if (info.uid)
            known.insert(name, *info.uid);

    auto add = [&](std::optional<qint64> uid, const std::optional<QString> &package, const QString &line) {
        std::optional<qint64> resolved = uid;
        if (!resolved && package && !package->isEmpty()) {
            auto found = known.constFind(*package);
            if (found != known.cend())
                resolved = *found;
        }
        if (!resolved)
            return;
        lines[*resolved].append(line);
    };

    for (const ActivityInfo &activity : parseResumed(text(capture, "activities")))
        add(std::nullopt, activity.package, QStringLiteral("On screen: %1/%2").arg(activity.package, activity.activity));
    for (const ProcessInfo &process : parseProcesses(text(capture, "processes"))) {
        QStringList flags;
        if (process.foregroundActivity)
            flags.append(QStringLiteral("on screen"));
        if (process.foregroundService)
            flags.append(QStringLiteral("foreground service"));
        const QString suffix = flags.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(flags.join(QStringLiteral(", ")));
        const QString package = process.packages.isEmpty() ? process.name : process.packages.first();
        add(process.uid, package, QStringLiteral("Process ") + process.name + QStringLiteral(" pid ") + QString::number(process.pid) + suffix);
    }
    for (const ServiceInfo &service : parseServices(text(capture, "services"))) {
        const QString label = service.foreground ? QStringLiteral("Foreground service") : QStringLiteral("Service");
        add(std::nullopt, service.package, label + u' ' + service.component);
    }
    for (const JobInfo &job : parseJobs(text(capture, "jobs")))
        add(std::nullopt, job.package, QStringLiteral("Scheduled job ") + job.component);
    for (const WakeInfo &wake : parseWakelocks(text(capture, "power")))
        add(wake.uid, wake.package, QStringLiteral("Wakelock %1 (%2)").arg(wake.name, wake.kind));
    for (const SensorInfo &sensor : parseSensors(text(capture, "sensors")))
        for (const QString &name : sensor.sensors)
            add(sensor.uid, sensor.package, QStringLiteral("Sensor %1 (pid %2)").arg(name, py::str(optional(sensor.pid))));
    for (const LocationInfo &request : parseLocation(text(capture, "location"))) {
        const QString detail = request.request.isEmpty() ? QString() : QStringLiteral(": ") + request.request;
        add(request.uid, request.package, QStringLiteral("Location request") + detail);
    }
    const std::optional<QString> owner = parseClipboard(text(capture, "clipboard"));
    if (owner && !owner->isEmpty())
        add(std::nullopt, owner, QStringLiteral("Owns the current clipboard contents"));
    return lines;
}

QJsonObject opRow(const OpEntry &entry) {
    const catalog::Info *info = catalog::opInfo(entry.op);
    return {
        {"package", entry.package},
        {"uid", optional(entry.uid)},
        {"op", entry.op},
        {"mode", optional(entry.mode)},
        {"category", info->category},
        {"detail", info->detail},
        {"last_access", optional(entry.lastAccess)},
        {"last_reject", optional(entry.lastReject)},
    };
}

QJsonObject permissionRow(const QString &package, const QString &permission) {
    const catalog::Info *info = catalog::permissionInfo(permission);
    return {
        {"package", package},
        {"permission", permission},
        {"category", info->category},
        {"detail", info->detail},
    };
}

QJsonObject socketRow(const SocketInfo &sock) {
    return {
        {"proto", sock.proto},
        {"state", sock.state},
        {"local", endpoint(sock.localIp, sock.localPort)},
        {"remote", endpoint(sock.remoteIp, sock.remotePort)},
        {"uid", sock.uid},
    };
}

QJsonObject dnsRow(const DnsInfo &query) {
    return {
        {"host", query.host},
        {"uid", optional(query.uid)},
        {"tracker", optional(catalog::trackerForHost(query.host))},
    };
}

Card emptyCard(qint64 uid, const QStringList &names, const Packages &packages) {
    Card card;
    card.uid = uid;
    card.packages = names;
    for (const QString &name : names) {
        const PackageInfo *info = packages.find(name);
        if (info && info->version && !info->version->isEmpty())
            card.versions.insert(name, *info->version);
    }
    return card;
}

void ensurePackage(Card &card, const QString &package, const Packages &packages) {
    if (!card.packages.contains(package)) {
        card.packages.append(package);
        card.packages = sortedStrings(card.packages);
    }
    const PackageInfo *info = packages.find(package);
    if (info && info->version && !info->version->isEmpty())
        card.versions.insert(package, *info->version);
}

std::optional<qint64> uidForPackage(const QString &package, const Packages &packages, std::optional<qint64> fallback) {
    const PackageInfo *info = packages.find(package);
    if (info && info->uid)
        return info->uid;
    return fallback;
}

void sortRows(QList<QJsonObject> &rows) {
    struct Keyed {
        int rank;
        QString name;
        QJsonObject row;
    };
    QList<Keyed> keyed;
    keyed.reserve(rows.size());
    for (const QJsonObject &row : rows)
        keyed.append({catalog::categoryRank(firstTruthy(row, {"category", "name"})),
                      firstTruthy(row, {"op", "permission", "name"}), row});
    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed &a, const Keyed &b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        return py::less(a.name, b.name);
    });
    rows.clear();
    for (const Keyed &item : keyed)
        rows.append(item.row);
}

void finishCard(Card &card) {
    if (!card.uidPackages)
        card.uidPackages = card.packages;
    card.doing = dedupe(card.doing);
    for (QList<QJsonObject> *rows : {&card.observed, &card.attempted, &card.permittedUnseen, &card.declared, &card.trackers}) {
        *rows = dedupeDicts(*rows);
        sortRows(*rows);
    }
    std::stable_sort(card.connections.begin(), card.connections.end(), [](const QJsonObject &a, const QJsonObject &b) {
        for (const char *key : {"proto", "remote", "state"}) {
            const int order = py::compare(a.value(QLatin1String(key)).toString(), b.value(QLatin1String(key)).toString());
            if (order != 0)
                return order < 0;
        }
        return false;
    });
    card.dns = dedupeDns(card.dns);
}

void filterCard(Card &card, const QSet<QString> &wanted) {
    QStringList kept;
    for (const QString &name : card.packages)
        if (wanted.contains(name))
            kept.append(name);
    card.packages = kept;
    QJsonObject versions;
    for (auto it = card.versions.begin(); it != card.versions.end(); ++it)
        if (wanted.contains(it.key()))
            versions.insert(it.key(), it.value());
    card.versions = versions;
    for (QList<QJsonObject> *rows : {&card.observed, &card.attempted, &card.permittedUnseen, &card.declared, &card.trackers}) {
        QList<QJsonObject> filtered;
        for (const QJsonObject &row : *rows) {
            const QJsonValue package = row.value("package");
            if (package.isString() && wanted.contains(package.toString()))
                filtered.append(row);
        }
        *rows = filtered;
    }
}

bool isLive(const Card &card) {
    return !card.doing.isEmpty() || !card.observed.isEmpty() || !card.attempted.isEmpty() || !card.trackers.isEmpty() ||
           !card.connections.isEmpty() || !card.dns.isEmpty() || card.rxBytes || card.txBytes;
}

QList<QJsonObject> declaredOnly(const Card &card) {
    OrderedMap<QString, QStringList> byPackage;
    for (const QJsonObject &row : card.declared)
        byPackage[row.value("package").toString()].append(row.value("category").toString());
    for (const QJsonObject &row : card.permittedUnseen)
        byPackage[row.value("package").toString()].append(row.value("category").toString());
    QList<QJsonObject> rows;
    for (const auto &[package, categories] : byPackage) {
        const QStringList unique = sortByRank(dedupe(categories));
        if (!unique.isEmpty())
            rows.append({{"uid", card.uid}, {"package", package}, {"categories", QJsonArray::fromStringList(unique)}});
    }
    return rows;
}

QStringList grantedCategories(const PackageInfo &info) {
    QStringList categories;
    for (const QString &permission : info.granted)
        if (const catalog::Info *known = catalog::permissionInfo(permission))
            categories.append(known->category);
    return sortByRank(dedupe(categories));
}

QJsonArray toArray(const QList<QJsonObject> &rows) {
    QJsonArray array;
    for (const QJsonObject &row : rows)
        array.append(row);
    return array;
}

QJsonObject cardJson(const Card &card) {
    return {
        {"uid", card.uid},
        {"packages", QJsonArray::fromStringList(card.packages)},
        {"versions", card.versions},
        {"doing", QJsonArray::fromStringList(card.doing)},
        {"observed", toArray(card.observed)},
        {"attempted", toArray(card.attempted)},
        {"permitted_unseen", toArray(card.permittedUnseen)},
        {"declared", toArray(card.declared)},
        {"trackers", toArray(card.trackers)},
        {"transmitted", QJsonObject{
             {"rx_bytes", card.rxBytes},
             {"tx_bytes", card.txBytes},
             {"network_permission", card.networkPermission},
             {"connections", toArray(card.connections)},
             {"dns", toArray(card.dns)},
         }},
        {"uid_packages", QJsonArray::fromStringList(card.uidPackages.value_or(card.packages))},
    };
}

} // namespace

QJsonObject buildReport(const Capture &capture, bool includeAll, const QSet<QString> *filter) {
    const Packages packages = packagesOf(capture);
    const QList<OpEntry> ops = parseAppops(text(capture, "appops"));
    const QList<SocketInfo> socketList = sockets(capture);
    const QList<UidTraffic> traffic = parseNetstats(text(capture, "netstats"));
    const QList<DnsInfo> dnsRows = parseDns(text(capture, "dns"));

    QHash<qint64, QStringList> uidNames;
    for (const auto &[name, info] : packages)
        if (info.uid)
            uidNames[*info.uid].append(name);
    for (QStringList &names : uidNames)
        names = sortedStrings(names);

    const OrderedMap<qint64, QStringList> doing = doingOf(capture, packages);

    QList<QJsonObject> observed, attempted, permitted;
    for (const OpEntry &entry : ops) {
        if (!catalog::opInfo(entry.op))
            continue;
        const QJsonObject row = opRow(entry);
        if (entry.lastAccess && !entry.lastAccess->isEmpty())
            observed.append(row);
        else if ((entry.lastReject && !entry.lastReject->isEmpty()) ||
                 (entry.mode && (*entry.mode == QLatin1String("ignore") || *entry.mode == QLatin1String("deny"))))
            attempted.append(row);
        else if (entry.mode && (*entry.mode == QLatin1String("allow") || *entry.mode == QLatin1String("foreground")))
            permitted.append(row);
    }

    OrderedMap<qint64, Card> cards;
    auto cardFor = [&](qint64 uid) -> Card & {
        if (!cards.contains(uid))
            cards.insert(uid, emptyCard(uid, uidNames.value(uid), packages));
        return cards[uid];
    };

    for (const auto &[uid, lines] : doing)
        cardFor(uid).doing += lines;
    const std::pair<QList<QJsonObject> Card::*, const QList<QJsonObject> *> buckets[] = {
        {&Card::observed, &observed},
        {&Card::attempted, &attempted},
        {&Card::permittedUnseen, &permitted},
    };
    for (const auto &[member, rows] : buckets) {
        for (const QJsonObject &row : *rows) {
            const QJsonValue rowUid = row.value("uid");
            const std::optional<qint64> fallback = rowUid.isNull() ? std::nullopt : std::optional<qint64>(rowUid.toInteger());
            const std::optional<qint64> uid = uidForPackage(row.value("package").toString(), packages, fallback);
            if (!uid)
                continue;
            Card &card = cardFor(*uid);
            ensurePackage(card, row.value("package").toString(), packages);
            (card.*member).append(row);
        }
    }
    for (const auto &[name, info] : packages) {
        if (!info.uid)
            continue;
        const QList<catalog::Tracker> trackers = catalog::findTrackers(info.body);
        QList<QJsonObject> granted;
        for (const QString &permission : info.granted)
            if (catalog::permissionInfo(permission))
                granted.append(permissionRow(name, permission));
        if (trackers.isEmpty() && granted.isEmpty() && !info.internet)
            continue;
        Card &card = cardFor(*info.uid);
        ensurePackage(card, name, packages);
        for (const catalog::Tracker &tracker : trackers)
            card.trackers.append({{"package", name}, {"name", tracker.name}, {"prefix", tracker.prefix}, {"collects", tracker.collects}});
        card.declared += granted;
        if (info.internet)
            card.networkPermission = true;
    }

    for (const SocketInfo &sock : socketList)
        cardFor(sock.uid).connections.append(socketRow(sock));
    for (const UidTraffic &item : traffic) {
        Card &card = cardFor(item.uid);
        card.rxBytes += item.rxBytes;
        card.txBytes += item.txBytes;
    }
    for (const DnsInfo &query : dnsRows)
        if (query.uid && (uidNames.contains(*query.uid) || cards.contains(*query.uid)))
            cardFor(*query.uid).dns.append(dnsRow(query));

    QList<QJsonObject> unattributed;
    for (const DnsInfo &query : dnsRows)
        if (!query.uid || !(cards.contains(*query.uid) || uidNames.contains(*query.uid)))
            unattributed.append(dnsRow(query));

    QList<Card> live;
    QList<QJsonObject> declared;
    for (auto &[uid, card] : cards) {
        finishCard(card);
        const QStringList &names = card.packages;
        if (filter && !names.isEmpty()) {
            bool shared = false;
            for (const QString &name : names)
                shared = shared || filter->contains(name);
            if (!shared)
                continue;
        }
        if (filter && names.isEmpty())
            continue;
        if (filter)
            filterCard(card, *filter);
        if (isLive(card) || includeAll)
            live.append(card);
        else
            declared += declaredOnly(card);
    }

    // Packages that never grew a card still belong in the declared-only list
    // when they hold a sensitive grant and nothing live was recorded.
    QSet<QString> seenPackages;
    for (const auto &[uid, card] : cards)
        for (const QString &name : card.packages)
            seenPackages.insert(name);
    for (const auto &[name, info] : packages) {
        if (seenPackages.contains(name) || !info.uid)
            continue;
        if (filter && !filter->contains(name))
            continue;
        const QStringList categories = grantedCategories(info);
        if (!categories.isEmpty() && !includeAll) {
            declared.append({{"uid", *info.uid}, {"package", name}, {"categories", QJsonArray::fromStringList(categories)}});
        } else if (includeAll) {
            Card card = emptyCard(*info.uid, {name}, packages);
            for (const QString &permission : info.granted)
                if (catalog::permissionInfo(permission))
                    card.declared.append(permissionRow(name, permission));
            finishCard(card);
            live.append(card);
        }
    }

    std::stable_sort(live.begin(), live.end(), [](const Card &a, const Card &b) {
        if (a.observed.size() != b.observed.size())
            return a.observed.size() > b.observed.size();
        if (a.attempted.size() != b.attempted.size())
            return a.attempted.size() > b.attempted.size();
        if (a.connections.size() != b.connections.size())
            return a.connections.size() > b.connections.size();
        return py::less(a.packages.isEmpty() ? QString() : a.packages.first(),
                        b.packages.isEmpty() ? QString() : b.packages.first());
    });
    std::stable_sort(declared.begin(), declared.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return py::less(a.value("package").toString(), b.value("package").toString());
    });

    const QJsonObject &meta = capture.meta;
    auto metaValue = [&](const char *key) {
        const QJsonValue value = meta.value(QLatin1String(key));
        return py::truthy(value) ? value : QJsonValue(QString());
    };
    QJsonObject errors;
    const QJsonObject metaErrors = meta.value("errors").toObject();
    for (auto it = metaErrors.begin(); it != metaErrors.end(); ++it)
        errors.insert(it.key(), py::str(it.value()));

    QJsonArray liveArray;
    for (const Card &card : live)
        liveArray.append(cardJson(card));
    return {
        {"device", QJsonObject{
             {"model", metaValue("model")},
             {"android", metaValue("android")},
             {"sdk", metaValue("sdk")},
             {"serial", metaValue("serial")},
             {"captured_at", metaValue("captured_at")},
         }},
        {"sources", QJsonArray::fromStringList(catalog::sources())},
        {"limits", QJsonArray::fromStringList(catalog::limits())},
        {"live", liveArray},
        {"declared_only", toArray(declared)},
        {"unattributed_dns", toArray(dedupeDns(unattributed))},
        {"errors", errors},
    };
}

QJsonObject watchSnapshot(const Capture &capture) {
    QHash<qint64, QStringList> uidNames;
    for (const PmPackage &package : parsePmList(text(capture, "packages")))
        uidNames[package.uid].append(package.name);
    QJsonArray connections;
    for (const SocketInfo &sock : sockets(capture)) {
        connections.append(QJsonObject{
            {"proto", sock.proto},
            {"state", sock.state},
            {"remote", endpoint(sock.remoteIp, sock.remotePort)},
            {"uid", sock.uid},
            {"packages", QJsonArray::fromStringList(sortedStrings(uidNames.value(sock.uid)))},
        });
    }
    QJsonArray ops;
    for (const OpEntry &entry : parseAppops(text(capture, "appops"))) {
        if (!catalog::opInfo(entry.op))
            continue;
        ops.append(QJsonObject{
            {"package", entry.package},
            {"op", entry.op},
            {"last_access", optional(entry.lastAccess)},
            {"last_reject", optional(entry.lastReject)},
        });
    }
    QStringList focus;
    for (const ActivityInfo &item : parseResumed(text(capture, "activities")))
        focus.append(item.package + u'/' + item.activity);
    return {
        {"connections", connections},
        {"ops", ops},
        {"focus", QJsonArray::fromStringList(sortedStrings(focus))},
    };
}

namespace {

QStringList strings(const QJsonValue &value) {
    QStringList out;
    for (const QJsonValue &item : value.toArray())
        out.append(item.toString());
    return out;
}

// item.get(key), with a missing key read as None.
QJsonValue get(const QJsonObject &item, const char *key) {
    const QJsonValue value = item.value(QLatin1String(key));
    return value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
}

QString socketIdentity(const QJsonObject &item) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{get(item, "proto"), get(item, "uid"), get(item, "remote"), get(item, "state")})
                                 .toJson(QJsonDocument::Compact));
}

QStringList sortedDifference(const QStringList &from, const QStringList &minus) {
    QSet<QString> left(from.cbegin(), from.cend());
    for (const QString &item : minus)
        left.remove(item);
    return sortedStrings(QStringList(left.cbegin(), left.cend()));
}

} // namespace

QStringList diffWatch(const QJsonObject &previous, const QJsonObject &current) {
    QStringList events;
    const QStringList currentFocus = strings(current.value("focus"));
    const QStringList previousFocus = strings(previous.value("focus"));
    for (const QString &item : sortedDifference(currentFocus, previousFocus))
        events.append(QStringLiteral("On screen: ") + item);
    for (const QString &item : sortedDifference(previousFocus, currentFocus))
        events.append(QStringLiteral("Left screen: ") + item);

    QSet<QString> previousSockets;
    for (const QJsonValue &item : previous.value("connections").toArray())
        previousSockets.insert(socketIdentity(item.toObject()));
    for (const QJsonValue &value : current.value("connections").toArray()) {
        const QJsonObject item = value.toObject();
        if (previousSockets.contains(socketIdentity(item)))
            continue;
        QString who = strings(item.value("packages")).join(QStringLiteral(", "));
        if (who.isEmpty())
            who = QStringLiteral("uid ") + py::str(item.value("uid"));
        events.append(QStringLiteral("%1 %2 %3 -> %4")
                          .arg(who, py::str(item.value("proto")), py::str(item.value("state")), py::str(item.value("remote"))));
    }

    QHash<QPair<QString, QString>, QPair<QJsonValue, QJsonValue>> previousOps;
    for (const QJsonValue &value : previous.value("ops").toArray()) {
        const QJsonObject item = value.toObject();
        previousOps.insert({py::str(item.value("package")), py::str(item.value("op"))},
                           {get(item, "last_access"), get(item, "last_reject")});
    }
    for (const QJsonValue &value : current.value("ops").toArray()) {
        const QJsonObject item = value.toObject();
        const QJsonValue access = get(item, "last_access");
        const QJsonValue reject = get(item, "last_reject");
        const QPair<QString, QString> key(py::str(item.value("package")), py::str(item.value("op")));
        auto found = previousOps.constFind(key);
        if (found != previousOps.cend() && found->first == access && found->second == reject)
            continue;
        if (!(py::truthy(access) || py::truthy(reject)))
            continue;
        const QJsonValue when = py::truthy(access) ? access : reject;
        events.append(QStringLiteral("%1 %2 %3").arg(key.first, key.second, py::str(when)));
    }
    return events;
}
