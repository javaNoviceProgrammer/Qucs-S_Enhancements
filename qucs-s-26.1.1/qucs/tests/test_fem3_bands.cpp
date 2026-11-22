/*
 * test_fem3_bands.cpp - a 3D mesh by band of layers' interfaces joined
 *                       (multiphysics/fem3_bands.h): the two plans' overlay
 *                       covering each triangle, the constraints exact on
 *                       what both bands' spaces hold - a uniform B's
 *                       potential across an interface among them -, the
 *                       gradients kept (the interpolations commuting with
 *                       ∇), the reduction's masters never constrained
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

#include <QJsonDocument>
#include <QtTest>

#include <cmath>
#include <map>
#include <random>

using namespace qucs_s::fem;

namespace {

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
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

/// A box of air 10 × 10 × 20 (mm) meshed to 2 by band, two blocks 1 × 1 × 1
/// in it asked to 0.2 - one near its floor at (\a x1, \a y1), one near its
/// top at (\a x2, \a y2): bands between them.
std::shared_ptr<const Mesh3> bandedMesh(double x1, double y1, double x2, double y2)
{
    Model m = Model::blank3d();
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), QStringLiteral("2"));
    m.mesh().props.insert(QStringLiteral("plan"), QStringLiteral("band"));
    addBox(m, QStringLiteral("air"), -5, -5, 0, 10, 10, 20);
    addBox(m, QStringLiteral("low"), x1, y1, 2, 1, 1, 1);
    addBox(m, QStringLiteral("high"), x2, y2, 16, 1, 1, 1);
    Node size = m.make(QStringLiteral("mesh/size"));
    size.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("low"), QStringLiteral("high")}}});
    size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.2"));
    m.add(m.mesh().tag, size);
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    const GeometryBuild3 g = buildGeometry3(m, *ps);
    if (!g.ok()) return nullptr;
    return buildMesh3(m, *ps, g.topology).mesh;
}

/// Each interface's height and its two bands (finer, coarser).
struct Side {
    int height, fine, coarse;
};
std::vector<Side> sides(const Mesh3& m)
{
    std::vector<Side> out;
    for (std::size_t i = 0; i + 1 < m.bands.size(); ++i) {
        const int f = m.interfaceFiner[i];
        out.push_back({m.bands[i].last, f, f == int(i) ? int(i) + 1 : int(i)});
    }
    return out;
}

/// Each constrained row's mismatch on \a v: |v_dof - Σ w v_master|, the
/// largest, against v's largest.
double mismatch(const Constraints3& c, const std::vector<double>& v)
{
    double worst = 0, scale = 1e-300;
    for (double x : v) scale = std::max(scale, std::abs(x));
    for (std::size_t k = 0; k < c.dofs.size(); ++k) {
        double s = 0;
        for (const auto& [d, w] : c.rows[k]) s += w * v[std::size_t(d)];
        worst = std::max(worst, std::abs(v[std::size_t(c.dofs[k])] - s));
    }
    return worst / scale;
}

/// The plan triangles of band \a band with a prism beside height \a height.
std::vector<int> trianglesBeside(const Mesh3& m, int band, int height)
{
    const int layer = height == m.bands[std::size_t(band)].first ? height : height - 1;
    std::vector<int> out;
    for (std::size_t e = 0; e < m.prisms.size(); ++e)
        if (m.prismBand[e] == band && m.prismLayer[e] == layer) out.push_back(m.prismTriangle[e]);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

class TestFem3Bands : public QObject
{
    Q_OBJECT

private slots:
    /// Every fine triangle beside an interface covered by the other band's
    /// triangles exactly (its overlay's pieces' areas its own, to rounding):
    /// on five meshes of blocks put at random.
    void overlayCovers()
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> at(-4.5, 3.5);
        for (int trial = 0; trial < 5; ++trial) {
            const auto mesh = bandedMesh(at(rng), at(rng), at(rng), at(rng));
            QVERIFY(mesh && mesh->byBand());
            int checked = 0;
            for (const Side& s : sides(*mesh)) {
                const PlanLocator coarse(*mesh->bands[std::size_t(s.coarse)].plan, mesh->unitScale);
                const PlanLocator fine(*mesh->bands[std::size_t(s.fine)].plan, mesh->unitScale);
                for (int t : trianglesBeside(*mesh, s.fine, s.height)) {
                    const Corners& c = fine.corners(t);
                    const double area = std::abs((c[1].x() - c[0].x()) * (c[2].y() - c[0].y()) - (c[1].y() - c[0].y()) * (c[2].x() - c[0].x())) / 2;
                    const double covered = overlayArea(coarse, c);
                    QVERIFY2(std::abs(covered - area) <= 1e-9 * area,
                             qPrintable(QStringLiteral("trial %1, a triangle of %2 m² covered %3").arg(trial).arg(area).arg(covered)));
                    ++checked;
                }
            }
            QVERIFY(checked > 100);
        }
    }

    /// A nodal space's constraints exact on what both bands hold: a linear
    /// function (order 1), a quadratic (order 2) - each band's interpolant
    /// at an interface, the finer's rows the coarser's combinations.
    void nodalExact()
    {
        const auto mesh = bandedMesh(-4, -1, 2, 1);
        QVERIFY(mesh && mesh->byBand());
        for (int order : {1, 2}) {
            const Space3 space(mesh, order);
            const Constraints3 c = nodalConstraints(space);
            QVERIFY(!c.empty());
            const auto f = [order](QPointF p) {
                const double x = p.x() * 1e3, y = p.y() * 1e3;
                return order == 1 ? 1 + 2 * x - 3 * y : 1 + 2 * x - 3 * y + 0.5 * x * x - 0.7 * x * y + 0.3 * y * y;
            };
            std::vector<double> v(std::size_t(space.size()), 0.0);
            for (const Side& s : sides(*mesh)) {
                interpolateNodes(space, s.coarse, s.height, f, v);
                interpolateNodes(space, s.fine, s.height, f, v);
            }
            const double off = mismatch(c, v);
            QVERIFY2(off < 1e-10, qPrintable(QStringLiteral("order %1: %2 constrained, off by %3").arg(order).arg(c.dofs.size()).arg(off)));
        }
    }

    /// An edge space's constraints exact on what both bands hold: the
    /// potential of a uniform B across the interface, A = ½ B × r, and a
    /// constant (order 1, Whitney's); any linear field (order 2).
    void edgeExact()
    {
        const auto mesh = bandedMesh(-4, -1, 2, 1);
        QVERIFY(mesh && mesh->byBand());
        for (int order : {1, 2}) {
            const EdgeSpace3 space(mesh, order);
            const Constraints3 c = edgeConstraints(space);
            QVERIFY(!c.empty());
            const auto field = [order](QPointF p, double out[2]) {
                const double x = p.x() * 1e3, y = p.y() * 1e3, B = 1.7;
                out[0] = 0.3 - 0.5 * B * y;
                out[1] = -0.2 + 0.5 * B * x;
                if (order == 2) {
                    out[0] += 0.4 * x + 0.9 * y;
                    out[1] += -0.6 * x + 0.25 * y;
                }
            };
            std::vector<double> v(std::size_t(space.size()), 0.0);
            for (const Side& s : sides(*mesh)) {
                interpolateEdges(space, s.coarse, s.height, field, v);
                interpolateEdges(space, s.fine, s.height, field, v);
            }
            const double off = mismatch(c, v);
            QVERIFY2(off < 1e-10, qPrintable(QStringLiteral("order %1: %2 constrained, off by %3").arg(order).arg(c.dofs.size()).arg(off)));
        }
    }

    /// The gradients kept: a coarse nodal function at random, its gradient's
    /// edge coefficients through the edge constraints the fine side's
    /// gradient of the nodal constraints' function - orders 1 and 2.
    void commuting()
    {
        const auto mesh = bandedMesh(-4, -1, 2, 1);
        QVERIFY(mesh && mesh->byBand());
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> u(-1, 1);
        for (int order : {1, 2}) {
            const Space3 nodes(mesh, order);
            const EdgeSpace3 edges(mesh, order);
            const Constraints3 cn = nodalConstraints(nodes), ce = edgeConstraints(edges);
            std::vector<double> phi(std::size_t(nodes.size()));
            for (double& x : phi) x = u(rng);
            // (The fine side's values the constraints' of the coarse ones.)
            for (std::size_t k = 0; k < cn.dofs.size(); ++k) {
                double s = 0;
                for (const auto& [d, w] : cn.rows[k]) s += w * phi[std::size_t(d)];
                phi[std::size_t(cn.dofs[k])] = s;
            }
            // A band's nodal function's gradient at a point of its plan.
            std::vector<PlanLocator> locators;
            for (const Mesh3::Band& b : mesh->bands) locators.emplace_back(*b.plan, mesh->unitScale);
            const auto gradientOf = [&](int band, int height) {
                return [&, band, height](QPointF p, double out[2]) {
                    const PlanLocator& at = locators[std::size_t(band)];
                    double xi = 0, eta = 0;
                    const int t = at.locate(p, &xi, &eta);
                    out[0] = out[1] = 0;
                    if (t < 0) return;
                    const Space& plan = nodes.plan(band);
                    const Space::Map m = plan.map(t, xi, eta);
                    Shape sh;
                    shapeAt(order, xi, eta, sh);
                    const int* d2 = plan.dofs(t);
                    for (int k = 0; k < sh.count; ++k) {
                        const int dof = nodes.dofAt(band, d2[k], order == 2 ? 2 * height : height);
                        if (dof < 0) continue;
                        out[0] += phi[std::size_t(dof)] * (m.invT[0][0] * sh.dxi[k] + m.invT[0][1] * sh.deta[k]);
                        out[1] += phi[std::size_t(dof)] * (m.invT[1][0] * sh.dxi[k] + m.invT[1][1] * sh.deta[k]);
                    }
                };
            };
            std::vector<double> coarse(std::size_t(edges.size()), 0.0), fine(std::size_t(edges.size()), 0.0);
            for (const Side& s : sides(*mesh)) {
                interpolateEdges(edges, s.coarse, s.height, gradientOf(s.coarse, s.height), coarse);
                interpolateEdges(edges, s.fine, s.height, gradientOf(s.fine, s.height), fine);
            }
            double worst = 0, scale = 1e-300;
            for (std::size_t k = 0; k < ce.dofs.size(); ++k) {
                double s = 0;
                for (const auto& [d, w] : ce.rows[k]) s += w * coarse[std::size_t(d)];
                worst = std::max(worst, std::abs(fine[std::size_t(ce.dofs[k])] - s));
                scale = std::max(scale, std::abs(fine[std::size_t(ce.dofs[k])]));
            }
            QVERIFY2(worst <= 1e-9 * scale, qPrintable(QStringLiteral("order %1: the gradients off by %2 of %3").arg(order).arg(worst).arg(scale)));
        }
    }

    /// A cylinder through every band - whole in each, the same triangles -:
    /// there the constrained side's trace the other's exactly (C the
    /// identity, up to each entity's basis: two plans orient an edge each its
    /// own way), for a field at random - orders 1 and 2.
    void sharedIdentity()
    {
        Model m = Model::blank3d();
        m.mesh().props.insert(QStringLiteral("custom"), true);
        m.mesh().props.insert(QStringLiteral("hmax"), QStringLiteral("2"));
        m.mesh().props.insert(QStringLiteral("plan"), QStringLiteral("band"));
        addBox(m, QStringLiteral("air"), -5, -5, 0, 10, 10, 20);
        addBox(m, QStringLiteral("low"), -4, -1, 2, 1, 1, 1);
        Node c = m.make(QStringLiteral("cylinder"));
        c.label = QStringLiteral("pillar");
        c.props.insert(QStringLiteral("center"), pairOf(2, 2));
        c.props.insert(QStringLiteral("radius"), QStringLiteral("1.5"));
        c.props.insert(QStringLiteral("z"), QStringLiteral("0"));
        c.props.insert(QStringLiteral("height"), QStringLiteral("20"));
        m.add(m.geometry().tag, c);
        Node size = m.make(QStringLiteral("mesh/size"));
        size.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("low")}}});
        size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.2"));
        m.add(m.mesh().tag, size);
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        const GeometryBuild3 g = buildGeometry3(m, *ps);
        QVERIFY(g.ok());
        const auto mesh = buildMesh3(m, *ps, g.topology).mesh;
        QVERIFY(mesh && mesh->byBand());
        int pillar = -1;
        const int owner = int(g.topology->objectNames.indexOf(QStringLiteral("pillar")));
        for (std::size_t pd = 0; pd < g.topology->plan->domains.size(); ++pd)
            if (g.topology->cell(int(pd), 0) >= 0 && g.topology->domains[std::size_t(g.topology->cell(int(pd), 0))].owner == owner) pillar = int(pd);
        QVERIFY(pillar >= 0);
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> u(-1, 1);
        std::vector<PlanLocator> at;
        for (const Mesh3::Band& b : mesh->bands) at.emplace_back(*b.plan, mesh->unitScale);
        for (int order : {1, 2}) {
            const EdgeSpace3 edges(mesh, order);
            const Space3 nodes(mesh, order);
            const Constraints3 ce = edgeConstraints(edges), cn = nodalConstraints(nodes);
            // A field at random, the constrained ones the constraints'.
            std::vector<double> ve(std::size_t(edges.size())), vn(std::size_t(nodes.size()));
            for (double& x : ve) x = u(rng);
            for (double& x : vn) x = u(rng);
            for (std::size_t k = 0; k < ce.dofs.size(); ++k) {
                double s = 0;
                for (const auto& [d, w] : ce.rows[k]) s += w * ve[std::size_t(d)];
                ve[std::size_t(ce.dofs[k])] = s;
            }
            for (std::size_t k = 0; k < cn.dofs.size(); ++k) {
                double s = 0;
                for (const auto& [d, w] : cn.rows[k]) s += w * vn[std::size_t(d)];
                vn[std::size_t(cn.dofs[k])] = s;
            }
            // A band's traces at a point (E across z, and the nodal one).
            const auto edgeTrace = [&](int band, int height, QPointF p, double out[2]) {
                double xi = 0, eta = 0;
                const int t = at[std::size_t(band)].locate(p, &xi, &eta);
                out[0] = out[1] = 0;
                QVERIFY(t >= 0);
                const AffineMap am = affineMap(edges.planNodes(band), t);
                EdgeSpace::Basis b;
                edges.planEdges(band).basis(t, am, xi, eta, b);
                const int* ed = edges.planEdges(band).dofs(t);
                for (int k = 0; k < b.count; ++k) {
                    const int dof = edges.acrossDof(band, ed[k], edges.levelOfHeight(height));
                    if (dof < 0) continue;
                    out[0] += ve[std::size_t(dof)] * b.vx[k];
                    out[1] += ve[std::size_t(dof)] * b.vy[k];
                }
            };
            const auto nodalTrace = [&](int band, int height, QPointF p) {
                double xi = 0, eta = 0;
                const int t = at[std::size_t(band)].locate(p, &xi, &eta);
                if (t < 0) return std::nan("");
                Shape sh;
                shapeAt(order, xi, eta, sh);
                const int* d2 = nodes.plan(band).dofs(t);
                double v = 0;
                for (int k = 0; k < sh.count; ++k) v += vn[std::size_t(nodes.dofAt(band, d2[k], order == 2 ? 2 * height : height))] * sh.n[k];
                return v;
            };
            int checked = 0;
            double worst = 0;
            for (const Side& s : sides(*mesh)) {
                const PlanLocator& fine = at[std::size_t(s.fine)];
                const Mesh& plan = *mesh->bands[std::size_t(s.fine)].plan;
                for (int t : trianglesBeside(*mesh, s.fine, s.height)) {
                    if (plan.domain[std::size_t(t)] != pillar) continue;
                    const Corners& c = fine.corners(t);
                    for (const auto& q : triangleRule(4).points) {
                        // (Inside, off the sides: a point of the rule.)
                        const QPointF p = c[0] + (c[1] - c[0]) * q[0] + (c[2] - c[0]) * q[1];
                        double ef[2], ec[2];
                        edgeTrace(s.fine, s.height, p, ef);
                        edgeTrace(s.coarse, s.height, p, ec);
                        worst = std::max({worst, std::abs(ef[0] - ec[0]), std::abs(ef[1] - ec[1])});
                        if (order == 1)   // (a straight triangle: order 2's maps on the arc curved)
                            worst = std::max(worst, std::abs(nodalTrace(s.fine, s.height, p) - nodalTrace(s.coarse, s.height, p)));
                        ++checked;
                    }
                }
            }
            QVERIFY(checked > 10);
            QVERIFY2(worst < 1e-8, qPrintable(QStringLiteral("order %1: in the pillar the two sides' traces differ by %2").arg(order).arg(worst)));
        }
    }

    /// The reduction: as many columns as degrees of freedom not constrained;
    /// no master constrained; a constrained one's row its constraint's.
    void reduction()
    {
        const auto mesh = bandedMesh(-4, -1, 2, 1);
        QVERIFY(mesh);
        const EdgeSpace3 space(mesh, 2);
        const Constraints3 c = edgeConstraints(space);
        std::vector<int> reduced;
        const Eigen::SparseMatrix<double> C = reductionMatrix(space.size(), c, &reduced);
        QCOMPARE(int(C.rows()), space.size());
        QCOMPARE(int(C.cols()), space.size() - int(c.dofs.size()));
        for (std::size_t k = 0; k < c.dofs.size(); ++k) {
            QCOMPARE(reduced[std::size_t(c.dofs[k])], -1);
            for (const auto& [d, w] : c.rows[k]) {
                QVERIFY2(reduced[std::size_t(d)] >= 0, "a master constrained");
                // (The finer band of an interface constrained, the coarser
                // its masters.)
                const int fine = space.band(c.dofs[k]), coarse = space.band(d);
                QVERIFY(std::abs(fine - coarse) == 1);
                QVERIFY2(mesh->interfaceFiner[std::size_t(std::min(fine, coarse))] == fine, "the coarser side constrained");
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestFem3Bands)
#include "test_fem3_bands.moc"
