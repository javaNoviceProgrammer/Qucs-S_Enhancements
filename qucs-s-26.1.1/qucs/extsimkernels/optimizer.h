/*
 * optimizer.h - an optimization component's search run with ngspice: every
 * candidate one simulation, several at a time
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_OPTIMIZER_H
#define QUCS_OPTIMIZER_H

#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QVector>

#include "optimization.h"

class Ngspice;
class Schematic;

/*!
 * \brief The search of an optimization (.Opt) with ngspice.
 *
 * The start - the variables' initial values - is simulated first: its
 * measurements are what the objectives are measured against, and a goal
 * the results do not have ends the run there. Then differential evolution
 * proposes a population of candidates each generation; each is a netlist
 * of the schematic with the candidate's values (optimization::NetlistScope)
 * simulated in a folder of its own under the schematic's Scratch folder
 * (opt/1, opt/2, ...), as many at a time as the machine has cores (up to
 * eight). The goals are read from the dataset of each. The search ends
 * after the generations of the settings, when the costs of the population
 * no longer differ (the settings' minimum cost variance), or - when every
 * goal is a limit (LE, GE, EQ) - once all are met.
 *
 * SimulationRun runs it when the schematic has an active optimization
 * component, then simulates the best point it found the ordinary way.
 */
class Optimizer : public QObject
{
    Q_OBJECT
public:
    Optimizer(Schematic* schematic, const qucs_s::optimization::Problem& problem, QObject* parent = nullptr);
    ~Optimizer() override;

    /// Simulates the start, then searches.
    void start();
    /// No more simulations: those running are stopped, and the run
    /// finishes with the best point so far.
    void stop();
    bool isRunning() const { return a_phase != Idle && a_phase != Done; }

    const qucs_s::optimization::Problem& problem() const { return a_problem; }
    /// The best point: all the variables' values, by name.
    QMap<QString, double> bestValues() const;
    /// Its measurements, in the order of the goals, and its cost.
    const QVector<double>& bestMeasurements() const { return a_bestMeasured; }
    double bestCost() const { return a_bestCost; }
    /// Simulations run so far.
    int simulations() const { return a_simulations; }
    /// Simulations at a time.
    int parallel() const { return int(a_workers.size()); }

    /// The prefix of the optimized simulation's results in the dataset
    /// ("ac"), by which a goal is found among several of its name.
    static QString analysisOf(const Schematic* schematic, const QString& simulation);

signals:
    /// A line for the console.
    void message(const QString& line);
    void progress(int percent);
    /// The run is over: \a ok when it has a best point (the start at
    /// least), false when it failed - message() said why.
    void finished(bool ok);

private:
    struct Worker {
        Ngspice* kernel = nullptr;
        int candidate = -1;   // in a_batch; -1: idle
        QString dir;
    };

    void nextRound();
    void dispatch();
    void startOn(int worker, int candidate);
    void evaluated(int worker);
    void failedToStart(int worker);
    void roundDone();
    void finish(bool ok);
    bool goalsMet() const;
    QVector<double> measureDataset(const QString& file, QString* error) const;
    QString pointLine(const QVector<double>& x, const QVector<double>& measured) const;

    enum Phase { Idle, Start, Search, Stopping, Done };

    QPointer<Schematic> a_schematic;
    qucs_s::optimization::Problem a_problem;
    qucs_s::optimization::DifferentialEvolution a_de;
    QString a_analysis;
    QVector<double> a_scale;
    Phase a_phase = Idle;

    QList<qucs_s::optimization::DifferentialEvolution::Candidate> a_batch;
    QVector<double> a_costs;
    int a_next = 0;          // the next candidate of the batch to start
    int a_outstanding = 0;   // candidates simulating
    QVector<Worker> a_workers;

    QVector<double> a_bestX;
    QVector<double> a_bestMeasured;
    double a_bestCost;
    bool a_limitsOnly;       // no goal to minimize or maximize
    int a_simulations = 0;
    QString a_startFailure;
};

#endif
