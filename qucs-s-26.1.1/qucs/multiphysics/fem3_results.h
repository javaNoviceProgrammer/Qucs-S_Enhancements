/*
 * fem3_results.h - a 3D solution's results: an expression's values at its
 *                  mesh's nodes, its slice by a plane across x, y or z, a
 *                  study's solutions by point and time, and derived values -
 *                  integrals, averages, extrema over domains, faces and
 *                  edges, values at points, globals
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_RESULTS_H
#define QUCS_FEM3_RESULTS_H

#include "fem3_solver.h"
#include "fem_results.h"

#include <array>
#include <limits>
#include <optional>
#include <vector>

namespace qucs_s::fem {

/// \a e's value at each of the mesh's nodes: each prism's at its corner,
/// the prisms about a node averaged (where it jumps - a coefficient's, a
/// gradient's across two materials - their mean); NaN where none can read
/// it. A field's own variable (ht.T, V) is its values as they are.
std::vector<double> nodalValues(const Solution3& solution, const Expression& e);

/// A slice's triangle: its corners (the geometry's unit) and \a e's value at
/// each, of the prism it cuts (its domain's).
struct SliceTriangle {
    std::array<Vec3, 3> p;
    std::array<double, 3> value;
    int domain = -1;
};
/// The prisms cut by the plane \a axis (0 x, 1 y, 2 z) = \a at (the
/// geometry's unit): each cut - a triangle across z, a quadrilateral as two
/// across x or y - with \a e at its corners, read in its prism.
std::vector<SliceTriangle> slice(const Solution3& solution, const Expression& e, int axis, double at);

/// The model cut by the plane through \a origin of normal \a normal (the
/// geometry's unit): its triangles, \a e at their corners read in their
/// prisms. (The prisms split as isosurfaces() splits them.)
std::vector<SliceTriangle> slicePlane(const Solution3& solution, const Expression& e, const Vec3& origin, const Vec3& normal);

/// An isosurface's triangle: its corners (the geometry's unit), its level's
/// place among the levels asked, its prism's domain.
struct IsoTriangle {
    std::array<Vec3, 3> p;
    int level = 0;
    int domain = -1;
};
/// Where \a nodal - an expression's values at the mesh's nodes, as
/// nodalValues() gives them - is each of \a levels: each prism split into
/// three tetrahedra, each wall along its diagonal through its corner of the
/// least number (as the prism beside it splits it: no cracks between them),
/// the values linear in each.
std::vector<IsoTriangle> isosurfaces(const Solution3& solution, const std::vector<double>& nodal, const std::vector<double>& levels);

/// A vector field's arrows on a grid of \a nx by \a ny by \a nz points across
/// \a box (the geometry's unit), each in the middle of its cell: where, and
/// its vector (SI); none where no domain defines it.
struct Arrow3 {
    Vec3 at;
    Vec3 v;
};
std::vector<Arrow3> arrows3(const Solution3& solution, const std::array<Expression, 3>& v, const Box3& box, int nx, int ny, int nz);

/// One solution of a 3D study's, as results take it: its sweep's point and
/// its output time.
struct Instance3 {
    int point = 0;        ///< from 0
    int snapshot = -1;    ///< its output time; -1 at rest
    double time = 0;      ///< s
    QString label;        ///< "t = 1 ms", "P = 10 mW", both; empty: the only one
};
using Solutions3 = std::vector<std::shared_ptr<Solution3>>;
/// Every instance of \a set - each point of its sweep, each output time -
/// or (\a all false) the one asked: point \a point (from 1; 0 the last),
/// output time \a time (empty: the last; else the nearest). A time that does
/// not read: none, and why in \a error.
std::vector<Instance3> instances3(const Solutions3& set, bool all, int point = 0, const QString& time = {}, QString* error = nullptr);
/// The solution of \a at, its output time selected (null: none).
Solution3* select3(const Solutions3& set, const Instance3& at);

/// What an expression comes to over a 3D solution's domains
/// (Level::Domain), faces (Boundary) or edges (Edge) \a picked: its
/// integral, the volume (area, length) it is defined on, its least and
/// greatest - read at the quadrature's points and the cells' corners.
struct Over3 {
    double integral = 0, measure = 0;
    double min = std::numeric_limits<double>::infinity(), max = -std::numeric_limits<double>::infinity();
    bool any = false;
};
Over3 over3(const Solution3& solution, const Expression& e, Level level, const QVector<int>& picked);

/// \a e at the model's point \a x (m); none where no prism holds it, or it
/// is not finite there.
std::optional<double> evaluateAt3(const Solution3& solution, const Expression& e, const Vec3& x);

/// Evaluates a Derived Values node - global, integral, average, maximum,
/// minimum, pointeval - on a 3D solution as it is selected.
DerivedResult evaluateDerived3(const Solution3& solution, const Node& node);

} // namespace qucs_s::fem

#endif // QUCS_FEM3_RESULTS_H
