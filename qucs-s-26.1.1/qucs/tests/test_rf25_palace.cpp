/***************************************************************************
                            test_rf25_palace.cpp
                           ----------------------
 *  An RF 2.5D model written as a Palace project (rf25_palace.h): its mesh
 *  read back - every tetrahedron turned positive, no face of the domain
 *  without its boundary, the volumes and the boundaries' areas those of the
 *  box, the conductors and the ports as drawn - and its configuration as
 *  Palace takes it: the materials, the boundaries by attribute, each port
 *  driven in turn, the study's sweep.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 ***************************************************************************/

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "rf25/rf25_layout.h"
#include "rf25/rf25_model.h"
#include "rf25/rf25_palace.h"

#include <array>
#include <cmath>
#include <map>
#include <set>

using namespace qucs_s::rf25;

namespace {

/// A mesh in Gmsh's MSH 2.2 as read back: its physical names by dimension
/// and tag, its nodes, its tetrahedra and triangles with their tags.
struct Msh {
    std::map<std::pair<int, int>, QString> names;
    std::map<int, std::array<double, 3>> nodes;
    std::vector<std::pair<std::array<int, 4>, int>> tets;
    std::vector<std::pair<std::array<int, 3>, int>> tris;
    QString error;

    int tagOf(int dim, const QString& name) const
    {
        for (const auto& [key, n] : names)
            if (key.first == dim && n == name) return key.second;
        return -1;
    }
    double volume(const std::array<int, 4>& t) const
    {
        const auto& a = nodes.at(t[0]);
        const auto& b = nodes.at(t[1]);
        const auto& c = nodes.at(t[2]);
        const auto& d = nodes.at(t[3]);
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double wx = d[0] - a[0], wy = d[1] - a[1], wz = d[2] - a[2];
        return (ux * (vy * wz - vz * wy) - uy * (vx * wz - vz * wx) + uz * (vx * wy - vy * wx)) / 6;
    }
    double area(const std::array<int, 3>& t) const
    {
        const auto& a = nodes.at(t[0]);
        const auto& b = nodes.at(t[1]);
        const auto& c = nodes.at(t[2]);
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        return std::sqrt(cx * cx + cy * cy + cz * cz) / 2;
    }
    double areaOf(int tag) const
    {
        double s = 0;
        for (const auto& [t, g] : tris)
            if (g == tag) s += area(t);
        return s;
    }
};

Msh readMsh(const QByteArray& bytes)
{
    Msh m;
    QTextStream in(bytes);
    QString line;
    const auto expect = [&](const char* what) {
        line = in.readLine();
        if (line != QLatin1String(what)) m.error = QStringLiteral("expected %1, read %2").arg(QLatin1String(what), line);
        return m.error.isEmpty();
    };
    if (!expect("$MeshFormat")) return m;
    if (in.readLine() != QLatin1String("2.2 0 8")) {
        m.error = QStringLiteral("not MSH 2.2 ASCII");
        return m;
    }
    if (!expect("$EndMeshFormat") || !expect("$PhysicalNames")) return m;
    const int nNames = in.readLine().toInt();
    for (int i = 0; i < nNames; ++i) {
        const QString l = in.readLine();
        const int q = int(l.indexOf(QLatin1Char('"')));
        const QStringList head = l.left(q).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        m.names[{head.at(0).toInt(), head.at(1).toInt()}] = l.mid(q + 1, l.size() - q - 2);
    }
    if (!expect("$EndPhysicalNames") || !expect("$Nodes")) return m;
    const int nNodes = in.readLine().toInt();
    for (int i = 0; i < nNodes; ++i) {
        const QStringList f = in.readLine().split(QLatin1Char(' '));
        m.nodes[f.at(0).toInt()] = {f.at(1).toDouble(), f.at(2).toDouble(), f.at(3).toDouble()};
    }
    if (!expect("$EndNodes") || !expect("$Elements")) return m;
    const int nElements = in.readLine().toInt();
    for (int i = 0; i < nElements; ++i) {
        const QStringList f = in.readLine().split(QLatin1Char(' '));
        const int type = f.at(1).toInt(), tags = f.at(2).toInt(), physical = f.at(3).toInt();
        const int first = 3 + tags;
        if (type == 4) m.tets.push_back({{f.at(first).toInt(), f.at(first + 1).toInt(), f.at(first + 2).toInt(), f.at(first + 3).toInt()}, physical});
        else if (type == 2) m.tris.push_back({{f.at(first).toInt(), f.at(first + 1).toInt(), f.at(first + 2).toInt()}, physical});
        else m.error = QStringLiteral("element type %1").arg(type);
    }
    expect("$EndElements");
    return m;
}

/// The configuration as Palace reads it: its comments stripped.
QJsonObject configOf(const QByteArray& bytes)
{
    QByteArray body;
    for (const QByteArray& l : bytes.split('\n'))
        if (!l.trimmed().startsWith("//")) body += l + '\n';
    return QJsonDocument::fromJson(body).object();
}

/// The faces of the tetrahedra on one only that no triangle covers: the
/// domain's skin left without a boundary.
int bareFaces(const Msh& m)
{
    std::map<std::array<int, 3>, int> count;
    for (const auto& [t, g] : m.tets) {
        static const int local[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
        for (const auto& f : local) {
            std::array<int, 3> k{{t[std::size_t(f[0])], t[std::size_t(f[1])], t[std::size_t(f[2])]}};
            std::sort(k.begin(), k.end());
            ++count[k];
        }
    }
    std::set<std::array<int, 3>> covered;
    for (const auto& [t, g] : m.tris) {
        std::array<int, 3> k = t;
        std::sort(k.begin(), k.end());
        covered.insert(k);
    }
    int bare = 0;
    for (const auto& [k, n] : count)
        if (n == 1 && !covered.count(k)) ++bare;
    return bare;
}

QString microstrip(const QString& metal, const QString& ground = QStringLiteral("perfect"))
{
    return QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
        {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "%2", "sigma": "5.8e7"}},
        {"type": "rf_substrate", "tag": "s", "label": "Alumina", "properties": {"thickness": "254[um]", "epsr": "9.8", "tand": "0.0002"}},
        {"type": "rf_metal", "tag": "m", "label": "M1", "properties": {%1}},
        {"type": "rf_cover", "tag": "c", "label": "Air", "properties": {"height": "500[um]"}}]})")
        .arg(metal, ground);
}

Model modelOf(const QString& stack, const QString& ports, const QString& study = {})
{
    const QString text = QStringLiteral(R"({"format": "qucs-s rf 2.5d 1", "tree": {"type": "rf_model", "tag": "model", "children": [%1,
        {"type": "rf_ports", "tag": "ports", "children": [%2]},
        %3]}})")
                             .arg(stack, ports,
                                  study.isEmpty() ? QStringLiteral(R"({"type": "rf_study", "tag": "study", "properties": {"kind": "full_wave", "from": "1[GHz]", "to": "20[GHz]", "points": "39"}})")
                                                  : study);
    QString why;
    const std::optional<Model> m = Model::fromJson(text.toUtf8(), &why);
    if (!m) qFatal("%s", qPrintable(why));
    return *m;
}

Conductor box(const QString& layer, double x0, double y0, double x1, double y1)
{
    Conductor c;
    c.layer = layer;
    c.outline = QPolygonF(QList<QPointF>{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    return c;
}

const char* const kTwoEdgePorts =
    R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}},
       {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "2000[um]", "y": "0", "facing": "+x"}})";

constexpr double kL = 2000, kW = 240, kH = 254;
/// A metal's name with a double quote in it, as JSON writes it. (Not in a raw
/// string literal: moc cannot read one with an escaped quote in it.)
const QString kQuoted = QStringLiteral("Top \\\"M2\\\"");

} // namespace

class TestRf25Palace : public QObject
{
    Q_OBJECT

private slots:
    // A microstrip of thick, lossy copper: the strip cut out of the mesh, its
    // surface of its conductivity; a port under each end down to the ground.
    void aThickMicrostripIsMeshedWithItsStripCutOut()
    {
        const Model model = modelOf(microstrip(QStringLiteral(R"("thickness": "10[um]", "sigma": "5.8e7")")), QLatin1String(kTwoEdgePorts));
        PalaceOptions options;
        options.margin = 400e-6;
        options.air = 600e-6;
        const PalaceExport e = palaceExportOf(model, {box(QStringLiteral("M1"), 0, -kW / 2, kL, kW / 2)}, options, QStringLiteral("line"),
                                              QStringLiteral("Qucs-S test"));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.ports, 2);
        QCOMPARE(e.box, QRectF(QPointF(-400, -kW / 2 - 400), QPointF(kL + 400, kW / 2 + 400)));
        QVERIFY(std::abs(e.floor) < 1e-9);
        QVERIFY(std::abs(e.ceiling - (kH + 10 + 600)) < 1e-6);
        const Msh m = readMsh(e.mesh);
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        QCOMPARE(int(m.tets.size()), e.tetrahedra);
        QCOMPARE(int(m.nodes.size()), e.nodes);
        // Every tetrahedron turned positive, none flat.
        double volume = 0, least = INFINITY;
        for (const auto& [t, g] : m.tets) {
            const double v = m.volume(t);
            least = std::min(least, v);
            volume += v;
        }
        QVERIFY2(least > 0, qPrintable(QString::number(least)));
        // The box less the strip.
        const double boxVolume = e.box.width() * e.box.height() * (e.ceiling - e.floor);
        QVERIFY2(std::abs(volume - (boxVolume - kL * kW * 10)) < 1e-9 * boxVolume, qPrintable(QString::number(volume - boxVolume)));
        QVERIFY(std::abs(volume - e.volume) < 1e-9 * boxVolume);
        // No face of the domain without its boundary.
        QCOMPARE(bareFaces(m), 0);
        // The boundaries, by name: the ground the floor, the absorbing
        // boundary the sides and the ceiling, the strip its whole surface,
        // each port its rectangle under its face.
        const double floorArea = e.box.width() * e.box.height();
        QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("GND"))) - floorArea) < 1e-9 * floorArea);
        // (Perfect sides, by default; the ceiling absorbing.)
        const double sides = 2 * (e.box.width() + e.box.height()) * (e.ceiling - e.floor);
        QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("absorbing"))) - floorArea) < 1e-9 * floorArea);
        QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("sides"))) - sides) < 1e-9 * sides);
        const double strip = 2 * kL * kW + 2 * (kL + kW) * 10;
        QVERIFY2(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("M1"))) - strip) < 1e-9 * strip, qPrintable(QString::number(m.areaOf(m.tagOf(2, QStringLiteral("M1"))))));
        for (const char* p : {"port P1", "port P2"})
            QVERIFY2(std::abs(m.areaOf(m.tagOf(2, QLatin1String(p))) - kW * kH) < 1e-9 * kW * kH, p);
        // The port rectangles in the planes of the strip's two ends.
        for (const auto& [t, g] : m.tris)
            if (g == m.tagOf(2, QStringLiteral("port P2")))
                for (const int n : t) QVERIFY(std::abs(m.nodes.at(n)[0] - kL) < 1e-9);

        // The configuration.
        const QJsonObject c = configOf(e.config);
        QVERIFY(e.config.startsWith("// Palace driven solve of the RF 2.5D model line, written by Qucs-S test."));
        QCOMPARE(c.value(QStringLiteral("Problem")).toObject().value(QStringLiteral("Type")).toString(), QStringLiteral("Driven"));
        QCOMPARE(c.value(QStringLiteral("Model")).toObject().value(QStringLiteral("Mesh")).toString(), QStringLiteral("line.msh"));
        QCOMPARE(c.value(QStringLiteral("Model")).toObject().value(QStringLiteral("L0")).toDouble(), 1e-6);
        // The air and the alumina, each by its attribute.
        const QJsonArray materials = c.value(QStringLiteral("Domains")).toObject().value(QStringLiteral("Materials")).toArray();
        QCOMPARE(materials.size(), 2);
        for (const QJsonValue& v : materials) {
            const QJsonObject mat = v.toObject();
            const int a = mat.value(QStringLiteral("Attributes")).toArray().at(0).toInt();
            const QString named = m.names.at({3, a});
            QCOMPARE(mat.value(QStringLiteral("Permittivity")).toDouble(), named == QLatin1String("Alumina") ? 9.8 : 1.0);
            QCOMPARE(mat.value(QStringLiteral("LossTan")).toDouble(), named == QLatin1String("Alumina") ? 0.0002 : 0.0);
        }
        const QJsonObject b = c.value(QStringLiteral("Boundaries")).toObject();
        QCOMPARE(b.value(QStringLiteral("PEC")).toObject().value(QStringLiteral("Attributes")).toArray(),
                 (QJsonArray{m.tagOf(2, QStringLiteral("GND")), m.tagOf(2, QStringLiteral("sides"))}));
        const QJsonArray cond = b.value(QStringLiteral("Conductivity")).toArray();
        QCOMPARE(cond.size(), 1);
        QCOMPARE(cond.at(0).toObject().value(QStringLiteral("Attributes")).toArray(), QJsonArray{m.tagOf(2, QStringLiteral("M1"))});
        QCOMPARE(cond.at(0).toObject().value(QStringLiteral("Conductivity")).toDouble(), 5.8e7);
        QCOMPARE(b.value(QStringLiteral("Absorbing")).toObject().value(QStringLiteral("Attributes")).toArray(),
                 QJsonArray{m.tagOf(2, QStringLiteral("absorbing"))});
        // Each port driven in turn, 50 Ohm, from the strip down to the ground.
        const QJsonArray ports = b.value(QStringLiteral("LumpedPort")).toArray();
        QCOMPARE(ports.size(), 2);
        for (int i = 0; i < 2; ++i) {
            const QJsonObject p = ports.at(i).toObject();
            QCOMPARE(p.value(QStringLiteral("Index")).toInt(), i + 1);
            QCOMPARE(p.value(QStringLiteral("Excitation")).toInt(), i + 1);
            QCOMPARE(p.value(QStringLiteral("R")).toDouble(), 50.0);
            QCOMPARE(p.value(QStringLiteral("Direction")).toString(), QStringLiteral("-Z"));
            QCOMPARE(p.value(QStringLiteral("Attributes")).toArray(), QJsonArray{m.tagOf(2, QStringLiteral("port P%1").arg(i + 1))});
        }
        // The study's sweep, adaptive; the solver as asked.
        const QJsonObject solver = c.value(QStringLiteral("Solver")).toObject();
        QCOMPARE(solver.value(QStringLiteral("Order")).toInt(), 2);
        const QJsonObject driven = solver.value(QStringLiteral("Driven")).toObject();
        const QJsonObject sample = driven.value(QStringLiteral("Samples")).toArray().at(0).toObject();
        QCOMPARE(sample.value(QStringLiteral("Type")).toString(), QStringLiteral("Linear"));
        QCOMPARE(sample.value(QStringLiteral("MinFreq")).toDouble(), 1.0);
        QCOMPARE(sample.value(QStringLiteral("MaxFreq")).toDouble(), 20.0);
        QCOMPARE(sample.value(QStringLiteral("NSample")).toInt(), 39);
        QCOMPARE(driven.value(QStringLiteral("AdaptiveTol")).toDouble(), 1e-3);
        QCOMPARE(solver.value(QStringLiteral("Linear")).toObject().value(QStringLiteral("Type")).toString(), QStringLiteral("Default"));
        QCOMPARE(e.portSaid.size(), 2);
        // Absorbing sides, asked for: a quarter wave out at 20 GHz, one
        // absorbing boundary with the ceiling.
        {
            PalaceOptions open = options;
            open.sides = QStringLiteral("absorbing");
            open.margin = -1;
            const PalaceExport a = palaceExportOf(model, {box(QStringLiteral("M1"), 0, -kW / 2, kL, kW / 2)}, open);
            QVERIFY2(a.ok(), qPrintable(a.problems.join(QLatin1Char(' '))));
            QVERIFY(std::abs(a.marginUsed - 299792458.0 / 20e9 / 4 * 1e6) < 1e-6);
            const Msh ma = readMsh(a.mesh);
            QCOMPARE(ma.tagOf(2, QStringLiteral("sides")), -1);
            const double floorA = a.box.width() * a.box.height();
            const double sidesA = 2 * (a.box.width() + a.box.height()) * (a.ceiling - a.floor);
            QVERIFY(std::abs(ma.areaOf(ma.tagOf(2, QStringLiteral("absorbing"))) - (sidesA + floorA)) < 1e-9 * floorA);
            QCOMPARE(bareFaces(ma), 0);
        }
        PalaceOptions wrong = options;
        wrong.sides = QStringLiteral("open");
        QVERIFY(!palaceExportOf(model, {box(QStringLiteral("M1"), 0, -kW / 2, kL, kW / 2)}, wrong).ok());

        // Written: the configuration and the mesh it names beside it.
        QTemporaryDir dir;
        QStringList files;
        QString why;
        QVERIFY2(writePalaceExport(e, dir.filePath(QStringLiteral("line.json")), &files, &why), qPrintable(why));
        QCOMPARE(files, (QStringList{dir.filePath(QStringLiteral("line.json")), dir.filePath(QStringLiteral("line.msh"))}));
        QFile mesh(files.at(1));
        QVERIFY(mesh.open(QIODevice::ReadOnly));
        QCOMPARE(mesh.readAll(), e.mesh);
    }

    // Metals as sheets: each at its bottom where its shape is, its loss of
    // its thickness, the box whole; the floor of a metal ground its
    // conductivity, an open one air.
    // A port on a side that corners on its line split - an outline not
    // healed - is on the whole side, as the solver's face is: its rectangle
    // the strip's width, not the piece its point is on (the review of
    // 1ed66b44).
    void aPortOnASplitSideIsTheWholeSide()
    {
        const Model model = modelOf(microstrip(QStringLiteral(R"("thickness": "10[um]", "sigma": "5.8e7")")), QLatin1String(kTwoEdgePorts));
        Conductor split = box(QStringLiteral("M1"), 0, -kW / 2, kL, kW / 2);
        split.outline = QPolygonF(QList<QPointF>{{0, -kW / 2}, {kL, -kW / 2}, {kL, kW / 2}, {0, kW / 2}, {0, kW / 4}, {0, -kW / 4}});
        PalaceOptions options;
        options.margin = 400e-6;
        options.air = 600e-6;
        const PalaceExport e = palaceExportOf(model, {split}, options, QStringLiteral("split"), QStringLiteral("Qucs-S test"));
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        const Msh m = readMsh(e.mesh);
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        const double port = m.areaOf(m.tagOf(2, QStringLiteral("port P1")));
        QVERIFY2(std::abs(port - kW * kH) < 1e-9 * kW * kH, qPrintable(QString::number(port)));
    }

    void aSheetIsAPerfectSheetAndTheGroundItsKind()
    {
        const Conductor strip = box(QStringLiteral("M1"), 0, -kW / 2, kL, kW / 2);
        PalaceOptions options;
        options.margin = 300e-6;
        options.air = 500e-6;
        options.sheets = true;
        {
            const PalaceExport e = palaceExportOf(model(QStringLiteral("metal")), {strip}, options);
            QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
            QCOMPARE(e.name, QStringLiteral("rf25"));
            const Msh m = readMsh(e.mesh);
            QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
            double volume = 0;
            for (const auto& [t, g] : m.tets) volume += m.volume(t);
            const double boxVolume = e.box.width() * e.box.height() * (e.ceiling - e.floor);
            QVERIFY(std::abs(volume - boxVolume) < 1e-9 * boxVolume);
            QCOMPARE(bareFaces(m), 0);
            QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("M1"))) - kL * kW) < 1e-9 * kL * kW);
            // The sheet on the alumina's top.
            for (const auto& [t, g] : m.tris)
                if (g == m.tagOf(2, QStringLiteral("M1")))
                    for (const int n : t) QVERIFY(std::abs(m.nodes.at(n)[2] - kH) < 1e-9);
            const QJsonObject b = configOf(e.config).value(QStringLiteral("Boundaries")).toObject();
            QCOMPARE(b.value(QStringLiteral("PEC")).toObject().value(QStringLiteral("Attributes")).toArray(), QJsonArray{m.tagOf(2, QStringLiteral("sides"))});
            const QJsonArray cond = b.value(QStringLiteral("Conductivity")).toArray();
            QCOMPARE(cond.size(), 2);
            for (const QJsonValue& v : cond) {
                const QJsonObject o = v.toObject();
                const QString on = m.names.at({2, o.value(QStringLiteral("Attributes")).toArray().at(0).toInt()});
                QCOMPARE(o.value(QStringLiteral("Conductivity")).toDouble(), 5.8e7);
                // (The sheet's two faces on one: of its thickness, µm.)
                QCOMPARE(o.value(QStringLiteral("Thickness")).toDouble(), on == QLatin1String("M1") ? 2.0 : 0.0);
                QVERIFY(on == QLatin1String("M1") || on == QLatin1String("GND"));
            }
            QVERIFY(e.notes.join(QLatin1Char(' ')).contains(QStringLiteral("Each metal is a sheet at its bottom")));
        }
        {
            const PalaceExport e = palaceExportOf(model(QStringLiteral("open")), {strip}, options);
            // (Its ports return to the ground, which is not there.)
            QVERIFY(!e.ok());
            QVERIFY2(e.problems.join(QLatin1Char(' ')).contains(QStringLiteral("No port could be written")), qPrintable(e.problems.join(' ')));
            QVERIFY(e.notes.join(QLatin1Char(' ')).contains(QStringLiteral("no ground or metal under its face")));
        }
    }

    // A coplanar line's port, returning through the grounds beside it: two
    // elements across its gaps, each from the signal out to its ground.
    void aCoplanarPortIsTwoElementsAcrossItsGaps()
    {
        const QString ports = QStringLiteral(
            R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x", "return": "beside"}},
               {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "1000[um]", "y": "0", "facing": "+x", "return": "beside"}})");
        const Model model = modelOf(microstrip(QStringLiteral(R"("thickness": "2[um]")")), ports);
        const QList<Conductor> cpw{box(QStringLiteral("M1"), 0, -31, 1000, 31), box(QStringLiteral("M1"), 0, 40, 1000, 340),
                                   box(QStringLiteral("M1"), 0, -340, 1000, -40)};
        PalaceOptions options;
        options.margin = 300e-6;
        options.air = 500e-6;
        options.sheets = true;
        const PalaceExport e = palaceExportOf(model, cpw, options);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        const Msh m = readMsh(e.mesh);
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        QCOMPARE(bareFaces(m), 0);
        const QJsonArray lumped = configOf(e.config).value(QStringLiteral("Boundaries")).toObject().value(QStringLiteral("LumpedPort")).toArray();
        QCOMPARE(lumped.size(), 2);
        for (const QJsonValue& v : lumped) {
            const QJsonObject p = v.toObject();
            const QJsonArray elements = p.value(QStringLiteral("Elements")).toArray();
            QCOMPARE(elements.size(), 2);
            QStringList ways;
            for (const QJsonValue& el : elements) {
                const QJsonObject o = el.toObject();
                ways << o.value(QStringLiteral("Direction")).toString();
                // Each the 9 µm gap square, in the strip's plane, its
                // direction out of the signal towards its own side's ground.
                const int a = o.value(QStringLiteral("Attributes")).toArray().at(0).toInt();
                QVERIFY2(std::abs(m.areaOf(a) - 81) < 1e-9, qPrintable(QString::number(m.areaOf(a))));
                double y = 0;
                for (const auto& [t, g] : m.tris)
                    if (g == a)
                        for (const int n : t) {
                            QVERIFY(std::abs(m.nodes.at(n)[2] - kH) < 1e-9);
                            y += m.nodes.at(n)[1];
                        }
                QCOMPARE(ways.last(), y > 0 ? QStringLiteral("+Y") : QStringLiteral("-Y"));
            }
            ways.sort();
            QCOMPARE(ways, (QStringList{QStringLiteral("+Y"), QStringLiteral("-Y")}));
        }
    }

    // An internal port from the ground up to a pad, and a lumped element
    // across a gap: the element an undriven port of its R and C in parallel,
    // numbered after the measuring ports; one in series not written, said.
    void anInternalPortAndALumpedElement()
    {
        const QString ports = QStringLiteral(
            R"({"type": "rf_internal_port", "tag": "p1", "label": "In", "properties": {"x": "50[um]", "y": "0", "from": "ground", "to": "M1", "width": "40[um]"}},
               {"type": "rf_internal_port", "tag": "r1", "label": "R1", "properties": {"x": "300[um]", "y": "0", "from": "ground", "to": "M1", "height": "30[um]", "element": "lumped", "r": "75", "c": "1e-13", "topology": "parallel"}},
               {"type": "rf_internal_port", "tag": "r2", "label": "R2", "properties": {"x": "300[um]", "y": "0", "from": "ground", "to": "M1", "element": "lumped", "r": "75", "l": "1e-9"}})");
        const Model model = modelOf(microstrip(QStringLiteral(R"("thickness": "2[um]")")), ports);
        PalaceOptions options;
        options.margin = 300e-6;
        options.air = 500e-6;
        options.sheets = true;
        const PalaceExport e = palaceExportOf(model, {box(QStringLiteral("M1"), 0, -50, 400, 50)}, options);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.ports, 1);
        QCOMPARE(e.lumped, 1);
        QVERIFY(e.notes.join(QLatin1Char(' ')).contains(QStringLiteral("R2 is a lumped element Palace cannot take")));
        const Msh m = readMsh(e.mesh);
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        QCOMPARE(bareFaces(m), 0);
        // The port 40 µm across x, the element 30 across y, each the whole
        // alumina high.
        QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("port In"))) - 40 * kH) < 1e-9 * 40 * kH);
        QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("port R1"))) - 30 * kH) < 1e-9 * 30 * kH);
        for (const auto& [t, g] : m.tris)
            if (g == m.tagOf(2, QStringLiteral("port R1")))
                for (const int n : t) QVERIFY(std::abs(m.nodes.at(n)[0] - 300) < 1e-9);
        const QJsonArray lumped = configOf(e.config).value(QStringLiteral("Boundaries")).toObject().value(QStringLiteral("LumpedPort")).toArray();
        QCOMPARE(lumped.size(), 2);
        const QJsonObject port = lumped.at(0).toObject(), element = lumped.at(1).toObject();
        QCOMPARE(port.value(QStringLiteral("Excitation")).toInt(), 1);
        QCOMPARE(element.value(QStringLiteral("Index")).toInt(), 2);
        QVERIFY(!element.contains(QStringLiteral("Excitation")));
        QCOMPARE(element.value(QStringLiteral("R")).toDouble(), 75.0);
        QCOMPARE(element.value(QStringLiteral("C")).toDouble(), 1e-13);
        QVERIFY(!element.contains(QStringLiteral("L")));
    }

    // Two metals and a via between them, the lower metal's room the
    // dielectric over it: thick, each cut out and the via from the lower's
    // top to the upper's bottom; as sheets, the via from sheet to sheet. An
    // edge port on the upper metal returns down to the lower one under it.
    void aViaJoinsItsMetalsAndAPortReturnsToTheMetalUnder()
    {
        const QString stack = QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
            {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "perfect"}},
            {"type": "rf_substrate", "tag": "s", "label": "Sub", "properties": {"thickness": "100[um]", "epsr": "4"}},
            {"type": "rf_metal", "tag": "m1", "label": "M1", "properties": {"thickness": "2[um]", "sigma": "5.8e7"}},
            {"type": "rf_dielectric", "tag": "d", "label": "ILD", "properties": {"thickness": "10[um]", "epsr": "3.9"}},
            {"type": "rf_via", "tag": "v", "label": "V1", "properties": {"from": "M1", "to": "M2", "sigma": "5.8e7"}},
            {"type": "rf_metal", "tag": "m2", "label": "M2", "properties": {"thickness": "2[um]", "sigma": "5.8e7"}},
            {"type": "rf_cover", "tag": "c", "label": "Air", "properties": {"height": "200[um]"}}]})");
        const Model model = modelOf(stack, QStringLiteral(R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M2", "x": "0", "y": "50[um]", "facing": "-x"}})"));
        const QList<Conductor> shapes{box(QStringLiteral("M1"), 0, 0, 100, 100), box(QStringLiteral("M2"), 0, 0, 100, 100),
                                      box(QStringLiteral("V1"), 40, 40, 60, 60)};
        for (const bool sheets : {false, true}) {
            PalaceOptions options;
            options.margin = 100e-6;
            options.air = 200e-6;
            options.sheets = sheets;
            const PalaceExport e = palaceExportOf(model, shapes, options);
            QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
            const Msh m = readMsh(e.mesh);
            QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
            QCOMPARE(bareFaces(m), 0);
            double volume = 0;
            for (const auto& [t, g] : m.tets) {
                QVERIFY(m.volume(t) > 0);
                volume += m.volume(t);
            }
            const double boxVolume = e.box.width() * e.box.height() * (e.ceiling - e.floor);
            const double cut = sheets ? 20 * 20 * 12 : 2 * 100 * 100 * 2 + 20 * 20 * 10;
            QVERIFY2(std::abs(volume - (boxVolume - cut)) < 1e-9 * boxVolume, qPrintable(QString::number(boxVolume - volume)));
            const double m1 = m.areaOf(m.tagOf(2, QStringLiteral("M1"))), m2 = m.areaOf(m.tagOf(2, QStringLiteral("M2")));
            const double v1 = m.areaOf(m.tagOf(2, QStringLiteral("V1")));
            if (sheets) {
                // Each sheet but where the via stands on it or under it; the
                // via its sides and its two ends.
                QVERIFY2(std::abs(m1 - (10000 - 400)) < 1e-6 && std::abs(m2 - (10000 - 400)) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(m1).arg(m2)));
                QVERIFY2(std::abs(v1 - (4 * 20 * 12 + 2 * 400)) < 1e-6, qPrintable(QString::number(v1)));
            } else {
                // Each metal all round but where the via joins it; the via its sides.
                QVERIFY2(std::abs(m1 - (2 * 10000 - 400 + 4 * 100 * 2)) < 1e-6 && std::abs(m2 - m1) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(m1).arg(m2)));
                QVERIFY2(std::abs(v1 - 4 * 20 * 10) < 1e-6, qPrintable(QString::number(v1)));
                // The lower metal's room beside its shapes the dielectric over it.
                const int ild = m.tagOf(3, QStringLiteral("ILD"));
                QVERIFY(ild > 0);
                for (const auto& [t, g] : m.tets) {
                    double z = 0;
                    for (const int n : t) z += m.nodes.at(n)[2] / 4;
                    if (z > 100 && z < 102) QCOMPARE(g, ild);
                }
            }
            // The port down from M2's face to M1's top (or its sheet).
            QVERIFY2(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("port P1"))) - 100 * (sheets ? 12 : 10)) < 1e-6,
                     qPrintable(QString::number(m.areaOf(m.tagOf(2, QStringLiteral("port P1"))))));
            QVERIFY2(e.portSaid.join(QLatin1Char(' ')).contains(QStringLiteral("down to M1")), qPrintable(e.portSaid.join(' ')));
        }
        // A via farm merged into its footprint, 40 µm square, a quarter of it
        // metal (Conductor::share - P44 of the open items of 8 November):
        // written as its metal thinned in place, the 20 µm square about its
        // middle that the via above is (within a per cent), not as solid
        // metal over the footprint - and said.
        Conductor farm = box(QStringLiteral("V1"), 30, 30, 70, 70);
        farm.share = 0.25;
        PalaceOptions options;
        options.margin = 100e-6;
        options.air = 200e-6;
        const PalaceExport e = palaceExportOf(model, {shapes.at(0), shapes.at(1), farm}, options);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        const Msh m = readMsh(e.mesh);
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        const double v1 = m.areaOf(m.tagOf(2, QStringLiteral("V1")));
        QVERIFY2(std::abs(v1 / (4 * 20 * 10) - 1) < 0.01, qPrintable(QString::number(v1)));
        QVERIFY2(e.notes.join(QLatin1Char(' ')).contains(QStringLiteral("merged via farm(s) written as their metal thinned in place")),
                 qPrintable(e.notes.join(QLatin1Char(' '))));
    }

    // An internal port whose To metal is under its From, the metals thick:
    // from the lower's top up to the upper's bottom, its plus below - and a
    // metal named with a double quote, its physical group's name kept whole.
    void anInternalPortDownToALowerMetalAndAQuotedName()
    {
        const QString stack = QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
            {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "perfect"}},
            {"type": "rf_substrate", "tag": "s", "label": "Sub", "properties": {"thickness": "100[um]", "epsr": "4"}},
            {"type": "rf_metal", "tag": "m1", "label": "M1", "properties": {"thickness": "2[um]", "sigma": "5.8e7"}},
            {"type": "rf_dielectric", "tag": "d", "label": "ILD", "properties": {"thickness": "10[um]", "epsr": "3.9"}},
            {"type": "rf_metal", "tag": "m2", "label": "%1", "properties": {"thickness": "2[um]", "sigma": "5.8e7"}},
            {"type": "rf_cover", "tag": "c", "label": "Air", "properties": {"height": "200[um]"}}]})")
                                  .arg(kQuoted);
        const Model model = modelOf(stack, QStringLiteral(R"({"type": "rf_internal_port", "tag": "p1", "label": "P1", "properties": {"x": "50[um]", "y": "50[um]", "from": "%1", "to": "M1", "width": "20[um]"}})")
                                               .arg(kQuoted));
        const QString top = QStringLiteral("Top \"M2\"");
        PalaceOptions options;
        options.margin = 100e-6;
        options.air = 200e-6;
        const PalaceExport e = palaceExportOf(model, {box(QStringLiteral("M1"), 0, 0, 100, 100), box(top, 0, 0, 100, 100)}, options);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' ')) + e.notes.join(QLatin1Char(' '))));
        const Msh m = readMsh(e.mesh);
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        QCOMPARE(bareFaces(m), 0);
        // M1's top at 102 µm up to the upper metal's bottom at 112.
        const int port = m.tagOf(2, QStringLiteral("port P1"));
        QVERIFY2(std::abs(m.areaOf(port) - 20 * 10) < 1e-6, qPrintable(QString::number(m.areaOf(port))));
        for (const auto& [t, g] : m.tris)
            if (g == port)
                for (const int n : t) QVERIFY(m.nodes.at(n)[2] > 102 - 1e-9 && m.nodes.at(n)[2] < 112 + 1e-9);
        const QJsonObject p = configOf(e.config).value(QStringLiteral("Boundaries")).toObject().value(QStringLiteral("LumpedPort")).toArray().at(0).toObject();
        QCOMPARE(p.value(QStringLiteral("Direction")).toString(), QStringLiteral("+Z"));
        // The quoted name, its quotes made single: the physical names read whole.
        QVERIFY(m.tagOf(2, QStringLiteral("Top 'M2'")) > 0);
        QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("Top 'M2'"))) - (2 * 10000 + 4 * 100 * 2)) < 1e-6);
    }

    // A box: the lid's side walls and the lid perfect, nothing absorbing.
    void aShieldedLineIsClosedByItsBox()
    {
        const QString stack = QStringLiteral(R"({"type": "rf_stack", "tag": "stack", "children": [
            {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "perfect"}},
            {"type": "rf_substrate", "tag": "s", "label": "Alumina", "properties": {"thickness": "254[um]", "epsr": "9.8"}},
            {"type": "rf_metal", "tag": "m", "label": "M1", "properties": {"thickness": "2[um]"}},
            {"type": "rf_cover", "tag": "c", "label": "Lid", "properties": {"height": "1000[um]", "kind": "perfect", "walls": true,
             "box_x0": "-1000[um]", "box_y0": "-1000[um]", "box_x1": "3000[um]", "box_y1": "1000[um]"}}]})");
        const Model model = modelOf(stack, QLatin1String(kTwoEdgePorts));
        const PalaceExport e = palaceExportOf(model, {box(QStringLiteral("M1"), 0, -kW / 2, kL, kW / 2)});
        QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
        QCOMPARE(e.box, QRectF(QPointF(-1000, -1000), QPointF(3000, 1000)));
        // (The lid over the cover's air, which is over the strip's room.)
        QVERIFY(std::abs(e.ceiling - (kH + 2 + 1000)) < 1e-6);
        const Msh m = readMsh(e.mesh);
        QVERIFY2(m.error.isEmpty(), qPrintable(m.error));
        QCOMPARE(bareFaces(m), 0);
        QCOMPARE(m.tagOf(2, QStringLiteral("absorbing")), -1);
        const QJsonObject b = configOf(e.config).value(QStringLiteral("Boundaries")).toObject();
        QVERIFY(!b.contains(QStringLiteral("Absorbing")));
        const QJsonArray pec = b.value(QStringLiteral("PEC")).toObject().value(QStringLiteral("Attributes")).toArray();
        for (const char* n : {"GND", "Lid", "walls"}) QVERIFY2(pec.contains(m.tagOf(2, QLatin1String(n))), n);
        // A shape past the walls: refused, said.
        const PalaceExport past = palaceExportOf(model, {box(QStringLiteral("M1"), 0, -kW / 2, 3500, kW / 2)});
        QVERIFY(!past.ok());
        QVERIFY2(past.problems.join(QLatin1Char(' ')).contains(QStringLiteral("past the lid's side walls")), qPrintable(past.problems.join(' ')));
        // The strip of its conductivity, as thick as it is (cut out).
        QCOMPARE(b.value(QStringLiteral("Conductivity")).toArray().at(0).toObject().value(QStringLiteral("Attributes")).toArray(),
                 QJsonArray{m.tagOf(2, QStringLiteral("M1"))});
        const double strip = 2 * kL * kW + 2 * (kL + kW) * 2;
        QVERIFY(std::abs(m.areaOf(m.tagOf(2, QStringLiteral("M1"))) - strip) < 1e-9 * strip);
    }

    // What cannot be written is said: no study, no shapes, a mesh too large.
    void whatCannotBeWrittenIsSaid()
    {
        const Conductor strip = box(QStringLiteral("M1"), 0, -kW / 2, kL, kW / 2);
        const Model model = modelOf(microstrip(QStringLiteral(R"("thickness": "2[um]")")), QLatin1String(kTwoEdgePorts));
        QVERIFY(!palaceExportOf(model, {}).ok());
        PalaceOptions tiny;
        tiny.edges = 0.05e-6;
        tiny.largest = 0.1e-6;
        QElapsedTimer clock;
        clock.start();
        const PalaceExport big = palaceExportOf(model, {strip}, tiny);
        QVERIFY(!big.ok());
        // (Refused before its plan of some 1e8 triangles is made.)
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY2(big.problems.join(QLatin1Char(' ')).contains(QStringLiteral("tetrahedra or more, past the")), qPrintable(big.problems.join(' ')));
        PalaceOptions badOrder;
        badOrder.order = 7;
        QVERIFY(!palaceExportOf(model, {strip}, badOrder).ok());
        // A port on no edge: said, and the other written.
        const QString ports = QStringLiteral(
            R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}},
               {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M2", "x": "2000[um]", "y": "0", "facing": "+x"}})");
        PalaceOptions options;
        options.margin = 300e-6;
        options.air = 500e-6;
        options.sheets = true;
        const PalaceExport one = palaceExportOf(modelOf(microstrip(QStringLiteral(R"("thickness": "2[um]")")), ports), {strip}, options);
        QVERIFY2(one.ok(), qPrintable(one.problems.join(' ')));
        QCOMPARE(one.ports, 1);
        QVERIFY(one.notes.join(QLatin1Char(' ')).contains(QStringLiteral("P2 is not written")));
        // One off its edge, 500 µm past the strip's end, as the solve refuses
        // it: not written - the nearest edge however far was.
        const QString off = QStringLiteral(
            R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}},
               {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "2500[um]", "y": "0", "facing": "+x"}})");
        const PalaceExport away = palaceExportOf(modelOf(microstrip(QStringLiteral(R"("thickness": "2[um]")")), off), {strip}, options);
        QVERIFY2(away.ok(), qPrintable(away.problems.join(' ')));
        QCOMPARE(away.ports, 1);
        QVERIFY(away.notes.join(QLatin1Char(' ')).contains(QStringLiteral("P2 is not written")));
        // One on the strip's end's corner, no facing given: not written, and
        // said as the solve says it - give it its facing; it was said as no
        // edge facing its way (a bug hunt's second hour).
        const QString corner = QStringLiteral(
            R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}},
               {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "%1[um]", "y": "%2[um]"}})")
                                   .arg(kL).arg(kW / 2);
        const PalaceExport atCorner = palaceExportOf(modelOf(microstrip(QStringLiteral(R"("thickness": "2[um]")")), corner), {strip}, options);
        QCOMPARE(atCorner.ports, 1);
        QVERIFY2(atCorner.notes.join(QLatin1Char(' ')).contains(QStringLiteral("P2 is not written: it is on a corner of a shape on M1")),
                 qPrintable(atCorner.notes.join(" | ")));
    }

    // Runs: opt in with QUCS_RF25_PALACE_OUT=<folder>. The
    // thick microstrip 2 and 4 mm long, every option chosen, each in a folder
    // of its own to run Palace in - the two lines' difference its line alone
    // -, and the same with Metals: sheets ("line2s", "line4s").
    void linesForPalace()
    {
        const QString folder = qEnvironmentVariable("QUCS_RF25_PALACE_OUT");
        if (folder.isEmpty()) QSKIP("QUCS_RF25_PALACE_OUT names a folder for the runs");
        for (const double length : {2000.0, 4000.0}) {
            const QString ports = QStringLiteral(
                R"({"type": "rf_edge_port", "tag": "p1", "label": "P1", "properties": {"conductor": "M1", "x": "0", "y": "0", "facing": "-x"}},
                   {"type": "rf_edge_port", "tag": "p2", "label": "P2", "properties": {"conductor": "M1", "x": "%1[um]", "y": "0", "facing": "+x"}})")
                                      .arg(length);
            const Model model = modelOf(microstrip(QStringLiteral(R"("thickness": "10[um]", "sigma": "5.8e7")")), ports,
                                        QStringLiteral(R"({"type": "rf_study", "tag": "study", "properties": {"kind": "full_wave", "from": "2[GHz]", "to": "20[GHz]", "points": "10"}})"));
            // (Each as thick as it is, and as a sheet: RF 2.5D's metal.)
            for (const bool sheets : {false, true}) {
                const QString name = QStringLiteral("line%1%2").arg(int(length / 1000)).arg(sheets ? QStringLiteral("s") : QString());
                PalaceOptions options;
                options.sheets = sheets;
                const PalaceExport e = palaceExportOf(model, {box(QStringLiteral("M1"), 0, -kW / 2, length, kW / 2)}, options, name);
                QVERIFY2(e.ok(), qPrintable(e.problems.join(QLatin1Char(' '))));
                const QDir dir(QDir(folder).filePath(name));
                QDir().mkpath(dir.path());
                QString why;
                QVERIFY2(writePalaceExport(e, dir.filePath(name + QStringLiteral(".json")), nullptr, &why), qPrintable(why));
                qInfo("%s: %s", qPrintable(name), qPrintable(e.notes.join(QLatin1Char(' '))));
            }
        }
    }

private:
    static Model model(const QString& ground)
    {
        return modelOf(microstrip(QStringLiteral(R"("thickness": "2[um]", "sigma": "5.8e7")"), ground), QLatin1String(kTwoEdgePorts));
    }
};

QTEST_GUILESS_MAIN(TestRf25Palace)
#include "test_rf25_palace.moc"
