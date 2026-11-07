/*
 * fem_adapt.h - a 2D multiphysics model's mesh convergence check: values it
 *               watches, a mesh refined where the error is until they have
 *               settled or a cap is reached; each value's estimated limit and
 *               error from the rate it was seen to converge at, and how far to
 *               trust them; one more refinement that the same error indicator
 *               did not choose, against two meshes that happen to agree; what
 *               it found, kept in its node
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_ADAPT_H
#define QUCS_FEM_ADAPT_H

#include "fem_results.h"

#include <QJsonArray>
#include <QJsonObject>

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace qucs_s::fem {

// ---------------------------------------------------------------- The mesh

/// \a mesh with each triangle \a marked (and what keeping it conforming asks)
/// cut by its longest edge: its old nodes and their numbers as they were, so
/// a coarser mesh's triangles are unions of the new one's. A new node on an arc
/// goes on it. \a periodic: the sides of a periodic condition made the same node
/// for node again. Null where \a marked is not the triangles' size.
std::shared_ptr<Mesh> bisectMesh(const Mesh& mesh, const std::vector<char>& marked, const std::vector<MeshSizes::Periodic>& periodic = {});

/// What the check refines by, for one mesh: each triangle's indicator, and what
/// it was found from ("Zienkiewicz-Zhu of es", "recovered E of emw").
struct RefinementIndicator {
    std::vector<double> eta;   ///< by triangle; empty: no field gives one
    QStringList fields;        ///< the fields it was found from, in words
    QStringList without;       ///< and those solved that give none, and why
};
/// The indicator of \a solution as it stands (its snapshot selected): each solved
/// field's or \a physics' (their tags; empty: all), each brought to the same
/// total (they are of different units) - nodal fields by the Zienkiewicz-Zhu
/// recovery of each component's gradient (a semiconductor's V, log n and log p),
/// edge elements' by the recovery of the electric field; none in an absorbing
/// layer.
RefinementIndicator refinementIndicator(const Solution& solution, const QStringList& physics = {});

/// \a goal's triangles, the least that hold \a share of its square and at least
/// \a least of them.
std::vector<char> markBulk(const std::vector<double>& eta, double share = 0.5, double least = 0.35);

// --------------------------------------------------------------- The estimate

/// How far a value's estimate can be believed.
struct ValueEstimate {
    enum Trust {
        Few,            ///< one mesh: nothing to compare
        Unmoved,        ///< it did not move at all (exact on any mesh, or not of the mesh)
        Extrapolated,   ///< a steady rate: the limit extrapolated
        Bounded,        ///< no rate to believe: the error bounded from the last change, with room
        Erratic         ///< it jumps about: the error bounded by its jumps, the limit not known
    };
    Trust trust = Few;
    double value = std::numeric_limits<double>::quiet_NaN();      ///< on the finest mesh
    double limit = std::numeric_limits<double>::quiet_NaN();      ///< as the mesh goes to nought
    double error = std::numeric_limits<double>::infinity();       ///< of value (absolute), with a safety factor
    double rate = std::numeric_limits<double>::quiet_NaN();       ///< q of error ~ unknowns^-q (NaN: none)
    bool steady = false;                                          ///< the same rate on the meshes before
    QString why;                                                  ///< in words
    static QString trustWord(Trust trust);
    static std::optional<Trust> trustOf(const QString& word);
};

/// \a values on meshes of \a unknowns (ascending; of those closer than 1.4 times,
/// the finer): the last three as Q = limit + C unknowns^-q, on elements
/// of \a order. Believed where the changes shrink with one sign at a rate within
/// reason for the order, and with room (1.25, Roache's for three meshes); else
/// bounded from the changes, with more (3).
ValueEstimate estimateValue(const std::vector<double>& unknowns, const std::vector<double>& values, int order);

// --------------------------------------------------------------------- The node

/// One value watched: as an optimization's objective, an expression of the
/// results' global values and Derived Values nodes.
struct ConvergenceWatch {
    QString expression;
    double tolerance = 0;      ///< relative; 0: the check's
    QString absoluteText;      ///< below this it counts as settled, however small (a value near 0)
    double absolute = 0;
};

/// A convergence node read and checked: everything a run needs.
struct ConvergencePlan {
    QString tag, name, study;
    std::vector<ConvergenceWatch> values;
    double tolerance = 0.01;
    int maxMeshes = 12;         ///< solves at most (of each point of a sweep)
    double maxUnknowns = 2e6;   ///< no mesh is solved past it
    QString refine = QStringLiteral("auto");   ///< auto, goal, uniform
    QStringList physics;        ///< the fields it refines by (empty: every one)
    bool check = true;          ///< the refinement the indicator does not choose
    QString sweep = QStringLiteral("own");   ///< of a study's sweep: own, first, last, ends, all
    QStringList warnings;
};

/// \a node of \a model, its values read in \a parameters; none, and why in \a
/// error, where it cannot be run.
std::optional<ConvergencePlan> convergencePlan(const Model& model, const Node& node, const ParameterScope& parameters, QString* error);
/// The check to run or read: \a tag, or (empty) the model's first; null where
/// there is none.
const Node* convergenceNode(const Model& model, const QString& tag);
std::vector<const Node*> convergenceNodes(const Model& model);

/// One mesh of a run.
struct ConvergenceRow {
    int index = 0;           ///< from 1
    int point = 0;           ///< its sweep point in the result's points (from 0)
    QString kind;            ///< start, adaptive, goal, uniform, check
    int triangles = 0;
    double unknowns = 0;
    double smallest = 0;     ///< the smallest element's size (the geometry's unit)
    double seconds = 0;      ///< its mesh and its solve
    double indicator = std::numeric_limits<double>::quiet_NaN();   ///< the indicator's total, on the field brought to 1 (NaN: none)
    std::vector<double> values;   ///< by column (SI; NaN: not read)
};

/// What a run found: what the node keeps ("result") and every view reads.
struct ConvergenceResult {
    QString status;          ///< converged, unknowns, meshes, projected, stalled, failed, cancelled
    bool converged = false;
    bool falseConvergence = false;   ///< values that agreed were moved by the check: caught
    QString summary;
    QString error;           ///< it could not be run at all: why (nothing else is set)
    QString setup;           ///< what it was of (convergenceSetup): the model changed since?
    QString method;          ///< what refined the mesh, in words
    QStringList evidence;    ///< what stands behind the verdict, each a line
    QStringList notes;
    QStringList columns;     ///< the values watched
    QStringList units;       ///< each one's unit
    QStringList points;      ///< a sweep's points checked ("w = 2 mm"); empty: the model's own
    std::vector<ConvergenceRow> rows;
    /// Each value on the finest mesh (of each point).
    struct Final {
        int point = 0;
        int column = 0;
        double value = std::numeric_limits<double>::quiet_NaN();
        double limit = std::numeric_limits<double>::quiet_NaN();
        double error = std::numeric_limits<double>::infinity();
        double relative = std::numeric_limits<double>::infinity();
        double rate = std::numeric_limits<double>::quiet_NaN();
        double tolerance = 0;
        QString trust;
        QString why;
        bool settled = false;
    };
    std::vector<Final> values;
    int solves = 0;
    int adjoints = 0;            ///< goal-oriented refinement's second solves, on the same meshes
    double unknownsSolved = 0;   ///< summed over the solves, the adjoints' too
    double largest = 0;          ///< the largest mesh's unknowns
    double seconds = 0;
    /// The refined mesh of the last series run, while its run is in memory.
    std::shared_ptr<const Mesh> mesh;
    /// The sizes that mesh asks for, "x y h" (the geometry's unit): what Keep
    /// Refined Mesh writes in. Empty where nothing was refined.
    QStringList sizes;
    double smallestSize = 0;

    /// As a node keeps it.
    QJsonObject toJson() const;
    /// (Whatever a file holds: what it does not hold is left out.)
    static ConvergenceResult fromJson(const QJsonObject& object);
};

/// What a run tells as it goes, on its thread.
struct ConvergenceListener {
    std::function<void(const ConvergenceRow&, const QStringList& columns, const QStringList& points)> row;
    std::function<void(const QString&)> line;
};

/// Runs \a tag's check: \a topology and \a mesh are of the model's own parameters,
/// either null when not built. Blocking; \a progress false stops it, with what
/// it has. The model and its mesh node are not touched.
ConvergenceResult runConvergence(const Model& model, const QString& tag, std::shared_ptr<ParameterScope> parameters,
                                 std::shared_ptr<const Topology> topology, std::shared_ptr<const Mesh> mesh, const Progress& progress = {},
                                 const ConvergenceListener& listener = {});

/// What a result depends on, as a text: everything of the model but the result. A
/// result whose setup is not this is of the model before a change.
QString convergenceSetup(const Model& model, const Node& node);

/// The refined mesh of \a node's result written into the model's mesh node as
/// an Adapted Sizes feature (one is replaced): built, it gives about that mesh
/// again - about, as it is meshed anew. False and why where it cannot.
bool keepRefinedMesh(Model& model, const Node& node, QString* error);

/// A check's next row of the "values" table, for what \a solved has.
QJsonArray convergenceValueRow(const Solution* solved, const QStringList& used);

} // namespace qucs_s::fem

#endif // QUCS_FEM_ADAPT_H
