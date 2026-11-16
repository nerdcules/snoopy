#include "apk.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <zlib.h>

namespace apk {

namespace {

// Little-endian reads. Callers check bounds first.
quint8 u8(const QByteArray &data, qint64 pos) { return quint8(data.at(pos)); }

quint16 u16(const QByteArray &data, qint64 pos) { return quint16(u8(data, pos) | (u8(data, pos + 1) << 8)); }

quint32 u32(const QByteArray &data, qint64 pos) { return quint32(u16(data, pos)) | (quint32(u16(data, pos + 2)) << 16); }

bool fits(const QByteArray &data, qint64 pos, qint64 length) { return pos >= 0 && pos + length <= data.size(); }

QString baseName(const QString &name) { return name.mid(name.lastIndexOf('/') + 1); }

bool bitmapName(const QString &path) {
    const QString lower = path.toLower();
    if ((lower.endsWith(".png") || lower.endsWith(".webp")) && !lower.contains(".9."))
        return true;
    // Shrunk resource paths drop the extension too: Edge's launcher bitmaps
    // are res/HxS, res/6Y0 and so on (Android 17). A name without one may be
    // an image; the bytes read are checked for PNG or WebP before use.
    // A reference (@7f080001) is not a path.
    if (!lower.startsWith("res/"))
        return false;
    const QString file = lower.section('/', -1);
    return !file.isEmpty() && !file.contains('.');
}

// _ulen: a UTF-8 pool length, one byte or two with the high bit set.
quint32 ulen(const QByteArray &data, qint64 &pos) {
    if (pos >= data.size())
        return 0;
    quint32 value = u8(data, pos++);
    if ((value & 0x80) && pos < data.size())
        value = ((value & 0x7F) << 8) | u8(data, pos++);
    return value;
}

// The entry's offset from entriesStart, or -1 (_entry_offset).
qint64 entryOffset(const QByteArray &table, qint64 pos, quint16 headerSize, quint8 flags, quint32 index, quint32 count) {
    if (flags & 0x01) { // sparse: (index, offset / 4) pairs
        const qint64 base = pos + headerSize;
        for (quint32 item = 0; item < count; ++item) {
            const qint64 slot = base + qint64(item) * 4;
            if (!fits(table, slot, 4))
                return -1;
            if (u16(table, slot) == index)
                return qint64(u16(table, slot + 2)) * 4;
        }
        return -1;
    }
    if (flags & 0x02) { // 16-bit offsets / 4
        const qint64 slot = pos + headerSize + qint64(index) * 2;
        if (!fits(table, slot, 2))
            return -1;
        const quint16 units = u16(table, slot);
        return units == 0xFFFF ? -1 : qint64(units) * 4;
    }
    const qint64 slot = pos + headerSize + qint64(index) * 4;
    if (!fits(table, slot, 4))
        return -1;
    const quint32 offset = u32(table, slot);
    return offset == 0xFFFFFFFFu ? -1 : qint64(offset);
}

// One ResXMLTree_attribute: name index, data type and data.
struct Attribute {
    quint32 name;
    quint8 type;
    quint32 data;
};

// The attributes of the start-element chunk at pos, or none when the chunk
// is cut short. minimumSize mirrors the two Python loops: _manifest_icon_id
// checks attributeSize >= 20 before the loop, _xml_refs inside it; both stop
// at the first attribute that doesn't fit.
QList<Attribute> attributes(const QByteArray &blob, qint64 pos, quint16 headerSize, QString *elementName,
                            const QStringList &strings) {
    const qint64 ext = pos + headerSize;
    if (ext + 16 > blob.size())
        return {};
    const quint32 name = u32(blob, ext + 4);
    const quint16 attributeStart = u16(blob, ext + 8);
    const quint16 attributeSize = u16(blob, ext + 10);
    const quint16 attributeCount = u16(blob, ext + 12);
    if (elementName)
        *elementName = name < quint32(strings.size()) ? strings.at(int(name)) : QString();
    QList<Attribute> list;
    if (attributeSize < 20)
        return list;
    const qint64 base = ext + attributeStart;
    for (int item = 0; item < attributeCount; ++item) {
        const qint64 attribute = base + qint64(item) * attributeSize;
        if (attribute + 20 > blob.size())
            break;
        list.append({u32(blob, attribute + 4), u8(blob, attribute + 15), u32(blob, attribute + 16)});
    }
    return list;
}

} // namespace

bool isLauncher(const QString &name) {
    const QString base = baseName(name).toLower();
    if (!base.startsWith("ic_launcher"))
        return false;
    if (base.contains(".9."))
        return false;
    return base.endsWith(".png") || base.endsWith(".webp");
}

int rank(const QString &name) {
    const QString lower = name.toLower();
    int score = 0;
    if (lower.contains("xxxhdpi"))
        score += 40;
    else if (lower.contains("xxhdpi"))
        score += 30;
    else if (lower.contains("xhdpi"))
        score += 20;
    else if (lower.contains("hdpi"))
        score += 10;
    const QString fileName = baseName(lower);
    if (fileName == "ic_launcher.png" || fileName == "ic_launcher.webp")
        score += 8;
    if (fileName.contains("round"))
        score += 2;
    if (fileName.contains("foreground"))
        score -= 4;
    return score;
}

QString chooseLauncher(const QStringList &names) {
    QStringList ranked;
    for (const QString &name : names)
        if (isLauncher(name))
            ranked.append(name);
    if (ranked.isEmpty())
        return {};
    // Python's sort is stable, so the earliest of equal ranks wins.
    std::stable_sort(ranked.begin(), ranked.end(), [](const QString &a, const QString &b) { return rank(a) > rank(b); });
    return ranked.first();
}

QString bestImage(const Found &found) {
    bool any = false;
    QPair<int, QString> best;
    for (const auto &item : found) {
        if (item.first >= 65000 || !bitmapName(item.second))
            continue;
        // The last of the sorted (density, path) tuples.
        if (!any || item.first > best.first || (item.first == best.first && item.second > best.second))
            best = item;
        any = true;
    }
    return any ? best.second : QString();
}

QString mime(const QByteArray &data) {
    if (data.startsWith("\x89PNG"))
        return "image/png";
    if (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
        return "image/webp";
    return "image/svg+xml";
}

Eocd findEocd(const QByteArray &tail) {
    Eocd eocd;
    const qsizetype at = tail.lastIndexOf(QByteArray("PK\x05\x06", 4));
    if (at < 0 || !fits(tail, at + 12, 8))
        return eocd;
    eocd.found = true;
    eocd.directorySize = u32(tail, at + 12);
    eocd.directoryOffset = u32(tail, at + 16);
    return eocd;
}

QList<ZipEntry> centralDirectory(const QByteArray &directory) {
    QList<ZipEntry> entries;
    const QByteArray signature("PK\x01\x02", 4);
    qint64 position = 0;
    while (position + 46 <= directory.size() && directory.mid(position, 4) == signature) {
        ZipEntry entry;
        entry.method = u16(directory, position + 10);
        entry.compressedSize = u32(directory, position + 20);
        entry.uncompressedSize = u32(directory, position + 24);
        const quint16 nameLength = u16(directory, position + 28);
        const quint16 extraLength = u16(directory, position + 30);
        const quint16 commentLength = u16(directory, position + 32);
        entry.localOffset = u32(directory, position + 42);
        entry.name = QString::fromUtf8(directory.mid(position + 46, nameLength));
        entries.append(entry);
        position += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
}

int localDataOffset(const QByteArray &localHeader, bool checkSignature) {
    if (localHeader.size() < 30)
        return -1;
    if (checkSignature && !localHeader.startsWith(QByteArray("PK\x03\x04", 4)))
        return -1;
    return 30 + u16(localHeader, 26) + u16(localHeader, 28);
}

QByteArray inflateRaw(const QByteArray &data, bool *ok) {
    *ok = false;
    z_stream stream{};
    if (inflateInit2(&stream, -15) != Z_OK)
        return {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    QByteArray out;
    char buffer[65536];
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof buffer;
        status = inflate(&stream, Z_NO_FLUSH);
        if (status == Z_OK || status == Z_STREAM_END)
            out.append(buffer, qsizetype(sizeof buffer - stream.avail_out));
        if (status == Z_OK && stream.avail_in == 0 && stream.avail_out != 0)
            status = Z_BUF_ERROR; // ran out of input before the end
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END)
        return {};
    *ok = true;
    return out;
}

namespace {

// A ResStringPool's header, read once; strings are decoded on demand.
struct Pool {
    bool valid = false;
    qint64 offset = 0;
    quint16 headerSize = 0;
    quint32 count = 0;
    quint32 stringsStart = 0;
    bool utf8 = false;
};

Pool poolAt(const QByteArray &data, qint64 offset) {
    Pool pool;
    if (!fits(data, offset, 28))
        return pool;
    pool.offset = offset;
    pool.headerSize = u16(data, offset + 2);
    pool.count = u32(data, offset + 8);
    pool.utf8 = u32(data, offset + 16) & 0x100;
    pool.stringsStart = u32(data, offset + 20);
    pool.valid = fits(data, offset + pool.headerSize, qint64(pool.count) * 4);
    return pool;
}

// String index of the pool; a null QString when it lies outside the data.
QString poolString(const QByteArray &data, const Pool &pool, quint32 index) {
    qint64 pos = pool.offset + pool.stringsStart + u32(data, pool.offset + pool.headerSize + qint64(index) * 4);
    if (pos >= data.size())
        return QString();
    if (pool.utf8) {
        ulen(data, pos); // the UTF-16 length, unused
        const quint32 length = ulen(data, pos);
        return QString::fromUtf8(data.mid(pos, length));
    }
    if (!fits(data, pos, 2))
        return QString();
    quint32 length = u16(data, pos);
    pos += 2;
    if ((length & 0x8000) && fits(data, pos, 2)) {
        length = ((length & 0x7FFF) << 16) | u16(data, pos);
        pos += 2;
    }
    const QByteArray bytes = data.mid(pos, qint64(length) * 2);
    QString text = QString::fromUtf16(reinterpret_cast<const char16_t *>(bytes.constData()), bytes.size() / 2);
    if (bytes.size() % 2)
        text.append(QChar::ReplacementCharacter);
    return text;
}

// A ResTable_config asks for nothing but a density or a platform version:
// every byte after its size field is zero except those.
bool plainConfig(const QByteArray &table, qint64 config) {
    if (!fits(table, config, 4))
        return true;
    const qint64 size = std::min<qint64>(u32(table, config), table.size() - config);
    for (qint64 at = 4; at < size; ++at) {
        if (at == 14 || at == 15 || (at >= 24 && at < 28)) // density; sdkVersion, minorVersion
            continue;
        if (u8(table, config + at) != 0)
            return false;
    }
    return true;
}

// _type_hit: the value of entry index in the ResTable_type chunk at pos.
std::optional<TableValue> typeHit(const QByteArray &table, qint64 pos, quint16 headerSize, quint32 index, const Pool &strings) {
    if (pos + 20 > table.size())
        return std::nullopt;
    const quint8 flags = u8(table, pos + 9);
    const quint32 entryCount = u32(table, pos + 12);
    const quint32 entriesStart = u32(table, pos + 16);
    // A sparse chunk's count is the number of (index, offset) pairs it
    // holds, not the highest index: YouTube's launcher icon is entry 15 of a
    // 5-pair chunk (Android 17). icons.py stopped here for every sparse
    // chunk and found no icon.
    if (!(flags & 0x01) && index >= entryCount)
        return std::nullopt;
    TableValue hit;
    hit.density = fits(table, pos + 34, 2) ? u16(table, pos + 34) : 0;
    hit.sdk = fits(table, pos + 44, 2) ? u16(table, pos + 44) : 0;
    hit.plain = plainConfig(table, pos + 20);
    const qint64 offset = entryOffset(table, pos, headerSize, flags, index, entryCount);
    if (offset < 0)
        return std::nullopt;
    const qint64 entry = pos + entriesStart + offset;
    if (!fits(table, entry, 8))
        return std::nullopt;
    const quint16 entrySize = u16(table, entry);
    const quint16 entryFlags = u16(table, entry + 2);
    if ((entryFlags & 0x0001) || entry + entrySize + 8 > table.size())
        return std::nullopt;
    const qint64 value = entry + entrySize;
    hit.value.type = u8(table, value + 3);
    hit.value.data = u32(table, value + 4);
    if (hit.value.type == TypeString) {
        if (!strings.valid || hit.value.data >= strings.count)
            return std::nullopt;
        hit.value.string = poolString(table, strings, hit.value.data);
    }
    return hit;
}

} // namespace

QStringList stringPool(const QByteArray &data, int offset) {
    const Pool pool = poolAt(data, offset);
    if (!pool.valid)
        return {};
    QStringList values;
    values.reserve(int(pool.count));
    for (quint32 item = 0; item < pool.count; ++item)
        values.append(poolString(data, pool, item));
    return values;
}

QList<TableValue> tableValues(const QByteArray &table, quint32 target) {
    if (table.size() < 12 || !table.startsWith(QByteArray("\x02\x00", 2)))
        return {};
    const quint16 header = u16(table, 2);
    const qint64 end = std::min<qint64>(table.size(), u32(table, 4));
    Pool strings;
    QList<TableValue> found;
    const quint8 typeId = (target >> 16) & 0xFF;
    const quint32 index = target & 0xFFFF;

    // Packages nest one level in a real table; the depth limit only stops a
    // chunk whose header size is 0 from recursing into itself forever.
    std::function<void(qint64, qint64, int)> walk = [&](qint64 start, qint64 stop, int depth) {
        if (depth > 8)
            return;
        qint64 pos = start;
        while (pos + 8 <= stop) {
            const quint16 type = u16(table, pos);
            const quint16 headerSize = u16(table, pos + 2);
            const quint32 size = u32(table, pos + 4);
            if (size < 8 || pos + size > stop)
                return;
            // The first pool with strings in it, as icons.py's
            // `if not strings` (an empty pool was passed over).
            if (type == 0x0001 && (!strings.valid || strings.count == 0)) {
                strings = poolAt(table, pos);
            } else if (type == 0x0200) {
                // Only the target's package: a framework id (0x01...) is not
                // in an app's table, whatever its type and entry numbers.
                if (fits(table, pos + 12, 0) && u32(table, pos + 8) == (target >> 24))
                    walk(pos + headerSize, pos + size, depth + 1);
            } else if (type == 0x0201 && pos + 8 < stop && u8(table, pos + 8) == typeId) {
                if (const auto hit = typeHit(table, pos, headerSize, index, strings))
                    found.append(*hit);
            }
            pos += size;
        }
    };
    walk(header, end, 0);
    return found;
}

Found resolveInTable(const QByteArray &table, quint32 target) {
    Found found;
    for (const TableValue &hit : tableValues(table, target)) {
        if (hit.value.type == TypeString)
            found.append({hit.density, hit.value.string});
        else if (hit.value.type == TypeReference)
            found.append({hit.density, QString("@%1").arg(hit.value.data, 8, 16, QLatin1Char('0'))});
    }
    return found;
}

quint32 manifestIconId(const QByteArray &manifest) {
    if (manifest.size() < 8 || !manifest.startsWith(QByteArray("\x03\x00", 2)))
        return 0;
    QStringList strings;
    QList<quint32> resourceMap;
    qint64 pos = u16(manifest, 2);
    while (pos + 8 <= manifest.size()) {
        const quint16 type = u16(manifest, pos);
        const quint16 header = u16(manifest, pos + 2);
        const quint32 size = u32(manifest, pos + 4);
        if (size < 8)
            break;
        if (type == 0x0001) {
            strings = stringPool(manifest, int(pos));
        } else if (type == 0x0180) {
            resourceMap.clear();
            const qint64 count = (qint64(size) - header) / 4;
            for (qint64 item = 0; item < count && fits(manifest, pos + header + item * 4, 4); ++item)
                resourceMap.append(u32(manifest, pos + header + item * 4));
        } else if (type == 0x0102 && !strings.isEmpty()) {
            QString element;
            const QList<Attribute> list = attributes(manifest, pos, header, &element, strings);
            if (element == "application") {
                for (const Attribute &attribute : list) {
                    const QString name = attribute.name < quint32(strings.size()) ? strings.at(int(attribute.name)) : QString();
                    const quint32 resource = attribute.name < quint32(resourceMap.size()) ? resourceMap.at(int(attribute.name)) : 0;
                    if (attribute.type == 0x01 && (resource == 0x01010002 || name == "icon"))
                        return attribute.data;
                }
            }
        }
        pos += size;
    }
    return 0;
}

QList<quint32> xmlRefs(const QByteArray &blob) {
    if (blob.size() < 8 || !blob.startsWith(QByteArray("\x03\x00", 2)))
        return {};
    QStringList strings;
    QList<quint32> refs;
    qint64 pos = u16(blob, 2);
    while (pos + 8 <= blob.size()) {
        const quint16 type = u16(blob, pos);
        const quint16 header = u16(blob, pos + 2);
        const quint32 size = u32(blob, pos + 4);
        if (size < 8)
            break;
        if (type == 0x0001) {
            strings = stringPool(blob, int(pos));
        } else if (type == 0x0102 && !strings.isEmpty()) {
            for (const Attribute &attribute : attributes(blob, pos, header, nullptr, strings)) {
                const QString name = attribute.name < quint32(strings.size()) ? strings.at(int(attribute.name)) : QString();
                if (attribute.type == 0x01 && (name == "drawable" || name == "icon" || name == "src") &&
                    !refs.contains(attribute.data))
                    refs.append(attribute.data);
            }
        }
        pos += size;
    }
    return refs;
}

QString deviceWalker() {
    // Byte for byte the _WALKER of icons.py at 7b2d820 (2756 characters).
    return QString::fromUtf8(R"WALK(
apk=$1; start=$2; type_id=$3; entry=$4
readb() { set -- $(toybox dd if="$apk" bs=1 skip="$1" count="$2" status=none | toybox od -An -tu1); i=1; for b do eval "B$i=\$b"; i=$((i+1)); done; }
u16() { echo $(( $1 + $2 * 256 )); }
u32() { echo $(( $1 + $2 * 256 + $3 * 65536 + $4 * 16777216 )); }
str_at() {
  readb $((pool + phsz + $1 * 4)) 4
  rel=$(u32 "$B1" "$B2" "$B3" "$B4")
  strpos=$((pool + sstart + rel))
  readb "$strpos" 4
  p=0; if [ "$B1" -ge 128 ]; then p=2; else p=1; fi
  readb $((strpos + p)) 2
  if [ "$B1" -ge 128 ]; then n=$(( (B1 & 127) * 256 + B2 )); p=$((p+2)); else n=$B1; p=$((p+1)); fi
  [ "$n" -gt 180 ] && n=180
  toybox dd if="$apk" bs=1 skip=$((strpos + p)) count="$n" status=none; echo
}
emit() {
  ent=$((pos + es + $1))
  readb "$ent" 8
  esize=$(u16 "$B1" "$B2"); eflags=$(u16 "$B3" "$B4")
  [ $((eflags & 1)) -ne 0 ] && return
  readb $((ent + esize)) 8
  dtype=$B4; data=$(u32 "$B5" "$B6" "$B7" "$B8")
  [ "$dtype" -ne 3 ] && return
  path=$(str_at "$data")
  case $path in
    *.png|*.webp|*.xml) echo "$dens $path";;
  esac
}
readb "$start" 4; th=$(u16 "$B3" "$B4"); pool=$((start + th))
readb "$pool" 24; phsz=$(u16 "$B3" "$B4"); psz=$(u32 "$B5" "$B6" "$B7" "$B8"); sstart=$(u32 "$B21" "$B22" "$B23" "$B24")
pkg=$((pool + psz)); readb "$pkg" 8; pksz=$(u32 "$B5" "$B6" "$B7" "$B8"); ph=$(u16 "$B3" "$B4")
pos=$((pkg + ph)); end=$((pkg + pksz))
while [ "$pos" -lt "$end" ]; do
  readb "$pos" 16
  typ=$(u16 "$B1" "$B2"); hsz=$(u16 "$B3" "$B4"); sz=$(u32 "$B5" "$B6" "$B7" "$B8")
  tid=$B9; tflags=$B10
  if [ "$typ" -eq 513 ] && [ "$tid" -eq "$type_id" ]; then
    readb $((pos + 12)) 8
    ecount=$(u32 "$B1" "$B2" "$B3" "$B4"); es=$(u32 "$B5" "$B6" "$B7" "$B8")
    readb $((pos + 34)) 2; dens=$(u16 "$B1" "$B2")
    if [ "$entry" -lt "$ecount" ]; then
      if [ $((tflags & 1)) -ne 0 ]; then
        bytes=$((ecount * 4))
        if [ "$bytes" -le 65536 ]; then
          set -- $(toybox dd if="$apk" bs=1 skip=$((pos + hsz)) count="$bytes" status=none | toybox od -An -tu1)
          i=0
          while [ "$i" -lt "$ecount" ]; do
            b=$((i * 4 + 1))
            eval "p1=\$$b"; eval "p2=\$$((b+1))"; eval "p3=\$$((b+2))"; eval "p4=\$$((b+3))"
            idx=$(u16 "$p1" "$p2")
            if [ "$idx" -eq "$entry" ]; then emit $(( $(u16 "$p3" "$p4") * 4 )); break; fi
            i=$((i+1))
          done
        fi
      elif [ $((tflags & 2)) -ne 0 ]; then
        readb $((pos + hsz + entry * 2)) 2
        o16=$(u16 "$B1" "$B2")
        [ "$o16" -ne 65535 ] && emit $((o16 * 4))
      else
        readb $((pos + hsz + entry * 4)) 4
        eoff=$(u32 "$B1" "$B2" "$B3" "$B4")
        [ "$eoff" -ne 4294967295 ] && emit "$eoff"
      fi
    fi
  fi
  [ "$sz" -lt 8 ] && break
  pos=$((pos + sz))
done
)WALK");
}

Found parseWalkerOutput(const QString &text) {
    Found found;
    for (const QString &raw : text.split(QRegularExpression("\r\n|\r|\n"))) {
        const QString line = raw.trimmed();
        const qsizetype space = line.indexOf(QRegularExpression("\\s"));
        if (space < 0)
            continue;
        const QString first = line.left(space);
        const QString rest = line.mid(space).trimmed();
        if (rest.isEmpty() || first.isEmpty())
            continue;
        bool digits = true;
        for (const QChar c : first)
            digits = digits && c >= '0' && c <= '9';
        bool ok = false;
        const int density = first.toInt(&ok);
        if (!digits || !ok)
            continue;
        found.append({density, rest});
    }
    return found;
}


// ---- typed values and binary XML ----

namespace {

// Framework attribute ids (android.R.attr), as the XML resource map gives
// them. Attributes are matched on these first and on their names second.
// Checked against Outlook's launcher files on the phone (Android 17):
// drawable, color, width, height, the inset sides, viewportWidth/Height,
// fillColor, pathData, fillAlpha, and the gradient's type, startX..endY,
// centerX/Y, gradientRadius and offset. The others are from android.R.attr
// as recalled, not checked; a wrong one still matches by name unless the
// APK's attribute names were stripped.
namespace attr {
constexpr quint32 kName = 0x01010003;
constexpr quint32 kDrawable = 0x01010199;
constexpr quint32 kColor = 0x010101a5;
constexpr quint32 kAlpha = 0x0101031f;
constexpr quint32 kWidth = 0x01010159;
constexpr quint32 kHeight = 0x01010155;
constexpr quint32 kLeft = 0x010101ad;
constexpr quint32 kTop = 0x010101ae;
constexpr quint32 kRight = 0x010101af;
constexpr quint32 kBottom = 0x010101b0;
constexpr quint32 kGravity = 0x010100af;
constexpr quint32 kSrc = 0x01010119;
constexpr quint32 kInset = 0x010104b5;
constexpr quint32 kInsetLeft = 0x010101b7;
constexpr quint32 kInsetRight = 0x010101b8;
constexpr quint32 kInsetTop = 0x010101b9;
constexpr quint32 kInsetBottom = 0x010101ba;
constexpr quint32 kShape = 0x0101015c;
constexpr quint32 kRadius = 0x010101a8;
// <vector>, <group>, <path>, <clip-path>
constexpr quint32 kViewportWidth = 0x01010402;
constexpr quint32 kViewportHeight = 0x01010403;
constexpr quint32 kFillColor = 0x01010404;
constexpr quint32 kPathData = 0x01010405;
constexpr quint32 kStrokeColor = 0x01010406;
constexpr quint32 kStrokeWidth = 0x01010407;
constexpr quint32 kStrokeLineCap = 0x0101040b;
constexpr quint32 kStrokeLineJoin = 0x0101040c;
constexpr quint32 kStrokeMiterLimit = 0x0101040d;
constexpr quint32 kStrokeAlpha = 0x010104cb;
constexpr quint32 kFillAlpha = 0x010104cc;
constexpr quint32 kFillType = 0x0101051e;
constexpr quint32 kRotation = 0x01010326;
constexpr quint32 kPivotX = 0x010101b5;
constexpr quint32 kPivotY = 0x010101b6;
constexpr quint32 kScaleX = 0x01010324;
constexpr quint32 kScaleY = 0x01010325;
constexpr quint32 kTranslateX = 0x0101045a;
constexpr quint32 kTranslateY = 0x0101045b;
// <gradient> and its <item>s
constexpr quint32 kStartColor = 0x0101019d;
constexpr quint32 kEndColor = 0x0101019e;
constexpr quint32 kCenterColor = 0x0101020b;
constexpr quint32 kAngle = 0x010101a0;
constexpr quint32 kType = 0x010101a1;
constexpr quint32 kCenterX = 0x010101a2;
constexpr quint32 kCenterY = 0x010101a3;
constexpr quint32 kGradientRadius = 0x010101a4;
constexpr quint32 kStartX = 0x01010510;
constexpr quint32 kStartY = 0x01010511;
constexpr quint32 kEndX = 0x01010512;
constexpr quint32 kEndY = 0x01010513;
constexpr quint32 kOffset = 0x01010514;
constexpr quint32 kTileMode = 0; // id not known here: matched by name
} // namespace attr

// A dimension or fraction (TypedValue.complexToFloat).
double complexNumber(quint32 data) {
    static const double radix[4] = {1.0 / (1 << 8), 1.0 / (1 << 15), 1.0 / (1 << 23), 1.0 / (1u << 31)};
    return double(qint32(data & 0xFFFFFF00u)) * radix[(data >> 4) & 3];
}

constexpr int kMaxXmlDepth = 64;
constexpr int kMaxXmlElements = 20000;

} // namespace

bool Value::isNumber() const {
    return type == TypeFloat || type == TypeDimension || type == TypeFraction || type == TypeIntDec || type == TypeIntHex ||
           type == TypeBool;
}

quint32 Value::argb() const { return (type == TypeRgb8 || type == TypeRgb4) ? (data | 0xFF000000u) : data; }

double Value::number() const {
    switch (type) {
    case TypeFloat: {
        float value;
        std::memcpy(&value, &data, sizeof value);
        return std::isfinite(value) ? double(value) : 0.0;
    }
    case TypeDimension:
    case TypeFraction:
        return complexNumber(data);
    case TypeIntDec:
    case TypeIntHex:
        return double(qint32(data));
    case TypeBool:
        return data ? 1.0 : 0.0;
    default:
        return 0.0;
    }
}

const XmlAttribute *XmlElement::attribute(quint32 resourceId, const QString &name) const {
    for (const XmlAttribute &item : attributes)
        if (resourceId && item.resourceId == resourceId)
            return &item;
    // By name: an attribute with no id, or a framework one this reader has
    // the wrong id for. An app's own attribute (0x7f...) never stands in.
    for (const XmlAttribute &item : attributes)
        if (!name.isEmpty() && item.name == name && (item.resourceId == 0 || (item.resourceId >> 24) == 0x01))
            return &item;
    return nullptr;
}

std::optional<XmlElement> parseXml(const QByteArray &blob) {
    if (blob.size() < 8 || u16(blob, 0) != 0x0003)
        return std::nullopt;
    const qint64 end = std::min<qint64>(blob.size(), u32(blob, 4));
    QStringList strings;
    QList<quint32> resourceMap;
    QList<XmlElement> stack;
    std::optional<XmlElement> root;
    int elements = 0;
    qint64 pos = u16(blob, 2);
    if (pos < 8)
        return std::nullopt;
    while (pos + 8 <= end) {
        const quint16 type = u16(blob, pos);
        const quint16 header = u16(blob, pos + 2);
        const quint32 size = u32(blob, pos + 4);
        if (size < 8 || header > size || pos + size > end)
            return std::nullopt;
        if (type == 0x0001) {
            strings = stringPool(blob, int(pos));
        } else if (type == 0x0180) {
            resourceMap.clear();
            for (qint64 at = pos + header; at + 4 <= pos + size; at += 4)
                resourceMap.append(u32(blob, at));
        } else if (type == 0x0102) {
            const qint64 ext = pos + header;
            if (ext + 20 > pos + size || root || stack.size() >= kMaxXmlDepth || ++elements > kMaxXmlElements)
                return std::nullopt;
            XmlElement element;
            element.name = strings.value(int(u32(blob, ext + 4)));
            const quint16 attributeStart = u16(blob, ext + 8);
            const quint16 attributeSize = u16(blob, ext + 10);
            const quint16 attributeCount = u16(blob, ext + 12);
            if (attributeCount && attributeSize < 20)
                return std::nullopt;
            for (int item = 0; item < attributeCount; ++item) {
                const qint64 at = ext + attributeStart + qint64(item) * attributeSize;
                if (at + 20 > pos + size)
                    return std::nullopt;
                XmlAttribute attribute;
                const quint32 name = u32(blob, at + 4);
                attribute.name = strings.value(int(name));
                attribute.resourceId = name < quint32(resourceMap.size()) ? resourceMap.at(int(name)) : 0;
                attribute.value.type = u8(blob, at + 15);
                attribute.value.data = u32(blob, at + 16);
                if (attribute.value.type == TypeString)
                    attribute.value.string = strings.value(int(attribute.value.data));
                element.attributes.append(attribute);
            }
            stack.append(element);
        } else if (type == 0x0103) {
            if (stack.isEmpty())
                return std::nullopt;
            XmlElement element = stack.takeLast();
            if (stack.isEmpty())
                root = std::move(element);
            else
                stack.last().children.append(std::move(element));
        }
        pos += size;
    }
    if (!stack.isEmpty() || !root)
        return std::nullopt;
    return root;
}

// ---- vector drawables ----

namespace {

double numberOf(const XmlElement &element, quint32 id, const QString &name, double fallback) {
    const XmlAttribute *found = element.attribute(id, name);
    return found && found->value.isNumber() ? found->value.number() : fallback;
}

QString fmt(double value) { return QString::number(value, 'g', 8); }

QString rgbHex(quint32 argb) { return QString("#%1").arg(argb & 0xFFFFFF, 6, 16, QLatin1Char('0')); }

double alphaOf(quint32 argb) { return double(argb >> 24) / 255.0; }

quint32 withAlpha(quint32 argb, double factor) {
    const double alpha = std::clamp(alphaOf(argb) * factor, 0.0, 1.0);
    return (argb & 0xFFFFFF) | (quint32(std::lround(alpha * 255)) << 24);
}

QString xmlText(QString text) {
    text.replace('&', "&amp;");
    text.replace('<', "&lt;");
    text.replace('>', "&gt;");
    text.replace('"', "&quot;");
    return text;
}

// The colour value of attribute id on element, through lookup.
Paint paintAttribute(const XmlElement &element, quint32 id, const QString &name, const PaintLookup &lookup) {
    const XmlAttribute *found = element.attribute(id, name);
    return found ? paintOf(found->value, lookup) : Paint{};
}

// A colour state list item without states: only colour, alpha and lStar.
bool stateless(const XmlElement &item) {
    for (const XmlAttribute &attribute : item.attributes) {
        if (attribute.resourceId == attr::kColor || attribute.resourceId == attr::kAlpha || attribute.resourceId == attr::kDrawable)
            continue;
        if (attribute.name == "color" || attribute.name == "alpha" || attribute.name == "lStar" || attribute.name == "drawable")
            continue;
        return false;
    }
    return true;
}

// The item a state list shows in no particular state: the first without
// states, else the last.
const XmlElement *defaultItem(const XmlElement &list) {
    const XmlElement *last = nullptr;
    for (const XmlElement &child : list.children) {
        if (child.name != "item")
            continue;
        if (stateless(child))
            return &child;
        last = &child;
    }
    return last;
}

// SVG text for one vector drawable; gradients and clips go into defs.
class SvgWriter {
public:
    explicit SvgWriter(const PaintLookup &lookup) : m_lookup(lookup) {}

    QString defs;

    QString children(const XmlElement &parent) {
        QString out;
        int clips = 0;
        for (const XmlElement &child : parent.children) {
            if (child.name == "group") {
                out += "<g" + transform(child) + ">" + children(child) + "</g>";
            } else if (child.name == "path") {
                out += path(child);
            } else if (child.name == "clip-path") {
                // Clips the rest of its group.
                const QString d = pathData(child);
                if (d.isEmpty())
                    continue;
                const QString id = QString("c%1").arg(m_next++);
                defs += "<clipPath id=\"" + id + "\"><path d=\"" + xmlText(d) + "\"/></clipPath>";
                out += "<g clip-path=\"url(#" + id + ")\">";
                ++clips;
            }
        }
        for (int i = 0; i < clips; ++i)
            out += "</g>";
        return out;
    }

private:
    static QString pathData(const XmlElement &element) {
        const XmlAttribute *found = element.attribute(attr::kPathData, "pathData");
        return found && found->value.type == TypeString ? found->value.string.trimmed() : QString();
    }

    // VectorDrawable's group matrix: translate(-pivot), scale, rotate,
    // translate(pivot + translate), applied in that order.
    static QString transform(const XmlElement &group) {
        const double rotation = numberOf(group, attr::kRotation, "rotation", 0);
        const double pivotX = numberOf(group, attr::kPivotX, "pivotX", 0);
        const double pivotY = numberOf(group, attr::kPivotY, "pivotY", 0);
        const double scaleX = numberOf(group, attr::kScaleX, "scaleX", 1);
        const double scaleY = numberOf(group, attr::kScaleY, "scaleY", 1);
        const double translateX = numberOf(group, attr::kTranslateX, "translateX", 0);
        const double translateY = numberOf(group, attr::kTranslateY, "translateY", 0);
        if (rotation == 0 && scaleX == 1 && scaleY == 1 && translateX == 0 && translateY == 0)
            return {};
        return QString(" transform=\"translate(%1 %2) rotate(%3) scale(%4 %5) translate(%6 %7)\"")
            .arg(fmt(translateX + pivotX), fmt(translateY + pivotY), fmt(rotation), fmt(scaleX), fmt(scaleY), fmt(-pivotX),
                 fmt(-pivotY));
    }

    // An SVG paint for p: a colour or url(#gradient), and its opacity.
    QPair<QString, double> svgPaint(const Paint &p, double alpha) {
        switch (p.kind) {
        case Paint::Solid:
        case Paint::Sweep:
            return {rgbHex(p.argb), alphaOf(p.argb) * alpha};
        case Paint::Linear:
        case Paint::Radial: {
            const QString id = QString("g%1").arg(m_next++);
            static const char *const spread[] = {"pad", "repeat", "reflect"};
            QString gradient = p.kind == Paint::Linear
                                   ? QString("<linearGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" x1=\"%2\" y1=\"%3\" "
                                             "x2=\"%4\" y2=\"%5\"")
                                         .arg(id, fmt(p.x1), fmt(p.y1), fmt(p.x2), fmt(p.y2))
                                   : QString("<radialGradient id=\"%1\" gradientUnits=\"userSpaceOnUse\" cx=\"%2\" cy=\"%3\" "
                                             "r=\"%4\" fx=\"%2\" fy=\"%3\"")
                                         .arg(id, fmt(p.cx), fmt(p.cy), fmt(p.r));
            gradient += QString(" spreadMethod=\"%1\">").arg(spread[std::clamp(p.tile, 0, 2)]);
            for (const auto &[offset, argb] : p.stops)
                gradient += QString("<stop offset=\"%1\" stop-color=\"%2\" stop-opacity=\"%3\"/>")
                                .arg(fmt(std::clamp(offset, 0.0, 1.0)), rgbHex(argb), fmt(alphaOf(argb)));
            gradient += p.kind == Paint::Linear ? "</linearGradient>" : "</radialGradient>";
            defs += gradient;
            return {"url(#" + id + ")", alpha};
        }
        case Paint::None:
            break;
        }
        return {"none", 1};
    }

    QString path(const XmlElement &element) {
        const QString d = pathData(element);
        if (d.isEmpty())
            return {};
        const auto [fill, fillOpacity] = svgPaint(paintAttribute(element, attr::kFillColor, "fillColor", m_lookup),
                                                   numberOf(element, attr::kFillAlpha, "fillAlpha", 1));
        QString out = "<path d=\"" + xmlText(d) + "\" fill=\"" + fill + "\"";
        if (fill != "none" && fillOpacity < 1)
            out += " fill-opacity=\"" + fmt(fillOpacity) + "\"";
        if (numberOf(element, attr::kFillType, "fillType", 0) == 1)
            out += " fill-rule=\"evenodd\"";
        const double width = numberOf(element, attr::kStrokeWidth, "strokeWidth", 0);
        const Paint strokePaint = paintAttribute(element, attr::kStrokeColor, "strokeColor", m_lookup);
        if (width > 0 && strokePaint.kind != Paint::None) {
            const auto [stroke, strokeOpacity] = svgPaint(strokePaint, numberOf(element, attr::kStrokeAlpha, "strokeAlpha", 1));
            static const char *const caps[] = {"butt", "round", "square"};
            static const char *const joins[] = {"miter", "round", "bevel"};
            out += " stroke=\"" + stroke + "\" stroke-width=\"" + fmt(width) + "\"";
            if (strokeOpacity < 1)
                out += " stroke-opacity=\"" + fmt(strokeOpacity) + "\"";
            out += QString(" stroke-linecap=\"%1\" stroke-linejoin=\"%2\" stroke-miterlimit=\"%3\"")
                       .arg(caps[std::clamp(int(numberOf(element, attr::kStrokeLineCap, "strokeLineCap", 0)), 0, 2)],
                            joins[std::clamp(int(numberOf(element, attr::kStrokeLineJoin, "strokeLineJoin", 0)), 0, 2)],
                            fmt(numberOf(element, attr::kStrokeMiterLimit, "strokeMiterLimit", 4)));
        }
        return out + "/>";
    }

    const PaintLookup &m_lookup;
    int m_next = 0;
};

} // namespace

Paint paintOf(const Value &value, const PaintLookup &lookup) {
    Paint paint;
    if (value.isColor()) {
        paint.kind = Paint::Solid;
        paint.argb = value.argb();
    } else if (value.type == TypeReference && value.data && lookup) {
        paint = lookup(value.data);
    } else if (value.type == TypeString && value.string.startsWith('#')) {
        const QColor color = QColor::fromString(value.string);
        if (color.isValid()) {
            paint.kind = Paint::Solid;
            paint.argb = color.rgba();
        }
    }
    return paint;
}

Paint paintOfXml(const XmlElement &root, const PaintLookup &lookup) {
    if (root.name == "selector") {
        const XmlElement *item = defaultItem(root);
        if (!item)
            return {};
        Paint paint = paintAttribute(*item, attr::kColor, "color", lookup);
        if (paint.kind == Paint::Solid)
            paint.argb = withAlpha(paint.argb, numberOf(*item, attr::kAlpha, "alpha", 1));
        return paint;
    }
    if (root.name != "gradient")
        return {};
    Paint paint;
    for (const XmlElement &item : root.children) {
        if (item.name != "item")
            continue;
        const Paint color = paintAttribute(item, attr::kColor, "color", lookup);
        if (color.kind == Paint::Solid)
            paint.stops.append({numberOf(item, attr::kOffset, "offset", 0), color.argb});
    }
    if (paint.stops.isEmpty()) {
        const Paint start = paintAttribute(root, attr::kStartColor, "startColor", lookup);
        const Paint center = paintAttribute(root, attr::kCenterColor, "centerColor", lookup);
        const Paint end = paintAttribute(root, attr::kEndColor, "endColor", lookup);
        if (start.kind == Paint::Solid)
            paint.stops.append({0.0, start.argb});
        if (center.kind == Paint::Solid)
            paint.stops.append({0.5, center.argb});
        if (end.kind == Paint::Solid)
            paint.stops.append({1.0, end.argb});
    }
    if (paint.stops.isEmpty())
        return {};
    paint.argb = paint.stops.first().second;
    const int type = int(numberOf(root, attr::kType, "type", 0));
    paint.tile = std::max(0, int(numberOf(root, attr::kTileMode, "tileMode", 0)));
    paint.cx = numberOf(root, attr::kCenterX, "centerX", 0);
    paint.cy = numberOf(root, attr::kCenterY, "centerY", 0);
    if (type == 1) {
        paint.kind = Paint::Radial;
        paint.r = numberOf(root, attr::kGradientRadius, "gradientRadius", 0);
        if (paint.r <= 0) // Android refuses a radial gradient without a radius
            paint.kind = Paint::Solid;
    } else if (type == 2) {
        paint.kind = Paint::Sweep;
    } else {
        paint.kind = Paint::Linear;
        paint.x1 = numberOf(root, attr::kStartX, "startX", 0);
        paint.y1 = numberOf(root, attr::kStartY, "startY", 0);
        paint.x2 = numberOf(root, attr::kEndX, "endX", 0);
        paint.y2 = numberOf(root, attr::kEndY, "endY", 0);
    }
    return paint;
}

QByteArray vectorToSvg(const XmlElement &root, const PaintLookup &lookup) {
    if (root.name != "vector")
        return {};
    const double width = numberOf(root, attr::kViewportWidth, "viewportWidth", 0);
    const double height = numberOf(root, attr::kViewportHeight, "viewportHeight", 0);
    if (!(width > 0) || !(height > 0))
        return {};
    SvgWriter writer(lookup);
    const QString body = writer.children(root);
    const double alpha = std::clamp(numberOf(root, attr::kAlpha, "alpha", 1), 0.0, 1.0);
    return QString("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %1 %2\" width=\"%1\" height=\"%2\" "
                   "preserveAspectRatio=\"none\"><defs>%3</defs><g opacity=\"%4\">%5</g></svg>")
        .arg(fmt(width), fmt(height), writer.defs, fmt(alpha), body)
        .toUtf8();
}

// ---- drawing a drawable ----

namespace {

constexpr int kMaxDrawDepth = 12;
// Configurations of one drawable tried before giving up on it.
constexpr int kMaxCandidates = 3;

// The framework colours an app's table cannot give: @android:color/white
// (eBay's icon background, Android 17), black and transparent.
std::optional<quint32> frameworkColor(quint32 id) {
    switch (id) {
    case 0x0106000b:
        return 0xFFFFFFFFu;
    case 0x0106000c:
        return 0xFF000000u;
    case 0x0106000d:
        return 0x00000000u;
    default:
        return std::nullopt;
    }
}

bool binaryXmlBytes(const QByteArray &bytes) { return bytes.startsWith(QByteArray("\x03\x00\x08\x00", 4)); }

class Renderer {
public:
    Renderer(const Resources &resources, QStringList *met) : m_resources(resources), m_met(met) {}

    bool drawResource(QPainter &painter, const QRectF &rect, quint32 id, double dp, int depth) {
        // A resource that refers back to itself, through however many
        // others, ends here.
        if (depth > kMaxDrawDepth || !id || m_drawing.contains(id))
            return false;
        m_drawing.insert(id);
        bool drawn = false;
        int tried = 0;
        for (const Value &value : candidates(id)) {
            if ((drawn = drawValue(painter, rect, value, dp, depth + 1)))
                break;
            if (++tried >= kMaxCandidates)
                break;
        }
        m_drawing.remove(id);
        return drawn;
    }

private:
    void note(const QString &what) {
        if (m_met && !m_met->contains(what))
            m_met->append(what);
    }

    // A resource's values, the one Android would pick on a phone in no
    // particular mode first: plain configurations, anydpi before the
    // densest bitmap, the newest platform version.
    QList<Value> candidates(quint32 id) {
        QList<TableValue> values = m_resources.values ? m_resources.values(id) : QList<TableValue>();
        if (values.isEmpty())
            if (const std::optional<quint32> color = frameworkColor(id))
                return {Value{TypeArgb8, *color, {}}};
        const auto densityRank = [](int density) { return density == 65534 ? 100000 : density == 65535 ? 1 : density == 0 ? 160 : density; };
        std::stable_sort(values.begin(), values.end(), [&](const TableValue &a, const TableValue &b) {
            if (a.plain != b.plain)
                return a.plain;
            if (densityRank(a.density) != densityRank(b.density))
                return densityRank(a.density) > densityRank(b.density);
            return a.sdk > b.sdk;
        });
        QList<Value> list;
        for (const TableValue &value : values)
            list.append(value.value);
        return list;
    }

    Paint paint(quint32 id, int depth) {
        if (depth > kMaxDrawDepth || m_painting.contains(id))
            return {};
        m_painting.insert(id);
        const auto done = qScopeGuard([&] { m_painting.remove(id); });
        int tried = 0;
        for (const Value &value : candidates(id)) {
            Paint found;
            if (value.isColor() || value.type == TypeReference) {
                found = paintOf(value, [&](quint32 next) { return paint(next, depth + 1); });
            } else if (value.type == TypeString) {
                if (const auto root = parseXml(file(value.string)))
                    found = paintOfXml(*root, [&](quint32 next) { return paint(next, depth + 1); });
            }
            if (found.kind != Paint::None)
                return found;
            if (++tried >= kMaxCandidates)
                break;
        }
        return {};
    }

    QByteArray file(const QString &path) { return m_resources.file ? m_resources.file(path) : QByteArray(); }

    // The files an XML file's references lead to (two hops at most, the
    // first choice of each), handed to prefetch.
    void prefetchReferences(const XmlElement &root) {
        if (!m_resources.prefetch)
            return;
        QStringList names;
        std::function<void(quint32, int)> follow = [&](quint32 id, int hops) {
            const QList<Value> values = candidates(id);
            if (values.isEmpty() || hops > 1)
                return;
            const Value &best = values.first();
            if (best.type == TypeString && !names.contains(best.string))
                names.append(best.string);
            else if (best.type == TypeReference)
                follow(best.data, hops + 1);
        };
        std::function<void(const XmlElement &)> walk = [&](const XmlElement &element) {
            for (const XmlAttribute &attribute : element.attributes)
                if (attribute.value.type == TypeReference && attribute.value.data)
                    follow(attribute.value.data, 0);
            for (const XmlElement &child : element.children)
                walk(child);
        };
        walk(root);
        if (names.size() > 1)
            m_resources.prefetch(names);
    }

    bool fill(QPainter &painter, const QRectF &rect, const Paint &paint) {
        if (paint.kind == Paint::None)
            return false;
        painter.fillRect(rect, QColor::fromRgba(paint.argb));
        return true;
    }

    bool drawValue(QPainter &painter, const QRectF &rect, const Value &value, double dp, int depth) {
        if (value.isColor()) {
            note("color");
            return fill(painter, rect, paintOf(value, {}));
        }
        if (value.type == TypeReference)
            return drawResource(painter, rect, value.data, dp, depth);
        if (value.type == TypeString)
            return drawFile(painter, rect, value.string, dp, depth);
        return false;
    }

    bool drawFile(QPainter &painter, const QRectF &rect, const QString &path, double dp, int depth) {
        const QByteArray bytes = file(path);
        if (bytes.isEmpty())
            return false;
        if (binaryXmlBytes(bytes)) {
            const auto root = parseXml(bytes);
            if (!root)
                return false;
            prefetchReferences(*root);
            return drawElement(painter, rect, *root, dp, depth);
        }
        const QImage image = QImage::fromData(bytes);
        if (image.isNull())
            return false;
        note("bitmap");
        painter.drawImage(rect, image);
        return true;
    }

    // The drawable an element holds: its android:drawable (or src), else
    // its first child element.
    bool drawContent(QPainter &painter, const QRectF &rect, const XmlElement &element, double dp, int depth,
                     quint32 id = attr::kDrawable, const QString &name = "drawable") {
        if (const XmlAttribute *found = element.attribute(id, name))
            return drawValue(painter, rect, found->value, dp, depth);
        for (const XmlElement &child : element.children)
            return drawElement(painter, rect, child, dp, depth);
        return false;
    }

    // A dimension in dp, or a fraction of extent.
    static double length(const XmlElement &element, quint32 id, const QString &name, double extent, double dp) {
        const XmlAttribute *found = element.attribute(id, name);
        if (!found || !found->value.isNumber())
            return 0;
        if (found->value.type == TypeFraction)
            return found->value.number() * extent;
        return found->value.number() * dp;
    }

    bool drawElement(QPainter &painter, const QRectF &rect, const XmlElement &element, double dp, int depth) {
        if (depth > kMaxDrawDepth)
            return false;
        const QString &name = element.name;
        if (name == "adaptive-icon")
            return drawAdaptive(painter, rect, element, depth);
        if (name == "vector") {
            const QByteArray svg = vectorToSvg(element, [&](quint32 id) { return paint(id, depth + 1); });
            QSvgRenderer renderer(svg);
            if (svg.isEmpty() || !renderer.isValid())
                return false;
            note("vector");
            renderer.setAspectRatioMode(Qt::IgnoreAspectRatio);
            renderer.render(&painter, rect);
            return true;
        }
        if (name == "bitmap" || name == "nine-patch") {
            note(name);
            return drawContent(painter, rect, element, dp, depth + 1, attr::kSrc, "src");
        }
        if (name == "color") {
            note(name);
            return fill(painter, rect, paintAttribute(element, attr::kColor, "color", [&](quint32 id) { return paint(id, depth + 1); }));
        }
        if (name == "inset") {
            note(name);
            const double w = rect.width(), h = rect.height();
            const auto side = [&](quint32 id, const QString &key, double extent) {
                const double own = length(element, id, key, extent, dp);
                return own != 0 ? own : length(element, attr::kInset, "inset", extent, dp);
            };
            const QRectF inner = rect.adjusted(side(attr::kInsetLeft, "insetLeft", w), side(attr::kInsetTop, "insetTop", h),
                                               -side(attr::kInsetRight, "insetRight", w), -side(attr::kInsetBottom, "insetBottom", h));
            return inner.isValid() && drawContent(painter, inner, element, dp, depth + 1);
        }
        if (name == "layer-list") {
            note(name);
            bool drawn = false;
            for (const XmlElement &item : element.children) {
                if (item.name != "item")
                    continue;
                QRectF inner = rect.adjusted(length(item, attr::kLeft, "left", rect.width(), dp), length(item, attr::kTop, "top", rect.height(), dp),
                                             -length(item, attr::kRight, "right", rect.width(), dp),
                                             -length(item, attr::kBottom, "bottom", rect.height(), dp));
                const double width = length(item, attr::kWidth, "width", rect.width(), dp);
                const double height = length(item, attr::kHeight, "height", rect.height(), dp);
                if (width > 0 || height > 0) {
                    // An item with a size sits at its gravity: centred, or
                    // top-left by default.
                    const int gravity = int(numberOf(item, attr::kGravity, "gravity", 0));
                    QRectF sized(inner.topLeft(), QSizeF(width > 0 ? width : inner.width(), height > 0 ? height : inner.height()));
                    if ((gravity & 0x07) == 0x01)
                        sized.moveLeft(inner.center().x() - sized.width() / 2);
                    if ((gravity & 0x70) == 0x10)
                        sized.moveTop(inner.center().y() - sized.height() / 2);
                    inner = sized;
                }
                if (inner.isValid())
                    drawn = drawContent(painter, inner, item, dp, depth + 1) || drawn;
            }
            return drawn;
        }
        if (name == "selector") {
            note(name);
            const XmlElement *item = defaultItem(element);
            return item && drawContent(painter, rect, *item, dp, depth + 1);
        }
        if (name == "shape")
            return drawShape(painter, rect, element, dp, depth);
        note(name + " (not drawn)");
        return false;
    }

    // <adaptive-icon>: background under foreground on a 108 dp square, the
    // middle 72 dp shown, clipped to a rounded square. <monochrome> is not
    // drawn. The icon counts as drawn only when its foreground is: a plain
    // background square says less than the monogram.
    bool drawAdaptive(QPainter &painter, const QRectF &rect, const XmlElement &element, int depth) {
        note("adaptive-icon");
        const double w = rect.width(), h = rect.height();
        const QRectF layer = rect.adjusted(-w / 4, -h / 4, w / 4, h / 4);
        const double dp = layer.width() / 108;
        painter.save();
        QPainterPath mask;
        mask.addRoundedRect(rect, w / 4, h / 4);
        painter.setClipPath(mask, Qt::IntersectClip);
        for (const XmlElement &child : element.children)
            if (child.name == "background")
                drawContent(painter, layer, child, dp, depth + 1);
        bool foreground = false;
        for (const XmlElement &child : element.children)
            if (child.name == "foreground")
                foreground = drawContent(painter, layer, child, dp, depth + 1) || foreground;
        painter.restore();
        return foreground;
    }

    // <shape>: a rectangle (with corner radius) or oval, filled with its
    // <solid> colour or <gradient>, outlined by its <stroke>.
    bool drawShape(QPainter &painter, const QRectF &rect, const XmlElement &element, double dp, int depth) {
        const int kind = int(numberOf(element, attr::kShape, "shape", 0));
        if (kind > 1) {
            note("shape (line or ring, not drawn)");
            return false;
        }
        note("shape");
        const PaintLookup lookup = [&](quint32 id) { return paint(id, depth + 1); };
        QPainterPath outline;
        double radius = 0;
        for (const XmlElement &child : element.children)
            if (child.name == "corners")
                radius = length(child, attr::kRadius, "radius", std::min(rect.width(), rect.height()), dp);
        if (kind == 1)
            outline.addEllipse(rect);
        else
            outline.addRoundedRect(rect, radius, radius);
        bool drawn = false;
        for (const XmlElement &child : element.children) {
            if (child.name == "solid") {
                const Paint solid = paintAttribute(child, attr::kColor, "color", lookup);
                if (solid.kind != Paint::None) {
                    painter.fillPath(outline, QColor::fromRgba(solid.argb));
                    drawn = true;
                }
            } else if (child.name == "gradient") {
                const QBrush brush = shapeGradient(child, rect, dp, lookup);
                if (brush.style() != Qt::NoBrush) {
                    painter.fillPath(outline, brush);
                    drawn = true;
                }
            } else if (child.name == "stroke") {
                const Paint stroke = paintAttribute(child, attr::kColor, "color", lookup);
                const double width = length(child, attr::kWidth, "width", rect.width(), dp);
                if (stroke.kind != Paint::None && width > 0) {
                    painter.strokePath(outline, QPen(QColor::fromRgba(stroke.argb), width));
                    drawn = true;
                }
            }
        }
        return drawn;
    }

    // A shape's <gradient>: linear at a multiple of 45 degrees, radial, or
    // sweep, from start (centre) to end colour, in the shape's bounds.
    static QBrush shapeGradient(const XmlElement &element, const QRectF &rect, double dp, const PaintLookup &lookup) {
        QGradientStops stops;
        const Paint start = paintAttribute(element, attr::kStartColor, "startColor", lookup);
        const Paint center = paintAttribute(element, attr::kCenterColor, "centerColor", lookup);
        const Paint end = paintAttribute(element, attr::kEndColor, "endColor", lookup);
        if (start.kind == Paint::Solid)
            stops.append({0.0, QColor::fromRgba(start.argb)});
        if (center.kind == Paint::Solid)
            stops.append({0.5, QColor::fromRgba(center.argb)});
        if (end.kind == Paint::Solid)
            stops.append({1.0, QColor::fromRgba(end.argb)});
        if (stops.isEmpty())
            return {};
        const int type = int(numberOf(element, attr::kType, "type", 0));
        const QPointF middle(rect.left() + rect.width() * numberOf(element, attr::kCenterX, "centerX", 0.5),
                             rect.top() + rect.height() * numberOf(element, attr::kCenterY, "centerY", 0.5));
        if (type == 1) {
            const double radius = length(element, attr::kGradientRadius, "gradientRadius", std::min(rect.width(), rect.height()), dp);
            QRadialGradient gradient(middle, radius > 0 ? radius : std::min(rect.width(), rect.height()) / 2);
            gradient.setStops(stops);
            return gradient;
        }
        if (type == 2) {
            // Android sweeps clockwise from three o'clock, Qt counter-clockwise.
            QGradientStops reversed;
            for (const auto &stop : stops)
                reversed.prepend({1.0 - stop.first, stop.second});
            QConicalGradient gradient(middle, 0);
            gradient.setStops(reversed);
            return gradient;
        }
        const double angle = numberOf(element, attr::kAngle, "angle", 0) * M_PI / 180;
        const QPointF half(std::cos(angle) * rect.width() / 2, -std::sin(angle) * rect.height() / 2);
        QLinearGradient gradient(rect.center() - half, rect.center() + half);
        gradient.setStops(stops);
        return gradient;
    }

    const Resources &m_resources;
    QStringList *m_met;
    QSet<quint32> m_drawing, m_painting;
};

bool anyInk(const QImage &image) {
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(line[x]))
                return true;
    }
    return false;
}

} // namespace

QByteArray renderIcon(quint32 resourceId, const Resources &resources, int size, QStringList *met) {
    if (size <= 0 || size > 1024)
        return {};
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    bool drawn = false;
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        Renderer renderer(resources, met);
        // A legacy launcher icon is 48 dp square.
        drawn = renderer.drawResource(painter, QRectF(0, 0, size, size), resourceId, size / 48.0, 0);
    }
    if (!drawn || !anyInk(image))
        return {};
    QByteArray out;
    QBuffer buffer(&out);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return out;
}

} // namespace apk
