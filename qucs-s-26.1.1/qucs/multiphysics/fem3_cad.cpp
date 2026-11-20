/*
 * fem3_cad.cpp - a 3D multiphysics model's geometry and mesh to other
 *                tools' formats (STL, OBJ, a faceted STEP; Gmsh's MSH 4.1,
 *                Abaqus's INP, Nastran's bulk data), and triangulated solids
 *                read (STL, OBJ) and cut into a layered geometry's slabs
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_cad.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QHash>
#include <QIODevice>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Cad3", text);
}
QString tr(const char* text, const char* comment, int n)
{
    return QCoreApplication::translate("qucs_s::fem::Cad3", text, comment, n);
}

/// A name fit for the formats: letters, digits and _ (else _).
QString plainName(const QString& name)
{
    QString out;
    for (const QChar c : name) out += c.isLetterOrNumber() && c.unicode() < 128 ? c : QLatin1Char('_');
    return out.isEmpty() ? QStringLiteral("_") : out;
}

/// Points welded: each a key on a grid of \a quantum.
struct Welder {
    double quantum = 1e-12;
    std::map<std::array<long long, 3>, int> ids;
    std::vector<Vec3> points;
    int id(const Vec3& p)
    {
        const std::array<long long, 3> k{std::llround(p.x / quantum), std::llround(p.y / quantum), std::llround(p.z / quantum)};
        const auto [it, made] = ids.emplace(k, int(points.size()));
        if (made) points.push_back(p);
        return it->second;
    }
};

double signedVolume(const std::vector<Vec3>& p, const std::array<int, 3>& t)
{
    const Vec3& a = p[std::size_t(t[0])];
    const Vec3& b = p[std::size_t(t[1])];
    const Vec3& c = p[std::size_t(t[2])];
    return a.dot(b.cross(c)) / 6;
}

/// Whether the ray from \a q along \a dir crosses triangle \a t
/// (Möller and Trumbore's), ahead of \a q.
bool rayCrosses(const Vec3& q, const Vec3& dir, const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 e1 = b - a, e2 = c - a, h = dir.cross(e2);
    const double det = e1.dot(h);
    if (std::abs(det) < 1e-300) return false;
    const double f = 1 / det;
    const Vec3 sv = q - a;
    const double u = f * sv.dot(h);
    if (u < 0 || u > 1) return false;
    const Vec3 qv = sv.cross(e1);
    const double v = f * dir.dot(qv);
    if (v < 0 || u + v > 1) return false;
    return f * e2.dot(qv) > 0;
}

/// Whether \a q is inside the solid \a tris bound (by how many of them a
/// ray from it crosses: odd inside), wound or not; \a only those of them
/// (empty: all).
bool insideOf(const Vec3& q, const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris, const std::vector<int>& only = {})
{
    // (A direction no edge of a model on axes lies along.)
    const Vec3 dir = Vec3{0.5896, 0.5557, 0.5845};
    int crossings = 0;
    const auto test = [&](const std::array<int, 3>& t) {
        crossings += rayCrosses(q, dir, points[std::size_t(t[0])], points[std::size_t(t[1])], points[std::size_t(t[2])]) ? 1 : 0;
    };
    if (only.empty())
        for (const auto& t : tris) test(t);
    else
        for (int i : only) test(tris[std::size_t(i)]);
    return crossings % 2 == 1;
}

/// \a tris wound alike (those that share an edge cross it in opposite
/// directions), each set of them that is joined turned out of the solid
/// they all bound - an outline's out, a cavity's into the cavity (its volume
/// then negative): a point just off one of its triangles along its normal
/// outside the solid. Their components are given back in \a component (by
/// triangle).
void orient(const std::vector<Vec3>& points, std::vector<std::array<int, 3>>& tris, std::vector<int>* component = nullptr)
{
    const std::size_t n = tris.size();
    std::unordered_map<long long, std::vector<std::pair<int, int>>> byEdge;   // (triangle, its corner the edge starts at)
    const auto key = [](int a, int b) { return (static_cast<long long>(std::min(a, b)) << 32) | static_cast<long long>(std::max(a, b)); };
    for (std::size_t t = 0; t < n; ++t)
        for (int k = 0; k < 3; ++k) byEdge[key(tris[t][std::size_t(k)], tris[t][std::size_t((k + 1) % 3)])].emplace_back(int(t), k);
    std::vector<int> comp(n, -1);
    std::vector<char> flip(n, 0);
    int comps = 0;
    for (std::size_t s = 0; s < n; ++s) {
        if (comp[s] >= 0) continue;
        std::vector<int> stack{int(s)};
        comp[s] = comps;
        while (!stack.empty()) {
            const int t = stack.back();
            stack.pop_back();
            for (int k = 0; k < 3; ++k) {
                const int a = tris[std::size_t(t)][std::size_t(k)], b = tris[std::size_t(t)][std::size_t((k + 1) % 3)];
                // (Its direction along it as wound now: a to b, turned if t is.)
                const bool forward = !flip[std::size_t(t)];
                for (const auto& [u, j] : byEdge[key(a, b)]) {
                    if (u == t || comp[std::size_t(u)] >= 0) continue;
                    const int ua = tris[std::size_t(u)][std::size_t(j)];
                    // u crosses it the other way: a to b along u (as stored)
                    // is b to a, wound alike with t forward.
                    const bool same = ua == a;
                    flip[std::size_t(u)] = (same == forward) ? 1 : 0;
                    comp[std::size_t(u)] = comps;
                    stack.push_back(u);
                }
            }
        }
        ++comps;
    }
    for (std::size_t t = 0; t < n; ++t)
        if (flip[t]) std::swap(tris[t][1], tris[t][2]);
    // Each component's way out: off its largest triangle along its normal,
    // inside the solid or not.
    Box3 box;
    for (const auto& t : tris)
        for (int v : t) box.add(points[std::size_t(v)]);
    const double eps = std::max(box.diagonal(), 1e-300) * 1e-7;
    std::vector<int> largest(std::size_t(comps), -1);
    std::vector<double> size(std::size_t(comps), -1.0);
    for (std::size_t t = 0; t < n; ++t) {
        const auto& tri = tris[t];
        const double a = (points[std::size_t(tri[1])] - points[std::size_t(tri[0])]).cross(points[std::size_t(tri[2])] - points[std::size_t(tri[0])]).norm();
        if (a > size[std::size_t(comp[t])]) {
            size[std::size_t(comp[t])] = a;
            largest[std::size_t(comp[t])] = int(t);
        }
    }
    std::vector<char> turn(std::size_t(comps), 0);
    for (int c = 0; c < comps; ++c) {
        const auto& tri = tris[std::size_t(largest[std::size_t(c)])];
        const Vec3 a = points[std::size_t(tri[0])], b = points[std::size_t(tri[1])], d = points[std::size_t(tri[2])];
        const Vec3 normal = (b - a).cross(d - a).normalized();
        const Vec3 off = (a + b + d) / 3 + normal * eps;
        turn[std::size_t(c)] = insideOf(off, points, tris) ? 1 : 0;
    }
    for (std::size_t t = 0; t < n; ++t)
        if (turn[std::size_t(comp[t])]) std::swap(tris[t][1], tris[t][2]);
    if (component) *component = comp;
}

/// A real as ISO 10303-21 writes one: a point always (1., 1.5, 1.E-07).
QByteArray stepReal(double v)
{
    if (!std::isfinite(v)) v = 0;
    QString s = QString::number(v, 'G', 15);
    const int e = int(s.indexOf(QLatin1Char('E')));
    const QString mantissa = e >= 0 ? s.left(e) : s, exponent = e >= 0 ? s.mid(e) : QString();
    QString m = mantissa;
    if (!m.contains(QLatin1Char('.'))) m += QLatin1Char('.');
    return (m + exponent).toLatin1();
}

QByteArray number(double v)
{
    return QByteArray::number(v, 'g', 15);
}

} // namespace

double DomainShell::volume() const
{
    double v = 0;
    for (const auto& t : triangles) v += signedVolume(points, t);
    return v;
}

std::vector<DomainShell> domainShells(const Topology3& topology, const std::vector<char>& only)
{
    std::vector<DomainShell> out;
    const double quantum = std::max(topology.bounds.diagonal(), 1e-300) * 1e-10;
    // (Each face's triangles once.)
    std::vector<std::vector<std::array<Vec3, 3>>> faceTris(topology.faces.size());
    for (int f = 0; f < int(topology.faces.size()); ++f) faceTris[std::size_t(f)] = topology.faceTriangles(f);
    for (int d = 0; d < int(topology.domains.size()); ++d) {
        if (!only.empty() && (d >= int(only.size()) || !only[std::size_t(d)])) continue;
        DomainShell sh;
        const int owner = topology.domains[std::size_t(d)].owner;
        sh.name = QStringLiteral("d%1_%2").arg(d + 1).arg(plainName(topology.objectNames.value(owner)));
        Welder w;
        w.quantum = quantum;
        for (int f = 0; f < int(topology.faces.size()); ++f) {
            const Topology3::Face& face = topology.faces[std::size_t(f)];
            if (face.back != d && face.front != d) continue;
            for (const auto& tri : faceTris[std::size_t(f)]) {
                const std::array<int, 3> t{w.id(tri[0]), w.id(tri[1]), w.id(tri[2])};
                if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
                sh.triangles.push_back(t);
                sh.faces.push_back(f);
            }
        }
        sh.points = std::move(w.points);
        orient(sh.points, sh.triangles);
        out.push_back(std::move(sh));
    }
    return out;
}

QByteArray stlOf(const std::vector<DomainShell>& shells, double unitScale, bool binary, bool mm)
{
    const double k = mm ? unitScale / 1e-3 : 1.0;
    const auto normalOf = [](const Vec3& a, const Vec3& b, const Vec3& c) { return (b - a).cross(c - a).normalized(); };
    if (binary) {
        QByteArray out;
        QDataStream s(&out, QIODevice::WriteOnly);
        s.setByteOrder(QDataStream::LittleEndian);
        s.setFloatingPointPrecision(QDataStream::SinglePrecision);
        QByteArray header("Qucs-S Multiphysics 3D geometry");
        header += mm ? " (mm)" : "";
        header.resize(80, ' ');
        s.writeRawData(header.constData(), 80);
        quint32 count = 0;
        for (const DomainShell& sh : shells) count += quint32(sh.triangles.size());
        s << count;
        for (const DomainShell& sh : shells)
            for (const auto& t : sh.triangles) {
                const Vec3 a = sh.points[std::size_t(t[0])] * k, b = sh.points[std::size_t(t[1])] * k, c = sh.points[std::size_t(t[2])] * k;
                const Vec3 n = normalOf(a, b, c);
                for (const Vec3& v : {n, a, b, c}) s << float(v.x) << float(v.y) << float(v.z);
                s << quint16(0);
            }
        return out;
    }
    QByteArray out;
    for (const DomainShell& sh : shells) {
        const QByteArray name = sh.name.toLatin1();
        out += "solid " + name + '\n';
        for (const auto& t : sh.triangles) {
            const Vec3 a = sh.points[std::size_t(t[0])] * k, b = sh.points[std::size_t(t[1])] * k, c = sh.points[std::size_t(t[2])] * k;
            const Vec3 n = normalOf(a, b, c);
            out += "  facet normal " + number(n.x) + ' ' + number(n.y) + ' ' + number(n.z) + "\n    outer loop\n";
            for (const Vec3& v : {a, b, c}) out += "      vertex " + number(v.x) + ' ' + number(v.y) + ' ' + number(v.z) + '\n';
            out += "    endloop\n  endfacet\n";
        }
        out += "endsolid " + name + '\n';
    }
    return out;
}

QByteArray objOf(const std::vector<DomainShell>& shells, const Topology3& topology)
{
    QByteArray out = "# Qucs-S Multiphysics 3D geometry: each domain an object, closed, wound out of it\n";
    out += "# unit: " + (topology.unitName.isEmpty() ? QByteArray("m * ") + number(topology.unitScale) : topology.unitName.toUtf8()) + '\n';
    int base = 1;
    for (const DomainShell& sh : shells) {
        out += "o " + sh.name.toLatin1() + '\n';
        for (const Vec3& p : sh.points) out += "v " + number(p.x) + ' ' + number(p.y) + ' ' + number(p.z) + '\n';
        for (const auto& t : sh.triangles)
            out += "f " + QByteArray::number(base + t[0]) + ' ' + QByteArray::number(base + t[1]) + ' ' + QByteArray::number(base + t[2]) + '\n';
        base += int(sh.points.size());
    }
    return out;
}

QByteArray stepOf(const std::vector<DomainShell>& shells, const Topology3& topology, const QString& name)
{
    const double k = topology.unitScale / 1e-3;   // to millimetres
    QByteArray data;
    int next = 0;
    const auto add = [&](const QByteArray& entity) {
        data += '#' + QByteArray::number(++next) + '=' + entity + ";\n";
        return next;
    };
    const auto ref = [](int id) { return '#' + QByteArray::number(id); };
    const auto list = [&](const std::vector<int>& ids) {
        QByteArray s = "(";
        for (std::size_t i = 0; i < ids.size(); ++i) s += (i ? "," : "") + ref(ids[i]);
        return s + ')';
    };
    const QByteArray product = plainName(name.isEmpty() ? QStringLiteral("geometry") : name).toLatin1();
    const int appContext = add("APPLICATION_CONTEXT('automotive design')");
    add("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000," + ref(appContext) + ')');
    const int productContext = add("PRODUCT_CONTEXT(''," + ref(appContext) + ",'mechanical')");
    const int prod = add("PRODUCT('" + product + "','" + product + "',''," + list({productContext}) + ')');
    const int formation = add("PRODUCT_DEFINITION_FORMATION('',''," + ref(prod) + ')');
    const int defContext = add("PRODUCT_DEFINITION_CONTEXT('part definition'," + ref(appContext) + ",'design')");
    const int definition = add("PRODUCT_DEFINITION('design',''," + ref(formation) + ',' + ref(defContext) + ')');
    const int shapeDef = add("PRODUCT_DEFINITION_SHAPE(''," + QByteArray("''") + ',' + ref(definition) + ')');
    add("PRODUCT_RELATED_PRODUCT_CATEGORY('part',$," + list({prod}) + ')');
    const int lengthUnit = add("(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT(.MILLI.,.METRE.))");
    const int angleUnit = add("(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.))");
    const int solidAngleUnit = add("(NAMED_UNIT(*)SI_UNIT($,.STERADIAN.)SOLID_ANGLE_UNIT())");
    const int uncertainty = add("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-07)," + ref(lengthUnit) + ",'distance_accuracy_value','confusion accuracy')");
    const int context = add("(GEOMETRIC_REPRESENTATION_CONTEXT(3)GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT(" + list({uncertainty})
                            + ")GLOBAL_UNIT_ASSIGNED_CONTEXT(" + list({lengthUnit, angleUnit, solidAngleUnit})
                            + ")REPRESENTATION_CONTEXT('Context #1','3D Context with UNIT and UNCERTAINTY'))");
    const int origin = add("CARTESIAN_POINT('',(0.,0.,0.))");
    const int zAxis = add("DIRECTION('',(0.,0.,1.))");
    const int xAxis = add("DIRECTION('',(1.,0.,0.))");
    const int placement = add("AXIS2_PLACEMENT_3D(''," + ref(origin) + ',' + ref(zAxis) + ',' + ref(xAxis) + ')');
    std::vector<int> items{placement};
    const auto point = [&](const Vec3& v) { return add("CARTESIAN_POINT('',(" + stepReal(v.x) + ',' + stepReal(v.y) + ',' + stepReal(v.z) + "))"); };
    const auto direction = [&](const Vec3& d) { return add("DIRECTION('',(" + stepReal(d.x) + ',' + stepReal(d.y) + ',' + stepReal(d.z) + "))"); };
    for (const DomainShell& sh : shells) {
        // Its points each a vertex (made where first used), each edge one
        // line between two (shared by the two faces on it, each along it
        // its own way).
        std::vector<int> vertexId(sh.points.size(), 0);
        const auto vertex = [&](int p) {
            if (vertexId[std::size_t(p)] == 0) vertexId[std::size_t(p)] = add("VERTEX_POINT(''," + ref(point(sh.points[std::size_t(p)] * k)) + ')');
            return vertexId[std::size_t(p)];
        };
        std::map<std::pair<int, int>, int> edgeId;   // (from, to) as made
        const auto orientedEdge = [&](int a, int b) {
            auto it = edgeId.find({a, b});
            bool along = true;
            if (it == edgeId.end()) {
                it = edgeId.find({b, a});
                along = false;
            }
            if (it == edgeId.end()) {
                const Vec3 pa = sh.points[std::size_t(a)] * k, pb = sh.points[std::size_t(b)] * k;
                const Vec3 d = pb - pa;
                const int line = add("LINE(''," + ref(point(pa)) + ',' + ref(add("VECTOR(''," + ref(direction(d.normalized())) + ',' + stepReal(d.norm()) + ')')) + ')');
                const int edge = add("EDGE_CURVE(''," + ref(vertex(a)) + ',' + ref(vertex(b)) + ',' + ref(line) + ",.T.)");
                it = edgeId.emplace(std::pair<int, int>{a, b}, edge).first;
                along = true;
            }
            return add("ORIENTED_EDGE('',*,*," + ref(it->second) + (along ? ",.T.)" : ",.F.)"));
        };
        const auto edgeLoop = [&](const std::vector<int>& loop) {
            std::vector<int> ids;
            for (std::size_t i = 0; i < loop.size(); ++i) ids.push_back(orientedEdge(loop[i], loop[(i + 1) % loop.size()]));
            return add("EDGE_LOOP(''," + list(ids) + ')');
        };
        // A face's plane: through \a at, its normal out of the domain.
        const auto plane = [&](const Vec3& at, const Vec3& normal) {
            const Vec3 ref0 = std::abs(normal.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            const Vec3 x = (ref0 - normal * ref0.dot(normal)).normalized();
            return add("PLANE(''," + ref(add("AXIS2_PLACEMENT_3D(''," + ref(point(at * k)) + ',' + ref(direction(normal)) + ',' + ref(direction(x)) + ')')) + ')');
        };
        std::vector<int> faces;
        // Each face of the geometry's triangles: a planar one's outlines and
        // holes (the edges of its triangles no other of them has), one
        // polygon an outline; a curved one's triangles each a facet.
        std::map<int, std::vector<int>> byFace;
        for (std::size_t t = 0; t < sh.triangles.size(); ++t) byFace[sh.faces[t]].push_back(int(t));
        // (A curved wall's triangles in planes of their own: those of one
        // plane - a strip between two of its arc's points - one face.)
        std::vector<std::vector<int>> groups;
        const double grid = std::max(topology.bounds.diagonal(), 1e-300) * 1e-9;
        for (const auto& [f, tris] : byFace) {
            const bool planar = f >= 0 && f < int(topology.faces.size()) && topology.faces[std::size_t(f)].planar;
            if (planar) {
                groups.push_back(tris);
                continue;
            }
            std::map<std::array<long long, 4>, std::vector<int>> byPlane;
            for (int t : tris) {
                const auto& tri = sh.triangles[std::size_t(t)];
                const Vec3 a = sh.points[std::size_t(tri[0])];
                const Vec3 nn = (sh.points[std::size_t(tri[1])] - a).cross(sh.points[std::size_t(tri[2])] - a).normalized();
                byPlane[{std::llround(nn.x * 1e6), std::llround(nn.y * 1e6), std::llround(nn.z * 1e6), std::llround(nn.dot(a) / grid)}].push_back(t);
            }
            for (auto& [key, g] : byPlane) groups.push_back(std::move(g));
        }
        for (const std::vector<int>& tris : groups) {
            std::multiset<std::pair<int, int>> edges;
            for (int t : tris)
                for (int j = 0; j < 3; ++j) edges.emplace(sh.triangles[std::size_t(t)][std::size_t(j)], sh.triangles[std::size_t(t)][std::size_t((j + 1) % 3)]);
            std::multimap<int, int> boundary;   // a to b
            for (const auto& e : edges)
                if (edges.find({e.second, e.first}) == edges.end()) boundary.emplace(e.first, e.second);
            std::vector<std::vector<int>> loops;
            while (!boundary.empty()) {
                auto it = boundary.begin();
                const int start = it->first;
                std::vector<int> loop{start};
                int at = it->second;
                boundary.erase(it);
                for (int guard = 0; at != start && guard < 1 << 24; ++guard) {
                    loop.push_back(at);
                    auto nx = boundary.find(at);
                    if (nx == boundary.end()) break;
                    at = nx->second;
                    boundary.erase(nx);
                }
                if (loop.size() >= 3) loops.push_back(std::move(loop));
            }
            if (loops.empty()) continue;
            // The face's normal (out of the domain: its triangles wound so),
            // each loop's area about it, the plan it is drawn in.
            const auto& t0 = sh.triangles[std::size_t(tris.front())];
            const Vec3 n = (sh.points[std::size_t(t0[1])] - sh.points[std::size_t(t0[0])]).cross(sh.points[std::size_t(t0[2])] - sh.points[std::size_t(t0[0])]).normalized();
            const auto area = [&](const std::vector<int>& loop) {
                Vec3 s;
                for (std::size_t i = 0; i < loop.size(); ++i) s = s + sh.points[std::size_t(loop[i])].cross(sh.points[std::size_t(loop[(i + 1) % loop.size()])]);
                return s.dot(n) / 2;
            };
            const int drop = std::abs(n.z) >= std::abs(n.x) && std::abs(n.z) >= std::abs(n.y) ? 2 : std::abs(n.y) >= std::abs(n.x) ? 1 : 0;
            const auto flat = [&](int p) {
                const Vec3& v = sh.points[std::size_t(p)];
                return drop == 2 ? QPointF(v.x, v.y) : drop == 1 ? QPointF(v.x, v.z) : QPointF(v.y, v.z);
            };
            const auto inside = [&](const std::vector<int>& loop, QPointF q) {
                bool in = false;
                for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
                    const QPointF a = flat(loop[i]), b = flat(loop[j]);
                    if ((a.y() > q.y()) != (b.y() > q.y()) && q.x() < (b.x() - a.x()) * (q.y() - a.y()) / (b.y() - a.y()) + a.x()) in = !in;
                }
                return in;
            };
            std::vector<int> outlines, holes;
            for (int i = 0; i < int(loops.size()); ++i) (area(loops[std::size_t(i)]) > 0 ? outlines : holes).push_back(i);
            std::map<int, std::vector<int>> holesOf;
            for (int hIdx : holes) {
                int best = -1;
                double bestArea = INFINITY;
                for (int o : outlines) {
                    const double a = area(loops[std::size_t(o)]);
                    if (a < bestArea && inside(loops[std::size_t(o)], flat(loops[std::size_t(hIdx)].front()))) {
                        best = o;
                        bestArea = a;
                    }
                }
                if (best >= 0) holesOf[best].push_back(hIdx);
            }
            for (int o : outlines) {
                std::vector<int> bounds{add("FACE_OUTER_BOUND(''," + ref(edgeLoop(loops[std::size_t(o)])) + ",.T.)")};
                for (int hIdx : holesOf[o]) bounds.push_back(add("FACE_BOUND(''," + ref(edgeLoop(loops[std::size_t(hIdx)])) + ",.T.)"));
                faces.push_back(add("ADVANCED_FACE(''," + list(bounds) + ',' + ref(plane(sh.points[std::size_t(loops[std::size_t(o)].front())], n)) + ",.T.)"));
            }
        }
        const int shell = add("CLOSED_SHELL(''," + list(faces) + ')');
        items.push_back(add("MANIFOLD_SOLID_BREP('" + sh.name.toLatin1() + "'," + ref(shell) + ')'));
    }
    const int representation = add("ADVANCED_BREP_SHAPE_REPRESENTATION(''," + list(items) + ',' + ref(context) + ')');
    add("SHAPE_DEFINITION_REPRESENTATION(" + ref(shapeDef) + ',' + ref(representation) + ')');
    QByteArray out = "ISO-10303-21;\nHEADER;\n";
    out += "FILE_DESCRIPTION(('Qucs-S Multiphysics 3D geometry: each domain a solid of planar faces'),'2;1');\n";
    out += "FILE_NAME('" + product + ".step','" + QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toLatin1() + "',(''),(''),'Qucs-S','Qucs-S','');\n";
    out += "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n";
    out += data;
    out += "ENDSEC;\nEND-ISO-10303-21;\n";
    return out;
}

// ------------------------------------------------------------------ The mesh

std::array<int, 6> orientedPrism(const Mesh3& mesh, int prism)
{
    std::array<int, 6> p = mesh.prisms[std::size_t(prism)];
    const Vec3& a = mesh.nodes[std::size_t(p[0])];
    const Vec3& b = mesh.nodes[std::size_t(p[1])];
    const Vec3& c = mesh.nodes[std::size_t(p[2])];
    const Vec3& top = mesh.nodes[std::size_t(p[3])];
    if ((b - a).cross(c - a).dot(top - a) < 0) {
        std::swap(p[1], p[2]);
        std::swap(p[4], p[5]);
    }
    return p;
}

namespace {

QString domainName(const Mesh3& mesh, int d)
{
    const Topology3* t = mesh.topology.get();
    const int owner = t && d >= 0 && d < int(t->domains.size()) ? t->domains[std::size_t(d)].owner : -1;
    return QStringLiteral("d%1_%2").arg(d + 1).arg(plainName(t ? t->objectNames.value(owner) : QString()));
}

int domainCount(const Mesh3& mesh)
{
    int n = mesh.topology ? int(mesh.topology->domains.size()) : 0;
    for (int d : mesh.domain) n = std::max(n, d + 1);
    return n;
}

int faceCount(const Mesh3& mesh)
{
    int n = mesh.topology ? int(mesh.topology->faces.size()) : 0;
    for (const Mesh3::Face& f : mesh.faces) n = std::max(n, f.face + 1);
    return n;
}

QByteArray unitComment(const Mesh3& mesh)
{
    const Topology3* t = mesh.topology.get();
    return t && !t->unitName.isEmpty() ? t->unitName.toUtf8() : QByteArray("m * ") + number(mesh.unitScale);
}

} // namespace

QByteArray mshOf(const Mesh3& mesh)
{
    const int D = domainCount(mesh), F = faceCount(mesh);
    std::vector<std::vector<int>> prismsOf(static_cast<std::size_t>(D));
    for (int e = 0; e < int(mesh.prisms.size()); ++e)
        if (mesh.domain[std::size_t(e)] >= 0) prismsOf[std::size_t(mesh.domain[std::size_t(e)])].push_back(e);
    std::vector<std::vector<int>> trisOf(static_cast<std::size_t>(F)), quadsOf(static_cast<std::size_t>(F));
    for (int i = 0; i < int(mesh.faces.size()); ++i) {
        const Mesh3::Face& f = mesh.faces[std::size_t(i)];
        if (f.face < 0) continue;
        (f.quad ? quadsOf : trisOf)[std::size_t(f.face)].push_back(i);
    }
    const auto boxOf = [&](const auto& nodeLists) {
        Box3 b;
        for (const auto& nodes : nodeLists)
            for (int n : nodes)
                if (n >= 0) b.add(mesh.nodes[std::size_t(n)]);
        return b;
    };
    QByteArray out = "$MeshFormat\n4.1 0 8\n$EndMeshFormat\n";
    out += "$Comments\nQucs-S Multiphysics 3D mesh, unit: " + unitComment(mesh) + "\n$EndComments\n";
    std::vector<int> surfaces, volumes;
    for (int f = 0; f < F; ++f)
        if (!trisOf[std::size_t(f)].empty() || !quadsOf[std::size_t(f)].empty()) surfaces.push_back(f);
    for (int d = 0; d < D; ++d)
        if (!prismsOf[std::size_t(d)].empty()) volumes.push_back(d);
    out += "$PhysicalNames\n" + QByteArray::number(qsizetype(surfaces.size() + volumes.size())) + '\n';
    for (int f : surfaces) out += "2 " + QByteArray::number(f + 1) + " \"face_" + QByteArray::number(f + 1) + "\"\n";
    for (int d : volumes) out += "3 " + QByteArray::number(d + 1) + " \"" + domainName(mesh, d).toLatin1() + "\"\n";
    out += "$EndPhysicalNames\n";
    out += "$Entities\n0 0 " + QByteArray::number(qsizetype(surfaces.size())) + ' ' + QByteArray::number(qsizetype(volumes.size())) + '\n';
    const auto boxText = [](const Box3& b) {
        return number(b.x0) + ' ' + number(b.y0) + ' ' + number(b.z0) + ' ' + number(b.x1) + ' ' + number(b.y1) + ' ' + number(b.z1);
    };
    for (int f : surfaces) {
        std::vector<std::array<int, 4>> nodes;
        for (int i : trisOf[std::size_t(f)]) nodes.push_back(mesh.faces[std::size_t(i)].nodes);
        for (int i : quadsOf[std::size_t(f)]) nodes.push_back(mesh.faces[std::size_t(i)].nodes);
        out += QByteArray::number(f + 1) + ' ' + boxText(boxOf(nodes)) + " 1 " + QByteArray::number(f + 1) + " 0\n";
    }
    for (int d : volumes) {
        std::vector<std::array<int, 6>> nodes;
        for (int e : prismsOf[std::size_t(d)]) nodes.push_back(mesh.prisms[std::size_t(e)]);
        out += QByteArray::number(d + 1) + ' ' + boxText(boxOf(nodes)) + " 1 " + QByteArray::number(d + 1) + " 0\n";
    }
    out += "$EndEntities\n";
    // (Every node in one block, on the first volume.)
    const qsizetype N = qsizetype(mesh.nodes.size());
    out += "$Nodes\n1 " + QByteArray::number(N) + " 1 " + QByteArray::number(N) + '\n';
    out += "3 " + QByteArray::number(volumes.empty() ? 1 : volumes.front() + 1) + " 0 " + QByteArray::number(N) + '\n';
    for (qsizetype i = 1; i <= N; ++i) out += QByteArray::number(i) + '\n';
    for (const Vec3& p : mesh.nodes) out += number(p.x) + ' ' + number(p.y) + ' ' + number(p.z) + '\n';
    out += "$EndNodes\n";
    int blocks = int(volumes.size()), elements = 0;
    for (int f : surfaces) {
        blocks += (trisOf[std::size_t(f)].empty() ? 0 : 1) + (quadsOf[std::size_t(f)].empty() ? 0 : 1);
        elements += int(trisOf[std::size_t(f)].size() + quadsOf[std::size_t(f)].size());
    }
    for (int d : volumes) elements += int(prismsOf[std::size_t(d)].size());
    out += "$Elements\n" + QByteArray::number(blocks) + ' ' + QByteArray::number(elements) + " 1 " + QByteArray::number(elements) + '\n';
    int id = 0;
    for (int f : surfaces)
        for (int quad = 0; quad < 2; ++quad) {
            const std::vector<int>& list = quad ? quadsOf[std::size_t(f)] : trisOf[std::size_t(f)];
            if (list.empty()) continue;
            out += "2 " + QByteArray::number(f + 1) + ' ' + (quad ? "3 " : "2 ") + QByteArray::number(qsizetype(list.size())) + '\n';
            for (int i : list) {
                out += QByteArray::number(++id);
                for (int k = 0; k < (quad ? 4 : 3); ++k) out += ' ' + QByteArray::number(mesh.faces[std::size_t(i)].nodes[std::size_t(k)] + 1);
                out += '\n';
            }
        }
    for (int d : volumes) {
        out += "3 " + QByteArray::number(d + 1) + " 6 " + QByteArray::number(qsizetype(prismsOf[std::size_t(d)].size())) + '\n';
        for (int e : prismsOf[std::size_t(d)]) {
            out += QByteArray::number(++id);
            for (int n : orientedPrism(mesh, e)) out += ' ' + QByteArray::number(n + 1);
            out += '\n';
        }
    }
    out += "$EndElements\n";
    return out;
}

QByteArray inpOf(const Mesh3& mesh)
{
    const int D = domainCount(mesh), F = faceCount(mesh);
    QByteArray out = "*HEADING\nQucs-S Multiphysics 3D mesh, unit: " + unitComment(mesh) + "\n*NODE\n";
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Vec3& p = mesh.nodes[i];
        out += QByteArray::number(qsizetype(i + 1)) + ", " + number(p.x) + ", " + number(p.y) + ", " + number(p.z) + '\n';
    }
    int id = 0;
    for (int d = 0; d < D; ++d) {
        QByteArray block;
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            if (mesh.domain[std::size_t(e)] != d) continue;
            block += QByteArray::number(++id);
            for (int n : orientedPrism(mesh, e)) block += ", " + QByteArray::number(n + 1);
            block += '\n';
        }
        if (!block.isEmpty()) out += "*ELEMENT, TYPE=C3D6, ELSET=" + domainName(mesh, d).toUpper().toLatin1() + '\n' + block;
    }
    for (int f = 0; f < F; ++f)
        for (int quad = 0; quad < 2; ++quad) {
            QByteArray block;
            for (const Mesh3::Face& face : mesh.faces) {
                if (face.face != f || face.quad != bool(quad)) continue;
                block += QByteArray::number(++id);
                for (int k = 0; k < (quad ? 4 : 3); ++k) block += ", " + QByteArray::number(face.nodes[std::size_t(k)] + 1);
                block += '\n';
            }
            if (!block.isEmpty()) out += QByteArray("*ELEMENT, TYPE=") + (quad ? "S4" : "S3") + ", ELSET=FACE_" + QByteArray::number(f + 1) + '\n' + block;
        }
    return out;
}

QByteArray bdfOf(const Mesh3& mesh)
{
    const int D = domainCount(mesh), F = faceCount(mesh);
    const auto small = [](const QByteArray& v) { return v.leftJustified(8, ' ', true); };
    const auto smallInt = [&](long long v) { return small(QByteArray::number(v)); };
    const auto large = [](double v) { return QByteArray::number(v, 'E', 8).rightJustified(16, ' ', true); };
    QByteArray out = "$ Qucs-S Multiphysics 3D mesh, unit: " + unitComment(mesh) + "\n$ Each domain's prisms a PSOLID, each face's elements a PSHELL (ids: domains from 1, faces after them)\nBEGIN BULK\n";
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Vec3& p = mesh.nodes[i];
        out += "GRID*   " + QByteArray::number(qsizetype(i + 1)).rightJustified(16, ' ') + QByteArray(16, ' ') + large(p.x) + large(p.y) + "\n*       " + large(p.z) + '\n';
    }
    int id = 0;
    for (int e = 0; e < int(mesh.prisms.size()); ++e) {
        const int d = mesh.domain[std::size_t(e)];
        if (d < 0) continue;
        out += small("CPENTA") + smallInt(++id) + smallInt(d + 1);
        for (int n : orientedPrism(mesh, e)) out += smallInt(n + 1);
        out += '\n';
    }
    for (const Mesh3::Face& face : mesh.faces) {
        if (face.face < 0) continue;
        out += small(face.quad ? "CQUAD4" : "CTRIA3") + smallInt(++id) + smallInt(D + face.face + 1);
        for (int k = 0; k < (face.quad ? 4 : 3); ++k) out += smallInt(face.nodes[std::size_t(k)] + 1);
        out += '\n';
    }
    for (int d = 0; d < D; ++d) out += "$ " + domainName(mesh, d).toLatin1() + '\n' + small("PSOLID") + smallInt(d + 1) + smallInt(d + 1) + '\n';
    for (int f = 0; f < F; ++f) out += small("PSHELL") + smallInt(D + f + 1) + smallInt(D + f + 1) + small("1.0") + '\n';
    out += "ENDDATA\n";
    return out;
}

// ------------------------------------------------------------------ Reading

TriangleSoup readTriangles(const QByteArray& data, const QString& fileName)
{
    TriangleSoup out;
    const bool obj = fileName.endsWith(QLatin1String(".obj"), Qt::CaseInsensitive);
    if (!obj && data.size() >= 84) {
        quint32 count = 0;
        std::memcpy(&count, data.constData() + 80, 4);   // (little-endian: every machine this builds on)
        if (qint64(84) + qint64(count) * 50 == data.size()) {
            QDataStream s(data);
            s.setByteOrder(QDataStream::LittleEndian);
            s.setFloatingPointPrecision(QDataStream::SinglePrecision);
            s.skipRawData(84);
            out.triangles.reserve(count);
            for (quint32 i = 0; i < count; ++i) {
                float v[12];
                for (float& x : v) s >> x;
                quint16 attr;
                s >> attr;
                out.triangles.push_back({Vec3{v[3], v[4], v[5]}, Vec3{v[6], v[7], v[8]}, Vec3{v[9], v[10], v[11]}});
            }
            return out;
        }
    }
    const QList<QByteArray> lines = data.split('\n');
    // (A group by its name: a new one at each name.)
    int group = -1;
    const auto begin = [&](const QByteArray& name) {
        out.names << QString::fromUtf8(name.trimmed());
        group = int(out.names.size()) - 1;
    };
    const auto push = [&](const std::array<Vec3, 3>& t) {
        out.triangles.push_back(t);
        out.group.push_back(group);
    };
    if (obj) {
        std::vector<Vec3> v;
        for (const QByteArray& raw : lines) {
            const QByteArray line = raw.simplified();
            if (line.startsWith("o ") || line.startsWith("g ")) begin(line.mid(2));
            else if (line.startsWith("v ")) {
                const QList<QByteArray> f = line.split(' ');
                if (f.size() >= 4) v.push_back({f[1].toDouble(), f[2].toDouble(), f[3].toDouble()});
            } else if (line.startsWith("f ")) {
                const QList<QByteArray> f = line.split(' ');
                std::vector<int> idx;
                for (int i = 1; i < f.size(); ++i) {
                    const int k = f[i].split('/').front().toInt();
                    const int at = k > 0 ? k - 1 : int(v.size()) + k;
                    if (at < 0 || at >= int(v.size())) {
                        out.error = tr("a face of the OBJ names a vertex it does not have (%1)").arg(k);
                        return out;
                    }
                    idx.push_back(at);
                }
                for (std::size_t i = 2; i < idx.size(); ++i) push({v[std::size_t(idx[0])], v[std::size_t(idx[i - 1])], v[std::size_t(idx[i])]});
            }
        }
        if (out.triangles.empty()) out.error = tr("the OBJ has no faces");
        if (out.names.isEmpty()) out.group.clear();
        return out;
    }
    std::vector<Vec3> corners;
    for (const QByteArray& raw : lines) {
        const QByteArray line = raw.simplified();
        if (line.startsWith("solid")) {
            begin(line.mid(5));
            corners.clear();
            continue;
        }
        if (!line.startsWith("vertex ")) continue;
        const QList<QByteArray> f = line.split(' ');
        if (f.size() >= 4) corners.push_back({f[1].toDouble(), f[2].toDouble(), f[3].toDouble()});
        if (corners.size() == 3) {
            push({corners[0], corners[1], corners[2]});
            corners.clear();
        }
    }
    if (out.triangles.empty()) out.error = tr("not an STL (ASCII or binary) or a Wavefront OBJ with faces");
    // (One group, or none named: no groups to keep.)
    if (out.names.size() <= 1) {
        out.group.clear();
        out.names.clear();
    }
    return out;
}

// ------------------------------------------------------------------ Slabs

namespace {

/// A loop of the plan made canonical: its points but those on a line
/// through their neighbours, from its least (x, then y).
std::vector<QPointF> canonical(const std::vector<QPointF>& loop, double tolerance)
{
    std::vector<QPointF> p;
    const std::size_t n = loop.size();
    for (std::size_t i = 0; i < n; ++i) {
        const QPointF a = loop[(i + n - 1) % n], b = loop[i], c = loop[(i + 1) % n];
        const double cross = (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x());
        const double len = std::hypot(b.x() - a.x(), b.y() - a.y()) * std::hypot(c.x() - b.x(), c.y() - b.y());
        if (std::abs(cross) > tolerance * std::max(len, 1e-300)) p.push_back(b);
    }
    if (p.empty()) return p;
    const auto least = std::min_element(p.begin(), p.end(), [](QPointF a, QPointF b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
    std::rotate(p.begin(), least, p.end());
    return p;
}

bool sameSection(const std::vector<std::vector<QPointF>>& a, const std::vector<std::vector<QPointF>>& b, double tolerance)
{
    if (a.size() != b.size()) return false;
    std::vector<std::vector<QPointF>> ca, cb;
    for (const auto& l : a) ca.push_back(canonical(l, 1e-9));
    for (const auto& l : b) cb.push_back(canonical(l, 1e-9));
    const auto order = [](const std::vector<QPointF>& x, const std::vector<QPointF>& y) {
        if (x.empty() || y.empty()) return x.size() < y.size();
        return x.front().x() != y.front().x() ? x.front().x() < y.front().x() : x.front().y() < y.front().y();
    };
    std::sort(ca.begin(), ca.end(), order);
    std::sort(cb.begin(), cb.end(), order);
    for (std::size_t i = 0; i < ca.size(); ++i) {
        if (ca[i].size() != cb[i].size()) return false;
        for (std::size_t j = 0; j < ca[i].size(); ++j)
            if (std::abs(ca[i][j].x() - cb[i][j].x()) > tolerance || std::abs(ca[i][j].y() - cb[i][j].y()) > tolerance) return false;
    }
    return true;
}

} // namespace

std::vector<SlabbedSolid> slabSolids(const std::vector<std::array<Vec3, 3>>& triangles, int slices, QStringList* warnings, const std::vector<int>& groups)
{
    std::vector<SlabbedSolid> out;
    Box3 bounds;
    for (const auto& t : triangles)
        for (const Vec3& p : t) bounds.add(p);
    if (bounds.isEmpty()) return out;
    const double diag = std::max(bounds.diagonal(), 1e-300);
    const double tol = diag * 1e-9;
    Welder w;
    w.quantum = diag * 1e-10;
    // The triangles by group, each group's solids those that share edges.
    std::map<int, std::vector<std::array<int, 3>>> byGroup;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const auto& t = triangles[i];
        const std::array<int, 3> k{w.id(t[0]), w.id(t[1]), w.id(t[2])};
        if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2]) continue;
        byGroup[i < groups.size() ? groups[i] : -1].push_back(k);
    }
    const std::vector<Vec3>& P = w.points;
    std::vector<std::pair<int, std::vector<std::array<int, 3>>>> bodies;
    for (auto& [g, tris] : byGroup) {
        std::vector<int> comp;
        orient(P, tris, &comp);
        const int n = comp.empty() ? 0 : *std::max_element(comp.begin(), comp.end()) + 1;
        // Each component an outline (its volume positive) or a cavity's
        // (negative): a cavity's with the least outline round it.
        std::vector<std::vector<int>> members(static_cast<std::size_t>(n));
        std::vector<double> volume(static_cast<std::size_t>(n), 0.0);
        for (std::size_t t = 0; t < tris.size(); ++t) {
            members[std::size_t(comp[t])].push_back(int(t));
            volume[std::size_t(comp[t])] += signedVolume(P, tris[t]);
        }
        std::vector<int> owner(static_cast<std::size_t>(n), -1);
        for (int c = 0; c < n; ++c) {
            if (volume[std::size_t(c)] >= 0) {
                owner[std::size_t(c)] = c;
                continue;
            }
            const Vec3 q = P[std::size_t(tris[std::size_t(members[std::size_t(c)].front())][0])];
            double best = INFINITY;
            for (int o = 0; o < n; ++o)
                if (volume[std::size_t(o)] > 0 && volume[std::size_t(o)] < best && insideOf(q, P, tris, members[std::size_t(o)])) {
                    best = volume[std::size_t(o)];
                    owner[std::size_t(c)] = o;
                }
        }
        for (int b = 0; b < n; ++b) {
            if (owner[std::size_t(b)] != b) continue;
            std::vector<std::array<int, 3>> mine;
            for (int c = 0; c < n; ++c)
                if (owner[std::size_t(c)] == b)
                    for (int t : members[std::size_t(c)]) mine.push_back(tris[std::size_t(t)]);
            bodies.emplace_back(g, std::move(mine));
        }
    }
    int notClosed = 0, sliced = 0;
    for (auto& [group, mine] : bodies) {
        // Closed: each edge on two of its triangles.
        {
            std::map<std::pair<int, int>, int> uses;
            for (const auto& t : mine)
                for (int j = 0; j < 3; ++j) ++uses[{std::min(t[std::size_t(j)], t[std::size_t((j + 1) % 3)]), std::max(t[std::size_t(j)], t[std::size_t((j + 1) % 3)])}];
            if (std::any_of(uses.begin(), uses.end(), [](const auto& u) { return u.second != 2; })) ++notClosed;
        }
        SlabbedSolid solid;
        solid.group = group;
        std::vector<double> zs;
        double zmin = INFINITY, zmax = -INFINITY;
        for (const auto& t : mine) {
            const Vec3 n = (P[std::size_t(t[1])] - P[std::size_t(t[0])]).cross(P[std::size_t(t[2])] - P[std::size_t(t[0])]);
            const double len = n.norm();
            const bool level = std::abs(n.z) >= (1 - 1e-9) * len, upright = std::abs(n.z) <= 1e-9 * len;
            if (!level && !upright) solid.prismatic = false;
            for (int v : t) {
                zs.push_back(P[std::size_t(v)].z);
                zmin = std::min(zmin, P[std::size_t(v)].z);
                zmax = std::max(zmax, P[std::size_t(v)].z);
            }
        }
        if (!(zmax - zmin > tol)) continue;
        std::vector<double> levels;
        if (solid.prismatic) {
            std::sort(zs.begin(), zs.end());
            for (double z : zs)
                if (levels.empty() || z - levels.back() > tol) levels.push_back(z);
        } else {
            const int n = std::max(1, slices);
            for (int i = 0; i <= n; ++i) levels.push_back(zmin + (zmax - zmin) * i / n);
            ++sliced;
        }
        for (std::size_t s = 0; s + 1 < levels.size(); ++s) {
            double zc = (levels[s] + levels[s + 1]) / 2;
            // (Off any point's height: none of its triangles' corners on the
            // cut.)
            for (int guard = 0; guard < 8; ++guard) {
                bool on = false;
                for (double z : zs) on = on || std::abs(z - zc) < tol;
                if (!on) break;
                zc += (levels[s + 1] - levels[s]) * 1e-3;
            }
            // Each triangle across the cut a segment, from edge to edge, its
            // inside on its left (seen from above); the segments joined by
            // their edges into loops.
            std::map<std::pair<int, int>, QPointF> cross;
            std::map<std::pair<int, int>, std::pair<int, int>> nextOf;   // edge to edge
            const auto at = [&](int a, int c) {
                const std::pair<int, int> e{std::min(a, c), std::max(a, c)};
                auto it = cross.find(e);
                if (it != cross.end()) return it->second;
                const Vec3& p = P[std::size_t(e.first)];
                const Vec3& q = P[std::size_t(e.second)];
                const double u = (zc - p.z) / (q.z - p.z);
                const QPointF r(p.x + u * (q.x - p.x), p.y + u * (q.y - p.y));
                cross.emplace(e, r);
                return r;
            };
            for (const auto& t : mine) {
                std::vector<std::pair<int, int>> edges;
                for (int j = 0; j < 3; ++j) {
                    const int a = t[std::size_t(j)], c = t[std::size_t((j + 1) % 3)];
                    if ((P[std::size_t(a)].z - zc) * (P[std::size_t(c)].z - zc) < 0) edges.emplace_back(std::min(a, c), std::max(a, c));
                }
                if (edges.size() != 2) continue;
                const Vec3 n = (P[std::size_t(t[1])] - P[std::size_t(t[0])]).cross(P[std::size_t(t[2])] - P[std::size_t(t[0])]);
                const QPointF p0 = at(edges[0].first, edges[0].second), p1 = at(edges[1].first, edges[1].second);
                // (Out of the solid is to its right: d = (-ny, nx).)
                const bool forward = (p1.x() - p0.x()) * -n.y + (p1.y() - p0.y()) * n.x >= 0;
                if (forward) nextOf[edges[0]] = edges[1];
                else nextOf[edges[1]] = edges[0];
            }
            SlabbedSolid::Slab slab;
            slab.z0 = levels[s];
            slab.z1 = levels[s + 1];
            while (!nextOf.empty()) {
                const std::pair<int, int> start = nextOf.begin()->first;
                std::vector<QPointF> loop;
                std::pair<int, int> e = start;
                for (int guard = 0; guard < 1 << 24; ++guard) {
                    loop.push_back(cross[e]);
                    auto it = nextOf.find(e);
                    if (it == nextOf.end()) break;
                    const std::pair<int, int> nx = it->second;
                    nextOf.erase(it);
                    e = nx;
                    if (e == start) break;
                }
                if (loop.size() >= 3) slab.loops.push_back(std::move(loop));
            }
            if (slab.loops.empty()) continue;
            // (A prism's slab alike the one under it: that one taller.)
            if (solid.prismatic && !solid.slabs.empty() && std::abs(solid.slabs.back().z1 - slab.z0) < tol
                && sameSection(solid.slabs.back().loops, slab.loops, tol * 10)) {
                solid.slabs.back().z1 = slab.z1;
                continue;
            }
            solid.slabs.push_back(std::move(slab));
        }
        if (!solid.slabs.empty()) out.push_back(std::move(solid));
    }
    if (warnings) {
        if (notClosed > 0) *warnings << tr("%n of its solids not closed (an edge not on two triangles): their sections may be wrong", nullptr, notClosed);
        if (sliced > 0)
            *warnings << tr("%n of its solids not prismatic (a face neither level nor upright): each cut into %1 slices of its height, its "
                            "section at each one's middle",
                            nullptr, sliced)
                             .arg(std::max(1, slices));
    }
    return out;
}

} // namespace qucs_s::fem
