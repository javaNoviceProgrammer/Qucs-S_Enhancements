/*
 * rf25_dielectric.h - a dielectric of an RF 2.5D stack that ends: a layer
 *                     there only inside its shapes, something else beyond
 *                     them - the substrate at a chip's or a board's edge, a
 *                     trench, a dielectric under one part alone (F2 of the
 *                     open items of 8 November)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Every Green's function of the solver is of a stack whose layers run on
 * for ever across the plane. A layer that ends is the stack's Green's
 * function and what the difference does: by the volume equivalence, the
 * stack as it is equals a background stack plus, where the two differ, a
 * polarization (eps - eps_background) E. Where the difference is one
 * permittivity throughout a region, that polarization is a charge on the
 * region's faces alone, sigma = (eps - eps_b) E_inside . n, which the
 * background's own Green's function carries - the faces on its interfaces
 * too, where the background's images give them their share of either side.
 *
 * So for each layer that ends:
 *
 *  - Its region: the background is the stack with the layer as it is
 *    beyond its shapes, everywhere, and the region its shapes, within a
 *    box round the conductors - grown by six heights of the stack, past
 *    which what a conductor's field polarizes is a part in some hundreds.
 *  - Its faces in panels: its top and bottom in squares graded from the
 *    conductors and the region's edges outward, cut to it and kept convex,
 *    where a metal lies on it that metal's cells; its walls in strips along
 *    each edge in rows up its thickness. A face on a ground plane has none:
 *    its charge and its image are one.
 *  - Each panel's charge from the field just inside it, the field from the
 *    potential: of a panel near, its own closed form (Wilton, Rao,
 *    Glisson, Schaubert, Al-Bundak and Butler, IEEE Trans. AP 32 (1984)
 *    276) for each of the strongest images of the background, the rest from
 *    its table; of one far, its charge at its middle. The field normal to a
 *    face is a one-sided difference of the potential there and a little
 *    inside, second order - which takes the face's own jump as it is.
 *
 * What the conductors see of it is a correction to their potential
 * coefficients, P = P_background + P_cb M^-1 T: T what their charges put on
 * the panels, M the panels' own system, P_cb what the panels' charges put on
 * the conductors - made once, static, for the quasi-static solve and for
 * the full-wave one's P alike.
 *
 * What it does not do: the polarization is the static one, so a full-wave
 * solve sees the layer's end in its charge and not in its current - true
 * while the region is small beside the wavelength in it. A layer that ends
 * keeps no loss tangent of its own inside or beyond its shapes but the
 * background's. A region is a layer's whole thickness.
 */
#ifndef QUCS_RF25_DIELECTRIC_H
#define QUCS_RF25_DIELECTRIC_H

#include "rf25_green.h"

#include <Eigen/Dense>

#include <QList>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <atomic>

namespace qucs_s::rf25 {

/// \a polygon's integral of 1/R over itself from a point: the closed form
/// of Wilton et al. for a plane polygon - its corners \a corners (x, y, z
/// triples, m, in order round it), the point \a at (x, y, z). The polygon's
/// potential at the point is this times its charge density over 4 pi eps.
/// Exact anywhere, on the polygon itself too.
double polygonIntegral(const QList<std::array<double, 3>>& corners, const std::array<double, 3>& at);

/// A layer of the stack that ends: there only inside \a shapes (µm), and
/// of permittivity \a beyond outside them.
struct Extent {
    QString layer;               ///< a dielectric's or the substrate's name
    QList<QPolygonF> shapes;     ///< µm
    double beyond = 1;           ///< the relative permittivity beyond its shapes
};

/// One of a solve's charged cells, as the correction sees it.
struct ChargedCell {
    QRectF rect;                 ///< m: its box (a cell's charge is spread over it)
    double z = 0;                ///< m: the metal's height, as layersOf() has it
};

/*!
 * What a solve's stack and its ended layers come to: the background the
 * solve's own Green's functions are to be built of, and each region.
 */
struct DielectricPlan {
    Layers background;           ///< the stack with each ended layer as its background has it
    struct Region {
        QString layer;
        int slab = -1;           ///< its slab of the background
        double delta = 0;        ///< eps_r as it is less the background's, there
        bool inside = true;      ///< the region is the layer's shapes
        QList<QPolygonF> outline;   ///< m, within the box
    };
    QList<Region> regions;
    QRectF box;                  ///< m: the conductors' box grown by the margin
    QStringList problems;
    QStringList lines;
    bool ok() const { return problems.isEmpty(); }
    bool isEmpty() const { return regions.isEmpty(); }
};

/// \a layers (layersOf() of the stack) with \a extents, round conductors
/// whose box is \a around (m): the background and the regions.
DielectricPlan dielectricPlan(const Layers& layers, const QList<Extent>& extents, const QRectF& around);

/// What the conductors see of the plan's regions.
struct DielectricCorrection {
    /// Between the cells given, in their order, V/C: what the regions add to
    /// their potential coefficients - symmetric. Of the factors U V^T where
    /// it is held so (\a factored).
    Eigen::MatrixXd delta;
    Eigen::MatrixXd u, v;
    bool factored = false;
    int panels = 0;
    QStringList lines;
    QStringList problems;
    bool ok() const { return problems.isEmpty(); }
    /// y += delta x (dense or factored).
    void apply(const Eigen::VectorXd& x, Eigen::VectorXd& y) const;
    /// Its entry (i, j).
    double at(int i, int j) const;
};

/*!
 * The correction of \a plan for \a cells. \a factored: held as U V^T (a
 * compressed solve's many cells) rather than dense. \a stop is heeded.
 * \a mostPanels (0: the build's) bounds the panels, the grading coarsened
 * to keep within it.
 */
DielectricCorrection dielectricCorrection(const DielectricPlan& plan, const QList<ChargedCell>& cells, bool factored = false,
                                          const std::atomic_bool* stop = nullptr, int mostPanels = 0);

/// Tests: the potential at \a at of a unit charge spread over a wall from
/// \a from to \a to (m), \a z0 to \a z1, in slab \a slab of \a layers - as
/// the correction reads a wall near a point.
double wallPotentialForTests(const Layers& layers, int slab, QPointF from, QPointF to, double z0, double z1,
                             const std::array<double, 3>& at);

} // namespace qucs_s::rf25

#endif
