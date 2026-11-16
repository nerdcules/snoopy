#include "names.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QRegularExpression>

static void hostInfoLookup(const QString &address, QObject *context, std::function<void(QString)> done) {
    QHostInfo::lookupHost(address, context, [address, done](const QHostInfo &info) {
        QString name = info.error() == QHostInfo::NoError ? info.hostName() : QString();
        if (name.endsWith('.'))
            name.chop(1);
        // QHostInfo hands the address back when there is no PTR record.
        done(name == address ? QString() : name);
    });
}

Names::Names(Lookup lookup, QObject *parent) : QObject(parent), m_lookup(lookup ? std::move(lookup) : Lookup(hostInfoLookup)) {}

QString Names::key(const QString &host) {
    QString text = host.trimmed();
    while (text.startsWith('['))
        text.remove(0, 1);
    while (text.endsWith(']'))
        text.chop(1);
    static const QRegularExpression v4("^\\d{1,3}(?:\\.\\d{1,3}){3}$");
    static const QRegularExpression v6("^[0-9A-Fa-f:]+$");
    if (v4.match(text).hasMatch())
        return text;
    if (text.contains(':') && v6.match(text).hasMatch())
        return text;
    return {};
}

QString Names::cached(const QString &host) const {
    const QString address = key(host);
    return address.isEmpty() ? QString() : m_cache.value(address);
}

void Names::warm(const QStringList &hosts) {
    for (const QString &host : hosts) {
        const QString address = key(host);
        if (address.isEmpty() || m_cache.contains(address) || m_pending.contains(address))
            continue;
        m_pending.insert(address);
        m_lookup(address, this, [this, address](const QString &name) {
            m_pending.remove(address);
            m_cache.insert(address, name);
            if (!name.isEmpty())
                emit resolved();
        });
    }
}
