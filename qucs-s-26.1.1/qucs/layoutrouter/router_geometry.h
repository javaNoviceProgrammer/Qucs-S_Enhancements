/*
 * router_geometry.h - the metal router's geometry: points and boxes in
 *                     whole units of the layout (a nanometre by default),
 *                     polygons cut into boxes, and exact distances
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ROUTER_GEOMETRY_H
#define QUCS_ROUTER_GEOMETRY_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace qucs_s::router {

/// A length in whole units (Problem::unit µm each; 1 nm unless the layout's
/// database unit is finer). Whole numbers so that a line exactly at its
/// spacing is legal, and is legal on every machine.
using Coord = std::int64_t;

struct Pt {
    Coord x = 0, y = 0;
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Pt& o) const { return !(*this == o); }
};

/// A box, closed: [x0, x1] x [y0, y1]. Empty when x1 < x0 or y1 < y0.
struct Box {
    Coord x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool empty() const { return x1 < x0 || y1 < y0; }
    Coord width() const { return x1 - x0; }
    Coord height() const { return y1 - y0; }
    Pt centre() const { return {x0 + (x1 - x0) / 2, y0 + (y1 - y0) / 2}; }
    bool contains(Pt p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
    bool contains(const Box& b) const { return b.x0 >= x0 && b.x1 <= x1 && b.y0 >= y0 && b.y1 <= y1; }
    /// Overlapping with some area (not just touching).
    bool overlaps(const Box& b) const { return b.x0 < x1 && b.x1 > x0 && b.y0 < y1 && b.y1 > y0; }
    /// Overlapping or touching.
    bool meets(const Box& b) const { return b.x0 <= x1 && b.x1 >= x0 && b.y0 <= y1 && b.y1 >= y0; }
    /// Sharing area or a stretch of edge (corners alone do not join metal).
    bool joins(const Box& b) const
    {
        const Coord w = std::min(x1, b.x1) - std::max(x0, b.x0);
        const Coord h = std::min(y1, b.y1) - std::max(y0, b.y0);
        return (w > 0 && h >= 0) || (w >= 0 && h > 0);
    }
    Box grown(Coord d) const { return {x0 - d, y0 - d, x1 + d, y1 + d}; }
    Box united(const Box& b) const
    {
        if (empty()) return b;
        if (b.empty()) return *this;
        return {std::min(x0, b.x0), std::min(y0, b.y0), std::max(x1, b.x1), std::max(y1, b.y1)};
    }
    Box intersected(const Box& b) const { return {std::max(x0, b.x0), std::max(y0, b.y0), std::min(x1, b.x1), std::min(y1, b.y1)}; }
    bool operator==(const Box& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

using Poly = std::vector<Pt>;

Box boundsOf(const Poly& poly);
/// Twice the signed area: positive counterclockwise.
double area2(const Poly& poly);
/// Whether \a p is inside \a poly or on its edge (nonzero winding).
bool inside(const Poly& poly, Pt p);
/// A box as a polygon, counterclockwise.
Poly polyOf(const Box& b);

/// \a poly cut into boxes by horizontal slabs at its vertices (nonzero
/// winding). Outer boxes cover it - a slanted edge's slab is cut again
/// every \a slab units at most, so they cover no more than that much
/// beside it; inner boxes lie inside it. A Manhattan polygon's are the
/// same, and exact. Boxes of one slab next to each other are not merged;
/// boxes over each other with the same sides are.
void boxesOf(const Poly& poly, Coord slab, std::vector<Box>* outer, std::vector<Box>* inner);

/// The squared distance between two boxes (0 when they meet).
inline double boxDistance2(const Box& a, const Box& b)
{
    const double dx = double(std::max<Coord>({0, a.x0 - b.x1, b.x0 - a.x1}));
    const double dy = double(std::max<Coord>({0, a.y0 - b.y1, b.y0 - a.y1}));
    return dx * dx + dy * dy;
}

/// Whether a box at least \a s from \a b would be too near \a a: closer
/// than \a s, or, for \a s of 0, overlapping it with some area.
inline bool tooNear(const Box& a, const Box& b, Coord s)
{
    if (s <= 0) return a.overlaps(b);
    const double dx = double(std::max<Coord>({0, a.x0 - b.x1, b.x0 - a.x1}));
    const double dy = double(std::max<Coord>({0, a.y0 - b.y1, b.y0 - a.y1}));
    return dx * dx + dy * dy < double(s) * double(s);
}

/// The squared distance between two segments, and the midpoint of the
/// nearest pair of their points in \a at.
double segmentDistance2(Pt a0, Pt a1, Pt b0, Pt b1, Pt* at = nullptr);
/// The distance between two polygons (0 when they overlap or touch), and
/// where, in \a at; stops looking once it is below \a stopBelow.
double polyDistance(const Poly& a, const Poly& b, Pt* at = nullptr, double stopBelow = 0);

/// Whether two polygons share some area (not just an edge or a corner).
bool polysOverlap(const Poly& a, const Poly& b);
/// Whether they share area or a stretch of edge.
bool polysJoin(const Poly& a, const Poly& b);

/// A uniform grid of buckets of boxes, for "what is near here".
class BoxIndex
{
public:
    void build(const std::vector<Box>& boxes, Coord bucket = 0);
    /// Calls \a each(i) once for every box that meets \a region, until it
    /// returns false.
    template<class F> void query(const Box& region, F&& each) const;
    const std::vector<Box>& boxes() const { return a_boxes; }
    bool isEmpty() const { return a_boxes.empty(); }

private:
    std::vector<Box> a_boxes;
    Box a_bounds;
    Coord a_bucket = 1;
    int a_nx = 0, a_ny = 0;
    std::vector<std::uint32_t> a_start;   // a CSR of the boxes in each bucket
    std::vector<std::uint32_t> a_items;
    std::vector<std::uint32_t> a_huge;    // boxes in so many buckets they are kept apart
};

template<class F> void BoxIndex::query(const Box& region, F&& each) const
{
    // Thread-safe: a box is reported from the first bucket it shares with
    // the region, so nothing need remember what was reported.
    if (a_boxes.empty() || region.empty()) return;
    for (std::uint32_t i : a_huge)
        if (a_boxes[i].meets(region) && !each(int(i))) return;
    if (!region.meets(a_bounds)) return;
    const auto col = [&](Coord x) { return int(std::clamp<Coord>((x - a_bounds.x0) / a_bucket, 0, a_nx - 1)); };
    const auto row = [&](Coord y) { return int(std::clamp<Coord>((y - a_bounds.y0) / a_bucket, 0, a_ny - 1)); };
    const int i0 = col(region.x0), i1 = col(region.x1), j0 = row(region.y0), j1 = row(region.y1);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const std::size_t b = std::size_t(j) * std::size_t(a_nx) + std::size_t(i);
            for (std::uint32_t k = a_start[b]; k < a_start[b + 1]; ++k) {
                const std::uint32_t id = a_items[k];
                const Box& box = a_boxes[id];
                if (i != std::max(i0, col(box.x0)) || j != std::max(j0, row(box.y0))) continue;
                if (box.meets(region) && !each(int(id))) return;
            }
        }
}

} // namespace qucs_s::router

#endif // QUCS_ROUTER_GEOMETRY_H
