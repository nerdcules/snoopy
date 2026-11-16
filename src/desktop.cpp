#include "desktop.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <unistd.h>

namespace desktop {

namespace {

constexpr char32_t kPhoneGlyph = 0xF10B; // nf-fa-mobile, in JetBrains Mono Nerd Font

QString directoryOr(const QString &directory) {
    return directory.isEmpty() ? stateDirectory() : directory;
}

QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void writeJson(const QString &path, const QJsonObject &object) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

int run(const QString &program, const QStringList &arguments, int timeoutMs = 15000) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000))
        return -1;
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        return -1;
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

} // namespace

QString stateDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/snoopy";
}

QJsonObject status(bool connected, const QString &transport, const QJsonObject &device, const QJsonArray &findings,
                   const QString &message, const QString &source) {
    int alerts = 0, warnings = 0, fresh = 0, freshAlerts = 0;
    for (const QJsonValue &value : findings) {
        const QJsonObject item = value.toObject();
        const QString level = item.value("level").toString();
        const bool isNew = item.value("new").toBool();
        alerts += level == "alert";
        warnings += level == "warn";
        fresh += isNew && level != "info";
        freshAlerts += isNew && level == "alert";
    }
    return {{"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"connected", connected},
            {"transport", transport},
            {"model", device.value("model").toString()},
            {"android", device.value("android").toString()},
            {"alerts", alerts},
            {"warnings", warnings},
            {"newCount", fresh},
            {"newAlerts", freshAlerts},
            {"message", message},
            {"source", source}};
}

void writeStatus(const QJsonObject &status, const QString &directory) {
    writeJson(directoryOr(directory) + "/status.json", status);
}

QJsonArray takeUnnotified(const QString &phone, const QJsonArray &findings, const QString &directory) {
    if (phone.isEmpty())
        return {};
    const QString path = directoryOr(directory) + "/notified.json";
    QJsonObject notified = readJson(path);
    QJsonArray told = notified.value(phone).toArray();
    QSet<QString> known;
    for (const QJsonValue &key : told)
        known.insert(key.toString());
    QJsonArray fresh;
    for (const QJsonValue &value : findings) {
        const QJsonObject item = value.toObject();
        const QString key = item.value("key").toString();
        if (!item.value("new").toBool() || item.value("level").toString() == "info" || known.contains(key))
            continue;
        fresh.append(item);
        told.append(key);
        known.insert(key);
    }
    if (!fresh.isEmpty()) {
        notified.insert(phone, told);
        writeJson(path, notified);
    }
    return fresh;
}

QList<Notification> notificationsFor(const QJsonArray &findings) {
    const QStringList click{QCoreApplication::applicationFilePath(), "open", "--page", "watch"};
    QList<Notification> out;
    if (findings.isEmpty())
        return out;
    // The toast's body is rich text: escape what the phone reported.
    if (findings.size() <= 3) {
        for (const QJsonValue &value : findings) {
            const QJsonObject item = value.toObject();
            out.append({item.value("title").toString(), item.value("detail").toString().toHtmlEscaped(),
                        item.value("level").toString() == "alert" ? 2 : 1, click});
        }
        return out;
    }
    QStringList lines;
    bool alert = false;
    for (int i = 0; i < findings.size(); ++i) {
        const QJsonObject item = findings.at(i).toObject();
        alert = alert || item.value("level").toString() == "alert";
        if (i < 3)
            lines.append(item.value("title").toString().toHtmlEscaped());
    }
    lines.append(QObject::tr("and %n more on the Watch page", "", int(findings.size()) - 3));
    out.append({QObject::tr("%n new on your phone", "", int(findings.size())), lines.join("\n"), alert ? 2 : 1, click});
    return out;
}

void send(const Notification &notification) {
    QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                          "org.freedesktop.Notifications", "Notify");
    QVariantMap hints;
    hints.insert("urgency", QVariant::fromValue(uchar(notification.urgency)));
    hints.insert("omarchy-glyph", QString::fromUcs4(&kPhoneGlyph, 1));
    if (!notification.clickArgv.isEmpty())
        hints.insert("omarchy-exec-argv",
                     QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(notification.clickArgv)).toJson(QJsonDocument::Compact)));
    message << QString("Snoopy") << uint(0) << QString() << notification.summary << notification.body << QStringList()
            << hints << int(-1);
    QDBusConnection::sessionBus().call(message, QDBus::Block, 5000);
}

void publish(const QString &phone, bool connected, const QString &transport, const QJsonObject &device,
             const QJsonArray &findings, const QString &message, const QString &source) {
    writeStatus(status(connected, transport, device, findings, message, source));
    if (!connected)
        return;
    for (const Notification &notification : notificationsFor(takeUnnotified(phone, findings)))
        send(notification);
}

Remote::Remote(QObject *parent) : QObject(parent) {}

QString Remote::serverName() {
    // SNOOPY_INSTANCE keeps tests away from a window the user has open.
    const QString instance = qEnvironmentVariable("SNOOPY_INSTANCE");
    return instance.isEmpty() ? QStringLiteral("snoopy-%1").arg(getuid()) : instance;
}

namespace {

bool sendLine(const QString &line) {
    QLocalSocket socket;
    socket.connectToServer(Remote::serverName());
    if (!socket.waitForConnected(500))
        return false;
    socket.write((line + "\n").toUtf8());
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}

} // namespace

bool Remote::listen() {
    m_server = new QLocalServer(this);
    if (!m_server->listen(serverName())) {
        // A crashed window leaves its socket behind; nobody answers there.
        if (sendLine("ping"))
            return false;
        QLocalServer::removeServer(serverName());
        if (!m_server->listen(serverName()))
            return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server->nextPendingConnection()) {
            auto read = [this, socket] {
                while (socket->canReadLine()) {
                    const QString line = QString::fromUtf8(socket->readLine()).trimmed();
                    // "page" alone raises the window; "ping" only checks it's there.
                    if (line == "page" || line.startsWith("page "))
                        emit showPage(line.mid(4).trimmed());
                }
            };
            connect(socket, &QLocalSocket::readyRead, this, read);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            // A short message can arrive before readyRead is connected.
            read();
        }
    });
    return true;
}

bool Remote::sendToRunning(const QString &page) {
    return sendLine(page.isEmpty() ? QStringLiteral("page") : QStringLiteral("page ") + page);
}

bool installPlugin(const std::function<void(const QString &)> &report) {
    const QString target = QDir::homePath() + "/.config/omarchy/plugins/snoopy.phone";
    const bool replacing = QDir(target).exists();
    // Always a copy: Omarchy refuses plugin folders with symlinks.
    QDir(target).removeRecursively();
    QDirIterator files(":/plugin/snoopy.phone", QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString source = files.next();
        const QString destination = target + source.mid(QString(":/plugin/snoopy.phone").size());
        QDir().mkpath(QFileInfo(destination).absolutePath());
        QFile::remove(destination);
        if (!QFile::copy(source, destination)) {
            report(QObject::tr("Could not write %1").arg(destination));
            return false;
        }
        QFile(destination).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup |
                                          QFileDevice::ReadOther);
    }
    if (run("omarchy", {"plugin", "validate", target}) != 0) {
        report(QObject::tr("omarchy plugin validate refused %1").arg(target));
        return false;
    }
    run("omarchy-shell", {"shell", "rescanPlugins"});
    // The shell finds a new plugin a moment after the rescan.
    bool enabled = false;
    for (int tries = 0; tries < 10 && !enabled; ++tries) {
        enabled = run("omarchy", {"plugin", "enable", "snoopy.phone", "--section", "left"}) == 0;
        if (!enabled)
            QThread::msleep(500);
    }
    if (!enabled) {
        report(QObject::tr("Couldn't turn the bar widget on; try: omarchy plugin enable snoopy.phone"));
        return false;
    }
    // A shell that already loaded the old widget can keep parts of it.
    if (replacing)
        run("omarchy-restart-shell", {}, 30000);
    report(QObject::tr("Bar widget installed in %1").arg(target));
    return true;
}

bool enableDaemon(const std::function<void(const QString &)> &report) {
    run("systemctl", {"--user", "daemon-reload"});
    if (run("systemctl", {"--user", "enable", "--now", "snoopy.service"}) != 0) {
        report(QObject::tr("Couldn't start snoopy.service; is the package installed? Try: systemctl --user status snoopy"));
        return false;
    }
    report(QObject::tr("snoopy.service is on: the phone is checked every few minutes, also with the window closed."));
    return true;
}

} // namespace desktop
