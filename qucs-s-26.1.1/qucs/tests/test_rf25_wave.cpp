/*
 * test_rf25_wave.cpp - the retarded Green's functions of a layered stack
 * (rf25/rf25_wave.h), held to what has a closed form: Bessel's J0 of a
 * complex argument; the spectral kernels of a grounded slab (Mosig's); a
 * charge and a current over a ground plane in one medium, which are
 * themselves and their images at any frequency; and the static Green's
 * function of phase B, which they must become as the frequency falls.
 * Then what has none: that they are reciprocal, that the path the
 * integral takes off the real axis makes no difference, and that the
 * table is the integral.
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
};

QTEST_MAIN(TestRf25Wave)
#include "test_rf25_wave.moc"
