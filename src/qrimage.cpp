#include "qrimage.h"

#include <QUrl>
#include <qrencode.h>

QImage qrImage(const QString &text, int scale) {
    const QByteArray bytes = text.toUtf8();
    QRcode *code = QRcode_encodeString8bit(bytes.constData(), 0, QR_ECLEVEL_M);
    if (!code)
        return {};
    constexpr int quiet = 4;
    const int side = (code->width + 2 * quiet) * scale;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < code->width; ++y) {
        for (int x = 0; x < code->width; ++x) {
            if (!(code->data[y * code->width + x] & 1))
                continue;
            for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx)
                    image.setPixel((x + quiet) * scale + dx, (y + quiet) * scale + dy, qRgb(0, 0, 0));
        }
    }
    QRcode_free(code);
    return image;
}

QImage QrProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    // Whole modules only, so the code stays sharp: the largest scale that
    // fits the size asked for.
    const QString text = QUrl::fromPercentEncoding(id.toUtf8());
    const QImage unit = qrImage(text, 1);
    int scale = 8;
    if (!unit.isNull() && requestedSize.isValid() && requestedSize.width() > 0)
        scale = qMax(1, requestedSize.width() / unit.width());
    const QImage image = qrImage(text, scale);
    if (size)
        *size = image.size();
    return image;
}
