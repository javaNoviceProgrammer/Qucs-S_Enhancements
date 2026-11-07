/*
 * optimization.h - what an optimization component (.Opt) asks for, and the
 * search that answers it with a SPICE simulator: its variables, goals and
 * settings as the component stores them, the goals measured in a Qucs
 * dataset and the netlist of one candidate. The variables, goals, cost and
 * differential evolution themselves are shared (optimizer_core.h).
 *
 * With qucsator an optimization is ASCO's; with ngspice Qucs runs it
 * itself (SimulationRun, extsimkernels/optimizer.h) from these pieces.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_OPTIMIZATION_H
#define QUCS_OPTIMIZATION_H

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include "multiphysics/optimizer_core.h"

class Component;
class Schematic;

namespace qucs_s::optimization {

/// The optimization component's variables, goals and settings; false and
/// the first thing wrong in \a error when it cannot be run.
struct Problem {
    QString component;    ///< the component's name, "Opt1"
    QString simulation;   ///< the simulation it optimizes, "AC1"; empty: all
    Settings settings;
    QList<Variable> variables;
    QList<Goal> goals;

    static bool read(const Component* optimization, Problem* problem, QString* error = nullptr);
    /// The search runs over one coordinate for each active variable, in
    /// their order; these are the bounds and the start.
    QVector<double> lower() const;
    QVector<double> upper() const;
    QVector<double> initialCoordinates() const;
    /// The values of all the variables at \a coordinates, by name: the
    /// inactive ones at their initial values.
    QMap<QString, double> values(const QVector<double>& coordinates) const;
};

/// The variables of a Qucs dataset (the .dat Qucs writes and reads), by
/// name as the file has them, each with its values. A complex variable
/// counts by its magnitude, unless all its imaginary parts are zero, when
/// it is the real part. An error and an empty table if the file cannot be
/// read.
QHash<QString, QVector<double>> readDataset(const QString& file, QString* error = nullptr);

/// The name in \a names a goal means: its own (in any case) or, the
/// simulator having put the analysis before it, "ac.gain" for "gain" - the
/// one of \a analysis ("ac", "tran", "dc", ...) when there are several;
/// failing that, its voltage or current: "ac.v(vo)" for "vo". Empty when
/// there is none, or several and none of them the analysis'.
QString resolve(const QStringList& names, const QString& goal, const QString& analysis = QString());
/// The prefix ngspice gives the results of a simulation component's
/// analysis in the dataset: "ac" for .AC, "tran" for .TR, "dc" for .DC and
/// .SW; empty for the others.
QString analysisPrefix(const QString& model);

/// For as long as it lives, the schematic netlists as one candidate of an
/// optimization: every parameter definition of a variable (in an equation,
/// a .PARAM or a .GLOBAL_PARAM component, any case) carries the
/// candidate's value, the simulations other than the optimized one (and
/// what it runs on, and its Fourier analyses) are left out unless
/// \a allSimulations, and the optimization component is. Everything goes
/// back as it was when it ends.
/// \c undefined() names the variables nothing defines: the netlist needs
/// a .PARAM line for them (AbstractSpiceKernel::setExtraParameters).
class NetlistScope {
public:
    NetlistScope(Schematic* schematic, const Problem& problem, const QMap<QString, double>& values,
                 bool allSimulations = false);
    ~NetlistScope();
    NetlistScope(const NetlistScope&) = delete;
    NetlistScope& operator=(const NetlistScope&) = delete;

    const QStringList& undefined() const { return a_undefined; }
    /// ".PARAM name=value" for each of the undefined variables.
    QString extraParameters() const;

private:
    struct Saved {
        Component* component;
        int property;          // -1: isActive
        QString value;
        int active;
    };
    QList<Saved> a_saved;
    QStringList a_undefined;
    QMap<QString, double> a_values;
};

/// The simulations an optimization of \a simulation runs: it, the one it
/// sweeps (and so on down), and the Fourier analyses of those. Names in
/// lower case. Every simulation when \a simulation is empty or not there.
QStringList simulationsOf(const Schematic* schematic, const QString& simulation);

} // namespace qucs_s::optimization

#endif
