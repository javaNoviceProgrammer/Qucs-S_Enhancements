/*
 * fem_line.h - a line across a model's domains for a physics solved in 1D
 *              (not installed: the solver's own): its stretch in each
 *              domain found from the mesh's triangles, quadratic elements
 *              of its own along it, the boundaries it meets
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_LINE_H
#define QUCS_FEM_LINE_H

#include "fem_solver.h"

#include <QPointF>

#include <set>
#include <vector>

namespace qucs_s::fem::detail {

/*!
 * A line from S to E (m) across the domains of a physics: nodes along it
 * (m from S) of quadratic elements, two a element and one more; each
 * element's domain (-1 a gap: in no domain of the physics); the boundaries
 * of the geometry through each element's ends.
 */
struct Line {
    QPointF S, E;
    double L = 0;
    std::vector<double> z;
    std::vector<int> domain;                 ///< by element
    std::vector<std::set<int>> boundaries;   ///< by node (at elements' ends)
    int elements() const { return int(domain.size()); }
    QPointF point(double s) const { return S + (E - S) * (s / L); }
    /// Along it (m from S) of a point (m), its projection.
    double along(QPointF p) const;
    /// The element at \a s and where in it (0 to 1).
    int element(double s, double* xi) const;
};

/// A line along the growth axis or an axis (\a axis 0 x, 1 y) through
/// \a at (text in the geometry's unit; empty: the middle of the domains in
/// \a on) across the domains in \a on, its elements \a size (m) long at most
/// - or, \a size 0, \a count of them over its length. False and why.
bool buildLine(const Solution& s, const std::vector<char>& on, int axis, const QString& at, double size, int count, Line* line,
               QString* error);

/// The quadratic shape functions on [0, 1] at \a xi, and their derivatives.
void lineShape(double xi, double N[3], double dN[3]);

} // namespace qucs_s::fem::detail

#endif // QUCS_FEM_LINE_H
