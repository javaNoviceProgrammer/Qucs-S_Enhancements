/*
 * rf25_fullwave.h - the full-wave solve of an RF 2.5D model: the current on
 *                   its metals at each frequency, and what its ports see
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Phase C of docs/proposals/2026-10-23-rf-2p5d-moment-method.md - "full-wave
 * MPIE: ... the dense fill and solve, the currents" - on the retarded Green's
 * functions of rf25_wave.h.
 *
 * The mixed-potential integral equation, discretized on the mesh as phase
 * B's inductance already is: the runs of current between cells that share
 * an edge - a rooftop on each pair of rectangles - and the charge each cell
 * holds, its potential taken at its centre:
 *
 *     (Zs l/w + jw L) I - A^T V = 0      a run's drop drives its current
 *     jw Q + A I = J                     a cell's charge is what flows into it
 *     V = P Q                            its potential, of every charge
 *
 * L is the runs' partial inductances and P the cells' potential
 * coefficients, both from the retarded Green's functions of the stack in
 * place of the static ones: their singular parts in closed form - two runs'
 * static partial inductance (phase B's, and its image in the ground) and a
 * rectangle's static potential - and the retardation of those, and the
 * stack's own remainder, at the centres. A is the incidence of runs on
 * cells. Eliminating the charge leaves a dense complex system in the runs'
 * currents alone:
 *
 *     (Zs l/w + jw L + A^T P A / jw) I = A^T P J / jw
 *
 * An edge port drives the face of the conductor it is on: its current goes
 * into the cells along that face, each by its share of the face's length,
 * and comes back through the ground plane; its voltage is those cells'
 * potentials averaged with the same shares, which keeps the port matrix
 * reciprocal. The reference plane is then at those cells' centres, half
 * their depth inside the face; taking the port's own discontinuity out and
 * moving the plane along its feed is rf25_deembed.h's, after the solve,
 * from the face's width and metal reported here. An internal port drives
 * the cell under its point, against the ground or against the cell under it
 * on another metal.
 *
 * A via is a run of current in z, from the cell of the metal under it - or
 * the ground, which takes its charge at nought - to the cell of the metal
 * over it; the via's cells that land on the same two cells are one run.
 * Formulation C wants two more kernels for it (ViaGreen): G_zz along the
 * vias, and the correction a vertical current makes to the scalar
 * potential, which a run in the plane sees as the difference between its
 * two ends - that is also what its own vector potential does along the via
 * (G_zx), so the matrix stays symmetric. The correction is taken at the
 * cells, as the node potentials are, and at a port's cells too, as though
 * the port's own path up from the ground carried it: round any path the
 * dielectric's part of the vias' inductance then cancels, as magnetostatics
 * says it must. A via to the ground stands at the centre of the cell it
 * lands on, where its current enters the metal.
 *
 * The frequencies are independent, and solved side by side on as many
 * threads as the machine has and the memory allows.
 *
 * A metal is a sheet with the surface impedance of a slab whose current
 * runs on both its faces - and more in the cells along its edges, where
 * the current crowds as close as the metal's thickness lets it (or, thin
 * beside its skin depth, its own resistance) - a via a bar with its current
 * on its four faces. Or, solved as its two faces (its "Solved as"), it is
 * two sheets, its bottom and its top at its thickness over it, the room
 * between them what is over the metal, joined along its edges by its walls
 * (thickened()): its walls hold field in the air beside them, which a
 * coplanar line's eps_eff feels (3 µm of copper in 50 µm gaps: 2 to 3 %
 * under the sheet's, as QFEM's is), and its current shares its faces as the
 * field has it, each face with its slab's surface impedance from its own
 * side and the two coupled through the metal (faceImpedances()). A port on
 * such a metal drives both faces of its face, half each, and an internal
 * port or a via up from it stands on its top. What it does NOT do yet: a
 * stack with no ground plane, since the ports are referred to one; and a
 * port's own path up from the ground, which carries no field of its own
 * beyond the vias' correction - in a stack all of vacuum, where a line over
 * its ground hardly radiates, that path's share of the radiation is missed,
 * and S gives back up to a part in 1e4 more than it gets.
 */
#ifndef QUCS_RF25_FULLWAVE_H
#define QUCS_RF25_FULLWAVE_H

#include "rf25_inductance.h"
#include "rf25_wave.h"

#include <QHash>
#include <QList>
#include <QPointF>
#include <QSizeF>
#include <QStringList>

#include <atomic>

namespace qucs_s::rf25 {

/// How high conductor \a name sits in \a stack, m: the dielectrics below
/// it summed, the ground at nought wherever the tree lists it. -1 where it
/// is no conductor of the stack.
double metalHeight(const Stack& stack, const QString& name);

/// A port of the full-wave solve, as the model's Ports say it.
struct FullWavePort {
    QString name;              ///< what it is called ("P1")
    QPointF at;                ///< where, the layout's units
    /// At a point - between two metals, or a metal and the ground - rather
    /// than along the face of a conductor.
    bool internal = false;
    /// An edge port's metal; an internal port's "to". Empty: whichever is
    /// there.
    QString metal;
    QString from;              ///< an internal port's other metal; empty: the ground
    /// An internal port's rectangle about its point, the layout's units:
    /// every cell of its metal in it driven together, by its share of the
    /// rectangle's area (and of the other metal's, against it). Empty: the
    /// one cell under the point.
    QSizeF size;
    /// An edge port's outward direction; nought: the face it is nearest.
    QPointF facing;
    /// An edge port that returns through the conductors beside it on its
    /// own metal, at its own edge's line - as a ground-signal-ground
    /// probe's grounds touch them - rather than through the ground plane:
    /// what a coplanar line wants, whose ground-plane return radiated (the
    /// phase C check of 1 November, F6). The grounds either side are tied
    /// together, and the solve shares the current out between them.
    bool coplanar = false;
    double impedance = 50;     ///< its reference, Ohm
    /// An edge port's own discontinuity to be taken out after the solve,
    /// and how far its reference plane is then moved in along its line, m
    /// (rf25_deembed.h; the solve itself takes no notice): along its feed,
    /// as two strips of it say, or along a line of given impedance (Ohm)
    /// and effective permittivity, where those are above nought.
    bool calibrate = false;
    double shift = 0;
    double lineImpedance = 0, lineEps = 0;
    /// Whether anything is to be done to it after the solve.
    bool deembeds() const { return calibrate || shift != 0; }
    /// Whether that wants strips of its feed solved.
    bool wantsStrips() const { return calibrate || (shift != 0 && !(lineImpedance > 0)); }
};

/// What de-embedding a port had, at the top frequency solved.
struct PortDeembedding {
    bool done = false;
    bool calibrated = false;   ///< its own discontinuity taken out, by strips of its feed
    bool givenLine = false;    ///< the shift along a line given, not the feed's
    double shift = 0;          ///< m, the reference plane moved in from the face
    double length = 0;         ///< the shorter calibration strip, the layout's units
    double longer = 0;         ///< and the longer
    bool coplanar = false;     ///< its strips a coplanar feed's, grounds and vias with it
    double firstVia = 0;       ///< the layout's units: where its feed's own line begins, in from the face
    /// Np/m: the most its strips read its line as giving back - their own
    /// ends coupling past it - which is then taken as losing nothing; nought
    /// where they did not.
    double gave = 0;
    Complex impedance;         ///< the feed line's, Ohm, in the port's own voltage and current
    double epsEff = 0;         ///< its effective permittivity, (beta c0 / w)^2
    double alpha = 0;          ///< its attenuation, Np/m
    double shuntC = 0;         ///< the port's own discontinuity: F at its face,
    double seriesL = 0;        ///< and H at the port
};

/*!
 * What strips of an edge port's feed are made of, to take the port's own
 * discontinuity out (rf25_deembed.h): the face's width on its metal and,
 * for a coplanar port, the conductors beside it on its edge's line - each
 * side's gap and width there - and the vias down them, which strips of
 * the feed must have as it has them: a grounded coplanar line without its
 * vias is another line, its grounds' own modes free.
 *
 * In the port's own frame: across, along the edge's line from the face's
 * lower end; in, from the face along the feed.
 */
struct Feed {
    QString metal;
    double width = 0;                  ///< the face's, the layout's units
    bool coplanar = false;
    /// Either side of the face along its edge - below its lower end, above
    /// its upper - the gap to the conductor beside it and that conductor's
    /// width on the line; nought where there is none.
    double gap[2] = {0, 0}, ground[2] = {0, 0};
    /// The first via of each row down the feed, or a wall.
    struct Via {
        QString layer;
        double across0 = 0, across1 = 0, in0 = 0, in1 = 0;
        bool wall = false;             ///< runs on the whole length of the feed from in0
    };
    QList<Via> vias;
    double pitch = 0;                  ///< the rows' period along the feed; nought: walls, or none
    /// Why strips cannot have the feed's vias as it has them, where they
    /// cannot: then it is not calibrated.
    QString unlike;
    /// Where the feed's own line begins, in from the face: the centre of
    /// the nearest first via of a row, or the far start of a wall - nought
    /// with none. Strips take the port's own discontinuity to there, and
    /// the line back to the face.
    double offset() const;
    /// One text for each feed alike, to share a calibration by.
    QString key() const;
};

/// What a long solve is told, and tells: a flag that stops it between
/// frequencies, how many it has solved, and how many threads it may use.
struct FullWaveControl {
    std::atomic_bool* stop = nullptr;
    std::atomic_int* done = nullptr;
    int threads = 0;           ///< 0: as many as the machine has and the memory allows
    /// The most runs and cells it sets up a dense solve for; 0: the build's
    /// own. (Lower, a test meets the limit without a solve of thousands.)
    int mostUnknowns = 0;
};

/// What the full-wave solve found.
struct FullWave {
    QList<double> frequencies;       ///< Hz
    /// At each frequency, the ports' impedance matrix, Ohm, and their
    /// scattering matrix, each port into its own reference: [f][row][column].
    QList<QList<QList<Complex>>> z, s;
    QStringList portNames;
    QList<double> references;        ///< Ohm, each port's
    int runs = 0, cells = 0;         ///< the unknowns: currents, and the charges they leave
    /// At each frequency, each port driven by an ampere in turn, the
    /// others open: each run's current, A - [f][port][run].
    QList<QList<QList<Complex>>> currents;
    /// Each edge port's face: how wide it is, the layout's units, and the
    /// metal it is on; nought and empty for an internal port or one on no
    /// face.
    QList<double> faceWidths;
    QStringList faceMetals;
    /// Each edge port's feed, as calibration strips of it are made: a
    /// coplanar one's grounds and vias with it. Empty for an internal port.
    QList<Feed> feeds;
    /// Where the ports were de-embedded (rf25_deembed.h): Z as solved, at
    /// the cells the ports drive, which is what their currents are of; and
    /// what each port had. Both empty where none was.
    QList<QList<QList<Complex>>> zSolved;
    QList<PortDeembedding> deembedding;
    QList<Filament> geometry;        ///< the runs themselves, beside the currents: in the plane, then the vias
    /// The cells of the mesh each run goes from and to; -1 from: the ground
    /// a via stands on.
    QList<QPair<int, int>> ends;
    QHash<int, int> viaCells;        ///< each via cell of the mesh: the run it is part of
    /// The worst over the frequencies of how far Z is from symmetric (its
    /// largest difference over its largest term) and how far the ports'
    /// S gives back more than it is given (the largest eigenvalue of
    /// S^H S, less one): both nought for a reciprocal, passive answer.
    double asymmetry = 0, gain = 0;
    /// The same of the ports as solved, before any de-embedding: the solve's
    /// own, beside what a de-embedding leaves.
    double solvedAsymmetry = 0, solvedGain = 0;
    int threads = 1;                 ///< how many it used
    /// Refused as more runs and cells than a dense solve is set up for.
    bool tooLarge = false;
    QStringList portsSaid;           ///< what each port drives
    QStringList problems;
    QStringList lines;               ///< what it did, for the panel
    bool ok() const { return problems.isEmpty() && !z.isEmpty(); }
};

/*!
 * The mesh of \a conductors on \a stack solved full-wave at each of
 * \a frequencies (Hz), driven at \a ports, each with its own reference.
 * \a micrometre is how many metres one unit of the layout is; \a control,
 * given, can stop it and counts the frequencies done.
 */
FullWave fullWaveOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack,
                    const QList<FullWavePort>& ports, const QList<double>& frequencies, double micrometre = 1e-6,
                    const FullWaveControl* control = nullptr);

/// The same with an edge port at each of \a ports (the layout's units),
/// on whichever face is nearest, each into \a reference Ohm.
FullWave fullWaveOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, const QList<QPointF>& ports,
                    const QList<double>& frequencies, double reference = 50, double micrometre = 1e-6);

/// The surface impedance of a metal sheet whose current runs on both its
/// faces, Ohm a square: (gamma / 2 sigma) coth(gamma t / 2), gamma =
/// (1 + j)/delta - 1/(sigma t) at direct current, and half a thick metal's
/// (1 + j)/(sigma delta) once it is many skin depths thick. Nothing for a
/// metal that says no conductivity or no thickness: it is perfect.
Complex sheetImpedance(double sigma, double thickness, double frequency);

/// Z parameters made S parameters, the same \a reference at every port:
/// (Z - Z0)(Z + Z0)^-1.
QList<QList<Complex>> scatteringOf(const QList<QList<Complex>>& z, double reference);
/// The same, each port into its own of \a references (real, above nought):
/// Kurokawa's power waves, R^-1/2 (Z - R)(Z + R)^-1 R^1/2.
QList<QList<Complex>> scatteringOf(const QList<QList<Complex>>& z, const QList<double>& references);

/// The surface current density on each cell of the mesh, A/m (its size;
/// the cells by their index in the mesh), at frequency \a k with port
/// \a port driven: by an ampere, the other ports open; or, \a terminated,
/// by a source of its own reference impedance sending a wave of a watt in,
/// the other ports ended in theirs - what a circuit does to it. The runs
/// on either side of a cell averaged along each of the two directions; a
/// via's own cells, its current round its sides per metre of them. Empty
/// where there is no such solve.
QHash<int, double> currentDensity(const FullWave& solved, int k, int port, bool terminated = false);

/// \a solved's ports as a Touchstone 1.1 file (.sNp), every port into
/// \a reference Ohm, \a comments at its head. Empty where it solved to
/// nothing.
QString touchstoneOf(const FullWave& solved, double reference, const QStringList& comments = {});
/// The same as Touchstone 2.0, each port into its own reference, which its
/// [Reference] says.
QString touchstone2Of(const FullWave& solved, const QStringList& comments = {});

} // namespace qucs_s::rf25

#endif
