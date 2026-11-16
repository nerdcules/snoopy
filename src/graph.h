#pragma once

#include <QJsonArray>
#include <QString>
#include <functional>

// The graph the window draws: one card per app with a recent read or a live
// destination, each with "reads" and "destinations", ordered by bytes. As
// android-diagnoser's graph_from (summary.py at 7b2d820).
//
// nameFor gives a destination host's reverse DNS name, or an empty string;
// without it every destination stays numeric.
QJsonArray graphFrom(const QJsonArray &flows, const QJsonArray &apps,
                     const std::function<QString(const QString &host)> &nameFor = {});

// "WhatsApp" for com.whatsapp, "Life360" for its package, otherwise the last
// part of the package name, split at capitals.
QString friendlyName(const QString &package);
