#pragma once

#include <QJsonObject>
#include <QString>

// The plain-text report `scan` and `report` print without --json, as
// android-diagnoser's render_text (render.py at 7b2d820).
QString renderText(const QJsonObject &report);
