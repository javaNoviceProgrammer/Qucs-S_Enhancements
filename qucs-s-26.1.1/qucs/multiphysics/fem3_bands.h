/*
 * fem3_bands.h - a 3D mesh by band of layers: its interfaces joined - each
 *                interface's finer band's degrees of freedom there fixed as
 *                the canonical (moment) interpolation of the coarser band's
 *                trace, for nodal and edge fields alike
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_BANDS_H
#define QUCS_FEM3_BANDS_H

#include "fem3_fe.h"
#include "fem3_wave.h"

#include <Eigen/Sparse>

#include <QPointF>
#include <QRectF>
#include <QString>

#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace qucs_s::fem {

/// A triangle's corners, counter-clockwise (metres).
using Corners = std::array<QPointF, 3>;

/*!
 * Where a point is in a plan's triangles: a grid of their boxes. The
 * triangles taken straight (their corners), in metres (the plan's nodes
 * times \a scale).
 */
class PlanLocator
{
public:
    PlanLocator(const Mesh& plan, double scale);
    const Corners& corners(int t) const { return a_corners[std::size_t(t)]; }
    int triangles() const { return int(a_corners.size()); }
    /// The triangles whose box meets the box of \a a and \a b.
    std::vector<int> near(QPointF a, QPointF b) const;
    /// The triangle holding \a p (within a tolerance of its size), or -1;
    /// its barycentric (ξ, η) in \a xi, \a eta.
    int locate(QPointF p, double* xi, double* eta) const;

private:
    std::vector<Corners> a_corners;
    QRectF a_box;
    int a_nx = 1, a_ny = 1;
    double a_cw = 1, a_ch = 1;
    std::vector<std::vector<int>> a_cells;
};

/// The polygon where triangles \a p and \a c overlap (counter-clockwise;
/// empty: none, or a sliver of an area under a millionth of \a tol²). Clipped
/// exactly - not widened by \a tol: two triangles beside a side would both
/// take a strip of it.
std::vector<QPointF> clipTriangle(const Corners& c, const Corners& p, double tol);
/// Of triangle \a p, the area the located plan's triangles cover: the sum
/// of its overlays with them (the triangle's own where they cover it).
double overlayArea(const PlanLocator& at, const Corners& p);

/*!
 * Constraints: degrees of freedom each a combination of others - a mesh by
 * band's interfaces' finer side's, of the coarser side's there. Its masters
 * are never constrained.
 */
struct Constraints3 {
    std::vector<int> dofs;                                   ///< constrained, rising
    std::vector<std::vector<std::pair<int, double>>> rows;   ///< each one's masters and weights
    bool empty() const { return dofs.empty(); }
};

/// A nodal space's constraints at its mesh's interfaces (none: one band).
/// The finer band's degrees of freedom on an interface not the coarser
/// band's too (a node the two share is one): its vertices' values the
/// coarser trace's there; order 2, its edges' middles from the trace's mean
/// along the edge, (6m - a - b)/4 on a straight one (the value at a curved
/// one's middle).
Constraints3 nodalConstraints(const Space3& space);
/// An edge space's constraints at its mesh's interfaces (E across z at the
/// interface's level): the finer band's from the canonical moments of the
/// coarser band's tangential trace - along each edge (against 1, and order
/// 2 against 2s - 1 too) and, order 2, over each triangle (its two
/// components) - integrated over the two plans' overlay, each triangle's
/// own coefficients from them by its moment matrix.
Constraints3 edgeConstraints(const EdgeSpace3& space);
/// Whether no master of \a c is constrained (chained constraints are not
/// taken); why not in \a why.
bool constraintsUnchained(const Constraints3& c, int n, QString* why);
/// u = C v for \a n degrees of freedom: C (n by the unconstrained), each
/// one's reduced number in \a reduced (-1: constrained). A master
/// constrained: an empty matrix, and why (constraintsUnchained()).
Eigen::SparseMatrix<double> reductionMatrix(int n, const Constraints3& c, std::vector<int>* reduced, QString* why = nullptr);

/// A plane field's canonical interpolation into band \a band's edge
/// functions at height \a height (E across z there): each of its degrees of
/// freedom's value, by number (others untouched) - \a field gives (Ex, Ey)
/// at a point (the geometry's unit).
void interpolateEdges(const EdgeSpace3& space, int band, int height, const std::function<void(QPointF, double[2])>& field,
                      std::vector<double>& values);
/// A plane function's canonical interpolation into band \a band's nodal
/// functions at height \a height (vertices' values; order 2 edges' means).
void interpolateNodes(const Space3& space, int band, int height, const std::function<double(QPointF)>& f, std::vector<double>& values);

} // namespace qucs_s::fem

#endif // QUCS_FEM3_BANDS_H
