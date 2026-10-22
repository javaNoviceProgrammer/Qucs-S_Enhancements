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

#include <cstring>

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
    // hole's in-plane mass 0.11, along the axis 0.35, εr 12.9) - against the
    // same exciton found another way: its radial equation in real space by
    // finite volumes (0.01 nm, to 200 nm; 0.02 nm the same to 1e-5), the
    // potential of the sin² envelopes' separation, its lowest state by Sturm
    // bisection: bound by 8.4451 meV, |ψ(0)|² 3.28688e15 /m². (Bastard's
    // variational: 8.316 meV, 4.01e15 - its line 22 % too strong.) Drawn
    // together along the axis - its second function e^(-|ze - zh|/σ), σ
    // 2.556 nm -, as a real-space solve of the same two functions (coupled
    // radial equations, 0.02 nm to 250 nm: their overlaps, energies along
    // the axis and attraction from the separation's distribution, 0.01 nm
    // bins) finds it: 8.4904 meV, 4.0030e15 - 1.22 times the products'
    // line; with three such functions (1/10, 1/3.3, 1 /nm) 8.5151, 4.1169e15.
    void onePairAgainstRealSpace()
    {
        for (const bool drawn : {false, true}) {
            const femtest::Run x = femtest::run(femtest::modelOf(femtest::box(
                QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                QString::fromUtf8(R"x({"type": "spectrum", "energies": "1.6[eV]", "states": 1, "linewidth": "%1", "lineshape": "gaussian",
                                      "excitons": true, "excitonmodel": "coupled", "excitondrawn": %2, "epsr": "12.9"})x")
                    .arg(femtest::slowBuild() ? "6[meV]" : "1[meV]", drawn ? "true" : "false"))));
            QVERIFY2(x.error.isEmpty(), qPrintable(x.error));
            x.solution->select(0);
            const double Eb = femtest::global(*x.solution, "qw.Eb"), aX = femtest::global(*x.solution, "qw.aX");
            const double psi0 = 2 / (M_PI * aX * aX);
            const double wantEb = drawn ? 8.4904e-3 : 8.4451e-3, wantPsi = drawn ? 4.0030e15 : 3.28688e15;
            qInfo("one pair%s: bound by %.4f meV (%.4f), |ψ(0)|² %.5g /m² (%.5g)", drawn ? ", drawn together" : "", Eb / Q * 1e3, wantEb * 1e3, psi0,
                  wantPsi);
            QVERIFY2(std::abs(Eb / (wantEb * Q) - 1) < 1e-3, qPrintable(QString::number(Eb / Q * 1e3)));
            QVERIFY2(std::abs(psi0 / wantPsi - 1) < 3e-3, qPrintable(QString::number(psi0)));
            QVERIFY(std::isnan(femtest::global(*x.solution, "qw.gX")));
            // (Said: how many drawn together, and β's length.)
            const QString log = x.solution->log.join(QLatin1Char('\n'));
            QVERIFY2(log.contains(QLatin1String("the coupled excitons' pair drawn together along the axis, e^(-|ze - zh|/2.556 nm)")) == drawn, qPrintable(log));
        }
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
    // line, the electron and hole drawn together along the axis
    // (excitondrawn), with one pair within 3 % of 4 states' at 10 nm, 7 % at
    // 20 (a wide well's exciton draws on its higher subbands) - the subbands'
    // products alone drew them together slowly: |ψ(0)|² 1.14 times one
    // pair's at 10 nm with 4 states; 23.0 µeV of radiative decay at 14 nm
    // with one pair, 29.7 with 4 states, 30.2 with 6 (measured 36.6; drawn
    // together: test_fem_quantum's excitonsAgainstKhramtsov).
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
                                                      "lineshape": "gaussian", "excitons": true, "excitonmodel": "coupled",
                                                      "excitonsize": %7}]}]})json")
                .arg(L)
                .arg(25 + L)
                .arg(std::min(0.5, L / 10))
                .arg(energies)
                .arg(states)
                .arg(femtest::slowBuild() ? "6[meV]" : "0.2[meV]")   // (its continuum 3 meV apart: the bound states the same)
                // (Under the sanitizers its k points fewer: the pairs drawn
                // together's functions double its dense solve's size.)
                .arg(femtest::slowBuild() ? 400 : 1200)
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
            QVERIFY2(strength[1] >= strength[0] && strength[1] < (L < 15 ? 1.03 : 1.08) * strength[0], qPrintable(QString::number(strength[1] / strength[0])));
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
        // (Its own value, the mixing's measure: 8.721 meV - its products
        // alone 8.687; its spinors at k = 0 instead, as in ac8ce07, 8.895.)
        QVERIFY2(std::abs(Eb / (8.721e-3 * Q) - 1) < 3e-3, qPrintable(QString::number(Eb / Q * 1e3, 'f', 4)));
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

    // Lumerical's GaAs/AlGaAs electroabsorption modulator's well, its
    // materials as it gives them (9.4 nm GaAs, 20 nm Al0.3Ga0.7As each side,
    // 4x4 bands), in a field F (V/m).
    static QByteArray eamWell(const QString& field, const QString& step)
    {
        return QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
          "parameters": [{"name": "F", "expression": "%1[V/m]"}],
          "components": [{"unit": "nm",
            "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "20"]},
                         {"type": "rectangle", "name": "well", "position": ["0", "20"], "size": ["10", "9.4"]},
                         {"type": "rectangle", "name": "above", "position": ["0", "29.4"], "size": ["10", "20"]}],
            "materials": [{"type": "material", "library": "Aluminium gallium arsenide", "x": "0.3", "Eg": "1.8966495[eV]", "Ep": "24.32[eV]",
                           "VBO": "-0.165427[eV]", "epsilonr": "12.27", "gamma1": "5.83", "gamma2": "1.674", "gamma3": "2.417", "me": "0.0879",
                           "selection": {"only": ["below", "above"]}},
                          {"type": "material", "library": "Gallium arsenide", "Eg": "1.424[eV]", "Ep": "25.7[eV]", "VBO": "0[eV]", "epsilonr": "13.2",
                           "gamma1": "6.85", "gamma2": "2.1", "gamma3": "2.9", "me": "0.063", "selection": {"only": ["well"]}}],
            "physics": [{"type": "quantum", "tag": "qw", "valence": "kp4", "F": "F", "T": "300[K]"}],
            "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
          "studies": [{"type": "study", "steps": [%2]}]})json")
            .arg(field, step)
            .toUtf8();
    }

    // At 99 kV/cm (the modulator's -10.25 V) the barrier's notch holds valence
    // states above the well's hh1 at k = 0, and they cross it along k. The
    // localized selection leaves them out, its subbands followed along k by
    // their overlaps (KpBands::order): its free TE absorption near the
    // e1-hh1 edge, 1 + 2 subbands, within 3 % of every state's to 8 (by
    // energy, the notch's too: theirs next to nothing) - by index, each k's
    // states by energy, past the crossing the subbands were others: 0.15 to
    // 0.31 of it. Its excitons there: the coupled bound within 15 % of the
    // two-parameter one (whose e1-hh1 globals the first kept pair's: they
    // were NaN, the hh1 its third and fourth state).
    void aHighFieldSubbandFollowed()
    {
        if (femtest::slowBuild()) QSKIP("the k·p bands' and pairs' solves: minutes under the sanitizers");
        auto spectrum = [](const QString& how) {
            return femtest::run(femtest::modelOf(eamWell(QStringLiteral("9.92813e6"),
                QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.445[eV], 1.47[eV], 6)", "linewidth": "3[meV]",
                                    "lineshape": "gaussian", "epsr": "10", "index": "3.5038", %1})x").arg(how))));
        };
        const femtest::Run taken = spectrum(QStringLiteral(R"("select": "localized", "pmin": "0.6", "cstates": 1, "vstates": 2, "states": 4)"));
        const femtest::Run every = spectrum(QStringLiteral(R"("select": "energy", "states": 8)"));
        QVERIFY2(taken.error.isEmpty() && every.error.isEmpty(), qPrintable(taken.error + every.error));
        double worst = 0;
        for (int k = 0; k < taken.solution->snapshotCount(); ++k) {
            taken.solution->select(k);
            every.solution->select(k);
            worst = std::max(worst, std::abs(femtest::global(*taken.solution, "qw.alphaTE") / femtest::global(*every.solution, "qw.alphaTE") - 1));
        }
        qInfo("99 kV/cm: the localized subbands' TE within %.2f %% of every state's", worst * 100);
        QVERIFY2(worst < 0.03, qPrintable(QString::number(worst)));
        const QString excitons = QStringLiteral(R"("select": "localized", "pmin": "0.6", "cstates": 1, "vstates": 2, "states": 4, "excitons": true, )");
        const femtest::Run coupled = spectrum(excitons + QStringLiteral(R"("excitonmodel": "coupled")"));
        const femtest::Run two = spectrum(excitons + QStringLiteral(R"("excitonmodel": "two")"));
        QVERIFY2(coupled.error.isEmpty() && two.error.isEmpty(), qPrintable(coupled.error + two.error));
        coupled.solution->select(0);
        two.solution->select(0);
        const double Eb = femtest::global(*coupled.solution, "qw.Eb"), Eb2 = femtest::global(*two.solution, "qw.Eb");
        qInfo("99 kV/cm: bound by %.3f meV (two parameters %.3f)", Eb / Q * 1e3, Eb2 / Q * 1e3);
        QVERIFY2(std::isfinite(Eb2) && std::abs(Eb / Eb2 - 1) < 0.15, qPrintable(QString("%1 %2").arg(Eb).arg(Eb2)));
    }

    // Too few subbands for the coupled excitons: a 10 nm GaAs/Al0.3Ga0.7As
    // well's, its hh2 7 meV below its lh1 - mixing with it along k within the
    // excitons' reach. With 2 valence pairs (hh1, lh1) the 1s line draws its
    // strength from the continuum near the edge - the TE weight the mixing
    // moves to the left-out subbands not there to draw on -: TE 20 to 50 meV
    // above the e1-hh1 edge 0.91 to 0.94 of the free one, said; with 4,
    // 0.97 to 1.08, 1.02 on the mean (its next, within 30 meV of hh3, said
    // too; with 8, 1.06
    // to 1.11; an independent real-space solve,
    // the axial 4x4 on 16 states at k = 0 as the hole's basis, 1.14, 1.13,
    // 1.10 - an attraction's Sommerfeld factor, above 1).
    void tooFewSubbandsSaid()
    {
        if (femtest::slowBuild()) QSKIP("the k·p pairs' dense solves: minutes under the sanitizers");
        auto spectrum = [](int vstates, const char* model) {
            return femtest::run(femtest::modelOf(femtest::heterostructure(
                QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("10"), QStringLiteral("25"),
                QStringLiteral(R"(, "valence": "kp4")"),
                QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.4812[eV], 1.5112[eV], 4)", "states": 8, "cstates": 1, "vstates": %1,
                                    "linewidth": "4[meV]", "lineshape": "gaussian", "excitons": %2, "excitonmodel": "coupled", "excitondrawn": false,
                                    "epsr": "12.9"})x")
                    .arg(vstates)
                    .arg(QLatin1String(std::strcmp(model, "none") == 0 ? "false" : "true")))));
        };
        double ratio[2][4] = {};
        for (int v = 0; v < 2; ++v) {
            const femtest::Run x = spectrum(v == 0 ? 2 : 4, "coupled"), free = spectrum(v == 0 ? 2 : 4, "none");
            QVERIFY2(x.error.isEmpty() && free.error.isEmpty(), qPrintable(x.error + free.error));
            for (int k = 0; k < 4; ++k) {
                x.solution->select(k);
                free.solution->select(k);
                ratio[v][k] = femtest::global(*x.solution, "qw.alphaTE") / femtest::global(*free.solution, "qw.alphaTE");
            }
            const QStringList said = x.solution->log.filter(QLatin1String("Warning:")).filter(QLatin1String("the coupled excitons take"));
            if (v == 0)
                QVERIFY2(said.size() == 1 && said.front().contains(QLatin1String("the spectrum's 2 valence subbands"))
                             && said.front().contains(QLatin1String("\"vstates\" 3 or more")),
                         qPrintable(x.solution->log.join(QLatin1Char('\n'))));
            qInfo("%d valence pairs: TE 20, 30, 40, 50 meV above the edge %.3f %.3f %.3f %.3f of the free", v == 0 ? 2 : 4, ratio[v][0], ratio[v][1],
                  ratio[v][2], ratio[v][3]);
        }
        // (Each below the free with 2; with 4 their mean above it - a
        // point of it rippling some ±5 % with the continuum's states.)
        double mean[2] = {0, 0};
        for (int k = 0; k < 4; ++k) {
            QVERIFY2(ratio[0][k] < 0.97, qPrintable(QString::number(ratio[0][k])));
            for (int v = 0; v < 2; ++v) mean[v] += ratio[v][k] / 4;
        }
        QVERIFY2(mean[0] < 0.95 && mean[1] > 1.0, qPrintable(QString("%1 %2").arg(mean[0]).arg(mean[1])));
    }

    // The coupled excitons' continuum: its ripple at the line's width
    // warned of where some 2 % or more (it was a line of the log: the update
    // check of ac8ce07, 4), not by the spacing alone (152 meV "apart" warned
    // of a spectrum a larger size moved 1 % at most: the update check of 21
    // October, 4); a size out of its range refused.
    void theContinuumWidenedSaid()
    {
        auto spectrum = [](const char* size, const char* width) {
            return femtest::run(femtest::modelOf(femtest::box(
                QStringLiteral("20"), QStringLiteral("10"), QString(), QStringLiteral("0[eV]"),
                QString::fromUtf8(R"x({"type": "spectrum", "energies": "1.6[eV]", "states": 1, "linewidth": "%2[meV]", "lineshape": "gaussian",
                                      "excitons": true, "excitonmodel": "coupled", "epsr": "12.9", "excitonsize": %1})x")
                    .arg(QLatin1String(size), QLatin1String(width)))));
        };
        const femtest::Run small = spectrum("100", "1");
        QVERIFY2(small.error.isEmpty(), qPrintable(small.error));
        const QStringList warned = small.solution->log.filter(QLatin1String("Warning:")).filter(QLatin1String("continuum states"));
        QVERIFY2(warned.size() == 1 && warned.front().contains(QLatin1String("unknowns, \"excitonsize\")"))
                     && warned.front().contains(QStringLiteral("ripples about its trend some ±4 % at the ripple's peaks at this line's 1 meV"))
                     && warned.front().contains(QLatin1String("a wider line smooths it")),
                 qPrintable(small.solution->log.join(QLatin1Char('\n'))));
        // (At 2 meV ±1.5 %: not said.)
        const femtest::Run wide = spectrum("100", "2");
        QVERIFY2(wide.error.isEmpty(), qPrintable(wide.error));
        QVERIFY2(wide.solution->log.filter(QLatin1String("continuum states")).isEmpty(), qPrintable(wide.solution->log.join(QLatin1Char('\n'))));
        const femtest::Run refused = spectrum("50", "1");
        QVERIFY2(refused.error.contains(QLatin1String("the coupled excitons' size: a whole number from 100 to 20000")), qPrintable(refused.error));
    }

    // Miller's 96 Å well at a point of the check of 21 October's sweep -
    // Al0.24 barriers 30 nm, their gap 1.798 eV, Chao and Chuang's Luttinger
    // parameters, 4 conduction and 14 valence subbands coupled: its e1-hh1
    // exciton was bound by 62.6 eV (the sweep's others 8.70-8.76 meV). The
    // hole spinors' basis holds some at its grid's top little (0.5 %): their
    // projections, noise, were not orthogonal (0.75 apart), the excitons'
    // overlaps no longer positive, their generalized solve broken. The
    // spinors on their basis orthonormal now (Gram and Schmidt's, those it
    // holds most of first): bound by 8.7 meV, no k point's functions drawn
    // together left out (without them orthonormal some were, here, on 1200
    // unknowns; on the check's 4000 the 62.6 eV).
    void aSweepPointsExciton()
    {
        if (femtest::slowBuild()) QSKIP("its 14 valence subbands' coupled excitons: minutes under the sanitizers");
        QByteArray json = femtest::heterostructure(
            QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.24"), QStringLiteral("9.6"), QStringLiteral("30"),
            QStringLiteral(R"(, "valence": "kp4", "h1d": "0.25[nm]", "T": "300[K]")"),
            QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.42[eV], 1.78[eV], 37)", "cstates": 4, "vstates": 14, "continuum": "bound",
                               "excitons": true, "excitonmodel": "coupled", "excitonsize": 1200, "linewidth": "5.5[meV]", "lineshape": "gaussian"})x"));
        json.replace(R"("compound": "GaAs", "x": "0")", R"("compound": "GaAs", "x": "0", "gamma1": "6.80", "gamma2": "1.90", "gamma3": "2.73")");
        json.replace(R"("compound": "AlGaAs", "x": "0.24")",
                     R"("compound": "AlGaAs", "x": "0.24", "Eg": "1.798[eV]", "gamma1": "5.795", "gamma2": "1.534", "gamma3": "2.568")");
        const femtest::Run x = femtest::run(femtest::modelOf(json));
        QVERIFY2(x.error.isEmpty(), qPrintable(x.error));
        x.solution->select(0);
        const double Eb = femtest::global(*x.solution, "qw.Eb") / Q;
        const QString log = x.solution->log.join(QLatin1Char('\n'));
        qInfo("the sweep's point: e1-hh1 bound by %.4f meV; %s", Eb * 1e3, qPrintable(x.solution->log.filter(QLatin1String("spinors on")).join(QString())));
        QVERIFY2(Eb > 0.008 && Eb < 0.0095, qPrintable(QString::number(Eb)));
        QVERIFY2(!log.contains(QLatin1String("functions drawn together left out")), qPrintable(log));
    }

    // Two pairs of one electron and one hole, both drawn together: products
    // one function, taken as two orthonormal - their overlaps with the
    // functions drawn together short of positive at every k (S's least
    // eigenvalue below 0, as the 62.6 eV exciton's of the check of 21
    // October at some). There the functions drawn together are left out,
    // said (Result::dropped): the states those of the products alone - the
    // two not drawn together's, to 1e-9. Solved generalized, S not
    // positive, they ran away (or the solve failed).
    void overlapsShortOfPositiveLeftOut()
    {
        const double mr = 0.0567 * M0, epsr = 12.9, ry = rydberg(mr, epsr), hb2m = Hbar * Hbar / (2 * M0);
        exciton::Envelopes env;
        for (int p = 0; p <= 400; ++p) env.z.push_back(-20e-9 + 40e-9 * p / 400);
        auto layer = [&env](double width) {
            std::vector<double> x;
            double norm = 0;
            for (double z : env.z) {
                x.push_back(std::exp(-0.5 * z * z / (width * width)));
                norm += x.back() * x.back();
            }
            for (double& v : x) v /= std::sqrt(norm);
            return x;
        };
        env.electrons = {layer(2e-9)};
        const std::vector<double> h = layer(1.5e-9);
        env.holes = {{std::vector<std::complex<double>>(h.begin(), h.end())}};
        env.electronStiffness.assign(env.z.size(), hb2m / 0.067);
        env.holeStiffness = {std::vector<double>(env.z.size(), hb2m / 0.35)};
        exciton::Options o;
        o.epsr = epsr;
        o.reach = 0.15 * Q;
        o.spacing = 4e-3 * Q;
        o.maxSize = 4000;
        o.correlated = 1;
        o.beta = 1 / 2.5e-9;
        const exciton::Result one = exciton::solve(env, {parabolic(0, mr)}, o);
        o.correlated = 2;
        const exciton::Result two = exciton::solve(env, {parabolic(0, mr), parabolic(0, mr)}, o);
        o.correlated = 0;
        const exciton::Result plain = exciton::solve(env, {parabolic(0, mr), parabolic(0, mr)}, o);
        QVERIFY2(!one.energy.empty() && !two.energy.empty() && !plain.energy.empty(), qPrintable(one.note + two.note + plain.note));
        qInfo("one pair drawn together %.4f Ry; its product twice, drawn together %.6f Ry (%d k points fall back, the grid's %d and its tail's), not "
              "drawn %.6f Ry",
              one.energy[0] / ry, two.energy[0] / ry, two.dropped, two.points, plain.energy[0] / ry);
        QVERIFY2(one.dropped == 0 && two.dropped >= two.points, qPrintable(QStringLiteral("%1 %2 %3").arg(one.dropped).arg(two.dropped).arg(two.points)));
        QVERIFY2(std::abs(two.energy[0] / plain.energy[0] - 1) < 1e-9, qPrintable(QStringLiteral("%1 %2").arg(two.energy[0] / ry).arg(plain.energy[0] / ry)));
    }

    // Miller's 96 Å well between Al0.3Ga0.7As barriers to hard walls 20 nm
    // off (the check of 21 October): of the 14 valence subbands asked
    // ("vstates"), 6 in the well, the rest the barriers' box states - the
    // coupled excitons take those in the well alone ("continuum": bound),
    // and the warning asked for "vstates" 15 or more each time, chasing the
    // next box state ("the next in the wells, 4.18 meV below the last
    // taken"). Now none: the next in the wells, mostly in them and above
    // the barriers' valence top, is not near.
    void boxStatesNotAskedFor()
    {
        if (femtest::slowBuild()) QSKIP("its 14 valence subbands' coupled excitons: minutes under the sanitizers");
        const femtest::Run x = femtest::run(femtest::modelOf(femtest::heterostructure(
            QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("9.6"), QStringLiteral("20"),
            QStringLiteral(R"(, "valence": "kp4", "h1d": "0.25[nm]")"),
            QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.44[eV], 1.50[eV], 3)", "cstates": 2, "vstates": 14, "continuum": "bound",
                               "excitons": true, "excitonmodel": "coupled", "excitonsize": 600, "linewidth": "5.5[meV]", "lineshape": "gaussian"})x"))));
        QVERIFY2(x.error.isEmpty(), qPrintable(x.error));
        const QStringList said = x.solution->log.filter(QLatin1String("the coupled excitons take"));
        qInfo("%s", qPrintable(x.solution->log.filter(QLatin1String("the spectrum's subbands")).join(QString())));
        QVERIFY2(said.isEmpty(), qPrintable(said.join(QLatin1Char('\n'))));
    }

    // The coupled excitons on a superlattice's states (wishlist 4.2 of the
    // check of 21 October): one period, its ends periodic - a 10 nm GaAs
    // well between halves of 30 nm of Al0.3Ga0.7As, its wells apart: its
    // e1-hh1 exciton the single walled well's, bound alike to 1 %; its
    // spectrum's transitions there.
    void aSuperlatticesExcitons()
    {
        if (femtest::slowBuild()) QSKIP("two k·p spectra's coupled excitons: minutes under the sanitizers");
        auto spectrum = [](const char* ends) {
            return femtest::run(femtest::modelOf(femtest::heterostructure(
                QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("10"), QStringLiteral("15"),
                QStringLiteral(R"(, "valence": "kp4", "h1d": "0.25[nm]", "ends": "%1")").arg(QLatin1String(ends)),
                QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.45[eV], 1.52[eV], 8)", "cstates": 1, "vstates": 2,
                                   "excitons": true, "excitonmodel": "coupled", "excitonsize": 600, "linewidth": "5[meV]", "lineshape": "gaussian"})x"))));
        };
        const femtest::Run periodic = spectrum("periodic"), walls = spectrum("walls");
        QVERIFY2(periodic.error.isEmpty() && walls.error.isEmpty(), qPrintable(periodic.error + walls.error));
        periodic.solution->select(0);
        walls.solution->select(0);
        const double ep = femtest::global(*periodic.solution, "qw.Eb") / Q, ew = femtest::global(*walls.solution, "qw.Eb") / Q;
        qInfo("one period, periodic: e1-hh1 bound by %.4f meV; the walled well %.4f meV", ep * 1e3, ew * 1e3);
        QVERIFY2(ep > 0.005 && std::abs(ep / ew - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(ep).arg(ew)));
        QVERIFY(!periodic.solution->log.join(QLatin1Char(' ')).contains(QLatin1String("a spectrum of 0 ")));
    }

    // How many pairs are coupled, given ("excitonpairs" - the lowest 12 of
    // each kind of hole, k·p 16, a superlattice's above-barrier minibands
    // past them: the check of 21 October's n = 3): 2 of each kind here,
    // said; past 64 taken as 64, said.
    void pairsCoupledGiven()
    {
        auto spectrum = [](int pairs) {
            return femtest::run(femtest::modelOf(femtest::heterostructure(
                QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QStringLiteral("10"), QStringLiteral("10"),
                QStringLiteral(R"(, "h1d": "0.5[nm]")"),
                QStringLiteral(R"x({"type": "spectrum", "energies": "1.5[eV]", "states": 2, "excitons": true, "excitonmodel": "coupled",
                                   "excitonpairs": %1, "excitonsize": %2, "linewidth": "5[meV]", "lineshape": "gaussian"})x")
                    .arg(pairs)
                    .arg(femtest::slowBuild() ? 200 : 400))));
        };
        const femtest::Run two = spectrum(2), many = spectrum(80);
        QVERIFY2(two.error.isEmpty() && many.error.isEmpty(), qPrintable(two.error + many.error));
        const QString said = two.solution->log.join(QLatin1Char('\n'));
        QVERIFY2(said.contains(QRegularExpression(QStringLiteral("the 2 lowest of the [0-9]+ pairs of heavy holes coupled"))), qPrintable(said));
        QVERIFY2(many.study.warnings.join(QLatin1Char(' ')).contains(QLatin1String("its excitonpairs 80 taken as 64, the most it takes")),
                 qPrintable(many.study.warnings.join(QLatin1Char('\n'))));
    }

    // The bulk's absorption against GaAs's measured: Aspnes and Studna's
    // (Phys. Rev. B 27, 985, 1983), the library's fit to it from 0.05 to
    // 0.45 eV above the gap - α = 6.578e4/cm √(E - Eg)/E (eV), 15.1, 18.4
    // and 20.3 ×10³/cm at 1.55, 1.63 and 1.70 eV. A slab of GaAs alone
    // ("continuum": bound, the stack's thickness): its free carriers' √ε
    // 0.66 of it, Elliott's (its excitons, Sommerfeld's factor; Urbach's 6
    // meV) within 10 % (1.094, 0.989, 0.939) - the 3D absorption's absolute
    // strength, Kane's Ep 28.8 eV, n and |Mb|², checked against a
    // measurement. And the wells' with it: a wide well's free absorption per
    // its thickness the bulk's free carriers' (30, 40, 60 nm at 1.63 eV: 0.94,
    // 0.97, 0.98 of it) - their 2D steps' sum its 3D density of states
    // (wishlist 4.6 of the check of 21 October; W7 of the 20 October one).
    void theBulksAbsoluteStrength()
    {
        auto slab = [](const char* more) {
            return femtest::run(femtest::modelOf(femtest::heterostructure(
                QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("20"), QStringLiteral("20"),
                QStringLiteral(R"(, "h1d": "0.5[nm]")"),
                QStringLiteral(R"x({"type": "spectrum", "energies": "1.55[eV], 1.63[eV], 1.70[eV]", "states": 1, "continuum": "bound", "normalize": "stack",
                                   "linewidth": "2[meV]", "lineshape": "gaussian" %1})x").arg(QLatin1String(more)))));
        };
        const femtest::Run free = slab(""), elliott = slab(R"(, "bulkexcitons": true, "urbach": "6[meV]")");
        QVERIFY2(free.error.isEmpty() && elliott.error.isEmpty(), qPrintable(free.error + elliott.error));
        for (int k = 0; k < 3; ++k) {
            free.solution->select(k);
            elliott.solution->select(k);
            const double E = femtest::global(*free.solution, "qw.Ephoton") / Q, measured = 6.578e4 * std::sqrt(E - 1.424) / E * 100;   // (1/m)
            const double a0 = femtest::global(*free.solution, "qw.alphaTE"), ae = femtest::global(*elliott.solution, "qw.alphaTE");
            qInfo("%.2f eV: measured %.0f /cm; free carriers %.0f (%.3f), Elliott's %.0f (%.3f)", E, measured / 100, a0 / 100, a0 / measured, ae / 100, ae / measured);
            QVERIFY2(a0 / measured > 0.6 && a0 / measured < 0.72 && std::abs(ae / measured - 1) < 0.1, qPrintable(QStringLiteral("%1 %2").arg(a0 / measured).arg(ae / measured)));
        }
        free.solution->select(1);
        const double bulk = femtest::global(*free.solution, "qw.alphaTE");
        double last = 0;
        for (const char* L : {"30", "40", "60"}) {
            const femtest::Run well = femtest::run(femtest::modelOf(femtest::heterostructure(
                QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("AlGaAs"), QStringLiteral("0.3"), QLatin1String(L), QStringLiteral("10"),
                QStringLiteral(R"(, "h1d": "0.5[nm]")"),
                QStringLiteral(R"x({"type": "spectrum", "energies": "1.60[eV], 1.63[eV], 1.66[eV]", "states": 16, "linewidth": "10[meV]", "lineshape": "gaussian"})x"))));
            QVERIFY2(well.error.isEmpty(), qPrintable(well.error));
            QStringList r;
            for (int k = 0; k < 3; ++k) {
                well.solution->select(k);
                r << QString::number(femtest::global(*well.solution, "qw.alphaTE") / 100, 'f', 0);
            }
            well.solution->select(1);
            const double at = femtest::global(*well.solution, "qw.alphaTE");
            qInfo("a %s nm well at 1.60, 1.63, 1.66 eV: %s /cm (the bulk's free carriers at 1.63: %.0f; %.3f of it)", L, qPrintable(r.join(", ")), bulk / 100, at / bulk);
            QVERIFY2(at > last && std::abs(at / bulk - 1) < 0.07, qPrintable(QStringLiteral("%1 %2").arg(at / bulk).arg(last / bulk)));
            last = at;
        }
        QVERIFY2(std::abs(last / bulk - 1) < 0.03, qPrintable(QString::number(last / bulk)));
    }

    // The bulk's edge (wishlist 4.3 of the check of 21 October: Miller's
    // barriers' own at 1.74-1.76 eV, the bare √ε none below the gap): below
    // its lines Urbach's tail, e^(E/E_U) whatever lines lie above - a
    // GaAs slab's 52 and 42 meV under its gap e^(10/6) apart, to 1 %; with
    // neither, nothing there (the spectrum's 2 meV Gaussian's).
    void theBulksEdge()
    {
        auto slab = [](const char* more) {
            return femtest::run(femtest::modelOf(femtest::heterostructure(
                QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("20"), QStringLiteral("20"),
                QStringLiteral(R"(, "h1d": "0.5[nm]")"),
                QStringLiteral(R"x({"type": "spectrum", "energies": "1.3705[eV], 1.3805[eV]", "states": 1, "continuum": "bound", "normalize": "stack",
                                   "linewidth": "2[meV]", "lineshape": "gaussian" %1})x").arg(QLatin1String(more)))));
        };
        const femtest::Run urbach = slab(R"(, "bulkexcitons": true, "urbach": "6[meV]")"), plain = slab("");
        QVERIFY2(urbach.error.isEmpty() && plain.error.isEmpty(), qPrintable(urbach.error + plain.error));
        double a[2], b[2];
        for (int k = 0; k < 2; ++k) {
            urbach.solution->select(k);
            plain.solution->select(k);
            a[k] = femtest::global(*urbach.solution, "qw.alphaTE");
            b[k] = femtest::global(*plain.solution, "qw.alphaTE");
        }
        qInfo("below the gap: Urbach's %.4g, %.4g /cm (their ratio %.4f, e^(10/6) %.4f); the plain √ε's %.3g, %.3g", a[0] / 100, a[1] / 100, a[1] / a[0],
              std::exp(10.0 / 6), b[0] / 100, b[1] / 100);
        QVERIFY2(std::abs(a[1] / a[0] / std::exp(10.0 / 6) - 1) < 0.01 && a[0] > 1, qPrintable(QStringLiteral("%1 %2").arg(a[0]).arg(a[1])));
        QVERIFY2(b[1] < 1e-6 * a[1], qPrintable(QStringLiteral("%1 %2").arg(b[1]).arg(a[1])));
    }

    // The barriers' edge under the continuum "states" (the update check of
    // 21 October, 3: Miller's n = 3, a miniband above the barriers, and the
    // barriers' excitonic edge 30 meV above it, one only under each
    // continuum): the box's states their free carriers, the barriers' bulk
    // adds its excitons' excess - Elliott's with Urbach's tail less the free
    // carriers' -, counted once. Against a slab of the barrier alone
    // ("bound": Elliott's in place of its free carriers'), what each adds is
    // the same per the barriers' share of the line (50 of 60 nm): below
    // their gap Urbach's tail (52 and 42 meV under it e^(10/6) apart), 150
    // meV above it the Sommerfeld factor's excess. Urbach's width without
    // the bulk's excitons, unused under "states", said.
    void theBarriersEdgeUnderStates()
    {
        auto stack = [](bool slab, const char* continuum, const char* more) {
            return femtest::modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "below", "size": ["10", "25"]},
                             {"type": "rectangle", "name": "well", "position": ["0", "25"], "size": ["10", "10"]},
                             {"type": "rectangle", "name": "above", "position": ["0", "35"], "size": ["10", "25"]}],
                "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.5[nm]", "features": [
                  {"type": "material", "source": "user", "Ec": "1.520[eV]", "Ev": "0[eV]", "me": "0.067", "mhh": "0.377", "mlh": "0.09",
                   "mhhp": "0.111", "mlhp": "0.21", "Ep": "28.8[eV]", "nr": "3.6", "epsilonr": "12.53", "selection": {"only": ["well"]}},
                  {"type": "material", "source": "user", "Ec": "1.757575[eV]", "Ev": "-0.127925[eV]", "me": "0.0836", "mhh": "0.444", "mlh": "0.11",
                   "mhhp": "0.13", "mlhp": "0.24", "Ep": "27[eV]", "nr": "3.5", "epsilonr": "12.0", "selection": {"only": %1}}]}],
                "mesh": {"size": "normal", "custom": true, "hmax": "2"}}],
              "studies": [{"type": "study", "steps": [{"type": "spectrum", "energies": "1.8335[eV], 1.8435[eV], 2.0355[eV]", "states": 4,
                                                      "continuum": "%2", "normalize": "stack", "linewidth": "2[meV]", "lineshape": "gaussian" %3}]}]})json")
                                          .arg(slab ? QStringLiteral(R"(["below", "well", "above"])") : QStringLiteral(R"(["below", "above"])"),
                                               QLatin1String(continuum), QLatin1String(more))
                                          .toUtf8());
        };
        const char* edge = R"(, "bulkexcitons": true, "urbach": "6[meV]")";
        const femtest::Run states = femtest::run(stack(false, "states", "")), statesEdge = femtest::run(stack(false, "states", edge));
        const femtest::Run slab = femtest::run(stack(true, "bound", "")), slabEdge = femtest::run(stack(true, "bound", edge));
        QVERIFY2(states.error.isEmpty() && statesEdge.error.isEmpty() && slab.error.isEmpty() && slabEdge.error.isEmpty(),
                 qPrintable(states.error + statesEdge.error + slab.error + slabEdge.error));
        double added[3], alone[3];
        for (int k = 0; k < 3; ++k) {
            for (const femtest::Run* r : {&states, &statesEdge, &slab, &slabEdge}) r->solution->select(k);
            added[k] = femtest::global(*statesEdge.solution, "qw.alphaTE") - femtest::global(*states.solution, "qw.alphaTE");
            alone[k] = femtest::global(*slabEdge.solution, "qw.alphaTE") - femtest::global(*slab.solution, "qw.alphaTE");
            qInfo("%.4f eV: the barriers' edge adds %.5g /cm under \"states\", the slab's %.5g - %.4f of it", femtest::global(*states.solution, "qw.Ephoton") / Q,
                  added[k] / 100, alone[k] / 100, added[k] / alone[k]);
            QVERIFY2(alone[k] > 0 && std::abs(added[k] / alone[k] / (50.0 / 60) - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(added[k]).arg(alone[k])));
        }
        QVERIFY2(std::abs(added[1] / added[0] / std::exp(10.0 / 6) - 1) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(added[0]).arg(added[1])));
        const femtest::Run unused = femtest::run(stack(false, "states", R"(, "urbach": "6[meV]")"));
        QVERIFY2(unused.error.isEmpty(), qPrintable(unused.error));
        const QString said = (unused.study.warnings + unused.solution->log).join(QLatin1Char('\n'));
        QVERIFY2(said.contains(QLatin1String("its Urbach width is not used: with the continuum \"states\" it broadens the bulk's excitons' lines alone")),
                 qPrintable(said));
        QVERIFY2(!(statesEdge.study.warnings + statesEdge.solution->log).join(QLatin1Char(' ')).contains(QLatin1String("Urbach width is not used")),
                 qPrintable(statesEdge.study.warnings.join(QLatin1Char('\n'))));
    }

    // The barriers' bulk is each barrier's own - its material, gap and
    // thickness -, not the material where the line starts: a well there got
    // its own excitons over the barriers' thickness and its steps faded above
    // its own gap, and barriers of two alloys were 11 times apart mirrored
    // (the hunt of 22 October, A1). And their excess under "states" only
    // adds: a wide line's free part, or a Lorentzian's tail, took more than
    // the box gave - -468 /cm with no carriers, 115 /cm off half an eV below
    // the barriers' gap (A2).
    void theBarriersEachTheirOwn()
    {
        auto stack = [](const char* first, const char* firstX, const char* second, const char* secondX, const char* third, const char* thirdX,
                        const QString& sizes, const QString& spectrum) {
            const QStringList s = sizes.split(QLatin1Char(','));
            return femtest::modelOf(QString::fromUtf8(R"json({"format": "qucs-s multiphysics 1",
              "components": [{"unit": "nm",
                "geometry": [{"type": "rectangle", "name": "r1", "size": ["10", "%7"]},
                             {"type": "rectangle", "name": "r2", "position": ["0", "%7"], "size": ["10", "%8"]},
                             {"type": "rectangle", "name": "r3", "position": ["0", "%7 + %8"], "size": ["10", "%9"]}],
                "materials": [{"material": "a", "compound": "%1", "x": "%2", "selection": {"only": ["r1"]}},
                              {"material": "b", "compound": "%3", "x": "%4", "selection": {"only": ["r2"]}},
                              {"material": "c", "compound": "%5", "x": "%6", "selection": {"only": ["r3"]}}],
                "physics": [{"type": "quantum", "tag": "qw", "h1d": "0.5[nm]"}], "mesh": {"size": "normal", "custom": true, "hmax": "3"}}],
              "studies": [{"type": "study", "steps": [%10]}]})json")
                                          .arg(QLatin1String(first), QLatin1String(firstX), QLatin1String(second), QLatin1String(secondX),
                                               QLatin1String(third), QLatin1String(thirdX), s.value(0), s.value(1), s.value(2))
                                          .arg(spectrum)
                                          .toUtf8());
        };
        auto alphas = [](const femtest::Run& r) {
            std::vector<double> a;
            Solution& s = *r.solution;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                a.push_back(femtest::global(s, "qw.alphaTE"));
            }
            return a;
        };
        auto same = [](const std::vector<double>& a, const std::vector<double>& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t k = 0; k < a.size(); ++k)
                if (std::abs(a[k] - b[k]) > 1e-6 * std::max(std::abs(b[k]), 1.0)) return false;
            return true;
        };
        auto said = [](const std::vector<double>& a) {
            QStringList out;
            for (double x : a) out << QString::number(x / 100, 'g', 5);
            return out.join(QLatin1String(", "));
        };
        // Mirrored barriers of two alloys (8 nm GaAs between 30 nm of Al0.3 and
        // of Al0.1): the same spectrum either way round, each continuum.
        for (const char* continuum : {R"("bound")", R"("states", "bulkexcitons": true, "urbach": "6[meV]")"}) {
            const QString spectrum = QStringLiteral(R"({"type": "spectrum", "energies": "1.50[eV], 1.65[eV], 1.72[eV], 1.85[eV]", "states": 30, "linewidth": "5[meV]",
                                                       "lineshape": "gaussian", "normalize": "stack", "continuum": %1})")
                                         .arg(QLatin1String(continuum));
            const femtest::Run ab = femtest::run(stack("AlGaAs", "0.3", "GaAs", "0", "AlGaAs", "0.1", QStringLiteral("30,8,30"), spectrum));
            const femtest::Run ba = femtest::run(stack("AlGaAs", "0.1", "GaAs", "0", "AlGaAs", "0.3", QStringLiteral("30,8,30"), spectrum));
            QVERIFY2(ab.error.isEmpty() && ba.error.isEmpty(), qPrintable(ab.error + ba.error));
            qInfo("%s: Al0.3 first %s, Al0.1 first %s /cm", continuum, qPrintable(said(alphas(ab))), qPrintable(said(alphas(ba))));
            QVERIFY2(same(alphas(ab), alphas(ba)), qPrintable(said(alphas(ab)) + QLatin1String(" against ") + said(alphas(ba))));
        }
        // A well where the line starts (9.4 nm GaAs, 16 nm Al0.3): as the
        // barrier first - nothing below the well's first transition at 1.44 eV.
        for (const char* continuum : {R"("bound")", R"("states", "bulkexcitons": true, "urbach": "6[meV]")"}) {
            const QString spectrum = QStringLiteral(R"({"type": "spectrum", "energies": "1.40[eV], 1.44[eV], 1.60[eV], 1.85[eV]", "states": 8, "linewidth": "5[meV]",
                                                       "lineshape": "gaussian", "normalize": "stack", "continuum": %1})")
                                         .arg(QLatin1String(continuum));
            const femtest::Run wellFirst = femtest::run(stack("GaAs", "0", "AlGaAs", "0.3", "AlGaAs", "0.3", QStringLiteral("9.4,8,8"), spectrum));
            const femtest::Run barrierFirst = femtest::run(stack("AlGaAs", "0.3", "AlGaAs", "0.3", "GaAs", "0", QStringLiteral("8,8,9.4"), spectrum));
            QVERIFY2(wellFirst.error.isEmpty() && barrierFirst.error.isEmpty(), qPrintable(wellFirst.error + barrierFirst.error));
            qInfo("%s: the well first %s, the barrier first %s /cm", continuum, qPrintable(said(alphas(wellFirst))), qPrintable(said(alphas(barrierFirst))));
            QVERIFY2(same(alphas(wellFirst), alphas(barrierFirst)) && alphas(wellFirst)[1] < 1,
                     qPrintable(said(alphas(wellFirst)) + QLatin1String(" against ") + said(alphas(barrierFirst))));
        }
        // A2: few states, a wide line and a narrow Urbach (2 nm GaAs in 50 nm
        // of Al0.3): never negative; a Lorentzian line: nothing taken off half
        // an eV below the barriers' gap.
        const QString thin = QStringLiteral("50,2,50");
        const femtest::Run wide = femtest::run(stack("AlGaAs", "0.3", "GaAs", "0", "AlGaAs", "0.3", thin,
            QStringLiteral(R"x({"type": "spectrum", "energies": "linspace(1.78[eV], 1.88[eV], 11)", "states": 2, "linewidth": "30[meV]", "lineshape": "gaussian",
                               "continuum": "states", "normalize": "stack", "bulkexcitons": true, "urbach": "0.5[meV]"})x")));
        QVERIFY2(wide.error.isEmpty(), qPrintable(wide.error));
        for (double a : alphas(wide)) QVERIFY2(a >= 0, qPrintable(said(alphas(wide))));
        auto lorentzian = [&](bool edge) {
            return femtest::run(stack("AlGaAs", "0.3", "GaAs", "0", "AlGaAs", "0.3", thin,
                QStringLiteral(R"({"type": "spectrum", "energies": "1.30[eV], 1.40[eV]", "states": 1, "linewidth": "2[meV]", "lineshape": "lorentzian",
                                   "continuum": "states", "normalize": "stack" %1})")
                    .arg(edge ? QLatin1String(R"(, "bulkexcitons": true, "urbach": "6[meV]")") : QLatin1String(""))));
        };
        const femtest::Run plain = lorentzian(false), withEdge = lorentzian(true);
        QVERIFY2(plain.error.isEmpty() && withEdge.error.isEmpty(), qPrintable(plain.error + withEdge.error));
        QVERIFY2(same(alphas(withEdge), alphas(plain)) && alphas(withEdge)[0] > 0, qPrintable(said(alphas(withEdge)) + QLatin1String(" against ") + said(alphas(plain))));
    }

    // A flag given as an expression of the parameters, read at each point
    // of a sweep: "bulkexcitons": "ex" swept 0, 1 - its first point the
    // plain √ε's, its second Elliott's with its Urbach tail (it was false at
    // both, said nowhere: the update check of 21 October, 1). One that is
    // not a constant refused, the flag named; another study's not this one's
    // to refuse.
    void flagsOfTheParameters()
    {
        auto slab = [](const char* flag, const char* sweep) {
            QByteArray json = femtest::heterostructure(
                QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("GaAs"), QStringLiteral("0"), QStringLiteral("20"), QStringLiteral("20"),
                QStringLiteral(R"(, "h1d": "0.5[nm]")"),
                QStringLiteral(R"x(%2 {"type": "spectrum", "energies": "1.3705[eV], 1.3805[eV]", "states": 1, "continuum": "bound", "normalize": "stack",
                                   "linewidth": "2[meV]", "lineshape": "gaussian", "bulkexcitons": %1, "urbach": "6[meV]"})x")
                    .arg(QLatin1String(flag), QLatin1String(sweep)));
            json.replace(R"("parameters": [{"name": "Lw")", R"("parameters": [{"name": "ex", "expression": "1"}, {"name": "Lw")");
            return json;
        };
        auto alphas = [](const femtest::Run& r, int point) {
            Solution& s = *r.study.solutions[std::size_t(point)];
            std::vector<double> a;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                a.push_back(femtest::global(s, "qw.alphaTE"));
            }
            return a;
        };
        const femtest::Run swept = femtest::run(femtest::modelOf(slab(R"("ex")", R"({"type": "sweep", "parameters": [["ex", "0, 1", ""]]},)")));
        const femtest::Run off = femtest::run(femtest::modelOf(slab("false", ""))), on = femtest::run(femtest::modelOf(slab("true", "")));
        QVERIFY2(swept.error.isEmpty() && off.error.isEmpty() && on.error.isEmpty(), qPrintable(swept.error + off.error + on.error));
        QCOMPARE(swept.study.solutions.size(), std::size_t(2));
        const std::vector<double> a0 = alphas(swept, 0), a1 = alphas(swept, 1), plain = alphas(off, 0), elliott = alphas(on, 0);
        for (std::size_t k = 0; k < 2; ++k) {
            QVERIFY2(a0[k] == plain[k] && a1[k] == elliott[k] && elliott[k] > 10 * plain[k],
                     qPrintable(QStringLiteral("%1 %2 %3 %4").arg(a0[k]).arg(plain[k]).arg(a1[k]).arg(elliott[k])));
        }
        // (A comparison of the parameters: ex > 0.5 at ex 1 on.)
        const femtest::Run compared = femtest::run(femtest::modelOf(slab(R"("ex > 0.5")", "")));
        QVERIFY2(compared.error.isEmpty() && alphas(compared, 0) == elliott, qPrintable(compared.error));
        const femtest::Run refused = femtest::run(femtest::modelOf(slab(R"("maybe")", "")));
        QVERIFY2(refused.error.contains(QLatin1String(R"(its "bulkexcitons" is "maybe" - true, false or an expression of the parameters)")),
                 qPrintable(refused.error));
        // (Another study's mistake: this one, of flags as expressions too,
        // solves.)
        QByteArray two = slab(R"("ex")", "");
        two.replace(R"("studies": [)", R"("studies": [{"type": "study", "tag": "other", "steps": [{"type": "spectrum", "energies": "1.4[eV]", "bulkexcitons": "maybe"}]}, )");
        two.replace(R"({"type": "study", "steps")", R"({"type": "study", "tag": "this", "steps")");
        const Model model = femtest::modelOf(two);
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(model);
        const GeometryBuild g = buildGeometry(model, *ps);
        QVERIFY(g.ok());
        const StudyRun mine = runStudy(model, ps, g.topology, nullptr, QStringLiteral("this"));
        QVERIFY2(mine.ok(), qPrintable(mine.error));
        const StudyRun theirs = runStudy(model, ps, g.topology, nullptr, QStringLiteral("other"));
        QVERIFY2(theirs.error.contains(QLatin1String(R"("bulkexcitons" is "maybe")")), qPrintable(theirs.error));
        // A number 1, "True": on (a number was false, said nowhere; "True"
        // refused as an expression - the hunt of 22 October, A8 and N5).
        for (const char* on : {"1", R"("True")", R"("1")"}) {
            const femtest::Run r = femtest::run(femtest::modelOf(slab(on, "")));
            QVERIFY2(r.error.isEmpty() && alphas(r, 0) == elliott, qPrintable(QStringLiteral("%1: %2").arg(on, r.error)));
        }
        const femtest::Run zero = femtest::run(femtest::modelOf(slab("0", "")));
        QVERIFY2(zero.error.isEmpty() && alphas(zero, 0) == plain, qPrintable(zero.error));
        // A disabled step's, a disabled physics feature's or a plot's
        // malformed flag: not this solve's to refuse (each failed every study:
        // B4).
        QByteArray others = slab(R"("ex")", R"({"type": "spectrum", "enabled": false, "energies": "1.4[eV]", "bulkexcitons": "maybe"},)");
        others.replace(R"("studies": [)", R"("results": {"plots": [{"type": "plotgroup1d", "tag": "pg", "ylog": "maybe", "plots": []}]}, "studies": [)");
        const femtest::Run quiet = femtest::run(femtest::modelOf(others));
        QVERIFY2(quiet.error.isEmpty() && alphas(quiet, quiet.study.solutions.size() - 1) == elliott, qPrintable(quiet.error));
    }
};

QTEST_GUILESS_MAIN(TestFemExciton)
#include "test_fem_exciton.moc"
