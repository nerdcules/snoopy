#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// Pure parsing for the launcher-icon reader (icons.cpp): APK names, the zip
// central directory, resources.arsc and binary XML. Nothing here runs a
// process, so tests feed it bytes they build themselves. Ports of the helpers
// in android-diagnoser's icons.py (at 7b2d820); Python names are noted.
namespace apk {

// (density, path) as the resource table gives them. Density 0 is the default
// configuration; 65534 and 65535 are anydpi and nodpi.
using Found = QList<QPair<int, QString>>;

// _is_launcher: the file is called ic_launcher*, png or webp, not nine-patch.
bool isLauncher(const QString &name);
// _rank: density first (xxxhdpi 40 ... hdpi 10), +8 for the plain name,
// +2 for round, -4 for foreground.
int rank(const QString &name);
// choose_launcher: the highest-ranked launcher name, the earliest on a tie.
// Empty when there is none.
QString chooseLauncher(const QStringList &names);
// best_image: the highest-density png or webp below 65000 (so not anydpi or
// nodpi). Empty when there is none.
QString bestImage(const Found &found);
// _mime: image/png, image/webp, or image/svg+xml for anything else.
QString mime(const QByteArray &data);

// End of central directory, searched backwards in the tail of a file.
struct Eocd {
    bool found = false;
    quint32 directorySize = 0;
    quint32 directoryOffset = 0;
};
Eocd findEocd(const QByteArray &tail);

struct ZipEntry {
    QString name;
    quint16 method = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 localOffset = 0;
};
// The central directory's entries, in file order, until the first record
// that isn't a PK\1\2 header or doesn't fit.
QList<ZipEntry> centralDirectory(const QByteArray &directory);
// Where an entry's data starts, relative to its local header, read from the
// first 30 bytes of that header (30 + name length + extra length); -1 when
// fewer than 30 bytes are given. checkSignature also requires PK\3\4.
int localDataOffset(const QByteArray &localHeader, bool checkSignature);
// zlib.decompress(data, -15): raw deflate. ok is false on a broken or
// truncated stream.
QByteArray inflateRaw(const QByteArray &data, bool *ok);

// _pool: the strings of the ResStringPool chunk at offset (UTF-8 or UTF-16).
QStringList stringPool(const QByteArray &data, int offset);
// resolve_in_table: every configuration's value for resource id target in a
// resources.arsc. String values come back as paths; references as "@xxxxxxxx".
Found resolveInTable(const QByteArray &table, quint32 target);
// _manifest_icon_id: the android:icon reference on <application> in a binary
// AndroidManifest.xml; 0 when there is none.
quint32 manifestIconId(const QByteArray &manifest);
// _xml_refs: drawable, icon and src references in a binary XML file (an
// adaptive icon's layers), in order, without repeats.
QList<quint32> xmlRefs(const QByteArray &blob);

// _WALKER: the shell script that walks a large resources.arsc in place on the
// phone with toybox dd and od. Arguments: apk, data start, type id, entry.
// Prints "density path" lines.
QString deviceWalker();
// The walker's output as (density, path) pairs; other lines are skipped.
Found parseWalkerOutput(const QString &text);

} // namespace apk
