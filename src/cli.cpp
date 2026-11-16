#include "cli.h"

#include "adb.h"
#include "capture.h"
#include "icons.h"
#include "render.h"
#include "report.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>
#include <QThread>
#include <QTime>
#include <csignal>

namespace {

const QStringList kCommands{"scan", "watch", "report", "icon", "skill", "help", "--help", "-h"};

void usage(QTextStream &out) {
    out << "snoopy - what each app on a connected Android phone reads, and where it is connected\n\n"
           "  snoopy [open] [--serial S]   open the window\n"
           "  snoopy scan [--json] [--all] [--package P]... [--out DIR] [--serial S]\n"
           "                               one sample of the phone, as a report\n"
           "  snoopy watch [--interval 5] [--count 12] [--package P]... [--serial S]\n"
           "                               print sockets, focus and app ops as they change (--count 0: until Ctrl+C)\n"
           "  snoopy report --capture DIR [--json] [--all] [--package P]...\n"
           "                               the report for a capture saved with scan --out\n"
           "  snoopy icon PACKAGE [--out FILE] [--fresh] [--json] [--serial S]\n"
           "                               an app's launcher icon from the phone (--fresh skips the cache)\n"
           "  snoopy skill install         install the agent skill into ~/.claude/skills/snoopy\n\n"
           "The phone is read over adb (USB debugging on, this computer accepted).\n"
           "HTTPS message bodies stay encrypted: a connection shows its address, not its contents.\n"
           "Commands exit 0 on success and 1 with the problem on stderr.\n";
}

// --name value pairs and bare flags, in any order after the command.
struct Options {
    QHash<QString, QString> values;
    QStringList packages;
    QSet<QString> flags;
    QString problem;
};

Options parse(const QStringList &arguments, const QStringList &valued, const QStringList &bare) {
    Options options;
    for (int i = 2; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (bare.contains(argument)) {
            options.flags.insert(argument);
        } else if (valued.contains(argument)) {
            if (i + 1 >= arguments.size()) {
                options.problem = QString("%1 needs a value").arg(argument);
                return options;
            }
            const QString value = arguments.at(++i);
            if (argument == "--package")
                options.packages.append(value);
            else
                options.values.insert(argument, value);
        } else {
            options.problem = QString("unknown option %1").arg(argument);
            return options;
        }
    }
    return options;
}

void emitReport(QTextStream &out, const QJsonObject &report, bool json) {
    if (json)
        out << QJsonDocument(report).toJson(QJsonDocument::Indented);
    else
        out << renderText(report);
}

int scan(const Options &options, QTextStream &out, QTextStream &err) {
    Adb adb(options.values.value("--serial"));
    const Capture capture = collectSnapshot(adb);
    if (options.values.contains("--out")) {
        QString error;
        if (!saveCapture(capture, options.values.value("--out"), &error)) {
            err << error << "\n";
            return 1;
        }
    }
    const QSet<QString> filter(options.packages.begin(), options.packages.end());
    emitReport(out, buildReport(capture, options.flags.contains("--all"), filter.isEmpty() ? nullptr : &filter),
               options.flags.contains("--json"));
    return 0;
}

int report(const Options &options, QTextStream &out, QTextStream &err) {
    if (!options.values.contains("--capture")) {
        err << "Usage: snoopy report --capture DIR [--json] [--all] [--package P]\n";
        return 1;
    }
    Capture capture;
    QString error;
    if (!loadCapture(options.values.value("--capture"), &capture, &error)) {
        err << error << "\n";
        return 1;
    }
    const QSet<QString> filter(options.packages.begin(), options.packages.end());
    emitReport(out, buildReport(capture, options.flags.contains("--all"), filter.isEmpty() ? nullptr : &filter),
               options.flags.contains("--json"));
    return 0;
}

volatile std::sig_atomic_t g_interrupted = 0;

// Keeps only the wanted packages, as android-diagnoser's watch --package.
QJsonObject filterSnapshot(const QJsonObject &snapshot, const QSet<QString> &wanted) {
    QJsonArray connections;
    for (const QJsonValue &value : snapshot.value("connections").toArray()) {
        QJsonObject item = value.toObject();
        QJsonArray names;
        for (const QJsonValue &name : item.value("packages").toArray())
            if (wanted.contains(name.toString()))
                names.append(name);
        if (names.isEmpty())
            continue;
        item.insert("packages", names);
        connections.append(item);
    }
    QJsonArray ops;
    for (const QJsonValue &value : snapshot.value("ops").toArray())
        if (wanted.contains(value.toObject().value("package").toString()))
            ops.append(value);
    QJsonArray focus;
    for (const QJsonValue &value : snapshot.value("focus").toArray())
        if (wanted.contains(value.toString().section('/', 0, 0)))
            focus.append(value);
    return {{"connections", connections}, {"ops", ops}, {"focus", focus}};
}

int watch(const Options &options, QTextStream &out, QTextStream &err) {
    bool ok = true;
    const double interval = options.values.value("--interval", "5").toDouble(&ok);
    if (!ok || interval <= 0) {
        err << "--interval must be greater than 0\n";
        return 1;
    }
    const int count = options.values.value("--count", "12").toInt(&ok);
    if (!ok || count < 0) {
        err << "--count must be 0 or more\n";
        return 1;
    }
    const QSet<QString> wanted(options.packages.begin(), options.packages.end());
    Adb adb(options.values.value("--serial"));
    adb.resolveSerial();
    std::signal(SIGINT, [](int) { g_interrupted = 1; });

    QJsonObject previous;
    bool first = true;
    for (int seen = 0; (count == 0 || seen < count) && !g_interrupted; ++seen) {
        QJsonObject current = watchSnapshot(collectWatch(adb));
        if (!wanted.isEmpty())
            current = filterSnapshot(current, wanted);
        if (first) {
            QSet<QString> apps;
            for (const QJsonValue &item : current.value("connections").toArray())
                for (const QJsonValue &name : item.toObject().value("packages").toArray())
                    apps.insert(name.toString());
            out << "Watching. Baseline: " << current.value("connections").toArray().size() << " sockets, "
                << current.value("focus").toArray().size() << " resumed activities.\n";
            if (!apps.isEmpty()) {
                QStringList sorted(apps.begin(), apps.end());
                sorted.sort();
                out << "Apps with sockets: " << sorted.join(", ") << "\n";
            }
            first = false;
        } else {
            for (const QString &event : diffWatch(previous, current))
                out << QTime::currentTime().toString("HH:mm:ss ") << event << "\n";
        }
        out.flush();
        previous = current;
        if (count != 0 && seen + 1 >= count)
            break;
        // Sleep in short steps so Ctrl+C stops promptly.
        for (int waited = 0; waited < int(interval * 1000) && !g_interrupted; waited += 100)
            QThread::msleep(100);
    }
    if (g_interrupted)
        out << "\nStopped.\n";
    return 0;
}

int icon(const QStringList &arguments, const Options &options, QTextStream &out, QTextStream &err) {
    const QString package = arguments.value(2);
    if (package.isEmpty() || package.startsWith("--")) {
        err << "Usage: snoopy icon PACKAGE [--out FILE] [--fresh] [--json] [--serial S]\n";
        return 1;
    }
    Adb adb(options.values.value("--serial"));
    const QString serial = adb.resolveSerial();
    QTemporaryDir fresh;
    IconSource source(options.flags.contains("--fresh") ? fresh.path() : QString());
    QElapsedTimer timer;
    timer.start();
    QString mime;
    const QByteArray image = source.icon(package, serial, &mime);
    const qint64 ms = timer.elapsed();
    if (options.values.contains("--out")) {
        QSaveFile file(options.values.value("--out"));
        if (!file.open(QIODevice::WriteOnly) || file.write(image) != image.size() || !file.commit()) {
            err << "Could not write " << options.values.value("--out") << "\n";
            return 1;
        }
    }
    // A monogram means the phone had no png or webp icon for the package.
    const bool found = mime != "image/svg+xml";
    if (options.flags.contains("--json"))
        out << QJsonDocument(QJsonObject{{"package", package}, {"mime", mime}, {"bytes", image.size()},
                                         {"found", found}, {"ms", ms}}).toJson(QJsonDocument::Compact) << "\n";
    else
        out << package << ": " << (found ? mime : QString("no icon on the phone, monogram")) << ", " << image.size()
            << " bytes, " << ms << " ms\n";
    return 0;
}

int installSkill(QTextStream &out, QTextStream &err) {
    QFile source(":/skill/SKILL.md");
    if (!source.open(QIODevice::ReadOnly)) {
        err << "The bundled skill is missing from this build.\n";
        return 1;
    }
    const QString directory = QDir::homePath() + "/.claude/skills/snoopy";
    if (!QDir().mkpath(directory)) {
        err << "Could not create " << directory << "\n";
        return 1;
    }
    QFile target(directory + "/SKILL.md");
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate) || target.write(source.readAll()) < 0) {
        err << "Could not write " << target.fileName() << "\n";
        return 1;
    }
    out << "Installed " << target.fileName() << "\n";
    return 0;
}

} // namespace

bool isCliCommand(const QStringList &arguments) {
    return arguments.size() > 1 && kCommands.contains(arguments.at(1));
}

int runCli(const QStringList &arguments) {
    QTextStream out(stdout), err(stderr);
    const QString command = arguments.at(1);
    if (command == "help" || command == "--help" || command == "-h") {
        usage(out);
        return 0;
    }
    if (command == "skill") {
        if (arguments.size() > 2 && arguments.at(2) == "install")
            return installSkill(out, err);
        err << "Usage: snoopy skill install\n";
        return 1;
    }

    Options options;
    if (command == "scan")
        options = parse(arguments, {"--serial", "--package", "--out"}, {"--json", "--all"});
    else if (command == "watch")
        options = parse(arguments, {"--serial", "--package", "--interval", "--count"}, {});
    else if (command == "icon")
        options = parse(arguments.mid(0, 2) + arguments.mid(3), {"--serial", "--out"}, {"--fresh", "--json"});
    else
        options = parse(arguments, {"--capture", "--package"}, {"--json", "--all"});
    if (!options.problem.isEmpty()) {
        err << "snoopy " << command << ": " << options.problem << "\n";
        return 1;
    }

    try {
        if (command == "scan")
            return scan(options, out, err);
        if (command == "watch")
            return watch(options, out, err);
        if (command == "icon")
            return icon(arguments, options, out, err);
        return report(options, out, err);
    } catch (const AdbError &error) {
        out.flush();
        err << "snoopy: " << error.message() << "\n";
        return 1;
    }
}
