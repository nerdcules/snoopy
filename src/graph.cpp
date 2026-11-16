#include "graph.h"

#include "parsers.h"

#include <QHash>
#include <QJsonObject>

#include <algorithm>

// summary.py, function for function. names.cached_name becomes nameFor.

namespace {

const QHash<QString, QString> &knownNames() {
    static const QHash<QString, QString> known = {
        {"com.whatsapp", "WhatsApp"},
        {"com.android.vending", "Play Store"},
        {"com.google.android.gms", "Play services"},
        {"com.google.android.youtube", "YouTube"},
        {"com.google.android.apps.youtube.music", "YouTube Music"},
        {"com.google.android.googlequicksearchbox", "Google"},
        {"com.google.android.apps.photos", "Photos"},
        {"com.google.android.apps.messaging", "Messages"},
        {"com.zhiliaoapp.musically", "TikTok"},
        {"com.facebook.katana", "Facebook"},
        {"com.instagram.android", "Instagram"},
        {"com.android.chrome", "Chrome"},
        {"com.anthropic.claude", "Claude"},
        {"com.microsoft.launcher", "Microsoft Launcher"},
        {"com.microsoft.skydrive", "OneDrive"},
        {"com.microsoft.appmanager", "Phone Link"},
        {"com.microsoft.emmx", "Edge"},
        {"com.microsoft.office.outlook", "Outlook"},
        {"com.facebook.orca", "Messenger"},
        {"com.linkedin.android", "LinkedIn"},
        {"com.amazon.mShop.android.shopping", "Amazon"},
        {"com.wsandroid.suite", "Webroot"},
        {"com.life360.android.safetymapd", "Life360"},
        {"com.trainerize.Trainerize", "Trainerize"},
        {"com.google.android.projection.gearhead", "Android Auto"},
    };
    return known;
}

struct Group {
    QString label;
    QStringList needles;
};

const QList<Group> &groups() {
    static const QList<Group> list = {
        {"Location", {"location", "gps", "wi-fi scan"}},
        {"Camera", {"camera"}},
        {"Microphone", {"microphone", "playback audio"}},
        {"Contacts", {"contact"}},
        {"Photos & files", {"photo", "video", "audio file", "files"}},
        {"Clipboard", {"clipboard"}},
        {"Identity", {"phone", "advertising", "account", "hardware", "identifier"}},
        {"Messages", {"sms", "call history"}},
    };
    return list;
}

const QHash<QString, QString> &blurbs() {
    static const QHash<QString, QString> blurb = {
        {"Location", "Where the phone is."},
        {"Camera", "Images or video from the camera."},
        {"Microphone", "Audio from the microphone or from playback."},
        {"Contacts", "Names and numbers from the address book."},
        {"Photos & files", "Photos, videos, or shared files."},
        {"Clipboard", "Text or images copied on the phone."},
        {"Identity", "Phone, account, or advertising identifiers."},
        {"Messages", "Texts or call history."},
        {"Motion", "Motion sensor readings."},
    };
    return blurb;
}

struct Line {
    qint64 seconds = 0;
    QString text;
};

struct Read {
    QString label;
    QList<Line> lines;
};

struct Destination {
    QString label;
    QString kind;
    QString endpoint;
    qint64 count = 1;
    QString relation;
};

struct Card {
    QString package;
    QString name;
    QList<Read> reads;
    QList<Destination> destinations;
    qint64 txBytes = 0;
    qint64 rxBytes = 0;
};

QString str(const QJsonObject &item, const char *key) {
    const QJsonValue value = item.value(QLatin1String(key));
    return py::truthy(value) ? py::str(value) : QString();
}

qint64 intOf(const QJsonValue &value, qint64 fallback) {
    if (!py::truthy(value))
        return fallback;
    return value.toInteger(qint64(value.toDouble()));
}

bool looksLikePackage(const QString &name) {
    static const QRegularExpression pattern = py::re(QStringLiteral(R"re(\A(?:[A-Za-z][\w]*(\.[A-Za-z][\w]*)+)\z)re"));
    return pattern.match(name).hasMatch();
}

QString packageId(const QString &name, const QJsonObject &info) {
    const QJsonArray packages = info.value("packages").toArray();
    if (packages.size() == 1)
        return py::str(packages.first());
    if (looksLikePackage(name))
        return name;
    return QString();
}

bool quiet(const QString &name) {
    if (name.isEmpty() || name.startsWith(QLatin1String("uid ")) || name.contains(u',') ||
        name.contains(QLatin1String(" other packages")))
        return true;
    return name == QLatin1String("android") || name.startsWith(QLatin1String("android.")) ||
           name.startsWith(QLatin1String("com.android.")) || name.startsWith(QLatin1String("com.google.android."));
}

bool localService(const Destination &node) {
    return node.kind.contains(QLatin1String("port 67")) || node.kind.contains(QLatin1String("port 68"));
}

bool recent(const QString &detail) {
    static const QRegularExpression days = py::re(QStringLiteral(R"re(\(-(\d+)d)re"));
    static const QRegularExpression hours = py::re(QStringLiteral(R"re(\(-(\d+)h)re"));
    const QRegularExpressionMatch d = days.match(detail);
    if (d.hasMatch() && py::toInt(d.capturedView(1)) >= 1)
        return false;
    const QRegularExpressionMatch h = hours.match(detail);
    if (h.hasMatch() && py::toInt(h.capturedView(1)) >= 24)
        return false;
    return true;
}

const QRegularExpression &agoPattern() {
    static const QRegularExpression ago = py::re(QStringLiteral(R"re(\(-(\d+)([dhms]))re"));
    return ago;
}

QString ago(const QString &detail) {
    const QRegularExpressionMatch m = agoPattern().match(detail);
    if (!m.hasMatch())
        return QStringLiteral("in this sample");
    const qint64 count = py::toInt(m.capturedView(1));
    static const QHash<QString, QString> words = {{"d", "day"}, {"h", "hour"}, {"m", "minute"}, {"s", "second"}};
    QString word = words.value(m.captured(2));
    if (count != 1)
        word += u's';
    return QString::number(count) + u' ' + word + QStringLiteral(" ago");
}

qint64 agoSeconds(const QString &detail) {
    const QRegularExpressionMatch m = agoPattern().match(detail);
    if (!m.hasMatch())
        return 0;
    static const QHash<QString, qint64> scale = {{"d", 86400}, {"h", 3600}, {"m", 60}, {"s", 1}};
    return py::toInt(m.capturedView(1)) * scale.value(m.captured(2));
}

QString itemName(const QString &source) {
    if (source.startsWith(QLatin1String("android.sensor.")))
        return source.mid(15).replace(u'_', u' ');
    return source;
}

std::optional<QString> kindOf(const QString &source) {
    if (source.startsWith(QLatin1String("android.sensor.")))
        return QStringLiteral("Motion");
    const QString lowered = source.toLower();
    for (const Group &group : groups())
        for (const QString &needle : group.needles)
            if (lowered.contains(needle))
                return group.label;
    return std::nullopt;
}

int kindRank(const QString &kind) {
    for (int i = 0; i < groups().size(); ++i)
        if (groups()[i].label == kind)
            return i;
    if (kind == QLatin1String("Motion"))
        return int(groups().size());
    return int(groups().size()) + 1;
}

QString port(const QString &endpoint) {
    if (endpoint.startsWith(u'[') && endpoint.contains(QLatin1String("]:")))
        return endpoint.mid(endpoint.lastIndexOf(QLatin1String("]:")) + 2);
    if (endpoint.count(u':') == 1)
        return endpoint.mid(endpoint.lastIndexOf(u':') + 1);
    return QString();
}

QString host(const QString &endpoint) {
    if (endpoint.startsWith(u'[') && endpoint.contains(QLatin1String("]:")))
        return endpoint.left(endpoint.lastIndexOf(QLatin1String("]:"))) + u']';
    if (endpoint.count(u':') == 1)
        return endpoint.left(endpoint.lastIndexOf(u':'));
    return endpoint;
}

QString shortHost(const QString &value) {
    if (value.startsWith(u'[') && value.endsWith(u']') && value.count(u':') > 2) {
        QStringList parts;
        for (const QString &part : value.mid(1, value.size() - 2).split(u':'))
            if (!part.isEmpty())
                parts.append(part);
        if (parts.size() >= 2)
            return u'[' + parts.first() + QStringLiteral(":…:") + parts.last() + u']';
    }
    return value;
}

Destination destinationOf(const QJsonObject &flow) {
    const QString target = str(flow, "destination");
    const QString information = str(flow, "information");
    Destination node;
    if (information == QLatin1String("DNS lookup")) {
        node.label = target;
        node.kind = str(flow, "via");
        if (node.kind.isEmpty())
            node.kind = QStringLiteral("DNS");
        return node;
    }
    const QString p = port(target);
    if (p == QLatin1String("443") || information == QLatin1String("QUIC")) {
        node.kind = QStringLiteral("Encrypted");
    } else if (!p.isEmpty()) {
        const QString proto = information.startsWith(QLatin1String("UDP")) ? QStringLiteral("UDP") : QStringLiteral("TCP");
        node.kind = proto + QStringLiteral(" · port ") + p;
    } else {
        node.kind = information.isEmpty() ? QStringLiteral("Connected") : information;
    }
    node.label = shortHost(host(target));
    return node;
}

void addDestination(Card &card, const QJsonObject &flow) {
    Destination node = destinationOf(flow);
    if (node.label.isEmpty() || localService(node))
        return;
    const qint64 count = intOf(flow.value("count"), 1);
    for (Destination &existing : card.destinations) {
        if (existing.label == node.label && existing.kind == node.kind) {
            existing.count += count;
            return;
        }
    }
    node.relation = str(flow, "information") == QLatin1String("DNS lookup") ? QStringLiteral("lookup") : QStringLiteral("connects");
    node.endpoint = str(flow, "destination");
    if (node.endpoint.isEmpty())
        node.endpoint = node.label;
    node.count = count;
    card.destinations.append(node);
}

void addRead(Card &card, const QString &kind, const QString &source, const QString &detail) {
    const QString line = itemName(source) + QStringLiteral(", ") + ago(detail) + u'.';
    const qint64 seconds = agoSeconds(detail);
    for (Read &item : card.reads) {
        if (item.label == kind) {
            const bool known = std::any_of(item.lines.cbegin(), item.lines.cend(), [&](const Line &l) { return l.text == line; });
            if (!known)
                item.lines.append({seconds, line});
            return;
        }
    }
    card.reads.append({kind, {{seconds, line}}});
}

QString describeDestination(const Destination &node, const QString &named) {
    const QString endpoint = node.endpoint.isEmpty() ? node.label : node.endpoint;
    const QString where = named.isEmpty() ? endpoint : named + QStringLiteral(" (") + endpoint + u')';
    const QString kind = node.kind.isEmpty() ? QStringLiteral("Connected") : node.kind;
    const qint64 count = node.count ? node.count : 1;
    if (node.relation == QLatin1String("lookup")) {
        const QString tracker = (!kind.isEmpty() && kind != QLatin1String("DNS"))
                                    ? QStringLiteral(" ") + kind + QStringLiteral(" is a known tracker in that name.")
                                    : QString();
        return QStringLiteral("DNS lookup for ") + endpoint + u'.' + tracker;
    }
    const QString noun = count == 1 ? QStringLiteral("socket") : QStringLiteral("sockets");
    if (kind == QLatin1String("Encrypted"))
        return QStringLiteral("Encrypted connection to ") + where + QStringLiteral(". ") + QString::number(count) + u' ' + noun +
               QStringLiteral(". The address is visible. The contents are not.");
    return kind + QStringLiteral(" to ") + where + QStringLiteral(". ") + QString::number(count) + u' ' + noun + u'.';
}

// names._key: the bare address cached_name looks up, or "" for anything
// that is not an IPv4 or IPv6 literal.
QString nameKey(const QString &value) {
    static const QRegularExpression v4 = py::re(QStringLiteral(R"re(^\d{1,3}(?:\.\d{1,3}){3}$)re"));
    static const QRegularExpression v6 = py::re(QStringLiteral(R"re(\A(?:[0-9A-Fa-f:]+)\z)re"));
    QString text = py::strip(value);
    while (!text.isEmpty() && (text.front() == u'[' || text.front() == u']'))
        text.remove(0, 1);
    while (!text.isEmpty() && (text.back() == u'[' || text.back() == u']'))
        text.chop(1);
    if (py::match(v4, text).hasMatch())
        return text;
    if (text.contains(u':') && v6.match(text).hasMatch())
        return text;
    return QString();
}

} // namespace

QString friendlyName(const QString &package) {
    auto known = knownNames().constFind(package);
    if (known != knownNames().cend())
        return *known;
    if (package.isEmpty() || package.contains(u' ') || package.startsWith(QLatin1String("uid ")))
        return package.isEmpty() ? QStringLiteral("App") : package;
    static const QRegularExpression camel = py::re(QStringLiteral("([a-z])([A-Z])"));
    QString tail = package.mid(package.lastIndexOf(u'.') + 1).replace(u'_', u' ');
    tail.replace(camel, QStringLiteral("\\1 \\2"));
    if (tail.isEmpty())
        return package;
    const qsizetype first = tail.at(0).isHighSurrogate() && tail.size() > 1 ? 2 : 1;
    return tail.left(first).toUpper() + tail.mid(first);
}

QJsonArray graphFrom(const QJsonArray &flows, const QJsonArray &apps, const std::function<QString(const QString &)> &nameFor) {
    QHash<QString, QJsonObject> volumes;
    for (const QJsonValue &value : apps) {
        const QJsonObject item = value.toObject();
        volumes.insert(py::str(item.value("name")), item);
    }
    OrderedMap<QString, Card> cards;
    auto cardFor = [&](const QString &name) -> Card & {
        if (!cards.contains(name)) {
            const QJsonObject info = volumes.value(name);
            Card card;
            card.package = packageId(name, info);
            card.name = name == QLatin1String("uid 0") ? QStringLiteral("System")
                                                       : friendlyName(card.package.isEmpty() ? name : card.package);
            card.txBytes = intOf(info.value("tx_bytes"), 0);
            card.rxBytes = intOf(info.value("rx_bytes"), 0);
            cards.insert(name, card);
        }
        return cards[name];
    };

    for (const QJsonValue &value : flows) {
        const QJsonObject flow = value.toObject();
        if (flow.value("kind").toString() != QLatin1String("transmission"))
            continue;
        const QString source = str(flow, "source");
        if (source.isEmpty() || source == QLatin1String("Unknown app"))
            continue;
        addDestination(cardFor(source), flow);
    }

    for (const QJsonValue &value : flows) {
        const QJsonObject flow = value.toObject();
        if (flow.value("kind").toString() != QLatin1String("collection") || flow.value("via").toString() != QLatin1String("read"))
            continue;
        const QString detail = str(flow, "detail");
        if (!recent(detail))
            continue;
        const QString source = str(flow, "source");
        const std::optional<QString> kind = kindOf(source);
        if (!kind)
            continue;
        const QString name = str(flow, "destination");
        if (!cards.contains(name)) {
            if (quiet(name))
                continue;
            cardFor(name);
        }
        addRead(cards[name], *kind, source.isEmpty() ? *kind : source, detail);
    }

    QList<QJsonObject> ordered;
    QList<std::pair<qint64, QString>> keys;
    for (auto &[name, card] : cards) {
        if (card.destinations.size() > 6)
            card.destinations = card.destinations.mid(0, 6);
        if (card.reads.isEmpty() && card.destinations.isEmpty())
            continue;
        QJsonArray destinations;
        for (const Destination &node : card.destinations) {
            const QString hostName = node.relation == QLatin1String("lookup") ? node.label : host(node.endpoint);
            const QString key = nameKey(hostName);
            const QString named = (key.isEmpty() || !nameFor) ? QString() : nameFor(key);
            destinations.append(QJsonObject{
                {"label", node.label},
                {"kind", node.kind},
                {"endpoint", node.endpoint},
                {"count", node.count},
                {"relation", node.relation},
                {"host", hostName},
                {"name", named},
                {"detail", describeDestination(node, named)},
            });
        }
        QList<Read> reads = card.reads;
        std::stable_sort(reads.begin(), reads.end(), [](const Read &a, const Read &b) { return kindRank(a.label) < kindRank(b.label); });
        QJsonArray readArray;
        for (Read &item : reads) {
            std::stable_sort(item.lines.begin(), item.lines.end(), [](const Line &a, const Line &b) {
                if (a.seconds != b.seconds)
                    return a.seconds < b.seconds;
                return py::less(a.text, b.text);
            });
            QStringList lines;
            for (qsizetype i = 0; i < item.lines.size() && i < 4; ++i)
                lines.append(item.lines[i].text);
            const QString detail = py::strip(blurbs().value(item.label) + u' ' + lines.join(u' '));
            readArray.append(QJsonObject{{"label", item.label}, {"detail", detail}});
        }
        ordered.append(QJsonObject{
            {"package", card.package},
            {"name", card.name},
            {"reads", readArray},
            {"destinations", destinations},
            {"tx_bytes", card.txBytes},
            {"rx_bytes", card.rxBytes},
        });
        keys.append({-(card.txBytes + card.rxBytes), card.name});
    }
    QList<qsizetype> index(ordered.size());
    for (qsizetype i = 0; i < index.size(); ++i)
        index[i] = i;
    std::stable_sort(index.begin(), index.end(), [&](qsizetype a, qsizetype b) {
        if (keys[a].first != keys[b].first)
            return keys[a].first < keys[b].first;
        return py::less(keys[a].second, keys[b].second);
    });
    QJsonArray result;
    for (qsizetype i : index)
        result.append(ordered[i]);
    return result;
}
