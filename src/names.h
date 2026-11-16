#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>

// Reverse DNS names for destination addresses, looked up on this computer in
// the background and kept for the life of the process. A name is the
// address's PTR record, not necessarily the name the app looked up.
class Names : public QObject {
    Q_OBJECT

public:
    // lookup resolves one address to a name ("" when there is none) and calls
    // back on this object's thread. Tests pass a fake; the default asks
    // QHostInfo, which does a reverse lookup for an address.
    using Lookup = std::function<void(const QString &address, QObject *context, std::function<void(QString)> done)>;
    explicit Names(Lookup lookup = {}, QObject *parent = nullptr);

    // The cached name for a host ("1.2.3.4", "[2001:db8::1]"), or "".
    QString cached(const QString &host) const;
    // Starts lookups for the hosts not looked up yet.
    void warm(const QStringList &hosts);

    // The address inside host, or "" when host isn't an IP address (names
    // from DNS lookups are never reverse-resolved).
    static QString key(const QString &host);

signals:
    // A lookup finished with a name, so anything showing names can refresh.
    void resolved();

private:
    Lookup m_lookup;
    QHash<QString, QString> m_cache;
    QSet<QString> m_pending;
};
