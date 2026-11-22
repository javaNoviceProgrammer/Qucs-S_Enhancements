/*
 * fem3_mesh.cpp - a 3D multiphysics model's swept mesh: its plan meshed,
 *                 each slab divided in z, prisms through the slabs each
 *                 triangle's domains fill
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_mesh.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <new>
#include <set>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Mesh3", text);
}

constexpr double Infinity = std::numeric_limits<double>::infinity();

} // namespace

int Mesh3::node(int planNode, int height) const
{
    if (byBand()) {
        for (int b = 0; b < int(bands.size()); ++b)
            if (height >= bands[std::size_t(b)].first && height <= bands[std::size_t(b)].last) return node(b, planNode, height);
        return -1;
    }
    if (!plan || planNode < 0 || height < 0 || planNode >= int(plan->nodes.size()) || height >= int(heights.size())) return -1;
    return nodeAt[std::size_t(height) * plan->nodes.size() + std::size_t(planNode)];
}

int Mesh3::node(int band, int planNode, int height) const
{
    if (!byBand()) return band == 0 ? node(planNode, height) : -1;
    if (band < 0 || band >= int(bands.size())) return -1;
    const Band& b = bands[std::size_t(band)];
    if (!b.plan || planNode < 0 || planNode >= int(b.plan->nodes.size()) || height < b.first || height > b.last) return -1;
    return b.nodeAt[std::size_t(height - b.first) * b.plan->nodes.size() + std::size_t(planNode)];
}

void Mesh3::computeStatistics()
{
    minAngle = plan ? plan->minAngle : 0;
    minQuality = plan ? plan->minQuality : 0;
    for (const Band& b : bands)
        if (b.plan) {
            minAngle = std::min(minAngle, b.plan->minAngle);
            minQuality = std::min(minQuality, b.plan->minQuality);
        }
    maxStretch = 0;
    meanStretch = 0;
    volume = 0;
    if (!plan) return;
    for (std::size_t i = 0; i < prisms.size(); ++i) {
        const Mesh& pl = bands.empty() ? *plan : planOf(int(i));
        const std::array<int, 3>& tri = pl.triangles[std::size_t(prismTriangle[i])];
        const QPointF a = pl.nodes[std::size_t(tri[0])], b = pl.nodes[std::size_t(tri[1])], c = pl.nodes[std::size_t(tri[2])];
        const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x())) / 2;
        const double h = heights[std::size_t(prismLayer[i]) + 1] - heights[std::size_t(prismLayer[i])];
        // (The side of an equilateral triangle of its area.)
        const double side = std::sqrt(4 * area / std::sqrt(3.0));
        const double stretch = side > 0 && h > 0 ? std::max(h / side, side / h) : Infinity;
        maxStretch = std::max(maxStretch, stretch);
        meanStretch += stretch;
        volume += area * h;
    }
    if (!prisms.empty()) meanStretch /= double(prisms.size());
}

std::vector<std::vector<double>> sweptDivisions(const std::vector<double>& levels, const std::vector<double>& hmax,
                                                const std::vector<int>& count, const std::vector<double>& hLevel, double growth)
{
    const int K = levels.size() > 1 ? int(levels.size()) - 1 : 0;
    std::vector<std::vector<double>> out(std::size_t(std::max(K, 0)));
    if (K < 1) return out;
    const double g = std::max(1.0, growth) - 1;
    // Each slab's element at the most: its size's, its count's (one at
    // least) - a thin slab's its thickness.
    std::vector<double> least(static_cast<std::size_t>(K));
    for (int k = 0; k < K; ++k) {
        const double t = levels[std::size_t(k) + 1] - levels[std::size_t(k)];
        const int n = k < int(count.size()) && count[std::size_t(k)] > 0 ? count[std::size_t(k)] : 1;
        const double h = k < int(hmax.size()) && hmax[std::size_t(k)] > 0 ? hmax[std::size_t(k)] : Infinity;
        least[std::size_t(k)] = std::min(h, t / n);
    }
    // The size at z: each slab's grown by the distance from it at the rate,
    // each level's asked likewise - the least of them.
    auto size = [&](double z) {
        double h = Infinity;
        for (int k = 0; k < K; ++k) {
            const double lo = levels[std::size_t(k)], hi = levels[std::size_t(k) + 1];
            const double d = z < lo ? lo - z : z > hi ? z - hi : 0;
            h = std::min(h, least[std::size_t(k)] + g * d);
        }
        for (int i = 0; i <= K && i < int(hLevel.size()); ++i)
            if (hLevel[std::size_t(i)] > 0 && std::isfinite(hLevel[std::size_t(i)]))
                h = std::min(h, hLevel[std::size_t(i)] + g * std::abs(z - levels[std::size_t(i)]));
        return h;
    };
    const int samples = 1024;
    for (int k = 0; k < K; ++k) {
        const double lo = levels[std::size_t(k)], hi = levels[std::size_t(k) + 1];
        // ∫ dz / h across it, sampled.
        std::vector<double> cumulative(samples + 1, 0);
        double previous = 1 / size(lo);
        for (int s = 1; s <= samples; ++s) {
            const double z = lo + (hi - lo) * s / samples;
            const double now = 1 / size(z);
            cumulative[std::size_t(s)] = cumulative[std::size_t(s) - 1] + (previous + now) / 2 * (hi - lo) / samples;
            previous = now;
        }
        const double total = cumulative.back();
        const int given = k < int(count.size()) ? count[std::size_t(k)] : 0;
        // (So many at most: a size of 1e-9 across 1 mm asked 1e6, and more
        // than an int's range overflowed.)
        const int n = given > 0 ? given : int(std::clamp(std::ceil(total - 1e-6), 1.0, double(MaxSweptDivisions)));
        std::vector<double>& cuts = out[std::size_t(k)];
        int s = 0;
        for (int j = 1; j < n; ++j) {
            const double target = total * j / n;
            while (s < samples && cumulative[std::size_t(s) + 1] < target) ++s;
            const double c0 = cumulative[std::size_t(s)], c1 = cumulative[std::size_t(std::min(s + 1, samples))];
            const double u = c1 > c0 ? (target - c0) / (c1 - c0) : 0;
            cuts.push_back(lo + (hi - lo) * (s + u) / samples);
        }
    }
    return out;
}

namespace {

/// A band of heights and how its plan is meshed: its plan's topology (the
/// geometry's, or one with some plan domains merged) and sizes; a merged
/// topology's boundaries and points by the geometry's plan's numbers.
struct BandPlan {
    int first = 0, last = 0;
    std::shared_ptr<const Topology> topology;
    MeshSizes sizes;
    std::vector<int> merged;       ///< by plan domain: its representative (empty: none merged)
    std::vector<int> boundaryOf;   ///< by the topology's boundary: the plan's (empty: the plan's own)
    std::vector<int> pointOf;      ///< by the topology's point: the plan's
    bool finerThanNext = false;    ///< at its interface with the next band, the constrained side
};

/// \a plan with its domains merged into their representatives \a rep: the
/// boundaries between two of one left out (their segments no boundary's),
/// the points on none of the rest left out; the boundaries' and points' plan
/// numbers in \a boundaryOf and \a pointOf.
std::shared_ptr<Topology> mergedPlan(const Topology& plan, const std::vector<int>& rep, std::vector<int>* boundaryOf, std::vector<int>* pointOf)
{
    auto out = std::make_shared<Topology>(plan);
    const auto of = [&rep](int d) { return d >= 0 ? rep[std::size_t(d)] : -1; };
    std::vector<int> renumbered(plan.boundaries.size(), -1);
    out->boundaries.clear();
    boundaryOf->clear();
    for (std::size_t b = 0; b < plan.boundaries.size(); ++b) {
        const Topology::Boundary& bd = plan.boundaries[b];
        if (bd.up >= 0 && bd.down >= 0 && of(bd.up) == of(bd.down)) continue;
        renumbered[b] = int(out->boundaries.size());
        Topology::Boundary kept = bd;
        kept.up = of(bd.up);
        kept.down = of(bd.down);
        out->boundaries.push_back(kept);
        boundaryOf->push_back(int(b));
    }
    for (Topology::Segment& seg : out->segments) {
        if (seg.boundary >= 0) seg.boundary = renumbered[std::size_t(seg.boundary)];
        seg.left = of(seg.left);
        seg.right = of(seg.right);
    }
    out->points.clear();
    pointOf->clear();
    for (std::size_t q = 0; q < plan.points.size(); ++q) {
        Topology::Point kept = plan.points[q];
        kept.boundaries.clear();
        for (int b : plan.points[q].boundaries)
            if (renumbered[std::size_t(b)] >= 0) kept.boundaries.push_back(renumbered[std::size_t(b)]);
        if (kept.boundaries.empty()) continue;
        out->points.push_back(kept);
        pointOf->push_back(int(q));
    }
    // (A representative's extent its domains'; the others empty.)
    for (std::size_t d = 0; d < plan.domains.size(); ++d) {
        const int r = rep[d];
        if (r == int(d)) continue;
        Topology::Domain& to = out->domains[std::size_t(r)];
        to.bounds = to.bounds.united(plan.domains[d].bounds);
        to.area += plan.domains[d].area;
        out->domains[d].area = 0;
        out->domains[d].triangles.clear();
    }
    return out;
}

/// The bands of a 3D mesh by band: each layer's sizes - each cell's (\a
/// cellMax by plan domain, \a wallMax by boundary, \a riseMax by point, each
/// by slab) grown at the growth rate by its distance in z, the least -, the
/// layers of the same sizes to within a factor of four in a band. An
/// interface is kept off the levels (in the air, the finer band taking the
/// layer), each band's plan domains merged where they are the same domain
/// and have the same floors through it, and its plan's sizes the least of
/// its layers'. One band: empty.
std::vector<BandPlan> bandPlans(const Topology3& t, const MeshSizes& s, const std::vector<double>& heights, const std::vector<int>& levelHeight,
                                const std::vector<int>& slabOf, const std::vector<char>& isLevel, const std::vector<double>& cellMax,
                                const std::vector<double>& wallMax, const std::vector<double>& riseMax)
{
    const Topology& plan = *t.plan;
    const int K = t.slabs(), L = int(t.levels.size()), NL = int(heights.size()) - 1;
    const int D = int(plan.domains.size()), B = int(plan.boundaries.size()), P = int(plan.points.size());
    const double g = std::max(1.0, s.growth) - 1;
    const auto distance = [&t](double z, int k) {
        const double lo = t.levels[std::size_t(k)], hi = t.levels[std::size_t(k) + 1];
        return z < lo ? lo - z : z > hi ? z - hi : 0.0;
    };
    // Each layer's sizes: plan domains', boundaries', points'.
    const int W = D + B + P;
    std::vector<double> size(std::size_t(NL) * std::size_t(W), Infinity);
    for (int j = 0; j < NL; ++j) {
        const double z = 0.5 * (heights[std::size_t(j)] + heights[std::size_t(j) + 1]);
        double* row = &size[std::size_t(j) * std::size_t(W)];
        for (int k = 0; k < K; ++k) {
            const double grown = g * distance(z, k);
            for (int x = 0; x < D; ++x) row[x] = std::min(row[x], cellMax[std::size_t(x) * std::size_t(K) + std::size_t(k)] + grown);
            for (int x = 0; x < B; ++x) row[D + x] = std::min(row[D + x], wallMax[std::size_t(x) * std::size_t(K) + std::size_t(k)] + grown);
            for (int x = 0; x < P; ++x) row[D + B + x] = std::min(row[D + B + x], riseMax[std::size_t(x) * std::size_t(K) + std::size_t(k)] + grown);
        }
    }
    // (Its step: 0 at the mesh's largest size or more, one more each fourth.)
    const auto step = [&s](double h) { return h < s.hmax ? 1 + int(std::floor(std::log(s.hmax / h) / std::log(4.0))) : 0; };
    std::vector<int> fineness(std::size_t(NL), 0);
    const auto sameSteps = [&](int a, int b) {
        for (int x = 0; x < W; ++x)
            if (step(size[std::size_t(a) * std::size_t(W) + std::size_t(x)]) != step(size[std::size_t(b) * std::size_t(W) + std::size_t(x)])) return false;
        return true;
    };
    for (int j = 0; j < NL; ++j)
        for (int x = 0; x < W; ++x) fineness[std::size_t(j)] += step(size[std::size_t(j) * std::size_t(W) + std::size_t(x)]);
    // Runs of layers alike: [first, last) of heights.
    std::vector<std::pair<int, int>> runs;
    for (int j = 0; j < NL; ++j) {
        if (runs.empty() || !sameSteps(runs.back().first, j)) runs.emplace_back(j, j + 1);
        else runs.back().second = j + 1;
    }
    // Off the levels: the finer of the two takes the layer - which, decided
    // once, from the two runs as they were (asked again after a step, the
    // layer just moved said the other, and two levels in a row bounced it
    // for ever).
    for (std::size_t r = 1; r < runs.size();) {
        int& j = runs[r].first;
        bool merged = false;
        const bool lowerFiner = fineness[std::size_t(runs[r - 1].first)] >= fineness[std::size_t(runs[r].first)];
        while (isLevel[std::size_t(j)]) {
            if (lowerFiner) ++j;
            else --j;
            runs[r - 1].second = j;
            if (runs[r].first >= runs[r].second) {
                runs[r - 1].second = runs[r].second;
                runs.erase(runs.begin() + std::ptrdiff_t(r));
                merged = true;
                break;
            }
            if (runs[r - 1].first >= runs[r - 1].second) {
                runs[r].first = runs[r - 1].first;
                runs.erase(runs.begin() + std::ptrdiff_t(r - 1));
                merged = true;
                break;
            }
        }
        if (!merged) ++r;
    }
    if (runs.size() <= 1) return {};
    // Each band's sizes: the least of its layers'. At each interface one
    // side constrained, the finer (by its steps, the lower of two alike) -
    // and made finer or as fine everywhere: the other side's sizes taken
    // where they are smaller, so that on its interface it is the finer
    // throughout (no master constrained, no chain).
    const std::size_t R = runs.size();
    std::vector<std::vector<double>> leasts(R, std::vector<double>(std::size_t(W), Infinity));
    for (std::size_t r = 0; r < R; ++r)
        for (int j = runs[r].first; j < runs[r].second; ++j)
            for (int x = 0; x < W; ++x) leasts[r][std::size_t(x)] = std::min(leasts[r][std::size_t(x)], size[std::size_t(j) * std::size_t(W) + std::size_t(x)]);
    std::vector<char> finerThanNext(R, 0);
    {
        const std::vector<std::vector<double>> own = leasts;
        const auto steps = [&](std::size_t r) {
            int n = 0;
            for (int x = 0; x < W; ++x) n += step(own[r][std::size_t(x)]);
            return n;
        };
        for (std::size_t r = 0; r + 1 < R; ++r) {
            const bool lower = steps(r) >= steps(r + 1);
            finerThanNext[r] = lower;
            std::vector<double>& finer = leasts[lower ? r : r + 1];
            const std::vector<double>& other = own[lower ? r + 1 : r];
            for (int x = 0; x < W; ++x) finer[std::size_t(x)] = std::min(finer[std::size_t(x)], other[std::size_t(x)]);
        }
    }
    std::vector<BandPlan> out;
    for (std::size_t r = 0; r < R; ++r) {
        const int first = runs[r].first, last = runs[r].second;
        BandPlan bp;
        bp.first = first;
        bp.last = last;
        const int k0 = slabOf[std::size_t(first)], k1 = slabOf[std::size_t(last - 1)];
        // Its plan domains merged: the same domain in each of its slabs, the
        // same floors at each of its levels, no wall or rim between them.
        std::vector<int> parent(static_cast<std::size_t>(D), 0);
        for (int d = 0; d < D; ++d) parent[std::size_t(d)] = d;
        std::function<int(int)> find = [&](int d) { return parent[std::size_t(d)] == d ? d : parent[std::size_t(d)] = find(parent[std::size_t(d)]); };
        bool any = false;
        for (int b = 0; b < B; ++b) {
            const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
            if (bd.up < 0 || bd.down < 0) continue;
            bool same = true;
            for (int k = k0; k <= k1 && same; ++k)
                same = t.cell(bd.up, k) == t.cell(bd.down, k) && t.wallFace[std::size_t(b) * std::size_t(K) + std::size_t(k)] < 0;
            for (int i = 0; i < L && same; ++i) {
                if (levelHeight[std::size_t(i)] < first || levelHeight[std::size_t(i)] > last) continue;
                same = t.floorFace[std::size_t(bd.up) * std::size_t(L) + std::size_t(i)] == t.floorFace[std::size_t(bd.down) * std::size_t(L) + std::size_t(i)]
                       && t.rimEdge[std::size_t(b) * std::size_t(L) + std::size_t(i)] < 0;
            }
            if (!same) continue;
            const int a = find(bd.up), c = find(bd.down);
            if (a == c) continue;
            parent[std::size_t(std::max(a, c))] = std::min(a, c);
            any = true;
        }
        const std::vector<double>& least = leasts[r];
        bp.finerThanNext = finerThanNext[r] != 0;
        bp.sizes = s;
        bp.sizes.periodic.clear();
        if (any) {
            bp.merged.resize(std::size_t(D));
            for (int d = 0; d < D; ++d) bp.merged[std::size_t(d)] = find(d);
            bp.topology = mergedPlan(plan, bp.merged, &bp.boundaryOf, &bp.pointOf);
            // A merged class's size its largest domain's (the air about a
            // footprint); a smaller one asked where a footprint was - along
            // its boundaries left out, at sources the plan grows from at the
            // growth rate - not through all the class (the least through it
            // meshed a whole band as fine as a wire's footprint).
            bp.sizes.domainMax.assign(std::size_t(D), Infinity);
            std::vector<int> largest(std::size_t(D), -1);
            for (int d = 0; d < D; ++d) {
                int& l = largest[std::size_t(bp.merged[std::size_t(d)])];
                if (l < 0 || plan.domains[std::size_t(d)].area > plan.domains[std::size_t(l)].area) l = d;
            }
            for (int d = 0; d < D; ++d)
                if (largest[std::size_t(d)] >= 0) bp.sizes.domainMax[std::size_t(d)] = least[std::size_t(largest[std::size_t(d)])];
            for (int b = 0; b < B; ++b) {
                const Topology::Boundary& bd = plan.boundaries[std::size_t(b)];
                if (bd.up < 0 || bd.down < 0 || bp.merged[std::size_t(bd.up)] != bp.merged[std::size_t(bd.down)]) continue;
                const double base = bp.sizes.domainMax[std::size_t(bp.merged[std::size_t(bd.up)])];
                const double h = std::max({std::min({least[std::size_t(bd.up)], least[std::size_t(bd.down)], least[std::size_t(D + b)]}), s.hmin, 1e-12});
                if (!(h < base) || !std::isfinite(h)) continue;
                const std::vector<QPointF> line = bd.polyline(plan);
                for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                    const QPointF a = line[i], c = line[i + 1];
                    const int n = std::clamp(int(std::ceil(std::hypot(c.x() - a.x(), c.y() - a.y()) / h)), 1, 100000);
                    for (int k = 0; k < n; ++k) {
                        const QPointF q = a + (c - a) * (double(k) / n);
                        bp.sizes.points.push_back({q.x(), q.y(), h});
                    }
                }
                if (!line.empty()) bp.sizes.points.push_back({line.back().x(), line.back().y(), h});
            }
            const auto byBoundary = [&bp](const auto& v, auto none) {
                std::decay_t<decltype(v)> r(bp.boundaryOf.size(), none);
                if (v.size() >= std::size_t(1))
                    for (std::size_t i = 0; i < bp.boundaryOf.size(); ++i)
                        if (std::size_t(bp.boundaryOf[i]) < v.size()) r[i] = v[std::size_t(bp.boundaryOf[i])];
                return r;
            };
            std::vector<double> walls(static_cast<std::size_t>(B), Infinity);
            for (int b = 0; b < B; ++b) walls[std::size_t(b)] = least[std::size_t(D + b)];
            bp.sizes.boundaryMax = byBoundary(walls, Infinity);
            bp.sizes.boundaryCount = byBoundary(s.boundaryCount, 0);
            if (!s.layerWalls.empty()) bp.sizes.layerWalls = byBoundary(s.layerWalls, char(0));
            bp.sizes.copyOf.assign(bp.boundaryOf.size(), -1);
            bp.sizes.copyReversed.assign(bp.boundaryOf.size(), 0);
            bp.sizes.pointMax.assign(bp.pointOf.size(), Infinity);
            for (std::size_t q = 0; q < bp.pointOf.size(); ++q) bp.sizes.pointMax[q] = least[std::size_t(D + B + bp.pointOf[q])];
        } else {
            bp.topology = t.plan;
            bp.sizes.domainMax.assign(least.begin(), least.begin() + D);
            bp.sizes.boundaryMax.assign(least.begin() + D, least.begin() + D + B);
            bp.sizes.pointMax.assign(least.begin() + D + B, least.end());
        }
        out.push_back(std::move(bp));
    }
    return out;
}

/// Each boundary \a fixed names - held by two bands or more, meshed with one
/// count in each - made one polyline in all: the nodes any of them has on it
/// put in the others' (their boundary edges split there). With one count
/// the only nodes apart are the plan mesher's splits, each an edge's middle,
/// so no two nodes come near (a union of each band's own discretization -
/// a straight wall's ten - left nodes 0.0002 mm apart, triangles of 0.004°).
void matchBandBoundaries(std::vector<std::shared_ptr<Mesh>>& plans, const std::vector<char>& fixed)
{
    const int B = int(fixed.size());
    for (int b = 0; b < B; ++b) {
        if (!fixed[std::size_t(b)]) continue;
        for (int pass = 0; pass < 16; ++pass) {
            // Each plan's edges on it, and its nodes there.
            std::vector<std::vector<int>> edgesOn(plans.size());
            std::vector<QPointF> all;
            double shortest = Infinity;
            for (std::size_t m = 0; m < plans.size(); ++m)
                for (int e = 0; e < int(plans[m]->edges.size()); ++e) {
                    const Mesh::Edge& ed = plans[m]->edges[std::size_t(e)];
                    if (ed.boundary != b) continue;
                    edgesOn[m].push_back(e);
                    const QPointF a = plans[m]->nodes[std::size_t(ed.a)], c = plans[m]->nodes[std::size_t(ed.b)];
                    shortest = std::min(shortest, std::hypot(c.x() - a.x(), c.y() - a.y()));
                    all.push_back(a);
                    all.push_back(c);
                }
            // (The same node: within a thousandth of its shortest edge.)
            const double tol = 1e-3 * shortest;
            const auto close = [tol](QPointF a, QPointF c) { return std::abs(a.x() - c.x()) <= tol && std::abs(a.y() - c.y()) <= tol; };
            std::sort(all.begin(), all.end(), [](QPointF a, QPointF c) { return a.x() < c.x() || (a.x() == c.x() && a.y() < c.y()); });
            std::vector<QPointF> unique;
            for (const QPointF q : all)
                if (std::none_of(unique.begin(), unique.end(), [&](QPointF x) { return close(x, q); })) unique.push_back(q);
            bool split = false;
            for (std::size_t m = 0; m < plans.size(); ++m) {
                if (edgesOn[m].empty()) continue;
                Mesh& pl = *plans[m];
                for (const QPointF q : unique) {
                    int best = -1;
                    double bestDist = Infinity, bestS = 0;
                    bool there = false;
                    for (int e : edgesOn[m]) {
                        const Mesh::Edge& ed = pl.edges[std::size_t(e)];
                        const QPointF a = pl.nodes[std::size_t(ed.a)], c = pl.nodes[std::size_t(ed.b)];
                        if (close(a, q) || close(c, q)) {
                            there = true;
                            break;
                        }
                        const QPointF d = c - a;
                        const double l2 = d.x() * d.x() + d.y() * d.y();
                        if (!(l2 > 0)) continue;
                        const double u = ((q.x() - a.x()) * d.x() + (q.y() - a.y()) * d.y()) / l2;
                        if (u <= 0.05 || u >= 0.95) continue;
                        // (An arc's node off the chord by its sagitta: the
                        // nearest edge whose span it is in.)
                        const double dist = std::hypot(a.x() + u * d.x() - q.x(), a.y() + u * d.y() - q.y());
                        if (dist < bestDist && dist <= 0.25 * std::sqrt(l2)) {
                            best = e;
                            bestDist = dist;
                            bestS = u;
                        }
                    }
                    if (there || best < 0) continue;
                    splitBoundaryEdge(pl, best, q, bestS);
                    edgesOn[m].push_back(int(pl.edges.size()) - 1);
                    split = true;
                }
            }
            if (!split) break;
        }
    }
    for (auto& pl : plans) pl->computeStatistics();
}

/// Plan domain \a pd of \a master put in \a target in place of its own -
/// two bands' plans made one there, its rim's nodes the same in both
/// (matchBandBoundaries() first): its triangles copied, its inner nodes
/// added, the target's old ones there left out, the boundary edges told
/// their triangles again.
void adoptDomain(Mesh& target, const Mesh& master, int pd)
{
    // The rim's nodes: those of the boundary edges by the domain.
    const auto rimOf = [pd](const Mesh& m) {
        std::vector<int> rim;
        for (const Mesh::Edge& e : m.edges) {
            const bool by = (e.left >= 0 && m.domain[std::size_t(e.left)] == pd) || (e.right >= 0 && m.domain[std::size_t(e.right)] == pd);
            if (!by) continue;
            rim.push_back(e.a);
            rim.push_back(e.b);
        }
        std::sort(rim.begin(), rim.end());
        rim.erase(std::unique(rim.begin(), rim.end()), rim.end());
        return rim;
    };
    const std::vector<int> rimT = rimOf(target), rimM = rimOf(master);
    std::map<std::pair<double, double>, int> at;
    for (int n : rimT) at[{target.nodes[std::size_t(n)].x(), target.nodes[std::size_t(n)].y()}] = n;
    std::vector<int> map(master.nodes.size(), -1);
    for (int n : rimM) {
        const auto it = at.find({master.nodes[std::size_t(n)].x(), master.nodes[std::size_t(n)].y()});
        if (it != at.end()) map[std::size_t(n)] = it->second;
    }
    std::vector<std::array<int, 3>> triangles;
    std::vector<int> domain;
    for (std::size_t i = 0; i < target.triangles.size(); ++i)
        if (target.domain[i] != pd) {
            triangles.push_back(target.triangles[i]);
            domain.push_back(target.domain[i]);
        }
    for (std::size_t i = 0; i < master.triangles.size(); ++i) {
        if (master.domain[i] != pd) continue;
        std::array<int, 3> tri{};
        for (int c = 0; c < 3; ++c) {
            int& to = map[std::size_t(master.triangles[i][std::size_t(c)])];
            if (to < 0) {
                to = int(target.nodes.size());
                target.nodes.push_back(master.nodes[std::size_t(master.triangles[i][std::size_t(c)])]);
            }
            tri[std::size_t(c)] = to;
        }
        triangles.push_back(tri);
        domain.push_back(pd);
    }
    // Nodes no triangle uses left out.
    std::vector<int> renumber(target.nodes.size(), -1);
    std::vector<QPointF> nodes;
    for (std::array<int, 3>& tri : triangles)
        for (int& v : tri) {
            if (renumber[std::size_t(v)] < 0) {
                renumber[std::size_t(v)] = int(nodes.size());
                nodes.push_back(target.nodes[std::size_t(v)]);
            }
            v = renumber[std::size_t(v)];
        }
    target.nodes = std::move(nodes);
    target.triangles = std::move(triangles);
    target.domain = std::move(domain);
    for (int& n : target.pointNode)
        if (n >= 0) n = renumber[std::size_t(n)];
    std::map<std::pair<int, int>, int> leftOf;
    for (std::size_t i = 0; i < target.triangles.size(); ++i)
        for (int c = 0; c < 3; ++c) leftOf[{target.triangles[i][std::size_t(c)], target.triangles[i][std::size_t((c + 1) % 3)]}] = int(i);
    for (Mesh::Edge& e : target.edges) {
        e.a = renumber[std::size_t(e.a)];
        e.b = renumber[std::size_t(e.b)];
        const auto l = leftOf.find({e.a, e.b}), r = leftOf.find({e.b, e.a});
        e.left = l != leftOf.end() ? l->second : -1;
        e.right = r != leftOf.end() ? r->second : -1;
    }
    target.computeStatistics();
}

Mesh3Build buildMesh3Within(const Model& model, const ParameterScope& parameters, std::shared_ptr<const Topology3> topology,
                            const Progress& progress)
{
    Mesh3Build out;
    if (!topology || !topology->plan || topology->domains.empty()) {
        out.error = tr("there is no geometry: build it first");
        return out;
    }
    const Topology3& t = *topology;
    const Topology& plan = *t.plan;
    const int K = t.slabs(), L = int(t.levels.size());
    const int D = int(plan.domains.size()), B = int(plan.boundaries.size()), P = int(plan.points.size());
    QStringList problems;
    // The sizes for all: the preset's of its largest side, its height too.
    const double extent = std::max({t.bounds.x1 - t.bounds.x0, t.bounds.y1 - t.bounds.y0, t.bounds.z1 - t.bounds.z0});
    QString why;
    MeshSizes s = globalMeshSizes(model, parameters, extent, t.unitScale, &why);
    if (!why.isEmpty()) problems << why;
    s.domainMax.assign(std::size_t(D), Infinity);
    s.boundaryMax.assign(std::size_t(B), Infinity);
    s.pointMax.assign(std::size_t(P), Infinity);
    s.boundaryCount.assign(std::size_t(B), 0);
    std::vector<double> zmax(std::size_t(K), s.hmax), hLevel(std::size_t(L), Infinity);
    std::vector<int> zcount(std::size_t(K), 0);
    // (By band: the sizes kept by cell - a plan domain's, a boundary's, a
    // point's in each slab -, not through all the slabs.)
    std::vector<double> cellMax(std::size_t(D) * std::size_t(std::max(K, 1)), Infinity);
    std::vector<double> wallMax(std::size_t(B) * std::size_t(std::max(K, 1)), Infinity);
    std::vector<double> riseMax(std::size_t(P) * std::size_t(std::max(K, 1)), Infinity);
    // (A level's: the slabs on either side of it.)
    auto atLevel = [K](std::vector<double>& v, int x, int level, double h) {
        for (int k : {level - 1, level})
            if (k >= 0 && k < K) v[std::size_t(x) * std::size_t(K) + std::size_t(k)] = std::min(v[std::size_t(x) * std::size_t(K) + std::size_t(k)], h);
    };
    auto inSlab = [K](std::vector<double>& v, int x, int k, double h) {
        if (k >= 0 && k < K) v[std::size_t(x) * std::size_t(K) + std::size_t(k)] = std::min(v[std::size_t(x) * std::size_t(K) + std::size_t(k)], h);
    };
    // (Explicit captures: GCC 12 and 13 crash on long lambdas taking all.)
    auto length = [&problems, &t, &parameters](const QString& text, const QString& what) {
        if (text.trimmed().isEmpty()) return Infinity;
        Expression::Options options;
        options.lengthUnit = t.unitScale;
        options.numbersAreLengths = true;
        const Expression e = Expression::compile(text, parameters.scope, options);
        if (!e.isValid() || !e.isConstant()) {
            problems << tr("%1: %2").arg(what, e.isValid() ? tr("not a constant") : e.error());
            return Infinity;
        }
        if (!(e.constant() > 0)) {
            problems << tr("%1 must be more than 0").arg(what);
            return Infinity;
        }
        return e.constant();
    };
    auto lower = [](double& at, double h) { at = std::min(at, h); };
    // Its features: a size taken to the plan and to the slabs (a swept
    // mesh's price - a domain's size runs through every slab its plan
    // domains fill); so many prisms across a domain's height.
    for (const Node& f : model.mesh().children) {
        if (!f.enabled) continue;
        if (f.type == QLatin1String("mesh/size")) {
            const Level level = levelOf(f.text(QStringLiteral("level")));
            const QVector<int> picked = resolveSelection3(t, model, level, f.selection(), &why);
            if (!why.isEmpty()) problems << tr("%1: %2").arg(f.name(), why);
            const double h = length(f.text(QStringLiteral("hmax")), f.name());
            if (!std::isfinite(h)) continue;
            for (int i : picked) {
                if (level == Level::Domain) {
                    for (const auto& [pd, k] : t.domains[std::size_t(i)].cells) {
                        lower(s.domainMax[std::size_t(pd)], h);
                        lower(zmax[std::size_t(k)], h);
                        inSlab(cellMax, pd, k, h);
                    }
                } else if (level == Level::Boundary) {
                    const Topology3::Face& face = t.faces[std::size_t(i)];
                    for (const auto& [x, y] : face.atoms) {
                        if (face.floor) {
                            lower(s.domainMax[std::size_t(x)], h);
                            lower(hLevel[std::size_t(y)], h);
                            atLevel(cellMax, x, y, h);
                        } else {
                            lower(s.boundaryMax[std::size_t(x)], h);
                            lower(zmax[std::size_t(y)], h);
                            inSlab(wallMax, x, y, h);
                        }
                    }
                } else if (level == Level::Edge) {
                    const Topology3::Edge& edge = t.edges[std::size_t(i)];
                    for (const auto& [x, y] : edge.atoms) {
                        if (edge.rise) {
                            lower(s.pointMax[std::size_t(x)], h);
                            lower(zmax[std::size_t(y)], h);
                            inSlab(riseMax, x, y, h);
                        } else {
                            lower(s.boundaryMax[std::size_t(x)], h);
                            lower(hLevel[std::size_t(y)], h);
                            atLevel(wallMax, x, y, h);
                        }
                    }
                } else if (level == Level::Point) {
                    const Topology3::Point& point = t.points[std::size_t(i)];
                    lower(s.pointMax[std::size_t(point.planPoint)], h);
                    lower(hLevel[std::size_t(point.level)], h);
                    atLevel(riseMax, point.planPoint, point.level, h);
                }
            }
        } else if (f.type == QLatin1String("mesh/zdistribution")) {
            const QVector<int> picked = resolveSelection3(t, model, Level::Domain, f.selection(), &why);
            if (!why.isEmpty()) problems << tr("%1: %2").arg(f.name(), why);
            const int n = std::clamp(f.integer(QStringLiteral("count")), 1, 100000);
            for (int d : picked) {
                std::set<int> slabs;
                for (const auto& [pd, k] : t.domains[std::size_t(d)].cells) slabs.insert(k);
                double height = 0;
                for (int k : slabs) height += t.levels[std::size_t(k) + 1] - t.levels[std::size_t(k)];
                for (int k : slabs) {
                    const double thick = t.levels[std::size_t(k) + 1] - t.levels[std::size_t(k)];
                    zcount[std::size_t(k)] = std::max(zcount[std::size_t(k)], std::max(1, int(std::lround(n * thick / height))));
                }
            }
        } else {
            const NodeKind* k = f.kind();
            problems << tr("%1: a %2 is not a 3D mesh's (yet)").arg(f.name(), k ? k->title : f.type);
        }
    }
    // Each slab's heights: its levels and its divisions (the plans' sizes
    // do not move them).
    auto mesh = std::make_shared<Mesh3>();
    mesh->topology = topology;
    mesh->unitScale = t.unitScale;
    const std::vector<std::vector<double>> divisions = sweptDivisions(t.levels, zmax, zcount, hLevel, s.growth);
    for (int k = 0; k < K; ++k) {
        mesh->levelHeight.push_back(int(mesh->heights.size()));
        mesh->heights.push_back(t.levels[std::size_t(k)]);
        for (double z : divisions[std::size_t(k)]) mesh->heights.push_back(z);
    }
    mesh->levelHeight.push_back(int(mesh->heights.size()));
    mesh->heights.push_back(t.levels.back());
    const int H = int(mesh->heights.size());
    std::vector<int> slabOf(std::size_t(std::max(H - 1, 0)), 0);
    for (int k = 0; k < K; ++k)
        for (int j = mesh->levelHeight[std::size_t(k)]; j < mesh->levelHeight[std::size_t(k) + 1]; ++j) slabOf[std::size_t(j)] = k;
    std::vector<char> isLevel(std::size_t(H), 0);
    for (int j : mesh->levelHeight) isLevel[std::size_t(j)] = 1;
    // The bands: one, its plan of the sizes through all the slabs; or, by
    // band, runs of layers alike in their sizes, each its own plan.
    std::vector<BandPlan> plans;
    if (model.mesh().text(QStringLiteral("plan")) == QLatin1String("band") && K > 0 && H > 2)
        plans = bandPlans(t, s, mesh->heights, mesh->levelHeight, slabOf, isLevel, cellMax, wallMax, riseMax);
    if (plans.size() <= 1) {
        plans.assign(1, BandPlan{});
        plans[0].first = 0;
        plans[0].last = H - 1;
        plans[0].topology = t.plan;
        plans[0].sizes = s;
    }
    const int NB = int(plans.size());
    std::vector<std::shared_ptr<Mesh>> planMeshes(static_cast<std::size_t>(NB));
    // A band's plan meshed (a merged plan's boundaries and points renumbered
    // by the plan's own numbers).
    const auto meshBand = [&](int b) {
        auto planProgress = [&progress, b, NB](double fraction, const QString& what) {
            return !progress || progress(0.8 * (b + fraction) / NB, NB > 1 ? tr("Band %1's plan: %2").arg(b + 1).arg(what) : tr("The plan: %1").arg(what));
        };
        const MeshBuild pm = buildMesh(plans[std::size_t(b)].topology, plans[std::size_t(b)].sizes, planProgress);
        if (!pm.mesh) {
            out.error = pm.error.isEmpty() ? tr("the plan's mesh failed") : tr("the plan: %1").arg(pm.error);
            return false;
        }
        out.warnings << pm.warnings;
        const BandPlan& bp = plans[std::size_t(b)];
        if (!bp.boundaryOf.empty()) {
            for (Mesh::Edge& e : pm.mesh->edges)
                if (e.boundary >= 0) e.boundary = bp.boundaryOf[std::size_t(e.boundary)];
            std::vector<int> nodes(std::size_t(P), -1);
            for (std::size_t q = 0; q < pm.mesh->pointNode.size() && q < bp.pointOf.size(); ++q) nodes[std::size_t(bp.pointOf[q])] = pm.mesh->pointNode[q];
            pm.mesh->pointNode = nodes;
            pm.mesh->topology = t.plan;
        }
        planMeshes[std::size_t(b)] = pm.mesh;
        return true;
    };
    for (int b = 0; b < NB; ++b)
        if (!meshBand(b)) return out;
    // A plan domain whole in consecutive bands - none merged into it: a
    // conductor, a source or a wall through an interface - gets the same
    // triangles in all of them, the finest's: there the interface's degrees
    // of freedom are one, not one side's interpolated from the other's
    // (inside a conductor that error is not small).
    std::vector<std::array<int, 3>> wholeRuns;   // (plan domain, first band, past its last)
    if (NB > 1) {
        const auto whole = [&plans](int b, int pd) {
            const std::vector<int>& m = plans[std::size_t(b)].merged;
            if (m.empty()) return true;
            if (m[std::size_t(pd)] != pd) return false;
            for (std::size_t d = 0; d < m.size(); ++d)
                if (m[d] == pd && int(d) != pd) return false;
            return true;
        };
        for (int pd = 0; pd < D; ++pd)
            for (int b0 = 0; b0 < NB;) {
                int b1 = b0;
                while (b1 < NB && whole(b1, pd)) ++b1;
                if (b1 - b0 >= 2) wholeRuns.push_back({pd, b0, b1});
                b0 = std::max(b1, b0 + 1);
            }
    }
    // The boundaries whose polylines must be one in every band that holds
    // them: a curve's (else the two bands' domains differ by slivers), and
    // a whole domain's rim (its triangles copied). Each gets one count of
    // elements in every band - the most any band gave it -, the bands that
    // hold them meshed again so, then their splits matched. A straight wall
    // of no whole domain is left: its polylines bound the same domain, and
    // where they differ on an interface the constraints join them as they do
    // the rest.
    if (NB > 1) {
        std::vector<char> rim(std::size_t(B), 0), fixed(std::size_t(B), 0);
        for (const auto& [pd, b0, b1] : wholeRuns)
            for (int b = 0; b < B; ++b)
                if (plan.boundaries[std::size_t(b)].up == pd || plan.boundaries[std::size_t(b)].down == pd) rim[std::size_t(b)] = 1;
        std::vector<int> count(std::size_t(B), 0), holders(std::size_t(B), 0);
        for (int b = 0; b < NB; ++b) {
            std::vector<int> here(std::size_t(B), 0);
            for (const Mesh::Edge& e : planMeshes[std::size_t(b)]->edges)
                if (e.boundary >= 0) ++here[std::size_t(e.boundary)];
            for (int x = 0; x < B; ++x)
                if (here[std::size_t(x)] > 0) {
                    ++holders[std::size_t(x)];
                    count[std::size_t(x)] = std::max(count[std::size_t(x)], here[std::size_t(x)]);
                }
        }
        bool any = false;
        for (int x = 0; x < B; ++x) {
            const Topology::Boundary& bd = plan.boundaries[std::size_t(x)];
            const bool curved = bd.curve >= 0 && plan.curves[std::size_t(bd.curve)].kind != Curve::Line;
            fixed[std::size_t(x)] = holders[std::size_t(x)] >= 2 && (curved || rim[std::size_t(x)]);
            any = any || fixed[std::size_t(x)];
        }
        if (any) {
            for (int b = 0; b < NB; ++b) {
                BandPlan& bp = plans[std::size_t(b)];
                bool touches = false;
                const int nb = bp.boundaryOf.empty() ? B : int(bp.boundaryOf.size());
                if (int(bp.sizes.boundaryCount.size()) < nb) bp.sizes.boundaryCount.resize(std::size_t(nb), 0);
                for (int local = 0; local < nb; ++local) {
                    const int x = bp.boundaryOf.empty() ? local : bp.boundaryOf[std::size_t(local)];
                    if (!fixed[std::size_t(x)]) continue;
                    bp.sizes.boundaryCount[std::size_t(local)] = count[std::size_t(x)];
                    touches = true;
                }
                if (touches && !meshBand(b)) return out;
            }
            matchBandBoundaries(planMeshes, fixed);
        }
        for (const auto& [pd, b0, b1] : wholeRuns) {
            int master = b0;
            for (int b = b0; b < b1; ++b)
                if (plans[std::size_t(b)].sizes.domainMax[std::size_t(pd)] < plans[std::size_t(master)].sizes.domainMax[std::size_t(pd)]) master = b;
            for (int b = b0; b < b1; ++b)
                if (b != master) adoptDomain(*planMeshes[std::size_t(b)], *planMeshes[std::size_t(master)], pd);
        }
    }
    for (int b = 0; b + 1 < NB; ++b) mesh->interfaceFiner.push_back(plans[std::size_t(b)].finerThanNext ? b : b + 1);
    if (progress && !progress(0.8, tr("Sweeping the prisms"))) {
        out.error = tr("cancelled");
        return out;
    }
    // Its prisms counted before they are made: a size far smaller than the
    // geometry (1e-9 for 1e-3, a slip) asked for more than the memory
    // holds, and running out of it in the mesh's thread aborted Qucs-S
    {
        double prisms = 0, nodeSlots = 0, triangles = 0;
        for (int b = 0; b < NB; ++b) {
            const Mesh& m2 = *planMeshes[std::size_t(b)];
            const BandPlan& bp = plans[std::size_t(b)];
            triangles += double(m2.triangles.size());
            nodeSlots += double(m2.nodes.size()) * double(bp.last - bp.first + 1);
            for (std::size_t i = 0; i < m2.triangles.size(); ++i)
                for (int j = bp.first; j < bp.last; ++j)
                    if (t.cell(m2.domain[i], slabOf[std::size_t(j)]) >= 0) prisms += 1;
        }
        if (prisms > double(MaxPrisms) || nodeSlots > double(MaxNodeSlots)) {
            out.error = tr("the mesh would have about %1 prisms (%2 plan triangles, %3 heights), more than %4: a larger size, or "
                           "fewer divisions across the heights")
                            .arg(prisms, 0, 'g', 3)
                            .arg(triangles)
                            .arg(H)
                            .arg(double(MaxPrisms), 0, 'g', 3);
            return out;
        }
    }
    mesh->plan = planMeshes.front();
    // Each band's nodes, numbered by band, by height, then by plan node; its
    // prisms; its floors' triangles and walls' quadrilaterals.
    for (int b = 0; b < NB; ++b) {
        const Mesh& m2 = *planMeshes[std::size_t(b)];
        const BandPlan& bp = plans[std::size_t(b)];
        Mesh3::Band band;
        band.plan = planMeshes[std::size_t(b)];
        band.first = bp.first;
        band.last = bp.last;
        band.merged = bp.merged;
        const std::size_t N = m2.nodes.size(), HB = std::size_t(bp.last - bp.first + 1);
        std::vector<char> used(N * HB, 0);
        for (std::size_t i = 0; i < m2.triangles.size(); ++i)
            for (int j = bp.first; j < bp.last; ++j) {
                if (t.cell(m2.domain[i], slabOf[std::size_t(j)]) < 0) continue;
                for (int n : m2.triangles[i]) {
                    used[std::size_t(j - bp.first) * N + std::size_t(n)] = 1;
                    used[std::size_t(j + 1 - bp.first) * N + std::size_t(n)] = 1;
                }
            }
        band.nodeAt.assign(N * HB, -1);
        // (At its interface with the band below, a node where one of that
        // band's is: that one - the two plans' same nodes one node.)
        std::map<std::pair<double, double>, int> below;
        if (b > 0) {
            const Mesh3::Band& under = mesh->bands.back();
            const std::size_t NU = under.plan->nodes.size(), top = std::size_t(under.last - under.first);
            for (std::size_t n = 0; n < NU; ++n) {
                const int node = under.nodeAt[top * NU + n];
                if (node >= 0) below[{under.plan->nodes[n].x(), under.plan->nodes[n].y()}] = node;
            }
        }
        for (std::size_t j = 0; j < HB; ++j)
            for (std::size_t n = 0; n < N; ++n) {
                if (!used[j * N + n]) continue;
                if (j == 0 && b > 0) {
                    const auto it = below.find({m2.nodes[n].x(), m2.nodes[n].y()});
                    if (it != below.end()) {
                        band.nodeAt[n] = it->second;
                        continue;
                    }
                }
                band.nodeAt[j * N + n] = int(mesh->nodes.size());
                mesh->nodes.push_back(Vec3{m2.nodes[n].x(), m2.nodes[n].y(), mesh->heights[std::size_t(bp.first) + j]});
                mesh->nodeOrigin.emplace_back(int(n), bp.first + int(j));
                mesh->nodeBand.push_back(b);
            }
        auto at = [&band, &bp, N](int n, int j) { return band.nodeAt[std::size_t(j - bp.first) * N + std::size_t(n)]; };
        for (std::size_t i = 0; i < m2.triangles.size(); ++i) {
            const std::array<int, 3>& tri = m2.triangles[i];
            for (int j = bp.first; j < bp.last; ++j) {
                const int d = t.cell(m2.domain[i], slabOf[std::size_t(j)]);
                if (d < 0) continue;
                mesh->prisms.push_back({at(tri[0], j), at(tri[1], j), at(tri[2], j), at(tri[0], j + 1), at(tri[1], j + 1), at(tri[2], j + 1)});
                mesh->domain.push_back(d);
                mesh->prismTriangle.push_back(int(i));
                mesh->prismLayer.push_back(j);
                mesh->prismBand.push_back(b);
            }
        }
        // Its floors: the levels it holds (not an interface's height: no
        // level is one).
        for (int i = 0; i < L; ++i) {
            const int j = mesh->levelHeight[std::size_t(i)];
            if (j < bp.first || j > bp.last) continue;
            for (std::size_t tr3 = 0; tr3 < m2.triangles.size(); ++tr3) {
                const int face = t.floorFace[std::size_t(m2.domain[tr3]) * std::size_t(L) + std::size_t(i)];
                if (face < 0) continue;
                const std::array<int, 3>& tri = m2.triangles[tr3];
                Mesh3::Face f;
                f.nodes = {at(tri[0], j), at(tri[1], j), at(tri[2], j), -1};
                f.face = face;
                f.plan = int(tr3);
                f.height = j;
                f.band = b;
                mesh->faces.push_back(f);
            }
        }
        // Its walls: its plan's boundary edges through its layers.
        for (std::size_t ei = 0; ei < m2.edges.size(); ++ei) {
            const Mesh::Edge& e = m2.edges[ei];
            if (e.boundary < 0) continue;
            const Topology::Boundary& bd = plan.boundaries[std::size_t(e.boundary)];
            for (int j = bp.first; j < bp.last; ++j) {
                const int k = slabOf[std::size_t(j)];
                const int face = t.wallFace[std::size_t(e.boundary) * std::size_t(K) + std::size_t(k)];
                if (face < 0) continue;
                // (Seen from its front: the boundary's right, unless the face
                // runs the other way here.)
                const bool turned = t.cell(bd.up, k) != t.faces[std::size_t(face)].back;
                Mesh3::Face f;
                f.quad = true;
                f.face = face;
                f.plan = int(ei);
                f.height = j;
                f.band = b;
                if (!turned) f.nodes = {at(e.a, j), at(e.b, j), at(e.b, j + 1), at(e.a, j + 1)};
                else f.nodes = {at(e.a, j), at(e.a, j + 1), at(e.b, j + 1), at(e.b, j)};
                mesh->faces.push_back(f);
            }
        }
        mesh->bands.push_back(std::move(band));
    }
    // One band: its node table the mesh's (Mesh3::node()).
    if (NB == 1) {
        mesh->nodeAt = std::move(mesh->bands.front().nodeAt);
        mesh->bands.front().nodeAt.clear();
    }
    for (const Topology3::Point& p : t.points) {
        const int j = mesh->levelHeight[std::size_t(p.level)];
        int node = -1;
        for (int b = 0; b < NB && node < 0; ++b) {
            if (j < plans[std::size_t(b)].first || j > plans[std::size_t(b)].last) continue;
            const Mesh& m2 = *planMeshes[std::size_t(b)];
            const int n = p.planPoint < int(m2.pointNode.size()) ? m2.pointNode[std::size_t(p.planPoint)] : -1;
            if (n >= 0) node = mesh->node(b, n, j);
        }
        mesh->pointNode.push_back(node);
    }
    mesh->computeStatistics();
    if (mesh->prisms.empty()) {
        out.error = tr("the mesh has no prisms");
        return out;
    }
    if (!problems.isEmpty()) out.warnings.prepend(problems.join(QStringLiteral("; ")));
    if (progress) progress(1.0, tr("Done"));
    out.mesh = mesh;
    return out;
}

} // namespace

Mesh3Build buildMesh3(const Model& model, const ParameterScope& parameters, std::shared_ptr<const Topology3> topology,
                      const Progress& progress)
{
    // (Out of memory, a failed mesh: thrown in the mesh's thread, it was
    // uncaught and aborted Qucs-S.)
    try {
        return buildMesh3Within(model, parameters, std::move(topology), progress);
    } catch (const std::bad_alloc&) {
        Mesh3Build out;
        out.error = tr("there was not memory enough for the mesh: a larger size makes a smaller one");
        return out;
    }
}

} // namespace qucs_s::fem
