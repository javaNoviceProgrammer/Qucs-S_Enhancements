/*
 * rf25_wave.h - the retarded Green's functions of a layered stack: the
 *               vector and scalar potentials of a horizontal current and
 *               of its charge, at a frequency
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The first piece of phase C of
 * docs/proposals/2026-10-23-rf-2p5d-moment-method.md - "full-wave MPIE:
 * the spectral Green's functions ... and the Sommerfeld fallback" - and,
 * as the proposal says of the static one before it, the heart of the work
 * and the risk.
 *
 * The mixed-potential integral equation for currents on horizontal metal
 * sheets wants two kernels of the stack (Michalski and Zheng's
 * formulation C, for horizontal sources and fields):
 *
 *   - the vector potential of a horizontal current element, G_A, the same
 *     for x and y and with no cross term between them;
 *   - the scalar potential of a point charge, K_phi.
 *
 * Each is a Sommerfeld integral, G(rho) = 1/(2 pi) Int Gt(kr) J0(kr rho)
 * kr dkr, of a spectral function made of the stack's equivalent
 * transmission lines - one for TE, one for TM - the voltage at the field's
 * height of a unit current at the source's:
 *
 *     Gt_A   = V_h / (j w)
 *     Kt_phi = (j w / kr^2) (V_e - V_h)
 *
 * with the lines' impedances Z_e = kz/(w eps) and Z_h = w mu0/kz in each
 * layer, kz = sqrt(k^2 - kr^2) taken with Im kz <= 0, a ground plane a
 * short and the half-spaces matched. Each layer's permittivity is complex:
 * its loss tangent and, for a substrate, its conductivity are in it, so a
 * lossy silicon substrate is a case like any other.
 *
 * How the integral is done, which is where such solvers spend their
 * unplanned time:
 *
 *   - **The singular part in closed form.** As kr grows each kernel tends
 *     to c exp(-u dz)/u, u = sqrt(kr^2 - k^2), whose transform is
 *     c exp(-jkR)/(2 pi R) - Sommerfeld's identity. c comes from the
 *     interfaces at the source and the field (a charge on a dielectric
 *     interface has 1/(eps_a + eps_b)); k is chosen so that the 1/kr^3
 *     term matches too (on an interface, the harmonic mean of k^2 for the
 *     scalar, the arithmetic for the vector). Taken out, it leaves a
 *     remainder that is finite at rho = 0 and decays fast in kr. The
 *     vector potential's ground image goes out the same way.
 *   - **A path off the real axis.** The surface-wave poles and the branch
 *     points lie on the real kr axis (just below it, with loss). The
 *     remainder is integrated from 0 to beyond all of them on a
 *     half-ellipse in the upper half plane, where it is smooth, and from
 *     there along the real axis.
 *   - **The tail.** Beyond the ellipse, panels growing geometrically -
 *     until what is left is nothing where J0 hardly turns; and where it
 *     does, J0 split into Hankel's two functions and each half turned onto
 *     a vertical line, up and down, where it decays as exp(-t rho)
 *     however slowly the integrand dies along the real axis.
 *   - **A table over distance**, filled once per frequency and pair of
 *     heights and read by every pair of cells.
 *
 * What it does NOT do yet: vertical currents (a via's), which want the
 * kernels' zz and zx terms; and a metal lid over the stack.
 */
#ifndef QUCS_RF25_WAVE_H
#define QUCS_RF25_WAVE_H

#include "rf25_green.h"

#include <QList>
#include <QStringList>

#include <complex>

namespace qucs_s::rf25 {

using Complex = std::complex<double>;

/// Bessel's J0 of a complex argument, to about 1e-11: its power series
/// below |z| = 12, Hankel's asymptotic form above.
Complex besselJ0(Complex z);
/// Hankel's H0 of the first (\a kind 1) or second (2) kind, for Re z > 0:
/// J0 +- j Y0 by their series below |z| = 12, the asymptotic form above.
Complex hankelH0(Complex z, int kind);

/*!
 * The retarded Green's functions of \a layers at a frequency, between a
 * horizontal current (and its charge) at one height and the field at
 * another - two metal heights of the stack, or one.
 */
class WaveGreen
{
public:
    /// What is taken out of a kernel in closed form:
    /// coefficient exp(-j k R)/(2 pi R), R = sqrt(rho^2 + dz^2).
    struct Term {
        Complex coefficient;
        Complex k;           ///< 1/m, the wavenumber of its retardation
        double dz = 0;       ///< m, the height it is seen across
    };

    WaveGreen() = default;
    /// For a source at height \a zSource and a field at \a zField (m) in
    /// \a layers, at \a frequency (Hz), tabulated out to \a rhoMax (m).
    /// Invalid, with why in problems(), where a height is in no slab or
    /// the frequency is none.
    WaveGreen(const Layers& layers, double zField, double zSource, double frequency, double rhoMax);

    bool ok() const { return a_problems.isEmpty(); }
    QStringList problems() const { return a_problems; }
    double frequency() const { return a_frequency; }

    /// The scalar potential at distance \a rho (m) of a unit point charge,
    /// V/C: the closed-form part and the table's remainder.
    Complex scalar(double rho) const;
    /// The vector potential, H/m^2, of a unit horizontal current element
    /// (one ampere-metre) at distance \a rho, along its own direction -
    /// the xx term, which is also the yy.
    Complex vector(double rho) const;

    /// The parts taken out in closed form, for a method of moments to
    /// integrate over its cells: the scalar's singular term; the vector's,
    /// and its image in the ground (a coefficient of nothing without one).
    Term scalarTerm() const { return a_scalarTerm; }
    Term vectorTerm() const { return a_vectorTerm; }
    Term vectorImage() const { return a_vectorImage; }
    /// What is left, smooth and finite at nought, from the table.
    Complex scalarRemainder(double rho) const;
    Complex vectorRemainder(double rho) const;
    /// The same straight from the Sommerfeld integral, no table - on a
    /// half-ellipse of height \a ellipse (1/m), or the default for nought.
    /// For the tests, which hold the path's choice to make no difference.
    Complex scalarRemainderByIntegral(double rho, double ellipse = 0) const;
    Complex vectorRemainderByIntegral(double rho, double ellipse = 0) const;

    /// The spectral functions at \a kr (1/m, complex): the two kernels, and
    /// what of them the closed-form terms leave. For the tests, which hold
    /// them to the closed forms a grounded slab has.
    Complex scalarSpectral(Complex kr) const;
    Complex vectorSpectral(Complex kr) const;

    /// Each slab's complex permittivity at the frequency, F/m.
    Complex epsilonOf(int slab) const;

    /// Fills the table over distance, if it is not already: the first
    /// remainder asked for does it otherwise. Call it before handing the
    /// same Green's function to several threads.
    void prepare() const;
    /// How many distances the table holds (nought before prepare()).
    int tableSize() const { return int(a_rho.size()); }

private:
    struct Pair {
        Complex scalar, vector;
    };
    /// Both spectral kernels at \a kr, from one pass of the lines.
    Pair spectral(Complex kr) const;
    /// What the closed-form terms are spectrally, to take out.
    Pair extracted(Complex kr) const;
    /// The remainders' Sommerfeld integral at \a rho.
    Pair integral(double rho, double ellipse) const;
    Pair fromTable(double rho) const;

    Layers a_layers;
    double a_frequency = 0, a_omega = 0;
    double a_zField = 0, a_zSource = 0;
    int a_fld = -1, a_src = -1;
    QList<Complex> a_eps;            ///< each slab's, F/m
    Complex a_epsAbove, a_epsBelow;  ///< the half-spaces'
    double a_kMax = 0;               ///< 1/m, the largest Re k of the stack
    double a_decay = 0;              ///< 1/m, beyond which the remainder is nothing
    double a_reach = 0;              ///< m, dz and twice the nearest reflecting interface (0: none)
    Term a_scalarTerm, a_vectorTerm, a_vectorImage;
    double a_rhoMax = 0;             ///< m, how far the table goes
    mutable QList<double> a_rho;     ///< the table's distances, m, from nought
    mutable QList<Pair> a_table;     ///< the remainders there
    QStringList a_problems;
};

} // namespace qucs_s::rf25

#endif
