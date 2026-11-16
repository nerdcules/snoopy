#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QString>

// A QR code for text, dark on light with the four-module quiet zone
// scanners need, each module scale pixels. Null if it can't be encoded.
QImage qrImage(const QString &text, int scale);

// image://qr/<percent-encoded text> in QML.
class QrProvider : public QQuickImageProvider {
public:
    QrProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
