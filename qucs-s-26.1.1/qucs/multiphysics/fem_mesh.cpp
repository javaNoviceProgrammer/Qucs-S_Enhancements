/*
 * fem_mesh.cpp - a multiphysics model's mesh, on CDT
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_mesh.h"

#include <CDT.h>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Mesh", text);
}

constexpr double Infinity = std::numeric_limits<double>::infinity();

double cross(QPointF a, QPointF b)
{
    return a.x() * b.y() - a.y() * b.x();
}

double norm(QPointF a)
{
    return std::hypot(a.x(), a.y());
}

/// The presets: the largest and smallest element as parts of the
/// geometry's size, the growth rate, the curvature factor, the elements
/// across a narrow region.
struct Preset {
    const char* name;
    double hmax, hmin, growth, curvature, narrow;
};

const Preset Presets[] = {
    {"extremely fine", 0.01, 1e-5, 1.1, 0.2, 2},
    {"extra fine", 0.02, 3e-5, 1.15, 0.25, 1.5},
    {"finer", 0.035, 1e-4, 1.2, 0.25, 1},
    {"fine", 0.05, 2e-4, 1.25, 0.3, 1},
    {"normal", 0.07, 3e-4, 1.3, 0.3, 1},
    {"coarse", 0.1, 5e-4, 1.4, 0.4, 0.8},
    {"coarser", 0.15, 1e-3, 1.5, 0.5, 0.6},
    {"extra coarse", 0.25, 2e-3, 1.8, 0.6, 0.5},
    {"extremely coarse", 0.4, 5e-3, 2, 0.8, 0.3},
};

/// Size sources (points of the boundaries with the size there), and the
/// size anywhere: the least of each one's size grown by the rate over the
/// distance - found in a k-d tree, branches whose least cannot win cut.
class SizeTree
{
public:
    struct Source {
        double x, y, h;
    };
    void build(std::vector<Source> sources)
    {
        a_src = std::move(sources);
        a_nodes.clear();
        if (!a_src.empty()) buildNode(0, int(a_src.size()));
    }
    /// min(best, min over sources of h + k |p - s|).
    double query(QPointF p, double k, double best) const
    {
        if (a_nodes.empty()) return best;
        int stack[128];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const KNode& n = a_nodes[std::size_t(stack[--sp])];
            const double dx = std::max({n.x0 - p.x(), 0.0, p.x() - n.x1});
            const double dy = std::max({n.y0 - p.y(), 0.0, p.y() - n.y1});
            if (n.minH + k * std::hypot(dx, dy) >= best) continue;
            if (n.left < 0) {
                for (int i = n.begin; i < n.end; ++i) {
                    const Source& s = a_src[std::size_t(i)];
                    best = std::min(best, s.h + k * std::hypot(s.x - p.x(), s.y - p.y()));
                }
                continue;
            }
            if (sp + 2 > 128) continue;
            // The nearer child last: searched first.
            const KNode& l = a_nodes[std::size_t(n.left)];
            const double cl = std::hypot(std::max({l.x0 - p.x(), 0.0, p.x() - l.x1}), std::max({l.y0 - p.y(), 0.0, p.y() - l.y1}));
            const KNode& r = a_nodes[std::size_t(n.right)];
            const double cr = std::hypot(std::max({r.x0 - p.x(), 0.0, p.x() - r.x1}), std::max({r.y0 - p.y(), 0.0, p.y() - r.y1}));
            if (cl < cr) {
                stack[sp++] = n.right;
                stack[sp++] = n.left;
            } else {
                stack[sp++] = n.left;
                stack[sp++] = n.right;
            }
        }
        return best;
    }

private:
    struct KNode {
        double x0, y0, x1, y1, minH;
        int begin, end, left = -1, right = -1;
    };
    int buildNode(int begin, int end)
    {
        KNode n{Infinity, Infinity, -Infinity, -Infinity, Infinity, begin, end};
        for (int i = begin; i < end; ++i) {
            const Source& s = a_src[std::size_t(i)];
            n.x0 = std::min(n.x0, s.x);
            n.y0 = std::min(n.y0, s.y);
            n.x1 = std::max(n.x1, s.x);
            n.y1 = std::max(n.y1, s.y);
            n.minH = std::min(n.minH, s.h);
        }
        const int index = int(a_nodes.size());
        a_nodes.push_back(n);
        if (end - begin > 8) {
            const int mid = (begin + end) / 2;
            const bool byX = n.x1 - n.x0 >= n.y1 - n.y0;
            std::nth_element(a_src.begin() + begin, a_src.begin() + mid, a_src.begin() + end,
                             [byX](const Source& a, const Source& b) { return byX ? a.x < b.x : a.y < b.y; });
            const int l = buildNode(begin, mid);
            const int r = buildNode(mid, end);
            a_nodes[std::size_t(index)].left = l;
            a_nodes[std::size_t(index)].right = r;
        }
        return index;
    }
    std::vector<Source> a_src;
    std::vector<KNode> a_nodes;
};

/// A boundary walked by its length: on its curve when it is one.
class BoundaryPath
{
public:
    BoundaryPath(const Topology& t, const Topology::Boundary& b)
    {
        if (b.curve >= 0) {
            a_curve = &t.curves[std::size_t(b.curve)];
            if (a_curve->kind == Curve::Polyline) {
                // At its own points between the ends, either way.
                const double dir = b.t1 >= b.t0 ? 1 : -1;
                a_t.push_back(b.t0);
                for (double tt = dir > 0 ? std::floor(b.t0) + 1 : std::ceil(b.t0) - 1; dir > 0 ? tt < b.t1 - 1e-9 : tt > b.t1 + 1e-9; tt += dir)
                    if (std::abs(tt - b.t0) > 1e-9) a_t.push_back(tt);
                a_t.push_back(b.t1);
                for (double tt : a_t) a_pts.push_back(a_curve->at(tt));
            } else {
                const int n = a_curve->kind == Curve::Line ? 1 : std::max(16, int(std::ceil(std::abs(b.t1 - b.t0) / (M_PI / 720))));
                for (int i = 0; i <= n; ++i) {
                    const double tt = b.t0 + (b.t1 - b.t0) * i / n;
                    a_t.push_back(tt);
                    a_pts.push_back(a_curve->at(tt));
                }
            }
            // Its ends exactly at the topology's vertices.
            a_pts.front() = t.vertices[std::size_t(b.start)];
            a_pts.back() = t.vertices[std::size_t(b.end)];
        } else {
            a_pts = b.polyline(t);
        }
        a_cum.push_back(0);
        for (std::size_t i = 1; i < a_pts.size(); ++i) a_cum.push_back(a_cum.back() + norm(a_pts[i] - a_pts[i - 1]));
    }
    double length() const { return a_cum.back(); }
    bool onCurve() const { return a_curve != nullptr; }
    /// The point at \a s along it, and the curve's parameter there.
    QPointF at(double s, double* t = nullptr) const
    {
        const std::size_t i = segment(s);
        const double len = a_cum[i + 1] - a_cum[i];
        const double u = len > 0 ? std::clamp((s - a_cum[i]) / len, 0.0, 1.0) : 0;
        if (a_curve) {
            const double tt = a_t[i] + u * (a_t[i + 1] - a_t[i]);
            if (t) *t = tt;
            return a_curve->at(tt);
        }
        if (t) *t = 0;
        return a_pts[i] + (a_pts[i + 1] - a_pts[i]) * u;
    }
    QPointF tangent(double s) const
    {
        const std::size_t i = segment(s);
        const QPointF d = a_pts[i + 1] - a_pts[i];
        const double n = norm(d);
        return n > 0 ? d / n : QPointF(1, 0);
    }
    double radius(double s) const
    {
        if (!a_curve || a_curve->kind == Curve::Line) return Infinity;
        double t = 0;
        at(s, &t);
        return a_curve->radiusAt(t);
    }

private:
    std::size_t segment(double s) const
    {
        const auto it = std::upper_bound(a_cum.begin(), a_cum.end(), s);
        std::size_t i = it == a_cum.begin() ? 0 : std::size_t(it - a_cum.begin()) - 1;
        return std::min(i, a_cum.size() - 2);
    }
    const Curve* a_curve = nullptr;
    std::vector<QPointF> a_pts;
    std::vector<double> a_t;
    std::vector<double> a_cum;
};

/// The topology's segments on a grid: how far a ray goes before it meets
/// another boundary (the width of a narrow region).
class SegmentGrid
{
public:
    SegmentGrid(const Topology& t) : a_t(t)
    {
        a_box = t.bounds;
        const int n = int(t.segments.size());
        a_n = std::clamp(int(std::sqrt(double(n))), 1, 256);
        a_cw = std::max(a_box.width() / a_n, 1e-300);
        a_ch = std::max(a_box.height() / a_n, 1e-300);
        a_cells.resize(std::size_t(a_n * a_n));
        for (int s = 0; s < n; ++s) {
            const Topology::Segment& seg = t.segments[std::size_t(s)];
            if (seg.boundary < 0) continue;
            const QPointF a = t.vertices[std::size_t(seg.v0)], b = t.vertices[std::size_t(seg.v1)];
            const auto [i0, j0] = cell(std::min(a.x(), b.x()), std::min(a.y(), b.y()));
            const auto [i1, j1] = cell(std::max(a.x(), b.x()), std::max(a.y(), b.y()));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) a_cells[std::size_t(j * a_n + i)].push_back(s);
        }
        a_stamp.assign(t.segments.size(), 0);
    }
    /// Along \a dir from \a p, to \a reach at most: the distance to the
    /// first segment of a boundary \a skip does not exclude.
    double ray(QPointF p, QPointF dir, double reach, const std::function<bool(int)>& skip)
    {
        ++a_counter;
        const QPointF q = p + dir * reach;
        const auto [i0, j0] = cell(std::min(p.x(), q.x()), std::min(p.y(), q.y()));
        const auto [i1, j1] = cell(std::max(p.x(), q.x()), std::max(p.y(), q.y()));
        double best = reach;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                for (int s : a_cells[std::size_t(j * a_n + i)]) {
                    if (a_stamp[std::size_t(s)] == a_counter) continue;
                    a_stamp[std::size_t(s)] = a_counter;
                    const Topology::Segment& seg = a_t.segments[std::size_t(s)];
                    if (skip(seg.boundary)) continue;
                    const QPointF a = a_t.vertices[std::size_t(seg.v0)], b = a_t.vertices[std::size_t(seg.v1)];
                    const QPointF e = b - a;
                    const double den = cross(dir, e);
                    if (std::abs(den) < 1e-300) continue;
                    const double dist = cross(a - p, e) / den;
                    const double u = cross(a - p, dir) / den;
                    if (u < 0 || u > 1 || dist <= 0) continue;
                    best = std::min(best, dist);
                }
        return best;
    }

private:
    std::pair<int, int> cell(double x, double y) const
    {
        return {std::clamp(int((x - a_box.left()) / a_cw), 0, a_n - 1), std::clamp(int((y - a_box.top()) / a_ch), 0, a_n - 1)};
    }
    const Topology& a_t;
    QRectF a_box;
    int a_n = 1;
    double a_cw = 1, a_ch = 1;
    std::vector<std::vector<int>> a_cells;
    std::vector<unsigned> a_stamp;
    unsigned a_counter = 0;
};

struct ConstraintEdge {
    int a, b;          // nodes, in the boundary's direction
    int boundary;
    int curve;
    double ta, tb;
};

class Mesher
{
public:
    Mesher(std::shared_ptr<const Topology> topology, const MeshSizes& sizes, const Progress& progress)
        : a_topo(std::move(topology)), a_t(*a_topo), a_sizes(sizes), a_progress(progress)
    {
        a_k = std::max(0.0, sizes.growth - 1);
    }

    MeshBuild run()
    {
        MeshBuild out;
        if (a_t.domains.empty()) {
            out.error = tr("the geometry has no domains to mesh");
            return out;
        }
        if (!(a_sizes.hmax > 0)) {
            out.error = tr("the maximum element size must be more than 0");
            return out;
        }
        if (!step(0.02, tr("Sizes"))) return cancelled();
        sizeSources();
        layerColumns();
        if (!step(0.08, tr("Boundaries"))) return cancelled();
        discretize();
        if (!a_error.isEmpty()) {
            out.error = a_error;
            return out;
        }
        std::shared_ptr<Mesh> mesh;
        try {
            mesh = triangulate();
        } catch (const std::exception& e) {
            out.error = tr("the mesher failed: %1").arg(QString::fromLocal8Bit(e.what()));
            return out;
        }
        if (a_cancelled) return cancelled();
        if (!mesh) {
            out.error = a_error.isEmpty() ? tr("the mesher failed") : a_error;
            return out;
        }
        out.mesh = mesh;
        out.warnings = a_warnings;
        return out;
    }

private:
    MeshBuild cancelled()
    {
        MeshBuild out;
        out.error = tr("cancelled");
        return out;
    }
    bool step(double fraction, const QString& what)
    {
        if (a_progress && !a_progress(fraction, what)) a_cancelled = true;
        return !a_cancelled;
    }
    double domainCap(int d) const
    {
        double h = a_sizes.hmax;
        if (d >= 0 && d < int(a_sizes.domainMax.size())) h = std::min(h, a_sizes.domainMax[std::size_t(d)]);
        return h;
    }
    /// The size wanted at \a p in domain \a d.
    double size(QPointF p, int d) const
    {
        const double cap = domainCap(d);
        return std::max(a_sizes.hmin, a_tree.query(p, a_k, cap));
    }
    /// A layered region's domain; a boundary of one.
    bool layered(int d) const { return d >= 0 && d < int(a_sizes.layerLateral.size()) && a_sizes.layerLateral[std::size_t(d)] > 0; }
    bool layeredBoundary(int b) const
    {
        const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
        return layered(bd.up) || layered(bd.down);
    }
    /// Whether boundary \a b runs along the layers of layered domain \a d.
    bool alongLayers(int b, int d) const
    {
        const std::vector<QPointF> line = a_t.boundaries[std::size_t(b)].polyline(a_t);
        if (line.size() < 2 || !layered(d)) return false;
        const QPointF along = line.back() - line.front();
        return a_sizes.layerAxis[std::size_t(d)] == 1 ? std::abs(along.x()) > std::abs(along.y()) : std::abs(along.y()) > std::abs(along.x());
    }
    /// A coordinate along the layers of domain \a d (x when they are stacked along y).
    double lateralOf(QPointF p, int d) const { return a_sizes.layerAxis[std::size_t(d)] == 1 ? p.x() : p.y(); }

    /// Each layered domain's columns: where along its layers its grid's
    /// lines are (its ends among them). A stack - layered domains sharing a
    /// boundary along their layers - has one set: its breakpoints (its
    /// domains' ends, its boundaries' ends - where a layer is split, an
    /// aperture's edge) and between each two ceil(gap / its lateral size)
    /// columns alike; so every layer and every boundary along them has the
    /// same lines, the grid's cells quadrilaterals. (Each layer's own width
    /// divided apart had them disagree where a layer was split: a 2 um
    /// aperture in a 5 um stack at 0.06 um came to 34 + 50 columns in one
    /// layer and 84 in the next - slivers, a singular matrix.)
    void layerColumns()
    {
        const int nd = int(a_t.domains.size());
        a_columns.assign(std::size_t(nd), {});
        std::vector<int> parent(static_cast<std::size_t>(nd));
        for (int d = 0; d < nd; ++d) parent[std::size_t(d)] = d;
        auto find = [&parent](int d) {
            while (parent[std::size_t(d)] != d) d = parent[std::size_t(d)] = parent[std::size_t(parent[std::size_t(d)])];
            return d;
        };
        const int nb = int(a_t.boundaries.size());
        for (int b = 0; b < nb; ++b) {
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
            if (layered(bd.up) && layered(bd.down) && a_sizes.layerAxis[std::size_t(bd.up)] == a_sizes.layerAxis[std::size_t(bd.down)]
                && alongLayers(b, bd.up))
                parent[std::size_t(find(bd.up))] = find(bd.down);
        }
        std::map<int, std::vector<double>> cuts;
        std::map<int, double> step, wallStep;
        double extent = 1e-300;
        for (int d = 0; d < nd; ++d) {
            if (!layered(d)) continue;
            const QRectF r = a_t.domains[std::size_t(d)].bounds;
            const bool yStack = a_sizes.layerAxis[std::size_t(d)] == 1;
            const int root = find(d);
            cuts[root].push_back(yStack ? r.left() : r.top());
            cuts[root].push_back(yStack ? r.right() : r.bottom());
            auto it = step.find(root);
            step[root] = it == step.end() ? a_sizes.layerLateral[std::size_t(d)] : std::min(it->second, a_sizes.layerLateral[std::size_t(d)]);
            const double given = d < int(a_sizes.layerWall.size()) ? a_sizes.layerWall[std::size_t(d)] : 0;
            const double wall = given > 0 ? given : a_sizes.layerLateral[std::size_t(d)] / 32;
            auto w = wallStep.find(root);
            wallStep[root] = w == wallStep.end() ? wall : std::min(w->second, wall);
            extent = std::max({extent, std::abs(r.left()), std::abs(r.right()), std::abs(r.top()), std::abs(r.bottom())});
        }
        if (cuts.empty()) return;
        for (int b = 0; b < nb; ++b) {
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
            for (int d : {bd.up, bd.down})
                if (alongLayers(b, d))
                    for (int v : {bd.start, bd.end}) cuts[find(d)].push_back(lateralOf(a_t.vertices[std::size_t(v)], d));
        }
        const double tol = 1e-9 * extent;
        // Its walls: a material's sides across its layers - between two of
        // a semiconductor's domains of different materials: a
        // heterojunction's side wall, an oxide aperture's edge. The columns
        // there from the wall's size, each the next twice the last, to the
        // lateral size. (Even, a node on a heterojunction's wall held its
        // part of the material beside it at its own carriers - half a column
        // of it, current from the cladding under that part taken as into the
        // wall: a GaAs ring in Al0.6Ga0.4As 300 nm wide passed 326, 259,
        // 230, 209 nA with columns 200, 100, 50 and 25 nm wide, some 190 in
        // the limit; graded so, 195, 192, 192 nA, and as a laser 29.8, 29.6,
        // 29.5 µA where it was 57.8, 41.8, 35.6. The current crowds at an
        // aperture's edge: the VCSEL example at 1.9 V 1.418, 1.398, 1.388 mA
        // on even columns 100, 50 and 25 nm wide, graded 1.382 and 1.383.)
        std::map<int, std::vector<double>> walls;
        for (int b = 0; b < nb; ++b) {
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
            if (b >= int(a_sizes.layerWalls.size()) || !a_sizes.layerWalls[std::size_t(b)] || !layered(bd.up) || !layered(bd.down)) continue;
            if (alongLayers(b, bd.up) || find(bd.up) != find(bd.down)) continue;
            walls[find(bd.up)].push_back(lateralOf(a_t.vertices[std::size_t(bd.start)], bd.up));
        }
        std::map<int, std::vector<double>> lines;
        for (auto& [root, c] : cuts) {
            std::sort(c.begin(), c.end());
            std::vector<double> u;
            for (double x : c)
                if (u.empty() || x - u.back() > tol) u.push_back(x);
            const double h = step[root], h0 = std::min(h, wallStep[root]);
            const auto isWall = [&walls, root = root, tol](double x) {
                return std::any_of(walls[root].cbegin(), walls[root].cend(), [x, tol](double w) { return std::abs(w - x) <= tol; });
            };
            std::vector<double>& out = lines[root];
            for (std::size_t i = 0; i + 1 < u.size(); ++i) {
                out.push_back(u[i]);
                const double a = u[i], b = u[i + 1], gap = b - a;
                const bool fromA = h0 < h && isWall(a), fromB = h0 < h && isWall(b);
                // The graded columns from each wall end (half the gap each
                // when both are walls), each leaving at least as much again
                // after it - the rest no column narrower than the last.
                std::vector<double> nearA, nearB;
                for (const auto& [from, near] : {std::pair{fromA, &nearA}, std::pair{fromB, &nearB}}) {
                    if (!from) continue;
                    const double room = fromA && fromB ? gap / 2 : gap;
                    double size = h0, used = 0;
                    while (size < h && used + 2 * size <= room + tol) {
                        near->push_back(size);
                        used += size;
                        size *= 2;
                    }
                }
                double usedA = 0, usedB = 0;
                for (double size : nearA) usedA += size;
                for (double size : nearB) usedB += size;
                std::vector<double> points;
                double x = a;
                for (double size : nearA) points.push_back(x += size);
                // The rest even, no column wider than the lateral size.
                const double from = a + usedA, to = b - usedB, rest = to - from;
                if (rest > tol) {
                    const int n = std::clamp(int(std::ceil(rest / h - 1e-9)), 1, 100000);
                    for (int k = 1; k < n; ++k) points.push_back(from + rest * k / n);
                    points.push_back(to);
                }
                double y = b;
                for (double size : nearB) points.push_back(y -= size);
                std::sort(points.begin(), points.end());
                for (double p : points)
                    if (p > out.back() + tol && p < b - tol) out.push_back(p);
            }
            out.push_back(u.back());
        }
        for (int d = 0; d < nd; ++d) {
            if (!layered(d)) continue;
            const QRectF r = a_t.domains[std::size_t(d)].bounds;
            const bool yStack = a_sizes.layerAxis[std::size_t(d)] == 1;
            const double lo = yStack ? r.left() : r.top(), hi = yStack ? r.right() : r.bottom();
            for (double x : lines[find(d)])
                if (x > lo - tol && x < hi + tol) a_columns[std::size_t(d)].push_back(x);
        }
    }

    /// On a boundary: the finer of its two sides'.
    double boundarySize(QPointF p, const Topology::Boundary& b) const
    {
        double h = Infinity;
        for (int d : {b.up, b.down})
            if (d >= 0) h = std::min(h, size(p, d));
        return h == Infinity ? size(p, -1) : h;
    }

    void sizeSources()
    {
        // A boundary's own size: the least of all, its own, its domains'.
        const int nb = int(a_t.boundaries.size());
        std::vector<double> own(std::size_t(nb), a_sizes.hmax);
        for (int b = 0; b < nb; ++b) {
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
            double h = a_sizes.hmax;
            if (b < int(a_sizes.boundaryMax.size())) h = std::min(h, a_sizes.boundaryMax[std::size_t(b)]);
            for (int d : {bd.up, bd.down})
                if (d >= 0) h = std::min(h, domainCap(d));
            // (A layered region's: its elements' spacing along it.)
            if (layeredBoundary(b) && b < int(a_sizes.boundaryCount.size()) && a_sizes.boundaryCount[std::size_t(b)] > 0)
                h = BoundaryPath(a_t, bd).length() / a_sizes.boundaryCount[std::size_t(b)];
            own[std::size_t(b)] = layeredBoundary(b) ? h : std::max(h, a_sizes.hmin);
        }
        // Boundaries that meet another at a point: not across a narrow
        // region from it.
        std::vector<QSet<int>> touching(static_cast<std::size_t>(nb));
        for (const Topology::Point& p : a_t.points)
            for (int a : p.boundaries)
                for (int b : p.boundaries) touching[std::size_t(a)].insert(b);
        SegmentGrid grid(a_t);
        auto sample = [&](int b, double spacing, std::vector<SizeTree::Source>& out, double* least) {
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
            const BoundaryPath path(a_t, bd);
            const double len = path.length();
            const int n = std::clamp(int(std::ceil(len / spacing)), 1, 20000);
            *least = Infinity;
            for (int i = 0; i <= n; ++i) {
                const double s = len * i / n;
                const QPointF p = path.at(s);
                double h = own[std::size_t(b)];
                // On a curve: the curvature factor times its radius.
                h = std::min(h, a_sizes.curvature * path.radius(s));
                // Across a narrow region: its width over the elements wanted
                // (a layered region's layers not: their grid says).
                if (a_sizes.narrow > 0 && !layeredBoundary(b)) {
                    const QPointF tg = path.tangent(s);
                    const QPointF nrm(-tg.y(), tg.x());
                    const double reach = h * std::max(1.0, a_sizes.narrow) * 1.01;
                    auto skip = [&](int other) { return other < 0 || touching[std::size_t(b)].contains(other); };
                    // (not from its very ends: they meet the next boundary)
                    const QPointF inset = (i == 0 ? tg : i == n ? -tg : QPointF()) * (len * 1e-6);
                    const double gap = std::min(grid.ray(p + inset, nrm, reach, skip), grid.ray(p + inset, -nrm, reach, skip));
                    if (gap < reach) h = std::min(h, gap / a_sizes.narrow);
                }
                if (!layeredBoundary(b)) h = std::max(h, a_sizes.hmin);
                *least = std::min(*least, h);
                out.push_back({p.x(), p.y(), h});
            }
        };
        std::vector<SizeTree::Source> sources;
        for (int b = 0; b < nb; ++b) {
            // Sampled at half the boundary's size; where it came out finer
            // (a curve, a narrow region), again at half of that.
            std::vector<SizeTree::Source> first;
            double least = Infinity;
            const double len = BoundaryPath(a_t, a_t.boundaries[std::size_t(b)]).length();
            sample(b, std::max(own[std::size_t(b)] / 2, len / 20000), first, &least);
            if (least < own[std::size_t(b)] * 0.75) {
                first.clear();
                sample(b, std::max(least / 2, len / 20000), first, &least);
            }
            sources.insert(sources.end(), first.begin(), first.end());
        }
        for (const auto& q : a_sizes.points) sources.push_back({q[0], q[1], std::max(q[2], a_sizes.hmin)});
        for (int p = 0; p < int(a_t.points.size()) && p < int(a_sizes.pointMax.size()); ++p)
            if (a_sizes.pointMax[std::size_t(p)] < Infinity) {
                const QPointF q = a_t.vertices[std::size_t(a_t.points[std::size_t(p)].vertex)];
                sources.push_back({q.x(), q.y(), std::max(a_sizes.pointMax[std::size_t(p)], a_sizes.hmin)});
            }
        a_tree.build(std::move(sources));
    }

    void discretize()
    {
        // The topology's vertices at boundaries' ends and the points: nodes.
        auto nodeOf = [&](int vertex) {
            auto it = a_vertexNode.constFind(vertex);
            if (it != a_vertexNode.cend()) return *it;
            a_nodes.push_back(a_t.vertices[std::size_t(vertex)]);
            const int n = int(a_nodes.size()) - 1;
            a_vertexNode.insert(vertex, n);
            return n;
        };
        for (const Topology::Point& p : a_t.points) nodeOf(p.vertex);
        // Each boundary's nodes between its ends: where along it (a length
        // from its start).
        const int nb = int(a_t.boundaries.size());
        std::vector<std::vector<double>> where(static_cast<std::size_t>(nb));
        for (int b = 0; b < nb; ++b) {
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
            const BoundaryPath path(a_t, bd);
            const double len = path.length();
            int n = b < int(a_sizes.boundaryCount.size()) ? a_sizes.boundaryCount[std::size_t(b)] : 0;
            std::vector<double> sAt, fAt;   // the density's integral along it
            if (n <= 0) {
                double s = 0, f = 0;
                sAt.push_back(0);
                fAt.push_back(0);
                double h = boundarySize(path.at(0), bd);
                for (int guard = 0; s < len && guard < 2000000; ++guard) {
                    const double ds = std::min(std::max(h / 4, len * 1e-6), len - s);
                    const double h2 = boundarySize(path.at(s + ds), bd);
                    f += ds * 0.5 * (1 / h + 1 / h2);
                    s += ds;
                    h = h2;
                    sAt.push_back(s);
                    fAt.push_back(f);
                }
                n = std::max(1, int(std::lround(f)));
            }
            // A curve, or a boundary not straight, in two pieces at least
            // (two boundaries between the same two points are not one edge);
            // a closed one in three.
            if (path.onCurve() && a_t.curves[std::size_t(bd.curve)].kind != Curve::Line) n = std::max(n, 2);
            if (!path.onCurve()) n = std::max(n, 2);
            if (bd.start == bd.end) n = std::max(n, 3);
            for (int i = 1; i < n; ++i) {
                double s = len * i / n;
                if (!fAt.empty()) {
                    // Where the density's integral reaches i/n of its whole.
                    const double target = fAt.back() * i / n;
                    const auto it = std::lower_bound(fAt.begin(), fAt.end(), target);
                    const std::size_t k = std::clamp<std::size_t>(std::size_t(it - fAt.begin()), 1, fAt.size() - 1);
                    const double f0 = fAt[k - 1], f1 = fAt[k];
                    const double u = f1 > f0 ? (target - f0) / (f1 - f0) : 0;
                    s = sAt[k - 1] + u * (sAt[k] - sAt[k - 1]);
                }
                where[std::size_t(b)].push_back(s);
            }
            // Along a layered region's layers: at its stack's columns.
            for (int d : {bd.up, bd.down}) {
                if (!alongLayers(b, d) || a_columns[std::size_t(d)].size() < 2) continue;
                const double from = lateralOf(a_t.vertices[std::size_t(bd.start)], d), to = lateralOf(a_t.vertices[std::size_t(bd.end)], d);
                const double tol = 1e-9 * std::max(1e-300, len);
                std::vector<double> at;
                for (double x : a_columns[std::size_t(d)])
                    if (x > std::min(from, to) + tol && x < std::max(from, to) - tol) at.push_back(std::abs(x - from));
                std::sort(at.begin(), at.end());
                if (!at.empty() || path.onCurve()) where[std::size_t(b)] = at;
                break;
            }
        }
        // A periodic condition's two sides meshed alike: each pair of
        // boundaries, one the other moved, at the nodes of both.
        for (int d = 0; d < nb && d < int(a_sizes.copyOf.size()); ++d) {
            const int src = a_sizes.copyOf[std::size_t(d)];
            if (src < 0 || src >= nb || src == d) continue;
            const bool turned = a_sizes.copyReversed[std::size_t(d)];
            const double lenD = BoundaryPath(a_t, a_t.boundaries[std::size_t(d)]).length();
            const double lenS = BoundaryPath(a_t, a_t.boundaries[std::size_t(src)]).length();
            if (!(lenD > 0) || !(lenS > 0)) continue;
            // (as fractions of their lengths, the source's way)
            std::vector<double> own = where[std::size_t(d)], other = where[std::size_t(src)];
            for (double& f : own) f = turned ? 1 - f / lenD : f / lenD;
            for (double& f : other) f /= lenS;
            // Each fraction with the spacing about it in its own list, the
            // two merged: one closer to the last than 0.3 of that left out.
            auto spaced = [](const std::vector<double>& fs) {
                std::vector<std::pair<double, double>> out;
                std::vector<double> sorted = fs;
                std::sort(sorted.begin(), sorted.end());
                for (std::size_t i = 0; i < sorted.size(); ++i) {
                    const double before = sorted[i] - (i > 0 ? sorted[i - 1] : 0.0);
                    const double after = (i + 1 < sorted.size() ? sorted[i + 1] : 1.0) - sorted[i];
                    out.push_back({sorted[i], std::min(before, after)});
                }
                return out;
            };
            std::vector<std::pair<double, double>> all = spaced(own), more = spaced(other);
            all.insert(all.end(), more.begin(), more.end());
            std::sort(all.begin(), all.end());
            std::vector<double> merged;
            double lastAt = 0, lastGap = 1;
            for (const auto& [f, gap] : all) {
                if (f - lastAt < 0.3 * std::min(gap, lastGap) || 1 - f < 0.3 * gap) continue;
                merged.push_back(f);
                lastAt = f;
                lastGap = gap;
            }
            std::vector<double> theirs = merged, mine = merged;
            for (double& f : theirs) f *= lenS;
            for (double& f : mine) f = (turned ? 1 - f : f) * lenD;
            if (turned) std::reverse(mine.begin(), mine.end());
            where[std::size_t(src)] = theirs;
            where[std::size_t(d)] = mine;
        }
        std::map<std::pair<int, int>, int> used;
        for (int b = 0; b < nb; ++b) {
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(b)];
            const BoundaryPath path(a_t, bd);
            std::vector<int> nodes{nodeOf(bd.start)};
            std::vector<double> params{bd.t0};
            for (double at : where[std::size_t(b)]) {
                double t = 0;
                a_nodes.push_back(path.at(at, &t));
                nodes.push_back(int(a_nodes.size()) - 1);
                params.push_back(t);
            }
            nodes.push_back(nodeOf(bd.end));
            params.push_back(bd.t1);
            for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
                const std::pair<int, int> key(std::min(nodes[i], nodes[i + 1]), std::max(nodes[i], nodes[i + 1]));
                if (used.count(key)) {
                    a_error = tr("boundaries %1 and %2 meet twice too near: make the mesh finer there")
                                  .arg(used[key] + 1)
                                  .arg(b + 1);
                    return;
                }
                used[key] = b;
                a_edges.push_back({nodes[i], nodes[i + 1], b, path.onCurve() ? bd.curve : -1, params[i], params[i + 1]});
            }
        }
        // A layered region's grid inside it: its lateral spacing along the
        // layers, so many rows across each.
        for (int d = 0; d < int(a_t.domains.size()); ++d) {
            if (!layered(d)) continue;
            const QRectF r = a_t.domains[std::size_t(d)].bounds;
            const bool yStack = a_sizes.layerAxis[std::size_t(d)] == 1;
            const double across = yStack ? r.height() : r.width();
            const std::vector<double>& columns = a_columns[std::size_t(d)];
            const int rows = std::max(1, a_sizes.layerCount[std::size_t(d)]);
            for (std::size_t i = 1; i + 1 < columns.size(); ++i)
                for (int j = 1; j < rows; ++j) {
                    const double v = across * j / rows;
                    a_nodes.push_back(yStack ? QPointF(columns[i], r.top() + v) : QPointF(r.left() + v, columns[i]));
                }
        }
    }

    // --- The triangulation ------------------------------------------------
    using Tri = CDT::Triangulation<double>;

    /// A fixed edge's boundary, and whether a -> b (nodes) is its direction.
    bool edgeInfo(const Tri& cdt, int a, int b, int offset, int* boundary, bool* forward) const
    {
        auto it = a_edgeIndex.find({std::min(a, b), std::max(a, b)});
        if (it != a_edgeIndex.end()) {
            const ConstraintEdge& e = a_edges[std::size_t(it->second)];
            *boundary = e.boundary;
            *forward = e.a == a;
            return true;
        }
        // A piece of one, split as the triangles were refined.
        const CDT::Edge piece(CDT::VertInd(a + offset), CDT::VertInd(b + offset));
        auto po = cdt.pieceToOriginals.find(piece);
        if (po == cdt.pieceToOriginals.end() || po->second.empty()) return false;
        const CDT::Edge& o = po->second.front();
        auto oi = a_edgeIndex.find({int(o.v1()) - offset, int(o.v2()) - offset});
        if (oi == a_edgeIndex.end()) return false;
        const ConstraintEdge& e = a_edges[std::size_t(oi->second)];
        *boundary = e.boundary;
        const QPointF dirE = a_nodes0(e.b) - a_nodes0(e.a);
        const auto& va = cdt.vertices[std::size_t(a + offset)];
        const auto& vb = cdt.vertices[std::size_t(b + offset)];
        *forward = (vb.x - va.x) * dirE.x() + (vb.y - va.y) * dirE.y() > 0;
        return true;
    }
    QPointF a_nodes0(int n) const { return a_nodes[std::size_t(n)]; }

    /// Each triangle's domain (-1: outside every one): those by a boundary
    /// from its side's domain, the others from them, never across a
    /// boundary.
    std::vector<int> classify(const Tri& cdt, int offset) const
    {
        const std::size_t nt = cdt.triangles.size();
        std::vector<int> dom(nt, -2);
        std::vector<int> stack;
        auto isFixed = [&](CDT::VertInd a, CDT::VertInd b) { return cdt.fixedEdges.count(CDT::Edge(a, b)) > 0; };
        for (std::size_t t = 0; t < nt; ++t) {
            const CDT::Triangle& tri = cdt.triangles[t];
            if (offset > 0 && (tri.vertices[0] < 3 || tri.vertices[1] < 3 || tri.vertices[2] < 3)) {
                dom[t] = -1;
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const CDT::VertInd a = tri.vertices[std::size_t(k)], b = tri.vertices[std::size_t((k + 1) % 3)];
                if (!isFixed(a, b)) continue;
                int boundary = -1;
                bool forward = true;
                if (!edgeInfo(cdt, int(a) - offset, int(b) - offset, offset, &boundary, &forward)) continue;
                const Topology::Boundary& bd = a_t.boundaries[std::size_t(boundary)];
                // The triangle is on the left of a -> b.
                const int d = forward ? bd.up : bd.down;
                if (dom[t] == -2) {
                    dom[t] = d;
                    if (d >= 0) stack.push_back(int(t));
                }
            }
        }
        while (!stack.empty()) {
            const int t = stack.back();
            stack.pop_back();
            const CDT::Triangle& tri = cdt.triangles[std::size_t(t)];
            for (int k = 0; k < 3; ++k) {
                const CDT::TriInd nb = tri.neighbors[std::size_t(k)];
                if (nb == CDT::noNeighbor || dom[nb] != -2) continue;
                if (isFixed(tri.vertices[std::size_t(k)], tri.vertices[std::size_t((k + 1) % 3)])) continue;
                dom[nb] = dom[std::size_t(t)];
                stack.push_back(int(nb));
            }
        }
        for (int& d : dom)
            if (d == -2) d = -1;
        return dom;
    }

    static bool circumcircle(QPointF a, QPointF b, QPointF c, QPointF* center, double* radius)
    {
        const QPointF ab = b - a, ac = c - a;
        const double d = 2 * cross(ab, ac);
        if (d == 0) return false;
        const double ab2 = ab.x() * ab.x() + ab.y() * ab.y(), ac2 = ac.x() * ac.x() + ac.y() * ac.y();
        const QPointF o((ac.y() * ab2 - ab.y() * ac2) / d, (ab.x() * ac2 - ac.x() * ab2) / d);
        *center = a + o;
        *radius = norm(o);
        return true;
    }
    static bool inside(QPointF p, QPointF a, QPointF b, QPointF c)
    {
        return cross(b - a, p - a) >= 0 && cross(c - b, p - b) >= 0 && cross(a - c, p - c) >= 0;
    }
    static double segmentDistance(QPointF p, QPointF a, QPointF b)
    {
        const QPointF d = b - a;
        const double len2 = d.x() * d.x() + d.y() * d.y();
        const double u = len2 > 0 ? std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / len2, 0.0, 1.0) : 0;
        return norm(p - (a + d * u));
    }

    std::shared_ptr<Mesh> triangulate()
    {
        for (int i = 0; i < int(a_edges.size()); ++i)
            a_edgeIndex.emplace(std::pair<int, int>(std::min(a_edges[std::size_t(i)].a, a_edges[std::size_t(i)].b),
                                                    std::max(a_edges[std::size_t(i)].a, a_edges[std::size_t(i)].b)), i);
        const QRectF box = a_t.bounds;
        const double diag = std::hypot(box.width(), box.height());
        Tri cdt(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, diag * 1e-12);
        {
            std::vector<CDT::V2d<double>> pts;
            pts.reserve(a_nodes.size());
            for (QPointF p : a_nodes) pts.emplace_back(p.x(), p.y());
            cdt.insertVertices(pts);
            std::vector<CDT::Edge> edges;
            for (const ConstraintEdge& e : a_edges) edges.emplace_back(CDT::VertInd(e.a), CDT::VertInd(e.b));
            cdt.insertEdges(edges);
        }
        constexpr int offset = 3;
        // A layered region's sides never split: its grid kept.
        for (const ConstraintEdge& e : a_edges)
            if (layeredBoundary(e.boundary)) cdt.unsplitEdges.insert(CDT::Edge(CDT::VertInd(e.a + offset), CDT::VertInd(e.b + offset)));
        const std::size_t budget = 4000000;   // vertices at most
        // Every vertex's place, to a tolerance: no point put twice (two
        // circumcenters alike; one on a vertex there).
        const double snap = std::max(diag * 1e-11, 1e-300);
        std::unordered_set<unsigned long long> taken;
        auto keyOf = [snap](double x, double y, long long dx, long long dy) {
            const long long ix = std::llround(std::floor(x / snap)) + dx, iy = std::llround(std::floor(y / snap)) + dy;
            return (static_cast<unsigned long long>(ix) * 0x9E3779B97F4A7C15ull) ^ static_cast<unsigned long long>(iy);
        };
        auto isTaken = [&](double x, double y) {
            for (long long dx = -1; dx <= 1; ++dx)
                for (long long dy = -1; dy <= 1; ++dy)
                    if (taken.count(keyOf(x, y, dx, dy))) return true;
            return false;
        };
        for (QPointF p : a_nodes) taken.insert(keyOf(p.x(), p.y(), 0, 0));
        // Refined: each triangle too large for the size there split at its
        // circumcenter (else its centroid), a pass at a time.
        constexpr double TooLarge = 0.62;    // circumradius over size: an equilateral triangle's is 0.577
        std::vector<std::array<CDT::VertInd, 3>> seen;
        std::vector<char> good;
        for (int pass = 0; pass < 80; ++pass) {
            if (!step(0.1 + 0.6 * std::min(1.0, pass / 25.0), tr("Triangles"))) return nullptr;
            const std::vector<int> dom = classify(cdt, offset);
            const std::size_t nt = cdt.triangles.size();
            seen.resize(nt, {CDT::noVertex, CDT::noVertex, CDT::noVertex});
            good.resize(nt, 0);
            struct Candidate {
                QPointF p;
                double priority;
                int tri;
            };
            std::vector<Candidate> candidates;
            for (std::size_t t = 0; t < nt; ++t) {
                const int d = dom[t];
                const CDT::Triangle& tri = cdt.triangles[t];
                if (d < 0 || layered(d)) continue;
                if (good[t] && seen[t] == tri.vertices) continue;
                seen[t] = tri.vertices;
                good[t] = 0;
                const auto& v0 = cdt.vertices[tri.vertices[0]];
                const auto& v1 = cdt.vertices[tri.vertices[1]];
                const auto& v2 = cdt.vertices[tri.vertices[2]];
                const QPointF a(v0.x, v0.y), b(v1.x, v1.y), c(v2.x, v2.y);
                QPointF cc;
                double r = 0;
                if (!circumcircle(a, b, c, &cc, &r)) continue;
                const QPointF g = (a + b + c) / 3;
                const double h = size(g, d);
                if (r <= TooLarge * h) {
                    good[t] = 1;
                    continue;
                }
                // Its circumcenter, when it is in it or the triangle beyond
                // the edge it is past, and not by a boundary there.
                QPointF pick = g;
                int where = -1;
                if (inside(cc, a, b, c)) where = int(t);
                else {
                    const QPointF pts[3] = {a, b, c};
                    for (int k = 0; k < 3; ++k) {
                        if (cross(pts[(k + 1) % 3] - pts[k], cc - pts[k]) >= 0) continue;
                        const CDT::TriInd nb = tri.neighbors[std::size_t(k)];
                        if (nb == CDT::noNeighbor || dom[nb] != d) break;
                        if (cdt.fixedEdges.count(CDT::Edge(tri.vertices[std::size_t(k)], tri.vertices[std::size_t((k + 1) % 3)]))) break;
                        const CDT::Triangle& nt2 = cdt.triangles[nb];
                        const auto& w0 = cdt.vertices[nt2.vertices[0]];
                        const auto& w1 = cdt.vertices[nt2.vertices[1]];
                        const auto& w2 = cdt.vertices[nt2.vertices[2]];
                        if (inside(cc, {w0.x, w0.y}, {w1.x, w1.y}, {w2.x, w2.y})) where = int(nb);
                        break;
                    }
                }
                // A thin triangle whose circumcenter will not do: left to
                // the angles' refinement (its centroid would make thinner ones).
                const double shortest = std::min({norm(b - a), norm(c - b), norm(a - c)});
                const bool thin = r > 1.0 * shortest;
                if (where >= 0) {
                    const CDT::Triangle& wt = cdt.triangles[std::size_t(where)];
                    bool nearBoundary = false;
                    for (int k = 0; k < 3 && !nearBoundary; ++k) {
                        const CDT::VertInd ea = wt.vertices[std::size_t(k)], eb = wt.vertices[std::size_t((k + 1) % 3)];
                        if (!cdt.fixedEdges.count(CDT::Edge(ea, eb))) continue;
                        const auto& pa = cdt.vertices[ea];
                        const auto& pb = cdt.vertices[eb];
                        if (segmentDistance(cc, {pa.x, pa.y}, {pb.x, pb.y}) < 0.35 * h) nearBoundary = true;
                    }
                    if (!nearBoundary) pick = cc;
                    else if (thin || r < 1.2 * h) continue;   // left to the angles' refinement
                } else if (thin || r < 1.2 * h) {
                    continue;
                }
                candidates.push_back({pick, r / h, int(t)});
            }
            if (candidates.empty()) break;
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.priority > y.priority; });
            // One per neighbourhood a pass: none beside another's triangle.
            std::vector<char> blocked(nt, 0);
            std::vector<CDT::V2d<double>> batch;
            for (const Candidate& c : candidates) {
                if (blocked[std::size_t(c.tri)]) continue;
                blocked[std::size_t(c.tri)] = 1;
                const CDT::Triangle& tri = cdt.triangles[std::size_t(c.tri)];
                for (CDT::TriInd nb : tri.neighbors)
                    if (nb != CDT::noNeighbor) {
                        blocked[nb] = 1;
                        // (and those beyond them)
                        for (CDT::TriInd nb2 : cdt.triangles[nb].neighbors)
                            if (nb2 != CDT::noNeighbor) blocked[nb2] = 1;
                    }
                if (isTaken(c.p.x(), c.p.y())) continue;
                taken.insert(keyOf(c.p.x(), c.p.y(), 0, 0));
                batch.emplace_back(c.p.x(), c.p.y());
            }
            if (batch.empty()) break;
            if (cdt.vertices.size() + batch.size() > budget) {
                a_warnings << tr("the mesh reached %1 nodes: it is coarser than asked").arg(budget);
                break;
            }
            cdt.insertVertices(batch);
        }
        if (!step(0.75, tr("Angles"))) return nullptr;
        // The angles made good: Ruppert's refinement, outside the domains not.
        {
            const std::vector<int> dom = classify(cdt, offset);
            CDT::TriIndUSet outside;
            for (std::size_t t = 0; t < dom.size(); ++t)
                if (dom[t] < 0 || layered(dom[t])) outside.insert(CDT::TriInd(t));
            const CDT::VertInd room = CDT::VertInd(std::max<std::size_t>(budget > cdt.vertices.size() ? budget - cdt.vertices.size() : 0, 0));
            const CDT::Unrefined unrefined = cdt.refineTriangles(room, CDT::RefinementCriterion::SmallestAngle,
                                                                 CDT::degToRad(25.0), &outside, std::max(a_sizes.hmin * 0.25, diag * 1e-9));
            a_sharp = int(unrefined.sharpFixedCorner);
            const std::vector<int> dom2 = classify(cdt, offset);
            CDT::TriIndUSet erase;
            for (std::size_t t = 0; t < dom2.size(); ++t)
                if (dom2[t] < 0) erase.insert(CDT::TriInd(t));
            cdt.finalizeTriangulation(erase);
        }
        if (!step(0.85, tr("Nodes"))) return nullptr;
        return extract(cdt);
    }

    std::shared_ptr<Mesh> extract(const Tri& cdt)
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->topology = a_topo;
        mesh->unitScale = a_t.unitScale;
        const std::vector<int> dom = classify(cdt, 0);
        // The nodes used (some may be left by triangles erased).
        std::vector<int> map(cdt.vertices.size(), -1);
        for (std::size_t t = 0; t < cdt.triangles.size(); ++t) {
            if (dom[t] < 0) continue;
            for (CDT::VertInd v : cdt.triangles[t].vertices) map[v] = 0;
        }
        for (const auto& [key, index] : a_edgeIndex) {
            Q_UNUSED(index);
            if (std::size_t(key.first) < map.size() && map[std::size_t(key.first)] < 0) map[std::size_t(key.first)] = 0;
            if (std::size_t(key.second) < map.size() && map[std::size_t(key.second)] < 0) map[std::size_t(key.second)] = 0;
        }
        for (std::size_t v = 0; v < cdt.vertices.size(); ++v) {
            if (map[v] < 0) continue;
            map[v] = int(mesh->nodes.size());
            mesh->nodes.emplace_back(cdt.vertices[v].x, cdt.vertices[v].y);
        }
        std::map<std::pair<int, int>, std::pair<int, int>> edgeTris;   // (a, b) -> triangle on the left of a->b
        for (std::size_t t = 0; t < cdt.triangles.size(); ++t) {
            if (dom[t] < 0) continue;
            const auto& v = cdt.triangles[t].vertices;
            mesh->triangles.push_back({map[v[0]], map[v[1]], map[v[2]]});
            mesh->domain.push_back(dom[t]);
        }
        const int nt = int(mesh->triangles.size());
        for (int t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k) {
                const int a = mesh->triangles[std::size_t(t)][std::size_t(k)];
                const int b = mesh->triangles[std::size_t(t)][std::size_t((k + 1) % 3)];
                edgeTris[{a, b}] = {t, k};
            }
        // The boundaries' edges, each in its boundary's direction.
        std::vector<CDT::Edge> fixed(cdt.fixedEdges.begin(), cdt.fixedEdges.end());
        std::sort(fixed.begin(), fixed.end());
        std::vector<char> onBoundary(mesh->nodes.size(), 0);
        for (const CDT::Edge& e : fixed) {
            const int ca = int(e.v1()), cb = int(e.v2());
            int boundary = -1;
            bool forward = true;
            if (!edgeInfo(cdt, ca, cb, 0, &boundary, &forward)) continue;
            if (map[std::size_t(ca)] < 0 || map[std::size_t(cb)] < 0) continue;
            Mesh::Edge me;
            me.a = map[std::size_t(forward ? ca : cb)];
            me.b = map[std::size_t(forward ? cb : ca)];
            me.boundary = boundary;
            auto direct = a_edgeIndex.find({std::min(ca, cb), std::max(ca, cb)});
            const Topology::Boundary& bd = a_t.boundaries[std::size_t(boundary)];
            if (direct != a_edgeIndex.end()) {
                const ConstraintEdge& ce = a_edges[std::size_t(direct->second)];
                me.curve = ce.curve;
                me.ta = ce.ta;
                me.tb = ce.tb;
            } else if (bd.curve >= 0 && a_t.curves[std::size_t(bd.curve)].kind == Curve::Arc) {
                // A piece of an arc's edge, split: its new node put on the arc.
                const Curve& c = a_t.curves[std::size_t(bd.curve)];
                me.curve = bd.curve;
                me.ta = c.paramOf(mesh->nodes[std::size_t(me.a)]);
                me.tb = c.paramOf(mesh->nodes[std::size_t(me.b)]);
            } else if (bd.curve >= 0) {
                me.curve = bd.curve;
                const Curve& c = a_t.curves[std::size_t(bd.curve)];
                me.ta = c.paramOf(mesh->nodes[std::size_t(me.a)]);
                me.tb = c.paramOf(mesh->nodes[std::size_t(me.b)]);
            }
            auto l = edgeTris.find({me.a, me.b});
            auto r = edgeTris.find({me.b, me.a});
            me.left = l != edgeTris.end() ? l->second.first : -1;
            me.right = r != edgeTris.end() ? r->second.first : -1;
            onBoundary[std::size_t(me.a)] = onBoundary[std::size_t(me.b)] = 1;
            mesh->edges.push_back(me);
        }
        // Nodes made on an arc's chord: put on the arc, where no triangle
        // turns over.
        snapToCurves(*mesh);
        mesh->pointNode.resize(a_t.points.size(), -1);
        for (int p = 0; p < int(a_t.points.size()); ++p) {
            const int n = a_vertexNode.value(a_t.points[std::size_t(p)].vertex, -1);
            if (n >= 0 && std::size_t(n) < map.size()) mesh->pointNode[std::size_t(p)] = map[std::size_t(n)];
        }
        if (!step(0.9, tr("Smoothing"))) return nullptr;
        std::vector<char> fixedNode = onBoundary;
        for (int n : mesh->pointNode)
            if (n >= 0) fixedNode[std::size_t(n)] = 1;
        // (A layered region's grid as it is.)
        for (int t = 0; t < int(mesh->triangles.size()); ++t)
            if (layered(mesh->domain[std::size_t(t)]))
                for (int v : mesh->triangles[std::size_t(t)]) fixedNode[std::size_t(v)] = 1;
        // (Each pass of the smoothing and of the Delaunay edges asks
        // whether to stop: 349,000 nodes took 1.3 and 3 s without, a model
        // closed during them waited - the hunt of 19 October, B2.)
        smooth(*mesh, fixedNode);
        if (a_cancelled) return nullptr;
        alignLayered(*mesh);
        keepDelaunay(*mesh);
        if (a_cancelled) return nullptr;
        mesh->sharpCorners = a_sharp;
        mesh->computeStatistics();
        return mesh;
    }

    void snapToCurves(Mesh& mesh)
    {
        std::vector<std::vector<int>> around(mesh.nodes.size());
        for (int t = 0; t < int(mesh.triangles.size()); ++t)
            for (int v : mesh.triangles[std::size_t(t)]) around[std::size_t(v)].push_back(t);
        for (const Mesh::Edge& e : mesh.edges) {
            if (e.curve < 0) continue;
            const Curve& c = a_t.curves[std::size_t(e.curve)];
            if (c.kind != Curve::Arc) continue;
            for (const auto& [node, t] : {std::pair<int, double>(e.a, e.ta), std::pair<int, double>(e.b, e.tb)}) {
                const QPointF target = c.at(t);
                QPointF& p = mesh.nodes[std::size_t(node)];
                if (norm(target - p) < 1e-14 * (1 + norm(p))) continue;
                const QPointF old = p;
                double before = M_PI;
                for (int tri : around[std::size_t(node)]) before = std::min(before, minAngleOf(mesh, tri));
                p = target;
                // Kept where no triangle turns over, nor gets thinner than
                // 15° (or than it was).
                bool ok = true;
                double after = M_PI;
                for (int tri : around[std::size_t(node)]) {
                    const auto& v = mesh.triangles[std::size_t(tri)];
                    const QPointF a = mesh.nodes[std::size_t(v[0])], b = mesh.nodes[std::size_t(v[1])], cc = mesh.nodes[std::size_t(v[2])];
                    if (cross(b - a, cc - a) <= 0) ok = false;
                    else after = std::min(after, minAngleOf(mesh, tri));
                }
                if (!ok || after < std::min(before, 15 * M_PI / 180)) p = old;
            }
        }
    }

    static double minAngleOf(const Mesh& m, int t)
    {
        const auto& v = m.triangles[std::size_t(t)];
        double least = M_PI;
        for (int k = 0; k < 3; ++k) {
            const QPointF a = m.nodes[std::size_t(v[std::size_t(k)])];
            const QPointF b = m.nodes[std::size_t(v[std::size_t((k + 1) % 3)])];
            const QPointF c = m.nodes[std::size_t(v[std::size_t((k + 2) % 3)])];
            const QPointF u = b - a, w = c - a;
            least = std::min(least, std::atan2(std::abs(cross(u, w)), u.x() * w.x() + u.y() * w.y()));
        }
        return least;
    }

    /// The free nodes moved where their triangles' centroids are, weighted
    /// by area - a move kept only when no triangle round it gets worse.
    // A semiconductor's domains Delaunay again (MeshSizes::delaunay): an
    // edge inside them whose two angles opposite sum to more than π (the
    // smoothing moved a node) flipped; a segment of the geometry - a
    // boundary, an interface - whose angle across it on their side is
    // obtuse split at its middle (on an arc, on the arc), then flipped
    // about again; until none is left, or the segments are short.
    void keepDelaunay(Mesh& mesh)
    {
        const std::vector<char>& flag = a_sizes.delaunay;
        bool any = false;
        for (char f : flag) any = any || f;
        if (!any) return;
        auto flagged = [&](int t) {
            const int d = mesh.domain[std::size_t(t)];
            return d >= 0 && d < int(flag.size()) && flag[std::size_t(d)];
        };
        const QRectF box = mesh.bounds();
        const double shortest = std::max(a_sizes.hmin * 0.02, std::hypot(box.width(), box.height()) * 1e-9);
        auto key = [](int a, int b) { return std::pair<int, int>(std::min(a, b), std::max(a, b)); };
        auto area2 = [&](int a, int b, int c) { return cross(mesh.nodes[std::size_t(b)] - mesh.nodes[std::size_t(a)], mesh.nodes[std::size_t(c)] - mesh.nodes[std::size_t(a)]); };
        // The angle at o across a-b: its cotangent.
        auto cotAt = [&](int o, int a, int b) {
            const QPointF u = mesh.nodes[std::size_t(a)] - mesh.nodes[std::size_t(o)], v = mesh.nodes[std::size_t(b)] - mesh.nodes[std::size_t(o)];
            return (u.x() * v.x() + u.y() * v.y()) / std::abs(cross(u, v));
        };
        for (int round = 0; round < 30; ++round) {
            // Flips, a pass at a time (each triangle at most once a pass).
            int flipped = 0;
            for (int pass = 0; pass < 50; ++pass) {
                if (!step(0.95, tr("Smoothing"))) return;
                std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> around;   // edge: (triangle, its corner opposite)
                for (int t = 0; t < int(mesh.triangles.size()); ++t)
                    for (int k = 0; k < 3; ++k)
                        around[key(mesh.triangles[std::size_t(t)][std::size_t(k)], mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)])].push_back({t, (k + 2) % 3});
                std::set<std::pair<int, int>> segments;
                for (const Mesh::Edge& e : mesh.edges) segments.insert(key(e.a, e.b));
                std::vector<char> touched(mesh.triangles.size(), 0);
                int now = 0;
                for (const auto& [edge, list] : around) {
                    if (list.size() != 2 || segments.count(edge)) continue;
                    const auto [t1, k1] = list[0];
                    const auto [t2, k2] = list[1];
                    if (touched[std::size_t(t1)] || touched[std::size_t(t2)] || !flagged(t1) || !flagged(t2)) continue;
                    const int a = edge.first, b = edge.second;
                    const int c = mesh.triangles[std::size_t(t1)][std::size_t(k1)], d = mesh.triangles[std::size_t(t2)][std::size_t(k2)];
                    if (cotAt(c, a, b) + cotAt(d, a, b) >= -1e-12) continue;
                    // (A convex quadrilateral: c and d on either side of a-b, a and b of c-d.)
                    if (!(area2(c, d, a) * area2(c, d, b) < 0)) continue;
                    std::array<int, 3> n1{a, d, c}, n2{d, b, c};
                    if (area2(n1[0], n1[1], n1[2]) < 0) std::swap(n1[1], n1[2]);
                    if (area2(n2[0], n2[1], n2[2]) < 0) std::swap(n2[1], n2[2]);
                    mesh.triangles[std::size_t(t1)] = n1;
                    mesh.triangles[std::size_t(t2)] = n2;
                    touched[std::size_t(t1)] = touched[std::size_t(t2)] = 1;
                    ++now;
                }
                flipped += now;
                if (now == 0) break;
            }
            // Segments with an obtuse angle across them, on a flagged side.
            std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> around;
            for (int t = 0; t < int(mesh.triangles.size()); ++t)
                for (int k = 0; k < 3; ++k)
                    around[key(mesh.triangles[std::size_t(t)][std::size_t(k)], mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)])].push_back({t, (k + 2) % 3});
            std::vector<int> split;
            for (int i = 0; i < int(mesh.edges.size()); ++i) {
                const Mesh::Edge& e = mesh.edges[std::size_t(i)];
                if (norm(mesh.nodes[std::size_t(e.b)] - mesh.nodes[std::size_t(e.a)]) < 2 * shortest) continue;
                const auto it = around.find(key(e.a, e.b));
                if (it == around.end()) continue;
                for (const auto& [t, k] : it->second)
                    if (flagged(t) && cotAt(mesh.triangles[std::size_t(t)][std::size_t(k)], e.a, e.b) < -1e-12) {
                        split.push_back(i);
                        break;
                    }
            }
            if (split.empty()) break;
            for (int i : split) {
                Mesh::Edge e = mesh.edges[std::size_t(i)];
                // Its middle - on an arc, the arc's.
                QPointF m = (mesh.nodes[std::size_t(e.a)] + mesh.nodes[std::size_t(e.b)]) / 2;
                const double tm = (e.ta + e.tb) / 2;
                if (e.curve >= 0 && a_t.curves[std::size_t(e.curve)].kind == Curve::Arc) m = a_t.curves[std::size_t(e.curve)].at(tm);
                const int mi = int(mesh.nodes.size());
                mesh.nodes.push_back(m);
                // Each triangle across it in two.
                for (int t = 0; t < int(mesh.triangles.size()); ++t) {
                    std::array<int, 3>& v = mesh.triangles[std::size_t(t)];
                    for (int k = 0; k < 3; ++k) {
                        const int p = v[std::size_t(k)], q = v[std::size_t((k + 1) % 3)];
                        if (key(p, q) != key(e.a, e.b)) continue;
                        const int o = v[std::size_t((k + 2) % 3)];
                        v = {p, mi, o};
                        mesh.triangles.push_back({mi, q, o});
                        mesh.domain.push_back(mesh.domain[std::size_t(t)]);
                        break;
                    }
                }
                Mesh::Edge second = e;
                mesh.edges[std::size_t(i)].b = mi;
                mesh.edges[std::size_t(i)].tb = tm;
                second.a = mi;
                second.ta = tm;
                mesh.edges.push_back(second);
            }
            Q_UNUSED(flipped);
        }
        // Each segment's triangles again (on its left, its right).
        std::map<std::pair<int, int>, int> directed;
        for (int t = 0; t < int(mesh.triangles.size()); ++t)
            for (int k = 0; k < 3; ++k) directed[{mesh.triangles[std::size_t(t)][std::size_t(k)], mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)]}] = t;
        for (Mesh::Edge& e : mesh.edges) {
            const auto l = directed.find({e.a, e.b}), r = directed.find({e.b, e.a});
            e.left = l != directed.end() ? l->second : -1;
            e.right = r != directed.end() ? r->second : -1;
        }
    }

    /// A layered region's cells each split the one way, lower left to upper
    /// right: the triangulator's choice between a cell's two diagonals (its
    /// corners on a circle) is arbitrary, and a node's share of its triangles
    /// (a third of each - a semiconductor's box) then differs from its
    /// neighbour's - a stack uniform across solved as if it were not.
    void alignLayered(Mesh& mesh)
    {
        bool any = false;
        for (int d = 0; d < int(a_t.domains.size()); ++d) any = any || layered(d);
        if (!any) return;
        auto key = [](int a, int b) { return std::pair<int, int>(std::min(a, b), std::max(a, b)); };
        std::map<std::pair<int, int>, std::vector<int>> across;
        for (int t = 0; t < int(mesh.triangles.size()); ++t)
            for (int k = 0; k < 3; ++k) across[key(mesh.triangles[std::size_t(t)][std::size_t(k)], mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)])].push_back(t);
        std::vector<char> done(mesh.triangles.size(), 0);
        int flipped = 0;
        for (int t = 0; t < int(mesh.triangles.size()); ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (done[std::size_t(t)] || !layered(d)) continue;
            // Its diagonal: the edge neither along x nor along y.
            const std::array<int, 3>& v = mesh.triangles[std::size_t(t)];
            int diagonal = -1;
            for (int k = 0; k < 3; ++k) {
                const QPointF e = mesh.nodes[std::size_t(v[std::size_t((k + 1) % 3)])] - mesh.nodes[std::size_t(v[std::size_t(k)])];
                const double len = std::hypot(e.x(), e.y());
                if (std::abs(e.x()) > 1e-9 * len && std::abs(e.y()) > 1e-9 * len) diagonal = k;
            }
            if (diagonal < 0) continue;
            const int a = v[std::size_t(diagonal)], b = v[std::size_t((diagonal + 1) % 3)], c = v[std::size_t((diagonal + 2) % 3)];
            const std::vector<int>& pair = across[key(a, b)];
            if (pair.size() != 2) continue;
            const int u = pair[0] == t ? pair[1] : pair[0];
            if (done[std::size_t(u)] || mesh.domain[std::size_t(u)] != d) continue;
            done[std::size_t(t)] = done[std::size_t(u)] = 1;
            const QPointF e = mesh.nodes[std::size_t(b)] - mesh.nodes[std::size_t(a)];
            if (e.x() * e.y() > 0) continue;   // (lower left to upper right already)
            // The cell's other corner: the other triangle's, off a-b.
            int o = -1;
            for (int w : mesh.triangles[std::size_t(u)])
                if (w != a && w != b) o = w;
            if (o < 0) continue;
            // Split along c-o instead: (a, o, c) and (o, b, c), counter-clockwise.
            auto ccw = [&mesh](std::array<int, 3> tri) {
                const QPointF p = mesh.nodes[std::size_t(tri[0])], q = mesh.nodes[std::size_t(tri[1])], r = mesh.nodes[std::size_t(tri[2])];
                if (cross(q - p, r - p) < 0) std::swap(tri[1], tri[2]);
                return tri;
            };
            mesh.triangles[std::size_t(t)] = ccw({a, o, c});
            mesh.triangles[std::size_t(u)] = ccw({o, b, c});
            ++flipped;
        }
        if (flipped == 0) return;
        // Each segment's triangles again (on its left, its right).
        std::map<std::pair<int, int>, int> directed;
        for (int t = 0; t < int(mesh.triangles.size()); ++t)
            for (int k = 0; k < 3; ++k) directed[{mesh.triangles[std::size_t(t)][std::size_t(k)], mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)]}] = t;
        for (Mesh::Edge& edge : mesh.edges) {
            const auto l = directed.find({edge.a, edge.b}), r = directed.find({edge.b, edge.a});
            edge.left = l != directed.end() ? l->second : -1;
            edge.right = r != directed.end() ? r->second : -1;
        }
    }

    void smooth(Mesh& mesh, const std::vector<char>& fixedNode)
    {
        std::vector<std::vector<int>> around(mesh.nodes.size());
        for (int t = 0; t < int(mesh.triangles.size()); ++t)
            for (int v : mesh.triangles[std::size_t(t)]) around[std::size_t(v)].push_back(t);
        for (int sweep = 0; sweep < 4; ++sweep) {
            if (!step(0.9, tr("Smoothing"))) return;
            int moved = 0;
            for (std::size_t n = 0; n < mesh.nodes.size(); ++n) {
                if (fixedNode[n] || around[n].empty()) continue;
                double worst = M_PI;
                QPointF sum;
                double area = 0;
                for (int t : around[n]) {
                    worst = std::min(worst, minAngleOf(mesh, t));
                    const auto& v = mesh.triangles[std::size_t(t)];
                    const QPointF a = mesh.nodes[std::size_t(v[0])], b = mesh.nodes[std::size_t(v[1])], c = mesh.nodes[std::size_t(v[2])];
                    const double ar = std::abs(cross(b - a, c - a)) / 2;
                    sum += (a + b + c) / 3 * ar;
                    area += ar;
                }
                if (area <= 0) continue;
                const QPointF old = mesh.nodes[n];
                mesh.nodes[n] = sum / area;
                double after = M_PI;
                bool ok = true;
                for (int t : around[n]) {
                    const auto& v = mesh.triangles[std::size_t(t)];
                    const QPointF a = mesh.nodes[std::size_t(v[0])], b = mesh.nodes[std::size_t(v[1])], c = mesh.nodes[std::size_t(v[2])];
                    if (cross(b - a, c - a) <= 0) {
                        ok = false;
                        break;
                    }
                    after = std::min(after, minAngleOf(mesh, t));
                }
                if (!ok || after < worst) mesh.nodes[n] = old;
                else ++moved;
            }
            if (moved == 0) break;
        }
    }

    std::shared_ptr<const Topology> a_topo;
    const Topology& a_t;
    MeshSizes a_sizes;
    Progress a_progress;
    double a_k = 0.3;
    bool a_cancelled = false;
    QString a_error;
    QStringList a_warnings;
    SizeTree a_tree;
    std::vector<QPointF> a_nodes;
    QHash<int, int> a_vertexNode;
    std::vector<std::vector<double>> a_columns;   // a layered domain's (layerColumns)
    std::vector<ConstraintEdge> a_edges;
    std::map<std::pair<int, int>, int> a_edgeIndex;
    int a_sharp = 0;
};

} // namespace

// ------------------------------------------------------------------ Mesh

QRectF Mesh::bounds() const
{
    if (nodes.empty()) return {};
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (QPointF p : nodes) {
        x0 = std::min(x0, p.x());
        y0 = std::min(y0, p.y());
        x1 = std::max(x1, p.x());
        y1 = std::max(y1, p.y());
    }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

double Mesh::quality(int t) const
{
    const auto& v = triangles[std::size_t(t)];
    const QPointF a = nodes[std::size_t(v[0])], b = nodes[std::size_t(v[1])], c = nodes[std::size_t(v[2])];
    const double area = cross(b - a, c - a) / 2;
    auto sq = [](QPointF d) { return d.x() * d.x() + d.y() * d.y(); };
    const double sum = sq(b - a) + sq(c - b) + sq(a - c);
    return sum > 0 ? 4 * std::sqrt(3.0) * area / sum : 0;
}

void Mesh::computeStatistics()
{
    histogram.fill(0);
    minAngle = 180;
    minQuality = 1;
    double total = 0;
    for (int t = 0; t < int(triangles.size()); ++t) {
        const double q = quality(t);
        minQuality = std::min(minQuality, q);
        total += q;
        ++histogram[std::size_t(std::clamp(int(q * 10), 0, 9))];
        const auto& v = triangles[std::size_t(t)];
        for (int k = 0; k < 3; ++k) {
            const QPointF a = nodes[std::size_t(v[std::size_t(k)])];
            const QPointF u = nodes[std::size_t(v[std::size_t((k + 1) % 3)])] - a, w = nodes[std::size_t(v[std::size_t((k + 2) % 3)])] - a;
            minAngle = std::min(minAngle, std::atan2(std::abs(cross(u, w)), u.x() * w.x() + u.y() * w.y()) * 180 / M_PI);
        }
    }
    meanQuality = triangles.empty() ? 0 : total / double(triangles.size());
    if (triangles.empty()) minAngle = minQuality = 0;
}

// ------------------------------------------------------------------ Sizes

MeshSizes meshSizes(const Model& model, const ParameterScope& parameters, const Topology& topology, QString* error)
{
    MeshSizes s;
    QStringList problems;
    const Node& m = model.mesh();
    const QString preset = m.text(QStringLiteral("size"));
    const Preset* p = &Presets[4];
    for (const Preset& candidate : Presets)
        if (preset == QLatin1String(candidate.name)) p = &candidate;
    const double L = std::max({topology.bounds.width(), topology.bounds.height(), 1e-300});
    s.hmax = p->hmax * L;
    s.hmin = p->hmin * L;
    s.growth = p->growth;
    s.curvature = p->curvature;
    s.narrow = p->narrow;
    auto length = [&](const QString& text, const QString& what, double fallback) {
        if (text.trimmed().isEmpty()) return fallback;
        // In the geometry's unit, as its features' lengths are.
        Expression::Options options;
        options.lengthUnit = topology.unitScale;
        options.numbersAreLengths = true;
        const Expression e = Expression::compile(text, parameters.scope, options);
        if (!e.isValid() || !e.isConstant()) {
            problems << tr("%1: %2").arg(what, e.isValid() ? tr("not a constant") : e.error());
            return fallback;
        }
        const double v = e.constant();
        if (!(v > 0)) {
            problems << tr("%1 must be more than 0").arg(what);
            return fallback;
        }
        return v;
    };
    auto number = [&](const QString& text, const QString& what, double fallback) {
        if (text.trimmed().isEmpty()) return fallback;
        const Expression e = Expression::compile(text, parameters.scope);
        if (!e.isValid() || !e.isConstant() || !(e.constant() > 0)) {
            problems << tr("%1 must be a number more than 0").arg(what);
            return fallback;
        }
        return e.constant();
    };
    if (m.flag(QStringLiteral("custom"))) {
        s.hmax = length(m.text(QStringLiteral("hmax")), tr("the maximum element size"), s.hmax);
        s.hmin = length(m.text(QStringLiteral("hmin")), tr("the minimum element size"), s.hmin);
        s.growth = std::max(1.0, number(m.text(QStringLiteral("growth")), tr("the growth rate"), s.growth));
        s.curvature = number(m.text(QStringLiteral("curvature")), tr("the curvature factor"), s.curvature);
        s.narrow = number(m.text(QStringLiteral("narrow")), tr("the resolution of narrow regions"), s.narrow);
    }
    s.hmin = std::min(s.hmin, s.hmax);
    s.domainMax.assign(topology.domains.size(), Infinity);
    s.boundaryMax.assign(topology.boundaries.size(), Infinity);
    s.pointMax.assign(topology.points.size(), Infinity);
    s.boundaryCount.assign(topology.boundaries.size(), 0);
    for (const Node& f : m.children) {
        if (!f.enabled) continue;
        QString why;
        if (f.type == QLatin1String("mesh/size")) {
            const Level level = levelOf(f.text(QStringLiteral("level")));
            const QVector<int> picked = resolveSelection(topology, model, level, f.selection(), &why);
            if (!why.isEmpty()) problems << tr("%1: %2").arg(f.name(), why);
            const double h = length(f.text(QStringLiteral("hmax")), f.name(), Infinity);
            std::vector<double>& target = level == Level::Domain ? s.domainMax : level == Level::Boundary ? s.boundaryMax : s.pointMax;
            for (int i : picked)
                if (i >= 0 && i < int(target.size())) target[std::size_t(i)] = std::min(target[std::size_t(i)], h);
        } else if (f.type == QLatin1String("mesh/distribution")) {
            const QVector<int> picked = resolveSelection(topology, model, Level::Boundary, f.selection(), &why);
            if (!why.isEmpty()) problems << tr("%1: %2").arg(f.name(), why);
            const int n = std::clamp(f.integer(QStringLiteral("count")), 1, 100000);
            for (int i : picked) s.boundaryCount[std::size_t(i)] = n;
        }
    }
    // Layered regions: each a rectangle (its area its box's), a grid of its
    // lateral size along its layers and so many elements across each.
    s.layerLateral.assign(topology.domains.size(), 0);
    s.layerCount.assign(topology.domains.size(), 0);
    s.layerAxis.assign(topology.domains.size(), 1);
    s.layerWall.assign(topology.domains.size(), 0);
    for (const Node& f : m.children) {
        if (!f.enabled || f.type != QLatin1String("mesh/layered")) continue;
        QString why;
        const QVector<int> picked = resolveSelection(topology, model, Level::Domain, f.selection(), &why);
        if (!why.isEmpty()) problems << tr("%1: %2").arg(f.name(), why);
        const double h = length(f.text(QStringLiteral("hlateral")), tr("%1's lateral size").arg(f.name()), s.hmax);
        const double wall = length(f.text(QStringLiteral("hwall")), tr("%1's lateral size at its side walls").arg(f.name()), 0);
        const int n = std::clamp(f.integer(QStringLiteral("count")), 1, 1000);
        const char axis = f.text(QStringLiteral("axis")) == QLatin1String("x") ? 0 : 1;
        for (int d : picked) {
            if (d < 0 || d >= int(topology.domains.size())) continue;
            const Topology::Domain& dm = topology.domains[std::size_t(d)];
            const double box = dm.bounds.width() * dm.bounds.height();
            if (!(box > 0) || std::abs(dm.area - box) > 1e-9 * box) {
                problems << tr("%1: domain %2 is not a rectangle along x and y - a layered region's layers are").arg(f.name()).arg(d + 1);
                continue;
            }
            s.layerLateral[std::size_t(d)] = h;
            s.layerCount[std::size_t(d)] = n;
            s.layerAxis[std::size_t(d)] = axis;
            s.layerWall[std::size_t(d)] = wall;
        }
    }
    // Their boundaries' elements: along the layers as many as the lateral
    // size makes (the finer side's), across them each layer's count of its
    // thickness (a distribution given kept).
    for (int b = 0; b < int(topology.boundaries.size()); ++b) {
        const Topology::Boundary& bd = topology.boundaries[std::size_t(b)];
        if (s.boundaryCount[std::size_t(b)] > 0) continue;
        const std::vector<QPointF> line = bd.polyline(topology);
        if (line.size() < 2) continue;
        const QPointF along = line.back() - line.front();
        int count = 0;
        for (int d : {bd.up, bd.down}) {
            if (d < 0 || !(s.layerLateral[std::size_t(d)] > 0)) continue;
            const Topology::Domain& dm = topology.domains[std::size_t(d)];
            const bool yStack = s.layerAxis[std::size_t(d)] == 1;
            const bool lateral = yStack ? std::abs(along.x()) > std::abs(along.y()) : std::abs(along.y()) > std::abs(along.x());
            const double len = std::hypot(along.x(), along.y());
            if (lateral) {
                count = std::max(count, int(std::ceil(len / s.layerLateral[std::size_t(d)] - 1e-9)));
            } else {
                const double thick = yStack ? dm.bounds.height() : dm.bounds.width();
                count = std::max(count, std::max(1, int(std::lround(s.layerCount[std::size_t(d)] * len / thick))));
            }
        }
        if (count > 0) s.boundaryCount[std::size_t(b)] = std::min(count, 100000);
    }
    // A semiconductor's domains kept Delaunay.
    s.delaunay.assign(topology.domains.size(), 0);
    for (const Node* ph : model.physics()) {
        if (!ph->enabled || ph->type != QLatin1String("semiconductor")) continue;
        QString why;
        for (int d : resolveSelection(topology, model, Level::Domain, ph->selection(), &why))
            if (d >= 0 && d < int(s.delaunay.size())) s.delaunay[std::size_t(d)] = 1;
    }
    // A material's walls: boundaries between two of a semiconductor's
    // domains of different materials (each domain's the last material that
    // has it, as the physics take it; two of the same kind and properties,
    // wherever they are, one).
    {
        const auto same = [](const Node* a, const Node* c) {
            if (a == c) return true;
            QJsonObject p = a->props, q = c->props;
            p.remove(QStringLiteral("selection"));
            q.remove(QStringLiteral("selection"));
            return a->type == c->type && p == q;
        };
        std::vector<const Node*> material(topology.domains.size(), nullptr);
        for (const Node& mat : model.materials().children) {
            if (!mat.enabled) continue;
            QString why;
            for (int d : resolveSelection(topology, model, Level::Domain, mat.selection(), &why))
                if (d >= 0 && d < int(material.size())) material[std::size_t(d)] = &mat;
        }
        s.layerWalls.assign(topology.boundaries.size(), 0);
        for (int b = 0; b < int(topology.boundaries.size()); ++b) {
            const Topology::Boundary& bd = topology.boundaries[std::size_t(b)];
            if (bd.up < 0 || bd.down < 0 || !s.delaunay[std::size_t(bd.up)] || !s.delaunay[std::size_t(bd.down)]) continue;
            const Node *a = material[std::size_t(bd.up)], *c = material[std::size_t(bd.down)];
            if (a != nullptr && c != nullptr && !same(a, c)) s.layerWalls[std::size_t(b)] = 1;
        }
    }
    // A periodic condition's two sides meshed alike (where they are not
    // one the other moved, the physics says so).
    s.copyOf.assign(topology.boundaries.size(), -1);
    s.copyReversed.assign(topology.boundaries.size(), 0);
    for (const Node* ph : model.physics()) {
        if (!ph->enabled || ph->type != QLatin1String("waves")) continue;
        for (const Node& f : ph->children) {
            if (!f.enabled || f.type != QLatin1String("waves/periodic")) continue;
            QString why;
            const QVector<int> picked = resolveSelection(topology, model, Level::Boundary, f.selection(), &why);
            const std::optional<PeriodicSides> sides = periodicSides(topology, picked, &why);
            if (!sides) continue;
            for (std::size_t b = 0; b < sides->copyOf.size(); ++b)
                if (sides->copyOf[b] >= 0 && s.copyOf[b] < 0) {
                    s.copyOf[b] = sides->copyOf[b];
                    s.copyReversed[b] = sides->reversed[b];
                }
            s.periodic.push_back({sides->source, sides->destination, sides->shift});
        }
    }
    // A point source's mesh four times finer about it than its domain's: a
    // dipole's near field (1/r) is what its power is read from.
    for (const Node* ph : model.physics()) {
        if (!ph->enabled || ph->type != QLatin1String("waves")) continue;
        for (const Node& f : ph->children) {
            if (!f.enabled || (f.type != QLatin1String("waves/dipole") && f.type != QLatin1String("waves/linecurrent"))) continue;
            Expression::Options options;
            options.lengthUnit = topology.unitScale;
            options.numbersAreLengths = true;
            const QStringList at = f.pair(QStringLiteral("position"));
            const Expression x = Expression::compile(at.value(0), parameters.scope, options);
            const Expression y = Expression::compile(at.value(1), parameters.scope, options);
            // (one that is not a constant said by its physics)
            if (!x.isValid() || !x.isConstant() || !y.isValid() || !y.isConstant()) continue;
            const QPointF p(x.constant(), y.constant());
            const int d = topology.domainAt(p);
            const double there = d >= 0 && d < int(s.domainMax.size()) ? std::min(s.hmax, s.domainMax[std::size_t(d)]) : s.hmax;
            s.points.push_back({p.x(), p.y(), there / 4});
        }
    }
    if (error) *error = problems.join(QLatin1String("; "));
    return s;
}

namespace {

/// Boundary edge \a e of \a m split at \a p (on it): each triangle by it in
/// two, the edges that bound them told.
void splitBoundaryEdge(Mesh& m, int e, QPointF p, double s)
{
    const int np = int(m.nodes.size());
    m.nodes.push_back(p);
    Mesh::Edge& ed = m.edges[std::size_t(e)];
    const int a = ed.a, b = ed.b;
    Mesh::Edge second = ed;
    second.a = np;
    second.ta = ed.ta + s * (ed.tb - ed.ta);
    ed.b = np;
    ed.tb = second.ta;
    auto edgeOf = [&m](int u, int v) {
        for (int k = 0; k < int(m.edges.size()); ++k) {
            const Mesh::Edge& x = m.edges[std::size_t(k)];
            if ((x.a == u && x.b == v) || (x.a == v && x.b == u)) return k;
        }
        return -1;
    };
    for (int* side : {&ed.left, &ed.right}) {
        const int t = *side;
        if (t < 0) continue;
        std::array<int, 3>& tri = m.triangles[std::size_t(t)];
        // The new one: the old with a made p (the half by b); the old with b
        // made p (the half by a) - the same turn both.
        std::array<int, 3> half = tri;
        for (int& v : half)
            if (v == a) v = np;
        for (int& v : tri)
            if (v == b) v = np;
        const int t2 = int(m.triangles.size());
        m.triangles.push_back(half);
        m.domain.push_back(m.domain[std::size_t(t)]);
        int c = -1;
        for (int v : half)
            if (v != np && v != b) c = v;
        // The edge (b, c) now bounds the new half.
        if (const int k = edgeOf(b, c); k >= 0) {
            Mesh::Edge& bc = m.edges[std::size_t(k)];
            if (bc.left == t) bc.left = t2;
            if (bc.right == t) bc.right = t2;
        }
        // (second's side: the half by b)
        if (side == &ed.left) second.left = t2;
        else second.right = t2;
    }
    m.edges.push_back(second);
}

/// Each periodic condition's two sides made the same node for node: a node
/// of one moved onto the other where it has none, splitting its edge there.
void matchPeriodicSides(Mesh& m, const MeshSizes& sizes)
{
    if (sizes.periodic.empty()) return;
    const QRectF box = m.bounds();
    const double tol = 1e-8 * std::max(std::hypot(box.width(), box.height()), 1e-300);
    bool changed = false;
    for (int pass = 0; pass < 8; ++pass) {
        bool split = false;
        for (const MeshSizes::Periodic& pc : sizes.periodic)
            for (int dir : {1, -1}) {
                const std::vector<int>& from = dir > 0 ? pc.source : pc.destination;
                const std::vector<int>& to = dir > 0 ? pc.destination : pc.source;
                const QPointF move = dir > 0 ? pc.shift : -pc.shift;
                std::set<int> fromNodes;
                std::vector<int> toEdges;
                for (int e = 0; e < int(m.edges.size()); ++e) {
                    const Mesh::Edge& ed = m.edges[std::size_t(e)];
                    if (std::find(from.begin(), from.end(), ed.boundary) != from.end()) {
                        fromNodes.insert(ed.a);
                        fromNodes.insert(ed.b);
                    }
                    if (std::find(to.begin(), to.end(), ed.boundary) != to.end()) toEdges.push_back(e);
                }
                for (int n : fromNodes) {
                    const QPointF p = m.nodes[std::size_t(n)] + move;
                    int best = -1;
                    double bestDist = std::numeric_limits<double>::infinity(), bestS = 0, bestLen = 0;
                    for (int e : toEdges) {
                        const Mesh::Edge& ed = m.edges[std::size_t(e)];
                        const QPointF a = m.nodes[std::size_t(ed.a)], d = m.nodes[std::size_t(ed.b)] - a;
                        const double l2 = d.x() * d.x() + d.y() * d.y();
                        if (!(l2 > 0)) continue;
                        const double s = std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 0.0, 1.0);
                        const double dist = std::hypot(a.x() + s * d.x() - p.x(), a.y() + s * d.y() - p.y());
                        if (dist < bestDist) {
                            best = e;
                            bestDist = dist;
                            bestS = s;
                            bestLen = std::sqrt(l2);
                        }
                    }
                    if (best < 0 || bestDist > tol) continue;   // (not on the other side: the physics says so)
                    if (bestS * bestLen <= tol || (1 - bestS) * bestLen <= tol) continue;   // a node there already
                    const Mesh::Edge& ed = m.edges[std::size_t(best)];
                    const QPointF a = m.nodes[std::size_t(ed.a)], b = m.nodes[std::size_t(ed.b)];
                    splitBoundaryEdge(m, best, a + (b - a) * bestS, bestS);
                    toEdges.push_back(int(m.edges.size()) - 1);
                    split = changed = true;
                }
            }
        if (!split) break;
    }
    if (changed) m.computeStatistics();
}

} // namespace

MeshBuild buildMesh(std::shared_ptr<const Topology> topology, const MeshSizes& sizes, const Progress& progress)
{
    if (!topology) {
        MeshBuild out;
        out.error = tr("there is no geometry");
        return out;
    }
    Mesher mesher(std::move(topology), sizes, progress);
    MeshBuild out = mesher.run();
    if (out.mesh) matchPeriodicSides(*out.mesh, sizes);
    if (out.mesh && out.mesh->triangles.empty()) {
        out.error = tr("the mesh has no triangles");
        out.mesh.reset();
    }
    if (out.mesh && progress) progress(1.0, tr("Done"));
    return out;
}

MeshBuild buildMesh(const Model& model, const ParameterScope& parameters, std::shared_ptr<const Topology> topology,
                    const Progress& progress)
{
    if (!topology) return buildMesh(nullptr, MeshSizes(), progress);
    QString why;
    const MeshSizes sizes = meshSizes(model, parameters, *topology, &why);
    MeshBuild out = buildMesh(topology, sizes, progress);
    if (!why.isEmpty()) out.warnings.prepend(why);
    return out;
}

} // namespace qucs_s::fem
