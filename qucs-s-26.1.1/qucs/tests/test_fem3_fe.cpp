/*
 * test_fem3_fe.cpp - the 3D finite elements (multiphysics/fem3_fe.h):
 *                    Lagrange prisms of order 1 and 2 - their shape
 *                    functions, a node's own each, summing to one, their
 *                    derivatives -, the prism's rules exact to their degree,
 *                    a swept mesh's space (order 1 its nodes, order 2's
 *                    shared between prisms that meet), its maps, and its
 *                    faces' points (a curved wall's area order 2's)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_fe.h"

#include <QJsonDocument>
#include <QtTest>

#include <cmath>
#include <map>
#include <set>

using namespace qucs_s::fem;

namespace {

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

Model model3d(double hmax)
{
    Model m = Model::blank3d();
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QString::number(hmax, 'g', 17));
    return m;
}

void addBox(Model& m, const QString& label, double x, double y, double z, double w, double d, double h)
{
    Node n = m.make(QStringLiteral("box"));
    n.label = label;
    n.props.insert(QStringLiteral("position"), pairOf(x, y));
    n.props.insert(QStringLiteral("size"), pairOf(w, d));
    n.props.insert(QStringLiteral("z"), QString::number(z, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
}

void addCylinder(Model& m, const QString& label, double x, double y, double z, double r, double h)
{
    Node n = m.make(QStringLiteral("cylinder"));
    n.label = label;
    n.props.insert(QStringLiteral("center"), pairOf(x, y));
    n.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
    n.props.insert(QStringLiteral("z"), QString::number(z, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
}

std::shared_ptr<const Mesh3> meshOf(const Model& m)
{
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    const GeometryBuild3 g = buildGeometry3(m, *ps);
    if (!g.ok()) return nullptr;
    return buildMesh3(m, *ps, g.topology).mesh;
}

/// ∫ ξ^a η^b ζ^c over the reference prism: a! b! / (a + b + 2)! / (c + 1).
double monomial(int a, int b, int c)
{
    auto fact = [](int n) {
        double f = 1;
        for (int i = 2; i <= n; ++i) f *= i;
        return f;
    };
    return fact(a) * fact(b) / fact(a + b + 2) / (c + 1);
}

} // namespace

class TestFem3Fe : public QObject
{
    Q_OBJECT

private slots:
    // Each shape function one at its own node and nought at the others; all
    // of them one together, their derivatives nought together, and each
    // derivative its function's slope (a centred difference).
    void prismShapes()
    {
        for (int order : {1, 2}) {
            // The nodes: the triangle's (corners, then edge middles) at each
            // level.
            std::vector<std::array<double, 3>> nodes;
            const double tri[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0.5, 0}, {0.5, 0.5}, {0, 0.5}};
            const int nt = order == 1 ? 3 : 6;
            const std::vector<double> levels = order == 1 ? std::vector<double>{0, 1} : std::vector<double>{0, 0.5, 1};
            for (double z : levels)
                for (int a = 0; a < nt; ++a) nodes.push_back({tri[a][0], tri[a][1], z});
            PrismShape s;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                prismShapeAt(order, nodes[i][0], nodes[i][1], nodes[i][2], s);
                QCOMPARE(s.count, int(nodes.size()));
                for (int k = 0; k < s.count; ++k) QVERIFY2(std::abs(s.n[k] - (std::size_t(k) == i ? 1.0 : 0.0)) < 1e-14, qPrintable(QString::number(k)));
            }
            const double pts[3][3] = {{0.2, 0.3, 0.1}, {0.6, 0.1, 0.7}, {0.05, 0.85, 0.5}};
            for (const auto& p : pts) {
                prismShapeAt(order, p[0], p[1], p[2], s);
                double sum = 0, dx = 0, dy = 0, dz = 0;
                for (int k = 0; k < s.count; ++k) {
                    sum += s.n[k];
                    dx += s.dxi[k];
                    dy += s.deta[k];
                    dz += s.dzeta[k];
                }
                QVERIFY(std::abs(sum - 1) < 1e-14);
                QVERIFY(std::abs(dx) < 1e-13 && std::abs(dy) < 1e-13 && std::abs(dz) < 1e-13);
                const double h = 1e-6;
                PrismShape a, b;
                for (int axis = 0; axis < 3; ++axis) {
                    double q[3] = {p[0], p[1], p[2]}, r[3] = {p[0], p[1], p[2]};
                    q[axis] += h;
                    r[axis] -= h;
                    prismShapeAt(order, q[0], q[1], q[2], a);
                    prismShapeAt(order, r[0], r[1], r[2], b);
                    for (int k = 0; k < s.count; ++k) {
                        const double fd = (a.n[k] - b.n[k]) / (2 * h);
                        const double d = axis == 0 ? s.dxi[k] : axis == 1 ? s.deta[k] : s.dzeta[k];
                        QVERIFY2(std::abs(fd - d) < 1e-7, qPrintable(QStringLiteral("order %1, function %2, axis %3").arg(order).arg(k).arg(axis)));
                    }
                }
            }
        }
    }

    // A rule of degree d times n Gauss points: exact for ξ^a η^b with a + b
    // <= d and ζ^c with c <= 2n - 1; its weights the prism's volume.
    void prismRules()
    {
        for (int d = 1; d <= 5; ++d)
            for (int n = 1; n <= 3; ++n) {
                const PrismRule& r = prismRule(d, n);
                double volume = 0;
                for (const auto& q : r.points) volume += q[3];
                QVERIFY(std::abs(volume - 0.5) < 1e-14);
                for (int a = 0; a <= d; ++a)
                    for (int b = 0; a + b <= d; ++b)
                        for (int c = 0; c <= 2 * n - 1; ++c) {
                            double sum = 0;
                            for (const auto& q : r.points) sum += q[3] * std::pow(q[0], a) * std::pow(q[1], b) * std::pow(q[2], c);
                            QVERIFY2(std::abs(sum - monomial(a, b, c)) < 1e-13,
                                     qPrintable(QStringLiteral("degree %1, %2 points: x^%3 y^%4 z^%5").arg(d).arg(n).arg(a).arg(b).arg(c)));
                        }
            }
    }

    // Order 1's degrees of freedom are the mesh's nodes; order 2's the plan's
    // nodes and edges' middles at every level a box's prisms fill, each
    // where its plan dof is at its level's z, shared by the prisms that meet
    // there - none twice in a prism, every prism's at the points its own
    // shape functions' nodes are.
    void spaces()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("blk"), 0, 0, 0, 2, 1, 1);
        const std::shared_ptr<const Mesh3> mesh = meshOf(m);
        QVERIFY(mesh && !mesh->prisms.empty());
        const Space3 p1(mesh, 1);
        QCOMPARE(p1.size(), int(mesh->nodes.size()));
        for (int e = 0; e < p1.elements(); ++e)
            for (int k = 0; k < 6; ++k) QCOMPARE(p1.dofs(e)[k], mesh->prisms[std::size_t(e)][std::size_t(k)]);
        const Space3 p2(mesh, 2);
        const int planNodes = int(mesh->plan->nodes.size());
        int planEdges = 0;
        {
            std::set<std::pair<int, int>> edges;
            for (const auto& t : mesh->plan->triangles)
                for (int k = 0; k < 3; ++k) edges.insert({std::min(t[std::size_t(k)], t[std::size_t((k + 1) % 3)]), std::max(t[std::size_t(k)], t[std::size_t((k + 1) % 3)])});
            planEdges = int(edges.size());
        }
        const int levels = 2 * int(mesh->heights.size()) - 1;
        QCOMPARE(p2.size(), (planNodes + planEdges) * levels);
        const double s = mesh->unitScale;
        for (int e = 0; e < p2.elements(); ++e) {
            const int* d = p2.dofs(e);
            std::set<int> seen(d, d + 18);
            QCOMPARE(int(seen.size()), 18);
            // Each dof where the prism's map puts its node.
            const double tri[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0.5, 0}, {0.5, 0.5}, {0, 0.5}};
            for (int lz = 0; lz < 3; ++lz)
                for (int a = 0; a < 6; ++a) {
                    const Vec3 x = p2.map(e, tri[a][0], tri[a][1], 0.5 * lz).x;
                    const Vec3 y = p2.point(d[a + 6 * lz]);
                    QVERIFY2((x - y).norm() < 1e-12 * s, qPrintable(QStringLiteral("prism %1, node %2").arg(e).arg(a + 6 * lz)));
                }
        }
        // The map's Jacobian: the triangle's area times the layer's height.
        double volume = 0;
        for (int e = 0; e < p1.elements(); ++e) volume += 0.5 * std::abs(p1.map(e, 1.0 / 3, 1.0 / 3, 0.5).det);
        QVERIFY(std::abs(volume - 2 * 1 * 1 * s * s * s) < 1e-12 * s * s * s);
    }

    // A face's points: its area by their weights - a box's six faces, a
    // cylinder's wall 2πrh within order 2's error of an arc (order 1 the
    // polygon's) -, each point on its face, beside a prism of its own.
    void facePoints()
    {
        Model m = model3d(0.25);
        addBox(m, QStringLiteral("blk"), 0, 0, 0, 2, 1, 1);
        const std::shared_ptr<const Mesh3> mesh = meshOf(m);
        QVERIFY(mesh);
        const double s = mesh->unitScale;
        for (int order : {1, 2}) {
            const Space3 sp(mesh, order);
            std::map<int, double> area;
            for (int f = 0; f < int(mesh->faces.size()); ++f) {
                const std::vector<int> d = sp.faceDofs(f);
                QCOMPARE(int(d.size()), mesh->faces[std::size_t(f)].quad ? (order == 1 ? 4 : 9) : (order == 1 ? 3 : 6));
                for (int k : d) QVERIFY(k >= 0);
                // (Its order 1 dofs its own nodes; its points between them.)
                const Mesh3::Face& mf = mesh->faces[std::size_t(f)];
                const int corners = mf.quad ? 4 : 3;
                if (order == 1) {
                    std::set<int> own(mf.nodes.begin(), mf.nodes.begin() + corners), dofs(d.begin(), d.end());
                    QCOMPARE(dofs, own);
                }
                double zlo = 1e300, zhi = -1e300;
                for (int c = 0; c < corners; ++c) {
                    zlo = std::min(zlo, mesh->nodes[std::size_t(mf.nodes[std::size_t(c)])].z * s);
                    zhi = std::max(zhi, mesh->nodes[std::size_t(mf.nodes[std::size_t(c)])].z * s);
                }
                for (const Space3::FacePoint& p : sp.facePoints(f, 2 * order)) {
                    QVERIFY2(p.x.z >= zlo - 1e-15 && p.x.z <= zhi + 1e-15, qPrintable(QStringLiteral("face %1: z %2 not in [%3, %4]").arg(f).arg(p.x.z).arg(zlo).arg(zhi)));
                    area[mesh->faces[std::size_t(f)].face] += p.weight;
                    double sum = 0;
                    for (std::size_t k = 0; k < d.size(); ++k) sum += p.n[k];
                    QVERIFY(std::abs(sum - 1) < 1e-13);
                    QVERIFY(p.prism >= 0);
                    // (The prism's map there is the face's point.)
                    QVERIFY((sp.map(p.prism, p.xi, p.eta, p.zeta).x - p.x).norm() < 1e-12 * s);
                }
            }
            QCOMPARE(int(area.size()), 6);
            double total = 0;
            for (const auto& [face, a] : area) total += a;
            QVERIFY2(std::abs(total - 2 * (2 * 1 + 2 * 1 + 1 * 1) * s * s) < 1e-12 * s * s, qPrintable(QString::number(total / (s * s))));
        }
        Model c = model3d(0.3);
        addCylinder(c, QStringLiteral("cyl"), 0, 0, 0, 1, 1);
        const std::shared_ptr<const Mesh3> cm = meshOf(c);
        QVERIFY(cm);
        double wall[3] = {0, 0, 0};
        for (int order : {1, 2}) {
            const Space3 sp(cm, order);
            for (int f = 0; f < int(cm->faces.size()); ++f)
                if (cm->faces[std::size_t(f)].quad)
                    for (const Space3::FacePoint& p : sp.facePoints(f, 4)) wall[order] += p.weight;
        }
        const double exact = 2 * M_PI * 1 * 1 * s * s;
        QVERIFY2(std::abs(wall[2] - exact) < 1e-4 * exact, qPrintable(QStringLiteral("order 2: %1 of %2").arg(wall[2]).arg(exact)));
        QVERIFY2(std::abs(wall[1] - exact) > 10 * std::abs(wall[2] - exact), "order 1's polygon short of the arc");
    }
};

QTEST_GUILESS_MAIN(TestFem3Fe)
#include "test_fem3_fe.moc"
