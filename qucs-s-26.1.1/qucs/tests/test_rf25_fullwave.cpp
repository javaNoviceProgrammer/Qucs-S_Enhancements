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
#include <QRegularExpression>

#include "rf25/rf25_fullwave.h"

#include <Eigen/Dense>

#include <atomic>
#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
/// The speed of light as the solver has it, 1/sqrt(mu0 eps0): CODATA's
/// eps0 with mu0 = 4 pi 1e-7 puts c0 a part in 1e10 off 299792458.
const double kC0 = 1.0 / std::sqrt(kMu0 * rf::kEps0);

/// Whether this build is one whose dense solves are many times slower - a
/// sanitized one, as CI's Linux job is (QUCS_TEST_SLOW: its path in any).
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

/// A ground plane, 100 µm of 9.8 with M1 on it, and 10 µm of 4 with M2 on
/// that: two metals with no via between them.
rf::Stack twoMetals()
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
    return two;
}

/// How many cells a port said it drives along its face: "drives the N cell(s)".
int cellsDriven(const QString& said)
{
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("drives the (\\d+) cell")).match(said);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

/// A port of the model's kinds.
rf::FullWavePort edgePort(QPointF at, const QString& metal = QString(), QPointF facing = QPointF(), double impedance = 50)
{
    rf::FullWavePort p;
    p.at = at;
    p.metal = metal;
    p.facing = facing;
    p.impedance = impedance;
    return p;
}

rf::FullWavePort internalPort(QPointF at, const QString& metal, const QString& from = QString())
{
    rf::FullWavePort p;
    p.internal = true;
    p.at = at;
    p.metal = metal;
    p.from = from;
    return p;
}

/// \a stack with a via \a name from \a from to \a to, perfect.
rf::Stack withVia(rf::Stack stack, const QString& name, const QString& from, const QString& to)
{
    rf::Layer via;
    via.kind = rf::Layer::Via;
    via.name = name;
    via.tag = name.toLower();
    via.from = from;
    via.to = to;
    stack.layers << via;
    return stack;
}

/// A rectangle in micrometres on \a layer.
rf::Conductor on(const QString& layer, double x, double y, double w, double h)
{
    rf::Conductor c = strip(x, y, w, h);
    c.layer = layer;
    return c;
}

/// Those conductors meshed \a across cells across with the edges not
/// refined, cells \a stretch times as long along a strip, none longer than
/// \a largest (m): cells that land where a test puts them.
rf::Mesh evenly(const QList<rf::Conductor>& conductors, int across, double largest, double stretch = 4)
{
    rf::MeshSettings how;
    how.maximum = largest;
    how.cellsAcross = across;
    how.stretch = stretch;
    how.edges = false;
    return rf::meshOf(conductors, how, rf::Stack(), 0);
}

/// The admittance matrix of frequency \a k of a solve with its ports past
/// the first \a kept shorted: those ports' block of Z inverted.
Eigen::MatrixXcd keptAdmittance(const rf::FullWave& fw, int k, int kept)
{
    const int n = int(fw.z.at(k).size());
    Eigen::MatrixXcd z(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) z(i, j) = fw.z.at(k).at(i).at(j);
    return Eigen::MatrixXcd(z.inverse()).topLeftCorner(kept, kept);
}

/// Int_a0^a1 Int_b0^b1 dz dz' / sqrt(rho^2 + (z - z')^2): two lines along z,
/// in closed form - a via's partial inductance over mu0/(4 pi).
double linePair(double a0, double a1, double b0, double b1, double rho)
{
    const auto f = [rho](double s) { return s * std::asinh(s / rho) - std::sqrt(s * s + rho * rho); };
    return f(a1 - b0) - f(a1 - b1) - f(a0 - b0) + f(a0 - b1);
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
    // The recheck of 1 November (afternoon): a copper coplanar line lost about
    // half of what QFEM's thick copper does (39-45 against 77 dB/m at 60
    // GHz), and more as the mesh was refined - the sheet's edge cells carry
    // the current crowding to the edge as their average, and lose as that
    // average. Those cells lose as the crowding the metal's thickness stops
    // (Holloway and Kuester's stopping distance): a copper microstrip three
    // micrometres thick at 10 GHz, five skin depths, loses as phase B's
    // Wheeler R of its thick cross-section says, at four cells across and
    // at eight alike; and near what a 2D solve of the current through the
    // metal's own cross-section gives, 219.0 Ohm/m. (Measured +12 % and +7
    // % of Wheeler's, +16 % and +10 % of the 2D solve's; the sheet as it
    // was, 33 % under.)
    void aMetalsLossIsItsThickEdgesWhereTheCurrentCrowds()
    {
        const double h = 100e-6, w = 100;
        const rf::Stack copper = grounded(9.8, h, 0, 3e-6, 5.8e7);
        rf::Extraction e;
        const double f = 10e9;
        QList<double> alpha;
        for (const int across : {4, 8}) {
            const rf::FullWave a = line(copper, 1000, w, across, 100e-6, 4, {f}, across == 4 ? &e : nullptr);
            const rf::FullWave b = line(copper, 2000, w, across, 100e-6, 4, {f});
            QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
            // The runs along the strip's edges alone, said: its two outer
            // rows and the rows across its ends - some a quarter of them at
            // four across, neither none nor all.
            const QRegularExpressionMatch edges =
                QRegularExpression(QStringLiteral("where the current crowds as close as the thickness lets it \\((\\d+) run\\(s\\) along an edge\\)"))
                    .match(a.lines.join(' '));
            QVERIFY2(edges.hasMatch(), qPrintable(a.lines.join(" | ")));
            const double share = edges.captured(1).toDouble() / a.runs;
            QVERIFY2(share > 0.1 && share < 0.4, qPrintable(QStringLiteral("%1 of %2 runs along an edge").arg(edges.captured(1)).arg(a.runs)));
            const rf::LineParameters lp = rf::lineParameters(e, 0, 1000e-6, f);
            const TwoLine wave = twoLine(chainOf(zOf(a, 0)), chainOf(zOf(b, 0)), 1000e-6, 2000e-6,
                                         2 * M_PI * f / kC0 * std::sqrt(lp.epsilonEff));
            alpha << wave.gamma.real();
        }
        const rf::LineParameters lp = rf::lineParameters(e, 0, 1000e-6, f);
        QVERIFY2(lp.problems.isEmpty() && lp.rDc > 0, qPrintable(lp.problems.join(" | ")));
        const double wheeler = lp.r / (2 * lp.z0), eddy = 219.0 / (2 * lp.z0);
        qWarning("10 GHz, 3 um of copper: alpha %.4g Np/m at 4 across, %.4g at 8; Wheeler's R %.4g Ohm/m, alpha %.4g (%.3f, %.3f); "
                 "the 2D solve's 219 Ohm/m (%.3f, %.3f)",
                 alpha.at(0), alpha.at(1), lp.r, wheeler, alpha.at(0) / wheeler - 1, alpha.at(1) / wheeler - 1,
                 alpha.at(0) / eddy - 1, alpha.at(1) / eddy - 1);
        for (const double a : std::as_const(alpha))
            QVERIFY2(std::abs(a / wheeler - 1) < 0.15 && std::abs(a / eddy - 1) < 0.2,
                     qPrintable(QStringLiteral("alpha %1 Np/m, Wheeler %2, the 2D solve %3").arg(a).arg(wheeler).arg(eddy)));
        QVERIFY2(std::abs(alpha.at(1) / alpha.at(0) - 1) < 0.07,
                 qPrintable(QStringLiteral("%1 at four across, %2 at eight").arg(alpha.at(0)).arg(alpha.at(1))));
    }

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
        // The metal's own: a 2D solve of the current through this cross-
        // section's metal - a micrometre of copper, two thirds of a skin
        // depth at 2 GHz, its current through all of it and crowding to its
        // edges as far as its own resistance lets it - gives 241.3 Ohm/m,
        // 1.40 times its direct current's. (The same solve gives QFEM's
        // copper coplanar line of the recheck of 1 November to 2 %.) The
        // sheet's crowding as phase B takes it, 204 Ohm/m, was what the
        // full wave was held to, and both were the edge cells' average.
        constexpr double eddy = 241.3;
        const double alphaC = eddy / (2 * lp.z0);
        qWarning("2 GHz: alpha %.5g Np/m; phase B's G %.4g S/m, alpha_d %.5g; the metal's 241.3 Ohm/m of a 2D eddy-current "
                 "solve, alpha_c %.5g; together %.5g (%.3f)",
                 wave.gamma.real(), lp.g, alphaD, alphaC, alphaD + alphaC, wave.gamma.real() / (alphaD + alphaC) - 1);
        // (Measured +5 %.)
        QVERIFY2(std::abs(wave.gamma.real() / (alphaD + alphaC) - 1) < 0.1,
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

    // A metal solved as its two faces (its "Solved as"): a sheet at its
    // bottom and one at its top, joined along its edges by its walls, each
    // face with its slab's surface impedance and the two coupled through
    // the metal. Its walls hold field in the air beside them, and its
    // current shares its faces as the field has it.
    void aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent()
    {
        const double h = 100e-6, w = 100, sigma = 5.8e7, t = 3e-6;
        // (A copper metal for its loss, and a perfect one of the same
        //  thickness for the line's C and L: at 100 MHz copper's current is
        //  still spread by its resistance, a fifth of w L, which puts L 2 to
        //  3 % over the charge's 1/(c0^2 C0) as a sheet or as its faces.)
        const auto asFaces = [](rf::Stack s) {
            s.layers.last().twoSheets = true;
            return s;
        };
        const rf::Stack copper = grounded(9.8, h, 0, t, sigma), perfect = grounded(9.8, h, 0, t, 0);
        // (A sanitized build solves lines of 1000 and 1500 µm, whose ends
        //  cancel in the low frequencies' differences as the longer ones'
        //  do, and leaves out the copper's loss at 10 GHz and its own
        //  inductance at 2 GHz - the two-line method wants twenty substrate
        //  heights - which the Release builds hold to. A line of two faces
        //  is twice the unknowns, its walls' kernels and three pairs of
        //  heights, and this function was two minutes of a sanitized build.)
        const bool slow = slowBuild();
        const double l1 = slow ? 1000 : 2000, l2 = slow ? 1500 : 3000;
        const QList<double> fs = slow ? QList<double>{1e7} : QList<double>{1e7, 10e9, 2e9};
        const rf::FullWave sa = slow ? rf::FullWave() : line(copper, l1, w, 4, 100e-6, 4, fs);
        const rf::FullWave sb = slow ? rf::FullWave() : line(copper, l2, w, 4, 100e-6, 4, fs);
        const rf::FullWave fa = line(asFaces(copper), l1, w, 4, 100e-6, 4, fs);
        const rf::FullWave fb = line(asFaces(copper), l2, w, 4, 100e-6, 4, fs);
        QVERIFY2((slow || (sa.ok() && sb.ok())) && fa.ok() && fb.ok(), qPrintable(why(sa) + why(sb) + why(fa) + why(fb)));
        // The mesh as given: its cells, and the runs between them a sheet has.
        const rf::Mesh given = meshed({strip(0, 0, l1, w)}, 4, 100e-6);
        const int own = int(given.cells.size()), sheetRuns = given.cost.unknowns;
        // Solved as two sheets and the walls between them, said; handed
        // back on the mesh as given - its own cells, and a run for each of
        // the sheet's.
        QVERIFY2(says(fa.lines, "M1 is solved as its two faces"), qPrintable(fa.lines.join(" | ")));
        QVERIFY2(says(fa.lines, "walls of the metals solved as their two faces"), qPrintable(fa.lines.join(" | ")));
        QVERIFY2(fa.runs > 2 * sheetRuns && fa.cells == 2 * own, qPrintable(QStringLiteral("%1 runs over %2 cells, as a sheet %3 over %4")
                                                                              .arg(fa.runs).arg(fa.cells).arg(sheetRuns).arg(own)));
        QCOMPARE(int(fa.geometry.size()), sheetRuns);
        QCOMPARE(int(fa.currents.at(0).at(0).size()), sheetRuns);
        const QHash<int, double> density = rf::currentDensity(fa, 0, 0);
        QVERIFY(!density.isEmpty());
        for (auto it = density.cbegin(); it != density.cend(); ++it) QVERIFY2(it.key() < own, qPrintable(QString::number(it.key())));
        // Its ports drive both faces of the face they are on, half to each.
        QVERIFY2(says(fa.portsSaid, "half of it on M1's top face"), qPrintable(fa.portsSaid.join(" | ")));

        // 10 MHz: direct current, through the whole metal however the faces
        // share it - the faces coupled through it, which as two sheets
        // apart was half (measured 1.4e-3, as the sheet's).
        const LowFrequency df = lowFrequency(fa, fb, 0, l1 * 1e-6, l2 * 1e-6);
        const double rDc = 1 / (sigma * w * 1e-6 * t);
        qWarning("R' at 10 MHz: as its faces %.5g Ohm/m, 1/(sigma W t) %.5g", df.rPer, rDc);
        QVERIFY2(std::abs(df.rPer / rDc - 1) < 5e-3, qPrintable(QStringLiteral("R' %1 Ohm/m, %2").arg(df.rPer).arg(rDc)));
        // And the current through the line's middle, of both faces folded
        // into the sheet's runs, is what charges the line beyond it: half
        // the ampere at 10 MHz with the far port open, as the sheet's
        // (measured 1e-6).
        const double middle = l1 / 2 * 1e-6;
        const auto through = [middle](const rf::FullWave& fw) {
            Complex sum(0, 0);
            for (int r = 0; r < fw.geometry.size(); ++r) {
                const rf::Filament& f = fw.geometry.at(r);
                if (f.z0 != f.z1 || !((f.from.x() - middle) * (f.to.x() - middle) < 0)) continue;
                sum += (f.to.x() > f.from.x() ? 1.0 : -1.0) * fw.currents.at(0).at(0).at(r);
            }
            return sum;
        };
        qWarning("at 10 MHz through the middle: %.6f A as its faces", std::abs(through(fa)));
        QVERIFY2(std::abs(std::abs(through(fa)) - 0.5) < 1e-3, qPrintable(QStringLiteral("%1 A").arg(std::abs(through(fa)))));
        if (!slow)
            QVERIFY2(std::abs(through(fa) / through(sa) - 1.0) < 1e-3,
                     qPrintable(QStringLiteral("%1 A, as a sheet %2").arg(std::abs(through(fa))).arg(std::abs(through(sa)))));

        // 100 MHz, of the perfect metal: the line per metre as the
        // quasi-static extraction of the same two faces has it, C' to its
        // charge and L' within the two discretizations' 1.5 % (measured
        // -4e-4 and -0.75 %); and the walls' field in the air beside them,
        // which lowers eps_eff and Z0 - a 2D solve of the cross-section's
        // two sheets on its four cells across lowers them 1.48 and 1.13 %,
        // of the whole thickness's 1.84 and 1.47 % (measured 1.49 and 1.10).
        rf::Extraction ps, pf;
        const rf::FullWave pa = line(perfect, l1, w, 4, 100e-6, 4, {1e8}, &ps);
        const rf::FullWave pb = line(perfect, l2, w, 4, 100e-6, 4, {1e8});
        const QList<double> ps2 = slow ? QList<double>{1e8} : QList<double>{1e8, 2e9};
        const rf::FullWave qa = line(asFaces(perfect), l1, w, 4, 100e-6, 4, ps2, &pf);
        const rf::FullWave qb = line(asFaces(perfect), l2, w, 4, 100e-6, 4, ps2);
        QVERIFY2(pa.ok() && pb.ok() && qa.ok() && qb.ok(), qPrintable(why(pa) + why(pb) + why(qa) + why(qb)));
        QVERIFY(!says(pa.lines, "two faces") && !says(pa.portsSaid, "top face"));
        const LowFrequency ls = lowFrequency(pa, pb, 0, l1 * 1e-6, l2 * 1e-6), lf = lowFrequency(qa, qb, 0, l1 * 1e-6, l2 * 1e-6);
        const rf::LineParameters lps = rf::lineParameters(ps, 0, l1 * 1e-6), lpf = rf::lineParameters(pf, 0, l1 * 1e-6);
        QVERIFY2(lps.ok() && lpf.ok(), qPrintable((lps.problems + lpf.problems).join(" | ")));
        const double epsSheet = ls.cPer * ls.lPer * kC0 * kC0, epsFaces = lf.cPer * lf.lPer * kC0 * kC0;
        const double zSheet = std::sqrt(ls.lPer / ls.cPer), zFaces = std::sqrt(lf.lPer / lf.cPer);
        qWarning("100 MHz: C' %.6g pF/m vs the extraction's %.6g (%.2e); L' %.6g nH/m vs %.6g (%.2e); eps_eff %.4f as a sheet, "
                 "%.4f as its faces (%.2f %%), the extraction's %.4f and %.4f (%.2f %%); Z0 %.3f, %.3f (%.2f %%), the "
                 "extraction's %.3f, %.3f (%.2f %%)",
                 lf.cPer * 1e12, lpf.c * 1e12, lf.cPer / lpf.c - 1, lf.lPer * 1e9, lpf.l * 1e9, lf.lPer / lpf.l - 1, epsSheet,
                 epsFaces, 100 * (epsFaces / epsSheet - 1), lps.epsilonEff, lpf.epsilonEff, 100 * (lpf.epsilonEff / lps.epsilonEff - 1),
                 zSheet, zFaces, 100 * (zFaces / zSheet - 1), lps.z0, lpf.z0, 100 * (lpf.z0 / lps.z0 - 1));
        QVERIFY2(std::abs(lf.cPer / lpf.c - 1) < 1e-3, qPrintable(QStringLiteral("C' %1 F/m, the extraction %2").arg(lf.cPer).arg(lpf.c)));
        QVERIFY2(std::abs(lf.lPer / lpf.l - 1) < 0.015, qPrintable(QStringLiteral("L' %1 H/m, the extraction %2").arg(lf.lPer).arg(lpf.l)));
        for (const auto& [now, was, two, whole] : {std::tuple(epsFaces, epsSheet, 1.48, 1.84), std::tuple(lpf.epsilonEff, lps.epsilonEff, 1.48, 1.84),
                                                   std::tuple(zFaces, zSheet, 1.13, 1.47), std::tuple(lpf.z0, lps.z0, 1.13, 1.47)})
            QVERIFY2(100 * (1 - now / was) > 0.8 * two && 100 * (1 - now / was) < whole,
                     qPrintable(QStringLiteral("%1 as its faces, %2 as a sheet: %3 % lower, the 2D two sheets' %4 % and the "
                                               "whole thickness's %5 %")
                                    .arg(now).arg(was).arg(100 * (1 - now / was)).arg(two).arg(whole)));

        if (slow) return;
        // 10 GHz: the current through the faces as the field has it - more
        // on the face toward the ground, 56 % to 44 % where the 2D solve of
        // the current through the metal has 59 to 41, and as much drawn into
        // the outer cells as it has, which a sheet's is not - and the loss
        // that solve's 219 Ohm/m (measured +15.5 % on four cells across, the
        // sheet's +18.5 %).
        const rf::LineParameters lp = rf::lineParameters(ps, 0, l1 * 1e-6);
        const double beta = 2 * M_PI * 10e9 / kC0 * std::sqrt(lp.epsilonEff);
        const TwoLine ws = twoLine(chainOf(zOf(sa, 1)), chainOf(zOf(sb, 1)), l1 * 1e-6, l2 * 1e-6, beta);
        const TwoLine wf = twoLine(chainOf(zOf(fa, 1)), chainOf(zOf(fb, 1)), l1 * 1e-6, l2 * 1e-6, beta);
        const double eddy = 219.0 / (2 * lp.z0);
        qWarning("10 GHz: alpha %.4g Np/m as a sheet (%.3f of the 2D solve's), %.4g as its faces (%.3f)", ws.gamma.real(),
                 ws.gamma.real() / eddy - 1, wf.gamma.real(), wf.gamma.real() / eddy - 1);
        QVERIFY2(std::abs(wf.gamma.real() / eddy - 1) < 0.2,
                 qPrintable(QStringLiteral("alpha %1 Np/m, the 2D solve %2").arg(wf.gamma.real()).arg(eddy)));

        // 2 GHz, two skin depths thick: the copper's own inductance - L'
        // over the perfect metal's - that solve's 1.46 %, its edges' share
        // with their reactance their resistance, as a thick metal's is.
        const double beta2 = 2 * M_PI * 2e9 / kC0 * std::sqrt(lp.epsilonEff);
        const TwoLine cu = twoLine(chainOf(zOf(fa, 2)), chainOf(zOf(fb, 2)), l1 * 1e-6, l2 * 1e-6, beta2);
        const TwoLine pec = twoLine(chainOf(zOf(qa, 1)), chainOf(zOf(qb, 1)), l1 * 1e-6, l2 * 1e-6, beta2);
        const double internal = (cu.z0 * cu.gamma).imag() / (pec.z0 * pec.gamma).imag() - 1;
        qWarning("2 GHz: L' of the copper over the perfect metal's, as its faces, %.4f (the 2D solve's 0.0146)", internal);
        QVERIFY2(std::abs(internal / 0.0146 - 1) < 0.25, qPrintable(QStringLiteral("%1, the 2D solve's 0.0146").arg(internal)));
    }

    // Lines on two metals, one over the other: every pair of heights its
    // own Green's functions, and at low frequency the two nets' Maxwell
    // matrix phase B's.
    void linesOnTwoMetalsAreSolvedTogether()
    {
        const rf::Stack two = twoMetals();
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
        QCOMPARE(fw.currents.size(), 1);
        QCOMPARE(fw.currents.first().size(), 2);
        QCOMPARE(fw.currents.first().first().size(), fw.runs);
        QCOMPARE(fw.geometry.size(), fw.runs);
        QCOMPARE(fw.ends.size(), fw.runs);
        const QList<Complex>& current = fw.currents.first().first();
        // The current across a line x = at (µm): the runs along x that cross it.
        const auto across = [&fw, &current](double at) {
            Complex sum(0, 0);
            for (int r = 0; r < fw.geometry.size(); ++r) {
                const rf::Filament& f = fw.geometry.at(r);
                const double x0 = std::min(f.from.x(), f.to.x()) * 1e6, x1 = std::max(f.from.x(), f.to.x()) * 1e6;
                if (x0 < at && x1 > at) sum += current.at(r) * (f.to.x() > f.from.x() ? 1.0 : -1.0);
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

    // The model's own ports: an edge port keeps to the metal it names and
    // to the face it says it faces; an internal port drives the cell under
    // it, against the ground or against the cell under it on another metal.
    void itsPortsAreTheModelsOwn()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> one{strip(0, 0, 600, 100)};
        const rf::Mesh mesh = meshed(one, 4, 50e-6);
        // At the corner: facing -x drives the strip's end, -y its side.
        const rf::FullWave end = rf::fullWaveOf(mesh, one, stack, {edgePort(QPointF(0, 0), QString(), QPointF(-1, 0))}, {1e9});
        const rf::FullWave side = rf::fullWaveOf(mesh, one, stack, {edgePort(QPointF(0, 0), QString(), QPointF(0, -1))}, {1e9});
        QVERIFY2(end.ok() && side.ok(), qPrintable(why(end) + why(side)));
        const int across = cellsDriven(end.portsSaid.value(0)), along = cellsDriven(side.portsSaid.value(0));
        qWarning("at the corner: %d cells facing -x, %d facing -y", across, along);
        QVERIFY2(across > 0 && along > 2 * across, qPrintable(end.portsSaid.join(" | ") + " / " + side.portsSaid.join(" | ")));
        // And the very faces: each the same port as one in the middle of
        // that face, which no facing need tell apart.
        const rf::FullWave endMiddle = rf::fullWaveOf(mesh, one, stack, {edgePort(QPointF(0, 50))}, {1e9});
        const rf::FullWave sideMiddle = rf::fullWaveOf(mesh, one, stack, {edgePort(QPointF(300, 0))}, {1e9});
        QVERIFY2(endMiddle.ok() && sideMiddle.ok(), qPrintable(why(endMiddle) + why(sideMiddle)));
        QVERIFY(std::abs(end.z[0][0][0] - endMiddle.z[0][0][0]) < 1e-12 * std::abs(endMiddle.z[0][0][0]));
        QVERIFY(std::abs(side.z[0][0][0] - sideMiddle.z[0][0][0]) < 1e-12 * std::abs(sideMiddle.z[0][0][0]));

        // A strip on M1 under a wider one on M2, their ends one over the
        // other: the port keeps to the metal it names.
        const rf::Stack two = twoMetals();
        rf::Conductor wide = strip(0, 0, 600, 300);
        wide.layer = QStringLiteral("M2");
        const QList<rf::Conductor> pair{strip(0, 0, 600, 100), wide};
        const rf::Mesh both = meshed(pair, 4, 50e-6);
        const rf::FullWave onM1 = rf::fullWaveOf(both, pair, two, {edgePort(QPointF(0, 50), QStringLiteral("M1"))}, {1e9});
        const rf::FullWave onM2 = rf::fullWaveOf(both, pair, two, {edgePort(QPointF(0, 50), QStringLiteral("M2"))}, {1e9});
        QVERIFY2(onM1.ok() && onM2.ok(), qPrintable(why(onM1) + why(onM2)));
        qWarning("on M1 %d cells, on M2 %d", cellsDriven(onM1.portsSaid.value(0)), cellsDriven(onM2.portsSaid.value(0)));
        QVERIFY2(cellsDriven(onM2.portsSaid.value(0)) > cellsDriven(onM1.portsSaid.value(0)),
                 qPrintable(onM1.portsSaid.join(" | ") + " / " + onM2.portsSaid.join(" | ")));

        // An internal port to the ground in the middle of the strip: at low
        // frequency the strip is one potential, and it sees its whole C.
        const double w = 2 * M_PI * 1e8;
        const rf::Extraction e = rf::extract(mesh, one, stack);
        QVERIFY2(e.ok(), qPrintable(e.problems.join(" | ")));
        const rf::FullWave middle = rf::fullWaveOf(mesh, one, stack, {internalPort(QPointF(300, 50), QStringLiteral("M1"))}, {1e8});
        QVERIFY2(middle.ok(), qPrintable(why(middle)));
        QVERIFY(says(middle.portsSaid, "against the ground"));
        const double c = (1.0 / middle.z[0][0][0]).imag() / w;
        qWarning("an internal port to the ground: C %.6g pF, phase B %.6g (%.2e)", c * 1e12, e.selfCapacitance(0) * 1e12,
                 c / e.selfCapacitance(0) - 1);
        QVERIFY2(std::abs(c / e.selfCapacitance(0) - 1) < 1e-3,
                 qPrintable(QStringLiteral("C %1 pF, phase B %2").arg(c * 1e12).arg(e.selfCapacitance(0) * 1e12)));

        // And between two plates, one over the other: the capacitance between
        // two conductors that hold no charge between them, of phase B's
        // Maxwell matrix (Caa Cbb - Cab^2)/(Caa + Cbb + 2 Cab).
        rf::Conductor upper = strip(0, 0, 300, 300);
        upper.layer = QStringLiteral("M2");
        const QList<rf::Conductor> plates{strip(0, 0, 300, 300), upper};
        const rf::Mesh pm = meshed(plates, 4, 50e-6);
        const rf::Extraction pe = rf::extract(pm, plates, two);
        QVERIFY2(pe.ok() && pe.maxwell.size() == 2, qPrintable(pe.problems.join(" | ")));
        const double caa = pe.maxwell[0][0], cbb = pe.maxwell[1][1], cab = 0.5 * (pe.maxwell[0][1] + pe.maxwell[1][0]);
        const double between = (caa * cbb - cab * cab) / (caa + cbb + 2 * cab);
        const rf::FullWave across2 =
            rf::fullWaveOf(pm, plates, two, {internalPort(QPointF(150, 150), QStringLiteral("M2"), QStringLiteral("M1"))}, {1e8});
        QVERIFY2(across2.ok(), qPrintable(why(across2)));
        QVERIFY(says(across2.portsSaid, "on M1"));
        const double cp = (1.0 / across2.z[0][0][0]).imag() / w;
        qWarning("between the plates: C %.6g pF, of phase B's matrix %.6g (%.2e)", cp * 1e12, between * 1e12, cp / between - 1);
        QVERIFY2(std::abs(cp / between - 1) < 3e-3, qPrintable(QStringLiteral("C %1 pF, phase B %2").arg(cp * 1e12).arg(between * 1e12)));

        // An internal port on no cell of its metal, and against its own cell.
        QVERIFY(says(rf::fullWaveOf(mesh, one, stack, {internalPort(QPointF(300, 50), QStringLiteral("M7"))}, {1e9}).problems,
                     "over no cell"));
        QVERIFY(says(rf::fullWaveOf(mesh, one, stack, {internalPort(QPointF(300, 50), QStringLiteral("M1"), QStringLiteral("M1"))},
                                    {1e9}).problems,
                     "to drive against"));
    }

    // Each port into its own reference: Kurokawa's power waves, against
    // the two-port's closed form; and a solve's S is of its ports' own.
    void eachPortIsReferredToItsOwnImpedance()
    {
        const Complex z11(30, 5), z12(10, -2), z22(70, 20);
        const double r1 = 50, r2 = 25;
        const QList<QList<Complex>> s = rf::scatteringOf({{z11, z12}, {z12, z22}}, QList<double>{r1, r2});
        const Complex d = (z11 + r1) * (z22 + r2) - z12 * z12;
        const Complex want11 = ((z11 - r1) * (z22 + r2) - z12 * z12) / d, want22 = ((z11 + r1) * (z22 - r2) - z12 * z12) / d;
        const Complex want21 = 2.0 * z12 * std::sqrt(r1 * r2) / d;
        QVERIFY(std::abs(s[0][0] - want11) < 1e-14 && std::abs(s[1][1] - want22) < 1e-14);
        QVERIFY(std::abs(s[1][0] - want21) < 1e-14 && std::abs(s[0][1] - want21) < 1e-14);
        // One reference is the old (Z - Z0)(Z + Z0)^-1.
        const QList<QList<Complex>> same = rf::scatteringOf({{z11, z12}, {z12, z22}}, QList<double>{50, 50});
        const QList<QList<Complex>> plain = rf::scatteringOf({{z11, z12}, {z12, z22}}, 50.0);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) QVERIFY(std::abs(same[i][j] - plain[i][j]) < 1e-15);
        QVERIFY(rf::scatteringOf({{z11}}, QList<double>{50, 50}).isEmpty());

        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> one{strip(0, 0, 600, 100)};
        const rf::Mesh mesh = meshed(one, 4, 50e-6);
        const rf::FullWave fw = rf::fullWaveOf(
            mesh, one, stack, {edgePort(QPointF(0, 50), QString(), QPointF(), 50), edgePort(QPointF(600, 50), QString(), QPointF(), 25)},
            {5e9});
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        QCOMPARE(fw.references, (QList<double>{50, 25}));
        QCOMPARE(fw.portNames, (QStringList{QStringLiteral("P1"), QStringLiteral("P2")}));
        const QList<QList<Complex>> own = rf::scatteringOf(fw.z[0], QList<double>{50, 25});
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) QVERIFY(std::abs(fw.s[0][i][j] - own[i][j]) < 1e-15);
        // A reference of nothing at one port is refused, naming it.
        rf::FullWavePort bad = edgePort(QPointF(600, 50), QString(), QPointF(), 0);
        bad.name = QStringLiteral("Out");
        QVERIFY(says(rf::fullWaveOf(mesh, one, stack, {edgePort(QPointF(0, 50)), bad}, {5e9}).problems, "Out"));
    }

    // A sweep's frequencies are solved side by side - the answer the same
    // on one thread as on several -, counted as they are done, and stopped
    // when asked.
    void aSweepIsSolvedSideBySideAndCanBeStopped()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> one{strip(0, 0, 600, 100)};
        const rf::Mesh mesh = meshed(one, 4, 50e-6);
        const QList<rf::FullWavePort> ports{edgePort(QPointF(0, 50)), edgePort(QPointF(600, 50))};
        const QList<double> fs{1e9, 4e9, 9e9};
        std::atomic_int done{0};
        rf::FullWaveControl alone;
        alone.threads = 1;
        alone.done = &done;
        const rf::FullWave a = rf::fullWaveOf(mesh, one, stack, ports, fs, 1e-6, &alone);
        QVERIFY2(a.ok(), qPrintable(why(a)));
        QCOMPARE(a.threads, 1);
        QCOMPARE(done.load(), 3);
        rf::FullWaveControl several;
        several.threads = 3;
        const rf::FullWave b = rf::fullWaveOf(mesh, one, stack, ports, fs, 1e-6, &several);
        QVERIFY2(b.ok(), qPrintable(why(b)));
        QCOMPARE(b.threads, 3);
        QCOMPARE(b.frequencies, fs);
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) QCOMPARE(b.z[k][i][j], a.z[k][i][j]);
        // Reciprocal and passive by its own checks.
        QVERIFY2(b.asymmetry < 1e-12 && b.gain < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(b.asymmetry).arg(b.gain)));
        // Stopped before it began: nothing, and said.
        std::atomic_bool stop{true};
        rf::FullWaveControl stopped;
        stopped.stop = &stop;
        const rf::FullWave c = rf::fullWaveOf(mesh, one, stack, ports, fs, 1e-6, &stopped);
        QVERIFY(!c.ok() && c.z.isEmpty() && says(c.problems, "Stopped, 0 of 3"));
    }

    // The current density drawn on the layout, and the Touchstone written,
    // are of the solve.
    void theCurrentDensityAndTheTouchstoneAreOfTheSolve()
    {
        const double length = 600, w = 100;
        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> one{strip(0, 0, length, w)};
        const rf::Mesh mesh = meshed(one, 4, 50e-6);
        const rf::FullWave fw = rf::fullWaveOf(mesh, one, stack, QList<QPointF>{QPointF(0, w / 2), QPointF(length, w / 2)}, {1e8, 3e9, 6e9});
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        // At low frequency, across the middle the cells' |J| times their
        // breadth is the half of the port's ampere that charges the far
        // half: of the two columns of cells either side of it, each the
        // mean of its runs, the mean - by symmetry, exactly that half.
        const QHash<int, double> j = rf::currentDensity(fw, 0, 0);
        QVERIFY(!j.isEmpty());
        double left = 0, right = length;
        for (const rf::MeshCell& cell : mesh.cells) {
            if (cell.centre.x() < length / 2) left = std::max(left, cell.centre.x());
            if (cell.centre.x() > length / 2) right = std::min(right, cell.centre.x());
        }
        double middle = 0, atEnd = 0, nearEnd = 0;
        for (auto it = j.cbegin(); it != j.cend(); ++it) {
            const rf::MeshCell& cell = mesh.cells.at(it.key());
            const QRectF box = cell.outline.boundingRect();
            if (std::abs(cell.centre.x() - left) < 1e-6 || std::abs(cell.centre.x() - right) < 1e-6)
                middle += 0.5 * it.value() * box.height() * 1e-6;
            if (box.left() < 50) nearEnd = std::max(nearEnd, it.value());
            if (box.right() > length - 50) atEnd = std::max(atEnd, it.value());
        }
        qWarning("across the middle %.5f A; |J| up to %.4g A/m at the driven end, %.4g at the open one", middle, nearEnd, atEnd);
        QVERIFY2(std::abs(middle - 0.5) < 0.02, qPrintable(QString::number(middle)));
        QVERIFY2(nearEnd > 5 * atEnd, qPrintable(QStringLiteral("%1 %2").arg(nearEnd).arg(atEnd)));
        QVERIFY(rf::currentDensity(fw, 3, 0).isEmpty() && rf::currentDensity(fw, 0, 2).isEmpty());
        // Driven as a circuit drives it - a source of 50 Ohm, the far port
        // ended in 50 -, the millimetre of line is a short piece of wire at
        // 100 MHz: the same current all along it, 2 sqrt(50) / (50 + 50).
        const QHash<int, double> ended = rf::currentDensity(fw, 0, 0, true);
        double endedMiddle = 0;
        for (auto it = ended.cbegin(); it != ended.cend(); ++it) {
            const rf::MeshCell& cell = mesh.cells.at(it.key());
            if (std::abs(cell.centre.x() - left) < 1e-6 || std::abs(cell.centre.x() - right) < 1e-6)
                endedMiddle += 0.5 * it.value() * cell.outline.boundingRect().height() * 1e-6;
        }
        const double through = 2 * std::sqrt(50.0) / 100;
        qWarning("ended in 50 Ohm: across the middle %.6f A, of %.6f", endedMiddle, through);
        QVERIFY2(std::abs(endedMiddle / through - 1) < 0.01, qPrintable(QStringLiteral("%1 A, %2").arg(endedMiddle).arg(through)));

        // A two-port: S11 S21 S12 S22 a line, in the file's reference.
        const QString two = rf::touchstoneOf(fw, 75, {QStringLiteral("a line\nof 600 µm")});
        QVERIFY(two.startsWith(QStringLiteral("! a line\n! of 600 µm\n")));
        QVERIFY(two.contains(QStringLiteral("! Ports: 1 P1, 2 P2\n")) && two.contains(QStringLiteral("# Hz S RI R 75\n")));
        QStringList data;
        for (const QString& line : two.split(QLatin1Char('\n')))
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('!')) && !line.startsWith(QLatin1Char('#'))) data << line;
        QCOMPARE(data.size(), 3);
        const QStringList cells = data.at(2).split(QLatin1Char(' '));
        QCOMPARE(cells.size(), 9);
        const QList<QList<Complex>> s = rf::scatteringOf(fw.z[2], 75.0);
        QCOMPARE(cells.at(0).toDouble(), 6e9);
        QVERIFY(std::abs(cells.at(3).toDouble() - s[1][0].real()) < 1e-11 && std::abs(cells.at(6).toDouble() - s[0][1].imag()) < 1e-11);
        QVERIFY(rf::touchstoneOf(fw, 0).isEmpty() && rf::touchstoneOf(rf::FullWave(), 50).isEmpty());

        // Touchstone's own two-port order is S11 S21 S12 S22, which only a
        // two-port that is not reciprocal can show.
        rf::FullWave oneWay;
        oneWay.frequencies = {1e9};
        oneWay.z = {{{Complex(50, 0), Complex(10, 0)}, {Complex(30, 0), Complex(50, 0)}}};
        oneWay.portNames = {"A", "B"};
        const QList<QList<Complex>> sOneWay = rf::scatteringOf(oneWay.z[0], 50.0);
        QVERIFY(std::abs(sOneWay[1][0] - sOneWay[0][1]) > 0.1);
        QStringList oneLine;
        for (const QString& line : rf::touchstoneOf(oneWay, 50).split(QLatin1Char('\n')))
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('!')) && !line.startsWith(QLatin1Char('#'))) oneLine << line;
        QCOMPARE(oneLine.size(), 1);
        const QStringList terms = oneLine.first().split(QLatin1Char(' '));
        QVERIFY(std::abs(terms.at(3).toDouble() - sOneWay[1][0].real()) < 1e-11);   // S21 second
        QVERIFY(std::abs(terms.at(5).toDouble() - sOneWay[0][1].real()) < 1e-11);   // S12 third

        // Five ports: by rows, four to a line, each row on lines of its own.
        rf::FullWave five;
        five.frequencies = {1e9};
        QList<QList<Complex>> z;
        for (int i = 0; i < 5; ++i) {
            QList<Complex> row;
            for (int k = 0; k < 5; ++k) row << Complex(i == k ? 60.0 : 5.0, 0);
            z << row;
        }
        five.z = {z};
        five.portNames = {"A", "B", "C", "D", "E"};
        const QString many = rf::touchstoneOf(five, 50);
        QStringList rows;
        for (const QString& line : many.split(QLatin1Char('\n')))
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('!')) && !line.startsWith(QLatin1Char('#'))) rows << line;
        QCOMPARE(rows.size(), 10);
        QCOMPARE(rows.at(0).split(QLatin1Char(' ')).size(), 9);   // f and four pairs
        QCOMPARE(rows.at(1).split(QLatin1Char(' '), Qt::SkipEmptyParts).size(), 2);   // the fifth pair
        QCOMPARE(rows.at(2).split(QLatin1Char(' '), Qt::SkipEmptyParts).size(), 8);   // row two's first four
    }

    // A via to the ground at the far end of a line: the line shorted by it
    // less the same line shorted at the same cells by internal ports - ideal
    // shorts, with no field - is the via alone, the port and the line being
    // in both. In vacuum that is its partial inductance and its image's, in
    // closed form, the cross-section's mean distance with itself standing in
    // for its breadth. Through 200 µm of 9.8 it is the same, as
    // magnetostatics says: the dielectric's part of the via's own G_zz, its
    // potential along the runs and at the port, and the line's G_zx along it
    // all cancel. With the via over one cell and over two.
    void aViaToTheGroundIsItsOwnInductance()
    {
        const double h = 200e-6, w = 50, l = 2000, f = 1e8, omega = 2 * M_PI * f;
        const double g = 0.2235 * 2 * w * 1e-6;
        const double closed = kMu0 / (4 * M_PI) * (linePair(0, h, 0, h, g) + linePair(0, h, -h, 0, g));
        for (const int across : {1, 2}) {
            QList<double> own;
            for (const double epsr : {1.0, 9.8}) {
                const rf::Stack stack = withVia(grounded(epsr, h), QStringLiteral("V0"), QStringLiteral("ground"), QStringLiteral("M1"));
                const QList<rf::Conductor> line = {strip(0, 0, l, w)};
                const QList<rf::Conductor> shorted = {strip(0, 0, l, w), on(QStringLiteral("V0"), l - w, 0, w, w)};
                const rf::Mesh meshA = evenly(shorted, across, 400e-6), meshB = evenly(line, across, 400e-6);
                QList<rf::FullWavePort> ports = {edgePort(QPointF(0, w / 2))};
                for (const rf::MeshCell& c : meshB.cells)
                    if (c.outline.boundingRect().intersects(QRectF(l - w, 0, w, w)))
                        ports << internalPort(c.centre, QStringLiteral("M1"));
                QCOMPARE(ports.size(), 1 + across);
                const rf::FullWave a = rf::fullWaveOf(meshA, shorted, stack, {edgePort(QPointF(0, w / 2))}, {f});
                const rf::FullWave b = rf::fullWaveOf(meshB, line, stack, ports, {f});
                QVERIFY2(a.ok(), qPrintable(why(a)));
                QVERIFY2(b.ok(), qPrintable(why(b)));
                QVERIFY2(says(a.lines, qPrintable(QStringLiteral("%1 of the runs are vias, %1 of them to the ground").arg(across))),
                         qPrintable(a.lines.join(" | ")));
                const Complex za = a.z.at(0).at(0).at(0), zb = 1.0 / keptAdmittance(b, 0, 1)(0, 0);
                own << (za - zb).imag() / omega;
                // It radiates a little and gives nothing back.
                QVERIFY2(za.real() > 0 && a.gain <= 1e-12, qPrintable(QStringLiteral("Re Z %1, gain %2").arg(za.real()).arg(a.gain)));
                // An ampere in at the port all goes down the via at 100 MHz,
                // drawn on the via's own cells round its sides: each run's
                // density times its perimeter, summed, is the ampere.
                const QHash<int, double> density = rf::currentDensity(a, 0, 0);
                QHash<int, double> down;
                for (auto it = a.viaCells.cbegin(); it != a.viaCells.cend(); ++it) {
                    QVERIFY(meshA.cells.at(it.key()).layer == QLatin1String("V0") && density.contains(it.key()));
                    down.insert(it.value(), density.value(it.key()) * 4 * a.geometry.at(it.value()).width);
                }
                double total = 0;
                for (const double i : std::as_const(down)) total += i;
                QVERIFY2(down.size() == across && std::abs(total - 1.0) < 1e-3,
                         qPrintable(QStringLiteral("%1 via run(s) drawn, %2 A down them").arg(down.size()).arg(total)));
                qWarning().noquote() << QStringLiteral("%1 across, eps_r %2: the via %3 nH, closed form %4 (%5)")
                                            .arg(across).arg(epsr).arg(own.last() * 1e9, 0, 'f', 6).arg(closed * 1e9, 0, 'f', 6)
                                            .arg(own.last() / closed - 1, 0, 'e', 2);
            }
            // (Measured: over one cell 2e-5 of the closed form, the dielectric
            //  2.4e-4 of vacuum; over two cells, a pair of vias side by side.)
            if (across == 1) QVERIFY2(std::abs(own.at(0) / closed - 1) < 2e-4, "in vacuum, the closed form");
            else QVERIFY2(std::abs(own.at(0) / closed - 1) < 0.05, "two vias side by side, near the one");
            QVERIFY2(std::abs(own.at(1) / own.at(0) - 1) < (across == 1 ? 1e-3 : 3e-3), "the dielectric changes nothing");
        }
    }

    // A via of a metal is a bar: at 10 MHz, the skin far deeper than its
    // half-width, its resistance is its length over sigma and its area -
    // the line shorted by it less by an ideal short, in its real part.
    void aViasResistanceIsItsBars()
    {
        const double h = 200e-6, w = 50, l = 2000, f = 1e7, sigma = 1e6;
        rf::Stack stack = withVia(grounded(9.8, h), QStringLiteral("V0"), QStringLiteral("ground"), QStringLiteral("M1"));
        stack.layers.last().sigma = sigma;
        const QList<rf::Conductor> line = {strip(0, 0, l, w)};
        const QList<rf::Conductor> shorted = {strip(0, 0, l, w), on(QStringLiteral("V0"), l - w, 0, w, w)};
        const rf::Mesh meshB = evenly(line, 1, 400e-6);
        QPointF end;
        for (const rf::MeshCell& c : meshB.cells)
            if (c.outline.boundingRect().intersects(QRectF(l - w, 0, w, w))) end = c.centre;
        const rf::FullWave a = rf::fullWaveOf(evenly(shorted, 1, 400e-6), shorted, stack, {edgePort(QPointF(0, w / 2))}, {f});
        const rf::FullWave b = rf::fullWaveOf(meshB, line, stack, {edgePort(QPointF(0, w / 2)), internalPort(end, QStringLiteral("M1"))}, {f});
        QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
        const Complex extra = a.z.at(0).at(0).at(0) - 1.0 / keptAdmittance(b, 0, 1)(0, 0);
        const double want = h / (sigma * w * w * 1e-12);
        qWarning().noquote() << QStringLiteral("a via of %1 S/m at 10 MHz: R %2 Ohm, h/(sigma A) %3 (%4)")
                                    .arg(sigma).arg(extra.real(), 0, 'f', 6).arg(want, 0, 'f', 6).arg(extra.real() / want - 1, 0, 'e', 2);
        QVERIFY2(std::abs(extra.real() / want - 1) < 1e-3,
                 qPrintable(QStringLiteral("R %1 Ohm, of h/(sigma A) %2").arg(extra.real()).arg(want)));
    }

    // A via between two metals in a line that climbs from M1 to M2: the two-
    // port's series inductance at a low frequency (-1/Y12) less the same
    // lines joined by an internal port from M1 to M2, shorted, is the via
    // alone - its partial inductance and its image's, through 100 µm of 4 on
    // 100 µm of 9.8 as in vacuum. The two-port is reciprocal and passive,
    // and joined: the line's wave passes.
    void aViaBetweenTwoMetalsJoinsThem()
    {
        const double h1 = 100e-6, h2 = 100e-6, w = 50, f = 1e8, omega = 2 * M_PI * f;
        const double g = 0.2235 * 2 * w * 1e-6;
        const double closed = kMu0 / (4 * M_PI) * (linePair(h1, h1 + h2, h1, h1 + h2, g) + linePair(h1, h1 + h2, -h1 - h2, -h1, g));
        QList<double> own;
        for (const bool vacuum : {true, false}) {
            rf::Stack two = twoMetals();
            for (rf::Layer& layer : two.layers)
                if (layer.kind == rf::Layer::Dielectric) layer.thickness = h2;
            if (vacuum)
                for (rf::Layer& layer : two.layers) layer.epsr = 1;
            two = withVia(two, QStringLiteral("V1"), QStringLiteral("M1"), QStringLiteral("M2"));
            const QList<rf::Conductor> lines = {strip(0, 0, 1000, w), on(QStringLiteral("M2"), 1000 - w, 0, 1000, w)};
            const QList<rf::Conductor> climbing = lines + QList<rf::Conductor>{on(QStringLiteral("V1"), 1000 - w, 0, w, w)};
            const QList<rf::FullWavePort> ends = {edgePort(QPointF(0, w / 2), QStringLiteral("M1")),
                                                  edgePort(QPointF(2000 - w, w / 2), QStringLiteral("M2"))};
            const rf::FullWave a = rf::fullWaveOf(evenly(climbing, 1, 400e-6, 1), climbing, two, ends, {f, 5e9, 20e9});
            const QList<rf::FullWavePort> joint = {
                ends.at(0), ends.at(1), internalPort(QPointF(1000 - w / 2, w / 2), QStringLiteral("M2"), QStringLiteral("M1"))};
            const rf::FullWave b = rf::fullWaveOf(evenly(lines, 1, 400e-6, 1), lines, two, joint, {f});
            QVERIFY2(a.ok(), qPrintable(why(a)));
            QVERIFY2(b.ok(), qPrintable(why(b)));
            QVERIFY2(says(a.lines, "1 of the runs are vias, 0 of them to the ground"), qPrintable(a.lines.join(" | ")));
            const Eigen::MatrixXcd ya = keptAdmittance(a, 0, 2), yb = keptAdmittance(b, 0, 2);
            own << (-1.0 / ya(0, 1) + 1.0 / yb(0, 1)).imag() / omega;
            // Passive through the dielectrics. In vacuum a port's own vertical
            // current, which no port of this solve gives a field, is the
            // larger part of what a line over a ground radiates, and without
            // it S gives back a hair more than it gets - the lines alone, no
            // via, as much (2e-5 of a wave): phase D's ports.
            QVERIFY2(a.asymmetry < 1e-10 && (vacuum ? a.gain < 1e-3 : a.gain <= 1e-12),
                     qPrintable(QStringLiteral("asymmetry %1, gain %2").arg(a.asymmetry).arg(a.gain)));
            QVERIFY2(std::abs(a.s.at(0).at(1).at(0)) > 0.9,
                     qPrintable(QStringLiteral("|S21| %1 at 100 MHz").arg(std::abs(a.s.at(0).at(1).at(0)))));
            qWarning().noquote() << QStringLiteral("%1: the via %2 nH, closed form %3 (%4); |S21| %5 at 5 GHz, %6 at 20")
                                        .arg(vacuum ? QStringLiteral("vacuum") : QStringLiteral("9.8 and 4"))
                                        .arg(own.last() * 1e9, 0, 'f', 6).arg(closed * 1e9, 0, 'f', 6)
                                        .arg(own.last() / closed - 1, 0, 'e', 2)
                                        .arg(std::abs(a.s.at(1).at(1).at(0)), 0, 'f', 4).arg(std::abs(a.s.at(2).at(1).at(0)), 0, 'f', 4);
        }
        QVERIFY2(std::abs(own.at(0) / closed - 1) < 2e-3, "in vacuum, the closed form");
        QVERIFY2(std::abs(own.at(1) / own.at(0) - 1) < 5e-3, "the dielectrics change nothing");

        {   // M1 2 µm thick and solved as its two faces (its "Solved as"):
            // the via leaves its top face, so it is 2 µm shorter than it
            // would be from its bottom, and an internal port between the two
            // metals stands on that top face too - the via its closed form
            // from there, in vacuum.
            const double t = 2e-6;
            rf::Stack two = twoMetals();
            for (rf::Layer& layer : two.layers) {
                if (layer.kind == rf::Layer::Dielectric) layer.thickness = h2;
                layer.epsr = 1;
                if (layer.name == QLatin1String("M1")) {
                    layer.thickness = t;
                    layer.twoSheets = true;
                }
            }
            two = withVia(two, QStringLiteral("V1"), QStringLiteral("M1"), QStringLiteral("M2"));
            const QList<rf::Conductor> lines = {strip(0, 0, 1000, w), on(QStringLiteral("M2"), 1000 - w, 0, 1000, w)};
            const QList<rf::Conductor> climbing = lines + QList<rf::Conductor>{on(QStringLiteral("V1"), 1000 - w, 0, w, w)};
            const QList<rf::FullWavePort> ends = {edgePort(QPointF(0, w / 2), QStringLiteral("M1")),
                                                  edgePort(QPointF(2000 - w, w / 2), QStringLiteral("M2"))};
            const rf::FullWave a = rf::fullWaveOf(evenly(climbing, 1, 400e-6, 1), climbing, two, ends, {f});
            const QList<rf::FullWavePort> joint = {
                ends.at(0), ends.at(1), internalPort(QPointF(1000 - w / 2, w / 2), QStringLiteral("M2"), QStringLiteral("M1"))};
            const rf::FullWave b = rf::fullWaveOf(evenly(lines, 1, 400e-6, 1), lines, two, joint, {f});
            QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
            QVERIFY2(says(a.lines, "1 of the runs are vias, 0 of them to the ground"), qPrintable(a.lines.join(" | ")));
            // (The port's current goes up from that face: a wide metal's two
            //  faces meet only round its edges, as a thick one's do once its
            //  skin is thin.)
            QVERIFY2(says(b.portsSaid, "the cell under it on M1's top face"), qPrintable(b.portsSaid.join(" | ")));
            // The via's cells, on the mesh as given, are its run.
            QVERIFY(!a.viaCells.isEmpty());
            for (auto it = a.viaCells.cbegin(); it != a.viaCells.cend(); ++it)
                QVERIFY2(it.value() >= 0 && it.value() < a.geometry.size() && a.geometry.at(it.value()).z0 != a.geometry.at(it.value()).z1,
                         qPrintable(QStringLiteral("cell %1, run %2").arg(it.key()).arg(it.value())));
            const Eigen::MatrixXcd ya = keptAdmittance(a, 0, 2), yb = keptAdmittance(b, 0, 2);
            const double via = (-1.0 / ya(0, 1) + 1.0 / yb(0, 1)).imag() / omega;
            const double top = h1 + t;
            const double shorter = kMu0 / (4 * M_PI) * (linePair(top, top + h2, top, top + h2, g) + linePair(top, top + h2, -top - h2, -top, g));
            // (Measured 1.4 % over it: the bottom face's share of the current
            //  climbs the walls beside the via's foot, alongside it. From the
            //  bottom face, 2 µm longer, it read 0.12 % less, its walls' current
            //  then against it: the stack says where it stands, not this.)
            qWarning("from M1's top face: the via %.6f nH, its closed form from there %.6f (%.2e)", via * 1e9, shorter * 1e9,
                     via / shorter - 1);
            QVERIFY2(via > shorter && via / shorter - 1 < 0.02,
                     qPrintable(QStringLiteral("%1 nH, %2").arg(via * 1e9).arg(shorter * 1e9)));
            QVERIFY2(a.asymmetry < 1e-10, qPrintable(QStringLiteral("asymmetry %1").arg(a.asymmetry)));
        }
    }

    // The phase C check of 1 November, F6: a grounded coplanar line driven
    // by edge ports that return through the ground plane radiates from its
    // ends - a millimetre of it lost a third of the power at 60 GHz, and a
    // longer line less, so it is at the ends - where a ground-signal-ground
    // probe's return, through the conductors either side of it on the line
    // of its edge, is what a coplanar line wants. 100 µm on 200 µm of 9.8,
    // 50 µm slots, 250 µm grounds with a fence of vias down each.
    void aCoplanarPortReturnsThroughItsGrounds()
    {
        const rf::Stack stack = withVia(grounded(9.8, 200e-6), QStringLiteral("V1"), QStringLiteral("ground"), QStringLiteral("M1"));
        const double l = 1000;
        const QString m1 = QStringLiteral("M1");
        QList<rf::Conductor> cpw = {on(m1, 0, -50, l, 100), on(m1, 0, 100, l, 250), on(m1, 0, -350, l, 250)};
        for (double x = 25; x < l; x += 150)
            for (const double y : {200.0, -250.0}) cpw << on(QStringLiteral("V1"), x, y, 50, 50);
        const rf::Mesh mesh = meshed(cpw, 3, 1e-3);
        const QList<double> fs = {60e9};
        const QList<rf::FullWavePort> ground = {edgePort(QPointF(0, 0), m1, QPointF(-1, 0)), edgePort(QPointF(l, 0), m1, QPointF(1, 0))};
        QList<rf::FullWavePort> beside = ground;
        for (rf::FullWavePort& p : beside) p.coplanar = true;
        const rf::FullWave viaGround = rf::fullWaveOf(mesh, cpw, stack, ground, fs);
        const rf::FullWave viaBeside = rf::fullWaveOf(mesh, cpw, stack, beside, fs);
        QVERIFY2(viaGround.ok(), qPrintable(why(viaGround)));
        QVERIFY2(viaBeside.ok(), qPrintable(why(viaBeside)));
        QVERIFY2(says(viaBeside.portsSaid, "against the conductors beside it") && says(viaBeside.portsSaid, "either side"),
                 qPrintable(viaBeside.portsSaid.join(" | ")));
        QVERIFY2(says(viaBeside.lines, "a coplanar port's through the conductors beside it"), qPrintable(viaBeside.lines.join(" | ")));
        QVERIFY2(viaBeside.asymmetry < 1e-9 && viaBeside.gain <= 1e-6,
                 qPrintable(QStringLiteral("%1 %2").arg(viaBeside.asymmetry).arg(viaBeside.gain)));
        const auto lost = [](const rf::FullWave& w) {
            const Complex s11 = w.s.at(0).at(0).at(0), s21 = w.s.at(0).at(1).at(0);
            return 1 - std::norm(s11) - std::norm(s21);
        };
        qWarning().noquote() << QStringLiteral("1 mm at 60 GHz loses %1 of the power returned through the ground, %2 "
                                               "through the conductors beside it")
                                    .arg(lost(viaGround), 0, 'f', 4).arg(lost(viaBeside), 0, 'f', 4);
        QVERIFY2(lost(viaBeside) < 0.03, qPrintable(QString::number(lost(viaBeside))));
        QVERIFY2(lost(viaGround) > 0.15, qPrintable(QString::number(lost(viaGround))));
        // An ampere into P1 at 10 GHz goes along the signal from its face,
        // and comes back alike along each ground: the grounds tied, and the
        // solve sharing the current out between them. (Not all of it by the
        // runs out of the faces' cells: some charges them, and on a ground
        // some goes down the fence's first via, which stands on its face.)
        const rf::FullWave low = rf::fullWaveOf(mesh, cpw, stack, beside, {10e9});
        QVERIFY2(low.ok(), qPrintable(why(low)));
        // (What leaves each face's own cells by the runs out of them, so
        //  that it holds whatever length the cells along the line have.)
        const auto onFace = [&mesh](int cell) { return cell >= 0 && std::abs(mesh.cells.at(cell).outline.boundingRect().left()) < 1e-6; };
        double signal = 0, upper = 0, lower = 0;
        for (int r = 0; r < low.geometry.size(); ++r) {
            const rf::Filament& f = low.geometry.at(r);
            const int a = low.ends.at(r).first, b = low.ends.at(r).second;
            if (f.z0 != f.z1 || onFace(a) == onFace(b)) continue;
            const double i = (onFace(a) ? 1 : -1) * low.currents.at(0).at(0).at(r).real();
            const double y = mesh.cells.at(onFace(a) ? a : b).centre.y();
            if (std::abs(y) < 50) signal += i;
            else if (y > 100 && y < 350) upper += i;
            else if (y < -100 && y > -350) lower += i;
        }
        QVERIFY2(signal > 0.8 && signal < 1.05 && upper < -0.2 && std::abs(upper - lower) < 1e-3 * std::abs(upper),
                 qPrintable(QStringLiteral("%1 %2 %3").arg(signal).arg(upper).arg(lower)));
        {   // Of 3 µm of metal solved as its two faces (its "Solved as"), the
            // port and the grounds beside it drive both faces, half each -
            // a face of a thick conductor is its whole wall - and the same
            // current comes back alike along each ground, folded into the
            // mesh's own runs.
            rf::Stack thick = stack;
            for (rf::Layer& l : thick.layers)
                if (l.kind == rf::Layer::Metal) {
                    l.thickness = 3e-6;
                    l.twoSheets = true;
                }
            const rf::FullWave faced = rf::fullWaveOf(mesh, cpw, thick, beside, {10e9});
            QVERIFY2(faced.ok(), qPrintable(why(faced)));
            QVERIFY2(says(faced.portsSaid, "half of it on M1's top face") && says(faced.portsSaid, "half on their top faces"),
                     qPrintable(faced.portsSaid.join(" | ")));
            // Its feed's fences found as the layout has them, its walls no
            // via of it: the strips of a calibration copy them.
            QVERIFY2(faced.feeds.at(0).unlike.isEmpty() && faced.feeds.at(0).vias.size() == low.feeds.at(0).vias.size()
                         && !faced.feeds.at(0).vias.isEmpty(),
                     qPrintable(QStringLiteral("%1 via(s), %2; as a sheet %3").arg(faced.feeds.at(0).vias.size())
                                    .arg(faced.feeds.at(0).unlike).arg(low.feeds.at(0).vias.size())));
            QVERIFY(!says(low.portsSaid, "top face"));
            QVERIFY2(faced.asymmetry < 1e-9 && faced.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(faced.asymmetry).arg(faced.gain)));
            QCOMPARE(faced.geometry.size(), low.geometry.size());
            double s2 = 0, u2 = 0, l2 = 0;
            for (int r = 0; r < faced.geometry.size(); ++r) {
                const rf::Filament& f = faced.geometry.at(r);
                const int a = faced.ends.at(r).first, b = faced.ends.at(r).second;
                if (f.z0 != f.z1 || onFace(a) == onFace(b)) continue;
                const double i = (onFace(a) ? 1 : -1) * faced.currents.at(0).at(0).at(r).real();
                const double y = mesh.cells.at(onFace(a) ? a : b).centre.y();
                if (std::abs(y) < 50) s2 += i;
                else if (y > 100 && y < 350) u2 += i;
                else if (y < -100 && y > -350) l2 += i;
            }
            QVERIFY2(s2 > 0.8 && s2 < 1.05 && u2 < -0.2 && std::abs(u2 - l2) < 1e-3 * std::abs(u2),
                     qPrintable(QStringLiteral("%1 %2 %3").arg(s2).arg(u2).arg(l2)));
        }
        // A ground on one side only is a ground-signal probe's.
        const QList<rf::Conductor> gs = {cpw.at(0), cpw.at(1)};
        const rf::FullWave oneSide = rf::fullWaveOf(meshed(gs, 3, 1e-3), gs, grounded(9.8, 200e-6), beside, {10e9});
        QVERIFY2(oneSide.ok(), qPrintable(why(oneSide)));
        QVERIFY2(says(oneSide.portsSaid, "on one side"), qPrintable(oneSide.portsSaid.join(" | ")));
        // And none beside it is said, not solved as something else.
        const QList<rf::Conductor> alone = {cpw.at(0)};
        const rf::FullWave none = rf::fullWaveOf(meshed(alone, 3, 1e-3), alone, grounded(9.8, 200e-6), beside, {10e9});
        QVERIFY(!none.ok());
        QVERIFY2(why(none).contains(QStringLiteral("none either side")), qPrintable(why(none)));
    }

    // An internal port over a rectangle (the ports placed by hand, 1
    // November: a rectangle drawn on a metal): every cell of its metal the
    // rectangle overlaps driven together against the ground, by its share
    // of the area, as an edge port's face is shared out. Over a line's end,
    // the whole width and no deeper than its last cells, it drives what the
    // edge port there does, and is that port to the last digit - where the
    // one cell under a point is not (0.0013 off in S11 at 20 GHz, 0.0025 in
    // S21). A rectangle over no cell of its metal is said.
    void anInternalPortOverARectangleDrivesItsCells()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> line = {strip(0, 0, 2000, 100)};
        const rf::Mesh mesh = meshed(line, 3, 100e-6);
        const QList<double> fs = {20e9};
        const rf::FullWave edges = rf::fullWaveOf(mesh, line, stack, {edgePort(QPointF(0, 50)), edgePort(QPointF(2000, 50))}, fs);
        rf::FullWavePort near = internalPort(QPointF(25, 50), QStringLiteral("M1")), far = internalPort(QPointF(1975, 50), QStringLiteral("M1"));
        const rf::FullWave points = rf::fullWaveOf(mesh, line, stack, {near, far}, fs);
        near.size = far.size = QSizeF(50, 100);
        const rf::FullWave boxes = rf::fullWaveOf(mesh, line, stack, {near, far}, fs);
        QVERIFY2(edges.ok() && points.ok() && boxes.ok(), qPrintable(why(edges) + why(points) + why(boxes)));
        QVERIFY2(says(boxes.portsSaid, "drives the 5 cell(s) of M1 under its 50 by 100") && says(boxes.portsSaid, "rectangle at 25, 50"),
                 qPrintable(boxes.portsSaid.join(" | ")));
        QVERIFY2(says(points.portsSaid, "drives the cell at 25, 50"), qPrintable(points.portsSaid.join(" | ")));
        QVERIFY(boxes.asymmetry < 1e-9 && boxes.gain < 1e-6);
        const Complex e21 = edges.s.at(0).at(1).at(0), b21 = boxes.s.at(0).at(1).at(0), p21 = points.s.at(0).at(1).at(0);
        const Complex e11 = edges.s.at(0).at(0).at(0), b11 = boxes.s.at(0).at(0).at(0), p11 = points.s.at(0).at(0).at(0);
        QVERIFY2(std::abs(b11 - e11) < 1e-9 && std::abs(b21 - e21) < 1e-9 && std::abs(p11 - e11) > 3e-4 && std::abs(p21 - e21) > 3e-4,
                 qPrintable(QStringLiteral("off the edge ports': as a rectangle S11 %1, S21 %2; as a point %3, %4")
                                .arg(std::abs(b11 - e11)).arg(std::abs(b21 - e21)).arg(std::abs(p11 - e11)).arg(std::abs(p21 - e21))));
        // Over nothing of its metal: said.
        rf::FullWavePort off = internalPort(QPointF(1000, 400), QStringLiteral("M1"));
        off.size = QSizeF(40, 40);
        const rf::FullWave none = rf::fullWaveOf(mesh, line, stack, {off}, fs);
        QVERIFY(!none.ok());
        QVERIFY2(none.problems.join(' ').contains(QStringLiteral("rectangle at 1000, 400 µm is over no cell of M1")),
                 qPrintable(why(none)));
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
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, QList<QPointF>(), {1e9}), "port"));
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
        // More runs and cells than this build sets up a dense matrix for,
        // said and not solved: six thousand at most since a metal can be
        // solved as its two faces, which doubles its cells (the recheck of
        // 75ff6f9).
        const QList<rf::Conductor> square{strip(0, 0, 2000, 2000)};
        const rf::FullWave big = rf::fullWaveOf(meshed(square, 0, 25e-6, 1), square, stack, {QPointF(0, 1000)}, {1e9});
        QVERIFY(big.tooLarge);
        QVERIFY2(refused(big, "(6000 in all)"), qPrintable(why(big)));
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
        const rf::FullWave joined = rf::fullWaveOf(meshed(stacked, 4, 50e-6), stacked, two, ports, {1e9});
        QVERIFY2(joined.ok(), qPrintable(why(joined)));
        QVERIFY2(says(joined.lines, "are vias"), qPrintable(joined.lines.join(" | ")));
        // A via over no metal it joins is left out, and said.
        rf::Conductor stray = strip(900, 0, 50, 50);
        stray.layer = QStringLiteral("V1");
        const QList<rf::Conductor> astray{strip(0, 0, 400, 100), upper, joint, stray};
        const rf::FullWave left = rf::fullWaveOf(meshed(astray, 4, 50e-6), astray, two, ports, {1e9});
        QVERIFY2(left.ok(), qPrintable(why(left)));
        QVERIFY2(says(left.lines, "land on no metal"), qPrintable(left.lines.join(" | ")));
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
