/*
 * test_fem_layout.cpp - the multiphysics solver's layouts: a GDSII cell
 *                       (written here, record by record) and an OASIS one
 *                       (written by gdstk) read flattened - their placements,
 *                       a layer's shapes made one - seen from above as
 *                       objects, and sliced along a cut line into a
 *                       cross-section of its layer stack; heat through it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_import.h"
#include "fem_results.h"

#include <QTemporaryDir>
#include <QtTest>

#include <gdstk/gdstk.hpp>
#include <zlib.h>

#include <cmath>

using namespace qucs_s::fem;

// gdstk's Vec2 is a union of {x, y} and others: {1, 2} is one, braces or not.
#pragma GCC diagnostic ignored "-Wmissing-braces"

namespace {

/// A GDSII file, record by record.
class Gds
{
public:
    Gds()
    {
        record(0x0002, int16s({600}));
        record(0x0102, int16s(std::vector<int>(12, 0)));
        record(0x0206, ascii("LIB"));
        record(0x0305, reals({1e-3, 1e-9}));   // a database unit: 1 nm
    }
    void beginCell(const char* name)
    {
        record(0x0502, int16s(std::vector<int>(12, 0)));
        record(0x0606, ascii(name));
    }
    void endCell() { record(0x0700, {}); }
    /// A rectangle (µm) on layer/datatype.
    void box(int layer, int datatype, double x0, double y0, double x1, double y1)
    {
        record(0x0800, {});
        record(0x0D02, int16s({layer}));
        record(0x0E02, int16s({datatype}));
        record(0x1003, int32s({nm(x0), nm(y0), nm(x1), nm(y0), nm(x1), nm(y1), nm(x0), nm(y1), nm(x0), nm(y0)}));
        record(0x1100, {});
    }
    void place(const char* cell, double x, double y)
    {
        record(0x0A00, {});
        record(0x1206, ascii(cell));
        record(0x1003, int32s({nm(x), nm(y)}));
        record(0x1100, {});
    }
    QByteArray finish()
    {
        record(0x0400, {});
        return a_bytes;
    }

private:
    static int nm(double um) { return int(std::lround(um * 1000)); }
    void record(int type, const QByteArray& data)
    {
        const int n = 4 + int(data.size());
        a_bytes.append(char(n >> 8)).append(char(n & 0xff)).append(char(type >> 8)).append(char(type & 0xff)).append(data);
    }
    static QByteArray int16s(const std::vector<int>& v)
    {
        QByteArray b;
        for (int x : v) b.append(char((x >> 8) & 0xff)).append(char(x & 0xff));
        return b;
    }
    static QByteArray int32s(const std::vector<int>& v)
    {
        QByteArray b;
        for (int x : v) b.append(char((x >> 24) & 0xff)).append(char((x >> 16) & 0xff)).append(char((x >> 8) & 0xff)).append(char(x & 0xff));
        return b;
    }
    static QByteArray ascii(const char* s)
    {
        QByteArray b(s);
        if (b.size() % 2) b.append('\0');
        return b;
    }
    /// GDSII's 8-byte reals: sign, a 7-bit exponent of 16 (excess 64), a
    /// 56-bit fraction.
    static QByteArray reals(const std::vector<double>& v)
    {
        QByteArray b;
        for (double x : v) {
            int e = 64;
            double m = std::abs(x);
            while (m >= 1) {
                m /= 16;
                ++e;
            }
            while (m > 0 && m < 1.0 / 16) {
                m *= 16;
                --e;
            }
            auto f = quint64(std::llround(m * std::pow(2.0, 56)));
            b.append(char((x < 0 ? 0x80 : 0) | e));
            for (int i = 6; i >= 0; --i) b.append(char((f >> (8 * i)) & 0xff));
        }
        return b;
    }
    QByteArray a_bytes;
};

/// A waveguide (1/0) under a heater (11/0), placed twice in TOP, 10 µm
/// apart; on 2/0 two boxes that overlap (one shape when made one).
QByteArray heaterLayout()
{
    Gds g;
    g.beginCell("WG");
    g.box(1, 0, -0.25, -5, 0.25, 5);
    g.box(11, 0, -1, -4, 1, 4);
    g.endCell();
    g.beginCell("TOP");
    g.place("WG", 0, 0);
    g.place("WG", 10, 0);
    g.box(2, 0, -20, -6, -10, 6);
    g.box(2, 0, -12, -6, -5, 6);
    g.endCell();
    return g.finish();
}

Model modelOf(const QByteArray& json, const QString& file)
{
    QString why;
    std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    m->fileName = file;
    return *m;
}

/// The area (the geometry's unit²) of the domains whose smallest object is
/// \a object.
double areaOf(const Topology& t, const QString& object)
{
    double a = 0;
    for (const Topology::Domain& d : t.domains)
        if (t.objectNames.value(d.owner) == object) a += d.area;
    return a;
}

} // namespace

class TestFemLayout : public QObject
{
    Q_OBJECT

private slots:
    void readFlattened()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("heater.gds"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(heaterLayout());
        f.close();
        QVERIFY(isLayoutPath(path));
        const LayoutShapes s = readLayoutShapes(path);
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QCOMPARE(s.cell, QStringLiteral("TOP"));
        QCOMPARE(s.cells.size(), 2);
        QCOMPARE(int(s.layers.size()), 3);
        // Both placements of the waveguide.
        const LayoutShapes::Layer* wg = s.layer({1, 0});
        QVERIFY(wg);
        QCOMPARE(int(wg->loops.size()), 2);
        // The two boxes of 2/0 one shape, counterclockwise.
        const LayoutShapes::Layer* pad = s.layer({2, 0});
        QVERIFY(pad);
        QCOMPARE(int(pad->loops.size()), 1);
        double area = 0;
        for (std::size_t i = 0; i < pad->loops[0].size(); ++i) {
            const QPointF a = pad->loops[0][i], b = pad->loops[0][(i + 1) % pad->loops[0].size()];
            area += (a.x() * b.y() - b.x() * a.y()) / 2;
        }
        QVERIFY(std::abs(area - 15 * 12) < 1e-6);
        // Layers chosen; a cell by name; a window.
        QCOMPARE(int(readLayoutShapes(path, {}, QStringLiteral("11/*")).layers.size()), 1);
        QCOMPARE(readLayoutShapes(path, QStringLiteral("WG")).cell, QStringLiteral("WG"));
        QVERIFY(readLayoutShapes(path, QStringLiteral("NONE")).error.contains(QStringLiteral("no cell NONE")));
        QCOMPARE(int(readLayoutShapes(path, {}, QStringLiteral("1/0"), QRectF(5, -1, 10, 2)).layer({1, 0})->loops.size()), 1);
        QVERIFY(!readLayoutShapes(path, {}, QStringLiteral("x/y")).error.isEmpty());
        // Gzipped, the same.
        const QString gz = dir.filePath(QStringLiteral("heater.gds.gz"));
        gzFile z = gzopen(QFile::encodeName(gz).constData(), "wb");
        const QByteArray bytes = heaterLayout();
        gzwrite(z, bytes.constData(), unsigned(bytes.size()));
        gzclose(z);
        const LayoutShapes zs = readLayoutShapes(gz);
        QVERIFY2(zs.error.isEmpty(), qPrintable(zs.error));
        QCOMPARE(int(zs.layers.size()), 3);
        // Not a layout: said.
        const QString junk = dir.filePath(QStringLiteral("junk.gds"));
        QFile j(junk);
        QVERIFY(j.open(QIODevice::WriteOnly));
        j.write("not a layout at all");
        j.close();
        QVERIFY(readLayoutShapes(junk).error.contains(QStringLiteral("not a GDSII")));
    }

    void readOasis()
    {
        // An OASIS file, as gdstk writes one: a cell placed twice, a box of
        // its own.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cells.oas"));
        {
            gdstk::Library lib = {};
            lib.init("LIB", 1e-6, 1e-9);
            auto* pad = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
            pad->init("PAD");
            lib.cell_array.append(pad);
            auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
            *p = gdstk::rectangle(gdstk::Vec2{0, 0}, gdstk::Vec2{2, 3}, gdstk::make_tag(2, 0));
            pad->polygon_array.append(p);
            auto* top = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
            top->init("TOP");
            lib.cell_array.append(top);
            auto* box = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
            *box = gdstk::rectangle(gdstk::Vec2{-10, -1}, gdstk::Vec2{10, 1}, gdstk::make_tag(1, 0));
            top->polygon_array.append(box);
            for (double x : {-5.0, 5.0}) {
                auto* r = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
                r->init(pad);
                r->origin = gdstk::Vec2{x, 4};
                top->reference_array.append(r);
            }
            QCOMPARE(lib.write_oas(QFile::encodeName(path).constData(), 0, 6, OASIS_CONFIG_DETECT_ALL), gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        QVERIFY(isLayoutPath(path));
        const LayoutShapes s = readLayoutShapes(path);
        QVERIFY2(s.error.isEmpty(), qPrintable(s.error));
        QCOMPARE(s.cell, QStringLiteral("TOP"));
        QCOMPARE(int(s.layers.size()), 2);
        const LayoutShapes::Layer* pads = s.layer({2, 0});
        QVERIFY(pads);
        QCOMPARE(int(pads->loops.size()), 2);
        // Cut across the pads: two stretches of 2 µm, where they were placed.
        const auto spans = cutIntervals(pads->loops, QPointF(-10, 5), QPointF(10, 5));
        QCOMPARE(int(spans.size()), 2);
        QVERIFY(std::abs(spans[0].first - 5) < 1e-6 && std::abs(spans[0].second - 7) < 1e-6);
        QVERIFY(std::abs(spans[1].first - 15) < 1e-6 && std::abs(spans[1].second - 17) < 1e-6);
    }

    void cutIntervalsAlongALine()
    {
        // A square with a square hole: in, out, in.
        const std::vector<std::vector<QPointF>> loops = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{3, 3}, {3, 7}, {7, 7}, {7, 3}}};
        const auto spans = cutIntervals(loops, QPointF(-5, 5), QPointF(15, 5));
        QCOMPARE(int(spans.size()), 2);
        QVERIFY(std::abs(spans[0].first - 5) < 1e-9 && std::abs(spans[0].second - 8) < 1e-9);
        QVERIFY(std::abs(spans[1].first - 12) < 1e-9 && std::abs(spans[1].second - 15) < 1e-9);
        // Starting inside.
        const auto inside = cutIntervals(loops, QPointF(1, 1), QPointF(1, 20));
        QCOMPARE(int(inside.size()), 1);
        QVERIFY(std::abs(inside[0].first) < 1e-9 && std::abs(inside[0].second - 9) < 1e-9);
        QVERIFY(cutIntervals(loops, QPointF(20, 0), QPointF(30, 0)).empty());
    }

    void importedFromAbove()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("heater.gds")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(heaterLayout());
        f.close();
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um", "geometry": [
            {"type": "import", "label": "chip", "file": "heater.gds", "layers": "1/0, 2/0"}]}],
          "studies": []})json";
        const Model model = modelOf(json, dir.filePath(QStringLiteral("top.qfem")));
        const auto params = evaluateParameters(model);
        const GeometryBuild g = buildGeometry(model, *params);
        QVERIFY2(g.ok(), qPrintable(QStringList(g.errors.values()).join(QLatin1String("; "))));
        QVERIFY(g.files.contains(QFileInfo(f.fileName()).absoluteFilePath()));
        QVERIFY(g.topology->objectNames.contains(QStringLiteral("chip_1_0")));
        QVERIFY(g.topology->objectNames.contains(QStringLiteral("chip_2_0")));
        QVERIFY(std::abs(areaOf(*g.topology, QStringLiteral("chip_1_0")) - 10) < 1e-6);
        QVERIFY(std::abs(areaOf(*g.topology, QStringLiteral("chip_2_0")) - 180) < 1e-6);
        // In mm the same, a thousandth.
        Model mm = model;
        mm.component().props.insert(QStringLiteral("unit"), QStringLiteral("mm"));
        const GeometryBuild g2 = buildGeometry(mm, *evaluateParameters(mm));
        QVERIFY(g2.ok());
        QVERIFY(std::abs(areaOf(*g2.topology, QStringLiteral("chip_2_0")) - 180e-6) < 1e-12);
    }

    void crossSectionWithHeat()
    {
        // The two waveguides' cross-section: a buried oxide, the waveguides in
        // a cladding, heaters above - and heat from the heaters through it.
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("heater.gds")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(heaterLayout());
        f.close();
        const QByteArray json = R"json({"format": "qucs-s multiphysics 1",
          "components": [{"unit": "um", "geometry": [
            {"type": "crosssection", "label": "slice", "file": "heater.gds", "start": ["-25", "0"], "end": ["15", "0"],
             "stack": [["box", "", "-2", "2"], ["cladding", "", "0", "3"], ["wg", "1/0", "0", "0.22"],
                       ["heater", "11/0", "1.5", "0.1"], ["pads", "2/0", "3", "1"], ["none", "5/0", "4", "1"]]}],
            "materials": [{"material": "Silicon dioxide", "selection": {"only": ["box", "cladding"]}},
                          {"material": "Silicon", "selection": {"only": ["wg"]}},
                          {"label": "TiN", "k": "20[W/(m*K)]", "selection": {"only": ["heater"]}},
                          {"material": "Aluminium", "selection": {"only": ["pads"]}}],
            "physics": [{"type": "heat", "tag": "ht", "features": [
              {"type": "source", "kind": "power", "P0": "20[W]", "selection": {"only": ["heater"]}},
              {"type": "temperature", "T0": "293.15[K]", "selection": {"of": ["box"], "side": "bottom"}}]}],
            "mesh": {"size": "fine"}}],
          "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})json";
        const Model model = modelOf(json, dir.filePath(QStringLiteral("slice.qfem")));
        const auto params = evaluateParameters(model);
        const GeometryBuild g = buildGeometry(model, *params);
        QVERIFY2(g.ok(), qPrintable(QStringList(g.errors.values()).join(QLatin1String("; "))));
        const Topology& t = *g.topology;
        // The waveguides 0.5 × 0.22 each, the heaters 2 × 0.1, the pads where
        // the cut crosses them: 15 µm wide.
        QVERIFY(std::abs(areaOf(t, QStringLiteral("wg")) - 2 * 0.5 * 0.22) < 1e-9);
        QVERIFY(std::abs(areaOf(t, QStringLiteral("heater")) - 2 * 2 * 0.1) < 1e-9);
        QVERIFY(std::abs(areaOf(t, QStringLiteral("pads")) - 15) < 1e-9);
        QVERIFY(std::abs(areaOf(t, QStringLiteral("box")) - 80) < 1e-9);
        // Where they are: x from the cut's start.
        const QRectF b = t.bounds;
        QVERIFY(std::abs(b.left()) < 1e-9 && std::abs(b.right() - 40) < 1e-9);
        int wgDomain = -1;
        for (int d = 0; d < int(t.domains.size()); ++d)
            if (t.objectNames.value(t.domains[std::size_t(d)].owner) == QLatin1String("wg") && t.domains[std::size_t(d)].bounds.left() < 30)
                wgDomain = d;
        QVERIFY(wgDomain >= 0);
        QVERIFY(std::abs(t.domains[std::size_t(wgDomain)].bounds.left() - 24.75) < 1e-9);
        // A layer the cut misses: said, not an error.
        QVERIFY(g.warnings.values().join(QLatin1Char(' ')).contains(QStringLiteral("crosses nothing of layer 5/0")));
        // Heat: the waveguides warmed by the heaters above them (20 mW per
        // millimetre of the two: 20 W over the model's metre).
        const StudyRun run = runStudy(model, params, g.topology, nullptr, QString());
        QVERIFY2(run.ok(), qPrintable(run.error));
        const Solution& s = *run.solutions.back();
        const Locator locator(s.mesh());
        const std::optional<double> twg = evaluateAt(s, locator, QStringLiteral("T"), QPointF(25, 0.11));
        const std::optional<double> tfar = evaluateAt(s, locator, QStringLiteral("T"), QPointF(2, 0.11));
        QVERIFY(twg && tfar);
        QVERIFY2(*twg > *tfar + 1, qPrintable(QStringLiteral("%1 %2").arg(*twg).arg(*tfar)));
        QVERIFY(std::abs(*s.global(QStringLiteral("ht.P")) - 20) < 1e-6);
        // Layer stack rows wrong: said.
        Model bad = model;
        Node* xs = bad.find(bad.geometry().children.front().tag);
        QJsonArray rows = xs->value(QStringLiteral("stack")).toArray();
        rows.append(QJsonArray{QStringLiteral("x"), QStringLiteral("1/0"), QStringLiteral("0"), QStringLiteral("0")});
        xs->props.insert(QStringLiteral("stack"), rows);
        const GeometryBuild g3 = buildGeometry(bad, *evaluateParameters(bad));
        QVERIFY(!g3.ok());
        QVERIFY(QStringList(g3.errors.values()).join(QLatin1Char(' ')).contains(QStringLiteral("thickness must be more than 0")));
    }
};

QTEST_GUILESS_MAIN(TestFemLayout)
#include "test_fem_layout.moc"
