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
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QScopeGuard>

#include "rf25/rf25_dense.h"
#include "rf25/rf25_fast.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_layout.h"
#include "rf25/rf25_static.h"
#include "multiphysics/fem_sparse.h"

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

/// The dense LU's overrides (rf25/rf25_dense.h: QUCS_RF25_LU, and
/// QUCS_FEM_SOLVER=eigen) away while a test chooses its LU, and back after.
struct LuOverridesAway {
    const bool hadLu = qEnvironmentVariableIsSet("QUCS_RF25_LU"), hadFem = qEnvironmentVariableIsSet("QUCS_FEM_SOLVER");
    const QByteArray lu = qgetenv("QUCS_RF25_LU"), fem = qgetenv("QUCS_FEM_SOLVER");
    LuOverridesAway()
    {
        qunsetenv("QUCS_RF25_LU");
        qunsetenv("QUCS_FEM_SOLVER");
    }
    ~LuOverridesAway()
    {
        rf::setDenseLu(rf::DenseLu::Automatic);
        if (hadLu) qputenv("QUCS_RF25_LU", lu);
        if (hadFem) qputenv("QUCS_FEM_SOLVER", fem);
    }
};

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

/// The rf_benchmark's GSSG, \a l µm long along x: signals of 50 µm 50 µm
/// apart, gaps of 35 µm and grounds of 300 - S1 above x's axis, S2 below.
QList<rf::Conductor> gssg(double l)
{
    return {strip(0, 25, l, 50), strip(0, -75, l, 50), strip(0, 110, l, 300), strip(0, -410, l, 300)};
}

/// Its four coplanar ports, S1's and S2's at each end, P1 and P2 at x = 0
/// - each pair a differential one, where \a paired.
QList<rf::FullWavePort> gssgPorts(double l, bool paired = true)
{
    const QString m1 = QStringLiteral("M1");
    QList<rf::FullWavePort> p = {edgePort(QPointF(0, 50), m1, QPointF(-1, 0)), edgePort(QPointF(0, -50), m1, QPointF(-1, 0)),
                                 edgePort(QPointF(l, 50), m1, QPointF(1, 0)), edgePort(QPointF(l, -50), m1, QPointF(1, 0))};
    for (int i = 0; i < p.size(); ++i) {
        p[i].name = QStringLiteral("P%1").arg(i + 1);
        p[i].coplanar = true;
    }
    if (paired) {
        p[0].pair = QStringLiteral("P2");
        p[2].pair = QStringLiteral("P4");
    }
    return p;
}

/// The two-port of modes \a i and \a j of frequency \a k of a solve's
/// mixed-mode S (mixedModeOf()'s order), as a chain matrix: its S into
/// \a reference, the modes', made Z.
Matrix2 modeChain(const rf::FullWave& fw, int k, int i, int j, double reference)
{
    const QList<QList<Complex>> m = rf::mixedModeOf(fw, k);
    Matrix2 s;
    s << m[i][i], m[i][j], m[j][i], m[j][j];
    const Matrix2 one = Matrix2::Identity();
    return chainOf(reference * (one + s) * (one - s).inverse());
}

/// Int_a0^a1 Int_b0^b1 dz dz' / sqrt(rho^2 + (z - z')^2): two lines along z,
/// in closed form - a via's partial inductance over mu0/(4 pi).
double linePair(double a0, double a1, double b0, double b1, double rho)
{
    const auto f = [rho](double s) { return s * std::asinh(s / rho) - std::sqrt(s * s + rho * rho); };
    return f(a1 - b0) - f(a1 - b1) - f(a0 - b0) + f(a0 - b1);
}

/// The MOM comb of theSideWallsChargeIsInAThickCombsCapacitance and the two
/// after it: 12 fingers of 1 µm in 1 µm gaps between two bars, in oxide 4 µm
/// over the ground; its metal \a t thick, solved as its two faces or as a
/// sheet.
rf::Stack combStack(double t, bool faces)
{
    rf::Stack s;
    rf::Layer ground;
    ground.kind = rf::Layer::Ground;
    ground.name = QStringLiteral("GND");
    ground.tag = QStringLiteral("gnd");
    rf::Layer oxide;
    oxide.kind = rf::Layer::Substrate;
    oxide.name = QStringLiteral("OX");
    oxide.tag = QStringLiteral("ox");
    oxide.thickness = 4e-6;
    oxide.epsr = 4.1;
    rf::Layer metal;
    metal.kind = rf::Layer::Metal;
    metal.name = QStringLiteral("M1");
    metal.tag = QStringLiteral("m1");
    metal.thickness = t;
    metal.sigma = 1e12;
    metal.twoSheets = faces;
    rf::Layer above = oxide;
    above.kind = rf::Layer::Dielectric;
    above.name = QStringLiteral("OX2");
    above.tag = QStringLiteral("ox2");
    above.thickness = 4e-6 - t;
    s.layers << ground << oxide << metal << above;
    return s;
}

QList<rf::Conductor> momComb()
{
    QList<rf::Conductor> c = {strip(0, -3, 23, 3), strip(0, 42, 23, 3)};
    for (int i = 0; i < 12; ++i) c << (i % 2 == 0 ? strip(2 * i, 0, 1, 41) : strip(2 * i, 1, 1, 41));
    return c;
}

QList<rf::FullWavePort> combPorts()
{
    return {edgePort(QPointF(11.5, -3), QString(), QPointF(0, -1)), edgePort(QPointF(11.5, 45), QString(), QPointF(0, 1))};
}

/// The comb's capacitance at frequency \a k of \a w, \a f.
double combCapacitance(const rf::FullWave& w, int k, double f)
{
    const Matrix2 y = zOf(w, k).inverse();
    return -y(0, 1).imag() / (2 * M_PI * f);
}

} // namespace

class TestRf25Fullwave : public QObject
{
    Q_OBJECT

private slots:
    // Its functions in two ctest entries on a sanitized build - each its own
    // time limit, the two at once: QUCS_TEST_PART 1 those before
    // aViaBetweenTwoMetalsJoinsThem, 2 it and those after, unset all (G9's
    // ports' paths, a via's vertical kernels in every solve, took the one
    // entry to 1150 s sanitized here: the two some 550 and 600).
    void init()
    {
        const QByteArray part = qgetenv("QUCS_TEST_PART");
        if (part.isEmpty()) return;
        const QMetaObject* mo = metaObject();
        int split = -1, here = -1;
        for (int k = mo->methodOffset(); k < mo->methodCount(); ++k) {
            const QByteArray name = mo->method(k).name();
            if (name == "aViaBetweenTwoMetalsJoinsThem") split = k;
            if (name == QTest::currentTestFunction()) here = k;
        }
        if (split < 0 || here < 0) return;
        if ((part == "2") != (here >= split)) QSKIP("in the test's other part (QUCS_TEST_PART)");
    }

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

    // A frequency's fill shared out among the threads the frequencies at a
    // time leave it - its Green's functions' tables a distance to a thread,
    // its system's columns and P's - is the same to the last digit as on
    // one thread: a copper line solved as its two faces (each face's twin,
    // the walls' vias between them), shorted to the ground by a via at its
    // far end, driven at its edge and at its middle (the ports' paths up
    // from the ground), its runs along it and across it. (A sanitized build
    // solves it as a sheet, at one frequency, on one thread and four, and
    // leaves out the compressed solve: the faces' three pairs of heights and
    // two vias' kernels, made afresh for each solve, were 101 s of it here.)
    void aFrequencysFillIsTheSameOnAnyNumberOfThreads()
    {
        const bool slow = slowBuild();
        rf::Stack stack = withVia(grounded(9.8, 100e-6, 0, 3e-6, 5.8e7), QStringLiteral("V0"), QStringLiteral("ground"),
                                  QStringLiteral("M1"));
        stack.layers[2].twoSheets = !slow;
        const QList<rf::Conductor> shorted = {strip(0, 0, 1000, 100), on(QStringLiteral("V0"), 900, 0, 100, 100)};
        const rf::Mesh mesh = meshed(shorted, 2, 200e-6);
        const QList<rf::FullWavePort> ports{edgePort(QPointF(0, 50)), internalPort(QPointF(500, 50), QStringLiteral("M1"))};
        const QList<double> fs = slow ? QList<double>{6e9} : QList<double>{2e9, 9e9};
        const auto solve = [&mesh, &shorted, &stack, &ports, &fs](int threads) {
            // (Its Green's functions made afresh, as many a solve's first
            //  frequencies' are: kept from the solve before, each is made
            //  sooner than a thread starts.)
            rf::forgetGreens();
            rf::FullWaveControl control;
            control.threads = threads;
            return rf::fullWaveOf(mesh, shorted, stack, ports, fs, 1e-6, &control);
        };
        const rf::FillShared before = rf::fillShared();
        const rf::FullWave alone = solve(1);
        QVERIFY2(alone.ok(), qPrintable(why(alone)));
        QCOMPARE(alone.threads, 1);
        QCOMPARE(rf::fillShared().greens, before.greens);
        QCOMPARE(rf::fillShared().columns, before.columns);
        QVERIFY2(!alone.compressed && (slow || says(alone.lines, "walls of the metals solved as their two faces"))
                     && says(alone.lines, "of them to the ground") && says(alone.lines, "path(s) of their own"),
                 qPrintable(alone.lines.join(" | ")));
        // On four threads, each frequency its own share of them, and on
        // seven - more than share out evenly.
        for (const int threads : slow ? QList<int>{4} : QList<int>{4, 7}) {
            const rf::FillShared was = rf::fillShared();
            const rf::FullWave shared = solve(threads);
            QVERIFY2(shared.ok(), qPrintable(why(shared)));
            const int side = int(fs.size());
            QCOMPARE(shared.threads, side * (threads / side));
            QVERIFY2(rf::fillShared().greens > was.greens && rf::fillShared().columns > was.columns,
                     qPrintable(QStringLiteral("%1 of the tables' distances and %2 columns made on other threads")
                                    .arg(rf::fillShared().greens - was.greens).arg(rf::fillShared().columns - was.columns)));
            QVERIFY2(shared.z == alone.z && shared.s == alone.s, qPrintable(QStringLiteral("on %1 threads").arg(threads)));
            QVERIFY2(shared.currents == alone.currents, qPrintable(QStringLiteral("on %1 threads").arg(threads)));
        }
        // What cannot be made is said as on one thread: of a metal right on
        // the ground plane, the first of its pairs of heights that cannot,
        // and once.
        rf::Stack low = grounded(9.8, 100e-6);
        rf::Layer m0 = low.layers.at(2);
        m0.name = QStringLiteral("M0");
        m0.tag = QStringLiteral("m0");
        low.layers.insert(1, m0);
        const QList<rf::Conductor> two = {on(QStringLiteral("M0"), 0, 0, 1000, 100), strip(0, 200, 1000, 100)};
        QStringList said;
        for (const int threads : {1, 4}) {
            rf::FullWaveControl control;
            control.threads = threads;
            const rf::FullWave fw = rf::fullWaveOf(meshed(two, 2, 200e-6), two, low, {edgePort(QPointF(0, 250), QStringLiteral("M1"))},
                                                   {1e9}, 1e-6, &control);
            QVERIFY2(!fw.ok() && fw.problems.size() == 1 && says(fw.problems, "shorted to it"), qPrintable(why(fw)));
            if (threads == 1)
                said = fw.problems;
            else
                QCOMPARE(fw.problems, said);
        }
        // Held compressed, its Green's functions shared out alike, and its S
        // the dense one's. (Its products are summed a thread at a time, the
        // last digits as the threads fall.)
        if (slow) return;
        rf::forgetGreens();
        rf::FullWaveControl held;
        held.threads = 4;
        held.matrix = rf::FullWaveControl::Matrix::Compressed;
        const rf::FillShared was = rf::fillShared();
        const rf::FullWave compressed = rf::fullWaveOf(mesh, shorted, stack, ports, fs, 1e-6, &held);
        QVERIFY2(compressed.ok() && compressed.compressed, qPrintable(why(compressed)));
        QVERIFY(rf::fillShared().greens > was.greens);
        QCOMPARE(rf::fillShared().columns, was.columns);
        double apart = 0;
        for (int k = 0; k < fs.size(); ++k)
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) apart = std::max(apart, std::abs(compressed.s.at(k).at(i).at(j) - alone.s.at(k).at(i).at(j)));
        QVERIFY2(apart < 1e-5, qPrintable(QStringLiteral("S %1 from the dense solve's").arg(apart)));
    }

    // The static parts a dense solve makes once - each two runs' aligned
    // partial inductances, direct and of the images, and each two cells'
    // static potentials -, shared out among all of its threads before its
    // frequencies, are the same to the last digit as on one thread, and a
    // compressed solve makes none: the compressed solve's strip at a slant,
    // whose runs between cells cut to its outline are oblique and whose
    // cells differ in size - 517 runs over 306 cells, their static parts
    // some 50 ms on one thread here, so that the other threads take their
    // share however late a loaded machine starts them (the copper line of
    // the test above has 1.6 ms of them).
    void theStaticPartsAreTheSameOnAnyNumberOfThreads()
    {
        const QString m1 = QStringLiteral("M1");
        rf::Conductor slant;
        slant.layer = m1;
        slant.outline = QPolygonF({QPointF(0, 0), QPointF(1500, 866), QPointF(1500, 966), QPointF(0, 100)});
        const QList<rf::Conductor> one{slant};
        const rf::Mesh mesh = meshed(one, 3, 100e-6);
        const rf::Stack stack = grounded(9.8, 100e-6, 0, 2e-6, 5.8e7);
        const QList<rf::FullWavePort> ports{edgePort(QPointF(0, 50), m1, QPointF(-1, 0)),
                                            edgePort(QPointF(1500, 916), m1, QPointF(1, 0))};
        const auto solve = [&mesh, &one, &stack, &ports](int threads, rf::FullWaveControl::Matrix matrix) {
            rf::FullWaveControl control;
            control.threads = threads;
            control.matrix = matrix;
            return rf::fullWaveOf(mesh, one, stack, ports, {5e9}, 1e-6, &control);
        };
        const rf::FillShared before = rf::fillShared();
        const rf::FullWave alone = solve(1, rf::FullWaveControl::Matrix::Dense);
        QVERIFY2(alone.ok() && !alone.compressed, qPrintable(why(alone)));
        QCOMPARE(rf::fillShared().statics, before.statics);
        int oblique = 0;
        for (const rf::Filament& f : alone.geometry)
            oblique += std::abs(f.to.x() - f.from.x()) > 1e-12 && std::abs(f.to.y() - f.from.y()) > 1e-12 ? 1 : 0;
        QVERIFY2(oblique > 10, qPrintable(QString::number(oblique)));
        const rf::FillShared was = rf::fillShared();
        const rf::FullWave shared = solve(4, rf::FullWaveControl::Matrix::Dense);
        QVERIFY2(shared.ok(), qPrintable(why(shared)));
        QCOMPARE(shared.threads, 4);
        const long long others = rf::fillShared().statics - was.statics;
        qWarning("%d runs (%d oblique) over %d cells: %lld of the static parts' columns and blocks made on other threads",
                 alone.runs, oblique, alone.cells, others);
        QVERIFY(others > 0);
        QVERIFY(shared.z == alone.z && shared.s == alone.s && shared.currents == alone.currents);
        const rf::FillShared held = rf::fillShared();
        const rf::FullWave compressed = solve(4, rf::FullWaveControl::Matrix::Compressed);
        QVERIFY2(compressed.ok() && compressed.compressed, qPrintable(why(compressed)));
        QCOMPARE(rf::fillShared().statics, held.statics);
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

    // A via to the ground at the far end of a line, and an internal port's
    // own path to the same cells shorted - a port's path up from the ground
    // has a field, as a via has (G9 of the phase E check, addPortPaths()).
    // Over the via's own square the port's path is the via: its span, its
    // place at the centre of the cell it lands on, its cross-section; the
    // line shorted by either reads the same. Over the whole cell under a
    // point the path is broader, and the line shorted by the via less by it
    // is the two's partial inductances and their images' in closed form,
    // each cross-section's mean distance with itself standing in for its
    // breadth. Through 200 µm of 9.8 the same, as magnetostatics says: the
    // dielectric's part of G_zz, the potential along the runs and at the
    // port, and the line's G_zx along it all cancel. With the via over one
    // cell, and over two - a pair of vias side by side.
    void aViaToTheGroundIsItsOwnInductance()
    {
        const double h = 200e-6, w = 50, l = 2000, f = 1e8, omega = 2 * M_PI * f;
        // (Rosa's mean distance of a rectangle with itself, 0.2235 (a + b).)
        const auto closed = [h](const QRectF& box) {
            const double g = 0.2235 * (box.width() + box.height()) * 1e-6;
            return kMu0 / (4 * M_PI) * (linePair(0, h, 0, h, g) + linePair(0, h, -h, 0, g));
        };
        for (const int across : {1, 2}) {
            QList<double> broader;
            QRectF cell, part;   // (the last cell under the via, and the via's part of it)
            for (const double epsr : {1.0, 9.8}) {
                const rf::Stack stack = withVia(grounded(epsr, h), QStringLiteral("V0"), QStringLiteral("ground"), QStringLiteral("M1"));
                const QList<rf::Conductor> line = {strip(0, 0, l, w)};
                const QList<rf::Conductor> shorted = {strip(0, 0, l, w), on(QStringLiteral("V0"), l - w, 0, w, w)};
                const rf::Mesh meshA = evenly(shorted, across, 400e-6), meshB = evenly(line, across, 400e-6);
                QList<rf::FullWavePort> points = {edgePort(QPointF(0, w / 2))}, squares = points;
                for (const rf::MeshCell& c : meshB.cells)
                    if (c.outline.boundingRect().intersects(QRectF(l - w, 0, w, w))) {
                        points << internalPort(c.centre, QStringLiteral("M1"));
                        cell = c.outline.boundingRect();
                        part = cell.intersected(QRectF(l - w, 0, w, w));
                        rf::FullWavePort over = internalPort(part.center(), QStringLiteral("M1"));
                        over.size = part.size();
                        squares << over;
                    }
                QCOMPARE(points.size(), 1 + across);
                const rf::FullWave a = rf::fullWaveOf(meshA, shorted, stack, {edgePort(QPointF(0, w / 2))}, {f});
                const rf::FullWave b = rf::fullWaveOf(meshB, line, stack, points, {f});
                const rf::FullWave c = rf::fullWaveOf(meshB, line, stack, squares, {f});
                QVERIFY2(a.ok() && b.ok() && c.ok(), qPrintable(why(a) + why(b) + why(c)));
                QVERIFY2(says(a.lines, qPrintable(QStringLiteral("%1 of the runs are vias, %1 of them to the ground").arg(across))),
                         qPrintable(a.lines.join(" | ")));
                const Complex za = a.z.at(0).at(0).at(0);
                const double asVia = (za - 1.0 / keptAdmittance(c, 0, 1)(0, 0)).imag() / omega;
                broader << (za - 1.0 / keptAdmittance(b, 0, 1)(0, 0)).imag() / omega;
                // The port's path over the via's square is the via.
                QVERIFY2(std::abs(asVia) < 1e-4 * closed(part),
                         qPrintable(QStringLiteral("%1 nH off the via's %2").arg(asVia * 1e9).arg(closed(part) * 1e9)));
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
                qWarning().noquote() << QStringLiteral("%1 across, eps_r %2: the via less the port's path over its square %3 nH; "
                                                       "over the cell %4 nH, of the two closed forms %5")
                                            .arg(across).arg(epsr).arg(asVia * 1e9, 0, 'e', 2).arg(broader.last() * 1e9, 0, 'f', 6)
                                            .arg((closed(part) - closed(cell)) * 1e9, 0, 'f', 6);
            }
            // (Over two cells a pair side by side, coupled as near ones are:
            //  measured 1.2 times half the one's, the vias' squares and the
            //  paths' not alike near.)
            const double one = closed(part) - closed(cell);
            if (across == 1) QVERIFY2(std::abs(broader.at(0) / one - 1) < 1e-3, "in vacuum, the closed forms");
            else QVERIFY2(broader.at(0) / (0.5 * one) > 0.7 && broader.at(0) / (0.5 * one) < 1.3, "a pair side by side");
            // (Measured 5e-3: 0.17 pH of the 72 the broader path has - the
            //  dielectric's part of a path's G_zz cancels to a part in a
            //  thousand of the whole, its potential taken across its mean
            //  distance, here 2.5 times the via's.)
            QVERIFY2(std::abs(broader.at(1) / broader.at(0) - 1) < 1e-2, "the dielectric changes nothing");
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

    // A via between two metals in a line that climbs from M1 to M2, and an
    // internal port from M1 to M2 joining the same lines, shorted - the
    // port's path from M1 to M2 has a field, as the via has (G9 of the phase
    // E check, addPortPaths()). Over the via's own square, the cell under
    // its point, the path is the via, and the two-port's series inductance
    // at a low frequency (-1/Y12) reads the same either way; over a square
    // 20 µm across, a narrower path, the via less it is the two's partial
    // inductances and their images' in closed form, through 100 µm of 4 on
    // 100 µm of 9.8 as in vacuum. The two-port is reciprocal and passive -
    // in vacuum too, where a port's path without a field had S give back
    // 2e-5 of a wave - and joined: the line's wave passes.
    void aViaBetweenTwoMetalsJoinsThem()
    {
        // (Sanitized, five solves where there are eleven: the line at its low
        //  frequency alone, the joint over the via's own square in vacuum
        //  alone and never driven the other way round, and the thick metal
        //  not at all. Each frequency here builds the horizontal kernels of
        //  two heights and the vertical ones of three spans - the via's and
        //  the two ports' paths' - some 38 s under the sanitizers, and the
        //  eleven ran past QtTest's 300.)
        const bool slow = slowBuild();
        const double h1 = 100e-6, h2 = 100e-6, w = 50, f = 1e8, omega = 2 * M_PI * f;
        // (A square's: Rosa's mean distance 0.2235 (a + b), from height lo.)
        const auto closed = [h2](double side, double lo) {
            const double g = 0.2235 * 2 * side * 1e-6;
            return kMu0 / (4 * M_PI) * (linePair(lo, lo + h2, lo, lo + h2, g) + linePair(lo, lo + h2, -lo - h2, -lo, g));
        };
        const auto joints = [w](const QList<rf::FullWavePort>& ends, double side, bool down = false) {
            rf::FullWavePort joint = internalPort(QPointF(1000 - w / 2, w / 2), QStringLiteral("M2"), QStringLiteral("M1"));
            if (down) std::swap(joint.metal, joint.from);
            if (side > 0) joint.size = QSizeF(side, side);
            return QList<rf::FullWavePort>{ends.at(0), ends.at(1), joint};
        };
        QList<double> narrower;
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
            const QList<double> fs = slow ? QList<double>{f} : QList<double>{f, 5e9, 20e9};
            const rf::FullWave a = rf::fullWaveOf(evenly(climbing, 1, 400e-6, 1), climbing, two, ends, fs);
            const rf::FullWave d = rf::fullWaveOf(evenly(lines, 1, 400e-6, 1), lines, two, joints(ends, 20), {f});
            QVERIFY2(a.ok() && d.ok(), qPrintable(why(a) + why(d)));
            QVERIFY2(says(a.lines, "1 of the runs are vias, 0 of them to the ground"), qPrintable(a.lines.join(" | ")));
            const Eigen::MatrixXcd ya = keptAdmittance(a, 0, 2), yd = keptAdmittance(d, 0, 2);
            narrower << (-1.0 / ya(0, 1) + 1.0 / yd(0, 1)).imag() / omega;
            QString asVia = QStringLiteral("not solved");
            if (!slow || vacuum) {
                const rf::FullWave b = rf::fullWaveOf(evenly(lines, 1, 400e-6, 1), lines, two, joints(ends, 0), {f});
                QVERIFY2(b.ok(), qPrintable(why(b)));
                const double off = (-1.0 / ya(0, 1) + 1.0 / keptAdmittance(b, 0, 2)(0, 1)).imag() / omega;
                QVERIFY2(std::abs(off) < 1e-4 * closed(w, h1),
                         qPrintable(QStringLiteral("%1 nH off the via's %2").arg(off * 1e9).arg(closed(w, h1) * 1e9)));
                asVia = QStringLiteral("%1 nH").arg(off * 1e9, 0, 'e', 2);
            }
            if (!slow) {
                // (Driven the other way round, to M1 from M2, its path's
                //  current comes down: the same short.)
                const rf::FullWave e = rf::fullWaveOf(evenly(lines, 1, 400e-6, 1), lines, two, joints(ends, 20, true), {f});
                QVERIFY2(e.ok(), qPrintable(why(e)));
                const Complex one = 1.0 / yd(0, 1), other = 1.0 / keptAdmittance(e, 0, 2)(0, 1);
                QVERIFY2(std::abs(one - other) < 1e-9 * std::abs(one),
                         qPrintable(QStringLiteral("%1 Ohm one way round, %2 the other").arg(std::abs(one)).arg(std::abs(other))));
            }
            QVERIFY2(a.asymmetry < 1e-10 && a.gain <= 1e-12, qPrintable(QStringLiteral("asymmetry %1, gain %2").arg(a.asymmetry).arg(a.gain)));
            QVERIFY2(std::abs(a.s.at(0).at(1).at(0)) > 0.9,
                     qPrintable(QStringLiteral("|S21| %1 at 100 MHz").arg(std::abs(a.s.at(0).at(1).at(0)))));
            QString passes;
            for (int k = 0; k < fs.size(); ++k)
                passes += QStringLiteral(" %1 at %2 GHz").arg(std::abs(a.s.at(k).at(1).at(0)), 0, 'f', 4).arg(fs.at(k) / 1e9);
            qWarning().noquote() << QStringLiteral("%1: the via less the port's path over its square %2; over 20 µm %3 nH, of the "
                                                   "two closed forms %4; |S21|%5")
                                        .arg(vacuum ? QStringLiteral("vacuum") : QStringLiteral("9.8 and 4"))
                                        .arg(asVia).arg(narrower.last() * 1e9, 0, 'f', 6)
                                        .arg((closed(w, h1) - closed(20, h1)) * 1e9, 0, 'f', 6).arg(passes);
        }
        QVERIFY2(std::abs(narrower.at(0) / (closed(w, h1) - closed(20, h1)) - 1) < 2e-3, "in vacuum, the closed forms");
        QVERIFY2(std::abs(narrower.at(1) / narrower.at(0) - 1) < 5e-3, "the dielectrics change nothing");

        if (!slow) {   // M1 2 µm thick and solved as its two faces (its "Solved as"):
            // the via leaves its top face, so it is 2 µm shorter than it
            // would be from its bottom, and an internal port between the two
            // metals stands on that top face too - its path the via over the
            // via's square, and over 20 µm the closed forms from there, in
            // vacuum.
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
            const rf::FullWave b = rf::fullWaveOf(evenly(lines, 1, 400e-6, 1), lines, two, joints(ends, 0), {f});
            const rf::FullWave d = rf::fullWaveOf(evenly(lines, 1, 400e-6, 1), lines, two, joints(ends, 20), {f});
            QVERIFY2(a.ok() && b.ok() && d.ok(), qPrintable(why(a) + why(b) + why(d)));
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
            const Eigen::MatrixXcd ya = keptAdmittance(a, 0, 2), yb = keptAdmittance(b, 0, 2), yd = keptAdmittance(d, 0, 2);
            const double asVia = (-1.0 / ya(0, 1) + 1.0 / yb(0, 1)).imag() / omega;
            const double less = (-1.0 / ya(0, 1) + 1.0 / yd(0, 1)).imag() / omega;
            const double top = h1 + t, fromTop = closed(w, top) - closed(20, top);
            qWarning("from M1's top face: the via less the port's path over its square %.2e nH; over 20 µm %.6f nH, of the "
                     "closed forms from there %.6f (%.2e)",
                     asVia * 1e9, less * 1e9, fromTop * 1e9, less / fromTop - 1);
            QVERIFY2(std::abs(asVia) < 1e-4 * closed(w, top), qPrintable(QStringLiteral("%1 nH").arg(asVia * 1e9)));
            QVERIFY2(std::abs(less / fromTop - 1) < 2e-2, qPrintable(QStringLiteral("%1 nH, %2").arg(less * 1e9).arg(fromTop * 1e9)));
            QVERIFY2(a.asymmetry < 1e-10, qPrintable(QStringLiteral("asymmetry %1").arg(a.asymmetry)));
        }
    }

    // A grounded coplanar line driven by edge ports that return through the
    // conductors either side of it on the line of its edge, as a ground-
    // signal-ground probe's do, or through the ground plane: 100 µm on 200
    // µm of 9.8, 50 µm slots, 250 µm grounds with a fence of vias down each.
    // The phase C check of 1 November (F6) had a millimetre of it lose a
    // third of the power at 60 GHz through the ground and less as the line
    // was longer - at the ends - which was the ports' own paths up from the
    // ground, without a field then (G9 of the phase E check, addPortPaths()):
    // with theirs the two returns lose alike, under a percent.
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
        QVERIFY2(lost(viaBeside) < 0.02 && lost(viaGround) < 0.02 && lost(viaGround) > 0,
                 qPrintable(QStringLiteral("%1 through the ground, %2 beside").arg(lost(viaGround)).arg(lost(viaBeside))));
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
    // edge port there does; its path up from the ground is a column as deep
    // as the cells where the edge port's is a ribbon along the face (G9 of
    // the phase E check, addPortPaths()), and it is within a few thousandths
    // of that port at 20 GHz - nearer than the one cell under a point. A
    // sliver 2 µm deep along the face, its path all but the ribbon, is four
    // times the nearer: the edge port's path is as wide as its face and no
    // deeper, whatever length its cells have along the line. (With no field
    // on either path the rectangle was the edge port to the last digit.) A
    // rectangle over no cell of its metal is said.
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
        rf::FullWavePort nearSliver = near, farSliver = far;
        nearSliver.at = QPointF(1, 50);
        farSliver.at = QPointF(1999, 50);
        nearSliver.size = farSliver.size = QSizeF(2, 100);
        const rf::FullWave slivers = rf::fullWaveOf(mesh, line, stack, {nearSliver, farSliver}, fs);
        QVERIFY2(edges.ok() && points.ok() && boxes.ok() && slivers.ok(),
                 qPrintable(why(edges) + why(points) + why(boxes) + why(slivers)));
        QVERIFY2(says(slivers.portsSaid, "drives the 5 cell(s) of M1 under its 2 by 100"), qPrintable(slivers.portsSaid.join(" | ")));
        QVERIFY2(says(boxes.portsSaid, "drives the 5 cell(s) of M1 under its 50 by 100") && says(boxes.portsSaid, "rectangle at 25, 50"),
                 qPrintable(boxes.portsSaid.join(" | ")));
        QVERIFY2(says(points.portsSaid, "drives the cell at 25, 50"), qPrintable(points.portsSaid.join(" | ")));
        QVERIFY(boxes.asymmetry < 1e-9 && boxes.gain < 1e-6);
        const Complex e21 = edges.s.at(0).at(1).at(0), b21 = boxes.s.at(0).at(1).at(0), p21 = points.s.at(0).at(1).at(0);
        const Complex e11 = edges.s.at(0).at(0).at(0), b11 = boxes.s.at(0).at(0).at(0), p11 = points.s.at(0).at(0).at(0);
        const Complex s21 = slivers.s.at(0).at(1).at(0), s11 = slivers.s.at(0).at(0).at(0);
        QVERIFY2(std::abs(b11 - e11) < 5e-3 && std::abs(b21 - e21) < 8e-3 && std::abs(p11 - e11) > 2 * std::abs(b11 - e11)
                     && std::abs(p21 - e21) > 2 * std::abs(b21 - e21) && std::abs(s11 - e11) < 0.4 * std::abs(b11 - e11)
                     && std::abs(s21 - e21) < 0.4 * std::abs(b21 - e21),
                 qPrintable(QStringLiteral("off the edge ports': as a rectangle S11 %1, S21 %2; as a point %3, %4; as a sliver %5, %6")
                                .arg(std::abs(b11 - e11)).arg(std::abs(b21 - e21)).arg(std::abs(p11 - e11)).arg(std::abs(p21 - e21))
                                .arg(std::abs(s11 - e11)).arg(std::abs(s21 - e21))));
        // Over nothing of its metal: said.
        rf::FullWavePort off = internalPort(QPointF(1000, 400), QStringLiteral("M1"));
        off.size = QSizeF(40, 40);
        const rf::FullWave none = rf::fullWaveOf(mesh, line, stack, {off}, fs);
        QVERIFY(!none.ok());
        QVERIFY2(none.problems.join(' ').contains(QStringLiteral("rectangle at 1000, 400 µm is over no cell of M1")),
                 qPrintable(why(none)));
    }

    // A GSSG on 150 µm of InP with nothing under it (the GSSG review of 1
    // November, G4 and G5). A port on each signal returns through the two
    // grounds - the outermost of the four, there being no ground plane and
    // none marked - past the other signal, the grounds tied as a GSSG
    // probe's are; the two at each end a differential pair. With no plane
    // what goes into a port comes out of it, and the potential's nought, at
    // infinity, drops out: at 2 GHz each mode is the quasi-static
    // extraction's of the same mesh, the differential mode the odd mode's
    // and the common mode the even's - infinity floating there, as the
    // structure's charge is none in all here.
    void aGssgReturnsThroughItsGroundsWithNoGroundPlane()
    {
        rf::Stack open = grounded(12.4, 150e-6);
        open.layers[0].variant = QStringLiteral("open");
        const double l1 = 600, l2 = 1200;
        const QList<rf::Conductor> c1 = gssg(l1), c2 = gssg(l2);
        const rf::Mesh m1 = meshed(c1, 3, 100e-6), m2 = meshed(c2, 3, 100e-6);
        const rf::FullWave a = rf::fullWaveOf(m1, c1, open, gssgPorts(l1), {2e9});
        const rf::FullWave b = rf::fullWaveOf(m2, c2, open, gssgPorts(l2), {2e9});
        QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
        QVERIFY2(says(b.lines, "There is no ground plane") && says(b.lines, "the outermost across the line"),
                 qPrintable(b.lines.join(" | ")));
        // Each signal's port against both grounds, past the other signal -
        // whose strips its feed cannot be - and the grounds' names said.
        const QRegularExpression through(QStringLiteral("return through (M1[ #0-9]*) and (M1[ #0-9]*), the outermost"));
        const QRegularExpressionMatch grounds = through.match(b.lines.join(' '));
        QVERIFY2(grounds.hasMatch(), qPrintable(b.lines.join(" | ")));
        for (int p = 0; p < 4; ++p) {
            const QString& said = b.portsSaid.at(p);
            QVERIFY2(said.contains(QStringLiteral("which is no return")) && said.contains(grounds.captured(1))
                         && said.contains(grounds.captured(2)) && said.contains(QStringLiteral("either side")),
                     qPrintable(said));
            QVERIFY2(b.feeds.at(p).unlike.contains(QStringLiteral("no return, lies between it and its return")),
                     qPrintable(b.feeds.at(p).unlike));
        }
        QCOMPARE(b.pairs, (QList<QPair<int, int>>{qMakePair(0, 1), qMakePair(2, 3)}));
        QVERIFY2(says(b.lines, "Differential pairs, each its plus port first: P1 and P2; P3 and P4"), qPrintable(b.lines.join(" | ")));
        QVERIFY2(b.asymmetry < 1e-9 && b.gain <= 1e-6, qPrintable(QStringLiteral("%1 %2").arg(b.asymmetry).arg(b.gain)));
        // The modes: a symmetric GSSG turns none into the other, to the
        // mesh's own symmetry.
        QStringList order, of;
        const QList<QList<Complex>> modes = rf::mixedModeOf(b, 0, &order, &of);
        QCOMPARE(order, (QStringList{"D1", "D2", "C1", "C2"}));
        QCOMPARE(of, (QStringList{"P1 - P2", "P3 - P4", "P1 + P2", "P3 + P4"}));
        double converted = 0;
        for (int i = 0; i < 2; ++i)
            for (int j = 2; j < 4; ++j) converted = std::max({converted, std::abs(modes[i][j]), std::abs(modes[j][i])});
        QVERIFY2(converted < 1e-6, qPrintable(QString::number(converted)));
        // Each mode by the two lengths, against the extraction's: within
        // 1.5 %, the partial inductances' L under the charge's 1/(c0^2 C0)
        // as the existing line's is (n_eff and Z both 0.8 % under for the
        // odd mode, 1.2 % for the even, at three cells across; 1.1 and 1.3
        // at two, 0.6 and 1.0 at four).
        const rf::Extraction e = rf::extract(m2, c2, open);
        const rf::SignalLines lines = rf::signalsOf(e, l2 * 1e-6);
        QVERIFY2(lines.ok() && lines.nets.size() == 2, qPrintable(lines.problem));
        const double k0 = 2 * M_PI * 2e9 / kC0;
        for (const bool odd : {true, false}) {
            const double sign = odd ? -1 : 1;
            const double c = lines.c[0][0] + sign * lines.c[0][1], c0 = lines.c0[0][0] + sign * lines.c0[0][1];
            const double nStatic = std::sqrt(c / c0), zStatic = 1 / (kC0 * std::sqrt(c * c0));
            const int i = odd ? 0 : 2;
            const double reference = odd ? 100 : 25;
            const TwoLine two = twoLine(modeChain(a, 0, i, i + 1, reference), modeChain(b, 0, i, i + 1, reference), l1 * 1e-6,
                                        l2 * 1e-6, nStatic * k0);
            const double n = two.gamma.imag() / k0;
            // (The mode's own Z0 is the line's: the differential mode's is
            //  twice the odd mode's, the common mode's half the even's.)
            const double z = two.z0.real() * (odd ? 0.5 : 2.0);
            qWarning("%s mode: n_eff %.5f, extraction %.5f (%.2e); Z %.3f Ohm, extraction %.3f (%.2e)", odd ? "odd" : "even", n,
                     nStatic, n / nStatic - 1, z, zStatic, z / zStatic - 1);
            QVERIFY2(std::abs(n / nStatic - 1) < 0.015 && std::abs(z / zStatic - 1) < 0.015,
                     qPrintable(QStringLiteral("%1: n_eff %2 against %3, Z %4 against %5")
                                    .arg(odd ? "odd" : "even").arg(n).arg(nStatic).arg(z).arg(zStatic)));
        }
        // Driven against the ground there is none to return through, and
        // that is said rather than driven against infinity: an edge port of
        // the ground, an internal one from it, or a via down to it.
        QList<rf::FullWavePort> grounded = gssgPorts(l1, false);
        grounded[1].coplanar = false;
        const rf::FullWave edge = rf::fullWaveOf(m1, c1, open, grounded, {2e9});
        QVERIFY(!edge.ok());
        QVERIFY2(says(edge.problems, "There is no ground plane under the stack, and P2 return(s) through the ground"), qPrintable(why(edge)));
        const rf::FullWave inside = rf::fullWaveOf(m1, c1, open, {internalPort(QPointF(300, 50), QStringLiteral("M1"))}, {2e9});
        QVERIFY2(says(inside.problems, "drive an internal port from a metal"), qPrintable(why(inside)));
        // From a metal it is solved: a pair of strips one over the other,
        // driven between them at each end.
        rf::Stack pair = twoMetals();
        pair.layers[0].variant = QStringLiteral("open");
        const QList<rf::Conductor> broadside{on(QStringLiteral("M1"), 0, 0, 800, 100), on(QStringLiteral("M2"), 0, 0, 800, 100)};
        const rf::FullWave between = rf::fullWaveOf(meshed(broadside, 2, 100e-6), broadside, pair,
                                                    {internalPort(QPointF(25, 50), QStringLiteral("M2"), QStringLiteral("M1")),
                                                     internalPort(QPointF(775, 50), QStringLiteral("M2"), QStringLiteral("M1"))},
                                                    {2e9});
        QVERIFY2(between.ok() && between.asymmetry < 1e-9 && between.gain <= 1e-6, qPrintable(why(between)));
        rf::Stack down = withVia(open, QStringLiteral("V1"), QStringLiteral("ground"), QStringLiteral("M1"));
        QList<rf::Conductor> viaed = c1;
        viaed << on(QStringLiteral("V1"), 300, 200, 50, 50);
        const rf::FullWave via = rf::fullWaveOf(meshed(viaed, 2, 100e-6), viaed, down, gssgPorts(l1), {2e9});
        QVERIFY2(says(via.problems, "go down to the ground, and there is no ground plane"), qPrintable(why(via)));
    }

    // A differential pair, and the conductors a coplanar port returns
    // through named (the GSSG review's G5). The mixed-mode S is T S T',
    // T orthogonal: each pair's differential mode, then each common mode,
    // then the ports of no pair - as Touchstone 2.0's [Mixed-Mode Order]
    // writes it. A pair is checked as given; and over a ground plane, where
    // nothing makes the GSSG's grounds its returns, a port that would return
    // through the other signal is said, then returns through the grounds
    // once they are marked.
    void aDifferentialPairsModesAreItsMixedModeS()
    {
        rf::Stack open = grounded(12.4, 150e-6);
        open.layers[0].variant = QStringLiteral("open");
        const double l = 600;
        const QList<rf::Conductor> c = gssg(l);
        const rf::Mesh mesh = meshed(c, 2, 100e-6);
        QList<rf::FullWavePort> ports = gssgPorts(l);
        ports[1].pair = QStringLiteral("P1");   // (named from both ends: one pair)
        const rf::FullWave fw = rf::fullWaveOf(mesh, c, open, ports, {5e9, 10e9});
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        QCOMPARE(fw.pairs, (QList<QPair<int, int>>{qMakePair(0, 1), qMakePair(2, 3)}));
        // T S T' by hand, and power kept: T is orthogonal.
        const double h = 1 / std::sqrt(2.0);
        Eigen::MatrixXd t = Eigen::MatrixXd::Zero(4, 4);
        t(0, 0) = h, t(0, 1) = -h, t(1, 2) = h, t(1, 3) = -h, t(2, 0) = h, t(2, 1) = h, t(3, 2) = h, t(3, 3) = h;
        for (int k = 0; k < 2; ++k) {
            Eigen::MatrixXcd s(4, 4);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) s(i, j) = fw.s.at(k).at(i).at(j);
            const Eigen::MatrixXcd want = t * s * t.transpose();
            const QList<QList<Complex>> got = rf::mixedModeOf(fw, k);
            double off = 0;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) off = std::max(off, std::abs(got[i][j] - want(i, j)));
            QVERIFY2(off < 1e-12, qPrintable(QString::number(off)));
        }
        QVERIFY(rf::mixedModeOf(fw, 2).isEmpty());
        // A port of no pair stays as it is, after the modes.
        QList<rf::FullWavePort> three = gssgPorts(l, false);
        three[0].pair = QStringLiteral("P2");
        const rf::FullWave partly = rf::fullWaveOf(mesh, c, open, three, {5e9});
        QVERIFY2(partly.ok(), qPrintable(why(partly)));
        QStringList order, of;
        const QList<QList<Complex>> modes = rf::mixedModeOf(partly, 0, &order, &of);
        QCOMPARE(order, (QStringList{"D1", "C1", "P3", "P4"}));
        QCOMPARE(of, (QStringList{"P1 - P2", "P1 + P2", "P3", "P4"}));
        QVERIFY(std::abs(modes[2][3] - partly.s.at(0).at(2).at(3)) < 1e-15);
        // Touchstone 2.0 of the modes: [Mixed-Mode Order] after [Reference],
        // each matrix the modes'. None without a pair.
        const QString text = rf::touchstone2Of(fw, {QStringLiteral("a GSSG")}, true);
        QVERIFY2(text.contains(QStringLiteral("[Reference] 50 50 50 50\n[Mixed-Mode Order] D1,2 D3,4 C1,2 C3,4\n[Network Data]\n")),
                 qPrintable(text.left(800)));
        QVERIFY2(text.contains(QStringLiteral("! Mixed-mode S: D1,2 P1 - P2, D3,4 P3 - P4, C1,2 P1 + P2, C3,4 P3 + P4")), qPrintable(text.left(800)));
        const QList<QList<Complex>> first = rf::mixedModeOf(fw, 0);
        const QStringList data = text.section(QStringLiteral("[Network Data]\n"), 1).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QStringList row = data.at(0).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QCOMPARE(row.at(0).toDouble(), 5e9);
        QVERIFY2(std::abs(row.at(1).toDouble() - first[0][0].real()) < 1e-9 && std::abs(row.at(2).toDouble() - first[0][0].imag()) < 1e-9
                     && std::abs(row.at(3).toDouble() - first[0][1].real()) < 1e-9,
                 qPrintable(data.at(0)));
        QVERIFY(rf::touchstone2Of(partly, {}, true).contains(QStringLiteral("[Mixed-Mode Order] D1,2 C1,2 S3 S4\n")));
        QVERIFY(rf::touchstone2Of(rf::fullWaveOf(mesh, c, open, gssgPorts(l, false), {5e9}), {}, true).isEmpty());
        QVERIFY(!rf::touchstone2Of(fw).contains(QStringLiteral("Mixed")));
        // A pair checked as given: a port the layout has not, itself, a
        // port in two pairs, and two references.
        const auto refusal = [&](QList<rf::FullWavePort> given) {
            const rf::FullWave w = rf::fullWaveOf(mesh, c, open, given, {5e9});
            return w.ok() ? QString() : why(w);
        };
        QList<rf::FullWavePort> bad = gssgPorts(l, false);
        bad[0].pair = QStringLiteral("P9");
        QVERIFY2(refusal(bad).contains(QStringLiteral("P1 is paired with P9, and the layout has no port called that")), qPrintable(refusal(bad)));
        bad[0].pair = QStringLiteral("P1");
        QVERIFY2(refusal(bad).contains(QStringLiteral("P1 is paired with itself")), qPrintable(refusal(bad)));
        bad[0].pair = QStringLiteral("P2");
        bad[2].pair = QStringLiteral("P2");
        QVERIFY2(refusal(bad).contains(QStringLiteral("P2 is in two pairs")), qPrintable(refusal(bad)));
        bad[2].pair.clear();
        bad[1].impedance = 25;
        QVERIFY2(refusal(bad).contains(QStringLiteral("P1 and P2 are a pair, and their reference impedances differ (50 and 25 Ohm)")),
                 qPrintable(refusal(bad)));
        // Over a ground plane, nothing marked: the nearest conductor either
        // side would be the other signal - said, not solved; marked, the
        // grounds are its returns.
        const rf::Stack plane = grounded(12.4, 150e-6);
        const rf::FullWave unmarked = rf::fullWaveOf(mesh, c, plane, gssgPorts(l), {5e9});
        QVERIFY2(says(unmarked.problems, "which P2 drives: mark the conductors the lines return through"), qPrintable(why(unmarked)));
        const QList<rf::ReturnMark> marks = {{QPointF(300, 200), QStringLiteral("M1")}, {QPointF(300, -200), QString()}};
        const rf::FullWave marked = rf::fullWaveOf(mesh, c, plane, gssgPorts(l), {5e9}, 1e-6, nullptr, marks);
        QVERIFY2(marked.ok(), qPrintable(why(marked)));
        QVERIFY2(says(marked.lines, "marked as returns") && says(marked.portsSaid, "which is no return"),
                 qPrintable((marked.lines + marked.portsSaid).join(" | ")));
        QVERIFY2(says(marked.lines, "The ground plane is taken as perfect"), qPrintable(marked.lines.join(" | ")));
    }

    // Phase E: past the dense solve's few thousand runs and cells the
    // system is held compressed - each block between clusters apart beside
    // their size of low rank - and solved by GMRES, preconditioned by the
    // near field's sparse LU. Asked for where the dense solve can be had
    // too, it is the dense solve: a line from 10 MHz to 40 GHz; a metal
    // solved as its two faces, its walls vias; a lossy tee; two metals and
    // a via; a GSSG with no ground plane, its ports coplanar and paired; a
    // pad on a farm of vias at 100 MHz, as near singular as a system gets,
    // where it is GMRES's residual that counts (at 1e-6 it was 1e-3 out);
    // and a strip at a slant, whose runs between cells cut to its outline
    // are oblique - in both of L's kernels at once. Measured within 2.6e-7
    // of the dense solve's S at its tolerance of 1e-5; held here to 2e-6,
    // reciprocal and passive as the dense solve is.
    void aCompressedSolveIsTheDenseOne()
    {
        const bool slow = slowBuild();
        struct Case {
            const char* name;
            QList<rf::Conductor> conductors;
            rf::Mesh mesh;
            rf::Stack stack;
            QList<rf::FullWavePort> ports;
            QList<double> fs;
        };
        QList<Case> cases;
        const QString m1 = QStringLiteral("M1");
        const QList<rf::Conductor> line{strip(0, 0, 3000, 100)};
        cases << Case{"line", line, meshed(line, 4, 150e-6, 6), grounded(9.8, 100e-6),
                      {edgePort(QPointF(0, 50)), edgePort(QPointF(3000, 50))},
                      slow ? QList<double>{1e7, 10e9} : QList<double>{1e7, 10e9, 40e9}};
        if (!slow) {
            rf::Stack faces = grounded(9.8, 100e-6, 0, 3e-6, 5.8e7);
            faces.layers.last().twoSheets = true;
            const QList<rf::Conductor> short1{strip(0, 0, 1000, 100)};
            cases << Case{"faces", short1, meshed(short1, 4, 100e-6, 4), faces,
                          {edgePort(QPointF(0, 50)), edgePort(QPointF(1000, 50))}, {1e7, 10e9}};
        }
        rf::Conductor tee;
        tee.layer = m1;
        tee.outline = QPolygonF({QPointF(0, 0), QPointF(1000, 0), QPointF(1000, 100), QPointF(550, 100), QPointF(550, 600),
                                 QPointF(450, 600), QPointF(450, 100), QPointF(0, 100)});
        cases << Case{"tee", {tee}, meshed({tee}, 3, 100e-6), grounded(9.8, 100e-6, 0.02, 2e-6, 5.8e7),
                      {edgePort(QPointF(0, 50)), edgePort(QPointF(1000, 50)), edgePort(QPointF(500, 600))}, {12e9}};
        const double w = 50;
        const QList<rf::Conductor> climbing = {strip(0, 0, 1000, w), on(QStringLiteral("M2"), 1000 - w, 0, 1000, w),
                                               on(QStringLiteral("V1"), 1000 - w, 0, w, w)};
        cases << Case{"via", climbing, evenly(climbing, 2, 100e-6, 2),
                      withVia(twoMetals(), QStringLiteral("V1"), QStringLiteral("M1"), QStringLiteral("M2")),
                      {edgePort(QPointF(0, w / 2), m1), edgePort(QPointF(2000 - w, w / 2), QStringLiteral("M2"))},
                      slow ? QList<double>{1e8} : QList<double>{1e8, 20e9}};
        rf::Stack open = grounded(12.4, 150e-6);
        open.layers[0].variant = QStringLiteral("open");
        const QList<rf::Conductor> pairs = gssg(600);
        cases << Case{"gssg", pairs, meshed(pairs, 3, 100e-6), open, gssgPorts(600), {2e9}};
        QList<rf::Conductor> farm = {on(m1, 0, -50, 1000, 100), on(m1, 1000, -300, 600, 600)};
        for (int i = 0; i < (slow ? 6 : 12); ++i)
            for (int j = 0; j < (slow ? 6 : 12); ++j) farm << on(QStringLiteral("V1"), 1020 + 48 * i * (slow ? 2 : 1), -280 + 48 * j * (slow ? 2 : 1), 20, 20);
        cases << Case{"farm", farm, meshed(farm, 3, 50e-6),
                      withVia(grounded(9.8, 200e-6), QStringLiteral("V1"), QStringLiteral("ground"), m1),
                      {edgePort(QPointF(0, 0), m1, QPointF(-1, 0))}, {1e8}};
        rf::Conductor slant;
        slant.layer = m1;
        slant.outline = QPolygonF({QPointF(0, 0), QPointF(1500, 866), QPointF(1500, 966), QPointF(0, 100)});
        cases << Case{"slant", {slant}, meshed({slant}, 3, 100e-6), grounded(9.8, 100e-6, 0, 2e-6, 5.8e7),
                      {edgePort(QPointF(0, 50), m1, QPointF(-1, 0)), edgePort(QPointF(1500, 916), m1, QPointF(1, 0))}, {5e9}};
        for (const Case& c : cases) {
            rf::FullWaveControl dense, held;
            dense.matrix = rf::FullWaveControl::Matrix::Dense;
            held.matrix = rf::FullWaveControl::Matrix::Compressed;
            const rf::FullWave d = rf::fullWaveOf(c.mesh, c.conductors, c.stack, c.ports, c.fs, 1e-6, &dense);
            const rf::FullWave h = rf::fullWaveOf(c.mesh, c.conductors, c.stack, c.ports, c.fs, 1e-6, &held);
            QVERIFY2(d.ok() && h.ok(), qPrintable(QStringLiteral("%1: %2 | %3").arg(QLatin1String(c.name), why(d), why(h))));
            QVERIFY(!d.compressed && h.compressed);
            QVERIFY2(says(h.lines, "The system held compressed") && says(h.lines, "Solved by GMRES"), qPrintable(h.lines.join(" | ")));
            double worst = 0;
            for (int k = 0; k < c.fs.size(); ++k)
                for (int i = 0; i < d.s.at(k).size(); ++i)
                    for (int j = 0; j < d.s.at(k).size(); ++j) worst = std::max(worst, std::abs(h.s[k][i][j] - d.s[k][i][j]));
            qWarning("%s: %d runs over %d cells, S within %.2g of the dense solve's; %.2f MB, rank %d, %d iteration(s), "
                     "residual %.2g, asymmetry %.2g, gain %.2g (dense %.2g)",
                     c.name, h.runs, h.cells, worst, h.memory / 1e6, h.largestRank, h.iterations, h.residual, h.asymmetry, h.gain,
                     d.gain);
            QVERIFY2(worst < 2e-6, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(c.name)).arg(worst)));
            QVERIFY2(h.asymmetry < 1e-9, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(c.name)).arg(h.asymmetry)));
            QVERIFY2(h.gain <= d.gain + 1e-9, qPrintable(QStringLiteral("%1: %2 / %3").arg(QLatin1String(c.name)).arg(h.gain).arg(d.gain)));
            QVERIFY(h.iterations > 0 && h.residual <= 1e-10 && h.memory > 0 && h.tolerance == 1e-5);
            // The near field's LU is what keeps the iterations few - 10 to 36
            // here, at 10 MHz too - where the system is as ill conditioned as
            // a low frequency's: without it GMRES does not get there at all.
            QVERIFY2(h.iterations < 80, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(c.name)).arg(h.iterations)));
            // And the plainest of them a little over what they take: the near
            // field's P whole, not its diagonal alone, about halves them (a
            // line 13 against 30, a via 10 against 18, the slant 16 against 29).
            const QLatin1String name(c.name);
            const int few = name == QLatin1String("line") ? 20 : name == QLatin1String("via") ? 14 : name == QLatin1String("slant") ? 24 : 80;
            QVERIFY2(h.iterations <= few, qPrintable(QStringLiteral("%1: %2 iterations").arg(name).arg(h.iterations)));
            if (QLatin1String(c.name) == QLatin1String("slant")) {
                // Its runs oblique, most of them, and both kernels' own.
                int oblique = 0;
                for (const rf::Filament& f : h.geometry) {
                    const double dx = f.to.x() - f.from.x(), dy = f.to.y() - f.from.y();
                    oblique += std::abs(dx) > 1e-12 && std::abs(dy) > 1e-12 ? 1 : 0;
                }
                QVERIFY2(oblique > 10, qPrintable(QString::number(oblique)));
            }
        }
    }

    // Dense up to the limit and compressed past it, unasked: the limit
    // lowered here, a line past it held compressed and said so, its S the
    // dense solve's. And in a Release build lines too fine for a dense solve
    // - ten cells across, 4 and 6 mm long, some 5700 and 8500 runs and
    // cells - are the line they are: at 10 GHz the two-line method's
    // effective permittivity within 2e-3 of Kirschning and Jansen from its
    // own quasi-static one, and its Z0 within a per cent of sqrt(L'/C').
    void pastTheDenseLimitItIsHeldCompressed()
    {
        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> l{strip(0, 0, 3000, 100)};
        const rf::Mesh mesh = meshed(l, 4, 150e-6, 6);
        const QList<rf::FullWavePort> ends{edgePort(QPointF(0, 50)), edgePort(QPointF(3000, 50))};
        rf::FullWaveControl low;
        low.mostUnknowns = 200;
        const rf::FullWave held = rf::fullWaveOf(mesh, l, stack, ends, {10e9}, 1e-6, &low);
        const rf::FullWave dense = rf::fullWaveOf(mesh, l, stack, ends, {10e9});
        QVERIFY2(held.ok() && dense.ok(), qPrintable(why(held) + why(dense)));
        QVERIFY(held.compressed && !dense.compressed);
        QVERIFY2(says(held.lines, "past the 200 a dense matrix is set up for"), qPrintable(held.lines.join(" | ")));
        QVERIFY(!says(dense.lines, "held compressed"));
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) QVERIFY(std::abs(held.s[0][i][j] - dense.s[0][i][j]) < 2e-6);
        if (slowBuild()) return;
        const double h = 100e-6, w = 100, epsr = 9.8;
        const QList<double> fs{1e8, 10e9};
        rf::FullWaveControl past;
        past.mostUnknowns = 1000;
        const auto fine = [&](double length) {
            const QList<rf::Conductor> c{strip(0, 0, length, w)};
            return rf::fullWaveOf(meshed(c, 10, 25e-6, 4), c, stack, {edgePort(QPointF(0, w / 2)), edgePort(QPointF(length, w / 2))}, fs,
                                  1e-6, &past);
        };
        const rf::FullWave a = fine(4000), b = fine(6000);
        QVERIFY2(a.ok() && b.ok(), qPrintable(why(a) + why(b)));
        QVERIFY2(a.compressed && b.compressed && a.runs + a.cells > 5000 && b.runs + b.cells > 8000,
                 qPrintable(QStringLiteral("%1 and %2").arg(a.runs + a.cells).arg(b.runs + b.cells)));
        const LowFrequency lf = lowFrequency(a, b, 0, 4000e-6, 6000e-6);
        const double eps0 = kC0 * kC0 * lf.lPer * lf.cPer, z0 = std::sqrt(lf.lPer / lf.cPer);
        const double k0 = 2 * M_PI * fs.at(1) / kC0;
        const double kj = kirschningJansen(eps0, epsr, w * 1e-6 / h, fs.at(1) / 1e9, h * 1e3);
        const TwoLine wave = twoLine(chainOf(zOf(a, 1)), chainOf(zOf(b, 1)), 4000e-6, 6000e-6, k0 * std::sqrt(kj));
        const double eps = std::pow(wave.gamma.imag() / k0, 2);
        qWarning("ten across, %d and %d runs and cells: eps_eff %.5f (quasi-static %.5f), Kirschning and Jansen %.5f; Z0 %.3f "
                 "%+.3fj vs %.3f; %.1f MB, %d iteration(s)",
                 a.runs + a.cells, b.runs + b.cells, eps, eps0, kj, wave.z0.real(), wave.z0.imag(), z0, b.memory / 1e6, b.iterations);
        QVERIFY2(std::abs(eps / kj - 1) < 2e-3, qPrintable(QStringLiteral("eps_eff %1, K&J %2").arg(eps).arg(kj)));
        QVERIFY2(std::abs(wave.z0 - z0) < 0.01 * z0, qPrintable(QStringLiteral("Z0 %1, %2").arg(wave.z0.real()).arg(z0)));
        QVERIFY2(a.iterations < 80 && b.iterations < 80, qPrintable(QStringLiteral("%1 %2").arg(a.iterations).arg(b.iterations)));
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
        // No ground plane for the ports to return through: their current
        // comes back through the conductors or not at all.
        rf::Stack open = stack;
        open.layers[0].variant = QStringLiteral("open");
        QVERIFY(refused(rf::fullWaveOf(mesh, one, open, ports, {1e9}), "no ground plane under the stack, and P1, P2 return(s)"));
        // Two ports on the same face.
        QVERIFY(refused(rf::fullWaveOf(mesh, one, stack, {QPointF(0, 20), QPointF(0, 80)}, {1e9}), "same face"));
        // More runs and cells than this build sets up a dense matrix for, a
        // dense solve asked: said and not solved - six thousand at most since
        // a metal can be solved as its two faces, which doubles its cells (the
        // recheck of 75ff6f9) - and the compressed solve named. Held
        // compressed, as the automatic choice holds it past that, more memory
        // than a solve may take is said the same way, before anything is
        // filled. (The bound lowered here.)
        const QList<rf::Conductor> square{strip(0, 0, 2000, 2000)};
        const rf::Mesh squareMesh = meshed(square, 0, 25e-6, 1);
        rf::FullWaveControl asDense, tight;
        asDense.matrix = rf::FullWaveControl::Matrix::Dense;
        tight.mostBytes = 1e7;
        const rf::FullWave big = rf::fullWaveOf(squareMesh, square, stack, {edgePort(QPointF(0, 1000))}, {1e9}, 1e-6, &asDense);
        QVERIFY(big.tooLarge);
        // (The limit in effect: RF 2.5D > Settings…'s, Automatic's here.)
        const QByteArray inAll = QStringLiteral("(%1 in all)").arg(rf::denseLimit()).toUtf8();
        QVERIFY2(refused(big, inAll.constData()) && refused(big, "hold it compressed") && refused(big, "raise the limit"),
                 qPrintable(why(big)));
        const rf::FullWave over = rf::fullWaveOf(squareMesh, square, stack, {edgePort(QPointF(0, 1000))}, {1e9}, 1e-6, &tight);
        QVERIFY(over.tooLarge);
        QVERIFY2(refused(over, "held compressed, would take some") && refused(over, "a solve may take here"), qPrintable(why(over)));
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
        // More than the dense limit holds, a dense solve asked (unasked, it
        // is held compressed).
        const QList<rf::Conductor> plate{strip(0, 0, 1000, 1000)};
        rf::MeshSettings fine;
        fine.maximum = 20e-6;
        fine.cellsAcross = 0;
        fine.edges = false;
        QVERIFY(refused(rf::fullWaveOf(rf::meshOf(plate, fine, rf::Stack(), 0), plate, stack, {edgePort(QPointF(0, 500))}, {1e9},
                                       1e-6, &asDense),
                        "more than a dense complex matrix is set up for"));
    }

    // Where a stack begins to carry a second surface wave (the phase E check
    // of 2 November): over a ground, its TE1's cutoff - a quarter wave of
    // k0 (eps - 1)^1/2 through one slab, 39.79 GHz through 635 µm of
    // alumina; through two where tan(kz1 h1) tan(kz2 h2) = kz1/kz2; with no
    // ground, the slab's half wave. None up to the frequency asked: nought.
    void theSecondSurfaceWaveIsTE1sCutoff()
    {
        const double one = rf::secondSurfaceWave(grounded(9.8, 635e-6), 1e12);
        QVERIFY2(std::abs(one / (kC0 / (4 * 635e-6 * std::sqrt(8.8))) - 1) < 1e-9, qPrintable(QString::number(one)));
        QCOMPARE(rf::secondSurfaceWave(grounded(9.8, 635e-6), 30e9), 0.0);
        // Silicon under 20 µm of oxide: the root of the two slabs' own
        // equation, found apart.
        rf::Stack two = grounded(11.9, 300e-6);
        rf::Layer oxide;
        oxide.kind = rf::Layer::Dielectric;
        oxide.name = QStringLiteral("OX");
        oxide.tag = QStringLiteral("ox");
        oxide.thickness = 20e-6;
        oxide.epsr = 3.9;
        two.layers.insert(2, oxide);
        const double silicon = kC0 / (4 * 300e-6 * std::sqrt(10.9));
        const auto rest = [](double f) {
            const double k0 = 2 * M_PI * f / kC0, k1 = k0 * std::sqrt(10.9), k2 = k0 * std::sqrt(2.9);
            return std::tan(k1 * 300e-6) * std::tan(k2 * 20e-6) - k1 / k2;
        };
        double lo = 0.8 * silicon, hi = silicon * (1 - 1e-9);
        for (int i = 0; i < 100; ++i) (rest(0.5 * (lo + hi)) < 0 ? lo : hi) = 0.5 * (lo + hi);
        const double both = rf::secondSurfaceWave(two, 1e12);
        qWarning().noquote() << QStringLiteral("TE1: silicon alone %1 GHz, under oxide %2 (its own root %3)")
                                    .arg(silicon / 1e9, 0, 'f', 4).arg(both / 1e9, 0, 'f', 4).arg(lo / 1e9, 0, 'f', 4);
        QVERIFY2(std::abs(both / lo - 1) < 1e-6 && both < silicon, qPrintable(QString::number(both)));
        // No ground: a half wave.
        rf::Stack free = grounded(9.8, 635e-6);
        free.layers.removeFirst();
        const double half = rf::secondSurfaceWave(free, 1e12);
        QVERIFY2(std::abs(half / (kC0 / (2 * 635e-6 * std::sqrt(8.8))) - 1) < 1e-9, qPrintable(QString::number(half)));
    }

    // Past it a strip is as passive as below it (G9 of the phase E check,
    // its cause found after: what gave power back there was the ports' own
    // paths up from the ground, without a field - addPortPaths()). On 635
    // µm of alumina a strip alone at 42 GHz, past its TE1 at 39.79, gives
    // back nothing, and a loss tangent of a hundredth loses the more - where
    // at 41.5 GHz it gave back 2.2 times, and with the loss more again. The
    // cutoff is said among the lines, the top of a microstrip's band on it,
    // and is no warning; a sweep below it says nothing of it.
    void pastTheSecondSurfaceWaveAStripIsPassive()
    {
        const bool slow = slowBuild();
        const double l = slow ? 2500 : 5000;
        const QList<rf::Conductor> line = {strip(0, 0, l, 600)};
        const rf::Mesh mesh = meshed(line, 2, slow ? 400e-6 : 200e-6);
        const QList<rf::FullWavePort> ports = {edgePort(QPointF(0, 300), QString(), QPointF(-1, 0)),
                                               edgePort(QPointF(l, 300), QString(), QPointF(1, 0))};
        const rf::FullWave below = rf::fullWaveOf(mesh, line, grounded(9.8, 635e-6), ports, {36e9});
        QVERIFY2(below.ok(), qPrintable(why(below)));
        QVERIFY2(below.warnings.isEmpty() && !says(below.lines, "second surface wave"), qPrintable(below.lines.join(" | ")));
        QVERIFY2(below.secondWave > 39.7e9 && below.secondWave < 39.9e9, qPrintable(QString::number(below.secondWave)));
        double most[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            const rf::FullWave past = rf::fullWaveOf(mesh, line, grounded(9.8, 635e-6, i == 0 ? 0 : 0.01), ports, {36e9, 42e9});
            QVERIFY2(past.ok(), qPrintable(why(past)));
            QVERIFY2(past.warnings.isEmpty(), qPrintable(past.warnings.join(" | ")));
            QVERIFY2(says(past.lines, "From 39.79 GHz the stack carries a second surface wave, its TE1"),
                     qPrintable(past.lines.join(" | ")));
            Eigen::Matrix2cd s;
            s << past.s.at(1).at(0).at(0), past.s.at(1).at(0).at(1), past.s.at(1).at(1).at(0), past.s.at(1).at(1).at(1);
            most[i] = Eigen::SelfAdjointEigenSolver<Eigen::Matrix2cd>(s.adjoint() * s).eigenvalues().maxCoeff();
            QVERIFY2(past.gain <= 1e-6 && most[i] < 1, qPrintable(QStringLiteral("%1 at 42 GHz").arg(most[i])));
        }
        qWarning("past TE1, at 42 GHz: of a wave in at most %.4f comes back out, %.4f with a loss tangent of 0.01", most[0], most[1]);
        QVERIFY2(most[1] < most[0], "the loss loses the more");
    }

    // A port's own path up from the ground is a current of its own and
    // radiates as one (G9): a patch 200 µm square a millimetre over the
    // ground in vacuum, driven by an internal port, is a short monopole
    // whose current is the same all the way up and whose charge is at its
    // top, its radiation resistance 160 pi^2 (h/lambda)^2 - 0.158 Ohm at 3
    // GHz, as much again at four times that at 6. The path's vector
    // potential is three halves of it, and the charge's scalar potential
    // takes a half away: without the path's field a port had the half alone,
    // a resistance of -0.079 Ohm, and gave back.
    void aPortsPathRadiatesAsAMonopole()
    {
        const double h = 1e-3;
        const QList<rf::Conductor> patch = {strip(0, 0, 200, 200)};
        const rf::Mesh mesh = evenly(patch, 2, 100e-6, 1);
        const QList<double> fs = {3e9, 6e9};
        const rf::FullWave fw = rf::fullWaveOf(mesh, patch, grounded(1.0, h), {internalPort(QPointF(50, 50), QStringLiteral("M1"))}, fs);
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        for (int k = 0; k < fs.size(); ++k) {
            const double r = fw.z.at(k).at(0).at(0).real(), lambda = kC0 / fs.at(k);
            const double monopole = 160 * M_PI * M_PI * (h / lambda) * (h / lambda);
            qWarning("a millimetre's path at %.0f GHz: %.6f Ohm, a short monopole's %.6f (%.2e)", fs.at(k) / 1e9, r, monopole,
                     r / monopole - 1);
            QVERIFY2(std::abs(r / monopole - 1) < 0.02, qPrintable(QStringLiteral("%1 Ohm, %2").arg(r).arg(monopole)));
        }
    }

    // G9 with nothing but vacuum under the strip, and no surface wave of any
    // kind to blame: a strip 600 µm wide 2 mm over its ground, 5 mm long,
    // gave back up to 49 times what it was given at 64 GHz, its ports' paths
    // without a field. With theirs it is passive over the sweep.
    void aStripOverVacuumGivesNothingBack()
    {
        const bool slow = slowBuild();
        const QList<rf::Conductor> line = {strip(0, 0, 5000, 600)};
        const rf::Mesh mesh = meshed(line, 2, slow ? 400e-6 : 200e-6);
        const QList<double> fs = slow ? QList<double>{34e9, 64e9} : QList<double>{16e9, 28e9, 34e9, 46e9, 58e9, 64e9};
        const rf::FullWave fw = rf::fullWaveOf(mesh, line, grounded(1.0, 2e-3),
                                               {edgePort(QPointF(0, 300), QString(), QPointF(-1, 0)),
                                                edgePort(QPointF(5000, 300), QString(), QPointF(1, 0))},
                                               fs);
        QVERIFY2(fw.ok(), qPrintable(why(fw)));
        double most = 0;
        for (int k = 0; k < fs.size(); ++k) {
            Eigen::Matrix2cd s;
            s << fw.s.at(k).at(0).at(0), fw.s.at(k).at(0).at(1), fw.s.at(k).at(1).at(0), fw.s.at(k).at(1).at(1);
            most = std::max(most, Eigen::SelfAdjointEigenSolver<Eigen::Matrix2cd>(s.adjoint() * s).eigenvalues().maxCoeff());
        }
        qWarning("over 2 mm of vacuum: of a wave in at most %.4f comes back out", most);
        const int driven = cellsDriven(fw.portsSaid.value(0)) + cellsDriven(fw.portsSaid.value(1));
        QVERIFY2(driven > 2 && says(fw.lines, qPrintable(QStringLiteral("by %1 path(s) of their own").arg(driven))),
                 qPrintable(fw.lines.join(" | ")));
        QVERIFY2(fw.gain <= 1e-6 && most < 1 && fw.asymmetry < 1e-9,
                 qPrintable(QStringLiteral("%1, gain %2, asymmetry %3").arg(most).arg(fw.gain).arg(fw.asymmetry)));
    }

    // A stack's Green's functions are kept between solves (keepGreens()),
    // and what a solve gives does not hang on what was solved before it: a
    // line solved keeping nothing, and again after a line twice as long on
    // the same stack has kept its tables, reads the same to the last digit -
    // the second solve building nothing, its tables the first part of the
    // longer one's - and so again. (At 1 and 2 GHz, where a grid begun at a
    // thousandth of the farthest distance would have begun at 2 µm for the
    // one and 4 for the other.)
    void aSolveReadsTheSameWhateverWasSolvedBefore()
    {
        const bool slow = slowBuild();
        const rf::Stack stack = grounded(9.8, 254e-6);
        const QList<double> fs = slow ? QList<double>{1e9} : QList<double>{1e9, 2e9};
        const auto solve = [&stack, &fs](double l) {
            const QList<rf::Conductor> line = {strip(0, 0, l, 250)};
            return rf::fullWaveOf(meshed(line, 2, 200e-6), line, stack,
                                  {edgePort(QPointF(0, 125), QString(), QPointF(-1, 0)), edgePort(QPointF(l, 125), QString(), QPointF(1, 0))},
                                  fs);
        };
        rf::keepGreens(0);
        const rf::FullWave own = solve(2000), longOwn = solve(4000);
        QVERIFY2(own.ok() && longOwn.ok(), qPrintable(why(own) + why(longOwn)));
        QCOMPARE(rf::greensKept().tables, 0);
        rf::keepGreens(256e6);
        rf::forgetGreens();
        const rf::FullWave longKept = solve(4000);
        const long long built = rf::greensKept().built;
        const rf::FullWave kept = solve(2000);
        QVERIFY2(kept.ok() && longKept.ok(), qPrintable(why(kept) + why(longKept)));
        QCOMPARE(rf::greensKept().built, built);
        QVERIFY(rf::greensKept().reused > 0);
        QVERIFY2(kept.z == own.z && longKept.z == longOwn.z, "to the last digit, kept or not");
        const rf::FullWave again = solve(2000);
        QVERIFY(again.z == own.z && rf::greensKept().built == built);
        rf::forgetGreens();
    }

    // How large a system is held dense and how many frequencies are solved
    // at once, as RF 2.5D > Settings… sets them for the process: past the
    // limit held compressed and said so, a control's own limit over it,
    // Automatic's again at nought; a sweep's frequencies at once as the
    // memory allows, a frequency some 16 (nr^2 + nc^2) bytes, its system
    // factorized where it is.
    void theDenseLimitAndTheSweepMemoryAreTheSettings()
    {
        const auto automatic = qScopeGuard([] {
            rf::setDenseLimit(0);
            rf::setSweepMemory(0);
        });
        const rf::Stack stack = grounded(9.8, 100e-6);
        const QList<rf::Conductor> l{strip(0, 0, 3000, 100)};
        const rf::Mesh mesh = meshed(l, 4, 150e-6, 6);
        const QList<rf::FullWavePort> ends{edgePort(QPointF(0, 50)), edgePort(QPointF(3000, 50))};
        QCOMPARE(rf::denseLimit(), rf::automaticDenseLimit());
        rf::setDenseLimit(200);
        QCOMPARE(rf::denseLimit(), 200);
        const rf::FullWave held = rf::fullWaveOf(mesh, l, stack, ends, {10e9});
        QVERIFY2(held.ok() && held.compressed, qPrintable(why(held)));
        QVERIFY2(held.runs + held.cells > 200, qPrintable(QString::number(held.runs + held.cells)));
        QVERIFY2(says(held.lines, "past the 200 a dense matrix is set up for"), qPrintable(held.lines.join(" | ")));
        rf::FullWaveControl own;
        own.mostUnknowns = 100000;
        const rf::FullWave dense = rf::fullWaveOf(mesh, l, stack, ends, {10e9}, 1e-6, &own);
        QVERIFY2(dense.ok() && !dense.compressed, qPrintable(why(dense)));
        rf::setDenseLimit(0);
        QCOMPARE(rf::denseLimit(), rf::automaticDenseLimit());
        QVERIFY(!rf::fullWaveOf(mesh, l, stack, ends, {10e9}).compressed);
        // Four frequencies on eight threads: as many at once as 2.5, 0.5 and
        // (Automatic) many of its systems' memory allow.
        const double each = 16.0 * (double(dense.runs) * dense.runs + double(dense.cells) * dense.cells) + 1e6;
        rf::FullWaveControl eight;
        eight.threads = 8;
        const QList<double> fs{1e9, 2e9, 3e9, 4e9};
        const auto atATime = [&](double memory) {
            rf::setSweepMemory(memory);
            const rf::FullWave sweep = rf::fullWaveOf(mesh, l, stack, ends, fs, 1e-6, &eight);
            return sweep.ok() ? sweep.lines.filter(QStringLiteral("4 frequency(ies) from 1 to 4 GHz")).join(QString()) : why(sweep);
        };
        QString said = atATime(2.5 * each);
        QVERIFY2(said.contains(QLatin1String("2 at a time")), qPrintable(said));
        said = atATime(0.5 * each);
        QVERIFY2(said.contains(QLatin1String("1 at a time")), qPrintable(said));
        said = atATime(0);
        QVERIFY2(rf::automaticSweepMemory() > 4 * each && said.contains(QLatin1String("4 at a time")), qPrintable(said));
        // Automatic's: a quarter of the machine's memory, 1.5 GB at least.
        QCOMPARE(rf::sweepMemory(), rf::automaticSweepMemory());
        QCOMPARE(rf::automaticSweepMemory(), std::max(1.5e9, rf::physicalMemory() / 4));
    }

    // Its system factorized by Accelerate's LAPACK where the build and the
    // macOS have it, by Eigen's elsewhere and as asked (rf25_dense.h): the
    // same S and currents to rounding either way, and whose LU it was and
    // what each frequency's fill and LU took said.
    void theSystemIsFactorizedAlikeEitherWay()
    {
        const LuOverridesAway away;
        const rf::Stack stack = grounded(9.8, 254e-6);
        const QList<rf::Conductor> line = {strip(0, 0, 2000, 250)};
        const QList<double> fs = slowBuild() ? QList<double>{1e9} : QList<double>{1e9, 5e9};
        const auto solve = [&stack, &line, &fs] {
            return rf::fullWaveOf(meshed(line, 2, 200e-6), line, stack,
                                  {edgePort(QPointF(0, 125), QString(), QPointF(-1, 0)), edgePort(QPointF(2000, 125), QString(), QPointF(1, 0))},
                                  fs);
        };
        rf::setDenseLu(rf::DenseLu::Eigen);
        const long long eigenBefore = rf::eigenLuCount();
        const rf::FullWave eigen = solve();
        QCOMPARE(rf::eigenLuCount() - eigenBefore, (long long)fs.size());
        rf::setDenseLu(rf::DenseLu::Automatic);
        const rf::FullWave automatic = solve();
        QVERIFY2(eigen.ok() && automatic.ok(), qPrintable(why(eigen) + why(automatic)));
        QVERIFY(!eigen.compressed && !automatic.compressed);
        QVERIFY(!eigen.accelerated);
        QCOMPARE(automatic.accelerated, rf::accelerateLuHere());
        QVERIFY2(eigen.fillSeconds > 0 && eigen.luSeconds > 0 && automatic.fillSeconds > 0 && automatic.luSeconds > 0,
                 qPrintable(QStringLiteral("fill %1 and %2 s, LU %3 and %4 s").arg(eigen.fillSeconds).arg(automatic.fillSeconds)
                                .arg(eigen.luSeconds).arg(automatic.luSeconds)));
        double apart = 0, largest = 0, currentApart = 0, currentLargest = 0;
        for (int k = 0; k < fs.size(); ++k) {
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    apart = std::max(apart, std::abs(eigen.s.at(k).at(i).at(j) - automatic.s.at(k).at(i).at(j)));
                    largest = std::max(largest, std::abs(eigen.s.at(k).at(i).at(j)));
                }
            for (int q = 0; q < 2; ++q)
                for (int r = 0; r < eigen.currents.at(k).at(q).size(); ++r) {
                    currentApart = std::max(currentApart, std::abs(eigen.currents.at(k).at(q).at(r) - automatic.currents.at(k).at(q).at(r)));
                    currentLargest = std::max(currentLargest, std::abs(eigen.currents.at(k).at(q).at(r)));
                }
        }
        QVERIFY2(apart <= 1e-10 * largest, qPrintable(QStringLiteral("S %1 apart, of %2").arg(apart).arg(largest)));
        QVERIFY2(currentApart <= 1e-9 * currentLargest,
                 qPrintable(QStringLiteral("the currents %1 apart, of %2").arg(currentApart).arg(currentLargest)));
    }

    // A filter's shapes drawn touching - one of them with a slanted side,
    // the set gdstk's union alone leaves in two - solve as the outline drawn
    // whole: Z the same to a part in 1e7. The stub lost its line before,
    // S21 -17.2 dB against -0.04 at 2 GHz, passive and reciprocal and with
    // nothing said (B1 of the open items of 8 November).
    void shapesDrawnTouchingSolveAsTheOutlineWhole()
    {
        const rf::Stack stack = grounded(2.2, 1.6e-3);
        rf::Processing how;
        how.dropSlivers = false;
        const QPolygonF pad({QPointF(-950, -1200), QPointF(-550, -1600), QPointF(1550, -1600), QPointF(1550, 0), QPointF(-950, 0)});
        QMap<QString, QList<QPolygonF>> drawn;
        drawn[QStringLiteral("M1")] = {QPolygonF(QRectF(0, 0, 3600, 3900)), QPolygonF(QRectF(1600, -2100, 2600, 2100)),
                                       QPolygonF(QRectF(2150, -5200, 100, 3100)), pad};
        QMap<QString, QList<QPolygonF>> whole;
        whole[QStringLiteral("M1")] = {QPolygonF({QPointF(-950, -1200), QPointF(-550, -1600), QPointF(1550, -1600), QPointF(1550, 0),
                                                  QPointF(1600, 0), QPointF(1600, -2100), QPointF(2150, -2100), QPointF(2150, -5200),
                                                  QPointF(2250, -5200), QPointF(2250, -2100), QPointF(4200, -2100), QPointF(4200, 0),
                                                  QPointF(3600, 0), QPointF(3600, 3900), QPointF(0, 3900), QPointF(0, 0), QPointF(-950, 0)})};
        rf::MeshSettings settings;
        settings.cellsAcross = 3;
        const QList<rf::FullWavePort> ports = {edgePort(QPointF(-950, -800), QString(), QPointF(-1, 0)),
                                               edgePort(QPointF(4200, -1050), QString(), QPointF(1, 0))};
        QList<rf::FullWave> solved;
        for (const auto* shapes : {&drawn, &whole}) {
            const rf::Processed done = rf::processShapes(*shapes, {}, how, stack);
            QCOMPARE(done.conductors.size(), 1);
            const rf::Mesh mesh = rf::meshOf(done.conductors, settings, stack, 3e9);
            QVERIFY2(mesh.ok() && mesh.warnings.isEmpty(), qPrintable((mesh.problems + mesh.warnings).join(" | ")));
            solved << rf::fullWaveOf(mesh, done.conductors, stack, ports, {2e9});
            QVERIFY2(solved.last().ok(), qPrintable(solved.last().problems.join(" | ")));
        }
        // (To 1e-7: the join moves the outline by the union's nanometre, which
        //  is 3e-9 of Z here; the stub cut off is all of it.)
        const Matrix2 a = zOf(solved.at(0), 0), b = zOf(solved.at(1), 0);
        QVERIFY2((a - b).norm() <= 1e-7 * b.norm(), qPrintable(QStringLiteral("|dZ| %1 of %2").arg((a - b).norm()).arg(b.norm())));
        // And its S21 is a through's, not a stub's cut off.
        const std::complex<double> s21 = 2.0 * a(1, 0) * 50.0 / ((a(0, 0) + 50.0) * (a(1, 1) + 50.0) - a(0, 1) * a(1, 0));
        QVERIFY2(std::abs(s21) > 0.9, qPrintable(QString::number(std::abs(s21))));
        // Unmerged, a line cut along a slant is still one line: its two
        // pieces meet along the slant, cell for cell, a through's S21 and
        // no warning.
        rf::Conductor left, right;
        left.layer = right.layer = QStringLiteral("M1");
        left.outline = QPolygonF({QPointF(0, 0), QPointF(10000, 0), QPointF(12000, 4000), QPointF(0, 4000)});
        right.outline = QPolygonF({QPointF(10000, 0), QPointF(30000, 0), QPointF(30000, 4000), QPointF(12000, 4000)});
        const QList<rf::FullWavePort> ends = {edgePort(QPointF(0, 2000), QString(), QPointF(-1, 0)),
                                              edgePort(QPointF(30000, 2000), QString(), QPointF(1, 0))};
        const rf::FullWave slant = rf::fullWaveOf(rf::meshOf({left, right}, settings, stack, 3e9), {left, right}, stack, ends, {2e9});
        QVERIFY2(slant.ok() && slant.warnings.isEmpty(), qPrintable((slant.problems + slant.warnings).join(" | ")));
        const Matrix2 zs = zOf(slant, 0);
        const std::complex<double> through = 2.0 * zs(1, 0) * 50.0 / ((zs(0, 0) + 50.0) * (zs(1, 1) + 50.0) - zs(0, 1) * zs(1, 0));
        QVERIFY2(std::abs(through) > 0.9, qPrintable(QString::number(std::abs(through))));
        // Two shapes over each other, unmerged, share no cell's edge: the
        // solve's warnings say so, naming them.
        const QList<rf::Conductor> overlapping = {strip(0, 0, 20000, 4000), strip(9000, 700, 21000, 3300)};
        const rf::FullWave apart = rf::fullWaveOf(rf::meshOf(overlapping, settings, stack, 3e9), overlapping, stack, ends, {2e9});
        QVERIFY2(apart.ok(), qPrintable(apart.problems.join(" | ")));
        QVERIFY2(apart.warnings.size() == 1 && apart.warnings.first().contains(QStringLiteral("no current crosses")),
                 qPrintable(apart.warnings.join(" | ")));
    }


    // GMRES keeps its hold as the mesh is refined: a comb of 1 µm fingers in
    // 1 µm gaps, sixteen cells across them, held compressed, converges at
    // 100 GHz in a few dozen iterations - 1253 with the near field's
    // preconditioner reaching the touching cells alone, and on the
    // reviewers' twelve-finger comb none at all past ten across (B3 of the
    // open items of 8 November). Its bar is told how far GMRES has got. A
    // GMRES held near its limit says so; held short of its residual, every
    // frequency is still solved, and each says it is near, not exact.
    void gmresKeepsItsHoldAsTheMeshIsRefined()
    {
        if (slowBuild()) QSKIP("a compressed solve of 21 000 unknowns: Release only");
        const rf::Stack stack = grounded(4.1, 4e-6);
        QList<rf::Conductor> comb = {strip(0, -3, 7, 3), strip(0, 12, 7, 3)};
        for (int i = 0; i < 4; ++i) comb << (i % 2 == 0 ? strip(2 * i, 0, 1, 11) : strip(2 * i, 1, 1, 11));
        rf::MeshSettings how;
        how.cellsAcross = 16;
        const rf::Mesh mesh = rf::meshOf(comb, how, stack, 100e9);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        const QList<rf::FullWavePort> ports = {edgePort(QPointF(3.5, -3), QString(), QPointF(0, -1)),
                                               edgePort(QPointF(3.5, 15), QString(), QPointF(0, 1))};
        std::atomic_int iterations{0};
        std::atomic<double> residual{0};
        rf::FullWaveControl control;
        control.matrix = rf::FullWaveControl::Matrix::Compressed;
        control.gmresIterations = &iterations;
        control.gmresResidual = &residual;
        const rf::FullWave w = rf::fullWaveOf(mesh, comb, stack, ports, {100e9}, 1e-6, &control);
        QVERIFY2(w.ok() && w.compressed, qPrintable(w.problems.join(" | ")));
        qWarning("%lld cells: %d GMRES iterations at most, residual %.2g", mesh.cells.size(), w.iterations, w.residual);
        QVERIFY2(w.iterations > 0 && w.iterations <= 60, qPrintable(QString::number(w.iterations)));
        QVERIFY2(w.warnings.isEmpty(), qPrintable(w.warnings.join(" | ")));
        QVERIFY2(iterations.load() >= 1 && iterations.load() <= w.iterations && residual.load() > 0,
                 qPrintable(QStringLiteral("%1 %2").arg(iterations.load()).arg(residual.load())));
        // Its limit just past what it needs: said to be near it.
        rf::FullWaveControl tight;
        tight.matrix = rf::FullWaveControl::Matrix::Compressed;
        tight.mostIterations = w.iterations + 1;
        const rf::FullWave near = rf::fullWaveOf(mesh, comb, stack, ports, {100e9}, 1e-6, &tight);
        QVERIFY2(near.ok() && near.warnings.size() == 1 && near.warnings.first().contains(QStringLiteral("near its limit")),
                 qPrintable((near.problems + near.warnings).join(" | ")));
        // Short of it, at two frequencies: both solved, both said near.
        tight.mostIterations = std::max(1, w.iterations / 2);
        const rf::FullWave shortOf = rf::fullWaveOf(mesh, comb, stack, ports, {50e9, 100e9}, 1e-6, &tight);
        QVERIFY2(shortOf.ok() && shortOf.z.size() == 2, qPrintable(shortOf.problems.join(" | ")));
        QVERIFY2(shortOf.warnings.size() == 2 && shortOf.warnings.at(1).contains(QStringLiteral("stopped short of its residual")),
                 qPrintable(shortOf.warnings.join(" | ")));
        // And its S near the converged one's.
        const Matrix2 a = zOf(w, 0), b = zOf(shortOf, 1);
        QVERIFY2((a - b).norm() < 1e-3 * a.norm(), qPrintable(QStringLiteral("%1 of %2").arg((a - b).norm()).arg(a.norm())));
    }


    // A MOM comb of 1 µm metal in 1 µm gaps solved as its two faces holds the
    // charge on its side walls (wallGapsOf()): within 3 % of Palace's 31.8 fF
    // at six cells across, its rise from 1 to 100 GHz within 1 % of Palace's
    // 1.135. Without the walls it read 25.1 fF, 21 % under (B2 of the open
    // items of 8 November). (The quasi-static solve and the thin limit are
    // the two functions after it: the three were one, past QtTest's 300 s on
    // CI's macOS 15 runner.) Not where Eigen's LU factorizes the near field
    // the compressed solve is preconditioned with - before macOS 26 (CI's
    // runner has 15), on Linux, or chosen: its fill took the process to
    // 6.7 GB on an M2 Ultra (3.7 by Accelerate's), and on CI's runner of
    // 7 GB, beside the other tests, it swapped for 800 s and more until
    // ctest's 900 s ended it, the tests beside it twice as slow.
    void theSideWallsChargeIsInAThickCombsCapacitance()
    {
        if (slowBuild()) QSKIP("compressed solves of 45 000 unknowns: Release only");
        if (!qucs_s::fem::acceleratedLu())
            QSKIP(qPrintable(QStringLiteral("Eigen's LU (%1): the near field's fill takes 6.7 GB")
                                 .arg(qucs_s::fem::acceleratedLuHindrance().isEmpty() ? QStringLiteral("chosen") : qucs_s::fem::acceleratedLuHindrance())));
        const QList<rf::Conductor> c = momComb();
        rf::MeshSettings how;
        how.cellsAcross = 6;
        const rf::Stack thick = combStack(1e-6, true);
        const rf::Mesh mesh = rf::meshOf(c, how, thick, 100e9);
        rf::FullWaveControl control;
        control.matrix = rf::FullWaveControl::Matrix::Compressed;
        const rf::FullWave w = rf::fullWaveOf(mesh, c, thick, combPorts(), {1e9, 100e9}, 1e-6, &control);
        QVERIFY2(w.ok(), qPrintable(w.problems.join(" | ")));
        const double c1 = combCapacitance(w, 0, 1e9), c100 = combCapacitance(w, 1, 100e9);
        qWarning("two faces: C %.3f fF at 1 GHz, %.3f at 100 GHz, the rise %.4f", c1 * 1e15, c100 * 1e15, c100 / c1);
        QVERIFY2(std::abs(c1 - 31.8e-15) < 0.03 * 31.8e-15, qPrintable(QString::number(c1 * 1e15)));
        QVERIFY2(std::abs(c100 / c1 - 1.135) < 0.01 * 1.135, qPrintable(QString::number(c100 / c1)));
        QVERIFY2(std::any_of(w.lines.begin(), w.lines.end(), [](const QString& l) { return l.contains(QStringLiteral("side walls across their narrow gaps")); }),
                 qPrintable(w.lines.join(" | ")));
    }

    // The same comb's quasi-static solve, the same walls, the full wave's at
    // 1 GHz within 1 % (at three cells across, within its dense limit).
    void aThickCombsQuasiStaticSolveIsItsFullWave()
    {
        if (slowBuild()) QSKIP("compressed solves of 45 000 unknowns: Release only");
        const QList<rf::Conductor> c = momComb();
        rf::MeshSettings coarse;
        coarse.cellsAcross = 3;
        const rf::Stack thick = combStack(1e-6, true);
        rf::FullWaveControl control;
        control.matrix = rf::FullWaveControl::Matrix::Compressed;
        const rf::Mesh mesh3 = rf::meshOf(c, coarse, thick, 100e9);
        const rf::FullWave w3 = rf::fullWaveOf(mesh3, c, thick, combPorts(), {1e9}, 1e-6, &control);
        const rf::Extraction e = rf::extract(mesh3, c, thick);
        QVERIFY2(w3.ok() && e.ok() && e.c.size() == 2, qPrintable((w3.problems + e.problems).join(" | ")));
        qWarning("three across: quasi-static %.3f fF, full wave %.3f", e.c.at(0).at(1) * 1e15, combCapacitance(w3, 0, 1e9) * 1e15);
        QVERIFY2(std::abs(e.c.at(0).at(1) - combCapacitance(w3, 0, 1e9)) < 0.01 * e.c.at(0).at(1),
                 qPrintable(QStringLiteral("%1 against %2").arg(e.c.at(0).at(1)).arg(combCapacitance(w3, 0, 1e9))));
    }

    // In the thin limit the comb's two faces 0.01 µm apart are the sheet,
    // which never converged as two faces: within 0.1 % of it, and within 1 %
    // of Palace's 14.45 fF.
    void aThinCombsTwoFacesAreItsSheet()
    {
        if (slowBuild()) QSKIP("compressed solves of 45 000 unknowns: Release only");
        const QList<rf::Conductor> c = momComb();
        rf::MeshSettings how;
        how.cellsAcross = 6;
        rf::FullWaveControl control;
        control.matrix = rf::FullWaveControl::Matrix::Compressed;
        const rf::Stack film = combStack(0.01e-6, true), sheet = combStack(0, false);
        const rf::FullWave wf = rf::fullWaveOf(rf::meshOf(c, how, film, 100e9), c, film, combPorts(), {1e9}, 1e-6, &control);
        const rf::FullWave ws = rf::fullWaveOf(rf::meshOf(c, how, sheet, 100e9), c, sheet, combPorts(), {1e9}, 1e-6, &control);
        QVERIFY2(wf.ok() && ws.ok(), qPrintable((wf.problems + ws.problems).join(" | ")));
        const double cf = combCapacitance(wf, 0, 1e9), cs = combCapacitance(ws, 0, 1e9);
        qWarning("0.01 µm two faces %.3f fF, the sheet %.3f", cf * 1e15, cs * 1e15);
        QVERIFY2(std::abs(cf - cs) < 1e-3 * cs && std::abs(cf - 14.45e-15) < 0.01 * 14.45e-15,
                 qPrintable(QStringLiteral("%1 %2").arg(cf * 1e15).arg(cs * 1e15)));
        QVERIFY2(std::any_of(wf.lines.begin(), wf.lines.end(), [](const QString& l) { return l.contains(QStringLiteral("is solved as a sheet")); }),
                 qPrintable(wf.lines.join(" | ")));
    }

};

QTEST_MAIN(TestRf25Fullwave)
#include "test_rf25_fullwave.moc"
