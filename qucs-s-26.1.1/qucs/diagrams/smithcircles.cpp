/*
 * smithcircles.cpp - circles of a two-port on the Smith chart (see
 * smithcircles.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "smithcircles.h"

#include <QObject>

#include <cmath>

namespace qucs_s::smith {

double SParameters::k() const
{
    const double p = std::abs(s12 * s21);
    if (!(p > 0)) return std::numeric_limits<double>::infinity();
    return (1 - std::norm(s11) - std::norm(s22) + std::norm(delta())) / (2 * p);
}

double SParameters::mu() const
{
    // (1 - |S11|^2) / (|S22 - delta S11*| + |S12 S21|)
    const double d = std::abs(s22 - delta() * std::conj(s11)) + std::abs(s12 * s21);
    return d > 0 ? (1 - std::norm(s11)) / d : std::numeric_limits<double>::infinity();
}

namespace {

// Where |a + b G| / |c + d G| style: the stability circle of the plane
// whose own S is \a own and whose other is \a other.
Circle stability(Complex own, Complex other, Complex delta, double p)
{
    Circle c;
    const double den = std::norm(own) - std::norm(delta);
    if (std::abs(den) < 1e-15) {
        c.ok = false;
        c.why = QObject::tr("a straight line, not a circle (|S|^2 = |delta|^2)");
        return c;
    }
    c.centre = std::conj(own - delta * std::conj(other)) / den;
    c.radius = p / std::abs(den);
    // The chart's centre (Gamma 0) gives |Gamma| = |S of the other port|:
    // stable there with it below 1; that side of the circle the stable one.
    const bool centreInside = std::abs(c.centre) < c.radius;
    const bool centreStable = std::abs(other) < 1;
    c.stableInside = centreInside == centreStable;
    return c;
}

} // namespace

Circle inputStability(const SParameters& s)
{
    // In the source's plane: |Gamma_out| = 1.
    return stability(s.s11, s.s22, s.delta(), std::abs(s.s12 * s.s21));
}

Circle outputStability(const SParameters& s)
{
    // In the load's plane: |Gamma_in| = 1.
    return stability(s.s22, s.s11, s.delta(), std::abs(s.s12 * s.s21));
}

Circle availableGain(const SParameters& s, double dB)
{
    Circle c;
    const double s21 = std::norm(s.s21);
    if (!(s21 > 0)) {
        c.ok = false;
        c.why = QObject::tr("no gain: S21 is 0");
        return c;
    }
    const double g = std::pow(10.0, dB / 10.0) / s21;   // the normalized gain
    const Complex c1 = s.s11 - s.delta() * std::conj(s.s22);
    const double p = std::abs(s.s12 * s.s21);
    const double den = 1 + g * (std::norm(s.s11) - std::norm(s.delta()));
    const double under = 1 - 2 * s.k() * p * g + p * p * g * g;
    if (std::abs(den) < 1e-15 || under < 0) {
        c.ok = false;
        c.why = QObject::tr("%1 dB is more than the available gain").arg(dB);
        return c;
    }
    c.centre = g * std::conj(c1) / den;
    c.radius = std::sqrt(under) / std::abs(den);
    return c;
}

Circle noise(double nfDb, double fminDb, Complex gammaOpt, double rn, double z0)
{
    Circle c;
    const double f = std::pow(10.0, nfDb / 10.0), fmin = std::pow(10.0, fminDb / 10.0);
    if (f < fmin || !(rn > 0) || !(z0 > 0)) {
        c.ok = false;
        c.why = f < fmin ? QObject::tr("%1 dB is below the minimum noise figure").arg(nfDb) : QObject::tr("no noise resistance");
        return c;
    }
    const double n = (f - fmin) / (4 * rn / z0) * std::norm(1.0 + gammaOpt);
    c.centre = gammaOpt / (n + 1);
    c.radius = std::sqrt(n * (n + 1 - std::norm(gammaOpt))) / (n + 1);
    return c;
}

} // namespace qucs_s::smith
