/*
 * qucscontrol_watch.cpp - Claude's tools for what happens while it waits:
 *                         simulations followed by an id after their call
 *                         has answered (simulate's 'background', or a run
 *                         past its timeout) - their state, their outcome,
 *                         stopped -, and wait_for: a run's end, a dialog,
 *                         a document's change or a file written, waited
 *                         for without asking again and again
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"
#include "dialogs/simmessage.h"
#include "extsimkernels/simulationrun.h"
#include "qucs.h"
#include "simulationconsole.h"

#include <QAction>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <optional>

using namespace qucs_s::control;

namespace {

// The last \a count lines of \a text.
QString tailOf(const QString& text, int count)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    return lines.mid(std::max<qsizetype>(0, lines.size() - count)).join(QLatin1Char('\n'));
}

// A text answer put before an answer of a tool's.
QJsonObject withText(const QString& text, const QJsonObject& result)
{
    QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}};
    for (const QJsonValue& v : result.value(QLatin1String("content")).toArray()) content.append(v);
    return {{QStringLiteral("content"), content}, {QStringLiteral("isError"), result.value(QLatin1String("isError")).toBool()}};
}

} // namespace

// ----------------------------------------------------------------------
// Simulations followed by an id

int QucsControl::beginSimRun(const QString& schematic, const QString& simulator)
{
    SimRun run;
    run.id = ++a_simRunSerial;
    run.schematic = schematic;
    run.simulator = simulator;
    run.began = QDateTime::currentDateTime();
    a_simRuns.append(run);
    // (The last ones: an outcome is read soon after.)
    while (a_simRuns.size() > 20 && a_simRuns.first().ended.isValid()) a_simRuns.removeFirst();
    return run.id;
}

QucsControl::SimRun* QucsControl::simRun(int id)
{
    for (SimRun& r : a_simRuns)
        if (r.id == id) return &r;
    return nullptr;
}

void QucsControl::endSimRun(int id, const QJsonObject& result)
{
    SimRun* run = simRun(id);
    if (run == nullptr) return;
    run->ended = QDateTime::currentDateTime();
    run->result = result;
    run->process.clear();
    // Told in the next answer of every conversation (the one that began it
    // may be waiting for something else).
    noteForConversations(tr("Simulation %1 (%2, %3) has ended%4: simulation_status {\"id\": %1} gives its outcome.")
                             .arg(id).arg(QFileInfo(run->schematic).fileName(), run->simulator,
                                          run->stoppedBy.isEmpty() ? QString() : tr(", stopped by %1").arg(run->stoppedBy)));
}

void QucsControl::stopProcess(QObject* process)
{
    if (auto* run = qobject_cast<SimulationRun*>(process)) run->stop();
    else if (auto* sim = qobject_cast<SimMessage*>(process)) QMetaObject::invokeMethod(sim, "AbortSim", Qt::DirectConnection);
}

QJsonObject QucsControl::simulationStatus(const QJsonObject& args)
{
    const QDateTime now = QDateTime::currentDateTime();
    const auto stateOf = [&now](const SimRun& r) {
        QJsonObject o{{QStringLiteral("id"), r.id},
                      {QStringLiteral("schematic"), QDir::toNativeSeparators(r.schematic)},
                      {QStringLiteral("simulator"), r.simulator},
                      {QStringLiteral("began"), r.began.toString(Qt::ISODate)},
                      {QStringLiteral("state"), r.ended.isValid() ? QStringLiteral("ended") : QStringLiteral("running")},
                      {QStringLiteral("seconds"), double(r.began.msecsTo(r.ended.isValid() ? r.ended : now)) / 1000.0}};
        if (r.ended.isValid()) {
            o.insert(QStringLiteral("ended"), r.ended.toString(Qt::ISODate));
            const QJsonArray content = r.result.value(QLatin1String("content")).toArray();
            const QJsonObject report = content.isEmpty() ? QJsonObject()
                                                         : QJsonDocument::fromJson(content.at(0).toObject().value(QLatin1String("text")).toString().toUtf8()).object();
            if (report.contains(QLatin1String("succeeded"))) o.insert(QStringLiteral("succeeded"), report.value(QLatin1String("succeeded")));
            else if (r.result.value(QLatin1String("isError")).toBool()) o.insert(QStringLiteral("succeeded"), false);
        }
        if (!r.stoppedBy.isEmpty()) o.insert(QStringLiteral("stopped by"), r.stoppedBy);
        return o;
    };
    if (!args.contains(QLatin1String("id"))) {
        QJsonArray runs;
        for (auto it = a_simRuns.crbegin(); it != a_simRuns.crend(); ++it) runs.append(stateOf(*it));
        return jsonResult(QJsonObject{{QStringLiteral("runs"), runs},
                                      {QStringLiteral("note"), runs.isEmpty() ? tr("None is followed: simulate with 'background' begins one.")
                                                                              : tr("simulation_status with an 'id' gives a run's outcome.")}});
    }
    const int id = args.value(QLatin1String("id")).toInt();
    SimRun* run = simRun(id);
    if (run == nullptr) {
        QStringList ids;
        for (const SimRun& r : std::as_const(a_simRuns)) ids << QString::number(r.id);
        return errorResult(tr("There is no simulation %1 followed (%2).").arg(id).arg(ids.isEmpty() ? tr("none is") : tr("these are: %1").arg(ids.join(QStringLiteral(", ")))));
    }
    if (run->ended.isValid())
        return withText(tr("Simulation %1 (%2, %3) ended at %4, after %5 s%6. Its outcome, as simulate gives it:")
                            .arg(id).arg(QFileInfo(run->schematic).fileName(), run->simulator, run->ended.toString(QStringLiteral("HH:mm:ss")))
                            .arg(run->began.secsTo(run->ended))
                            .arg(run->stoppedBy.isEmpty() ? QString() : tr(" - stopped by %1").arg(run->stoppedBy)),
                        run->result);
    // Running: for how long, and what it printed last.
    QJsonObject o = stateOf(*run);
    QString output;
    if (auto* sim = qobject_cast<SimMessage*>(run->process.data())) output = sim->ProgText->toPlainText();
    else if (a_app->simulationConsole() != nullptr) output = a_app->simulationConsole()->console()->toPlainText();
    if (!output.trimmed().isEmpty()) o.insert(QStringLiteral("last lines"), tailOf(output, 12));
    o.insert(QStringLiteral("note"), tr("wait_for {\"event\": \"simulation_finished\", \"id\": %1} waits for its end; stop_simulation stops it.").arg(id));
    return jsonResult(o);
}

namespace {

// A simulation running now - the console's, or Qucsator's window.
bool simulationRunning(QucsApp* app)
{
    if (app->simulationConsole() != nullptr && app->simulationConsole()->isRunning()) return true;
    for (SimMessage* m : app->findChildren<SimMessage*>())
        if (m->SimProcess.state() != QProcess::NotRunning) return true;
    return false;
}

} // namespace

void QucsControl::stopSimulation(const QJsonObject& args, const Done& done)
{
    SimRun* run = nullptr;
    if (args.contains(QLatin1String("id"))) {
        const int id = args.value(QLatin1String("id")).toInt();
        run = simRun(id);
        if (run == nullptr) {
            done(errorResult(tr("There is no simulation %1 followed (simulation_status lists them).").arg(id)));
            return;
        }
        if (run->ended.isValid()) {
            done(textResult(tr("Simulation %1 had ended already, at %2: simulation_status {\"id\": %1} gives its outcome.")
                                .arg(id).arg(run->ended.toString(QStringLiteral("HH:mm:ss")))));
            return;
        }
    } else {
        // The one running - followed, or not (the user's).
        for (SimRun& r : a_simRuns)
            if (!r.ended.isValid()) run = &r;
        if (run == nullptr && !simulationRunning(a_app)) {
            done(errorResult(tr("No simulation is running.")));
            return;
        }
    }
    const int id = run != nullptr ? run->id : 0;
    QPointer<QObject> process = run != nullptr ? run->process : QPointer<QObject>();
    if (run != nullptr) run->stoppedBy = QStringLiteral("stop_simulation");
    // Its own process; else what runs now (it began a moment ago, or is the
    // user's): Simulation > Stop Simulation, Qucsator's Abort.
    if (process) {
        stopProcess(process);
    } else {
        if (SimulationConsole* console = a_app->simulationConsole(); console != nullptr && console->isRunning()) console->stopAction()->trigger();
        for (SimMessage* m : a_app->findChildren<SimMessage*>())
            if (m->SimProcess.state() != QProcess::NotRunning) stopProcess(m);
    }
    // Its end, told (a moment: the simulator is ended, its output read).
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto* watch = new QTimer(this);
    watch->setInterval(100);
    connect(watch, &QTimer::timeout, this, [this, watch, clock, id, done] {
        SimRun* r = id != 0 ? simRun(id) : nullptr;
        const bool over = id != 0 ? (r == nullptr || r->ended.isValid()) : !simulationRunning(a_app);
        if (!over && clock->elapsed() < 15000) return;
        watch->stop();
        watch->deleteLater();
        if (!over) {
            done(textResult(tr("Asked to stop; it has not ended yet after 15 s%1.")
                                .arg(id != 0 ? tr(": simulation_status {\"id\": %1} tells when it has").arg(id) : QString())));
            return;
        }
        if (r != nullptr && r->ended.isValid()) {
            done(withText(tr("Simulation %1 stopped. Its outcome, as simulate gives it:").arg(id), r->result));
            return;
        }
        QString output;
        if (a_app->simulationConsole() != nullptr) output = a_app->simulationConsole()->console()->toPlainText();
        done(jsonResult(QJsonObject{{QStringLiteral("stopped"), true}, {QStringLiteral("last lines"), tailOf(output, 12)}}));
    });
    watch->start();
}

// ----------------------------------------------------------------------
// wait_for

void QucsControl::waitFor(const QJsonObject& args, const Done& done)
{
    const QString event = args.value(QLatin1String("event")).toString().trimmed().toLower();
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(60), 1, 3600);
    const quint64 caller = a_callers.isEmpty() ? 0 : a_callers.last();
    // What has happened, once it has (an answer); nothing until then.
    std::function<std::optional<QJsonObject>()> happened;
    QString what;
    if (event == QLatin1String("simulation_finished")) {
        if (args.contains(QLatin1String("id"))) {
            // A run followed by its id: its outcome, as simulate gives it.
            const int id = args.value(QLatin1String("id")).toInt();
            if (simRun(id) == nullptr) {
                done(errorResult(tr("There is no simulation %1 followed (simulation_status lists them).").arg(id)));
                return;
            }
            what = tr("simulation %1's end").arg(id);
            happened = [this, id]() -> std::optional<QJsonObject> {
                if (SimRun* r = simRun(id); r != nullptr && !r->ended.isValid()) return std::nullopt;
                return simulationStatus(QJsonObject{{QStringLiteral("id"), id}});
            };
        } else {
            // The run going now, or the next to begin - the user's too.
            what = tr("a simulation's end");
            struct Seen {
                bool running = false;
                bool wasSimulated = false, hasError = false, stopped = false, known = false;
                int exitCode = 0;
                QPointer<SimulationRun> run;
            };
            auto seen = std::make_shared<Seen>();
            happened = [this, seen]() -> std::optional<QJsonObject> {
                if (simulationRunning(a_app)) {
                    seen->running = true;
                    SimulationConsole* console = a_app->simulationConsole();
                    if (SimulationRun* run = console != nullptr ? console->currentRun() : nullptr; run != nullptr && seen->run != run) {
                        seen->run = run;
                        // (Freed once it has ended: what it says, kept then.)
                        connect(run, &SimulationRun::simulated, this, [seen](SimulationRun* r) {
                            seen->known = true;
                            seen->wasSimulated = r->wasSimulated();
                            seen->hasError = r->hasError();
                            seen->stopped = r->wasStopped();
                            seen->exitCode = r->exitCode();
                        });
                    }
                    return std::nullopt;
                }
                if (!seen->running) return std::nullopt;
                QJsonObject o{{QStringLiteral("event"), QStringLiteral("simulation_finished")}, {QStringLiteral("happened"), true}};
                if (seen->known) {
                    o.insert(QStringLiteral("succeeded"), seen->wasSimulated && !seen->hasError && seen->exitCode == 0);
                    o.insert(QStringLiteral("stopped"), seen->stopped);
                    o.insert(QStringLiteral("exit code"), seen->exitCode);
                }
                if (a_app->simulationConsole() != nullptr)
                    o.insert(QStringLiteral("last lines"), tailOf(a_app->simulationConsole()->console()->toPlainText(), 12));
                o.insert(QStringLiteral("note"), tr("get_dataset reads its results; check_schematic and the last lines tell why one failed."));
                return o;
            };
        }
    } else if (event == QLatin1String("dialog")) {
        what = tr("a dialog");
        happened = [this]() -> std::optional<QJsonObject> {
            QWidget* dialog = openDialog();
            if (dialog == nullptr) return std::nullopt;
            return QJsonObject{{QStringLiteral("event"), QStringLiteral("dialog")},
                               {QStringLiteral("happened"), true},
                               {QStringLiteral("dialog"), dialog->windowTitle().isEmpty() ? QString::fromLatin1(dialog->metaObject()->className())
                                                                                         : dialog->windowTitle()},
                               {QStringLiteral("note"), tr("get_dialog reads it; set_dialog answers it.")}};
        };
    } else if (event == QLatin1String("document_changed")) {
        QString error;
        QucsDoc* doc = document(args, &error);
        if (doc == nullptr) {
            done(errorResult(error));
            return;
        }
        // Changed since 'revision' (from an answer before: no change missed
        // in between), else since now.
        const quint64 since = args.contains(QLatin1String("revision")) ? quint64(args.value(QLatin1String("revision")).toDouble())
                                                                        : doc->revision();
        const QString title = titleOf(doc);
        QPointer<QWidget> widget(QucsApp::documentWidget(doc));
        what = tr("a change of %1").arg(title);
        happened = [this, widget, since, title, caller]() -> std::optional<QJsonObject> {
            QucsDoc* d = widget ? QucsApp::docIn(widget) : nullptr;
            if (d == nullptr)
                return QJsonObject{{QStringLiteral("event"), QStringLiteral("document_changed")}, {QStringLiteral("happened"), true},
                                   {QStringLiteral("document"), title}, {QStringLiteral("closed"), true}};
            if (d->revision() == since) return std::nullopt;
            QJsonObject o{{QStringLiteral("event"), QStringLiteral("document_changed")},
                          {QStringLiteral("happened"), true},
                          {QStringLiteral("document"), titleOf(d)},
                          {QStringLiteral("path"), QDir::toNativeSeparators(d->getDocName())},
                          {QStringLiteral("revision"), double(d->revision())},
                          {QStringLiteral("was"), double(since)},
                          {QStringLiteral("unsaved changes"), d->getDocChanged()}};
            QStringList by;
            for (const QucsDoc::Edit& e : d->recentEdits())
                if (e.revision > since && !by.contains(whoMade(e.by, caller))) by << whoMade(e.by, caller);
            if (!by.isEmpty()) o.insert(QStringLiteral("by"), by.join(QStringLiteral(", ")));
            return o;
        };
    } else if (event == QLatin1String("file_written")) {
        const QString given = args.value(QLatin1String("path")).toString().trimmed();
        if (given.isEmpty()) {
            done(errorResult(tr("'path' is the file waited for (a dataset, a netlist, an export).")));
            return;
        }
        const QString file = absolute(given);
        struct Stamp {
            bool there = false;
            QDateTime modified;
            qint64 size = -1;
            bool operator==(const Stamp& o) const { return there == o.there && modified == o.modified && size == o.size; }
        };
        const auto stampOf = [file] {
            const QFileInfo info(file);
            return info.exists() ? Stamp{true, info.lastModified(), info.size()} : Stamp{};
        };
        struct Watch {
            Stamp first, last;
            QElapsedTimer still;   // since it last changed
            bool changed = false;
        };
        auto w = std::make_shared<Watch>();
        w->first = w->last = stampOf();
        what = tr("%1 written").arg(QDir::toNativeSeparators(file));
        happened = [w, stampOf, file]() -> std::optional<QJsonObject> {
            const Stamp now = stampOf();
            if (!(now == w->last)) {
                w->last = now;
                w->changed = true;
                w->still.start();
                return std::nullopt;
            }
            // Written - and nothing more for a moment (not read half-written).
            if (!w->changed || w->still.elapsed() < 400) return std::nullopt;
            if (now == w->first) {
                w->changed = false;   // (back as it was: not written)
                return std::nullopt;
            }
            QJsonObject o{{QStringLiteral("event"), QStringLiteral("file_written")}, {QStringLiteral("happened"), true},
                          {QStringLiteral("path"), QDir::toNativeSeparators(file)}};
            if (!now.there) {
                o.insert(QStringLiteral("removed"), true);
            } else {
                o.insert(QStringLiteral("size"), now.size);
                o.insert(QStringLiteral("modified"), now.modified.toString(Qt::ISODate));
                if (!w->first.there) o.insert(QStringLiteral("made"), true);
            }
            return o;
        };
    } else {
        done(errorResult(tr("'event' is simulation_finished, dialog, document_changed or file_written.")));
        return;
    }
    // Now already (a dialog open, a run followed that has ended).
    if (const auto now = happened()) {
        done(now->contains(QLatin1String("content")) ? *now : jsonResult(*now));
        return;
    }
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto* watch = new QTimer(this);
    watch->setInterval(100);
    connect(watch, &QTimer::timeout, this, [watch, clock, happened, done, what, event, timeout] {
        std::optional<QJsonObject> now = happened();
        if (!now && clock->elapsed() < qint64(timeout) * 1000) return;
        watch->stop();
        watch->deleteLater();
        if (!now) {
            done(jsonResult(QJsonObject{{QStringLiteral("event"), event},
                                        {QStringLiteral("happened"), false},
                                        {QStringLiteral("waited"), timeout},
                                        {QStringLiteral("note"), tr("Not yet after %1 s (waited for %2): wait_for again to wait on.").arg(timeout).arg(what)}}));
            return;
        }
        QJsonObject result = now->contains(QLatin1String("content")) ? *now : jsonResult(*now);
        done(result);
    });
    watch->start();
}
