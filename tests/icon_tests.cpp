// The launcher-icon reader (icons.cpp, apk.cpp). Every input is built here:
// zips with stored and deflated entries, binary XML, resources.arsc tables,
// and a fake phone that answers the adb commands icons.py uses from
// in-memory files.
#include <QtConcurrent>
#include <QtTest>

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

    // dd calls, as (skip, count).
    QList<QPair<qint64, qint64>> reads() {
        QList<QPair<qint64, qint64>> list;
        for (const Call &call : calls()) {
            if (call.arguments.value(0) == "exec-out")
                list.append({call.arguments.at(5).mid(5).toLongLong(), call.arguments.at(6).mid(6).toLongLong()});
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
        file.write("2\n");
        file.close();
        FakePhone phone;
        phone.install("com.demo.app", {{"base.apk", launcherApk()}});
        IconSource source(m_cache, phone.runner(), "/fake/adb");
        QString mime;
        QCOMPARE(source.icon("com.demo.app", {}, &mime), IconSource::monogram("com.demo.app"));
        QCOMPARE(mime, QString("image/svg+xml"));
        QVERIFY(phone.calls().isEmpty());
    }

    void oldMissIsTriedAgain() {
        // An empty .miss is from the first reader, which missed icons this
        // one finds.
        QVERIFY(QDir().mkpath(m_cache));
        QFile file(m_cache + "/com.demo.app.miss");
        QVERIFY(file.open(QIODevice::WriteOnly));
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
