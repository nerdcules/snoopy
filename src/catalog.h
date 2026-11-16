#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <optional>

// Sensitive operations, permissions and known tracker components, as
// android-diagnoser's catalog.py (7b2d820). A tracker description states
// what that SDK is built to collect; a component match means the code is in
// the app, not that a given payload was seen.
namespace catalog {

struct Info {
    QString category;
    QString detail;
};

struct Tracker {
    QString name;
    QString prefix;
    QString collects;
};

// Permission name -> (category, detail).
const Info *permissionInfo(const QString &permission);
// App-op name -> (category, detail).
const Info *opInfo(const QString &op);

const QStringList &categoryOrder();
const QStringList &sources();
const QStringList &limits();
const QList<Tracker> &trackers();
const QList<QPair<QString, QString>> &trackerDomains();

// Index in categoryOrder(), or its length for an unknown category.
int categoryRank(const QString &category);
// "allowed", "denied", ...; "mode unrecorded" without a mode.
QString modePhrase(const std::optional<QString> &mode);
// The tracker whose domain is the host or a suffix of it, longest first.
std::optional<QString> trackerForHost(const QString &host);
// The SDKs whose prefix appears in the text as a whole name, by name.
QList<Tracker> findTrackers(const QString &text);

} // namespace catalog
