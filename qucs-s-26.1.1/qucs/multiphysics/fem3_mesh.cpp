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
    if (!plan || planNode < 0 || height < 0 || planNode >= int(plan->nodes.size()) || height >= int(heights.size())) return -1;
    return nodeAt[std::size_t(height) * plan->nodes.size() + std::size_t(planNode)];
}

void Mesh3::computeStatistics()
{
    minAngle = plan ? plan->minAngle : 0;
    minQuality = plan ? plan->minQuality : 0;
    maxStretch = 0;
    meanStretch = 0;
    volume = 0;
    if (!plan) return;
    for (std::size_t i = 0; i < prisms.size(); ++i) {
        const std::array<int, 3>& tri = plan->triangles[std::size_t(prismTriangle[i])];
        const QPointF a = plan->nodes[std::size_t(tri[0])], b = plan->nodes[std::size_t(tri[1])], c = plan->nodes[std::size_t(tri[2])];
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
        const int n = given > 0 ? given : std::max(1, int(std::ceil(total - 1e-6)));
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

Mesh3Build buildMesh3(const Model& model, const ParameterScope& parameters, std::shared_ptr<const Topology3> topology,
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
                    }
                } else if (level == Level::Boundary) {
                    const Topology3::Face& face = t.faces[std::size_t(i)];
                    for (const auto& [x, y] : face.atoms) {
                        if (face.floor) {
                            lower(s.domainMax[std::size_t(x)], h);
                            lower(hLevel[std::size_t(y)], h);
                        } else {
                            lower(s.boundaryMax[std::size_t(x)], h);
                            lower(zmax[std::size_t(y)], h);
                        }
                    }
                } else if (level == Level::Edge) {
                    const Topology3::Edge& edge = t.edges[std::size_t(i)];
                    for (const auto& [x, y] : edge.atoms) {
                        if (edge.rise) {
                            lower(s.pointMax[std::size_t(x)], h);
                            lower(zmax[std::size_t(y)], h);
                        } else {
                            lower(s.boundaryMax[std::size_t(x)], h);
                            lower(hLevel[std::size_t(y)], h);
                        }
                    }
                } else if (level == Level::Point) {
                    const Topology3::Point& point = t.points[std::size_t(i)];
                    lower(s.pointMax[std::size_t(point.planPoint)], h);
                    lower(hLevel[std::size_t(point.level)], h);
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
    // The plan meshed.
    auto planProgress = [&progress](double fraction, const QString& what) {
        return !progress || progress(0.8 * fraction, tr("The plan: %1").arg(what));
    };
    const MeshBuild pm = buildMesh(t.plan, s, planProgress);
    if (!pm.mesh) {
        out.error = pm.error.isEmpty() ? tr("the plan's mesh failed") : tr("the plan: %1").arg(pm.error);
        return out;
    }
    out.warnings << pm.warnings;
    if (progress && !progress(0.8, tr("Sweeping the prisms"))) {
        out.error = tr("cancelled");
        return out;
    }
    const Mesh& m2 = *pm.mesh;
    auto mesh = std::make_shared<Mesh3>();
    mesh->plan = pm.mesh;
    mesh->topology = topology;
    mesh->unitScale = t.unitScale;
    // Each slab's heights: its levels and its divisions.
    const std::vector<std::vector<double>> divisions = sweptDivisions(t.levels, zmax, zcount, hLevel, s.growth);
    for (int k = 0; k < K; ++k) {
        mesh->levelHeight.push_back(int(mesh->heights.size()));
        mesh->heights.push_back(t.levels[std::size_t(k)]);
        for (double z : divisions[std::size_t(k)]) mesh->heights.push_back(z);
    }
    mesh->levelHeight.push_back(int(mesh->heights.size()));
    mesh->heights.push_back(t.levels.back());
    const std::size_t N = m2.nodes.size(), H = mesh->heights.size();
    // The nodes the prisms use, numbered by height, then by plan node.
    std::vector<char> used(N * H, 0);
    for (std::size_t i = 0; i < m2.triangles.size(); ++i) {
        const int pd = m2.domain[i];
        for (int k = 0; k < K; ++k) {
            if (t.cell(pd, k) < 0) continue;
            for (int j = mesh->levelHeight[std::size_t(k)]; j <= mesh->levelHeight[std::size_t(k) + 1]; ++j)
                for (int n : m2.triangles[i]) used[std::size_t(j) * N + std::size_t(n)] = 1;
        }
    }
    mesh->nodeAt.assign(N * H, -1);
    for (std::size_t j = 0; j < H; ++j)
        for (std::size_t n = 0; n < N; ++n) {
            if (!used[j * N + n]) continue;
            mesh->nodeAt[j * N + n] = int(mesh->nodes.size());
            mesh->nodes.push_back(Vec3{m2.nodes[n].x(), m2.nodes[n].y(), mesh->heights[j]});
            mesh->nodeOrigin.emplace_back(int(n), int(j));
        }
    auto at = [&](int n, int j) { return mesh->nodeAt[std::size_t(j) * N + std::size_t(n)]; };
    for (std::size_t i = 0; i < m2.triangles.size(); ++i) {
        const int pd = m2.domain[i];
        const std::array<int, 3>& tri = m2.triangles[i];
        for (int k = 0; k < K; ++k) {
            const int d = t.cell(pd, k);
            if (d < 0) continue;
            for (int j = mesh->levelHeight[std::size_t(k)]; j < mesh->levelHeight[std::size_t(k) + 1]; ++j) {
                mesh->prisms.push_back({at(tri[0], j), at(tri[1], j), at(tri[2], j), at(tri[0], j + 1), at(tri[1], j + 1), at(tri[2], j + 1)});
                mesh->domain.push_back(d);
                mesh->prismTriangle.push_back(int(i));
                mesh->prismLayer.push_back(j);
            }
        }
    }
    // Its boundary faces: the floors' triangles, the walls' quadrilaterals.
    for (int i = 0; i < L; ++i) {
        const int j = mesh->levelHeight[std::size_t(i)];
        for (std::size_t tr3 = 0; tr3 < m2.triangles.size(); ++tr3) {
            const int pd = m2.domain[tr3];
            const int face = t.floorFace[std::size_t(pd) * std::size_t(L) + std::size_t(i)];
            if (face < 0) continue;
            const std::array<int, 3>& tri = m2.triangles[tr3];
            Mesh3::Face f;
            f.nodes = {at(tri[0], j), at(tri[1], j), at(tri[2], j), -1};
            f.face = face;
            mesh->faces.push_back(f);
        }
    }
    for (const Mesh::Edge& e : m2.edges) {
        if (e.boundary < 0) continue;
        const Topology::Boundary& bd = plan.boundaries[std::size_t(e.boundary)];
        for (int k = 0; k < K; ++k) {
            const int face = t.wallFace[std::size_t(e.boundary) * std::size_t(K) + std::size_t(k)];
            if (face < 0) continue;
            // (Seen from its front: the boundary's right, unless the face runs
            // the other way here.)
            const bool turned = t.cell(bd.up, k) != t.faces[std::size_t(face)].back;
            for (int j = mesh->levelHeight[std::size_t(k)]; j < mesh->levelHeight[std::size_t(k) + 1]; ++j) {
                Mesh3::Face f;
                f.quad = true;
                f.face = face;
                if (!turned) f.nodes = {at(e.a, j), at(e.b, j), at(e.b, j + 1), at(e.a, j + 1)};
                else f.nodes = {at(e.a, j), at(e.a, j + 1), at(e.b, j + 1), at(e.b, j)};
                mesh->faces.push_back(f);
            }
        }
    }
    for (const Topology3::Point& p : t.points) {
        const int n = p.planPoint < int(m2.pointNode.size()) ? m2.pointNode[std::size_t(p.planPoint)] : -1;
        mesh->pointNode.push_back(n >= 0 ? at(n, mesh->levelHeight[std::size_t(p.level)]) : -1);
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

} // namespace qucs_s::fem
