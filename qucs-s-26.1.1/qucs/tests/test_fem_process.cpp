/*
 * test_fem_process.cpp - a process flow: its shape steps against exact
 *                        polygons (deposits, etches of each profile, a
 *                        crystal plane's 54.74°), masks from a layout's
 *                        layer, the regions as the geometry's objects
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_geometry.h"
#include "fem_model.h"
#include "fem_process.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <gdstk/gdstk.hpp>

#include <cmath>

using namespace qucs_s::fem;

namespace {

/// A model in µm of one process flow: its properties and its steps.
Model flowModel(const QString& steps, const QString& flowProps = QStringLiteral(R"("width": "4")"), const QString& parameters = QString())
{
    const QString json = QStringLiteral(R"({"format": "qucs-s multiphysics 1", %3
      "components": [{"unit": "um", "geometry": [{"type": "process", "tag": "pf1", "label": "flow", %1, "features": [%2]},
                                                {"type": "form union", "tag": "fin"}]}]})")
                             .arg(flowProps, steps, parameters.isEmpty() ? QString() : QStringLiteral("\"parameters\": %1,").arg(parameters));
    QString why;
    const std::optional<Model> m = Model::fromJson(json.toUtf8(), &why);
    if (!m) qWarning("%s", qPrintable(why));
    return m.value_or(Model());
}

ProcessRun runFlow(const Model& m, const QString& until = {})
{
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    return runProcess(*m.find(QStringLiteral("pf1")), m, *ps, 1e-6, until);
}

const ProcessRegion* region(const ProcessRun& r, const QString& name)
{
    for (const ProcessRegion& g : r.state.regions)
        if (g.name == name) return &g;
    return nullptr;
}

double areaOf(const ProcessRun& r, const QString& name)
{
    const ProcessRegion* g = region(r, name);
    return g ? g->area() : 0;
}

/// Inside a region: its loops wind round p (nonzero).
bool inside(const ProcessRegion* g, QPointF p)
{
    if (!g) return false;
    int winding = 0;
    for (const auto& loop : g->loops)
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const QPointF a = loop[i], b = loop[(i + 1) % loop.size()];
            if (a.y() <= p.y()) {
                if (b.y() > p.y() && (b.x() - a.x()) * (p.y() - a.y()) - (p.x() - a.x()) * (b.y() - a.y()) > 0) ++winding;
            } else if (b.y() <= p.y() && (b.x() - a.x()) * (p.y() - a.y()) - (p.x() - a.x()) * (b.y() - a.y()) < 0) {
                --winding;
            }
        }
    return winding != 0;
}

bool hasCorner(const ProcessRegion* g, QPointF p, double tol = 1.5e-3)
{
    if (!g) return false;
    for (const auto& loop : g->loops)
        for (const QPointF& q : loop)
            if (std::abs(q.x() - p.x()) <= tol && std::abs(q.y() - p.y()) <= tol) return true;
    return false;
}

QString said(const ProcessRun& r)
{
    QStringList lines;
    for (const ProcessStepReport& s : r.steps) lines << s.name + QStringLiteral(": ") + s.said + (s.warnings.isEmpty() ? QString() : QStringLiteral(" [") + s.warnings.join(QStringLiteral("; ")) + QLatin1Char(']'));
    return r.error + QLatin1Char('\n') + lines.join(QLatin1Char('\n'));
}

const QString Si = QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2"})");

} // namespace

class TestFemProcess : public QObject
{
    Q_OBJECT

private slots:
    // A trench etched straight down through a mask, then a conformal film:
    // the trench's walls where the mask's edges were, its floor at its
    // depth; the film's area exact - flat tops, a quarter disc at each
    // convex corner, the walls and the floor between them.
    void aTrenchAndAConformalFilm()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "0.5", "mask": "openings", "openings": "1.5, 2.5"},
            {"type": "deposit", "tag": "d1", "label": "ox", "material": "Silicon dioxide", "thickness": "0.1"})"));
        const ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const ProcessRegion* si = region(r, QStringLiteral("si"));
        QVERIFY(si);
        QVERIFY2(std::abs(si->area() - (4 * 2 - 1 * 0.5)) < 1e-6, qPrintable(QString::number(si->area(), 'g', 10)));
        for (const QPointF& c : {QPointF(1.5, 0), QPointF(1.5, -0.5), QPointF(2.5, -0.5), QPointF(2.5, 0)})
            QVERIFY2(hasCorner(si, c), qPrintable(QStringLiteral("%1, %2").arg(c.x()).arg(c.y())));
        const double film = 3 * 0.1 + 2 * M_PI * 0.01 / 4 + 2 * 0.1 * 0.5 + (1 - 0.2) * 0.1;
        QVERIFY2(std::abs(areaOf(r, QStringLiteral("ox")) / film - 1) < 1e-3, qPrintable(QStringLiteral("%1 against %2").arg(areaOf(r, QStringLiteral("ox"))).arg(film)));
        // Planar instead: the trench filled, the top flat at 0.2 above.
        const Model p = flowModel(Si + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "0.5", "mask": "openings", "openings": "1.5, 2.5"},
            {"type": "deposit", "tag": "d1", "label": "ox", "material": "Silicon dioxide", "thickness": "0.2", "mode": "planar"})"));
        const ProcessRun rp = runFlow(p);
        QVERIFY2(rp.ok(), qPrintable(said(rp)));
        QVERIFY2(std::abs(areaOf(rp, QStringLiteral("ox")) - (4 * 0.2 + 1 * 0.5)) < 1e-6, qPrintable(said(rp)));
        QVERIFY(std::abs(rp.state.top() - 0.2) < 1e-9);
    }

    // An SOI wafer's top silicon etched to its buried oxide: the oxide not
    // touched, over-etching too; at a twentieth of the rate, over-etched by
    // half the time, the oxide loses a twentieth of half the silicon's
    // depth. A buried oxide of 2 nm under 500 nm - thinner than a step of
    // the front - is not gone through.
    void anEtchStopsOnItsLayer()
    {
        const QString soi = QStringLiteral(R"({"type": "stack", "tag": "s", "layers": [["handle", "Silicon", "1"], ["box", "Silicon dioxide", "%1"], ["device", "Silicon", "%2"]]})");
        const Model m = flowModel(soi.arg(QStringLiteral("0.2"), QStringLiteral("0.3")) + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "stop": "Silicon dioxide", "overetch": "0.2", "mask": "openings", "openings": "1, 3"})"));
        ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        QVERIFY2(std::abs(areaOf(r, QStringLiteral("device")) - 0.6) < 1e-6, qPrintable(said(r)));
        QVERIFY2(std::abs(areaOf(r, QStringLiteral("box")) - 0.8) < 1e-9, qPrintable(said(r)));
        QVERIFY2(r.steps.back().said.contains(QLatin1String("to Silicon dioxide")), qPrintable(said(r)));
        const Model slow = flowModel(soi.arg(QStringLiteral("0.2"), QStringLiteral("0.3")) + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "stop": "Silicon dioxide", "stopratio": "0.05", "overetch": "0.5", "mask": "openings", "openings": "1, 3"})"));
        r = runFlow(slow);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double lost = 0.8 - areaOf(r, QStringLiteral("box"));
        QVERIFY2(std::abs(lost - 2 * 0.3 * 0.5 * 0.05) < 2 * 0.003, qPrintable(QStringLiteral("%1 lost").arg(lost)));
        const Model thin = flowModel(soi.arg(QStringLiteral("0.002"), QStringLiteral("0.5")) + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "0.7", "materials": "Silicon", "mask": "openings", "openings": "1, 3"})"));
        r = runFlow(thin);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        QVERIFY2(std::abs(areaOf(r, QStringLiteral("handle")) - 4) < 1e-9 && std::abs(areaOf(r, QStringLiteral("box")) - 0.008) < 1e-9,
                 qPrintable(said(r)));
    }

    // An isotropic etch through a narrow opening: a stadium under it - its
    // floor a half disc of the depth, its sides the depth under the mask.
    void anIsotropicEtchUndercuts()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "0.5", "mode": "isotropic", "mask": "openings", "openings": "1.9, 2.1"})"));
        const ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double removed = 8 - areaOf(r, QStringLiteral("si"));
        const double stadium = 0.2 * 0.5 + M_PI * 0.25 / 2;
        QVERIFY2(std::abs(removed / stadium - 1) < 3e-3, qPrintable(QStringLiteral("%1 against %2").arg(removed).arg(stadium)));
        const ProcessRegion* si = region(r, QStringLiteral("si"));
        QVERIFY(!inside(si, QPointF(1.42, -0.002)) && !inside(si, QPointF(2.58, -0.002)));
        QVERIFY(inside(si, QPointF(1.38, -0.002)) && inside(si, QPointF(2.62, -0.002)));
    }

    // A crystal plane's etch of (100) silicon through a 2 µm opening: a V
    // of {111} walls at 54.74° from the surface, its point √2 µm down - deep
    // enough to meet. Tapered at 80°: a trapezoid.
    void aCrystalPlaneEtchMakesAV()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "1.9", "mode": "crystal", "mask": "openings", "openings": "1, 3"})"));
        ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double v = 0.5 * 2 * std::sqrt(2.0);
        QVERIFY2(std::abs((8 - areaOf(r, QStringLiteral("si"))) / v - 1) < 2e-3, qPrintable(QStringLiteral("%1 against %2\n").arg(8 - areaOf(r, QStringLiteral("si"))).arg(v) + said(r)));
        // The walls' angle, from the V's point to the opening's edge.
        const ProcessRegion* si = region(r, QStringLiteral("si"));
        double bottom = 0;
        for (const auto& loop : si->loops)
            for (const QPointF& q : loop)
                if (std::abs(q.x() - 2) < 0.01) bottom = std::min(bottom, q.y());
        const double degrees = std::atan(-bottom / 1.0) * 180 / M_PI;
        QVERIFY2(std::abs(degrees - 54.7356) < 0.1, qPrintable(QString::number(degrees)));
        const Model t = flowModel(Si + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "0.5", "mode": "tapered", "angle": "80", "mask": "openings", "openings": "1, 3"})"));
        r = runFlow(t);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double trapezoid = 0.5 * (2 + (2 - 2 * 0.5 / std::tan(80 * M_PI / 180))) / 2;
        QVERIFY2(std::abs((8 - areaOf(r, QStringLiteral("si"))) / trapezoid - 1) < 3e-3, qPrintable(said(r)));
    }

    // Directional: on what is seen from above - the cavity's floor under an
    // overhang gets nothing; lifted off: in the mask's openings alone.
    void directionalDepositsAndLiftOff()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "deposit", "tag": "d1", "label": "hard", "material": "Silicon nitride", "thickness": "0.1", "mode": "planar"},
            {"type": "etch", "tag": "e1", "depth": "0.1", "materials": "Silicon nitride", "mask": "openings", "openings": "1.8, 2.2"},
            {"type": "strip", "tag": "s0", "material": "hard_unused"},
            {"type": "etch", "tag": "e2", "depth": "0.3", "mode": "isotropic", "materials": "Silicon"},
            {"type": "deposit", "tag": "d2", "label": "metal", "material": "Gold", "thickness": "0.05", "mode": "directional"})"));
        ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const ProcessRegion* metal = region(r, QStringLiteral("metal"));
        QVERIFY2(metal, qPrintable(said(r)));
        // The floor straight under the opening: metal; under the overhang: none.
        QVERIFY2(inside(metal, QPointF(2.0, -0.28)), qPrintable(said(r)));
        QVERIFY2(!inside(metal, QPointF(1.65, -0.2)) && !inside(metal, QPointF(2.35, -0.2)), qPrintable(said(r)));
        QVERIFY(inside(metal, QPointF(0.5, 0.125)));   // (on the nitride)
        const Model lift = flowModel(Si + QStringLiteral(R"(,
            {"type": "deposit", "tag": "d1", "label": "pad", "material": "Gold", "thickness": "0.05", "mode": "directional",
             "mask": "openings", "openings": "1, 2"})"));
        r = runFlow(lift);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        QVERIFY2(std::abs(areaOf(r, QStringLiteral("pad")) - 0.05) < 1e-9, qPrintable(said(r)));
    }

    // Selective epitaxy grows in the oxide's window alone; CMP to the
    // silicon's top; a strip takes what the air touches, not what is buried.
    void epitaxyPlanarizeAndStrip()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "deposit", "tag": "d1", "label": "ox", "material": "Silicon dioxide", "thickness": "0.1", "mode": "planar"},
            {"type": "etch", "tag": "e1", "stop": "Silicon", "mask": "openings", "openings": "1.5, 2.5"},
            {"type": "epitaxy", "tag": "g1", "label": "epi", "material": "Silicon", "thickness": "0.15"},
            {"type": "planarize", "tag": "c1", "to": "Silicon dioxide"})"));
        ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const ProcessRegion* epi = region(r, QStringLiteral("epi"));
        QVERIFY2(epi && epi->crystal, qPrintable(said(r)));
        // Grown 0.15 in the window, then planarized to the oxide's top, 0.1.
        QVERIFY2(std::abs(epi->area() - 1 * 0.1) < 1e-6, qPrintable(QString::number(epi->area())));
        QVERIFY(std::abs(r.state.top() - 0.1) < 1e-9);
        // Resist under oxide: buried, kept, said; opened to the air: stripped.
        const Model s = flowModel(Si + QStringLiteral(R"(,
            {"type": "deposit", "tag": "d1", "label": "pr", "material": "Photoresist", "thickness": "0.1", "mode": "planar"},
            {"type": "deposit", "tag": "d2", "label": "ox", "material": "Silicon dioxide", "thickness": "0.1", "mode": "planar"},
            {"type": "strip", "tag": "s1", "material": "Photoresist"},
            {"type": "etch", "tag": "e1", "depth": "0.1", "materials": "Silicon dioxide", "mask": "openings", "openings": "1, 2"},
            {"type": "strip", "tag": "s2", "material": "Photoresist"})"));
        r = runFlow(s, QStringLiteral("s1"));
        QVERIFY2(r.ok() && std::abs(areaOf(r, QStringLiteral("pr")) - 0.4) < 1e-9, qPrintable(said(r)));
        QVERIFY2(r.steps.back().warnings.join(' ').contains(QLatin1String("no Photoresist exposed")), qPrintable(said(r)));
        r = runFlow(s);
        QVERIFY2(r.ok() && region(r, QStringLiteral("pr")) == nullptr, qPrintable(said(r)));
    }

    // A mask from a layout's layer along the cut: open where it is drawn
    // (2 to 4 µm), or where it is not, each opening grown by its bias.
    void aMaskFromALayoutsLayer()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("mask.gds"));
        {
            gdstk::Library lib = {};
            lib.init("LIB", 1e-6, 1e-9);
            auto* top = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
            top->init("TOP");
            lib.cell_array.append(top);
            auto* box = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
            *box = gdstk::rectangle(gdstk::Vec2{2, -1}, gdstk::Vec2{4, 1}, gdstk::make_tag(1, 0));
            top->polygon_array.append(box);
            QCOMPARE(lib.write_gds(QFile::encodeName(path).constData(), 0, nullptr), gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        const QString flow = QStringLiteral(R"("file": "%1", "start": ["0", "0"], "end": ["6", "0"])").arg(path);
        const QString etch = QStringLiteral(R"(,{"type": "etch", "tag": "e1", "depth": "0.2", "mask": "layer", "layer": "1/0", "polarity": "%1", "bias": "%2"})");
        ProcessRun r = runFlow(flowModel(Si + etch.arg(QStringLiteral("drawn"), QStringLiteral("0")), flow));
        QVERIFY2(r.ok(), qPrintable(said(r)));
        QVERIFY(std::abs(r.state.x1 - r.state.x0 - 6) < 1e-9);
        QVERIFY2(std::abs(12 - areaOf(r, QStringLiteral("si")) - 2 * 0.2) < 1e-6, qPrintable(said(r)));
        QVERIFY(hasCorner(region(r, QStringLiteral("si")), QPointF(2, -0.2)) && hasCorner(region(r, QStringLiteral("si")), QPointF(4, -0.2)));
        r = runFlow(flowModel(Si + etch.arg(QStringLiteral("undrawn"), QStringLiteral("0")), flow));
        QVERIFY2(r.ok() && std::abs(12 - areaOf(r, QStringLiteral("si")) - 4 * 0.2) < 1e-6, qPrintable(said(r)));
        r = runFlow(flowModel(Si + etch.arg(QStringLiteral("drawn"), QStringLiteral("0.1")), flow));
        QVERIFY2(r.ok() && std::abs(12 - areaOf(r, QStringLiteral("si")) - 2.2 * 0.2) < 1e-6, qPrintable(said(r)));
        QVERIFY(r.files.contains(QFileInfo(path).absoluteFilePath()));
        // (No layout: a layer's mask refused, said.)
        r = runFlow(flowModel(Si + etch.arg(QStringLiteral("drawn"), QStringLiteral("0"))));
        QVERIFY2(!r.ok() && r.error.contains(QLatin1String("give the flow its layout")), qPrintable(r.error));
    }

    // A film thinner than the thinnest kept: dropped, said; the flow's
    // order and its errors.
    void sliversAndErrors()
    {
        ProcessRun r = runFlow(flowModel(Si + QStringLiteral(R"(,
            {"type": "deposit", "tag": "d1", "label": "skin", "material": "Silicon dioxide", "thickness": "0.0004"})")));
        QVERIFY2(r.ok() && region(r, QStringLiteral("skin")) == nullptr, qPrintable(said(r)));
        QVERIFY2(r.warnings.join(' ').contains(QLatin1String("sliver(s) thinner than")), qPrintable(said(r)));
        r = runFlow(flowModel(QStringLiteral(R"({"type": "deposit", "tag": "d1", "material": "Gold", "thickness": "0.1"})")));
        QVERIFY2(!r.ok() && r.error.contains(QLatin1String("starts with a substrate")), qPrintable(r.error));
        r = runFlow(flowModel(Si + QStringLiteral(R"(,{"type": "etch", "tag": "e1", "stop": "Gold"})")));
        QVERIFY2(!r.ok() && r.error.contains(QLatin1String("no Gold to stop on")), qPrintable(r.error));
        r = runFlow(flowModel(Si + QStringLiteral(R"(,{"type": "etch", "tag": "e1", "depth": "0.1", "materials": "Gold"})")));
        QVERIFY2(!r.ok() && r.error.contains(QLatin1String("etches nothing here")), qPrintable(r.error));
    }

    // The regions are the geometry's objects, named by their steps: built
    // whole and up to a step, a selection by name, the file read back.
    void theRegionsAreTheGeometry()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "0.5", "mask": "openings", "openings": "1.5, 2.5"},
            {"type": "deposit", "tag": "d1", "label": "ox", "material": "Silicon dioxide", "thickness": "0.1"},
            {"type": "deposit", "tag": "d2", "material": "Silicon nitride", "thickness": "0.05"})"));
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        GeometryBuild g = buildGeometry(m, *ps);
        QVERIFY2(g.ok(), qPrintable(g.errors.values().join('\n')));
        QCOMPARE(g.topology->objectNames, (QStringList{"si", "ox", "d2"}));
        QCOMPARE(int(g.topology->domains.size()), 3);
        QString why;
        const QVector<int> ox = resolveSelection(*g.topology, m, Level::Domain, QJsonObject{{"only", QJsonArray{"ox"}}}, &why);
        QVERIFY2(ox.size() == 1 && why.isEmpty(), qPrintable(why));
        QCOMPARE(m.objectNames(), (QStringList{"si", "ox", "d2"}));
        // Up to the first deposit: the nitride not yet there.
        g = buildGeometry(m, *ps, QStringLiteral("d1"));
        QVERIFY(g.ok());
        QCOMPARE(g.topology->objectNames, (QStringList{"si", "ox"}));
        // Written and read back: its steps as they were.
        const std::optional<Model> again = Model::fromJson(m.toJson(), &why);
        QVERIFY2(again, qPrintable(why));
        QCOMPARE(again->toJson(), m.toJson());
        QVERIFY(m.toJson().contains("\"type\": \"etch\""));
        QCOMPARE(again->find(QStringLiteral("e1"))->type, QStringLiteral("process/etch"));
    }
};

QTEST_GUILESS_MAIN(TestFemProcess)
#include "test_fem_process.moc"
