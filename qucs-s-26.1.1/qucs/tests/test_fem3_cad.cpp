/*
 * test_fem3_cad.cpp - a 3D model's geometry and mesh to other tools'
 *                     formats and solids read from theirs (multiphysics/
 *                     fem3_cad.cpp): each domain a closed shell of its
 *                     volume, STL and OBJ read back, a STEP file whose every
 *                     reference is defined, the mesh's MSH, INP and BDF of
 *                     its nodes and elements, prismatic solids imported
 *                     exactly and others sliced, and what is refused
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_cad.h"
#include "fem3_geometry.h"
#include "fem3_mesh.h"
#include "fem_model.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <map>

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

void addCylinder(Model& m, const QString& label, double x, double y, double r, double z, double h)
{
    Node n = m.make(QStringLiteral("cylinder"));
    n.label = label;
    n.props.insert(QStringLiteral("center"), pairOf(x, y));
    n.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
    n.props.insert(QStringLiteral("z"), QString::number(z, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    m.add(m.geometry().tag, n);
}

/// A CAD Import of \a file (beside the model, \a dir), its unit the model's.
void addImport(Model& m, const QString& label, const QString& file, int slices = 10)
{
    Node n = m.make(QStringLiteral("solidimport"));
    n.label = label;
    n.props.insert(QStringLiteral("file"), file);
    n.props.insert(QStringLiteral("unit"), QStringLiteral("model"));
    n.props.insert(QStringLiteral("slices"), slices);
    m.add(m.geometry().tag, n);
}

struct Built {
    GeometryBuild3 g;
    std::shared_ptr<const Mesh3> mesh;
    QString error;
};

Built build(const Model& m, bool mesh = false)
{
    Built out;
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    out.g = buildGeometry3(m, *ps);
    if (!out.g.ok()) {
        out.error = out.g.errors.value(out.g.failedAt);
        return out;
    }
    if (mesh) {
        const Mesh3Build mb = buildMesh3(m, *ps, out.g.topology);
        if (!mb.mesh) out.error = mb.error;
        out.mesh = mb.mesh;
    }
    return out;
}

QString warningsOf(const GeometryBuild3& g)
{
    return QStringList(g.warnings.values()).join(QStringLiteral("; "));
}

double totalVolume(const Topology3& t)
{
    double v = 0;
    for (const auto& d : t.domains) v += d.volume;
    return v;
}

/// Triangles of a closed prism: polygon \a outline (counter-clockwise) and
/// \a holes (each clockwise) from \a z0 to \a z1, each face fanned about a
/// point - a test's own STL, not the exporter's.
std::vector<std::array<Vec3, 3>> prismOf(const std::vector<QPointF>& outline, double z0, double z1, int bands = 1)
{
    std::vector<std::array<Vec3, 3>> t;
    const std::size_t n = outline.size();
    // (A convex-from-its-first-corner fan is enough for the shapes used.)
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const QPointF a = outline[0], b = outline[i], c = outline[i + 1];
        t.push_back({Vec3{a.x(), a.y(), z1}, Vec3{b.x(), b.y(), z1}, Vec3{c.x(), c.y(), z1}});
        t.push_back({Vec3{a.x(), a.y(), z0}, Vec3{c.x(), c.y(), z0}, Vec3{b.x(), b.y(), z0}});
    }
    // (Its walls in \a bands, a seam of points between each: heights where
    // its section does not change.)
    for (int k = 0; k < bands; ++k) {
        const double lo = z0 + (z1 - z0) * k / bands, hi = z0 + (z1 - z0) * (k + 1) / bands;
        for (std::size_t i = 0; i < n; ++i) {
            const QPointF a = outline[i], b = outline[(i + 1) % n];
            t.push_back({Vec3{a.x(), a.y(), lo}, Vec3{b.x(), b.y(), lo}, Vec3{b.x(), b.y(), hi}});
            t.push_back({Vec3{a.x(), a.y(), lo}, Vec3{b.x(), b.y(), hi}, Vec3{a.x(), a.y(), hi}});
        }
    }
    return t;
}

/// The triangles as an ASCII STL.
QByteArray stlText(const std::vector<std::array<Vec3, 3>>& tris)
{
    QByteArray out = "solid test\n";
    for (const auto& t : tris) {
        out += "facet normal 0 0 0\nouter loop\n";
        for (const Vec3& p : t) out += "vertex " + QByteArray::number(p.x, 'g', 17) + ' ' + QByteArray::number(p.y, 'g', 17) + ' ' + QByteArray::number(p.z, 'g', 17) + '\n';
        out += "endloop\nendfacet\n";
    }
    return out + "endsolid test\n";
}

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

/// Where a test keeps what it writes (QUCS_CAD_KEEP: a folder of them, to be
/// read by other tools).
void keep(const QString& name, const QByteArray& data)
{
    const QString dir = qEnvironmentVariable("QUCS_CAD_KEEP");
    if (dir.isEmpty()) return;
    QFile f(QDir(dir).absoluteFilePath(name));
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

} // namespace

class TestFem3Cad : public QObject
{
    Q_OBJECT

private slots:
    // A box with a cylinder in part of it, and a box on top: each domain's
    // shell closed - each edge on two of its triangles, crossed the other way
    // - and of its domain's volume; the lid's exactly.
    void shellsAreClosedAndHoldTheirVolumes()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("block"), 0, 0, 0, 4, 3, 2);
        addCylinder(m, QStringLiteral("rod"), 2, 1.5, 0.8, 0.5, 1);
        addBox(m, QStringLiteral("lid"), 0, 0, 2, 4, 3, 0.5);
        const Built b = build(m);
        QVERIFY2(b.error.isEmpty(), qPrintable(b.error));
        const Topology3& t = *b.g.topology;
        const std::vector<DomainShell> shells = domainShells(t);
        QCOMPARE(int(shells.size()), int(t.domains.size()));
        for (std::size_t d = 0; d < shells.size(); ++d) {
            const DomainShell& sh = shells[d];
            std::map<std::pair<int, int>, int> directed;
            for (const auto& tri : sh.triangles)
                for (int j = 0; j < 3; ++j) ++directed[{tri[std::size_t(j)], tri[std::size_t((j + 1) % 3)]}];
            for (const auto& [e, n] : directed) {
                QVERIFY2(n == 1, qPrintable(QStringLiteral("%1: an edge crossed %2 times one way").arg(sh.name).arg(n)));
                QVERIFY2(directed.count({e.second, e.first}) == 1, qPrintable(QStringLiteral("%1: an edge on one triangle").arg(sh.name)));
            }
            const double want = t.domains[d].volume, got = sh.volume();
            QVERIFY2(std::abs(got - want) < 1e-9 * want, qPrintable(QStringLiteral("%1: volume %2, the domain's %3").arg(sh.name).arg(got).arg(want)));
        }
        double boxes = 0;
        for (const DomainShell& sh : shells)
            if (sh.name.contains(QStringLiteral("lid"))) boxes += sh.volume();
        QVERIFY(std::abs(boxes - 4 * 3 * 0.5) < 1e-12);
    }

    // STL (ASCII and binary) and OBJ read back: their triangles all there,
    // in millimetres where asked.
    void stlAndObjReadBack()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("block"), 0, 0, 0, 4, 3, 2);
        addCylinder(m, QStringLiteral("rod"), 2, 1.5, 0.8, 1, 2);
        const Built b = build(m);
        QVERIFY2(b.error.isEmpty(), qPrintable(b.error));
        const Topology3& t = *b.g.topology;
        const std::vector<DomainShell> shells = domainShells(t);
        int count = 0;
        double volume = 0;
        for (const DomainShell& sh : shells) {
            count += int(sh.triangles.size());
            volume += sh.volume();
        }
        for (bool binary : {false, true}) {
            const QByteArray stl = stlOf(shells, t.unitScale, binary);
            keep(binary ? QStringLiteral("block_binary.stl") : QStringLiteral("block.stl"), stl);
            const TriangleSoup soup = readTriangles(stl, QStringLiteral("x.stl"));
            QVERIFY2(soup.error.isEmpty(), qPrintable(soup.error));
            QCOMPARE(int(soup.triangles.size()), count);
            // (Millimetres: the model's unit is.)
            double v = 0;
            for (const auto& tri : soup.triangles) v += tri[0].dot(tri[1].cross(tri[2])) / 6;
            QVERIFY2(std::abs(v - volume) < (binary ? 1e-5 : 1e-9) * volume, qPrintable(QStringLiteral("%1 against %2").arg(v).arg(volume)));
        }
        const QByteArray obj = objOf(shells, t);
        keep(QStringLiteral("block.obj"), obj);
        QCOMPARE(obj.count("\no "), qsizetype(shells.size()));
        const TriangleSoup soup = readTriangles(obj, QStringLiteral("x.obj"));
        QVERIFY2(soup.error.isEmpty(), qPrintable(soup.error));
        QCOMPARE(int(soup.triangles.size()), count);
    }

    // A STEP file: its header and data, every entity defined once and every
    // reference to one defined, a solid B-rep each domain - a box's six faces
    // each a plane in a loop of four shared edges, a face with a hole an
    // outline and a bound - in millimetres.
    void aStepFile()
    {
        Model m = model3d(0.5);
        addBox(m, QStringLiteral("plate"), 0, 0, 0, 4, 3, 1);
        const Built one = build(m);
        QVERIFY2(one.error.isEmpty(), qPrintable(one.error));
        const QByteArray step = stepOf(domainShells(*one.g.topology), *one.g.topology, QStringLiteral("plate"));
        keep(QStringLiteral("plate.step"), step);
        QVERIFY(step.startsWith("ISO-10303-21;\nHEADER;"));
        QVERIFY(step.trimmed().endsWith("END-ISO-10303-21;"));
        const auto check = [](const QByteArray& text, int breps) {
            QSet<int> defined;
            const QRegularExpression def(QStringLiteral("^#(\\d+)=(.*);$"), QRegularExpression::MultilineOption);
            QStringList bodies;
            for (auto it = def.globalMatch(QString::fromLatin1(text)); it.hasNext();) {
                const QRegularExpressionMatch mt = it.next();
                const int id = mt.captured(1).toInt();
                if (defined.contains(id)) return QStringLiteral("#%1 defined twice").arg(id);
                defined.insert(id);
                bodies << mt.captured(2);
            }
            const QRegularExpression use(QStringLiteral("#(\\d+)"));
            for (const QString& b : bodies)
                for (auto it = use.globalMatch(b); it.hasNext();) {
                    const int id = it.next().captured(1).toInt();
                    if (!defined.contains(id)) return QStringLiteral("#%1 used, not defined").arg(id);
                }
            const int n = int(bodies.filter(QRegularExpression(QStringLiteral("^(MANIFOLD_SOLID_BREP|BREP_WITH_VOIDS)\\("))).size());
            if (n != breps) return QStringLiteral("%1 solid B-reps, not %2").arg(n).arg(breps);
            return QString();
        };
        QString why = check(step, 1);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        // (A box: six planar faces, each a loop of four edges, its twelve
        // edges each shared by two of them - in millimetres, the model's.)
        QCOMPARE(int(step.count("=ADVANCED_FACE(")), 6);
        QCOMPARE(int(step.count("=PLANE(")), 6);
        QCOMPARE(int(step.count("=EDGE_CURVE(")), 12);
        QCOMPARE(int(step.count("=VERTEX_POINT(")), 8);
        const QRegularExpression loops(QStringLiteral("EDGE_LOOP\\('',\\((#\\d+(,#\\d+)*)\\)\\)"));
        int quads = 0;
        for (auto it = loops.globalMatch(QString::fromLatin1(step)); it.hasNext();) quads += it.next().captured(1).count(QLatin1Char(',')) == 3 ? 1 : 0;
        QCOMPARE(quads, 6);
        QVERIFY(step.contains("SI_UNIT(.MILLI.,.METRE.)"));
        QVERIFY(step.contains("CARTESIAN_POINT('',(4.,3.,1.))"));
        // A plate with a hole through it and a rod in the hole: two B-reps,
        // the plate's top one face of an outline and a bound.
        addCylinder(m, QStringLiteral("rod"), 2, 1.5, 0.5, 0, 1);
        const Built two = build(m);
        QVERIFY2(two.error.isEmpty(), qPrintable(two.error));
        const QByteArray holed = stepOf(domainShells(*two.g.topology), *two.g.topology, QStringLiteral("plate and rod"));
        keep(QStringLiteral("plate_and_rod.step"), holed);
        why = check(holed, int(two.g.topology->domains.size()));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QVERIFY2(holed.count("FACE_BOUND(") >= 2, "the plate's top and bottom each a bound about the rod");
        QVERIFY(!holed.contains("BREP_WITH_VOIDS("));
        // A rod wholly inside a block: the block's domain a solid with a
        // void, the rod's surface its shell taken the other way.
        Model inside = model3d(0.5);
        addBox(inside, QStringLiteral("block"), 0, 0, 0, 4, 3, 2);
        addCylinder(inside, QStringLiteral("rod"), 2, 1.5, 0.8, 0.5, 1);
        const Built three = build(inside);
        QVERIFY2(three.error.isEmpty(), qPrintable(three.error));
        const QByteArray hollow = stepOf(domainShells(*three.g.topology), *three.g.topology, QStringLiteral("block and rod"));
        keep(QStringLiteral("block_and_rod.step"), hollow);
        why = check(hollow, int(three.g.topology->domains.size()));
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QCOMPARE(int(hollow.count("=BREP_WITH_VOIDS(")), 1);
        QCOMPARE(int(hollow.count("=ORIENTED_CLOSED_SHELL('',*,")), 1);
        QVERIFY(hollow.contains(",.F.);\n") && QRegularExpression(QStringLiteral("ORIENTED_CLOSED_SHELL\\('',\\*,#\\d+,\\.F\\.\\)")).match(QString::fromLatin1(hollow)).hasMatch());
        QCOMPARE(int(hollow.count("=MANIFOLD_SOLID_BREP(")), int(three.g.topology->domains.size()) - 1);
    }

    // The mesh in MSH 4.1, INP and BDF: its nodes and elements all there,
    // each prism wound so that its volume is positive, their volumes the
    // mesh's.
    void meshFormats()
    {
        Model m = model3d(0.6);
        addBox(m, QStringLiteral("block"), 0, 0, 0, 4, 3, 2);
        addCylinder(m, QStringLiteral("rod"), 2, 1.5, 0.8, 1, 2);
        const Built b = build(m, true);
        QVERIFY2(b.error.isEmpty() && b.mesh, qPrintable(b.error));
        const Mesh3& mesh = *b.mesh;
        double volume = 0;
        for (int e = 0; e < int(mesh.prisms.size()); ++e) {
            const std::array<int, 6> p = orientedPrism(mesh, e);
            const Vec3& a = mesh.nodes[std::size_t(p[0])];
            const double v = (mesh.nodes[std::size_t(p[1])] - a).cross(mesh.nodes[std::size_t(p[2])] - a).dot(mesh.nodes[std::size_t(p[3])] - a);
            QVERIFY(v > 0);
            volume += v / 2;
        }
        // (The mesh's own: its plan's chords of the rod's circle, not the
        // geometry's.)
        QVERIFY2(std::abs(volume - mesh.volume) < 1e-9 * volume, qPrintable(QStringLiteral("%1 against %2").arg(volume).arg(mesh.volume)));
        // (A prism wound the other way given: wound positive.)
        {
            Mesh3 turned = mesh;
            std::swap(turned.prisms[0][1], turned.prisms[0][2]);
            std::swap(turned.prisms[0][4], turned.prisms[0][5]);
            const std::array<int, 6> p = orientedPrism(turned, 0);
            const Vec3& a = turned.nodes[std::size_t(p[0])];
            QVERIFY((turned.nodes[std::size_t(p[1])] - a).cross(turned.nodes[std::size_t(p[2])] - a).dot(turned.nodes[std::size_t(p[3])] - a) > 0);
        }
        const QByteArray msh = mshOf(mesh);
        keep(QStringLiteral("block.msh"), msh);
        const QList<QByteArray> lines = msh.split('\n');
        const int nodesAt = int(lines.indexOf("$Nodes")), elementsAt = int(lines.indexOf("$Elements"));
        QVERIFY(nodesAt > 0 && elementsAt > 0);
        QCOMPARE(lines[nodesAt + 1].split(' ').value(1).toLongLong(), qint64(mesh.nodes.size()));
        QCOMPARE(lines[elementsAt + 1].split(' ').value(1).toLongLong(), qint64(mesh.prisms.size() + mesh.faces.size()));
        QCOMPARE(int(msh.count("\n3 ")) >= int(mesh.topology->domains.size()), true);
        const QByteArray inp = inpOf(mesh);
        keep(QStringLiteral("block.inp"), inp);
        const auto rowsAfter = [](const QByteArray& text, const QByteArray& header) {
            int n = 0;
            bool in = false;
            for (const QByteArray& line : text.split('\n')) {
                if (line.startsWith('*')) in = line.startsWith(header);
                else if (in && !line.trimmed().isEmpty()) ++n;
            }
            return n;
        };
        QCOMPARE(rowsAfter(inp, "*NODE"), int(mesh.nodes.size()));
        QCOMPARE(rowsAfter(inp, "*ELEMENT, TYPE=C3D6"), int(mesh.prisms.size()));
        const QByteArray bdf = bdfOf(mesh);
        keep(QStringLiteral("block.bdf"), bdf);
        QCOMPARE(int(bdf.count("\nGRID*   ")), int(mesh.nodes.size()));
        QCOMPARE(int(bdf.count("\nCPENTA  ")), int(mesh.prisms.size()));
        QCOMPARE(int(bdf.count("\nCTRIA3  ") + bdf.count("\nCQUAD4  ")), int(mesh.faces.size()));
        QVERIFY(bdf.trimmed().endsWith("ENDDATA"));
    }

    // Prismatic solids imported exactly: an L-shaped prism and a square tube
    // (a hole through it), each of its volume; a model's own geometry
    // written as an STL and read back, a solid each of its domains, of its
    // volume.
    void prismaticSolidsImportExactly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // (The L's walls with a seam half-way up: a height where its section
        // does not change, its two slabs one.)
        std::vector<std::array<Vec3, 3>> tris = prismOf({{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 2}, {0, 2}}, 0, 1.5, 2);
        // (A tube: its outline and its hole, walls each way, its floor and
        // top as a frame of four quads.)
        const double o0 = 5, o1 = 8, i0 = 6, i1 = 7, z0 = 0.5, z1 = 2;
        const auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
            tris.push_back({a, b, c});
            tris.push_back({a, c, d});
        };
        for (double z : {z0, z1}) {
            const bool up = z == z1;
            const auto q = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) { up ? quad(a, b, c, d) : quad(a, d, c, b); };
            q({o0, o0, z}, {o1, o0, z}, {i1, i0, z}, {i0, i0, z});
            q({o1, o0, z}, {o1, o1, z}, {i1, i1, z}, {i1, i0, z});
            q({o1, o1, z}, {o0, o1, z}, {i0, i1, z}, {i1, i1, z});
            q({o0, o1, z}, {o0, o0, z}, {i0, i0, z}, {i0, i1, z});
        }
        const double ox[4] = {o0, o1, o1, o0}, oy[4] = {o0, o0, o1, o1}, ix[4] = {i0, i1, i1, i0}, iy[4] = {i0, i0, i1, i1};
        for (int k = 0; k < 4; ++k) {
            const int n = (k + 1) % 4;
            quad({ox[k], oy[k], z0}, {ox[n], oy[n], z0}, {ox[n], oy[n], z1}, {ox[k], oy[k], z1});
            quad({ix[n], iy[n], z0}, {ix[k], iy[k], z0}, {ix[k], iy[k], z1}, {ix[n], iy[n], z1});
        }
        // (A hollow cube, 4 across with a cavity 2 across in it: two closed
        // surfaces of one solid, the inner wound into the cavity.)
        for (const auto& t : prismOf({{10, 0}, {14, 0}, {14, 4}, {10, 4}}, 0, 4)) tris.push_back(t);
        for (auto t : prismOf({{11, 1}, {13, 1}, {13, 3}, {11, 3}}, 1, 3)) {
            std::swap(t[1], t[2]);
            tris.push_back(t);
        }
        writeFile(dir.filePath(QStringLiteral("parts.stl")), stlText(tris));
        Model m = model3d(0.5);
        m.fileName = dir.filePath(QStringLiteral("model.qfem3d"));
        addImport(m, QStringLiteral("parts"), QStringLiteral("parts.stl"));
        const Built b = build(m);
        QVERIFY2(b.error.isEmpty(), qPrintable(b.error));
        const Topology3& t = *b.g.topology;
        QVERIFY2(warningsOf(b.g).isEmpty(), qPrintable(warningsOf(b.g)));
        QVERIFY(t.objectNames.contains(QStringLiteral("parts_1")) && t.objectNames.contains(QStringLiteral("parts_3")) && !t.objectNames.contains(QStringLiteral("parts_4")));
        const double want = (3 * 1 + 1 * 1) * 1.5 + (9 - 1) * 1.5 + (64 - 8);
        QVERIFY2(std::abs(totalVolume(t) - want) < 1e-9 * want, qPrintable(QStringLiteral("%1 against %2").arg(totalVolume(t)).arg(want)));
        // (A prism's levels alike made one: the L and the tube one slab
        // each, the cube three - under, beside and over its cavity.)
        QCOMPARE(int(t.levels.size()), 7);
        // A model's geometry out and back in: an ASCII STL, a solid each
        // domain (a binary STL has none: solids that touch would be one).
        Model a = model3d(0.5);
        addBox(a, QStringLiteral("block"), 0, 0, 0, 4, 3, 2);
        addBox(a, QStringLiteral("step"), 1, 1, 2, 2, 1, 1);
        const Built ba = build(a);
        QVERIFY2(ba.error.isEmpty(), qPrintable(ba.error));
        writeFile(dir.filePath(QStringLiteral("blocks.stl")), stlOf(domainShells(*ba.g.topology), ba.g.topology->unitScale, false, false));
        Model back = model3d(0.5);
        back.fileName = m.fileName;
        addImport(back, QStringLiteral("blocks"), QStringLiteral("blocks.stl"));
        const Built bb = build(back);
        QVERIFY2(bb.error.isEmpty(), qPrintable(bb.error));
        QCOMPARE(int(bb.g.topology->domains.size()), int(ba.g.topology->domains.size()));
        QVERIFY2(bb.g.topology->objectNames.contains(QStringLiteral("blocks_d1_block")), qPrintable(bb.g.topology->objectNames.join(QStringLiteral(", "))));
        QVERIFY2(std::abs(totalVolume(*bb.g.topology) - totalVolume(*ba.g.topology)) < 1e-9 * totalVolume(*ba.g.topology),
                 qPrintable(QStringLiteral("%1 against %2").arg(totalVolume(*bb.g.topology)).arg(totalVolume(*ba.g.topology))));
    }

    // A solid not prismatic - a square pyramid, 2 across and 3 high - cut
    // into slices, each its section at its middle: its volume the midpoint
    // rule's of its sections' areas, said in a warning; more slices nearer
    // its own 4.
    void otherSolidsAreSliced()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const Vec3 apex{1, 1, 3}, c0{0, 0, 0}, c1{2, 0, 0}, c2{2, 2, 0}, c3{0, 2, 0};
        const std::vector<std::array<Vec3, 3>> tris{{c0, c2, c1}, {c0, c3, c2}, {c0, c1, apex}, {c1, c2, apex}, {c2, c3, apex}, {c3, c0, apex}};
        writeFile(dir.filePath(QStringLiteral("pyramid.stl")), stlText(tris));
        for (int slices : {4, 16}) {
            Model m = model3d(0.5);
            m.fileName = dir.filePath(QStringLiteral("model.qfem3d"));
            addImport(m, QStringLiteral("pyramid"), QStringLiteral("pyramid.stl"), slices);
            const Built b = build(m);
            QVERIFY2(b.error.isEmpty(), qPrintable(b.error));
            QVERIFY2(warningsOf(b.g).contains(QStringLiteral("not prismatic")), qPrintable(warningsOf(b.g)));
            double want = 0;
            const double h = 3.0 / slices;
            for (int k = 0; k < slices; ++k) {
                const double side = 2 * (1 - (k + 0.5) * h / 3);
                want += side * side * h;
            }
            const double got = totalVolume(*b.g.topology);
            qInfo("pyramid in %d slices: %.6f (midpoint rule %.6f, exact 4)", slices, got, want);
            QVERIFY2(std::abs(got - want) < 1e-9 * want, qPrintable(QStringLiteral("%1 against %2").arg(got).arg(want)));
            QCOMPARE(int(b.g.topology->slabs()), slices);
        }
    }

    // Refused, and why: no file, a file that is not one, a coordinate not
    // finite, slices past a thousand, a solid with no height.
    void whatIsRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Model m = model3d(0.5);
        m.fileName = dir.filePath(QStringLiteral("model.qfem3d"));
        addImport(m, QStringLiteral("none"), QStringLiteral("missing.stl"));
        Built b = build(m);
        QVERIFY2(b.error.contains(QStringLiteral("could not be read")), qPrintable(b.error));
        writeFile(dir.filePath(QStringLiteral("text.stl")), "hello\n");
        m = model3d(0.5);
        m.fileName = dir.filePath(QStringLiteral("model.qfem3d"));
        addImport(m, QStringLiteral("text"), QStringLiteral("text.stl"));
        b = build(m);
        QVERIFY2(b.error.contains(QStringLiteral("not an STL")), qPrintable(b.error));
        // (A coordinate not finite: refused where it is read.)
        for (const char* bad : {"nan", "inf"}) {
            QByteArray text = stlText(prismOf({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, 0, 1));
            text.replace("vertex 1 0 1", QByteArray("vertex 1 ") + bad + " 1");
            const TriangleSoup soup = readTriangles(text, QStringLiteral("x.stl"));
            QVERIFY2(soup.error.contains(QStringLiteral("not a finite number")) && soup.triangles.empty(), qPrintable(soup.error));
        }
        // (Slices past a thousand.)
        writeFile(dir.filePath(QStringLiteral("box.stl")), stlText(prismOf({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, 0, 1)));
        m = model3d(0.5);
        m.fileName = dir.filePath(QStringLiteral("model.qfem3d"));
        addImport(m, QStringLiteral("many"), QStringLiteral("box.stl"), 2000);
        b = build(m);
        QVERIFY2(b.error.contains(QStringLiteral("a thousand at most")), qPrintable(b.error));
        writeFile(dir.filePath(QStringLiteral("flat.stl")), stlText({{Vec3{0, 0, 1}, Vec3{1, 0, 1}, Vec3{0, 1, 1}}}));
        m = model3d(0.5);
        m.fileName = dir.filePath(QStringLiteral("model.qfem3d"));
        addImport(m, QStringLiteral("flat"), QStringLiteral("flat.stl"));
        b = build(m);
        QVERIFY2(b.error.contains(QStringLiteral("no solid with a height")), qPrintable(b.error));
    }
};

QTEST_GUILESS_MAIN(TestFem3Cad)

#include "test_fem3_cad.moc"
