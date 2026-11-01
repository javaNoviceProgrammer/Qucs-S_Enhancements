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
 * their depth inside the face; moving it, and de-embedding a port's own
 * feed, are phase D's. An internal port drives the cell under its point,
 * against the ground or against the cell under it on another metal.
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
 * runs on both its faces, a via a bar with its current on its four faces.
 * What it does NOT do yet: a stack with no ground plane, since the ports
 * are referred to one; a metal's loss beyond what a sheet's crowding at its
 * edges gives (the quasi-static R of phase B is Wheeler's, of the thick
 * cross-section); and a port's own path up from the ground, which carries
 * no field of its own beyond the vias' correction - in a stack all of
 * vacuum, where a line over its ground hardly radiates, that path's share
 * of the radiation is missed, and S gives back up to a part in 1e4 more than it
 * gets.
 */
#ifndef QUCS_RF25_FULLWAVE_H
#define QUCS_RF25_FULLWAVE_H

#include "rf25_inductance.h"
#include "rf25_wave.h"

#include <QHash>
#include <QList>
#include <QPointF>
#include <QStringList>

#include <atomic>

namespace qucs_s::rf25 {

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
    /// An edge port's outward direction; nought: the face it is nearest.
    QPointF facing;
    double impedance = 50;     ///< its reference, Ohm
};

/// What a long solve is told, and tells: a flag that stops it between
/// frequencies, how many it has solved, and how many threads it may use.
struct FullWaveControl {
    std::atomic_bool* stop = nullptr;
    std::atomic_int* done = nullptr;
    int threads = 0;           ///< 0: as many as the machine has and the memory allows
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
    int threads = 1;                 ///< how many it used
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

} // namespace qucs_s::rf25

#endif
