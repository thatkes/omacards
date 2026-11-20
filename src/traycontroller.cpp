#include "traycontroller.h"
#include "backend.h"
#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

#include <utility>

namespace {
QIcon trayIcon(const QString &accent) {
    QIcon themed = QIcon::fromTheme("omacards");
    if (!themed.isNull())
        return themed;
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(accent));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(4, 4, 56, 56, 12, 12);
    painter.setPen(Qt::black);
    painter.setFont(QFont("Sans", 24, QFont::Bold));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, "C");
    return QIcon(pixmap);
}
} // namespace

TrayController::TrayController(Backend &backend, const Theme &theme, std::function<void()> activate, QObject *parent)
    : QObject(parent), m_backend(backend), m_theme(theme), m_activate(std::move(activate)),
      m_menu(std::make_unique<QMenu>()), m_tray(trayIcon(theme.accent())) {
    m_tray.setToolTip(QObject::tr("omacards — %1 due").arg(m_backend.dueCount()));

    m_menu->addAction(QObject::tr("Open"), m_activate);
    m_menu->addAction(QObject::tr("Review"), [this] {
        m_backend.setMode("review");
        m_activate();
    });
    m_menu->addAction(QObject::tr("Cram"), [this] {
        m_backend.setMode("cram");
        m_activate();
    });
    m_menu->addSeparator();
    m_menu->addAction(QObject::tr("Quit"), &QApplication::quit);
    m_tray.setContextMenu(m_menu.get());

    QObject::connect(&m_backend, &Backend::dueCountChanged, this,
                     [this] { m_tray.setToolTip(QObject::tr("omacards — %1 due").arg(m_backend.dueCount())); });
    QObject::connect(&m_theme, &Theme::changed, this, [this] { m_tray.setIcon(trayIcon(m_theme.accent())); });
    QObject::connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Context)
            return;
        m_backend.setMode("review");
        m_activate();
    });

    m_tray.show();
}
