/*
 * router_geometry.cpp - the metal router's geometry (router_geometry.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router_geometry.h"

#include <cmath>
#include <limits>

namespace qucs_s::router {

Box boundsOf(const Poly& poly)
{
    Box b;
    for (const Pt& p : poly) b = b.united(Box{p.x, p.y, p.x, p.y});
    return b;
}

double area2(const Poly& poly)
{
    double a = 0;
    for (std::size_t i = 0, n = poly.size(); i < n; ++i) {
        const Pt& p = poly[i];
        const Pt& q = poly[(i + 1) % n];
        a += double(p.x) * double(q.y) - double(q.x) * double(p.y);
    }
    return a;
}

namespace {

using Wide = __int128;

// The sign of (b - a) x (c - a).
int orientation(Pt a, Pt b, Pt c)
{
    const Wide v = Wide(b.x - a.x) * Wide(c.y - a.y) - Wide(b.y - a.y) * Wide(c.x - a.x);
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

bool onSegment(Pt a, Pt b, Pt p)
{
    return orientation(a, b, p) == 0 && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y)
           && p.y <= std::max(a.y, b.y);
}

bool segmentsMeet(Pt a0, Pt a1, Pt b0, Pt b1)
{
    const int o1 = orientation(a0, a1, b0), o2 = orientation(a0, a1, b1);
    const int o3 = orientation(b0, b1, a0), o4 = orientation(b0, b1, a1);
    if (o1 != o2 && o3 != o4 && o1 * o2 <= 0 && o3 * o4 <= 0) return true;
    return onSegment(a0, a1, b0) || onSegment(a0, a1, b1) || onSegment(b0, b1, a0) || onSegment(b0, b1, a1);
}

double pointSegment2(double px, double py, double ax, double ay, double bx, double by, double* qx, double* qy)
{
    const double dx = bx - ax, dy = by - ay;
    const double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    *qx = ax + t * dx;
    *qy = ay + t * dy;
    return (px - *qx) * (px - *qx) + (py - *qy) * (py - *qy);
}

} // namespace

bool inside(const Poly& poly, Pt p)
{
    const std::size_t n = poly.size();
    if (n < 3) return false;
    int winding = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Pt& a = poly[i];
        const Pt& b = poly[(i + 1) % n];
        if (onSegment(a, b, p)) return true;
        if (a.y <= p.y) {
            if (b.y > p.y && orientation(a, b, p) > 0) ++winding;
        } else if (b.y <= p.y && orientation(a, b, p) < 0) {
            --winding;
        }
    }
    return winding != 0;
}

Poly polyOf(const Box& b)
{
    return {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
}

void boxesOf(const Poly& poly, Coord slab, std::vector<Box>* outer, std::vector<Box>* inner)
{
    struct Edge {
        double xa, ya, xb, yb;   // ya < yb
        int dir;
        Coord ylo, yhi;
        bool slanted;
    };
    std::vector<Edge> edges;
    std::vector<Coord> ys;
    const std::size_t n = poly.size();
    if (n < 3) return;
    edges.reserve(n);
    ys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Pt& a = poly[i];
        const Pt& b = poly[(i + 1) % n];
        ys.push_back(a.y);
        if (a.y == b.y) continue;
        Edge e;
        if (a.y < b.y) e = {double(a.x), double(a.y), double(b.x), double(b.y), 1, a.y, b.y, a.x != b.x};
        else e = {double(b.x), double(b.y), double(a.x), double(a.y), -1, b.y, a.y, a.x != b.x};
        edges.push_back(e);
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.ylo < b.ylo; });
    std::vector<Box> outs, ins;
    std::vector<int> active;
    std::size_t next = 0;
    struct Cross {
        double xlo, xhi, xmid;
        int dir;
    };
    std::vector<Cross> crossings;
    const auto xAt = [](const Edge& e, double y) { return e.xa + (e.xb - e.xa) * (y - e.ya) / (e.yb - e.ya); };
    for (std::size_t s = 0; s + 1 < ys.size(); ++s) {
        const Coord ya = ys[s], yb = ys[s + 1];
        while (next < edges.size() && edges[next].ylo <= ya) active.push_back(int(next++));
        active.erase(std::remove_if(active.begin(), active.end(), [&](int i) { return edges[i].yhi <= ya; }), active.end());
        bool slanted = false;
        for (int i : active) slanted = slanted || edges[i].slanted;
        Coord step = yb - ya;
        if (slanted && slab > 0 && step > slab) step = slab;
        for (Coord y0 = ya; y0 < yb; y0 += step) {
            const Coord y1 = std::min(yb, y0 + step);
            crossings.clear();
            for (int i : active) {
                const Edge& e = edges[i];
                if (e.ylo > y0 || e.yhi < y1) continue;
                const double x0 = xAt(e, double(y0)), x1 = xAt(e, double(y1));
                crossings.push_back({std::min(x0, x1), std::max(x0, x1), 0.5 * (x0 + x1), e.dir});
            }
            std::sort(crossings.begin(), crossings.end(), [](const Cross& a, const Cross& b) { return a.xmid < b.xmid; });
            int winding = 0;
            const Cross* left = nullptr;
            for (const Cross& c : crossings) {
                const int before = winding;
                winding += c.dir;
                if (before == 0 && winding != 0) left = &c;
                else if (before != 0 && winding == 0 && left) {
                    const Coord ox0 = Coord(std::floor(left->xlo)), ox1 = Coord(std::ceil(c.xhi));
                    if (outer && ox1 > ox0) outs.push_back({ox0, y0, ox1, y1});
                    const Coord ix0 = Coord(std::ceil(left->xhi)), ix1 = Coord(std::floor(c.xlo));
                    if (inner && ix1 > ix0) ins.push_back({ix0, y0, ix1, y1});
                    left = nullptr;
                }
            }
        }
    }
    const auto merge = [](std::vector<Box>& boxes, std::vector<Box>* to) {
        std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
            if (a.x0 != b.x0) return a.x0 < b.x0;
            if (a.x1 != b.x1) return a.x1 < b.x1;
            return a.y0 < b.y0;
        });
        for (std::size_t i = 0; i < boxes.size();) {
            Box b = boxes[i++];
            while (i < boxes.size() && boxes[i].x0 == b.x0 && boxes[i].x1 == b.x1 && boxes[i].y0 <= b.y1) b.y1 = std::max(b.y1, boxes[i++].y1);
            to->push_back(b);
        }
    };
    if (outer) merge(outs, outer);
    if (inner) merge(ins, inner);
}

double segmentDistance2(Pt a0, Pt a1, Pt b0, Pt b1, Pt* at)
{
    if (segmentsMeet(a0, a1, b0, b1)) {
        if (at) {
            // (where they meet, near enough: the nearest endpoint of one to the other)
            double qx, qy;
            pointSegment2(0, 0, double(b0.x - a0.x), double(b0.y - a0.y), double(b1.x - a0.x), double(b1.y - a0.y), &qx, &qy);
            *at = {a0.x + Coord(std::llround(qx)), a0.y + Coord(std::llround(qy))};
        }
        return 0;
    }
    // Relative to a0, so the squares stay small and exact for near segments.
    const double ax = 0, ay = 0, bx = double(a1.x - a0.x), by = double(a1.y - a0.y);
    const double cx = double(b0.x - a0.x), cy = double(b0.y - a0.y), dx = double(b1.x - a0.x), dy = double(b1.y - a0.y);
    double best = std::numeric_limits<double>::max(), mx = 0, my = 0;
    double qx, qy, d;
    if ((d = pointSegment2(ax, ay, cx, cy, dx, dy, &qx, &qy)) < best) best = d, mx = (ax + qx) / 2, my = (ay + qy) / 2;
    if ((d = pointSegment2(bx, by, cx, cy, dx, dy, &qx, &qy)) < best) best = d, mx = (bx + qx) / 2, my = (by + qy) / 2;
    if ((d = pointSegment2(cx, cy, ax, ay, bx, by, &qx, &qy)) < best) best = d, mx = (cx + qx) / 2, my = (cy + qy) / 2;
    if ((d = pointSegment2(dx, dy, ax, ay, bx, by, &qx, &qy)) < best) best = d, mx = (dx + qx) / 2, my = (dy + qy) / 2;
    if (at) *at = {a0.x + Coord(std::llround(mx)), a0.y + Coord(std::llround(my))};
    return best;
}

double polyDistance(const Poly& a, const Poly& b, Pt* at, double stopBelow)
{
    if (a.empty() || b.empty()) return std::numeric_limits<double>::max();
    if (inside(b, a.front())) {
        if (at) *at = a.front();
        return 0;
    }
    if (inside(a, b.front())) {
        if (at) *at = b.front();
        return 0;
    }
    double best2 = std::numeric_limits<double>::max();
    const double stop2 = stopBelow * stopBelow;
    const std::size_t na = a.size(), nb = b.size();
    for (std::size_t i = 0; i < na; ++i) {
        const Pt a0 = a[i], a1 = a[(i + 1) % na];
        const Box ea{std::min(a0.x, a1.x), std::min(a0.y, a1.y), std::max(a0.x, a1.x), std::max(a0.y, a1.y)};
        for (std::size_t j = 0; j < nb; ++j) {
            const Pt b0 = b[j], b1 = b[(j + 1) % nb];
            const Box eb{std::min(b0.x, b1.x), std::min(b0.y, b1.y), std::max(b0.x, b1.x), std::max(b0.y, b1.y)};
            if (boxDistance2(ea, eb) >= best2) continue;
            Pt where;
            const double d2 = segmentDistance2(a0, a1, b0, b1, &where);
            if (d2 < best2) {
                best2 = d2;
                if (at) *at = where;
                if (best2 <= stop2 && stopBelow > 0) return std::sqrt(best2);
                if (best2 == 0) return 0;
            }
        }
    }
    return std::sqrt(best2);
}

void BoxIndex::build(const std::vector<Box>& boxes, Coord bucket)
{
    a_boxes = boxes;
    a_huge.clear();
    a_start.clear();
    a_items.clear();
    a_bounds = Box();
    for (const Box& b : a_boxes) a_bounds = a_bounds.united(b);
    if (a_boxes.empty()) {
        a_nx = a_ny = 0;
        return;
    }
    if (bucket <= 0) {
        // About as many buckets as boxes, square ones.
        const double area = std::max(1.0, double(a_bounds.width() + 1) * double(a_bounds.height() + 1));
        bucket = Coord(std::max(1.0, std::sqrt(area / double(a_boxes.size()))));
    }
    a_bucket = std::max<Coord>(1, bucket);
    a_nx = int(std::min<Coord>(4096, a_bounds.width() / a_bucket + 1));
    a_ny = int(std::min<Coord>(4096, a_bounds.height() / a_bucket + 1));
    a_bucket = std::max(a_bucket, std::max((a_bounds.width() + a_nx) / a_nx, (a_bounds.height() + a_ny) / a_ny));
    const std::size_t buckets = std::size_t(a_nx) * std::size_t(a_ny);
    std::vector<std::uint32_t> counts(buckets + 1, 0);
    const auto span = [&](const Box& b, int& i0, int& i1, int& j0, int& j1) {
        i0 = int(std::clamp<Coord>((b.x0 - a_bounds.x0) / a_bucket, 0, a_nx - 1));
        i1 = int(std::clamp<Coord>((b.x1 - a_bounds.x0) / a_bucket, 0, a_nx - 1));
        j0 = int(std::clamp<Coord>((b.y0 - a_bounds.y0) / a_bucket, 0, a_ny - 1));
        j1 = int(std::clamp<Coord>((b.y1 - a_bounds.y0) / a_bucket, 0, a_ny - 1));
    };
    std::vector<char> huge(a_boxes.size(), 0);
    for (std::size_t k = 0; k < a_boxes.size(); ++k) {
        int i0, i1, j0, j1;
        span(a_boxes[k], i0, i1, j0, j1);
        if (std::size_t(i1 - i0 + 1) * std::size_t(j1 - j0 + 1) > 4096) {
            huge[k] = 1;
            a_huge.push_back(std::uint32_t(k));
            continue;
        }
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) ++counts[std::size_t(j) * std::size_t(a_nx) + std::size_t(i) + 1];
    }
    for (std::size_t b = 0; b < buckets; ++b) counts[b + 1] += counts[b];
    a_start = counts;
    a_items.assign(a_start.back(), 0);
    std::vector<std::uint32_t> fill(a_start.begin(), a_start.end() - 1);
    for (std::size_t k = 0; k < a_boxes.size(); ++k) {
        if (huge[k]) continue;
        int i0, i1, j0, j1;
        span(a_boxes[k], i0, i1, j0, j1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) a_items[fill[std::size_t(j) * std::size_t(a_nx) + std::size_t(i)]++] = std::uint32_t(k);
    }
}

} // namespace qucs_s::router
