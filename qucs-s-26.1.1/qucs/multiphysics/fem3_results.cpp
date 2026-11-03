/*
 * fem3_results.cpp - a 3D solution's results: nodal values and slices
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_results.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qucs_s::fem {

namespace {

const double NaN = std::numeric_limits<double>::quiet_NaN();

/// The field whose own variable \a e is alone (ht.T, T), or -1.
int ownVariable(const Solution3& s, const Expression& e)
{
    if (e.variables().size() != 1) return -1;
    const int slot = e.variables().front();
    const Solution3::VariableDef& d = s.definitions()[std::size_t(slot)];
    if (d.kind != Solution3::VariableDef::Value) return -1;
    return e.text().trimmed() == d.info.name ? d.field : -1;
}

} // namespace

std::vector<double> nodalValues(const Solution3& s, const Expression& e)
{
    const Mesh3& m = s.mesh();
    std::vector<double> out(m.nodes.size(), NaN);
    if (!e.isValid()) return out;
    // (A field's own variable: its values at the nodes, the first of its
    // degrees of freedom, where it is solved.)
    if (const int f = ownVariable(s, e); f >= 0) {
        const Field3& field = s.fields()[std::size_t(f)];
        for (std::size_t p = 0; p < m.prisms.size(); ++p) {
            const int d = m.domain[p];
            if (d < 0 || d >= int(field.domains.size()) || !field.domains[std::size_t(d)]) continue;
            for (int n : m.prisms[p])
                if (std::size_t(n) < field.values.size()) out[std::size_t(n)] = field.values[std::size_t(n)];
        }
        return out;
    }
    std::vector<double> sum(m.nodes.size(), 0);
    std::vector<int> count(m.nodes.size(), 0);
    PointEvaluator3 ev(s);
    static const double corner[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
    for (int p = 0; p < int(m.prisms.size()); ++p)
        for (int k = 0; k < 6; ++k) {
            ev.at(p, corner[k][0], corner[k][1], corner[k][2]);
            const double v = ev.eval(e);
            if (!std::isfinite(v)) continue;
            const int n = m.prisms[std::size_t(p)][std::size_t(k)];
            sum[std::size_t(n)] += v;
            ++count[std::size_t(n)];
        }
    for (std::size_t n = 0; n < out.size(); ++n)
        if (count[n] > 0) out[n] = sum[n] / count[n];
    return out;
}

std::vector<SliceTriangle> slice(const Solution3& s, const Expression& e, int axis, double at)
{
    std::vector<SliceTriangle> out;
    const Mesh3& m = s.mesh();
    if (!e.isValid() || !m.plan || m.heights.size() < 2 || axis < 0 || axis > 2) return out;
    PointEvaluator3 ev(s);
    auto value = [&ev, &e](int p, double xi, double eta, double zeta) {
        ev.at(p, xi, eta, zeta);
        return ev.eval(e);
    };
    const std::vector<double>& h = m.heights;
    if (axis == 2) {
        // The layer the plane is in (on a height: the one above it, or the
        // top one's).
        if (at < h.front() || at > h.back()) return out;
        int j = int(std::upper_bound(h.begin(), h.end(), at) - h.begin()) - 1;
        j = std::clamp(j, 0, int(h.size()) - 2);
        const double zeta = (at - h[std::size_t(j)]) / (h[std::size_t(j) + 1] - h[std::size_t(j)]);
        for (int p = 0; p < int(m.prisms.size()); ++p) {
            if (m.prismLayer[std::size_t(p)] != j) continue;
            const std::array<int, 3>& tri = m.plan->triangles[std::size_t(m.prismTriangle[std::size_t(p)])];
            SliceTriangle t;
            t.domain = m.domain[std::size_t(p)];
            const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
            for (int k = 0; k < 3; ++k) {
                const QPointF q = m.plan->nodes[std::size_t(tri[std::size_t(k)])];
                t.p[std::size_t(k)] = Vec3{q.x(), q.y(), at};
                t.value[std::size_t(k)] = value(p, ref[k][0], ref[k][1], zeta);
            }
            out.push_back(t);
        }
        return out;
    }
    // Across x or y: each prism's triangle cut by the line, the cut swept
    // between its heights.
    for (int p = 0; p < int(m.prisms.size()); ++p) {
        const std::array<int, 3>& tri = m.plan->triangles[std::size_t(m.prismTriangle[std::size_t(p)])];
        double c[3];
        QPointF v[3];
        for (int k = 0; k < 3; ++k) {
            v[k] = m.plan->nodes[std::size_t(tri[std::size_t(k)])];
            c[k] = axis == 0 ? v[k].x() : v[k].y();
        }
        if (at < std::min({c[0], c[1], c[2]}) || at > std::max({c[0], c[1], c[2]})) continue;
        // Where the line crosses its edges: their points, by reference
        // coordinates, once each.
        struct Cut {
            QPointF x;
            double xi, eta;
        };
        std::vector<Cut> cuts;
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        const double size = std::max(std::abs(c[0] - c[1]), std::max(std::abs(c[1] - c[2]), std::abs(c[2] - c[0])));
        for (int k = 0; k < 3; ++k) {
            const int a = k, b = (k + 1) % 3;
            if ((c[a] - at) * (c[b] - at) > 0 || c[a] == c[b]) continue;
            const double u = std::clamp((at - c[a]) / (c[b] - c[a]), 0.0, 1.0);
            Cut cut{v[a] + (v[b] - v[a]) * u, ref[a][0] + u * (ref[b][0] - ref[a][0]), ref[a][1] + u * (ref[b][1] - ref[a][1])};
            bool seen = false;
            for (const Cut& o : cuts) seen = seen || std::hypot(o.x.x() - cut.x.x(), o.x.y() - cut.x.y()) <= 1e-12 * std::max(size, 1e-300);
            if (!seen) cuts.push_back(cut);
        }
        if (cuts.size() != 2) continue;
        const int j = m.prismLayer[std::size_t(p)];
        const double z0 = h[std::size_t(j)], z1 = h[std::size_t(j) + 1];
        const Cut& A = cuts[0];
        const Cut& B = cuts[1];
        const Vec3 a0{A.x.x(), A.x.y(), z0}, b0{B.x.x(), B.x.y(), z0}, b1{B.x.x(), B.x.y(), z1}, a1{A.x.x(), A.x.y(), z1};
        const double va0 = value(p, A.xi, A.eta, 0), vb0 = value(p, B.xi, B.eta, 0), vb1 = value(p, B.xi, B.eta, 1), va1 = value(p, A.xi, A.eta, 1);
        const int d = m.domain[std::size_t(p)];
        out.push_back({{a0, b0, b1}, {va0, vb0, vb1}, d});
        out.push_back({{a0, b1, a1}, {va0, vb1, va1}, d});
    }
    return out;
}

} // namespace qucs_s::fem
