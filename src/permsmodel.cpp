#include "permsmodel.h"

#include "adb.h"
#include "graph.h"
#include "monitor.h"

#include <QJsonArray>
#include <QtConcurrent>

namespace {

QString hardwareSerial(Adb &adb) {
    return adb.shell("getprop ro.serialno").out.trimmed();
}

} // namespace

PermissionsModel::PermissionsModel(Monitor *monitor, QObject *parent) : QObject(parent), m_monitor(monitor) {
    connect(&m_watcher, &QFutureWatcher<Job>::finished, this, &PermissionsModel::finished);
}

PermissionsModel::~PermissionsModel() {
    m_watcher.waitForFinished();
}

QString PermissionsModel::appName() const {
    return m_app.package.isEmpty() ? QString() : friendlyName(m_app.package);
}

QVariantList PermissionsModel::controls() const {
    QVariantList list;
    for (const perms::Control &c : perms::controls(m_app))
        list.append(QVariantMap{{"id", c.id}, {"group", c.group}, {"label", c.label}, {"state", c.state},
                                {"options", c.options}, {"changeable", c.changeable}, {"reason", c.reason}});
    return list;
}

QVariantList PermissionsModel::presets() const {
    QVariantList list;
    for (const QString &id : perms::presetIds())
        list.append(QVariantMap{{"id", id}, {"title", perms::presetTitle(id)}});
    return list;
}

void PermissionsModel::start(std::function<Job()> work) {
    if (m_watcher.isRunning())
        return;
    const QString serial = m_monitor ? m_monitor->serial() : QString();
    m_watcher.setFuture(QtConcurrent::run([work] { return work(); }));
    Q_UNUSED(serial)
    emit busyChanged();
}

void PermissionsModel::open(const QString &package) {
    const QString serial = m_monitor ? m_monitor->serial() : QString();
    m_app = perms::AppState();
    m_app.package = package;
    m_error.clear();
    m_lastResult.clear();
    emit appChanged();
    emit resultChanged();
    start([serial, package] {
        Job job;
        try {
            Adb adb(serial);
            adb.resolveSerial();
            perms::Changer changer(adb, hardwareSerial(adb));
            job.app = changer.read(package);
            job.hasApp = true;
            job.journal = changer.journal();
            if (!job.app.found)
                job.error = tr("%1 isn't installed on the phone.").arg(package);
        } catch (const AdbError &error) {
            job.error = error.message();
        }
        return job;
    });
}

QVariantMap PermissionsModel::planFor(const QString &control, const QString &to) const {
    const perms::Plan plan = perms::plan(m_app, control, to);
    QStringList commands;
    for (const QStringList &command : plan.commands)
        commands.append("adb shell " + command.join(' '));
    return {{"label", plan.label}, {"from", plan.from}, {"to", plan.to}, {"detail", plan.detail},
            {"commands", commands}, {"blocked", plan.blocked}, {"reason", plan.reason}, {"control", control}};
}

void PermissionsModel::apply(const QString &control, const QString &to) {
    const QString serial = m_monitor ? m_monitor->serial() : QString();
    const QString package = m_app.package;
    start([serial, package, control, to] {
        Job job;
        try {
            Adb adb(serial);
            adb.resolveSerial();
            perms::Changer changer(adb, hardwareSerial(adb));
            // Planned again from the phone's state now, not the panel's.
            job.results.append(changer.apply(perms::plan(changer.read(package), control, to)));
            job.app = changer.read(package);
            job.hasApp = true;
            job.journal = changer.journal();
        } catch (const AdbError &error) {
            job.error = error.message();
        }
        return job;
    });
}

void PermissionsModel::undo(int id) {
    const QString serial = m_monitor ? m_monitor->serial() : QString();
    const QString package = m_app.package;
    start([serial, package, id] {
        Job job;
        try {
            Adb adb(serial);
            adb.resolveSerial();
            perms::Changer changer(adb, hardwareSerial(adb));
            const QJsonObject entry = changer.undo(id);
            job.results.append(entry);
            const QString which = package.isEmpty() ? entry.value("package").toString() : package;
            if (!which.isEmpty()) {
                job.app = changer.read(which);
                job.hasApp = true;
            }
            job.journal = changer.journal();
        } catch (const AdbError &error) {
            job.error = error.message();
        }
        return job;
    });
}

void PermissionsModel::refreshJournal() {
    const QString serial = m_monitor ? m_monitor->serial() : QString();
    start([serial] {
        Job job;
        try {
            Adb adb(serial);
            adb.resolveSerial();
            job.journal = perms::Changer(adb, hardwareSerial(adb)).journal();
        } catch (const AdbError &error) {
            job.error = error.message();
        }
        return job;
    });
}

void PermissionsModel::loadPreset(const QString &id) {
    m_presetId = id;
    m_steps.clear();
    m_presetSteps.clear();
    if (m_monitor) {
        m_steps = perms::preset(id, m_monitor->lastCapture(), QJsonArray::fromVariantList(m_monitor->findings()));
        for (const perms::Step &step : m_steps)
            m_presetSteps.append(QVariantMap{{"package", step.package}, {"control", step.control}, {"to", step.to},
                                             {"title", step.title}, {"reason", step.reason}});
    }
    emit presetChanged();
}

void PermissionsModel::applyPreset(const QVariantList &indexes) {
    const QString serial = m_monitor ? m_monitor->serial() : QString();
    QList<perms::Step> chosen;
    for (const QVariant &index : indexes)
        if (index.toInt() >= 0 && index.toInt() < m_steps.size())
            chosen.append(m_steps.at(index.toInt()));
    start([serial, chosen] {
        Job job;
        try {
            Adb adb(serial);
            adb.resolveSerial();
            perms::Changer changer(adb, hardwareSerial(adb));
            for (const perms::Step &step : chosen) {
                const perms::Plan plan = perms::plan(changer.read(step.package), step.control, step.to);
                if (plan.blocked || plan.commands.isEmpty())
                    continue;
                job.results.append(changer.apply(plan));
            }
            job.journal = changer.journal();
        } catch (const AdbError &error) {
            job.error = error.message();
        }
        return job;
    });
}

void PermissionsModel::loadReview(bool askAll) {
    m_questions.clear();
    if (m_monitor && !m_monitor->phone().isEmpty()) {
        perms::Decisions decisions(m_monitor->phone());
        if (askAll)
            decisions.forget();
        for (const perms::Question &q : perms::review(m_monitor->lastCapture(),
                                                      QJsonArray::fromVariantList(m_monitor->findings()), decisions.kept())) {
            QVariantList options;
            for (const perms::Option &option : q.options)
                options.append(QVariantMap{{"state", option.state}, {"label", option.label}});
            m_questions.append(QVariantMap{{"package", q.package}, {"name", friendlyName(q.package)}, {"control", q.control},
                                           {"title", q.title}, {"why", q.why}, {"suggested", q.suggested},
                                           {"options", options}, {"round", q.round}});
        }
    }
    emit questionsChanged();
}

void PermissionsModel::answer(int index, const QString &state) {
    if (index < 0 || index >= m_questions.size() || m_watcher.isRunning())
        return;
    const QVariantMap q = m_questions.at(index).toMap();
    if (state == "keep") {
        if (m_monitor)
            perms::Decisions(m_monitor->phone()).keep(perms::questionKey(q.value("package").toString(), q.value("control").toString()));
        emit answered(index, QVariantMap{{"kept", true}});
        return;
    }
    m_answering = index;
    const QString serial = m_monitor ? m_monitor->serial() : QString();
    const QString package = q.value("package").toString(), control = q.value("control").toString();
    start([serial, package, control, state] {
        Job job;
        try {
            Adb adb(serial);
            adb.resolveSerial();
            perms::Changer changer(adb, hardwareSerial(adb));
            job.results.append(changer.apply(perms::plan(changer.read(package), control, state)));
            job.journal = changer.journal();
        } catch (const AdbError &error) {
            job.error = error.message();
        }
        return job;
    });
}

void PermissionsModel::finished() {
    const Job job = m_watcher.result();
    if (job.hasApp)
        m_app = job.app;
    m_error = job.error;
    m_journal = job.journal.toVariantList();
    // The panel reports the last change made; a preset, how many held.
    if (job.results.size() == 1) {
        m_lastResult = job.results.first().toObject().toVariantMap();
    } else if (job.results.size() > 1) {
        int held = 0;
        for (const QJsonValue &result : job.results)
            held += result.toObject().value("verified").toBool() ? 1 : 0;
        m_lastResult = QVariantMap{{"preset", true}, {"applied", int(job.results.size())}, {"held", held}};
    }
    emit appChanged();
    emit journalChanged();
    emit resultChanged();
    emit busyChanged();
    if (m_answering >= 0) {
        const int index = m_answering;
        m_answering = -1;
        QVariantMap result = job.results.isEmpty() ? QVariantMap() : job.results.first().toObject().toVariantMap();
        if (!job.error.isEmpty())
            result.insert("output", job.error);
        emit answered(index, result);
    }
}
