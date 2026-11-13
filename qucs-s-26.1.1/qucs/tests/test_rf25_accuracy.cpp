/*
 * test_rf25_accuracy.cpp - RF 2.5D held to references that are not itself:
 *                          2D solves of each line's cross-section, a
 *                          boundary-element solve, Palace and QFEM
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The accuracy limits of the open items of 8 November
 * (docs/bug_hunts/2026-11-08-claude-rf25-open-items.md): the walls' current
 * (A1), the metal's loss (A2), and a quasi-static Z0's convergence. Each
 * reference is a number made once, outside the test, and said where it came
 * from:
 *
 * - **The 2D eddy-current solves** (eddy.py, lines.py beside
 *   docs/feature_gaps/2026-11-04-claude-rf25-fullwave-gaps.md): each line's
 *   R and L per metre, its metal in rectangles of uniform current graded to
 *   a hundredth of a micrometre at its faces, the chuck's image under it.
 * - **The boundary-element solve** (wheeler.py, there too): the alumina
 *   line's external inductance in vacuum, 1/(c^2 C0).
 * - **Palace 0.19** (a 3D and 2D finite-element solver, run once beside
 *   this work) and **QFEM** (the 2D multiphysics engine's mode solve, its
 *   metal meshed through its skin depth): the reviewers' coplanar line on
 *   quartz, S 50 / G 20 / grounds 160 µm.
 *
 * Each check holds RF 2.5D to what it reaches now, with what the reference
 * says beside it; the plan's phases tighten them as they mend each limit.
 */
#include <QtTest>

#include "rf25/rf25_deembed.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_model.h"
#include "rf25/rf25_static.h"

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;

namespace {

constexpr double kC0 = 299792458.0;

/// Whether this build is one whose dense solves are many times slower - a
/// sanitized one (QUCS_TEST_SLOW: its path in any).
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

/// A stack: a ground ("perfect" or "open"), a substrate \a height m of
/// \a epsr, one metal M1 \a thickness m of \a sigma (S/m; nought: perfect)
/// solved as its two faces or as a sheet - made as the solvers' own tests
/// make theirs (a model's file says neither a perfect metal nor a thin one).
rf::Stack stackOf(const QString& ground, double height, double epsr, double thickness, double sigma, bool faces)
{
    rf::Stack s;
    rf::Layer plane;
    plane.kind = rf::Layer::Ground;
    plane.name = QStringLiteral("GND");
    plane.tag = QStringLiteral("gnd");
    plane.variant = ground;
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
    metal.twoSheets = faces;
    s.layers << plane << sub << metal;
    return s;
}

/// A conductor on M1: a rectangle in micrometres.
rf::Conductor strip(double x, double y, double w, double h)
{
    rf::Conductor c;
    c.layer = QStringLiteral("M1");
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

/// A line's feed: coplanar where it has grounds (\a gap, \a ground in µm),
/// a microstrip's strip where it has none.
rf::Feed feedOf(double width, double gap, double ground)
{
    rf::Feed feed;
    feed.metal = QStringLiteral("M1");
    feed.width = width;
    feed.coplanar = ground > 0;
    if (feed.coplanar) {
        feed.gap[0] = feed.gap[1] = gap;
        feed.ground[0] = feed.ground[1] = ground;
    }
    return feed;
}

/// R (Ohm/m) and L (H/m) of a calibrated line at frequency index \a k.
std::pair<double, double> rlOf(const rf::Calibration& cal, int k)
{
    const std::complex<double> z = cal.gamma.at(k) * cal.impedance.at(k);
    return {z.real(), z.imag() / (2 * M_PI * cal.frequencies.at(k))};
}

/// A line's effective permittivity at frequency index \a k.
double epsOf(const rf::Calibration& cal, int k)
{
    const double beta = cal.gamma.at(k).imag();
    const double n = beta * kC0 / (2 * M_PI * cal.frequencies.at(k));
    return n * n;
}

/// One of the five lines of lines.py: signal, gap, ground (µm), thickness
/// (µm), sigma, height over the chuck (µm), cells across, the largest cell
/// (µm), and at each frequency (GHz) the 2D eddy solve's R (Ohm/m) and L
/// (H/m).
struct Line {
    const char* name;
    double s, g, wg, t, sigma, h, epsr;
    int across;
    double maximum;
    struct At {
        double ghz, r, l;
    } at[3];
};

// (lines.py's reference(), run on 12 November: eddy.py's Problem, graded
//  from min(0.01 µm, t/100) at the faces by 1.2. The microstrip's halved:
//  lines.py took its half's drop twice, though its mirror carries the other
//  half's current - 8.36e-7 H/m for a strip as wide as it is high in air,
//  where Hammerstad and Jensen's Z0/c is 4.2e-7; the 4 November comparison
//  was of ratios, which the doubling left alone. The alumina line on its
//  own alumina, as test_rf25_measured has it; the rest on 3.8, which R and
//  L do not see. The film's cells along it 50 µm, not lines.py's 20: its
//  calibration strip, 8 mm long, is otherwise past the dense limit.)
const Line kLines[] = {
    {"alumina", 50, 25, 270, 6.5, 36e6, 500, 9.7, 3, 100, {{10, 731.436, 3.49949e-7}, {30, 1267.329, 3.45061e-7}, {60, 1795.371, 3.43091e-7}}},
    {"copper", 100, 50, 250, 3, 5.8e7, 200, 3.8, 4, 100, {{2, 178.087, 3.94393e-7}, {10, 366.963, 3.89279e-7}, {30, 642.653, 3.86772e-7}}},
    {"film", 10, 5, 100, 1, 4.1e7, 500, 3.8, 3, 50, {{10, 4638.391, 3.94557e-7}, {40, 7262.752, 3.75028e-7}, {100, 11180.89, 3.67034e-7}}},
    {"narrow", 20, 6, 100, 3, 4.1e7, 100, 3.8, 3, 50, {{10, 1892.006, 2.98545e-7}, {30, 3341.747, 2.85912e-7}, {60, 4737.56, 2.80701e-7}}},
    {"microstrip", 100, 0, 0, 3, 5.8e7, 100, 3.8, 4, 100, {{2, 107.6815, 4.17892e-7}, {10, 219.878, 4.150205e-7}, {30, 383.7825, 4.13527e-7}}},
};

/// What a line solved full wave reads against its 2D solve: the largest
/// relative errors of R and L over its frequencies, and its attenuation's,
/// (1 + dR)/(1 + dL) - 1 (its eps_eff right).
struct LineErrors {
    double r = 0, l = 0, alpha = 0;
    QString said;
};

LineErrors solveLine(const Line& line)
{
    LineErrors out;
    // (A chuck under the line, as the 2D solve has it; the substrate's
    //  permittivity is nothing to R and L.)
    const rf::Stack stack = stackOf(QStringLiteral("perfect"), line.h * 1e-6, line.epsr, line.t * 1e-6, line.sigma, true);
    rf::MeshSettings how;
    how.cellsAcross = line.across;
    how.maximum = line.maximum * 1e-6;
    how.gaps = true;   // (as the tab meshes either study)
    QList<double> fs;
    for (const Line::At& a : line.at) fs << a.ghz * 1e9;
    const rf::Calibration cal = rf::calibrate(stack, how, 0, feedOf(line.s, line.g, line.wg), fs);
    if (!cal.ok()) {
        out.said = cal.problems.join(QStringLiteral(" | "));
        out.r = out.l = out.alpha = 1e9;
        return out;
    }
    QStringList said;
    for (int k = 0; k < 3; ++k) {
        const auto [r, l] = rlOf(cal, k);
        const double dr = r / line.at[k].r - 1, dl = l / line.at[k].l - 1, da = (1 + dr) / (1 + dl) - 1;
        if (std::abs(dr) > std::abs(out.r)) out.r = dr;
        if (std::abs(dl) > std::abs(out.l)) out.l = dl;
        if (std::abs(da) > std::abs(out.alpha)) out.alpha = da;
        said << QStringLiteral("%1 GHz: R %2 Ohm/m (%3 %), L %4 nH/m (%5 %), alpha %6 %")
                    .arg(line.at[k].ghz)
                    .arg(r, 0, 'f', 1)
                    .arg(100 * dr, 0, 'f', 1)
                    .arg(l * 1e9, 0, 'f', 2)
                    .arg(100 * dl, 0, 'f', 1)
                    .arg(100 * da, 0, 'f', 1);
    }
    out.said = QStringLiteral("%1: ").arg(QLatin1String(line.name)) + said.join(QStringLiteral("; "));
    return out;
}

} // namespace

class TestRf25Accuracy : public QObject
{
    Q_OBJECT

private slots:
    // The reviewers' coplanar line on quartz, its metal thin and perfect,
    // quasi-static: Palace's thin-metal Z0 73.72 Ω and eps_eff 2.4005. With
    // its charge constant on each cell, against the edge's singularity, the
    // solve converges at the first order - its step halving at each
    // doubling of the cells across: 77.67, 75.68, 74.70 Ω at 6, 12 and 24
    // (+5.4, +2.7, +1.3 %), the second step 0.49 of the first -, though to
    // Palace's: extrapolated, 73.77 Ω. eps_eff is Palace's within 0.03 %.
    void aThinCoplanarLinesZ0ConvergesOnPalaces()
    {
        const rf::Stack stack = stackOf(QStringLiteral("open"), 500e-6, 3.8, 0, 0, false);
        QVERIFY2(stack.ok(), qPrintable(stack.problems.join(" | ")));
        const double length = 1000;
        const QList<rf::Conductor> gsg = {strip(0, -25, length, 50), strip(0, 45, length, 160), strip(0, -205, length, 160)};
        QList<double> z0;
        for (const int across : {6, 12, 24}) {
            rf::MeshSettings how;
            how.cellsAcross = across;
            how.maximum = 100e-6;
            const rf::Mesh mesh = rf::meshOf(gsg, how, stack, 0);
            const rf::Extraction e = rf::extract(mesh, gsg, stack);
            QVERIFY2(e.ok() && e.signalNets().size() == 1, qPrintable(e.problems.join(" | ") + e.returnsSaid));
            const rf::LineParameters lp = rf::lineParameters(e, e.signalNets().first(), length * 1e-6);
            QVERIFY2(lp.ok(), qPrintable(lp.problems.join(" | ")));
            z0 << lp.z0;
            qInfo("thin coplanar line, %d across: Z0 %.3f Ohm (%+.2f %% of Palace's 73.72), eps_eff %.4f (%+.2f %% of 2.4005)", across,
                  lp.z0, 100 * (lp.z0 / 73.72 - 1), lp.epsilonEff, 100 * (lp.epsilonEff / 2.4005 - 1));
        }
        const double ratio = (z0.at(1) - z0.at(2)) / (z0.at(0) - z0.at(1));
        qInfo("its steps: %.3f then %.3f Ohm, the second %.3f of the first", z0.at(0) - z0.at(1), z0.at(1) - z0.at(2), ratio);
        // Now: within 6 % of Palace at 6 across and 1.5 % at 24, converging
        // (each finer nearer), toward Palace's - Richardson's estimate of
        // where it goes within 0.5 % of it.
        QVERIFY2(z0.at(0) > z0.at(1) && z0.at(1) > z0.at(2), "not converging");
        QVERIFY2(std::abs(z0.at(0) / 73.72 - 1) < 0.06, qPrintable(QString::number(z0.at(0))));
        QVERIFY2(std::abs(z0.at(2) / 73.72 - 1) < 0.015, qPrintable(QString::number(z0.at(2))));
        const double limit = z0.at(2) - (z0.at(1) - z0.at(2)) * ratio / (1 - ratio);
        qInfo("extrapolated: %.3f Ohm (%+.2f %% of Palace's)", limit, 100 * (limit / 73.72 - 1));
        QVERIFY2(ratio > 0 && ratio < 1 && std::abs(limit / 73.72 - 1) < 0.005, qPrintable(QString::number(limit)));
    }

    // The alumina line's metal perfect, solved as its two faces, full wave
    // at 10 GHz: its L against the boundary-element solve's external
    // inductance, 3.38279e-7 H/m (wheeler.py: C0 32.8915 pF/m in vacuum over
    // the chuck). Its faces carry no current down their walls: L over it.
    void aPerfectMetalsInductanceIsTheBoundaryElementSolves()
    {
        if (slowBuild()) QSKIP("its strips, millimetres long: many minutes under the sanitizers");
        const rf::Stack stack = stackOf(QStringLiteral("perfect"), 500e-6, 9.7, 6.5e-6, 0, true);
        QVERIFY2(stack.ok(), qPrintable(stack.problems.join(" | ")));
        rf::MeshSettings how;
        how.cellsAcross = 3;
        how.maximum = 100e-6;
        how.gaps = true;
        const rf::Calibration cal = rf::calibrate(stack, how, 0, feedOf(50, 25, 270), {10e9});
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        const auto [r, l] = rlOf(cal, 0);
        const double dl = l / 3.38279e-7 - 1;
        qInfo("a perfect metal's L: %.5g H/m, %+.2f %% of the boundary-element solve's (R %.3g Ohm/m)", l, 100 * dl, r);
        QVERIFY2(std::abs(r) < 0.01 * 2 * M_PI * 10e9 * l, qPrintable(QStringLiteral("R %1 Ohm/m of a perfect metal").arg(r)));
        // (Now +5.5 %.)
        QVERIFY2(std::abs(dl) < 0.06, qPrintable(QStringLiteral("L %1 H/m, %2 %").arg(l).arg(100 * dl)));
    }

    // The five lines of lines.py, full wave as the tab meshes them, each
    // against its 2D eddy solve: R and L, and what they make of the
    // attenuation.
    void theFiveLinesAreTheir2DSolves_data()
    {
        QTest::addColumn<int>("index");
        QTest::addColumn<double>("rMost");
        QTest::addColumn<double>("lMost");
        QTest::addColumn<double>("alphaMost");
        // (Now, 12 November, the worst over each line's three frequencies:
        //  alumina R +14.8, L +6.3, alpha +8.0 %; copper -9.3, +3.7, -12.3;
        //  film -14.9, +5.4, -18.7; narrow -12.6, +9.2, -20.0; the
        //  microstrip -5.0 at 2 GHz but +32 % at 30 - the full wave's own
        //  loss beyond the 2D solve's metal, its radiation or its substrate
        //  wave -, L +2.8. L is high on every line: its walls carry no
        //  current (A1). The plan's phases 2 and 4 tighten these.)
        QTest::newRow("alumina") << 0 << 0.16 << 0.07 << 0.10;
        QTest::newRow("copper") << 1 << 0.11 << 0.05 << 0.14;
        QTest::newRow("film") << 2 << 0.17 << 0.06 << 0.20;
        QTest::newRow("narrow") << 3 << 0.14 << 0.10 << 0.22;
        QTest::newRow("microstrip") << 4 << 0.35 << 0.04 << 0.30;
    }
    void theFiveLinesAreTheir2DSolves()
    {
        if (slowBuild()) QSKIP("its strips, millimetres long: many minutes under the sanitizers");
        QFETCH(int, index);
        QFETCH(double, rMost);
        QFETCH(double, lMost);
        QFETCH(double, alphaMost);
        const LineErrors e = solveLine(kLines[index]);
        qInfo("%s", qPrintable(e.said));
        QVERIFY2(std::abs(e.r) < rMost, qPrintable(e.said));
        QVERIFY2(std::abs(e.l) < lMost, qPrintable(e.said));
        QVERIFY2(std::abs(e.alpha) < alphaMost, qPrintable(e.said));
    }

    // The reviewers' coplanar line, 2 µm of gold as its two faces, 12 cells
    // across, full wave: Z0 and eps_eff against Palace 2D's and QFEM's mode
    // solves (the Palace benchmark's comparison 1). With no current down
    // its walls its L is high, and Z0 with it, about 2.7 %; eps_eff right.
    void theBenchmarksCoplanarLineIsPalacesAndQfems()
    {
        if (slowBuild()) QSKIP("its strips, millimetres long: many minutes under the sanitizers");
        const rf::Stack stack = stackOf(QStringLiteral("open"), 500e-6, 3.8, 2e-6, 4.1e7, true);
        QVERIFY2(stack.ok(), qPrintable(stack.problems.join(" | ")));
        rf::MeshSettings how;
        how.cellsAcross = 12;
        how.maximum = 100e-6;   // (about a twentieth of a wave at 100 GHz, the benchmark's top)
        how.gaps = true;
        const struct {
            double ghz, palaceZ, palaceEps, qfemZ, qfemEps;
        } ref[] = {{10, 70.7, 2.373, 71.1, 2.391}, {30, 70.5, 2.355, 70.6, 2.361}, {50, 70.3, 2.345, 70.4, 2.350}};
        QList<double> fs;
        for (const auto& r : ref) fs << r.ghz * 1e9;
        const rf::Calibration cal = rf::calibrate(stack, how, 0, feedOf(50, 20, 160), fs);
        QVERIFY2(cal.ok(), qPrintable(cal.problems.join(" | ")));
        for (int k = 0; k < 3; ++k) {
            const double z = cal.impedance.at(k).real(), eps = epsOf(cal, k);
            const double mid = 0.5 * (ref[k].palaceZ + ref[k].qfemZ), midEps = 0.5 * (ref[k].palaceEps + ref[k].qfemEps);
            qInfo("%g GHz: Z0 %.2f Ohm (Palace 2D %.1f, QFEM %.1f: %+.2f %% of their mean), eps_eff %.4f (%.3f, %.3f: %+.2f %%)", ref[k].ghz, z,
                  ref[k].palaceZ, ref[k].qfemZ, 100 * (z / mid - 1), eps, ref[k].palaceEps, ref[k].qfemEps, 100 * (eps / midEps - 1));
            // (Now +2.73, +2.71, +2.76 %; eps_eff +0.66, +0.62, +0.46 %.)
            QVERIFY2(std::abs(z / mid - 1) < 0.03, qPrintable(QStringLiteral("Z0 %1 Ohm at %2 GHz").arg(z).arg(ref[k].ghz)));
            QVERIFY2(std::abs(eps / midEps - 1) < 0.01, qPrintable(QStringLiteral("eps_eff %1 at %2 GHz").arg(eps).arg(ref[k].ghz)));
        }
    }
};

QTEST_GUILESS_MAIN(TestRf25Accuracy)
#include "test_rf25_accuracy.moc"
