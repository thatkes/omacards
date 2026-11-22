// omacards — keyboard-first flashcards for Omarchy.
#include "backend.h"
#include "cli.h"
#include "theme.h"
#include "traycontroller.h"
#include "translator.h"

#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSystemTrayIcon>
#include <QUrl>

#include <memory>

namespace {
QStringList argumentsFrom(int argc, char *argv[]) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    return arguments;
}
} // namespace

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
    auto ownedBackend = Backend::fromEnvironment();
    Backend &backend = *ownedBackend;
    const auto syncMathColors = [&theme, &backend] { backend.setMathColors(theme.foreground(), theme.accent()); };
    QObject::connect(&theme, &Theme::changed, &backend, syncMathColors);
    // Set the ink before the first card, so its data URIs use the active theme
    // even though the cached SVG itself stays neutral.
    syncMathColors();

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

    // Keep the controller alive for the whole event loop. A stack-local object
    // declared inside the if would die before app.exec() returns.
    std::unique_ptr<TrayController> tray;
    if (QSystemTrayIcon::isSystemTrayAvailable())
        tray = std::make_unique<TrayController>(backend, theme, activate);

    return app.exec();
}
