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

/// An expression over the domains: each element in triangles (in four for
/// a quadratic field), its value at their corners. Points in the
/// geometry's unit, values as shown.
struct SurfaceData {
    std::vector<QPointF> points;   ///< three a triangle
    std::vector<double> values;    ///< one a point (NaN: not defined there)
    double min = 0, max = 0;       ///< of those defined
    Dim dim;
    DisplayUnit unit;
    QString error, warning;
};
SurfaceData surfaceData(const Solution& solution, const QString& expression, const QString& unit = {});

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
/// minimum, pointeval, lineparams) on \a solution.
DerivedResult evaluateDerived(const Solution& solution, const Node& node);

} // namespace qucs_s::fem

#endif // QUCS_FEM_RESULTS_H
