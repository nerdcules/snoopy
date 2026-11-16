#pragma once

#include <QJsonObject>

// A report as flows: where information comes from and where it goes.
// {"flows": [...], "apps": [...]}, as android-diagnoser's project_report
// (flows.py at 7b2d820).
QJsonObject projectReport(const QJsonObject &report);
