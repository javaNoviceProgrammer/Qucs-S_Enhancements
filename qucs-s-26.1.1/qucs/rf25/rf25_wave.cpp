/*
 * rf25_wave.cpp - the retarded Green's functions of a layered stack
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_wave.h"

#include <QCoreApplication>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

constexpr double kMu0 = 4e-7 * M_PI;
const Complex kJ(0.0, 1.0);

/// Gauss-Legendre's sixteen nodes and weights on [-1, 1].
struct Gauss {
    double x[16];
    double w[16];
};

const Gauss& gauss16()
{
    static const Gauss rule = [] {
        Gauss g{};
        const int n = 16;
        for (int i = 0; i < (n + 1) / 2; ++i) {
            double z = std::cos(M_PI * (i + 0.75) / (n + 0.5));
            double pp = 1;
            for (int it = 0; it < 100; ++it) {
                double p1 = 1, p2 = 0;
                for (int j = 1; j <= n; ++j) {
                    const double p3 = p2;
                    p2 = p1;
                    p1 = ((2.0 * j - 1) * z * p2 - (j - 1.0) * p3) / j;
                }
                pp = n * (z * p1 - p2) / (z * z - 1);
                const double z1 = z;
                z = z1 - p1 / pp;
                if (std::abs(z - z1) < 1e-15) break;
            }
            g.x[i] = -z;
            g.x[n - 1 - i] = z;
            g.w[i] = g.w[n - 1 - i] = 2 / ((1 - z * z) * pp * pp);
        }
        return g;
    }();
    return rule;
}

/// kz = sqrt(k^2 - kr^2), the branch with Im kz <= 0: the wave that goes
/// out from the source and decays, which is the proper sheet everywhere
/// the integral goes (the cuts lie below the real axis).
Complex kzOf(Complex k2, Complex kr)
{
    Complex s = std::sqrt(k2 - kr * kr);
    if (s.imag() > 0) s = -s;
    return s;
}

/// exp(-j kz d).
Complex travel(Complex kz, double d)
{
    return std::exp(-kJ * kz * d);
}

/*!
 * The voltage at height \a zf (in slab \a f) of a unit current at height
 * \a zs (in slab \a m) on one of the stack's equivalent lines: \a Z and
 * \a kz each slab's impedance and kz, \a up the reflection looking up from
 * the top of each slab, \a down looking down from its bottom.
 *
 * At the source the line goes both ways: the wave going up has
 * Z (1 + Gd) / (2 (1 - Gu Gd)) and the one going down Z (1 + Gu) / (...),
 * Gu and Gd the reflections seen from it - no division by anything that
 * vanishes but at the Green's function's own poles. From there the
 * voltage is carried slab by slab, continuous across each interface.
 */
Complex lineVoltage(const Layers& layers, int m, double zs, int f, double zf, const Complex* Z, const Complex* kz,
                    const Complex* up, const Complex* down)
{
    const Layers::Slab& s = layers.slabs.at(m);
    const Complex gu = up[m] * travel(kz[m], 2 * (s.z1 - zs));
    const Complex gd = down[m] * travel(kz[m], 2 * (zs - s.z0));
    const Complex d = 1.0 - gu * gd;
    const Complex rising = Z[m] * (1.0 + gd) / (2.0 * d);
    const Complex falling = Z[m] * (1.0 + gu) / (2.0 * d);
    if (f == m) {
        if (zf >= zs) return rising * travel(kz[m], zf - zs) * (1.0 + up[m] * travel(kz[m], 2 * (s.z1 - zf)));
        return falling * travel(kz[m], zs - zf) * (1.0 + down[m] * travel(kz[m], 2 * (zf - s.z0)));
    }
    if (f > m) {
        Complex v = rising * travel(kz[m], s.z1 - zs) * (1.0 + up[m]);
        for (int n = m + 1; n <= f; ++n) {
            const Layers::Slab& t = layers.slabs.at(n);
            const double depth = t.z1 - t.z0;
            const Complex wave = v / (1.0 + up[n] * travel(kz[n], 2 * depth));
            if (n == f) return wave * travel(kz[n], zf - t.z0) * (1.0 + up[n] * travel(kz[n], 2 * (t.z1 - zf)));
            v = wave * travel(kz[n], depth) * (1.0 + up[n]);
        }
    }
    Complex v = falling * travel(kz[m], zs - s.z0) * (1.0 + down[m]);
    for (int n = m - 1; n >= f; --n) {
        const Layers::Slab& t = layers.slabs.at(n);
        const double depth = t.z1 - t.z0;
        const Complex wave = v / (1.0 + down[n] * travel(kz[n], 2 * depth));
        if (n == f) return wave * travel(kz[n], t.z1 - zf) * (1.0 + down[n] * travel(kz[n], 2 * (zf - t.z0)));
        v = wave * travel(kz[n], depth) * (1.0 + down[n]);
    }
    return v;
}

/// The stack's two equivalent lines at one kr: each slab's kz and its TM
/// and TE impedances, and the reflections looking up from the top of each
/// slab and down from its bottom.
struct Lines {
    QVarLengthArray<Complex, 8> kz, ze, zh, upE, upH, downE, downH;
};

/// The lines of \a layers at \a kr, \a eps each slab's permittivity at the
/// angular frequency \a w: the half-space above matched; below, a short
/// where there is a ground and matched where there is not.
Lines linesOf(const Layers& layers, const QList<Complex>& eps, Complex epsAbove, Complex epsBelow, double w, Complex kr)
{
    const int count = int(layers.slabs.size());
    Lines l;
    for (QVarLengthArray<Complex, 8>* a : {&l.kz, &l.ze, &l.zh, &l.upE, &l.upH, &l.downE, &l.downH}) a->resize(count);
    for (int n = 0; n < count; ++n) {
        l.kz[n] = kzOf(w * w * kMu0 * eps.at(n), kr);
        l.ze[n] = l.kz[n] / (w * eps.at(n));
        l.zh[n] = w * kMu0 / l.kz[n];
    }
    const Complex kzAbove = kzOf(w * w * kMu0 * epsAbove, kr);
    const Complex zeAbove = kzAbove / (w * epsAbove), zhAbove = w * kMu0 / kzAbove;
    l.upE[count - 1] = (zeAbove - l.ze[count - 1]) / (zeAbove + l.ze[count - 1]);
    l.upH[count - 1] = (zhAbove - l.zh[count - 1]) / (zhAbove + l.zh[count - 1]);
    for (int n = count - 2; n >= 0; --n) {
        const Complex e = travel(l.kz[n + 1], 2 * (layers.slabs.at(n + 1).z1 - layers.slabs.at(n + 1).z0));
        const Complex rE = (l.ze[n + 1] - l.ze[n]) / (l.ze[n + 1] + l.ze[n]), gE = l.upE[n + 1] * e;
        const Complex rH = (l.zh[n + 1] - l.zh[n]) / (l.zh[n + 1] + l.zh[n]), gH = l.upH[n + 1] * e;
        l.upE[n] = (rE + gE) / (1.0 + rE * gE);
        l.upH[n] = (rH + gH) / (1.0 + rH * gH);
    }
    if (layers.groundBelow) {
        l.downE[0] = l.downH[0] = Complex(-1, 0);
    } else {
        const Complex kzBelow = kzOf(w * w * kMu0 * epsBelow, kr);
        const Complex zeBelow = kzBelow / (w * epsBelow), zhBelow = w * kMu0 / kzBelow;
        l.downE[0] = (zeBelow - l.ze[0]) / (zeBelow + l.ze[0]);
        l.downH[0] = (zhBelow - l.zh[0]) / (zhBelow + l.zh[0]);
    }
    for (int n = 1; n < count; ++n) {
        const Complex e = travel(l.kz[n - 1], 2 * (layers.slabs.at(n - 1).z1 - layers.slabs.at(n - 1).z0));
        const Complex rE = (l.ze[n - 1] - l.ze[n]) / (l.ze[n - 1] + l.ze[n]), gE = l.downE[n - 1] * e;
        const Complex rH = (l.zh[n - 1] - l.zh[n]) / (l.zh[n - 1] + l.zh[n]), gH = l.downH[n - 1] * e;
        l.downE[n] = (rE + gE) / (1.0 + rE * gE);
        l.downH[n] = (rH + gH) / (1.0 + rH * gH);
    }
    return l;
}

} // namespace

// ------------------------------------------------------------ Bessel's J0

Complex besselJ0(Complex z)
{
    // (Even: Hankel's form below is for the right half-plane.)
    if (z.real() < 0) z = -z;
    if (std::abs(z) < 12.0) {
        // The power series, sum (-z^2/4)^k / (k!)^2: below 12 its terms
        // grow at most some thousands of times before they fall, so it
        // keeps about twelve digits.
        const Complex q = -z * z / 4.0;
        Complex term(1, 0), sum(1, 0);
        for (int k = 1; k < 80; ++k) {
            term *= q / double(k * k);
            sum += term;
            if (std::abs(term) < 1e-17 * std::abs(sum)) break;
        }
        return sum;
    }
    // Hankel's asymptotic form, J0 = (H0(1) + H0(2))/2 with
    // H0(1,2) ~ sqrt(2/(pi z)) exp(+-j(z - pi/4)) sum (+-j)^k a_k / z^k,
    // a_k = prod (-(2i - 1)^2) / (k! 8^k), summed while its terms fall:
    // at 12 the smallest is about exp(-24).
    Complex plus(1, 0), minus(1, 0), a(1, 0), jk(1, 0), mk(1, 0);
    double last = 1;
    for (int k = 1; k < 60; ++k) {
        a *= -double((2 * k - 1) * (2 * k - 1)) / (8.0 * k) / z;
        jk *= kJ;
        mk *= -kJ;
        const double size = std::abs(a);
        if (size > last) break;
        last = size;
        plus += jk * a;
        minus += mk * a;
        if (size < 1e-17) break;
    }
    const Complex phase = z - M_PI / 4;
    return std::sqrt(2.0 / (M_PI * z)) * 0.5 * (std::exp(kJ * phase) * plus + std::exp(-kJ * phase) * minus);
}

namespace {

/// Hankel's asymptotic series for H0 of the first kind (\a kind 1) or the
/// second (2): sqrt(2/(pi z)) exp(+-j(z - pi/4)) sum (+-j)^k a_k / z^k,
/// summed while its terms fall - at |z| = 12 the smallest is exp(-24).
Complex hankelAsymptotic(Complex z, int kind)
{
    const Complex turn = kind == 1 ? kJ : -kJ;
    Complex sum(1, 0), a(1, 0), power(1, 0);
    double last = 1;
    for (int k = 1; k < 60; ++k) {
        a *= -double((2 * k - 1) * (2 * k - 1)) / (8.0 * k) / z;
        power *= turn;
        const double size = std::abs(a);
        if (size > last) break;
        last = size;
        sum += power * a;
        if (size < 1e-17) break;
    }
    return std::sqrt(2.0 / (M_PI * z)) * std::exp(turn * (z - M_PI / 4)) * sum;
}

/// Bessel's Y0 for |z| below 12, by its series:
/// (2/pi)(ln(z/2) + gamma) J0(z) + (2/pi) sum (-1)^(k+1) H_k (z^2/4)^k / (k!)^2.
Complex besselY0Series(Complex z)
{
    const Complex q = z * z / 4.0;
    Complex term(1, 0), sum(0, 0);
    double harmonic = 0;
    for (int k = 1; k < 80; ++k) {
        term *= q / double(k * k);
        harmonic += 1.0 / k;
        const Complex add = (k % 2 == 1 ? 1.0 : -1.0) * harmonic * term;
        sum += add;
        if (std::abs(add) < 1e-17 * std::abs(sum)) break;
    }
    constexpr double gamma = 0.57721566490153286;
    return 2.0 / M_PI * ((std::log(z / 2.0) + gamma) * besselJ0(z) + sum);
}

} // namespace

Complex hankelH0(Complex z, int kind)
{
    // (In the right half-plane, which is where the integral's tail asks.)
    if (std::abs(z) >= 12.0) return hankelAsymptotic(z, kind);
    const Complex y = besselY0Series(z);
    return kind == 1 ? besselJ0(z) + kJ * y : besselJ0(z) - kJ * y;
}

namespace {

/*!
 * (1/2 pi) Int_0^inf f(kr) J0(kr rho) kr dkr of \a count functions at once,
 * \a values (kr, out) giving them all at a complex kr.
 *
 * The path is the one WaveGreen's remainders were first integrated on, and
 * each part of it is there for a reason found the hard way:
 *
 *   - From nought to a = 1.5 \a kMax, beyond every pole and branch point, on
 *     a half-ellipse above the real axis, kr = a/2 (1 - cos t) + j b sin t:
 *     the poles lie on the axis (below it, with loss), and J0 grows as
 *     exp(b rho) off it, so b - \a ellipse, or by default - is kept under
 *     1/rho but not so low as to graze them.
 *   - Along the real axis from there, in panels doubling in length: just
 *     past a the branch points beneath it still shape the integrand on the
 *     scale of k, which one panel to the first zero of J0 - thousands of
 *     times longer - could not see (a ground's image came out a part in a
 *     thousand wrong). On until a panel is as long as half a period of J0,
 *     or, where J0 hardly turns before it is all gone (\a decay, 1/m),
 *     until what one adds is nothing beside \a floors, each function's own.
 *     Far enough out a remainder is below the arithmetic's own noise - it
 *     is a kernel less its closed form, two numbers that agree to (k/kr)^4 -
 *     and the panels stop there whatever they add.
 *   - The rest along two vertical lines from where the panels stopped: J0 =
 *     (H0(1) + H0(2))/2, the first turned up into the upper half-plane and
 *     the second down, where each decays as exp(-t rho) however slowly the
 *     integrand dies along the real axis - which, with an oxide a few
 *     micrometres thick, it does over thousands of periods of J0 (an
 *     extrapolation of their sums ran out of terms a part in 1e5 short).
 *     Nothing lies between: every branch point and pole is left of it.
 *     \a reach (m), the distance the functions change over, sets the step.
 *
 *       Int_lo^inf f J0(kr rho) kr dkr
 *         = (j/2) Int_0^inf [f H0(1) kr](lo + jt) - [f H0(2) kr](lo - jt) dt
 */
template <typename Values>
void sommerfeld(double rho, double ellipse, double kMax, double decay, double reach, int count, const double* floors,
                const Values& values, Complex* out)
{
    const Gauss& g = gauss16();
    QVarLengthArray<Complex, 16> sum(count), r(count), part(count), tail(count), fu(count), fd(count);
    std::fill(sum.begin(), sum.end(), Complex(0, 0));
    const double a = std::max(1.5 * kMax, 1e-9);
    const double b = ellipse > 0 ? ellipse : std::min(0.5 * a, std::max(0.05 * a, rho > 0 ? 1.0 / rho : 0.5 * a));
    const int arcs = 16;
    for (int p = 0; p < arcs; ++p)
        for (int i = 0; i < 16; ++i) {
            const double t = M_PI * (p + (g.x[i] + 1) / 2) / arcs;
            const double weight = g.w[i] * M_PI / (2 * arcs);
            const Complex kr(a / 2 * (1 - std::cos(t)), b * std::sin(t));
            const Complex dk(a / 2 * std::sin(t), b * std::cos(t));
            values(kr, r.data());
            const Complex common = besselJ0(kr * rho) * kr * dk * weight;
            for (int c = 0; c < count; ++c) sum[c] += r[c] * common;
        }

    const auto panel = [&part, &r, &g, &values, rho, count](double lo, double hi) {
        std::fill(part.begin(), part.end(), Complex(0, 0));
        for (int i = 0; i < 16; ++i) {
            const double kr = lo + (hi - lo) * (g.x[i] + 1) / 2;
            const double common = besselJ0(kr * rho) * kr * g.w[i] * (hi - lo) / 2;
            values(Complex(kr, 0), r.data());
            for (int c = 0; c < count; ++c) part[c] += r[c] * common;
        }
    };
    // (Every function nothing beside its own sum, or below its floor.)
    const auto nothing = [&floors, count](const QVarLengthArray<Complex, 16>& added,
                                          const QVarLengthArray<Complex, 16>& total, double part) {
        for (int c = 0; c < count; ++c)
            if (!(std::abs(added[c]) <= part * std::abs(total[c]) + floors[c])) return false;
        return true;
    };
    const double end = std::max(decay, a);
    const double half = rho > 0 ? M_PI / rho : std::numeric_limits<double>::infinity();
    const double cap = std::max(end, 1e4 * a);
    double lo = a;
    for (int k = 0; k < 200; ++k) {
        if (lo >= half && (end - lo) * rho >= 4 * M_PI) break;
        const double hi = 2 * lo;
        panel(lo, hi);
        for (int c = 0; c < count; ++c) sum[c] += part[c];
        if (hi >= cap || (hi > end && nothing(part, sum, 1e-15))) {
            for (int c = 0; c < count; ++c) out[c] = sum[c] / (2 * M_PI);
            return;
        }
        lo = hi;
    }
    const double step = std::min(1.0 / rho, reach > 0 ? M_PI / (2 * reach) : 1.0 / rho);
    const double last = 40.0 / rho;
    std::fill(tail.begin(), tail.end(), Complex(0, 0));
    for (double t0 = 0; t0 < last; t0 += step) {
        std::fill(part.begin(), part.end(), Complex(0, 0));
        for (int i = 0; i < 16; ++i) {
            const double t = t0 + step * (g.x[i] + 1) / 2;
            const double weight = g.w[i] * step / 2;
            const Complex up(lo, t), down(lo, -t);
            values(up, fu.data());
            values(down, fd.data());
            const Complex hu = hankelH0(up * rho, 1) * up, hd = hankelH0(down * rho, 2) * down;
            for (int c = 0; c < count; ++c) part[c] += (fu[c] * hu - fd[c] * hd) * weight;
        }
        for (int c = 0; c < count; ++c) tail[c] += part[c];
        if (t0 * rho > 5 && nothing(part, tail, 1e-16)) break;
    }
    for (int c = 0; c < count; ++c) out[c] = (sum[c] + kJ / 2.0 * tail[c]) / (2 * M_PI);
}

} // namespace

// -------------------------------------------------------------- The kernels

WaveGreen::WaveGreen(const Layers& layers, double zField, double zSource, double frequency, double rhoMax)
    : a_layers(layers), a_frequency(frequency), a_zField(zField), a_zSource(zSource)
{
    if (!layers.ok()) {
        a_problems += layers.problems;
        if (a_problems.isEmpty()) a_problems << tr("The stack has no dielectric to solve in.");
        return;
    }
    if (!(frequency > 0) || !std::isfinite(frequency)) {
        a_problems << tr("A retarded Green's function is of a frequency above nought, not %1 Hz.").arg(frequency);
        return;
    }
    a_omega = 2 * M_PI * frequency;
    // A height within a hair of a boundary is on it: a metal's height and
    // the stack's are sums of the same thicknesses, but not always in one
    // order, and an exact comparison put 300 + 2 µm a rounding step below
    // a metal at 302 and the metal in no slab.
    const double hair = 1e-12 * std::max(1e-9, layers.height());
    const auto snapped = [&layers, hair](double z) {
        for (const Layers::Slab& s : layers.slabs)
            for (const double face : {s.z0, s.z1})
                if (std::abs(z - face) <= hair) return face;
        return z;
    };
    zSource = snapped(zSource);
    zField = snapped(zField);
    a_zSource = zSource;
    a_zField = zField;
    a_src = layers.slabAt(zSource);
    a_fld = layers.slabAt(zField);
    if (a_src < 0 || a_fld < 0) {
        a_problems << tr("A height of %1 µm or %2 µm is in no slab of the stack.")
                          .arg(zSource * 1e6, 0, 'g', 4)
                          .arg(zField * 1e6, 0, 'g', 4);
        return;
    }
    if (layers.groundBelow && (zSource <= 0 || zField <= 0)) {
        a_problems << tr("A metal on the ground plane itself is shorted to it, and has no potential of its own.");
        return;
    }

    // Each slab's permittivity at the frequency: its loss tangent and its
    // conductivity in its imaginary part, eps0 (eps_r (1 - j tand)) - j sigma/w.
    for (const Layers::Slab& s : layers.slabs)
        a_eps << kEps0 * Complex(s.epsr, -s.epsr * s.tand) - Complex(0, s.sigma / a_omega);
    a_epsAbove = kEps0 * layers.epsrAbove;
    a_epsBelow = kEps0 * layers.epsrBelow;
    const auto k2Of = [this](Complex eps) { return a_omega * a_omega * kMu0 * eps; };
    for (const Complex& e : a_eps) a_kMax = std::max(a_kMax, std::abs(std::sqrt(k2Of(e))));
    a_kMax = std::max({a_kMax, std::abs(std::sqrt(k2Of(a_epsAbove))), std::abs(std::sqrt(k2Of(a_epsBelow)))});

    const int count = int(layers.slabs.size());
    const int m = a_src, f = a_fld;
    const double tol = 1e-12 * std::max(1e-9, layers.height());
    const auto epsHigh = [&](int n) { return n + 1 < count ? a_eps.at(n + 1) : a_epsAbove; };
    const auto epsLow = [&](int n) { return n > 0 ? a_eps.at(n - 1) : a_epsBelow; };
    const auto onTop = [&](int n, double z) { return std::abs(z - layers.slabs.at(n).z1) <= tol; };
    const auto onBottom = [&](int n, double z) { return std::abs(z - layers.slabs.at(n).z0) <= tol; };
    // The static Fresnel coefficients of the TM line, whose impedances go
    // as 1/eps when kr is large: looking up out of slab n, and down.
    const auto fresnelUp = [&](int n) { return (a_eps.at(n) - epsHigh(n)) / (a_eps.at(n) + epsHigh(n)); };
    const auto fresnelDown = [&](int n) -> Complex {
        if (n == 0 && layers.groundBelow) return Complex(-1, 0);
        return (a_eps.at(n) - epsLow(n)) / (a_eps.at(n) + epsLow(n));
    };
    const auto localUp = [&](int n, double z) { return onTop(n, z) ? fresnelUp(n) : Complex(0, 0); };
    const auto localDown = [&](int n, double z) { return onBottom(n, z) ? fresnelDown(n) : Complex(0, 0); };

    // The scalar kernel's singular term. As kr grows only the interfaces
    // at the charge and at the field count - every other reflection has
    // a path to travel and dies as exp(-2 kr d) - so its strength is the
    // line's at the charge, Z (1 + Gd)/2, carried across the interfaces
    // between by (1 + r) each and seen at the field by (1 + Gu): a charge
    // on a dielectric interface has 1/(eps_a + eps_b), as it must.
    Complex factor;
    if (f == m) {
        const double low = std::min(zSource, zField), high = std::max(zSource, zField);
        factor = (1.0 + localDown(m, low)) * (1.0 + localUp(m, high)) / 2.0;
    } else if (f > m) {
        factor = (1.0 + localDown(m, zSource)) / 2.0;
        for (int n = m; n < f; ++n) factor *= 1.0 + fresnelUp(n);
        factor *= 1.0 + localUp(f, zField);
    } else {
        factor = (1.0 + localUp(m, zSource)) / 2.0;
        for (int n = m; n > f; --n) factor *= 1.0 + fresnelDown(n);
        factor *= 1.0 + localDown(f, zField);
    }
    const double dz = std::abs(zField - zSource);
    a_scalarTerm.coefficient = factor / a_eps.at(m);
    a_scalarTerm.dz = dz;
    a_vectorTerm.coefficient = kMu0 / 2;   // mu0 exp(-jkR)/(4 pi R): TE sees no dielectric as kr grows
    a_vectorTerm.dz = dz;

    // Their retardation: chosen so that the 1/kr^3 term matches as well,
    // which leaves the remainder falling as 1/kr^5. In one medium that is
    // its own k; on an interface between two, the harmonic mean of k^2
    // for the scalar (2 ka^2 kb^2 / (ka^2 + kb^2)) and the arithmetic for
    // the vector; across slabs, k^2 averaged along the way.
    const auto along = [&](double from, double to) {
        // (Each slab's k^2 times the length of [from, to] it holds.)
        Complex sum(0, 0);
        for (int n = 0; n < count; ++n) {
            const double lo = std::max(from, layers.slabs.at(n).z0), hi = std::min(to, layers.slabs.at(n).z1);
            if (hi > lo) sum += k2Of(a_eps.at(n)) * (hi - lo);
        }
        return sum;
    };
    Complex scalarK2, vectorK2;
    if (dz <= tol) {
        const Complex ka2 = k2Of(a_eps.at(m));
        Complex kb2 = ka2;
        if (onTop(m, zSource)) kb2 = k2Of(epsHigh(m));
        else if (onBottom(m, zSource) && !layers.groundBelow) kb2 = k2Of(epsLow(m));
        scalarK2 = 2.0 * ka2 * kb2 / (ka2 + kb2);
        vectorK2 = (ka2 + kb2) / 2.0;
    } else {
        scalarK2 = vectorK2 = along(std::min(zSource, zField), std::max(zSource, zField)) / dz;
    }
    a_scalarTerm.k = std::sqrt(scalarK2);
    a_vectorTerm.k = std::sqrt(vectorK2);
    // The vector potential's image in a ground: a horizontal current's is
    // the opposite current, and no dielectric changes that as kr grows.
    if (layers.groundBelow) {
        a_vectorImage.coefficient = -kMu0 / 2;
        a_vectorImage.dz = zSource + zField;
        a_vectorImage.k = std::sqrt((along(0, zSource) + along(0, zField)) / (zSource + zField));
    }

    // How fast what is left dies in kr: as exp(-kr (dz + 2 d)), d the
    // nearest interface that reflects and is at neither height - and as
    // 1/kr^5 for what the closed forms did not match.
    QList<double> faces;
    if (layers.groundBelow || std::abs(a_epsBelow - a_eps.first()) > 1e-9 * std::abs(a_eps.first()))
        faces << layers.slabs.first().z0;
    for (int n = 0; n + 1 < count; ++n)
        if (std::abs(a_eps.at(n) - a_eps.at(n + 1)) > 1e-9 * std::abs(a_eps.at(n))) faces << layers.slabs.at(n).z1;
    if (std::abs(a_epsAbove - a_eps.last()) > 1e-9 * std::abs(a_eps.last())) faces << layers.slabs.last().z1;
    double nearest = std::numeric_limits<double>::infinity();
    for (const double face : std::as_const(faces))
        for (const double z : {zSource, zField}) {
            const double d = std::abs(face - z);
            if (d > tol) nearest = std::min(nearest, d);
        }
    a_reach = std::isfinite(nearest) ? dz + 2 * nearest : 0;
    a_decay = std::max(1.5 * a_kMax, 200 * a_kMax);
    if (a_reach > 0) a_decay = std::max(a_decay, 46 / a_reach);
    a_rhoMax = std::max(rhoMax, 1e-9);
}

Complex WaveGreen::epsilonOf(int slab) const
{
    return slab >= 0 && slab < a_eps.size() ? a_eps.at(slab) : Complex(0, 0);
}

WaveGreen::Pair WaveGreen::spectral(Complex kr) const
{
    const Lines l = linesOf(a_layers, a_eps, a_epsAbove, a_epsBelow, a_omega, kr);
    const Complex ve =
        lineVoltage(a_layers, a_src, a_zSource, a_fld, a_zField, l.ze.data(), l.kz.data(), l.upE.data(), l.downE.data());
    const Complex vh =
        lineVoltage(a_layers, a_src, a_zSource, a_fld, a_zField, l.zh.data(), l.kz.data(), l.upH.data(), l.downH.data());
    return {kJ * a_omega / (kr * kr) * (ve - vh), vh / (kJ * a_omega)};
}

WaveGreen::Pair WaveGreen::extracted(Complex kr) const
{
    // c exp(-u dz)/u, u = j kz = sqrt(kr^2 - k^2): Sommerfeld's identity
    // makes it c exp(-jkR)/(2 pi R).
    const auto term = [&kr](const Term& t) {
        if (t.coefficient == Complex(0, 0)) return Complex(0, 0);
        const Complex u = kJ * kzOf(t.k * t.k, kr);
        return t.coefficient * std::exp(-u * t.dz) / u;
    };
    return {term(a_scalarTerm), term(a_vectorTerm) + term(a_vectorImage)};
}

Complex WaveGreen::scalarSpectral(Complex kr) const
{
    return ok() ? spectral(kr).scalar : Complex(0, 0);
}

Complex WaveGreen::vectorSpectral(Complex kr) const
{
    return ok() ? spectral(kr).vector : Complex(0, 0);
}

WaveGreen::Pair WaveGreen::integral(double rho, double ellipse) const
{
    // What is small enough to stop at: a part in 1e12 of the kernel's own
    // size at this distance.
    const double scale = 1.0 / (2 * M_PI * std::max({rho, a_reach, 1e-12}));
    const double floors[2] = {1e-13 * std::abs(a_scalarTerm.coefficient) * scale,
                              1e-13 * std::abs(a_vectorTerm.coefficient) * scale};
    Complex out[2];
    sommerfeld(
        rho, ellipse, a_kMax, a_decay, a_reach, 2, floors,
        [this](Complex kr, Complex* v) {
            const Pair full = spectral(kr), cut = extracted(kr);
            v[0] = full.scalar - cut.scalar;
            v[1] = full.vector - cut.vector;
        },
        out);
    return {out[0], out[1]};
}

void WaveGreen::prepare() const
{
    if (!ok() || !a_rho.isEmpty()) return;
    const double rhoMax = a_rhoMax;
    // From nought, then log-spaced from well inside the nearest image's
    // distance - forty a decade - until the step reaches a sixtieth of
    // the shortest wavelength, and evenly from there.
    double low = std::min(1e-3 * rhoMax, 1e-3 / a_kMax);
    if (a_reach > 0) low = std::min(low, 0.01 * a_reach);
    const double most = 2 * M_PI / (60 * a_kMax);
    const double ratio = std::pow(10.0, 1.0 / 40);
    a_rho << 0.0;
    for (double r = low;; r = std::min(r * ratio, r + most)) {
        a_rho << r;
        if (r >= rhoMax) break;
    }
    a_table.reserve(a_rho.size());
    for (const double r : std::as_const(a_rho)) a_table << integral(r, 0);
}

WaveGreen::Pair WaveGreen::fromTable(double rho) const
{
    prepare();
    if (a_rho.size() < 8 || rho >= a_rho.last()) return integral(std::max(rho, 0.0), 0);
    if (rho <= a_rho.at(1)) {
        // The remainder is even in rho: straight in rho^2 to the first.
        const double t = (rho / a_rho.at(1)) * (rho / a_rho.at(1));
        const Pair& p0 = a_table.at(0);
        const Pair& p1 = a_table.at(1);
        return {p0.scalar + t * (p1.scalar - p0.scalar), p0.vector + t * (p1.vector - p0.vector)};
    }
    // A quintic through the six nodes about it, of rho times the value -
    // which tends to a constant where the value goes as 1/rho. (A cubic
    // left a part in 1e5 where the far field turns over a wavelength.)
    const int i = int(std::upper_bound(a_rho.begin(), a_rho.end(), rho) - a_rho.begin()) - 1;
    const int j = std::clamp(i - 2, 1, int(a_rho.size()) - 6);
    Pair out{Complex(0, 0), Complex(0, 0)};
    for (int p = j; p < j + 6; ++p) {
        double weight = 1;
        for (int q = j; q < j + 6; ++q)
            if (q != p) weight *= (rho - a_rho.at(q)) / (a_rho.at(p) - a_rho.at(q));
        out.scalar += weight * a_rho.at(p) * a_table.at(p).scalar;
        out.vector += weight * a_rho.at(p) * a_table.at(p).vector;
    }
    return {out.scalar / rho, out.vector / rho};
}

Complex WaveGreen::scalarRemainder(double rho) const
{
    return ok() ? fromTable(rho).scalar : Complex(0, 0);
}

Complex WaveGreen::vectorRemainder(double rho) const
{
    return ok() ? fromTable(rho).vector : Complex(0, 0);
}

Complex WaveGreen::scalarRemainderByIntegral(double rho, double ellipse) const
{
    return ok() ? integral(rho, ellipse).scalar : Complex(0, 0);
}

Complex WaveGreen::vectorRemainderByIntegral(double rho, double ellipse) const
{
    return ok() ? integral(rho, ellipse).vector : Complex(0, 0);
}

namespace {

/// c exp(-jkR)/(2 pi R), R = sqrt(rho^2 + dz^2).
Complex closedForm(const WaveGreen::Term& t, double rho)
{
    if (t.coefficient == Complex(0, 0)) return Complex(0, 0);
    const double r = std::hypot(rho, t.dz);
    if (!(r > 0)) return Complex(std::numeric_limits<double>::infinity(), 0);
    return t.coefficient * std::exp(-kJ * t.k * r) / (2 * M_PI * r);
}

} // namespace

Complex WaveGreen::scalar(double rho) const
{
    if (!ok()) return Complex(0, 0);
    return closedForm(a_scalarTerm, rho) + fromTable(rho).scalar;
}

Complex WaveGreen::vector(double rho) const
{
    if (!ok()) return Complex(0, 0);
    return closedForm(a_vectorTerm, rho) + closedForm(a_vectorImage, rho) + fromTable(rho).vector;
}

// ------------------------------------------------------------ A via's kernels

namespace {

/// (exp(-u |s|) + u |s| - 1)/u^2: the second antiderivative of exp(-u |s|),
/// even, and nought with its slope at nought - by its series there, where
/// the three terms all but cancel.
Complex secondAntiderivative(Complex u, double s)
{
    const double a = std::abs(s);
    const Complex x = u * a;
    if (std::abs(x) < 1e-2)
        return a * a * (0.5 - x * (1.0 / 6 - x * (1.0 / 24 - x * (1.0 / 120 - x * (1.0 / 720 - x * (1.0 / 5040 - x / 40320.0))))));
    return (std::exp(-x) + x - 1.0) / (u * u);
}

/// Int_a0^a1 Int_b0^b1 exp(-u |z - z'|) dz dz', in the four terms of the
/// second antiderivative: right whether the two overlap, touch or are apart.
Complex pairIntegral(Complex u, double a0, double a1, double b0, double b1)
{
    return secondAntiderivative(u, a1 - b0) - secondAntiderivative(u, a1 - b1) - secondAntiderivative(u, a0 - b0)
           + secondAntiderivative(u, a0 - b1);
}

/// (1 - (1 + kr d) exp(-kr d))/kr^3: 1/kr^3 as kr grows and d^2/(2 kr) at
/// nought, so that (1/2 pi) Int ... J0(kr rho) kr dkr converges at both ends,
/// to (sqrt(d^2 + rho^2) - rho)/(2 pi). By its series where kr d is small.
Complex tailSpectral(Complex kr, double d)
{
    const Complex x = kr * d;
    if (std::abs(x) < 1e-2)
        return d * d * d * (0.5 / x - (1.0 / 3 - x * (1.0 / 8 - x * (1.0 / 30 - x * (1.0 / 144 - x / 840.0)))));
    return (1.0 - (1.0 + x) * std::exp(-x)) / (kr * kr * kr);
}

} // namespace

ViaGreen::ViaGreen(const Layers& layers, Span span, const QList<Span>& others, const QList<double>& heights,
                   double frequency, double rhoMax)
    : a_layers(layers), a_frequency(frequency)
{
    if (!layers.ok()) {
        a_problems += layers.problems;
        if (a_problems.isEmpty()) a_problems << tr("The stack has no dielectric to solve in.");
        return;
    }
    if (!(frequency > 0) || !std::isfinite(frequency)) {
        a_problems << tr("A retarded Green's function is of a frequency above nought, not %1 Hz.").arg(frequency);
        return;
    }
    a_omega = 2 * M_PI * frequency;
    for (const Layers::Slab& s : layers.slabs)
        a_eps << kEps0 * Complex(s.epsr, -s.epsr * s.tand) - Complex(0, s.sigma / a_omega);
    a_epsAbove = kEps0 * layers.epsrAbove;
    a_epsBelow = kEps0 * layers.epsrBelow;
    for (int n = 0; n < a_eps.size(); ++n) a_kMax = std::max(a_kMax, std::abs(std::sqrt(slabK2(n))));
    a_kMax = std::max({a_kMax, std::abs(std::sqrt(a_omega * a_omega * kMu0 * a_epsAbove)),
                       std::abs(std::sqrt(a_omega * a_omega * kMu0 * a_epsBelow))});

    // A via's parts, a slab each. Its metals sit on faces of the slabs, so
    // its ends are faces (to a hair: the heights are sums of the same
    // thicknesses, not always in one order) and it spans the slabs whole.
    const double hair = 1e-12 * std::max(1e-9, layers.height());
    const auto snapped = [&layers, hair](double z) {
        for (const Layers::Slab& s : layers.slabs)
            for (const double face : {s.z0, s.z1})
                if (std::abs(z - face) <= hair) return face;
        return z;
    };
    const auto partsOf = [&layers, &snapped, hair](Span s, QList<Segment>* parts) {
        s.z0 = snapped(s.z0);
        s.z1 = snapped(s.z1);
        if (!(s.z1 - s.z0 > hair)) return false;
        double covered = 0;
        for (int n = 0; n < layers.slabs.size(); ++n) {
            const Layers::Slab& slab = layers.slabs.at(n);
            const double lo = std::max(s.z0, slab.z0), hi = std::min(s.z1, slab.z1);
            if (!(hi - lo > hair)) continue;
            if (lo != slab.z0 || hi != slab.z1) return false;   // (a part of a slab)
            parts->append({n, lo, hi});
            covered += hi - lo;
        }
        // (And none of it beyond the stack.)
        return !parts->isEmpty() && std::abs(covered - (s.z1 - s.z0)) <= 2 * hair * parts->size();
    };
    const auto said = [](Span s) {
        return tr("A via from %1 to %2 µm does not span whole slabs of the stack: its ends are not metal heights.")
            .arg(s.z0 * 1e6, 0, 'g', 4)
            .arg(s.z1 * 1e6, 0, 'g', 4);
    };
    if (!partsOf(span, &a_own)) {
        a_problems << said(span);
        return;
    }
    for (const Span& o : others) {
        QList<Segment> parts;
        if (!partsOf(o, &parts)) {
            a_problems << said(o);
            return;
        }
        a_others << parts;
    }
    for (const double h : heights) {
        const double z = snapped(h);
        if (layers.slabAt(z) < 0) {
            a_problems << tr("A height of %1 µm is in no slab of the stack.").arg(h * 1e6, 0, 'g', 4);
            return;
        }
        a_heights << z;
    }
    a_count = int(a_others.size() + a_heights.size());

    // The static Fresnel coefficients of the TM line, whose impedances go as
    // 1/eps when kr is large: looking up out of slab n, and down.
    const int count = int(layers.slabs.size());
    const auto epsHigh = [this, count](int n) { return n + 1 < count ? a_eps.at(n + 1) : a_epsAbove; };
    const auto epsLow = [this](int n) { return n > 0 ? a_eps.at(n - 1) : a_epsBelow; };
    const auto fresnelUp = [this, &epsHigh](int n) { return (a_eps.at(n) - epsHigh(n)) / (a_eps.at(n) + epsHigh(n)); };
    const auto fresnelDown = [this, &epsLow, &layers](int n) -> Complex {
        if (n == 0 && layers.groundBelow) return Complex(-1, 0);
        return (a_eps.at(n) - epsLow(n)) / (a_eps.at(n) + epsLow(n));
    };

    // G_zz's closed forms: as kr grows, mu0 I_v^e / eps' is the free-space
    // direct term between parts in one slab, and its images in that slab's
    // faces - a vertical current's image in a ground is the same current,
    // in a dielectric face -Gamma of it; between neighbouring slabs the
    // direct term again, carried across with 2 eps_n/(eps_n + eps_m). The
    // images are always listed, a coefficient of nothing where a face does
    // not reflect, so that the list is the same at every frequency.
    const Complex half = kJ * a_omega * kMu0 / 2.0;
    for (const QList<Segment>& other : std::as_const(a_others)) {
        QList<Term> terms;
        for (const Segment& n : std::as_const(a_own))
            for (const Segment& m : other) {
                if (n.slab == m.slab) {
                    const Complex k = std::sqrt(slabK2(n.slab));
                    terms << Term{half, k, n.z0, n.z1, m.z0, m.z1};
                    terms << Term{-fresnelUp(n.slab) * half, k, n.z0, n.z1, 2 * n.z1 - m.z1, 2 * n.z1 - m.z0};
                    terms << Term{-fresnelDown(n.slab) * half, k, n.z0, n.z1, 2 * n.z0 - m.z1, 2 * n.z0 - m.z0};
                } else if (std::abs(n.slab - m.slab) == 1) {
                    const Complex k = std::sqrt((slabK2(n.slab) + slabK2(m.slab)) / 2.0);
                    const Complex carried = 2.0 * a_eps.at(n.slab) / (a_eps.at(n.slab) + a_eps.at(m.slab));
                    terms << Term{carried * half, k, n.z0, n.z1, m.z0, m.z1};
                }
            }
        a_terms << terms;
    }
    // The potential's at a face this via ends on: (w^2 mu0 / kr^2)(V_v^e -
    // V_v^h) there is the TM line's reflection in that face, +-Gamma/2 a
    // wave, integrated along the part below or above it - over j w,
    // -j w mu0 Gamma / (2 kr^3) at the top and + at the bottom. A ground
    // reflects both lines alike, and leaves nothing.
    for (const double h : std::as_const(a_heights)) {
        Tail t;
        if (std::abs(h - a_own.last().z1) <= hair) {
            const Segment& top = a_own.last();
            t.coefficient = -kJ * a_omega * kMu0 * fresnelUp(top.slab) / 2.0;
            t.d = top.z1 - top.z0;
        } else if (std::abs(h - a_own.first().z0) <= hair && !(layers.groundBelow && a_own.first().slab == 0
                                                               && a_own.first().z0 <= hair)) {
            const Segment& bottom = a_own.first();
            t.coefficient = kJ * a_omega * kMu0 * fresnelDown(bottom.slab) / 2.0;
            t.d = bottom.z1 - bottom.z0;
        }
        a_tails << t;
    }

    // What is left changes over the thinnest part of either via (the next
    // images are there), the slabs just beyond this one, and the distance
    // from a height to the nearest face of this via.
    double reach = std::numeric_limits<double>::infinity();
    for (const Segment& s : std::as_const(a_own)) reach = std::min(reach, s.z1 - s.z0);
    for (const QList<Segment>& other : std::as_const(a_others))
        for (const Segment& s : other) reach = std::min(reach, s.z1 - s.z0);
    for (const int n : {a_own.first().slab - 1, a_own.last().slab + 1})
        if (n >= 0 && n < count) reach = std::min(reach, layers.slabs.at(n).z1 - layers.slabs.at(n).z0);
    for (const double h : std::as_const(a_heights))
        for (const Segment& s : std::as_const(a_own))
            for (const double face : {s.z0, s.z1})
                if (std::abs(h - face) > hair) reach = std::min(reach, std::abs(h - face));
    a_reach = std::isfinite(reach) ? reach : 0;
    a_decay = std::max(1.5 * a_kMax, 200 * a_kMax);
    if (a_reach > 0) a_decay = std::max(a_decay, 46 / a_reach);
    a_rhoMax = std::max(rhoMax, 1e-9);
}

Complex ViaGreen::slabK2(int slab) const
{
    return a_omega * a_omega * kMu0 * a_eps.at(slab);
}

void ViaGreen::spectral(Complex kr, Complex* full) const
{
    const Lines l = linesOf(a_layers, a_eps, a_epsAbove, a_epsBelow, a_omega, kr);
    const double w = a_omega;
    // V_i between two heights, TM or TE: the voltage at zf of a unit shunt
    // current at zs.
    const auto voltage = [this, &l](bool tm, double zf, double zs) {
        return lineVoltage(a_layers, a_layers.slabAt(zs), zs, a_layers.slabAt(zf), zf, tm ? l.ze.data() : l.zh.data(),
                           l.kz.data(), tm ? l.upE.data() : l.upH.data(), tm ? l.downE.data() : l.downH.data());
    };
    const Complex jkzZh = kJ * w * kMu0;
    const auto jkzZe = [&l](int slab) { return kJ * l.kz[slab] * l.ze[slab]; };
    int c = 0;
    for (const QList<Segment>& other : std::as_const(a_others)) {
        Complex sum(0, 0);
        for (const Segment& n : std::as_const(a_own))
            for (const Segment& m : other) {
                const Complex second = voltage(true, n.z1, m.z1) - voltage(true, n.z1, m.z0) - voltage(true, n.z0, m.z1)
                                       + voltage(true, n.z0, m.z0);
                const double same = n.slab == m.slab ? n.z1 - n.z0 : 0.0;
                const Complex currents = (same - second / jkzZe(m.slab)) / jkzZe(n.slab);   // Int Int I_v^e
                sum += kMu0 * currents / a_eps.at(m.slab);
            }
        full[c++] = sum;
    }
    for (const double h : std::as_const(a_heights)) {
        Complex sum(0, 0);
        for (const Segment& m : std::as_const(a_own))
            sum += (voltage(true, h, m.z1) - voltage(true, h, m.z0)) / jkzZe(m.slab)
                   - (voltage(false, h, m.z1) - voltage(false, h, m.z0)) / jkzZh;
        full[c++] = -kJ * w * kMu0 / (kr * kr) * sum;
    }
}

Complex ViaGreen::symmetricSpectral(int other, Complex kr) const
{
    if (!ok() || other < 0 || other >= a_others.size()) return Complex(0, 0);
    const Lines l = linesOf(a_layers, a_eps, a_epsAbove, a_epsBelow, a_omega, kr);
    const double w = a_omega;
    const auto voltage = [this, &l](bool tm, double zf, double zs) {
        return lineVoltage(a_layers, a_layers.slabAt(zs), zs, a_layers.slabAt(zf), zf, tm ? l.ze.data() : l.zh.data(),
                           l.kz.data(), tm ? l.upE.data() : l.upH.data(), tm ? l.downE.data() : l.downH.data());
    };
    const Complex jkzZh = kJ * w * kMu0;
    const auto jkzZe = [&l](int slab) { return kJ * l.kz[slab] * l.ze[slab]; };
    Complex sum(0, 0);
    for (const Segment& n : std::as_const(a_own))
        for (const Segment& m : a_others.at(other)) {
            const auto second = [&](bool tm) {
                return voltage(tm, n.z1, m.z1) - voltage(tm, n.z1, m.z0) - voltage(tm, n.z0, m.z1) + voltage(tm, n.z0, m.z0);
            };
            const double same = n.slab == m.slab ? n.z1 - n.z0 : 0.0;
            const Complex ie = (same - second(true) / jkzZe(m.slab)) / jkzZe(n.slab);
            const Complex ih = (same - second(false) / jkzZh) / jkzZh;
            sum += kMu0 * ie * (1.0 / a_eps.at(n.slab) + 1.0 / a_eps.at(m.slab)) - w * w * kMu0 * kMu0 / (kr * kr) * (ie - ih);
        }
    return sum;
}

void ViaGreen::extracted(Complex kr, Complex* cut) const
{
    int c = 0;
    for (const QList<Term>& terms : std::as_const(a_terms)) {
        Complex sum(0, 0);
        for (const Term& t : terms) {
            if (t.coefficient == Complex(0, 0)) continue;
            // (Sommerfeld's identity: exp(-u |dz|)/u is exp(-jkR)/R's.)
            const Complex u = kJ * kzOf(t.k * t.k, kr);
            sum += t.coefficient * pairIntegral(u, t.a0, t.a1, t.b0, t.b1) / u;
        }
        cut[c++] = sum;
    }
    for (const Tail& t : std::as_const(a_tails))
        cut[c++] = t.coefficient == Complex(0, 0) ? Complex(0, 0) : t.coefficient * tailSpectral(kr, t.d);
}

void ViaGreen::integral(double rho, double ellipse, Complex* out) const
{
    // What is small enough to stop at: a part in 1e12 of a coupling's own
    // size at this distance, jw mu0/2 times the two vias' lengths over
    // 2 pi rho, and of the largest a potential can be.
    const double scale = 1.0 / (2 * M_PI * std::max({rho, a_reach, 1e-12}));
    const double own = a_own.last().z1 - a_own.first().z0;
    QVarLengthArray<double, 16> floors(a_count);
    int c = 0;
    for (const QList<Segment>& other : std::as_const(a_others))
        floors[c++] = 1e-13 * a_omega * kMu0 / 2 * own * (other.last().z1 - other.first().z0) * scale;
    for (int h = 0; h < a_heights.size(); ++h) floors[c++] = 1e-13 * a_omega * kMu0 / 2 * own * own * scale;
    const int count = a_count;
    sommerfeld(
        rho, ellipse, a_kMax, a_decay, a_reach, count, floors.data(),
        [this, count](Complex kr, Complex* v) {
            QVarLengthArray<Complex, 16> full(count), cut(count);
            spectral(kr, full.data());
            extracted(kr, cut.data());
            for (int i = 0; i < count; ++i) v[i] = full[i] - cut[i];
        },
        out);
}

void ViaGreen::prepare() const
{
    if (!ok() || !a_rho.isEmpty() || a_count == 0) return;
    // From nought, then log-spaced from well inside the thinnest slab -
    // forty a decade - until the step reaches a sixtieth of the shortest
    // wavelength, and evenly from there: WaveGreen's table.
    double low = std::min(1e-3 * a_rhoMax, 1e-3 / a_kMax);
    if (a_reach > 0) low = std::min(low, 0.01 * a_reach);
    const double most = 2 * M_PI / (60 * a_kMax);
    const double ratio = std::pow(10.0, 1.0 / 40);
    QList<double> rho{0.0};
    for (double r = low;; r = std::min(r * ratio, r + most)) {
        rho << r;
        if (r >= a_rhoMax) break;
    }
    QList<QList<Complex>> table;
    table.reserve(rho.size());
    QVarLengthArray<Complex, 16> out(a_count);
    for (const double r : std::as_const(rho)) {
        integral(r, 0, out.data());
        table << QList<Complex>(out.begin(), out.end());
    }
    a_table = table;
    a_rho = rho;
}

Complex ViaGreen::fromTable(int component, double rho) const
{
    prepare();
    if (a_rho.size() < 8 || rho >= a_rho.last()) {
        QVarLengthArray<Complex, 16> out(a_count);
        integral(std::max(rho, 0.0), 0, out.data());
        return out[component];
    }
    if (rho <= a_rho.at(1)) {
        const double t = (rho / a_rho.at(1)) * (rho / a_rho.at(1));
        return a_table.at(0).at(component) + t * (a_table.at(1).at(component) - a_table.at(0).at(component));
    }
    const int i = int(std::upper_bound(a_rho.begin(), a_rho.end(), rho) - a_rho.begin()) - 1;
    const int j = std::clamp(i - 2, 1, int(a_rho.size()) - 6);
    Complex out(0, 0);
    for (int p = j; p < j + 6; ++p) {
        double weight = 1;
        for (int q = j; q < j + 6; ++q)
            if (q != p) weight *= (rho - a_rho.at(q)) / (a_rho.at(p) - a_rho.at(q));
        out += weight * a_rho.at(p) * a_table.at(p).at(component);
    }
    return out / rho;
}

QList<ViaGreen::Term> ViaGreen::couplingTerms(int other) const
{
    return ok() && other >= 0 && other < a_terms.size() ? a_terms.at(other) : QList<Term>();
}

Complex ViaGreen::couplingRemainder(int other, double rho) const
{
    if (!ok() || other < 0 || other >= a_others.size()) return Complex(0, 0);
    return fromTable(other, rho);
}

ViaGreen::Tail ViaGreen::potentialTail(int height) const
{
    return ok() && height >= 0 && height < a_tails.size() ? a_tails.at(height) : Tail();
}

Complex ViaGreen::potentialRemainder(int height, double rho) const
{
    if (!ok() || height < 0 || height >= a_heights.size()) return Complex(0, 0);
    return fromTable(int(a_others.size()) + height, rho);
}

Complex ViaGreen::potential(int height, double rho) const
{
    if (!ok() || height < 0 || height >= a_heights.size()) return Complex(0, 0);
    const Tail& t = a_tails.at(height);
    const Complex closed = t.coefficient == Complex(0, 0)
                               ? Complex(0, 0)
                               : t.coefficient * (std::hypot(t.d, rho) - rho) / (2 * M_PI);
    return closed + potentialRemainder(height, rho);
}

Complex ViaGreen::couplingSpectral(int other, Complex kr, bool remainder) const
{
    if (!ok() || other < 0 || other >= a_others.size()) return Complex(0, 0);
    QVarLengthArray<Complex, 16> full(a_count), cut(a_count);
    spectral(kr, full.data());
    if (!remainder) return full[other];
    extracted(kr, cut.data());
    return full[other] - cut[other];
}

Complex ViaGreen::potentialSpectral(int height, Complex kr, bool remainder) const
{
    if (!ok() || height < 0 || height >= a_heights.size()) return Complex(0, 0);
    QVarLengthArray<Complex, 16> full(a_count), cut(a_count);
    spectral(kr, full.data());
    const int c = int(a_others.size()) + height;
    if (!remainder) return full[c];
    extracted(kr, cut.data());
    return full[c] - cut[c];
}

Complex ViaGreen::couplingRemainderByIntegral(int other, double rho, double ellipse) const
{
    if (!ok() || other < 0 || other >= a_others.size()) return Complex(0, 0);
    QVarLengthArray<Complex, 16> out(a_count);
    integral(rho, ellipse, out.data());
    return out[other];
}

Complex ViaGreen::potentialRemainderByIntegral(int height, double rho, double ellipse) const
{
    if (!ok() || height < 0 || height >= a_heights.size()) return Complex(0, 0);
    QVarLengthArray<Complex, 16> out(a_count);
    integral(rho, ellipse, out.data());
    return out[a_others.size() + height];
}

} // namespace qucs_s::rf25
