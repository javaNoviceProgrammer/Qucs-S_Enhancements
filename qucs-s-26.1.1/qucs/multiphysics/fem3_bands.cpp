/*
 * fem3_bands.cpp - a 3D mesh by band of layers: its interfaces joined
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_bands.h"

#include "fem_edge.h"
#include "fem_fe.h"

#include <Eigen/Dense>

#include <QCoreApplication>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

namespace qucs_s::fem {

namespace {

double cross2(QPointF a, QPointF b)
{
    return a.x() * b.y() - a.y() * b.x();
}

/// Twice a triangle's signed area.
double twiceArea(const Corners& c)
{
    return cross2(c[1] - c[0], c[2] - c[0]);
}

/// \a p's (ξ, η) in triangle \a c: p = c0 + ξ (c1 - c0) + η (c2 - c0).
void barycentric(const Corners& c, QPointF p, double* xi, double* eta)
{
    const double d = twiceArea(c);
    *xi = cross2(p - c[0], c[2] - c[0]) / d;
    *eta = cross2(c[1] - c[0], p - c[0]) / d;
}

double polygonArea(const std::vector<QPointF>& p)
{
    double a = 0;
    for (std::size_t i = 0; i < p.size(); ++i) a += cross2(p[i], p[(i + 1) % p.size()]);
    return a / 2;
}

/// Gauss-Legendre's 4 points on [0, 1] and their weights (exact to degree 7).
const std::array<std::array<double, 2>, 4>& gauss4()
{
    static const std::array<std::array<double, 2>, 4> g = [] {
        const double a = std::sqrt(3.0 / 7 - 2.0 / 7 * std::sqrt(6.0 / 5)), b = std::sqrt(3.0 / 7 + 2.0 / 7 * std::sqrt(6.0 / 5));
        const double wa = (18 + std::sqrt(30.0)) / 36, wb = (18 - std::sqrt(30.0)) / 36;
        return std::array<std::array<double, 2>, 4>{{{0.5 - b / 2, wb / 2}, {0.5 - a / 2, wa / 2}, {0.5 + a / 2, wa / 2}, {0.5 + b / 2, wb / 2}}};
    }();
    return g;
}

/// A combination of degrees of freedom: dof -> weight.
using Combination = std::map<int, double>;

void addTo(Combination& to, const Combination& from, double scale)
{
    for (const auto& [d, w] : from) to[d] += scale * w;
}

/// The layer of band \a band beside its height \a height (its bottom's or
/// top's).
int layerBeside(const Mesh3& m, int band, int height)
{
    return height == m.bands[std::size_t(band)].first ? height : height - 1;
}

/// The plan triangles of band \a band with a prism in layer \a layer.
std::vector<int> trianglesIn(const Mesh3& m, int band, int layer)
{
    std::vector<int> out;
    for (std::size_t e = 0; e < m.prisms.size(); ++e)
        if (m.prismBand[e] == band && m.prismLayer[e] == layer) out.push_back(m.prismTriangle[e]);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Corners cornersOf(const AffineMap& am)
{
    return {am.p0, am.p1, am.p2};
}

double sizeOf(const Corners& c)
{
    return std::sqrt(std::abs(twiceArea(c)) / 2);
}

/// A reference triangle's corners.
constexpr double kRef[3][2] = {{0, 0}, {1, 0}, {0, 1}};

/// An edge space's moments on a triangle, in order: each edge l (corner l
/// to l + 1) against 1 (and order 2, 2s - 1), then (order 2) the triangle's
/// two components. A field's at (ξ, η) given by \a at (the triangle's own
/// functions or another's).
template <typename Value, typename At>
std::vector<Value> edgeMoments(int order, const Corners& c, double det, At at, Value zero)
{
    std::vector<Value> out;
    for (int l = 0; l < 3; ++l) {
        const QPointF d = c[std::size_t((l + 1) % 3)] - c[std::size_t(l)];
        Value m0 = zero, m1 = zero;
        for (const auto& [s, w] : gauss4()) {
            const double xi = kRef[l][0] + s * (kRef[(l + 1) % 3][0] - kRef[l][0]);
            const double eta = kRef[l][1] + s * (kRef[(l + 1) % 3][1] - kRef[l][1]);
            const auto [vx, vy] = at(xi, eta);
            m0 += w * (vx * d.x() + vy * d.y());
            m1 += w * (2 * s - 1) * (vx * d.x() + vy * d.y());
        }
        out.push_back(m0);
        if (order == 2) out.push_back(m1);
    }
    if (order == 2) {
        Value fx = zero, fy = zero;
        for (const auto& q : triangleRule(4).points) {
            const auto [vx, vy] = at(q[0], q[1]);
            fx += q[2] * std::abs(det) * vx;
            fy += q[2] * std::abs(det) * vy;
        }
        out.push_back(fx);
        out.push_back(fy);
    }
    return out;
}

/// Triangle \a t's moment matrix: [moment i][local function k] of its
/// own edge functions - its coefficients from moments by its inverse.
Eigen::MatrixXd momentMatrix(const EdgeSpace& edges, int t, const AffineMap& am)
{
    const int n = edges.perElement();
    Eigen::MatrixXd M(n, n);
    for (int k = 0; k < n; ++k) {
        const std::vector<double> col = edgeMoments<double>(
            edges.order(), cornersOf(am), am.det,
            [&](double xi, double eta) {
                EdgeSpace::Basis b;
                edges.basis(t, am, xi, eta, b);
                return std::pair<double, double>(b.vx[k], b.vy[k]);
            },
            0.0);
        for (int i = 0; i < n; ++i) M(i, k) = col[std::size_t(i)];
    }
    return M;
}

/// A point's (ξ, η) in a (perhaps curved) plan triangle of a nodal space:
/// the straight one's, then Newton's steps on the map.
void locateIn(const Space& space, int t, const Corners& c, QPointF p, double* xi, double* eta)
{
    barycentric(c, p, xi, eta);
    for (int it = 0; it < 4; ++it) {
        const Space::Map m = space.map(t, *xi, *eta);
        const QPointF r = p - m.x;
        if (std::hypot(r.x(), r.y()) < 1e-14 * sizeOf(c)) break;
        *xi += m.invT[0][0] * r.x() + m.invT[1][0] * r.y();
        *eta += m.invT[0][1] * r.x() + m.invT[1][1] * r.y();
    }
}

/// The two sides of an interface: its height, the constrained (finer) band
/// and the other.
struct Interface {
    int height = 0, fine = 0, coarse = 0;
};

std::vector<Interface> interfacesOf(const Mesh3& m)
{
    std::vector<Interface> out;
    for (std::size_t i = 0; i + 1 < m.bands.size(); ++i) {
        Interface f;
        f.height = m.bands[i].last;
        f.fine = i < m.interfaceFiner.size() ? m.interfaceFiner[i] : int(i);
        f.coarse = f.fine == int(i) ? int(i) + 1 : int(i);
        out.push_back(f);
    }
    return out;
}

void prune(std::vector<std::pair<int, double>>& row)
{
    double largest = 0;
    for (const auto& [d, w] : row) largest = std::max(largest, std::abs(w));
    row.erase(std::remove_if(row.begin(), row.end(), [largest](const std::pair<int, double>& x) { return std::abs(x.second) <= 1e-13 * largest; }),
              row.end());
}

Constraints3 finish(std::map<int, Combination>& rows)
{
    Constraints3 out;
    for (auto& [d, c] : rows) {
        out.dofs.push_back(d);
        std::vector<std::pair<int, double>> row(c.begin(), c.end());
        prune(row);
        out.rows.push_back(std::move(row));
    }
    return out;
}

} // namespace

// ------------------------------------------------------------- the plans

PlanLocator::PlanLocator(const Mesh& plan, double scale)
{
    // (Its box by its corners' least and greatest: a QRectF of one point is
    // null, and united() leaves a null one out.)
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const std::array<int, 3>& tri : plan.triangles) {
        Corners c;
        for (int k = 0; k < 3; ++k) c[std::size_t(k)] = plan.nodes[std::size_t(tri[std::size_t(k)])] * scale;
        a_corners.push_back(c);
        for (const QPointF& q : c) {
            x0 = std::min(x0, q.x());
            y0 = std::min(y0, q.y());
            x1 = std::max(x1, q.x());
            y1 = std::max(y1, q.y());
        }
    }
    a_box = a_corners.empty() ? QRectF(0, 0, 1, 1) : QRectF(QPointF(x0, y0), QPointF(x1, y1));
    const int n = std::clamp(int(std::sqrt(double(a_corners.size()))), 1, 1024);
    a_nx = a_ny = n;
    a_cw = std::max(a_box.width() / n, 1e-300);
    a_ch = std::max(a_box.height() / n, 1e-300);
    a_cells.resize(std::size_t(n) * std::size_t(n));
    for (int t = 0; t < int(a_corners.size()); ++t) {
        const Corners& c = a_corners[std::size_t(t)];
        const double x0 = std::min({c[0].x(), c[1].x(), c[2].x()}), x1 = std::max({c[0].x(), c[1].x(), c[2].x()});
        const double y0 = std::min({c[0].y(), c[1].y(), c[2].y()}), y1 = std::max({c[0].y(), c[1].y(), c[2].y()});
        const int i0 = std::clamp(int((x0 - a_box.left()) / a_cw), 0, a_nx - 1), i1 = std::clamp(int((x1 - a_box.left()) / a_cw), 0, a_nx - 1);
        const int j0 = std::clamp(int((y0 - a_box.top()) / a_ch), 0, a_ny - 1), j1 = std::clamp(int((y1 - a_box.top()) / a_ch), 0, a_ny - 1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) a_cells[std::size_t(j) * std::size_t(a_nx) + std::size_t(i)].push_back(t);
    }
}

std::vector<int> PlanLocator::near(QPointF a, QPointF b) const
{
    const double x0 = std::min(a.x(), b.x()), x1 = std::max(a.x(), b.x()), y0 = std::min(a.y(), b.y()), y1 = std::max(a.y(), b.y());
    const int i0 = std::clamp(int((x0 - a_box.left()) / a_cw) - 1, 0, a_nx - 1), i1 = std::clamp(int((x1 - a_box.left()) / a_cw) + 1, 0, a_nx - 1);
    const int j0 = std::clamp(int((y0 - a_box.top()) / a_ch) - 1, 0, a_ny - 1), j1 = std::clamp(int((y1 - a_box.top()) / a_ch) + 1, 0, a_ny - 1);
    std::vector<int> out;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            for (int t : a_cells[std::size_t(j) * std::size_t(a_nx) + std::size_t(i)]) out.push_back(t);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int PlanLocator::locate(QPointF p, double* xi, double* eta) const
{
    int best = -1;
    double bestMargin = -1e300;
    for (int t : near(p, p)) {
        double a = 0, b = 0;
        barycentric(a_corners[std::size_t(t)], p, &a, &b);
        const double margin = std::min({a, b, 1 - a - b});
        if (margin > bestMargin) {
            bestMargin = margin;
            best = t;
            *xi = a;
            *eta = b;
        }
    }
    return bestMargin >= -1e-7 ? best : -1;
}

std::vector<QPointF> clipTriangle(const Corners& c, const Corners& p, double tol)
{
    std::vector<QPointF> poly(p.begin(), p.end());
    if (twiceArea(p) < 0) std::reverse(poly.begin(), poly.end());
    const bool ccw = twiceArea(c) > 0;
    for (int i = 0; i < 3 && !poly.empty(); ++i) {
        const QPointF a = c[std::size_t(ccw ? i : 2 - i)], b = c[std::size_t(ccw ? (i + 1) % 3 : (4 - i) % 3)];
        const QPointF e = b - a;
        const double len = std::hypot(e.x(), e.y());
        // (Inside: on the side's left - not widened: two triangles beside a
        // side would both take a strip of it.)
        auto side = [&](QPointF q) { return cross2(e, q - a) / std::max(len, 1e-300); };
        std::vector<QPointF> out;
        for (std::size_t k = 0; k < poly.size(); ++k) {
            const QPointF q = poly[k], r = poly[(k + 1) % poly.size()];
            const double sq = side(q), sr = side(r);
            if (sq >= 0) out.push_back(q);
            if ((sq >= 0) != (sr >= 0)) out.push_back(q + (r - q) * (sq / (sq - sr)));
        }
        poly = std::move(out);
    }
    if (poly.size() < 3 || polygonArea(poly) <= tol * tol * 1e-6) return {};
    return poly;
}

double overlayArea(const PlanLocator& at, const Corners& p)
{
    const QPointF lo(std::min({p[0].x(), p[1].x(), p[2].x()}), std::min({p[0].y(), p[1].y(), p[2].y()}));
    const QPointF hi(std::max({p[0].x(), p[1].x(), p[2].x()}), std::max({p[0].y(), p[1].y(), p[2].y()}));
    const double tol = 1e-9 * sizeOf(p);
    double area = 0;
    for (int t : at.near(lo, hi)) area += polygonArea(clipTriangle(at.corners(t), p, tol));
    return area;
}

// ------------------------------------------------------- the constraints

namespace {

/// The segment from \a a to \a b in the located triangles' pieces: cut where
/// it crosses their sides, each piece the triangle that holds its middle -
/// one, also where it runs along a side two triangles share (a tangential
/// trace the same from either).
struct Piece {
    double s0, s1;
    int t;
};
std::vector<Piece> piecesOf(const PlanLocator& at, QPointF a, QPointF b)
{
    std::vector<double> cuts{0.0, 1.0};
    const QPointF d = b - a;
    for (int t : at.near(a, b)) {
        const Corners& c = at.corners(t);
        for (int k = 0; k < 3; ++k) {
            const QPointF p = c[std::size_t(k)], e = c[std::size_t((k + 1) % 3)] - p;
            const double den = cross2(d, e);
            if (std::abs(den) <= 1e-14 * std::hypot(d.x(), d.y()) * std::hypot(e.x(), e.y())) continue;
            const double s = cross2(p - a, e) / den, u = cross2(p - a, d) / den;
            if (s > 0 && s < 1 && u >= -1e-12 && u <= 1 + 1e-12) cuts.push_back(s);
        }
    }
    std::sort(cuts.begin(), cuts.end());
    std::vector<Piece> out;
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        if (cuts[i + 1] - cuts[i] <= 1e-12) continue;
        double xi = 0, eta = 0;
        const int t = at.locate(a + d * (0.5 * (cuts[i] + cuts[i + 1])), &xi, &eta);
        if (t >= 0) out.push_back({cuts[i], cuts[i + 1], t});
    }
    return out;
}

/// An edge space's interface pieces: band \a band's plan as located, its
/// edge functions' degrees of freedom at the interface's level.
struct EdgeSide {
    const EdgeSpace3& space;
    int band, level;
    PlanLocator at;
    EdgeSide(const EdgeSpace3& s, int b, int height)
        : space(s), band(b), level(s.levelOfHeight(height)), at(*s.mesh().bands[std::size_t(b)].plan, s.mesh().unitScale)
    {
    }
    AffineMap mapOf(int t) const { return affineMap(space.planNodes(band), t); }
};

/// The coarse side's tangential trace's moments along the segment from \a a
/// to \a b (against 1 and 2s - 1, \a d its direction a -> b in metres), each
/// a combination of its degrees of freedom.
void segmentMoments(const EdgeSide& coarse, QPointF a, QPointF b, Combination& m0, Combination& m1)
{
    const QPointF d = b - a;
    const EdgeSpace& edges = coarse.space.planEdges(coarse.band);
    for (const Piece& piece : piecesOf(coarse.at, a, b)) {
        const int t = piece.t;
        const double s0 = piece.s0, s1 = piece.s1;
        const AffineMap am = coarse.mapOf(t);
        const int* ed = edges.dofs(t);
        for (const auto& [g, w] : gauss4()) {
            const double s = s0 + g * (s1 - s0), ww = w * (s1 - s0);
            double xi = 0, eta = 0;
            barycentric(cornersOf(am), a + d * s, &xi, &eta);
            EdgeSpace::Basis eb;
            edges.basis(t, am, xi, eta, eb);
            for (int k = 0; k < eb.count; ++k) {
                const int dof = coarse.space.acrossDof(coarse.band, ed[k], coarse.level);
                if (dof < 0) continue;
                const double v = eb.vx[k] * d.x() + eb.vy[k] * d.y();
                m0[dof] += ww * v;
                m1[dof] += ww * (2 * s - 1) * v;
            }
        }
    }
}

/// The coarse side's trace's integral over triangle \a p (its x and y
/// components), over the overlay.
void triangleMoments(const EdgeSide& coarse, const Corners& p, Combination& fx, Combination& fy)
{
    const QPointF lo(std::min({p[0].x(), p[1].x(), p[2].x()}), std::min({p[0].y(), p[1].y(), p[2].y()}));
    const QPointF hi(std::max({p[0].x(), p[1].x(), p[2].x()}), std::max({p[0].y(), p[1].y(), p[2].y()}));
    const double tol = 1e-9 * sizeOf(p);
    const EdgeSpace& edges = coarse.space.planEdges(coarse.band);
    for (int t : coarse.at.near(lo, hi)) {
        const std::vector<QPointF> poly = clipTriangle(coarse.at.corners(t), p, tol);
        if (poly.empty()) continue;
        const AffineMap am = coarse.mapOf(t);
        const int* ed = edges.dofs(t);
        for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
            const QPointF q0 = poly[0], q1 = poly[i], q2 = poly[i + 1];
            const double jac = std::abs(cross2(q1 - q0, q2 - q0));
            for (const auto& q : triangleRule(4).points) {
                const QPointF x = q0 + (q1 - q0) * q[0] + (q2 - q0) * q[1];
                double xi = 0, eta = 0;
                barycentric(cornersOf(am), x, &xi, &eta);
                EdgeSpace::Basis eb;
                edges.basis(t, am, xi, eta, eb);
                for (int k = 0; k < eb.count; ++k) {
                    const int dof = coarse.space.acrossDof(coarse.band, ed[k], coarse.level);
                    if (dof < 0) continue;
                    fx[dof] += q[2] * jac * eb.vx[k];
                    fy[dof] += q[2] * jac * eb.vy[k];
                }
            }
        }
    }
}

/// A nodal space's interface side.
struct NodeSide {
    const Space3& space;
    int band, height, level;
    PlanLocator at;
    NodeSide(const Space3& s, int b, int h)
        : space(s), band(b), height(h), level(s.order() == 2 ? 2 * h : h), at(*s.mesh().bands[std::size_t(b)].plan, s.mesh().unitScale)
    {
    }
};

/// The coarse side's trace at \a p, a combination of its degrees of freedom.
Combination traceAt(const NodeSide& coarse, QPointF p)
{
    Combination out;
    double xi = 0, eta = 0;
    const int t = coarse.at.locate(p, &xi, &eta);
    if (t < 0) return out;
    const Space& plan = coarse.space.plan(coarse.band);
    locateIn(plan, t, coarse.at.corners(t), p, &xi, &eta);
    Shape sh;
    shapeAt(coarse.space.order(), xi, eta, sh);
    const int* d2 = plan.dofs(t);
    for (int k = 0; k < sh.count; ++k) {
        const int dof = coarse.space.dofAt(coarse.band, d2[k], coarse.level);
        if (dof >= 0) out[dof] += sh.n[k];
    }
    return out;
}

/// Its mean along the segment from \a a to \a b.
Combination traceMean(const NodeSide& coarse, QPointF a, QPointF b)
{
    Combination out;
    const QPointF d = b - a;
    const Space& plan = coarse.space.plan(coarse.band);
    for (const Piece& piece : piecesOf(coarse.at, a, b)) {
        const int t = piece.t;
        const double s0 = piece.s0, s1 = piece.s1;
        const int* d2 = plan.dofs(t);
        for (const auto& [g, w] : gauss4()) {
            const double s = s0 + g * (s1 - s0);
            double xi = 0, eta = 0;
            locateIn(plan, t, coarse.at.corners(t), a + d * s, &xi, &eta);
            Shape sh;
            shapeAt(coarse.space.order(), xi, eta, sh);
            for (int k = 0; k < sh.count; ++k) {
                const int dof = coarse.space.dofAt(coarse.band, d2[k], coarse.level);
                if (dof >= 0) out[dof] += w * (s1 - s0) * sh.n[k];
            }
        }
    }
    return out;
}

/// The degrees of freedom of band \a band's prisms beside height \a height.
std::unordered_set<int> usedBeside(const Space3& space, int band, int height)
{
    std::unordered_set<int> out;
    const Mesh3& m = space.mesh();
    const int layer = layerBeside(m, band, height);
    for (int e = 0; e < space.elements(); ++e)
        if (m.prismBand[std::size_t(e)] == band && m.prismLayer[std::size_t(e)] == layer)
            for (int k = 0; k < space.perElement(); ++k) out.insert(space.dofs(e)[k]);
    return out;
}

} // namespace

Constraints3 edgeConstraints(const EdgeSpace3& space)
{
    const Mesh3& m = space.mesh();
    std::map<int, Combination> rows;
    if (!m.byBand()) return {};
    for (const Interface& f : interfacesOf(m)) {
        const EdgeSide fine(space, f.fine, f.height), coarse(space, f.coarse, f.height);
        const EdgeSpace& edges = space.planEdges(f.fine);
        const int n = edges.perElement(), order = space.order();
        for (int t : trianglesIn(m, f.fine, layerBeside(m, f.fine, f.height))) {
            const int* ed = edges.dofs(t);
            bool wanted = false;
            for (int k = 0; k < n; ++k) {
                const int dof = space.acrossDof(f.fine, ed[k], fine.level);
                wanted = wanted || (dof >= 0 && !rows.count(dof));
            }
            if (!wanted) continue;
            const AffineMap am = fine.mapOf(t);
            const Corners c = cornersOf(am);
            // The coarse trace's moments on this triangle, in its moment
            // matrix's order.
            std::vector<Combination> mu;
            for (int l = 0; l < 3; ++l) {
                Combination m0, m1;
                segmentMoments(coarse, c[std::size_t(l)], c[std::size_t((l + 1) % 3)], m0, m1);
                mu.push_back(m0);
                if (order == 2) mu.push_back(m1);
            }
            if (order == 2) {
                Combination fx, fy;
                triangleMoments(coarse, c, fx, fy);
                mu.push_back(fx);
                mu.push_back(fy);
            }
            const Eigen::MatrixXd inv = momentMatrix(edges, t, am).inverse();
            for (int k = 0; k < n; ++k) {
                const int dof = space.acrossDof(f.fine, ed[k], fine.level);
                if (dof < 0 || rows.count(dof)) continue;
                Combination row;
                for (int i = 0; i < n; ++i) addTo(row, mu[std::size_t(i)], inv(k, i));
                rows[dof] = row;
            }
        }
    }
    return finish(rows);
}

Constraints3 nodalConstraints(const Space3& space)
{
    const Mesh3& m = space.mesh();
    std::map<int, Combination> rows;
    if (!m.byBand()) return {};
    for (const Interface& f : interfacesOf(m)) {
        const NodeSide fine(space, f.fine, f.height), coarse(space, f.coarse, f.height);
        const std::unordered_set<int> shared = usedBeside(space, f.coarse, f.height);
        const Space& plan = space.plan(f.fine);
        for (int t : trianglesIn(m, f.fine, layerBeside(m, f.fine, f.height))) {
            const int* d2 = plan.dofs(t);
            const int corners = 3, per = space.order() == 2 ? 6 : 3;
            for (int a = 0; a < per; ++a) {
                const int dof = space.dofAt(f.fine, d2[a], fine.level);
                if (dof < 0 || rows.count(dof) || shared.count(dof)) continue;
                const QPointF p = plan.point(d2[a]);
                if (a < corners) {
                    rows[dof] = traceAt(coarse, p);
                    continue;
                }
                // An edge's middle: from the trace's mean along it (a
                // straight edge's), its value there on a curved one.
                const QPointF pa = plan.point(d2[a - 3]), pb = plan.point(d2[(a - 3 + 1) % 3]);
                const QPointF mid = (pa + pb) * 0.5;
                if (std::hypot(p.x() - mid.x(), p.y() - mid.y()) > 1e-9 * std::hypot(pb.x() - pa.x(), pb.y() - pa.y())) {
                    rows[dof] = traceAt(coarse, p);
                    continue;
                }
                Combination row;
                addTo(row, traceMean(coarse, pa, pb), 6.0 / 4);
                addTo(row, traceAt(coarse, pa), -1.0 / 4);
                addTo(row, traceAt(coarse, pb), -1.0 / 4);
                rows[dof] = row;
            }
        }
    }
    return finish(rows);
}

bool constraintsUnchained(const Constraints3& c, int n, QString* why)
{
    std::vector<char> constrained(std::size_t(std::max(n, 0)), 0);
    for (int d : c.dofs)
        if (d >= 0 && d < n) constrained[std::size_t(d)] = 1;
    for (std::size_t k = 0; k < c.dofs.size(); ++k)
        for (const auto& [m, w] : c.rows[k])
            if (m < 0 || m >= n || constrained[std::size_t(m)]) {
                if (why)
                    *why = QCoreApplication::translate("fem3", "the interfaces' constraints chain: degree of freedom %1's master %2 is "
                                                               "constrained itself")
                               .arg(c.dofs[k])
                               .arg(m);
                return false;
            }
    return true;
}

Eigen::SparseMatrix<double> reductionMatrix(int n, const Constraints3& c, std::vector<int>* reduced, QString* why)
{
    // (A master constrained: not taken - its weight dropped silently, else,
    // where Q_ASSERT is not compiled.)
    if (!constraintsUnchained(c, n, why)) {
        if (reduced) reduced->clear();
        return Eigen::SparseMatrix<double>();
    }
    std::vector<int> red(std::size_t(n), 0);
    for (int d : c.dofs) red[std::size_t(d)] = -1;
    int next = 0;
    for (int& r : red)
        if (r == 0) r = next++;
        else r = -1;
    // (Rebuilt: the first pass marked; the second numbered.)
    next = 0;
    std::vector<char> constrained(std::size_t(n), 0);
    for (int d : c.dofs) constrained[std::size_t(d)] = 1;
    for (int i = 0; i < n; ++i) red[std::size_t(i)] = constrained[std::size_t(i)] ? -1 : next++;
    std::vector<Eigen::Triplet<double>> t;
    for (int i = 0; i < n; ++i)
        if (red[std::size_t(i)] >= 0) t.emplace_back(i, red[std::size_t(i)], 1.0);
    for (std::size_t k = 0; k < c.dofs.size(); ++k)
        for (const auto& [master, w] : c.rows[k]) t.emplace_back(c.dofs[k], red[std::size_t(master)], w);
    Eigen::SparseMatrix<double> C(n, next);
    C.setFromTriplets(t.begin(), t.end());
    if (reduced) *reduced = red;
    return C;
}

void interpolateEdges(const EdgeSpace3& space, int band, int height, const std::function<void(QPointF, double[2])>& field,
                      std::vector<double>& values)
{
    const Mesh3& m = space.mesh();
    const EdgeSpace& edges = space.planEdges(band);
    const int level = space.levelOfHeight(height), n = edges.perElement();
    for (int t : trianglesIn(m, band, layerBeside(m, band, height))) {
        const AffineMap am = affineMap(space.planNodes(band), t);
        const std::vector<double> mu = edgeMoments<double>(
            space.order(), cornersOf(am), am.det,
            [&](double xi, double eta) {
                double f[2] = {0, 0};
                field(am.at(xi, eta), f);
                return std::pair<double, double>(f[0], f[1]);
            },
            0.0);
        const Eigen::VectorXd coef = momentMatrix(edges, t, am).inverse() * Eigen::Map<const Eigen::VectorXd>(mu.data(), n);
        const int* ed = edges.dofs(t);
        for (int k = 0; k < n; ++k) {
            const int dof = space.acrossDof(band, ed[k], level);
            if (dof >= 0 && dof < int(values.size())) values[std::size_t(dof)] = coef(k);
        }
    }
}

void interpolateNodes(const Space3& space, int band, int height, const std::function<double(QPointF)>& f, std::vector<double>& values)
{
    const Mesh3& m = space.mesh();
    const Space& plan = space.plan(band);
    const int level = space.order() == 2 ? 2 * height : height, per = space.order() == 2 ? 6 : 3;
    for (int t : trianglesIn(m, band, layerBeside(m, band, height))) {
        const int* d2 = plan.dofs(t);
        for (int a = 0; a < per; ++a) {
            const int dof = space.dofAt(band, d2[a], level);
            if (dof < 0 || dof >= int(values.size())) continue;
            const QPointF p = plan.point(d2[a]);
            if (a < 3) {
                values[std::size_t(dof)] = f(p);
                continue;
            }
            const QPointF pa = plan.point(d2[a - 3]), pb = plan.point(d2[(a - 3 + 1) % 3]);
            const QPointF mid = (pa + pb) * 0.5;
            if (std::hypot(p.x() - mid.x(), p.y() - mid.y()) > 1e-9 * std::hypot(pb.x() - pa.x(), pb.y() - pa.y())) {
                values[std::size_t(dof)] = f(p);
                continue;
            }
            double mean = 0;
            for (const auto& [s, w] : gauss4()) mean += w * f(pa + (pb - pa) * s);
            values[std::size_t(dof)] = (6 * mean - f(pa) - f(pb)) / 4;
        }
    }
}

} // namespace qucs_s::fem
