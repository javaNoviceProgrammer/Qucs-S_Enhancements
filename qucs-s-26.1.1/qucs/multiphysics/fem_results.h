/*
 * fem_results.h - what a multiphysics solution shows: an expression's
 *                 values over the domains (a color map), its contours, a
 *                 vector field's arrows, a value at a point, integrals,
 *                 averages, maxima over domains or boundaries, global
 *                 values, a transmission line's quasi-TEM parameters
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_RESULTS_H
#define QUCS_FEM_RESULTS_H

#include "fem_solver.h"

#include <QLineF>

#include <optional>
#include <vector>

namespace qucs_s::fem {

/// The triangle a point is in and where in it, on a grid of the mesh's
/// triangles. Points in the geometry's unit.
class Locator
{
public:
    explicit Locator(const Mesh& mesh);
    /// False when \a p is in no triangle.
    bool locate(QPointF p, int* t, double* xi, double* eta) const;

private:
    const Mesh& a_mesh;
    QRectF a_box;
    int a_nx = 1, a_ny = 1;
    double a_cw = 1, a_ch = 1;
    std::vector<std::vector<int>> a_cells;
};

/// A unit a value is shown in: shown = (SI - offset) / scale.
struct DisplayUnit {
    double scale = 1, offset = 0;
    QString name;   ///< as shown ("mV", "degC"); empty: SI's
};
/// \a text for values of \a dim: empty, SI's own; one not of that unit,
/// SI and why in \a warning.
DisplayUnit displayUnit(const QString& text, const Dim& dim, QString* warning = nullptr);

/// Where a plot is drawn: each point moved by a displacement (x, y: its
/// expressions, in metres) times a scale.
struct Deformation {
    QString x, y;
    double scale = 1;        ///< 0: automatic - the largest a tenth of the model's size
    double used = 0;         ///< the scale it was drawn with
    double largest = 0;      ///< the largest displacement (m)
};

/// An expression over the domains: each element in triangles (in four for
/// a quadratic field), its value at their corners. Points in the
/// geometry's unit (moved, deformed), values as shown.
struct SurfaceData {
    std::vector<QPointF> points;   ///< three a triangle
    std::vector<double> values;    ///< one a point (NaN: not defined there)
    double min = 0, max = 0;       ///< of those defined
    Dim dim;
    DisplayUnit unit;
    QString error, warning;
};
SurfaceData surfaceData(const Solution& solution, const QString& expression, const QString& unit = {}, Deformation* deformation = nullptr);

/// An expression along a line from \a a to \a b (the geometry's unit), at
/// \a count points: where (and how far along, in metres), and its value
/// (SI; NaN outside the domains).
struct LineData {
    std::vector<QPointF> points;
    std::vector<double> along;
    std::vector<double> values;
    Dim dim;
    QString error;
};
LineData lineData(const Solution& solution, const Locator& locator, const QString& expression, QPointF a, QPointF b, int count);

/// The lines where a surface's values are each of \a levels values,
/// evenly between its least and greatest.
struct ContourData {
    std::vector<double> levels;
    std::vector<std::vector<QLineF>> lines;   ///< by level
};
ContourData contourData(const SurfaceData& surface, int levels);

/// Arrows of a vector field on a grid over \a region (the geometry's
/// unit): where, and the vector (SI).
struct ArrowData {
    std::vector<QPointF> at;
    std::vector<QPointF> vector;
    double longest = 0;
    QString error;
};
ArrowData arrowData(const Solution& solution, const QString& x, const QString& y, const QRectF& region, int nx, int ny);

/// \a expression at \a p (the geometry's unit), SI; its unit in \a dim.
std::optional<double> evaluateAt(const Solution& solution, const Locator& locator, const QString& expression, QPointF p,
                                 QString* error = nullptr, Dim* dim = nullptr);

/// A derived value found: its name, value (SI) and how it is shown.
struct DerivedValue {
    QString name;
    double value = 0;
    Dim dim;
    QString text;   ///< as shown, with its unit
};
struct DerivedResult {
    QList<DerivedValue> values;
    QString error;
};
/// Evaluates a Derived Values node (global, integral, average, maximum,
/// minimum, pointeval, lineparams) on \a solution (its snapshot selected).
DerivedResult evaluateDerived(const Solution& solution, const Node& node);

/// A study's solutions as results take them: each of its sweep's (one
/// without a sweep), and of each, each output time (none at rest).
struct SolutionSet {
    std::vector<std::shared_ptr<Solution>> solutions;
    QStringList swept;   ///< the parameters swept
};
/// One solution of a set: its sweep point and its snapshot (-1: as it is).
struct Instance {
    int point = 0;
    int snapshot = -1;
    double time = 0;
    QString label;       ///< "t = 0.5 s", "w = 2 mm", "w = 2 mm, t = 0.5 s"; empty: the only one
};
/// Every instance of \a set (each point, each time), or those \a which
/// picks: the point \a point (from 1; 0 the last) and the time \a time
/// (empty: the last).
std::vector<Instance> instances(const SolutionSet& set, bool all, int point = 0, const QString& time = {});
/// The solution of \a instance, its snapshot selected.
Solution* select(const SolutionSet& set, const Instance& instance);

} // namespace qucs_s::fem

#endif // QUCS_FEM_RESULTS_H
