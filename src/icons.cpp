#include "icons.h"

#include "apk.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <optional>

// A port of android-diagnoser's icons.py at 7b2d820. The order of attempts,
// the size limits and the adb commands are the same; the Python names are
// noted where a function maps onto one.

namespace {

// The phone cannot be asked for an icon right now (_Unavailable): adb did not
// start, timed out, or dd failed. The package gets a monogram and no miss.
struct Unavailable {};

constexpr int kRunTimeout = 20000;
constexpr int kReadyTimeout = 3000;
constexpr int kWalkerTimeout = 50000;
// Written into each .miss file. Raise it whenever the reader learns to find
// icons it used to miss, so old misses are tried again.
const QByteArray kReaderVersion = QByteArrayLiteral("2\n");

QString shellQuote(const QString &text) {
    QString quoted = text;
    quoted.replace('\'', "'\\''");
    return '\'' + quoted + '\'';
}

// Where an entry's data starts in the APK, and how it is stored (_locate).
struct Located {
    qint64 dataStart = 0;
    quint16 method = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
};

// One package's walk through the APKs on one phone. Every process call goes
// through Adb::run, which adds -s <serial> when there is one.
class Device {
public:
    Device(const QString &serial, const Runner &runner, const QString &adbPath)
        : m_adb(serial, runner, adbPath), m_hasAdb(!adbPath.isEmpty()) {}

    // _device_ready: `adb get-state` says device within 3 s.
    bool ready() {
        if (!m_hasAdb)
            return false;
        const ProcessResult result = m_adb.run({"get-state"}, kReadyTimeout);
        return result.started && !result.timedOut && result.code == 0 && result.out.contains("device");
    }

    // _extract, after the ready check: ic_launcher from the APKs, else the
    // manifest's icon. Empty when neither gives a png or webp.
    QByteArray extract(const QString &package) {
        const QStringList paths = apkPaths(package);
        QString bestName, bestPath;
        int bestRank = -1;
        for (const QString &path : paths) {
            const QString chosen = apk::chooseLauncher(entryNames(path));
            if (!chosen.isEmpty() && apk::rank(chosen) > bestRank) {
                bestName = chosen;
                bestRank = apk::rank(chosen);
                bestPath = path;
            }
        }
        if (!bestName.isEmpty() && !bestPath.isEmpty()) {
            const QByteArray image = readEntry(bestPath, bestName);
            if (!image.isEmpty())
                return image;
        }
        return iconFromManifest(paths);
    }

private:
    // _run: stdout whatever the exit code; a failure to start or a timeout
    // makes the phone unavailable.
    ProcessResult call(const QStringList &arguments, int timeoutMs) {
        const ProcessResult result = m_adb.run(arguments, timeoutMs);
        if (!result.started || result.timedOut)
            throw Unavailable();
        return result;
    }

    QString run(const QStringList &arguments) { return QString::fromUtf8(call(arguments, kRunTimeout).out); }

    // _dd: count bytes from skip. Toybox 0.8.13 (Android 17) takes byte
    // offsets with a large block (iflag=skip_bytes,count_bytes): a 200,000
    // byte read took 112 ms against 387 ms at bs=1, same bytes. An older
    // toybox that refuses the flag gets bs=1, as icons.py read it, from the
    // first refusal on. One lookup reads the same tail and directory more
    // than once, so every read is kept for the rest of it. A non-zero exit
    // is a failure (check_output).
    QByteArray dd(const QString &path, qint64 skip, qint64 count) {
        if (count <= 0)
            return {};
        const QString key = QString("%1\n%2\n%3").arg(path).arg(skip).arg(count);
        if (const auto cached = m_reads.constFind(key); cached != m_reads.cend())
            return cached.value();
        if (m_fastReads != 0) {
            const ProcessResult result =
                call({"exec-out", "toybox", "dd", "if=" + path, "bs=65536", "iflag=skip_bytes,count_bytes",
                      QString("skip=%1").arg(skip), QString("count=%1").arg(count), "status=none"},
                     kRunTimeout);
            if (result.code == 0) {
                m_fastReads = 1;
                m_reads.insert(key, result.out);
                return result.out;
            }
            if (m_fastReads == 1)
                throw Unavailable();
            m_fastReads = 0;
        }
        const ProcessResult result = call({"exec-out", "toybox", "dd", "if=" + path, "bs=1", QString("skip=%1").arg(skip),
                                           QString("count=%1").arg(count), "status=none"},
                                          kRunTimeout);
        if (result.code != 0)
            throw Unavailable();
        m_reads.insert(key, result.out);
        return result.out;
    }

    // _apk_paths: `pm path` lines.
    QStringList apkPaths(const QString &package) {
        QStringList paths;
        for (const QString &raw : run({"shell", "pm", "path", package}).split(QRegularExpression("\r\n|\r|\n"))) {
            const QString line = raw.trimmed();
            if (line.startsWith("package:"))
                paths.append(line.section(':', 1));
        }
        return paths;
    }

    // _file_size: the fifth field of `ls -l`, or 0. Kept for the lookup.
    qint64 fileSize(const QString &path) {
        if (const auto cached = m_sizes.constFind(path); cached != m_sizes.cend())
            return cached.value();
        const qint64 size = listedSize(path);
        m_sizes.insert(path, size);
        return size;
    }

    qint64 listedSize(const QString &path) {
        const QStringList parts = run({"shell", "ls", "-l", path}).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() < 5)
            return 0;
        bool ok = false;
        const qint64 size = parts.at(4).toLongLong(&ok);
        return ok ? size : 0;
    }

    // _entry_names: the launcher-looking names in the central directory,
    // found from the last 256,000 bytes of the APK.
    QStringList entryNames(const QString &path) {
        const qint64 size = fileSize(path);
        if (size <= 0)
            return {};
        const qint64 tailSize = std::min<qint64>(size, 256000);
        const apk::Eocd eocd = apk::findEocd(dd(path, size - tailSize, tailSize));
        if (!eocd.found)
            return {};
        if (eocd.directorySize == 0 || eocd.directorySize > 2000000 || eocd.directoryOffset == 0xFFFFFFFFu)
            return {};
        QStringList names;
        for (const apk::ZipEntry &entry : apk::centralDirectory(dd(path, eocd.directoryOffset, eocd.directorySize)))
            if (apk::isLauncher(entry.name))
                names.append(entry.name);
        return names;
    }

    // _read_entry: the named entry if it is at most 400,000 bytes compressed,
    // stored or deflated, and a png or webp.
    QByteArray readEntry(const QString &path, const QString &name) {
        const qint64 size = fileSize(path);
        const qint64 tailSize = std::min<qint64>(size, 256000);
        const apk::Eocd eocd = apk::findEocd(dd(path, std::max<qint64>(size - tailSize, 0), tailSize));
        if (!eocd.found)
            return {};
        std::optional<apk::ZipEntry> found;
        for (const apk::ZipEntry &entry : apk::centralDirectory(dd(path, eocd.directoryOffset, eocd.directorySize))) {
            if (entry.name == name && entry.compressedSize <= 400000) {
                found = entry;
                break;
            }
        }
        if (!found)
            return {};
        const int offset = apk::localDataOffset(dd(path, found->localOffset, 30), true);
        if (offset < 0)
            return {};
        const QByteArray data = dd(path, qint64(found->localOffset) + offset, found->compressedSize);
        QByteArray raw;
        if (found->method == 0) {
            raw = data;
        } else if (found->method == 8) {
            bool ok = false;
            raw = apk::inflateRaw(data, &ok);
            if (!ok)
                return {};
        } else {
            return {};
        }
        if (apk::mime(raw) == "image/svg+xml")
            return {};
        return raw;
    }

    // _locate: an entry's data start and sizes, from the last 400,000 bytes.
    std::optional<Located> locate(const QString &path, const QString &want) {
        const qint64 size = fileSize(path);
        if (size <= 0)
            return std::nullopt;
        const apk::Eocd eocd = apk::findEocd(dd(path, std::max<qint64>(size - 400000, 0), std::min<qint64>(size, 400000)));
        if (!eocd.found)
            return std::nullopt;
        if (eocd.directorySize == 0 || eocd.directorySize > 3000000 || eocd.directoryOffset == 0xFFFFFFFFu)
            return std::nullopt;
        for (const apk::ZipEntry &entry : apk::centralDirectory(dd(path, eocd.directoryOffset, eocd.directorySize))) {
            if (entry.name != want)
                continue;
            const int offset = apk::localDataOffset(dd(path, entry.localOffset, 30), false);
            if (offset < 0)
                return std::nullopt;
            return Located{qint64(entry.localOffset) + offset, entry.method, entry.compressedSize, entry.uncompressedSize};
        }
        return std::nullopt;
    }

    // _read_named: an entry's bytes, when it is at most limit compressed.
    QByteArray readNamed(const QString &path, const QString &want, qint64 limit) {
        const std::optional<Located> located = locate(path, want);
        if (!located)
            return {};
        if (located->compressedSize == 0 || located->compressedSize > limit)
            return {};
        QByteArray data = dd(path, located->dataStart, located->compressedSize);
        if (located->method == 8) {
            bool ok = false;
            data = apk::inflateRaw(data, &ok);
            if (!ok)
                return {};
        } else if (located->method != 0) {
            return {};
        }
        return data;
    }

    // _icon_from_manifest: base.apk's (else the first APK's) manifest icon.
    QByteArray iconFromManifest(const QStringList &paths) {
        QString base;
        for (const QString &path : paths) {
            if (path.endsWith("base.apk")) {
                base = path;
                break;
            }
        }
        if (base.isEmpty() && !paths.isEmpty())
            base = paths.first();
        if (base.isEmpty())
            return {};
        const QByteArray manifest = readNamed(base, "AndroidManifest.xml", 500000);
        if (manifest.isEmpty())
            return {};
        const quint32 resourceId = apk::manifestIconId(manifest);
        if (!resourceId)
            return {};
        return followIcon(paths, resourceId, 0);
    }

    // _follow_icon: the best bitmap for the resource, else the layers of an
    // XML (adaptive) icon, three levels deep at most.
    QByteArray followIcon(const QStringList &paths, quint32 resourceId, int depth) {
        if (depth > 3 || !resourceId)
            return {};
        const apk::Found found = resolveResource(paths, resourceId);
        const QString chosen = apk::bestImage(found);
        if (!chosen.isEmpty()) {
            for (const QString &path : paths) {
                const QByteArray image = readNamed(path, chosen, 400000);
                if (!image.isEmpty() && apk::mime(image) != "image/svg+xml")
                    return image;
            }
            return {};
        }
        for (const auto &[density, name] : found) {
            Q_UNUSED(density)
            if (!name.toLower().endsWith(".xml"))
                continue;
            for (const QString &path : paths) {
                const QByteArray blob = readNamed(path, name, 200000);
                if (blob.isEmpty())
                    continue;
                for (const quint32 child : apk::xmlRefs(blob)) {
                    const QByteArray image = followIcon(paths, child, depth + 1);
                    if (!image.isEmpty())
                        return image;
                }
            }
        }
        return {};
    }

    // _resolve_resource: tables up to 1,500,000 bytes are read and indexed
    // here; larger ones (stored, up to 20,000,000) were walked on the phone.
    // With byte-offset reads a table comes across fast (8,000,000 bytes in
    // 676 ms over USB, Pixel 8 Pro), while the walker took 12,176 ms on
    // YouTube's table and found nothing, so it now only runs when the phone
    // reads one byte per block.
    apk::Found resolveResource(const QStringList &paths, quint32 resourceId) {
        apk::Found found;
        QList<QPair<QString, qint64>> large;
        for (const QString &path : paths) {
            const std::optional<Located> located = locate(path, "resources.arsc");
            if (!located)
                continue;
            if (located->method != 0 || located->compressedSize > 20000000)
                continue;
            const qint64 copyLimit = m_fastReads == 1 ? 20000000 : 1500000;
            if (located->compressedSize <= copyLimit) {
                const QByteArray table = dd(path, located->dataStart, located->compressedSize);
                found += apk::resolveInTable(table, resourceId);
            } else {
                large.append({path, located->dataStart});
            }
        }
        if (!apk::bestImage(found).isEmpty())
            return found;
        for (const auto &[path, dataStart] : large) {
            found += deviceResolve(path, dataStart, resourceId);
            if (!apk::bestImage(found).isEmpty())
                break;
        }
        return found;
    }

    // _device_resolve: run the walker on the phone. icons.py fed the script
    // to `sh -s` on stdin; Runner has no stdin, so it goes in as `sh -c`
    // (adb joins shell arguments with spaces, unescaped, like ssh).
    apk::Found deviceResolve(const QString &path, qint64 dataStart, quint32 resourceId) {
        const ProcessResult result =
            m_adb.run({"shell", "sh", "-c", shellQuote(apk::deviceWalker()), "sh", shellQuote(path), QString::number(dataStart),
                       QString::number((resourceId >> 16) & 0xFF), QString::number(resourceId & 0xFFFF)},
                      kWalkerTimeout);
        if (!result.started || result.timedOut)
            return {};
        return apk::parseWalkerOutput(QString::fromUtf8(result.out));
    }

    // -1 not tried yet, 1 the phone's dd takes byte offsets, 0 it doesn't.
    int m_fastReads = -1;
    QHash<QString, QByteArray> m_reads;
    QHash<QString, qint64> m_sizes;
    Adb m_adb;
    bool m_hasAdb;
};

bool packageShaped(const QString &package) {
    // icons.py's _PACKAGE with fullmatch; \w is Unicode there, as here.
    const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z]\\w*(?:\\.[A-Za-z]\\w*)+\\z"),
                                     QRegularExpression::UseUnicodePropertiesOption);
    return pattern.match(package).hasMatch();
}

// CSS hsl() to #rrggbb (CSS Color 4's algorithm). Qt's SVG renderer paints
// hsl() fills black, so the monogram carries the hex colour.
QString hslHex(int hue, double saturation, double lightness) {
    auto channel = [&](int n) {
        const double k = std::fmod(n + hue / 30.0, 12.0);
        const double a = saturation * std::min(lightness, 1 - lightness);
        const double value = lightness - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
        return int(std::lround(value * 255));
    };
    return QString("#%1%2%3")
        .arg(channel(0), 2, 16, QLatin1Char('0'))
        .arg(channel(8), 2, 16, QLatin1Char('0'))
        .arg(channel(4), 2, 16, QLatin1Char('0'));
}

QString xmlEscape(QString text) {
    text.replace('&', "&amp;");
    text.replace('<', "&lt;");
    text.replace('>', "&gt;");
    return text;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

IconSource::IconSource(const QString &cacheDirectory, Runner runner, const QString &adbPath)
    : m_cacheDirectory(cacheDirectory.isEmpty() ? QDir::homePath() + "/.cache/snoopy/icons" : cacheDirectory),
      m_runner(std::move(runner)), m_adbPath(adbPath) {}

QByteArray IconSource::icon(const QString &package, const QString &serial, QString *mime) {
    const auto monogramFor = [&](const QString &name) {
        if (mime)
            *mime = "image/svg+xml";
        return monogram(name);
    };
    const auto image = [&](const QByteArray &data) {
        if (mime)
            *mime = apk::mime(data);
        return data;
    };

    if (!packageShaped(package))
        return monogramFor(package.isEmpty() ? QStringLiteral("?") : package);
    const QDir cache(m_cacheDirectory);
    const QString cached = cache.filePath(package + ".img");
    const QString missing = cache.filePath(package + ".miss");
    // The cache, read without the lock: files are replaced whole.
    const auto fromCache = [&]() -> std::optional<QByteArray> {
        if (QFileInfo(cached).isFile()) {
            const QByteArray data = readFile(cached);
            if (!data.isEmpty())
                return image(data);
        }
        // A miss counts only if this reader wrote it: one from an older
        // reader (which missed YouTube's and Edge's icons) is tried again.
        if (QFileInfo(missing).isFile() && readFile(missing) == kReaderVersion)
            return monogramFor(package);
        return std::nullopt;
    };
    if (const auto hit = fromCache())
        return *hit;

    Device device(serial, m_runner, m_adbPath);
    if (!device.ready())
        return monogramFor(package);

    QMutexLocker lock(&m_mutex);
    // Another thread may have read this package while this one waited.
    if (const auto hit = fromCache())
        return *hit;
    QByteArray found;
    try {
        found = device.extract(package);
    } catch (const Unavailable &) {
        return monogramFor(package);
    } catch (const AdbError &) {
        return monogramFor(package);
    }

    const bool bitmap = !found.isEmpty() && apk::mime(found) != "image/svg+xml";
    if (QDir().mkpath(m_cacheDirectory)) {
        if (bitmap) {
            QSaveFile file(cached);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(found);
                file.commit();
            }
            return image(found);
        }
        QSaveFile file(missing);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(kReaderVersion);
            file.commit();
        }
    } else if (bitmap) {
        return image(found);
    }
    return monogramFor(package);
}

QByteArray IconSource::monogram(const QString &package) {
    const QString tail = package.mid(package.lastIndexOf('.') + 1);
    // One code point, as Python's [:1].
    const int length = tail.size() > 1 && tail.at(0).isHighSurrogate() && tail.at(1).isLowSurrogate() ? 2 : 1;
    const QString letter = tail.isEmpty() ? QStringLiteral("?") : tail.left(length).toUpper();
    const int hue = int(quint8(QCryptographicHash::hash(package.toUtf8(), QCryptographicHash::Sha256).at(0))) % 360;
    return QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
                   "<rect width='64' height='64' rx='16' fill='%1'/>"
                   "<text x='32' y='42' text-anchor='middle' font-size='30' "
                   "font-family='sans-serif' fill='white'>%2</text></svg>")
        .arg(hslHex(hue, 0.38, 0.38), xmlEscape(letter))
        .toUtf8();
}
