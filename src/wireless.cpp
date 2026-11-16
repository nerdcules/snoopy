#include "wireless.h"

#include <QRandomGenerator>
#include <QRegularExpression>
#include <QtConcurrent>

namespace {

const QString kPairingType = QStringLiteral("_adb-tls-pairing._tcp");
const QString kConnectType = QStringLiteral("_adb-tls-connect._tcp");
constexpr int kGiveUpMs = 180000;

QString randomText(int length) {
    static const QString alphabet = QStringLiteral("abcdefghijkmnpqrstuvwxyz23456789");
    QString text;
    for (int i = 0; i < length; ++i)
        text.append(alphabet.at(int(QRandomGenerator::system()->bounded(quint32(alphabet.size())))));
    return text;
}

QList<MdnsService> services(Adb &adb) {
    const ProcessResult result = adb.run({"mdns", "services"}, 10000);
    return parseMdnsServices(QString::fromUtf8(result.out));
}

} // namespace

QList<MdnsService> parseMdnsServices(const QString &text) {
    QList<MdnsService> found;
    for (const QString &line : text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() == 3 && parts.at(1).startsWith("_adb") && parts.at(2).contains(':'))
            found.append({parts.at(0), parts.at(1), parts.at(2)});
    }
    return found;
}

QString pairingQrText(const QString &name, const QString &password) {
    return QStringLiteral("WIFI:T:ADB;S:%1;P:%2;;").arg(name, password);
}

QString phoneOf(const QString &serial) {
    static const QRegularExpression mdns(QStringLiteral("^adb-(.+)-[^-.]+\\._adb-tls-connect\\._tcp\\.?$"));
    const QRegularExpressionMatch match = mdns.match(serial);
    return match.hasMatch() ? match.captured(1) : serial;
}

bool isWireless(const QString &serial) {
    static const QRegularExpression hostPort(QStringLiteral("^[0-9a-fA-F.:\\[\\]]+:\\d+$"));
    return serial.contains(kConnectType) || hostPort.match(serial).hasMatch();
}

int connectAnnounced(Adb &adb) {
    QStringList present;
    for (const auto &[serial, state] : adb.devices())
        present.append(serial);
    int connected = 0;
    for (const MdnsService &service : services(adb)) {
        if (service.type != kConnectType)
            continue;
        bool known = false;
        for (const QString &serial : present)
            known = known || serial.startsWith(service.instance) || serial == service.address;
        if (known)
            continue;
        const ProcessResult result = adb.run({"connect", service.address}, 10000);
        if (QString::fromUtf8(result.out).contains("connected to"))
            ++connected;
    }
    return connected;
}

Pairing::Pairing(Runner runner, const QString &adbPath, QObject *parent)
    : QObject(parent), m_runner(std::move(runner)), m_adbPath(adbPath) {
    m_poll.setInterval(1000);
    connect(&m_poll, &QTimer::timeout, this, &Pairing::poll);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(kGiveUpMs);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        m_poll.stop();
        set("failed", tr("The phone didn't answer within three minutes. Check that it is on the same Wi-Fi network "
                         "as this computer and that Wireless debugging is on, then try again."));
    });
    connect(&m_watcher, &QFutureWatcher<Step>::finished, this, &Pairing::stepDone);
}

Pairing::~Pairing() {
    m_poll.stop();
    m_watcher.waitForFinished();
}

void Pairing::start() {
    ++m_generation;
    m_name = "snoopy-" + randomText(6);
    m_password = randomText(10);
    m_qrText = pairingQrText(m_name, m_password);
    m_guid.clear();
    m_serial.clear();
    m_connectLooks = 0;
    if (m_adbPath.isEmpty()) {
        set("failed", tr("adb is not installed. Install the android-tools package, then try again."));
        return;
    }
    set("waiting", tr("On the phone: Settings > System > Developer options > Wireless debugging > "
                      "Pair device with QR code, then scan this code. Both must be on the same Wi-Fi network."));
    m_deadline.start();
    m_poll.start();
    poll();
}

void Pairing::cancel() {
    ++m_generation;
    m_poll.stop();
    m_deadline.stop();
    if (m_state != "connected")
        set("idle", QString());
}

void Pairing::poll() {
    if (m_watcher.isRunning() || (m_state != "waiting" && m_state != "connecting"))
        return;
    const int generation = m_generation;
    const QString state = m_state, name = m_name, password = m_password, guid = m_guid;
    // adb connects to a freshly paired phone by itself within a moment; a
    // second connection by address would list the phone twice. Snoopy
    // connects by hand only after three looks.
    const bool byHand = state == "connecting" && ++m_connectLooks > 3;
    m_watcher.setFuture(QtConcurrent::run([this, state, name, password, guid, generation, byHand] {
        Step result = step(state, name, password, guid, byHand);
        result.generation = generation;
        return result;
    }));
}

Pairing::Step Pairing::step(const QString &state, const QString &name, const QString &password, const QString &guid,
                            bool connectByHand) const {
    Adb adb({}, m_runner, m_adbPath);
    Step result{state, QString(), QString(), guid};
    try {
        if (state == "waiting") {
            for (const MdnsService &service : services(adb)) {
                if (service.type != kPairingType || service.instance != name)
                    continue;
                const ProcessResult pair = adb.run({"pair", service.address, password}, 30000);
                const QString output = QString::fromUtf8(pair.out + pair.err).trimmed();
                if (!output.contains("Successfully paired")) {
                    result.state = "failed";
                    result.message = tr("Pairing failed: %1").arg(output.isEmpty() ? tr("adb gave no answer.") : output);
                    return result;
                }
                static const QRegularExpression guidPattern(QStringLiteral("guid=([^\\]\\s]+)"));
                result.guid = guidPattern.match(output).captured(1);
                result.state = "connecting";
                return result;
            }
            return result;
        }
        // Connecting: adb may already have connected by itself.
        for (const auto &[serial, deviceState] : adb.devices()) {
            if (deviceState == "device" && isWireless(serial) && (guid.isEmpty() || serial.startsWith(guid))) {
                result.state = "connected";
                result.serial = serial;
                return result;
            }
        }
        if (!connectByHand)
            return result;
        for (const MdnsService &service : services(adb)) {
            if (service.type != kConnectType || (!guid.isEmpty() && service.instance != guid))
                continue;
            const ProcessResult connection = adb.run({"connect", service.address}, 10000);
            const QString output = QString::fromUtf8(connection.out).trimmed();
            if (output.contains("connected to")) {
                result.state = "connected";
                result.serial = service.address;
                return result;
            }
        }
    } catch (const AdbError &error) {
        result.state = "failed";
        result.message = error.message();
    }
    return result;
}

void Pairing::stepDone() {
    const Step result = m_watcher.result();
    // A cancel or restart while adb was busy makes this answer stale.
    if (result.generation != m_generation)
        return;
    m_guid = result.guid;
    if (result.state == m_state && result.message.isEmpty())
        return;
    if (result.state == "connecting") {
        set("connecting", tr("Paired. Connecting over Wi-Fi…"));
        poll();
        return;
    }
    if (result.state == "connected") {
        m_poll.stop();
        m_deadline.stop();
        m_serial = result.serial;
        set("connected", tr("Connected over Wi-Fi. You can unplug the cable."));
        emit connected(m_serial);
        return;
    }
    if (result.state == "failed") {
        m_poll.stop();
        m_deadline.stop();
        set("failed", result.message);
    }
}

void Pairing::set(const QString &state, const QString &message) {
    m_state = state;
    m_message = message;
    emit changed();
}
