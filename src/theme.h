#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>

// Follows the Omarchy accent live and Hyprland's window rounding, and hands
// QML the fixed dark palette the rest of the window is painted with.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    // Every text size and the metrics around it are multiplied by this, so the
    // whole window zooms with Ctrl +/- the way a browser does. 1.0 is the
    // designed size; it is clamped and remembered between runs.
    Q_PROPERTY(qreal fontScale READ fontScale NOTIFY fontScaleChanged)
    Q_PROPERTY(QString window READ window CONSTANT)
    Q_PROPERTY(QString panel READ panel CONSTANT)
    Q_PROPERTY(QString panelBorder READ panelBorder CONSTANT)
    Q_PROPERTY(QString text READ text CONSTANT)
    Q_PROPERTY(QString muted READ muted CONSTANT)
    Q_PROPERTY(QString good READ good CONSTANT)
    Q_PROPERTY(QString warn READ warn CONSTANT)
    Q_PROPERTY(QString bad READ bad CONSTANT)
    // The graph's three kinds of node and edge, as android-diagnoser's
    // dashboard coloured them: what was read, where it connects, what it
    // looked up.
    Q_PROPERTY(QString information READ information CONSTANT)
    Q_PROPERTY(QString destination READ destination CONSTANT)
    Q_PROPERTY(QString lookup READ lookup CONSTANT)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }
    qreal fontScale() const { return m_fontScale; }

    Q_INVOKABLE void increaseFont() { setFontScale(m_fontScale + 0.1); }
    Q_INVOKABLE void decreaseFont() { setFontScale(m_fontScale - 0.1); }
    Q_INVOKABLE void resetFont() { setFontScale(1.0); }

    QString window() const { return "#0e0e10"; }
    QString panel() const { return "#17171b"; }
    QString panelBorder() const { return "#28282e"; }
    QString text() const { return "#ececf0"; }
    QString muted() const { return "#8b8b95"; }
    QString good() const { return "#5cd68c"; }
    QString warn() const { return "#f4c542"; }
    QString bad() const { return "#f16a6a"; }
    QString information() const { return "#e2b15a"; }
    QString destination() const { return "#7dbaa6"; }
    QString lookup() const { return "#8eb4d4"; }

    static QString readAccent(const QString &path);

signals:
    void changed();
    void fontScaleChanged();

private:
    void reload();
    void refreshRounding();
    void setFontScale(qreal scale);
    QString scalePath() const;

    QString m_directory;
    bool m_persistScale = false;
    qreal m_fontScale = 1.0;
    QString m_accent = "#FFD60A";
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
