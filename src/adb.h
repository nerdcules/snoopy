#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>
#include <stdexcept>

// What one finished (or failed) process gave back. A process that could not
// start, or ran past its timeout, has started false or timedOut true and an
// exit code of -1.
struct ProcessResult {
    QByteArray out;
    QByteArray err;
    int code = -1;
    bool started = false;
    bool timedOut = false;
};

// Runs a program to completion. Tests pass their own runner so nothing
// touches a real phone; the app uses runProcess.
using Runner = std::function<ProcessResult(const QString &program, const QStringList &arguments, int timeoutMs)>;

ProcessResult runProcess(const QString &program, const QStringList &arguments, int timeoutMs);

// A problem the user has to fix (no adb, no phone, two phones). The message
// is shown as it is, in the window or on stderr.
class AdbError : public std::runtime_error {
public:
    explicit AdbError(const QString &message) : std::runtime_error(message.toStdString()), m_message(message) {}
    QString message() const { return m_message; }

private:
    QString m_message;
};

// adb from SNOOPY_ADB, then PATH, then the platform-tools folder Android's
// own download unpacks to under ~/.local/opt. Empty when there is none.
QString findAdb();

// "serial state" pairs from `adb devices -l`.
QList<QPair<QString, QString>> parseDevices(const QString &text);

// A thin client for one phone. The phone does the inspection; this only
// reads what it prints. Blocking: the window calls it from a worker thread.
class Adb {
public:
    explicit Adb(const QString &serial = {}, Runner runner = runProcess, const QString &adbPath = findAdb());

    QString serial() const { return m_serial; }
    QString adbPath() const { return m_adbPath; }

    // Which connection to read a phone over: either (a phone on both counts
    // once and the cable wins), only the cable, or only Wi-Fi.
    enum class Link { Any, Cable, Wifi };

    QList<QPair<QString, QString>> devices();
    // The one ready phone's serial over link, or an AdbError saying what is
    // wrong.
    QString resolveSerial(Link link = Link::Any);

    struct Shell {
        QString out;
        QString err;
        int code = -1;
    };
    // `adb [-s serial] shell <command>`, with \r\n and \r turned into \n.
    Shell shell(const QString &command, int timeoutMs = 180000);
    // `adb [-s serial] <arguments>`, bytes as they came. Icons use exec-out.
    ProcessResult run(const QStringList &arguments, int timeoutMs = 20000);

private:
    QString requireAdb() const;

    QString m_serial;
    Runner m_runner;
    QString m_adbPath;
};
