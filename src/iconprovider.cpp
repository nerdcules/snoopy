#include "iconprovider.h"

#include "icons.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QRunnable>
#include <QUrl>

namespace {

// Decoded at the size asked for, so a monogram's SVG stays crisp and a large
// launcher image isn't held at full size.
QImage decode(const QByteArray &bytes, const QSize &size) {
    QBuffer buffer;
    buffer.setData(bytes);
    QImageReader reader(&buffer);
    const QSize natural = reader.size();
    if (natural.isValid())
        reader.setScaledSize(natural.scaled(size, Qt::KeepAspectRatio));
    return reader.read();
}

class Response : public QQuickImageResponse, public QRunnable {
public:
    Response(IconSource *source, QString package, QString serial, QSize size)
        : m_source(source), m_package(std::move(package)), m_serial(std::move(serial)), m_size(size) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void run() override {
        const QSize size = m_size.isValid() && !m_size.isEmpty() ? m_size : QSize(96, 96);
        QString mime;
        QImage image = decode(m_source->icon(m_package, m_serial, &mime), size);
        // An icon that won't decode falls back to the monogram.
        if (image.isNull())
            image = decode(IconSource::monogram(m_package), size);
        m_image = image;
        emit finished();
    }

private:
    IconSource *m_source;
    QString m_package;
    QString m_serial;
    QSize m_size;
    QImage m_image;
};

} // namespace

IconProvider::IconProvider(IconSource *source, std::function<QString()> serial)
    : m_source(source), m_serial(std::move(serial)) {
    // Icons queue behind each other on the phone anyway (IconSource
    // serializes device access); two threads keep a cached icon from
    // waiting behind a slow first read.
    m_pool.setMaxThreadCount(2);
}

QQuickImageResponse *IconProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    auto *response = new Response(m_source, QUrl::fromPercentEncoding(id.toUtf8()), m_serial ? m_serial() : QString(),
                                  requestedSize);
    m_pool.start(response);
    return response;
}
