/*
 * rf25_edge.cpp - the current across a metal's edge, solved in its
 *                 cross-section: what a run along the edge loses
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_edge.h"

#include "rf25_static.h"

#include <Eigen/Dense>

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <tuple>

namespace qucs_s::rf25 {

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
/// How far in the strip runs before its mirror, in thicknesses: far enough
/// that an edge's band of a few thicknesses sees the edge's own crowding
/// alone, the other edge's past it.
constexpr double kReach = 20;

/// Lines from \a a to \a b: cells from \a first at \a a, each \a ratio
/// times the last up to \a cap, the last stretched or squeezed to end at b.
QList<double> graded(double a, double b, double first, double ratio, double cap)
{
    QList<double> lines{a};
    double w = first;
    while (lines.last() + w < b - 0.5 * std::min(w, cap)) {
        lines << lines.last() + w;
        w = std::min(w * ratio, cap);
    }
    lines << b;
    return lines;
}

/// F(X, Y), whose fourth mixed derivative d4F/dX2dY2 is ln r: a
/// rectangle pair's mean of ln r is it summed at the sixteen corner
/// differences (meanlog.py beside the write-up).
double meanLogTerm(double x, double y)
{
    const double r2 = x * x + y * y;
    const double lnr = r2 > 0 ? 0.5 * std::log(r2) : 0;
    const double ay = x != 0 ? std::atan(y / x) : 0, ax = y != 0 ? std::atan(x / y) : 0;
    const double x2 = x * x, y2 = y * y;
    return -(x2 * x2 - 6 * x2 * y2 + y2 * y2) / 24 * lnr + (x2 * x * y * ay + x * y2 * y * ax) / 6 - 25.0 / 48 * x2 * y2;
}

/// The mean of ln r between two axis-aligned rectangles: in closed form
/// near, by their centres and the second-order term ten sizes apart.
double meanLog(const QRectF& a, const QRectF& b)
{
    const double cx = a.center().x() - b.center().x(), cy = a.center().y() - b.center().y();
    const double size = std::max({a.width(), a.height(), b.width(), b.height()});
    const double d2 = cx * cx + cy * cy;
    if (d2 > 100 * size * size) {
        return 0.5 * std::log(d2)
               - (a.width() * a.width() - a.height() * a.height() + b.width() * b.width() - b.height() * b.height()) / 24
                     * (cx * cx - cy * cy) / (d2 * d2);
    }
    const double xa[2] = {a.left(), a.right()}, xb[2] = {b.left(), b.right()};
    const double ya[2] = {a.top(), a.bottom()}, yb[2] = {b.top(), b.bottom()};
    double sum = 0;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const double sx = (i + j) % 2 == 0 ? -1.0 : 1.0;   // (a's right less b's left +, ...)
            const double x = xa[i] - xb[j];
            for (int k = 0; k < 2; ++k)
                for (int l = 0; l < 2; ++l) {
                    const double sy = (k + l) % 2 == 0 ? -1.0 : 1.0;
                    sum += sx * sy * meanLogTerm(x, ya[k] - yb[l]);
                }
        }
    return sum / (a.width() * a.height() * b.width() * b.height());
}

} // namespace

EdgeSolution solveEdge(double sigma, double thickness, double frequency)
{
    EdgeSolution out;
    if (!(sigma > 0) || !(thickness > 0) || !(frequency > 0)) return out;
    out.sigma = sigma;
    out.thickness = thickness;
    out.frequency = frequency;
    const double omega = 2 * M_PI * frequency, half = thickness / 2;
    const double delta = std::sqrt(2 / (omega * kMu0 * sigma));
    out.reach = kReach * thickness;
    // Graded toward the wall (x = 0) and the face (y = t/2) to a fifth of
    // a skin depth, or a twentieth of the metal where that is finer.
    const double first = std::min(delta / 5, thickness / 20);
    const QList<double> xs = graded(0, out.reach, first, 1.25, thickness);
    QList<double> ys = graded(0, half, first, 1.25, thickness / 10);   // (from the face down)
    for (double& y : ys) y = half - y;
    std::reverse(ys.begin(), ys.end());
    for (int i = 0; i + 1 < xs.size(); ++i)
        for (int j = 0; j + 1 < ys.size(); ++j) out.cells << QRectF(QPointF(xs.at(i), ys.at(j)), QPointF(xs.at(i + 1), ys.at(j + 1)));
    const int n = int(out.cells.size());
    // Each rectangle's mean ln r to every other, and to its images: about
    // the middle (y -> -y) and about the strip's own middle (x -> 2 reach -
    // x), all carrying the same current.
    const auto mirroredX = [&out](const QRectF& r) { return QRectF(QPointF(2 * out.reach - r.right(), r.top()), QPointF(2 * out.reach - r.left(), r.bottom())); };
    const auto mirroredY = [](const QRectF& r) { return QRectF(QPointF(r.left(), -r.bottom()), QPointF(r.right(), -r.top())); };
    Eigen::MatrixXcd m(n + 1, n + 1);
    Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(n + 1);
    const std::complex<double> jwl(0, omega * kMu0 / (2 * M_PI));
    for (int i = 0; i < n; ++i) {
        const QRectF& a = out.cells.at(i);
        for (int j = 0; j < n; ++j) {
            const QRectF& b = out.cells.at(j);
            const double lnr = meanLog(a, b) + meanLog(a, mirroredX(b)) + meanLog(a, mirroredY(b)) + meanLog(a, mirroredX(mirroredY(b)));
            m(i, j) = -jwl * lnr;
        }
        m(i, i) += 1 / (sigma * a.width() * a.height());
        m(i, n) = -1;   // (the drop, alike across the metal)
        m(n, i) = 1;
    }
    m(n, n) = 0;
    rhs(n) = 0.25;   // (a quarter of a whole metal's ampere: one edge's top half)
    const Eigen::VectorXcd x = m.partialPivLu().solve(rhs);
    for (int i = 0; i < n; ++i) out.current << x(i);
    return out;
}

std::shared_ptr<const EdgeSolution> edgeSolution(double sigma, double thickness, double frequency)
{
    static QMutex mutex;
    static std::map<std::tuple<double, double, double>, std::shared_ptr<const EdgeSolution>> kept;
    const auto key = std::make_tuple(sigma, thickness, frequency);
    {
        const QMutexLocker lock(&mutex);
        const auto it = kept.find(key);
        if (it != kept.end()) return it->second;
    }
    auto made = std::make_shared<const EdgeSolution>(solveEdge(sigma, thickness, frequency));
    const QMutexLocker lock(&mutex);
    // (A sweep's metals and frequencies, kept while the program runs: a few
    //  hundred rectangles each, some kilobytes. Cleared past a thousand.)
    if (kept.size() > 1000) kept.clear();
    kept.emplace(key, made);
    return made;
}

EdgeBand bandOf(const EdgeSolution& edge, double from, double to)
{
    EdgeBand band;
    for (int i = 0; i < edge.cells.size(); ++i) {
        const QRectF& r = edge.cells.at(i);
        const double lo = std::max(r.left(), from), hi = std::min(r.right(), to);
        if (!(hi > lo)) continue;
        const double share = (hi - lo) / r.width();
        const std::complex<double> current = edge.current.at(i) * share;
        band.current += current;
        // (Its loss over its share of the rectangle's conductance: |I|^2
        //  share^2 / (sigma A share).)
        band.loss += std::norm(edge.current.at(i)) * share / (edge.sigma * r.width() * r.height());
    }
    return band;
}

QHash<int, QList<ChargePart>> wallCharges(const Thickened& thick, double micrometre, const std::function<double(int)>& zOf)
{
    QHash<int, QList<ChargePart>> out;
    if (!thick.any) return out;
    // The sides that face another conductor's wall across a narrow gap -
    // wallGapsOf()'s pairs, no wider than sixteen thicknesses: bit 0 the
    // side at the cell's least y, 1 its most y, 2 its least x, 3 its most
    // x; for both faces of each cell.
    QHash<int, int> facing;
    for (const WallGap& g : wallGapsOf(thick, micrometre)) {
        const QPointF ca = thick.mesh.cells.at(g.a).centre, cb = thick.mesh.cells.at(g.b).centre;
        const int sa = g.alongX ? (cb.y() > ca.y() ? 2 : 1) : (cb.x() > ca.x() ? 8 : 4);
        const int sb = g.alongX ? (sa == 2 ? 1 : 2) : (sa == 8 ? 4 : 8);
        for (const auto& [cell, side] : {std::pair(g.a, sa), std::pair(g.b, sb)}) {
            facing[cell] |= side;
            const int other = thick.topOf.value(cell, thick.bottomOf.value(cell, -1));
            if (other >= 0) facing[other] |= side;
        }
    }
    if (facing.isEmpty()) return out;
    QHash<QString, QList<int>> shapesOn;
    for (int s = 0; s < thick.conductors.size(); ++s) shapesOn[thick.conductors.at(s).layer] << s;
    for (auto it = facing.constBegin(); it != facing.constEnd(); ++it) {
        const int m = it.key(), sides = it.value();
        const int partner = thick.topOf.value(m, thick.bottomOf.value(m, -1));
        if (partner < 0) continue;
        const double z = zOf(m), other = zOf(partner), t = std::abs(z - other);
        if (!(t > 0)) continue;
        const double lo = z > other ? z - t / 2 : z, hi = z > other ? z : z + t / 2;
        const MeshCell& cell = thick.mesh.cells.at(m);
        const QRectF box = cell.outline.boundingRect();
        const QRectF r(box.left() * micrometre, box.top() * micrometre, box.width() * micrometre, box.height() * micrometre);
        const auto outside = [&](const QPointF& probe) {   // (the mesh's units)
            for (const int s : shapesOn.value(cell.layer)) {
                const Conductor& shape = thick.conductors.at(s);
                if (shape.outline.boundingRect().contains(probe) && shape.outline.containsPoint(probe, Qt::OddEvenFill)) return false;
            }
            return true;
        };
        const double ex = 5e-4 * box.width(), ey = 5e-4 * box.height();
        QList<ChargePart> parts{wholeCell(r, z)};
        parts.first().weight = r.width() * r.height();
        const auto wall = [&](int plane, double from, double to, double at, bool alongX) {
            ChargePart w;
            w.plane = plane;
            w.rect = QRectF(QPointF(from, lo), QPointF(to, hi));
            w.at = at;
            w.x = alongX ? 0.5 * (from + to) : at;
            w.y = alongX ? at : 0.5 * (from + to);
            w.z = 0.5 * (lo + hi);
            w.weight = (to - from) * (hi - lo);
            parts << w;
        };
        if ((sides & 1) && outside(QPointF(box.center().x(), box.top() - ey))) wall(1, r.left(), r.right(), r.top(), true);
        if ((sides & 2) && outside(QPointF(box.center().x(), box.bottom() + ey))) wall(1, r.left(), r.right(), r.bottom(), true);
        if ((sides & 4) && outside(QPointF(box.left() - ex, box.center().y()))) wall(2, r.top(), r.bottom(), r.left(), false);
        if ((sides & 8) && outside(QPointF(box.right() + ex, box.center().y()))) wall(2, r.top(), r.bottom(), r.right(), false);
        if (parts.size() == 1) continue;
        double total = 0;
        for (const ChargePart& p : std::as_const(parts)) total += p.weight;
        for (ChargePart& p : parts) p.weight /= total;
        out.insert(m, parts);
    }
    return out;
}

ChargePart wholeCell(const QRectF& rect, double z)
{
    ChargePart p;
    p.rect = rect;
    p.at = z;
    p.x = rect.center().x();
    p.y = rect.center().y();
    p.z = z;
    return p;
}

double partPotential(const ChargePart& q, double x, double y, double z)
{
    switch (q.plane) {
    case 1: return rectanglePotential(q.rect, QPointF(x, z), std::abs(y - q.at));
    case 2: return rectanglePotential(q.rect, QPointF(y, z), std::abs(x - q.at));
    default: return rectanglePotential(q.rect, QPointF(x, y), std::abs(z - q.at));
    }
}

double partsPotential(const QList<ChargePart>& at, const QList<ChargePart>& of)
{
    double sum = 0;
    for (const ChargePart& p : at)
        for (const ChargePart& q : of) sum += p.weight * q.weight * partPotential(q, p.x, p.y, p.z);
    return sum;
}

QList<ChargePart> movedBy(const QList<ChargePart>& parts, double dx, double dy)
{
    QList<ChargePart> out = parts;
    for (ChargePart& p : out) {
        p.x += dx;
        p.y += dy;
        if (p.plane == 0) p.rect.translate(dx, dy);
        else if (p.plane == 1) {
            p.rect.translate(dx, 0);
            p.at += dy;
        } else {
            p.rect.translate(dy, 0);
            p.at += dx;
        }
    }
    return out;
}

QHash<int, int> edgeSidesOf(const Mesh& mesh, const QList<Conductor>& conductors)
{
    QHash<QString, QList<int>> shapesOn;
    for (int s = 0; s < conductors.size(); ++s) shapesOn[conductors.at(s).layer] << s;
    QHash<int, int> out;
    for (int m = 0; m < mesh.cells.size(); ++m) {
        const MeshCell& cell = mesh.cells.at(m);
        const QRectF box = cell.outline.boundingRect();
        const auto outside = [&](const QPointF& probe) {
            for (const int s : shapesOn.value(cell.layer)) {
                const Conductor& shape = conductors.at(s);
                if (shape.outline.boundingRect().contains(probe) && shape.outline.containsPoint(probe, Qt::OddEvenFill)) return false;
            }
            return true;
        };
        const double ex = 5e-4 * box.width(), ey = 5e-4 * box.height();
        int sides = 0;
        if (outside(QPointF(box.center().x(), box.top() - ey))) sides |= 1;
        if (outside(QPointF(box.center().x(), box.bottom() + ey))) sides |= 2;
        if (outside(QPointF(box.left() - ex, box.center().y()))) sides |= 4;
        if (outside(QPointF(box.right() + ex, box.center().y()))) sides |= 8;
        if (sides != 0) out.insert(m, sides);
    }
    return out;
}

namespace {

/// How many sub-strips a part is cut into toward a free side.
constexpr int kProfileStrips = 4;

/// The cuts of [0, w] toward its free ends (\a low at 0, \a high at w),
/// graded as the square - a fourth of the cut's half at the first -, and
/// each piece's share of a density s^-alpha from each free end (of both
/// together where both are: s^-alpha (w - s)^-alpha), by the substitution
/// u = s^(1 - alpha) that takes the singularity out, at 16 points a piece.
QList<std::pair<std::pair<double, double>, double>> profile1Dimpl(double w, bool low, bool high, double alpha)
{
    QList<double> cuts;
    if (low && high) {
        for (int k = 0; k <= kProfileStrips; ++k) cuts << 0.5 * w * double(k * k) / (kProfileStrips * kProfileStrips);
        for (int k = kProfileStrips - 1; k >= 0; --k) cuts << w - 0.5 * w * double(k * k) / (kProfileStrips * kProfileStrips);
    } else {
        for (int k = 0; k <= kProfileStrips; ++k) cuts << w * double(k * k) / (kProfileStrips * kProfileStrips);
        if (high) {
            QList<double> mirrored;
            for (auto it = cuts.crbegin(); it != cuts.crend(); ++it) mirrored << w - *it;
            cuts = mirrored;
        }
    }
    const auto density = [&](double s) {
        double d = 1;
        if (low) d *= std::pow(std::max(s, 1e-300 * w), -alpha);
        if (high) d *= std::pow(std::max(w - s, 1e-300 * w), -alpha);
        return d;
    };
    QList<std::pair<std::pair<double, double>, double>> out;
    double total = 0;
    for (int k = 0; k + 1 < cuts.size(); ++k) {
        const double a = cuts.at(k), b = cuts.at(k + 1);
        if (!(b > a)) continue;
        // (Toward the nearer free end, s^-alpha ds = du/(1 - alpha) with
        //  u = s^(1 - alpha): the rest of the density is smooth in u.)
        const bool towardLow = low && (!high || a + b < w);
        const double e = 1 - alpha;
        const double s0 = towardLow ? a : w - b, s1 = towardLow ? b : w - a;   // (distances from that end)
        const double u0 = std::pow(s0, e), u1 = std::pow(s1, e);
        double sum = 0;
        for (int i = 0; i < 16; ++i) {
            const double u = u0 + (i + 0.5) / 16 * (u1 - u0);
            const double d = std::pow(u, 1 / e);   // (its distance from that end)
            const double s = towardLow ? d : w - d;
            sum += density(s) * std::pow(std::max(d, 1e-300 * w), alpha);   // (the singular factor taken out)
        }
        const double share = sum / 16 * (u1 - u0) / e;
        out.push_back({{a, b}, share});
        total += share;
    }
    for (auto& piece : out) piece.second /= total;
    return out;
}

} // namespace

QList<std::pair<std::pair<double, double>, double>> edgeProfile(double w, bool low, bool high, double alpha)
{
    return profile1Dimpl(w, low, high, alpha);
}

QList<ChargePart> profiled(const QList<ChargePart>& parts, int sides, double alpha)
{
    const bool lowY = sides & 1, highY = sides & 2, lowX = sides & 4, highX = sides & 8;
    if (!(lowY || highY || lowX || highX)) return parts;
    // (At a corner - free sides across x and across y -, each way's profile
    //  times the other's: d^-1/2 each would go as r^-1 at the corner, where
    //  a plate's right-angled corner holds its charge as r^-0.70; 0.35 each
    //  way goes as it does along the diagonal.)
    if ((lowY || highY) && (lowX || highX)) alpha = std::min(alpha, 0.35);
    QList<ChargePart> out;
    for (const ChargePart& p : parts) {
        if (p.plane != 0) {
            out << p;
            continue;
        }
        const auto xs = lowX || highX ? edgeProfile(p.rect.width(), lowX, highX, alpha)
                                      : QList<std::pair<std::pair<double, double>, double>>{{{0, p.rect.width()}, 1.0}};
        const auto ys = lowY || highY ? edgeProfile(p.rect.height(), lowY, highY, alpha)
                                      : QList<std::pair<std::pair<double, double>, double>>{{{0, p.rect.height()}, 1.0}};
        for (const auto& [xr, wx] : xs)
            for (const auto& [yr, wy] : ys) {
                ChargePart q = p;
                q.rect = QRectF(QPointF(p.rect.left() + xr.first, p.rect.top() + yr.first), QPointF(p.rect.left() + xr.second, p.rect.top() + yr.second));
                q.x = q.rect.center().x();
                q.y = q.rect.center().y();
                q.weight = p.weight * wx * wy;
                out << q;
            }
    }
    return out;
}

namespace {
std::atomic_bool& profilesOn()
{
    static std::atomic_bool on{true};
    return on;
}
} // namespace

void setEdgeProfiles(bool on)
{
    profilesOn().store(on);
}

bool edgeProfiles()
{
    return profilesOn().load();
}

QHash<int, QList<ChargePart>> chargeParts(const Thickened& thick, double micrometre, const std::function<double(int)>& zOf)
{
    QHash<int, QList<ChargePart>> out = wallCharges(thick, micrometre, zOf);
    if (!edgeProfiles()) return out;
    const QHash<int, int> sides = edgeSidesOf(thick.mesh, thick.conductors);
    // (An edge with another conductor over or under it nearer than its
    //  cell is wide holds its charge as plates facing do - nearly even, the
    //  field between them -, not as an edge with nothing near: no profile
    //  there. Two plates 2 µm apart read under their parallel plate's
    //  capacitance with one.)
    QHash<QString, double> heightOf;
    for (int m = 0; m < thick.mesh.cells.size(); ++m)
        if (!heightOf.contains(thick.mesh.cells.at(m).layer)) heightOf.insert(thick.mesh.cells.at(m).layer, zOf(m));
    const auto facedNear = [&](int m) {
        const MeshCell& cell = thick.mesh.cells.at(m);
        const QRectF box = cell.outline.boundingRect();
        const double size = std::max(box.width(), box.height()) * micrometre, z = zOf(m);
        // (Its metal's own other face is no other conductor.)
        const int partner = thick.topOf.value(m, thick.bottomOf.value(m, -1));
        const QString own = partner >= 0 ? thick.mesh.cells.at(partner).layer : QString();
        for (const Conductor& c : thick.conductors) {
            if (c.layer == cell.layer || c.layer == own) continue;
            // (Planes alone: a via's, or the walls joining a metal's faces,
            //  face no edge - the full wave set the walls at the bottom
            //  face's height, a top face's edge then facing them.)
            const Layer* layer = thick.stack.named(c.layer);
            if (layer == nullptr || layer->kind != Layer::Metal) continue;
            const double dz = std::abs(heightOf.value(c.layer, z + 2 * size) - z);
            if (dz > 0 && dz < size && c.outline.boundingRect().intersects(box)) return true;
        }
        return false;
    };
    for (auto it = sides.constBegin(); it != sides.constEnd(); ++it) {
        const int m = it.key();
        if (facedNear(m)) continue;
        QList<ChargePart> parts = out.value(m);
        if (parts.isEmpty()) {
            const QRectF box = thick.mesh.cells.at(m).outline.boundingRect();
            parts << wholeCell(QRectF(box.left() * micrometre, box.top() * micrometre, box.width() * micrometre, box.height() * micrometre), zOf(m));
        }
        out.insert(m, profiled(parts, it.value(), 0.5));
    }
    return out;
}

} // namespace qucs_s::rf25
