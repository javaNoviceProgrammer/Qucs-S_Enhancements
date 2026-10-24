/*
 * test_rf25_static.cpp - the quasi-static extraction (rf25/rf25_static.h):
 * the rectangle's closed form against a brute-force integral, a
 * parallel-plate capacitor, a microstrip's Z0 and eps_eff against
 * Hammerstad and Jensen's closed form, and a coupled pair's matrix.
 *
 * Phase B of docs/proposals/2026-10-23-rf-2p5d-moment-method.md, whose
 * test list asks for "a parallel-plate capacitor ... a microstrip's Z0
 * ... a two-line coupling".
 */
#include <QtTest>

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
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

/// Hammerstad and Jensen's microstrip, the textbook closed form: its
/// effective permittivity and characteristic impedance for a zero-thickness
/// strip of width \a w on a substrate \a h thick of \a epsr.
QPair<double, double> hammerstad(double w, double h, double epsr)
{
    const double u = w / h;
    const double eff = (epsr + 1) / 2 + (epsr - 1) / 2 * (1.0 / std::sqrt(1 + 12 / u) + (u < 1 ? 0.04 * std::pow(1 - u, 2) : 0.0));
    const double z0 = u <= 1 ? 60.0 / std::sqrt(eff) * std::log(8 / u + u / 4)
                             : 120 * M_PI / (std::sqrt(eff) * (u + 1.393 + 0.667 * std::log(u + 1.444)));
    return {eff, z0};
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

    // A microstrip: its per-metre capacitance from two lengths of it (the
    // difference cancels what the two ends add), and from that its
    // eps_eff and Z0 against Hammerstad and Jensen's closed form.
    void aMicrostripsImpedanceIsThatOfTheClosedForm()
    {
        const double h = 100e-6, epsr = 9.8;
        const rf::Stack stack = grounded(epsr, h);
        const auto solve = [&](double w, double length, double cell) {
            const QList<rf::Conductor> line{strip(0, 0, w, length)};
            return rf::extract(meshed(line, cell), line, stack);
        };
        rf::Extraction kept;
        // A narrow strip and a wide one: the two branches of the closed
        // form, so that one number agreeing is not taken for a method
        // that works.
        for (const QPair<double, double>& it : {QPair<double, double>{100.0, 25.0}, QPair<double, double>{300.0, 37.5}}) {
            const double w = it.first, cell = it.second;
            const rf::Extraction shortLine = solve(w, 400, cell), longLine = solve(w, 800, cell);
            QVERIFY2(shortLine.ok(), qPrintable(shortLine.problems.join(" | ")));
            QVERIFY2(longLine.ok(), qPrintable(longLine.problems.join(" | ")));
            // Per metre, with what the two ends add removed by the
            // difference between two lengths of the same line.
            const double dl = (800 - 400) * 1e-6;
            const double c = (longLine.selfCapacitance(0) - shortLine.selfCapacitance(0)) / dl;
            double bareShort = 0, bareLong = 0;
            for (double v : shortLine.c0.at(0)) bareShort += v;
            for (double v : longLine.c0.at(0)) bareLong += v;
            const double c0 = (bareLong - bareShort) / dl;
            QVERIFY2(c > 0 && c0 > 0, qPrintable(QStringLiteral("C %1 F/m, C0 %2 F/m").arg(c).arg(c0)));
            const double epsEff = c / c0;
            const double z0 = 1.0 / (rf::kLightSpeed * std::sqrt(c * c0));
            const QPair<double, double> want = hammerstad(w * 1e-6, h, epsr);
            qWarning("microstrip W/h = %.2f: C %.4g pF/m, C0 %.4g pF/m, eps_eff %.4f (Hammerstad %.4f), Z0 %.2f ohm (Hammerstad %.2f)",
                     w * 1e-6 / h, c * 1e12, c0 * 1e12, epsEff, want.first, z0, want.second);
            // Within a few per cent of the closed form, which is itself
            // good to about one: measured at 1.9 % (W/h = 1) and 1.6 %
            // (W/h = 3) on eps_eff, and 1.7 % and 0.3 % on Z0.
            QVERIFY2(std::abs(epsEff - want.first) <= 0.04 * want.first,
                     qPrintable(QStringLiteral("W/h %1: eps_eff %2 vs %3").arg(w * 1e-6 / h).arg(epsEff).arg(want.first)));
            QVERIFY2(std::abs(z0 - want.second) <= 0.04 * want.second,
                     qPrintable(QStringLiteral("W/h %1: Z0 %2 vs %3").arg(w * 1e-6 / h).arg(z0).arg(want.second)));
            // A wider line is a lower impedance and holds more charge.
            if (!kept.ok()) kept = longLine;
            else QVERIFY2(z0 < 49.21, qPrintable(QStringLiteral("the wide strip's Z0 %1").arg(z0)));
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
            // Symmetric, as a capacitance matrix must be.
            QVERIFY2(std::abs(e.c.at(0).at(1) - e.c.at(1).at(0)) <= 1e-6 * std::abs(e.c.at(0).at(1)),
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
