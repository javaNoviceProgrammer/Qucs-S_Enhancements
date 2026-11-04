/*
 * fem3_results.cpp - a 3D solution's results: nodal values, slices, a
 *                    study's instances and derived values
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_results.h"

#include "fem3_geometry.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QSet>

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

// ------------------------------------------------- Isosurfaces, any slice

namespace {

/// A prism's three tetrahedra - its corners 0, 1, 2 its floor and 3, 4, 5
/// above them, as Mesh3 numbers them -, each wall split along its diagonal
/// through its corner of the least global number, as the prism beside it
/// splits the wall they share: an isosurface without cracks (Dompierre,
/// Labbé, Vallet and Camarero, "How to subdivide pyramids, prisms and
/// hexahedra into tetrahedra", 1999).
std::array<std::array<int, 4>, 3> prismTets(const std::array<int, 6>& g)
{
    int least = 0;
    for (int i = 1; i < 6; ++i)
        if (g[std::size_t(i)] < g[std::size_t(least)]) least = i;
    // Its corners turned so the least is a: a, b, c a floor (or the top)
    // and d, e, f across from them.
    static const int turn[6][6] = {{0, 1, 2, 3, 4, 5}, {1, 2, 0, 4, 5, 3}, {2, 0, 1, 5, 3, 4},
                                   {3, 4, 5, 0, 1, 2}, {4, 5, 3, 1, 2, 0}, {5, 3, 4, 2, 0, 1}};
    const int* t = turn[least];
    const int a = t[0], b = t[1], c = t[2], d = t[3], e = t[4], f = t[5];
    // The walls by a split through it; the third through its least corner.
    if (std::min(g[std::size_t(b)], g[std::size_t(f)]) < std::min(g[std::size_t(c)], g[std::size_t(e)]))
        return {{{a, b, c, f}, {a, b, f, e}, {a, e, f, d}}};
    return {{{a, b, c, e}, {a, e, c, f}, {a, e, f, d}}};
}

constexpr double CornerRef[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};

/// A point where a tetrahedron's edge crosses a level: its place (the
/// geometry's unit) and its reference coordinates in the prism.
struct Crossing {
    Vec3 x;
    double ref[3];
};

/// The triangles where the linear function of \a v (at a tetrahedron's
/// corners \a p, reference coordinates \a r) is nought: none, one or two.
void marchTet(const std::array<Vec3, 4>& p, const std::array<std::array<double, 3>, 4>& r, const std::array<double, 4>& v,
              std::vector<std::array<Crossing, 3>>& out)
{
    int above[4], na = 0, below[4], nb = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(v[std::size_t(i)])) return;
        if (v[std::size_t(i)] > 0) above[na++] = i;
        else below[nb++] = i;
    }
    if (na == 0 || nb == 0) return;
    auto cross = [&](int i, int j) {
        const double a = v[std::size_t(i)], b = v[std::size_t(j)];
        const double u = a == b ? 0.5 : a / (a - b);
        Crossing c;
        const Vec3& pi = p[std::size_t(i)];
        const Vec3& pj = p[std::size_t(j)];
        c.x = Vec3{pi.x + u * (pj.x - pi.x), pi.y + u * (pj.y - pi.y), pi.z + u * (pj.z - pi.z)};
        for (int k = 0; k < 3; ++k) c.ref[k] = r[std::size_t(i)][std::size_t(k)] + u * (r[std::size_t(j)][std::size_t(k)] - r[std::size_t(i)][std::size_t(k)]);
        return c;
    };
    if (na == 1 || nb == 1) {
        // One corner alone on its side: a triangle about it.
        const int lone = na == 1 ? above[0] : below[0];
        const int* rest = na == 1 ? below : above;
        out.push_back({cross(lone, rest[0]), cross(lone, rest[1]), cross(lone, rest[2])});
        return;
    }
    // Two and two: a quadrilateral, as two triangles.
    const Crossing q0 = cross(above[0], below[0]), q1 = cross(above[0], below[1]), q2 = cross(above[1], below[1]), q3 = cross(above[1], below[0]);
    out.push_back({q0, q1, q2});
    out.push_back({q0, q2, q3});
}

} // namespace

std::vector<IsoTriangle> isosurfaces(const Solution3& s, const std::vector<double>& nodal, const std::vector<double>& levels)
{
    std::vector<IsoTriangle> out;
    const Mesh3& m = s.mesh();
    if (nodal.size() != m.nodes.size()) return out;
    std::vector<std::array<Crossing, 3>> cut;
    for (int p = 0; p < int(m.prisms.size()); ++p) {
        const std::array<int, 6>& g = m.prisms[std::size_t(p)];
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (int n : g) {
            lo = std::min(lo, nodal[std::size_t(n)]);
            hi = std::max(hi, nodal[std::size_t(n)]);
        }
        if (!(hi >= lo)) continue;
        const auto tets = prismTets(g);
        for (int l = 0; l < int(levels.size()); ++l) {
            const double level = levels[std::size_t(l)];
            if (level < lo || level > hi) continue;
            for (const auto& t : tets) {
                std::array<Vec3, 4> x;
                std::array<std::array<double, 3>, 4> r;
                std::array<double, 4> v;
                for (int k = 0; k < 4; ++k) {
                    const int corner = t[std::size_t(k)];
                    x[std::size_t(k)] = m.nodes[std::size_t(g[std::size_t(corner)])];
                    r[std::size_t(k)] = {CornerRef[corner][0], CornerRef[corner][1], CornerRef[corner][2]};
                    v[std::size_t(k)] = nodal[std::size_t(g[std::size_t(corner)])] - level;
                }
                cut.clear();
                marchTet(x, r, v, cut);
                for (const auto& tri : cut) out.push_back({{tri[0].x, tri[1].x, tri[2].x}, l, m.domain[std::size_t(p)]});
            }
        }
    }
    return out;
}

std::vector<SliceTriangle> slicePlane(const Solution3& s, const Expression& e, const Vec3& origin, const Vec3& normal)
{
    std::vector<SliceTriangle> out;
    const Mesh3& m = s.mesh();
    const double len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (!e.isValid() || !(len > 0)) return out;
    const Vec3 n{normal.x / len, normal.y / len, normal.z / len};
    std::vector<double> distance(m.nodes.size());
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        const Vec3& x = m.nodes[i];
        distance[i] = (x.x - origin.x) * n.x + (x.y - origin.y) * n.y + (x.z - origin.z) * n.z;
    }
    PointEvaluator3 ev(s);
    std::vector<std::array<Crossing, 3>> cut;
    for (int p = 0; p < int(m.prisms.size()); ++p) {
        const std::array<int, 6>& g = m.prisms[std::size_t(p)];
        bool pos = false, neg = false;
        for (int k : g) (distance[std::size_t(k)] > 0 ? pos : neg) = true;
        if (!pos || !neg) continue;
        for (const auto& t : prismTets(g)) {
            std::array<Vec3, 4> x;
            std::array<std::array<double, 3>, 4> r;
            std::array<double, 4> v;
            for (int k = 0; k < 4; ++k) {
                const int corner = t[std::size_t(k)];
                x[std::size_t(k)] = m.nodes[std::size_t(g[std::size_t(corner)])];
                r[std::size_t(k)] = {CornerRef[corner][0], CornerRef[corner][1], CornerRef[corner][2]};
                v[std::size_t(k)] = distance[std::size_t(g[std::size_t(corner)])];
            }
            cut.clear();
            marchTet(x, r, v, cut);
            for (const auto& tri : cut) {
                SliceTriangle st;
                st.domain = m.domain[std::size_t(p)];
                for (int k = 0; k < 3; ++k) {
                    st.p[std::size_t(k)] = tri[std::size_t(k)].x;
                    ev.at(p, tri[std::size_t(k)].ref[0], tri[std::size_t(k)].ref[1], tri[std::size_t(k)].ref[2]);
                    st.value[std::size_t(k)] = ev.eval(e);
                }
                out.push_back(st);
            }
        }
    }
    return out;
}

std::vector<Arrow3> arrows3(const Solution3& s, const std::array<Expression, 3>& v, const Box3& box, int nx, int ny, int nz)
{
    std::vector<Arrow3> out;
    for (const Expression& e : v)
        if (!e.isValid()) return out;
    nx = std::clamp(nx, 1, 200);
    ny = std::clamp(ny, 1, 200);
    nz = std::clamp(nz, 1, 200);
    const double k = s.mesh().unitScale;
    PointEvaluator3 ev(s);
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j)
            for (int l = 0; l < nz; ++l) {
                const Vec3 at{box.x0 + (i + 0.5) * (box.x1 - box.x0) / nx, box.y0 + (j + 0.5) * (box.y1 - box.y0) / ny,
                              box.z0 + (l + 0.5) * (box.z1 - box.z0) / nz};
                if (!ev.at(Vec3{k * at.x, k * at.y, k * at.z})) continue;
                const Vec3 a{ev.eval(v[0]), ev.eval(v[1]), ev.eval(v[2])};
                if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z)) continue;
                out.push_back({at, a});
            }
    return out;
}

// --------------------------------------------------------- Instances

std::vector<Instance3> instances3(const Solutions3& set, bool all, int point, const QString& time, QString* error)
{
    std::vector<Instance3> out;
    if (set.empty()) return out;
    const bool swept = set.size() > 1;
    auto add = [&out, &set, swept, all](int p, int snapshot) {
        const Solution3& s = *set[std::size_t(p)];
        Instance3 in;
        in.point = p;
        in.snapshot = snapshot;
        in.time = snapshot >= 0 ? s.snapshotTime(snapshot) : 0;
        if (all) {
            QStringList label;
            if (swept) label << s.label;
            if (snapshot >= 0 && s.timeDependent) label << QStringLiteral("t = %1").arg(formatQuantity(in.time, Dim::of(0, 0, 1)));
            in.label = label.join(QLatin1String(", "));
        }
        out.push_back(in);
    };
    if (all) {
        for (int p = 0; p < int(set.size()); ++p) {
            const int n = set[std::size_t(p)]->snapshotCount();
            if (n == 0) add(p, -1);
            for (int i = 0; i < n; ++i) add(p, i);
        }
        return out;
    }
    const int p = point <= 0 || point > int(set.size()) ? int(set.size()) - 1 : point - 1;
    const Solution3& s = *set[std::size_t(p)];
    int snapshot = s.snapshotCount() > 0 ? s.snapshotCount() - 1 : -1;
    if (!time.trimmed().isEmpty() && s.snapshotCount() > 0) {
        QString why;
        const std::optional<double> t = evaluateConstant(time, s.parameters().scope, Dim::of(0, 0, 1), &why);
        if (!t) {
            if (error) *error = QCoreApplication::translate("Results3", "the time %1: %2").arg(time, why);
            return out;
        }
        snapshot = s.snapshotAt(*t);
    }
    add(p, snapshot);
    return out;
}

Solution3* select3(const Solutions3& set, const Instance3& at)
{
    if (at.point < 0 || at.point >= int(set.size()) || !set[std::size_t(at.point)]) return nullptr;
    Solution3* s = set[std::size_t(at.point)].get();
    if (at.snapshot >= 0) s->select(at.snapshot);
    return s;
}

// ------------------------------------------------------- Derived values

namespace {

QString tr3(const char* text)
{
    return QCoreApplication::translate("Results3", text);
}

/// The space a solution's points are mapped by: its highest order's (an
/// order 2 one curved where its triangle is), null without a field.
const Space3* mapSpace(const Solution3& s)
{
    const Space3* best = nullptr;
    for (const Field3& f : s.fields())
        if (f.space && (!best || f.space->order() > best->order())) best = f.space.get();
    return best;
}

/// Gauss's 3 points on [0, 1]: where, and their weights.
constexpr double GaussAt[3] = {0.5 - 0.3872983346207417, 0.5, 0.5 + 0.3872983346207417};
constexpr double GaussWeight[3] = {5.0 / 18, 8.0 / 18, 5.0 / 18};

} // namespace

std::optional<double> evaluateAt3(const Solution3& s, const Expression& e, const Vec3& x)
{
    if (!e.isValid()) return std::nullopt;
    PointEvaluator3 ev(s);
    if (!ev.at(x)) return std::nullopt;
    const double v = ev.eval(e);
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

Over3 over3(const Solution3& s, const Expression& e, Level level, const QVector<int>& picked)
{
    Over3 o;
    const Space3* sp = mapSpace(s);
    if (!sp || !e.isValid()) return o;
    const Mesh3& m = s.mesh();
    const Topology3& topo = s.topology();
    const QSet<int> on(picked.begin(), picked.end());
    PointEvaluator3 ev(s);
    auto take = [&o](double w, double v) {
        if (!std::isfinite(v)) return;
        o.integral += w * v;
        o.measure += w;
        o.min = std::min(o.min, v);
        o.max = std::max(o.max, v);
        o.any = true;
    };
    auto extreme = [&o](double v) {
        if (!std::isfinite(v)) return;
        o.min = std::min(o.min, v);
        o.max = std::max(o.max, v);
    };
    if (level == Level::Domain) {
        const PrismRule& rule = prismRule(4, 3);
        static const double corner[6][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
        for (int p = 0; p < int(m.prisms.size()); ++p) {
            if (!on.contains(m.domain[std::size_t(p)])) continue;
            for (const auto& q : rule.points) {
                const Space3::Map map = sp->map(p, q[0], q[1], q[2]);
                ev.at(p, q[0], q[1], q[2], map.x);
                take(q[3] * std::abs(map.det), ev.eval(e));
            }
            for (const auto& c : corner) {
                ev.at(p, c[0], c[1], c[2]);
                extreme(ev.eval(e));
            }
        }
        return o;
    }
    if (level == Level::Boundary) {
        // Each of the mesh's faces on a face picked, read on the side behind
        // it (in front where nothing is behind, or it is not defined there).
        for (int fi = 0; fi < int(m.faces.size()); ++fi) {
            const int face = m.faces[std::size_t(fi)].face;
            if (!on.contains(face)) continue;
            const Topology3::Face& tf = topo.faces[std::size_t(face)];
            for (int side : {tf.back, tf.front}) {
                if (side < 0) continue;
                const std::vector<Space3::FacePoint> points = sp->facePoints(fi, 4, side);
                bool defined = false;
                std::vector<std::pair<double, double>> got;
                for (const Space3::FacePoint& fp : points) {
                    if (fp.prism < 0) continue;
                    ev.at(fp.prism, fp.xi, fp.eta, fp.zeta, fp.x);
                    const double v = ev.eval(e);
                    defined = defined || std::isfinite(v);
                    got.emplace_back(fp.weight, v);
                }
                if (!defined) continue;
                for (const auto& [w, v] : got) take(w, v);
                break;
            }
        }
        return o;
    }
    if (level == Level::Edge) {
        // Each edge picked, segment by segment of its polyline, by Gauss's
        // three points, each found in the mesh.
        const double k = topo.unitScale;
        for (int edge : picked) {
            if (edge < 0 || edge >= int(topo.edges.size())) continue;
            const std::vector<Vec3> line = topo.edgePolyline(edge);
            for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                const Vec3 a = line[i], b = line[i + 1];
                const double length = k * std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
                for (int g = 0; g < 3; ++g) {
                    const double t = GaussAt[g];
                    const Vec3 x{k * (a.x + t * (b.x - a.x)), k * (a.y + t * (b.y - a.y)), k * (a.z + t * (b.z - a.z))};
                    if (ev.at(x)) take(GaussWeight[g] * length, ev.eval(e));
                }
                for (const Vec3& end : {a, b})
                    if (ev.at(Vec3{k * end.x, k * end.y, k * end.z})) extreme(ev.eval(e));
            }
        }
        return o;
    }
    return o;
}

DerivedResult evaluateDerived3(const Solution3& s, const Node& node)
{
    DerivedResult out;
    const QString type = node.type;
    const QString unitText = node.text(QStringLiteral("unit"));
    auto add = [&](const QString& name, double value, const Dim& dim, const QString& expression = {}) {
        QString warning;
        DisplayUnit unit = displayUnit(unitText, dim, &warning);
        if (unit.name.isEmpty() && unitText.trimmed().isEmpty() && !expression.isEmpty()) unit = quotientUnit(expression, dim, s.scope());
        out.values << DerivedValue{name, value, dim, shown(value, dim, unit)};
        if (!warning.isEmpty() && out.error.isEmpty()) out.error = warning;
    };
    if (type == QLatin1String("global")) {
        QStringList problems;
        for (const QString& raw : node.text(QStringLiteral("expression")).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const NamedExpression item = namedExpression(raw);
            if (item.expression.isEmpty()) continue;
            const Expression e = Expression::compile(item.expression, s.scope());
            if (!e.isValid()) {
                problems << tr3("%1: %2").arg(item.expression, e.error());
                continue;
            }
            if (!e.isConstant()) {
                problems << tr3("%1 varies over the model: evaluate it at a point, or integrate it").arg(item.expression);
                continue;
            }
            add(item.name, e.constant(), e.dim(), item.expression);
        }
        if (!problems.isEmpty()) out.error = problems.join(QLatin1String("; "));
        return out;
    }
    if (type == QLatin1String("integral") || type == QLatin1String("average") || type == QLatin1String("maximum")
        || type == QLatin1String("minimum")) {
        const QString text = node.text(QStringLiteral("expression"));
        const Expression e = Expression::compile(text, s.scope());
        if (!e.isValid()) {
            out.error = tr3("%1: %2").arg(text, e.error());
            return out;
        }
        const Level level = levelOf(node.text(QStringLiteral("level")));
        QString why;
        const QVector<int> picked = resolveSelection3(s.topology(), s.model(), level, node.selection(), &why);
        if (!why.isEmpty()) {
            out.error = why;
            return out;
        }
        const Over3 o = over3(s, e, level, picked);
        if (!o.any) {
            out.error = tr3("%1 is defined on none of them").arg(text);
            return out;
        }
        const Dim measure = level == Level::Edge ? Dim::length() : level == Level::Boundary ? Dim::area() : Dim::of(3);
        if (type == QLatin1String("integral")) add(text, o.integral, e.dim() * measure);
        else if (type == QLatin1String("average")) add(text, o.integral / o.measure, e.dim(), text);
        else if (type == QLatin1String("maximum")) add(text, o.max, e.dim(), text);
        else add(text, o.min, e.dim(), text);
        return out;
    }
    if (type == QLatin1String("pointeval")) {
        const QString text = node.text(QStringLiteral("expression"));
        const Expression e = Expression::compile(text, s.scope());
        if (!e.isValid()) {
            out.error = tr3("%1: %2").arg(text, e.error());
            return out;
        }
        const double k = s.topology().unitScale;
        std::vector<Vec3> where;   // the geometry's unit
        if (node.text(QStringLiteral("where")) == QLatin1String("points")) {
            QString why;
            for (int p : resolveSelection3(s.topology(), s.model(), Level::Point, node.selection(), &why))
                if (p >= 0 && p < int(s.topology().points.size())) where.push_back(s.topology().points[std::size_t(p)].position);
            if (!why.isEmpty()) out.error = why;
        } else {
            for (const QJsonValue& v : node.value(QStringLiteral("coordinates3")).toArray()) {
                const QJsonArray row = v.toArray();
                double c[3] = {0, 0, 0};
                bool ok = true;
                for (int i = 0; i < 3 && ok; ++i) {
                    const QString t = row.at(i).isDouble() ? formatNumber(row.at(i).toDouble(), 15) : row.at(i).toString();
                    Expression::Options options;
                    options.lengthUnit = k;
                    options.numbersAreLengths = true;
                    const Expression ce = Expression::compile(t.trimmed().isEmpty() ? QStringLiteral("0") : t, s.parameters().scope, options);
                    if (!ce.isValid() || !ce.isConstant()) {
                        out.error = tr3("the coordinate %1: %2").arg(t, ce.isValid() ? tr3("not a constant") : ce.error());
                        ok = false;
                        break;
                    }
                    c[i] = ce.constant();
                }
                if (ok) where.push_back(Vec3{c[0], c[1], c[2]});
            }
        }
        for (const Vec3& p : where) {
            const QString name = tr3("%1 at (%2, %3, %4)").arg(text, formatNumber(p.x, 6), formatNumber(p.y, 6), formatNumber(p.z, 6));
            const std::optional<double> v = evaluateAt3(s, e, Vec3{k * p.x, k * p.y, k * p.z});
            if (!v) {
                if (out.error.isEmpty()) out.error = tr3("%1 is in no domain where it is defined").arg(name);
                continue;
            }
            add(name, *v, e.dim(), text);
        }
        return out;
    }
    out.error = tr3("%1: a %2 is not evaluated in 3D").arg(node.name(), node.kind() ? node.kind()->title : node.type);
    return out;
}

} // namespace qucs_s::fem
