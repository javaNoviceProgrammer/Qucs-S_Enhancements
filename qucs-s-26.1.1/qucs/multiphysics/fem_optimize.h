/*
 * fem_optimize.h - a multiphysics model's optimization: the parameters a
 *                  node of the model varies and over which ranges, what it
 *                  asks of the results (objectives, targets, limits), any of
 *                  the optimizers (optimizer_core.h), each evaluation a whole
 *                  solve; what it found, kept in the node, and the optimum
 *                  written into the parameters
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_OPTIMIZE_H
#define QUCS_FEM_OPTIMIZE_H

#include "fem_results.h"
#include "optimizer_core.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace qucs_s::fem {

/// One row of an optimization's objectives: an expression of a solution's
/// global values, parameters and Derived Values nodes' values (es.C11,
/// ht.Tmax - 300[K], lp1.Z0), and what is asked of it.
struct OptimizationObjective {
    enum Kind {
        Minimize,      ///< MIN: as small as it can be
        Maximize,      ///< MAX: as large
        Target,        ///< TARGET: at a value, the square of its miss added (least squares)
        LessEqual,     ///< LE: a limit, at or below a value
        GreaterEqual,  ///< GE: at or above
        Equal,         ///< EQ: at a value, a limit from both sides
        Monitor        ///< MON: only shown
    };
    QString expression;
    Kind kind = Monitor;
    QString valueText;      ///< of TARGET, LE, GE, EQ: read once its unit is known
    double value = 0;       ///< (SI)
    double weight = 1;      ///< how much it counts among the others
    bool last = false;      ///< its last value, not its worst over the study's sweep, times or frequencies
    QString study;          ///< the study it is read from; empty: the optimization's

    bool isLimit() const { return kind == LessEqual || kind == GreaterEqual || kind == Equal; }
    bool isGoal() const { return kind == Minimize || kind == Maximize || kind == Target; }
    static QString kindName(Kind kind);
    /// optimization::Goal's type: how a series is judged by its worst.
    optimization::GoalType goalType() const;
};

/// A parameter's spread about its value in design centering.
struct OptimizationVariation {
    QString parameter;
    bool uniform = false;       ///< between value +- spread; else a Gaussian of that sigma
    double spread = 0;          ///< SI, or relative when \a relative
    bool relative = false;      ///< a share of the value ("5 %")
};

/// An optimization node read and checked: everything a run needs.
struct OptimizationPlan {
    QString tag, name, study;
    bool center = false;                        ///< design centering: the worst Cpk of the limits, over samples
    optimization::SearchOptions search;
    bool penalty = false;                       ///< limits as weighted penalties, not an augmented Lagrangian
    double objectiveWeight = 10, constraintWeight = 100;
    int parallel = 1;                           ///< solves at once
    int refresh = 0;                            ///< a line every so many iterations; 0: none
    int samples = 100;
    bool lhs = false;
    std::vector<optimization::Variable> variables;   ///< named by their parameters, in SI
    std::vector<Dim> dims;                           ///< each one's unit
    std::vector<OptimizationObjective> objectives;
    std::vector<OptimizationVariation> variation;
    QStringList stages;                         ///< the studies solved for each candidate, in order
    QStringList warnings;

    /// Its active variables' places in \a variables.
    std::vector<int> active() const;
};

/// \a node of \a model, its values read in \a parameters (the model's own);
/// none, and why in \a error, where it cannot be run: a parameter the model
/// has not, a range that is not one, nothing asked, a method that cannot take
/// what is asked.
std::optional<OptimizationPlan> optimizationPlan(const Model& model, const Node& node, const ParameterScope& parameters, QString* error);

/// The optimization to run or read: \a tag, or (empty) the model's first; null
/// where there is none.
const Node* optimizationNode(const Model& model, const QString& tag);
std::vector<const Node*> optimizationNodes(const Model& model);

/// One solve of a run.
struct OptimizationRow {
    int index = 0;                 ///< from 1
    std::vector<double> values;    ///< the varied parameters' (SI)
    std::vector<double> measured;  ///< by objective (SI; NaN: not measured)
    double cost = std::numeric_limits<double>::quiet_NaN();   ///< the objective, limits apart (NaN: it did not solve)
    bool feasible = true;          ///< every limit met
    bool best = false;             ///< the best so far
    QString why;                   ///< why it did not solve
};

/// What a run found: what the node keeps ("result") and every view reads.
struct OptimizationResult {
    QString method;                 ///< the method it ran
    QString status;                 ///< optimization::statusWord's
    QString summary;                ///< "converged, objective = 1.2e-13 after 29 evaluations"
    QString error;                  ///< it could not be run at all: why (nothing else is set)
    QString setup;                  ///< what it was of (optimizationSetup): the model changed since?
    bool settled = false;           ///< ended on its own criterion or schedule
    bool feasible = true;
    int evaluations = 0, solves = 0, failed = 0;
    double seconds = 0;
    /// A named value, at the optimum: a varied parameter's, an objective's.
    struct Value {
        QString name;
        double value = std::numeric_limits<double>::quiet_NaN();
        QString text;
    };
    QList<Value> parameters;        ///< empty: no best point
    QList<Value> measured;
    QStringList kinds;              ///< each objective's, MIN...
    QStringList units;              ///< each column's unit: the parameters', then the objectives'
    QStringList notes;              ///< a parameter on a bound, failures, an iteration cap...
    QStringList search;             ///< a constraint's rounds, the winning start, the polish
    QStringList constraints;        ///< each limit at the optimum: active, its multiplier, its slack
    double yield = std::numeric_limits<double>::quiet_NaN();   ///< design centering's
    double cpk = std::numeric_limits<double>::quiet_NaN();
    /// A Pareto front's designs, by its first objective.
    struct FrontPoint {
        std::vector<double> values;
        std::vector<double> measured;
    };
    std::vector<FrontPoint> front;
    QStringList columns;            ///< the varied parameters' names, then the objectives'
    std::vector<OptimizationRow> history;

    bool hasBest() const { return !parameters.isEmpty(); }
    /// Its values may be written in: not where nothing solved, or a limit
    /// could not be met (as apply_optimum on a schematic).
    bool canApply(QString* why = nullptr) const;
    /// As a node keeps it: at most \a rows of its history, a row a line.
    QJsonObject toJson(int rows = 1000) const;
    /// (Whatever a file holds: what it does not hold is left out.)
    static OptimizationResult fromJson(const QJsonObject& object);
};

/// What a run tells as it goes, on its thread.
struct OptimizationListener {
    std::function<void(const OptimizationRow&, const QStringList& columns)> row;
    std::function<void(const QString&)> line;
};

/// Runs \a tag's optimization: \a topology and \a mesh are of the model's own
/// parameters, either null when not built. Blocking; \a progress false stops
/// it, with the best point so far.
OptimizationResult runOptimization(const Model& model, const QString& tag, std::shared_ptr<ParameterScope> parameters,
                                   std::shared_ptr<const Topology> topology, std::shared_ptr<const Mesh> mesh, const Progress& progress = {},
                                   const OptimizationListener& listener = {});

/// What a result depends on, as a text: everything of the model but the varied
/// parameters' own expressions and the result itself. A result whose setup is
/// not this is of the model before a change.
QString optimizationSetup(const Model& model, const Node& node);

/// \a value of parameter \a name as its expression writes it: in the unit it has
/// (2.5[mm] for one of 3[mm], not 0.0025[m]), else in SI.
QString parameterText(const Model& model, const ParameterScope& parameters, const QString& name, double value);

/// A row of an optimization's parameters to vary for \a name: over a factor of
/// two each way about its value, in its unit.
QJsonArray optimizationVariableRow(const Model& model, const ParameterScope& parameters, const QString& name);

/// The optimum of \a node's result (a Pareto front: its design \a point, from
/// 1) written into the model's parameters - each in the unit its expression
/// has -; what it wrote ("w = 2.5[mm]") in \a written. False and why where it
/// cannot.
bool applyOptimum(Model& model, const Node& node, int point, QStringList* written, QString* error);

} // namespace qucs_s::fem

#endif // QUCS_FEM_OPTIMIZE_H
