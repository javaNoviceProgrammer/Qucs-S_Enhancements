/*
 * rf25_dielectric.cpp - a dielectric of an RF 2.5D stack that ends: its
 *                       region's faces in panels, the bound charge on them
 *                       from the field just inside each, and what that does
 *                       to the conductors' potential coefficients
 *                       (rf25_dielectric.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_dielectric.h"

#include "rf25_fast.h"
#include "rf25_layout.h"

#include <QCoreApplication>
#include <QHash>
#include <QLineF>
#include <QMap>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("RF25Dielectric", text);
}

using Point3 = std::array<double, 3>;

/// The box round the conductors is grown by this many heights of the stack
/// (the top of the layer that ends, or its thickness, whichever is more):
/// past it what a conductor's field polarizes is a part in some hundreds.
constexpr double kMarginHeights = 6.0;
/// Panels no larger than this share of their distance from the conductors
/// and the region's edges, nor larger than a sixth of the box.
constexpr double kGrading = 0.4;
/// The most panels set up, the grading coarsened past them.
constexpr int kMostPanels = 4000;
/// A panel further than this many of its own sides from a point is its
/// charge at its middle; nearer, its closed form for each strong image.
constexpr double kFarSides = 4.0;
/// The strongest images taken in closed form, the rest from the table.
constexpr int kStrongest = 40;
/// The most rows a wall has up its layer's thickness.
constexpr int kMostRows = 8;

Point3 minus(const Point3& a, const Point3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Point3& a, const Point3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Point3 cross(const Point3& a, const Point3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Point3& a) { return std::sqrt(dot(a, a)); }

/// ln((R+ + l+)/(R- + l-)) along a line, the form that does not cancel where
/// the point is on the line behind the edge.
double edgeLog(double lp, double lm, double rp, double rm)
{
    if (lm >= 0 || lp >= 0) {
        const double a = rp + lp, b = rm + lm;
        if (a > 0 && b > 0) return std::log(a / b);
    }
    // Both ends behind the point: ln((R- - l-)/(R+ - l+)).
    const double a = rm - lm, b = rp - lp;
    return a > 0 && b > 0 ? std::log(a / b) : 0.0;
}

/// The integral of dl / sqrt(l^2 + r^2) along a segment from \a a to \a b
/// (m, horizontal), from the point \a at (horizontal) raised \a z off its
/// line's plane.
double lineIntegral(QPointF a, QPointF b, QPointF at, double z)
{
    const QPointF d = b - a;
    const double length = std::hypot(d.x(), d.y());
    if (!(length > 0)) return 0;
    const QPointF t = d / length;
    const QPointF w = at - a;
    const double along = w.x() * t.x() + w.y() * t.y();
    const double across2 = std::max(0.0, w.x() * w.x() + w.y() * w.y() - along * along) + z * z;
    const double r = std::sqrt(std::max(across2, 1e-300));
    return std::asinh((length - along) / r) - std::asinh(-along / r);
}

} // namespace

double polygonIntegral(const QList<Point3>& corners, const Point3& at)
{
    const int n = int(corners.size());
    if (n < 3) return 0;
    // Its plane: Newell's normal, which also says which way round it goes.
    Point3 normal{0, 0, 0};
    for (int i = 0; i < n; ++i) {
        const Point3& p = corners.at(i);
        const Point3& q = corners.at((i + 1) % n);
        normal[0] += (p[1] - q[1]) * (p[2] + q[2]);
        normal[1] += (p[2] - q[2]) * (p[0] + q[0]);
        normal[2] += (p[0] - q[0]) * (p[1] + q[1]);
    }
    const double area2 = norm(normal);
    if (!(area2 > 0)) return 0;
    for (double& c : normal) c /= area2;
    const double d = dot(minus(at, corners.first()), normal);
    const double ad = std::abs(d);
    // The point projected onto the plane.
    const Point3 rho{at[0] - d * normal[0], at[1] - d * normal[1], at[2] - d * normal[2]};
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const Point3& p = corners.at(i);
        const Point3& q = corners.at((i + 1) % n);
        const Point3 edge = minus(q, p);
        const double length = norm(edge);
        if (!(length > 0)) continue;
        const Point3 l{edge[0] / length, edge[1] / length, edge[2] / length};
        const Point3 u = cross(l, normal);   // outward, the polygon going round the normal
        const double p0 = dot(minus(p, rho), u);
        const double lp = dot(minus(q, rho), l), lm = dot(minus(p, rho), l);
        const double rp = norm(minus(at, q)), rm = norm(minus(at, p));
        const double r02 = p0 * p0 + ad * ad;
        if (std::abs(p0) > 1e-15 * length) sum += p0 * edgeLog(lp, lm, rp, rm);
        if (ad > 0 && std::abs(p0) > 0)
            sum -= ad * (std::atan2(p0 * lp, r02 + ad * rp) - std::atan2(p0 * lm, r02 + ad * rm));
    }
    return std::abs(sum);
}

// ---------------------------------------------------------------- the plan

DielectricPlan dielectricPlan(const Layers& layers, const QList<Extent>& extents, const QRectF& around)
{
    DielectricPlan plan;
    plan.background = layers;
    if (extents.isEmpty()) return plan;
    // The box: the conductors' grown by six heights of the stack.
    double top = 0;
    for (const Extent& e : extents)
        for (const Layers::Slab& s : layers.slabs)
            if (s.name == e.layer) top = std::max({top, s.z1, s.z1 - s.z0});
    const double margin = kMarginHeights * std::max(top, 1e-9);
    plan.box = around.adjusted(-margin, -margin, margin, margin);
    const QRectF boxUm(plan.box.left() * 1e6, plan.box.top() * 1e6, plan.box.width() * 1e6, plan.box.height() * 1e6);
    const QPolygonF boxPolygon(boxUm);
    for (const Extent& e : extents) {
        int slab = -1;
        for (int i = 0; i < layers.slabs.size(); ++i)
            if (layers.slabs.at(i).name == e.layer) slab = i;
        if (slab < 0) {
            plan.problems << tr("%1 is given an extent, but it is no dielectric of the stack.").arg(e.layer);
            continue;
        }
        const double eps = layers.slabs.at(slab).epsr;
        if (std::abs(eps - e.beyond) < 1e-12) {
            plan.lines << tr("%1 ends where it is the same beyond its shapes (eps_r %2): nothing changes.").arg(e.layer).arg(eps);
            continue;
        }
        const QList<QPolygonF> shapes = unionOf(e.shapes);
        // The background the layer as it is beyond its shapes, everywhere;
        // the region its shapes within the box. (The other way round - the
        // layer everywhere, the region what is beyond its shapes - was tried
        // and dropped: its own panels' field nearly cancels their charge, a
        // fifth of it left at a high-contrast interface, and it read a plate
        // on a mesa cut flush with it as fringing less than in air alone.)
        DielectricPlan::Region r;
        r.layer = e.layer;
        r.slab = slab;
        r.inside = true;
        const QList<QPolygonF> region = clipTo(shapes, boxPolygon);
        plan.background.slabs[slab].epsr = e.beyond;
        r.delta = eps - e.beyond;
        for (const QPolygonF& p : region) {
            QPolygonF m;
            for (const QPointF& q : p) m << q * 1e-6;
            r.outline << m;
        }
        if (r.outline.isEmpty()) {
            plan.lines << tr("%1 has no shape within %2 µm of the conductors: it is as beyond its shapes there, eps_r %3.")
                              .arg(e.layer).arg(margin * 1e6, 0, 'g', 3).arg(e.beyond);
            continue;
        }
        plan.lines << tr("%1 ends: eps_r %2 inside its shapes, %3 beyond them - its shapes within %4 µm of the conductors solved as "
                         "a region of their own.")
                          .arg(e.layer)
                          .arg(eps)
                          .arg(e.beyond)
                          .arg(margin * 1e6, 0, 'g', 3);
        plan.regions << r;
    }
    return plan;
}

// ---------------------------------------------------------------- panels

namespace {

struct Panel {
    enum Kind { Top, Bottom, Wall } kind = Top;
    int region = 0;
    QList<Point3> corners;       ///< m, round it
    QPointF from, to;            ///< a wall's edge (m)
    double z0 = 0, z1 = 0;       ///< a wall's rows (m); a face's height in z0
    double zLo = 0, zHi = 0;     ///< its slab's faces (m)
    double gammaLo = 0, gammaHi = 0;   ///< their reflections, seen from inside it
    int row = 0;                 ///< a wall's row
    Point3 centre{0, 0, 0};
    Point3 normal{0, 0, 0};      ///< out of the region
    double area = 0;
    double size = 0;             ///< its longest side
};

/// Heights, to a picometre, as keys.
qint64 keyOf(double z)
{
    return qint64(std::llround(z * 1e12));
}

/// The Green's functions of the background for each pair of heights asked
/// for, built before any thread reads them.
class Greens
{
public:
    explicit Greens(const Layers& layers) : a_layers(layers) {}
    void want(double zf, double zs)
    {
        const auto k = pairKey(zf, zs);
        if (a_greens.count(k) == 0) a_greens.emplace(k, Green(a_layers, std::min(zf, zs), std::max(zf, zs)));
    }
    /// Built, each; false and why where one could not be.
    bool build(const std::atomic_bool* stop, QStringList* problems)
    {
        for (auto& [key, g] : a_greens) {
            g.stopWith(stop);
            (void)g.images();
            if (stop != nullptr && stop->load()) return false;
            if (!g.ok()) {
                *problems += g.problems();
                return false;
            }
            const double want = std::abs(double(key.second - key.first)) * 1e-12;
            double off = std::numeric_limits<double>::infinity(), a = 1;
            for (const Green::Image& t : g.allTerms())
                if (std::abs(std::abs(t.z) - want) < off) {
                    off = std::abs(std::abs(t.z) - want);
                    a = t.a;
                }
            a_direct[key] = a;
        }
        return true;
    }
    const Green& of(double zf, double zs) const { return a_greens.at(pairKey(zf, zs)); }
    /// The coefficient of the direct path between the two heights: one
    /// within a slab, its transmission across an interface - the term at
    /// their distance apart. Found as they are built.
    double directOf(double zf, double zs) const
    {
        const auto it = a_direct.find(pairKey(zf, zs));
        return it != a_direct.end() ? it->second : 1.0;
    }
    int size() const { return int(a_greens.size()); }

private:
    static std::pair<qint64, qint64> pairKey(double a, double b)
    {
        return std::make_pair(std::min(keyOf(a), keyOf(b)), std::max(keyOf(a), keyOf(b)));
    }
    Layers a_layers;
    std::map<std::pair<qint64, qint64>, Green> a_greens;
    std::map<std::pair<qint64, qint64>, double> a_direct;
};

/// The 2-point Gauss rule on a row: where its two heights are.
std::array<double, 2> gaussHeights(double z0, double z1)
{
    const double m = 0.5 * (z0 + z1), h = 0.5 * (z1 - z0) / std::sqrt(3.0);
    return {m - h, m + h};
}

/// What the strongest images leave of a unit charge on horizontal panel
/// \a p, at \a at: at its middle where it is small beside how far that is,
/// else averaged over a grid of points on it. (A face's outer panels are
/// hundreds of micrometres; near one, its middle alone stood for all of it,
/// and under a thin layer - its images' remainder changing over the
/// layer's thickness - a substrate's region grown outward lost capacitance.)
double remainderOver(const Panel& p, const Point3& at, const Green& g, int strong)
{
    const double far = std::max(std::hypot(at[0] - p.centre[0], at[1] - p.centre[1]), 1e-12);
    const double reach = std::max(far, std::abs(at[2] - p.z0));
    const int n = std::clamp(int(std::ceil(2 * p.size / std::max(reach, 1e-12))), 1, 8);
    if (n == 1) return g.beyondStrongest(far, strong);
    QPolygonF outline;
    for (const Point3& q : p.corners) outline << QPointF(q[0], q[1]);
    const QRectF b = outline.boundingRect();
    double sum = 0;
    int inside = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const QPointF q(b.left() + (i + 0.5) * b.width() / n, b.top() + (j + 0.5) * b.height() / n);
            if (!outline.containsPoint(q, Qt::OddEvenFill)) continue;
            sum += g.beyondStrongest(std::max(std::hypot(at[0] - q.x(), at[1] - q.y()), 1e-12), strong);
            ++inside;
        }
    return inside > 0 ? sum / inside : g.beyondStrongest(far, strong);
}

/// The same along a wall's strip, at one of its heights.
double remainderAlong(const Panel& p, const Point3& at, const Green& g, int strong)
{
    const double length = QLineF(p.from, p.to).length();
    const double far = std::max(std::hypot(at[0] - p.centre[0], at[1] - p.centre[1]), 1e-12);
    const int n = std::clamp(int(std::ceil(2 * length / std::max(far, 1e-12))), 1, 8);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const QPointF q = p.from + (p.to - p.from) * ((i + 0.5) / n);
        sum += g.beyondStrongest(std::max(std::hypot(at[0] - q.x(), at[1] - q.y()), 1e-12), strong);
    }
    return sum / n;
}

/// The potential at \a at of a unit charge spread evenly over panel \a p,
/// in the background, V/C.
double panelPotential(const Panel& p, const Point3& at, const Greens& greens)
{
    const QPointF here(at[0], at[1]);
    // (Never quite nought: a Green's function across two heights is nought
    // at no distance at all - Green::potential() answers nothing there -, and
    // a panel straight above another, its middle read at the other's, was a
    // field read across a step from nothing to its potential: a wall's top
    // row coupled to a row sixty micrometres below five times as strongly as
    // to itself, at ten rows.)
    const double far = std::max(std::hypot(at[0] - p.centre[0], at[1] - p.centre[1]), 1e-12);
    const double distance = std::sqrt(far * far + (at[2] - p.centre[2]) * (at[2] - p.centre[2]));
    if (p.kind != Panel::Wall) {
        const Green& g = greens.of(at[2], p.z0);
        if (distance > kFarSides * p.size) return g.potential(far);
        const QList<Green::Image> terms = g.strongestTerms(kStrongest);
        double sum = 0;
        QList<Point3> c = p.corners;
        for (Point3& q : c) q[2] = 0;
        for (const Green::Image& t : terms) sum += t.a * polygonIntegral(c, {at[0], at[1], t.z});
        return sum / (4 * M_PI * g.epsilon() * p.area) + remainderOver(p, at, g, int(terms.size()));
    }
    const double mid = 0.5 * (p.z0 + p.z1);
    if (distance > kFarSides * p.size) return greens.of(at[2], mid).potential(far);
    // Near a wall: each of its strong images whole, in its closed form. An
    // image's distance from the point moves with a charge's height up the
    // wall at a slope of one, one way or the other - which its distances at
    // the two Gauss heights say - so it is a vertical rectangle like the
    // wall: from the point, at the distances its two ends have. The direct
    // path is the wall itself (where the point is within the wall's height
    // its distance is no straight line). What the two heights' strong terms
    // do not share, a line at its own height; the rest from the tables.
    // (Each face's first reflection whole and the rest as lines was not
    // enough: a thin layer over the wall's top is a train of images against
    // its edge, and a plate on a mesa under a 0.3 µm oxide read 10 fF where
    // under 1 µm it read 14.)
    const std::array<double, 2> heights = gaussHeights(p.z0, p.z1);
    const Green& g1 = greens.of(at[2], heights[0]);
    const Green& g2 = greens.of(at[2], heights[1]);
    const QList<Green::Image> t1 = g1.strongestTerms(kStrongest), t2 = g2.strongestTerms(kStrongest);
    const double delta = heights[1] - heights[0];
    const double tol = 1e-6 * std::max(p.zHi - p.zLo, delta);
    const double length = std::hypot(p.to.x() - p.from.x(), p.to.y() - p.from.y());
    QList<bool> used(t2.size(), false);
    double whole = 0, lines = 0;
    const double d1 = std::abs(at[2] - heights[0]), d2 = std::abs(at[2] - heights[1]);
    for (const Green::Image& a : t1) {
        int match = -1;
        double slope = 0;
        // The direct path first, by its own distances at the two heights -
        // where the point is within the wall's height they do not change by
        // the heights' spacing, and as a pair it was missed and made lines.
        if (std::abs(std::abs(a.z) - d1) <= tol) {
            for (int k = 0; k < t2.size() && match < 0; ++k)
                if (!used.at(k) && std::abs(std::abs(t2.at(k).z) - d2) <= tol && std::abs(a.a - t2.at(k).a) <= 1e-6 * std::abs(a.a) + 1e-12)
                    match = k;
            if (match >= 0) {
                used[match] = true;
                whole += a.a * polygonIntegral(p.corners, at);
                continue;
            }
        }
        for (int k = 0; k < t2.size(); ++k) {
            if (used.at(k)) continue;
            const Green::Image& b = t2.at(k);
            if (std::abs(a.a - b.a) > 1e-6 * std::abs(a.a) + 1e-12) continue;
            const double change = std::abs(b.z) - std::abs(a.z);
            if (std::abs(std::abs(change) - delta) > 1e-3 * delta + tol) continue;
            match = k;
            slope = change > 0 ? 1.0 : -1.0;
            break;
        }
        if (match < 0) {
            lines += 0.5 * a.a * lineIntegral(p.from, p.to, here, a.z);
            continue;
        }
        used[match] = true;
        // Its distances at the wall's two ends; below the point, at them.
        const double dLow = std::abs(a.z) + slope * (p.z0 - heights[0]), dHigh = std::abs(a.z) + slope * (p.z1 - heights[0]);
        if (dLow < 0 || dHigh < 0) {
            lines += 0.5 * a.a * lineIntegral(p.from, p.to, here, a.z) + 0.5 * a.a * lineIntegral(p.from, p.to, here, t2.at(match).z);
            continue;
        }
        const QList<Point3> image{{p.from.x(), p.from.y(), at[2] - dLow}, {p.to.x(), p.to.y(), at[2] - dLow},
                                  {p.to.x(), p.to.y(), at[2] - dHigh}, {p.from.x(), p.from.y(), at[2] - dHigh}};
        whole += a.a * polygonIntegral(image, at);
    }
    for (int k = 0; k < t2.size(); ++k)
        if (!used.at(k)) lines += 0.5 * t2.at(k).a * lineIntegral(p.from, p.to, here, t2.at(k).z);
    return whole / (4 * M_PI * g1.epsilon() * p.area) + lines / (4 * M_PI * g1.epsilon() * length)
           + 0.5 * (remainderAlong(p, at, g1, int(t1.size())) + remainderAlong(p, at, g2, int(t2.size())));
}

/// The potential at \a at of a unit charge spread over \a cell, V/C.
double cellPotentialAt(const ChargedCell& cell, const Point3& at, const Greens& greens)
{
    const Green& g = greens.of(at[2], cell.z);
    const QPointF c = cell.rect.center();
    const double far = std::max(std::hypot(at[0] - c.x(), at[1] - c.y()), 1e-12);
    const double size = std::max(cell.rect.width(), cell.rect.height());
    const double distance = std::sqrt(far * far + (at[2] - cell.z) * (at[2] - cell.z));
    if (distance > kFarSides * size) return g.potential(far);
    const QList<Green::Image> terms = g.strongestTerms(kStrongest);
    const QRectF& r = cell.rect;
    const QList<Point3> corners{{r.left(), r.top(), 0}, {r.right(), r.top(), 0}, {r.right(), r.bottom(), 0}, {r.left(), r.bottom(), 0}};
    double sum = 0;
    for (const Green::Image& t : terms) sum += t.a * polygonIntegral(corners, {at[0], at[1], t.z});
    return sum / (4 * M_PI * g.epsilon() * std::max(r.width() * r.height(), 1e-300)) + g.beyondStrongest(far, int(terms.size()));
}

/// Where the field normal to panel \a p just inside it is read: its middle
/// and two points a step \a h further in; the field there is
/// (-3 phi0 + 4 phi1 - phi2) / 2h.
/// The step panel \a p's field is read over: its region's, or for a wall a
/// twentieth of its own shorter side where that is less (its probes stay at
/// its own height).
double stepOf(const Panel& p, double regionStep)
{
    if (p.kind != Panel::Wall) return regionStep;
    return std::min(regionStep, std::min(QLineF(p.from, p.to).length(), p.z1 - p.z0) / 20);
}

std::array<Point3, 3> probesOf(const Panel& p, double h)
{
    std::array<Point3, 3> out;
    for (int k = 0; k < 3; ++k)
        for (int a = 0; a < 3; ++a) out[std::size_t(k)][std::size_t(a)] = p.centre[std::size_t(a)] - k * h * p.normal[std::size_t(a)];
    return out;
}

double fieldOf(const std::array<double, 3>& phi, double h)
{
    return (-3 * phi[0] + 4 * phi[1] - phi[2]) / (2 * h);
}

/// Whether \a polygon turns one way all round (collinear corners aside).
bool isConvex(const QPolygonF& polygon)
{
    QPolygonF q = polygon;
    if (q.size() > 1 && q.first() == q.last()) q.removeLast();
    const int n = int(q.size());
    if (n < 3) return false;
    int sign = 0;
    const QRectF b = q.boundingRect();
    const double tiny = 1e-9 * std::max(b.width(), b.height()) * std::max(b.width(), b.height());
    for (int i = 0; i < n; ++i) {
        const QPointF a = q.at(i), c = q.at((i + 1) % n), d = q.at((i + 2) % n);
        const double cross = (c.x() - a.x()) * (d.y() - c.y()) - (c.y() - a.y()) * (d.x() - c.x());
        if (std::abs(cross) <= tiny) continue;
        const int s = cross > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        else if (s != sign) return false;
    }
    return true;
}

/// The distance from \a at to the nearest of \a rects (m).
double nearestOf(const QList<QRectF>& rects, QPointF at)
{
    double best = std::numeric_limits<double>::infinity();
    for (const QRectF& r : rects) {
        const double dx = std::max({r.left() - at.x(), 0.0, at.x() - r.right()});
        const double dy = std::max({r.top() - at.y(), 0.0, at.y() - r.bottom()});
        best = std::min(best, std::hypot(dx, dy));
    }
    return best;
}

double toSegment(QPointF a, QPointF b, QPointF at)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    const double t = l2 > 0 ? std::clamp(((at.x() - a.x()) * d.x() + (at.y() - a.y()) * d.y()) / l2, 0.0, 1.0) : 0.0;
    return std::hypot(at.x() - a.x() - t * d.x(), at.y() - a.y() - t * d.y());
}

/// The region's boundary edges (m), each with the region on its left as it
/// runs - a keyhole's two sides, and two pieces' shared edge, left out.
QList<QLineF> edgesOf(const QList<QPolygonF>& outline)
{
    QList<QLineF> all;
    for (const QPolygonF& p : outline) {
        QPolygonF q = p;
        if (q.size() > 1 && q.first() == q.last()) q.removeLast();
        if (q.size() < 3) continue;
        const bool ccw = [&q] {
            double a = 0;
            for (int i = 0; i < q.size(); ++i) {
                const QPointF& u = q.at(i);
                const QPointF& v = q.at((i + 1) % q.size());
                a += u.x() * v.y() - v.x() * u.y();
            }
            return a > 0;
        }();
        for (int i = 0; i < q.size(); ++i) {
            QLineF e(q.at(i), q.at((i + 1) % q.size()));
            if (!ccw) e = QLineF(e.p2(), e.p1());
            if (e.length() > 0) all << e;
        }
    }
    // (To a nanometre: what gdstk's grid put them on.)
    const auto key = [](QPointF a, QPointF b) {
        return QStringLiteral("%1,%2,%3,%4").arg(std::llround(a.x() * 1e9)).arg(std::llround(a.y() * 1e9)).arg(std::llround(b.x() * 1e9)).arg(std::llround(b.y() * 1e9));
    };
    QHash<QString, int> count;
    for (const QLineF& e : std::as_const(all)) ++count[key(e.p1(), e.p2())];
    QList<QLineF> out;
    for (const QLineF& e : std::as_const(all))
        if (!count.contains(key(e.p2(), e.p1()))) out << e;
    return out;
}

} // namespace

double wallPotentialForTests(const Layers& layers, int slab, QPointF from, QPointF to, double z0, double z1, const std::array<double, 3>& at)
{
    Panel p;
    p.kind = Panel::Wall;
    p.from = from;
    p.to = to;
    p.z0 = z0;
    p.z1 = z1;
    const Layers::Slab& s = layers.slabs.at(slab);
    p.zLo = s.z0;
    p.zHi = s.z1;
    const double epsAbove = slab + 1 < layers.slabs.size() ? layers.slabs.at(slab + 1).epsr : layers.epsrAbove;
    const double epsBelow = slab > 0 ? layers.slabs.at(slab - 1).epsr : layers.epsrBelow;
    p.gammaHi = (s.epsr - epsAbove) / (s.epsr + epsAbove);
    p.gammaLo = slab == 0 && layers.groundBelow ? -1.0 : (s.epsr - epsBelow) / (s.epsr + epsBelow);
    p.corners = {{from.x(), from.y(), z0}, {to.x(), to.y(), z0}, {to.x(), to.y(), z1}, {from.x(), from.y(), z1}};
    const QPointF c = 0.5 * (from + to);
    p.centre = {c.x(), c.y(), 0.5 * (z0 + z1)};
    p.area = QLineF(from, to).length() * (z1 - z0);
    p.size = std::max(QLineF(from, to).length(), z1 - z0);
    Greens g(layers);
    g.want(at[2], 0.5 * (z0 + z1));
    for (const double z : gaussHeights(z0, z1)) g.want(at[2], z);
    QStringList problems;
    if (!g.build(nullptr, &problems)) return 0;
    return panelPotential(p, at, g);
}

// ----------------------------------------------------------- the correction

void DielectricCorrection::apply(const Eigen::VectorXd& x, Eigen::VectorXd& y) const
{
    if (factored) y += u * (v.transpose() * x);
    else if (delta.size() > 0) y += delta * x;
}

double DielectricCorrection::at(int i, int j) const
{
    if (factored) return u.row(i).dot(v.row(j));
    return delta.size() > 0 ? delta(i, j) : 0.0;
}

DielectricCorrection dielectricCorrection(const DielectricPlan& plan, const QList<ChargedCell>& given, bool factored,
                                          const std::atomic_bool* stop, int mostPanels)
{
    // A metal a hair off a region's face - an oxide of some nanometres
    // between - taken as on it: the step its field is read over is some
    // tenths of a micrometre, and the thin layer's images a train too close
    // to read across (a 10 nm oxide was 6 % out and its mesas no answer);
    // moved by so little the metal is where it was.
    QList<ChargedCell> cells = given;
    for (ChargedCell& c : cells) {
        const double side = std::min(c.rect.width(), c.rect.height());
        for (const DielectricPlan::Region& r : plan.regions) {
            const Layers::Slab& s = plan.background.slabs.at(r.slab);
            for (const double face : {s.z0, s.z1})
                if (c.z != face && std::abs(c.z - face) < 0.005 * side) c.z = face;
        }
    }
    DielectricCorrection out;
    out.factored = factored;
    const int nc = int(cells.size());
    if (plan.regions.isEmpty() || nc == 0) {
        if (!factored) out.delta = Eigen::MatrixXd::Zero(nc, nc);
        return out;
    }
    const Layers& layers = plan.background;
    const int most = mostPanels > 0 ? mostPanels : kMostPanels;
    QList<QRectF> rects;
    double smallestCell = std::numeric_limits<double>::infinity();
    for (const ChargedCell& c : cells) {
        rects << c.rect;
        smallestCell = std::min(smallestCell, std::max(c.rect.width(), c.rect.height()));
    }
    // The panels: graded from the conductors and the region's edges, the
    // grading coarsened until they are no more than the most.
    QList<Panel> panels;
    // The step a face's field is read over, by region: a twentieth of its
    // smallest panel by the grading. (A twentieth of the smallest panel
    // there was, a sliver the cutting left was some nanometres, and the
    // difference across it all rounding: a mesa's capacitance a sixth of
    // what it was, at some margins and not others.)
    QList<double> step(plan.regions.size(), 0.0);
    double coarsen = 1;
    for (int attempt = 0; attempt < 12; ++attempt, coarsen *= 1.5) {
        panels.clear();
        for (int ri = 0; ri < plan.regions.size(); ++ri) {
            const DielectricPlan::Region& r = plan.regions.at(ri);
            const Layers::Slab& slab = layers.slabs.at(r.slab);
            const double t = slab.z1 - slab.z0;
            const double smallest = coarsen * std::max(std::min(t, 2 * smallestCell) / 2, 1e-9);
            const double largest = std::max(smallest, std::max(plan.box.width(), plan.box.height()) / 6);
            step[ri] = smallest / 20;
            // (Less than a fifth of the smallest panel across: a sliver
            // the cutting left, dropped.)
            const double least = 0.001 * smallest * smallest;
            const QList<QLineF> edges = edgesOf(r.outline);
            const auto sizeAt = [&](QPointF at) {
                double d = nearestOf(rects, at);
                for (const QLineF& e : edges) d = std::min(d, toSegment(e.p1(), e.p2(), at));
                return std::clamp(kGrading * d, smallest, largest);
            };
            // Its faces: squares split while larger than the grading, then
            // cut to the region (in µm, gdstk's grid).
            QRectF all;
            for (const QPolygonF& p : r.outline) all = all.united(p.boundingRect());
            const bool top = true;   // (a metal lid over the stack is not solved: layersOf() refuses it)
            const bool bottom = !(r.slab == 0 && layers.groundBelow);
            QList<QRectF> squares{QRectF(all.topLeft(), QSizeF(std::max(all.width(), all.height()), std::max(all.width(), all.height())))};
            QList<QRectF> leaves;
            while (!squares.isEmpty()) {
                const QRectF s = squares.takeLast();
                if (s.width() > sizeAt(s.center()) * 1.0001 && s.width() > smallest * 1.0001) {
                    const double h = s.width() / 2;
                    for (int k = 0; k < 4; ++k) squares << QRectF(s.left() + (k % 2) * h, s.top() + (k / 2) * h, h, h);
                } else {
                    leaves << s;
                }
                if (leaves.size() > 4 * most) break;
            }
            QList<QPolygonF> outlineUm;
            for (const QPolygonF& p : r.outline) {
                QPolygonF q;
                for (const QPointF& v : p) q << v * 1e6;
                outlineUm << q;
            }
            // A face where a metal lies on it takes that metal's cells for its
            // panels there, each the same rectangle: a panel larger than the
            // cells over it, its field read at its middle, put the field under
            // one cell over the whole panel - a cell's bound charge 1.7 times
            // its own, where the most it can be is 1 - 1/eps_r.
            const auto addFace = [&](const QPolygonF& piece, int side, double minimum) {
                QPolygonF q = piece;
                if (q.size() > 1 && q.first() == q.last()) q.removeLast();
                const double area = areaOf(q) * 1e-12;
                if (q.size() < 3 || !(area > minimum)) return;
                Panel p;
                p.kind = side == 0 ? Panel::Top : Panel::Bottom;
                p.region = ri;
                // (The bottom a hair inside its slab, as a charge on an
                // interface belongs to the slab below it.)
                p.z0 = side == 0 ? slab.z1 : slab.z0 + 1e-6 * t;
                double cx = 0, cy = 0, twice = 0;
                for (int k = 0; k < q.size(); ++k) {
                    const QPointF& a = q.at(k);
                    const QPointF& b = q.at((k + 1) % q.size());
                    const double c = a.x() * b.y() - b.x() * a.y();
                    twice += c;
                    cx += (a.x() + b.x()) * c;
                    cy += (a.y() + b.y()) * c;
                    p.corners << Point3{a.x() * 1e-6, a.y() * 1e-6, p.z0};
                }
                p.centre = {cx / (3 * twice) * 1e-6, cy / (3 * twice) * 1e-6, p.z0};
                const QRectF b = q.boundingRect();
                p.size = std::max(b.width(), b.height()) * 1e-6;
                p.area = area;
                p.normal = {0, 0, side == 0 ? 1.0 : -1.0};
                panels << p;
            };
            for (const int side : {0, 1}) {
                if (side == 0 && !top) continue;
                if (side == 1 && !bottom) continue;
                const double z = side == 0 ? slab.z1 : slab.z0;
                // (Of a metal on the face, or near it - within two of its
                // cells' sides above or below: a metal a hundredth of a
                // micrometre over an oxide on the face, its cells not the
                // panels under them, was the same misreading. The nearest
                // metal's first; a farther one's only where that leaves room.)
                QMap<qint64, QList<QPolygonF>> byHeight;
                for (const ChargedCell& c : cells) {
                    const double side2 = std::max(c.rect.width(), c.rect.height());
                    if (std::abs(c.z - z) <= 2 * side2 + 1e-9 * std::max(t, 1e-9))
                        byHeight[keyOf(std::abs(c.z - z))]
                            << QPolygonF(QRectF(c.rect.left() * 1e6, c.rect.top() * 1e6, c.rect.width() * 1e6, c.rect.height() * 1e6));
                }
                QList<QPolygonF> onIt, covered;
                // (Each piece convex, its middle on it: a square cut round a
                // conductor's corner was an L, its middle in the notch - under
                // the conductor -, and its field read there carried the
                // conductor's own jump: a mesa's capacitance five times what it
                // was, at some gradings. A piece that is not is split again.)
                const auto tile = [&](const QList<QPolygonF>& within, const QRectF& square, double minimum) {
                    QList<QRectF> work{square};
                    while (!work.isEmpty()) {
                        const QRectF sq = work.takeLast();
                        const QList<QPolygonF> pieces = clipTo(within, QPolygonF(sq));
                        bool convex = true;
                        for (const QPolygonF& piece : pieces) convex = convex && isConvex(piece);
                        if (!convex && sq.width() > 0.1 * smallest * 1e6) {
                            const double hw = sq.width() / 2, hh = sq.height() / 2;
                            for (int k = 0; k < 4; ++k) work << QRectF(sq.left() + (k % 2) * hw, sq.top() + (k / 2) * hh, hw, hh);
                            continue;
                        }
                        for (const QPolygonF& piece : pieces)
                            if (isConvex(piece)) addFace(piece, side, minimum);
                    }
                };
                for (auto it = byHeight.cbegin(); it != byHeight.cend(); ++it) {
                    const QList<QPolygonF> room = covered.isEmpty() ? outlineUm : differenceOf(outlineUm, covered);
                    for (const QPolygonF& cell : it.value()) tile(room, cell.boundingRect(), std::min(least, 0.04 * areaOf(cell) * 1e-12));
                    onIt << it.value();
                    covered = unionOf(onIt);
                }
                const QList<QPolygonF> rest = onIt.isEmpty() ? outlineUm : differenceOf(outlineUm, covered);
                for (const QRectF& sq : std::as_const(leaves))
                    tile(rest, QRectF(sq.left() * 1e6, sq.top() * 1e6, sq.width() * 1e6, sq.height() * 1e6), least);
            }
            // Its walls: each edge in strips no longer than the grading,
            // rows up the thickness closer at its two ends.
            double shortest = std::numeric_limits<double>::infinity();
            QList<QPair<QLineF, int>> strips;
            for (const QLineF& e : edges) {
                QList<double> at{0};
                if (e.length() < 0.2 * smallest) continue;   // (a sliver's edge)
                while (at.last() < 1) {
                    const QPointF here = e.pointAt(at.last());
                    const double along = sizeAt(here) / e.length();
                    at << std::min(1.0, at.last() + std::max(along, 1e-6));
                }
                // (A last strip much shorter than the one before it joins it.)
                if (at.size() > 2 && at.at(at.size() - 1) - at.at(at.size() - 2) < 0.5 * (at.at(at.size() - 2) - at.at(at.size() - 3)))
                    at.removeAt(at.size() - 2);
                for (int k = 0; k + 1 < at.size(); ++k) {
                    const QLineF s(e.pointAt(at.at(k)), e.pointAt(at.at(k + 1)));
                    strips << qMakePair(s, 0);
                    shortest = std::min(shortest, s.length());
                }
            }
            const int rows = std::clamp(int(std::ceil(t / std::max(shortest, 1e-12))), 4, kMostRows);
            const double epsHere = slab.epsr;
            const double epsAbove = r.slab + 1 < layers.slabs.size() ? layers.slabs.at(r.slab + 1).epsr : layers.epsrAbove;
            const double epsBelow = r.slab > 0 ? layers.slabs.at(r.slab - 1).epsr : layers.epsrBelow;
            const double gammaHi = (epsHere - epsAbove) / (epsHere + epsAbove);
            const double gammaLo = r.slab == 0 && layers.groundBelow ? -1.0 : (epsHere - epsBelow) / (epsHere + epsBelow);
            // (Closer toward a face where the wall meets another - the field
            // is singular along the corner -, and not toward the ground, where
            // it is not and a thin row and its image are a dipole too thin to
            // read across.)
            const bool onGround = r.slab == 0 && layers.groundBelow;
            QList<double> zs;
            for (int k = 0; k <= rows; ++k)
                zs << (onGround ? slab.z0 + t * std::sin(0.5 * M_PI * k / rows) : slab.z0 + t * 0.5 * (1 - std::cos(M_PI * k / rows)));
            for (const auto& [s, unused] : std::as_const(strips)) {
                // Out of the region: the region on the edge's left.
                const QPointF d = (s.p2() - s.p1()) / s.length();
                const Point3 n{d.y(), -d.x(), 0};
                for (int k = 0; k < rows; ++k) {
                    Panel p;
                    p.kind = Panel::Wall;
                    p.region = ri;
                    p.from = s.p1();
                    p.to = s.p2();
                    p.z0 = zs.at(k);
                    p.z1 = zs.at(k + 1);
                    p.zLo = slab.z0;
                    p.zHi = slab.z1;
                    p.gammaHi = gammaHi;
                    p.gammaLo = gammaLo;
                    p.row = k;
                    p.corners = {{s.p1().x(), s.p1().y(), p.z0}, {s.p2().x(), s.p2().y(), p.z0},
                                 {s.p2().x(), s.p2().y(), p.z1}, {s.p1().x(), s.p1().y(), p.z1}};
                    const QPointF c = 0.5 * (s.p1() + s.p2());
                    p.centre = {c.x(), c.y(), 0.5 * (p.z0 + p.z1)};
                    p.normal = n;
                    p.area = s.length() * (p.z1 - p.z0);
                    p.size = std::max(s.length(), p.z1 - p.z0);
                    panels << p;
                }
            }
        }
        if (panels.size() <= most) break;
    }
    if (panels.size() > most) {
        out.problems << tr("An ended dielectric's faces would take %1 panels, more than the %2 this build sets up: the conductors "
                           "and the layer's edge are too far apart beside its thickness.")
                            .arg(panels.size()).arg(most);
        return out;
    }
    const int nb = int(panels.size());
    out.panels = nb;

    // The steps a face's field from a far source is read over: three, twelve
    // and forty-eight near steps (inside the slab), each where the source is
    // sixteen of them away. (Read over the near step - some tenths of a
    // micrometre - across tables of three heights, a far source's field was
    // their interpolation's error over that step: a substrate's region grown
    // outward under an oxide lost capacitance, unevenly with its panels.)
    // (From three near steps, so that a source no longer near - four of its
    // own sides away, two of the smallest panels at the least - is sixteen
    // of one away: a source between was read over the near step across the
    // tables, and the finer that step the further out.)
    QList<QList<double>> farSteps;
    for (int ri = 0; ri < plan.regions.size(); ++ri) {
        const Layers::Slab& sl = layers.slabs.at(plan.regions.at(ri).slab);
        const double most = (sl.z1 - sl.z0) / 3;
        farSteps << QList<double>{std::min(3 * step.at(ri), most), std::min(12 * step.at(ri), most), std::min(48 * step.at(ri), most)};
    }
    // The Green's functions: of every height a field is read at, for every
    // height a charge sits at.
    Greens greens(layers);
    QList<double> sourceZ, fieldZ;
    const auto addZ = [](QList<double>& list, double z) {
        for (const double v : list)
            if (keyOf(v) == keyOf(z)) return;
        list << z;
    };
    for (const Panel& p : std::as_const(panels)) {
        if (p.kind == Panel::Wall) {
            addZ(sourceZ, 0.5 * (p.z0 + p.z1));
            for (const double z : gaussHeights(p.z0, p.z1)) addZ(sourceZ, z);
        } else {
            addZ(sourceZ, p.z0);
        }
        for (const Point3& probe : probesOf(p, stepOf(p, step.at(p.region)))) addZ(fieldZ, probe[2]);
        if (p.kind != Panel::Wall)
            for (const double far : farSteps.at(p.region))
                for (const Point3& probe : probesOf(p, far)) addZ(fieldZ, probe[2]);
    }
    QList<double> cellZ;
    for (const ChargedCell& c : cells) addZ(cellZ, c.z);
    for (const double zf : std::as_const(fieldZ)) {
        for (const double zs : std::as_const(sourceZ)) greens.want(zf, zs);
        for (const double zs : std::as_const(cellZ)) greens.want(zf, zs);
    }
    for (const double zf : std::as_const(cellZ))
        for (const double zs : std::as_const(sourceZ)) greens.want(zf, zs);
    if (!greens.build(stop, &out.problems)) {
        if (out.problems.isEmpty()) out.problems << tr("Stopped.");
        return out;
    }

    // The panels' system M Q = T q and what they put on the cells, P_cb:
    // Q_i / A_i - delta_i E_i(Q) = delta_i E_i(q).
    const int threads = std::max(1, QThread::idealThreadCount());
    Eigen::MatrixXd m(nb, nb), tq(nb, nc), pcb(nc, nb);
    onThreads(threads, [&](int which) {
        for (int i = which; i < nb; i += threads) {
            if (stop != nullptr && stop->load()) return;
            const Panel& pi = panels.at(i);
            const double h = stepOf(pi, step.at(pi.region));
            const double delta = plan.regions.at(pi.region).delta * kEps0;
            // The step for a source \a apart from it, \a size across: the
            // near one where it is near, else one sixteen times smaller than
            // the distance - along a wall any, across a face one of its two.
            const auto stepFor = [&](double apart, double size) {
                if (apart <= kFarSides * size) return h;
                if (pi.kind == Panel::Wall) return std::max(h, apart / 16);
                double best = h;
                for (const double f : farSteps.at(pi.region))
                    if (apart >= 16 * f) best = std::max(best, f);
                return best;
            };
            const auto apartFrom = [&pi](const Point3& c) {
                return std::sqrt((pi.centre[0] - c[0]) * (pi.centre[0] - c[0]) + (pi.centre[1] - c[1]) * (pi.centre[1] - c[1])
                                 + (pi.centre[2] - c[2]) * (pi.centre[2] - c[2]));
            };
            for (int j = 0; j < nb; ++j) {
                const Panel& pj = panels.at(j);
                const double hj = stepFor(apartFrom(pj.centre), pj.size);
                const std::array<Point3, 3> probes = probesOf(pi, hj);
                std::array<double, 3> phi{};
                for (int k = 0; k < 3; ++k) phi[std::size_t(k)] = panelPotential(pj, probes[std::size_t(k)], greens);
                m(i, j) = (i == j ? 1.0 / pi.area : 0.0) - delta * fieldOf(phi, hj);
            }
            for (int c = 0; c < nc; ++c) {
                const ChargedCell& cell = cells.at(c);
                const double hc = stepFor(apartFrom({cell.rect.center().x(), cell.rect.center().y(), cell.z}),
                                          std::max(cell.rect.width(), cell.rect.height()));
                const std::array<Point3, 3> probes = probesOf(pi, hc);
                std::array<double, 3> phi{};
                for (int k = 0; k < 3; ++k) phi[std::size_t(k)] = cellPotentialAt(cell, probes[std::size_t(k)], greens);
                tq(i, c) = delta * fieldOf(phi, hc);
            }
        }
    });
    onThreads(threads, [&](int which) {
        for (int c = which; c < nc; c += threads) {
            if (stop != nullptr && stop->load()) return;
            const ChargedCell& cell = cells.at(c);
            const Point3 at{cell.rect.center().x(), cell.rect.center().y(), cell.z};
            for (int j = 0; j < nb; ++j) pcb(c, j) = panelPotential(panels.at(j), at, greens);
        }
    });
    if (stop != nullptr && stop->load()) {
        out.problems << tr("Stopped.");
        return out;
    }
    const Eigen::PartialPivLU<Eigen::MatrixXd> lu(m);
    const Eigen::MatrixXd x = lu.solve(tq);   // nb x nc: the panels' charges of each cell's
    if (!x.allFinite()) {
        out.problems << tr("The ended dielectric's panels did not solve.");
        return out;
    }
    if (factored) {
        // Symmetric as U V^T: (P_cb X + X^T P_cb^T) / 2.
        out.u.resize(nc, 2 * nb);
        out.v.resize(nc, 2 * nb);
        out.u << pcb, 0.5 * x.transpose();
        out.v << 0.5 * x.transpose(), pcb;
    } else {
        const Eigen::MatrixXd d = pcb * x;
        out.delta = 0.5 * (d + d.transpose());
    }
    int walls = 0;
    for (const Panel& p : std::as_const(panels)) walls += p.kind == Panel::Wall;
    out.lines << tr("The ended dielectric's faces in %1 panel(s), %2 of them on its walls; %3 Green's function(s) of the background.")
                     .arg(nb).arg(walls).arg(greens.size());
    return out;
}

} // namespace qucs_s::rf25
