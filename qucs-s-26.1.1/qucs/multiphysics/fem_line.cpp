/*
 * fem_line.cpp - a line across a model's domains for a physics solved in 1D
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_line.h"
#include "fem_problem.h"

#include <algorithm>
#include <cmath>

namespace qucs_s::fem::detail {

double Line::along(QPointF p) const
{
    const QPointF d = p - S;
    return L > 0 ? (d.x() * (E.x() - S.x()) + d.y() * (E.y() - S.y())) / L : 0;
}

int Line::element(double s, double* xi) const
{
    const int ne = elements();
    if (ne == 0) return -1;
    const std::ptrdiff_t at = std::upper_bound(z.begin(), z.end(), s) - z.begin();
    int e = int(std::max<std::ptrdiff_t>(0, at - 1) / 2);
    e = std::clamp(e, 0, ne - 1);
    const double z0 = z[std::size_t(2 * e)], z2 = z[std::size_t(2 * e + 2)];
    *xi = z2 > z0 ? std::clamp((s - z0) / (z2 - z0), 0.0, 1.0) : 0;
    return e;
}

void lineShape(double xi, double N[3], double dN[3])
{
    N[0] = (1 - xi) * (1 - 2 * xi);
    N[1] = 4 * xi * (1 - xi);
    N[2] = xi * (2 * xi - 1);
    dN[0] = 4 * xi - 3;
    dN[1] = 4 - 8 * xi;
    dN[2] = 4 * xi - 1;
}

bool buildLine(const Solution& s, const std::vector<char>& on, int axis, const QString& at, double size, int count, Line* line,
               QString* error)
{
    const Topology& topo = s.topology();
    const Mesh& mesh = s.mesh();
    const double unit = mesh.unitScale;
    QRectF box;
    for (std::size_t d = 0; d < topo.domains.size() && d < on.size(); ++d)
        if (on[d]) box = box.isNull() ? topo.domains[d].bounds : box.united(topo.domains[d].bounds);
    if (box.isNull()) {
        *error = tr("it is on no domain");
        return false;
    }
    double across = (axis == 1 ? box.center().x() : box.center().y()) * unit;
    if (!at.trimmed().isEmpty()) {
        Expression::Options inUnit;
        inUnit.lengthUnit = unit;
        inUnit.numbersAreLengths = true;
        const Expression e = Expression::compile(at, s.parameters().scope, inUnit);
        if (!e.isValid() || !e.isConstant()) {
            *error = tr("where its line runs: %1").arg(e.isValid() ? tr("it must be a constant") : e.error());
            return false;
        }
        // (Not finite: said so - "1/0" was "its domains have no extent
        // along the line", the hunt of 22 October, N10.)
        if (!std::isfinite(e.constant())) {
            *error = tr("where its line runs: it is %1, not a finite number").arg(formatNumber(e.constant()));
            return false;
        }
        across = e.constant() * unit;
    }
    Line& l = *line;
    l = Line();
    if (axis == 1) {
        l.S = QPointF(across, box.top() * unit);
        l.E = QPointF(across, box.bottom() * unit);
    } else {
        l.S = QPointF(box.left() * unit, across);
        l.E = QPointF(box.right() * unit, across);
    }
    l.L = std::hypot(l.E.x() - l.S.x(), l.E.y() - l.S.y());
    if (!(l.L > 0)) {
        *error = tr("its domains have no extent along the line");
        return false;
    }
    const QPointF u = (l.E - l.S) / l.L;
    // Each triangle's stretch of the line (its corners counter-clockwise).
    struct Piece {
        double lo, hi;
        int d;
    };
    std::vector<Piece> pieces;
    std::vector<double> cuts = {0, l.L};
    const double eps = 1e-9 * l.L;
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
        QPointF p[3];
        for (int k = 0; k < 3; ++k) p[k] = mesh.nodes[std::size_t(mesh.triangles[t][std::size_t(k)])] * unit;
        double lo = 0, hi = l.L;
        bool empty = false;
        for (int k = 0; k < 3 && !empty; ++k) {
            const QPointF a = p[k], e = p[(k + 1) % 3] - p[k];
            const double alpha = e.x() * (l.S.y() - a.y()) - e.y() * (l.S.x() - a.x());
            const double beta = e.x() * u.y() - e.y() * u.x();
            const double scale = std::hypot(e.x(), e.y());
            if (std::abs(beta) < 1e-12 * scale) {
                if (alpha < -1e-12 * scale * l.L) empty = true;
            } else if (beta > 0) lo = std::max(lo, -alpha / beta);
            else hi = std::min(hi, -alpha / beta);
        }
        if (empty || hi - lo <= eps) continue;
        pieces.push_back({lo, hi, mesh.domain[t]});
        cuts.push_back(lo);
        cuts.push_back(hi);
    }
    std::sort(cuts.begin(), cuts.end());
    std::vector<double> breaks;
    for (double c : cuts)
        if (breaks.empty() || c - breaks.back() > eps) breaks.push_back(c);
    // The layers: each stretch between two cuts in the domain at its middle.
    struct Layer {
        double s0, s1;
        int d;
    };
    std::vector<Layer> layers;
    for (std::size_t k = 0; k + 1 < breaks.size(); ++k) {
        const double m = (breaks[k] + breaks[k + 1]) / 2;
        int d = -1;
        for (const Piece& p : pieces)
            if (m > p.lo && m < p.hi) {
                d = p.d;
                break;
            }
        if (d >= 0 && (std::size_t(d) >= on.size() || !on[std::size_t(d)])) d = -1;
        if (!layers.empty() && layers.back().d == d) layers.back().s1 = breaks[k + 1];
        else layers.push_back({breaks[k], breaks[k + 1], d});
    }
    double inside = 0;
    for (const Layer& ly : layers)
        if (ly.d >= 0) inside += ly.s1 - ly.s0;
    if (!(inside > 0)) {
        *error = tr("its line at %1 crosses none of its domains").arg(formatQuantity(across, Dim::length()));
        return false;
    }
    const double h = size > 0 ? size : inside / std::max(1, count);
    long total = 0;
    for (const Layer& ly : layers) total += ly.d >= 0 ? long(std::ceil((ly.s1 - ly.s0) / h - 1e-9)) : 1;
    if (total > 400000) {
        *error = tr("%1 elements along its line: a larger element size").arg(total);
        return false;
    }
    // Quadratic elements: as many as the size asks in each layer, one across a gap.
    l.z.assign(1, 0.0);
    std::vector<char> ends(1, 1);
    for (const Layer& ly : layers) {
        const int n = ly.d >= 0 ? std::max(1, int(std::ceil((ly.s1 - ly.s0) / h - 1e-9))) : 1;
        for (int k = 0; k < n; ++k) {
            const double z0 = ly.s0 + (ly.s1 - ly.s0) * k / n, z1 = ly.s0 + (ly.s1 - ly.s0) * (k + 1) / n;
            l.z.push_back((z0 + z1) / 2);
            l.z.push_back(z1);
            l.domain.push_back(ly.d);
            ends.push_back(0);
            ends.push_back(k + 1 == n ? 1 : 0);
        }
    }
    // The boundaries through each layer's ends: the mesh's boundary edges there.
    l.boundaries.assign(l.z.size(), {});
    const double tol = 1e-7 * l.L;
    for (std::size_t i = 0; i < l.z.size(); ++i) {
        if (!ends[i]) continue;
        const QPointF p = l.point(l.z[i]);
        for (const Mesh::Edge& me : mesh.edges) {
            if (me.boundary < 0) continue;
            const QPointF a = mesh.nodes[std::size_t(me.a)] * unit, b = mesh.nodes[std::size_t(me.b)] * unit;
            const QPointF ab = b - a, ap = p - a;
            const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
            if (!(len2 > 0)) continue;
            const double t = std::clamp((ap.x() * ab.x() + ap.y() * ab.y()) / len2, 0.0, 1.0);
            const QPointF c = a + ab * t;
            if (std::hypot(p.x() - c.x(), p.y() - c.y()) <= tol) l.boundaries[i].insert(me.boundary);
        }
    }
    return true;
}

} // namespace qucs_s::fem::detail
