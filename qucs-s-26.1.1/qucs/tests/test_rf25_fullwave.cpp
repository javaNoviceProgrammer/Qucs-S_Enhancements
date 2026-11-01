/*
 * test_rf25_fullwave.cpp - the full-wave solve (rf25/rf25_fullwave.h): at
 * low frequency the quasi-static extraction of phase B, a line's wave at
 * its own per-metre values and its dispersion against Kirschning and
 * Jansen, the losses of its substrate and its metal, reciprocity and
 * passivity, the current it returns, and what it will not solve.
 *
 * Phase C of docs/proposals/2026-10-23-rf-2p5d-moment-method.md: "full-wave
 * MPIE: ... the dense fill and solve, the currents".
 *
 * The lines are long beside the substrate - twenty heights and more. A
 * port's near field reaches the other port directly, not along the line,
 * and on a line a few heights long that is not small: 635 µm alumina with
 * lines of 2 to 6 mm gave an effective permittivity at 10 GHz twice as
 * dispersed as Kirschning and Jansen, varying with the pair of lengths
 * taken, where on 100 µm with lines of 2 and 3 mm it agrees with them to
 * a part in a thousand.
 */
#include <QtTest>

#include "rf25/rf25_fullwave.h"

#include <Eigen/Dense>

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
/// The speed of light as the solver has it, 1/sqrt(mu0 eps0): CODATA's
/// eps0 with mu0 = 4 pi 1e-7 puts c0 a part in 1e10 off 299792458.
const double kC0 = 1.0 / std::sqrt(kMu0 * rf::kEps0);

/// A stack of a substrate of \a epsr and \a height (m) on a ground plane,
/// with one metal on top of it and air above.
rf::Stack grounded(double epsr, double height, double tand = 0, double thickness = 0, double sigma = 0)
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
    sub.tand = tand;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    metal.thickness = thickness;
    metal.sigma = sigma;
    s.layers << ground << sub << metal;
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

/// Those conductors meshed \a across cells across, \a stretch times as
/// long along a straight run, none longer than \a largest (m).
rf::Mesh meshed(const QList<rf::Conductor>& conductors, int across, double largest, double stretch = 4)
{
    rf::MeshSettings how;
    how.maximum = largest;
    how.cellsAcross = across;
    how.stretch = stretch;
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

using Matrix2 = Eigen::Matrix2cd;

/// Frequency \a k of a two-port's solve as a matrix.
Matrix2 zOf(const rf::FullWave& fw, int k)
{
    Matrix2 z;
    z << fw.z[k][0][0], fw.z[k][0][1], fw.z[k][1][0], fw.z[k][1][1];
    return z;
}

/// Its chain (ABCD) matrix.
Matrix2 chainOf(const Matrix2& z)
{
    Matrix2 t;
    t << z(0, 0) / z(1, 0), z.determinant() / z(1, 0), 1.0 / z(1, 0), z(1, 1) / z(1, 0);
    return t;
}

/*!
 * The two-line method: a line \a l1 long and one \a l2 long (m), the same
 * ends on both, their chain matrices \a t1 and \a t2. The ends cancel in
 * t2 t1^-1, which is the line of the difference in length seen through
 * them, so its trace is 2 cosh(gamma dl) whatever they are: gamma, its
 * phase taken nearest \a beta (1/m) where a turn of 2 pi would do as well.
 *
 * And Z0, taking each end as a series impedance and a shunt admittance
 * (the port's feed and the open end's fringe): the chain matrix's C is
 * then 2 Ys cosh + (1/Z0 + Ys^2 Z0) sinh and its B 2 Zs a cosh + (Zs^2/Z0
 * + a^2 Z0) sinh, a = 1 + Zs Ys - each linear in two unknowns across the
 * two lengths - and the ratio of the two sinh terms is Z0^2 to the second
 * order of the ends.
 */
struct TwoLine {
    Complex gamma, z0;
};
TwoLine twoLine(const Matrix2& t1, const Matrix2& t2, double l1, double l2, double beta)
{
    const double dl = l2 - l1;
    const Complex a = std::acosh((t2 * t1.inverse()).trace() / 2.0);
    Complex g = a / dl;
    double best = -1;
    for (const double sign : {1.0, -1.0})
        for (int n = -8; n <= 8; ++n) {
            const Complex c = (sign * a + Complex(0, 2 * M_PI * n)) / dl;
            if (best < 0 || std::abs(c.imag() - beta) < best) {
                best = std::abs(c.imag() - beta);
                g = c;
            }
        }
    const Complex c1 = std::cosh(g * l1), s1 = std::sinh(g * l1), c2 = std::cosh(g * l2), s2 = std::sinh(g * l2);
    const Complex det = c1 * s2 - c2 * s1;
    const Complex u = (c1 * t2(1, 0) - c2 * t1(1, 0)) / det;   // the sinh term of C
    const Complex v = (c1 * t2(0, 1) - c2 * t1(0, 1)) / det;   // and of B
    return {g, std::sqrt(v / u)};
}

/// Kirschning and Jansen's dispersion of a microstrip (Electron. Lett.
/// 1982): its effective permittivity at \a ghz on a substrate \a mm thick
/// of \a epsr, for a strip \a u = W/h wide whose quasi-static value is
/// \a eps0. They give it as good to 0.6 % for 0.1 <= u <= 100, epsr to 20
/// and f h to 25 GHz mm.
double kirschningJansen(double eps0, double epsr, double u, double ghz, double mm)
{
    const double fn = ghz * mm;
    const double p1 = 0.27488 + (0.6315 + 0.525 / std::pow(1 + 0.0157 * fn, 20)) * u - 0.065683 * std::exp(-8.7513 * u);
    const double p2 = 0.33622 * (1 - std::exp(-0.03442 * epsr));
    const double p3 = 0.0363 * std::exp(-4.6 * u) * (1 - std::exp(-std::pow(fn / 38.7, 4.97)));
    const double p4 = 1 + 2.751 * (1 - std::exp(-std::pow(epsr / 15.916, 8)));
    const double p = p1 * p2 * std::pow((0.1844 + p3 * p4) * fn, 1.5763);
    return epsr - (epsr - eps0) / (1 + p);
}

/// Hammerstad and Jensen's microstrip in vacuum: Z01 of a strip of width
/// \a w over a ground \a h below, good to 0.01 %. L per metre is Z01/c0.
double hammerstadJensenAir(double w, double h)
{
    const double u = w / h;
    const double f = 6 + (2 * M_PI - 6) * std::exp(-std::pow(30.666 / u, 0.7528));
    return kMu0 * kC0 / (2 * M_PI) * std::log(f / u + std::sqrt(1 + 4 / (u * u)));
}

/// A line \a length µm long and \a width wide, meshed as meshed() says,
/// ported at both ends and solved at each of \a frequencies; and, given
/// \a quasiStatic, phase B's extraction of the same mesh.
rf::FullWave line(const rf::Stack& stack, double length, double width, int across, double largest, double stretch,
                  const QList<double>& frequencies, rf::Extraction* quasiStatic = nullptr)
{
    const QList<rf::Conductor> conductors{strip(0, 0, length, width)};
    const rf::Mesh mesh = meshed(conductors, across, largest, stretch);
    if (quasiStatic != nullptr) *quasiStatic = rf::extract(mesh, conductors, stack);
    return rf::fullWaveOf(mesh, conductors, stack, {QPointF(0, width / 2), QPointF(length, width / 2)}, frequencies);
}

/// What a line's two lengths say at a low frequency \a k: the shunt
/// capacitance of all of it (Im sum Y / w) and the series inductance and
/// resistance from one port to the other (-1/Y12), of each; and per metre,
/// by their difference, where the ends cancel.
struct LowFrequency {
    double c1 = 0, c2 = 0, cPer = 0, lPer = 0, rPer = 0;
};
LowFrequency lowFrequency(const rf::FullWave& a, const rf::FullWave& b, int k, double l1, double l2)
{
    const double w = 2 * M_PI * a.frequencies.at(k);
    const Matrix2 ya = zOf(a, k).inverse(), yb = zOf(b, k).inverse();
    LowFrequency out;
    out.c1 = ya.sum().imag() / w;
    out.c2 = yb.sum().imag() / w;
    out.cPer = (out.c2 - out.c1) / (l2 - l1);
    out.lPer = ((-1.0 / yb(0, 1)).imag() - (-1.0 / ya(0, 1)).imag()) / w / (l2 - l1);
    out.rPer = ((-1.0 / yb(0, 1)).real() - (-1.0 / ya(0, 1)).real()) / (l2 - l1);
    return out;
}

/*!
 * The capacitance of a mesh's cells on a stack of one metal, every pair of
 * cells in closed form: the static Green's function's sixteen strongest
 * images each integrated over the charged cell's rectangle, the rest -
 * three millimetres and more away - at its centre. Phase B's rule for near
 * cells taken to every distance, where phase B itself takes a cell four of
 * its longest sides away as a point charge.
 */
double exactCapacitance(const rf::Mesh& mesh, const rf::Stack& stack)
{
    const rf::Layers layers = rf::layersOf(stack);
    const rf::Green g(layers, layers.height());
    const QList<rf::Green::Image> terms = g.strongestTerms(16);
    const int n = int(mesh.cells.size());
    Eigen::MatrixXd p(n, n);
    for (int i = 0; i < n; ++i) {
        const QPointF at = mesh.cells.at(i).centre * 1e-6;
        for (int j = 0; j < n; ++j) {
            const QRectF b = mesh.cells.at(j).outline.boundingRect();
            const QRectF rect(b.left() * 1e-6, b.top() * 1e-6, b.width() * 1e-6, b.height() * 1e-6);
            double sum = 0;
            for (const rf::Green::Image& t : terms) sum += t.a * rf::rectanglePotential(rect, at, t.z);
            const double rho = std::hypot(at.x() - rect.center().x(), at.y() - rect.center().y());
            p(i, j) = sum / (4 * M_PI * g.epsilon()) + g.beyondStrongest(rho, int(terms.size()));
        }
    }
    // Made symmetric, as the solve makes its own: a potential at a centre
    // of a charge over a cell is not quite the other way round where two
    // cells differ in size, and here that alone is 1.2e-4 of the answer.
    const Eigen::MatrixXd symmetric = 0.5 * (p + p.transpose());
    return symmetric.partialPivLu().solve(Eigen::VectorXd::Ones(n)).sum();
}

bool says(const QStringList& lines, const char* words)
{
    for (const QString& l : lines)
        if (l.contains(QLatin1String(words), Qt::CaseInsensitive)) return true;
    return false;
}

QString why(const rf::FullWave& fw)
{
    return fw.problems.join(QStringLiteral(" | "));
}

} // namespace

class TestRf25Fullwave : public QObject
{
    Q_OBJECT

private slots:
    // The sheet impedance between its two limits, and S from Z where both
    // are known in closed form.
    void theSheetAndTheScatteringMatrixAreTheirClosedForms()
    {
        const double sigma = 5.8e7, t = 2e-6;
        // At direct current, the whole thickness: 1/(sigma t).
        const Complex dc = rf::sheetImpedance(sigma, t, 1e3);
        QVERIFY2(std::abs(dc.real() * sigma * t - 1) < 1e-6 && std::abs(dc.imag()) < 1e-6 * dc.real(),
                 qPrintable(QStringLiteral("%1 %2j against %3").arg(dc.real()).arg(dc.imag()).arg(1 / (sigma * t))));
        // Many skin depths thick, each face carrying half: (1 + j)/(2 sigma delta).
        const double f = 1e12, delta = 1 / std::sqrt(M_PI * f * kMu0 * sigma);
        const Complex thick = rf::sheetImpedance(sigma, 100 * delta, f);
        const Complex want = Complex(1, 1) / (2 * sigma * delta);
        QVERIFY2(std::abs(thick - want) < 1e-9 * std::abs(want),
                 qPrintable(QStringLiteral("%1 %2j against %3 %4j").arg(thick.real()).arg(thick.imag()).arg(want.real()).arg(want.imag())));
        // Perfect where it says no conductivity or no thickness.
        QCOMPARE(rf::sheetImpedance(0, t, 1e9), Complex(0, 0));
        QCOMPARE(rf::sheetImpedance(sigma, 0, 1e9), Complex(0, 0));

        // One port: (Z - Z0)/(Z + Z0).
        const Complex z(30, 40);
        const QList<QList<Complex>> one = rf::scatteringOf({{z}}, 50);
        QVERIFY(std::abs(one[0][0] - (z - 50.0) / (z + 50.0)) < 1e-15);
        // A shunt resistor R across a line: S11 = -Z0/(2R + Z0), S21 = 2R/(2R + Z0).
        const double r = 50;
        const QList<QList<Complex>> shunt = rf::scatteringOf({{r, r}, {r, r}}, 50);
        QVERIFY(std::abs(shunt[0][0] + 1.0 / 3) < 1e-15 && std::abs(shunt[1][1] + 1.0 / 3) < 1e-15);
        QVERIFY(std::abs(shunt[0][1] - 2.0 / 3) < 1e-15 && std::abs(shunt[1][0] - 2.0 / 3) < 1e-15);
    }

    // At low frequency the full-wave solve is the quasi-static one: the
    // charge it leaves on a line is phase B's capacitance, and the current
    // between its ports the inductance of its partial inductances.
    void atLowFrequencyItIsTheQuasiStaticExtraction()
    {
        const double h = 100e-6, w = 100;
        const rf::Stack stack = grounded(9.8, h);
        rf::Extraction e1, e2;
        const rf::FullWave a = line(stack, 2000, w, 4, 100e-6, 4, {1e8}, &e1);
        const rf::FullWave b = line(stack, 3000, w, 4, 100e-6, 4, {1e8}, &e2);
        QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
        QVERIFY2(e1.ok() && e2.ok(), qPrintable((e1.problems + e2.problems).join(" | ")));
        QCOMPARE(a.frequencies, QList<double>{1e8});
        const LowFrequency lf = lowFrequency(a, b, 0, 2000e-6, 3000e-6);
        const rf::LineParameters lp = rf::lineParameters(e2, 0, 3000e-6);
        const double lHj = hammerstadJensenAir(w * 1e-6, h) / kC0;
        qWarning("C %.6g pF vs %.6g (%.2e), %.6g vs %.6g (%.2e); C' %.6g pF/m vs %.6g (%.2e); L' %.6g nH/m vs %.6g (%.2e), "
                 "H&J %.6g (%.2e)",
                 lf.c1 * 1e12, e1.selfCapacitance(0) * 1e12, lf.c1 / e1.selfCapacitance(0) - 1, lf.c2 * 1e12,
                 e2.selfCapacitance(0) * 1e12, lf.c2 / e2.selfCapacitance(0) - 1, lf.cPer * 1e12, lp.c * 1e12, lf.cPer / lp.c - 1,
                 lf.lPer * 1e9, lp.l * 1e9, lf.lPer / lp.l - 1, lHj * 1e9, lf.lPer / lHj - 1);
        // The charge: that of the same cells with every pair in closed
        // form, to 2e-4 - the retarded kernels' static limit, integrated as
        // this integrates them. (Measured 7.6e-5, of which some 4e-5 is
        // the line's own inductance at 100 MHz, w^2 L C / 3.)
        const double exact = exactCapacitance(meshed({strip(0, 0, 2000, w)}, 4, 100e-6), stack);
        qWarning("C %.6g pF, every pair in closed form %.6g (%.2e)", lf.c1 * 1e12, exact * 1e12, lf.c1 / exact - 1);
        QVERIFY2(std::abs(lf.c1 / exact - 1) < 2e-4,
                 qPrintable(QStringLiteral("C %1 pF, every pair in closed form %2").arg(lf.c1 * 1e12).arg(exact * 1e12)));
        // And phase B's to 5e-4, which takes a cell four of its longest
        // sides away as a point charge (measured 1.6e-5 and 4.4e-5).
        QVERIFY2(std::abs(lf.c1 / e1.selfCapacitance(0) - 1) < 5e-4 && std::abs(lf.c2 / e2.selfCapacitance(0) - 1) < 5e-4,
                 qPrintable(QStringLiteral("C %1 and %2 pF, phase B %3 and %4")
                                .arg(lf.c1 * 1e12).arg(lf.c2 * 1e12).arg(e1.selfCapacitance(0) * 1e12).arg(e2.selfCapacitance(0) * 1e12)));
        // And per metre phase B's of its cross-section as if the line ran
        // on for ever (measured 6e-5).
        QVERIFY2(std::abs(lf.cPer / lp.c - 1) < 5e-4,
                 qPrintable(QStringLiteral("C' %1 pF/m, phase B %2").arg(lf.cPer * 1e12).arg(lp.c * 1e12)));
        // The inductance per metre, of partial inductances where phase B
        // has 1/(c0^2 C0) of the charge: the true value (Hammerstad and
        // Jensen's, good to 0.01 %) and phase B's each within 1.5 % - the
        // two discretizations differ, and on four cells across neither is
        // nearer than a per cent.
        QVERIFY2(std::abs(lf.lPer / lHj - 1) < 0.015,
                 qPrintable(QStringLiteral("L' %1 nH/m, Hammerstad and Jensen %2").arg(lf.lPer * 1e9).arg(lHj * 1e9)));
        QVERIFY2(std::abs(lf.lPer / lp.l - 1) < 0.015,
                 qPrintable(QStringLiteral("L' %1 nH/m, phase B %2").arg(lf.lPer * 1e9).arg(lp.l * 1e9)));
        // Reciprocal, to rounding: P and L are symmetric by construction.
        const Matrix2 z = zOf(b, 0);
        QVERIFY2(std::abs(z(0, 1) - z(1, 0)) < 1e-12 * std::abs(z(0, 1)),
                 qPrintable(QStringLiteral("Z12 %1, Z21 %2").arg(z(0, 1).imag()).arg(z(1, 0).imag())));
        // Lossless and below any radiation worth the name: capacitive, its
        // real part a part in 1e9 of its reactance.
        QVERIFY2(z(0, 0).imag() < 0 && std::abs(z(0, 0).real()) < 1e-9 * std::abs(z(0, 0).imag()),
                 qPrintable(QStringLiteral("Z11 %1 %2j").arg(z(0, 0).real()).arg(z(0, 0).imag())));
        QVERIFY(says(a.lines, "perfect"));
        QCOMPARE(a.portsSaid.size(), 2);
    }

    // Above it, the line's wave: at 10 GHz its effective permittivity is
    // its own quasi-static one as Kirschning and Jansen disperse it, its
    // Z0 its own sqrt(L/C), and by 40 GHz it has dispersed as they say.
    void aLinesWaveIsItsOwnLineDispersed()
    {
        const double h = 100e-6, w = 100, epsr = 9.8;
        const rf::Stack stack = grounded(epsr, h);
        const QList<double> fs{1e8, 10e9, 40e9};
        const rf::FullWave a = line(stack, 2000, w, 4, 150e-6, 6, fs);
        const rf::FullWave b = line(stack, 3000, w, 4, 150e-6, 6, fs);
        QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
        const LowFrequency lf = lowFrequency(a, b, 0, 2000e-6, 3000e-6);
        const double eps0 = kC0 * kC0 * lf.lPer * lf.cPer, z0 = std::sqrt(lf.lPer / lf.cPer);
        for (int k = 1; k < fs.size(); ++k) {
            const double k0 = 2 * M_PI * fs.at(k) / kC0;
            const double kj = kirschningJansen(eps0, epsr, w * 1e-6 / h, fs.at(k) / 1e9, h * 1e3);
            const TwoLine wave = twoLine(chainOf(zOf(a, k)), chainOf(zOf(b, k)), 2000e-6, 3000e-6, k0 * std::sqrt(kj));
            const double eps = std::pow(wave.gamma.imag() / k0, 2);
            qWarning("%g GHz: eps_eff %.5f (quasi-static %.5f), Kirschning and Jansen %.5f; rise %.4f vs %.4f; alpha %.3g Np/m "
                     "(beta %.4g); Z0 %.3f %+.3fj vs %.3f",
                     fs.at(k) / 1e9, eps, eps0, kj, eps - eps0, kj - eps0, wave.gamma.real(), wave.gamma.imag(),
                     wave.z0.real(), wave.z0.imag(), z0);
            if (k == 1) {
                // 10 GHz: what is left is the two ports' own coupling, a
                // part in a thousand of the phase.
                QVERIFY2(std::abs(eps / kj - 1) < 2e-3, qPrintable(QStringLiteral("eps_eff %1, K&J %2").arg(eps).arg(kj)));
                QVERIFY2(std::abs(wave.gamma.real()) < 1e-3 * wave.gamma.imag(),
                         qPrintable(QStringLiteral("alpha %1 of a lossless line").arg(wave.gamma.real())));
                QVERIFY2(std::abs(wave.z0 - z0) < 0.01 * z0,
                         qPrintable(QStringLiteral("Z0 %1 %2j, sqrt(L'/C') %3").arg(wave.z0.real()).arg(wave.z0.imag()).arg(z0)));
            } else {
                // 40 GHz: within their 0.6 % and a little more, and risen
                // by as much as they say within a third - a quasi-static
                // solve would not have risen at all.
                QVERIFY2(std::abs(eps / kj - 1) < 0.01, qPrintable(QStringLiteral("eps_eff %1, K&J %2").arg(eps).arg(kj)));
                QVERIFY2(eps - eps0 > 0.7 * (kj - eps0) && eps - eps0 < 1.35 * (kj - eps0),
                         qPrintable(QStringLiteral("risen %1, K&J %2").arg(eps - eps0).arg(kj - eps0)));
            }
        }
    }

    // The losses: a metal's resistance at low frequency is its sheet's,
    // and at 2 GHz a lossy substrate and a lossy metal attenuate the line
    // as phase B's G and the sheet's resistance where the charge crowds say.
    void aSubstratesAndAMetalsLossesAreTheirs()
    {
        const double h = 100e-6, w = 100, sigma = 5.8e7, t = 1e-6;
        const rf::Stack lossy = grounded(9.8, h, 0.01, t, sigma);
        rf::Extraction e;
        const QList<double> fs{1e7, 2e9};
        const rf::FullWave a = line(lossy, 1000, w, 4, 100e-6, 4, fs, &e);
        const rf::FullWave b = line(lossy, 2000, w, 4, 100e-6, 4, fs);
        QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
        const LowFrequency lf = lowFrequency(a, b, 0, 1000e-6, 2000e-6);
        const double rDc = 1 / (sigma * w * 1e-6 * t);
        qWarning("R' at 10 MHz %.5g Ohm/m vs 1/(sigma W t) %.5g (%.2e)", lf.rPer, rDc, lf.rPer / rDc - 1);
        // 10 MHz, where the resistance still divides the current: 1/(sigma W t).
        QVERIFY2(std::abs(lf.rPer / rDc - 1) < 5e-3, qPrintable(QStringLiteral("R' %1 Ohm/m, %2").arg(lf.rPer).arg(rDc)));

        const rf::LineParameters lp = rf::lineParameters(e, 0, 1000e-6, 2e9);
        const double k0 = 2 * M_PI * 2e9 / kC0;
        const TwoLine wave = twoLine(chainOf(zOf(a, 1)), chainOf(zOf(b, 1)), 1000e-6, 2000e-6, k0 * std::sqrt(lp.epsilonEff));
        const double alphaD = lp.g * lp.z0 / 2;
        const double rSheet = rf::sheetImpedance(sigma, t, 2e9).real() * e.perMetre.crowding.value(0);
        const double alphaC = rSheet / (2 * lp.z0);
        qWarning("2 GHz: alpha %.5g Np/m; phase B's G %.4g S/m, alpha_d %.5g; a sheet of R %.4g Ohm/m where the charge crowds, "
                 "alpha_c %.5g; together %.5g (%.3f)",
                 wave.gamma.real(), lp.g, alphaD, rSheet, alphaC, alphaD + alphaC, wave.gamma.real() / (alphaD + alphaC) - 1);
        // (Measured 2.7 %: the current where the charge crowds is two
        //  discretizations of one edge singularity, the solve's runs and
        //  phase B's charges, and the crowding factor is all edge.)
        QVERIFY2(std::abs(wave.gamma.real() / (alphaD + alphaC) - 1) < 0.05,
                 qPrintable(QStringLiteral("alpha %1, %2 + %3").arg(wave.gamma.real()).arg(alphaD).arg(alphaC)));

        // The substrate's loss alone: phase B's G.
        const rf::Stack dielectric = grounded(9.8, h, 0.01);
        rf::Extraction ed;
        const rf::FullWave c = line(dielectric, 1000, w, 4, 100e-6, 4, {2e9}, &ed);
        const rf::FullWave d = line(dielectric, 2000, w, 4, 100e-6, 4, {2e9});
        QVERIFY2(c.ok() && d.ok(), qPrintable(why(c) + why(d)));
        const rf::LineParameters lpd = rf::lineParameters(ed, 0, 1000e-6, 2e9);
        const TwoLine dwave = twoLine(chainOf(zOf(c, 0)), chainOf(zOf(d, 0)), 1000e-6, 2000e-6, k0 * std::sqrt(lpd.epsilonEff));
        qWarning("tan delta alone at 2 GHz: alpha %.5g Np/m, phase B %.5g (%.3f)", dwave.gamma.real(), lpd.gamma.real(),
                 dwave.gamma.real() / lpd.gamma.real() - 1);
        QVERIFY2(std::abs(dwave.gamma.real() / lpd.gamma.real() - 1) < 0.02,
                 qPrintable(QStringLiteral("alpha %1, phase B %2").arg(dwave.gamma.real()).arg(lpd.gamma.real())));
    }

    // Lines on two metals, one over the other: every pair of heights its
    // own Green's functions, and at low frequency the two nets' Maxwell
    // matrix phase B's.
    void linesOnTwoMetalsAreSolvedTogether()
    {
        rf::Stack two;
        const auto layer = [](rf::Layer::Kind kind, const QString& name, double thickness, double er) {
            rf::Layer l;
            l.kind = kind;
            l.name = name;
            l.tag = name.toLower();
            l.thickness = thickness;
            l.epsr = er;
            return l;
        };
        two.layers << layer(rf::Layer::Ground, QStringLiteral("GND"), 0, 1) << layer(rf::Layer::Substrate, QStringLiteral("SUB"), 100e-6, 9.8)
                   << layer(rf::Layer::Metal, QStringLiteral("M1"), 0, 1) << layer(rf::Layer::Dielectric, QStringLiteral("ILD"), 10e-6, 4)
                   << layer(rf::Layer::Metal, QStringLiteral("M2"), 0, 1);
        rf::Conductor upper = strip(0, 50, 600, 100);
        upper.layer = QStringLiteral("M2");
        const QList<rf::Conductor> conductors{strip(0, 0, 600, 100), upper};
        const rf::Mesh mesh = meshed(conductors, 4, 50e-6);
        const rf::Extraction e = rf::extract(mesh, conductors, two);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        QCOMPARE(e.names.size(), 2);
        // Each line's two ends, where only its own edge is.
        const QList<QPointF> ports{QPointF(0, 25), QPointF(600, 25), QPointF(0, 125), QPointF(600, 125)};
        const rf::FullWave fw = rf::fullWaveOf(mesh, conductors, two, ports, {1e8});
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        QVERIFY(says(fw.lines, "2 metal height"));
        Eigen::Matrix4cd z;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) z(i, j) = fw.z[0][i][j];
        const Eigen::Matrix4cd y = z.inverse();
        const double w = 2 * M_PI * 1e8;
        // Each net's ports at one voltage: its block of Y summed.
        const int lower = e.names.at(0).contains(QStringLiteral("M1")) ? 0 : 1;
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b) {
                const double c = y.block(2 * a, 2 * b, 2, 2).sum().imag() / w;
                const int i = a == 0 ? lower : 1 - lower, j = b == 0 ? lower : 1 - lower;
                const double want = e.maxwell.at(i).at(j);
                qWarning("C[%d][%d] %.6g pF, phase B %.6g (%.2e)", a, b, c * 1e12, want * 1e12, c / want - 1);
                // To 2.5e-3 (measured 6.8e-4 to 1.44e-3): phase B leaves its
                // potential matrix as it is, where this solve makes it
                // symmetric, and on two metals whose cells do not line up
                // that alone is a part in a thousand - made symmetric, phase
                // B met this to 5e-4. A Green's function of the wrong pair
                // of heights is tens of per cent.
                QVERIFY2(std::abs(c / want - 1) < 2.5e-3,
                         qPrintable(QStringLiteral("C[%1][%2] %3 pF, phase B %4").arg(a).arg(b).arg(c * 1e12).arg(want * 1e12)));
            }
    }

    // A junction of three lines: reciprocal whatever it is made of, and
    // passive - lossy, it gives back less than it is given; lossless, all
    // of it but what it radiates.
    void aJunctionIsReciprocalAndPassive()
    {
        rf::Conductor tee;
        tee.layer = QStringLiteral("M1");
        tee.outline = QPolygonF({QPointF(0, 0), QPointF(1000, 0), QPointF(1000, 100), QPointF(550, 100), QPointF(550, 600),
                                 QPointF(450, 600), QPointF(450, 100), QPointF(0, 100)});
        const QList<rf::Conductor> conductors{tee};
        const rf::Mesh mesh = meshed(conductors, 3, 100e-6);
        const QList<QPointF> ports{QPointF(0, 50), QPointF(1000, 50), QPointF(500, 600)};
        for (const bool lossy : {true, false}) {
            const rf::Stack stack = lossy ? grounded(9.8, 100e-6, 0.02, 2e-6, 5.8e7) : grounded(9.8, 100e-6);
            const rf::FullWave fw = rf::fullWaveOf(mesh, conductors, stack, ports, {12e9});
            QVERIFY2(fw.ok(), qPrintable(why(fw)));
            QCOMPARE(fw.z.size(), 1);
            QCOMPARE(fw.z.first().size(), 3);
            Eigen::Matrix3cd z, s;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    z(i, j) = fw.z[0][i][j];
                    s(i, j) = fw.s[0][i][j];
                }
            const double asymmetry = (z - z.transpose()).norm() / z.norm();
            // What it gives back of a wave in at each port: the least
            // eigenvalue of I - S^H S is the least it keeps.
            const Eigen::Matrix3cd kept = Eigen::Matrix3cd::Identity() - s.adjoint() * s;
            const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3cd> eig(kept);
            const double least = eig.eigenvalues().minCoeff(), most = eig.eigenvalues().maxCoeff();
            qWarning("%s: %d runs, asymmetry %.2e, kept %.3g to %.3g; |S11| %.4f |S21| %.4f |S31| %.4f", lossy ? "lossy" : "lossless",
                     fw.runs, asymmetry, least, most, std::abs(s(0, 0)), std::abs(s(1, 0)), std::abs(s(2, 0)));
            QVERIFY2(asymmetry < 1e-12, qPrintable(QStringLiteral("Z asymmetric by %1").arg(asymmetry)));
            QVERIFY2(least > -1e-9, qPrintable(QStringLiteral("gives back more than it is given: %1").arg(least)));
            if (lossy) QVERIFY2(least > 1e-3, qPrintable(QStringLiteral("a lossy junction keeps %1").arg(least)));
            else QVERIFY2(most < 0.02, qPrintable(QStringLiteral("a lossless junction keeps %1").arg(most)));
        }
    }

    // The current it returns: of the first port at the last frequency,
    // one per run. At low frequency a line's current is what charges the
    // line beyond: half of it at the middle, by symmetry, and all of it at
    // the near end.
    void theCurrentIsWhatChargesTheLineBeyond()
    {
        const double length = 600, w = 100;
        const rf::FullWave fw = line(grounded(9.8, 100e-6), length, w, 4, 50e-6, 4, {1e8});
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        QCOMPARE(fw.current.size(), fw.runs);
        QCOMPARE(fw.geometry.size(), fw.runs);
        // The current across a line x = at (µm): the runs along x that cross it.
        const auto across = [&fw](double at) {
            Complex sum(0, 0);
            for (int r = 0; r < fw.geometry.size(); ++r) {
                const rf::Filament& f = fw.geometry.at(r);
                const double x0 = std::min(f.from.x(), f.to.x()) * 1e6, x1 = std::max(f.from.x(), f.to.x()) * 1e6;
                if (x0 < at && x1 > at) sum += fw.current.at(r) * (f.to.x() > f.from.x() ? 1.0 : -1.0);
            }
            return sum;
        };
        // (Between the first two columns of cells' centres, 12.5 and 37.5 µm
        //  in from either end.)
        const Complex middle = across(length / 2), nearEnd = across(25), farEnd = across(length - 25);
        qWarning("current at the middle %.6f %+.2ej, near the driven end %.4f, near the open one %.4f", middle.real(), middle.imag(),
                 nearEnd.real(), farEnd.real());
        QVERIFY2(std::abs(middle - 0.5) < 1e-4, qPrintable(QStringLiteral("%1 %2j at the middle").arg(middle.real()).arg(middle.imag())));
        QVERIFY2(nearEnd.real() > 0.9 && nearEnd.real() <= 1 + 1e-9, qPrintable(QStringLiteral("%1 near the driven end").arg(nearEnd.real())));
        QVERIFY2(std::abs(farEnd) < 0.1, qPrintable(QStringLiteral("%1 near the open end").arg(farEnd.real())));
    }

    void whatItCannotSolveIsSaid()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> one{strip(0, 0, 400, 100)};
        const rf::Mesh mesh = meshed(one, 4, 50e-6);
        const QList<QPointF> ports{QPointF(0, 50), QPointF(400, 50)};
        const auto refused = [](const rf::FullWave& fw, const char* words) {
            return !fw.ok() && fw.z.isEmpty() && says(fw.problems, words);
        };
        QVERIFY(rf::fullWaveOf(mesh, one, stack, ports, {1e9}).ok());
        // No mesh, no port, no frequency, a frequency of nothing, none at all, a reference of nothing.
        QVERIFY(refused(rf::fullWaveOf(rf::Mesh(), one, stack, ports, {1e9}), "mesh the layout"));
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, {}, {1e9}), "port"));
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, ports, {}), "No frequency"));
        // (In its own words, before anything is solved: the retarded Green's
        //  functions refuse it too, but only once the static parts are filled
        //  and every frequency before it solved.)
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, ports, {1e9, 0}), "the quasi-static extraction answers"));
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, ports, {-1e9}), "the quasi-static extraction answers"));
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, ports, {std::nan("")}), "the quasi-static extraction answers"));
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, ports, {1e9}, 0), "reference"));
        // No ground plane to refer the ports to.
        rf::Stack open = stack;
        open.layers[0].variant = QStringLiteral("open");
        QVERIFY(refused(rf::fullWaveOf(mesh, one, open, ports, {1e9}), "ground"));
        // Two ports on the same face.
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, {QPointF(0, 20), QPointF(0, 80)}, {1e9}), "same face"));
        // A port off every edge drives the face nearest it, as the
        // inductance's terminals do, and says which.
        const rf::FullWave inside = rf::fullWaveOf(mesh, one, stack, {QPointF(200, 70)}, {1e9});
        QVERIFY2(inside.ok(), qPrintable(why(inside)));
        QVERIFY2(says(inside.portsSaid, "along the face"), qPrintable(inside.portsSaid.join(" | ")));
        // A via, whose current runs up and down.
        rf::Stack two;
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
        two.layers << layer(rf::Layer::Ground, QStringLiteral("GND"), 0, 1) << layer(rf::Layer::Substrate, QStringLiteral("SUB"), 100e-6, 9.8)
                   << layer(rf::Layer::Metal, QStringLiteral("M1"), 0, 1) << layer(rf::Layer::Dielectric, QStringLiteral("ILD"), 10e-6, 4)
                   << via << layer(rf::Layer::Metal, QStringLiteral("M2"), 0, 1);
        rf::Conductor upper = strip(300, 0, 400, 100), joint = strip(300, 0, 100, 100);
        upper.layer = QStringLiteral("M2");
        joint.layer = QStringLiteral("V1");
        const QList<rf::Conductor> stacked{strip(0, 0, 400, 100), upper, joint};
        QVERIFY(refused(rf::fullWaveOf(meshed(stacked, 4, 50e-6), stacked, two, ports, {1e9}), "via"));
        // More than a dense complex matrix of this build holds.
        const QList<rf::Conductor> plate{strip(0, 0, 1000, 1000)};
        rf::MeshSettings fine;
        fine.maximum = 20e-6;
        fine.cellsAcross = 0;
        fine.edges = false;
        QVERIFY(refused(rf::fullWaveOf(rf::meshOf(plate, fine, rf::Stack(), 0), plate, stack, {QPointF(0, 500)}, {1e9}),
                        "more than this build"));
    }
};

QTEST_MAIN(TestRf25Fullwave)
#include "test_rf25_fullwave.moc"
