/*
 * rf25_deembed.cpp - an edge port's own discontinuity taken out of a
 *                    full-wave solve, and its reference plane moved along
 *                    its line
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_deembed.h"

#include <QCoreApplication>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

constexpr double kMu0 = 4e-7 * M_PI;

using Chain = std::array<Complex, 4>;   // [A, B, C, D]
using MatrixC = Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic>;

Chain times(const Chain& x, const Chain& y)
{
    return {x[0] * y[0] + x[1] * y[2], x[0] * y[1] + x[1] * y[3], x[2] * y[0] + x[3] * y[2], x[2] * y[1] + x[3] * y[3]};
}

Chain inverse(const Chain& x)
{
    const Complex d = x[0] * x[3] - x[1] * x[2];
    return {x[3] / d, -x[1] / d, -x[2] / d, x[0] / d};
}

/// A reciprocal two-port the other way round, its ports swapped.
Chain reversed(const Chain& x)
{
    return {x[3], x[1], x[2], x[0]};
}

/// A line \a d (m) long of propagation constant \a gamma and impedance \a z.
Chain lineOf(Complex gamma, Complex z, double d)
{
    const Complex c = std::cosh(gamma * d), s = std::sinh(gamma * d);
    return {c, z * s, s / z, c};
}

/// A two-port's chain matrix from its Z matrix.
Chain chainOf(const QList<QList<Complex>>& z)
{
    const Complex z11 = z[0][0], z12 = z[0][1], z21 = z[1][0], z22 = z[1][1];
    return {z11 / z21, (z11 * z22 - z12 * z21) / z21, 1.0 / z21, z22 / z21};
}

MatrixC matrixOf(const QList<QList<Complex>>& rows)
{
    const int n = int(rows.size());
    MatrixC m(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) m(i, j) = rows.at(i).at(j);
    return m;
}

QList<QList<Complex>> rowsOf(const MatrixC& m)
{
    QList<QList<Complex>> out;
    for (int i = 0; i < m.rows(); ++i) {
        QList<Complex> row;
        for (int j = 0; j < m.cols(); ++j) row << m(i, j);
        out << row;
    }
    return out;
}

/// How far a line's chain matrix \a a is from \b, the line \a z's: the
/// largest term of the difference over the largest of b, B taken in z and C
/// in 1/z - each a cosh or a sinh, none in ohms beside the others.
double apart(const Chain& a, const Chain& b, Complex z)
{
    const Chain na = {a[0], a[1] / z, a[2] * z, a[3]}, nb = {b[0], b[1] / z, b[2] * z, b[3]};
    double d = 0, m = 0;
    for (int i = 0; i < 4; ++i) {
        d = std::max(d, std::abs(na[i] - nb[i]));
        m = std::max(m, std::abs(nb[i]));
    }
    return m > 0 ? d / m : d;
}

constexpr double kPhi = 1.6180339887498949;

/// Strips of a feed solved at one length, and what they say at each
/// frequency.
struct Strips {
    double between = 0;            ///< the shorter strip's line between its ends' first vias, the layout's units
    double third = 0;              ///< the third's, which checks the other two
    double strip = 0;              ///< how long the shorter strip is, whole
    QList<double> at;              ///< Hz, as solved: those asked, and one below them to follow the phase up from
    double anchor = 0;
    bool anchored = false;
    QList<Chain> error;            ///< the port's own discontinuity at each of them
    QList<Complex> gamma, impedance;
    QList<double> residual;
    QList<double> halfWaves, gains;   ///< Hz
    QString problem;
    QStringList warnings;          ///< its strips' solves' (GMRES short of its residual, near its limit)
    bool tooLarge = false;
    int facing = 0;                ///< the sides its meshes graded where they face a gap (Mesh::facing)
    int indexOf(double f) const { return int(std::lower_bound(at.begin(), at.end(), f) - at.begin()); }
};

/*!
 * Strips of \a feed, the shorter's line \a base less its ends' first vias
 * (a whole number of the rows' periods), the longer's twice it and the
 * third's phi times it, solved at \a frequencies and read at each: E from
 * the shorter and the longer, the line from the shorter - its impedance
 * from the third where that is the further from a whole number of half
 * waves - and how far the third and the shorter are from it. \a cut: where
 * a strip is more than a dense solve takes, said so and nothing solved.
 */
Strips solveStrips(const Stack& stack, const MeshSettings& how, double top, const Feed& feed, const QList<double>& frequencies,
                   double micrometre, const FullWaveControl* control, double base, double epsMax, bool cut)
{
    Strips out;
    const QString& metal = feed.metal;
    const double width = feed.width, offset = feed.offset();
    double between = base - 2 * offset;
    int periods = 0;
    if (feed.pitch > 0) {
        periods = std::max(1, int(std::ceil(between / feed.pitch - 1e-9)));
        between = feed.pitch * periods;
    } else if (!(between > 0)) {
        between = base;
    }
    out.between = between;
    out.strip = 2 * offset + between;
    // The third: where the shorter is a whole number of half waves, the
    // longer is too, and the third is not - but with rows of vias it is a
    // whole number of their periods, never the shorter's or the longer's.
    if (feed.pitch > 0) {
        int m = int(std::lround(kPhi * periods));
        if (m == periods || m == 2 * periods) m = 3 * periods;
        out.third = feed.pitch * m;
    } else {
        out.third = kPhi * between;
    }
    const double c0 = 1.0 / std::sqrt(kMu0 * kEps0);
    // A frequency low enough that the shorter strip is a few degrees long,
    // where its phase has no turn to miss: the others are followed up from
    // it.
    out.anchor = 0.05 * c0 / (out.strip * micrometre * std::sqrt(epsMax));
    out.at = frequencies;
    std::sort(out.at.begin(), out.at.end());
    out.at.erase(std::unique(out.at.begin(), out.at.end()), out.at.end());
    out.anchored = out.anchor < out.at.first();
    if (out.anchored) out.at.prepend(out.anchor);

    // The strips, a port at each end facing out, meshed as the structure
    // was so that the cells at a port are the cells at its own: a coplanar
    // feed's grounds beside the signal as at the port, and their vias as far
    // in from each end as the feed's first, a period apart. The longest
    // first, which is the one too large if any is.
    static_assert(kStripsPerLength == 3, "the strips here are the shorter, the longer and the third");
    const double lines[kStripsPerLength] = {between, 2 * between, out.third};
    QList<Chain> chains[kStripsPerLength];
    for (const int i : {1, 2, 0}) {
        const double l = 2 * offset + lines[i];
        QList<Conductor> parts;
        const auto rect = [&parts](const QString& layer, double x0, double y0, double x1, double y1) {
            Conductor c;
            c.layer = layer;
            c.outline = QPolygonF(QRectF(QPointF(x0, y0), QPointF(x1, y1)));
            parts << c;
        };
        rect(metal, 0, 0, l, width);
        if (feed.coplanar) {
            if (feed.ground[0] > 0) rect(metal, 0, -feed.gap[0] - feed.ground[0], l, -feed.gap[0]);
            if (feed.ground[1] > 0) rect(metal, 0, width + feed.gap[1], l, width + feed.gap[1] + feed.ground[1]);
            for (const Feed::Via& v : feed.vias) {
                if (v.wall) {
                    rect(v.layer, v.in0, v.across0, l - v.in0, v.across1);
                    continue;
                }
                const double centre = 0.5 * (v.in0 + v.in1), half = 0.5 * (v.in1 - v.in0);
                for (double c = centre; c <= l - centre + 1e-6 * l; c += feed.pitch)
                    rect(v.layer, c - half, v.across0, c + half, v.across1);
            }
        }
        const Mesh mesh = meshOf(parts, how, stack, top, micrometre);
        out.facing = std::max(out.facing, mesh.facing);
        if (!mesh.problems.isEmpty() || mesh.cells.isEmpty()) {
            out.problem = tr("A calibration strip of %1 by %2 µm on %3 could not be meshed: %4")
                              .arg(l, 0, 'g', 4)
                              .arg(width, 0, 'g', 4)
                              .arg(metal, mesh.problems.join(QLatin1Char(' ')));
            return out;
        }
        FullWavePort near, far;
        near.name = QStringLiteral("A");
        near.at = QPointF(0, width / 2);
        near.metal = metal;
        near.facing = QPointF(-1, 0);
        near.coplanar = feed.coplanar;
        far.name = QStringLiteral("B");
        far.at = QPointF(l, width / 2);
        far.metal = metal;
        far.facing = QPointF(1, 0);
        far.coplanar = feed.coplanar;
        const FullWave fw = fullWaveOf(mesh, parts, stack, {near, far}, out.at, micrometre, control);
        if (fw.tooLarge && cut) {
            out.tooLarge = true;
            return out;
        }
        if (!fw.ok()) {
            out.problem = tr("A calibration strip %1 µm long would not solve: %2")
                              .arg(l, 0, 'g', 4)
                              .arg(fw.problems.join(QLatin1Char(' ')));
            return out;
        }
        for (const QList<QList<Complex>>& z : fw.z) chains[i] << chainOf(z);
        // (What makes a strip's answer doubtful is the calibration's too:
        //  a strip's GMRES short of its residual was taken as solved, unsaid.)
        for (const QString& w : fw.warnings) out.warnings << tr("A calibration strip %1 µm long: %2").arg(l, 0, 'g', 4).arg(w);
    }

    // At each frequency, up from the lowest: E from E E' = T_l T_2l^-1 T_l,
    // its D one; the line from L(l) = E^-1 T_l E'^-1, and its phase followed
    // from the frequency before - a sweep's step can be many turns of a long
    // strip, but not of the change in its effective permittivity.
    const double length = between * micrometre, third = out.third * micrometre;
    double beta = 0, previous = 0;   // (beta l at the frequency before, and that frequency)
    for (int k = 0; k < out.at.size(); ++k) {
        Chain t[3] = {chains[0].at(k), chains[1].at(k), chains[2].at(k)};
        // (Each strip is symmetric: its two ends' terms averaged.)
        for (Chain& each : t) {
            const Complex mean = 0.5 * (each[0] + each[3]);
            each[0] = each[3] = mean;
        }
        const Chain m = times(times(t[0], inverse(t[1])), t[0]);
        const Complex m11 = 0.5 * (m[0] + m[3]);
        const Chain e = {(m11 + 1.0) / 2.0, m[1] / (m11 + 1.0), m[2] / 2.0, Complex(1, 0)};
        const Chain l1 = times(times(inverse(e), t[0]), inverse(reversed(e)));
        const Chain l3 = times(times(inverse(e), t[2]), inverse(reversed(e)));
        const auto impedanceOf = [](const Chain& line) {
            Complex z = std::sqrt(line[1] / line[2]);
            return z.real() < 0 ? -z : z;
        };
        const Complex z1 = impedanceOf(l1), z3 = impedanceOf(l3);
        const Complex c = 0.5 * (l1[0] + l1[3]), s = 0.5 * (l1[2] * z1 + l1[1] / z1);
        Complex wave = std::log(c + s);   // gamma l
        if (k > 0) {
            const double predicted = beta * out.at.at(k) / previous;
            wave += Complex(0, 2 * M_PI * std::round((predicted - wave.imag()) / (2 * M_PI)));
        }
        beta = wave.imag();
        previous = out.at.at(k);
        const Complex gamma = wave / length;
        // (Where the shorter strip is within a quarter turn of a whole
        //  number of half waves its chain matrix is all but the identity,
        //  and the impedance is found poorly from it: from the third there,
        //  where that is the further from one. Elsewhere the shorter's, as
        //  the line it is of and the ends its E was found at.)
        const double turns = wave.imag() / M_PI;
        const bool nearHalf = turns > 0.75 && std::abs(turns - std::round(turns)) < 0.25;
        const Complex z = nearHalf && std::abs(std::sinh(gamma * third)) > std::abs(std::sinh(wave)) ? z3 : z1;
        out.residual << std::max(apart(l1, lineOf(gamma, z, length), z), apart(l3, lineOf(gamma, z, third), z));
        if (turns > 0.9 && std::abs(turns - std::round(turns)) < 0.1) out.halfWaves << out.at.at(k);
        // (A line gives nothing back. Two strips read a little of what the
        //  ports radiate to one another as the line's loss or gain - a few
        //  parts in 1e3 of a strip, inside the de-embedding's own error - but
        //  where the substrate's surface wave carries it, it falls off too
        //  slowly for any strip to put the ports apart: 254 µm of alumina from
        //  20 GHz gave back a percent and more.)
        if (wave.real() < -5e-3) out.gains << out.at.at(k);
        // (E P ends at the feed's first via: the line taken back from there
        //  to the face, so that the reference plane is at the face.)
        const Complex lossless(std::max(0.0, wave.real()) / length, wave.imag() / length);
        out.error << (offset > 0 ? times(e, lineOf(lossless, z, -offset * micrometre)) : e);
        out.gamma << gamma;
        out.impedance << z;
    }
    return out;
}

} // namespace

int Calibration::indexOf(double frequency) const
{
    for (int i = 0; i < frequencies.size(); ++i)
        if (std::abs(frequencies.at(i) - frequency) <= 1e-9 * std::max(1.0, std::abs(frequency))) return i;
    return -1;
}

Calibration calibrate(const Stack& stack, const MeshSettings& how, double top, const QString& metal, double width,
                      const QList<double>& frequencies, double micrometre, const FullWaveControl* control, double chosen)
{
    Feed feed;
    feed.metal = metal;
    feed.width = width;
    return calibrate(stack, how, top, feed, frequencies, micrometre, control, chosen);
}

Calibration calibrate(const Stack& stack, const MeshSettings& how, double top, const Feed& feed,
                      const QList<double>& frequencies, double micrometre, const FullWaveControl* control, double chosen)
{
    Calibration out;
    const QString& metal = feed.metal;
    const double width = feed.width;
    out.metal = metal;
    out.width = width;
    out.feed = feed;
    out.frequencies = frequencies;
    if (!(width > 0) || !std::isfinite(width)) {
        out.problems << tr("A feed of no width cannot be calibrated.");
        return out;
    }
    if (frequencies.isEmpty()) {
        out.problems << tr("No frequency to calibrate at.");
        return out;
    }
    const double h = metalHeight(stack, metal);
    if (!(h > 0)) {
        out.problems << tr("%1 is no metal over the ground: a feed on it cannot be calibrated.").arg(metal);
        return out;
    }
    if (!feed.unlike.isEmpty()) {
        out.problems << tr("Strips of its feed cannot be made like it: %1.").arg(feed.unlike);
        return out;
    }

    // The strips' length: the ports' near fields reach out over the metal's
    // height and the feed's width, and must not meet, or what is taken for
    // the port's own discontinuity would be the two ports' together. (At
    // four heights they did: a line de-embedded to a thru was 0.4 degrees
    // out at 20 GHz, and |S11| 4e-3; at eight, 0.005 degrees and 5e-4. A
    // coplanar feed's at a quarter of that read a line of 32 dB/m where it
    // lost nothing, at 60 GHz.) Where the feed has rows of vias, the line
    // between the ends' first vias is a whole number of their periods. A
    // structure's own feed must run as far as half of it - one port's
    // field - for the port there to be what its strips find.
    double epsMax = 1;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) epsMax = std::max(epsMax, l.epsr);
    // With no ground plane under the stack, a coplanar feed's field is across
    // its gaps, between its signal and the conductors beside it: the metal's
    // height over the bottom of the stack - all of a thick substrate - is
    // no reach of it. Its strips are then as long as eight of its signal
    // and gaps and twice its width, as a grounded feed's are of eight
    // heights. (A feed 50 µm wide in gaps of 6 µm over 500 µm of silicon:
    // strips of 4.1, 8.3 and 6.7 mm took a 1 s solve to 102 s; from 400 µm
    // up the strips read the same line and discontinuity, within their own
    // scatter - eps_eff 4.13, 1.44 fF - at 110 GHz. Nor is a thin
    // substrate's height a bound on them: with no plane, it bounds nothing.)
    bool plane = false;
    for (const Layer& l : stack.layers) plane = plane || (l.kind == Layer::Ground && l.variant != QLatin1String("open"));
    const double span = width + feed.gap[0] + feed.gap[1];
    const double byHeight = 8 * h / micrometre + 2 * width;
    const bool byGaps = feed.coplanar && !plane && span > width;
    const double first = chosen > 0 ? chosen : byGaps ? 8 * span + 2 * width : byHeight;   // the layout's units
    if (!feed.coplanar) out.reach = 4 * h / micrometre + width;
    const double offset = feed.offset();
    // Where the strips at that length are more than a dense solve takes on
    // this mesh - a coplanar feed's three conductors at eight cells across -
    // they are cut to a half and then a quarter, and it is said: their ports
    // the nearer, the line they read the less sure.
    Strips base;
    double used = first, whole = 0;
    for (int attempt = 0;; ++attempt) {
        used = first / double(1 << attempt);
        base = solveStrips(stack, how, top, feed, frequencies, micrometre, control, used, epsMax, attempt < 2 && chosen <= 0);
        if (attempt == 0) whole = base.strip;
        if (!base.tooLarge) break;
    }
    if (!base.problem.isEmpty()) {
        out.problems << base.problem;
        return out;
    }

    // Where they do not agree on one line between two ends alike at every
    // frequency asked - unless a length was given - shorter strips too; each
    // frequency taken from the longest that agree within a percent there,
    // whose ports are the furthest apart, else from those that agree best.
    QList<Strips> sets{base};
    const auto best = [&sets](double f) {
        int b = 0;
        for (int i = 0; i < sets.size(); ++i) {
            const double r = sets.at(i).residual.at(sets.at(i).indexOf(f));
            if (r <= kStripsAgree) return i;
            if (r < sets.at(b).residual.at(sets.at(b).indexOf(f))) b = i;
        }
        return b;
    };
    for (int halving = 1; halving <= 3 && chosen <= 0; ++halving) {
        bool all = true;
        for (const double f : frequencies) {
            const Strips& s = sets.at(best(f));
            all = all && s.residual.at(s.indexOf(f)) <= kStripsAgree;
        }
        if (all) break;
        if (control != nullptr && control->stop != nullptr && control->stop->load()) break;
        Strips shorter = solveStrips(stack, how, top, feed, frequencies, micrometre, control, used / double(1 << halving), epsMax, false);
        if (!shorter.problem.isEmpty() || !(shorter.between < sets.last().between)) break;
        sets << shorter;
    }
    double shorterFrom = 0;   // Hz: the lowest frequency taken from shorter strips
    for (const double f : frequencies) {
        const int b = best(f);
        const Strips& s = sets.at(b);
        const int k = s.indexOf(f);
        out.discontinuity << s.error.at(k);
        out.gamma << s.gamma.at(k);
        out.impedance << s.impedance.at(k);
        out.residual << s.residual.at(k);
        out.stripAt << s.strip;
        if (b > 0 && (shorterFrom == 0 || f < shorterFrom)) shorterFrom = f;
    }
    for (const Strips& s : std::as_const(sets)) out.warnings << s.warnings;
    out.length = base.between;
    out.strip = base.strip;
    out.third = base.third;
    for (const Strips& s : std::as_const(sets)) out.tried << s.strip;
    // Up to where they agree, and where first not; the most they differ.
    QList<int> order;
    for (int i = 0; i < frequencies.size(); ++i) order << i;
    std::sort(order.begin(), order.end(), [&frequencies](int a, int b) { return frequencies.at(a) < frequencies.at(b); });
    double offFrom = 0;
    for (const int i : std::as_const(order)) {
        if (out.residual.at(i) > kStripsAgree) {
            offFrom = frequencies.at(i);
            break;
        }
        out.agreedTo = frequencies.at(i);
    }
    for (int i = 0; i < frequencies.size(); ++i)
        if (out.residual.at(i) > out.worst) {
            out.worst = out.residual.at(i);
            out.worstAt = frequencies.at(i);
        }
    double baseAgreed = 0;
    for (const int i : std::as_const(order)) {
        const double f = frequencies.at(i);
        if (base.residual.at(base.indexOf(f)) > kStripsAgree) break;
        baseAgreed = f;
    }

    const int last = int(order.last());
    const double w = 2 * M_PI * frequencies.at(last);
    const double c0 = 1.0 / std::sqrt(kMu0 * kEps0);
    const QString following = base.anchored ? tr(" and at %1 MHz to follow the phase up from").arg(base.anchor / 1e6, 0, 'g', 3)
                                            : QString();
    const double thirdStrip = 2 * offset + base.third;
    if (!feed.coplanar) {
        out.lines << tr("Three strips of the feed, %1 µm wide on %2, %3, %4 and %5 µm long - the third's line %6 times the "
                        "shorter's, to check the other two by - solved at the %7 frequency(ies)%8.")
                         .arg(width, 0, 'g', 4)
                         .arg(metal)
                         .arg(base.strip, 0, 'g', 4)
                         .arg(base.strip + base.between, 0, 'g', 4)
                         .arg(thirdStrip, 0, 'g', 4)
                         .arg(base.third / base.between, 0, 'g', 4)
                         .arg(frequencies.size())
                         .arg(following);
    } else {
        QStringList beside;
        for (int k = 0; k < 2; ++k)
            if (feed.ground[k] > 0)
                beside << tr("%1 µm wide %2 µm from it").arg(feed.ground[k], 0, 'g', 4).arg(feed.gap[k], 0, 'g', 4);
        int rows = 0, walls = 0;
        for (const Feed::Via& v : feed.vias) (v.wall ? walls : rows) += 1;
        QStringList what;
        if (rows > 0)
            what << tr("%1 row(s) of vias every %2 µm, the first %3 µm in from each end").arg(rows).arg(feed.pitch, 0, 'g', 4)
                        .arg(offset, 0, 'g', 4);
        if (walls > 0) what << tr("%1 wall(s) of vias").arg(walls);
        const QString vias = what.isEmpty() ? tr("no vias") : what.join(tr(" and "));
        out.lines << tr("Three strips of the coplanar feed on %1 - the signal %2 µm wide, the conductor(s) beside it %3, "
                        "%4 - %5, %6 and %7 µm long, the third's line %8 times the shorter's to check the other two by, "
                        "their ports coplanar too, solved at the %9 frequency(ies)%10.")
                         .arg(metal)
                         .arg(width, 0, 'g', 4)
                         .arg(beside.join(tr(" and ")), vias)
                         .arg(base.strip, 0, 'g', 4)
                         .arg(base.strip + base.between, 0, 'g', 4)
                         .arg(thirdStrip, 0, 'g', 4)
                         .arg(base.third / base.between, 0, 'g', 4)
                         .arg(frequencies.size())
                         .arg(following);
        if (byGaps && chosen <= 0)
            out.lines << tr("There is no ground plane under the stack: the strips as long as eight of the feed's signal and "
                            "gaps, %1 µm, and twice its width, not as eight of the metal's %2 µm over the bottom of the stack.")
                             .arg(span, 0, 'g', 4)
                             .arg(h / micrometre, 0, 'g', 4);
        // (Meshed as the structure was - its mesh's settings, narrow gaps
        //  graded as its own are - so that the cells at a port are the cells
        //  at its own.)
        if (base.facing > 0)
            out.lines << tr("The strips meshed as the structure is: %1 side(s) facing the feed's gaps, their outermost cells "
                            "halved until they are no wider than a quarter of the gap.")
                             .arg(base.facing);
        if (offset > 0)
            out.lines << tr("The port's own discontinuity found to the feed's first via, %1 µm in from its face, where "
                            "the line its rows make begins; the line taken back from there to the face.")
                             .arg(offset, 0, 'g', 4);
    }
    out.lines << tr("At %1 GHz: the line %2 Ohm, eps_eff %3, %4 dB/m; the port's own discontinuity %5 fF at its face and "
                    "%6 pH at the port%7.")
                     .arg(frequencies.at(last) / 1e9, 0, 'g', 4)
                     .arg(out.impedance.at(last).real(), 0, 'g', 4)
                     .arg(std::pow(out.gamma.at(last).imag() * c0 / w, 2), 0, 'g', 4)
                     .arg(20 / std::log(10.0) * out.gamma.at(last).real(), 0, 'g', 3)
                     .arg(out.discontinuity.at(last)[2].imag() / w * 1e15, 0, 'g', 3)
                     .arg(out.discontinuity.at(last)[1].imag() / w * 1e12, 0, 'g', 3)
                     .arg(out.residual.at(last) > kStripsAgree ? tr(" - neither to be trusted: the strips do not agree on one line there")
                                                               : QString());
    if (offFrom == 0)
        out.lines << tr("The three agree on one line between two ends alike over the sweep: the third differs from it by "
                        "%1 at most, at %2 GHz.")
                         .arg(out.worst, 0, 'g', 2)
                         .arg(out.worstAt / 1e9, 0, 'g', 4);
    else
        out.lines << tr("From %1 GHz the strips do not agree on one line between two ends alike: the third differs from the "
                        "line the other two read by up to %2, at %3 GHz - the ports at a strip's two ends reach one another "
                        "past the line, through the substrate's surface wave, and the de-embedding is not to be trusted "
                        "there.")
                         .arg(offFrom / 1e9, 0, 'g', 4)
                         .arg(out.worst, 0, 'g', 2)
                         .arg(out.worstAt / 1e9, 0, 'g', 4);
    if (sets.size() > 1) {
        QStringList lengths;
        for (int i = 1; i < sets.size(); ++i) lengths << QString::number(sets.at(i).strip, 'g', 4);
        out.lines << tr("Strips of %1 µm agreed within a percent to %2 GHz; shorter ones, %3 µm, were solved beside them, "
                        "and each frequency is taken from the longest that agree within a percent there, else from those that agree "
                        "best - the shorter from %4 GHz.")
                         .arg(base.strip, 0, 'g', 4)
                         .arg(baseAgreed / 1e9, 0, 'g', 4)
                         .arg(lengths.join(tr(" and ")))
                         .arg(shorterFrom / 1e9, 0, 'g', 4);
    }
    if (out.strip < whole)
        out.lines << tr("Strips of %1 µm, eight heights and twice the width, were more than a dense solve takes on this "
                        "mesh: they were cut to %2 µm, their ports the nearer and the line they read the less sure. A "
                        "coarser mesh takes them whole.")
                         .arg(whole, 0, 'g', 4)
                         .arg(out.strip, 0, 'g', 4);
    if (!base.halfWaves.isEmpty())
        out.lines << tr("At %1 GHz the shorter strip is a whole number of half waves long, where its line gives the "
                        "impedance poorly: the third strip's is taken there.")
                         .arg(base.halfWaves.first() / 1e9, 0, 'g', 4);
    if (!base.gains.isEmpty())
        out.lines << tr("From %1 GHz the line comes out giving back more than it is given: the strips' two ports couple "
                        "through the substrate's surface wave there, which they take for the line's own, and the "
                        "de-embedding is the less sure for it.")
                         .arg(base.gains.first() / 1e9, 0, 'g', 4);
    return out;
}

FullWave deembedded(const FullWave& solved, const QList<Deembedding>& ports)
{
    FullWave out = solved;
    if (!solved.ok()) return out;
    const int np = int(solved.portNames.size());
    bool any = false;
    for (int p = 0; p < std::min(np, int(ports.size())); ++p) any = any || ports.at(p).any();
    if (!any) return out;
    out.zSolved = solved.z;
    out.z.clear();
    out.s.clear();
    out.asymmetry = out.gain = out.gainAt = 0;
    out.deembedding = QList<PortDeembedding>(np);
    // What the structure as solved gives back at each frequency, beside what
    // de-embedded it does: a de-embedding that gives back where the
    // structure does not is the de-embedding's.
    QList<double> solvedGiven;
    int worst = -1;
    const int top = int(std::max_element(solved.frequencies.begin(), solved.frequencies.end()) - solved.frequencies.begin());
    QList<double> gave(np, 0.0);   // Np/m: the most each port's line was read as giving back
    const double c0 = 1.0 / std::sqrt(kMu0 * kEps0);
    for (int k = 0; k < solved.frequencies.size(); ++k) {
        const double f = solved.frequencies.at(k), w = 2 * M_PI * f;
        Eigen::VectorXcd a = Eigen::VectorXcd::Ones(np), b = Eigen::VectorXcd::Zero(np), c = Eigen::VectorXcd::Zero(np),
                         d = Eigen::VectorXcd::Ones(np);
        for (int p = 0; p < std::min(np, int(ports.size())); ++p) {
            const Deembedding& each = ports.at(p);
            if (!each.any()) continue;
            // Its own discontinuity, where strips of its feed found it; and
            // the line its reference plane moves along: the feed's, or one
            // given (lossless, gamma = j w sqrt(eps)/c0).
            const bool calibrated = each.calibration != nullptr && each.calibration->ok();
            const bool given = each.impedance > 0 && each.eps > 0;
            Chain e = {Complex(1, 0), Complex(0, 0), Complex(0, 0), Complex(1, 0)};
            Complex gamma(0, w * std::sqrt(std::max(each.eps, 0.0)) / c0), impedance(each.impedance, 0);
            int i = -1;
            if (calibrated) {
                i = each.calibration->indexOf(f);
                if (i < 0) {
                    out.problems << tr("%1's calibration was not solved at %2 GHz.").arg(solved.portNames.at(p)).arg(f / 1e9, 0, 'g', 6);
                    return out;
                }
                e = each.calibration->discontinuity.at(i);
                if (!given) {
                    gamma = each.calibration->gamma.at(i);
                    impedance = each.calibration->impedance.at(i);
                    // A line gives nothing back: where the strips read it as
                    // gaining - their own two ends coupling past it, which a
                    // longer pair of strips reads less of - a plane is moved
                    // along it as losing nothing (the recheck of 1 November:
                    // -2 to -6 dB/m of a perfect metal's line at 60 GHz from
                    // two lines 0.9 mm apart, -1.6 from strips 1.8 mm apart).
                    if (gamma.real() < 0) {
                        gave[p] = std::min(gave.at(p), gamma.real());
                        gamma = Complex(0, gamma.imag());
                    }
                }
            }
            const Chain total = each.shift != 0 ? times(e, lineOf(gamma, impedance, each.shift)) : e;
            a(p) = total[0];
            b(p) = total[1];
            c(p) = total[2];
            d(p) = total[3];
            if (k == top) {
                PortDeembedding& said = out.deembedding[p];
                said.done = true;
                said.calibrated = calibrated;
                said.givenLine = given;
                said.shift = each.shift;
                said.length = calibrated ? each.calibration->strip : 0;
                said.longer = calibrated ? each.calibration->strip + each.calibration->length : 0;
                said.coplanar = calibrated && each.calibration->feed.coplanar;
                said.firstVia = calibrated ? each.calibration->feed.offset() : 0;
                said.impedance = impedance;
                said.epsEff = std::pow(gamma.imag() * c0 / w, 2);
                said.alpha = gamma.real();
                said.shuntC = e[2].imag() / w;
                said.seriesL = e[1].imag() / w;
                said.feedLength = solved.feeds.value(p).length;
                if (calibrated) {
                    const Calibration& cal = *each.calibration;
                    said.agreedTo = cal.agreedTo;
                    said.worst = cal.worst;
                    said.worstAt = cal.worstAt;
                    for (int j = 0; j < cal.frequencies.size(); ++j)
                        if (cal.residual.value(j) > kStripsAgree && (said.offFrom == 0 || cal.frequencies.at(j) < said.offFrom))
                            said.offFrom = cal.frequencies.at(j);
                    said.topAgrees = cal.residual.value(i) <= kStripsAgree;
                    said.strips = cal.tried;
                    said.third = 2 * cal.feed.offset() + cal.third;
                    said.reach = cal.reach;
                }
            }
        }
        // In waves, each port's F its own two-port in its reference at both
        // ends: S = E11 + E12 S' (1 - E22 S')^-1 E21, so
        //     S' = (1 + X E22)^-1 X,   X = E12^-1 (S - E11) E21^-1.
        // In Z, as it was, (A - Z C)^-1 (Z D - B) is no number where the
        // two planes meet and what is between them is a thru - which has no
        // Z: a line de-embedded half way from each end came out 59 % active
        // (the recheck of 1 November, its own thru meshed as its strips).
        const MatrixC measured = matrixOf(solved.s.at(k));
        Eigen::VectorXcd e11(np), e12(np), e21(np), e22(np);
        for (int p = 0; p < np; ++p) {
            const double r = out.references.at(p);
            const Complex den = a(p) + b(p) / r + c(p) * r + d(p);
            e11(p) = (a(p) + b(p) / r - c(p) * r - d(p)) / den;
            e12(p) = 2.0 * (a(p) * d(p) - b(p) * c(p)) / den;
            e21(p) = 2.0 / den;
            e22(p) = (-a(p) + b(p) / r - c(p) * r + d(p)) / den;
        }
        MatrixC x = measured;
        x.diagonal() -= e11;
        x = e12.cwiseInverse().asDiagonal() * x * e21.cwiseInverse().asDiagonal();
        MatrixC grown = x * e22.asDiagonal();
        grown.diagonal().array() += 1.0;
        const MatrixC s = Eigen::PartialPivLU<MatrixC>(grown).solve(x);
        if (!s.allFinite()) {
            out.problems << tr("De-embedding at %1 GHz came to nothing finite.").arg(f / 1e9, 0, 'g', 6);
            return out;
        }
        out.s << rowsOf(s);
        // Z from it, R^1/2 (1 - S)^-1 (1 + S) R^1/2: none worth the name for
        // a thru, whose S stands.
        Eigen::VectorXd root(np);
        for (int p = 0; p < np; ++p) root(p) = std::sqrt(out.references.at(p));
        MatrixC minus = -s, plus = s;
        minus.diagonal().array() += 1.0;
        plus.diagonal().array() += 1.0;
        const auto scale = root.cast<Complex>().asDiagonal();
        out.z << rowsOf(scale * Eigen::PartialPivLU<MatrixC>(minus).solve(plus) * scale);
        const double largest = s.cwiseAbs().maxCoeff();
        if (largest > 0) out.asymmetry = std::max(out.asymmetry, (s - s.transpose()).cwiseAbs().maxCoeff() / largest);
        const Eigen::SelfAdjointEigenSolver<MatrixC> eig(s.adjoint() * s);
        if (eig.info() == Eigen::Success && eig.eigenvalues().maxCoeff() - 1 > out.gain) {
            out.gain = eig.eigenvalues().maxCoeff() - 1;
            out.gainAt = f;
            worst = k;
        }
        const Eigen::SelfAdjointEigenSolver<MatrixC> raw(measured.adjoint() * measured);
        solvedGiven << (raw.info() == Eigen::Success ? raw.eigenvalues().maxCoeff() - 1 : 0.0);
    }
    for (int p = 0; p < np; ++p) {
        if (gave.at(p) < 0) out.deembedding[p].gave = gave.at(p);
        const PortDeembedding& said = out.deembedding.at(p);
        if (!said.done) continue;
        const QString plane = said.shift == 0 ? tr("at its face")
                                              : tr("moved %1 µm %2 from its face").arg(std::abs(said.shift) * 1e6, 0, 'g', 4)
                                                    .arg(said.shift > 0 ? tr("in") : tr("out"));
        const QString line = tr("%1 a line of %2 Ohm, eps_eff %3")
                                 .arg(said.givenLine ? tr("along a given line,") : tr("along"))
                                 .arg(said.impedance.real(), 0, 'g', 4)
                                 .arg(said.epsEff, 0, 'g', 4);
        const QString gaining = gave.at(p) < 0 && said.shift != 0 ? tr(" - which its strips read as giving back up to %1 dB/m, their own ends "
                                                    "coupling past it, and which is moved along as losing nothing")
                                                     .arg(-20 / std::log(10.0) * gave.at(p), 0, 'g', 3)
                                               : QString();
        // (Given at the top: where its strips do not agree there, said so.)
        const QString untrusted = said.calibrated && !said.topAgrees
                                      ? tr(" - %1 not to be trusted at %2 GHz: its strips do not agree on one line from %3 GHz")
                                            .arg(said.givenLine || said.shift == 0 ? tr("its own discontinuity") : tr("its own discontinuity and the line"))
                                            .arg(solved.frequencies.at(top) / 1e9, 0, 'g', 4)
                                            .arg(said.offFrom / 1e9, 0, 'g', 4)
                                      : QString();
        if (said.calibrated)
            out.lines << tr("%1 de-embedded: its own discontinuity, %2 fF at its face and %3 pH at the port at %4 GHz, taken "
                            "out by two strips of its %5feed, %6 and %7 µm long; its reference plane %8%9%10%11.")
                             .arg(solved.portNames.at(p))
                             .arg(said.shuntC * 1e15, 0, 'g', 3)
                             .arg(said.seriesL * 1e12, 0, 'g', 3)
                             .arg(solved.frequencies.at(top) / 1e9, 0, 'g', 4)
                             .arg(said.coplanar ? tr("coplanar ") : QString())
                             .arg(said.length, 0, 'g', 4)
                             .arg(said.longer, 0, 'g', 4)
                             .arg(plane, said.shift == 0 ? QString() : QStringLiteral(" ") + line, gaining)
                             .arg(untrusted);
        else
            out.lines << tr("%1's reference plane %2 %3; its own discontinuity left in, as solved.")
                             .arg(solved.portNames.at(p), plane, line);
    }
    // What makes it not to be trusted, said aloud: strips that do not agree
    // on one line, a feed shorter than the port's own field reaches - the
    // MOM capacitor of the phase E check, its feeds 50 µm long over a ground
    // 305 µm down - and a de-embedding that gives back where the structure
    // as solved does not.
    for (int p = 0; p < std::min(np, int(ports.size())); ++p) {
        const PortDeembedding& said = out.deembedding.at(p);
        const Calibration* cal = ports.at(p).calibration;
        if (!said.done || !said.calibrated || cal == nullptr) continue;
        if (said.offFrom > 0)
            out.warnings << tr("%1's calibration strips do not agree on one line from %2 GHz - the third differs from the "
                               "line the other two read by up to %3, at %4 GHz: its de-embedding is not to be trusted there.")
                                .arg(solved.portNames.at(p))
                                .arg(said.offFrom / 1e9, 0, 'g', 4)
                                .arg(said.worst, 0, 'g', 2)
                                .arg(said.worstAt / 1e9, 0, 'g', 4);
        if (said.reach > 0 && said.feedLength >= 0 && said.feedLength < said.reach)
            out.warnings << tr("%1's feed is as wide as its face and alone for %2 µm in from it, and the port's own field "
                               "reaches some %3 µm along it - four heights over the ground and its width: its own "
                               "discontinuity there is not the one its strips find, and its de-embedding is not to be "
                               "trusted at any frequency. A feed as long, or a port that returns beside it, keeps them apart.")
                                .arg(solved.portNames.at(p))
                                .arg(said.feedLength, 0, 'g', 4)
                                .arg(said.reach, 0, 'g', 4);
    }
    if (out.gain > 1e-6 && worst >= 0 && solvedGiven.value(worst) < 0.5 * out.gain)
        out.warnings << tr("De-embedded, it gives back %1 % more than it is given at %2 GHz, where as solved it gives back "
                           "%3: that is the de-embedding's, not the structure's.")
                            .arg(100 * out.gain, 0, 'g', 3)
                            .arg(out.gainAt / 1e9, 0, 'g', 4)
                            .arg(solvedGiven.value(worst) > 1e-6 ? tr("%1 %").arg(100 * solvedGiven.value(worst), 0, 'g', 3)
                                                                   : tr("nothing"));
    out.lines << out.warnings.mid(solved.warnings.size());
    return out;
}

} // namespace qucs_s::rf25
