#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QLocalServer;

// Snoopy on the Omarchy desktop: the status file the bar widget watches,
// notifications for new Watch findings, one window that later launches
// talk to, and installing the bar widget.
namespace desktop {

// ~/.local/state/snoopy (the bar widget reads status.json there).
QString stateDirectory();

// What the bar widget shows, from the last sample:
//   updatedAt (ISO, UTC), connected, transport ("usb"/"wifi"), model,
//   android, alerts, warnings, newCount, newAlerts, message, source.
QJsonObject status(bool connected, const QString &transport, const QJsonObject &device, const QJsonArray &findings,
                   const QString &message, const QString &source);
// Written whole, then renamed into place.
void writeStatus(const QJsonObject &status, const QString &directory = {});

// The new alerts and warnings not notified yet for phone, remembered in
// <directory>/notified.json so the window and the daemon each tell once.
QJsonArray takeUnnotified(const QString &phone, const QJsonArray &findings, const QString &directory = {});

// One notification per finding, or one summary for more than three.
// Clicking runs `snoopy open --page watch`. Sent with send (tests) or over
// D-Bus to org.freedesktop.Notifications with Omarchy's hints.
struct Notification {
    QString summary;
    QString body;
    int urgency = 1;
    QStringList clickArgv;
};
QList<Notification> notificationsFor(const QJsonArray &findings);
void send(const Notification &notification);

// After a sample: status file, then notifications for what is new.
void publish(const QString &phone, bool connected, const QString &transport, const QJsonObject &device,
             const QJsonArray &findings, const QString &message, const QString &source);

// One window. The first launch listens; a later `snoopy open --page P`
// hands the page to it and exits.
class Remote : public QObject {
    Q_OBJECT
public:
    explicit Remote(QObject *parent = nullptr);
    // Starts listening. False if another window already listens.
    bool listen();
    // Sends page to a running window; false if none answered.
    static bool sendToRunning(const QString &page);
    static QString serverName();

signals:
    void showPage(const QString &page);

private:
    QLocalServer *m_server = nullptr;
};

// Copies the bundled bar widget into ~/.config/omarchy/plugins and turns it
// on (left section of the bar). Messages go to report.
bool installPlugin(const std::function<void(const QString &)> &report);
// Enables and starts the snoopy.service user unit.
bool enableDaemon(const std::function<void(const QString &)> &report);

} // namespace desktop
