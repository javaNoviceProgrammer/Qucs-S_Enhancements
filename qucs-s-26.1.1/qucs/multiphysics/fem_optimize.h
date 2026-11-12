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
    /// Of a mode analysis, the mode read: from 1; 0 the one its step chose, else
    /// the first (as fem_evaluate and a solve's globals read it); -1 every mode,
    /// by its worst.
    int mode = 0;

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

/*!
 * The pieces of an optimization node another kind of model reads its plan by
 * (RF 2.5D's), as optimizationPlan() reads a multiphysics one's: each false and
 * why in \a error where the node cannot be run. Its rows' own columns past
 * the first four of an objective (expression, kind, value, weight) are the
 * caller's.
 */
/// Its mode (optimize, center), method and every setting of the search.
bool readOptimizationSettings(const Node& node, const ParameterScope& parameters, OptimizationPlan* plan, QString* error);
/// Its parameters to vary (name, start, minimum, maximum, type, active), each
/// one of \a known (a model's parameters' names, in order: said where one is
/// not).
bool readOptimizationVariables(const Node& node, const ParameterScope& parameters, const QStringList& known, OptimizationPlan* plan, QString* error);
/// An objective row's first four cells - its expression, kind, value, weight -
/// into \a objective; false where the row is to be skipped (no expression)
/// with \a error empty, or is wrong with \a error set.
bool readOptimizationObjective(const QJsonArray& row, const ParameterScope& parameters, const QString& planName, OptimizationObjective* objective, QString* error);
/// The objectives read into \a plan ask something to reach, and within the
/// counts a search holds (targets, limits).
bool checkOptimizationObjectives(OptimizationPlan* plan, QString* error);
/// What the objectives read into \a plan ask of its method: what the method
/// can take (design centering's spreads read here), and the warnings its
/// choice deserves. \a evaluate gives the
/// parameters with overrides (a model's own evaluateParameters()), to see
/// that no value of a target or a limit moves with a parameter varied.
bool checkOptimizationPlan(const Node& node, const ParameterScope& parameters,
                           const std::function<std::shared_ptr<ParameterScope>(const QHash<QString, QString>&)>& evaluate, OptimizationPlan* plan,
                           QString* error);


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

/// An expression of \a solution as it stands (its snapshot selected): its globals
/// and parameters, and the values of its Derived Values nodes it names (lp1.Z0; a
/// node of one value by its tag). False and why where it is not one number.
bool solutionValue(Solution& solution, const QString& text, double* value, Dim* dim, QString* why);


/// What a run tells as it goes, on its thread.
struct OptimizationListener {
    std::function<void(const OptimizationRow&, const QStringList& columns)> row;
    std::function<void(const QString&)> line;
};
/// What one candidate's solve measured: each objective's value, SI, and its
/// unit; why it did not solve.
struct OptimizationMeasured {
    bool solved = false;
    bool stopped = false;     ///< stopped before it had an answer: nothing is known of it
    bool fatal = false;       ///< solved, but what is asked cannot be read: no candidate's could be
    QString why;
    std::vector<double> y;    ///< by objective (a sweep's points already judged by the worst, or the last)
    std::vector<Dim> dims;
    QStringList warnings;     ///< what the solve said, told once in the result's notes
    double yield = std::numeric_limits<double>::quiet_NaN(), cpk = std::numeric_limits<double>::quiet_NaN();
};
/// A candidate to solve: the model's parameters with \a overrides in place of
/// their expressions (SI values with their units); \a progress how far its
/// solve has got (false: stop); \a stopped whether the run was asked to stop.
struct OptimizationCandidate {
    QHash<QString, QString> overrides;
    Progress progress;
    std::function<bool()> stopped;
};
/// Solves a candidate and measures the plan's objectives on it. Called on
/// worker threads, several at once where the plan solves several at once.
using OptimizationSolver = std::function<OptimizationMeasured(const OptimizationCandidate&)>;
/// Runs \a plan, each candidate by \a solve: \a parameters the model's own.
/// \a lastNotes, after the run, adds what only the solver knows to the notes
/// (each candidate meshed again...). Blocking; \a progress false stops it, with
/// the best point so far.
OptimizationResult runOptimizationPlan(const OptimizationPlan& plan, std::shared_ptr<ParameterScope> parameters, const OptimizationSolver& solve,
                                       const Progress& progress = {}, const OptimizationListener& listener = {},
                                       const std::function<QStringList()>& lastNotes = {});
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
QString parameterText(const QList<Model::Parameter>& rows, const ParameterScope& parameters, const QString& name, double value);

/// A row of an optimization's parameters to vary for \a name: over a factor of
/// two each way about its value, in its unit.
QJsonArray optimizationVariableRow(const Model& model, const ParameterScope& parameters, const QString& name);
QJsonArray optimizationVariableRow(const QList<Model::Parameter>& rows, const ParameterScope& parameters, const QString& name);

/// The optimum of \a node's result (a Pareto front: its design \a point, from
/// 1) written into the model's parameters - each in the unit its expression
/// has -; what it wrote ("w = 2.5[mm]") in \a written. False and why where it
/// cannot.
bool applyOptimum(Model& model, const Node& node, int point, QStringList* written, QString* error);
/// The same into \a rows, a model's parameters (\a parameters, theirs
/// evaluated): \a plan, where the node's plan can be read, holds the
/// parameters it held at a start of their own, written in too.
bool applyOptimum(QList<Model::Parameter>& rows, const ParameterScope& parameters, const Node& node, const OptimizationPlan* plan, int point,
                  QStringList* written, QString* error);

} // namespace qucs_s::fem

#endif // QUCS_FEM_OPTIMIZE_H
