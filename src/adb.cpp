#include "adb.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

ProcessResult runProcess(const QString &program, const QStringList &arguments, int timeoutMs) {
    ProcessResult result;
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        result.err = process.errorString().toUtf8();
        return result;
    }
    result.started = true;
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        result.timedOut = true;
        process.kill();
        process.waitForFinished(2000);
        result.out = process.readAllStandardOutput();
        result.err = process.readAllStandardError();
        return result;
    }
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    result.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return result;
}

static bool executable(const QString &path) {
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable();
}

QString findAdb() {
    const QString override = qEnvironmentVariable("SNOOPY_ADB");
    if (!override.isEmpty() && executable(override))
        return override;
    const QString found = QStandardPaths::findExecutable("adb");
    if (!found.isEmpty())
        return found;
    const QString bundled = QDir::homePath() + "/.local/opt/android-platform-tools/platform-tools/adb";
    return executable(bundled) ? bundled : QString();
}

QList<QPair<QString, QString>> parseDevices(const QString &text) {
    QList<QPair<QString, QString>> devices;
    for (const QString &raw : text.split('\n')) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith("List of devices") || line.startsWith('*'))
            continue;
        const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 2)
            devices.append({parts.at(0), parts.at(1)});
    }
    return devices;
}

static QString normalize(const QByteArray &bytes) {
    QString text = QString::fromUtf8(bytes);
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    return text;
}

Adb::Adb(const QString &serial, Runner runner, const QString &adbPath)
    : m_serial(serial), m_runner(runner ? std::move(runner) : Runner(runProcess)), m_adbPath(adbPath) {}

QString Adb::requireAdb() const {
    if (m_adbPath.isEmpty())
        throw AdbError(QObject::tr("adb is not installed. Install the android-tools package, plug the phone in, "
                                   "and accept the USB debugging prompt."));
    return m_adbPath;
}

QList<QPair<QString, QString>> Adb::devices() {
    const ProcessResult result = m_runner(requireAdb(), {"devices", "-l"}, 30000);
    if (!result.started)
        throw AdbError(QObject::tr("adb could not be started: %1").arg(QString::fromUtf8(result.err).trimmed()));
    return parseDevices(normalize(result.out));
}

QString Adb::resolveSerial() {
    if (!m_serial.isEmpty())
        return m_serial;
    QStringList ready, other;
    for (const auto &[serial, state] : devices()) {
        if (state == "device")
            ready.append(serial);
        else
            other.append(QString("%1 (%2)").arg(serial, state));
    }
    if (ready.size() == 1) {
        m_serial = ready.first();
        return m_serial;
    }
    if (ready.isEmpty() && !other.isEmpty())
        throw AdbError(QObject::tr("A phone is connected but not ready: %1. Unlock it and accept the USB debugging prompt.")
                           .arg(other.join(", ")));
    if (ready.isEmpty())
        throw AdbError(QObject::tr("No Android device is connected. Enable USB debugging and plug the phone in."));
    throw AdbError(QObject::tr("More than one device is connected. Pick one with --serial. Devices: %1").arg(ready.join(", ")));
}

ProcessResult Adb::run(const QStringList &arguments, int timeoutMs) {
    QStringList full;
    if (!m_serial.isEmpty())
        full << "-s" << m_serial;
    full << arguments;
    return m_runner(requireAdb(), full, timeoutMs);
}

Adb::Shell Adb::shell(const QString &command, int timeoutMs) {
    const ProcessResult result = run({"shell", command}, timeoutMs);
    if (!result.started)
        throw AdbError(QObject::tr("adb could not be started: %1").arg(QString::fromUtf8(result.err).trimmed()));
    Shell shell;
    shell.out = normalize(result.out);
    shell.err = normalize(result.err);
    shell.code = result.code;
    return shell;
}
