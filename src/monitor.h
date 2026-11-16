#pragma once

#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "capture.h"

class Names;

// What one sample gave: the report, or the problem that stopped it.
struct Sample {
    QJsonObject report;
    Capture capture;
    QString serial;
    QString error;
};

// Samples the phone on a worker thread every interval seconds, keeps the
// latest flows and graph, and records which flows appeared or stopped since
// the sample before. The first sample is the baseline and records nothing.
class Monitor : public QObject {
    Q_OBJECT
    // "starting" until the first sample, "live" after one succeeds, and
    // "waiting" when the phone can't be read (message says why).
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantMap device READ device NOTIFY changed)
    Q_PROPERTY(QVariantList graph READ graph NOTIFY graphChanged)
    Q_PROPERTY(QVariantList events READ events NOTIFY eventsChanged)
    Q_PROPERTY(QStringList limits READ limits NOTIFY changed)
    Q_PROPERTY(QStringList errors READ errors NOTIFY changed)
    Q_PROPERTY(QDateTime updatedAt READ updatedAt NOTIFY changed)
    Q_PROPERTY(int samples READ samples NOTIFY changed)
    Q_PROPERTY(int flowCount READ flowCount NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(int interval READ interval WRITE setInterval NOTIFY intervalChanged)
    // "usb" or "wifi" for the phone the last sample read, "" before one.
    Q_PROPERTY(QString transport READ transport NOTIFY changed)

public:
    using Sampler = std::function<Sample(const QString &serial)>;

    // serial picks a phone when several are connected. The default sampler
    // collects a snapshot over adb and builds its report.
    explicit Monitor(const QString &serial = {}, Sampler sampler = {}, Names *names = nullptr, QObject *parent = nullptr);
    ~Monitor() override;

    QString status() const { return m_status; }
    QString message() const { return m_message; }
    QVariantMap device() const { return m_device; }
    QVariantList graph() const { return m_graph; }
    QVariantList events() const { return m_events; }
    QStringList limits() const { return m_limits; }
    QStringList errors() const { return m_errors; }
    QDateTime updatedAt() const { return m_updatedAt; }
    int samples() const { return m_samples; }
    int flowCount() const { return int(m_flows.size()); }
    bool busy() const { return m_watcher.isRunning(); }
    bool paused() const { return m_paused; }
    int interval() const { return m_interval; }
    // The serial of the phone the last sample read, for the icon reader.
    QString serial() const;
    QString transport() const;

    void setPaused(bool paused);
    void setInterval(int seconds);

    // Starts sampling: one now, then every interval.
    Q_INVOKABLE void start();
    // A sample now, unless one is already running.
    Q_INVOKABLE void sampleNow();
    Q_INVOKABLE void clearEvents();

    // Applies one report as a sample would. Public for tests.
    void relay(const QJsonObject &report);
    // The phone couldn't be read; keeps the last flows on screen.
    void setWaiting(const QString &message);

    static constexpr int kMaxEvents = 200;

signals:
    void changed();
    void graphChanged();
    void eventsChanged();
    void busyChanged();
    void pausedChanged();
    void intervalChanged();
    // After every sample, good or not.
    void sampled();

private:
    void finished();
    void rebuildGraph();

    Sampler m_sampler;
    Names *m_names = nullptr;
    QFutureWatcher<Sample> m_watcher;
    QTimer m_timer;
    QTimer m_namesDebounce;

    QString m_requestedSerial;
    mutable QMutex m_serialMutex;
    QString m_serial;

    QString m_status = "starting";
    QString m_message;
    QVariantMap m_device;
    QJsonArray m_flows;
    QJsonArray m_apps;
    QVariantList m_graph;
    QVariantList m_events;
    QStringList m_limits;
    QStringList m_errors;
    QDateTime m_updatedAt;
    int m_samples = 0;
    bool m_paused = false;
    int m_interval = 20;
};
