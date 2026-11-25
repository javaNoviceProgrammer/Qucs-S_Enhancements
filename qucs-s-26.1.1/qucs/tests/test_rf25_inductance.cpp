/*
 * test_rf25_inductance.cpp - the partial inductances (rf25/rf25_inductance.h):
 * a strip's own against the textbook, two filaments' mutual against theirs,
 * a strip cut into pieces adding back up to itself, a rectangular loop
 * against Grover's formula, a two-wire line against its exact value, and
 * the mesh solved for the current that makes a loop.
 *
 * Partial inductances for L.
 */
#include <QtTest>

#include <algorithm>
#include <array>

#include "rf25/rf25_inductance.h"
#include "rf25/rf25_layout.h"
#include "multiphysics/fem_sparse.h"
#include <Eigen/Dense>

#include <cmath>
#include <chrono>
#include <thread>
#include <QElapsedTimer>
#include <tuple>

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
    how.cellsAcross = 0; // (and of that size alone, as the tests were written)
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

/// A planar spiral of S. S. Mohan et al., "Simple accurate expressions for
/// planar spiral inductances" (IEEE JSSC 34(10), 1999), its Table IV:
/// \a sides 4, 8 or 12, \a n turns, \a dout
/// across the flats, track \a w and gap \a s (µm). Its centre line - a
/// square's segments 2 a0, 2 a0, 2 a0 and then pairs shorter by the pitch;
/// a polygon's turns each a regular polygon a pitch inside the one before,
/// stepping in over its last \a m sides, as Mohan's Fig. 1 draws them, each
/// corner where two sides cross, so that the gap is s everywhere - with a
/// lead of w at its outer end out along -x (a polygon's, on along side 0)
/// and, for a polygon, w/2 on along the next side at its inner end. (The
/// review's generator stepped every side in by a pitch over the sides, its
/// turns half a pitch smaller than Mohan's and L 9-12 % under his.)
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

/// \a pts widened to \a w, mitred at each bend, on a nanometre's grid as
/// a GDS has it: one conductor of M1.
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

/// The length of \a pts, µm.
double lengthOf(const QList<QPointF>& pts)
{
    double sum = 0;
    for (int i = 1; i < pts.size(); ++i) sum += std::hypot(pts.at(i).x() - pts.at(i - 1).x(), pts.at(i).y() - pts.at(i - 1).y());
    return sum;
}

/// As the tab meshes a model whose Mesh node asks for one cell across and
/// \a largest µm at most.
rf::MeshSettings likeTheTab(double largest, int across = 1)
{
    rf::MeshSettings how;
    how.maximum = largest * 1e-6;
    how.cellsAcross = across;
    how.gaps = true;
    return how;
}

/// Whether this build is a sanitized one (QUCS_TEST_SLOW: its path in any):
/// the loops' dense solves some 130 times a Release build's there (the
/// suite 23 s in Release; under ASan on an M2 Ultra the compressed loop's
/// function 278 s and the strip at 45 degrees past QtTest's 900, where CI's
/// Linux job has 900 for the whole suite).
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
    // A wire as thick as it is wide is a bar:
    // its runs' geometric mean distance a rectangle's, w by t -
    // 0.2235 (w + t) at any aspect, the strip's w e^-3/2 as t goes to nought
    // -, and near ones over both cross-sections. A square loop of copper 0.5
    // by 0.5 mm, 10 mm along its middle with a 0.2 mm gap, in free space, is
    // 24.17 nH by a Neumann sum over 12 by 12 filaments of each side (24.18 at
    // 4 by 4); taken as a strip of no thickness, the loop read 29.4, 22 %
    // high. Its resistance at direct current is the 3D magnetostatic solve's
    // 2.594 mOhm.
    void aWireAsThickAsItIsWideIsABar()
    {
        QVERIFY(std::abs(rf::selfDistance(1, 1) / (0.2235 * 2) - 1) < 5e-3);
        QVERIFY(std::abs(rf::selfDistance(1, 0.1) / (0.2235 * 1.1) - 1) < 5e-3);
        QVERIFY(std::abs(rf::selfDistance(1, 1e-6) - std::exp(-1.5)) < 1e-5);
        QCOMPARE(rf::selfDistance(1, 0), std::exp(-1.5));
        QVERIFY(std::abs(rf::selfDistance(1, 3) - rf::selfDistance(3, 1)) < 1e-12);
        // (Two bars near each other, thin against the distance between
        //  them - 1 by 0.4 um, 2 um apart -: through their thicknesses at
        //  one point, their heights' spread added in quadrature, within 3e-4
        //  of the mean over both cross-sections - the review of 54357932.)
        {
            const double l = 1e-3, w = 1e-6, t = 0.4e-6, apart = 2e-6;
            rf::Filament a = alongX(0, l, 0, w, t), b = alongX(0, l, apart, w, t);
            a.bar = b.bar = t;
            const int n = 12;
            double mean = 0;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                    for (int k = 0; k < n; ++k)
                        for (int m = 0; m < n; ++m) {
                            const double dy = apart + ((j + 0.5) / n - (i + 0.5) / n) * w, dz = ((m + 0.5) / n - (k + 0.5) / n) * t;
                            mean += exactPair(l, std::hypot(dy, dz));
                        }
            mean /= double(n) * n * n * n;
            const double got = rf::partialInductance(a, b);
            QVERIFY2(std::abs(got / mean - 1) < 3e-4, qPrintable(QStringLiteral("%1 against %2").arg(got).arg(mean)));
        }

        rf::Stack stack = grounded(1, 5e-3, 0.5e-3, 6e7);
        stack.layers[0].variant = QStringLiteral("open");
        rf::Conductor loop;
        loop.layer = QStringLiteral("M1");
        loop.outline = QPolygonF(QList<QPointF>{{-5250, -100}, {-5250, -5250}, {5250, -5250}, {5250, 5250}, {-5250, 5250}, {-5250, 100},
                                                {-4750, 100}, {-4750, 4750}, {4750, 4750}, {4750, -4750}, {-4750, -4750}, {-4750, -100}});
        rf::MeshSettings how;
        // (Ten across, the review's mesh, 2,928 cells: past the dense limit,
        //  solved compressed, 9 s here; a slow build four, solved dense - 24.13.)
        how.cellsAcross = slowBuild() ? 4 : 10;
        const rf::Mesh mesh = rf::meshOf({loop}, how, stack, 1e8);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const rf::Inductance l = rf::inductanceOf(mesh, {loop}, stack, QPointF(-5000, -100), QPointF(-5000, 100), 1e-6, 1e4);
        QVERIFY2(l.ok(), qPrintable(l.problems.join(" | ")));
        qWarning("a square loop of 0.5 by 0.5 mm: %d cells, L_dc %.3f nH (24.17), R_dc %.4f mOhm (2.594)", int(mesh.cells.size()), l.lDc * 1e9,
                 l.rDc * 1e3);
        QVERIFY2(std::abs(l.lDc / 24.17e-9 - 1) < 0.01, qPrintable(QStringLiteral("%1 nH").arg(l.lDc * 1e9)));
        QVERIFY2(std::abs(l.rDc / 2.594e-3 - 1) < 0.01, qPrintable(QStringLiteral("%1 mOhm").arg(l.rDc * 1e3)));
        // (Its runs bars; the same loop's of a metal a tenth as thick as it
        //  is wide strips, as before - within a per cent or so of bars, and
        //  a spiral's finest cells, a tenth of its thickness wide, bars
        //  beside bars that GMRES took 2,000 iterations over where it had
        //  taken 22 - the review of 54357932.)
        const auto bars = [](const rf::Inductance& of) {
            int n = 0;
            for (const rf::Filament& f : of.runs) n += f.bar > 0 ? 1 : 0;
            return n;
        };
        QVERIFY2(bars(l) > 0, "the thick loop's runs are not bars");
        for (rf::Layer& layer : stack.layers)
            if (layer.kind == rf::Layer::Metal) layer.thickness = 0.05e-3;
        rf::MeshSettings coarse;
        coarse.cellsAcross = 2;
        const rf::Mesh thinMesh = rf::meshOf({loop}, coarse, stack, 1e8);
        const rf::Inductance thin = rf::inductanceOf(thinMesh, {loop}, stack, QPointF(-5000, -100), QPointF(-5000, 100), 1e-6, 1e4);
        QVERIFY2(thin.ok(), qPrintable(thin.problems.join(" | ")));
        QVERIFY2(!thin.runs.isEmpty() && bars(thin) == 0, qPrintable(QStringLiteral("%1 of %2 runs bars").arg(bars(thin)).arg(thin.runs.size())));
    }

    // Two bars, each its own or near the other, are their double volume
    // integral of 1/r exactly - Hoer and Love's -, against that integral
    // taken numerically (the length in closed form, the cross-sections'
    // differences adaptively): a bar shorter than it is thick, its own and
    // its neighbour's, which by a geometric mean distance and four points
    // across were a quarter under and 9 % over, the neighbour over its own;
    // a long one 0.05 µm wide of 3 µm metal, its neighbour 11 % over; and
    // one over another, apart along, across and up. Then a two-turn coil's
    // L, 3 µm metal on 5 µm traces 2 µm apart, is positive definite: 103 of
    // its 1,534 eigenvalues were negative, and its compressed solve's GMRES
    // stood still on a finer mesh (the review's P71).
    void twoBarsAreTheirDoubleVolumeIntegral()
    {
        const double um = 1e-6, unit = kMu0 / (4 * M_PI) * um;   // (the integrals below are in µm)
        const auto bar = [um](double x0, double x1, double y, double w, double z, double t) {
            rf::Filament f = alongX(x0 * um, x1 * um, y * um, w * um, t * um);
            f.z0 = f.z1 = z * um;
            f.bar = t * um;
            return f;
        };
        const auto near = [](double got, double want, double within) {
            return std::abs(got / want - 1) < within;
        };
        {
            const rf::Filament a = bar(0, 0.42, 0, 0.41, 0, 3), b = bar(0, 0.42, 0.41, 0.41, 0, 3);
            const double own = rf::partialInductance(a, a), mutual = rf::partialInductance(a, b);
            QVERIFY2(near(own, 0.299510753759 * unit, 1e-9), qPrintable(QString::number(own / unit, 'g', 12)));
            QVERIFY2(near(mutual, 0.213536623105 * unit, 1e-9), qPrintable(QString::number(mutual / unit, 'g', 12)));
        }
        {
            const rf::Filament a = bar(0, 25, 0, 0.05, 0, 3), b = bar(0, 25, 0.05, 0.05, 0, 3);
            const double own = rf::partialInductance(a, a), mutual = rf::partialInductance(a, b);
            QVERIFY2(near(own, 166.783539485 * unit, 1e-8), qPrintable(QString::number(own / unit, 'g', 12)));
            QVERIFY2(near(mutual, 165.117729454 * unit, 1e-8), qPrintable(QString::number(mutual / unit, 'g', 12)));
        }
        {
            // (2 by 1 µm, 5 long; 1 by 2, 3 long, 1 µm along and 1.5 up from
            //  it - and drawn the other way, the same less.)
            const rf::Filament a = bar(0, 5, 0, 2, 0, 1), b = bar(1, 4, 0, 1, 1.5, 2), back = bar(4, 1, 0, 1, 1.5, 2);
            const double mutual = rf::partialInductance(a, b);
            QVERIFY2(near(mutual, 6.18477403688 * unit, 1e-9), qPrintable(QString::number(mutual / unit, 'g', 12)));
            QVERIFY2(near(-rf::partialInductance(a, back), mutual, 1e-12), qPrintable(QString::number(rf::partialInductance(a, back) / unit)));
            QVERIFY2(near(rf::partialInductance(b, a), mutual, 1e-9), qPrintable(QString::number(rf::partialInductance(b, a) / unit)));
        }
        {
            // (A bar a million times longer than it is wide: the exact sum's
            //  terms 1e16 times it, its rounding all of it - 6,939 where its
            //  own is 16,198 - and its geometric mean distance's, within a
            //  part in 1e4 so long, is taken.)
            const rf::Filament a = bar(0, 1000, 0, 0.002, 0, 1);
            const double own = rf::partialInductance(a, a), byDistance = exactPair(1000 * um, rf::selfDistance(0.002 * um, um));
            QVERIFY2(near(own, byDistance, 1e-12), qPrintable(QStringLiteral("%1 against %2").arg(own / unit).arg(byDistance / unit)));
        }

        const QList<QPointF> line{{-37.5, -67.5}, {-37.5, 37.5}, {37.5, 37.5},  {37.5, -37.5},
                                  {-30.5, -37.5}, {-30.5, 30.5}, {30.5, 30.5}, {30.5, -30.5}, {-26, -30.5}};
        const rf::Conductor coil = widened(line, 5);
        const rf::Stack stack = grounded(4.1, 8e-6, 3e-6, 5.8e7);
        rf::MeshSettings how;
        how.cellsAcross = 3;
        how.gaps = true;
        const rf::Mesh mesh = rf::meshOf({coil}, how, stack, 1e9);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const rf::Inductance loop = rf::inductanceOf(mesh, {coil}, stack, line.first(), line.last());
        QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
        const QList<QList<double>> l = rf::partialInductances(loop.runs);
        const int n = int(l.size());
        Eigen::MatrixXd m(n, n);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) m(i, j) = l.at(i).at(j) / std::sqrt(l.at(i).at(i) * l.at(j).at(j));
        const Eigen::VectorXd eigen = Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd>(m, Eigen::EigenvaluesOnly).eigenvalues();
        int negative = 0;
        for (int i = 0; i < n; ++i) negative += eigen(i) < 0 ? 1 : 0;
        QVERIFY2(negative == 0, qPrintable(QStringLiteral("%1 of %2 runs' L's eigenvalues negative, the least %3").arg(negative).arg(n).arg(eigen(0))));
    }

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

    // The full-wave solve's L is the cosine between two runs times their
    // aligned partial inductance (alignedInductance()), a kernel smooth in
    // where they are - which a compressed matrix of it wants. Of two parallel
    // runs it is partialInductance()'s, whichever way each points; it is the
    // same either way round; and of two runs at 60 degrees, five of their
    // lengths apart, the cosine times it is Neumann's double integral of
    // dl.dl'/r along them, here by Gauss's rule (to the second order of
    // their length over their distance, which is where the turned runs'
    // geometry differs). partialInductance() of two oblique runs is the
    // cosine times the aligned one too: as though parallel, without the
    // cosine, as it was until 14 November, it was 2.11 times
    // Neumann's here, and a dense loop at 45 degrees 30-50 % high.
    void anObliquePairIsTheCosineTimesItsAlignedOne()
    {
        const double l = 100e-6, w = 1e-6;
        const rf::Filament a = alongX(0, l, 0, w), b = alongX(l, 0, 30e-6, w), c = alongX(20e-6, 120e-6, 400e-6, w);
        for (const rf::Filament* other : {&b, &c}) {
            QCOMPARE(rf::alignedInductance(a, *other), std::abs(rf::partialInductance(a, *other)));
            QCOMPARE(rf::alignedInductance(*other, a), rf::alignedInductance(a, *other));
        }
        QCOMPARE(rf::alignedInductance(a, a), rf::partialInductance(a, a));
        // At 60 degrees, its middle 500 µm off.
        const double angle = M_PI / 3;
        rf::Filament slant;
        slant.from = QPointF(200e-6, 500e-6);
        slant.to = slant.from + QPointF(std::cos(angle), std::sin(angle)) * l;
        slant.width = w;
        const double aligned = rf::alignedInductance(a, slant);
        QCOMPARE(rf::alignedInductance(slant, a), aligned);
        // Neumann's: mu0/4pi Int Int dl.dl'/r, sixteen points along each.
        static const double xg[8] = {-0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                                     0.1834346424956498,  0.5255324099163290,  0.7966664774136267,  0.9602898564975363};
        static const double wg[8] = {0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                                     0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};
        double neumann = 0;
        for (int half = 0; half < 2; ++half)
            for (int i = 0; i < 8; ++i)
                for (int hb = 0; hb < 2; ++hb)
                    for (int j = 0; j < 8; ++j) {
                        const double s = l * (0.25 * (2 * half + 1) + 0.25 * xg[i]);
                        const double t = l * (0.25 * (2 * hb + 1) + 0.25 * xg[j]);
                        const QPointF p(s, 0), q = slant.from + QPointF(std::cos(angle), std::sin(angle)) * t;
                        neumann += 0.25 * wg[i] * wg[j] * 0.25 / std::hypot(p.x() - q.x(), p.y() - q.y());
                    }
        neumann *= kMu0 / (4 * M_PI) * l * l * std::cos(angle);
        const double cosine = std::cos(angle) * aligned;
        qWarning("60 degrees: cos x aligned %.6g pH, Neumann's %.6g pH; partialInductance %.6g pH", cosine * 1e12, neumann * 1e12,
                 rf::partialInductance(a, slant) * 1e12);
        QVERIFY2(std::abs(cosine / neumann - 1) < 5e-3, qPrintable(QStringLiteral("%1 vs %2").arg(cosine).arg(neumann)));
        // And partialInductance(), the dense loop's, the same - the cosine
        // signed, which way round the second points.
        QCOMPARE(rf::partialInductance(a, slant), cosine);
        rf::Filament back = slant;
        std::swap(back.from, back.to);
        QCOMPARE(rf::partialInductance(a, back), -cosine);
        // At right angles its kernel is still a number, smooth in where they
        // are, though nothing passes between them.
        const double across = rf::alignedInductance(a, alongY(500e-6, 600e-6, 200e-6, w));
        QVERIFY2(across > 0 && std::isfinite(across), qPrintable(QString::number(across)));
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

    // A strip standing upright beside one lying in the plane - a metal's
    // wall and its face, solved as two faces whose edge runs carry current
    // down their walls -: the partial
    // inductance by the two cross-sections' geometric mean distance, against
    // the double integral averaged over both cross-sections by a midpoint
    // rule fine enough to settle. Strips that meet at a corner, two walls end
    // to end (a top face's half and its bottom's), and a wall across a gap
    // from a face. The four-point average across widths is not good enough
    // where they meet.
    void anUprightStripBesideAFlatOneIsTheirDoubleIntegral()
    {
        for (const double l : {200e-6, 30e-6}) {   // (ten times the face's width, and a run as short as a rooftop's)
        const double w = 20e-6, t = 6e-6;
        const auto flat = [l](double y, double z, double width) {
            rf::Filament f;
            f.from = QPointF(0, y);
            f.to = QPointF(l, y);
            f.z0 = f.z1 = z;
            f.width = width;
            return f;
        };
        const auto upright = [l](double y, double z, double rise) {
            rf::Filament f;
            f.from = QPointF(0, y);
            f.to = QPointF(l, y);
            f.z0 = f.z1 = z;
            f.rise = rise;
            return f;
        };
        // (Each a cross-section's segment, (y, z) from and to.)
        struct Case {
            const char* what;
            rf::Filament a, b;
            double a0y, a0z, a1y, a1z, b0y, b0z, b1y, b1z;
        };
        const Case cases[] = {
            {"a wall meeting its face at a corner", flat(0, 0, w), upright(w / 2, t / 4, t / 2), -w / 2, 0, w / 2, 0, w / 2, 0, w / 2,
             t / 2},
            {"two walls end to end", upright(0, t / 4, t / 2), upright(0, 3 * t / 4, t / 2), 0, 0, 0, t / 2, 0, t / 2, 0, t},
            {"a wall across a gap from a face", upright(w / 2, t / 4, t / 2), flat(w / 2 + 10e-6 + w / 2, 0, w), w / 2, 0, w / 2, t / 2,
             w / 2 + 10e-6, 0, w / 2 + 10e-6 + w, 0},
        };
        for (const Case& c : cases) {
            const double got = rf::partialInductance(c.a, c.b);
            const int n = 240;
            double sum = 0;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const double u = (i + 0.5) / n, v = (j + 0.5) / n;
                    const double ya = c.a0y + u * (c.a1y - c.a0y), za = c.a0z + u * (c.a1z - c.a0z);
                    const double yb = c.b0y + v * (c.b1y - c.b0y), zb = c.b0z + v * (c.b1z - c.b0z);
                    sum += exactPair(l, std::max(std::hypot(ya - yb, za - zb), 1e-12));
                }
            const double wanted = sum / (double(n) * n);
            QVERIFY2(std::abs(got / wanted - 1) < 5e-4,
                     qPrintable(QStringLiteral("%1, %2 µm long: %3 pH vs %4 pH")
                                    .arg(QLatin1String(c.what)).arg(l * 1e6).arg(got * 1e12).arg(wanted * 1e12)));
            // (Symmetric.)
            QVERIFY2(std::abs(rf::partialInductance(c.b, c.a) / got - 1) < 1e-12, c.what);
        }
        // An upright strip's own: a strip of its rise's, as a flat one's of
        // its width.
        QCOMPARE(rf::partialInductance(upright(0, 0, w), upright(0, 0, w)), rf::partialInductance(flat(0, 0, w), flat(0, 0, w)));
        }
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
        // In the order of the cells they join, not of a hash, which Qt seeds
        // anew in every run of the program: a compressed solve's clusters are
        // cut from this order, and the same model solved twice was 1e-7 apart.
        QVERIFY(std::is_sorted(ends.begin(), ends.end()));
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
    // A loop whose two ends' points lie on a plate under them too - a model
    // built for the full wave, its two via ports both from one ground plate
    // to the leads -: each end on the metal it names, the leads', not on the
    // plate's one edge both points are on ("both ends
    // of the loop are on the same face"). The bar's L as without the plate.
    void aLoopsEndsAreOnTheMetalsTheyName()
    {
        rf::Stack stack = grounded(4, 100e-6, 3e-6, 5.8e7);
        rf::Layer plate = stack.layers.at(2), oxide = stack.layers.at(1);
        plate.name = QStringLiteral("M0");
        plate.tag = QStringLiteral("m0");
        oxide.kind = rf::Layer::Dielectric;
        oxide.name = QStringLiteral("OX");
        oxide.tag = QStringLiteral("ox");
        oxide.thickness = 10e-6;
        stack.layers.insert(2, plate);
        stack.layers.insert(3, oxide);
        const double length = 400.0, width = 40.0;
        rf::Conductor under;
        under.layer = QStringLiteral("M0");
        under.outline = QPolygonF(QRectF(-50, 20, 500, 100));   // (its lower edge through both ends' points)
        const QList<rf::Conductor> both{strip(0, 0, length, width), under};
        const rf::Mesh mesh = meshed(both, 20);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const QPointF a(10, 20), b(length - 10, 20);
        const rf::Inductance plain = rf::inductanceOf(mesh, both, stack, a, b, 1e-6, 1e9);
        QVERIFY2(!plain.ok() && plain.problems.join(' ').contains(QStringLiteral("same face")), qPrintable(plain.problems.join(" | ")));
        const rf::Inductance named = rf::inductanceOf(mesh, both, stack, a, b, 1e-6, 1e9, nullptr, QStringLiteral("M1"), QStringLiteral("M1"));
        QVERIFY2(named.ok(), qPrintable(named.problems.join(" | ")));
        const QList<rf::Conductor> alone{strip(0, 0, length, width)};
        const rf::Inductance bar = rf::inductanceOf(meshed(alone, 20), alone, stack, a, b, 1e-6, 1e9, nullptr, QStringLiteral("M1"), QStringLiteral("M1"));
        QVERIFY2(bar.ok() && std::abs(named.l / bar.l - 1) < 1e-6, qPrintable(QStringLiteral("%1 against %2 nH").arg(named.l * 1e9).arg(bar.l * 1e9)));
    }

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

    // At direct current the resistance divides the current, and a bar's
    // resistance is rho l/(w t) exactly whatever the mesh - of ten cells
    // across, the outermost half as wide, as the mesh's own default makes
    // it. The least-energy current crowds to the edges instead, and its
    // loss, given as the resistance at direct current until then, came out
    // 29 % high here.
    void aBarsResistanceAtDirectCurrentIsItsOwnOnAnyMesh()
    {
        const double length = 400.0, width = 40.0;   // µm
        const QList<rf::Conductor> bar{strip(0, 0, length, width)};
        rf::MeshSettings how;   // ten across, graded, stretched along
        how.maximum = 40e-6;
        const rf::Mesh mesh = rf::meshOf(bar, how, rf::Stack(), 0);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        QVERIFY2(mesh.cells.size() > 100, qPrintable(QString::number(mesh.cells.size())));
        const double sigma = 5.8e7, thick = 3e-6;
        const rf::Stack stack = grounded(9.8, 100e-6, thick, sigma);
        const rf::Inductance got = rf::inductanceOf(mesh, bar, stack, QPointF(0, width / 2), QPointF(length, width / 2),
                                                    1e-6, 10e9);
        QVERIFY2(got.ok(), qPrintable(got.problems.join(" | ")));
        const double bars = length * 1e-6 / (sigma * width * 1e-6 * thick);
        QVERIFY2(std::abs(got.rDc - bars) < 1e-9 * bars,
                 qPrintable(QStringLiteral("%1 Ohm solved vs %2 Ohm of the bar").arg(got.rDc).arg(bars)));
        // The current at direct current is the bar's even current, whose
        // inductance is the textbook's; the least-energy current's is no
        // more than it, being the least.
        const double wanted = textbookStrip(length * 1e-6, width * 1e-6);
        QVERIFY2(std::abs(got.lDc - wanted) < 0.02 * wanted,
                 qPrintable(QStringLiteral("%1 nH at direct current vs %2 nH of the bar").arg(got.lDc * 1e9).arg(wanted * 1e9)));
        QVERIFY2(got.l <= got.lDc * (1 + 1e-9) && got.l > 0.9 * got.lDc,
                 qPrintable(QStringLiteral("%1 nH least, %2 nH at direct current").arg(got.l * 1e9).arg(got.lDc * 1e9)));
        // And Q is of the one inductance over the true resistance.
        QVERIFY2(std::abs(got.q - 2 * M_PI * 10e9 * got.l / got.rDc) < 1e-9 * got.q, qPrintable(QString::number(got.q)));
        QVERIFY2(got.lines.join(QLatin1Char(' ')).contains(QStringLiteral("at direct current")), qPrintable(got.lines.join(" | ")));
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
        // (A mesh past what a dense matrix of this build holds is no longer
        // refused but solved compressed: aLoopPastTheDenseLimitIsSolvedCompressed.)
    }

    // Past the dense limit: the same
    // loop solved compressed - L as H-matrices, GMRES preconditioned by the
    // near field's LU - is the dense solve's, on a U of three strips, a
    // plate whose current turns a corner and two metals joined by a via:
    // L, L and R at direct current to a part in 1e6, each run's current to
    // 1e-4 of the largest. And the mesh this build used to refuse, past 3000 runs and
    // cells, is solved, its L within 2 % of a coarser dense mesh's, said.
    // A via farm that feeds a plate across its area, merged (the
    // processing's default), feeds it so still:
    // the union its footprint, conducting as the vias in it do
    // (Conductor::share), where it was shrunk to the vias' area - a block in
    // the plate's middle, the plate's spreading added in series (0.44 Ohm on
    // the review's capacitor). Two plates 60 µm square of 0.2 µm metal, one
    // on M1 and one on M2, each fed by a strip 40 µm long, 36 vias of 2 µm on a 10 µm
    // pitch between them; R at direct current end to end, unmerged, merged,
    // and merged as solid metal (asSolid(): the old shrunk block, thinned in
    // place) - 0.5158, 0.5169 and 0.5581 Ohm.
    void aViaFarmMergedFeedsItsPlateAcrossItsArea()
    {
        rf::Stack two = grounded(1, 100e-6, 0.2e-6, 5.8e7);
        rf::Layer ild;
        ild.kind = rf::Layer::Dielectric;
        ild.name = QStringLiteral("ILD");
        ild.tag = QStringLiteral("ild");
        ild.thickness = 1e-6;
        ild.epsr = 4;
        rf::Layer via;
        via.kind = rf::Layer::Via;
        via.name = QStringLiteral("V1");
        via.tag = QStringLiteral("v1");
        via.from = QStringLiteral("M1");
        via.to = QStringLiteral("M2");
        via.sigma = 1e5;   // (resistive: the vias some 70 mOhm of it, so that their own conductance tells)
        rf::Layer m2 = two.layers.last();
        m2.name = QStringLiteral("M2");
        m2.tag = QStringLiteral("m2");
        two.layers << ild << via << m2;
        QMap<QString, QList<QPolygonF>> drawn;
        drawn[QStringLiteral("M1")] = {QPolygonF(QRectF(0, 0, 60, 60)), QPolygonF(QRectF(-40, 20, 40, 20))};
        drawn[QStringLiteral("M2")] = {QPolygonF(QRectF(0, 0, 60, 60)), QPolygonF(QRectF(60, 20, 40, 20))};
        double wanted = 0;
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j) {
                drawn[QStringLiteral("V1")] << QPolygonF(QRectF(4 + 10 * i, 4 + 10 * j, 2, 2));
                wanted += 4;
            }
        const auto resistance = [&](bool merge, bool solid, QString* said) {
            rf::Processing how;
            how.dropSlivers = false;
            how.mergeVias = merge;
            how.viaDistance = 9e-6;
            const rf::Processed done = rf::processShapes(drawn, {}, how, two);
            const QList<rf::Conductor> cs = solid ? rf::asSolid(done.conductors) : done.conductors;
            double metal = 0;
            for (const rf::Conductor& c : cs)
                if (c.layer == QLatin1String("V1")) metal += c.area() * c.share;
            *said = QStringLiteral("%1 via shape(s), %2 um2 of metal").arg(done.byLayer().value(QStringLiteral("V1"))).arg(metal);
            if (std::abs(metal / wanted - 1) > 0.02) return -1.0;
            const rf::Inductance l = rf::inductanceOf(meshed(cs, 2), cs, two, QPointF(-40, 30), QPointF(100, 30), 1e-6, 0,
                                                      nullptr, QStringLiteral("M1"), QStringLiteral("M2"));
            if (!l.ok()) *said += l.problems.join(" | ");
            return l.ok() ? l.rDc : -1.0;
        };
        QString a, b, c;
        const double apart = resistance(false, false, &a), merged = resistance(true, false, &b), shrunk = resistance(true, true, &c);
        qWarning("a plate fed by a via farm: R_dc %.4f Ohm unmerged (%s), %.4f merged (%s), %.4f merged as solid metal (%s)", apart,
                 qPrintable(a), merged, qPrintable(b), shrunk, qPrintable(c));
        QVERIFY2(apart > 0 && merged > 0 && shrunk > 0, qPrintable(a + " / " + b + " / " + c));
        QVERIFY2(std::abs(merged / apart - 1) < 0.02, qPrintable(QStringLiteral("%1 against %2 Ohm").arg(merged).arg(apart)));
        QVERIFY2(shrunk / apart - 1 > 0.05, qPrintable(QStringLiteral("%1 against %2 Ohm").arg(shrunk).arg(apart)));
    }

    void aLoopPastTheDenseLimitIsSolvedCompressed()
    {
        const rf::Stack stack = grounded(9.8, 100e-6, 3e-6, 5.8e7);
        const auto both = [&stack](const QList<rf::Conductor>& shapes, const rf::Stack& on, double cell, QPointF from, QPointF to) {
            const rf::Mesh mesh = meshed(shapes, cell);
            rf::LoopProgress sizedDense;
            const rf::Inductance dense = rf::inductanceOf(mesh, shapes, on, from, to, 1e-6, 0, nullptr, QString(), QString(), &sizedDense);
            rf::setLoopDenseLimit(50);
            // (How far it has got, for what is said while it runs: its runs
            //  and cells, compressed, and its GMRES's last iteration and
            //  residual.)
            rf::LoopProgress progress;
            const rf::Inductance compressed = rf::inductanceOf(mesh, shapes, on, from, to, 1e-6, 0, nullptr, QString(), QString(), &progress);
            rf::setLoopDenseLimit(0);
            Q_UNUSED(stack);
            QString said;
            if (progress.runs.load() != compressed.branches || !progress.compressed.load() || progress.iterations.load() != compressed.iterations
                || !(progress.residual.load() > 0 && progress.residual.load() < 1e-9) || sizedDense.compressed.load() || sizedDense.iterations.load() != 0)
                said += QStringLiteral("progress: %1 runs (%2), compressed %3, iteration %4 (%5), residual %6; dense said compressed %7; ")
                            .arg(progress.runs.load()).arg(compressed.branches).arg(progress.compressed.load()).arg(progress.iterations.load())
                            .arg(compressed.iterations).arg(progress.residual.load()).arg(sizedDense.compressed.load());
            if (!dense.ok() || dense.compressed) said += QStringLiteral("dense: %1; ").arg(dense.problems.join(" | "));
            if (!compressed.ok() || !compressed.compressed) said += QStringLiteral("compressed: %1; ").arg(compressed.problems.join(" | "));
            if (!said.isEmpty()) return said;
            for (const auto& [name, a, b] : {std::make_tuple("L", dense.l, compressed.l), std::make_tuple("L dc", dense.lDc, compressed.lDc),
                                            std::make_tuple("R dc", dense.rDc, compressed.rDc)})
                if (std::abs(b / a - 1) > 1e-6) said += QStringLiteral("%1: %2 against %3; ").arg(QLatin1String(name)).arg(b).arg(a);
            // Each run's current within 1e-4 of the largest: the compressed
            // L is ACA's to 1e-6 a block, and a run's current is first order
            // in it, where the energy, L, is second. GMRES met its 1e-10 on
            // every platform; what is left is how the cross approximation
            // fell - the U's runs 2.9e-6 A apart here (clang, arm64), 1.7e-5
            // on CI's sanitized Linux (GCC at -O0, no FMA), past the 1e-5 A
            // this said until 12 November, its L within its 1e-6 all the same.
            double most = 0, largest = 0;
            for (int i = 0; i < dense.current.size(); ++i) {
                most = std::max(most, std::abs(dense.current.at(i) - compressed.current.value(i)));
                largest = std::max(largest, std::abs(dense.current.at(i)));
            }
            qInfo("compressed: %lld runs, the largest current %.3g A, a run's at most %.3g A from the dense solve's; GMRES %d iteration(s) to %.2g",
                  qlonglong(dense.current.size()), largest, most, compressed.iterations, compressed.residual);
            if (most > 1e-4 * largest) said += QStringLiteral("a run's current %1 A out, the largest %2 A; ").arg(most).arg(largest);
            if (!compressed.lines.join(' ').contains(QStringLiteral("solved compressed: L held in")))
                said += compressed.lines.join(" | ");
            return said;
        };
        const QList<rf::Conductor> u{strip(0, 0, 600, 40), strip(560, 40, 40, 300), strip(0, 340, 600, 40)};
        QString said = both(u, stack, 10, QPointF(0, 20), QPointF(0, 360));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        const QList<rf::Conductor> plate{strip(0, 0, 400, 200)};
        said = both(plate, stack, 20, QPointF(0, 100), QPointF(200, 0));
        QVERIFY2(said.isEmpty(), qPrintable(said));
        // Two metals and a via between them: a strip on M1, back on M2.
        rf::Stack two = stack;
        rf::Layer ild;
        ild.kind = rf::Layer::Dielectric;
        ild.name = QStringLiteral("ILD");
        ild.tag = QStringLiteral("ild");
        ild.thickness = 5e-6;
        ild.epsr = 4;
        rf::Layer via;
        via.kind = rf::Layer::Via;
        via.name = QStringLiteral("V1");
        via.tag = QStringLiteral("v1");
        via.from = QStringLiteral("M1");
        via.to = QStringLiteral("M2");
        via.sigma = 5.8e7;
        rf::Layer m2 = two.layers.last();
        m2.name = QStringLiteral("M2");
        m2.tag = QStringLiteral("m2");
        two.layers << ild << via << m2;
        rf::Conductor upper = strip(0, 60, 400, 40), post = strip(380, 40, 20, 20);
        rf::Conductor lower = strip(0, 0, 400, 40);
        upper.layer = QStringLiteral("M2");
        post.layer = QStringLiteral("V1");
        rf::Conductor lowerReach = strip(380, 40, 20, 20);
        rf::Conductor upperReach = strip(380, 40, 20, 20);
        upperReach.layer = QStringLiteral("M2");
        said = both({lower, lowerReach, post, upperReach, upper}, two, 10, QPointF(0, 20), QPointF(0, 80));
        QVERIFY2(said.isEmpty(), qPrintable(said));

        // The mesh this build used to refuse. (Not under the sanitizers: its
        // two solves, compressed and a coarser dense one, are most of the
        // function there.)
        if (slowBuild()) return;
        const QList<rf::Conductor> big{strip(0, 0, 400, 120)};
        const rf::Inductance over = rf::inductanceOf(meshed(big, 5), big, stack, QPointF(0, 60), QPointF(400, 60));
        QVERIFY2(over.ok() && over.compressed && over.branches + over.nodes > 3000, qPrintable(over.problems.join(" | ")));
        QCOMPARE(rf::loopDenseLimit(), 3000);
        const rf::Inductance coarse = rf::inductanceOf(meshed(big, 10), big, stack, QPointF(0, 60), QPointF(400, 60));
        QVERIFY2(coarse.ok() && !coarse.compressed, qPrintable(coarse.problems.join(" | ")));
        QVERIFY2(std::abs(over.l / coarse.l - 1) < 0.02, qPrintable(QStringLiteral("%1 against %2 nH").arg(over.l * 1e9).arg(coarse.l * 1e9)));
        qWarning().noquote() << over.lines.join(QStringLiteral(" | "));
    }

    // A compressed loop whose GMRES does not converge is solved dense where
    // that fits, and says so; where it does not, it says how far it got, what
    // dense would take, and that another mesh may converge - not "Mesh the
    // layout more coarsely, so that it is solved dense", which past the
    // dense limit no mesh but a far coarser one could do (the review's P71).
    // GMRES held to two iterations stands in for one that stands still.
    void aCompressedLoopThatDoesNotConvergeIsSolvedDense()
    {
        const rf::Stack stack = grounded(9.8, 100e-6, 3e-6, 5.8e7);
        const QList<rf::Conductor> u{strip(0, 0, 600, 40), strip(560, 40, 40, 300), strip(0, 340, 600, 40)};
        const rf::Mesh mesh = meshed(u, 10);
        const rf::Inductance dense = rf::inductanceOf(mesh, u, stack, QPointF(0, 20), QPointF(0, 360));
        QVERIFY2(dense.ok() && !dense.compressed, qPrintable(dense.problems.join(" | ")));
        rf::setLoopDenseLimit(50);
        rf::setLoopMostIterations(2);
        const rf::Inductance fallen = rf::inductanceOf(mesh, u, stack, QPointF(0, 20), QPointF(0, 360));
        rf::setLoopMemory(40e6);
        const rf::Inductance refused = rf::inductanceOf(mesh, u, stack, QPointF(0, 20), QPointF(0, 360));
        rf::setLoopMemory(0);
        rf::setLoopMostIterations(0);
        rf::setLoopDenseLimit(0);
        QVERIFY2(fallen.ok() && !fallen.compressed, qPrintable(fallen.problems.join(" | ")));
        QVERIFY2(std::abs(fallen.l / dense.l - 1) < 1e-9 && std::abs(fallen.lDc / dense.lDc - 1) < 1e-9,
                 qPrintable(QStringLiteral("%1 and %2 nH against %3 and %4").arg(fallen.l * 1e9).arg(fallen.lDc * 1e9).arg(dense.l * 1e9).arg(dense.lDc * 1e9)));
        const QString said = fallen.lines.join(' ');
        QVERIFY2(said.contains(QStringLiteral("solved compressed first: GMRES did not converge")) && said.contains(QStringLiteral("so the loop was solved dense")),
                 qPrintable(said));
        QVERIFY(!refused.ok());
        const QString why = refused.problems.join(' ');
        QVERIFY2(why.contains(QStringLiteral("did not converge")) && why.contains(QStringLiteral("solved dense, more than"))
                     && why.contains(QStringLiteral("may converge")) && !why.contains(QStringLiteral("more coarsely")),
                 qPrintable(why));
    }


    // Mohan's spirals, at the cell
    // sizes where their loops came out shorted: "L02 ... 15 µm 0.0151 Ohm",
    // "L05 ... 10 µm 0.387 Ohm", "L16 ... 15 µm 0.00106 Ohm". Two
    // things together: each turn's rectangles graded from their own ends
    // met in lines a hair apart, cells 0.5 nm wide whose edges the runs could
    // not tell apart (cells left joined to nothing); and the loop's ports
    // were numbered as the count of joined cells, which was then a real
    // cell's number turns away. Now no cell is thinner than a quarter of the
    // finest grading, and each loop is its metal's resistance along its
    // length and Mohan's measured inductance.
    void aSpiralIsMeshedCleanlyAndItsLoopIsItsLength()
    {
        if (slowBuild()) QSKIP("three spirals' loops held to the measured values on their meshes: many minutes under the sanitizers");
        const rf::Stack stack = grounded(3.9, 10e-6, 0.9e-6, 3.5e7);
        const double sheet = 1 / (3.5e7 * 0.9e-6);
        struct Spiral {
            const char* name;
            double n, dout, w, s, largest, measured;
        };
        for (const Spiral& sp : {Spiral{"L02", 3.75, 292, 13, 1.9, 15, 6.00}, Spiral{"L05", 4.75, 206, 7.8, 1.9, 10, 6.10},
                                 Spiral{"L16", 12, 180, 3.2, 2.1, 15, 20.5}}) {
            const QList<QPointF> line = mohanCentreLine(4, sp.n, sp.dout, sp.w, sp.s);
            const rf::Conductor spiral = widened(line, sp.w);
            const rf::Mesh mesh = rf::meshOf({spiral}, likeTheTab(sp.largest), stack, 1e9);
            QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
            // (A quarter of the finest grading at most: its gaps' cells are
            //  halved to a quarter of 1.9 µm, 0.24 µm, and were 0.0005.)
            QVERIFY2(mesh.smallest > 0.05, qPrintable(QStringLiteral("%1: a cell %2 µm across").arg(sp.name).arg(mesh.smallest)));
            const rf::Inductance loop = rf::inductanceOf(mesh, {spiral}, stack, line.first(), line.last());
            QVERIFY2(loop.ok(), qPrintable(QStringLiteral("%1: %2").arg(sp.name, loop.problems.join(" | "))));
            // (Its corners counted as whole squares overstate the length a
            //  little: 0.98-0.99 of it, as the review measured.)
            const double along = sheet * lengthOf(line) / sp.w;
            QVERIFY2(loop.rDc > 0.95 * along && loop.rDc < along,
                     qPrintable(QStringLiteral("%1: %2 Ohm against %3 along it").arg(sp.name).arg(loop.rDc).arg(along)));
            QVERIFY2(loop.rDcLeast > 0 && loop.rDcLeast <= loop.rDc, qPrintable(QString::number(loop.rDcLeast)));
            QVERIFY2(std::abs(loop.lDc * 1e9 / sp.measured - 1) < 0.03,
                     qPrintable(QStringLiteral("%1: %2 nH against %3 measured").arg(sp.name).arg(loop.lDc * 1e9).arg(sp.measured)));
            qWarning("%s at %g um: %lld cells, the smallest %.3g um; L_dc %.4g nH (measured %.3g), R_dc %.4g Ohm (%.3f of along it)",
                     sp.name, sp.largest, qlonglong(mesh.cells.size()), mesh.smallest, loop.lDc * 1e9, sp.measured, loop.rDc,
                     loop.rDc / along);
        }
    }

    // A cell joined to nothing - a shape smaller than a cell, on its own -
    // is no part of the loop, and the current still goes in where the port
    // is: the ports were numbered as the count of the cells that runs join,
    // which with one left out was the last cell of the strip's own number,
    // beside where the current comes out, and the strip was shorted.
    //
    // And a piece joined to itself but to neither end - two cells of their
    // own - is left out, and said: solved with it, the nodal matrix was
    // singular.
    void aCellJoinedToNothingIsNoPlaceForTheCurrentToGoIn()
    {
        const rf::Stack stack = grounded(1, 100e-6, 1e-6, 5.8e7);
        const double along = 400.0 / 20 / (5.8e7 * 1e-6);
        {
            const QList<rf::Conductor> both{strip(0, 300, 2, 2), strip(100, 0, 400, 20)};
            const rf::Mesh mesh = meshed(both, 10);
            const rf::Inductance loop = rf::inductanceOf(mesh, both, stack, QPointF(100, 10), QPointF(500, 10));
            QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
            QVERIFY2(std::abs(loop.rDc / along - 1) < 0.01, qPrintable(QStringLiteral("%1 Ohm against %2").arg(loop.rDc).arg(along)));
            // (The strip's 80 cells and the two ports': the lone cell is none.)
            QCOMPARE(loop.nodes, 82);
        }
        const QList<rf::Conductor> three{strip(0, 300, 20, 10), strip(100, 0, 400, 20)};
        const rf::Mesh mesh = meshed(three, 10);
        const rf::Inductance loop = rf::inductanceOf(mesh, three, stack, QPointF(100, 10), QPointF(500, 10));
        QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
        QVERIFY2(std::abs(loop.rDc / along - 1) < 0.01, qPrintable(QStringLiteral("%1 Ohm against %2").arg(loop.rDc).arg(along)));
        QVERIFY2(loop.lines.join(' ').contains(QStringLiteral("2 cell(s) joined to neither end of the loop are left out")),
                 qPrintable(loop.lines.join(" | ")));
        QCOMPARE(loop.nodes, 82);
    }

    // A strip at 45 degrees is the strip along x. Three faults made it 30-50
    // % high, or a third of its resistance, or "in pieces": two oblique runs'
    // partial inductance measured along the first without Neumann's cosine
    // (the dense loop's; the compressed one had the cosine); a cell cut by
    // the slanted side held at its box's middle, which for a right triangle
    // is on its hypotenuse, so that a stub to it had no length; and L in
    // picohenries beside A's ones, which the dense LU took for singular on a
    // fine mesh. And a U turned as well. (R at 45 degrees reads under the
    // strip's along x, the slanted mesh's own, and the less the finer: -16.1,
    // -6.0, -3.1 and -1.6 % at 1, 3, 6 and 12 across. At 3 across it read
    // -3.8 % while some of each end's cells, their corners off the slanted
    // face by the cuts' rounding, were not found and the current crowded
    // into the rest.)
    void aStripAt45DegreesIsTheStripAlongX()
    {
        const rf::Stack stack = grounded(3.9, 10e-6, 0.9e-6, 3.5e7);
        const double L = 400, w = 18, c = M_SQRT1_2;
        const auto turned = [c](double x, double y) { return QPointF(c * x - c * y, c * x + c * y); };
        const auto shape = [](const QList<QPointF>& points) {
            rf::Conductor k;
            k.layer = QStringLiteral("M1");
            k.outline = QPolygonF(points);
            return k;
        };
        const auto loopOf = [&](const rf::Conductor& k, QPointF a, QPointF b, const rf::MeshSettings& how) {
            return rf::inductanceOf(rf::meshOf({k}, how, stack, 1e9), {k}, stack, a, b);
        };
        const QList<QPointF> u{{0, 0}, {200, 0}, {200, 200}, {200 - w, 200}, {200 - w, w}, {w, w}, {w, 200}, {0, 200}};
        QList<QPointF> uTurned;
        for (const QPointF& q : u) uTurned << turned(q.x(), q.y());
        for (const auto& [largest, across, within, resistance] :
             {std::tuple<double, int, double, double>{15, 1, 0.03, 0.2}, {5, 3, 0.015, 0.065}}) {
            const rf::MeshSettings how = likeTheTab(largest, across);
            const rf::Inductance flat = loopOf(shape({{0, 0}, {L, 0}, {L, w}, {0, w}}), QPointF(0, w / 2), QPointF(L, w / 2), how);
            const rf::Inductance slant = loopOf(shape({turned(0, 0), turned(L, 0), turned(L, w), turned(0, w)}), turned(0, w / 2),
                                                turned(L, w / 2), how);
            QVERIFY2(flat.ok() && slant.ok() && !slant.compressed, qPrintable((flat.problems + slant.problems).join(" | ")));
            const QString said = QStringLiteral("%1 µm, %2 across: %3 nH and %4 Ohm at 45 degrees, %5 and %6 along x")
                                     .arg(largest).arg(across).arg(slant.lDc * 1e9).arg(slant.rDc).arg(flat.lDc * 1e9).arg(flat.rDc);
            QVERIFY2(std::abs(slant.lDc / flat.lDc - 1) < within, qPrintable(said));
            QVERIFY2(std::abs(slant.rDc / flat.rDc - 1) < resistance, qPrintable(said));
            const rf::Inductance u0 = loopOf(shape(u), QPointF(w / 2, 200), QPointF(200 - w / 2, 200), how);
            const rf::Inductance u45 = loopOf(shape(uTurned), turned(w / 2, 200), turned(200 - w / 2, 200), how);
            QVERIFY2(u0.ok() && u45.ok(), qPrintable((u0.problems + u45.problems).join(" | ")));
            QVERIFY2(std::abs(u45.lDc / u0.lDc - 1) < within + 0.01,
                     qPrintable(QStringLiteral("a U: %1 nH at 45 degrees, %2 along x").arg(u45.lDc * 1e9).arg(u0.lDc * 1e9)));
            qWarning().noquote() << said;
            // (Under the sanitizers the coarser mesh alone: the finer, and
            // the dense and compressed loops below on it, past QtTest's 900 s
            // there.)
            if (slowBuild()) return;
        }
        // Dense and compressed, the same loop alike.
        const rf::Conductor slanted = shape({turned(0, 0), turned(L, 0), turned(L, w), turned(0, w)});
        const rf::Inductance dense = loopOf(slanted, turned(0, w / 2), turned(L, w / 2), likeTheTab(5, 3));
        rf::setLoopDenseLimit(100);
        const rf::Inductance held = loopOf(slanted, turned(0, w / 2), turned(L, w / 2), likeTheTab(5, 3));
        rf::setLoopDenseLimit(0);
        QVERIFY2(dense.ok() && held.ok() && held.compressed && !dense.compressed, qPrintable((dense.problems + held.problems).join(" | ")));
        QVERIFY2(std::abs(held.l / dense.l - 1) < 0.005, qPrintable(QStringLiteral("%1 against %2 nH").arg(held.l * 1e9).arg(dense.l * 1e9)));
    }

    // A long line's loop, compressed, stops at its rounding: its node
    // potentials grow with its cells along it, and their rounding is the
    // floor of GMRES's residual. A line 3 mm long on 1 µm cells met 1.35e-10
    // in 28 iterations and restarted at one product a time to the 3,000
    // allowed - 157 s for the answer it had at the 28th, where on 2 µm cells
    // it took 18 iterations and a third of a second (the review's P66: a
    // meander's 138,746 cells 885 s). Said, and its answer the same.
    void aLongLinesLoopStopsAtItsRounding()
    {
        const rf::Stack stack = grounded(1, 10e-3, 1e-6, 5.8e7);
        const QList<rf::Conductor> line{strip(0, 0, 3000, 10)};
        const rf::Inductance l = rf::inductanceOf(meshed(line, 1), line, stack, QPointF(0, 5), QPointF(3000, 5), 1e-6, 0);
        QVERIFY2(l.ok() && l.compressed, qPrintable(l.problems.join(" | ")));
        QVERIFY2(l.iterations < 200, qPrintable(QStringLiteral("%1 iterations to %2").arg(l.iterations).arg(l.residual)));
        QVERIFY2(l.residual < 1e-9, qPrintable(QString::number(l.residual)));
        QVERIFY2(std::abs(l.rDc * 5.8e7 * 1e-6 / 300 - 1) < 1e-6, qPrintable(QString::number(l.rDc * 5.8e7 * 1e-6)));
        QVERIFY2(std::abs(l.lDc / 4.13862e-9 - 1) < 1e-4, qPrintable(QString::number(l.lDc * 1e9)));
        QVERIFY2(l.lines.join(' ').contains(QStringLiteral("as low as its rounding goes")), qPrintable(l.lines.join(" | ")));
    }

    // Stopped while its near field is factorized - which Accelerate's
    // factorization cannot be, "still at work after 60 s" till Qucs-S was
    // quit -, the loop ends at once and
    // lets the factorization finish in the background. (Held there by the
    // test's hook, which asks it to stop and takes three seconds.)
    void aLoopStopsWhileItsNearFieldIsFactorized()
    {
        const rf::Stack stack = grounded(9.8, 100e-6, 1e-6, 5.8e7);
        const QList<rf::Conductor> big{strip(0, 0, 400, 120)};
        const rf::Mesh mesh = meshed(big, 5);
        static std::atomic_bool stop{false};
        // (Timed from the stop asked, not from the loop's start: the loop's
        // L and near field before it are some 2.5 s more under the
        // sanitizers, which 3197 ms there was.)
        static std::atomic<qint64> askedAt{0};
        static QElapsedTimer clock;
        clock.start();
        stop = false;
        askedAt = -1;
        rf::setLoopFactorizing([] {
            askedAt = clock.elapsed();
            stop = true;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        });
        const rf::Inductance stopped = rf::inductanceOf(mesh, big, stack, QPointF(0, 60), QPointF(400, 60), 1e-6, 0, &stop);
        const qint64 took = askedAt.load() >= 0 ? clock.elapsed() - askedAt.load() : -1;
        rf::setLoopFactorizing({});
        QVERIFY2(took >= 0, "the factorization was never reached");
        QVERIFY2(!stopped.ok() && stopped.problems.join(' ').contains(QStringLiteral("stopped")) &&
                     stopped.problems.join(' ').contains(QStringLiteral("in the background")),
                 qPrintable(stopped.problems.join(" | ")));
        QVERIFY2(took < 2500, qPrintable(QStringLiteral("%1 ms").arg(took)));
    }

    // A magnetic layer in the stack: the loop's partial inductances are free
    // space's, and it says so - the full wave has the material.
    void aLoopOverAMagneticLayerSaysItsLIsOfFreeSpace()
    {
        rf::Stack stack = grounded(4, 100e-6, 1e-6, 5.8e7);
        for (rf::Layer& l : stack.layers)
            if (l.kind == rf::Layer::Substrate) l.mur = 3;
        const QList<rf::Conductor> wire{strip(0, 0, 400, 20)};
        const rf::Inductance loop = rf::inductanceOf(meshed(wire, 10), wire, stack, QPointF(0, 10), QPointF(400, 10));
        QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
        QVERIFY2(loop.lines.join(' ').contains(QStringLiteral("is magnetic")), qPrintable(loop.lines.join(" | ")));
        // Nor of a metal lid over it, which it says too.
        rf::Stack lidded = grounded(4, 100e-6, 1e-6, 5.8e7);
        rf::Layer lid;
        lid.kind = rf::Layer::Cover;
        lid.name = QStringLiteral("LID");
        lid.tag = QStringLiteral("lid");
        lid.variant = QStringLiteral("perfect");
        lid.thickness = 200e-6;
        lidded.layers << lid;
        const rf::Inductance under = rf::inductanceOf(meshed(wire, 10), wire, lidded, QPointF(0, 10), QPointF(400, 10));
        QVERIFY2(under.ok() && under.lines.join(' ').contains(QStringLiteral("has a metal lid")), qPrintable(under.lines.join(" | ") + under.problems.join(" | ")));
        lidded.layers.last().walls = true;
        lidded.layers.last().box = QRectF(QPointF(-1e-3, -1e-3), QPointF(1e-3, 1e-3));
        const rf::Inductance boxed = rf::inductanceOf(meshed(wire, 10), wire, lidded, QPointF(0, 10), QPointF(400, 10));
        QVERIFY2(boxed.ok() && boxed.lines.join(' ').contains(QStringLiteral("a metal lid and side walls")), qPrintable(boxed.lines.join(" | ")));
    }

    // A strip of cells a tenth of a micrometre, solved dense: L's entries a
    // few times 1e-14 H beside A's ones, which the LU's test of whether it
    // is invertible - against its largest pivot - took for nothing, "in
    // pieces that nothing joins", until L was put in its own scale.
    void aFineStripIsSolvedDense()
    {
        if (slowBuild()) QSKIP("a thousand cells a tenth of a micrometre, dense: minutes under the sanitizers");
        const rf::Stack stack = grounded(1, 100e-6, 1e-6, 5.8e7);
        const QList<rf::Conductor> fine{strip(0, 0, 10, 1)};
        const rf::Inductance loop = rf::inductanceOf(meshed(fine, 0.1), fine, stack, QPointF(0, 0.5), QPointF(10, 0.5));
        QVERIFY2(loop.ok() && !loop.compressed, qPrintable(loop.problems.join(" | ")));
        const double along = 10.0 / 1 / (5.8e7 * 1e-6);
        QVERIFY2(std::abs(loop.rDc / along - 1) < 0.01, qPrintable(QStringLiteral("%1 Ohm against %2").arg(loop.rDc).arg(along)));
        // (1 µm wide and 1 µm thick: a bar, its own partial inductance of
        //  its square's geometric mean distance - 5.69 pH, where a strip of no
        //  thickness, the textbook's, is 6.99.)
        const double g = rf::selfDistance(1e-6, 1e-6), l = 10e-6;
        const double bar = kMu0 * l / (2 * M_PI) * (std::asinh(l / g) - std::sqrt(1 + g * g / (l * l)) + g / l);
        QVERIFY2(std::abs(loop.lDc / bar - 1) < 0.05, qPrintable(QStringLiteral("%1 pH against %2").arg(loop.lDc * 1e12).arg(bar * 1e12)));
    }

    // Polygon spirals of Mohan's Table IV, drawn as his Fig. 1 draws them:
    // the octagon L55 (n 4, dout 346, w 18, s 2 µm) and the dodecagon L61
    // (n 7.75, dout 198, w 6.5, s 3). A slanted side's trapezoid is one from
    // corner to corner, not cut again at every other corner's height - 221
    // slivers 0.83 µm across, 87,992 cells, whose loop ran the machine out of
    // memory -, and a part cut thin
    // between two corners at nearly one height, the dodecagon's 0.012 µm
    // apart, is meshed by its width, not its depth (12,770 cells, 64 s, the
    // gaps said to be 0.012 µm). Each loop is Mohan's ASITIC value to 3 %.
    void aPolygonSpiralIsMeshedCornerToCorner()
    {
        if (slowBuild()) QSKIP("two spirals' loops held to ASITIC's values on their meshes: many minutes under the sanitizers");
        const rf::Stack stack = grounded(3.9, 10e-6, 0.9e-6, 3.5e7);
        struct Spiral {
            const char* name;
            int sides;
            double n, dout, w, s;
            int most;
            double asitic;
        };
        for (const Spiral& sp : {Spiral{"L55", 8, 4, 346, 18, 2, 4000, 5.90}, Spiral{"L61", 12, 7.75, 198, 6.5, 3, 6500, 7.44}}) {
            const QList<QPointF> line = mohanCentreLine(sp.sides, sp.n, sp.dout, sp.w, sp.s);
            const rf::Conductor spiral = widened(line, sp.w);
            const rf::Mesh mesh = rf::meshOf({spiral}, likeTheTab(15), stack, 1e9);
            QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
            const QString said = QStringLiteral("%1: %2 cells, the narrowest conductor %3 µm, the narrowest gap %4 µm")
                                     .arg(sp.name).arg(mesh.cells.size()).arg(mesh.narrowest).arg(mesh.narrowestGap);
            QVERIFY2(mesh.cells.size() < sp.most, qPrintable(said));
            // (Within 0.2 %: a polygon's corners are its own and stay, where a
            //  curve's are put on one cut - moved too, the dodecagon's gaps read
            //  2.988 µm; straight across by a twentieth of its width, 2.85.)
            QVERIFY2(std::abs(mesh.narrowestGap / sp.s - 1) < 0.002, qPrintable(said));
            QVERIFY2(mesh.narrowest > 0.2 * sp.w, qPrintable(said));
            const rf::Inductance loop = rf::inductanceOf(mesh, {spiral}, stack, line.first(), line.last());
            QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
            QVERIFY2(std::abs(loop.lDc * 1e9 / sp.asitic - 1) < 0.03,
                     qPrintable(QStringLiteral("%1: %2 nH against ASITIC's %3").arg(sp.name).arg(loop.lDc * 1e9).arg(sp.asitic)));
            const double along = lengthOf(line) / sp.w / (3.5e7 * 0.9e-6);
            QVERIFY2(loop.rDc > 0.85 * along && loop.rDc < along,
                     qPrintable(QStringLiteral("%1: %2 Ohm against %3").arg(sp.name).arg(loop.rDc).arg(along)));
            qWarning("%s; L_dc %.4g nH (ASITIC %.3g), R_dc %.4g Ohm (%.3f of along it)", qPrintable(said), loop.lDc * 1e9, sp.asitic,
                     loop.rDc, loop.rDc / along);
        }
    }

    // A loop's ends on a slanted face take the whole face:
    // its cells' corners up to 0.7 nm off the face's line,
    // they were looked for within 1e-6 of its length, and the current went
    // in at the one cell nearest - a 2 mm strip's R at direct current 15 %
    // high at 10 degrees, 11 % at 30, against 45 degrees', whose corners
    // the cuts happen to keep on it. Now within 4 % of each other and of
    // the strip along x.
    void aLoopsEndsOnASlantedFaceTakeTheWholeFace()
    {
        const rf::Stack stack = grounded(4, 200e-6, 0.6e-6, 3.5e7);
        QList<double> r;
        for (const double deg : {0.0, 10.0, 30.0, 45.0}) {
            const double a = deg * M_PI / 180, ca = std::cos(a), sa = std::sin(a);
            const auto rot = [&](double x, double y) { return QPointF(x * ca - y * sa, x * sa + y * ca); };
            rf::Conductor c;
            c.layer = QStringLiteral("M1");
            c.outline = QPolygonF(QList<QPointF>{rot(0, -50), rot(2000, -50), rot(2000, 50), rot(0, 50)});
            rf::MeshSettings how;
            how.maximum = 100e-6;
            how.cellsAcross = 6;
            const rf::Mesh mesh = rf::meshOf({c}, how, stack, 10e9);
            const rf::Inductance ind = rf::inductanceOf(mesh, {c}, stack, rot(0, 0), rot(2000, 0), 1e-6, 1e9);
            QVERIFY2(ind.ok(), qPrintable(ind.problems.join(" | ")));
            r << ind.rDc;
        }
        qWarning("a strip's R at direct current at 0, 10, 30 and 45 degrees: %.4f, %.4f, %.4f and %.4f Ohm", r.at(0), r.at(1), r.at(2), r.at(3));
        for (int k = 1; k < r.size(); ++k) QVERIFY2(std::abs(r.at(k) / r.at(0) - 1) < 0.045, qPrintable(QStringLiteral("%1 against %2").arg(r.at(k)).arg(r.at(0))));
    }

    // A loop's end on a corner says which edge the current goes in along:
    // it has no facing to give, and of the
    // two edges it took one as found, nothing said.
    void aLoopsEndOnACornerSaysItsEdge()
    {
        const rf::Stack stack = grounded(4, 200e-6, 0.6e-6, 3.5e7);
        const QList<rf::Conductor> line{strip(0, -50, 2000, 100)};
        rf::MeshSettings how;
        how.maximum = 100e-6;
        how.cellsAcross = 6;
        const rf::Mesh mesh = rf::meshOf(line, how, stack, 10e9);
        const rf::Inductance corner = rf::inductanceOf(mesh, line, stack, QPointF(0, -50), QPointF(2000, 0), 1e-6, 1e9);
        QVERIFY2(corner.ok(), qPrintable(corner.problems.join(" | ")));
        QVERIFY2(corner.lines.join(' ').contains(QStringLiteral("is on a corner, where two edges meet: the current goes in along the edge")),
                 qPrintable(corner.lines.join(" | ")));
        const rf::Inductance middle = rf::inductanceOf(mesh, line, stack, QPointF(0, 0), QPointF(2000, 0), 1e-6, 1e9);
        QVERIFY2(middle.ok() && !middle.lines.join(' ').contains(QStringLiteral("on a corner")), qPrintable(middle.lines.join(" | ")));
        // (The middle of the end of a line more than 500 times as long as it
        //  is wide: on its long sides too by their thousandth, and said to be
        //  on a corner - the review of ff1233ad.)
        const QList<rf::Conductor> thin{strip(0, -5, 6000, 10)};
        rf::MeshSettings along = how;
        along.maximum = 500e-6;
        along.cellsAcross = 2;
        const rf::Inductance far = rf::inductanceOf(rf::meshOf(thin, along, stack, 1e9), thin, stack, QPointF(0, 0), QPointF(6000, 0), 1e-6, 1e9);
        QVERIFY2(far.ok() && !far.lines.join(' ').contains(QStringLiteral("on a corner")), qPrintable(far.lines.join(" | ")));
    }

    // A compressed loop past what a solve may take is refused before it is
    // made, and says what it would have taken: an octagonal spiral of
    // 172,000 runs went on into its near field's LU, 25 GB in 30 s and
    // climbing, until the system ended Qucs-S without a word.
    void aLoopPastTheMemoryItMayTakeIsRefused()
    {
        const rf::Stack stack = grounded(9.8, 100e-6, 1e-6, 5.8e7);
        const QList<rf::Conductor> big{strip(0, 0, 400, 120)};
        const rf::Mesh mesh = meshed(big, 5);
        rf::setLoopMemory(1e6);
        const rf::Inductance refused = rf::inductanceOf(mesh, big, stack, QPointF(0, 60), QPointF(400, 60));
        rf::setLoopMemory(0);
        QVERIFY2(!refused.ok() && refused.problems.join(' ').contains(QStringLiteral("for their L alone, more than the 0.001 GB a solve may take")),
                 qPrintable(refused.problems.join(" | ")));
        const rf::Inductance solved = rf::inductanceOf(mesh, big, stack, QPointF(0, 60), QPointF(400, 60));
        QVERIFY2(solved.ok() && solved.compressed, qPrintable(solved.problems.join(" | ")));
        // Room for L as it is weighed, and not for the near field's factors
        // beside it: refused there, before they are made - where their size
        // is known before they are made, Accelerate's analysis.
        // Room for L, and not for its near field's pairs: refused as it
        // counts them, before the triplets and the matrix are made - a mesh
        // of nanometre slivers beside 15 µm runs had 259 million of them, and
        // the solve went past 60 GB.
        {
            const double rows = solved.branches + solved.nodes - 1;
            rf::setLoopMemory(20e3 * solved.branches + 16.0 * 104 * rows + 1e6);
            const rf::Inductance pairs = rf::inductanceOf(mesh, big, stack, QPointF(0, 60), QPointF(400, 60));
            rf::setLoopMemory(0);
            QVERIFY2(!pairs.ok() && pairs.problems.join(' ').contains(QStringLiteral("its near field")) &&
                         pairs.problems.join(' ').contains(QStringLiteral("more pairs of runs")),
                     qPrintable(pairs.problems.join(" | ")));
        }
        if (qucs_s::fem::luBackend() != qucs_s::fem::Backend::Accelerate) {
            qWarning("The near field factorized by %s here, whose size is not known before: not checked.",
                     qPrintable(qucs_s::fem::backendName(qucs_s::fem::luBackend())));
            return;
        }
        rf::setLoopMemory(20e3 * solved.branches * 1.01);
        const rf::Inductance factors = rf::inductanceOf(mesh, big, stack, QPointF(0, 60), QPointF(400, 60));
        rf::setLoopMemory(0);
        QVERIFY2(!factors.ok() && factors.problems.join(' ').contains(QStringLiteral("the factors of its near field")),
                 qPrintable(factors.problems.join(" | ")));
        qWarning().noquote() << factors.problems.join(" | ");
    }
    // A loop's ends on its conductor's edges:
    // the nearest edge was taken however far, so that an end 4 mm off the
    // layout went in at the nearest face, nothing said. Off every shape it
    // is refused; inside one, on none of its edges, said; on an edge, a
    // thousandth of its length off it, as before.
    void aLoopsEndOffItsConductorIsRefused()
    {
        const rf::Stack stack = grounded(4, 200e-6);
        const rf::Conductor bar = strip(0, 0, 1000, 50);
        const rf::Mesh mesh = meshed({bar}, 25);
        const rf::Inductance on = rf::inductanceOf(mesh, {bar}, stack, QPointF(0, 25), QPointF(1000, 25), 1e-6, 1e9);
        QVERIFY2(on.ok(), qPrintable(on.problems.join(" | ")));
        const rf::Inductance hair = rf::inductanceOf(mesh, {bar}, stack, QPointF(-0.01, 25), QPointF(1000.01, 25), 1e-6, 1e9);
        QVERIFY2(hair.ok() && std::abs(hair.l / on.l - 1) < 1e-12, qPrintable(hair.problems.join(" | ")));
        const rf::Inductance off = rf::inductanceOf(mesh, {bar}, stack, QPointF(0, 25), QPointF(5000, 25), 1e-6, 1e9);
        QVERIFY2(!off.ok() && off.problems.join(' ').contains(QStringLiteral("Its end, at 5000, 25 µm, is on no conductor")),
                 qPrintable(off.problems.join(" | ")));
        const rf::Inductance inside = rf::inductanceOf(mesh, {bar}, stack, QPointF(0, 25), QPointF(500, 25), 1e-6, 1e9);
        QVERIFY2(inside.lines.join(' ').contains(QStringLiteral("Its end, at 500, 25 µm, is inside a conductor, on none of its edges")),
                 qPrintable(inside.lines.join(" | ") + inside.problems.join(" | ")));
    }

};

QTEST_MAIN(TestRf25Inductance)
#include "test_rf25_inductance.moc"
