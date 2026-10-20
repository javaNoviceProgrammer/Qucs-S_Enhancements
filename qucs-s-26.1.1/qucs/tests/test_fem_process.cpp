/*
 * test_fem_process.cpp - a process flow: its shape steps against exact
 *                        polygons (deposits, etches of each profile, a
 *                        crystal plane's 54.74°), masks from a layout's
 *                        layer, the regions as the geometry's objects; its
 *                        dopants against analytic profiles (an erfc, a
 *                        Gaussian, the Implant profile, segregation's
 *                        steady state, Deal and Grove's law), its moments
 *                        against their tables, its hand-offs
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_geometry.h"
#include "fem_materials.h"
#include "fem_model.h"
#include "fem_process.h"
#include "fem_process_p.h"
#include "fem_results.h"
#include "fem_solver.h"

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

constexpr double KB = 8.617333262e-5;

/// A sanitized build's (QUCS_TEST_SLOW too): coarser meshes there.
bool slowBuild()
{
    if (qEnvironmentVariableIsSet("QUCS_TEST_SLOW")) return true;
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
    return false;
}

/// A column of silicon (one cell wide), its dopants' rows 1 nm.
const QString column = QStringLiteral(R"("width": "0.1", "dx": "0.1", "dy": "1[nm]", "depth": "1.5")");

/// A field's value in the cell holding (x, y) (µm), its total or active.
double cellValue(const ProcessRun& r, const QString& name, double x, double y, bool active = false)
{
    const ProcessDopants* d = r.state.dopants.get();
    if (!d) return NAN;
    const ProcessDopants::Field* f = d->field(name);
    int i = 0, j = 0;
    if (!f || !d->cellAt(x, y, &i, &j)) return NAN;
    return (active && !f->active.empty() ? f->active : f->total)[d->index(i, j)];
}

/// A species' dose over the window (m⁻²): in material m alone, or all.
double doseOf(const ProcessRun& r, const QString& name, int material = -2)
{
    const ProcessDopants* d = r.state.dopants.get();
    const ProcessDopants::Field* f = d ? d->field(name) : nullptr;
    if (!f) return 0;
    double sum = 0;
    for (int j = 0; j < d->ny(); ++j)
        for (int i = 0; i < d->nx(); ++i) {
            const std::size_t k = d->index(i, j);
            if (material != -2 && d->material[k] != material) continue;
            sum += f->total[k] * (d->xe[std::size_t(i + 1)] - d->xe[std::size_t(i)]) * (d->ye[std::size_t(j + 1)] - d->ye[std::size_t(j)]) * 1e-12;
        }
    return sum / ((d->xe.back() - d->xe.front()) * 1e-6);
}

/// Where a species falls to \\a level going down column x from its peak (µm).
double depthTo(const ProcessRun& r, const QString& name, double x, double level)
{
    const ProcessDopants* d = r.state.dopants.get();
    const ProcessDopants::Field* f = d->field(name);
    int i = 0, j = 0;
    d->cellAt(x, 0, &i, &j);
    int peak = -1;
    for (int k = d->ny() - 1; k >= 0; --k)
        if (d->material[d->index(i, k)] >= 0 && (peak < 0 || f->total[d->index(i, k)] > f->total[d->index(i, peak)])) peak = k;
    for (int k = peak; k > 0; --k) {
        const double a = f->total[d->index(i, k)], b = f->total[d->index(i, k - 1)];
        if (a >= level && b < level) {
            const double ya = 0.5 * (d->ye[std::size_t(k)] + d->ye[std::size_t(k + 1)]), yb = 0.5 * (d->ye[std::size_t(k - 1)] + d->ye[std::size_t(k)]);
            return -(ya + (yb - ya) * (a - level) / (a - b));
        }
    }
    return NAN;
}

struct Solved {
    std::shared_ptr<ParameterScope> parameters;
    std::shared_ptr<Solution> solution;
    QString error;
};

Solved solve(const QByteArray& json)
{
    Solved out;
    QString why;
    const std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) {
        out.error = why;
        return out;
    }
    out.parameters = evaluateParameters(*m);
    const GeometryBuild g = buildGeometry(*m, *out.parameters);
    if (!g.ok()) {
        out.error = QStringLiteral("geometry: ") + QStringList(g.errors.values()).join(QLatin1String("; "));
        return out;
    }
    const StudyRun run = runStudy(*m, out.parameters, g.topology, nullptr, {});
    if (!run.ok()) {
        out.error = run.error;
        return out;
    }
    out.solution = run.solutions.back();
    return out;
}

double valueAt(const Solution& s, const QString& expression, double x, double y)
{
    const Locator locator(s.mesh());
    return evaluateAt(s, locator, expression, QPointF(x, y)).value_or(NAN);
}

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
    // ----------------------------------------------------------- Stage 2

    // A Pearson IV's table has the moments it was made of: its mean, σ, γ
    // and β to 1e-3 (the proposal's target), for skewness either way.
    void pearsonMomentsAreItsOwn()
    {
        for (const double g : {0.0, -0.5, 0.8, -1.0, 0.3}) {
            const double b = std::max(3.6, process::Pearson::leastBeta(g) + 0.6);
            const process::Pearson p = process::Pearson::make(1.0, 0.2, g, b);
            QVERIFY(process::Pearson::typeIV(g, b) || g == 0);
            const std::array<double, 4> m = p.momentsOfTable();
            const QString what = QStringLiteral("γ %1 β %2: %3 %4 %5 %6").arg(g).arg(b).arg(m[0], 0, 'g', 8).arg(m[1], 0, 'g', 8).arg(m[2], 0, 'g', 8).arg(m[3], 0, 'g', 8);
            QVERIFY2(std::abs(m[0] - 1) / 0.2 < 1e-3 && std::abs(m[1] / 0.2 - 1) < 1e-3, qPrintable(what));
            QVERIFY2(std::abs(m[2] - g) < 1e-3 && std::abs(m[3] / b - 1) < 1e-3, qPrintable(what));
        }
        // A Gaussian's own: its cdf at the mean a half, at 1σ 0.8413.
        const process::Pearson n = process::Pearson::gaussian(0, 1);
        QVERIFY(std::abs(n.cdf(0) - 0.5) < 1e-6 && std::abs(n.cdf(1) - 0.841345) < 1e-5);
    }

    // The moments: boron, phosphorus and arsenic in silicon their table's
    // (Plummer et al.'s, read off its curves); in its oxide the theory's,
    // scaled as in silicon - within a third of silicon's; antimony, the
    // theory's, shallower than arsenic; an energy past the table said.
    void rangesFromTheTablesAndTheTheory()
    {
        const Model m = flowModel(Si);
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        QString why;
        const std::optional<process::Target> si = process::targetOf(QStringLiteral("Silicon"), m, *ps, &why);
        const std::optional<process::Target> ox = process::targetOf(QStringLiteral("Silicon dioxide"), m, *ps, &why);
        QVERIFY2(si && ox, qPrintable(why));
        QVERIFY2(std::abs(si->atoms / 4.994e28 - 1) < 1e-3, qPrintable(QString::number(si->atoms)));
        QCOMPARE(process::hostOf(*si), 4);
        QCOMPARE(process::hostOf(*ox), 0);
        struct Case {
            const char* ion;
            double keV, Rp, dRp;
        };
        for (const Case& c : {Case{"B", 40, 0.1453, 0.0566}, Case{"P", 120, 0.1497, 0.0523}, Case{"As", 200, 0.1315, 0.0446}}) {
            const std::optional<process::Ion> ion = process::ionOf(QLatin1String(c.ion));
            const std::optional<process::ImplantMoments> s = process::implantMoments(*ion, c.keV * 1e3, *si, &why);
            const std::optional<process::ImplantMoments> o = process::implantMoments(*ion, c.keV * 1e3, *ox, &why);
            QVERIFY2(s && o, qPrintable(why));
            const QString what = QStringLiteral("%1 %2 keV: Rp %3 µm ΔRp %4 (%5); in its oxide %6").arg(QLatin1String(c.ion)).arg(c.keV).arg(s->Rp * 1e6).arg(s->dRp * 1e6).arg(s->source).arg(o->Rp * 1e6);
            QVERIFY2(std::abs(s->Rp * 1e6 / c.Rp - 1) < 1e-6 && std::abs(s->dRp * 1e6 / c.dRp - 1) < 1e-6, qPrintable(what));
            QVERIFY2(o->Rp / s->Rp > 0.67 && o->Rp / s->Rp < 1.33 && !s->extrapolated, qPrintable(what));
        }
        const std::optional<process::ImplantMoments> as = process::implantMoments(*process::ionOf(QStringLiteral("As")), 1e5, *si);
        const std::optional<process::ImplantMoments> sb = process::implantMoments(*process::ionOf(QStringLiteral("Sb")), 1e5, *si);
        QVERIFY(as && sb && sb->Rp < 0.9 * as->Rp && sb->source.contains(QLatin1String("transport theory")));
        const std::optional<process::ImplantMoments> far = process::implantMoments(*process::ionOf(QStringLiteral("B")), 3e5, *si);
        QVERIFY2(far && far->extrapolated && far->note.contains(QLatin1String("outside its table")), qPrintable(far ? far->note : QString()));
        // BF2: boron at 11/49 of the energy.
        const std::optional<process::Ion> bf2 = process::ionOf(QStringLiteral("BF2"));
        QVERIFY(bf2 && bf2->atom == QLatin1String("B") && std::abs(bf2->energyShare - 11.009 / 48.99) < 1e-3);
    }

    // A constant source at the surface: boron held at 1e18 cm⁻³ for an hour
    // at 1000 °C, its diffusivity intrinsic (Fair's 0.76 e^(-3.46 eV/kT)
    // cm²/s): Cs erfc(d / 2√(Dt)) to 1e-3 of Cs.
    void predepositionIsAnErfc()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "diffuse", "tag": "d1", "temperature": "1000[degC]", "time": "60[min]", "model": "constant", "source": "B", "surface": "1e18"})"),
                                  column);
        const ProcessRun r = runFlow(m);
        QVERIFY2(r.ok() && r.state.dopants, qPrintable(said(r)));
        const double D = 0.757e-4 * std::exp(-3.46 / (KB * 1273.15)), L = 2 * std::sqrt(D * 3600);
        double worst = 0, at = 0;
        const ProcessDopants& d = *r.state.dopants;
        for (int j = 0; j < d.ny(); ++j) {
            const double y = 0.5 * (d.ye[std::size_t(j)] + d.ye[std::size_t(j + 1)]);
            if (y > 0 || y < -1) continue;
            const double e = std::abs(d.field(QStringLiteral("B"))->total[d.index(0, j)] - 1e24 * std::erfc(-y * 1e-6 / L)) / 1e24;
            if (e > worst) {
                worst = e;
                at = y;
            }
        }
        qInfo("predeposition: the erfc's to %.2g of Cs (worst at %.3f µm)", worst, at);
        QVERIFY2(worst < 1e-3, qPrintable(QStringLiteral("%1 of Cs at %2 µm; %3").arg(worst).arg(at).arg(said(r))));
    }

    // A drive-in: a Gaussian implant (Rp 0.2 µm, ΔRp 20 nm, given) annealed
    // an hour at 1000 °C - a Gaussian of σ² = ΔRp² + 2Dt with its image at
    // the surface (the Implant profile's formula): to 1e-3 of its peak, the
    // dose kept to 1e-4.
    void aDriveInKeepsItsDose()
    {
        const Model m = flowModel(Si + QStringLiteral(R"(,
            {"type": "implant", "tag": "i1", "species": "B", "energy": "40", "dose": "1e14", "profile": "gaussian",
             "moments": [["Silicon", "0.2", "0.02", "", "", "0.02"]]},
            {"type": "diffuse", "tag": "d1", "temperature": "1000[degC]", "time": "60[min]", "model": "constant"})"),
                                  column);
        const ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double D = 0.757e-4 * std::exp(-3.46 / (KB * 1273.15)), dose = 1e18, Rp = 0.2e-6;
        const double sd = std::sqrt(0.02e-6 * 0.02e-6 + 2 * D * 3600);
        auto exact = [&](double d) {
            return dose / (std::sqrt(2 * M_PI) * sd) * (std::exp(-(d - Rp) * (d - Rp) / (2 * sd * sd)) + std::exp(-(d + Rp) * (d + Rp) / (2 * sd * sd)));
        };
        const double peak = exact(Rp);
        double worst = 0;
        const ProcessDopants& d = *r.state.dopants;
        for (int j = 0; j < d.ny(); ++j) {
            const double y = 0.5 * (d.ye[std::size_t(j)] + d.ye[std::size_t(j + 1)]);
            if (y > 0 || y < -1.2) continue;
            worst = std::max(worst, std::abs(d.field(QStringLiteral("B"))->total[d.index(0, j)] - exact(-y * 1e-6)) / peak);
        }
        const double kept = doseOf(r, QStringLiteral("B"));
        qInfo("drive-in: the Gaussian's to %.2g of its peak, the dose to %.2g", worst, kept / dose - 1);
        // A shallow one as implanted (Rp 40 nm, ΔRp 20 nm: 2 % above the
        // surface): what falls above folded back - the Implant profile's
        // image -, to 1e-3 of its peak.
        {
            const ProcessRun s = runFlow(flowModel(Si + QStringLiteral(R"(,
                {"type": "implant", "tag": "i1", "species": "B", "energy": "10", "dose": "1e14", "profile": "gaussian",
                 "moments": [["Silicon", "0.04", "0.02", "", "", "0.02"]]})"),
                                                   column));
            QVERIFY2(s.ok(), qPrintable(said(s)));
            const double sd0 = 0.02e-6, rp0 = 0.04e-6;
            auto image = [&](double d) {
                return dose / (std::sqrt(2 * M_PI) * sd0) * (std::exp(-(d - rp0) * (d - rp0) / (2 * sd0 * sd0)) + std::exp(-(d + rp0) * (d + rp0) / (2 * sd0 * sd0)));
            };
            double off = 0;
            const ProcessDopants& ds = *s.state.dopants;
            for (int j = 0; j < ds.ny(); ++j) {
                const double y = 0.5 * (ds.ye[std::size_t(j)] + ds.ye[std::size_t(j + 1)]);
                if (y > 0 || y < -0.3) continue;
                off = std::max(off, std::abs(ds.field(QStringLiteral("B"))->total[ds.index(0, j)] - image(-y * 1e-6)) / image(rp0));
            }
            qInfo("a shallow implant: its image's to %.2g of its peak", off);
            QVERIFY2(off < 1e-3, qPrintable(QString::number(off)));
        }
        QVERIFY2(worst < 1e-3 && std::abs(kept / dose - 1) < 1e-4,
                 qPrintable(QStringLiteral("profile %1 of its peak, dose %2; %3").arg(worst).arg(kept / dose - 1).arg(said(r))));
    }

    // The proposal's "implant + anneal": boron through a mask's window (0.3
    // to 0.7 µm, resist 2 µm), Rp 0.1 µm, straggles 30 nm, annealed 30
    // minutes at 1000 °C - the grid's against the Implant profile's formula
    // (its depth's Gaussian and image, its window's erf edges) to 1e-3 of
    // the peak; then a semiconductor's doping "From the process flow" against
    // the same doping typed in as an Implant (Fair's 0.757 cm²/s, its D0
    // rounded to 0.76 by default), each at its nodes, to 1e-3.
    void implantAndAnnealAsTheImplantProfile()
    {
        const QString flow = QStringLiteral(R"({"type": "process", "tag": "pf1", "width": "1", "dx": "2.5[nm]", "dy": "2.5[nm]", "depth": "0.6", "features": [
              {"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "0.6", "dopant": "P", "concentration": "1e15"},
              {"type": "implant", "tag": "i1", "species": "B", "energy": "40", "dose": "1e14", "profile": "gaussian",
               "moments": [["Silicon", "0.1", "0.03", "", "", "0.03"]], "mask": "openings", "openings": "0.3, 0.7", "resist": "2[um]"},
              {"type": "diffuse", "tag": "d1", "temperature": "1000[degC]", "time": "30[min]", "model": "constant"}]})");
        const Model fm = *Model::fromJson(QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um", "geometry": [%1, {"type": "form union", "tag": "fin"}]}]})").arg(flow).toUtf8());
        const ProcessRun r = runFlow(fm);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double D = 0.757e-4 * std::exp(-3.46 / (KB * 1273.15)), dose = 1e18, Rp = 1e-7;
        const double sd = std::sqrt(9e-16 + 2 * D * 1800);
        auto exact = [&](double x, double d) {
            return dose / (std::sqrt(2 * M_PI) * sd) * (std::exp(-(d - Rp) * (d - Rp) / (2 * sd * sd)) + std::exp(-(d + Rp) * (d + Rp) / (2 * sd * sd)))
                   * 0.5 * (std::erf((x - 0.3e-6) / (std::sqrt(2.0) * sd)) - std::erf((x - 0.7e-6) / (std::sqrt(2.0) * sd)));
        };
        const double peak = exact(0.5e-6, Rp);
        double worst = 0;
        QPointF at;
        const ProcessDopants& d = *r.state.dopants;
        for (int j = 0; j < d.ny(); ++j)
            for (int i = 0; i < d.nx(); ++i) {
                const double x = 0.5 * (d.xe[std::size_t(i)] + d.xe[std::size_t(i + 1)]), y = 0.5 * (d.ye[std::size_t(j)] + d.ye[std::size_t(j + 1)]);
                if (y > 0) continue;
                const double e = std::abs(d.field(QStringLiteral("B"))->total[d.index(i, j)] - exact(x * 1e-6, -y * 1e-6)) / peak;
                if (e > worst) {
                    worst = e;
                    at = QPointF(x, y);
                }
            }
        qInfo("implant and anneal: the grid's the Implant formula's to %.2g of the peak (worst at %.3f, %.3f µm)", worst, at.x(), at.y());
        QVERIFY2(worst < 1e-3, qPrintable(QStringLiteral("%1 of the peak at (%2, %3); %4").arg(worst).arg(at.x()).arg(at.y()).arg(said(r))));
        // The hand-off: a semiconductor of each doping.
        auto device = [&](const QString& geometry, const QString& doping) {
            return QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "um", "geometry": [%1],
                "materials": [{"material": "Silicon", "selection": {"all": true}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "features": [%2,
                  {"type": "ohmic", "name": "b", "V0": "0[V]", "selection": {"of": ["si"], "side": "bottom"}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.04"}}],
                "studies": [{"type": "study", "steps": [{"type": "stationary"}]}]})")
                .arg(geometry, doping)
                .toUtf8();
        };
        const Solved byProcess = solve(device(flow + QStringLiteral(R"(, {"type": "form union", "tag": "fin"})"),
                                              QStringLiteral(R"({"type": "doping", "profile": "process", "selection": {"all": true}})")));
        const Solved typed = solve(device(QStringLiteral(R"({"type": "rectangle", "name": "si", "position": ["0", "-0.6"], "size": ["1", "0.6"]}, {"type": "form union", "tag": "fin"})"),
                                          QStringLiteral(R"({"type": "doping", "dopant": "donor", "N0": "1e15[1/cm^3]", "selection": {"all": true}},
                                            {"type": "doping", "dopant": "acceptor", "profile": "implant", "dose": "1e14[1/cm^2]", "Rp": "0.1[um]", "dRp": "30[nm]",
                                             "dRl": "30[nm]", "x1": "0.3", "x2": "0.7", "ta": "30[min]", "D0": "0.757[cm^2/s]", "selection": {"all": true}})")));
        QVERIFY2(byProcess.error.isEmpty() && typed.error.isEmpty(), qPrintable(byProcess.error + typed.error));
        // (At each one's nodes - their meshes differ, a 40 nm element's
        // interpolation 3 % - against the formula.)
        auto nodes = [&](const Solution& sol, double* nd) {
            double w = 0;
            for (const QPointF& p : sol.mesh().nodes) {
                w = std::max(w, std::abs(valueAt(sol, QStringLiteral("semi.Na"), p.x(), p.y()) - exact(p.x() * 1e-6, -p.y() * 1e-6)) / peak);
                if (nd) *nd = std::max(*nd, std::abs(valueAt(sol, QStringLiteral("semi.Nd"), p.x(), p.y()) / 1e21 - 1));
            }
            return w;
        };
        double worstNd = 0;
        const double fromFlow = nodes(*byProcess.solution, &worstNd), fromImplant = nodes(*typed.solution, nullptr);
        qInfo("the semiconductor's Na at its nodes, from the flow: %.2g of the peak off the formula (the Implant's own %.2g)", fromFlow, fromImplant);
        QVERIFY2(fromFlow < 1e-3 && fromImplant < 1e-9 && worstNd < 1e-9,
                 qPrintable(QStringLiteral("Na from the flow %1 of the peak off, from the Implant %2; Nd %3").arg(fromFlow).arg(fromImplant).arg(worstNd)));
    }

    // Segregation's steady state: boron 1e17 cm⁻³ in 0.2 µm of silicon under
    // 0.1 µm of oxide (its diffusivity given there as silicon's), ten hours
    // at 1100 °C: C_Si/C_ox the interface's ratio 0.3 throughout, C_Si = C0
    // t_Si/(t_Si + t_ox/m), to 1e-3.
    void segregationReachesItsRatio()
    {
        const Model m = flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "0.2", "dopant": "B", "concentration": "1e17"},
            {"type": "deposit", "tag": "d1", "label": "ox", "material": "Silicon dioxide", "thickness": "0.1", "mode": "planar"},
            {"type": "diffuse", "tag": "a1", "temperature": "1100[degC]", "time": "10[h]", "model": "constant",
             "parameters": [["B", "Silicon dioxide", "D0", "0.757"], ["B", "Silicon dioxide", "Ea", "3.46"]]})"),
                                  QStringLiteral(R"("width": "0.1", "dx": "0.1", "dy": "1[nm]", "depth": "1")"));
        const ProcessRun r = runFlow(m);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double cSi = 1e23 * 0.2 / (0.2 + 0.1 / 0.3), cOx = cSi / 0.3;
        double worst = 0;
        for (double y : {-0.19, -0.1, -0.01, 0.01, 0.05, 0.09}) {
            const double v = cellValue(r, QStringLiteral("B"), 0.05, y), want = y < 0 ? cSi : cOx;
            worst = std::max(worst, std::abs(v / want - 1));
        }
        qInfo("segregation: each layer's steady value to %.2g", worst);
        QVERIFY2(worst < 1e-3, qPrintable(QStringLiteral("%1 off; %2").arg(worst).arg(said(r))));
    }

    // Deal and Grove: wet, an hour at 1000 °C, its rates given as their
    // table's (A 0.226 µm, B 0.287 µm²/h): x² + A x = B t, 0.4345 µm, to 1 %;
    // 0.44 of it silicon. Its own rates (their Arrhenius fits) within 5 % of
    // the table's then, wet and dry ((111); dry from their τ, 0.37 h).
    // Massoud's dry at 900 °C, (100): his closed form with his constants,
    // to 1 %.
    void dealGroveFollowsItsLaw()
    {
        // (Its regions on a grid of 0.1 nm: a thin oxide's 1 nm grid its 8 %.)
        auto grown = [&](const QString& step, const QString& orientation = QStringLiteral("100")) {
            const ProcessRun r = runFlow(flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "orientation": "%1"}, %2)").arg(orientation, step),
                                                   QStringLiteral(R"("width": "0.2", "dx": "0.1", "dy": "2[nm]", "depth": "1", "grid": "0.1[nm]")")));
            if (!r.ok()) qWarning("%s", qPrintable(said(r)));
            return std::make_pair(areaOf(r, QStringLiteral("ox")) / 0.2, areaOf(r, QStringLiteral("si")) / 0.2);
        };
        const auto wet = grown(QStringLiteral(R"({"type": "oxidize", "tag": "o1", "label": "ox", "ambient": "wet", "temperature": "1000[degC]", "time": "1[h]", "B": "0.287", "BA": "1.2699"})"));
        const double A = 0.226, B = 0.287, x = (-A + std::sqrt(A * A + 4 * B)) / 2;
        QVERIFY2(std::abs(wet.first / x - 1) < 0.01, qPrintable(QStringLiteral("%1 µm, Deal and Grove's %2").arg(wet.first).arg(x)));
        QVERIFY2(std::abs((2 - wet.second) / (0.44 * wet.first) - 1) < 0.01, qPrintable(QStringLiteral("silicon %1 µm consumed").arg(2 - wet.second)));
        const auto wetOwn = grown(QStringLiteral(R"({"type": "oxidize", "tag": "o1", "label": "ox", "ambient": "wet", "temperature": "1000[degC]", "time": "1[h]"})"),
                                  QStringLiteral("111"));
        QVERIFY2(std::abs(wetOwn.first / x - 1) < 0.05, qPrintable(QStringLiteral("wet %1 µm, the table's %2").arg(wetOwn.first).arg(x)));
        const double Ad = 0.165, Bd = 0.0117, xd = (-Ad + std::sqrt(Ad * Ad + 4 * Bd * (1 + 0.37))) / 2;
        const auto dry = grown(QStringLiteral(R"({"type": "oxidize", "tag": "o1", "label": "ox", "ambient": "dry", "rates": "dealgrove", "temperature": "1000[degC]", "time": "1[h]"})"),
                               QStringLiteral("111"));
        QVERIFY2(std::abs(dry.first / xd - 1) < 0.05, qPrintable(QStringLiteral("dry %1 µm, the table's %2").arg(dry.first).arg(xd)));
        // Massoud's: x² + A x = B t + M1 (1 - e^(-t/τ1)) + M2 (1 - e^(-t/τ2)).
        const double kT = KB * 1173.15, nm2 = 1e-6 / 60, nm = 1e-3 / 60;   // nm²/min to µm²/s, nm/min to µm/s
        const double Bm = 1.70e11 * nm2 * std::exp(-2.22 / kT), Am = Bm / (7.35e6 * nm * std::exp(-1.76 / kT));
        const double K1 = 2.49e11 * nm2 * std::exp(-2.18 / kT), K2 = 3.72e11 * nm2 * std::exp(-2.28 / kT);
        const double t1 = 4.14e-6 * 60 * std::exp(1.38 / kT), t2 = 2.71e-7 * 60 * std::exp(1.88 / kT), t = 1800;
        const double rhs = Bm * t + K1 * t1 * (1 - std::exp(-t / t1)) + K2 * t2 * (1 - std::exp(-t / t2));
        const double xm = (-Am + std::sqrt(Am * Am + 4 * rhs)) / 2;
        const auto massoud = grown(QStringLiteral(R"({"type": "oxidize", "tag": "o1", "label": "ox", "temperature": "900[degC]", "time": "30[min]"})"));
        qInfo("Deal and Grove: wet given %.5f µm (%.5f), consumed %.4f of it; wet own %.4f, dry own %.4f (the table's %.4f, %.4f); Massoud's %.5f (%.5f)",
              wet.first, x, (2 - wet.second) / wet.first, wetOwn.first, dry.first, x, xd, massoud.first, xm);
        QVERIFY2(std::abs(massoud.first / xm - 1) < 0.01, qPrintable(QStringLiteral("Massoud's %1 µm, his form's %2").arg(massoud.first).arg(xm)));
    }

    // The interface sweeping the dopants: boron depleted at the silicon's
    // surface, phosphorus piled up (their ratios 0.3 and 10, the oxide 0.44
    // of a silicon's thickness the less), each's dose kept.
    void oxidationSweepsItsDopants()
    {
        for (const char* species : {"B", "P"}) {
            const ProcessRun r = runFlow(flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "2", "dopant": "%1", "concentration": "1e17"},
                {"type": "oxidize", "tag": "o1", "label": "ox", "ambient": "dry", "temperature": "1000[degC]", "time": "1[h]"})").arg(QLatin1String(species)),
                                                   QStringLiteral(R"("width": "0.1", "dx": "0.1", "dy": "1[nm]", "depth": "1")")));
            QVERIFY2(r.ok(), qPrintable(said(r)));
            const double top = -0.44 * areaOf(r, QStringLiteral("ox")) / 0.1;
            const double surface = cellValue(r, QLatin1String(species), 0.05, top - 0.002), bulk = cellValue(r, QLatin1String(species), 0.05, -1.5);
            const double kept = doseOf(r, QLatin1String(species)) / (1e23 * 2e-6);
            const QString what = QStringLiteral("%1: at the surface %2 of the bulk's, the dose %3 kept; %4").arg(QLatin1String(species)).arg(surface / bulk).arg(kept).arg(said(r));
            QVERIFY2(std::abs(kept - 1) < 1e-6, qPrintable(what));
            QVERIFY2(species[0] == 'B' ? surface / bulk < 0.9 : surface / bulk > 1.1, qPrintable(what));
        }
    }

    // Zinc in InP by a square law (D = Dsq (C/Cs)² + a tail's): its front -
    // where half the surface's - deeper as √t (four times the time, twice
    // the depth, to 2 %), and steep: from a half to a twentieth in under a
    // third of its depth (an erfc's 1.9 times).
    void zincFollowsItsSquareLaw()
    {
        auto front = [&](const QString& time, double* steep) {
            const ProcessRun r = runFlow(flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "inp", "material": "Indium phosphide", "thickness": "3"},
                {"type": "diffuse", "tag": "z1", "temperature": "500[degC]", "time": "%1", "source": "Zn", "surface": "1e19",
                 "parameters": [["Zn", "Indium phosphide", "Dsq0", "1e-11"], ["Zn", "Indium phosphide", "Esq", "0"], ["Zn", "Indium phosphide", "Cref", "1e19"],
                                ["Zn", "Indium phosphide", "D0", "1e-15"], ["Zn", "Indium phosphide", "Ea", "0"]]})").arg(time),
                                                   QStringLiteral(R"("width": "0.1", "dx": "0.1", "dy": "2[nm]", "depth": "3")")));
            if (!r.ok()) qWarning("%s", qPrintable(said(r)));
            const double half = depthTo(r, QStringLiteral("Zn"), 0.05, 0.5e25), twentieth = depthTo(r, QStringLiteral("Zn"), 0.05, 0.05e25);
            *steep = (twentieth - half) / half;
            return half;
        };
        double s1 = 0, s2 = 0;
        const double a = front(QStringLiteral("10[min]"), &s1), b = front(QStringLiteral("40[min]"), &s2);
        const QString what = QStringLiteral("fronts %1 and %2 µm, steepness %3 %4").arg(a).arg(b).arg(s1).arg(s2);
        qInfo("zinc: %s", qPrintable(what));
        QVERIFY2(std::abs(b / a / 2 - 1) < 0.02, qPrintable(what));
        QVERIFY2(s1 < 0.33 && s2 < 0.33, qPrintable(what));
        // With no tail, the square law's similarity solution (-(η/2) f' =
        // (f² f')', f(0) = 1, η = x/√(Dsq t); shot in f): a half of the
        // surface's at η 0.8525, a fifth at 1.0534 - to 1 %. (A harmonic
        // mean at a face, or the surface's diffusivity an empty cell's, stops
        // the front: D is nought ahead of it.)
        const ProcessRun r = runFlow(flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "inp", "material": "Indium phosphide", "thickness": "3"},
            {"type": "diffuse", "tag": "z1", "temperature": "500[degC]", "time": "10[min]", "source": "Zn", "surface": "1e19",
             "parameters": [["Zn", "Indium phosphide", "Dsq0", "1e-11"], ["Zn", "Indium phosphide", "Esq", "0"], ["Zn", "Indium phosphide", "Cref", "1e19"]]})"),
                                               QStringLiteral(R"("width": "0.1", "dx": "0.1", "dy": "2[nm]", "depth": "3")")));
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double L = std::sqrt(1e-15 * 600) * 1e6;   // µm
        const double half = depthTo(r, QStringLiteral("Zn"), 0.05, 0.5e25), fifth = depthTo(r, QStringLiteral("Zn"), 0.05, 0.2e25);
        const QString exact = QStringLiteral("a half at %1 µm (%2), a fifth at %3 (%4)").arg(half).arg(0.852467 * L).arg(fifth).arg(1.053426 * L);
        qInfo("zinc, no tail: %s", qPrintable(exact));
        QVERIFY2(std::abs(half / (0.852467 * L) - 1) < 0.01 && std::abs(fifth / (1.053426 * L) - 1) < 0.01, qPrintable(exact));
    }

    // Quantum-well intermixing: a 10 nm In0.53Ga0.47As well in InP, both
    // sublattices interdiffused to √(Dt) 2 nm - at the well's middle the
    // shares of the erf slab, In 1 - 0.47 erf(Lw/4L), As erf(Lw/4L), to
    // 1e-3; then the quantum wells physics on the flow's layers: its first
    // electron state higher, the edges at the middle the alloy's there.
    void wellsIntermixAndTheirBandsMove()
    {
        // D t = 4 nm²: 2 min at 750 °C, D there 4/120 nm²/s.
        const double D750 = 4e-18 / 120;
        auto model = [&](bool mixed) {
            return QStringLiteral(R"({"format": "qucs-s multiphysics 1", "components": [{"unit": "nm",
              "geometry": [{"type": "process", "tag": "pf1", "width": "10", "dx": "10", "dy": "0.25[nm]", "depth": "100", "features": [
                {"type": "stack", "tag": "s1", "layers": [["below", "Indium phosphide", "30"], ["well", "InGaAs", "10"], ["above", "Indium phosphide", "30"]]},
                {"type": "intermix", "tag": "q1", "enabled": %1, "temperature": "750[degC]", "time": "2[min]", "diii": "%2[m^2/s]", "dv": "%2[m^2/s]",
                 "tref": "750[degC]", "eiii": "2.54[eV]", "ev": "2.54[eV]"}]},
                {"type": "form union", "tag": "fin"}],
              "materials": [{"material": "InGaAs", "compound": "InGaAs", "x": "0.53", "selection": {"only": ["well"]}},
                            {"material": "Indium phosphide", "selection": {"only": ["below", "above"]}}],
              "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.1[nm]"}],
              "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
              "studies": [{"type": "study", "steps": [{"type": "states", "states": 1}]}]})")
                .arg(mixed ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(D750, 0, 'g', 12)
                .toUtf8();
        };
        QString why;
        const std::optional<Model> m = Model::fromJson(model(true), &why);
        QVERIFY2(m, qPrintable(why));
        const ProcessRun r = runProcess(*m->find(QStringLiteral("pf1")), *m, *evaluateParameters(*m), 1e-9);
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double f = std::erf(10.0 / (4 * 2.0));
        const double in = cellValue(r, QStringLiteral("x:In"), 5, -35.1), as = cellValue(r, QStringLiteral("x:As"), 5, -35.1);
        QVERIFY2(std::abs(in - (1 - 0.47 * f)) < 1e-3 && std::abs(as - f) < 1e-3,
                 qPrintable(QStringLiteral("In %1 (%2), As %3 (%4); %5").arg(in).arg(1 - 0.47 * f).arg(as).arg(f).arg(said(r))));
        const Solved before = solve(model(false)), after = solve(model(true));
        QVERIFY2(before.error.isEmpty() && after.error.isEmpty(), qPrintable(before.error + after.error));
        const double e0 = before.solution->global(QStringLiteral("qw.Ee1")).value_or(NAN), e1 = after.solution->global(QStringLiteral("qw.Ee1")).value_or(NAN);
        // (The edge at the middle: InGaAsP's there less the layer's.)
        const std::optional<BandParameters> layer = bandParameters(QStringLiteral("InGaAs"), 0.53, 0, 300);
        const std::optional<BandParameters> mid = bandParameters(QStringLiteral("InGaAsP"), 0.47 * f, f, 300);
        const double rise = (mid->VBO + mid->Eg) - (layer->VBO + layer->Eg);
        qInfo("intermixing: In %.5f (%.5f), As %.5f (%.5f); Ee1 %.4f to %.4f eV, the middle's edge up %.4f eV", in, 1 - 0.47 * f, as, f,
              e0 / 1.602176634e-19, e1 / 1.602176634e-19, rise);
        QVERIFY2(e1 > e0 && rise > 0 && (e1 - e0) / 1.602176634e-19 > 0.2 * rise,
                 qPrintable(QStringLiteral("Ee1 %1 to %2 eV; the middle's edge up %3 eV").arg(e0 / 1.602176634e-19).arg(e1 / 1.602176634e-19).arg(rise)));
    }

    // Transient enhanced diffusion: boron implanted (2e14 cm⁻² at 20 keV),
    // half an hour at 800 °C - with the implant's +1 interstitials it goes
    // deeper than by the Fermi level alone.
    void interstitialsEnhanceBoron()
    {
        auto depth = [&](const char* model) {
            const ProcessRun r = runFlow(flowModel(Si + QStringLiteral(R"(,
                {"type": "implant", "tag": "i1", "species": "B", "energy": "20", "dose": "2e14"},
                {"type": "diffuse", "tag": "d1", "temperature": "800[degC]", "time": "30[min]", "model": "%1"})").arg(QLatin1String(model)),
                                                   column));
            if (!r.ok()) qWarning("%s", qPrintable(said(r)));
            return depthTo(r, QStringLiteral("B"), 0.05, 1e23);
        };
        const double plain = depth("fermi"), ted = depth("defects");
        qInfo("TED: to 1e17 cm⁻³ %.4f µm, with interstitials %.4f", plain, ted);
        QVERIFY2(ted > plain + 0.01, qPrintable(QStringLiteral("to 1e17 cm⁻³: %1 µm, with interstitials %2").arg(plain).arg(ted)));
    }

    // Masks and topography: an implant through a mask's openings leaves the
    // silicon under its resist untouched; a tilted beam into a trench
    // shadowed by its wall: its floor's far side lit, its near side not.
    void anImplantFollowsItsMaskAndTheTopography()
    {
        const ProcessRun r = runFlow(flowModel(Si + QStringLiteral(R"(,
            {"type": "implant", "tag": "i1", "species": "P", "energy": "50", "dose": "1e15", "mask": "openings", "openings": "1.5, 2.5"})"),
                                               QStringLiteral(R"("width": "4", "dx": "20[nm]", "dy": "2[nm]", "depth": "0.5")")));
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double open = cellValue(r, QStringLiteral("P"), 2.0, -0.06), shut = cellValue(r, QStringLiteral("P"), 0.5, -0.06);
        QVERIFY2(open > 1e25 && shut < 1e-9 * open && said(r).contains(QLatin1String("in the mask")), qPrintable(said(r)));
        // A trench 0.5 µm wide and 1 deep; the beam 10° toward +x: its floor
        // lit from 1.93 µm (its wall's shadow tan 10° wide), not at 1.8.
        const ProcessRun t = runFlow(flowModel(Si + QStringLiteral(R"(,
            {"type": "etch", "tag": "e1", "depth": "1", "mask": "openings", "openings": "1.75, 2.25"},
            {"type": "implant", "tag": "i1", "species": "As", "energy": "40", "dose": "1e15", "tilt": "10"})"),
                                               QStringLiteral(R"("width": "4", "dx": "10[nm]", "dy": "2[nm]", "depth": "0.5")")));
        QVERIFY2(t.ok(), qPrintable(said(t)));
        const double right = cellValue(t, QStringLiteral("As"), 2.2, -1.02), left = cellValue(t, QStringLiteral("As"), 1.8, -1.02);
        QVERIFY2(right > 1e24 && left < 1e-3 * right, qPrintable(QStringLiteral("floor: left %1, right %2; %3").arg(left).arg(right).arg(said(t))));
    }

    // Arsenic at a high dose (1e16 cm⁻² at 50 keV, half an hour at 1000 °C):
    // by the Fermi level its diffusivity grows with it - a box, its front
    // steeper than with the intrinsic one (from a half to a tenth of its
    // peak, a smaller share of its depth).
    void arsenicMakesABox()
    {
        auto steepness = [&](const char* model) {
            const ProcessRun r = runFlow(flowModel(Si + QStringLiteral(R"(,
                {"type": "implant", "tag": "i1", "species": "As", "energy": "50", "dose": "1e16"},
                {"type": "diffuse", "tag": "d1", "temperature": "1000[degC]", "time": "30[min]", "model": "%1"})").arg(QLatin1String(model)),
                                                   column));
            if (!r.ok()) qWarning("%s", qPrintable(said(r)));
            double peak = 0;
            const ProcessDopants& d = *r.state.dopants;
            for (double v : d.field(QStringLiteral("As"))->total) peak = std::max(peak, v);
            const double half = depthTo(r, QStringLiteral("As"), 0.05, peak / 2), tenth = depthTo(r, QStringLiteral("As"), 0.05, peak / 10);
            return std::make_pair((tenth - half) / half, half);
        };
        const auto fermi = steepness("fermi"), intrinsic = steepness("constant");
        qInfo("arsenic: from a half to a tenth %.3f of its depth (%.4f µm) by the Fermi level, %.3f (%.4f) intrinsic", fermi.first, fermi.second, intrinsic.first, intrinsic.second);
        QVERIFY2(fermi.first < 0.8 * intrinsic.first && fermi.second > intrinsic.second,
                 qPrintable(QStringLiteral("Fermi %1 (to a half %2 µm), intrinsic %3 (%4)").arg(fermi.first).arg(fermi.second).arg(intrinsic.first).arg(intrinsic.second)));
    }

    // End to end (the proposal's): a carrier-depletion phase shifter - a
    // 500 x 220 nm rib on a 90 nm slab (the example's) - built by its
    // process: its p and n halves implanted (Gaussians, Rp 90 nm, ΔRp 25 nm,
    // through their masks), its contacts' sides heavily, annealed five
    // minutes at 950 °C, the rib etched, clad in oxide; the same structure
    // (the flow's shape steps alone) with its doping typed in as Implant
    // profiles of those moments and anneals. Their effective indices at 0 V
    // and Δn_eff from 0 to -2 V the same, to 1 %. (A sanitized build's: the
    // holes the bias takes from the silicon, its modes minutes there.) Their
    // tails 4.5σ short of the buried oxide: there the flow's reflect, an
    // Implant profile's do not - 50 nm straggles had the holes 4 % apart.
    // Their dopings' integrals over the silicon alike to 2e-3: the grid read
    // at the etched slab's surface out to it, not its nearest cell's middle
    // (4 % short, its profile steep there).
    void aPhaseShifterBuiltByItsProcess()
    {
        const bool modes = !slowBuild();
        auto model = [&](bool byProcess) {
            const QString implants = QStringLiteral(R"(
              {"type": "implant", "tag": "ib", "species": "B", "energy": "20", "dose": "6.6e12", "profile": "gaussian",
               "moments": [["Silicon", "0.09", "0.025", "", "", "0.025"]], "mask": "openings", "openings": "-3, 0"},
              {"type": "implant", "tag": "ip", "species": "P", "energy": "40", "dose": "6.6e12", "profile": "gaussian",
               "moments": [["Silicon", "0.09", "0.025", "", "", "0.025"]], "mask": "openings", "openings": "0, 3"},
              {"type": "implant", "tag": "ibc", "species": "B", "energy": "20", "dose": "2e15", "profile": "gaussian",
               "moments": [["Silicon", "0.09", "0.025", "", "", "0.025"]], "mask": "openings", "openings": "-3, -1.5"},
              {"type": "implant", "tag": "ipc", "species": "P", "energy": "40", "dose": "2e15", "profile": "gaussian",
               "moments": [["Silicon", "0.09", "0.025", "", "", "0.025"]], "mask": "openings", "openings": "1.5, 3"},
              {"type": "diffuse", "tag": "an", "temperature": "950[degC]", "time": "5[min]", "model": "constant",
               "parameters": [["P", "Silicon", "D0", "3.85"], ["P", "Silicon", "Ea", "3.66"], ["P", "Silicon", "Dm0", "0"], ["P", "Silicon", "Dmm0", "0"]]},)");
            const QString D = QStringLiteral(R"("ta": "5[min]", "Ta": "1223.15[K]", "surface": "0", "Rp": "0.09[um]", "dRp": "25[nm]", "dRl": "25[nm]")");
            const QString typed = QStringLiteral(R"(
                {"type": "doping", "dopant": "acceptor", "profile": "implant", "dose": "6.6e12[1/cm^2]", "x2": "0", %1, "D0": "0.757[cm^2/s]", "Ea": "3.46[eV]", "selection": {"all": true}},
                {"type": "doping", "dopant": "donor", "profile": "implant", "dose": "6.6e12[1/cm^2]", "x1": "0", %1, "D0": "3.85[cm^2/s]", "Ea": "3.66[eV]", "selection": {"all": true}},
                {"type": "doping", "dopant": "acceptor", "profile": "implant", "dose": "2e15[1/cm^2]", "x2": "-1.5", %1, "D0": "0.757[cm^2/s]", "Ea": "3.46[eV]", "selection": {"all": true}},
                {"type": "doping", "dopant": "donor", "profile": "implant", "dose": "2e15[1/cm^2]", "x1": "1.5", %1, "D0": "3.85[cm^2/s]", "Ea": "3.66[eV]", "selection": {"all": true}},)")
                                      .arg(D);
            return QStringLiteral(R"({"format": "qucs-s multiphysics 1", "parameters": [{"name": "Va", "expression": "0[V]"}],
              "components": [{"unit": "um", "thickness": "1[mm]",
                "geometry": [{"type": "process", "tag": "pf1", "x0": "-3", "width": "6", "dx": "5[nm]", "dy": "2[nm]", "depth": "0.3", "features": [
                    {"type": "stack", "tag": "st", "layers": [["box", "Silicon dioxide", "1"], ["si", "Silicon", "0.22"]]}, %1
                    {"type": "etch", "tag": "rib", "depth": "0.13", "mask": "openings", "openings": "-3, -0.25; 0.25, 3"},
                    {"type": "deposit", "tag": "clad", "label": "cladding", "material": "Silicon dioxide", "thickness": "1", "mode": "planar"}]},
                  {"type": "form union", "tag": "fin"}],
                "materials": [{"material": "Silicon dioxide", "selection": {"only": ["box", "cladding"]}}, {"material": "Silicon", "selection": {"only": ["si"]}}],
                "physics": [{"type": "semiconductor", "tag": "semi", "selection": {"only": ["si"]}, "features": [%2
                    {"type": "srh", "selection": {"all": true}},
                    {"type": "ohmic", "name": "a", "V0": "Va", "selection": {"of": ["si"], "side": "left"}},
                    {"type": "ohmic", "name": "k", "V0": "0[V]", "selection": {"of": ["si"], "side": "right"}}]},
                  {"type": "modes", "tag": "emw", "features": [{"type": "wave", "plasma": true, "selection": {"only": ["si"]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "0.2", "features": [{"type": "size", "level": "domain", "selection": {"only": ["si"]}, "hmax": "%3"}]}}],
              "studies": [{"type": "study", "steps": [{"type": "sweep", "parameters": [["Va", "0, -2", "V"]]},
                {"type": "stationary", "physics": ["semi"]} %4]}]})")
                .arg(byProcess ? implants : QString(),
                     byProcess ? QStringLiteral(R"({"type": "doping", "profile": "process", "selection": {"all": true}},)") : typed,
                     slowBuild() ? QStringLiteral("0.05") : QStringLiteral("0.03"),
                     modes ? QStringLiteral(R"(, {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "2.6"})") : QString())
                .toUtf8();
        };
        double dopings[2][2] = {{0, 0}, {0, 0}};   // ∫Na, ∫Nd: the flow's, the typed
        auto neffs = [&](bool byProcess) -> std::pair<double, double> {
            QString why;
            const std::optional<Model> m = Model::fromJson(model(byProcess), &why);
            if (!m) return {NAN, NAN};
            const std::shared_ptr<ParameterScope> ps = evaluateParameters(*m);
            const GeometryBuild g = buildGeometry(*m, *ps);
            if (!g.ok()) {
                qWarning("%s", qPrintable(QStringList(g.errors.values()).join(QLatin1String("; "))));
                return {NAN, NAN};
            }
            const StudyRun run = runStudy(*m, ps, g.topology, nullptr, {});
            if (!run.ok() || run.solutions.size() != 2) {
                qWarning("%s", qPrintable(run.error));
                return {NAN, NAN};
            }
            // (Each doping's integral over the silicon: the hand-off at the
            // etched slab's surface, its profile steep there.)
            for (int k = 0; k < 2; ++k) {
                Node n;
                n.type = QStringLiteral("integral");
                n.props.insert(QStringLiteral("expression"), k == 0 ? QStringLiteral("semi.Na*1[m]") : QStringLiteral("semi.Nd*1[m]"));
                n.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("si")}}});
                dopings[byProcess ? 0 : 1][k] = evaluateDerived(*run.solutions[0], n).values.value(0).value;
            }
            auto value = [&](Solution& s) {
                s.select(s.snapshotCount() - 1);
                if (modes) return s.global(QStringLiteral("emw.neff")).value_or(NAN);
                Node holes;
                holes.type = QStringLiteral("integral");
                holes.props.insert(QStringLiteral("expression"), QStringLiteral("semi.p*1[m]"));
                holes.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{QStringLiteral("si")}}});
                const DerivedResult d = evaluateDerived(s, holes);
                return d.values.isEmpty() ? double(NAN) : d.values.front().value;
            };
            return {value(*run.solutions[0]), value(*run.solutions[1])};
        };
        const auto flow = neffs(true), typed = neffs(false);
        const double dFlow = flow.second - flow.first, dTyped = typed.second - typed.first;
        const QString what = QStringLiteral("%1 at 0 V %2 (typed %3), its change to -2 V %4 (typed %5)")
                                 .arg(modes ? QStringLiteral("n_eff") : QStringLiteral("holes"))
                                 .arg(flow.first, 0, 'g', 9).arg(typed.first, 0, 'g', 9).arg(dFlow, 0, 'g', 5).arg(dTyped, 0, 'g', 5);
        const QString doped = QStringLiteral("∫Na %1 (typed %2), ∫Nd %3 (typed %4)")
                                  .arg(dopings[0][0], 0, 'g', 6).arg(dopings[1][0], 0, 'g', 6).arg(dopings[0][1], 0, 'g', 6).arg(dopings[1][1], 0, 'g', 6);
        qInfo("phase shifter: %s; %s", qPrintable(what), qPrintable(doped));
        QVERIFY2(std::abs(dopings[0][0] / dopings[1][0] - 1) < 2e-3 && std::abs(dopings[0][1] / dopings[1][1] - 1) < 2e-3, qPrintable(doped));
        QVERIFY2(std::isfinite(dFlow) && std::isfinite(dTyped) && (modes ? dTyped > 0 : dTyped < 0), qPrintable(what));
        QVERIFY2((modes ? std::abs(flow.first - typed.first) < 1e-5 : std::abs(flow.first / typed.first - 1) < 5e-3) && std::abs(dFlow / dTyped - 1) < 0.01,
                 qPrintable(what));
    }

    // Activation and dose loss: boron at 1e20 cm⁻³, its solubility given
    // as 5e19 - active that much, the rest inactive, said; a surface's loss
    // given - its dose less, said.
    void solubilityAndDoseLoss()
    {
        const ProcessRun r = runFlow(flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "1", "dopant": "B", "concentration": "1e20"},
            {"type": "diffuse", "tag": "a1", "temperature": "900[degC]", "time": "10[min]", "parameters": [["B", "Silicon", "Css0", "5e19"], ["B", "Silicon", "Ecss", "0"]]})"),
                                               column));
        QVERIFY2(r.ok(), qPrintable(said(r)));
        const double total = cellValue(r, QStringLiteral("B"), 0.05, -0.5), active = cellValue(r, QStringLiteral("B"), 0.05, -0.5, true);
        QVERIFY2(std::abs(total / 1e26 - 1) < 1e-6 && std::abs(active / 5e25 - 1) < 1e-6 && said(r).contains(QLatin1String("50 % of the B inactive")),
                 qPrintable(QStringLiteral("%1 %2; %3").arg(total).arg(active).arg(said(r))));
        const ProcessRun l = runFlow(flowModel(QStringLiteral(R"({"type": "substrate", "tag": "sub", "label": "si", "material": "Silicon", "thickness": "1", "dopant": "B", "concentration": "1e18"},
            {"type": "diffuse", "tag": "a1", "temperature": "1100[degC]", "time": "60[min]", "model": "constant", "parameters": [["B", "Silicon", "loss", "1e-5"]]})"),
                                               column));
        QVERIFY2(l.ok() && doseOf(l, QStringLiteral("B")) < 0.99e18 * 1e-6 * 1e6 && said(l).contains(QLatin1String("lost at the surfaces")), qPrintable(said(l)));
    }

    // Transport theory against a Monte Carlo of the same collisions (a
    // TRIM-like binary collision code with this potential's table and
    // Lindhard and Scharff's stopping, 20000 ions, written to check it):
    // P, As, Sb at 80 keV in silicon, the electronic stopping as LSS's -
    // Rp and ΔRp to 4 %, γ to 0.05, β to 0.1, the lateral straggle to 4 %.
    // (A heavy ion's large collisions exact: to second order in the energy
    // given, As's γ came out 0.87.)
    void theoryAgainstAMonteCarlo()
    {
        const Model m = flowModel(Si);
        const std::optional<process::Target> si = process::targetOf(QStringLiteral("Silicon"), m, *evaluateParameters(m));
        QVERIFY(si);
        struct Ref {
            int Z;
            double M, Rp, dRp, gamma, beta, lateral;
        };
        for (const Ref& c : {Ref{15, 30.974, 0.1130, 0.0427, -0.00, 2.64, 0.0374}, Ref{33, 74.92, 0.0566, 0.0199, 0.34, 2.88, 0.0148},
                             Ref{51, 120.9, 0.0453, 0.0134, 0.41, 3.11, 0.0101}}) {
            const process::RangeMoments t = process::rangeMoments(c.Z, c.M, *si, 8e4);
            const QString what = QStringLiteral("Z %1: Rp %2 (%3) ΔRp %4 (%5) γ %6 (%7) β %8 (%9) lateral %10 (%11)")
                                     .arg(c.Z).arg(t.Rp * 1e6).arg(c.Rp).arg(t.dRp * 1e6).arg(c.dRp).arg(t.gamma).arg(c.gamma).arg(t.beta).arg(c.beta)
                                     .arg(t.lateral * 1e6).arg(c.lateral);
            QVERIFY2(t.ok && std::abs(t.Rp * 1e6 / c.Rp - 1) < 0.04 && std::abs(t.dRp * 1e6 / c.dRp - 1) < 0.04 && std::abs(t.gamma - c.gamma) < 0.05
                         && std::abs(t.beta - c.beta) < 0.1 && std::abs(t.lateral * 1e6 / c.lateral - 1) < 0.04,
                     qPrintable(what));
        }
    }

    // LOCOS: a pad oxide, nitride opened over 1.5 to 2.5 µm, an hour of wet
    // oxide at 1000 °C - grown in the opening as Deal and Grove from its pad,
    // none far under the nitride, a bird's beak between, the nitride's edge
    // lifted by it.
    void locosGrowsWhereTheNitrideIsOpen()
    {
        const ProcessRun r = runFlow(flowModel(Si + QStringLiteral(R"(,
            {"type": "deposit", "tag": "pad", "label": "pad", "material": "Silicon dioxide", "thickness": "0.01", "mode": "planar"},
            {"type": "deposit", "tag": "nit", "label": "nitride", "material": "Silicon nitride", "thickness": "0.1", "mode": "planar"},
            {"type": "etch", "tag": "e1", "materials": "Silicon nitride", "stop": "Silicon dioxide", "mask": "openings", "openings": "1.5, 2.5"},
            {"type": "oxidize", "tag": "o1", "label": "field", "ambient": "wet", "temperature": "1000[degC]", "time": "1[h]"})"),
                                               QStringLiteral(R"("width": "4", "dx": "20[nm]", "dy": "2[nm]", "depth": "1")")));
        QVERIFY2(r.ok(), qPrintable(said(r)));
        // The silicon's top down each column: 0.44 of what grew.
        auto siliconTop = [&](double x) {
            const ProcessRegion* si = region(r, QStringLiteral("si"));
            double top = -10;
            for (double y = 0.01; y > -1; y -= 0.0005)
                if (inside(si, QPointF(x, y))) {
                    top = y;
                    break;
                }
            return top;
        };
        const double open = -siliconTop(2.0) / 0.44, far = -siliconTop(0.5) / 0.44, edge = -siliconTop(1.46) / 0.44;
        // Deal and Grove's wet (100) from 10 nm of pad.
        const double kT = KB * 1273.15, B = 386 * std::exp(-0.78 / kT), A = B / (1.63e8 / 1.68 * std::exp(-2.05 / kT));
        const double rhs = B * 1 + 0.01 * 0.01 + A * 0.01, x = (-A + std::sqrt(A * A + 4 * rhs)) / 2 - 0.01;
        const QString what = QStringLiteral("grown: open %1 µm (Deal and Grove %2), far under the nitride %3, at its edge %4; %5").arg(open).arg(x).arg(far).arg(edge).arg(said(r));
        qInfo("LOCOS: %s", qPrintable(what.section(QLatin1Char(';'), 0, 0)));
        QVERIFY2(std::abs(open / x - 1) < 0.01 && far < 0.005 && edge > 0.2 * x && edge < 0.95 * x && said(r).contains(QLatin1String("bird's beak")),
                 qPrintable(what));
    }

    // Channelling: boron into crystalline silicon with a channelled part
    // (a fifth, about 0.25 µm) - its tail there; arsenic at a dose that
    // makes the crystal amorphous takes none.
    void channellingAndItsAmorphousCut()
    {
        auto at = [&](const QString& species, const QString& dose, double y) {
            const ProcessRun r = runFlow(flowModel(Si + QStringLiteral(R"(,
                {"type": "implant", "tag": "i1", "species": "%1", "energy": "20", "dose": "%2", "profile": "dual",
                 "channel": [["Silicon", "0.2", "0.25", "0.05", "0", "3"]]})").arg(species, dose), column));
            if (!r.ok()) qWarning("%s", qPrintable(said(r)));
            return std::make_pair(cellValue(r, species, 0.05, y), said(r));
        };
        const auto b = at(QStringLiteral("B"), QStringLiteral("1e13"), -0.25);
        // 1e13 cm⁻² of which a fifth at 0.25 µm, 50 nm straggle: 1.6e17 cm⁻³ there.
        QVERIFY2(std::abs(b.first / (0.2 * 1e13 / (std::sqrt(2 * M_PI) * 0.05e-4) * 1e6) - 1) < 0.15, qPrintable(QString::number(b.first) + b.second));
        const auto as = at(QStringLiteral("As"), QStringLiteral("1e15"), -0.25);
        QVERIFY2(as.second.contains(QLatin1String("amorphous to")) && as.first < 1e-6 * b.first, qPrintable(QString::number(as.first) + as.second));
    }

    // Doped as made: an epitaxial stack's rows each its own doping; a
    // boron-doped glass deposited over silicon, an anneal with boron's
    // diffusivity in the glass given - boron in the silicon from it.
    void dopedAsMade()
    {
        const ProcessRun s = runFlow(flowModel(QStringLiteral(R"({"type": "stack", "tag": "s1", "layers": [["n", "Silicon", "1", "P", "1e17"], ["p", "Silicon", "0.5", "B", "2e18"]]})"),
                                               column));
        QVERIFY2(s.ok() && std::abs(cellValue(s, QStringLiteral("P"), 0.05, -1) / 1e23 - 1) < 1e-9 && std::abs(cellValue(s, QStringLiteral("B"), 0.05, -0.2) / 2e24 - 1) < 1e-9
                     && cellValue(s, QStringLiteral("B"), 0.05, -1) == 0,
                 qPrintable(said(s)));
        const ProcessRun g = runFlow(flowModel(Si + QStringLiteral(R"(,
            {"type": "deposit", "tag": "bsg", "label": "glass", "material": "Silicon dioxide", "thickness": "0.2", "mode": "planar", "dopant": "B", "concentration": "1e21"},
            {"type": "diffuse", "tag": "a1", "temperature": "1000[degC]", "time": "30[min]", "model": "constant",
             "parameters": [["B", "Silicon dioxide", "D0", "0.757"], ["B", "Silicon dioxide", "Ea", "3.46"], ["B", "Silicon dioxide", "m:Silicon", "1"]]})"),
                                               column));
        QVERIFY2(g.ok() && cellValue(g, QStringLiteral("B"), 0.05, -0.02) > 1e25, qPrintable(said(g)));
    }

    // Amorphization: arsenic 1e15 cm⁻² at 80 keV makes silicon amorphous
    // near its surface; boron 1e13 at 40 keV does not.
    void heavyImplantsAmorphize()
    {
        const ProcessRun as = runFlow(flowModel(Si + QStringLiteral(R"(, {"type": "implant", "tag": "i1", "species": "As", "energy": "80", "dose": "1e15"})"), column));
        const ProcessRun b = runFlow(flowModel(Si + QStringLiteral(R"(, {"type": "implant", "tag": "i1", "species": "B", "energy": "40", "dose": "1e13"})"), column));
        QVERIFY2(as.ok() && b.ok(), qPrintable(said(as) + said(b)));
        QVERIFY2(said(as).contains(QLatin1String("amorphous to")) && !said(b).contains(QLatin1String("amorphous")), qPrintable(said(as) + said(b)));
    }
};

QTEST_GUILESS_MAIN(TestFemProcess)
#include "test_fem_process.moc"
