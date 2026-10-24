/*
 * test_rf25_inductance.cpp - the partial inductances (rf25/rf25_inductance.h):
 * a strip's own against the textbook, two filaments' mutual against theirs,
 * a strip cut into pieces adding back up to itself, a rectangular loop
 * against Grover's formula, a two-wire line against its exact value, and
 * the mesh solved for the current that makes a loop.
 *
 * The last line of phase B of docs/proposals/2026-10-23-rf-2p5d-moment-
 * method.md: "partial inductances for L".
 */
#include <QtTest>

#include "rf25/rf25_inductance.h"

#include <cmath>

namespace rf = qucs_s::rf25;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;

/// A run of current along x, from \a x0 to \a x1 at \a y, of \a width.
rf::Filament alongX(double x0, double x1, double y, double width, double thickness = 0, double sigma = 0)
{
    rf::Filament f;
    f.from = QPointF(x0, y);
    f.to = QPointF(x1, y);
    f.width = width;
    f.thickness = thickness;
    f.sigma = sigma;
    return f;
}

/// A run of current along y, from \a y0 to \a y1 at \a x.
rf::Filament alongY(double y0, double y1, double x, double width)
{
    rf::Filament f;
    f.from = QPointF(x, y0);
    f.to = QPointF(x, y1);
    f.width = width;
    return f;
}

/// The textbook self partial inductance of a thin flat strip of length
/// \a l and width \a w: (mu0 l/2pi)(ln(2l/w) + 1/2), which holds for
/// l >> w.
double textbookStrip(double l, double w)
{
    return kMu0 * l / (2 * M_PI) * (std::log(2 * l / w) + 0.5);
}

/// The exact double line integral of 1/r over two parallel filaments of
/// the same length \a l, side by side and \a r apart, times mu0/4pi.
double exactPair(double l, double r)
{
    const auto H = [r](double s) { return s * std::asinh(s / r) - std::sqrt(s * s + r * r); };
    return kMu0 / (4 * M_PI) * (2 * H(l) - 2 * H(0));
}

/// Grover's rectangle: the inductance of a rectangular loop of sides
/// \a a and \a b whose conductor's geometric mean distance is \a rho.
double groversRectangle(double a, double b, double rho)
{
    const double d = std::sqrt(a * a + b * b);
    return kMu0 / M_PI
           * (a * std::log(2 * a * b / (rho * (a + d))) + b * std::log(2 * a * b / (rho * (b + d))) + 2 * (d - a - b));
}

/// A stack of a substrate of \a epsr and \a height on a ground plane,
/// with one metal on top of \a thickness and \a sigma.
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

rf::Conductor strip(double x, double y, double w, double h)
{
    rf::Conductor c;
    c.layer = QStringLiteral("M1");
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

rf::Mesh meshed(const QList<rf::Conductor>& conductors, double cell)
{
    rf::MeshSettings how;
    how.maximum = cell * 1e-6;
    how.edges = false;   // (an even grid, so the cells share whole edges)
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

} // namespace

class TestRf25Inductance : public QObject
{
    Q_OBJECT

private slots:

    // A strip's own partial inductance is the textbook's, which is what
    // the geometric mean distance of w e^-3/2 is chosen to make it.
    void aStripsOwnInductanceIsTheTextbooks()
    {
        // The geometric mean distance itself.
        QVERIFY2(std::abs(rf::selfDistance(1.0) - std::exp(-1.5)) < 1e-15, "the GMD of a unit strip is e^-3/2");
        QCOMPARE(rf::selfDistance(0), 0.0);
        // A width that is no width must not come back as a distance of
        // less than nothing, which would be an inductance of less too.
        QCOMPARE(rf::selfDistance(-10e-6), 0.0);

        const double w = 10e-6;
        const double rho = rf::selfDistance(w);
        for (const double l : {1e-3, 10e-3, 100e-3}) {
            const rf::Filament f = alongX(0, l, 0, w);
            const double got = rf::partialInductance(f, f);
            // Exactly the double integral at the geometric mean distance.
            QVERIFY2(std::abs(got - exactPair(l, rho)) < 1e-12 * got,
                     qPrintable(QStringLiteral("l %1: %2 vs %3").arg(l).arg(got).arg(exactPair(l, rho))));
            // And the textbook's (mu0 l/2pi)(ln(2l/w) + 1/2), which is
            // the *asymptotic* form: expanding asinh and the root leaves
            // exactly (mu0/2pi)(rho - rho^2/4l) over it, and nothing
            // else, so that offset is asserted rather than tolerated.
            const double wanted = textbookStrip(l, w);
            const double offset = kMu0 / (2 * M_PI) * (rho - rho * rho / (4 * l));
            QVERIFY2(std::abs(got - wanted - offset) < 1e-9 * got,
                     qPrintable(QStringLiteral("l %1: %2 nH vs %3 nH + %4 nH")
                                    .arg(l).arg(got * 1e9).arg(wanted * 1e9).arg(offset * 1e9)));
            // The offset is small, which is why the textbook may drop it.
            QVERIFY2(std::abs(offset) < 1e-3 * got, "the asymptote is not close");
        }
        // It grows faster than its length, the logarithm seeing to that.
        const double shortOne = rf::partialInductance(alongX(0, 1e-3, 0, w), alongX(0, 1e-3, 0, w));
        const double longOne = rf::partialInductance(alongX(0, 2e-3, 0, w), alongX(0, 2e-3, 0, w));
        QVERIFY2(longOne > 2 * shortOne, "twice the strip is more than twice the inductance");
        // A wider strip holds less: the current is spread further out.
        QVERIFY2(rf::partialInductance(alongX(0, 1e-3, 0, 40e-6), alongX(0, 1e-3, 0, 40e-6)) < shortOne,
                 "a wider strip has the lower inductance");
        // A run of no length or no width has none.
        QCOMPARE(rf::partialInductance(alongX(0, 0, 0, w), alongX(0, 0, 0, w)), 0.0);
        const rf::Filament none = alongX(0, 1e-3, 0, 0);
        QCOMPARE(rf::partialInductance(none, none), 0.0);
    }

    // Two filaments side by side: their mutual is the same double
    // integral with their distance in it, and it is exact where they are
    // far enough apart for their widths not to matter.
    void twoFilamentsMutualIsTheirDoubleIntegral()
    {
        const double l = 1e-3, w = 1e-6;
        for (const double d : {10e-6, 100e-6, 1e-3}) {
            const rf::Filament a = alongX(0, l, 0, w);
            const rf::Filament b = alongX(0, l, d, w);
            const double got = rf::partialInductance(a, b);
            // Far beyond kNearWidths of their width, so the distance
            // between their axes is the whole of it: exactly exactPair.
            const double wanted = exactPair(l, d);
            QVERIFY2(std::abs(got - wanted) < 1e-12 * wanted,
                     qPrintable(QStringLiteral("d %1: %2 vs %3").arg(d).arg(got).arg(wanted)));
            // Reciprocal.
            QVERIFY2(std::abs(rf::partialInductance(b, a) - got) < 1e-15 * got, "the mutual is not reciprocal");
        }
        // It falls as they are moved apart, and never below nothing while
        // they point the same way.
        double last = 1e300;
        for (const double d : {10e-6, 50e-6, 250e-6, 1.25e-3}) {
            const double m = rf::partialInductance(alongX(0, l, 0, w), alongX(0, l, d, w));
            QVERIFY2(m > 0 && m < last, qPrintable(QStringLiteral("d %1: %2").arg(d).arg(m)));
            last = m;
        }
        // Turned about, it is the same size and the other sign.
        const rf::Filament there = alongX(0, l, 0, w);
        const rf::Filament back = alongX(l, 0, 100e-6, w);
        const double forward = rf::partialInductance(there, alongX(0, l, 100e-6, w));
        QVERIFY2(std::abs(rf::partialInductance(there, back) + forward) < 1e-12 * forward,
                 "a current going the other way does not give the mutual its sign");
        // Across one another: no flux at all.
        QCOMPARE(rf::partialInductance(there, alongY(0, l, 0, w)), 0.0);
        QCOMPARE(rf::partialInductance(there, alongY(-l, l, 500e-6, w)), 0.0);
        // Its own mutual is larger than any other's, the distance being
        // the smallest it can be.
        QVERIFY(rf::partialInductance(there, there) > forward);
    }

    // Two strips near enough that their breadth matters: the distance is
    // averaged across both widths, and that average is held to a fine
    // numerical one. Taken between their middles instead, it is wrong by
    // per cents - which is why the near pairs are integrated at all.
    void aNearPairIsAveragedAcrossItsWidths()
    {
        const double l = 200e-6, w = 20e-6;
        // A quarter of a width apart edge to edge: well inside the six
        // widths at which the averaging is given up.
        for (const double d : {25e-6, 40e-6, 80e-6}) {
            const rf::Filament a = alongX(0, l, 0, w);
            const rf::Filament b = alongX(0, l, d, w);
            const double got = rf::partialInductance(a, b);
            // The reference: the same double integral averaged over both
            // widths by a midpoint rule fine enough to settle.
            const int n = 240;
            double sum = 0;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const double pa = w * ((i + 0.5) / n - 0.5);
                    const double qb = w * ((j + 0.5) / n - 0.5);
                    sum += exactPair(l, std::abs(d + qb - pa));
                }
            const double wanted = sum / (double(n) * n);
            QVERIFY2(std::abs(got - wanted) < 2e-4 * wanted,
                     qPrintable(QStringLiteral("d %1 µm: %2 pH vs %3 pH").arg(d * 1e6).arg(got * 1e12).arg(wanted * 1e12)));
            // And the middles alone would not do: at a width and a
            // quarter apart the difference is per cents, not parts in
            // ten thousand.
            const double middles = exactPair(l, d);
            if (d < 2 * w)
                QVERIFY2(std::abs(middles - wanted) > 0.01 * wanted,
                         qPrintable(QStringLiteral("d %1 µm: middles %2 pH vs averaged %3 pH")
                                        .arg(d * 1e6).arg(middles * 1e12).arg(wanted * 1e12)));
        }
        // Far apart, the averaging is given up and the middles are right.
        const double far = 10 * w;
        QVERIFY2(std::abs(rf::partialInductance(alongX(0, l, 0, w), alongX(0, l, far, w)) - exactPair(l, far))
                     < 1e-12 * exactPair(l, far),
                 "a far pair is not taken between its middles");
    }

    // A strip cut into pieces adds back up to the strip: the sum over
    // every pair of pieces is the whole one's own inductance. It is the
    // check that the kernel is consistent with itself, and the reason a
    // mesh of a conductor can stand for the conductor.
    void aStripCutIntoPiecesAddsBackUpToItself()
    {
        const double l = 1e-3, w = 10e-6;
        const double whole = rf::partialInductance(alongX(0, l, 0, w), alongX(0, l, 0, w));
        for (const int pieces : {2, 4, 16}) {
            QList<rf::Filament> cut;
            for (int i = 0; i < pieces; ++i)
                cut << alongX(l * i / pieces, l * (i + 1) / pieces, 0, w);
            double sum = 0;
            for (const rf::Filament& a : cut)
                for (const rf::Filament& b : cut) sum += rf::partialInductance(a, b);
            QVERIFY2(std::abs(sum - whole) < 1e-9 * whole,
                     qPrintable(QStringLiteral("%1 pieces: %2 nH vs %3 nH").arg(pieces).arg(sum * 1e9).arg(whole * 1e9)));
            // And the matrix of them says the same, symmetrically.
            const QList<QList<double>> m = rf::partialInductances(cut);
            QCOMPARE(m.size(), pieces);
            double again = 0;
            for (int i = 0; i < pieces; ++i) {
                QCOMPARE(m.at(i).size(), pieces);
                for (int j = 0; j < pieces; ++j) {
                    QVERIFY2(m.at(i).at(j) == m.at(j).at(i), "the partial inductance matrix is not symmetric");
                    again += m.at(i).at(j);
                }
                QVERIFY2(m.at(i).at(i) > 0, "a run's own inductance is not positive");
            }
            QVERIFY2(std::abs(again - whole) < 1e-9 * whole, "the matrix does not add up to the whole strip");
        }
    }

    // A two-wire line: out along one strip and back along the other. Its
    // inductance is exactly the difference of the two double integrals,
    // and per metre it tends to (mu0/pi) ln(d/GMD).
    void aTwoWireLineIsTheDifferenceOfItsIntegrals()
    {
        const double l = 1e-3, w = 1e-6, d = 100e-6;
        const QList<rf::Filament> loop{alongX(0, l, 0, w), alongX(l, 0, d, w)};
        const double got = rf::loopInductance(loop);
        // Exact: twice each one's own, less twice their mutual.
        const double rho = rf::selfDistance(w);
        const double wanted = 2 * exactPair(l, rho) - 2 * exactPair(l, d);
        QVERIFY2(std::abs(got - wanted) < 1e-12 * wanted,
                 qPrintable(QStringLiteral("%1 nH vs %2 nH").arg(got * 1e9).arg(wanted * 1e9)));
        QVERIFY2(got > 0, "a loop's inductance is positive");

        // Long beside its own width, it is (mu0/pi) ln(d/GMD) a metre.
        const double longer = 1.0;
        const QList<rf::Filament> line{alongX(0, longer, 0, w), alongX(longer, 0, d, w)};
        const double perMetre = rf::loopInductance(line) / longer;
        const double asymptotic = kMu0 / M_PI * std::log(d / rho);
        QVERIFY2(std::abs(perMetre - asymptotic) < 1e-3 * asymptotic,
                 qPrintable(QStringLiteral("%1 nH/m vs %2 nH/m").arg(perMetre * 1e9).arg(asymptotic * 1e9)));
        // Wider apart is more inductance; a wider conductor is less.
        QVERIFY(rf::loopInductance({alongX(0, l, 0, w), alongX(l, 0, 4 * d, w)}) > got);
        QVERIFY(rf::loopInductance({alongX(0, l, 0, 8 * w), alongX(l, 0, d, 8 * w)}) < got);
    }

    // A rectangular loop against Grover's own formula for one, which is
    // an anchor from outside this file.
    void aRectangularLoopIsGroversFormula()
    {
        const double w = 10e-6;
        const double rho = rf::selfDistance(w);
        for (const QPair<double, double> sides : {qMakePair(1e-3, 1e-3), qMakePair(2e-3, 1e-3), qMakePair(5e-3, 1e-3)}) {
            const double a = sides.first, b = sides.second;
            // Round it: along +x, up +y, back -x, down -y.
            const QList<rf::Filament> loop{alongX(0, a, 0, w), alongY(0, b, a, w), alongX(a, 0, b, w), alongY(b, 0, 0, w)};
            const double got = rf::loopInductance(loop);
            const double wanted = groversRectangle(a, b, rho);
            QVERIFY2(std::abs(got - wanted) < 0.02 * wanted,
                     qPrintable(QStringLiteral("%1 x %2 mm: %3 nH vs Grover's %4 nH")
                                    .arg(a * 1e3).arg(b * 1e3).arg(got * 1e9).arg(wanted * 1e9)));
            QVERIFY2(got > 0, "a rectangular loop's inductance is positive");
        }
        // A 1 mm square of 10 µm trace is some nanohenries - about a
        // nanohenry a millimetre of perimeter, as the rule of thumb says.
        const QList<rf::Filament> square{alongX(0, 1e-3, 0, w), alongY(0, 1e-3, 1e-3, w), alongX(1e-3, 0, 1e-3, w),
                                         alongY(1e-3, 0, 0, w)};
        const double l = rf::loopInductance(square);
        QVERIFY2(l > 2e-9 && l < 8e-9, qPrintable(QStringLiteral("%1 nH").arg(l * 1e9)));
        // A bigger loop holds more.
        const QList<rf::Filament> bigger{alongX(0, 2e-3, 0, w), alongY(0, 2e-3, 2e-3, w), alongX(2e-3, 0, 2e-3, w),
                                         alongY(2e-3, 0, 0, w)};
        QVERIFY(rf::loopInductance(bigger) > l);
        // Gone round the other way it is the same loop.
        const QList<rf::Filament> widdershins{alongY(0, 1e-3, 0, w), alongX(0, 1e-3, 1e-3, w), alongY(1e-3, 0, 1e-3, w),
                                              alongX(1e-3, 0, 0, w)};
        QVERIFY2(std::abs(rf::loopInductance(widdershins) - l) < 1e-12 * l, "a loop's inductance depends on its direction");
    }

    // The mesh's runs of current: the pairs of cells that share an edge.
    void theRunsOfCurrentAreTheCellsThatShareAnEdge()
    {
        const QList<rf::Conductor> line{strip(0, 0, 400, 40)};
        const rf::Mesh mesh = meshed(line, 40);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        QVERIFY(mesh.cells.size() >= 10);
        const rf::Stack stack = grounded(9.8, 100e-6, 3e-6, 5.8e7);
        QList<QPair<int, int>> ends;
        const QList<rf::Filament> runs = rf::runsOf(mesh, line, stack, 1e-6, &ends);
        QVERIFY2(!runs.isEmpty(), "no run of current was found in a meshed strip");
        QCOMPARE(ends.size(), runs.size());
        // A chain of n cells has n-1 runs between them; a grid has more.
        QVERIFY2(runs.size() >= mesh.cells.size() - 1, qPrintable(QStringLiteral("%1 runs of %2 cells").arg(runs.size()).arg(mesh.cells.size())));
        for (const rf::Filament& f : runs) {
            QVERIFY2(f.length() > 0, "a run of no length");
            QVERIFY2(f.width > 0, "a run of no width");
            QCOMPARE(f.z0, f.z1);
            QCOMPARE(f.thickness, 3e-6);
            QCOMPARE(f.sigma, 5.8e7);
            QVERIFY2(f.resistance() > 0, "a copper run with no resistance");
        }
        // Its height is where the metal sits: on top of 100 µm.
        QCOMPARE(runs.first().z0, 100e-6);
        // A metal that says no conductivity has no resistance.
        for (const rf::Filament& f : rf::runsOf(mesh, line, grounded(9.8, 100e-6), 1e-6)) QCOMPARE(f.resistance(), 0.0);
    }

    // The mesh solved: a straight bar's inductance is its own partial
    // inductance, since a current going along it has no choice, and its
    // resistance is the bar's.
    void aStraightBarsSolvedInductanceIsItsOwn()
    {
        const double length = 400.0, width = 40.0;   // µm
        const QList<rf::Conductor> bar{strip(0, 0, length, width)};
        const rf::Mesh mesh = meshed(bar, 40);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const double sigma = 5.8e7, thick = 3e-6;
        const rf::Stack stack = grounded(9.8, 100e-6, thick, sigma);
        const rf::Inductance got = rf::inductanceOf(mesh, bar, stack, QPointF(0, width / 2), QPointF(length, width / 2),
                                                    1e-6, 10e9);
        QVERIFY2(got.ok(), qPrintable(got.problems.join(" | ")));
        QVERIFY(got.branches > 0 && got.nodes > 0);
        QCOMPARE(got.current.size(), got.branches);

        // The bar's own partial inductance, which is what a current with
        // one way to go must come to.
        const double wanted = textbookStrip(length * 1e-6, width * 1e-6);
        QVERIFY2(std::abs(got.l - wanted) < 0.02 * wanted,
                 qPrintable(QStringLiteral("%1 nH solved vs %2 nH of the bar").arg(got.l * 1e9).arg(wanted * 1e9)));
        // Its resistance at direct current is *exactly* the bar's,
        // rho l/(w t): the runs between the cell centres and the two
        // stubs at the ends tile the bar's length with nothing over.
        // Without the stubs a bar of ten cells came out a tenth short.
        const double bars = length * 1e-6 / (sigma * width * 1e-6 * thick);
        QVERIFY2(std::abs(got.rDc - bars) < 1e-9 * bars,
                 qPrintable(QStringLiteral("%1 Ohm solved vs %2 Ohm of the bar").arg(got.rDc).arg(bars)));
        // A finer mesh does not move either, the tiling being exact.
        const rf::Inductance finer = rf::inductanceOf(meshed(bar, 20), bar, stack, QPointF(0, width / 2),
                                                      QPointF(length, width / 2));
        QVERIFY2(finer.ok(), qPrintable(finer.problems.join(" | ")));
        QVERIFY2(std::abs(finer.rDc - bars) < 1e-9 * bars,
                 qPrintable(QStringLiteral("finer: %1 Ohm vs %2 Ohm").arg(finer.rDc).arg(bars)));
        // The answer does not move with the mesh, which is the whole
        // point of driving the face rather than a point on it: fed at one
        // cell, a bar two cells wide came out a sixth too resistive.
        QVERIFY2(std::abs(finer.rDc - got.rDc) < 1e-9 * got.rDc,
                 qPrintable(QStringLiteral("%1 vs %2 Ohm").arg(finer.rDc, 0, 'g', 12).arg(got.rDc, 0, 'g', 12)));
        QVERIFY2(std::abs(finer.l - got.l) < 0.01 * got.l,
                 qPrintable(QStringLiteral("finer: %1 nH vs %2 nH").arg(finer.l * 1e9).arg(got.l * 1e9)));
        QVERIFY2(finer.branches > got.branches, "the finer mesh has more runs of current");
        // Q at the frequency asked for, and nothing where none is.
        QVERIFY2(got.q > 0, "no Q at 10 GHz");
        QVERIFY2(std::abs(got.q - 2 * M_PI * 10e9 * got.l / got.rDc) < 1e-9 * got.q, "Q is not wL/R");
        QCOMPARE(got.frequency, 10e9);
        const rf::Inductance quiet = rf::inductanceOf(mesh, bar, stack, QPointF(0, width / 2), QPointF(length, width / 2));
        QVERIFY(quiet.ok());
        QCOMPARE(quiet.q, 0.0);
        QCOMPARE(quiet.frequency, 0.0);
        QVERIFY2(std::abs(quiet.l - got.l) < 1e-12 * got.l, "the inductance moved with a frequency it does not use");

        // Every amp that goes in comes out: the currents across any cut
        // of the bar add to one.
        double most = 0;
        for (const double i : quiet.current) most = std::max(most, std::abs(i));
        QVERIFY2(std::abs(most - 1.0) < 0.05, qPrintable(QStringLiteral("the largest run carries %1 A").arg(most)));
        // And it says what it did - including where the current went in
        // and came out, which must be the two ends it was given and not
        // whichever faces happened to be found.
        QVERIFY2(got.lines.size() >= 3, qPrintable(got.lines.join(" | ")));
        QVERIFY2(got.lines.join(QLatin1Char(' ')).contains(QStringLiteral("nH")), qPrintable(got.lines.join(" | ")));
        const QString where = got.lines.join(QLatin1Char(' '));
        QVERIFY2(where.contains(QStringLiteral("In at the cell at 20,")), qPrintable(where));
        QVERIFY2(where.contains(QStringLiteral("out at 380,")), qPrintable(where));
    }

    // A wide plate taken corner to corner: the current spreads, so it
    // loses less than the same current squeezed down a narrow strip.
    void aWidePlateLosesLessThanANarrowStrip()
    {
        const double sigma = 5.8e7, thick = 3e-6;
        const rf::Stack stack = grounded(9.8, 100e-6, thick, sigma);
        const QList<rf::Conductor> wide{strip(0, 0, 400, 200)};
        const QList<rf::Conductor> narrow{strip(0, 0, 400, 40)};
        const rf::Inductance w = rf::inductanceOf(meshed(wide, 40), wide, stack, QPointF(0, 100), QPointF(400, 100));
        const rf::Inductance n = rf::inductanceOf(meshed(narrow, 40), narrow, stack, QPointF(0, 20), QPointF(400, 20));
        QVERIFY2(w.ok(), qPrintable(w.problems.join(" | ")));
        QVERIFY2(n.ok(), qPrintable(n.problems.join(" | ")));
        QVERIFY2(w.rDc < n.rDc, qPrintable(QStringLiteral("wide %1 Ohm vs narrow %2 Ohm").arg(w.rDc).arg(n.rDc)));
        QVERIFY2(w.l < n.l, qPrintable(QStringLiteral("wide %1 nH vs narrow %2 nH").arg(w.l * 1e9).arg(n.l * 1e9)));
        // The wide one has more than one way through, so more runs.
        QVERIFY(w.branches > n.branches);
    }

    // In across one face and out across another that meets it at a
    // corner: a current that turns a right angle. It crowds hard at the
    // inside of the corner, which is what minimising the magnetic energy
    // does and what minimising the current's bare square would not.
    void aCurrentThatTurnsACornerCrowdsAtIt()
    {
        const rf::Stack stack = grounded(9.8, 100e-6, 3e-6, 5.8e7);
        const QList<rf::Conductor> plate{strip(0, 0, 400, 200)};
        // In across the left face (x = 0), out across the bottom one
        // (y = 0). The two faces meet at the cell in the corner, which is
        // in both of them - and that is a loop, not a refusal.
        const rf::Inductance turn = rf::inductanceOf(meshed(plate, 40), plate, stack, QPointF(0, 100), QPointF(200, 0));
        QVERIFY2(turn.ok(), qPrintable(turn.problems.join(" | ")));
        QCOMPARE(turn.current.size(), turn.branches);
        double most = 0, least = 1e300;
        for (const double i : turn.current) {
            most = std::max(most, std::abs(i));
            if (std::abs(i) > 0) least = std::min(least, std::abs(i));
        }
        // Nearly the whole amp takes the short way round, and somewhere
        // in the plate a run carries next to none of it.
        QVERIFY2(most > 0.5, qPrintable(QStringLiteral("the busiest run carries %1 A").arg(most)));
        QVERIFY2(most / least > 100, qPrintable(QStringLiteral("the current spreads only %1 to 1").arg(most / least)));
        // Turning the corner is the short way, so it loses far less than
        // going the length of the plate.
        const rf::Inductance across = rf::inductanceOf(meshed(plate, 40), plate, stack, QPointF(0, 100), QPointF(400, 100));
        QVERIFY2(across.ok(), qPrintable(across.problems.join(" | ")));
        QVERIFY2(turn.rDc < across.rDc, qPrintable(QStringLiteral("%1 vs %2 Ohm").arg(turn.rDc).arg(across.rDc)));
        QVERIFY2(turn.l < across.l, qPrintable(QStringLiteral("%1 vs %2 nH").arg(turn.l * 1e9).arg(across.l * 1e9)));
    }

    // What it will not find an inductance of.
    void whatHasNoLoopIsRefused()
    {
        const rf::Stack stack = grounded(9.8, 100e-6, 3e-6, 5.8e7);
        const QList<rf::Conductor> bar{strip(0, 0, 400, 40)};
        const rf::Mesh mesh = meshed(bar, 40);
        // No cells at all.
        QVERIFY(!rf::inductanceOf(rf::Mesh(), bar, stack, QPointF(0, 0), QPointF(400, 20)).ok());
        QVERIFY2(!rf::inductanceOf(rf::Mesh(), bar, stack, QPointF(0, 0), QPointF(400, 20)).problems.isEmpty(),
                 "it said nothing of having no cells");
        // Both ends on the same face.
        const rf::Inductance same = rf::inductanceOf(mesh, bar, stack, QPointF(0, 10), QPointF(0, 30));
        QVERIFY(!same.ok());
        QVERIFY2(same.problems.join(QLatin1Char(' ')).contains(QStringLiteral("same face")), qPrintable(same.problems.join(" | ")));
        // A layout in two pieces that nothing joins: a current cannot get
        // from one to the other, and it says *that* rather than whatever
        // the next guard would have said.
        const QList<rf::Conductor> apart{strip(0, 0, 200, 40), strip(1000, 0, 200, 40)};
        const rf::Inductance split = rf::inductanceOf(meshed(apart, 40), apart, stack, QPointF(0, 20), QPointF(1200, 20));
        QVERIFY2(!split.ok(), qPrintable(QStringLiteral("it found %1 nH across a gap").arg(split.l * 1e9)));
        QVERIFY2(split.problems.join(QLatin1Char(' ')).contains(QStringLiteral("in pieces")),
                 qPrintable(split.problems.join(" | ")));
        // A single cell has no run at all, and says so in its own words.
        const QList<rf::Conductor> tiny{strip(0, 0, 10, 10)};
        const rf::Inductance one = rf::inductanceOf(meshed(tiny, 40), tiny, stack, QPointF(0, 0), QPointF(10, 10));
        QVERIFY(!one.ok());
        QVERIFY2(one.problems.join(QLatin1Char(' ')).contains(QStringLiteral("nowhere for a current to flow")),
                 qPrintable(one.problems.join(" | ")));
        // And a mesh past what a dense matrix of this build holds is
        // refused before one is set up, with the count in the message.
        const QList<rf::Conductor> big{strip(0, 0, 400, 120)};
        const rf::Inductance over = rf::inductanceOf(meshed(big, 5), big, stack, QPointF(0, 60), QPointF(400, 60));
        QVERIFY2(!over.ok(), qPrintable(QStringLiteral("it set up %1 runs").arg(over.branches)));
        QVERIFY2(over.problems.join(QLatin1Char(' ')).contains(QStringLiteral("more than this build")),
                 qPrintable(over.problems.join(" | ")));
        QVERIFY2(over.branches > 3000, qPrintable(QStringLiteral("%1 runs").arg(over.branches)));
    }
};

QTEST_MAIN(TestRf25Inductance)
#include "test_rf25_inductance.moc"
