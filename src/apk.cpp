#include "apk.h"

#include <QRegularExpression>

#include <algorithm>
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

// _type_hit: the value of entry index in the ResTable_type chunk at pos.
Found typeHit(const QByteArray &table, qint64 pos, quint16 headerSize, quint32 index, const QStringList &strings) {
    if (pos + 20 > table.size())
        return {};
    const quint8 flags = u8(table, pos + 9);
    const quint32 entryCount = u32(table, pos + 12);
    const quint32 entriesStart = u32(table, pos + 16);
    // A sparse chunk's count is the number of (index, offset) pairs it
    // holds, not the highest index: YouTube's launcher icon is entry 15 of a
    // 5-pair chunk (Android 17). icons.py stopped here for every sparse
    // chunk and found no icon.
    if (!(flags & 0x01) && index >= entryCount)
        return {};
    const int density = fits(table, pos + 34, 2) ? u16(table, pos + 34) : 0;
    const qint64 offset = entryOffset(table, pos, headerSize, flags, index, entryCount);
    if (offset < 0)
        return {};
    const qint64 entry = pos + entriesStart + offset;
    if (!fits(table, entry, 8))
        return {};
    const quint16 entrySize = u16(table, entry);
    const quint16 entryFlags = u16(table, entry + 2);
    if ((entryFlags & 0x0001) || entry + entrySize + 8 > table.size())
        return {};
    const qint64 value = entry + entrySize;
    const quint8 type = u8(table, value + 3);
    const quint32 data = u32(table, value + 4);
    if (type == 0x03 && data < quint32(strings.size()))
        return {{density, strings.at(int(data))}};
    if (type == 0x01)
        return {{density, QString("@%1").arg(data, 8, 16, QLatin1Char('0'))}};
    return {};
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

QStringList stringPool(const QByteArray &data, int offset) {
    if (!fits(data, offset, 28))
        return {};
    const quint16 headerSize = u16(data, offset + 2);
    const quint32 count = u32(data, offset + 8);
    const quint32 flags = u32(data, offset + 16);
    const quint32 stringsStart = u32(data, offset + 20);
    const bool utf8 = flags & 0x100;
    if (!fits(data, qint64(offset) + headerSize, qint64(count) * 4))
        return {};
    const qint64 base = qint64(offset) + stringsStart;
    QStringList values;
    values.reserve(int(count));
    for (quint32 item = 0; item < count; ++item) {
        qint64 pos = base + u32(data, qint64(offset) + headerSize + qint64(item) * 4);
        if (pos >= data.size()) {
            values.append(QString());
            continue;
        }
        if (utf8) {
            ulen(data, pos); // the UTF-16 length, unused
            const quint32 length = ulen(data, pos);
            values.append(QString::fromUtf8(data.mid(pos, length)));
        } else {
            if (!fits(data, pos, 2)) {
                values.append(QString());
                continue;
            }
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
            values.append(text);
        }
    }
    return values;
}

Found resolveInTable(const QByteArray &table, quint32 target) {
    if (table.size() < 12 || !table.startsWith(QByteArray("\x02\x00", 2)))
        return {};
    const quint16 header = u16(table, 2);
    const qint64 end = std::min<qint64>(table.size(), u32(table, 4));
    QStringList strings;
    Found found;
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
            if (type == 0x0001 && strings.isEmpty())
                strings = stringPool(table, int(pos));
            else if (type == 0x0200)
                walk(pos + headerSize, pos + size, depth + 1);
            else if (type == 0x0201 && pos + 8 < stop && u8(table, pos + 8) == typeId)
                found += typeHit(table, pos, headerSize, index, strings);
            pos += size;
        }
    };
    walk(header, end, 0);
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

} // namespace apk
