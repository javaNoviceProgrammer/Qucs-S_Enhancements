/*
 * test_fem3_geometry.cpp - a 3D multiphysics model's geometry: its schema
 *                          (the .qfem3d's format, the kinds of each space),
 *                          layered solids made one - boxes, cylinders,
 *                          extruded sketches, a layer stack over a layout,
 *                          their booleans - into domains, faces, edges and
 *                          points, each domain's surface closed; the
 *                          numbering; and the selections
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_geometry.h"

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace qucs_s::fem;

namespace {

QJsonArray pairOf(double a, double b)
{
    return QJsonArray{QString::number(a, 'g', 17), QString::number(b, 'g', 17)};
}

/// A layer stack's rows - built by append: a braced list of one list is a copy
/// of that list to some compilers (CI's AppleClang).
QJsonArray rowsOf(std::initializer_list<QJsonArray> rows)
{
    QJsonArray out;
    for (const QJsonArray& r : rows) out.append(r);
    return out;
}

Model model3d(const QString& unit = QStringLiteral("mm"))
{
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), unit);
    return m;
}

Node* addBox(Model& m, const QString& label, double x, double y, double z, double w, double d, double h)
{
    Node n = m.make(QStringLiteral("box"));
    n.label = label;
    n.props.insert(QStringLiteral("position"), pairOf(x, y));
    n.props.insert(QStringLiteral("size"), pairOf(w, d));
    n.props.insert(QStringLiteral("z"), QString::number(z, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    QString why;
    Node* added = m.add(m.geometry().tag, n, -1, &why);
    if (!added) qWarning() << why;
    return added;
}

Node* addCylinder(Model& m, const QString& label, double x, double y, double r, double z, double h)
{
    Node n = m.make(QStringLiteral("cylinder"));
    n.label = label;
    n.props.insert(QStringLiteral("center"), pairOf(x, y));
    n.props.insert(QStringLiteral("radius"), QString::number(r, 'g', 17));
    n.props.insert(QStringLiteral("z"), QString::number(z, 'g', 17));
    n.props.insert(QStringLiteral("height"), QString::number(h, 'g', 17));
    return m.add(m.geometry().tag, n);
}

GeometryBuild3 build(const Model& m)
{
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    return buildGeometry3(m, *ps);
}

QString errorsOf(const GeometryBuild3& b)
{
    QStringList out;
    for (auto it = b.errors.begin(); it != b.errors.end(); ++it) out << it.key() + QStringLiteral(": ") + it.value();
    return out.join(QStringLiteral("; "));
}

QVector<int> pick(const Topology3& t, const Model& m, Level level, const char* rule, QString* error = nullptr)
{
    return resolveSelection3(t, m, level, QJsonDocument::fromJson(rule).object(), error);
}

/// Each domain's surface closed and turned outward: its faces' triangles,
/// each outward from it, sum to no area vector, and enclose its volume
/// (the divergence theorem); each face's triangles its area; each edge's
/// polyline its length, from its start to its end.
void checkClosed(const Topology3& t)
{
    const double scale = std::max(1.0, t.bounds.diagonal());
    std::vector<Vec3> flux(t.domains.size());
    std::vector<double> volume(t.domains.size(), 0);
    for (int f = 0; f < int(t.faces.size()); ++f) {
        const Topology3::Face& face = t.faces[std::size_t(f)];
        QVERIFY2(face.back != face.front, qPrintable(QStringLiteral("face %1 has one domain on both sides").arg(f + 1)));
        double area = 0;
        for (const std::array<Vec3, 3>& tri : t.faceTriangles(f)) {
            const Vec3 n = (tri[1] - tri[0]).cross(tri[2] - tri[0]) * 0.5;
            area += n.norm();
            const double v = tri[0].dot(tri[1].cross(tri[2])) / 6;
            // (Its normal points out of back, into front.)
            if (face.back >= 0) {
                flux[std::size_t(face.back)] = flux[std::size_t(face.back)] + n;
                volume[std::size_t(face.back)] += v;
            }
            if (face.front >= 0) {
                flux[std::size_t(face.front)] = flux[std::size_t(face.front)] - n;
                volume[std::size_t(face.front)] -= v;
            }
            if (face.planar) QVERIFY2(n.normalized().dot(face.normal) > 1 - 1e-9, qPrintable(QStringLiteral("face %1 turned").arg(f + 1)));
        }
        QVERIFY2(std::abs(area - face.area) <= 1e-9 * scale * scale,
                 qPrintable(QStringLiteral("face %1: triangles %2, area %3").arg(f + 1).arg(area).arg(face.area)));
    }
    for (std::size_t d = 0; d < t.domains.size(); ++d) {
        QVERIFY2(flux[d].norm() <= 1e-9 * scale * scale, qPrintable(QStringLiteral("domain %1 is not closed").arg(d + 1)));
        QVERIFY2(std::abs(volume[d] - t.domains[d].volume) <= 1e-9 * scale * scale * scale,
                 qPrintable(QStringLiteral("domain %1: %2 enclosed, volume %3").arg(d + 1).arg(volume[d]).arg(t.domains[d].volume)));
    }
    for (int e = 0; e < int(t.edges.size()); ++e) {
        const Topology3::Edge& edge = t.edges[std::size_t(e)];
        const std::vector<Vec3> line = t.edgePolyline(e);
        QVERIFY(line.size() >= 2);
        double length = 0;
        for (std::size_t i = 0; i + 1 < line.size(); ++i) length += (line[i + 1] - line[i]).norm();
        QVERIFY2(std::abs(length - edge.length) <= 1e-9 * scale, qPrintable(QStringLiteral("edge %1's length").arg(e + 1)));
        QVERIFY(edge.start >= 0 && edge.end >= 0);
        QVERIFY2((line.front() - t.points[std::size_t(edge.start)].position).norm() <= 1e-9 * scale,
                 qPrintable(QStringLiteral("edge %1 starts elsewhere").arg(e + 1)));
        QVERIFY2((line.back() - t.points[std::size_t(edge.end)].position).norm() <= 1e-9 * scale,
                 qPrintable(QStringLiteral("edge %1 ends elsewhere").arg(e + 1)));
        QVERIFY(edge.faces.size() >= 2);
    }
}

/// A GDSII file of rectangles (µm) on layers, one cell.
QByteArray gdsOf(const std::vector<std::array<double, 6>>& boxes)
{
    QByteArray bytes;
    auto record = [&](int type, const QByteArray& data) {
        const int n = 4 + int(data.size());
        bytes.append(char(n >> 8)).append(char(n & 0xff)).append(char(type >> 8)).append(char(type & 0xff)).append(data);
    };
    auto int16s = [](const std::vector<int>& v) {
        QByteArray b;
        for (int x : v) b.append(char((x >> 8) & 0xff)).append(char(x & 0xff));
        return b;
    };
    auto int32s = [](const std::vector<int>& v) {
        QByteArray b;
        for (int x : v) b.append(char((x >> 24) & 0xff)).append(char((x >> 16) & 0xff)).append(char((x >> 8) & 0xff)).append(char(x & 0xff));
        return b;
    };
    auto ascii = [](const char* s) {
        QByteArray b(s);
        if (b.size() % 2) b.append('\0');
        return b;
    };
    auto real8 = [](double v) {
        // GDSII's excess-64 base-16 real.
        QByteArray b(8, '\0');
        if (v == 0) return b;
        int sign = v < 0 ? 0x80 : 0;
        v = std::abs(v);
        int exponent = 64;
        while (v >= 1) {
            v /= 16;
            ++exponent;
        }
        while (v < 1.0 / 16) {
            v *= 16;
            --exponent;
        }
        b[0] = char(sign | exponent);
        for (int i = 1; i < 8; ++i) {
            v *= 256;
            const int digit = int(v);
            b[i] = char(digit);
            v -= digit;
        }
        return b;
    };
    auto nm = [](double um) { return int(std::lround(um * 1000)); };
    record(0x0002, int16s({600}));
    record(0x0102, int16s(std::vector<int>(12, 0)));
    record(0x0206, ascii("LIB"));
    record(0x0305, real8(1e-3) + real8(1e-9));
    record(0x0502, int16s(std::vector<int>(12, 0)));
    record(0x0606, ascii("TOP"));
    for (const auto& b : boxes) {
        record(0x0800, {});
        record(0x0D02, int16s({int(b[0])}));
        record(0x0E02, int16s({int(b[1])}));
        record(0x1003, int32s({nm(b[2]), nm(b[3]), nm(b[4]), nm(b[3]), nm(b[4]), nm(b[5]), nm(b[2]), nm(b[5]), nm(b[2]), nm(b[3])}));
        record(0x1100, {});
    }
    record(0x0700, {});
    record(0x0400, {});
    return bytes;
}

/// A 2D model's heat order as made: its kind's default (2).
QString flatOrder()
{
    Model flat = Model::blank();
    return flat.make(QStringLiteral("heat")).text(QStringLiteral("order"));
}

} // namespace

class TestFem3Geometry : public QObject
{
    Q_OBJECT

private slots:
    /// A 3D model's file and kinds: "qucs-s multiphysics 3d 1", its space;
    /// a box a 3D model's, a rectangle a 2D one's or a sketch's.
    void schema()
    {
        Model m = model3d();
        QVERIFY(m.is3D());
        QCOMPARE(m.space(), unsigned(Spaces::ThreeD));
        // (A study at rest, as a 2D model's: its physics solved by Compute.)
        QCOMPARE(int(m.studies().size()), 1);
        QCOMPARE(m.studies().front()->children.size(), std::size_t(1));
        QCOMPARE(m.studies().front()->children.front().type, QStringLiteral("stationary"));
        const QJsonObject o = m.toObject();
        QCOMPARE(o.value(QStringLiteral("format")).toString(), QStringLiteral("qucs-s multiphysics 3d 1"));
        QCOMPARE(o.value(QStringLiteral("components")).toArray().at(0).toObject().value(QStringLiteral("space")).toString(), QStringLiteral("3D"));
        // Kinds by space.
        QString why;
        Node rect = m.make(QStringLiteral("rectangle"));
        QVERIFY(!m.add(m.geometry().tag, rect, -1, &why));
        QVERIFY2(why.contains(QStringLiteral("Extrude's sketch")), qPrintable(why));
        Node* ext = m.add(m.geometry().tag, m.make(QStringLiteral("extrude")));
        QVERIFY(ext);
        const QString extTag = ext->tag;
        QVERIFY(m.add(extTag, m.make(QStringLiteral("rectangle")), -1, &why));
        QVERIFY2(!m.add(extTag, m.make(QStringLiteral("box")), -1, &why), "a box in a sketch");
        QVERIFY(!m.add(extTag, m.make(QStringLiteral("crosssection")), -1, &why));
        const QStringList kinds3 = childKinds(m.geometry(), m.spaceUnder(m.geometry()));
        QVERIFY(kinds3.contains(QStringLiteral("box")) && kinds3.contains(QStringLiteral("stack")) && kinds3.contains(QStringLiteral("union")));
        QVERIFY(!kinds3.contains(QStringLiteral("rectangle")) && !kinds3.contains(QStringLiteral("process")));
        const QStringList sketch = childKinds(*m.find(extTag), m.spaceUnder(*m.find(extTag)));
        QVERIFY(sketch.contains(QStringLiteral("rectangle")) && sketch.contains(QStringLiteral("union")) && !sketch.contains(QStringLiteral("box")));
        // Its physics heat, electrostatics and currents (phase C) and light's -
        // the eigenmode expansion and the full-wave solve - alone; a study; a
        // 2D model's kinds as they were.
        QStringList physics3 = childKinds(m.component(), m.space());
        physics3.sort();
        QCOMPARE(physics3, (QStringList{QStringLiteral("currents"), QStringLiteral("electrostatics"), QStringLiteral("eme"), QStringLiteral("heat"),
                                        QStringLiteral("waves3"), QStringLiteral("waves3t")}));
        QCOMPARE(childKinds(m.root, m.space()), QStringList{QStringLiteral("study")});
        QVERIFY(!m.add(m.component().tag, m.make(QStringLiteral("solid")), -1, &why));
        // A physics of order 1 in 3D: made so, and read so where a file gives
        // none.
        QCOMPARE(m.make(QStringLiteral("heat")).text(QStringLiteral("order")), QStringLiteral("1"));
        QCOMPARE(flatOrder(), QStringLiteral("2"));
        const std::optional<Model> read = Model::fromJson(QByteArray(R"({"format": "qucs-s multiphysics 3d 1", "components": [{"unit": "mm", "space": "3D", "physics": [{"type": "heat", "tag": "ht"}]}]})"));
        QVERIFY(read && read->is3D());
        QCOMPARE(read->find(QStringLiteral("ht"))->text(QStringLiteral("order")), QStringLiteral("1"));
        Model flat = Model::blank();
        QVERIFY(!flat.is3D());
        QVERIFY(!flat.add(flat.geometry().tag, flat.make(QStringLiteral("box")), -1, &why));
        QVERIFY2(why.contains(QStringLiteral("3D model's")), qPrintable(why));
        const QStringList kinds2 = childKinds(flat.geometry(), flat.space());
        QVERIFY(kinds2.contains(QStringLiteral("rectangle")) && !kinds2.contains(QStringLiteral("box")) && !kinds2.contains(QStringLiteral("extrude")));
        // (Every physics but a 3D model's alone: its eigenmode expansion and
        // its full-wave solve.)
        QStringList physics2;
        for (const QString& type : physicsTypes())
            if (nodeKind(type)->spaces & Spaces::TwoD) physics2 << type;
        QCOMPARE(physics2.size(), physicsTypes().size() - 3);
        QCOMPARE(childKinds(flat.component(), flat.space()), physics2);
        QCOMPARE(flat.toObject().value(QStringLiteral("format")).toString(), QStringLiteral("qucs-s multiphysics 1"));
        // The space not shown in 3D; an edge a 3D selection's level alone.
        const NodeKind* comp = nodeKind(QStringLiteral("component"));
        QVERIFY(!propertyVisible(m.component(), *comp->property(QStringLiteral("space")), comp));
        QVERIFY(!propertyVisible(m.component(), *comp->property(QStringLiteral("thickness")), comp));
        QVERIFY(propertyVisible(flat.component(), *comp->property(QStringLiteral("space")), comp));
        const PropertyDef* level = nodeKind(QStringLiteral("selection"))->property(QStringLiteral("level"));
        QVERIFY(level->choiceIn(3, Spaces::ThreeD) && !level->choiceIn(3, Spaces::Plane) && level->choiceIn(1, Spaces::Plane));
        QCOMPARE(levelOf(QStringLiteral("edge")), Level::Edge);
        QCOMPARE(levelNames(Level::Edge), QStringLiteral("edges"));
        // Read back as written; a 3D format's space taken as 3D.
        addBox(m, QStringLiteral("blk"), 0, 0, 0, 1, 1, 1);
        QString note;
        const std::optional<Model> back = Model::fromJson(m.toJson(), &note);
        QVERIFY2(back && note.isEmpty(), qPrintable(note));
        QVERIFY(back->is3D());
        QCOMPARE(back->toJson(), m.toJson());
        QJsonObject doc = m.toObject();
        QJsonArray comps = doc.value(QStringLiteral("components")).toArray();
        QJsonObject c0 = comps.at(0).toObject();
        c0.insert(QStringLiteral("space"), QStringLiteral("2D"));
        comps[0] = c0;
        doc.insert(QStringLiteral("components"), comps);
        const std::optional<Model> mixed = Model::fromJson(QJsonDocument(doc).toJson(), &note);
        QVERIFY(mixed && mixed->is3D());
        QVERIFY2(note.contains(QStringLiteral("read as 3D")), qPrintable(note));
        // The objects' names: a sketch's and a stack's rows too.
        Node stack = m.make(QStringLiteral("stack"));
        stack.label = QStringLiteral("chip");
        stack.props.insert(QStringLiteral("stack"), rowsOf({QJsonArray{QStringLiteral("oxide"), QString(), QStringLiteral("0"), QStringLiteral("2")}}));
        m.add(m.geometry().tag, stack);
        QCOMPARE(m.geometry().children[std::size_t(m.geometry().children.size()) - 2].value(QStringLiteral("stack")).toArray().at(0).toArray().size(), 4);
        const QStringList names = m.objectNames();
        QVERIFY2(names.contains(QStringLiteral("r1")) && names.contains(QStringLiteral("oxide")) && names.contains(QStringLiteral("chip"))
                     && names.contains(QStringLiteral("blk")),
                 qPrintable(names.join(QStringLiteral(", "))));
    }

    /// One box: a domain, six faces numbered from below, twelve edges, eight
    /// points.
    void aBox()
    {
        Model m = model3d();
        addBox(m, QStringLiteral("blk"), 0, 0, 0, 2, 3, 1);
        const GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QCOMPARE(int(t.domains.size()), 1);
        QCOMPARE(int(t.faces.size()), 6);
        QCOMPARE(int(t.edges.size()), 12);
        QCOMPARE(int(t.points.size()), 8);
        QVERIFY(std::abs(t.domains[0].volume - 6) < 1e-12);
        QCOMPARE(t.objectNames, QStringList{QStringLiteral("blk")});
        checkClosed(t);
        // Bottom, left (x = 0), front (y = 0), back, right, top.
        const Vec3 expected[6] = {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}};
        for (int f = 0; f < 6; ++f) {
            QVERIFY(t.faces[std::size_t(f)].exterior());
            QVERIFY2(t.normalAt(f, 0).dot(expected[f]) > 1 - 1e-12, qPrintable(QStringLiteral("face %1").arg(f + 1)));
        }
        QVERIFY(std::abs(t.faces[0].area - 6) < 1e-12 && std::abs(t.faces[1].area - 3) < 1e-12 && std::abs(t.faces[2].area - 2) < 1e-12);
        double edges = 0;
        for (const Topology3::Edge& e : t.edges) edges += e.length;
        QVERIFY(std::abs(edges - 4 * (2 + 3 + 1)) < 1e-12);
        QCOMPARE(t.points[0].position.z, 0.0);
        QCOMPARE(t.points[7].position.z, 1.0);
        QCOMPARE(t.domainAt(Vec3{1, 1, 0.5}), 0);
        QCOMPARE(t.domainAt(Vec3{1, 1, 1.5}), -1);
        // Each side by the way its face looks out of the box.
        const char* sides[6] = {"bottom", "left", "front", "back", "right", "top"};
        for (int f = 0; f < 6; ++f)
            QCOMPARE(pick(t, m, Level::Boundary, qPrintable(QStringLiteral(R"({"of": ["blk"], "side": "%1"})").arg(QLatin1String(sides[f])))),
                     QVector<int>{f});
    }

    /// Two boxes stacked, one footprint: two domains, the face between them
    /// one, and the edges where three faces meet broken.
    void stackedBoxes()
    {
        Model m = model3d();
        addBox(m, QStringLiteral("lower"), 0, 0, 0, 1, 1, 1);
        addBox(m, QStringLiteral("upper"), 0, 0, 1, 1, 1, 2);
        const GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QCOMPARE(int(t.domains.size()), 2);
        QCOMPARE(int(t.faces.size()), 11);
        QCOMPARE(int(t.edges.size()), 20);
        QCOMPARE(int(t.points.size()), 12);
        QCOMPARE(int(t.levels.size()), 3);
        checkClosed(t);
        // Numbered from below.
        QVERIFY(std::abs(t.domains[0].volume - 1) < 1e-12 && std::abs(t.domains[1].volume - 2) < 1e-12);
        QCOMPARE(t.objectNames.at(t.domains[1].owner), QStringLiteral("upper"));
        const QVector<int> between = pick(t, m, Level::Boundary, R"({"between": ["lower", "upper"]})");
        QCOMPARE(between.size(), 1);
        const Topology3::Face& mid = t.faces[std::size_t(between[0])];
        QVERIFY(mid.floor && mid.back == 0 && mid.front == 1);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"interior": true})"), between);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"exterior": true})").size(), 10);
        // V - E + F - D: a ball's 1.
        QCOMPARE(int(t.points.size()) - int(t.edges.size()) + int(t.faces.size()) - int(t.domains.size()), 1);
    }

    /// Two boxes side by side at a height, one sitting half over the other's
    /// edge: an L of two domains, faces joined where they are one plane.
    void lShape()
    {
        Model m = model3d();
        addBox(m, QStringLiteral("base"), 0, 0, 0, 2, 1, 1);
        addBox(m, QStringLiteral("tower"), 0, 0, 1, 1, 1, 1);
        const GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QCOMPARE(int(t.domains.size()), 2);
        QCOMPARE(int(t.faces.size()), 12);
        checkClosed(t);
        QCOMPARE(int(t.points.size()) - int(t.edges.size()) + int(t.faces.size()) - int(t.domains.size()), 1);
        // The base's bottom one face, though the tower's outline crosses it.
        const QVector<int> bottom = pick(t, m, Level::Boundary, R"({"plane": {"z": 0}})");
        QCOMPARE(bottom.size(), 1);
        QVERIFY(std::abs(t.faces[std::size_t(bottom[0])].area - 2) < 1e-12);
        // The base's front wall one face too: one plane, the same domains.
        const QVector<int> front = pick(t, m, Level::Boundary, R"({"of": ["base"], "side": "front"})");
        QCOMPARE(front.size(), 1);
        QVERIFY(std::abs(t.faces[std::size_t(front[0])].area - 2) < 1e-12);
        // The top at z = 1: the tower over the base's left half, the right
        // half open.
        QCOMPARE(pick(t, m, Level::Boundary, R"({"plane": {"z": 1}})").size(), 2);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"of": ["base"], "side": "top"})").size(), 2);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"of": ["tower"], "side": "top"})").size(), 1);
    }

    /// Two boxes overlapping at one height: three domains - each alone and
    /// what they share, owned by the smaller.
    void overlap()
    {
        Model m = model3d();
        addBox(m, QStringLiteral("big"), 0, 0, 0, 3, 2, 1);
        addBox(m, QStringLiteral("small"), 2, 0.5, 0, 2, 1, 1);
        const GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QCOMPARE(int(t.domains.size()), 3);
        checkClosed(t);
        int shared = -1;
        for (int d = 0; d < 3; ++d)
            if (t.domains[std::size_t(d)].objects.size() == 2) shared = d;
        QVERIFY(shared >= 0);
        QVERIFY(std::abs(t.domains[std::size_t(shared)].volume - 1) < 1e-12);
        QCOMPARE(t.objectNames.at(t.domains[std::size_t(shared)].owner), QStringLiteral("small"));
        QCOMPARE(pick(t, m, Level::Domain, R"({"objects": ["big"]})").size(), 2);
        QCOMPARE(pick(t, m, Level::Domain, R"({"only": ["small"]})").size(), 2);
        QCOMPARE(pick(t, m, Level::Domain, R"({"only": ["big"]})").size(), 1);
    }

    /// A cylinder: a curved wall, a floor and a ceiling; two closed edges,
    /// each a point.
    void aCylinder()
    {
        Model m = model3d();
        addCylinder(m, QStringLiteral("rod"), 0, 0, 1, 0, 4);
        const GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QCOMPARE(int(t.domains.size()), 1);
        QCOMPARE(int(t.faces.size()), 3);
        QCOMPARE(int(t.edges.size()), 2);
        QCOMPARE(int(t.points.size()), 2);
        checkClosed(t);
        const Topology3::Face& wall = t.faces[1];
        QVERIFY(!wall.floor && !wall.planar);
        // Its volume the plan circle's polygon's: within 1 % of πr²h.
        QVERIFY(std::abs(t.domains[0].volume - 4 * M_PI) < 0.01 * 4 * M_PI);
        const Vec3 n = t.normalAt(1, 0);
        const Vec3 radial = Vec3{wall.middle.x, wall.middle.y, 0}.normalized();
        QVERIFY(n.dot(radial) > 0.99);
        for (const Topology3::Edge& e : t.edges) QCOMPARE(e.start, e.end);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"normal": [0, 0, 1]})").size(), 1);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"of": ["rod"], "side": "top"})"), QVector<int>{2});
        QCOMPARE(pick(t, m, Level::Boundary, R"({"of": ["rod"], "side": "bottom"})"), QVector<int>{0});
    }

    /// A sketch extruded: each object a solid of its name, the extrude's
    /// name all of them; its booleans its own objects alone.
    void extrudeSketch()
    {
        Model m = model3d();
        Node* ext = m.add(m.geometry().tag, m.make(QStringLiteral("extrude")));
        ext->label = QStringLiteral("die");
        ext->props.insert(QStringLiteral("z"), QStringLiteral("1"));
        ext->props.insert(QStringLiteral("height"), QStringLiteral("2"));
        const QString tag = ext->tag;
        Node r = m.make(QStringLiteral("rectangle"));
        r.label = QStringLiteral("pad");
        r.props.insert(QStringLiteral("size"), pairOf(2, 2));
        m.add(tag, r);
        Node c = m.make(QStringLiteral("circle"));
        c.label = QStringLiteral("post");
        c.props.insert(QStringLiteral("center"), pairOf(5, 1));
        c.props.insert(QStringLiteral("radius"), QStringLiteral("0.5"));
        m.add(tag, c);
        GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        QCOMPARE(b.topology->objectNames, (QStringList{QStringLiteral("pad"), QStringLiteral("post")}));
        QCOMPARE(b.topology->objectGroups, (QStringList{QStringLiteral("die"), QStringLiteral("die")}));
        QCOMPARE(int(b.topology->domains.size()), 2);
        QCOMPARE(pick(*b.topology, m, Level::Domain, R"({"objects": ["die"]})").size(), 2);
        QCOMPARE(b.topology->levels, (std::vector<double>{1, 3}));
        checkClosed(*b.topology);
        // A sketch's union of a box outside it: refused, the feature named.
        addBox(m, QStringLiteral("blk"), 10, 10, 0, 1, 1, 1);
        Node u = m.make(QStringLiteral("union"));
        u.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("pad"), QStringLiteral("blk")});
        QString why;
        QVERIFY2(m.add(tag, u, -1, &why), qPrintable(why));
        b = build(m);
        QVERIFY(!b.ok());
        QCOMPARE(b.failedAt, tag);
        QVERIFY2(b.errors.value(tag).contains(QStringLiteral("no object blk")), qPrintable(errorsOf(b)));
    }

    /// Booleans of solids: a box with a cylinder cut out (a hole through it),
    /// an empty intersection refused.
    void booleans()
    {
        Model m = model3d();
        addBox(m, QStringLiteral("plate"), -2, -2, 0, 4, 4, 1);
        addCylinder(m, QStringLiteral("drill"), 0, 0, 0.5, -1, 3);
        Node d = m.make(QStringLiteral("difference"));
        d.label = QStringLiteral("holed");
        d.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("plate")});
        d.props.insert(QStringLiteral("tools"), QJsonArray{QStringLiteral("drill")});
        m.add(m.geometry().tag, d);
        GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QCOMPARE(t.objectNames, QStringList{QStringLiteral("holed")});
        QCOMPARE(int(t.domains.size()), 1);
        // Bottom, top, four outer walls, the hole's wall.
        QCOMPARE(int(t.faces.size()), 7);
        checkClosed(t);
        QVERIFY(std::abs(t.domains[0].volume - (16 - M_PI * 0.25)) < 0.01);
        // The hole's wall faces in: its normal out of the plate toward the axis.
        int hole = -1;
        for (int f = 0; f < 7; ++f)
            if (!t.faces[std::size_t(f)].planar) hole = f;
        QVERIFY(hole >= 0);
        const Topology3::Face& hw = t.faces[std::size_t(hole)];
        QVERIFY(t.normalAt(hole, 0).dot(Vec3{hw.middle.x, hw.middle.y, 0}.normalized()) < -0.99);
        // Nothing in common: refused, the feature named.
        Model e = model3d();
        addBox(e, QStringLiteral("a"), 0, 0, 0, 1, 1, 1);
        addBox(e, QStringLiteral("b"), 5, 0, 0, 1, 1, 1);
        Node i = e.make(QStringLiteral("intersection"));
        i.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("a"), QStringLiteral("b")});
        const QString itag = e.add(e.geometry().tag, i)->tag;
        b = build(e);
        QVERIFY(!b.ok());
        QCOMPARE(b.failedAt, itag);
        QVERIFY2(b.errors.value(itag).contains(QStringLiteral("nothing is left")), qPrintable(errorsOf(b)));
        // A union: one solid, one domain, its faces on one plane joined.
        Model u = model3d();
        addBox(u, QStringLiteral("a"), 0, 0, 0, 2, 1, 1);
        addBox(u, QStringLiteral("b"), 1, 0, 0, 2, 1, 1);
        Node un = u.make(QStringLiteral("union"));
        un.props.insert(QStringLiteral("input"), QJsonArray{QStringLiteral("a"), QStringLiteral("b")});
        u.add(u.geometry().tag, un);
        b = build(u);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        QCOMPARE(int(b.topology->domains.size()), 1);
        QCOMPARE(int(b.topology->faces.size()), 6);
        QCOMPARE(int(b.topology->edges.size()), 12);
        checkClosed(*b.topology);
    }

    /// A layer stack over a layout: a substrate and an oxide across the
    /// window, a metal where layer 1/0 is drawn; a box in the oxide its own
    /// domain.
    void layerStack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile gds(dir.filePath(QStringLiteral("chip.gds")));
        QVERIFY(gds.open(QIODevice::WriteOnly));
        gds.write(gdsOf({{1, 0, 10, 10, 30, 14}, {2, 0, 0, 0, 1, 1}}));
        gds.close();
        Model m = model3d(QStringLiteral("um"));
        m.fileName = dir.filePath(QStringLiteral("chip.qfem3d"));
        Node s = m.make(QStringLiteral("stack"));
        s.label = QStringLiteral("chip");
        s.props.insert(QStringLiteral("file"), QStringLiteral("chip.gds"));
        s.props.insert(QStringLiteral("margin"), QStringLiteral("5"));
        s.props.insert(QStringLiteral("stack"), QJsonArray{QJsonArray{QStringLiteral("substrate"), QString(), QStringLiteral("0"), QStringLiteral("10")},
                                                          QJsonArray{QStringLiteral("oxide"), QString(), QStringLiteral("10"), QStringLiteral("2")},
                                                          QJsonArray{QStringLiteral("metal"), QStringLiteral("1/0"), QStringLiteral("12"), QStringLiteral("1")}});
        m.add(m.geometry().tag, s);
        GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QCOMPARE(t.objectNames, (QStringList{QStringLiteral("substrate"), QStringLiteral("oxide"), QStringLiteral("metal")}));
        QCOMPARE(int(t.domains.size()), 3);
        checkClosed(t);
        // The window round both layers' shapes (the stack's layers alone:
        // 1/0), 5 µm beyond.
        QCOMPARE(t.bounds.x0, 5.0);
        QCOMPARE(t.bounds.x1, 35.0);
        QCOMPARE(t.bounds.y0, 5.0);
        QCOMPARE(t.bounds.y1, 19.0);
        QCOMPARE(t.bounds.z1, 13.0);
        const QVector<int> metal = pick(t, m, Level::Domain, R"({"layer": ["metal"]})");
        QCOMPARE(metal.size(), 1);
        QVERIFY(std::abs(t.domains[std::size_t(metal[0])].volume - 20 * 4 * 1) < 1e-9);
        QCOMPARE(pick(t, m, Level::Domain, R"({"layer": ["chip"]})").size(), 3);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"of": ["metal"], "side": "top"})").size(), 1);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"between": ["oxide", "metal"]})").size(), 1);
        QVERIFY(b.files.contains(QFileInfo(gds.fileName()).absoluteFilePath()));
        // A box in the oxide: what they share its own domain, the box's.
        addBox(m, QStringLiteral("heater"), 15, 8, 10.5, 4, 2, 1);
        b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        QCOMPARE(int(b.topology->domains.size()), 4);
        checkClosed(*b.topology);
        // The oxide's walls one face a side, through the slabs the heater
        // makes of it.
        QCOMPARE(pick(*b.topology, m, Level::Boundary, R"({"of": ["oxide"], "side": "front"})").size(), 1);
        QString why;
        const QVector<int> heater = pick(*b.topology, m, Level::Domain, R"({"only": ["heater"]})", &why);
        QCOMPARE(heater.size(), 1);
        QVERIFY(std::abs(b.topology->domains[std::size_t(heater[0])].volume - 8) < 1e-9);
        QCOMPARE(pick(*b.topology, m, Level::Domain, R"({"layer": ["heater"]})", &why).size(), 0);
        QVERIFY2(why.contains(QStringLiteral("not a layer")), qPrintable(why));
        // A row of a layer the window has nothing of: left out, said.
        QJsonArray rows = s.props.value(QStringLiteral("stack")).toArray();
        rows.append(QJsonArray{QStringLiteral("poly"), QStringLiteral("7/0"), QStringLiteral("13"), QStringLiteral("1")});
        m.geometry().children.front().props.insert(QStringLiteral("stack"), rows);
        b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        QVERIFY(!b.topology->objectNames.contains(QStringLiteral("poly")));
        QVERIFY2(b.warnings.value(m.geometry().children.front().tag).contains(QStringLiteral("nothing of layer 7/0")),
                 qPrintable(b.warnings.values().join(QStringLiteral("; "))));
        // A window narrower than the metal: the metal cut to it.
        Model narrow = model3d(QStringLiteral("um"));
        narrow.fileName = m.fileName;
        Node n = s;
        n.props.insert(QStringLiteral("window"), QStringLiteral("box"));
        n.props.insert(QStringLiteral("from"), pairOf(15, 0));
        n.props.insert(QStringLiteral("to"), pairOf(25, 20));
        narrow.add(narrow.geometry().tag, n);
        b = build(narrow);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const QVector<int> cut = pick(*b.topology, narrow, Level::Domain, R"({"layer": ["metal"]})");
        QCOMPARE(cut.size(), 1);
        QVERIFY(std::abs(b.topology->domains[std::size_t(cut[0])].volume - 10 * 4 * 1) < 1e-9);
        // The rows in µm whatever the geometry's unit: in mm a thousandth.
        Model mm = model3d(QStringLiteral("mm"));
        Node r = mm.make(QStringLiteral("stack"));
        r.props.insert(QStringLiteral("window"), QStringLiteral("box"));
        r.props.insert(QStringLiteral("to"), pairOf(2000, 1000));
        r.props.insert(QStringLiteral("stack"), rowsOf({QJsonArray{QStringLiteral("wafer"), QString(), QStringLiteral("0"), QStringLiteral("500")}}));
        mm.add(mm.geometry().tag, r);
        b = build(mm);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        QVERIFY(std::abs(b.topology->bounds.z1 - 0.5) < 1e-12 && std::abs(b.topology->bounds.x1 - 2) < 1e-12);
    }

    /// The selection rules in 3D: numbers, box, sides, normal, plane,
    /// adjacent, named, except; what each says wrong.
    void selections()
    {
        Model m = model3d();
        addBox(m, QStringLiteral("lower"), 0, 0, 0, 1, 1, 1);
        addBox(m, QStringLiteral("upper"), 0, 0, 1, 1, 1, 1);
        Node sel = m.make(QStringLiteral("selection"));
        sel.label = QStringLiteral("tops");
        sel.props.insert(QStringLiteral("level"), QStringLiteral("boundary"));
        sel.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("normal"), QJsonArray{0, 0, 1}}});
        m.add(m.component().child(QStringLiteral("selections"))->tag, sel);
        const GeometryBuild3 b = build(m);
        QVERIFY2(b.ok(), qPrintable(errorsOf(b)));
        const Topology3& t = *b.topology;
        QString why;
        QCOMPARE(pick(t, m, Level::Domain, R"({"numbers": [2]})"), QVector<int>{1});
        QCOMPARE(pick(t, m, Level::Domain, R"({"box": [-1, -1, -1, 2, 2, 1.5]})"), QVector<int>{0});
        // Exterior faces facing up: the upper box's top alone; the interior
        // one faces both ways.
        QCOMPARE(pick(t, m, Level::Boundary, R"({"named": "tops"})").size(), 2);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"normal": [0, 0, 1], "except": {"interior": true}})").size(), 1);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"of": ["upper"], "side": ["left", "right", "front", "back"]})").size(), 4);
        QCOMPARE(pick(t, m, Level::Boundary, R"({"adjacent": {"objects": ["lower"]}})").size(), 6);
        QCOMPARE(pick(t, m, Level::Edge, R"({"plane": {"z": 1}})").size(), 4);
        QCOMPARE(pick(t, m, Level::Point, R"({"plane": {"z": 2}})").size(), 4);
        QCOMPARE(pick(t, m, Level::Edge, R"({"of": ["upper"]})").size(), 12);
        QCOMPARE(pick(t, m, Level::Point, R"({"of": ["upper"], "side": "top"})").size(), 4);
        QCOMPARE(pick(t, m, Level::Edge, R"({"all": true})").size(), 20);
        // What it says wrong.
        pick(t, m, Level::Boundary, R"({"of": ["nowhere"]})", &why);
        QVERIFY2(why.contains(QStringLiteral("no solid nowhere")), qPrintable(why));
        pick(t, m, Level::Boundary, R"({"of": ["upper"], "side": "north"})", &why);
        QVERIFY2(why.contains(QStringLiteral("front, back")), qPrintable(why));
        pick(t, m, Level::Domain, R"({"box": [0, 0, 1, 1]})", &why);
        QVERIFY2(why.contains(QStringLiteral("six numbers")), qPrintable(why));
        pick(t, m, Level::Boundary, R"({"numbers": [99]})", &why);
        QVERIFY2(why.contains(QStringLiteral("no face 99")), qPrintable(why));
        pick(t, m, Level::Domain, R"({"named": "tops"})", &why);
        QVERIFY2(why.contains(QStringLiteral("of boundaries, not domains")), qPrintable(why));
        pick(t, m, Level::Boundary, R"({"all": true, "of": ["upper"]})", &why);
        QVERIFY2(why.contains(QStringLiteral("of of the same selection left out")), qPrintable(why));
        QCOMPARE(describeSelection3(QJsonDocument::fromJson(R"({"of": ["upper"], "side": "top"})").object(), Level::Boundary),
                 QStringLiteral("the faces of upper (top)"));
        QCOMPARE(describeSelection3(QJsonDocument::fromJson(R"({"plane": {"z": 1}})").object(), Level::Edge),
                 QStringLiteral("the edges in the plane z = 1"));
    }

    /// What a 3D geometry refuses, and where.
    void errors()
    {
        // A rectangle of the plan at the top of a 3D geometry (a file's).
        Model m = model3d();
        Node r;
        r.type = QStringLiteral("rectangle");
        r.tag = QStringLiteral("r1");
        r.label = QStringLiteral("r1");
        m.geometry().children.insert(m.geometry().children.begin(), r);
        GeometryBuild3 b = build(m);
        QCOMPARE(b.failedAt, QStringLiteral("r1"));
        QVERIFY2(b.errors.value(QStringLiteral("r1")).contains(QStringLiteral("Extrude's sketch")), qPrintable(errorsOf(b)));
        // No height; an empty sketch; a name twice; a layer without a layout.
        Model h = model3d();
        Node* bx = addBox(h, QStringLiteral("flat"), 0, 0, 0, 1, 1, 1);
        bx->props.insert(QStringLiteral("height"), QStringLiteral("0"));
        b = build(h);
        QVERIFY2(b.errors.value(bx->tag).contains(QStringLiteral("height must be more than 0")), qPrintable(errorsOf(b)));
        Model e = model3d();
        const QString ext = e.add(e.geometry().tag, e.make(QStringLiteral("extrude")))->tag;
        b = build(e);
        QVERIFY2(b.errors.value(ext).contains(QStringLiteral("sketch is empty")), qPrintable(errorsOf(b)));
        Model twice = model3d();
        Node s = twice.make(QStringLiteral("stack"));
        s.props.insert(QStringLiteral("window"), QStringLiteral("box"));
        s.props.insert(QStringLiteral("stack"), QJsonArray{QJsonArray{QStringLiteral("a"), QString(), QStringLiteral("0"), QStringLiteral("1")},
                                                          QJsonArray{QStringLiteral("a"), QString(), QStringLiteral("1"), QStringLiteral("1")}});
        const QString st = twice.add(twice.geometry().tag, s)->tag;
        b = build(twice);
        QVERIFY2(b.errors.value(st).contains(QStringLiteral("solid a already")), qPrintable(errorsOf(b)));
        Model nolayout = model3d();
        Node l = nolayout.make(QStringLiteral("stack"));
        l.props.insert(QStringLiteral("stack"), rowsOf({QJsonArray{QStringLiteral("m1"), QStringLiteral("1/0"), QStringLiteral("0"), QStringLiteral("1")}}));
        const QString lt = nolayout.add(nolayout.geometry().tag, l)->tag;
        b = build(nolayout);
        QVERIFY2(b.errors.value(lt).contains(QStringLiteral("give it a layout")), qPrintable(errorsOf(b)));
        // A stack with nothing to wrap: its window asked for.
        Model alone = model3d();
        Node a = alone.make(QStringLiteral("stack"));
        a.props.insert(QStringLiteral("stack"), rowsOf({QJsonArray{QStringLiteral("si"), QString(), QStringLiteral("0"), QStringLiteral("1")}}));
        const QString at = alone.add(alone.geometry().tag, a)->tag;
        b = build(alone);
        QVERIFY2(b.errors.value(at).contains(QStringLiteral("give it a box")), qPrintable(errorsOf(b)));
        // A solid thinner than the union's tolerance: refused, said why.
        Model thin = model3d();
        addBox(thin, QStringLiteral("big"), 0, 0, 0, 1, 1, 1);
        Node* foil = addBox(thin, QStringLiteral("foil"), 0, 0, 1, 1, 1, 1);
        foil->props.insert(QStringLiteral("height"), QStringLiteral("1e-9"));
        const QString ft = foil->tag;
        b = build(thin);
        QCOMPARE(b.failedAt, ft);
        QVERIFY2(b.errors.value(ft).contains(QStringLiteral("less than the union's tolerance")), qPrintable(errorsOf(b)));
        // A feature that fails and leaves a solid of its own too thin: its
        // own error said, not the union's of what it left.
        Model both = model3d();
        addBox(both, QStringLiteral("blk"), 2, 0, 0, 1, 1, 1);
        Node* failing = both.add(both.geometry().tag, both.make(QStringLiteral("extrude")));
        failing->props.insert(QStringLiteral("height"), QStringLiteral("1e-12"));
        const QString et = failing->tag;
        both.add(et, both.make(QStringLiteral("rectangle")));
        Node* twin = both.add(et, both.make(QStringLiteral("rectangle")));
        twin->props.insert(QStringLiteral("position"), pairOf(0, 2));
        twin->label = QStringLiteral("blk");   // (a name the box has: past Model::add's own check)
        b = build(both);
        QCOMPARE(b.failedAt, et);
        QVERIFY2(b.errors.value(et).contains(QStringLiteral("solid blk already")), qPrintable(errorsOf(b)));
        // No solid at all: said, not built.
        b = build(model3d());
        QVERIFY(b.topology && b.topology->domains.empty());
        QVERIFY2(b.warnings.value(QStringLiteral("fin")).contains(QStringLiteral("no solids")), qPrintable(b.warnings.values().join(QStringLiteral("; "))));
    }
};

QTEST_GUILESS_MAIN(TestFem3Geometry)
#include "test_fem3_geometry.moc"
