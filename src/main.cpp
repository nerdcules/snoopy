// snoopy - what each app on a connected Android phone reads, and where it
// is connected.

#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QProcess>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "cli.h"
#include "desktop.h"
#include "iconprovider.h"
#include "icons.h"
#include "monitor.h"
#include "names.h"
#include "permsmodel.h"
#include "qrimage.h"
#include "theme.h"
#include "translator.h"
#include "wireless.h"

int main(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments.append(QString::fromLocal8Bit(argv[i]));

    // The display-free commands only read the phone over adb, so they skip
    // the GUI stack entirely and work over SSH.
    if (isCliCommand(arguments)) {
        QCoreApplication app(argc, argv);
        app.setApplicationName("snoopy");
        // English always, so scripts and agents read the same words; the
        // catalogue still gives English its plural forms ("1 time").
        Translator english(QStringLiteral("/nonexistent/snoopy-cli-language"));
        app.installTranslator(&english);
        return runCli(arguments);
    }

    // `snoopy open --page watch` (the bar widget, a notification) goes to
    // the window that is already open, if there is one.
    const int pageAt = arguments.indexOf("--page");
    const QString page = pageAt > 0 ? arguments.value(pageAt + 1) : QString();
    {
        QCoreApplication probe(argc, argv);
        if (desktop::Remote::sendToRunning(page)) {
            if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
                QProcess::execute("hyprctl", {"dispatch", "focuswindow", "class:^(snoopy)$"});
            return 0;
        }
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("snoopy");
    app.setApplicationVersion("0.1.0");

    // Matches the window to snoopy.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("snoopy");
    app.setWindowIcon(QIcon::fromTheme("snoopy"));

    QQuickStyle::setStyle("Material");

    // `snoopy open --serial S` picks one phone when several are plugged in.
    const int serialAt = arguments.indexOf("--serial");
    const QString serial = serialAt > 0 ? arguments.value(serialAt + 1) : QString();

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    // While this window runs, snoopy.service leaves the phone to it.
    QDir().mkpath(desktop::stateDirectory());
    QLockFile windowLock(desktop::stateDirectory() + "/window.lock");
    windowLock.tryLock(0);
    desktop::Remote remote;
    remote.listen();

    Theme theme;
    Names names;
    Monitor monitor(serial, {}, &names);
    IconSource icons;
    Pairing pairing;
    PermissionsModel permissions(&monitor);
    // A phone that just paired is read over Wi-Fi from now on, at once.
    QObject::connect(&pairing, &Pairing::connected, &monitor, [&monitor] {
        monitor.setLink("wifi");
        monitor.sampleNow();
    });

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.addImageProvider("appicon", new IconProvider(&icons, [&monitor] { return monitor.serial(); }));
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.addImageProvider("qr", new QrProvider);
    engine.rootContext()->setContextProperty("monitor", &monitor);
    engine.rootContext()->setContextProperty("pairing", &pairing);
    engine.rootContext()->setContextProperty("remote", &remote);
    engine.rootContext()->setContextProperty("perms", &permissions);
    engine.rootContext()->setContextProperty("initialPage", page == "watch" ? 1 : 0);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    monitor.start();
    return app.exec();
}
