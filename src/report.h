#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "capture.h"

// The report: per Linux uid, what the phone saw each app read, what it was
// granted, the trackers it ships, and its open sockets, bytes and DNS
// lookups. Same keys, values and order as android-diagnoser's build_report
// (analyze.py at 7b2d820); tests/fixtures/golden holds that program's output.
//
// filter, when given, keeps only those packages (report --package).
QJsonObject buildReport(const Capture &capture, bool includeAll = false, const QSet<QString> *filter = nullptr);

// The light snapshot `watch` compares: {"connections", "ops", "focus"}.
QJsonObject watchSnapshot(const Capture &capture);

// One line per change between two watch snapshots, as `watch` prints them.
QStringList diffWatch(const QJsonObject &previous, const QJsonObject &current);
