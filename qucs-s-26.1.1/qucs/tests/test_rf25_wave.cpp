/*
 * test_rf25_wave.cpp - the retarded Green's functions of a layered stack
 * (rf25/rf25_wave.h), held to what has a closed form: Bessel's J0 of a
 * complex argument; the spectral kernels of a grounded slab (Mosig's); a
 * charge and a current over a ground plane in one medium, which are
 * themselves and their images at any frequency; and the static Green's
 * function of phase B, which they must become as the frequency falls.
 * Then what has none: that they are reciprocal, that the path the
 * integral takes off the real axis makes no difference, and that the
 * table is the integral. And a via's kernels, against a grounded slab's
 * line Green's function integrated by hand and against the other form the
 * formulation allows.
 *
 * Phase C of docs/proposals/2026-10-23-rf-2p5d-moment-method.md.
 */
#include <QtTest>

#include "rf25/rf25_green.h"
#include "rf25/rf25_wave.h"

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

constexpr double kMu0 = 4e-7 * M_PI;
const Complex kJ(0, 1);

/// A stack of slabs, built by hand: each (eps_r, thickness, tan delta),
/// from the bottom up.
rf::Layers stackOf(const QList<std::array<double, 3>>& each, bool ground, double below = 1.0, double above = 1.0)
{
    rf::Layers l;
    double z = 0;
    for (const auto& it : each) {
        rf::Layers::Slab s;
        s.z0 = z;
        s.z1 = z + it[1];
        s.epsr = it[0];
        s.tand = it[2];
        s.name = QStringLiteral("D%1").arg(l.slabs.size());
        l.slabs << s;
        z = s.z1;
    }
    l.groundBelow = ground;
    l.epsrBelow = below;
    l.epsrAbove = above;
    return l;
}

/// J0 by the trapezoidal rule on its integral (1/2 pi) Int cos(z sin t) dt
/// over a period - which for a periodic analytic integrand converges
/// faster than any power: the reference the series and the asymptotic form
/// are held to, resting on neither.
Complex referenceJ0(Complex z)
{
    const int n = 400;
    Complex sum(0, 0);
    for (int i = 0; i < n; ++i) sum += std::cos(z * std::sin(2 * M_PI * i / n));
    return sum / double(n);
}

/// tanh of a complex argument that may be large: std::tanh's overflows.
Complex tanhOf(Complex x)
{
    if (x.real() > 20) {
        const Complex e = std::exp(-2.0 * x);
        return (1.0 - e) / (1.0 + e);
    }
    return std::tanh(x);
}

/// u = j kz = sqrt(kr^2 - k^2), on the branch the solver takes.
Complex uOf(Complex k2, Complex kr)
{
    Complex kz = std::sqrt(k2 - kr * kr);
    if (kz.imag() > 0) kz = -kz;
    return kJ * kz;
}

double relative(Complex got, Complex want)
{
    return std::abs(got - want) / std::abs(want);
}

/*!
 * Int_0^h Int_0^h I_v(z|z') dz dz' of a unit series voltage source on a line
 * of impedance \a z, u = j kz, in one slab of thickness \a h whose faces
 * reflect the voltage by \a gu (its top, looking up) and \a gd (its bottom,
 * looking down): the slab's own Green's function - the direct wave, its
 * image in each face, and the waves that go between the two - integrated
 * term by term in closed form. A route to the via's coupling that shares
 * nothing with the solver's, which takes the voltages of a shunt source
 * between the faces.
 */
Complex slabCurrents(Complex z, Complex u, double h, Complex gu, Complex gd)
{
    const Complex e = std::exp(-u * h), e2 = e * e;
    const Complex d = 1.0 - gu * gd * e2;
    const Complex same = 2.0 * h / u - 2.0 * (1.0 - e) / (u * u);         // exp(-u |s - s'|)
    const Complex face = (1.0 - e) * (1.0 - e) / (u * u);                  // exp(-u (s + s')), and the top's
    const Complex far = e2 * same;                                         // exp(-u (2h + |s - s'|))
    const Complex near = -2.0 * h * e2 / u + 2.0 * (e - e2) / (u * u);     // exp(-u (2h - |s - s'|))
    return (0.5 * same - gd / (2.0 * d) * face - gu / (2.0 * d) * face + gu * gd / (2.0 * d) * (far + near)) / z;
}

/// Int_0^h V_v(h|z') dz' on the same line: the voltage at the slab's top of
/// a series source anywhere in it, the direct wave and its reflections.
Complex slabTopVoltage(Complex u, double h, Complex gu, Complex gd)
{
    const Complex e = std::exp(-u * h), e2 = e * e;
    const Complex d = 1.0 - gu * gd * e2;
    return (1.0 - e) / u * (0.5 + gu / (2.0 * d) - gd * (1.0 + gu) * e / (2.0 * d) + gd * gu * e2 / (2.0 * d));
}

} // namespace

class TestRf25Wave : public QObject
{
    Q_OBJECT

private slots:
    // J0 of a complex argument, against its periodic integral: below 12,
    // where the power series answers; above, where Hankel's form does;
    // off the real axis, where the integral of a half-ellipse needs it;
    // and its symmetries.
    void theBesselFunctionOfAComplexArgumentIsJ0()
    {
        const QList<Complex> points{Complex(0.3, 0),   Complex(2.404825557695773, 0), Complex(7.5, 0),
                                    Complex(11.9, 0),  Complex(12.1, 0),              Complex(19.5, 0),
                                    Complex(1, 1),     Complex(3, -2),                Complex(11.5, 3),
                                    Complex(14, 2.5),  Complex(0, 2),                 Complex(25, 0.5)};
        for (const Complex& z : points) {
            const Complex got = rf::besselJ0(z), want = referenceJ0(z);
            QVERIFY2(std::abs(got - want) <= 2e-11 * std::max(1.0, std::abs(want)),
                     qPrintable(QStringLiteral("J0(%1 %2j): %3 %4j vs %5 %6j")
                                    .arg(z.real()).arg(z.imag()).arg(got.real(), 0, 'g', 15).arg(got.imag(), 0, 'g', 15)
                                    .arg(want.real(), 0, 'g', 15).arg(want.imag(), 0, 'g', 15)));
        }
        // The first zero; I0(2) = J0(2j); and the real J0 of phase B, to its
        // own 1e-8, all the way out.
        QVERIFY(std::abs(rf::besselJ0(Complex(2.404825557695773, 0))) < 1e-12);
        QVERIFY(std::abs(rf::besselJ0(Complex(0, 2)) - 2.2795853023360673) < 1e-12);
        for (double x = 0; x < 60; x += 0.37)
            QVERIFY2(std::abs(rf::besselJ0(Complex(x, 0)) - rf::besselJ0(x)) < 2e-8, qPrintable(QString::number(x)));
        // Even, and real on the real axis.
        for (const Complex& z : {Complex(4, 1.5), Complex(17, -2)}) {
            QVERIFY(std::abs(rf::besselJ0(-z) - rf::besselJ0(z)) < 1e-12 * std::abs(rf::besselJ0(z)));
            QVERIFY(std::abs(rf::besselJ0(std::conj(z)) - std::conj(rf::besselJ0(z))) < 1e-12 * std::abs(rf::besselJ0(z)));
        }
    }

    // Hankel's H0 of either kind, for the integral's tail: against its
    // integral (2/(j pi)) Int_0^inf exp(j z cosh t) dt in the upper
    // half-plane, and H0(2)(z) = conj H0(1)(conj z) in the lower - and on
    // the real axis, J0 +- j Y0 at Y0's tabled values.
    void hankelsFunctionsAreH0()
    {
        const auto reference = [](Complex z) {
            // (Simpson's rule out to where exp(-Im z cosh t) is nothing.)
            const double end = std::acosh(60.0 / z.imag() + 1);
            const int n = 400000;
            const double h = end / n;
            Complex sum(0, 0);
            for (int i = 0; i <= n; ++i) {
                const double w = (i == 0 || i == n) ? 1 : (i % 2 ? 4 : 2);
                sum += w * std::exp(kJ * z * std::cosh(i * h));
            }
            return 2.0 / (kJ * M_PI) * sum * h / 3.0;
        };
        for (const Complex& z : {Complex(3.5, 0.7), Complex(9, 2), Complex(11.5, 1), Complex(12.5, 1.5), Complex(20, 4), Complex(4, 6)}) {
            const Complex want = reference(z);
            QVERIFY2(std::abs(rf::hankelH0(z, 1) - want) < 1e-9 * std::abs(want),
                     qPrintable(QStringLiteral("H0(1)(%1 %2j): %3").arg(z.real()).arg(z.imag()).arg(std::abs(rf::hankelH0(z, 1) - want) / std::abs(want))));
            QVERIFY2(std::abs(rf::hankelH0(std::conj(z), 2) - std::conj(want)) < 1e-9 * std::abs(want),
                     qPrintable(QStringLiteral("H0(2)(%1 %2j)").arg(z.real()).arg(-z.imag())));
        }
        const QList<QPair<double, double>> tabled{{1.0, 0.088256964215676957}, {2.0, 0.51037567264974512},
                                                  {5.0, -0.30851762524903376}, {10.0, 0.055671167283599392}};
        for (const auto& [x, y0] : tabled) {
            QVERIFY(std::abs(rf::hankelH0(Complex(x, 0), 1) - (rf::besselJ0(Complex(x, 0)) + kJ * y0)) < 1e-12);
            QVERIFY(std::abs(rf::hankelH0(Complex(x, 0), 2) - (rf::besselJ0(Complex(x, 0)) - kJ * y0)) < 1e-12);
        }
    }

    // The spectral kernels of a strip on a grounded slab, air above, are
    // Mosig's closed forms: G_A = mu0 / D_TE, and
    // G_V = (u0 + u1 tanh u1h) / (eps0 D_TE D_TM), D_TE = u0 + u1 coth u1h,
    // D_TM = eps_r u0 + u1 tanh u1h. At points on the ellipse the integral
    // takes and on the real axis past it, lossless and lossy - which holds
    // the lines, their reflections, the source's split and the sign of the
    // scalar kernel to them in one go.
    void theSpectralKernelsAreThoseOfAGroundedSlab()
    {
        const double h = 254e-6, f = 24e9, w = 2 * M_PI * f;
        for (const double tand : {0.0, 0.02}) {
            const double epsr = 9.8;
            const rf::Layers l = stackOf({{epsr, h, tand}}, true);
            const rf::WaveGreen g(l, h, h, f, 1e-3);
            QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
            const Complex er(epsr, -epsr * tand);
            // (From mu0 eps0, as the solver takes it: CODATA's eps0 with
            // mu0 = 4 pi 1e-7 makes mu0 eps0 c^2 one less 5.5e-10, which
            // is what this test measures to.)
            const double k0 = w * std::sqrt(kMu0 * rf::kEps0);
            const Complex k02(k0 * k0, 0), k12 = k0 * k0 * er;
            for (const Complex kr : {Complex(0.2 * k0, 0.1 * k0), Complex(1.4 * k0, 0.3 * k0), Complex(2.6 * k0, 0.2 * k0),
                                     Complex(4.0 * k0, 0), Complex(30 * k0, 0), Complex(2e3 * k0, 0)}) {
                const Complex u0 = uOf(k02, kr), u1 = uOf(k12, kr);
                const Complex t = tanhOf(u1 * h);
                const Complex dte = u0 + u1 / t, dtm = er * u0 + u1 * t;
                const Complex wantA = kMu0 / dte;
                const Complex wantV = (u0 + u1 * t) / (rf::kEps0 * dte * dtm);
                QVERIFY2(relative(g.vectorSpectral(kr), wantA) < 1e-10,
                         qPrintable(QStringLiteral("tand %1, kr %2+%3j: G_A off by %4")
                                        .arg(tand).arg(kr.real() / k0).arg(kr.imag() / k0).arg(relative(g.vectorSpectral(kr), wantA))));
                QVERIFY2(relative(g.scalarSpectral(kr), wantV) < 1e-10,
                         qPrintable(QStringLiteral("tand %1, kr %2+%3j: G_V off by %4")
                                        .arg(tand).arg(kr.real() / k0).arg(kr.imag() / k0).arg(relative(g.scalarSpectral(kr), wantV))));
            }
            // And the singular terms are the interface's: a charge on it has
            // 1/(eps_sub + eps_air), a current mu0/2 and its image in the
            // ground -mu0/2 at twice the height.
            QVERIFY(relative(g.scalarTerm().coefficient, 1.0 / (rf::kEps0 * (er + 1.0))) < 1e-14);
            QVERIFY(std::abs(g.vectorTerm().coefficient - kMu0 / 2) < 1e-20);
            QVERIFY(std::abs(g.vectorImage().coefficient + kMu0 / 2) < 1e-20);
            QCOMPARE(g.vectorImage().dz, 2 * h);
        }
    }

    // One medium over a ground plane, at any frequency, is the source and
    // its image - a charge's opposite charge and a horizontal current's
    // opposite current - each a spherical wave. Exact, so it holds the
    // whole of it: the lines, the ground's short, the singular terms and,
    // for the scalar, whose image is not taken out in closed form, the
    // Sommerfeld integral and its table.
    void overAGroundInOneMediumTheyAreTheSourceAndItsImage()
    {
        const double epsr = 4, zs = 300e-6;
        const rf::Layers l = stackOf({{epsr, 2e-3, 0}}, true, 1.0, epsr);
        for (const double f : {1e9, 10e9, 30e9}) {
            const double k = 2 * M_PI * f * std::sqrt(epsr * kMu0 * rf::kEps0);
            for (const double zf : {300e-6, 500e-6}) {
                const rf::WaveGreen g(l, zf, zs, f, 5e-3);
                QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
                for (const double rho : {1e-6, 3e-5, 4e-4, 1.3e-3, 4.6e-3}) {
                    const double r = std::hypot(rho, zf - zs), ri = std::hypot(rho, zf + zs);
                    const Complex wave = std::exp(-kJ * k * r) / r - std::exp(-kJ * k * ri) / ri;
                    const Complex wantS = wave / (4 * M_PI * rf::kEps0 * epsr), wantA = kMu0 * wave / (4 * M_PI);
                    // Measured against the two parts, which is what a method
                    // of moments integrates: far off they nearly cancel, and
                    // the total is their small difference. (Measured at 9e-9
                    // for the scalar, whose image is the integral's, and 3e-16
                    // for the vector, whose image is in closed form.)
                    const double parts = 1 / r + 1 / ri;
                    const double sizeS = parts / (4 * M_PI * rf::kEps0 * epsr), sizeA = kMu0 * parts / (4 * M_PI);
                    QVERIFY2(std::abs(g.scalar(rho) - wantS) < 1e-7 * sizeS,
                             qPrintable(QStringLiteral("%1 GHz, dz %2 µm, rho %3 µm: scalar off by %4")
                                            .arg(f / 1e9).arg((zf - zs) * 1e6).arg(rho * 1e6).arg(std::abs(g.scalar(rho) - wantS) / sizeS)));
                    QVERIFY2(std::abs(g.vector(rho) - wantA) < 1e-7 * sizeA,
                             qPrintable(QStringLiteral("%1 GHz, dz %2 µm, rho %3 µm: vector off by %4")
                                            .arg(f / 1e9).arg((zf - zs) * 1e6).arg(rho * 1e6).arg(std::abs(g.vector(rho) - wantA) / sizeA)));
                    // The vector potential's closed forms - the current and
                    // its image - are all of it here: nothing is left to
                    // integrate.
                    QVERIFY2(std::abs(g.vectorRemainderByIntegral(rho)) < 1e-12 * sizeA,
                             qPrintable(QStringLiteral("%1 GHz, rho %2 µm: %3 left").arg(f / 1e9).arg(rho * 1e6)
                                            .arg(std::abs(g.vectorRemainderByIntegral(rho)) / sizeA)));
                }
            }
        }
    }

    // As the frequency falls the scalar kernel becomes phase B's static
    // potential - the stack's images, which rest on no integral at all -
    // and the vector kernel the static mu0/(4 pi) (1/R - 1/R') of a current
    // over its ground, dielectrics or not. On one metal and across two.
    void atLowFrequencyTheyAreTheStaticOnes()
    {
        const rf::Layers l = stackOf({{9.8, 100e-6, 0}, {4.0, 3e-6, 0}}, true);
        const double f = 1e6;
        for (const auto& heights : {std::pair{100e-6, 100e-6}, std::pair{103e-6, 103e-6}, std::pair{103e-6, 100e-6}}) {
            const double zf = heights.first, zs = heights.second;
            const rf::WaveGreen g(l, zf, zs, f, 6e-3);
            QVERIFY2(g.ok(), qPrintable(g.problems().join(" | ")));
            const rf::Green still(l, zf, zs);
            QVERIFY2(still.ok(), qPrintable(still.problems().join(" | ")));
            // Measured against the singular part, since over a ground the
            // whole falls as a dipole's 1/rho^3: at 1 mm it is 0.15 % of the
            // direct term, and a part in 1e8 of that is a part in 1e5 of it.
            // (Measured at 6.4e-8 and 5e-12 - the scalar's mostly phase B's
            // own coarsened images.)
            for (const double rho : {1e-6, 1e-5, 1e-4, 1e-3, 5e-3}) {
                const double r = std::hypot(rho, zf - zs), ri = std::hypot(rho, zf + zs);
                const double sizeS = std::abs(g.scalarTerm().coefficient) / (2 * M_PI * r);
                const double want = still.potential(rho);
                QVERIFY2(std::abs(g.scalar(rho) - want) < 3e-7 * sizeS,
                         qPrintable(QStringLiteral("z %1 from %2 µm, rho %3 µm: %4 vs the static %5, off by %6")
                                        .arg(zf * 1e6).arg(zs * 1e6).arg(rho * 1e6).arg(g.scalar(rho).real()).arg(want)
                                        .arg(std::abs(g.scalar(rho) - want) / sizeS)));
                const double wantA = kMu0 / (4 * M_PI) * (1 / r - 1 / ri);
                const double sizeA = kMu0 / (4 * M_PI) * (1 / r + 1 / ri);
                QVERIFY2(std::abs(g.vector(rho) - wantA) < 1e-7 * sizeA,
                         qPrintable(QStringLiteral("z %1 from %2 µm, rho %3 µm: %4 vs the static %5")
                                        .arg(zf * 1e6).arg(zs * 1e6).arg(rho * 1e6).arg(g.vector(rho).real()).arg(wantA)));
            }
        }
    }

    // Reciprocal: the field at one metal of a source at another is the
    // field at the other of the same source at the first - through a
    // lossy substrate and an oxide, at a frequency where the substrate
    // carries surface waves.
    void theyAreReciprocal()
    {
        const rf::Layers l = stackOf({{9.8, 100e-6, 0.002}, {4.0, 3e-6, 0.001}}, true);
        const double f = 20e9, a = 100e-6, b = 103e-6;
        const rf::WaveGreen ab(l, a, b, f, 3e-3), ba(l, b, a, f, 3e-3);
        QVERIFY(ab.ok() && ba.ok());
        QVERIFY(relative(ab.scalarTerm().coefficient, ba.scalarTerm().coefficient) < 1e-14);
        for (const double rho : {5e-6, 5e-5, 5e-4, 2e-3}) {
            const auto whole = [rho](const rf::WaveGreen& g, bool scalar) {
                const rf::WaveGreen::Term t = scalar ? g.scalarTerm() : g.vectorTerm();
                const double r = std::hypot(rho, t.dz);
                Complex v = t.coefficient * std::exp(-kJ * t.k * r) / (2 * M_PI * r);
                if (!scalar && g.vectorImage().coefficient != Complex(0, 0)) {
                    const rf::WaveGreen::Term i = g.vectorImage();
                    const double ri = std::hypot(rho, i.dz);
                    v += i.coefficient * std::exp(-kJ * i.k * ri) / (2 * M_PI * ri);
                }
                return v + (scalar ? g.scalarRemainderByIntegral(rho) : g.vectorRemainderByIntegral(rho));
            };
            QVERIFY2(relative(whole(ab, true), whole(ba, true)) < 1e-8,
                     qPrintable(QStringLiteral("rho %1 µm: scalar %2").arg(rho * 1e6).arg(relative(whole(ab, true), whole(ba, true)))));
            QVERIFY2(relative(whole(ab, false), whole(ba, false)) < 1e-8,
                     qPrintable(QStringLiteral("rho %1 µm: vector %2").arg(rho * 1e6).arg(relative(whole(ab, false), whole(ba, false)))));
        }
    }

    // The path off the real axis makes no difference: a thick lossless
    // substrate at 30 GHz has surface-wave poles on the axis itself, and a
    // low ellipse and a high one, either side of nothing, agree.
    void thePathOffTheAxisMakesNoDifference()
    {
        const double h = 635e-6, f = 30e9;
        const rf::Layers l = stackOf({{9.8, h, 0}}, true);
        const rf::WaveGreen g(l, h, h, f, 3e-3);
        QVERIFY(g.ok());
        const double a = 1.5 * 2 * M_PI * f * std::sqrt(9.8) / rf::kLightSpeed;
        for (const double rho : {0.0, 2e-5, 3e-4, 2e-3}) {
            const double r = std::max(rho, 1e-6);
            const Complex singular = g.scalarTerm().coefficient / (2 * M_PI * r);   // the kernel's size, for the measure
            const Complex low = g.scalarRemainderByIntegral(rho, 0.08 * a), high = g.scalarRemainderByIntegral(rho, 0.5 * a);
            QVERIFY2(std::abs(low - high) < 1e-9 * std::abs(singular),
                     qPrintable(QStringLiteral("rho %1 µm: %2 between the two paths").arg(rho * 1e6).arg(std::abs(low - high) / std::abs(singular))));
            const Complex lowA = g.vectorRemainderByIntegral(rho, 0.08 * a), highA = g.vectorRemainderByIntegral(rho, 0.5 * a);
            QVERIFY2(std::abs(lowA - highA) < 1e-9 * kMu0 / (4 * M_PI * r),
                     qPrintable(QStringLiteral("rho %1 µm: %2 between the two paths").arg(rho * 1e6).arg(std::abs(lowA - highA) / (kMu0 / (4 * M_PI * r)))));
        }
    }

    // The table is the integral: between its nodes, the remainders read
    // from it are what the Sommerfeld integral gives there, to a part in a
    // million of the kernel.
    void theTableIsTheIntegral()
    {
        const rf::Layers l = stackOf({{9.8, 100e-6, 0.002}, {4.0, 3e-6, 0}}, true);
        for (const auto& heights : {std::pair{100e-6, 100e-6}, std::pair{103e-6, 100e-6}}) {
            const rf::WaveGreen g(l, heights.first, heights.second, 40e9, 2e-3);
            g.prepare();
            QVERIFY2(g.tableSize() > 50, qPrintable(QString::number(g.tableSize())));
            for (const double rho : {7.3e-7, 2.9e-6, 1.33e-4, 7.7e-4, 1.37e-3, 1.99e-3}) {
                // (Against the singular part and the remainder together, the
                // sizes of what is added. Measured at 5.5e-9.)
                const double r = std::hypot(rho, g.scalarTerm().dz);
                const Complex table = g.scalarRemainder(rho), integral = g.scalarRemainderByIntegral(rho);
                const double sizeS = std::abs(g.scalarTerm().coefficient) / (2 * M_PI * r) + std::abs(integral);
                QVERIFY2(std::abs(table - integral) < 1e-7 * sizeS,
                         qPrintable(QStringLiteral("rho %1 µm: scalar %2").arg(rho * 1e6).arg(std::abs(table - integral) / sizeS)));
                const Complex tableA = g.vectorRemainder(rho), integralA = g.vectorRemainderByIntegral(rho);
                const double sizeA = std::abs(g.vectorTerm().coefficient) / (2 * M_PI * r) + std::abs(integralA);
                QVERIFY2(std::abs(tableA - integralA) < 1e-7 * sizeA,
                         qPrintable(QStringLiteral("rho %1 µm: vector %2").arg(rho * 1e6).arg(std::abs(tableA - integralA) / sizeA)));
            }
            // Past its end it is the integral itself.
            QVERIFY(std::abs(g.scalarRemainder(3e-3) - g.scalarRemainderByIntegral(3e-3)) < 1e-12 * std::abs(g.scalar(3e-3)));
        }
    }

    // A conductive substrate is a permittivity like any other, its
    // conductivity in the imaginary part - and it behaves as one should.
    // Silicon of 10 S/m relaxes a charge in some 1e-11 s, so at 100 MHz it
    // holds charge as a ground would: near a metal on 2 µm of oxide over
    // it, the kernel is that of the oxide on a ground plane (in size,
    // measured within 1.4e-5 at 0.5 µm and 1.1e-4 at 2 µm). Nearer its relaxation
    // frequency, some 15 GHz, it is no ground any more (3 % and 23 % off at
    // 10 GHz) - and with its conductivity left out it would be a dielectric
    // of 11.7, nothing like one.
    void aConductiveSubstrateHoldsChargeLikeAGround()
    {
        rf::Layers si = stackOf({{11.7, 300e-6, 0}, {3.9, 2e-6, 0}}, false);
        si.slabs[0].sigma = 10;
        const double top = si.slabs.last().z1;
        const rf::Layers ground = stackOf({{3.9, 2e-6, 0}}, true);
        const double oxide = ground.slabs.last().z1;
        const rf::WaveGreen slow(si, top, top, 1e8, 1e-4), groundSlow(ground, oxide, oxide, 1e8, 1e-4);
        QVERIFY2(slow.ok(), qPrintable(slow.problems().join(" | ")));
        // In size; and in phase all but its loss: its image charge falls
        // short of a ground's by about 2 eps_ox w / sigma, 4.3e-3 at 100 MHz,
        // which shows in the phase as far as the image counts in the kernel
        // - measured 6.6e-4 rad at 0.5 µm, and 4.3e-3 at 2 µm, where the
        // charge and its image nearly cancel and the shortfall is what is
        // left.
        double lag = 0;
        for (const double rho : {5e-7, 2e-6}) {
            const Complex ratio = slow.scalar(rho) / groundSlow.scalar(rho);
            QVERIFY2(std::abs(std::abs(ratio) - 1.0) < 3e-4,
                     qPrintable(QStringLiteral("100 MHz, rho %1 µm: %2 off a ground in size").arg(rho * 1e6).arg(std::abs(ratio) - 1.0)));
            QVERIFY2(std::abs(std::arg(ratio)) < 1e-2,
                     qPrintable(QStringLiteral("100 MHz, rho %1 µm: %2 rad off a ground").arg(rho * 1e6).arg(std::arg(ratio))));
            QVERIFY2(std::abs(std::arg(ratio)) > lag, "the loss's lag does not grow with the image's share");
            lag = std::abs(std::arg(ratio));
        }
        const rf::WaveGreen fast(si, top, top, 1e10, 1e-4), groundFast(ground, oxide, oxide, 1e10, 1e-4);
        QVERIFY2(std::abs(fast.scalar(2e-6) / groundFast.scalar(2e-6) - 1.0) > 0.05, "silicon near its relaxation is still a ground");
        // A metal's height as a model gives it - 302 µm, written so - is a
        // rounding step off the 300 + 2 the stack sums, and is on its top.
        const rf::WaveGreen written(si, 302e-6, 302e-6, 1e8, 1e-4);
        QVERIFY2(written.ok(), qPrintable(written.problems().join(" | ")));
        QVERIFY(std::abs(written.scalar(2e-6) - slow.scalar(2e-6)) < 1e-12 * std::abs(slow.scalar(2e-6)));
        rf::Layers dielectric = si;
        dielectric.slabs[0].sigma = 0;
        const rf::WaveGreen plain(dielectric, top, top, 1e8, 1e-4);
        QVERIFY2(std::abs(plain.scalar(2e-6) / groundSlow.scalar(2e-6) - 1.0) > 0.05, "a dielectric of 11.7 holds charge as a ground");
    }

    // What it cannot answer is said: no frequency, a height in no slab, a
    // metal on the ground plane itself.
    void whatItCannotAnswerIsSaid()
    {
        const rf::Layers l = stackOf({{9.8, 100e-6, 0}}, true);
        QVERIFY(!rf::WaveGreen(l, 100e-6, 100e-6, 0, 1e-3).ok());
        QVERIFY(!rf::WaveGreen(l, 100e-6, 100e-6, std::nan(""), 1e-3).ok());
        const rf::WaveGreen above(l, 300e-6, 100e-6, 1e9, 1e-3);
        QVERIFY(!above.ok());
        QVERIFY2(above.problems().join(' ').contains(QStringLiteral("in no slab")), qPrintable(above.problems().join(' ')));
        const rf::WaveGreen shorted(l, 0, 100e-6, 1e9, 1e-3);
        QVERIFY(!shorted.ok());
        QVERIFY2(shorted.problems().join(' ').contains(QStringLiteral("ground plane")), qPrintable(shorted.problems().join(' ')));
        QCOMPARE(shorted.scalar(1e-4), Complex(0, 0));
        rf::Layers none;
        QVERIFY(!rf::WaveGreen(none, 0, 0, 1e9, 1e-3).ok());
    }

    // A via through a grounded slab, from the ground to its top: its
    // coupling to itself, mu0/eps Int Int I_v^e, and its potential at its
    // top, -j (w mu0/kr^2) Int (V_v^e - V_v^h), against the slab's own line
    // Green's function summed and integrated by hand (slabCurrents,
    // slabTopVoltage) - lossless and lossy, on the ellipse and the axis.
    void aViasKernelsAreItsLinesIntegratedAlongIt()
    {
        const double h = 254e-6, f = 24e9, w = 2 * M_PI * f;
        const double k0 = w * std::sqrt(kMu0 * rf::kEps0);
        for (const double tand : {0.0, 0.02}) {
            const double epsr = 9.8;
            const rf::Layers l = stackOf({{epsr, h, tand}}, true);
            const rf::ViaGreen via(l, {0, h}, {{0, h}}, {h}, f, 1e-3);
            QVERIFY2(via.ok(), qPrintable(via.problems().join(" | ")));
            const Complex er(epsr, -epsr * tand), e1 = rf::kEps0 * er;
            for (const Complex kr : {Complex(0.2 * k0, 0.1 * k0), Complex(1.4 * k0, 0.3 * k0), Complex(2.6 * k0, 0.2 * k0),
                                     Complex(4.0 * k0, 0), Complex(30 * k0, 0), Complex(400 * k0, 0)}) {
                const Complex u0 = uOf(Complex(k0 * k0, 0), kr), u1 = uOf(k0 * k0 * er, kr);
                const Complex kz0 = -kJ * u0, kz1 = -kJ * u1;
                const Complex ze0 = kz0 / (w * rf::kEps0), ze1 = kz1 / (w * e1);
                const Complex zh0 = w * kMu0 / kz0, zh1 = w * kMu0 / kz1;
                const Complex gue = (ze0 - ze1) / (ze0 + ze1), guh = (zh0 - zh1) / (zh0 + zh1);
                const Complex wantCoupling = kMu0 * slabCurrents(ze1, u1, h, gue, -1.0) / e1;
                const Complex wantPotential = -kJ * w * kMu0 / (kr * kr)
                                              * (slabTopVoltage(u1, h, gue, -1.0) - slabTopVoltage(u1, h, guh, -1.0));
                QVERIFY2(relative(via.couplingSpectral(0, kr), wantCoupling) < 1e-10,
                         qPrintable(QStringLiteral("tand %1, kr %2+%3j: coupling off by %4")
                                        .arg(tand).arg(kr.real() / k0).arg(kr.imag() / k0)
                                        .arg(relative(via.couplingSpectral(0, kr), wantCoupling))));
                QVERIFY2(relative(via.potentialSpectral(0, kr), wantPotential) < 1e-9,
                         qPrintable(QStringLiteral("tand %1, kr %2+%3j: potential off by %4")
                                        .arg(tand).arg(kr.real() / k0).arg(kr.imag() / k0)
                                        .arg(relative(via.potentialSpectral(0, kr), wantPotential))));
            }
            // The closed forms are the slab's: the direct term, its image in
            // the ground the same current, in the air face -Gamma of it.
            const QList<rf::ViaGreen::Term> terms = via.couplingTerms(0);
            QCOMPARE(terms.size(), 3);
            const Complex half = kJ * w * kMu0 / 2.0, gamma = (er - 1.0) / (er + 1.0);
            QVERIFY(relative(terms.at(0).coefficient, half) < 1e-14);
            QVERIFY(relative(terms.at(1).coefficient, -gamma * half) < 1e-14);
            QVERIFY(relative(terms.at(2).coefficient, half) < 1e-14);
            QCOMPARE(terms.at(1).b0, h);
            QCOMPARE(terms.at(1).b1, 2 * h);
            QCOMPARE(terms.at(2).b0, -h);
            QCOMPARE(terms.at(2).b1, 0.0);
            QVERIFY(relative(via.potentialTail(0).coefficient, -kJ * w * kMu0 * gamma / 2.0) < 1e-14);
            QCOMPARE(via.potentialTail(0).d, h);
        }
    }

    // Formulation C has two forms of a via's coupling to another: G_zz with
    // the scalar correction's potential at the via's two ends, which the
    // solver takes, and the symmetric mu0 I_v^e (1/eps + 1/eps') - (w^2
    // mu0^2/kr^2)(I_v^e - I_v^h), which folds the two together. They rest on
    // the same line voltages through different sums, so holding them to one
    // another holds the sums - and the symmetric one to the two vias either
    // way round. Vias to the ground and between metals, in one slab and
    // across two, with no ground under the stack at all.
    void theTwoFormsOfAViasCouplingAgree()
    {
        const double f = 12e9, k0 = 2 * M_PI * f * std::sqrt(kMu0 * rf::kEps0);
        struct Case {
            rf::Layers layers;
            QList<rf::ViaGreen::Span> vias;
        };
        const double a = 100e-6, b = 30e-6, c = 8e-6;
        const QList<Case> cases = {
            {stackOf({{9.8, a, 0.01}, {3.9, b, 0}, {7.0, c, 0}}, true), {{0, a}, {a, a + b}, {a, a + b + c}, {0, a + b}}},
            {stackOf({{11.7, a, 0}, {4.2, b, 0.002}}, false, 11.7, 1.0), {{0, a}, {a, a + b}, {0, a + b}}},
        };
        for (const Case& it : cases) {
            QList<rf::ViaGreen> greens;
            for (const rf::ViaGreen::Span& s : it.vias) {
                QList<double> ends;
                for (const rf::ViaGreen::Span& o : it.vias) ends << o.z0 << o.z1;
                greens << rf::ViaGreen(it.layers, s, it.vias, ends, f, 1e-3);
                QVERIFY2(greens.last().ok(), qPrintable(greens.last().problems().join(" | ")));
            }
            for (int i = 0; i < it.vias.size(); ++i)
                for (int j = 0; j < it.vias.size(); ++j)
                    for (const Complex kr : {Complex(0.3 * k0, 0.2 * k0), Complex(2.0 * k0, 0.4 * k0), Complex(5 * k0, 0),
                                             Complex(60 * k0, 0), Complex(900 * k0, 0)}) {
                        // j's potential at i's top (height 2i + 1) less at its
                        // bottom (2i).
                        const Complex folded = greens.at(i).couplingSpectral(j, kr)
                                               + greens.at(j).potentialSpectral(2 * i + 1, kr)
                                               - greens.at(j).potentialSpectral(2 * i, kr);
                        const Complex symmetric = greens.at(i).symmetricSpectral(j, kr);
                        QVERIFY2(relative(folded, symmetric) < 1e-9,
                                 qPrintable(QStringLiteral("vias %1 and %2, kr %3+%4j: %5 apart")
                                                .arg(i).arg(j).arg(kr.real() / k0).arg(kr.imag() / k0)
                                                .arg(relative(folded, symmetric))));
                        QVERIFY2(relative(greens.at(j).symmetricSpectral(i, kr), symmetric) < 1e-11,
                                 qPrintable(QStringLiteral("vias %1 and %2, kr %3+%4j: not symmetric, %5")
                                                .arg(i).arg(j).arg(kr.real() / k0).arg(kr.imag() / k0)
                                                .arg(relative(greens.at(j).symmetricSpectral(i, kr), symmetric))));
                    }
        }
    }

    // In one medium over a ground a via is itself and its image, a vertical
    // current's image being the same current: the coupling is the two
    // spherical waves integrated along both vias, which in the spectral
    // domain is the direct term's and the image's closed forms - in two
    // slabs of the same medium as much as in one. For a via in the slab on
    // the ground the closed forms are all of it and nothing is left; the
    // image of a part in a slab above is left to the table, being smooth -
    // it is twice that slab's height away at least. With no face
    // to reflect the two lines apart there is no correction to the potential
    // at all. Then a slab with air over it, where something is left: what
    // the closed forms leave dies away as kr grows - by the direct term and
    // the images being the right ones - and the table is the integral.
    void whatAViasClosedFormsLeaveIsSmallAndTabulated()
    {
        const double h = 200e-6;
        // Int_a Int_b exp(-u |z - z'|) dz dz', by the four terms of its second
        // antiderivative (written here again, not taken from the solver).
        const auto pair = [](Complex u, double a0, double a1, double b0, double b1) {
            const auto second = [&u](double s) { return (std::exp(-u * std::abs(s)) + u * std::abs(s) - 1.0) / (u * u); };
            return second(a1 - b0) - second(a1 - b1) - second(a0 - b0) + second(a0 - b1);
        };
        for (const double f : {2e9, 30e9}) {
            const double w = 2 * M_PI * f, k0 = w * std::sqrt(kMu0 * rf::kEps0);
            const double epsr = 4;
            const rf::Layers one = stackOf({{epsr, h, 0}, {epsr, h, 0}}, true, 1.0, epsr);
            const QList<rf::ViaGreen::Span> spans = {{0, h}, {h, 2 * h}, {0, 2 * h}};
            for (int i = 0; i < spans.size(); ++i) {
                const rf::ViaGreen via(one, spans.at(i), spans, {h, 2 * h}, f, 3e-3);
                QVERIFY2(via.ok(), qPrintable(via.problems().join(" | ")));
                // (Just past the branch point, too, where u h is below a
                //  hundredth and the closed forms' antiderivative takes its
                //  series.)
                for (int j = 0; j < spans.size(); ++j)
                    for (const Complex kr : {Complex(0.4 * k0, 0.3 * k0), Complex(1.03 * k0 * std::sqrt(epsr), 0),
                                             Complex(3 * k0, 0), Complex(80 * k0, 0)}) {
                        const Complex u = uOf(k0 * k0 * epsr, kr);
                        const rf::ViaGreen::Span a = spans.at(i), b = spans.at(j);
                        const Complex want = kJ * w * kMu0 / 2.0 / u * (pair(u, a.z0, a.z1, b.z0, b.z1) + pair(u, a.z0, a.z1, -b.z1, -b.z0));
                        const Complex whole = via.couplingSpectral(j, kr);
                        QVERIFY2(relative(whole, want) < 1e-11,
                                 qPrintable(QStringLiteral("%1 GHz, vias %2 and %3, kr %4: off by %5")
                                                .arg(f / 1e9).arg(i).arg(j).arg(kr.real() / k0).arg(relative(whole, want))));
                        if (i == 0 && j == 0) {
                            const Complex left = via.couplingSpectral(j, kr, true);
                            QVERIFY2(std::abs(left) < 1e-12 * std::abs(whole),
                                     qPrintable(QStringLiteral("%1 GHz, via %2, kr %3: %4 left")
                                                    .arg(f / 1e9).arg(i).arg(kr.real() / k0).arg(std::abs(left) / std::abs(whole))));
                        }
                    }
                for (int height = 0; height < 2; ++height) {
                    QCOMPARE(via.potentialTail(height).coefficient, Complex(0, 0));
                    const Complex p = via.potentialSpectral(height, Complex(3 * k0, 0));
                    const Complex scale = kJ * (2 * M_PI * f * kMu0 / (27 * k0 * k0 * k0));
                    QVERIFY2(std::abs(p) < 1e-12 * std::abs(scale),
                             qPrintable(QStringLiteral("a potential of %1").arg(std::abs(p / scale))));
                }
            }

            const rf::Layers slab = stackOf({{9.8, h, 0.002}, {3.0, h / 4, 0}}, true);
            const QList<rf::ViaGreen::Span> vias = {{0, h}, {h, 1.25 * h}};
            const rf::ViaGreen ground(slab, vias.at(0), vias, {h, 1.25 * h}, f, 3e-3);
            const rf::ViaGreen upper(slab, vias.at(1), vias, {h, 1.25 * h}, f, 3e-3);
            QVERIFY(ground.ok() && upper.ok());
            for (const rf::ViaGreen* via : {&ground, &upper})
                for (int j = 0; j < 2; ++j) {
                    // Past the thinnest slab's own scale: the closed forms
                    // match to (k/kr)^2.
                    const double far = 40.0 / (h / 4);
                    const Complex whole = via->couplingSpectral(j, far), left = via->couplingSpectral(j, far, true);
                    QVERIFY2(std::abs(left) < 1e-4 * std::abs(whole),
                             qPrintable(QStringLiteral("%1 GHz, vias %2 and %3: %4 of it left at kr h/4 = 40")
                                            .arg(f / 1e9).arg(via == &ground ? 0 : 1).arg(j)
                                            .arg(std::abs(left) / std::abs(whole))));
                }
            for (int height = 0; height < 2; ++height) {
                // At the via's ends the potential's tail is its own.
                const double far = 40.0 / (h / 4);
                const Complex whole = ground.potentialSpectral(height, far), left = ground.potentialSpectral(height, far, true);
                if (height == 0) {
                    QVERIFY2(std::abs(left) < 2e-3 * std::abs(whole),
                             qPrintable(QStringLiteral("%1 GHz: %2 of the potential left at the via's top")
                                            .arg(f / 1e9).arg(std::abs(left) / std::abs(whole))));
                }
            }
            // The table is the integral, both kinds of component.
            for (const double rho : {0.0, 3e-6, 7.7e-5, 4.1e-4, 2.2e-3}) {
                for (int j = 0; j < 2; ++j) {
                    const Complex byTable = ground.couplingRemainder(j, rho), byIntegral = ground.couplingRemainderByIntegral(j, rho);
                    const double size = 2 * M_PI * f * kMu0 / 2 * h * h / (2 * M_PI * std::max(rho, h));
                    QVERIFY2(std::abs(byTable - byIntegral) < 1e-6 * size,
                             qPrintable(QStringLiteral("%1 GHz, rho %2 µm: the coupling's table off by %3")
                                            .arg(f / 1e9).arg(rho * 1e6).arg(std::abs(byTable - byIntegral) / size)));
                }
                for (int height = 0; height < 2; ++height) {
                    const Complex byTable = ground.potentialRemainder(height, rho);
                    const Complex byIntegral = ground.potentialRemainderByIntegral(height, rho);
                    const double size = 2 * M_PI * f * kMu0 / 2 * h * h / (2 * M_PI * std::max(rho, h));
                    QVERIFY2(std::abs(byTable - byIntegral) < 1e-6 * size,
                             qPrintable(QStringLiteral("%1 GHz, rho %2 µm: the potential's table off by %3")
                                            .arg(f / 1e9).arg(rho * 1e6).arg(std::abs(byTable - byIntegral) / size)));
                }
            }
        }
    }

    // A via whose ends are not metal heights - half way up a slab - is
    // said, not answered.
    void aViaThatIsNotWholeSlabsIsSaid()
    {
        const rf::Layers l = stackOf({{9.8, 100e-6, 0}, {4, 50e-6, 0}}, true);
        const rf::ViaGreen half(l, {0, 50e-6}, {}, {}, 1e9, 1e-3);
        QVERIFY(!half.ok());
        QVERIFY2(half.problems().join(' ').contains(QStringLiteral("whole slabs")), qPrintable(half.problems().join(' ')));
        QVERIFY(!rf::ViaGreen(l, {0, 100e-6}, {{20e-6, 100e-6}}, {}, 1e9, 1e-3).ok());
        QVERIFY(!rf::ViaGreen(l, {0, 400e-6}, {}, {}, 1e9, 1e-3).ok());   // (beyond the stack)
        QVERIFY(!rf::ViaGreen(l, {0, 100e-6}, {}, {400e-6}, 1e9, 1e-3).ok());
        QVERIFY(!rf::ViaGreen(l, {0, 100e-6}, {}, {}, 0, 1e-3).ok());
        QVERIFY(rf::ViaGreen(l, {0, 150e-6}, {{100e-6, 150e-6}}, {100e-6, 150e-6}, 1e9, 1e-3).ok());
    }
};

QTEST_MAIN(TestRf25Wave)
#include "test_rf25_wave.moc"
