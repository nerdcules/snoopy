#pragma once

#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <optional>
#include <tuple>
#include <utility>

// Parsers for the dumps Android already keeps about each app, as
// android-diagnoser's parsers.py and uids.py (7b2d820). Each parser takes the
// raw text of one source and returns rows; lines it does not recognise are
// skipped so a newer dumpsys does not abort a scan.

// Text helpers that behave like the Python str and re functions the
// reference uses, so the port gives the same rows for the same text.
namespace py {

// str.splitlines(): breaks on \n, \r, \r\n, \v, \f, \x1c-\x1e, \x85, U+2028
// and U+2029; no empty last line.
QStringList splitLines(const QString &text);
// Universal newlines, as Path.read_text: \r\n and \r become \n.
QString universalNewlines(const QString &text);
// str.isspace() for one code point, and str.strip().
bool isSpace(char32_t c);
QString strip(const QString &text);
// int() of a run of decimal digits (any Unicode Nd digit, as \d matches).
qint64 toInt(QStringView digits);
// Python's string order: by code point, not by UTF-16 unit.
int compare(const QString &a, const QString &b);
inline bool less(const QString &a, const QString &b) { return compare(a, b) < 0; }
// str() of a JSON value as Python would print it (None, True, 10245, ...).
QString str(const QJsonValue &value);
// Python truthiness of a JSON value.
bool truthy(const QJsonValue &value);

// A pattern compiled the way Python's re treats a str pattern (Unicode \w,
// \d, \s, \b).
QRegularExpression re(const QString &pattern, bool ignoreCase = false);
// re.match: anchored at the start.
QRegularExpressionMatch match(const QRegularExpression &re, const QString &text);
// re.fullmatch.
bool fullMatch(const QRegularExpression &re, const QString &text);

// The order CPython 3.14 iterates set(a) | set(b) for non-negative ints,
// where a and b are each iterated in the order given (dict insertion order).
QList<qint64> setUnionOrder(const QList<qint64> &a, const QList<qint64> &b);

} // namespace py

// A dict: keys stay in first-insertion order, assignment keeps the position.
template <typename K, typename V>
class OrderedMap {
public:
    bool contains(const K &key) const { return m_index.contains(key); }
    V &operator[](const K &key) {
        auto found = m_index.constFind(key);
        if (found != m_index.cend())
            return m_values[*found].second;
        m_index.insert(key, m_values.size());
        m_values.emplace_back(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());
        return m_values.last().second;
    }
    const V *find(const K &key) const {
        auto found = m_index.constFind(key);
        return found == m_index.cend() ? nullptr : &m_values[*found].second;
    }
    V *find(const K &key) {
        auto found = m_index.constFind(key);
        return found == m_index.cend() ? nullptr : &m_values[*found].second;
    }
    void insert(const K &key, const V &value) { (*this)[key] = value; }
    qsizetype size() const { return m_values.size(); }
    bool isEmpty() const { return m_values.isEmpty(); }
    QList<K> keys() const {
        QList<K> out;
        out.reserve(m_values.size());
        for (const auto &item : m_values)
            out.append(item.first);
        return out;
    }
    auto begin() { return m_values.begin(); }
    auto end() { return m_values.end(); }
    auto begin() const { return m_values.cbegin(); }
    auto end() const { return m_values.cend(); }

private:
    QHash<K, qsizetype> m_index;
    QList<std::pair<K, V>> m_values;
};

// u0a245 -> 10245, u0s1000 -> 1000, u10a1 -> 1010001, digits as they are.
std::optional<qint64> parseUid(const QString &token);

struct OpEntry {
    QString package;
    std::optional<qint64> uid;
    QString op;
    std::optional<QString> mode;
    std::optional<QString> lastAccess;
    std::optional<QString> lastReject;
};

struct PmPackage {
    QString name;
    qint64 uid = 0;
    std::optional<QString> apkPath;
};

struct PackageDetails {
    QString name;
    std::optional<qint64> userId;
    std::optional<QString> versionName;
    QStringList granted;
    QStringList requested;
    QString body;
};

struct ProcessInfo {
    qint64 pid = 0;
    QString name;
    std::optional<qint64> uid;
    QStringList packages;
    bool foregroundActivity = false;
    bool foregroundService = false;
};

struct ServiceInfo {
    QString package;
    QString component;
    bool foreground = false;
};

struct JobInfo {
    QString package;
    QString component;
};

struct WakeInfo {
    QString name;
    QString kind;
    std::optional<qint64> uid;
    std::optional<QString> package;
};

struct ActivityInfo {
    QString package;
    QString activity;
};

struct LocationInfo {
    QString package;
    std::optional<qint64> uid;
    QString request;
};

struct SensorInfo {
    QString package;
    std::optional<qint64> uid;
    std::optional<qint64> pid;
    QStringList sensors;
};

struct SocketInfo {
    QString proto;
    QString state;
    QString localIp;
    qint64 localPort = 0;
    QString remoteIp;
    qint64 remotePort = 0;
    qint64 uid = 0;
};

struct DnsInfo {
    QString host;
    std::optional<qint64> uid;
};

struct UidTraffic {
    qint64 uid = 0;
    qint64 rxBytes = 0;
    qint64 txBytes = 0;
};

QList<OpEntry> parseAppops(const QString &text);
QList<PmPackage> parsePmList(const QString &text);
OrderedMap<QString, PackageDetails> parsePackageDump(const QString &text);
QList<ProcessInfo> parseProcesses(const QString &text);
QList<ServiceInfo> parseServices(const QString &text);
QList<JobInfo> parseJobs(const QString &text);
QList<WakeInfo> parseWakelocks(const QString &text);
QList<ActivityInfo> parseResumed(const QString &text);
QList<LocationInfo> parseLocation(const QString &text);
QList<SensorInfo> parseSensors(const QString &text);
std::optional<QString> parseClipboard(const QString &text);
// Little-endian hex from /proc/net as an address; nullopt when it is not 4
// or 16 bytes of hex.
std::optional<QString> parseIp(const QString &hex);
QList<SocketInfo> parseProcNet(const QString &text, const QString &proto);
// uid -> (rx, tx), in the order Python's dict holds them.
QList<UidTraffic> parseNetstats(const QString &text);
QList<DnsInfo> parseDns(const QString &text);
