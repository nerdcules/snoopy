// The launcher-icon reader (icons.cpp, apk.cpp). Every input is built here:
// zips with stored and deflated entries, binary XML, resources.arsc tables,
// and a fake phone that answers the adb commands icons.py uses from
// in-memory files.
#include <QtConcurrent>
#include <QtTest>

#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <zlib.h>

#include "apk.h"
#include "icons.h"
#include "tests.h"

namespace {

// ---- little-endian building blocks ----

void le16(QByteArray &out, quint32 value) {
    out.append(char(value & 0xFF));
    out.append(char((value >> 8) & 0xFF));
}

void le32(QByteArray &out, quint32 value) {
    le16(out, value & 0xFFFF);
    le16(out, value >> 16);
}

// A resource chunk: type, header size, total size, the rest of the header,
// then the body.
QByteArray chunk(quint16 type, const QByteArray &headerRest, const QByteArray &body) {
    QByteArray out;
    le16(out, type);
    le16(out, quint32(8 + headerRest.size()));
    le32(out, quint32(8 + headerRest.size() + body.size()));
    return out + headerRest + body;
}

// A ResStringPool. padding adds unused bytes inside the chunk, to make a
// table large without changing what it says.
QByteArray stringPool(const QStringList &strings, bool utf8, int padding = 0) {
    QByteArray offsets, data;
    for (const QString &text : strings) {
        le32(offsets, quint32(data.size()));
        if (utf8) {
            const QByteArray bytes = text.toUtf8();
            const auto length = [&](int n) {
                if (n > 0x7F) {
                    data.append(char(0x80 | (n >> 8)));
                    data.append(char(n & 0xFF));
                } else {
                    data.append(char(n));
                }
            };
            length(int(text.size()));
            length(int(bytes.size()));
            data += bytes;
            data.append('\0');
        } else {
            const int n = int(text.size());
            if (n > 0x7FFF) {
                le16(data, 0x8000 | (n >> 16));
                le16(data, n & 0xFFFF);
            } else {
                le16(data, n);
            }
            data.append(reinterpret_cast<const char *>(text.utf16()), n * 2);
            le16(data, 0);
        }
    }
    while (data.size() % 4)
        data.append('\0');
    QByteArray header;
    le32(header, quint32(strings.size()));
    le32(header, 0);
    le32(header, utf8 ? 0x100 : 0);
    le32(header, quint32(28 + offsets.size()));
    le32(header, 0);
    return chunk(0x0001, header, offsets + data + QByteArray(padding, '\0'));
}

// ---- binary XML ----

struct XmlAttribute {
    QString name;
    quint8 type; // 0x01 reference, 0x03 string, 0x10 int
    quint32 data;
};

struct XmlElement {
    QString name;
    QList<XmlAttribute> attributes;
};

// Elements nest in order: the first is the root, each next one inside the
// one before.
QByteArray binaryXml(const QStringList &strings, const QList<quint32> &resourceMap, const QList<XmlElement> &elements,
                     bool utf8 = false) {
    const auto index = [&](const QString &text) {
        const qsizetype at = strings.indexOf(text);
        Q_ASSERT(at >= 0);
        return quint32(at);
    };
    QByteArray body = stringPool(strings, utf8);
    if (!resourceMap.isEmpty()) {
        QByteArray map;
        for (quint32 id : resourceMap)
            le32(map, id);
        body += chunk(0x0180, {}, map);
    }
    QByteArray lineAndComment;
    le32(lineAndComment, 1);
    le32(lineAndComment, 0xFFFFFFFF);
    for (const XmlElement &element : elements) {
        QByteArray ext;
        le32(ext, 0xFFFFFFFF);
        le32(ext, index(element.name));
        le16(ext, 20);
        le16(ext, 20);
        le16(ext, quint32(element.attributes.size()));
        le16(ext, 0);
        le16(ext, 0);
        le16(ext, 0);
        for (const XmlAttribute &attribute : element.attributes) {
            le32(ext, 0xFFFFFFFF);
            le32(ext, index(attribute.name));
            le32(ext, attribute.type == 0x03 ? attribute.data : 0xFFFFFFFF);
            le16(ext, 8);
            ext.append('\0');
            ext.append(char(attribute.type));
            le32(ext, attribute.data);
        }
        body += chunk(0x0102, lineAndComment, ext);
    }
    for (auto it = elements.crbegin(); it != elements.crend(); ++it) {
        QByteArray ext;
        le32(ext, 0xFFFFFFFF);
        le32(ext, index(it->name));
        body += chunk(0x0103, lineAndComment, ext);
    }
    return chunk(0x0003, {}, body);
}

constexpr quint32 kIcon = 0x7F0D0000;       // mipmap/ic_app
constexpr quint32 kForeground = 0x7F080001; // drawable/fg
constexpr quint32 kBackground = 0x7F060000; // color/bg

// A manifest whose <application> has a label string and android:icon.
QByteArray manifest(quint32 iconId) {
    const QStringList strings = {"label", "icon", "manifest", "application", "package", "Demo", "com.demo"};
    return binaryXml(strings, {0x01010001, 0x01010002},
                     {{"manifest", {{"package", 0x03, 6}}}, {"application", {{"label", 0x03, 5}, {"icon", 0x01, iconId}}}});
}

// ---- resources.arsc ----

struct Value {
    quint8 type;
    quint32 data;
};

enum class Encoding { Dense, Offset16, Sparse };

struct Config {
    quint8 typeId;
    quint16 density;
    Encoding encoding;
    int entryCount;
    QMap<int, Value> entries;
    quint8 uiMode = 0; // 0x20: night
};

QByteArray typeChunk(const Config &config) {
    QByteArray entries;
    QMap<int, int> offsetOf;
    for (auto it = config.entries.cbegin(); it != config.entries.cend(); ++it) {
        offsetOf[it.key()] = int(entries.size());
        le16(entries, 8);
        le16(entries, 0);
        le32(entries, 0);
        le16(entries, 8);
        entries.append('\0');
        entries.append(char(it->type));
        le32(entries, it->data);
    }
    QByteArray offsets;
    quint8 flags = 0;
    quint32 count = quint32(config.entryCount);
    if (config.encoding == Encoding::Dense) {
        for (int i = 0; i < config.entryCount; ++i)
            le32(offsets, offsetOf.contains(i) ? quint32(offsetOf[i]) : 0xFFFFFFFF);
    } else if (config.encoding == Encoding::Offset16) {
        flags = 0x02;
        for (int i = 0; i < config.entryCount; ++i)
            le16(offsets, offsetOf.contains(i) ? quint32(offsetOf[i] / 4) : 0xFFFF);
    } else {
        flags = 0x01;
        count = quint32(offsetOf.size());
        for (auto it = offsetOf.cbegin(); it != offsetOf.cend(); ++it) {
            le16(offsets, quint32(it.key()));
            le16(offsets, quint32(it.value() / 4));
        }
    }
    while (offsets.size() % 4)
        offsets.append('\0');
    QByteArray resConfig(64, '\0');
    resConfig[0] = 64;
    resConfig[14] = char(config.density & 0xFF);
    resConfig[15] = char(config.density >> 8);
    resConfig[29] = char(config.uiMode);
    QByteArray header;
    header.append(char(config.typeId));
    header.append(char(flags));
    le16(header, 0);
    le32(header, count);
    le32(header, quint32(8 + 12 + 64 + offsets.size()));
    header += resConfig;
    return chunk(0x0201, header, offsets + entries);
}

QByteArray table(const QStringList &strings, const QList<Config> &configs, int padding = 0) {
    QByteArray packageHeader;
    le32(packageHeader, 0x7F);
    packageHeader += QByteArray(256, '\0');
    for (int i = 0; i < 5; ++i)
        le32(packageHeader, 0);
    QByteArray body = stringPool({"color", "drawable", "mipmap"}, false) + stringPool({"ic_app", "fg", "bg"}, true);
    QByteArray specHeader;
    specHeader.append(char(0x0D));
    specHeader.append('\0');
    le16(specHeader, 0);
    le32(specHeader, 1);
    QByteArray specFlags;
    le32(specFlags, 0);
    body += chunk(0x0202, specHeader, specFlags);
    for (const Config &config : configs)
        body += typeChunk(config);
    QByteArray tableHeader;
    le32(tableHeader, 1);
    return chunk(0x0002, tableHeader, stringPool(strings, true, padding) + chunk(0x0200, packageHeader, body));
}

// The adaptive-icon case: ic_app is only an XML file whose layers are a
// colour and a drawable that has bitmaps.
const QStringList kAdaptivePaths = {"res/mipmap-anydpi-v26/ic_app.xml", "res/drawable-xxxhdpi-v4/fg.png",
                                    "res/drawable-mdpi-v4/fg.png"};

QByteArray adaptiveTable() {
    return table(kAdaptivePaths, {{0x0D, 65534, Encoding::Dense, 1, {{0, {0x03, 0}}}},
                                  {0x06, 0, Encoding::Dense, 1, {{0, {0x1C, 0xFF336699}}}},
                                  {0x08, 640, Encoding::Offset16, 2, {{1, {0x03, 1}}}},
                                  {0x08, 160, Encoding::Dense, 2, {{1, {0x03, 2}}}}});
}

QByteArray adaptiveXml() {
    return binaryXml({"adaptive-icon", "background", "foreground", "drawable"}, {0x01010199},
                     {{"adaptive-icon", {}}, {"background", {{"drawable", 0x01, kBackground}}},
                      {"foreground", {{"drawable", 0x01, kForeground}}}});
}

// ---- zip ----

struct ZipItem {
    QString name;
    QByteArray data;
    bool deflate = false;
};

QByteArray rawDeflate(const QByteArray &data) {
    // qCompress: a 4-byte length, then a zlib stream (2-byte header, raw
    // deflate, 4-byte Adler-32).
    const QByteArray z = qCompress(data, 9);
    return z.mid(6, z.size() - 10);
}

QByteArray zip(const QList<ZipItem> &items) {
    QByteArray out, directory;
    const QByteArray extra("\0\0\0", 3); // zipalign-style padding in the local header
    for (const ZipItem &item : items) {
        const QByteArray stored = item.deflate ? rawDeflate(item.data) : item.data;
        const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(item.data.constData()), uInt(item.data.size())));
        const QByteArray name = item.name.toUtf8();
        const quint32 offset = quint32(out.size());
        out += QByteArray("PK\x03\x04", 4);
        le16(out, 20);
        le16(out, 0);
        le16(out, item.deflate ? 8 : 0);
        le32(out, 0);
        le32(out, crc);
        le32(out, quint32(stored.size()));
        le32(out, quint32(item.data.size()));
        le16(out, quint32(name.size()));
        le16(out, quint32(extra.size()));
        out += name + extra + stored;

        directory += QByteArray("PK\x01\x02", 4);
        le16(directory, 20);
        le16(directory, 20);
        le16(directory, 0);
        le16(directory, item.deflate ? 8 : 0);
        le32(directory, 0);
        le32(directory, crc);
        le32(directory, quint32(stored.size()));
        le32(directory, quint32(item.data.size()));
        le16(directory, quint32(name.size()));
        le16(directory, 0);
        le16(directory, 0);
        le16(directory, 0);
        le16(directory, 0);
        le32(directory, 0);
        le32(directory, offset);
        directory += name;
    }
    QByteArray end("PK\x05\x06", 4);
    le16(end, 0);
    le16(end, 0);
    le16(end, quint32(items.size()));
    le16(end, quint32(items.size()));
    le32(end, quint32(directory.size()));
    le32(end, quint32(out.size()));
    le16(end, 0);
    return out + directory + end;
}

QByteArray png(const QByteArray &tag) { return QByteArray("\x89PNG\r\n\x1a\n", 8) + tag + QByteArray(3000, 'p'); }

QByteArray webp(const QByteArray &tag) {
    QByteArray out("RIFF");
    le32(out, 0);
    return out + "WEBP" + tag + QByteArray(3000, 'w');
}

QByteArray svgText() { return "<svg xmlns='http://www.w3.org/2000/svg'/>" + QByteArray(500, ' '); }

// ---- the fake phone ----

QString apkPath(const QString &package, const QString &file = "base.apk") {
    return "/data/app/~~r4nd0m==/" + package + "-Zm9v==/" + file;
}

class FakePhone {
public:
    QHash<QString, QByteArray> files;
    QHash<QString, QStringList> packages;
    QString state = "device"; // "device", "unauthorized", or "nostart"
    QSet<QString> failingReads;
    QString toyboxDir; // when set, `shell sh -c ...` runs here with a fake toybox
    bool oldToybox = false; // refuses iflag=skip_bytes,count_bytes, as toybox before 0.8 may
    int delayMs = 0;

    void install(const QString &package, const QList<QPair<QString, QByteArray>> &apks) {
        for (const auto &[file, bytes] : apks) {
            files[apkPath(package, file)] = bytes;
            packages[package].append(apkPath(package, file));
        }
    }

    Runner runner() {
        return [this](const QString &, const QStringList &arguments, int timeoutMs) { return handle(arguments, timeoutMs); };
    }

    struct Call {
        QString serial;
        QStringList arguments;
        int timeoutMs;
    };

    QList<Call> calls() {
        QMutexLocker lock(&m_mutex);
        return m_calls;
    }

    void clearCalls() {
        QMutexLocker lock(&m_mutex);
        m_calls.clear();
    }

    // dd calls, as (skip, count), one byte per block or at byte offsets.
    QList<QPair<qint64, qint64>> reads() {
        QList<QPair<qint64, qint64>> list;
        for (const Call &call : calls()) {
            if (call.arguments.value(0) != "exec-out")
                continue;
            qint64 skip = -1, count = -1;
            for (const QString &argument : call.arguments) {
                if (argument.startsWith("skip="))
                    skip = argument.mid(5).toLongLong();
                else if (argument.startsWith("count="))
                    count = argument.mid(6).toLongLong();
            }
            list.append({skip, count});
        }
        return list;
    }

private:
    ProcessResult handle(QStringList arguments, int timeoutMs) {
        QString serial;
        if (arguments.size() >= 2 && arguments.first() == "-s") {
            serial = arguments.at(1);
            arguments = arguments.mid(2);
        }
        {
            QMutexLocker lock(&m_mutex);
            m_calls.append({serial, arguments, timeoutMs});
        }
        if (delayMs)
            QThread::msleep(delayMs);
        ProcessResult result;
        result.started = true;
        result.code = 0;
        if (arguments == QStringList{"get-state"}) {
            if (state == "nostart")
                return ProcessResult{};
            if (state == "device") {
                result.out = "device\n";
            } else {
                result.err = "error: device " + state.toUtf8() + "\n";
                result.code = 1;
            }
            return result;
        }
        if (arguments.size() == 4 && arguments.mid(0, 3) == QStringList{"shell", "pm", "path"}) {
            for (const QString &path : packages.value(arguments.at(3)))
                result.out += "package:" + path.toUtf8() + "\n";
            result.code = packages.contains(arguments.at(3)) ? 0 : 1;
            return result;
        }
        if (arguments.size() == 4 && arguments.mid(0, 3) == QStringList{"shell", "ls", "-l"}) {
            const QString path = arguments.at(3);
            if (!files.contains(path)) {
                result.err = "ls: " + path.toUtf8() + ": No such file or directory\n";
                result.code = 1;
                return result;
            }
            result.out = QString("-rw-r--r-- 1 system system %1 2026-11-16 10:00 %2\n").arg(files[path].size()).arg(path).toUtf8();
            return result;
        }
        if (arguments.size() == 8 && arguments.mid(0, 3) == QStringList{"exec-out", "toybox", "dd"} &&
            arguments.at(3).startsWith("if=") && arguments.at(4) == "bs=1" && arguments.at(5).startsWith("skip=") &&
            arguments.at(6).startsWith("count=") && arguments.at(7) == "status=none") {
            const QString path = arguments.at(3).mid(3);
            if (!files.contains(path) || failingReads.contains(path)) {
                result.code = 1;
                return result;
            }
            result.out = files[path].mid(arguments.at(5).mid(5).toLongLong(), arguments.at(6).mid(6).toLongLong());
            return result;
        }
        if (arguments.size() == 9 && arguments.mid(0, 3) == QStringList{"exec-out", "toybox", "dd"} &&
            arguments.at(3).startsWith("if=") && arguments.at(4) == "bs=65536" &&
            arguments.at(5) == "iflag=skip_bytes,count_bytes" && arguments.at(6).startsWith("skip=") &&
            arguments.at(7).startsWith("count=") && arguments.at(8) == "status=none") {
            const QString path = arguments.at(3).mid(3);
            if (oldToybox) {
                result.err = "dd: bad iflag skip_bytes\n";
                result.code = 1;
                return result;
            }
            if (!files.contains(path) || failingReads.contains(path)) {
                result.code = 1;
                return result;
            }
            result.out = files[path].mid(arguments.at(6).mid(5).toLongLong(), arguments.at(7).mid(6).toLongLong());
            return result;
        }
        if (arguments.size() == 9 && arguments.mid(0, 3) == QStringList{"shell", "sh", "-c"}) {
            if (toyboxDir.isEmpty())
                return result;
            // The device runs what adb sends: the arguments joined by spaces.
            QProcess process;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert("PATH", toyboxDir + ":" + environment.value("PATH"));
            process.setProcessEnvironment(environment);
            process.start("sh", {"-c", arguments.mid(1).join(' ')});
            process.waitForFinished(timeoutMs);
            result.out = process.readAllStandardOutput();
            result.err = process.readAllStandardError();
            result.code = process.exitCode();
            return result;
        }
        result.code = 1;
        result.err = "unexpected command";
        return result;
    }

    QMutex m_mutex;
    QList<Call> m_calls;
};

// An APK with launcher files at several densities, png and webp, stored and
// deflated, plus files that only look like launchers.
QByteArray launcherApk() {
    return zip({{"AndroidManifest.xml", manifest(kIcon), true},
                {"res/mipmap-hdpi-v4/ic_launcher.png", png("hdpi"), false},
                {"res/mipmap-xhdpi-v4/ic_launcher.webp", webp("xhdpi"), true},
                {"res/mipmap-xxhdpi-v4/ic_launcher.png", png("xxhdpi"), true},
                {"res/mipmap-xxhdpi-v4/ic_launcher_round.png", png("xxhdpi-round"), true},
                {"res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png", png("xxxhdpi-fg"), false},
                {"res/drawable-xxxhdpi-v4/ic_launcher.9.png", png("nine"), false},
                {"res/mipmap-anydpi-v26/ic_launcher.xml", "<adaptive-icon/>", true},
                {"classes.dex", QByteArray(5000, 'd'), true}});
}

// An APK with no ic_launcher file: the icon is in resources.arsc, at three
// densities in three type-chunk encodings, plus an XML for anydpi.
QByteArray manifestApk(bool withXxhdpiBitmap = true) {
    const QStringList strings = {"res/mipmap-mdpi-v4/whatever.png", "res/mipmap-xxhdpi-v4/whatever.png",
                                 "res/mipmap-anydpi-v26/whatever.xml"};
    QList<ZipItem> items = {{"AndroidManifest.xml", manifest(kIcon), true},
                            {"resources.arsc",
                             table(strings, {{0x0D, 160, Encoding::Dense, 3, {{0, {0x03, 0}}}},
                                             {0x0D, 480, Encoding::Offset16, 3, {{0, {0x03, 1}}, {2, {0x10, 7}}}},
                                             {0x0D, 65534, Encoding::Sparse, 1, {{0, {0x03, 2}}}}}),
                             false},
                            {"res/mipmap-mdpi-v4/whatever.png", png("mdpi"), false}};
    if (withXxhdpiBitmap)
        items.append({"res/mipmap-xxhdpi-v4/whatever.png", png("xxhdpi"), true});
    return zip(items);
}

// ---- binary XML trees (drawables) ----

// android.R.attr ids, as aapt2 writes them into the resource map.
constexpr quint32 aDrawable = 0x01010199, aColor = 0x010101a5, aWidth = 0x01010159, aHeight = 0x01010155,
                  aViewportWidth = 0x01010402, aViewportHeight = 0x01010403, aFillColor = 0x01010404, aPathData = 0x01010405,
                  aStrokeColor = 0x01010406, aStrokeWidth = 0x01010407, aStrokeLineCap = 0x0101040b, aFillAlpha = 0x010104cc,
                  aFillType = 0x0101051e, aRotation = 0x01010326, aPivotX = 0x010101b5, aPivotY = 0x010101b6,
                  aTranslateY = 0x0101045b, aScaleX = 0x01010324, aInsetLeft = 0x010101b7, aInsetRight = 0x010101b8,
                  aInsetTop = 0x010101b9, aInsetBottom = 0x010101ba, aSrc = 0x01010119, aType = 0x010101a1,
                  aStartX = 0x01010510, aStartY = 0x01010511, aEndX = 0x01010512, aEndY = 0x01010513, aOffset = 0x01010514,
                  aCenterX = 0x010101a2, aCenterY = 0x010101a3, aGradientRadius = 0x010101a4, aAlpha = 0x0101031f;

struct Attr {
    QString name;
    quint32 id; // 0: not in the resource map
    quint8 type;
    quint32 data;
    QString text; // for type 0x03
};

struct Node {
    QString name;
    QList<Attr> attributes;
    QList<Node> children;
};

quint32 floatBits(float value) {
    quint32 bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

Attr ref(const QString &name, quint32 id, quint32 target) { return {name, id, 0x01, target, {}}; }
Attr argb(const QString &name, quint32 id, quint32 color) { return {name, id, 0x1C, color, {}}; }
Attr flt(const QString &name, quint32 id, float value) { return {name, id, 0x04, floatBits(value), {}}; }
Attr integer(const QString &name, quint32 id, qint32 value) { return {name, id, 0x10, quint32(value), {}}; }
Attr text(const QString &name, quint32 id, const QString &value) { return {name, id, 0x03, 0, value}; }
// A whole number of dp (radix 23p0, unit dip).
Attr dp(const QString &name, quint32 id, int value) { return {name, id, 0x05, (quint32(value) << 8) | 0x01, {}}; }
// A fraction of the bounds (radix 0p23, unit fraction).
Attr fraction(const QString &name, quint32 id, double value) {
    return {name, id, 0x06, (quint32(std::lround(value * (1 << 23))) << 8) | (3 << 4), {}};
}

// A binary XML file for the tree at root, as aapt2 lays it out: attribute
// names with resource ids first in the pool, covered by the resource map.
// withMap false leaves the map out (names only); obfuscate renames every
// attribute (ids only, as a shrunk APK may).
QByteArray axml(const Node &root, bool withMap = true, bool obfuscate = false) {
    QStringList strings;
    QList<quint32> map;
    std::function<void(const Node &)> mapped = [&](const Node &node) {
        for (const Attr &attribute : node.attributes) {
            if (attribute.id && withMap && !map.contains(attribute.id)) {
                strings.append(obfuscate ? QString("x%1").arg(map.size()) : attribute.name);
                map.append(attribute.id);
            }
        }
        for (const Node &child : node.children)
            mapped(child);
    };
    mapped(root);
    const auto index = [&](const QString &value) {
        qsizetype at = strings.indexOf(value);
        if (at < 0) {
            strings.append(value);
            at = strings.size() - 1;
        }
        return quint32(at);
    };
    const auto nameIndex = [&](const Attr &attribute) {
        if (attribute.id && withMap)
            return quint32(map.indexOf(attribute.id));
        return index(attribute.name);
    };
    QByteArray elements;
    QByteArray lineAndComment;
    le32(lineAndComment, 1);
    le32(lineAndComment, 0xFFFFFFFF);
    std::function<void(const Node &)> writeNode = [&](const Node &node) {
        QByteArray ext;
        le32(ext, 0xFFFFFFFF);
        le32(ext, index(node.name));
        le16(ext, 20);
        le16(ext, 20);
        le16(ext, quint32(node.attributes.size()));
        le16(ext, 0);
        le16(ext, 0);
        le16(ext, 0);
        for (const Attr &attribute : node.attributes) {
            const quint32 data = attribute.type == 0x03 ? index(attribute.text) : attribute.data;
            le32(ext, 0xFFFFFFFF);
            le32(ext, nameIndex(attribute));
            le32(ext, attribute.type == 0x03 ? data : 0xFFFFFFFF);
            le16(ext, 8);
            ext.append('\0');
            ext.append(char(attribute.type));
            le32(ext, data);
        }
        elements += chunk(0x0102, lineAndComment, ext);
        for (const Node &child : node.children)
            writeNode(child);
        QByteArray end;
        le32(end, 0xFFFFFFFF);
        le32(end, index(node.name));
        elements += chunk(0x0103, lineAndComment, end);
    };
    writeNode(root);
    QByteArray body = stringPool(strings, false);
    if (!map.isEmpty()) {
        QByteArray bytes;
        for (quint32 id : map)
            le32(bytes, id);
        body += chunk(0x0180, {}, bytes);
    }
    return chunk(0x0003, {}, body + elements);
}

// A <vector> of viewport x viewport (and as many dp) holding children.
Node vector(int viewport, const QList<Node> &children) {
    return {"vector",
            {dp("width", aWidth, viewport), dp("height", aHeight, viewport), flt("viewportWidth", aViewportWidth, viewport),
             flt("viewportHeight", aViewportHeight, viewport)},
            children};
}

Node path(const QString &d, const Attr &fill, const QList<Attr> &more = {}) {
    return {"path", QList<Attr>{text("pathData", aPathData, d), fill} + more, {}};
}

Node adaptive(const Attr &background, const Attr &foreground) {
    return {"adaptive-icon", {}, {{"background", {background}, {}}, {"foreground", {foreground}, {}}, {"monochrome", {foreground}, {}}}};
}

// A real PNG: one colour, size x size.
QByteArray solidPng(QRgb color, int size = 48) {
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return out;
}

// Resources for apk::renderIcon from a table and files in memory.
struct Memory {
    QByteArray table;
    QHash<QString, QByteArray> files;
    QStringList asked;
    QList<QStringList> prefetched;

    apk::Resources resources() {
        apk::Resources resources;
        resources.values = [this](quint32 id) { return apk::tableValues(table, id); };
        resources.file = [this](const QString &path) {
            asked.append(path);
            return files.value(path);
        };
        resources.prefetch = [this](const QStringList &paths) { prefetched.append(paths); };
        return resources;
    }
};

// The table every drawing test uses: mipmap 0x7F0D0000 (the icon) and the
// drawables 0x7F080000.. are file paths or references; colours 0x7F060000..
// are colour values, references, or files.
struct Entry {
    quint8 type;
    int index;
    Value value;
};

QByteArray drawTable(const QStringList &strings, const QList<Entry> &entries, quint16 density = 0) {
    QMap<quint8, QMap<int, Value>> byType;
    for (const Entry &entry : entries)
        byType[entry.type][entry.index] = entry.value;
    QList<Config> configs;
    for (auto it = byType.cbegin(); it != byType.cend(); ++it) {
        const int count = it.value().isEmpty() ? 0 : it.value().lastKey() + 1;
        configs.append({it.key(), it.key() == 0x0D ? quint16(65534) : density, Encoding::Dense, count, it.value()});
    }
    return table(strings, configs);
}

QRgb pixelAt(const QImage &image, double x, double y) { return image.pixel(int(x), int(y)); }

// Same colour within tolerance per channel (antialiasing and gradient steps).
bool near(QRgb actual, QRgb expected, int tolerance = 6) {
    return std::abs(qRed(actual) - qRed(expected)) <= tolerance && std::abs(qGreen(actual) - qGreen(expected)) <= tolerance &&
           std::abs(qBlue(actual) - qBlue(expected)) <= tolerance && std::abs(qAlpha(actual) - qAlpha(expected)) <= tolerance;
}

QString hex(QRgb color) { return QString("#%1").arg(color, 8, 16, QLatin1Char('0')); }

#define QVERIFY_COLOR(actual, expected)                                                                                      \
    QVERIFY2(near((actual), (expected)), qPrintable(hex(actual) + " != " + hex(expected)))

QStringList cacheFiles(const QString &directory) { return QDir(directory).entryList(QDir::Files); }

class IconTests : public QObject {
    Q_OBJECT

    QTemporaryDir *m_temporary = nullptr;
    QString m_cache;

private slots:
    void init() {
        m_temporary = new QTemporaryDir;
        QVERIFY(m_temporary->isValid());
        m_cache = m_temporary->path() + "/icons";
    }

    void cleanup() {
        delete m_temporary;
        m_temporary = nullptr;
    }

    // ---- apk.cpp ----

    void launcherRanking() {
        QCOMPARE(apk::rank("res/mipmap-xxxhdpi-v4/ic_launcher.png"), 48);
        QCOMPARE(apk::rank("res/mipmap-xxhdpi-v4/ic_launcher.webp"), 38);
        QCOMPARE(apk::rank("res/mipmap-xxhdpi-v4/ic_launcher_round.png"), 32);
        // "foreground" contains "round": +2 and -4, as in icons.py's _rank.
        QCOMPARE(apk::rank("res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png"), 38);
        QCOMPARE(apk::rank("res/mipmap-hdpi/ic_launcher.png"), 18);
        QCOMPARE(apk::rank("res/mipmap/ic_launcher.png"), 8);
        QVERIFY(apk::isLauncher("res/a/IC_LAUNCHER.PNG"));
        QVERIFY(!apk::isLauncher("res/a/ic_launcher.9.png"));
        QVERIFY(!apk::isLauncher("res/a/ic_launcher.xml"));
        QVERIFY(!apk::isLauncher("res/ic_launcher/app.png"));
        QCOMPARE(apk::chooseLauncher({"res/mipmap-hdpi-v4/ic_launcher.png", "res/mipmap-xxhdpi-v4/ic_launcher.png",
                                      "res/mipmap-xxhdpi-v4/ic_launcher_round.png",
                                      "res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png", "res/mipmap-xxxhdpi/ic_launcher.9.png"}),
                 QString("res/mipmap-xxhdpi-v4/ic_launcher.png"));
        // Equal ranks: the first listed wins, as Python's stable sort. So an
        // xxxhdpi foreground layer listed first beats the xxhdpi icon (38 each).
        QCOMPARE(apk::chooseLauncher({"res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png", "res/mipmap-xxhdpi-v4/ic_launcher.png"}),
                 QString("res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png"));
        QCOMPARE(apk::chooseLauncher({"b/xhdpi/ic_launcher.webp", "a/xhdpi/ic_launcher.png"}), QString("b/xhdpi/ic_launcher.webp"));
        QCOMPARE(apk::chooseLauncher({"res/a.png", "res/ic_launcher.xml"}), QString());
    }

    void bestImageSkipsXmlNinePatchAndAnydpi() {
        QCOMPARE(apk::bestImage({{160, "res/a.png"}, {480, "res/b.webp"}, {65534, "res/c.png"}, {640, "res/d.xml"},
                                 {640, "res/e.9.png"}, {640, "@7f080001"}}),
                 QString("res/b.webp"));
        QCOMPARE(apk::bestImage({{480, "res/b.png"}, {480, "res/a.png"}}), QString("res/b.png"));
        QCOMPARE(apk::bestImage({{65535, "res/nodpi.png"}, {0, "res/x.xml"}}), QString());
        // Shrunk resource paths without an extension (Edge, Android 17).
        QCOMPARE(apk::bestImage({{160, "res/HxS"}, {640, "res/w5-"}, {65534, "res/xyc.xml"}}), QString("res/w5-"));
        QCOMPARE(apk::bestImage({{640, "@7f080001"}, {640, "res/a.xml"}}), QString());
    }

    void mimeSniffing() {
        QCOMPARE(apk::mime(png("x")), QString("image/png"));
        QCOMPARE(apk::mime(webp("x")), QString("image/webp"));
        QCOMPARE(apk::mime("RIFF0000WAVE"), QString("image/svg+xml"));
        QCOMPARE(apk::mime(svgText()), QString("image/svg+xml"));
        QCOMPARE(apk::mime({}), QString("image/svg+xml"));
    }

    void zipDirectoryStoredAndDeflated() {
        const QByteArray stored = png("stored");
        const QByteArray deflated = webp("deflated");
        const QByteArray bytes = zip({{"a/stored.png", stored, false}, {"b/deflated.webp", deflated, true}});
        const apk::Eocd eocd = apk::findEocd(bytes.right(100));
        QVERIFY(eocd.found);
        const QList<apk::ZipEntry> entries = apk::centralDirectory(bytes.mid(eocd.directoryOffset, eocd.directorySize));
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].name, QString("a/stored.png"));
        QCOMPARE(entries[0].method, quint16(0));
        QCOMPARE(entries[0].compressedSize, quint32(stored.size()));
        QCOMPARE(entries[1].name, QString("b/deflated.webp"));
        QCOMPARE(entries[1].method, quint16(8));
        QVERIFY(entries[1].compressedSize < entries[1].uncompressedSize);

        const int offset = apk::localDataOffset(bytes.mid(entries[1].localOffset, 30), true);
        QCOMPARE(offset, 30 + 15 + 3);
        bool ok = false;
        QCOMPARE(apk::inflateRaw(bytes.mid(entries[1].localOffset + offset, entries[1].compressedSize), &ok), deflated);
        QVERIFY(ok);
        apk::inflateRaw(bytes.mid(entries[1].localOffset + offset, entries[1].compressedSize / 2), &ok);
        QVERIFY(!ok);
        QCOMPARE(apk::localDataOffset(QByteArray(30, 'x'), true), -1);
        QCOMPARE(apk::localDataOffset(QByteArray(30, 'x'), false), 30 + 0x7878 + 0x7878);
        QCOMPARE(apk::localDataOffset(QByteArray(29, 'x'), false), -1);
        QVERIFY(!apk::findEocd(QByteArray(100, 'z')).found);
        QVERIFY(!apk::findEocd(QByteArray("PK\x05\x06tooshort", 12)).found);
    }

    void stringPoolEncodings() {
        const QString longText(300, QChar(0x00E9)); // 600 UTF-8 bytes: two-byte lengths
        const QString veryLong(40000, 'x');          // over 0x7FFF UTF-16 units
        const QStringList strings = {"res/a.png", QString::fromUtf8("caf\xC3\xA9"), longText, ""};
        QCOMPARE(apk::stringPool(stringPool(strings, true), 0), strings);
        QCOMPARE(apk::stringPool(stringPool(strings + QStringList{veryLong}, false), 0), strings + QStringList{veryLong});
        QCOMPARE(apk::stringPool(QByteArray(20, '\0'), 0), QStringList());
    }

    void tableResolvesEveryConfig() {
        const QStringList strings = {"res/mipmap-mdpi-v4/a.png", "res/mipmap-xxhdpi-v4/a.png", "res/mipmap-anydpi-v26/a.xml"};
        const QByteArray bytes = table(strings, {{0x0D, 160, Encoding::Dense, 3, {{0, {0x03, 0}}, {1, {0x03, 2}}}},
                                                 {0x0D, 480, Encoding::Offset16, 3, {{0, {0x03, 1}}}},
                                                 {0x0D, 65534, Encoding::Sparse, 2, {{0, {0x03, 2}}, {1, {0x03, 0}}}},
                                                 {0x0D, 320, Encoding::Dense, 1, {{0, {0x01, 0x7F0D0005}}}},
                                                 {0x08, 640, Encoding::Dense, 1, {{0, {0x03, 0}}}}});
        const apk::Found found = apk::resolveInTable(bytes, kIcon);
        const apk::Found expected = {{160, strings[0]}, {480, strings[1]}, {65534, strings[2]}, {320, "@7f0d0005"}};
        QCOMPARE(found, expected);
        QCOMPARE(apk::bestImage(found), strings[1]);
        // Entry 1: dense mdpi and sparse anydpi have it; offset16 marks it absent.
        QCOMPARE(apk::resolveInTable(bytes, kIcon + 1), (apk::Found{{160, strings[2]}, {65534, strings[0]}}));
        QCOMPARE(apk::resolveInTable(bytes, kIcon + 9), apk::Found());
        QCOMPARE(apk::resolveInTable(bytes.left(11), kIcon), apk::Found());
        QCOMPARE(apk::resolveInTable("\x03\x00" + bytes.mid(2), kIcon), apk::Found());
        // Cut short anywhere: no crash, and nothing beyond the cut.
        for (int cut = 0; cut < bytes.size(); cut += 7)
            QVERIFY(apk::resolveInTable(bytes.left(cut), kIcon).size() <= 4);
    }

    void sparseEntryPastThePairCount() {
        // As YouTube's table on Android 17: entry 15 in a chunk of 5 pairs.
        const QStringList strings = {"res/a.png", "res/b.png"};
        QMap<int, Value> pairs;
        for (int index : {2, 4, 9, 12, 15})
            pairs.insert(index, {0x03, quint32(index == 15 ? 1 : 0)});
        const QByteArray bytes = table(strings, {{0x0D, 480, Encoding::Sparse, 0, pairs}});
        QCOMPARE(apk::resolveInTable(bytes, kIcon + 15), (apk::Found{{480, strings[1]}}));
        QCOMPARE(apk::resolveInTable(bytes, kIcon + 14), apk::Found());
    }

    void manifestIconByNameOrResourceId() {
        QCOMPARE(apk::manifestIconId(manifest(kIcon)), kIcon);
        // Obfuscated attribute names: the resource map still says icon.
        const QByteArray renamed = binaryXml({"a", "b", "application"}, {0x01010001, 0x01010002},
                                             {{"application", {{"a", 0x03, 0}, {"b", 0x01, 0x7F0D0042}}}}, true);
        QCOMPARE(apk::manifestIconId(renamed), quint32(0x7F0D0042));
        // icon on another element, or not a reference: none.
        const QByteArray elsewhere =
            binaryXml({"icon", "activity", "application"}, {}, {{"application", {{"icon", 0x10, 5}}}, {"activity", {{"icon", 0x01, kIcon}}}});
        QCOMPARE(apk::manifestIconId(elsewhere), quint32(0));
        QCOMPARE(apk::manifestIconId(adaptiveXml()), quint32(0));
        QCOMPARE(apk::manifestIconId("<manifest/>"), quint32(0));
        const QByteArray bytes = manifest(kIcon);
        for (int cut = 0; cut < bytes.size(); cut += 5)
            apk::manifestIconId(bytes.left(cut));
    }

    void xmlRefsOfAdaptiveIcon() {
        QCOMPARE(apk::xmlRefs(adaptiveXml()), (QList<quint32>{kBackground, kForeground}));
        const QByteArray repeated = binaryXml({"layer-list", "item", "drawable", "src", "other"}, {},
                                              {{"layer-list", {}}, {"item", {{"drawable", 0x01, 5}, {"other", 0x01, 6}, {"src", 0x01, 5}}}});
        QCOMPARE(apk::xmlRefs(repeated), QList<quint32>{5});
        QCOMPARE(apk::xmlRefs(png("x")), QList<quint32>());
    }

    void walkerIsThePythonScript() {
        // sha256 and length of icons.py's _WALKER at 7b2d820.
        const QByteArray script = apk::deviceWalker().toUtf8();
        QCOMPARE(script.size(), 2756);
        QCOMPARE(QCryptographicHash::hash(script, QCryptographicHash::Sha256).toHex(),
                 QByteArray("5fddd8065e302ff4e0c281ecd46f22ddf68ed745aebe6d6b0de5bcc5fbb6c881"));
        QVERIFY(!script.contains('\''));
        QCOMPARE(apk::parseWalkerOutput("480 res/a.png\r\n  160   res/b c.webp  \nx res/c.png\n640\n\n65534 res/d.xml"),
                 (apk::Found{{480, "res/a.png"}, {160, "res/b c.webp"}, {65534, "res/d.xml"}}));
    }

    void monogramRendersExpectedColour_data() {
        QTest::addColumn<QString>("package");
        QTest::addColumn<QString>("colour");
        QTest::addColumn<QString>("letter");
        // Colours from Python's colorsys.hls_to_rgb(hue / 360, 0.38, 0.38),
        // hue = first SHA-256 byte (138 and 4).
        QTest::newRow("example") << QString("com.example.app") << QString("#3c8652") << QString("A");
        QTest::newRow("whatsapp") << QString("com.whatsapp") << QString("#86413c") << QString("W");
    }

    void monogramRendersExpectedColour() {
        QFETCH(QString, package);
        QFETCH(QString, colour);
        QFETCH(QString, letter);
        const QByteArray svg = IconSource::monogram(package);
        QVERIFY(!svg.contains("hsl("));
        QVERIFY(svg.contains("fill='" + colour.toUtf8() + "'"));
        QVERIFY(svg.contains("rx='16'"));
        QVERIFY(svg.contains("<text x='32' y='42' text-anchor='middle' font-size='30' font-family='sans-serif' fill='white'>" +
                             letter.toUtf8() + "</text>"));
        const QImage image = QImage::fromData(svg, "svg");
        QVERIFY(!image.isNull());
        QCOMPARE(QColor(image.pixel(2, image.height() / 2)).name(), colour);
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0); // rounded corner
    }

    void monogramLetters() {
        QVERIFY(IconSource::monogram("?").contains(">?</text>"));
        QVERIFY(IconSource::monogram("com.").contains(">?</text>"));
        QVERIFY(IconSource::monogram("a.&b").contains(">&amp;</text>"));
        QVERIFY(IconSource::monogram("a.<b").contains(">&lt;</text>"));
        QVERIFY(IconSource::monogram(QString::fromUtf8("com.\xF0\x9D\x90\x80x")).contains(QByteArray("\xF0\x9D\x90\x80</text>")));
    }

    // ---- binary XML, vector and adaptive icons ----

    void xmlTreeWithTypedValues() {
        const Node root{"layer-list",
                        {integer("count", 0x7F010000, -3)},
                        {{"item",
                          {argb("a", aColor, 0x80112233),
                           {"b", aFillColor, 0x1D, 0xFF445566, {}},
                           {"c", aStrokeColor, 0x1E, 0xCC778899, {}},
                           {"d", aAlpha, 0x1F, 0x00AABBCC, {}}},
                          {}},
                         {"item",
                          {dp("w", aWidth, 108), fraction("f", aInsetLeft, 0.25), flt("x", aPivotX, 1.5f),
                           {"b", 0x0101000e, 0x12, 0xFFFFFFFF, {}}, text("s", aPathData, "M0,0h1"),
                           ref("r", aDrawable, 0x7F080001)},
                          {{"deep", {}, {}}}}}};
        for (const bool obfuscate : {false, true}) {
            const auto parsed = apk::parseXml(axml(root, true, obfuscate));
            QVERIFY(parsed);
            QCOMPARE(parsed->name, QString("layer-list"));
            QCOMPARE(parsed->children.size(), 2);
            const apk::XmlElement &first = parsed->children[0];
            const apk::XmlElement &second = parsed->children[1];
            QCOMPARE(second.children.size(), 1);
            QCOMPARE(second.children[0].name, QString("deep"));
            QVERIFY(first.attribute(aColor, "a")->value.isColor());
            QCOMPARE(first.attribute(aColor, "a")->value.argb(), 0x80112233u);
            QCOMPARE(first.attribute(aFillColor, "b")->value.argb(), 0xFF445566u); // RGB8
            QCOMPARE(first.attribute(aStrokeColor, "c")->value.argb(), 0xCC778899u); // ARGB4, expanded by aapt
            QCOMPARE(first.attribute(aAlpha, "d")->value.argb(), 0xFFAABBCCu);   // RGB4: opaque
            QCOMPARE(second.attribute(aWidth, "w")->value.number(), 108.0);
            QCOMPARE(second.attribute(aWidth, "w")->resourceId, aWidth);
            QCOMPARE(second.attribute(aInsetLeft, "f")->value.type, quint8(0x06));
            QCOMPARE(second.attribute(aInsetLeft, "f")->value.number(), 0.25);
            QCOMPARE(second.attribute(aPivotX, "x")->value.number(), 1.5);
            QCOMPARE(second.attribute(0x0101000e, "b")->value.number(), 1.0);
            QCOMPARE(second.attribute(aPathData, "s")->value.string, QString("M0,0h1"));
            QCOMPARE(second.attribute(aDrawable, "r")->value.type, quint8(0x01));
            QCOMPARE(second.attribute(aDrawable, "r")->value.data, 0x7F080001u);
            QCOMPARE(parsed->attribute(0x7F010000, "count")->value.number(), -3.0);
            QCOMPARE(first.attribute(aWidth, "w"), nullptr);
            // Renamed attributes (a shrunk APK) are found by id only.
            QCOMPARE(first.attributes[0].name, obfuscate ? QString("x1") : QString("a"));
        }
        // No resource map: names find them.
        const auto unmapped = apk::parseXml(axml(root, false));
        QVERIFY(unmapped);
        QCOMPARE(unmapped->children[1].attribute(aWidth, "w")->resourceId, 0u);
        QCOMPARE(unmapped->children[1].attribute(aWidth, "w")->value.number(), 108.0);
        // An app's own attribute of the same name does not stand in for the
        // framework's.
        const auto own = apk::parseXml(axml({"path", {argb("fillColor", 0x7F040001, 0xFF00FF00)}, {}}));
        QVERIFY(own);
        QCOMPARE(own->attribute(aFillColor, "fillColor"), nullptr);
    }

    void tableValuesTypesAndConfigs() {
        const QStringList strings = {"res/color/sel.xml"};
        const QByteArray bytes =
            table(strings, {{0x06, 0, Encoding::Dense, 4, {{0, {0x1C, 0xFF102030}}, {1, {0x01, 0x7F060000}}, {2, {0x03, 0}}, {3, {0x1D, 0xFF0000FF}}}},
                            {0x06, 0, Encoding::Offset16, 1, {{0, {0x1C, 0xFF000000}}}, 0x20}});
        const QList<apk::TableValue> colour = apk::tableValues(bytes, 0x7F060000);
        QCOMPARE(colour.size(), 2);
        QCOMPARE(colour[0].value.type, quint8(0x1C));
        QCOMPARE(colour[0].value.argb(), 0xFF102030u);
        QVERIFY(colour[0].plain);
        QCOMPARE(colour[1].value.argb(), 0xFF000000u);
        QVERIFY(!colour[1].plain); // night
        QCOMPARE(apk::tableValues(bytes, 0x7F060001).value(0).value.data, 0x7F060000u);
        QCOMPARE(apk::tableValues(bytes, 0x7F060002).value(0).value.string, strings[0]);
        QCOMPARE(apk::resolveInTable(bytes, 0x7F060002), (apk::Found{{0, strings[0]}}));
        QCOMPARE(apk::resolveInTable(bytes, 0x7F060001), (apk::Found{{0, "@7f060000"}}));
        // A colour is not a path for the bitmap reader.
        QCOMPARE(apk::resolveInTable(bytes, 0x7F060003), apk::Found());
        // Another package's id (the framework's white) is not this table's
        // type 6 entry 0x0b.
        QVERIFY(apk::tableValues(bytes, 0x01060000).isEmpty());
        for (int cut = 0; cut < bytes.size(); cut += 3)
            QVERIFY(apk::tableValues(bytes.left(cut), 0x7F060000).size() <= 2);
    }

    void vectorGroupTransformAndReferencedColor() {
        // A square at 10..30 in a group rotated 90 degrees about (50, 50) and
        // moved down 50: it lands at 70..90 x 60..80. Its colour is
        // @color/accent, which is @color/red, 0xFFE53935. A second, literal
        // blue square at half alpha sits in the middle.
        const Node v = vector(100, {{"group",
                                     {flt("rotation", aRotation, 90), flt("pivotX", aPivotX, 50), flt("pivotY", aPivotY, 50),
                                      flt("translateY", aTranslateY, 50)},
                                     {path("M10,10h20v20h-20z", ref("fillColor", aFillColor, 0x7F060000))}},
                                    path("M40,40h20v20h-20z", argb("fillColor", aFillColor, 0xFF1E88E5),
                                         {flt("fillAlpha", aFillAlpha, 0.5f)}),
                                    path("M0,95h100", argb("fillColor", aFillColor, 0),
                                         {argb("strokeColor", aStrokeColor, 0xFF00FF00), flt("strokeWidth", aStrokeWidth, 4),
                                          integer("strokeLineCap", aStrokeLineCap, 1), integer("fillType", aFillType, 1)})});
        Memory memory;
        memory.table = drawTable({"res/drawable/v.xml"}, {{0x0D, 0, {0x01, 0x7F080000}},
                                                          {0x08, 0, {0x03, 0}},
                                                          {0x06, 0, {0x01, 0x7F060001}},
                                                          {0x06, 1, {0x1C, 0xFFE53935}}});
        memory.files["res/drawable/v.xml"] = axml(v);

        const auto parsed = apk::parseXml(memory.files["res/drawable/v.xml"]);
        QVERIFY(parsed);
        const QByteArray svg = apk::vectorToSvg(*parsed, [](quint32 id) {
            apk::Paint paint;
            if (id == 0x7F060000) {
                paint.kind = apk::Paint::Solid;
                paint.argb = 0xFFE53935;
            }
            return paint;
        });
        QVERIFY(svg.contains("viewBox=\"0 0 100 100\""));
        QVERIFY(svg.contains("transform=\"translate(50 100) rotate(90) scale(1 1) translate(-50 -50)\""));
        QVERIFY(svg.contains("fill=\"#e53935\""));
        QVERIFY(svg.contains("fill=\"#1e88e5\" fill-opacity=\"0.5\""));
        QVERIFY(svg.contains("fill-rule=\"evenodd\" stroke=\"#00ff00\" stroke-width=\"4\""));
        QVERIFY(svg.contains("stroke-linecap=\"round\""));

        QStringList met;
        const QImage image = QImage::fromData(apk::renderIcon(kIcon, memory.resources(), 192, &met));
        QCOMPARE(image.size(), QSize(192, 192));
        QCOMPARE(met, QStringList{"vector"});
        const double scale = 1.92;
        QVERIFY_COLOR(pixelAt(image, 80 * scale, 70 * scale), 0xFFE53935u);  // moved square
        QCOMPARE(qAlpha(pixelAt(image, 20 * scale, 20 * scale)), 0);           // not where it was drawn
        QCOMPARE(qAlpha(pixelAt(image, 80 * scale, 20 * scale)), 0);           // nor only rotated
        QVERIFY_COLOR(pixelAt(image, 50 * scale, 50 * scale), 0x801E88E5u);  // half-alpha blue
        QVERIFY_COLOR(pixelAt(image, 50 * scale, 95 * scale), 0xFF00FF00u);  // the stroke
        QCOMPARE(qAlpha(pixelAt(image, 50 * scale, 90 * scale)), 0);
    }

    void adaptiveIconOnThePhone() {
        // ic_app is an adaptive icon: a colour background and a vector
        // foreground with a yellow square at 44..64 of 108 dp.
        const QStringList strings = {"res/mipmap-anydpi-v26/ic_app.xml", "res/drawable/fg.xml"};
        const QByteArray tableBytes = drawTable(strings, {{0x0D, 0, {0x03, 0}}, {0x06, 0, {0x1C, 0xFF336699}}, {0x08, 1, {0x03, 1}}});
        const QByteArray icon = axml(adaptive(ref("drawable", aDrawable, kBackground), ref("drawable", aDrawable, kForeground)));
        const QByteArray foreground = axml(vector(108, {path("M44,44h20v20h-20z", argb("fillColor", aFillColor, 0xFFFFCC00))}));
        FakePhone phone;
        phone.install("com.vector.app", {{"base.apk", zip({{"AndroidManifest.xml", manifest(kIcon), true},
                                                           {"resources.arsc", tableBytes, false},
                                                           {strings[0], icon, true},
                                                           {strings[1], foreground, true}})}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        const QByteArray bytes = source.icon("com.vector.app", {}, &mime);
        QCOMPARE(mime, QString("image/png"));
        const QImage image = QImage::fromData(bytes, "PNG");
        QCOMPARE(image.size(), QSize(192, 192));
        // The middle 72 of 108 dp fills the 192 pixels: dp 18 is pixel 0.
        const auto px = [](double dp) { return (dp - 18) * 192 / 72; };
        QVERIFY_COLOR(image.pixel(96, 96), 0xFFFFCC00u);
        QVERIFY_COLOR(pixelAt(image, px(46), px(54)), 0xFFFFCC00u);
        QVERIFY_COLOR(pixelAt(image, px(42), px(54)), 0xFF336699u);
        QVERIFY_COLOR(pixelAt(image, px(66), px(54)), 0xFF336699u);
        QVERIFY_COLOR(image.pixel(20, 20), 0xFF336699u); // a corner of the safe zone, inside the rounding
        QVERIFY_COLOR(image.pixel(96, 1), 0xFF336699u);
        QCOMPARE(qAlpha(image.pixel(1, 1)), 0);           // clipped to a rounded square
        QCOMPARE(qAlpha(image.pixel(190, 190)), 0);
        // Cached as an image: the next source reads no phone.
        QCOMPARE(cacheFiles(m_cache), QStringList{"com.vector.app.img"});
        phone.clearCalls();
        IconSource again(m_cache, phone.runner(), "/fake/adb");
        QCOMPARE(again.icon("com.vector.app", {}, &mime), bytes);
        QCOMPARE(mime, QString("image/png"));
        QVERIFY(phone.calls().isEmpty());
    }

    void referenceChains() {
        // mipmap -> @drawable/alias -> @drawable/real -> the adaptive XML;
        // background -> @color/bg -> @color/bg2 (a night variant too);
        // foreground -> @drawable/fg -> a vector filled with @color/c, a
        // colour state list whose stateless item is @color/blue at half
        // alpha; a second path uses @color/loop, which refers to itself.
        const QStringList strings = {"res/mipmap-anydpi-v26/ic.xml", "res/drawable/fg.xml", "res/color/c.xml"};
        const QByteArray tableBytes =
            table(strings, {{0x0D, 65534, Encoding::Dense, 1, {{0, {0x01, 0x7F080000}}}},
                            {0x08, 0, Encoding::Dense, 3, {{0, {0x01, 0x7F080001}}, {1, {0x03, 0}}, {2, {0x03, 1}}}},
                            {0x06, 0, Encoding::Dense, 5,
                             {{0, {0x01, 0x7F060001}}, {1, {0x1C, 0xFF00AA55}}, {2, {0x03, 2}}, {3, {0x1D, 0xFF0000FF}}, {4, {0x01, 0x7F060004}}}},
                            {0x06, 0, Encoding::Dense, 2, {{1, {0x1C, 0xFF000000}}}, 0x20}});
        Memory memory;
        memory.table = tableBytes;
        memory.files[strings[0]] = axml(adaptive(ref("drawable", aDrawable, 0x7F060000), ref("drawable", aDrawable, 0x7F080002)));
        memory.files[strings[1]] = axml(vector(108, {path("M36,36h36v36h-36z", ref("fillColor", aFillColor, 0x7F060002)),
                                                     path("M0,0h10v10h-10z", ref("fillColor", aFillColor, 0x7F060004))}));
        memory.files[strings[2]] =
            axml({"selector",
                  {},
                  {{"item", {{"state_pressed", 0x010100a7, 0x12, 0xFFFFFFFF, {}}, argb("color", aColor, 0xFFFF0000)}, {}},
                   {"item", {ref("color", aColor, 0x7F060003), flt("alpha", aAlpha, 0.5f)}, {}}}});
        QStringList met;
        const QImage image = QImage::fromData(apk::renderIcon(kIcon, memory.resources(), 192, &met));
        QCOMPARE(image.size(), QSize(192, 192));
        QCOMPARE(met, (QStringList{"adaptive-icon", "color", "vector"}));
        QVERIFY_COLOR(image.pixel(20, 20), 0xFF00AA55u);         // the plain bg2, not the night one
        QVERIFY_COLOR(image.pixel(96, 96), 0xFF0055AAu);         // half blue over bg2
    }

    void gradients() {
        // A linear gradient (red to blue, left to right) from an aapt
        // <gradient> file, and a radial one (white in the middle to black).
        const QStringList strings = {"res/drawable/v.xml", "res/drawable/$v__0.xml", "res/drawable/$v__1.xml"};
        Memory memory;
        memory.table = drawTable(strings, {{0x0D, 0, {0x01, 0x7F080000}}, {0x08, 0, {0x03, 0}}, {0x08, 1, {0x03, 1}}, {0x08, 2, {0x03, 2}}});
        memory.files[strings[0]] = axml(vector(100, {path("M0,0h100v50h-100z", ref("fillColor", aFillColor, 0x7F080001)),
                                                     path("M0,50h100v50h-100z", ref("fillColor", aFillColor, 0x7F080002))}));
        memory.files[strings[1]] =
            axml({"gradient",
                  {integer("type", aType, 0), flt("startX", aStartX, 0), flt("startY", aStartY, 0), flt("endX", aEndX, 100),
                   flt("endY", aEndY, 0)},
                  {{"item", {flt("offset", aOffset, 0), argb("color", aColor, 0xFFFF0000)}, {}},
                   {"item", {flt("offset", aOffset, 1), argb("color", aColor, 0xFF0000FF)}, {}}}});
        memory.files[strings[2]] =
            axml({"gradient",
                  {integer("type", aType, 1), flt("centerX", aCenterX, 50), flt("centerY", aCenterY, 75),
                   flt("gradientRadius", aGradientRadius, 25), argb("startColor", 0x0101019d, 0xFFFFFFFF),
                   argb("endColor", 0x0101019e, 0xFF000000)},
                  {}});
        const QImage image = QImage::fromData(apk::renderIcon(kIcon, memory.resources()));
        QCOMPARE(image.size(), QSize(192, 192));
        QVERIFY_COLOR(image.pixel(1, 40), 0xFFFF0000u);
        QVERIFY_COLOR(image.pixel(190, 40), 0xFF0000FFu);
        QVERIFY(qRed(image.pixel(96, 40)) > 100 && qBlue(image.pixel(96, 40)) > 100);
        QVERIFY_COLOR(image.pixel(96, 144), 0xFFFFFFFFu); // the radial centre
        QVERIFY_COLOR(image.pixel(2, 144), 0xFF000000u);  // beyond its radius
        // Both gradient files were offered to prefetch together.
        QCOMPARE(memory.prefetched.value(0), (QStringList{strings[1], strings[2]}));
    }

    void bitmapInsetAndLayerList() {
        // Background: a green PNG. Foreground: a layer-list whose item is an
        // inset of a quarter on each side around <bitmap src=red PNG>.
        const QStringList strings = {"res/mipmap-anydpi-v26/ic.xml", "res/drawable-xxhdpi-v4/bg.png", "res/drawable/fg.xml",
                                     "res/drawable/dot.xml", "res/drawable-nodpi-v4/dot.png"};
        Memory memory;
        memory.table = drawTable(strings, {{0x0D, 0, {0x03, 0}},
                                           {0x08, 0, {0x03, 1}},
                                           {0x08, 1, {0x03, 2}},
                                           {0x08, 2, {0x03, 3}},
                                           {0x08, 3, {0x03, 4}}});
        memory.files[strings[0]] = axml(adaptive(ref("drawable", aDrawable, 0x7F080000), ref("drawable", aDrawable, 0x7F080001)));
        memory.files[strings[1]] = solidPng(0xFF00C853);
        memory.files[strings[2]] =
            axml({"layer-list",
                  {},
                  {{"item",
                    {},
                    {{"inset",
                      {fraction("insetLeft", aInsetLeft, 0.25), fraction("insetRight", aInsetRight, 0.25),
                       fraction("insetTop", aInsetTop, 0.25), fraction("insetBottom", aInsetBottom, 0.25),
                       ref("drawable", aDrawable, 0x7F080002)},
                      {}}}}}});
        memory.files[strings[3]] = axml({"bitmap", {ref("src", aSrc, 0x7F080003)}, {}});
        memory.files[strings[4]] = solidPng(0xFFD50000, 16);
        QStringList met;
        const QImage image = QImage::fromData(apk::renderIcon(kIcon, memory.resources(), 192, &met));
        QCOMPARE(image.size(), QSize(192, 192));
        QCOMPARE(met, (QStringList{"adaptive-icon", "bitmap", "layer-list", "inset"}));
        // The layer is 288 pixels from -48; the inset leaves 24..168.
        QVERIFY_COLOR(image.pixel(96, 96), 0xFFD50000u);
        QVERIFY_COLOR(image.pixel(30, 96), 0xFFD50000u);
        QVERIFY_COLOR(image.pixel(18, 96), 0xFF00C853u);
        QVERIFY_COLOR(image.pixel(96, 174), 0xFF00C853u);
    }

    void malformedXmlDrawsNothing() {
        const QByteArray good = axml(vector(100, {path("M0,0h100v100h-100z", argb("fillColor", aFillColor, 0xFFFF0000))}));
        QVERIFY(apk::parseXml(good));
        for (int cut = 0; cut < good.size(); ++cut)
            QVERIFY2(!apk::parseXml(good.left(cut)), qPrintable(QString::number(cut)));
        QVERIFY(!apk::parseXml(png("x")));
        QVERIFY(!apk::parseXml(QByteArray("\x03\x00\x08\x00\xff\xff\xff\xff", 8) + QByteArray(64, '\xff')));
        // A chunk claiming more than the file holds, and a zero-size one.
        QByteArray lying = good;
        lying[good.size() - 20] = '\x7f';
        apk::parseXml(lying);
        QByteArray zero = good;
        zero.replace(8, 8, QByteArray(8, '\0'));
        QVERIFY(!apk::parseXml(zero));

        const QStringList strings = {"res/drawable/v.xml"};
        const auto drawn = [&](const QByteArray &file, quint32 icon = kIcon) {
            Memory memory;
            memory.table = drawTable(strings, {{0x0D, 0, {0x03, 0}}, {0x0D, 1, {0x01, 0x7F0D0001}}, {0x0D, 2, {0x01, 0x7F0D0003}},
                                               {0x0D, 3, {0x01, 0x7F0D0002}}});
            memory.files[strings[0]] = file;
            return apk::renderIcon(icon, memory.resources());
        };
        QVERIFY(!drawn(good).isEmpty());
        for (int cut = 0; cut < good.size(); cut += 9)
            QVERIFY(drawn(good.left(cut)).isEmpty());
        QVERIFY(drawn(QByteArray(300, '\x03')).isEmpty());
        QVERIFY(drawn(png("not really")).isEmpty());
        // A vector without a viewport, a path without data, a path whose
        // colour is a theme attribute (?attr): nothing drawn.
        QVERIFY(drawn(axml({"vector", {dp("width", aWidth, 24)}, {path("M0,0h9v9z", argb("fillColor", aFillColor, 0xFFFF0000))}})).isEmpty());
        QVERIFY(drawn(axml(vector(24, {path("", argb("fillColor", aFillColor, 0xFFFF0000))}))).isEmpty());
        QVERIFY(drawn(axml(vector(24, {path("M0,0h24v24h-24z", {"fillColor", aFillColor, 0x02, 0x7F040000, {}})}))).isEmpty());
        // References in a loop end.
        QVERIFY(drawn(good, 0x7F0D0001).isEmpty());
        QVERIFY(drawn(good, 0x7F0D0002).isEmpty());
        QVERIFY(apk::renderIcon(kIcon, apk::Resources{}).isEmpty());

        // On the phone: a cut-short icon is a miss with a monogram.
        FakePhone phone;
        const QByteArray icon = axml(adaptive(argb("drawable", aDrawable, 0xFF336699), ref("drawable", aDrawable, kForeground)));
        phone.install("com.broken.app",
                      {{"base.apk", zip({{"AndroidManifest.xml", manifest(kIcon), true},
                                         {"resources.arsc", drawTable({"res/mipmap-anydpi-v26/ic.xml", "res/drawable/fg.xml"},
                                                                      {{0x0D, 0, {0x03, 0}}, {0x08, 1, {0x03, 1}}}),
                                          false},
                                         {"res/mipmap-anydpi-v26/ic.xml", icon, true},
                                         {"res/drawable/fg.xml", good.left(good.size() - 12), true}})}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.broken.app", {}, &mime), IconSource::monogram("com.broken.app"));
        QCOMPARE(mime, QString("image/svg+xml"));
        QCOMPARE(cacheFiles(m_cache), QStringList{"com.broken.app.miss"});
    }

    void xmlIconReadsAreFew_data() {
        QTest::addColumn<int>("gradients");
        QTest::newRow("one gradient") << 1;
        QTest::newRow("six gradients") << 6;
    }

    void xmlIconReadsAreFew() {
        // A foreground whose paths each fill with their own gradient file,
        // as aapt writes inline gradients: the files come across in one
        // read, so six cost no more reads than one. The APK is larger than
        // the tail reads, so those do not hold the files already.
        QFETCH(int, gradients);
        QStringList strings = {"res/mipmap-anydpi-v26/ic.xml", "res/drawable/fg.xml"};
        QList<Entry> entries = {{0x0D, 0, {0x03, 0}}, {0x06, 0, {0x1C, 0xFF336699}}, {0x08, 1, {0x03, 1}}};
        QList<Node> paths;
        QList<ZipItem> items = {{"AndroidManifest.xml", manifest(kIcon), true}};
        for (int i = 0; i < gradients; ++i) {
            strings.append(QString("res/drawable/$fg__%1.xml").arg(i));
            entries.append({0x08, 2 + i, {0x03, quint32(2 + i)}});
            paths.append(path(QString("M%1,0h10v108h-10z").arg(i * 10), ref("fillColor", aFillColor, 0x7F080002 + i)));
            items.append({strings.last(),
                          axml({"gradient",
                                {integer("type", aType, 0), flt("startX", aStartX, 0), flt("endX", aEndX, 108)},
                                {{"item", {flt("offset", aOffset, 0), argb("color", aColor, 0xFFFF0000)}, {}},
                                 {"item", {flt("offset", aOffset, 1), argb("color", aColor, 0xFF0000FF)}, {}}}}),
                          true});
        }
        items.append({strings[0], axml(adaptive(ref("drawable", aDrawable, kBackground), ref("drawable", aDrawable, kForeground))), true});
        items.append({strings[1], axml(vector(108, paths)), true});
        items.append({"resources.arsc", drawTable(strings, entries), false});
        QByteArray noise(600000, '\0');
        for (int i = 0; i < noise.size(); ++i)
            noise[i] = char((i * 7919) >> 3);
        items.append({"assets/large.bin", noise, false});
        FakePhone phone;
        phone.install("com.gradient.app", {{"base.apk", zip(items)}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        source.icon("com.gradient.app", {}, &mime);
        QCOMPARE(mime, QString("image/png"));
        QSet<QString> seen;
        for (const auto &call : phone.calls()) {
            if (call.arguments.value(2) != "dd")
                continue;
            QVERIFY2(!seen.contains(call.arguments.join(' ')), "read twice");
            seen.insert(call.arguments.join(' '));
        }
        // dd: the 256,000-byte tail (which holds the directory), the
        // 400,000-byte tail (locate), the manifest with its local header,
        // the table's local header, the table, the icon XML, the
        // foreground XML, and the gradient files: one file alone, six
        // together.
        QCOMPARE(phone.reads().size(), 8);
        const QPair<qint64, qint64> last = phone.reads().last();
        QVERIFY2(last.second < 400000, qPrintable(QString::number(last.second)));
    }

    // ---- IconSource against the fake phone ----

    void launcherFoundHighestDensity() {
        FakePhone phone;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        const QByteArray image = source.icon("com.demo.app", "SERIAL1", &mime);
        QCOMPARE(mime, QString("image/png"));
        QCOMPARE(image, png("xxhdpi"));
        for (const auto &call : phone.calls())
            QCOMPARE(call.serial, QString("SERIAL1"));
        QCOMPARE(phone.calls().first().arguments, QStringList{"get-state"});
        QCOMPARE(phone.calls().first().timeoutMs, 3000);
        QCOMPARE(phone.calls().at(1).arguments, (QStringList{"shell", "pm", "path", "com.demo.app"}));
        // The cache holds the image; a new source reads it without adb.
        QCOMPARE(cacheFiles(m_cache), QStringList{"com.demo.app.img"});
        phone.clearCalls();
        IconSource again(m_cache, phone.runner(), "/fake/adb");
        QCOMPARE(again.icon("com.demo.app", {}, &mime), image);
        QCOMPARE(mime, QString("image/png"));
        QVERIFY(phone.calls().isEmpty());
    }

    void readsAreFastAndNotRepeated() {
        FakePhone phone;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.demo.app", "SERIAL1", &mime), png("xxhdpi"));
        QSet<QString> seen;
        int lists = 0;
        for (const auto &call : phone.calls()) {
            if (call.arguments.value(0) == "shell" && call.arguments.value(1) == "ls")
                ++lists;
            if (call.arguments.value(2) != "dd")
                continue;
            // Byte offsets with a large block, never one byte at a time.
            QCOMPARE(call.arguments.value(4), QString("bs=65536"));
            const QString key = call.arguments.join(' ');
            QVERIFY2(!seen.contains(key), qPrintable("read twice: " + key));
            seen.insert(key);
        }
        QVERIFY(!seen.isEmpty());
        QCOMPARE(lists, 1); // one APK, its size asked once
    }

    void oldToyboxFallsBackToOneByteBlocks() {
        FakePhone phone;
        phone.oldToybox = true;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.demo.app", "SERIAL1", &mime), png("xxhdpi"));
        int fast = 0, slow = 0;
        for (const auto &call : phone.calls()) {
            if (call.arguments.value(2) != "dd")
                continue;
            (call.arguments.value(4) == "bs=1" ? slow : fast)++;
        }
        QCOMPARE(fast, 1); // refused once, then never asked again
        QVERIFY(slow > 0);
    }

    void launcherAcrossSplitsAndWebp() {
        FakePhone phone;
        phone.install("com.split.app",
                      {{"base.apk", zip({{"res/mipmap-xhdpi-v4/ic_launcher.png", png("base-xhdpi"), false}})},
                       {"split_config.xxxhdpi.apk", zip({{"res/mipmap-xxxhdpi-v4/ic_launcher.webp", webp("split"), true}})}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.split.app", {}, &mime), webp("split"));
        QCOMPARE(mime, QString("image/webp"));
        for (const auto &call : phone.calls())
            QVERIFY(call.serial.isEmpty());
    }

    void oversizeLauncherFallsBackToManifest() {
        // xxxhdpi ic_launcher.png ranks highest but is over 400,000 bytes:
        // icons.py does not try the next launcher, it goes to the manifest.
        FakePhone phone;
        const QList<ZipItem> items = {{"AndroidManifest.xml", manifest(kIcon), true},
                                      {"resources.arsc",
                                       table({"res/mipmap-xxhdpi-v4/whatever.png"}, {{0x0D, 480, Encoding::Dense, 1, {{0, {0x03, 0}}}}}),
                                       false},
                                      {"res/mipmap-xxhdpi-v4/whatever.png", png("from-manifest"), false},
                                      {"res/mipmap-hdpi-v4/ic_launcher.png", png("hdpi"), false},
                                      {"res/mipmap-xxxhdpi-v4/ic_launcher.png", png("huge") + QByteArray(400001, 'h'), false}};
        phone.install("com.big.icon", {{"base.apk", zip(items)}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.big.icon", {}, &mime), png("from-manifest"));
        for (const auto &[skip, count] : phone.reads())
            QVERIFY2(count <= 400000, qPrintable(QString::number(count)));
    }

    void svgPretendingLauncherFallsBackToManifest() {
        FakePhone phone;
        const QList<ZipItem> items = {{"AndroidManifest.xml", manifest(kIcon), true},
                                      {"resources.arsc",
                                       table({"res/mipmap-xxhdpi-v4/whatever.png"}, {{0x0D, 480, Encoding::Dense, 1, {{0, {0x03, 0}}}}}),
                                       false},
                                      {"res/mipmap-xxhdpi-v4/whatever.png", png("from-manifest"), true},
                                      {"res/mipmap-xxxhdpi-v4/ic_launcher.png", svgText(), true}};
        phone.install("com.svg.icon", {{"base.apk", zip(items)}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.svg.icon", {}, &mime), png("from-manifest"));
        QCOMPARE(mime, QString("image/png"));
    }

    void manifestFallbackPicksHighestDensity() {
        // No ic_launcher file. The table has mdpi, xxhdpi and anydpi; the
        // xxhdpi bitmap lives in a density split, not in base.apk.
        FakePhone phone;
        const QByteArray split = zip({{"res/mipmap-xxhdpi-v4/whatever.png", png("xxhdpi"), true}});
        phone.install("com.manifest.app", {{"base.apk", manifestApk(false)}, {"split_config.xxhdpi.apk", split}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.manifest.app", {}, &mime), png("xxhdpi"));
        QCOMPARE(mime, QString("image/png"));
        QCOMPARE(cacheFiles(m_cache), QStringList{"com.manifest.app.img"});
    }

    void manifestFollowsAdaptiveIconLayers() {
        FakePhone phone;
        phone.install("com.adaptive.app", {{"base.apk", zip({{"AndroidManifest.xml", manifest(kIcon), true},
                                                             {"resources.arsc", adaptiveTable(), false},
                                                             {kAdaptivePaths[0], adaptiveXml(), true},
                                                             {kAdaptivePaths[1], png("fg-xxxhdpi"), true},
                                                             {kAdaptivePaths[2], png("fg-mdpi"), false}})}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.adaptive.app", {}, &mime), png("fg-xxxhdpi"));
    }

    void largeTableWalkedOnPhone() {
        // The walker only runs on a phone without byte-offset reads.
        // A stored resources.arsc over 1,500,000 bytes is not copied off: the
        // walker script runs "on the phone" (here: sh with toybox = coreutils).
        if (QStandardPaths::findExecutable("dd").isEmpty() || QStandardPaths::findExecutable("od").isEmpty())
            QSKIP("dd and od are needed to run the walker");
        const QString bin = m_temporary->path() + "/bin";
        QVERIFY(QDir().mkpath(bin));
        QFile toybox(bin + "/toybox");
        QVERIFY(toybox.open(QIODevice::WriteOnly));
        toybox.write("#!/bin/sh\nexec \"$@\"\n");
        toybox.close();
        toybox.setPermissions(toybox.permissions() | QFileDevice::ExeOwner);

        const QStringList strings = {"res/mipmap-mdpi-v4/whatever.png", "res/mipmap-xxhdpi-v4/whatever.png",
                                     "res/mipmap-anydpi-v26/whatever.xml"};
        const QByteArray big = table(strings,
                                     {{0x0D, 160, Encoding::Dense, 3, {{0, {0x03, 0}}}},
                                      {0x0D, 480, Encoding::Offset16, 3, {{0, {0x03, 1}}}},
                                      {0x0D, 65534, Encoding::Sparse, 1, {{0, {0x03, 2}}}}},
                                     1600000);
        QVERIFY(big.size() > 1500000);
        const QByteArray bytes = zip({{"AndroidManifest.xml", manifest(kIcon), true},
                                      {"resources.arsc", big, false},
                                      {strings[0], png("mdpi"), false},
                                      {strings[1], png("xxhdpi"), true}});
        // The walker reads the APK by path, so the "device" path is a real file.
        const QString path = m_temporary->path() + "/data/app/com.big.table-1/base.apk";
        QVERIFY(QDir().mkpath(QFileInfo(path).path()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        file.close();

        FakePhone phone;
        phone.toyboxDir = bin;
        phone.oldToybox = true;
        phone.files[path] = bytes;
        phone.packages["com.big.table"] = {path};
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.big.table", {}, &mime), png("xxhdpi"));
        int walks = 0;
        for (const auto &call : phone.calls()) {
            if (call.arguments.mid(0, 3) == QStringList{"shell", "sh", "-c"}) {
                ++walks;
                QCOMPARE(call.timeoutMs, 50000);
                QCOMPARE(call.arguments.mid(4), (QStringList{"sh", "'" + path + "'", call.arguments.at(6), "13", "0"}));
            }
        }
        QCOMPARE(walks, 1);
        for (const auto &[skip, count] : phone.reads())
            QVERIFY2(count <= 1500000, qPrintable(QString::number(count)));

        // With byte-offset reads the same table is copied and indexed here.
        FakePhone fast;
        fast.toyboxDir = bin;
        fast.files[path] = bytes;
        fast.packages["com.big.table"] = {path};
        QTemporaryDir otherCache;
        IconSource copying(otherCache.path(), fast.runner(), "/fake/adb");
        QCOMPARE(copying.icon("com.big.table", {}, &mime), png("xxhdpi"));
        for (const auto &call : fast.calls())
            QVERIFY(call.arguments.mid(0, 3) != (QStringList{"shell", "sh", "-c"}));
    }

    void monogramAndMissWhenNothingResolves() {
        FakePhone phone;
        // A manifest with no icon attribute, and no ic_launcher file.
        phone.install("com.no.icon", {{"base.apk", zip({{"AndroidManifest.xml",
                                                        binaryXml({"application", "label"}, {}, {{"application", {{"label", 0x03, 1}}}}),
                                                        true}})}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.no.icon", {}, &mime), IconSource::monogram("com.no.icon"));
        QCOMPARE(mime, QString("image/svg+xml"));
        QCOMPARE(cacheFiles(m_cache), QStringList{"com.no.icon.miss"});
        // Stamped with the reader's version.
        QCOMPARE(QFileInfo(m_cache + "/com.no.icon.miss").size(), 2);
        // Installed APK with no package at all: also a miss.
        QCOMPARE(source.icon("com.not.installed", {}, &mime), IconSource::monogram("com.not.installed"));
        QVERIFY(QFileInfo::exists(m_cache + "/com.not.installed.miss"));
    }

    void phoneNotReadyGivesMonogramWithoutMiss_data() {
        QTest::addColumn<QString>("state");
        QTest::newRow("unauthorized") << QString("unauthorized");
        QTest::newRow("adb does not start") << QString("nostart");
    }

    void phoneNotReadyGivesMonogramWithoutMiss() {
        QFETCH(QString, state);
        FakePhone phone;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        phone.state = state;
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.demo.app", {}, &mime), IconSource::monogram("com.demo.app"));
        QCOMPARE(mime, QString("image/svg+xml"));
        QCOMPARE(phone.calls().size(), 1);
        QCOMPARE(phone.calls().first().arguments, QStringList{"get-state"});
        QVERIFY(cacheFiles(m_cache).isEmpty());
        // Ready later: tried again, and found.
        phone.state = "device";
        QCOMPARE(source.icon("com.demo.app", {}, &mime), png("xxhdpi"));
    }

    void noAdbGivesMonogramWithoutMiss() {
        FakePhone phone;
        IconSource source(m_cache, phone.runner(), QString());
        QString mime;
        QCOMPARE(source.icon("com.demo.app", {}, &mime), IconSource::monogram("com.demo.app"));
        QVERIFY(phone.calls().isEmpty());
        QVERIFY(cacheFiles(m_cache).isEmpty());
    }

    void failedReadIsUnavailableNotMiss() {
        FakePhone phone;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        phone.failingReads.insert(apkPath("com.demo.app"));
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.demo.app", {}, &mime), IconSource::monogram("com.demo.app"));
        QVERIFY(cacheFiles(m_cache).isEmpty());
        phone.failingReads.clear();
        QCOMPARE(source.icon("com.demo.app", {}, &mime), png("xxhdpi"));
    }

    void cacheHitMakesNoAdbCall() {
        QVERIFY(QDir().mkpath(m_cache));
        QFile file(m_cache + "/com.cached.app.img");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(webp("cached"));
        file.close();
        FakePhone phone;
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.cached.app", "SERIAL", &mime), webp("cached"));
        QCOMPARE(mime, QString("image/webp"));
        QVERIFY(phone.calls().isEmpty());
    }

    void missIsHonoured() {
        QVERIFY(QDir().mkpath(m_cache));
        QFile file(m_cache + "/com.demo.app.miss");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("3\n");
        file.close();
        FakePhone phone;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.demo.app", {}, &mime), IconSource::monogram("com.demo.app"));
        QCOMPARE(mime, QString("image/svg+xml"));
        QVERIFY(phone.calls().isEmpty());
    }

    void oldMissIsTriedAgain_data() {
        QTest::addColumn<QByteArray>("stamp");
        // An empty .miss is from the first reader, "2" from the one before
        // XML icons were drawn: both missed icons this one finds.
        QTest::newRow("first reader") << QByteArray();
        QTest::newRow("reader 2") << QByteArray("2\n");
    }

    void oldMissIsTriedAgain() {
        QFETCH(QByteArray, stamp);
        QVERIFY(QDir().mkpath(m_cache));
        QFile file(m_cache + "/com.demo.app.miss");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(stamp);
        file.close();
        FakePhone phone;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.demo.app", {}, &mime), png("xxhdpi"));
        QVERIFY(!phone.calls().isEmpty());
    }

    void badPackageNames_data() {
        QTest::addColumn<QString>("package");
        QTest::addColumn<QString>("monogramOf");
        QTest::newRow("empty") << QString("") << QString("?");
        QTest::newRow("one part") << QString("com") << QString("com");
        QTest::newRow("leading digit") << QString("1com.example") << QString("1com.example");
        QTest::newRow("part with digit first") << QString("com.1example") << QString("com.1example");
        QTest::newRow("double dot") << QString("com..example") << QString("com..example");
        QTest::newRow("trailing dot") << QString("com.example.") << QString("com.example.");
        QTest::newRow("space") << QString("com.exa mple") << QString("com.exa mple");
        QTest::newRow("slash") << QString("com/example.app") << QString("com/example.app");
        QTest::newRow("traversal") << QString("../../etc.passwd") << QString("../../etc.passwd");
        QTest::newRow("newline") << "com.example\n" << "com.example\n";
        QTest::newRow("shell") << "com.example;reboot" << "com.example;reboot";
        QTest::newRow("dash") << QString("com.exam-ple") << QString("com.exam-ple");
    }

    void badPackageNames() {
        QFETCH(QString, package);
        QFETCH(QString, monogramOf);
        FakePhone phone;
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon(package, {}, &mime), IconSource::monogram(monogramOf));
        QCOMPARE(mime, QString("image/svg+xml"));
        QVERIFY(phone.calls().isEmpty());
        QVERIFY(!QFileInfo::exists(m_cache));
    }

    void unicodePackageNameIsAccepted() {
        // Python's \w is Unicode: such a name is asked for, not refused.
        FakePhone phone;
        phone.state = "unauthorized";
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        source.icon(QString::fromUtf8("com.ex\xC3\xA4mple_2.app"), {}, &mime);
        QCOMPARE(phone.calls().size(), 1);
    }

    void concurrentCallsDoNotInterleave() {
        FakePhone phone;
        phone.delayMs = 2;
        QStringList packages;
        for (int i = 0; i < 6; ++i) {
            const QString package = QString("com.thread%1.app").arg(i);
            phone.install(package, {{"base.apk", launcherApk()}});
            packages << package;
        }
        // Each package asked for twice at once.
        const QStringList requests = packages + packages;
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QThreadPool pool;
        pool.setMaxThreadCount(int(requests.size()));
        const QList<QByteArray> results = QtConcurrent::blockingMapped<QList<QByteArray>>(
            &pool, requests, [&](const QString &package) { return source.icon(package, {}, nullptr); });
        for (const QByteArray &result : results)
            QCOMPARE(result, png("xxhdpi"));

        const QList<FakePhone::Call> calls = phone.calls();
        for (const QString &package : packages) {
            QList<int> positions;
            int listings = 0;
            for (int i = 0; i < calls.size(); ++i) {
                if (calls[i].arguments == QStringList{"get-state"})
                    continue;
                if (calls[i].arguments.join(' ').contains("/" + package + "-") || calls[i].arguments.contains(package))
                    positions << i;
                if (calls[i].arguments.mid(0, 3) == QStringList{"shell", "pm", "path"} && calls[i].arguments.at(3) == package)
                    ++listings;
            }
            QVERIFY(!positions.isEmpty());
            // One package's device calls run together, no other package's between.
            QVERIFY2(positions.last() - positions.first() + 1 == positions.size(),
                     qPrintable(QString("%1: calls %2..%3, %4 of them")
                                    .arg(package)
                                    .arg(positions.first())
                                    .arg(positions.last())
                                    .arg(positions.size())));
            // The second request found the first one's cache after the lock.
            QCOMPARE(listings, 1);
        }
    }
};

} // namespace

int runIconTests(int argc, char **argv) {
    IconTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "icon_tests.moc"
