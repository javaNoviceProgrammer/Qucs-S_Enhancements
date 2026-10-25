/*
 * rf25_static.h - the quasi-static extraction: the charge on the metals,
 *                 the capacitance matrix, and a line's RLGC
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Phase B of docs/proposals/2026-10-23-rf-2p5d-moment-method.md: "the
 * quasi-static extraction: charge MoM with the static layered Green's
 * function for C". The mesh of rf25_mesh.h gives the cells; the Green's
 * function of rf25_green.h gives the potential of a charge in the stack;
 * this puts the two together.
 *
 * The method is the textbook one. Each cell carries a uniform charge, the
 * potential at each cell's centre is the sum over every cell of its charge
 * times a coefficient, and holding one conductor at a volt and the rest at
 * none gives a column of the capacitance matrix. Near pairs and a cell's
 * own term are integrated over the cell in closed form - the potential of
 * a uniformly charged rectangle has one, for every image of the stack as
 * well as for the charge itself - and far pairs are taken at the centre,
 * where the difference is below a part in ten thousand.
 *
 * From C, and from the C0 of the same structure with every dielectric
 * replaced by vacuum, come a line's quasi-TEM parameters by the relations
 * the multiphysics engine's own lineparams uses, so that the two agree:
 *
 *     eps_eff = C/C0,  Z0 = 1/(c0 sqrt(C C0)),  L = 1/(c0^2 C0)
 *
 * Several metal layers are solved together: the Green's function answers
 * the potential at a point on one layer of a charge on another, and a via
 * joins what it lands on into one net. A via carries no charge of its own
 * here - in the thin-metal approximation it is a wire between two sheets,
 * and its sidewall's charge is small beside theirs - so its cells are not
 * unknowns, only the joins they make.
 *
 * What it does NOT do yet: R from the surface impedance, the partial
 * inductances that a non-TEM structure needs for L, the ports and the
 * S-parameters, which are the rest of phase B.
 */
#ifndef QUCS_RF25_STATIC_H
#define QUCS_RF25_STATIC_H

#include "rf25_green.h"
#include "rf25_mesh.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <complex>

namespace qucs_s::rf25 {

/// One conductor held at one potential: the cells that make it up.
struct Net {
    QString name;         ///< the conductors in it ("M1", "M1 #2", "M1+M2")
    QStringList layers;   ///< the metals it reaches, in the stack's order
    QList<int> cells;     ///< indices into the mesh
    double area = 0;      ///< µm²
};

/*!
 * The conductors of \a mesh grouped into nets. The processing has already
 * joined what touches on a layer, so a cell belongs to the conductor it
 * was cut from; what remains is the vias, which join a conductor on one
 * metal to one on another where they overlap in x and y.
 *
 * A net is named for the conductors in it ("M1", "M1 #2", "M1+M2" where a
 * via joins them). A via's own cells are left out: it carries the current
 * between two sheets, not a charge of its own.
 */
QList<Net> netsOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack);

/// What the extraction found.
struct Extraction {
    QStringList names;                ///< the nets, in the order of the matrices
    QList<QList<double>> c;           ///< the capacitance matrix, F (mutual form)
    QList<QList<double>> c0;          ///< the same with every dielectric vacuum, F
    QList<QList<double>> maxwell;     ///< as solved: the short-circuit matrix, F
    QList<QList<double>> maxwell0;    ///< the same with every dielectric vacuum, F
    /*!
     * The matrix the dielectrics' losses make, F, in the same
     * short-circuit form: sum over the lossy layers of tand_k eps_k
     * dC/deps_k, so that the conductance matrix of a line is w times it.
     *
     * It is the matrix form of \a lossTangent, and the one that is
     * actually solved for - a layer's permittivity is nudged a part in a
     * thousand and the whole matrix is solved again, which gives dC/deps
     * for every pair of nets at once. Reducing it to one number per net
     * (which is what \a lossTangent is) throws away how a layer's loss
     * couples two conductors, and coupled lines need that.
     */
    QList<QList<double>> dielectricLoss;
    int unknowns = 0;                 ///< the cells it solved for
    double conditionHint = 0;         ///< the matrix's diagonal spread, for a warning
    /*!
     * How the charge crowds on each net, 1/m^2: sum(q^2/A) over its cells
     * divided by (sum q)^2, with the net at a volt.
     *
     * It is what a conductor's resistance per metre needs beyond the
     * surface resistance. For a line of width W carrying its current
     * evenly it comes to 1/(W L), so Rs L times it is the Rs/W of the
     * textbook; where the current crowds at the edges - which it does,
     * and which is the whole reason to solve rather than divide by a
     * width - it is larger, and the resistance with it. A transmission
     * line's current follows its charge, which is why the charge this
     * solved for can stand for it.
     */
    QList<double> crowding;
    /// Each net's effective loss tangent: the dielectrics' own, weighted
    /// by how much of the field each holds (eps dC/deps). G = w C tand.
    /// The diagonal of \a dielectricLoss over that of the capacitance, as
    /// a single line needs it.
    QList<double> lossTangent;
    /// The metal each net is on, for its surface resistance: its
    /// conductivity (S/m) and thickness (m), or zero where it said none.
    QList<double> sigma, thickness;
    /*!
     * The line's own values per metre, of its middle cross-section as if
     * the line ran on for ever: each cell there a strip, tiled along by
     * copies of itself, and the charge per metre on those strips solved
     * for, so that the fringing of its two open ends is not in them.
     * Dividing the whole charge by the length put both ends' fringing into
     * every value "per metre" - a 50 Ohm line came out 7 % low, a coupled
     * pair's even mode 9 % (the phase B check of 24 October). The ends are
     * a real capacitance, but it is at the ends, which is what a port puts
     * in a circuit, not spread along the line.
     *
     * Empty (fromMiddle false) where the layout is no uniform line - a net
     * that does not run the whole length, or whose cross-section changes
     * along it - and then the whole over the length is all there is, and
     * said to be.
     */
    struct PerMetre {
        bool fromMiddle = false;
        double length = 0;            ///< m: the line's whole length along its axis
        double reach = 0;             ///< m: how far either way its copies were summed
        QList<QList<double>> maxwell, maxwell0, dielectricLoss;   ///< F/m, as the matrices above
        QList<double> crowding;       ///< 1/m: a net's R per metre is its surface resistance times this
        QString said;                 ///< what was done, in words
    };
    PerMetre perMetre;
    /// Whether there is a ground plane under the stack, so that a lone
    /// conductor has something to be a line against.
    bool grounded = false;
    QStringList problems;
    QStringList lines;                ///< what it did, for the panel
    bool ok() const { return problems.isEmpty() && !c.isEmpty(); }
    /// The self capacitance of net \a i to everything else, F.
    double selfCapacitance(int i) const;
};

/*!
 * \a mesh (cells in the layout's micrometres) solved on \a stack: the
 * capacitance matrix of its nets.
 *
 * \a micrometre is how many metres one unit of the cells is. The cells
 * must all be on one metal layer of the stack, and that layer must be a
 * metal of it; anything else is said in problems() rather than answered.
 */
Extraction extract(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, double micrometre = 1e-6);

/// A line's quasi-TEM parameters, per metre, from the capacitance of a
/// length of it.
struct LineParameters {
    double length = 0;      ///< the length the capacitance was of, m
    double c = 0;           ///< F/m
    double c0 = 0;          ///< F/m, every dielectric vacuum
    double epsilonEff = 0;  ///< C/C0
    double z0 = 0;          ///< Ohm, the lossless quasi-TEM value
    double l = 0;           ///< H/m
    double velocity = 0;    ///< m/s
    double nEff = 0;
    // At a frequency, the rest of RLGC and what comes of it. Zero where
    // none was asked for (the quasi-static values above stand alone).
    double frequency = 0;   ///< Hz
    double r = 0;           ///< Ohm/m, from the surface impedance
    double g = 0;           ///< S/m, from the dielectrics' loss tangents
    double skinDepth = 0;   ///< m, in the metal at that frequency
    double surfaceResistance = 0;   ///< Ohm a square
    std::complex<double> zc;        ///< sqrt((R + jwL)/(G + jwC)), Ohm
    std::complex<double> gamma;     ///< sqrt((R + jwL)(G + jwC)), 1/m
    double alphaDb = 0;     ///< dB/m
    QStringList problems;
    bool ok() const { return problems.isEmpty(); }
};

/*!
 * The quasi-TEM parameters of net \a index of \a extraction, taken as a
 * uniform line of length \a length (m) - the length of the layout that was
 * meshed, which is what makes a capacitance into one per metre.
 *
 * The relations are those of the multiphysics engine's lineparams, so that
 * a structure solved both ways gives the same numbers.
 */
LineParameters lineParameters(const Extraction& extraction, int index, double length, double frequency = 0);

/*!
 * The surface impedance of a metal of conductivity \a sigma (S/m) and
 * thickness \a t (m) at \a frequency, Ohm a square.
 *
 * (1 + j)/(sigma d) coth((1 + j) t/d) with d the skin depth: a slab, not
 * a half space, so it is right both where the metal is many skin depths
 * thick (it tends to 1/(sigma d)) and where it is thin beside one (it
 * tends to the direct-current 1/(sigma t)). Zero conductivity or
 * thickness gives nothing, which is how a metal that says neither is
 * treated - as perfect.
 */
std::complex<double> surfaceImpedance(double sigma, double t, double frequency);

/// The potential at \a point of a rectangle \a rect carrying unit total
/// charge spread evenly over it, times 4 pi eps, with the charge \a z out
/// of the plane: the closed form a uniformly charged rectangle has. Public
/// because it is what the tests hold the near terms to.
double rectanglePotential(const QRectF& rect, const QPointF& point, double z);

} // namespace qucs_s::rf25

#endif
