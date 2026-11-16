#include "parsers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVariant>

#include <algorithm>
#include <cmath>

// Every pattern below is the one in parsers.py, written as a raw string.

namespace py {

QStringList splitLines(const QString &text) {
    QStringList lines;
    const qsizetype size = text.size();
    qsizetype start = 0;
    qsizetype i = 0;
    while (i < size) {
        const char16_t c = text.at(i).unicode();
        const bool brk = c == '\n' || c == '\r' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d ||
                         c == 0x1e || c == 0x85 || c == 0x2028 || c == 0x2029;
        if (!brk) {
            ++i;
            continue;
        }
        lines.append(text.mid(start, i - start));
        if (c == '\r' && i + 1 < size && text.at(i + 1) == u'\n')
            ++i;
        ++i;
        start = i;
    }
    if (start < size)
        lines.append(text.mid(start));
    return lines;
}

QString universalNewlines(const QString &text) {
    if (!text.contains(u'\r'))
        return text;
    QString out = text;
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(u'\r', u'\n');
    return out;
}

bool isSpace(char32_t c) {
    switch (c) {
    case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20:
    case 0x85: case 0xa0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202f: case 0x205f: case 0x3000:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200a;
    }
}

QString strip(const QString &text) {
    qsizetype start = 0;
    qsizetype end = text.size();
    while (start < end && isSpace(text.at(start).unicode()))
        ++start;
    while (end > start && isSpace(text.at(end - 1).unicode()))
        --end;
    if (start == 0 && end == text.size())
        return text;
    return text.mid(start, end - start);
}

qint64 toInt(QStringView digits) {
    qint64 value = 0;
    for (char32_t c : digits.toUcs4()) {
        const int digit = QChar::digitValue(c);
        value = value * 10 + (digit < 0 ? 0 : digit);
    }
    return value;
}

int compare(const QString &a, const QString &b) {
    const qsizetype n = std::min(a.size(), b.size());
    for (qsizetype i = 0; i < n; ++i) {
        int x = a.at(i).unicode();
        int y = b.at(i).unicode();
        if (x == y)
            continue;
        // A surrogate stands for a code point above every BMP unit.
        if (x >= 0xd800 && x <= 0xdfff)
            x += 0x10000;
        if (y >= 0xd800 && y <= 0xdfff)
            y += 0x10000;
        return x < y ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

QString str(const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return QStringLiteral("None");
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Double: {
        const QVariant variant = value.toVariant();
        if (variant.typeId() == QMetaType::LongLong)
            return QString::number(variant.toLongLong());
        const double number = value.toDouble();
        QString text = QString::number(number, 'g', QLocale::FloatingPointShortest);
        if (std::isfinite(number) && !text.contains(u'.') && !text.contains(u'e'))
            text += QStringLiteral(".0");
        return text;
    }
    default:
        return QString::fromUtf8(QJsonDocument::fromVariant(value.toVariant()).toJson(QJsonDocument::Compact));
    }
}

bool truthy(const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::Bool:
        return value.toBool();
    case QJsonValue::Double:
        return value.toDouble() != 0;
    case QJsonValue::String:
        return !value.toString().isEmpty();
    case QJsonValue::Array:
        return !value.toArray().isEmpty();
    case QJsonValue::Object:
        return !value.toObject().isEmpty();
    default:
        return false;
    }
}

QRegularExpression re(const QString &pattern, bool ignoreCase) {
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (ignoreCase)
        options |= QRegularExpression::CaseInsensitiveOption;
    QRegularExpression expression(pattern, options);
    expression.optimize();
    return expression;
}

QRegularExpressionMatch match(const QRegularExpression &re, const QString &text) {
    return re.match(text, 0, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
}

namespace {

// setobject.c, for keys whose hash is the key itself.
struct IntSet {
    static constexpr size_t kLinearProbes = 9;
    QList<std::optional<qint64>> table = QList<std::optional<qint64>>(8);
    size_t mask = 7;
    qint64 fill = 0;
    qint64 used = 0;

    static size_t hashOf(qint64 key) {
        const quint64 modulus = (quint64(1) << 61) - 1;
        return size_t(quint64(key) % modulus);
    }

    static void insertClean(QList<std::optional<qint64>> &t, size_t m, qint64 key) {
        size_t perturb = hashOf(key);
        size_t i = perturb & m;
        while (true) {
            if (!t[i]) {
                t[i] = key;
                return;
            }
            if (i + kLinearProbes <= m) {
                for (size_t j = 1; j <= kLinearProbes; ++j) {
                    if (!t[i + j]) {
                        t[i + j] = key;
                        return;
                    }
                }
            }
            perturb >>= 5;
            i = (i * 5 + 1 + perturb) & m;
        }
    }

    void resize(qint64 minused) {
        size_t size = 8;
        while (qint64(size) <= minused)
            size <<= 1;
        QList<std::optional<qint64>> fresh(qsizetype(size), std::nullopt);
        for (const auto &key : table)
            if (key)
                insertClean(fresh, size - 1, *key);
        table = fresh;
        mask = size - 1;
    }

    void add(qint64 key) {
        size_t perturb = hashOf(key);
        size_t i = perturb & mask;
        while (true) {
            size_t probes = (i + kLinearProbes <= mask) ? kLinearProbes : 0;
            size_t index = i;
            while (true) {
                const auto &entry = table[qsizetype(index)];
                if (!entry) {
                    table[qsizetype(index)] = key;
                    ++fill;
                    ++used;
                    if (size_t(fill) * 5 < mask * 3)
                        return;
                    resize(used > 50000 ? used * 2 : used * 4);
                    return;
                }
                if (*entry == key)
                    return;
                if (probes == 0)
                    break;
                --probes;
                ++index;
            }
            perturb >>= 5;
            i = (i * 5 + 1 + perturb) & mask;
        }
    }

    void merge(const IntSet &other) {
        if (other.used == 0)
            return;
        if (size_t(fill + other.used) * 5 >= mask * 3)
            resize((used + other.used) * 2);
        if (fill == 0 && mask == other.mask) {
            table = other.table;
            fill = other.fill;
            used = other.used;
            return;
        }
        if (fill == 0) {
            fill = other.used;
            used = other.used;
            for (const auto &key : other.table)
                if (key)
                    insertClean(table, mask, *key);
            return;
        }
        for (const auto &key : other.table)
            if (key)
                add(*key);
    }
};

} // namespace

QList<qint64> setUnionOrder(const QList<qint64> &a, const QList<qint64> &b) {
    IntSet first;
    for (qint64 key : a)
        first.add(key);
    IntSet second;
    for (qint64 key : b)
        second.add(key);
    IntSet result;
    result.merge(first);
    result.merge(second);
    QList<qint64> order;
    for (const auto &key : result.table)
        if (key)
            order.append(*key);
    return order;
}

} // namespace py

namespace {

bool isDigits(const QString &text) {
    if (text.isEmpty())
        return false;
    for (char32_t c : text.toUcs4())
        if (QChar::digitValue(c) < 0)
            return false;
    return true;
}

std::optional<qint64> intOpt(const QRegularExpressionMatch &m, int group) {
    if (!m.hasMatch() || m.capturedStart(group) < 0)
        return std::nullopt;
    return py::toInt(m.capturedView(group));
}

std::optional<QString> nonEmpty(const QString &text) {
    if (text.isEmpty())
        return std::nullopt;
    return text;
}

QString rstripChars(QString text, const QString &chars) {
    while (!text.isEmpty() && chars.contains(text.back()))
        text.chop(1);
    return text;
}

} // namespace

std::optional<qint64> parseUid(const QString &token) {
    static const QRegularExpression formatted = py::re(QStringLiteral(R"re(\A(?:^u(\d+)([as])(\d+)$)\z)re"));
    const QString text = py::strip(token);
    if (isDigits(text))
        return py::toInt(text);
    const QRegularExpressionMatch m = formatted.match(text);
    if (!m.hasMatch())
        return std::nullopt;
    const qint64 user = py::toInt(m.capturedView(1));
    const qint64 number = py::toInt(m.capturedView(3));
    const qint64 appId = m.captured(2) == QLatin1String("a") ? number + 10000 : number;
    return user * 100000 + appId;
}

// ---------------------------------------------------------------- appops

namespace {

const QSet<QString> &modes() {
    static const QSet<QString> set = {"allow", "ignore", "deny", "foreground", "default", "errored", "ask"};
    return set;
}

struct InlineOp {
    std::optional<QString> mode, access, reject;
};

InlineOp inlineOp(const QString &rest) {
    InlineOp out;
    QStringList parts;
    for (const QString &part : rest.split(u';')) {
        const QString stripped = py::strip(part);
        if (!stripped.isEmpty())
            parts.append(stripped);
    }
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const QString &part = parts[index];
        const QString lower = part.toLower();
        if (index == 0 && modes().contains(lower)) {
            out.mode = lower;
            continue;
        }
        if (lower.startsWith(QLatin1String("time=")))
            out.access = nonEmpty(py::strip(part.section(u'=', 1)));
        else if (lower.startsWith(QLatin1String("rejecttime=")))
            out.reject = nonEmpty(py::strip(part.section(u'=', 1)));
        else if (modes().contains(lower) && !out.mode)
            out.mode = lower;
    }
    return out;
}

} // namespace

QList<OpEntry> parseAppops(const QString &text) {
    static const QRegularExpression uidLine = py::re(QStringLiteral(R"re(^Uid\s+([^:\s]+):\s*$)re"));
    static const QRegularExpression pkgLine = py::re(QStringLiteral(R"re(^Package\s+([^:\s]+):\s*$)re"));
    static const QRegularExpression opLine = py::re(QStringLiteral(R"re(^([A-Z][A-Z0-9_]+)(?:\s*\(([^)]+)\))?(?::\s*(.*))?$)re"));
    static const QRegularExpression accessLine = py::re(QStringLiteral(R"re(^Access:\s*(.*)$)re"));
    static const QRegularExpression rejectLine = py::re(QStringLiteral(R"re(^Reject:\s*(.*)$)re"));

    std::optional<qint64> currentUid;
    std::optional<QString> currentPackage;
    qsizetype current = -1;
    QList<OpEntry> found;
    QHash<QPair<QString, QString>, qsizetype> index;

    auto remember = [&](const OpEntry &entry) -> qsizetype {
        const QPair<QString, QString> key(entry.package, entry.op);
        auto existing = index.constFind(key);
        if (existing == index.cend()) {
            index.insert(key, found.size());
            found.append(entry);
            return found.size() - 1;
        }
        OpEntry &old = found[*existing];
        if (!old.mode && entry.mode && !entry.mode->isEmpty())
            old.mode = entry.mode;
        if (!old.lastAccess && entry.lastAccess && !entry.lastAccess->isEmpty())
            old.lastAccess = entry.lastAccess;
        if (!old.lastReject && entry.lastReject && !entry.lastReject->isEmpty())
            old.lastReject = entry.lastReject;
        if (!old.uid && entry.uid)
            old.uid = entry.uid;
        return *existing;
    };

    for (const QString &raw : py::splitLines(text)) {
        const QString line = py::strip(raw);
        if (line.isEmpty())
            continue;
        QRegularExpressionMatch m = py::match(uidLine, line);
        if (m.hasMatch()) {
            currentUid = parseUid(m.captured(1));
            currentPackage.reset();
            current = -1;
            continue;
        }
        m = py::match(pkgLine, line);
        if (m.hasMatch()) {
            currentPackage = m.captured(1);
            current = -1;
            continue;
        }
        m = py::match(accessLine, line);
        if (m.hasMatch() && current >= 0 && !found[current].lastAccess) {
            found[current].lastAccess = nonEmpty(py::strip(m.captured(1)));
            continue;
        }
        m = py::match(rejectLine, line);
        if (m.hasMatch() && current >= 0 && !found[current].lastReject) {
            found[current].lastReject = nonEmpty(py::strip(m.captured(1)));
            continue;
        }
        m = py::match(opLine, line);
        if (m.hasMatch() && currentPackage && !currentPackage->isEmpty()) {
            const QString mode = m.captured(2);
            const InlineOp inl = inlineOp(m.captured(3));
            OpEntry entry;
            entry.package = *currentPackage;
            entry.uid = currentUid;
            entry.op = m.captured(1);
            if (!mode.isEmpty())
                entry.mode = mode;
            else if (inl.mode && !inl.mode->isEmpty())
                entry.mode = inl.mode;
            entry.lastAccess = inl.access;
            entry.lastReject = inl.reject;
            current = remember(entry);
        }
    }
    return found;
}

// ---------------------------------------------------------------- packages

QList<PmPackage> parsePmList(const QString &text) {
    static const QRegularExpression pmPath = py::re(QStringLiteral(R"re(^package:(.*)=([^ =]+) uid:(\d+)\s*$)re"));
    static const QRegularExpression pmSimple = py::re(QStringLiteral(R"re(^package:([^ =]+) uid:(\d+)\s*$)re"));
    QList<PmPackage> packages;
    for (const QString &raw : py::splitLines(text)) {
        const QString line = py::strip(raw);
        QRegularExpressionMatch m = py::match(pmPath, line);
        if (m.hasMatch()) {
            packages.append({m.captured(2), py::toInt(m.capturedView(3)), m.captured(1)});
            continue;
        }
        m = py::match(pmSimple, line);
        if (m.hasMatch())
            packages.append({m.captured(1), py::toInt(m.capturedView(2)), std::nullopt});
    }
    return packages;
}

OrderedMap<QString, PackageDetails> parsePackageDump(const QString &text) {
    static const QRegularExpression pkgHeader = py::re(QStringLiteral(R"re(^Package \[([^\]]+)\])re"));
    static const QRegularExpression section = py::re(QStringLiteral(R"re(^  ([A-Za-z][^:\n]*):\s*$)re"));
    static const QRegularExpression perm = py::re(QStringLiteral(R"re(^\s{4,}([A-Za-z0-9_.]+)(?::\s*(.*))?$)re"));
    static const QRegularExpression version = py::re(QStringLiteral(R"re(^\s+versionName=(\S+))re"));
    static const QRegularExpression userId = py::re(QStringLiteral(R"re(^\s+userId=(\d+))re"));

    OrderedMap<QString, PackageDetails> packages;
    std::optional<PackageDetails> current;
    std::optional<QString> currentSection;
    QStringList body;

    auto finish = [&] {
        if (current) {
            current->body = body.join(u'\n');
            packages.insert(current->name, *current);
        }
        current.reset();
        body.clear();
    };

    for (const QString &raw : py::splitLines(text)) {
        // The header and the indentation checks only look at the first
        // characters; skip the regexes when they cannot match.
        if (raw.startsWith(QLatin1String("Package ["))) {
            const QRegularExpressionMatch header = py::match(pkgHeader, raw);
            if (header.hasMatch()) {
                finish();
                current = PackageDetails();
                current->name = header.captured(1);
                currentSection.reset();
                body.clear();
                continue;
            }
        }
        if (!current)
            continue;
        if (!raw.isEmpty() && !raw.startsWith(u' ') && !raw.startsWith(u'\t')) {
            finish();
            continue;
        }
        body.append(raw);
        QRegularExpressionMatch m = py::match(version, raw);
        if (m.hasMatch())
            current->versionName = m.captured(1);
        m = py::match(userId, raw);
        if (m.hasMatch() && !current->userId)
            current->userId = py::toInt(m.capturedView(1));
        m = py::match(section, raw);
        if (m.hasMatch()) {
            currentSection = m.captured(1);
            continue;
        }
        if (!currentSection)
            continue;
        const QString &name = *currentSection;
        const bool requested = name == QLatin1String("requested permissions");
        if (!requested && name != QLatin1String("install permissions") && name != QLatin1String("runtime permissions"))
            continue;
        m = py::match(perm, raw);
        if (!m.hasMatch())
            continue;
        const QString permission = m.captured(1);
        QString rest = m.captured(2);
        if (requested)
            current->requested.append(permission);
        else if (rest.remove(u' ').contains(QLatin1String("granted=true")))
            current->granted.append(permission);
    }
    finish();
    return packages;
}

// ---------------------------------------------------------------- activity

QList<ProcessInfo> parseProcesses(const QString &text) {
    static const QRegularExpression proc = py::re(QStringLiteral(R"re(ProcessRecord\{[0-9a-fA-F]+\s+(\d+):([^/\s}]+))re"));
    static const QRegularExpression procUid = py::re(QStringLiteral(R"re(\bUID\s+(\d+)\b)re"));
    static const QRegularExpression uidField = py::re(QStringLiteral(R"re(\buid=(\d+))re"));
    static const QRegularExpression packageList = py::re(QStringLiteral(R"re(packageList=\{([^}]*)\})re"));
    QList<ProcessInfo> processes;
    for (const QString &raw : py::splitLines(text)) {
        const QRegularExpressionMatch m = proc.match(raw);
        if (m.hasMatch()) {
            ProcessInfo info;
            info.pid = py::toInt(m.capturedView(1));
            info.name = m.captured(2);
            info.uid = intOpt(procUid.match(raw), 1);
            processes.append(info);
            continue;
        }
        if (processes.isEmpty())
            continue;
        ProcessInfo &current = processes.last();
        const QRegularExpressionMatch uid = uidField.match(raw);
        if (uid.hasMatch() && !current.uid)
            current.uid = py::toInt(uid.capturedView(1));
        const QRegularExpressionMatch listed = packageList.match(raw);
        if (listed.hasMatch()) {
            current.packages.clear();
            for (const QString &part : listed.captured(1).split(u','))
                if (!py::strip(part).isEmpty())
                    current.packages.append(py::strip(part));
        }
        if (raw.contains(QLatin1String("foregroundActivities=true")))
            current.foregroundActivity = true;
        if (raw.contains(QLatin1String("foregroundServices=true")))
            current.foregroundService = true;
    }
    return processes;
}

QList<ServiceInfo> parseServices(const QString &text) {
    static const QRegularExpression service = py::re(QStringLiteral(R"re(ServiceRecord\{([^}]*)\})re"));
    static const QRegularExpression component = py::re(QStringLiteral(R"re(([\w.$]+)/(\S+))re"));
    static const QRegularExpression packageName = py::re(QStringLiteral(R"re(\bpackageName=([\w.$]+))re"));
    static const QRegularExpression foreground = py::re(QStringLiteral(R"re((?<![\w])foreground=true)re"));
    QList<ServiceInfo> services;
    bool active = false;
    for (const QString &raw : py::splitLines(text)) {
        const QRegularExpressionMatch m = service.match(raw);
        if (m.hasMatch()) {
            const QRegularExpressionMatch c = component.match(m.captured(1));
            if (c.hasMatch()) {
                const QString package = c.captured(1);
                services.append({package, package + u'/' + c.captured(2), false});
                active = true;
            } else {
                active = false;
            }
            continue;
        }
        if (!active)
            continue;
        ServiceInfo &current = services.last();
        const QRegularExpressionMatch named = packageName.match(raw);
        if (named.hasMatch())
            current.package = named.captured(1);
        if (raw.contains(QLatin1String("isForeground=true")) || foreground.match(raw).hasMatch())
            current.foreground = true;
    }
    return services;
}

QList<JobInfo> parseJobs(const QString &text) {
    static const QRegularExpression job = py::re(QStringLiteral(R"re(^\s*JOB #\S+:\s+(?:\d+\s+)?(\S+))re"));
    static const QRegularExpression jobPkg = py::re(QStringLiteral(R"re(\bpkg=([\w.$]+))re"));
    QList<JobInfo> jobs;
    for (const QString &raw : py::splitLines(text)) {
        const QRegularExpressionMatch m = py::match(job, raw);
        if (m.hasMatch()) {
            const QString component = m.captured(1);
            jobs.append({component.section(u'/', 0, 0), component});
            continue;
        }
        if (jobs.isEmpty())
            continue;
        const QRegularExpressionMatch named = jobPkg.match(raw);
        if (named.hasMatch())
            jobs.last().package = named.captured(1);
    }
    return jobs;
}

QList<WakeInfo> parseWakelocks(const QString &text) {
    static const QRegularExpression wakeKind = py::re(QStringLiteral(
        R"re((PARTIAL_WAKE_LOCK|FULL_WAKE_LOCK|SCREEN_BRIGHT_WAKE_LOCK|SCREEN_DIM_WAKE_LOCK|DOZE_WAKE_LOCK)\s+'([^']+)')re"));
    static const QRegularExpression wakeUid = py::re(QStringLiteral(R"re(\(uid=(\d+))re"));
    static const QRegularExpression workSource = py::re(QStringLiteral(R"re(WorkSource\{(\d+)\s+([\w.$]+))re"));
    QList<WakeInfo> wakes;
    for (const QString &raw : py::splitLines(text)) {
        const QRegularExpressionMatch kind = wakeKind.match(raw);
        if (!kind.hasMatch())
            continue;
        const QRegularExpressionMatch uid = wakeUid.match(raw);
        const QRegularExpressionMatch source = workSource.match(raw);
        WakeInfo wake;
        wake.name = kind.captured(2);
        wake.kind = kind.captured(1);
        wake.uid = uid.hasMatch() ? intOpt(uid, 1) : intOpt(source, 1);
        if (source.hasMatch())
            wake.package = source.captured(2);
        wakes.append(wake);
    }
    return wakes;
}

QList<ActivityInfo> parseResumed(const QString &text) {
    static const QRegularExpression activity = py::re(QStringLiteral(R"re(ActivityRecord\{[^}]*\s([\w.$]+)/([^\s}]+))re"));
    QList<ActivityInfo> found;
    QSet<QPair<QString, QString>> seen;
    for (const QString &raw : py::splitLines(text)) {
        if (!raw.contains(QLatin1String("ResumedActivity")) && !raw.contains(QLatin1String("Resumed:")))
            continue;
        const QRegularExpressionMatch m = activity.match(raw);
        if (!m.hasMatch())
            continue;
        const QPair<QString, QString> item(m.captured(1), m.captured(2));
        if (seen.contains(item))
            continue;
        seen.insert(item);
        found.append({item.first, item.second});
    }
    return found;
}

QList<LocationInfo> parseLocation(const QString &text) {
    static const QRegularExpression locPkg = py::re(QStringLiteral(R"re(\b((?:[a-zA-Z][a-zA-Z0-9_]*\.)+[a-zA-Z][a-zA-Z0-9_]*)\((\d+)\))re"));
    static const QRegularExpression locReq = py::re(QStringLiteral(R"re(Request\[([^\]]*)\])re"));
    static const QRegularExpression locNamed = py::re(QStringLiteral(R"re(\b(?:packageName|package)=([a-zA-Z][\w.]*))re"));
    QList<LocationInfo> found;
    QSet<QPair<QString, QString>> seen;
    for (const QString &raw : py::splitLines(text)) {
        QString request;
        const QRegularExpressionMatch req = locReq.match(raw);
        if (req.hasMatch())
            request = py::strip(req.captured(1));
        QList<QPair<QString, std::optional<qint64>>> pairs;
        for (auto it = locPkg.globalMatch(raw); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            pairs.append({m.captured(1), py::toInt(m.capturedView(2))});
        }
        for (auto it = locNamed.globalMatch(raw); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            pairs.append({rstripChars(m.captured(1), QStringLiteral(".,")), std::nullopt});
        }
        const QString lower = raw.toLower();
        if (pairs.isEmpty() || (!raw.contains(QLatin1String("Request")) && !lower.contains(QLatin1String("listener")) &&
                                !lower.contains(QLatin1String("provider"))))
            continue;
        for (const auto &pair : pairs) {
            const QPair<QString, QString> key(pair.first, request);
            if (seen.contains(key))
                continue;
            seen.insert(key);
            found.append({pair.first, pair.second, request});
        }
    }
    return found;
}

QList<SensorInfo> parseSensors(const QString &text) {
    static const QRegularExpression conn = py::re(QStringLiteral(
        R"re(^\s*([a-zA-Z][\w.]*)\s+\|\s+WakeLockRefCount\s+\d+\s+\|\s+uid\s+(\d+)\s+\|\s+pid\s+(\d+)\s*$)re"));
    static const QRegularExpression sensorName = py::re(QStringLiteral(R"re(^\s*Sensor:\s+(\S+)\s*$)re"));
    QList<SensorInfo> found;
    for (const QString &raw : py::splitLines(text)) {
        const QRegularExpressionMatch m = py::match(conn, raw);
        if (m.hasMatch()) {
            found.append({m.captured(1), py::toInt(m.capturedView(2)), py::toInt(m.capturedView(3)), {}});
            continue;
        }
        const QRegularExpressionMatch sensor = py::match(sensorName, raw);
        if (sensor.hasMatch() && !found.isEmpty())
            found.last().sensors.append(sensor.captured(1));
    }
    return found;
}

std::optional<QString> parseClipboard(const QString &text) {
    static const QRegularExpression clip = py::re(QStringLiteral(R"re(mPrimaryClipOwner(?:Package)?[=:\s]+([a-zA-Z][\w.]*))re"));
    const QRegularExpressionMatch m = clip.match(text);
    if (!m.hasMatch())
        return std::nullopt;
    return m.captured(1);
}

// ---------------------------------------------------------------- sockets

namespace {

// glibc's inet_ntop for AF_INET6, which Python's socket.inet_ntop calls.
QString formatIpv6(const unsigned char bytes[16]) {
    unsigned words[8];
    for (int i = 0; i < 8; ++i)
        words[i] = (unsigned(bytes[2 * i]) << 8) | bytes[2 * i + 1];
    int bestBase = -1, bestLen = 0, curBase = -1, curLen = 0;
    for (int i = 0; i < 8; ++i) {
        if (words[i] == 0) {
            if (curBase == -1) {
                curBase = i;
                curLen = 1;
            } else {
                ++curLen;
            }
        } else if (curBase != -1) {
            if (bestBase == -1 || curLen > bestLen) {
                bestBase = curBase;
                bestLen = curLen;
            }
            curBase = -1;
        }
    }
    if (curBase != -1 && (bestBase == -1 || curLen > bestLen)) {
        bestBase = curBase;
        bestLen = curLen;
    }
    if (bestBase != -1 && bestLen < 2)
        bestBase = -1;
    QString out;
    for (int i = 0; i < 8; ++i) {
        if (bestBase != -1 && i >= bestBase && i < bestBase + bestLen) {
            if (i == bestBase)
                out += u':';
            continue;
        }
        if (i != 0)
            out += u':';
        if (i == 6 && bestBase == 0 && (bestLen == 6 || (bestLen == 5 && words[5] == 0xffff))) {
            out += QStringLiteral("%1.%2.%3.%4").arg(int(bytes[12])).arg(int(bytes[13])).arg(int(bytes[14])).arg(int(bytes[15]));
            return out;
        }
        out += QString::number(words[i], 16);
    }
    if (bestBase != -1 && bestBase + bestLen == 8)
        out += u':';
    return out;
}

} // namespace

std::optional<QString> parseIp(const QString &hex) {
    const QString text = py::strip(hex);
    if (text.size() % 2 != 0)
        return std::nullopt;
    QByteArray raw;
    for (qsizetype i = 0; i < text.size(); i += 2) {
        bool ok = false;
        const int byte = text.mid(i, 2).toInt(&ok, 16);
        if (!ok)
            return std::nullopt;
        raw.append(char(byte));
    }
    if (raw.size() == 4) {
        const auto *b = reinterpret_cast<const unsigned char *>(raw.constData());
        return QStringLiteral("%1.%2.%3.%4").arg(int(b[3])).arg(int(b[2])).arg(int(b[1])).arg(int(b[0]));
    }
    if (raw.size() == 16) {
        unsigned char ordered[16];
        for (int group = 0; group < 4; ++group)
            for (int j = 0; j < 4; ++j)
                ordered[group * 4 + j] = static_cast<unsigned char>(raw[group * 4 + 3 - j]);
        return formatIpv6(ordered);
    }
    return std::nullopt;
}

QList<SocketInfo> parseProcNet(const QString &text, const QString &proto) {
    static const QRegularExpression sock = py::re(QStringLiteral(
        R"re(^\s*\d+:\s+([0-9A-Fa-f]+):([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+):([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+\S+\s+\S+\s+\S+\s+(\d+))re"));
    static const QHash<QString, QString> states = {
        {"01", "ESTABLISHED"}, {"02", "SYN_SENT"}, {"03", "SYN_RECV"}, {"04", "FIN_WAIT1"},
        {"05", "FIN_WAIT2"}, {"06", "TIME_WAIT"}, {"07", "CLOSE"}, {"08", "CLOSE_WAIT"},
        {"09", "LAST_ACK"}, {"0A", "LISTEN"}, {"0B", "CLOSING"},
    };
    QList<SocketInfo> sockets;
    for (const QString &raw : py::splitLines(text)) {
        const QRegularExpressionMatch m = py::match(sock, raw);
        if (!m.hasMatch())
            continue;
        const QString code = m.captured(5).toUpper();
        const QString state = states.value(code, code);
        if (state == QLatin1String("LISTEN"))
            continue;
        const std::optional<QString> localIp = parseIp(m.captured(1));
        const std::optional<QString> remoteIp = parseIp(m.captured(3));
        if (!localIp || !remoteIp)
            continue;
        const qint64 remotePort = m.captured(4).toLongLong(nullptr, 16);
        if (remotePort == 0)
            continue;
        SocketInfo info;
        info.proto = proto;
        info.state = state;
        info.localIp = *localIp;
        info.localPort = m.captured(2).toLongLong(nullptr, 16);
        info.remoteIp = *remoteIp;
        info.remotePort = remotePort;
        info.uid = py::toInt(m.capturedView(6));
        sockets.append(info);
    }
    return sockets;
}

// ---------------------------------------------------------------- netstats

namespace {

// Android 17's BPF table, mAppUidStatsMap: one total per uid.
QList<UidTraffic> parseAppUidStatsMap(const QStringList &lines) {
    static const QRegularExpression row = py::re(QStringLiteral(R"re(^\s*(\d+)\s+(\d+)\s+\d+\s+(\d+)\s+\d+\s*$)re"));
    OrderedMap<qint64, UidTraffic> totals;
    bool inMap = false;
    for (const QString &raw : lines) {
        const QString stripped = py::strip(raw);
        if (stripped == QLatin1String("uid rxBytes rxPackets txBytes txPackets")) {
            inMap = true;
            continue;
        }
        if (!inMap)
            continue;
        const QRegularExpressionMatch m = py::match(row, raw);
        if (!m.hasMatch()) {
            if (!stripped.isEmpty())
                inMap = false;
            continue;
        }
        const qint64 uid = py::toInt(m.capturedView(1));
        totals[uid] = {uid, py::toInt(m.capturedView(2)), py::toInt(m.capturedView(3))};
    }
    QList<UidTraffic> out;
    for (const auto &item : totals)
        out.append(item.second);
    return out;
}

// Android 14's UID stats history; the UID tag stats section is left out.
QList<UidTraffic> parseUidStatsHistory(const QStringList &lines) {
    static const QRegularExpression nsUid = py::re(QStringLiteral(R"re(\buid=(\d+)\b)re"));
    static const QRegularExpression nsBucket = py::re(QStringLiteral(R"re(\bbucketStart=\d+\b.*\brxBytes=(\d+)\b.*\btxBytes=(\d+)\b)re"));
    static const QRegularExpression nsShort = py::re(QStringLiteral(R"re(\bst=\d+\b.*\brb=(\d+)\b.*\btb=(\d+)\b)re"));
    OrderedMap<qint64, QPair<qint64, qint64>> bucket;
    OrderedMap<qint64, QPair<qint64, qint64>> shortForm;
    bool inUid = false;
    std::optional<qint64> current;
    for (const QString &raw : lines) {
        const QString stripped = py::strip(raw);
        if (stripped.startsWith(QLatin1String("UID tag stats"))) {
            inUid = false;
            current.reset();
            continue;
        }
        if (stripped.startsWith(QLatin1String("UID stats"))) {
            inUid = true;
            current.reset();
            continue;
        }
        if (!inUid)
            continue;
        const QRegularExpressionMatch uid = nsUid.match(raw);
        if (uid.hasMatch())
            current = py::toInt(uid.capturedView(1));
        if (!current)
            continue;
        const QRegularExpressionMatch b = nsBucket.match(raw);
        if (b.hasMatch()) {
            auto &sum = bucket[*current];
            sum.first += py::toInt(b.capturedView(1));
            sum.second += py::toInt(b.capturedView(2));
            continue;
        }
        const QRegularExpressionMatch s = nsShort.match(raw);
        if (s.hasMatch()) {
            auto &sum = shortForm[*current];
            sum.first += py::toInt(s.capturedView(1));
            sum.second += py::toInt(s.capturedView(2));
        }
    }
    // Python walks set(bucket) | set(short); keep that order.
    QList<UidTraffic> out;
    for (qint64 uid : py::setUnionOrder(bucket.keys(), shortForm.keys())) {
        const auto *b = bucket.find(uid);
        if (b && (b->first || b->second)) {
            out.append({uid, b->first, b->second});
        } else {
            const auto *s = shortForm.find(uid);
            out.append({uid, s ? s->first : 0, s ? s->second : 0});
        }
    }
    return out;
}

} // namespace

QList<UidTraffic> parseNetstats(const QString &text) {
    const QStringList lines = py::splitLines(text);
    const QList<UidTraffic> mapped = parseAppUidStatsMap(lines);
    if (!mapped.isEmpty())
        return mapped;
    return parseUidStatsHistory(lines);
}

// ---------------------------------------------------------------- dns

QList<DnsInfo> parseDns(const QString &text) {
    static const QRegularExpression dnsUid = py::re(QStringLiteral(R"re(\buid[=: ]+(\d+)\b)re"), true);
    static const QRegularExpression dnsHost = py::re(
        QStringLiteral(R"re(\b(?:host|hostname|query|qname)[=: ]+([a-zA-Z0-9.-]+\.[a-zA-Z]{2,})\b)re"), true);
    static const QRegularExpression dnsResolve = py::re(QStringLiteral(R"re(\b(?:resolving|lookup)\s+([a-zA-Z0-9.-]+\.[a-zA-Z]{2,})\b)re"), true);
    QList<DnsInfo> found;
    QSet<QPair<QString, qint64>> seen;
    QSet<QString> seenNoUid;
    for (const QString &raw : py::splitLines(text)) {
        QRegularExpressionMatch m = dnsHost.match(raw);
        if (!m.hasMatch())
            m = dnsResolve.match(raw);
        if (!m.hasMatch())
            continue;
        const QString host = rstripChars(m.captured(1), QStringLiteral(".")).toLower();
        const std::optional<qint64> uid = intOpt(dnsUid.match(raw), 1);
        if (uid) {
            if (seen.contains({host, *uid}))
                continue;
            seen.insert({host, *uid});
        } else {
            if (seenNoUid.contains(host))
                continue;
            seenNoUid.insert(host);
        }
        found.append({host, uid});
    }
    return found;
}
