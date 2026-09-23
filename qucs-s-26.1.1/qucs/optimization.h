/*
 * optimization.h - what an optimization component (.Opt) asks for, and the
 * search that answers it with a SPICE simulator: its variables, goals and
 * settings as the component stores them, the goals measured in a Qucs
 * dataset, the cost of a candidate, differential evolution over the
 * variables, and the netlist of one candidate
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

#include <random>

class Component;
class Schematic;

namespace qucs_s::optimization {

/// One variable of the optimization: "Var=Rload|yes|1000|10|1e4|LOG_DOUBLE".
struct Variable {
    QString name;
    bool active = true;     ///< "no": held at its initial value
    double initial = 0;
    double min = 0;
    double max = 0;
    /// LIN_DOUBLE, LOG_DOUBLE, LIN_INT, LOG_INT, or a series of preferred
    /// values: E3, E6, E12, E24, E48, E96, E192.
    QString type = QStringLiteral("LIN_DOUBLE");

    /// Reads the property value; false and why in \a error when it is not
    /// one (fewer fields, a bound that is not a number, min above max).
    static bool parse(const QString& value, Variable* variable, QString* error = nullptr);
    /// The property value, the initial value written with \a precision
    /// significant digits.
    QString toString(int precision = 7) const;

    /// Searched on a logarithmic scale: the LOG types and the series,
    /// while both bounds are above zero.
    bool logarithmic() const;
    /// The search runs over coordinates: the value itself, or its
    /// logarithm. These are the coordinates' bounds.
    double lower() const;
    double upper() const;
    double coordinate(double value) const;
    /// The value a coordinate stands for: rounded for the integer types,
    /// the nearest preferred value for a series, always within the bounds.
    double value(double coordinate) const;
};

/// The preferred values of an E series (E3 ... E192) in one decade,
/// 1 <= v < 10; empty for any other name.
QList<double> eSeries(const QString& name);
/// The value of the series nearest to \a value (on a logarithmic scale).
double nearestInSeries(const QList<double>& series, double value);

enum class GoalType { Minimize, Maximize, LessEqual, GreaterEqual, Equal, Monitor };

/// One goal: "Goal=gain|GE|20" - a result of the simulation that is to be
/// as small or as large as possible, below, above or at a value, or only
/// shown.
struct Goal {
    QString name;
    GoalType type = GoalType::Monitor;
    double target = 0;   ///< for LE, GE and EQ

    static bool parse(const QString& value, Goal* goal, QString* error = nullptr);
    /// "MIN", "LE", ...
    static QString typeName(GoalType type);
    bool isConstraint() const;
    bool isObjective() const;
    /// A constraint that \a measured satisfies (a relative 1e-9 of slack
    /// for EQ).
    bool met(double measured) const;
    /// "gain = 20.13 (>= 20, met)"
    QString describe(double measured) const;
};

/// The settings of the search: "DE=3|50|2|20|0.85|1|3|1e-6|10|100".
struct Settings {
    int method = 3;              ///< 1..10: DE/best/1/exp ... DE/rand/2/bin
    int generations = 50;
    int refresh = 2;             ///< a progress line every so many generations
    int population = 20;
    double F = 0.85;             ///< the weight of a difference vector
    double CR = 1;               ///< the crossover probability
    int seed = 3;
    double minCostVariance = 1e-6;  ///< the population is taken as converged below it
    double objectiveWeight = 10;
    double constraintWeight = 100;

    static Settings parse(const QString& value);
    /// "DE/rand-to-best/1/exp"
    QString methodName() const;
};

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

/// The number a goal is judged by. A scalar is itself; a sweep counts by
/// its worst point: its largest value to minimize, for LE and to monitor,
/// its smallest to maximize and for GE, for EQ the one farthest from the
/// target. NaN for no values.
double measure(const Goal& goal, const QVector<double>& values);

/// The cost of a candidate from its measurements (in the order of the
/// goals; NaN: missing): the objectives relative to \a scale (the
/// measurements at the start), weighted by the settings' objective weight,
/// less for MAX; plus each constraint's shortfall relative to its target
/// (to 1 for a target of 0), weighted by the constraint weight. Infinite
/// when a goal was not measured.
double cost(const QList<Goal>& goals, const QVector<double>& measured, const QVector<double>& scale,
            const Settings& settings);
/// The scale cost() takes, from the measurements at the start: their
/// magnitude, 1 where that is 0 or missing.
QVector<double> scaleOf(const QVector<double>& measured);

/// Differential evolution (Storn and Price) over a box: asks for
/// candidates, is told their costs. The first candidate given is kept in
/// the population as its first member.
class DifferentialEvolution {
public:
    struct Candidate {
        int member = 0;           ///< the member of the population it would replace
        QVector<double> x;
    };

    DifferentialEvolution(const QVector<double>& lower, const QVector<double>& upper, const Settings& settings);

    /// Starts with \a start, whose cost is known, as the first member and
    /// the rest of the population drawn at random within the bounds.
    void start(const QVector<double>& start, double startCost);
    /// The candidates to evaluate now: the random members first, then a
    /// trial for every member each generation.
    QList<Candidate> next();
    /// The costs of next()'s candidates, in its order. A trial replaces its
    /// member when it costs no more.
    void report(const QVector<double>& costs);

    /// Generations completed (the random members are generation 0).
    int generation() const { return a_generation; }
    bool finished() const;
    bool converged() const;
    double costVariance() const;
    const QVector<double>& best() const { return a_population.at(a_best); }
    double bestCost() const { return a_cost.at(a_best); }
    int populationSize() const { return a_np; }

private:
    double uniform(double lo, double hi);
    int pick(QList<int>* taken);
    QVector<double> trial(int i);
    void updateBest();

    QVector<double> a_lower, a_upper;
    Settings a_settings;
    int a_np = 0;
    int a_dim = 0;
    int a_generation = -1;   // -1: the random members are still to be evaluated
    int a_best = 0;
    QVector<QVector<double>> a_population;
    QVector<double> a_cost;
    QList<Candidate> a_pending;
    std::mt19937 a_rng;
};

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

/// A number as the netlist and the component get it: 7 significant digits.
QString number(double value);

} // namespace qucs_s::optimization

#endif
