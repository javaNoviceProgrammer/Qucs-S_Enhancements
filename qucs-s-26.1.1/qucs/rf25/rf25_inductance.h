/*
 * rf25_inductance.h - the partial inductances of a layout, and the
 *                     inductance of a loop made of them: the magnetic
 *                     half of the quasi-static extraction
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The last line of phase B of docs/proposals/2026-10-23-rf-2p5d-moment-
 * method.md: "partial inductances for L".
 *
 * Why this is not the L the earlier rounds already answer. For a uniform
 * transmission line, L = 1/(c0^2 C0) is exact and comes free with the
 * charge solve - the magnetic problem of a line is the electric one of
 * the same cross-section in vacuum, turned about. An *inductor* is not a
 * uniform line: its current goes round a loop, the loop's own field
 * threads it, and no cross-section describes that. For it one needs the
 * current's path and the flux each piece of it puts through every other,
 * which is Ruehli's partial inductance (1972) and the L of PEEC.
 *
 * The method, briefly:
 *
 *  - A conductor is cut into straight runs of current - here the pairs of
 *    mesh cells that share an edge, which is exactly what the mesh has
 *    been counting as "the unknowns: where current flows" since phase A.
 *  - Between two parallel runs, the partial inductance is the double line
 *    integral of 1/r, which has a closed form in four terms. Between two
 *    orthogonal runs it is nothing, which is what makes an axis-aligned
 *    layout cheap: only x with x and y with y.
 *  - A run's own partial inductance is the same formula with the distance
 *    replaced by the geometric mean distance of its cross-section with
 *    itself. For a thin strip of width w that is w e^-3/2, and the
 *    formula then reproduces the textbook (mu0 l/2pi)(ln(2l/w) + 1/2)
 *    exactly - which is the anchor the tests hold it to.
 *  - The current itself is not assumed: it is solved for. Of all the
 *    currents that obey Kirchhoff's law and carry the terminal current,
 *    the one that flows is the one of least magnetic energy, so
 *
 *        minimise  1/2 I^T Lp I   subject to  A I = s
 *
 *    with A the node-branch incidence and s the injection. That is a
 *    saddle-point system in I and the node potentials, and its answer is
 *    where a current goes once its inductance outweighs its resistance -
 *    crowding to the short way round, which is the whole reason to solve
 *    rather than to guess. Then L = I^T Lp I / I_total^2.
 *  - At direct current it is the resistance that divides the current
 *    instead: minimise I^T R I subject to the same, which is the
 *    network's own nodal solve. Its loss is the loop's resistance at
 *    direct current - a different quantity from a line's R per metre -
 *    and its energy the inductance at direct current, beside the other.
 *    (The least-energy current's own loss was given as the resistance at
 *    direct current until the phase B check of 24 October: on a bar ten
 *    cells across it was 29 % high, the crowding counted as resistance.)
 *
 * What it does not do: between the two, where R and wL are alike, the
 * current is neither; that wants (R + jwL) i = v solved at each
 * frequency, which is the next thing, and so does the current leaving
 * the inside of the metal. The thin-metal approximation of the rest of
 * the solver is kept: the cross-section's thickness enters the resistance
 * but not the geometric mean distance.
 */
#ifndef QUCS_RF25_INDUCTANCE_H
#define QUCS_RF25_INDUCTANCE_H

#include "rf25_mesh.h"
#include "rf25_static.h"

#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>

namespace qucs_s::rf25 {

/*!
 * One straight run of current: from \a from to \a to, the current spread
 * evenly over \a width across its own direction and \a thickness through
 * it. SI throughout.
 *
 * A run in the plane has from.z == to.z; a via is a run in z, which is
 * said by \a vertical with \a from and \a to then the same point and
 * \a z0, \a z1 the metals it joins.
 */
struct Filament {
    QPointF from, to;        ///< m, in the plane
    double z0 = 0, z1 = 0;   ///< m; a run in the plane has z0 == z1
    double width = 0;        ///< m, across its direction
    double thickness = 0;    ///< m
    double sigma = 0;        ///< S/m; 0: perfect

    /// Its length, m.
    double length() const;
    /// Its direction, a unit vector (x, y, z).
    void direction(double* x, double* y, double* z) const;
    /// Its middle, m.
    void middle(double* x, double* y, double* z) const;
    /// Its resistance at direct current, Ohm; 0 where it says no
    /// conductivity or no thickness.
    double resistance() const;
};

/*!
 * The geometric mean distance of a strip of width \a width with itself,
 * m: what a run's own partial inductance uses in place of a distance.
 *
 * w e^-3/2 for a thin strip, which is the standard result and the one
 * that makes the self partial inductance come out as the textbook's
 * (mu0 l/2pi)(ln(2l/w) + 1/2). Zero width gives nothing, which would be
 * an infinite inductance, so it is refused by the caller rather than
 * answered.
 */
double selfDistance(double width);

/*!
 * The partial inductance between runs \a a and \a b, H.
 *
 * Nothing where they are orthogonal. Where they are parallel it is
 * (mu0/4pi) times the double line integral of 1/r, in the four-term
 * closed form, with the transverse distance averaged over both widths by
 * quadrature where they are near enough for it to matter and taken
 * between their middles where it is not. Where they are the same run it
 * is the self term, by selfDistance().
 */
double partialInductance(const Filament& a, const Filament& b);

/// The partial inductance matrix of \a runs, H: symmetric, and its
/// diagonal each run's own.
QList<QList<double>> partialInductances(const QList<Filament>& runs);

/*!
 * A loop of runs carrying the same current round it in turn: its
 * inductance, H.
 *
 * The sum of every pair's partial inductance, each with the sign of how
 * the two runs are directed - which for a closed loop is what Ruehli's
 * partial inductances add up to. The runs must be given in order round
 * the loop, each starting where the one before it ended.
 */
double loopInductance(const QList<Filament>& loop);

/// What an inductance extraction found.
struct Inductance {
    double l = 0;             ///< H: the loop's inductance, of the least-energy current
    double lDc = 0;           ///< H: the same of the current at direct current
    double rDc = 0;           ///< Ohm: its resistance at direct current
    double q = 0;             ///< wL/R at the frequency asked for; 0 at none
    double frequency = 0;     ///< Hz, for q
    int branches = 0;         ///< the runs of current it solved for
    int nodes = 0;            ///< the cells they join
    QList<double> current;    ///< each run's current, A, for the drawing
    QList<Filament> runs;     ///< the runs themselves, beside the currents
    QStringList problems;
    QStringList lines;        ///< what it did, for the panel
    bool ok() const { return problems.isEmpty() && l > 0; }
};

/*!
 * The mesh of \a conductors on \a stack solved for the current that goes
 * in at \a from and out at \a to (both in the layout's own units), and
 * the inductance and resistance of the loop it makes.
 *
 * \a micrometre is how many metres one unit of the cells is; \a frequency
 * (Hz) is only for the Q it reports.
 *
 * The terminals are the cells nearest the two points. What it will not
 * solve is said in problems() rather than answered: a mesh with no two
 * cells sharing an edge (nothing for current to flow through), terminals
 * that land on the same cell or in parts of the layout that are not
 * joined, and a mesh past what a dense matrix of this build holds.
 */
Inductance inductanceOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, QPointF from, QPointF to,
                        double micrometre = 1e-6, double frequency = 0);

/// The runs of current the mesh of \a conductors makes: the pairs of
/// cells that share an edge, and the vias that join one layer to
/// another. Public because it is what the tests count.
QList<Filament> runsOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, double micrometre = 1e-6,
                       QList<QPair<int, int>>* ends = nullptr);

} // namespace qucs_s::rf25

#endif
