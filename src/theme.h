#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

// Follows Omarchy's current theme: palette (light or dark), accent, and
// Hyprland's effective corner rounding. The palette layer follows keymemo
// (MIT, David Heinemeier Hansson); the watcher re-arms around every change
// because a theme switch replaces links and files.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QString background READ background NOTIFY changed)
    Q_PROPERTY(QString foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QString soft READ soft NOTIFY changed)
    Q_PROPERTY(QString muted READ muted NOTIFY changed)
    Q_PROPERTY(QString panel READ panel NOTIFY changed)
    Q_PROPERTY(QString raised READ raised NOTIFY changed)
    Q_PROPERTY(QString border READ border NOTIFY changed)
    Q_PROPERTY(QString red READ red NOTIFY changed)
    Q_PROPERTY(QString yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QString green READ green NOTIFY changed)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    QString background() const { return m_background; }
    QString foreground() const { return m_foreground; }
    QString soft() const { return m_soft; }
    QString muted() const { return m_muted; }
    QString panel() const { return m_panel; }
    QString raised() const { return m_raised; }
    QString border() const { return m_border; }
    QString red() const { return m_red; }
    QString yellow() const { return m_yellow; }
    QString green() const { return m_green; }
    bool dark() const { return m_dark; }
    int radius() const { return m_radius; }

    static QString readAccent(const QString &path);
    static QString readColor(const QString &path, const QString &key, const QString &fallback = {});
    static QHash<QString, QString> readColors(const QString &path);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QString m_accent = "#FFD60A";
    QString m_background = "#0e0e10";
    QString m_foreground = "#c0caf5";
    QString m_soft = "#a9b1d6";
    QString m_muted = "#565f89";
    QString m_panel = "#13141c";
    QString m_raised = "#1b1d28";
    QString m_border = "#232636";
    QString m_red = "#f7768e";
    QString m_yellow = "#e0af68";
    QString m_green = "#9ece6a";
    bool m_dark = true;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
