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
/// (180 to 200), S the same to 1e-3 dB (docs/feature_gaps/
/// 2026-11-04-rf25-settings.md). RF 2.5D > Settings… sets another.
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
constexpr int kMostCompressed = 2000000;
/// GMRES short of kResidual: the residual below which its answer still
/// stands, said doubtful (solveCompressed()); and the share of
/// kMostIterations past which it is said to be near its limit.
constexpr double kTrustedResidual = 1e-4;
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

/// (exp(-j k r) - 1)/r: what the retardation adds to a static 1/r, finite
/// at nought (-j k there) and smooth, so it is taken at the centres.
Complex retardation(Complex k, double r)
{
    const Complex x = -kJ * k * r;
    if (std::abs(x) < 1e-4) return -kJ * k * (1.0 + x / 2.0 + x * x / 6.0);
    return (std::exp(x) - 1.0) / r;
}

/// The stack's scalar remainder averaged over \a rect (m) at \a at, by
/// Gauss's rule with as many points as a remainder changing over \a scale
/// wants.
Complex remainderOver(const WaveGreen& g, const QRectF& rect, const QPointF& at, double scale)
{
    const Rule& rx = gaussRule(pointsAlong(rect.width(), scale));
    const Rule& ry = gaussRule(pointsAlong(rect.height(), scale));
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
            const double tol = 1e-6 * std::max(1.0, std::max(outline.boundingRect().width(), outline.boundingRect().height()));
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
    const double tol = 1e-6 * std::max(1.0, std::max(all.width(), all.height()));
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
/// feature_gaps/2026-11-04-rf25-settings.md).
constexpr double kSweepMemory = 1.5e9;
std::atomic<int> denseLimitSet{0};
std::atomic<double> sweepMemorySet{0};

/// The cell of \a reached under \a where on \a layer (any, where it is
/// empty): the one whose outline holds the point, else the one whose
/// centre is nearest it. -1 where there is none on that layer.
int cellUnder(const Mesh& mesh, const QList<int>& reached, QPointF where, const QString& layer)
{
    int best = -1;
    double closest = 0;
    for (const int c : reached) {
        const MeshCell& cell = mesh.cells.at(c);
        if (!layer.isEmpty() && cell.layer != layer) continue;
        if (cell.outline.containsPoint(where, Qt::OddEvenFill)) return c;
        const QPointF d = cell.centre - where;
        const double far = d.x() * d.x() + d.y() * d.y();
        if (best < 0 || far < closest) {
            best = c;
            closest = far;
        }
    }
    return best;
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
 * \a said what was left out.
 */
QList<ViaRun> viaRunsOf(const Mesh& mesh, const Stack& stack, double micrometre, QList<ViaGreen::Span>* spans,
                        QStringList* said)
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
        double area = 0, sigma = 0, z0 = 0, z1 = 0;
        QPointF weighted;
        QRectF box;
        QList<int> cells;
    };
    QStringList names;   // (the stack's vias, in the order the mesh meets them)
    QMap<QString, Group> groups;
    QStringList keys;    // (the groups in the order they were met)
    int stranded = 0;
    for (int index = 0; index < mesh.cells.size(); ++index) {
        const MeshCell& cell = mesh.cells.at(index);
        const Layer* via = stack.named(cell.layer);
        if (via == nullptr || via->kind != Layer::Via) continue;
        const double za = metalHeight(stack, via->from), zb = metalHeight(stack, via->to);
        if (!(za >= 0) || !(zb >= 0) || za == zb) {
            ++stranded;
            continue;
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
        v.side = std::sqrt(g.area) * micrometre;
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
    if (stranded > 0)
        said->append(QCoreApplication::translate("qucs_s::rf25", "%1 via cell(s) land on no metal they join, and are left out.")
                         .arg(stranded));
    return out;
}

/// Everything a frequency's solve needs that does not change with the
/// frequency: made once, and read by every thread.
struct Context {
    Layers layers;
    QList<Filament> runs;                ///< in the plane, then the vias
    QList<int> tail, head;               ///< each run's cells, as charges (a via's tail -1 on the ground)
    QList<QRectF> rect;                  ///< each charge's cell, m
    QList<QPointF> centre;               ///< m
    QList<double> heights;               ///< the metals', m, from the lowest
    QList<int> hNode, hRun;              ///< which of them each charge and run in the plane is at
    QList<double> cellZ;                 ///< each charge's height, m
    QList<double> dirX, dirY, midX, midY;
    QList<Filament> mirrored;            ///< each run in the plane's image in the ground
    /// A dense solve's static parts, made once for every frequency and on
    /// every thread (makeStatics()): each pair of runs' aligned partial
    /// inductance (alignedInductance()), direct and of the image, and each
    /// cell's static potential of a charge spread over another. (A
    /// compressed solve makes the few it wants as it goes.)
    Eigen::MatrixXd lDirect, lImage, pStatic;
    QList<Via> vias;
    QList<ViaGreen::Span> spans;         ///< each of the stack's vias that the mesh has
    /// Each port's current into the charges (and out of an internal
    /// port's other metal): charges x columns. A port is one column, or a
    /// coplanar one a column against each conductor beside it.
    MatrixC drive;
    QList<int> portOf;                   ///< which port each column is
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
    /// edge, where the current crowds inside the cells (crowding()).
    QList<int> freeSides;
    /// Each run in the plane's own on its metal's other face, where the
    /// metal is solved as its two (thickened()); -1 on a sheet.
    QList<int> twin;
    /// The side walls' capacitances across narrow gaps (wallGapsOf()), F:
    /// runs of their own after the vias, from lumpedFrom on, each between two
    /// facing edge cells - no length, no inductance, its own impedance
    /// 1/jwC (-1: none).
    QList<double> lumped;
    int lumpedFrom = -1;
    double rhoMax = 0;
    int nr = 0, nc = 0, np = 0, nh = 0;
    int planar = 0;                      ///< the runs in the plane, which come first
};

/// What one frequency solved to.
struct AtFrequency {
    MatrixC z;                           ///< ports x ports
    MatrixC current;                     ///< runs x ports
    double rcond = 1;
    /// Dense: the seconds its fill took (the Green's functions, the system
    /// and P, up to its LU) and its LU (the factors, the solve, rcond), and
    /// whether Accelerate's LAPACK made them (rf25_dense.h).
    double fillSeconds = 0, luSeconds = 0;
    bool accelerated = false;
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
 * at 1 GHz lose a third less than it does (the recheck of 1 November).
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
 * (docs/feature_gaps/2026-11-04-claude-rf25-fullwave-gaps.md).
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
std::atomic<long long> sharedGreens{0}, sharedColumns{0}, sharedStatics{0};

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
    /// Via \a v's potential at cell \a cell of a unit current up it.
    Complex viaPotential(int v, int cell) const { return potentialOf(c.vias.at(v), cell); }
    /// G_zz: via \a i's voltage of a unit current on via \a j.
    Complex viaCoupling(int i, int j) const { return couplingOf(c.vias.at(i), c.vias.at(j)); }
    /// The same of any two vias - a port's path among them.
    Complex potentialOf(const Via& via, int cell) const;
    Complex couplingOf(const Via& a, const Via& b) const;
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
    const Complex l = 2.0 * d.coefficient / kMu0 * direct + 2.0 * i.coefficient / kMu0 * image;
    return l + c.runs.at(a).length() * c.runs.at(b).length() * smooth(c.hRun.at(a), c.hRun.at(b), rho);
}

Complex Kernels::smooth(int h0, int h1, double rho) const
{
    const WaveGreen& g = green(h0, h1);
    const WaveGreen::Term d = g.vectorTerm(), i = g.vectorImage();
    return d.coefficient / (2 * M_PI) * retardation(d.k, std::hypot(rho, d.dz))
           + i.coefficient / (2 * M_PI) * retardation(i.k, std::hypot(rho, i.dz)) + g.vectorRemainder(rho);
}

Complex Kernels::inductance(int a, int b) const
{
    return inductance(a, b, alignedInductance(c.runs.at(a), c.runs.at(b)), alignedInductance(c.runs.at(a), c.mirrored.at(b)));
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
    const auto oneWay = [this, &g, &t, rho, scale](int to, int from, double stat) {
        const Complex v = t.coefficient / (2 * M_PI) * (stat + retardation(t.k, std::hypot(rho, t.dz)));
        const double longest = std::max(c.rect.at(from).width(), c.rect.at(from).height());
        const bool centred = to != from && longest * longest < kCentreBound * scale * scale;
        return v + (centred ? g.scalarRemainder(rho) : remainderOver(g, c.rect.at(from), c.centre.at(to), scale));
    };
    return i == j ? oneWay(i, i, pij) : 0.5 * (oneWay(i, j, pij) + oneWay(j, i, pji));
}

Complex Kernels::potential(int i, int j) const
{
    const double dz = std::abs(c.cellZ.at(i) - c.cellZ.at(j));
    return potential(i, j, rectanglePotential(c.rect.at(j), c.centre.at(i), dz),
                     i == j ? 0.0 : rectanglePotential(c.rect.at(i), c.centre.at(j), dz));
}

Complex Kernels::potentialOf(const Via& via, int cell) const
{
    // Nowhere nearer the via than its cross-section's mean distance with
    // itself: the dielectric's part of its own G_zz, taken across that
    // distance, is what its potential is there (to a part in a thousand
    // through 200 µm of 9.8, against 5 % at its axis), and the two must
    // cancel round a path for its inductance to be the vacuum's, as it is.
    const double apart = std::hypot(c.centre.at(cell).x() - via.at.x(), c.centre.at(cell).y() - via.at.y());
    return viaGreens.at(via.span).potential(c.hNode.at(cell), std::max(apart, via.gmd));
}

Complex Kernels::couplingOf(const Via& a, const Via& b) const
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
template <typename Add> void ownImpedances(const Context& c, double f, Add add)
{
    for (int a = 0; a < c.planar; ++a) {
        const Filament& run = c.runs.at(a);
        if (!(run.width > 0)) continue;
        const Complex sheet = sheetImpedance(run.sigma, run.thickness, f);
        const double squares = run.length() / run.width;
        const int twin = c.twin.value(a, -1);
        const double crowd = crowding(run.width, run.thickness, run.sigma, c.freeSides.value(a), f, sheet, twin >= 0);
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
        if (v.side > 0 && run.sigma > 0) add(v.run, v.run, surfaceImpedance(run.sigma, v.side / 4, f) * run.length() / (4 * v.side));
    }
    // A wall's capacitance across its gap: 1/jwC.
    for (int k = 0; k < c.lumped.size(); ++k)
        add(c.lumpedFrom + k, c.lumpedFrom + k, 1.0 / (Complex(0, 2 * M_PI * f) * c.lumped.at(k)));
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
    onThreads(std::clamp(threads, 1, std::max(1, count)), [&next, &job, &counted, count](int t) {
        for (int i = next++; i < count; i = next++) {
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
    // A wall's capacitance: no field of its own - its own impedance and its
    // charges are the rest's (ownImpedances(), chargeColumn()).
    if (c.lumpedFrom >= 0 && b >= c.lumpedFrom) {
        for (int a = 0; a <= b; ++a) m(a, b) = Complex(0, 0);
        return;
    }
    if (b < np0) {
        // jw L: each two runs' cosine times their kernel - nothing between
        // runs at right angles, which is what makes an axis-aligned layout
        // cheap.
        for (int a = 0; a <= b; ++a) {
            const double dot = k.cosine(a, b);
            m(a, b) = std::abs(dot) < 1e-9 ? Complex(0, 0) : k.jw * (dot * k.inductance(a, b, c.lDirect(a, b), c.lImage(a, b)));
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
        m(a, b) += (pAt(c.tail.at(a), tail) - pAt(c.tail.at(a), head) - pAt(c.head.at(a), tail) + pAt(c.head.at(a), head)) / jw;
}

/// Column \a b of a dense solve's static partial inductances, direct and of
/// the images (Context::lDirect and lImage), its part above the diagonal and
/// on it: each run's with the runs before it, nought at right angles.
void staticInductanceColumn(const Context& c, Eigen::MatrixXd& direct, Eigen::MatrixXd& image, int b)
{
    for (int a = 0; a <= b; ++a) {
        const bool crossed = std::abs(c.dirX.at(a) * c.dirX.at(b) + c.dirY.at(a) * c.dirY.at(b)) < 1e-9;
        direct(a, b) = crossed ? 0.0 : alignedInductance(c.runs.at(a), c.runs.at(b));
        image(a, b) = crossed ? 0.0 : alignedInductance(c.runs.at(a), c.mirrored.at(b));
    }
}

/// Column \a j of a dense solve's static potentials (Context::pStatic),
/// whole: each cell's of a unit charge spread over cell j - not cell j's of
/// one spread over it, where the two differ in size.
void staticPotentialColumn(const Context& c, Eigen::MatrixXd& p, int j)
{
    for (int i = 0; i < c.nc; ++i)
        p(i, j) = rectanglePotential(c.rect.at(j), c.centre.at(i), std::abs(c.cellZ.at(i) - c.cellZ.at(j)));
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

AtFrequency solveAt(const Context& c, double f, int threads)
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
    sharedOut(nr, threads, [&c, &p, &m, jw](int b) { chargeColumn(c, p, jw, m, b); });
    ownImpedances(c, f, [&m](int a, int b, Complex v) { m(a, b) += v; });

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
    MatrixC rhs(nr, c.np);
    for (int a = 0; a < nr; ++a)
        for (int q = 0; q < c.np; ++q) rhs(a, q) = (driveAt(c.tail.at(a), q) - driveAt(c.head.at(a), q)) / jw;
    for (int j = 0; j < nv; ++j)
        for (int q = 0; q < c.np; ++q) rhs(c.vias.at(j).run, q) -= portVia(q, j);
    MatrixC pathZ = MatrixC::Zero(c.np, c.np);
    addPortPaths(c, k, rhs, pathZ);
    addSourceFields(c, k, rhs, pathZ);
    out.fillSeconds = double(clock.nsecsElapsed()) * 1e-9;
    // (m factorized where it is: it is not needed past here.)
    const DenseFactors<Complex> lu(std::move(m));
    out.current = lu.solve(rhs);
    out.rcond = lu.rcond();
    out.accelerated = lu.accelerated();
    out.luSeconds = double(clock.nsecsElapsed()) * 1e-9 - out.fillSeconds;
    if (!out.current.allFinite()) {
        out.problems << tr("The solve at %1 GHz came to nothing finite.").arg(f / 1e9, 0, 'g', 4);
        return out;
    }

    // Each port's voltage: its cells' potentials by the same shares, of its
    // own charge, J^T P J / jw, and of the charges the currents leave, with
    // what the vias' and the paths' currents add along its path - the
    // drive's coupling the other way round, by reciprocity.
    out.z = c.drive.transpose() * pDrive / jw + pathZ - rhs.transpose() * out.current;
    return out;
}

/*!
 * The pairs of \a a and \a b whose boxes are near: no further apart than
 * \a reach times the longer of the two's longest sides in the plane. Of
 * one list (\a b null), each pair once, the first no later than the
 * second. Found by bins in the plane as wide as the widest box.
 */
QList<QPair<int, int>> nearPairs(const QList<Box>& a, const QList<Box>* b, double reach)
{
    const QList<Box>& other = b != nullptr ? *b : a;
    QList<QPair<int, int>> out;
    if (a.isEmpty() || other.isEmpty()) return out;
    const auto size = [](const Box& x) { return std::max(x.side(0), x.side(1)); };
    double widest = 0, lo[2] = {a.first().lo[0], a.first().lo[1]};
    for (const QList<Box>* list : {&a, &other})
        for (const Box& x : *list) {
            widest = std::max(widest, size(x));
            lo[0] = std::min(lo[0], x.lo[0]);
            lo[1] = std::min(lo[1], x.lo[1]);
        }
    const double bin = widest > 0 ? widest : 1.0, grow = reach * widest;
    const auto binOf = [&lo, bin](double v, int axis) { return qint64(std::floor((v - lo[axis]) / bin)); };
    const auto key = [](qint64 i, qint64 j) { return (i << 32) ^ (j & 0xffffffffLL); };
    QHash<qint64, QList<int>> bins;
    for (int j = 0; j < other.size(); ++j) {
        const Box& x = other.at(j);
        for (qint64 i0 = binOf(x.lo[0], 0); i0 <= binOf(x.hi[0], 0); ++i0)
            for (qint64 i1 = binOf(x.lo[1], 1); i1 <= binOf(x.hi[1], 1); ++i1) bins[key(i0, i1)] << j;
    }
    std::vector<int> seen(std::size_t(other.size()), -1);
    for (int i = 0; i < a.size(); ++i) {
        const Box& x = a.at(i);
        for (qint64 i0 = binOf(x.lo[0] - grow, 0); i0 <= binOf(x.hi[0] + grow, 0); ++i0)
            for (qint64 i1 = binOf(x.lo[1] - grow, 1); i1 <= binOf(x.hi[1] + grow, 1); ++i1) {
                const auto found = bins.constFind(key(i0, i1));
                if (found == bins.constEnd()) continue;
                for (const int j : found.value()) {
                    if (seen[std::size_t(j)] == i) continue;
                    seen[std::size_t(j)] = i;
                    if (b == nullptr && j < i) continue;
                    if (x.distance(other.at(j)) <= reach * std::max(size(x), size(other.at(j)))) out << qMakePair(i, j);
                }
            }
    }
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
    VectorC q = VectorC::Zero(c.nc), v = VectorC::Zero(c.nc);
    for (int a = 0; a < c.nr; ++a) {
        if (c.tail.at(a) >= 0) q[c.tail.at(a)] += x[a];
        q[c.head.at(a)] -= x[a];
    }
    p.multiply(q, v);
    for (int a = 0; a < c.nr; ++a) y[a] += ((c.tail.at(a) >= 0 ? v[c.tail.at(a)] : Complex(0, 0)) - v[c.head.at(a)]) / jw;
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
 * preconditioned with, by its LU.
 */
Eigen::SparseMatrix<Complex> nearField(const Context& c, const Compression& z, const Kernels& k,
                                       const std::vector<Eigen::Triplet<Complex>>& own)
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
    for (const QPair<int, int>& q : z.nearCells) {
        const Complex v = k.potential(q.first, q.second);
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
    Eigen::SparseMatrix<Complex> charged = transposed * (pNear * incidence);
    charged /= jw;
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
    Eigen::SparseMatrix<Complex> m(nr, nr);
    m.setFromTriplets(near.begin(), near.end());
    m += charged;
    m.makeCompressed();
    return m;
}

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
AtFrequency solveCompressed(const Context& c, const Compression& z, double f, int threads, const FullWaveControl* control)
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
    Eigen::SparseMatrix<Complex> d(nr, nr);
    d.setFromTriplets(own.begin(), own.end());
    const System system{c, z, kx, ky, p, phi, g, d, jw};
    fem::SparseLu<Complex> lu;
    lu.compute(nearField(c, z, k, own));
    if (lu.info() != Eigen::Success) {
        out.problems << tr("At %1 GHz the near field's LU, which the compressed solve is preconditioned with, failed: %2")
                            .arg(f / 1e9, 0, 'g', 4)
                            .arg(QString::fromStdString(lu.lastErrorMessage()));
        return out;
    }
    const Operator precondition = [&lu](const VectorC& x, VectorC& y) { y = lu.solve(x); };

    // The drive: each column's potentials P J, and the vias' potential at
    // its cells.
    MatrixC pd(nc, columns), ph = MatrixC::Zero(nv, columns), rhs(nr, columns);
    for (int q = 0; q < columns; ++q) {
        const VectorC dq = c.drive.col(q);
        VectorC t = VectorC::Zero(nc);
        p.multiply(dq, t);
        pd.col(q) = t;
        if (nv == 0) continue;
        VectorC u = VectorC::Zero(nv);
        phi.multiplyTransposed(dq, u);
        ph.col(q) = u;
    }
    for (int a = 0; a < nr; ++a)
        for (int q = 0; q < columns; ++q)
            rhs(a, q) = ((c.tail.at(a) >= 0 ? pd(c.tail.at(a), q) : Complex(0, 0)) - pd(c.head.at(a), q)) / jw;
    for (int j = 0; j < nv; ++j)
        for (int q = 0; q < columns; ++q) rhs(c.vias.at(j).run, q) -= ph(j, q);
    MatrixC pathZ = MatrixC::Zero(columns, columns);
    addPortPaths(c, k, rhs, pathZ);
    addSourceFields(c, k, rhs, pathZ);
    MatrixC current(nr, columns), applied(nr, columns);
    for (int q = 0; q < columns; ++q) {
        VectorC x = VectorC::Zero(nr), mx;
        const Gmres solved = gmres(system, precondition, rhs.col(q), x, kResidual, kRestart, most, stop, progress);
        if (solved.stopped) return AtFrequency();
        out.iterations = std::max(out.iterations, solved.iterations);
        out.residual = std::max(out.residual, solved.residual);
        current.col(q) = x;
        system(x, mx);
        applied.col(q) = mx;
    }
    // Short of its residual: where what it left is small - the ports' Z is
    // variational, its error the residual's square over how near singular
    // the system is - the answer stands, said doubtful, and the sweep goes
    // on; past kTrustedResidual, or not finite, it is no answer. A sweep
    // stopped at the first frequency that fell short, with nothing of the
    // rest (B3 of the open items of 8 November).
    if (out.residual > kTrustedResidual || !current.allFinite()) {
        out.problems << tr("At %1 GHz the compressed solve did not converge: GMRES left a residual of %2 after %3 "
                           "iterations. Solve it dense (the study's Matrix), or mesh it differently.")
                            .arg(f / 1e9, 0, 'g', 4)
                            .arg(out.residual, 0, 'g', 2)
                            .arg(out.iterations);
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
    out.z = c.drive.transpose() * pd / jw + pathZ
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
        if (!coplanar) return parts.join(QLatin1Char('|'));
        const int a = mirror ? 1 : 0, b = 1 - a;
        parts << QStringLiteral("coplanar") << n(gap[a]) << n(ground[a]) << n(gap[b]) << n(ground[b]) << n(pitch)
              << unlike;
        QStringList each;
        for (const Via& v : vias)
            each << QStringList{v.layer, n(mirror ? width - v.across1 : v.across0), n(mirror ? width - v.across0 : v.across1),
                                n(v.in0), n(v.in1), v.wall ? QStringLiteral("wall") : QString()}
                        .join(QLatin1Char(','));
        each.sort();
        return (parts + each).join(QLatin1Char('|'));
    };
    return std::min(text(false), text(true));
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
    const double above = layers.epsrAbove;
    if (!layers.groundBelow && layers.epsrBelow > above + 1e-9) return 0;
    const double c0 = 1.0 / std::sqrt(kMu0 * kEps0);
    double rate = 0;   // d(sum of kz h)/df
    for (const Layers::Slab& slab : layers.slabs)
        rate += 2 * M_PI / c0 * std::sqrt(std::max(0.0, slab.epsr - above)) * (slab.z1 - slab.z0);
    if (!(rate > 0)) return 0;
    const bool grounded = layers.groundBelow;
    const auto resonance = [&layers, above, grounded, c0](double f) {
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
        return grounded ? a : c;
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

FullWave fullWaveOf(const Mesh& given, const QList<Conductor>& givenConductors, const Stack& givenStack,
                    const QList<FullWavePort>& ports, const QList<double>& frequencies, double micrometre,
                    const FullWaveControl* control, const QList<ReturnMark>& returns)
{
    FullWave out;
    // (What makes the mesh's answer doubtful - touching conductors it has
    //  apart - is the solve's too.)
    out.warnings << given.warnings;
    // A metal solved as its two faces is two sheets and the walls that join
    // them, which is what the rest solves (thickened()); what is handed
    // back is of the mesh as given.
    const Thickened thick = thickened(givenStack, givenConductors, given);
    const Mesh& mesh = thick.mesh;
    const QList<Conductor>& conductors = thick.conductors;
    const Stack& stack = thick.stack;
    if (mesh.cells.isEmpty()) {
        out.problems << tr("There are no cells to solve on: mesh the layout first.");
        return out;
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
        if (port.wantsStrips()) {
            out.problems << tr("%1 returns through the terminals it names (%2), and its own discontinuity cannot be taken "
                               "out by strips of a feed: they are a line between two edge ports, and it is a source "
                               "between its own cells and its returns'. Take 'calibrate' off - its reference plane is then "
                               "at those cells - and de-embed after the solve (L-2L, TRL or open-short), or move its plane "
                               "along a given line ('along' line).")
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
    // Which of each run's two sides are its conductor's edge: a point just
    // beyond the side, at the run's middle, on no shape of its layer
    // (shapes that touch are one conductor there).
    c.freeSides = QList<int>(c.planar, 0);
    QHash<QString, QList<int>> shapesOn;   // (each layer's shapes: a via farm's are hundreds of other layers')
    for (int s = 0; s < conductors.size(); ++s) shapesOn[conductors.at(s).layer] << s;
    for (int a = 0; a < c.planar; ++a) {
        const Filament& run = c.runs.at(a);
        const QString& layer = mesh.cells.at(ends.at(a).first).layer;
        const QPointF d = run.to - run.from;
        const double length = std::hypot(d.x(), d.y());
        if (!(length > 0)) continue;
        const QPointF normal(-d.y() / length, d.x() / length), middle = (run.from + run.to) / (2 * micrometre);
        const double reach = 0.5005 * run.width / micrometre;
        for (const double side : {-1.0, 1.0}) {
            const QPointF probe = middle + side * reach * normal;
            bool inside = false;
            for (const int s : shapesOn.value(layer)) {
                const Conductor& shape = conductors.at(s);
                if (shape.outline.boundingRect().contains(probe) && shape.outline.containsPoint(probe, Qt::OddEvenFill)) {
                    inside = true;
                    break;
                }
            }
            if (!inside) ++c.freeSides[a];
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
    QStringList viaSaid;
    const QList<ViaRun> viaRuns = viaRunsOf(mesh, stack, micrometre, &c.spans, &viaSaid);
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
    // The side walls' charge, which neither face of a metal solved as its
    // two holds: across each narrow gap a capacitance between the facing
    // edge cells, from a 2D solve of solid metal against its two faces
    // (wallGapsOf()) - a run of its own, no length and no inductance. Left
    // out, a MOM comb of 1 µm metal in 1 µm gaps read 19 % under Palace's
    // capacitance (B2 of the open items of 8 November).
    QString wallsSaid;
    {
        const QList<WallGap> gaps = wallGapsOf(thick, micrometre);
        if (!gaps.isEmpty()) {
            c.lumpedFrom = int(c.runs.size());
            double total = 0, narrowest = 0;
            for (const WallGap& g : gaps) {
                Filament run;
                run.from = run.to = 0.5 * (mesh.cells.at(g.a).centre + mesh.cells.at(g.b).centre) * micrometre;
                c.lumped << g.capacitance;
                c.runs << run;
                ends << qMakePair(g.a, g.b);
                total += g.capacitance;
                if (narrowest <= 0 || g.gap < narrowest) narrowest = g.gap;
            }
            wallsSaid = tr("The side walls across their narrow gaps, as a %1 µm gap at its narrowest: %2 capacitance(s) "
                           "between facing edge cells, %3 fF in all - the field between the walls that their two faces "
                           "leave out, by a 2D solve of straight walls (corners and tips are taken as straight).")
                            .arg(narrowest / 1e-6, 0, 'g', 3).arg(gaps.size()).arg(total * 1e15, 0, 'g', 4);
        }
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
    const bool compress = asked == FullWaveControl::Matrix::Compressed
                          || (asked == FullWaveControl::Matrix::Automatic && nr + nc > most);
    if (!compress && nr + nc > most) {
        out.tooLarge = true;
        out.problems << tr("%1 runs of current over %2 cells is more than a dense complex matrix is set up for (%3 in "
                           "all). Mesh the layout more coarsely: fewer cells across a conductor, or a larger largest "
                           "cell - or hold it compressed (the study's Matrix), or raise the limit (RF 2.5D > "
                           "Settings…).")
                            .arg(nr).arg(nc).arg(most);
        return out;
    }
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
                //  no conductor solved: the returns check of 4 November.)
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
            // the source", and none of those could be modelled (B7 of the open
            // items of 8 November).
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
                        if (c.lumpedFrom >= 0 && r >= c.lumpedFrom) break;   // (a wall's capacitance is no join)
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
            // there are counted twice - said (it solved without a word: the
            // returns check of 4 November).
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
    QList<double> zOf(nc, -1);
    QRectF extent;   // (of every cell, for how far the Green's tables go)
    for (int i = 0; i < nc; ++i) {
        const MeshCell& cell = mesh.cells.at(cellOf.at(i));
        c.rect[i] = scaledBy(cell.outline.boundingRect(), micrometre);
        c.centre[i] = cell.centre * micrometre;
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
    for (int a = 0; a < nr; ++a) c.hRun[a] = a < c.planar ? heightIndex(c.runs.at(a).z0) : -1;
    // Each port's own path to the cells it drives (PortPath): up from the
    // ground, or from the cells of the metal an internal port is driven
    // from, shared out between them as the port shares its current; its
    // span one more of the vias' where it is none of theirs. (None for a
    // port that names its returns, or a coplanar port; and none at all
    // where a test asks for none.)
    for (int k = 0; k < c.np && (control == nullptr || control->portPaths); ++k) {
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
        if (c.lumpedFrom >= 0 && a >= c.lumpedFrom) {
            // (A wall's capacitance has no direction: no kernel takes it.)
            c.dirX[a] = c.dirY[a] = 0;
            c.midX[a] = c.runs.at(a).from.x();
            c.midY[a] = c.runs.at(a).from.y();
            continue;
        }
        c.runs.at(a).direction(&c.dirX[a], &c.dirY[a], nullptr);
        c.runs.at(a).middle(&c.midX[a], &c.midY[a], nullptr);
        c.mirrored[a].z0 = c.mirrored[a].z1 = -c.runs.at(a).z0;
    }
    const int nf = int(frequencies.size());
    int threads = control != nullptr && control->threads > 0 ? control->threads
                                                              : int(std::max(1u, std::thread::hardware_concurrency()));
    // (Made in place, and pointed at by the matrices made from it.)
    Compression z;
    int side = 1;   // frequencies at a time
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
    }
    const int inner = std::max(1, threads / side);
    out.threads = side * inner;
    if (control != nullptr && control->atOnce != nullptr) control->atOnce->store(side);
    if (control != nullptr && control->begins) control->begins(nf);
    // (A vector, not a QList: the threads write to it side by side, and a
    //  QList's implicit sharing is no place for that.)
    std::vector<AtFrequency> solved(static_cast<std::size_t>(nf));
    std::atomic_int next{0};
    std::atomic_bool failed{false};
    const auto work = [&c, &z, &frequencies, &solved, &next, &failed, control, nf, compress, inner] {
        const std::atomic_bool* stop = control != nullptr ? control->stop : nullptr;
        for (int k = next++; k < nf; k = next++) {
            if (failed || (stop != nullptr && stop->load())) return;
            if (!allowedOne(control)) return;
            solved[std::size_t(k)] = compress ? solveCompressed(c, z, frequencies.at(k), inner, control)
                                                : solveAt(c, frequencies.at(k), inner);
            if (!solved[std::size_t(k)].problems.isEmpty()) failed = true;
            if (control != nullptr && control->done != nullptr) ++*control->done;
        }
    };
    QList<QThread*> pool;
    for (int t = 1; t < side; ++t) {
        QThread* thread = QThread::create(work);
        thread->setStackSize(8u << 20);
        thread->start();
        pool << thread;
    }
    work();
    for (QThread* thread : std::as_const(pool)) {
        thread->wait();
        delete thread;
    }
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
        }
    }
    out.lines << out.portsSaid;
    if (!naming.isEmpty())
        out.lines << tr("%1 port(s) return through the terminals and ports they name, each port's returns tied as a "
                        "probe's tips are: a source between its own cells and theirs, with no path up to them, so that "
                        "its reference plane is at those cells and the probe's own loop is not in the answer - its "
                        "current between them radiating, its loop's inductance left out.")
                         .arg(naming.size());
    out.lines << thick.said;
    if (!wallsSaid.isEmpty()) out.lines << wallsSaid;
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
    out.lines << viaSaid;
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
    else
        out.lines << (coplanar ? tr("The ground plane is taken as perfect, %1; an edge port's current as coming back "
                                    "through the ground, and a coplanar port's through the conductors beside it.")
                                     .arg(sheet)
                               : tr("The ground plane is taken as perfect, %1; an edge port's current as coming back "
                                    "through the ground.")
                                     .arg(sheet));
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
    for (int k = 0; k < nf; ++k) {
        AtFrequency a = solved[std::size_t(k)];
        if (c.np != np) {
            const MatrixC x = Eigen::PartialPivLU<MatrixC>(a.z).solve(ofPort);
            const MatrixC y = ofPort.transpose() * x;
            const MatrixC z = Eigen::PartialPivLU<MatrixC>(y).solve(MatrixC::Identity(np, np));
            if (!z.allFinite()) {
                out.problems << tr("At %1 GHz the returns of the ports that have more than one could not be tied: their "
                                   "columns came out singular.")
                                    .arg(frequencies.at(k) / 1e9, 0, 'g', 4);
                return out;
            }
            if (k == topAt) shareOut(x * z);
            a.current = a.current * (x * z);
            a.z = z;
        } else if (k == topAt) {
            shareOut(ofPort);
        }
        const QList<QList<Complex>> zRows = rowsOf(a.z);
        out.frequencies << frequencies.at(k);
        out.z << zRows;
        out.s << scatteringOf(zRows, out.references);
        QList<QList<Complex>> perPort;
        for (int q = 0; q < np; ++q) {
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
        if (a.rcond < 1e-14)
            out.lines << tr("At %1 GHz the system is nearly singular (a reciprocal condition of %2): the answer there may "
                            "be rounding.")
                             .arg(frequencies.at(k) / 1e9, 0, 'g', 4).arg(a.rcond, 0, 'g', 2);
    }
    out.solvedAsymmetry = out.asymmetry;
    out.solvedGain = out.gain;
    out.solvedGainAt = out.gainAt;
    out.lines << returnSharesOf(out);
    // Where the stack begins to carry a second surface wave - TE1 over a
    // ground - which a strip launches beside the TM0 of every frequency: the
    // top of a microstrip's band on it, said where the sweep passes it.
    // (Taken in the phase E check of 2 November for what gave power back
    // past it, which was the ports' own paths, then without a field:
    // addPortPaths().)
    const double top = *std::max_element(frequencies.begin(), frequencies.end());
    out.secondWave = secondSurfaceWave(givenStack, 2 * top);
    if (out.secondWave > 0 && top >= out.secondWave)
        out.lines << tr("From %1 GHz the stack carries a second surface wave, its TE1, which a strip launches beside the "
                        "TM0 of every frequency: the top of a microstrip's band on it, past which a line gives more of "
                        "what it carries to the substrate.")
                         .arg(out.secondWave / 1e9, 0, 'g', 4);
    out.geometry = c.runs;
    out.ends = ends;
    // As the caller meshed it: a top face's run in its bottom's - the
    // current through the whole metal there - the walls' runs left out, and
    // a via landing on a top face on its metal's cell (thickened()).
    if (thick.any) {
        QList<int> kept;
        QHash<int, int> keptAt;
        for (int a = 0; a < nr; ++a) {
            if ((a < c.planar && thick.bottomOf.contains(ends.at(a).second)) || walled.contains(a)
                || (c.lumpedFrom >= 0 && a >= c.lumpedFrom))
                continue;
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
