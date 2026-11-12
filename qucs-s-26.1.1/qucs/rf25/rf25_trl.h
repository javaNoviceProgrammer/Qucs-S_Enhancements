/*
 * rf25_trl.h - a multiline TRL calibration of an RF 2.5D two-port: its
 *              error boxes - a GSG pad and its feed at each end, or any
 *              fixture - found from standards solved as the structure is,
 *              and taken out of its S (F3 of the open items of 8 November)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The strips' calibration (rf25_deembed.h) takes out a port's own face: a
 * feed's two lengths, solved as RF 2.5D makes them. A pad at a port - a
 * probe's landing, with its own fringing and its taper to the line - is no
 * part of that: it is a fixture, and what a fixture is is told by standards
 * made with it, as on a wafer. The thru (the two fixtures joined, the line
 * between them as long as the fixture's feed on each side of the device),
 * one line or more each longer than it, and a reflect (each fixture ended
 * alike, open or short) - each its own model, solved by RF 2.5D at the
 * structure's frequencies.
 *
 * The 8-term error model (Engen and Hoer, IEEE Trans. MTT 27 (1979) 987):
 * in the wave-cascading matrices of [b1; a1] = T [a2; b2], what is solved is
 * T_A T T_B, A the fixture at the first port and B at the second. Of the
 * thru and a line longer by l,
 *
 *     T_L T_T^-1 = T_A diag(e^-gl, e^gl) T_A^-1,   T_T^-1 T_L = T_B^-1 diag(e^-gl, e^gl) T_B
 *
 * so A's columns and B's rows are the eigenvectors: e00, e33, e01e10/e11
 * and e23e32/e22. The thru gives e11 e22 and e10 e32, the reflect - the
 * same unknown reflection at both - e11 and e22, its sign told by the
 * reflect being near an open or a short. With several lines (Marks, IEEE
 * Trans. MTT 39 (1991) 1205, whose weighting this simplifies) each
 * frequency takes its eigenvectors from the line whose phase beside the
 * thru's is furthest from 0 and 180 degrees, and the line's propagation
 * constant from all of them, each weighted by that distance; the lines not
 * taken are a check, corrected, of how near a matched line each comes.
 *
 * The corrected S is at the thru's middle - the device's model the
 * fixture with the thru's half on each side -, in the lines' own
 * impedance, which no calibration of lines alone can know: given it, S is
 * stated in the ports' references.
 */
#ifndef QUCS_RF25_TRL_H
#define QUCS_RF25_TRL_H

#include <QList>
#include <QStringList>

#include <array>
#include <complex>

namespace qucs_s::rf25 {

using Complex = std::complex<double>;
/// A two-port's S: S11, S12, S21, S22.
using S2 = std::array<Complex, 4>;

/// What the standards are, beside their S.
struct TrlStandards {
    QList<double> frequencies;          ///< Hz, ascending
    QList<S2> thru;                     ///< at each frequency
    QList<QList<S2>> lines;             ///< each line's, at each frequency
    QList<double> longer;               ///< each line's length beyond the thru's, m
    QList<S2> reflect;                  ///< S11 and S22 read: each end's reflection
    bool shortReflect = false;          ///< the reflect is near a short (else an open)
};

/// What the calibration found, at each frequency.
struct TrlCalibration {
    QList<double> frequencies;
    QList<Complex> gamma;               ///< the lines' propagation constant, 1/m
    QList<int> line;                    ///< which line's eigenvectors were taken
    QList<double> phase;                ///< that line's |sin(beta l)|: how well it tells e^-gl from e^gl
    QList<Complex> reflect;             ///< the reflect's reflection as found
    QList<double> residual;             ///< the most |S11| or |S22| of the other lines corrected (nought: one line)
    /// Each frequency's error boxes, their 7 terms: e00, e11, e01e10 of the
    /// first; e22, e33, e23e32 of the second; e10e32.
    QList<std::array<Complex, 7>> terms;
    QStringList problems;
    QStringList lines;                  ///< what it did, in words
    QStringList warnings;               ///< what makes it doubtful
    bool ok() const { return problems.isEmpty() && !terms.isEmpty(); }
};

/// The calibration of \a standards; \a epsGuess, the lines' effective
/// permittivity guessed, for the phase at the lowest frequency where one
/// line is longer than half a wave there.
TrlCalibration calibrateTrl(const TrlStandards& standards, double epsGuess = 1);

/// \a measured (at the calibration's frequencies) with its error boxes taken
/// out: at the thru's middle, in the lines' impedance.
QList<S2> corrected(const TrlCalibration& calibration, const QList<S2>& measured);

/// \a s, a two-port's in \a from Ohm at each port, in \a to Ohm.
S2 renormalized(const S2& s, double from, double to);

/// The wave-cascading matrix of [b1; a1] = T [a2; b2], and back.
std::array<Complex, 4> cascadeOf(const S2& s);
S2 scatteringOf(const std::array<Complex, 4>& t);

} // namespace qucs_s::rf25

#endif
