/*
 * test_rf25_static.cpp - the quasi-static extraction (rf25/rf25_static.h):
 * the rectangle's closed form against a brute-force integral, a
 * parallel-plate capacitor, a microstrip's Z0 and eps_eff against
 * Hammerstad and Jensen's closed form of 1980, and a coupled pair's matrix.
 *
 * Phase B of docs/proposals/2026-10-23-rf-2p5d-moment-method.md, whose
 * test list asks for "a parallel-plate capacitor ... a microstrip's Z0
 * ... a two-line coupling".
 */
#include <QtTest>

#include "rf25/rf25_sparams.h"
#include "rf25/rf25_static.h"

#include <cmath>

namespace rf = qucs_s::rf25;

namespace {

/// A stack of a substrate of \a epsr and \a height (m) on a ground plane,
/// with one metal on top of it and air above.
rf::Stack grounded(double epsr, double height)
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
    metal.thickness = 0;
    s.layers << ground << sub << metal;
    return s;
}

/// A stack of two metals: a ground plane, \a lower m of substrate with M1
/// on it, \a gap m of \a epsr with M2 on that, and a via between them.
rf::Stack twoMetals(double epsr, double lower, double gap)
{
    rf::Stack s;
    const auto layer = [](rf::Layer::Kind kind, const QString& name, double thickness, double er) {
        rf::Layer l;
        l.kind = kind;
        l.name = name;
        l.tag = name.toLower();
        l.thickness = thickness;
        l.epsr = er;
        return l;
    };
    rf::Layer via = layer(rf::Layer::Via, QStringLiteral("V1"), 0, 1);
    via.from = QStringLiteral("M1");
    via.to = QStringLiteral("M2");
    s.layers << layer(rf::Layer::Ground, QStringLiteral("GND"), 0, 1)
             << layer(rf::Layer::Substrate, QStringLiteral("SUB"), lower, epsr)
             << layer(rf::Layer::Metal, QStringLiteral("M1"), 0, 1)
             << layer(rf::Layer::Dielectric, QStringLiteral("ILD"), gap, epsr)
             << via
             << layer(rf::Layer::Metal, QStringLiteral("M2"), 0, 1);
    return s;
}

/// A stack of several dielectrics on a ground plane with one metal on
/// top: \a each is (epsr, thickness in m), from the bottom up.
rf::Stack layered(const QList<QPair<double, double>>& each)
{
    rf::Stack s;
    rf::Layer ground;
    ground.kind = rf::Layer::Ground;
    ground.name = QStringLiteral("GND");
    ground.tag = QStringLiteral("gnd");
    s.layers << ground;
    int n = 0;
    for (const auto& it : each) {
        rf::Layer d;
        d.kind = n == 0 ? rf::Layer::Substrate : rf::Layer::Dielectric;
        d.name = QStringLiteral("D%1").arg(++n);
        d.tag = d.name.toLower();
        d.thickness = it.second;
        d.epsr = it.first;
        s.layers << d;
    }
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    s.layers << metal;
    return s;
}

/// A conductor on M1: a rectangle in micrometres.
rf::Conductor strip(double x, double y, double w, double h, const QString& layer = QStringLiteral("M1"))
{
    rf::Conductor c;
    c.layer = layer;
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

/// Those conductors meshed with cells of \a cell micrometres.
rf::Mesh meshed(const QList<rf::Conductor>& conductors, double cell, bool edges = true)
{
    rf::MeshSettings how;
    how.maximum = cell * 1e-6;
    how.edges = edges;
    // (Of that size alone, as these were written; the tests of cells across
    // a conductor set their own.)
    how.cellsAcross = 0;
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

/// Hammerstad and Jensen's microstrip (IEEE MTT-S 1980), zero thickness:
/// its effective permittivity and characteristic impedance for a strip of
/// width \a w on a substrate \a h thick of \a epsr. They give it as good to
/// 0.01 % for Z0 in air and 0.2 % for eps_eff, and a 2D finite-difference
/// solve of the same cross-section agrees with it to 0.1 % and 0.2 % (the
/// phase B check of 24 October). The form this test held to before - "6.620,
/// 49.21" at W/h = 1 - was Hammerstad's of 1975, 0.7 % high in eps_eff.
QPair<double, double> hammerstadJensen(double w, double h, double epsr)
{
    const double u = w / h;
    const double a = 1 + std::log((std::pow(u, 4) + std::pow(u / 52, 2)) / (std::pow(u, 4) + 0.432)) / 49
                     + std::log(1 + std::pow(u / 18.1, 3)) / 18.7;
    const double b = 0.564 * std::pow((epsr - 0.9) / (epsr + 3), 0.053);
    const double eff = (epsr + 1) / 2 + (epsr - 1) / 2 * std::pow(1 + 10 / u, -a * b);
    const double f = 6 + (2 * M_PI - 6) * std::exp(-std::pow(30.666 / u, 0.7528));
    const double eta0 = 4e-7 * M_PI * rf::kLightSpeed;   // mu0 c0
    const double z01 = eta0 / (2 * M_PI) * std::log(f / u + std::sqrt(1 + 4 / (u * u)));
    return {eff, z01 / std::sqrt(eff)};
}

/// Hammerstad and Jensen's air line with its thickness correction: the
/// impedance of a strip \a w wide and \a t thick, \a h over a ground, in
/// vacuum.
double z01Thick(double w, double h, double t)
{
    const double eta0 = 4e-7 * M_PI * rf::kLightSpeed;
    double u = w / h;
    const double t1 = t / h;
    if (t1 > 0) {
        const double coth = 1 / std::tanh(std::sqrt(6.517 * u));
        u += t1 / M_PI * std::log(1 + 4 * M_E / (t1 * coth * coth));
    }
    const double f = 6 + (2 * M_PI - 6) * std::exp(-std::pow(30.666 / u, 0.7528));
    return eta0 / (2 * M_PI) * std::log(f / u + std::sqrt(1 + 4 / (u * u)));
}

/// Wheeler's incremental-inductance rule on it: R = (Rs/eta0) dZ01/dn, the
/// strip's every wall receding (its edges in, its faces in, its bottom up)
/// and, apart, the ground's (down). The strip's share and the ground's,
/// Ohm/m. A separate 2D solve of the whole thick perimeter puts this 0.4 to
/// 1.2 % above the exact value at W/h 0.3 to 3.
QPair<double, double> wheelerOnHammerstadJensen(double w, double h, double t, double rs, double rsGround)
{
    const double eta0 = 4e-7 * M_PI * rf::kLightSpeed, dn = 1e-10;
    const double base = z01Thick(w, h, t);
    const double strip = (z01Thick(w - 2 * dn, h + dn, t - 2 * dn) - base) / dn;
    const double ground = (z01Thick(w, h + dn, t) - base) / dn;
    return {rs / eta0 * strip, rsGround / eta0 * ground};
}

bool says(const QStringList& lines, const char* words)
{
    for (const QString& l : lines)
        if (l.contains(QLatin1String(words), Qt::CaseInsensitive)) return true;
    return false;
}

} // namespace

class TestRf25Static : public QObject
{
    Q_OBJECT

private slots:
    // The potential of a uniformly charged rectangle, in closed form,
    // against the same thing integrated the slow way.
    void theRectanglesClosedFormIsTheIntegral()
    {
        const QRectF rect(0, 0, 3e-6, 1e-6);
        const QList<QPointF> points{QPointF(1.5e-6, 0.5e-6), QPointF(0, 0), QPointF(3e-6, 1e-6),
                                    QPointF(-2e-6, 0.5e-6),  QPointF(10e-6, 10e-6)};
        for (double z : {0.0, 0.3e-6, 2e-6}) {
            for (const QPointF& at : points) {
                // A midpoint sum over the rectangle, fine enough to settle.
                const int n = 400;
                double sum = 0;
                const double dx = rect.width() / n, dy = rect.height() / n;
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j) {
                        const double x = rect.left() + (i + 0.5) * dx - at.x();
                        const double y = rect.top() + (j + 0.5) * dy - at.y();
                        sum += dx * dy / std::sqrt(x * x + y * y + z * z);
                    }
                const double brute = sum / (rect.width() * rect.height());
                const double closed = rf::rectanglePotential(rect, at, z);
                // (A point on the rectangle itself makes the sum converge
                // slowly, so it is held less tightly there.)
                const bool inside = rect.contains(at) || z == 0;
                QVERIFY2(std::abs(closed - brute) <= (inside ? 2e-3 : 1e-5) * std::abs(brute),
                         qPrintable(QStringLiteral("z %1, at (%2, %3): closed %4 vs %5 (%6 relative)")
                                        .arg(z).arg(at.x()).arg(at.y()).arg(closed).arg(brute)
                                        .arg(std::abs(closed - brute) / std::abs(brute))));
            }
        }
        // Far away it is a point charge: 1/r.
        const double far = rf::rectanglePotential(rect, QPointF(1000e-6, 0), 0);
        QVERIFY(std::abs(far - 1.0 / std::hypot(1000e-6 - 1.5e-6, 0.5e-6)) < 1e-4 * far);
        // A rectangle of no area has no potential.
        QCOMPARE(rf::rectanglePotential(QRectF(0, 0, 0, 1e-6), QPointF(1e-6, 0), 0), 0.0);
    }

    // The cells of the mesh grouped into the conductors they were cut
    // from, and what it refuses.
    void theCellsAreGroupedIntoConductors()
    {
        const QList<rf::Conductor> two{strip(0, 0, 20, 4), strip(40, 0, 20, 4)};
        const rf::Mesh mesh = meshed(two, 4);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const QList<rf::Net> nets = rf::netsOf(mesh, two, grounded(4.0, 10e-6));
        QCOMPARE(nets.size(), 2);
        QCOMPARE(nets.at(0).name, QStringLiteral("M1"));
        QCOMPARE(nets.at(1).name, QStringLiteral("M1 #2"));
        QCOMPARE(nets.at(0).layers, QStringList{QStringLiteral("M1")});
        int all = 0;
        for (const rf::Net& n : nets) {
            QVERIFY(!n.cells.isEmpty());
            all += n.cells.size();
        }
        QCOMPARE(all, mesh.cells.size());

        // Nothing meshed is nothing to solve.
        rf::Mesh empty;
        QVERIFY(!rf::extract(empty, two, grounded(4.0, 10e-6)).ok());
        QVERIFY2(rf::extract(empty, two, grounded(4.0, 10e-6)).problems.first().contains(QStringLiteral("no cells")),
                 qPrintable(rf::extract(empty, two, grounded(4.0, 10e-6)).problems.join(" | ")));
    }

    // A plate over a ground plane: the parallel-plate value plus the
    // fringing a real solver finds, which is more, and less of it the
    // wider the plate is.
    void aPlateOverAGroundPlaneIsItsParallelPlateValuePlusFringing()
    {
        const double h = 10e-6;
        const rf::Stack stack = grounded(1.0, h);   // vacuum, so C0 and C are one
        struct Case {
            double side;   // µm
            double cell;
        };
        double previousExcess = 1e9;
        for (const Case& c : {Case{40, 5}, Case{80, 8}, Case{160, 16}}) {
            const QList<rf::Conductor> plate{strip(0, 0, c.side, c.side)};
            const rf::Mesh mesh = meshed(plate, c.cell);
            QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
            const rf::Extraction e = rf::extract(mesh, plate, stack);
            QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
            QCOMPARE(e.names.size(), 1);
            const double ideal = rf::kEps0 * std::pow(c.side * 1e-6, 2) / h;
            const double got = e.selfCapacitance(0);
            const double excess = got / ideal - 1;
            QVERIFY2(got > ideal, qPrintable(QStringLiteral("side %1 µm: %2 F vs the plate's %3 F").arg(c.side).arg(got).arg(ideal)));
            // What the edges add is one perimeter's worth of field, so it
            // is proportional to the height over the width - measured at
            // 4.7, 5.0 and 5.3 times h/a as the plate widens, the slow
            // drift being the logarithm the edge correction carries.
            const double perEdge = excess * (c.side * 1e-6) / h;
            QVERIFY2(perEdge > 4.0 && perEdge < 6.5,
                     qPrintable(QStringLiteral("side %1 µm: the edges add %2 of the plate's value, %3 times h/a")
                                    .arg(c.side).arg(excess).arg(perEdge)));
            // The wider the plate, the smaller the share the edges take.
            QVERIFY2(excess < previousExcess,
                     qPrintable(QStringLiteral("side %1 µm: fringing %2 of the plate's value, was %3")
                                    .arg(c.side).arg(excess).arg(previousExcess)));
            previousExcess = excess;
            // In vacuum there is nothing to tell C and C0 apart.
            QVERIFY(std::abs(e.c.at(0).at(0) - e.c0.at(0).at(0)) < 1e-12 * std::abs(e.c.at(0).at(0)));
        }
    }

    // A plate over a stack of several dielectrics: the parallel-plate
    // value of dielectrics in series, and the fringing above it. This is
    // the case where the images far down the stack carry real weight -
    // four slabs put four per cent of the potential past the strongest
    // sixty-four of them, where one slab puts a part in 1e11.
    void aPlateOverSeveralDielectricsIsTheirSeriesCapacitance()
    {
        const QList<QPair<double, double>> stack{{11.9, 4e-6}, {4.1, 2e-6}, {3.0, 2e-6}, {7.0, 2e-6}};
        double overEpsr = 0, height = 0;
        for (const auto& it : stack) {
            overEpsr += it.second / it.first;   // d/epsr, which is what adds in series
            height += it.second;
        }
        double previousExcess = 1e9;
        for (double side : {40.0, 80.0, 160.0}) {
            const QList<rf::Conductor> plate{strip(0, 0, side, side)};
            const rf::Mesh mesh = meshed(plate, side / 8);
            QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
            const rf::Extraction e = rf::extract(mesh, plate, layered(stack));
            QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
            const double ideal = rf::kEps0 * std::pow(side * 1e-6, 2) / overEpsr;
            const double got = e.selfCapacitance(0);
            QVERIFY2(got > ideal, qPrintable(QStringLiteral("side %1 µm: %2 F vs the series value %3 F").arg(side).arg(got).arg(ideal)));
            // The same edge correction as over one dielectric, by the
            // whole height of the stack: measured at 4 to 6 times it.
            const double perEdge = (got / ideal - 1) * (side * 1e-6) / height;
            // Measured at 2.20, 2.10 and 1.94 as the plate widens - the
            // same edge correction as over one dielectric, with a smaller
            // constant because the fringing field runs through the top
            // layer rather than the series average. It is this that the
            // images far down the stack hold up: without them it reads
            // 2.27, 2.47, 4.81, rising where it should fall.
            QVERIFY2(perEdge > 1.5 && perEdge < 3.0,
                     qPrintable(QStringLiteral("side %1 µm: the edges add %2 of the series value, %3 times h/a")
                                    .arg(side).arg(got / ideal - 1).arg(perEdge)));
            QVERIFY(got / ideal - 1 < previousExcess);
            previousExcess = got / ideal - 1;
        }
    }

    // A microstrip's per-metre values, as the solver takes them - of its
    // middle cross-section, as if it ran on for ever - against Hammerstad
    // and Jensen's closed form at three widths: a narrow strip and both
    // branches of the older forms, so that one number agreeing is not taken
    // for a method that works. The same from a line of 400 µm and of 800,
    // which the whole over the length is not; and nearer the closed form
    // with twice the cells across.
    void aMicrostripsImpedanceIsThatOfTheClosedForm()
    {
        const double h = 100e-6, epsr = 9.8;
        const rf::Stack stack = grounded(epsr, h);
        const auto solve = [&](double w, double length, int across) {
            const QList<rf::Conductor> line{strip(0, 0, w, length)};
            rf::MeshSettings how;
            how.maximum = 40e-6;
            how.cellsAcross = across;
            return rf::extract(rf::meshOf(line, how, rf::Stack(), 0), line, stack);
        };
        rf::Extraction kept;
        double narrowZ0 = 0;
        for (const double w : {30.0, 100.0, 300.0}) {
            const rf::Extraction shortLine = solve(w, 400, 10), longLine = solve(w, 800, 10);
            QVERIFY2(shortLine.ok(), qPrintable(shortLine.problems.join(" | ")));
            QVERIFY2(longLine.ok(), qPrintable(longLine.problems.join(" | ")));
            QVERIFY2(longLine.perMetre.fromMiddle, qPrintable(longLine.perMetre.said));
            QVERIFY2(longLine.perMetre.said.contains(QStringLiteral("cross-section")), qPrintable(longLine.perMetre.said));
            const rf::LineParameters a = rf::lineParameters(shortLine, 0, 400e-6), b = rf::lineParameters(longLine, 0, 800e-6);
            QVERIFY2(a.ok() && b.ok(), qPrintable((a.problems + b.problems).join(" | ")));
            // The same per metre whatever the length: the ends are not in it.
            QVERIFY2(std::abs(a.c - b.c) <= 2e-3 * b.c && std::abs(a.c0 - b.c0) <= 2e-3 * b.c0,
                     qPrintable(QStringLiteral("W %1: C %2 and %3 pF/m, C0 %4 and %5 over 400 and 800 µm")
                                    .arg(w).arg(a.c * 1e12).arg(b.c * 1e12).arg(a.c0 * 1e12).arg(b.c0 * 1e12)));
            // Where the whole over the length has both ends' fringing in it,
            // and the more of it the shorter the line.
            const double whole400 = shortLine.selfCapacitance(0) / 400e-6, whole800 = longLine.selfCapacitance(0) / 800e-6;
            QVERIFY2(whole400 > whole800 && whole800 > 1.02 * b.c,
                     qPrintable(QStringLiteral("W %1: the whole over 400 µm %2, over 800 %3, per metre %4 pF/m")
                                    .arg(w).arg(whole400 * 1e12).arg(whole800 * 1e12).arg(b.c * 1e12)));
            const QPair<double, double> want = hammerstadJensen(w * 1e-6, h, epsr);
            qWarning("microstrip W/h = %.2f: C %.5g pF/m, C0 %.5g pF/m, eps_eff %.4f (H&J %.4f), Z0 %.3f ohm (H&J %.3f)",
                     w * 1e-6 / h, b.c * 1e12, b.c0 * 1e12, b.epsilonEff, want.first, b.z0, want.second);
            // Ten cells across: eps_eff within 0.5 % and Z0 within 1.5 % of
            // a closed form good to 0.2 % and 0.1 % - measured at 0.06, 0.12
            // and 0.06 % on eps_eff and 0.26, 0.37 and 1.0 % on Z0 for W/h
            // 0.3, 1 and 3. (Square cells of 10 µm give 0.8 % at W/h = 1:
            // these 40 µm along are a little nearer by a cancellation, the
            // error being of the cells across, first order in their size.)
            QVERIFY2(std::abs(b.epsilonEff - want.first) <= 0.005 * want.first,
                     qPrintable(QStringLiteral("W/h %1: eps_eff %2 vs %3").arg(w * 1e-6 / h).arg(b.epsilonEff).arg(want.first)));
            QVERIFY2(std::abs(b.z0 - want.second) <= 0.015 * want.second,
                     qPrintable(QStringLiteral("W/h %1: Z0 %2 vs %3").arg(w * 1e-6 / h).arg(b.z0).arg(want.second)));
            // A wider line is a lower impedance.
            if (narrowZ0 > 0) QVERIFY2(b.z0 < narrowZ0, qPrintable(QStringLiteral("W %1: Z0 %2 after %3").arg(w).arg(b.z0).arg(narrowZ0)));
            narrowZ0 = b.z0;
            if (w == 100.0) kept = longLine;
        }
        // Twice the cells across the 50 Ohm strip, and nearer: the error is
        // the mesh's, and it shrinks as the mesh does (0.37 % to 0.13 %).
        {
            const QPair<double, double> want = hammerstadJensen(100e-6, h, epsr);
            const rf::LineParameters coarse = rf::lineParameters(kept, 0, 800e-6);
            const rf::Extraction fine = solve(100, 800, 20);
            QVERIFY2(fine.ok(), qPrintable(fine.problems.join(" | ")));
            const rf::LineParameters lp = rf::lineParameters(fine, 0, 800e-6);
            qWarning("microstrip W/h = 1, 20 across: eps_eff %.4f, Z0 %.3f ohm", lp.epsilonEff, lp.z0);
            QVERIFY2(std::abs(lp.z0 - want.second) < 0.7 * std::abs(coarse.z0 - want.second),
                     qPrintable(QStringLiteral("Z0 %1 with 20 across, %2 with 10, %3 the closed form")
                                    .arg(lp.z0).arg(coarse.z0).arg(want.second)));
        }

        // And the line parameters say the same, by the relations the
        // multiphysics engine's lineparams uses.
        const rf::LineParameters lp = rf::lineParameters(kept, 0, 800e-6);
        QVERIFY2(lp.ok(), qPrintable(lp.problems.join(" | ")));
        QCOMPARE(lp.length, 800e-6);
        QVERIFY(lp.c > 0 && lp.c0 > 0);
        QVERIFY2(std::abs(lp.epsilonEff - lp.c / lp.c0) < 1e-12 * lp.epsilonEff, "eps_eff is C/C0");
        QVERIFY2(std::abs(lp.z0 - 1.0 / (rf::kLightSpeed * std::sqrt(lp.c * lp.c0))) < 1e-9 * lp.z0, "Z0 is 1/(c0 sqrt(C C0))");
        QVERIFY2(std::abs(lp.l - 1.0 / (rf::kLightSpeed * rf::kLightSpeed * lp.c0)) < 1e-9 * lp.l, "L is 1/(c0^2 C0)");
        QVERIFY2(std::abs(lp.nEff - std::sqrt(lp.epsilonEff)) < 1e-12, "n_eff is the root of eps_eff");
        QVERIFY2(std::abs(lp.velocity * lp.nEff - rf::kLightSpeed) < 1e-3, "v is c0 over n_eff");
        // A length of nothing, and a conductor that is not there.
        QVERIFY(!rf::lineParameters(kept, 0, 0).ok());
        QVERIFY(!rf::lineParameters(kept, 7, 1e-3).ok());
    }

    // The surface impedance of a slab, against the two limits it has in
    // closed form: a metal many skin depths thick, and one thin beside
    // one.
    void theSurfaceImpedanceIsThatOfASlab()
    {
        const double sigma = 5.8e7;   // copper
        constexpr double mu0 = 4e-7 * M_PI;
        for (double f : {1e6, 1e8, 1e10}) {
            const double depth = std::sqrt(2.0 / (2 * M_PI * f * mu0 * sigma));
            // Many skin depths thick: Rs tends to 1/(sigma delta), and
            // the reactance equals the resistance.
            const std::complex<double> thick = rf::surfaceImpedance(sigma, 60 * depth, f);
            QVERIFY2(std::abs(thick.real() - 1.0 / (sigma * depth)) <= 1e-6 / (sigma * depth),
                     qPrintable(QStringLiteral("f %1: %2 vs %3").arg(f).arg(thick.real()).arg(1.0 / (sigma * depth))));
            QVERIFY2(std::abs(thick.imag() - thick.real()) <= 1e-6 * thick.real(), "a thick metal's reactance is its resistance");
            // Thin beside one: it tends to the direct-current 1/(sigma t),
            // and its reactance falls away.
            const double t = depth / 100;
            const std::complex<double> thin = rf::surfaceImpedance(sigma, t, f);
            QVERIFY2(std::abs(thin.real() - 1.0 / (sigma * t)) <= 1e-3 / (sigma * t),
                     qPrintable(QStringLiteral("f %1: %2 vs %3").arg(f).arg(thin.real()).arg(1.0 / (sigma * t))));
            QVERIFY2(thin.imag() < 0.01 * thin.real(), "a thin metal is a resistance");
            // And between them it rises with frequency.
            QVERIFY(rf::surfaceImpedance(sigma, 1e-6, f).real() > 0);
            // Thick enough that cosh and sinh of it would both overflow a
            // double: the answer is still the 1/(sigma delta) it tends
            // to, and not the nothing that inf over inf gives.
            const std::complex<double> huge = rf::surfaceImpedance(sigma, 2000 * depth, f);
            QVERIFY2(std::isfinite(huge.real()) && std::isfinite(huge.imag()),
                     qPrintable(QStringLiteral("f %1: %2").arg(f).arg(huge.real())));
            QVERIFY2(std::abs(huge.real() - 1.0 / (sigma * depth)) <= 1e-9 / (sigma * depth),
                     qPrintable(QStringLiteral("f %1: %2 vs %3").arg(f).arg(huge.real()).arg(1.0 / (sigma * depth))));
        }
        // A metal that says no conductivity or no thickness is perfect.
        QCOMPARE(rf::surfaceImpedance(0, 1e-6, 1e9), std::complex<double>(0, 0));
        QCOMPARE(rf::surfaceImpedance(5.8e7, 0, 1e9), std::complex<double>(0, 0));
        QCOMPARE(rf::surfaceImpedance(5.8e7, 1e-6, 0), std::complex<double>(0, 0));
    }

    // R and G of a microstrip: the resistance from the surface impedance
    // and how the charge crowds, the conductance from the substrate's
    // loss tangent, and what the two make of the line.
    void aLinesResistanceAndConductanceAreThoseOfItsLosses()
    {
        const double w = 200.0, h = 100e-6, epsr = 9.8, tand = 0.002, sigma = 5.8e7, thick = 3e-6;
        rf::Stack stack = grounded(epsr, h);
        // Give the substrate its loss and the metal its conductivity.
        for (rf::Layer& l : stack.layers) {
            if (l.kind == rf::Layer::Substrate) l.tand = tand;
            if (l.kind == rf::Layer::Metal) {
                l.sigma = sigma;
                l.thickness = thick;
            }
        }
        const QList<rf::Conductor> line{strip(0, 0, w, 600)};
        const rf::Extraction e = rf::extract(meshed(line, 50), line, stack);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        QCOMPARE(e.crowding.size(), 1);
        QCOMPARE(e.lossTangent.size(), 1);
        QVERIFY2(e.crowding.first() > 0, "the charge crowds nowhere at all");
        // Nearly all of this line's field is in the substrate, so its
        // effective loss tangent is nearly the substrate's own.
        QVERIFY2(e.lossTangent.first() > 0.5 * tand && e.lossTangent.first() < tand,
                 qPrintable(QStringLiteral("%1 vs the substrate's %2").arg(e.lossTangent.first()).arg(tand)));

        const double f = 10e9;
        const rf::LineParameters lp = rf::lineParameters(e, 0, 600e-6, f);
        QVERIFY2(lp.ok(), qPrintable(lp.problems.join(" | ")));
        QCOMPARE(lp.frequency, f);
        // The skin depth of copper at 10 GHz is 0.66 µm, so a 3 µm metal
        // is thick beside it and Rs is 1/(sigma delta) = 26 mOhm.
        QVERIFY2(std::abs(lp.skinDepth - 6.6e-7) < 2e-8, qPrintable(QString::number(lp.skinDepth)));
        QVERIFY2(std::abs(lp.surfaceResistance - 0.0261) < 1e-3, qPrintable(QString::number(lp.surfaceResistance)));
        // R is Wheeler's, of the strip's faces - both of them, so less than
        // the Rs/W of a current on one - and the ground, being perfect,
        // loses nothing.
        const QPair<double, double> wheeler = wheelerOnHammerstadJensen(w * 1e-6, h, thick, lp.surfaceResistance, 0);
        QVERIFY2(std::abs(lp.r - wheeler.first) < 0.03 * wheeler.first,
                 qPrintable(QStringLiteral("%1 vs Wheeler's %2 Ohm/m").arg(lp.r).arg(wheeler.first)));
        QCOMPARE(lp.rGround, 0.0);
        QVERIFY2(std::abs(lp.rDc - 1.0 / (sigma * w * 1e-6 * thick)) < 1e-9 * lp.rDc, qPrintable(QString::number(lp.rDc)));
        // G is w C tand, with the effective loss tangent.
        QVERIFY2(std::abs(lp.g - 2 * M_PI * f * lp.c * e.lossTangent.first()) <= 1e-9 * lp.g, "G is w C tand");
        // The characteristic impedance is near the lossless one, since a
        // good line loses little a wavelength, and the attenuation is
        // small and positive.
        QVERIFY2(std::abs(lp.zc.real() - lp.z0) < 0.05 * lp.z0,
                 qPrintable(QStringLiteral("%1 vs %2").arg(lp.zc.real()).arg(lp.z0)));
        QVERIFY(lp.zc.imag() < 0);   // (a lossy line's Zc lags)
        QVERIFY2(lp.alphaDb > 0 && lp.alphaDb < 500,
                 qPrintable(QStringLiteral("%1 dB/m").arg(lp.alphaDb)));
        // In decibels, which is 20/ln(10) of the nepers gamma carries.
        QVERIFY2(std::abs(lp.alphaDb - lp.gamma.real() * 20.0 / std::log(10.0)) <= 1e-9 * lp.alphaDb,
                 qPrintable(QStringLiteral("%1 dB/m vs %2 Np/m").arg(lp.alphaDb).arg(lp.gamma.real())));
        // Beta is near w/v of the lossless line.
        const double beta = 2 * M_PI * f / lp.velocity;
        QVERIFY2(std::abs(lp.gamma.imag() - beta) < 0.05 * beta,
                 qPrintable(QStringLiteral("%1 vs %2").arg(lp.gamma.imag()).arg(beta)));

        // At no frequency there is no R and no G, and the quasi-static
        // values stand alone.
        const rf::LineParameters quiet = rf::lineParameters(e, 0, 600e-6);
        QVERIFY(quiet.ok());
        QCOMPARE(quiet.r, 0.0);
        QCOMPARE(quiet.g, 0.0);
        QCOMPARE(quiet.frequency, 0.0);
        QCOMPARE(quiet.z0, lp.z0);
        // And none of what a frequency makes: zero, and not the nothing
        // that zero over zero gives, nor the infinity that a skin depth
        // of no frequency is.
        QCOMPARE(quiet.zc, std::complex<double>(0, 0));
        QCOMPARE(quiet.gamma, std::complex<double>(0, 0));
        QCOMPARE(quiet.alphaDb, 0.0);
        QVERIFY(quiet.skinDepth == 0.0);
        QVERIFY(quiet.surfaceResistance == 0.0);

        // A metal that says no conductivity loses nothing.
        rf::Stack perfect = grounded(epsr, h);
        const rf::Extraction pe = rf::extract(meshed(line, 50), line, perfect);
        QVERIFY(pe.ok());
        const rf::LineParameters plp = rf::lineParameters(pe, 0, 600e-6, f);
        QCOMPARE(plp.r, 0.0);
        QCOMPARE(plp.g, 0.0);
    }

    // Two lines side by side: the matrix is symmetric, the coupling is a
    // negative off-diagonal term, and it falls as they are moved apart.
    void twoLinesCoupleAndTheCouplingFallsWithTheGap()
    {
        const rf::Stack stack = grounded(4.1, 10e-6);
        double previous = 0;
        for (double gap : {5.0, 20.0, 80.0}) {
            const QList<rf::Conductor> pair{strip(0, 0, 20, 200), strip(20 + gap, 0, 20, 200)};
            const rf::Mesh mesh = meshed(pair, 10);
            QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
            const rf::Extraction e = rf::extract(mesh, pair, stack);
            QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
            QCOMPARE(e.names.size(), 2);
            // Symmetric, as a capacitance matrix must be - to what point
            // matching allows: each cell's potential is matched at its
            // centre, and the half-wide cells at the conductors' edges make
            // that a little asymmetric (1e-5 measured, at a gap of 5 µm).
            QVERIFY2(std::abs(e.c.at(0).at(1) - e.c.at(1).at(0)) <= 1e-4 * std::abs(e.c.at(0).at(1)),
                     qPrintable(QStringLiteral("gap %1: %2 vs %3").arg(gap).arg(e.c.at(0).at(1)).arg(e.c.at(1).at(0))));
            // Two of the same line, so the same self capacitance.
            QVERIFY2(std::abs(e.selfCapacitance(0) - e.selfCapacitance(1)) <= 1e-3 * std::abs(e.selfCapacitance(0)),
                     qPrintable(QStringLiteral("gap %1: %2 vs %3").arg(gap).arg(e.selfCapacitance(0)).arg(e.selfCapacitance(1))));
            // What a line's C is: the charge on it when it alone is at a
            // volt, which is the Maxwell matrix's own diagonal - not the
            // mutual matrix's, which is its capacitance to ground alone
            // and is smaller by the coupling. With one conductor the two
            // are the same number, so only a pair tells them apart.
            QVERIFY2(std::abs(e.selfCapacitance(0) - e.maxwell.at(0).at(0)) <= 1e-9 * std::abs(e.maxwell.at(0).at(0)),
                     qPrintable(QStringLiteral("gap %1: %2 vs the Maxwell diagonal %3")
                                    .arg(gap).arg(e.selfCapacitance(0)).arg(e.maxwell.at(0).at(0))));
            QVERIFY2(e.selfCapacitance(0) > e.c.at(0).at(0) * 1.001,
                     qPrintable(QStringLiteral("gap %1: %2 vs the capacitance to ground %3")
                                    .arg(gap).arg(e.selfCapacitance(0)).arg(e.c.at(0).at(0))));
            // The mutual term is a capacitance between them: positive, and
            // the Maxwell term it comes from is negative.
            const double mutual = e.c.at(0).at(1);
            QVERIFY2(mutual > 0, qPrintable(QStringLiteral("gap %1: mutual %2").arg(gap).arg(mutual)));
            QVERIFY(e.maxwell.at(0).at(1) < 0);
            if (previous > 0)
                QVERIFY2(mutual < previous,
                         qPrintable(QStringLiteral("gap %1 µm: mutual %2 F, was %3 F at the narrower gap").arg(gap).arg(mutual).arg(previous)));
            previous = mutual;
        }
    }

    // Two plates facing each other across a dielectric: the capacitance
    // between them is the parallel-plate value and the fringing, which is
    // what the cross-layer Green's function is for.
    void twoPlatesFacingEachOtherAreAParallelPlateCapacitor()
    {
        const double epsr = 4.2, gap = 1e-6;
        const rf::Stack stack = twoMetals(epsr, 50e-6, gap);
        double previousExcess = 1e9;
        for (double side : {10.0, 20.0, 40.0}) {
            const QList<rf::Conductor> plates{strip(0, 0, side, side),
                                              strip(0, 0, side, side, QStringLiteral("M2"))};
            const rf::Mesh mesh = meshed(plates, side / 5);
            QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
            const rf::Extraction e = rf::extract(mesh, plates, stack);
            QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
            QCOMPARE(e.names.size(), 2);
            QCOMPARE(e.names, (QStringList{QStringLiteral("M1"), QStringLiteral("M2")}));
            // The two plates are one above the other, so their mutual
            // term is the capacitor: the parallel-plate value and more,
            // and the more falls away as the plates widen.
            const double ideal = rf::kEps0 * epsr * std::pow(side * 1e-6, 2) / gap;
            const double mutual = e.c.at(0).at(1);
            QVERIFY2(mutual > ideal, qPrintable(QStringLiteral("side %1 µm: %2 F vs the plate's %3 F").arg(side).arg(mutual).arg(ideal)));
            const double excess = mutual / ideal - 1;
            QVERIFY2(excess < previousExcess,
                     qPrintable(QStringLiteral("side %1 µm: %2 over the plate's value, was %3").arg(side).arg(excess).arg(previousExcess)));
            previousExcess = excess;
            // Symmetric, and each plate's charge is mostly on the other.
            // Symmetric to a part in ten thousand, which is what
            // collocation gives: a coefficient is the charge spread over
            // one cell seen at another's centre, and that is only equal
            // both ways round for cells of a size - which the edge
            // refinement sees to it they are not. A Galerkin formulation
            // would be symmetric exactly, at the price of a second
            // integral for every pair. Measured at 3e-5 here, far below
            // the method's own per cent.
            // Symmetric to a part in ten thousand, which is what
            // collocation gives and not better: a coefficient is the
            // charge spread over one cell seen at another's centre, and
            // that is equal both ways round only for cells of a size.
            // Measured at 3e-5, and it is 3e-5 however the pairs are
            // keyed or the near test judged - the asymmetry is the
            // method's, not the bookkeeping's.
            QVERIFY2(std::abs(e.c.at(0).at(1) - e.c.at(1).at(0)) <= 1e-4 * std::abs(mutual),
                     qPrintable(QStringLiteral("side %1 µm: %2 vs %3").arg(side).arg(e.c.at(0).at(1)).arg(e.c.at(1).at(0))));
            QVERIFY2(mutual > 0.5 * e.selfCapacitance(0),
                     qPrintable(QStringLiteral("side %1 µm: mutual %2 of the plate's own %3").arg(side).arg(mutual).arg(e.selfCapacitance(0))));
        }
    }

    // A via joins what it lands on: two conductors on two metals become
    // one net, held at one potential, and carry no charge of their own.
    void aViaJoinsTwoMetalsIntoOneNet()
    {
        const rf::Stack stack = twoMetals(4.2, 20e-6, 1e-6);
        // An M1 pad, an M2 pad over part of it, and a via where they meet.
        const QList<rf::Conductor> apart{strip(0, 0, 20, 20), strip(10, 0, 20, 20, QStringLiteral("M2"))};
        const rf::Extraction two = rf::extract(meshed(apart, 5), apart, stack);
        QVERIFY2(two.ok(), qPrintable(two.problems.join(" | ")));
        QCOMPARE(two.names.size(), 2);

        QList<rf::Conductor> joined = apart;
        joined << strip(12, 2, 4, 4, QStringLiteral("V1"));
        const rf::Extraction one = rf::extract(meshed(joined, 5), joined, stack);
        QVERIFY2(one.ok(), qPrintable(one.problems.join(" | ")));
        QCOMPARE(one.names.size(), 1);
        QCOMPARE(one.names.first(), QStringLiteral("M1+M2"));
        // The via itself is no unknown: joining them cannot add cells.
        QVERIFY2(one.unknowns == two.unknowns,
                 qPrintable(QStringLiteral("%1 unknowns joined, %2 apart").arg(one.unknowns).arg(two.unknowns)));
        // Joining two conductors is adding up their matrix: the charge on
        // the joined net at a volt is every term of the two-net Maxwell
        // matrix together, because the cells and their coefficients are
        // the same and only the voltages they are held at changed. Far
        // less than the two apart, since neither screens the other any
        // more - that is what the negative off-diagonal terms were.
        double every = 0;
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) every += two.maxwell.at(i).at(j);
        QVERIFY2(std::abs(one.selfCapacitance(0) - every) <= 1e-9 * std::abs(every),
                 qPrintable(QStringLiteral("%1 joined vs %2 summed from the two apart").arg(one.selfCapacitance(0)).arg(every)));
        QVERIFY2(one.selfCapacitance(0) < two.selfCapacitance(0),
                 qPrintable(QStringLiteral("%1 joined vs %2 with the other grounded")
                                .arg(one.selfCapacitance(0)).arg(two.selfCapacitance(0))));
        // A via that lands on nothing joins nothing.
        QList<rf::Conductor> missed = apart;
        missed << strip(200, 200, 4, 4, QStringLiteral("V1"));
        QCOMPARE(rf::extract(meshed(missed, 5), missed, stack).names.size(), 2);
    }

    // Two lines crossing on two metals: they couple where they cross, and
    // the coupling grows as the layers are brought together.
    void linesOnTwoMetalsCoupleWhereTheyCross()
    {
        double previous = 0;
        for (double gap : {4.0, 2.0, 1.0}) {
            const rf::Stack stack = twoMetals(4.2, 30e-6, gap * 1e-6);
            const QList<rf::Conductor> cross{strip(0, 20, 60, 10), strip(25, 0, 10, 60, QStringLiteral("M2"))};
            const rf::Mesh mesh = meshed(cross, 10);
            const rf::Extraction e = rf::extract(mesh, cross, stack);
            QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
            QCOMPARE(e.names.size(), 2);
            const double mutual = e.c.at(0).at(1);
            QVERIFY2(mutual > 0, qPrintable(QStringLiteral("gap %1 µm: mutual %2").arg(gap).arg(mutual)));
            if (previous > 0)
                QVERIFY2(mutual > previous,
                         qPrintable(QStringLiteral("gap %1 µm: mutual %2 F, was %3 F at the wider gap").arg(gap).arg(mutual).arg(previous)));
            previous = mutual;
        }
    }

    // What it will not answer: a layer that is
    // no metal, a stack with nothing to solve in, and a mesh too large for
    // a dense matrix.
    // The ground is read for what it is, not where the tree has it: a
    // plane under everything wherever it is put, none at all where it
    // says open, and a metal lid refused rather than read as air. It was
    // read by its place - refused after any slab as "above the stack" -
    // so the one order the solver took was the one the stack refused, and
    // "Open (no plane)" was solved as a perfect plane (the phase A check
    // of 24 October, and what came of mending it).
    void theGroundIsReadForWhatItIsNotWhereItIs()
    {
        // The blank model's stack - a substrate, an oxide, a metal, its own
        // ground taken out - with a ground put in at \a at (the end where
        // it is -1) and a cover.
        const auto with = [](int at, const QString& groundKind, const QString& coverKind) {
            rf::Model m = rf::Model::blank();
            rf::Node& stack = m.stackNode();
            stack.children.erase(stack.children.begin());   // (its ground)
            if (!coverKind.isEmpty()) {
                rf::Node cover = m.make(QStringLiteral("rf_cover"));
                cover.props.insert(QStringLiteral("kind"), coverKind);
                stack.children.push_back(cover);
            }
            rf::Node ground = m.make(QStringLiteral("rf_ground"));
            ground.label = QStringLiteral("GND");
            if (!groundKind.isEmpty()) ground.props.insert(QStringLiteral("kind"), groundKind);
            stack.children.insert(stack.children.begin() + (at < 0 ? int(stack.children.size()) : at), ground);
            return m.stack();
        };
        // Wherever it is, a plane below, and the stack whole.
        for (const int at : {0, 1, 2, 3, -1}) {
            const rf::Stack st = with(at, QString(), QStringLiteral("open"));
            QVERIFY2(st.ok(), qPrintable(QStringLiteral("at %1: ").arg(at) + st.problems.join(" | ")));
            const rf::Layers l = rf::layersOf(st);
            QVERIFY2(l.ok(), qPrintable(QStringLiteral("at %1: ").arg(at) + l.problems.join(" | ")));
            QVERIFY2(l.groundBelow, qPrintable(QStringLiteral("a ground at %1 was not taken as the plane below").arg(at)));
            QCOMPARE(l.slabs.size(), 2);
        }
        // Open is no plane, and a surface impedance is still one.
        QVERIFY(!rf::layersOf(with(0, QStringLiteral("open"), QString())).groundBelow);
        QVERIFY(rf::layersOf(with(0, QStringLiteral("impedance"), QString())).groundBelow);
        QVERIFY(rf::layersOf(with(0, QStringLiteral("perfect"), QString())).groundBelow);
        // A metal plane is a plane for the charge too, and is read with its
        // conductivity, for its share of the loss.
        QVERIFY(rf::layersOf(with(0, QStringLiteral("metal"), QString())).groundBelow);
        {
            rf::Model m = rf::Model::blank();
            rf::Node& ground = m.stackNode().children.front();
            ground.props.insert(QStringLiteral("kind"), QStringLiteral("metal"));
            ground.props.insert(QStringLiteral("sigma"), QStringLiteral("4.1e7"));
            const rf::Stack st = m.stack();
            QVERIFY2(st.ok(), qPrintable(st.problems.join(" | ")));
            QCOMPARE(st.layers.first().variant, QStringLiteral("metal"));
            QCOMPARE(st.layers.first().sigma, 4.1e7);
        }
        // A metal lid is not solved yet, and says so; an open cover is air.
        const rf::Layers lid = rf::layersOf(with(0, QString(), QStringLiteral("perfect")));
        QVERIFY2(!lid.ok() && says(lid.problems, "metal lid"), qPrintable(lid.problems.join(" | ")));
        QVERIFY(rf::layersOf(with(0, QString(), QStringLiteral("open"))).ok());

        // And the charge does not care where the tree put the ground: the
        // same capacitance to the last bit, first or last.
        const QList<rf::Conductor> line{strip(0, 0, 600, 40)};
        const rf::Mesh mesh = meshed(line, 40);
        const rf::Extraction first = rf::extract(mesh, line, with(0, QString(), QStringLiteral("open")));
        const rf::Extraction last = rf::extract(mesh, line, with(-1, QString(), QStringLiteral("open")));
        QVERIFY2(first.ok(), qPrintable(first.problems.join(" | ")));
        QVERIFY2(last.ok(), qPrintable(last.problems.join(" | ")));
        QCOMPARE(first.selfCapacitance(0), last.selfCapacitance(0));
        // Without the plane it holds far less: the charge has nothing near
        // to face. (It was the plane's capacitance however "open" it said.)
        const rf::Extraction open = rf::extract(mesh, line, with(0, QStringLiteral("open"), QString()));
        QVERIFY2(open.ok(), qPrintable(open.problems.join(" | ")));
        QVERIFY2(open.selfCapacitance(0) < 0.8 * first.selfCapacitance(0),
                 qPrintable(QStringLiteral("open %1 F vs a plane's %2 F").arg(open.selfCapacitance(0)).arg(first.selfCapacitance(0))));
    }

    // R by Wheeler's incremental-inductance rule on the line's own thick
    // cross-section, against the rule on Hammerstad and Jensen's thick air
    // line at three widths (the phase B check's F4): the same with twenty
    // cells across as with ten, where the crowding of a zero-thickness
    // sheet grew 10 % from 5 µm cells to 2.5; a metal ground's share
    // Wheeler's too; at a frequency whose skin depth dwarfs the metal, its
    // resistance at direct current; and a metal of no thickness the sheet's
    // crowding, said.
    void aLinesResistanceIsWheelersOnItsThickness()
    {
        const double h = 100e-6, t = 3e-6, sigma = 5.8e7, f = 10e9;
        const auto stackOf = [&](const QString& groundKind) {
            rf::Stack stack = grounded(9.8, h);
            for (rf::Layer& l : stack.layers) {
                if (l.kind == rf::Layer::Metal) {
                    l.sigma = sigma;
                    l.thickness = t;
                }
                if (l.kind == rf::Layer::Ground) {
                    l.variant = groundKind;
                    if (groundKind == QLatin1String("metal")) l.sigma = sigma;
                }
            }
            return stack;
        };
        // (Short, and its cells long along it: R is of the cross-section,
        // and the solve of the whole line - all this costs - wants no more.
        // Under the sanitizers 800 µm lines took five minutes. Four times
        // as long as it is wide, though, or its length is taken across.)
        const auto lengthOf = [](double w) { return std::max(200.0, 4 * w); };
        const auto solve = [&](double w, int across, const rf::Stack& stack) {
            const QList<rf::Conductor> line{strip(0, 0, w, lengthOf(w))};
            rf::MeshSettings how;
            how.maximum = 40e-6;
            how.cellsAcross = across;
            how.stretch = 40;
            return rf::extract(rf::meshOf(line, how, rf::Stack(), 0), line, stack);
        };
        const double rs = rf::surfaceImpedance(sigma, t, f).real();
        for (const double w : {30.0, 100.0, 300.0}) {
            const QPair<double, double> want = wheelerOnHammerstadJensen(w * 1e-6, h, t, rs, rs);
            const rf::Extraction ten = solve(w, 10, stackOf(QStringLiteral("perfect")));
            const rf::Extraction twenty = solve(w, 20, stackOf(QStringLiteral("perfect")));
            QVERIFY2(ten.ok() && twenty.ok(), qPrintable((ten.problems + twenty.problems).join(" | ")));
            QCOMPARE(ten.perMetre.wheeler.size(), 1);
            QVERIFY2(says(ten.lines, "Wheeler's rule"), qPrintable(ten.lines.join(" | ")));
            const double length = lengthOf(w) * 1e-6;
            const rf::LineParameters a = rf::lineParameters(ten, 0, length, f), b = rf::lineParameters(twenty, 0, length, f);
            QVERIFY2(a.ok() && b.ok(), qPrintable((a.problems + b.problems).join(" | ")));
            qWarning("W %.0f um: R %.1f (ten across), %.1f (twenty), Wheeler on H&J %.1f Ohm/m", w, a.r, b.r, want.first);
            // Within 3 % of the rule on the closed form - which is itself
            // 0.4 to 1.2 % above the exact - measured at -0.9, -0.6 and
            // -1.2 % for W 30, 100 and 300 (and within 0.2 % of the exact
            // solve of the whole perimeter: 616.2, 226.4, 80.5 Ohm/m).
            QVERIFY2(std::abs(a.r - want.first) < 0.03 * want.first,
                     qPrintable(QStringLiteral("W %1: R %2 vs %3").arg(w).arg(a.r).arg(want.first)));
            // Converged: twenty across within 1 % of ten (measured 0.1 % or
            // less).
            QVERIFY2(std::abs(b.r - a.r) < 0.01 * a.r, qPrintable(QStringLiteral("W %1: R %2 and %3").arg(w).arg(a.r).arg(b.r)));
            // A perfect ground loses nothing; a copper one its share, as
            // Wheeler's rule on the closed form has it: its factor times
            // copper's surface resistance - and, solved with a copper
            // ground at one width, that share added to the strip's.
            QCOMPARE(a.rGround, 0.0);
            // (A ground plane's copper is taken as deep: its bulk 1/(sigma
            // delta), not a 3 µm slab's.)
            const double rsBulk = std::sqrt(M_PI * f * 4e-7 * M_PI / sigma);
            const double copperShare = rsBulk * ten.perMetre.wheelerGround.at(0).at(0);
            qWarning("W %.0f um: the copper ground's %.2f Ohm/m, Wheeler on H&J %.2f", w, copperShare, want.second);
            QVERIFY2(std::abs(copperShare - want.second) < 0.03 * want.second,
                     qPrintable(QStringLiteral("W %1: ground %2 vs %3").arg(w).arg(copperShare).arg(want.second)));
            if (w == 100.0) {
                const rf::Extraction copper = solve(w, 10, stackOf(QStringLiteral("metal")));
                QVERIFY2(copper.ok(), qPrintable(copper.problems.join(" | ")));
                QCOMPARE(copper.groundKind, QStringLiteral("metal"));
                const rf::LineParameters c = rf::lineParameters(copper, 0, length, f);
                QVERIFY2(std::abs(c.rGround - copperShare) < 1e-9 * copperShare,
                         qPrintable(QStringLiteral("%1 vs %2").arg(c.rGround).arg(copperShare)));
                QVERIFY2(std::abs(c.r - (a.r + c.rGround)) < 1e-9 * c.r, "the ground's share added to the strip's");
            }
            // At 1 MHz the skin depth is 66 µm, twenty times the metal: the
            // current runs through all of it, and R is 1/(sigma w t).
            const rf::LineParameters dc = rf::lineParameters(ten, 0, length, 1e6);
            const double bar = 1.0 / (sigma * w * 1e-6 * t);
            QVERIFY2(std::abs(dc.r - bar) < 1e-9 * bar, qPrintable(QStringLiteral("W %1: %2 vs %3").arg(w).arg(dc.r).arg(bar)));
            QCOMPARE(dc.rDc, bar);
        }
        // A metal of no thickness: the sheet's crowding, and said why.
        rf::Stack sheet = stackOf(QStringLiteral("perfect"));
        for (rf::Layer& l : sheet.layers)
            if (l.kind == rf::Layer::Metal) l.thickness = 0;
        const rf::Extraction none = solve(100, 10, sheet);
        QVERIFY(none.ok());
        QVERIFY(none.perMetre.wheeler.isEmpty());
        QVERIFY2(says(none.lines, "has no thickness"), qPrintable(none.lines.join(" | ")));
        // Nor with no ground to return through.
        rf::Stack open = stackOf(QStringLiteral("perfect"));
        open.layers.removeFirst();
        const QList<rf::Conductor> pair{strip(0, 0, 100, 600), strip(150, 0, 100, 600)};
        rf::MeshSettings how;
        how.maximum = 40e-6;
        how.stretch = 40;
        const rf::Extraction lone = rf::extract(rf::meshOf(pair, how, rf::Stack(), 0), pair, open);
        QVERIFY(lone.ok() && lone.perMetre.wheeler.isEmpty());
        QVERIFY2(says(lone.lines, "With no ground, R"), qPrintable(lone.lines.join(" | ")));
    }

    // A metal's roughness raises its loss as Hammerstad and Bekkadal have
    // it - the node said so, and nothing read it: 1 + (2/pi) atan(1.4
    // (rms/delta)^2), nothing while the roughness is small beside the skin
    // depth, twice the loss at most. Of the metal's faces alone: the
    // ground's share, and the resistance at direct current, are not moved.
    void aMetalsRoughnessRaisesItsLoss()
    {
        const double sigma = 5.8e7, f = 10e9;
        const double depth = std::sqrt(2.0 / (2 * M_PI * f * 4e-7 * M_PI * sigma));
        QCOMPARE(rf::roughnessFactor(0, sigma, f), 1.0);
        QCOMPARE(rf::roughnessFactor(1e-6, sigma, 0), 1.0);
        QVERIFY(std::abs(rf::roughnessFactor(depth, sigma, f) - (1 + 2 / M_PI * std::atan(1.4))) < 1e-12);
        QVERIFY(rf::roughnessFactor(1e-4, sigma, f) > 1.999 && rf::roughnessFactor(1e-4, sigma, f) < 2);
        QVERIFY(rf::roughnessFactor(1e-9, sigma, f) < 1.0001);

        const auto stackOf = [&](double rms) {
            rf::Stack stack = grounded(9.8, 100e-6);
            for (rf::Layer& l : stack.layers) {
                if (l.kind == rf::Layer::Metal) {
                    l.sigma = sigma;
                    l.thickness = 3e-6;
                    l.roughness = rms;
                }
                if (l.kind == rf::Layer::Ground) {
                    l.variant = QStringLiteral("metal");
                    l.sigma = sigma;
                }
            }
            return stack;
        };
        const QList<rf::Conductor> line{strip(0, 0, 100, 400)};
        rf::MeshSettings how;
        how.maximum = 40e-6;
        how.stretch = 40;
        const rf::Mesh mesh = rf::meshOf(line, how, rf::Stack(), 0);
        const rf::Extraction smooth = rf::extract(mesh, line, stackOf(0)), rough = rf::extract(mesh, line, stackOf(1e-6));
        QVERIFY2(smooth.ok() && rough.ok(), qPrintable((smooth.problems + rough.problems).join(" | ")));
        const rf::LineParameters a = rf::lineParameters(smooth, 0, 400e-6, f), b = rf::lineParameters(rough, 0, 400e-6, f);
        const double k = rf::roughnessFactor(1e-6, sigma, f);
        QCOMPARE(a.roughnessFactor, 1.0);
        QVERIFY2(std::abs(b.roughnessFactor - k) < 1e-12, qPrintable(QString::number(b.roughnessFactor)));
        QVERIFY2(std::abs((b.r - b.rGround) - k * (a.r - a.rGround)) < 1e-9 * b.r,
                 qPrintable(QStringLiteral("rough %1, smooth %2, factor %3").arg(b.r - b.rGround).arg(a.r - a.rGround).arg(k)));
        QVERIFY2(std::abs(b.rGround - a.rGround) < 1e-12 * a.rGround, "the ground's share moved with the metal's roughness");
        QCOMPARE(b.rDc, a.rDc);
        QVERIFY(b.alphaDb > a.alphaDb);
    }

    // A coupled pair per metre, of its cross-section: its even and odd
    // impedances against the reviewer's own 2D finite-difference values
    // for W/h 1, s/h 0.5 over eps_r 9.8 (59.5 and 37.4 Ohm), the same from
    // two lengths of it.
    void aCoupledPairsModesAreOfItsCrossSection()
    {
        const double h = 100e-6, epsr = 9.8;
        const rf::Stack stack = grounded(epsr, h);
        const auto solve = [&](double length) {
            const QList<rf::Conductor> pair{strip(0, 0, 100, length), strip(150, 0, 100, length)};
            rf::MeshSettings how;   // ten across, as the mesh's own default
            how.maximum = 40e-6;
            return rf::extract(rf::meshOf(pair, how, rf::Stack(), 0), pair, stack);
        };
        const rf::Extraction shortPair = solve(500), longPair = solve(1000);
        QVERIFY2(shortPair.ok() && longPair.ok(), qPrintable((shortPair.problems + longPair.problems).join(" | ")));
        QVERIFY2(longPair.perMetre.fromMiddle, qPrintable(longPair.perMetre.said));
        const qucs_s::fem::CoupledLines a = rf::asCoupledLines(shortPair, 500e-6), b = rf::asCoupledLines(longPair, 1000e-6);
        QVERIFY2(a.error.isEmpty() && b.error.isEmpty(), qPrintable(a.error + b.error));
        QCOMPARE(b.modes.size(), std::size_t(2));
        const auto mode = [](const qucs_s::fem::CoupledLines& l, const char* name) {
            for (const qucs_s::fem::LineMode& m : l.modes)
                if (m.name == QLatin1String(name)) return m.Z.real();
            return 0.0;
        };
        const double even = mode(b, "even"), odd = mode(b, "odd");
        qWarning("coupled pair W/h 1, s/h 0.5: Z_even %.3f ohm (2D 59.5), Z_odd %.3f ohm (2D 37.4)", even, odd);
        // Within 2 % and 3 % - measured at 59.71 and 37.69, 0.36 % and
        // 0.79 % high, the error the mesh's, as a single line's is.
        QVERIFY2(std::abs(even - 59.5) < 0.02 * 59.5, qPrintable(QStringLiteral("Z_even %1").arg(even)));
        QVERIFY2(std::abs(odd - 37.4) < 0.03 * 37.4, qPrintable(QStringLiteral("Z_odd %1").arg(odd)));
        // The same whatever its length: the ends are not in it.
        QVERIFY2(std::abs(mode(a, "even") - even) < 2e-3 * even && std::abs(mode(a, "odd") - odd) < 2e-3 * odd,
                 qPrintable(QStringLiteral("500 µm: %1 and %2, 1000 µm: %3 and %4")
                                .arg(mode(a, "even")).arg(mode(a, "odd")).arg(even).arg(odd)));
    }

    // One conductor and no ground is no line: a capacitance to infinity,
    // and no Z0 or S parameters made of it (the phase B check's F6).
    void aLoneConductorWithNoGroundIsNoLine()
    {
        rf::Stack open = grounded(9.8, 100e-6);
        open.layers.removeFirst();   // (the ground)
        rf::MeshSettings how;
        how.maximum = 40e-6;
        const QList<rf::Conductor> line{strip(0, 0, 100, 1000)};
        const rf::Extraction e = rf::extract(rf::meshOf(line, how, rf::Stack(), 0), line, open);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));   // its capacitance is an answer
        QVERIFY(!e.grounded);
        QVERIFY(e.selfCapacitance(0) > 0);
        const rf::LineParameters lp = rf::lineParameters(e, 0, 1e-3, 10e9);
        QVERIFY(!lp.ok());
        QVERIFY2(says(lp.problems, "no line"), qPrintable(lp.problems.join(" | ")));
        QVERIFY2(e.perMetre.said.contains(QStringLiteral("no values per metre")), qPrintable(e.perMetre.said));
        // Two of them with no ground: between them a line, said with what
        // a capacitance to infinity per metre is not.
        const QList<rf::Conductor> pair{strip(0, 0, 100, 1000), strip(150, 0, 100, 1000)};
        const rf::Extraction two = rf::extract(rf::meshOf(pair, how, rf::Stack(), 0), pair, open);
        QVERIFY2(two.ok(), qPrintable(two.problems.join(" | ")));
        QVERIFY2(two.perMetre.said.contains(QStringLiteral("With no ground")), qPrintable(two.perMetre.said));
        // And with the ground under it, the one strip is a line.
        const rf::Extraction grounded1 = rf::extract(rf::meshOf(line, how, rf::Stack(), 0), line, grounded(9.8, 100e-6));
        QVERIFY(grounded1.grounded);
        QVERIFY2(rf::lineParameters(grounded1, 0, 1e-3, 10e9).ok(), qPrintable(rf::lineParameters(grounded1, 0, 1e-3).problems.join(" | ")));
    }

    // What is no uniform line - a bend, a taper - has no cross-section to
    // be of, and its per-metre values are the whole over its length, said.
    void whatIsNoUniformLineIsSaidToBe()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        rf::MeshSettings how;
        how.maximum = 40e-6;
        how.cellsAcross = 3;   // (what is said does not want more, and clipped cells are dear)
        rf::Conductor ell;
        ell.layer = QStringLiteral("M1");
        ell.outline = QPolygonF({QPointF(0, 0), QPointF(600, 0), QPointF(600, 50), QPointF(50, 50),
                                 QPointF(50, 300), QPointF(0, 300)});
        rf::Conductor taper;
        taper.layer = QStringLiteral("M1");
        taper.outline = QPolygonF({QPointF(0, 0), QPointF(800, 20), QPointF(800, 80), QPointF(0, 100)});
        // A step: 40 µm wide, then 100 from 300 µm on - as broad overall
        // as its middle is, its middle's cells rectangles, and still no
        // line: only its cross-section a quarter of the way along says so.
        // (A step at the middle itself cuts the middle's cells into shapes
        // that are no rectangles, which says so first.)
        rf::Conductor step;
        step.layer = QStringLiteral("M1");
        step.outline = QPolygonF({QPointF(0, 30), QPointF(300, 30), QPointF(300, 0), QPointF(800, 0),
                                  QPointF(800, 100), QPointF(300, 100), QPointF(300, 70), QPointF(0, 70)});
        for (const rf::Conductor& c : {ell, taper, step}) {
            const rf::Extraction e = rf::extract(rf::meshOf({c}, how, rf::Stack(), 0), {c}, stack);
            QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
            QVERIFY2(!e.perMetre.fromMiddle, qPrintable(e.perMetre.said));
            QVERIFY2(e.perMetre.said.contains(QStringLiteral("Not a uniform line")), qPrintable(e.perMetre.said));
            const rf::LineParameters lp = rf::lineParameters(e, 0, 800e-6);
            QVERIFY2(lp.ok(), qPrintable(lp.problems.join(" | ")));
            QVERIFY2(std::abs(lp.c - e.selfCapacitance(0) / 800e-6) < 1e-12 * lp.c, "the whole over the length");
        }
    }

    void whatItCannotSolveIsRefused()
    {
        // A conductor sent to a layer that is not a metal.
        const QList<rf::Conductor> onOxide{strip(0, 0, 20, 20, QStringLiteral("SUB"))};
        const rf::Extraction wrong = rf::extract(meshed(onOxide, 10), onOxide, grounded(4.1, 10e-6));
        QVERIFY(!wrong.ok());
        QVERIFY2(wrong.problems.first().contains(QStringLiteral("not a metal")), qPrintable(wrong.problems.join(" | ")));

        // A layer the stack has not at all.
        const QList<rf::Conductor> nowhere{strip(0, 0, 20, 20, QStringLiteral("M9"))};
        const rf::Extraction none = rf::extract(meshed(nowhere, 10), nowhere, grounded(4.1, 10e-6));
        QVERIFY(!none.ok());
        QVERIFY2(none.problems.first().contains(QStringLiteral("no layer M9")), qPrintable(none.problems.join(" | ")));

        // A stack that reflects so strongly that its images never die
        // away is solved all the same - what the expansion prunes is put
        // back as one image carrying its charge - and what that cost is
        // said rather than passed over.
        const QList<rf::Conductor> onHard{strip(0, 0, 20, 20)};
        const rf::Extraction mirrored = rf::extract(meshed(onHard, 10), onHard, grounded(1000.0, 10e-6));
        QVERIFY2(mirrored.ok(), qPrintable(mirrored.problems.join(" | ")));
        QVERIFY2(says(mirrored.lines, "coarsened"), qPrintable(mirrored.lines.join(" | ")));

        // More cells than a dense matrix of this build may hold.
        const QList<rf::Conductor> big{strip(0, 0, 400, 400)};
        const rf::Extraction huge = rf::extract(meshed(big, 4, false), big, grounded(4.1, 10e-6));
        QVERIFY(!huge.ok());
        QVERIFY2(huge.problems.first().contains(QStringLiteral("dense matrix")), qPrintable(huge.problems.join(" | ")));
    }
};

QTEST_MAIN(TestRf25Static)
#include "test_rf25_static.moc"
