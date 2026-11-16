// Placeholder until the APK icon reader lands: every app gets its monogram.
#include "icons.h"

#include <QCryptographicHash>
#include <QDir>

IconSource::IconSource(const QString &cacheDirectory, Runner runner, const QString &adbPath)
    : m_cacheDirectory(cacheDirectory.isEmpty() ? QDir::homePath() + "/.cache/snoopy/icons" : cacheDirectory),
      m_runner(std::move(runner)), m_adbPath(adbPath) {}

QByteArray IconSource::icon(const QString &package, const QString &, QString *mime) {
    if (mime)
        *mime = "image/svg+xml";
    return monogram(package);
}

QByteArray IconSource::monogram(const QString &package) {
    const QString tail = package.section('.', -1);
    const QString letter = tail.isEmpty() ? QStringLiteral("?") : tail.left(1).toUpper();
    const int hue = QCryptographicHash::hash(package.toUtf8(), QCryptographicHash::Sha256).at(0) & 0xff;
    return QString("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'><rect width='64' height='64' rx='14' "
                   "fill='hsl(%1,45%,38%)'/><text x='32' y='41' font-family='sans-serif' font-size='28' "
                   "font-weight='600' text-anchor='middle' fill='#fff'>%2</text></svg>")
        .arg(hue % 360)
        .arg(letter.toHtmlEscaped())
        .toUtf8();
}
