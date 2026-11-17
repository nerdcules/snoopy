#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "adb.h"
#include "capture.h"

// Tightening and loosening an app's permissions over adb. Everything the
// shell is allowed to change: runtime permissions (pm grant/revoke), app-op
// modes (cmd appops set), notification access (cmd notification
// allow/disallow_listener), accessibility services (the secure setting), and
// pausing, disabling or removing an app for the user (all reversible).
//
// A change is planned first (what it does, the exact commands, and the
// commands that undo it), then applied, then read back from the phone to
// check it took, and kept in a journal so it can be undone later.
namespace perms {

struct Permission {
    bool granted = false;
    QStringList flags;
};

// One app as the phone describes it now.
struct AppState {
    QString package;
    bool found = false;
    bool system = false;
    bool suspended = false;
    int enabled = 0; // 0 default, 1 enabled, 2 disabled, 3 disabled by the user
    QSet<QString> requested;
    QHash<QString, Permission> permissions;
    QHash<QString, QString> uidModes;
    QHash<QString, QString> packageModes;
    QHash<QString, qint64> lastAccess; // op -> seconds ago
    QStringList listeners;             // its enabled notification listeners
    QStringList accessibility;         // its enabled accessibility services
    QStringList admins;                // its active device admins
};

// From `dumpsys package <pkg>`, `cmd appops get <pkg>`, and the security
// dump's key=value lines (enabled listeners and accessibility services), plus
// the device-admin dump.
AppState parseApp(const QString &package, const QString &packageDump, const QString &appops, const QString &security,
                  const QString &devicePolicy = {});

// One row of the panel. state is one of options.
struct Control {
    QString id;    // "perm:location", "op:clipboard", "special:usage", "listener:<comp>", "app:pause", ...
    QString group; // "permission", "special" or "app"
    QString label;
    QString state;
    QStringList options;
    bool changeable = true;
    QString reason; // why not, when it can't be changed here
};
QList<Control> controls(const AppState &app);

// What a change does, before it is made.
struct Plan {
    QString package;
    QString control;
    QString label;
    QString from;
    QString to;
    QString detail;
    QList<QStringList> commands; // adb shell argument lists
    QList<QStringList> undo;
    bool blocked = false;
    QString reason;
};
Plan plan(const AppState &app, const QString &control, const QString &to);

// Packages Snoopy never pauses, disables or removes: the phone needs them.
bool essential(const QString &package);

// The control and state that fix a Watch finding, or an empty object:
// {"control", "to", "label"} on the finding as "fix".
QJsonObject fixFor(const QJsonObject &finding);

// Reads, applies and undoes changes on one phone over adb, and keeps the
// journal (<directory>/<phone>.json). Blocking: the window calls it from a
// worker thread.
class Changer {
public:
    Changer(Adb &adb, const QString &phone, const QString &directory = {});

    AppState read(const QString &package);
    // Runs the plan, reads the app again, and records the change. Returns
    // the journal entry: {id, at, package, control, label, from, to,
    // commands, undo, ok, verified, now, output}.
    QJsonObject apply(const Plan &plan);
    // Runs an entry's undo commands and marks it undone.
    QJsonObject undo(int id);
    QJsonArray journal() const;

private:
    QString path() const;
    void save(const QJsonArray &entries) const;
    QString run(const QStringList &command, bool *ok);

    Adb &m_adb;
    QString m_phone;
    QString m_directory;
};

// One step of a preset: which app, which control, to what, and why.
struct Step {
    QString package;
    QString control;
    QString to;
    QString title;
    QString reason;
};

// The presets, as reviewable lists built from a sample (and, for "watch",
// its findings). Ids: "background-location", "unused-mic-camera",
// "unused-personal", "watch". Only third-party apps.
QStringList presetIds();
QString presetTitle(const QString &id);
QList<Step> preset(const QString &id, const Capture &capture, const QJsonArray &findings);

} // namespace perms
