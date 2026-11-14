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

#include <QHash>

#include "rf25/rf25_deembed.h"
#include "rf25/rf25_edge.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_model.h"
#include "rf25/rf25_static.h"

#include <cmath>
#include <complex>

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

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
    QList<double> alphas;   ///< at each frequency
    QString said;
};

/// Every line's attenuation errors as theFiveLinesAreTheir2DSolves() finds
/// them, for theLinesLossOnTheWhole(): by line, at each frequency.
QHash<QString, QList<double>>& alphaErrors()
{
    static QHash<QString, QList<double>> kept;
    return kept;
}

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
        out.alphas << da;
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

using Complex = std::complex<double>;

/// GSG pads with tapers onto the benchmark's coplanar line (S 50, G 20,
/// grounds 160), a line \a l µm long between them: the signal pad 80 µm
/// wide and 60 long, an 80 µm taper, the ground pads from 70 µm out
/// (phase 6 of the accuracy limits, B4's pads). Its length end to end in
/// \a end.
QList<rf::Conductor> padsOf(double l, double* end)
{
    const double lp = 60, lt = 80, x0 = lp + lt, x1 = lp + lt + l, e = x1 + lt + lp;
    *end = e;
    const auto shape = [](const QList<QPointF>& points) {
        rf::Conductor c;
        c.layer = QStringLiteral("M1");
        c.outline = QPolygonF(points);
        return c;
    };
    const QList<QPointF> signal = {{0, -40}, {lp, -40}, {x0, -25}, {x1, -25}, {x1 + lt, -40}, {e, -40},
                                   {e, 40},  {x1 + lt, 40}, {x1, 25}, {x0, 25}, {lp, 40}, {0, 40}};
    const QList<QPointF> up = {{0, 70}, {lp, 70}, {x0, 45}, {x1, 45}, {x1 + lt, 70}, {e, 70}, {e, 205}, {0, 205}};
    QList<QPointF> down;
    for (auto it = up.crbegin(); it != up.crend(); ++it) down << QPointF(it->x(), -it->y());
    return {shape(signal), shape(up), shape(down)};
}

/// The pads' probes: at each end an internal port on the signal pad's first
/// 10 µm, returning through the ground pads beside it.
QList<rf::FullWavePort> padProbes(double end)
{
    QList<rf::FullWavePort> out;
    int k = 0;
    for (const double x : {5.0, end - 5}) {
        rf::FullWavePort p;
        p.name = QStringLiteral("P%1").arg(out.size() + 1);
        p.internal = true;
        p.at = QPointF(x, 0);
        p.metal = QStringLiteral("M1");
        p.size = QSizeF(10, 10);
        p.terminals = true;
        for (const double y : {75.0, -75.0}) {
            rf::PortTerminal t;
            t.name = QStringLiteral("T%1").arg(++k);
            t.at = QPointF(x, y);
            t.metal = QStringLiteral("M1");
            t.size = QSizeF(10, 10);
            p.returns << t;
        }
        out << p;
    }
    return out;
}

/// The power a two-port's S loses: 1 - |S11|^2 - |S21|^2.
double lostOf(const QList<QList<Complex>>& s)
{
    return 1 - std::norm(s.at(0).at(0)) - std::norm(s.at(1).at(0));
}

/// L-2L: the attenuation (dB/m) of the line \a dl m longer in \a longer
/// than in \a shorter, their ends alike - T_long T_short^-1's eigenvalues
/// exp(-+gamma dl).
double l2lAlpha(const QList<QList<Complex>>& shorter, const QList<QList<Complex>>& longer, double dl)
{
    const auto t = [](const QList<QList<Complex>>& s) {
        Eigen::Matrix2cd m;
        const Complex s11 = s.at(0).at(0), s12 = s.at(0).at(1), s21 = s.at(1).at(0), s22 = s.at(1).at(1);
        m << -(s11 * s22 - s12 * s21) / s21, s11 / s21, -s22 / s21, 1.0 / s21;
        return m;
    };
    const Eigen::Matrix2cd m = t(longer) * t(shorter).inverse();
    const Eigen::ComplexEigenSolver<Eigen::Matrix2cd> eig(m);
    const Complex lambda = std::abs(eig.eigenvalues()(0)) > std::abs(eig.eigenvalues()(1)) ? eig.eigenvalues()(0) : eig.eigenvalues()(1);
    return std::log(lambda).real() / dl * 20 / std::log(10.0);
}

/// Phase 7's bandstop filter, our own (docs/feature_gaps/2026-11-13-rf25-filter/filter.py): a 50 Ohm microstrip
/// 250 µm wide on 254 µm of alumina over a ground plane, three pairs of open stubs - one either side -, each 250 µm
/// wide and 2850 µm from the line's edge, the pairs 2925 µm apart and 1500 µm from each end: 8850 µm in all.
QList<rf::Conductor> stubFilter()
{
    const double w = 250, stub = 2850, pitch = 2925, feed = 1500, length = 2 * feed + 2 * pitch;
    QList<QPointF> top = {{0, w / 2}};
    for (int k = 0; k < 3; ++k) {
        const double c = feed + k * pitch;
        top << QPointF(c - w / 2, w / 2) << QPointF(c - w / 2, w / 2 + stub) << QPointF(c + w / 2, w / 2 + stub) << QPointF(c + w / 2, w / 2);
    }
    top << QPointF(length, w / 2);
    QList<QPointF> outline = top;
    for (auto it = top.crbegin(); it != top.crend(); ++it) outline << QPointF(it->x(), -it->y());
    rf::Conductor c;
    c.layer = QStringLiteral("M1");
    c.outline = QPolygonF(outline);
    return {c};
}

} // namespace

class TestRf25Accuracy : public QObject
{
    Q_OBJECT

private slots:
    // The reviewers' coplanar line on quartz, its metal thin and perfect,
    // quasi-static: Palace's thin-metal Z0 73.72 Ω and eps_eff 2.4005. Each
    // edge cell's charge spread as the edge has it, d^-1/2 (the plan's
    // phase 5): 73.99, 73.85, 73.81 Ω at 6, 12 and 24 across (+0.37, +0.17,
    // +0.13 %), the second step 0.22 of the first, extrapolated 73.81. With
    // each cell's charge even it was first order - 77.67, 75.68, 74.70
    // (+5.4, +2.7, +1.3 %), the second step 0.49 of the first. eps_eff is
    // Palace's within 0.03 % either way.
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
        // Within 0.7 % of Palace at 6 across and 0.3 % at 24, converging at
        // the second order (each step under 0.35 of the last), toward
        // Palace's - Richardson's estimate of where it goes within 0.5 %.
        QVERIFY2(z0.at(0) > z0.at(1) && z0.at(1) > z0.at(2), "not converging");
        QVERIFY2(std::abs(z0.at(0) / 73.72 - 1) < 0.007, qPrintable(QString::number(z0.at(0))));
        QVERIFY2(std::abs(z0.at(2) / 73.72 - 1) < 0.003, qPrintable(QString::number(z0.at(2))));
        QVERIFY2(ratio < 0.35, qPrintable(QString::number(ratio)));
        const double limit = z0.at(2) - (z0.at(1) - z0.at(2)) * ratio / (1 - ratio);
        qInfo("extrapolated: %.3f Ohm (%+.2f %% of Palace's)", limit, 100 * (limit / 73.72 - 1));
        QVERIFY2(ratio > 0 && ratio < 1 && std::abs(limit / 73.72 - 1) < 0.005, qPrintable(QString::number(limit)));
    }

    // A thin square plate in free space, quasi-static: its charge even on
    // each cell converges at the first order (each step half the last), to
    // Richardson's limit; spread as its edges have it (the plan's phase 5),
    // each mesh is nearer that limit - within 1 % at six across, where the
    // even charge is 2.7 % under -, and the two meet. (The limit is about
    // 0.3668 times 4 pi eps0 a, the 40.8 pF of a metre square.)
    void aSquarePlatesChargeIsNearerItsLimitWithItsEdgesProfile()
    {
        if (slowBuild()) QSKIP("its 48 across, dense: past half an hour under the sanitizers");
        const rf::Stack stack = stackOf(QStringLiteral("open"), 2000e-6, 1.0, 0, 0, false);
        const QList<rf::Conductor> plate = {strip(0, 0, 100, 100)};
        const auto capacitance = [&](int across, bool profiled) {
            rf::setEdgeProfiles(profiled);
            rf::MeshSettings how;
            how.cellsAcross = across;
            how.maximum = 100e-6;
            const rf::Extraction e = rf::extract(rf::meshOf(plate, how, stack, 0), plate, stack);
            rf::setEdgeProfiles(true);
            return e.ok() ? e.selfCapacitance(0) : 0.0;
        };
        const double e12 = capacitance(12, false), e24 = capacitance(24, false), e48 = capacitance(48, false);
        const double ratio = (e48 - e24) / (e24 - e12);
        QVERIFY2(ratio > 0.4 && ratio < 0.6, qPrintable(QString::number(ratio)));   // (first order)
        const double limit = e48 + (e48 - e24) * ratio / (1 - ratio);
        const double e6 = capacitance(6, false), p6 = capacitance(6, true), p48 = capacitance(48, true);
        qInfo("a square plate: Richardson's limit of its even charge %.5g F (%.4f of 4 pi eps0 a); six across %+.2f %% even, "
              "%+.2f %% with its edges' profile; 48 across %+.3f %%",
              limit, limit / (4 * M_PI * 8.8541878128e-12 * 100e-6), 100 * (e6 / limit - 1), 100 * (p6 / limit - 1), 100 * (p48 / limit - 1));
        // (-0.52 %: d^-1/2 at an edge, 0.35 each way at a corner. A corner
        //  of d^-1/2 each way reads -0.61 %, d^-1/3 everywhere -0.65 %.)
        QVERIFY2(std::abs(p6 / limit - 1) < 0.0057, qPrintable(QString::number(p6 / limit - 1)));
        QVERIFY2(std::abs(p6 / limit - 1) < 0.5 * std::abs(e6 / limit - 1), "the profile no nearer at six across");
        QVERIFY2(std::abs(p48 / limit - 1) < 0.002, qPrintable(QString::number(p48 / limit - 1)));
    }

    // The alumina line's metal perfect, solved as its two faces, full wave
    // at 10 GHz: its L against the boundary-element solve's external
    // inductance, 3.38279e-7 H/m (wheeler.py: C0 32.8915 pF/m in vacuum over
    // the chuck). Its faces' walls carry current (A1): +0.73 %, where the
    // faces alone read +5.5 %.
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
        // (+0.35 % with the edge runs' current spread as the edge has it -
        //  the plan's phase 5 -, +0.73 % with it even across each.)
        QVERIFY2(std::abs(dl) < 0.006, qPrintable(QStringLiteral("L %1 H/m, %2 %").arg(l).arg(100 * dl)));
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
        // (The worst over each line's three frequencies, with the walls'
        //  current and charge and the edge band's loss (A1, A2): alumina R
        //  +4.4, L +1.0, alpha +3.4 %; copper +4.3, +1.5, +3.2; film -3.7,
        //  -0.6, -3.6; narrow -6.5, -0.8, -6.2; the microstrip +8.0, +1.7,
        //  +6.8 up to 10 GHz and +35 % in R at 30 - the full wave's loss
        //  beyond its metal, radiation or a substrate wave, as before them:
        //  the plan's phase 6. They were, on 12 November before them: alumina
        //  +14.8, +6.3, +8.0; copper -9.3, +3.7, -12.3; film -14.9, +5.4,
        //  -18.7; narrow -12.6, +9.2, -20.0.)
        // (And with the edge's profile in the charge and the edge runs'
        //  current (phase 5), the worst of each: alumina R -1.8, L +0.6,
        //  alpha -1.9 %; copper +1.7, +0.4, +1.8; film -6.1, -1.0, -5.8;
        //  narrow -8.1, -0.7, -7.8; the microstrip +5.1, +0.5, +4.6 up to
        //  10 GHz, +31 % in R at 30.)
        QTest::newRow("alumina") << 0 << 0.05 << 0.012 << 0.05;
        QTest::newRow("copper") << 1 << 0.03 << 0.01 << 0.03;
        QTest::newRow("film") << 2 << 0.065 << 0.015 << 0.065;
        QTest::newRow("narrow") << 3 << 0.085 << 0.01 << 0.085;
        QTest::newRow("microstrip") << 4 << 0.37 << 0.015 << 0.35;
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
        alphaErrors().insert(QLatin1String(kLines[index].name), e.alphas);
        QVERIFY2(std::abs(e.r) < rMost, qPrintable(e.said));
        QVERIFY2(std::abs(e.l) < lMost, qPrintable(e.said));
        QVERIFY2(std::abs(e.alpha) < alphaMost, qPrintable(e.said));
    }

    // The five lines' attenuation on the whole, against their 2D solves: the
    // edge band's loss (A2) within 5 % on average and 10 % at worst, where
    // crowding() was 8.6 and 19.9 on the 4 November write-up's emulator (the
    // microstrip's 30 GHz left out: its loss there is more than its metal's,
    // the plan's phase 6).
    void theLinesLossOnTheWhole()
    {
        if (slowBuild()) QSKIP("its strips, millimetres long: many minutes under the sanitizers");
        const QHash<QString, QList<double>>& all = alphaErrors();
        if (all.size() < 5) QSKIP("the five lines were not all solved (run theFiveLinesAreTheir2DSolves first)");
        double sum = 0, worst = 0;
        int n = 0;
        for (auto it = all.constBegin(); it != all.constEnd(); ++it)
            for (int k = 0; k < it.value().size(); ++k) {
                if (it.key() == QLatin1String("microstrip") && k == 2) continue;
                sum += std::abs(it.value().at(k));
                worst = std::max(worst, std::abs(it.value().at(k)));
                ++n;
            }
        const double mean = sum / n;
        qInfo("the lines' attenuation: %.1f %% from their 2D solves on average, %.1f %% at worst", 100 * mean, 100 * worst);
        QVERIFY2(mean < 0.05 && worst < 0.10, qPrintable(QStringLiteral("%1 %, %2 %").arg(100 * mean).arg(100 * worst)));
    }

    // The reviewers' coplanar line, 2 µm of gold as its two faces, 12 cells
    // across, full wave: Z0 and eps_eff against Palace 2D's and QFEM's mode
    // solves (the Palace benchmark's comparison 1). With its walls' current
    // and charge (A1), Z0 +0.37, +0.36, +0.47 % at 10, 30, 50 GHz and eps_eff
    // +0.30, +0.26, +0.23 %, where the faces alone read Z0 +2.7 % - L high,
    // C 2.4 % low.
    void theBenchmarksCoplanarLineIsPalacesAndQfems()
    {
        if (slowBuild()) QSKIP("its strips, millimetres long: many minutes under the sanitizers");
        // (Solved as Automatic, as a new model's metal is: 2 µm in 20 µm gaps
        //  is a tenth of them, past Automatic's fortieth - its two faces.)
        rf::Stack stack = stackOf(QStringLiteral("open"), 500e-6, 3.8, 2e-6, 4.1e7, false);
        stack.layers.last().automatic = true;
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
            // (eps_eff within the two references' own spread - Palace 2D's
            //  and QFEM's 0.76 % apart at 10 GHz -: +0.69 % of their mean
            //  with the edge's profile (phase 5), 0.31 % of QFEM's, which
            //  meshes the metal through its skin depth; +0.29 % without it.)
            QVERIFY2(std::abs(z / mid - 1) < 0.01, qPrintable(QStringLiteral("Z0 %1 Ohm at %2 GHz").arg(z).arg(ref[k].ghz)));
            QVERIFY2(std::abs(eps / midEps - 1) < 0.008, qPrintable(QStringLiteral("eps_eff %1 at %2 GHz").arg(eps).arg(ref[k].ghz)));
        }
    }

    // GSG pads with tapers (A3 of the open items of 8 November, phase 6 of
    // the accuracy limits): what the pads lose above 35 GHz is the
    // structure's. Their metal perfect, nothing in them dissipates, and what
    // goes is radiated - into the air and the quartz, open below - by their
    // ends: Palace 3D (the pads in a half model, lumped ports across the
    // pads' gaps, 2.3 and 3.4 million unknowns) loses as much, the ends of
    // the 500 and 1000 µm lines interfering as RF 2.5D's do. L-2L through
    // the pads then reads the ends' radiation as the line's: an attenuation
    // where the line has none, up at 60 GHz, down at 80, up at 100, in both.
    void theRadiationOfGsgPadsIsPalaces()
    {
        const rf::Stack stack = stackOf(QStringLiteral("open"), 500e-6, 3.8, 0, 0, false);
        const QList<double> ghz = {40, 50, 60, 70, 80, 90, 100};
        // Palace's: the power lost, 1 - |S11|^2 - |S21|^2, at 500 and at
        // 1000 µm, and L-2L's attenuation (dB/mm) between the two.
        const double palace500[] = {0.00019, 0.00044, 0.00111, 0.00290, 0.00698, 0.01165, 0.01278};
        const double palace1000[] = {0.00071, 0.00263, 0.00675, 0.00562, 0.00376, 0.00310, 0.00374};
        const double palaceAlpha[] = {0.0053, 0.0248, 0.0702, 0.0392, 0.0320, 0.1011, 0.1158};
        QList<double> hz;
        for (const double f : ghz) hz << f * 1e9;
        QList<rf::FullWave> solved;
        for (const double l : {500.0, 1000.0}) {
            double end = 0;
            const QList<rf::Conductor> pads = padsOf(l, &end);
            rf::MeshSettings how;
            how.cellsAcross = 3;
            how.maximum = 100e-6;
            how.gaps = true;   // (as the tab meshes: its narrow gaps graded)
            const rf::FullWave w = rf::fullWaveOf(rf::meshOf(pads, how, stack, 0), pads, stack, padProbes(end), hz);
            QVERIFY2(w.ok() && w.s.size() == ghz.size(), qPrintable(w.problems.join(" | ")));
            QVERIFY2(w.gain <= 1e-9 && w.asymmetry < 1e-9, qPrintable(QStringLiteral("gain %1, asymmetry %2").arg(w.gain).arg(w.asymmetry)));
            solved << w;
        }
        const auto dB = [](double lost) { return -10 * std::log10(1 - lost); };
        QList<double> alpha;
        for (int k = 0; k < ghz.size(); ++k) {
            const double l500 = lostOf(solved.at(0).s.at(k)), l1000 = lostOf(solved.at(1).s.at(k));
            alpha << l2lAlpha(solved.at(0).s.at(k), solved.at(1).s.at(k), 500e-6) / 1e3;
            qInfo("%g GHz: lost %.5f / %.5f (Palace %.5f / %.5f), L-2L %.4f dB/mm (Palace %.4f)", ghz.at(k), l500, l1000, palace500[k],
                  palace1000[k], alpha.last(), palaceAlpha[k]);
            // Within 0.02 dB of Palace's, either length.
            QVERIFY2(std::abs(dB(l500) - dB(palace500[k])) < 0.02 && std::abs(dB(l1000) - dB(palace1000[k])) < 0.02,
                     qPrintable(QStringLiteral("%1 GHz: %2 and %3").arg(ghz.at(k)).arg(l500).arg(l1000)));
            QVERIFY2(std::abs(alpha.last() - palaceAlpha[k]) < 0.025, qPrintable(QStringLiteral("%1 GHz: %2 dB/mm").arg(ghz.at(k)).arg(alpha.last())));
        }
        // The 1000 µm structure loses most at 60 to 70 GHz, the 500 µm one
        // the more the higher; L-2L's attenuation swings as Palace's does.
        QVERIFY(lostOf(solved.at(1).s.at(2)) > lostOf(solved.at(1).s.at(5)) && lostOf(solved.at(0).s.at(6)) > lostOf(solved.at(0).s.at(2)));
        QVERIFY(alpha.at(2) > alpha.at(4) && alpha.at(6) > alpha.at(4));
    }

    // The same pads in 2 µm of gold (solved as Automatic: two faces): below
    // 40 GHz, where their ends radiate nothing to speak of, L-2L through
    // them is the line's own attenuation - eddy.py's 2D eddy-current solve
    // of its cross-section (R 904.6, 1243.7, 1542.6, 1797.8 Ohm/m and L
    // 366.4, 363.4, 361.8, 360.7 nH/m at 10 to 40 GHz; QFEM's 55.3, 76.4,
    // 95.1, 111.2 dB/m). (Palace's Conductivity boundary on the metal cut
    // out of its mesh reads 16 to 25 % under both - in its 2D mode solve
    // too, 41.4 dB/m at 10 GHz -, so its gold is no reference for loss:
    // the pads' 2D-corrected loss is within 4 to 9 % of it, the radiation
    // the same in both; the write-up of phase 6.)
    void aLineThroughGsgPadsLosesWhatItsCrossSectionDoes()
    {
        rf::Stack gold = stackOf(QStringLiteral("open"), 500e-6, 3.8, 2e-6, 4.1e7, false);
        gold.layers.last().automatic = true;
        const rf::Stack perfect = stackOf(QStringLiteral("open"), 500e-6, 3.8, 0, 0, false);
        const QList<double> ghz = {10, 20, 30, 40};
        const double eddy[] = {55.3, 76.4, 94.9, 110.8};   // dB/m: R sqrt(eps_eff)/(2 c L), eps_eff QFEM's
        QList<double> hz;
        for (const double f : ghz) hz << f * 1e9;
        // L-2L's attenuation at each frequency, on each stack.
        const auto l2l = [&](const rf::Stack& stack, QList<double>* alpha) {
            QList<rf::FullWave> solved;
            for (const double l : {500.0, 1000.0}) {
                double end = 0;
                const QList<rf::Conductor> pads = padsOf(l, &end);
                rf::MeshSettings how;
                how.cellsAcross = 3;
                how.maximum = 100e-6;
                how.gaps = true;
                const rf::FullWave w = rf::fullWaveOf(rf::meshOf(pads, how, stack, 0), pads, stack, padProbes(end), hz);
                if (!w.ok() || w.s.size() != ghz.size() || w.gain > 1e-9) return false;
                solved << w;
            }
            for (int k = 0; k < ghz.size(); ++k) *alpha << l2lAlpha(solved.at(0).s.at(k), solved.at(1).s.at(k), 500e-6);
            return true;
        };
        QList<double> lossy, lossless;
        QVERIFY(l2l(gold, &lossy) && l2l(perfect, &lossless));
        for (int k = 0; k < ghz.size(); ++k) {
            // (Less what the perfect metal's ends read: their radiation, 3.1
            //  dB/m at 40 GHz, nothing below 30.)
            const double alpha = lossy.at(k) - lossless.at(k);
            qInfo("%g GHz: L-2L through the pads %.1f dB/m (%.1f less the perfect metal's %.1f), the cross-section's %.1f (%+.1f %%)", ghz.at(k),
                  alpha, lossy.at(k), lossless.at(k), eddy[k], 100 * (alpha / eddy[k] - 1));
            QVERIFY2(std::abs(alpha / eddy[k] - 1) < 0.03, qPrintable(QStringLiteral("%1 GHz: %2 dB/m").arg(ghz.at(k)).arg(alpha)));
        }
    }

    // Phase 7 (A4 of the open items of 8 November): a bandstop filter's
    // stopband against Palace 3D - our own filter, stubFilter(), lossless,
    // edge ports at its ends. Palace (a half model, wave ports on the board's
    // ends, 3.5 million unknowns): -20 dB from 7.4377 to 13.6737 GHz, its
    // centre 10.5557, its width 6.2360 GHz; one pair of stubs alone, 10.2541
    // to 11.5740 (its null 10.9247). RF 2.5D's within 0.5 % - its centre
    // and edges - and its width within 2 %, as the plan asked; at 12 across
    // 7.4507 to 13.7180 GHz, +0.17 and +0.32 %. (Its least |S21| sits in
    // the stopband's flat floor - -69 dB in RF 2.5D from 10.4 to 11.2 GHz,
    // -82 in Palace -, where neither says where the least is.)
    void aStubFiltersStopbandIsPalaces()
    {
        const rf::Stack stack = stackOf(QStringLiteral("perfect"), 254e-6, 9.8, 0, 0, false);
        const QList<rf::Conductor> filter = stubFilter();
        rf::MeshSettings how;
        how.cellsAcross = 3;
        how.maximum = 400e-6;
        how.gaps = true;
        const rf::Mesh mesh = rf::meshOf(filter, how, stack, 0);
        QList<rf::FullWavePort> ports;
        for (const auto& [x, facing] : {std::pair<double, double>{0, -1}, {8850, 1}}) {
            rf::FullWavePort p;
            p.name = QStringLiteral("P%1").arg(ports.size() + 1);
            p.at = QPointF(x, 0);
            p.metal = QStringLiteral("M1");
            p.facing = QPointF(facing, 0);
            ports << p;
        }
        // Where |S21| crosses -20 dB near \a from (GHz), stepping \a step.
        const auto crossing = [&](double from, double to, double step, double* at) {
            QList<double> hz;
            for (double f = from; f <= to + 1e-9; f += step) hz << f * 1e9;
            const rf::FullWave w = rf::fullWaveOf(mesh, filter, stack, ports, hz);
            if (!w.ok() || w.s.size() != hz.size()) return false;
            for (int k = 0; k + 1 < hz.size(); ++k) {
                const double a = 20 * std::log10(std::abs(w.s.at(k).at(1).at(0))), b = 20 * std::log10(std::abs(w.s.at(k + 1).at(1).at(0)));
                if ((a + 20) * (b + 20) < 0) {
                    *at = (hz.at(k) + (hz.at(k + 1) - hz.at(k)) * (-20 - a) / (b - a)) / 1e9;
                    return true;
                }
            }
            return false;
        };
        double low = 0, high = 0;
        QVERIFY2(crossing(7.25, 7.75, 0.05, &low), "no -20 dB edge from 7.25 to 7.75 GHz");
        QVERIFY2(crossing(13.45, 13.95, 0.05, &high), "no -20 dB edge from 13.45 to 13.95 GHz");
        const double palaceLow = 7.4377, palaceHigh = 13.6737;
        qInfo("-20 dB from %.4f to %.4f GHz (Palace %.4f to %.4f): edges %+.2f and %+.2f %%, centre %+.2f %%, width %+.2f %%", low, high, palaceLow,
              palaceHigh, 100 * (low / palaceLow - 1), 100 * (high / palaceHigh - 1), 100 * ((low + high) / (palaceLow + palaceHigh) - 1),
              100 * ((high - low) / (palaceHigh - palaceLow) - 1));
        QVERIFY(std::abs(low / palaceLow - 1) < 0.005 && std::abs(high / palaceHigh - 1) < 0.005);
        QVERIFY(std::abs((low + high) / (palaceLow + palaceHigh) - 1) < 0.005);
        QVERIFY(std::abs((high - low) / (palaceHigh - palaceLow) - 1) < 0.02);
    }
};

QTEST_GUILESS_MAIN(TestRf25Accuracy)
#include "test_rf25_accuracy.moc"
