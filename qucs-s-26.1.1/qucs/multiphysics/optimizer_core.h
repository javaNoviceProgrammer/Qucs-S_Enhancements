/*
 * optimizer_core.h - the optimizers themselves, whatever it is they optimize:
 *                    the variables of a search and what a coordinate of one
 *                    stands for, the goals and how a sweep is judged, and
 *                    the methods - differential evolution in its ten
 *                    variants, particle swarm, simulated annealing, CMA-ES,
 *                    Bayesian optimization, Nelder-Mead, a trust region,
 *                    Levenberg-Marquardt, NSGA-II and false position with
 *                    Broyden's method - with constraints by an augmented
 *                    Lagrangian, more starts and a polish. Qt Core and no
 *                    more: an optimization component (optimization.h), an
 *                    NgOpt block's method list (ngoptimize.h) and a
 *                    multiphysics model's optimization (fem_optimize.h)
 *                    read the same code.
 *
 * The searches over a cube are a port to C++ of the `optimize` command of the
 * Ngspice_OpenVAF_Enhancements fork of ngspice (src/frontend/com_optimize.c,
 * its changes under ngspice's own Modified BSD licence), one evaluation asked
 * of a callback where that command runs a circuit's analyses: the same steps,
 * the same stream of random numbers, the same stops and what they are called.
 * Where it differs is said where it does (a swarm's generation evaluated
 * together; the shared differential evolution, not that command's own).
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_OPTIMIZER_CORE_H
#define QUCS_OPTIMIZER_CORE_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <random>
#include <vector>

namespace qucs_s::optimization {

/// One variable of an optimization: "Var=Rload|yes|1000|10|1e4|LOG_DOUBLE".
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
    /// (In optimization.cpp, with the component's reading of a number: the
    /// schematic's side alone reads this form.)
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
/// Whether \a type is one of Variable's types.
bool knownVariableType(const QString& type);

enum class GoalType { Minimize, Maximize, LessEqual, GreaterEqual, Equal, Monitor };

/// One goal: "Goal=gain|GE|20" - a result of the simulation that is to be
/// as small or as large as possible, below, above or at a value, or only
/// shown.
struct Goal {
    QString name;
    GoalType type = GoalType::Monitor;
    double target = 0;   ///< for LE, GE and EQ

    /// (In optimization.cpp, as Variable::parse.)
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

/// The settings of a differential evolution: "DE=3|50|2|20|0.85|1|3|1e-6|10|100".
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

    /// (In optimization.cpp, as Variable::parse.)
    static Settings parse(const QString& value);
    /// "DE/rand-to-best/1/exp"
    QString methodName() const;
};

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

/// A number as the netlist and the component get it: 7 significant digits.
QString number(double value);

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

// ---------------------------------------------------------------------------
// The searches

/// A method: its name in a file and a command (the first eight as ngspice's
/// -method, nsga2's too, tune as the tool of that name searches), what it is
/// called and what it does. (Translated in "NgOpt", as an NgOpt block's.)
struct SearchMethod {
    const char* id;
    const char* name;
    const char* note;
    bool global;       ///< it searches the whole box (polished on request)
    bool population;   ///< it takes a population
    bool block;        ///< an NgOpt block offers it (a Nutmeg script has nsga2)
};
const QList<SearchMethod>& searchMethods();
/// The method \a id; null for one that is not.
const SearchMethod* searchMethod(const QString& id);

/// Why a search stopped, as ngspice's optimize_status says it.
enum class SearchStatus { Converged, MaxIter, Completed, NoSolve, Unchanged, Interrupted, Infeasible };
/// converged, maxiter, completed, nosolve, unchanged, interrupted, infeasible
QString statusWord(SearchStatus status);

/// One constraint: its value held at or above \a lower and at or below
/// \a upper, whichever it has.
struct SearchConstraint {
    QString name;
    bool hasLower = false, hasUpper = false;
    double lower = 0, upper = 0;
};

/// What is searched: the coordinates' names, the start, in the cube [0, 1]^n.
struct SearchProblem {
    QStringList names;
    std::vector<double> start;
    /// So many residuals a point (Levenberg-Marquardt and tune read them); 0:
    /// a scalar objective alone.
    int residuals = 0;
    /// The sum of squares of being wholly off the targets: what the first
    /// penalty is balanced against when they are met at the start.
    double wholeOff = 0;
    /// So many objectives a point (nsga2 reads them, the less the better).
    int objectives = 0;
    std::vector<SearchConstraint> constraints;
};

/// How it is searched.
struct SearchOptions {
    QString method;            ///< a searchMethods() id; empty: lm with residuals, else nm
    int maxIter = 100;         ///< iterations; generations; sa's cooling levels; bayes's and tune's evaluations
    double tol = 1e-6;
    int population = 0;        ///< of de, pso, cmaes and nsga2; 0: automatic
    unsigned long long seed = 1;
    bool polish = false;       ///< a global method's best point finished by a local one
    int starts = 0;            ///< more starts from Latin-hypercube points
    double ctol = 1e-4;        ///< how near its bound a constraint counts as met, relative to max(1, |bound|)
    double fdStep = 1e-3;      ///< Levenberg-Marquardt's finite difference, in the cube
    // Differential evolution's: its variant (1..10, Settings::method), the
    // weight of a difference, its crossover (0: 0.9, 15/n past 16 knobs) and
    // the cost variance it stops at (0: none).
    int strategy = 7;
    double F = 0.8;
    double CR = 0;
    double minCostVariance = 0;
};

/// What an evaluation found.
struct Evaluated {
    bool solved = false;              ///< false: it counts as the worst there is
    bool stopped = false;             ///< not evaluated: the search was asked to stop
    double objective = 0;             ///< the cost, constraints apart
    std::vector<double> residuals;    ///< problem.residuals of them
    std::vector<double> objectives;   ///< problem.objectives of them
    std::vector<double> constraints;  ///< each constraint's value
    bool goalsMet = false;            ///< nothing is left to improve: the search ends here
};

/// Who evaluates: a point of the cube is whatever its caller makes of it.
class SearchEvaluator {
public:
    virtual ~SearchEvaluator() = default;
    /// \a points, in their order; as many results (those it did not get to,
    /// asked to stop: stopped). Never asked for more than a method can use at
    /// once, and what is asked together does not depend on any result of
    /// them: the search is the same however many are evaluated at a time.
    virtual std::vector<Evaluated> evaluate(const std::vector<std::vector<double>>& points) = 0;
    /// The search is to end with the best point so far.
    virtual bool stopRequested() { return false; }
    /// After each iteration: \a fraction of the budget spent, the iteration
    /// (from 1) and the best cost.
    virtual void iterated(double fraction, int iteration, double best)
    {
        (void) fraction;
        (void) iteration;
        (void) best;
    }
    /// A line of what it does (a constraint round, a start, the polish).
    virtual void said(const QString& line) { (void) line; }
};

/// A constraint side at the optimum.
struct ConstraintState {
    int constraint = 0;
    bool upper = false;
    double bound = 0, value = 0;
    enum Kind { Violated, Active, AtBound, Slack } kind = Slack;
    double multiplier = 0;   ///< active: the objective's change for a unit of the bound
    double slack = 0;        ///< slack: how far inside; violated: how far outside
};

/// A design of a Pareto front: its coordinates, its objectives.
struct ParetoPoint {
    std::vector<double> x;
    std::vector<double> objectives;
    std::vector<double> constraints;
};

/// What a search found.
struct SearchOutcome {
    SearchStatus status = SearchStatus::NoSolve;
    /// What the report opens with: "converged", "stopped at 100 iterations --
    /// NOT converged", "cooling schedule complete (100 levels)"...
    QString phrase;
    std::vector<double> best;         ///< in the cube (empty under nsga2)
    double cost = 1e30;               ///< the objective there
    int evaluations = 0, failed = 0;
    bool feasible = true;             ///< with constraints: all met there
    double violation = 0;             ///< the largest, relative to its bound's size
    std::vector<ConstraintState> constraints;
    QStringList notes;                ///< a knob on a bound, an objective that never moved, failures...
    QStringList search;               ///< "start 2 of 3 won", "polish converged -- cost ...", a constraint's rounds
    std::vector<ParetoPoint> front;   ///< nsga2's, by its first objective
    bool settled() const { return status == SearchStatus::Converged || status == SearchStatus::Completed; }
};

/// Runs the method on \a problem. Blocking: \a evaluator is asked on the
/// caller's thread, at most once at a time.
SearchOutcome search(const SearchProblem& problem, const SearchOptions& options, SearchEvaluator& evaluator);

} // namespace qucs_s::optimization

#endif // QUCS_OPTIMIZER_CORE_H
