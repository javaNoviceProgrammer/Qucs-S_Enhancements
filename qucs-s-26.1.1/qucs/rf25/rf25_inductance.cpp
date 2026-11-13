/*
 * rf25_inductance.cpp - the partial inductances of a layout, and the
 *                       inductance of the loop a current makes of them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_inductance.h"

#include "multiphysics/fem_sparse.h"
#include "rf25_fast.h"
#include "rf25_fullwave.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QThread>

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

constexpr double kMu0 = 4e-7 * M_PI;
/// Beyond this many runs and cells a dense solve is not set up (nor beyond
/// RF 2.5D > Settings…' dense limit, denseLimit(), where that is lower):
/// the loop is solved compressed instead, as the full-wave solve is past
/// its own (F5 of the open items of 8 November) - up to
/// kMostCompressedRuns, past which nothing is set up.
constexpr int kMostRuns = 3000;
constexpr int kMostCompressedRuns = 1000000;
/// The compressed loop solve: each low-rank block of L to a part in 1e6
/// of its own size, clusters of up to 32 runs, a pair of clusters low rank
/// where the smaller is no wider than 1.5 times their distance, the near
/// field - the preconditioner - the pairs within a side and a half of the
/// larger's longest side, as the full-wave solve's (rf25_fullwave.cpp);
/// GMRES to a residual of 1e-10, 100 vectors at a time, 3000 in all.
constexpr double kLoopTolerance = 1e-6;
constexpr int kLoopLeaf = 32;
constexpr double kLoopEta = 1.5;
constexpr double kLoopNear = 1.5;
constexpr double kLoopResidual = 1e-10;
constexpr int kLoopRestart = 100;
constexpr int kLoopMostIterations = 3000;
/// Tests: the dense limit, where set (0: kMostRuns and denseLimit()'s).
std::atomic<int> loopDenseLimitSet{0};
/// Two runs further apart than this many of their widths have their
/// transverse distance taken between their middles; nearer than it, it is
/// averaged across both widths.
constexpr double kNearWidths = 6.0;

/// Four-point Gauss-Legendre on [-1/2, 1/2]: the nodes and the weights,
/// which sum to one.
constexpr double kGaussAt[4] = {-0.43056815579702629, -0.16999052179242813, 0.16999052179242813, 0.43056815579702629};
constexpr double kGaussWeight[4] = {0.17392742256872693, 0.32607257743127307, 0.32607257743127307, 0.17392742256872693};

/// s asinh(s/rho) - sqrt(s^2 + rho^2): the second antiderivative of
/// 1/sqrt(s^2 + rho^2), and an even function of s.
double antiderivative(double s, double rho)
{
    return s * std::asinh(s / rho) - std::sqrt(s * s + rho * rho);
}

/*!
 * The double line integral of 1/r over two parallel runs, m: one from 0
 * to \a la along the axis, the other from \a d to \a d + \a lb, their
 * axes \a rho apart.
 *
 * Four terms of the antiderivative, which is exact.
 */
double parallelPair(double la, double lb, double d, double rho)
{
    return antiderivative(d + lb, rho) - antiderivative(d + lb - la, rho) - antiderivative(d, rho) + antiderivative(d - la, rho);
}

struct Vector {
    double x = 0, y = 0, z = 0;
    double dot(const Vector& o) const { return x * o.x + y * o.y + z * o.z; }
    double norm() const { return std::sqrt(dot(*this)); }
    Vector operator-(const Vector& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector operator+(const Vector& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector operator*(double s) const { return {x * s, y * s, z * s}; }
};

Vector directionOf(const Filament& f)
{
    Vector d{f.to.x() - f.from.x(), f.to.y() - f.from.y(), f.z1 - f.z0};
    const double n = d.norm();
    return n > 0 ? d * (1.0 / n) : Vector{0, 0, 0};
}

Vector startOf(const Filament& f) { return {f.from.x(), f.from.y(), f.z0}; }

/*!
 * The direction a run's width lies along: perpendicular to the run and,
 * for a run in the plane, in the plane.
 *
 * A via is a run in z whose cross-section is two-dimensional; one
 * direction of it is taken, which is the same approximation the thin
 * metal of the rest of the solver makes and is small beside a via's own
 * short length.
 */
Vector widthOf(const Filament& f)
{
    const Vector d = directionOf(f);
    if (std::abs(d.z) > 0.5) return {1, 0, 0};
    return {-d.y, d.x, 0};
}

/// A run's breadth across its direction: its rise standing upright, its
/// width lying in the plane.
double breadthOf(const Filament& f)
{
    return f.rise > 0 ? f.rise : f.width;
}

/// Gauss-Legendre's nodes and weights on [-1/2, 1/2], \a n of them (Newton
/// on Legendre's polynomial from Chebyshev's guesses), the weights summing
/// to one.
const std::vector<std::pair<double, double>>& gaussRule(int n)
{
    static const auto make = [](int m) {
        std::vector<std::pair<double, double>> rule;
        for (int i = 0; i < m; ++i) {
            double x = std::cos(M_PI * (i + 0.75) / (m + 0.5)), dp = 1;
            for (int it = 0; it < 100; ++it) {
                double p0 = 1, p1 = x;
                for (int k = 2; k <= m; ++k) {
                    const double p2 = ((2 * k - 1) * x * p1 - (k - 1) * p0) / k;
                    p0 = p1;
                    p1 = p2;
                }
                dp = m * (x * p1 - p0) / (x * x - 1);
                const double step = p1 / dp;
                x -= step;
                if (std::abs(step) < 1e-16) break;
            }
            rule.emplace_back(x / 2, 1 / ((1 - x * x) * dp * dp));
        }
        return rule;
    };
    static const std::vector<std::pair<double, double>> rule24 = make(24);
    Q_ASSERT(n == 24);
    Q_UNUSED(n);
    return rule24;
}

/// A segment of a cross-section: from (u0, v0) to (u1, v1), horizontal (v
/// alike) or vertical (u alike).
struct Segment {
    double u0, v0, u1, v1;
};

/// The mean of ln r from the point (\a u, \a v) over \a s, in closed form:
/// the integral of ln sqrt(x^2 + y^2) along x is x ln r - x + y atan(x/y).
double meanLogFrom(double u, double v, const Segment& s)
{
    const bool horizontal = std::abs(s.v1 - s.v0) <= std::abs(s.u1 - s.u0);
    const double a0 = horizontal ? s.u0 : s.v0, a1 = horizontal ? s.u1 : s.v1;
    const double length = std::abs(a1 - a0);
    const double y = horizontal ? v - s.v0 : u - s.u0;
    const double x0 = (horizontal ? u : v) - a0, x1 = (horizontal ? u : v) - a1;
    if (!(length > 0)) return 0.5 * std::log(x0 * x0 + y * y);
    const auto F = [y](double x) {
        const double r2 = x * x + y * y;
        double term = r2 > 0 ? 0.5 * x * std::log(r2) - x : 0;
        if (y != 0) term += y * std::atan(x / y);
        return term;
    };
    // (Along the segment from a0 to a1, x runs from x0 to x1 backwards: the
    //  integral is F(x0) - F(x1) over a1 - a0, signed - ln r is negative
    //  below a metre, and a mean of it taken whole turned it round.)
    return (F(x0) - F(x1)) / (a1 - a0);
}

/*!
 * The mean of ln r between two segments of a cross-section, m: the inner
 * integral in closed form, the outer by Gauss-Legendre in pieces split
 * where the inner segment's ends come nearest, so that two segments that
 * touch at a corner - a wall's strip and its face's - keep their
 * logarithm's singularity at a piece's end.
 */
double meanLogBetween(const Segment& a, const Segment& b)
{
    const bool horizontal = std::abs(a.v1 - a.v0) <= std::abs(a.u1 - a.u0);
    const double a0 = horizontal ? a.u0 : a.v0, a1 = horizontal ? a.u1 : a.v1;
    if (!(std::abs(a1 - a0) > 0)) return meanLogFrom(a.u0, a.v0, b);
    QList<double> cuts{std::min(a0, a1), std::max(a0, a1)};
    for (const double end : {horizontal ? b.u0 : b.v0, horizontal ? b.u1 : b.v1})
        if (end > cuts.first() && end < cuts.last()) cuts << end;
    std::sort(cuts.begin(), cuts.end());
    const auto& rule = gaussRule(24);
    double sum = 0;
    for (int k = 0; k + 1 < cuts.size(); ++k) {
        const double lo = cuts.at(k), hi = cuts.at(k + 1), span = hi - lo;
        if (!(span > 0)) continue;
        for (const auto& [at, weight] : rule) {
            const double t = lo + (at + 0.5) * span;
            sum += weight * span * (horizontal ? meanLogFrom(t, a.v0, b) : meanLogFrom(a.u0, t, b));
        }
    }
    return sum / (cuts.last() - cuts.first());
}

} // namespace

double Filament::length() const
{
    const double dx = to.x() - from.x(), dy = to.y() - from.y(), dz = z1 - z0;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Filament::direction(double* x, double* y, double* z) const
{
    const Vector d = directionOf(*this);
    if (x != nullptr) *x = d.x;
    if (y != nullptr) *y = d.y;
    if (z != nullptr) *z = d.z;
}

void Filament::middle(double* x, double* y, double* z) const
{
    if (x != nullptr) *x = 0.5 * (from.x() + to.x());
    if (y != nullptr) *y = 0.5 * (from.y() + to.y());
    if (z != nullptr) *z = 0.5 * (z0 + z1);
}

double Filament::resistance() const
{
    const double area = width * thickness;
    if (!(sigma > 0) || !(area > 0)) return 0;
    return length() / (sigma * area);
}

double selfDistance(double width)
{
    if (!(width > 0)) return 0;
    return width * std::exp(-1.5);
}

double partialInductance(const Filament& a, const Filament& b)
{
    const double la = a.length(), lb = b.length();
    if (!(la > 0) || !(lb > 0)) return 0;
    const Vector da = directionOf(a), db = directionOf(b);
    const double along = da.dot(db);
    // Orthogonal runs put no flux through one another, which is what
    // makes an axis-aligned layout cheap: only x with x and y with y.
    if (std::abs(along) < 1e-9) return 0;
    const double sign = along > 0 ? 1.0 : -1.0;

    // Both runs measured along a's own direction: a spans [0, la], and b
    // spans [d, d + lb] whichever way round it is drawn.
    const Vector origin = startOf(a);
    const Vector bFrom = startOf(b) - origin;
    const Vector bTo = (startOf(b) + db * lb) - origin;
    const double sFrom = bFrom.dot(da), sTo = bTo.dot(da);
    const double d = std::min(sFrom, sTo);

    // What is left of the offset once the along-the-axis part is taken
    // out: the distance between the two axes.
    const Vector offset = bFrom - da * sFrom;
    const double apart = offset.norm();

    const bool same = &a == &b;
    const double widest = std::max(breadthOf(a), breadthOf(b));
    // The same run, or two runs on the same axis - consecutive pieces of
    // one strip - have no distance between their axes, so the distance
    // that stands in for it is the cross-section's own geometric mean.
    // It is what makes a strip cut into pieces add back up to the whole.
    if (same || apart <= 1e-9 * std::max(widest, la)) {
        const double rho = selfDistance(widest);
        if (!(rho > 0)) return 0;
        return sign * kMu0 / (4 * M_PI) * parallelPair(la, lb, d, rho);
    }
    // Far enough apart that the widths do not matter: between the axes.
    if (apart > kNearWidths * widest || !(breadthOf(a) > 0) || !(breadthOf(b) > 0))
        return sign * kMu0 / (4 * M_PI) * parallelPair(la, lb, d, apart);
    // Near, and one of them standing upright - a wall's strip beside its
    // face's, or another wall's -: the double integral averaged over both
    // cross-sections, its logarithm apart. Of the four antiderivatives each
    // goes as -|s| ln rho as rho goes to nought, so the pair is a smooth
    // part less K ln rho, K = |d + lb| - |d + lb - la| - |d| + |d - la|
    // (twice the overlap): the smooth part averaged at four points across
    // each, the logarithm's mean between the two segments exactly. The
    // averaged distance below is not good enough for two strips that meet at
    // a corner, and a geometric mean distance alone is not for runs as short
    // as their cross-sections are wide (0.3 % at ten times).
    if (a.rise > 0 || b.rise > 0) {
        const Vector across = widthOf(a);
        const auto segmentOf = [&across](const Filament& f, const Vector& centre) {
            const double u = centre.dot(across), v = centre.z;
            if (f.rise > 0) return Segment{u, v - f.rise / 2, u, v + f.rise / 2};
            return Segment{u - f.width / 2, v, u + f.width / 2, v};
        };
        const Segment sa = segmentOf(a, Vector{0, 0, 0}), sb = segmentOf(b, offset);
        const double K = std::abs(d + lb) - std::abs(d + lb - la) - std::abs(d) + std::abs(d - la);
        double smooth = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                const double pu = sa.u0 + (kGaussAt[i] + 0.5) * (sa.u1 - sa.u0), pv = sa.v0 + (kGaussAt[i] + 0.5) * (sa.v1 - sa.v0);
                const double qu = sb.u0 + (kGaussAt[j] + 0.5) * (sb.u1 - sb.u0), qv = sb.v0 + (kGaussAt[j] + 0.5) * (sb.v1 - sb.v0);
                const double rho = std::max(std::hypot(pu - qu, pv - qv), 1e-12 * widest);
                smooth += kGaussWeight[i] * kGaussWeight[j] * (parallelPair(la, lb, d, rho) + K * std::log(rho));
            }
        return sign * kMu0 / (4 * M_PI) * (smooth - K * meanLogBetween(sa, sb));
    }

    // Near: the distance averaged across both widths, which is where a
    // neighbour's own breadth is most of the answer.
    const Vector wa = widthOf(a), wb = widthOf(b);
    double sum = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            const Vector here = offset + wa * (kGaussAt[i] * a.width) - wb * (kGaussAt[j] * b.width);
            // (Perpendicular to the axis already, both widths being so.)
            const double rho = std::max(here.norm(), 1e-12 * widest);
            sum += kGaussWeight[i] * kGaussWeight[j] * parallelPair(la, lb, d, rho);
        }
    return sign * kMu0 / (4 * M_PI) * sum;
}

double alignedInductance(const Filament& a, const Filament& b)
{
    const Vector da = directionOf(a), db = directionOf(b);
    const double along = da.dot(db);
    if (std::abs(std::abs(along) - 1) < 1e-12) return std::abs(partialInductance(a, b));
    // The direction halfway between the two, the second made to point as
    // the first does; each run turned onto it about its own middle.
    const double flip = along < 0 ? -1.0 : 1.0;
    double hx = da.x + flip * db.x, hy = da.y + flip * db.y;
    const double norm = std::hypot(hx, hy);
    if (!(norm > 0)) return 0;
    hx /= norm;
    hy /= norm;
    const auto turned = [hx, hy](const Filament& f) {
        Filament g = f;
        const QPointF middle = 0.5 * (f.from + f.to);
        const double half = 0.5 * std::hypot(f.to.x() - f.from.x(), f.to.y() - f.from.y());
        g.from = middle - QPointF(hx, hy) * half;
        g.to = middle + QPointF(hx, hy) * half;
        return g;
    };
    return std::abs(partialInductance(turned(a), turned(b)));
}

QList<QList<double>> partialInductances(const QList<Filament>& runs)
{
    QList<QList<double>> out;
    const int n = int(runs.size());
    for (int i = 0; i < n; ++i) out << QList<double>(n, 0.0);
    for (int i = 0; i < n; ++i) {
        out[i][i] = partialInductance(runs.at(i), runs.at(i));
        for (int j = i + 1; j < n; ++j) {
            const double m = partialInductance(runs.at(i), runs.at(j));
            out[i][j] = m;
            out[j][i] = m;
        }
    }
    return out;
}

double loopInductance(const QList<Filament>& loop)
{
    double sum = 0;
    for (int i = 0; i < loop.size(); ++i)
        for (int j = 0; j < loop.size(); ++j) sum += partialInductance(loop.at(i), loop.at(j));
    return sum;
}

namespace {

/// A key two cells sharing an edge agree on, whichever way round they
/// have it: the midpoint, rounded to a nanometre of the layout's units.
QString edgeKey(const QPointF& a, const QPointF& b)
{
    const QPointF middle = (a + b) / 2;
    return QStringLiteral("%1,%2").arg(std::llround(middle.x() * 1e3)).arg(std::llround(middle.y() * 1e3));
}

/// How high metal \a metal sits in \a stack, m; -1 where it is no metal
/// of it. (The same walk the charge extraction makes.)
double heightOf(const Stack& stack, const QString& metal)
{
    double z = 0;
    for (const Layer& l : stack.layers) {
        if (l.name == metal || l.tag == metal) return l.isConductor() ? z : -1;
        if (l.kind == Layer::Dielectric || l.kind == Layer::Substrate) z += l.thickness;
    }
    return -1;
}

} // namespace

QList<Filament> runsOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, double micrometre,
                       QList<QPair<int, int>>* ends, bool vias)
{
    Q_UNUSED(conductors);
    QList<Filament> runs;
    if (ends != nullptr) ends->clear();

    // The pairs of cells that share an edge: where current flows from one
    // to the other, which is what the mesh has been counting all along.
    QHash<QString, QList<int>> shared;
    for (int c = 0; c < mesh.cells.size(); ++c) {
        const MeshCell& cell = mesh.cells.at(c);
        for (int i = 0, n = int(cell.outline.size()); i < n; ++i)
            shared[cell.layer + QLatin1Char('|') + edgeKey(cell.outline.at(i), cell.outline.at((i + 1) % n))].append(c);
    }
    QMap<QString, double> height;
    for (const QString& layer : mesh.byLayer().keys()) height.insert(layer, heightOf(stack, layer));

    // In the order of the cells they join: a hash's order is its own and
    // changes from one run of the program to the next (Qt seeds it at
    // random), and the runs' order is what a compressed solve's clusters
    // are cut from - the same model solved twice was some 1e-7 apart.
    QList<QPair<QPair<int, int>, QString>> edges;
    for (auto it = shared.constBegin(); it != shared.constEnd(); ++it)
        if (it.value().size() == 2) edges << qMakePair(qMakePair(it.value().at(0), it.value().at(1)), it.key());
    std::sort(edges.begin(), edges.end());
    for (const QPair<QPair<int, int>, QString>& joined : std::as_const(edges)) {
        const int a = joined.first.first, b = joined.first.second;
        const MeshCell& ca = mesh.cells.at(a);
        const MeshCell& cb = mesh.cells.at(b);
        const double z = height.value(ca.layer, -1);
        if (!(z >= 0)) continue;
        // (Not between two cells of a via: a via's current runs from one
        // metal to the other, below. Its cells joined in the plane were an
        // island of their own, joined to nothing - a loop through a via of
        // more than one cell "in pieces that nothing joins".)
        if (const Layer* l = stack.named(ca.layer); l != nullptr && l->kind == Layer::Via) continue;
        // Its width is the edge the two cells share; its length is from
        // one centre to the other.
        double edge = 0;
        for (int i = 0, n = int(ca.outline.size()); i < n; ++i) {
            const QPointF p = ca.outline.at(i), q = ca.outline.at((i + 1) % n);
            if (edgeKey(p, q) != joined.second.section(QLatin1Char('|'), 1)) continue;
            edge = std::hypot(q.x() - p.x(), q.y() - p.y());
            break;
        }
        if (!(edge > 0)) continue;
        const Layer* metal = stack.named(ca.layer);
        Filament f;
        f.from = ca.centre * micrometre;
        f.to = cb.centre * micrometre;
        f.z0 = f.z1 = z;
        f.width = edge * micrometre;
        f.thickness = metal != nullptr ? metal->thickness : 0.0;
        f.sigma = metal != nullptr ? metal->sigma : 0.0;
        if (!(f.length() > 0)) continue;
        runs << f;
        if (ends != nullptr) ends->append({a, b});
    }

    // A via joins a cell of one metal to a cell of another: a run in z,
    // which puts no flux through anything in the plane. (The full-wave
    // solve makes its vias its own way, and asks for none of these: each
    // is a search of every cell, a large mesh's vias a large part of it.)
    for (int v = 0; vias && v < mesh.cells.size(); ++v) {
        const MeshCell& cell = mesh.cells.at(v);
        const Layer* via = stack.named(cell.layer);
        if (via == nullptr || via->kind != Layer::Via) continue;
        const double za = heightOf(stack, via->from), zb = heightOf(stack, via->to);
        if (!(za >= 0) || !(zb >= 0) || za == zb) continue;
        const auto nearest = [&mesh, &cell](const QString& layer) {
            int best = -1;
            double closest = 0;
            for (int c = 0; c < mesh.cells.size(); ++c) {
                if (mesh.cells.at(c).layer != layer) continue;
                const QPointF d = mesh.cells.at(c).centre - cell.centre;
                const double far = d.x() * d.x() + d.y() * d.y();
                if (best < 0 || far < closest) {
                    best = c;
                    closest = far;
                }
            }
            return best;
        };
        const int a = nearest(via->from), b = nearest(via->to);
        if (a < 0 || b < 0) continue;
        Filament f;
        f.from = f.to = cell.centre * micrometre;
        f.z0 = std::min(za, zb);
        f.z1 = std::max(za, zb);
        // A via's cross-section is what it covers; one side of it stands
        // for its breadth.
        f.width = std::sqrt(std::max(0.0, cell.area)) * micrometre;
        f.thickness = f.width;
        f.sigma = via->sigma;
        if (!(f.length() > 0) || !(f.width > 0)) continue;
        runs << f;
        if (ends != nullptr) ends->append({za < zb ? a : b, za < zb ? b : a});
    }
    return runs;
}

bool faceEdge(const QList<Conductor>& conductors, QPointF where, const QString& layer, QPointF facing, QLineF* edge,
              QPointF* outward, QString* onLayer)
{
    const double facingNorm = std::hypot(facing.x(), facing.y());
    double closest = 0;
    bool found = false;
    for (const Conductor& c : conductors) {
        if (!layer.isEmpty() && c.layer != layer) continue;
        for (int i = 0, n = int(c.outline.size()); i < n; ++i) {
            const QPointF a = c.outline.at(i), b = c.outline.at((i + 1) % n);
            const QPointF d = b - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            if (!(len2 > 0)) continue;
            // Its outward normal: of the edge's two, the one whose side of
            // the middle is not inside the shape.
            const double len = std::sqrt(len2);
            QPointF normal(d.y() / len, -d.x() / len);
            const QPointF middle = (a + b) / 2;
            if (c.outline.containsPoint(middle + normal * (1e-3 * len), Qt::OddEvenFill)) normal = -normal;
            if (facingNorm > 0 && (normal.x() * facing.x() + normal.y() * facing.y()) / facingNorm < M_SQRT1_2) continue;
            double t = ((where.x() - a.x()) * d.x() + (where.y() - a.y()) * d.y()) / len2;
            t = std::clamp(t, 0.0, 1.0);
            const QPointF on = a + d * t;
            const double far = std::hypot(where.x() - on.x(), where.y() - on.y());
            if (!found || far < closest) {
                closest = far;
                found = true;
                if (edge != nullptr) *edge = QLineF(a, b);
                if (outward != nullptr) *outward = normal;
                if (onLayer != nullptr) *onLayer = c.layer;
            }
        }
    }
    return found;
}

std::array<QLineF, 2> besideEdges(const QList<Conductor>& conductors, const QLineF& face, QPointF outward, const QString& layer,
                                  const QList<bool>* returns)
{
    std::array<QLineF, 2> out;
    const double len = face.length();
    if (!(len > 0)) return out;
    const QPointF unit = (face.p2() - face.p1()) / len, normal(-unit.y(), unit.x());
    const double tol = 1e-6 * std::max(1.0, len);
    // Every edge of the layer on the face's line, facing out the same way:
    // a conductor on the same side of it. As (from the face's end out, to
    // its far end) on either side.
    QList<std::pair<double, double>> sides[2];
    for (int k = 0; k < conductors.size(); ++k) {
        const Conductor& c = conductors.at(k);
        if (!layer.isEmpty() && c.layer != layer) continue;
        if (returns != nullptr && !returns->value(k, false)) continue;
        for (int i = 0, n = int(c.outline.size()); i < n; ++i) {
            const QPointF a = c.outline.at(i) - face.p1(), b = c.outline.at((i + 1) % n) - face.p1();
            if (std::abs(a.x() * normal.x() + a.y() * normal.y()) > tol || std::abs(b.x() * normal.x() + b.y() * normal.y()) > tol)
                continue;
            const QPointF d = b - a;
            const double el = std::hypot(d.x(), d.y());
            if (!(el > 0)) continue;
            QPointF out(d.y() / el, -d.x() / el);
            const QPointF middle = (c.outline.at(i) + c.outline.at((i + 1) % n)) / 2;
            if (c.outline.containsPoint(middle + out * (1e-3 * el), Qt::OddEvenFill)) out = -out;
            if (out.x() * outward.x() + out.y() * outward.y() < 0.9) continue;
            const double t0 = std::min(a.x() * unit.x() + a.y() * unit.y(), b.x() * unit.x() + b.y() * unit.y());
            const double t1 = std::max(a.x() * unit.x() + a.y() * unit.y(), b.x() * unit.x() + b.y() * unit.y());
            if (t1 <= tol) sides[0] << std::make_pair(-t1, -t0);
            else if (t0 >= len - tol) sides[1] << std::make_pair(t0 - len, t1 - len);
        }
    }
    // Of each side, the nearest stretch, through the edges that meet it.
    for (int k = 0; k < 2; ++k) {
        QList<std::pair<double, double>>& side = sides[k];
        if (side.isEmpty()) continue;
        std::sort(side.begin(), side.end());
        const double near = side.first().first;
        double reach = side.first().second;
        for (int i = 1; i < side.size() && side.at(i).first <= reach + tol; ++i) reach = std::max(reach, side.at(i).second);
        out[std::size_t(k)] = k == 0 ? QLineF(face.p1() - unit * near, face.p1() - unit * reach)
                                     : QLineF(face.p2() + unit * near, face.p2() + unit * reach);
    }
    return out;
}

QList<int> faceCells(const Mesh& mesh, const QList<Conductor>& conductors, const QList<int>& reached, QPointF where,
                     QPointF* onLine, QPointF* alongLine, const QString& layer, QPointF facing)
{
    /*
     * A port drives the whole face of the conductor it is on, not the one
     * cell nearest it. Which face is not a guess: the point lies on an
     * edge of a processed shape, and the cells that have a side on that
     * same edge are the face.
     *
     * It matters more than it sounds. Fed at a point, a current must
     * spread out from it, and that spreading resistance is real but is
     * not the bar's - a bar two cells wide came out a sixth too
     * resistive, and the answer moved as the mesh was refined, which is
     * the one thing a solver must not do.
     */
    QList<int> cells;
    if (onLine != nullptr) *onLine = where;
    if (alongLine != nullptr) *alongLine = QPointF(0, 0);
    const auto nearest = [&mesh, &reached, where, &layer] {
        int best = -1;
        double closest = 0;
        for (const int c : reached) {
            if (!layer.isEmpty() && mesh.cells.at(c).layer != layer) continue;
            const QPointF d = mesh.cells.at(c).centre - where;
            const double far = d.x() * d.x() + d.y() * d.y();
            if (best < 0 || far < closest) {
                best = c;
                closest = far;
            }
        }
        return best;
    };
    // The outline edge the point is on, over every shape - of the layer
    // asked for, and facing the way asked for.
    QLineF edge;
    QString onLayer;
    faceEdge(conductors, where, layer, facing, &edge, nullptr, &onLayer);
    const QPointF p = edge.p1(), q = edge.p2();
    if (onLayer.isEmpty()) {
        // No edge of that layer facing that way: the cell nearest it.
        const int one = nearest();
        if (one >= 0) cells << one;
        return cells;
    }
    const QPointF along = q - p;
    const double len = std::hypot(along.x(), along.y());
    if (!(len > 0)) return cells;
    const QPointF unit = along / len;
    const QPointF normal(-unit.y(), unit.x());
    if (onLine != nullptr) *onLine = p;
    if (alongLine != nullptr) *alongLine = unit;
    // A cell is on that face where one of its own sides lies along the
    // same line and within the edge's own extent.
    const double tol = 1e-6 * std::max(1.0, len);
    for (const int c : reached) {
        const MeshCell& cell = mesh.cells.at(c);
        if (cell.layer != onLayer) continue;
        for (int i = 0, n = int(cell.outline.size()); i < n; ++i) {
            const QPointF a = cell.outline.at(i), b = cell.outline.at((i + 1) % n);
            const double offA = (a.x() - p.x()) * normal.x() + (a.y() - p.y()) * normal.y();
            const double offB = (b.x() - p.x()) * normal.x() + (b.y() - p.y()) * normal.y();
            if (std::abs(offA) > tol || std::abs(offB) > tol) continue;
            const double tA = (a.x() - p.x()) * unit.x() + (a.y() - p.y()) * unit.y();
            const double tB = (b.x() - p.x()) * unit.x() + (b.y() - p.y()) * unit.y();
            if (std::max(tA, tB) < -tol || std::min(tA, tB) > len + tol) continue;
            cells << c;
            break;
        }
    }
    // A point that is on no face of anything - inside a shape, or off the
    // layout - drives the one cell nearest it.
    if (cells.isEmpty()) {
        const int one = nearest();
        if (one >= 0) cells << one;
    }
    return cells;
}

namespace {

/*!
 * The runs' partial inductances held compressed: as the full-wave solve
 * holds its L (rf25_fullwave.cpp's Compression), three kernels, of the runs
 * with a part along x, along y, and the vias, each between the runs'
 * cosines' parts -
 *
 *     L_ab = (x_a x_b + y_a y_b + z_a z_b) K_ab
 *
 * K alignedInductance() in the plane and the vias' own partial inductance
 * between them. Neither is nought between a pair of its runs as L is
 * between two at right angles, which a cross approximation would take for
 * a block done when it was half done. Two runs parallel or at right angles
 * - an axis-aligned mesh's every pair - have the dense solve's L to the
 * digit; two oblique ones Neumann's cosine times their aligned integral,
 * where the dense solve measures both along the first.
 *
 * Made in place, and never copied: the matrices point at the trees.
 */
struct LoopL {
    QList<int> xRun, yRun, zRun;
    QList<double> dx, dy, dz;
    ClusterTree xTree, yTree, zTree;
    HMatrix kx, ky, kz;
    QList<QPair<int, int>> near;   ///< the runs' near pairs not at right angles

    double cosine(int a, int b) const { return dx.at(a) * dx.at(b) + dy.at(a) * dy.at(b) + dz.at(a) * dz.at(b); }
    /// y = L x.
    void apply(const VectorC& x, VectorC& y) const
    {
        y = VectorC::Zero(x.size());
        const auto part = [&x, &y](const HMatrix& k, const QList<int>& runs, const QList<double>& d) {
            if (runs.isEmpty()) return;
            VectorC in(runs.size()), out = VectorC::Zero(runs.size());
            for (int i = 0; i < runs.size(); ++i) in(i) = d.at(runs.at(i)) * x(runs.at(i));
            k.multiply(in, out);
            for (int i = 0; i < runs.size(); ++i) y(runs.at(i)) += d.at(runs.at(i)) * out(i);
        };
        part(kx, xRun, dx);
        part(ky, yRun, dy);
        part(kz, zRun, dz);
    }
};

/// The kernel of \a a and \a b, as LoopL holds it.
double loopKernel(const Filament& a, const Filament& b)
{
    const bool planar = a.z0 == a.z1 && b.z0 == b.z1;
    return planar ? alignedInductance(a, b) : std::abs(partialInductance(a, b));
}

/// \a runs' L held compressed into \a l, filled; false (and why) where it
/// cannot be, or \a stop was set on the way.
bool compressedL(LoopL& l, const QList<Filament>& runs, int threads, const std::atomic_bool* stop, QString* why)
{
    QList<Box> boxes, xs, ys, zs;
    for (int a = 0; a < runs.size(); ++a) {
        const Filament& f = runs.at(a);
        double x = 0, y = 0, z = 0;
        f.direction(&x, &y, &z);
        l.dx << x;
        l.dy << y;
        l.dz << z;
        Box box;
        if (f.z0 != f.z1) {
            // A via: what it covers, from one metal to the other.
            const QPointF c = f.from;
            box = Box::of(c.x() - f.width / 2, c.y() - f.width / 2, f.z0, c.x() + f.width / 2, c.y() + f.width / 2, f.z1);
        } else {
            // A run covers the halves of its two cells it joins: from one
            // centre to the other, as wide as the edge between them.
            const double nx = -y * f.width / 2, ny = x * f.width / 2;
            box = Box::of(f.from.x() - nx, f.from.y() - ny, f.z0, f.from.x() + nx, f.from.y() + ny, f.z0)
                      .united(Box::of(f.to.x() - nx, f.to.y() - ny, f.z0, f.to.x() + nx, f.to.y() + ny, f.z0));
        }
        boxes << box;
        if (std::abs(x) > 1e-9) {
            l.xRun << a;
            xs << box;
        }
        if (std::abs(y) > 1e-9) {
            l.yRun << a;
            ys << box;
        }
        if (std::abs(z) > 1e-9) {
            l.zRun << a;
            zs << box;
        }
    }
    l.xTree = ClusterTree(xs, kLoopLeaf);
    l.yTree = ClusterTree(ys, kLoopLeaf);
    l.zTree = ClusterTree(zs, kLoopLeaf);
    l.kx = HMatrix(&l.xTree, &l.xTree, true, kLoopEta);
    l.ky = HMatrix(&l.yTree, &l.yTree, true, kLoopEta);
    l.kz = HMatrix(&l.zTree, &l.zTree, true, kLoopEta);
    const auto entry = [&runs](const QList<int>& which) {
        const QList<int>* w = &which;
        return [&runs, w](int a, int b) { return Complex(loopKernel(runs.at(w->at(a)), runs.at(w->at(b))), 0); };
    };
    const bool filled = l.kx.fill(entry(l.xRun), kLoopTolerance, threads, stop) && l.ky.fill(entry(l.yRun), kLoopTolerance, threads, stop)
                        && l.kz.fill(entry(l.zRun), kLoopTolerance, threads, stop);
    if (!filled) {
        if (why != nullptr) *why = tr("The inductance was stopped.");
        return false;
    }
    for (const QPair<int, int>& pair : nearPairs(boxes, nullptr, kLoopNear))
        if (std::abs(l.cosine(pair.first, pair.second)) >= 1e-9) l.near << pair;
    return true;
}

} // namespace

void setLoopDenseLimit(int runsAndCells)
{
    loopDenseLimitSet = std::max(0, runsAndCells);
}

int loopDenseLimit()
{
    const int set = loopDenseLimitSet.load();
    return set > 0 ? set : std::min(kMostRuns, denseLimit());
}

Inductance inductanceOf(const Mesh& mesh, const QList<Conductor>& conductors, const Stack& stack, QPointF from, QPointF to,
                        double micrometre, double frequency, const std::atomic_bool* stop)
{
    Inductance out;
    if (mesh.cells.isEmpty()) {
        out.problems << tr("There are no cells to find an inductance on: mesh the layout first.");
        return out;
    }
    QList<QPair<int, int>> ends;
    out.runs = runsOf(mesh, conductors, stack, micrometre, &ends);
    out.branches = int(out.runs.size());
    if (out.runs.isEmpty()) {
        out.problems << tr("No two cells of the mesh share an edge, so there is nowhere for a current to flow. Mesh the "
                           "layout more finely.");
        return out;
    }

    // The cells the runs join, numbered as they are met.
    QMap<int, int> nodeOf;
    for (const QPair<int, int>& e : std::as_const(ends)) {
        if (!nodeOf.contains(e.first)) nodeOf.insert(e.first, nodeOf.size());
        if (!nodeOf.contains(e.second)) nodeOf.insert(e.second, nodeOf.size());
    }
    out.nodes = int(nodeOf.size());
    // Past the dense limit, compressed (F5 of the open items of 8
    // November); past what that sets up, refused.
    if (out.branches + out.nodes > kMostCompressedRuns) {
        out.problems << tr("%1 runs of current over %2 cells is more than this build will set up even compressed "
                           "(%3 in all). Mesh the layout more coarsely: fewer cells across a conductor (the Mesh "
                           "node's, ten by default), or a larger largest cell.")
                            .arg(out.branches).arg(out.nodes).arg(kMostCompressedRuns);
        return out;
    }

    // Where the current goes in and comes out: the faces of the conductor
    // the two points lie on, of the cells a run reaches.
    const QList<int> reached = nodeOf.keys();
    const auto faceAt = [&](QPointF where, QPointF* onLine, QPointF* alongLine) {
        return faceCells(mesh, conductors, reached, where, onLine, alongLine);
    };
    QPointF sourceOn, sourceAlong, sinkOn, sinkAlong;
    const QList<int> sourceFace = faceAt(from, &sourceOn, &sourceAlong);
    const QList<int> sinkFace = faceAt(to, &sinkOn, &sinkAlong);
    const int source = sourceFace.isEmpty() ? -1 : sourceFace.first();
    const int sink = sinkFace.isEmpty() ? -1 : sinkFace.first();
    // (Kept for the stubs below: how wide the cell is across the way the
    //  current comes into it.)
    const auto acrossOf = [&mesh](int cell, QPointF along) {
        const QRectF box = mesh.cells.at(cell).outline.boundingRect();
        const double n = std::hypot(along.x(), along.y());
        if (!(n > 0)) return std::min(box.width(), box.height());
        // The extent across the direction: the box's own side that the
        // direction does not run along.
        return std::abs(along.x()) > std::abs(along.y()) ? box.height() : box.width();
    };
    if (source < 0 || sink < 0) {
        out.problems << tr("Neither end of the loop landed on a cell that a current can reach.");
        return out;
    }
    // The two faces must be different ones: a current that goes in and
    // comes out at the same place makes no loop.
    //
    // Different, not disjoint. Two faces that meet at a corner share the
    // cell in that corner - a current going in at the left face and out
    // at the bottom one turns through a right angle, which is a loop like
    // any other - so it is the faces that are compared, not their cells.
    {
        QList<int> a = sourceFace, b = sinkFace;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a == b) {
            out.problems << tr("Both ends of the loop are on the same face of the layout, at the cell at %1, %2 µm: "
                               "put them apart, or mesh the layout more finely.")
                                .arg(mesh.cells.at(a.first()).centre.x(), 0, 'g', 4)
                                .arg(mesh.cells.at(a.first()).centre.y(), 0, 'g', 4);
            return out;
        }
    }

    // The two stubs: from where the current goes in to the first cell's
    // centre, and from the last cell's centre to where it comes out.
    // Without them the solve is of a path a half cell short at each end,
    // which on a bar of ten cells is a tenth of its resistance.
    const auto stub = [&](int cell, QPointF outside, QPointF on, QPointF unit, bool inwards) {
        const QPointF centre = mesh.cells.at(cell).centre;
        // Where this cell's own centre meets the face: a face of several
        // cells is then driven across its breadth, each cell fed straight
        // in, rather than every cell fed from one place on it.
        QPointF at = outside;
        if (unit != QPointF(0, 0)) {
            const double t = (centre.x() - on.x()) * unit.x() + (centre.y() - on.y()) * unit.y();
            at = on + unit * t;
        }
        const QPointF along = centre - at;
        Filament f;
        f.from = (inwards ? at : centre) * micrometre;
        f.to = (inwards ? centre : at) * micrometre;
        f.z0 = f.z1 = heightOf(stack, mesh.cells.at(cell).layer);
        f.width = acrossOf(cell, along) * micrometre;
        const Layer* m = stack.named(mesh.cells.at(cell).layer);
        f.thickness = m != nullptr ? m->thickness : 0.0;
        f.sigma = m != nullptr ? m->sigma : 0.0;
        return f;
    };
    const int portIn = out.nodes, portOut = out.nodes + 1;
    int stubs = 0;
    for (const int cell : sourceFace) {
        const Filament a = stub(cell, from, sourceOn, sourceAlong, true);
        if (!(a.length() > 0) || !(a.width > 0)) continue;
        out.runs << a;
        ends.append({portIn, cell});
        ++stubs;
    }
    for (const int cell : sinkFace) {
        const Filament b = stub(cell, to, sinkOn, sinkAlong, false);
        if (!(b.length() > 0) || !(b.width > 0)) continue;
        out.runs << b;
        ends.append({cell, portOut});
        ++stubs;
    }
    out.branches = int(out.runs.size());
    // The ports are nodes of their own where they have a stub; where they
    // have none the current goes straight in at the cell.
    QMap<int, int> portNode;
    for (const QPair<int, int>& e : std::as_const(ends)) {
        for (const int which : {e.first, e.second})
            if ((which == portIn || which == portOut) && !nodeOf.contains(which)) nodeOf.insert(which, nodeOf.size());
    }
    Q_UNUSED(portNode);
    out.nodes = int(nodeOf.size());
    const int inAt = nodeOf.contains(portIn) ? portIn : source;
    const int outAt = nodeOf.contains(portOut) ? portOut : sink;
    out.lines << tr("The current goes in across %1 cell(s) of one face and out across %2 of another, by %3 stub(s), so "
                    "that the path is the whole of it and the face is driven across its breadth.")
                     .arg(sourceFace.size()).arg(sinkFace.size()).arg(stubs);

    const int nb = out.branches, nn = out.nodes;
    // Of every current that obeys Kirchhoff's law and carries the
    // terminal current, the one that flows is the one of least magnetic
    // energy. Minimising 1/2 I'LI subject to AI = s is a saddle-point
    // system in the currents and the nodes' potentials:
    //
    //     [ L   A' ] [ I ]   [ 0 ]
    //     [ A   0  ] [ u ] = [ s ]
    //
    // One node's row is dropped, since the injections sum to nothing and
    // the potentials are only fixed up to a constant.
    const int drop = nodeOf.value(outAt);
    const int rows = nb + nn - 1;
    const auto row = [drop, nb](int node) { return node < drop ? nb + node : nb + node - 1; };
    const int in = nodeOf.value(inAt);
    if (in == drop) {
        out.problems << tr("The current would go in and out at the same node.");
        return out;
    }
    // A, sparse: each run's two cells.
    std::vector<Eigen::Triplet<double>> incidence;
    for (int b = 0; b < nb; ++b) {
        const int a = nodeOf.value(ends.at(b).first), c = nodeOf.value(ends.at(b).second);
        if (a != drop) incidence.emplace_back(row(a) - nb, b, 1.0);
        if (c != drop) incidence.emplace_back(row(c) - nb, b, -1.0);
    }
    const bool dense = nb + nn <= loopDenseLimit();
    // L applied to a current: of the dense matrix, or the compressed one.
    QList<QList<double>> lp;
    std::unique_ptr<LoopL> compressed;
    std::function<void(const Eigen::VectorXd&, Eigen::VectorXd&)> applyL;
    Eigen::VectorXd answer;
    if (dense) {
        lp = partialInductances(out.runs);
        Eigen::MatrixXd m = Eigen::MatrixXd::Zero(rows, rows);
        Eigen::VectorXd rhs = Eigen::VectorXd::Zero(rows);
        for (int i = 0; i < nb; ++i)
            for (int j = 0; j < nb; ++j) m(i, j) = lp.at(i).at(j);
        for (const Eigen::Triplet<double>& t : incidence) {
            m(nb + t.row(), t.col()) += t.value();
            m(t.col(), nb + t.row()) += t.value();
        }
        rhs(row(in)) = 1.0;   // one amp in, and out at the node that was dropped
        const Eigen::FullPivLU<Eigen::MatrixXd> lu(m);
        if (!lu.isInvertible()) {
            out.problems << tr("The runs of current do not make a path from one end of the loop to the other: the layout is "
                               "in pieces that nothing joins.");
            return out;
        }
        answer = lu.solve(rhs);
        applyL = [&lp, nb](const Eigen::VectorXd& x, Eigen::VectorXd& y) {
            y = Eigen::VectorXd::Zero(nb);
            for (int i = 0; i < nb; ++i)
                for (int j = 0; j < nb; ++j) y(i) += lp.at(i).at(j) * x(j);
        };
    } else {
        // Compressed: L as H-matrices, the system by GMRES preconditioned
        // by the near field's LU - the near pairs' L with the whole of A.
        QElapsedTimer clock;
        clock.start();
        compressed = std::make_unique<LoopL>();
        QString why;
        const int threads = std::max(1, QThread::idealThreadCount());
        if (!compressedL(*compressed, out.runs, threads, stop, &why)) {
            out.problems << why;
            return out;
        }
        const LoopL& l = *compressed;
        // L in its own scale, the runs' own partial inductances' mean: L's
        // rows are picohenries, A's ones, and GMRES's residual - of the
        // whole - would hold the first to nothing (unscaled, the near field
        // alone met 1e-10 on a U of three strips, its L 2 % out).
        double scale = 0;
        for (int a = 0; a < nb; ++a) scale += loopKernel(out.runs.at(a), out.runs.at(a));
        scale = scale > 0 ? scale / nb : 1.0;
        std::vector<Eigen::Triplet<Complex>> near;
        for (const QPair<int, int>& p : l.near) {
            const double v = l.cosine(p.first, p.second) * loopKernel(out.runs.at(p.first), out.runs.at(p.second)) / scale;
            near.emplace_back(p.first, p.second, v);
            if (p.first != p.second) near.emplace_back(p.second, p.first, v);
        }
        for (const Eigen::Triplet<double>& t : incidence) {
            near.emplace_back(nb + t.row(), t.col(), t.value());
            near.emplace_back(t.col(), nb + t.row(), t.value());
        }
        Eigen::SparseMatrix<Complex> pre(rows, rows);
        pre.setFromTriplets(near.begin(), near.end());
        pre.makeCompressed();
        fem::SparseLu<Complex> lu;
        lu.compute(pre);
        if (lu.info() != Eigen::Success) {
            out.problems << tr("The runs of current do not make a path from one end of the loop to the other: the layout is "
                               "in pieces that nothing joins (the near field's LU, which the compressed solve is "
                               "preconditioned with, failed).");
            return out;
        }
        Eigen::SparseMatrix<double> a(nn - 1, nb);
        a.setFromTriplets(incidence.begin(), incidence.end());
        const Eigen::SparseMatrix<double> at = a.transpose();
        const Operator system = [&l, &a, &at, nb, rows, scale](const VectorC& x, VectorC& y) {
            VectorC li;
            l.apply(x.head(nb), li);
            li /= scale;
            y.resize(rows);
            const Eigen::VectorXd re = x.tail(rows - nb).real(), im = x.tail(rows - nb).imag();
            y.head(nb) = li + (at * re).cast<Complex>() + Complex(0, 1) * (at * im).cast<Complex>();
            const Eigen::VectorXd ire = x.head(nb).real(), iim = x.head(nb).imag();
            y.tail(rows - nb) = (a * ire).cast<Complex>() + Complex(0, 1) * (a * iim).cast<Complex>();
        };
        const Operator precondition = [&lu](const VectorC& x, VectorC& y) { y = lu.solve(x); };
        VectorC rhs = VectorC::Zero(rows), x = VectorC::Zero(rows);
        rhs(row(in)) = 1.0;
        const Gmres solved = gmres(system, precondition, rhs, x, kLoopResidual, kLoopRestart, kLoopMostIterations, stop);
        if (solved.stopped) {
            out.problems << tr("The inductance was stopped.");
            return out;
        }
        if (!(solved.residual < 1e-4) || !x.allFinite()) {
            out.problems << tr("The compressed solve of the loop did not converge: GMRES left a residual of %1 after %2 "
                               "iterations. Mesh the layout more coarsely, so that it is solved dense.")
                                .arg(solved.residual, 0, 'g', 2)
                                .arg(solved.iterations);
            return out;
        }
        answer = x.real();
        applyL = [&l](const Eigen::VectorXd& v, Eigen::VectorXd& y) {
            VectorC r;
            l.apply(v.cast<Complex>(), r);
            y = r.real();
        };
        const double held = l.kx.bytes() + l.ky.bytes() + l.kz.bytes();
        const double whole = 8.0 * double(rows) * rows;
        out.compressed = true;
        out.iterations = solved.iterations;
        out.residual = solved.residual;
        out.lines << tr("Past the dense limit (%1 runs and cells), solved compressed: L held in %2 MB where the dense "
                        "system would take %3 MB, GMRES %4 iteration(s) to a residual of %5%6, in %7 s.")
                         .arg(loopDenseLimit())
                         .arg(held / 1048576.0, 0, 'f', 1)
                         .arg(whole / 1048576.0, 0, 'f', 0)
                         .arg(solved.iterations)
                         .arg(solved.residual, 0, 'g', 2)
                         .arg(solved.converged ? QString() : tr(" (short of the 1e-10 asked)"))
                         .arg(clock.elapsed() / 1000.0, 0, 'f', 1);
    }

    // The energy the current carries is the inductance.
    Eigen::VectorXd current = answer.head(nb), li;
    applyL(current, li);
    const double energy = current.dot(li);
    double loss = 0;
    out.current.clear();
    for (int i = 0; i < nb; ++i) {
        out.current << current(i);
        loss += out.runs.at(i).resistance() * current(i) * current(i);
    }
    if (!(energy > 0) || !std::isfinite(energy)) {
        out.problems << tr("The loop's inductance came out as %1 H, which is none.").arg(energy, 0, 'g', 3);
        return out;
    }
    out.l = energy;        // (one amp, so the energy is the inductance)

    // At direct current the resistance divides the current, not the
    // inductance: the network's nodal solve, G u = s with G = A R^-1 A',
    // and the current R^-1 A' u. Its loss is the resistance at direct
    // current and its energy the inductance there. (The least-energy
    // current's loss, given as that until the phase B check of 24
    // October, counted its crowding as resistance: 29 % high on a bar ten
    // cells across.) Where a run says no resistance there is no such
    // current, and the least-energy one's loss is all there is. (Sparse:
    // a compressed loop's nodes are past what a dense G holds.)
    bool resistive = nb > 0;
    for (const Filament& f : std::as_const(out.runs))
        if (!(f.resistance() > 0)) resistive = false;
    out.lDc = energy;
    out.rDc = loss;
    if (resistive) {
        const int m = nn - 1;
        const auto at = [drop](int node) { return node < drop ? node : node - 1; };
        std::vector<Eigen::Triplet<double>> terms;
        for (int b = 0; b < nb; ++b) {
            const double y = 1.0 / out.runs.at(b).resistance();
            const int a = nodeOf.value(ends.at(b).first), c = nodeOf.value(ends.at(b).second);
            if (a != drop) terms.emplace_back(at(a), at(a), y);
            if (c != drop) terms.emplace_back(at(c), at(c), y);
            if (a != drop && c != drop) {
                terms.emplace_back(at(a), at(c), -y);
                terms.emplace_back(at(c), at(a), -y);
            }
        }
        Eigen::SparseMatrix<double> g(m, m);
        g.setFromTriplets(terms.begin(), terms.end());
        Eigen::VectorXd s = Eigen::VectorXd::Zero(m);
        s(at(in)) = 1.0;
        const Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(g);
        const Eigen::VectorXd u = ldlt.solve(s);
        if (ldlt.info() == Eigen::Success && u.allFinite()) {
            Eigen::VectorXd dc(nb);
            for (int b = 0; b < nb; ++b) {
                const int a = nodeOf.value(ends.at(b).first), c = nodeOf.value(ends.at(b).second);
                const double ua = a == drop ? 0.0 : u(at(a)), uc = c == drop ? 0.0 : u(at(c));
                dc(b) = (ua - uc) / out.runs.at(b).resistance();
            }
            double dcLoss = 0;
            for (int i = 0; i < nb; ++i) dcLoss += out.runs.at(i).resistance() * dc(i) * dc(i);
            Eigen::VectorXd ldc;
            applyL(dc, ldc);
            const double dcEnergy = dc.dot(ldc);
            if (dcLoss > 0 && dcEnergy > 0) {
                out.rDc = dcLoss;
                out.lDc = dcEnergy;
            }
        }
    }
    out.frequency = frequency;
    if (frequency > 0 && out.rDc > 0) out.q = 2 * M_PI * frequency * energy / out.rDc;

    double longest = 0;
    for (const Filament& f : std::as_const(out.runs)) longest = std::max(longest, f.length());
    out.lines << tr("%1 run(s) of current over %2 cell(s); the longest is %3 µm.")
                     .arg(nb).arg(nn).arg(longest * 1e6, 0, 'g', 3);
    out.lines << tr("In at the cell at %1, %2 µm and out at %3, %4 µm.")
                     .arg(mesh.cells.at(source).centre.x(), 0, 'g', 4)
                     .arg(mesh.cells.at(source).centre.y(), 0, 'g', 4)
                     .arg(mesh.cells.at(sink).centre.x(), 0, 'g', 4)
                     .arg(mesh.cells.at(sink).centre.y(), 0, 'g', 4);
    out.lines << tr("The loop is %1 nH where its inductance outweighs its resistance, %2 nH and %3 Ohm at direct "
                    "current.")
                     .arg(out.l * 1e9, 0, 'g', 4).arg(out.lDc * 1e9, 0, 'g', 4).arg(out.rDc, 0, 'g', 4);
    if (out.q > 0) out.lines << tr("Q is %1 at %2 GHz, of that inductance over the resistance at direct current.")
                                    .arg(out.q, 0, 'g', 3).arg(frequency / 1e9, 0, 'g', 3);
    return out;
}

} // namespace qucs_s::rf25
