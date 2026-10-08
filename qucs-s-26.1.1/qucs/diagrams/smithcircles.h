/*
 * smithcircles.h - circles of a two-port on the Smith chart: where it is
 * stable (source and load), its available gain, its noise figure
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef QUCS_SMITHCIRCLES_H
#define QUCS_SMITHCIRCLES_H

#include <QString>

#include <complex>

/*!
 * A two-port's S-parameters at one frequency, and the circles amplifier
 * design draws on the Smith chart (Gonzalez, Microwave Transistor
 * Amplifiers): each a centre and a radius in the reflection coefficient's
 * plane - the source's (Gamma_S) for the input stability, available gain
 * and noise circles, the load's (Gamma_L) for the output stability circle.
 */
namespace qucs_s::smith {

using Complex = std::complex<double>;

struct SParameters {
    Complex s11, s12, s21, s22;
    Complex delta() const { return s11 * s22 - s12 * s21; }
    /// Rollett's stability factor: unconditionally stable with K > 1 and
    /// |delta| < 1.
    double k() const;
    /// Edwards and Sinsky's mu (the distance from the centre of the Smith
    /// chart to the nearest unstable load): unconditionally stable above 1.
    double mu() const;
};

struct Circle {
    Complex centre;
    double radius = 0;
    /// A stability circle's stable side: inside it (else outside).
    bool stableInside = false;
    bool ok = true;     ///< false: none (no such gain, a circle of no size)
    QString why;        ///< ... why
};

/// Where |Gamma_in| = 1 in the source's plane (input stability), and
/// |Gamma_out| = 1 in the load's (output stability).
Circle inputStability(const SParameters& s);
Circle outputStability(const SParameters& s);

/// Where the available gain is \a dB (the source's plane, the output
/// conjugately matched); none when it is beyond what the two-port gives.
Circle availableGain(const SParameters& s, double dB);

/// Where the noise figure is \a nfDb, of a two-port of the minimum noise
/// figure \a fminDb at \a gammaOpt, of noise resistance \a rn in a
/// system of \a z0 (the source's plane); none below the minimum.
Circle noise(double nfDb, double fminDb, Complex gammaOpt, double rn, double z0 = 50.0);

} // namespace qucs_s::smith

#endif
