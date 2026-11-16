// Placeholders until the port of android-diagnoser's analysis lands. Each
// returns an empty result of the right shape so the app builds and runs.
#include "flows.h"
#include "graph.h"
#include "render.h"
#include "report.h"

QJsonObject buildReport(const Capture &, bool, const QSet<QString> *) { return {}; }
QJsonObject watchSnapshot(const Capture &) { return {}; }
QStringList diffWatch(const QJsonObject &, const QJsonObject &) { return {}; }
QJsonObject projectReport(const QJsonObject &) { return {{"flows", QJsonArray()}, {"apps", QJsonArray()}}; }
QJsonArray graphFrom(const QJsonArray &, const QJsonArray &, const std::function<QString(const QString &)> &) { return {}; }
QString friendlyName(const QString &package) { return package; }
QString renderText(const QJsonObject &) { return {}; }
