#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// Pure parsing for the launcher-icon reader (icons.cpp): APK names, the zip
// central directory, resources.arsc and binary XML, and drawing an XML
// (vector, adaptive) icon to a PNG. Nothing here runs a process, so tests
// feed it bytes they build themselves. Most helpers are ports of
// android-diagnoser's icons.py (at 7b2d820); Python names are noted.
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

// ---- typed values, binary XML and the resource table ----

// Res_value data types.
enum ValueType : quint8 {
    TypeNull = 0x00,
    TypeReference = 0x01,
    TypeAttribute = 0x02, // ?attr: a theme attribute, not resolvable without the theme
    TypeString = 0x03,
    TypeFloat = 0x04,
    TypeDimension = 0x05,
    TypeFraction = 0x06,
    TypeIntDec = 0x10,
    TypeIntHex = 0x11,
    TypeBool = 0x12,
    TypeArgb8 = 0x1C,
    TypeRgb8 = 0x1D,
    TypeArgb4 = 0x1E,
    TypeRgb4 = 0x1F,
};

// One typed value. string is set for TypeString.
struct Value {
    quint8 type = TypeNull;
    quint32 data = 0;
    QString string;

    bool isColor() const { return type >= TypeArgb8 && type <= TypeRgb4; }
    bool isNumber() const;
    // The colour as 0xAARRGGBB (the RGB types are opaque). Only for isColor().
    quint32 argb() const;
    // A float, an integer, a dimension's or fraction's number (dp for dip,
    // px for px, and so on: the unit is dropped) or a bool as 0 or 1.
    double number() const;
};

struct XmlAttribute {
    QString name;          // may be empty or obfuscated in a shrunk APK
    quint32 resourceId = 0; // from the resource map chunk, 0 when not mapped
    Value value;
};

struct XmlElement {
    QString name;
    QList<XmlAttribute> attributes;
    QList<XmlElement> children;

    // The attribute with this android resource id; without a resource map,
    // the one whose name is name. Null when there is none.
    const XmlAttribute *attribute(quint32 resourceId, const QString &name) const;
};

// A binary XML file (AXML) as a tree: elements, their attributes with the
// resource ids the resource map gives them, and typed values. Nullopt when
// the file is not binary XML, is cut short, or nests too deep.
std::optional<XmlElement> parseXml(const QByteArray &blob);

// One configuration's value of a resource in a resources.arsc. plain is
// true when the configuration asks for nothing but a density or a platform
// version (no night mode, locale, screen size...).
struct TableValue {
    int density = 0;
    int sdk = 0;
    bool plain = true;
    Value value;
};
// Every configuration's value for resource id target, of any type (colours,
// references, file paths); map entries (styles) are skipped. Strings are
// decoded one at a time, not the whole pool.
QList<TableValue> tableValues(const QByteArray &table, quint32 target);

// ---- vector drawables ----

// How a vector path is filled or stroked: one colour, or a gradient in the
// path's coordinates (a <gradient> file, from aapt's inline <aapt:attr>).
struct Paint {
    enum Kind { None, Solid, Linear, Radial, Sweep } kind = None;
    quint32 argb = 0; // Solid; for a sweep, its first colour
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0; // Linear
    double cx = 0, cy = 0, r = 0;         // Radial (and a sweep's centre)
    int tile = 0;                         // 0 clamp, 1 repeat, 2 mirror
    QList<QPair<double, quint32>> stops;
};

// Looks a colour reference up (resolving it through the table and reading
// a <selector> or <gradient> file it names). Kind None when it cannot.
using PaintLookup = std::function<Paint(quint32 resourceId)>;

// A colour value as a Paint: a literal colour, a reference through lookup,
// else None.
Paint paintOf(const Value &value, const PaintLookup &lookup);
// A <selector> (colour state list: its stateless item) or a <gradient>
// file as a Paint.
Paint paintOfXml(const XmlElement &root, const PaintLookup &lookup);
// A <vector> drawable as SVG 1.1: the viewport becomes the viewBox, groups
// keep their transforms, paths their fill, stroke, alpha and fill rule;
// gradients become linear or radial gradients (a sweep its first colour).
// Empty when root is not a vector or has no viewport.
QByteArray vectorToSvg(const XmlElement &root, const PaintLookup &lookup);

// ---- drawing a drawable ----

// What renderIcon needs from the APKs: a resource's values in every
// configuration, and a file's bytes (empty when it isn't there). prefetch,
// when set, is told the files an XML file refers to before they are asked
// for one by one, so they can be read together.
struct Resources {
    std::function<QList<TableValue>(quint32)> values;
    std::function<QByteArray(const QString &path)> file;
    std::function<void(const QStringList &paths)> prefetch;
};

// The drawable resource id drawn to a size x size PNG: an adaptive icon
// (background under foreground, the middle 72 of 108 dp, clipped to a
// rounded square), a vector, a bitmap, or a <layer-list>, <inset>,
// <bitmap>, <shape>, <color> or <selector> made of these. Empty when
// nothing could be drawn. met, when given, gets the element names drawn.
QByteArray renderIcon(quint32 resourceId, const Resources &resources, int size = 192, QStringList *met = nullptr);

// _WALKER: the shell script that walks a large resources.arsc in place on the
// phone with toybox dd and od. Arguments: apk, data start, type id, entry.
// Prints "density path" lines.
QString deviceWalker();
// The walker's output as (density, path) pairs; other lines are skipped.
Found parseWalkerOutput(const QString &text);

} // namespace apk
