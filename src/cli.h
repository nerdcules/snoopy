#pragma once

#include <QStringList>

// Commands that need no display, so scripts and agents can read the same
// numbers the window shows. Returns true when the arguments named one.
bool isCliCommand(const QStringList &arguments);
int runCli(const QStringList &arguments);
