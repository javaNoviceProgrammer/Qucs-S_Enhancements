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

/*!
 * How weak a term may be before it is pruned, measured as what it is
 * worth beside one at the stack's finest length (Budget::worth).
 *
 * A part in a hundred million, which is four orders below what the method
 * of moments on top of this can tell apart and three below what the
 * spectral integral the images are checked against can resolve. Tightening
 * it does not make an answer better; it only makes a deep stack
 * combinatorial, which five slabs at 1e-13 showed by running to eighteen
 * thousand images and half a minute.
 */
constexpr double kTermFloor = 1e-13;
constexpr int kMostTerms = 20000;      ///< and no more than this many are kept

/*!
 * What a term is worth, and what was dropped.
 *
 * Enumerating every path through a stack is combinatorial - five slabs ran
 * to eighteen thousand images and half a minute - and nearly all of the
 * long ones are worth nothing, because an image at a distance z is worth
 * at most a/z. So a term is judged by that, against the finest length of
 * the stack, and not by its coefficient alone: an image a hundred slabs
 * away must be a hundred times stronger to be kept.
 *
 * What is pruned is not thrown away. Its charge and its first moment are
 * kept and put back at the end as one image at their own mean distance,
 * which is right in both limits - near the charge those paths were
 * negligible anyway, and far from it only the total charge is felt - and
 * between them is wrong by no more than the pruned terms themselves.
 */
struct Budget {
    double zRef = 1;        ///< the length a path is judged against, m
    double lumpA = 0;       ///< the coarsened terms' charge
    double lumpAz = 0;      ///< and their first moment
    double coarsened = 0;   ///< their worth, which the lump stands for
    double missing = 0;     ///< the worth of what nothing stands for

    /// What a term at \a z with coefficient \a a is worth, relative to
    /// one at the reference length.
    double worth(double a, double z) const { return std::abs(a) * zRef / std::max(z, zRef); }
    /// A term too weak to keep on its own: its charge goes into the lump,
    /// which is put back at the end, so nothing of it is lost but where
    /// along the path it sat.
    void coarsen(double a, double z)
    {
        lumpA += a;
        lumpAz += a * z;
        coarsened += worth(a, z);
    }
    /// A term nothing stands for: the bounces ran out before the series
    /// did, so this much of the reflection is simply not there.
    void lose(double a, double z) { missing += worth(a, z); }
};

///  s with the terms below the floor dropped and equal lengths added
/// together; what was dropped is added to  lost.
Series tidied(Series s, Budget* lost)
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
        if (lost != nullptr && lost->worth(t.first, t.second) < kTermFloor) lost->coarsen(t.first, t.second);
        else if (std::abs(t.first) >= kTermFloor) kept << t;
    }
    if (kept.size() > kMostTerms) {
        std::sort(kept.begin(), kept.end(), [&lost](const auto& a, const auto& b) {
            return (lost != nullptr ? lost->worth(a.first, a.second) : std::abs(a.first))
                   > (lost != nullptr ? lost->worth(b.first, b.second) : std::abs(b.first));
        });
        for (int i = kMostTerms; i < kept.size(); ++i)
            if (lost != nullptr) lost->coarsen(kept.at(i).first, kept.at(i).second);
        kept.erase(kept.begin() + kMostTerms, kept.end());
    }
    return kept;
}

Series times(const Series& a, const Series& b, Budget* lost)
{
    Series out;
    out.reserve(std::min(a.size() * b.size(), qsizetype(kMostTerms * 2)));
    for (const auto& x : a)
        for (const auto& y : b) {
            const double c = x.first * y.first, z = x.second + y.second;
            if (lost != nullptr && lost->worth(c, z) < kTermFloor) lost->coarsen(c, z);
            else if (std::abs(c) >= kTermFloor) out << QPair<double, double>{c, z};
        }
    return tidied(std::move(out), lost);
}

/// 1/(1 -  x), where every term of  x decays: the geometric series,
/// to the term that no longer matters.
Series overOneMinus(const Series& x, Budget* lost)
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
        for (const auto& t : std::as_const(power)) lost->lose(t.first, t.second);
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
            // A ground is a plane under everything, wherever the tree has
            // it - the stack puts it at nought - so where it comes in the
            // list says nothing. It was read by its place: one listed
            // after a slab was refused as "above the stack", and the one
            // order accepted was the one the stack's own checks refused,
            // so no model with a ground was both whole and solvable (the
            // phase A check of 24 October).
            //
            // Open is no plane at all, and was solved as a perfect one.
            if (l.variant != QLatin1String("open")) out.groundBelow = true;
            break;
        case Layer::Cover:
            // An open cover is the air above, which is what is solved. A
            // metal lid is not solved yet, and is said rather than read
            // as the air it is not.
            if (l.variant == QLatin1String("perfect"))
                out.problems << tr("A metal lid over the stack is not solved yet: make the cover open (air above), or "
                                   "take it out.");
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

Green::Green(const Layers& layers, double z) : Green(layers, z, z) {}

double Green::interfaceAt(int i) const
{
    const double here = a_layers.slabs.at(i).epsr;
    const double above = i + 1 < a_layers.slabs.size() ? a_layers.slabs.at(i + 1).epsr : a_layers.epsrAbove;
    return (here - above) / (here + above);
}

double Green::thicknessAt(int i) const
{
    return a_layers.slabs.at(i).z1 - a_layers.slabs.at(i).z0;
}

double Green::upwardAt(int i, double k) const
{
    double r = interfaceAt(a_layers.slabs.size() - 1);
    for (int j = int(a_layers.slabs.size()) - 2; j >= i; --j) {
        const double e = std::exp(-2 * k * thicknessAt(j + 1));
        r = (interfaceAt(j) + r * e) / (1 + interfaceAt(j) * r * e);
    }
    return r;
}

double Green::downwardAt(int i, double k) const
{
    // Looking down from slab j's bottom interface: the reflection of that
    // interface is the one above it, taken the other way round.
    const auto below = [this](int j) {
        const double here = a_layers.slabs.at(j).epsr;
        const double under = j > 0 ? a_layers.slabs.at(j - 1).epsr : a_layers.epsrBelow;
        return (here - under) / (here + under);
    };
    double r = a_ground ? -1.0 : below(0);
    for (int j = 1; j <= i; ++j) {
        const double e = std::exp(-2 * k * thicknessAt(j - 1));
        r = (below(j) + r * e) / (1 + below(j) * r * e);
    }
    return r;
}

Green::Green(const Layers& layers, double zField, double zSource)
    : a_layers(layers), a_z(zField), a_zSource(zSource)
{
    if (!layers.ok()) {
        a_problems = layers.problems.isEmpty() ? QStringList{tr("The stack cannot be read as layers.")} : layers.problems;
        return;
    }
    a_src = layers.slabAt(zSource);
    a_fld = layers.slabAt(zField);
    if (a_src < 0) {
        a_problems << tr("A charge at %1 µm is outside the stack (it is %2 µm high).")
                          .arg(zSource * 1e6, 0, 'g', 4).arg(layers.height() * 1e6, 0, 'g', 4);
        return;
    }
    if (a_fld < 0) {
        a_problems << tr("A point at %1 µm is outside the stack (it is %2 µm high).")
                          .arg(zField * 1e6, 0, 'g', 4).arg(layers.height() * 1e6, 0, 'g', 4);
        return;
    }
    a_epsr = layers.slabs.at(a_src).epsr;
    a_ground = layers.groundBelow;
    // The stack's finest length, which is what a path is judged against.
    a_zRef = std::numeric_limits<double>::max();
    for (int i = 0; i < layers.slabs.size(); ++i)
        a_zRef = std::min(a_zRef, layers.slabs.at(i).z1 - layers.slabs.at(i).z0);
    if (!(a_zRef > 0) || a_zRef == std::numeric_limits<double>::max()) a_zRef = 1e-9;

}

void Green::build() const
{
    if (a_built || !ok()) return;
    a_built = true;
    const Layers& layers = a_layers;
    // ---- the images, by the same recursion written symbolically.
    //
    // Judged against the finest length of the stack: an image far away is
    // worth little however strong it is, and enumerating every one of the
    // long paths is what makes a deep stack combinatorial.
    Budget budget;
    budget.zRef = a_zRef;
    //
    // A series here is a sum of a exp(-k z): the coefficient of an image
    // and the length of the path it travelled. Every exponent below is
    // negative, so no path is of negative length and nothing grows.
    // (Built once each: the chain below asks for the same slab's
    // reflections several times over, and each one is the whole nested
    // expansion above or below it.)
    QHash<int, Series> upCache, downCache;
    const auto up = [this, &budget, &upCache](int i) {
        if (upCache.contains(i)) return upCache.value(i);
        Series r{{interfaceAt(int(a_layers.slabs.size()) - 1), 0.0}};
        for (int j = int(a_layers.slabs.size()) - 2; j >= i; --j) {
            const Series e = shifted(r, 2 * thicknessAt(j + 1));
            r = times(Series{{interfaceAt(j), 0.0}} + e, overOneMinus(scaled(e, -interfaceAt(j)), &budget),
                      &budget);
        }
        upCache.insert(i, r);
        return r;
    };
    const auto belowOf = [this](int j) {
        const double here = a_layers.slabs.at(j).epsr;
        const double under = j > 0 ? a_layers.slabs.at(j - 1).epsr : a_layers.epsrBelow;
        return (here - under) / (here + under);
    };
    const auto down = [this, &belowOf, &budget, &downCache](int i) {
        if (downCache.contains(i)) return downCache.value(i);
        Series r{{a_ground ? -1.0 : belowOf(0), 0.0}};
        for (int j = 1; j <= i; ++j) {
            const Series e = shifted(r, 2 * thicknessAt(j - 1));
            r = times(Series{{belowOf(j), 0.0}} + e, overOneMinus(scaled(e, -belowOf(j)), &budget), &budget);
        }
        downCache.insert(i, r);
        return r;
    };

    Series series;
    if (a_fld == a_src) {
        const double h = thicknessAt(a_src);
        const double u = a_z - a_layers.slabs.at(a_src).z0, us = a_zSource - a_layers.slabs.at(a_src).z0;
        const Series ru = up(a_src), rd = down(a_src);
        const Series both = shifted(times(ru, rd, &budget), 2 * h);
        // One bounce down, one up, and the two round trips - whose paths
        // are 2h minus and plus the gap between the heights, both of them
        // longer than the slab itself.
        Series numerator = shifted(rd, u + us) + shifted(ru, 2 * h - u - us)
                           + shifted(times(ru, rd, &budget), 2 * h - (u - us))
                           + shifted(times(ru, rd, &budget), 2 * h + (u - us));
        series = times(tidied(std::move(numerator), &budget), overOneMinus(both, &budget), &budget);
        // Two metals with one dielectric between them are both in that
        // slab, at its two faces: the same slab, but not the same height.
        // The way straight from one to the other is then a term like any
        // other, since there is no 1/rho to take out of it.
        if (a_z != a_zSource) series << QPair<double, double>{1.0, std::abs(a_z - a_zSource)};
    } else {
        // Across two slabs. The wave that leaves the source's slab on the
        // side the point is, then through each interface between them,
        // then what it does inside the point's own slab. Taken upward;
        // where the point is below the charge it is the same thing upside
        // down, which is what the mirrored indices do.
        const bool upward = a_fld > a_src;
        const int lo = upward ? a_src : a_fld, hi = upward ? a_fld : a_src;
        // Measured from the source's slab in the direction of travel.
        const int from = upward ? a_src : a_fld;   // the slab the wave starts in
        Q_UNUSED(lo);
        Q_UNUSED(hi);
        const double hs = thicknessAt(a_src);
        const double us = a_zSource - a_layers.slabs.at(a_src).z0;
        // How far the charge is from the face it leaves by, and how far
        // the point is from the face the wave arrives at.
        const double leaveAt = upward ? hs - us : us;
        const double arriveAt = upward ? a_z - a_layers.slabs.at(a_fld).z0
                                       : a_layers.slabs.at(a_fld).z1 - a_z;
        const Series ruS = upward ? up(a_src) : down(a_src);
        const Series rdS = upward ? down(a_src) : up(a_src);
        // Leaving the source's slab: the direct way out, and the way that
        // went the other way first and came back.
        Series amp = times(Series{{1.0, leaveAt}} + shifted(rdS, 2 * hs - leaveAt),
                           overOneMinus(shifted(times(ruS, rdS, &budget), 2 * hs), &budget), &budget);
        // Through each interface on the way, and across each slab between
        // them. The cavity of a slab it only passes through is of that
        // slab's two faces alone: the reflection back the way it came is
        // the one interface it just crossed, not everything beyond it,
        // which those paths are already counted in.
        const int step = upward ? 1 : -1;
        for (int j = a_src; j != a_fld; j += step) {
            const int face = upward ? j : j - 1;        // the interface between j and the next
            const double t = upward ? 1 + interfaceAt(face) : 1 - interfaceAt(face);
            amp = scaled(amp, t);
            const int next = j + step;
            if (next != a_fld) {
                const double hj = thicknessAt(next);
                const Series ahead = upward ? up(next) : down(next);
                const double back = upward ? -interfaceAt(face) : interfaceAt(face);
                amp = times(shifted(amp, hj), overOneMinus(shifted(scaled(ahead, back), 2 * hj), &budget),
                            &budget);
            }
        }
        // And inside the point's own slab: straight to it, and round off
        // the far face.
        const double hf = thicknessAt(a_fld);
        const int lastFace = upward ? a_fld - 1 : a_fld;
        const Series ahead = upward ? up(a_fld) : down(a_fld);
        const double back = upward ? -interfaceAt(lastFace) : interfaceAt(lastFace);
        const Series inside = times(Series{{1.0, arriveAt}} + shifted(ahead, 2 * hf - arriveAt),
                                    overOneMinus(shifted(scaled(ahead, back), 2 * hf), &budget), &budget);
        series = times(amp, inside, &budget);
        Q_UNUSED(from);
    }
    series = tidied(std::move(series), &budget);
    // What was pruned, put back as one image at the mean distance of the
    // paths it stands for: near the charge those paths were worth nothing,
    // far from it only their charge is felt, and this is right in both.
    if (std::abs(budget.lumpA) > kTermFloor) {
        const double z = std::abs(budget.lumpAz / budget.lumpA);
        series << QPair<double, double>{budget.lumpA, std::max(z, budget.zRef)};
    }
    // What is missing is what nothing stands for - the bounces that ran
    // out - and not what was coarsened into the lump, whose charge is
    // still there and only its place along the path approximate.
    a_truncated = budget.missing;
    a_coarsened = budget.coarsened;
    // Strongest first, by what each is worth near the charge: that is the
    // order strongestTerms() hands out, and the order a reader wants.
    std::sort(series.begin(), series.end(), [&budget](const auto& a, const auto& b) {
        return budget.worth(a.first, a.second) > budget.worth(b.first, b.second);
    });
    a_images.reserve(series.size());
    for (const auto& t : std::as_const(series)) a_images << Image{t.first, t.second};

    // The table over distance, filled from the images. Building it costs
    // one pass over them; reading it costs nothing, which is what a method
    // of moments needs when a stack of three slabs runs to six thousand
    // images and every pair of cells would otherwise sum all of them.
    double small = std::numeric_limits<double>::max(), large = 0;
    const auto note = [&](double d) {
        if (d > 0) {
            small = std::min(small, d);
            large = std::max(large, d);
        }
    };
    for (int i = 0; i < layers.slabs.size(); ++i) note(thicknessAt(i));
    note(std::abs(a_z - a_zSource));
    note(a_layers.slabs.at(a_src).z1 - a_zSource);
    note(a_zSource - a_layers.slabs.at(a_src).z0);
    if (large <= 0) {
        small = large = std::max(1e-9, a_zRef);
    }
    const double from = small * 1e-4, to = large * 1e4;
    // A hundred and twenty a decade: the product curves most where the
    // nearest image sits, and sixty left a few parts in a hundred
    // thousand there. Filling costs one pass over the images, which is
    // nothing beside what reading the table saves.
    const int count = std::clamp(int(std::ceil(std::log10(to / from) * 120)), 320, 3200);
    a_rho.reserve(count);
    a_table.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double rho = from * std::pow(to / from, double(i) / (count - 1));
        a_rho << rho;
        a_table << rho * reflected(rho);
    }
}

double Green::reflectedByTable(double rho) const
{
    if (!ok() || rho <= 0) return 0;
    build();
    if (a_table.isEmpty() || rho < a_rho.first() || rho > a_rho.last()) return reflected(rho);
    // A cubic through the four samples around it (Catmull-Rom), of rho
    // times the value rather than the value: that product tends to a
    // constant at either end and is smooth between, where the value
    // itself curves like 1/sqrt(rho^2 + z^2).
    const double t = std::log(rho / a_rho.first()) / std::log(a_rho.last() / a_rho.first());
    const double x = t * (a_table.size() - 1);
    const int i = std::clamp(int(x), 0, int(a_table.size()) - 2);
    const double f = x - i;
    const double p0 = a_table.at(std::max(0, i - 1)), p1 = a_table.at(i), p2 = a_table.at(i + 1),
                 p3 = a_table.at(std::min(int(a_table.size()) - 1, i + 2));
    const double product =
        p1 + 0.5 * f * ((p2 - p0) + f * ((2 * p0 - 5 * p1 + 4 * p2 - p3) + f * (3 * (p1 - p2) + p3 - p0)));
    return product / rho;
}

QList<Green::Image> Green::strongestTerms(int most) const
{
    QList<Image> all = allTerms();
    if (all.size() <= most) return all;
    all.erase(all.begin() + most, all.end());
    return all;
}

double Green::beyondStrongest(double rho, int most) const
{
    if (!ok() || rho <= 0) return 0;
    build();
    double sum = 0;
    for (int i = most; i < a_images.size(); ++i)
        sum += a_images.at(i).a / std::sqrt(rho * rho + a_images.at(i).z * a_images.at(i).z);
    // (allTerms() puts the charge itself first at one height, so the
    // images beyond the n'th of allTerms() are the images beyond the
    // (n-1)'th of a_images.)
    if (sameHeight() && most > 0) {
        sum = 0;
        for (int i = most - 1; i < a_images.size(); ++i)
            sum += a_images.at(i).a / std::sqrt(rho * rho + a_images.at(i).z * a_images.at(i).z);
    }
    return sum / (4 * M_PI * epsilon());
}

double Green::spectralReflection(double k) const
{
    if (!ok()) return 0;
    if (k <= 0) k = 1e-12 / std::max(1e-12, a_layers.height());
    const double hs = thicknessAt(a_src);
    const double us = a_zSource - a_layers.slabs.at(a_src).z0;
    if (a_fld == a_src) {
        const double u = a_z - a_layers.slabs.at(a_src).z0;
        const double ru = upwardAt(a_src, k), rd = downwardAt(a_src, k);
        const double den = 1 - ru * rd * std::exp(-2 * k * hs);
        if (std::abs(den) < 1e-14) return 0;
        const double direct = a_z != a_zSource ? std::exp(-k * std::abs(u - us)) : 0.0;
        return direct
               + (rd * std::exp(-k * (u + us)) + ru * std::exp(-k * (2 * hs - u - us))
                  + ru * rd * (std::exp(-k * (2 * hs - (u - us))) + std::exp(-k * (2 * hs + (u - us)))))
                     / den;
    }
    const bool upward = a_fld > a_src;
    const double leaveAt = upward ? hs - us : us;
    const double arriveAt = upward ? a_z - a_layers.slabs.at(a_fld).z0 : a_layers.slabs.at(a_fld).z1 - a_z;
    const double ruS = upward ? upwardAt(a_src, k) : downwardAt(a_src, k);
    const double rdS = upward ? downwardAt(a_src, k) : upwardAt(a_src, k);
    double amp = (std::exp(-k * leaveAt) + rdS * std::exp(-k * (2 * hs - leaveAt)))
                 / (1 - ruS * rdS * std::exp(-2 * k * hs));
    const int step = upward ? 1 : -1;
    for (int j = a_src; j != a_fld; j += step) {
        const int face = upward ? j : j - 1;
        amp *= upward ? 1 + interfaceAt(face) : 1 - interfaceAt(face);
        const int next = j + step;
        if (next != a_fld) {
            const double hj = thicknessAt(next);
            const double ahead = upward ? upwardAt(next, k) : downwardAt(next, k);
            const double back = upward ? -interfaceAt(face) : interfaceAt(face);
            amp *= std::exp(-k * hj) / (1 - ahead * back * std::exp(-2 * k * hj));
        }
    }
    const double hf = thicknessAt(a_fld);
    const int lastFace = upward ? a_fld - 1 : a_fld;
    const double ahead = upward ? upwardAt(a_fld, k) : downwardAt(a_fld, k);
    const double back = upward ? -interfaceAt(lastFace) : interfaceAt(lastFace);
    return amp * (std::exp(-k * arriveAt) + ahead * std::exp(-k * (2 * hf - arriveAt)))
           / (1 - ahead * back * std::exp(-2 * k * hf));
}

QList<Green::Image> Green::allTerms() const
{
    build();
    QList<Image> out;
    if (sameHeight()) out << Image{1.0, 0.0};
    out += a_images;
    return out;
}

double Green::freeSpace(double rho) const
{
    // Across two heights the direct path crosses interfaces to get there,
    // so it is one of the images and there is no free-space term of its
    // own to take out.
    return sameHeight() && rho > 0 ? 1.0 / (4 * M_PI * epsilon() * rho) : 0.0;
}

double Green::reflected(double rho) const
{
    if (!ok() || rho <= 0) return 0;
    build();
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
    // integrate. What is left of the reflections decays. Across two
    // heights there is no such part at all, since every path there has a
    // slab to cross.
    const double toTop = a_layers.slabs.at(a_src).z1 - a_zSource, toBottom = a_zSource - a_layers.slabs.at(a_src).z0;
    const double rUp = interfaceAt(a_src);
    const double rDown = a_ground && a_src == 0
                             ? -1.0
                             : (a_layers.slabs.at(a_src).epsr - (a_src > 0 ? a_layers.slabs.at(a_src - 1).epsr : a_layers.epsrBelow))
                                   / (a_layers.slabs.at(a_src).epsr + (a_src > 0 ? a_layers.slabs.at(a_src - 1).epsr : a_layers.epsrBelow));
    const double tail = sameHeight() ? ((toBottom <= 0 ? rDown : 0.0) + (toTop <= 0 ? rUp : 0.0)) : 0.0;
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
    note(toTop);
    note(toBottom);
    note(std::abs(a_z - a_zSource));
    for (int i = 0; i < a_layers.slabs.size(); ++i) note(thicknessAt(i));
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
    return freeSpace(rho) + reflectedByTable(rho);
}

} // namespace qucs_s::rf25
