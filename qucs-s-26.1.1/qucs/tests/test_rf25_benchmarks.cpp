/*
 * test_rf25_benchmarks.cpp - RF 2.5D against references that are not itself,
 * gathered where a build runs them all: published measurements, closed forms
 * and filament sums, each case's reference and its source written beside
 * it, its tolerance about today's answer so that a drift is caught, and a
 * table of them all said at the end
 * ("A benchmark regression suite on every build. The references
 * that caught real faults are in my folders, not in CI").
 *
 * Its heavy cases - a spiral's loop, dense, some 130 times a Release build's
 * time under the sanitizers - are skipped there, as the accuracy suite's
 * are; CI's macOS job runs them in Release.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rf25/rf25_inductance.h"
#include "rf25/rf25_layout.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_static.h"

#include <QtTest>

#include <cmath>

namespace rf = qucs_s::rf25;

namespace {

/// One benchmark's answer, for the table said at the end.
struct Row {
    QString name, source;
    double value = 0, reference = 0, tolerance = 0;   // tolerance: relative, about the reference
    QString unit;
};
QList<Row>& rows()
{
    static QList<Row> all;
    return all;
}

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

rf::Stack grounded(double epsr, double height, double thickness = 0, double sigma = 0)
{
    rf::Stack s;
    rf::Layer ground;
    ground.kind = rf::Layer::Ground;
    ground.name = QStringLiteral("GND");
    ground.tag = QStringLiteral("gnd");
    rf::Layer sub;
    sub.kind = rf::Layer::Substrate;
    sub.name = QStringLiteral("SUB");
    sub.tag = QStringLiteral("sub");
    sub.thickness = height;
    sub.epsr = epsr;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    metal.thickness = thickness;
    metal.sigma = sigma;
    s.layers << ground << sub << metal;
    return s;
}

rf::Conductor strip(double x, double y, double w, double h, const QString& layer = QStringLiteral("M1"))
{
    rf::Conductor c;
    c.layer = layer;
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

/// Mohan's centre line (Mohan, del Mar Hershenson, Boyd and Lee, "Simple
/// accurate expressions for planar spiral inductances", IEEE JSSC 34(10),
/// 1999): \a sides a turn, \a n turns, outer diameter \a dout, width \a w,
/// space \a s (µm); a polygon's step spread over its last \a m sides - as
/// test_rf25_inductance draws them.
QList<QPointF> mohanCentreLine(int sides, double n, double dout, double w, double s, int m = 1)
{
    const double a0 = dout / 2 - w / 2, p = w + s;
    const int count = int(std::lround(sides * n));
    QList<QPointF> pts;
    if (sides == 4) {
        pts << QPointF(-a0, -a0);
        const QPointF dirs[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
        for (int k = 0; k < count; ++k) {
            const double L = k == 0 ? 2 * a0 : 2 * a0 - p * ((k - 1) / 2);
            pts << pts.last() + dirs[k % 4] * L;
        }
        pts.prepend(pts.first() - QPointF(w, 0));
        return pts;
    }
    const auto line = [&](int k) {
        const int t = std::max(k, 0) / sides, j = std::max(k, 0) % sides;
        const double phi = -M_PI / 2 + 2 * M_PI * k / sides;
        return std::array<double, 3>{std::cos(phi), std::sin(phi), a0 - p * (t + std::max(0, j - (sides - m)) / double(m))};
    };
    const auto cross = [&](int k) {
        const auto [a1, b1, c1] = line(k - 1);
        const auto [a2, b2, c2] = line(k);
        const double d = a1 * b2 - a2 * b1;
        return QPointF((c1 * b2 - c2 * b1) / d, (a1 * c2 - a2 * c1) / d);
    };
    for (int k = 0; k <= count; ++k) pts << cross(k);
    pts[0] -= QPointF(w, 0);
    const auto [nx, ny, c] = line(count);
    Q_UNUSED(c);
    pts << pts.last() + QPointF(-ny, nx) * (w / 2);
    return pts;
}

/// The strip \a w wide along \a pts, mitred at its corners.
rf::Conductor widened(const QList<QPointF>& pts, double w)
{
    QList<QPointF> left, right;
    const auto normal = [](QPointF a, QPointF b) {
        const QPointF d = b - a;
        const double l = std::hypot(d.x(), d.y());
        return QPointF(-d.y() / l, d.x() / l);
    };
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF n1 = i == 0 ? normal(pts.at(0), pts.at(1)) : normal(pts.at(i - 1), pts.at(i));
        const QPointF n2 = i + 1 == pts.size() ? n1 : normal(pts.at(i), pts.at(i + 1));
        QPointF m = n1 + n2;
        m /= std::hypot(m.x(), m.y());
        const double k = w / 2 / (m.x() * n1.x() + m.y() * n1.y());
        left << pts.at(i) + m * k;
        right.prepend(pts.at(i) - m * k);
    }
    rf::Conductor c;
    c.layer = QStringLiteral("M1");
    for (const QPointF& q : left + right) c.outline << QPointF(std::round(q.x() * 1e3) / 1e3, std::round(q.y() * 1e3) / 1e3);
    return c;
}

/// The complete elliptic integral of the first kind, by the arithmetic-
/// geometric mean.
double ellipticK(double k)
{
    double a = 1, b = std::sqrt(1 - k * k);
    for (int i = 0; i < 60 && std::abs(a - b) > 1e-16 * a; ++i) {
        const double m = 0.5 * (a + b);
        b = std::sqrt(a * b);
        a = m;
    }
    return M_PI / (2 * a);
}

} // namespace

class TestRf25Benchmarks : public QObject
{
    Q_OBJECT

private slots:
    // Mohan's spirals (IEEE JSSC 1999), the inductance measured at a low
    // frequency, through the loop inductance at the tab's mesh - 15 µm cells,
    // one across: the three squares once found shorted, and the seven
    // polygons once found 10 times past their cells. Each within 10 % of the measured, the paper's own expressions'
    // spread, and within 1.5 % of its own answer today, so that a drift is
    // caught (today: L02 +0.7 %, L05 -0.7, L16 -1.2, L55 +1.0, L56 -0.7, L57
    // +8.3, L58 +3.9, L59 -1.6, L60 +0.9, L61 +2.1 against the measured). (L57's
    // +8.3 % is the measurement's: ASITIC reads it 7.7 % over too. The
    // measured values here, not ASITIC's - L61 7.30 measured, where
    // test_rf25_inductance holds ASITIC's 7.44.)
    void mohansSpiralsAreTheirMeasuredInductance_data()
    {
        QTest::addColumn<int>("sides");
        QTest::addColumn<double>("turns");
        QTest::addColumn<double>("dout");
        QTest::addColumn<double>("w");
        QTest::addColumn<double>("s");
        QTest::addColumn<int>("m");
        QTest::addColumn<double>("measured");
        QTest::addColumn<double>("today");
        QTest::newRow("L02") << 4 << 3.75 << 292.0 << 13.0 << 1.9 << 1 << 6.00 << 6.0409;
        QTest::newRow("L05") << 4 << 4.75 << 206.0 << 7.8 << 1.9 << 1 << 6.10 << 6.0566;
        QTest::newRow("L16") << 4 << 12.0 << 180.0 << 3.2 << 2.1 << 1 << 20.5 << 20.246;
        QTest::newRow("L55") << 8 << 4.0 << 346.0 << 18.0 << 2.0 << 1 << 5.90 << 5.9574;
        QTest::newRow("L56") << 8 << 5.0 << 346.0 << 18.0 << 2.0 << 1 << 7.50 << 7.4479;
        QTest::newRow("L57") << 8 << 4.0 << 326.0 << 8.0 << 12.0 << 1 << 5.60 << 6.0620;
        QTest::newRow("L58") << 8 << 5.0 << 326.0 << 8.0 << 12.0 << 1 << 7.20 << 7.4820;
        QTest::newRow("L59") << 12 << 6.75 << 197.0 << 8.2 << 3.0 << 2 << 5.45 << 5.3614;
        QTest::newRow("L60") << 12 << 8.0 << 198.0 << 7.5 << 3.0 << 3 << 6.30 << 6.3576;
        QTest::newRow("L61") << 12 << 7.75 << 198.0 << 6.5 << 3.0 << 2 << 7.30 << 7.4515;
    }
    void mohansSpiralsAreTheirMeasuredInductance()
    {
        if (slowBuild()) QSKIP("a spiral's loop: many minutes under the sanitizers");
        QFETCH(int, sides);
        QFETCH(double, turns);
        QFETCH(double, dout);
        QFETCH(double, w);
        QFETCH(double, s);
        QFETCH(int, m);
        QFETCH(double, measured);
        QFETCH(double, today);
        // (Mohan's process as test_rf25_inductance has it: 0.9 µm of
        //  aluminium, 3.5e7 S/m, 10 µm of oxide over the substrate.)
        const rf::Stack stack = grounded(3.9, 10e-6, 0.9e-6, 3.5e7);
        const QList<QPointF> line = mohanCentreLine(sides, turns, dout, w, s, m);
        const rf::Conductor spiral = widened(line, w);
        rf::MeshSettings how;
        how.maximum = 15e-6;
        how.cellsAcross = 1;
        how.gaps = true;
        const rf::Mesh mesh = rf::meshOf({spiral}, how, stack, 1e9);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const rf::Inductance loop = rf::inductanceOf(mesh, {spiral}, stack, line.first(), line.last());
        QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
        const double nh = loop.lDc * 1e9;
        rows() << Row{QStringLiteral("Mohan %1, L at a low frequency").arg(QTest::currentDataTag()),
                      QStringLiteral("measured, Mohan et al., IEEE JSSC 1999"), nh, measured, 0.10, QStringLiteral("nH")};
        qInfo("%s: %lld cells, L_dc %.4f nH, measured %.2f (%+.1f %%)", QTest::currentDataTag(), qlonglong(mesh.cells.size()), nh,
              measured, 100 * (nh / measured - 1));
        QVERIFY2(std::abs(nh / measured - 1) < 0.10, qPrintable(QStringLiteral("%1 nH against %2 measured").arg(nh).arg(measured)));
        if (today > 0)
            QVERIFY2(std::abs(nh / today - 1) < 0.015, qPrintable(QStringLiteral("%1 nH against %2 today").arg(nh).arg(today)));
    }

    // A square loop of wire as thick as it is wide: 10 mm along its middle,
    // 0.5 by 0.5 mm, a
    // 0.2 mm gap - 24.17 nH by a Neumann sum over 16 by 16 filaments of each
    // side, each filament's own term its rectangle's geometric mean distance
    // (Grover, Inductance Calculations, 1946). The loop inductance, its runs
    // bars, within 0.5 %.
    void aThickSquareLoopIsItsFilamentSum()
    {
        if (slowBuild()) QSKIP("its loop's bars: minutes under the sanitizers");
        const rf::Stack stack = grounded(1, 5e-3, 0.5e-3, 6e7);
        rf::Conductor loop;
        loop.layer = QStringLiteral("M1");
        loop.outline = QPolygonF(QList<QPointF>{{-5250, -100}, {-5250, -5250}, {5250, -5250}, {5250, 5250}, {-5250, 5250}, {-5250, 100},
                                                {-4750, 100}, {-4750, 4750}, {4750, 4750}, {4750, -4750}, {-4750, -4750}, {-4750, -100}});
        rf::MeshSettings how;
        how.cellsAcross = 2;
        const rf::Mesh mesh = rf::meshOf({loop}, how, stack, 1e8);
        const rf::Inductance l = rf::inductanceOf(mesh, {loop}, stack, QPointF(-5000, -100), QPointF(-5000, 100), 1e-6, 1e4);
        QVERIFY2(l.ok(), qPrintable(l.problems.join(" | ")));
        const double nh = l.lDc * 1e9;
        rows() << Row{QStringLiteral("Thick square loop, L at direct current"), QStringLiteral("filament sum, Grover's GMDs"), nh, 24.174, 0.005,
                      QStringLiteral("nH")};
        QVERIFY2(std::abs(nh / 24.174 - 1) < 0.005, qPrintable(QStringLiteral("%1 nH against 24.174").arg(nh)));
    }

    // A coplanar line in air as a sheet - signal 50 µm, gaps 2, 5 and 20 µm,
    // grounds 160 µm - its capacitance a metre the conformal map's exactly,
    // its grounds finite: 4 eps0 K(k)/K(k'), k = (a/b) sqrt((1 - b^2/c^2)/
    // (1 - a^2/c^2)), a, b, c the signal's half-width, the gap's far edge and
    // the ground's (Ghione and Naldi, Electronics Letters 1984). The
    // quasi-static solve's, within 0.5 %.
    void aCoplanarSheetIsItsConformalMap_data()
    {
        QTest::addColumn<double>("gap");
        QTest::newRow("2 um") << 2.0;
        QTest::newRow("5 um") << 5.0;
        QTest::newRow("20 um") << 20.0;
    }
    void aCoplanarSheetIsItsConformalMap()
    {
        QFETCH(double, gap);
        rf::Stack stack = grounded(1, 100e-6);
        stack.layers[0].variant = QStringLiteral("open");
        const double sig = 50, wg = 160, l = 1000;
        const QList<rf::Conductor> line{strip(0, -sig / 2, l, sig), strip(0, sig / 2 + gap, l, wg), strip(0, -sig / 2 - gap - wg, l, wg)};
        rf::MeshSettings how;
        how.cellsAcross = 4;
        how.maximum = 100e-6;
        how.gaps = true;
        const rf::Mesh mesh = rf::meshOf(line, how, stack, 0);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const rf::Extraction e = rf::extract(mesh, line, stack);
        QVERIFY2(e.ok() && e.perMetre.fromMiddle, qPrintable(e.problems.join(" | ")));
        int s = 0;
        for (int i = 1; i < 3; ++i)
            if (e.c.at(i).at(i) < e.c.at(s).at(s)) s = i;
        const double c = e.perMetre.maxwell.at(s).at(s);
        const double a = sig / 2, b = a + gap, cc = b + wg;
        const double k = (a / b) * std::sqrt((1 - b * b / (cc * cc)) / (1 - a * a / (cc * cc)));
        const double exact = 4 * rf::kEps0 * ellipticK(k) / ellipticK(std::sqrt(1 - k * k));
        rows() << Row{QStringLiteral("Coplanar sheet in air, gap %1 um, C a metre").arg(gap), QStringLiteral("conformal map, finite grounds"),
                      c * 1e12, exact * 1e12, 0.005, QStringLiteral("pF/m")};
        QVERIFY2(std::abs(c / exact - 1) < 0.005, qPrintable(QStringLiteral("%1 pF/m against %2").arg(c * 1e12).arg(exact * 1e12)));
    }

    // MIM plates of no thickness, facing each other cell for cell
    // (plates as faces read 72.4 fF where 151.6 was right):
    // 40 µm square, 40 nm of eps_r 7 between them - the parallel plate's
    // eps A/d, 2.48 pF, its fringe a fraction of d over the side more. The
    // quasi-static solve at 5 µm cells within 0 and +2 % of it.
    void mimPlatesAreTheirParallelPlate()
    {
        rf::Stack stack;
        rf::Layer ground;
        ground.kind = rf::Layer::Ground;
        ground.name = QStringLiteral("GND");
        ground.tag = QStringLiteral("gnd");
        rf::Layer sub;
        sub.kind = rf::Layer::Substrate;
        sub.name = QStringLiteral("SUB");
        sub.tag = QStringLiteral("sub");
        sub.thickness = 10e-6;
        sub.epsr = 4;
        rf::Layer bottom;
        bottom.kind = rf::Layer::Metal;
        bottom.name = QStringLiteral("M1");
        bottom.tag = QStringLiteral("m1");
        rf::Layer film;
        film.kind = rf::Layer::Dielectric;
        film.name = QStringLiteral("MIM");
        film.tag = QStringLiteral("mim");
        film.thickness = 40e-9;
        film.epsr = 7;
        rf::Layer top = bottom;
        top.name = QStringLiteral("M2");
        top.tag = QStringLiteral("m2");
        stack.layers << ground << sub << bottom << film << top;
        const QList<rf::Conductor> plates{strip(0, 0, 40, 40), strip(0, 0, 40, 40, QStringLiteral("M2"))};
        rf::MeshSettings how;
        how.maximum = 5e-6;
        const rf::Mesh mesh = rf::meshOf(plates, how, stack, 0);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const rf::Extraction e = rf::extract(mesh, plates, stack);
        QVERIFY2(e.ok() && e.names.size() == 2, qPrintable(e.problems.join(" | ")));
        const double between = std::abs(e.c.at(0).at(1));
        const double plate = rf::kEps0 * 7 * 40e-6 * 40e-6 / 40e-9;
        rows() << Row{QStringLiteral("MIM plates of no thickness, C between them"), QStringLiteral("parallel plate, eps A/d"), between * 1e12,
                      plate * 1e12, 0.02, QStringLiteral("pF")};
        QVERIFY2(between >= plate && between < 1.02 * plate, qPrintable(QStringLiteral("%1 pF against %2").arg(between * 1e12).arg(plate * 1e12)));
    }

    // The table of them all, each against its reference.
    void cleanupTestCase()
    {
        QStringList out{QStringLiteral("| Benchmark | Got | Reference | Off | Held to | Source |"), QStringLiteral("|---|---|---|---|---|---|")};
        for (const Row& r : rows())
            out << QStringLiteral("| %1 | %2 %3 | %4 %3 | %5 % | %6 % | %7 |")
                       .arg(r.name)
                       .arg(r.value, 0, 'g', 6)
                       .arg(r.unit)
                       .arg(r.reference, 0, 'g', 6)
                       .arg(100 * (r.value / r.reference - 1), 0, 'f', 2)
                       .arg(100 * r.tolerance, 0, 'g', 3)
                       .arg(r.source);
        qInfo().noquote() << out.join(QLatin1Char('\n'));
    }
};

QTEST_GUILESS_MAIN(TestRf25Benchmarks)
#include "test_rf25_benchmarks.moc"
