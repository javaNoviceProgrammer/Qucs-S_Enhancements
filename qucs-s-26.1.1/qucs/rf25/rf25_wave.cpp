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

#include "rf25_fast.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QVarLengthArray>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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

/// The principal square root of \a z, as std::sqrt's - its cut along the
/// negative real axis, the sign of its imaginary part its side - by the
/// half-sum of |z| and |Re z|, not as polar(sqrt |z|, arg z / 2): a hypot,
/// an atan2 and a sine and cosine for every slab at every point of a
/// table's path. Within a few of the last digits of it.
Complex root(Complex z)
{
    const double x = z.real(), y = z.imag();
    if (x == 0 && y == 0) return Complex(0, y);
    const double t = std::sqrt((std::abs(x) + std::hypot(x, y)) / 2);
    if (x >= 0) return Complex(t, y / (2 * t));
    return Complex(std::abs(y) / (2 * t), std::copysign(t, y));
}

/// kz = sqrt(k^2 - kr^2), the branch with Im kz <= 0: the wave that goes
/// out from the source and decays, which is the proper sheet everywhere
/// the integral goes (the cuts lie below the real axis).
Complex kzOf(Complex k2, Complex kr)
{
    Complex s = root(k2 - kr * kr);
    if (s.imag() > 0) s = -s;
    return s;
}

/// exp(-j kz d).
Complex travel(Complex kz, double d)
{
    return std::exp(-kJ * kz * d);
}

/*!
 * \a a / \a b by Smith's algorithm, inline: the larger of b's parts divides
 * the smaller, so that nothing overflows that the quotient itself would
 * not. The library's complex division (__divdc3) scales by exponents and
 * checks for infinities and NaNs as a call of its own: half of a table's
 * time went to it in the spectral functions below, which divide on every
 * slab at every point of the path. Within a few of the last digits of it;
 * nought over nought the library's as before.
 */
inline Complex over(Complex a, Complex b)
{
    const double c = b.real(), d = b.imag();
    if (std::abs(c) >= std::abs(d)) {
        if (c == 0) return a / b;
        const double r = d / c, den = c + d * r;
        return Complex((a.real() + a.imag() * r) / den, (a.imag() - a.real() * r) / den);
    }
    const double r = c / d, den = c * r + d;
    return Complex((a.real() * r + a.imag()) / den, (a.imag() * r - a.real()) / den);
}
inline Complex over(double a, Complex b)
{
    return over(Complex(a, 0), b);
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
    const Complex rising = over(Z[m] * (1.0 + gd), 2.0 * d);
    const Complex falling = over(Z[m] * (1.0 + gu), 2.0 * d);
    if (f == m) {
        if (zf >= zs) return rising * travel(kz[m], zf - zs) * (1.0 + up[m] * travel(kz[m], 2 * (s.z1 - zf)));
        return falling * travel(kz[m], zs - zf) * (1.0 + down[m] * travel(kz[m], 2 * (zf - s.z0)));
    }
    if (f > m) {
        Complex v = rising * travel(kz[m], s.z1 - zs) * (1.0 + up[m]);
        for (int n = m + 1; n <= f; ++n) {
            const Layers::Slab& t = layers.slabs.at(n);
            const double depth = t.z1 - t.z0;
            const Complex wave = over(v, 1.0 + up[n] * travel(kz[n], 2 * depth));
            if (n == f) return wave * travel(kz[n], zf - t.z0) * (1.0 + up[n] * travel(kz[n], 2 * (t.z1 - zf)));
            v = wave * travel(kz[n], depth) * (1.0 + up[n]);
        }
    }
    Complex v = falling * travel(kz[m], zs - s.z0) * (1.0 + down[m]);
    for (int n = m - 1; n >= f; --n) {
        const Layers::Slab& t = layers.slabs.at(n);
        const double depth = t.z1 - t.z0;
        const Complex wave = over(v, 1.0 + down[n] * travel(kz[n], 2 * depth));
        if (n == f) return wave * travel(kz[n], t.z1 - zf) * (1.0 + down[n] * travel(kz[n], 2 * (zf - t.z0)));
        v = wave * travel(kz[n], depth) * (1.0 + down[n]);
    }
    return v;
}

/// The stack's two equivalent lines at one kr: each slab's kz and its TM
/// and TE impedances, and the reflections looking up from the top of each
/// slab and down from its bottom.
struct Lines {
    // (kzE the TM line's, kzH the TE's: the same but in a uniaxial slab.)
    QVarLengthArray<Complex, 8> kzE, kzH, ze, zh, upE, upH, downE, downH;
};

/// The lines of \a layers at \a kr, \a eps each slab's permittivity and
/// \a mu its permeability at the angular frequency \a w: above, a short where there is a metal lid and the
/// half-space matched where there is not; below, a short where there is a ground and matched where there is not.
Lines linesOf(const Layers& layers, const QList<Complex>& eps, const QList<Complex>& epsZ, const QList<Complex>& mu,
              Complex epsAbove, Complex epsBelow, double w, Complex kr)
{
    const double muAbove = kMu0 * layers.murAbove, muBelow = kMu0 * layers.murBelow;
    const int count = int(layers.slabs.size());
    Lines l;
    for (QVarLengthArray<Complex, 8>* a : {&l.kzE, &l.kzH, &l.ze, &l.zh, &l.upE, &l.upH, &l.downE, &l.downH}) a->resize(count);
    // (Each slab's own mu in its kz and in the TE line's impedance, w mu/kz:
    //  a magnetic slab's. The half-spaces are mu0's. A uniaxial slab's TE
    //  line sees its eps in the plane alone; its TM line's kz is
    //  sqrt(eps_t/eps_z) sqrt(w^2 mu eps_z - kr^2), its impedance kz/(w eps_t).)
    for (int n = 0; n < count; ++n) {
        l.kzH[n] = kzOf(w * w * mu.at(n) * eps.at(n), kr);
        l.kzE[n] = epsZ.at(n) == eps.at(n) ? l.kzH[n] : std::sqrt(eps.at(n) / epsZ.at(n)) * kzOf(w * w * mu.at(n) * epsZ.at(n), kr);
        l.ze[n] = over(l.kzE[n], w * eps.at(n));
        l.zh[n] = over(w * mu.at(n), l.kzH[n]);
    }
    if (layers.groundAbove) {
        // (A metal lid: a short at the top, as the ground at the bottom.)
        l.upE[count - 1] = l.upH[count - 1] = Complex(-1, 0);
    } else {
        const Complex kzAbove = kzOf(w * w * muAbove * epsAbove, kr);
        const Complex zeAbove = over(kzAbove, w * epsAbove), zhAbove = over(w * muAbove, kzAbove);
        l.upE[count - 1] = over(zeAbove - l.ze[count - 1], zeAbove + l.ze[count - 1]);
        l.upH[count - 1] = over(zhAbove - l.zh[count - 1], zhAbove + l.zh[count - 1]);
    }
    for (int n = count - 2; n >= 0; --n) {
        const double depth = layers.slabs.at(n + 1).z1 - layers.slabs.at(n + 1).z0;
        const Complex rE = over(l.ze[n + 1] - l.ze[n], l.ze[n + 1] + l.ze[n]), gE = l.upE[n + 1] * travel(l.kzE[n + 1], 2 * depth);
        const Complex rH = over(l.zh[n + 1] - l.zh[n], l.zh[n + 1] + l.zh[n]), gH = l.upH[n + 1] * travel(l.kzH[n + 1], 2 * depth);
        l.upE[n] = over(rE + gE, 1.0 + rE * gE);
        l.upH[n] = over(rH + gH, 1.0 + rH * gH);
    }
    if (layers.groundBelow) {
        l.downE[0] = l.downH[0] = Complex(-1, 0);
    } else {
        const Complex kzBelow = kzOf(w * w * muBelow * epsBelow, kr);
        const Complex zeBelow = over(kzBelow, w * epsBelow), zhBelow = over(w * muBelow, kzBelow);
        l.downE[0] = over(zeBelow - l.ze[0], zeBelow + l.ze[0]);
        l.downH[0] = over(zhBelow - l.zh[0], zhBelow + l.zh[0]);
    }
    for (int n = 1; n < count; ++n) {
        const double depth = layers.slabs.at(n - 1).z1 - layers.slabs.at(n - 1).z0;
        const Complex rE = over(l.ze[n - 1] - l.ze[n], l.ze[n - 1] + l.ze[n]), gE = l.downE[n - 1] * travel(l.kzE[n - 1], 2 * depth);
        const Complex rH = over(l.zh[n - 1] - l.zh[n], l.zh[n - 1] + l.zh[n]), gH = l.downH[n - 1] * travel(l.kzH[n - 1], 2 * depth);
        l.downE[n] = over(rE + gE, 1.0 + rE * gE);
        l.downH[n] = over(rH + gH, 1.0 + rH * gH);
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
        // (Its stop by the squares, std::norm, not two hypots a term: the
        //  terms where the two tests differ are under half the sum's last
        //  digit, and add nothing - the sum is the same to the last digit.)
        const Complex q = -z * z / 4.0;
        Complex term(1, 0), sum(1, 0);
        for (int k = 1; k < 80; ++k) {
            term *= q / double(k * k);
            sum += term;
            if (std::norm(term) < 1e-34 * std::norm(sum)) break;
        }
        return sum;
    }
    // Hankel's asymptotic form, J0 = (H0(1) + H0(2))/2 with
    // H0(1,2) ~ sqrt(2/(pi z)) exp(+-j(z - pi/4)) sum (+-j)^k a_k / z^k,
    // a_k = prod (-(2i - 1)^2) / (k! 8^k), summed while its terms fall:
    // at 12 the smallest is about exp(-24).
    // (1/z once, and each term's size by its square: hankelAsymptotic().)
    const Complex inverse = over(1.0, z);
    Complex plus(1, 0), minus(1, 0), a(1, 0), jk(1, 0), mk(1, 0);
    double last = 1;
    for (int k = 1; k < 60; ++k) {
        a *= -double((2 * k - 1) * (2 * k - 1)) / (8.0 * k) * inverse;
        jk *= kJ;
        mk *= -kJ;
        const double size = std::norm(a);
        if (size > last) break;
        last = size;
        plus += jk * a;
        minus += mk * a;
        if (size < 1e-34) break;
    }
    const Complex phase = z - M_PI / 4;
    return root(over(2.0, M_PI * z)) * 0.5 * (std::exp(kJ * phase) * plus + std::exp(-kJ * phase) * minus);
}

namespace {

/// Hankel's asymptotic series for H0 of the first kind (\a kind 1) or the
/// second (2): sqrt(2/(pi z)) exp(+-j(z - pi/4)) sum (+-j)^k a_k / z^k,
/// summed while its terms fall - at |z| = 12 the smallest is exp(-24).
Complex hankelAsymptotic(Complex z, int kind)
{
    // (1/z made once, where each term divided by it - a library call -,
    //  and each term's size by its square, where a hypot each.)
    const Complex turn = kind == 1 ? kJ : -kJ;
    const Complex inverse = over(1.0, z);
    Complex sum(1, 0), a(1, 0), power(1, 0);
    double last = 1;
    for (int k = 1; k < 60; ++k) {
        a *= -double((2 * k - 1) * (2 * k - 1)) / (8.0 * k) * inverse;
        power *= turn;
        const double size = std::norm(a);
        if (size > last) break;
        last = size;
        sum += power * a;
        if (size < 1e-34) break;
    }
    return root(over(2.0, M_PI * z)) * std::exp(turn * (z - M_PI / 4)) * sum;
}

/// Bessel's Y0 for |z| below 12, by its series, \a j0 its J0(z):
/// (2/pi)(ln(z/2) + gamma) J0(z) + (2/pi) sum (-1)^(k+1) H_k (z^2/4)^k / (k!)^2.
Complex besselY0Series(Complex z, Complex j0)
{
    const Complex q = z * z / 4.0;
    Complex term(1, 0), sum(0, 0);
    double harmonic = 0;
    for (int k = 1; k < 80; ++k) {
        term *= q / double(k * k);
        harmonic += 1.0 / k;
        const Complex add = (k % 2 == 1 ? 1.0 : -1.0) * harmonic * term;
        sum += add;
        if (std::norm(add) < 1e-34 * std::norm(sum)) break;   // (as besselJ0()'s)
    }
    constexpr double gamma = 0.57721566490153286;
    return 2.0 / M_PI * ((std::log(z / 2.0) + gamma) * j0 + sum);
}

} // namespace

Complex hankelH0(Complex z, int kind)
{
    // (In the right half-plane, which is where the integral's tail asks.)
    if (std::abs(z) >= 12.0) return hankelAsymptotic(z, kind);
    // (J0 once, for Y0's series and for itself: it was made twice.)
    const Complex j0 = besselJ0(z);
    const Complex y = besselY0Series(z, j0);
    return kind == 1 ? j0 + kJ * y : j0 - kJ * y;
}

/*!
 * The spectral functions at the points of the path that a table's
 * distances take alike (sommerfeld()), each made once, by whichever
 * distance first asks, and read by the rest: the half-ellipse at either of
 * its two heights that every distance but those of one decade takes - half
 * the real axis's a, beyond some 2/a, or a twentieth, within some 20/a -,
 * and each panel along the real axis from a, which every distance takes
 * the same as far as it goes. The same points, made by the same
 * expressions, so that the integrals are what they were to the last digit:
 * only the work is shared. Of a coplanar line's tables' functions some 40 %
 * were these, made again at every distance.
 */
namespace {
std::atomic<long long> sharedPoints{0};   ///< (greensKept().sharedPoints)
std::atomic<bool> pathsShared{true};      ///< (sharePaths())
} // namespace

struct SharedPath {
    static constexpr int kPanels = 200;
    int count = 0;
    double a = 0;
    std::function<void(Complex, Complex*)> values;
    std::once_flag arcMade[2];
    std::vector<Complex> arc[2];               ///< 256 points, count values each: b = a/2, then a/20
    std::once_flag panelMade[kPanels];
    std::vector<Complex> panel[kPanels];        ///< 16 points from a 2^k to a 2^(k+1)
    SharedPath(int functions, double kMax, std::function<void(Complex, Complex*)> f)
        : count(functions), a(std::max(1.5 * kMax, 1e-9)), values(std::move(f))
    {
    }
};

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
                const Values& values, Complex* out, SharedPath* shared = nullptr)
{
    if (!pathsShared.load(std::memory_order_relaxed)) shared = nullptr;
    const Gauss& g = gauss16();
    QVarLengthArray<Complex, 16> sum(count), r(count), part(count), tail(count), fu(count), fd(count);
    std::fill(sum.begin(), sum.end(), Complex(0, 0));
    const double a = std::max(1.5 * kMax, 1e-9);
    const double b = ellipse > 0 ? ellipse : std::min(0.5 * a, std::max(0.05 * a, rho > 0 ? 1.0 / rho : 0.5 * a));
    const int arcs = 16;
    // (The arc's points at either of the heights a table's distances share,
    //  made once for all of them: SharedPath.)
    const int which = shared == nullptr || ellipse > 0 || shared->a != a ? -1
                      : b == 0.5 * a                                     ? 0
                      : b == 0.05 * a                                    ? 1
                                                                         : -1;
    if (which >= 0)
        std::call_once(shared->arcMade[which], [shared, which, a, b, count, &g] {
            std::vector<Complex>& made = shared->arc[which];
            made.assign(std::size_t(256 * count), Complex(0, 0));
            for (int p = 0; p < arcs; ++p)
                for (int i = 0; i < 16; ++i) {
                    const double t = M_PI * (p + (g.x[i] + 1) / 2) / arcs;
                    shared->values(Complex(a / 2 * (1 - std::cos(t)), b * std::sin(t)), made.data() + std::size_t((16 * p + i) * count));
                }
        });
    for (int p = 0; p < arcs; ++p)
        for (int i = 0; i < 16; ++i) {
            const double t = M_PI * (p + (g.x[i] + 1) / 2) / arcs;
            const double weight = g.w[i] * M_PI / (2 * arcs);
            const Complex kr(a / 2 * (1 - std::cos(t)), b * std::sin(t));
            const Complex dk(a / 2 * std::sin(t), b * std::cos(t));
            const Complex* f = r.data();
            if (which >= 0)
                f = shared->arc[which].data() + std::size_t((16 * p + i) * count);
            else
                values(kr, r.data());
            const Complex common = besselJ0(kr * rho) * kr * dk * weight;
            for (int c = 0; c < count; ++c) sum[c] += f[c] * common;
        }
    if (which >= 0) sharedPoints += 256;

    // (Panel k from a 2^k, the same for every distance as far as it goes:
    //  made once where shared.)
    const bool panelsShared = shared != nullptr && shared->a == a;
    const auto panel = [&part, &r, &g, &values, rho, count, panelsShared, shared](double lo, double hi, int k) {
        std::fill(part.begin(), part.end(), Complex(0, 0));
        const bool kept = panelsShared && k < SharedPath::kPanels;
        if (kept)
            std::call_once(shared->panelMade[k], [shared, lo, hi, k, count, &g] {
                std::vector<Complex>& made = shared->panel[k];
                made.assign(std::size_t(16 * count), Complex(0, 0));
                for (int i = 0; i < 16; ++i)
                    shared->values(Complex(lo + (hi - lo) * (g.x[i] + 1) / 2, 0), made.data() + std::size_t(i * count));
            });
        for (int i = 0; i < 16; ++i) {
            const double kr = lo + (hi - lo) * (g.x[i] + 1) / 2;
            const double common = besselJ0(kr * rho) * kr * g.w[i] * (hi - lo) / 2;
            const Complex* f = r.data();
            if (kept)
                f = shared->panel[k].data() + std::size_t(i * count);
            else
                values(Complex(kr, 0), r.data());
            for (int c = 0; c < count; ++c) part[c] += f[c] * common;
        }
        if (kept) sharedPoints += 16;
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
        panel(lo, hi, k);
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
            // (H0(2) of the conjugate is H0(1)'s conjugate, and down is up's:
            //  one Hankel function where there were two, some a third of a
            //  table's time.)
            const Complex hu = hankelH0(up * rho, 1) * up, hd = std::conj(hu);
            for (int c = 0; c < count; ++c) part[c] += (fu[c] * hu - fd[c] * hd) * weight;
        }
        for (int c = 0; c < count; ++c) tail[c] += part[c];
        if (t0 * rho > 5 && nothing(part, tail, 1e-16)) break;
    }
    for (int c = 0; c < count; ++c) out[c] = (sum[c] + kJ / 2.0 * tail[c]) / (2 * M_PI);
}

// ------------------------------------------------------ The tables kept

/// The bytes of what a kernel's table is made of: its key among those kept.
struct KeyWriter {
    QByteArray bytes;
    template <typename T> void add(T v) { bytes.append(reinterpret_cast<const char*>(&v), sizeof v); }
    KeyWriter(char kind, const Layers& layers, double frequency, double low, double most)
    {
        add(kind);
        add(int(layers.slabs.size()));
        for (const Layers::Slab& s : layers.slabs) {
            for (const double v : {s.z0, s.z1, s.epsr, s.tand, s.sigma}) add(v);
            add(s.dispersive);
            if (s.dispersive) add(s.dispersionAt);
            if (s.mur != 1 || s.mtand != 0) {
                add(s.mur);
                add(s.mtand);
            }
            if (s.uniaxial()) add(s.epsz);
        }
        if (layers.murAbove != 1 || layers.murBelow != 1) {
            add(layers.murAbove);
            add(layers.murBelow);
        }
        add(layers.groundBelow);
        add(layers.groundAbove);
        for (const double v : {layers.epsrBelow, layers.epsrAbove, frequency, low, most}) add(v);
    }
};

/// A kept table: its distances, its values (so many a distance), when it
/// was last used, and its own lock while it grows.
struct Kept {
    QMutex lock;
    QList<double> rho;
    QList<Complex> values;
    qint64 used = 0;
    double bytes = 0;   ///< under the store's lock
};

struct Store {
    QMutex lock;
    QHash<QByteArray, std::shared_ptr<Kept>> kept;
    qint64 clock = 0;
    double budget = 256e6, bytes = 0;
    long long built = 0, reused = 0;
};

Store& store()
{
    static Store s;
    return s;
}

/// Under the store's lock: the least lately used forgotten until what is
/// kept is within the budget - all of them where it is nought - \a keep
/// last of all.
void trim(Store& s, const Kept* keep)
{
    while (s.bytes > s.budget || (s.budget <= 0 && !s.kept.isEmpty())) {
        auto oldest = s.kept.end();
        for (auto it = s.kept.begin(); it != s.kept.end(); ++it)
            if (it.value().get() != keep && (oldest == s.kept.end() || it.value()->used < oldest.value()->used)) oldest = it;
        if (oldest == s.kept.end()) break;
        s.bytes -= oldest.value()->bytes;
        s.kept.erase(oldest);
    }
    if (s.kept.isEmpty()) s.bytes = 0;
}

/*!
 * \a node(r, out) at each of \a at's distances from the \a from-th on, its
 * \a width values into \a out - the \a from-th's first - shared among as many
 * as \a threads threads as they come free: each distance made by one of them
 * alone, into its own place, so that what is made is the same on any number.
 * The furthest first: their integrals go on to the vertical lines, and take
 * some three times as long as the nearest, which are left to even out the
 * threads' ends (a via's 197 distances at 10 GHz on 24 threads: 25.5 ms,
 * 26.7 nearest first). Returns how many were made on threads other than the
 * caller's.
 */
int nodesOf(const QList<double>& at, int from, int width, int threads, const std::function<void(double, Complex*)>& node,
            Complex* out)
{
    const int count = int(at.size()) - from;
    if (count <= 0) return 0;
    std::atomic_int next{0}, elsewhere{0};
    onThreads(std::clamp(threads, 1, count), [&at, &node, &next, &elsewhere, from, width, out, count](int t) {
        for (int k = next++; k < count; k = next++) {
            const int i = count - 1 - k;
            node(at.at(from + i), out + qsizetype(i) * width);
            if (t > 0) ++elsewhere;
        }
    });
    return elsewhere.load();
}

/*!
 * The table of the kernel \a key to the first distance at or past \a far,
 * eight distances at least: the distances into \a rho, the values - \a width
 * a distance - into \a values. Those kept are handed over and the rest made
 * by \a node(r, out) and kept in turn, the kernel's own lock held while they
 * are, so that a second solve of it waits for the first rather than does it
 * again. The distances are nought, \a low, then each the last times
 * 10^(1/40) or plus \a most, whichever is the less: the same sequence
 * however far it goes, so that what is handed over is the table this
 * kernel would have made itself, to the last digit, and no more of it.
 * Those it makes are shared among as many as \a threads threads (nodesOf()),
 * and how many were made on threads other than the caller's returned.
 */
int tableOf(const QByteArray& key, int width, double low, double most, double far, int threads,
            const std::function<void(double, Complex*)>& node, QList<double>* rho, QList<Complex>* values)
{
    const double ratio = std::pow(10.0, 1.0 / 40);
    QList<double> wanted{0.0};
    for (double r = low;; r = std::min(r * ratio, r + most)) {
        wanted << r;
        if (r >= far && wanted.size() >= 8) break;
    }
    const int n = int(wanted.size());
    Store& s = store();
    std::shared_ptr<Kept> kept;
    {
        QMutexLocker held(&s.lock);
        if (s.budget > 0) {
            kept = s.kept.value(key);
            if (!kept) {
                kept = std::make_shared<Kept>();
                s.kept.insert(key, kept);
            }
            kept->used = ++s.clock;
        }
    }
    if (!kept) {
        // Keeping nothing: made here, as wanted.
        QList<Complex> made(n * width);
        const int elsewhere = nodesOf(wanted, 0, width, threads, node, made.data());
        *rho = wanted;
        *values = made;
        QMutexLocker held(&s.lock);
        s.built += n;
        return elsewhere;
    }
    QMutexLocker growing(&kept->lock);
    const int had = int(kept->rho.size());
    int elsewhere = 0;
    if (had < n) {
        // (Its places made first, for the threads to fill side by side, and
        //  its distances added once each has its value.)
        kept->values.resize(qsizetype(n) * width);
        elsewhere = nodesOf(wanted, had, width, threads, node, kept->values.data() + qsizetype(had) * width);
        for (int i = had; i < n; ++i) kept->rho << wanted.at(i);
    }
    // (Copies of its own, never the kept lists shared: a kernel's table is
    // read from many threads, and a list shared with the store is one a
    // reader's non-const call would detach under the others.)
    *rho = QList<double>(kept->rho.cbegin(), kept->rho.cbegin() + n);
    *values = QList<Complex>(kept->values.cbegin(), kept->values.cbegin() + qsizetype(n) * width);
    const double now = 8.0 * kept->rho.size() + 16.0 * kept->values.size();
    QMutexLocker held(&s.lock);
    s.built += std::max(0, n - had);
    s.reused += std::min(had, n);
    // (Unless forgotten meanwhile, its bytes counted and the rest trimmed.)
    if (s.kept.value(key) == kept) {
        s.bytes += now - kept->bytes;
        kept->bytes = now;
        trim(s, kept.get());
    }
    return elsewhere;
}

} // namespace

GreensKept greensKept()
{
    Store& s = store();
    QMutexLocker held(&s.lock);
    GreensKept out;
    out.tables = int(s.kept.size());
    out.bytes = s.bytes;
    out.built = s.built;
    out.reused = s.reused;
    out.sharedPoints = sharedPoints.load();
    return out;
}

void forgetGreens()
{
    Store& s = store();
    QMutexLocker held(&s.lock);
    s.kept.clear();
    s.bytes = 0;
    s.built = s.reused = 0;
    sharedPoints = 0;
}

void sharePaths(bool shared)
{
    pathsShared = shared;
}

void keepGreens(double bytes)
{
    Store& s = store();
    QMutexLocker held(&s.lock);
    s.budget = std::max(0.0, bytes);
    trim(s, nullptr);
}

// -------------------------------------------------------------- The kernels

namespace {
/// A slab's permeability, H/m: mu0 mur (1 - j mtand).
Complex permeabilityOf(const Layers::Slab& s)
{
    return kMu0 * Complex(s.mur, -s.mur * s.mtand);
}
} // namespace

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
    if (layers.groundAbove && (zSource >= layers.height() || zField >= layers.height())) {
        a_problems << tr("A metal on the lid itself is shorted to it, and has no potential of its own.");
        return;
    }

    // Each slab's permittivity at the frequency: its loss tangent and its
    // conductivity in its imaginary part, eps0 (eps_r (1 - j tand)) - j sigma/w.
    for (const Layers::Slab& s : layers.slabs) {
        a_eps << kEps0 * s.permittivityAt(a_omega / (2 * M_PI)) - Complex(0, s.sigma / a_omega);
        a_epsZ << (s.uniaxial() ? kEps0 * s.permittivityZAt(a_omega / (2 * M_PI)) - Complex(0, s.sigma / a_omega) : a_eps.last());
        a_mu << permeabilityOf(s);
    }
    a_epsAbove = kEps0 * layers.epsrAbove;
    a_epsBelow = kEps0 * layers.epsrBelow;
    const Complex muAbove(kMu0 * layers.murAbove, 0), muBelow(kMu0 * layers.murBelow, 0);
    const auto aboveK2 = [this, muAbove] { return a_omega * a_omega * muAbove * a_epsAbove; };
    const auto belowK2 = [this, muBelow] { return a_omega * a_omega * muBelow * a_epsBelow; };
    const auto slabK2 = [this](int n) { return a_omega * a_omega * a_mu.at(n) * a_eps.at(n); };
    for (int n = 0; n < a_eps.size(); ++n)
        a_kMax = std::max({a_kMax, std::abs(std::sqrt(slabK2(n))), std::abs(std::sqrt(a_omega * a_omega * a_mu.at(n) * a_epsZ.at(n)))});
    a_kMax = std::max({a_kMax, std::abs(std::sqrt(aboveK2())), std::abs(std::sqrt(belowK2()))});

    const int count = int(layers.slabs.size());
    const int m = a_src, f = a_fld;
    const double tol = 1e-12 * std::max(1e-9, layers.height());
    // (A uniaxial slab's TM line goes as 1/sqrt(eps_t eps_z) when kr is
    //  large, and its kz as sqrt(eps_t/eps_z) kr: to the scalar's closed form
    //  it is a slab of sqrt(eps_t eps_z) that much deeper - the same as the
    //  static transformation. An isotropic slab's is its eps, its depth.)
    const auto epsEq = [&](int n) { return a_epsZ.at(n) == a_eps.at(n) ? a_eps.at(n) : std::sqrt(a_eps.at(n) * a_epsZ.at(n)); };
    const auto stretch = [&](int n) { return a_epsZ.at(n) == a_eps.at(n) ? 1.0 : std::sqrt(a_eps.at(n) / a_epsZ.at(n)).real(); };
    const auto epsHigh = [&](int n) { return n + 1 < count ? epsEq(n + 1) : a_epsAbove; };
    const auto epsLow = [&](int n) { return n > 0 ? epsEq(n - 1) : a_epsBelow; };
    const auto onTop = [&](int n, double z) { return std::abs(z - layers.slabs.at(n).z1) <= tol; };
    const auto onBottom = [&](int n, double z) { return std::abs(z - layers.slabs.at(n).z0) <= tol; };
    // The static Fresnel coefficients of the TM line, whose impedances go
    // as 1/eps when kr is large: looking up out of slab n, and down.
    const auto fresnelUp = [&](int n) -> Complex {
        if (n + 1 == count && layers.groundAbove) return Complex(-1, 0);   // (a metal lid)
        return (epsEq(n) - epsHigh(n)) / (epsEq(n) + epsHigh(n));
    };
    const auto fresnelDown = [&](int n) -> Complex {
        if (n == 0 && layers.groundBelow) return Complex(-1, 0);
        return (epsEq(n) - epsLow(n)) / (epsEq(n) + epsLow(n));
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
    // (The scalar's depth in the stretched heights: each slab's share of the
    //  way times its stretch.)
    const auto stretched = [&](double from, double to) {
        double sum = 0;
        for (int n = 0; n < count; ++n) {
            const double lo = std::max(from, layers.slabs.at(n).z0), hi = std::min(to, layers.slabs.at(n).z1);
            if (hi > lo) sum += stretch(n) * (hi - lo);
        }
        return sum;
    };
    const double dzScalar = dz <= tol ? dz : stretched(std::min(zSource, zField), std::max(zSource, zField));
    a_scalarTerm.coefficient = factor / epsEq(m);
    a_scalarTerm.dz = dzScalar;
    // The vector kernel's: mu exp(-jkR)/(4 pi R). The TE line's impedances
    // go as mu when kr is large, as the TM line's go as 1/eps, so a magnetic
    // slab's faces reflect it as a dielectric's reflect the scalar - by
    // (mu_b - mu_a)/(mu_b + mu_a) - and a current on such a face has
    // mu_a mu_b/(mu_a + mu_b) of it. No dielectric changes it; the ground's
    // image is its own term, below.
    const auto muHigh = [&](int n) { return n + 1 < count ? a_mu.at(n + 1) : muAbove; };
    const auto muLow = [&](int n) { return n > 0 ? a_mu.at(n - 1) : muBelow; };
    const auto fresnelUpH = [&](int n) -> Complex {
        if (n + 1 == count && layers.groundAbove) return Complex(-1, 0);   // (a metal lid: a horizontal current's image the opposite)
        return (muHigh(n) - a_mu.at(n)) / (muHigh(n) + a_mu.at(n));
    };
    const auto fresnelDownH = [&](int n) -> Complex {
        if (n == 0 && layers.groundBelow) return Complex(0, 0);   // (the ground's image, below)
        return (muLow(n) - a_mu.at(n)) / (muLow(n) + a_mu.at(n));
    };
    const auto localUpH = [&](int n, double z) { return onTop(n, z) ? fresnelUpH(n) : Complex(0, 0); };
    const auto localDownH = [&](int n, double z) { return onBottom(n, z) ? fresnelDownH(n) : Complex(0, 0); };
    Complex factorH;
    if (f == m) {
        const double low = std::min(zSource, zField), high = std::max(zSource, zField);
        factorH = (1.0 + localDownH(m, low)) * (1.0 + localUpH(m, high)) / 2.0;
    } else if (f > m) {
        factorH = (1.0 + localDownH(m, zSource)) / 2.0;
        for (int n = m; n < f; ++n) factorH *= 1.0 + fresnelUpH(n);
        factorH *= 1.0 + localUpH(f, zField);
    } else {
        factorH = (1.0 + localUpH(m, zSource)) / 2.0;
        for (int n = m; n > f; --n) factorH *= 1.0 + fresnelDownH(n);
        factorH *= 1.0 + localDownH(f, zField);
    }
    a_vectorTerm.coefficient = a_mu.at(m) * factorH;
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
            if (hi > lo) sum += slabK2(n) * (hi - lo);
        }
        return sum;
    };
    // (The scalar's, of the TM line: w^2 mu eps_z in a uniaxial slab, along
    //  the stretched way; the vector's of the TE line, eps_t's.)
    const auto slabK2z = [this](int n) { return a_omega * a_omega * a_mu.at(n) * a_epsZ.at(n); };
    const auto alongZ = [&](double from, double to) {
        Complex sum(0, 0);
        for (int n = 0; n < count; ++n) {
            const double lo = std::max(from, layers.slabs.at(n).z0), hi = std::min(to, layers.slabs.at(n).z1);
            if (hi > lo) sum += slabK2z(n) * stretch(n) * (hi - lo);
        }
        return sum;
    };
    Complex scalarK2, vectorK2;
    if (dz <= tol) {
        const Complex ka2 = slabK2(m), kaz2 = slabK2z(m);
        Complex kb2 = ka2, kbz2 = kaz2;
        if (onTop(m, zSource)) {
            kb2 = m + 1 < count ? slabK2(m + 1) : aboveK2();
            kbz2 = m + 1 < count ? slabK2z(m + 1) : aboveK2();
        } else if (onBottom(m, zSource) && !layers.groundBelow) {
            kb2 = m > 0 ? slabK2(m - 1) : belowK2();
            kbz2 = m > 0 ? slabK2z(m - 1) : belowK2();
        }
        scalarK2 = 2.0 * kaz2 * kbz2 / (kaz2 + kbz2);
        vectorK2 = (ka2 + kb2) / 2.0;
    } else {
        scalarK2 = alongZ(std::min(zSource, zField), std::max(zSource, zField)) / dzScalar;
        vectorK2 = along(std::min(zSource, zField), std::max(zSource, zField)) / dz;
    }
    a_scalarTerm.k = std::sqrt(scalarK2);
    a_vectorTerm.k = std::sqrt(vectorK2);
    // The vector potential's image in a ground: a horizontal current's is
    // the opposite current, and no dielectric changes that as kr grows - nor
    // a permeability, where it is the same from the ground up to both
    // heights; where it is not, the image is left in the remainder, the
    // ground then a face it dies over.
    bool evenBelow = true;
    for (int n = 0; n <= std::max(m, f) && n < count; ++n) evenBelow = evenBelow && a_mu.at(n) == a_mu.at(m);
    if (layers.groundBelow && evenBelow) {
        a_vectorImage.coefficient = -a_mu.at(m) / 2.0;
        a_vectorImage.dz = zSource + zField;
        a_vectorImage.k = std::sqrt((along(0, zSource) + along(0, zField)) / (zSource + zField));
    }
    // And in a metal lid, the same where mu is the same from both heights up
    // to it. (Left in the remainder, a lid near a line was read at the runs'
    // middles alone: 20 µm over a strip of runs 100 µm long, its L 9 % low.)
    bool evenAbove = true;
    for (int n = std::min(m, f); n < count; ++n) evenAbove = evenAbove && a_mu.at(n) == a_mu.at(m);
    if (layers.groundAbove && evenAbove) {
        const double top = layers.height();
        a_vectorLid.coefficient = -a_mu.at(m) / 2.0;
        a_vectorLid.dz = 2 * top - zSource - zField;
        a_vectorLid.k = std::sqrt((along(zSource, top) + along(zField, top)) / a_vectorLid.dz);
    }

    // How fast what is left dies in kr: as exp(-kr (dz + 2 d)), d the
    // nearest interface that reflects and is at neither height - and as
    // 1/kr^5 for what the closed forms did not match.
    QList<double> faces;
    if (layers.groundBelow || std::abs(a_epsBelow - a_eps.first()) > 1e-9 * std::abs(a_eps.first()))
        faces << layers.slabs.first().z0;
    // (A magnetic slab's faces reflect too.)
    const auto muDiffers = [](Complex a, Complex b) { return std::abs(a - b) > 1e-9 * std::abs(a); };
    if (!layers.groundBelow && muDiffers(a_mu.first(), muBelow)) faces << layers.slabs.first().z0;
    for (int n = 0; n + 1 < count; ++n)
        if (std::abs(a_eps.at(n) - a_eps.at(n + 1)) > 1e-9 * std::abs(a_eps.at(n)) || muDiffers(a_mu.at(n), a_mu.at(n + 1))
            || muDiffers(a_epsZ.at(n), a_epsZ.at(n + 1)))
            faces << layers.slabs.at(n).z1;
    if (layers.groundAbove || std::abs(a_epsAbove - a_eps.last()) > 1e-9 * std::abs(a_eps.last()) || muDiffers(a_mu.last(), muAbove))
        faces << layers.slabs.last().z1;
    double nearest = std::numeric_limits<double>::infinity();
    for (const double face : std::as_const(faces))
        for (const double z : {zSource, zField}) {
            const double d = std::abs(face - z);
            if (d > tol) nearest = std::min(nearest, d);
        }
    // (A uniaxial slab's TM reflections die over its stretched depth: the
    //  least stretch of the stack, where it is under 1, shortens the reach.)
    double least = 1;
    for (int n = 0; n < count; ++n) least = std::min(least, stretch(n));
    a_reach = std::isfinite(nearest) ? least * (dz + 2 * nearest) : 0;
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
    const Lines l = linesOf(a_layers, a_eps, a_epsZ, a_mu, a_epsAbove, a_epsBelow, a_omega, kr);
    const Complex ve =
        lineVoltage(a_layers, a_src, a_zSource, a_fld, a_zField, l.ze.data(), l.kzE.data(), l.upE.data(), l.downE.data());
    const Complex vh =
        lineVoltage(a_layers, a_src, a_zSource, a_fld, a_zField, l.zh.data(), l.kzH.data(), l.upH.data(), l.downH.data());
    return {over(kJ * a_omega, kr * kr) * (ve - vh), over(vh, kJ * a_omega)};
}

WaveGreen::Pair WaveGreen::extracted(Complex kr) const
{
    // c exp(-u dz)/u, u = j kz = sqrt(kr^2 - k^2): Sommerfeld's identity
    // makes it c exp(-jkR)/(2 pi R).
    const auto term = [&kr](const Term& t) {
        if (t.coefficient == Complex(0, 0)) return Complex(0, 0);
        const Complex u = kJ * kzOf(t.k * t.k, kr);
        return over(t.coefficient * std::exp(-u * t.dz), u);
    };
    return {term(a_scalarTerm), term(a_vectorTerm) + term(a_vectorImage) + term(a_vectorLid)};
}

Complex WaveGreen::scalarSpectral(Complex kr) const
{
    return ok() ? spectral(kr).scalar : Complex(0, 0);
}

Complex WaveGreen::vectorSpectral(Complex kr) const
{
    return ok() ? spectral(kr).vector : Complex(0, 0);
}

WaveGreen::Pair WaveGreen::integral(double rho, double ellipse, SharedPath* shared) const
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
        out, shared);
    return {out[0], out[1]};
}

int WaveGreen::prepare(int threads) const
{
    if (!ok() || !a_rho.isEmpty()) return 0;
    // From nought, then log-spaced from well inside the nearest image's
    // distance and a thousandth of the wavelength over 2 pi - forty a
    // decade - until the step reaches a sixtieth of the shortest wavelength,
    // and evenly from there: what the remainder changes over, not how far it
    // is wanted, so that every extent is one sequence and its table kept
    // between solves (tableOf()). (It began at a thousandth of the farthest
    // distance too, which gave each layout's tables a grid of their own and
    // nothing to share - finer there than the images ask: the remainder,
    // read straight in rho^2 below the first distance, is within (rho/d)^4
    // of itself, a part in 1e8 at a hundredth of the image's d.)
    double low = 1e-3 / a_kMax;
    if (a_reach > 0) low = std::min(low, 0.01 * a_reach);
    const double most = 2 * M_PI / (60 * a_kMax);
    KeyWriter key('W', a_layers, a_frequency, low, most);
    key.add(a_zField);
    key.add(a_zSource);
    QList<Complex> values;
    // (The points of the path its distances share: SharedPath.)
    SharedPath shared(2, a_kMax, [this](Complex kr, Complex* v) {
        const Pair full = spectral(kr), cut = extracted(kr);
        v[0] = full.scalar - cut.scalar;
        v[1] = full.vector - cut.vector;
    });
    const int elsewhere = tableOf(key.bytes, 2, low, most, a_rhoMax, threads,
                                  [this, &shared](double r, Complex* v) {
                                      const Pair p = integral(r, 0, &shared);
                                      v[0] = p.scalar;
                                      v[1] = p.vector;
                                  },
                                  &a_rho, &values);
    a_table.clear();
    a_table.reserve(a_rho.size());
    for (int i = 0; i < a_rho.size(); ++i) a_table << Pair{values.at(2 * i), values.at(2 * i + 1)};
    // What the quintic reads (oneFromTable()), made once here: of the half of a
    // compressed solve's fill and of a dense one's spent reading remainders,
    // most was its weights' 30 divisions at every distance read.
    const int n = int(a_rho.size());
    a_scaled.assign(std::size_t(2 * n), Complex(0, 0));
    for (int i = 0; i < n; ++i) {
        a_scaled[std::size_t(2 * i)] = a_rho.at(i) * a_table.at(i).scalar;
        a_scaled[std::size_t(2 * i + 1)] = a_rho.at(i) * a_table.at(i).vector;
    }
    a_inverse.assign(std::size_t(6 * std::max(0, n - 5)), 0.0);
    for (int j = 0; j + 6 <= n; ++j)
        for (int p = 0; p < 6; ++p) {
            double d = 1;
            for (int q = 0; q < 6; ++q)
                if (q != p) d *= a_rho.at(j + p) - a_rho.at(j + q);
            a_inverse[std::size_t(6 * j + p)] = 1 / d;
        }
    return elsewhere;
}

Complex WaveGreen::oneFromTable(double rho, bool vector) const
{
    prepare();
    // Read through const views: the lists are mutable, and their non-const
    // begin() or last() detaches a list that is shared - from many threads
    // at once where a compressed solve fills its blocks, the heap's undoing.
    const QList<double>& nodes = a_rho;
    const QList<Pair>& table = a_table;
    if (nodes.size() < 8 || rho >= nodes.last()) {
        const Pair p = integral(std::max(rho, 0.0), 0);
        return vector ? p.vector : p.scalar;
    }
    if (rho <= nodes.at(1)) {
        // The remainder is even in rho: straight in rho^2 to the first.
        const double t = (rho / nodes.at(1)) * (rho / nodes.at(1));
        const Complex v0 = vector ? table.at(0).vector : table.at(0).scalar;
        const Complex v1 = vector ? table.at(1).vector : table.at(1).scalar;
        return v0 + t * (v1 - v0);
    }
    // A quintic through the six nodes about it, of rho times the value -
    // which tends to a constant where the value goes as 1/rho. (A cubic
    // left a part in 1e5 where the far field turns over a wavelength.)
    // Lagrange's weights, prod_q (rho - rho_q) / (rho_p - rho_q), their
    // denominators made with the table (prepare()).
    const int i = int(std::upper_bound(nodes.begin(), nodes.end(), rho) - nodes.begin()) - 1;
    const int j = std::clamp(i - 2, 1, int(nodes.size()) - 6);
    const double* x = nodes.constData() + j;
    const double d[6] = {rho - x[0], rho - x[1], rho - x[2], rho - x[3], rho - x[4], rho - x[5]};
    // (Each weight's numerator, the product of the others: from the left
    //  and from the right.)
    double left[6], right[6];
    left[0] = right[5] = 1;
    for (int p = 1; p < 6; ++p) left[p] = left[p - 1] * d[p - 1];
    for (int p = 4; p >= 0; --p) right[p] = right[p + 1] * d[p + 1];
    const double* inverse = a_inverse.data() + std::size_t(6 * j);
    const Complex* scaled = a_scaled.data() + std::size_t(2 * j + (vector ? 1 : 0));
    double re = 0, im = 0;
    for (int p = 0; p < 6; ++p) {
        const double w = left[p] * right[p] * inverse[p];
        re += w * scaled[2 * p].real();
        im += w * scaled[2 * p].imag();
    }
    return Complex(re / rho, im / rho);
}

Complex WaveGreen::scalarRemainder(double rho) const
{
    return ok() ? oneFromTable(rho, false) : Complex(0, 0);
}

Complex WaveGreen::vectorRemainder(double rho) const
{
    return ok() ? oneFromTable(rho, true) : Complex(0, 0);
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
    return closedForm(a_scalarTerm, rho) + oneFromTable(rho, false);
}

Complex WaveGreen::vector(double rho) const
{
    if (!ok()) return Complex(0, 0);
    return closedForm(a_vectorTerm, rho) + closedForm(a_vectorImage, rho) + closedForm(a_vectorLid, rho) + oneFromTable(rho, true);
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
    return over(std::exp(-x) + x - 1.0, u * u);
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
    return over(1.0 - (1.0 + x) * std::exp(-x), kr * kr * kr);
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
    for (const Layers::Slab& s : layers.slabs) {
        a_eps << kEps0 * s.permittivityAt(a_omega / (2 * M_PI)) - Complex(0, s.sigma / a_omega);
        a_epsZ << (s.uniaxial() ? kEps0 * s.permittivityZAt(a_omega / (2 * M_PI)) - Complex(0, s.sigma / a_omega) : a_eps.last());
        a_mu << permeabilityOf(s);
    }
    a_epsAbove = kEps0 * layers.epsrAbove;
    a_epsBelow = kEps0 * layers.epsrBelow;
    for (int n = 0; n < a_eps.size(); ++n)
        a_kMax = std::max({a_kMax, std::abs(std::sqrt(slabK2(n))), std::abs(std::sqrt(a_omega * a_omega * a_mu.at(n) * a_epsZ.at(n)))});
    a_kMax = std::max({a_kMax, std::abs(std::sqrt(a_omega * a_omega * kMu0 * layers.murAbove * a_epsAbove)),
                       std::abs(std::sqrt(a_omega * a_omega * kMu0 * layers.murBelow * a_epsBelow))});

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
    // (A uniaxial slab: to the TM line as kr grows, a slab of
    //  sqrt(eps_t eps_z), its heights stretched by sqrt(eps_t/eps_z) - its
    //  closed forms in the stretched heights, as WaveGreen's scalar's.)
    const auto epsEq = [this](int n) { return a_epsZ.at(n) == a_eps.at(n) ? a_eps.at(n) : std::sqrt(a_eps.at(n) * a_epsZ.at(n)); };
    const auto stretch = [this](int n) { return a_epsZ.at(n) == a_eps.at(n) ? 1.0 : std::sqrt(a_eps.at(n) / a_epsZ.at(n)).real(); };
    const auto mapped = [&layers, &stretch, count](double z) {
        double out = 0;
        for (int n = 0; n < count; ++n) {
            const Layers::Slab& slab = layers.slabs.at(n);
            if (z <= slab.z0) break;
            out += stretch(n) * (std::min(z, slab.z1) - slab.z0);
        }
        return out;
    };
    const auto slabK2z = [this](int n) { return a_omega * a_omega * a_mu.at(n) * a_epsZ.at(n); };
    const auto epsHigh = [&epsEq, this, count](int n) { return n + 1 < count ? epsEq(n + 1) : a_epsAbove; };
    const auto epsLow = [&epsEq, this](int n) { return n > 0 ? epsEq(n - 1) : a_epsBelow; };
    const auto fresnelUp = [&epsEq, &epsHigh, &layers, count](int n) -> Complex {
        if (n + 1 == count && layers.groundAbove) return Complex(-1, 0);   // (a metal lid)
        return (epsEq(n) - epsHigh(n)) / (epsEq(n) + epsHigh(n));
    };
    const auto fresnelDown = [&epsEq, &epsLow, &layers](int n) -> Complex {
        if (n == 0 && layers.groundBelow) return Complex(-1, 0);
        return (epsEq(n) - epsLow(n)) / (epsEq(n) + epsLow(n));
    };

    // G_zz's closed forms: as kr grows, mu0 I_v^e / eps' is the free-space
    // direct term between parts in one slab, and its images in that slab's
    // faces - a vertical current's image in a ground is the same current,
    // in a dielectric face -Gamma of it; between neighbouring slabs the
    // direct term again, carried across with 2 eps_n/(eps_n + eps_m). The
    // images are always listed, a coefficient of nothing where a face does
    // not reflect, so that the list is the same at every frequency.
    // (Of a magnetic stack, Michalski and Mosig's formulation C: G_zz is
    //  mu I_v^e / eps', mu the field's and eps' the source's - n is the
    //  field's part here, m the source's - so each closed form takes the
    //  field slab's mu.)
    for (const QList<Segment>& other : std::as_const(a_others)) {
        QList<Term> terms;
        for (const Segment& n : std::as_const(a_own))
            for (const Segment& m : other) {
                // (A uniaxial slab's G_zz as kr grows: mu exp(-jk'R')/(4 pi R')
                //  over its stretch sqrt(eps_t/eps_z), k' of eps_z and R' in the
                //  stretched heights - the same at large kr as an isotropic
                //  slab's along its real heights.)
                const Complex half = kJ * a_omega * a_mu.at(n.slab) / 2.0;
                const double an0 = mapped(n.z0), an1 = mapped(n.z1), am0 = mapped(m.z0), am1 = mapped(m.z1);
                if (n.slab == m.slab) {
                    const Complex k = std::sqrt(slabK2z(n.slab));
                    const Complex own = half / stretch(n.slab);
                    terms << Term{own, k, an0, an1, am0, am1};
                    terms << Term{-fresnelUp(n.slab) * own, k, an0, an1, 2 * an1 - am1, 2 * an1 - am0};
                    terms << Term{-fresnelDown(n.slab) * own, k, an0, an1, 2 * an0 - am1, 2 * an0 - am0};
                } else if (std::abs(n.slab - m.slab) == 1) {
                    const Complex k = std::sqrt((slabK2z(n.slab) * stretch(n.slab) + slabK2z(m.slab) * stretch(m.slab))
                                                / (stretch(n.slab) + stretch(m.slab)));
                    const Complex carried = 2.0 * epsEq(n.slab) / (epsEq(n.slab) + epsEq(m.slab));
                    terms << Term{carried * half / std::sqrt(stretch(n.slab) * stretch(m.slab)), k, an0, an1, am0, am1};
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
            // (In a uniaxial slab over its stretch twice: its TM line's
            //  current in its kz Ze, and its integral along the stretched part.)
            t.coefficient = -kJ * a_omega * a_mu.at(top.slab) * fresnelUp(top.slab) / (2.0 * stretch(top.slab) * stretch(top.slab));
            t.d = stretch(top.slab) * (top.z1 - top.z0);
        } else if (std::abs(h - a_own.first().z0) <= hair && !(layers.groundBelow && a_own.first().slab == 0
                                                               && a_own.first().z0 <= hair)) {
            const Segment& bottom = a_own.first();
            t.coefficient = kJ * a_omega * a_mu.at(bottom.slab) * fresnelDown(bottom.slab) / (2.0 * stretch(bottom.slab) * stretch(bottom.slab));
            t.d = stretch(bottom.slab) * (bottom.z1 - bottom.z0);
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
    double least = 1;
    for (int n = 0; n < count; ++n) least = std::min(least, stretch(n));
    a_reach = std::isfinite(reach) ? least * reach : 0;
    // A uniaxial part's potential at a height: its tail's strength, built as
    // the isotropic one from the stretched slab, is not quite the kernel's -
    // its TM line runs in the stretched heights and its TE line in the real
    // ones -, and a constant 0.9 % of the kernel was left over on sapphire,
    // never dying. So the strength is read from the kernel itself, far out
    // where every exponential has died, and what is left falls as fast as an
    // isotropic slab's.
    bool uniaxialOwn = false;
    for (const Segment& part : std::as_const(a_own)) uniaxialOwn = uniaxialOwn || a_epsZ.at(part.slab) != a_eps.at(part.slab);
    if (uniaxialOwn && !a_tails.isEmpty()) {
        const double far = 1e4 * std::max({a_kMax, a_reach > 0 ? 1 / a_reach : 0.0, 1 / (a_own.last().z1 - a_own.first().z0)});
        std::vector<Complex> full(static_cast<std::size_t>(a_count));
        spectral(Complex(far, 0), full.data());
        for (int h = 0; h < a_tails.size(); ++h) {
            Tail& t = a_tails[h];
            if (!(t.d > 0)) t.d = least * (a_own.last().z1 - a_own.first().z0);
            const Complex shape = tailSpectral(Complex(far, 0), t.d);
            t.coefficient = std::abs(shape) > 0 ? full[std::size_t(a_others.size() + h)] / shape : Complex(0, 0);
        }
    }
    a_decay = std::max(1.5 * a_kMax, 200 * a_kMax);
    if (a_reach > 0) a_decay = std::max(a_decay, 46 / a_reach);
    a_rhoMax = std::max(rhoMax, 1e-9);
}

Complex ViaGreen::slabK2(int slab) const
{
    return a_omega * a_omega * a_mu.at(slab) * a_eps.at(slab);
}

void ViaGreen::spectral(Complex kr, Complex* full) const
{
    const Lines l = linesOf(a_layers, a_eps, a_epsZ, a_mu, a_epsAbove, a_epsBelow, a_omega, kr);
    const double w = a_omega;
    // V_i between two heights, TM or TE: the voltage at zf of a unit shunt
    // current at zs.
    const auto voltage = [this, &l](bool tm, double zf, double zs) {
        return lineVoltage(a_layers, a_layers.slabAt(zs), zs, a_layers.slabAt(zf), zf, tm ? l.ze.data() : l.zh.data(),
                           tm ? l.kzE.data() : l.kzH.data(), tm ? l.upE.data() : l.upH.data(), tm ? l.downE.data() : l.downH.data());
    };
    // (j kz Zh is j w mu: each slab's own.)
    const auto jkzZh = [this, w](int slab) { return kJ * w * a_mu.at(slab); };
    const auto jkzZe = [&l](int slab) { return kJ * l.kzE[slab] * l.ze[slab]; };
    int c = 0;
    for (const QList<Segment>& other : std::as_const(a_others)) {
        Complex sum(0, 0);
        for (const Segment& n : std::as_const(a_own))
            for (const Segment& m : other) {
                const Complex second = voltage(true, n.z1, m.z1) - voltage(true, n.z1, m.z0) - voltage(true, n.z0, m.z1)
                                       + voltage(true, n.z0, m.z0);
                const double same = n.slab == m.slab ? n.z1 - n.z0 : 0.0;
                const Complex currents = over(same - over(second, jkzZe(m.slab)), jkzZe(n.slab));   // Int Int I_v^e
                sum += over(a_mu.at(n.slab) * currents, a_epsZ.at(m.slab));   // (eps' along z, a uniaxial slab's)
            }
        full[c++] = sum;
    }
    // The potential at a height of the via's current: -(j w mu / kr^2)
    // times the TM and TE lines' currents' difference integrated along it -
    // each part's source slab's mu.
    for (const double h : std::as_const(a_heights)) {
        Complex sum(0, 0);
        for (const Segment& m : std::as_const(a_own))
            sum += a_mu.at(m.slab)
                   * (over(voltage(true, h, m.z1) - voltage(true, h, m.z0), jkzZe(m.slab))
                      - over(voltage(false, h, m.z1) - voltage(false, h, m.z0), jkzZh(m.slab)));
        full[c++] = over(-kJ * w, kr * kr) * sum;
    }
}

Complex ViaGreen::symmetricSpectral(int other, Complex kr) const
{
    if (!ok() || other < 0 || other >= a_others.size()) return Complex(0, 0);
    const Lines l = linesOf(a_layers, a_eps, a_epsZ, a_mu, a_epsAbove, a_epsBelow, a_omega, kr);
    const double w = a_omega;
    const auto voltage = [this, &l](bool tm, double zf, double zs) {
        return lineVoltage(a_layers, a_layers.slabAt(zs), zs, a_layers.slabAt(zf), zf, tm ? l.ze.data() : l.zh.data(),
                           tm ? l.kzE.data() : l.kzH.data(), tm ? l.upE.data() : l.upH.data(), tm ? l.downE.data() : l.downH.data());
    };
    const auto jkzZh = [this, w](int slab) { return kJ * w * a_mu.at(slab); };
    const auto jkzZe = [&l](int slab) { return kJ * l.kzE[slab] * l.ze[slab]; };
    Complex sum(0, 0);
    for (const Segment& n : std::as_const(a_own))
        for (const Segment& m : a_others.at(other)) {
            const auto second = [&](bool tm) {
                return voltage(tm, n.z1, m.z1) - voltage(tm, n.z1, m.z0) - voltage(tm, n.z0, m.z1) + voltage(tm, n.z0, m.z0);
            };
            const double same = n.slab == m.slab ? n.z1 - n.z0 : 0.0;
            const Complex ie = (same - second(true) / jkzZe(m.slab)) / jkzZe(n.slab);
            const Complex ih = (same - second(false) / jkzZh(m.slab)) / jkzZh(n.slab);
            const Complex mn = a_mu.at(n.slab), mm = a_mu.at(m.slab);
            sum += ie * (mn / a_epsZ.at(m.slab) + mm / a_epsZ.at(n.slab)) - w * w * mn * mm / (kr * kr) * (ie - ih);
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
            sum += over(t.coefficient * pairIntegral(u, t.a0, t.a1, t.b0, t.b1), u);
        }
        cut[c++] = sum;
    }
    for (const Tail& t : std::as_const(a_tails))
        cut[c++] = t.coefficient == Complex(0, 0) ? Complex(0, 0) : t.coefficient * tailSpectral(kr, t.d);
}

void ViaGreen::integral(double rho, double ellipse, Complex* out, SharedPath* shared) const
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
        out, shared);
}

int ViaGreen::prepare(int threads) const
{
    if (!ok() || !a_rho.isEmpty() || a_count == 0) return 0;
    // WaveGreen's table: from nought, then log-spaced from well inside the
    // thinnest slab and a thousandth of the wavelength over 2 pi, and evenly
    // from a sixtieth of the shortest - one sequence for every extent, kept
    // between solves - its key the via's parts, the others' and the heights.
    double low = 1e-3 / a_kMax;
    if (a_reach > 0) low = std::min(low, 0.01 * a_reach);
    const double most = 2 * M_PI / (60 * a_kMax);
    KeyWriter key('V', a_layers, a_frequency, low, most);
    const auto parts = [&key](const QList<Segment>& segments) {
        key.add(int(segments.size()));
        for (const Segment& s : segments) {
            key.add(s.slab);
            key.add(s.z0);
            key.add(s.z1);
        }
    };
    parts(a_own);
    key.add(int(a_others.size()));
    for (const QList<Segment>& other : std::as_const(a_others)) parts(other);
    key.add(int(a_heights.size()));
    for (const double h : std::as_const(a_heights)) key.add(h);
    QList<Complex> values;
    QList<double> rho;
    const int count = a_count;
    SharedPath shared(count, a_kMax, [this, count](Complex kr, Complex* v) {
        QVarLengthArray<Complex, 16> full(count), cut(count);
        spectral(kr, full.data());
        extracted(kr, cut.data());
        for (int i = 0; i < count; ++i) v[i] = full[i] - cut[i];
    });
    const int elsewhere = tableOf(key.bytes, a_count, low, most, a_rhoMax, threads,
                                  [this, &shared](double r, Complex* v) { integral(r, 0, v, &shared); }, &rho, &values);
    QList<QList<Complex>> table;
    table.reserve(rho.size());
    for (int i = 0; i < rho.size(); ++i) table << values.mid(i * a_count, a_count);
    a_table = table;
    a_rho = rho;
    return elsewhere;
}

Complex ViaGreen::fromTable(int component, double rho) const
{
    prepare();
    // (Through const views, as WaveGreen's: never detaching from a reader.)
    const QList<double>& nodes = a_rho;
    const QList<QList<Complex>>& table = a_table;
    if (nodes.size() < 8 || rho >= nodes.last()) {
        QVarLengthArray<Complex, 16> out(a_count);
        integral(std::max(rho, 0.0), 0, out.data());
        return out[component];
    }
    if (rho <= nodes.at(1)) {
        const double t = (rho / nodes.at(1)) * (rho / nodes.at(1));
        return table.at(0).at(component) + t * (table.at(1).at(component) - table.at(0).at(component));
    }
    const int i = int(std::upper_bound(nodes.begin(), nodes.end(), rho) - nodes.begin()) - 1;
    const int j = std::clamp(i - 2, 1, int(nodes.size()) - 6);
    Complex out(0, 0);
    for (int p = j; p < j + 6; ++p) {
        double weight = 1;
        for (int q = j; q < j + 6; ++q)
            if (q != p) weight *= (rho - nodes.at(q)) / (nodes.at(p) - nodes.at(q));
        out += weight * nodes.at(p) * table.at(p).at(component);
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
