/*
 * fem_fe.cpp - the finite elements of the multiphysics solver
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_fe.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace qucs_s::fem {

const TriangleRule& triangleRule(int degree)
{
    static const TriangleRule rules[6] = {
        // 0, 1: the centroid.
        {{{1.0 / 3, 1.0 / 3, 0.5}}},
        {{{1.0 / 3, 1.0 / 3, 0.5}}},
        // 2: three points.
        {{{1.0 / 6, 1.0 / 6, 1.0 / 6}, {2.0 / 3, 1.0 / 6, 1.0 / 6}, {1.0 / 6, 2.0 / 3, 1.0 / 6}}},
        // 3 and 4: Dunavant's six points (his rule of degree 3 has a weight
        // below zero).
        {{{0.445948490915965, 0.445948490915965, 0.223381589678011 / 2},
          {0.108103018168070, 0.445948490915965, 0.223381589678011 / 2},
          {0.445948490915965, 0.108103018168070, 0.223381589678011 / 2},
          {0.091576213509771, 0.091576213509771, 0.109951743655322 / 2},
          {0.816847572980459, 0.091576213509771, 0.109951743655322 / 2},
          {0.091576213509771, 0.816847572980459, 0.109951743655322 / 2}}},
        {{{0.445948490915965, 0.445948490915965, 0.223381589678011 / 2},
          {0.108103018168070, 0.445948490915965, 0.223381589678011 / 2},
          {0.445948490915965, 0.108103018168070, 0.223381589678011 / 2},
          {0.091576213509771, 0.091576213509771, 0.109951743655322 / 2},
          {0.816847572980459, 0.091576213509771, 0.109951743655322 / 2},
          {0.091576213509771, 0.816847572980459, 0.109951743655322 / 2}}},
        // 5: seven points.
        {{{1.0 / 3, 1.0 / 3, 0.225 / 2},
          {0.470142064105115, 0.470142064105115, 0.132394152788506 / 2},
          {0.059715871789770, 0.470142064105115, 0.132394152788506 / 2},
          {0.470142064105115, 0.059715871789770, 0.132394152788506 / 2},
          {0.101286507323456, 0.101286507323456, 0.125939180544827 / 2},
          {0.797426985353087, 0.101286507323456, 0.125939180544827 / 2},
          {0.101286507323456, 0.797426985353087, 0.125939180544827 / 2}}},
    };
    return rules[std::clamp(degree, 0, 5)];
}

const std::vector<std::array<double, 2>>& lineRule(int n)
{
    static const std::vector<std::array<double, 2>> rules[4] = {
        {{0.5, 1.0}},
        {{0.5, 1.0}},
        {{0.5 - 0.5 / std::sqrt(3.0), 0.5}, {0.5 + 0.5 / std::sqrt(3.0), 0.5}},
        {{0.5 - 0.5 * std::sqrt(0.6), 5.0 / 18}, {0.5, 8.0 / 18}, {0.5 + 0.5 * std::sqrt(0.6), 5.0 / 18}},
    };
    return rules[std::clamp(n, 0, 3)];
}

void shapeAt(int order, double xi, double eta, Shape& s)
{
    const double l1 = 1 - xi - eta, l2 = xi, l3 = eta;
    if (order == 1) {
        s.count = 3;
        s.n[0] = l1;
        s.n[1] = l2;
        s.n[2] = l3;
        s.dxi[0] = -1;
        s.deta[0] = -1;
        s.dxi[1] = 1;
        s.deta[1] = 0;
        s.dxi[2] = 0;
        s.deta[2] = 1;
        return;
    }
    s.count = 6;
    s.n[0] = l1 * (2 * l1 - 1);
    s.n[1] = l2 * (2 * l2 - 1);
    s.n[2] = l3 * (2 * l3 - 1);
    s.n[3] = 4 * l1 * l2;
    s.n[4] = 4 * l2 * l3;
    s.n[5] = 4 * l3 * l1;
    s.dxi[0] = -(4 * l1 - 1);
    s.deta[0] = -(4 * l1 - 1);
    s.dxi[1] = 4 * l2 - 1;
    s.deta[1] = 0;
    s.dxi[2] = 0;
    s.deta[2] = 4 * l3 - 1;
    s.dxi[3] = 4 * (l1 - l2);
    s.deta[3] = -4 * l2;
    s.dxi[4] = 4 * l3;
    s.deta[4] = 4 * l2;
    s.dxi[5] = -4 * l3;
    s.deta[5] = 4 * (l1 - l3);
}

namespace {

unsigned long long edgeKey(int a, int b)
{
    const unsigned long long lo = static_cast<unsigned long long>(std::min(a, b));
    const unsigned long long hi = static_cast<unsigned long long>(std::max(a, b));
    return (hi << 32) | lo;
}

} // namespace

int Space::localEdge(const std::array<int, 3>& tri, int a, int b)
{
    for (int k = 0; k < 3; ++k) {
        const int p = tri[std::size_t(k)], q = tri[std::size_t((k + 1) % 3)];
        if ((p == a && q == b) || (p == b && q == a)) return k;
    }
    return -1;
}

Space::Space(std::shared_ptr<const Mesh> mesh, int order) : a_mesh(std::move(mesh)), a_order(order == 2 ? 2 : 1)
{
    const Mesh& m = *a_mesh;
    const double s = m.unitScale;
    const int nn = int(m.nodes.size());
    const int nt = int(m.triangles.size());
    a_points.reserve(std::size_t(nn));
    for (QPointF p : m.nodes) a_points.push_back(p * s);
    a_curved.assign(std::size_t(nt), 0);
    a_nodes.resize(std::size_t(nt));
    a_affine.resize(std::size_t(nt));
    const int per = perElement();
    a_dofs.resize(std::size_t(nt) * std::size_t(per));
    std::unordered_map<unsigned long long, int> edges;
    // The middles on arcs: the curved boundary edges'.
    std::unordered_map<unsigned long long, QPointF> curvedMid;
    if (a_order == 2 && m.topology)
        for (const Mesh::Edge& e : m.edges) {
            if (e.curve < 0) continue;
            const Curve& c = m.topology->curves[std::size_t(e.curve)];
            if (c.kind != Curve::Arc) continue;
            const QPointF mid = c.at((e.ta + e.tb) / 2) * s;
            const QPointF chord = (a_points[std::size_t(e.a)] + a_points[std::size_t(e.b)]) / 2;
            const double len = std::hypot(a_points[std::size_t(e.b)].x() - a_points[std::size_t(e.a)].x(),
                                          a_points[std::size_t(e.b)].y() - a_points[std::size_t(e.a)].y());
            if (std::hypot(mid.x() - chord.x(), mid.y() - chord.y()) > 1e-10 * len) curvedMid[edgeKey(e.a, e.b)] = mid;
        }
    int next = nn;
    for (int t = 0; t < nt; ++t) {
        const auto& v = m.triangles[std::size_t(t)];
        int* d = &a_dofs[std::size_t(t) * std::size_t(per)];
        std::array<QPointF, 6>& g = a_nodes[std::size_t(t)];
        for (int k = 0; k < 3; ++k) {
            d[k] = v[std::size_t(k)];
            g[std::size_t(k)] = a_points[std::size_t(v[std::size_t(k)])];
        }
        if (a_order == 2) {
            for (int k = 0; k < 3; ++k) {
                const int a = v[std::size_t(k)], b = v[std::size_t((k + 1) % 3)];
                const unsigned long long key = edgeKey(a, b);
                auto it = edges.find(key);
                QPointF mid = (a_points[std::size_t(a)] + a_points[std::size_t(b)]) / 2;
                auto cm = curvedMid.find(key);
                if (cm != curvedMid.end()) {
                    mid = cm->second;
                    a_curved[std::size_t(t)] = 1;
                }
                if (it == edges.end()) {
                    it = edges.emplace(key, next++).first;
                    a_points.push_back(mid);
                }
                d[3 + k] = it->second;
                g[std::size_t(3 + k)] = mid;
            }
        }
        // The affine map from the corners.
        const QPointF p0 = g[0], p1 = g[1], p2 = g[2];
        const double xx = p1.x() - p0.x(), xe = p2.x() - p0.x(), yx = p1.y() - p0.y(), ye = p2.y() - p0.y();
        const double det = xx * ye - xe * yx;
        Affine& af = a_affine[std::size_t(t)];
        af.det = det;
        af.invT[0][0] = ye / det;
        af.invT[0][1] = -yx / det;
        af.invT[1][0] = -xe / det;
        af.invT[1][1] = xx / det;
    }
    a_size = next;
    a_edgeDofs.resize(m.edges.size());
    a_edgeMid.resize(m.edges.size());
    for (std::size_t e = 0; e < m.edges.size(); ++e) {
        const Mesh::Edge& me = m.edges[e];
        a_edgeDofs[e] = {me.a, me.b, -1};
        a_edgeMid[e] = (a_points[std::size_t(me.a)] + a_points[std::size_t(me.b)]) / 2;
        if (a_order == 2) {
            auto it = edges.find(edgeKey(me.a, me.b));
            if (it != edges.end()) {
                a_edgeDofs[e][2] = it->second;
                a_edgeMid[e] = a_points[std::size_t(it->second)];
            }
        }
    }
}

Space::Map Space::map(int t, double xi, double eta) const
{
    Map m;
    const std::array<QPointF, 6>& g = a_nodes[std::size_t(t)];
    if (!a_curved[std::size_t(t)]) {
        const Affine& af = a_affine[std::size_t(t)];
        m.x = g[0] + (g[1] - g[0]) * xi + (g[2] - g[0]) * eta;
        m.det = af.det;
        m.invT[0][0] = af.invT[0][0];
        m.invT[0][1] = af.invT[0][1];
        m.invT[1][0] = af.invT[1][0];
        m.invT[1][1] = af.invT[1][1];
        return m;
    }
    Shape s;
    shapeAt(2, xi, eta, s);
    double x = 0, y = 0, xx = 0, xe = 0, yx = 0, ye = 0;
    for (int k = 0; k < 6; ++k) {
        x += s.n[k] * g[std::size_t(k)].x();
        y += s.n[k] * g[std::size_t(k)].y();
        xx += s.dxi[k] * g[std::size_t(k)].x();
        xe += s.deta[k] * g[std::size_t(k)].x();
        yx += s.dxi[k] * g[std::size_t(k)].y();
        ye += s.deta[k] * g[std::size_t(k)].y();
    }
    const double det = xx * ye - xe * yx;
    m.x = QPointF(x, y);
    m.det = det;
    m.invT[0][0] = ye / det;
    m.invT[0][1] = -yx / det;
    m.invT[1][0] = -xe / det;
    m.invT[1][1] = xx / det;
    return m;
}

std::array<int, 3> Space::edgeDofs(int e) const
{
    return a_edgeDofs[std::size_t(e)];
}

QPointF Space::edgePoint(int e, double s, double* jacobian) const
{
    const std::array<int, 3>& d = a_edgeDofs[std::size_t(e)];
    const QPointF a = a_points[std::size_t(d[0])], b = a_points[std::size_t(d[1])];
    const QPointF mid = a_edgeMid[std::size_t(e)];
    if (a_order == 1 || std::hypot(mid.x() - (a.x() + b.x()) / 2, mid.y() - (a.y() + b.y()) / 2) == 0) {
        if (jacobian) *jacobian = std::hypot(b.x() - a.x(), b.y() - a.y());
        return a + (b - a) * s;
    }
    const double la = (1 - s) * (1 - 2 * s), lm = 4 * s * (1 - s), lb = s * (2 * s - 1);
    const double da = -3 + 4 * s, dm = 4 - 8 * s, db = -1 + 4 * s;
    if (jacobian) {
        const QPointF d1 = a * da + mid * dm + b * db;
        *jacobian = std::hypot(d1.x(), d1.y());
    }
    return a * la + mid * lm + b * lb;
}

} // namespace qucs_s::fem
