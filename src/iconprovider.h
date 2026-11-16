#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>
#include <functional>

class IconSource;

// image://appicon/<package> in QML: the app's launcher icon from the phone,
// or its monogram. Reading an icon takes several adb calls, so each request
// runs on its own small pool and the graph draws monograms meanwhile.
class IconProvider : public QQuickAsyncImageProvider {
public:
    // serial gives the phone to read from at request time.
    IconProvider(IconSource *source, std::function<QString()> serial);

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
    IconSource *m_source;
    std::function<QString()> m_serial;
    QThreadPool m_pool;
};
