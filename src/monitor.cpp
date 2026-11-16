#include "monitor.h"

#include "adb.h"
#include "flows.h"
#include "graph.h"
#include "names.h"
#include "report.h"
#include "watch.h"
#include "wireless.h"

#include <QSet>
#include <QSettings>
#include <QtConcurrent>

namespace {

Sample adbSample(const QString &serial, const QString &link) {
    Sample sample;
    try {
        Adb adb(serial);
        const Adb::Link over = link == QLatin1String("wifi") ? Adb::Link::Wifi : Adb::Link::Cable;
        // Over Wi-Fi with no phone ready: a phone paired for wireless
        // debugging may be announcing itself on the network; connect to it
        // and look again.
        try {
            adb.resolveSerial(over);
        } catch (const AdbError &) {
            if (over != Adb::Link::Wifi || connectAnnounced(adb) == 0)
                throw;
            adb.resolveSerial(over);
        }
        sample.capture = collectSnapshot(adb);
        sample.serial = adb.serial();
        sample.report = buildReport(sample.capture);
        sample.findings = watchFindings(sample.capture, QDate::currentDate());
        sample.phone = parseSecurity(sample.capture.text("security")).value("hardware_serial");
        if (sample.phone.isEmpty())
            sample.phone = phoneOf(sample.serial);
    } catch (const AdbError &error) {
        sample.error = error.message();
    } catch (const std::exception &error) {
        sample.error = QString::fromUtf8(error.what());
    }
    return sample;
}

// The same identity android-diagnoser's relay gave a flow, so a socket that
// stays open from one sample to the next is not reported again.
QString flowKey(const QJsonObject &flow) {
    QStringList parts;
    for (const char *field : {"kind", "source", "destination", "information", "via"})
        parts.append(flow.value(field).toVariant().toString());
    return parts.join(QChar(0x1f));
}

QStringList strings(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list.append(item.toString());
    return list;
}

} // namespace

Monitor::Monitor(const QString &serial, Sampler sampler, Names *names, QObject *parent)
    : QObject(parent), m_sampler(sampler ? sampler : Sampler(adbSample)), m_names(names),
      m_requestedSerial(serial) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &Monitor::sampleNow);
    connect(&m_watcher, &QFutureWatcher<Sample>::finished, this, &Monitor::finished);
    // A burst of names arriving together redraws the graph once.
    m_namesDebounce.setSingleShot(true);
    m_namesDebounce.setInterval(300);
    connect(&m_namesDebounce, &QTimer::timeout, this, &Monitor::rebuildGraph);
    if (m_names)
        connect(m_names, &Names::resolved, &m_namesDebounce, qOverload<>(&QTimer::start));
    // The app (default sampler) remembers the choice; tests don't touch
    // the user's settings.
    m_persistLink = !sampler;
    if (m_persistLink)
        m_link = QSettings("snoopy", "snoopy").value("link", "usb").toString() == "wifi" ? "wifi" : "usb";
}

void Monitor::setLink(const QString &link) {
    const QString value = link == QLatin1String("wifi") ? QStringLiteral("wifi") : QStringLiteral("usb");
    if (value == m_link)
        return;
    m_link = value;
    if (m_persistLink)
        QSettings("snoopy", "snoopy").setValue("link", m_link);
    emit linkChanged();
    sampleNow();
}

Monitor::~Monitor() {
    m_timer.stop();
    m_watcher.waitForFinished();
}

QString Monitor::serial() const {
    QMutexLocker lock(&m_serialMutex);
    return m_serial;
}

void Monitor::start() {
    sampleNow();
}

void Monitor::sampleNow() {
    if (m_watcher.isRunning())
        return;
    m_timer.stop();
    const QString serial = m_requestedSerial, link = m_link;
    const Sampler sampler = m_sampler;
    m_watcher.setFuture(QtConcurrent::run([sampler, serial, link] { return sampler(serial, link); }));
    emit busyChanged();
}

QString Monitor::transport() const {
    const QString current = serial();
    return current.isEmpty() ? QString() : isWireless(current) ? QStringLiteral("wifi") : QStringLiteral("usb");
}

void Monitor::finished() {
    const Sample sample = m_watcher.result();
    if (sample.error.isEmpty()) {
        {
            QMutexLocker lock(&m_serialMutex);
            m_serial = sample.serial;
        }
        relay(sample.report);
        applyFindings(sample.phone, sample.findings);
    } else {
        setWaiting(sample.error);
    }
    emit busyChanged();
    emit sampled();
    if (!m_paused)
        m_timer.start(m_interval * 1000);
}

void Monitor::relay(const QJsonObject &report) {
    const QJsonObject projected = projectReport(report);
    const QJsonArray flows = projected.value("flows").toArray();
    const QDateTime now = QDateTime::currentDateTime();

    if (m_samples > 0) {
        QSet<QString> previous, current;
        for (const QJsonValue &flow : m_flows)
            previous.insert(flowKey(flow.toObject()));
        for (const QJsonValue &flow : flows)
            current.insert(flowKey(flow.toObject()));
        QVariantList added;
        for (const QJsonValue &flow : flows) {
            if (previous.contains(flowKey(flow.toObject())))
                continue;
            QVariantMap event = flow.toObject().toVariantMap();
            event.insert("at", now);
            event.insert("change", "appeared");
            added.append(event);
        }
        for (const QJsonValue &flow : m_flows) {
            if (current.contains(flowKey(flow.toObject())))
                continue;
            QVariantMap event = flow.toObject().toVariantMap();
            event.insert("at", now);
            event.insert("change", "stopped");
            added.append(event);
        }
        if (!added.isEmpty()) {
            // Newest first, as the window lists them; within one sample the
            // order stays appeared, then stopped.
            for (auto it = added.crbegin(); it != added.crend(); ++it)
                m_events.prepend(*it);
            while (m_events.size() > kMaxEvents)
                m_events.removeLast();
            emit eventsChanged();
        }
    }

    m_flows = flows;
    m_apps = projected.value("apps").toArray();
    m_device = report.value("device").toObject().toVariantMap();
    m_limits = strings(report.value("limits"));
    m_errors.clear();
    const QJsonObject errors = report.value("errors").toObject();
    for (auto it = errors.begin(); it != errors.end(); ++it)
        m_errors.append(QString("%1: %2").arg(it.key(), it.value().toString()));
    m_updatedAt = now;
    ++m_samples;
    m_status = "live";
    m_message.clear();
    rebuildGraph();
    emit changed();
}

void Monitor::setWaiting(const QString &message) {
    m_status = "waiting";
    m_message = message;
    emit changed();
}

void Monitor::rebuildGraph() {
    const QJsonArray graph = graphFrom(m_flows, m_apps, [this](const QString &host) {
        return m_names ? m_names->cached(host) : QString();
    });
    m_graph = graph.toVariantList();
    if (m_names) {
        QStringList hosts;
        for (const QJsonValue &card : graph)
            for (const QJsonValue &destination : card.toObject().value("destinations").toArray())
                hosts.append(destination.toObject().value("host").toString());
        m_names->warm(hosts);
    }
    emit graphChanged();
}

void Monitor::applyFindings(const QString &phone, const QJsonArray &findings) {
    m_phone = phone;
    const QJsonArray marked = phone.isEmpty() ? findings
                                              : WatchBaseline(m_baselineDirectory).apply(phone, findings,
                                                                                         QDateTime::currentDateTime());
    m_findings = marked.toVariantList();
    m_flagged.clear();
    for (const QJsonValue &value : marked) {
        const QJsonObject item = value.toObject();
        if (item.value("group").toString() == "background" && item.value("level").toString() == "alert")
            m_flagged.append(item.value("package").toString());
    }
    m_flagged.removeDuplicates();
    emit findingsChanged();
}

int Monitor::newFindings() const {
    int count = 0;
    for (const QVariant &item : m_findings)
        count += item.toMap().value("new").toBool() ? 1 : 0;
    return count;
}

void Monitor::acknowledgeFindings() {
    if (m_phone.isEmpty())
        return;
    WatchBaseline(m_baselineDirectory).acknowledge(m_phone);
    for (QVariant &item : m_findings) {
        QVariantMap map = item.toMap();
        map.remove("new");
        map.remove("since");
        item = map;
    }
    emit findingsChanged();
}

void Monitor::setBaselineDirectory(const QString &directory) {
    m_baselineDirectory = directory;
}

void Monitor::clearEvents() {
    if (m_events.isEmpty())
        return;
    m_events.clear();
    emit eventsChanged();
}

void Monitor::setPaused(bool paused) {
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (m_paused)
        m_timer.stop();
    else if (!m_watcher.isRunning())
        sampleNow();
    emit pausedChanged();
}

void Monitor::setInterval(int seconds) {
    seconds = qBound(5, seconds, 300);
    if (seconds == m_interval)
        return;
    m_interval = seconds;
    if (m_timer.isActive())
        m_timer.start(m_interval * 1000);
    emit intervalChanged();
}
