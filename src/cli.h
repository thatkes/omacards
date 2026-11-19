#pragma once

#include <QStringList>

namespace Cli {
// Headless commands run offscreen, so the platform must be set before
// QGuiApplication is constructed.
bool needsOffscreen(const QStringList &arguments);
int run(const QStringList &arguments);
} // namespace Cli
