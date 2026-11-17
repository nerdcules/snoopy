#pragma once

#include <QFutureWatcher>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "perms.h"

class Monitor;

// The window's side of perms: one app at a time in the Permissions panel,
// the presets, and the journal of changes, for the phone the monitor reads.
// Reading and changing go over adb on a worker thread.
class PermissionsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString packageName READ package NOTIFY appChanged)
    Q_PROPERTY(QString appName READ appName NOTIFY appChanged)
    Q_PROPERTY(bool system READ system NOTIFY appChanged)
    Q_PROPERTY(QVariantList controls READ controls NOTIFY appChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY appChanged)
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY resultChanged)
    Q_PROPERTY(QVariantList journal READ journal NOTIFY journalChanged)
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    Q_PROPERTY(QString presetId READ presetId NOTIFY presetChanged)
    Q_PROPERTY(QVariantList presetSteps READ presetSteps NOTIFY presetChanged)
    // The guided review's questions, from the monitor's last sample.
    Q_PROPERTY(QVariantList questions READ questions NOTIFY questionsChanged)

public:
    explicit PermissionsModel(Monitor *monitor, QObject *parent = nullptr);
    ~PermissionsModel() override;

    QString package() const { return m_app.package; }
    QString appName() const;
    bool system() const { return m_app.system; }
    QVariantList controls() const;
    bool busy() const { return m_watcher.isRunning(); }
    QString error() const { return m_error; }
    QVariantMap lastResult() const { return m_lastResult; }
    QVariantList journal() const { return m_journal; }
    QVariantList presets() const;
    QString presetId() const { return m_presetId; }
    QVariantList presetSteps() const { return m_presetSteps; }
    QVariantList questions() const { return m_questions; }

    // Builds the review's questions; askAll forgets what was kept before.
    Q_INVOKABLE void loadReview(bool askAll);
    // Answers question index: "keep" is remembered and changes nothing;
    // any other state is applied and checked on the phone. answered()
    // reports the outcome.
    Q_INVOKABLE void answer(int index, const QString &state);

    // Reads package's state from the phone into the panel.
    Q_INVOKABLE void open(const QString &package);
    // What setting control to state would do: label, from, to, detail,
    // commands (as text), blocked, reason. No adb call.
    Q_INVOKABLE QVariantMap planFor(const QString &control, const QString &to) const;
    Q_INVOKABLE void apply(const QString &control, const QString &to);
    Q_INVOKABLE void undo(int id);
    Q_INVOKABLE void refreshJournal();
    // The steps of a preset, from the monitor's last sample and findings.
    Q_INVOKABLE void loadPreset(const QString &id);
    // Applies the preset steps whose indexes are given, one app at a time.
    Q_INVOKABLE void applyPreset(const QVariantList &indexes);

signals:
    void appChanged();
    void busyChanged();
    void resultChanged();
    void journalChanged();
    void presetChanged();
    void questionsChanged();
    void answered(int index, const QVariantMap &result);

private:
    struct Job {
        perms::AppState app;
        QJsonArray results;
        QJsonArray journal;
        QString error;
        bool hasApp = false;
    };
    void start(std::function<Job()> work);
    void finished();

    Monitor *m_monitor;
    QFutureWatcher<Job> m_watcher;
    perms::AppState m_app;
    QString m_error;
    QVariantMap m_lastResult;
    QVariantList m_journal;
    QString m_presetId;
    QVariantList m_presetSteps;
    QList<perms::Step> m_steps;
    QVariantList m_questions;
    int m_answering = -1;
};
