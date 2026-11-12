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
 * A port's current comes up to its cells by a path of its own: a via whose
 * current the port sets rather than the solve, from the ground - or from
 * the cells of the metal an internal port is driven from - to each cell it
 * drives, at the cell's centre, as wide as the face the cell has (an
 * internal port's, the cell's box). It has the field a via's current has:
 * its G_zz, with every via's along it, and the formulation's correction to
 * the scalar potential, which the runs see along them and which by
 * reciprocity is also their G_zx along it. Without that field the charge a
 * port brings up had its potential and the current that brings it none,
 * the scalar potential alone takes a vertical current's radiation the
 * wrong way, and S gave back more than it got once the ground was some way
 * down: twice on 635 µm of alumina at 41.5 GHz, 49 times over 2 mm of
 * vacuum (G9 of the phase E check). A path is part of its port's own
 * discontinuity, which rf25_deembed.h takes out; an internal port shorted
 * over a via's square is that via.
 *
 * A via is a run of current in z, from the cell of the metal under it - or
 * the ground, which takes its charge at nought - to the cell of the metal
 * over it; the via's cells that land on the same two cells are one run.
 * Formulation C wants two more kernels for it (ViaGreen): G_zz along the
 * vias, and the correction a vertical current makes to the scalar
 * potential, which a run in the plane sees as the difference between its
 * two ends - that is also what its own vector potential does along the via
 * (G_zx), so the matrix stays symmetric. The correction is taken at the
 * cells, as the node potentials are, and at a port's cells too, at the top
 * of the port's own path up from the ground: round any path the
 * dielectric's part of the vias' inductance then cancels, as magnetostatics
 * says it must. A via to the ground stands at the centre of the cell it
 * lands on, where its current enters the metal.
 *
 * The frequencies are independent, and solved side by side on as many
 * threads as the machine has and the memory allows.
 *
 * Past a few thousand runs and cells the system is not set up dense but
 * held compressed (rf25_fast.h, phase E): L as two kernels, of the runs
 * with a part along x and of those with a part along y, each between the
 * runs' parts along its axis - L_ab = (x_a x_b + y_a y_b) K_ab, the cosine
 * between two runs times a kernel smooth in where they are - and P, each
 * via's potential at each cell and the vias' couplings, each an H-matrix
 * whose blocks between clusters apart beside their size are of low rank.
 * A is applied as it is, so a loop of current, which leaves no charge, is
 * as free of P as in the dense solve. It is solved by GMRES, preconditioned
 * by the sparse LU of the same system between the near pairs alone, and
 * the ports' Z taken from the currents variationally, so that the
 * iteration's error enters it squared; as the tolerances go to nought it
 * is the dense solve. Some 310 000 unknowns, a patch of 2 mm in cells of
 * 6.25 µm, take 25 s and 6 GB a frequency on a desktop of 2026, where the
 * dense matrix would be 1.5 TB.
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
 * port or a via up from it stands on its top. With no ground plane under
 * the stack a port's current comes back through the layout's own
 * conductors: a coplanar port's through those beside it, an internal
 * port's through the metal it is driven from.
 *
 * Or a port names what it returns through: terminals - the cells of a
 * metal under a rectangle, or a conductor's face - and other ports, whose
 * own cells are then its return too. Each return is a column of its own,
 * the port's cells less the return's, and the columns of a port are tied
 * as a coplanar port's two grounds are; a source between its cells and
 * theirs, with no path up to them, as a probe's tips land on its pads.
 * Its current from its returns to its cells - and a coplanar port's across
 * its gaps - has the part of its field that radiates and not the static
 * part, the probe's own loop's inductance: without it the scalar potential
 * alone took that current's radiation the wrong way, minus half a dipole's,
 * and with no ground plane a probe on one side of its signal gave back.
 * Its columns and every other port's must be independent: two ports that
 * return through each other round a loop are one.
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
#include <functional>

namespace qucs_s::rf25 {

/// How high conductor \a name sits in \a stack, m: the dielectrics below
/// it summed, the ground at nought wherever the tree lists it. -1 where it
/// is no conductor of the stack.
double metalHeight(const Stack& stack, const QString& name);

/*!
 * What a port returns through where it names it (FullWavePort::terminals):
 * a terminal of the model - the cells of a metal under a rectangle about a
 * point, the one cell under the point, or the face of the conductor at the
 * point as an edge port's - or another port, whose own cells, what it
 * drives, are then this one's return as well, and it a port of its own.
 */
struct PortTerminal {
    QString name;              ///< what it is called: "T1", or the other port's "P2"
    /// Another port's own cells, by its name, rather than a terminal's.
    bool port = false;
    QPointF at;                ///< the layout's units
    QString metal;             ///< empty: whichever is there
    /// A rectangle about the point, the layout's units, its cells each by
    /// its share of the area; empty: the one cell under the point.
    QSizeF size;
    /// The face of the conductor at the point instead, its cells each by
    /// its share of the face's length, as an edge port's; \a facing its
    /// outward direction (nought: the face nearest the point).
    bool face = false;
    QPointF facing;
};

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
    /// A port, edge or internal, that returns through the terminals and the
    /// ports it names (\a returns) rather than the ground, the metal under it
    /// or the conductors beside it - a GSG probe's tips landing on its pads,
    /// the ground ring round a pad -, all of them tied: one voltage, the
    /// current shared out between them by the solve, as a coplanar port's
    /// two grounds are. A source between its own cells and theirs, with no
    /// path of its own up to them, so that the probe's own loop is not in
    /// the answer - its current from them to it has only the part of its
    /// field that radiates (a coplanar port's across its gaps alike) -; and
    /// no ground plane wanted.
    bool terminals = false;
    QList<PortTerminal> returns;
    /// An edge port's partner in a differential pair, by its name: this port
    /// its plus and that one its minus, as a probe's two signals drive a
    /// GSSG - the pair's mixed-mode S given beside the single-ended
    /// (mixedModeOf()). Empty: none.
    QString pair;
    double impedance = 50;     ///< its reference, Ohm
    /// An edge port's own discontinuity to be taken out after the solve,
    /// and how far its reference plane is then moved in along its line, m
    /// (rf25_deembed.h; the solve itself takes no notice): along its feed,
    /// as two strips of it say, or along a line of given impedance (Ohm)
    /// and effective permittivity, where those are above nought.
    bool calibrate = false;
    double shift = 0;
    double lineImpedance = 0, lineEps = 0;
    /// How long the shorter of the strips its discontinuity is found by is,
    /// m; nought: chosen (rf25_deembed.h).
    double strips = 0;
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
    /// How far its strips agree on one line between two ends alike: up to
    /// \a agreedTo, Hz, within a percent at every frequency; at most they
    /// differ by \a worst, at \a worstAt (Hz). Nought where not calibrated.
    double agreedTo = 0, worst = 0, worstAt = 0;
    /// Hz: the lowest frequency they do not agree at (nought: none); and
    /// whether they agree at the top, which the line and the discontinuity
    /// above are given at - where they do not, those are not to be trusted.
    double offFrom = 0;
    bool topAgrees = true;
    QList<double> strips;      ///< the shorter strips' lengths, the layout's units, longest first
    double third = 0;          ///< the third strip's, which checks the other two
    /// How far its feed runs straight from its face before the structure
    /// begins (below nought: not looked at), and how far its own field
    /// reaches along it - four heights over the ground and its width (nought
    /// for a coplanar feed) - the layout's units.
    double feedLength = -1, reach = 0;
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
    /// How far it runs straight in from the face, as wide as the face and
    /// nothing over or under it, the layout's units - at most as far as was
    /// looked; below nought: not looked at.
    double length = -1;
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
    /// The stack's layers that end: there only inside their shapes
    /// (rf25_dielectric.h) - their static polarization in the potential
    /// coefficients, the background's Green's functions with each beyond.
    QList<Extent> extents;
    std::atomic_bool* stop = nullptr;
    std::atomic_int* done = nullptr;
    /// Told, as a solve begins its frequencies - its setup done - how many
    /// they are: with done, how far several solves under one control have
    /// got, a calibration's two strips (rf25_deembed.h).
    std::function<void(int)> begins;
    /// Tests: how many frequencies more may begin, each taking one, the
    /// next waiting while there are none; below nought, any number, as with
    /// none given. A sweep held part way is seen part way in any build, not
    /// only in one slow enough to be caught at it.
    std::atomic_int* allowed = nullptr;
    int threads = 0;           ///< 0: as many as the machine has and the memory allows
    /// The most runs and cells it sets up a dense solve for; 0: the build's
    /// own. (Lower, a test meets the limit without a solve of thousands.)
    int mostUnknowns = 0;
    /// How the system is held: dense up to mostUnknowns and compressed past
    /// it (Automatic), or always the one asked - a dense solve past the
    /// limit refused, as it was before there was a compressed one.
    enum class Matrix { Automatic, Dense, Compressed };
    Matrix matrix = Matrix::Automatic;
    /// The compressed matrix's tolerance, each low-rank block's of its own
    /// size; 0: the build's own (1e-5).
    double tolerance = 0;
    /// The most memory a frequency's compressed solve may take, bytes; 0:
    /// half the machine's. (Lower, a test meets the bound.)
    double mostBytes = 0;
    /// A compressed solve's GMRES as it goes, for its bar: the most
    /// iterations the frequencies under way have taken so far and the
    /// residual of the one furthest on (B3 of the open items of 8 November:
    /// "0 of 1 frequencies" for fifteen minutes said nothing of it).
    std::atomic_int* gmresIterations = nullptr;
    std::atomic<double>* gmresResidual = nullptr;
    /// How many frequencies it solves at a time, once it knows: a sweep of
    /// them all at once counts none done until they end together, which its
    /// bar says (the review of 2 November, W1: "0 of 11" throughout).
    std::atomic_int* atOnce = nullptr;
    /// Tests: the most GMRES iterations a port's compressed solve takes; 0:
    /// the build's own (3000). (Lower, a test meets the limit.)
    int mostIterations = 0;
    /// Tests: false leaves the ports' own paths up to their cells out
    /// (PortPath), each port an ideal source between its cells as one that
    /// names its returns is - a port from a metal then the same columns as
    /// one returning through a terminal of that metal under it.
    bool portPaths = true;
};

/// RF 2.5D > Settings…, for the process: the most runs and cells a dense
/// system is set up for, and the memory a dense sweep's frequencies at once
/// may hold, bytes - a frequency some 16 (nr^2 + nc^2), its system and P,
/// the system factorized where it is (rf25_dense.h). 0: Automatic's. A
/// FullWaveControl's mostUnknowns is over the limit.
void setDenseLimit(int runsAndCells);
void setSweepMemory(double bytes);
/// What is in effect.
int denseLimit();
double sweepMemory();
/// Automatic's: the build's limit, 6000, whichever LU factorizes
/// (rf25_dense.h); and a quarter of the machine's memory, 1.5 GB at least.
int automaticDenseLimit();
double automaticSweepMemory();

/*!
 * A return of a port as solved: of a coplanar port, each conductor beside
 * it; of one that names its returns, each terminal and port named - how
 * many cells of it the port drives against, and its share of the port's
 * current at the top frequency, an ampere into the port and the other
 * ports open (the shares add up to one; a symmetric GSG's are a half each).
 */
struct PortReturn {
    QString name;
    int cells = 0;
    Complex share;
};

/// What the full-wave solve found.
struct FullWave {
    QList<double> frequencies;       ///< Hz
    /// Where an adaptive sweep solved some of the frequencies and read the
    /// rest from a rational fit of Z through them (rf25_adaptive.h): those
    /// it solved, Hz. Empty where every frequency was solved.
    QList<double> solvedFrequencies;
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
    /// The differential pairs: each its plus port's index and its minus's.
    QList<QPair<int, int>> pairs;
    /// Each port's returns, where it has more than the ground or a metal
    /// under it (PortReturn): a coplanar port's conductors beside it, and
    /// the terminals and ports a port names. Whether each port names its
    /// returns (FullWavePort::terminals): its reference plane is then at its
    /// own cells and theirs, an ideal source between them.
    QList<QList<PortReturn>> returnsOf;
    QList<bool> namesReturns;
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
    /// Hz: where each gave back the most (nought where nothing).
    double gainAt = 0, solvedGainAt = 0;
    /// Hz: where the stack begins to carry a second surface wave - TE1 over
    /// a ground plane - the top of a microstrip's band on it; nought where
    /// there is none below twice the sweep's top.
    double secondWave = 0;
    int threads = 1;                 ///< how many it used
    /// Dense: the seconds every frequency's fill and LU took, each on its
    /// own thread (rf25_fullwave.cpp's AtFrequency) - an LU's not counting
    /// its wait for OpenBLAS's thread -, and whether Accelerate's LAPACK or
    /// OpenBLAS's factorized them (rf25_dense.h).
    double fillSeconds = 0, luSeconds = 0;
    bool accelerated = false;
    bool openBlas = false;
    /// Held compressed and solved by iteration (rf25_fast.h), not dense: the
    /// low-rank blocks' tolerance; what the matrices of the frequency that
    /// took the most held, bytes, beside what they would dense; their
    /// largest rank; and of every frequency and port, the most iterations
    /// GMRES took and the largest residual it left.
    bool compressed = false;
    double tolerance = 0;
    double memory = 0, denseMemory = 0;
    int largestRank = 0;
    int iterations = 0;
    double residual = 0;
    /// Refused as more runs and cells than a dense solve is set up for -
    /// or, compressed, as more memory than a solve may take.
    bool tooLarge = false;
    QStringList portsSaid;           ///< what each port drives
    QStringList problems;
    QStringList lines;               ///< what it did, for the panel
    /// What makes the answer, or part of it, not to be trusted - said
    /// among the lines too, and kept apart for whoever reads only these.
    QStringList warnings;
    bool ok() const { return problems.isEmpty() && !z.isEmpty(); }
};

/// The frequency, Hz, from which \a stack carries a second surface wave -
/// TE1 over a ground plane, or the slab's first TE wave past the one with no
/// cutoff where there is none - as a lossless stack; nought where there is
/// none up to \a most.
double secondSurfaceWave(const Stack& stack, double most);

/*!
 * The mesh of \a conductors on \a stack solved full-wave at each of
 * \a frequencies (Hz), driven at \a ports, each with its own reference.
 * \a micrometre is how many metres one unit of the layout is; \a control,
 * given, can stop it and counts the frequencies done.
 */
FullWave fullWaveOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack,
                    const QList<FullWavePort>& ports, const QList<double>& frequencies, double micrometre = 1e-6,
                    const FullWaveControl* control = nullptr, const QList<ReturnMark>& returns = {});

/// What solves' frequencies have had made on threads other than their own
/// since the process began, for tests. A frequency is given the threads
/// that the frequencies at a time leave (FullWaveControl::threads), and its
/// Green's functions' tables are made on them a distance to a thread; a
/// dense solve's matrices, a column (or a block of columns) to a thread. The
/// dense solve's static parts, made once before its frequencies, are shared
/// out alike among all of the solve's threads (statics).
struct FillShared {
    long long greens = 0;    ///< the tables' distances
    long long columns = 0;   ///< columns, or blocks of them
    long long statics = 0;   ///< the static parts' columns
};
FillShared fillShared();

/*!
 * The mixed-mode S of frequency \a k of \a solved: each differential
 * pair's (FullWave::pairs) differential and common mode, then each port of
 * no pair as it is - D1, D2..., C1, C2..., then the rest - each pair's two
 * ports of one reference impedance, the differential mode's twice it and
 * the common mode's half, so that it is T S T' with T orthogonal. \a order
 * names the modes ("D1"), and \a of says of which ports ("P1 - P2").
 * Empty where there is no pair.
 */
QList<QList<Complex>> mixedModeOf(const FullWave& solved, int k, QStringList* order = nullptr, QStringList* of = nullptr);

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

/// How each port of \a solved with more than one return shares its current
/// out between them at the top frequency (FullWave::returnsOf), a line a
/// port: "At 60 GHz P1's current comes back 50.0 % through T1, 50.0 %
/// through T2." - a share's phase where it is a twentieth of a degree or
/// more. Empty where none has more than one.
QStringList returnSharesOf(const FullWave& solved);

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
/// [Reference] says - or, \a mixed, the mixed-mode S of its differential
/// pairs (mixedModeOf()), as its [Mixed-Mode Order] says: each pair's
/// differential mode, then each common mode, then the ports of no pair.
/// Empty where it solved to nothing, or \a mixed and it has no pair.
QString touchstone2Of(const FullWave& solved, const QStringList& comments = {}, bool mixed = false);

} // namespace qucs_s::rf25

#endif
