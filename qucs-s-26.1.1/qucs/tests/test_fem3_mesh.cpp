/*
 * test_fem3_mesh.cpp - a 3D multiphysics model's swept mesh: each slab's
 *                      divisions in z (by size, graded from thin layers, by
 *                      count), prisms through the slabs their domains fill,
 *                      a node a plan node at a height, the boundary faces on
 *                      the geometry's, turned out of their domains and closed
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_mesh.h"

#include <QJsonDocument>
#include <QElapsedTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace qucs_s::fem;

namespace {

constexpr double Inf = std::numeric_limits<double>::infinity();

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

Model model3d(double hmax = 0)
{
    Model m = Model::blank3d();
    if (hmax > 0) {
        m.mesh().props.insert(QStringLiteral("custom"), true);
        m.mesh().props.insert(QStringLiteral("hmax"), QString::number(hmax, 'g', 17));
    }
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

struct Built {
    std::shared_ptr<Topology3> topology;
    Mesh3Build mesh;
};

Built meshOf(const Model& m)
{
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    Built b;
    const GeometryBuild3 g = buildGeometry3(m, *ps);
    b.topology = g.topology;
    if (g.ok()) b.mesh = buildMesh3(m, *ps, g.topology);
    return b;
}

/// A mesh's prisms where its plan's triangles are, at its heights; its
/// faces turned from their face's back into its front, each domain's closed;
/// its volume each domain's. By band: each band's prisms its plan's, through
/// its heights only, no interface at a level.
void checkMesh(const Mesh3& m)
{
    const Topology3& t = *m.topology;
    const double scale = std::max(1.0, t.bounds.diagonal());
    QVERIFY(!m.bands.empty());
    QCOMPARE(m.nodes.size(), m.nodeOrigin.size());
    QCOMPARE(m.nodes.size(), m.nodeBand.size());
    QCOMPARE(m.prisms.size(), m.prismBand.size());
    for (std::size_t b = 0; b + 1 < m.bands.size(); ++b) {
        QCOMPARE(m.bands[b].last, m.bands[b + 1].first);
        QVERIFY2(std::find(m.levelHeight.begin(), m.levelHeight.end(), m.bands[b].last) == m.levelHeight.end(), "an interface at a level");
    }
    QCOMPARE(m.bands.front().first, 0);
    QCOMPARE(m.bands.back().last, int(m.heights.size()) - 1);
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        const auto [n, j] = m.nodeOrigin[i];
        const int b = m.nodeBand[i];
        const Mesh& plan = *m.bands[std::size_t(b)].plan;
        QCOMPARE(m.node(b, n, j), int(i));
        QCOMPARE(m.nodes[i].x, plan.nodes[std::size_t(n)].x());
        QCOMPARE(m.nodes[i].y, plan.nodes[std::size_t(n)].y());
        QCOMPARE(m.nodes[i].z, m.heights[std::size_t(j)]);
    }
    std::vector<double> volume(t.domains.size(), 0);
    for (std::size_t p = 0; p < m.prisms.size(); ++p) {
        const Mesh3::Band& band = m.bands[std::size_t(m.prismBand[p])];
        QVERIFY(m.prismLayer[p] >= band.first && m.prismLayer[p] < band.last);
        const std::array<int, 3>& tri = band.plan->triangles[std::size_t(m.prismTriangle[p])];
        for (int c = 0; c < 3; ++c) {
            QVERIFY(m.prisms[p][std::size_t(c)] >= 0 && m.prisms[p][std::size_t(c) + 3] >= 0);
            // (Its band's node there - at its interface with the band below,
            // where that band's plan has a node at the same place, that one.)
            for (int up = 0; up < 2; ++up) {
                const int node = m.prisms[p][std::size_t(c + 3 * up)], j = m.prismLayer[p] + up;
                QCOMPARE(m.node(m.prismBand[p], tri[std::size_t(c)], j), node);
                const QPointF q = band.plan->nodes[std::size_t(tri[std::size_t(c)])];
                QCOMPARE(m.nodes[std::size_t(node)].x, q.x());
                QCOMPARE(m.nodes[std::size_t(node)].y, q.y());
                QCOMPARE(m.nodes[std::size_t(node)].z, m.heights[std::size_t(j)]);
                if (m.nodeBand[std::size_t(node)] != m.prismBand[p]) {
                    QVERIFY2(j == band.first && m.nodeBand[std::size_t(node)] == m.prismBand[p] - 1, "a node another band's only at its interface below");
                } else {
                    QCOMPARE(m.nodeOrigin[std::size_t(node)], (std::pair<int, int>(tri[std::size_t(c)], j)));
                }
            }
        }
        const Vec3 a = m.nodes[std::size_t(m.prisms[p][0])], b = m.nodes[std::size_t(m.prisms[p][1])], c = m.nodes[std::size_t(m.prisms[p][2])];
        const double area = ((b - a).cross(c - a)).z / 2;
        QVERIFY2(area > 0, "a prism's bottom is counter-clockwise seen from above");
        volume[std::size_t(m.domain[p])] += area * (m.nodes[std::size_t(m.prisms[p][3])].z - a.z);
    }
    // Each domain's volume its cells' - each band's plan domains' triangles
    // times its layers' heights in the slabs the domain fills - exactly; the
    // geometry's within the polygon a curved wall's plan nodes make (a P1
    // mesh's; on straight walls exact).
    std::vector<int> slabOf(m.heights.size(), 0);
    for (std::size_t k = 0; k + 1 < m.levelHeight.size(); ++k)
        for (int j = m.levelHeight[k]; j < m.levelHeight[k + 1]; ++j) slabOf[std::size_t(j)] = int(k);
    std::vector<double> cells(t.domains.size(), 0);
    for (const Mesh3::Band& band : m.bands) {
        std::vector<double> planArea(t.plan->domains.size(), 0);
        for (std::size_t i = 0; i < band.plan->triangles.size(); ++i) {
            const std::array<int, 3>& tri = band.plan->triangles[i];
            const QPointF a = band.plan->nodes[std::size_t(tri[0])], b = band.plan->nodes[std::size_t(tri[1])], c = band.plan->nodes[std::size_t(tri[2])];
            planArea[std::size_t(band.plan->domain[i])] += ((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x())) / 2;
        }
        for (std::size_t pd = 0; pd < planArea.size(); ++pd)
            for (int j = band.first; j < band.last; ++j) {
                const int d = t.cell(int(pd), slabOf[std::size_t(j)]);
                if (d >= 0) cells[std::size_t(d)] += planArea[pd] * (m.heights[std::size_t(j) + 1] - m.heights[std::size_t(j)]);
            }
    }
    double total = 0;
    for (std::size_t d = 0; d < t.domains.size(); ++d) {
        total += volume[d];
        QVERIFY2(std::abs(volume[d] - cells[d]) <= 1e-9 * scale * scale * scale,
                 qPrintable(QStringLiteral("domain %1: prisms %2, its cells %3").arg(d + 1).arg(volume[d]).arg(cells[d])));
        bool curved = false;
        for (const Topology3::Face& f : t.faces) curved = curved || (!f.planar && (f.back == int(d) || f.front == int(d)));
        QVERIFY2(std::abs(volume[d] - t.domains[d].volume) <= (curved ? 5e-2 : 1e-9) * t.domains[d].volume,
                 qPrintable(QStringLiteral("domain %1: prisms %2, its volume %3").arg(d + 1).arg(volume[d]).arg(t.domains[d].volume)));
    }
    QVERIFY(std::abs(total - m.volume) <= 1e-9 * scale * scale * scale);
    std::vector<Vec3> flux(t.domains.size());
    for (const Mesh3::Face& f : m.faces) {
        QVERIFY(f.face >= 0 && f.face < int(t.faces.size()));
        const int n = f.quad ? 4 : 3;
        Vec3 area;
        for (int i = 0; i < n; ++i) QVERIFY(f.nodes[std::size_t(i)] >= 0);
        for (int i = 1; i + 1 < n; ++i)
            area = area + (m.nodes[std::size_t(f.nodes[std::size_t(i)])] - m.nodes[std::size_t(f.nodes[0])])
                              .cross(m.nodes[std::size_t(f.nodes[std::size_t(i) + 1])] - m.nodes[std::size_t(f.nodes[0])]) * 0.5;
        const Topology3::Face& face = t.faces[std::size_t(f.face)];
        if (face.planar) QVERIFY2(area.normalized().dot(face.normal) > 1 - 1e-9, "a mesh face turned");
        if (face.back >= 0) flux[std::size_t(face.back)] = flux[std::size_t(face.back)] + area;
        if (face.front >= 0) flux[std::size_t(face.front)] = flux[std::size_t(face.front)] - area;
    }
    for (std::size_t d = 0; d < t.domains.size(); ++d)
        QVERIFY2(flux[d].norm() <= 1e-9 * scale * scale, qPrintable(QStringLiteral("domain %1's faces are not closed").arg(d + 1)));
    QCOMPARE(m.pointNode.size(), t.points.size());
    for (std::size_t p = 0; p < t.points.size(); ++p) {
        QVERIFY(m.pointNode[p] >= 0);
        QVERIFY((m.nodes[std::size_t(m.pointNode[p])] - t.points[p].position).norm() <= 1e-9 * scale);
    }
}

} // namespace

class TestFem3Mesh : public QObject
{
    Q_OBJECT

private slots:
    /// Divisions in z: by size, by count, graded from a thin slab and from a
    /// level asked fine.
    void divisions()
    {
        auto d = sweptDivisions({0, 10}, {1}, {0}, {Inf, Inf}, 1.3);
        QCOMPARE(int(d[0].size()), 9);
        for (int i = 0; i < 9; ++i) QVERIFY(std::abs(d[0][std::size_t(i)] - (i + 1)) < 1e-9);
        d = sweptDivisions({0, 2}, {Inf}, {4}, {Inf, Inf}, 1.3);
        QCOMPARE(int(d[0].size()), 3);
        QVERIFY(std::abs(d[0][1] - 1) < 1e-9);
        // A count over the size: two, though the size asks twenty.
        d = sweptDivisions({0, 2}, {0.1}, {2}, {Inf, Inf}, 1.3);
        QCOMPARE(int(d[0].size()), 1);
        // A thin slab under a thick one: the thick one's elements from about
        // the thin one's height, each at most the rate times the last.
        d = sweptDivisions({0, 1, 101}, {Inf, Inf}, {0, 0}, {Inf, Inf, Inf}, 1.3);
        QVERIFY(d[0].empty());
        const std::vector<double>& thick = d[1];
        QVERIFY2(thick.size() >= 9 && thick.size() <= 14, qPrintable(QString::number(thick.size())));
        std::vector<double> z{1};
        z.insert(z.end(), thick.begin(), thick.end());
        z.push_back(101);
        QVERIFY(z[1] - z[0] < 1.5);
        for (std::size_t i = 2; i < z.size(); ++i) {
            QVERIFY(z[i] > z[i - 1]);
            QVERIFY2((z[i] - z[i - 1]) <= 1.45 * (z[i - 1] - z[i - 2]) + 1e-9, qPrintable(QStringLiteral("division %1").arg(i)));
        }
        // A level asked fine: graded from it.
        d = sweptDivisions({0, 10}, {10}, {0}, {0.1, Inf}, 1.5);
        QVERIFY(d[0].front() < 0.2);
        QVERIFY(d[0].size() >= 6);
    }

    /// A box: its plan's triangles swept through its divisions.
    void aBox()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("blk"), 0, 0, 0, 2, 3, 1);
        const Built b = meshOf(m);
        QVERIFY2(b.mesh.mesh, qPrintable(b.mesh.error));
        const Mesh3& mesh = *b.mesh.mesh;
        QCOMPARE(mesh.heights, (std::vector<double>{0, 0.5, 1}));
        QCOMPARE(mesh.levelHeight, (std::vector<int>{0, 2}));
        QCOMPARE(mesh.prisms.size(), 2 * mesh.plan->triangles.size());
        QCOMPARE(mesh.nodes.size(), 3 * mesh.plan->nodes.size());
        QVERIFY(std::abs(mesh.volume - 6) < 1e-9);
        checkMesh(mesh);
        // The floors' triangles twice the plan's; the walls' quadrilaterals
        // the plan's boundary edges times the divisions.
        int floors = 0, walls = 0;
        for (const Mesh3::Face& f : mesh.faces) (f.quad ? walls : floors) += 1;
        QCOMPARE(floors, 2 * int(mesh.plan->triangles.size()));
        QCOMPARE(walls, 2 * int(mesh.plan->edges.size()));
        QVERIFY(mesh.maxStretch >= 1 && mesh.meanStretch >= 1);
    }

    /// Stacked boxes, a count across the upper one; a thin layer graded from.
    void counts()
    {
        Model m = model3d(1);
        addBox(m, QStringLiteral("lower"), 0, 0, 0, 2, 2, 2);
        addBox(m, QStringLiteral("upper"), 0, 0, 2, 2, 2, 3);
        Node zd = m.make(QStringLiteral("mesh/zdistribution"));
        zd.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("objects"), QJsonArray{QStringLiteral("upper")}}});
        zd.props.insert(QStringLiteral("count"), 6);
        m.add(m.mesh().tag, zd);
        Built b = meshOf(m);
        QVERIFY2(b.mesh.mesh, qPrintable(b.mesh.error));
        const Mesh3& mesh = *b.mesh.mesh;
        // Six across the upper one; the lower one graded to its half unit.
        QCOMPARE(mesh.levelHeight[2] - mesh.levelHeight[1], 6);
        QVERIFY(mesh.levelHeight[1] >= 2);
        QVERIFY(mesh.heights[std::size_t(mesh.levelHeight[1])] - mesh.heights[std::size_t(mesh.levelHeight[1]) - 1] < 0.75);
        checkMesh(mesh);
        // A thin film between two thick blocks: one division across it, its
        // neighbours' first ones about as tall.
        Model f = model3d(2);
        addBox(f, QStringLiteral("below"), 0, 0, 0, 4, 4, 10);
        addBox(f, QStringLiteral("film"), 0, 0, 10, 4, 4, 0.05);
        addBox(f, QStringLiteral("above"), 0, 0, 10.05, 4, 4, 10);
        b = meshOf(f);
        QVERIFY2(b.mesh.mesh, qPrintable(b.mesh.error));
        const Mesh3& fm = *b.mesh.mesh;
        const int film = fm.levelHeight[1];
        QCOMPARE(fm.levelHeight[2], film + 1);
        QVERIFY(fm.heights[std::size_t(film)] - fm.heights[std::size_t(film) - 1] < 0.1);
        QVERIFY(fm.heights[std::size_t(film) + 2] - fm.heights[std::size_t(film) + 1] < 0.1);
        checkMesh(fm);
        // A count across a domain that spans three slabs: shared among them
        // by their thickness.
        Model span = model3d(10);
        addBox(span, QStringLiteral("tall"), 0, 0, 0, 2, 2, 4);
        addBox(span, QStringLiteral("beside"), 3, 0, 2, 2, 2, 1);
        Node across = span.make(QStringLiteral("mesh/zdistribution"));
        across.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("tall")}}});
        across.props.insert(QStringLiteral("count"), 8);
        span.add(span.mesh().tag, across);
        b = meshOf(span);
        QVERIFY2(b.mesh.mesh, qPrintable(b.mesh.error));
        QCOMPARE(b.mesh.mesh->levelHeight, (std::vector<int>{0, 4, 6, 8}));
        checkMesh(*b.mesh.mesh);
    }

    /// A box in a slab of oxide: the plan refined where the box is, through
    /// every slab (the swept price); a size on a face, on a domain.
    void sizes()
    {
        Model m = model3d(2);
        addBox(m, QStringLiteral("oxide"), 0, 0, 0, 10, 10, 4);
        addBox(m, QStringLiteral("heater"), 4, 4, 1, 2, 2, 1);
        Built coarse = meshOf(m);
        QVERIFY2(coarse.mesh.mesh, qPrintable(coarse.mesh.error));
        checkMesh(*coarse.mesh.mesh);
        Node size = m.make(QStringLiteral("mesh/size"));
        size.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("heater")}}});
        size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.25"));
        m.add(m.mesh().tag, size);
        Built fine = meshOf(m);
        QVERIFY2(fine.mesh.mesh, qPrintable(fine.mesh.error));
        const Mesh3& mesh = *fine.mesh.mesh;
        checkMesh(mesh);
        QVERIFY(mesh.plan->triangles.size() > 2 * coarse.mesh.mesh->plan->triangles.size());
        // The heater's slab divided to 0.25: four across its 1.
        QCOMPARE(mesh.levelHeight[2] - mesh.levelHeight[1], 4);
        // Every prism's triangle in a plan domain the cell's.
        for (std::size_t p = 0; p < mesh.prisms.size(); ++p) {
            const int pd = mesh.plan->domain[std::size_t(mesh.prismTriangle[p])];
            const int k = fine.topology->slabAt((mesh.heights[std::size_t(mesh.prismLayer[p])] + mesh.heights[std::size_t(mesh.prismLayer[p]) + 1]) / 2);
            QCOMPARE(fine.topology->cell(pd, k), mesh.domain[p]);
        }
    }

    /// By band (the mesh's Plan): two small blocks asked fine (0.25), one
    /// near the floor of a tall box of air and one near its top, and a
    /// cylinder through all its height. Bands, none of whose interfaces is a
    /// level; a band of air between them coarse, the blocks' footprints
    /// merged there; the cylinder's wall one surface through them (its
    /// domains closed); every prism's domain the cell's; fewer prisms than
    /// one plan for every layer.
    void aMeshByBand()
    {
        Model m = model3d(4);
        addBox(m, QStringLiteral("air"), -10, -10, 0, 20, 20, 40);
        addBox(m, QStringLiteral("low"), -6, -1, 2, 2, 2, 1);
        addBox(m, QStringLiteral("high"), 4, -1, 36, 2, 2, 1);
        Node c = m.make(QStringLiteral("cylinder"));
        c.label = QStringLiteral("pillar");
        c.props.insert(QStringLiteral("center"), pairOf(0, 6));
        c.props.insert(QStringLiteral("radius"), QStringLiteral("2"));
        c.props.insert(QStringLiteral("z"), QStringLiteral("0"));
        c.props.insert(QStringLiteral("height"), QStringLiteral("40"));
        m.add(m.geometry().tag, c);
        Node size = m.make(QStringLiteral("mesh/size"));
        size.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("low"), QStringLiteral("high")}}});
        size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.25"));
        m.add(m.mesh().tag, size);
        const Built one = meshOf(m);
        QVERIFY2(one.mesh.mesh, qPrintable(one.mesh.error));
        QVERIFY(!one.mesh.mesh->byBand());
        m.mesh().props.insert(QStringLiteral("plan"), QStringLiteral("band"));
        const Built banded = meshOf(m);
        QVERIFY2(banded.mesh.mesh, qPrintable(banded.mesh.error));
        const Mesh3& mesh = *banded.mesh.mesh;
        checkMesh(mesh);
        QString said = QStringLiteral("by band: %1 prisms against %2 (%3 bands:").arg(mesh.prisms.size()).arg(one.mesh.mesh->prisms.size()).arg(mesh.bands.size());
        for (const Mesh3::Band& b : mesh.bands)
            said += QStringLiteral(" z %1 to %2, %3 triangles%4;")
                        .arg(mesh.heights[std::size_t(b.first)])
                        .arg(mesh.heights[std::size_t(b.last)])
                        .arg(b.plan->triangles.size())
                        .arg(b.merged.empty() ? QString() : QStringLiteral(" merged"));
        qInfo("%s", qPrintable(said + QLatin1Char(')')));
        QVERIFY2(mesh.bands.size() >= 3, qPrintable(said));
        QVERIFY2(mesh.prisms.size() < 0.7 * one.mesh.mesh->prisms.size(), qPrintable(said));
        // Its plans as good as one plan's: each band's smallest angle 20° or
        // more, its worst stretch near one plan's (the union of every band's
        // nodes on a wall made 0.004°, the stretch 267 for 32).
        QString quality = QStringLiteral("smallest angles:");
        for (const Mesh3::Band& b : mesh.bands) quality += QStringLiteral(" %1°").arg(b.plan->minAngle, 0, 'f', 1);
        quality += QStringLiteral("; worst stretch %1 against one plan's %2").arg(mesh.maxStretch).arg(one.mesh.mesh->maxStretch);
        qInfo("%s", qPrintable(quality));
        for (const Mesh3::Band& b : mesh.bands) QVERIFY2(b.plan->minAngle >= 20, qPrintable(quality));
        QVERIFY2(mesh.maxStretch <= 1.5 * one.mesh.mesh->maxStretch, qPrintable(quality));
        // The band at mid-height: coarse, its blocks' footprints merged.
        const Mesh3::Band* middle = nullptr;
        for (const Mesh3::Band& b : mesh.bands)
            if (mesh.heights[std::size_t(b.first)] <= 20 && mesh.heights[std::size_t(b.last)] >= 20) middle = &b;
        QVERIFY(middle);
        QVERIFY2(!middle->merged.empty() && middle->plan->triangles.size() < one.mesh.mesh->plan->triangles.size() / 2, qPrintable(said));
        // The pillar - whole in every band - the same triangles in each; at
        // each interface its nodes one, the finer side named.
        QCOMPARE(int(mesh.interfaceFiner.size()), int(mesh.bands.size()) - 1);
        int pillar = -1;
        for (std::size_t pd = 0; pd < banded.topology->plan->domains.size(); ++pd)
            if (banded.topology->cell(int(pd), 0) >= 0 && banded.topology->domains[std::size_t(banded.topology->cell(int(pd), 0))].owner
                                                             == int(banded.topology->objectNames.indexOf(QStringLiteral("pillar"))))
                pillar = int(pd);
        QVERIFY(pillar >= 0);
        const auto trianglesOf = [pillar](const Mesh& plan) {
            std::vector<std::array<double, 6>> out;
            for (std::size_t i = 0; i < plan.triangles.size(); ++i) {
                if (plan.domain[i] != pillar) continue;
                std::array<double, 6> tri{};
                for (int c = 0; c < 3; ++c) {
                    tri[std::size_t(2 * c)] = plan.nodes[std::size_t(plan.triangles[i][std::size_t(c)])].x();
                    tri[std::size_t(2 * c + 1)] = plan.nodes[std::size_t(plan.triangles[i][std::size_t(c)])].y();
                }
                out.push_back(tri);
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto first = trianglesOf(*mesh.bands.front().plan);
        QVERIFY(!first.empty());
        for (const Mesh3::Band& b : mesh.bands) QVERIFY2(trianglesOf(*b.plan) == first, "the pillar's triangles the same in every band");
        for (std::size_t i = 0; i + 1 < mesh.bands.size(); ++i) {
            const int f = mesh.interfaceFiner[i];
            QVERIFY(f == int(i) || f == int(i) + 1);
            // (The pillar's nodes at the interface the band below's.)
            const Mesh3::Band& up = mesh.bands[i + 1];
            for (std::size_t t3 = 0; t3 < up.plan->triangles.size(); ++t3) {
                if (up.plan->domain[t3] != pillar) continue;
                for (int n : up.plan->triangles[t3]) {
                    const int node = mesh.node(int(i) + 1, n, up.first);
                    QVERIFY2(node >= 0 && mesh.nodeBand[std::size_t(node)] == int(i), "a shared node at an interface not one");
                }
            }
        }
        // Every prism's domain the cell's of its triangle's (merged) plan
        // domain and its slab.
        for (std::size_t p = 0; p < mesh.prisms.size(); ++p) {
            const int pd = mesh.planOf(int(p)).domain[std::size_t(mesh.prismTriangle[p])];
            const int k = banded.topology->slabAt((mesh.heights[std::size_t(mesh.prismLayer[p])] + mesh.heights[std::size_t(mesh.prismLayer[p]) + 1]) / 2);
            QCOMPARE(banded.topology->cell(pd, k), mesh.domain[p]);
        }
    }

    /// By band, slabs of one layer stacked - eight full-width 1 mm slabs,
    /// a fine block above them: an interface moved off the levels one way
    /// only (before, two levels in a row bounced it for ever, the mesh never
    /// made).
    void stackedSlabsByBand()
    {
        Model m = model3d(4);
        m.mesh().props.insert(QStringLiteral("plan"), QStringLiteral("band"));
        addBox(m, QStringLiteral("air"), -10, -10, 0, 20, 20, 40);
        for (int i = 0; i < 8; ++i) addBox(m, QStringLiteral("slab%1").arg(i), -10, -10, 20 + i, 20, 20, 1);
        addBox(m, QStringLiteral("block"), -1, -1, 36, 2, 2, 1);
        Node size = m.make(QStringLiteral("mesh/size"));
        size.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("block")}}});
        size.props.insert(QStringLiteral("hmax"), QStringLiteral("0.25"));
        m.add(m.mesh().tag, size);
        QElapsedTimer clock;
        clock.start();
        const Built b = meshOf(m);
        QVERIFY2(b.mesh.mesh, qPrintable(b.mesh.error));
        QVERIFY2(clock.elapsed() < 30000, qPrintable(QStringLiteral("%1 ms").arg(clock.elapsed())));
        checkMesh(*b.mesh.mesh);
    }

    /// A cylinder over a box: curved walls swept; a hole where a box is cut.
    void curves()
    {
        Model m = model3d(0.3);
        addBox(m, QStringLiteral("plate"), -2, -2, 0, 4, 4, 1);
        Node c = m.make(QStringLiteral("cylinder"));
        c.label = QStringLiteral("rod");
        c.props.insert(QStringLiteral("radius"), QStringLiteral("0.75"));
        c.props.insert(QStringLiteral("z"), QStringLiteral("1"));
        c.props.insert(QStringLiteral("height"), QStringLiteral("2"));
        m.add(m.geometry().tag, c);
        const Built b = meshOf(m);
        QVERIFY2(b.mesh.mesh, qPrintable(b.mesh.error));
        checkMesh(*b.mesh.mesh);
        // No prism over the plate beside the rod.
        for (std::size_t p = 0; p < b.mesh.mesh->prisms.size(); ++p) {
            const Vec3 a = b.mesh.mesh->nodes[std::size_t(b.mesh.mesh->prisms[p][0])];
            if (a.z >= 1) QVERIFY(std::hypot(a.x, a.y) <= 0.75 + 1e-9);
        }
    }

    /// A slab's divisions by a size far smaller than it clamped: 1e-9
    /// across 1 asked for a million, and smaller sizes past an int's range.
    void divisionsClamped()
    {
        const double inf = std::numeric_limits<double>::infinity();
        for (double h : {1e-9, 1e-15, 1e-300}) {
            const std::vector<std::vector<double>> d = sweptDivisions({0, 1}, {h}, {0}, {inf, inf}, 1.2);
            QCOMPARE(int(d.size()), 1);
            QCOMPARE(int(d[0].size()), MaxSweptDivisions - 1);
            QVERIFY(std::is_sorted(d[0].begin(), d[0].end()) && d[0].front() > 0 && d[0].back() < 1);
        }
    }

    /// A mesh of more prisms than MaxPrisms refused before it is made,
    /// said: a size far smaller than the geometry (1e-9 for a photodiode in
    /// mm) ran the mesh's thread out of memory, uncaught, and Qucs-S
    /// aborted.
    void hugeMeshesRefused()
    {
        Model m = model3d(0.02);
        addBox(m, QStringLiteral("blk"), 0, 0, 0, 1, 1, 1);
        Node dist = m.make(QStringLiteral("mesh/zdistribution"));
        dist.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
        dist.props.insert(QStringLiteral("count"), 100000);
        m.add(m.mesh().tag, dist);
        QElapsedTimer t;
        t.start();
        const Built b = meshOf(m);
        QVERIFY(!b.mesh.mesh);
        QVERIFY2(b.mesh.error.contains(QStringLiteral("prisms")) && b.mesh.error.contains(QStringLiteral("a larger size")), qPrintable(b.mesh.error));
        QVERIFY2(t.elapsed() < 60000, "refused before it is made");
    }

    /// What it refuses and says.
    void refusals()
    {
        Model m = model3d();
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        Mesh3Build none = buildMesh3(m, *ps, nullptr);
        QVERIFY(!none.mesh && none.error.contains(QStringLiteral("build it first")));
        addBox(m, QStringLiteral("blk"), 0, 0, 0, 1, 1, 1);
        // A 2D mesh's feature in a 3D file: left, said.
        Node dist;
        dist.type = QStringLiteral("mesh/distribution");
        dist.tag = QStringLiteral("dis1");
        m.mesh().children.push_back(dist);
        const Built b = meshOf(m);
        QVERIFY2(b.mesh.mesh, qPrintable(b.mesh.error));
        QVERIFY2(b.mesh.warnings.join(QStringLiteral("; ")).contains(QStringLiteral("not a 3D mesh's")), qPrintable(b.mesh.warnings.join(QStringLiteral("; "))));
    }
};

QTEST_GUILESS_MAIN(TestFem3Mesh)
#include "test_fem3_mesh.moc"
