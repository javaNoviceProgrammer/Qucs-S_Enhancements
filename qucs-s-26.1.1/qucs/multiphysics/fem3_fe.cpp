/*
 * fem3_fe.cpp - the 3D finite elements of the multiphysics solver: Lagrange
 *               prisms, their rules of integration, the space of a swept
 *               mesh's degrees of freedom
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_fe.h"

#include <algorithm>
#include <cmath>

namespace qucs_s::fem {

void prismShapeAt(int order, double xi, double eta, double zeta, PrismShape& s)
{
    Shape t;
    shapeAt(order == 2 ? 2 : 1, xi, eta, t);
    // Along ζ: the line's Lagrange functions of the same order.
    double l[3], dl[3];
    int nl = 2;
    if (order == 2) {
        nl = 3;
        l[0] = (1 - zeta) * (1 - 2 * zeta);
        l[1] = 4 * zeta * (1 - zeta);
        l[2] = zeta * (2 * zeta - 1);
        dl[0] = 4 * zeta - 3;
        dl[1] = 4 - 8 * zeta;
        dl[2] = 4 * zeta - 1;
    } else {
        l[0] = 1 - zeta;
        l[1] = zeta;
        dl[0] = -1;
        dl[1] = 1;
    }
    s.count = t.count * nl;
    for (int j = 0; j < nl; ++j)
        for (int a = 0; a < t.count; ++a) {
            const int i = a + t.count * j;
            s.n[i] = t.n[a] * l[j];
            s.dxi[i] = t.dxi[a] * l[j];
            s.deta[i] = t.deta[a] * l[j];
            s.dzeta[i] = t.n[a] * dl[j];
        }
}

const PrismRule& prismRule(int degree, int n)
{
    // Every pair made once (a function's static: safe from threads).
    static const std::vector<PrismRule> rules = [] {
        std::vector<PrismRule> out(6 * 4);
        for (int d = 0; d <= 5; ++d)
            for (int k = 0; k <= 3; ++k) {
                PrismRule& r = out[std::size_t(d * 4 + k)];
                for (const auto& p : triangleRule(d).points)
                    for (const auto& q : lineRule(k)) r.points.push_back({p[0], p[1], q[0], p[2] * q[1]});
            }
        return out;
    }();
    return rules[std::size_t(std::clamp(degree, 0, 5) * 4 + std::clamp(n, 0, 3))];
}

namespace {

/// A key of a plan degree of freedom at a level.
long long levelKey(int planDof, int level, int levels)
{
    return (long long)planDof * levels + level;
}

} // namespace

Space3::Space3(std::shared_ptr<const Mesh3> mesh, int order) : a_mesh(std::move(mesh)), a_order(order == 2 ? 2 : 1)
{
    const Mesh3& m = *a_mesh;
    const int H = int(m.heights.size());
    // (Its bands' plans - one: the mesh's.)
    const int NB = std::max(1, int(m.bands.size()));
    for (int b = 0; b < NB; ++b) a_plans.push_back(std::make_shared<Space>(m.bands.empty() ? m.plan : m.bands[std::size_t(b)].plan, a_order));
    a_plan = a_plans.front();
    a_prismAt.resize(std::size_t(NB));
    a_edgeTriangles.resize(std::size_t(NB));
    for (int b = 0; b < NB; ++b) {
        const Mesh& pm = planMesh(b);
        const int nt = int(pm.triangles.size());
        a_prismAt[std::size_t(b)].assign(std::size_t(nt) * std::size_t(std::max(H, 1)), -1);
        // The triangles beside each plan edge a wall may stand on.
        std::unordered_map<long long, int> edgeOf;
        auto key = [](int a, int c) { return (long long)std::min(a, c) * 2147483648LL + std::max(a, c); };
        for (int ei = 0; ei < int(pm.edges.size()); ++ei) edgeOf.emplace(key(pm.edges[std::size_t(ei)].a, pm.edges[std::size_t(ei)].b), ei);
        auto& beside = a_edgeTriangles[std::size_t(b)];
        beside.assign(pm.edges.size(), {{{-1, -1}, {-1, -1}}});
        for (int tr = 0; tr < nt; ++tr)
            for (int k = 0; k < 3; ++k) {
                const auto& v = pm.triangles[std::size_t(tr)];
                auto it = edgeOf.find(key(v[std::size_t(k)], v[std::size_t((k + 1) % 3)]));
                if (it == edgeOf.end()) continue;
                auto& two = beside[std::size_t(it->second)];
                (two[0].first < 0 ? two[0] : two[1]) = {tr, k};
            }
    }
    for (int e = 0; e < int(m.prisms.size()); ++e)
        a_prismAt[std::size_t(bandOf(e))][std::size_t(m.prismTriangle[std::size_t(e)]) * std::size_t(H) + std::size_t(m.prismLayer[std::size_t(e)])] = e;
    const int per = perElement();
    a_dofs.resize(m.prisms.size() * std::size_t(per));
    if (a_order == 1) {
        // The mesh's nodes, as they are (by band: two bands' same node at an
        // interface one).
        a_size = int(m.nodes.size());
        a_origin = m.nodeOrigin;
        a_dofBand = m.nodeBand.empty() ? std::vector<int>(m.nodes.size(), 0) : m.nodeBand;
        for (std::size_t e = 0; e < m.prisms.size(); ++e)
            for (int k = 0; k < 6; ++k) a_dofs[e * 6 + std::size_t(k)] = m.prisms[e][std::size_t(k)];
        return;
    }
    // Order 2: the plan's degrees of freedom at the heights (even levels) and
    // the layers' middles (odd), the nodes first as they are numbered; by
    // band, each band's own (at an interface, the nodes the two bands
    // share one; the rest each band's).
    const int levels = 2 * H - 1;
    a_levels = levels;
    a_keys.resize(std::size_t(NB));
    for (int i = 0; i < int(m.nodes.size()); ++i) {
        const auto [pn, j] = m.nodeOrigin[std::size_t(i)];
        const int b = m.nodeBand.empty() ? 0 : m.nodeBand[std::size_t(i)];
        a_keys[std::size_t(b)].emplace(levelKey(pn, 2 * j, levels), i);
        a_origin.emplace_back(pn, 2 * j);
        a_dofBand.push_back(b);
    }
    int next = int(m.nodes.size());
    for (std::size_t e = 0; e < m.prisms.size(); ++e) {
        const int b = bandOf(int(e)), tr = m.prismTriangle[e], j = m.prismLayer[e];
        const int* d2 = a_plans[std::size_t(b)]->dofs(tr);
        auto& keys = a_keys[std::size_t(b)];
        for (int lz = 0; lz < 3; ++lz)
            for (int a = 0; a < 6; ++a) {
                const int level = 2 * j + lz;
                const long long key = levelKey(d2[a], level, levels);
                auto it = keys.find(key);
                if (it == keys.end()) {
                    // (A corner at a height: the mesh's node there - at an
                    // interface perhaps the band below's.)
                    if (a < 3 && lz != 1) {
                        it = keys.emplace(key, m.prisms[e][std::size_t(a + (lz == 2 ? 3 : 0))]).first;
                    } else {
                        it = keys.emplace(key, next++).first;
                        a_origin.emplace_back(d2[a], level);
                        a_dofBand.push_back(b);
                    }
                }
                a_dofs[e * 18 + std::size_t(a + 6 * lz)] = it->second;
            }
    }
    a_size = next;
}

double Space3::levelZ(int level) const
{
    const Mesh3& m = *a_mesh;
    if (a_order == 1) return m.heights[std::size_t(level)] * m.unitScale;
    if (level % 2 == 0) return m.heights[std::size_t(level / 2)] * m.unitScale;
    return 0.5 * (m.heights[std::size_t(level / 2)] + m.heights[std::size_t(level / 2 + 1)]) * m.unitScale;
}

Vec3 Space3::point(int dof) const
{
    const auto [pd, level] = a_origin[std::size_t(dof)];
    const QPointF p = a_plans[std::size_t(a_dofBand[std::size_t(dof)])]->point(pd);
    return Vec3{p.x(), p.y(), levelZ(level)};
}

int Space3::bandOf(int e) const
{
    return a_mesh->prismBand.empty() ? 0 : a_mesh->prismBand[std::size_t(e)];
}

const Mesh& Space3::planMesh(int band) const
{
    return a_mesh->bands.empty() ? *a_mesh->plan : *a_mesh->bands[std::size_t(band)].plan;
}

int Space3::prismAt(int t, int j) const
{
    return prismAt(0, t, j);
}

int Space3::prismAt(int band, int t, int j) const
{
    const int H = int(a_mesh->heights.size());
    if (band < 0 || band >= int(a_prismAt.size())) return -1;
    const std::vector<int>& at = a_prismAt[std::size_t(band)];
    if (t < 0 || j < 0 || j >= H || std::size_t(t) * std::size_t(H) + std::size_t(j) >= at.size()) return -1;
    return at[std::size_t(t) * std::size_t(H) + std::size_t(j)];
}

Space3::Map Space3::map(int e, double xi, double eta, double zeta) const
{
    const Mesh3& m = *a_mesh;
    const int t = m.prismTriangle[std::size_t(e)], j = m.prismLayer[std::size_t(e)];
    const Space::Map p = a_plans[std::size_t(bandOf(e))]->map(t, xi, eta);
    const double z0 = m.heights[std::size_t(j)] * m.unitScale, z1 = m.heights[std::size_t(j) + 1] * m.unitScale;
    Map out;
    out.x = Vec3{p.x.x(), p.x.y(), z0 + zeta * (z1 - z0)};
    out.det = p.det * (z1 - z0);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 2; ++c) out.invT[r][c] = p.invT[r][c];
    out.invDz = 1 / (z1 - z0);
    return out;
}

int Space3::dofAt(int planDof, int level) const
{
    if (!a_mesh->byBand()) return dofAt(0, planDof, level);
    // (The band that holds the level's height - at an interface the lower.)
    const int height = a_order == 1 ? level : level / 2;
    for (int b = 0; b < int(a_mesh->bands.size()); ++b)
        if (height >= a_mesh->bands[std::size_t(b)].first && height <= a_mesh->bands[std::size_t(b)].last) return dofAt(b, planDof, level);
    return -1;
}

int Space3::dofAt(int band, int planDof, int level) const
{
    if (a_order == 1) return a_mesh->node(band, planDof, level);
    if (band < 0 || band >= int(a_keys.size())) return -1;
    auto it = a_keys[std::size_t(band)].find(levelKey(planDof, level, a_levels));
    return it == a_keys[std::size_t(band)].end() ? -1 : it->second;
}

std::vector<int> Space3::faceDofs(int f) const
{
    const Mesh3::Face& face = a_mesh->faces[std::size_t(f)];
    std::vector<int> out;
    const int step = a_order == 2 ? 2 : 1;   // levels a height
    const Space& plan = *a_plans[std::size_t(face.band)];
    if (!face.quad) {
        const int per = a_order == 2 ? 6 : 3;
        const int* d2 = plan.dofs(face.plan);
        for (int a = 0; a < per; ++a) out.push_back(dofAt(face.band, d2[a], step * face.height));
        return out;
    }
    const std::array<int, 3> d2 = plan.edgeDofs(face.plan);
    const int np = a_order == 2 ? 3 : 2, nl = a_order == 2 ? 3 : 2;
    for (int lz = 0; lz < nl; ++lz)
        for (int p = 0; p < np; ++p) out.push_back(dofAt(face.band, d2[std::size_t(p)], step * face.height + lz));
    return out;
}

std::vector<Space3::FacePoint> Space3::facePoints(int f, int degree, int domain) const
{
    const Mesh3& m = *a_mesh;
    const Mesh3::Face& face = m.faces[std::size_t(f)];
    std::vector<FacePoint> out;
    // (The prism beside it: one of the domain asked for where there is one.)
    auto pick = [&m, domain](int a, int b) {
        if (a >= 0 && (b < 0 || m.domain[std::size_t(a)] == domain)) return 0;
        return b >= 0 ? 1 : -1;
    };
    const Space& plan = *a_plans[std::size_t(face.band)];
    if (!face.quad) {
        const double z = m.heights[std::size_t(face.height)] * m.unitScale;
        const int above = prismAt(face.band, face.plan, face.height), below = prismAt(face.band, face.plan, face.height - 1);
        const int side = pick(above, below);
        for (const auto& q : triangleRule(degree).points) {
            const Space::Map p = plan.map(face.plan, q[0], q[1]);
            Shape s;
            shapeAt(a_order, q[0], q[1], s);
            FacePoint fp;
            for (int a = 0; a < s.count; ++a) fp.n[a] = s.n[a];
            fp.x = Vec3{p.x.x(), p.x.y(), z};
            fp.weight = q[2] * std::abs(p.det);
            fp.prism = side == 0 ? above : side == 1 ? below : -1;
            fp.xi = q[0];
            fp.eta = q[1];
            fp.zeta = side == 0 ? 0.0 : 1.0;
            out.push_back(fp);
        }
        return out;
    }
    // A wall: the triangles beside its edge, and where the edge runs in each.
    const auto& beside = a_edgeTriangles[std::size_t(face.band)][std::size_t(face.plan)];
    const int pa = beside[0].first >= 0 ? prismAt(face.band, beside[0].first, face.height) : -1;
    const int pb = beside[1].first >= 0 ? prismAt(face.band, beside[1].first, face.height) : -1;
    const int side = pick(pa, pb);
    const int tri = side == 0 ? beside[0].first : side == 1 ? beside[1].first : -1;
    const int k = side == 0 ? beside[0].second : side == 1 ? beside[1].second : 0;
    const Mesh& pm = planMesh(face.band);
    const bool forward = tri >= 0 && pm.triangles[std::size_t(tri)][std::size_t(k)] == pm.edges[std::size_t(face.plan)].a;
    static const double corner[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    const double z0 = m.heights[std::size_t(face.height)] * m.unitScale, z1 = m.heights[std::size_t(face.height) + 1] * m.unitScale;
    const int n = std::clamp(degree / 2 + 1, 2, 3);
    for (const auto& qs : lineRule(n))
        for (const auto& qz : lineRule(n)) {
            const double s = qs[0], zeta = qz[0];
            double jacobian = 0;
            const QPointF p = plan.edgePoint(face.plan, s, &jacobian);
            double ls[3], lz[3];
            int np = 2;
            if (a_order == 2) {
                np = 3;
                ls[0] = (1 - s) * (1 - 2 * s);
                ls[1] = s * (2 * s - 1);
                ls[2] = 4 * s * (1 - s);
                lz[0] = (1 - zeta) * (1 - 2 * zeta);
                lz[1] = 4 * zeta * (1 - zeta);
                lz[2] = zeta * (2 * zeta - 1);
            } else {
                ls[0] = 1 - s;
                ls[1] = s;
                lz[0] = 1 - zeta;
                lz[1] = zeta;
            }
            FacePoint fp;
            for (int l = 0; l < np; ++l)
                for (int a = 0; a < np; ++a) fp.n[a + np * l] = ls[a] * lz[l];
            fp.x = Vec3{p.x(), p.y(), z0 + zeta * (z1 - z0)};
            fp.weight = qs[1] * qz[1] * jacobian * (z1 - z0);
            if (tri >= 0) {
                const double u = forward ? s : 1 - s;
                const double* c0 = corner[k];
                const double* c1 = corner[(k + 1) % 3];
                fp.prism = side == 0 ? pa : pb;
                fp.xi = c0[0] + u * (c1[0] - c0[0]);
                fp.eta = c0[1] + u * (c1[1] - c0[1]);
                fp.zeta = zeta;
            }
            out.push_back(fp);
        }
    return out;
}

} // namespace qucs_s::fem
