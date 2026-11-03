/*
 * fem3_results.h - a 3D solution's results: an expression's values at its
 *                  mesh's nodes, and its slice by a plane across x, y or z
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

#include <array>
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

} // namespace qucs_s::fem

#endif // QUCS_FEM3_RESULTS_H
