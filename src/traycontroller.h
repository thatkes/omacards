#pragma once

#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

#include <functional>
#include <memory>

class Backend;
class Theme;

// Owns the system tray icon, its menu and all tray-related signal wiring.
// Destroying the controller tears the icon down; the context menu is kept in a
// member declared before the icon so it outlives the icon during destruction.
class TrayController : public QObject {
public:
    TrayController(Backend &backend, const Theme &theme, std::function<void()> activate, QObject *parent = nullptr);

private:
    Backend &m_backend;
    const Theme &m_theme;
    std::function<void()> m_activate;
    std::unique_ptr<QMenu> m_menu;
    QSystemTrayIcon m_tray;
};
