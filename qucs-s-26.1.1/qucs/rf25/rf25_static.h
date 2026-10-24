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
    int unknowns = 0;                 ///< the cells it solved for
    double conditionHint = 0;         ///< the matrix's diagonal spread, for a warning
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
    double z0 = 0;          ///< Ohm
    double l = 0;           ///< H/m
    double velocity = 0;    ///< m/s
    double nEff = 0;
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
LineParameters lineParameters(const Extraction& extraction, int index, double length);

/// The potential at \a point of a rectangle \a rect carrying unit total
/// charge spread evenly over it, times 4 pi eps, with the charge \a z out
/// of the plane: the closed form a uniformly charged rectangle has. Public
/// because it is what the tests hold the near terms to.
double rectanglePotential(const QRectF& rect, const QPointF& point, double z);

} // namespace qucs_s::rf25

#endif
