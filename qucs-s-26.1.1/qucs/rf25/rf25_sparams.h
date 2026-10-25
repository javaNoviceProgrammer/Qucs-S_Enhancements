/*
 * rf25_sparams.h - the quasi-static extraction as a circuit: one net as a
 *                  transmission line, all of them as coupled lines, their
 *                  S-parameters, and the frequency above which none of it
 *                  is the whole story
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The last piece of phase B of docs/proposals/2026-10-23-rf-2p5d-moment-
 * method.md: "RLGC, Z0, and S-parameters below the first resonance".
 *
 * It is deliberately thin. The multiphysics engine already turns an R, L,
 * G and C per metre into S-parameters, a Touchstone file, a SPICE
 * subcircuit and a Qucs part, and it does so for N coupled lines as well
 * as for one - multiphysics/fem_export.h, tested against the coupled-line
 * model. What the RF 2.5D extraction produces *is* an R, L, G and C per
 * metre. So this file carries the extraction across into the engine's own
 * LineRLGC and CoupledLines and then gets out of the way, rather than
 * writing a second set of writers that could drift from the first.
 *
 * Two things are worth saying about what that gives:
 *
 *  - As a two-port of a given length, a line of a right RLGC is right at
 *    any frequency: the telegrapher's equations carry its own resonances.
 *    The "first resonance" the proposal names is not a limit of the
 *    S-parameters but of the quasi-static *cross-section* - the frequency
 *    at which the cross-section stops being electrically small and a mode
 *    other than quasi-TEM appears. quasiStaticLimit() is that frequency,
 *    and it is reported rather than assumed.
 *
 *  - A sweep costs one electrostatic solve, not one per frequency. C, C0
 *    and L do not move with frequency in a quasi-static extraction; only
 *    R (the skin effect) and G (w C tand) do. So a hundred frequencies
 *    cost the one solve and a hundred cheap evaluations.
 */
#ifndef QUCS_RF25_SPARAMS_H
#define QUCS_RF25_SPARAMS_H

#include "../multiphysics/fem_export.h"
#include "rf25_static.h"

namespace qucs_s::rf25 {

/*!
 * Net \a index of \a extraction as the multiphysics engine's own line: a
 * length \a length (m) of it, at \a frequency (Hz; none: the lossless
 * quasi-static values alone).
 *
 * Its error() carries whatever lineParameters() refused.
 */
fem::LineRLGC asLine(const Extraction& extraction, int index, double length, double frequency = 0);

/*!
 * Every net of \a extraction as coupled lines: a length \a length (m) of
 * them at \a frequency (Hz), their R, L, G, C and C0 per metre and the
 * modes of those.
 *
 * The matrices are the short-circuit (Maxwell) ones the engine's own
 * coupled lines use, so that its mode solver, its Zc matrix and its
 * 2N-port S-parameters apply unchanged:
 *
 *   - C  and C0 are what the charge solve found, per metre;
 *   - L  = C0^-1/c^2, the quasi-TEM relation, which is exact for a
 *          uniform line of any number of conductors;
 *   - G  = w times the loss matrix the nudged permittivities gave, so a
 *          layer's loss couples two conductors as well as loading one;
 *   - R  is diagonal: each net's surface resistance times how its own
 *          charge crowds. The proximity effect - one conductor pushing
 *          another's current about - is not in an electrostatic solve,
 *          so the off-diagonal terms are nothing and the write-up says
 *          so rather than letting a caller assume otherwise.
 *
 * A frequency of none gives the lossless line (R and G nothing), whose
 * modes are taken at 1 GHz as the engine's quasi-TEM ones are.
 */
fem::CoupledLines asCoupledLines(const Extraction& extraction, double length, double frequency = 0);

/*!
 * The frequencies a study asks for: \a points of them from \a from to
 * \a to (Hz), spaced logarithmically where \a logarithmic.
 *
 * Empty and why in \a problem where they are no sweep - no points, a
 * negative frequency, a logarithmic sweep from nothing.
 */
QList<double> sweepFrequencies(double from, double to, int points, bool logarithmic, QString* problem = nullptr);

/*!
 * Property \a key of \a node as a number, the parameters of \a model in
 * scope, \a fallback where it is not set.
 *
 * False in \a ok where it is set to something that is no constant, so
 * that a caller can tell "not given" from "given as nonsense" - the two
 * want different answers.
 */
double numberOf(const Node& node, const QString& key, const Model& model, double fallback, bool* ok = nullptr);

/*!
 * The frequencies the study of \a model asks for: its from, to, points
 * and spacing, read as the mesh reads its own.
 *
 * Empty and why in \a problem where the study says no sweep, or none that
 * can be swept.
 */
QList<double> studySweep(const Model& model, QString* problem = nullptr);

/// Net \a index of \a extraction at each of \a frequencies, as the
/// engine's own sweep - which its Touchstone writer takes. One
/// electrostatic solve stands for all of them.
fem::LineSweep lineSweep(const Extraction& extraction, int index, double length, const QList<double>& frequencies);
/// All of its nets at each of \a frequencies, as coupled lines.
fem::CoupledSweep coupledSweep(const Extraction& extraction, double length, const QList<double>& frequencies);

/// What quasiStaticLimit() found, and why that is the number.
struct QuasiStaticLimit {
    double frequency = 0;     ///< Hz; the cross-section stops being small above it
    double transverse = 0;    ///< m, the dimension that set it
    double epsilonMax = 1;    ///< the slowest dielectric it crosses
    QString what;             ///< which dimension it was ("the stack is 100 µm deep")
    /// The line's own first half-wave resonance, v/(2 length), Hz: not a
    /// limit of the model but where its S-parameters first turn over.
    double resonance = 0;
};

/*!
 * The frequency above which a quasi-static extraction of \a stack and
 * \a mesh is no longer the whole story.
 *
 * A per-metre RLGC is a property of the cross-section, and it holds while
 * the cross-section is electrically small. The dimension that matters is
 * the larger of how deep the dielectrics go and how wide the widest
 * conductor is, and the wavelength is that of the slowest dielectric the
 * field crosses; the limit is a tenth of that wavelength, which is the
 * usual engineering rule and is stated as such rather than dressed up.
 *
 * \a velocity (m/s, from the line's own eps_eff) and \a length (m) give
 * the line's first half-wave resonance beside it; zero where they are not
 * known.
 */
QuasiStaticLimit quasiStaticLimit(const Stack& stack, const Mesh& mesh, double micrometre = 1e-6, double velocity = 0, double length = 0,
                                  int conductors = 1);

} // namespace qucs_s::rf25

#endif
