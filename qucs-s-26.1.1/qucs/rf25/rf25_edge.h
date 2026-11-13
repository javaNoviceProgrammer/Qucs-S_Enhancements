/*
 * rf25_edge.h - the current across a metal's edge, solved in its
 *               cross-section: what a run along the edge loses
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * A2 of the open items of 8 November (docs/bug_hunts/2026-11-08-claude-
 * rf25-open-items.md), with A1: a metal solved as its two faces carries an
 * edge run's current on its face's strip and its half of the wall
 * (rf25_fullwave.cpp's wallsOf()), each part's current even across it. The
 * real current is not: it crowds to the corner as the edge's own field has
 * it, and loses more for it - a fifth to a third more on the lines of
 * docs/feature_gaps/2026-11-04-claude-rf25-fullwave-gaps.md. How much more
 * is solved here, once for each metal and frequency, in the cross-section
 * of its edge: eddy.py's method beside that write-up - the metal cut into
 * rectangles of uniform current graded toward its faces and its wall, the
 * mean of ln r between them in closed form, each rectangle's drop its own
 * resistance and every current's inductance, the drop alike across the
 * metal -, the edge's strip mirrored about its middle 20 thicknesses in
 * and about its mid-thickness. A band of it from the edge, a top face's
 * strip with its half of the wall, then loses what its rectangles do.
 */
#ifndef QUCS_RF25_EDGE_H
#define QUCS_RF25_EDGE_H

#include "rf25_mesh.h"

#include <QHash>
#include <QList>
#include <QRectF>

#include <complex>
#include <functional>
#include <memory>

namespace qucs_s::rf25 {

/// A metal's edge solved in its cross-section (solveEdge()): x in from the
/// edge, y up from the metal's middle to its top face (m); each
/// rectangle's current, of a whole metal's current of 1 A across its
/// strip's two edges and both faces.
struct EdgeSolution {
    double sigma = 0, thickness = 0, frequency = 0;
    double reach = 0;                    ///< m: how far in the strip runs before its mirror
    QList<QRectF> cells;                 ///< x, y: [0, reach] by [0, t/2]
    QList<std::complex<double>> current; ///< A
    bool ok() const { return !cells.isEmpty(); }
};

/// The edge of a metal \a thickness m thick of \a sigma S/m at \a frequency
/// Hz, solved; empty where any is nought or less.
EdgeSolution solveEdge(double sigma, double thickness, double frequency);

/// The same, made once for each metal and frequency and kept (on any
/// thread).
std::shared_ptr<const EdgeSolution> edgeSolution(double sigma, double thickness, double frequency);

/// A band of an edge's top half from \a from to \a to in from the edge
/// (m): its current, and its loss per metre - each rectangle's |I|^2
/// over its conductance, those cut by the band's sides in their share.
struct EdgeBand {
    std::complex<double> current;
    double loss = 0;                     ///< W/m
};
EdgeBand bandOf(const EdgeSolution& edge, double from, double to);

/*!
 * A part of a cell's charge, where the charge is spread over more than the
 * cell (A1 of the open items of 8 November): on a metal solved as its two
 * faces a cell at the metal's edge, where its wall faces another
 * conductor's across a narrow gap (wallGapsOf()'s, sixteen thicknesses at
 * most), is its face's rectangle and its half of that wall - from the face
 * to the metal's middle -, the charge shared by area. There the walls hold
 * the field between them, which no face can; at an edge with nothing near
 * the faces' edge cells hold its corner's charge near enough - a microstrip
 * of 3 µm copper read Z0 1.53 % under the sheet's with them alone, where
 * Hammerstad and Jensen's thickness correction says 1.49, and 2.26 % with
 * charge on its walls as well. Each part a rectangle in its own plane (m): the
 * plane of the cells (0), a wall along x at y = \a at (1: its x and z), a
 * wall along y at x = \a at (2: its y and z); \a at across it, its centre,
 * and its share.
 */
struct ChargePart {
    int plane = 0;
    QRectF rect;
    double at = 0;
    double x = 0, y = 0, z = 0;
    double weight = 1;
};

/// The cells of \a thick's mesh at the edge of a metal solved as its two
/// faces whose wall faces another's across a narrow gap, each with its
/// parts (its face first); \a zOf each cell's height
/// (m), \a micrometre how many metres the mesh's unit is.
QHash<int, QList<ChargePart>> wallCharges(const Thickened& thick, double micrometre, const std::function<double(int)>& zOf);
/// A cell as one part: its rectangle (m) at height \a z.
ChargePart wholeCell(const QRectF& rect, double z);
/// Part \a q's potential at (x, y, z) of a unit charge spread over it, as
/// rectanglePotential() has a cell's (its mean of 1/r): in q's own plane.
double partPotential(const ChargePart& q, double x, double y, double z);
/// The potential at \a at's parts of a unit charge spread over \a of's,
/// each by its share: the static part of a potential coefficient.
double partsPotential(const QList<ChargePart>& at, const QList<ChargePart>& of);
/// \a parts moved by (dx, dy) m.
QList<ChargePart> movedBy(const QList<ChargePart>& parts, double dx, double dy);

} // namespace qucs_s::rf25

#endif
