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
    const int count = int(a_layers.slabs.size());
    QVarLengthArray<Complex, 8> kz(count), ze(count), zh(count), upE(count), upH(count), downE(count), downH(count);
    const double w = a_omega;
    for (int n = 0; n < count; ++n) {
        kz[n] = kzOf(w * w * kMu0 * a_eps.at(n), kr);
        ze[n] = kz[n] / (w * a_eps.at(n));
        zh[n] = w * kMu0 / kz[n];
    }
    // The half-space above, matched; below, a short where there is a
    // ground and matched where there is not.
    const Complex kzAbove = kzOf(w * w * kMu0 * a_epsAbove, kr);
    const Complex zeAbove = kzAbove / (w * a_epsAbove), zhAbove = w * kMu0 / kzAbove;
    upE[count - 1] = (zeAbove - ze[count - 1]) / (zeAbove + ze[count - 1]);
    upH[count - 1] = (zhAbove - zh[count - 1]) / (zhAbove + zh[count - 1]);
    for (int n = count - 2; n >= 0; --n) {
        const Complex e = travel(kz[n + 1], 2 * (a_layers.slabs.at(n + 1).z1 - a_layers.slabs.at(n + 1).z0));
        const Complex rE = (ze[n + 1] - ze[n]) / (ze[n + 1] + ze[n]), gE = upE[n + 1] * e;
        const Complex rH = (zh[n + 1] - zh[n]) / (zh[n + 1] + zh[n]), gH = upH[n + 1] * e;
        upE[n] = (rE + gE) / (1.0 + rE * gE);
        upH[n] = (rH + gH) / (1.0 + rH * gH);
    }
    if (a_layers.groundBelow) {
        downE[0] = downH[0] = Complex(-1, 0);
    } else {
        const Complex kzBelow = kzOf(w * w * kMu0 * a_epsBelow, kr);
        const Complex zeBelow = kzBelow / (w * a_epsBelow), zhBelow = w * kMu0 / kzBelow;
        downE[0] = (zeBelow - ze[0]) / (zeBelow + ze[0]);
        downH[0] = (zhBelow - zh[0]) / (zhBelow + zh[0]);
    }
    for (int n = 1; n < count; ++n) {
        const Complex e = travel(kz[n - 1], 2 * (a_layers.slabs.at(n - 1).z1 - a_layers.slabs.at(n - 1).z0));
        const Complex rE = (ze[n - 1] - ze[n]) / (ze[n - 1] + ze[n]), gE = downE[n - 1] * e;
        const Complex rH = (zh[n - 1] - zh[n]) / (zh[n - 1] + zh[n]), gH = downH[n - 1] * e;
        downE[n] = (rE + gE) / (1.0 + rE * gE);
        downH[n] = (rH + gH) / (1.0 + rH * gH);
    }
    const Complex ve = lineVoltage(a_layers, a_src, a_zSource, a_fld, a_zField, ze.data(), kz.data(), upE.data(), downE.data());
    const Complex vh = lineVoltage(a_layers, a_src, a_zSource, a_fld, a_zField, zh.data(), kz.data(), upH.data(), downH.data());
    return {kJ * w / (kr * kr) * (ve - vh), vh / (kJ * w)};
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
    const Gauss& g = gauss16();
    const auto remainder = [this](Complex kr) {
        const Pair full = spectral(kr), out = extracted(kr);
        return Pair{full.scalar - out.scalar, full.vector - out.vector};
    };
    // What is small enough to stop at: a part in 1e12 of the kernel's own
    // size at this distance.
    const double scale = 1.0 / (2 * M_PI * std::max({rho, a_reach, 1e-12}));
    const double floorS = 1e-13 * std::abs(a_scalarTerm.coefficient) * scale;
    const double floorV = 1e-13 * std::abs(a_vectorTerm.coefficient) * scale;

    // From nought to beyond every pole and branch point on a half-ellipse
    // above the real axis, kr = a/2 (1 - cos t) + j b sin t: the poles lie
    // on the axis (below it, with loss), and J0 grows as exp(b rho) off
    // it, so b is kept under 1/rho but not so low as to graze them.
    const double a = std::max(1.5 * a_kMax, 1e-9);
    const double b = ellipse > 0 ? ellipse : std::min(0.5 * a, std::max(0.05 * a, rho > 0 ? 1.0 / rho : 0.5 * a));
    Pair sum{Complex(0, 0), Complex(0, 0)};
    const int arcs = 16;
    for (int p = 0; p < arcs; ++p)
        for (int i = 0; i < 16; ++i) {
            const double t = M_PI * (p + (g.x[i] + 1) / 2) / arcs;
            const double weight = g.w[i] * M_PI / (2 * arcs);
            const Complex kr(a / 2 * (1 - std::cos(t)), b * std::sin(t));
            const Complex dk(a / 2 * std::sin(t), b * std::cos(t));
            const Pair r = remainder(kr);
            const Complex common = besselJ0(kr * rho) * kr * dk * weight;
            sum.scalar += r.scalar * common;
            sum.vector += r.vector * common;
        }

    // The real axis from there.
    const auto panel = [&](double lo, double hi) {
        Pair part{Complex(0, 0), Complex(0, 0)};
        for (int i = 0; i < 16; ++i) {
            const double kr = lo + (hi - lo) * (g.x[i] + 1) / 2;
            const double common = besselJ0(kr * rho) * kr * g.w[i] * (hi - lo) / 2;
            const Pair r = remainder(Complex(kr, 0));
            part.scalar += r.scalar * common;
            part.vector += r.vector * common;
        }
        return part;
    };
    const double decay = std::max(a_decay, a);
    const double half = rho > 0 ? M_PI / rho : std::numeric_limits<double>::infinity();
    // Beyond this the remainder is below the arithmetic's own noise: it is
    // the kernel less its closed form, two numbers that agree to (k/kr)^4,
    // and what is left of their difference is rounding.
    const double cap = std::max(decay, 1e4 * a);
    // First, panels doubling in length: just past a the branch points
    // beneath it still shape the integrand on the scale of k, which one
    // panel to the first zero of J0 - thousands of times longer - could
    // not see (a ground's image came out a part in a thousand wrong). On
    // until a panel is as long as half a period of J0, or, where J0
    // hardly turns before it is all gone, until what one adds is nothing.
    double lo = a;
    for (int k = 0; k < 200; ++k) {
        if (lo >= half && (decay - lo) * rho >= 4 * M_PI) break;
        const double hi = 2 * lo;
        const Pair part = panel(lo, hi);
        sum.scalar += part.scalar;
        sum.vector += part.vector;
        if (hi >= cap
            || (hi > decay && std::abs(part.scalar) <= 1e-15 * std::abs(sum.scalar) + floorS
                && std::abs(part.vector) <= 1e-15 * std::abs(sum.vector) + floorV))
            return {sum.scalar / (2 * M_PI), sum.vector / (2 * M_PI)};
        lo = hi;
    }
    // Then the rest along two vertical lines from lo: J0 = (H0(1) + H0(2))/2,
    // the first turned up into the upper half-plane and the second down,
    // where each decays as exp(-t rho) however slowly the integrand dies
    // along the real axis - which, with an oxide a few micrometres thick,
    // it does over thousands of periods of J0 (an extrapolation of their
    // sums ran out of terms a part in 1e5 short). Nothing lies between:
    // every branch point and pole is left of lo.
    //   Int_lo^inf f J0(kr rho) kr dkr
    //     = (j/2) Int_0^inf [f H0(1) kr](lo + jt) - [f H0(2) kr](lo - jt) dt
    const double step = std::min(1.0 / rho, a_reach > 0 ? M_PI / (2 * a_reach) : 1.0 / rho);
    const double last = 40.0 / rho;
    Pair tail{Complex(0, 0), Complex(0, 0)};
    for (double t0 = 0; t0 < last; t0 += step) {
        Pair part{Complex(0, 0), Complex(0, 0)};
        for (int i = 0; i < 16; ++i) {
            const double t = t0 + step * (g.x[i] + 1) / 2;
            const double weight = g.w[i] * step / 2;
            const Complex up(lo, t), down(lo, -t);
            const Pair fu = remainder(up), fd = remainder(down);
            const Complex hu = hankelH0(up * rho, 1) * up, hd = hankelH0(down * rho, 2) * down;
            part.scalar += (fu.scalar * hu - fd.scalar * hd) * weight;
            part.vector += (fu.vector * hu - fd.vector * hd) * weight;
        }
        tail.scalar += part.scalar;
        tail.vector += part.vector;
        if (t0 * rho > 5 && std::abs(part.scalar) <= 1e-16 * std::abs(tail.scalar) + floorS
            && std::abs(part.vector) <= 1e-16 * std::abs(tail.vector) + floorV)
            break;
    }
    sum.scalar += kJ / 2.0 * tail.scalar;
    sum.vector += kJ / 2.0 * tail.vector;
    return {sum.scalar / (2 * M_PI), sum.vector / (2 * M_PI)};
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

} // namespace qucs_s::rf25
