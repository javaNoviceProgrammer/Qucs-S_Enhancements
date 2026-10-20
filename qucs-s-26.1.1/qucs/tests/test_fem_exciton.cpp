/*
 * test_fem_exciton.cpp - the excitons of subband pairs coupled, in momentum
 *                        space (fem_exciton): the Coulomb kernel's angular
 *                        channels; a hole of two components mixed along k
 *                        against the same exciton in real space; any phase
 *                        of the holes the same; the ideal 2D exciton's exact
 *                        states, strength and Sommerfeld factor; the free
 *                        continuum without the attraction; the form factors
 *                        of envelopes; two pairs coupled - and a quantum
 *                        wells spectrum's "excitonmodel": "coupled": its
 *                        free limit, one pair against the same exciton in
 *                        real space, the subbands against Khramtsov et al.'s
 *                        exact binding, the k·p bands' (no TM on the heavy
 *                        hole's line; without their mixing the parabolic
 *                        bands'), mixed continua spread, a continuum
 *                        widened said
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_exciton.h"
#include "fem_quantum_testing.h"

#include <QtTest>

#include <cmath>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19, Hbar = 1.054571817e-34, M0 = 9.1093837015e-31, Eps0 = 8.8541878128e-12;

// A parabolic pair: its edge E0 (J), reduced mass mr (kg), its amplitude 1.
exciton::Pair parabolic(double E0, double mr, int electron = 0, int hole = 0, double amplitude = 1)
{
    exciton::Pair p;
    p.electron = electron;
    p.hole = hole;
    p.energy = [E0, mr](double k) { return E0 + Hbar * Hbar * k * k / (2 * mr); };
    p.amplitude = [amplitude](double, std::vector<std::complex<double>>& a) { a[0] = amplitude; };
    return p;
}

// The 3D Rydberg and Bohr radius of a reduced mass in a permittivity.
double rydberg(double mr, double epsr)
{
    return mr * std::pow(Q * Q / (4 * M_PI * Eps0 * epsr), 2) / (2 * Hbar * Hbar);
}

double bohr(double mr, double epsr)
{
    return 4 * M_PI * Eps0 * epsr * Hbar * Hbar / (mr * Q * Q);
}

// A hole of two components mixed along k: J_z 3/2 and -1/2, coupled by g
// k₋² as Luttinger's R couples them - the pair's kinetic matrix at θ 0
// [[a1 k², g k²], [g k², D + a2 k²]] (eV, nm; the electron's 0.067, the
// components' 0.11 and 0.21 in the plane). Its two bands (the lower's
// spinor (cos φ, -sin φ), the upper's (sin φ, cos φ), tan 2φ = 2gk²/(D +
// (a2 - a1) k²)), TE by the first component. Strictly 2D: the envelopes
// at one point. \a phased: each band's spinor and amplitude by a phase of
// their own at every k, the basis' second state by another (its
// coefficients by its conjugate) - the same exciton.
exciton::Envelopes mixedHoles(bool phased)
{
    exciton::Envelopes env;
    env.z = {0};
    env.electrons = {{1.0}};
    const std::complex<double> turn = phased ? std::polar(1.0, 0.7) : std::complex<double>(1);
    env.holes = {{{1.0}, {0.0}}, {{0.0}, {turn}}};
    env.angular = {1.5, -0.5};
    return env;
}

std::vector<exciton::Pair> mixedPairs(double g, double D, bool phased)
{
    const double hb2m = 0.0380998212, a1 = hb2m / 0.067 + hb2m / 0.11, a2 = hb2m / 0.067 + hb2m / 0.21;
    std::vector<exciton::Pair> pairs;
    for (int band = 0; band < 2; ++band) {
        exciton::Pair p;
        auto angle = [g, D, a1, a2](double k) { return 0.5 * std::atan2(2 * g * k * k * 1e-18, D + (a2 - a1) * k * k * 1e-18); };
        auto phase = [phased, band](double k) {
            return phased ? std::polar(1.0, 2 * M_PI * std::fmod(std::abs(std::sin(k * 1e-7 * (band + 1.3))) * 43758.5453, 1.0))
                          : std::complex<double>(1);
        };
        p.energy = [g, D, a1, a2, band](double k) {
            const double s = k * k * 1e-18, A = a1 * s, C = D + a2 * s, B = g * s;
            const double half = std::sqrt(0.25 * (C - A) * (C - A) + B * B);
            return (0.5 * (A + C) + (band == 0 ? -half : half)) * Q;
        };
        const std::complex<double> unturn = phased ? std::polar(1.0, -0.7) : std::complex<double>(1);
        p.holeAt = [angle, phase, band, unturn](double k, std::vector<std::complex<double>>& c) {
            const double f = angle(k);
            c[0] = phase(k) * (band == 0 ? std::cos(f) : std::sin(f));
            c[1] = phase(k) * (band == 0 ? -std::sin(f) : std::cos(f)) * unturn;
        };
        p.amplitude = [angle, phase, band](double k, std::vector<std::complex<double>>& a) {
            a[0] = phase(k) * (band == 0 ? std::cos(angle(k)) : std::sin(angle(k)));
        };
        pairs.push_back(std::move(p));
    }
    return pairs;
}

// (Its continuum coarse and short - 4 meV apart, to 0.15 eV: its 1s as
// near as with 1 meV to 0.3 eV, its solves 50 times as fast.)
exciton::Options mixedOptions()
{
    exciton::Options o;
    o.epsr = 12.9;
    o.channel = 1.5;
    o.reach = 0.15 * Q;
    o.spacing = 4e-3 * Q;
    o.maxSize = 4000;
    return o;
}

// ∫ dθ/(2π) cos(Lθ)/|k - k'|, by 20 Gauss-Legendre points on each of [0,
// w], [w, 2w], [2w, 4w] ... to π - w = |k - k'|/√(kk') its peak's width at
// 0, the nearest singularity a panel's width off: geometric convergence.
double channelByQuadrature(int L, double k, double kp)
{
    static std::vector<double> x, wt;
    if (x.empty()) {
        const int n = 20;
        for (int i = 0; i < n; ++i) {
            double z = std::cos(M_PI * (i + 0.75) / (n + 0.5)), dp = 1;
            for (int it = 0; it < 100; ++it) {
                double p0 = 1, p1 = 0;
                for (int j = 0; j < n; ++j) {
                    const double p2 = p1;
                    p1 = p0;
                    p0 = ((2 * j + 1) * z * p1 - j * p2) / (j + 1);
                }
                dp = n * (z * p0 - p1) / (z * z - 1);
                const double dz = p0 / dp;
                z -= dz;
                if (std::abs(dz) < 1e-16) break;
            }
            x.push_back(z);
            wt.push_back(2 / ((1 - z * z) * dp * dp));
        }
    }
    auto f = [L, k, kp](double t) { return std::cos(L * t) / (M_PI * std::sqrt(k * k + kp * kp - 2 * k * kp * std::cos(t))); };
    const double width = std::abs(k - kp) / std::sqrt(k * kp);
    double sum = 0;
    for (double a = 0, b = std::min(M_PI, width); a < M_PI; a = b, b = std::min(M_PI, 2 * b))
        for (std::size_t i = 0; i < x.size(); ++i) sum += 0.5 * (b - a) * wt[i] * f(0.5 * (a + b) + 0.5 * (b - a) * x[i]);
    return sum;
}

} // namespace

class TestFemExciton : public QObject
{
    Q_OBJECT

private slots:
    void theEllipticIntegral()
    {
        QVERIFY(std::abs(exciton::ellipticK(0) - M_PI / 2) < 1e-14);
        QVERIFY(std::abs(exciton::ellipticK(0.5) - 1.8540746773013719) < 1e-12);
        QVERIFY(std::abs(exciton::ellipticK(0.99) - 3.6956373629898747) < 1e-11);
    }

    // The 2D Coulomb kernel's channels L 0-3 (Legendre's Q_{L-1/2} of K and
    // E near k' = k, of its series far off) against the angle's integral
    // done numerically; the s channel's (2/π) K(m)/(k + k'); at k' → k each
    // channel's excess over the s channel's (ψ(1/2) - ψ(L + 1/2))/(πk).
    void theCoulombChannels()
    {
        double worst = 0;
        for (double kp : {0.05, 0.3, 0.9, 0.999, 1.001, 1.5, 2.5, 40.0})
            for (int L = 0; L <= 3; ++L) {
                const double a = exciton::coulombChannel(L, 1.0, kp), b = channelByQuadrature(L, 1.0, kp);
                worst = std::max(worst, std::abs(a / b - 1));
                QVERIFY2(std::abs(a / b - 1) < 1e-9, qPrintable(QString("L %1, k' %2: %3 against %4").arg(L).arg(kp).arg(a, 0, 'g', 15).arg(b, 0, 'g', 15)));
            }
        qInfo("the channels within %.1e of the angle's integral", worst);
        // (Far off the quadrature cancels - a channel's part 1e-8 of its
        // integrand: there 1/|k - k'|'s series in k/k', Legendre's P_n,
        // their cos 2θ parts 3/4 and 5/16 - P_2's and P_4's.)
        const double x = 1e-4;
        QVERIFY(std::abs(exciton::coulombChannel(2, 1.0, 1e4) / (x * (0.375 * x * x + 0.15625 * x * x * x * x)) - 1) < 1e-12);
        for (double kp : {0.2, 0.7, 3.0}) {
            const double m = 4 * kp / ((1 + kp) * (1 + kp));
            QVERIFY(std::abs(exciton::coulombChannel(0, 1.0, kp) / (2 / M_PI * exciton::ellipticK(m) / (1 + kp)) - 1) < 1e-13);
        }
        for (int L = 1; L <= 3; ++L) {
            double psi = 0;
            for (int n = 0; n < L; ++n) psi -= 2.0 / (2 * n + 1);
            const double excess = exciton::coulombChannel(L, 2.0, 2.0 * (1 + 1e-7)) - exciton::coulombChannel(0, 2.0, 2.0 * (1 + 1e-7));
            QVERIFY2(std::abs(excess / (psi / (M_PI * 2.0)) - 1) < 1e-5, qPrintable(QString::number(excess / (psi / (M_PI * 2.0)))));
        }
    }

    // A hole of two components mixed along k (mixedPairs: J_z 3/2 and -1/2,
    // g k₋² between them) - its lower band's 1s in the channel J 3/2, its
    // first component s, its second d - against the same exciton in real
    // space: its radial equations, the derivative couplings in them (the
    // energy functional's finite volumes, to 0.01 nm, Richardson; with g 0
    // the ideal 2D's to 1e-6): D 15 meV, with g 0.2 eV nm² bound by
    // 14.10853 meV, |ψ1(0)|² 1.042958e16 /m²; with 0.4, 16.03751 meV and
    // 1.464738e16 (unmixed 13.61744, 9.4743e15). The second component taken
    // in the s channel too (its J_z not given) instead: far off.
    void aMixedHoleAgainstRealSpace()
    {
        struct Case {
            double g, Eb, psi0;
        };
        for (const Case& c : {Case{0.2, 14.10853e-3, 1.042958e16}, Case{0.4, 16.03751e-3, 1.464738e16}}) {
            const exciton::Result r = exciton::solve(mixedHoles(false), mixedPairs(c.g, 0.015, false), mixedOptions());
            QVERIFY2(!r.energy.empty(), qPrintable(r.note));
            QVERIFY(!r.complex);
            const double Eb = -r.energy[0] / Q, psi0 = r.strength[0][0];
            qInfo("g %.1f eV nm²: bound by %.5f meV (%.5f), |ψ1(0)|² %.6g /m² (%.6g)", c.g, Eb * 1e3, c.Eb * 1e3, psi0, c.psi0);
            QVERIFY2(std::abs(Eb / c.Eb - 1) < 1e-3, qPrintable(QString::number(Eb / c.Eb)));
            QVERIFY2(std::abs(psi0 / c.psi0 - 1) < 0.01, qPrintable(QString::number(psi0 / c.psi0)));
            exciton::Envelopes s = mixedHoles(false);
            s.angular.clear();
            const exciton::Result one = exciton::solve(s, mixedPairs(c.g, 0.015, false), mixedOptions());
            QVERIFY2(std::abs(-one.energy[0] / Q / c.Eb - 1) > 0.02, qPrintable(QString::number(-one.energy[0] / Q / c.Eb)));
        }
    }

    // The holes' phases are anyone's: each band's spinor and amplitude by a
    // phase of their own at every k, a basis state by another - the
    // Hamiltonian complex then - the same states and strengths (the hole
    // densities' real parts alone, as before, were not: the update check of
    // ac8ce07, 2).
    void anyPhaseTheSame()
    {
        const exciton::Result plain = exciton::solve(mixedHoles(false), mixedPairs(0.4, 0.015, false), mixedOptions());
        const exciton::Result phased = exciton::solve(mixedHoles(true), mixedPairs(0.4, 0.015, true), mixedOptions());
        QVERIFY(!plain.complex && phased.complex);
        QCOMPARE(phased.energy.size(), plain.energy.size());
        double worstE = 0, worstS = 0, most = 0;
        for (std::size_t n = 0; n < plain.energy.size(); ++n) most = std::max(most, plain.strength[0][n]);
        for (std::size_t n = 0; n < 40; ++n) {
            worstE = std::max(worstE, std::abs(phased.energy[n] - plain.energy[n]) / Q);
            worstS = std::max(worstS, std::abs(phased.strength[0][n] - plain.strength[0][n]) / most);
        }
        qInfo("phased: the lowest 40 states within %.1e eV, their strengths within %.1e of the most", worstE, worstS);
        // (The complex solve's rounding against the real's: its tail's
        // energies some 500 eV.)
        QVERIFY2(worstE < 1e-9 && worstS < 1e-9, qPrintable(QString("%1 eV, %2").arg(worstE).arg(worstS)));
    }

    // The ideal 2D exciton (a form factor of 1: no thickness): its 1s bound
    // by 4 Ry, its 2s by 4/9 Ry, the 1s's |ψ(0)|² 8/(π a0²); above the edge
    // its continuum raised by 2/(1 + e^(-2π √(Ry/ε))) - the free density
    // mr/(2πħ²) without the attraction.
    void theIdeal2DExciton()
    {
        const double mr = 0.0567 * M0, epsr = 12.9, ry = rydberg(mr, epsr), a0 = bohr(mr, epsr);
        exciton::Options o;
        o.epsr = epsr;
        o.reach = 0.3 * Q;
        o.spacing = 0.5e-3 * Q;
        o.formFactor = [](int, int, double) { return 1.0; };
        const exciton::Result r = exciton::solve({}, {parabolic(0, mr)}, o);
        QVERIFY2(r.note.isEmpty(), qPrintable(r.note));
        QVERIFY(r.energy.size() > 100);
        const double e1 = -r.energy[0] / ry, e2 = -r.energy[1] / ry;
        QVERIFY2(std::abs(e1 / 4 - 1) < 5e-3, qPrintable(QString("1s %1 Ry").arg(e1)));
        QVERIFY2(std::abs(e2 / (4.0 / 9) - 1) < 0.02, qPrintable(QString("2s %1 Ry").arg(e2)));
        const double f1 = r.strength[0][0] / (8 / (M_PI * a0 * a0));
        QVERIFY2(std::abs(f1 - 1) < 0.02, qPrintable(QString("1s strength %1 of 8/(π a0²)").arg(f1)));
        // The continuum's strength between Ry and 4 Ry above the edge,
        // against the Sommerfeld factor's integral there.
        double sum = 0;
        for (std::size_t n = 0; n < r.energy.size(); ++n)
            if (r.energy[n] > ry && r.energy[n] <= 4 * ry) sum += r.strength[0][n];
        double expected = 0;
        for (int j = 0; j < 3000; ++j) {
            const double e = ry * (1 + 3 * (j + 0.5) / 3000);
            expected += 2 / (1 + std::exp(-2 * M_PI * std::sqrt(ry / e))) * mr / (2 * M_PI * Hbar * Hbar) * 3 * ry / 3000;
        }
        QVERIFY2(std::abs(sum / expected - 1) < 0.05, qPrintable(QString("continuum %1 of Sommerfeld's").arg(sum / expected)));
        // Past the grid, the free continuum's density.
        QVERIFY(std::abs(r.beyond[0][0] / (mr / (2 * M_PI * Hbar * Hbar)) - 1) < 1e-3);
        QVERIFY(r.top[0] > 0.29 * Q);
    }

    // Without the attraction (a permittivity of a million): no state below
    // the edge, and the continuum's strength the free density's.
    void withoutTheAttraction()
    {
        const double mr = 0.0567 * M0;
        exciton::Options o;
        o.epsr = 1e6;
        o.reach = 0.2 * Q;
        o.spacing = 1e-3 * Q;
        o.formFactor = [](int, int, double) { return 1.0; };
        const exciton::Result r = exciton::solve({}, {parabolic(1.5 * Q, mr)}, o);
        QVERIFY(r.energy.front() > 1.5 * Q - 1e-6 * Q);
        double sum = 0;
        for (std::size_t n = 0; n < r.energy.size(); ++n)
            if (r.energy[n] < 1.6 * Q) sum += r.strength[0][n];
        QVERIFY2(std::abs(sum / (mr / (2 * M_PI * Hbar * Hbar) * 0.1 * Q) - 1) < 0.01, qPrintable(QString::number(sum)));
    }

    // The form factors of envelopes: an electron and a hole in one thin
    // layer (Gaussians of 0.02 nm) bound nearly as the ideal 2D's - 4 Ry,
    // less 32 <|ze - zh|>/a0 Ry to the first order (a contact term: 0.06
    // Ry); 10 nm apart (an indirect exciton across a barrier) far less, its
    // line far weaker.
    void envelopesFormFactors()
    {
        const double mr = 0.0567 * M0, epsr = 12.9, ry = rydberg(mr, epsr);
        auto bound = [&](double apart, double width, double* strength) {
            exciton::Envelopes env;
            const double half = apart / 2 + 20 * width + 1e-9;
            for (int p = 0; p <= 4000; ++p) env.z.push_back(-half + 2 * half * p / 4000);
            auto layer = [&env, width](double centre) {
                std::vector<double> x;
                double norm = 0;
                for (double z : env.z) {
                    x.push_back(std::exp(-0.5 * std::pow((z - centre) / width, 2)));
                    norm += x.back() * x.back();
                }
                for (double& v : x) v /= std::sqrt(norm);
                return x;
            };
            env.electrons = {layer(-apart / 2)};
            const std::vector<double> h = layer(apart / 2);
            env.holes = {{std::vector<std::complex<double>>(h.begin(), h.end())}};
            exciton::Options o;
            o.epsr = epsr;
            o.reach = 0.2 * Q;
            o.spacing = 1e-3 * Q;
            const exciton::Result r = exciton::solve(env, {parabolic(0, mr)}, o);
            if (strength) *strength = r.strength[0][0];
            return -r.energy[0] / ry;
        };
        double s0 = 0, s10 = 0;
        const double thin = bound(0, 0.02e-9, &s0), apart = bound(10e-9, 0.5e-9, &s10);
        QVERIFY2(thin > 3.9 && thin < 4.01, qPrintable(QString("thin %1 Ry").arg(thin)));
        QVERIFY2(apart < 0.5 * thin && apart > 0.1, qPrintable(QString("10 nm apart %1 Ry").arg(apart)));
        QVERIFY2(s10 < 0.3 * s0, qPrintable(QString("%1 against %2").arg(s10).arg(s0)));
    }

    // Two pairs: apart (no form factor between them), each as alone; coupled
    // (a form factor between them), the lower's state pushed down and its
    // strength shared - the coupling's, both amplitudes' interference.
    void twoPairsCoupled()
    {
        const double mr = 0.0567 * M0, epsr = 12.9, ry = rydberg(mr, epsr);
        exciton::Options o;
        o.epsr = epsr;
        o.reach = 0.15 * Q;
        o.spacing = 1e-3 * Q;
        o.maxSize = 4000;
        const std::vector<exciton::Pair> two = {parabolic(0, mr, 0, 0), parabolic(3 * ry, mr, 1, 1)};
        o.formFactor = [](int a, int b, double) { return a == b ? 1.0 : 0.0; };
        const exciton::Result apart = exciton::solve({}, two, o);
        o.formFactor = [](int a, int b, double q) { return a == b ? 1.0 : 0.3 * (1 - std::exp(-q * 5e-9)); };
        const exciton::Result coupled = exciton::solve({}, two, o);
        o.formFactor = [](int, int, double) { return 1.0; };
        const exciton::Result alone = exciton::solve({}, {two[0]}, o);
        QVERIFY2(std::abs(apart.energy[0] / alone.energy[0] - 1) < 1e-6, qPrintable(QString::number(apart.energy[0] / alone.energy[0])));
        QVERIFY2(coupled.energy[0] < apart.energy[0] - 1e-4 * ry, qPrintable(QString::number((coupled.energy[0] - apart.energy[0]) / ry)));
        // (The off-diagonal form factor's attraction mixes the second pair
        // into the first's 1s: its strength, of both amplitudes, changed.)
        QVERIFY(std::abs(coupled.strength[0][0] / apart.strength[0][0] - 1) > 1e-4);
    }

    // A spectrum's coupled excitons without the attraction (εr a million):
    // its absorption the one without excitons - parabolic bands (2 states,
    // their pairs' continua interleaved, a Lorentzian 1 meV wide: each state
    // spread over its pair's share of the plane at its k point, past the
    // grid's top the free continuum's tail) within 0.5 % (as lines, 94 %
    // off); the k·p bands', its pairs' energies and weights averaged over the
    // plane's directions (the axial approximation), within 1 %.
    void theFreeLimit()
    {
        for (const char* valence : {"", R"(, "valence": "kp4")"}) {
            const bool kp = QLatin1String(valence).size() > 0;
            if (kp && femtest::slowBuild()) continue;   // (its pairs' dense solve, minutes under the sanitizers)
            const int states = kp || femtest::slowBuild() ? 1 : 2;
            auto spectrum = [valence, states, kp](const QString& more) {
                return femtest::run(femtest::modelOf(femtest::lighter(femtest::box(
                    QStringLiteral("20"), QStringLiteral("10"), QString::fromLatin1(valence), QStringLiteral("0[eV]"),
                    QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.57[eV], 1.75[eV], 19)", "states": %2,
                                          "linewidth": "%3", "lineshape": "%4" %1})x")
                                        .arg(more)
                                        .arg(states)
                                        .arg(kp || femtest::slowBuild() ? "5[meV]" : "1[meV]")
                                        .arg(kp || femtest::slowBuild() ? "gaussian" : "lorentzian")))));
            };
            const femtest::Run off = spectrum(QString());
            const femtest::Run free = spectrum(R"(, "excitons": true, "excitonmodel": "coupled", "epsr": "1e6")");
            QVERIFY2(off.error.isEmpty() && free.error.isEmpty(), qPrintable(off.error + free.error));
            double most = 0, worst = 0;
            for (int k = 0; k < off.solution->snapshotCount(); ++k) {
                off.solution->select(k);
                most = std::max(most, femtest::global(*off.solution, "qw.alphaTE"));
            }
            for (int k = 0; k < off.solution->snapshotCount(); ++k) {
                off.solution->select(k);
                free.solution->select(k);
                for (const char* g : {"qw.alphaTE", "qw.alphaTM"}) {
                    const double a = femtest::global(*off.solution, g), b = femtest::global(*free.solution, g);
                    if (a > 0.01 * most) worst = std::max(worst, std::abs(b / a - 1));
                }
            }
            qInfo("%s: without the attraction within %.2f %% of the spectrum without excitons", kp ? "k·p" : "parabolic", worst * 100);
            QVERIFY2(worst < (kp ? 0.01 : 0.005), qPrintable(QString::number(worst)));
        }
    }

    // One pair - an infinite well 10 nm wide, its e1-hh1 (me 0.067, the heavy
    // hole's in-plane mass 0.11, εr 12.9) - against the same exciton found
    // another way: its radial equation in real space by finite volumes
    // (0.01 nm, to 200 nm; 0.02 nm the same to 1e-5), the potential of the
    // sin² envelopes' separation, its lowest state by Sturm bisection:
    // bound by 8.4451 meV, |ψ(0)|² 3.28688e15 /m². (Bastard's variational:
    // 8.316 meV, 4.01e15 - its line 22 % too strong.)
    void onePairAgainstRealSpace()
    {
        const femtest::Run x = femtest::run(femtest::modelOf(femtest::box(
            QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
            QString::fromUtf8(R"x({"type": "spectrum", "energies": "1.6[eV]", "states": 1, "linewidth": "%1", "lineshape": "gaussian",
                                  "excitons": true, "excitonmodel": "coupled", "epsr": "12.9"})x")
                .arg(femtest::slowBuild() ? "6[meV]" : "1[meV]"))));
        QVERIFY2(x.error.isEmpty(), qPrintable(x.error));
        x.solution->select(0);
        const double Eb = femtest::global(*x.solution, "qw.Eb"), aX = femtest::global(*x.solution, "qw.aX");
        const double psi0 = 2 / (M_PI * aX * aX);
        qInfo("one pair: bound by %.4f meV (8.4451), |ψ(0)|² %.5g /m² (3.28688e15)", Eb / Q * 1e3, psi0);
        QVERIFY2(std::abs(Eb / (8.4451e-3 * Q) - 1) < 1e-3, qPrintable(QString::number(Eb / Q * 1e3)));
        QVERIFY2(std::abs(psi0 / 3.28688e15 - 1) < 3e-3, qPrintable(QString::number(psi0)));
        QVERIFY(std::isnan(femtest::global(*x.solution, "qw.gX")));
    }

    // The subbands coupled converge on the exact exciton: Khramtsov et al.,
    // "Excitons in square quantum wells: microscopic modeling and
    // experiment" (arXiv:1508.00480) - GaAs/Al0.3Ga0.7As (their Table I),
    // its 3D Schrödinger equation solved, the heavy-hole exciton bound by
    // Rx(L) = (3.5431 L² + 146.694 L)/(L² + 11.1758 L + 6.0473) meV. Here,
    // with 4 states (12 pairs coupled), within 1 % at 10 and 20 nm (one pair
    // 1.6 and 3.7 % below: a wide well's exciton draws on its higher
    // subbands); 2 states, under the sanitizers (20 nm alone), within
    // 2.5 %. And its 1s
    // line stronger the more subbands - the electron and hole correlated
    // along the axis: |ψ(0)|² 1.14 times one pair's at 10 nm with 4 states;
    // 23.0 µeV of radiative decay at 14 nm with one pair, 29.7 with 4
    // states, 30.2 with 6 (measured 36.6).
    void subbandsAgainstKhramtsov()
    {
        auto stack = [](double L, const QString& energies, int states) {
            return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "25"]},
                             {"type": "rectangle", "name": "well", "position": ["0", "25"], "size": ["10", "%1"]},
                             {"type": "rectangle", "name": "above", "position": ["0", "%2"], "size": ["10", "25"]}],
                "physics": [{"type": "quantum", "tag": "qw", "light": false, "features": [
                  {"type": "material", "source": "user", "Ec": "1.520[eV]", "Ev": "0[eV]", "me": "0.067", "mhh": "0.377", "mlh": "0.09",
                   "mhhp": "0.111", "mlhp": "0.21", "Ep": "28.8[eV]", "nr": "3.6", "epsilonr": "12.53", "selection": {"only": ["well"]}},
                  {"type": "material", "source": "user", "Ec": "1.757575[eV]", "Ev": "-0.127925[eV]", "me": "0.067", "mhh": "0.377", "mlh": "0.09",
                   "mhhp": "0.111", "mlhp": "0.21", "Ep": "28.8[eV]", "nr": "3.6", "epsilonr": "12.53", "selection": {"only": ["below", "above"]}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "%3"}}],
              "studies": [{"type": "study", "steps": [{"type": "spectrum", "energies": "%4", "states": %5, "linewidth": "%6",
                                                      "lineshape": "gaussian", "excitons": true, "excitonmodel": "coupled"}]}]})json")
                .arg(L)
                .arg(25 + L)
                .arg(std::min(0.5, L / 10))
                .arg(energies)
                .arg(states)
                .arg(femtest::slowBuild() ? "6[meV]" : "0.2[meV]")   // (its continuum 3 meV apart: the bound states the same)
                .toUtf8();
        };
        auto exact = [](double L) { return (3.5431 * L * L + 146.694 * L) / (L * L + 11.1758 * L + 6.0473) * 1e-3 * Q; };
        const int more = femtest::slowBuild() ? 2 : 4;
        // (Under the sanitizers the wide well alone - its coupling the most:
        // each well some 150 s there, two dense solves of 700.)
        const std::vector<double> wells = femtest::slowBuild() ? std::vector<double>{20.0} : std::vector<double>{10.0, 20.0};
        for (double L : wells) {
            double Eb[2] = {0, 0}, strength[2] = {0, 0};
            for (int m = 0; m < 2; ++m) {
                const femtest::Run x = femtest::run(femtest::modelOf(stack(L, QStringLiteral("1.6[eV]"), m == 0 ? 1 : more)));
                QVERIFY2(x.error.isEmpty(), qPrintable(x.error));
                x.solution->select(0);
                // (Every pair of the states, the dark - forbidden by parity -
                // too: 4 states 16 of heavy holes, 12 coupled.)
                if (m == 1 && more == 4)
                    QVERIFY2(x.solution->log.join('\n').contains(QLatin1String("the 12 lowest of the 16 pairs of heavy holes coupled")),
                             qPrintable(x.solution->log.join('\n')));
                Eb[m] = femtest::global(*x.solution, "qw.Eb");
                const double aX = femtest::global(*x.solution, "qw.aX");
                strength[m] = 2 / (M_PI * aX * aX);
            }
            qInfo("%.0f nm: bound by %.3f meV with one pair, %.3f with %d states (exact %.3f); |ψ(0)|² %.3g, %.3g", L, Eb[0] / Q * 1e3,
                  Eb[1] / Q * 1e3, more, exact(L) / Q * 1e3, strength[0], strength[1]);
            QVERIFY(Eb[1] > Eb[0]);
            QVERIFY2(std::abs(Eb[1] / exact(L) - 1) < (more == 4 ? 0.01 : 0.025), qPrintable(QString::number(Eb[1] / exact(L))));
            QVERIFY2(strength[1] > 1.05 * strength[0], qPrintable(QString::number(strength[1] / strength[0])));
        }
    }

    // The k·p bands' coupled excitons (4x4, a 10 nm infinite well): bound
    // more than the parabolic bands' (the holes heavier in the plane past k
    // 0), within 10 % of the two-parameter variational one on the same
    // bands - the heavy and light holes' mixing in the attraction too (its
    // channels J 3/2 and 1/2); its line where the spectrum peaks, and no TM
    // there: the heavy-hole exciton's light-hole part is p-like about it, its
    // TM gone by the angle (an incoherent part of its own along k gave it
    // TM 0.18 of TE: the update check of ac8ce07, 3). The log's transitions
    // the k·p pairs (it said 0), the coupled among them.
    void kpBandsCoupled()
    {
        if (femtest::slowBuild()) QSKIP("the k·p pairs' dense solve: minutes under the sanitizers");
        auto spectrum = [](const QString& valence, const QString& model) {
            return femtest::run(femtest::modelOf(femtest::box(
                QStringLiteral("20"), QStringLiteral("10"), valence, QStringLiteral("0[eV]"),
                QString::fromUtf8(R"x({"type": "spectrum", "energies": "linspace(1.545[eV], 1.575[eV], 31)", "states": 1, "linewidth": "1[meV]",
                                      "lineshape": "gaussian", "excitons": true, "excitonmodel": "%1", "epsr": "12.9"})x").arg(model))));
        };
        const femtest::Run kp = spectrum(QStringLiteral(R"(, "valence": "kp4")"), QStringLiteral("coupled"));
        const femtest::Run two = spectrum(QStringLiteral(R"(, "valence": "kp4")"), QStringLiteral("two"));
        const femtest::Run para = spectrum(QString(), QStringLiteral("coupled"));
        QVERIFY2(kp.error.isEmpty() && two.error.isEmpty() && para.error.isEmpty(), qPrintable(kp.error + two.error + para.error));
        kp.solution->select(0);
        two.solution->select(0);
        para.solution->select(0);
        const double Eb = femtest::global(*kp.solution, "qw.Eb"), Ex = femtest::global(*kp.solution, "qw.Ex");
        const double Eb2 = femtest::global(*two.solution, "qw.Eb"), EbP = femtest::global(*para.solution, "qw.Eb");
        qInfo("k·p: bound by %.3f meV (two parameters %.3f, parabolic coupled %.3f)", Eb / Q * 1e3, Eb2 / Q * 1e3, EbP / Q * 1e3);
        QVERIFY(Eb > EbP);
        QVERIFY2(std::abs(Eb / Eb2 - 1) < 0.1, qPrintable(QString::number(Eb / Eb2)));
        // (Its own value, the mixing's measure: 8.687 meV; its spinors at k =
        // 0 instead, as in ac8ce07, 8.895.)
        QVERIFY2(std::abs(Eb / (8.687e-3 * Q) - 1) < 3e-3, qPrintable(QString::number(Eb / Q * 1e3, 'f', 4)));
        double best = 0, at = 0;
        for (int k = 0; k < kp.solution->snapshotCount(); ++k) {
            kp.solution->select(k);
            const double a = femtest::global(*kp.solution, "qw.alphaTE");
            if (a > best) {
                best = a;
                at = femtest::global(*kp.solution, "qw.Ephoton");
            }
        }
        QVERIFY2(std::abs(at - Ex) < 1.5e-3 * Q, qPrintable(QString("peak %1, Ex %2").arg(at / Q).arg(Ex / Q)));
        double te = 0, tm = 0;
        for (int k = 0; k < kp.solution->snapshotCount(); ++k) {
            kp.solution->select(k);
            if (std::abs(femtest::global(*kp.solution, "qw.Ephoton") - at) > 1e-6 * Q) continue;
            te = femtest::global(*kp.solution, "qw.alphaTE");
            tm = femtest::global(*kp.solution, "qw.alphaTM");
        }
        qInfo("k·p: at its line TM %.3g of TE", tm / te);
        QVERIFY2(te > 0 && tm < 0.01 * te, qPrintable(QString("TM %1 of TE").arg(tm / te)));
        const QString log = kp.solution->log.join(QLatin1Char('\n'));
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("a spectrum of (\\d+) transitions \\((\\d+) of them the coupled excitons'\\)")).match(log);
        QVERIFY2(m.hasMatch() && m.captured(2).toInt() > 0 && m.captured(1).toInt() >= m.captured(2).toInt(), qPrintable(log));
    }

    // The k·p bands without their mixing - γ2 = γ3 = 0: the heavy and light
    // holes of one mass, 0.2, along the axis and in the plane, one energy -
    // the parabolic bands of that mass: their coupled excitons the same, the
    // k·p's in their channels of every k's four states lined up along k (J
    // 3/2: the heavy hole's in s; 1/2: the light's), the parabolic's of the
    // two kinds of hole apart.
    void kpWithoutMixingAsParabolic()
    {
        if (femtest::slowBuild()) QSKIP("the k·p pairs' dense solves: minutes under the sanitizers");
        auto spectrum = [](bool kp) {
            QByteArray json = femtest::box(QStringLiteral("20"), QStringLiteral("10"), kp ? QStringLiteral(R"(, "valence": "kp4")") : QString(),
                                           QStringLiteral("0[eV]"),
                                           QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.555[eV], 1.60[eV], 46)", "states": 1,
                                                              "linewidth": "1[meV]", "lineshape": "gaussian", "excitons": true,
                                                              "excitonmodel": "coupled", "epsr": "12.9"})x"));
            json.replace(R"("mhh": "0.35", "mlh": "0.09")", R"("mhh": "0.2", "mlh": "0.2", "gamma3": "0")");
            json.replace(R"("mhhp": "0.11", "mlhp": "0.21")", R"("mhhp": "0.2", "mlhp": "0.2")");
            return femtest::run(femtest::modelOf(json));
        };
        const femtest::Run kp = spectrum(true), para = spectrum(false);
        QVERIFY2(kp.error.isEmpty() && para.error.isEmpty(), qPrintable(kp.error + para.error));
        kp.solution->select(0);
        para.solution->select(0);
        const double Eb = femtest::global(*kp.solution, "qw.Eb"), EbP = femtest::global(*para.solution, "qw.Eb");
        double most = 0, worst = 0;
        for (int k = 0; k < para.solution->snapshotCount(); ++k) {
            para.solution->select(k);
            most = std::max(most, femtest::global(*para.solution, "qw.alphaTE"));
        }
        for (int k = 0; k < para.solution->snapshotCount(); ++k) {
            kp.solution->select(k);
            para.solution->select(k);
            for (const char* g : {"qw.alphaTE", "qw.alphaTM"})
                worst = std::max(worst, std::abs(femtest::global(*kp.solution, g) - femtest::global(*para.solution, g)) / most);
        }
        qInfo("k·p without mixing: bound by %.4f meV, parabolic %.4f; its spectrum within %.2g of the most", Eb / Q * 1e3, EbP / Q * 1e3, worst);
        QVERIFY2(std::abs(Eb / EbP - 1) < 1e-3, qPrintable(QString::number(Eb / EbP)));
        QVERIFY2(worst < 0.01, qPrintable(QString::number(worst)));
    }

    // Where pairs' continua mix (a 10 nm GaAs/Al0.3Ga0.7As well's, 4
    // states, a 1 meV Lorentzian; e1-hh1's and e1-lh1's continua and e1-hh2's,
    // dark, from 1.48 eV) each continuum state spread over its largest
    // parts' cells, weighted by their shares: its absorption within 3 % (TE)
    // and 2 % (TM) of its own mean over 2 meV - each over its largest part's
    // cell alone, 3.4 % and 2.4 % (the update check of ac8ce07, 4: ±16 % at
    // 0.5 meV, then).
    void aMixedContinuumSpread()
    {
        if (femtest::slowBuild()) QSKIP("its dense solves, minutes under the sanitizers");
        const femtest::Run x = femtest::run(femtest::modelOf(femtest::heterostructure(
            QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("10"), QStringLiteral("25"),
            QString(),
            QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.48[eV], 1.50[eV], 81)", "states": 4, "linewidth": "1[meV]",
                               "lineshape": "lorentzian", "excitons": true, "excitonmodel": "coupled", "epsr": "12.9"})x"))));
        QVERIFY2(x.error.isEmpty(), qPrintable(x.error));
        double worst[2] = {0, 0};
        const char* names[2] = {"qw.alphaTE", "qw.alphaTM"};
        for (int g = 0; g < 2; ++g) {
            std::vector<double> a;
            for (int k = 0; k < x.solution->snapshotCount(); ++k) {
                x.solution->select(k);
                a.push_back(femtest::global(*x.solution, names[g]));
            }
            for (std::size_t i = 4; i + 4 < a.size(); ++i) {
                double mean = 0;
                for (std::size_t j = i - 4; j <= i + 4; ++j) mean += a[j] / 9;
                worst[g] = std::max(worst[g], std::abs(a[i] / mean - 1));
            }
        }
        qInfo("mixed continua: TE within %.2f %%, TM %.2f %% of their means over 2 meV", worst[0] * 100, worst[1] * 100);
        QVERIFY2(worst[0] < 0.03 && worst[1] < 0.02, qPrintable(QString("%1, %2").arg(worst[0]).arg(worst[1])));
    }

    // A continuum wider than the line asks, to fit the coupled excitons'
    // size: a warning naming what would smooth it (it was a line of the log:
    // the update check of ac8ce07, 4); a size out of its range refused.
    void theContinuumWidenedSaid()
    {
        auto spectrum = [](const char* size) {
            return femtest::run(femtest::modelOf(femtest::box(
                QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                QString::fromUtf8(R"x({"type": "spectrum", "energies": "1.6[eV]", "states": 1, "linewidth": "1[meV]", "lineshape": "gaussian",
                                      "excitons": true, "excitonmodel": "coupled", "epsr": "12.9", "excitonsize": %1})x").arg(QLatin1String(size)))));
        };
        const femtest::Run small = spectrum("100");
        QVERIFY2(small.error.isEmpty(), qPrintable(small.error));
        const QStringList warned = small.solution->log.filter(QLatin1String("Warning:")).filter(QLatin1String("continuum states"));
        QVERIFY2(warned.size() == 1 && warned.front().contains(QLatin1String("not the 0.5 the line's width asks"))
                     && warned.front().contains(QLatin1String("unknowns, \"excitonsize\")")) && warned.front().contains(QLatin1String("a line of 1 meV or more smooths it")),
                 qPrintable(small.solution->log.join(QLatin1Char('\n'))));
        const femtest::Run refused = spectrum("50");
        QVERIFY2(refused.error.contains(QLatin1String("the coupled excitons' size: a whole number from 100 to 20000")), qPrintable(refused.error));
    }
};

QTEST_GUILESS_MAIN(TestFemExciton)
#include "test_fem_exciton.moc"
