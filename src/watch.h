#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

#include "capture.h"

// The Watch page: things on the phone worth a look, read from one sample.
// Nothing here proves a phone is compromised or clean; each finding says
// what was seen and what it can and can't mean.
//
// A finding is a JSON object:
//   key      stable identity, for the baseline ("bg|pkg|RECORD_AUDIO|cch")
//   level    "alert", "warn" or "info"
//   group    "background", "control", "network", "outside" or "device"
//   package  the app it is about, or ""
//   title    one line
//   detail   what it means and what it doesn't
//   ago      seconds since it happened, for events; absent for state
//   new      true when the phone's baseline doesn't know the key

// One "Access:" or "Reject:" line of `dumpsys appops`, with the op and app
// it belongs to. state is the app's process state then (top, fg, fgsvc, bg,
// cch, pers); flags who made the call (s itself, tp/up a trusted or
// untrusted proxy, ...).
struct OpAccess {
    QString package;
    qint64 uid = -1;
    QString op;
    QString attribution;
    bool rejected = false;
    QString state;
    QString flags;
    qint64 ago = -1;
    qint64 durationMs = 0;
};
QList<OpAccess> parseOpAccesses(const QString &appops);

// "-1d2h3m4s5ms" (the part in brackets) as seconds; -1 if it isn't one.
qint64 agoSeconds(const QString &text);

// The key=value lines of the security dump.
QHash<QString, QString> parseSecurity(const QString &text);

// Third-party packages and who installed them ("null" when nobody said).
QHash<QString, QString> parseInstallers(const QString &text);

// Packages with a launcher activity.
QSet<QString> parseLaunchers(const QString &text);

// Every finding in the sample. today is when it was taken, for the
// security patch's age.
QJsonArray watchFindings(const Capture &capture, const QDate &today);

// What one phone looked like the first time Snoopy saw it, and since:
// findings whose key it holds are not new. Stored as JSON in directory,
// one file per phone.
class WatchBaseline {
public:
    explicit WatchBaseline(const QString &directory = {});

    // Marks new findings for phone. The first look at a phone becomes its
    // baseline: nothing is new then. A key first seen later stays new until
    // acknowledge(), and keeps when it was first seen.
    // With write false nothing is stored: no baseline is made and nothing
    // becomes pending (the check command only looks).
    QJsonArray apply(const QString &phone, const QJsonArray &findings, const QDateTime &now, bool write = true);
    // Everything currently new for phone becomes known.
    void acknowledge(const QString &phone);

private:
    QJsonObject load(const QString &phone) const;
    void save(const QString &phone, const QJsonObject &state) const;
    QString path(const QString &phone) const;

    QString m_directory;
};
