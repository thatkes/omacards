// omacards — keyboard-first flashcards for Omarchy.
#include "backend.h"
#include "cli.h"
#include "theme.h"
#include "translator.h"

#include <QColor>
#include <QFont>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSystemTrayIcon>
#include <QUrl>

namespace {
QStringList argumentsFrom(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    return arguments;
}

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
}

int main(int argc, char *argv[]) {
    const QStringList allArguments = argumentsFrom(argc, argv);
    const QStringList arguments = allArguments.mid(1);

    // Agent commands must work on a headless machine. Set the platform before
    // any QGuiApplication exists, then exit without loading QML.
    if (Cli::needsOffscreen(arguments))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    if (Cli::needsOffscreen(arguments))
        return Cli::run(arguments);

    app.setApplicationName("omacards");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("omarchy");
    app.setDesktopFileName("omacards");
    app.setWindowIcon(QIcon::fromTheme("omacards"));
    QQuickStyle::setStyle("Material");

    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    QString error;
    if (!backend.initialize(&error))
        qWarning() << "omacards:" << error;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    QObject *window = engine.rootObjects().first();
    auto activate = [window] {
        QMetaObject::invokeMethod(window, "show", Qt::QueuedConnection);
        QMetaObject::invokeMethod(window, "raise", Qt::QueuedConnection);
        QMetaObject::invokeMethod(window, "requestActivate", Qt::QueuedConnection);
    };
    QObject::connect(&backend, &Backend::showWindowRequested, window, activate);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        QSystemTrayIcon tray(trayIcon(theme.accent()));
        tray.setToolTip(QObject::tr("omacards — %1 due").arg(backend.dueCount()));
        auto menu = new QMenu;
        menu->addAction(QObject::tr("Open"), activate);
        menu->addAction(QObject::tr("Review"), [&backend, activate] {
            backend.setMode("review");
            activate();
        });
        menu->addAction(QObject::tr("Cram"), [&backend, activate] {
            backend.setMode("cram");
            activate();
        });
        menu->addSeparator();
        menu->addAction(QObject::tr("Quit"), &QApplication::quit);
        tray.setContextMenu(menu);
        QObject::connect(&backend, &Backend::dueCountChanged, &tray, [&tray, &backend] {
            tray.setToolTip(QObject::tr("omacards — %1 due").arg(backend.dueCount()));
        });
        QObject::connect(&theme, &Theme::changed, &tray, [&tray, &theme] {
            tray.setIcon(trayIcon(theme.accent()));
        });
        QObject::connect(&tray, &QSystemTrayIcon::activated, &tray, [activate, &backend](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Context)
                return;
            backend.setMode("review");
            activate();
        });
        tray.show();
        return app.exec();
    }

    return app.exec();
}
