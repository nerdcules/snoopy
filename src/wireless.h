#pragma once

#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include "adb.h"

// Wireless debugging (Android 11 and later), paired the way Android Studio
// does it: Snoopy shows a QR code naming a service and a password; the
// phone (Developer options > Wireless debugging > Pair device with QR code)
// scans it and announces _adb-tls-pairing._tcp under that name; adb finds it
// over mDNS, pairs, and the phone then announces _adb-tls-connect._tcp,
// which adb connects to. Everything after that is ordinary adb.

// One line of `adb mdns services`: instance, service type, host:port.
struct MdnsService {
    QString instance;
    QString type;
    QString address;
};
QList<MdnsService> parseMdnsServices(const QString &text);

// The text the phone's QR scanner expects.
QString pairingQrText(const QString &name, const QString &password);

// The phone behind a serial: a wireless serial from mDNS,
// "adb-<phone>-<suffix>._adb-tls-connect._tcp", gives <phone>; a USB
// serial is the phone. A host:port serial can't be traced and is itself.
QString phoneOf(const QString &serial);
bool isWireless(const QString &serial);

// Connects to every phone announcing _adb-tls-connect._tcp that adb isn't
// connected to yet. Only phones already paired with this computer accept.
// Returns how many connected.
int connectAnnounced(Adb &adb);

class Pairing : public QObject {
    Q_OBJECT
    // "idle", "waiting" (for the phone to scan), "pairing", "connecting",
    // "connected", "failed".
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString qrText READ qrText NOTIFY changed)
    Q_PROPERTY(QString serial READ serial NOTIFY changed)

public:
    explicit Pairing(Runner runner = runProcess, const QString &adbPath = findAdb(), QObject *parent = nullptr);
    ~Pairing() override;

    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString qrText() const { return m_qrText; }
    QString serial() const { return m_serial; }

    // A new name and password, then waits for the phone. Gives up after
    // three minutes.
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();

    // How often adb is asked while waiting, in ms. Tests shorten it.
    void setPollInterval(int ms) { m_poll.setInterval(ms); }

signals:
    void changed();
    // The phone is connected over Wi-Fi as serial.
    void connected(const QString &serial);

private:
    struct Step {
        QString state;
        QString message;
        QString serial;
        QString guid;
        int generation = 0;
        bool connectByHand = false;
    };
    void poll();
    void stepDone();
    void set(const QString &state, const QString &message);
    Step step(const QString &state, const QString &name, const QString &password, const QString &guid,
              bool connectByHand) const;

    Runner m_runner;
    QString m_adbPath;
    QTimer m_poll;
    QTimer m_deadline;
    QFutureWatcher<Step> m_watcher;
    QString m_state = "idle";
    QString m_message;
    QString m_qrText;
    QString m_serial;
    QString m_name;
    QString m_password;
    QString m_guid;
    int m_generation = 0;
    int m_connectLooks = 0;
};
