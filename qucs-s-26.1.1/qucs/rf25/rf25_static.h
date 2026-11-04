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
 * unknowns, only the joins they make. A metal solved as its two faces is
 * two sheets, its bottom and its top, and its walls a via between them
 * (thickened()): the charge on both, its top's in what is over the metal -
 * its walls' field across a narrow gap, which the one sheet had in the
 * dielectric under it as much as in the air.
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
#include <QPointF>
#include <QString>
#include <QStringList>

#include <atomic>
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

/// A conductor a line returns through, marked on the layout (a Return
/// Conductor under Ports): the net under \a at (µm), on \a metal - any
/// where it is empty.
struct ReturnMark {
    QPointF at;
    QString metal;
};

/*!
 * Which of \a nets a line returns through - held at nought with the ground,
 * the rest the lines' signals - and why, in \a said:
 *
 *   - a net under a mark of \a marks;
 *   - a net a via joins to the ground plane, at its potential;
 *   - where neither is and there is no ground plane, the two outermost of
 *     three or more nets across a straight run of them, each the run's whole
 *     length and wholly outside the rest: a coplanar line's grounds.
 *
 * None where none is, and every net is then a line against the ground or,
 * with no ground, against infinity.
 */
QList<bool> returnNetsOf(const QList<Net>& nets, const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack,
                        const QList<ReturnMark>& marks, QString* said = nullptr);

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
     *
     * A uniform line's is its cross-section's (PerMetre) over its length,
     * its two ends' share left out: its losses are read per metre, and the
     * whole solved again for each lossy layer was read by nothing - two
     * thirds of its time once the stack's Green's functions took
     * milliseconds.
     */
    QList<QList<double>> dielectricLoss;
    int unknowns = 0;                 ///< the cells it solved for
    /// The dense systems of all of them it filled and factored: the stack's
    /// and the vacuum's, and for a structure that is no uniform line the
    /// whole again for each lossy layer.
    int solves = 0;
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
    /// Its rms roughness, m, for the loss it adds (roughnessFactor()).
    QList<double> roughness;
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
        /*!
         * R by Wheeler's incremental-inductance rule, R = (Rs/mu0) dL/dn:
         * what each net's metal - [k] - and the ground add to the
         * resistance matrix per metre per ohm of their surface resistance,
         * dL/dn/mu0 with every face of that conductor receding into it.
         * L is of the cross-section's own rectangles, the metal's thickness
         * and walls in them, by a 2D solve in vacuum over the ground - L
         * is the vacuum's whatever the dielectrics. The crowding above is a
         * zero-thickness sheet's, whose edges make R grow without end as
         * the mesh is refined (the phase B check's F4: +10.5 % from 5 µm
         * cells to 2.5); this does not. Empty where the line's metal has no
         * thickness or there is no ground, and the crowding is then all
         * there is, said.
         */
        QList<QList<QList<double>>> wheeler;   ///< [k][i][j], 1/m
        QList<QList<double>> wheelerGround;    ///< [i][j], 1/m
        QList<double> dcResistance;            ///< Ohm/m, each net's at direct current
        /// Ohm/m: the return conductors' at direct current, all of them
        /// together, which every signal's current comes back through.
        double dcReturn = 0;
        QString said;                 ///< what was done, in words
    };
    PerMetre perMetre;
    /// Whether there is a ground plane under the stack, so that a lone
    /// conductor has something to be a line against.
    bool grounded = false;
    /*!
     * Which nets the lines return through (returnNetsOf()): held at nought
     * with the ground, so that the lines are of the rest, the signals,
     * against them. Empty, or all false, where none is: every net is then a
     * line against the ground, or against infinity with none.
     *
     * The matrices above are of every net, as solved; the lines' are of the
     * signals with the returns at nought (signalsOf()), and Wheeler's
     * factors of PerMetre are of the signals alone where there are returns.
     */
    QList<bool> returns;
    QString returnsSaid;      ///< which, and why
    /// The signals' nets, in order: every net where none returns.
    QList<int> signalNets() const;
    bool hasReturns() const { return returns.contains(true); }
    /// The ground's own kind ("perfect", "impedance", "metal", "open")
    /// and, for its loss, its surface impedance (Ohm a square) or its
    /// metal's conductivity (S/m).
    QString groundKind;
    double groundValue = 0;
    QStringList problems;
    QStringList lines;                ///< what it did, for the panel
    bool ok() const { return problems.isEmpty() && !c.isEmpty(); }
    /// The self capacitance of net \a i to everything else, F.
    double selfCapacitance(int i) const;
};

/*!
 * What an extraction looks at and tells as it goes: its stop - between its
 * steps, as the stack's Green's functions are built, as its matrices are
 * filled - and how far it has got, for the tab's bar and rf25_solve's
 * answer when it is still at work.
 *
 * Stopped, it returns at once with "Stopped." in its problems. Before, the
 * tab's Stop let a quasi-static solve go and it ran on regardless: a stack
 * whose Green's function did not end kept a core and twelve gigabytes busy
 * until Qucs-S was killed, and a second solve started beside it (the review
 * of 3 November).
 */
struct ExtractControl {
    /// What it is doing.
    enum Step {
        Starting,
        Greens,         ///< building the stack's Green's functions: done of all
        Charge,         ///< solving the charge on the cells (the matrix filled, then factored)
        Vacuum,         ///< the same with every dielectric vacuum, for C0
        CrossSection,   ///< a uniform line's middle cross-section, per metre
        Losses          ///< each lossy dielectric's share, a solve each: done of all
    };
    const std::atomic_bool* stop = nullptr;
    std::atomic_int* step = nullptr;   ///< a Step
    std::atomic_int* done = nullptr;
    std::atomic_int* all = nullptr;
    /// The images the Green's function last built has, for what is said
    /// while the next is built.
    std::atomic_int* images = nullptr;
    /// Tests: the most pairs each Green's function's expansion may form
    /// (Green::limitPairs; 0, its own).
    double mostPairs = 0;

    bool stopped() const { return stop != nullptr && stop->load(); }
    void at(Step s, int d = 0, int a = 0) const
    {
        if (step != nullptr) step->store(s);
        if (done != nullptr) done->store(d);
        if (all != nullptr) all->store(a);
    }
};

/// What \a step of an extraction is, in words, with how far into it
/// \a done of \a all are and the images of the Green's function built last:
/// what the tab's bar and rf25_solve say while it is at work.
QString extractStepSaid(int step, int done, int all, int images);

/*!
 * \a mesh (cells in the layout's micrometres) solved on \a stack: the
 * capacitance matrix of its nets.
 *
 * \a micrometre is how many metres one unit of the cells is. The cells
 * must all be on one metal layer of the stack, and that layer must be a
 * metal of it; anything else is said in problems() rather than answered.
 */
Extraction extract(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, double micrometre = 1e-6,
                   const QList<ReturnMark>& returns = {}, const ExtractControl* control = nullptr);

/*!
 * The lines of \a extraction's signals against its returns, per metre, in
 * Maxwell form - the charge on each signal of a volt on one, the rest and
 * the returns at nought: \a c, \a c0 with every dielectric vacuum, and
 * \a loss, the dielectrics' (as Extraction::dielectricLoss).
 *
 * Of the line's middle where that was taken, else the whole over
 * \a length. Where there is no ground plane a line's charge to infinity
 * per metre is nothing - it falls as the log of its length - and so the
 * charge is held to none in all (infinity floats, Kron-reduced out); with a
 * ground plane the plane takes it. With no returns every net is a signal,
 * the matrices as solved.
 */
struct SignalLines {
    QList<int> nets;                  ///< the signals' nets
    QList<QList<double>> c, c0, loss; ///< F/m
    QString problem;
    bool ok() const { return problem.isEmpty() && !c.isEmpty(); }
};
SignalLines signalsOf(const Extraction& extraction, double length);

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
    double rGround = 0;     ///< Ohm/m of it the ground's (Wheeler's rule only)
    double rDc = 0;         ///< Ohm/m at direct current (Wheeler's rule only)
    double g = 0;           ///< S/m, from the dielectrics' loss tangents
    double skinDepth = 0;   ///< m, in the metal at that frequency
    double surfaceResistance = 0;   ///< Ohm a square, the roughness's factor in it
    double roughnessFactor = 1;     ///< what the metal's roughness multiplies it by
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

/*!
 * The resistance matrix per metre of \a extraction's line at \a frequency
 * by Wheeler's rule (Extraction::PerMetre::wheeler): each metal's surface
 * resistance - a slab's, Re surfaceImpedance() - times its factor, and the
 * ground's times its own, a net's own resistance no less than at direct
 * current, where the current runs through the whole metal and not along
 * its faces. \a ground, given, is the ground's part alone. Empty where the
 * factors were not taken.
 */
QList<QList<double>> resistancePerMetre(const Extraction& extraction, double frequency,
                                        QList<QList<double>>* ground = nullptr);

/*!
 * What a metal's surface roughness \a rms (m) multiplies its surface
 * resistance by at \a frequency, for a metal of conductivity \a sigma:
 * Hammerstad and Bekkadal's 1 + (2/pi) atan(1.4 (rms/delta)^2), delta the
 * skin depth - nothing added while the roughness is small beside the skin
 * depth, and at most twice the loss once it is large, the current then
 * following the surface up and down. 1 where there is no roughness.
 */
double roughnessFactor(double rms, double sigma, double frequency);

/// The surface resistance of the ground of \a extraction at \a frequency,
/// Ohm a square: nothing for a perfect plane, the given one for a surface
/// impedance, a metal's 1/(sigma delta) for a metal plane.
double groundSurfaceResistance(const Extraction& extraction, double frequency);

/// The potential at \a point of a rectangle \a rect carrying unit total
/// charge spread evenly over it, times 4 pi eps, with the charge \a z out
/// of the plane: the closed form a uniformly charged rectangle has. Public
/// because it is what the tests hold the near terms to.
double rectanglePotential(const QRectF& rect, const QPointF& point, double z);

} // namespace qucs_s::rf25

#endif
