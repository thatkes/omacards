#pragma once

#include <QStringList>

namespace Cli {
// Every command path in the checked-in registry, embedded as a Qt resource.
QStringList commandPaths();
// First-level command tokens from the registry that run headlessly.
QStringList headlessCommands();
// True for the registry's help paths (`help`, `--help`, `-h`).
bool isHelpCommand(const QString &command);
// Headless commands run offscreen, so the platform must be set before
// QGuiApplication is constructed.
bool needsOffscreen(const QStringList &arguments);
int run(const QStringList &arguments);
} // namespace Cli
