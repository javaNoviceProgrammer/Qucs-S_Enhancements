/*
 * qucscontrol_jobs.cpp - the Qucs-S tools' calls as jobs (claudejobs.h): the
 *                        long ones listed as they begin, any other past a
 *                        moment; how far each has got, from what is doing the
 *                        work; and the tools that tell of them.
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

#include "claudejobs.h"
#include "claudejobswindow.h"
#include "dialogs/simmessage.h"
#include "multiphysics3ddoc.h"
#include "multiphysicsdoc.h"
#include "qucs.h"
#include "rf25doc.h"
#include "simulationconsole.h"
#include "simulationruns.h"

#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProgressBar>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace qucs_s::control;
namespace jobs = qucs_s::claude::jobs;

namespace {

// Listed as they begin: each is what a job is, whatever it takes.
QString kindOfTool(const QString& tool)
{
    if (tool == QLatin1String("simulate")) return QStringLiteral("simulation");
    if (tool == QLatin1String("fem_solve") || tool == QLatin1String("fem_converge") || tool == QLatin1String("rf25_solve")
        || tool == QLatin1String("rf25_converge"))
        return QStringLiteral("solve");
    if (tool == QLatin1String("fem_build")) return QStringLiteral("build");
    if (tool == QLatin1String("tune")) return QStringLiteral("tune");
    return {};
}

// Any other tool's call is listed once it has taken this long: a batch of
// edits, a script of a moment, are not jobs.
constexpr int kListedPast = 2000;   // ms
constexpr int kPollEvery = 250;     // ms

bool isListedAtOnce(const QString& tool)
{
    return !kindOfTool(tool).isEmpty();
}

// Those that tell of jobs, and a wait: nothing runs in them. (A wait the user
// sends to the background is listed then.)
bool isNeverAJob(const QString& tool)
{
    return tool == QLatin1String("list_background_jobs") || tool == QLatin1String("get_background_job")
           || tool == QLatin1String("show_background_jobs");
}

QString whyNoPercentOf(const QString& tool)
{
    if (tool == QLatin1String("simulate"))
        return QCoreApplication::translate("QucsControl", "The simulator has said no percentage yet: this run may have none to say (a DC operating point, a build of ngspice that prints none).");
    if (tool == QLatin1String("fem_solve") || tool == QLatin1String("fem_build") || tool == QLatin1String("fem_converge"))
        return QCoreApplication::translate("QucsControl", "The model has not said how far it has got yet.");
    if (tool == QLatin1String("rf25_solve") || tool == QLatin1String("rf25_converge"))
        return QCoreApplication::translate("QucsControl", "This step cannot be counted: a matrix being filled and factored says nothing until it is done.");
    if (tool == QLatin1String("run_script"))
        return QCoreApplication::translate("QucsControl", "A script may do anything: nothing knows how much of it is left.");
    if (tool == QLatin1String("tune"))
        return QCoreApplication::translate("QucsControl", "It tunes until its targets are met, or its most runs are used: how many it will take is not known.");
    return QCoreApplication::translate("QucsControl", "This tool does not say how far it has got.");
}

// The result's text, and the object it is when it is JSON.
QString textOfResult(const QJsonObject& result)
{
    const QJsonArray content = result.value(QLatin1String("content")).toArray();
    return content.isEmpty() ? QString() : content.at(0).toObject().value(QLatin1String("text")).toString();
}

QJsonObject jsonOfResult(const QJsonObject& result)
{
    return QJsonDocument::fromJson(textOfResult(result).toUtf8()).object();
}

QString outcomeOf(const QJsonObject& result)
{
    const QJsonObject o = jsonOfResult(result);
    if (o.isEmpty()) return jobs::oneLine(textOfResult(result), 400);
    if (o.value(QLatin1String("error")).isString()) return jobs::oneLine(o.value(QLatin1String("error")).toString(), 400);
    const QString note = o.value(QLatin1String("note")).toString();
    // (Said in full: by what alone was a tool's name, or nothing.)
    if (o.value(QLatin1String("stopped")).toBool()) {
        const QString by = o.value(QLatin1String("stopped by")).toString();
        QString said = by.isEmpty() ? QCoreApplication::translate("QucsControl", "Stopped before it ended.")
                                    : QCoreApplication::translate("QucsControl", "Stopped by %1.").arg(by);
        if (!note.isEmpty()) said += QLatin1Char(' ') + note;
        return jobs::oneLine(said, 400);
    }
    return jobs::oneLine(note, 400);
}

// What the work of a model is at: 0 to 100, or -1 while it has said nothing.
// (Nought is nothing said: a bar at nought for all of a run claims the run
// has done nothing. The rest is as the model's own bar shows it.)
int percentOfFraction(double fraction)
{
    return fraction > 0 ? std::clamp(int(std::floor(100 * fraction)), 0, 100) : -1;
}

} // namespace

struct QucsControl::Jobs {
    struct One {
        quint64 number = 0;
        quint64 caller = 0;
        QString tool;
        QJsonObject arguments;
        QString id;              // the job in the list; empty: not listed (yet)
        QElapsedTimer age;
        bool queued = false;     // waits behind a call that runs alone
        bool answered = false;   // its call is over, and what it began goes on
        int simRun = 0;          // the run it goes on as, by its id
        bool followsDocument = false;   // ... or the document it solves
        quint64 stopsBefore = 0; // the document's stops as it was followed
        QPointer<QObject> document;     // what it works on (a model)
        int stepDone = 0, stepOf = 0;   // a batch's calls, a convergence's meshes
    };

    ~Jobs()
    {
        // (Nothing left to say how they end: the control is going.)
        for (const One& one : std::as_const(all))
            if (!one.id.isEmpty())
                jobs::Registry::instance().update(one.id, [](jobs::Job& job) {
                    job.status = jobs::Status::Lost;
                    job.canStop = false;
                });
    }

    QMap<quint64, One> all;   // calls under way, and what goes on after them
    quint64 last = 0;
    QTimer* poll = nullptr;
};

QucsControl::Jobs& QucsControl::jobsOfCalls()
{
    if (!a_jobs) {
        a_jobs = std::make_shared<Jobs>();
        a_jobs->poll = new QTimer(this);
        a_jobs->poll->setInterval(kPollEvery);
        connect(a_jobs->poll, &QTimer::timeout, this, &QucsControl::pollJobs);
    }
    return *a_jobs;
}

quint64 QucsControl::jobBegins(quint64 caller, const QString& tool, const QJsonObject& arguments)
{
    if (isNeverAJob(tool)) return 0;
    Jobs& jobs = jobsOfCalls();
    Jobs::One one;
    one.number = ++jobs.last;
    one.caller = caller;
    one.tool = tool;
    one.arguments = arguments;
    one.age.start();
    const quint64 number = one.number;
    jobs.all.insert(number, one);
    if (isListedAtOnce(tool)) listJob(number);
    if (!jobs.poll->isActive()) jobs.poll->start();
    return number;
}

void QucsControl::listJob(quint64 number)
{
    Jobs& all = jobsOfCalls();
    const auto it = all.all.find(number);
    if (it == all.all.end() || !it->id.isEmpty()) return;
    jobs::Job job;
    job.source = QStringLiteral("qucs-s");
    job.kind = kindOfTool(it->tool);
    if (job.kind.isEmpty()) {
        job.kind = it->tool == QLatin1String("run_script") ? QStringLiteral("script")
                   : it->tool == QLatin1String("batch")    ? QStringLiteral("batch")
                                                           : QStringLiteral("tool");
    }
    const QString subject = it->tool == QLatin1String("batch")
                                ? tr("%n call(s)", nullptr, int(it->arguments.value(QLatin1String("calls")).toArray().size()))
                                : subjectOf(it->tool, it->arguments);
    job.name = jobs::oneLine(subject.isEmpty() ? it->tool : QStringLiteral("%1: %2").arg(it->tool, subject), 300);
    job.detail = QString::fromUtf8(QJsonDocument(it->arguments).toJson(QJsonDocument::Indented)).left(8000);
    job.tool = it->tool;
    job.caller = it->caller;
    job.status = it->queued ? jobs::Status::Queued : jobs::Status::Started;
    if (it->queued) job.queued = QDateTime::currentDateTime();
    job.whyNoPercent = whyNoPercentOf(it->tool);
    job.background = it->arguments.value(QLatin1String("background")).toBool();
    job.whyNoStop = it->tool == QLatin1String("tune")
                        ? tr("Qucs-S has no way to stop tune part way. Simulation > Stop Simulation stops the run it is at, which ends it.")
                        : tr("Qucs-S has no way to stop %1 part way: it ends by itself.").arg(it->tool);
    QPointer<QucsControl> self(this);
    it->id = jobs::Registry::instance().add(job, [self, number] { return self && self->stopJob(number); });
}

void QucsControl::jobQueued(quint64 number)
{
    if (number == 0) return;
    Jobs& jobs = jobsOfCalls();
    const auto it = jobs.all.find(number);
    if (it == jobs.all.end()) return;
    it->queued = true;
    if (it->id.isEmpty()) return;
    qucs_s::claude::jobs::Registry::instance().update(it->id, [](qucs_s::claude::jobs::Job& job) {
        job.status = qucs_s::claude::jobs::Status::Queued;
        job.queued = QDateTime::currentDateTime();
        job.began = {};
    });
}

void QucsControl::jobRuns(quint64 number)
{
    if (number == 0) return;
    Jobs& jobs = jobsOfCalls();
    const auto it = jobs.all.find(number);
    if (it == jobs.all.end()) return;
    it->queued = false;
    // What it works on when it names none: what is in front as it begins.
    if (it->arguments.value(QLatin1String("path")).toString().isEmpty())
        if (auto* object = dynamic_cast<QObject*>(a_app->getDoc())) it->document = object;
    if (it->id.isEmpty()) return;
    qucs_s::claude::jobs::Registry::instance().update(it->id, [](qucs_s::claude::jobs::Job& job) {
        if (job.status == qucs_s::claude::jobs::Status::Queued) job.status = qucs_s::claude::jobs::Status::Started;
    });
}

void QucsControl::jobStep(const Running& call, int done, int of)
{
    if (call.job == 0 || of <= 0) return;
    Jobs& jobs = jobsOfCalls();
    const auto it = jobs.all.find(call.job);
    if (it == jobs.all.end()) return;
    it->stepDone = std::clamp(done, 0, of);
    it->stepOf = of;
}

void QucsControl::callSentToBackground(quint64 caller, const QString& tool)
{
    Jobs& jobs = jobsOfCalls();
    // (The latest of its name not sent yet: which call it was, the host is
    // not told.)
    for (auto it = jobs.all.end(); it != jobs.all.begin();) {
        --it;
        if (it->caller != caller || it->tool != tool || it->answered) continue;
        listJob(it.key());
        const QString id = it->id;
        bool sent = false;
        qucs_s::claude::jobs::Registry::instance().update(id, [&sent](qucs_s::claude::jobs::Job& job) {
            sent = job.background;
            job.background = true;
        });
        if (!sent) return;
    }
}

void QucsControl::jobCancelled(quint64 caller, const QString& tool)
{
    if (!a_jobs) return;
    for (const Jobs::One& one : std::as_const(a_jobs->all))
        if (one.caller == caller && one.tool == tool && !one.answered && !one.id.isEmpty())
            qucs_s::claude::jobs::Registry::instance().update(one.id, [](qucs_s::claude::jobs::Job& job) { job.stopAsked = true; });
}

QucsDoc* QucsControl::jobDocument(quint64 number)
{
    Jobs& jobs = jobsOfCalls();
    const auto it = jobs.all.find(number);
    if (it == jobs.all.end()) return nullptr;
    if (it->document) return dynamic_cast<QucsDoc*>(it->document.data());
    const QString path = it->arguments.value(QLatin1String("path")).toString();
    if (path.isEmpty()) return nullptr;
    // (Its file, else - a path relative to a folder not known here - its name.)
    QList<QucsDoc*> documents = a_app->allDocuments();
    // (Kept running with its tab closed: still what it works on.)
    if (const qucs_s::runs::Tracker* runs = a_app->runTracker())
        for (QWidget* w : runs->parked())
            if (auto* kept = dynamic_cast<QucsDoc*>(w)) documents << kept;
    for (const bool exact : {true, false})
        for (QucsDoc* doc : documents) {
            const QString name = doc->getDocName();
            if (!name.isEmpty() && (exact ? sameFile(name, path) : QFileInfo(name).fileName() == QFileInfo(path).fileName())) {
                it->document = dynamic_cast<QObject*>(doc);
                return doc;
            }
        }
    return nullptr;
}

QString QucsControl::jobWorkingOn(QObject* document, quint64* caller)
{
    if (caller != nullptr) *caller = 0;
    if (!a_jobs || document == nullptr) return {};
    // (The newest first: a later call is the one that began this run.)
    for (auto it = a_jobs->all.end(); it != a_jobs->all.begin();) {
        --it;
        if (it->queued) continue;
        QucsDoc* doc = jobDocument(it.key());
        if (doc == nullptr || dynamic_cast<QObject*>(doc) != document) continue;
        if (caller != nullptr && *caller == 0) *caller = it->caller;
        // A tool that follows its run, or a batch or script that waits on the
        // calls in it: their job is the run's. (Any other - a menu action -
        // ends before the run does: the run is a job of its own, Claude's.)
        const bool waitsOnIt = it->tool == QLatin1String("batch") || it->tool == QLatin1String("run_script");
        if (!isListedAtOnce(it->tool) && !waitsOnIt) continue;
        listJob(it.key());
        return it->id;
    }
    return {};
}

void QucsControl::jobAnswered(quint64 number, const QJsonObject& result)
{
    Jobs& all = jobsOfCalls();
    const auto it = all.all.find(number);
    if (it == all.all.end()) return;
    it->answered = true;
    // Answered, and what it began goes on: a simulation followed by its id, a
    // solve past its call's timeout. Its job is not over.
    if (it->tool == QLatin1String("simulate")) {
        const QJsonObject o = jsonOfResult(result);
        // ('background': its id. Past its timeout: followed from this moment,
        // by the id just given to it - the answer says it only in words.)
        int id = o.value(QLatin1String("running")).toBool() ? o.value(QLatin1String("id")).toInt() : 0;
        if (o.contains(QLatin1String("finished")) && !o.value(QLatin1String("finished")).toBool() && !a_simRuns.isEmpty())
            id = a_simRuns.last().id;
        if (id > 0) {
            const SimRun* run = simRun(id);
            if (run != nullptr && run->ended.isValid()) return endJob(number, run->result);
            it->simRun = id;
            listJob(number);
            jobs::Registry::instance().update(it->id, [](jobs::Job& job) { job.background = true; });
            return;
        }
    } else if (it->tool == QLatin1String("fem_solve") || it->tool == QLatin1String("fem_build") || it->tool == QLatin1String("rf25_solve")
               || it->tool == QLatin1String("rf25_converge")) {
        if (result.value(QLatin1String("isError")).toBool()) {
            QucsDoc* doc = jobDocument(number);
            bool busy = false;
            if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) {
                busy = model->isBusy();
                it->stopsBefore = model->stops();
            } else if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) {
                busy = !model3->idle();
                it->stopsBefore = model3->stops();
            } else if (auto* rf = dynamic_cast<RF25Doc*>(doc)) {
                busy = rf->isSolving();
            }
            if (busy) {
                it->followsDocument = true;
                listJob(number);
                jobs::Registry::instance().update(it->id, [](jobs::Job& job) { job.background = true; });
                return;
            }
        }
    }
    endJob(number, result);
}

void QucsControl::endJob(quint64 number, const QJsonObject& result)
{
    Jobs& all = jobsOfCalls();
    const auto it = all.all.find(number);
    if (it == all.all.end()) return;
    const QString id = it->id;
    const QString tool = it->tool;
    all.all.erase(it);
    if (id.isEmpty()) return;   // (over in a moment: no job)
    const QJsonObject o = jsonOfResult(result);
    const bool failed = result.value(QLatin1String("isError")).toBool()
                        || (tool == QLatin1String("simulate") && o.contains(QLatin1String("succeeded")) && !o.value(QLatin1String("succeeded")).toBool());
    const QString outcome = outcomeOf(result);
    jobs::Registry::instance().update(id, [failed, outcome, &o](jobs::Job& job) {
        job.outcome = outcome;
        // A stop asked for, and it ended without having done its work: it was
        // stopped. It ended well: it got there first, and that is what is said.
        if ((job.stopAsked && failed) || o.value(QLatin1String("stopped")).toBool()) {
            job.status = jobs::Status::Stopped;
        } else if (failed) {
            job.status = jobs::Status::Failed;
        } else {
            job.status = jobs::Status::Finished;
            job.percent = 100;
            if (job.stopAsked) job.said = QCoreApplication::translate("QucsControl", "It ended by itself before the stop reached it.");
        }
        job.canStop = false;
    });
}

void QucsControl::jobSimEnded(int simRunId, const QJsonObject& result)
{
    if (!a_jobs) return;
    for (const Jobs::One& one : std::as_const(a_jobs->all))
        if (one.simRun == simRunId) return endJob(one.number, result);
}

bool QucsControl::stopJob(quint64 number)
{
    Jobs& all = jobsOfCalls();
    const auto it = all.all.find(number);
    if (it == all.all.end()) return false;
    // Waiting its turn: taken out, as a call cancelled before it began.
    for (qsizetype i = 0; i < a_waiting.size(); ++i)
        if (a_waiting.at(i).job == number) {
            const Waiting w = a_waiting.takeAt(i);
            w.done(errorResult(tr("Stopped by the user before it began (the Background Jobs window).")));
            return true;
        }
    // Under way: what it waits on stopped, as when its client cancels it.
    // (A copy: a stop may end its call at once.)
    bool stopped = false;
    const QList<Stopper> stoppers = a_stoppers;
    for (const Stopper& s : stoppers)
        if (s.call.job == number) {
            s.stop();
            stopped = true;
        }
    if (stopped) return true;
    // Gone on after its answer.
    if (it->simRun != 0) {
        const int id = it->simRun;
        stopSimulation(QJsonObject{{QStringLiteral("id"), id}}, [](const QJsonObject&) {});
        // (As its outcome and the conversations' note say: not a tool's doing.)
        if (SimRun* run = simRun(id); run != nullptr && !run->ended.isValid())
            run->stoppedBy = tr("the Background Jobs window");
        return true;
    }
    if (it->followsDocument) {
        QucsDoc* doc = jobDocument(number);
        if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) model->cancelJob();
        else if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) model3->cancel();
        else if (auto* rf = dynamic_cast<RF25Doc*>(doc)) rf->stopSolving();
        else return false;
        return true;
    }
    return false;
}

void QucsControl::pollJobs()
{
    Jobs& all = jobsOfCalls();
    const QList<quint64> numbers = all.all.keys();
    for (const quint64 number : numbers) {
        const auto it = all.all.find(number);
        if (it == all.all.end()) continue;
        if (it->id.isEmpty() && it->tool != QLatin1String("wait_for") && it->age.elapsed() >= kListedPast) listJob(number);
        if (it->id.isEmpty()) continue;

        // How far: from what does the work, as its own bar shows it.
        int percent = -1;
        QString said;
        QucsDoc* doc = jobDocument(number);
        if (it->tool == QLatin1String("simulate") && !it->queued) {
            if (SimulationConsole* console = a_app->simulationConsole(); console != nullptr && console->isRunning())
                percent = console->percent() > 0 ? console->percent() : -1;
            for (SimMessage* m : a_app->findChildren<SimMessage*>())
                if (m->SimProcess.state() != QProcess::NotRunning && m->SimProgress->value() > 0) percent = m->SimProgress->value();
        } else if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc); model != nullptr && model->isBusy()) {
            percent = percentOfFraction(model->jobFraction());
            said = model->jobProgress();
        } else if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc); model3 != nullptr && model3->isBusy()) {
            percent = percentOfFraction(model3->jobFraction());
            said = model3->jobProgress();
        } else if (auto* rf = dynamic_cast<RF25Doc*>(doc); rf != nullptr && rf->isSolving()) {
            int done = 0, of = 0;
            said = rf->solveCount(&done, &of);
            percent = of > 0 && done > 0 ? std::clamp(100 * done / of, 0, 100) : -1;
        }
        if (it->stepOf > 0) {
            // Of a batch's calls, a convergence's meshes: those done, and of
            // the one under way what it has said.
            percent = std::clamp((100 * it->stepDone + std::max(percent, 0)) / it->stepOf, 0, 100);
        }

        // Whether it can be stopped, as it stands now.
        bool stoppableNow = it->queued || it->simRun != 0 || it->followsDocument;
        for (const Stopper& s : std::as_const(a_stoppers))
            if (s.call.job == number) stoppableNow = true;
        for (const Waiting& w : std::as_const(a_waiting))
            if (w.job == number) stoppableNow = true;

        const QString file = doc != nullptr ? doc->getDocName() : QString();
        jobs::Registry::instance().update(it->id, [percent, said, stoppableNow, file](jobs::Job& job) {
            // (A later part may say nothing where an earlier said something:
            // what was said stands, as the last known.)
            if (percent >= 0) job.percent = percent;
            if (!said.isEmpty()) job.said = said;
            job.canStop = stoppableNow;
            // (What it works on, for the window's Show.)
            if (!file.isEmpty()) job.document = file;
        });

        // A solve followed past its answer: over when its document is free.
        if (it->followsDocument) {
            bool busy = false, stopsChanged = false, unsolved = false;
            const QString tool = it->tool;
            const QString study = it->arguments.value(QLatin1String("study")).toString();
            const QString closed = doc == nullptr ? tr("Its document was closed.") : QString();
            if (auto* model = dynamic_cast<MultiphysicsDoc*>(doc)) {
                busy = model->isBusy();
                stopsChanged = model->stops() != it->stopsBefore;
                unsolved = tool == QLatin1String("fem_solve") && model->solutionStale(study);
            } else if (auto* model3 = dynamic_cast<Multiphysics3DDoc*>(doc)) {
                busy = !model3->idle();
                stopsChanged = model3->stops() != it->stopsBefore;
                unsolved = tool == QLatin1String("fem_solve") && (!model3->solution() || model3->solutionStale());
            } else if (auto* rf = dynamic_cast<RF25Doc*>(doc)) {
                busy = rf->isSolving();
            }
            if (!busy) {
                const QString id = it->id;
                all.all.erase(it);
                jobs::Registry::instance().update(id, [closed, stopsChanged, unsolved](jobs::Job& job) {
                    if (!closed.isEmpty()) {
                        job.status = jobs::Status::Lost;
                        job.said = closed;
                    } else if (stopsChanged || job.stopAsked) {
                        job.status = jobs::Status::Stopped;
                    } else if (unsolved) {
                        job.status = jobs::Status::Failed;
                        job.outcome = QCoreApplication::translate("QucsControl", "It ended without a solution of the model as it stands.");
                    } else {
                        job.status = jobs::Status::Finished;
                        job.percent = 100;
                    }
                    job.canStop = false;
                });
            }
        }
    }
    if (all.all.isEmpty()) all.poll->stop();
}

// ----------------------------------------------------------------------
// The tools

namespace {

// What is known of jobs under --mcp-server, for the answers there.
QString headlessNote()
{
    return QCoreApplication::translate(
        "QucsControl",
        "This is qucs-s --mcp-server: no window of it shows, and it knows only the Qucs-S tools called of it, this run. The commands "
        "and agents of the Claude Code that called it are its own (its /bashes, /agents).");
}

} // namespace

QJsonObject QucsControl::listBackgroundJobs(const QJsonObject& args)
{
    pollJobsIfAny();
    const QString status = args.value(QLatin1String("status")).toString().trimmed();
    static const QStringList kinds{QStringLiteral("all"), QStringLiteral("going"), QStringLiteral("over")};
    bool known = status.isEmpty() || kinds.contains(status);
    for (const jobs::Status s : {jobs::Status::Queued, jobs::Status::Started, jobs::Status::Running, jobs::Status::Stopping, jobs::Status::Finished,
                                 jobs::Status::Failed, jobs::Status::Stopped, jobs::Status::Lost})
        known = known || status == jobs::statusKey(s);
    if (!known)
        return errorResult(tr("'status' is all, going, over, or a job's status (queued, started, running, stopping, finished, failed, "
                              "stopped, lost): %1 is none of them. Nothing was listed.").arg(status));
    const bool mine = args.value(QLatin1String("mine")).toBool();
    const QString by = args.value(QLatin1String("started_by")).toString().trimmed();
    if (!by.isEmpty() && by != QLatin1String("any") && by != QLatin1String("user") && by != QLatin1String("claude"))
        return errorResult(tr("'started_by' is any, user or claude: %1 is none of them. Nothing was listed.").arg(by));
    if (args.contains(QLatin1String("max")) && args.value(QLatin1String("max")).toInt(0) < 1)
        return errorResult(tr("'max' is how many at most, 1 or more. Nothing was listed."));
    const int most = args.value(QLatin1String("max")).toInt(50);
    const quint64 caller = running().caller;
    QJsonArray list;
    int going = 0, over = 0, matching = 0;
    for (const jobs::Job& job : jobs::Registry::instance().jobs()) {
        jobs::isOver(job.status) ? ++over : ++going;
        if (mine && job.caller != caller) continue;
        if ((by == QLatin1String("user") || by == QLatin1String("claude")) && jobs::startedBy(job) != by) continue;
        if (status == QLatin1String("going") ? jobs::isOver(job.status)
            : status == QLatin1String("over") ? !jobs::isOver(job.status)
            : !status.isEmpty() && status != QLatin1String("all") ? jobs::statusKey(job.status) != status
                                                                  : false)
            continue;
        ++matching;
        if (list.size() < most) list.append(jobs::toJson(job, caller, false));
    }
    QJsonObject o{{QStringLiteral("going"), going}, {QStringLiteral("over"), over}, {QStringLiteral("jobs"), list}};
    if (matching > list.size()) o.insert(QStringLiteral("more"), tr("%1 more match: 'max' shows more.").arg(matching - list.size()));
    if (a_headless) o.insert(QStringLiteral("note"), headlessNote());
    return jsonResult(o);
}

QJsonObject QucsControl::getBackgroundJob(const QJsonObject& args)
{
    pollJobsIfAny();
    const QString id = args.value(QLatin1String("id")).toString().trimmed();
    if (id.isEmpty()) return errorResult(tr("get_background_job needs 'id', a job's id as list_background_jobs gives it (\"job-3\")."));
    const auto job = jobs::Registry::instance().job(id);
    if (!job) return errorResult(tr("There is no job %1 (list_background_jobs lists them; cleared ones are gone).").arg(id));
    QJsonObject o = jobs::toJson(*job, running().caller, true);
    if (a_headless) o.insert(QStringLiteral("note"), headlessNote());
    return jsonResult(o);
}

QJsonObject QucsControl::showBackgroundJobs(const QJsonObject& args)
{
    pollJobsIfAny();
    const QString filter = args.value(QLatin1String("filter")).toString().trimmed();
    static const QStringList filters{QStringLiteral("all"), QStringLiteral("simulations"), QStringLiteral("user"), QStringLiteral("claude")};
    if (!filter.isEmpty() && !filters.contains(filter))
        return errorResult(tr("'filter' is all, simulations, user or claude: %1 is none of them. The window was not opened.").arg(filter));
    QJsonArray list;
    const QList<jobs::Job> all = jobs::Registry::instance().jobs();
    int going = 0;
    for (const jobs::Job& job : all) {
        if (!jobs::isOver(job.status)) ++going;
        if (list.size() < 50) list.append(jobs::toJson(job, running().caller, false));
    }
    QJsonObject o{{QStringLiteral("going"), going}, {QStringLiteral("over"), int(all.size()) - going}, {QStringLiteral("jobs"), list}};
    if (a_headless) {
        o.insert(QStringLiteral("shown"), false);
        o.insert(QStringLiteral("note"), headlessNote());
        return jsonResult(o);
    }
    if (qucs_s::runs::Tracker* runs = a_app->runTracker()) runs->pollNow();
    ClaudeJobsWindow* window = ClaudeJobsWindow::showWindow(a_app);
    if (!filter.isEmpty())
        window->setFilter(filter == QLatin1String("simulations") ? ClaudeJobsWindow::Simulations
                          : filter == QLatin1String("user")      ? ClaudeJobsWindow::Yours
                          : filter == QLatin1String("claude")    ? ClaudeJobsWindow::Claudes
                                                                 : ClaudeJobsWindow::AllJobs);
    o.insert(QStringLiteral("shown"), window->isVisible());
    // (What can be known of it from here, as neither get_dialog nor screenshot
    // reads it.)
    o.insert(QStringLiteral("window"), QJsonObject{{QStringLiteral("title"), window->windowTitle()},
                                                   {QStringLiteral("active"), window->isActiveWindow()},
                                                   {QStringLiteral("rows"), window->table()->rowCount()},
                                                   {QStringLiteral("filter"), window->filter()->currentText()},
                                                   {QStringLiteral("note"), tr("The user can stop a job or clear those over there.")}});
    return jsonResult(o);
}

void QucsControl::pollJobsIfAny()
{
    if (a_jobs && !a_jobs->all.isEmpty()) pollJobs();
    // (And the user's runs, read now rather than at the next turn.)
    if (!a_headless)
        if (qucs_s::runs::Tracker* runs = a_app->runTracker()) runs->pollNow();
}
