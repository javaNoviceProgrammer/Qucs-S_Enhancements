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

#include <QCoreApplication>
#include <QHash>
#include <QRectF>
#include <QThread>

#include <Eigen/Dense>

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
/// set up: the inductance's bound - some 200 MB (two static inductance
/// matrices, the system and its LU) and, measured at 2858, 0.8 s a
/// frequency after 0.4 s once, on a desktop of 2026.
constexpr int kMostUnknowns = 3000;
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
};

/// The faces beside an edge port's own on its edge's line - what a
/// ground-signal-ground probe's grounds touch: either side of \a face, the
/// nearest run of cells of \a reached on its layer with a side along that
/// line (through \a on, along \a along) and inside on the same side of it
/// as the port's own face. A side with none is empty.
Beside besideFaces(const Mesh& mesh, const QList<int>& reached, const QList<int>& face, QPointF on, QPointF along)
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
    for (const int c : reached) {
        if (mesh.cells.at(c).layer != layer || face.contains(c)) continue;
        double t0 = 0, t1 = 0;
        if (!sideOn(c, &t0, &t1)) continue;
        const QPointF d = mesh.cells.at(c).centre - on;
        if ((d.x() * normal.x() + d.y() * normal.y()) * inside <= 0) continue;
        if (t1 <= lo + tol) below << std::make_tuple(lo - t1, lo - t0, c);
        else if (t0 >= hi - tol) above << std::make_tuple(t0 - hi, t1 - hi, c);
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
            reach = std::max(reach, far);
        }
        out.far[std::size_t(k)] = reach;
        ++k;
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

/// How much memory the frequencies solved side by side may take, bytes.
constexpr double kSweepMemory = 1.5e9;

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
    // own size, does; -1 where the via is over none of that metal.
    const auto under = [&mesh](const QString& metal, const MeshCell& v) {
        int best = -1;
        double closest = 0;
        const QRectF box = v.outline.boundingRect();
        const double grow = std::hypot(box.width(), box.height()) / 2;
        for (int c = 0; c < mesh.cells.size(); ++c) {
            const MeshCell& cell = mesh.cells.at(c);
            if (cell.layer != metal) continue;
            if (cell.outline.containsPoint(v.centre, Qt::OddEvenFill)) return c;
            if (!cell.outline.boundingRect().adjusted(-grow, -grow, grow, grow).contains(v.centre)) continue;
            const QPointF d = cell.centre - v.centre;
            const double far = d.x() * d.x() + d.y() * d.y();
            if (best < 0 || far < closest) {
                best = c;
                closest = far;
            }
        }
        return best;
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
    QList<double> dirX, dirY, midX, midY;
    Eigen::MatrixXd lDirect, lImage, pStatic;
    QList<Via> vias;
    QList<ViaGreen::Span> spans;         ///< each of the stack's vias that the mesh has
    /// Each port's current into the charges (and out of an internal
    /// port's other metal): charges x columns. A port is one column, or a
    /// coplanar one a column against each conductor beside it.
    MatrixC drive;
    QList<int> portOf;                   ///< which port each column is
    /// How many of each run in the plane's two sides are its conductor's
    /// edge, where the current crowds inside the cells (crowding()).
    QList<int> freeSides;
    double rhoMax = 0;
    int nr = 0, nc = 0, np = 0, nh = 0;
    int planar = 0;                      ///< the runs in the plane, which come first
};

/// What one frequency solved to.
struct AtFrequency {
    MatrixC z;                           ///< ports x ports
    MatrixC current;                     ///< runs x ports
    double rcond = 1;
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
 * Radio Sci. 29 (1994) 539; it is the t in Ghione's coplanar loss) - where
 * the metal is thick beside its skin depth. Where it is not, its \a sheet
 * resistance beyond a thick metal's two faces' spreads the current over
 * Pearl's length 2 R/(w mu0) as well: a micrometre of copper at 2 GHz,
 * a micrometre and a half, past which an edge cell of 12.5 holds little
 * crowding; at direct current, all of it, and the factor is one. Never
 * below one.
 */
double crowding(double width, double thickness, double sigma, int free, double frequency, Complex sheet)
{
    if (free <= 0 || !(width > 0) || !(thickness > 0) || !(sigma > 0) || !(frequency > 0)) return 1;
    constexpr double mu0 = 4e-7 * M_PI;
    const double omega = 2 * M_PI * frequency;
    const double stop = thickness / (4 * M_PI * std::exp(M_PI));
    const double faces = std::sqrt(omega * mu0 / (2 * sigma)) / 2;   // a thick metal's sheet: Rs/2
    const double spread = 2 * std::max(0.0, sheet.real() - faces) / (omega * mu0);
    const double log = std::log(1 + width / (stop + spread));
    return std::max(1.0, free >= 2 ? 2 / (M_PI * M_PI) * log : log / 4);
}

AtFrequency solveAt(const Context& c, double f)
{
    AtFrequency out;
    const double w = 2 * M_PI * f;
    const Complex jw(0, w);
    const int nr = c.nr, nc = c.nc, nh = c.nh;

    // The retarded Green's functions of each pair of heights.
    QList<WaveGreen> greens;
    QList<int> pairOf(nh * nh, -1);
    for (int i = 0; i < nh; ++i)
        for (int j = i; j < nh; ++j) {
            WaveGreen g(c.layers, c.heights.at(i), c.heights.at(j), f, c.rhoMax);
            if (!g.ok()) {
                out.problems << g.problems();
                return out;
            }
            g.prepare();
            pairOf[i * nh + j] = pairOf[j * nh + i] = int(greens.size());
            greens << g;
        }

    // A via's kernels, one for each of the stack's vias the mesh has: its
    // coupling to every via, and its potential at every metal height.
    QList<ViaGreen> viaGreens;
    for (const ViaGreen::Span& s : c.spans) {
        ViaGreen v(c.layers, s, c.spans, c.heights, f, c.rhoMax);
        if (!v.ok()) {
            out.problems << v.problems();
            return out;
        }
        v.prepare();
        viaGreens << v;
    }

    // jw L: the static partial inductances scaled as the vector
    // potential's singular terms are, and the rest at the centres - the
    // retardation of the source and of its image, and the stack's own
    // remainder.
    MatrixC m = MatrixC::Zero(nr, nr);
    const int np0 = c.planar;
    for (int a = 0; a < np0; ++a)
        for (int b = a; b < np0; ++b) {
            const double dot = c.dirX.at(a) * c.dirX.at(b) + c.dirY.at(a) * c.dirY.at(b);
            Complex l(0, 0);
            if (std::abs(dot) >= 1e-9) {
                const WaveGreen& g = greens.at(pairOf.at(c.hRun.at(a) * nh + c.hRun.at(b)));
                const WaveGreen::Term direct = g.vectorTerm(), image = g.vectorImage();
                const double rho = std::hypot(c.midX.at(a) - c.midX.at(b), c.midY.at(a) - c.midY.at(b));
                l = 2.0 * direct.coefficient / kMu0 * c.lDirect(a, b) + 2.0 * image.coefficient / kMu0 * c.lImage(a, b);
                const Complex smooth = direct.coefficient / (2 * M_PI) * retardation(direct.k, std::hypot(rho, direct.dz))
                                       + image.coefficient / (2 * M_PI) * retardation(image.k, std::hypot(rho, image.dz))
                                       + g.vectorRemainder(rho);
                l += dot * c.runs.at(a).length() * c.runs.at(b).length() * smooth;
            }
            m(a, b) = m(b, a) = jw * l;
        }

    // The vias. Between two: G_zz along both - the closed forms between
    // their axes, statically in closed form and their retardation by
    // Gauss's rule, and the table's rest - and the scalar correction of the
    // one's current at the other's two cells, ended by the ground's nought
    // for a via to it. Its potential is taken at the cells, as the node
    // potentials are, so that what a via's current adds along the runs that
    // carry it away cancels what it adds along the via itself; and the two
    // ways round averaged, which differ by where a via stands in its cell.
    // Nowhere nearer the via than its cross-section's mean distance with
    // itself: the dielectric's part of its own G_zz, taken across that
    // distance, is what its potential is there (to a part in a thousand
    // through 200 µm of 9.8, against 5 % at its axis), and the two must
    // cancel round a path for its inductance to be the vacuum's, as it is.
    const int nv = int(c.vias.size());
    const auto distance = [](QPointF p, QPointF q) { return std::hypot(p.x() - q.x(), p.y() - q.y()); };
    const auto potentialOf = [&c, &viaGreens, &distance](const Via& v, int cell) {
        return viaGreens.at(v.span).potential(c.hNode.at(cell), std::max(distance(c.centre.at(cell), v.at), v.gmd));
    };
    MatrixC block(nv, nv);
    for (int i = 0; i < nv; ++i)
        for (int j = 0; j < nv; ++j) {
            const Via& a = c.vias.at(i);
            const Via& b = c.vias.at(j);
            const ViaGreen& green = viaGreens.at(a.span);
            const double apart = distance(a.at, b.at);
            const double gmd = std::max(a.gmd, b.gmd);
            Complex v(0, 0);
            for (const ViaGreen::Term& t : green.couplingTerms(b.span)) {
                if (t.coefficient == Complex(0, 0)) continue;
                const double near = viaPair(t.a0, t.a1, t.b0, t.b1, apart, a.side, b.side, gmd);
                const Complex late = retardedPair(t.k, t.a0, t.a1, t.b0, t.b1, std::max(apart, gmd));
                v += t.coefficient * (near + late) / (2 * M_PI);
            }
            v += green.couplingRemainder(b.span, apart);
            const int head = c.head.at(a.run), tail = c.tail.at(a.run);
            v += potentialOf(b, head);
            if (tail >= 0) v -= potentialOf(b, tail);
            block(i, j) = v;
        }
    for (int i = 0; i < nv; ++i)
        for (int j = i; j < nv; ++j)
            m(c.vias.at(i).run, c.vias.at(j).run) = m(c.vias.at(j).run, c.vias.at(i).run) = 0.5 * (block(i, j) + block(j, i));
    // A run in the plane and a via: the via's potential at the run's head
    // less at its tail - which is also what the run's vector potential does
    // along the via (G_zx), by the formulation's reciprocity, both ways.
    for (int a = 0; a < np0; ++a)
        for (int j = 0; j < nv; ++j) {
            const Via& b = c.vias.at(j);
            const Complex v = potentialOf(b, c.head.at(a)) - potentialOf(b, c.tail.at(a));
            m(a, b.run) = m(b.run, a) = v;
        }
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
                for (int j = 0; j < nv; ++j) portVia(q, j) += c.drive(i, q) * potentialOf(c.vias.at(j), i);

    // P: each cell's potential of a unit charge spread over another - the
    // static singular part over the cell in closed form, its retardation
    // at the centres, and the remainder integrated over the charged cell,
    // or at its centre where the cell is small beside the distance the
    // remainder changes over. Made symmetric: a potential at a centre of a
    // charge over a cell is not quite the other way round where the cells
    // differ in size, and the port matrix is reciprocal only if P is.
    MatrixC p(nc, nc);
    const auto coefficient = [&c](int i, int j, const WaveGreen& g) {
        const WaveGreen::Term t = g.scalarTerm();
        const double rho = std::hypot(c.centre.at(i).x() - c.centre.at(j).x(), c.centre.at(i).y() - c.centre.at(j).y());
        const Complex v = t.coefficient / (2 * M_PI) * (c.pStatic(i, j) + retardation(t.k, std::hypot(rho, t.dz)));
        const double longest = std::max(c.rect.at(j).width(), c.rect.at(j).height());
        const double scale = std::hypot(rho, g.reach());
        const bool centred = i != j && longest * longest < kCentreBound * scale * scale;
        return v + (centred ? g.scalarRemainder(rho) : remainderOver(g, c.rect.at(j), c.centre.at(i), scale));
    };
    for (int i = 0; i < nc; ++i)
        for (int j = i; j < nc; ++j) {
            const WaveGreen& g = greens.at(pairOf.at(c.hNode.at(i) * nh + c.hNode.at(j)));
            const Complex v = i == j ? coefficient(i, i, g) : 0.5 * (coefficient(i, j, g) + coefficient(j, i, g));
            p(i, j) = p(j, i) = v;
        }

    // The charge eliminated: M = Zs l/w + jw L + A^T P A / jw. (A via to the
    // ground has no tail: the ground takes its charge, at nought.)
    const auto pAt = [&p](int i, int j) { return i < 0 || j < 0 ? Complex(0, 0) : p(i, j); };
    for (int a = 0; a < nr; ++a)
        for (int b = 0; b < nr; ++b)
            m(a, b) += (pAt(c.tail.at(a), c.tail.at(b)) - pAt(c.tail.at(a), c.head.at(b)) - pAt(c.head.at(a), c.tail.at(b))
                        + pAt(c.head.at(a), c.head.at(b)))
                       / jw;
    for (int a = 0; a < np0; ++a) {
        const Filament& run = c.runs.at(a);
        if (!(run.width > 0)) continue;
        const Complex sheet = sheetImpedance(run.sigma, run.thickness, f);
        m(a, a) += crowding(run.width, run.thickness, run.sigma, c.freeSides.value(a), f, sheet) * sheet * run.length() / run.width;
    }
    // A via's own: a bar whose current runs on its four faces, each with the
    // surface impedance of a quarter of its depth - 1/(sigma area) at direct
    // current, (1 + j)/(sigma delta) round its perimeter once the skin is
    // thinner.
    for (const Via& v : c.vias) {
        const Filament& run = c.runs.at(v.run);
        if (v.side > 0 && run.sigma > 0) m(v.run, v.run) += surfaceImpedance(run.sigma, v.side / 4, f) * run.length() / (4 * v.side);
    }

    // Each port driven by an ampere in turn: A^T P J / jw the drive.
    const MatrixC pDrive = p * c.drive;
    const auto driveAt = [&pDrive](int i, int q) { return i < 0 ? Complex(0, 0) : pDrive(i, q); };
    MatrixC rhs(nr, c.np);
    for (int a = 0; a < nr; ++a)
        for (int q = 0; q < c.np; ++q) rhs(a, q) = (driveAt(c.tail.at(a), q) - driveAt(c.head.at(a), q)) / jw;
    for (int j = 0; j < nv; ++j)
        for (int q = 0; q < c.np; ++q) rhs(c.vias.at(j).run, q) -= portVia(q, j);
    const Eigen::PartialPivLU<MatrixC> lu(m);
    out.current = lu.solve(rhs);
    out.rcond = lu.rcond();
    if (!out.current.allFinite()) {
        out.problems << tr("The solve at %1 GHz came to nothing finite.").arg(f / 1e9, 0, 'g', 4);
        return out;
    }

    // The charges it leaves, (J - A I)/jw; their potentials; and each
    // port's voltage, its cells' potentials by the same shares.
    MatrixC charge = c.drive;
    for (int a = 0; a < nr; ++a)
        for (int q = 0; q < c.np; ++q) {
            if (c.tail.at(a) >= 0) charge(c.tail.at(a), q) -= out.current(a, q);
            charge(c.head.at(a), q) += out.current(a, q);
        }
    charge /= jw;
    out.z = c.drive.transpose() * (p * charge);
    for (int j = 0; j < nv; ++j)
        for (int q = 0; q < c.np; ++q)
            for (int r = 0; r < c.np; ++r) out.z(r, q) += portVia(r, j) * out.current(c.vias.at(j).run, q);
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

} // namespace

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

FullWave fullWaveOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack,
                    const QList<FullWavePort>& ports, const QList<double>& frequencies, double micrometre,
                    const FullWaveControl* control)
{
    FullWave out;
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
    if (!c.layers.groundBelow) {
        out.problems << tr("The full-wave solve refers its ports to a ground plane, and the stack has none: put a ground "
                           "under it.");
        return out;
    }

    // The runs in the plane, on the metals - runsOf joins a via's own cells
    // too, as a little sheet of their own, and makes its vias its own way -
    // and then the vias.
    QList<QPair<int, int>> ends;
    {
        QList<QPair<int, int>> all;
        const QList<Filament> runs = runsOf(mesh, conductors, stack, micrometre, &all);
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
            for (const Conductor& shape : conductors)
                if (shape.layer == layer && shape.outline.boundingRect().contains(probe)
                    && shape.outline.containsPoint(probe, Qt::OddEvenFill)) {
                    inside = true;
                    break;
                }
            if (!inside) ++c.freeSides[a];
        }
    }
    QStringList viaSaid;
    const QList<ViaRun> viaRuns = viaRunsOf(mesh, stack, micrometre, &c.spans, &viaSaid);
    int grounded = 0;
    for (const ViaRun& v : viaRuns) {
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
    const int nr = int(c.runs.size()), nc = int(cellOf.size()), np = int(ports.size());
    c.nr = nr;
    c.nc = nc;
    c.np = np;
    out.runs = nr;
    out.cells = nc;
    const int most = control != nullptr && control->mostUnknowns > 0 ? control->mostUnknowns : kMostUnknowns;
    if (nr + nc > most) {
        out.tooLarge = true;
        out.problems << tr("%1 runs of current over %2 cells is more than this build sets up a dense complex matrix for "
                           "(%3 in all). Mesh the layout more coarsely: fewer cells across a conductor, or a larger "
                           "largest cell.")
                            .arg(nr).arg(nc).arg(most);
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
    bool coplanar = false;
    out.faceWidths = QList<double>(np, 0.0);
    out.faceMetals = QStringList(QList<QString>(np));
    out.feeds = QList<Feed>(np);
    for (int p = 0; p < np; ++p) {
        const FullWavePort& port = ports.at(p);
        const QString& name = out.portNames.at(p);
        const QString x = QString::number(port.at.x(), 'g', 4), y = QString::number(port.at.y(), 'g', 4);
        if (port.internal) {
            // The one cell under its point - or, over a rectangle about it,
            // every cell of its metal the rectangle overlaps, each by its
            // share of the area, as an edge port's face is shared out.
            const bool over = port.size.width() > 0 && port.size.height() > 0;
            const QRectF rect(port.at - QPointF(port.size.width(), port.size.height()) / 2, port.size);
            const auto driven = [&](const QString& metal) {
                if (over) return cellsUnder(mesh, cellOf, rect, metal);
                const int one = cellUnder(mesh, cellOf, port.at, metal);
                return one < 0 ? QList<std::pair<int, double>>() : QList<std::pair<int, double>>{std::make_pair(one, 1.0)};
            };
            const QString w = QString::number(port.size.width(), 'g', 4), h = QString::number(port.size.height(), 'g', 4);
            const QList<std::pair<int, double>> to = driven(port.metal);
            if (to.isEmpty()) {
                out.problems << (over ? tr("%1's %2 by %3 µm rectangle at %4, %5 µm is over no cell of %6 that a current "
                                           "reaches.")
                                            .arg(name, w, h, x, y, port.metal.isEmpty() ? tr("any metal") : port.metal)
                                      : tr("%1, at %2, %3 µm, is over no cell of %4 that a current reaches.")
                                            .arg(name, x, y, port.metal.isEmpty() ? tr("any metal") : port.metal));
                return out;
            }
            Eigen::VectorXcd column = Eigen::VectorXcd::Zero(nc);
            for (const auto& [cell, share] : to) column(nodeOf.value(cell)) += share;
            QString against = tr("the ground");
            if (!port.from.isEmpty() && port.from.compare(QLatin1String("ground"), Qt::CaseInsensitive) != 0) {
                const QList<std::pair<int, double>> from = driven(port.from);
                bool same = from.isEmpty();
                for (const auto& [cell, share] : from)
                    for (const auto& [other, part] : to) same = same || cell == other;
                if (same) {
                    out.problems << tr("%1, at %2, %3 µm, has no cell of %4 under it to drive against.")
                                        .arg(name, x, y, port.from);
                    return out;
                }
                for (const auto& [cell, share] : from) column(nodeOf.value(cell)) -= share;
                against = over ? tr("the %1 cell(s) of %2 under it").arg(from.size()).arg(port.from)
                               : tr("the cell under it on %1").arg(port.from);
            }
            columns << column;
            portOf << p;
            out.portsSaid << (over ? tr("%1 drives the %2 cell(s) of %3 under its %4 by %5 µm rectangle at %6, %7 µm "
                                        "against %8.")
                                         .arg(name).arg(to.size()).arg(mesh.cells.at(to.first().first).layer, w, h, x, y, against)
                                   : tr("%1 drives the cell at %2, %3 µm on %4 against %5.")
                                         .arg(name, x, y, mesh.cells.at(to.first().first).layer, against));
            continue;
        }
        QPointF on, along;
        const QList<int> face = faceCells(mesh, conductors, cellOf, port.at, &on, &along, port.metal, port.facing);
        if (face.isEmpty()) {
            out.problems << tr("%1, at %2, %3 µm, is on no cell a current reaches.").arg(name, x, y);
            return out;
        }
        double faceWidth = 0;
        const QList<double> shares = sharesOf(mesh, face, on, along, &faceWidth);
        Eigen::VectorXcd own = Eigen::VectorXcd::Zero(nc);
        for (int k = 0; k < face.size(); ++k) own(nodeOf.value(face.at(k))) += shares.at(k);
        if (along != QPointF(0, 0)) {
            out.faceWidths[p] = faceWidth;
            out.faceMetals[p] = mesh.cells.at(face.first()).layer;
            out.feeds[p].metal = out.faceMetals.at(p);
            out.feeds[p].width = faceWidth;
        }
        if (!port.coplanar) {
            columns << own;
            portOf << p;
            if (along != QPointF(0, 0))
                out.portsSaid << tr("%1 drives the %2 cell(s) along the face at %3, %4 µm.").arg(name).arg(face.size()).arg(x, y);
            else
                out.portsSaid << tr("%1 is on no face, and drives the one cell nearest %2, %3 µm.").arg(name, x, y);
            continue;
        }
        if (along == QPointF(0, 0)) {
            out.problems << tr("%1 returns through the conductors beside it, so it wants the face of one to stand on, and "
                               "%2, %3 µm is on none.")
                                .arg(name, x, y);
            return out;
        }
        coplanar = true;
        const Beside beside = besideFaces(mesh, cellOf, face, on, along);
        QList<int> counts;
        for (const QList<int>& ground : beside.cells) {
            if (ground.isEmpty()) continue;
            const QList<double> back = sharesOf(mesh, ground, on, along);
            Eigen::VectorXcd column = own;
            for (int k = 0; k < ground.size(); ++k) column(nodeOf.value(ground.at(k))) -= back.at(k);
            columns << column;
            portOf << p;
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
            feedVias(&feed, conductors, stack, on, unit, beside.inside < 0 ? -normal : normal, beside.lo,
                     2 * (8 * height + 2 * faceWidth));
        }
        if (counts.isEmpty()) {
            out.problems << tr("%1 returns through the conductors beside it, and on the line of its edge at %2, %3 µm there "
                               "is none either side of it: draw its grounds to the same edge, or return it through the "
                               "ground plane.")
                                .arg(name, x, y);
            return out;
        }
        out.portsSaid << (counts.size() == 2
                              ? tr("%1 drives the %2 cell(s) along the face at %3, %4 µm against the conductors beside it, "
                                   "%5 and %6 cell(s) of their faces either side: the two tied, as a probe's grounds are.")
                                    .arg(name).arg(face.size()).arg(x, y).arg(counts.at(0)).arg(counts.at(1))
                              : tr("%1 drives the %2 cell(s) along the face at %3, %4 µm against the conductor beside it on "
                                   "one side, %5 cell(s) of its face: a ground-signal probe's.")
                                    .arg(name).arg(face.size()).arg(x, y).arg(counts.at(0)));
    }
    for (int a = 0; a < columns.size(); ++a)
        for (int b = 0; b < a; ++b)
            if (portOf.at(a) != portOf.at(b) && (columns.at(a) - columns.at(b)).norm() < 1e-12) {
                out.problems << tr("%1 and %2 drive the same face or cell of the layout: put them apart.")
                                    .arg(out.portNames.at(portOf.at(b)), out.portNames.at(portOf.at(a)));
                return out;
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
    for (int i = 0; i < nc; ++i) c.hNode[i] = heightIndex(zOf.at(i));
    for (int a = 0; a < nr; ++a) c.hRun[a] = a < c.planar ? heightIndex(c.runs.at(a).z0) : -1;
    // As far as any two points of the layout are apart, and a little more.
    c.rhoMax = 1.05 * std::hypot(extent.width(), extent.height()) + 1e-9;

    // What does not change with the frequency: the runs' static partial
    // inductances, direct and of their images in the ground, and the
    // static potential of each cell's charge spread over it - the singular
    // parts of the two kernels, in closed form.
    c.lDirect = Eigen::MatrixXd::Zero(nr, nr);
    c.lImage = Eigen::MatrixXd::Zero(nr, nr);
    c.dirX.resize(nr);
    c.dirY.resize(nr);
    c.midX.resize(nr);
    c.midY.resize(nr);
    QList<Filament> mirrored = c.runs;
    for (int a = 0; a < nr; ++a) {
        c.runs.at(a).direction(&c.dirX[a], &c.dirY[a], nullptr);
        c.runs.at(a).middle(&c.midX[a], &c.midY[a], nullptr);
        mirrored[a].z0 = mirrored[a].z1 = -c.runs.at(a).z0;
    }
    for (int a = 0; a < nr; ++a)
        for (int b = a; b < nr; ++b) {
            if (std::abs(c.dirX.at(a) * c.dirX.at(b) + c.dirY.at(a) * c.dirY.at(b)) < 1e-9) continue;
            c.lDirect(a, b) = c.lDirect(b, a) = partialInductance(c.runs.at(a), c.runs.at(b));
            c.lImage(a, b) = c.lImage(b, a) = partialInductance(c.runs.at(a), mirrored.at(b));
        }
    c.pStatic.resize(nc, nc);
    for (int i = 0; i < nc; ++i)
        for (int j = 0; j < nc; ++j)
            c.pStatic(i, j) = rectanglePotential(c.rect.at(j), c.centre.at(i), std::abs(zOf.at(i) - zOf.at(j)));

    // The frequencies, side by side: each its own Green's functions and
    // its own system, the static parts shared. As many threads as the
    // machine has, and no more than the memory allows - a system and its
    // LU and P are some 16 (2 nr^2 + nc^2) bytes a frequency.
    const int nf = int(frequencies.size());
    const double perFrequency = 16.0 * (2.0 * double(nr) * nr + double(nc) * nc) + 1e6;
    int threads = control != nullptr && control->threads > 0 ? control->threads
                                                              : int(std::max(1u, std::thread::hardware_concurrency()));
    threads = std::min({threads, nf, std::max(1, int(kSweepMemory / perFrequency))});
    out.threads = threads;
    // (A vector, not a QList: the threads write to it side by side, and a
    //  QList's implicit sharing is no place for that.)
    std::vector<AtFrequency> solved(static_cast<std::size_t>(nf));
    std::atomic_int next{0};
    std::atomic_bool failed{false};
    const auto work = [&c, &frequencies, &solved, &next, &failed, control, nf] {
        for (int k = next++; k < nf; k = next++) {
            if (failed || (control != nullptr && control->stop != nullptr && control->stop->load())) return;
            solved[std::size_t(k)] = solveAt(c, frequencies.at(k));
            if (!solved[std::size_t(k)].problems.isEmpty()) failed = true;
            if (control != nullptr && control->done != nullptr) ++*control->done;
        }
    };
    QList<QThread*> pool;
    for (int t = 1; t < threads; ++t) {
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
                     .arg(threads);
    out.lines << out.portsSaid;
    if (!c.vias.isEmpty())
        out.lines << tr("%1 of the runs are vias, %2 of them to the ground: a via's current runs up it from the cell "
                        "under it to the cell over it, entering each at its centre.")
                         .arg(c.vias.size()).arg(grounded);
    out.lines << viaSaid;
    int alongEdges = 0;
    for (const int free : std::as_const(c.freeSides)) alongEdges += free > 0 ? 1 : 0;
    const QString sheet = tr("a metal as a sheet with the surface impedance of its thickness, and more in the cells along "
                             "its edges, where the current crowds as close as the thickness lets it (%1 run(s) along an "
                             "edge)")
                              .arg(alongEdges);
    out.lines << (coplanar ? tr("The ground plane is taken as perfect, %1; an edge port's current as coming back through "
                                "the ground, and a coplanar port's through the conductors beside it.")
                                 .arg(sheet)
                           : tr("The ground plane is taken as perfect, %1; an edge port's current as coming back through "
                                "the ground.")
                                 .arg(sheet));
    // The columns of a port side by side: one voltage, their currents
    // added. The ports' Z is (B^T Z^-1 B)^-1, B which port each column is
    // of, and an ampere into a port is Z^-1 B Z into the columns.
    MatrixC ofPort = MatrixC::Zero(c.np, np);
    for (int k = 0; k < c.np; ++k) ofPort(k, c.portOf.at(k)) = 1;
    for (int k = 0; k < nf; ++k) {
        AtFrequency a = solved[std::size_t(k)];
        if (c.np != np) {
            const MatrixC x = Eigen::PartialPivLU<MatrixC>(a.z).solve(ofPort);
            const MatrixC y = ofPort.transpose() * x;
            const MatrixC z = Eigen::PartialPivLU<MatrixC>(y).solve(MatrixC::Identity(np, np));
            if (!z.allFinite()) {
                out.problems << tr("At %1 GHz the coplanar ports' grounds could not be tied: their columns came out "
                                   "singular.")
                                    .arg(frequencies.at(k) / 1e9, 0, 'g', 4);
                return out;
            }
            a.current = a.current * (x * z);
            a.z = z;
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
        if (eig.info() == Eigen::Success) out.gain = std::max(out.gain, eig.eigenvalues().maxCoeff() - 1);
        if (a.rcond < 1e-14)
            out.lines << tr("At %1 GHz the system is nearly singular (a reciprocal condition of %2): the answer there may "
                            "be rounding.")
                             .arg(frequencies.at(k) / 1e9, 0, 'g', 4).arg(a.rcond, 0, 'g', 2);
    }
    out.solvedAsymmetry = out.asymmetry;
    out.solvedGain = out.gain;
    out.geometry = c.runs;
    out.ends = ends;
    out.lines << tr("The Green's functions of %1 pair(s) of heights, tabulated to %2 mm at each frequency.")
                     .arg(c.nh * (c.nh + 1) / 2).arg(c.rhoMax * 1e3, 0, 'g', 3);
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

QString touchstone2Of(const FullWave& solved, const QStringList& comments)
{
    if (!solved.ok()) return {};
    const int np = int(solved.portNames.size());
    QStringList references;
    for (const double r : solved.references) references << num(r);
    // (The option line's R a default the [Reference] keyword then gives each
    //  port its own of; a two-port's order said, as 2.0 asks.)
    QString keywords = QStringLiteral("[Version] 2.0\n# Hz S RI R %1\n[Number of Ports] %2\n")
                           .arg(num(solved.references.value(0, 50)))
                           .arg(np);
    if (np == 2) keywords += QStringLiteral("[Two-Port Data Order] 21_12\n");
    keywords += QStringLiteral("[Number of Frequencies] %1\n[Reference] %2\n[Network Data]\n")
                    .arg(solved.frequencies.size())
                    .arg(references.join(QLatin1Char(' ')));
    return touchstoneText(solved, solved.s, comments, keywords, QStringLiteral("[End]\n"));
}

} // namespace qucs_s::rf25
