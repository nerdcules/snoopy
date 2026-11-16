#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>

#include "adb.h"

// Launcher icons, read from the APK on the connected phone, as
// android-diagnoser's icons.py (at 7b2d820):
//   1. a file named ic_launcher in the APK, png or webp, highest density;
//   2. otherwise the manifest's icon resource, resolved in resources.arsc to
//      a single png or webp (the table is indexed in place on the phone, not
//      copied off when it is large);
//   3. otherwise that resource drawn from its XML (an adaptive icon, a
//      vector, layers of these) to a 192 x 192 PNG (apk::renderIcon);
//   4. otherwise a bitmap layer of the XML icon, as icons.py;
//   5. otherwise a letter monogram (SVG).
// Found icons and misses are cached on disk, so each package is read once.
//
// Blocking and slow (several adb calls per package); the window calls it from
// a worker thread. One instance may be shared: device access is serialized.
class IconSource {
public:
    // cacheDirectory defaults to ~/.cache/snoopy/icons. Tests pass a temp
    // directory and a fake runner.
    explicit IconSource(const QString &cacheDirectory = {}, Runner runner = runProcess, const QString &adbPath = findAdb());

    // Image bytes for package and their mime type (image/png, image/webp or,
    // for a monogram, image/svg+xml). A drawn XML icon is image/png. Never empty. A package name that isn't
    // a.b.c-shaped, or a phone that isn't ready, gets a monogram (and a phone
    // that isn't ready is not cached as a miss, so it is tried again).
    QByteArray icon(const QString &package, const QString &serial, QString *mime);

    // The monogram SVG: the package's last part's first letter on a hue
    // derived from its SHA-256.
    static QByteArray monogram(const QString &package);

private:
    QString m_cacheDirectory;
    Runner m_runner;
    QString m_adbPath;
    QMutex m_mutex;
};
