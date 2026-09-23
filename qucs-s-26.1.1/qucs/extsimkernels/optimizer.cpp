/*
 * optimizer.cpp - an optimization component's search run with ngspice
 * (see optimizer.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "optimizer.h"

#include <QDir>
#include <QFile>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <limits>

#include "misc.h"
#include "ngspice.h"
#include "valuereading.h"
#include "schematic.h"
#include "simulationrun.h"

using namespace qucs_s::optimization;

namespace {
const double kInfinity = std::numeric_limits<double>::infinity();
const double kNaN = std::numeric_limits<double>::quiet_NaN();
}

Optimizer::Optimizer(Schematic* schematic, const Problem& problem, QObject* parent)
    : QObject(parent),
      a_schematic(schematic),
      a_problem(problem),
      a_de(problem.lower(), problem.upper(), problem.settings),
      a_analysis(analysisOf(schematic, problem.simulation)),
      a_bestCost(kInfinity),
    a_limitsOnly(std::none_of(problem.goals.begin(), problem.goals.end(), [](const Goal& g) { return g.isObjective(); }))
{
    const int cores = std::clamp(QThread::idealThreadCount(), 1, 8);
    const int count = std::min(cores, a_de.populationSize());
    const QString base = misc::scratchDirFor(schematic != nullptr ? schematic->getDocName() : QString())
                         + QDir::separator() + QStringLiteral("opt");
    for (int i = 0; i < count; ++i) {
        Worker w;
        w.dir = base + QDir::separator() + QString::number(i + 1);
        a_workers << w;
    }
}

Optimizer::~Optimizer()
{
    for (Worker& w : a_workers)
        if (w.kernel != nullptr) {
            w.kernel->disconnect(this);
            w.kernel->killThemAll();
        }
}

QString Optimizer::analysisOf(const Schematic* schematic, const QString& simulation)
{
    if (schematic == nullptr) return QString();
    const Component* c = nullptr;
    for (Component* o : schematic->a_DocComps)
        if (o->isSimulation && o->Name.compare(simulation, Qt::CaseInsensitive) == 0) c = o;
    // A sweep's results are those of the simulation it runs.
    for (int depth = 0; c != nullptr && c->Model == QLatin1String(".SW") && depth < 8; ++depth) {
        const Component* inner = nullptr;
        for (Component* o : schematic->a_DocComps)
            if (o->isSimulation && !c->Props.isEmpty()
                && o->Name.compare(c->Props.at(0)->Value, Qt::CaseInsensitive) == 0)
                inner = o;
        if (inner == nullptr) break;
        c = inner;
    }
    return c != nullptr ? analysisPrefix(c->Model) : QString();
}

QMap<QString, double> Optimizer::bestValues() const
{
    return a_problem.values(a_bestX);
}

void Optimizer::start()
{
    if (a_phase != Idle) return;
    a_phase = Start;
    a_batch = {DifferentialEvolution::Candidate{0, a_problem.initialCoordinates()}};
    a_costs = QVector<double>(1, kNaN);
    a_next = 0;
    emit progress(0);
    dispatch();
}

void Optimizer::stop()
{
    if (!isRunning() || a_phase == Stopping) return;
    a_phase = Stopping;
    a_batch = a_batch.mid(0, a_next);   // nothing more to start
    bool running = false;
    for (Worker& w : a_workers)
        if (w.kernel != nullptr) {
            running = true;
            w.kernel->killThemAll();   // its finished() comes back to evaluated()
        }
    if (!running) finish(!a_bestX.isEmpty());
}

void Optimizer::nextRound()
{
    if (a_phase != Search) return;
    if (a_de.finished()) {
        finish(true);
        return;
    }
    a_batch = a_de.next();
    a_costs = QVector<double>(a_batch.size(), kNaN);
    a_next = 0;
    dispatch();
}

void Optimizer::dispatch()
{
    for (int w = 0; w < a_workers.size() && a_next < a_batch.size(); ++w)
        if (a_workers.at(w).kernel == nullptr) startOn(w, a_next++);
}

void Optimizer::startOn(int w, int candidate)
{
    if (a_schematic.isNull()) {
        stop();
        return;
    }
    Worker& worker = a_workers[w];
    QDir().mkpath(worker.dir);
    QFile::remove(worker.dir + QDir::separator() + QStringLiteral("opt.dat"));
    auto* kernel = new Ngspice(a_schematic, this);
    kernel->setWorkdir(worker.dir);
    kernel->setOperatingPointOnly(false);
    SimulationRun::configureNgspice(kernel);
    worker.kernel = kernel;
    worker.candidate = candidate;
    ++a_outstanding;
    ++a_simulations;
    // Queued: the kernel reports a netlist it refused from inside
    // slotSimulate(), while the schematic still holds the candidate.
    connect(kernel, &AbstractSpiceKernel::finished, this, [this, w] { evaluated(w); }, Qt::QueuedConnection);
    connect(kernel, &AbstractSpiceKernel::errors, this,
            [this, w](QProcess::ProcessError e) {
                if (e == QProcess::FailedToStart) failedToStart(w);
            },
            Qt::QueuedConnection);
    const NetlistScope scope(a_schematic, a_problem, a_problem.values(a_batch.at(candidate).x));
    kernel->setExtraParameters(scope.extraParameters());
    kernel->slotSimulate();
}

void Optimizer::failedToStart(int w)
{
    Worker& worker = a_workers[w];
    if (worker.kernel == nullptr) return;
    const QString output = worker.kernel->getOutput().trimmed();
    const QString why = !output.isEmpty() ? output
                                          : tr("Failed to start simulator \"%1\": %2")
                                                .arg(worker.kernel->simulatorCommand(),
                                                     worker.kernel->processErrorString());
    worker.kernel->deleteLater();
    worker.kernel = nullptr;
    worker.candidate = -1;
    --a_outstanding;
    // ngspice does not start at all, or the schematic is not simulated:
    // no candidate would be.
    emit message(why);
    a_startFailure = why;
    if (a_phase == Start || a_phase == Search) {
        a_phase = Stopping;
        a_batch = a_batch.mid(0, a_next);
        for (Worker& other : a_workers)
            if (other.kernel != nullptr) other.kernel->killThemAll();
    }
    if (a_outstanding == 0) finish(false);
}

void Optimizer::evaluated(int w)
{
    Worker& worker = a_workers[w];
    if (worker.kernel == nullptr) return;
    Ngspice* kernel = worker.kernel;
    const int k = worker.candidate;
    worker.kernel = nullptr;
    worker.candidate = -1;
    --a_outstanding;

    QVector<double> measured(a_problem.goals.size(), kNaN);
    QString error;
    if (a_phase != Stopping && !a_schematic.isNull() && k >= 0 && k < a_batch.size()) {
        const QString output = kernel->getOutput();
        if (SimulationRun::logContainsError(output)) {
            error = tr("the simulation failed:\n%1").arg(output.trimmed());
        } else {
            const QString dataset = worker.dir + QDir::separator() + QStringLiteral("opt.dat");
            kernel->convertToQucsData(dataset);
            measured = measureDataset(dataset, &error);
        }
    }
    kernel->deleteLater();

    if (a_phase == Start) {
        if (!error.isEmpty()) {
            emit message(tr("The start cannot be simulated: %1").arg(error));
            a_startFailure = error;
            finish(false);
            return;
        }
        a_scale = scaleOf(measured);
        const double c = cost(a_problem.goals, measured, a_scale, a_problem.settings);
        a_bestX = a_batch.first().x;
        a_bestMeasured = measured;
        a_bestCost = c;
        emit message(tr("Start: cost %1").arg(number(c)) + pointLine(a_bestX, measured));
        a_de.start(a_bestX, c);
        a_phase = Search;
        if (goalsMet()) finish(true);   // nothing to improve
        else nextRound();
        return;
    }

    if (a_phase == Search && k >= 0 && k < a_costs.size()) {
        const double c = cost(a_problem.goals, measured, a_scale, a_problem.settings);
        a_costs[k] = c;
        if (c < a_bestCost) {
            a_bestCost = c;
            a_bestX = a_batch.at(k).x;
            a_bestMeasured = measured;
        }
        if (a_next < a_batch.size()) startOn(w, a_next++);
        else if (a_outstanding == 0) roundDone();
        return;
    }

    if (a_phase == Stopping && a_outstanding == 0) finish(!a_bestX.isEmpty() && a_startFailure.isEmpty());
}

void Optimizer::roundDone()
{
    a_de.report(a_costs);
    const int g = a_de.generation();
    const int total = a_problem.settings.generations;
    emit progress(std::clamp(100 * g / std::max(total, 1), 0, 100));
    const bool last = a_de.finished() || goalsMet();
    if (g >= 1 && (g % a_problem.settings.refresh == 0 || last))
        emit message(tr("Generation %1 of %2: cost %3").arg(g).arg(total).arg(number(a_bestCost))
                     + pointLine(a_bestX, a_bestMeasured));
    if (goalsMet()) finish(true);
    else nextRound();
}

void Optimizer::finish(bool ok)
{
    if (a_phase == Done) return;
    const bool stopped = a_phase == Stopping;
    a_phase = Done;
    if (ok) {
        const int g = std::max(a_de.generation(), 0);
        const QString generations = g == 1 ? tr("1 generation") : tr("%1 generations").arg(g);
        if (stopped)
            emit message(tr("Stopped after %1.").arg(generations));
        else if (goalsMet())
            emit message(g == 0 ? tr("The start meets every goal.") : tr("Every goal is met after %1.").arg(generations));
        else if (a_de.converged())
            emit message(tr("Converged after %1: the costs of the population differ by less than %2.")
                             .arg(generations, number(a_problem.settings.minCostVariance)));
        else
            emit message(tr("The %1 are done.").arg(generations));
        emit message(tr("Best: cost %1 after %2")
                         .arg(number(a_bestCost),
                              a_simulations == 1 ? tr("1 simulation") : tr("%1 simulations").arg(a_simulations))
                     + pointLine(a_bestX, a_bestMeasured));
    } else if (stopped && a_startFailure.isEmpty()) {
        emit message(tr("Stopped before the start was simulated."));
    }
    emit progress(100);
    emit finished(ok);
}

QVector<double> Optimizer::measureDataset(const QString& file, QString* error) const
{
    QVector<double> measured(a_problem.goals.size(), kNaN);
    const QHash<QString, QVector<double>> table = readDataset(file, error);
    if (table.isEmpty()) return measured;
    const QStringList names = table.keys();
    QStringList missing;
    for (int i = 0; i < a_problem.goals.size(); ++i) {
        const Goal& g = a_problem.goals.at(i);
        const QString name = resolve(names, g.name, a_analysis);
        if (name.isEmpty()) {
            missing << g.name;
            continue;
        }
        measured[i] = measure(g, table.value(name));
    }
    if (!missing.isEmpty() && error != nullptr) {
        QStringList sorted = names;
        sorted.sort(Qt::CaseInsensitive);
        *error = tr("the results have no %1 - they are: %2").arg(missing.join(QStringLiteral(", ")),
                                                                 sorted.join(QStringLiteral(", ")));
    }
    return measured;
}

bool Optimizer::goalsMet() const
{
    // Only limits, all of them met: the cost is 0 and cannot fall.
    return a_limitsOnly && !a_bestX.isEmpty() && a_bestCost <= 0;
}

QString Optimizer::pointLine(const QVector<double>& x, const QVector<double>& measured) const
{
    QStringList parts;
    const QMap<QString, double> values = a_problem.values(x);
    for (const Variable& v : a_problem.variables)
        if (v.active)
            parts << QStringLiteral("%1 = %2").arg(v.name, qucs_s::units::engineering(values.value(v.name)));
    for (int i = 0; i < a_problem.goals.size(); ++i)
        parts << a_problem.goals.at(i).describe(measured.value(i, kNaN));
    return QStringLiteral("\n    ") + parts.join(QStringLiteral("\n    "));
}
