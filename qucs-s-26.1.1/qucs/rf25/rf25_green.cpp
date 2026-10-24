/*
 * rf25_green.cpp - the static Green's function of a layered stack
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rf25_green.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

/// Ten-point Gauss-Legendre on [-1, 1]: its nodes and weights.
const double kGaussX[10] = {-0.9739065285171717, -0.8650633666889845, -0.6794095682990244, -0.4333953941292472,
                            -0.1488743389816312, 0.1488743389816312,  0.4333953941292472,  0.6794095682990244,
                            0.8650633666889845,  0.9739065285171717};
const double kGaussW[10] = {0.0666713443086881, 0.1494513491505806, 0.2190863625159820, 0.2692667193099963,
                            0.2955242247147529, 0.2955242247147529, 0.2692667193099963, 0.2190863625159820,
                            0.1494513491505806, 0.0666713443086881};

/// Integral of \a f over [a, b] by that rule.
template <typename F>
double gauss(const F& f, double a, double b)
{
    const double mid = (a + b) / 2, half = (b - a) / 2;
    double sum = 0;
    for (int i = 0; i < 10; ++i) sum += kGaussW[i] * f(mid + half * kGaussX[i]);
    return sum * half;
}

/// The n'th positive zero of J0, n from 1: the first five tabulated and
/// McMahon's expansion beyond, which is good to 1e-10 there. The
/// partition only has to lie near the zeros for the series it makes to
/// alternate, so this is more than enough.
double besselZero(int n)
{
    static const double first[5] = {2.404825557695773, 5.520078110286311, 8.653727912911013, 11.79153443901428,
                                    14.93091770848779};
    if (n >= 1 && n <= 5) return first[n - 1];
    const double b = (n - 0.25) * M_PI, b8 = 8 * b;
    return b + 1.0 / b8 * (1 - 124.0 / (3 * b8 * b8) * (1 - 120.0 * 1.0 / (b8 * b8)));
}

/// A function of k that is a sum of a_i exp(-k z_i), with z >= 0: what
/// every reflection of a stratified medium is, once the rational function
/// of exp(-2 k h) it is written as has been expanded. The terms are kept
/// in a list, and the operations are the few the recursion needs.
using Series = QList<QPair<double, double>>;   // (coefficient, path length)

constexpr double kTermFloor = 1e-13;   ///< a term smaller than this is dropped
constexpr int kMostTerms = 20000;      ///< and no more than this many are kept

///  s with the terms below the floor dropped and equal lengths added
/// together; what was dropped is added to  lost.
Series tidied(Series s, double* lost)
{
    std::sort(s.begin(), s.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    Series out;
    for (const auto& t : std::as_const(s)) {
        if (!out.isEmpty() && std::abs(t.second - out.last().second) <= 1e-15 * (1 + std::abs(t.second)))
            out.last().first += t.first;
        else
            out << t;
    }
    Series kept;
    for (const auto& t : std::as_const(out)) {
        if (std::abs(t.first) < kTermFloor) {
            if (lost != nullptr) *lost += std::abs(t.first);
        } else {
            kept << t;
        }
    }
    if (kept.size() > kMostTerms) {
        std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return std::abs(a.first) > std::abs(b.first); });
        for (int i = kMostTerms; i < kept.size(); ++i)
            if (lost != nullptr) *lost += std::abs(kept.at(i).first);
        kept.erase(kept.begin() + kMostTerms, kept.end());
    }
    return kept;
}

Series times(const Series& a, const Series& b, double* lost)
{
    Series out;
    out.reserve(std::min(a.size() * b.size(), qsizetype(kMostTerms * 2)));
    for (const auto& x : a)
        for (const auto& y : b) {
            const double c = x.first * y.first;
            if (std::abs(c) >= kTermFloor) out << QPair<double, double>{c, x.second + y.second};
            else if (lost != nullptr) *lost += std::abs(c);
        }
    return tidied(std::move(out), lost);
}

/// 1/(1 -  x), where every term of  x decays: the geometric series,
/// to the term that no longer matters.
Series overOneMinus(const Series& x, double* lost)
{
    Series out{{1.0, 0.0}}, power{{1.0, 0.0}};
    constexpr int kMostBounces = 2000;
    for (int n = 0; n < kMostBounces; ++n) {
        power = times(power, x, lost);
        if (power.isEmpty()) return tidied(std::move(out), lost);
        double biggest = 0;
        for (const auto& t : std::as_const(power)) biggest = std::max(biggest, std::abs(t.first));
        out += power;
        if (biggest < kTermFloor) return tidied(std::move(out), lost);
    }
    // The bounces ran out before the terms did: a stack that reflects
    // nearly everything, where the expansion is not the whole function.
    // What the next term would have been is counted as lost, which is the
    // order of all the rest of them, so that the caller can see it.
    if (lost != nullptr)
        for (const auto& t : std::as_const(power)) *lost += std::abs(t.first);
    return tidied(std::move(out), lost);
}

Series scaled(const Series& a, double by)
{
    Series out;
    out.reserve(a.size());
    for (const auto& t : a) out << QPair<double, double>{t.first * by, t.second};
    return out;
}

///  a with every path lengthened by  z: multiplying by exp(-k z).
Series shifted(const Series& a, double z)
{
    Series out;
    out.reserve(a.size());
    for (const auto& t : a) out << QPair<double, double>{t.first, t.second + z};
    return out;
}

} // namespace

double besselJ0(double x)
{
    // Its power series below 8, where it converges fast and without
    // cancellation, and the amplitude-and-phase form above - the standard
    // split (Abramowitz and Stegun 9.4), to about 1e-8 relative.
    const double ax = std::abs(x);
    if (ax < 8.0) {
        const double y = x * x;
        const double p = 57568490574.0
                         + y * (-13362590354.0 + y * (651619640.7 + y * (-11214424.18 + y * (77392.33017 + y * (-184.9052456)))));
        const double q = 57568490411.0 + y * (1029532985.0 + y * (9494680.718 + y * (59272.64853 + y * (267.8532712 + y))));
        return p / q;
    }
    const double z = 8.0 / ax, y = z * z;
    const double xx = ax - 0.785398164;
    const double p0 = 1.0 + y * (-0.1098628627e-2 + y * (0.2734510407e-4 + y * (-0.2073370639e-5 + y * 0.2093887211e-6)));
    const double q0 = -0.1562499995e-1 + y * (0.1430488765e-3 + y * (-0.6911147651e-5 + y * (0.7621095161e-6 + y * (-0.934935152e-7))));
    return std::sqrt(0.636619772 / ax) * (std::cos(xx) * p0 - z * std::sin(xx) * q0);
}

// ------------------------------------------------------------ The stack

int Layers::slabAt(double z) const
{
    for (int i = 0; i < slabs.size(); ++i)
        if (z >= slabs.at(i).z0 && z <= slabs.at(i).z1) return i;
    return -1;
}

Layers layersOf(const Stack& stack)
{
    Layers out;
    // The metals take no room: a conductor is a surface charge at a
    // height, which is the planar approximation this solver is built on
    // (a metal's thickness is for its resistance, not for its charge).
    // Each dielectric keeps its own thickness, so the stack is shorter
    // than the one the panel draws by the metals' thickness, and every
    // distance between a metal and a dielectric boundary is the one the
    // model gave.
    double z = 0;
    for (const Layer& l : stack.layers) {
        switch (l.kind) {
        case Layer::Ground:
            if (out.slabs.isEmpty()) out.groundBelow = true;
            else out.problems << tr("A ground plane above the stack is not solved yet: put it at the bottom.");
            break;
        case Layer::Cover:
            // (A cover closes the stack at the top; nothing is solved
            // above it yet, so it is read as the air above.)
            break;
        case Layer::Metal:
        case Layer::Via:
            break;   // no room of its own
        case Layer::Substrate:
        case Layer::Dielectric:
            if (l.thickness > 0) {
                Layers::Slab s;
                s.z0 = z;
                s.z1 = z + l.thickness;
                s.epsr = l.epsr > 0 ? l.epsr : 1;
                s.name = l.name;
                out.slabs << s;
                z = s.z1;
            }
            break;
        }
    }
    if (out.slabs.isEmpty()) out.problems << tr("The stack has no dielectric to solve in.");
    return out;
}

// ------------------------------------------------- The Green's function

Green::Green(const Layers& layers, double z) : a_z(z)
{
    if (!layers.ok()) {
        a_problems = layers.problems.isEmpty() ? QStringList{tr("The stack cannot be read as layers.")} : layers.problems;
        return;
    }
    const int at = layers.slabAt(z);
    if (at < 0) {
        a_problems << tr("A charge at %1 µm is outside the stack (it is %2 µm high).")
                          .arg(z * 1e6, 0, 'g', 4).arg(layers.height() * 1e6, 0, 'g', 4);
        return;
    }
    const Layers::Slab& slab = layers.slabs.at(at);
    a_epsr = slab.epsr;
    a_up = slab.z1 - z;
    a_down = z - slab.z0;
    a_ground = layers.groundBelow;

    // The interfaces above the charge's slab, and the slabs between them:
    // the first coefficient is of its own top interface.
    for (int i = at; i < layers.slabs.size(); ++i) {
        const double above = i + 1 < layers.slabs.size() ? layers.slabs.at(i + 1).epsr : layers.epsrAbove;
        const double here = layers.slabs.at(i).epsr;
        a_rUp << (here - above) / (here + above);
        if (i + 1 < layers.slabs.size()) a_hUp << (layers.slabs.at(i + 1).z1 - layers.slabs.at(i + 1).z0);
    }
    // And below it, the first of its own bottom interface.
    for (int i = at; i >= 0; --i) {
        const double below = i > 0 ? layers.slabs.at(i - 1).epsr : layers.epsrBelow;
        const double here = layers.slabs.at(i).epsr;
        a_rDown << (here - below) / (here + below);
        if (i > 0) a_hDown << (layers.slabs.at(i - 1).z1 - layers.slabs.at(i - 1).z0);
    }

    // The images: the reflections expanded into the paths that make them.
    // Looking up, from the topmost interface down to the charge's own; the
    // recursion of a stratified medium, written out.
    Series up{{a_rUp.isEmpty() ? 0.0 : a_rUp.last(), 0.0}};
    for (int i = a_rUp.size() - 2; i >= 0; --i) {
        const Series e = shifted(up, 2 * a_hUp.at(i));          // R' exp(-2 k h)
        const Series num = Series{{a_rUp.at(i), 0.0}} + e;      // r + R' E
        const Series den = scaled(e, -a_rUp.at(i));             // -(r R' E)
        up = times(num, overOneMinus(den, &a_truncated), &a_truncated);
    }
    Series down{{a_ground ? -1.0 : (a_rDown.isEmpty() ? 0.0 : a_rDown.last()), 0.0}};
    for (int i = a_rDown.size() - 2; i >= 0; --i) {
        const Series e = shifted(down, 2 * a_hDown.at(i));
        const Series num = Series{{a_rDown.at(i), 0.0}} + e;
        const Series den = scaled(e, -a_rDown.at(i));
        down = times(num, overOneMinus(den, &a_truncated), &a_truncated);
    }
    // The charge and the point are at one height, so one bounce down is
    // 2 a_down of path, one bounce up 2 a_up, and a round trip the slab
    // twice over.
    const double h = a_up + a_down;
    const Series both = shifted(times(up, down, &a_truncated), 2 * h);
    Series series = shifted(down, 2 * a_down) + shifted(up, 2 * a_up) + scaled(both, 2.0);
    series = times(tidied(std::move(series), &a_truncated), overOneMinus(both, &a_truncated), &a_truncated);
    series = tidied(std::move(series), &a_truncated);
    std::sort(series.begin(), series.end(), [](const auto& a, const auto& b) { return std::abs(a.first) > std::abs(b.first); });
    a_images.reserve(series.size());
    for (const auto& t : std::as_const(series)) a_images << Image{t.first, t.second};
}

double Green::spectralReflection(double k) const
{
    if (k <= 0) {
        // The limit at k = 0: every exponential is one. (A test asks for
        // it; the integral's own panels start above zero.)
        double ru = a_rUp.isEmpty() ? 0.0 : a_rUp.last();
        for (int i = a_rUp.size() - 2; i >= 0; --i) ru = (a_rUp.at(i) + ru) / (1 + a_rUp.at(i) * ru);
        double rd = a_ground ? -1.0 : (a_rDown.isEmpty() ? 0.0 : a_rDown.last());
        for (int i = a_rDown.size() - 2; i >= 0; --i) rd = (a_rDown.at(i) + rd) / (1 + a_rDown.at(i) * rd);
        const double den = 1 - ru * rd;
        return std::abs(den) < 1e-14 ? 0.0 : (rd + ru + 2 * ru * rd) / den;
    }
    // Looking up from the charge's slab, and down: the generalized
    // reflection of a stratified medium, which only ever holds decaying
    // exponentials and so cannot overflow.
    double ru = a_rUp.isEmpty() ? 0.0 : a_rUp.last();
    for (int i = a_rUp.size() - 2; i >= 0; --i) {
        const double e = std::exp(-2 * k * a_hUp.at(i));
        ru = (a_rUp.at(i) + ru * e) / (1 + a_rUp.at(i) * ru * e);
    }
    double rd = a_ground ? -1.0 : (a_rDown.isEmpty() ? 0.0 : a_rDown.last());
    for (int i = a_rDown.size() - 2; i >= 0; --i) {
        const double e = std::exp(-2 * k * a_hDown.at(i));
        rd = (a_rDown.at(i) + rd * e) / (1 + a_rDown.at(i) * rd * e);
    }
    // The charge and the point are at one height, so the two one-bounce
    // terms are e^-2k(distance to that interface) and the round trip is
    // twice the slab.
    const double h = a_up + a_down;
    const double eUp = std::exp(-2 * k * a_up), eDown = std::exp(-2 * k * a_down), eBoth = std::exp(-2 * k * h);
    const double den = 1 - ru * rd * eBoth;
    if (std::abs(den) < 1e-14) return 0;
    return (rd * eDown + ru * eUp + 2 * ru * rd * eBoth) / den;
}

double Green::freeSpace(double rho) const
{
    return rho > 0 ? 1.0 / (4 * M_PI * epsilon() * rho) : 0.0;
}

double Green::reflected(double rho) const
{
    if (!ok() || rho <= 0) return 0;
    double sum = 0;
    for (const Image& i : a_images) sum += i.a / std::sqrt(rho * rho + i.z * i.z);
    return sum / (4 * M_PI * epsilon());
}

double Green::reflectedByIntegral(double rho) const
{
    if (!ok() || rho <= 0) return 0;
    // What the reflections tend to as k grows: a charge lying on an
    // interface sees its image at no distance at all, and that part of the
    // integral is the closed form 1/rho rather than something to
    // integrate. What is left of the reflections decays.
    const double rUp = a_rUp.isEmpty() ? 0.0 : a_rUp.first();
    const double rDown = a_ground ? -1.0 : (a_rDown.isEmpty() ? 0.0 : a_rDown.first());
    const double tail = (a_down <= 0 ? rDown : 0.0) + (a_up <= 0 ? rUp : 0.0);
    const auto f = [this, tail](double k) { return spectralReflection(k) - tail; };

    // The scales the integrand decays on: the smallest sets how far in k
    // it reaches, the largest how finely it must be sampled there.
    double small = std::numeric_limits<double>::max(), large = 0;
    const auto note = [&](double d) {
        if (d > 0) {
            small = std::min(small, d);
            large = std::max(large, d);
        }
    };
    note(a_up);
    note(a_down);
    for (double h : std::as_const(a_hUp)) note(h);
    for (double h : std::as_const(a_hDown)) note(h);
    if (large <= 0) return tail / (4 * M_PI * epsilon() * rho);
    const double kMax = 40.0 / small;

    double integral = 0;
    // Panels while J0 turns a manageable number of times over the range,
    // which is the accurate way: graded in k, so that an integrand made of
    // exponentials of very different scales is followed, and no panel
    // wider than half a turn of J0, so that the oscillation is resolved.
    // (Where it turns hundreds of times the panels would be thousands, and
    // the branch below does it by the series instead.)
    if (rho * kMax < 2000) {
        const double k0 = 1e-4 / large;
        const double widest = M_PI / rho;   // half a turn of J0
        const auto integrand = [&](double k) { return f(k) * besselJ0(k * rho); };
        const auto over = [&](double a, double b) {
            const int splits = std::max(1, int(std::ceil((b - a) / widest)));
            double sum = 0;
            for (int i = 0; i < splits; ++i)
                sum += gauss(integrand, a + (b - a) * i / splits, a + (b - a) * (i + 1) / splits);
            return sum;
        };
        integral = over(0, k0);
        const int perDecade = 14;
        const int panels = std::max(1, int(std::ceil(std::log10(kMax / k0) * perDecade)));
        const double step = std::pow(kMax / k0, 1.0 / panels);
        double a = k0;
        for (int i = 0; i < panels; ++i) {
            const double b = a * step;
            integral += over(a, b);
            a = b;
        }
        return integral / (4 * M_PI * epsilon()) + (tail != 0 ? tail / (4 * M_PI * epsilon() * rho) : 0.0);
    }
    // Far out, J0 turns many times over the range the integrand lives on:
    // the integral is taken between J0's zeros, which makes an alternating
    // series, and the series is accelerated by averaging its partial sums
    // (Euler's transformation). That is what keeps the cost to a few dozen
    // intervals where panels would be thousands - but it is an accelerator
    // only: when the intervals reach the end of the range, the plain sum
    // is the whole integral and is the answer, and averaging it with
    // earlier partial sums would only spoil it.
    const auto averaged = [](const QList<double>& partial) {
        QList<double> level = partial;
        for (int pass = 0; pass < 2 && level.size() > 2; ++pass) {
            QList<double> next;
            next.reserve(level.size() - 1);
            for (int i = 0; i + 1 < level.size(); ++i) next << (level.at(i) + level.at(i + 1)) / 2;
            level = next;
        }
        return level.isEmpty() ? 0.0 : level.last();
    };
    QList<double> partial;
    double sum = 0, a = 0, estimate = 0;
    bool settled = false;
    for (int n = 1; n <= 4000; ++n) {
        const double b = besselZero(n) / rho;
        sum += gauss([&](double k) { return f(k) * besselJ0(k * rho); }, a, std::min(b, kMax));
        partial << sum;
        a = b;
        if (b >= kMax) break;   // the whole range covered: sum is the integral
        if (partial.size() >= 8) {
            const double now = averaged(partial);
            if (std::abs(now - estimate) <= 1e-13 * std::abs(now)) {
                settled = true;
                estimate = now;
                break;
            }
            estimate = now;
        }
    }
    integral = settled ? estimate : sum;
    return integral / (4 * M_PI * epsilon()) + (tail != 0 ? tail / (4 * M_PI * epsilon() * rho) : 0.0);
}

double Green::potential(double rho) const
{
    if (!ok() || rho <= 0) return 0;
    return freeSpace(rho) + reflected(rho);
}

} // namespace qucs_s::rf25
