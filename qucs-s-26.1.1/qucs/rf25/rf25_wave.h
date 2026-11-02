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
 *     heights and read by every pair of cells - and kept between solves
 *     (keepGreens()).
 *
 * A via's vertical current wants two more kernels of formulation C, which
 * ViaGreen below gives. What neither does yet is a metal lid over the
 * stack.
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
 * The kernels' tables, kept between solves. A stack's tables at a frequency
 * are the same for every structure on it, and a calibration's three strips,
 * the shorter ones beside them and a two-line pair each built them again -
 * since a port's path up from the ground has a field, most of a solve under
 * the sanitizers. A table's distances are a sequence of its own kernel's,
 * however far it is wanted: from nought, then log-spaced from well inside
 * the nearest image's distance and a thousandth of the wavelength over
 * 2 pi, and evenly from a sixtieth of the shortest wavelength. So a table
 * kept to a further distance holds a nearer one's nodes exactly, and each
 * kernel is handed the nodes its own table would have had and no more: what
 * it gives is the same to the last digit whatever was solved before it.
 * Kept by the stack, the heights or spans, the frequency and the grid; the
 * least lately used forgotten past a budget.
 */
struct GreensKept {
    int tables = 0;          ///< kept now
    double bytes = 0;        ///< what they hold
    long long built = 0;     ///< distances integrated since forgetGreens()
    long long reused = 0;    ///< distances handed over from a kept table
};
/// What is kept, and what was built and reused since forgetGreens().
GreensKept greensKept();
/// Forgets every kept table, and the counts.
void forgetGreens();
/// How much may be kept, bytes - 256 MB unless set; nought keeps nothing,
/// every table built as it is wanted. What no longer fits is forgotten.
void keepGreens(double bytes);

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
    /// The distance the remainders change over, m: the height between the
    /// two and twice the nearest interface that reflects - where the first
    /// image is. Nought where nothing reflects. What a method of moments
    /// judges a cell's size against, to integrate the remainder over it.
    double reach() const { return a_reach; }

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

/*!
 * The retarded kernels of a via: a vertical current, the same all along
 * it, from one metal height of the stack to another (or from the ground),
 * at a frequency. Formulation C, which WaveGreen's horizontal kernels are
 * of, wants two more for it (Michalski and Zheng; the line Green's
 * functions V and I of a shunt current source, i, or a series voltage
 * source, v, at z'):
 *
 *   - G_zz, the z-component of the vector potential of a vertical current,
 *     (mu0 / (j w eps(z'))) I_v^e(z|z'), integrated along this via and the
 *     other: the coupling, Ohm, of a unit current on the other via to the
 *     voltage along this one;
 *   - the correction a vertical current makes to the scalar potential,
 *     whose source derivative is (w^2 mu0 / kr^2)(V_v^e - V_v^h)(z|z'),
 *     integrated along the via and divided by j w: the potential, Ohm, its
 *     unit current adds at a metal height. A horizontal run's voltage gets
 *     the difference between its two ends - which by the formulation's
 *     reciprocity is also what the vector potential of the run's current
 *     does along the via (G_zx), so the matrix stays symmetric with no
 *     G_zx of its own.
 *
 * Integrated along whole slabs - a via always spans whole slabs, its metals
 * sitting on their faces - every one of them comes down to the voltages of
 * a shunt current source between the slabs' faces, the same lineVoltage
 * WaveGreen uses:
 *
 *     Int_n Int_m I_v = (delta_nm d_n - D^2 V_i / (j kz_m Z_m)) / (j kz_n Z_n)
 *     Int_m V_v(z|z') dz' = (V_i(z|top_m) - V_i(z|bottom_m)) / (j kz_m Z_m)
 *
 * D^2 V_i the second difference of V_i between slab n's faces and slab m's.
 *
 * As kr grows, G_zz tends to the free-space direct term between the parts
 * of the two vias in the same slab, with its images in that slab's two
 * faces: -Gamma each in a dielectric face (Gamma the TM Fresnel coefficient
 * looking out of the slab) and +1 in a ground; between neighbouring slabs
 * the direct term with 2 eps_n / (eps_n + eps_m), n this via's slab. These
 * are taken out in closed form - Term below, for a method of moments to
 * integrate between its two vias - and the rest tabulated over distance.
 * The potential, at a face this via ends on, tends to
 * -/+ j w mu0 Gamma / (2 kr^3), taken out as a closed form with that tail
 * and no trouble at nought, coefficient (sqrt(d^2 + rho^2) - rho)/(2 pi).
 */
class ViaGreen
{
public:
    /// A via's extent in height, m: z0 below z1.
    struct Span {
        double z0 = 0, z1 = 0;
    };
    /// What is taken out in closed form:
    /// coefficient Int_a0^a1 Int_b0^b1 exp(-jkR)/(2 pi R) dz dz', R = sqrt(rho^2 + (z - z')^2),
    /// [b0, b1] the other via's part, or its image in a face.
    struct Term {
        Complex coefficient;
        Complex k;           ///< 1/m
        double a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    };
    /// The potential's closed form: coefficient (sqrt(d^2 + rho^2) - rho)/(2 pi).
    struct Tail {
        Complex coefficient;
        double d = 0;        ///< m
    };

    ViaGreen() = default;
    /// The via spanning \a span in \a layers at \a frequency (Hz): its coupling
    /// to each via of \a others (itself among them, if wanted) and its
    /// potential at each of \a heights (metal heights, m), tabulated out to
    /// \a rhoMax (m). Invalid, with why in problems(), where a span is not
    /// whole slabs or a height is in none.
    ViaGreen(const Layers& layers, Span span, const QList<Span>& others, const QList<double>& heights, double frequency,
             double rhoMax);

    bool ok() const { return a_problems.isEmpty(); }
    QStringList problems() const { return a_problems; }
    double frequency() const { return a_frequency; }

    /// jw Int Int G_zz along this via and \a other, Ohm: what is taken out
    /// in closed form, and the rest at lateral distance \a rho (m).
    QList<Term> couplingTerms(int other) const;
    Complex couplingRemainder(int other, double rho) const;
    /// The potential at height \a height (an index into the heights) at
    /// lateral distance \a rho of a unit current up this via, Ohm: the
    /// closed form and the table's rest.
    Complex potential(int height, double rho) const;
    Tail potentialTail(int height) const;
    Complex potentialRemainder(int height, double rho) const;

    /// The spectral functions at \a kr (1/m), whole or (\a remainder) less
    /// their closed forms; and the remainders straight from the Sommerfeld
    /// integral, no table. For the tests.
    Complex couplingSpectral(int other, Complex kr, bool remainder = false) const;
    Complex potentialSpectral(int height, Complex kr, bool remainder = false) const;
    Complex couplingRemainderByIntegral(int other, double rho, double ellipse = 0) const;
    Complex potentialRemainderByIntegral(int height, double rho, double ellipse = 0) const;
    /// The coupling with the potential's part folded in, spectrally:
    /// Int Int mu0 I_v^e (1/eps + 1/eps') - (w^2 mu0^2/kr^2)(I_v^e - I_v^h),
    /// symmetric in the two vias. For the tests, which hold it to the
    /// coupling plus the other via's potential at this one's top less at
    /// its bottom.
    Complex symmetricSpectral(int other, Complex kr) const;

    double reach() const { return a_reach; }
    /// Fills the table over distance (the first remainder asked for does it
    /// otherwise). Call it before handing it to several threads.
    void prepare() const;
    int tableSize() const { return int(a_rho.size()); }

private:
    struct Segment {
        int slab = -1;
        double z0 = 0, z1 = 0;
    };
    /// Every component at \a kr - the couplings, then the potentials - and
    /// what the closed forms are of them.
    void spectral(Complex kr, Complex* full) const;
    void extracted(Complex kr, Complex* cut) const;
    void integral(double rho, double ellipse, Complex* out) const;
    Complex fromTable(int component, double rho) const;
    Complex slabK2(int slab) const;   ///< w^2 mu0 eps of a slab

    Layers a_layers;
    double a_frequency = 0, a_omega = 0;
    QList<Complex> a_eps;            ///< each slab's, F/m
    Complex a_epsAbove, a_epsBelow;
    double a_kMax = 0, a_decay = 0, a_reach = 0, a_rhoMax = 0;
    QList<Segment> a_own;            ///< this via's parts, a slab each
    QList<QList<Segment>> a_others;  ///< each other via's
    QList<double> a_heights;
    QList<QList<Term>> a_terms;      ///< each other via's closed forms
    QList<Tail> a_tails;             ///< each height's
    int a_count = 0;                 ///< components: others, then heights
    mutable QList<double> a_rho;
    mutable QList<QList<Complex>> a_table;   ///< [rho][component]
    QStringList a_problems;
};

} // namespace qucs_s::rf25

#endif
