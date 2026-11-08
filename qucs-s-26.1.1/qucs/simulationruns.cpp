/*
 * simulationruns.cpp - every run, the user's and Claude's, in the one list of
 *                      jobs; documents kept running without their tab; the
 *                      tabs closed lately
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "simulationruns.h"

#include "claudejobs.h"
#include "dialogs/simmessage.h"
#include "extsimkernels/simulationrun.h"
#include "main.h"
#include "misc.h"
#include "multiphysics3ddoc.h"
#include "multiphysicsdoc.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "rf25doc.h"
#include "schematic.h"
#include "simulationconsole.h"

#include <QDir>
#include <QFileInfo>
#include <QProgressBar>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace jobs = qucs_s::claude::jobs;

namespace qucs_s::runs {

namespace {

// As a model's own bar shows it: nought is nothing said (a bar at nought for a
// whole run would claim it did nothing).
int percentOfFraction(double fraction)
{
    return fraction > 0 ? std::clamp(int(std::floor(100 * fraction)), 0, 100) : -1;
}

QString tr(const char* text)
{
    return QCoreApplication::translate("SimulationRuns", text);
}

QString simulatorName()
{
    switch (QucsSettings.DefaultSimulator) {
    case spicecompat::simNgspice: return QStringLiteral("ngspice");
    case spicecompat::simXyce: return QStringLiteral("Xyce");
    case spicecompat::simSpiceOpus: return QStringLiteral("SPICE OPUS");
    case spicecompat::simQucsator: return QStringLiteral("Qucsator");
    default: return tr("the simulator");
    }
}

} // namespace

Activity activityOf(QucsDoc* doc)
{
    Activity a;
    if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) {
        // A job (a mesh, a solve, an optimization, a convergence check), or a
        // process flow's geometry built in the background for one asked for.
        // (Built again after an edit, or for a step shown, it is no run: closed,
        // nothing asked for is lost.)
        const double fraction = model->jobFraction();
        a.busy = model->isBusy() || model->buildAwaited();
        if (!a.busy) return a;
        a.listed = model->isBusy();
        a.percent = percentOfFraction(fraction);
        a.said = model->jobProgress();
        a.whyNoPercent = tr("The model has not said how far it has got yet.");
        if (const QString tag = model->optimizing(); !tag.isEmpty()) {
            a.kind = QStringLiteral("optimization");
            a.what = tr("optimization %1").arg(tag);
        } else if (const QString check = model->converging(); !check.isEmpty()) {
            a.kind = QStringLiteral("convergence");
            a.what = tr("mesh convergence %1").arg(check);
        } else if (const QString study = model->computing(); !study.isEmpty()) {
            a.kind = QStringLiteral("solve");
            a.what = tr("study %1").arg(study);
        } else if (model->meshBeingMade()) {
            a.kind = QStringLiteral("mesh");
            a.what = tr("mesh");
        } else {
            a.kind = QStringLiteral("build");
            a.what = tr("geometry, for what was asked");
        }
        return a;
    }
    if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) {
        // A solve, or a mesh asked for. (Its geometry made again after an edit
        // is no run: closed, nothing asked for is lost.)
        a.busy = model3->solving() || model3->meshing();
        if (!a.busy) return a;
        a.listed = true;
        a.kind = model3->solving() ? QStringLiteral("solve") : QStringLiteral("mesh");
        a.what = model3->solving() ? tr("solve") : tr("mesh");
        a.percent = percentOfFraction(model3->jobFraction());
        a.said = model3->jobProgress();
        a.whyNoPercent = tr("The model has not said how far it has got yet.");
        return a;
    }
    if (auto* rf = dynamic_cast<RF25Doc*>(doc)) {
        // (Its shapes and mesh, made again at every edit in a moment, are no
        // run: the solve is.)
        a.busy = rf->isSolving();
        if (!a.busy) return a;
        a.listed = true;
        a.kind = QStringLiteral("solve");
        a.what = rf->studyIsFullWave() ? tr("full-wave sweep") : tr("quasi-static solve");
        int done = 0, of = 0;
        const QString count = rf->solveCount(&done, &of);
        a.percent = of > 0 && done > 0 ? std::clamp(100 * done / of, 0, 100) : -1;
        a.said = rf->solvingSaid().isEmpty() ? count : rf->solvingSaid();
        a.whyNoPercent = tr("This step cannot be counted: a matrix being filled and factored says nothing until it is done.");
        return a;
    }
    return a;
}

bool stopActivity(QucsDoc* doc)
{
    if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) {
        if (!model->isBusy() && !model->buildAwaited()) return false;
        model->cancelJob();
        return true;
    }
    if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) {
        if (!model3->solving() && !model3->meshing()) return false;
        model3->cancel();
        return true;
    }
    if (auto* rf = dynamic_cast<RF25Doc*>(doc)) {
        if (!rf->isSolving()) return false;
        rf->stopSolving();
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------
Tracker::Tracker(QucsApp* app) : QObject(app), a_app(app)
{
    // (In the main window, never shown: a view drawn with the GPU stays in
    // the window it was made in.)
    a_holder = new QWidget(app);
    a_holder->setObjectName(QStringLiteral("keptRunningDocuments"));
    a_holder->hide();
    a_poll = new QTimer(this);
    a_poll->setInterval(kPollEvery);
    connect(a_poll, &QTimer::timeout, this, &Tracker::pollNow);
    a_poll->start();
    a_soon = new QTimer(this);
    a_soon->setSingleShot(true);
    a_soon->setInterval(0);
    connect(a_soon, &QTimer::timeout, this, &Tracker::pollNow);
}

Tracker::~Tracker()
{
    // (Nothing left to say how they end: the window goes.)
    for (const Run& run : std::as_const(a_runs))
        if (!run.job.isEmpty() && !run.claudes)
            jobs::Registry::instance().update(run.job, [](jobs::Job& job) {
                job.status = jobs::Status::Lost;
                job.said = tr("Qucs-S closed while it ran.");
            });
}

QList<QucsDoc*> Tracker::documents() const
{
    QList<QucsDoc*> docs = a_app->allDocuments();
    for (const QPointer<QWidget>& w : a_parked)
        if (QucsDoc* doc = w ? dynamic_cast<QucsDoc*>(w.data()) : nullptr) docs << doc;
    return docs;
}

Tracker::Run* Tracker::runOf(const QObject* document)
{
    for (Run& run : a_runs)
        if (run.document == document) return &run;
    return nullptr;
}

const Tracker::Run* Tracker::runOf(const QObject* document) const
{
    for (const Run& run : a_runs)
        if (run.document == document) return &run;
    return nullptr;
}

QString Tracker::titleOf(QObject* document) const
{
    auto* doc = dynamic_cast<QucsDoc*>(document);
    if (doc == nullptr) return tr("a document");
    const QString name = doc->getDocName();
    return name.isEmpty() ? tr("untitled") : QFileInfo(name).fileName();
}

Activity Tracker::activity(QucsDoc* doc) const
{
    if (doc == nullptr) return {};
    auto* object = dynamic_cast<QObject*>(doc);
    if (const Run* run = runOf(object); run != nullptr && (run->spiceRun || run->qucsatorRun)) {
        Activity a;
        a.busy = run->spiceRun ? run->spice && run->spice->isRunning()
                               : run->qucsator && run->qucsator->SimProcess.state() != QProcess::NotRunning;
        a.listed = true;
        a.kind = QStringLiteral("simulation");
        a.what = run->last.what;
        a.whyNoPercent = run->last.whyNoPercent;
        if (run->spiceRun) {
            SimulationConsole* console = a_app->simulationConsole();
            if (console != nullptr && console->currentRun() == run->spice && console->percent() > 0) a.percent = console->percent();
        } else if (run->qucsator && run->qucsator->SimProgress->value() > 0) {
            a.percent = run->qucsator->SimProgress->value();
        }
        return a;
    }
    return activityOf(doc);
}

bool Tracker::stop(QucsDoc* doc)
{
    return stopRun(dynamic_cast<QObject*>(doc));
}

bool Tracker::stopRun(QObject* document)
{
    Run* run = runOf(document);
    if (run != nullptr) run->stopAsked = true;
    if (run != nullptr && run->spiceRun) {
        if (!run->spice || !run->spice->isRunning()) return false;
        run->spice->stop();
        return true;
    }
    if (run != nullptr && run->qucsatorRun) {
        if (!run->qucsator || run->qucsator->SimProcess.state() == QProcess::NotRunning) return false;
        // (Its Abort button's own slot.)
        return QMetaObject::invokeMethod(run->qucsator, "AbortSim");
    }
    return stopActivity(dynamic_cast<QucsDoc*>(document));
}

QStringList Tracker::runningNow() const
{
    QStringList lines;
    QSet<const QObject*> said;
    for (QucsDoc* doc : documents()) {
        const Activity a = activity(doc);
        if (!a.busy) continue;
        said.insert(dynamic_cast<QObject*>(doc));
        QString line = a.what.isEmpty() ? titleOf(dynamic_cast<QObject*>(doc))
                                        : QStringLiteral("%1 - %2").arg(titleOf(dynamic_cast<QObject*>(doc)), a.what);
        if (a.percent >= 0) line += QStringLiteral(", %1 %").arg(a.percent);
        if (isParked(dynamic_cast<QObject*>(doc))) line += QLatin1Char(' ') + tr("(its tab closed)");
        lines << line;
    }
    // A schematic's run whose schematic is gone: it goes on all the same.
    for (const Run& run : a_runs)
        if (!run.document && ((run.spiceRun && run.spice && run.spice->isRunning())
                              || (run.qucsatorRun && run.qucsator && run.qucsator->SimProcess.state() != QProcess::NotRunning)))
            lines << QStringLiteral("%1 - %2").arg(QFileInfo(run.file).fileName(), run.last.what);
    return lines;
}

// ----------------------------------------------------------------------
void Tracker::watchSpiceRun(SimulationRun* run, Schematic* schematic)
{
    if (run == nullptr || schematic == nullptr || a_app->TuningMode) return;   // (a tuner's step is no run of its own)
    QObject* document = schematic;
    if (runOf(document) != nullptr) end(document, int(jobs::Status::Lost), tr("Another run of it began."));
    Run r;
    r.document = document;
    r.file = schematic->getDocName();
    r.spice = run;
    r.spiceRun = true;
    r.age.start();
    r.last.kind = QStringLiteral("simulation");
    r.last.what = tr("simulation (%1)").arg(simulatorName());
    r.last.listed = true;
    r.last.busy = true;
    r.last.whyNoPercent = tr("The simulator has said no percentage yet: this run may have none to say (a DC operating point, a build "
                             "that prints none).");
    if (a_claude) r.job = a_claude(document, &r.caller);
    r.claudes = !r.job.isEmpty();
    a_runs.append(r);
    if (r.claudes) {
        const QString file = r.file;
        jobs::Registry::instance().update(r.job, [file](jobs::Job& job) { job.document = file; });
        rememberJob(a_runs.last());
    } else {
        list(a_runs.last());
    }
    connect(run, &SimulationRun::simulated, this, [this](SimulationRun* r) {
        // (Found by the run: its schematic may be gone.)
        for (qsizetype i = 0; i < a_runs.size(); ++i) {
            if (!a_runs.at(i).spiceRun || a_runs.at(i).spice != r) continue;
            const int status = r->wasStopped() ? int(jobs::Status::Stopped)
                               : r->hasError() || !r->wasSimulated() ? int(jobs::Status::Failed)
                                                                      : int(jobs::Status::Finished);
            QString outcome;
            if (!r->datasetError().isEmpty()) outcome = r->datasetError();
            else if (r->warningCount() > 0) outcome = tr("%n warning(s)", nullptr, r->warningCount());
            const Run ended = a_runs.takeAt(i);
            if (!ended.job.isEmpty() && !ended.claudes)
                jobs::Registry::instance().update(ended.job, [status, outcome](jobs::Job& job) {
                    job.status = jobs::Status(status);
                    if (!outcome.isEmpty()) job.outcome = jobs::oneLine(outcome, 400);
                    if (job.status == jobs::Status::Finished) job.percent = 100;
                });
            emit changed();
            return;
        }
    });
    // Deleted before its end was seen (the window closing): lost.
    connect(run, &QObject::destroyed, this, [this] {
        for (qsizetype i = a_runs.size() - 1; i >= 0; --i) {
            if (!a_runs.at(i).spiceRun || !a_runs.at(i).spice.isNull()) continue;
            const Run ended = a_runs.takeAt(i);
            if (!ended.job.isEmpty() && !ended.claudes)
                jobs::Registry::instance().update(ended.job, [](jobs::Job& job) {
                    job.status = jobs::Status::Lost;
                    job.said = tr("It went before its end was seen.");
                });
            emit changed();
        }
    });
    emit changed();
}

void Tracker::watchQucsatorRun(SimMessage* sim, QWidget* document)
{
    if (sim == nullptr || document == nullptr || a_app->TuningMode) return;
    if (runOf(document) != nullptr) end(document, int(jobs::Status::Lost), tr("Another run of it began."));
    Run r;
    r.document = document;
    if (auto* doc = dynamic_cast<QucsDoc*>(document)) r.file = doc->getDocName();
    r.qucsator = sim;
    r.qucsatorRun = true;
    r.age.start();
    r.last.kind = QStringLiteral("simulation");
    r.last.what = tr("simulation (%1)").arg(QStringLiteral("Qucsator"));
    r.last.listed = true;
    r.last.busy = true;
    r.last.whyNoPercent = tr("Qucsator has said no percentage yet.");
    if (a_claude) r.job = a_claude(document, &r.caller);
    r.claudes = !r.job.isEmpty();
    a_runs.append(r);
    if (r.claudes) {
        const QString file = r.file;
        jobs::Registry::instance().update(r.job, [file](jobs::Job& job) { job.document = file; });
        rememberJob(a_runs.last());
    } else {
        list(a_runs.last());
    }
    connect(sim, &SimMessage::SimulationEnded, this, [this](int status, SimMessage* m) {
        for (qsizetype i = 0; i < a_runs.size(); ++i)
            if (a_runs.at(i).qucsatorRun && a_runs.at(i).qucsator == m) {
                Run ended = a_runs.takeAt(i);
                const int to = m->simKilled ? int(jobs::Status::Stopped) : status == 0 ? int(jobs::Status::Finished) : int(jobs::Status::Failed);
                if (!ended.job.isEmpty() && !ended.claudes)
                    jobs::Registry::instance().update(ended.job, [to](jobs::Job& job) {
                        job.status = jobs::Status(to);
                        if (job.status == jobs::Status::Finished) job.percent = 100;
                    });
                emit changed();
                return;
            }
    });
    // (Closed before it ended - the application going -: no end will come.)
    connect(sim, &QObject::destroyed, this, [this] {
        for (qsizetype i = a_runs.size() - 1; i >= 0; --i) {
            if (!a_runs.at(i).qucsatorRun || !a_runs.at(i).qucsator.isNull()) continue;
            const Run ended = a_runs.takeAt(i);
            if (!ended.job.isEmpty() && !ended.claudes)
                jobs::Registry::instance().update(ended.job, [](jobs::Job& job) {
                    job.status = jobs::Status::Lost;
                    job.said = tr("Its window was closed before its end was seen.");
                });
            emit changed();
        }
    });
    emit changed();
}

// ----------------------------------------------------------------------
void Tracker::begin(QucsDoc* doc, const Activity& now)
{
    Run r;
    r.document = dynamic_cast<QObject*>(doc);
    r.file = doc->getDocName();
    r.age.start();
    r.last = now;
    if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) r.stopsBefore = model->stops();
    else if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) r.stopsBefore = model3->stops();
    a_runs.append(r);
    Run& run = a_runs.last();
    if (now.listed) list(run);
    emit changed();
}

void Tracker::list(Run& run)
{
    if (!run.job.isEmpty()) return;
    // A run Claude's tool began is that tool's job, which follows it to its end.
    if (a_claude && !run.claudes && run.caller == 0) {
        run.job = a_claude(run.document.data(), &run.caller);
        run.claudes = !run.job.isEmpty();
    }
    QString file = run.file;
    if (auto* doc = dynamic_cast<QucsDoc*>(run.document.data())) file = doc->getDocName();
    if (run.claudes) {
        jobs::Registry::instance().update(run.job, [file](jobs::Job& job) { job.document = file; });
        rememberJob(run);
        return;
    }
    jobs::Job job;
    // (Begun by a call of Claude's that is no job of a run - a menu action it
    // triggered -: its conversation's, all the same.)
    job.source = run.caller != 0 ? QStringLiteral("qucs-s") : QStringLiteral("user");
    job.caller = run.caller;
    job.kind = run.last.kind;
    const QString title = file.isEmpty() ? titleOf(run.document.data()) : QFileInfo(file).fileName();
    job.name = jobs::oneLine(run.last.what.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, run.last.what), 300);
    job.detail = file.isEmpty() ? tr("A document without a file.") : QDir::toNativeSeparators(file);
    job.document = file;
    job.status = jobs::Status::Started;
    job.percent = run.last.percent;
    job.said = run.last.said;
    job.whyNoPercent = run.last.whyNoPercent;
    job.canStop = true;
    job.background = true;
    job.whyNoStop = tr("It cannot be stopped from here.");
    QPointer<Tracker> self(this);
    QPointer<QObject> document(run.document);
    // (Of a schematic's run, by the run: the schematic may be closed.)
    QPointer<SimulationRun> spice(run.spice);
    QPointer<SimMessage> qucsator(run.qucsator);
    const bool spiceRun = run.spiceRun, qucsatorRun = run.qucsatorRun;
    run.job = jobs::Registry::instance().add(job, [self, document, spice, qucsator, spiceRun, qucsatorRun] {
        if (!self) return false;
        if (spiceRun) {
            if (!spice || !spice->isRunning()) return false;
            if (Run* r = document ? self->runOf(document) : nullptr) r->stopAsked = true;
            spice->stop();
            return true;
        }
        if (qucsatorRun) {
            if (!qucsator || qucsator->SimProcess.state() == QProcess::NotRunning) return false;
            return QMetaObject::invokeMethod(qucsator, "AbortSim");
        }
        return document && self->stopRun(document);
    });
    rememberJob(run);
}

void Tracker::rememberJob(const Run& run)
{
    if (run.job.isEmpty() || !run.document) return;
    a_jobDocuments.append({run.job, run.document});
    // (Those whose document went, and beyond what the list holds: forgotten.)
    for (qsizetype i = a_jobDocuments.size() - 1; i >= 0; --i)
        if (a_jobDocuments.at(i).second.isNull()) a_jobDocuments.removeAt(i);
    while (a_jobDocuments.size() > jobs::Registry::kMost) a_jobDocuments.removeFirst();
}

void Tracker::end(QObject* document, int status, const QString& outcome, int percent)
{
    for (qsizetype i = 0; i < a_runs.size(); ++i) {
        if (a_runs.at(i).document != document) continue;
        Run ended = a_runs.takeAt(i);
        if (!ended.job.isEmpty() && !ended.claudes)
            jobs::Registry::instance().update(ended.job, [status, outcome, percent](jobs::Job& job) {
                job.status = jobs::Status(status);
                if (!outcome.isEmpty()) job.outcome = jobs::oneLine(outcome, 400);
                if (percent >= 0) job.percent = percent;
            });
        emit changed();
        return;
    }
}

void Tracker::endModelRun(Run& run, QucsDoc* doc)
{
    // How it ended, from the model: a stop, an error, or its work done.
    int status = int(jobs::Status::Finished);
    QString outcome;
    const bool asked = run.stopAsked || (!run.job.isEmpty() && [&run] {
                           const auto job = jobs::Registry::instance().job(run.job);
                           return job && job->stopAsked;
                       }());
    if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) {
        if (model->stops() != run.stopsBefore || asked) status = int(jobs::Status::Stopped);
        else if (!model->lastError().isEmpty()) {
            status = int(jobs::Status::Failed);
            outcome = model->lastError();
        } else if (!model->lastWarnings().isEmpty()) {
            outcome = model->lastWarnings().constFirst();
        }
    } else if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) {
        if (model3->stops() != run.stopsBefore || asked) status = int(jobs::Status::Stopped);
        else if (run.last.kind == QLatin1String("solve") && (!model3->solution() || model3->solutionStale())) {
            status = int(jobs::Status::Failed);
            outcome = model3->messages().isEmpty() ? tr("It ended without a solution of the model as it stands.") : model3->messages().constLast();
        }
    } else if (auto* rf = dynamic_cast<RF25Doc*>(doc)) {
        if (asked) status = int(jobs::Status::Stopped);
        else if (!rf->extraction() && !rf->fullWave()) {
            status = int(jobs::Status::Failed);
            outcome = rf->solveProblem().isEmpty() ? tr("It ended without a result.") : rf->solveProblem();
        }
    }
    end(run.document.data(), status, outcome, status == int(jobs::Status::Finished) ? 100 : -1);
}

void Tracker::follow(QObject* document)
{
    if (document == nullptr || a_followed.contains(document)) return;
    a_followed.insert(document);
    connect(document, &QObject::destroyed, this, [this, document] {
        a_followed.remove(document);
        pollSoon();
    });
    // (A run is seen as it begins and ends, not only at the next reading.)
    if (auto* model = qobject_cast<MultiphysicsDoc*>(document)) connect(model, &MultiphysicsDoc::stateChanged, this, &Tracker::pollSoon);
    else if (auto* model3 = qobject_cast<Multiphysics3DDoc*>(document)) connect(model3, &Multiphysics3DDoc::stateChanged, this, &Tracker::pollSoon);
    else if (auto* rf = qobject_cast<RF25Doc*>(document)) connect(rf, &RF25Doc::solvedChanged, this, &Tracker::pollSoon);
}

void Tracker::pollSoon()
{
    if (!a_soon->isActive()) a_soon->start();
}

void Tracker::pollNow()
{
    bool changedAny = false;
    // Held documents gone (deleted from elsewhere): forgotten.
    for (qsizetype i = a_parked.size() - 1; i >= 0; --i)
        if (a_parked.at(i).isNull()) {
            a_parked.removeAt(i);
            changedAny = true;
        }

    const QList<QucsDoc*> docs = documents();
    for (QucsDoc* doc : docs) {
        auto* object = dynamic_cast<QObject*>(doc);
        if (object == nullptr) continue;
        if (qobject_cast<MultiphysicsDoc*>(object) == nullptr && qobject_cast<Multiphysics3DDoc*>(object) == nullptr
            && qobject_cast<RF25Doc*>(object) == nullptr)
            continue;
        follow(object);
        const Activity now = activityOf(doc);
        Run* run = runOf(object);
        if (now.busy && run == nullptr) {
            begin(doc, now);
            continue;
        }
        if (run == nullptr) continue;
        if (!now.busy) {
            endModelRun(*run, doc);
            changedAny = true;
            continue;
        }
        // What it runs may move on within one job (a geometry, then its mesh,
        // then the study): the newest is said.
        const bool kindChanged = run->last.kind != now.kind || run->last.what != now.what;
        run->last.percent = now.percent >= 0 ? now.percent : run->last.percent;
        if (!now.said.isEmpty()) run->last.said = now.said;
        if (kindChanged && run->job.isEmpty()) {
            run->last.kind = now.kind;
            run->last.what = now.what;
            run->last.listed = run->last.listed || now.listed;
        }
        if (run->job.isEmpty() && (now.listed || run->age.elapsed() >= kListedPast)) {
            run->last.kind = now.kind;
            run->last.what = now.what;
            list(*run);
            changedAny = true;
        }
        if (!run->job.isEmpty() && !run->claudes) {
            const int percent = now.percent;
            const QString said = now.said;
            jobs::Registry::instance().update(run->job, [percent, said](jobs::Job& job) {
                if (percent >= 0) job.percent = percent;
                if (!said.isEmpty()) job.said = said;
                job.canStop = true;
            });
        }
    }

    // Model runs whose document went (deleted with its run stopped, or the
    // window closing).
    for (qsizetype i = a_runs.size() - 1; i >= 0; --i) {
        Run& run = a_runs[i];
        if (run.document || run.spiceRun || run.qucsatorRun) continue;
        const QString job = run.job;
        const bool claudes = run.claudes;
        const bool asked = run.stopAsked;   // (Stop and Close)
        a_runs.removeAt(i);
        if (!job.isEmpty() && !claudes)
            jobs::Registry::instance().update(job, [asked](jobs::Job& j) {
                j.status = j.stopAsked || asked ? jobs::Status::Stopped : jobs::Status::Lost;
                j.said = asked ? tr("Stopped, and its tab closed.") : tr("Its document was closed, which stopped it.");
            });
        changedAny = true;
    }

    // A schematic's run: how far, as its own bar shows it.
    for (Run& run : a_runs) {
        if ((!run.spiceRun && !run.qucsatorRun) || run.job.isEmpty() || run.claudes) continue;
        int percent = -1;
        if (run.spiceRun) {
            SimulationConsole* console = a_app->simulationConsole();
            if (run.spice && console != nullptr && console->currentRun() == run.spice && console->percent() > 0) percent = console->percent();
        } else if (run.qucsator && run.qucsator->SimProgress->value() > 0) {
            percent = run.qucsator->SimProgress->value();
        }
        if (percent >= 0)
            jobs::Registry::instance().update(run.job, [percent](jobs::Job& job) { job.percent = percent; });
    }

    trimParked();
    if (changedAny) emit changed();
}

// ----------------------------------------------------------------------
void Tracker::park(QWidget* document)
{
    if (document == nullptr || isParked(document)) return;
    document->setParent(a_holder);
    document->hide();
    a_parked.append(document);
    follow(document);
    emit changed();
    trimParked();
}

bool Tracker::isParked(const QObject* document) const
{
    if (document == nullptr) return false;
    for (const QPointer<QWidget>& w : a_parked)
        if (w.data() == document) return true;
    return false;
}

QList<QWidget*> Tracker::parked() const
{
    QList<QWidget*> list;
    for (const QPointer<QWidget>& w : a_parked)
        if (w) list << w.data();
    return list;
}

QWidget* Tracker::parkedFile(const QString& file) const
{
    if (file.isEmpty()) return nullptr;
    const QString wanted = QDir::toNativeSeparators(file);
    for (const QPointer<QWidget>& w : a_parked) {
        auto* doc = w ? dynamic_cast<QucsDoc*>(w.data()) : nullptr;
        if (doc == nullptr || doc->getDocName().isEmpty()) continue;
        if (QDir::toNativeSeparators(doc->getDocName()) == wanted || misc::isSameFile(doc->getDocName(), file)) return w.data();
    }
    return nullptr;
}

void Tracker::unpark(QWidget* document)
{
    for (qsizetype i = 0; i < a_parked.size(); ++i)
        if (a_parked.at(i).data() == document) {
            a_parked.removeAt(i);
            // (Its entry among the tabs closed lately is used up.)
            for (qsizetype c = a_closed.size() - 1; c >= 0; --c)
                if (a_closed.at(c).parked.data() == document) a_closed.removeAt(c);
            emit changed();
            return;
        }
}

QList<QWidget*> Tracker::parkedWithChanges() const
{
    QList<QWidget*> list;
    for (QWidget* w : parked())
        if (auto* doc = dynamic_cast<QucsDoc*>(w); doc != nullptr && doc->getDocChanged()) list << w;
    return list;
}

void Tracker::releaseParked()
{
    const QList<QWidget*> all = parked();
    a_parked.clear();
    for (qsizetype c = a_closed.size() - 1; c >= 0; --c)
        if (!a_closed.at(c).parked.isNull()) a_closed.removeAt(c);
    for (QWidget* w : all) {
        stopRun(w);
        // (A model a tool still waits on is let go once the wait is over.)
        if (auto* doc = dynamic_cast<QucsDoc*>(w); doc != nullptr && doc->beingWaitedFor()) doc->closeWhenFree();
        else delete w;
    }
    pollNow();
    emit changed();
}

void Tracker::trimParked()
{
    // Finished, unchanged, beyond the most kept: the oldest let go. (A running
    // one, or one with changes not saved, is never let go here.)
    int idle = 0;
    for (const QPointer<QWidget>& w : a_parked) {
        auto* doc = w ? dynamic_cast<QucsDoc*>(w.data()) : nullptr;
        if (doc != nullptr && !activity(doc).busy && !doc->getDocChanged()) ++idle;
    }
    for (qsizetype i = 0; i < a_parked.size() && idle > kIdleParkedMost;) {
        QWidget* w = a_parked.at(i).data();
        auto* doc = dynamic_cast<QucsDoc*>(w);
        if (doc == nullptr || activity(doc).busy || doc->getDocChanged()) {
            ++i;
            continue;
        }
        unpark(w);
        --idle;
        if (auto* doc = dynamic_cast<QucsDoc*>(w); doc != nullptr && doc->beingWaitedFor()) doc->closeWhenFree();
        else delete w;
    }
}

// ----------------------------------------------------------------------
void Tracker::noteClosed(const QString& file, const QString& title, QWidget* parked)
{
    if (file.isEmpty() && parked == nullptr) return;   // (nothing to bring it back from)
    for (qsizetype i = a_closed.size() - 1; i >= 0; --i) {
        const Closed& c = a_closed.at(i);
        if ((!file.isEmpty() && c.file == file) || (parked != nullptr && c.parked.data() == parked)) a_closed.removeAt(i);
    }
    a_closed.prepend({file, title, parked, QDateTime::currentDateTime()});
    while (a_closed.size() > kClosedMost) a_closed.removeLast();
    emit changed();
}

QList<Tracker::Closed> Tracker::closed() const
{
    QList<Closed> list;
    for (const Closed& c : a_closed)
        if (!c.file.isEmpty() || c.parked) list << c;
    return list;
}

Tracker::Closed Tracker::takeClosed(int index)
{
    const QList<Closed> list = closed();
    if (index < 0 || index >= list.size()) return {};
    const Closed taken = list.at(index);
    for (qsizetype i = 0; i < a_closed.size(); ++i)
        if (a_closed.at(i).file == taken.file && a_closed.at(i).parked == taken.parked && a_closed.at(i).when == taken.when) {
            a_closed.removeAt(i);
            break;
        }
    emit changed();
    return taken;
}

void Tracker::clearClosed()
{
    a_closed.clear();
    emit changed();
}

// ----------------------------------------------------------------------
QString Tracker::jobOf(const QObject* document) const
{
    const Run* run = runOf(document);
    return run != nullptr ? run->job : QString();
}

QWidget* Tracker::documentOfJob(const QString& id) const
{
    if (id.isEmpty()) return nullptr;
    for (const Run& run : a_runs)
        if (run.job == id && run.document) return qobject_cast<QWidget*>(run.document.data());
    for (const auto& [job, document] : a_jobDocuments)
        if (job == id && document) return qobject_cast<QWidget*>(document.data());
    return nullptr;
}

QString Tracker::jobOfParked(const QObject* document) const
{
    for (qsizetype i = a_jobDocuments.size() - 1; i >= 0; --i)
        if (a_jobDocuments.at(i).second.data() == document) return a_jobDocuments.at(i).first;
    return {};
}

void Tracker::releaseOne(QWidget* document)
{
    if (!isParked(document)) return;
    unpark(document);
    stopRun(document);
    if (auto* doc = dynamic_cast<QucsDoc*>(document); doc != nullptr && doc->beingWaitedFor()) doc->closeWhenFree();
    else delete document;
    pollNow();
}

} // namespace qucs_s::runs
