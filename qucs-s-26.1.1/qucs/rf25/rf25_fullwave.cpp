/*
 * rf25_fullwave.cpp - the full-wave solve of an RF 2.5D model: the current
 *                     on its metals at each frequency, and what its ports see
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_fullwave.h"
#include "rf25_box.h"
#include "rf25_edge.h"

#include "rf25_dense.h"
#include "rf25_fast.h"

#include "multiphysics/fem_sparse.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QRectF>
#include <QSet>
#include <QThread>

#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <thread>
#include <tuple>
#include <vector>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

constexpr double kMu0 = 4e-7 * M_PI;
const Complex kJ(0, 1);
/// Beyond this many runs and cells together the dense complex solve is not
/// set up: the inductance's bound - some 750 MB there (two static inductance
/// matrices, the system and its LU) and, measured at 5588, 8 s a frequency
/// on a desktop of 2026. It was 3000 (0.8 s at 2858) until a metal could be
/// solved as its two faces, which doubles its cells: a millimetre of
/// coplanar line eight cells across was more than that so (the recheck of
/// 75ff6f9). Accelerate's LU (rf25_dense.h) made the dense solve some
/// twice as quick a frequency, but not the bound worth raising by default:
/// past it the compressed solve was the quicker of the two on lines of 7000
/// to 13 000 runs and cells, 7 to 17 times on microstrip (28 to 31 GMRES
/// iterations), and on a coplanar line of narrow gaps the two about even
/// (180 to 200), S the same to 1e-3 dB. RF 2.5D > Settings… sets another.
constexpr int kMostUnknowns = 6000;
/// The compressed solve (rf25_fast.h). Each low-rank block to a part in
/// 1e5 of its own size; clusters of up to kLeaf unknowns; a pair of
/// clusters low rank where the smaller is no wider than kEta times their
/// distance. Measured against the dense solve on lines, a tee, two metals
/// and a via, a coplanar line's fence of vias, a GSSG with no plane and a
/// pad on 144 vias, from 10 MHz to 60 GHz: S within 2.6e-7 of it, and 1.8e-6
/// on the fence's line 1.5 wavelengths long at 60 GHz (a part in 1e6, 3e-8
/// and 3e-7; eta 2 a tenth less memory and five times the error). Its
/// preconditioner the near field's sparse LU, of the pairs whose boxes are
/// within kNear of the larger's longest side in the plane (the cells that
/// touch and the next beyond them, and the faces of a metal solved as its
/// two). At half a side, the cells touching alone, GMRES lost its hold as
/// the mesh was refined - a MOM comb of 1 µm fingers 38 iterations at six
/// across, 108 at eight and none converging at ten or twelve (a residual of
/// 3.4e-7 after 3000), its thick-metal twin no frequency in 900 s -: at a
/// side and a half, 23, 24, 26 and 28, the thick one 96-121, S the same to
/// every digit the answer gives (B3 of the open items of 8 November; one
/// side 52-69 at twelve across, two the six-across comb four times slower). GMRES to a residual
/// of kResidual, kRestart vectors at a time, kMostIterations in all; the
/// ports' Z then from the currents variationally, its error the residual's
/// square over what the system is near singular by - which at a low
/// frequency is much: the pad on its vias at 100 MHz was 1e-3 out of the
/// dense solve's S at a residual of 1e-6, and 2e-10 at 1e-10 (some 1.7
/// times the iterations). Beyond kMostCompressed unknowns nothing is set
/// up, whatever the memory.
constexpr double kTolerance = 1e-5;
constexpr int kLeaf = 32;
constexpr double kEta = 1.5;
constexpr double kNear = 1.5;
constexpr double kResidual = 1e-10;
constexpr int kRestart = 100;
constexpr int kMostIterations = 3000;
/// The charges' term over the currents' past which a dense solve keeps the
/// charges as unknowns of their own (AtFrequency::augmented). Measured on a
/// square copper loop solved both ways: the eliminated system some 5e-18
/// of it off the other in L - 5e-8 at 3.6e9, 1.3e-6 at 2.3e11, 4e-5 at
/// 7.4e12, 2.5 % at 2.4e15 -, so a decade inside where they part by a
/// part in a million; below it the eliminated system's symmetric factors,
/// the cheaper, are kept.
constexpr double kAugmentedFrom = 1e10;
/// The iterations a compressed solve tries the charges as unknowns of their
/// own for, before it solves with them eliminated. In the loop-tree basis
/// (LoopTree) a centimetre loop at 10 Hz converged in 4 and an open line at
/// 1 Hz in 11 - 4 and 6 since its near field is factorized as that basis's
/// blocks, and a line whose conductors all float in 45 at every frequency,
/// where it was lost at 10 kHz -; in the runs' own an open line, its cells'
/// potentials 10^11 V against milliohms, took all 3,000 at 100 kHz and did
/// not converge below (step 5's follow-ups) - which this stands guard
/// against.
constexpr int kAugmentedMost = 300;
/// A higher frequency's near-field blocks precondition a lower one's
/// (NearBlocks) till a frequency on them takes more iterations past theirs
/// there than kReuseSlack - GMRES's own scatter from one frequency to the
/// next, fresh blocks or kept - and than kReuseShare of theirs: then the
/// next factorizes its own. Of iterations alone, never of a clock, so that
/// a sweep is the same to the bit from one run to the next.
constexpr double kReuseShare = 0.15;
constexpr int kReuseSlack = 2;
/// An ended layer's correction to P (rf25_dielectric.h) held dense up to
/// this many cells, as its factors past them.
constexpr int kMostDenseCorrection = 5000;
std::atomic_int g_mostDenseCorrection{0};
constexpr int kMostCompressed = 2000000;
/// GMRES short of kResidual: the residual below which its answer still
/// stands, said doubtful (solveCompressed()); and the share of
/// kMostIterations past which it is said to be near its limit. Of the
/// eliminated system alone, whose ports' Z is variational, its error the
/// residual's square: with the charges as unknowns of their own the Z is
/// J^T V + Z_paths - e^T I, its error the residual's own, and an answer
/// short of kResidual there is never kept - it falls back to the charges
/// eliminated (kAugmentedMost).
constexpr double kTrustedResidual = 1e-4;
/// A dense sweep's systems factorized as L D L^T (rf25_dense.h) where the
/// machine solves this many frequencies or more at once: there half an
/// LU's arithmetic is the quicker, 1.2 times at 12 and 1.6 at 24 at 3084
/// rows; fewer, the LU's own blocking over the BLAS is.
constexpr int kSymmetricFrom = 12;
constexpr double kNearMiss = 0.8;

/// The rank a low-rank block is guessed at before it is filled, for the
/// memory a solve will take: a guess, and said as one.
constexpr double kGuessRank = 16;
/// How small a charged cell must be beside the distance the stack's
/// remainder changes over - the distance itself, or the remainder's reach,
/// whichever is the longer - for its charge to be taken at its centre: its
/// longest side squared over that distance squared, which is twelve times
/// the most the centre is out by, here a part in 1e4. Larger, the remainder
/// is integrated over the cell. (Judged by the square root of its area, as
/// phase B judged its own near pairs until this round, a cell four times as
/// long as it is wide was taken at its centre where the centre was a part
/// in a hundred out.)
constexpr double kCentreBound = 1.2e-3;
/// The most Gauss points along a side of a cell, for the remainder.
constexpr int kMostGauss = 8;

/// Gauss-Legendre's nodes and weights on [-1/2, 1/2], the weights summing
/// to one: \a n points, 1 to kMostGauss, by Newton's method on Legendre's
/// polynomial once and kept.
struct Rule {
    QList<double> at, weight;
};

const Rule& gaussRule(int n)
{
    static const QList<Rule> rules = [] {
        QList<Rule> all;
        all << Rule{};   // (none of order nought)
        for (int order = 1; order <= kMostGauss; ++order) {
            Rule r;
            for (int i = 0; i < order; ++i) {
                double x = std::cos(M_PI * (i + 0.75) / (order + 0.5));
                double p = 0, dp = 1;
                for (int pass = 0; pass < 100; ++pass) {
                    double p0 = 1, p1 = x;
                    for (int k = 2; k <= order; ++k) {
                        const double p2 = ((2 * k - 1) * x * p1 - (k - 1) * p0) / k;
                        p0 = p1;
                        p1 = p2;
                    }
                    p = p1;
                    dp = order * (x * p1 - p0) / (x * x - 1);
                    const double step = p / dp;
                    x -= step;
                    if (std::abs(step) < 1e-16) break;
                }
                r.at << x / 2;
                r.weight << 1.0 / ((1 - x * x) * dp * dp);   // (2/((1 - x^2) P'^2), halved)
            }
            all << r;
        }
        return all;
    }();
    return rules.at(std::clamp(n, 1, kMostGauss));
}

/// How many Gauss points along a side \a length long, for a remainder that
/// changes over \a scale: two while the side is short beside it, more as
/// it is not.
int pointsAlong(double length, double scale)
{
    if (!(scale > 0)) return 2;
    return std::clamp(1 + int(std::ceil(4 * length / scale)), 2, kMostGauss);
}

/// \a v over \a jw, which is imaginary: each part divided by w. (A complex
/// division is a library call - __divdc3, scaled against overflow - and the
/// charges' A^T P A / jw made one for every term of the system: a sixth of
/// a dense frequency's fill on a filter of 3000 runs.)
inline Complex overJw(Complex v, Complex jw)
{
    const double w = jw.imag();
    return Complex(v.imag() / w, -v.real() / w);
}

/// (exp(-j k r) - 1)/r: what the retardation adds to a static 1/r, finite
/// at nought (-j k there) and smooth, so it is taken at the centres.
Complex retardation(Complex k, double r)
{
    const Complex x = -kJ * k * r;
    if (std::abs(x) < 1e-4) return -kJ * k * (1.0 + x / 2.0 + x * x / 6.0);
    return (std::exp(x) - 1.0) / r;
}

/// The stack's scalar remainder averaged over \a rect (m) at \a at by
/// Gauss's rule, \a nx by \a ny points.
Complex gaussOver(const WaveGreen& g, const QRectF& rect, const QPointF& at, int nx, int ny)
{
    const Rule& rx = gaussRule(nx);
    const Rule& ry = gaussRule(ny);
    const QPointF centre = rect.center();
    Complex sum(0, 0);
    for (int i = 0; i < rx.at.size(); ++i)
        for (int j = 0; j < ry.at.size(); ++j) {
            const double x = centre.x() + rx.at.at(i) * rect.width() - at.x();
            const double y = centre.y() + ry.at.at(j) * rect.height() - at.y();
            sum += rx.weight.at(i) * ry.weight.at(j) * g.scalarRemainder(std::hypot(x, y));
        }
    return sum;
}

/// The remainder's integral over \a rect at \a at - not its average - on
/// rectangles graded toward \a at: each one Gauss's rule takes where its
/// points resolve the remainder there, changing over the distance from \a at
/// or over \a reach, whichever is the longer; else its four quarters.
Complex gradedOver(const WaveGreen& g, const QRectF& rect, const QPointF& at, double reach, int depth)
{
    const double dx = std::max({rect.left() - at.x(), 0.0, at.x() - rect.right()});
    const double dy = std::max({rect.top() - at.y(), 0.0, at.y() - rect.bottom()});
    const double local = std::hypot(std::hypot(dx, dy), reach);
    const double longest = std::max(rect.width(), rect.height());
    const double area = rect.width() * rect.height();
    if (depth >= 40 || 1 + std::ceil(4 * longest / local) <= kMostGauss || longest <= 1e-3 * reach)
        return area * gaussOver(g, rect, at, pointsAlong(rect.width(), local), pointsAlong(rect.height(), local));
    // (Halved across its long side alone where it is long and thin.)
    const QPointF c = rect.center();
    if (rect.width() > 2 * rect.height())
        return gradedOver(g, QRectF(rect.topLeft(), QPointF(c.x(), rect.bottom())), at, reach, depth + 1)
             + gradedOver(g, QRectF(QPointF(c.x(), rect.top()), rect.bottomRight()), at, reach, depth + 1);
    if (rect.height() > 2 * rect.width())
        return gradedOver(g, QRectF(rect.topLeft(), QPointF(rect.right(), c.y())), at, reach, depth + 1)
             + gradedOver(g, QRectF(QPointF(rect.left(), c.y()), rect.bottomRight()), at, reach, depth + 1);
    Complex sum(0, 0);
    for (const QRectF& q : {QRectF(rect.topLeft(), c), QRectF(QPointF(c.x(), rect.top()), QPointF(rect.right(), c.y())),
                            QRectF(QPointF(rect.left(), c.y()), QPointF(c.x(), rect.bottom())), QRectF(c, rect.bottomRight())})
        sum += gradedOver(g, q, at, reach, depth + 1);
    return sum;
}

/// The stack's scalar remainder averaged over \a rect (m) at \a at, by
/// Gauss's rule with as many points as a remainder changing over \a scale
/// wants - or, where that is more than its rule has, on rectangles graded
/// toward \a at (gradedOver()), each of its quarters split at \a at where it
/// is over it. A thin slab over a metal - 1 nm under a MIM's 39 nm - leaves
/// a remainder that changes over nanometres at its own cells' centres, where
/// eight points a side on 0.625 µm cells took 35 % too little of it: their
/// own potentials 2 % low, and the C between the plates 11 % low.
Complex remainderOver(const WaveGreen& g, const QRectF& rect, const QPointF& at, double scale)
{
    const double longest = std::max(rect.width(), rect.height());
    if (!(scale > 0) || 1 + std::ceil(4 * longest / scale) <= kMostGauss)
        return gaussOver(g, rect, at, pointsAlong(rect.width(), scale), pointsAlong(rect.height(), scale));
    const double area = rect.width() * rect.height();
    if (!(area > 0)) return gaussOver(g, rect, at, kMostGauss, kMostGauss);
    const double reach = std::max(g.reach(), 1e-6 * longest);
    QList<double> xs{rect.left()}, ys{rect.top()};
    if (at.x() > rect.left() && at.x() < rect.right()) xs << at.x();
    if (at.y() > rect.top() && at.y() < rect.bottom()) ys << at.y();
    xs << rect.right();
    ys << rect.bottom();
    Complex sum(0, 0);
    for (int i = 0; i + 1 < xs.size(); ++i)
        for (int j = 0; j + 1 < ys.size(); ++j)
            sum += gradedOver(g, QRectF(QPointF(xs.at(i), ys.at(j)), QPointF(xs.at(i + 1), ys.at(j + 1))), at, reach, 0);
    return sum / area;
}

/// Each of a port's cells' share of the face it drives: the length of its
/// side on the face's line over all of theirs - or, a port on no face, the
/// one cell's whole.
QList<double> sharesOf(const Mesh& mesh, const QList<int>& cells, QPointF on, QPointF along, double* whole = nullptr)
{
    QList<double> lengths(cells.size(), 0.0);
    const double n = std::hypot(along.x(), along.y());
    double sum = 0;
    if (n > 0) {
        const QPointF unit = along / n, normal(-unit.y(), unit.x());
        for (int k = 0; k < cells.size(); ++k) {
            const QPolygonF& outline = mesh.cells.at(cells.at(k)).outline;
            // (Within the cuts' grid too, as faceCells() takes the cells.)
            const double tol =
                std::max(1e-6 * std::max(1.0, std::max(outline.boundingRect().width(), outline.boundingRect().height())), kBooleanGrid);
            for (int i = 0, m = int(outline.size()); i < m; ++i) {
                const QPointF a = outline.at(i), b = outline.at((i + 1) % m);
                const double offA = (a.x() - on.x()) * normal.x() + (a.y() - on.y()) * normal.y();
                const double offB = (b.x() - on.x()) * normal.x() + (b.y() - on.y()) * normal.y();
                if (std::abs(offA) > tol || std::abs(offB) > tol) continue;
                lengths[k] = std::max(lengths.at(k), std::hypot(b.x() - a.x(), b.y() - a.y()));
            }
            sum += lengths.at(k);
        }
    }
    if (whole != nullptr) *whole = sum;
    if (!(sum > 0)) return QList<double>(cells.size(), 1.0 / std::max<qsizetype>(1, cells.size()));
    for (double& l : lengths) l /= sum;
    return lengths;
}

/// The faces beside an edge port's own on its edge's line, and where they
/// are along it.
struct Beside {
    std::array<QList<int>, 2> cells;   ///< below the face's lower end, above its upper
    double lo = 0, hi = 0;             ///< the face's own ends, along the line from its point
    /// Each side's conductor along the line: its near and far ends, out
    /// from the face's own end.
    std::array<double, 2> near = {0, 0}, far = {0, 0};
    double inside = 0;                 ///< the side of the line the port's conductor is on: its sign
    /// Each side's nearest cell of a conductor that is no return, where one
    /// lies between the face and that side's return - passed over - or -1.
    std::array<int, 2> passed = {-1, -1};
    /// Whether each side's cells are of the face's own net: a shorted
    /// coplanar line's grounds, joined to its signal further on.
    std::array<bool, 2> own = {false, false};
};

/// The faces beside an edge port's own on its edge's line - what a
/// ground-signal-ground probe's grounds touch: either side of \a face, the
/// nearest run of cells of \a reached on its layer with a side along that
/// line (through \a on, along \a along) and inside on the same side of it
/// as the port's own face. A side with none is empty. Given \a netOf (a
/// cell's net), the face's own net only across a gap from the face on that
/// line - a shorted coplanar line's grounds, a loop's other end: a source
/// between two places of one conductor, apart, is no short (B7 of the open
/// items of 8 November; refused before, "there is none either side of
/// it") -, never its own cells' neighbours there; and given \a isReturn (a net's)
/// with a return among them, only the returns' cells, a conductor that is
/// no return passed over - a GSSG's other signal - and said (Beside::passed).
Beside besideFaces(const Mesh& mesh, const QList<int>& reached, const QList<int>& face, QPointF on, QPointF along,
                   const QHash<int, int>* netOf = nullptr, const QList<bool>* isReturn = nullptr)
{
    Beside out;
    const double norm = std::hypot(along.x(), along.y());
    if (face.isEmpty() || !(norm > 0)) return out;
    const QPointF unit = along / norm, normal(-unit.y(), unit.x());
    const QString layer = mesh.cells.at(face.first()).layer;
    QRectF all;
    for (const int c : reached) all = all.united(mesh.cells.at(c).outline.boundingRect());
    const double tol = std::max(1e-6 * std::max(1.0, std::max(all.width(), all.height())), kBooleanGrid);   // (faceCells()'s)
    // A cell's side along the line, as from and to along it.
    const auto sideOn = [&mesh, on, unit, normal, tol](int c, double* t0, double* t1) {
        const QPolygonF& outline = mesh.cells.at(c).outline;
        for (int i = 0, m = int(outline.size()); i < m; ++i) {
            const QPointF a = outline.at(i) - on, b = outline.at((i + 1) % m) - on;
            if (std::abs(a.x() * normal.x() + a.y() * normal.y()) > tol || std::abs(b.x() * normal.x() + b.y() * normal.y()) > tol)
                continue;
            const double ta = a.x() * unit.x() + a.y() * unit.y(), tb = b.x() * unit.x() + b.y() * unit.y();
            *t0 = std::min(ta, tb);
            *t1 = std::max(ta, tb);
            return true;
        }
        return false;
    };
    // The port's own face: how far along the line it reaches, and which
    // side of the line its conductor is on.
    double lo = 0, hi = 0, inside = 0;
    bool any = false;
    for (const int c : face) {
        double t0 = 0, t1 = 0;
        if (!sideOn(c, &t0, &t1)) continue;
        lo = any ? std::min(lo, t0) : t0;
        hi = any ? std::max(hi, t1) : t1;
        any = true;
        const QPointF d = mesh.cells.at(c).centre - on;
        inside += d.x() * normal.x() + d.y() * normal.y();
    }
    if (!any) return out;
    out.lo = lo;
    out.hi = hi;
    out.inside = inside;
    QList<std::tuple<double, double, int>> below, above;   // (from the face out, the far end, the cell)
    const int own = netOf != nullptr ? netOf->value(face.first(), -1) : -1;
    // How far the face's own conductor runs on along the line from it with
    // no gap - cells of the same net, side by side with the face: none of
    // those is a return, only its own net's cells past a gap.
    double ownLo = lo, ownHi = hi;
    if (own >= 0) {
        QList<std::pair<double, double>> spans;
        for (const int c : reached) {
            if (mesh.cells.at(c).layer != layer || face.contains(c) || netOf->value(c, -1) != own) continue;
            double t0 = 0, t1 = 0;
            if (!sideOn(c, &t0, &t1)) continue;
            const QPointF d = mesh.cells.at(c).centre - on;
            if ((d.x() * normal.x() + d.y() * normal.y()) * inside <= 0) continue;
            spans << std::make_pair(t0, t1);
        }
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [t0, t1] : std::as_const(spans))
                if (t1 >= ownLo - tol && t0 <= ownHi + tol && (t0 < ownLo - tol || t1 > ownHi + tol)) {
                    ownLo = std::min(ownLo, t0);
                    ownHi = std::max(ownHi, t1);
                    grew = true;
                }
        }
    }
    const bool returns = isReturn != nullptr && isReturn->contains(true);
    std::array<std::pair<double, int>, 2> other = {std::make_pair(0.0, -1), std::make_pair(0.0, -1)};
    for (const int c : reached) {
        if (mesh.cells.at(c).layer != layer || face.contains(c)) continue;
        const int net = netOf != nullptr ? netOf->value(c, -1) : -1;
        double t0 = 0, t1 = 0;
        if (!sideOn(c, &t0, &t1)) continue;
        const QPointF d = mesh.cells.at(c).centre - on;
        if ((d.x() * normal.x() + d.y() * normal.y()) * inside <= 0) continue;
        const bool lower = t1 <= lo + tol;
        if (!lower && !(t0 >= hi - tol)) continue;
        const bool ownNet = own >= 0 && net == own;
        if (ownNet && t1 >= ownLo - tol && t0 <= ownHi + tol) continue;   // (the face's own conductor, running on)
        if (returns && !ownNet && !isReturn->value(net, false)) {
            std::pair<double, int>& o = other[lower ? 0 : 1];
            const double near = lower ? lo - t1 : t0 - hi;
            if (o.second < 0 || near < o.first) o = std::make_pair(near, c);
            continue;
        }
        if (lower) below << std::make_tuple(lo - t1, lo - t0, c);
        else above << std::make_tuple(t0 - hi, t1 - hi, c);
    }
    // Of each side, the cells from the nearest outwards, each meeting the
    // last: the nearest conductor's face, the slot between left out.
    int k = 0;
    for (QList<std::tuple<double, double, int>>* side : {&below, &above}) {
        std::sort(side->begin(), side->end());
        double reach = 0;
        for (int i = 0; i < side->size(); ++i) {
            const auto [near, far, cell] = side->at(i);
            if (i > 0 && near > reach + tol) break;
            if (i == 0) out.near[std::size_t(k)] = near;
            out.cells[std::size_t(k)] << cell;
            if (own >= 0 && netOf->value(cell, -1) == own) out.own[std::size_t(k)] = true;
            reach = std::max(reach, far);
        }
        out.far[std::size_t(k)] = reach;
        const std::pair<double, int>& o = other[std::size_t(k)];
        if (o.second >= 0 && (out.cells[std::size_t(k)].isEmpty() || o.first < out.near[std::size_t(k)]))
            out.passed[std::size_t(k)] = o.second;
        ++k;
    }
    return out;
}

/*!
 * How far a port's feed runs straight in from its face, the layout's units:
 * the face \a width wide from \a lo along \a unit from \a on, in along
 * \a inward, on \a metal of \a conductors - until its metal reaches out
 * past the face's ends (by more than \a beyond) or no longer holds the
 * face's middle, or another conductor, a metal's or a via's, lies over or
 * under it. \a window at most; looked at a quarter of the width at a time,
 * or a twentieth of the window where that is less.
 */
double feedLength(const QList<Conductor>& conductors, const QString& metal, QPointF on, QPointF unit, QPointF inward,
                  double lo, double width, double window, double beyond)
{
    if (!(width > 0) || !(window > 0)) return 0;
    QList<const QPolygonF*> own, others;
    for (const Conductor& c : conductors) (c.layer == metal ? own : others) << &c.outline;
    const auto in = [](const QList<const QPolygonF*>& set, QPointF at) {
        for (const QPolygonF* outline : set)
            if (outline->containsPoint(at, Qt::OddEvenFill)) return true;
        return false;
    };
    const double step = std::min(width / 4, window / 20);
    const int across = 16;
    for (double d = 0.5 * step; d < window; d += step) {
        const QPointF row = on + inward * d;
        const auto at = [row, unit](double t) { return row + unit * t; };
        if (!in(own, at(lo + width / 2)) || in(own, at(lo - beyond)) || in(own, at(lo + width + beyond)))
            return std::max(0.0, d - 0.5 * step);
        for (int i = 1; i < across; ++i)
            if (in(others, at(lo + width * i / across))) return std::max(0.0, d - 0.5 * step);
    }
    return window;
}

/*!
 * The layers that end, \a extents (the layout's units), as a feed \a width
 * wide stands on them - its face's lower end at \a lo along \a unit from
 * \a on, in along \a inward -, in the frame its calibration strips are laid
 * out in: x in from the face, y across from that lower end. Each layer's
 * shapes are cut across the feed \a depth in from the face, and each piece
 * across it laid along the strips from \a reach before the face to \a reach
 * past it - the strips are a line, the feed as it is there all along them.
 * A layer with no shape across the feed is there as it is beyond its
 * shapes, and kept so: with no shapes.
 */
QList<Extent> feedExtents(const QList<Extent>& extents, QPointF on, QPointF unit, QPointF inward, double lo, double width,
                          double depth, double reach)
{
    QList<Extent> out;
    // (A strip across the feed, a hair deep, as far out either side as
    //  the strips' own reach.)
    const double hair = 1e-4 * width;
    const QPointF at = on + inward * depth;
    const QPolygonF cut({at + unit * (lo - reach) - inward * hair, at + unit * (lo + width + reach) - inward * hair,
                         at + unit * (lo + width + reach) + inward * hair, at + unit * (lo - reach) + inward * hair});
    for (const Extent& e : extents) {
        QList<std::pair<double, double>> spans;
        for (const QPolygonF& shape : e.shapes) {
            const QPolygonF piece = shape.intersected(cut);
            // (Its closed rings one after another, each ending at its own
            //  first corner.)
            int first = 0;
            double a = 0, b = 0;
            for (int i = 0; i < piece.size(); ++i) {
                const QPointF d = piece.at(i) - on;
                const double across = d.x() * unit.x() + d.y() * unit.y() - lo;
                a = i == first ? across : std::min(a, across);
                b = i == first ? across : std::max(b, across);
                if (i > first && piece.at(i) == piece.at(first)) {
                    if (b - a > hair) spans << std::make_pair(a, b);
                    first = i + 1;
                }
            }
            if (first < piece.size() && b - a > hair) spans << std::make_pair(a, b);
        }
        std::sort(spans.begin(), spans.end());
        Extent strip{e.layer, {}, e.beyond};
        for (const auto& [a, b] : spans) {
            if (!strip.shapes.isEmpty()) {
                const QRectF last = strip.shapes.last().boundingRect();
                if (a <= last.bottom() + hair) {
                    strip.shapes.last() = QPolygonF(QRectF(QPointF(-reach, last.top()), QPointF(reach, std::max(b, last.bottom()))));
                    continue;
                }
            }
            strip.shapes << QPolygonF(QRectF(QPointF(-reach, a), QPointF(reach, b)));
        }
        out << strip;
    }
    return out;
}

/*!
 * The vias down a coplanar feed's grounds, into \a feed: of \a conductors
 * on the stack's vias that land on its metal, those over a ground beside
 * the face and within \a window (the layout's units) of it, in the port's
 * frame - across from the face's lower end at \a lo along \a unit from
 * \a on, in along \a inward. Then whether they are rows of one period, or
 * walls, laid out so that strips of the feed can have them at both of
 * their ends alike; where not, \a feed says why.
 */
void feedVias(Feed* feed, const QList<Conductor>& conductors, const Stack& stack, QPointF on, QPointF unit, QPointF inward,
              double lo, double window)
{
    const Layer* metal = stack.named(feed->metal);
    if (metal == nullptr) return;
    const double tol = 1e-6 * std::max(1.0, window);
    const double w = feed->width;
    // Each via over the feed, in the frame.
    QList<Feed::Via> found;
    for (const Conductor& c : conductors) {
        const Layer* via = stack.named(c.layer);
        if (via == nullptr || via->kind != Layer::Via) continue;
        if (stack.named(via->from) != metal && stack.named(via->to) != metal) continue;
        const QRectF box = c.outline.boundingRect();
        Feed::Via v;
        v.layer = c.layer;
        bool first = true;
        for (const QPointF& corner : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()}) {
            const QPointF d = corner - on;
            const double across = d.x() * unit.x() + d.y() * unit.y() - lo, in = d.x() * inward.x() + d.y() * inward.y();
            v.across0 = first ? across : std::min(v.across0, across);
            v.across1 = first ? across : std::max(v.across1, across);
            v.in0 = first ? in : std::min(v.in0, in);
            v.in1 = first ? in : std::max(v.in1, in);
            first = false;
        }
        if (!(v.in1 > tol) || !(v.in0 < window)) continue;
        v.in0 = std::max(v.in0, 0.0);
        const double middle = 0.5 * (v.across0 + v.across1);
        if (middle > 0 && middle < w) {
            feed->unlike = QCoreApplication::translate("qucs_s::rf25", "a via stands on its own conductor %1 µm in from its "
                                                                      "face, so its feed is no line there")
                               .arg(0.5 * (v.in0 + v.in1), 0, 'g', 4);
            return;
        }
        const bool below = feed->ground[0] > 0 && middle <= -feed->gap[0] && middle >= -feed->gap[0] - feed->ground[0];
        const bool above = feed->ground[1] > 0 && middle >= w + feed->gap[1] && middle <= w + feed->gap[1] + feed->ground[1];
        if (below || above) found << v;
    }
    if (found.isEmpty()) return;
    // In rows: the same layer and the same place across, nearest first -
    // and level ones by their place across, so that both ends of a line
    // find a fence's two rows in one order.
    std::sort(found.begin(), found.end(), [](const Feed::Via& a, const Feed::Via& b) {
        return a.in0 != b.in0 ? a.in0 < b.in0 : a.across0 < b.across0;
    });
    QList<QList<Feed::Via>> rows;
    for (const Feed::Via& v : std::as_const(found)) {
        bool placed = false;
        for (QList<Feed::Via>& row : rows)
            if (row.first().layer == v.layer && std::abs(row.first().across0 - v.across0) <= tol
                && std::abs(row.first().across1 - v.across1) <= tol) {
                row << v;
                placed = true;
                break;
            }
        if (!placed) rows << QList<Feed::Via>{v};
    }
    const auto say = [feed](const QString& why) {
        feed->unlike = why;
        feed->vias.clear();
        feed->pitch = 0;
    };
    for (const QList<Feed::Via>& row : std::as_const(rows)) {
        // A via far longer down the feed than across it is a wall, taken
        // to run as far as the feed does.
        Feed::Via first = row.first();
        if (first.in1 - first.in0 >= 3 * (first.across1 - first.across0)) {
            first.wall = true;
            feed->vias << first;
            continue;
        }
        if (row.size() < 2) {
            say(QCoreApplication::translate("qucs_s::rf25", "only one via of a row down its grounds is within %1 µm of its "
                                                            "face, so strips of the feed cannot tell how often they come")
                    .arg(window, 0, 'g', 4));
            return;
        }
        // Its period from the first two, the feed's own as far as the rest
        // keep to it; what the structure does beyond is not the feed's.
        const double pitch = row.at(1).in0 - row.at(0).in0, length = first.in1 - first.in0;
        if (std::abs(row.at(1).in1 - row.at(1).in0 - length) > 1e-3 * pitch || !(pitch > length)) {
            say(QCoreApplication::translate("qucs_s::rf25", "the first vias down its grounds are not alike, so strips of "
                                                            "the feed cannot have them as it has"));
            return;
        }
        if (feed->pitch > 0 && std::abs(pitch - feed->pitch) > 1e-3 * pitch) {
            say(QCoreApplication::translate("qucs_s::rf25", "its grounds' rows of vias come every %1 and every %2 µm, and "
                                                            "strips of the feed have one period")
                    .arg(feed->pitch, 0, 'g', 4)
                    .arg(pitch, 0, 'g', 4));
            return;
        }
        feed->pitch = pitch;
        feed->vias << first;
    }
    // Both ends of a strip alike: each row's vias where the mirror of the
    // strip puts them - its first via's centre a whole number of half
    // periods beyond the nearest row's - and each wall begun before it.
    const double reference = feed->offset();
    for (const Feed::Via& v : std::as_const(feed->vias)) {
        if (v.wall) {
            if (v.in0 > reference + tol) {
                say(QCoreApplication::translate("qucs_s::rf25", "a wall of vias down its grounds begins beyond the rows "
                                                                "beside it, so strips of the feed cannot have both alike"));
                return;
            }
            continue;
        }
        const double halves = 2 * (0.5 * (v.in0 + v.in1) - reference) / feed->pitch;
        if (std::abs(halves - std::round(halves)) > 1e-3) {
            say(QCoreApplication::translate("qucs_s::rf25", "its grounds' rows of vias are staggered by other than half "
                                                            "their period, so strips of the feed cannot have them at both "
                                                            "ends alike"));
            return;
        }
    }
}

QRectF scaledBy(const QRectF& box, double unit)
{
    return QRectF(box.left() * unit, box.top() * unit, box.width() * unit, box.height() * unit);
}

using MatrixC = Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic>;

/// How much memory the frequencies solved side by side may take, bytes, by
/// default: a quarter of the machine's, no less than this. More at once is
/// quicker: a coplanar line's 40 frequencies held dense (6996 runs and
/// cells, some 435 MB each) 73 s three at a time in 1.5 GB, 44 s six in 3,
/// 31 s thirteen in 6 and 27 s all 24 threads in 12, the app's peak some
/// half a gigabyte over the budget each time (an M2 Ultra; docs/
/// an earlier note).
constexpr double kSweepMemory = 1.5e9;
std::atomic<int> denseLimitSet{0};
std::atomic<double> sweepMemorySet{0};

/// The cell of \a reached under \a where on \a layer (any, where it is
/// empty): the one whose outline holds the point, else the one whose
/// outline is nearest it, within the booleans' grid (kBooleanGrid) - a
/// point on the line between two cells, which neither outline may hold. -1
/// where there is none on that layer, or none so near: the nearest cell
/// however far, as it was, took an internal port between two strips onto
/// the nearer, and one whose other metal ended 400 µm short onto that end,
/// said as "the cell under it" (a bug hunt's second hour).
int cellUnder(const Mesh& mesh, const QList<int>& reached, QPointF where, const QString& layer)
{
    const auto distance = [&where](const QPolygonF& outline) {
        double least = std::numeric_limits<double>::infinity();
        for (int i = 0; i < outline.size(); ++i) {
            const QPointF a = outline.at(i), b = outline.at((i + 1) % outline.size());
            const QPointF ab = b - a, aw = where - a;
            const double along = QPointF::dotProduct(ab, ab);
            const double t = along > 0 ? std::clamp(QPointF::dotProduct(aw, ab) / along, 0.0, 1.0) : 0.0;
            const QPointF d = aw - t * ab;
            least = std::min(least, std::hypot(d.x(), d.y()));
        }
        return least;
    };
    int best = -1;
    double closest = 0;
    for (const int c : reached) {
        const MeshCell& cell = mesh.cells.at(c);
        if (!layer.isEmpty() && cell.layer != layer) continue;
        if (cell.outline.containsPoint(where, Qt::OddEvenFill)) return c;
        const double far = distance(cell.outline);
        if (best < 0 || far < closest) {
            best = c;
            closest = far;
        }
    }
    return best >= 0 && closest <= 2 * kBooleanGrid ? best : -1;
}

/// The cells of \a reached on \a layer (any, where it is empty) that
/// \a rect overlaps, each with its share of the area they overlap it by
/// together; empty where it overlaps none.
QList<std::pair<int, double>> cellsUnder(const Mesh& mesh, const QList<int>& reached, const QRectF& rect, const QString& layer)
{
    QList<std::pair<int, double>> out;
    double total = 0;
    for (const int c : reached) {
        const MeshCell& cell = mesh.cells.at(c);
        if (!layer.isEmpty() && cell.layer != layer) continue;
        const QRectF overlap = cell.outline.boundingRect().intersected(rect);
        const double area = overlap.width() * overlap.height();
        if (!(area > 0)) continue;
        out << std::make_pair(c, area);
        total += area;
    }
    for (std::pair<int, double>& each : out) each.second /= total;
    return out;
}

/// s asinh(s/rho) - sqrt(s^2 + rho^2): the second antiderivative of
/// 1/sqrt(s^2 + rho^2).
double antiderivative(double s, double rho)
{
    return s * std::asinh(s / rho) - std::sqrt(s * s + rho * rho);
}

/// Int_a0^a1 Int_b0^b1 dz dz' / sqrt(rho^2 + (z - z')^2), m: two lines
/// along z whose axes are \a rho apart, in the four terms of the
/// antiderivative - the static part of two vias' coupling.
double linePair(double a0, double a1, double b0, double b1, double rho)
{
    return antiderivative(a1 - b0, rho) - antiderivative(a1 - b1, rho) - antiderivative(a0 - b0, rho)
           + antiderivative(a0 - b1, rho);
}

/// The same between two vias of square cross-sections a side \a wa and
/// \a wb (m) whose axes are \a apart (m): for one via with itself, or with
/// its image, the cross-section's geometric mean distance with itself
/// (\a gmd); far apart, between the axes; near, averaged over both
/// cross-sections, where their breadth is most of the answer.
double viaPair(double a0, double a1, double b0, double b1, double apart, double wa, double wb, double gmd)
{
    const double widest = std::max(wa, wb);
    if (apart <= 1e-6 * std::max(widest, gmd)) return linePair(a0, a1, b0, b1, gmd);
    if (apart > 3 * widest) return linePair(a0, a1, b0, b1, apart);
    const double at[2] = {-0.5 / std::sqrt(3.0), 0.5 / std::sqrt(3.0)};   // (Gauss's two points on a side)
    double sum = 0;
    for (const double ax : at)
        for (const double ay : at)
            for (const double bx : at)
                for (const double by : at) {
                    const double r = std::hypot(apart + ax * wa - bx * wb, ay * wa - by * wb);
                    sum += linePair(a0, a1, b0, b1, std::max(r, 1e-6 * widest)) / 16;
                }
    return sum;
}

/// Int Int (exp(-jkR) - 1)/R dz dz' between two lines along z, R =
/// sqrt(rho^2 + (z - z')^2): what the retardation adds to linePair(),
/// smooth, by Gauss's rule.
Complex retardedPair(Complex k, double a0, double a1, double b0, double b1, double rho)
{
    const Rule& r = gaussRule(4);
    Complex sum(0, 0);
    for (int i = 0; i < r.at.size(); ++i)
        for (int j = 0; j < r.at.size(); ++j) {
            const double z = (a0 + a1) / 2 + r.at.at(i) * (a1 - a0), zz = (b0 + b1) / 2 + r.at.at(j) * (b1 - b0);
            sum += r.weight.at(i) * r.weight.at(j) * retardation(k, std::hypot(rho, z - zz));
        }
    return sum * (a1 - a0) * (b1 - b0);
}

/// A via of the mesh: a run of current in z from a cell of one metal (or
/// the ground) to a cell of the next, the via cells that join the same two
/// made one.
struct Via {
    int run = -1;             ///< its index among the runs
    int span = 0;             ///< which of the stack's vias it is of
    QPointF at;               ///< where it stands, m
    double side = 0;          ///< its cross-section's side, m (the square of its area)
    double gmd = 0;           ///< its cross-section's mean distance with itself, m
};

/*!
 * A port's own path to a cell it drives, up from the ground - or from the
 * metal an internal port is driven from - as a via whose current the port
 * sets rather than the solve: \a current of its column's ampere up it, its
 * ends \a ends, each cell's potential added in with its weight (+ at its
 * top, - at its bottom; none for the ground, at nought). It stands at the
 * cell's centre, where its current enters the metal, as a via to the
 * ground does.
 */
struct PortPath {
    Via via;                         ///< run -1: no unknown of the solve
    int column = -1;
    double current = 0;
    QList<QPair<int, double>> ends;
};

/*!
 * A leg of the ports' own currents where they come up by no path of their
 * own - a coplanar port's, across its gaps from the conductors beside it; a
 * port's that names its returns, from them to its cells, as a probe carries
 * it. The columns whose sides touch - their own cells, their returns' - are
 * one probe's, with one hub at the middle of those sides; from it a straight
 * leg in the plane runs to each side's middle (by its shares), at the height
 * of most of the probe's cells, and each column's ampere comes in along its
 * return's leg and out along its own's (Context::sourceOf). Their dipole is
 * the columns' charges', and what flows along them is the cells' injections
 * alone: a current round two ports' ties, which drives nothing, carries
 * nothing here either, and columns made of the same sides some other way
 * come out the same. They have only the part of a current's field that
 * radiates (Kernels::smooth()): its static part, the probe's own loop's
 * inductance, is not in the answer - a reactance, which takes no power -,
 * and without the rest the scalar potential alone took the current's
 * radiation the wrong way, minus half of it, as G9's paths did up from the
 * ground: a ground-signal probe at each end of a lossless line with nothing
 * below gave back 9e-4 at 40 GHz, 5.3e-3 on gold at 60.
 */
struct SourceRun {
    QPointF from, to;                ///< m: the hub, and the side's middle
    int height = 0;                  ///< which of the heights
    double length = 0;               ///< m
    double dirX = 0, dirY = 0;
};

/// A via run as the mesh makes it, before the solve numbers its cells.
struct ViaRun {
    Filament run;
    int tail = -1, head = -1;  ///< the mesh's cells below and above it; -1 below: the ground
    int span = 0;              ///< which of the stack's vias it is of
    QPointF at;                ///< m
    double side = 0, gmd = 0;  ///< m
    QList<int> cells;          ///< the via's own cells of the mesh it is made of
};

/*!
 * The cells of a mesh in bins of the plane, layer by layer, each in every
 * bin its box touches: what is near a point, without a search of every
 * cell. The bins are as wide as the mesh's widest cell.
 */
class CellBins
{
public:
    explicit CellBins(const Mesh& mesh)
    {
        for (const MeshCell& cell : mesh.cells) {
            const QRectF box = cell.outline.boundingRect();
            a_bin = std::max({a_bin, box.width(), box.height()});
        }
        if (!(a_bin > 0)) a_bin = 1;
        for (int c = 0; c < mesh.cells.size(); ++c) {
            const QRectF box = mesh.cells.at(c).outline.boundingRect();
            QHash<qint64, QList<int>>& layer = a_layers[mesh.cells.at(c).layer];
            for (qint64 i = binOf(box.left()); i <= binOf(box.right()); ++i)
                for (qint64 j = binOf(box.top()); j <= binOf(box.bottom()); ++j) layer[key(i, j)] << c;
        }
    }
    /// The cells of \a layer in the bins within \a reach of \a at, each once.
    QList<int> near(const QString& layer, QPointF at, double reach) const
    {
        QList<int> out;
        const auto found = a_layers.constFind(layer);
        if (found == a_layers.constEnd()) return out;
        for (qint64 i = binOf(at.x() - reach); i <= binOf(at.x() + reach); ++i)
            for (qint64 j = binOf(at.y() - reach); j <= binOf(at.y() + reach); ++j) {
                const auto bin = found->constFind(key(i, j));
                if (bin != found->constEnd()) out << *bin;
            }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

private:
    qint64 binOf(double v) const { return qint64(std::floor(v / a_bin)); }
    static qint64 key(qint64 i, qint64 j) { return (i << 32) ^ (j & 0xffffffffLL); }
    double a_bin = 0;
    QHash<QString, QHash<qint64, QList<int>>> a_layers;
};

/*!
 * The vias of \a mesh as runs of current in z, each from the cell of the
 * lower metal under it - or the ground - to the cell of the upper one. The
 * cells of a via that land on the same two cells are one run: their
 * current goes the same way, and a via cut ten across would otherwise be a
 * hundred. A run stands where its cells are, but a via to the ground at
 * the centre of the cell it lands on: its current enters the metal there,
 * as the runs that carry it away leave from there, and the potential the
 * formulation adds for a vertical current must be taken at one place for
 * the two to cancel as they do along a whole path. \a spans gets each of the
 * stack's vias the mesh has, from the lower metal's height to the upper's;
 * \a warned what was left out.
 */
QList<ViaRun> viaRunsOf(const Mesh& mesh, const Stack& stack, double micrometre, QList<ViaGreen::Span>* spans,
                        QStringList* warned)
{
    // The cell of metal \a metal under via cell \a v: the one whose outline
    // holds its centre, else the nearest whose box, grown by the via cell's
    // own size, does; -1 where the via is over none of that metal. (Of the
    // metal's cells in the bins the grown box touches: every cell of the
    // mesh for each of a via's cells was most of a large via farm's solve.)
    const CellBins bins(mesh);
    const auto under = [&mesh, &bins](const QString& metal, const MeshCell& v) {
        const QRectF box = v.outline.boundingRect();
        const double grow = std::hypot(box.width(), box.height()) / 2;
        int best = -1, holding = -1;
        double closest = 0;
        for (const int c : bins.near(metal, v.centre, grow)) {
            const MeshCell& cell = mesh.cells.at(c);
            if (cell.outline.containsPoint(v.centre, Qt::OddEvenFill)) {
                if (holding < 0 || c < holding) holding = c;
                continue;
            }
            if (!cell.outline.boundingRect().adjusted(-grow, -grow, grow, grow).contains(v.centre)) continue;
            const QPointF d = cell.centre - v.centre;
            const double far = d.x() * d.x() + d.y() * d.y();
            if (best < 0 || far < closest || (far == closest && c < best)) {
                best = c;
                closest = far;
            }
        }
        return holding >= 0 ? holding : best;
    };
    struct Group {
        int span = 0, tail = -1, head = -1;
        double area = 0, metal = 0, sigma = 0, z0 = 0, z1 = 0;
        QPointF weighted;
        QRectF box;
        QList<int> cells;
    };
    QStringList names;   // (the stack's vias, in the order the mesh meets them)
    QMap<QString, Group> groups;
    QStringList keys;    // (the groups in the order they were met)
    int stranded = 0;
    // (A via's cells through a shape of a metal between its two, which it
    //  does not join: as made it would short to it, and a 3D export does -
    //  a via from M1 to M3 through an M2 strip left it a net of its own, |S31|
    //  0.021 where joined it reads some 0.5, nothing said: a bug hunt's second hour. Of each via and metal, how many.)
    QMap<QPair<QString, QString>, int> through;
    for (int index = 0; index < mesh.cells.size(); ++index) {
        const MeshCell& cell = mesh.cells.at(index);
        const Layer* via = stack.named(cell.layer);
        if (via == nullptr || via->kind != Layer::Via) continue;
        const double za = metalHeight(stack, via->from), zb = metalHeight(stack, via->to);
        if (!(za >= 0) || !(zb >= 0) || za == zb) {
            ++stranded;
            continue;
        }
        for (const Layer& between : stack.layers) {
            if (between.kind != Layer::Metal || between.face == Layer::Top || between.name == via->from || between.name == via->to)
                continue;
            const double z = metalHeight(stack, between.name);
            if (!(z > std::min(za, zb) && z < std::max(za, zb))) continue;
            for (const int c : bins.near(between.name, cell.centre, 0))
                if (mesh.cells.at(c).outline.containsPoint(cell.centre, Qt::OddEvenFill)) {
                    ++through[qMakePair(cell.layer, between.name)];
                    break;
                }
        }
        const QString lower = za < zb ? via->from : via->to, upper = za < zb ? via->to : via->from;
        const bool grounded = stack.isGround(lower);
        const int tail = grounded ? -1 : under(lower, cell), head = under(upper, cell);
        if (head < 0 || (!grounded && tail < 0)) {
            ++stranded;
            continue;
        }
        if (!names.contains(cell.layer)) {
            names << cell.layer;
            spans->append({std::min(za, zb), std::max(za, zb)});
        }
        const QString key = QStringLiteral("%1|%2|%3").arg(cell.layer).arg(tail).arg(head);
        if (!groups.contains(key)) keys << key;
        Group& g = groups[key];
        g.span = int(names.indexOf(cell.layer));
        g.tail = tail;
        g.head = head;
        g.area += cell.area;
        g.metal += cell.area * cell.share;
        g.weighted += cell.centre * cell.area;
        g.box = g.box.isNull() ? cell.outline.boundingRect() : g.box.united(cell.outline.boundingRect());
        g.cells << index;
        g.sigma = via->sigma;
        g.z0 = std::min(za, zb);
        g.z1 = std::max(za, zb);
    }
    QList<ViaRun> out;
    for (const QString& key : std::as_const(keys)) {
        const Group& g = groups.value(key);
        if (!(g.area > 0)) continue;
        ViaRun v;
        v.tail = g.tail;
        v.head = g.head;
        v.span = g.span;
        v.at = (g.tail < 0 ? mesh.cells.at(g.head).centre : g.weighted / g.area) * micrometre;
        // (Its side the square of its metal: a merged farm's footprint
        //  conducts as the vias in it do - Conductor::share. Its field, its
        //  mean distance, the footprint's.)
        v.side = std::sqrt(g.metal) * micrometre;
        // Rosa's mean distance of a rectangle with itself, 0.2235 (a + b):
        // a square's 0.447 a, a thin strip's e^-3/2 of its width.
        v.gmd = 0.2235 * (g.box.width() + g.box.height()) * micrometre;
        v.run.from = v.run.to = v.at;
        v.run.z0 = g.z0;
        v.run.z1 = g.z1;
        v.run.width = v.run.thickness = v.side;
        v.run.sigma = g.sigma;
        v.cells = g.cells;
        out << v;
    }
    // (Warned of, not a line of the notes alone: a ground via drawn off the
    //  strip it was to short went with no word, the line read through, S21
    //  0.998 where on it 0.021.)
    if (stranded > 0 && warned != nullptr)
        warned->append(QCoreApplication::translate("qucs_s::rf25", "%1 via cell(s) land on no metal they join, and are left out: a "
                                                                   "via drawn off the pads of its two metals joins nothing.")
                           .arg(stranded));
    for (auto it = through.cbegin(); it != through.cend() && warned != nullptr; ++it)
        warned->append(QCoreApplication::translate("qucs_s::rf25", "%1 cell(s) of %2 pass through a shape of %3, a metal between "
                                                                   "the two it joins, without joining it: here %3 stays apart, "
                                                                   "but as made the via would short to it, as a 3D solver's "
                                                                   "export of it does. Move the via, or cut %3 round it.")
                           .arg(it.value())
                           .arg(it.key().first, it.key().second));
    return out;
}

/// Everything a frequency's solve needs that does not change with the
/// frequency: made once, and read by every thread.
/*!
 * A compressed solve's static parts of the pairs whose parts make them dear
 * (Context::runStatics and cellStatics): row by row, each row's others in
 * order and two values for each - a pair of runs' aligned partial
 * inductances, direct and of the image, or a cell's static potential of a
 * charge spread over another. Empty: none kept.
 */
struct PairTable {
    std::vector<qint64> start;           ///< each row's first, and one past the last row's
    std::vector<int> other;
    std::vector<double> first, second;
    bool empty() const { return start.empty(); }
    /// Where row \a a's pair with \a b is, or -1 where it is not kept.
    qint64 find(int a, int b) const
    {
        if (start.empty() || a < 0 || a + 1 >= qint64(start.size())) return -1;
        const int* begin = other.data() + start[std::size_t(a)];
        const int* end = other.data() + start[std::size_t(a) + 1];
        const int* at = std::lower_bound(begin, end, b);
        return at != end && *at == b ? qint64(at - other.data()) : -1;
    }
};

struct Context {
    Layers layers;
    QList<Filament> runs;                ///< in the plane, then the vias
    QList<int> tail, head;               ///< each run's cells, as charges (a via's tail -1 on the ground)
    QList<QRectF> rect;                  ///< each charge's cell, m
    QList<QPointF> centre;               ///< m
    /// Each charge's cell's frame (MeshCell::angle): a strip's cell lies in
    /// its segment's, its rect of that frame's axes about its centre - a
    /// point seen from it turned into that frame (seenFrom()). Nought: the
    /// layout's own axes.
    QList<double> angle;
    /// \a at (m) as charge \a from's rect sees it: turned into its frame.
    QPointF seenFrom(int from, const QPointF& at) const
    {
        const double a = from < angle.size() ? angle.at(from) : 0.0;
        return a == 0 ? at : MeshCell::turnedAbout(at, centre.at(from), -a);
    }
    /// Charge \a of's \a parts (m; a strip's cell's in its own frame) as
    /// charge \a from's frame has them: each part in the plane moved to
    /// where it is seen from there, its rectangle of the same size. As they
    /// are where neither is a strip's cell.
    QList<ChargePart> partsSeenFrom(int from, int of, QList<ChargePart> parts) const
    {
        const double a = of < angle.size() ? angle.at(of) : 0.0;
        if ((from >= angle.size() || angle.at(from) == 0) && a == 0) return parts;
        for (ChargePart& p : parts) {
            if (p.plane != 0) continue;
            QPointF to(p.x, p.y);
            if (a != 0) to = MeshCell::turnedAbout(to, centre.at(of), a);
            to = seenFrom(from, to);
            p.rect.translate(to.x() - p.x, to.y() - p.y);
            p.x = to.x();
            p.y = to.y();
        }
        return parts;
    }
    QList<double> heights;               ///< the metals', m, from the lowest
    QList<int> hNode, hRun;              ///< which of them each charge and run in the plane is at
    QList<double> cellZ;                 ///< each charge's height, m
    QList<double> dirX, dirY, midX, midY;
    QList<Filament> mirrored;            ///< each run in the plane's image in the ground
    QList<Filament> lidMirrored;         ///< and in a metal lid's, where there is one (Layers::groundAbove)
    /// The lid's image folded into the ground's in the static parts (lImage,
    /// runStatics): both planes there and no layer magnetic, so that their
    /// images' coefficients are the same - made once, not at every frequency.
    bool lidFolded = false;
    /// A dense solve's static parts, made once for every frequency and on
    /// every thread (makeStatics()): each pair of runs' aligned partial
    /// inductance (alignedInductance()), direct and of the image, and each
    /// cell's static potential of a charge spread over another. (A
    /// compressed solve makes the few it wants as it goes.)
    Eigen::MatrixXd lDirect, lImage, pStatic;
    /// The stack's ended layers' static polarization in P (rf25_dielectric.h),
    /// added to every potential coefficient; whether there is any.
    DielectricCorrection dielectric;
    bool corrected = false;
    QStringList dielectricLines;
    QList<Via> vias;
    QList<ViaGreen::Span> spans;         ///< each of the stack's vias that the mesh has
    /// Each port's current into the charges (and out of an internal
    /// port's other metal): charges x columns. A port is one column, or a
    /// coplanar one a column against each conductor beside it.
    MatrixC drive;
    QList<int> portOf;                   ///< which port each column is
    /*!
     * Where a port has more than one column, the columns the solves take
     * instead, columns x columns (columnBasis()): each port's first, in the
     * ports' order; then its others' differences from it, those that carry
     * a net current from one conductor to another; then the loops among
     * those differences, which carry none into any conductor. Each solve's
     * Z and currents are of these, and the ports are tied as a Schur
     * complement over the differences and loops. Empty: the columns as they
     * are.
     */
    MatrixC basis;
    /// The ports' own paths to the cells they drive (PortPath): none for a
    /// coplanar port, whose current comes back beside it. Their spans are
    /// among the vias'.
    QList<PortPath> paths;
    /// The legs of the ports' own currents where they come up by no path
    /// (SourceRun), and what each column carries along each, legs x columns:
    /// +1 out along its own side's, -1 along its return's.
    QList<SourceRun> sources;
    Eigen::MatrixXd sourceOf;
    /// How many of each run in the plane's two sides are its conductor's
    /// edge, where the current crowds inside the cells (crowding()); and
    /// which: bit 0 the side at -normal, bit 1 at +normal, the normal the
    /// run's direction turned a quarter anticlockwise.
    QList<int> freeSides, freeMask;
    QList<bool> roundRuns;               ///< each planar run's: a round wire's, its loss crowding to no edge
    /// A run's current spread over more than its cells: each part a
    /// filament, its image in the ground, and its share of the current.
    /// Empty where the run is its cells alone. On a metal solved as its two
    /// faces a run along an edge is its face's strip and its half of the
    /// wall there - from the face to the metal's middle - its current shared
    /// by length (wallsOf(); A1 of the open items of 8 November: walls that
    /// carry no current left L 3 to 9 % high).
    struct Part {
        Filament run, image;
        double weight = 1;
    };
    QList<QList<Part>> parts;
    /// Each run with parts as one filament where its current is on the whole
    /// (and its image): what a pair far apart takes (staticPair()).
    QList<Filament> centroid, centroidImage;
    /// A compressed solve's static parts of the pairs whose parts make them
    /// dear - each of the edge profile's and the walls' pieces with each of
    /// the other's -, made once before its frequencies (cacheStatics()), so
    /// that each frequency's fill reads them where it made them again: some
    /// half of a gold pad's compressed fill, 45 % of its solve. A dense solve
    /// has them all (lDirect, lImage, pStatic) and keeps none here.
    PairTable runStatics, cellStatics;
    /// Each cell's charge spread over more than its cell, as Part a run's
    /// current (rf25_edge.h's wallCharges()): empty where it is its cell
    /// alone.
    QList<QList<ChargePart>> charges;
    /// Each cell's share of what its parts change (staticPotential()) where
    /// a layer ends: the singular term's strength where the cell is - each
    /// ended layer as inside its shapes where its centre is within them -
    /// over the background's. Empty: one everywhere.
    QList<double> profileScale;
    /// Each run in the plane's own on its metal's other face, where the
    /// metal is solved as its two (thickened()); -1 on a sheet.
    QList<int> twin;
    /// Each run with a twin: how much of a wide slab's inside its faces'
    /// surface impedance holds at a low frequency (facesInside()), of its
    /// conductor's breadth across it at its middle. Empty, or one: all.
    QList<double> inside;
    /// A metal or impedance ground's loss, where it is asked for in the full
    /// wave too (Layer::lossInFullWave): each pair of planar runs near enough
    /// to share the ground current their mirror images carry, and what they
    /// share of it for a unit current in each, K - Ohm per Ohm of the plane's
    /// surface impedance (groundPairsOf()).
    struct GroundPair {
        int a = 0, b = 0;
        double k = 0;
    };
    QList<GroundPair> groundPairs;
    double groundSigma = 0;          ///< S/m: a metal ground's
    double groundImpedance = -1;     ///< Ohm per square: an impedance ground's; below nought, a metal's
    double rhoMax = 0;
    /// A box (Layers::walls): its guide's modes slower than boxSplit (1/m)
    /// in the rectangle's own sum, the rest over the walls' images out to
    /// boxReach (m) - its runs along x or y, its ports at its walls.
    bool boxed = false;
    double boxSplit = 0, boxReach = 0;
    /// A plane of symmetry (FullWaveControl::Symmetry): 1 across x, 2 across
    /// y, at symAt (m); its charge's image's sign (+1 a magnetic wall, -1 an
    /// electric one) - a current along it the same, across it the opposite;
    /// and whether the ports are on it, the whole's Z half the half's.
    int symAxis = 0;
    double symAt = 0, symCharge = 1;
    bool halved = false;
    QPointF symmetric(const QPointF& p) const
    {
        return symAxis == 1 ? QPointF(2 * symAt - p.x(), p.y()) : QPointF(p.x(), 2 * symAt - p.y());
    }
    int nr = 0, nc = 0, np = 0, nh = 0;
    int planar = 0;                      ///< the runs in the plane, which come first
};

/*!
 * The walls' current (Context::parts): each run along an edge of a metal
 * solved as its two faces made its face's strip and its half of the wall
 * on each free side - a strip standing upright on the edge's line, from the
 * face to the metal's middle -, the current shared between them by length,
 * as the cross-section's own solve of solid metal shares it near enough
 * (walls.py beside an earlier note:
 * a perfect metal's L within 0.01 % of a boundary-element solve's, where
 * the faces alone are 3 % over). The charge goes on the walls too
 * (Context::charges), and the run's loss is its band's of the edge solved
 * in its cross-section (rf25_edge.h): ownImpedances().
 */
void wallsOf(Context& c)
{
    c.parts = QList<QList<Context::Part>>(c.nr);
    c.centroid = c.runs;
    c.centroidImage = c.mirrored;
    for (int a = 0; a < c.planar; ++a) {
        const int twin = c.twin.value(a, -1), mask = c.freeMask.value(a);
        const Filament& run = c.runs.at(a);
        if (mask == 0 || !(run.width > 0)) continue;
        const QPointF d = run.to - run.from;
        const double length = std::hypot(d.x(), d.y());
        if (!(length > 0)) continue;
        const QPointF normal(-d.y() / length, d.x() / length);
        const bool faces = twin >= 0 && run.thickness > 0;
        const int walls = faces ? (mask & 1 ? 1 : 0) + (mask & 2 ? 1 : 0) : 0;
        const double half = run.thickness / 2;
        const double total = run.width + walls * half;
        const double faceShare = faces ? run.width / total : 1.0;
        // The face's strip as the edge has its current (Z0's convergence,
        // the plan's phase 5): sub-strips graded toward its free sides, each
        // its share of d^-alpha - a half on a sheet; a third on a face of
        // two, whose walls carry current as a solid corner's does -, where an
        // even current left L first order in the cells across, as the even
        // charge left C. (A half on the faces' runs too took the lines' R 3
        // to 11 % under their 2D solves.)
        const auto pieces = edgeProfiles() ? edgeProfile(run.width, mask & 1, mask & 2, faces ? 1.0 / 3 : 0.5)
                                           : QList<std::pair<std::pair<double, double>, double>>{{{0.0, run.width}, 1.0}};
        if (!faces && pieces.size() == 1) continue;   // (a sheet's run, even: no parts)
        double shiftX = 0, shiftY = 0, shiftZ = 0;
        for (const auto& [range, share] : pieces) {
            Filament strip = run;
            const QPointF shift = (-0.5 * run.width + 0.5 * (range.first + range.second)) * normal;
            strip.from = run.from + shift;
            strip.to = run.to + shift;
            strip.width = range.second - range.first;
            Filament image = strip;
            image.z0 = image.z1 = -strip.z0;
            c.parts[a] << Context::Part{strip, image, faceShare * share};
            shiftX += faceShare * share * shift.x();
            shiftY += faceShare * share * shift.y();
        }
        if (faces) {
            const bool top = run.z0 > c.runs.at(twin).z0;
            const double middle = top ? run.z0 - half / 2 : run.z0 + half / 2;
            for (const double side : {-1.0, 1.0}) {
                if (!(mask & (side < 0 ? 1 : 2))) continue;
                Filament wall = run;
                const QPointF shift = side * 0.5 * run.width * normal;
                wall.from = run.from + shift;
                wall.to = run.to + shift;
                wall.z0 = wall.z1 = middle;
                wall.width = 0;
                wall.rise = half;
                Filament image = wall;
                image.z0 = image.z1 = -middle;
                c.parts[a] << Context::Part{wall, image, half / total};
                shiftX += half / total * shift.x();
                shiftY += half / total * shift.y();
                shiftZ += half / total * (middle - run.z0);
            }
        }
        Filament& whole = c.centroid[a];
        whole.from += QPointF(shiftX, shiftY);
        whole.to += QPointF(shiftX, shiftY);
        whole.z0 = whole.z1 = run.z0 + shiftZ;
        c.centroidImage[a] = whole;
        c.centroidImage[a].z0 = c.centroidImage[a].z1 = -whole.z0;
    }
}

/*!
 * Runs \a a and \a b's static partial inductance, aligned
 * (alignedInductance()), \a direct and of b's image in the ground \a image:
 * over their parts where either has them (Context::parts), each pair by
 * their shares of the current. (Iterated over the one list where a is b,
 * so that a part with itself is the same filament - its own partial
 * inductance by its geometric mean distance.)
 */
/// Whether runs \a a and \a b's static parts are taken over their parts,
/// not each run whole or as its centroid: either has parts, and they are
/// near - within eight of their breadths and their metal's thickness.
bool dearPair(const Context& c, int a, int b)
{
    const bool partsA = a < c.parts.size() && !c.parts.at(a).isEmpty();
    const bool partsB = b < c.parts.size() && !c.parts.at(b).isEmpty();
    if (!partsA && !partsB) return false;
    if (a == b) return true;
    const Filament& ra = c.runs.at(a);
    const Filament& rb = c.runs.at(b);
    const double size = std::max({ra.width, rb.width, ra.thickness, rb.thickness});
    const double apart = std::hypot(0.5 * (ra.from.x() + ra.to.x() - rb.from.x() - rb.to.x()),
                                    0.5 * (ra.from.y() + ra.to.y() - rb.from.y() - rb.to.y()));
    return !(apart > 8 * size * (1 + 1e-6));
}

void staticPairOf(const Context& c, int a, int b, double* direct, double* image);

/// staticPairOf()'s, the lid's image folded into the ground's where it is
/// (Context::lidFolded): a run's whole filament, the lid far beside it.
void staticPairWithLid(const Context& c, int a, int b, double* direct, double* image)
{
    staticPairOf(c, a, b, direct, image);
    if (c.lidFolded) *image += alignedInductance(c.runs.at(a), c.lidMirrored.at(b));
}

void staticPairOf(const Context& c, int a, int b, double* direct, double* image)
{
    const bool partsA = a < c.parts.size() && !c.parts.at(a).isEmpty();
    const bool partsB = b < c.parts.size() && !c.parts.at(b).isEmpty();
    if (!partsA && !partsB) {
        *direct = alignedInductance(c.runs.at(a), c.runs.at(b));
        *image = alignedInductance(c.runs.at(a), c.mirrored.at(b));
        return;
    }
    // (Far apart - eight of their breadths and their metal's thickness -,
    //  each its current's centroid: the parts' shift toward the edge, to
    //  the first order, without their pairs.)
    if (a != b) {
        const Filament& ra = c.runs.at(a);
        const Filament& rb = c.runs.at(b);
        const double size = std::max({ra.width, rb.width, ra.thickness, rb.thickness});
        const double apart = std::hypot(0.5 * (ra.from.x() + ra.to.x() - rb.from.x() - rb.to.x()),
                                        0.5 * (ra.from.y() + ra.to.y() - rb.from.y() - rb.to.y()));
        if (apart > 8 * size * (1 + 1e-6)) {   // (a margin: staticPotential()'s)
            *direct = alignedInductance(c.centroid.at(a), c.centroid.at(b));
            *image = alignedInductance(c.centroid.at(a), c.centroidImage.at(b));
            return;
        }
    }
    const Context::Part wholeA{c.runs.at(a), c.mirrored.at(a), 1}, wholeB{c.runs.at(b), c.mirrored.at(b), 1};
    const QList<Context::Part> onlyA{wholeA}, onlyB{wholeB};
    const QList<Context::Part>& pa = partsA ? c.parts.at(a) : onlyA;
    const QList<Context::Part>& pb = a == b ? pa : (partsB ? c.parts.at(b) : onlyB);
    double d = 0, i = 0;
    for (const Context::Part& x : pa)
        for (const Context::Part& y : pb) {
            d += x.weight * y.weight * alignedInductance(x.run, y.run);
            i += x.weight * y.weight * alignedInductance(x.run, y.image);
        }
    *direct = d;
    *image = i;
}

/// staticPairOf(), read where a compressed solve keeps it (cacheStatics()).
void staticPair(const Context& c, int a, int b, double* direct, double* image)
{
    const qint64 at = c.runStatics.find(a, b);
    if (at >= 0) {
        *direct = c.runStatics.first[std::size_t(at)];
        *image = c.runStatics.second[std::size_t(at)];
        return;
    }
    staticPairWithLid(c, a, b, direct, image);
}

double staticPotential(const Context& c, int i, int j);

/// staticPairWithLid() of run a and run b's mirror in a plane of symmetry
/// (Context::symAxis): b's parts - its sub-strips graded to its edges -
/// mirrored with it, or far apart its current's centroid's, as
/// staticPairOf() takes them; their signs outside.
void symmetryPair(const Context& c, int a, int b, double* direct, double* image)
{
    const auto mirror = [&c](Filament f) {
        f.from = c.symmetric(f.from);
        f.to = c.symmetric(f.to);
        return f;
    };
    const bool partsA = a < c.parts.size() && !c.parts.at(a).isEmpty();
    const bool partsB = b < c.parts.size() && !c.parts.at(b).isEmpty();
    const Filament mb = mirror(c.runs.at(b));
    double d = 0, i = 0;
    if (!partsA && !partsB) {
        d = alignedInductance(c.runs.at(a), mb);
        i = alignedInductance(c.runs.at(a), mirror(c.mirrored.at(b)));
    } else {
        const Filament& ra = c.runs.at(a);
        const double size = std::max({ra.width, mb.width, ra.thickness, mb.thickness});
        const double apart = std::hypot(0.5 * (ra.from.x() + ra.to.x() - mb.from.x() - mb.to.x()),
                                        0.5 * (ra.from.y() + ra.to.y() - mb.from.y() - mb.to.y()));
        if (apart > 8 * size * (1 + 1e-6)) {
            d = alignedInductance(c.centroid.at(a), mirror(c.centroid.at(b)));
            i = alignedInductance(c.centroid.at(a), mirror(c.centroidImage.at(b)));
        } else {
            const Context::Part wholeA{c.runs.at(a), c.mirrored.at(a), 1}, wholeB{c.runs.at(b), c.mirrored.at(b), 1};
            const QList<Context::Part> onlyA{wholeA}, onlyB{wholeB};
            const QList<Context::Part>& pa = partsA ? c.parts.at(a) : onlyA;
            const QList<Context::Part>& pb = partsB ? c.parts.at(b) : onlyB;
            for (const Context::Part& x : pa)
                for (const Context::Part& y : pb) {
                    d += x.weight * y.weight * alignedInductance(x.run, mirror(y.run));
                    i += x.weight * y.weight * alignedInductance(x.run, mirror(y.image));
                }
        }
    }
    if (c.lidFolded) i += alignedInductance(c.runs.at(a), mirror(c.lidMirrored.at(b)));
    *direct = d;
    *image = i;
}

/// What one frequency solved to.
struct AtFrequency {
    MatrixC z;                           ///< ports x ports
    MatrixC current;                     ///< runs x ports
    double rcond = 1;
    /// How far the charges' term outweighs the rest of a run's own in M, at
    /// its largest over the runs: past some 1e11 the currents are lost in
    /// its rounding - the mixed potential's breakdown at a low frequency,
    /// an open where a coil is (N4 of the open items). Nought: not measured.
    double chargeOverSeries = 0;
    /// At a low frequency: solved with the cells' charges as unknowns of
    /// their own (kAugmentedFrom).
    bool augmented = false;
    /// Compressed: so tried, short of its residual in kAugmentedMost
    /// iterations, and solved with them eliminated instead - the iterations
    /// it took.
    int augmentedTried = 0;
    /// Compressed: not tried so, as it fell short at a higher frequency of
    /// the sweep - the ratio it fell short at, there (nought: tried, or not
    /// asked).
    double augmentedShortAt = 0;
    /// Compressed, so solved: the currents its loops' and its tree's, the
    /// ports' voltages along the tree (LoopTree).
    bool loopTree = false;
    /// So solved, preconditioned by the near field's blocks as a higher
    /// frequency of its thread factorized them (NearBlocks): the iterations
    /// they took there.
    int reusedFrom = 0;
    /// Dense: the seconds its fill took (the Green's functions, the system
    /// and P, up to its LU) and its LU (the factors, the solve, rcond: not
    /// its wait for OpenBLAS's thread), and whether Accelerate's LAPACK or
    /// OpenBLAS's made them (rf25_dense.h).
    double fillSeconds = 0, luSeconds = 0;
    bool accelerated = false;
    bool openBlas = false;
    bool symmetric = false;          ///< factorized as L D L^T (DenseFactors)
    /// Compressed: what its matrices held, bytes, and their largest rank;
    /// the most iterations a port's GMRES took, and the largest residual.
    double bytes = 0;
    int largestRank = 0;
    int iterations = 0;
    double residual = 0;
    /// What makes its answer doubtful: GMRES short of its residual, or near
    /// its limit.
    QStringList warnings;
    QStringList problems;
};

/*!
 * How much more a run along its conductor's edge loses than its cells'
 * average current says - the factor on its sheet's surface impedance. The
 * current on a sheet crowds to a free edge as one over the root of the
 * distance from it, and a cell along the edge carries it as its average:
 * of the singular part a cell \a width across (m) holds, its loss is
 * ln(e/D)/4 times its average's with one side on the edge, (2/pi^2)
 * ln(e/D) with both. D is where the metal's \a thickness stops the
 * crowding: t/(4 pi e^pi), the stopping distance at which a sheet loses
 * what a rectangular edge's walls and corners do (Holloway and Kuester,
 * Radio Sci. 29 (1994) 539; it is the t in Ghione's coplanar loss) - at a
 * thick metal's resistance, Rs/2 a face. Where the metal is not thick
 * beside its skin depth, its \a sheet resistance beyond that is spread
 * over the cell as it is through the metal - the edge's loss is the
 * crowding at Rs/2 and that beyond it evenly, over the sheet's own: where
 * the half slab dips under Rs/2 (copper 3 µm thick at 5 GHz, 8 %) the
 * edge's loss does not. The crowding itself wants the cell wide beside
 * Pearl's length 2 R/(w mu0), over which the sheet's own resistance spreads
 * a current sideways, and fades where it is not - near direct current,
 * where the factor is one. So a 2D solve of the current through a thick
 * metal's edge says, within about 10 % from a third of a skin depth thick
 * to ten; the stopping distance alone had a metal three micrometres thick
 * at 1 GHz lose a third less than it does.
 *
 * A metal solved as its two \a faces stops it three times as far out. Its
 * walls draw into its edge cells the current that crowds there - a
 * microstrip's outer cell carries 0.469 of it, as a 2D solve of its
 * current through the metal does 0.474, where a sheet's carries 0.444 -
 * and what is left within the cell is the less: the 2D solve's edge cell
 * loses 1.50 times its current's average at four cells across, 1.28 at
 * eight, where Holloway and Kuester's stopping distance says 1.82 and
 * 1.64. Three times it says 1.50 and 1.33, and within -4 to +10 % from 2
 * to 60 GHz and from 1 to 3 µm thick.
 *
 * Graded at a narrow gap (MeshSettings::gaps), an edge cell is down to a
 * quarter of it, and on a thick metal narrower than the metal is thick:
 * there the 2D solve's band loses less than its current's average - 0.78
 * of it 4.2 µm in from the edge of 6.5 µm of metal -, its current running
 * down the wall too. The faces carry none down their walls, though, and
 * such a cell of theirs takes the less of the current the lower the
 * frequency, so the factor as it is, though over the band's, comes the
 * nearer: on five lines it left a coplanar line's attenuation 9 % from
 * their 2D solves on average, where one faded to nought as the cell
 * narrows left 16 %, and none tried came nearer
 */
double crowding(double width, double thickness, double sigma, int free, double frequency, Complex sheet, bool faces = false)
{
    if (free <= 0 || !(width > 0) || !(thickness > 0) || !(sigma > 0) || !(frequency > 0) || !(sheet.real() > 0)) return 1;
    constexpr double mu0 = 4e-7 * M_PI;
    const double omega = 2 * M_PI * frequency;
    const double stop = thickness / (4 * M_PI * std::exp(M_PI)) * (faces ? 3 : 1);
    const double log = std::log(1 + width / stop);
    const double thick = std::max(1.0, free >= 2 ? 2 / (M_PI * M_PI) * log : log / 4);
    const double pearl = 2 * sheet.real() / (omega * mu0) / width;
    const double crowd = 1 + (thick - 1) / (1 + pearl * pearl);
    const double half = std::sqrt(omega * mu0 / (2 * sigma)) / 2;   // a thick metal's sheet: Rs/2
    return (crowd * half + std::max(0.0, sheet.real() - half)) / sheet.real();
}

/*!
 * A face of a metal \a thickness (m) thick solved as its two faces
 * (thickened()): its own surface impedance \a own, Zs coth(gamma t) - the
 * slab's, with the field on its side alone - and \a across, Zs csch(gamma
 * t), what the current on the other face adds to it through the metal. The
 * two faces carrying a current alike are a sheet's (Zs/2) coth(gamma t/2),
 * sheetImpedance(). Thin beside its skin depth both are 1/(sigma t): the
 * faces are one metal, and how they share a current is nothing to it.
 * Many skin depths thick the faces are apart, each Zs.
 */
void faceImpedances(double sigma, double thickness, double frequency, Complex* own, Complex* across)
{
    *own = *across = Complex(0, 0);
    if (!(sigma > 0) || !(thickness > 0) || !(frequency > 0)) return;
    const double delta = std::sqrt(1 / (M_PI * frequency * kMu0 * sigma));
    const Complex gamma = Complex(1, 1) / delta, zs = gamma / sigma;
    // (coth = (1 + e^2)/(1 - e^2) and csch = 2e/(1 - e^2), e = exp(-gamma t),
    //  which neither overflows nor loses the thin metal's 1/(sigma t).)
    const Complex e = std::exp(-gamma * thickness);
    *own = zs * (1.0 + e * e) / (1.0 - e * e);
    *across = zs * 2.0 * e / (1.0 - e * e);
}

/// What fillShared() counts.
std::atomic<long long> sharedGreens{0}, sharedColumns{0}, sharedStatics{0}, keptRuns{0}, keptCells{0};
/// (cacheStatics()'s work on other threads: no dense solve's static parts.)
std::atomic<long long> sharedKept{0};

/*!
 * The Green's functions of one frequency, and the terms of the solve's
 * matrices of them: shared by the dense solve and the compressed one, so
 * that the two solve the one matrix - the dense one with the static parts
 * it made once for every frequency, the compressed one making the few it
 * wants as it goes.
 */
struct Kernels {
    const Context& c;
    double f = 0;
    Complex jw;
    QList<WaveGreen> greens;
    QList<int> pairOf;
    QList<ViaGreen> viaGreens;
    /// A box's guide's slow modes, each pair of heights' (as greens): the
    /// scalar kernel's and the vector's.
    QList<QList<GuideMode>> boxScalar, boxVector;
    QStringList problems;

    /// Made on as many as \a threads threads, a distance of a Green's
    /// function's table to a thread.
    Kernels(const Context& context, double frequency, int threads = 1);
    const WaveGreen& green(int h0, int h1) const { return greens.at(pairOf.at(h0 * c.nh + h1)); }
    /// The cosine between runs \a a and \a b in the plane.
    double cosine(int a, int b) const { return c.dirX.at(a) * c.dirX.at(b) + c.dirY.at(a) * c.dirY.at(b); }
    /// The kernel of runs \a a and \a b in the plane, H, whose L is their
    /// cosine times it: the static partial inductances, aligned (\a direct,
    /// and of b's image in the ground, \a image), scaled as the vector
    /// potential's singular terms are, and the rest at the middles - the
    /// retardation of the source and of its image, and the stack's own
    /// remainder.
    Complex inductance(int a, int b, double direct, double image) const;
    Complex inductance(int a, int b) const;
    /// The part of that kernel that is no static partial inductance, H/m^2
    /// a unit current element's at \a rho (m), between heights \a h0 and
    /// \a h1: the retardation of the source and of its image, and the
    /// stack's own remainder - what radiates, smooth and finite at nought.
    Complex smooth(int h0, int h1, double rho) const;
    /// P(i, j): cell i's potential of a unit charge spread over cell j and
    /// the other way round, averaged - each way's static part given
    /// (rectanglePotential(), \a pij and \a pji) or made here.
    Complex potential(int i, int j, double pij, double pji) const;
    Complex potential(int i, int j) const;
    /// What a box adds to P(i, j): the walls' images of the layered kernel
    /// less its slow modes, and those modes as the rectangle has them less
    /// their free parts - the two ways round averaged.
    Complex boxPotential(int i, int j) const;
    /// What a box adds to runs a and b's kernel, H, as inductance() is (its
    /// cosine times it): of runs along x the walls' images of A_x (+1
    /// across x's walls, -1 across y's) and the rectangle's cos-sin sum, of
    /// runs along y A_y's, the other way about; nothing between the two.
    Complex boxInductance(int a, int b) const;
    /// What a plane of symmetry adds to P(i, j): cell j's charge's mirror
    /// image's potential at cell i, its sign the wall's - the two ways round
    /// averaged.
    Complex symmetryPotential(int i, int j) const;
    /// What it adds to runs a and b's kernel, H, as inductance() is: run b's
    /// mirror's, its sign its current's - along the plane the charge's, across
    /// it the opposite.
    Complex symmetryInductance(int a, int b) const;
    /// Via \a v's potential at cell \a cell of a unit current up it.
    Complex viaPotential(int v, int cell) const { return potentialOf(c.vias.at(v), cell); }
    /// G_zz: via \a i's voltage of a unit current on via \a j.
    Complex viaCoupling(int i, int j) const { return couplingOf(c.vias.at(i), c.vias.at(j)); }
    /// The same of any two vias - a port's path among them - with, across a
    /// plane of symmetry, the mirror's (a vertical current runs along the
    /// plane: its sign the charge's).
    Complex potentialOf(const Via& via, int cell) const;
    Complex couplingOf(const Via& a, const Via& b) const;

private:
    Complex potentialOfOne(const Via& via, int cell) const;
    Complex couplingOfOne(const Via& a, const Via& b) const;
};

Kernels::Kernels(const Context& context, double frequency, int threads)
    : c(context), f(frequency), jw(0, 2 * M_PI * frequency)
{
    // The retarded Green's functions of each pair of heights; and a via's
    // kernels, one for each of the stack's vias the mesh has - its coupling
    // to every via, and its potential at every metal height. One after
    // another, the first that cannot be made said, each one's table - where
    // it is not kept from an earlier solve - made on all of the frequency's
    // threads, a distance to a thread as they come free. (Made side by side,
    // a thread each, they waited on the slowest: a coplanar line's at
    // 10 GHz, its walls' via's 0.46 s where its three pairs took 0.14 to
    // 0.21. One after another, all four take some 55 ms on 24 threads.)
    const int nh = c.nh;
    pairOf = QList<int>(nh * nh, -1);
    for (int i = 0; i < nh; ++i)
        for (int j = i; j < nh; ++j) {
            WaveGreen g(c.layers, c.heights.at(i), c.heights.at(j), f, c.rhoMax);
            if (!g.ok()) {
                problems << g.problems();
                return;
            }
            sharedGreens += g.prepare(threads);
            pairOf[i * nh + j] = pairOf[j * nh + i] = int(greens.size());
            greens << std::move(g);
            if (c.boxed) {
                QStringList lost;
                boxScalar << guideModes(c.layers, c.heights.at(i), c.heights.at(j), f, BoxKernel::Scalar, c.boxSplit, &lost);
                boxVector << guideModes(c.layers, c.heights.at(i), c.heights.at(j), f, BoxKernel::Vector, c.boxSplit, &lost);
                if (!lost.isEmpty()) {
                    problems << lost;
                    return;
                }
            }
        }
    for (const ViaGreen::Span& s : c.spans) {
        ViaGreen v(c.layers, s, c.spans, c.heights, f, c.rhoMax);
        if (!v.ok()) {
            problems << v.problems();
            return;
        }
        sharedGreens += v.prepare(threads);
        viaGreens << std::move(v);
    }
}

Complex Kernels::inductance(int a, int b, double direct, double image) const
{
    const WaveGreen& g = green(c.hRun.at(a), c.hRun.at(b));
    const WaveGreen::Term d = g.vectorTerm(), i = g.vectorImage();
    const double rho = std::hypot(c.midX.at(a) - c.midX.at(b), c.midY.at(a) - c.midY.at(b));
    Complex l = 2.0 * d.coefficient / kMu0 * direct + 2.0 * i.coefficient / kMu0 * image;
    // (A metal lid's image: far, a run's whole filament - in `image' already
    //  where it is folded into the ground's, cached.)
    if (const WaveGreen::Term lid = g.vectorLid(); !c.lidFolded && lid.coefficient != 0.0 && b < c.lidMirrored.size())
        l += 2.0 * lid.coefficient / kMu0 * alignedInductance(c.runs.at(a), c.lidMirrored.at(b));
    return l + c.runs.at(a).length() * c.runs.at(b).length() * smooth(c.hRun.at(a), c.hRun.at(b), rho);
}

Complex Kernels::smooth(int h0, int h1, double rho) const
{
    const WaveGreen& g = green(h0, h1);
    const WaveGreen::Term d = g.vectorTerm(), i = g.vectorImage(), lid = g.vectorLid();
    return d.coefficient / (2 * M_PI) * retardation(d.k, std::hypot(rho, d.dz))
           + i.coefficient / (2 * M_PI) * retardation(i.k, std::hypot(rho, i.dz))
           + (lid.coefficient != 0.0 ? lid.coefficient / (2 * M_PI) * retardation(lid.k, std::hypot(rho, lid.dz)) : Complex(0, 0))
           + g.vectorRemainder(rho);
}

Complex Kernels::inductance(int a, int b) const
{
    double direct = 0, image = 0;
    staticPair(c, a, b, &direct, &image);
    return inductance(a, b, direct, image);
}

Complex Kernels::potential(int i, int j, double pij, double pji) const
{
    // The static singular part over the cell in closed form, its
    // retardation at the centres, and the remainder integrated over the
    // charged cell - or at its centre where the cell is small beside the
    // distance the remainder changes over. Made symmetric: a potential at a
    // centre of a charge over a cell is not quite the other way round where
    // the cells differ in size, and the port matrix is reciprocal only if P
    // is.
    const WaveGreen& g = green(c.hNode.at(i), c.hNode.at(j));
    const WaveGreen::Term t = g.scalarTerm();
    const double rho = std::hypot(c.centre.at(i).x() - c.centre.at(j).x(), c.centre.at(i).y() - c.centre.at(j).y());
    const double scale = std::hypot(rho, g.reach());
    // (The retardation at the centres, and the remainder there where it is
    //  taken at them, the same both ways round: each made once.)
    const Complex retarded = retardation(t.k, std::hypot(rho, t.dz));
    Complex atCentres(0, 0);
    bool made = false;
    const auto oneWay = [this, &g, &t, &retarded, &atCentres, &made, rho, scale](int to, int from, double stat) {
        const Complex v = t.coefficient / (2 * M_PI) * (stat + retarded);
        const double longest = std::max(c.rect.at(from).width(), c.rect.at(from).height());
        const bool centred = to != from && longest * longest < kCentreBound * scale * scale;
        if (!centred) return v + remainderOver(g, c.rect.at(from), c.seenFrom(from, c.centre.at(to)), scale);
        if (!made) {
            atCentres = g.scalarRemainder(rho);
            made = true;
        }
        return v + atCentres;
    };
    const Complex base = i == j ? oneWay(i, i, pij) : 0.5 * (oneWay(i, j, pij) + oneWay(j, i, pji));
    Complex boxed = c.boxed ? base + boxPotential(i, j) : base;
    if (c.symAxis != 0) boxed += symmetryPotential(i, j);
    // (A correction held as its factors is no entry's: its entry is a sum
    //  over every panel, and a compressed fill and its frequencies asked for
    //  millions of them - all but the whole of a calibration strip's time
    //  over a layer that ends. P's products add it as U (V^T x) instead -
    //  addFactored() -, the near field's LU and a dense column take it apart.)
    return c.corrected && !c.dielectric.factored ? boxed + c.dielectric.at(i, j) : boxed;
}

Complex Kernels::symmetryPotential(int i, int j) const
{
    const WaveGreen& g = green(c.hNode.at(i), c.hNode.at(j));
    const WaveGreen::Term t = g.scalarTerm();
    // A charge's part mirrored: its rectangle in its own plane, and where it
    // stands (ChargePart).
    const auto mirrorPart = [this](ChargePart p) {
        const QPointF centre = c.symmetric(QPointF(p.x, p.y));
        p.x = centre.x();
        p.y = centre.y();
        const bool acrossX = c.symAxis == 1;
        // (Its rectangle's first coordinate: x in the cells' plane and on a
        //  wall along x, y on a wall along y; its second y, or z.)
        const bool firstMirrored = p.plane == 0 ? acrossX : p.plane == 1 ? acrossX : !acrossX;
        const bool secondMirrored = p.plane == 0 && !acrossX;
        if (firstMirrored) p.rect = QRectF(QPointF(2 * c.symAt - p.rect.right(), p.rect.top()), QSizeF(p.rect.width(), p.rect.height()));
        if (secondMirrored) p.rect = QRectF(QPointF(p.rect.left(), 2 * c.symAt - p.rect.bottom()), QSizeF(p.rect.width(), p.rect.height()));
        if ((p.plane == 1 && !acrossX) || (p.plane == 2 && acrossX)) p.at = 2 * c.symAt - p.at;
        return p;
    };
    const auto partsOf = [this](int k) {
        return k < c.charges.size() && !c.charges.at(k).isEmpty() ? c.charges.at(k) : QList<ChargePart>{wholeCell(c.rect.at(k), c.cellZ.at(k))};
    };
    const auto oneWay = [&](int to, int from) {
        const QPointF at = c.centre.at(to);
        const QRectF& own = c.rect.at(from);
        const QRectF image = QRectF(c.symmetric(own.topLeft()), c.symmetric(own.bottomRight())).normalized();
        const double rho = std::hypot(at.x() - image.center().x(), at.y() - image.center().y());
        const double scale = std::hypot(rho, g.reach());
        // (Its static part as a cell's pair's is - staticPotentialOf(): its
        //  edge's profile, or its walls', where either has one and they are
        //  near.)
        const bool partsTo = to < c.charges.size() && !c.charges.at(to).isEmpty();
        const bool partsFrom = from < c.charges.size() && !c.charges.at(from).isEmpty();
        const auto walled = [this](int k) {
            if (k >= c.charges.size()) return false;
            for (const ChargePart& p : c.charges.at(k))
                if (p.plane != 0) return true;
            return false;
        };
        double stat = rectanglePotential(image, at, t.dz);
        if (partsTo || partsFrom) {
            const double side = std::max({c.rect.at(to).width(), c.rect.at(to).height(), own.width(), own.height()});
            if (walled(to) || walled(from) || !(rho > 4 * side * (1 + 1e-6))) {
                QList<ChargePart> of;
                for (const ChargePart& p : partsOf(from)) of << mirrorPart(p);
                stat = partsPotential(partsOf(to), of);
            }
        }
        Complex v = t.coefficient / (2 * M_PI) * (stat + retardation(t.k, std::hypot(rho, t.dz)));
        const double longest = std::max(image.width(), image.height());
        v += longest * longest < kCentreBound * scale * scale ? g.scalarRemainder(rho) : remainderOver(g, image, at, scale);
        return c.symCharge * v;
    };
    return i == j ? oneWay(i, i) : 0.5 * (oneWay(i, j) + oneWay(j, i));
}

Complex Kernels::symmetryInductance(int a, int b) const
{
    // (Runs along x or y alone - fullWaveOf() refuses an oblique one -, so a
    //  run's mirror is along its own axis, and the cosine outside takes the
    //  directions; its sign is its current's: along the plane the charge's,
    //  across it the opposite. Its static part as the run's own pairs are,
    //  its parts and all: symmetryPair().)
    const bool alongX = std::abs(c.dirX.at(b)) > 0.5;
    const bool across = (c.symAxis == 1) == alongX;
    const double sign = across ? -c.symCharge : c.symCharge;
    const WaveGreen& g = green(c.hRun.at(a), c.hRun.at(b));
    const WaveGreen::Term d = g.vectorTerm(), gi = g.vectorImage(), lid = g.vectorLid();
    const auto oneWay = [&](int to, int from) {
        double direct = 0, image = 0;
        symmetryPair(c, to, from, &direct, &image);
        Complex l = 2.0 * d.coefficient / kMu0 * direct + 2.0 * gi.coefficient / kMu0 * image;
        if (!c.lidFolded && lid.coefficient != 0.0 && from < c.lidMirrored.size()) {
            Filament above = c.lidMirrored.at(from);
            above.from = c.symmetric(above.from);
            above.to = c.symmetric(above.to);
            l += 2.0 * lid.coefficient / kMu0 * alignedInductance(c.runs.at(to), above);
        }
        const QPointF mid = c.symmetric(QPointF(c.midX.at(from), c.midY.at(from)));
        const double rho = std::hypot(c.midX.at(to) - mid.x(), c.midY.at(to) - mid.y());
        return l + c.runs.at(to).length() * c.runs.at(from).length() * smooth(c.hRun.at(to), c.hRun.at(from), rho);
    };
    return sign * (a == b ? oneWay(a, a) : 0.5 * (oneWay(a, b) + oneWay(b, a)));
}

Complex Kernels::boxPotential(int i, int j) const
{
    const WaveGreen& g = green(c.hNode.at(i), c.hNode.at(j));
    const QList<GuideMode>& modes = boxScalar.at(pairOf.at(c.hNode.at(i) * c.nh + c.hNode.at(j)));
    const WaveGreen::Term t = g.scalarTerm();
    const auto oneWay = [&](int to, int from) {
        const QPointF at = c.centre.at(to);
        Complex sum(0, 0);
        for (const BoxImage& im : boxImagesOf(c.layers.box, c.rect.at(from), at, c.boxReach)) {
            const double rho = std::hypot(at.x() - im.rect.center().x(), at.y() - im.rect.center().y());
            const double scale = std::hypot(rho, g.reach());
            Complex v = t.coefficient / (2 * M_PI) * (rectanglePotential(im.rect, at, t.dz) + retardation(t.k, std::hypot(rho, t.dz)));
            const double longest = std::max(im.rect.width(), im.rect.height());
            v += longest * longest < kCentreBound * scale * scale ? g.scalarRemainder(rho) : remainderOver(g, im.rect, at, scale);
            for (const GuideMode& m : modes) v -= modeKernel(m, rho);
            sum += im.sign * v;
        }
        for (const GuideMode& m : modes)
            sum += 2.0 * m.kr * m.residue * rectangleGreen(m.kr, c.layers.box, at, c.centre.at(from), false, false);
        return sum;
    };
    return i == j ? oneWay(i, i) : 0.5 * (oneWay(i, j) + oneWay(j, i));
}

Complex Kernels::boxInductance(int a, int b) const
{
    const bool alongX = std::abs(c.dirX.at(a)) > 0.5;
    if (alongX != (std::abs(c.dirX.at(b)) > 0.5)) return Complex(0, 0);
    const double signX = alongX ? 1.0 : -1.0, signY = alongX ? -1.0 : 1.0;
    const WaveGreen& g = green(c.hRun.at(a), c.hRun.at(b));
    const QList<GuideMode>& modes = boxVector.at(pairOf.at(c.hRun.at(a) * c.nh + c.hRun.at(b)));
    const WaveGreen::Term d = g.vectorTerm(), gi = g.vectorImage(), lid = g.vectorLid();
    const double top = c.layers.height();
    const auto oneWay = [&](int to, int from) {
        const Filament& source = c.runs.at(from);
        const QRectF rect = QRectF(source.from, source.to).normalized();
        const QPointF at(c.midX.at(to), c.midY.at(to)), mid(c.midX.at(from), c.midY.at(from));
        const double lengths = c.runs.at(to).length() * source.length();
        Complex sum(0, 0);
        for (const BoxImage& im : boxImagesOf(c.layers.box, rect, at, c.boxReach, signX, signY)) {
            // The image run: its ends mapped as its rectangle is - its sign
            // the mirrors' (a current across a wall it runs into the same,
            // along one the opposite); its direction no matter, an aligned
            // partial inductance taking none (the cosine has it).
            const auto mapped = [&](const QPointF& p) {
                const double x = im.mirroredX ? im.rect.right() - (p.x() - rect.left()) : p.x() + (im.rect.left() - rect.left());
                const double y = im.mirroredY ? im.rect.bottom() - (p.y() - rect.top()) : p.y() + (im.rect.top() - rect.top());
                return QPointF(x, y);
            };
            Filament image = source;
            image.from = mapped(source.from);
            image.to = mapped(source.to);
            Filament below = image, above = image;
            below.z0 = below.z1 = -image.z0;
            above.z0 = above.z1 = 2 * top - image.z0;
            Complex l = 2.0 * d.coefficient / kMu0 * alignedInductance(c.runs.at(to), image)
                        + 2.0 * gi.coefficient / kMu0 * alignedInductance(c.runs.at(to), below);
            if (lid.coefficient != 0.0) l += 2.0 * lid.coefficient / kMu0 * alignedInductance(c.runs.at(to), above);
            const double rho = std::hypot(at.x() - im.rect.center().x(), at.y() - im.rect.center().y());
            Complex rest = smooth(c.hRun.at(to), c.hRun.at(from), rho);
            for (const GuideMode& m : modes) rest -= modeKernel(m, rho);
            sum += im.sign * (l + lengths * rest);
        }
        for (const GuideMode& m : modes)
            sum += lengths * 2.0 * m.kr * m.residue * rectangleGreen(m.kr, c.layers.box, at, mid, alongX, !alongX);
        return sum;
    };
    return a == b ? oneWay(a, a) : 0.5 * (oneWay(a, b) + oneWay(b, a));
}

Complex Kernels::potential(int i, int j) const
{
    return potential(i, j, staticPotential(c, i, j), i == j ? 0.0 : staticPotential(c, j, i));
}

Complex Kernels::potentialOf(const Via& via, int cell) const
{
    if (c.symAxis == 0) return potentialOfOne(via, cell);
    Via image = via;
    image.at = c.symmetric(via.at);
    return potentialOfOne(via, cell) + c.symCharge * potentialOfOne(image, cell);
}

Complex Kernels::couplingOf(const Via& a, const Via& b) const
{
    if (c.symAxis == 0) return couplingOfOne(a, b);
    Via image = b;
    image.at = c.symmetric(b.at);
    return couplingOfOne(a, b) + c.symCharge * couplingOfOne(a, image);
}

Complex Kernels::potentialOfOne(const Via& via, int cell) const
{
    // Nowhere nearer the via than its cross-section's mean distance with
    // itself: the dielectric's part of its own G_zz, taken across that
    // distance, is what its potential is there (to a part in a thousand
    // through 200 µm of 9.8, against 5 % at its axis), and the two must
    // cancel round a path for its inductance to be the vacuum's, as it is.
    const double apart = std::hypot(c.centre.at(cell).x() - via.at.x(), c.centre.at(cell).y() - via.at.y());
    return viaGreens.at(via.span).potential(c.hNode.at(cell), std::max(apart, via.gmd));
}

Complex Kernels::couplingOfOne(const Via& a, const Via& b) const
{
    // G_zz along both - the closed forms between their axes, statically in
    // closed form and their retardation by Gauss's rule - and the table's
    // rest.
    const ViaGreen& green = viaGreens.at(a.span);
    const double apart = std::hypot(a.at.x() - b.at.x(), a.at.y() - b.at.y());
    const double gmd = std::max(a.gmd, b.gmd);
    Complex v(0, 0);
    for (const ViaGreen::Term& t : green.couplingTerms(b.span)) {
        if (t.coefficient == Complex(0, 0)) continue;
        const double near = viaPair(t.a0, t.a1, t.b0, t.b1, apart, a.side, b.side, gmd);
        const Complex late = retardedPair(t.k, t.a0, t.a1, t.b0, t.b1, std::max(apart, gmd));
        v += t.coefficient * (near + late) / (2 * M_PI);
    }
    v += green.couplingRemainder(b.span, apart);
    return v;
}

/*!
 * What each run's own metal adds to the system at frequency \a f, each
 * term as \a add(row, column, impedance): a sheet's surface impedance, and
 * more along its edges; a face of a metal solved as its two, its own
 * slab's and its twin's through the metal; a via's, round its four faces.
 */
/*!
 * A metal or impedance ground's loss, in the full wave as the quasi-static
 * extraction counts it: the plane is a perfect one to the fields, and loses
 * on the current it carries - the runs' mirror images'. A line's wave is
 * quasi-TEM: at each cross-section the plane carries what the strip's own
 * current there has it carry, spread across as a 2D solve spreads it - the
 * Poisson kernel (H / pi) / (H^2 + s^2), H = h_a + h_b, s apart across the
 * line -, and nothing along it. So two runs share Rs d_a.d_b / (w_a w_b)
 * times how far they run side by side times that kernel over their two
 * widths, in closed form: Phi(s) = (s atan(s / H) - H/2 ln(s^2 + H^2)) / pi
 * at the four corners. (The 3D static kernel, taken along the line too,
 * lost what a wave's phase along it makes cancel, e^(-k h) at k = beta: on
 * 100 um of alumina it kept 78 % of the ground's loss at 50 GHz, on 203 um
 * of laminate 65 %.)
 * Pairs within ten H across: beyond, the kernel holds 6 % of the current
 * but 0.04 % of its loss, which goes as its square.
 */
void groundPairsOf(Context& c)
{
    struct Rect {
        QPointF o, u, v;   // m: its start, along it, across it
        double l = 0, w = 0, z = 0;
    };
    QList<Rect> rects;
    QList<int> which;
    double zMost = 0;
    for (int a = 0; a < c.planar; ++a) {
        const Filament& run = c.runs.at(a);
        const double l = run.length();
        if (!(run.width > 0) || run.rise > 0 || !(l > 0) || !(run.z0 > 0)) continue;
        Rect r;
        r.o = run.from;
        r.u = (run.to - run.from) / l;
        r.v = QPointF(-r.u.y(), r.u.x());
        r.l = l;
        r.w = run.width;
        r.z = run.z0;
        rects << r;
        which << a;
        zMost = std::max(zMost, run.z0);
    }
    if (rects.isEmpty()) return;
    const double reach = 20 * zMost;   // (ten of the largest H)
    // (Binned by the reach, so that each run meets only the bins beside its own.)
    QHash<qint64, QList<int>> bins;
    const auto key = [](qint64 x, qint64 y) { return (x << 32) ^ qint64(std::uint32_t(qint32(y))); };
    QList<QPointF> mids;
    double longest = 0;
    for (int i = 0; i < rects.size(); ++i) {
        const Rect& r = rects.at(i);
        mids << r.o + r.u * (0.5 * r.l);
        bins[key(qint64(std::floor(mids.last().x() / reach)), qint64(std::floor(mids.last().y() / reach)))] << i;
        longest = std::max(longest, r.l);
    }
    const int around = 1 + int(std::ceil(longest / reach));
    const auto phi = [](double s, double h) { return (s * std::atan(s / h) - 0.5 * h * std::log(s * s + h * h)) / M_PI; };
    for (int i = 0; i < rects.size(); ++i) {
        const Rect& ri = rects.at(i);
        const qint64 bx = qint64(std::floor(mids.at(i).x() / reach)), by = qint64(std::floor(mids.at(i).y() / reach));
        for (qint64 x = bx - around; x <= bx + around; ++x)
            for (qint64 y = by - around; y <= by + around; ++y) {
                const auto it = bins.constFind(key(x, y));
                if (it == bins.constEnd()) continue;
                for (const int j : it.value()) {
                    if (j < i) continue;
                    const Rect& rj = rects.at(j);
                    const double along = ri.u.x() * rj.u.x() + ri.u.y() * rj.u.y();
                    if (std::abs(along) < 1e-6) continue;   // (runs across one another share nothing)
                    // How far they run side by side, along i, and how far
                    // apart across it.
                    const QPointF a0 = rj.o - ri.o, a1 = rj.o + rj.u * rj.l - ri.o;
                    const double s0 = a0.x() * ri.u.x() + a0.y() * ri.u.y(), s1 = a1.x() * ri.u.x() + a1.y() * ri.u.y();
                    const double side = std::min(ri.l, std::max(s0, s1)) - std::max(0.0, std::min(s0, s1));
                    if (!(side > 0)) continue;
                    const QPointF m = mids.at(j) - ri.o;
                    const double across = m.x() * ri.v.x() + m.y() * ri.v.y(), h = ri.z + rj.z;
                    if (std::abs(across) > 10 * h + 0.5 * (ri.w + rj.w)) continue;
                    const double i1 = -0.5 * ri.w, i2 = 0.5 * ri.w, j1 = across - 0.5 * rj.w, j2 = across + 0.5 * rj.w;
                    const double over = phi(i2 - j1, h) - phi(i1 - j1, h) - phi(i2 - j2, h) + phi(i1 - j2, h);
                    const double k = along * side * over / (ri.w * rj.w);
                    if (k == 0) continue;
                    c.groundPairs << Context::GroundPair{which.at(i), which.at(j), k};
                    if (j != i) c.groundPairs << Context::GroundPair{which.at(j), which.at(i), k};
                }
            }
    }
}

/// The ground's own share of the run pairs' impedance (groundPairsOf()), at
/// \a f: its surface impedance times what each pair shares.
template <typename Add> void groundImpedances(const Context& c, double f, Add add)
{
    if (c.groundPairs.isEmpty() || !(f > 0)) return;
    Complex zs(c.groundImpedance, 0);
    if (c.groundImpedance < 0) {
        const double rs = std::sqrt(M_PI * f * kMu0 / c.groundSigma);
        zs = Complex(rs, rs);   // (a half space of the metal: its internal inductance too)
    }
    for (const Context::GroundPair& g : c.groundPairs) add(g.a, g.b, zs * g.k);
}

template <typename Add> void ownImpedances(const Context& c, double f, Add rawAdd)
{
    for (int a = 0; a < c.planar; ++a) {
        const Filament& run = c.runs.at(a);
        if (!(run.width > 0)) continue;
        // Its metal's roughness: every term of its own surface impedance -
        // its slab's, its twin's through the metal, its edge's and its walls'
        // - the longer path the current takes along a rough surface, as the
        // quasi-static solve has it in its loss (roughnessFactor()). The full
        // wave had none, its loss a smooth metal's whatever the node said
        const double rough = run.nodule > 0 ? hurayFactor(run.nodule, run.surfaceRatio, run.sigma, f)
                                            : roughnessFactor(run.roughness, run.sigma, f);
        const auto add = [&rawAdd, rough](int i, int j, Complex v) { rawAdd(i, j, rough * v); };
        const Complex sheet = sheetImpedance(run.sigma, run.thickness, f);
        const double squares = run.length() / run.width;
        const int twin = c.twin.value(a, -1);
        const double crowd = crowding(run.width, run.thickness, run.sigma, c.roundRuns.value(a) ? 0 : c.freeSides.value(a), f, sheet, twin >= 0);
        if (twin < 0) {
            add(a, a, crowd * sheet * squares);
            continue;
        }
        // A face of a metal solved as its two: its own slab's impedance,
        // and its twin's current's through the metal (faceImpedances());
        // and what an edge loses beyond its cells' average, of the two
        // faces' current together - its walls' and corners', within the
        // metal's thickness of the edge, where the two faces' current
        // crowds as one - its reactance its resistance, a thick metal's.
        Complex own, across;
        faceImpedances(run.sigma, run.thickness, f, &own, &across);
        // The faces' even current's inside, the slab's, held to its
        // section's (Context::inside): never more than its share k of the
        // slab's at a low frequency, X0 = omega mu0 t/6 a square for the two
        // faces' even mode, nor than the slab's own at the frequency,
        // X' = min(X, k X0). The slab's falls once its skins part, and
        // where it falls under k X0 - some 3/k skin depths thick - the
        // current is on the faces' skins, whose reactance is a narrow
        // conductor's as it is a wide one's. A square loop of wire took a
        // wide slab's inside, six times its own beyond its faces' field, and
        // read the more the finer its cells across. Both non-increasing in X/omega, so is their least:
        // the share faded as 1/(1 + (t/2 delta)^4), from whole to nought
        // between one skin depth and three, went faster than the slab's own
        // reactance fell, and gave back what it took - the loop's L rose 6 %
        // over its value at direct current between 40 and 300 kHz, which no
        // conductor's can.
        if (const double k = c.inside.value(a, 1.0); k < 1 && run.sigma > 0) {
            const double even = (own + across).imag(), held = k * 2 * M_PI * f * kMu0 * run.thickness / 6;
            if (even > held) {
                const Complex shift(0, -0.5 * (even - held));
                own += shift;
                across += shift;
            }
        }
        const QList<Context::Part>& parts = c.parts.value(a);
        if (!parts.isEmpty()) {
            // A run along an edge that carries current down its wall too
            // (wallsOf()): each part its own loss on its share - the face's
            // strip its slab's, and its twin's through the metal on both
            // strips' shares; each half wall a surface the metal's breadth
            // behind it deep -, and no edge factor, whose crowding the walls
            // now are.
            // (Its face's strips - the edge's profile - each its slab's on its
            //  share, its twin's alike through the metal strip by strip; its
            //  walls, those with a rise.)
            const QList<Context::Part>& theirs = c.parts.value(twin);
            Complex wallOwn, wallAcross;
            faceImpedances(run.sigma, run.width, f, &wallOwn, &wallAcross);
            Complex self(0, 0), mutual(0, 0);
            int walls = 0;
            for (int p = 0; p < parts.size(); ++p) {
                const Context::Part& part = parts.at(p);
                if (part.run.rise > 0) {
                    self += wallOwn * (run.length() / part.run.rise) * part.weight * part.weight;
                    ++walls;
                    continue;
                }
                self += own * (run.length() / part.run.width) * part.weight * part.weight;
                const double its = p < theirs.size() && theirs.at(p).run.rise <= 0 ? theirs.at(p).weight : part.weight;
                mutual += across * (run.length() / part.run.width) * part.weight * its;
            }
            // Its parts' current is even across each; the edge's is not, and
            // loses more: the band of the edge's cross-section the run is -
            // its face's strip from the edge, its half of the wall -, solved
            // (rf25_edge.h), loses what its rectangles do. The run's own
            // resistance is made what leaves the band's loss once its twin's
            // share through the metal is taken (one wall: a strip a cell wide
            // with both its walls is left even).
            if (walls == 1 && run.sigma > 0) {
                const auto edge = edgeSolution(run.sigma, run.thickness, f);
                if (edge && edge->ok()) {
                    const EdgeBand band = bandOf(*edge, 0, run.width);
                    const double carried = std::norm(band.current);
                    // (The run's resistance R with R |I|^2 + Re(mutual) |I|^2
                    //  the band's loss over the run's length, I the band's
                    //  current, its twin's alike.)
                    const double r = carried > 0 ? band.loss * run.length() / carried - mutual.real() : 0;
                    if (r > 0) self = Complex(r, self.imag());
                }
            }
            add(a, a, self);
            add(a, twin, mutual);
            continue;
        }
        const Complex edge = (crowd - 1) * sheet.real() * Complex(1, 1);
        add(a, a, (own + edge) * squares);
        add(a, twin, (across + edge) * squares);
    }
    // A via's own: a bar whose current runs on its four faces, each with the
    // surface impedance of a quarter of its depth - 1/(sigma area) at direct
    // current, (1 + j)/(sigma delta) round its perimeter once the skin is
    // thinner.
    for (const Via& v : c.vias) {
        const Filament& run = c.runs.at(v.run);
        if (v.side > 0 && run.sigma > 0) rawAdd(v.run, v.run, surfaceImpedance(run.sigma, v.side / 4, f) * run.length() / (4 * v.side));
    }
}

/*!
 * What the ports' own paths add at \a k's frequency (PortPath), each a via
 * whose current is its port's: to the runs' drive \a rhs, each path's
 * potential along every run in the plane - its head's less its tail's,
 * which by the formulation's reciprocity is also what the run's vector
 * potential does along the path (G_zx) - and every via's G_zz along the
 * path; and to \a own, the columns' own impedances, each two paths' G_zz
 * and their potentials at each other's ends, the two ways round averaged.
 * Between a via and a path the coupling is the one way round whole: the
 * via's G_zz along the path and its potential at the path's ends, which
 * the drive has already (portVia). G_zz goes as 1/eps of its source's slab,
 * and only with the other's potential is it the same both ways - half the
 * G_zz each way and the potential one way gave back 1e-6 of a wave at 5
 * GHz, a via between two dielectrics and a port's path through the other.
 * Without them a port's charge had its potential but the current that
 * brings it up had no field, and the scalar potential alone takes a
 * vertical current's radiation the wrong way: a strip on 635 µm of alumina
 * gave back twice what it was given at 41.5 GHz, one over vacuum 3.9 times
 * at 64 GHz (G9 of the phase E check).
 */
void addPortPaths(const Context& c, const Kernels& k, MatrixC& rhs, MatrixC& own)
{
    const int count = int(c.paths.size());
    VectorC at(c.nc);
    for (const PortPath& path : c.paths) {
        for (int i = 0; i < c.nc; ++i) at[i] = k.potentialOf(path.via, i);
        for (int a = 0; a < c.planar; ++a) rhs(a, path.column) -= path.current * (at[c.head.at(a)] - at[c.tail.at(a)]);
        for (const Via& via : c.vias) rhs(via.run, path.column) -= path.current * k.couplingOf(path.via, via);
    }
    // (Path \a from's potential at path \a to's ends.)
    const auto along = [&c, &k](int from, int to) {
        Complex sum(0, 0);
        for (const QPair<int, double>& end : c.paths.at(to).ends) sum += end.second * k.potentialOf(c.paths.at(from).via, end.first);
        return sum;
    };
    for (int i = 0; i < count; ++i)
        for (int j = i; j < count; ++j) {
            const PortPath& a = c.paths.at(i);
            const PortPath& b = c.paths.at(j);
            const Complex v = a.current * b.current * 0.5
                              * (k.couplingOf(a.via, b.via) + k.couplingOf(b.via, a.via) + along(i, j) + along(j, i));
            own(a.column, b.column) += v;
            if (i != j) own(b.column, a.column) += v;
        }
}

/*!
 * What the ports' own currents add at \a k's frequency where they come up
 * by no path (SourceRun): the radiating part of each leg's vector potential
 * (Kernels::smooth()) along every run in the plane - the run's cosine with
 * the leg times both lengths and the kernel averaged along the leg by
 * Gauss's rule - into the drive \a rhs, as each column carries it along its
 * legs (Context::sourceOf) - the same, by reciprocity, the column's voltage
 * of the runs' currents -; and each two legs' alike into \a own, the
 * columns' own impedances.
 */
void addSourceFields(const Context& c, const Kernels& k, MatrixC& rhs, MatrixC& own)
{
    const int legs = int(c.sources.size());
    if (legs == 0) return;
    // (Of a leg and what it is seen at \a rho from: Gauss's points along it,
    //  as many as a kernel that changes over the distance - or the
    //  remainder's reach, or the wavelength, where shorter - wants.)
    const auto pointsOf = [&k](const SourceRun& s, int h, double rho) {
        const WaveGreen& g = k.green(h, s.height);
        const double wave = 1.0 / std::max(1e-300, std::abs(g.vectorTerm().k));
        return pointsAlong(s.length, std::min(std::hypot(rho, g.reach()), wave));
    };
    const auto along = [](const SourceRun& s, double t) {
        return QPointF(s.from.x() + (t + 0.5) * (s.to.x() - s.from.x()), s.from.y() + (t + 0.5) * (s.to.y() - s.from.y()));
    };
    MatrixC fields = MatrixC::Zero(rhs.rows(), legs), between = MatrixC::Zero(legs, legs);
    for (int l = 0; l < legs; ++l) {
        const SourceRun& s = c.sources.at(l);
        const QPointF middle = along(s, 0);
        for (int a = 0; a < c.planar; ++a) {
            const double dot = c.dirX.at(a) * s.dirX + c.dirY.at(a) * s.dirY;
            if (std::abs(dot) < 1e-9) continue;
            const int h = c.hRun.at(a);
            const Rule& r = gaussRule(pointsOf(s, h, std::hypot(c.midX.at(a) - middle.x(), c.midY.at(a) - middle.y())));
            Complex sum(0, 0);
            for (int i = 0; i < r.at.size(); ++i) {
                const QPointF at = along(s, r.at.at(i));
                sum += r.weight.at(i) * k.smooth(h, s.height, std::hypot(c.midX.at(a) - at.x(), c.midY.at(a) - at.y()));
            }
            fields(a, l) = k.jw * dot * c.runs.at(a).length() * s.length * sum;
        }
    }
    for (int i = 0; i < legs; ++i)
        for (int j = i; j < legs; ++j) {
            const SourceRun& a = c.sources.at(i);
            const SourceRun& b = c.sources.at(j);
            const double dot = a.dirX * b.dirX + a.dirY * b.dirY;
            if (std::abs(dot) < 1e-9) continue;
            const QPointF ma = along(a, 0), mb = along(b, 0);
            const double apart = std::hypot(ma.x() - mb.x(), ma.y() - mb.y());
            const Rule& r = gaussRule(std::max(pointsOf(a, b.height, apart), pointsOf(b, a.height, apart)));
            Complex sum(0, 0);
            for (int p = 0; p < r.at.size(); ++p)
                for (int q = 0; q < r.at.size(); ++q) {
                    const QPointF x = along(a, r.at.at(p)), y = along(b, r.at.at(q));
                    sum += r.weight.at(p) * r.weight.at(q) * k.smooth(a.height, b.height, std::hypot(x.x() - y.x(), x.y() - y.y()));
                }
            between(i, j) = between(j, i) = k.jw * dot * a.length * b.length * sum;
        }
    const MatrixC of = c.sourceOf.cast<Complex>();
    rhs -= fields * of;
    own += of.transpose() * between * of;
}

/// \a job(i) for each i below \a count, each on one of \a threads threads,
/// as they come free, and on no other: in turn from nought. Each done on a
/// thread other than the caller's is counted in \a counted (fillShared()).
void sharedOut(int count, int threads, const std::function<void(int)>& job,
               std::atomic<long long>& counted = sharedColumns)
{
    std::atomic_int next{0};
    const int n = std::clamp(threads, 1, std::max(1, count));
    // (Dealt in turn where a test asks: sharesDealt().)
    const bool dealt = sharesDealt();
    onThreads(n, [&next, &job, &counted, count, n, dealt](int t) {
        for (int i = dealt ? t : next++; i < count; i = dealt ? i + n : next++) {
            if (t > 0) ++counted;
            job(i);
        }
    });
}

/*!
 * Column \a b of a frequency's system M, its part above the diagonal and on
 * it, the charges' part apart (chargeColumn()): a run in the plane's jw L
 * with the runs in the plane before it; a via's coupling to every run in
 * the plane and to the vias up to itself. (A via's run is the runs in the
 * plane's count and its own index: fullWaveOf() numbers them so.)
 */
void systemColumn(const Context& c, const Kernels& k, MatrixC& m, int b)
{
    const int np0 = c.planar;
    if (b < np0) {
        // jw L: each two runs' cosine times their kernel - nothing between
        // runs at right angles, which is what makes an axis-aligned layout
        // cheap.
        for (int a = 0; a <= b; ++a) {
            const double dot = k.cosine(a, b);
            m(a, b) = std::abs(dot) < 1e-9 ? Complex(0, 0) : k.jw * (dot * k.inductance(a, b, c.lDirect(a, b), c.lImage(a, b)));
            if (c.boxed && std::abs(dot) >= 1e-9) m(a, b) += k.jw * (dot * k.boxInductance(a, b));
            if (c.symAxis != 0 && std::abs(dot) >= 1e-9) m(a, b) += k.jw * (dot * k.symmetryInductance(a, b));
        }
        return;
    }
    const int j = b - np0;
    Q_ASSERT(c.vias.at(j).run == b);
    // A run in the plane and a via: the via's potential at the run's head
    // less at its tail - which is also what the run's vector potential does
    // along the via (G_zx), by the formulation's reciprocity, both ways.
    for (int a = 0; a < np0; ++a) m(a, b) = k.viaPotential(j, c.head.at(a)) - k.viaPotential(j, c.tail.at(a));
    // Between two vias: G_zz along both, and the scalar correction of the
    // one's current at the other's two cells, ended by the ground's nought
    // for a via to it. Its potential is taken at the cells, as the node
    // potentials are, so that what a via's current adds along the runs that
    // carry it away cancels what it adds along the via itself; and the two
    // ways round averaged, which differ by where a via stands in its cell.
    const auto onto = [&c, &k](int i, int from) {
        const int head = c.head.at(c.vias.at(i).run), tail = c.tail.at(c.vias.at(i).run);
        Complex v = k.viaCoupling(i, from);
        v += k.viaPotential(from, head);
        if (tail >= 0) v -= k.viaPotential(from, tail);
        return v;
    };
    for (int i = 0; i <= j; ++i) {
        const Complex ij = onto(i, j);
        m(c.vias.at(i).run, b) = 0.5 * (ij + (i == j ? ij : onto(j, i)));
    }
}

/// Column \a j of P, its part above the diagonal and on it: each cell's
/// potential of a unit charge spread over another.
void potentialColumn(const Context& c, const Kernels& k, MatrixC& p, int j)
{
    for (int i = 0; i <= j; ++i) p(i, j) = k.potential(i, j, c.pStatic(i, j), c.pStatic(j, i));
    // (A correction held as its factors, its column in one product.)
    if (c.corrected && c.dielectric.factored) {
        const Eigen::VectorXd column = c.dielectric.u.topRows(j + 1) * c.dielectric.v.row(j).transpose();
        for (int i = 0; i <= j; ++i) p(i, j) += column[i];
    }
}

/// \a y += the correction of the layers that end, held as its factors, times
/// \a x: U (V^T x), which Kernels::potential() leaves out. Nothing where it
/// is held whole (its entries in P's) or there is none.
void addFactored(const Context& c, const VectorC& x, VectorC& y)
{
    if (!c.corrected || !c.dielectric.factored) return;
    const Eigen::VectorXd re = c.dielectric.v.transpose() * x.real(), im = c.dielectric.v.transpose() * x.imag();
    y.real() += c.dielectric.u * re;
    y.imag() += c.dielectric.u * im;
}

/// The columns a thread makes below the diagonal at a time
/// (mirrorColumns()): each of their rows above is read in one piece.
constexpr int kMirrorBlock = 16;

/// A symmetric matrix's columns from \a b0 to before \a b1, below their
/// diagonal, made their rows' above it.
template <typename Matrix> void mirrorColumns(Matrix& x, int b0, int b1)
{
    for (int a = b0 + 1; a < x.rows(); ++a)
        for (int b = b0; b < std::min(b1, a); ++b) x(a, b) = x(b, a);
}

/// Column \a b of M: the charges eliminated, A^T P A / jw. (A via to the
/// ground has no tail: the ground takes its charge, at nought.)
void chargeColumn(const Context& c, const MatrixC& p, Complex jw, MatrixC& m, int b)
{
    const auto pAt = [&p](int i, int j) { return i < 0 || j < 0 ? Complex(0, 0) : p(i, j); };
    const int tail = c.tail.at(b), head = c.head.at(b);
    for (int a = 0; a < c.nr; ++a)
        m(a, b) += overJw(pAt(c.tail.at(a), tail) - pAt(c.tail.at(a), head) - pAt(c.head.at(a), tail) + pAt(c.head.at(a), head), jw);
}

/// Column \a b of a dense solve's static partial inductances, direct and of
/// the images (Context::lDirect and lImage), its part above the diagonal and
/// on it: each run's with the runs before it, nought at right angles.
void staticInductanceColumn(const Context& c, Eigen::MatrixXd& direct, Eigen::MatrixXd& image, int b)
{
    for (int a = 0; a <= b; ++a) {
        const bool crossed = std::abs(c.dirX.at(a) * c.dirX.at(b) + c.dirY.at(a) * c.dirY.at(b)) < 1e-9;
        if (crossed)
            direct(a, b) = image(a, b) = 0.0;
        else
            staticPair(c, a, b, &direct(a, b), &image(a, b));
    }
}

/// Column \a j of a dense solve's static potentials (Context::pStatic),
/// whole: each cell's of a unit charge spread over cell j - not cell j's of
/// one spread over it, where the two differ in size.
/*!
 * Cell \a i's static potential of a unit charge spread over cell \a j:
 * rectanglePotential() of j's rectangle at i's centre - over their parts
 * where either has them (Context::charges), each part's potential at each
 * of i's parts' centres by their shares.
 */
/// Whether cells \a i and \a j's static potential is taken over their
/// charges' parts: either has parts, and either has a wall or they are near
/// (staticPotentialOf()).
bool dearPotential(const Context& c, int i, int j)
{
    const bool partsI = i < c.charges.size() && !c.charges.at(i).isEmpty();
    const bool partsJ = j < c.charges.size() && !c.charges.at(j).isEmpty();
    if (!partsI && !partsJ) return false;
    const auto walled = [&c](int k) {
        for (const ChargePart& p : c.charges.at(k))
            if (p.plane != 0) return true;
        return false;
    };
    if ((partsI && walled(i)) || (partsJ && walled(j))) return true;
    const double side = std::max({c.rect.at(i).width(), c.rect.at(i).height(), c.rect.at(j).width(), c.rect.at(j).height()});
    const double apart = std::hypot(c.centre.at(i).x() - c.centre.at(j).x(), c.centre.at(i).y() - c.centre.at(j).y());
    return !(apart > 4 * side * (1 + 1e-6));
}

/// Cell \a i's potential of a unit charge spread over cell \a j, at i's
/// centre - or \a shift (m) from it along i's longest side, in i's own
/// frame, its parts moved with it.
double staticPotentialAt(const Context& c, int i, int j, double shift)
{
    const bool partsI = i < c.charges.size() && !c.charges.at(i).isEmpty();
    const bool partsJ = j < c.charges.size() && !c.charges.at(j).isEmpty();
    const bool alongX = c.rect.at(i).width() >= c.rect.at(i).height();
    const double turn = i < c.angle.size() ? c.angle.at(i) : 0.0;
    const QPointF own = alongX ? QPointF(shift, 0) : QPointF(0, shift);
    const QPointF moved = turn == 0 ? own : QPointF(own.x() * std::cos(turn) - own.y() * std::sin(turn),
                                                   own.x() * std::sin(turn) + own.y() * std::cos(turn));
    // (Seen from j: a strip's cell's rect is of its own frame - Context::angle.)
    const QPointF seen = c.seenFrom(j, c.centre.at(i) + moved);
    if (!partsI && !partsJ) return rectanglePotential(c.rect.at(j), seen, std::abs(c.cellZ.at(i) - c.cellZ.at(j)));
    // (A charge spread only by its edge's profile, far away, is the cell's:
    //  its parts within four of the longest side of either, as the
    //  quasi-static solve judges a near pair. Walls are taken at every
    //  distance: two facing across a gap are many cells apart.)
    const auto walled = [&c](int k) {
        for (const ChargePart& p : c.charges.at(k))
            if (p.plane != 0) return true;
        return false;
    };
    // (Where a layer ends, the parts' change by the singular term's strength
    //  where the cells are over the background's: Context::profileScale.)
    const auto scaledParts = [&c, i, j](double plain, double parts) {
        if (c.profileScale.isEmpty()) return parts;
        return plain + 0.5 * (c.profileScale.at(i) + c.profileScale.at(j)) * (parts - plain);
    };
    if (!(partsI && walled(i)) && !(partsJ && walled(j))) {
        const double side = std::max({c.rect.at(i).width(), c.rect.at(i).height(), c.rect.at(j).width(), c.rect.at(j).height()});
        const double apart = std::hypot(c.centre.at(i).x() - c.centre.at(j).x(), c.centre.at(i).y() - c.centre.at(j).y());
        // (A margin past the cut, so that two pairs alike - a symmetric
        //  layout's mirror images - are taken alike: on an even grid their
        //  distances fall on the cut, a rounding either side.)
        if (apart > 4 * side * (1 + 1e-6)) return rectanglePotential(c.rect.at(j), seen, std::abs(c.cellZ.at(i) - c.cellZ.at(j)));
    }
    // (A strip's cell's parts are of its own frame: i's seen from j's.)
    QList<ChargePart> at = partsI ? c.charges.at(i) : QList<ChargePart>{wholeCell(c.rect.at(i), c.cellZ.at(i))};
    if (shift != 0) at = movedBy(at, own.x(), own.y());
    const double parts = partsPotential(c.partsSeenFrom(j, i, at),
                                        partsJ ? c.charges.at(j) : QList<ChargePart>{wholeCell(c.rect.at(j), c.cellZ.at(j))});
    if (c.profileScale.isEmpty()) return parts;
    return scaledParts(rectanglePotential(c.rect.at(j), seen, std::abs(c.cellZ.at(i) - c.cellZ.at(j))), parts);
}

double staticPotentialOf(const Context& c, int i, int j)
{
    // A near pair of cells of unlike lengths - a long cell beside a gap and
    // the shorter ones beside it -: the larger's mean potential of the
    // smaller's charge, along its length by the midpoint rule, the same
    // both ways round. Its potential at its centre alone does not stand for
    // a long cell, and the solve's mean of the two ways, made symmetric, took
    // it for one: a coplanar line's gap cells 0.31 µm wide and 143 µm long
    // read eps_eff 1.12 in air as a sheet, and 1.37 as its two faces, where
    // it is 1 (P1 of the open items of 8 November). This is Galerkin's
    // entry, nearly - symmetric and positive, as P must be for a passive
    // solve -, where the centre taken of the shorter cell alone left
    // solves that gave power.
    // (Weighed in from twice the length to two and a half times, and the
    //  midpoints' count blended between whole numbers, so that it moves
    //  with the cells smoothly: a shape drawn whole and as pieces, its
    //  outline a nanometre apart, read Z 2e-6 apart at a sharp cut, where
    //  it is the same to 1e-7.)
    const double here = staticPotentialAt(c, i, j, 0);
    if (i == j) return here;
    const double si = std::max(c.rect.at(i).width(), c.rect.at(i).height());
    const double sj = std::max(c.rect.at(j).width(), c.rect.at(j).height());
    const double shorter = std::min(si, sj), length = std::max(si, sj);
    const double apart = std::hypot(c.centre.at(i).x() - c.centre.at(j).x(), c.centre.at(i).y() - c.centre.at(j).y());
    const double weight = shorter > 0 ? std::clamp((length / shorter - 2) / 0.5, 0.0, 1.0) : 0.0;
    if (!(weight > 0) || apart > 4 * length * (1 + 1e-6)) return here;
    const int big = si > sj ? i : j, small = big == i ? j : i;
    const auto midpoints = [&](int n) {
        double sum = 0;
        for (int k = 0; k < n; ++k) sum += staticPotentialAt(c, big, small, (-0.5 + (k + 0.5) / n) * length);
        return sum / n;
    };
    const double count = std::clamp(2 * length / shorter, 2.0, 32.0);
    const int n = int(std::floor(count));
    const double part = count - n;
    const double mean = part > 0 && n < 32 ? (1 - part) * midpoints(n) + part * midpoints(n + 1) : midpoints(n);
    return (1 - weight) * here + weight * mean;
}

double staticPotential(const Context& c, int i, int j)
{
    const qint64 at = c.cellStatics.find(i, j);
    return at >= 0 ? c.cellStatics.first[std::size_t(at)] : staticPotentialOf(c, i, j);
}

/*!
 * Each two of the items within the larger's reach of each other - item i
 * at (\a x[i], \a y[i]), reaching \a reach[i] - for which \a keep holds,
 * both ways round and each with itself, each row's in order: found from the
 * one of the two that reaches the further, in bins of the plane as wide as
 * the middle reach, on \a threads threads.
 */
std::vector<std::vector<int>> pairsWithin(const std::vector<double>& x, const std::vector<double>& y,
                                          const std::vector<double>& reach, int threads,
                                          const std::function<bool(int, int)>& keep)
{
    const int n = int(x.size());
    std::vector<std::vector<int>> rows(static_cast<std::size_t>(n));
    if (n == 0) return rows;
    std::vector<double> middle = reach;
    std::nth_element(middle.begin(), middle.begin() + n / 2, middle.end());
    const double bin = std::max(middle[std::size_t(n / 2)], 1e-12);
    const auto binOf = [bin](double v) { return qint64(std::floor(v / bin)); };
    QHash<qint64, std::vector<int>> bins;
    for (int i = 0; i < n; ++i) bins[(binOf(x[std::size_t(i)]) << 32) ^ (binOf(y[std::size_t(i)]) & 0xffffffffLL)].push_back(i);
    // (Each pair found once, from the one reaching the further - or, alike,
    //  the later -, into that one's own list; then both ways round.)
    std::vector<std::vector<int>> found(static_cast<std::size_t>(n));
    sharedOut(n, threads, [&](int b) {
        const double r = reach[std::size_t(b)], xb = x[std::size_t(b)], yb = y[std::size_t(b)];
        std::vector<int>& mine = found[std::size_t(b)];
        for (qint64 i = binOf(xb - r); i <= binOf(xb + r); ++i)
            for (qint64 j = binOf(yb - r); j <= binOf(yb + r); ++j) {
                const auto in = bins.constFind((i << 32) ^ (j & 0xffffffffLL));
                if (in == bins.constEnd()) continue;
                for (const int a : *in) {
                    const double ra = reach[std::size_t(a)];
                    if (ra > r || (ra == r && a > b)) continue;
                    if (std::hypot(x[std::size_t(a)] - xb, y[std::size_t(a)] - yb) > r) continue;
                    if (keep(a, b)) mine.push_back(a);
                }
            }
    }, sharedKept);
    for (int b = 0; b < n; ++b)
        for (const int a : found[std::size_t(b)]) {
            rows[std::size_t(b)].push_back(a);
            if (a != b) rows[std::size_t(a)].push_back(b);
        }
    sharedOut(n, threads, [&rows](int a) {
        std::vector<int>& row = rows[std::size_t(a)];
        std::sort(row.begin(), row.end());
        row.erase(std::unique(row.begin(), row.end()), row.end());
    }, sharedKept);
    return rows;
}

/// \a rows made a table (PairTable), its values - one or two to a pair - by
/// \a value on \a threads threads, each row by one of them alone.
PairTable pairTableOf(const std::vector<std::vector<int>>& rows, bool two, int threads,
                      const std::function<void(int, int, double*, double*)>& value)
{
    PairTable t;
    t.start.assign(rows.size() + 1, 0);
    for (std::size_t a = 0; a < rows.size(); ++a) t.start[a + 1] = t.start[a] + qint64(rows[a].size());
    const std::size_t count = std::size_t(t.start.back());
    t.other.resize(count);
    t.first.resize(count);
    if (two) t.second.resize(count);
    sharedOut(int(rows.size()), threads, [&t, &rows, &value, two](int a) {
        double unused = 0;
        for (std::size_t k = 0; k < rows[std::size_t(a)].size(); ++k) {
            const std::size_t at = std::size_t(t.start[std::size_t(a)]) + k;
            t.other[at] = rows[std::size_t(a)][k];
            value(a, t.other[at], &t.first[at], two ? &t.second[at] : &unused);
        }
    }, sharedKept);
    return t;
}

void staticPotentialColumn(const Context& c, Eigen::MatrixXd& p, int j)
{
    for (int i = 0; i < c.nc; ++i) p(i, j) = staticPotential(c, i, j);
}

/*!
 * A dense solve's static parts (Context::lDirect, lImage and pStatic), made
 * once before its frequencies and shared out among \a threads threads a
 * column at a time as they come free, each column written by its own thread
 * alone, as solveAt() fills a frequency's: first the potentials' columns,
 * each whole, and the inductances' above the diagonal and on it, the
 * longest first, so that the threads end together; then the inductances'
 * part below from the others' above, a block of columns at a time. Each
 * term is made by the expression that made it on one thread, so the parts
 * are the same to the last digit on any number of threads.
 */
void makeStatics(Context& c, int threads)
{
    const int nr = c.nr, nc = c.nc;
    c.lDirect.resize(nr, nr);
    c.lImage.resize(nr, nr);
    c.pStatic.resize(nc, nc);
    sharedOut(nc + nr, threads, [&c, nr, nc](int i) {
        if (i < nc)
            staticPotentialColumn(c, c.pStatic, i);
        else
            staticInductanceColumn(c, c.lDirect, c.lImage, nr - 1 - (i - nc));
    }, sharedStatics);
    const int blocks = (nr + kMirrorBlock - 1) / kMirrorBlock;
    sharedOut(2 * blocks, threads, [&c, blocks](int i) {
        Eigen::MatrixXd& x = i < blocks ? c.lDirect : c.lImage;
        const int b0 = (i < blocks ? i : i - blocks) * kMirrorBlock;
        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));
    }, sharedStatics);
}

/*!
 * The runs split into loops and a tree (a solve at a low frequency): a spanning forest of the cells and the ground, the runs as
 * its edges - its tree runs one to each cell but its roots, the rest each
 * a loop, itself and the tree's path back between its ends. A loop leaves
 * no charge, so that the charges' term, A^T P Q, is nought around it; and
 * its rounding cancels there, each cell's potential taken once each way.
 * Along a tree run it does not: where the cells' potentials are 10^11 V and
 * the drops along the runs milliohms, as an open line's at 1 Hz, a run's
 * row held the potentials' rounding, some 10^-5 V, and in the runs' basis no
 * one scale for its rows weighed both that and the loops' drops. Split, the
 * loops' rows are weighed against their drops and the tree runs' against
 * the current the charge takes.
 *
 *     I = L i + T t          i each loop's current, t each tree run's own
 *
 * held in one vector by run - a loop's current at its own run, a tree run's
 * at its - and made, and its transpose taken, along the tree, the loops
 * never made: a sum over the subtree below each tree run, and a potential
 * from each root. A component with a via to the ground is rooted there.
 */
class LoopTree
{
public:
    explicit LoopTree(const Context& c) : a_c(c), a_ground(c.nc)
    {
        const int nodes = c.nc + 1;
        QList<QList<int>> at(nodes);
        for (int a = 0; a < c.nr; ++a) {
            at[nodeOf(c.tail.at(a))] << a;
            at[nodeOf(c.head.at(a))] << a;
        }
        a_tree.assign(std::size_t(c.nr), 0);
        a_parent.assign(std::size_t(nodes), -1);
        a_up.assign(std::size_t(nodes), -1);
        a_component.assign(std::size_t(nodes), -1);
        // (The ground first, so that every component that reaches it is
        //  rooted on it, its potential nought.)
        const auto grow = [&](int root) {
            const int k = int(a_roots.size());
            a_roots << root;
            a_component[std::size_t(root)] = k;
            a_order << root;
            for (int n = int(a_order.size()) - 1; n < a_order.size(); ++n) {
                const int u = a_order.at(n);
                for (const int a : at.at(u)) {
                    const int t = nodeOf(c.tail.at(a)), h = nodeOf(c.head.at(a));
                    const int v = t == u ? h : t;
                    if (a_component[std::size_t(v)] >= 0) continue;
                    a_component[std::size_t(v)] = k;
                    a_parent[std::size_t(v)] = u;
                    a_up[std::size_t(v)] = a;
                    a_tree[std::size_t(a)] = 1;
                    a_order << v;
                }
            }
        };
        if (!at.at(a_ground).isEmpty()) grow(a_ground);
        for (int i = 0; i < c.nc; ++i)
            if (a_component[std::size_t(i)] < 0) grow(i);
    }

    bool tree(int run) const { return a_tree[std::size_t(run)] != 0; }
    int components() const { return int(a_roots.size()); }
    bool grounded(int component) const { return a_roots.at(component) == a_ground; }
    /// A floating component's root cell; -1 for one rooted on the ground.
    int rootCell(int component) const { return grounded(component) ? -1 : a_roots.at(component); }
    int componentOf(int cell) const { return a_component[std::size_t(cell)]; }

    /// I = L i + T t, of the loops' and the tree runs' currents by run.
    VectorC currents(const VectorC& y) const
    {
        VectorC i = y;
        VectorC in = VectorC::Zero(a_ground + 1);
        for (int a = 0; a < a_c.nr; ++a) {
            if (tree(a)) continue;
            // (Its loop back along the tree, from its head to its tail.)
            in[nodeOf(a_c.head.at(a))] += y[a];
            in[nodeOf(a_c.tail.at(a))] -= y[a];
        }
        for (int n = int(a_order.size()) - 1; n >= 0; --n) {
            const int v = a_order.at(n), a = a_up[std::size_t(v)];
            if (a < 0) continue;
            // (What comes up out of v's subtree, along its run's way.)
            i[a] += nodeOf(a_c.tail.at(a)) == v ? in[v] : -in[v];
            in[a_parent[std::size_t(v)]] += in[v];
        }
        return i;
    }

    /// [L T]^T w: each loop's sum of w around it, each tree run's own.
    VectorC transposed(const VectorC& w) const
    {
        const VectorC phi = along(w);
        VectorC out = w;
        for (int a = 0; a < a_c.nr; ++a)
            if (!tree(a)) out[a] = w[a] + phi[nodeOf(a_c.tail.at(a))] - phi[nodeOf(a_c.head.at(a))];
        return out;
    }

    /// The currents' loops and tree runs, of the currents I: [L T]^-1 I.
    VectorC ofCurrents(const VectorC& i) const
    {
        VectorC loops = i;
        for (int a = 0; a < a_c.nr; ++a)
            if (tree(a)) loops[a] = 0;
        return i - (currents(loops) - loops);
    }

    /// w by run whose [L T]^T w is r: [L T]^-T r.
    VectorC ofTransposed(const VectorC& r) const
    {
        VectorC w = r;
        for (int a = 0; a < a_c.nr; ++a)
            if (!tree(a)) w[a] = 0;
        const VectorC phi = along(w);
        for (int a = 0; a < a_c.nr; ++a)
            if (!tree(a)) w[a] = r[a] - (phi[nodeOf(a_c.tail.at(a))] - phi[nodeOf(a_c.head.at(a))]);
        return w;
    }

    /// The charge the tree runs' currents leave each cell, A T t: the
    /// loops' none.
    VectorC charges(const VectorC& y) const
    {
        VectorC q = VectorC::Zero(a_c.nc);
        for (int a = 0; a < a_c.nr; ++a) {
            if (!tree(a)) continue;
            if (a_c.tail.at(a) >= 0) q[a_c.tail.at(a)] += y[a];
            q[a_c.head.at(a)] -= y[a];
        }
        return q;
    }

    /// Each cell's potential from its component's root's, of the drops
    /// along the tree runs, drop[a] its tail's less its head's.
    VectorC potentials(const VectorC& drop) const
    {
        const VectorC phi = along(drop);
        return -phi.head(a_c.nc);
    }

    /*!
     * Each port's voltage J^T V of one solve, V each cell's potential taken
     * along the tree: its component's root's, plus the drops along the tree
     * runs from it - \a field, by run, each run's M I less its e, the field
     * along the metal (Zs J + jw A); only the tree runs' are read. A
     * grounded component's root is the ground's nought; a floating one's,
     * its cells' mean of \a charged, P Q, less those drops - so that P's own
     * error, a part in 10^5 compressed, is in its common potential alone,
     * and a port's voltage against another's on its metal is the drops
     * between them, to the currents' digits: of P Q's differences, at 10^11
     * V on an open line at 1 Hz, it was their rounding. Each port's drive is
     * summed on each component apart before it meets the root, so that the
     * drops' digits are not rounded into a potential 10^14 times theirs.
     */
    VectorC voltages(const MatrixC& drive, const VectorC& field, const VectorC& charged) const
    {
        VectorC drop = field;
        for (int a = 0; a < a_c.nr; ++a)
            if (!tree(a)) drop[a] = 0;
        const VectorC along = potentials(drop);
        VectorC root = VectorC::Zero(components());
        std::vector<int> counted(std::size_t(components()), 0);
        for (int i = 0; i < a_c.nc; ++i) {
            const int k = componentOf(i);
            if (grounded(k)) continue;
            root[k] += charged[i] - along[i];
            ++counted[std::size_t(k)];
        }
        for (int k = 0; k < components(); ++k)
            if (counted[std::size_t(k)] > 0) root[k] /= double(counted[std::size_t(k)]);
        VectorC out = VectorC::Zero(drive.cols());
        for (int r = 0; r < drive.cols(); ++r) {
            VectorC shares = VectorC::Zero(components());
            for (int i = 0; i < a_c.nc; ++i) {
                if (drive(i, r) == Complex(0, 0)) continue;
                out[r] += drive(i, r) * along[i];
                shares[componentOf(i)] += drive(i, r);
            }
            for (int k = 0; k < components(); ++k) out[r] += shares[k] * root[k];
        }
        return out;
    }

    /// A drive's sum on one component as the whole number of amperes it is
    /// (each column one in and one out), its shares' rounding some 10^-16:
    /// how columnBasis() tells a loop, carrying none, from a charge's edge.
    static Complex amperes(Complex sum)
    {
        const double whole = std::round(sum.real());
        return std::abs(sum - whole) < 1e-9 ? Complex(whole, 0) : sum;
    }

private:
    int nodeOf(int cell) const { return cell >= 0 ? cell : a_ground; }

    /// Each node's sum of w along the tree from its root, a run counted as
    /// it points away from the root.
    VectorC along(const VectorC& w) const
    {
        VectorC phi = VectorC::Zero(a_ground + 1);
        for (const int v : a_order) {
            const int a = a_up[std::size_t(v)];
            if (a < 0) continue;
            const int u = a_parent[std::size_t(v)];
            phi[v] = phi[u] + (nodeOf(a_c.tail.at(a)) == u ? w[a] : -w[a]);
        }
        return phi;
    }

    const Context& a_c;
    int a_ground;                        ///< its node, after the cells
    std::vector<char> a_tree;            ///< by run
    std::vector<int> a_parent, a_up;     ///< by node: its parent, and the run to it
    std::vector<int> a_component;        ///< by node
    QList<int> a_order;                  ///< the nodes, each after its parent
    QList<int> a_roots;                  ///< by component
};

/*!
 * The columns \a c's solves take where a port has more than one
 * (Context::basis), of the \a np ports: empty where none has.
 *
 * Each column carries an ampere from its port's own cells to one of its
 * returns, so that each holds the own cells' charge term: at 1 Hz some 10^12
 * Ohm of a coplanar line's signal, in every term alike, and the columns'
 * matrix all but one of rank. Tied as (B^T Z^-1 B)^-1, that inverse lost
 * what was below it - the line's C at 1 Hz 234 % off, a part in 10^13 at 10
 * Hz, two terminals' 10^59, with nothing said. A port's other columns less its first carry the ampere between
 * its returns alone, the own cells' drive cancelling to the bit, so that
 * that term is in each port's first column alone. And of two ports whose
 * returns are on the same two conductors, those differences less each other
 * carry nothing into either, a loop round both ports' ties through the
 * conductors, of an impedance jwL: as a difference of two differences of
 * 10^12 Ohm each it was their rounding - a 2 mm line's Z at 1 Hz, dense,
 * real parts as large as itself and S21 nought. Taken apart (the
 * differences as the edges of a graph of the conductors, each beyond a
 * spanning forest of them a loop with its way back along the forest), each
 * loop is a column of its own, its small impedance its own solve's.
 */
MatrixC columnBasis(const Context& c, int np)
{
    const int nk = c.np;
    if (nk == np) return {};
    QList<QList<int>> of(np);
    for (int k = 0; k < nk; ++k) of[c.portOf.at(k)] << k;
    for (const QList<int>& columns : std::as_const(of))
        if (columns.isEmpty()) return {};
    // Each column's whole amperes into each conductor (component of the
    // runs); where one is no whole number, no loops are taken apart.
    const LoopTree loops(c);
    const int n = loops.components();
    std::vector<std::vector<int>> into(std::size_t(nk), std::vector<int>(std::size_t(n), 0));
    bool whole = true;
    for (int k = 0; k < nk; ++k) {
        VectorC sum = VectorC::Zero(n);
        for (int i = 0; i < c.nc; ++i)
            if (c.drive(i, k) != Complex(0, 0)) sum[loops.componentOf(i)] += c.drive(i, k);
        for (int m = 0; m < n; ++m) {
            const Complex a = LoopTree::amperes(sum[m]);
            if (a.imag() != 0 || a.real() != std::round(a.real())) whole = false;
            into[std::size_t(k)][std::size_t(m)] = int(std::lround(a.real()));
        }
    }
    MatrixC t = MatrixC::Zero(nk, nk);
    for (int p = 0; p < np; ++p) t(of.at(p).first(), p) = 1;
    int at = np;
    // The differences: each a loop where it carries none into any
    // conductor, an edge where an ampere from one to another.
    struct Edge {
        VectorC v;
        int plus = -1, minus = -1;   ///< the conductors it carries an ampere into and out of
    };
    QList<Edge> edges;
    QList<VectorC> loopsOf;
    for (int p = 0; p < np; ++p) {
        const int first = of.at(p).first();
        for (int r = 1; r < of.at(p).size(); ++r) {
            const int k = of.at(p).at(r);
            VectorC v = VectorC::Zero(nk);
            v[k] = 1;
            v[first] = -1;
            Edge e{v};
            int others = 0;
            for (int m = 0; whole && m < n; ++m) {
                const int d = into[std::size_t(k)][std::size_t(m)] - into[std::size_t(first)][std::size_t(m)];
                if (d == 1 && e.plus < 0) e.plus = m;
                else if (d == -1 && e.minus < 0) e.minus = m;
                else if (d != 0) ++others;
            }
            if (!whole || others > 0 || (e.plus < 0) != (e.minus < 0))
                t.col(at++) = v;   // (no shape of the two: as it is)
            else if (e.plus < 0)
                loopsOf << v;
            else
                edges << e;
        }
    }
    // A spanning forest of the conductors by the edges: each edge that joins
    // two of its trees one of it, kept; each other a loop - it, less the way
    // from its minus to its plus along the forest, which carries the same.
    std::vector<int> joined(static_cast<std::size_t>(n));
    std::iota(joined.begin(), joined.end(), 0);
    const auto find = [&joined](int x) {
        while (joined[std::size_t(x)] != x) x = joined[std::size_t(x)] = joined[std::size_t(joined[std::size_t(x)])];
        return x;
    };
    QList<QList<QPair<int, int>>> forest(n);   // (by conductor: its neighbour, and the edge between)
    for (int e = 0; e < edges.size(); ++e) {
        const Edge& edge = edges.at(e);
        const int a = find(edge.plus), b = find(edge.minus);
        if (a != b) {
            joined[std::size_t(a)] = b;
            forest[edge.plus] << qMakePair(edge.minus, e);
            forest[edge.minus] << qMakePair(edge.plus, e);
            t.col(at++) = edge.v;
            continue;
        }
        // (The way along the forest, from its minus to its plus.)
        std::vector<int> cameBy(std::size_t(n), -2);
        std::vector<int> from(std::size_t(n), -1);
        QList<int> queue{edge.minus};
        cameBy[std::size_t(edge.minus)] = -1;
        for (int q = 0; q < queue.size() && cameBy[std::size_t(edge.plus)] == -2; ++q)
            for (const QPair<int, int>& next : forest.at(queue.at(q)))
                if (cameBy[std::size_t(next.first)] == -2) {
                    cameBy[std::size_t(next.first)] = next.second;
                    from[std::size_t(next.first)] = queue.at(q);
                    queue << next.first;
                }
        VectorC loop = edge.v;
        for (int m = edge.plus; m != edge.minus; m = from[std::size_t(m)]) {
            const Edge& way = edges.at(cameBy[std::size_t(m)]);
            // (Into m from the one before it: +1 where the edge carries its
            //  ampere that way.)
            loop -= way.plus == m ? way.v : VectorC(-way.v);
        }
        loopsOf << loop;
    }
    for (const VectorC& loop : std::as_const(loopsOf)) t.col(at++) = loop;
    return t;
}

AtFrequency solveAt(const Context& c, double f, int threads, bool symmetric, double augmentedFrom)
{
    AtFrequency out;
    QElapsedTimer clock;
    clock.start();
    const Kernels k(c, f, threads);
    if (!k.problems.isEmpty()) {
        out.problems << k.problems;
        return out;
    }
    const Complex jw = k.jw;
    const int nr = c.nr, nc = c.nc, nv = int(c.vias.size());

    // M = Zs l/w + jw L + A^T P A / jw, the charge eliminated, and P, each
    // cell's potential of a unit charge spread over another: both
    // symmetric, and each column held in one piece (Eigen's order), so a
    // column is its row. Shared out among the frequency's threads a column
    // at a time as they come free, each column written by its own thread
    // alone: first each one's part above the diagonal and on it, the
    // longest first, so that the threads end together; then the part below
    // from the others' above, a block of columns at a time; then M's
    // charges, which read P whole. Each term is made as one thread made it,
    // in the same order, so the system is the same to the last digit on any
    // number of threads.
    MatrixC m(nr, nr), p(nc, nc);
    sharedOut(nr + nc, threads, [&c, &k, &m, &p, nr, nc](int i) {
        if (i < nr)
            systemColumn(c, k, m, nr - 1 - i);
        else
            potentialColumn(c, k, p, nc - 1 - (i - nr));
    });
    const int mBlocks = (nr + kMirrorBlock - 1) / kMirrorBlock, pBlocks = (nc + kMirrorBlock - 1) / kMirrorBlock;
    sharedOut(mBlocks + pBlocks, threads, [&m, &p, mBlocks](int i) {
        MatrixC& x = i < mBlocks ? m : p;
        const int b0 = (i < mBlocks ? i : i - mBlocks) * kMirrorBlock;
        mirrorColumns(x, b0, std::min(int(x.cols()), b0 + kMirrorBlock));
    });
    ownImpedances(c, f, [&m](int a, int b, Complex v) { m(a, b) += v; });
    groundImpedances(c, f, [&m](int a, int b, Complex v) { m(a, b) += v; });
    // The charges' term against the rest of each run's own (N4): the rest
    // is the diagonal before the charges' term is added.
    {
        const auto pAt = [&p](int i, int j) { return i < 0 || j < 0 ? Complex(0, 0) : p(i, j); };
        for (int a = 0; a < nr; ++a) {
            const int t = c.tail.at(a), h = c.head.at(a);
            const Complex charge = overJw(pAt(t, t) - pAt(t, h) - pAt(h, t) + pAt(h, h), jw);
            const double rest = std::abs(m(a, a));
            const double ratio = std::abs(charge) / std::max(rest, 1e-300 * std::abs(charge) + 1e-300);
            out.chargeOverSeries = std::max(out.chargeOverSeries, ratio);
        }
    }
    // At a low frequency the charges kept as unknowns of their own, not
    // eliminated: the charges' term over the currents' some 1e10 times or
    // more (kAugmentedFrom). Eliminated, a run's equation is its charges'
    // A^T P A / jw beside its currents' Zs + jw L, and past some 1e15 of
    // that the currents are lost in its rounding - a centimetre loop read
    // 21.4 to 26.0 nH at 1 kHz from one mesh to the next, where 24.2 is
    // right (the production-readiness review: P53's
    // breakdown, of which only the hint had been mended).
    const bool augmented = out.chargeOverSeries > (augmentedFrom > 0 ? augmentedFrom : kAugmentedFrom);
    if (!augmented) sharedOut(nr, threads, [&c, &p, &m, jw](int b) { chargeColumn(c, p, jw, m, b); });

    // A port and a via: the via's potential at the port's cells, by their
    // shares. Along the runs a via's potential telescopes from the via to
    // the port, and a port's current comes up to its cells from the ground
    // by a path the solve otherwise gives no field: this is that path's part
    // of it, ending it at the ground's nought - without which a via's
    // inductance seen from a port in a dielectric was short by twice its
    // potential at the port, a tenth of it 1.8 mm away through 200 µm of 9.8.
    // Into each port's voltage and, the same, into each via's drive.
    MatrixC portVia = MatrixC::Zero(c.np, nv);
    for (int q = 0; q < c.np; ++q)
        for (int i = 0; i < nc; ++i)
            if (c.drive(i, q) != Complex(0, 0))
                for (int j = 0; j < nv; ++j) portVia(q, j) += c.drive(i, q) * k.viaPotential(j, i);

    // Each port driven by an ampere in turn: A^T P J / jw the drive.
    const MatrixC pDrive = p * c.drive;
    const auto driveAt = [&pDrive](int i, int q) { return i < 0 ? Complex(0, 0) : pDrive(i, q); };
    // (With the charges as unknowns, the rest of it alone, e: Q holds that
    //  part. Taken from the whole, e was lost in its rounding - the part
    //  1/jw outweighs it as the charges' term the currents'.)
    MatrixC rhs = MatrixC::Zero(nr, c.np);
    if (!augmented)
        for (int a = 0; a < nr; ++a)
            for (int q = 0; q < c.np; ++q) rhs(a, q) = (driveAt(c.tail.at(a), q) - driveAt(c.head.at(a), q)) / jw;
    for (int j = 0; j < nv; ++j)
        for (int q = 0; q < c.np; ++q) rhs(c.vias.at(j).run, q) -= portVia(q, j);
    MatrixC pathZ = MatrixC::Zero(c.np, c.np);
    addPortPaths(c, k, rhs, pathZ);
    addSourceFields(c, k, rhs, pathZ);
    // (Where a port has more than one column, the columns solved for:
    //  Context::basis. Each term above is linear in the columns' drives.)
    const bool based = c.basis.size() > 0;
    const MatrixC drive = based ? MatrixC(c.drive * c.basis) : c.drive;
    if (based) {
        rhs = rhs * c.basis;
        pathZ = c.basis.transpose() * pathZ * c.basis;
    }
    out.fillSeconds = double(clock.nsecsElapsed()) * 1e-9;
    if (augmented) {
        // The runs and the cells' charges together:
        //     (Zs l/w + jw L) I - A^T P Q = e      e: the rest of the drive
        //     A I + jw Q = J
        // e the right-hand side without its A^T P J / jw, which Q holds; and
        // each port's voltage J^T P Q + its path's - e^T I, the charge's
        // potential found, not a difference of two terms 1/jw each. Its
        // condition goes as 1/w where the eliminated system's goes as 1/w^2:
        // the digits a centimetre loop lost at 1 kHz, some fifteen, some
        // eight.
        const int na = nr + nc;
        // (M's rows of the tree runs kept, for the drops along them: each
        //  port's voltage taken along the tree, LoopTree::voltages().)
        const LoopTree loops(c);
        std::vector<int> treeRuns;
        for (int a = 0; a < nr; ++a)
            if (loops.tree(a)) treeRuns.push_back(a);
        MatrixC treeRows(Eigen::Index(treeRuns.size()), nr);
        for (std::size_t t = 0; t < treeRuns.size(); ++t) treeRows.row(Eigen::Index(t)) = m.row(treeRuns[t]);
        MatrixC big = MatrixC::Zero(na, na);
        big.topLeftCorner(nr, nr) = m;
        m.resize(0, 0);
        for (int a = 0; a < nr; ++a) {
            const int t = c.tail.at(a), h = c.head.at(a);
            for (int j = 0; j < nc; ++j) big(a, nr + j) = -((t >= 0 ? p(t, j) : Complex(0, 0)) - (h >= 0 ? p(h, j) : Complex(0, 0)));
            if (t >= 0) big(nr + t, a) += 1.0;
            if (h >= 0) big(nr + h, a) -= 1.0;
        }
        for (int i = 0; i < nc; ++i) big(nr + i, nr + i) = jw;
        const MatrixC& extra = rhs;
        MatrixC both(na, c.np);
        both.topRows(nr) = extra;
        both.bottomRows(nc) = drive;
        const DenseFactors<Complex> lu(std::move(big), threads, false);
        const MatrixC solved = lu.solve(both);
        out.current = solved.topRows(nr);
        out.rcond = lu.rcond();
        out.accelerated = lu.accelerated();
        out.openBlas = lu.openBlas();
        out.symmetric = false;
        out.augmented = true;
        out.luSeconds = double(clock.nsecsElapsed()) * 1e-9 - out.fillSeconds - lu.waited();
        if (!solved.allFinite()) {
            out.problems << tr("The solve at %1 GHz came to nothing finite.").arg(f / 1e9, 0, 'g', 4);
            return out;
        }
        // Each port's voltage along the tree: of P Q's differences, at 10^11 V
        // on an open line at 1 Hz, it was their rounding - the line's series
        // inductance 22 % off at 1 kHz and 2.7e-3 at 10 kHz, where along the
        // tree 1.5 % and 1.5e-4, Z's own rounding.
        const MatrixC charged = p * solved.bottomRows(nc);
        const MatrixC field = treeRows * out.current;
        out.z = pathZ - extra.transpose() * out.current;
        for (int q = 0; q < c.np; ++q) {
            VectorC drop = VectorC::Zero(nr);
            for (std::size_t t = 0; t < treeRuns.size(); ++t) drop[treeRuns[t]] = field(Eigen::Index(t), q) - extra(treeRuns[t], q);
            out.z.col(q) += loops.voltages(drive, drop, charged.col(q));
        }
        return out;
    }
    // (m factorized where it is: it is not needed past here.)
    // (On the frequency's share of the threads: rf25_dense.h.)
    // (Symmetric, as each column's part below the diagonal is the part
    //  above's - but for the charges' last digits, added a column at a time
    //  -: as L D L^T where many frequencies at once share the machine's
    //  matrix units, its upper triangle read. DenseFactors.)
    const DenseFactors<Complex> lu(std::move(m), threads, symmetric);
    out.current = lu.solve(rhs);
    out.rcond = lu.rcond();
    out.accelerated = lu.accelerated();
    out.openBlas = lu.openBlas();
    out.symmetric = lu.symmetric();
    out.luSeconds = double(clock.nsecsElapsed()) * 1e-9 - out.fillSeconds - lu.waited();
    if (!out.current.allFinite()) {
        out.problems << tr("The solve at %1 GHz came to nothing finite.").arg(f / 1e9, 0, 'g', 4);
        return out;
    }

    // Each port's voltage: its cells' potentials by the same shares, of its
    // own charge, J^T P J / jw, and of the charges the currents leave, with
    // what the vias' and the paths' currents add along its path - the
    // drive's coupling the other way round, by reciprocity.
    out.z = drive.transpose() * (based ? MatrixC(pDrive * c.basis) : pDrive) / jw + pathZ - rhs.transpose() * out.current;
    return out;
}

/*!
 * What a compressed solve is made of that is the same at every frequency:
 * the cluster trees and the shapes of the matrices over them, and the
 * pairs near enough to precondition with. L is held as two kernels, of
 * the runs with a part along x and of those with a part along y - an
 * oblique run in both - each between the runs' cosines' halves:
 *
 *     L_ab = (x_a x_b + y_a y_b) K_ab   so   L I = X Kx X I + Y Ky Y I
 *
 * X and Y the runs' directions' parts. Neither kernel is nought between a
 * pair of its runs, as L is between two at right angles - which a cross
 * approximation, sampling a block a row at a time, would take for done
 * when it was half done - and an axis-aligned layout's two kernels are
 * half of L, the half between runs at right angles left out.
 *
 * Made in place, and never copied: the matrices point at the trees.
 */
struct Compression {
    double tolerance = kTolerance;
    ClusterTree xTree, yTree, cellTree, viaTree;
    QList<int> xRun, yRun;                     ///< each kernel's runs, in the plane
    HMatrix kx, ky, p, phi, g;                 ///< their shapes, unfilled
    QList<QPair<int, int>> nearRuns, nearCells, nearViaCells, nearVias;
    /// A correction held as its factors (rf25_dielectric.h): its entry for
    /// each of nearCells, made once - static, the same at every frequency -
    /// for the near field's LU; empty where it is held whole or there is none.
    QList<double> nearCorrection;
    double estimate = 0;                       ///< bytes a frequency is guessed to take
    double denseBytes = 0;                     ///< what the dense solve would take
};

/// The trees, shapes and near pairs of \a c's compressed solve, into \a z.
void plan(Compression& z, const Context& c)
{
    const int nv = int(c.vias.size());
    QList<Box> cells, runs, vias;
    for (int i = 0; i < c.nc; ++i) {
        const QRectF& r = c.rect.at(i);
        cells << Box::of(r.left(), r.top(), c.cellZ.at(i), r.right(), r.bottom(), c.cellZ.at(i));
    }
    // A run covers the halves of its two cells it joins: from one centre
    // to the other, as wide as the edge between them.
    for (int a = 0; a < c.planar; ++a) {
        const Filament& run = c.runs.at(a);
        const double nx = -c.dirY.at(a) * run.width / 2, ny = c.dirX.at(a) * run.width / 2;
        Box box = Box::of(run.from.x() - nx, run.from.y() - ny, run.z0, run.from.x() + nx, run.from.y() + ny, run.z0);
        box = box.united(Box::of(run.to.x() - nx, run.to.y() - ny, run.z0, run.to.x() + nx, run.to.y() + ny, run.z0));
        runs << box;
    }
    for (const Via& v : c.vias) {
        const Filament& run = c.runs.at(v.run);
        vias << Box::of(v.at.x() - v.side / 2, v.at.y() - v.side / 2, run.z0, v.at.x() + v.side / 2, v.at.y() + v.side / 2,
                        run.z1);
    }
    QList<Box> xs, ys;
    for (int a = 0; a < c.planar; ++a) {
        if (std::abs(c.dirX.at(a)) > 1e-9) {
            z.xRun << a;
            xs << runs.at(a);
        }
        if (std::abs(c.dirY.at(a)) > 1e-9) {
            z.yRun << a;
            ys << runs.at(a);
        }
    }
    z.xTree = ClusterTree(xs, kLeaf);
    z.yTree = ClusterTree(ys, kLeaf);
    z.cellTree = ClusterTree(cells, kLeaf);
    z.viaTree = ClusterTree(vias, kLeaf);
    z.kx = HMatrix(&z.xTree, &z.xTree, true, kEta);
    z.ky = HMatrix(&z.yTree, &z.yTree, true, kEta);
    z.p = HMatrix(&z.cellTree, &z.cellTree, true, kEta);
    z.phi = HMatrix(&z.cellTree, &z.viaTree, false, kEta);
    z.g = HMatrix(&z.viaTree, &z.viaTree, true, kEta);
    // The near pairs: runs not at right angles, cells, a via and the cells
    // beside it, two vias.
    for (const QPair<int, int>& pair : nearPairs(runs, nullptr, kNear))
        if (std::abs(c.dirX.at(pair.first) * c.dirX.at(pair.second) + c.dirY.at(pair.first) * c.dirY.at(pair.second)) >= 1e-9)
            z.nearRuns << pair;
    z.nearCells = nearPairs(cells, nullptr, kNear);
    // (A factored correction's near entries, each a sum over the panels:
    //  once here, where every frequency's near field had each again.)
    if (c.corrected && c.dielectric.factored) {
        z.nearCorrection.resize(z.nearCells.size());
        for (int k = 0; k < z.nearCells.size(); ++k) z.nearCorrection[k] = c.dielectric.at(z.nearCells.at(k).first, z.nearCells.at(k).second);
    }
    if (nv > 0) {
        z.nearViaCells = nearPairs(vias, &cells, kNear);
        z.nearVias = nearPairs(vias, nullptr, kNear);
    }
    // What a frequency will take: the matrices' dense blocks, their low-rank
    // ones at a guessed rank, the near field and some ten times it for its
    // LU, and GMRES's vectors.
    double held = 0;
    for (const HMatrix* h : {&z.kx, &z.ky, &z.p, &z.phi, &z.g}) held += h->denseBlockBytes() + 16 * kGuessRank * h->lowRankSpan();
    // (And what the dense solve would take: the system and its LU, and P.)
    z.denseBytes = 16.0 * (2.0 * double(c.nr) * c.nr + double(c.nc) * c.nc);
    const double near = 16.0 * (2.0 * z.nearRuns.size() + 32.0 * z.nearCells.size() + 8.0 * z.nearViaCells.size()
                                + 2.0 * z.nearVias.size() + 4.0 * c.nr);
    z.estimate = held + 11 * near + 16.0 * c.nr * (kRestart + 8) + 32.0 * (c.nr + c.nc) * c.np + 1e7;
}

/*!
 * A compressed solve's static parts of the pairs whose parts make them
 * dear (Context::runStatics and cellStatics), made once before its
 * frequencies on all of its \a threads threads, each by the expression that
 * makes it as it goes: the pairs of runs within eight of their breadths
 * where either has parts (dearPair()), and of cells within four of their
 * sides where either has parts (dearPotential()) - and, where they fit in
 * \a most bytes too, each cell with a wall with every cell, which
 * dearPotential() takes at every distance. And every pair of the blocks
 * the matrices hold dense (\a z's kx, ky and p not low rank), which every
 * frequency fills whole, and of their near field's: each of their two
 * partial inductances there was some 8 % of a pad's compressed solve, each
 * frequency again. What does not fit is made as it goes, as before;
 * nothing at all where even the near pairs do not.
 */
void cacheStatics(Context& c, const Compression& z, int threads, double most)
{
    keptRuns = 0;
    keptCells = 0;
    const int np = c.planar, nc = c.nc;
    // The runs: each at its middle, reaching eight of its breadth and
    // thickness (and a little more, dearPair() deciding).
    std::vector<double> x(static_cast<std::size_t>(np)), y(static_cast<std::size_t>(np)), reach(static_cast<std::size_t>(np));
    for (int a = 0; a < np; ++a) {
        const Filament& r = c.runs.at(a);
        x[std::size_t(a)] = 0.5 * (r.from.x() + r.to.x());
        y[std::size_t(a)] = 0.5 * (r.from.y() + r.to.y());
        reach[std::size_t(a)] = 8.001 * std::max(r.width, r.thickness);
    }
    std::vector<std::vector<int>> runRows = pairsWithin(x, y, reach, threads, [&c](int a, int b) { return dearPair(c, a, b); });
    // The cells: each at its centre, reaching four of its longest side.
    x.assign(std::size_t(nc), 0);
    y.assign(std::size_t(nc), 0);
    reach.assign(std::size_t(nc), 0);
    for (int i = 0; i < nc; ++i) {
        x[std::size_t(i)] = c.centre.at(i).x();
        y[std::size_t(i)] = c.centre.at(i).y();
        reach[std::size_t(i)] = 4.001 * std::max(c.rect.at(i).width(), c.rect.at(i).height());
    }
    std::vector<std::vector<int>> cellRows = pairsWithin(x, y, reach, threads, [&c](int i, int j) {
        return dearPotential(c, i, j);
    });
    // The matrices' dense blocks, by their items' runs or cells, and the
    // near field's pairs: both ways round, each row then in order.
    std::vector<std::vector<int>> runRowsAll = runRows;
    const auto denseBlocks = [](const HMatrix& h, const QList<int>* runs, std::vector<std::vector<int>>& rows) {
        for (const HMatrix::Block& b : h.blocks()) {
            if (b.lowRank) continue;
            const ClusterTree::Node& r = h.rowTree()->nodes()[std::size_t(b.row)];
            const ClusterTree::Node& s = h.columnTree()->nodes()[std::size_t(b.column)];
            for (int p = r.begin; p < r.end; ++p)
                for (int q = s.begin; q < s.end; ++q) {
                    const int u = h.rowTree()->order()[std::size_t(p)], v = h.columnTree()->order()[std::size_t(q)];
                    const int i = runs != nullptr ? runs->at(u) : u, j = runs != nullptr ? runs->at(v) : v;
                    rows[std::size_t(i)].push_back(j);
                    rows[std::size_t(j)].push_back(i);
                }
        }
    };
    denseBlocks(z.kx, &z.xRun, runRowsAll);
    denseBlocks(z.ky, &z.yRun, runRowsAll);
    for (const QPair<int, int>& r : z.nearRuns) {
        runRowsAll[std::size_t(r.first)].push_back(r.second);
        runRowsAll[std::size_t(r.second)].push_back(r.first);
    }
    std::vector<std::vector<int>> cellRowsAll = cellRows;
    denseBlocks(z.p, nullptr, cellRowsAll);
    for (const QPair<int, int>& q : z.nearCells) {
        cellRowsAll[std::size_t(q.first)].push_back(q.second);
        cellRowsAll[std::size_t(q.second)].push_back(q.first);
    }
    const auto ordered = [threads](std::vector<std::vector<int>>& rows) {
        sharedOut(int(rows.size()), threads, [&rows](int a) {
            std::vector<int>& row = rows[std::size_t(a)];
            std::sort(row.begin(), row.end());
            row.erase(std::unique(row.begin(), row.end()), row.end());
        }, sharedKept);
    };
    ordered(runRowsAll);
    ordered(cellRowsAll);
    // (The dense blocks too where they fit, else the dear pairs alone.)
    const auto pairsIn = [](const std::vector<std::vector<int>>& rows) {
        qint64 n = 0;
        for (const std::vector<int>& row : rows) n += qint64(row.size());
        return double(n);
    };
    double nearBytes = 20.0 * pairsIn(runRowsAll) + 12.0 * pairsIn(cellRowsAll) + 8.0 * (np + nc);
    if (nearBytes <= most) {
        runRows = std::move(runRowsAll);
        cellRows = std::move(cellRowsAll);
    } else {
        nearBytes = 20.0 * pairsIn(runRows) + 12.0 * pairsIn(cellRows) + 8.0 * (np + nc);
        if (nearBytes > most) return;
    }
    // The walls, at every distance, where they fit.
    QList<int> walled;
    for (int i = 0; i < nc; ++i)
        for (const ChargePart& p : c.charges.at(i))
            if (p.plane != 0) {
                walled << i;
                break;
            }
    if (!walled.isEmpty() && nearBytes + 12.0 * 2.0 * double(walled.size()) * double(nc) <= most) {
        std::vector<char> isWalled(std::size_t(nc), 0);
        for (const int w : std::as_const(walled)) isWalled[std::size_t(w)] = 1;
        sharedOut(nc, threads, [&](int i) {
            std::vector<int>& row = cellRows[std::size_t(i)];
            if (isWalled[std::size_t(i)]) {
                row.resize(std::size_t(nc));
                std::iota(row.begin(), row.end(), 0);
                return;
            }
            row.insert(row.end(), walled.begin(), walled.end());
            std::sort(row.begin(), row.end());
            row.erase(std::unique(row.begin(), row.end()), row.end());
        }, sharedKept);
    }
    c.runStatics = pairTableOf(runRows, true, threads, [&c](int a, int b, double* direct, double* image) {
        staticPairWithLid(c, a, b, direct, image);
    });
    c.cellStatics = pairTableOf(cellRows, false, threads, [&c](int i, int j, double* p, double*) {
        *p = staticPotentialOf(c, i, j);
    });
    keptRuns = qint64(c.runStatics.other.size());
    keptCells = qint64(c.cellStatics.other.size());
}

/// The system of a compressed solve, M = Zs l/w + jw L + A^T P A / jw and
/// the vias' terms, multiplying a vector: what GMRES asks of it.
struct System {
    const Context& c;
    const Compression& z;
    const HMatrix& kx;
    const HMatrix& ky;
    const HMatrix& p;
    const HMatrix& phi;
    const HMatrix& g;
    const Eigen::SparseMatrix<Complex>& own;
    Complex jw;
    /// A^T P A / jw with the rest; false where the charges are unknowns of
    /// their own (kAugmentedFrom), the runs' part of that system alone.
    bool charges = true;
    void operator()(const VectorC& x, VectorC& y) const;
};

void System::operator()(const VectorC& x, VectorC& y) const
{
    y = own * x;
    // jw L: each kernel between the runs' parts along its axis.
    for (const bool alongX : {true, false}) {
        const QList<int>& runs = alongX ? z.xRun : z.yRun;
        const QList<double>& part = alongX ? c.dirX : c.dirY;
        if (runs.isEmpty()) continue;
        VectorC in(runs.size()), result = VectorC::Zero(runs.size());
        for (int t = 0; t < runs.size(); ++t) in[t] = part.at(runs.at(t)) * x[runs.at(t)];
        (alongX ? kx : ky).multiply(in, result);
        for (int t = 0; t < runs.size(); ++t) y[runs.at(t)] += jw * (part.at(runs.at(t)) * result[t]);
    }
    // A^T P A / jw: the charge each cell is left (a via to the ground
    // leaving its charge to the ground), its potentials, each run's drop.
    if (charges) {
        VectorC q = VectorC::Zero(c.nc), v = VectorC::Zero(c.nc);
        for (int a = 0; a < c.nr; ++a) {
            if (c.tail.at(a) >= 0) q[c.tail.at(a)] += x[a];
            q[c.head.at(a)] -= x[a];
        }
        p.multiply(q, v);
        addFactored(c, q, v);
        // (Divided, not overJw(): one a run, against the dense system's one
        //  a term, its cost is nothing.)
        for (int a = 0; a < c.nr; ++a) y[a] += ((c.tail.at(a) >= 0 ? v[c.tail.at(a)] : Complex(0, 0)) - v[c.head.at(a)]) / jw;
    }
    // The vias: W = -A^T Phi, Phi each via's potential at each cell, is a
    // run in the plane's coupling to a via both ways round, and half of it
    // each way round between two vias, beside their G_zz.
    const int nv = int(c.vias.size());
    if (nv == 0) return;
    VectorC xv(nv), u = VectorC::Zero(c.nc), s = VectorC::Zero(c.nc), t = VectorC::Zero(nv), gx = VectorC::Zero(nv);
    for (int j = 0; j < nv; ++j) xv[j] = x[c.vias.at(j).run];
    phi.multiply(xv, u);
    for (int a = 0; a < c.planar; ++a) {
        s[c.tail.at(a)] += x[a];
        s[c.head.at(a)] -= x[a];
    }
    for (const Via& via : c.vias) {
        if (c.tail.at(via.run) >= 0) s[c.tail.at(via.run)] += 0.5 * x[via.run];
        s[c.head.at(via.run)] -= 0.5 * x[via.run];
    }
    phi.multiplyTransposed(s, t);
    g.multiply(xv, gx);
    for (int a = 0; a < c.planar; ++a) y[a] += u[c.head.at(a)] - u[c.tail.at(a)];
    for (int j = 0; j < nv; ++j) {
        const int run = c.vias.at(j).run, tail = c.tail.at(run), head = c.head.at(run);
        y[run] += -0.5 * ((tail >= 0 ? u[tail] : Complex(0, 0)) - u[head]) - t[j] + gx[j];
    }
}

/*!
 * The near field of \a c's system at frequency \a f, sparse: the same
 * terms as the whole system's between the near pairs alone, with the
 * whole of A - so that a loop of current, which leaves no charge, is as
 * free of P in it as in the system, and the two scales of a low frequency,
 * jw L and P / jw, stand as they do there. What the compressed solve is
 * preconditioned with, by its LU. Given \a runsOut and \a pOut, its parts
 * instead - the runs' block (their impedances, the vias' with them) and the
 * cells' P -, nothing returned: what the charges as unknowns of their own
 * are preconditioned with, each block factorized apart (solveCompressed()).
 */
Eigen::SparseMatrix<Complex> nearField(const Context& c, const Compression& z, const Kernels& k,
                                       const std::vector<Eigen::Triplet<Complex>>& own,
                                       Eigen::SparseMatrix<Complex>* runsOut = nullptr, Eigen::SparseMatrix<Complex>* pOut = nullptr)
{
    const int nr = c.nr, nc = c.nc, nv = int(c.vias.size());
    const Complex jw = k.jw;
    std::vector<Eigen::Triplet<Complex>> near = own;
    for (const QPair<int, int>& r : z.nearRuns) {
        const Complex v = jw * (k.cosine(r.first, r.second) * k.inductance(r.first, r.second));
        near.emplace_back(r.first, r.second, v);
        if (r.first != r.second) near.emplace_back(r.second, r.first, v);
    }
    std::vector<Eigen::Triplet<Complex>> terms;
    for (int n = 0; n < z.nearCells.size(); ++n) {
        const QPair<int, int>& q = z.nearCells.at(n);
        // (A correction held as its factors, its near pairs' entries here -
        //  made once, plan().)
        const Complex v = k.potential(q.first, q.second) + (z.nearCorrection.isEmpty() ? 0.0 : z.nearCorrection.at(n));
        terms.emplace_back(q.first, q.second, v);
        if (q.first != q.second) terms.emplace_back(q.second, q.first, v);
    }
    Eigen::SparseMatrix<Complex> pNear(nc, nc);
    pNear.setFromTriplets(terms.begin(), terms.end());
    terms.clear();
    for (int a = 0; a < nr; ++a) {
        if (c.tail.at(a) >= 0) terms.emplace_back(c.tail.at(a), a, Complex(1, 0));
        terms.emplace_back(c.head.at(a), a, Complex(-1, 0));
    }
    Eigen::SparseMatrix<Complex> incidence(nc, nr);
    incidence.setFromTriplets(terms.begin(), terms.end());
    const Eigen::SparseMatrix<Complex> transposed = incidence.transpose();
    if (nv > 0) {
        terms.clear();
        for (const QPair<int, int>& q : z.nearViaCells) terms.emplace_back(q.second, q.first, k.viaPotential(q.first, q.second));
        Eigen::SparseMatrix<Complex> phiNear(nc, nv);
        phiNear.setFromTriplets(terms.begin(), terms.end());
        const Eigen::SparseMatrix<Complex> w = transposed * phiNear;   // (-W)
        for (int j = 0; j < w.outerSize(); ++j)
            for (Eigen::SparseMatrix<Complex>::InnerIterator it(w, j); it; ++it) {
                const int r = int(it.row()), run = c.vias.at(j).run;
                const Complex v = (r < c.planar ? -1.0 : -0.5) * it.value();
                near.emplace_back(r, run, v);
                near.emplace_back(run, r, v);
            }
        for (const QPair<int, int>& q : z.nearVias) {
            const Complex v = 0.5 * (k.viaCoupling(q.first, q.second) + k.viaCoupling(q.second, q.first));
            const int ri = c.vias.at(q.first).run, rj = c.vias.at(q.second).run;
            near.emplace_back(ri, rj, v);
            if (ri != rj) near.emplace_back(rj, ri, v);
        }
    }
    if (runsOut != nullptr && pOut != nullptr) {
        // (The charges unknowns of their own: the parts alone, which the
        //  loop-tree solve factorizes as its own blocks.)
        runsOut->resize(nr, nr);
        runsOut->setFromTriplets(near.begin(), near.end());
        runsOut->makeCompressed();
        *pOut = pNear;
        return {};
    }
    Eigen::SparseMatrix<Complex> charged = transposed * (pNear * incidence);
    charged /= jw;
    Eigen::SparseMatrix<Complex> m(nr, nr);
    m.setFromTriplets(near.begin(), near.end());
    m += charged;
    m.makeCompressed();
    return m;
}

/*!
 * The near field's blocks with the charges as unknowns of their own
 * (solveCompressed()), factorized at one frequency and kept for the next
 * on its thread. They precondition alone, and are made so that they scale
 * with the frequency by themselves: the tree runs' block as jw times its
 * own, jw N_tt + A_t^T P A_t, its N_tt under 10^-10 of the rest wherever
 * the charges are unknowns of their own and P all but static there - its
 * right-hand side times the frequency's own jw -; the loops' with N over
 * its mean diagonal, each frequency's own mean dividing what it is given.
 * So a lower frequency takes a higher one's as they are, its own near
 * field made and multiplied fresh. Where its metals' loss and their jw L
 * change places across the sweep the loops' block moves with them, and the
 * iterations grow - on the thick comb of 1 µm fingers, 100 at 1 GHz where
 * they were made, then 103, 131 and 199 a frequency at a time down to 30
 * MHz, where each made fresh took 70 to 86. So they are outgrown once a
 * frequency on them takes more iterations past theirs than kReuseSlack and
 * than kReuseShare of theirs: there factorizing cost some 30 iterations,
 * 2.6 s at 0.085 s each - 0.3 of the blocks' 100 -, and the excess grew
 * threefold a step, so that past half of that the next frequency's is like
 * to cost the whole. A frequency short on them factorizes its own and is
 * solved again before its charges are eliminated. One a thread: a sweep's
 * frequencies side by side never share one, and some LUs' solves are not
 * to be called from two threads at once; and each thread's frequencies
 * dealt to it in turn, not as it comes free (fullWaveOf()), so that which
 * blocks a frequency is solved on is the sweep's and its threads' alone.
 */
struct NearBlocks {
    fem::SparseLu<Complex> tree, loops;
    bool held = false;   ///< factorized, and not yet outgrown
    int iterations = 0;  ///< the most a port took at the frequency that factorized them
};

/*!
 * One frequency solved compressed: the matrices filled block by block,
 * each low rank where it can be; the near field's LU; each port's column
 * by GMRES, preconditioned by it; and the ports' Z from the currents
 * variationally,
 *
 *     Z = C - (G^T I + I^T G - I^T M I)
 *
 * G the columns' drives (A^T P J / jw, less the vias' potential at the
 * ports and the ports' paths' coupling, addPortPaths()) and C their
 * potential of their own charge, J^T P J / jw, and their paths' own: the
 * dense solve's C - G^T M^-1 G where I is M^-1 G, and its error the
 * square of I's - symmetric, where M is.
 */
AtFrequency solveCompressed(const Context& c, const Compression& z, double f, int threads, const FullWaveControl* control,
                            double* shortAt = nullptr, NearBlocks* kept = nullptr)
{
    const std::atomic_bool* stop = control != nullptr ? control->stop : nullptr;
    const int most = control != nullptr && control->mostIterations > 0 ? control->mostIterations : kMostIterations;
    // (Its progress for the bar: the most iterations of the frequencies
    //  under way, and the residual of that one.)
    const auto progress = [control](int iterations, double residual) {
        if (control == nullptr || control->gmresIterations == nullptr) return;
        int was = control->gmresIterations->load();
        while (iterations > was && !control->gmresIterations->compare_exchange_weak(was, iterations)) {
        }
        if (iterations >= was && control->gmresResidual != nullptr) control->gmresResidual->store(residual);
    };
    AtFrequency out;
    const Kernels k(c, f, threads);
    if (!k.problems.isEmpty()) {
        out.problems << k.problems;
        return out;
    }
    const Complex jw = k.jw;
    const int nr = c.nr, nc = c.nc, nv = int(c.vias.size()), columns = c.np;
    HMatrix kx = z.kx, ky = z.ky, p = z.p, phi = z.phi, g = z.g;
    const bool filled = kx.fill([&k, &z](int a, int b) { return k.inductance(z.xRun.at(a), z.xRun.at(b)); }, z.tolerance, threads, stop)
                        && ky.fill([&k, &z](int a, int b) { return k.inductance(z.yRun.at(a), z.yRun.at(b)); }, z.tolerance, threads,
                                   stop)
                        && p.fill([&k](int i, int j) { return k.potential(i, j); }, z.tolerance, threads, stop)
                        && phi.fill([&k](int cell, int v) { return k.viaPotential(v, cell); }, z.tolerance, threads, stop)
                        && g.fill([&k](int i, int j) { return 0.5 * (k.viaCoupling(i, j) + k.viaCoupling(j, i)); }, z.tolerance,
                                  threads, stop);
    if (!filled) return out;
    out.bytes = kx.bytes() + ky.bytes() + p.bytes() + phi.bytes() + g.bytes();
    out.largestRank = std::max({kx.largestRank(), ky.largestRank(), p.largestRank(), phi.largestRank(), g.largestRank()});

    std::vector<Eigen::Triplet<Complex>> own;
    ownImpedances(c, f, [&own](int a, int b, Complex v) { own.emplace_back(a, b, v); });
    // (The ground's share in the operator whole, and in the near field's
    //  LU only among the runs it holds near: its pairs reach ten heights,
    //  and the LU would fill with them.)
    std::vector<Eigen::Triplet<Complex>> withGround = own;
    if (!c.groundPairs.isEmpty()) {
        QSet<qint64> near;
        for (const QPair<int, int>& r : z.nearRuns) {
            near.insert((qint64(r.first) << 32) | qint64(r.second));
            near.insert((qint64(r.second) << 32) | qint64(r.first));
        }
        groundImpedances(c, f, [&](int a, int b, Complex v) {
            withGround.emplace_back(a, b, v);
            if (a == b || near.contains((qint64(a) << 32) | qint64(b))) own.emplace_back(a, b, v);
        });
    }
    Eigen::SparseMatrix<Complex> d(nr, nr);
    d.setFromTriplets(withGround.begin(), withGround.end());
    // The charges' term against the rest of a run's own (N4), estimated from
    // the kernels on runs spread over the layout - this solve holds no
    // diagonal to read it from: the rest a run's jw L and its sheet's
    // resistance. Before the solve, which it chooses.
    {
        const int np0 = c.planar;
        const int step = std::max(1, np0 / 64);
        for (int a = 0; a < np0; a += step) {
            const int t = c.tail.at(a), h = c.head.at(a);
            if (t < 0 || h < 0) continue;
            // (With a correction held as its factors, its four entries too:
            //  potential() leaves it to P's products - addFactored().)
            const double factored = c.corrected && c.dielectric.factored ? c.dielectric.at(t, t) - c.dielectric.at(t, h)
                                                                               - c.dielectric.at(h, t) + c.dielectric.at(h, h)
                                                                         : 0.0;
            const Complex charge = overJw(k.potential(t, t) - k.potential(t, h) - k.potential(h, t) + k.potential(h, h) + factored, jw);
            const Filament& run = c.runs.at(a);
            const Complex own = run.width > 0 ? sheetImpedance(run.sigma, run.thickness, f) * (run.length() / run.width) : Complex(0, 0);
            const double rest = std::abs(jw * k.inductance(a, a) + own);
            if (rest > 0) out.chargeOverSeries = std::max(out.chargeOverSeries, std::abs(charge) / rest);
        }
    }
    // At a low frequency the charges unknowns of their own, as the dense
    // solve keeps them (kAugmentedFrom): eliminated, GMRES lost the currents
    // in the charges' term as the dense LU did, and a loop at 1 kHz past the
    // dense limit was said past its limit and pointed to the loop
    // inductance (step 5's review).
    bool augmented =
        out.chargeOverSeries > (control != nullptr && control->augmentedFrom > 0 ? control->augmentedFrom : kAugmentedFrom);
    // (Not tried where it fell short at a smaller ratio of the charges'
    //  term in this sweep, a higher frequency: the ratio only grows as the
    //  frequency falls, and an open line's sweep spent kAugmentedMost
    //  iterations a frequency on it - the review of
    //  0a6ae44e. Of the higher frequencies its own thread has solved, as
    //  each thread's are dealt to it in turn, not of what the others have
    //  got to: whether it is tried the sweep's and its threads' alone, the
    //  same to the bit from one run to the next. Each thread falls short
    //  once at most before it skips - as the threads under way did when the
    //  record was the sweep's -, and none waits on another's.)
    if (augmented && shortAt != nullptr && out.chargeOverSeries >= *shortAt) {
        augmented = false;
        out.augmentedShortAt = *shortAt;
    }
    System system{c, z, kx, ky, p, phi, g, d, jw, !augmented};
    fem::SparseLu<Complex> lu;
    const Operator precondition = [&lu](const VectorC& x, VectorC& y) { y = lu.solve(x); };

    // The drive: each column's potentials P J, and the vias' potential at
    // its cells.
    MatrixC pd(nc, columns), ph = MatrixC::Zero(nv, columns), rhs(nr, columns), pathZ;
    for (int q = 0; q < columns; ++q) {
        const VectorC dq = c.drive.col(q);
        VectorC t = VectorC::Zero(nc);
        p.multiply(dq, t);
        addFactored(c, dq, t);
        pd.col(q) = t;
        if (nv == 0) continue;
        VectorC u = VectorC::Zero(nv);
        phi.multiplyTransposed(dq, u);
        ph.col(q) = u;
    }
    // (Where a port has more than one column, the columns solved for:
    //  Context::basis - the drive's terms taken in the columns as they are,
    //  then into these, each linear in the columns' drives.)
    const bool based = c.basis.size() > 0;
    const MatrixC drive = based ? MatrixC(c.drive * c.basis) : c.drive;
    MatrixC current(nr, columns), applied(nr, columns);
    // The charges unknowns of their own, in the loop-tree basis (LoopTree):
    // the runs' currents its loops' and its tree runs', and each cell's
    // charge as the current it takes, Q' = jw Q -
    //     [ L^T M L   L^T M T       0      ] [i ]   [ L^T e ]
    //     [ T^T M L   T^T M T  -T^T P / jw ] [t ] = [ T^T e ]
    //     [    0       A T         1       ] [Q']   [   J   ]
    // M the runs' part, the vias' with it; L^T A^T, the charges' term
    // around a loop, nought (and left out). Its rows scaled to an ampere: a
    // loop's by its run's near field's own, a tree run's by jw over its
    // cell's own potential, whose charges' term is then the current the
    // charge takes - its potentials' rounding, 10^-5 V of 10^11 at 1 Hz on an
    // open line, then a part in 10^16 of it, where weighed against the drops
    // along the runs it was all of theirs; preconditioned by the near field
    // in that basis, its blocks factorized apart (below).
    const int na = nr + nc;
    const auto pOf = [&p, &c, nc](const VectorC& q) {
        VectorC v = VectorC::Zero(nc);
        p.multiply(q, v);
        addFactored(c, q, v);
        return v;
    };
    const LoopTree loops(c);
    Eigen::VectorXd scale = Eigen::VectorXd::Ones(nr);
    const auto dropOf = [&c](const VectorC& v, int a) { return (c.tail.at(a) >= 0 ? v[c.tail.at(a)] : Complex(0, 0)) - v[c.head.at(a)]; };
    const Operator looped = [&](const VectorC& x, VectorC& y) {
        const VectorC i = loops.currents(x.head(nr));
        VectorC n;
        system(i, n);
        const VectorC v = pOf(x.tail(nc) / jw);
        const VectorC around = loops.transposed(n);
        y.resize(na);
        for (int a = 0; a < nr; ++a) y[a] = scale[a] * (loops.tree(a) ? n[a] - dropOf(v, a) : around[a]);
        y.tail(nc) = loops.charges(x.head(nr)) + x.tail(nc);
    };
    // Preconditioned by the near field in the loop-tree basis - as its
    // blocks, not as its augmented system in the runs' own: there a run's row
    // off the tree holds its cells' P / jw, some 10^16 times its own jw L at
    // 1 Hz on a 100 µm cell, which cancels only round a loop, and the loops'
    // currents its LU gave were that cancellation's rounding - in the
    // loop-tree basis its answer left 83 times what it was given at 10 kHz
    // on a coplanar line, 5e9 times at 1 Hz, and 1.8e13 times on a MIM's
    // plates 40 nm apart. Where the far field is small - a ground plane under
    // the line - GMRES made do with that to some kilohertz; where it is not,
    // it took the rounding for the operator and was lost: a ground-signal-
    // signal-ground line with no plane, every conductor floating, left a true
    // residual of 3.7 after 300 iterations at 10 kHz, the eliminated system no
    // better in 3000, more iterations changing nothing; the MIM fell back to
    // the eliminated system at 1 kHz and was said to be past its limit. Its
    // blocks:
    //  - the tree runs and the charges: Q' = J - A_t t into the tree runs'
    //    rows leaves (N_tt + A_t^T P A_t / jw) t, the eliminated near field
    //    on the tree alone - no loop in it, nothing to cancel;
    //  - then the loops: the current round them, divergence-free, of
    //        [ N   -A^T ] [I]   [g]
    //        [ A     0  ] [psi] = [0]
    //    g the loops' rows less N's of the tree runs' currents, psi what holds
    //    the current to its loops - no P in it, its size the runs' own drops;
    //    each floating component's root's row of A, which its others' sum,
    //    psi there held at nought instead.
    // Left out, the loops' own drive along the tree runs: their jw L against
    // the tree runs' P / jw, under 10^-10 of it wherever the charges are
    // unknowns of their own (kAugmentedFrom). Refined against the near field
    // taken whole in the loop-tree basis, the floating line took 42
    // iterations for 45, the rest as many: left out, the preconditioner one
    // linear operator, as GMRES wants it.
    Eigen::SparseMatrix<Complex> nearRuns, nearP;
    // (Kept from a higher frequency on this thread where it may be:
    //  NearBlocks.)
    NearBlocks alone;
    NearBlocks& blocks = kept != nullptr && (control == nullptr || control->reuseNear) ? *kept : alone;
    fem::SparseLu<Complex>& treeLu = blocks.tree;
    fem::SparseLu<Complex>& loopLu = blocks.loops;
    bool reused = false;
    std::vector<int> treeRuns;
    for (int a = 0; a < nr; ++a)
        if (loops.tree(a)) treeRuns.push_back(a);
    const int nt = int(treeRuns.size());
    double loopScale = 1;
    const Operator loopPrecondition = [&](const VectorC& x, VectorC& y) {
        const VectorC rr = x.head(nr).cwiseQuotient(scale.cast<Complex>());
        const VectorC v = nearP * (x.tail(nc) / jw);
        VectorC rt(nt);
        for (int j = 0; j < nt; ++j) rt[j] = rr[treeRuns[std::size_t(j)]] + dropOf(v, treeRuns[std::size_t(j)]);
        const VectorC t = nt > 0 ? VectorC(treeLu.solve(jw * rt)) : VectorC();
        VectorC along = VectorC::Zero(nr);
        for (int j = 0; j < nt; ++j) along[treeRuns[std::size_t(j)]] = t[j];
        VectorC b = VectorC::Zero(na);
        b.head(nr) = -(nearRuns * along);
        for (int a = 0; a < nr; ++a)
            if (!loops.tree(a)) b[a] += rr[a];
        b.head(nr) /= loopScale;
        const VectorC w = loopLu.solve(b);
        y.resize(na);
        for (int a = 0; a < nr; ++a) y[a] = loops.tree(a) ? along[a] : w[a];
        y.tail(nc) = x.tail(nc) - loops.charges(along);
    };
    const int augmentedMost = control != nullptr && control->augmentedMost > 0 ? control->augmentedMost : kAugmentedMost;
    MatrixC both;
    // Augmented first where it is asked; short of its residual in
    // kAugmentedMost iterations, eliminated, as before.
    for (bool again = true; again;) {
        again = false;
        system.charges = !augmented;
        const fem::SparseLu<Complex>* failed = nullptr;
        if (augmented) {
            (void)nearField(c, z, k, own, &nearRuns, &nearP);
            const VectorC diagonal = nearRuns.diagonal();
            for (int a = 0; a < nr; ++a) {
                const double size = loops.tree(a) ? std::abs(k.potential(c.head.at(a), c.head.at(a))) / std::abs(jw)
                                                  : std::abs(diagonal[a]);
                scale[a] = size > 0 ? 1 / size : 1.0;
            }
            // (The blocks, of the near field's parts: above - kept from a
            //  higher frequency where they hold, NearBlocks.)
            double sum = 0;
            for (int a = 0; a < nr; ++a) sum += std::abs(diagonal[a]);
            loopScale = sum > 0 ? sum / nr : 1.0;
            reused = blocks.held;
        }
        if (augmented && !reused) {
            blocks.held = false;
            std::vector<Eigen::Triplet<Complex>> terms;
            for (int a = 0; a < nr; ++a) {
                if (c.tail.at(a) >= 0) terms.emplace_back(c.tail.at(a), a, Complex(1, 0));
                terms.emplace_back(c.head.at(a), a, Complex(-1, 0));
            }
            Eigen::SparseMatrix<Complex> incidence(nc, nr), pick(nr, nt);
            incidence.setFromTriplets(terms.begin(), terms.end());
            terms.clear();
            for (int j = 0; j < nt; ++j) terms.emplace_back(treeRuns[std::size_t(j)], j, Complex(1, 0));
            pick.setFromTriplets(terms.begin(), terms.end());
            const Eigen::SparseMatrix<Complex> onTree = incidence * pick;
            Eigen::SparseMatrix<Complex> tree = Eigen::SparseMatrix<Complex>(pick.transpose()) * nearRuns * pick;
            tree *= jw;
            const Eigen::SparseMatrix<Complex> charged = Eigen::SparseMatrix<Complex>(onTree.transpose()) * (nearP * onTree);
            tree += charged;
            tree.makeCompressed();
            if (nt > 0) treeLu.compute(tree);
            // (The loops' rows scaled to the runs' own sizes, so that psi is
            //  of A's: loopScale, above.)
            std::vector<char> pinned(std::size_t(nc), 0);
            for (int m = 0; m < loops.components(); ++m)
                if (loops.rootCell(m) >= 0) pinned[std::size_t(loops.rootCell(m))] = 1;
            terms.clear();
            for (int j = 0; j < nearRuns.outerSize(); ++j)
                for (Eigen::SparseMatrix<Complex>::InnerIterator it(nearRuns, j); it; ++it)
                    terms.emplace_back(int(it.row()), int(it.col()), it.value() / loopScale);
            for (int j = 0; j < incidence.outerSize(); ++j)
                for (Eigen::SparseMatrix<Complex>::InnerIterator it(incidence, j); it; ++it) {
                    const int cell = int(it.row());
                    terms.emplace_back(j, nr + cell, -it.value());
                    if (pinned[std::size_t(cell)] == 0) terms.emplace_back(nr + cell, j, it.value());
                }
            for (int i = 0; i < nc; ++i)
                if (pinned[std::size_t(i)] != 0) terms.emplace_back(nr + i, nr + i, Complex(1, 0));
            Eigen::SparseMatrix<Complex> held(na, na);
            held.setFromTriplets(terms.begin(), terms.end());
            held.makeCompressed();
            loopLu.compute(held);
            failed = nt > 0 && treeLu.info() != Eigen::Success ? &treeLu : loopLu.info() != Eigen::Success ? &loopLu : nullptr;
            blocks.held = failed == nullptr;
            blocks.iterations = 0;
        } else if (!augmented) {
            lu.compute(nearField(c, z, k, own));
            failed = lu.info() != Eigen::Success ? &lu : nullptr;
        }
        if (failed != nullptr) {
            out.problems << tr("At %1 GHz the near field's LU, which the compressed solve is preconditioned with, failed: %2")
                                .arg(f / 1e9, 0, 'g', 4)
                                .arg(QString::fromStdString(failed->lastErrorMessage()));
            return out;
        }
        // (With the charges as unknowns, the rest alone, e: Q holds
        //  A^T P J / jw.)
        rhs.setZero();
        if (!augmented)
            for (int a = 0; a < nr; ++a)
                for (int q = 0; q < columns; ++q)
                    rhs(a, q) = ((c.tail.at(a) >= 0 ? pd(c.tail.at(a), q) : Complex(0, 0)) - pd(c.head.at(a), q)) / jw;
        for (int j = 0; j < nv; ++j)
            for (int q = 0; q < columns; ++q) rhs(c.vias.at(j).run, q) -= ph(j, q);
        pathZ = MatrixC::Zero(columns, columns);
        addPortPaths(c, k, rhs, pathZ);
        addSourceFields(c, k, rhs, pathZ);
        if (based) {
            rhs = rhs * c.basis;
            pathZ = c.basis.transpose() * pathZ * c.basis;
        }
        if (augmented) {
            both.resize(na, columns);
            for (int q = 0; q < columns; ++q) {
                both.col(q).head(nr) = loops.transposed(rhs.col(q)).cwiseProduct(scale.cast<Complex>());
                both.col(q).tail(nc) = drive.col(q);
            }
        }
        out.iterations = 0;
        out.residual = 0;
        for (int q = 0; q < columns; ++q) {
            VectorC x = VectorC::Zero(augmented ? na : nr), mx;
            const Gmres solved = augmented ? gmres(looped, loopPrecondition, both.col(q), x, kResidual, kRestart, std::min(most, augmentedMost),
                                                   stop, progress)
                                           : gmres(system, precondition, rhs.col(q), x, kResidual, kRestart, most, stop, progress);
            if (solved.stopped) return AtFrequency();
            out.iterations = std::max(out.iterations, solved.iterations);
            out.residual = std::max(out.residual, solved.residual);
            if (augmented) {
                // (Short on a higher frequency's blocks: its own made, and
                //  tried again, before the charges are eliminated.)
                if (!solved.converged && reused) {
                    blocks.held = false;
                    again = true;
                    break;
                }
                if (!solved.converged) {
                    blocks.held = false;
                    out.augmentedTried = out.iterations;
                    if (shortAt != nullptr) *shortAt = std::min(*shortAt, out.chargeOverSeries);
                    augmented = false;
                    again = true;
                    break;
                }
                both.col(q) = x;   // (its drive read, and done with)
                current.col(q) = loops.currents(x.head(nr));
                continue;
            }
            current.col(q) = x;
            system(x, mx);
            applied.col(q) = mx;
        }
    }
    // (The blocks' own iterations where they were made; outgrown where a
    //  frequency on them takes past those more than kReuseSlack and
    //  kReuseShare of them.)
    if (augmented) {
        if (!reused)
            blocks.iterations = out.iterations;
        else {
            out.reusedFrom = std::max(1, blocks.iterations);
            const int past = out.iterations - blocks.iterations;
            if (past > kReuseSlack && past > kReuseShare * blocks.iterations) blocks.held = false;
        }
    }
    // Short of its residual: where what it left is small - the ports' Z is
    // variational, its error the residual's square over how near singular
    // the system is (the eliminated system's: with the charges as unknowns,
    // short of kResidual it fell back above) - the answer stands, said
    // doubtful, and the sweep goes on; past kTrustedResidual, or not finite, it is no answer. A sweep
    // stopped at the first frequency that fell short, with nothing of the
    // rest.
    if (out.residual > kTrustedResidual || !current.allFinite()) {
        // (What to do about it, as it can be done: dense only where the
        //  system is not past what a dense solve is set up for, and at a low
        //  frequency - the charges' term far over the currents' - the loop
        //  inductance, which solves the currents alone at direct current:
        //  "solve it dense" was said of 11,800 runs that dense refused, a loop
        //  at 1 kHz.)
        QStringList ways;
        if (out.chargeOverSeries > 1e6)
            ways << tr("at a frequency this low its charges' term outweighs its currents' %1 times, which the iteration loses "
                       "them in - rf25_inductance gives L and R at direct current, of the currents alone, or start the "
                       "sweep higher")
                        .arg(out.chargeOverSeries, 0, 'g', 2);
        const int denseMost = control != nullptr && control->mostUnknowns > 0 ? control->mostUnknowns : denseLimit();
        if (c.nr + c.nc <= denseMost) ways << tr("solve it dense (the study's Matrix)");
        ways << tr("mesh it differently");
        QString what = ways.join(QStringLiteral("; "));
        what[0] = what.at(0).toUpper();
        out.problems << tr("At %1 GHz the compressed solve did not converge: GMRES left a residual of %2 after %3 iterations. "
                           "%4.")
                            .arg(f / 1e9, 0, 'g', 4)
                            .arg(out.residual, 0, 'g', 2)
                            .arg(out.iterations)
                            .arg(what);
        return out;
    }
    if (out.residual > 1e3 * kResidual)
        out.warnings << tr("At %1 GHz GMRES stopped short of its residual: %2 after %3 iterations, where %4 is asked. S "
                           "there is near, not to the last digits - more so at low frequencies; a coarser mesh, or the "
                           "matrix dense, solves it whole.")
                            .arg(f / 1e9, 0, 'g', 4)
                            .arg(out.residual, 0, 'g', 2)
                            .arg(out.iterations)
                            .arg(kResidual, 0, 'g', 2);
    else if (out.iterations >= kNearMiss * most)
        out.warnings << tr("At %1 GHz GMRES took %2 of its %3 iterations: near its limit, where a finer mesh or a higher "
                           "frequency may not converge.")
                            .arg(f / 1e9, 0, 'g', 4)
                            .arg(out.iterations)
                            .arg(most);
    out.current = current;
    if (augmented) {
        // Each port's voltage J^T V + its path's - e^T I, as the dense
        // augmented solve's, V taken along the tree (LoopTree::voltages()):
        // of P Q's differences, P a part in 10^5 compressed, the 0.1 ohm
        // along a 2 mm line was 10^6 ohm off at 1 Hz.
        out.z = pathZ;
        for (int q = 0; q < columns; ++q) {   // (the port driven)
            VectorC n;
            system(current.col(q), n);
            out.z.col(q) += loops.voltages(drive, n - rhs.col(q), pOf(both.col(q).tail(nc) / jw));
        }
        out.z -= rhs.transpose() * current;
        out.augmented = true;
        out.loopTree = true;
        return out;
    }
    out.z = drive.transpose() * (based ? MatrixC(pd * c.basis) : pd) / jw + pathZ
            - (rhs.transpose() * current + current.transpose() * rhs - current.transpose() * applied);
    return out;
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

MatrixC matrixOf(const QList<QList<Complex>>& rows)
{
    const int n = int(rows.size());
    MatrixC m(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) m(i, j) = rows.at(i).at(j);
    return m;
}

QString num(double v)
{
    return QString::number(v, 'g', 12);
}

/// A frequency taken from a test's allowance (FullWaveControl::allowed),
/// waiting while it has none: false where the solve is stopped meanwhile.
bool allowedOne(const FullWaveControl* control)
{
    std::atomic_int* allowed = control != nullptr ? control->allowed : nullptr;
    if (allowed == nullptr) return true;
    for (int n = allowed->load();; n = allowed->load()) {
        if (n < 0) return true;
        if (n > 0) {
            if (allowed->compare_exchange_weak(n, n - 1)) return true;
            continue;
        }
        if (control->stop != nullptr && control->stop->load()) return false;
        QThread::msleep(2);
    }
}

} // namespace

void setDenseLimit(int runsAndCells)
{
    denseLimitSet = std::max(0, runsAndCells);
}

void setSweepMemory(double bytes)
{
    sweepMemorySet = std::isfinite(bytes) && bytes > 0 ? bytes : 0.0;
}

int automaticDenseLimit()
{
    return kMostUnknowns;
}

double automaticSweepMemory()
{
    return std::max(kSweepMemory, physicalMemory() / 4);
}

int denseLimit()
{
    const int set = denseLimitSet.load();
    return set > 0 ? set : automaticDenseLimit();
}

double sweepMemory()
{
    const double set = sweepMemorySet.load();
    return set > 0 ? set : automaticSweepMemory();
}

FillShared fillShared()
{
    FillShared out;
    out.greens = sharedGreens.load();
    out.columns = sharedColumns.load();
    out.statics = sharedStatics.load();
    out.keptRuns = keptRuns.load();
    out.keptCells = keptCells.load();
    return out;
}

double metalHeight(const Stack& stack, const QString& name)
{
    if (stack.isGround(name)) return 0;
    double z = 0;
    for (const Layer& l : stack.layers) {
        if (l.name == name || l.tag == name) return !l.isConductor() ? -1.0 : l.kind == Layer::Ground ? 0.0 : z;
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness;
    }
    return -1;
}

double Feed::offset() const
{
    double rows = -1, walls = 0;
    for (const Via& v : vias) {
        if (v.wall) walls = std::max(walls, v.in0);
        else rows = rows < 0 ? 0.5 * (v.in0 + v.in1) : std::min(rows, 0.5 * (v.in0 + v.in1));
    }
    return rows >= 0 ? rows : walls;
}

QString Feed::key() const
{
    // A feed and its mirror image across its line are the same feed - the
    // two ends of a line see each other's so - and share one calibration.
    const auto n = [](double v) { return QString::number(v, 'g', 9); };
    const auto text = [this, &n](bool mirror) {
        QStringList parts = {metal, n(width)};
        if (coplanar) {
            const int a = mirror ? 1 : 0, b = 1 - a;
            parts << QStringLiteral("coplanar") << n(gap[a]) << n(ground[a]) << n(gap[b]) << n(ground[b]) << n(pitch)
                  << unlike;
            QStringList each;
            for (const Via& v : vias)
                each << QStringList{v.layer, n(mirror ? width - v.across1 : v.across0), n(mirror ? width - v.across0 : v.across1),
                                    n(v.in0), n(v.in1), v.wall ? QStringLiteral("wall") : QString()}
                            .join(QLatin1Char(','));
            each.sort();
            parts += each;
        }
        // (Its returns, where it names them: their metals and stretches.)
        if (!returns.isEmpty()) {
            QStringList each;
            for (const Return& r : returns)
                each << QStringList{r.metal, n(mirror ? width - r.across1 : r.across0), n(mirror ? width - r.across0 : r.across1)}.join(QLatin1Char(','));
            each.sort();
            parts << QStringLiteral("returns") << each << unlike;
        }
        // (The dielectric it stands on, where a layer ends: the same feed on
        //  another mesa is another line.)
        QStringList under;
        for (const Extent& e : extents) {
            QStringList spans;
            for (const QPolygonF& s : e.shapes) {
                const QRectF r = s.boundingRect();
                spans << (mirror ? n(width - r.bottom()) + QLatin1Char(',') + n(width - r.top())
                                 : n(r.top()) + QLatin1Char(',') + n(r.bottom()));
            }
            spans.sort();
            under << QStringList{e.layer, n(e.beyond), spans.join(QLatin1Char(';'))}.join(QLatin1Char(':'));
        }
        under.sort();
        if (!under.isEmpty()) parts << QStringLiteral("under") << under;
        return parts.join(QLatin1Char('|'));
    };
    return std::min(text(false), text(true));
}

int mostDenseCorrection()
{
    const int set = g_mostDenseCorrection.load();
    return set > 0 ? set : kMostDenseCorrection;
}

void setMostDenseCorrection(int cells)
{
    g_mostDenseCorrection.store(std::max(0, cells));
}

Complex sheetImpedance(double sigma, double thickness, double frequency)
{
    // Half the slab of half the thickness: the current divides between the
    // two faces, each carrying it in from its own side.
    if (!(sigma > 0) || !(thickness > 0)) return Complex(0, 0);
    return 0.5 * surfaceImpedance(sigma, thickness / 2, frequency);
}

QList<QList<Complex>> scatteringOf(const QList<QList<Complex>>& z, double reference)
{
    return scatteringOf(z, QList<double>(z.size(), reference));
}

QList<QList<Complex>> scatteringOf(const QList<QList<Complex>>& z, const QList<double>& references)
{
    const int n = int(z.size());
    if (references.size() != n) return {};
    MatrixC a = matrixOf(z), b = a;
    Eigen::VectorXd root(n);
    for (int i = 0; i < n; ++i) {
        a(i, i) -= references.at(i);
        b(i, i) += references.at(i);
        root(i) = std::sqrt(std::max(0.0, references.at(i)));
    }
    // (Z - R)(Z + R)^-1, the inverse on the right - with references that
    // differ the two do not commute - as (B^T \ A^T)^T; then scaled to
    // power waves.
    MatrixC s = Eigen::PartialPivLU<MatrixC>(b.transpose()).solve(a.transpose()).transpose();
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) s(i, j) *= root(j) / root(i);
    return rowsOf(s);
}

FullWave fullWaveOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, const QList<QPointF>& ports,
                    const QList<double>& frequencies, double reference, double micrometre)
{
    QList<FullWavePort> defined;
    for (int i = 0; i < ports.size(); ++i) {
        FullWavePort port;
        port.name = QStringLiteral("P%1").arg(i + 1);
        port.at = ports.at(i);
        port.impedance = reference;
        defined << port;
    }
    return fullWaveOf(mesh, conductors, stack, defined, frequencies, micrometre);
}

double secondSurfaceWave(const Stack& stack, double most)
{
    const Layers layers = layersOf(stack);
    if (!layers.ok() || !(most > 0)) return 0;
    // At its cutoff a surface wave grazes the half-space over the stack, its
    // kz there nought and its TE line open; in each slab kz = k0 (eps - eps
    // above)^1/2. The TE chain matrix from the bottom up, [A, jB; jC, D]
    // (A and D real, B and C real once their j is out): over a ground, V
    // nought at the bottom and I at the top, A nought - a quarter wave in
    // one slab; with none, I nought at both ends, C nought past the TE0 that
    // has no cutoff - a half wave. (With something denser below than above
    // the wave grazes that, which this does not follow.)
    // (Under a metal lid there is no half-space to graze: the second wave is
    //  the guide's TE1 between the ground and the lid, at its cutoff kr
    //  nought, kz = k0 eps^1/2 in each slab and V nought at both ends - B
    //  nought. Without a ground it has none of its own below.)
    const bool lid = layers.groundAbove;
    if (lid && !layers.groundBelow) return 0;
    const double above = lid ? 0.0 : layers.epsrAbove;
    if (!lid && !layers.groundBelow && layers.epsrBelow > above + 1e-9) return 0;
    const double c0 = 1.0 / std::sqrt(kMu0 * kEps0);
    double rate = 0;   // d(sum of kz h)/df
    for (const Layers::Slab& slab : layers.slabs)
        rate += 2 * M_PI / c0 * std::sqrt(std::max(0.0, slab.epsr - above)) * (slab.z1 - slab.z0);
    if (!(rate > 0)) return 0;
    const bool grounded = layers.groundBelow;
    const auto resonance = [&layers, above, grounded, lid, c0](double f) {
        const double k0 = 2 * M_PI * f / c0, w = 2 * M_PI * f;
        double a = 1, b = 0, c = 0, d = 1;
        for (const Layers::Slab& slab : layers.slabs) {
            const double kz = k0 * std::sqrt(std::max(0.0, slab.epsr - above)), h = slab.z1 - slab.z0, angle = kz * h;
            const double cosine = std::cos(angle);
            const double series = kz > 0 ? w * kMu0 / kz * std::sin(angle) : w * kMu0 * h;
            const double shunt = kz > 0 ? std::sin(angle) * kz / (w * kMu0) : 0;
            const double na = a * cosine - b * shunt, nb = a * series + b * cosine;
            const double nc = c * cosine + d * shunt, nd = d * cosine - c * series;
            a = na;
            b = nb;
            c = nc;
            d = nd;
        }
        return lid ? b : grounded ? a : c;
    };
    // Stepped a twentieth of a radian of the whole stack at a time, then
    // halved down to the root.
    const double step = 0.05 / rate;
    double lo = step, before = resonance(lo);
    for (double f = 2 * step; f <= most; f += step) {
        const double now = resonance(f);
        if ((before > 0) != (now > 0)) {
            double hi = f;
            for (int i = 0; i < 60; ++i) {
                const double mid = 0.5 * (lo + hi);
                if ((resonance(mid) > 0) == (before > 0)) lo = mid;
                else hi = mid;
            }
            return 0.5 * (lo + hi);
        }
        lo = f;
        before = now;
    }
    return 0;
}

FullWave foldLumped(const FullWave& wave, const QList<FullWavePort>& ports, const QList<int>& lumped)
{
    FullWave out = wave;
    const int n = int(wave.portNames.size());
    QList<int> keep;
    for (int p = 0; p < n; ++p)
        if (!lumped.contains(p)) keep << p;
    const int np = int(keep.size()), nl = int(lumped.size());
    // Each element's (A, B) at f: A V + B I = 0 on its port.
    const auto ends = [&](int e, double f, Complex* a, Complex* b) {
        const FullWavePort::Lumped& el = ports.at(lumped.at(e)).lumped;
        const Complex jw(0, 2 * M_PI * f);
        if (el.parallel) {
            *a = (el.r > 0 ? 1 / el.r : 0.0) + (el.l > 0 ? 1.0 / (jw * el.l) : Complex(0, 0)) + jw * el.c;
            *b = 1;
        } else {
            *a = 1;
            *b = el.r + jw * el.l + (el.c > 0 ? 1.0 / (jw * el.c) : Complex(0, 0));
        }
    };
    // Z folded at frequency f, and the elements' currents of each measuring
    // port's ampere.
    const auto fold = [&](const QList<QList<Complex>>& z, double f, MatrixC* currents) {
        MatrixC zpp(np, np), zpl(np, nl), zlp(nl, np), zll(nl, nl);
        for (int i = 0; i < np; ++i) {
            for (int j = 0; j < np; ++j) zpp(i, j) = z.at(keep.at(i)).at(keep.at(j));
            for (int j = 0; j < nl; ++j) zpl(i, j) = z.at(keep.at(i)).at(lumped.at(j));
        }
        for (int i = 0; i < nl; ++i) {
            for (int j = 0; j < np; ++j) zlp(i, j) = z.at(lumped.at(i)).at(keep.at(j));
            for (int j = 0; j < nl; ++j) zll(i, j) = z.at(lumped.at(i)).at(lumped.at(j));
        }
        MatrixC a = MatrixC::Zero(nl, nl), b = MatrixC::Zero(nl, nl);
        for (int e = 0; e < nl; ++e) ends(e, f, &a(e, e), &b(e, e));
        const MatrixC x = Eigen::PartialPivLU<MatrixC>(a * zll + b).solve(a * zlp);
        if (currents != nullptr) *currents = -x;
        return rowsOf(MatrixC(zpp - zpl * x));
    };
    const auto pick = [&keep](auto list) {
        decltype(list) kept;
        for (const int p : keep)
            if (p < list.size()) kept << list.at(p);
        return kept;
    };
    out.z.clear();
    out.s.clear();
    out.currents.clear();
    out.references = pick(wave.references);
    out.gain = 0;
    for (int k = 0; k < wave.frequencies.size(); ++k) {
        MatrixC il;
        const QList<QList<Complex>> z = fold(wave.z.at(k), wave.frequencies.at(k), &il);
        out.z << z;
        out.s << scatteringOf(z, out.references);
        if (k < wave.currents.size()) {
            // Each measuring port's runs: its own, and each element's as much
            // as its ampere drives through it. (None where they were not
            // kept.)
            QList<QList<Complex>> perPort;
            for (int i = 0; i < np && !wave.currents.at(k).isEmpty(); ++i) {
                QList<Complex> column = wave.currents.at(k).at(keep.at(i));
                for (int e = 0; e < nl; ++e)
                    for (int r = 0; r < column.size(); ++r) column[r] += wave.currents.at(k).at(lumped.at(e)).at(r) * il(e, i);
                perPort << column;
            }
            out.currents << perPort;
        }
        const MatrixC s = matrixOf(out.s.last());
        const Eigen::SelfAdjointEigenSolver<MatrixC> eig(s.adjoint() * s);
        if (eig.info() == Eigen::Success && eig.eigenvalues().maxCoeff() - 1 > out.gain) {
            out.gain = eig.eigenvalues().maxCoeff() - 1;
            out.gainAt = wave.frequencies.at(k);
        }
    }
    out.zSolved.clear();
    for (int k = 0; k < wave.zSolved.size() && k < wave.solvedFrequencies.size(); ++k)
        out.zSolved << fold(wave.zSolved.at(k), wave.solvedFrequencies.at(k), nullptr);
    out.portNames = pick(wave.portNames);
    out.faceWidths = pick(wave.faceWidths);
    out.faceMetals = pick(wave.faceMetals);
    out.feeds = pick(wave.feeds);
    out.deembedding = pick(wave.deembedding);
    out.returnsOf = pick(wave.returnsOf);
    out.namesReturns = pick(wave.namesReturns);
    if (wave.portsSaid.size() == n) out.portsSaid = pick(wave.portsSaid);
    out.pairs.clear();
    for (const QPair<int, int>& pq : wave.pairs)
        if (keep.contains(pq.first) && keep.contains(pq.second)) out.pairs << qMakePair(int(keep.indexOf(pq.first)), int(keep.indexOf(pq.second)));
    out.portPaths.clear();
    bool shuntPaths = false;
    for (const FullWave::PortPathLeg& leg : wave.portPaths) {
        if (keep.contains(leg.port)) {
            FullWave::PortPathLeg kept = leg;
            kept.port = int(keep.indexOf(leg.port));
            out.portPaths << kept;
        } else {
            shuntPaths = true;
        }
    }
    QStringList said;
    for (const int p : lumped) {
        const FullWavePort::Lumped& el = ports.at(p).lumped;
        QStringList parts;
        if (el.r > 0) parts << tr("R %1 Ohm").arg(el.r, 0, 'g', 4);
        if (el.l > 0) parts << tr("L %1 nH").arg(el.l * 1e9, 0, 'g', 4);
        if (el.c > 0) parts << tr("C %1 pF").arg(el.c * 1e12, 0, 'g', 4);
        said << tr("%1 (%2, %3)").arg(ports.at(p).name, parts.isEmpty() ? tr("nothing") : parts.join(QStringLiteral(", ")),
                                       el.parallel ? tr("in parallel") : tr("in series"));
    }
    out.lines << tr("Lumped elements, each solved as a port and folded in at every frequency by its own impedance: %1. The "
                    "ports are the measuring ones alone.")
                     .arg(said.join(QStringLiteral("; ")));
    if (shuntPaths)
        out.lines << tr("A shunt element's own path up from the ground is not in the far field's sources.");
    return out;
}

FullWave fullWaveOf(const Mesh& given, const QList<Conductor>& givenConductors, const Stack& givenStack,
                    const QList<FullWavePort>& givenPorts, const QList<double>& frequencies, double micrometre,
                    const FullWaveControl* control, const QList<ReturnMark>& returns)
{
    // Lumped elements: solved as the ports they stand as, then folded.
    QList<int> lumped;
    for (int p = 0; p < givenPorts.size(); ++p)
        if (givenPorts.at(p).lumped.on) lumped << p;
    if (!lumped.isEmpty()) {
        if (lumped.size() == givenPorts.size()) {
            FullWave none;
            none.problems << tr("The model has lumped elements and no port to measure them from.");
            return none;
        }
        QList<FullWavePort> plain = givenPorts;
        for (const int p : std::as_const(lumped)) {
            plain[p].lumped.on = false;
            plain[p].calibrate = false;
            plain[p].shift = 0;
        }
        const FullWave raw = fullWaveOf(given, givenConductors, givenStack, plain, frequencies, micrometre, control, returns);
        return raw.ok() ? foldLumped(raw, givenPorts, lumped) : raw;
    }
    // (A port on a plane of symmetry is moved a hair off it, into the half
    //  solved, below: its face the cut conductor's edge across the plane,
    //  not the cut along it.)
    QList<FullWavePort> ports = givenPorts;
    FullWave out;
    // (What makes the mesh's answer doubtful - touching conductors it has
    //  apart - is the solve's too.)
    out.warnings << given.warnings;
    // A metal solved as its two faces is two sheets and the walls that join
    // them, which is what the rest solves (thickened()); what is handed
    // back is of the mesh as given. Which metals are films, of the
    // conductors whole on a plane of symmetry, as the mesh has them.
    const bool halved = control != nullptr && control->symmetry.axis != 0;
    const QList<Conductor> wholeGiven =
        halved ? withMirrors(givenConductors, control->symmetry.axis, control->symmetry.at / micrometre) : QList<Conductor>();
    const Thickened thick = thickened(givenStack, givenConductors, given, halved ? &wholeGiven : nullptr);
    const Mesh& mesh = thick.mesh;
    const QList<Conductor>& conductors = thick.conductors;
    const Stack& stack = thick.stack;
    if (mesh.cells.isEmpty()) {
        out.problems << tr("There are no cells to solve on: mesh the layout first.");
        return out;
    }
    // Each cell on a layer of the stack that carries a current: one it has
    // not was passed over as reaching nothing, and the solve said "no two
    // cells of the mesh share an edge... mesh the layout more finely" - the quasi-static solve said which.
    {
        QSet<QString> seen;
        for (const MeshCell& cell : mesh.cells) {
            if (seen.contains(cell.layer)) continue;
            seen.insert(cell.layer);
            const Layer* layer = stack.named(cell.layer);
            if (layer == nullptr) {
                out.problems << tr("The stack has no layer %1, which the mesh has cells on.").arg(cell.layer);
                return out;
            }
            if (layer->kind == Layer::Dielectric || layer->kind == Layer::Substrate) {
                out.problems << tr("%1 is a dielectric of the stack, and the mesh has cells on it: only a metal or a via "
                                   "carries a current.")
                                    .arg(cell.layer);
                return out;
            }
        }
    }
    if (ports.isEmpty()) {
        out.problems << tr("A full-wave solve needs a port to drive it: put one on the layout.");
        return out;
    }
    if (frequencies.isEmpty()) {
        out.problems << tr("No frequency to solve at.");
        return out;
    }
    for (const double f : frequencies)
        if (!(f > 0) || !std::isfinite(f)) {
            out.problems << tr("A frequency of %1 Hz cannot be solved at: the full-wave solve needs one above nought "
                               "(at nought the quasi-static extraction answers).")
                                .arg(f);
            return out;
        }
    for (int p = 0; p < ports.size(); ++p) {
        out.portNames << (ports.at(p).name.isEmpty() ? QStringLiteral("P%1").arg(p + 1) : ports.at(p).name);
        out.references << ports.at(p).impedance;
        if (!(ports.at(p).impedance > 0) || !std::isfinite(ports.at(p).impedance)) {
            out.problems << tr("The reference impedance of %1, %2 Ohm, is none to give S parameters in.")
                                .arg(out.portNames.last()).arg(ports.at(p).impedance);
            return out;
        }
    }
    Context c;
    c.layers = layersOf(stack);
    if (!c.layers.ok()) {
        out.problems << c.layers.problems;
        return out;
    }
    // A box (rf25_box.h): its guide's slow modes as the rectangle has them,
    // the rest over the walls' images, out to one side of it. Its ports at
    // its walls, the wall their reference, with no path up from the ground;
    // no layer that ends in it, no via, its runs along x or y - each said.
    if (c.layers.walls) {
        c.boxed = true;
        const QRectF& box = c.layers.box;
        c.boxSplit = control != nullptr && control->boxSplit > 0 ? control->boxSplit : 25 / std::min(box.width(), box.height());
        c.boxReach = 25 / c.boxSplit;
        if (control != nullptr && !control->extents.isEmpty()) {
            out.problems << tr("A layer that ends at its shapes is not solved in a box: its correction knows no walls.");
            return out;
        }
        const double hair = 1e-6 * std::max(box.width(), box.height());
        for (const FullWavePort& port : ports) {
            const QPointF at = port.at * micrometre;
            const double off = std::min({std::abs(at.x() - box.left()), std::abs(at.x() - box.right()), std::abs(at.y() - box.top()),
                                         std::abs(at.y() - box.bottom())});
            if (port.internal || port.terminals || off > hair) {
                out.problems << tr("In a box a port stands at a wall: an edge port on the face of a conductor that meets it, the wall "
                                   "its reference (%1 does not).")
                                    .arg(port.name.isEmpty() ? tr("a port") : port.name);
                return out;
            }
        }
        // (Every shape inside the walls: a strip run past them, or one wholly
        //  outside, came to "nothing finite" at every frequency - a bug hunt's second hour.)
        for (const Conductor& conductor : conductors) {
            const QRectF bounds = conductor.outline.boundingRect();
            const QRectF at(bounds.topLeft() * micrometre, bounds.bottomRight() * micrometre);
            if (at.left() < box.left() - hair || at.right() > box.right() + hair || at.top() < box.top() - hair
                || at.bottom() > box.bottom() + hair) {
                out.problems << tr("In a box every shape is inside its walls, from %1 to %2 µm in x and %3 to %4 µm in y: a shape on "
                                   "%5 spans %6 to %7 µm in x and %8 to %9 µm in y. Cut it at the walls, or make the box larger.")
                                    .arg(box.left() * 1e6, 0, 'g', 6).arg(box.right() * 1e6, 0, 'g', 6)
                                    .arg(box.top() * 1e6, 0, 'g', 6).arg(box.bottom() * 1e6, 0, 'g', 6)
                                    .arg(conductor.layer)
                                    .arg(at.left() * 1e6, 0, 'g', 6).arg(at.right() * 1e6, 0, 'g', 6)
                                    .arg(at.top() * 1e6, 0, 'g', 6).arg(at.bottom() * 1e6, 0, 'g', 6);
                return out;
            }
        }
    }
    // A plane of symmetry: every charge's, run's, via's and port path's
    // mirror in it, signed by the wall (FullWaveControl::Symmetry).
    if (control != nullptr && control->symmetry.axis != 0) {
        const FullWaveControl::Symmetry& sym = control->symmetry;
        const QString wall = sym.magnetic ? tr("magnetic") : tr("electric");
        const QString plane = sym.axis == 1 ? tr("x = %1 µm").arg(sym.at * 1e6, 0, 'g', 6) : tr("y = %1 µm").arg(sym.at * 1e6, 0, 'g', 6);
        if (sym.axis != 1 && sym.axis != 2) {
            out.problems << tr("A plane of symmetry is across x or across y.");
            return out;
        }
        if (c.boxed) {
            out.problems << tr("A plane of symmetry in a box is not solved yet: the box's walls would have its images too.");
            return out;
        }
        if (control->extents.size() > 0) {
            out.problems << tr("A layer that ends at its shapes is not solved with a plane of symmetry: its correction has no "
                               "mirror.");
            return out;
        }
        c.symAxis = sym.axis;
        c.symAt = sym.at;
        c.symCharge = sym.magnetic ? 1 : -1;
        // Every cell on the plane's far side; on an electric wall none
        // touching it.
        const auto offPlane = [&sym](const QPointF& p) { return sym.axis == 1 ? p.x() - sym.at : p.y() - sym.at; };
        QRectF all;
        for (const MeshCell& cell : given.cells) all = all.isNull() ? cell.outline.boundingRect() : all.united(cell.outline.boundingRect());
        const double hair = 1e-6 * std::max(all.width(), all.height()) * micrometre;
        for (const MeshCell& cell : given.cells) {
            const QRectF r = cell.outline.boundingRect();
            const double nearest = std::min(offPlane(r.topLeft() * micrometre), offPlane(r.bottomRight() * micrometre));
            if (nearest < -hair) {
                out.problems << tr("A conductor reaches across the plane of symmetry at %1: the half on its far side is what is "
                                   "solved (the layout's Processing keeps it).")
                                    .arg(plane);
                return out;
            }
            if (!sym.magnetic && nearest <= hair) {
                out.problems << tr("A conductor touches the electric wall at %1: its current would run into it, which is not "
                                   "solved yet - keep it off the plane.")
                                    .arg(plane);
                return out;
            }
        }
        // The ports: a magnetic wall's all on the plane - edge ports, each
        // the half of its face - or all off it; an electric wall's all off it.
        int on = 0;
        for (const FullWavePort& port : std::as_const(ports))
            if (std::abs(offPlane(port.at * micrometre)) <= hair) ++on;
        if (on > 0 && !sym.magnetic) {
            out.problems << tr("A port on an electric wall is shorted by it: an electric wall's ports are off the plane, each "
                               "driven against its mirror.");
            return out;
        }
        if (on > 0 && on < ports.size()) {
            out.problems << tr("Some of the ports are on the plane of symmetry and some off it: they are all on it - the whole's "
                               "ports cut in half - or all off it, each driven with its mirror.");
            return out;
        }
        for (FullWavePort& port : ports) {
            if (on == 0) break;
            const QString who = port.name.isEmpty() ? tr("a port") : port.name;
            if (port.internal || port.coplanar || port.terminals || port.deembeds() || !port.pair.isEmpty()) {
                out.problems << tr("%1 is on the plane of symmetry: a port there is an edge port of its own, not internal, coplanar, "
                                   "returning through terminals, paired or de-embedded, not yet.")
                                    .arg(who);
                return out;
            }
            // (A hair into the half: its face the edge across the plane.)
            const double in = hair / 10 / micrometre;
            port.at += sym.axis == 1 ? QPointF(in, 0) : QPointF(0, in);
        }
        c.halved = on > 0;
        out.lines << (c.halved ? tr("A %1 wall of symmetry at %2: the half beyond it solved with every charge's and current's mirror "
                                    "in it; the ports on it, each the half of its face, the whole's Z half the half's.")
                                     .arg(wall, plane)
                      : sym.magnetic
                          ? tr("A magnetic wall of symmetry at %1: the half beyond it solved with every charge's and current's mirror "
                               "in it; each port driven with its mirror in phase - the network is of each port and its mirror "
                               "together, the even mode.")
                                .arg(plane)
                          : tr("An electric wall of symmetry at %1: the half beyond it solved with every charge's and current's mirror "
                               "in it, opposite; each port driven against its mirror - the network is the odd mode's.")
                                .arg(plane));
        out.lines << tr("Solved dense: a mirror's kernel is no function of the distance between the two cells alone.");
    }
    // A layer that ends (rf25_dielectric.h): the background's Green's
    // functions - each such layer as it is beyond its shapes - and its
    // shapes' static polarization added to P, below.
    DielectricPlan dielectricPlanned;
    if (control != nullptr && !control->extents.isEmpty()) {
        QRectF around;
        for (const MeshCell& cell : mesh.cells) {
            const QRectF b = cell.outline.boundingRect();
            around = around.united(QRectF(b.left() * micrometre, b.top() * micrometre, b.width() * micrometre, b.height() * micrometre));
        }
        dielectricPlanned = dielectricPlan(c.layers, control->extents, around);
        if (!dielectricPlanned.ok()) {
            out.problems << dielectricPlanned.problems;
            return out;
        }
        c.layers = dielectricPlanned.background;
    }
    // A port that names its returns names some, each once and not itself;
    // and it is an ideal source between its cells and theirs, which strips
    // of a feed - a line between two edge ports, returning as it does - do
    // not have, so its own discontinuity is not taken out by them.
    for (int p = 0; p < ports.size(); ++p) {
        const FullWavePort& port = ports.at(p);
        if (!port.terminals) continue;
        const QString& name = out.portNames.at(p);
        if (port.returns.isEmpty()) {
            out.problems << tr("%1 returns through the terminals and ports it names, and names none: name them (its "
                               "'returns' - a Terminal under Ports, or another port), or return it another way.")
                                .arg(name);
            return out;
        }
        QStringList named;
        for (const PortTerminal& t : port.returns) {
            if (t.port && t.name == name) {
                out.problems << tr("%1 returns through itself: a port's return is a terminal or another port.").arg(name);
                return out;
            }
            if (named.contains(t.name)) {
                out.problems << tr("%1 names %2 twice among its returns.").arg(name, t.name);
                return out;
            }
            named << t.name;
            if (t.port && !out.portNames.contains(t.name)) {
                out.problems << tr("%1 returns through %2, and there is no port called that.").arg(name, t.name);
                return out;
            }
        }
        // (Its discontinuity taken out by strips of its feed: where each
        //  return is a face, the strips have it along them and at both ends,
        //  as gds2palace's port walls do - P45 of the open items of 8
        //  November, where the double delay was done by hand. Another
        //  port, or the cells under a rectangle, a strip cannot have.)
        bool faces = !port.internal;
        for (const PortTerminal& t : port.returns) faces = faces && !t.port && t.face;
        if (port.wantsStrips() && !faces) {
            out.problems << tr("%1 returns through the terminals it names (%2), and its own discontinuity cannot be taken "
                               "out by strips of a feed: they are a line between two edge ports, each returning through "
                               "faces at its own, and %1 is not that. Return it through faces on its own face's line (each "
                               "terminal Of a face), or take 'calibrate' off - its reference plane is then at its cells - "
                               "and de-embed after the solve (L-2L, TRL or open-short), or move its plane along a given "
                               "line ('along' line).")
                                .arg(name, named.join(QStringLiteral(", ")));
            return out;
        }
    }
    // With no ground plane a port's current has nowhere to come back but
    // the layout's own conductors: a coplanar edge port's, through those
    // beside it, an internal port's from the metal under it, and one that
    // names its returns through those - what goes in comes out, and the
    // potential's nought, at infinity, drops out. One that returns through
    // the ground would be driven against infinity.
    if (!c.layers.groundBelow) {
        QStringList grounded;
        for (int p = 0; p < ports.size(); ++p) {
            const FullWavePort& port = ports.at(p);
            const bool fromGround = port.from.isEmpty() || port.from.compare(QLatin1String("ground"), Qt::CaseInsensitive) == 0;
            if (port.terminals) continue;
            if (port.internal ? fromGround : !port.coplanar) grounded << out.portNames.at(p);
        }
        if (!grounded.isEmpty()) {
            out.problems << tr("There is no ground plane under the stack, and %1 return(s) through the ground: with none, a "
                               "port's current comes back through the conductors of the layout. Make an edge port return "
                               "through the conductors beside it (its 'return'), drive an internal port from a metal (its "
                               "'from'), name a port's returns (terminals, or other ports), or put a ground under the "
                               "stack.")
                                .arg(grounded.join(QStringLiteral(", ")));
            return out;
        }
    }
    // The pairs, each its plus port's and its minus's: a pair named from
    // both ends is one, the first its plus.
    for (int p = 0; p < ports.size(); ++p) {
        const QString partner = ports.at(p).pair.trimmed();
        if (partner.isEmpty()) continue;
        const QString& name = out.portNames.at(p);
        const int q = out.portNames.indexOf(partner);
        if (q < 0 || q == p) {
            out.problems << (q < 0 ? tr("%1 is paired with %2, and the layout has no port called that.").arg(name, partner)
                                   : tr("%1 is paired with itself: a pair is two ports.").arg(name));
            return out;
        }
        if (ports.at(q).pair.trimmed() == name && q < p) continue;
        for (const QPair<int, int>& known : std::as_const(out.pairs))
            if (known.first == p || known.second == p || known.first == q || known.second == q) {
                const int twice = known.first == p || known.second == p ? p : q;
                out.problems << tr("%1 is in two pairs: a port is the plus or the minus of one pair at most.")
                                    .arg(out.portNames.at(twice));
                return out;
            }
        if (std::abs(out.references.at(p) - out.references.at(q)) > 1e-9 * out.references.at(p)) {
            out.problems << tr("%1 and %2 are a pair, and their reference impedances differ (%3 and %4 Ohm): a pair's "
                               "differential and common modes are of one reference for both.")
                                .arg(name, partner)
                                .arg(out.references.at(p), 0, 'g', 4)
                                .arg(out.references.at(q), 0, 'g', 4);
            return out;
        }
        out.pairs << qMakePair(p, q);
    }
    // The nets, and which are returns (returnNetsOf()): a coplanar port
    // returns through the nearest return either side, past a conductor
    // that is none - a GSSG's other signal.
    const QList<Net> nets = netsOf(mesh, conductors, stack);
    QString returnsSaid;
    const QList<bool> isReturn = returnNetsOf(nets, mesh, conductors, stack, returns, &returnsSaid);
    QHash<int, int> netOf;
    for (int n = 0; n < nets.size(); ++n)
        for (const int cell : nets.at(n).cells) netOf.insert(cell, n);

    // The runs in the plane, on the metals - runsOf joins a via's own cells
    // too, as a little sheet of their own, and makes its vias its own way -
    // and then the vias.
    QList<QPair<int, int>> ends;
    {
        QList<QPair<int, int>> all;
        const QList<Filament> runs = runsOf(mesh, conductors, stack, micrometre, &all, false);
        for (int i = 0; i < runs.size(); ++i) {
            if (runs.at(i).z0 != runs.at(i).z1) continue;
            const Layer* layer = stack.named(mesh.cells.at(all.at(i).first).layer);
            if (layer == nullptr || layer->kind != Layer::Metal) continue;
            c.runs << runs.at(i);
            ends << all.at(i);
        }
    }
    c.planar = int(c.runs.size());
    // A metal or impedance ground's loss, where asked for: on the current the
    // runs' mirror images carry (groundPairsOf()).
    if (c.layers.groundBelow)
        for (const Layer& l : stack.layers)
            if (l.kind == Layer::Ground && l.lossInFullWave && l.sigma > 0) {
                if (l.variant == QLatin1String("impedance")) c.groundImpedance = l.sigma;
                else c.groundSigma = l.sigma;
                groundPairsOf(c);
            }
    // Which of each run's two sides are its conductor's edge: a point just
    // beyond the side, at the run's middle, on no shape of its layer
    // (shapes that touch are one conductor there).
    c.freeSides = QList<int>(c.planar, 0);
    c.freeMask = QList<int>(c.planar, 0);
    QHash<QString, QList<int>> shapesOn;   // (each layer's shapes: a via farm's are hundreds of other layers')
    for (int s = 0; s < conductors.size(); ++s) shapesOn[conductors.at(s).layer] << s;
    // (A metal standing for a round wire, a bond wire's: its strip's edges
    //  set its current's profile, its inductance a round wire's as a strip 4a
    //  wide's is with its edges, but crowd no loss to them - a round wire has
    //  none.)
    QSet<QString> round;
    for (const Layer& l : stack.layers)
        if (l.roundWire) round.insert(l.name);
    c.roundRuns = QList<bool>(c.planar, false);
    for (int a = 0; a < c.planar; ++a) {
        const Filament& run = c.runs.at(a);
        const QString& layer = mesh.cells.at(ends.at(a).first).layer;
        c.roundRuns[a] = round.contains(layer);
        const QPointF d = run.to - run.from;
        const double length = std::hypot(d.x(), d.y());
        if (!(length > 0)) continue;
        const QPointF normal(-d.y() / length, d.x() / length), middle = (run.from + run.to) / (2 * micrometre);
        const double reach = 0.5005 * run.width / micrometre;
        for (const double side : {-1.0, 1.0}) {
            QPointF probe = middle + side * reach * normal;
            // (Beyond a magnetic wall of symmetry the conductor's mirror is
            //  there: the cut along the plane is no edge.)
            if (c.symAxis != 0 && c.symCharge > 0
                && (c.symAxis == 1 ? probe.x() * micrometre < c.symAt : probe.y() * micrometre < c.symAt))
                probe = c.symmetric(probe * micrometre) / micrometre;
            bool inside = false;
            for (const int s : shapesOn.value(layer)) {
                const Conductor& shape = conductors.at(s);
                if (shape.outline.boundingRect().contains(probe) && shape.outline.containsPoint(probe, Qt::OddEvenFill)) {
                    inside = true;
                    break;
                }
            }
            if (!inside) {
                ++c.freeSides[a];
                c.freeMask[a] |= side < 0 ? 1 : 2;
            }
        }
    }
    // Each run on a metal's top face and its own on the bottom: the runs
    // between the same two cells' copies.
    c.twin = QList<int>(c.planar, -1);
    if (thick.any) {
        QHash<QPair<int, int>, int> byEnds;
        for (int a = 0; a < c.planar; ++a) byEnds.insert(ends.at(a), a);
        for (int a = 0; a < c.planar; ++a) {
            const int x = ends.at(a).first, y = ends.at(a).second;
            if (!thick.topOf.contains(x) || !thick.topOf.contains(y)) continue;
            const int tx = thick.topOf.value(x), ty = thick.topOf.value(y);
            const int t = byEnds.value(qMakePair(tx, ty), byEnds.value(qMakePair(ty, tx), -1));
            if (t < 0) continue;
            c.twin[a] = t;
            c.twin[t] = a;
        }
    }
    // Each twin's breadth across its conductor at its middle - the stretch
    // of the line across the run that lies on its layer's shapes, joined
    // where they touch, out to a hundred thicknesses (breadthAcross()) -,
    // and how much of a wide slab's inside its faces hold there
    // (Context::inside).
    if (thick.any) {
        // (Beyond a magnetic wall the conductor's mirror is there, its
        //  current even across the plane: its breadth the whole's. Of the
        //  half's, a strip the plane halves along its length was half as
        //  broad, its share of a slab's inside the less, and a 100 by 50 µm
        //  line's series reactance 0.75 % under the whole's.)
        const QList<Conductor> mirrored = c.symAxis != 0 && c.symCharge > 0
                                              ? withMirrors(conductors, c.symAxis, c.symAt / micrometre) : conductors;
        QHash<QString, QList<int>> mirroredOn;
        for (int s = 0; s < mirrored.size(); ++s) mirroredOn[mirrored.at(s).layer] << s;
        c.inside = QList<double>(c.planar, 1.0);
        for (int a = 0; a < c.planar; ++a) {
            const Filament& run = c.runs.at(a);
            if (c.twin.at(a) < 0 || !(run.thickness > 0) || !(run.width > 0)) continue;
            const QPointF d = run.to - run.from;
            const double length = std::hypot(d.x(), d.y());
            if (!(length > 0)) continue;
            const QPointF normal(-d.y() / length, d.x() / length), middle = (run.from + run.to) / (2 * micrometre);
            QList<QPolygonF> outlines;
            for (const int s : mirroredOn.value(mesh.cells.at(ends.at(a).first).layer)) outlines << mirrored.at(s).outline;
            const double across = breadthAcross(outlines, middle, normal, 100 * run.thickness / micrometre);
            const double breadth = across > 0 ? across * micrometre : run.width;
            c.inside[a] = facesInside(std::max(breadth, run.width), run.thickness);
        }
    }
    QStringList viaWarned;
    const QList<ViaRun> viaRuns = viaRunsOf(mesh, stack, micrometre, &c.spans, &viaWarned);
    int grounded = 0;
    QSet<int> walled;   // (the runs that are a metal's walls, between its faces)
    for (const ViaRun& v : viaRuns) {
        if (!v.cells.isEmpty() && v.cells.first() >= thick.cells) walled << int(c.runs.size());
        Via via;
        via.run = int(c.runs.size());
        via.span = v.span;
        via.at = v.at;
        via.side = v.side;
        via.gmd = v.gmd;
        c.vias << via;
        for (const int cell : v.cells) out.viaCells.insert(cell, int(c.runs.size()));
        c.runs << v.run;
        ends << qMakePair(v.tail, v.head);
        if (v.tail < 0) ++grounded;
    }
    if (c.runs.isEmpty()) {
        out.problems << tr("No two cells of the mesh share an edge, so there is nowhere for a current to flow. Mesh the "
                           "layout more finely.");
        return out;
    }
    if (grounded > 0 && !c.layers.groundBelow) {
        out.problems << tr("%1 via(s) go down to the ground, and there is no ground plane under the stack: put one "
                           "under it, or take them out.")
                            .arg(grounded);
        return out;
    }

    // The cells the runs join - the charges - in the mesh's order. (Not the
    // ground a via stands on.)
    QList<int> cellOf;
    for (const QPair<int, int>& e : std::as_const(ends))
        for (const int cell : {e.first, e.second})
            if (cell >= 0) cellOf << cell;
    std::sort(cellOf.begin(), cellOf.end());
    cellOf.erase(std::unique(cellOf.begin(), cellOf.end()), cellOf.end());
    QHash<int, int> nodeOf;
    for (int i = 0; i < cellOf.size(); ++i) nodeOf.insert(cellOf.at(i), i);
    // (Those of them that are no metal's top face: where a port finds the
    //  metal it is on.)
    QList<int> bottoms;
    for (const int cell : std::as_const(cellOf))
        if (!thick.bottomOf.contains(cell)) bottoms << cell;
    const int nr = int(c.runs.size()), nc = int(cellOf.size()), np = int(ports.size());
    c.nr = nr;
    c.nc = nc;
    c.np = np;
    out.runs = nr;
    out.cells = nc;
    // Dense up to the limit, compressed past it - or as the control asks,
    // a dense matrix past the limit refused as it was before there was a
    // compressed one.
    const int most = control != nullptr && control->mostUnknowns > 0 ? control->mostUnknowns : denseLimit();
    const FullWaveControl::Matrix asked = control != nullptr ? control->matrix : FullWaveControl::Matrix::Automatic;
    // (A box dense: its kernel is no function of distance alone.)
    const bool compress = !c.boxed && c.symAxis == 0
                          && (asked == FullWaveControl::Matrix::Compressed || (asked == FullWaveControl::Matrix::Automatic && nr + nc > most));
    // (A box or a plane of symmetry is held dense alone: told to hold it
    //  compressed, which it cannot, and asked to, solved dense with no word -
    //  a bug hunt's second hour.)
    const bool denseAlone = c.boxed || c.symAxis != 0;
    const QString why = c.boxed ? tr("a box") : tr("a plane of symmetry");
    if (!compress && nr + nc > most) {
        out.tooLarge = true;
        out.problems << (denseAlone ? tr("%1 runs of current over %2 cells is more than a dense complex matrix is set up for (%3 in "
                                         "all), and a solve in %4 is held dense alone. Mesh the layout more coarsely: fewer "
                                         "cells across a conductor, or a larger largest cell - or raise the limit (RF 2.5D > "
                                         "Settings…).")
                                          .arg(nr).arg(nc).arg(most).arg(why)
                                    : tr("%1 runs of current over %2 cells is more than a dense complex matrix is set up for (%3 in "
                                         "all). Mesh the layout more coarsely: fewer cells across a conductor, or a larger largest "
                                         "cell - or hold it compressed (the study's Matrix), or raise the limit (RF 2.5D > "
                                         "Settings…).")
                                          .arg(nr).arg(nc).arg(most));
        return out;
    }
    if (denseAlone && asked == FullWaveControl::Matrix::Compressed)
        out.lines << tr("Asked to hold its matrix compressed, a solve in %1 is held dense: compressed it is not solved yet.").arg(why);
    if (nr + nc > kMostCompressed) {
        out.tooLarge = true;
        out.problems << tr("%1 runs of current over %2 cells is more than this build sets up even compressed (%3 in "
                           "all). Mesh the layout more coarsely: fewer cells across a conductor, or a larger largest "
                           "cell.")
                            .arg(nr).arg(nc).arg(kMostCompressed);
        return out;
    }
    c.tail.resize(nr);
    c.head.resize(nr);
    for (int a = 0; a < nr; ++a) {
        c.tail[a] = ends.at(a).first < 0 ? -1 : nodeOf.value(ends.at(a).first);
        c.head[a] = nodeOf.value(ends.at(a).second);
    }

    // The ports. An edge port drives the face of the conductor it is on,
    // its current shared out by the length of face each cell has; an
    // internal port the cell under it, against the ground or against the
    // cell under it on another metal. A coplanar edge port drives its face
    // against the faces beside it on its edge's line, a column against
    // each: the columns of a port are side by side - one voltage, their
    // currents added - so the grounds are tied, as a probe's are, and the
    // solve shares the current out between them.
    QList<Eigen::VectorXcd> columns;
    QList<int> portOf;
    // Each column's cells its port's own path goes to (PortPath), by node,
    // with the path's cross-section there - its side and its mean distance
    // with itself, m: an edge port's a ribbon as wide as the face the cell
    // has, an internal port's the cell's box. None for a coplanar port.
    QList<QMap<int, QPair<double, double>>> shapes;
    const auto ribbon = [micrometre](double width) {
        return qMakePair(width * micrometre, 0.2235 * width * micrometre);   // (Rosa's, a thin strip's)
    };
    const auto boxOf = [micrometre](const QRectF& box) {
        return qMakePair(std::sqrt(box.width() * box.height()) * micrometre, 0.2235 * (box.width() + box.height()) * micrometre);
    };
    bool coplanar = false;
    // A share of a port's current into a cell: of a metal solved as its two
    // faces, half into the cell and half into its top's - the face of a
    // thick conductor is its whole wall, both faces' edges together.
    const auto put = [&thick, &nodeOf](Eigen::VectorXcd& column, int cell, double share) {
        const int top = thick.topOf.value(cell, -1);
        if (top >= 0 && nodeOf.contains(top)) {
            column(nodeOf.value(cell)) += share / 2;
            column(nodeOf.value(top)) += share / 2;
        } else
            column(nodeOf.value(cell)) += share;
    };
    // Of a metal solved as its two faces, the face toward \a other: its
    // bottom toward the ground or a metal below, its top toward one above.
    const auto toward = [&stack, &thick](const QString& metal, const QString& other) {
        const Layer* layer = stack.named(metal);
        if (!thick.any || layer == nullptr || layer->face != Layer::Bottom || other.isEmpty() || stack.isGround(other))
            return metal;
        if (!(metalHeight(stack, other) > metalHeight(stack, layer->name))) return metal;
        for (const Layer& top : stack.layers)
            if (top.face == Layer::Top && top.faceOf == layer->name) return top.name;
        return metal;
    };
    // A layer as the model calls it: a metal's top face that metal's.
    const auto shown = [&stack](const QString& layer) {
        const Layer* l = stack.named(layer);
        return l != nullptr && l->face == Layer::Top ? tr("%1's top face").arg(l->faceOf) : layer;
    };
    out.faceWidths = QList<double>(np, 0.0);
    out.faceMetals = QStringList(QList<QString>(np));
    out.feeds = QList<Feed>(np);
    out.portsSaid = QStringList(QList<QString>(np));
    out.returnsOf = QList<QList<PortReturn>>(np);
    out.namesReturns = QList<bool>(np, false);
    // (Each port's own net, and the nets a coplanar port returns through.)
    QList<int> ownNet(np, -1);
    QList<QList<int>> returnNets(np);
    // Each port's own cells, what it drives, as its column's shares before
    // anything it returns through - a port that names its returns, or
    // another's that names it, is a column against each return -, and how
    // many there are; which of its port's returns each column is (-1: its
    // one); and the ports that name their returns, with what each drives,
    // their columns made once every port's own cells are known.
    QList<Eigen::VectorXcd> owns(np);
    QList<int> ownCount(np, 0);
    QList<int> columnReturn;
    QList<int> naming;
    QStringList drivesSaid = QStringList(QList<QString>(np));
    QList<Conductor> placedOn;   // (the conductors whole, on a plane of symmetry: portPlace())
    for (int p = 0; p < np; ++p) {
        const FullWavePort& port = ports.at(p);
        const QString& name = out.portNames.at(p);
        const QString x = QString::number(port.at.x(), 'g', 4), y = QString::number(port.at.y(), 'g', 4);
        out.namesReturns[p] = port.terminals;
        if (port.internal) {
            // The one cell under its point - or, over a rectangle about it,
            // every cell of its metal the rectangle overlaps, each by its
            // share of the area, as an edge port's face is shared out.
            const bool over = port.size.width() > 0 && port.size.height() > 0;
            const QRectF rect(port.at - QPointF(port.size.width(), port.size.height()) / 2, port.size);
            const auto driven = [&](const QString& metal) {
                const QList<int>& among = metal.isEmpty() ? bottoms : cellOf;
                if (over) return cellsUnder(mesh, among, rect, metal);
                const int one = cellUnder(mesh, among, port.at, metal);
                return one < 0 ? QList<std::pair<int, double>>() : QList<std::pair<int, double>>{std::make_pair(one, 1.0)};
            };
            const QString w = QString::number(port.size.width(), 'g', 4), h = QString::number(port.size.height(), 'g', 4);
            // (One that names its returns stands on both faces of a metal
            //  solved as its two, half each, as its returns do and an edge
            //  port's face does: with no path up from below, nothing makes
            //  one face its own.)
            const QList<std::pair<int, double>> to = driven(port.terminals ? port.metal : toward(port.metal, port.from));
            if (to.isEmpty()) {
                out.problems << (over ? tr("%1's %2 by %3 µm rectangle at %4, %5 µm is over no cell of %6 that a current "
                                           "reaches.")
                                            .arg(name, w, h, x, y, port.metal.isEmpty() ? tr("any metal") : port.metal)
                                      : tr("%1, at %2, %3 µm, is over no cell of %4 that a current reaches.")
                                            .arg(name, x, y, port.metal.isEmpty() ? tr("any metal") : port.metal));
                return out;
            }
            Eigen::VectorXcd column = Eigen::VectorXcd::Zero(nc);
            QMap<int, QPair<double, double>> shape;
            for (const auto& [cell, share] : to) {
                if (port.terminals) put(column, cell, share);
                else column(nodeOf.value(cell)) += share;
                const QRectF box = mesh.cells.at(cell).outline.boundingRect();
                shape.insert(nodeOf.value(cell), boxOf(over ? box.intersected(rect) : box));
            }
            ownNet[p] = netOf.value(to.first().first, -1);
            // (A rectangle over pieces of metal not otherwise joined ties them
            //  through the port, which no probe does: two strips under one
            //  read |S21| 0.17 where one alone reads 0.9998, nothing said - a bug hunt's second hour. An element across a gap
            //  is a port over one pad returning through a terminal over the
            //  other.)
            {
                QSet<int> under;
                for (const auto& [cell, share] : to) under.insert(netOf.value(cell, -1));
                if (under.size() > 1) {
                    const QString w2 = tr("%1's %2 by %3 µm rectangle at %4, %5 µm lies over %6 pieces of metal not otherwise "
                                          "joined, and ties them together through the port, as no probe would. To drive "
                                          "across a gap, put the port over one side and a terminal over the other among its "
                                          "returns.")
                                           .arg(name, w, h, x, y)
                                           .arg(under.size());
                    out.warnings << w2;
                    out.lines << w2;
                }
            }
            owns[p] = column;
            ownCount[p] = int(to.size());
            if (port.terminals) {
                const QString layer = shown(mesh.cells.at(to.first().first).layer);
                int topped = 0;
                for (const auto& [cell, share] : to)
                    if (thick.topOf.contains(cell) && nodeOf.contains(thick.topOf.value(cell))) ++topped;
                const QString halved = topped > 0 ? tr(", half of it on %1's top face").arg(layer) : QString();
                drivesSaid[p] = over ? tr("%1 drives the %2 cell(s) of %3 under its %4 by %5 µm rectangle at %6, %7 µm%8")
                                           .arg(name).arg(to.size()).arg(layer, w, h, x, y, halved)
                                     : tr("%1 drives the cell at %2, %3 µm on %4%5").arg(name, x, y, layer, halved);
                naming << p;
                continue;
            }
            QString against = tr("the ground");
            if (!port.from.isEmpty() && port.from.compare(QLatin1String("ground"), Qt::CaseInsensitive) != 0) {
                const QList<std::pair<int, double>> from = driven(toward(port.from, port.metal));
                bool same = from.isEmpty();
                for (const auto& [cell, share] : from)
                    for (const auto& [other, part] : to) same = same || cell == other;
                if (same) {
                    out.problems << tr("%1, at %2, %3 µm, has no cell of %4 under it to drive against.")
                                        .arg(name, x, y, port.from);
                    return out;
                }
                for (const auto& [cell, share] : from) column(nodeOf.value(cell)) -= share;
                const QString on = shown(mesh.cells.at(from.first().first).layer);
                against = over ? tr("the %1 cell(s) of %2 under it").arg(from.size()).arg(on) : tr("the cell under it on %1").arg(on);
            }
            columns << column;
            portOf << p;
            shapes << shape;
            columnReturn << -1;
            out.portsSaid[p] = over ? tr("%1 drives the %2 cell(s) of %3 under its %4 by %5 µm rectangle at %6, %7 µm "
                                         "against %8.")
                                          .arg(name).arg(to.size()).arg(shown(mesh.cells.at(to.first().first).layer), w, h, x, y, against)
                                    : tr("%1 drives the cell at %2, %3 µm on %4 against %5.")
                                          .arg(name, x, y, shown(mesh.cells.at(to.first().first).layer), against);
            continue;
        }
        // On the edge it drives (portPlace()): taken however far it was, a
        // port off the layout drove the nearest face, one inside a strip its
        // long side, one facing the wrong way its far end, nothing said.
        // (Of the conductors whole on a plane of symmetry, their mirrors
        //  joined to them: the cut along the plane is no edge, and a port
        //  moved a hair off the plane is not on a corner of it.)
        if (c.symAxis != 0 && placedOn.isEmpty()) placedOn = withMirrors(conductors, c.symAxis, c.symAt / micrometre);
        switch (portPlace(c.symAxis != 0 ? placedOn : conductors, port.at, port.metal, port.facing)) {
        case PortPlace::OnEdge:
            break;
        case PortPlace::OnCorner:
            out.problems << tr("%1, at %2, %3 µm, is on a corner, where two edges meet: give it the facing of the edge it is to "
                               "drive, or move it along that edge.")
                                .arg(name, x, y);
            return out;
        case PortPlace::FacingOtherWay:
            out.problems << tr("%1, at %2, %3 µm, is on an edge that does not face the way it is said to: turn its facing "
                               "to the edge's outward side, or leave it to the edge.")
                                .arg(name, x, y);
            return out;
        case PortPlace::Inside: {
            const QString w = tr("%1, at %2, %3 µm, is inside a conductor, on none of its edges: it drives the one cell "
                                 "nearest. An edge port belongs on the edge it drives; an internal port drives the cells "
                                 "under a point.")
                                  .arg(name, x, y);
            out.warnings << w;
            out.lines << w;
            break;
        }
        case PortPlace::Outside:
            out.problems << tr("%1, at %2, %3 µm, is on no conductor%4: put it on the edge it is to drive.")
                                .arg(name, x, y, port.metal.isEmpty() ? QString() : tr(" of %1").arg(shown(port.metal)));
            return out;
        }
        QPointF on, along;
        const QList<int> face = faceCells(mesh, conductors, cellOf, port.at, &on, &along, port.metal, port.facing);
        if (face.isEmpty()) {
            out.problems << tr("%1, at %2, %3 µm, is on no cell a current reaches.").arg(name, x, y);
            return out;
        }
        ownNet[p] = netOf.value(face.first(), -1);
        double faceWidth = 0;
        const QList<double> shares = sharesOf(mesh, face, on, along, &faceWidth);
        Eigen::VectorXcd own = Eigen::VectorXcd::Zero(nc);
        for (int k = 0; k < face.size(); ++k) put(own, face.at(k), shares.at(k));
        // (What of it goes to a top face, said as driven.)
        int topped = 0;
        for (const int cell : std::as_const(face)) {
            const int top = thick.topOf.value(cell, -1);
            if (top >= 0 && nodeOf.contains(top) && own(nodeOf.value(top)) != Complex(0, 0)) ++topped;
        }
        const QString halved = topped > 0 ? tr(", half of it on %1's top face").arg(mesh.cells.at(face.first()).layer) : QString();
        if (along != QPointF(0, 0)) {
            out.faceWidths[p] = faceWidth;
            out.faceMetals[p] = mesh.cells.at(face.first()).layer;
            out.feeds[p].metal = out.faceMetals.at(p);
            out.feeds[p].width = faceWidth;
            // How far its feed runs straight in from the face: where its own
            // field reaches past that, what strips of the feed find of the
            // port is not what it has here (rf25_deembed.h). A coplanar
            // feed's grounds are no widening: what is looked past its ends
            // is short of their gaps.
            const Beside span = besideFaces(mesh, cellOf, face, on, along);
            const QPointF unit = along / std::hypot(along.x(), along.y()), normal(-unit.y(), unit.x());
            double beyond = faceWidth / 24;
            for (std::size_t k = 0; k < 2; ++k)
                if (!span.cells[k].isEmpty() && span.near[k] > 0) beyond = std::min(beyond, 0.5 * span.near[k]);
            const double height = std::max(0.0, metalHeight(givenStack, out.faceMetals.at(p))) / micrometre;
            out.feeds[p].length = feedLength(givenConductors, out.faceMetals.at(p), on, unit, span.inside < 0 ? -normal : normal,
                                             span.lo, faceWidth, 2 * (8 * height + 2 * faceWidth), beyond);
            out.feeds[p].frameOn = on;
            out.feeds[p].frameUnit = unit;
            out.feeds[p].frameInward = span.inside < 0 ? -normal : normal;
            out.feeds[p].frameLo = span.lo;
            // The layers that end, as the feed stands on them: its strips
            // laid on the same (they were laid on each such layer as it is
            // inside its shapes, everywhere - a GSG line on a mesa calibrated
            // as on the mesa's material from side to side).
            if (control != nullptr && !control->extents.isEmpty()) {
                const double length = out.feeds.at(p).length;
                const double depth = 0.25 * std::min(faceWidth, length > 0 ? length : faceWidth);
                const QPointF inward = span.inside < 0 ? -normal : normal;
                const double reach = 10 * std::max(faceWidth, 2 * (8 * height + 2 * faceWidth));
                out.feeds[p].extents = feedExtents(control->extents, on, unit, inward, span.lo, faceWidth, depth, reach);
                // (The section again at the far end of its straight run: where
                //  a layer ends along the feed itself - a mesa that begins part
                //  way along it, or whose edge crosses it at an angle -, the
                //  strips, a line, have the one near the face: said.)
                const double deep = 0.9 * length;
                if (port.wantsStrips() && deep > 2 * depth) {
                    const QList<Extent> far = feedExtents(control->extents, on, unit, inward, span.lo, faceWidth, deep, reach);
                    const auto spans = [](const Extent& e) {
                        QList<std::pair<double, double>> out;
                        for (const QPolygonF& s : e.shapes) out << std::make_pair(s.boundingRect().top(), s.boundingRect().bottom());
                        return out;
                    };
                    bool same = far.size() == out.feeds.at(p).extents.size();
                    const double tol = 1e-3 * faceWidth;
                    for (int k = 0; same && k < far.size(); ++k) {
                        const auto a = spans(out.feeds.at(p).extents.at(k)), b = spans(far.at(k));
                        same = a.size() == b.size();
                        for (int m = 0; same && m < a.size(); ++m)
                            same = std::abs(a.at(m).first - b.at(m).first) <= tol && std::abs(a.at(m).second - b.at(m).second) <= tol;
                    }
                    if (!same) {
                        const QString w = tr("%1's feed stands on a layer that ends otherwise %2 µm in than at its face: its "
                                             "calibration strips, a line, stand on it as it is %3 µm in all along - what of the "
                                             "change is between is the structure's, not taken out.")
                                              .arg(name).arg(deep, 0, 'g', 4).arg(depth, 0, 'g', 3);
                        out.warnings << w;
                        out.lines << w;
                    }
                }
            }
        }
        owns[p] = own;
        ownCount[p] = int(face.size());
        if (port.terminals) {
            drivesSaid[p] = along != QPointF(0, 0)
                                ? tr("%1 drives the %2 cell(s) along the face at %3, %4 µm%5").arg(name).arg(face.size()).arg(x, y, halved)
                                : tr("%1 is on no face, and drives the one cell nearest %2, %3 µm").arg(name, x, y);
            naming << p;
            continue;
        }
        if (!port.coplanar) {
            // Its path up from the ground to each cell of the face (and of
            // its top, where the metal is solved as its two faces).
            QMap<int, QPair<double, double>> shape;
            for (int k = 0; k < face.size(); ++k) {
                const auto across = along != QPointF(0, 0) && shares.at(k) * faceWidth > 0
                                        ? ribbon(shares.at(k) * faceWidth)
                                        : boxOf(mesh.cells.at(face.at(k)).outline.boundingRect());
                shape.insert(nodeOf.value(face.at(k)), across);
                const int top = thick.topOf.value(face.at(k), -1);
                if (top >= 0 && nodeOf.contains(top)) shape.insert(nodeOf.value(top), across);
            }
            columns << own;
            portOf << p;
            shapes << shape;
            columnReturn << -1;
            if (along != QPointF(0, 0))
                out.portsSaid[p] = tr("%1 drives the %2 cell(s) along the face at %3, %4 µm%5.").arg(name).arg(face.size()).arg(x, y, halved);
            else
                out.portsSaid[p] = tr("%1 is on no face, and drives the one cell nearest %2, %3 µm.").arg(name, x, y);
            continue;
        }
        if (along == QPointF(0, 0)) {
            out.problems << tr("%1 returns through the conductors beside it, so it wants the face of one to stand on, and "
                               "%2, %3 µm is on none.")
                                .arg(name, x, y);
            return out;
        }
        coplanar = true;
        const Beside beside = besideFaces(mesh, cellOf, face, on, along, &netOf, &isReturn);
        QList<int> counts;
        QStringList through;
        int groundsTopped = 0;
        for (const QList<int>& ground : beside.cells) {
            if (ground.isEmpty()) continue;
            through << nets.value(netOf.value(ground.first(), -1)).name;
            returnNets[p] << netOf.value(ground.first(), -1);
            const QList<double> back = sharesOf(mesh, ground, on, along);
            Eigen::VectorXcd column = own;
            for (int k = 0; k < ground.size(); ++k) put(column, ground.at(k), -back.at(k));
            for (const int cell : ground) {
                const int top = thick.topOf.value(cell, -1);
                if (top >= 0 && nodeOf.contains(top) && column(nodeOf.value(top)) != own(nodeOf.value(top))) ++groundsTopped;
            }
            columns << column;
            portOf << p;
            shapes << QMap<int, QPair<double, double>>();
            out.returnsOf[p] << PortReturn{through.last(), int(ground.size()), Complex(0, 0)};
            columnReturn << int(out.returnsOf.at(p).size()) - 1;
            counts << int(ground.size());
        }
        // Its feed as calibration strips of it are made: the grounds' gaps
        // and widths on the edge's line, and the vias down them as far as
        // the strips reach.
        Feed& feed = out.feeds[p];
        feed.coplanar = true;
        for (std::size_t k = 0; k < 2; ++k)
            if (!beside.cells[k].isEmpty()) {
                feed.gap[k] = beside.near[k];
                feed.ground[k] = beside.far[k] - beside.near[k];
            }
        {
            const QPointF unit = along / std::hypot(along.x(), along.y()), normal(-unit.y(), unit.x());
            const double height = std::max(0.0, metalHeight(stack, feed.metal)) / micrometre;
            feedVias(&feed, givenConductors, givenStack, on, unit, beside.inside < 0 ? -normal : normal, beside.lo,
                     2 * (8 * height + 2 * faceWidth));
        }
        // Past a conductor that is no return, its feed is coupled lines,
        // which strips of the one signal and its returns are not.
        QStringList passed;
        for (const int cell : beside.passed)
            if (cell >= 0) passed << nets.value(netOf.value(cell, -1)).name;
        if (!passed.isEmpty())
            feed.unlike = tr("%1, no return, lies between it and its return, so its feed is coupled lines, which strips "
                             "of it and its returns alone are not")
                              .arg(passed.join(QStringLiteral(" and ")));
        if (counts.isEmpty()) {
            out.problems << tr("%1 returns through the conductors beside it, and on the line of its edge at %2, %3 µm there "
                               "is none either side of it: draw its grounds to the same edge, or return it through the "
                               "ground plane.")
                                .arg(name, x, y);
            return out;
        }
        const QString past = passed.isEmpty() ? QString() : tr(", past %1, which is no return").arg(passed.join(QStringLiteral(" and ")));
        const QString ownSaid =
            !beside.own[0] && !beside.own[1]
                ? QString()
                : tr("%1 returns through conductors beside it that are joined to its own further on - a shorted "
                            "line, a stub to its ground, a loop -: the source is between two places of one conductor, "
                            "apart on its edge, which is no short at a frequency.")
                      .arg(name);
        out.portsSaid[p] = (counts.size() == 2
                              ? tr("%1 drives the %2 cell(s) along the face at %3, %4 µm%5 against the conductors beside it, "
                                   "%6 and %7 cell(s) of the faces of %8 either side%9%10: the two tied, as a probe's grounds "
                                   "are.")
                                    .arg(name).arg(face.size()).arg(x, y, halved).arg(counts.at(0)).arg(counts.at(1))
                                    .arg(through.join(QStringLiteral(" and ")), past)
                                    .arg(groundsTopped > 0 ? tr(", half on their top faces") : QString())
                              : tr("%1 drives the %2 cell(s) along the face at %3, %4 µm%5 against the conductor beside it "
                                   "on one side, %6 cell(s) of the face of %7%8%9: a ground-signal probe's.")
                                    .arg(name).arg(face.size()).arg(x, y, halved).arg(counts.at(0))
                                    .arg(through.value(0), past)
                                    .arg(groundsTopped > 0 ? tr(", half on its top face") : QString()));
        if (!ownSaid.isEmpty()) out.portsSaid[p] += QLatin1Char(' ') + ownSaid;
    }
    // The ports that name their returns, now that every port's own cells
    // are known: a column against each return - the port's cells less the
    // return's, each by its share, both faces of a metal solved as its two
    // half each (put()) - side by side as a coplanar port's, so that the
    // returns are tied and the solve shares the current out between them.
    // No path of its own up to them: an ideal source between the cells.
    // A return is not on the port's own conductor, which would short the
    // source; and a terminal is not on another port's cells, unless that
    // port is named - then its cells are the return, and the two ports are
    // in series, each a port of its own.
    const auto netsUnder = [&](const Eigen::VectorXcd& column) {
        QSet<int> under;
        for (int i = 0; i < nc; ++i)
            if (column(i) != Complex(0, 0)) under.insert(netOf.value(thick.bottomOf.value(cellOf.at(i), cellOf.at(i)), -1));
        under.remove(-1);
        return under;
    };
    for (const int p : std::as_const(naming)) {
        const FullWavePort& port = ports.at(p);
        const QString& name = out.portNames.at(p);
        const QSet<int> own = netsUnder(owns.at(p));
        QStringList against, sameConductor;   // (the returns on its own conductor)
        QList<QPair<Eigen::VectorXcd, QString>> returnCells;   // (each return's cells so far)
        QString sameNet;
        for (const PortTerminal& t : port.returns) {
            const QString x = QString::number(t.at.x(), 'g', 4), y = QString::number(t.at.y(), 'g', 4);
            const QString metal = t.metal.isEmpty() ? tr("any metal") : t.metal;
            Eigen::VectorXcd back = Eigen::VectorXcd::Zero(nc);
            int cells = 0;
            if (t.port) {
                const int q = int(out.portNames.indexOf(t.name));
                back = owns.at(q);
                cells = ownCount.at(q);
            } else if (t.face) {
                QPointF on, along;
                const QList<int> face = faceCells(mesh, conductors, cellOf, t.at, &on, &along, t.metal, t.facing);
                // (On a face: within a hair of the edge found - the nearest
                //  facing that way, however far, was taken, and a terminal on
                //  no conductor solved.)
                QLineF edge;
                bool onEdge = false;
                if (faceEdge(conductors, t.at, t.metal, t.facing, &edge)) {
                    const QPointF d = edge.p2() - edge.p1();
                    const double len2 = d.x() * d.x() + d.y() * d.y();
                    const double u = len2 > 0 ? std::clamp(((t.at - edge.p1()).x() * d.x() + (t.at - edge.p1()).y() * d.y()) / len2, 0.0, 1.0) : 0.0;
                    const QPointF nearest = edge.p1() + d * u;
                    onEdge = std::hypot(t.at.x() - nearest.x(), t.at.y() - nearest.y()) <= 1e-3 * std::max(1.0, std::sqrt(len2));
                }
                if (face.isEmpty() || along == QPointF(0, 0) || !onEdge) {
                    out.problems << tr("%1's return %2, at %3, %4 µm, is on no face of %5 that a current reaches.")
                                        .arg(name, t.name, x, y, metal);
                    return out;
                }
                const QList<double> shares = sharesOf(mesh, face, on, along);
                for (int k = 0; k < face.size(); ++k) put(back, face.at(k), shares.at(k));
                cells = int(face.size());
                // Its place in the port's frame, for strips of its feed: its
                // stretch across, where it is on the port's own face's line
                // and faces its way.
                Feed& feed = out.feeds[p];
                if (port.wantsStrips() && feed.frameUnit != QPointF(0, 0)) {
                    const QPointF u = feed.frameUnit, n = feed.frameInward;
                    const double off = (on.x() - feed.frameOn.x()) * n.x() + (on.y() - feed.frameOn.y()) * n.y();
                    const double turn = std::abs(u.x() * along.y() - u.y() * along.x()) / std::max(1e-30, std::hypot(along.x(), along.y()));
                    const bool facesOut = t.facing == QPointF(0, 0) || port.facing == QPointF(0, 0)
                                          || t.facing.x() * port.facing.x() + t.facing.y() * port.facing.y() > 0.99;
                    if (std::abs(off) > 1e-3 * std::max(1.0, feed.width) || turn > 1e-6 || !facesOut) {
                        feed.unlike = tr("its return %1 is no face on its own face's line, facing its way, which strips of "
                                         "it could have at both their ends")
                                          .arg(t.name);
                    } else {
                        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                        for (const int cell : face)
                            for (const QPointF& corner : mesh.cells.at(cell).outline) {
                                const double s = (corner.x() - feed.frameOn.x()) * u.x() + (corner.y() - feed.frameOn.y()) * u.y() - feed.frameLo;
                                lo = std::min(lo, s);
                                hi = std::max(hi, s);
                            }
                        feed.returns << Feed::Return{mesh.cells.at(face.first()).layer, lo, hi};
                    }
                }
            } else {
                const bool over = t.size.width() > 0 && t.size.height() > 0;
                const QList<int>& among = t.metal.isEmpty() ? bottoms : cellOf;
                // (The one cell under a point is the nearest's where the point
                //  is in none: a point off the metal is said, not taken so.)
                if (!over) {
                    bool onShape = false;
                    for (const Conductor& shape : conductors) {
                        const Layer* layer = stack.named(shape.layer);
                        if (layer == nullptr || layer->kind != Layer::Metal || layer->face == Layer::Top) continue;
                        if (!t.metal.isEmpty() && shape.layer != t.metal) continue;
                        const QRectF box = shape.outline.boundingRect();
                        const double reach = 1e-6 * std::max(1.0, std::max(box.width(), box.height()));
                        for (const QPointF nudge : {QPointF(0, 0), QPointF(reach, 0), QPointF(-reach, 0), QPointF(0, reach), QPointF(0, -reach)})
                            onShape = onShape || shape.outline.containsPoint(t.at + nudge, Qt::OddEvenFill);
                        if (onShape) break;
                    }
                    if (!onShape) {
                        out.problems << tr("%1's return %2, at %3, %4 µm, is on no shape of %5.").arg(name, t.name, x, y, metal);
                        return out;
                    }
                }
                QList<std::pair<int, double>> under;
                if (over) {
                    under = cellsUnder(mesh, among, QRectF(t.at - QPointF(t.size.width(), t.size.height()) / 2, t.size), t.metal);
                } else if (const int one = cellUnder(mesh, among, t.at, t.metal); one >= 0) {
                    under << std::make_pair(one, 1.0);
                }
                if (under.isEmpty()) {
                    out.problems << (over ? tr("%1's return %2, a %3 by %4 µm rectangle at %5, %6 µm, is over no cell of %7 "
                                               "that a current reaches.")
                                                .arg(name, t.name)
                                                .arg(t.size.width(), 0, 'g', 4)
                                                .arg(t.size.height(), 0, 'g', 4)
                                                .arg(x, y, metal)
                                          : tr("%1's return %2, at %3, %4 µm, is over no cell of %5 that a current reaches.")
                                                .arg(name, t.name, x, y, metal));
                    return out;
                }
                for (const auto& [cell, share] : under) put(back, cell, share);
                cells = int(under.size());
            }
            // On the port's own conductor: a source across it - a shorted
            // line, a stub to its ground, a loop - which is no short at a
            // frequency, so long as the two are apart: refused on the port's
            // own cells or a cell next to them, said otherwise. Every return
            // on the port's own conductor was refused before, "it would short
            // the source", and none of those could be modelled.
            {
                const Eigen::VectorXcd& mine = owns.at(p);
                bool onOwn = false, besideOwn = false;
                for (int i = 0; i < nc && !onOwn; ++i)
                    onOwn = back(i) != Complex(0, 0) && mine(i) != Complex(0, 0);
                if (!onOwn) {
                    const auto bottom = [&thick](int cell) { return thick.bottomOf.value(cell, cell); };
                    QSet<int> portCells, returnCells;
                    for (int i = 0; i < nc; ++i) {
                        if (mine(i) != Complex(0, 0)) portCells.insert(bottom(cellOf.at(i)));
                        if (back(i) != Complex(0, 0)) returnCells.insert(bottom(cellOf.at(i)));
                    }
                    for (int r = 0; r < ends.size(); ++r) {
                        const QPair<int, int>& e = ends.at(r);
                        if ((portCells.contains(bottom(e.first)) && returnCells.contains(bottom(e.second)))
                            || (portCells.contains(bottom(e.second)) && returnCells.contains(bottom(e.first)))) {
                            besideOwn = true;
                            break;
                        }
                    }
                }
                if (onOwn || besideOwn) {
                    out.problems << tr("%1's return %2 is %3 the cells %1 drives: it would short the source. Put it apart "
                                       "from them - further along its own conductor, or on a ground beside it.")
                                        .arg(name, t.name, onOwn ? tr("on") : tr("next to"));
                    return out;
                }
                for (const int net : netsUnder(back))
                    if (own.contains(net)) {
                        sameConductor << t.name;
                        sameNet = nets.value(net).name;
                        break;
                    }
            }
            if (!t.port)
                for (int q = 0; q < np; ++q) {
                    if (q == p || owns.at(q).size() != nc) continue;
                    for (int i = 0; i < nc; ++i)
                        if (back(i) != Complex(0, 0) && owns.at(q)(i) != Complex(0, 0)) {
                            out.problems << tr("%1's return %2 is on the cells %3 drives: name %3 itself among %1's "
                                               "returns, if %1 is to return through what %3 drives (the two in series), "
                                               "or put %2 elsewhere.")
                                                .arg(name, t.name, out.portNames.at(q));
                            return out;
                        }
                }
            // Two returns over the same cells: their shares of the current
            // there are counted twice - said (it solved without a word).
            {
                QStringList over;
                for (int i = 0; i < nc; ++i)
                    if (back(i) != Complex(0, 0))
                        for (int r = 0; r < returnCells.size(); ++r)
                            if (returnCells.at(r).first(i) != Complex(0, 0) && !over.contains(returnCells.at(r).second))
                                over << returnCells.at(r).second;
                if (!over.isEmpty())
                    out.warnings << tr("%1's return %2 is over cells that %3 is over too: each takes its share of the "
                                       "current there, so those cells carry it twice. Put them apart.")
                                        .arg(name, t.name, over.join(QStringLiteral(" and ")));
                returnCells << qMakePair(back, t.name);
            }
            out.returnsOf[p] << PortReturn{t.name, cells, Complex(0, 0)};
            columns << Eigen::VectorXcd(owns.at(p) - back);
            portOf << p;
            shapes << QMap<int, QPair<double, double>>();
            columnReturn << int(out.returnsOf.at(p).size()) - 1;
            against << tr("%1 (%2 cell(s))").arg(t.name).arg(cells);
        }
        const int n = int(against.size());
        const QString list = n == 1 ? against.first() : tr("%1 and %2").arg(against.mid(0, n - 1).join(QStringLiteral(", ")), against.last());
        out.portsSaid[p] = n == 1   ? tr("%1 against %2: a ground-signal probe's.").arg(drivesSaid.at(p), list)
                           : n == 2 ? tr("%1 against %2, tied: a ground-signal-ground probe's.").arg(drivesSaid.at(p), list)
                                    : tr("%1 against %2, all tied.").arg(drivesSaid.at(p), list);
        if (!sameConductor.isEmpty()) {
            const int m = int(sameConductor.size());
            const QString which = m == 1 ? tr("return %1 is").arg(sameConductor.first())
                                         : tr("returns %1 and %2 are").arg(sameConductor.mid(0, m - 1).join(QStringLiteral(", ")),
                                                                           sameConductor.last());
            out.portsSaid[p] += QLatin1Char(' ')
                                + tr("%1's %2 on its own conductor (%3), apart from its cells: the source is across that "
                                     "conductor, as at a shorted line's end or a loop's two ends.")
                                      .arg(name, which, sameNet);
        }
    }
    // A coplanar port that returns through what another port drives - a
    // GSSG's other signal, where no conductor is marked a return - ties the
    // two signals together through each other.
    for (int p = 0; p < np; ++p)
        for (const int net : std::as_const(returnNets.at(p)))
            for (int q = 0; q < np; ++q)
                if (q != p && net >= 0 && ownNet.at(q) == net) {
                    out.problems << tr("%1 returns through %2, which %3 drives: mark the conductors the lines return "
                                       "through (a Return Conductor under Ports on each), and it returns through the "
                                       "nearest of them either side, past the other signals.")
                                        .arg(out.portNames.at(p), nets.value(net).name, out.portNames.at(q));
                    return out;
                }
    for (int a = 0; a < columns.size(); ++a)
        for (int b = 0; b < a; ++b)
            if (portOf.at(a) != portOf.at(b) && (columns.at(a) - columns.at(b)).norm() < 1e-12) {
                out.problems << tr("%1 and %2 drive the same face or cell of the layout: put them apart.")
                                    .arg(out.portNames.at(portOf.at(b)), out.portNames.at(portOf.at(a)));
                return out;
            }
    // And no current into the ports that drives nothing: ports that return
    // through each other round a loop - two named each other's return,
    // three round a ring - would drive no cell between them, and their Z be
    // singular. Of the columns' Gram matrix (columns x columns, of their
    // rank), each combination of columns that comes to nothing, where it is
    // a current into a port, names the ports. (One that is none - a current
    // round the ties of two ports that return through the same two
    // conductors, a GSSG's two signals' - changes nothing any port sees, and
    // the reduction below takes it as it has always taken it.)
    {
        const int n = int(columns.size());
        Eigen::MatrixXd gram(n, n);
        for (int a = 0; a < n; ++a)
            for (int b = 0; b <= a; ++b) gram(a, b) = gram(b, a) = columns.at(a).real().dot(columns.at(b).real());
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(gram);
        QStringList involved;
        for (int e = 0; eig.info() == Eigen::Success && e < n && eig.eigenvalues()(e) <= 1e-12 * eig.eigenvalues()(n - 1); ++e) {
            QList<double> intoPort(np, 0.0);
            for (int k = 0; k < n; ++k) intoPort[portOf.at(k)] += eig.eigenvectors()(k, e);
            for (int q = 0; q < np; ++q)
                if (std::abs(intoPort.at(q)) > 1e-6 && !involved.contains(out.portNames.at(q))) involved << out.portNames.at(q);
        }
        if (!involved.isEmpty()) {
            out.problems << tr("%1 drive no cell between them: their own cells and their returns' come round to nothing (a "
                               "port that returns through another, and that one through it). Give each a return of its "
                               "own.")
                                .arg(involved.join(QStringLiteral(", ")));
            return out;
        }
    }
    c.drive = MatrixC(nc, columns.size());
    for (int k = 0; k < columns.size(); ++k) c.drive.col(k) = columns.at(k);
    c.np = int(columns.size());
    c.portOf = portOf;

    // The cells in metres, and the heights they are at.
    c.rect.resize(nc);
    c.centre.resize(nc);
    c.angle = QList<double>(nc, 0.0);
    QList<double> zOf(nc, -1);
    QRectF extent;   // (of every cell, for how far the Green's tables go)
    for (int i = 0; i < nc; ++i) {
        const MeshCell& cell = mesh.cells.at(cellOf.at(i));
        c.rect[i] = scaledBy(cell.charged(), micrometre);
        c.centre[i] = cell.centre * micrometre;
        c.angle[i] = cell.turned() ? cell.angle : 0.0;
        extent = i == 0 ? c.rect.at(i) : extent.united(c.rect.at(i));
        zOf[i] = metalHeight(stack, cell.layer);
    }
    const double tol = 1e-9 * std::max(1e-9, c.layers.height());
    for (const double z : std::as_const(zOf)) {
        bool known = false;
        for (const double h : std::as_const(c.heights)) known = known || std::abs(h - z) <= tol;
        if (!known) c.heights << z;
    }
    std::sort(c.heights.begin(), c.heights.end());
    const auto heightIndex = [&c, tol](double z) {
        for (int i = 0; i < c.heights.size(); ++i)
            if (std::abs(c.heights.at(i) - z) <= tol) return i;
        return 0;
    };
    c.nh = int(c.heights.size());
    c.hNode.resize(nc);
    c.hRun.resize(nr);
    c.cellZ = zOf;
    for (int i = 0; i < nc; ++i) c.hNode[i] = heightIndex(zOf.at(i));
    // The walls' charge (Context::charges): each cell of a metal solved as
    // its two faces that is at the metal's edge made its face's rectangle
    // and its half of the wall on each side that is the edge - from the face
    // to the metal's middle, a rectangle standing on the side -, the charge
    // shared by area. Left on the faces alone, a coplanar line of 2 µm gold
    // held 2.4 % less than Palace's and QFEM's per metre; with its walls'
    // current alone its Z0 came within 1.9 %, its eps_eff 1 % under.
    {
        // (Beyond a magnetic wall of symmetry the conductors' mirrors: the
        //  cut along the plane is no edge to profile a charge to.)
        // (Each that reaches the plane united with its mirror, as the whole
        //  has it: beside it, the cut read as a gap of nothing, with walls.)
        Thickened mirroredToo = thick;
        if (c.symAxis != 0 && c.symCharge > 0) mirroredToo.conductors = withMirrors(thick.conductors, c.symAxis, c.symAt / micrometre);
        const QHash<int, QList<ChargePart>> walls
            = chargeParts(c.symAxis != 0 && c.symCharge > 0 ? mirroredToo : thick, micrometre,
                          [&](int m) { return metalHeight(stack, mesh.cells.at(m).layer); });
        c.charges = QList<QList<ChargePart>>(nc);
        for (int i = 0; i < nc; ++i) c.charges[i] = walls.value(cellOf.at(i));
    }
    // The ended layers' correction to P between the cells: dense, or (past
    // some thousands of cells, a compressed solve's) as its factors.
    if (!dielectricPlanned.isEmpty()) {
        QList<ChargedCell> charged;
        for (int i = 0; i < nc; ++i) {
            ChargedCell cell;
            cell.rect = c.rect.at(i);
            cell.z = zOf.at(i);
            charged << cell;
        }
        c.dielectric = dielectricCorrection(dielectricPlanned, charged, nc > mostDenseCorrection(),
                                            control != nullptr ? control->stop : nullptr);
        if (control != nullptr && control->stop != nullptr && control->stop->load()) return FullWave();
        if (!c.dielectric.ok()) {
            out.problems << c.dielectric.problems;
            return out;
        }
        c.corrected = true;
        c.dielectricLines = dielectricPlanned.lines + c.dielectric.lines;
        // A dispersive layer that ends: the correction for its shapes is
        // static, made once of its permittivity as given - said, as the
        // quasi-static solve says the frequency it takes it at (the review of
        // 3789cfcb).
        QStringList given, values;
        for (const DielectricPlan::Region& r : std::as_const(dielectricPlanned.regions))
            if (const Layer* l = stack.named(r.layer); l != nullptr && !l->dispersion.isEmpty() && !given.contains(l->name)) {
                given << l->name;
                values << QStringLiteral("%1 eps_r %2 at %3 GHz").arg(l->name).arg(l->epsr, 0, 'g', 4).arg(l->dispersionAt / 1e9, 0, 'g', 4);
            }
        if (!given.isEmpty())
            c.dielectricLines << tr("%1 ends at its shapes, and is dispersive: inside them it is taken at its permittivity as "
                                    "given (%2), at every frequency - the correction for a layer that ends is made once, of "
                                    "statics - and beyond them it is what is there.")
                                     .arg(given.join(QStringLiteral(", ")), values.join(QStringLiteral("; ")));
        // The edge profile's change where a cell is within an ended layer's
        // shapes (Context::profileScale): its charge's own near field, the
        // background's air under a metal on its substrate putting it in air,
        // which the polarization's panels, solved for even charge, do not
        // take back - a stub on a substrate ended 30 µm beside it read 4 %
        // more than on the substrate everywhere, as the quasi-static solve
        // did. The terms' static strength, at a low frequency.
        bool any = false;
        QList<double> scale(nc, 1.0);
        QHash<QString, double> known;
        for (int i = 0; i < nc; ++i) {
            // (Every cell's, a profiled cell's pair with one that is not
            //  scaled by both: Context::profileScale.)
            int mask = 0;
            for (int r = 0; r < dielectricPlanned.regions.size() && r < 30; ++r)
                for (const QPolygonF& o : dielectricPlanned.regions.at(r).outline)
                    if (o.containsPoint(c.centre.at(i), Qt::OddEvenFill)) mask |= 1 << r;
            if (mask == 0) continue;
            const QString key = QString::number(mask) + QLatin1Char('@') + QString::number(zOf.at(i), 'g', 17);
            if (!known.contains(key)) {
                Layers local = c.layers;
                for (int r = 0; r < dielectricPlanned.regions.size() && r < 30; ++r)
                    if (mask & (1 << r)) local.slabs[dielectricPlanned.regions.at(r).slab].epsr += dielectricPlanned.regions.at(r).delta;
                const WaveGreen there(local, zOf.at(i), zOf.at(i), 1e6, 1e-3), background(c.layers, zOf.at(i), zOf.at(i), 1e6, 1e-3);
                const Complex b = background.scalarTerm().coefficient;
                known.insert(key, there.ok() && background.ok() && std::abs(b) > 0 ? (there.scalarTerm().coefficient / b).real() : 1.0);
            }
            scale[i] = known.value(key);
            any = any || scale.at(i) != 1.0;
        }
        if (any) c.profileScale = scale;
    }
    for (int a = 0; a < nr; ++a) c.hRun[a] = a < c.planar ? heightIndex(c.runs.at(a).z0) : -1;
    // Each port's own path to the cells it drives (PortPath): up from the
    // ground, or from the cells of the metal an internal port is driven
    // from, shared out between them as the port shares its current; its
    // span one more of the vias' where it is none of theirs. (None for a
    // port that names its returns, or a coplanar port; and none at all
    // where a test asks for none.)
    for (int k = 0; k < c.np && !c.boxed && (control == nullptr || control->portPaths); ++k) {
        QList<QPair<int, double>> from;
        double fromWeight = 0, zFrom = 0;
        for (int i = 0; i < nc; ++i)
            if (c.drive(i, k).real() < 0) {
                from << qMakePair(i, -c.drive(i, k).real());
                fromWeight -= c.drive(i, k).real();
                zFrom = zOf.at(i);
            }
        for (auto it = shapes.at(k).cbegin(); it != shapes.at(k).cend(); ++it) {
            const int node = it.key();
            const double share = c.drive(node, k).real(), zTo = zOf.at(node);
            if (!(share > 0) || std::abs(zTo - zFrom) <= tol) continue;
            const bool up = zTo > zFrom;
            const double z0 = std::min(zFrom, zTo), z1 = std::max(zFrom, zTo);
            int span = -1;
            for (int s = 0; s < c.spans.size() && span < 0; ++s)
                if (std::abs(c.spans.at(s).z0 - z0) <= tol && std::abs(c.spans.at(s).z1 - z1) <= tol) span = s;
            if (span < 0) {
                span = int(c.spans.size());
                c.spans.append({z0, z1});
            }
            PortPath path;
            path.column = k;
            path.via.span = span;
            path.via.at = c.centre.at(node);
            path.via.side = it.value().first;
            path.via.gmd = it.value().second;
            path.current = up ? share : -share;
            path.ends << qMakePair(node, up ? 1.0 : -1.0);
            for (const QPair<int, double>& end : std::as_const(from))
                path.ends << qMakePair(end.first, (up ? -1.0 : 1.0) * end.second / fromWeight);
            c.paths << path;
        }
    }
    // The ports' own currents where they come up by no path (SourceRun):
    // each such column's two sides - its own cells, the positive part of its
    // column, and its return's, the negative -, a side the same cells by the
    // same shares wherever it comes; the columns whose sides touch one
    // probe, its hub at the middle of its sides' middles, a leg from it to
    // each side's middle (none to a side at the hub), at the height of most
    // of the probe's cells. (In ordered maps, so that every process sums
    // them alike.)
    {
        QSet<int> pathed;
        for (const PortPath& path : std::as_const(c.paths)) pathed.insert(path.column);
        QList<QList<QPair<int, double>>> sides;
        QList<QPointF> middles;
        QList<QPair<int, int>> sidesOf(c.np, qMakePair(-1, -1));
        const auto sideOf = [&](const QList<QPair<int, double>>& cells) {
            const int found = int(sides.indexOf(cells));
            if (found >= 0) return found;
            QPointF middle(0, 0);
            double weight = 0;
            for (const auto& [i, share] : cells) {
                middle += share * c.centre.at(i);
                weight += share;
            }
            sides << cells;
            middles << middle / weight;
            return int(sides.size()) - 1;
        };
        for (int k = 0; k < c.np; ++k) {
            if (pathed.contains(k)) continue;
            QList<QPair<int, double>> plus, minus;
            for (int i = 0; i < nc; ++i) {
                const double share = c.drive(i, k).real();
                if (share > 0) plus << qMakePair(i, share);
                else if (share < 0) minus << qMakePair(i, -share);
            }
            if (plus.isEmpty() || minus.isEmpty()) continue;
            sidesOf[k] = qMakePair(sideOf(plus), sideOf(minus));
        }
        // (The probes: the sides a column joins, by union-find.)
        QList<int> root(sides.size());
        for (int s = 0; s < root.size(); ++s) root[s] = s;
        const std::function<int(int)> find = [&root, &find](int s) { return root[s] == s ? s : root[s] = find(root[s]); };
        for (const QPair<int, int>& two : std::as_const(sidesOf))
            if (two.first >= 0) root[find(two.first)] = find(two.second);
        QMap<int, QList<int>> probes;
        for (int s = 0; s < sides.size(); ++s) probes[find(s)] << s;
        QList<int> legOf(sides.size(), -1);
        const double size = std::max(1e-9, std::hypot(extent.width(), extent.height()));
        for (auto it = probes.cbegin(); it != probes.cend(); ++it) {
            QPointF hub(0, 0);
            QMap<int, double> atHeight;
            for (const int s : it.value()) {
                hub += middles.at(s);
                for (const auto& [i, share] : sides.at(s)) atHeight[c.hNode.at(i)] += share;
            }
            hub /= double(it.value().size());
            int height = atHeight.firstKey();
            for (auto h = atHeight.cbegin(); h != atHeight.cend(); ++h)
                if (h.value() > atHeight.value(height)) height = h.key();
            for (const int s : it.value()) {
                SourceRun leg;
                leg.from = hub;
                leg.to = middles.at(s);
                leg.height = height;
                leg.length = std::hypot(leg.to.x() - hub.x(), leg.to.y() - hub.y());
                if (!(leg.length > 1e-9 * size)) continue;
                leg.dirX = (leg.to.x() - hub.x()) / leg.length;
                leg.dirY = (leg.to.y() - hub.y()) / leg.length;
                legOf[s] = int(c.sources.size());
                c.sources << leg;
            }
        }
        c.sourceOf = Eigen::MatrixXd::Zero(c.sources.size(), c.np);
        for (int k = 0; k < c.np; ++k) {
            const auto [mine, back] = sidesOf.at(k);
            if (mine < 0) continue;
            if (legOf.at(mine) >= 0) c.sourceOf(legOf.at(mine), k) += 1;
            if (legOf.at(back) >= 0) c.sourceOf(legOf.at(back), k) -= 1;
        }
    }
    // As far as any two points of the layout are apart, and a little more.
    c.rhoMax = 1.05 * std::hypot(extent.width(), extent.height()) + 1e-9;
    // (A box's images out to its reach beyond the box: its tables too, so
    //  that a far image is read, not integrated.)
    if (c.boxed) c.rhoMax = std::max(c.rhoMax, 1.05 * (std::hypot(c.layers.box.width(), c.layers.box.height()) + c.boxReach));
    // (A plane of symmetry's mirrors: as far as a cell is from another's.)
    if (c.symAxis != 0) {
        const QRectF image = QRectF(c.symmetric(extent.topLeft()), c.symmetric(extent.bottomRight())).normalized();
        const QRectF both = extent.united(image);
        c.rhoMax = std::max(c.rhoMax, 1.05 * std::hypot(both.width(), both.height()) + 1e-9);
    }

    // What does not change with the frequency: each run's direction and
    // middle, and its image in the ground; for a dense solve, the runs'
    // static partial inductances, direct and of their images, and the
    // static potential of each cell's charge spread over it - the singular
    // parts of the two kernels, in closed form, made on every thread before
    // the frequencies are (makeStatics()). The partial inductances are
    // aligned (alignedInductance()), the cosine between two runs taken
    // apart from them: of two runs neither parallel nor at right angles,
    // the partial inductance was taken as though they were parallel and
    // each as long as it is, without that cosine, where Neumann's formula
    // has it - an oblique run is one between cells cut to a slanting
    // outline, whose centres do not line up.
    c.dirX.resize(nr);
    c.dirY.resize(nr);
    c.midX.resize(nr);
    c.midY.resize(nr);
    c.mirrored = c.runs;
    for (int a = 0; a < nr; ++a) {
        c.runs.at(a).direction(&c.dirX[a], &c.dirY[a], nullptr);
        c.runs.at(a).middle(&c.midX[a], &c.midY[a], nullptr);
        c.mirrored[a].z0 = c.mirrored[a].z1 = -c.runs.at(a).z0;
    }
    if (c.symAxis != 0) {
        for (int a = 0; a < c.planar; ++a)
            if (std::abs(c.dirX.at(a)) > 1e-9 && std::abs(c.dirY.at(a)) > 1e-9) {
                out.problems << tr("With a plane of symmetry the runs go along x or y: a conductor's slanting outline meshed into runs "
                                   "at an angle is not solved with one yet.");
                return out;
            }
    }
    if (c.boxed) {
        bool oblique = false;
        for (int a = 0; a < c.planar; ++a) oblique = oblique || (std::abs(c.dirX.at(a)) > 1e-9 && std::abs(c.dirY.at(a)) > 1e-9);
        if (!c.vias.isEmpty() || oblique) {
            out.problems << (!c.vias.isEmpty() ? tr("A via in a box is not solved yet: its kernels' walls are a vertical current's.")
                                               : tr("A box's runs go along x or y: a conductor's slanting outline meshed into runs at an "
                                                    "angle is not solved in one."));
            return out;
        }
    }
    if (c.layers.groundAbove) {
        c.lidMirrored = c.runs;
        for (int a = 0; a < nr; ++a) c.lidMirrored[a].z0 = c.lidMirrored[a].z1 = 2 * c.layers.height() - c.runs.at(a).z0;
        bool magnetic = false;
        for (const Layers::Slab& s : c.layers.slabs) magnetic = magnetic || s.mur != 1 || s.mtand != 0;
        c.lidFolded = c.layers.groundBelow && !magnetic;
    }
    wallsOf(c);
    const int nf = int(frequencies.size());
    int threads = control != nullptr && control->threads > 0 ? control->threads
                                                              : int(std::max(1u, std::thread::hardware_concurrency()));
    // (Made in place, and pointed at by the matrices made from it.)
    Compression z;
    int side = 1;   // frequencies at a time
    bool symmetricLu = false;
    int machineAtOnce = 1;   // frequencies the machine would solve at once (what chooses L D L^T)
    if (!compress) {
        makeStatics(c, threads);
        // The frequencies side by side: each its own Green's functions and
        // its own system, the static parts shared. As many at a time as the
        // machine has threads, and no more than the memory allows - a system,
        // factorized where it is, and P are some 16 (nr^2 + nc^2) bytes a
        // frequency (measured in the app: 258 MB at 3588 runs over 1800
        // cells, 266 each of three at once) -, the threads they leave shared
        // out between them to fill their systems.
        const double perFrequency = 16.0 * (double(nr) * nr + double(nc) * nc) + 1e6;
        side = std::min({threads, nf, std::max(1, int(sweepMemory() / perFrequency))});
        // The systems factorized as L D L^T where the machine would solve
        // kSymmetricFrom frequencies or more at once: what the machine has,
        // not the threads asked, so that a solve is the same to the last
        // digit on any number of them.
        const int machine = int(std::max(1u, std::thread::hardware_concurrency()));
        const int here = std::min({machine, nf, std::max(1, int(sweepMemory() / perFrequency))});
        machineAtOnce = here;
        symmetricLu = control != nullptr && control->symmetricLu >= 0 ? control->symmetricLu > 0 : here >= kSymmetricFrom;
    } else {
        // Compressed: the trees and the shapes of the matrices, the same at
        // every frequency, and what a frequency will take. As many
        // frequencies at a time as the memory allows, the threads shared
        // out between them to fill and multiply their matrices.
        z.tolerance = control != nullptr && control->tolerance > 0 ? control->tolerance : kTolerance;
        plan(z, c);
        const double memory = physicalMemory();
        const double budget = control != nullptr && control->mostBytes > 0 ? control->mostBytes
                                                                            : (memory > 0 ? 0.5 * memory : 4e9);
        if (z.estimate > budget) {
            out.tooLarge = true;
            out.problems << tr("%1 runs of current over %2 cells, held compressed, would take some %3 GB a frequency, more "
                               "than the %4 GB a solve may take here. Mesh the layout more coarsely: fewer cells across a "
                               "conductor, or a larger largest cell.")
                                .arg(nr).arg(nc).arg(z.estimate / 1e9, 0, 'g', 3).arg(budget / 1e9, 0, 'g', 3);
            return out;
        }
        side = std::min({threads, nf, std::max(1, int(budget / z.estimate))});
        // (Every frequency's static parts of the pairs that make them dear,
        //  made once: in what a tenth of the budget holds, 2 GB at most.)
        if (control == nullptr || control->staticCache) cacheStatics(c, z, threads, std::min(2e9, 0.1 * budget));
    }
    const int inner = std::max(1, threads / side);
    out.threads = side * inner;
    if (control != nullptr && control->atOnce != nullptr) control->atOnce->store(side);
    if (control != nullptr && control->begins) control->begins(nf);
    // (Where a port has more than one column, its others less its first
    //  solved for as columns of their own.)
    c.basis = columnBasis(c, np);
    // (A vector, not a QList: the threads write to it side by side, and a
    //  QList's implicit sharing is no place for that.)
    std::vector<AtFrequency> solved(static_cast<std::size_t>(nf));
    std::atomic_int next{0};
    std::atomic_bool failed{false};
    // (Compressed, the highest frequency first: where the charges as
    //  unknowns of their own fall short, the lower ones are not tried so -
    //  the smallest ratio of the charges' term it fell short at.)
    std::vector<int> order(static_cast<std::size_t>(nf));
    std::iota(order.begin(), order.end(), 0);
    if (compress)
        std::stable_sort(order.begin(), order.end(), [&frequencies](int a, int b) { return frequencies.at(a) > frequencies.at(b); });
    // (Each thread's own, kept from one of its frequencies to its next,
    //  lower one: the near field's blocks, NearBlocks; and the least ratio of
    //  the charges' term it fell short at, solveCompressed().)
    std::vector<NearBlocks> kept(static_cast<std::size_t>(side));
    std::vector<double> shortAt(static_cast<std::size_t>(side), std::numeric_limits<double>::infinity());
    const auto work = [&c, &z, &frequencies, &solved, &next, &failed, &order, &shortAt, &kept, control, nf, compress, inner,
                       side, symmetricLu](int thread) {
        const std::atomic_bool* stop = control != nullptr ? control->stop : nullptr;
        // (Compressed, each thread's frequencies dealt to it in turn - the
        //  highest to the first, the next to the second, and round -, not
        //  as it comes free: which kept blocks a frequency is solved on
        //  (NearBlocks) the sweep's and its count of threads' alone, the
        //  same to the bit from one run to the next. The frequencies cost
        //  much alike, and dealt in the descending order each thread's share
        //  spans the sweep.)
        for (int n = compress ? thread : next++; n < nf; n = compress ? n + side : next++) {
            const int k = order[std::size_t(n)];
            if (failed || (stop != nullptr && stop->load())) return;
            if (!allowedOne(control)) return;
            solved[std::size_t(k)] = compress ? solveCompressed(c, z, frequencies.at(k), inner, control, &shortAt[std::size_t(thread)], &kept[std::size_t(thread)])
                                                : solveAt(c, frequencies.at(k), inner, symmetricLu, control != nullptr ? control->augmentedFrom : 0.0);
            if (!solved[std::size_t(k)].problems.isEmpty()) failed = true;
            if (control != nullptr && control->done != nullptr) ++*control->done;
        }
    };
    onThreads(side, [&work](int thread) { work(thread); });
    kept.clear();
    if (control != nullptr && control->stop != nullptr && control->stop->load()) {
        int done = 0;
        for (const AtFrequency& a : std::as_const(solved)) done += a.z.size() > 0 ? 1 : 0;
        out.problems << tr("Stopped, %1 of %2 frequencies solved.").arg(done).arg(nf);
        return out;
    }
    for (const AtFrequency& a : std::as_const(solved))
        if (!a.problems.isEmpty()) {
            out.problems << a.problems;
            return out;
        }

    out.lines << c.dielectricLines;
    out.lines << tr("%1 run(s) of current over %2 cell(s), at %3 metal height(s); %4 port(s), %5 frequency(ies) from "
                    "%6 to %7 GHz, %8 at a time.")
                     .arg(nr).arg(nc).arg(c.nh).arg(np).arg(nf)
                     .arg(*std::min_element(frequencies.begin(), frequencies.end()) / 1e9, 0, 'g', 4)
                     .arg(*std::max_element(frequencies.begin(), frequencies.end()) / 1e9, 0, 'g', 4)
                     .arg(side);
    if (compress) {
        // What the compressed matrices held, of the frequency that held the
        // most, and how GMRES did over every frequency and port.
        out.compressed = true;
        out.tolerance = z.tolerance;
        out.denseMemory = z.denseBytes;
        for (const AtFrequency& a : std::as_const(solved)) {
            out.memory = std::max(out.memory, a.bytes);
            out.largestRank = std::max(out.largestRank, a.largestRank);
            out.iterations = std::max(out.iterations, a.iterations);
            out.residual = std::max(out.residual, a.residual);
        }
        const auto size = [](double bytes) {
            return bytes >= 1e9 ? tr("%1 GB").arg(bytes / 1e9, 0, 'f', 2) : tr("%1 MB").arg(bytes / 1e6, 0, 'f', 1);
        };
        out.lines << tr("The system held compressed (%1 runs and cells, past the %2 a dense matrix is set up for, or as "
                        "asked): its blocks between clusters of runs and cells apart beside their size each of low rank, "
                        "to a part in %3 of its own size (rank %4 at most), the near ones dense - %5 where dense it would "
                        "be %6, %7 thread(s) a frequency. Solved by GMRES, preconditioned by the near field's sparse LU: "
                        "%8 iteration(s) at most, to a residual of %9; the ports' Z from the currents variationally.")
                         .arg(nr + nc).arg(most).arg(1.0 / z.tolerance, 0, 'g', 3).arg(out.largestRank)
                         .arg(size(out.memory), size(out.denseMemory)).arg(inner).arg(out.iterations)
                         .arg(out.residual, 0, 'g', 2);
        // (The near field's blocks kept from one frequency to the next on a
        //  thread: NearBlocks.)
        int looped = 0, reused = 0;
        for (const AtFrequency& a : std::as_const(solved)) {
            looped += a.loopTree ? 1 : 0;
            reused += a.loopTree && a.reusedFrom > 0 ? 1 : 0;
        }
        if (reused > 0)
            out.lines << tr("Of the %1 frequencies solved with the charges as unknowns of their own, %2 were preconditioned "
                            "by the near field's blocks as a higher frequency of their thread factorized them, scaled by "
                            "their own jw; %3 factorized their own.")
                             .arg(looped).arg(reused).arg(looped - reused);
        // (Each frequency's GMRES short of its residual, or near its limit.)
        for (const AtFrequency& a : std::as_const(solved))
            for (const QString& w : a.warnings) {
                out.warnings << w;
                out.lines << w;
            }
    } else {
        for (const AtFrequency& a : std::as_const(solved)) {
            out.fillSeconds += a.fillSeconds;
            out.luSeconds += a.luSeconds;
            out.accelerated = out.accelerated || a.accelerated;
            out.openBlas = out.openBlas || a.openBlas;
            out.symmetricLu = out.symmetricLu || a.symmetric;
        }
        // (Held dense, said as the compressed one is: the mesh's estimate of
        //  it held compressed is not what was done - NF6 of the open items.)
        const double bytes = 16.0 * double(nr + nc) * double(nr + nc);
        out.lines << tr("The system held dense (%1 runs and cells, %2 a dense matrix is set up for): %3 a frequency, "
                        "solved by LU.")
                         .arg(nr + nc)
                         .arg(nr + nc > most ? tr("past the %1, as asked").arg(most) : tr("within the %1").arg(most))
                         .arg(bytes >= 1e9 ? tr("%1 GB").arg(bytes / 1e9, 0, 'f', 2) : tr("%1 MB").arg(bytes / 1e6, 0, 'f', 1));
        if (out.symmetricLu)
            out.lines << tr("Each frequency's system factorized as the symmetric matrix it is, L D L^T - half an LU's "
                            "arithmetic, the quicker where many frequencies at once share the machine: this machine "
                            "would solve %1 at once.")
                             .arg(machineAtOnce);
    }
    out.lines << out.portsSaid;
    if (!naming.isEmpty())
        out.lines << tr("%1 port(s) return through the terminals and ports they name, each port's returns tied as a "
                        "probe's tips are: a source between its own cells and theirs, with no path up to them, so that "
                        "its reference plane is at those cells and the probe's own loop is not in the answer - its "
                        "current between them radiating, its loop's inductance left out.")
                         .arg(naming.size());
    out.lines << thick.said;
    // (What the walls carry, where a metal is solved as its two faces: the
    //  edge runs' current and the edge cells' charge on them, and their loss
    //  from each metal's edge solved in its cross-section.)
    {
        int alongWalls = 0, charged = 0;
        // (Parts that stand: a sheet's runs and cells have parts too, the
        //  edge's profile, and no walls.)
        for (const QList<Context::Part>& p : std::as_const(c.parts))
            alongWalls += std::any_of(p.begin(), p.end(), [](const Context::Part& q) { return q.run.rise > 0; }) ? 1 : 0;
        for (const QList<ChargePart>& p : std::as_const(c.charges))
            charged += std::any_of(p.begin(), p.end(), [](const ChargePart& q) { return q.plane != 0; }) ? 1 : 0;
        if (alongWalls > 0 || charged > 0)
            out.lines << tr("The walls of the metals solved as their two faces carry current: %1 run(s) along their edges, "
                            "each its face's strip and its half of the wall, their loss from each metal's edge solved in its "
                            "cross-section; and charge where a wall faces another's across a narrow gap: %2 cell(s) there, "
                            "each its face and its half of the wall.")
                             .arg(alongWalls)
                             .arg(charged);
    }
    if (!walled.isEmpty())
        out.lines << tr("%1 of the runs are the walls of the metals solved as their two faces, each from a cell along an "
                        "edge up to the same cell of the top face.")
                         .arg(walled.size());
    if (c.vias.size() > walled.size())
        out.lines << tr("%1 of the runs are vias, %2 of them to the ground: a via's current runs up it from the cell "
                        "under it to the cell over it, entering each at its centre.")
                         .arg(c.vias.size() - walled.size()).arg(grounded);
    if (!c.paths.isEmpty())
        out.lines << tr("The ports' currents come up to the cells they drive by %1 path(s) of their own, each from the "
                        "ground or from the metal an internal port is driven from, with the field a via's current has: "
                        "part of each port's own discontinuity.")
                         .arg(c.paths.size());
    out.lines << viaWarned;
    out.warnings << viaWarned;
    // A metal lid: what it is, and what its guide does to the ports.
    if (c.layers.groundAbove) {
        double gap = 0;
        for (const Layer& l : stack.layers)
            if (l.kind == Layer::Cover) gap = l.thickness;
        out.lines << tr("A perfect metal lid closes the stack %1 µm over its top. With the ground it is a guide of two "
                        "plates: a port's path up from the ground couples to the other ports' through it as two long "
                        "wires do, not dying with distance as over an open ground, and is part of what the ports "
                        "measure - a port's own de-embedding takes out its own path, not that.")
                         .arg(gap * 1e6, 0, 'g', 4);
        // Open at its sides, the guide takes what a port returning through
        // the ground puts into it away along it and back: a warning, not a
        // note alone (P38 of the open items: a stripline under such a lid
        // read eps_eff 3.14 for 3, its loss four times its metal's).
        bool fromGround = false;
        for (const FullWavePort& p : ports) fromGround = fromGround || !p.coplanar;
        if (!c.boxed && fromGround) {
            const QString w = tr("The lid has no side walls: with the ground it is a guide of two plates open on every side, "
                                 "and a port returning through the ground feeds it - what it carries away and back is in S, "
                                 "so a line's eps_eff and loss read the guide's too. Side walls round the layout (a box) "
                                 "close it; ports returning through the conductors beside them (coplanar) feed it the less.");
            out.warnings << w;
        }
    }
    int alongEdges = 0;
    for (const int free : std::as_const(c.freeSides)) alongEdges += free > 0 ? 1 : 0;
    const QString sheet = tr("a metal as a sheet with the surface impedance of its thickness, and more in the cells along "
                             "its edges, where the current crowds as close as the thickness lets it (%1 run(s) along an "
                             "edge)")
                              .arg(alongEdges);
    if (!c.layers.groundBelow)
        out.lines << tr("There is no ground plane: under the stack is air, as far down as it goes. %1; a coplanar port's "
                        "current as coming back through the conductors beside it, and an internal port's through the "
                        "metal it is driven from.")
                         .arg(sheet.left(1).toUpper() + sheet.mid(1));
    else {
        out.lines << (coplanar ? tr("The ground plane is taken as perfect, %1; an edge port's current as coming back "
                                    "through the ground, and a coplanar port's through the conductors beside it.")
                                     .arg(sheet)
                               : tr("The ground plane is taken as perfect, %1; an edge port's current as coming back "
                                    "through the ground.")
                                     .arg(sheet));
        // (Its loss: counted where asked for; where it has one and is not
        //  asked, said - the quasi-static extraction counts it always.)
        const Layer* ground = nullptr;
        for (const Layer& l : stack.layers)
            if (l.kind == Layer::Ground) ground = &l;
        const bool lossy = ground != nullptr && ground->sigma > 0
                           && (ground->variant == QLatin1String("metal") || ground->variant == QLatin1String("impedance"));
        const QString what = ground == nullptr ? QString()
                             : ground->variant == QLatin1String("impedance")
                                 ? tr("a surface impedance of %1 Ohm per square").arg(ground->sigma, 0, 'g', 4)
                                 : tr("a metal of %1 S/m").arg(ground->sigma, 0, 'g', 4);
        if (!c.groundPairs.isEmpty())
            out.lines << tr("The ground plane's own loss, %1, counted on the current each run's mirror image carries - the "
                            "fields still a perfect plane's -: %2 pair(s) of runs within ten heights of one another.")
                             .arg(what)
                             .arg(c.groundPairs.size());
        else if (lossy)
            out.lines << tr("The ground plane is %1, and its loss is not counted here - its \"Its loss in the full-wave "
                            "solve\" counts it; the quasi-static extraction always does.")
                             .arg(what);
    }
    if (coplanar && isReturn.contains(true)) out.lines << returnsSaid;
    if (!out.pairs.isEmpty()) {
        QStringList pairs;
        for (const QPair<int, int>& pair : std::as_const(out.pairs))
            pairs << tr("%1 and %2").arg(out.portNames.at(pair.first), out.portNames.at(pair.second));
        out.lines << tr("Differential pairs, each its plus port first: %1. Their mixed-mode S is each pair's "
                        "differential mode (its plus port's wave less its minus's, over the root of two, into twice the "
                        "ports' reference) and common mode (the two's sum over the root of two, into half), then the "
                        "ports of no pair as they are.")
                         .arg(pairs.join(QStringLiteral("; ")));
    }
    // The columns of a port side by side: one voltage, their currents
    // added. The ports' Z is (B^T Z^-1 B)^-1, B which port each column is
    // of, and an ampere into a port is Z^-1 B Z into the columns.
    // Each return's share of its port's current at the top frequency: the
    // current into its column with an ampere into the port, the others open
    // - the whole of it where the port has one column.
    MatrixC ofPort = MatrixC::Zero(c.np, np);
    for (int k = 0; k < c.np; ++k) ofPort(k, c.portOf.at(k)) = 1;
    const int topAt = int(std::max_element(frequencies.begin(), frequencies.end()) - frequencies.begin());
    const auto shareOut = [&out, &c, &columnReturn](const MatrixC& intoColumns) {
        for (int k = 0; k < c.np; ++k)
            if (columnReturn.at(k) >= 0)
                out.returnsOf[c.portOf.at(k)][columnReturn.at(k)].share = intoColumns(k, c.portOf.at(k));
    };
    // (Each frequency's currents kept where they fit, else at some of them
    //  alone - keptCurrentsOf(). And each frequency's own answer moved out of
    //  the sweep's, not copied: two copies of the currents for every
    //  frequency.)
    const QList<bool> keepCurrents = keptCurrentsOf(nf, 16.0 * np * nr, topAt);
    const int keptCount = int(std::count(keepCurrents.begin(), keepCurrents.end(), true));
    for (int k = 0; k < nf; ++k) {
        AtFrequency a = std::move(solved[std::size_t(k)]);
        if (!keepCurrents.at(k)) a.current.resize(0, 0);
        if (c.basis.size() > 0) {
            // In the columns solved for (Context::basis): each port's first,
            // then the differences, which carry no port's current. Tied, the
            // differences' voltages nought: their currents w = -Z_ww^-1 Z_wu,
            // and the ports' Z the Schur complement Z_uu - Z_uw Z_ww^-1 Z_wu.
            // (Scaled to a unit diagonal it was needed while the loops were
            //  differences of differences; as columns of their own, not.)
            const int nw = c.np - np;
            const MatrixC zww = a.z.bottomRightCorner(nw, nw), zwu = a.z.bottomLeftCorner(nw, np);
            MatrixC w = Eigen::PartialPivLU<MatrixC>(zww).solve(zwu);
            // (Two returns over the same cells: a difference of nothing, its
            //  row nought - of the least squares there, as below.)
            if (!w.allFinite()) {
                Eigen::CompleteOrthogonalDecomposition<MatrixC> cod(zww);
                cod.setThreshold(1e-10);
                w = cod.solve(zwu);
            }
            const MatrixC z = a.z.topLeftCorner(np, np) - a.z.topRightCorner(np, nw) * w;
            if (!z.allFinite() || !w.allFinite()) {
                out.problems << tr("At %1 GHz the returns of the ports that have more than one could not be tied: their "
                                   "columns came out singular.")
                                    .arg(frequencies.at(k) / 1e9, 0, 'g', 4);
                return out;
            }
            // (An ampere into each port, in the columns solved for, and in
            //  the columns as they are for its returns' shares.)
            MatrixC into(c.np, np);
            into.topRows(np) = MatrixC::Identity(np, np);
            into.bottomRows(nw) = -w;
            if (k == topAt) shareOut(c.basis * into);
            if (a.current.size() > 0) a.current = a.current * into;
            a.z = z;
        } else if (c.np != np) {
            MatrixC x = Eigen::PartialPivLU<MatrixC>(a.z).solve(ofPort);
            MatrixC y = ofPort.transpose() * x;
            MatrixC z = Eigen::PartialPivLU<MatrixC>(y).solve(MatrixC::Identity(np, np));
            // Two returns over the same cells are two columns alike, and
            // their matrix singular to its last digits: of its least squares
            // there, each of the two half the current - where an LU's pivot
            // of rounding alone gave numbers or none by the rounding (the
            // edge profile's, the plan's phase 5, none).
            if (!z.allFinite() || !x.allFinite()) {
                Eigen::CompleteOrthogonalDecomposition<MatrixC> cod(a.z);
                cod.setThreshold(1e-10);
                x = cod.solve(ofPort);
                y = ofPort.transpose() * x;
                z = Eigen::PartialPivLU<MatrixC>(y).solve(MatrixC::Identity(np, np));
            }
            if (!z.allFinite()) {
                out.problems << tr("At %1 GHz the returns of the ports that have more than one could not be tied: their "
                                   "columns came out singular.")
                                    .arg(frequencies.at(k) / 1e9, 0, 'g', 4);
                return out;
            }
            if (k == topAt) shareOut(x * z);
            if (a.current.size() > 0) a.current = a.current * (x * z);
            a.z = z;
        } else if (k == topAt) {
            shareOut(ofPort);
        }
        // (Ports on a magnetic wall: each the half of its face, carrying half
        //  of the whole's current at its voltage - the whole's Z half the
        //  half's, and the half's currents of half an ampere.)
        if (c.halved) {
            a.z *= 0.5;
            a.current *= 0.5;
        }
        const QList<QList<Complex>> zRows = rowsOf(a.z);
        out.frequencies << frequencies.at(k);
        out.z << zRows;
        out.s << scatteringOf(zRows, out.references);
        // Compressed with the charges as unknowns, Z is first order in GMRES's
        // residual: where that leaves Z's terms further apart than a part of
        // the ports' references, S - a line's S21 wants Z11 - Z21 to some
        // 10^-11 of Z at 1 Hz - is not to the digits, said (a 2 mm coplanar line's S21 at 1 Hz 0.42, its Z right).
        if (a.loopTree && a.z.size() > 0) {
            const double largest = a.z.cwiseAbs().maxCoeff();
            const double least = *std::min_element(out.references.begin(), out.references.end());
            const double apart = a.residual * largest;
            if (apart > 1e-3 * least) {
                const QString w = tr("At %1 Hz Z's terms are some %2 Ohm, and GMRES's residual of %3 leaves them some %4 Ohm "
                                     "from what they are, beside the ports' %5 Ohm: S there is not to the digits, as its "
                                     "terms are small differences of Z's. Solve it dense, or start the sweep higher.")
                                      .arg(frequencies.at(k), 0, 'g', 4)
                                      .arg(largest, 0, 'g', 3)
                                      .arg(a.residual, 0, 'g', 2)
                                      .arg(apart, 0, 'g', 2)
                                      .arg(least, 0, 'g', 4);
                out.warnings << w;
                out.lines << w;
            }
        }
        QList<QList<Complex>> perPort;
        for (int q = 0; q < np && a.current.size() > 0; ++q) {
            QList<Complex> column;
            column.reserve(nr);
            for (int r = 0; r < nr; ++r) column << a.current(r, q);
            perPort << column;
        }
        out.currents << perPort;
        // Reciprocity, and passivity: what S gives back of every wave in.
        const double largest = a.z.cwiseAbs().maxCoeff();
        if (largest > 0) out.asymmetry = std::max(out.asymmetry, (a.z - a.z.transpose()).cwiseAbs().maxCoeff() / largest);
        const MatrixC s = matrixOf(out.s.last());
        const Eigen::SelfAdjointEigenSolver<MatrixC> eig(s.adjoint() * s);
        if (eig.info() == Eigen::Success && eig.eigenvalues().maxCoeff() - 1 > out.gain) {
            out.gain = eig.eigenvalues().maxCoeff() - 1;
            out.gainAt = frequencies.at(k);
        }
        // Past its low-frequency limit (N4 of the open items): the charges'
        // term so far over the currents' that they are lost in its rounding -
        // a spiral at 10 Hz read S21 -118 dB, an open, with nothing but
        // GMRES's iterations said. Dense, some 1e15 - a line's loss within a
        // part in a thousand at 2.5e15, 2 % off at 2.5e16: 1e16 -, compressed
        // alike; but where GMRES ran near its limit, as it did there, from
        // 1e12. The ratio goes as one over the frequency where the metal's
        // resistance outweighs its reactance, as it does there.
        const int mostIterations = control != nullptr && control->mostIterations > 0 ? control->mostIterations : kMostIterations;
        const bool struggled = compress && a.iterations >= kNearMiss * mostIterations;
        // (Solved with the charges as unknowns its condition goes as the
        //  ratio's root: its limit the square of the eliminated one's.)
        if (const double limit = a.augmented ? (struggled ? 1e24 : 1e32) : struggled ? 1e12 : 1e16; a.chargeOverSeries > limit)
            out.warnings << tr("At %1 Hz the solve is past its low-frequency limit: its charges' term outweighs its currents' "
                               "%2 times, and the currents are lost in its rounding - a coil may read as an open. Solved "
                               "above some %3 Hz it is not.")
                                .arg(frequencies.at(k), 0, 'g', 4)
                                .arg(a.chargeOverSeries, 0, 'g', 2)
                                .arg(frequencies.at(k) * a.chargeOverSeries / limit, 0, 'g', 2);
        if (a.loopTree)
            out.lines << tr("At %1 Hz the charges' term outweighs the currents' %2 times: solved with the cells' charges as "
                            "unknowns of their own and the currents as loops and a tree - the loops free of the charges' "
                            "term -, the ports' voltages taken along the tree.")
                             .arg(frequencies.at(k), 0, 'g', 4)
                             .arg(a.chargeOverSeries, 0, 'g', 2);
        else if (a.augmented)
            out.lines << tr("At %1 Hz the charges' term outweighs the currents' %2 times: solved with the cells' charges as "
                            "unknowns of their own, whose condition goes as the root of that.")
                             .arg(frequencies.at(k), 0, 'g', 4)
                             .arg(a.chargeOverSeries, 0, 'g', 2);
        else if (a.augmentedTried > 0)
            out.lines << tr("At %1 Hz the charges' term outweighs the currents' %2 times: with the cells' charges as unknowns "
                            "of their own GMRES fell short of its residual in %3 iterations, so solved with them eliminated.")
                             .arg(frequencies.at(k), 0, 'g', 4)
                             .arg(a.chargeOverSeries, 0, 'g', 2)
                             .arg(a.augmentedTried);
        else if (a.augmentedShortAt > 0)
            out.lines << tr("At %1 Hz the charges' term outweighs the currents' %2 times: solved with the cells' charges "
                            "eliminated, as with them unknowns of their own GMRES fell short at a higher frequency of this "
                            "sweep, at %3 times.")
                             .arg(frequencies.at(k), 0, 'g', 4)
                             .arg(a.chargeOverSeries, 0, 'g', 2)
                             .arg(a.augmentedShortAt, 0, 'g', 2);
        else if (!compress && a.chargeOverSeries > 1e6)
            out.lines << tr("At %1 Hz the charges' term outweighs the currents' %2 times: solved with the charges eliminated, "
                            "some %3 digits of it lost.")
                             .arg(frequencies.at(k), 0, 'g', 4)
                             .arg(a.chargeOverSeries, 0, 'g', 2)
                             .arg(int(std::round(std::log10(a.chargeOverSeries))));
        if (a.rcond < 1e-14)
            out.lines << tr("At %1 GHz the system is nearly singular (a reciprocal condition of %2): the answer there may "
                            "be rounding.")
                             .arg(frequencies.at(k) / 1e9, 0, 'g', 4).arg(a.rcond, 0, 'g', 2);
    }
    if (keptCount < nf)
        out.lines << tr("Each run's current kept at %1 of the %2 frequencies, evenly spread, the highest among them: at "
                        "every one they would hold %3 GB. The drawn currents and the far field are of those.")
                         .arg(keptCount).arg(nf).arg(16.0 * np * nr * nf / 1e9, 0, 'f', 1);
    out.solvedAsymmetry = out.asymmetry;
    out.solvedGain = out.gain;
    out.solvedGainAt = out.gainAt;
    out.lines << returnSharesOf(out);
    // Where the stack begins to carry a second surface wave - TE1 over a
    // ground - which a strip launches beside the TM0 of every frequency: the
    // top of a microstrip's band on it, said where the sweep passes it.
    // (Taken in a phase E check for what gave power back
    // past it, which was the ports' own paths, then without a field:
    // addPortPaths().)
    const double top = *std::max_element(frequencies.begin(), frequencies.end());
    out.secondWave = secondSurfaceWave(givenStack, 2 * top);
    // A box's resonances up to the highest frequency solved, and one near one -
    // within a hundredth - flagged: there, without loss, the answer is the
    // box's mode.
    if (c.boxed) {
        const QList<BoxResonance> modes = boxResonances(c.layers, top);
        QStringList said;
        for (const BoxResonance& r : modes)
            said << tr("%1 GHz (%2 %3,%4)").arg(r.frequency / 1e9, 0, 'g', 5).arg(r.tm ? QStringLiteral("TM") : QStringLiteral("TE")).arg(r.m).arg(r.n);
        // (Up to the highest frequency solved, which a solve asked at one
        //  frequency may put past the study's top: said as a number.)
        out.lines << (said.isEmpty() ? tr("The box resonates at no frequency up to %1 GHz, the highest solved.").arg(top / 1e9, 0, 'g', 5)
                                     : tr("The box resonates, up to %1 GHz, the highest solved, at %2.")
                                           .arg(top / 1e9, 0, 'g', 5)
                                           .arg(said.join(QStringLiteral(", "))));
        for (const double f : frequencies)
            for (const BoxResonance& r : modes)
                if (std::abs(f / r.frequency - 1) < 0.01) {
                    const QString warning = tr("%1 GHz is within a hundredth of the box's resonance at %2 GHz: there the answer is "
                                               "the box's mode, as sharp as its losses let it be.")
                                                .arg(f / 1e9, 0, 'g', 5).arg(r.frequency / 1e9, 0, 'g', 5);
                    out.lines << warning;
                    out.warnings << warning;
                }
    }
    if (out.secondWave > 0 && top >= out.secondWave && c.layers.groundAbove)
        out.lines << tr("From %1 GHz the guide between the ground and the lid carries its TE1 beside the wave of every "
                        "frequency: a line under the lid gives more of what it carries to the guide past it.")
                         .arg(out.secondWave / 1e9, 0, 'g', 4);
    else if (out.secondWave > 0 && top >= out.secondWave)
        out.lines << tr("From %1 GHz the stack carries a second surface wave, its TE1, which a strip launches beside the "
                        "TM0 of every frequency: the top of a microstrip's band on it, past which a line gives more of "
                        "what it carries to the substrate.")
                         .arg(out.secondWave / 1e9, 0, 'g', 4);
    out.geometry = c.runs;
    for (const PortPath& p : std::as_const(c.paths))
        out.portPaths << FullWave::PortPathLeg{p.column, p.via.at, c.spans.at(p.via.span).z0, c.spans.at(p.via.span).z1, p.current};
    out.ends = ends;
    // As the caller meshed it: a top face's run in its bottom's - the
    // current through the whole metal there - the walls' runs left out, and
    // a via landing on a top face on its metal's cell (thickened()).
    if (thick.any) {
        QList<int> kept;
        QHash<int, int> keptAt;
        for (int a = 0; a < nr; ++a) {
            if ((a < c.planar && thick.bottomOf.contains(ends.at(a).second)) || walled.contains(a)) continue;
            keptAt.insert(a, int(kept.size()));
            kept << a;
        }
        const auto metalCell = [&thick](int cell) { return cell < 0 ? cell : thick.bottomOf.value(cell, cell); };
        QList<Filament> geometry;
        QList<QPair<int, int>> mapped;
        for (const int a : std::as_const(kept)) {
            geometry << c.runs.at(a);
            mapped << qMakePair(metalCell(ends.at(a).first), metalCell(ends.at(a).second));
        }
        for (QList<QList<Complex>>& atFrequency : out.currents)
            for (QList<Complex>& column : atFrequency) {
                QList<Complex> folded;
                folded.reserve(kept.size());
                for (const int a : std::as_const(kept)) {
                    Complex i = column.at(a);
                    const int t = a < c.planar ? c.twin.value(a, -1) : -1;
                    if (t >= 0) i += (ends.at(t).first == thick.topOf.value(ends.at(a).first) ? 1.0 : -1.0) * column.at(t);
                    folded << i;
                }
                column = folded;
            }
        QHash<int, int> viaCells;
        for (auto it = out.viaCells.cbegin(); it != out.viaCells.cend(); ++it)
            if (it.key() < thick.cells && keptAt.contains(it.value())) viaCells.insert(it.key(), keptAt.value(it.value()));
        out.viaCells = viaCells;
        out.geometry = geometry;
        out.ends = mapped;
    }
    out.lines << tr("The Green's functions of %1 pair(s) of heights, tabulated to %2 mm at each frequency.")
                     .arg(c.nh * (c.nh + 1) / 2).arg(c.rhoMax * 1e3, 0, 'g', 3);
    return out;
}

QStringList returnSharesOf(const FullWave& solved)
{
    QStringList out;
    if (solved.frequencies.isEmpty()) return out;
    const double top = *std::max_element(solved.frequencies.begin(), solved.frequencies.end());
    for (int p = 0; p < solved.returnsOf.size() && p < solved.portNames.size(); ++p) {
        if (solved.returnsOf.at(p).size() < 2) continue;
        QStringList each;
        for (const PortReturn& r : solved.returnsOf.at(p)) {
            const double degrees = std::arg(r.share) * 180 / M_PI;
            each << (std::abs(degrees) >= 0.05 ? tr("%1 % through %2 (at %3°)").arg(100 * std::abs(r.share), 0, 'f', 1).arg(r.name).arg(degrees, 0, 'f', 1)
                                               : tr("%1 % through %2").arg(100 * std::abs(r.share), 0, 'f', 1).arg(r.name));
        }
        out << tr("At %1 GHz %2's current comes back %3.").arg(top / 1e9, 0, 'g', 4).arg(solved.portNames.at(p), each.join(QStringLiteral(", ")));
    }
    return out;
}

QList<bool> keptCurrentsOf(int count, double perFrequency, int top)
{
    QList<bool> out(std::max(0, count), true);
    if (count <= 1 || !(perFrequency > 0)) return out;
    const double fit = std::floor(kKeptCurrentsBytes / perFrequency);
    if (fit >= count) return out;
    out.fill(false);
    const int many = int(std::max(1.0, fit));
    if (many == 1) {
        out[std::clamp(top, 0, count - 1)] = true;
        return out;
    }
    for (int j = 0; j < many; ++j) out[int(std::lround(double(j) * (count - 1) / (many - 1)))] = true;
    if (top >= 0 && top < count) out[top] = true;
    return out;
}

QHash<int, double> currentDensity(const FullWave& solved, int k, int port, bool terminated)
{
    QHash<int, double> out;
    if (k < 0 || k >= solved.currents.size() || port < 0 || port >= solved.currents.at(k).size()) return out;
    // The ports' currents: an ampere into this one; or, ended, what a source
    // of its own reference r sends - an EMF of 2 sqrt(r), a wave of a watt -
    // with each of the others ended in its own: (Z + R) i = 2 sqrt(r) e. At
    // the ports as solved, which is what the currents are of, de-embedded
    // or not.
    const QList<QList<Complex>>& each = solved.currents.at(k);
    const QList<QList<QList<Complex>>>& zAt = solved.zSolved.isEmpty() ? solved.z : solved.zSolved;
    const int np = int(each.size());
    QList<Complex> drive(np, Complex(0, 0));
    if (!terminated) {
        drive[port] = 1.0;
    } else {
        if (k >= zAt.size() || solved.references.size() != np) return out;
        MatrixC a = matrixOf(zAt.at(k));
        Eigen::VectorXcd e = Eigen::VectorXcd::Zero(np);
        for (int q = 0; q < np; ++q) a(q, q) += solved.references.at(q);
        e(port) = 2 * std::sqrt(solved.references.at(port));
        const Eigen::VectorXcd i = Eigen::PartialPivLU<MatrixC>(a).solve(e);
        for (int q = 0; q < np; ++q) drive[q] = i(q);
    }
    QList<Complex> current(each.first().size(), Complex(0, 0));
    for (int q = 0; q < np; ++q)
        for (int r = 0; r < current.size() && r < each.at(q).size(); ++r) current[r] += each.at(q).at(r) * drive.at(q);
    // Along each direction, the mean of the runs a cell has that way -
    // its current per metre of the edge it crosses.
    struct Sum {
        Complex x, y;
        double nx = 0, ny = 0;
    };
    QHash<int, Sum> sums;
    const int runs = int(std::min({solved.geometry.size(), solved.ends.size(), current.size()}));
    for (int r = 0; r < runs; ++r) {
        const Filament& f = solved.geometry.at(r);
        // (A via's current runs across no cell.)
        if (!(f.width > 0) || f.z0 != f.z1) continue;
        double dx = 0, dy = 0;
        f.direction(&dx, &dy, nullptr);
        const Complex j = current.at(r) / f.width;
        for (const int cell : {solved.ends.at(r).first, solved.ends.at(r).second}) {
            if (cell < 0) continue;
            Sum& s = sums[cell];
            s.x += j * dx;
            s.nx += std::abs(dx);
            s.y += j * dy;
            s.ny += std::abs(dy);
        }
    }
    for (auto it = sums.cbegin(); it != sums.cend(); ++it) {
        const Complex jx = it->nx > 0 ? it->x / it->nx : Complex(0, 0);
        const Complex jy = it->ny > 0 ? it->y / it->ny : Complex(0, 0);
        out.insert(it.key(), std::sqrt(std::norm(jx) + std::norm(jy)));
    }
    // A via's own cells: its current round its four sides, per metre.
    for (auto it = solved.viaCells.cbegin(); it != solved.viaCells.cend(); ++it) {
        const int r = it.value();
        if (r < 0 || r >= runs || !(solved.geometry.at(r).width > 0)) continue;
        out.insert(it.key(), std::abs(current.at(r)) / (4 * solved.geometry.at(r).width));
    }
    return out;
}

namespace {

/// A Touchstone file's head and data: \a comments, the ports, then
/// \a keywords (the option line among them) and each frequency's \a s, one
/// port's S11 S21 S12 S22 in that order, more ports' by rows, four pairs
/// to a line - which Touchstone 1.1 asks and 2.0 allows.
QString touchstoneText(const FullWave& solved, const QList<QList<QList<Complex>>>& s, const QStringList& comments,
                       const QString& keywords, const QString& end)
{
    const int np = int(solved.portNames.size());
    QString out;
    for (const QString& comment : comments)
        for (const QString& line : comment.split(QLatin1Char('\n'))) out += QStringLiteral("! ") + line + QLatin1Char('\n');
    QStringList ports;
    for (int p = 0; p < np; ++p) ports << QStringLiteral("%1 %2").arg(p + 1).arg(solved.portNames.at(p));
    out += QStringLiteral("! Ports: %1\n").arg(ports.join(QStringLiteral(", ")));
    out += np == 2 ? QStringLiteral("! f  S11 (re, im)  S21  S12  S22\n")
                   : QStringLiteral("! f, then the S-matrix by rows (re, im), four to a line\n");
    out += keywords;
    const auto pair = [](Complex v) { return QLatin1Char(' ') + num(v.real()) + QLatin1Char(' ') + num(v.imag()); };
    for (int k = 0; k < solved.frequencies.size(); ++k) {
        const QList<QList<Complex>>& m = s.at(k);
        QString line = num(solved.frequencies.at(k));
        if (np <= 2) {
            // One port: S11; two: S11 S21 S12 S22, Touchstone's own order.
            if (np == 1) line += pair(m[0][0]);
            else line += pair(m[0][0]) + pair(m[1][0]) + pair(m[0][1]) + pair(m[1][1]);
            out += line + QLatin1Char('\n');
            continue;
        }
        for (int i = 0; i < np; ++i) {
            if (i > 0) line.clear();
            for (int j = 0; j < np; ++j) {
                if (j > 0 && j % 4 == 0) {
                    out += line + QLatin1Char('\n');
                    line.clear();
                }
                line += pair(m[i][j]);
            }
            out += line + QLatin1Char('\n');
        }
    }
    return out + end;
}

} // namespace

QString touchstoneOf(const FullWave& solved, double reference, const QStringList& comments)
{
    if (!solved.ok() || !(reference > 0)) return {};
    QList<QList<QList<Complex>>> s;
    for (const QList<QList<Complex>>& z : solved.z) s << scatteringOf(z, reference);
    return touchstoneText(solved, s, comments, QStringLiteral("# Hz S RI R %1\n").arg(num(reference)), QString());
}

QString touchstone2Of(const FullWave& solved, const QStringList& comments, bool mixed)
{
    if (!solved.ok() || (mixed && solved.pairs.isEmpty())) return {};
    const int np = int(solved.portNames.size());
    QStringList references;
    for (const double r : solved.references) references << num(r);
    // (The option line's R a default the [Reference] keyword then gives each
    //  port its own of; a two-port's order said, as 2.0 asks.)
    QString keywords = QStringLiteral("[Version] 2.0\n# Hz S RI R %1\n[Number of Ports] %2\n")
                           .arg(num(solved.references.value(0, 50)))
                           .arg(np);
    if (np == 2) keywords += QStringLiteral("[Two-Port Data Order] 21_12\n");
    keywords += QStringLiteral("[Number of Frequencies] %1\n[Reference] %2\n")
                    .arg(solved.frequencies.size())
                    .arg(references.join(QLatin1Char(' ')));
    if (!mixed) return touchstoneText(solved, solved.s, comments, keywords + QStringLiteral("[Network Data]\n"), QStringLiteral("[End]\n"));
    // Mixed: the modes in the order [Mixed-Mode Order] says - D2,3 the
    // differential mode of ports 2 and 3, 2 its plus; C2,3 their common
    // mode; S4 port 4 as it is - each pair's ports of one reference, which
    // its differential mode has twice of and its common mode half.
    QList<QList<QList<Complex>>> modes;
    QStringList order, of;
    for (int k = 0; k < solved.frequencies.size(); ++k) modes << mixedModeOf(solved, k, &order, &of);
    QStringList entries;
    QList<bool> paired(np, false);
    for (const QChar mode : {QLatin1Char('D'), QLatin1Char('C')})
        for (const QPair<int, int>& pair : solved.pairs) {
            entries << QStringLiteral("%1%2,%3").arg(mode).arg(pair.first + 1).arg(pair.second + 1);
            paired[pair.first] = paired[pair.second] = true;
        }
    for (int p = 0; p < np; ++p)
        if (!paired.at(p)) entries << QStringLiteral("S%1").arg(p + 1);
    QStringList said = comments;
    QStringList named;
    for (int m = 0; m < order.size(); ++m) named << QStringLiteral("%1 %2").arg(entries.value(m), of.value(m));
    said << tr("Mixed-mode S: %1; each differential mode into twice its ports' reference, each common mode into half")
                .arg(named.join(QStringLiteral(", ")));
    keywords += QStringLiteral("[Mixed-Mode Order] %1\n[Network Data]\n").arg(entries.join(QLatin1Char(' ')));
    return touchstoneText(solved, modes, said, keywords, QStringLiteral("[End]\n"));
}

QList<QList<Complex>> mixedModeOf(const FullWave& solved, int k, QStringList* order, QStringList* of)
{
    if (order != nullptr) order->clear();
    if (of != nullptr) of->clear();
    const int np = int(solved.portNames.size());
    if (solved.pairs.isEmpty() || k < 0 || k >= solved.s.size()) return {};
    // T, a row a mode and orthogonal: each pair's differential mode, then
    // each pair's common mode, then each port of no pair. S's waves are
    // power waves into each port's own reference, a pair's two alike, so
    // that the modes' are T's of them, into 2 r and r / 2: T S T'.
    const double half = 1 / std::sqrt(2.0);
    MatrixC t = MatrixC::Zero(np, np);
    QList<bool> paired(np, false);
    QStringList names, what;
    int row = 0;
    for (const double sign : {-1.0, 1.0})
        for (int i = 0; i < solved.pairs.size(); ++i) {
            const int plus = solved.pairs.at(i).first, minus = solved.pairs.at(i).second;
            t(row, plus) = half;
            t(row, minus) = sign * half;
            ++row;
            paired[plus] = paired[minus] = true;
            names << QStringLiteral("%1%2").arg(sign < 0 ? QLatin1Char('D') : QLatin1Char('C')).arg(i + 1);
            what << QStringLiteral("%1 %2 %3").arg(solved.portNames.at(plus), sign < 0 ? QStringLiteral("-") : QStringLiteral("+"),
                                                   solved.portNames.at(minus));
        }
    for (int p = 0; p < np; ++p)
        if (!paired.at(p)) {
            t(row++, p) = 1;
            names << solved.portNames.at(p);
            what << solved.portNames.at(p);
        }
    if (order != nullptr) *order = names;
    if (of != nullptr) *of = what;
    return rowsOf(t * matrixOf(solved.s.at(k)) * t.transpose());
}

} // namespace qucs_s::rf25
