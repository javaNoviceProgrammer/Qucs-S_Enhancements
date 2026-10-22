/*
 * fem_semiconductor.cpp - semiconductors: the drift-diffusion equations
 *                         (Poisson's for the potential, continuity for
 *                         electrons and holes) on linear triangles, each
 *                         edge's carrier current Scharfetter and Gummel's
 *                         weighed as the Laplacian weighs it (edge-averaged
 *                         finite elements: an M-matrix on a Delaunay mesh);
 *                         solved together by Newton's method from
 *                         equilibrium, the contacts' voltages raised in
 *                         steps; in time (BDF), and small-signal about a
 *                         solution (the contacts' admittance matrix); a
 *                         contact's current weighed over the device where
 *                         it is resolved, not taken from its own rows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_problem.h"
#include "fem_geometry.h"
#include "fem_line.h"
#include "fem_materials.h"
#include "fem_mesh.h"

#include <Eigen/SparseCholesky>
#include <Eigen/LU>
#include <Eigen/QR>
#include <Eigen/SparseLU>

#include <functional>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace qucs_s::fem::detail {

namespace {

/// setSemiconductorJacobianFault's (the tests').
std::atomic<bool> tunnellingFieldDropped{false};

constexpr double Q = 1.602176634e-19;    // C
constexpr double KB = 1.380649e-23;      // J/K
constexpr double Mass0 = 9.1093837015e-31, HbarSI = 1.054571817e-34;
const double Inf = std::numeric_limits<double>::infinity();
// The field's extra: Vt, T, ni_ref, then each triangle's µn, µp, Ec - Ei, Eg,
// ni, τn, τp, Et, Cn, Cp, B, δ, incomplete ionization's n1d, gd, p1a, ga,
// Fermi-Dirac's Nc, Nv (0 for Boltzmann's), the carriers' Joule heat as
// solved (W/m³), band-to-band tunnelling's A (SI), B, γ (A 0: none),
// trap-assisted tunnelling's FΓn, FΓp and (Ec - Et), (Et - Ev) over kT, its
// corners' steps from their nodes' bands to its, sN then sP (a node of a
// heterojunction's other semiconductor; 0 else) (NaN where it is no
// semiconductor); its values V, n, p, Nd, Na, θn, θp by node (θ with
// Fermi-Dirac's degeneracy in it, as solved), a laser's gain and
// stimulated rate by node and its photons, then incomplete ionization's
// Nd⁺ and Na⁻ by node (a_ionAt).
constexpr int ExtraHead = 3, PerTriangle = 39;

/// Bernoulli's function x / (e^x - 1) and its derivative, safe at 0 and far
/// from it.
double bern(double x)
{
    if (std::abs(x) < 1e-5) return 1 - x / 2 + x * x / 12;
    if (x > 600) return x * std::exp(-x);
    if (x < -600) return -x;
    return x / std::expm1(x);
}

double dbern(double x)
{
    if (std::abs(x) < 1e-5) return -0.5 + x / 6;
    if (x > 600) return (1 - x) * std::exp(-x);
    if (x < -600) return -1;
    const double em = std::expm1(x);
    return (em - x * (em + 1)) / (em * em);
}

/// A domain's semiconductor (or insulator) as solved.
struct Medium {
    bool semiconductor = false;
    double epsr = 1;
    double Eg = 0, chi = 0;          // J
    double Nc = 0, Nv = 0;           // 1/m³
    double mun = 0, mup = 0;         // m²/(V s): the lattice's (most)
    bool dopingMobility = false;     // Caughey and Thomas's
    double munMin = 0, nRefN = 1, alphaN = 1, mupMin = 0, nRefP = 1, alphaP = 1;
    // Masetti's: µmin1 e^(-Pc/N) + (µmax - µmin2)/(1 + (N/Cr)^α) - µ1/(1 + (Cs/N)^β),
    // by carrier: µmin1, µmin2, µ1, Pc, Cr, Cs, α, β; µmax its own or the
    // lattice's (m²/(V s), at the physics' T).
    bool masetti = false;
    double ms[2][8] = {};
    double msMax[2] = {0, 0};
    // The low-field mobility's models beyond these, by carrier (SI unless
    // said): Arora's µmin, a, µd, b, Nref, c, α, d - µmin Tn^a + µd Tn^b/(1
    // + (N/(Nref Tn^c))^(α Tn^d)), Tn = TL/300 K; Dorkel and Leturcq's µL,
    // γ, A, B (A and B in cm, V, s and K); Klaassen's µmax, µmin, θ, Nref,
    // α (and its dopants' clustering, its masses); Yamaguchi's µL, S, Nref,
    // as, uL, vs, G (and its lcrit); an expression of the state's.
    enum class LowField { Plain, Arora, Dorkel, Klaassen, Yamaguchi, Expression };
    LowField lowField = LowField::Plain;
    double ar[2][8] = {}, dl[2][4] = {}, kl[2][5] = {}, ya[2][7] = {};
    double klNrefD = 4e26, klNrefA = 7.2e26, klCD = 0.21, klCA = 0.5, klMe = 1, klMh = 1.258;
    Expression muExpr[2];
    bool exprState = false;          // the expression reads n, p, Epar, Eperp or dsurf
    bool exprAcross = false;         // ... Eperp or dsurf: the nearest interface
    // Shirahata's surface model (else Lombardi's): µ0, θ, E1, E2, P1, P2.
    bool shirahata = false;
    double sh[2][6] = {};
    bool saturation = false;         // high fields: Caughey and Thomas's too
    double vsatn = 1e5, vsatp = 8e4, betan = 2, betap = 1;
    double rHn = 1, rHp = 1;         // Hall factors: µH = r µ (a magnetic field's)
    bool wholeField = false;         // saturated by |E|, not by the force along the current
    // Hot carriers: each carrier's saturation by its temperature, an energy
    // balance's (energy relaxation times τw, s); the electrons' velocity the
    // transferred electrons' - negative differential mobility past E0 (V/m).
    bool energyN = false, energyP = false;
    double tauWn = 0.3e-12, tauWp = 0.25e-12;
    bool transferred = false;
    double E0n = 4e5;
    double zetan = 2.5, zetap = 2.2;   // the mobilities' (TL/T)^-ζ
    // The saturation velocities by temperature: 2.4e7/(1 + 0.8 e^(TL/600 K))
    // cm/s both, not Quay's of the velocities given.
    bool vsatExp = false;
    // Quay's saturation velocities of the lattice's temperature, vsat300/((1
    // - A) + A TL/300 K): A of each carrier (0: none).
    double vsatAn = 0, vsatAp = 0;
    // Under a lattice temperature TL: ln ni (ni in 1/m³) and the gap (J)
    // of its material at ThermalFrom + ThermalStep k (its own Eg(T),
    // Nc(T), Nv(T)); empty without TL.
    std::vector<double> lnNiAt, gapAt;
    // Lombardi's scattering at an insulator's surface: µac = B/E⊥ + C (N/1
    // cm⁻³)^λ / (T E⊥^⅓) (C in cm, V, s and K), µsr = δ/E⊥², E⊥ the field
    // across the nearest interface, each faded as e^(-d/lcrit) away from it.
    bool surface = false;
    /// Whether its mobility changes with the state (the carriers, the
    /// fields): updated with it, held a Newton step.
    bool varies() const
    {
        return saturation || surface || lowField == LowField::Dorkel || lowField == LowField::Klaassen || lowField == LowField::Yamaguchi
               || (lowField == LowField::Expression && exprState);
    }
    /// Whether it reads the field across the nearest interface.
    bool perpendicular() const { return surface || lowField == LowField::Yamaguchi || (lowField == LowField::Expression && exprAcross); }
    double sBn = 0, sCn = 0, sLn = 0, sDn = 0, sBp = 0, sCp = 0, sLp = 0, sDp = 0, lcrit = 1e-8;
    // Slotboom's band gap narrowing: ΔEg = E (ln(N/Nref) + √(ln²(N/Nref) +
    // C)) (J), N the donors and acceptors - ni raised by e^(ΔEg/2kT), the
    // bands closing on the intrinsic level equally.
    bool bgn = false;
    double bgnE = 0, bgnN = 1, bgnC = 0.5;
    // Incomplete ionization: Nd⁺ = Nd/(1 + gd n/n1d), n1d = Nc e^(-ΔEd/kT),
    // Na⁻ = Na/(1 + ga p/p1a), p1a = Nv e^(-ΔEa/kT).
    bool incomplete = false;
    double n1d = Inf, gd = 2, p1a = Inf, ga = 4;
    // Fermi-Dirac's statistics: n = Nc ℱ1/2(η), not Nc e^η.
    bool fermi = false;
    bool srh = false;
    double taun = Inf, taup = Inf, Et = 0;   // Et: the trap's level above the intrinsic one (J)
    Expression taunOf, taupOf;       // lifetimes of x and y (else taun, taup)
    // Their doping dependence, τ0/(A + B N/N0 + C (N/N0)^E) (Fossum's);
    // trap-assisted tunnelling's field, Hurkx's FΓ (V/m; Inf: none).
    bool fossum = false;
    double N0n = 1, N0p = 1, fA = 1, fB = 0, fC = 1, fE = 1;
    bool tat = false;
    double FGn = Inf, FGp = Inf;      // (and the trap's depth below each band: Ec - Et, Et - Ev, from its level)
    double Cn = 0, Cp = 0, B = 0;
    std::vector<Expression> generation;
    std::vector<double> generationPhoton;   // each one's pairs' energy as made (J; NaN: the gap's)
    std::vector<int> optical;        // the Optical Generation features on it (a_optical's)
    double ni = 0, dEc = 0;          // the intrinsic density; Ec - Ei (J)
    // Impact ionization: Chynoweth's α = a e^(-(b/E)^β) (1/m, V/m), each
    // carrier's below E0 and above it (van Overstraeten and de Man's holes).
    bool ionization = false, ionizationCurrents = false;
    // Band-to-band tunnelling, Hurkx's: R = A E^γ e^(-B/E) (n p - ni²)/((n +
    // ni)(p + ni)) (1/m³ s; A in SI for E in V/m), each triangle's field.
    bool tunnelling = false, btGeneration = false;   // generation alone, not Hurkx's sign and size
    double btA = 0, btB = 0, btGamma = 2.5;
    double an = 0, bn = 0, an2 = 0, bn2 = 0, ap = 0, bp = 0, ap2 = 0, bp2 = 0, E0 = Inf, ionBetaN = 1, ionBetaP = 1;
    double ionPhonon = 0;   // its optical phonon's energy (J): a, b of the temperature by γ (0: as given at any)
    bool ionFermi = false;   // impact ionization driven by each carrier's quasi-Fermi gradient (else the field)
    bool ionAlong = false;   // ... by the field along each carrier's current, E·J/|J|
};

/// The Fermi-Dirac integral of order 1/2, normalized - ℱ(η) = (2/√π) ∫0^∞
/// √ε/(1 + e^(ε-η)) dε, e^η where the carriers are few - and its derivative
/// ℱ-1/2: tabulated once from 20 kT below the band's edge to 100 above at
/// 1/32 (Simpson's rule in √ε), cubic between (ℱ to 2e-9, its derivative
/// to 2e-7, against a quadrature a hundred times finer); Boltzmann's two
/// terms below, Sommerfeld's above.
struct FermiHalf {
    static constexpr double Lo = -20, Hi = 100, Step = 1.0 / 32;
    std::vector<double> f, d;
    FermiHalf()
    {
        const int n = int((Hi - Lo) / Step + 0.5) + 1;
        f.resize(std::size_t(n));
        d.resize(std::size_t(n));
        const double c = 2 / std::sqrt(M_PI);
        for (int k = 0; k < n; ++k) {
            const double eta = Lo + k * Step;
            // t = √ε: ℱ = (4/√π) ∫ t²/(1 + e^(t²-η)) dt, ℱ' = (2/√π) ∫ dt/(1 + e^(t²-η)).
            const double T = std::sqrt(std::max(eta, 0.0) + 60);
            const int m = 4000;
            const double h = T / m;
            double sf = 0, sd = 0;
            for (int j = 0; j <= m; ++j) {
                const double t = j * h, w = (j == 0 || j == m) ? 1 : (j % 2 ? 4 : 2);
                const double x = t * t - eta;
                const double occupied = x > 0 ? std::exp(-x) / (1 + std::exp(-x)) : 1 / (1 + std::exp(x));
                sf += w * t * t * occupied;
                sd += w * occupied;
            }
            f[std::size_t(k)] = 2 * c * sf * h / 3;
            d[std::size_t(k)] = c * sd * h / 3;
        }
    }
    void at(double eta, double* F, double* dF) const
    {
        if (eta < Lo) {
            const double e = std::exp(eta);
            *F = e * (1 - e / (2 * std::sqrt(2.0)));
            *dF = e * (1 - e / std::sqrt(2.0));
            return;
        }
        if (eta >= Hi) {
            const double c = 4 / (3 * std::sqrt(M_PI)), p2 = M_PI * M_PI;
            *F = c * std::pow(eta, 1.5) * (1 + p2 / (8 * eta * eta) + 7 * p2 * p2 / (640 * std::pow(eta, 4)));
            *dF = c * 1.5 * std::sqrt(eta) * (1 - p2 / (24 * eta * eta) - 7 * p2 * p2 / (384 * std::pow(eta, 4)));
            return;
        }
        const double u = (eta - Lo) / Step;
        const int k = std::min(int(u), int(f.size()) - 2);
        const double s = u - k, h = Step;
        const double f0 = f[std::size_t(k)], f1 = f[std::size_t(k + 1)], d0 = d[std::size_t(k)], d1 = d[std::size_t(k + 1)];
        *F = (2 * s * s * s - 3 * s * s + 1) * f0 + (s * s * s - 2 * s * s + s) * h * d0 + (-2 * s * s * s + 3 * s * s) * f1 + (s * s * s - s * s) * h * d1;
        *dF = (6 * s * s - 6 * s) / h * f0 + (3 * s * s - 4 * s + 1) * d0 + (-6 * s * s + 6 * s) / h * f1 + (3 * s * s - 2 * s) * d1;
    }
    /// η where ℱ(η) = \a u (Newton's in ln ℱ, from Joyce and Dixon's start
    /// or Sommerfeld's).
    double inverse(double u) const
    {
        double eta = u < 1 ? std::log(u) + u / std::sqrt(8.0) : std::pow(0.75 * std::sqrt(M_PI) * u, 2.0 / 3);
        for (int k = 0; k < 60; ++k) {
            double F = 0, dF = 0;
            at(eta, &F, &dF);
            const double step = (std::log(F) - std::log(u)) * F / dF;
            eta -= step;
            if (std::abs(step) < 1e-13 * std::max(1.0, std::abs(eta))) break;
        }
        return eta;
    }
};

const FermiHalf& fermiHalf()
{
    static const FermiHalf table;
    return table;
}

// A material's values of the lattice's temperature, tabulated from
// ThermalFrom by ThermalStep (K), cubic between (Catmull and Rom's), held
// at the ends.
constexpr double ThermalFrom = 150, ThermalStep = 5;
constexpr int ThermalPoints = 211;   // to 1200 K

double tabulated(const std::vector<double>& v, double T)
{
    if (v.empty()) return NaN;
    const double u = std::clamp((T - ThermalFrom) / ThermalStep, 0.0, double(v.size() - 1));
    const int k = std::min(int(u), int(v.size()) - 2);
    if (k < 0) return v.front();
    const double f = u - k;
    const double p0 = v[std::size_t(std::max(k - 1, 0))], p1 = v[std::size_t(k)], p2 = v[std::size_t(k + 1)],
                 p3 = v[std::size_t(std::min(k + 2, int(v.size()) - 1))];
    return p1 + 0.5 * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)));
}

/// Quay's saturation velocity at \a T (K) over its 300 K value, 1/((1 -
/// A) + A T/300 K).
double quayVsat(double A, double T)
{
    return 1 / ((1 - A) + A * T / 300);
}

/// The saturation velocity of both carriers at \a T (K), m/s: 2.4e7/(1 +
/// 0.8 e^(T/600 K)) cm/s - the mobility overview's eq. 15, a device
/// simulator's default (1.035e7 cm/s at 300 K).
double expVsat(double T)
{
    return 2.4e5 / (1 + 0.8 * std::exp(T / 600));
}

/// Arora, Hauser and Roulston's mobility (1982) of \a c (µmin, a, µd, b,
/// Nref, c, α, d; SI) at \a N dopants (1/m³) and \a T (K): µmin Tn^a + µd
/// Tn^b/(1 + (N/(Nref Tn^c))^(α Tn^d)), Tn = T/300 K.
double aroraMobility(const double* c, double N, double T)
{
    const double tn = T / 300;
    return c[0] * std::pow(tn, c[1]) + c[2] * std::pow(tn, c[3]) / (1 + std::pow(N / (c[4] * std::pow(tn, c[5])), c[6] * std::pow(tn, c[7])));
}

/// Dorkel and Leturcq's mobility (1981) of \a c (µL in m²/(V s), γ, A, B
/// in cm, V, s and K) at \a N dopants, carriers \a n and \a p (1/m³) and
/// \a T (K): the lattice's µL (T/300 K)^-γ; the dopants' µI = A T^1.5/N
/// (ln(1 + B T²/N) - B T²/(N + B T²))⁻¹; the carriers' µcc = 2e17 T^1.5/√(np)
/// / ln(1 + 8.28e8 T² (np)^-⅓); together µL (1.025/(1 + (X/1.68)^1.43) -
/// 0.025), X = √(6 µL (µI + µcc)/(µI µcc)) - in cm, as its paper's.
double dorkelMobility(const double* c, double N, double n, double p, double T)
{
    const double lattice = c[0] * 1e4 * std::pow(T / 300, -c[1]);   // cm²/(V s)
    const double Ncm = N * 1e-6, np = std::max(n, 0.0) * std::max(p, 0.0) * 1e-12;
    // (Each scattering's inverse: none where there is nothing to scatter.)
    double invI = 0, invCC = 0;
    if (Ncm > 1) {
        const double bt = c[3] * T * T / Ncm;
        invI = (std::log1p(bt) - bt / (1 + bt)) * Ncm / (c[2] * std::pow(T, 1.5));
    }
    if (np > 1) invCC = std::log1p(8.28e8 * T * T / std::cbrt(np)) * std::sqrt(np) / (2e17 * std::pow(T, 1.5));
    const double X = std::sqrt(6 * lattice * (invI + invCC));
    return lattice * (1.025 / (1 + std::pow(X / 1.68, 1.43)) - 0.025) * 1e-4;
}

/// Klaassen's screening functions (1992): G(P) at \a T (K) of a carrier of
/// mass \a m (m0), its minimum's P (below it G is the minimum's: as
/// device simulators take it), and F(P) of the mass ratio \a ratio.
double klaassenG(double P, double T, double m)
{
    return 1 - 0.89233 / std::pow(0.41372 + P * std::pow(T / (300 * m), 0.28227), 0.19778)
           + 0.005978 / std::pow(P * std::pow(300 * m / T, 0.72169), 1.80618);
}
double klaassenPmin(double T, double m)
{
    // (G' changes sign once: bisected in ln P.)
    double lo = std::log(1e-4), hi = std::log(1e2);
    auto slope = [T, m](double u) {
        const double P = std::exp(u), h = 1e-6 * P;
        return klaassenG(P + h, T, m) - klaassenG(P - h, T, m);
    };
    for (int k = 0; k < 60; ++k) {
        const double mid = 0.5 * (lo + hi);
        (slope(mid) < 0 ? lo : hi) = mid;
    }
    return std::exp(0.5 * (lo + hi));
}
double klaassenF(double P, double ratio)
{
    // (r5 -0.8552, device simulators' - Klaassen's paper's -0.01552.)
    const double q = std::pow(P, 0.6478);
    return (0.7643 * q + 2.2999 + 6.5505 * ratio) / (q + 2.3670 - 0.8552 * ratio);
}

/// Klaassen's unified mobility (1992) of carrier \a c (0 electrons, 1
/// holes) of \a m at donors \a Nd, acceptors \a Na, carriers \a n and \a p
/// (1/m³) and \a T (K), \a pmin its G's minimum's P there: 1/µ = 1/µL +
/// 1/µDA, the lattice's µmax Tn^-θ; the dopants' (clustered: Nd (1 + 1/(cD
/// + (Nref,D/Nd)²))) and the other carriers' µN (Nsc/Nsc,eff) (Nref/Nsc)^α
/// + µc (n + p)/Nsc,eff - Nsc those that scatter it, attracting and
/// repelling, Nsc,eff the repelling screened by G(P) and the other
/// carriers by F(P), P Conwell and Weisskopf's and Brooks and Herring's
/// screening together. (As device simulators have it - Xyce's.)
double klaassenMobility(const Medium& m, int c, double Nd, double Na, double n, double p, double T, double pmin)
{
    const double* k = m.kl[c];   // µmax, µmin, θ, Nref, α
    const double tn = T / 300;
    auto cluster = [](double N, double cl, double Nref) { return N > 0 ? N * (1 + 1 / (cl + (Nref / N) * (Nref / N))) : 0.0; };
    const double nd = cluster(Nd, m.klCD, m.klNrefD), na = cluster(Na, m.klCA, m.klNrefA);
    // (A carrier per cm³ at least.)
    n = std::max(n, 1e6);
    p = std::max(p, 1e6);
    const double mass = c == 0 ? m.klMe : m.klMh, ratio = c == 0 ? m.klMe / m.klMh : m.klMh / m.klMe;
    const double attract = c == 0 ? nd : na, repel = c == 0 ? na : nd, other = c == 0 ? p : n;
    const double Nsc = std::max(attract + repel + other, 1e6);
    const double cm3 = Nsc * 1e-6;
    const double Pcw = 3.97e13 / std::cbrt(cm3 * cm3), Pbh = 1.36e20 / ((n + p) * 1e-6) * mass;
    const double P = tn * tn / (2.459 / Pcw + 3.828 / Pbh);
    const double G = klaassenG(std::max(P, pmin), T, mass), F = klaassenF(P, ratio);
    const double Neff = attract + G * repel + other / F;
    const double muL = k[0] * std::pow(tn, -k[2]);
    const double muN = k[0] * k[0] / (k[0] - k[1]) * std::pow(tn, 3 * k[4] - 1.5);
    const double muC = k[0] * k[1] / (k[0] - k[1]) / std::sqrt(tn);
    const double muDA = muN * (Nsc / Neff) * std::pow(k[3] / Nsc, k[4]) + muC * (n + p) / Neff;
    return 1 / (1 / muL + 1 / muDA);
}

/// Yamaguchi's low-field mobility (1979) of \a c (µL, S, Nref, as, uL, vs,
/// G; SI) at \a N dopants (1/m³): µL (1 + N/(N/S + Nref))^-½.
double yamaguchiLowField(const double* c, double N)
{
    return c[0] / std::sqrt(1 + N / (N / c[1] + c[2]));
}

/// Van Overstraeten and de Man's γ at \a T (K) for an optical phonon of
/// \a phonon (J; none: 1): tanh(ħω/2k 300 K)/tanh(ħω/2kT) - the carriers'
/// mean free path between phonons, λ ∝ tanh(ħω/2kT) (Crowell and Sze),
/// shorter as the lattice warms: α = γ a e^(-(γ b/E)^β).
double phononGamma(double phonon, double T)
{
    if (!(phonon > 0) || !(T > 0)) return 1;
    return std::tanh(phonon / (2 * KB * 300)) / std::tanh(phonon / (2 * KB * T));
}

/// What tunnels through a heterojunction's spike \a step (J) high under a
/// field \a F (V/m), against what is emitted over it: δ = (1/kT) ∫0^step
/// e^(u/kT) T(u) du, u the depth below its top, T(u) = e^(-4√(2m) u^(3/2) /
/// 3qħF) a triangle's WKB transmission (m its mass, kg) - the carriers
/// beneath its top through its tip (Grinberg et al., a heterojunction
/// bipolar transistor's emitter).
double tunnelling(double F, double step, double m, double kT)
{
    if (!(F > 0) || !(step > 0) || !(m > 0)) return 0;
    const double hbar = 1.054571817e-34, c = 4 * std::sqrt(2 * m) / (3 * Q * hbar * F);
    const int n = 400;   // (Simpson's)
    const double du = step / n;
    double sum = 0;
    for (int k = 0; k <= n; ++k) {
        const double u = k * du;
        const double f = std::exp(u / kT - c * u * std::sqrt(u));
        sum += (k == 0 || k == n ? 1 : k % 2 ? 4 : 2) * f;
    }
    return sum * du / 3 / kT;
}

/// Chynoweth's ionization rate at field \a E (V/m) in its range, and its
/// change with E.
void ionizationRate(double E, double a, double b, double a2, double b2, double E0, double beta, double* alpha, double* dalpha)
{
    if (!(E > 1e3)) {
        *alpha = *dalpha = 0;
        return;
    }
    const bool high = E > E0;
    const double r = std::pow((high ? b2 : b) / E, beta);
    *alpha = (high ? a2 : a) * std::exp(-r);
    *dalpha = *alpha * beta * r / E;
}

/// Hurkx's trap-assisted tunnelling at field \a F (V/m), the trap \a a
/// (its depth below the carrier's band over kT) deep: Γ = a ∫0^1 e^(a u - K
/// u^(3/2)) du, K = (4/3) √(2 m* ΔE³)/(q ħ F) = (2/3√3) a^(3/2) FΓ/F, and its
/// change with F. (Its saddle point's 2√(3π) (F/FΓ) e^((F/FΓ)²) is twice to
/// thrice this where the field is moderate: the integrand's peak by u = 0.)
void hurkx(double F, double FG, double a, double* gamma, double* dgamma)
{
    *gamma = *dgamma = 0;
    if (!(F > 0) || !std::isfinite(FG) || !(a > 0)) return;
    const double K = 2 / (3 * std::sqrt(3.0)) * std::pow(a, 1.5) * FG / F;
    // (Simpson's rule on 256 intervals - the peak's width some 1/√(9K²/8a).)
    const int n = 256;
    double sum = 0, dsum = 0;
    for (int k = 0; k <= n; ++k) {
        const double u = double(k) / n, w = k == 0 || k == n ? 1 : k % 2 ? 4 : 2;
        const double u32 = u * std::sqrt(u);
        const double e = std::exp(std::min(a * u - K * u32, 600.0));
        sum += w * e;
        dsum += w * e * K * u32 / F;   // (dK/dF = -K/F)
    }
    *gamma = a * sum / (3 * n);
    *dgamma = a * dsum / (3 * n);
}

/// Light absorbed from a mode, made electron-hole pairs: the power fed P0
/// (of t in time), each photon absorbed across the gap a pair (η of them).
struct OpticalSource {
    QString mode;                    // the mode analysis's tag
    int number = 1;                  // which of its modes (1: the highest index)
    Expression absorbed;             // its <tag>.Qgap (W/m³, the mode at 1 W)
    Expression power;                // P0's
    double P0 = 0, eta = 1, RL = 50;
    int slices = 1;                  // the device cut along its length into
};

struct Contact {
    enum Kind { Ohmic, Schottky, Gate };
    QString name;
    Kind kind = Ohmic;
    Expression V0;
    double phiB = 0;                 // a Schottky contact's barrier (J)
    double vn = 1e5, vp = 1e5;       // its recombination velocities (m/s)
    double phim = 0;                 // a gate's work function (J)
    std::vector<int> nodes;
    std::vector<double> length;      // by node: the contact's area there (m²: a length times the thickness)
    double applied = 0;              // the voltage the solution is at (V): a circuit's source's
    double current = 0, charge = 0;  // A into the device, C on the metal
    // Its circuit: held at V0, or V0 through Rs and a load RL in series (at
    // rest and for a signal alike), or through a bias tee (Rs at rest, the
    // signal into RL); a capacitance Cp across the device at the contact.
    // Its own voltage then an unknown (iC), solved with the carriers.
    enum Circuit { Held, Series, BiasTee };
    Circuit circuit = Held;
    double Rs = 0, RL = 0, Cp = 0;   // Ω, Ω, F
    int iC = -1;
    double voltage = 0;              // the contact's own voltage as solved (V)
    double teeCurrent = 0;           // a bias tee's current at rest (A): in time its load carries the change from it
    double capCurrent = 0;           // Cp's, Cp dVc/dt (A, in time)
    double solvedR = 0;              // its resistance at rest as last solved with (Ω)
    double walkR = NaN;              // on a walk, its resistance at rest on the way there (NaN: its own)
    /// Its resistance in the current's path at rest: Rs and RL in series, a
    /// bias tee's Rs.
    double restR() const { return circuit == Series ? Rs + RL : circuit == BiasTee ? Rs : 0.0; }
    /// The current through its series resistance toward the device: the
    /// device's and Cp's (A).
    double seriesCurrent() const { return current + capCurrent; }
    /// Its load's current toward the device (A): the series current, or a
    /// bias tee's change from rest (none at rest).
    double loadCurrent(bool inTime) const
    {
        if (circuit == Series) return seriesCurrent();
        if (circuit == BiasTee) return inTime ? seriesCurrent() - teeCurrent : 0.0;
        return 0.0;
    }
};

class SemiconductorProblem : public Problem
{
public:
    SemiconductorProblem(Solution& solution, int field) : Problem(solution, field) {}

    bool setup(QString* error) override;
    void initialize() override;
    bool solve(const SolveOptions& options, QString* error) override;
    bool solveAt(const SolveOptions& options, QString* error);
    /// Its materials' largest Eg/2kT: past some 150 its carriers' statistics
    /// give out (ni e^(-Eg/2kT) of its states').
    double a_gapOverKT = 0;
    void setGlobals() override;
    bool hasMass() const override { return true; }
    bool harmonic() const override { return true; }
    bool holdContact(const QString& name, double volts) override
    {
        bool known = false;
        for (const Contact& c : a_contacts) known = known || c.name == name;
        if (!known) return false;
        if (std::isfinite(volts)) a_held[name] = volts;
        else a_held.erase(name);
        return true;
    }
    QString lasingMode() const override { return a_laser ? a_laser->mode : QString(); }
    std::optional<GainRequest> gainRequest() const override
    {
        if (!a_laser || a_laser->gain != 2) return std::nullopt;
        return a_laser->request;
    }
    void setGainTable(std::shared_ptr<const GainTable> table) override
    {
        if (a_laser) a_laser->table = std::move(table);
    }
    bool readyToLase(QString* why) override { return a_laser && prepareLasing(why); }

private:
    std::map<QString, double> a_held;   // contacts a step holds at a voltage of its own
    struct Edge {
        int i = -1, j = -1;
        double w = 0;                // Σ cot weights (m², the thickness in)
        double wn = 0, wp = 0;       // times the elements' mobilities (the lattice's, by doping)
        double h = 0;                // its length (m)
        int domain = -1;             // a semiconductor's it is in (one that ionizes, if one does)
        double tx = 0, ty = 0;       // its direction, i to j
        // Its triangles' bands (the first domain's of them; an edge along a
        // heterojunction is two, one each side's). Its carriers are theirs
        // at both ends - the bands' step at an end's node of the other's,
        // abrupt, not along the edge: from that node's bands to these, sN and
        // sP (V), its density times fN = e^(sN/Vt), fP = e^(-sP/Vt).
        int medium = -1;
        int partner = -1;            // the same nodes' edge on the other side
        double sNi = 0, sNj = 0, sPi = 0, sPj = 0;
        double fNi = 1, fNj = 1, fPi = 1, fPj = 1;
        // From a heterojunction's node into its other side, emitted over its
        // step (thermionic, tunnelling): each carrier's Richardson velocity
        // v_R = √(kT/2π m*), its step (J) and tunnelling mass (kg) - its own
        // side's (the barrier's) density of states' mass. 0: drift and
        // diffusion alone.
        double vRn = 0, vRp = 0, stepN = 0, stepP = 0, massN = 0, massP = 0;
        // (Its face's share across the interface, |t·n|: an edge at an
        // angle to it carries the current across as much less. Its end on
        // the interface: i, else j.)
        double across = 0;
        bool emitsAtI = false;
        // Its triangles' largest mobility of each carrier, times q Vt: its
        // own, wn/w, bounded by it where its current density is taken - a
        // ratio of two sums of cotangents, where they all but cancel (a
        // sliver of a face) two triangles' mobilities apart made it anything.
        double muN = 0, muP = 0;
    };
    int nodes() const { return a_n; }
    double doping(int i) const { return a_Nd[std::size_t(i)] - a_Na[std::size_t(i)]; }
    /// Equilibrium's carriers at a node of net doping \a C (no applied
    /// voltage), and their potential in Vt above the Fermi level's, v.
    void neutral(int i, double* n, double* p, double* v = nullptr) const;
    /// The contacts' voltages at the solution's time.
    bool targets(std::vector<double>& out, QString* error);
    /// The values a Dirichlet node takes (V, n, p) at the contacts' applied voltages.
    void contactValues(const std::vector<double>& applied);
    /// F(x) and (\a jac) its Jacobian - interior rows; the contacts' rows as
    /// they are (their reactions), Dirichlet rows replaced in \a system.
    void assemble(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, Eigen::VectorXd& F,
                  std::vector<Eigen::Triplet<double>>* jac) const;
    /// Newton's method from \a x at the contacts' values set; how many steps
    /// in \a steps.
    /// Newton's method from x; \a met first: none of its steps where x
    /// meets the equations already.
    /// \a consistent: its mobilities, held once settled, its solution's own
    /// at its end (a walk's last solve).
    bool newton(Eigen::VectorXd& x, double mass, const std::vector<double>* history, int maxSteps, int* steps, QString* why, bool met = false,
                bool consistent = false);
    /// Poisson's equation alone, the carriers at equilibrium with a Fermi
    /// level of -q \a fermi, the contacts at \a applied; from \a x's
    /// potential (\a warm) or charge neutrality's.
    bool equilibriumPoisson(Eigen::VectorXd& x, double fermi, const std::vector<double>& applied, bool warm, QString* why);
    /// Whether the contacts that hold the carriers are at one voltage (in
    /// \a fermi) and nothing makes carriers: no current flows, and the
    /// carriers are at equilibrium - Poisson's equation alone is exact.
    bool atEquilibrium(const std::vector<double>& goal, double* fermi) const;
    /// At rest: from where the solution is to the contacts' voltages, in steps.
    bool stationary(QString* why);
    bool smallSignal(double frequency, QString* why);
    /// One slice's small signal at its own \a x and pairs (useSlice()'s):
    /// the contacts' admittance \a Y (S) and the light's currents \a H (A,
    /// the light modulated wholly; empty without it).
    bool smallSignalSlice(const Eigen::VectorXd& x, double frequency, std::vector<std::vector<std::complex<double>>>& Y,
                          std::vector<std::complex<double>>& H, QString* why,
                          std::vector<std::vector<std::complex<double>>>* noise = nullptr);
    // Noise (the physics' option): each contact's short-circuit noise
    // currents' correlations (A²/Hz) at the last frequency, and through each
    // circuit's load.
    bool a_noise = false;
    // A magnetic flux density out of the plane (T): each carrier's current
    // turned, J = (J0 + c ẑ×J0)/(1 + c²), c = -rn µn Bz for electrons, rp µp
    // Bz for holes; each triangle's low-field mobilities as the edges were
    // weighed (its J0's), and the turned currents along the edges at x (A,
    // i to j; electrons at 2e, holes at 2e + 1) with their change in \a d
    // (rows those indices).
    double a_Bz = 0;
    std::vector<std::array<double, 2>> a_triMu;
    void hallFluxes(const Eigen::VectorXd& x, std::vector<double>& flux, std::vector<Eigen::Triplet<double>>* d) const;
    std::vector<std::vector<std::complex<double>>> a_noiseC;
    std::vector<double> a_noiseL;
    /// At equilibrium: the gates' quasi-static capacitance - the carriers
    /// following, their Fermi level held at \a fermi - from Poisson's equation
    /// linearized: the low-frequency limit, well conditioned (the small-signal
    /// system is not at 1 µHz: a MOS capacitor's inversion layer is held by
    /// almost nothing).
    void quasiStatic(const Eigen::VectorXd& x, double fermi, const std::vector<double>& applied);
    /// Poisson's equation with the carriers at equilibrium (Fermi level
    /// \a fermi): its residual on the potential's unknowns (\a Fv, by \a
    /// vIndex) and Jacobian (\a J, contacts' rows as assembled when \a
    /// keepContacts, else fixed).
    void equilibriumSystem(const Eigen::VectorXd& x, double fermi, const std::vector<int>& local, const std::vector<int>& vIndex,
                           const std::vector<char>& fixed, bool keepContacts, Eigen::VectorXd& Fv, Eigen::SparseMatrix<double>& J) const;
    void store(const Eigen::VectorXd& x);
    /// A solution with current: said when its elements where the current
    /// crosses a heterojunction are wider than a Debye length there.
    void heterojunctionMesh(const Eigen::VectorXd& x);
    /// Its ionized dopants at \a x into the field (a_ionAt's; degeneracy
    /// as last assembled).
    void storeIons(const Eigen::VectorXd& x);
    Eigen::VectorXd stateVector() const;
    /// The contacts' currents and charges at \a x (at equilibrium, \a
    /// still: no current, exactly).
    void terminalValues(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, bool still = false);
    /// By contact (empty for a gate): a weight over the nodes, 1 on its own,
    /// 0 on the other contacts' that hold carriers, between them as the
    /// carriers' conductance at \a x has it - flat where they are many. A
    /// contact's current is the edges' currents times the weight's change
    /// along them: the same as its rows' reaction where the equations hold,
    /// but not the difference of a 10^19 cm^-3 contact's two large fluxes
    /// (each edge's ~100 A per metre, its last digits pA).
    std::vector<std::vector<double>> currentWeights(const Eigen::VectorXd& x);
    /// The edge mobilities at the fields of \a x (high fields lower them).
    void updateMobilities(const Eigen::VectorXd& x);
    /// Hot carriers: each carrier's temperature by node from its energy
    /// balance at \a x, the edges' fluxes as weighed now (a_Tn, a_Tp; empty
    /// where no medium has it).
    void carrierTemperatures(const Eigen::VectorXd& x);
    std::vector<double> a_Tn, a_Tp;
    // (Each node's last temperature and its update's, solved less used: a
    // secant's slope for the next.)
    std::vector<double> a_TnWas, a_TpWas, a_dTnWas, a_dTpWas;
    // (And all of them at most ω of the way: halved where the updates grew
    // - a space-charge-limited layer's current, all its temperatures'
    // together -, half again where they fell; the last's largest.)
    double a_omegaT[2] = {1, 1}, a_residualT[2] = {0, 0};
    bool a_energy = false;
    // (Off along a stationary walk, its mobilities the field's: the
    // temperatures solved at its end, in turn with its Newton's method.)
    bool a_energyActive = true;
    /// Medium \a m's gap narrowing at node \a i, by its doping (J).
    /// And under a lattice temperature, its thermal shift.
    double narrowing(const Medium& m, int i) const
    {
        const double N = a_Nd[std::size_t(i)] + a_Na[std::size_t(i)];
        double g = 0;
        if (m.bgn && N > 0) {
            const double l = std::log(N / m.bgnN);
            g = m.bgnE * (l + std::sqrt(l * l + m.bgnC));
        }
        return g + thermalShift(m, i);
    }
    /// Medium \a m's at node \a i at its lattice's temperature TL: the
    /// narrowing that takes its intrinsic density from ni(T) to its
    /// material's ni(TL), 2kT ln(ni(TL)/ni(T)) (J; nought without TL) - the
    /// carriers' kT the physics' T, their bands the lattice's.
    double thermalShift(const Medium& m, int i) const
    {
        if (a_nodeTL.empty() || m.lnNiAt.empty() || !(a_nodeTL[std::size_t(i)] > 0)) return 0.0;
        return 2 * KB * a_T * (tabulated(m.lnNiAt, a_nodeTL[std::size_t(i)]) - tabulated(m.lnNiAt, a_T));
    }
    /// ... and its gap's own narrowing there, Eg(T) - Eg(TL) (J): what is
    /// shown of the bands (the rest of the shift its densities of states'
    /// and kT's, ni's).
    double thermalGap(const Medium& m, int i) const
    {
        if (a_nodeTL.empty() || m.gapAt.empty() || !(a_nodeTL[std::size_t(i)] > 0)) return 0.0;
        return tabulated(m.gapAt, a_T) - tabulated(m.gapAt, a_nodeTL[std::size_t(i)]);
    }
    /// Its bands at node \a i, θn and θp (V): the electron affinity's and
    /// the band edges' offsets from the reference's, and the densities of
    /// states' as ni has them (the gap's narrowing a step of its own,
    /// e^(ΔEg/2kT)).
    std::pair<double, double> bandsIn(const Medium& m, int i) const
    {
        const double delta = (m.chi + m.dEc - a_refChi - a_refDEc) / Q;
        const double spread = a_Vt * std::log(m.ni * std::exp(narrowing(m, i) / (2 * KB * a_T)) / a_refNi);
        return {delta + spread, delta - spread};
    }
    /// Along edge \a e at \a x: the electrons' and the holes' potential
    /// steps in Vt - the potential's, and across a heterojunction the bands'
    /// offsets (θn, θp: the intrinsic level's, and the densities of states'
    /// as ni has them), and Fermi-Dirac's degeneracy (ln γ, last assembled).
    void edgeSteps(const Edge& e, const Eigen::VectorXd& x, double* dn, double* dp) const
    {
        const double dV = x(a_iV[std::size_t(e.j)]) - x(a_iV[std::size_t(e.i)]);
        const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
        *dn = (dV + a_thetaN[j] + e.sNj - a_thetaN[i] - e.sNi) / a_Vt + a_lgN[j] - a_lgN[i];
        *dp = (dV + a_thetaP[j] + e.sPj - a_thetaP[i] - e.sPi) / a_Vt - a_lgP[j] + a_lgP[i];
    }

    int a_n = 0;                       // nodes (P1 degrees of freedom)
    double a_T = 300, a_Vt = 0.025852;
    // A lattice temperature's (TL) at each node (K; empty: none), as the
    // bands were last made; and each domain's bands' domain.
    std::vector<double> a_nodeTL;
    std::vector<int> a_bandsOf;
    /// The nodes' bands again at their lattice's temperatures: gaps,
    /// intrinsic densities, θ, a heterojunction's steps.
    void thermalBands();
    /// The edges' steps into a heterojunction's other semiconductor and
    /// the nodes' boxes for each carrier (of the nodes' bands).
    void heteroSteps();
    /// The field's extras of the bands, each triangle's: its gap, Ec - Ei,
    /// ni, its trap's depths, its corners' heterojunction steps.
    void bandExtras();
    double a_scale = 1;                // a carrier scale (1/m³): the largest doping or ni
    std::vector<Medium> a_media;       // by domain
    std::vector<char> a_elemSemi;      // by triangle: of a semiconductor in the physics
    std::vector<char> a_elemOn;        // in the physics
    std::vector<int> a_nodeDomain;     // a node's semiconductor domain (-1: none)
    std::vector<double> a_box;         // by node: its share of semiconductor (m³), its Voronoi box's
    // By node: its box as its carriers fill it (m³) - each part of another
    // semiconductor's bands (a barrier's beside a well's node) holding that
    // one's carriers, n e^(sN/Vt), p e^(-sP/Vt): the step from the node's
    // bands to its. (The whole box the node's, as it was, a heterojunction's
    // narrow gap's carriers over the wide gap's side too: a sheet of charge
    // the mesh's, its barrier raised.)
    std::vector<double> a_boxN, a_boxP;
    struct BoxPart {
        int domain = -1;
        double volume = 0;   // m³
    };
    std::vector<int> a_partStart;      // by node: its range of a_parts
    std::vector<BoxPart> a_parts;      // its box's share of each domain
    // By node: the parts of its box that recombine - its share of each domain
    // of its own bands, with that domain's lifetimes (a_recStart's range of
    // a_rec). A part in a semiconductor of other bands (a barrier beside a
    // well) holds that one's carriers, fewer by the bands' offset, not the
    // node's: it is left out.
    struct RecombiningPart {
        int domain = -1;
        double volume = 0;   // m³
        double tn = 0, tp = 0;
    };
    std::vector<int> a_recStart;
    std::vector<RecombiningPart> a_rec;
    std::vector<double> a_recVolume;   // by node: the sum of its parts' volumes
    std::vector<double> a_Nd, a_Na, a_ni, a_n1, a_p1;
    // By node: its SRH lifetimes τ0 (of x and y, by doping); trap-assisted
    // tunnelling's field there - its triangles' fields, by their volumes
    // (a_fieldStart's range of a_fieldTris: triangle, share).
    std::vector<double> a_tauN, a_tauP;
    std::vector<int> a_fieldStart;
    std::vector<std::pair<int, double>> a_fieldTris;
    bool a_tat = false;
    std::vector<double> a_gap;         // by node: the band gap's narrowing (J)
    bool a_incomplete = false;
    // Fermi-Dirac's statistics by node, at the carriers last assembled: ln γ
    // (n = γ Nc e^η, γ = ℱ1/2(η)/e^η - 1 where they are few), its change
    // with the carriers, and ℱ'/ℱ (δn = n ℱ'/ℱ δη); Boltzmann's 0, 0, 1. Each
    // carrier's band offset θ takes Vt ln γ: the flux as Boltzmann's, its
    // diffusion the generalized Einstein relation's.
    bool a_fermi = false;
    mutable std::vector<double> a_lgN, a_lgP, a_dlgN, a_dlgP, a_rN, a_rP;
    // Schrödinger-Poisson: by node, the quantum potentials Λn, Λp (V) taken
    // into the bands' offsets - θn + Λn, θp - Λp: the carriers there e^(Λ/Vt)
    // Boltzmann's, as the states have them; the offsets without them; the
    // nodes in the regions asked, and the states' carriers' expressions.
    std::vector<double> a_lamN, a_lamP, a_thetaN0, a_thetaP0;
    std::vector<int> a_quantumNodes;
    Expression a_quantumN, a_quantumP;
    std::vector<Eigen::VectorXd> a_andX, a_andG;   // Anderson's history: Λ/Vt and its steps
    bool a_quantumElectrons = false, a_quantumHoles = false;
    double quantumCorrection(QString* error) override;
    // Electrothermal: the lattice temperature for the mobilities (another
    // physics', ht.T), each triangle's factors from it at each solve.
    Expression a_TL;
    void latticeTemperature();
    void degeneracy(const Eigen::VectorXd& x) const;
    /// Node \a i's carriers at v, the potential in Vt above its Fermi
    /// level's (Boltzmann's n = ni e^v): Fermi-Dirac's where asked; their
    /// changes with v.
    void carriersAt(int i, double v, double* n, double* p, double* dn, double* dp) const;
    /// Node \a i's ionized donors and acceptors at \a n, \a p (1/m³) - the
    /// carriers' Boltzmann densities, n/γn and p/γp: the dopants filled by
    /// the Fermi level whatever the carriers' statistics - and their changes
    /// with them.
    void ionized(int i, double n, double p, double* nd, double* dnd, double* na, double* dna) const;
    /// The same at node \a i of \a x (its degeneracy as last assembled):
    /// the ionized donors and acceptors, and their change with n and with p.
    void ionizedAt(int i, const Eigen::VectorXd& x, double* nd, double* dndn, double* na, double* dnap) const
    {
        const std::size_t k = std::size_t(i);
        const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
        double dnd = 0, dna = 0;
        ionized(i, a_iN[k] >= 0 ? x(a_iN[k]) / gn : 0.0, a_iP[k] >= 0 ? x(a_iP[k]) / gp : 0.0, nd, &dnd, na, &dna);
        *dndn = dnd / gn;
        *dnap = dna / gp;
    }
    // Incomplete ionization's: the field's ionized donors, then acceptors, by
    // node from here (0: none) - their history a step in time's, as the
    // carriers'. A carrier a dopant takes leaves its band through no
    // current: each continuity's store the carriers' less their dopants'
    // ions, n - Nd⁺ and p - Na⁻, as its charge is.
    std::size_t a_ionAt = 0;
    // Circuits: the field's values from here, each contact's own voltage
    // and a bias tee's current at rest (0: none); the voltages the contacts'
    // values were last set at (contactValues(): a circuit's source's, its
    // nodes' offsets from it); the circuits' rows' worst residual, last
    // assembled (in units of their voltages).
    std::size_t a_circuitAt = 0;
    std::size_t a_lightAt = 0;
    // The last of Newton's solves held its mobilities before its potential
    // settled (from its fifteenth step): its solution theirs, not its own.
    bool a_heldUnsettled = false;
    double a_lightScale = 1;           // a walk's share of the light, on its way to it (1: all of it)
    std::vector<double> a_lastApplied;
    double a_circuitResidual = 0;
    /// Each circuit's row at \a x - V0 - Vc - R I (I into the device, Cp's
    /// with it in time), R its resistance in the current's path (Rs and RL
    /// in series; a bias tee's Rs at rest, Rs and RL in time, its load
    /// carrying the current's change from rest) - into \a G, its Jacobian's
    /// entries into \a kept (scaled by Newton's columns); I as reported (the
    /// edges' currents weighed, the carriers' change and the metal's charge's
    /// in time) from \a F and \a trip as assembled at x (the contacts' rows
    /// their reactions).
    struct CircuitCurrent {
        double I = 0, gross = 0;      // A; its terms' sizes summed (A)
        std::map<int, double> dI;     // its change by unknown, among all slices'
    };
    /// Slice \a s's share of each circuit's current (\a share its length's):
    /// I at its own \a x, with \a F and \a trip as assembled there - into
    /// \a out.
    void sliceCurrents(int s, double share, const Eigen::VectorXd& x, double mass, const std::vector<double>* history, const Eigen::VectorXd& F,
                       const std::vector<Eigen::Triplet<double>>& trip, std::vector<CircuitCurrent>& out);
    void circuitRows(const Eigen::VectorXd& X, double mass, const std::vector<double>* history, const std::vector<CircuitCurrent>& currents,
                     const Eigen::VectorXd& colScale, Eigen::VectorXd& G, std::vector<Eigen::Triplet<double>>& kept);
    /// Whether any contact is behind a circuit.
    bool hasCircuits() const
    {
        for (const Contact& c : a_contacts)
            if (c.iC >= 0) return true;
        return false;
    }
    /// The circuits' contacts' own voltages at equilibrium: their sources'
    /// (\a applied - no current through their resistances).
    void circuitsAtRest(Eigen::VectorXd& x, const std::vector<double>& applied) const
    {
        for (std::size_t k = 0; k < a_contacts.size(); ++k)
            if (a_contacts[k].iC >= 0) x(a_contacts[k].iC) = applied[k];
    }
    // By node, across heterojunctions: the intrinsic level's offset from the
    // first semiconductor's (V: Ei = -q (V + δ)), and the electrons' and
    // holes' band potentials θn = δ + Vt ln(ni/ni_ref), θp = δ - Vt ln(ni/ni_ref)
    // - n = ni_ref exp((V + θn - φn)/Vt), p = ni_ref exp((φp - V - θp)/Vt).
    std::vector<double> a_delta, a_thetaN, a_thetaP;
    double a_refNi = 1;
    std::vector<std::array<double, 9>> a_K;   // by triangle: ε0 εr ∫∇φi·∇φj (weighted)
    std::vector<Edge> a_edges;
    // By triangle of a semiconductor: its low-field mobilities, its edges
    // (k to k+1) and their cot weights, its shape functions' gradients.
    struct ElementMobility {
        double mun = 0, mup = 0;
        int edge[3] = {-1, -1, -1};
        double cot[3] = {0, 0, 0};
        double half = 0;             // a cot weight's per cotangent: half its thickness's weight
        double gx[3] = {0, 0, 0}, gy[3] = {0, 0, 0};
        int domain = -1;
        double volume = 0;           // its area times the thickness (m³)
        double N = 0;                // its donors and acceptors (1/m³)
        double Nd = 0, Na = 0;       // each (Klaassen's, an expression's)
        double n0 = 0, p0 = 0;       // its carriers at rest (its corners' neutral ones' mean)
        double pmin[2] = {0, 0};     // Klaassen's G's minimum's P, by carrier, at its temperature
        // Surface scattering's: the nearest interface's normal, e^(-d/lcrit),
        // its distance d (m; infinite where none is near).
        double nx = 0, ny = 0, damp = 0, dist = Inf;
        // The lattice's temperature's: (TL/T)^-ζ of each carrier's, its
        // saturation velocities' and impact ionization's (γ(TL)/γ(T)) factors.
        double tn = 1, tp = 1, vn = 1, vp = 1, ii = 1;
        double TL = 0;   // (its lattice's temperature, K; 0: the physics' T)
        // A corner on another semiconductor's bands: a heterojunction's step
        // in it, its carriers over it emitted, not drifting - not saturated.
        bool step = false;
    };
    std::vector<ElementMobility> a_elements;
    /// Triangle \a t's mobilities at \a x (m²/(V s)): its low-field ones,
    /// scattered at the nearest insulator's surface by the field across it,
    /// saturated by the force along each carrier's current - the low-field
    /// ones alone without \a x.
    void elementMobilities(int t, const Eigen::VectorXd* x, double* mun, double* mup) const;
    /// The edges' weights from the elements' mobilities at \a x (when there
    /// is one, and the model asks).
    void edgeWeights(const Eigen::VectorXd* x);
    /// A triangle's mobilities' change with its unknowns (\a col: V, n, p at
    /// its corners) at a solution - those that change (a saturation, a
    /// surface's scattering): the small signal's.
    struct MobilityChange {
        int t = -1;
        int col[9] = {};
        double dn[9] = {}, dp[9] = {};
    };
    std::vector<MobilityChange> mobilityChanges(const Eigen::VectorXd& x) const;
    std::vector<int> a_iV, a_iN, a_iP; // by node: unknowns (-1 none)
    int a_unknowns = 0;
    // A laser (its Lasing feature): the photons S of a resonator's mode the
    // last unknown (a_iS, -1 none), solved with the carriers - their
    // balance's row, the stimulated recombination in the carriers' rows.
    // What they read of the mode is set at each solve (prepareLasing).
    struct Lasing {
        QString name, mode;          // the feature's, the resonator's tag
        std::vector<char> on;        // by domain
        int gain = 0;                // 0 logarithmic, 1 linear, 2 its quantum wells' (table)
        GainRequest request;         // (2: what it asks for)
        std::shared_ptr<const GainTable> table;
        double g0 = 0, Ntr = 0, Ns = 0, a = 0;
        int carrier = 0;             // 0 √(n p), 1 n, 2 p
        double beta = 0, ng = 0, eta = 1;
        bool ready = false;          // the mode read
        double omega = 0, tauP = 0, photon = 0, Gamma = 0;
        std::vector<int> nodes;      // those on its domains
        std::vector<double> share;   // by entry: its part of the mode's energy (∫ we dV / U over its part of the domains)
        std::vector<double> volume;  // its part of the domains' volume (m³)
        std::vector<double> speed;   // the photons' there, c/ng (m/s)
        std::vector<double> B;       // the radiative coefficient there (m³/s)
    };
    std::unique_ptr<Lasing> a_laser;
    int a_iS = -1;
    bool a_laserSaid = false;
    double a_meshSaidRatio = 0;   // heterojunctionMesh()'s worst said, its line
    QString a_meshSaid;
    /// The lasing's material gain (1/m) at carriers n, p; its derivatives.
    double laserGain(double n, double p, double* dn, double* dp) const;
    /// What the lasing reads of its mode, as its resonator solved it (false and why: none).
    bool prepareLasing(QString* why);
    /// The lasing at x: the photons, the modal gain rate G (1/s), the
    /// spontaneous emission Rsp and stimulated recombination (pairs/s) on its
    /// domains, their carriers' and gain's means (by volume, by share).
    struct LaserState {
        double S = 0, G = 0, Rsp = 0, Rstim = 0, Nact = 0, gmean = 0;
    };
    LaserState laserState(const Eigen::VectorXd& x) const;
    std::vector<Contact> a_contacts;
    std::vector<int> a_nodeContact;    // by node (-1 none)
    std::vector<double> a_bcV, a_bcN, a_bcP;   // by node: a Dirichlet node's values
    std::vector<std::pair<int, Expression>> a_surface;   // node, charge density's expression
    std::vector<double> a_surfaceLength;                 // by entry: area (m²)
    // Interface traps, by level at a node: its share of the interface (m²),
    // density (1/m²), n1 and p1 (1/m³: ni e^(±(Et - Ei)/kT)), cn and cp (σ
    // vth, m³/s), and whether acceptor-like.
    struct TrapLevel {
        int node = -1;
        double area = 0, N = 0, n1 = 0, p1 = 0, cn = 0, cp = 0;
        bool acceptor = true;
        bool charged = true;   // false: a surface's recombination alone (N 1, cn Sn, cp Sp)
        /// A surface's recombination of its minority carriers alone (its
        /// "Apply to majority carriers" off): at an ohmic contact, the
        /// majority held at equilibrium, the minority by this flux.
        bool minorityOnly = false;
    };
    std::vector<TrapLevel> a_traps;
    /// By node: an ohmic contact's node whose minority carriers a surface's
    /// recombination takes - free, not held at equilibrium.
    std::vector<char> a_minorityFree;
    /// Where its way to the contacts' voltages is, told with each of
    /// Newton's steps (empty: not on its way).
    QString a_walkSaid;
    /// Whether node \a i's electrons are its minority carriers (its
    /// equilibrium's: an ohmic contact's values).
    bool electronsMinority(int i) const { return a_bcN[std::size_t(i)] < a_bcP[std::size_t(i)]; }
    // Fowler-Nordheim tunnelling through an insulator, after the solve: by
    // boundary edge, its length (m², the thickness in) and the insulator's
    // triangle beside it, with J = A E² e^(-B/E) at the barrier's mean field.
    struct FowlerNordheim {
        std::vector<std::array<int, 2>> edges;   // its node pair
        std::vector<int> triangles;              // the insulator's triangle at each
        std::vector<double> lengths;
        double A = 0, B = 0;
        double phi = 0;                          // the barrier, φB/q (V)
    };
    std::vector<FowlerNordheim> a_fn;
    /// Level \a t at Boltzmann's carriers \a n, \a p: its occupancy f, its
    /// rate 1/τ = cn (n + n1) + cp (p + p1), the pairs it recombines per area
    /// U, and their changes with n and p.
    struct TrapState {
        double f, D, U, dfdn, dfdp, dUdn, dUdp;
    };
    static TrapState trapState(const TrapLevel& t, double n, double p);
    std::vector<std::pair<int, std::array<double, 3>>> a_nodePoint;   // a node's triangle and reference point
    std::vector<double> a_generation;  // by node (1/(m³ s)), at the last solve
    // By node, the energy its pairs are made with (W/m³): each photon's
    // ħω over η for a mode's light (the photons absorbed across the gap
    // all of it, pairs or not), a User-Defined Generation's photon energy
    // or the gap's there; by slice too.
    std::vector<double> a_pairEnergy;
    std::vector<std::vector<double>> a_slicePairEnergy;
    std::vector<OpticalSource> a_optical;
    // Stimulated recombination (Lumerical's): on its domains, c (⟨R⟩ - R0)
    // above threshold, R0 = J_th/(q t), ⟨R⟩ the domains' mean recombination
    // at the last iterate (a_lastR, by node).
    struct Stimulated {
        std::vector<char> on;   // by domain
        double c = 0, R0 = 0;
    };
    std::vector<Stimulated> a_stimulated;
    mutable std::vector<double> a_lastR;
    // By node: its recombination as last assembled (pairs per second over
    // its box's recombining parts - the thermal generation's less, the
    // light's apart).
    mutable std::vector<double> a_recombined;
    /// Each node's stimulated rate (1/m³ s) from a_lastR; none without it.
    std::vector<double> stimulatedRates() const;
    double a_Gop = 0, a_Pin = 0;       // the optical generation's pairs per second, the light fed (W)
    double a_Pabs = 0;                 // the power the pairs are made with, absorbed (W)
    std::vector<std::complex<double>> a_Hop;   // small-signal: each contact's current, the light modulated wholly (A)
    std::vector<std::complex<double>> a_Hc;    // the same through each circuit's load (A; empty: no circuit)
    bool a_inTime = false;                     // the last solve a step in time's
    // Slices along the length (an Optical Generation's): each the cross-
    // section at its place along the device, the light falling along it.
    // The first's unknowns a solution's vector's (with the circuits'), each
    // other's a_sliceSize after (a_total in all); each slice its share of
    // the length (its currents weighed by it), its pairs by node, its start
    // along the device (then the end), and - their mobilities saturated by
    // their own fields - its edges' weights. The field's values: the first's
    // as one slice's, the others' V, n, p (and ions) in blocks after
    // (a_sliceBlock each from a_slicesAt).
    int a_slices = 1;
    int a_sliceSize = 0;
    int a_total = 0;
    std::vector<double> a_sliceWeight, a_sliceZ;
    std::vector<std::vector<double>> a_sliceGeneration;
    std::vector<std::vector<std::array<double, 4>>> a_sliceEdges;   // by slice: each edge's wn, wp, muN, muP
    std::vector<std::vector<double>> a_sliceCurrent;   // by slice: each contact's current, its own length's (A)
    std::size_t a_slicesAt = 0, a_sliceBlock = 0;
    mutable int a_sliceNow = 0;
    int sliceOffset(int s) const { return s == 0 ? 0 : a_unknowns + (s - 1) * a_sliceSize; }
    /// Slice \a s's share of the length (its currents weighed by it): even
    /// before the light has set them.
    double sliceWeight(int s) const
    {
        if (a_slices <= 1) return 1.0;
        return std::size_t(s) < a_sliceWeight.size() && a_sliceWeight.size() == std::size_t(a_slices) ? a_sliceWeight[std::size_t(s)] : 1.0 / a_slices;
    }
    /// Slice \a s's unknown \a k (its own vector's) among all of them.
    int globalIndex(int s, int k) const { return k < a_sliceSize ? sliceOffset(s) + k : k; }
    /// Slice \a s's own vector (a_unknowns long: its V, n, p, the circuits'
    /// voltages) from all of them \a X.
    Eigen::VectorXd sliceOf(const Eigen::VectorXd& X, int s) const
    {
        Eigen::VectorXd x = X.head(a_unknowns);
        if (s > 0) x.head(a_sliceSize) = X.segment(sliceOffset(s), a_sliceSize);
        return x;
    }
    /// The first slice's V, n and p in each other's (at rest, no light: all alike).
    void broadcastSlices(Eigen::VectorXd& X) const
    {
        for (int s = 1; s < a_slices; ++s) X.segment(sliceOffset(s), a_sliceSize) = X.head(a_sliceSize);
    }
    /// A step in time's history for slice \a s: its own blocks in the
    /// first's places.
    std::vector<double> sliceHistory(const std::vector<double>& h, int s) const;
    /// Slice \a s assembled next: its pairs, its edges' weights (as its
    /// mobilities last were).
    void useSlice(int s);
    /// Slice \a s's edges' weights kept, as they are now.
    void keepSliceEdges(int s);
    /// The pairs made by node (1/(m³ s)) for the slice assembled now.
    const std::vector<double>& generationNow() const
    {
        return a_slices > 1 && std::size_t(a_sliceNow) < a_sliceGeneration.size() && !a_sliceGeneration[std::size_t(a_sliceNow)].empty()
                   ? a_sliceGeneration[std::size_t(a_sliceNow)]
                   : a_generation;
    }
    const std::vector<double>& pairEnergyNow() const
    {
        return a_slices > 1 && std::size_t(a_sliceNow) < a_slicePairEnergy.size() && !a_slicePairEnergy[std::size_t(a_sliceNow)].empty()
                   ? a_slicePairEnergy[std::size_t(a_sliceNow)]
                   : a_pairEnergy;
    }
    /// The contacts' currents and charges at all slices' \a X: each slice's
    /// weighed by its share of the length (terminalValues()'s), in time
    /// with its metal's charge's change.
    void terminalAll(const Eigen::VectorXd& X, double mass, const std::vector<double>* history, bool still = false);
    /// In time, each contact's metal's charge's change (mass Q(x) - Q at the
    /// history's) into its current, and its dopants' ions'.
    void displacementCurrents(const Eigen::VectorXd& x, double mass, const std::vector<double>& history);
    bool a_saturation = false, a_lombardi = false;
    bool a_varying = false;   // some domain's mobility changes with the state (Medium::varies)
    // A mobility's expression's variables' slots (N, Nd, Na, T, n, p, Epar,
    // Eperp, dsurf) and how many there are.
    int a_slotN = -1, a_slotNd = -1, a_slotNa = -1, a_slotT = -1, a_slotn = -1, a_slotp = -1, a_slotEpar = -1, a_slotEperp = -1,
        a_slotD = -1, a_exprSlots = 0;
    /// A low-field model's mobilities of an element at \a T, its carriers \a
    /// n and \a p (1/m³), the force along each carrier's current \a Fn,
    /// \a Fp and the field across the nearest interface \a perp (V/m).
    void lowFieldMobilities(const Medium& m, const ElementMobility& em, double T, double n, double p, double Fn, double Fp, double perp,
                            double* mun, double* mup) const;
    // Heterojunctions: 0 drift and diffusion (the step at the node, the
    // quasi-Fermi levels continuous), 1 thermionic emission over it in
    // series with each edge into it, 2 and tunnelling through its spike.
    int a_emission = 0;
    // A mobility held once settled in Newton's method, made its solution's
    // own at the end: a saturation driven by the field along the current, a
    // low-field mobility of the carriers or the fields (Dorkel and
    // Leturcq's, Klaassen's, Yamaguchi's, an expression of them).
    bool a_alongCurrent = false;
    double a_refDEc = 0, a_refChi = 0; // the first semiconductor's: Ec - Ei, χ (J)
    // Impact ionization: whether its pairs are in the equations; the
    // ionization integrals, the most of the field lines' through where it
    // ionizes (electrons', holes'), the highest field (V/m), the line's of
    // the electrons' most its length (m) and its highest field's place
    // (m); its pairs' current, q ∫ G, as last assembled (A).
    bool a_ionization = false, a_ionizationAny = false;
    bool a_tunnelling = false;
    double a_In = NaN, a_Ip = NaN, a_Emax = NaN, a_ionLength = NaN, a_ionX = NaN, a_ionY = NaN;
    int a_ionLines = 0;
    mutable double a_ionCurrent = 0;
    mutable double a_genCurrent = 0;   // the pairs made otherwise, q ∫ G: thermal, tunnelling, light, surfaces (A)
    bool a_ionFermi = false;   // some domain's impact ionization driven by the quasi-Fermi gradients
    mutable std::vector<double> a_ionRate;   // by triangle: its pairs per volume (1/(m³ s)), as last assembled
    std::vector<std::array<int, 3>> a_neighbors;   // by triangle: across its edge k to k+1 (-1: none)
    /// The ionization integrals at \a x (a_In, a_Ip...).
    void ionizationIntegrals(const Eigen::VectorXd& x);
    QString a_initError;
    int a_clamped = 0;
    // Small-signal: the admittance matrix by contacts (S), at a frequency.
    std::vector<std::vector<std::complex<double>>> a_Y;
    double a_frequency = 0;
    // At equilibrium: the gates' quasi-static capacitance matrix (F), by gate.
    std::vector<std::vector<double>> a_C0;
    std::vector<int> a_gates;
    // The weights' system (its pattern the mesh's: analysed once).
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> a_weightSolver;
    int a_weightSize = -1;
    // Solved in 1D: along a_line (a_axis1d 0 x, 1 y; its elements a_h1d
    // long) - on a strip of its own, an auxiliary model's - its solution
    // set at each node by its coordinate.
    bool a_1d = false;
    int a_axis1d = 1;
    double a_h1d = 1e-9;
    Line a_line;
    bool solve1d(QString* error);
};

/// A band energy (eV, or V as eV per electron, or a plain number in eV)
/// in joules; NaN and why when it is not a constant.
double energyOf(const QString& text, const Scope& scope, QString* why)
{
    const Expression e = Expression::compile(text, scope);
    if (!e.isValid() || !e.isConstant()) {
        *why = e.isValid() ? tr("%1 must be a constant").arg(text) : e.error();
        return NaN;
    }
    if (e.dim() == Dim::of(2, 1, -2)) return e.constant();
    return e.constant() * Q;   // V or a plain number: eV
}

bool SemiconductorProblem::setup(QString* error)
{
    if (!a_node) {
        *error = tr("there is no physics %1").arg(field().tag);
        return false;
    }
    Field& f = field();
    const Model& model = a_s.model();
    const Topology& topo = a_s.topology();
    const Mesh& mesh = a_s.mesh();
    const Space& sp = *f.space;
    const Scope& constants = a_s.parameters().scope;
    QString why;
    a_n = sp.size();
    const QVector<int> domains = resolveSelection(topo, model, Level::Domain, a_node->selection(), &why);
    if (!why.isEmpty()) problem(tr("%1: %2").arg(a_node->name(), why));
    f.domains.assign(topo.domains.size(), 0);
    for (int d : domains) f.domains[std::size_t(d)] = 1;
    if (domains.isEmpty()) problem(tr("%1 is on no domain").arg(a_node->name()));
    if (sp.order() != 1) problem(tr("%1: its elements are linear (Scharfetter and Gummel's currents)").arg(a_node->name()));
    {
        const std::optional<double> t = evaluateConstant(a_node->text(QStringLiteral("T")), constants, Dim::temperature(), &why);
        if (!t || !(*t > 0)) problem(tr("%1: its temperature: %2").arg(a_node->name(), t ? tr("it must be more than 0") : why));
        else a_T = *t;
    }
    a_Vt = KB * a_T / Q;
    {
        const QString tl = a_node->text(QStringLiteral("TL")).trimmed();
        a_TL = tl.isEmpty() ? Expression() : compile(tl, *a_node, QStringLiteral("TL"));
    }
    a_noise = a_node->flag(QStringLiteral("noise"));
    {
        a_Bz = 0;
        const QString bz = a_node->text(QStringLiteral("Bz")).trimmed();
        if (!bz.isEmpty()) {
            const std::optional<double> b = evaluateConstant(bz, constants, Dim::of(0, 1, -2, -1), &why);
            if (!b) problem(tr("%1: its magnetic flux density Bz: %2").arg(a_node->name(), why));
            else a_Bz = *b;
        }
        if (a_Bz != 0 && a_noise) problem(tr("%1: a magnetic field and noise are not solved together").arg(a_node->name()));
    }
    // Its materials' and features' texts read T, its temperature (the
    // library's gaps, densities of states, mobilities... are of it).
    const MaterialScope atT(constants, a_T);
    auto number = [&](const QString& text, const Dim& dim, const QString& what) {
        const std::optional<double> v = evaluateConstant(text, atT, dim, &why);
        if (!v) {
            problem(tr("%1: %2").arg(what, why));
            return NaN;
        }
        return *v;
    };
    const std::vector<const Node*> material = materials();
    a_media.assign(topo.domains.size(), Medium());
    std::vector<const Node*> modelOf(topo.domains.size(), nullptr);
    std::vector<const Node*> srhOf(topo.domains.size(), nullptr);
    std::vector<const Node*> augerOf(topo.domains.size(), nullptr), radiativeOf(topo.domains.size(), nullptr);
    std::vector<const Node*> impactOf(topo.domains.size(), nullptr);
    std::vector<const Node*> btbtOf(topo.domains.size(), nullptr);
    const Dim conc = Dim::of(-3), mobility = Dim::of(0, -1, 2, 1), velocity = Dim::of(1, 0, -1);
    struct DopingDef {
        const Node* node;
        QVector<int> domains;
    };
    std::vector<DopingDef> dopings;
    std::vector<DopingDef> stimulatedDefs;
    std::vector<const Node*> boundaryFeatures;
    std::vector<char> quantumOn(topo.domains.size(), 0);
    a_optical.clear();
    a_quantumN = a_quantumP = Expression();
    a_quantumElectrons = a_quantumHoles = false;
    for (const Node& feature : a_node->children) {
        if (!feature.enabled) continue;
        const NodeKind* k = feature.kind();
        if (!k) continue;
        const QVector<int> picked = resolveSelection(topo, model, k->selection, feature.selection(), &why);
        if (!why.isEmpty()) problem(tr("%1: %2").arg(feature.name(), why));
        const QString t = feature.type;
        if (t == QLatin1String("semiconductor/model")) for (int d : picked) modelOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/srh")) for (int d : picked) srhOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/auger")) for (int d : picked) augerOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/radiative")) for (int d : picked) radiativeOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/impact")) for (int d : picked) impactOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/btbt")) for (int d : picked) btbtOf[std::size_t(d)] = &feature;
        else if (t == QLatin1String("semiconductor/stimulated")) stimulatedDefs.push_back({&feature, picked});
        else if (t == QLatin1String("semiconductor/laser")) {
            if (a_laser) {
                problem(tr("%1: a semiconductor lases in one mode - %2 is its lasing already").arg(feature.name(), a_laser->name));
                continue;
            }
            a_laser = std::make_unique<Lasing>();
            Lasing& L = *a_laser;
            L.name = feature.name();
            L.mode = feature.text(QStringLiteral("mode")).trimmed();
            if (L.mode.isEmpty()) L.mode = QStringLiteral("emr");
            L.on.assign(topo.domains.size(), 0);
            for (int d : picked) L.on[std::size_t(d)] = 1;
            if (picked.isEmpty()) problem(tr("%1 is on no domain: its gain region's").arg(feature.name()));
            const QString gain = feature.text(QStringLiteral("gain"));
            L.gain = gain == QLatin1String("linear") ? 1 : gain == QLatin1String("quantum") ? 2 : 0;
            if (L.gain != 2) L.Ntr = number(feature.text(QStringLiteral("Ntr")), Dim::of(-3), feature.name());
            if (L.gain == 2) {
                GainRequest& rq = L.request;
                rq.wells = feature.text(QStringLiteral("wells")).trimmed();
                if (rq.wells.isEmpty()) rq.wells = QStringLiteral("qw");
                rq.spectrum = feature.text(QStringLiteral("spectrum")).trimmed();
                rq.Nmin = number(feature.text(QStringLiteral("Nmin")), Dim::of(-3), feature.name());
                rq.Nmax = number(feature.text(QStringLiteral("Nmax")), Dim::of(-3), feature.name());
                rq.points = feature.integer(QStringLiteral("points"));
                rq.tm = feature.text(QStringLiteral("polarization")) == QLatin1String("tm");
                if (!(rq.Nmin > 0) || !(rq.Nmax > rq.Nmin) || rq.points < 3 || rq.points > 200)
                    problem(tr("%1: its densities from Nmin more than 0 to Nmax above it, at 3 to 200 points").arg(feature.name()));
            } else if (L.gain == 0) {
                L.g0 = number(feature.text(QStringLiteral("g0")), Dim::of(-1), feature.name());
                L.Ns = number(feature.text(QStringLiteral("Ns")), Dim::of(-3), feature.name());
                if (!(L.g0 > 0) || !(L.Ntr + L.Ns > 0) || !(L.Ns >= 0))
                    problem(tr("%1: its gain coefficient g0 more than 0, its shift Ns 0 or more, its transparency more than 0").arg(feature.name()));
            } else {
                L.a = number(feature.text(QStringLiteral("a")), Dim::of(2), feature.name());
                if (!(L.a > 0) || !(L.Ntr >= 0)) problem(tr("%1: its differential gain a more than 0, its transparency 0 or more").arg(feature.name()));
            }
            const QString carrier = feature.text(QStringLiteral("carrier"));
            L.carrier = carrier == QLatin1String("electrons") ? 1 : carrier == QLatin1String("holes") ? 2 : 0;
            L.beta = number(feature.text(QStringLiteral("beta")), Dim(), feature.name());
            L.eta = number(feature.text(QStringLiteral("eta")), Dim(), feature.name());
            // (Its group index given: 1 or more - 0 was taken as none given:
            // the update check of 110e8b9.)
            const bool ngGiven = !feature.text(QStringLiteral("ng")).trimmed().isEmpty();
            if (ngGiven) L.ng = number(feature.text(QStringLiteral("ng")), Dim(), feature.name());
            if (!(L.beta >= 0 && L.beta <= 1) || !(L.eta >= 0 && L.eta <= 1) || (ngGiven && !(L.ng >= 1)))
                problem(tr("%1: its β and η from 0 to 1, its group index 1 or more (none: the media's)").arg(feature.name()));
        }
        else if (t == QLatin1String("semiconductor/quantum")) {
            for (int d : picked) quantumOn[std::size_t(d)] = 1;
            a_quantumElectrons = feature.flag(QStringLiteral("electrons"));
            a_quantumHoles = feature.flag(QStringLiteral("holes"));
            if (a_quantumElectrons) a_quantumN = compile(feature.text(QStringLiteral("nq")), feature, QStringLiteral("nq"));
            if (a_quantumHoles) a_quantumP = compile(feature.text(QStringLiteral("pq")), feature, QStringLiteral("pq"));
        }
        else if (t == QLatin1String("semiconductor/doping")) dopings.push_back({&feature, picked});
        else if (t == QLatin1String("semiconductor/generation")) {
            const Expression g = compile(feature.text(QStringLiteral("G")), feature, QStringLiteral("G"));
            // (Its pairs' energy as made: its photons', given, else the gap's.)
            double photon = NaN;
            const QString eph = feature.text(QStringLiteral("Eph")).trimmed();
            if (!eph.isEmpty()) {
                QString why;
                photon = energyOf(eph, constants, &why);
                if (!(photon > 0)) problem(tr("%1: its photon energy: %2").arg(feature.name(), why.isEmpty() ? tr("it must be more than 0") : why));
            }
            if (g.isValid())
                for (int d : picked) {
                    a_media[std::size_t(d)].generation.push_back(g);
                    a_media[std::size_t(d)].generationPhoton.push_back(photon);
                }
        } else if (t == QLatin1String("semiconductor/optical")) {
            OpticalSource o;
            o.mode = feature.text(QStringLiteral("mode")).trimmed();
            if (o.mode.isEmpty()) o.mode = QStringLiteral("emw");
            if (a_s.fieldIndex(o.mode) < 0 || a_s.fields()[std::size_t(a_s.fieldIndex(o.mode))].type != QLatin1String("modes")) {
                problem(tr("%1: there is no mode analysis %2 (Electromagnetic Waves, Mode Analysis) to take its light from").arg(feature.name(), o.mode));
                continue;
            }
            // (What it reads noted: the mode's field.)
            o.absorbed = Expression::compile(o.mode + QStringLiteral(".Qgap"), a_s.pointScope());
            for (int slot : o.absorbed.variables())
                if (a_s.definitions()[std::size_t(slot)].field >= 0) a_reads.insert(a_s.definitions()[std::size_t(slot)].field);
            o.number = std::max(1, feature.integer(QStringLiteral("number")));
            o.power = compile(feature.text(QStringLiteral("P0")), feature, QStringLiteral("P0"), false);
            if (o.power.isValid() && o.power.isConstant()) o.P0 = o.power.constant();
            o.eta = number(feature.text(QStringLiteral("eta")), Dim(), feature.name());
            o.RL = number(feature.text(QStringLiteral("RL")), Dim::of(2, 1, -3, -2), feature.name());
            o.slices = feature.integer(QStringLiteral("slices"));
            if (o.slices < 1 || o.slices > 100) problem(tr("%1: its slices along the length from 1 to 100").arg(feature.name()));
            a_slices = std::max(a_slices, std::clamp(o.slices, 1, 100));
            if (!(o.P0 >= 0) || !(o.eta >= 0 && o.eta <= 1)) problem(tr("%1: its power at least 0, its efficiency between 0 and 1").arg(feature.name()));
            const int index = int(a_optical.size());
            a_optical.push_back(o);
            for (int d : picked) a_media[std::size_t(d)].optical.push_back(index);
        } else if (k->selection == Level::Boundary) boundaryFeatures.push_back(&feature);
    }
    // Each domain's medium.
    bool referenced = false;
    // A feature's parameter on domain d: as given on the feature (its source
    // "user", or set there), else its material's (the library's model set:
    // GaAs's own Caughey and Thomas, Auger...), else the feature's default -
    // silicon's: said, by material, where the material is no silicon (its gap
    // at 300 K not silicon's 1.12 eV, nor its entry Silicon).
    std::map<QString, std::pair<QStringList, QStringList>> silicons;   // material: its domains, the models given silicon's
    const MaterialScope at300(constants, 300);
    auto siliconLike = [&at300](const Node* mat) {
        if (!mat) return true;
        if (mat->text(QStringLiteral("library")).compare(QLatin1String("Silicon"), Qt::CaseInsensitive) == 0) return true;
        const std::optional<double> eg = evaluateConstant(materialProperty(*mat, QStringLiteral("Eg"), &at300), at300, Dim());
        if (!eg) return true;
        const double eV = Expression::compile(materialProperty(*mat, QStringLiteral("Eg"), &at300), at300).dim() == Dim::of(2, 1, -2) ? *eg / Q : *eg;
        return std::abs(eV - 1.12) <= 0.03;
    };
    auto param = [&material, &atT, this, &siliconLike, &silicons, &topo](const Node* f, const QString& key, int d, const QString& model) -> QString {
        if (f->text(QStringLiteral("source")) == QLatin1String("user") || f->props.contains(key)) return f->text(key);
        const Node* mat = material[std::size_t(d)];
        const QString mk = materialKeyOf(f->type, key);
        if (!mk.isEmpty() && mat) {
            const QString t = materialProperty(*mat, mk, &atT, a_T);
            if (!t.trimmed().isEmpty()) return t;
        }
        if (!model.isEmpty() && !siliconLike(mat)) {
            auto& [where, models] = silicons[mat->name()];
            const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
            if (!where.contains(object)) where << object;
            if (!models.contains(model)) models << model;
        }
        return f->text(key);
    };
    // A mobility's expression's variables (its parameters' and constants'
    // names below them).
    Scope exprScope(&constants);
    a_slotN = exprScope.addVariable(QStringLiteral("N"), conc);
    a_slotNd = exprScope.addVariable(QStringLiteral("Nd"), conc);
    a_slotNa = exprScope.addVariable(QStringLiteral("Na"), conc);
    a_slotT = exprScope.addVariable(QStringLiteral("T"), Dim::temperature());
    a_slotn = exprScope.addVariable(QStringLiteral("n"), conc);
    a_slotp = exprScope.addVariable(QStringLiteral("p"), conc);
    a_slotEpar = exprScope.addVariable(QStringLiteral("Epar"), Dim::of(1, 1, -3, -1));
    a_slotEperp = exprScope.addVariable(QStringLiteral("Eperp"), Dim::of(1, 1, -3, -1));
    a_slotD = exprScope.addVariable(QStringLiteral("dsurf"), Dim::length());
    a_exprSlots = exprScope.slotCount();
    a_varying = false;
    for (int d : domains) {
        Medium& m = a_media[std::size_t(d)];
        const Node* mf = modelOf[std::size_t(d)];
        const Node* mat = material[std::size_t(d)];
        const QString object = topo.objectNames.value(topo.domains[std::size_t(d)].owner);
        const bool user = mf && mf->text(QStringLiteral("source")) == QLatin1String("user");
        auto text = [&](const QString& key) {
            if (user) return mf->text(key);
            return mat ? materialProperty(*mat, key, &atT, a_T) : QString();
        };
        const QString where = tr("domain %1 (%2)").arg(d + 1).arg(object);
        if (!user && !mat) {
            problem(tr("%1 has no material").arg(where));
            continue;
        }
        const QString eps = text(QStringLiteral("epsilonr"));
        if (eps.trimmed().isEmpty()) problem(tr("%1: its material has no relative permittivity").arg(where));
        else m.epsr = number(eps, Dim(), where);
        m.semiconductor = !text(QStringLiteral("Eg")).trimmed().isEmpty();
        if (!m.semiconductor) continue;
        m.Eg = energyOf(text(QStringLiteral("Eg")), atT, &why);
        if (!std::isfinite(m.Eg)) problem(tr("%1: its band gap: %2").arg(where, why));
        const QString chi = text(QStringLiteral("chi"));
        m.chi = chi.trimmed().isEmpty() ? 4.05 * Q : energyOf(chi, atT, &why);
        for (const auto& [key, member, dim] : {std::tuple{QStringLiteral("Nc"), &Medium::Nc, conc}, std::tuple{QStringLiteral("Nv"), &Medium::Nv, conc},
                                               std::tuple{QStringLiteral("mun"), &Medium::mun, mobility},
                                               std::tuple{QStringLiteral("mup"), &Medium::mup, mobility}}) {
            const QString v = text(key);
            if (v.trimmed().isEmpty()) problem(tr("%1: its material has no %2").arg(where, key));
            else m.*member = number(v, dim, where);
        }
        if (!(m.Nc > 0) || !(m.Nv > 0) || !(m.Eg > 0)) {
            problem(tr("%1: its band gap and effective densities of states must be more than 0").arg(where));
            continue;
        }
        m.ni = std::sqrt(m.Nc * m.Nv) * std::exp(-m.Eg / (2 * KB * a_T));
        a_gapOverKT = std::max(a_gapOverKT, m.Eg / (2 * KB * a_T));
        m.dEc = m.Eg / 2 + KB * a_T / 2 * std::log(m.Nc / m.Nv);
        if (a_TL.isValid()) {
            // (Under a lattice temperature: its ni and gap through the
            // table's temperatures, its own texts at each.)
            for (int k = 0; k < ThermalPoints; ++k) {
                const double Tk = ThermalFrom + ThermalStep * k;
                const MaterialScope at(constants, Tk);
                auto textAt = [&](const QString& key) { return user ? mf->text(key) : materialProperty(*mat, key, &at, Tk); };
                QString w;
                const double eg = energyOf(textAt(QStringLiteral("Eg")), at, &w);
                const std::optional<double> nc = evaluateConstant(textAt(QStringLiteral("Nc")), at, conc, &w);
                const std::optional<double> nv = evaluateConstant(textAt(QStringLiteral("Nv")), at, conc, &w);
                if (!(eg > 0) || !nc || !nv || !(*nc > 0) || !(*nv > 0)) {
                    problem(tr("%1: its band gap and densities of states at %2 K, for its lattice temperature: %3").arg(where).arg(Tk).arg(w));
                    m.lnNiAt.clear();
                    m.gapAt.clear();
                    break;
                }
                m.gapAt.push_back(eg);
                m.lnNiAt.push_back(0.5 * std::log(*nc * *nv) - eg / (2 * KB * Tk));
            }
        }
        if (!referenced) {
            a_refDEc = m.dEc;
            a_refChi = m.chi;
            a_refNi = m.ni;
            referenced = true;
        }
        // (Its model and key named: "it is inf, not a finite number" said
        // which domain alone.)
        auto value = [&](const Node* f, const QString& key, const Dim& dim, const QString& model) {
            return number(param(f, key, d, model), dim, tr("%1: %2, %3").arg(where, model, key));
        };
        if (mf && mf->text(QStringLiteral("mobility")) == QLatin1String("masetti")) {
            m.masetti = true;
            const char* keys[2][8] = {{"m1n", "m2n", "mu1n", "pcn", "crn", "csn", "man", "mbn"}, {"m1p", "m2p", "mu1p", "pcp", "crp", "csp", "map", "mbp"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 8; ++k)
                    m.ms[c][k] = value(mf, QString::fromLatin1(keys[c][k]), k < 3 ? mobility : k < 6 ? conc : Dim(), tr("Masetti's mobility"));
            // (Its µmax: empty, the lattice's - the Lumerical Masetti model's.)
            for (int c = 0; c < 2; ++c) {
                const QString key = c == 0 ? QStringLiteral("mmaxn") : QStringLiteral("mmaxp");
                const QString text = param(mf, key, d, tr("Masetti's mobility"));
                m.msMax[c] = text.trimmed().isEmpty() ? (c == 0 ? m.mun : m.mup)
                                                       : number(text, mobility, tr("%1: %2, %3").arg(where, tr("Masetti's mobility"), key));
                // (Not a number: said already.)
                if (!std::isnan(m.msMax[c]) && !(m.msMax[c] > 0)) problem(tr("%1: Masetti's µmax must be more than 0").arg(where));
            }
            for (int c = 0; c < 2; ++c)
                if (!(m.ms[c][4] > 0) || !(m.ms[c][5] > 0) || !std::isfinite(m.ms[c][6]) || !std::isfinite(m.ms[c][7]))
                    problem(tr("%1: Masetti's Cr and Cs must be more than 0").arg(where));
        }
        if (mf && mf->text(QStringLiteral("mobility")) == QLatin1String("doping")) {
            m.dopingMobility = true;
            const QString ct = tr("Caughey and Thomas's mobility");
            m.munMin = value(mf, QStringLiteral("munmin"), mobility, ct);
            m.nRefN = value(mf, QStringLiteral("nrefn"), conc, ct);
            m.alphaN = value(mf, QStringLiteral("alphan"), Dim(), ct);
            m.mupMin = value(mf, QStringLiteral("mupmin"), mobility, ct);
            m.nRefP = value(mf, QStringLiteral("nrefp"), conc, ct);
            m.alphaP = value(mf, QStringLiteral("alphap"), Dim(), ct);
        }
        const QString lowField = mf ? mf->text(QStringLiteral("mobility")) : QString();
        if (lowField == QLatin1String("arora")) {
            m.lowField = Medium::LowField::Arora;
            const QString arora = tr("Arora's mobility");
            const char* keys[2][8] = {{"arminn", "aran", "ardn", "arbn", "arnrefn", "arcn", "aralphan", "ardexpn"},
                                      {"arminp", "arap", "ardp", "arbp", "arnrefp", "arcp", "aralphap", "ardexpp"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 8; ++k)
                    m.ar[c][k] = value(mf, QString::fromLatin1(keys[c][k]), k == 0 || k == 2 ? mobility : k == 4 ? conc : Dim(), arora);
            for (int c = 0; c < 2; ++c) {
                bool finite = true;
                for (double v : m.ar[c]) finite = finite && std::isfinite(v);
                if (!finite || !(m.ar[c][0] >= 0) || !(m.ar[c][2] > 0) || !(m.ar[c][4] > 0) || !(m.ar[c][6] > 0))
                    problem(tr("%1: Arora's µmin at least 0, µd, Nref and α more than 0").arg(where));
            }
        } else if (lowField == QLatin1String("dorkel")) {
            m.lowField = Medium::LowField::Dorkel;
            const QString dorkel = tr("Dorkel and Leturcq's mobility");
            const char* keys[2][4] = {{"dlmun", "dlgamman", "dlan", "dlbn"}, {"dlmup", "dlgammap", "dlap", "dlbp"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 4; ++k) m.dl[c][k] = value(mf, QString::fromLatin1(keys[c][k]), k == 0 ? mobility : Dim(), dorkel);
            for (int c = 0; c < 2; ++c)
                if (!(m.dl[c][0] > 0) || !std::isfinite(m.dl[c][1]) || !(m.dl[c][2] > 0) || !(m.dl[c][3] > 0))
                    problem(tr("%1: Dorkel and Leturcq's µL, A and B must be more than 0").arg(where));
        } else if (lowField == QLatin1String("klaassen")) {
            m.lowField = Medium::LowField::Klaassen;
            const QString klaassen = tr("Klaassen's mobility");
            const char* keys[2][5] = {{"klmaxn", "klminn", "klthetan", "klnrefn", "klalphan"}, {"klmaxp", "klminp", "klthetap", "klnrefp", "klalphap"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 5; ++k)
                    m.kl[c][k] = value(mf, QString::fromLatin1(keys[c][k]), k < 2 ? mobility : k == 3 ? conc : Dim(), klaassen);
            m.klNrefD = value(mf, QStringLiteral("klnrefd"), conc, klaassen);
            m.klNrefA = value(mf, QStringLiteral("klnrefa"), conc, klaassen);
            m.klCD = value(mf, QStringLiteral("klcd"), Dim(), klaassen);
            m.klCA = value(mf, QStringLiteral("klca"), Dim(), klaassen);
            m.klMe = value(mf, QStringLiteral("klme"), Dim(), klaassen);
            m.klMh = value(mf, QStringLiteral("klmh"), Dim(), klaassen);
            for (int c = 0; c < 2; ++c)
                if (!(m.kl[c][0] > m.kl[c][1]) || !(m.kl[c][1] > 0) || !std::isfinite(m.kl[c][2]) || !(m.kl[c][3] > 0) || !(m.kl[c][4] > 0))
                    problem(tr("%1: Klaassen's µmax more than µmin, µmin, Nref and α more than 0").arg(where));
            if (!(m.klNrefD > 0) || !(m.klNrefA > 0) || !(m.klCD >= 0) || !(m.klCA >= 0) || !(m.klMe > 0) || !(m.klMh > 0))
                problem(tr("%1: Klaassen's clustering's Nref more than 0, its c at least 0, its masses more than 0").arg(where));
        } else if (lowField == QLatin1String("yamaguchi")) {
            m.lowField = Medium::LowField::Yamaguchi;
            const QString yamaguchi = tr("Yamaguchi's mobility");
            const Dim perField = Dim::of(-1, -1, 3, 1);   // m/V
            const char* keys[2][7] = {{"ymuln", "ysn", "ynrefn", "yasn", "yuln", "yvsn", "ygn"}, {"ymulp", "ysp", "ynrefp", "yasp", "yulp", "yvsp", "ygp"}};
            const Dim dims[7] = {mobility, Dim(), conc, perField, velocity, velocity, Dim()};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 7; ++k) m.ya[c][k] = value(mf, QString::fromLatin1(keys[c][k]), dims[k], yamaguchi);
            m.lcrit = value(mf, QStringLiteral("ylcrit"), Dim::length(), yamaguchi);
            for (int c = 0; c < 2; ++c) {
                bool positive = true;
                for (int k = 0; k < 7; ++k) positive = positive && (k == 3 ? m.ya[c][k] >= 0 : m.ya[c][k] > 0);
                if (!positive) problem(tr("%1: Yamaguchi's µL, S, Nref, uL, vs and G must be more than 0, as at least 0").arg(where));
            }
            if (!(m.lcrit > 0)) problem(tr("%1: Yamaguchi's lcrit must be more than 0").arg(where));
            a_lombardi = true;   // (the field across the nearest interface: its side's normal and distance)
            // (A fit at 300 K, of no temperature: said where it is not; a
            // lattice temperature's in latticeTemperature.)
            if (!a_TL.isValid() && std::abs(a_T - 300) > 0.05) {
                const QString w = tr("Warning: %1, %2: Yamaguchi's mobility is of no temperature - its 300 K fit's at %3 K")
                                      .arg(a_node->name(), where, formatNumber(a_T, 4));
                if (!a_s.log.contains(w)) a_s.log << w;
            }
        } else if (lowField == QLatin1String("expression")) {
            m.lowField = Medium::LowField::Expression;
            for (int c = 0; c < 2; ++c) {
                const QString key = c == 0 ? QStringLiteral("muexpn") : QStringLiteral("muexpp");
                const QString text = mf->text(key);
                m.muExpr[c] = Expression::compile(text.trimmed().isEmpty() ? QStringLiteral("0") : text, exprScope);
                if (!m.muExpr[c].isValid()) {
                    problem(tr("%1: its %2 mobility's expression: %3").arg(where, c == 0 ? tr("electrons'") : tr("holes'"), m.muExpr[c].error()));
                    continue;
                }
                if (!m.muExpr[c].dim().isNone() && !(m.muExpr[c].dim() == mobility))
                    problem(tr("%1: its %2 mobility's expression is in %3, not m²/(V s)")
                                .arg(where, c == 0 ? tr("electrons'") : tr("holes'"), dimName(m.muExpr[c].dim())));
                for (int slot : {a_slotn, a_slotp, a_slotEpar, a_slotEperp, a_slotD})
                    m.exprState = m.exprState || m.muExpr[c].uses(slot);
                m.exprAcross = m.exprAcross || m.muExpr[c].uses(a_slotEperp) || m.muExpr[c].uses(a_slotD);
            }
            if (m.exprState) m.lcrit = 1e-8;
            if (m.exprAcross) a_lombardi = true;
        }
        if (mf) {
            // (Said only where a lattice temperature TL scales them.)
            const QString zetas = a_TL.isValid() ? tr("the mobilities' temperature exponents") : QString();
            m.zetan = value(mf, QStringLiteral("zetan"), Dim(), zetas);
            m.zetap = value(mf, QStringLiteral("zetap"), Dim(), zetas);
            if (!std::isfinite(m.zetan) || !std::isfinite(m.zetap)) m.zetan = m.zetap = 0;
        }
        if (mf && mf->flag(QStringLiteral("saturation"))) {
            m.saturation = a_saturation = true;
            const QString sat = tr("saturation");
            m.vsatn = value(mf, QStringLiteral("vsatn"), velocity, sat);
            m.vsatp = value(mf, QStringLiteral("vsatp"), velocity, sat);
            m.betan = value(mf, QStringLiteral("betan"), Dim(), sat);
            m.betap = value(mf, QStringLiteral("betap"), Dim(), sat);
            // (Quay's A: the velocities given at 300 K, at T and TL of it.)
            m.vsatAn = value(mf, QStringLiteral("Avn"), Dim(), sat);
            m.vsatAp = value(mf, QStringLiteral("Avp"), Dim(), sat);
            m.vsatExp = mf->text(QStringLiteral("vsatmodel")) == QLatin1String("exponential");
            if (m.vsatExp) m.vsatn = m.vsatp = expVsat(a_T);
            else if (!(m.vsatAn >= 0 && m.vsatAn < 1) || !(m.vsatAp >= 0 && m.vsatAp < 1))
                problem(tr("%1: its saturation velocities' temperature coefficients A from 0 to less than 1").arg(where));
            else {
                m.vsatn *= quayVsat(m.vsatAn, a_T);
                m.vsatp *= quayVsat(m.vsatAp, a_T);
            }
            if (!(m.betan > 0) || !(m.betap > 0)) problem(tr("%1: its saturation exponents β must be more than 0").arg(where));
            m.wholeField = mf->text(QStringLiteral("driving")) == QLatin1String("field");
            a_alongCurrent = a_alongCurrent || !m.wholeField;
            // (Hot carriers: driven by their temperatures, an energy balance's.)
            if (mf->text(QStringLiteral("driving")) == QLatin1String("energy")) {
                const Dim time = Dim::of(0, 0, 1);
                m.energyN = m.energyP = a_energy = true;
                m.tauWn = number(mf->text(QStringLiteral("tauwn")), time, where);
                m.tauWp = number(mf->text(QStringLiteral("tauwp")), time, where);
                if (!(m.tauWn > 0) || !(m.tauWp > 0)) problem(tr("%1: its energy relaxation times must be more than 0").arg(where));
            }
            if (mf->text(QStringLiteral("velocity")) == QLatin1String("transferred")) {
                m.transferred = true;
                m.E0n = number(mf->text(QStringLiteral("E0n")), Dim::of(1, 1, -3, -1), where);
                if (!(m.E0n > 0)) problem(tr("%1: its transferred electrons' field E0 must be more than 0").arg(where));
            }
        }
        if (mf && mf->flag(QStringLiteral("surface")) && mf->text(QStringLiteral("surfmodel")) == QLatin1String("shirahata")) {
            m.surface = m.shirahata = a_lombardi = true;
            const QString shirahata = tr("Shirahata's surface scattering");
            const Dim field = Dim::of(1, 1, -3, -1);
            const char* keys[2][6] = {{"shmu0n", "shthetan", "she1n", "she2n", "shp1n", "shp2n"}, {"shmu0p", "shthetap", "she1p", "she2p", "shp1p", "shp2p"}};
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 6; ++k)
                    m.sh[c][k] = value(mf, QString::fromLatin1(keys[c][k]), k == 0 ? mobility : k == 2 || k == 3 ? field : Dim(), shirahata);
            m.lcrit = value(mf, QStringLiteral("lcrit"), Dim::length(), shirahata);
            for (int c = 0; c < 2; ++c)
                if (!(m.sh[c][0] > 0) || !std::isfinite(m.sh[c][1]) || !(m.sh[c][2] > 0) || !(m.sh[c][3] > 0) || !(m.sh[c][4] > 0) || !(m.sh[c][5] > 0))
                    problem(tr("%1: Shirahata's µ0, E1, E2, P1 and P2 must be more than 0").arg(where));
            if (!(m.lcrit > 0)) problem(tr("%1: its surface scattering's lcrit must be more than 0").arg(where));
        } else if (mf && mf->flag(QStringLiteral("surface"))) {
            m.surface = a_lombardi = true;
            const Dim perSecond = Dim::voltage() / Dim::of(0, 0, 1);
            const QString lombardi = tr("Lombardi's surface scattering");
            m.sBn = value(mf, QStringLiteral("Bn"), velocity, lombardi);
            m.sCn = value(mf, QStringLiteral("Cn"), Dim(), lombardi);
            m.sLn = value(mf, QStringLiteral("lambdan"), Dim(), lombardi);
            m.sDn = value(mf, QStringLiteral("deltan"), perSecond, lombardi);
            m.sBp = value(mf, QStringLiteral("Bp"), velocity, lombardi);
            m.sCp = value(mf, QStringLiteral("Cp"), Dim(), lombardi);
            m.sLp = value(mf, QStringLiteral("lambdap"), Dim(), lombardi);
            m.sDp = value(mf, QStringLiteral("deltap"), perSecond, lombardi);
            m.lcrit = value(mf, QStringLiteral("lcrit"), Dim::length(), lombardi);
            if (!(m.sBn > 0) || !(m.sCn > 0) || !(m.sDn > 0) || !(m.sBp > 0) || !(m.sCp > 0) || !(m.sDp > 0) || !(m.lcrit > 0))
                problem(tr("%1: its surface scattering's B, C, δ and lcrit must be more than 0").arg(where));
        }
        if (mf && mf->flag(QStringLiteral("bgn"))) {
            m.bgn = true;
            const QString bgn = tr("band gap narrowing");
            m.bgnE = energyOf(param(mf, QStringLiteral("Ebgn"), d, bgn), atT, &why);
            if (!std::isfinite(m.bgnE)) problem(tr("%1: its band gap narrowing's energy: %2").arg(where, why));
            m.bgnN = value(mf, QStringLiteral("Nbgn"), conc, bgn);
            m.bgnC = value(mf, QStringLiteral("Cbgn"), Dim(), bgn);
            if (!(m.bgnN > 0) || !(m.bgnC >= 0)) problem(tr("%1: its band gap narrowing's Nref must be more than 0, C no less").arg(where));
        }
        if (mf && mf->flag(QStringLiteral("fermi"))) m.fermi = a_fermi = true;
        if (a_Bz != 0) {
            const QString hall = tr("Hall factors");
            m.rHn = mf ? value(mf, QStringLiteral("rhalln"), Dim(), hall) : 1.0;
            m.rHp = mf ? value(mf, QStringLiteral("rhallp"), Dim(), hall) : 1.0;
            if (!(m.rHn >= 0) || !(m.rHp >= 0)) problem(tr("%1: its Hall factors must be at least 0").arg(where));
        }
        if (mf && mf->flag(QStringLiteral("incomplete"))) {
            m.incomplete = a_incomplete = true;
            const QString levels = tr("its dopants' levels");
            const double Ed = energyOf(param(mf, QStringLiteral("Ed"), d, levels), atT, &why);
            const double Ea = energyOf(param(mf, QStringLiteral("Ea"), d, levels), atT, &why);
            m.gd = value(mf, QStringLiteral("gd"), Dim(), levels);
            m.ga = value(mf, QStringLiteral("ga"), Dim(), levels);
            if (!std::isfinite(Ed) || !std::isfinite(Ea) || !(m.gd > 0) || !(m.ga > 0))
                problem(tr("%1: its dopants' levels and degeneracies (more than 0)").arg(where));
            m.n1d = m.Nc * std::exp(-Ed / (KB * a_T));
            m.p1a = m.Nv * std::exp(-Ea / (KB * a_T));
        }
        if (const Node* r = srhOf[std::size_t(d)]) {
            m.srh = true;
            const bool own = r->text(QStringLiteral("source")) == QLatin1String("user");
            // Each lifetime the feature's where it sets it, as its other
            // parameters are (param()), else its material's. (Set on the
            // feature without "source": "user" they were the material's -
            // the VCSEL laser example's 5 ns taken as 1 ns: the update check
            // of 110e8b9.)
            auto lifetimeFrom = [&](const QString& key) -> const Node* {
                if (own || r->props.contains(key)) return r;
                return mat && !materialProperty(*mat, key, &atT, a_T).trimmed().isEmpty() ? mat : nullptr;
            };
            const Node *fromN = lifetimeFrom(QStringLiteral("taun")), *fromP = lifetimeFrom(QStringLiteral("taup"));
            auto rt = [&](const Node* from, const QString& key) { return from == r ? r->text(key) : materialProperty(*from, key, &atT, a_T); };
            if (!fromN || !fromP) problem(tr("%1: its material has no carrier lifetimes (taun, taup)").arg(where));
            else {
                // (Of x and y: each node's; a constant as it was. T the
                // physics' temperature.)
                const QString tn = rt(fromN, QStringLiteral("taun")), tp = rt(fromP, QStringLiteral("taup"));
                const MaterialScope pointAtT(a_s.pointScope(), a_T);
                m.taunOf = compile(tn, *fromN, QStringLiteral("taun"), false, true, &pointAtT);
                m.taupOf = compile(tp, *fromP, QStringLiteral("taup"), false, true, &pointAtT);
                m.taun = m.taunOf.isConstant() ? m.taunOf.constant() : 1.0;
                m.taup = m.taupOf.isConstant() ? m.taupOf.constant() : 1.0;
                if (m.taunOf.isConstant() && !(m.taun > 0)) problem(tr("%1: its electron lifetime must be more than 0").arg(where));
                if (m.taupOf.isConstant() && !(m.taup > 0)) problem(tr("%1: its hole lifetime must be more than 0").arg(where));
            }
            m.Et = energyOf(param(r, QStringLiteral("Et"), d, QString()), atT, &why);
            if (!std::isfinite(m.Et)) m.Et = 0;
            if (r->text(QStringLiteral("dependence")) == QLatin1String("doping")) {
                m.fossum = true;
                const QString fossum = tr("Fossum's doping dependence");
                m.N0n = value(r, QStringLiteral("N0n"), conc, fossum);
                m.N0p = value(r, QStringLiteral("N0p"), conc, fossum);
                m.fA = value(r, QStringLiteral("fA"), Dim(), fossum);
                m.fB = value(r, QStringLiteral("fB"), Dim(), fossum);
                m.fC = value(r, QStringLiteral("fC"), Dim(), fossum);
                m.fE = value(r, QStringLiteral("fE"), Dim(), fossum);
                if (!(m.N0n > 0) || !(m.N0p > 0) || !(m.fA >= 0) || !(m.fB >= 0) || !(m.fC >= 0) || !(m.fA + m.fB + m.fC > 0) || !std::isfinite(m.fE))
                    problem(tr("%1: its doping dependence's N0 more than 0, A, B and C no less (not all 0)").arg(r->name()));
            }
            if (r->flag(QStringLiteral("tat"))) {
                // FΓ = √(24 m* m0 (kT)³)/(q ħ).
                const double kT = KB * a_T;
                const QString tat = tr("trap-assisted tunnelling's masses");
                const double mtn = value(r, QStringLiteral("mtn"), Dim(), tat), mtp = value(r, QStringLiteral("mtp"), Dim(), tat);
                if (!(mtn > 0) || !(mtp > 0)) problem(tr("%1: its tunnelling masses must be more than 0").arg(r->name()));
                else {
                    m.tat = a_tat = true;
                    m.FGn = std::sqrt(24 * mtn * Mass0 * kT * kT * kT) / (Q * HbarSI);
                    m.FGp = std::sqrt(24 * mtp * Mass0 * kT * kT * kT) / (Q * HbarSI);
                }
            }
        }
        if (const Node* a = augerOf[std::size_t(d)]) {
            m.Cn = value(a, QStringLiteral("Cn"), Dim::of(6, 0, -1), tr("Auger's coefficients"));
            m.Cp = value(a, QStringLiteral("Cp"), Dim::of(6, 0, -1), tr("Auger's coefficients"));
        }
        if (const Node* r = radiativeOf[std::size_t(d)]) m.B = value(r, QStringLiteral("B"), Dim::of(3, 0, -1), tr("the radiative coefficient"));
        if (const Node* bt = btbtOf[std::size_t(d)]) {
            // A in cm^-1/2 V^-5/2 s^-1 for γ 2.5 (Hurkx's, Klaassen's
            // silicon): as given, scaled to SI for E in V/m.
            m.tunnelling = a_tunnelling = true;
            m.btGeneration = bt->text(QStringLiteral("kind")) == QLatin1String("generation");
            const QString btbt = tr("band-to-band tunnelling");
            m.btGamma = number(param(bt, QStringLiteral("gamma"), d, btbt), Dim(), bt->name());
            const double Acm = number(param(bt, QStringLiteral("A"), d, btbt), Dim(), bt->name());
            m.btB = number(param(bt, QStringLiteral("B"), d, btbt), Dim::of(1, 1, -3, -1), bt->name());
            if (!(Acm > 0) || !(m.btB > 0) || !std::isfinite(m.btGamma)) problem(tr("%1: its A and B must be more than 0").arg(bt->name()));
            m.btA = Acm * 1e6 * std::pow(1e-2, m.btGamma);   // (1/cm³ s per (V/cm)^γ → 1/m³ s per (V/m)^γ)
        }
        if (const Node* ii = impactOf[std::size_t(d)]) {
            m.ionization = a_ionizationAny = true;
            m.ionizationCurrents = ii->flag(QStringLiteral("currents"));
            a_ionization = a_ionization || m.ionizationCurrents;
            m.ionFermi = ii->text(QStringLiteral("driving")) == QLatin1String("fermi");
            m.ionAlong = ii->text(QStringLiteral("driving")) == QLatin1String("current");
            a_ionFermi = a_ionFermi || (m.ionFermi && m.ionizationCurrents);
            const QString model = ii->text(QStringLiteral("model"));
            if (model == QLatin1String("silicon")) {
                // Silicon at 300 K (van Overstraeten and de Man, 1970).
                m.an = m.an2 = 7.03e7;
                m.bn = m.bn2 = 1.231e8;
                m.ap = 1.582e8;
                m.bp = 2.036e8;
                m.ap2 = 6.71e7;
                m.bp2 = 1.693e8;
                m.E0 = 4e7;
                m.ionBetaN = m.ionBetaP = 1;
                m.ionPhonon = 0.063 * Q;
            } else {
                // Chynoweth's, as given (user) or its material's (one range
                // where the material has one: Selberherr's).
                const Dim perMetre = Dim::of(-1), field = Dim::of(1, 1, -3, -1);
                const QString ionization = tr("impact ionization's coefficients");
                auto coefficient = [&](const char* key, const char* first, const Dim& dim) {
                    const QString k = QString::fromLatin1(key);
                    if (model == QLatin1String("user") || ii->props.contains(k)) return number(ii->text(k), dim, ii->name());
                    // (A second range's: the material's, else its first's.)
                    if (first && mat && materialProperty(*mat, materialKeyOf(ii->type, k), &atT, a_T).trimmed().isEmpty()
                        && !materialProperty(*mat, materialKeyOf(ii->type, QString::fromLatin1(first)), &atT, a_T).trimmed().isEmpty())
                        return number(param(ii, QString::fromLatin1(first), d, ionization), dim, ii->name());
                    return number(param(ii, k, d, ionization), dim, ii->name());
                };
                m.an = coefficient("an", nullptr, perMetre);
                m.bn = coefficient("bn", nullptr, field);
                m.an2 = coefficient("an2", "an", perMetre);
                m.bn2 = coefficient("bn2", "bn", field);
                m.ap = coefficient("ap", nullptr, perMetre);
                m.bp = coefficient("bp", nullptr, field);
                m.ap2 = coefficient("ap2", "ap", perMetre);
                m.bp2 = coefficient("bp2", "bp", field);
                m.ionBetaN = coefficient("betan", nullptr, Dim());
                m.ionBetaP = coefficient("betap", nullptr, Dim());
                // (A material of one range: its second the first's, whatever E0.)
                m.E0 = coefficient("E0", nullptr, field);
                // (Its phonon: the material's - none, nought: as given at any T.)
                const QString eph = model == QLatin1String("user") || ii->props.contains(QStringLiteral("Eph"))
                                        ? ii->text(QStringLiteral("Eph"))
                                        : (mat ? materialProperty(*mat, QStringLiteral("iiEph"), &atT, a_T) : QString());
                if (!eph.trimmed().isEmpty()) {
                    m.ionPhonon = energyOf(eph, atT, &why);
                    if (!(m.ionPhonon >= 0)) problem(tr("%1: its optical phonon's energy: %2").arg(ii->name(), why));
                }
            }
            // At the physics' T: its coefficients' γ(T) (at 300 K one).
            if (m.ionPhonon > 0) {
                const double g = phononGamma(m.ionPhonon, a_T);
                for (double* v : {&m.an, &m.bn, &m.an2, &m.bn2, &m.ap, &m.bp, &m.ap2, &m.bp2}) *v *= g;
            }
        }
    }
    // Stimulated recombination: its share, its threshold's rate.
    a_stimulated.clear();
    a_lastR.clear();
    for (const DopingDef& sd : stimulatedDefs) {
        if (sd.domains.isEmpty()) continue;
        const Node& f = *sd.node;
        const int d0 = sd.domains.front();
        Stimulated st;
        st.on.assign(topo.domains.size(), 0);
        for (int d : sd.domains) st.on[std::size_t(d)] = 1;
        st.c = number(param(&f, QStringLiteral("c"), d0, QString()), Dim(), f.name());
        const double Jth = number(param(&f, QStringLiteral("Jth"), d0, QString()), Dim::of(-2, 0, 0, 1), f.name());
        const double thickness = number(param(&f, QStringLiteral("t"), d0, QString()), Dim::length(), f.name());
        if (!(st.c >= 0 && st.c <= 1) || !(Jth >= 0) || !(thickness > 0))
            problem(tr("%1: its share c from 0 to 1, its threshold no less than 0, its thickness more than 0").arg(f.name()));
        else st.R0 = Jth / (Q * thickness);
        a_stimulated.push_back(st);
    }
    // (Said once for each material: what is silicon's on it.)
    for (const auto& [name, said] : silicons)
        a_s.log << tr("Warning: %1 (%2): its material gives no parameters for %3 - silicon's (the features' defaults) taken; give them "
                      "on the feature or the material")
                       .arg(name, said.first.join(QStringLiteral(", ")), said.second.join(QStringLiteral(", ")));
    if (!referenced && !domains.isEmpty()) problem(tr("%1: none of its domains is a semiconductor (a material with a band gap Eg)").arg(a_node->name()));
    // Elements and nodes.
    const int nt = sp.elements();
    a_elemSemi.assign(std::size_t(nt), 0);
    a_elemOn.assign(std::size_t(nt), 0);
    a_nodeDomain.assign(std::size_t(a_n), -1);
    std::vector<char> nodeOn(std::size_t(a_n), 0);
    a_box.assign(std::size_t(a_n), 0);
    a_K.assign(std::size_t(nt), {});
    // An edge by its nodes and its semiconductor's bands (a domain's: the
    // first domain's of the same). One between two domains of one material
    // is that material's - from a node of a heterojunction, the band step at
    // the node; taken as of two media it had none, a GaAs disk's electrons
    // along the boundary of the two AlGaAs domains beside and under it: a
    // toy laser's current the mesh's (the update check of 48d5fa8). One
    // along a heterojunction is two, each its own side's triangles', its
    // current there its carriers'.
    auto sameBands = [this](int d, int e) {
        const Medium &a = a_media[std::size_t(d)], &b = a_media[std::size_t(e)];
        auto near = [](double x, double y) { return std::abs(x - y) <= 1e-9 * std::max(std::abs(x), std::abs(y)); };
        return near(a.Eg, b.Eg) && near(a.chi + a.dEc, b.chi + b.dEc) && near(a.ni, b.ni);
    };
    // (Graded over an element, as it was: all of one class - an edge one,
    // its steps along it, a node's box its own carriers'.)
    const bool graded = a_node->text(QStringLiteral("heterojunctions")) == QLatin1String("element");
    std::vector<int> bandsOf(a_media.size(), -1);
    for (std::size_t d = 0; d < a_media.size(); ++d) {
        if (!a_media[d].semiconductor) continue;
        bandsOf[d] = int(d);
        for (std::size_t e = 0; e < d; ++e)
            if (a_media[e].semiconductor && (graded || sameBands(int(d), int(e)))) {
                bandsOf[d] = bandsOf[e];
                break;
            }
    }
    a_bandsOf = bandsOf;
    a_nodeTL.clear();
    std::map<std::tuple<int, int, int>, int> edgeIndex;
    a_edges.clear();
    const double scale = mesh.unitScale;
    std::vector<double> weightOf(std::size_t(nt), 0.0);   // by triangle: the space's weight at its centre
    for (int t = 0; t < nt; ++t) {
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !f.domains[std::size_t(d)]) continue;
        a_elemOn[std::size_t(t)] = 1;
        const Medium& m = a_media[std::size_t(d)];
        const int* dof = sp.dofs(t);
        QPointF x[3];
        for (int k = 0; k < 3; ++k) {
            x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
            nodeOn[std::size_t(dof[k])] = 1;
        }
        const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
        const double area = std::abs(det) / 2;
        const double weight = a_s.weight((x[0] + x[1] + x[2]) / 3.0);
        weightOf[std::size_t(t)] = weight;
        // ∇φk = perpendicular of the opposite edge over 2 A (signed).
        double gx[3], gy[3];
        for (int k = 0; k < 3; ++k) {
            const QPointF& a = x[(k + 1) % 3];
            const QPointF& b = x[(k + 2) % 3];
            gx[k] = (a.y() - b.y()) / det;
            gy[k] = (b.x() - a.x()) / det;
        }
        std::array<double, 9>& K = a_K[std::size_t(t)];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) K[std::size_t(3 * a + b)] = Eps0 * m.epsr * area * weight * (gx[a] * gx[b] + gy[a] * gy[b]);
        if (!m.semiconductor) continue;
        a_elemSemi[std::size_t(t)] = 1;
        for (int k = 0; k < 3; ++k) {
            // A node between two semiconductors takes the narrower gap's
            // medium - an absorber's, a well's, a channel's: their carriers
            // on their own bands to its edge -, between equal gaps the lower
            // domain's, whatever the order of its triangles. (The first
            // triangle's, as it was, made a Ge/Si interface's nodes now one,
            // now the other, as the mesh had them: a photodiode's step on
            // Linux's mesh had a slow part of 4.5 %, on macOS's of 0.02 %.)
            int& nd = a_nodeDomain[std::size_t(dof[k])];
            if (nd < 0) nd = d;
            else if (nd != d) {
                const double g = a_media[std::size_t(d)].Eg, h = a_media[std::size_t(nd)].Eg;
                if (g < h - 1e-9 * std::abs(h) || (std::abs(g - h) <= 1e-9 * std::abs(h) && d < nd)) nd = d;
            }
        }
        for (int k = 0; k < 3; ++k) {
            const int a = dof[k], b = dof[(k + 1) % 3];
            const auto key = std::minmax(a, b);
            const int bands = bandsOf[std::size_t(d)];
            auto it = edgeIndex.find({key.first, key.second, bands});
            if (it == edgeIndex.end()) {
                it = edgeIndex.emplace(std::tuple{key.first, key.second, bands}, int(a_edges.size())).first;
                Edge e;
                e.i = key.first;
                e.j = key.second;
                e.h = std::hypot(x[(k + 1) % 3].x() - x[k].x(), x[(k + 1) % 3].y() - x[k].y());
                const QPointF along = (x[(k + 1) % 3] - x[k]) / e.h * (a == e.i ? 1.0 : -1.0);
                e.tx = along.x();
                e.ty = along.y();
                e.medium = bands;
                // (Its other side's, if it has one already.)
                for (auto other = edgeIndex.lower_bound({key.first, key.second, -1}); other != edgeIndex.end() && std::get<0>(other->first) == key.first
                                                                                        && std::get<1>(other->first) == key.second;
                     ++other)
                    if (other->second != int(a_edges.size())) {
                        e.partner = other->second;
                        a_edges[std::size_t(other->second)].partner = int(a_edges.size());
                    }
                a_edges.push_back(e);
            }
            // (Its weights: edgeWeights(), once the mobilities are known.)
        }
    }
    if (cancelled()) {
        *error = tr("cancelled");
        return false;
    }
    // Doping, by node: each feature's in its share of the node's box (a node
    // on a junction between two domains doped differently takes their mean)
    // - each triangle's part of the box its corner's circumcentric (Voronoi)
    // area, a mixed one's where it is obtuse (half to the obtuse corner, a
    // quarter to each other): a Layered Region's rectangles, halved by their
    // diagonals, give each corner a quarter. (By their whole areas a node on
    // a side wall where two layers meet took them 1/3 and 2/3 - one triangle
    // of a row against two -, 8.5 mV across a uniform PIN.)
    a_Nd.assign(std::size_t(a_n), 0);
    a_Na.assign(std::size_t(a_n), 0);
    a_nodePoint.assign(std::size_t(a_n), {-1, {0, 0, 0}});
    struct Share {
        int domain, triangle, corner;
        double area;
    };
    std::vector<std::vector<Share>> shares(static_cast<std::size_t>(a_n));
    for (int t = 0; t < nt; ++t) {
        if (!a_elemOn[std::size_t(t)]) continue;
        const int* dof = sp.dofs(t);
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int k = 0; k < 3; ++k) {
            const int i = dof[k];
            if (a_nodePoint[std::size_t(i)].first < 0 || (a_elemSemi[std::size_t(t)] && !a_elemSemi[std::size_t(a_nodePoint[std::size_t(i)].first)]))
                a_nodePoint[std::size_t(i)] = {t, {ref[k][0], ref[k][1], 0}};
            if (!a_elemSemi[std::size_t(t)]) continue;
            const std::array<int, 3>& tri = mesh.triangles[std::size_t(t)];
            const QPointF P = mesh.nodes[std::size_t(tri[std::size_t(k)])], Qn = mesh.nodes[std::size_t(tri[std::size_t((k + 1) % 3)])],
                          R = mesh.nodes[std::size_t(tri[std::size_t((k + 2) % 3)])];
            const double area = std::abs((Qn.x() - P.x()) * (R.y() - P.y()) - (R.x() - P.x()) * (Qn.y() - P.y())) / 2;
            auto dot = [](QPointF u, QPointF v) { return u.x() * v.x() + u.y() * v.y(); };
            const double atP = dot(Qn - P, R - P), atQ = dot(P - Qn, R - Qn), atR = dot(P - R, Qn - R);
            double part = area / 3;
            if (area > 0) {
                if (atP < 0) part = area / 2;
                else if (atQ < 0 || atR < 0) part = area / 4;
                else {
                    // (|PR|² cot Q + |PQ|² cot R)/8, cot = dot/(2 area).
                    part = (dot(R - P, R - P) * atQ + dot(Qn - P, Qn - P) * atR) / (16 * area);
                }
            }
            shares[std::size_t(i)].push_back({mesh.domain[std::size_t(t)], t, k, part});
        }
    }
    // Its box: those shares, as the edges' weights are the Voronoi box's (a
    // third of each triangle, as it was, put a wall's node of rows of two
    // heights off its doping's box - mV across a uniform PIN: the update
    // check of 8bd5709). The parts that recombine: of its own bands.
    a_recStart.assign(std::size_t(a_n) + 1, 0);
    a_rec.clear();
    a_recVolume.assign(std::size_t(a_n), 0);
    a_partStart.assign(std::size_t(a_n) + 1, 0);
    a_parts.clear();
    for (int i = 0; i < a_n; ++i) {
        a_recStart[std::size_t(i)] = int(a_rec.size());
        a_partStart[std::size_t(i)] = int(a_parts.size());
        const int own = a_nodeDomain[std::size_t(i)];
        for (const Share& sh : shares[std::size_t(i)]) {
            const double v = sh.area * scale * scale * weightOf[std::size_t(sh.triangle)];
            a_box[std::size_t(i)] += v;
            auto part = std::find_if(a_parts.begin() + a_partStart[std::size_t(i)], a_parts.end(), [&sh](const BoxPart& b) { return b.domain == sh.domain; });
            if (part == a_parts.end()) a_parts.push_back({sh.domain, v});
            else part->volume += v;
            if (own < 0 || !sameBands(sh.domain, own)) continue;
            auto it = std::find_if(a_rec.begin() + a_recStart[std::size_t(i)], a_rec.end(), [&sh](const RecombiningPart& r) { return r.domain == sh.domain; });
            if (it == a_rec.end()) {
                a_rec.push_back({sh.domain, 0, 0, 0});
                it = a_rec.end() - 1;
            }
            it->volume += v;
            a_recVolume[std::size_t(i)] += v;
        }
    }
    a_recStart[std::size_t(a_n)] = int(a_rec.size());
    a_partStart[std::size_t(a_n)] = int(a_parts.size());
    PointEvaluator ev(a_s);
    Expression::Options inUnit;
    inUnit.lengthUnit = topo.unitScale;
    inUnit.numbersAreLengths = true;
    for (const DopingDef& dd : dopings) {
        const Node& df = *dd.node;
        const bool donor = df.text(QStringLiteral("dopant")) == QLatin1String("donor");
        const QString profile = df.text(QStringLiteral("profile"));
        std::vector<char> on(topo.domains.size(), 0);
        for (int d : dd.domains) on[std::size_t(d)] = 1;
        Expression N;
        double N0 = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, sigma = 0;
        // An implant: its dose, range, spreads (annealed), surface and window.
        double dose = 0, Rp = 0, sd = 0, sl = 0, ys = 0, w1 = -Inf, w2 = Inf;
        // From a process flow: its dopants' grid, each domain's material
        // there (the grid's at a point inside it); its species those listed.
        std::shared_ptr<const ProcessDopants> flow;
        std::vector<int> flowMaterial;
        QStringList flowSpecies;
        if (profile == QLatin1String("process")) {
            if (!topo.process || !topo.process->dopants) {
                problem(tr("%1: its doping is from the process flow, but the geometry has no flow with dopants (an implant, a doped "
                           "substrate, a source...)")
                            .arg(df.name()));
                continue;
            }
            flow = topo.process->dopants;
            for (const Topology::Domain& dom : topo.domains) flowMaterial.push_back(flow->materialAt(dom.inside.x(), dom.inside.y()));
            for (const QString& sp : df.text(QStringLiteral("species")).split(QLatin1Char(','), Qt::SkipEmptyParts))
                flowSpecies << (sp.trimmed().compare(QLatin1String("BF2"), Qt::CaseInsensitive) == 0 ? QStringLiteral("B") : sp.trimmed());
            for (const QString& sp : flowSpecies)
                if (!flow->field(sp)) problem(tr("%1: the flow has no %2").arg(df.name(), sp));
        }
        if (profile == QLatin1String("expression")) N = compile(df.text(QStringLiteral("N")), df, QStringLiteral("N"));
        else if (profile == QLatin1String("implant")) {
            auto length = [&](const QString& text) {
                if (text.trimmed().isEmpty()) return NaN;
                const Expression e = Expression::compile(text, constants, inUnit);
                if (!e.isValid() || !e.isConstant()) {
                    problem(tr("%1: %2").arg(df.name(), e.isValid() ? tr("its surface and window must be constants") : e.error()));
                    return NaN;
                }
                return e.constant() * topo.unitScale;
            };
            dose = number(df.text(QStringLiteral("dose")), Dim::of(-2), df.name());
            Rp = number(df.text(QStringLiteral("Rp")), Dim::length(), df.name());
            const double dRp = number(df.text(QStringLiteral("dRp")), Dim::length(), df.name());
            const double dRl = number(df.text(QStringLiteral("dRl")), Dim::length(), df.name());
            ys = length(df.text(QStringLiteral("surface")));
            if (!std::isfinite(ys)) ys = 0;
            const double a = length(df.text(QStringLiteral("x1"))), b = length(df.text(QStringLiteral("x2")));
            if (std::isfinite(a)) w1 = a;
            if (std::isfinite(b)) w2 = b;
            if (w2 < w1) std::swap(w1, w2);
            // The anneal: 2 D t, D = D0 e^(-Ea/kT).
            const double D0 = number(df.text(QStringLiteral("D0")), Dim::of(2, 0, -1), df.name());
            const double Ea = energyOf(df.text(QStringLiteral("Ea")), constants, &why);
            const double Ta = number(df.text(QStringLiteral("Ta")), Dim::temperature(), df.name());
            const double ta = number(df.text(QStringLiteral("ta")), Dim::of(0, 0, 1), df.name());
            const double twoDt = ta > 0 && D0 > 0 && Ta > 0 && std::isfinite(Ea) ? 2 * D0 * std::exp(-Ea / (KB * Ta)) * ta : 0.0;
            if (!(dose >= 0) || !(Rp >= 0) || !(dRp > 0) || !(dRl > 0) || !(ta >= 0)) problem(tr("%1: its dose, range and straggles").arg(df.name()));
            sd = std::sqrt(dRp * dRp + twoDt);
            sl = std::sqrt(dRl * dRl + twoDt);
        } else {
            N0 = number(df.text(QStringLiteral("N0")), conc, df.name());
            if (profile == QLatin1String("box")) {
                auto length = [&](const QJsonValue& v) {
                    const Expression e = Expression::compile(v.toVariant().toString(), constants, inUnit);
                    if (!e.isValid() || !e.isConstant()) {
                        problem(tr("%1: %2").arg(df.name(), e.isValid() ? tr("its box must be constants") : e.error()));
                        return 0.0;
                    }
                    return e.constant() * topo.unitScale;
                };
                const QJsonArray base = df.value(QStringLiteral("position")).toArray(), size = df.value(QStringLiteral("size")).toArray();
                x0 = length(base.at(0));
                y0 = length(base.at(1));
                x1 = x0 + length(size.at(0));
                y1 = y0 + length(size.at(1));
                if (x1 < x0) std::swap(x0, x1);
                if (y1 < y0) std::swap(y0, y1);
                const double dj = number(df.text(QStringLiteral("dj")), Dim::length(), df.name());
                const double Nb = number(df.text(QStringLiteral("Nb")), conc, df.name());
                if (!(dj > 0) || !(Nb > 0) || !(N0 > Nb)) problem(tr("%1: its junction depth, background and peak must be more than 0, the peak more than the background").arg(df.name()));
                else sigma = dj / std::sqrt(std::log(N0 / Nb));
            }
        }
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int i = 0; i < a_n; ++i) {
            double inside = 0, all = 0;
            const Share* where = nullptr;
            for (const Share& sh : shares[std::size_t(i)]) {
                all += sh.area;
                if (on[std::size_t(sh.domain)]) {
                    inside += sh.area;
                    where = &sh;
                }
            }
            if (!where || !(all > 0)) continue;
            if (flow) {
                // Each selected domain's share of the node: its material's
                // dopants there, donors and acceptors as that host has them.
                const QPointF p = sp.point(i) / topo.unitScale;
                double nd = 0, na = 0;
                for (const Share& sh : shares[std::size_t(i)]) {
                    if (!on[std::size_t(sh.domain)]) continue;
                    const int m = flowMaterial[std::size_t(sh.domain)];
                    if (m < 0) continue;
                    double dn = 0, ac = 0;
                    if (flowSpecies.isEmpty()) flow->carriers(p.x(), p.y(), m, &dn, &ac);
                    else
                        for (const QString& name : flowSpecies)
                            if (const ProcessDopants::Field* f = flow->field(name)) {
                                const double v = flow->at(*f, p.x(), p.y(), m, true);
                                const int q = dopantCharge(name, flow->hosts[std::size_t(m)]);
                                if (v > 0 && q != 0) (q > 0 ? dn : ac) += v;
                            }
                    nd += sh.area * dn;
                    na += sh.area * ac;
                }
                a_Nd[std::size_t(i)] += nd / all;
                a_Na[std::size_t(i)] += na / all;
                continue;
            }
            const double frac = inside / all;
            double v = 0;
            if (profile == QLatin1String("expression")) {
                if (!N.isValid()) continue;
                ev.moveTo(where->triangle, ref[where->corner][0], ref[where->corner][1]);
                v = ev.eval(N);
            } else if (profile == QLatin1String("implant")) {
                // Below the surface: dose/(√(2π) σ) (e^(-(d - Rp)²/2σ²) + its
                // image e^(-(d + Rp)²/2σ²)), its window's edges as erf's.
                const QPointF p = sp.point(i);
                const double d = ys - p.y();
                if (d < 0 || !(sd > 0)) continue;
                const double depth = dose / (std::sqrt(2 * M_PI) * sd)
                                     * (std::exp(-(d - Rp) * (d - Rp) / (2 * sd * sd)) + std::exp(-(d + Rp) * (d + Rp) / (2 * sd * sd)));
                const double across = 0.5 * ((std::isfinite(w1) ? std::erf((p.x() - w1) / (std::sqrt(2.0) * sl)) : 1.0)
                                             - (std::isfinite(w2) ? std::erf((p.x() - w2) / (std::sqrt(2.0) * sl)) : -1.0));
                v = depth * across;
            } else if (profile == QLatin1String("box")) {
                const QPointF p = sp.point(i);
                const double dx = std::max({x0 - p.x(), 0.0, p.x() - x1}), dy = std::max({y0 - p.y(), 0.0, p.y() - y1});
                const double dist = std::hypot(dx, dy);
                v = sigma > 0 ? N0 * std::exp(-(dist / sigma) * (dist / sigma)) : (dist == 0 ? N0 : 0);
            } else {
                v = N0;
            }
            if (!std::isfinite(v) || v < 0) continue;
            (donor ? a_Nd : a_Na)[std::size_t(i)] += frac * v;
        }
    }
    a_ni.assign(std::size_t(a_n), 0);
    a_n1.assign(std::size_t(a_n), 0);
    a_p1.assign(std::size_t(a_n), 0);
    a_tauN.assign(std::size_t(a_n), Inf);
    a_tauP.assign(std::size_t(a_n), Inf);
    PointEvaluator lifetimes(a_s);
    QString lifetimeWhy;
    a_delta.assign(std::size_t(a_n), 0);
    a_thetaN.assign(std::size_t(a_n), 0);
    a_thetaP.assign(std::size_t(a_n), 0);
    a_scale = 0;
    a_gap.assign(std::size_t(a_n), 0);
    for (auto* v : {&a_lgN, &a_lgP, &a_dlgN, &a_dlgP}) v->assign(std::size_t(a_n), 0.0);
    a_rN.assign(std::size_t(a_n), 1.0);
    a_rP.assign(std::size_t(a_n), 1.0);
    bool hetero = false;
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        const double gap = narrowing(m, i);
        a_gap[std::size_t(i)] = gap;
        const double ni = m.ni * std::exp(gap / (2 * KB * a_T));
        a_ni[std::size_t(i)] = ni;
        a_n1[std::size_t(i)] = ni * std::exp(m.Et / (KB * a_T));
        a_p1[std::size_t(i)] = ni * std::exp(-m.Et / (KB * a_T));
        // Its lifetimes in a medium: of x and y there, by its doping.
        auto lifetimesIn = [this, &lifetimes, &lifetimeWhy, &sp, i](const Medium& in) {
            double tn = in.taun, tp = in.taup;
            if (!in.taunOf.isConstant() || !in.taupOf.isConstant()) {
                const auto& [t, r] = a_nodePoint[std::size_t(i)];
                if (t >= 0) {
                    lifetimes.moveTo(t, r[0], r[1]);
                    if (!in.taunOf.isConstant()) tn = lifetimes.eval(in.taunOf);
                    if (!in.taupOf.isConstant()) tp = lifetimes.eval(in.taupOf);
                }
            }
            if (in.fossum) {
                const double N = a_Nd[std::size_t(i)] + a_Na[std::size_t(i)];
                tn /= in.fA + in.fB * N / in.N0n + in.fC * std::pow(N / in.N0n, in.fE);
                tp /= in.fA + in.fB * N / in.N0p + in.fC * std::pow(N / in.N0p, in.fE);
            }
            if (!(tn > 0) || !(tp > 0)) {
                if (lifetimeWhy.isEmpty()) lifetimeWhy = tr("%1: a lifetime at (%2, %3) is not more than 0").arg(a_node->name()).arg(sp.point(i).x()).arg(sp.point(i).y());
                tn = tn > 0 ? tn : Inf;
                tp = tp > 0 ? tp : Inf;
            }
            return std::pair{tn, tp};
        };
        if (m.srh) std::tie(a_tauN[std::size_t(i)], a_tauP[std::size_t(i)]) = lifetimesIn(m);
        for (int k = a_recStart[std::size_t(i)]; k < a_recStart[std::size_t(i) + 1]; ++k) {
            RecombiningPart& part = a_rec[std::size_t(k)];
            const Medium& in = a_media[std::size_t(part.domain)];
            if (part.domain == d) std::tie(part.tn, part.tp) = std::pair{a_tauN[std::size_t(i)], a_tauP[std::size_t(i)]};
            else if (in.srh) std::tie(part.tn, part.tp) = lifetimesIn(in);
            else part.tn = part.tp = Inf;
        }
        a_scale = std::max({a_scale, std::abs(doping(i)), ni});
        // (A node on a heterojunction is its narrower gap's: the bands' step
        // is taken at it, by the edges into the other.)
        const double delta = (m.chi + m.dEc - a_refChi - a_refDEc) / Q;
        a_delta[std::size_t(i)] = delta;
        std::tie(a_thetaN[std::size_t(i)], a_thetaP[std::size_t(i)]) = bandsIn(m, i);
        hetero = hetero || std::abs(delta) > 1e-9 || std::abs(m.ni / a_refNi - 1) > 1e-9;
    }
    if (!lifetimeWhy.isEmpty()) problem(lifetimeWhy);
    // Schrödinger-Poisson's nodes, their quantum potentials nought to start.
    a_thetaN0 = a_thetaN;
    a_thetaP0 = a_thetaP;
    heteroSteps();
    a_lamN.assign(std::size_t(a_n), 0.0);
    a_lamP.assign(std::size_t(a_n), 0.0);
    a_quantumNodes.clear();
    if (a_quantumElectrons || a_quantumHoles)
        for (int t = 0; t < nt; ++t) {
            if (!a_elemSemi[std::size_t(t)] || !quantumOn[std::size_t(mesh.domain[std::size_t(t)])]) continue;
            for (int k = 0; k < 3; ++k) a_quantumNodes.push_back(sp.dofs(t)[k]);
        }
    std::sort(a_quantumNodes.begin(), a_quantumNodes.end());
    a_quantumNodes.erase(std::unique(a_quantumNodes.begin(), a_quantumNodes.end()), a_quantumNodes.end());
    if (hetero) {
        const QString line = tr("%1: heterojunctions - the bands' offsets (Anderson's rule: the electron affinities, the band gaps) "
                                "between its semiconductors")
                                 .arg(a_node->name());
        if (!a_s.log.contains(line)) a_s.log << line;
    }
    if (!(a_scale > 0)) a_scale = 1e16;
    if (cancelled()) {
        *error = tr("cancelled");
        return false;
    }
    if (a_Bz != 0 && a_slices > 1) problem(tr("%1: a magnetic field and a device in slices are not solved together").arg(a_node->name()));
    // Whose mobilities change with the state; held in Newton's method.
    for (int d : domains) {
        const Medium& m = a_media[std::size_t(d)];
        a_varying = a_varying || m.varies();
        if (m.lowField == Medium::LowField::Dorkel || m.lowField == Medium::LowField::Klaassen || m.lowField == Medium::LowField::Yamaguchi
            || (m.lowField == Medium::LowField::Expression && m.exprState))
            a_alongCurrent = true;
    }
    // Mobilities by element (doping: the element's nodes' mean), then by edge.
    std::vector<double> mun(std::size_t(nt), 0), mup(std::size_t(nt), 0);
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        const int* dof = sp.dofs(t);
        double N = 0;
        for (int k = 0; k < 3; ++k) N += (a_Nd[std::size_t(dof[k])] + a_Na[std::size_t(dof[k])]) / 3;
        mun[std::size_t(t)] = m.mun;
        mup[std::size_t(t)] = m.mup;
        if (m.dopingMobility) {
            mun[std::size_t(t)] = m.munMin + (m.mun - m.munMin) / (1 + std::pow(N / m.nRefN, m.alphaN));
            mup[std::size_t(t)] = m.mupMin + (m.mup - m.mupMin) / (1 + std::pow(N / m.nRefP, m.alphaP));
        }
        if (m.masetti) {
            // µmin1 e^(-Pc/N) + (µmax - µmin2)/(1 + (N/Cr)^α) - µ1/(1 + (Cs/N)^β); no
            // doping: µmax (less µmin2 and its terms of N).
            auto masetti = [&](const double* c, double lattice) {
                if (!(N > 0)) return lattice - c[1] + (c[3] > 0 ? 0.0 : c[0]);
                return c[0] * std::exp(-c[3] / N) + (lattice - c[1]) / (1 + std::pow(N / c[4], c[6])) - c[2] / (1 + std::pow(c[5] / N, c[7]));
            };
            mun[std::size_t(t)] = std::max(masetti(m.ms[0], m.msMax[0]), 1e-8);
            mup[std::size_t(t)] = std::max(masetti(m.ms[1], m.msMax[1]), 1e-8);
        }
        // (Arora's at the physics' T - a lattice temperature's own below -;
        // Yamaguchi's low-field one, its fields' in elementMobilities.)
        if (m.lowField == Medium::LowField::Arora) {
            mun[std::size_t(t)] = std::max(aroraMobility(m.ar[0], N, a_T), 1e-8);
            mup[std::size_t(t)] = std::max(aroraMobility(m.ar[1], N, a_T), 1e-8);
        }
        if (m.lowField == Medium::LowField::Yamaguchi) {
            mun[std::size_t(t)] = yamaguchiLowField(m.ya[0], N);
            mup[std::size_t(t)] = yamaguchiLowField(m.ya[1], N);
        }
    }
    a_elements.assign(std::size_t(nt), ElementMobility());
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        ElementMobility& em = a_elements[std::size_t(t)];
        em.mun = mun[std::size_t(t)];
        em.mup = mup[std::size_t(t)];
        em.domain = mesh.domain[std::size_t(t)];
        const int* dof = sp.dofs(t);
        for (int k = 0; k < 3; ++k) em.N += (a_Nd[std::size_t(dof[k])] + a_Na[std::size_t(dof[k])]) / 3;
        for (int k = 0; k < 3; ++k) {
            double n0 = 0, p0 = 0;
            neutral(dof[k], &n0, &p0);
            em.Nd += a_Nd[std::size_t(dof[k])] / 3;
            em.Na += a_Na[std::size_t(dof[k])] / 3;
            em.n0 += n0 / 3;
            em.p0 += p0 / 3;
        }
        {
            const Medium& m = a_media[std::size_t(em.domain)];
            if (m.lowField == Medium::LowField::Klaassen) {
                em.pmin[0] = klaassenPmin(a_T, m.klMe);
                em.pmin[1] = klaassenPmin(a_T, m.klMh);
            }
        }
        for (int k = 0; k < 3; ++k) em.step = em.step || bandsOf[std::size_t(a_nodeDomain[std::size_t(dof[k])])] != bandsOf[std::size_t(em.domain)];
        QPointF x[3];
        for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
        const double weight = a_s.weight((x[0] + x[1] + x[2]) / 3.0);
        const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
        em.volume = std::abs(det) / 2 * weight;
        em.half = 0.5 * weight;
        for (int k = 0; k < 3; ++k) {
            const auto key = std::minmax(dof[k], dof[(k + 1) % 3]);
            em.edge[k] = edgeIndex.at({key.first, key.second, bandsOf[std::size_t(em.domain)]});
            Edge& edge = a_edges[std::size_t(em.edge[k])];
            if (edge.domain < 0 || (!a_media[std::size_t(edge.domain)].ionization && a_media[std::size_t(em.domain)].ionization)) edge.domain = em.domain;
            // The edge's cot weight from this triangle: half the cotangent of
            // the angle opposite it.
            const QPointF u = x[k] - x[(k + 2) % 3], v = x[(k + 1) % 3] - x[(k + 2) % 3];
            const double cot = (u.x() * v.x() + u.y() * v.y()) / std::abs(u.x() * v.y() - u.y() * v.x());
            em.cot[k] = 0.5 * cot * weight;
            const QPointF& a = x[(k + 1) % 3];
            const QPointF& b = x[(k + 2) % 3];
            em.gx[k] = (a.y() - b.y()) / det;
            em.gy[k] = (b.x() - a.x()) / det;
        }
    }
    // Trap-assisted tunnelling's field at each node: its triangles' of its
    // own domain, by their volumes. (A heterojunction's node is its
    // narrower gap's - germanium's, its many intrinsic carriers -: the
    // silicon's field beside it, four times its own, took its tunnelling's
    // e^((F/FΓ)²) there.)
    a_fieldStart.assign(std::size_t(a_n) + 1, 0);
    a_fieldTris.clear();
    if (a_tat) {
        std::vector<std::vector<std::pair<int, double>>> around(static_cast<std::size_t>(a_n));
        for (int t = 0; t < nt; ++t) {
            const int d = mesh.domain[std::size_t(t)];
            if (!a_elemSemi[std::size_t(t)] || !a_media[std::size_t(d)].tat) continue;
            for (int k = 0; k < 3; ++k) {
                const int i = sp.dofs(t)[k];
                if (a_nodeDomain[std::size_t(i)] == d) around[std::size_t(i)].emplace_back(t, a_elements[std::size_t(t)].volume);
            }
        }
        for (int i = 0; i < a_n; ++i) {
            a_fieldStart[std::size_t(i)] = int(a_fieldTris.size());
            double total = 0;
            for (const auto& [t, v] : around[std::size_t(i)]) total += v;
            if (total > 0)
                for (const auto& [t, v] : around[std::size_t(i)]) a_fieldTris.emplace_back(t, v / total);
        }
        a_fieldStart[std::size_t(a_n)] = int(a_fieldTris.size());
    }
    // Each triangle's neighbours (a field line followed across them).
    a_neighbors.assign(std::size_t(nt), {-1, -1, -1});
    {
        std::map<std::pair<int, int>, std::pair<int, int>> side;   // an edge: (triangle, its edge k)
        for (int t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k) {
                const int a = mesh.triangles[std::size_t(t)][std::size_t(k)], b = mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)];
                const auto key = std::minmax(a, b);
                const auto it = side.find({key.first, key.second});
                if (it == side.end()) side.emplace(std::pair{key.first, key.second}, std::pair{t, k});
                else {
                    a_neighbors[std::size_t(t)][std::size_t(k)] = it->second.first;
                    a_neighbors[std::size_t(it->second.first)][std::size_t(it->second.second)] = t;
                }
            }
    }
    // Surface scattering: each triangle's nearest interface with an
    // insulator (a side shared with one), its normal and the distance to it
    // from the triangle's centre.
    if (a_lombardi) {
        struct Side {
            QPointF a, b;
            double nx, ny;
        };
        std::vector<Side> sides;
        for (int t = 0; t < nt; ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            for (int k = 0; k < 3; ++k) {
                const int u = a_neighbors[std::size_t(t)][std::size_t(k)];
                if (u < 0 || !a_elemOn[std::size_t(u)] || a_elemSemi[std::size_t(u)]) continue;
                const QPointF a = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale;
                const QPointF b = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)])] * scale;
                const double h = std::hypot(b.x() - a.x(), b.y() - a.y());
                if (h > 0) sides.push_back({a, b, -(b.y() - a.y()) / h, (b.x() - a.x()) / h});
            }
        }
        if (sides.empty()) {
            // (What reads it, named: its scattering none, Eperp 0, dsurf 1 m.)
            QStringList readers;
            for (const Medium& m : a_media) {
                if (!m.semiconductor) continue;
                QString what;
                if (m.surface) what = m.shirahata ? tr("Shirahata's surface scattering") : tr("Lombardi's surface scattering");
                if (m.lowField == Medium::LowField::Yamaguchi) what = tr("Yamaguchi's field across the interface");
                if (m.lowField == Medium::LowField::Expression && m.exprAcross) what = tr("the mobility's expression of Eperp or dsurf (0 and 1 m)");
                if (!what.isEmpty() && !readers.contains(what)) readers << what;
            }
            const QString line = tr("%1: %2, but no semiconductor meets an insulator: none").arg(a_node->name(), readers.join(QStringLiteral(", ")));
            if (!a_s.log.contains(line)) a_s.log << line;
        }
        for (int t = 0; t < nt && !sides.empty(); ++t) {
            ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0 || !a_media[std::size_t(em.domain)].perpendicular()) continue;
            QPointF c;
            for (int k = 0; k < 3; ++k) c += mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * scale / 3.0;
            double best = Inf;
            const Side* nearest = nullptr;
            for (const Side& side : sides) {
                const double ux = side.b.x() - side.a.x(), uy = side.b.y() - side.a.y();
                const double f = std::clamp(((c.x() - side.a.x()) * ux + (c.y() - side.a.y()) * uy) / (ux * ux + uy * uy), 0.0, 1.0);
                const double d = std::hypot(c.x() - side.a.x() - f * ux, c.y() - side.a.y() - f * uy);
                if (d < best) {
                    best = d;
                    nearest = &side;
                }
            }
            em.nx = nearest->nx;
            em.ny = nearest->ny;
            em.damp = std::exp(-best / a_media[std::size_t(em.domain)].lcrit);
            em.dist = best;
        }
    }
    // Heterojunctions' emission: each edge from a node of the other bands
    // into its own, its side's Richardson velocities and masses (of its
    // densities of states: Nc = 2 (2π m* kT/h²)^(3/2)), its steps.
    a_emission = a_node->text(QStringLiteral("heterojunctions")) == QLatin1String("thermionic") ? 1
                 : a_node->text(QStringLiteral("heterojunctions")) == QLatin1String("tunnelling") ? 2
                                                                                                  : 0;
    // (Each interface node's normal into each other side: the other side's
    // triangles' edges along the interface, two of its nodes the node's
    // bands', turned, weighed by their lengths.)
    std::map<std::pair<int, int>, QPointF> normals;   // (node, bands): the normal into those bands
    if (a_emission > 0)
        for (int t = 0; t < nt; ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            const int bands = a_bandsOf[std::size_t(mesh.domain[std::size_t(t)])];
            const int* dof = sp.dofs(t);
            for (int k = 0; k < 3; ++k) {
                const int i = dof[k], j = dof[(k + 1) % 3], o = dof[(k + 2) % 3];
                if (a_nodeDomain[std::size_t(i)] < 0 || a_nodeDomain[std::size_t(j)] < 0) continue;
                if (a_bandsOf[std::size_t(a_nodeDomain[std::size_t(i)])] == bands || a_bandsOf[std::size_t(a_nodeDomain[std::size_t(j)])] == bands) continue;
                if (a_nodeDomain[std::size_t(o)] >= 0 && a_bandsOf[std::size_t(a_nodeDomain[std::size_t(o)])] != bands) continue;
                const QPointF pi = sp.point(i), pj = sp.point(j), po = sp.point(o);
                QPointF nrm(-(pj.y() - pi.y()), pj.x() - pi.x());   // (its length the edge's)
                if (QPointF::dotProduct(nrm, po - pi) < 0) nrm = -nrm;
                normals[{i, bands}] += nrm;
                normals[{j, bands}] += nrm;
            }
        }
    for (Edge& e : a_edges) {
        e.vRn = e.vRp = e.stepN = e.stepP = e.massN = e.massP = e.across = 0;
        if (a_emission == 0 || e.medium < 0) continue;
        const bool atI = a_bandsOf[std::size_t(a_nodeDomain[std::size_t(e.i)])] != e.medium;
        const bool atJ = a_bandsOf[std::size_t(a_nodeDomain[std::size_t(e.j)])] != e.medium;
        // (Along the interface, both its ends the other bands': it crosses nothing.)
        if (atI == atJ) continue;
        const auto found = normals.find({atI ? e.i : e.j, e.medium});
        if (found == normals.end()) continue;
        const double len = std::hypot(found->second.x(), found->second.y());
        if (!(len > 0)) continue;
        e.across = std::abs(e.tx * found->second.x() + e.ty * found->second.y()) / len;
        if (!(e.across > 1e-6)) continue;
        e.emitsAtI = atI;
        const Medium& m = a_media[std::size_t(e.medium)];
        const double kT = KB * a_T, h = 6.62607015e-34;
        auto mass = [kT, h](double N) { return std::pow(N / 2, 2.0 / 3.0) * h * h / (2 * M_PI * kT); };
        e.massN = mass(m.Nc);
        e.massP = mass(m.Nv);
        e.vRn = std::sqrt(kT / (2 * M_PI * e.massN));
        e.vRp = std::sqrt(kT / (2 * M_PI * e.massP));
        e.stepN = Q * std::abs(atI ? e.sNi : e.sNj);
        e.stepP = Q * std::abs(atI ? e.sPi : e.sPj);
    }
    edgeWeights(nullptr);
    if (cancelled()) {
        *error = tr("cancelled");
        return false;
    }
    // Contacts and surface charges.
    a_contacts.clear();
    a_nodeContact.assign(std::size_t(a_n), -1);
    a_surface.clear();
    a_surfaceLength.clear();
    a_traps.clear();
    a_fn.clear();
    std::vector<std::tuple<int, int, QString>> surfaceNames;   // a surface's recombination: its levels' range, its name
    for (const Node* bf : boundaryFeatures) {
        const QString t = bf->type;
        const QVector<int> picked = resolveSelection(topo, model, Level::Boundary, bf->selection(), &why);
        std::vector<char> on(topo.boundaries.size(), 0);
        for (int b : picked) on[std::size_t(b)] = 1;
        // Its nodes, and each one's share of its length (m², weighted).
        std::map<int, double> share;
        for (const Mesh::Edge& me : mesh.edges) {
            if (me.boundary < 0 || !on[std::size_t(me.boundary)]) continue;
            const QPointF a = mesh.nodes[std::size_t(me.a)] * scale, b = mesh.nodes[std::size_t(me.b)] * scale;
            const double len = std::hypot(b.x() - a.x(), b.y() - a.y()) * a_s.weight((a + b) / 2.0);
            if (!nodeOn[std::size_t(me.a)] && !nodeOn[std::size_t(me.b)]) continue;
            share[me.a] += len / 2;
            share[me.b] += len / 2;
        }
        if (t == QLatin1String("semiconductor/polarization")) {
            // Wurtzite's polarization at its interfaces (c along the axis
            // chosen; the metal face's or the nitrogen's): each one's bound
            // charge -ΔP·n, n from its one side into the other - its sides'
            // P = Psp + e31 (εxx + εyy) + e33 εzz, their nitride compounds'
            // pseudomorphic on the substrate's lattice in the c-plane (none:
            // relaxed). Its interfaces straight (their ends' line its normal).
            const bool alongX = bf->text(QStringLiteral("axis")) == QLatin1String("x");
            const double face = bf->text(QStringLiteral("polarity")) == QLatin1String("nitrogen") ? -1.0 : 1.0;
            double lattice = NaN;
            const QString sub = bf->text(QStringLiteral("substrate")).trimmed();
            if (!sub.isEmpty()) {
                QString compound = sub;
                double sx = 0, sy = 0;
                for (const Node& m : model.materials().children)
                    if (m.name().compare(sub, Qt::CaseInsensitive) == 0 || m.text(QStringLiteral("library")).compare(sub, Qt::CaseInsensitive) == 0) {
                        compound = materialProperty(m, QStringLiteral("compound"));
                        sx = evaluateConstant(materialProperty(m, QStringLiteral("x")), constants, Dim()).value_or(0);
                        sy = evaluateConstant(materialProperty(m, QStringLiteral("y")), constants, Dim()).value_or(0);
                    }
                if (const std::optional<MaterialEntry> e = findMaterial(sub); e && compound == sub) {
                    compound = e->properties.value(QStringLiteral("compound")).toString();
                    sx = evaluateConstant(e->properties.value(QStringLiteral("x")).toString(), constants, Dim()).value_or(0);
                    sy = evaluateConstant(e->properties.value(QStringLiteral("y")).toString(), constants, Dim()).value_or(0);
                }
                if (const std::optional<WurtziteParameters> w = wurtziteParameters(compound, sx, a_T, nullptr, sy)) lattice = w->a;
                else lattice = number(sub, Dim::length(), tr("%1's substrate (a nitride or a lattice constant)").arg(bf->name()));
            }
            auto polarizationOf = [&](int d) -> std::optional<double> {
                const Node* mat = d >= 0 ? material[std::size_t(d)] : nullptr;
                if (!mat) return std::nullopt;
                const QString compound = materialProperty(*mat, QStringLiteral("compound")).trimmed();
                const double x = evaluateConstant(materialProperty(*mat, QStringLiteral("x")), constants, Dim()).value_or(0);
                const double y = evaluateConstant(materialProperty(*mat, QStringLiteral("y")), constants, Dim()).value_or(0);
                const std::optional<WurtziteParameters> w = isWurtzite(compound) ? wurtziteParameters(compound, x, a_T, nullptr, y) : std::nullopt;
                if (!w) return std::nullopt;
                const double exx = std::isfinite(lattice) ? (lattice - w->a) / w->a : 0, ezz = -2 * w->C13 / w->C33 * exx;
                return w->Psp + 2 * w->e31 * exx + w->e33 * ezz;
            };
            for (int b : picked) {
                const Topology::Boundary& bd = topo.boundaries[std::size_t(b)];
                const std::optional<double> pu = polarizationOf(bd.up), pd = polarizationOf(bd.down);
                if (!pu || !pd) {
                    problem(tr("%1: boundary %2 is not between two nitrides (their materials' compounds GaN, AlGaN, InGaN...)").arg(bf->name()).arg(b + 1));
                    continue;
                }
                const std::vector<QPointF> line = bd.polyline(topo);
                const QPointF dir = line.back() - line.front();
                const double length = std::hypot(dir.x(), dir.y());
                if (!(length > 0)) continue;
                // (Into its up side - its left as walked - from its down side.)
                const double nc = (alongX ? -dir.y() : dir.x()) / length;
                const double sigma = face * (*pd - *pu) * nc;
                std::map<int, double> own;
                for (const Mesh::Edge& me : mesh.edges) {
                    if (me.boundary != b) continue;
                    const QPointF pa = mesh.nodes[std::size_t(me.a)] * scale, pb = mesh.nodes[std::size_t(me.b)] * scale;
                    const double len = std::hypot(pb.x() - pa.x(), pb.y() - pa.y()) * a_s.weight((pa + pb) / 2.0);
                    if (!nodeOn[std::size_t(me.a)] && !nodeOn[std::size_t(me.b)]) continue;
                    own[me.a] += len / 2;
                    own[me.b] += len / 2;
                }
                const Expression e = Expression::compile(QString::number(sigma, 'g', 17), constants);
                for (const auto& [node, len] : own) {
                    a_surface.emplace_back(node, e);
                    a_surfaceLength.push_back(len);
                }
                a_s.log << tr("%1: boundary %2's polarization charge %3").arg(bf->name()).arg(b + 1).arg(formatQuantity(sigma, Dim::of(-2, 0, 1, 1)));
            }
            continue;
        }
        if (t == QLatin1String("semiconductor/charge")) {
            const Expression e = compile(bf->text(QStringLiteral("rhos")), *bf, QStringLiteral("rhos"));
            if (!e.isValid()) continue;
            for (const auto& [node, len] : share) {
                a_surface.emplace_back(node, e);
                a_surfaceLength.push_back(len);
            }
            continue;
        }
        if (t == QLatin1String("semiconductor/fn")) {
            // Lenzlinger and Snow's: A = q³ m0/(8π h φB m_ox), B = 8π √(2 m_ox)
            // φB^(3/2)/(3 q h).
            constexpr double H = 6.62607015e-34, M0 = 9.1093837015e-31;
            const double phiB = energyOf(bf->text(QStringLiteral("phiB")), constants, &why);
            const double mox = number(bf->text(QStringLiteral("mox")), Dim(), bf->name());
            if (!(phiB > 0) || !(mox > 0)) {
                problem(tr("%1: its barrier and mass must be more than 0").arg(bf->name()));
                continue;
            }
            FowlerNordheim fn;
            fn.A = Q * Q * Q / (8 * M_PI * H * phiB * mox);
            fn.B = 8 * M_PI * std::sqrt(2 * mox * M0) * std::pow(phiB, 1.5) / (3 * Q * H);
            fn.phi = phiB / Q;
            // Each edge of its boundaries with an insulator's triangle beside it.
            std::map<std::pair<int, int>, int> insulator;
            for (int tt = 0; tt < nt; ++tt) {
                if (!a_elemOn[std::size_t(tt)] || a_elemSemi[std::size_t(tt)]) continue;
                for (int k = 0; k < 3; ++k) {
                    const auto key = std::minmax(mesh.triangles[std::size_t(tt)][std::size_t(k)], mesh.triangles[std::size_t(tt)][std::size_t((k + 1) % 3)]);
                    insulator[{key.first, key.second}] = tt;
                }
            }
            for (const Mesh::Edge& me : mesh.edges) {
                if (me.boundary < 0 || !on[std::size_t(me.boundary)]) continue;
                const auto key = std::minmax(me.a, me.b);
                const auto it = insulator.find({key.first, key.second});
                if (it == insulator.end()) continue;
                const QPointF a = mesh.nodes[std::size_t(me.a)] * scale, b = mesh.nodes[std::size_t(me.b)] * scale;
                fn.edges.push_back({me.a, me.b});
                fn.triangles.push_back(it->second);
                fn.lengths.push_back(std::hypot(b.x() - a.x(), b.y() - a.y()) * a_s.weight((a + b) / 2.0));
            }
            if (fn.edges.empty()) problem(tr("%1: no insulator meets its boundaries").arg(bf->name()));
            a_fn.push_back(fn);
            continue;
        }
        if (t == QLatin1String("semiconductor/surfacerec")) {
            // A trap level of no charge: N 1, cn Sn, cp Sp - (n p - ni²)/((n
            // + n1)/Sp + (p + p1)/Sn) per area; of both carriers, or (its
            // "Apply to majority carriers" off) of an ohmic contact's
            // minority carriers alone, its nodes sorted out once the
            // contacts are known.
            const QString where = bf->name();
            const bool majority = bf->props.contains(QStringLiteral("majority")) ? bf->flag(QStringLiteral("majority")) : true;
            const double Sn = number(bf->text(QStringLiteral("Sn")), velocity, where);
            const double Sp = number(bf->text(QStringLiteral("Sp")), velocity, where);
            const double Et = energyOf(bf->text(QStringLiteral("Et")), constants, &why);
            if (!(Sn >= 0) || !(Sp >= 0) || !std::isfinite(Et)) {
                problem(tr("%1: its recombination velocities no less than 0, its level given").arg(where));
                continue;
            }
            if (Sn == 0 || Sp == 0) continue;
            int count = 0;
            for (const auto& [node, len] : share) {
                if (a_nodeDomain[std::size_t(node)] < 0) continue;
                const double ni = a_ni[std::size_t(node)];
                TrapLevel tl;
                tl.node = node;
                tl.area = len;
                tl.N = 1;
                tl.n1 = ni * std::exp(Et / (KB * a_T));
                tl.p1 = ni * std::exp(-Et / (KB * a_T));
                tl.cn = Sn;
                tl.cp = Sp;
                tl.charged = false;
                tl.minorityOnly = !majority;
                a_traps.push_back(tl);
                ++count;
            }
            if (count == 0) problem(tr("%1: none of its boundaries meets a semiconductor").arg(where));
            surfaceNames.push_back({int(a_traps.size()) - count, int(a_traps.size()), where});
            continue;
        }
        if (t == QLatin1String("semiconductor/traps")) {
            const QString where = bf->name();
            const double sn = number(bf->text(QStringLiteral("sigman")), Dim::area(), where);
            const double sp = number(bf->text(QStringLiteral("sigmap")), Dim::area(), where);
            const double vth = number(bf->text(QStringLiteral("vth")), velocity, where);
            if (!(sn > 0) || !(sp > 0) || !(vth > 0)) {
                problem(tr("%1: its cross sections and thermal velocity must be more than 0").arg(where));
                continue;
            }
            // Its levels: one, or the range's in equal steps (each Dit ΔE at
            // its middle).
            std::vector<std::pair<double, double>> levels;   // (Et - Ei in J, 1/m²)
            if (bf->text(QStringLiteral("distribution")) == QLatin1String("level")) {
                const double Nit = number(bf->text(QStringLiteral("Nit")), Dim::of(-2), where);
                const double Et = energyOf(bf->text(QStringLiteral("Et")), constants, &why);
                if (!(Nit >= 0) || !std::isfinite(Et)) problem(tr("%1: its density and level").arg(where));
                else levels.emplace_back(Et, Nit);
            } else {
                const double Dit = number(bf->text(QStringLiteral("Dit")), Dim::of(-4, -1, 2), where);
                const double lo = energyOf(bf->text(QStringLiteral("Elow")), constants, &why);
                const double hi = energyOf(bf->text(QStringLiteral("Ehigh")), constants, &why);
                // (Its levels from 1 to 1000: 2147483647 made at every node
                // took memory without end - the fuzz of 21 October.)
                const int count = bf->integer(QStringLiteral("levels"));
                if (!(Dit >= 0) || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) problem(tr("%1: its density and range").arg(where));
                else if (count < 1 || count > 1000) problem(tr("%1: its levels from 1 to 1000, not %2").arg(where).arg(count));
                else
                    for (int k = 0; k < count; ++k) levels.emplace_back(lo + (k + 0.5) * (hi - lo) / count, Dit * (hi - lo) / count);
            }
            const bool acceptor = bf->text(QStringLiteral("kind")) != QLatin1String("donor");
            for (const auto& [node, len] : share) {
                if (a_nodeDomain[std::size_t(node)] < 0) continue;
                const double ni = a_ni[std::size_t(node)];
                for (const auto& [Et, N] : levels) {
                    TrapLevel tl;
                    tl.node = node;
                    tl.area = len;
                    tl.N = N;
                    tl.n1 = ni * std::exp(Et / (KB * a_T));
                    tl.p1 = ni * std::exp(-Et / (KB * a_T));
                    tl.cn = sn * vth;
                    tl.cp = sp * vth;
                    tl.acceptor = acceptor;
                    a_traps.push_back(tl);
                }
            }
            continue;
        }
        if (t != QLatin1String("semiconductor/ohmic") && t != QLatin1String("semiconductor/schottky") && t != QLatin1String("semiconductor/gate"))
            continue;
        Contact c;
        c.name = bf->text(QStringLiteral("name"));
        c.kind = t == QLatin1String("semiconductor/ohmic") ? Contact::Ohmic : t == QLatin1String("semiconductor/gate") ? Contact::Gate : Contact::Schottky;
        c.V0 = compile(bf->text(QStringLiteral("V0")), *bf, QStringLiteral("V0"), false);
        // The metal beside it - a domain on its far side whose material has a
        // work function φ (the library's metals): its own where none is set.
        double metalPhi = NaN, semiconductorChi = NaN;
        QString metalName;
        for (int b : picked)
            for (int d : {topo.boundaries[std::size_t(b)].up, topo.boundaries[std::size_t(b)].down}) {
                if (d < 0) continue;
                const Node* mat = material[std::size_t(d)];
                if (a_media[std::size_t(d)].semiconductor) {
                    if (!std::isfinite(semiconductorChi)) semiconductorChi = a_media[std::size_t(d)].chi;
                    continue;
                }
                const QString phi = mat ? materialProperty(*mat, QStringLiteral("phi"), &atT, a_T) : QString();
                if (phi.trimmed().isEmpty() || std::isfinite(metalPhi)) continue;
                metalPhi = energyOf(phi, atT, &why);
                metalName = mat->name();
            }
        if (c.kind == Contact::Schottky) {
            if (!bf->props.contains(QStringLiteral("phiB")) && std::isfinite(metalPhi) && std::isfinite(semiconductorChi)) {
                c.phiB = metalPhi - semiconductorChi;
                a_s.log << tr("%1: its barrier its metal's, %2: φ %3 eV less χ %4 eV, %5 eV")
                               .arg(bf->name(), metalName, formatNumber(metalPhi / Q, 4), formatNumber(semiconductorChi / Q, 4),
                                    formatNumber(c.phiB / Q, 4));
            } else
                c.phiB = energyOf(bf->text(QStringLiteral("phiB")), constants, &why);
            if (!std::isfinite(c.phiB)) problem(tr("%1: its barrier: %2").arg(bf->name(), why));
            c.vn = number(bf->text(QStringLiteral("vn")), velocity, bf->name());
            c.vp = number(bf->text(QStringLiteral("vp")), velocity, bf->name());
        }
        if (c.kind == Contact::Gate) {
            if (!bf->props.contains(QStringLiteral("phim")) && std::isfinite(metalPhi)) {
                c.phim = metalPhi;
                a_s.log << tr("%1: its work function its metal's, %2: %3 eV").arg(bf->name(), metalName, formatNumber(metalPhi / Q, 4));
            } else
                c.phim = energyOf(bf->text(QStringLiteral("phim")), constants, &why);
            if (!std::isfinite(c.phim)) problem(tr("%1: its work function: %2").arg(bf->name(), why));
        }
        // Its circuit (a metal contact's, a Schottky one's): V0 through
        // resistances, a capacitance across.
        if (c.kind != Contact::Gate) {
            const QString circuit = bf->text(QStringLiteral("circuit"));
            c.circuit = circuit == QLatin1String("series") ? Contact::Series : circuit == QLatin1String("biastee") ? Contact::BiasTee : Contact::Held;
            if (c.circuit != Contact::Held) {
                const Dim ohm = Dim::of(2, 1, -3, -2);
                c.Rs = number(bf->text(QStringLiteral("Rs")), ohm, bf->name());
                c.RL = number(bf->text(QStringLiteral("RL")), ohm, bf->name());
                c.Cp = number(bf->text(QStringLiteral("Cp")), Dim::capacitance(), bf->name());
                if (!(c.Rs >= 0) || !(c.RL >= 0) || !(c.Cp >= 0))
                    problem(tr("%1: its circuit's Rs, RL and Cp must be 0 or more").arg(bf->name()));
            }
        }
        for (const Contact& other : a_contacts)
            if (other.name == c.name) problem(tr("two contacts are named %1").arg(c.name));
        const int index = int(a_contacts.size());
        for (const auto& [node, len] : share) {
            if (a_nodeContact[std::size_t(node)] >= 0) continue;   // (a corner: the first contact's)
            if (c.kind == Contact::Gate && a_nodeDomain[std::size_t(node)] >= 0 && c.nodes.empty())
                a_s.log << tr("Warning: %1 touches a semiconductor: a gate is on an insulator (a metal on a semiconductor is a Schottky "
                              "or ohmic contact); its potential is fixed there")
                               .arg(bf->name());
            if ((c.kind == Contact::Ohmic || c.kind == Contact::Schottky) && a_nodeDomain[std::size_t(node)] < 0) continue;
            a_nodeContact[std::size_t(node)] = index;
            c.nodes.push_back(node);
            c.length.push_back(len);
        }
        if (c.nodes.empty()) problem(tr("%1 is on no boundary of %2's %3").arg(bf->name(), a_node->name(),
                                                                                c.kind == Contact::Gate ? tr("domains") : tr("semiconductors")));
        a_contacts.push_back(c);
    }
    // A surface's recombination at an ohmic contact: of its minority carriers
    // alone there (its majority off), freed; of both, nothing (the contact
    // holds them at equilibrium). Off elsewhere: refused - a sink of one
    // carrier where no contact takes the other (the Lumerical advice:
    // "on" at an insulator's and a semiconductor's interface).
    a_minorityFree.assign(std::size_t(a_n), 0);
    for (const auto& [from, to, name] : surfaceNames) {
        int atContacts = 0, minority = 0, elsewhere = 0, schottky = 0;
        for (int k = from; k < to; ++k) {
            TrapLevel& tl = a_traps[std::size_t(k)];
            const int ci = a_nodeContact[std::size_t(tl.node)];
            const bool ohmic = ci >= 0 && a_contacts[std::size_t(ci)].kind == Contact::Ohmic;
            if (ci >= 0 && a_contacts[std::size_t(ci)].kind == Contact::Schottky) {
                tl.cn = tl.cp = 0;   // (its thermionic velocities stand)
                ++schottky;
                continue;
            }
            if (ohmic) {
                ++atContacts;
                if (tl.minorityOnly) {
                    a_minorityFree[std::size_t(tl.node)] = 1;
                    ++minority;
                } else
                    tl.cn = tl.cp = 0;
            } else if (tl.minorityOnly)
                ++elsewhere;
        }
        if (elsewhere > 0)
            problem(tr("%1: its minority carriers alone (Apply to majority carriers off) at an ohmic contact only - %2 of its nodes are on "
                       "none: at an insulator's or another semiconductor's interface both carriers recombine (on)")
                        .arg(name)
                        .arg(elsewhere));
        else if (atContacts > minority)
            a_s.log << tr("%1: at its ohmic contacts the carriers held at equilibrium, its recombination none there (Apply to majority "
                          "carriers off: their minority carriers' by it)")
                           .arg(name);
        else if (minority > 0)
            a_s.log << tr("%1: the minority carriers of %2 contact nodes by its recombination, the majority held at equilibrium").arg(name).arg(minority);
        if (schottky > 0)
            a_s.log << tr("%1: its recombination none at a Schottky contact's %2 nodes - the contact's thermionic velocities its").arg(name).arg(schottky);
    }
    bool carriersHeld = false;
    for (const Contact& c : a_contacts) carriersHeld = carriersHeld || c.kind != Contact::Gate;
    if (!carriersHeld && referenced) problem(tr("%1 has no metal contact on a semiconductor (ohmic or Schottky): its carriers' level is not set").arg(a_node->name()));
    if (cancelled()) {
        *error = tr("cancelled");
        return false;
    }
    // The unknowns: V where the physics is, n and p on its semiconductors.
    a_iV.assign(std::size_t(a_n), -1);
    a_iN.assign(std::size_t(a_n), -1);
    a_iP.assign(std::size_t(a_n), -1);
    a_unknowns = 0;
    for (int i = 0; i < a_n; ++i)
        if (nodeOn[std::size_t(i)]) a_iV[std::size_t(i)] = a_unknowns++;
    for (int i = 0; i < a_n; ++i)
        if (a_nodeDomain[std::size_t(i)] >= 0) {
            a_iN[std::size_t(i)] = a_unknowns++;
            a_iP[std::size_t(i)] = a_unknowns++;
        }
    // (A laser's photons: the last; then each circuit's contact's voltage;
    // then the other slices'.)
    a_sliceSize = a_unknowns;
    a_iS = a_laser ? a_unknowns++ : -1;
    for (Contact& c : a_contacts) c.iC = c.circuit != Contact::Held ? a_unknowns++ : -1;
    a_total = a_unknowns + (a_slices - 1) * a_sliceSize;
    if (a_slices > 1) {
        // (Slices are cross-sections the light alone tells apart: what one
        // solves across them all - a laser's photons, quantum states, the
        // recombination stimulated by a threshold - is not cut.)
        if (a_laser) problem(tr("%1: %2 lases as a whole - its Optical Generation's slices 1").arg(a_node->name(), a_laser->name));
        if (a_quantumElectrons || a_quantumHoles) problem(tr("%1: its Schrödinger-Poisson states are not cut into slices - its Optical Generation's slices 1").arg(a_node->name()));
        if (!a_stimulated.empty()) problem(tr("%1: its stimulated recombination is not cut into slices - its Optical Generation's slices 1").arg(a_node->name()));
        if (a_s.axisymmetric()) problem(tr("%1: about an axis it has no length to cut into slices - its Optical Generation's slices 1").arg(a_node->name()));
    }
    a_bcV.assign(std::size_t(a_n), 0);
    a_bcN.assign(std::size_t(a_n), 0);
    a_bcP.assign(std::size_t(a_n), 0);
    // The field's constant parts: the doping; what the variables read.
    // (A laser's: each node's gain and stimulated rate after, then S.)
    // (In one go: a step after another keeps the values it has - cut and
    // grown again, the ions' were nought under a solution kept, a step in
    // time's history then none of them: a 10 ps ramp did not converge.)
    const std::size_t base = std::size_t(a_laser ? 9 : 7) * std::size_t(a_n) + (a_laser ? 1 : 0);
    a_ionAt = a_incomplete ? base : 0;
    // (Then the pairs per second the solution was solved with - a walk to
    // it ramps the light from there -, and the circuits': each contact's own
    // voltage, a bias tee's current at rest and the resistance at rest it
    // was solved with - their history a step in time's, as the carriers'.)
    bool circuits = false;
    for (const Contact& c : a_contacts) circuits = circuits || c.iC >= 0;
    a_lightAt = base + (a_incomplete ? 2 * std::size_t(a_n) : 0);
    a_circuitAt = circuits ? a_lightAt + 1 : 0;
    a_sliceBlock = (a_incomplete ? 5 : 3) * std::size_t(a_n);
    a_slicesAt = a_lightAt + 1 + (circuits ? 3 * a_contacts.size() : 0);
    f.values.resize(a_slicesAt + std::size_t(a_slices - 1) * a_sliceBlock, 0.0);
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(3 * a_n + i)] = a_Nd[std::size_t(i)];
        f.values[std::size_t(4 * a_n + i)] = a_Na[std::size_t(i)];
        f.values[std::size_t(5 * a_n + i)] = a_thetaN[std::size_t(i)];
        f.values[std::size_t(6 * a_n + i)] = a_thetaP[std::size_t(i)];
    }
    // (The layout: ExtraHead's.)
    f.extra.assign(std::size_t(ExtraHead + PerTriangle * nt), NaN);
    f.extra[0] = a_Vt;
    f.extra[1] = a_T;
    f.extra[2] = a_refNi;
    for (int t = 0; t < nt; ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        double* e = &f.extra[std::size_t(ExtraHead + PerTriangle * t)];
        const int* tdof = sp.dofs(t);
        e[0] = mun[std::size_t(t)];
        e[1] = mup[std::size_t(t)];
        // (Its lifetimes: its corners' mean, by doping and of x and y; its
        // trap-assisted tunnelling's FΓ, 0 none.)
        double tn = Inf, tp = Inf;
        if (m.srh) {
            tn = tp = 0;
            for (int k = 0; k < 3; ++k) {
                tn += a_tauN[std::size_t(tdof[k])] / 3;
                tp += a_tauP[std::size_t(tdof[k])] / 3;
            }
        }
        e[5] = tn;
        e[6] = tp;
        e[22] = m.srh && m.tat ? m.FGn : 0.0;
        e[23] = m.srh && m.tat ? m.FGp : 0.0;
        e[7] = m.Et;
        e[8] = m.Cn;
        e[9] = m.Cp;
        e[10] = m.B;
        e[11] = (m.chi + m.dEc - a_refChi - a_refDEc) / Q;
        e[12] = m.incomplete ? m.n1d : Inf;
        e[13] = m.gd;
        e[14] = m.incomplete ? m.p1a : Inf;
        e[15] = m.ga;
        e[16] = m.fermi ? m.Nc : 0.0;
        e[17] = m.fermi ? m.Nv : 0.0;
        e[18] = 0;
        e[32] = 0;   // (impact ionization's pairs per volume, as solved)
        e[33] = NaN; // (sliced: the heat the carriers leave, as solved - their mean)
        e[19] = m.tunnelling ? (m.btGeneration ? -m.btA : m.btA) : 0.0;   // (negative: generation alone)
        e[20] = m.btB;
        e[21] = m.btGamma;
    }
    bandExtras();
    Solution::Coefficients& coef = a_s.coefficients()[std::size_t(a_fi)];
    coef.c.assign(topo.domains.size(), Expression());
    for (int d : domains) coef.c[std::size_t(d)] = Expression::compile(QString::number(a_media[std::size_t(d)].epsr, 'g', 17), a_s.pointScope());
    // Contacts as the field's terminals (their applied voltages kept with it).
    if (f.terminals.size() != a_contacts.size()) {
        f.terminals.clear();
        for (const Contact& c : a_contacts) {
            Field::Terminal term;
            term.name = c.name;
            f.terminals.push_back(term);
        }
    }
    // (Kept with them: the voltage applied, the current, the metal's charge;
    // a circuit's contact's own voltage and its tee's current at rest.)
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        Contact& c = a_contacts[k];
        if (f.terminals[k].name == c.name) {
            c.applied = f.terminals[k].potential;
            c.current = f.terminals[k].charge;
            c.charge = f.terminals[k].chargeIm;
        }
        c.voltage = c.applied;
        if (c.iC >= 0 && f.solved && f.values.size() >= a_circuitAt + 3 * a_contacts.size()) {
            c.voltage = f.values[a_circuitAt + 3 * k];
            c.teeCurrent = f.values[a_circuitAt + 3 * k + 1];
            c.solvedR = f.values[a_circuitAt + 3 * k + 2];
        }
    }
    // Solved in 1D: its line across its domains (no gap in it), and what a
    // strip of its own cannot take.
    a_1d = a_node->text(QStringLiteral("space")) == QLatin1String("1d");
    if (a_1d) {
        a_axis1d = a_node->text(QStringLiteral("axis")) == QLatin1String("x") ? 0 : 1;
        if (a_s.axisymmetric()) problem(tr("%1: solved in 1D in a plane's model alone (not about an axis)").arg(a_node->name()));
        if (a_laser) problem(tr("%1: %2 lases in its cavity's 2D solution - not solved in 1D").arg(a_node->name(), a_laser->name));
        if (a_TL.isValid()) problem(tr("%1: solved in 1D it takes no lattice temperature (TL)").arg(a_node->name()));
        if (circuits) problem(tr("%1: solved in 1D its contacts are held at their voltages - no circuit").arg(a_node->name()));
        if (a_slices > 1) problem(tr("%1: solved in 1D it is not cut along its length into slices").arg(a_node->name()));
        for (const Node& feature : a_node->children) {
            if (!feature.enabled) continue;
            if (feature.type == QLatin1String("semiconductor/optical") || feature.type == QLatin1String("semiconductor/quantum"))
                problem(tr("%1: solved in 1D it takes no %2 (another physics' field across its domains)").arg(a_node->name(), feature.name()));
        }
        const std::optional<double> h = evaluateConstant(a_node->text(QStringLiteral("h1d")), constants, Dim::length(), &why);
        if (!h || !(*h > 0)) {
            problem(tr("%1: its line's element size: %2").arg(a_node->name(), h ? tr("it must be more than 0") : why));
        } else {
            a_h1d = *h;
            QString lineWhy;
            if (!buildLine(a_s, f.domains, a_axis1d, a_node->text(QStringLiteral("at")), a_h1d, 0, &a_line, &lineWhy)) {
                problem(tr("%1: its line: %2").arg(a_node->name(), lineWhy));
            } else {
                for (int k = 0; k < a_line.elements(); ++k)
                    if (a_line.domain[std::size_t(k)] < 0) {
                        problem(tr("%1: its line leaves its domains between %2 and %3 µm along it: a 1D one is across them all, "
                                   "without a gap")
                                    .arg(a_node->name(), formatNumber(a_line.z[std::size_t(2 * k)] * 1e6, 4),
                                         formatNumber(a_line.z[std::size_t(2 * k + 2)] * 1e6, 4)));
                        break;
                    }
            }
        }
    }
    if (!a_problems.isEmpty()) {
        *error = a_problems.join(QLatin1Char('\n'));
        return false;
    }
    if (a_clamped > 0) {
        const QString line = tr("%1: %2 of %3 edges not Delaunay (their currents left out)").arg(a_node->name()).arg(a_clamped).arg(a_edges.size());
        if (!a_s.log.contains(line)) a_s.log << line;
    }
    return true;
}

void SemiconductorProblem::neutral(int i, double* n, double* p, double* v) const
{
    const double C = doping(i), ni = a_ni[std::size_t(i)];
    const double half = std::abs(C) / 2;
    const double major = half + std::sqrt(half * half + ni * ni);
    const double minor = ni * ni / major;
    *n = C >= 0 ? major : minor;
    *p = C >= 0 ? minor : major;
    if (v) *v = std::log(*n / ni);
    const int d = a_nodeDomain[std::size_t(i)];
    if (d < 0 || (!a_media[std::size_t(d)].incomplete && !a_media[std::size_t(d)].fermi)) return;
    // Fermi-Dirac's carriers, or the dopants ionized in part: p - n + Nd⁺ -
    // Na⁻ = 0, falling as the potential v rises - halved to a double's
    // digits. (The dopants' occupation by the Fermi level: by Boltzmann's
    // ni e^v, whatever the carriers' statistics.)
    auto charge = [&](double u) {
        double nu = 0, pu = 0, dn = 0, dp = 0, nd = 0, dnd = 0, na = 0, dna = 0;
        carriersAt(i, u, &nu, &pu, &dn, &dp);
        ionized(i, ni * std::exp(std::clamp(u, -600.0, 600.0)), ni * std::exp(std::clamp(-u, -600.0, 600.0)), &nd, &dnd, &na, &dna);
        return pu - nu + nd - na;
    };
    const double v0 = std::asinh(C / (2 * ni));
    double lo = v0 - 1, hi = v0 + 1;
    while (charge(lo) < 0 && lo > -600) lo -= 10;
    while (charge(hi) > 0 && hi < 600) hi += 10;
    for (int k = 0; k < 200 && hi - lo > 1e-15 * std::max(1.0, std::abs(lo)); ++k) {
        const double mid = (lo + hi) / 2;
        (charge(mid) > 0 ? lo : hi) = mid;
    }
    const double u = (lo + hi) / 2;
    double dn = 0, dp = 0;
    carriersAt(i, u, n, p, &dn, &dp);
    if (v) *v = u;
}

void SemiconductorProblem::carriersAt(int i, double v, double* n, double* p, double* dn, double* dp) const
{
    const double ni = a_ni[std::size_t(i)];
    v = std::clamp(v, -600.0, 600.0);
    // (Schrödinger-Poisson's quantum potentials: each carrier e^(Λ/Vt).)
    *n = ni * std::exp(std::clamp(v + a_lamN[std::size_t(i)] / a_Vt, -600.0, 600.0));
    *p = ni * std::exp(std::clamp(-v + a_lamP[std::size_t(i)] / a_Vt, -600.0, 600.0));
    *dn = *n;
    *dp = -*p;
    const int d = a_nodeDomain[std::size_t(i)];
    if (d < 0 || !a_media[std::size_t(d)].fermi) return;
    // n = Nc ℱ(η), Boltzmann's Nc e^η = ni e^v.
    const Medium& m = a_media[std::size_t(d)];
    const FermiHalf& fh = fermiHalf();
    double F = 0, dF = 0;
    fh.at(std::log(*n / m.Nc), &F, &dF);
    *n = m.Nc * F;
    *dn = m.Nc * dF;
    fh.at(std::log(*p / m.Nv), &F, &dF);
    *p = m.Nv * F;
    *dp = -m.Nv * dF;
}

double SemiconductorProblem::quantumCorrection(QString* error)
{
    (void)error;
    if (a_quantumNodes.empty()) return 0;
    // Each node's quantum potential moved by Vt ln(n_states/n): the
    // semiconductor's carriers there the states' next time - its carriers
    // e^(Λ/Vt) Boltzmann's, so Poisson's equation solved with them takes the
    // states' as rising e^(δV/Vt) with the potential (Trellakis's predictor
    // and corrector). (A step no more than 3 Vt, Λ within ±30 Vt; where the
    // states have almost none - ψ nought at an oxide - floored at 1e-8 of
    // their most, not counted below 1e-6 of it.) Once the steps are under
    // Vt, Anderson's mixing of the last five: the plain steps crawled at 0.96
    // a turn, the states' shape answering the potential's, not each node's.
    const Field& f = field();
    const std::size_t N = std::size_t(a_n);
    PointEvaluator ev(a_s);
    std::vector<double> nq(a_quantumNodes.size(), NaN), pq(a_quantumNodes.size(), NaN);
    double mostN = 0, mostP = 0;
    for (std::size_t k = 0; k < a_quantumNodes.size(); ++k) {
        const auto& [t, r] = a_nodePoint[std::size_t(a_quantumNodes[k])];
        ev.moveTo(t, r[0], r[1]);
        if (a_quantumElectrons && a_quantumN.isValid()) mostN = std::max(mostN, nq[k] = ev.eval(a_quantumN));
        if (a_quantumHoles && a_quantumP.isValid()) mostP = std::max(mostP, pq[k] = ev.eval(a_quantumP));
    }
    // The entries: each quantum node's Λn and Λp, their steps, and whether
    // their carriers count.
    std::vector<double*> lam;
    std::vector<double> step;
    std::vector<char> counted;
    auto entry = [&](double target, double now, double most, double* l) {
        if (!std::isfinite(target) || !(now > 0) || !(most > 0)) return;
        lam.push_back(l);
        step.push_back(std::clamp(a_Vt * std::log(std::max(target, 1e-8 * most) / now), -3 * a_Vt, 3 * a_Vt));
        counted.push_back(std::max(target, now) > 1e-6 * most);
    };
    for (std::size_t k = 0; k < a_quantumNodes.size(); ++k) {
        const std::size_t i = std::size_t(a_quantumNodes[k]);
        if (a_quantumElectrons) entry(nq[k], f.values[N + i], mostN, &a_lamN[i]);
        if (a_quantumHoles) entry(pq[k], f.values[2 * N + i], mostP, &a_lamP[i]);
    }
    const int m = int(lam.size());
    double largest = 0;
    for (int j = 0; j < m; ++j)
        if (counted[std::size_t(j)]) largest = std::max(largest, std::abs(step[std::size_t(j)]));
    Eigen::VectorXd x(m), g(m);
    for (int j = 0; j < m; ++j) {
        x(j) = *lam[std::size_t(j)] / a_Vt;
        g(j) = counted[std::size_t(j)] ? step[std::size_t(j)] / a_Vt : 0.0;
    }
    Eigen::VectorXd next = x;
    for (int j = 0; j < m; ++j) next(j) += step[std::size_t(j)] / a_Vt;
    if (largest < a_Vt && a_andX.size() > 0 && a_andX.back().size() == m) {
        // Anderson: x + g - (ΔX + ΔG) γ, γ the least squares' of ΔG γ = g.
        const int depth = int(a_andX.size());
        Eigen::MatrixXd dX(m, depth), dG(m, depth);
        for (int c = 0; c < depth; ++c) {
            const Eigen::VectorXd& x1 = c + 1 < depth ? a_andX[std::size_t(c + 1)] : x;
            const Eigen::VectorXd& g1 = c + 1 < depth ? a_andG[std::size_t(c + 1)] : g;
            dX.col(c) = x1 - a_andX[std::size_t(c)];
            dG.col(c) = g1 - a_andG[std::size_t(c)];
        }
        const Eigen::VectorXd gamma = dG.colPivHouseholderQr().solve(g);
        if (gamma.allFinite()) {
            const Eigen::VectorXd mixed = x + g - (dX + dG) * gamma;
            for (int j = 0; j < m; ++j)
                if (counted[std::size_t(j)]) next(j) = x(j) + std::clamp(mixed(j) - x(j), -3.0, 3.0);
        }
    }
    if (largest < a_Vt) {
        a_andX.push_back(x);
        a_andG.push_back(g);
        if (a_andX.size() > 5) {
            a_andX.erase(a_andX.begin());
            a_andG.erase(a_andG.begin());
        }
    } else {
        a_andX.clear();
        a_andG.clear();
    }
    double change = 0;
    for (int j = 0; j < m; ++j) {
        const double before = *lam[std::size_t(j)];
        *lam[std::size_t(j)] = std::clamp(next(j) * a_Vt, -30 * a_Vt, 30 * a_Vt);
        if (counted[std::size_t(j)]) change = std::max(change, std::abs(*lam[std::size_t(j)] - before) / a_Vt);
    }
    for (std::size_t i = 0; i < N; ++i) {
        a_thetaN[i] = a_thetaN0[i] + a_lamN[i];
        a_thetaP[i] = a_thetaP0[i] - a_lamP[i];
    }
    // (Its measure of rest: the plain step's, how far the carriers are from
    // the states'.)
    return largest / a_Vt;
}

SemiconductorProblem::TrapState SemiconductorProblem::trapState(const TrapLevel& t, double n, double p)
{
    TrapState s;
    s.D = t.cn * (n + t.n1) + t.cp * (p + t.p1);
    const double filling = t.cn * n + t.cp * t.p1;
    s.f = filling / s.D;
    s.dfdn = t.cn * (t.cn * t.n1 + t.cp * p) / (s.D * s.D);
    s.dfdp = -t.cp * filling / (s.D * s.D);
    const double excess = n * p - t.n1 * t.p1;
    s.U = t.N * t.cn * t.cp * excess / s.D;
    s.dUdn = t.N * t.cn * t.cp * (p * s.D - excess * t.cn) / (s.D * s.D);
    s.dUdp = t.N * t.cn * t.cp * (n * s.D - excess * t.cp) / (s.D * s.D);
    return s;
}

void SemiconductorProblem::degeneracy(const Eigen::VectorXd& x) const
{
    if (!a_fermi) return;
    const FermiHalf& fh = fermiHalf();
    // γ = u/e^η at u = n/Nc: ln γ = ln u - η, d ln γ/dn = (1/u - 1/ℱ')/Nc;
    // few carriers, ln γ = -u/2^1.5 (Boltzmann's next term) - its derivative
    // there not a difference of 1/u's.
    auto one = [&](double c, double N, double* lg, double* dlg, double* r) {
        const double u = std::max(c, 0.0) / N;
        if (u < 1e-6) {
            *lg = -u / std::sqrt(8.0);
            *dlg = -1 / (std::sqrt(8.0) * N);
            *r = 1 - u / std::sqrt(8.0);
            return;
        }
        const double eta = fh.inverse(u);
        double F = 0, dF = 0;
        fh.at(eta, &F, &dF);
        *lg = std::log(u) - eta;
        *dlg = (1 / u - 1 / dF) / N;
        *r = dF / F;
    };
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0 || !a_media[std::size_t(d)].fermi) continue;
        const Medium& m = a_media[std::size_t(d)];
        const std::size_t k = std::size_t(i);
        one(x(a_iN[k]), m.Nc, &a_lgN[k], &a_dlgN[k], &a_rN[k]);
        one(x(a_iP[k]), m.Nv, &a_lgP[k], &a_dlgP[k], &a_rP[k]);
    }
}

void SemiconductorProblem::ionized(int i, double n, double p, double* nd, double* dnd, double* na, double* dna) const
{
    const double Nd = a_Nd[std::size_t(i)], Na = a_Na[std::size_t(i)];
    const int d = a_nodeDomain[std::size_t(i)];
    if (d < 0 || !a_media[std::size_t(d)].incomplete) {
        *nd = Nd;
        *na = Na;
        *dnd = *dna = 0;
        return;
    }
    const Medium& m = a_media[std::size_t(d)];
    const double un = 1 + m.gd * n / m.n1d, up = 1 + m.ga * p / m.p1a;
    *nd = Nd / un;
    *dnd = -Nd * m.gd / m.n1d / (un * un);
    *na = Na / up;
    *dna = -Na * m.ga / m.p1a / (up * up);
}

bool SemiconductorProblem::targets(std::vector<double>& out, QString* error)
{
    out.assign(a_contacts.size(), 0);
    PointEvaluator ev(a_s);
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        if (!c.V0.isValid()) {
            *error = tr("%1: the voltage of %2 is not valid").arg(a_node->name(), c.name);
            return false;
        }
        double v = c.V0.isConstant() ? c.V0.constant() : NaN;
        if (const auto held = a_held.find(c.name); held != a_held.end()) v = held->second;
        else if (!c.V0.isConstant()) {
            const auto& [t, r] = a_nodePoint[std::size_t(c.nodes.front())];
            ev.moveTo(t, r[0], r[1]);
            v = ev.eval(c.V0);
        }
        if (!std::isfinite(v)) {
            *error = tr("%1: the voltage of %2 is not a number").arg(a_node->name(), c.name);
            return false;
        }
        out[k] = v;
    }
    return true;
}

void SemiconductorProblem::contactValues(const std::vector<double>& applied)
{
    a_lastApplied = applied;
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        for (int i : c.nodes) {
            const double va = applied[k];
            if (c.kind == Contact::Gate) {
                // The metal's Fermi level at -q Va, its vacuum level Φm above.
                a_bcV[std::size_t(i)] = va - (c.phim - a_refChi - a_refDEc) / Q;
                continue;
            }
            const Medium& m = a_media[std::size_t(a_nodeDomain[std::size_t(i)])];
            if (c.kind == Contact::Schottky) {
                // (Its barrier to the band's edge as narrowed.)
                a_bcV[std::size_t(i)] = va + (m.dEc - a_gap[std::size_t(i)] / 2 - c.phiB) / Q - a_delta[std::size_t(i)];
                continue;
            }
            double n = 0, p = 0, v = 0;
            neutral(i, &n, &p, &v);
            a_bcN[std::size_t(i)] = n;
            a_bcP[std::size_t(i)] = p;
            a_bcV[std::size_t(i)] = va + a_Vt * v - a_delta[std::size_t(i)];
        }
    }
}

void SemiconductorProblem::edgeWeights(const Eigen::VectorXd* x)
{
    for (Edge& e : a_edges) e.wn = e.wp = e.muN = e.muP = 0;
    if (a_Bz != 0) a_triMu.assign(a_elements.size(), {0.0, 0.0});
    std::vector<double> geometric(a_edges.size(), 0.0), magnitude(a_edges.size(), 0.0);
    // (Tunnelling: each emitting edge's field, its triangles' mean |E|.)
    std::vector<double> field(a_emission == 2 && x ? a_edges.size() : 0, 0.0), fields(field.size(), 0.0);
    for (int t = 0; t < int(a_elements.size()); ++t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0) continue;
        double mun = 0, mup = 0;
        elementMobilities(t, x, &mun, &mup);
        // (A magnetic field: the current along the field's own, J0/(1 + c²);
        // its turned part ẑ×J0 c/(1 + c²) in hallFluxes.)
        if (a_Bz != 0) {
            a_triMu[std::size_t(t)] = {mun, mup};
            const Medium& m = a_media[std::size_t(em.domain)];
            const double cn = m.rHn * mun * a_Bz, cp = m.rHp * mup * a_Bz;
            mun /= 1 + cn * cn;
            mup /= 1 + cp * cp;
        }
        double E = 0;
        if (!field.empty()) {
            const int* dof = a_s.fields()[std::size_t(a_fi)].space->dofs(t);
            double ex = 0, ey = 0;
            for (int k = 0; k < 3; ++k) {
                ex -= em.gx[k] * (*x)(a_iV[std::size_t(dof[k])]);
                ey -= em.gy[k] * (*x)(a_iV[std::size_t(dof[k])]);
            }
            E = std::hypot(ex, ey);
        }
        for (int k = 0; k < 3; ++k) {
            Edge& e = a_edges[std::size_t(em.edge[k])];
            e.wn += em.cot[k] * mun;
            e.wp += em.cot[k] * mup;
            e.muN = std::max(e.muN, mun);
            e.muP = std::max(e.muP, mup);
            geometric[std::size_t(em.edge[k])] += em.cot[k];
            magnitude[std::size_t(em.edge[k])] += std::abs(em.cot[k]) + em.half;
            if (!field.empty() && (e.vRn > 0 || e.vRp > 0)) {
                field[std::size_t(em.edge[k])] += E;
                fields[std::size_t(em.edge[k])] += 1;
            }
        }
    }
    // (An edge along a heterojunction whose one side's weight is negative -
    // an obtuse angle across it -: the two sides' together, on the other's
    // carriers, as an edge of one.)
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        Edge& e = a_edges[k];
        if (e.partner < 0 || !(geometric[k] < 0)) continue;
        const std::size_t o = std::size_t(e.partner);
        geometric[o] += geometric[k];
        magnitude[o] += magnitude[k];
        a_edges[o].wn += e.wn;
        a_edges[o].wp += e.wp;
        a_edges[o].muN = std::max(a_edges[o].muN, e.muN);
        a_edges[o].muP = std::max(a_edges[o].muP, e.muP);
        geometric[k] = magnitude[k] = e.wn = e.wp = 0;
    }
    a_clamped = 0;
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        Edge& e = a_edges[k];
        // (Not Delaunay there: a negative weight would make the carriers
        // oscillate; it is left out. One of nought - a right angle across
        // a boundary's edge - is as it should be: its rounding, -3e-17 of
        // a cotangent, is measured against the cotangents and the weight
        // both, not against the cotangents alone - nought there too.)
        if (geometric[k] < -1e-9 * magnitude[k]) ++a_clamped;
        e.w = std::max(geometric[k], 0.0);
        e.wn = e.w > 0 ? std::max(e.wn, 0.0) * Q * a_Vt : 0.0;
        e.wp = e.w > 0 ? std::max(e.wp, 0.0) * Q * a_Vt : 0.0;
        e.muN *= Q * a_Vt;
        e.muP *= Q * a_Vt;
        // An edge into a heterojunction's other side: the step's emission,
        // G = q v_R (1 + δ) over the edge's face across the interface, in
        // series at its end there with the edge's Scharfetter and Gummel
        // flux - the carriers crossing it no faster than they arrive at it
        // thermally (Crowell and Sze's thermionic emission and diffusion),
        // δ what tunnels through it. The two together the edge's flux times
        // G/(G + w B), B its Bernoulli toward that end at the step d: of x,
        // as solved (held a Newton step, as saturated mobilities are; 1
        // without it).
        if (e.w > 0 && (e.vRn > 0 || e.vRp > 0)) {
            const double face = e.w * e.h * e.across;
            const double F = !field.empty() && fields[k] > 0 ? field[k] / fields[k] : 0.0;
            double dN = 0, dP = 0;
            if (x) edgeSteps(e, *x, &dN, &dP);
            if (e.wn > 0 && e.vRn > 0) {
                const double G = Q * e.vRn * (1 + (field.empty() ? 0.0 : tunnelling(F, e.stepN, e.massN, KB * a_T))) * face;
                e.wn *= G / (G + e.wn * bern(e.emitsAtI ? -dN : dN));
            }
            if (e.wp > 0 && e.vRp > 0) {
                const double G = Q * e.vRp * (1 + (field.empty() ? 0.0 : tunnelling(F, e.stepP, e.massP, KB * a_T))) * face;
                e.wp *= G / (G + e.wp * bern(e.emitsAtI ? dP : -dP));
            }
        }
    }
}

void SemiconductorProblem::lowFieldMobilities(const Medium& m, const ElementMobility& em, double T, double n, double p, double Fn, double Fp,
                                              double perp, double* mun, double* mup) const
{
    switch (m.lowField) {
    case Medium::LowField::Dorkel:
        *mun = dorkelMobility(m.dl[0], em.N, n, p, T);
        *mup = dorkelMobility(m.dl[1], em.N, n, p, T);
        break;
    case Medium::LowField::Klaassen:
        *mun = klaassenMobility(m, 0, em.Nd, em.Na, n, p, T, em.pmin[0]);
        *mup = klaassenMobility(m, 1, em.Nd, em.Na, n, p, T, em.pmin[1]);
        break;
    case Medium::LowField::Yamaguchi: {
        // Its low-field mobility (em's), the field across the interface's
        // (faded from it), the force along the current's: µs = µ0 (1 + as D
        // E⊥)^-½, µ = µs (1 + x²/(x + G) + (µs E∥/vs)²)^-½, x = µs E∥/uL.
        auto yamaguchi = [&em, perp](double mu0, const double* c, double F) {
            const double ms = mu0 / std::sqrt(1 + c[3] * em.damp * perp);
            const double u = ms * F / c[4], v = ms * F / c[5];
            return ms / std::sqrt(1 + (u > 0 ? u * u / (u + c[6]) : 0.0) + v * v);
        };
        *mun = yamaguchi(em.mun, m.ya[0], Fn);
        *mup = yamaguchi(em.mup, m.ya[1], Fp);
        break;
    }
    case Medium::LowField::Expression: {
        std::vector<double> v(std::size_t(std::max(a_exprSlots, 1)), 0.0);
        auto set = [&v](int slot, double value) {
            if (slot >= 0 && slot < int(v.size())) v[std::size_t(slot)] = value;
        };
        set(a_slotN, em.N);
        set(a_slotNd, em.Nd);
        set(a_slotNa, em.Na);
        set(a_slotT, T);
        set(a_slotn, n);
        set(a_slotp, p);
        set(a_slotEperp, perp);
        set(a_slotD, em.dist < Inf ? em.dist : 1.0);
        set(a_slotEpar, Fn);
        const double en = m.muExpr[0].isValid() ? m.muExpr[0].eval(v.data()) : 0.0;
        set(a_slotEpar, Fp);
        const double ep = m.muExpr[1].isValid() ? m.muExpr[1].eval(v.data()) : 0.0;
        // (Positive: a mobility of nought or less, or not a number, a
        // millionth of a cm²/(V s).)
        *mun = std::isfinite(en) && en > 1e-10 ? en : 1e-10;
        *mup = std::isfinite(ep) && ep > 1e-10 ? ep : 1e-10;
        break;
    }
    default:
        break;
    }
}

void SemiconductorProblem::hallFluxes(const Eigen::VectorXd& x, std::vector<double>& flux, std::vector<Eigen::Triplet<double>>* d) const
{
    flux.assign(2 * a_edges.size(), 0.0);
    if (d) d->clear();
    if (a_Bz == 0 || a_triMu.size() != a_elements.size()) return;
    auto nn = [&](int i) { return a_iN[std::size_t(i)] >= 0 ? x(a_iN[std::size_t(i)]) : 0.0; };
    auto pp = [&](int i) { return a_iP[std::size_t(i)] >= 0 ? x(a_iP[std::size_t(i)]) : 0.0; };
    for (int t = 0; t < int(a_elements.size()); ++t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0 || em.step) continue;
        const Medium& m = a_media[std::size_t(em.domain)];
        // Its current density without the field, fitted to its three
        // edges' (each Scharfetter and Gummel's at its own mobility): J0 =
        // (Σ t tᵀ)⁻¹ Σ t j - exact where it is uniform; turned, ẑ×J0 =
        // (J0y, -J0x), through its share of each edge's face (its
        // cotangent's, signed: a uniform one's divergence nought).
        double M[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            const Edge& e = a_edges[std::size_t(em.edge[k])];
            M[0] += e.tx * e.tx;
            M[1] += e.tx * e.ty;
            M[2] += e.ty * e.ty;
        }
        const double det = M[0] * M[2] - M[1] * M[1];
        if (!(std::abs(det) > 1e-12)) continue;
        double wx[3], wy[3];
        for (int k = 0; k < 3; ++k) {
            const Edge& e = a_edges[std::size_t(em.edge[k])];
            wx[k] = (M[2] * e.tx - M[1] * e.ty) / det;
            wy[k] = (-M[1] * e.tx + M[0] * e.ty) / det;
        }
        for (int c = 0; c < 2; ++c) {
            const double mu = a_triMu[std::size_t(t)][std::size_t(c)], r = c == 0 ? m.rHn : m.rHp;
            const double cc = (c == 0 ? -1.0 : 1.0) * r * mu * a_Bz, share = cc / (1 + cc * cc);
            if (share == 0) continue;
            double j[3] = {0, 0, 0}, dj[3][4] = {};
            int col[3][4];
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                const int i = e.i, o = e.j;
                double dN = 0, dP = 0;
                edgeSteps(e, x, &dN, &dP);
                const double g = mu * Q * a_Vt / e.h;
                col[k][2] = a_iV[std::size_t(i)];
                col[k][3] = a_iV[std::size_t(o)];
                if (c == 0) {
                    const double bp = bern(dN), bm = bern(-dN);
                    j[k] = g * (e.fNj * nn(o) * bp - e.fNi * nn(i) * bm);
                    const double dv = g * (e.fNj * nn(o) * dbern(dN) + e.fNi * nn(i) * dbern(-dN)) / a_Vt;
                    col[k][0] = a_iN[std::size_t(i)];
                    col[k][1] = a_iN[std::size_t(o)];
                    dj[k][0] = -g * e.fNi * bm - dv * a_Vt * a_dlgN[std::size_t(i)];
                    dj[k][1] = g * e.fNj * bp + dv * a_Vt * a_dlgN[std::size_t(o)];
                    dj[k][2] = -dv;
                    dj[k][3] = dv;
                } else {
                    const double bp = bern(dP), bm = bern(-dP);
                    j[k] = g * (e.fPi * pp(i) * bp - e.fPj * pp(o) * bm);
                    const double dv = g * (e.fPi * pp(i) * dbern(dP) + e.fPj * pp(o) * dbern(-dP)) / a_Vt;
                    col[k][0] = a_iP[std::size_t(i)];
                    col[k][1] = a_iP[std::size_t(o)];
                    dj[k][0] = g * e.fPi * bp + dv * a_Vt * a_dlgP[std::size_t(i)];
                    dj[k][1] = -g * e.fPj * bm - dv * a_Vt * a_dlgP[std::size_t(o)];
                    dj[k][2] = -dv;
                    dj[k][3] = dv;
                }
            }
            double Jx = 0, Jy = 0;
            for (int k = 0; k < 3; ++k) {
                Jx += wx[k] * j[k];
                Jy += wy[k] * j[k];
            }
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                const double face = em.cot[k] * e.h;
                const std::size_t row = 2 * std::size_t(em.edge[k]) + std::size_t(c);
                flux[row] += share * (Jy * e.tx - Jx * e.ty) * face;
                if (!d) continue;
                for (int q = 0; q < 3; ++q) {
                    const double dH = share * (wy[q] * e.tx - wx[q] * e.ty) * face;
                    for (int u = 0; u < 4; ++u)
                        if (col[q][u] >= 0 && dj[q][u] != 0) d->emplace_back(int(row), col[q][u], dH * dj[q][u]);
                }
            }
        }
    }
}

void SemiconductorProblem::elementMobilities(int t, const Eigen::VectorXd* x, double* mun, double* mup) const
{
    const ElementMobility& em = a_elements[std::size_t(t)];
    *mun = em.mun * em.tn;
    *mup = em.mup * em.tp;
    const Medium& m = a_media[std::size_t(em.domain)];
    // (Its lattice's temperature: TL's where one is given.)
    const double TL = em.TL > 0 ? em.TL : a_T;
    const bool state = m.lowField == Medium::LowField::Dorkel || m.lowField == Medium::LowField::Klaassen
                       || m.lowField == Medium::LowField::Yamaguchi || m.lowField == Medium::LowField::Expression;
    if (!x) {
        // (Without a state: at rest - its carriers its dopants', no field.)
        if (state) lowFieldMobilities(m, em, TL, em.n0, em.p0, 0, 0, 0, mun, mup);
        return;
    }
    if (!m.saturation && !(m.surface && em.damp > 0) && !state) return;
    // (Lagged a Newton step: the field and the carriers at x.)
    const int* dof = a_s.fields()[std::size_t(a_fi)].space->dofs(t);
    double V[3], ex = 0, ey = 0;
    for (int k = 0; k < 3; ++k) {
        V[k] = (*x)(a_iV[std::size_t(dof[k])]);
        ex -= em.gx[k] * V[k];
        ey -= em.gy[k] * V[k];
    }
    // Each carrier's force along its current: its quasi-Fermi potential's
    // gradient, no more than the field (saturation's, below).
    auto alongCurrent = [this, x, dof, &V, &em](double whole, double* Fn, double* Fp) {
        double gn[2] = {0, 0}, gp[2] = {0, 0};
        for (int k = 0; k < 3; ++k) {
            const std::size_t i = std::size_t(dof[k]);
            const double n = std::max((*x)(a_iN[i]), 1e-300), p = std::max((*x)(a_iP[i]), 1e-300);
            const double phin = V[k] + a_thetaN[i] + a_Vt * a_lgN[i] - a_Vt * std::log(n / a_refNi);
            const double phip = V[k] + a_thetaP[i] - a_Vt * a_lgP[i] + a_Vt * std::log(p / a_refNi);
            gn[0] += em.gx[k] * phin;
            gn[1] += em.gy[k] * phin;
            gp[0] += em.gx[k] * phip;
            gp[1] += em.gy[k] * phip;
        }
        *Fn = std::min(whole, std::hypot(gn[0], gn[1]));
        *Fp = std::min(whole, std::hypot(gp[0], gp[1]));
    };
    if (state) {
        // Of its carriers (its corners' mean), the forces along their
        // currents and the field across the nearest interface: Dorkel and
        // Leturcq's, Klaassen's, Yamaguchi's (on its own: neither a
        // surface's scattering nor saturation's after it), an expression's.
        double n = 0, p = 0;
        for (int k = 0; k < 3; ++k) {
            n += std::max((*x)(a_iN[std::size_t(dof[k])]), 0.0) / 3;
            p += std::max((*x)(a_iP[std::size_t(dof[k])]), 0.0) / 3;
        }
        double Fn = 0, Fp = 0;
        if (m.lowField == Medium::LowField::Yamaguchi || m.lowField == Medium::LowField::Expression) alongCurrent(std::hypot(ex, ey), &Fn, &Fp);
        const double perp = em.dist < Inf ? std::abs(ex * em.nx + ey * em.ny) : 0.0;
        lowFieldMobilities(m, em, TL, n, p, Fn, Fp, perp, mun, mup);
        if (m.lowField == Medium::LowField::Yamaguchi) return;
    }
    if (m.surface && em.damp > 0 && m.shirahata) {
        // 1/µ = 1/µbulk + D (1/µE - 1/µE(0)), µE = µ0 (TL/300 K)^-θ / ((1 +
        // E⊥/E1)^P1 + (E⊥/E2)^P2): the field across the interface's
        // scattering more than none's - at none the bulk's. (The other
        // reading, (E⊥/E1)^P1 for (1 + E⊥/E1)^P1, cuts it by a quarter at
        // 63 V/m, its slope infinite at none.)
        const double across = std::abs(ex * em.nx + ey * em.ny), tn = TL / 300;
        auto shirahata = [&em, across, tn](double mu, const double* c) {
            const double base = c[0] * std::pow(tn, -c[1]);
            const double field = base / (std::pow(1 + across / c[2], c[4]) + std::pow(across / c[3], c[5]));
            return 1 / (1 / mu + em.damp * (1 / field - 1 / base));
        };
        *mun = shirahata(*mun, m.sh[0]);
        *mup = shirahata(*mup, m.sh[1]);
    } else if (m.surface && em.damp > 0) {
        // 1/µ = 1/µbulk + D/µac + D/µsr (Matthiessen's rule), D = e^(-d/lcrit);
        // E⊥ no less than 1 V/cm; T the lattice's.
        const double across = std::max(std::abs(ex * em.nx + ey * em.ny), 100.0);
        const double perCm3 = std::max(em.N * 1e-6, 1.0), vPerCm = across / 100;
        auto scatter = [&](double mu, double B, double C, double lambda, double delta) {
            const double ac = B / across + 1e-4 * C * std::pow(perCm3, lambda) / (TL * std::cbrt(vPerCm));
            const double sr = delta / (across * across);
            return 1 / (1 / mu + em.damp / ac + em.damp / sr);
        };
        *mun = scatter(*mun, m.sBn, m.sCn, m.sLn, m.sDn);
        *mup = scatter(*mup, m.sBp, m.sCp, m.sLp, m.sDp);
    }
    if (m.saturation && !em.step) {
        // µ(F) = µ0 / (1 + (µ0 F / vsat)^β)^(1/β), F the force along each
        // carrier's current: its quasi-Fermi potential's gradient, φn = V +
        // θn - Vt ln(n/ni), φp = V + θp + Vt ln(p/ni) - J/(q n µ), the field
        // along the current where it drifts, nought at equilibrium - no
        // more than the field: the field's work heats a carrier, and one
        // diffusing where there is little (a doped region's minority
        // carriers, their gradient steep) keeps its mobility. (Uncapped, the
        // photodiode example collected 98.5 % of its pairs, not 92.) Or the
        // whole field.
        // (E·J/|J| itself, J fitted to a triangle's three edge currents,
        // took an inversion layer's drift and diffusion across it - each
        // some 500 times the current along the channel, cancelling only in
        // all - for current: the channel's mobility halved.)
        const double whole = std::hypot(ex, ey);
        double Fn = whole, Fp = whole;
        if (!m.wholeField) alongCurrent(whole, &Fn, &Fp);
        // (Caughey and Thomas's, or the transferred electrons' - (µ0 +
        // (vsat/F)(F/E0)⁴)/(1 + (F/E0)⁴), GaAs's E0 4 kV/cm: past its peak
        // the velocity falls, the electrons in the heavier valleys.)
        auto reduce = [](double mu, double F, double vsat, double beta) { return mu / std::pow(1 + std::pow(mu * F / vsat, beta), 1 / beta); };
        const double mun0 = *mun, mup0 = *mup, vsn = m.vsatn * em.vn, vsp = m.vsatp * em.vp;
        auto electrons = [&m, mun0, vsn, &reduce](double F) {
            if (!m.transferred) return reduce(mun0, F, vsn, m.betan);
            const double r4 = std::pow(F / m.E0n, 4);
            return F > 0 ? (mun0 + vsn / F * r4) / (1 + r4) : mun0;
        };
        auto holes = [mup0, vsp, &m, &reduce](double F) { return reduce(mup0, F, vsp, m.betap); };
        // (Hot carriers: the field their temperature stands for, the one
        // that heats them so at rest and alone, µ(F) F² = (3/2) k (T - TL)/
        // (q τw) - the same as the field's where it is uniform; entering a
        // high field still cool, faster than saturated: overshoot.)
        auto heated = [](const std::function<double(double)>& mobility, double w) {
            if (!(w > 0)) return 0.0;
            double lo = 0, hi = 1e3;
            while (mobility(hi) * hi * hi < w && hi < 1e12) hi *= 4;
            for (int k = 0; k < 80; ++k) {
                const double mid = 0.5 * (lo + hi);
                (mobility(mid) * mid * mid < w ? lo : hi) = mid;
            }
            return 0.5 * (lo + hi);
        };
        const double TL = em.TL > 0 ? em.TL : a_T;
        if (m.energyN && a_Tn.size() == std::size_t(a_n)) {
            double T = 0;
            for (int k = 0; k < 3; ++k) T += a_Tn[std::size_t(dof[k])] / 3;
            Fn = heated(electrons, 1.5 * KB * (T - TL) / (Q * m.tauWn));
        }
        if (m.energyP && a_Tp.size() == std::size_t(a_n)) {
            double T = 0;
            for (int k = 0; k < 3; ++k) T += a_Tp[std::size_t(dof[k])] / 3;
            Fp = heated(holes, 1.5 * KB * (T - TL) / (Q * m.tauWp));
        }
        *mun = electrons(Fn);
        *mup = holes(Fp);
    }
}

std::vector<SemiconductorProblem::MobilityChange> SemiconductorProblem::mobilityChanges(const Eigen::VectorXd& x) const
{
    std::vector<MobilityChange> out;
    if (!a_varying) return out;
    const Space& sp = *a_s.fields()[std::size_t(a_fi)].space;
    Eigen::VectorXd y = x;
    for (int t = 0; t < int(a_elements.size()); ++t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0) continue;
        const Medium& m = a_media[std::size_t(em.domain)];
        // (A low-field mobility of the carriers or the fields: Dorkel and
        // Leturcq's, Klaassen's, Yamaguchi's, an expression of them.)
        const bool lowField = m.lowField == Medium::LowField::Dorkel || m.lowField == Medium::LowField::Klaassen
                              || m.lowField == Medium::LowField::Yamaguchi || (m.lowField == Medium::LowField::Expression && m.exprState);
        if ((!m.saturation || em.step) && !(m.surface && em.damp > 0) && !lowField) continue;
        const int* dof = sp.dofs(t);
        MobilityChange mc;
        mc.t = t;
        bool all = true;
        for (int k = 0; k < 3; ++k) {
            mc.col[k] = a_iV[std::size_t(dof[k])];
            mc.col[3 + k] = a_iN[std::size_t(dof[k])];
            mc.col[6 + k] = a_iP[std::size_t(dof[k])];
            all = all && mc.col[k] >= 0 && mc.col[3 + k] >= 0 && mc.col[6 + k] >= 0;
        }
        if (!all) continue;
        bool any = false;
        for (int c = 0; c < 9; ++c) {
            // (1e-7 Vt in the potential, of the carriers 1e-7 of theirs: 1e-7
            // Vt in their quasi-Fermi potential. 1e-4 Vt moved a weak
            // field's triangle by as much as its corners differ - its
            // differences the curvature of |∇φ|, not its slope: a junction's
            // conductances 7 % off, Gak not -Gaa.)
            const int col = mc.col[c];
            const double h = c < 3 ? 1e-7 * a_Vt : 1e-7 * std::abs(x(col));
            mc.dn[c] = mc.dp[c] = 0;
            if (!(h > 0)) continue;
            double n1 = 0, p1 = 0, n2 = 0, p2 = 0;
            y(col) = x(col) + h;
            elementMobilities(t, &y, &n1, &p1);
            y(col) = x(col) - h;
            elementMobilities(t, &y, &n2, &p2);
            y(col) = x(col);
            mc.dn[c] = (n1 - n2) / (2 * h);
            mc.dp[c] = (p1 - p2) / (2 * h);
            any = any || mc.dn[c] != 0 || mc.dp[c] != 0;
        }
        // Of the potential through its gradient alone: its three corners'
        // changes sum to nought - exactly, their differences' rounding taken
        // out (else the contacts raised together carry a current).
        const double meanN = (mc.dn[0] + mc.dn[1] + mc.dn[2]) / 3, meanP = (mc.dp[0] + mc.dp[1] + mc.dp[2]) / 3;
        for (int k = 0; k < 3; ++k) {
            mc.dn[k] -= meanN;
            mc.dp[k] -= meanP;
        }
        if (any) out.push_back(mc);
    }
    return out;
}

void SemiconductorProblem::updateMobilities(const Eigen::VectorXd& x)
{
    if (!(a_varying || a_emission > 0)) return;
    // (Fermi-Dirac's degeneracy at x, which a carrier's quasi-Fermi potential
    // reads: else that of the state last assembled - after a stationary
    // step another's, and a small signal's first frequency had other
    // mobilities than its next: an APD's response 4.44 at 100 MHz, 6.25 at
    // 100 MHz again, the Lumerical APD's review.)
    degeneracy(x);
    if (a_energy && a_energyActive) carrierTemperatures(x);
    edgeWeights(&x);
}

void SemiconductorProblem::carrierTemperatures(const Eigen::VectorXd& x)
{
    // Each carrier's energy balance by its nodes' boxes, its fluxes the
    // edges' as weighed now (their mobilities a Newton step behind):
    //   Σ S + (3/2) k n V (T - TL)/τw = what the field gives it,
    // S the energy each edge carries, the particles' (5/2) kT each and the
    // carriers' conduction of heat, κ = (5/2)(k²/q) µ n T (Wiedemann and
    // Franz's) - Scharfetter and Gummel's fitting for convection and
    // conduction together, D (B(-P) Ti - B(P) Tj), P its particles' (5/2) k
    // F over D; the field's gift each triangle's: its particles' flux,
    // fitted to its edges' (each over its face), times its band's slope,
    // over its volume - a third to each corner, V its corners' volume. (By
    // edge, each one's particles times its band's drop, half to each end,
    // summed to the device's J E exactly but not node by node: a uniform
    // bar's electrons 3 % cool in its middle, its mesh's arrangement's.)
    // Contacts and media without it at the lattice's temperature. (Linear
    // in T: κ's T and n the last's.)
    const std::size_t N = std::size_t(a_n);
    std::vector<double> TL(N, a_T);
    if (!a_nodeTL.empty())
        for (std::size_t i = 0; i < N; ++i)
            if (a_nodeTL[i] > 0) TL[i] = a_nodeTL[i];
    for (int carrier = 0; carrier < 2; ++carrier) {
        const bool electrons = carrier == 0;
        bool any = false;
        for (const Medium& m : a_media) any = any || (electrons ? m.energyN : m.energyP);
        std::vector<double>& T = electrons ? a_Tn : a_Tp;
        if (!any) {
            T.clear();
            continue;
        }
        if (T.size() != N) T = TL;
        std::vector<int> row(N, -1);
        int rows = 0;
        auto heats = [this, electrons](int i) {
            const int d = a_nodeDomain[std::size_t(i)];
            return d >= 0 && a_iN[std::size_t(i)] >= 0 && a_nodeContact[std::size_t(i)] < 0 && (electrons ? a_media[std::size_t(d)].energyN : a_media[std::size_t(d)].energyP);
        };
        for (int i = 0; i < a_n; ++i)
            if (heats(i)) row[std::size_t(i)] = rows++;
        if (rows == 0) {
            T = TL;
            continue;
        }
        std::vector<Eigen::Triplet<double>> trip;
        Eigen::VectorXd b = Eigen::VectorXd::Zero(rows);
        std::vector<double> flows(a_edges.size(), 0.0), corners(N, 0.0);
        // (Into row r of node i: its coefficient of node j's T, or a known T.)
        auto put = [&row, &trip, &b, &T](int r, int j, double c) {
            if (r < 0) return;
            if (row[std::size_t(j)] >= 0) trip.emplace_back(r, row[std::size_t(j)], c);
            else b(r) -= c * T[std::size_t(j)];
        };
        for (const Edge& e : a_edges) {
            if (e.w <= 0 || (electrons ? e.wn : e.wp) <= 0) continue;
            const int i = e.i, j = e.j;
            const int ri = row[std::size_t(i)], rj = row[std::size_t(j)];
            if (ri < 0 && rj < 0) continue;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            double flow = 0, gain = 0, ci = 0, cj = 0;   // particles i to j (1/s); each one's energy gained (J); densities on its bands
            if (electrons) {
                ci = e.fNi * x(a_iN[std::size_t(i)]);
                cj = e.fNj * x(a_iN[std::size_t(j)]);
                flow = -e.wn * (cj * bern(dN) - ci * bern(-dN)) / Q;
                gain = Q * a_Vt * (dN - (a_lgN[std::size_t(j)] - a_lgN[std::size_t(i)]));
            } else {
                ci = e.fPi * x(a_iP[std::size_t(i)]);
                cj = e.fPj * x(a_iP[std::size_t(j)]);
                flow = e.wp * (ci * bern(dP) - cj * bern(-dP)) / Q;
                gain = -Q * a_Vt * (dP + (a_lgP[std::size_t(j)] - a_lgP[std::size_t(i)]));
            }
            flows[std::size_t(&e - a_edges.data())] = flow;
            (void)gain;
            const double c = std::sqrt(std::max(ci, 1e-300) * std::max(cj, 1e-300));
            const double Te = 0.5 * (T[std::size_t(i)] + T[std::size_t(j)]);
            const double D = std::max(2.5 * KB * KB / Q * ((electrons ? e.wn : e.wp) / (Q * a_Vt)) * c * Te, 1e-300);
            const double P = 2.5 * KB * flow / D;
            const double bi = D * bern(-P), bj = D * bern(P);
            // S_ij = bi Ti - bj Tj leaves i, enters j.
            put(ri, i, bi);
            put(ri, j, -bj);
            put(rj, i, -bi);
            put(rj, j, bj);
        }
        // The field's gift, by triangle.
        const Space& tsp = *field().space;
        for (int t = 0; t < int(a_elements.size()); ++t) {
            const ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0 || !(em.volume > 0)) continue;
            const int* dof = tsp.dofs(t);
            for (int k = 0; k < 3; ++k) corners[std::size_t(dof[k])] += em.volume / 3;
            // (Its particles' flux density: least squares over its edges,
            // each weighed by its face, its flow along its direction - exact
            // for a uniform flux, an edge of a sliver of a face, a boundary's,
            // not the fit's: unweighed, its flow over its face took one
            // boundary node to 10¹² K.)
            double M[3] = {0, 0, 0}, rhs[2] = {0, 0};
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                const double face = e.w * e.h;
                if (!(face > 0) || (electrons ? e.wn : e.wp) <= 0) continue;
                const double flow = flows[std::size_t(em.edge[k])];
                M[0] += face * e.tx * e.tx;
                M[1] += face * e.tx * e.ty;
                M[2] += face * e.ty * e.ty;
                rhs[0] += e.tx * flow;
                rhs[1] += e.ty * flow;
            }
            const double det = M[0] * M[2] - M[1] * M[1];
            if (!(det > 1e-6 * (M[0] + M[2]) * (M[0] + M[2]))) continue;
            const double fx = (M[2] * rhs[0] - M[1] * rhs[1]) / det, fy = (-M[1] * rhs[0] + M[0] * rhs[1]) / det;
            // (Its band's edge, -q (V + θ): an electron's gift the drop along
            // its flux, a hole's the rise.)
            double gx = 0, gy = 0;
            for (int k = 0; k < 3; ++k) {
                const std::size_t c = std::size_t(dof[k]);
                const double u = x(a_iV[c]) + (electrons ? a_thetaN[c] : a_thetaP[c]);
                gx += em.gx[k] * u;
                gy += em.gy[k] * u;
            }
            const double gift = (electrons ? Q : -Q) * (fx * gx + fy * gy) * em.volume;
            for (int k = 0; k < 3; ++k) {
                const int r = row[std::size_t(dof[k])];
                if (r >= 0) b(r) += gift / 3;
            }
        }
        for (int i = 0; i < a_n; ++i) {
            const int r = row[std::size_t(i)];
            if (r < 0) continue;
            const Medium& m = a_media[std::size_t(a_nodeDomain[std::size_t(i)])];
            const double c = electrons ? x(a_iN[std::size_t(i)]) : x(a_iP[std::size_t(i)]);
            const double box = corners[std::size_t(i)];
            // (Its relaxation; a node all but empty held by a millionth of
            // its doping's, near the lattice.)
            const double floor = 1e-6 * std::max(std::abs(doping(i)), a_ni[std::size_t(i)]);
            const double k = 1.5 * KB * std::max(c, floor) * box / (electrons ? m.tauWn : m.tauWp);
            trip.emplace_back(r, r, k);
            b(r) += k * TL[std::size_t(i)];
        }
        Eigen::SparseMatrix<double> A(rows, rows);
        A.setFromTriplets(trip.begin(), trip.end());
        // (Each row by its largest: the densities span twenty decades.)
        Eigen::VectorXd scale = Eigen::VectorXd::Zero(rows);
        for (int k = 0; k < A.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator it(A, k); it; ++it) scale(it.row()) = std::max(scale(it.row()), std::abs(it.value()));
        for (int k = 0; k < A.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator it(A, k); it; ++it) it.valueRef() /= scale(it.row()) > 0 ? scale(it.row()) : 1.0;
        for (int r = 0; r < rows; ++r) b(r) /= scale(r) > 0 ? scale(r) : 1.0;
        Eigen::SparseLU<Eigen::SparseMatrix<double>> lu;
        lu.compute(A);
        if (lu.info() != Eigen::Success) continue;   // (the last kept)
        const Eigen::VectorXd t = lu.solve(b);
        if (lu.info() != Eigen::Success || !t.allFinite()) continue;
        // (Saturated, a velocity's temperature's mobility gives back about
        // vsat²/v - the update's slope -1, a plain one swinging about the
        // solution without end; unsaturated nought. Each node's step, of
        // its update r = solved - T, its secant's: T - r/r' - ω r, ω =
        // 1/(1 - slope) from a half to one; the first a half.)
        std::vector<double>& was = electrons ? a_TnWas : a_TpWas;
        std::vector<double>& rWas = electrons ? a_dTnWas : a_dTpWas;
        const bool secant = was.size() == N && rWas.size() == N;
        std::vector<double> now = T, rNow(N, 0.0);
        double residual = 0;
        for (int i = 0; i < a_n; ++i) {
            const std::size_t k = std::size_t(i);
            const double solved = row[k] >= 0 ? std::max(t(row[k]), 0.5 * TL[k]) : TL[k];
            rNow[k] = solved - T[k];
            residual = std::max(residual, std::abs(rNow[k]) / TL[k]);
        }
        double& omegaAll = a_omegaT[carrier];
        if (!secant) omegaAll = 1;
        else if (residual > a_residualT[carrier]) omegaAll = std::max(0.02, 0.5 * omegaAll);
        else omegaAll = std::min(1.0, 1.5 * omegaAll);
        a_residualT[carrier] = residual;
        for (int i = 0; i < a_n; ++i) {
            const std::size_t k = std::size_t(i);
            const double r = rNow[k];
            double omega = 0.5;
            if (secant && std::abs(T[k] - was[k]) > 1e-9 * T[k]) {
                const double slope = (r - rWas[k]) / (T[k] - was[k]);   // r' = g' - 1
                if (slope < 0) omega = std::clamp(-1 / slope, 0.5, 1.0);
            }
            T[k] += std::min(omega, omegaAll) * r;
        }
        was = std::move(now);
        rWas = std::move(rNow);
    }
}

std::vector<double> SemiconductorProblem::stimulatedRates() const
{
    std::vector<double> rates;
    if (a_stimulated.empty()) return rates;
    rates.assign(std::size_t(a_n), 0.0);
    if (a_lastR.size() != std::size_t(a_n)) return rates;
    for (const Stimulated& st : a_stimulated) {
        double volume = 0, recombined = 0;
        for (int i = 0; i < a_n; ++i) {
            const int d = a_nodeDomain[std::size_t(i)];
            if (d < 0 || !st.on[std::size_t(d)]) continue;
            volume += a_recVolume[std::size_t(i)];
            recombined += a_recVolume[std::size_t(i)] * a_lastR[std::size_t(i)];
        }
        const double rate = volume > 0 ? st.c * std::max(0.0, recombined / volume - st.R0) : 0.0;
        for (int i = 0; i < a_n; ++i) {
            const int d = a_nodeDomain[std::size_t(i)];
            if (d >= 0 && st.on[std::size_t(d)]) rates[std::size_t(i)] += rate;
        }
    }
    return rates;
}

double SemiconductorProblem::laserGain(double n, double p, double* dn, double* dp) const
{
    const Lasing& L = *a_laser;
    // The carriers it is of, N, and N's derivatives.
    n = std::max(n, 0.0);
    p = std::max(p, 0.0);
    double N = 0, dNn = 0, dNp = 0;
    if (L.carrier == 1) {
        N = n;
        dNn = 1;
    } else if (L.carrier == 2) {
        N = p;
        dNp = 1;
    } else {
        N = std::sqrt(n * p);
        if (N > 0) {
            dNn = 0.5 * N / n;
            dNp = 0.5 * N / p;
        }
    }
    double g = 0, dg = 0;
    if (L.gain == 2) {
        g = L.table ? L.table->at(N, &dg) : 0.0;
    } else if (L.gain == 1) {
        g = L.a * (N - L.Ntr);
        dg = L.a;
    } else {
        // (Its logarithm held at -10 g0 far below transparency: an absorption without end otherwise.)
        const double least = (L.Ntr + L.Ns) * std::exp(-10.0), Ne = N + L.Ns;
        g = L.g0 * std::log(std::max(Ne, least) / (L.Ntr + L.Ns));
        dg = Ne > least ? L.g0 / Ne : 0;
    }
    if (dn) *dn = dg * dNn;
    if (dp) *dp = dg * dNp;
    return g;
}

bool SemiconductorProblem::prepareLasing(QString* why)
{
    Lasing& L = *a_laser;
    L.ready = false;
    const int fi = a_s.fieldIndex(L.mode);
    if (fi < 0 || a_s.fields()[std::size_t(fi)].type != QLatin1String("resonator")) {
        *why = tr("%1: there is no resonator %2 (Electromagnetic Waves, Eigenfrequency) for it to lase in").arg(L.name, L.mode);
        return false;
    }
    const std::optional<double> freq = a_s.global(L.mode + QStringLiteral(".freq")), Qf = a_s.global(L.mode + QStringLiteral(".Q")),
                                U = a_s.global(L.mode + QStringLiteral(".U"));
    if (!a_s.fields()[std::size_t(fi)].solved || !freq || !Qf || !U || !(*U > 0) || !(*freq > 0)) {
        *why = tr("%1: its resonator %2's mode is not solved - an Eigenfrequency step before (the mode it keeps lases)").arg(L.name, L.mode);
        return false;
    }
    if (!std::isfinite(*Qf) || !(*Qf > 0)) {
        *why = tr("%1: the mode of %2 loses nothing (Q %3) - a laser's cavity loses its light: absorbing layers, a material's κ")
                   .arg(L.name, L.mode, formatNumber(*Qf, 4));
        return false;
    }
    L.omega = 2 * M_PI * *freq;
    L.tauP = *Qf / L.omega;
    L.photon = 1.054571817e-34 * L.omega;
    if (L.gain == 2 && (!L.table || std::abs(L.table->photon / L.photon - 1) > 1e-9)) {
        *why = tr("%1: its gain is its quantum wells' %2, tabulated at the mode's photon energy by a Laser L-I step - not solved in "
                  "another step").arg(L.name, L.request.wells);
        return false;
    }
    // Each node's share of the mode: over its domains' triangles, a third of
    // each one's ∫ we dV (the energy there) to each corner, over U; its part
    // of their volume; the photons' speed there (c/ng, or c/√Re ε of the
    // triangles about it, by volume); its radiative coefficient.
    const Expression we = Expression::compile(L.mode + QStringLiteral(".we"), a_s.pointScope());
    const Expression epsr = Expression::compile(L.mode + QStringLiteral(".epsr"), a_s.pointScope());
    const Space& sp = *field().space;
    const Mesh& mesh = a_s.mesh();
    const double unit = mesh.unitScale;
    std::vector<int> entry(std::size_t(a_n), -1);
    L.nodes.clear();
    L.share.clear();
    L.volume.clear();
    L.speed.clear();
    L.B.clear();
    std::vector<double> slow;   // by entry: Σ volume/v, for the volume's mean of 1/v
    PointEvaluator ev(a_s);
    const TriangleRule& rule = triangleRule(4);
    for (int t = 0; t < sp.elements(); ++t) {
        if (!a_elemSemi[std::size_t(t)]) continue;
        const int d = mesh.domain[std::size_t(t)];
        if (d < 0 || !L.on[std::size_t(d)]) continue;
        QPointF x[3];
        for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * unit;
        const double area = std::abs((x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y())) / 2;
        const double vol = area * a_s.weight((x[0] + x[1] + x[2]) / 3.0);
        double energy = 0;
        for (const auto& q : rule.points) {
            ev.moveTo(t, q[0], q[1]);
            const QPointF at = (1 - q[0] - q[1]) * x[0] + q[0] * x[1] + q[1] * x[2];
            const double w = ev.eval(we);
            if (std::isfinite(w)) energy += q[2] * 2 * area * a_s.weight(at) * w;
        }
        ev.moveTo(t, 1.0 / 3, 1.0 / 3);
        const double eps = ev.eval(epsr);
        const double v = L.ng > 0 ? 299792458.0 / L.ng : (eps >= 1 ? 299792458.0 / std::sqrt(eps) : 299792458.0);
        const int* dof = sp.dofs(t);
        for (int k = 0; k < 3; ++k) {
            const int i = dof[k];
            if (entry[std::size_t(i)] < 0) {
                entry[std::size_t(i)] = int(L.nodes.size());
                L.nodes.push_back(i);
                L.share.push_back(0);
                L.volume.push_back(0);
                L.speed.push_back(0);
                L.B.push_back(0);
                slow.push_back(0);
            }
            const std::size_t e = std::size_t(entry[std::size_t(i)]);
            L.share[e] += energy / 3 / *U;
            L.volume[e] += vol / 3;
            slow[e] += vol / 3 / v;
            L.B[e] += vol / 3 * a_media[std::size_t(d)].B;
        }
    }
    L.Gamma = 0;
    for (std::size_t e = 0; e < L.nodes.size(); ++e) {
        L.speed[e] = slow[e] > 0 ? L.volume[e] / slow[e] : 299792458.0;
        if (L.volume[e] > 0) L.B[e] /= L.volume[e];
        L.Gamma += L.share[e];
    }
    if (L.nodes.empty() || !(L.Gamma > 0)) {
        *why = tr("%1: the mode of %2 has none of its energy on its domains (Γ %3)").arg(L.name, L.mode, formatNumber(L.Gamma, 3));
        return false;
    }
    // (No spontaneous emission into the mode - β 0, or no Radiative on its
    // gain region: no photon starts it, S stays 0 at any current, its net
    // gain above nought unseen - S 0 with a gain of 600 GHz against a loss
    // of 118: the update check of 110e8b9.)
    if (!(L.beta > 0) || std::none_of(L.B.begin(), L.B.end(), [](double b) { return b > 0; })) {
        *why = tr("%1: no spontaneous emission into the mode (%2): no photon starts it, its light stays none at any current - give "
                  "β more than 0 (1e-4 is usual) and a Radiative recombination on its gain region")
                   .arg(L.name, !(L.beta > 0) ? tr("its β is 0") : tr("no Radiative recombination on its gain region"));
        return false;
    }
    L.ready = true;
    return true;
}

SemiconductorProblem::LaserState SemiconductorProblem::laserState(const Eigen::VectorXd& x) const
{
    LaserState out;
    if (!a_laser || a_iS < 0 || !a_laser->ready) return out;
    const Lasing& L = *a_laser;
    out.S = x(a_iS);
    double vol = 0;
    for (std::size_t e = 0; e < L.nodes.size(); ++e) {
        const std::size_t i = std::size_t(L.nodes[e]);
        if (a_iN[i] < 0) continue;
        const double n = x(a_iN[i]), p = x(a_iP[i]);
        const double g = laserGain(n, p, nullptr, nullptr);
        out.G += L.speed[e] * L.share[e] * g;
        out.gmean += L.share[e] * g;
        const double excess = n * p - a_ni[i] * a_ni[i] * std::exp(a_lgN[i] + a_lgP[i]);
        if (excess > 0) out.Rsp += L.B[e] * L.volume[e] * excess;
        const double N = L.carrier == 1 ? n : L.carrier == 2 ? p : std::sqrt(std::max(n * p, 0.0));
        out.Nact += N * L.volume[e];
        vol += L.volume[e];
    }
    out.Rstim = out.S * out.G;
    if (vol > 0) out.Nact /= vol;
    if (L.Gamma > 0) out.gmean /= L.Gamma;
    return out;
}

void SemiconductorProblem::assemble(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, Eigen::VectorXd& F,
                                    std::vector<Eigen::Triplet<double>>* jac) const
{
    const Space& sp = *a_s.fields()[std::size_t(a_fi)].space;
    F = Eigen::VectorXd::Zero(a_unknowns);
    if (jac) jac->clear();
    degeneracy(x);
    auto add = [&](int r, int c, double v) {
        if (jac && r >= 0 && c >= 0) jac->emplace_back(r, c, v);
    };
    auto V = [&](int i) { return x(a_iV[std::size_t(i)]); };
    // Trap-assisted tunnelling's field at node i: its triangles' |E|, by
    // their volumes.
    auto fieldAt = [&](int i) {
        double F = 0;
        const Space& fsp = *a_s.fields()[std::size_t(a_fi)].space;
        for (int k = a_fieldStart[std::size_t(i)]; k < a_fieldStart[std::size_t(i) + 1]; ++k) {
            const auto& [t, share] = a_fieldTris[std::size_t(k)];
            const ElementMobility& em = a_elements[std::size_t(t)];
            const int* tdof = fsp.dofs(t);
            double Ex = 0, Ey = 0;
            for (int c = 0; c < 3; ++c) {
                Ex -= em.gx[c] * V(tdof[c]);
                Ey -= em.gy[c] * V(tdof[c]);
            }
            F += share * std::hypot(Ex, Ey);
        }
        return F;
    };
    auto nn = [&](int i) { return a_iN[std::size_t(i)] >= 0 ? x(a_iN[std::size_t(i)]) : 0.0; };
    auto pp = [&](int i) { return a_iP[std::size_t(i)] >= 0 ? x(a_iP[std::size_t(i)]) : 0.0; };
    // Poisson: -Σ K V.
    for (int t = 0; t < sp.elements(); ++t) {
        if (!a_elemOn[std::size_t(t)]) continue;
        const int* dof = sp.dofs(t);
        const std::array<double, 9>& K = a_K[std::size_t(t)];
        for (int a = 0; a < 3; ++a) {
            const int r = a_iV[std::size_t(dof[a])];
            for (int b = 0; b < 3; ++b) {
                const double k = K[std::size_t(3 * a + b)];
                F(r) -= k * V(dof[b]);
                add(r, a_iV[std::size_t(dof[b])], -k);
            }
        }
    }
    // The carriers' currents along the edges (each carrier's potential step
    // the potential's and its band's offset).
    for (const Edge& e : a_edges) {
        if (e.w <= 0) continue;
        const int i = e.i, j = e.j;
        double dN = 0, dP = 0;
        edgeSteps(e, x, &dN, &dP);
        const int ni = a_iN[std::size_t(i)], nj = a_iN[std::size_t(j)], pi = a_iP[std::size_t(i)], pj = a_iP[std::size_t(j)];
        const int vi = a_iV[std::size_t(i)], vj = a_iV[std::size_t(j)];
        {
            const double bp = bern(dN), bm = bern(-dN), dbp = dbern(dN), dbm = dbern(-dN);
            const double fn = e.wn * (e.fNj * nn(j) * bp - e.fNi * nn(i) * bm);
            F(ni) += fn;
            F(nj) -= fn;
            // (Fermi-Dirac's: the step takes ln γ at each end, its change with
            // that end's n as the potential's, times Vt d ln γ/dn.)
            const double dvj = e.wn * (e.fNj * nn(j) * dbp + e.fNi * nn(i) * dbm) / a_Vt;
            const double dnj = e.wn * e.fNj * bp + dvj * a_Vt * a_dlgN[std::size_t(j)], dni = -e.wn * e.fNi * bm - dvj * a_Vt * a_dlgN[std::size_t(i)];
            for (const auto& [row, sign] : {std::pair{ni, 1.0}, std::pair{nj, -1.0}}) {
                add(row, nj, sign * dnj);
                add(row, ni, sign * dni);
                add(row, vj, sign * dvj);
                add(row, vi, -sign * dvj);
            }
        }
        {
            const double bp = bern(dP), bm = bern(-dP), dbp = dbern(dP), dbm = dbern(-dP);
            const double fp = e.wp * (e.fPi * pp(i) * bp - e.fPj * pp(j) * bm);
            F(pi) += fp;
            F(pj) -= fp;
            const double dvj = e.wp * (e.fPi * pp(i) * dbp + e.fPj * pp(j) * dbm) / a_Vt;
            const double dpi = e.wp * e.fPi * bp + dvj * a_Vt * a_dlgP[std::size_t(i)], dpj = -e.wp * e.fPj * bm - dvj * a_Vt * a_dlgP[std::size_t(j)];
            for (const auto& [row, sign] : {std::pair{pi, 1.0}, std::pair{pj, -1.0}}) {
                add(row, pi, sign * dpi);
                add(row, pj, sign * dpj);
                add(row, vj, sign * dvj);
                add(row, vi, -sign * dvj);
            }
        }
    }
    // A magnetic field: each edge's currents turned, as the edges' own.
    if (a_Bz != 0) {
        std::vector<double> hall;
        std::vector<Eigen::Triplet<double>> dh;
        hallFluxes(x, hall, jac ? &dh : nullptr);
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const Edge& e = a_edges[k];
            const int rows[2][2] = {{a_iN[std::size_t(e.i)], a_iN[std::size_t(e.j)]}, {a_iP[std::size_t(e.i)], a_iP[std::size_t(e.j)]}};
            for (int c = 0; c < 2; ++c) {
                if (rows[c][0] >= 0) F(rows[c][0]) += hall[2 * k + std::size_t(c)];
                if (rows[c][1] >= 0) F(rows[c][1]) -= hall[2 * k + std::size_t(c)];
            }
        }
        for (const auto& t : dh) {
            const Edge& e = a_edges[std::size_t(t.row() / 2)];
            const bool holes = t.row() % 2 == 1;
            add(holes ? a_iP[std::size_t(e.i)] : a_iN[std::size_t(e.i)], t.col(), t.value());
            add(holes ? a_iP[std::size_t(e.j)] : a_iN[std::size_t(e.j)], t.col(), -t.value());
        }
    }
    // Band-to-band tunnelling, by triangle: A E^γ e^(-B/E) at its field, the
    // pairs' sign and size at each corner's carriers, (n p - ni²)/((n +
    // ni)(p + ni)) - generation where the carriers are fewer than at
    // equilibrium (Hurkx's) -, a third of its volume to each.
    if (a_tunnelling) {
        const Space& tsp = *a_s.fields()[std::size_t(a_fi)].space;
        for (int t = 0; t < tsp.elements(); ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            const ElementMobility& em = a_elements[std::size_t(t)];
            const Medium& m = a_media[std::size_t(em.domain)];
            if (!m.tunnelling) continue;
            const int* dof = tsp.dofs(t);
            double Ex = 0, Ey = 0;
            for (int k = 0; k < 3; ++k) {
                Ex -= em.gx[k] * V(dof[k]);
                Ey -= em.gy[k] * V(dof[k]);
            }
            const double E = std::hypot(Ex, Ey);
            if (!(E > 1e3)) continue;
            const double rate = m.btA * std::pow(E, m.btGamma) * std::exp(-m.btB / E);
            if (!(rate > 0)) continue;
            // d rate/dE, and dE/dV at each corner.
            const double drate = rate * (m.btGamma / E + m.btB / (E * E));
            double dEdV[3];
            for (int k = 0; k < 3; ++k) dEdV[k] = -(Ex * em.gx[k] + Ey * em.gy[k]) / E;
            const double third = em.volume / 3;
            for (int k = 0; k < 3; ++k) {
                const int i = dof[k];
                const std::size_t ii = std::size_t(i);
                const double n = nn(i), p = pp(i), ni = a_ni[ii];
                const double den = (n + ni) * (p + ni), excess = n * p - ni * ni;
                // (Generation alone: D -1, whatever the carriers.)
                const double D = m.btGeneration ? -1.0 : excess / den;
                const double dDn = m.btGeneration ? 0.0 : (p * den - excess * (p + ni)) / (den * den);
                const double dDp = m.btGeneration ? 0.0 : (n * den - excess * (n + ni)) / (den * den);
                const double R = rate * D * third;   // pairs per second at the corner
                a_genCurrent += Q * std::max(-R, 0.0);
                const int rn = a_iN[ii], rp = a_iP[ii];
                F(rn) -= Q * R;
                F(rp) += Q * R;
                add(rn, rn, -Q * rate * dDn * third);
                add(rn, rp, -Q * rate * dDp * third);
                add(rp, rn, Q * rate * dDn * third);
                add(rp, rp, Q * rate * dDp * third);
                for (int c = 0; c < 3 && !tunnellingFieldDropped.load(); ++c) {
                    const int col = a_iV[std::size_t(dof[c])];
                    add(rn, col, -Q * drate * dEdV[c] * D * third);
                    add(rp, col, Q * drate * dEdV[c] * D * third);
                }
            }
        }
    }
    // Impact ionization, by triangle: its pairs (αn |Jn| + αp |Jp|)/q, the
    // α's at its field |E| (each edge's field along it is that projected -
    // a 45° edge's 0.71 E ionizes a seventh as much -, and each edge's
    // current that along it), Jn and Jp its three edges' currents along
    // them fitted (exact where the current is uniform); a third to each
    // corner's box.
    a_ionCurrent = 0;
    a_genCurrent = 0;
    if (a_ionization) {
        const Space& isp = *a_s.fields()[std::size_t(a_fi)].space;
        a_ionRate.assign(std::size_t(isp.elements()), 0.0);
        // (Each node's quasi-Fermi potentials, where a domain's is driven by
        // them: φn = V + θn - Vt ln(n/ni_ref), φp = V + θp + Vt ln(p/ni_ref).)
        std::vector<double> phiN, phiP;
        if (a_ionFermi) {
            phiN.assign(std::size_t(a_n), 0.0);
            phiP.assign(std::size_t(a_n), 0.0);
            for (int i = 0; i < a_n; ++i) {
                if (a_iN[std::size_t(i)] < 0) continue;
                phiN[std::size_t(i)] = V(i) + a_thetaN[std::size_t(i)] + a_Vt * a_lgN[std::size_t(i)] - a_Vt * std::log(std::max(nn(i), 1e-300) / a_refNi);
                phiP[std::size_t(i)] = V(i) + a_thetaP[std::size_t(i)] - a_Vt * a_lgP[std::size_t(i)] + a_Vt * std::log(std::max(pp(i), 1e-300) / a_refNi);
            }
        }
        // Each edge's current densities along it (A/m²) and their changes.
        struct EdgeCurrent {
            double jn = 0, jp = 0;
            double fn = 0, fp = 0;   // their rounding: 10⁻¹⁰ of the two terms they are the difference of
            double dn[4] = {0, 0, 0, 0}, dp[4] = {0, 0, 0, 0};   // by (n or p at i, at j, V at i, at j)
        };
        std::vector<EdgeCurrent> ec(a_edges.size());
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const Edge& e = a_edges[k];
            if (e.w <= 0 || e.domain < 0 || !a_media[std::size_t(e.domain)].ionizationCurrents) continue;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            // (Its flux over its dual face, w h: its mobility (Σ cot µ)/(Σ
            // cot) q Vt over its length - bounded by its triangles' largest.
            // Unbounded, saturated mobilities apart across an edge of a
            // sliver of a face, 10⁻¹⁵ of its neighbours', made a triangle's
            // holes' current 7·10⁵ A/m² in a diode carrying 0.04: the
            // electrons beside it no solution, an avalanche diode's wall past
            // -22.7 V.)
            const double gn = std::min(e.wn / e.w, e.muN) / e.h, gp = std::min(e.wp / e.w, e.muP) / e.h;
            const double bnp = bern(dN), bnm = bern(-dN), bpp = bern(dP), bpm = bern(-dP);
            EdgeCurrent& c = ec[k];
            c.jn = gn * (e.fNj * nn(e.j) * bnp - e.fNi * nn(e.i) * bnm);
            c.jp = gp * (e.fPi * pp(e.i) * bpp - e.fPj * pp(e.j) * bpm);
            c.fn = 1e-10 * gn * (std::abs(e.fNj * nn(e.j) * bnp) + std::abs(e.fNi * nn(e.i) * bnm));
            c.fp = 1e-10 * gp * (std::abs(e.fPi * pp(e.i) * bpp) + std::abs(e.fPj * pp(e.j) * bpm));
            const double dnV = gn * (e.fNj * nn(e.j) * dbern(dN) + e.fNi * nn(e.i) * dbern(-dN)) / a_Vt;
            const double dpV = gp * (e.fPi * pp(e.i) * dbern(dP) + e.fPj * pp(e.j) * dbern(-dP)) / a_Vt;
            c.dn[0] = -gn * e.fNi * bnm;
            c.dn[1] = gn * e.fNj * bnp;
            c.dn[2] = -dnV;
            c.dn[3] = dnV;
            c.dp[0] = gp * e.fPi * bpp;
            c.dp[1] = -gp * e.fPj * bpm;
            c.dp[2] = -dpV;
            c.dp[3] = dpV;
        }
        for (int t = 0; t < isp.elements(); ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            const ElementMobility& em = a_elements[std::size_t(t)];
            const Medium& m = a_media[std::size_t(em.domain)];
            if (!m.ionization || !m.ionizationCurrents) continue;
            const int* dof = isp.dofs(t);
            // Its field.
            double Ex = 0, Ey = 0;
            for (int k = 0; k < 3; ++k) {
                Ex -= em.gx[k] * V(dof[k]);
                Ey -= em.gy[k] * V(dof[k]);
            }
            const double Ef = std::hypot(Ex, Ey);
            // Its driving fields: the field, or each carrier's quasi-Fermi
            // gradient - nought where drift and diffusion balance: a doping
            // gradient's own field at rest accelerates nothing (the
            // Lumerical APD's p⁺⁺ Ge, a Gaussian of 6.6 nm, some 2e5 V/cm at
            // rest: a fifth of its pairs at -9 V made there, its breakdown
            // early - its review).
            double En = Ef, Ep = Ef, Fnx = 0, Fny = 0, Fpx = 0, Fpy = 0;
            if (m.ionFermi) {
                for (int k = 0; k < 3; ++k) {
                    Fnx += em.gx[k] * phiN[std::size_t(dof[k])];
                    Fny += em.gy[k] * phiN[std::size_t(dof[k])];
                    Fpx += em.gx[k] * phiP[std::size_t(dof[k])];
                    Fpy += em.gy[k] * phiP[std::size_t(dof[k])];
                }
                En = std::hypot(Fnx, Fny);
                Ep = std::hypot(Fpx, Fpy);
            }
            // (The field along each current: its currents first.)
            double an = 0, dan = 0, ap = 0, dap = 0;
            // (At its lattice's temperature: γ its a and b.)
            const double gT = em.ii;
            if (!m.ionAlong) {
                ionizationRate(En, gT * m.an, gT * m.bn, gT * m.an2, gT * m.bn2, m.E0, m.ionBetaN, &an, &dan);
                ionizationRate(Ep, gT * m.ap, gT * m.bp, gT * m.ap2, gT * m.bp2, m.E0, m.ionBetaP, &ap, &dap);
                if (an == 0 && ap == 0) continue;
            }
            // Its currents: J = (Σ t tᵀ)⁻¹ Σ t j over its edges.
            double M[3] = {0, 0, 0};   // xx, xy, yy
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                M[0] += e.tx * e.tx;
                M[1] += e.tx * e.ty;
                M[2] += e.ty * e.ty;
            }
            const double det = M[0] * M[2] - M[1] * M[1];
            if (!(std::abs(det) > 1e-12)) continue;
            // (A row of (Σ t tᵀ)⁻¹ t for edge k: how its j weighs in J.)
            double wx[3], wy[3];
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                wx[k] = (M[2] * e.tx - M[1] * e.ty) / det;
                wy[k] = (-M[1] * e.tx + M[0] * e.ty) / det;
            }
            // (The field driving it: each edge's current weighed by how far
            // it stands above its rounding - where drift and diffusion of
            // some 10¹² A/m² balance, between two nodes of a p⁺ side, the
            // current is their difference, its rounding ten thousand times
            // the current there. Taken whole, a junction's triangles' holes'
            // currents were that rounding: their pairs counted 99 times
            // without recombination, Newton's method cycling at the minority
            // carriers beside them near a breakdown - the hunt of 19 October,
            // A1 and A2. Weighed, a junction's triangle takes its current from
            // its edges across the junction; and what still is no more than
            // the rounding is smoothly nought, J²/√(J² + J0²), J0 the fit's
            // own rounding. J0 of a triangle's density instead - a p⁺
            // corner's - took the junction's genuine current with the noise:
            // 10 % of the multiplication on a mesh of 0.1 µm.)
            const bool byField = !m.ionAlong && !m.ionFermi;
            double cnx[3], cny[3], cpx[3], cpy[3];
            for (int k = 0; k < 3; ++k) {
                cnx[k] = cpx[k] = wx[k];
                cny[k] = cpy[k] = wy[k];
            }
            double J0Fn = 0, J0Fp = 0;
            if (byField) {
                // (Weighed least squares; with fewer than two edges above
                // their rounding, as they were.)
                auto weighed = [this, &em, &ec, det](bool holes, double* cx, double* cy) {
                    double rho = 0, w[3], A[3] = {0, 0, 0}, top = 0;
                    for (int k = 0; k < 3; ++k) rho = std::max(rho, std::abs(holes ? ec[std::size_t(em.edge[k])].jp : ec[std::size_t(em.edge[k])].jn));
                    for (int k = 0; k < 3; ++k) {
                        const double f = holes ? ec[std::size_t(em.edge[k])].fp : ec[std::size_t(em.edge[k])].fn;
                        w[k] = rho > 0 ? 1 / (1 + (f / rho) * (f / rho)) : 1.0;
                        top = std::max(top, w[k]);
                    }
                    for (int k = 0; k < 3; ++k) {
                        const Edge& e = a_edges[std::size_t(em.edge[k])];
                        w[k] /= top;
                        A[0] += w[k] * e.tx * e.tx;
                        A[1] += w[k] * e.tx * e.ty;
                        A[2] += w[k] * e.ty * e.ty;
                    }
                    const double d = A[0] * A[2] - A[1] * A[1];
                    if (!(std::abs(d) > 1e-3 * std::abs(det))) return;
                    for (int k = 0; k < 3; ++k) {
                        const Edge& e = a_edges[std::size_t(em.edge[k])];
                        cx[k] = w[k] * (A[2] * e.tx - A[1] * e.ty) / d;
                        cy[k] = w[k] * (-A[1] * e.tx + A[0] * e.ty) / d;
                    }
                };
                weighed(false, cnx, cny);
                weighed(true, cpx, cpy);
                for (int k = 0; k < 3; ++k) {
                    const EdgeCurrent& c = ec[std::size_t(em.edge[k])];
                    J0Fn += std::hypot(cnx[k], cny[k]) * c.fn;
                    J0Fp += std::hypot(cpx[k], cpy[k]) * c.fp;
                }
            }
            double Jnx = 0, Jny = 0, Jpx = 0, Jpy = 0;
            for (int k = 0; k < 3; ++k) {
                const EdgeCurrent& c = ec[std::size_t(em.edge[k])];
                Jnx += cnx[k] * c.jn;
                Jny += cny[k] * c.jn;
                Jpx += cpx[k] * c.jp;
                Jpy += cpy[k] * c.jp;
            }
            const double Jn = std::hypot(Jnx, Jny), Jp = std::hypot(Jpx, Jpy);
            // (Along each current, E·J/s - the Lumerical E dot J -, nought
            // where the current runs against the field; s = √(|J|² + J0²),
            // J0 a millionth of the carrier's drift at the field, q n µ |E|
            // (µ its own unsaturated: a thousandth cut a saturated
            // germanium's F by 0.25 %, an APD's gain near breakdown by a
            // third): where it drifts E·Ĵ, where drift and diffusion balance
            // - its current nought but its rounding, its direction any -
            // nought smoothly. As E·J/|J| a steep contact layer at rest, its
            // field 2e5 V/cm, took the noise's direction: no solution from
            // rest.)
            double unx = 0, uny = 0, upx = 0, upy = 0, sN = 0, sP = 0, J0n = 0, J0p = 0, cN = 0, cP = 0, nbar = 0, pbar = 0;
            if (m.ionAlong) {
                for (int k = 0; k < 3; ++k) {
                    nbar += nn(dof[k]) / 3;
                    pbar += pp(dof[k]) / 3;
                }
                cN = 1e-6 * Q * em.mun * em.tn;
                cP = 1e-6 * Q * em.mup * em.tp;
                J0n = cN * nbar * Ef;
                J0p = cP * pbar * Ef;
                sN = std::hypot(Jn, J0n);
                sP = std::hypot(Jp, J0p);
                if (sN > 0) {
                    unx = Jnx / sN;
                    uny = Jny / sN;
                }
                if (sP > 0) {
                    upx = Jpx / sP;
                    upy = Jpy / sP;
                }
                En = std::max(Ex * unx + Ey * uny, 0.0);
                Ep = std::max(Ex * upx + Ey * upy, 0.0);
                ionizationRate(En, gT * m.an, gT * m.bn, gT * m.an2, gT * m.bn2, m.E0, m.ionBetaN, &an, &dan);
                ionizationRate(Ep, gT * m.ap, gT * m.bp, gT * m.ap2, gT * m.bp2, m.E0, m.ionBetaP, &ap, &dap);
                if (an == 0 && ap == 0) continue;
                if (!(En > 0)) dan = 0;
                if (!(Ep > 0)) dap = 0;
            }
            double JnI = Jn, JpI = Jp, sFn = Jn, sFp = Jp;
            if (byField) {
                sFn = std::hypot(Jn, J0Fn);
                sFp = std::hypot(Jp, J0Fp);
                JnI = sFn > 0 ? Jn * Jn / sFn : 0.0;
                JpI = sFp > 0 ? Jp * Jp / sFp : 0.0;
            }
            const double third = em.volume / 3;
            const double g = (an * JnI + ap * JpI) * third;   // A, to each corner
            a_ionCurrent += 3 * g;
            a_ionRate[std::size_t(t)] = (an * JnI + ap * JpI) / Q;
            for (int k = 0; k < 3; ++k) {
                F(a_iN[std::size_t(dof[k])]) += g;
                F(a_iP[std::size_t(dof[k])]) -= g;
            }
            if (!jac) continue;
            // Its change: by the field (each corner's V), by the currents
            // (each edge's ends' n, p, V).
            // (Its corners' V, n and p: nine unknowns at most.)
            std::array<std::pair<int, double>, 9> dg{};
            int used = 0;
            auto addTo = [&](int col, double d) {
                for (int u = 0; u < used; ++u)
                    if (dg[std::size_t(u)].first == col) {
                        dg[std::size_t(u)].second += d;
                        return;
                    }
                if (used < 9) dg[std::size_t(used++)] = {col, d};
            };
            if (m.ionFermi)
                // (|∇φ|'s change by each corner's φ, φ's by its V, and by its
                // n (-Vt/n) or p (Vt/p).)
                for (int k = 0; k < 3; ++k) {
                    const int i = dof[k];
                    if (En > 0) {
                        const double d = (Fnx * em.gx[k] + Fny * em.gy[k]) / En * dan * Jn * third;
                        addTo(a_iV[std::size_t(i)], d);
                        addTo(a_iN[std::size_t(i)], -d * a_Vt / std::max(nn(i), 1e-300));
                    }
                    if (Ep > 0) {
                        const double d = (Fpx * em.gx[k] + Fpy * em.gy[k]) / Ep * dap * Jp * third;
                        addTo(a_iV[std::size_t(i)], d);
                        addTo(a_iP[std::size_t(i)], d * a_Vt / std::max(pp(i), 1e-300));
                    }
                }
            else if (m.ionAlong)
                // (dEn = u·dE + (E - En u)·dJ/s - En J0 dJ0/s², u = J/s: here
                // u·dE and J0's by each corner's V - through |E| - and its n
                // or p.)
                for (int k = 0; k < 3; ++k) {
                    const int i = dof[k];
                    const double gE = Ef > 0 ? (Ex * em.gx[k] + Ey * em.gy[k]) / Ef : 0.0;   // -d|E|/dV
                    const double wn = sN > 0 ? En * J0n / (sN * sN) : 0.0, wp = sP > 0 ? Ep * J0p / (sP * sP) : 0.0;
                    addTo(a_iV[std::size_t(i)], (dan * Jn * (-(unx * em.gx[k] + uny * em.gy[k]) + wn * cN * nbar * gE)
                                                 + dap * Jp * (-(upx * em.gx[k] + upy * em.gy[k]) + wp * cP * pbar * gE)) * third);
                    addTo(a_iN[std::size_t(i)], -dan * Jn * wn * cN * Ef / 3 * third);
                    addTo(a_iP[std::size_t(i)], -dap * Jp * wp * cP * Ef / 3 * third);
                }
            else if (Ef > 0)
                // (J0 a rounding's: its own changes left out.)
                for (int k = 0; k < 3; ++k) {
                    const double dE = -(Ex * em.gx[k] + Ey * em.gy[k]) / Ef;
                    addTo(a_iV[std::size_t(dof[k])], (dan * JnI + dap * JpI) * dE * third);
                }
            for (int k = 0; k < 3; ++k) {
                const Edge& e = a_edges[std::size_t(em.edge[k])];
                const EdgeCurrent& c = ec[std::size_t(em.edge[k])];
                // d|J| = Ĵ·dJ, dJ = (wx, wy) dj; along the current, its
                // field's change too: dan |J| (E - En û)·dJ/|J|.
                double sn = Jn > 0 ? an * (Jnx * cnx[k] + Jny * cny[k]) / Jn : 0.0;
                double sp = Jp > 0 ? ap * (Jpx * cpx[k] + Jpy * cpy[k]) / Jp : 0.0;
                if (byField) {
                    // (d(J²/s) = J (J² + 2 J0²)/s³ dJ = (J² + 2 J0²)/s³ J·dJ.)
                    sn = sFn > 0 ? an * (Jnx * cnx[k] + Jny * cny[k]) * (Jn * Jn + 2 * J0Fn * J0Fn) / (sFn * sFn * sFn) : 0.0;
                    sp = sFp > 0 ? ap * (Jpx * cpx[k] + Jpy * cpy[k]) * (Jp * Jp + 2 * J0Fp * J0Fp) / (sFp * sFp * sFp) : 0.0;
                }
                if (m.ionAlong) {
                    if (sN > 0) sn += dan * Jn / sN * ((Ex - En * unx) * cnx[k] + (Ey - En * uny) * cny[k]);
                    if (sP > 0) sp += dap * Jp / sP * ((Ex - Ep * upx) * cpx[k] + (Ey - Ep * upy) * cpy[k]);
                }
                const int cols[4][2] = {{a_iN[std::size_t(e.i)], a_iP[std::size_t(e.i)]}, {a_iN[std::size_t(e.j)], a_iP[std::size_t(e.j)]},
                                        {a_iV[std::size_t(e.i)], a_iV[std::size_t(e.i)]}, {a_iV[std::size_t(e.j)], a_iV[std::size_t(e.j)]}};
                for (int q = 0; q < 4; ++q) {
                    addTo(cols[q][0], sn * c.dn[q] * third);
                    addTo(cols[q][1], sp * c.dp[q] * third);
                }
            }
            for (int k = 0; k < 3; ++k)
                for (int u = 0; u < used; ++u) {
                    add(a_iN[std::size_t(dof[k])], dg[std::size_t(u)].first, dg[std::size_t(u)].second);
                    add(a_iP[std::size_t(dof[k])], dg[std::size_t(u)].first, -dg[std::size_t(u)].second);
                }
        }
    }
    // At each semiconductor node: the space charge, recombination, generation,
    // the carriers' change in time.
    const std::size_t N = std::size_t(a_n);
    const std::vector<double> stimulated = stimulatedRates();
    if (!a_stimulated.empty() && a_lastR.size() != N) a_lastR.assign(N, 0.0);
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        const double A = a_box[std::size_t(i)], An = a_boxN[std::size_t(i)], Ap = a_boxP[std::size_t(i)];
        const int rv = a_iV[std::size_t(i)], rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
        const double n = nn(i), p = pp(i), ni = a_ni[std::size_t(i)];
        // (Fermi-Dirac's: γ = 1 for Boltzmann's.)
        const double gn = std::exp(a_lgN[std::size_t(i)]), gp = std::exp(a_lgP[std::size_t(i)]);
        double nd = 0, dnd = 0, na = 0, dna = 0;
        if (m.incomplete) {
            ionized(i, n / gn, p / gp, &nd, &dnd, &na, &dna);
            F(rv) += Q * (Ap * p - An * n + A * (nd - na));
            add(rv, rn, Q * (A * dnd / gn - An));
            add(rv, rp, Q * (Ap - A * dna / gp));
        } else {
            F(rv) += Q * (Ap * p - An * n + A * doping(i));
            add(rv, rn, -Q * An);
            add(rv, rp, Q * Ap);
        }
        // Recombination: in each part of its box of its own bands, by that
        // domain's mechanisms and lifetimes - volume times rate summed. (The
        // whole box by the node's medium, as it was, recombined a well's
        // carriers over half its barriers' next rows as well - an 8 nm well
        // as some 13 nm, a laser's threshold with the mesh: the update check
        // of 110e8b9.)
        double RV = 0, dRnV = 0, dRpV = 0;
        const double excess = n * p - ni * ni * gn * gp;
        for (int k = a_recStart[std::size_t(i)]; k < a_recStart[std::size_t(i) + 1]; ++k) {
            const RecombiningPart& part = a_rec[std::size_t(k)];
            const Medium& in = a_media[std::size_t(part.domain)];
            const double Vk = part.volume;
            double R = 0, dRn = 0, dRp = 0;
            if (in.srh) {
                // (Its lifetimes there; a field's trap-assisted tunnelling
                // shortening them, τ/(1 + Γ), Γ of the node's field.)
                double tn = part.tn, tp = part.tp;
                double F = 0, gammaN = 0, gammaP = 0, dgN = 0, dgP = 0;
                const bool tat = in.tat && a_tat;
                const int from = tat ? a_fieldStart[std::size_t(i)] : 0, to = tat ? a_fieldStart[std::size_t(i) + 1] : 0;
                if (tat && to > from) {
                    // (The trap Et above the intrinsic level: Ec - Et, Et - Ev at
                    // the node, its gap narrowed.)
                    const double kT = KB * a_T, gap = a_gap[std::size_t(i)];
                    const double ecEi = in.dEc - gap / 2, eiEv = in.Eg - gap - ecEi;
                    F = fieldAt(i);
                    hurkx(F, in.FGn, (ecEi - in.Et) / kT, &gammaN, &dgN);
                    hurkx(F, in.FGp, (eiEv + in.Et) / kT, &gammaP, &dgP);
                    tn /= 1 + gammaN;
                    tp /= 1 + gammaP;
                }
                const double n1 = part.domain == d ? a_n1[std::size_t(i)] : ni * std::exp(in.Et / (KB * a_T));
                const double p1 = part.domain == d ? a_p1[std::size_t(i)] : ni * std::exp(-in.Et / (KB * a_T));
                const double nn1 = n + gn * n1, pp1 = p + gp * p1;
                const double D = tp * nn1 + tn * pp1;
                R += excess / D;
                dRn += (p * D - excess * tp) / (D * D);
                dRp += (n * D - excess * tn) / (D * D);
                if (jac && tat && to > from && (dgN != 0 || dgP != 0)) {
                    // dR/dV through τ's field: dR/dτn = -R pp1/D, dτ/dF = -τ dΓ/dF/(1 + Γ).
                    const double rate = excess / D;
                    const double dRdF = rate / D * (tn * dgN / (1 + gammaN) * pp1 + tp * dgP / (1 + gammaP) * nn1);
                    for (int f = from; f < to; ++f) {
                        const auto& [t, share] = a_fieldTris[std::size_t(f)];
                        const ElementMobility& em = a_elements[std::size_t(t)];
                        const int* tdof = a_s.fields()[std::size_t(a_fi)].space->dofs(t);
                        double Ex = 0, Ey = 0;
                        for (int c = 0; c < 3; ++c) {
                            Ex -= em.gx[c] * V(tdof[c]);
                            Ey -= em.gy[c] * V(tdof[c]);
                        }
                        const double E = std::hypot(Ex, Ey);
                        if (!(E > 0)) continue;
                        for (int c = 0; c < 3; ++c) {
                            const double dEdV = -(Ex * em.gx[c] + Ey * em.gy[c]) / E;
                            const int col = a_iV[std::size_t(tdof[c])];
                            add(rn, col, -Q * Vk * dRdF * share * dEdV);
                            add(rp, col, Q * Vk * dRdF * share * dEdV);
                        }
                    }
                }
            }
            if (in.Cn != 0 || in.Cp != 0) {
                const double c = in.Cn * n + in.Cp * p;
                R += c * excess;
                dRn += in.Cn * excess + c * p;
                dRp += in.Cp * excess + c * n;
            }
            if (in.B != 0) {
                R += in.B * excess;
                dRn += in.B * p;
                dRp += in.B * n;
            }
            RV += Vk * R;
            dRnV += Vk * dRn;
            dRpV += Vk * dRp;
        }
        const std::vector<double>& generated = generationNow();
        const double G = (generated.empty() ? 0.0 : generated[std::size_t(i)]) * a_lightScale;
        // (Stimulated: the last iterate's mean, its own derivative left out -
        // over the parts that recombine.)
        const double AR = a_recVolume[std::size_t(i)];
        if (!stimulated.empty()) {
            a_lastR[std::size_t(i)] = AR > 0 ? RV / AR : 0.0;
            RV += AR * stimulated[std::size_t(i)];
        }
        if (a_recombined.size() != std::size_t(a_n)) a_recombined.assign(std::size_t(a_n), 0.0);
        a_recombined[std::size_t(i)] = RV;
        F(rn) -= Q * (RV - A * G);
        F(rp) += Q * (RV - A * G);
        a_genCurrent += Q * (std::max(-RV, 0.0) + std::max(A * G, 0.0));
        add(rn, rn, -Q * dRnV);
        add(rn, rp, -Q * dRpV);
        add(rp, rn, Q * dRnV);
        add(rp, rp, Q * dRpV);
        if (mass > 0 && history) {
            F(rn) -= Q * An * (mass * n - (*history)[N + std::size_t(i)]);
            F(rp) += Q * Ap * (mass * p - (*history)[2 * N + std::size_t(i)]);
            double kn = An, kp = Ap;
            if (m.incomplete && a_ionAt > 0 && history->size() >= a_ionAt + 2 * N) {
                F(rn) += Q * A * (mass * nd - (*history)[a_ionAt + std::size_t(i)]);
                F(rp) -= Q * A * (mass * na - (*history)[a_ionAt + N + std::size_t(i)]);
                kn -= A * dnd / gn;
                kp -= A * dna / gp;
            }
            add(rn, rn, -Q * kn * mass);
            add(rp, rp, Q * kp * mass);
        }
    }
    // A laser: the stimulated recombination at each node of its domains,
    // S v g(N) Π (pairs/s), and the photons' balance (τp G - 1) S + τp β
    // Rsp = 0 - G = Σ v g Π, Rsp = Σ B (n p - ni²) of its volume -; until
    // its mode is read, S = 0.
    if (a_laser && a_iS >= 0) {
        const Lasing& L = *a_laser;
        const int rs = a_iS;
        if (!L.ready) {
            F(rs) = x(rs);
            add(rs, rs, 1.0);
        } else {
            const double S = x(rs);
            double G = 0, Rsp = 0;
            for (std::size_t e = 0; e < L.nodes.size(); ++e) {
                const int i = L.nodes[e];
                const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
                if (rn < 0 || rp < 0) continue;
                const double n = nn(i), p = pp(i);
                double dgn = 0, dgp = 0;
                const double g = laserGain(n, p, &dgn, &dgp);
                const double w = L.speed[e] * L.share[e];
                const double rate = S * w * g;
                G += w * g;
                F(rn) -= Q * rate;
                F(rp) += Q * rate;
                add(rn, rn, -Q * S * w * dgn);
                add(rn, rp, -Q * S * w * dgp);
                add(rn, rs, -Q * w * g);
                add(rp, rn, Q * S * w * dgn);
                add(rp, rp, Q * S * w * dgp);
                add(rp, rs, Q * w * g);
                double rsn = 0, rsp = 0;
                const double excess = n * p - a_ni[std::size_t(i)] * a_ni[std::size_t(i)] * std::exp(a_lgN[std::size_t(i)] + a_lgP[std::size_t(i)]);
                if (excess > 0 && L.B[e] > 0) {
                    Rsp += L.B[e] * L.volume[e] * excess;
                    rsn = L.B[e] * L.volume[e] * p;
                    rsp = L.B[e] * L.volume[e] * n;
                }
                add(rs, rn, L.tauP * (S * w * dgn + L.beta * rsn));
                add(rs, rp, L.tauP * (S * w * dgp + L.beta * rsp));
            }
            F(rs) = (L.tauP * G - 1) * S + L.tauP * L.beta * Rsp;
            add(rs, rs, L.tauP * G - 1);
        }
    }
    // Surface charges.
    for (std::size_t k = 0; k < a_surface.size(); ++k) {
        const int i = a_surface[k].first;
        const Expression& e = a_surface[k].second;
        double rho = 0;
        if (e.isConstant()) rho = e.constant();
        else {
            PointEvaluator ev(a_s);
            const auto& [t, r] = a_nodePoint[std::size_t(i)];
            ev.moveTo(t, r[0], r[1]);
            rho = ev.eval(e);
        }
        if (std::isfinite(rho)) F(a_iV[std::size_t(i)]) += rho * a_surfaceLength[k];
    }
    // Interface traps: each level's charge, -q N f (acceptor-like) or
    // q N (1 - f), and the pairs it recombines, at Boltzmann's carriers
    // (Fermi-Dirac's n/γ: the traps filled by the Fermi level).
    for (const TrapLevel& tl : a_traps) {
        const int i = tl.node;
        const std::size_t k = std::size_t(i);
        const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
        const TrapState st = trapState(tl, nn(i) / gn, pp(i) / gp);
        const int rv = a_iV[k], rn = a_iN[k], rp = a_iP[k];
        if (tl.charged) {
            F(rv) += Q * tl.N * (tl.acceptor ? -st.f : 1 - st.f) * tl.area;
            add(rv, rn, -Q * tl.N * st.dfdn / gn * tl.area);
            add(rv, rp, -Q * tl.N * st.dfdp / gp * tl.area);
        }
        if (tl.cn == 0 && tl.cp == 0) continue;
        a_genCurrent += Q * tl.area * std::max(-st.U, 0.0);
        if (tl.minorityOnly) {
            // (An ohmic contact's minority carriers alone: the majority held.)
            if (electronsMinority(i)) {
                F(rn) -= Q * tl.area * st.U;
                add(rn, rn, -Q * tl.area * st.dUdn / gn);
                add(rn, rp, -Q * tl.area * st.dUdp / gp);
            } else {
                F(rp) += Q * tl.area * st.U;
                add(rp, rn, Q * tl.area * st.dUdn / gn);
                add(rp, rp, Q * tl.area * st.dUdp / gp);
            }
            continue;
        }
        F(rn) -= Q * tl.area * st.U;
        F(rp) += Q * tl.area * st.U;
        add(rn, rn, -Q * tl.area * st.dUdn / gn);
        add(rn, rp, -Q * tl.area * st.dUdp / gp);
        add(rp, rn, Q * tl.area * st.dUdn / gn);
        add(rp, rp, Q * tl.area * st.dUdp / gp);
    }
    // Schottky contacts: thermionic emission over the barrier.
    for (const Contact& c : a_contacts) {
        if (c.kind != Contact::Schottky) continue;
        for (std::size_t k = 0; k < c.nodes.size(); ++k) {
            const int i = c.nodes[k];
            const Medium& m = a_media[std::size_t(a_nodeDomain[std::size_t(i)])];
            const double ni = a_ni[std::size_t(i)];
            const double neq = m.Nc * std::exp(-c.phiB / (KB * a_T)), peq = ni * ni / neq;
            const double L = c.length[k];
            const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
            F(rn) -= Q * c.vn * (nn(i) - neq) * L;
            add(rn, rn, -Q * c.vn * L);
            F(rp) += Q * c.vp * (pp(i) - peq) * L;
            add(rp, rp, Q * c.vp * L);
        }
    }
}

std::vector<double> SemiconductorProblem::sliceHistory(const std::vector<double>& h, int s) const
{
    std::vector<double> out = h;
    if (s == 0) return out;
    const std::size_t N = std::size_t(a_n), at = a_slicesAt + std::size_t(s - 1) * a_sliceBlock;
    if (h.size() < at + a_sliceBlock) return out;
    for (std::size_t k = 0; k < 3 * N; ++k) out[k] = h[at + k];
    if (a_ionAt > 0)
        for (std::size_t k = 0; k < 2 * N; ++k) out[a_ionAt + k] = h[at + 3 * N + k];
    return out;
}

void SemiconductorProblem::useSlice(int s)
{
    a_sliceNow = s;
    if (a_slices <= 1 || std::size_t(s) >= a_sliceEdges.size() || a_sliceEdges[std::size_t(s)].size() != a_edges.size()) return;
    const std::vector<std::array<double, 4>>& w = a_sliceEdges[std::size_t(s)];
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        a_edges[k].wn = w[k][0];
        a_edges[k].wp = w[k][1];
        a_edges[k].muN = w[k][2];
        a_edges[k].muP = w[k][3];
    }
}

void SemiconductorProblem::keepSliceEdges(int s)
{
    if (a_slices <= 1) return;
    if (a_sliceEdges.size() != std::size_t(a_slices)) a_sliceEdges.assign(std::size_t(a_slices), {});
    std::vector<std::array<double, 4>>& w = a_sliceEdges[std::size_t(s)];
    w.resize(a_edges.size());
    for (std::size_t k = 0; k < a_edges.size(); ++k) w[k] = {a_edges[k].wn, a_edges[k].wp, a_edges[k].muN, a_edges[k].muP};
}

bool SemiconductorProblem::newton(Eigen::VectorXd& x, double mass, const std::vector<double>* history, int maxSteps, int* steps, QString* why, bool met,
                                  bool consistent)
{
    // (All slices' unknowns in x - the first's and the circuits' as one
    // slice's, the others' after: each slice assembled at its own vector,
    // its rows and columns placed among all of them; a circuit's row takes
    // each slice's current weighed by its share of the length.)
    const int S = a_slices, total = a_total;
    std::vector<std::vector<double>> histories;
    if (history && S > 1) {
        histories.resize(std::size_t(S));
        for (int s = 1; s < S; ++s) histories[std::size_t(s)] = sliceHistory(*history, s);
    }
    auto historyOf = [&history, &histories](int s) -> const std::vector<double>* {
        return !history ? nullptr : s == 0 ? history : &histories[std::size_t(s)];
    };
    auto g = [this](int s, int k) { return globalIndex(s, k); };
    // Unknowns in units of Vt and of each node's own carriers (a relative
    // change: minority carriers 10^14 below the majority keep their digits);
    // each row divided by its largest entry.
    Eigen::VectorXd colScale(total);
    auto carrierScales = [this, &x, &colScale, &g, S]() {
        for (int s = 0; s < S; ++s)
            for (int i = 0; i < a_n; ++i) {
                if (a_iV[std::size_t(i)] >= 0) colScale(g(s, a_iV[std::size_t(i)])) = a_Vt;
                for (int k : {a_iN[std::size_t(i)], a_iP[std::size_t(i)]})
                    if (k >= 0) colScale(g(s, k)) = std::max(std::abs(x(g(s, k))), 1e-20 * a_scale);
            }
        // (A laser's photons: as the carriers, by their own number; a
        // circuit's contact's voltage as the potential.)
        if (a_iS >= 0) colScale(a_iS) = std::max(std::abs(x(a_iS)), 1e-6);
        for (const Contact& c : a_contacts)
            if (c.iC >= 0) colScale(c.iC) = a_Vt;
    };
    // A laser's photons from nothing (a solution without them): those its
    // spontaneous emission keeps below threshold, τp β Rsp/(1 - τp G).
    if (a_iS >= 0 && a_laser->ready && !(x(a_iS) > 0)) {
        const LaserState ls = laserState(x);
        const Lasing& L = *a_laser;
        x(a_iS) = L.tauP * L.beta * ls.Rsp / std::max(1 - L.tauP * ls.G, 1e-3);
    }
    std::vector<char> fixed(std::size_t(total), 0);
    Eigen::VectorXd target = Eigen::VectorXd::Zero(total);
    // A circuit's contact's nodes: their potentials its own voltage's, Vc
    // plus each one's offset at the voltage its values were set at (its
    // source's) - their rows V - Vc - offset; its own row the circuit's.
    std::vector<int> circuitOf(std::size_t(total), -1);
    const bool circuits = hasCircuits();
    if (circuits && a_Bz != 0) {
        *why = tr("%1: a magnetic field and contacts behind circuits are not solved together").arg(a_node->name());
        return false;
    }
    if (circuits && a_lastApplied.size() != a_contacts.size()) {
        *why = tr("%1: its contacts' values are not set").arg(a_node->name());
        return false;
    }
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        if (c.iC < 0) continue;
        if (!std::isfinite(x(c.iC))) x(c.iC) = a_lastApplied[k];
        for (int s = 0; s < S; ++s)
            for (int i : c.nodes) {
                circuitOf[std::size_t(g(s, a_iV[std::size_t(i)]))] = int(k);
                target(g(s, a_iV[std::size_t(i)])) = a_bcV[std::size_t(i)] - a_lastApplied[k];   // (the offset)
            }
    }
    for (int s = 0; s < S; ++s)
        for (const Contact& c : a_contacts)
            for (int i : c.nodes) {
                const int rv = g(s, a_iV[std::size_t(i)]);
                fixed[std::size_t(rv)] = 1;
                if (c.iC < 0) target(rv) = a_bcV[std::size_t(i)];
                if (c.kind == Contact::Ohmic) {
                    // (Its minority carriers a surface's recombination's: free.)
                    const bool freeN = a_minorityFree[std::size_t(i)] && electronsMinority(i), freeP = a_minorityFree[std::size_t(i)] && !electronsMinority(i);
                    if (!freeN) {
                        fixed[std::size_t(g(s, a_iN[std::size_t(i)]))] = 1;
                        target(g(s, a_iN[std::size_t(i)])) = a_bcN[std::size_t(i)];
                    }
                    if (!freeP) {
                        fixed[std::size_t(g(s, a_iP[std::size_t(i)]))] = 1;
                        target(g(s, a_iP[std::size_t(i)])) = a_bcP[std::size_t(i)];
                    }
                }
            }
    for (int k = 0; k < total; ++k)
        if (fixed[std::size_t(k)]) x(k) = circuitOf[std::size_t(k)] >= 0 ? x(a_contacts[std::size_t(circuitOf[std::size_t(k)])].iC) + target(k) : target(k);
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    SparseLu<double> lu;
    bool analyzed = false;
    int crawling = 0;   // steps in a row damped to less than 2 %
    int fading = 0;     // steps in a row the photons cut to a twentieth above threshold
    // (Mobilities saturated by the force along the current: lagged a step,
    // they settle only as the carriers do - where generation sets a
    // current, J/(q n µ), a lower µ a steeper force and a lower µ again, at
    // (v/vsat)² a step; under a MOSFET's channel, carriers that carry
    // nothing cycling about 1e-4. Held once the potential has settled to
    // 1e-6 Vt, or after fifteen steps, Newton finishing with them.)
    bool held = false;
    a_heldUnsettled = false;
    int refreshed = 0;   // the held mobilities found not their solution's, and updated
    // The equations' own imbalance at x: each row's |F| against the gross
    // flows through it - |J x| summed over its terms: a node's currents in
    // and out, its charge -, the worst (the rows held to the contacts'
    // values aside), of each slice's.
    auto imbalance = [this, &x, mass, &historyOf, &fixed, S]() {
        double worst = 0;
        for (int s = 0; s < S; ++s) {
            useSlice(s);
            const Eigen::VectorXd xs = sliceOf(x, s);
            const std::vector<double>* hs = historyOf(s);
            Eigen::VectorXd G;
            std::vector<Eigen::Triplet<double>> terms;
            assemble(xs, mass, hs, G, &terms);
            Eigen::VectorXd gross = Eigen::VectorXd::Zero(a_unknowns);
            for (const auto& t : terms) gross(t.row()) += std::abs(t.value() * xs(t.col()));
            // (A step in time's ionized dopants: their change's own size - its
            // term's derivative times the carriers is no measure of it where
            // they are few, a depletion's: its rounding above 1e-6 of that, a
            // 10 ps ramp never met.)
            if (mass > 0 && hs && a_ionAt > 0)
                for (int i = 0; i < a_n; ++i) {
                    const int d = a_nodeDomain[std::size_t(i)];
                    if (d < 0 || !a_media[std::size_t(d)].incomplete) continue;
                    double nd = 0, na = 0, dnd = 0, dna = 0;
                    ionizedAt(i, xs, &nd, &dnd, &na, &dna);
                    const double A = a_box[std::size_t(i)];
                    if (a_iN[std::size_t(i)] >= 0) gross(a_iN[std::size_t(i)]) += Q * A * mass * nd;
                    if (a_iP[std::size_t(i)] >= 0) gross(a_iP[std::size_t(i)]) += Q * A * mass * na;
                }
            for (int k = 0; k < a_unknowns; ++k) {
                if (s > 0 && k >= a_sliceSize) continue;
                if (!fixed[std::size_t(globalIndex(s, k))] && gross(k) > 0) worst = std::max(worst, std::abs(G(k)) / gross(k));
            }
        }
        return worst;
    };
    // (Its equations met where it is: no step - a solution just walked to,
    // solved again at its voltages, Newton's own rounding about 1e-6 of its
    // flows, took its forty steps and failed: an avalanche diode without
    // recombination at -40 V.)
    if (met && !circuits && !consistent && imbalance() < 1e-6) {
        if (steps) *steps = 0;
        return true;
    }
    for (int it = 0; it < maxSteps; ++it) {
        // (Asked to stop: between its steps.)
        if (cancelled()) {
            if (why) *why = tr("cancelled");
            return false;
        }
        carrierScales();
        if (!held)
            for (int s = 0; s < S; ++s) {
                useSlice(s);
                updateMobilities(sliceOf(x, s));
                keepSliceEdges(s);
            }
        // Each slice assembled: its rows and columns among all; the
        // circuits' rows from the contacts' rows as assembled, before they
        // are replaced. Dirichlet rows: x - target (a circuit's contact's,
        // x - Vc - its offset).
        F = Eigen::VectorXd::Zero(total);
        std::vector<Eigen::Triplet<double>> kept;
        std::vector<CircuitCurrent> currents;
        for (int s = 0; s < S; ++s) {
            useSlice(s);
            const Eigen::VectorXd xs = sliceOf(x, s);
            Eigen::VectorXd Fs;
            assemble(xs, mass, historyOf(s), Fs, &trip);
            kept.reserve(kept.size() + trip.size() + std::size_t(a_unknowns));
            for (const auto& t : trip) {
                const int r = g(s, t.row()), c = g(s, t.col());
                if (!fixed[std::size_t(r)]) kept.emplace_back(r, c, t.value() * colScale(c));
            }
            for (int k = 0; k < a_unknowns; ++k)
                if (k < a_sliceSize || s == 0) F(g(s, k)) = Fs(k);
            if (circuits) sliceCurrents(s, sliceWeight(s), xs, mass, historyOf(s), Fs, trip, currents);
        }
        if (circuits) circuitRows(x, mass, history, currents, colScale, F, kept);
        for (int k = 0; k < total; ++k)
            if (fixed[std::size_t(k)]) {
                kept.emplace_back(k, k, colScale(k));
                if (const int ck = circuitOf[std::size_t(k)]; ck >= 0) {
                    const int ic = a_contacts[std::size_t(ck)].iC;
                    kept.emplace_back(k, ic, -colScale(ic));
                    F(k) = x(k) - x(ic) - target(k);
                } else
                    F(k) = x(k) - target(k);
            }
        Eigen::SparseMatrix<double> J(total, total);
        J.setFromTriplets(kept.begin(), kept.end());
        // Row equilibration.
        Eigen::VectorXd rowMax = Eigen::VectorXd::Zero(total);
        for (int k = 0; k < J.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator itr(J, k); itr; ++itr)
                rowMax(itr.row()) = std::max(rowMax(itr.row()), std::abs(itr.value()));
        for (int k = 0; k < total; ++k)
            if (!(rowMax(k) > 0)) rowMax(k) = 1;
        for (int k = 0; k < J.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator itr(J, k); itr; ++itr) itr.valueRef() /= rowMax(itr.row());
        const Eigen::VectorXd rhs = -(F.array() / rowMax.array()).matrix();
        if (!a_walkSaid.isEmpty()) {
            // (Its residual: each equation's by its largest term - the
            // unknowns' units, Vt and each node's own carriers.)
            double residual = 0;
            for (int k = 0; k < total; ++k)
                if (!fixed[std::size_t(k)]) residual = std::max(residual, std::abs(rhs(k)));
            told(tr("%1; Newton's step %2, its residual %3").arg(a_walkSaid).arg(it + 1).arg(QString::number(residual, 'g', 2)));
        }
        J.makeCompressed();
        if (!analyzed) {
            lu.analyzePattern(J);
            analyzed = true;
        }
        lu.factorize(J);
        if (lu.info() != Eigen::Success) {
            *why = tr("%1: its Jacobian is singular (%2)").arg(a_node->name(), QString::fromStdString(lu.lastErrorMessage()));
            return false;
        }
        Eigen::VectorXd dx = lu.solve(rhs);
        if (!dx.allFinite()) {
            *why = tr("%1: Newton's step is not finite").arg(a_node->name());
            return false;
        }
        dx = (dx.array() * colScale.array()).matrix();
        // Damping: the potential moved no more than 10 Vt at once; the
        // carriers kept positive.
        double maxDV = 0;
        for (int s = 0; s < S; ++s)
            for (int i = 0; i < a_n; ++i)
                if (a_iV[std::size_t(i)] >= 0) maxDV = std::max(maxDV, std::abs(dx(g(s, a_iV[std::size_t(i)]))));
        const double alpha = maxDV > 10 * a_Vt ? 10 * a_Vt / maxDV : 1.0;
        double changeV = 0, changeC = 0;
        if (a_iS >= 0) {
            // (The photons kept positive, as the carriers - a step that takes
            // them down past a twentieth: below threshold, their balance's
            // own number at these carriers, τp β Rsp/(1 - τp G). Held at a
            // twentieth each step, as they were, those of a reverse bias -
            // nought, Rsp nought - never met their balance: a Laser L-I's
            // point below 0 V failed, the update check of 110e8b9, 4.)
            const double before = x(a_iS);
            double after = before + alpha * dx(a_iS);
            bool cut = false;
            if (after < 0.05 * before) {
                after = 0.05 * before;
                cut = true;
                if (a_laser->ready) {
                    const LaserState ls = laserState(x);
                    const Lasing& L = *a_laser;
                    if (L.tauP * ls.G < 1) {
                        after = std::max(L.tauP * L.beta * ls.Rsp / (1 - L.tauP * ls.G), 0.0);
                        cut = false;
                    }
                }
            }
            fading = cut ? fading + 1 : 0;
            x(a_iS) = std::max(after, 0.0);
            changeC = std::max(changeC, std::abs(x(a_iS) - before) / (before + 1e-6));
        }
        for (const Contact& c : a_contacts)
            if (c.iC >= 0) {
                x(c.iC) += alpha * dx(c.iC);
                changeV = std::max(changeV, std::abs(alpha * dx(c.iC)) / a_Vt);
            }
        for (int s = 0; s < S; ++s)
            for (int i = 0; i < a_n; ++i) {
                if (a_iV[std::size_t(i)] >= 0) {
                    const int k = g(s, a_iV[std::size_t(i)]);
                    x(k) += alpha * dx(k);
                    changeV = std::max(changeV, std::abs(alpha * dx(k)) / a_Vt);
                }
                for (int kk : {a_iN[std::size_t(i)], a_iP[std::size_t(i)]}) {
                    if (kk < 0) continue;
                    const int k = g(s, kk);
                    const double before = x(k);
                    double after = before + alpha * dx(k);
                    if (after < 0.05 * before) after = 0.05 * before;
                    x(k) = after;
                    changeC = std::max(changeC, std::abs(after - before) / (before + 1e-10 * a_ni[std::size_t(i)] + 1e-300));
                }
            }
        if (steps) *steps = it + 1;
        // Ten steps in a row crawling: it is going nowhere - said at once,
        // the voltage's step taken again smaller (its forty steps were a
        // breakdown's diode's most time).
        crawling = alpha < 0.02 ? crawling + 1 : 0;
        if (crawling >= 10) break;
        // The photons cut to a twentieth eight steps in a row above threshold
        // - 2.6e10 down, where they lase -: going nowhere too (Newton's step
        // taking them to none, step after step). Each such try took its forty
        // steps: a Laser L-I of one point at 1.9 V 110 s, 72 of them in nine
        // tries at its threshold - the update check of 110e8b9, 12.
        if (fading >= 8) break;
        if (!held && a_alongCurrent && alpha == 1.0 && changeV >= 1e-6 && it >= 15) a_heldUnsettled = true;
        held = held || (a_alongCurrent && alpha == 1.0 && (changeV < 1e-6 || it >= 15));
        // Converged: the potential still to 1e-8 Vt and the carriers to 1e-6
        // (or the potential to 1e-12 Vt: the carriers' last digits are noise)
        // - and the equations met, each node's to 1e-6 of the flows through
        // it. A Jacobian not theirs (a term missing) makes the steps small
        // short of the solution: band-to-band tunnelling without its field's
        // term took a reverse current 1e4 times too large for one. On then,
        // till they are met (or no solution).
        // (A circuit's row met too: its voltages to 10⁻⁹ - its residual as
        // assembled before this step, the step itself under 10⁻⁸ Vt.)
        if (alpha == 1.0 && changeV < 1e-8 && (!circuits || a_circuitResidual < 1e-9)) {
            const double imb = imbalance();
            // (Or the equations met to 10⁻¹⁴ of their flows - their rounding:
            // what still changes is the linear solve's own. An avalanche
            // diode's mesh of 10 nm at -32 V met them to 10⁻¹⁶, its
            // carriers wandering by up to 5·10⁻⁵ step after step, the
            // potential by 10⁻¹¹ Vt: no solution found - the hunt of 19
            // October, A1. 10⁻¹² took a current of 10⁻¹³ A among majority
            // flows of 10⁻² A as it was, 15 % off.)
            const bool met = ((changeC < 1e-6 || changeV < 1e-12) && imb < 1e-6) || imb < 1e-14;
            // (Its mobilities held: the solution's own? Held from fifteen
            // steps on they may not be, and the solution theirs, not its own
            // - a sliced photodiode's first slice 0.5 % off itself solved
            // alone. Updated at it, and on, while they move by more than
            // 10⁻⁶ - at most eight times: a MOSFET's channel's cycle.)
            // (Hot carriers always, held or not: their temperatures and
            // mobilities each other's only so, a temperature halfway each
            // time - at most sixty times.)
            const bool hot = a_energy && a_energyActive;
            if (met && ((held && consistent) || hot) && refreshed < (hot ? 200 : 8)) {
                double moved = 0;
                for (int s = 0; s < S; ++s) {
                    useSlice(s);
                    std::vector<std::pair<double, double>> was(a_edges.size());
                    for (std::size_t k = 0; k < a_edges.size(); ++k) was[k] = {a_edges[k].wn, a_edges[k].wp};
                    updateMobilities(sliceOf(x, s));
                    keepSliceEdges(s);
                    for (std::size_t k = 0; k < a_edges.size(); ++k) {
                        if (was[k].first > 0) moved = std::max(moved, std::abs(a_edges[k].wn / was[k].first - 1));
                        if (was[k].second > 0) moved = std::max(moved, std::abs(a_edges[k].wp / was[k].second - 1));
                    }
                }
                if (moved > 1e-6) {
                    ++refreshed;
                    // (Hot carriers unsettled after all: not a solution.)
                    if (hot && refreshed >= 200) {
                        *why = tr("%1: its hot carriers' temperatures did not settle (their mobilities still moving by %2)")
                                   .arg(a_node->name())
                                   .arg(moved, 0, 'g', 3);
                        return false;
                    }
                    continue;
                }
            }
            if (met) return true;
        }
    }
    *why = tr("%1: Newton's method did not converge in %2 steps").arg(a_node->name()).arg(maxSteps);
    return false;
}

bool SemiconductorProblem::atEquilibrium(const std::vector<double>& goal, double* fermi) const
{
    for (double g : a_generation)
        if (g != 0) return false;
    bool any = false;
    double v = 0;
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        if (a_contacts[k].kind == Contact::Gate) continue;
        if (!any) v = goal[k];
        else if (std::abs(goal[k] - v) > 1e-12) return false;
        any = true;
    }
    *fermi = v;
    return any;
}

bool SemiconductorProblem::equilibriumPoisson(Eigen::VectorXd& x, double fermi, const std::vector<double>& applied, bool warm, QString* why)
{
    // V alone; n = ni exp((V + δ - φ)/Vt), p = ni exp((φ - V - δ)/Vt).
    contactValues(applied);
    if (!warm)
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] < 0) continue;
            double v = fermi;
            if (a_nodeDomain[std::size_t(i)] >= 0) v += a_Vt * std::asinh(doping(i) / (2 * a_ni[std::size_t(i)])) - a_delta[std::size_t(i)];
            x(a_iV[std::size_t(i)]) = v;
        }
    std::vector<int> vIndex;
    std::vector<int> local(std::size_t(a_unknowns), -1);
    for (int i = 0; i < a_n; ++i)
        if (a_iV[std::size_t(i)] >= 0) {
            local[std::size_t(a_iV[std::size_t(i)])] = int(vIndex.size());
            vIndex.push_back(i);
        }
    const int nv = int(vIndex.size());
    std::vector<char> fixed(std::size_t(nv), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            const int k = local[std::size_t(a_iV[std::size_t(i)])];
            fixed[std::size_t(k)] = 1;
            x(a_iV[std::size_t(i)]) = a_bcV[std::size_t(i)];
        }
    SparseLu<double> lu;
    bool analyzed = false;
    for (int it = 0; it < 200; ++it) {
        if (cancelled()) {
            *why = tr("cancelled");
            return false;
        }
        Eigen::VectorXd Fv;
        Eigen::SparseMatrix<double> J;
        equilibriumSystem(x, fermi, local, vIndex, fixed, false, Fv, J);
        if (!analyzed) {
            lu.analyzePattern(J);
            analyzed = true;
        }
        lu.factorize(J);
        if (lu.info() != Eigen::Success) {
            *why = tr("%1: the equilibrium's potential: a singular matrix").arg(a_node->name());
            return false;
        }
        const Eigen::VectorXd dv = lu.solve(-Fv);
        double maxDV = 0;
        for (int k = 0; k < nv; ++k) maxDV = std::max(maxDV, std::abs(dv(k)));
        const double alpha = maxDV > 5 * a_Vt ? 5 * a_Vt / maxDV : 1.0;
        for (int k = 0; k < nv; ++k) x(a_iV[std::size_t(vIndex[std::size_t(k)])]) += alpha * dv(k);
        if (alpha == 1.0 && maxDV < 1e-10 * a_Vt) {
            for (int i = 0; i < a_n; ++i) {
                if (a_iN[std::size_t(i)] < 0) continue;
                const double v = (x(a_iV[std::size_t(i)]) + a_delta[std::size_t(i)] - fermi) / a_Vt;
                double dn = 0, dp = 0;
                carriersAt(i, v, &x(a_iN[std::size_t(i)]), &x(a_iP[std::size_t(i)]), &dn, &dp);
            }
            return true;
        }
    }
    *why = tr("%1: no equilibrium potential found in 200 steps").arg(a_node->name());
    return false;
}

void SemiconductorProblem::equilibriumSystem(const Eigen::VectorXd& x0, double fermi, const std::vector<int>& local, const std::vector<int>& vIndex,
                                             const std::vector<char>& fixed, bool keepContacts, Eigen::VectorXd& Fv,
                                             Eigen::SparseMatrix<double>& J) const
{
    // The carriers from V; the full assembly's Poisson rows give F and ∂F/∂V
    // (with ∂/∂n, ∂/∂p folded in through dn/dV, dp/dV: Boltzmann's n/Vt,
    // -p/Vt; Fermi-Dirac's Nc ℱ'/Vt, -Nv ℱ'/Vt).
    Eigen::VectorXd x = x0;
    std::vector<double> dnv(std::size_t(a_n), 0.0), dpv(std::size_t(a_n), 0.0);
    for (int i = 0; i < a_n; ++i) {
        if (a_iN[std::size_t(i)] < 0) continue;
        const double v = (x(a_iV[std::size_t(i)]) + a_delta[std::size_t(i)] - fermi) / a_Vt;
        carriersAt(i, v, &x(a_iN[std::size_t(i)]), &x(a_iP[std::size_t(i)]), &dnv[std::size_t(i)], &dpv[std::size_t(i)]);
    }
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    assemble(x, 0, nullptr, F, &trip);
    const int nv = int(vIndex.size());
    Fv.resize(nv);
    for (int k = 0; k < nv; ++k) Fv(k) = fixed[std::size_t(k)] && !keepContacts ? 0.0 : F(a_iV[std::size_t(vIndex[std::size_t(k)])]);
    std::vector<Eigen::Triplet<double>> kept;
    for (const auto& t : trip) {
        const int r = local[std::size_t(t.row())];
        if (r < 0 || (fixed[std::size_t(r)] && !keepContacts)) continue;
        int c = local[std::size_t(t.col())];
        double v = t.value();
        if (c < 0) {
            // ∂/∂n or ∂/∂p of this node's V row: through n(V), p(V).
            const int node = vIndex[std::size_t(r)];
            const bool isN = a_iN[std::size_t(node)] == t.col();
            if (!isN && a_iP[std::size_t(node)] != t.col()) continue;
            v *= (isN ? dnv[std::size_t(node)] : dpv[std::size_t(node)]) / a_Vt;
            c = r;
        }
        kept.emplace_back(r, c, v);
    }
    if (!keepContacts)
        for (int k = 0; k < nv; ++k)
            if (fixed[std::size_t(k)]) kept.emplace_back(k, k, 1.0);
    J.resize(nv, nv);
    J.setFromTriplets(kept.begin(), kept.end());
    J.makeCompressed();
}

void SemiconductorProblem::quasiStatic(const Eigen::VectorXd& x, double fermi, const std::vector<double>& applied)
{
    a_C0.clear();
    a_gates.clear();
    for (int k = 0; k < int(a_contacts.size()); ++k)
        if (a_contacts[std::size_t(k)].kind == Contact::Gate) a_gates.push_back(k);
    if (a_gates.empty()) return;
    contactValues(applied);
    std::vector<int> vIndex;
    std::vector<int> local(std::size_t(a_unknowns), -1);
    for (int i = 0; i < a_n; ++i)
        if (a_iV[std::size_t(i)] >= 0) {
            local[std::size_t(a_iV[std::size_t(i)])] = int(vIndex.size());
            vIndex.push_back(i);
        }
    const int nv = int(vIndex.size());
    std::vector<char> fixed(std::size_t(nv), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) fixed[std::size_t(local[std::size_t(a_iV[std::size_t(i)])])] = 1;
    Eigen::VectorXd Fv;
    Eigen::SparseMatrix<double> J, full;
    equilibriumSystem(x, fermi, local, vIndex, fixed, false, Fv, J);
    equilibriumSystem(x, fermi, local, vIndex, fixed, true, Fv, full);
    SparseLu<double> lu;
    lu.compute(J);
    if (lu.info() != Eigen::Success) return;
    // Each gate at 1 V more, the other contacts held: each gate's charge.
    const Eigen::SparseMatrix<double, Eigen::RowMajor> rows = full;
    a_C0.assign(a_gates.size(), std::vector<double>(a_gates.size(), 0.0));
    for (std::size_t b = 0; b < a_gates.size(); ++b) {
        Eigen::VectorXd rhs = Eigen::VectorXd::Zero(nv);
        for (int i : a_contacts[std::size_t(a_gates[b])].nodes) rhs(local[std::size_t(a_iV[std::size_t(i)])]) = 1;
        const Eigen::VectorXd dv = lu.solve(rhs);
        for (std::size_t a = 0; a < a_gates.size(); ++a) {
            double dq = 0;
            for (int i : a_contacts[std::size_t(a_gates[a])].nodes) {
                const int r = local[std::size_t(a_iV[std::size_t(i)])];
                for (Eigen::SparseMatrix<double, Eigen::RowMajor>::InnerIterator it(rows, r); it; ++it) dq -= it.value() * dv(it.col());
            }
            a_C0[a][b] = dq;
        }
    }
}

Eigen::VectorXd SemiconductorProblem::stateVector() const
{
    const Field& f = a_s.fields()[std::size_t(a_fi)];
    const std::size_t N = std::size_t(a_n);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(a_total);
    for (int i = 0; i < a_n; ++i) {
        if (a_iV[std::size_t(i)] >= 0) x(a_iV[std::size_t(i)]) = f.values[std::size_t(i)];
        if (a_iN[std::size_t(i)] >= 0) x(a_iN[std::size_t(i)]) = f.values[N + std::size_t(i)];
        if (a_iP[std::size_t(i)] >= 0) x(a_iP[std::size_t(i)]) = f.values[2 * N + std::size_t(i)];
    }
    if (a_iS >= 0 && f.values.size() > 9 * N) x(a_iS) = f.values[9 * N];
    // (A circuit's contact's own voltage: as solved, else its source's.)
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        if (c.iC < 0) continue;
        x(c.iC) = f.solved && a_circuitAt > 0 && f.values.size() >= a_circuitAt + 3 * a_contacts.size() ? f.values[a_circuitAt + 3 * k] : c.applied;
    }
    // (The other slices': their blocks as solved - their carriers there -,
    // else the first's.)
    for (int s = 1; s < a_slices; ++s) {
        const std::size_t at = a_slicesAt + std::size_t(s - 1) * a_sliceBlock;
        bool kept = f.solved && f.values.size() >= at + a_sliceBlock;
        for (int i = 0; kept && i < a_n; ++i)
            if (a_iN[std::size_t(i)] >= 0) {
                kept = f.values[at + N + std::size_t(i)] > 0;
                break;
            }
        for (int i = 0; i < a_n; ++i) {
            const std::size_t k = std::size_t(i);
            if (a_iV[k] >= 0) x(globalIndex(s, a_iV[k])) = kept ? f.values[at + k] : x(a_iV[k]);
            if (a_iN[k] >= 0) {
                x(globalIndex(s, a_iN[k])) = kept ? f.values[at + N + k] : x(a_iN[k]);
                x(globalIndex(s, a_iP[k])) = kept ? f.values[at + 2 * N + k] : x(a_iP[k]);
            }
        }
    }
    return x;
}

void SemiconductorProblem::storeIons(const Eigen::VectorXd& x)
{
    Field& f = field();
    const std::size_t N = std::size_t(a_n);
    if (a_ionAt == 0 || f.values.size() < a_ionAt + 2 * N) return;
    for (int i = 0; i < a_n; ++i) {
        double nd = 0, na = 0, dn = 0, dp = 0;
        if (a_nodeDomain[std::size_t(i)] >= 0) ionizedAt(i, x, &nd, &dn, &na, &dp);
        f.values[a_ionAt + std::size_t(i)] = nd;
        f.values[a_ionAt + N + std::size_t(i)] = na;
    }
}

void SemiconductorProblem::store(const Eigen::VectorXd& x)
{
    Field& f = field();
    const std::size_t N = std::size_t(a_n);
    degeneracy(x);
    for (int i = 0; i < a_n; ++i) {
        f.values[std::size_t(i)] = a_iV[std::size_t(i)] >= 0 ? x(a_iV[std::size_t(i)]) : 0.0;
        f.values[N + std::size_t(i)] = a_iN[std::size_t(i)] >= 0 ? x(a_iN[std::size_t(i)]) : 0.0;
        f.values[2 * N + std::size_t(i)] = a_iP[std::size_t(i)] >= 0 ? x(a_iP[std::size_t(i)]) : 0.0;
        // (The bands' offsets with Fermi-Dirac's degeneracy and the quantum
        // potentials in them: φn, φp and the currents read at a point as
        // solved.)
        if (a_fermi || !a_quantumNodes.empty()) {
            f.values[5 * N + std::size_t(i)] = a_thetaN[std::size_t(i)] + a_Vt * a_lgN[std::size_t(i)];
            f.values[6 * N + std::size_t(i)] = a_thetaP[std::size_t(i)] - a_Vt * a_lgP[std::size_t(i)];
        }
    }
    storeIons(x);
    // A laser's photons, and its gain and stimulated rate at each node of its
    // domains (nought elsewhere).
    if (a_iS >= 0 && f.values.size() > 9 * N) {
        f.values[9 * N] = x(a_iS);
        std::fill(f.values.begin() + std::ptrdiff_t(7 * N), f.values.begin() + std::ptrdiff_t(9 * N), 0.0);
        const Lasing& L = *a_laser;
        if (L.ready) {
            std::vector<double> volume(N, 0.0);
            for (std::size_t e = 0; e < L.nodes.size(); ++e) {
                const std::size_t i = std::size_t(L.nodes[e]);
                if (a_iN[i] < 0) continue;
                const double g = laserGain(x(a_iN[i]), x(a_iP[i]), nullptr, nullptr);
                f.values[7 * N + i] = g;
                // (Per volume: the node's share of the mode over its part of the domains' volume.)
                if (L.volume[e] > 0) f.values[8 * N + i] = x(a_iS) * L.speed[e] * L.share[e] * g / L.volume[e];
            }
        }
    }
    f.solved = true;
    // The carriers' Joule heat by triangle: each edge's currents times their
    // quasi-Fermi potentials' drop along it - the edges' sum the contacts'
    // I V less the pairs' recombination heat, exactly - shared among the
    // edge's triangles as their conductances are, over each one's volume.
    // (J²/(q n µ) at a point took a depletion region's drift and diffusion,
    // cancelling, for current: its heat thousands of times too much.) Cut
    // into slices, their mean by their shares of the length: a cross-
    // section's heat is the whole length's.
    // With it the pairs' recombination's, q R (φp - φn), by the nodes as
    // assembled - their sum with the edges' I V exactly -, each triangle's
    // the mean of its corners': semi.H as solved (read at a point the
    // first slice's carriers alone would be, sliced; and R there, between
    // the nodes, took ∫ semi.H to 1e-3 of I V only).
    // And the light's, by the nodes too: the energy each node's pairs are
    // made with less what they carry, q G (φp - φn) - a contact's all of
    // it, its pairs at once its own -, over the node's corners' volume: ∫
    // semi.H the contacts' I V and the power absorbed together, exactly.
    std::vector<double> heatOf(a_elements.size(), 0.0), wholeOf(a_elements.size(), 0.0), lightOf(a_elements.size(), 0.0);
    std::vector<double> corners(N, 0.0);
    for (int t = 0; t < int(a_elements.size()); ++t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0 || !(em.volume > 0)) continue;
        const int* tdof = field().space->dofs(t);
        for (int k = 0; k < 3; ++k) corners[std::size_t(tdof[k])] += em.volume / 3;
    }
    for (int sl = 0; sl < a_slices; ++sl) {
        const Eigen::VectorXd sliced = a_slices > 1 ? sliceOf(x, sl) : Eigen::VectorXd();
        const Eigen::VectorXd& xx = a_slices > 1 ? sliced : x;
        if (a_slices > 1) useSlice(sl);
        {
            // (Its recombination by node, a_recombined; the pairs' currents kept.)
            const double generated = a_genCurrent, ionized = a_ionCurrent;
            Eigen::VectorXd Fs;
            assemble(xx, 0, nullptr, Fs, nullptr);
            a_genCurrent = generated;
            a_ionCurrent = ionized;
        }
        std::vector<double> phin(N, 0.0), phip(N, 0.0);
        for (std::size_t i = 0; i < N; ++i) {
            if (a_iN[i] < 0) continue;
            phin[i] = xx(a_iV[i]) + a_thetaN[i] + a_Vt * a_lgN[i] - a_Vt * std::log(std::max(xx(a_iN[i]), 1e-300) / a_refNi);
            phip[i] = xx(a_iV[i]) + a_thetaP[i] - a_Vt * a_lgP[i] + a_Vt * std::log(std::max(xx(a_iP[i]), 1e-300) / a_refNi);
        }
        std::vector<double> pn(a_edges.size(), 0.0), pp(a_edges.size(), 0.0);
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const Edge& e = a_edges[k];
            if (e.w <= 0) continue;
            double dN = 0, dP = 0;
            edgeSteps(e, xx, &dN, &dP);
            const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
            const double fn = e.wn * (e.fNj * xx(a_iN[j]) * bern(dN) - e.fNi * xx(a_iN[i]) * bern(-dN));
            const double fp = e.wp * (e.fPi * xx(a_iP[i]) * bern(dP) - e.fPj * xx(a_iP[j]) * bern(-dP));
            pn[k] = fn * (phin[i] - phin[j]);
            pp[k] = fp * (phip[i] - phip[j]);
        }
        // (Each triangle's share of an edge's: its conductance's part of
        // their sum, its mobilities at the solution - not over the edge's
        // own, its mobilities held where saturated: the shares' sum not one,
        // ∫ semi.H 2e-6 of I V off.)
        std::vector<std::array<double, 2>> mobility(a_elements.size(), {0.0, 0.0});
        std::vector<double> sumN(a_edges.size(), 0.0), sumP(a_edges.size(), 0.0);
        for (int t = 0; t < int(a_elements.size()); ++t) {
            const ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0) continue;
            elementMobilities(t, &xx, &mobility[std::size_t(t)][0], &mobility[std::size_t(t)][1]);
            for (int k = 0; k < 3; ++k) {
                sumN[std::size_t(em.edge[k])] += em.cot[k] * mobility[std::size_t(t)][0];
                sumP[std::size_t(em.edge[k])] += em.cot[k] * mobility[std::size_t(t)][1];
            }
        }
        for (int t = 0; t < int(a_elements.size()); ++t) {
            const ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0 || f.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1)) || !(em.volume > 0)) continue;
            const double mun = mobility[std::size_t(t)][0], mup = mobility[std::size_t(t)][1];
            double heat = 0;
            for (int k = 0; k < 3; ++k) {
                const std::size_t ek = std::size_t(em.edge[k]);
                const Edge& e = a_edges[ek];
                if (e.wn > 0) heat += em.cot[k] * mun * (sumN[ek] > 0 ? 1 / sumN[ek] : Q * a_Vt / e.wn) * pn[ek];
                if (e.wp > 0) heat += em.cot[k] * mup * (sumP[ek] > 0 ? 1 / sumP[ek] : Q * a_Vt / e.wp) * pp[ek];
            }
            heatOf[std::size_t(t)] += sliceWeight(sl) * heat / em.volume;
            if (a_recombined.size() == N) {
                double recombination = 0;
                const int* tdof = field().space->dofs(t);
                for (int k = 0; k < 3; ++k) {
                    const std::size_t c = std::size_t(tdof[k]);
                    if (a_iN[c] < 0 || a_nodeContact[c] >= 0 || !(corners[c] > 0)) continue;
                    recombination += Q * a_recombined[c] / corners[c] * (phip[c] - phin[c]) / 3;
                }
                wholeOf[std::size_t(t)] += sliceWeight(sl) * (heat / em.volume + recombination);
            }
            const std::vector<double>& G = generationNow();
            const std::vector<double>& made = pairEnergyNow();
            if (made.size() == N && G.size() == N) {
                double light = 0;
                const int* tdof = field().space->dofs(t);
                for (int k = 0; k < 3; ++k) {
                    const std::size_t c = std::size_t(tdof[k]);
                    if (a_iN[c] < 0 || !(corners[c] > 0)) continue;
                    const double kept = a_nodeContact[c] >= 0 ? 0.0 : Q * G[c] * (phip[c] - phin[c]);
                    light += a_box[c] * a_lightScale * (made[c] - kept) / corners[c] / 3;
                }
                lightOf[std::size_t(t)] += sliceWeight(sl) * light;
                wholeOf[std::size_t(t)] += sliceWeight(sl) * light;
            }
        }
    }
    for (int t = 0; t < int(a_elements.size()); ++t)
        if (a_elements[std::size_t(t)].domain >= 0 && f.extra.size() >= std::size_t(ExtraHead + PerTriangle * (t + 1)) && a_elements[std::size_t(t)].volume > 0)
        {
            f.extra[std::size_t(ExtraHead + PerTriangle * t + 18)] = heatOf[std::size_t(t)];
            f.extra[std::size_t(ExtraHead + PerTriangle * t + 33)] = a_recombined.size() == N ? wholeOf[std::size_t(t)] : NaN;
            f.extra[std::size_t(ExtraHead + PerTriangle * t + 34)] = lightOf[std::size_t(t)];
        }
    // (The other slices' own blocks; the first slice's then as assembled.)
    for (int sl = 1; sl < a_slices; ++sl) {
        const std::size_t at = a_slicesAt + std::size_t(sl - 1) * a_sliceBlock;
        if (f.values.size() < at + a_sliceBlock) break;
        const Eigen::VectorXd xs = sliceOf(x, sl);
        degeneracy(xs);
        for (int i = 0; i < a_n; ++i) {
            const std::size_t k = std::size_t(i);
            f.values[at + k] = a_iV[k] >= 0 ? xs(a_iV[k]) : 0.0;
            f.values[at + N + k] = a_iN[k] >= 0 ? xs(a_iN[k]) : 0.0;
            f.values[at + 2 * N + k] = a_iP[k] >= 0 ? xs(a_iP[k]) : 0.0;
            if (a_ionAt > 0) {
                double nd = 0, na = 0, dn = 0, dp = 0;
                if (a_nodeDomain[k] >= 0) ionizedAt(i, xs, &nd, &dn, &na, &dp);
                f.values[at + 3 * N + k] = nd;
                f.values[at + 4 * N + k] = na;
            }
        }
    }
    if (a_slices > 1) {
        useSlice(0);
        degeneracy(x);
    }
    // Impact ionization's pairs by triangle, as solved (semi.Gion: where an
    // avalanche makes them).
    if (a_ionization) {
        // (The pairs' currents as the slices together had them, kept.)
        const double generated = a_genCurrent, ionized = a_ionCurrent;
        Eigen::VectorXd F;
        assemble(a_slices > 1 ? sliceOf(x, 0) : x, 0, nullptr, F, nullptr);
        a_genCurrent = generated;
        a_ionCurrent = ionized;
        for (int t = 0; t < int(a_elements.size()) && std::size_t(t) < a_ionRate.size(); ++t)
            if (f.extra.size() >= std::size_t(ExtraHead + PerTriangle * (t + 1))) f.extra[std::size_t(ExtraHead + PerTriangle * t + 32)] = a_ionRate[std::size_t(t)];
    }
    // Hot carriers' temperatures as solved, the first slice's (each
    // triangle its corners' mean): semi.Tn, semi.Tp.
    if (a_energy) {
        if (a_slices > 1) useSlice(0);
        const Eigen::VectorXd x0 = a_slices > 1 ? sliceOf(x, 0) : x;
        degeneracy(x0);
        carrierTemperatures(x0);
        const Space& tsp = *f.space;
        for (int t = 0; t < int(a_elements.size()); ++t) {
            if (a_elements[std::size_t(t)].domain < 0 || f.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1))) continue;
            const int* tdof = tsp.dofs(t);
            double* e = &f.extra[std::size_t(ExtraHead + PerTriangle * t)];
            e[35] = a_Tn.size() == N ? (a_Tn[std::size_t(tdof[0])] + a_Tn[std::size_t(tdof[1])] + a_Tn[std::size_t(tdof[2])]) / 3 : NaN;
            e[36] = a_Tp.size() == N ? (a_Tp[std::size_t(tdof[0])] + a_Tp[std::size_t(tdof[1])] + a_Tp[std::size_t(tdof[2])]) / 3 : NaN;
        }
    }
    // The mobilities as solved, for what is read at a point (semi.mun, the
    // current densities).
    if (a_varying || a_TL.isValid())
        for (int t = 0; t < int(a_elements.size()); ++t) {
            if (a_elements[std::size_t(t)].domain < 0 || f.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1))) continue;
            double* e = &f.extra[std::size_t(ExtraHead + PerTriangle * t)];
            elementMobilities(t, &x, &e[0], &e[1]);
        }
    // (A magnetic field's: each triangle's c, its current density's turn.)
    if (a_Bz != 0)
        for (int t = 0; t < int(a_elements.size()); ++t) {
            const ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0 || em.step || f.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1))) continue;
            double* e = &f.extra[std::size_t(ExtraHead + PerTriangle * t)];
            double mun = 0, mup = 0;
            elementMobilities(t, &x, &mun, &mup);
            const Medium& m = a_media[std::size_t(em.domain)];
            e[37] = -m.rHn * mun * a_Bz;
            e[38] = m.rHp * mup * a_Bz;
        }
    for (std::size_t k = 0; k < a_contacts.size() && k < f.terminals.size(); ++k) {
        f.terminals[k].potential = a_contacts[k].applied;
        f.terminals[k].charge = a_contacts[k].current;
        f.terminals[k].chargeIm = a_contacts[k].charge;
    }
    // A circuit's contact's own voltage, and its bias tee's current at rest.
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        Contact& c = a_contacts[k];
        if (c.iC < 0) {
            c.voltage = c.applied;
            continue;
        }
        c.voltage = x(c.iC);
        if (a_circuitAt > 0 && f.values.size() >= a_circuitAt + 3 * a_contacts.size()) {
            f.values[a_circuitAt + 3 * k] = c.voltage;
            f.values[a_circuitAt + 3 * k + 1] = c.teeCurrent;
            f.values[a_circuitAt + 3 * k + 2] = c.solvedR;
        }
    }
}

std::vector<std::vector<double>> SemiconductorProblem::currentWeights(const Eigen::VectorXd& x)
{
    const std::size_t nc = a_contacts.size();
    std::vector<std::vector<double>> weight(nc);
    // Held: each node of a contact that holds carriers (ohmic, Schottky);
    // the semiconductor's other nodes free.
    std::vector<int> held(std::size_t(a_n), -1), local(std::size_t(a_n), -1);
    std::vector<int> columns(nc, -1);
    int carriersHeld = 0;
    for (std::size_t k = 0; k < nc; ++k) {
        if (a_contacts[k].kind == Contact::Gate) continue;
        columns[k] = carriersHeld++;
        for (int i : a_contacts[k].nodes) held[std::size_t(i)] = int(k);
    }
    int free = 0;
    for (int i = 0; i < a_n; ++i)
        if (a_nodeDomain[std::size_t(i)] >= 0 && held[std::size_t(i)] < 0) local[std::size_t(i)] = free++;
    // Each edge's conductance: its currents' change with the quasi-Fermi
    // potentials' difference (n e^(..) B weighed), relative to the largest
    // and at least 10^-12 of it (a junction's depletion has less: the
    // weight's step is there all the same).
    std::vector<double> g(a_edges.size(), 0.0);
    double largest = 0;
    for (std::size_t k = 0; k < a_edges.size(); ++k) {
        const Edge& e = a_edges[k];
        if (e.w <= 0) continue;
        const int i = e.i, j = e.j;
        double dN = 0, dP = 0;
        edgeSteps(e, x, &dN, &dP);
        const double ni = e.fNi * x(a_iN[std::size_t(i)]), nj = e.fNj * x(a_iN[std::size_t(j)]);
        const double pi = e.fPi * x(a_iP[std::size_t(i)]), pj = e.fPj * x(a_iP[std::size_t(j)]);
        g[k] = e.wn * 0.5 * (ni * bern(-dN) + nj * bern(dN)) + e.wp * 0.5 * (pi * bern(dP) + pj * bern(-dP));
        if (std::isfinite(g[k])) largest = std::max(largest, g[k]);
    }
    for (double& v : g) v = largest > 0 && std::isfinite(v) ? std::max(v / largest, 1e-12) : 1e-12;
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(free, std::max(carriersHeld, 1));
    if (free > 0 && carriersHeld > 0) {
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(4 * a_edges.size() + std::size_t(free));
        for (std::size_t k = 0; k < a_edges.size(); ++k) {
            const int i = a_edges[k].i, j = a_edges[k].j;
            const int li = local[std::size_t(i)], lj = local[std::size_t(j)];
            const double gk = g[k];
            if (li >= 0) trip.emplace_back(li, li, gk);
            if (lj >= 0) trip.emplace_back(lj, lj, gk);
            if (li >= 0 && lj >= 0) {
                trip.emplace_back(li, lj, -gk);
                trip.emplace_back(lj, li, -gk);
            } else if (li >= 0 && held[std::size_t(j)] >= 0) {
                rhs(li, columns[std::size_t(held[std::size_t(j)])]) += gk;
            } else if (lj >= 0 && held[std::size_t(i)] >= 0) {
                rhs(lj, columns[std::size_t(held[std::size_t(i)])]) += gk;
            }
        }
        // (A little to nought everywhere: a region no contact reaches is
        // weighed nought, not singular.)
        for (int k = 0; k < free; ++k) trip.emplace_back(k, k, 1e-14);
        Eigen::SparseMatrix<double> L(free, free);
        L.setFromTriplets(trip.begin(), trip.end());
        if (a_weightSize != free) {
            a_weightSolver.analyzePattern(L);
            a_weightSize = free;
        }
        a_weightSolver.factorize(L);
        if (a_weightSolver.info() == Eigen::Success) rhs = a_weightSolver.solve(rhs);
        else rhs.setZero();
    }
    for (std::size_t k = 0; k < nc; ++k) {
        if (columns[k] < 0) continue;
        std::vector<double>& w = weight[k];
        w.assign(std::size_t(a_n), 0.0);
        for (int i = 0; i < a_n; ++i) {
            if (held[std::size_t(i)] >= 0) w[std::size_t(i)] = held[std::size_t(i)] == int(k) ? 1.0 : 0.0;
            else if (local[std::size_t(i)] >= 0) w[std::size_t(i)] = std::clamp(rhs(local[std::size_t(i)], columns[k]), 0.0, 1.0);
        }
    }
    return weight;
}

void SemiconductorProblem::displacementCurrents(const Eigen::VectorXd& x, double mass, const std::vector<double>& history)
{
    // Q at the step's start from the history's charge: the BDF
    // formula applied to Q (affine in the unknowns) is mass Q(x) - Q(history).
    const std::vector<double>& hv = history;
    Eigen::VectorXd xh = Eigen::VectorXd::Zero(a_unknowns);
    const std::size_t N = std::size_t(a_n);
    for (int i = 0; i < a_n; ++i) {
        if (a_iV[std::size_t(i)] >= 0) xh(a_iV[std::size_t(i)]) = hv[std::size_t(i)];
        if (a_iN[std::size_t(i)] >= 0) xh(a_iN[std::size_t(i)]) = hv[N + std::size_t(i)];
        if (a_iP[std::size_t(i)] >= 0) xh(a_iP[std::size_t(i)]) = hv[2 * N + std::size_t(i)];
    }
    // The charges' linear parts: Poisson's rows less the dopants' ions
    // (their charge, not affine where they are ionized in part, of v
    // itself: at the history's v - the carriers over the step - it
    // was nonsense, a junction's contacts' currents 14 times C dV/dt
    // and not each other's).
    auto charges = [this](const Eigen::VectorXd& v) {
        Eigen::VectorXd F;
        assemble(v, 0, nullptr, F, nullptr);
        std::vector<double> q(a_contacts.size(), 0);
        for (std::size_t k = 0; k < a_contacts.size(); ++k)
            for (int i : a_contacts[k].nodes) {
                double fv = F(a_iV[std::size_t(i)]);
                if (a_nodeDomain[std::size_t(i)] >= 0) {
                    double nd = 0, na = 0, dnd = 0, dna = 0;
                    ionizedAt(i, v, &nd, &dnd, &na, &dna);
                    fv -= Q * a_box[std::size_t(i)] * (nd - na);
                }
                q[k] -= fv;
            }
        return q;
    };
    const std::vector<double> now = charges(x), before = charges(xh);
    for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].current += mass * now[k] - before[k];
    // Their ions' change, by their own history (incomplete ionization's).
    if (a_ionAt > 0 && hv.size() >= a_ionAt + 2 * N) {
        Eigen::VectorXd F;
        assemble(x, 0, nullptr, F, nullptr);
        for (std::size_t k = 0; k < a_contacts.size(); ++k)
            for (int i : a_contacts[k].nodes) {
                if (a_nodeDomain[std::size_t(i)] < 0 || !a_media[std::size_t(a_nodeDomain[std::size_t(i)])].incomplete) continue;
                double nd = 0, na = 0, dnd = 0, dna = 0;
                ionizedAt(i, x, &nd, &dnd, &na, &dna);
                const double dNd = mass * nd - hv[a_ionAt + std::size_t(i)], dNa = mass * na - hv[a_ionAt + N + std::size_t(i)];
                a_contacts[k].current -= Q * a_box[std::size_t(i)] * (dNd - dNa);
            }
    }
}

void SemiconductorProblem::terminalAll(const Eigen::VectorXd& X, double mass, const std::vector<double>* history, bool still)
{
    const std::size_t nc = a_contacts.size();
    const bool inTime = mass > 0 && history;
    if (a_slices <= 1) {
        terminalValues(X, mass, history, still);
        if (inTime) displacementCurrents(X, mass, *history);
        a_sliceCurrent.clear();
        return;
    }
    // Each slice's at its own vector (its edges' weights, its history), its
    // currents and charges weighed by its share of the length - and the
    // pairs' currents (impact ionization's, the others') with them.
    std::vector<double> I(nc, 0.0), charge(nc, 0.0);
    a_sliceCurrent.assign(std::size_t(a_slices), std::vector<double>(nc, 0.0));
    double generated = 0, ionized = 0;
    for (int s = 0; s < a_slices; ++s) {
        useSlice(s);
        const Eigen::VectorXd xs = sliceOf(X, s);
        std::vector<double> hs;
        if (inTime) hs = sliceHistory(*history, s);
        terminalValues(xs, mass, inTime ? &hs : nullptr, still);
        if (inTime) displacementCurrents(xs, mass, hs);
        const double w = sliceWeight(s);
        for (std::size_t k = 0; k < nc; ++k) {
            I[k] += w * a_contacts[k].current;
            charge[k] += w * a_contacts[k].charge;
            a_sliceCurrent[std::size_t(s)][k] = w * a_contacts[k].current;
        }
        generated += w * a_genCurrent;
        ionized += w * a_ionCurrent;
    }
    for (std::size_t k = 0; k < nc; ++k) {
        a_contacts[k].current = I[k];
        a_contacts[k].charge = charge[k];
    }
    a_genCurrent = generated;
    a_ionCurrent = ionized;
    useSlice(0);
}

void SemiconductorProblem::terminalValues(const Eigen::VectorXd& x, double mass, const std::vector<double>* history, bool still)
{
    Eigen::VectorXd F;
    assemble(x, mass, history, F, nullptr);
    for (Contact& c : a_contacts) {
        double Qc = 0;
        for (int i : c.nodes) Qc -= F(a_iV[std::size_t(i)]);
        c.charge = Qc;
        c.current = 0;
    }
    // At equilibrium no current flows: nought, not the rounding of the
    // contacts' fluxes (-291 pA on a 10^19 cm^-3 one).
    if (still) return;
    // Each contact's: Σ over the edges of their currents times its weight's
    // change, and the carriers' change in time where it weighs (the
    // recombination's share is the electrons' and the holes' alike: none).
    const std::vector<std::vector<double>> weight = currentWeights(x);
    const std::size_t N = std::size_t(a_n);
    std::vector<double> hall;   // (a magnetic field's turned currents)
    if (a_Bz != 0) hallFluxes(x, hall, nullptr);
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const std::vector<double>& w = weight[k];
        if (w.empty()) continue;
        double I = 0;
        if (!hall.empty())
            for (std::size_t m = 0; m < a_edges.size(); ++m) I += (hall[2 * m] + hall[2 * m + 1]) * (w[std::size_t(a_edges[m].i)] - w[std::size_t(a_edges[m].j)]);
        for (const Edge& e : a_edges) {
            if (e.w <= 0) continue;
            const double dw = w[std::size_t(e.i)] - w[std::size_t(e.j)];
            if (dw == 0) continue;
            const int i = e.i, j = e.j;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            const double fn = e.wn * (e.fNj * x(a_iN[std::size_t(j)]) * bern(dN) - e.fNi * x(a_iN[std::size_t(i)]) * bern(-dN));
            const double fp = e.wp * (e.fPi * x(a_iP[std::size_t(i)]) * bern(dP) - e.fPj * x(a_iP[std::size_t(j)]) * bern(-dP));
            I += (fn + fp) * dw;
        }
        if (mass > 0 && history)
            for (int i = 0; i < a_n; ++i) {
                if (w[std::size_t(i)] == 0 || a_iN[std::size_t(i)] < 0) continue;
                const double dn = mass * x(a_iN[std::size_t(i)]) - (*history)[N + std::size_t(i)];
                const double dp = mass * x(a_iP[std::size_t(i)]) - (*history)[2 * N + std::size_t(i)];
                I += w[std::size_t(i)] * Q * (a_boxP[std::size_t(i)] * dp - a_boxN[std::size_t(i)] * dn);
                if (a_ionAt > 0 && history->size() >= a_ionAt + 2 * N && a_media[std::size_t(a_nodeDomain[std::size_t(i)])].incomplete) {
                    double nd = 0, na = 0, dnd = 0, dna = 0;
                    ionizedAt(i, x, &nd, &dnd, &na, &dna);
                    const double dNd = mass * nd - (*history)[a_ionAt + std::size_t(i)], dNa = mass * na - (*history)[a_ionAt + N + std::size_t(i)];
                    I += w[std::size_t(i)] * Q * a_box[std::size_t(i)] * (dNd - dNa);
                }
            }
        a_contacts[k].current = I;
    }
}

void SemiconductorProblem::sliceCurrents(int s, double share, const Eigen::VectorXd& x, double mass, const std::vector<double>* history,
                                         const Eigen::VectorXd& F, const std::vector<Eigen::Triplet<double>>& trip, std::vector<CircuitCurrent>& out)
{
    const std::size_t N = std::size_t(a_n);
    const bool inTime = mass > 0 && history;
    const std::vector<std::vector<double>> weight = currentWeights(x);
    // In time, each circuit's metal's charge: its nodes' Poisson rows as
    // assembled, -F (their entries -J), less their dopants' ions - affine in
    // the unknowns: at the history's state its value at x plus its entries
    // times the difference (as the step's own current takes it).
    std::map<int, int> contactOfRow;
    std::vector<std::map<int, double>> charge(a_contacts.size());
    Eigen::VectorXd xh;
    if (inTime) {
        for (std::size_t k = 0; k < a_contacts.size(); ++k)
            if (a_contacts[k].iC >= 0)
                for (int i : a_contacts[k].nodes) contactOfRow[a_iV[std::size_t(i)]] = int(k);
        for (const auto& t : trip)
            if (const auto it = contactOfRow.find(t.row()); it != contactOfRow.end()) charge[std::size_t(it->second)][t.col()] -= t.value();
        xh = Eigen::VectorXd::Zero(a_unknowns);
        for (int i = 0; i < a_n; ++i) {
            if (a_iV[std::size_t(i)] >= 0) xh(a_iV[std::size_t(i)]) = (*history)[std::size_t(i)];
            if (a_iN[std::size_t(i)] >= 0) xh(a_iN[std::size_t(i)]) = (*history)[N + std::size_t(i)];
            if (a_iP[std::size_t(i)] >= 0) xh(a_iP[std::size_t(i)]) = (*history)[2 * N + std::size_t(i)];
        }
    }
    out.resize(a_contacts.size());
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        if (c.iC < 0) continue;
        // Its current into the device, I, and its change with the unknowns:
        // the edges' currents (Scharfetter and Gummel's) times its weight's
        // change along them - the weights held (any one, 1 on the contact
        // and 0 on the others, gives the current where the equations hold).
        std::map<int, double> dI;
        double I = 0;
        // (Its terms' sizes summed: its rounding's measure - in time a heavily
        // doped region's carriers' change, each node's tens of amperes at a
        // femtosecond's step, cancel to its current: their last digits times
        // a megohm are microvolts.)
        double gross = 0;
        const std::vector<double>& w = weight[k];
        if (!w.empty()) {
            for (const Edge& e : a_edges) {
                if (e.w <= 0) continue;
                const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
                const double dw = w[i] - w[j];
                if (dw == 0) continue;
                double dN = 0, dP = 0;
                edgeSteps(e, x, &dN, &dP);
                const int ni = a_iN[i], nj = a_iN[j], pi = a_iP[i], pj = a_iP[j];
                const double fn = e.wn * (e.fNj * x(nj) * bern(dN) - e.fNi * x(ni) * bern(-dN));
                const double fp = e.wp * (e.fPi * x(pi) * bern(dP) - e.fPj * x(pj) * bern(-dP));
                I += (fn + fp) * dw;
                gross += (std::abs(fn) + std::abs(fp)) * std::abs(dw);
                // (Their change with each step dN, dP: the potential's, and
                // Fermi-Dirac's ln γ at each end with its carriers.)
                const double gN = e.wn * (e.fNj * x(nj) * dbern(dN) + e.fNi * x(ni) * dbern(-dN));
                const double gP = e.wp * (e.fPi * x(pi) * dbern(dP) + e.fPj * x(pj) * dbern(-dP));
                dI[nj] += dw * (e.wn * e.fNj * bern(dN) + gN * a_dlgN[j]);
                dI[ni] += dw * (-e.wn * e.fNi * bern(-dN) - gN * a_dlgN[i]);
                dI[pi] += dw * (e.wp * e.fPi * bern(dP) + gP * a_dlgP[i]);
                dI[pj] += dw * (-e.wp * e.fPj * bern(-dP) - gP * a_dlgP[j]);
                dI[a_iV[j]] += dw * (gN + gP) / a_Vt;
                dI[a_iV[i]] -= dw * (gN + gP) / a_Vt;
            }
            // (In time the carriers' change where it weighs, and their dopants'.)
            if (inTime)
                for (int i = 0; i < a_n; ++i) {
                    const std::size_t s = std::size_t(i);
                    if (w[s] == 0 || a_iN[s] < 0) continue;
                    const int rn = a_iN[s], rp = a_iP[s];
                    I += w[s] * Q * (a_boxP[s] * (mass * x(rp) - (*history)[2 * N + s]) - a_boxN[s] * (mass * x(rn) - (*history)[N + s]));
                    gross += std::abs(w[s]) * Q *
                             (a_boxP[s] * (std::abs(mass * x(rp)) + std::abs((*history)[2 * N + s])) + a_boxN[s] * (std::abs(mass * x(rn)) + std::abs((*history)[N + s])));
                    dI[rp] += w[s] * Q * a_boxP[s] * mass;
                    dI[rn] -= w[s] * Q * a_boxN[s] * mass;
                    if (a_ionAt > 0 && history->size() >= a_ionAt + 2 * N && a_media[std::size_t(a_nodeDomain[s])].incomplete) {
                        double nd = 0, na = 0, dnd = 0, dna = 0;
                        ionizedAt(i, x, &nd, &dnd, &na, &dna);
                        I += w[s] * Q * a_box[s] * ((mass * nd - (*history)[a_ionAt + s]) - (mass * na - (*history)[a_ionAt + N + s]));
                        dI[rn] += w[s] * Q * a_box[s] * mass * dnd;
                        dI[rp] -= w[s] * Q * a_box[s] * mass * dna;
                    }
                }
        }
        if (inTime) {
            // The metal's charge's change, mass Q(x) - Q(history).
            std::map<int, double> A = charge[k];
            double Qx = 0;
            for (int i : c.nodes) {
                const std::size_t s = std::size_t(i);
                double fv = F(a_iV[s]);
                if (a_nodeDomain[s] >= 0) {
                    double nd = 0, na = 0, dnd = 0, dna = 0;
                    ionizedAt(i, x, &nd, &dnd, &na, &dna);
                    fv -= Q * a_box[s] * (nd - na);
                    if (a_iN[s] >= 0) {
                        A[a_iN[s]] += Q * a_box[s] * dnd;
                        A[a_iP[s]] -= Q * a_box[s] * dna;
                    }
                    // (Their ions' own change, by their history.)
                    if (a_ionAt > 0 && history->size() >= a_ionAt + 2 * N && a_media[std::size_t(a_nodeDomain[s])].incomplete && a_iN[s] >= 0) {
                        I -= Q * a_box[s] * ((mass * nd - (*history)[a_ionAt + s]) - (mass * na - (*history)[a_ionAt + N + s]));
                        dI[a_iN[s]] -= Q * a_box[s] * mass * dnd;
                        dI[a_iP[s]] += Q * a_box[s] * mass * dna;
                    }
                }
                Qx -= fv;
            }
            double shift = 0;
            for (const auto& [col, a] : A) shift += a * (xh(col) - x(col));
            I += mass * Qx - (Qx + shift);
            gross += std::abs(mass * Qx) + std::abs(Qx) + std::abs(shift);
            for (const auto& [col, a] : A) dI[col] += mass * a;
        }
        CircuitCurrent& to = out[k];
        to.I += share * I;
        to.gross += share * gross;
        for (const auto& [col, d] : dI)
            if (d != 0 && col >= 0) to.dI[globalIndex(s, col)] += share * d;
    }
}

void SemiconductorProblem::circuitRows(const Eigen::VectorXd& X, double mass, const std::vector<double>* history, const std::vector<CircuitCurrent>& currents,
                                       const Eigen::VectorXd& colScale, Eigen::VectorXd& G, std::vector<Eigen::Triplet<double>>& kept)
{
    const bool inTime = mass > 0 && history;
    a_circuitResidual = 0;
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        const Contact& c = a_contacts[k];
        if (c.iC < 0 || k >= currents.size()) continue;
        const double I = currents[k].I, gross = currents[k].gross;
        const std::map<int, double>& dI = currents[k].dI;
        const Eigen::VectorXd& x = X;
        // Its row: V0 - Vc - R (I + Cp dVc/dt) [+ a bias tee's RL I0 in time].
        double Ic = 0, dIcdV = 0;
        if (inTime && c.Cp > 0 && a_circuitAt > 0 && history->size() >= a_circuitAt + 3 * a_contacts.size()) {
            Ic = c.Cp * (mass * x(c.iC) - (*history)[a_circuitAt + 3 * k]);
            dIcdV = c.Cp * mass;
        }
        const double R = inTime ? c.Rs + c.RL : std::isfinite(c.walkR) ? c.walkR : c.restR();
        double g = a_lastApplied[k] - x(c.iC) - R * (I + Ic);
        if (inTime && c.circuit == Contact::BiasTee) g += c.RL * c.teeCurrent;
        G(c.iC) = g;
        kept.emplace_back(c.iC, c.iC, (-1 - R * dIcdV) * colScale(c.iC));
        if (R > 0)
            for (const auto& [col, d] : dI)
                if (d != 0 && col >= 0) kept.emplace_back(c.iC, col, -R * d * colScale(col));
        a_circuitResidual = std::max(a_circuitResidual, std::abs(g) / std::max({a_Vt, std::abs(a_lastApplied[k]), R * std::abs(I + Ic), 1e-5 * R * gross}));
    }
}

void SemiconductorProblem::ionizationIntegrals(const Eigen::VectorXd& x)
{
    a_In = a_Ip = a_Emax = a_ionLength = a_ionX = a_ionY = NaN;
    a_ionLines = 0;
    const Space& sp = *field().space;
    const Mesh& mesh = a_s.mesh();
    const double unit = mesh.unitScale;
    auto ionizes = [&](int t) {
        if (t < 0 || !a_elemSemi[std::size_t(t)]) return false;
        return a_media[std::size_t(mesh.domain[std::size_t(t)])].ionization;
    };
    // Each triangle's field, -∇V; recovered at the nodes (their triangles'
    // by area, a domain's own), straight between them within a triangle: a
    // junction's node on a boundary, its doping the mean of fewer
    // triangles', makes one triangle's field a few percent high - the
    // integrals go as the field to the fifth, and a line from there gave
    // 37 % more.
    auto fieldIn = [&](int t) {
        const ElementMobility& em = a_elements[std::size_t(t)];
        const int* dof = sp.dofs(t);
        QPointF E(0, 0);
        for (int k = 0; k < 3; ++k) {
            const double v = x(a_iV[std::size_t(dof[k])]);
            E -= QPointF(em.gx[k] * v, em.gy[k] * v);
        }
        return E;
    };
    auto corner = [&](int t, int k) { return mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * unit; };
    // (Each domain's own at a node: across a junction one side's field is
    // not the other's - a p⁺ side's depletion, thinner than a triangle,
    // would pull the junction's node down.)
    std::map<std::pair<int, int>, std::pair<QPointF, double>> recovered;   // (node, domain): Σ area E, Σ area
    for (int t = 0; t < sp.elements(); ++t) {
        if (!ionizes(t)) continue;
        const QPointF a = corner(t, 0), b = corner(t, 1), c = corner(t, 2);
        const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
        const QPointF E = fieldIn(t);
        const int* dof = sp.dofs(t);
        for (int k = 0; k < 3; ++k) {
            auto& [sum, w] = recovered[{dof[k], mesh.domain[std::size_t(t)]}];
            sum += area * E;
            w += area;
        }
    }
    // (By triangle and corner, the recovered field there: looked up at each
    // of the lines' steps.)
    std::vector<QPointF> cornerField(3 * std::size_t(sp.elements()), QPointF(0, 0));
    for (int t = 0; t < sp.elements(); ++t) {
        if (!ionizes(t)) continue;
        for (int k = 0; k < 3; ++k) {
            const auto it = recovered.find({sp.dofs(t)[k], mesh.domain[std::size_t(t)]});
            if (it != recovered.end() && it->second.second > 0) cornerField[3 * std::size_t(t) + std::size_t(k)] = it->second.first / it->second.second;
        }
    }
    auto nodalIn = [&](int t, int k) { return cornerField[3 * std::size_t(t) + std::size_t(k)]; };
    auto barycentric = [&](int t, QPointF p, double l[3]) {
        const QPointF a = corner(t, 0), b = corner(t, 1), c = corner(t, 2);
        const double det = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
        l[1] = ((p.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (p.y() - a.y())) / det;
        l[2] = ((b.x() - a.x()) * (p.y() - a.y()) - (p.x() - a.x()) * (b.y() - a.y())) / det;
        l[0] = 1 - l[1] - l[2];
    };
    auto fieldAt = [&](int t, QPointF p) {
        double l[3];
        barycentric(t, p, l);
        QPointF E(0, 0);
        for (int k = 0; k < 3; ++k) E += std::clamp(l[k], 0.0, 1.0) * nodalIn(t, k);
        return E;
    };
    // The highest field where it ionizes; the lines' starts, the corners
    // whose α (the larger of the two) is a thousandth of the most or more,
    // the most first - the field's own were a weaker junction's of a
    // material that ionizes more, a third of the highest field: none.
    struct Seed {
        double alpha, E;
        int t, k;
    };
    std::vector<Seed> seeds;
    double Emax = 0, most = 0;
    for (int t = 0; t < sp.elements(); ++t) {
        if (!ionizes(t)) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        for (int k = 0; k < 3; ++k) {
            const QPointF E = nodalIn(t, k);
            const double e = std::hypot(E.x(), E.y());
            Emax = std::max(Emax, e);
            if (!(e > 0)) continue;
            double an = 0, ap = 0, d = 0;
            const double g = a_elements[std::size_t(t)].ii;
            ionizationRate(e, g * m.an, g * m.bn, g * m.an2, g * m.bn2, m.E0, m.ionBetaN, &an, &d);
            ionizationRate(e, g * m.ap, g * m.bp, g * m.ap2, g * m.bp2, m.E0, m.ionBetaP, &ap, &d);
            const double alpha = std::max(an, ap);
            most = std::max(most, alpha);
            seeds.push_back({alpha, e, t, k});
        }
    }
    if (seeds.empty() || !(Emax > 0)) return;
    a_Emax = Emax;
    seeds.erase(std::remove_if(seeds.begin(), seeds.end(), [most](const Seed& sd) { return !(sd.alpha >= 1e-3 * most) || !(sd.alpha > 0); }), seeds.end());
    if (seeds.empty()) {
        a_In = a_Ip = 0;
        return;
    }
    std::sort(seeds.begin(), seeds.end(), [](const Seed& a, const Seed& b) { return a.alpha > b.alpha; });
    // The triangle holding p, walked to from t (across the edge p is beyond).
    auto locate = [&](int t, QPointF p) {
        for (int walk = 0; walk < 2000 && t >= 0; ++walk) {
            double l[3];
            barycentric(t, p, l);
            int worst = 0;
            for (int k = 1; k < 3; ++k)
                if (l[k] < l[worst]) worst = k;
            if (l[worst] >= -1e-12) return t;
            // (Beyond the edge opposite its corner 'worst': edge worst+1 to worst+2.)
            t = a_neighbors[std::size_t(t)][std::size_t((worst + 1) % 3)];
        }
        return -1;
    };
    // Along the field (sign 1) or against it, from a point of triangle t:
    // each step a fifth of its triangle's size, the field where it is,
    // until it leaves what ionizes, the field turns back or falls to a
    // thousandth of the highest. The triangles it passes, covered.
    struct Sample {
        double s, E;
        int t;
        QPointF p;
    };
    std::vector<char> covered(std::size_t(sp.elements()), 0);
    // (Explicit captures: GCC 12 and 13 crash on long [&] lambdas, d7ae967.)
    auto follow = [&fieldAt, &corner, &locate, &ionizes, &covered, Emax](int start, QPointF from, double sign) {
        std::vector<Sample> out;
        int t = start;
        QPointF p = from;
        double s = 0;
        for (int step = 0; step < 200000; ++step) {
            const QPointF E = fieldAt(t, p);
            const double e = std::hypot(E.x(), E.y());
            if (!(e > 1e-3 * Emax)) break;
            const QPointF a = corner(t, 0), b = corner(t, 1), c = corner(t, 2);
            const double size = std::sqrt(std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2);
            const QPointF d = sign * E / e;
            const QPointF q = p + d * (0.2 * size);
            const int next = locate(t, q);
            if (!ionizes(next)) break;
            const QPointF En = fieldAt(next, q);
            if ((En.x() * d.x() + En.y() * d.y()) * sign <= 0) break;   // (the field turned)
            s += 0.2 * size;
            out.push_back({s, std::hypot(En.x(), En.y()), next, q});
            covered[std::size_t(next)] = 1;
            p = q;
            t = next;
        }
        return out;
    };
    // Each line's integrals, ordered along the field (x: where holes go):
    // holes along x from 0, Ip = ∫ αp e^(-∫_0^x (αp - αn)) dx; electrons
    // against it from W, In = ∫ αn e^(-∫_x^W (αn - αp)) dx.
    auto integrals = [this, &mesh](const std::vector<Sample>& line, double* In, double* Ip) {
        const std::size_t n = line.size();
        std::vector<double> xs(n), an(n), ap(n);
        for (std::size_t k = 0; k < n; ++k) {
            xs[k] = line[k].s - line.front().s;
            const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(line[k].t)])];
            double d = 0;
            const double g = a_elements[std::size_t(line[k].t)].ii;
            ionizationRate(line[k].E, g * m.an, g * m.bn, g * m.an2, g * m.bn2, m.E0, m.ionBetaN, &an[k], &d);
            ionizationRate(line[k].E, g * m.ap, g * m.bp, g * m.ap2, g * m.bp2, m.E0, m.ionBetaP, &ap[k], &d);
        }
        std::vector<double> K(n, 0.0);   // ∫_0^x (αp - αn)
        for (std::size_t k = 1; k < n; ++k) K[k] = K[k - 1] + 0.5 * ((ap[k] - an[k]) + (ap[k - 1] - an[k - 1])) * (xs[k] - xs[k - 1]);
        *In = *Ip = 0;
        for (std::size_t k = 1; k < n; ++k) {
            const double dx = xs[k] - xs[k - 1];
            *Ip += 0.5 * (ap[k] * std::exp(-K[k]) + ap[k - 1] * std::exp(-K[k - 1])) * dx;
            // ∫_x^W (αn - αp) = -(K(W) - K(x)).
            *In += 0.5 * (an[k] * std::exp(K[n - 1] - K[k]) + an[k - 1] * std::exp(K[n - 1] - K[k - 1])) * dx;
        }
        return xs.back();
    };
    // A line from each start not yet covered - the most ionizing first:
    // through a corner's high field another path may break down first (a
    // multiplication layer's under a ridge: the Lumerical APD's review) -
    // the most of each integral kept, 2000 lines at most.
    a_In = a_Ip = 0;
    for (const Seed& sd : seeds) {
        if (covered[std::size_t(sd.t)] && a_ionLines > 0) continue;
        if (a_ionLines >= 2000) break;
        const QPointF from = 0.98 * corner(sd.t, sd.k) + 0.01 * (corner(sd.t, (sd.k + 1) % 3) + corner(sd.t, (sd.k + 2) % 3));
        covered[std::size_t(sd.t)] = 1;
        const std::vector<Sample> against = follow(sd.t, from, -1), along = follow(sd.t, from, 1);
        std::vector<Sample> line;
        for (auto it = against.rbegin(); it != against.rend(); ++it) line.push_back({-it->s, it->E, it->t, it->p});
        line.push_back({0, sd.E, sd.t, from});
        for (const Sample& sm : along) line.push_back(sm);
        ++a_ionLines;
        if (line.size() < 2) continue;
        double In = 0, Ip = 0;
        const double length = integrals(line, &In, &Ip);
        if (In > a_In || !std::isfinite(a_ionLength)) {
            a_In = In;
            a_ionLength = length;
            // (Where: its highest field's point.)
            const Sample* top = &line.front();
            for (const Sample& sm : line)
                if (sm.E > top->E) top = &sm;
            a_ionX = top->p.x();
            a_ionY = top->p.y();
        }
        a_Ip = std::max(a_Ip, Ip);
    }
}

bool SemiconductorProblem::stationary(QString* why)
{
    Field& f = field();
    // (Hot carriers: walked with the field's mobilities, their temperatures
    // anew at its end.)
    struct Hot {
        bool& active;
        ~Hot() { active = true; }
    } hot{a_energyActive};
    if (a_energy) {
        a_energyActive = false;
        for (auto* v : {&a_Tn, &a_Tp, &a_TnWas, &a_TpWas, &a_dTnWas, &a_dTpWas}) v->clear();
        a_omegaT[0] = a_omegaT[1] = 1;
        a_residualT[0] = a_residualT[1] = 0;
    }
    std::vector<double> goal;
    if (!targets(goal, why)) return false;
    Eigen::VectorXd x = stateVector();
    const std::size_t nc = a_contacts.size();
    // (Whatever way it ends: all the light, the circuits' own resistances.)
    struct Ended {
        SemiconductorProblem* p;
        ~Ended()
        {
            p->a_lightScale = 1;
            for (Contact& c : p->a_contacts) c.walkR = NaN;
        }
    } ended{this};
    auto finish = [this, &goal, &f](const Eigen::VectorXd& solved, bool still) {
        a_lightScale = 1;
        for (Contact& c : a_contacts) c.walkR = NaN;
        for (std::size_t k = 0; k < a_contacts.size(); ++k) a_contacts[k].applied = goal[k];
        // (Kept with it: the light it was solved with, each circuit's resistance.)
        if (a_lightAt > 0 && f.values.size() > a_lightAt) f.values[a_lightAt] = a_Gop;
        for (Contact& c : a_contacts) c.solvedR = c.restR();
        terminalAll(solved, 0, nullptr, still);
        // (At rest no current in Cp; a bias tee's current at rest is its
        // series current - its load's in time is the change from it.)
        for (Contact& c : a_contacts) {
            c.capCurrent = 0;
            if (c.iC >= 0) c.teeCurrent = c.current;
        }
        store(solved);
    };
    double fermi = 0;
    if (atEquilibrium(goal, &fermi)) {
        // No current: Poisson's equation alone (a MOS capacitor, a junction
        // unbiased) - exact, the currents' equations hold at once; a laser
        // dark.
        if (!equilibriumPoisson(x, fermi, goal, f.solved, why)) return false;
        if (a_iS >= 0) x(a_iS) = 0;
        circuitsAtRest(x, goal);
        broadcastSlices(x);
        finish(x, true);
        quasiStatic(x, fermi, goal);
        return true;
    }
    a_C0.clear();
    // On the way, beside the voltages, λ from 0 to 1: the light from what
    // the solution was solved with to all of it, each circuit's resistance
    // at rest from its as solved to its own (the light switched on with no
    // voltage to walk, or a resistance swept: from the last solution Newton
    // may find none at once - tried first all at once, as it was).
    double lightFrom = a_Gop > 0 && f.solved && a_lightAt > 0 && f.values.size() > a_lightAt ? f.values[a_lightAt] / a_Gop : a_Gop > 0 ? 0.0 : 1.0;
    std::vector<double> fromR(nc, 0.0);
    for (std::size_t k = 0; k < nc; ++k) fromR[k] = f.solved ? a_contacts[k].solvedR : a_contacts[k].restR();
    bool homotopy = std::abs(lightFrom - 1) > 1e-12;
    for (std::size_t k = 0; k < nc; ++k)
        if (a_contacts[k].iC >= 0 && std::abs(fromR[k] - a_contacts[k].restR()) > 1e-12 * std::max(1.0, a_contacts[k].restR())) homotopy = true;
    auto along = [&](double lambda) {
        a_lightScale = lightFrom + lambda * (1 - lightFrom);
        for (std::size_t k = 0; k < nc; ++k)
            if (a_contacts[k].iC >= 0) a_contacts[k].walkR = fromR[k] + lambda * (a_contacts[k].restR() - fromR[k]);
    };
    goal.push_back(1.0);
    std::vector<double> at(nc + 1, 0.0);
    at[nc] = homotopy ? 0.0 : 1.0;
    // A laser ready on a state solved without it - its photons none, its
    // carriers perhaps past their clamp: initialize()'s walk to the voltages
    // went before its mode was read - walked again from equilibrium, with
    // it. (From there no smaller step was left: a stationary step at 1.6 V,
    // above threshold, failed - the update check of 110e8b9, 4.)
    const bool laserFresh = a_laser && a_laser->ready && a_iS >= 0 && !(x(a_iS) > 0);
    const bool warm = f.solved && !laserFresh;
    if (!warm) {
        // (From rest: the light from none, the resistances carrying nothing.)
        const std::vector<double> rest(nc, 0.0);
        if (!equilibriumPoisson(x, 0, rest, false, why)) return false;
        for (Contact& c : a_contacts) c.applied = 0;
        circuitsAtRest(x, rest);
        broadcastSlices(x);
        lightFrom = a_Gop > 0 ? 0.0 : 1.0;
        for (std::size_t k = 0; k < nc; ++k) fromR[k] = a_contacts[k].restR();
        at[nc] = std::abs(lightFrom - 1) > 1e-12 ? 0.0 : 1.0;
    } else {
        for (std::size_t k = 0; k < a_contacts.size(); ++k) at[k] = a_contacts[k].applied;
    }
    // From the voltages it is at to those asked, in steps that Newton's
    // method takes. Each step starts on the line of the last two solutions
    // - the potential on it, the carriers on their logarithms' (a junction's
    // densities go as e^(V/Vt)) - the step longer after an easy solve,
    // shorter after a hard one, halved where it fails (its start nearer the
    // solution with it: from the solution itself again, as it was, never
    // took where the line's had not). The Lumerical APD's review: its zeroth-
    // order steps, doubled only after fewer than 8 of Newton's and cut to a
    // quarter on a failure, took 80 steps to -0.5 V and 1270 (2519 s) from
    // -9.25 to -9.5 V at its breakdown.
    // (A sweep's point: from the last point's, its last step's line and
    // length - near a breakdown 0.3 mV, from which 0.1 V took eight
    // failures.)
    Eigen::VectorXd before;
    std::vector<double> beforeAt;
    double step = 0.1;
    if (warm && f.walkedFrom && int(f.walkedFrom->size()) == a_total && f.walkedFromVolts.size() == at.size()) {
        before = Eigen::Map<const Eigen::VectorXd>(f.walkedFrom->data(), a_total);
        beforeAt = f.walkedFromVolts;
        beforeAt[nc] = at[nc];   // (the last walk's λ another's: its line the voltages' alone)
        if (f.walkedStep > 0) step = std::min(4 * f.walkedStep, 1.0);
    }
    // The way's length in volts (λ's as one): the contact that moves most.
    std::size_t moving = 0;
    bool voltsMove = false;
    for (std::size_t k = 0; k < nc; ++k) {
        if (std::abs(goal[k] - at[k]) > std::abs(goal[moving] - at[moving])) moving = k;
        voltsMove = voltsMove || std::abs(goal[k] - at[k]) > 1e-12;
    }
    // (The light or a resistance alone changed: tried all at once first.)
    if (!voltsMove && at[nc] < 1) step = 1.0;
    auto apart = [](const std::vector<double>& a, const std::vector<double>& b) {
        double d = 0;
        for (std::size_t k = 0; k < a.size(); ++k) d = std::max(d, std::abs(a[k] - b[k]));
        return d;
    };
    // The start of a step to \a next: on the line from the solution before
    // through x, as far beyond x as \a next is (in the same direction:
    // else none), the carriers' factor at most 10^3.
    // (Explicit captures: GCC 12 and 13 crash on long [&] lambdas, d7ae967.)
    auto predicted = [this, &before, &beforeAt, &x, &at, &apart](const std::vector<double>& next, Eigen::VectorXd& trial) {
        if (before.size() != x.size() || beforeAt.size() != at.size()) return false;
        const double last = apart(at, beforeAt), ahead = apart(next, at);
        if (!(last > 1e-12) || !(ahead > 0)) return false;
        double dot = 0, la = 0, lb = 0;
        for (std::size_t k = 0; k < at.size(); ++k) {
            dot += (at[k] - beforeAt[k]) * (next[k] - at[k]);
            la += (at[k] - beforeAt[k]) * (at[k] - beforeAt[k]);
            lb += (next[k] - at[k]) * (next[k] - at[k]);
        }
        if (!(dot > 0.999 * std::sqrt(la * lb))) return false;
        const double r = std::min(ahead / last, 4.0);
        trial = x;
        for (int s = 0; s < a_slices; ++s)
            for (int i = 0; i < a_n; ++i) {
                if (a_iV[std::size_t(i)] >= 0) {
                    const int k = globalIndex(s, a_iV[std::size_t(i)]);
                    trial(k) = x(k) + r * (x(k) - before(k));
                }
                for (const int kk : {a_iN[std::size_t(i)], a_iP[std::size_t(i)]}) {
                    if (kk < 0) continue;
                    const int k = globalIndex(s, kk);
                    if (x(k) > 0 && before(k) > 0) trial(k) = x(k) * std::clamp(std::pow(x(k) / before(k), r), 1e-3, 1e3);
                }
            }
        if (a_iS >= 0 && x(a_iS) > 0 && before(a_iS) > 0) trial(a_iS) = x(a_iS) * std::clamp(std::pow(x(a_iS) / before(a_iS), r), 1e-3, 1e3);
        // (A circuit's contact's own voltage on the line, as the potential.)
        for (const Contact& c : a_contacts)
            if (c.iC >= 0) trial(c.iC) = x(c.iC) + r * (x(c.iC) - before(c.iC));
        return true;
    };
    int solves = 0, iterations = 0, retried = 0;
    QString failed;
    QString contactName = a_contacts.empty() ? QString() : a_contacts[moving].name;
    // (From the last solution, then - where none is found - from rest, as a
    // solve alone: a sweep of a forward-biased diode's temperature stopped
    // at 400 K, each point alone solved - its voltages there already, no
    // step to halve but nought: the hunt of 19 October, B8.)
    bool warmTry = warm, fromRest = false;
    bool moved = false;   // a step of its own taken (else the line it was handed kept)
    while (true) {
        double left = 0;
        for (std::size_t k = 0; k < goal.size(); ++k) left = std::max(left, std::abs(goal[k] - at[k]));
        if (left < 1e-12 && solves > 0) break;
        const double frac = left > 0 ? std::min(1.0, step / left) : 1.0;
        std::vector<double> next(at.size());
        for (std::size_t k = 0; k < at.size(); ++k) next[k] = at[k] + frac * (goal[k] - at[k]);
        contactValues(std::vector<double>(next.begin(), next.begin() + std::ptrdiff_t(nc)));
        along(next[nc]);
        Eigen::VectorXd trial = x;
        (void)predicted(next, trial);
        // (Told: where on its way, the step, Newton's residual - the APD's
        // review: "Solving Stationary" alone for 42 minutes.)
        if (!contactName.isEmpty()) {
            a_walkSaid = tr("%1 at %2 of %3 (step %4, %5)")
                             .arg(contactName, formatQuantity(next[moving], Dim::voltage()), formatQuantity(goal[moving], Dim::voltage()))
                             .arg(solves + 1)
                             .arg(formatQuantity(frac * left, Dim::voltage(), 3));
            if (next[nc] < 1) a_walkSaid += tr(", the light and circuits %1 % of their way").arg(formatNumber(100 * next[nc], 3));
        }
        int steps = 0;
        // (At its voltages already - a walk's end solved again, the light's
        // pairs added since or not -: no step where its equations are met.)
        const bool solved = newton(trial, 0, nullptr, 40, &steps, &failed, warmTry && frac * left < 1e-12);
        iterations += steps;
        if (solved) {
            before = x;
            beforeAt = at;
            x = trial;
            at = next;
            ++solves;
            moved = moved || frac * left > 1e-12;
            step = std::min(step * (steps <= 5 ? 2.0 : steps <= 10 ? 1.4 : steps <= 20 ? 1.0 : 0.7), 1.0);
        } else if (cancelled()) {
            a_walkSaid.clear();
            *why = tr("cancelled");
            return false;
        } else {
            ++retried;
            // (Half the step tried: a step capped to what was left, 0.25 V
            // of a 1 V one, halved to 0.5 V was tried again as it was.)
            step = std::min(step, frac * left) / 2;
            if (step < 1e-5 && warmTry) {
                warmTry = false;
                fromRest = true;
                std::fill(at.begin(), at.end(), 0.0);
                const std::vector<double> rest(nc, 0.0);
                if (!equilibriumPoisson(x, 0, rest, false, why)) return false;
                for (Contact& c : a_contacts) c.applied = 0;
                circuitsAtRest(x, rest);
                broadcastSlices(x);
                // (From rest: the light from none, the resistances carrying nothing.)
                lightFrom = a_Gop > 0 ? 0.0 : 1.0;
                for (std::size_t k = 0; k < nc; ++k) fromR[k] = a_contacts[k].restR();
                at[nc] = std::abs(lightFrom - 1) > 1e-12 ? 0.0 : 1.0;
                before.resize(0);
                beforeAt.clear();
                step = 0.1;
                solves = 0;
                moving = 0;
                for (std::size_t k = 0; k < nc; ++k)
                    if (std::abs(goal[k]) > std::abs(goal[moving])) moving = k;
                contactName = a_contacts.empty() ? QString() : a_contacts[moving].name;
                continue;
            }
            if (step < 1e-5) {
                a_walkSaid.clear();
                // (Not "did not converge", the cause's words too: a step at
                // rest that fails is an error - said as a warning, its study
                // went on with nothing solved, its currents nought.)
                QString cause = failed.section(QLatin1String(": "), 1);
                cause.replace(QLatin1String("did not converge"), QLatin1String("failed to converge"));
                *why = fromRest ? tr("%1: no solution found on the way to the contacts' voltages (steps down to 10 µV), from the last solution nor from rest: %2").arg(a_node->name(), cause)
                                : tr("%1: no solution found on the way to the contacts' voltages (steps down to 10 µV): %2").arg(a_node->name(), cause);
                return false;
            }
        }
    }
    a_walkSaid.clear();
    // (At its end its mobilities its solution's own: held from fifteen of
    // Newton's steps on, before the potential settled, the solution was
    // theirs - solved again with them updated at it while they move. Held
    // settled, they are its own already. Hot carriers always: their
    // temperatures, halfway each update, lag a walk's steps.)
    if ((a_varying && a_heldUnsettled && solves > 0) || a_energy) {
        a_energyActive = true;
        // (Not polished: the mobilities as the solution had them again.)
        std::vector<std::array<double, 4>> edges(a_edges.size());
        for (std::size_t k = 0; k < a_edges.size(); ++k) edges[k] = {a_edges[k].wn, a_edges[k].wp, a_edges[k].muN, a_edges[k].muP};
        const std::vector<std::vector<std::array<double, 4>>> sliceEdges = a_sliceEdges;
        Eigen::VectorXd polished = x;
        int steps = 0;
        QString failedPolish;
        // (Each update met again in a few steps: their room for eight.)
        if (newton(polished, 0, nullptr, a_energy ? 1000 : 120, &steps, &failedPolish, false, true)) {
            x = polished;
            iterations += steps;
        } else {
            for (std::size_t k = 0; k < a_edges.size(); ++k) {
                a_edges[k].wn = edges[k][0];
                a_edges[k].wp = edges[k][1];
                a_edges[k].muN = edges[k][2];
                a_edges[k].muP = edges[k][3];
            }
            a_sliceEdges = sliceEdges;
            a_s.log << tr("%1: its mobilities, held as it converged, not made its solution's own (%2)").arg(a_node->name(), failedPolish);
        }
    }
    // (Its last step kept: the next sweep point's first starts on its line.
    // A walk that took none - at its voltages already, solved again - leaves
    // the line it was handed: as a step of nought it lost the first sweep
    // point's, the second starting as from nothing - the hunt of 19 October,
    // B4.)
    if (moved && before.size() == x.size() && beforeAt.size() == at.size()) {
        f.walkedFrom = std::make_shared<const std::vector<double>>(before.data(), before.data() + before.size());
        f.walkedFromVolts = beforeAt;
        f.walkedStep = apart(at, beforeAt);
    } else if (moved) {
        f.walkedFrom.reset();
        f.walkedFromVolts.clear();
        f.walkedStep = 0;
    }
    finish(x, false);
    heterojunctionMesh(x);
    if (fromRest) a_s.log << tr("%1: from the last solution no solution found: solved from rest").arg(a_node->name());
    if (solves > 1 || retried > 0)
        a_s.log << (retried > 0 ? tr("%1: the contacts' voltages reached in %2 steps (%3 of Newton's; %4 tried again)").arg(a_node->name()).arg(solves).arg(iterations).arg(retried)
                                : tr("%1: the contacts' voltages reached in %2 steps (%3 of Newton's)").arg(a_node->name()).arg(solves).arg(iterations));
    return true;
}

void SemiconductorProblem::heterojunctionMesh(const Eigen::VectorXd& x)
{
    // Abrupt, a heterojunction's depletion and accumulation beside it vary
    // over a Debye length: elements wider than that there, its current is
    // the mesh's. A toy laser at 1.7 V with 4 rows across each layer passed
    // a third of its current, 10.3 µA for 36.6 at 256 rows, nothing said
    // (the update check of a690d27). (Graded over an element - one bands'
    // side throughout -, or solved in 1D on a line of its own: none here.)
    if (a_1d) return;
    // Its nodes: where edges of two bands' sides meet; along it, the
    // direction of its edges (two sides' each), a corner where two meet.
    std::vector<int> side(std::size_t(a_n), -1);
    std::vector<char> hetero(std::size_t(a_n), 0), corner(std::size_t(a_n), 0);
    std::vector<std::array<double, 2>> along(std::size_t(a_n), {0.0, 0.0});
    for (const Edge& e : a_edges) {
        for (const int v : {e.i, e.j}) {
            int& s = side[std::size_t(v)];
            if (s < 0) s = e.medium;
            else if (s != e.medium) hetero[std::size_t(v)] = 1;
            if (e.partner < 0) continue;
            std::array<double, 2>& t = along[std::size_t(v)];
            if (t[0] == 0 && t[1] == 0) t = {e.tx, e.ty};
            else if (std::abs(t[0] * e.tx + t[1] * e.ty) < 0.9) corner[std::size_t(v)] = 1;
        }
    }
    // The edges from one into its side, those that carry the contacts'
    // current across - their share of it, Σ of which is the current (the
    // edge's current times its contact weight's change; terminalValues()),
    // a tenth of the most or more: a side wall the carriers run along within
    // a well, its weight the same both ends, is not one - and their lengths
    // against the Debye length at their far nodes, of their carriers or
    // doping, whichever is more.
    if (std::find(hetero.cbegin(), hetero.cend(), 1) == hetero.cend()) return;
    std::size_t biggest = 0;
    for (std::size_t k = 1; k < a_contacts.size(); ++k)
        if (std::abs(a_contacts[k].current) > std::abs(a_contacts[biggest].current)) biggest = k;
    if (a_contacts.empty()) return;
    const std::vector<std::vector<double>> weights = currentWeights(x);
    if (biggest >= weights.size() || weights[biggest].empty()) return;
    const std::vector<double>& weight = weights[biggest];
    struct Crossing {
        double current, ratio, length, debye;
        int node;
    };
    std::vector<Crossing> crossings;
    double most = 0;
    for (const Edge& e : a_edges) {
        if (e.w <= 0 || !(e.h > 0) || hetero[std::size_t(e.i)] == hetero[std::size_t(e.j)]) continue;
        const int o = hetero[std::size_t(e.i)] ? e.j : e.i, at = hetero[std::size_t(e.i)] ? e.i : e.j;
        // (Across it - more than 60° off it -, not at a corner, where an
        // edge along one side runs across the other's end.)
        const std::array<double, 2>& t = along[std::size_t(at)];
        if (corner[std::size_t(at)] || (t[0] == 0 && t[1] == 0) || std::abs(t[0] * e.tx + t[1] * e.ty) >= 0.5) continue;
        const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
        if (a_iN[i] < 0 || a_iN[j] < 0 || a_nodeDomain[std::size_t(o)] < 0) continue;
        double dN = 0, dP = 0;
        edgeSteps(e, x, &dN, &dP);
        const double fn = e.wn * (e.fNj * x(a_iN[j]) * bern(dN) - e.fNi * x(a_iN[i]) * bern(-dN));
        const double fp = e.wp * (e.fPi * x(a_iP[i]) * bern(dP) - e.fPj * x(a_iP[j]) * bern(-dP));
        const double current = std::abs((fn + fp) * (weight[i] - weight[j]));
        if (!std::isfinite(current)) continue;
        const double density = std::max(std::abs(doping(o)), x(a_iN[std::size_t(o)]) + x(a_iP[std::size_t(o)]));
        if (!(density > 0)) continue;
        const double debye = std::sqrt(Eps0 * a_media[std::size_t(a_nodeDomain[std::size_t(o)])].epsr * a_Vt / (Q * density));
        crossings.push_back({current, e.h / debye, e.h, debye, o});
        most = std::max(most, current);
    }
    if (!(most > 0)) return;
    const Crossing* worst = nullptr;
    for (const Crossing& c : crossings)
        if (c.current >= 0.1 * most && (worst == nullptr || c.ratio > worst->ratio)) worst = &c;
    if (worst == nullptr || worst->ratio <= 1 || worst->ratio <= a_meshSaidRatio) return;
    const Topology& topo = a_s.topology();
    const int d = a_nodeDomain[std::size_t(worst->node)];
    const QString where = d < int(topo.domains.size()) ? topo.objectNames.value(topo.domains[std::size_t(d)].owner) : QString();
    const QString line = tr("Warning: %1: elements %2 nm across where its current crosses a heterojunction%3, its Debye length there "
                            "%4 nm (its carriers' or doping's): the current depends on the mesh - refine it there to under a Debye "
                            "length (a Layered Region's rows, a smaller size near the interface) until the current no longer changes")
                             .arg(a_node->name(), formatNumber(worst->length * 1e9, 3),
                                  where.isEmpty() ? QString() : tr(" (in “%1”)").arg(where), formatNumber(worst->debye * 1e9, 2));
    // (One line a study: its worst point's.)
    const qsizetype at = a_meshSaid.isEmpty() ? -1 : a_s.log.indexOf(a_meshSaid);
    if (at >= 0) a_s.log[at] = line;
    else a_s.log << line;
    a_meshSaid = line;
    a_meshSaidRatio = worst->ratio;
}

bool SemiconductorProblem::smallSignal(double frequency, QString* why)
{
    // Each slice's at its own state and pairs (the contacts held: each
    // alone), weighed by its share of the length; then the circuits.
    using cd = std::complex<double>;
    const Eigen::VectorXd X = stateVector();
    const std::size_t nc = a_contacts.size();
    std::vector<std::vector<cd>> Y(nc, std::vector<cd>(nc, cd(0, 0)));
    std::vector<cd> H;
    // (Noise: each slice's sources its own length's - their powers by the
    // slices' shares.)
    a_noiseC.clear();
    a_noiseL.clear();
    if (a_noise) a_noiseC.assign(nc, std::vector<cd>(nc, cd(0, 0)));
    for (int sl = 0; sl < a_slices; ++sl) {
        useSlice(sl);
        std::vector<std::vector<cd>> Ys, Ns;
        std::vector<cd> Hs;
        if (!smallSignalSlice(a_slices > 1 ? sliceOf(X, sl) : X, frequency, Ys, Hs, why, a_noise ? &Ns : nullptr)) {
            useSlice(0);
            return false;
        }
        const double w = sliceWeight(sl);
        for (std::size_t a = 0; a < nc; ++a)
            for (std::size_t b = 0; b < nc; ++b) Y[a][b] += w * Ys[a][b];
        if (a_noise && Ns.size() == nc)
            for (std::size_t a = 0; a < nc; ++a)
                for (std::size_t b = 0; b < nc; ++b) a_noiseC[a][b] += w * Ns[a][b];
        if (!Hs.empty()) {
            if (H.empty()) H.assign(nc, cd(0, 0));
            for (std::size_t c = 0; c < nc; ++c) H[c] += w * Hs[c];
        }
    }
    useSlice(0);
    a_Y = Y;
    a_Hop = H;
    const double omega = 2 * M_PI * frequency;
    // Behind circuits: each one's contact's voltage follows - its series
    // impedance Z (Rs and RL; a bias tee's signal into RL too), Cp across -
    // from the contacts' admittance and the light's currents: (Y + jωCp +
    // 1/Z) v = -h on those contacts, the others held; the current through Z
    // into the device, -v/Z: the light's through the load. (Z nought: held,
    // all of it.)
    a_Hc.clear();
    // (The noise through the loads also with no light: it was made under the
    // light's modulation alone - a dark device behind a load, thermal or a
    // dark current's shot, had none: the hunt of 22 October, A9.)
    if (!a_Hop.empty() || !a_noiseC.empty()) {
        std::vector<std::size_t> ckt;
        for (std::size_t k = 0; k < nc; ++k)
            if (a_contacts[k].iC >= 0 && a_contacts[k].Rs + a_contacts[k].RL > 0) ckt.push_back(k);
        bool any = false;
        for (const Contact& c : a_contacts) any = any || c.iC >= 0;
        if (any && !a_Hop.empty()) a_Hc = a_Hop;
        if (!ckt.empty()) {
            const Eigen::Index m = Eigen::Index(ckt.size());
            Eigen::MatrixXcd M(m, m);
            for (Eigen::Index a = 0; a < m; ++a) {
                const Contact& c = a_contacts[ckt[std::size_t(a)]];
                for (Eigen::Index e = 0; e < m; ++e) M(a, e) = a_Y[ckt[std::size_t(a)]][ckt[std::size_t(e)]];
                M(a, a) += cd(0, omega * c.Cp) + 1.0 / (c.Rs + c.RL);
            }
            if (!a_Hop.empty()) {
                Eigen::VectorXcd b(m);
                for (Eigen::Index a = 0; a < m; ++a) b(a) = -a_Hop[ckt[std::size_t(a)]];
                const Eigen::VectorXcd v = M.fullPivLu().solve(b);
                for (Eigen::Index a = 0; a < m; ++a) {
                    const Contact& c = a_contacts[ckt[std::size_t(a)]];
                    a_Hc[ckt[std::size_t(a)]] = -v(a) / (c.Rs + c.RL);
                }
            }
            // (Noise through the loads: a short-circuit noise current i at
            // the contacts the same way, I_L = (1/Z) M⁻¹ i - its powers D
            // M⁻¹ C M⁻ᴴ D; the circuit's own resistors' not in it.)
            if (!a_noiseC.empty()) {
                Eigen::MatrixXcd C(m, m);
                for (Eigen::Index a = 0; a < m; ++a)
                    for (Eigen::Index e = 0; e < m; ++e) C(a, e) = a_noiseC[ckt[std::size_t(a)]][ckt[std::size_t(e)]];
                const Eigen::MatrixXcd Mi = M.fullPivLu().inverse();
                const Eigen::MatrixXcd S = Mi * C * Mi.adjoint();
                a_noiseL.assign(nc, NaN);
                for (Eigen::Index a = 0; a < m; ++a) {
                    const Contact& c = a_contacts[ckt[std::size_t(a)]];
                    a_noiseL[ckt[std::size_t(a)]] = S(a, a).real() / ((c.Rs + c.RL) * (c.Rs + c.RL));
                }
            }
        }
    }
    a_frequency = frequency;
    a_inTime = false;
    return true;
}

bool SemiconductorProblem::smallSignalSlice(const Eigen::VectorXd& x, double frequency, std::vector<std::vector<std::complex<double>>>& Y,
                                            std::vector<std::complex<double>>& H, QString* why,
                                            std::vector<std::vector<std::complex<double>>>* noise)
{
    // (A magnetic field's turned currents: at rest and in time alone.)
    if (a_Bz != 0) {
        *why = tr("%1: a small signal in a magnetic field is not solved (at rest and in time it is)").arg(a_node->name());
        return false;
    }
    std::vector<double> at(a_contacts.size());
    for (std::size_t k = 0; k < a_contacts.size(); ++k) at[k] = a_contacts[k].applied;
    contactValues(at);
    updateMobilities(x);
    Eigen::VectorXd F;
    std::vector<Eigen::Triplet<double>> trip;
    assemble(x, 0, nullptr, F, &trip);
    // Each node's stores of its carriers (m³ of n, of p): its box's, the
    // dopants' ions with them - incomplete ionization's n - Nd⁺, p - Na⁻
    // (the carriers' alone, as it was: a junction's C_aa 1.52 pF, C_kk 2.09,
    // its charge not kept: the update check of 48d5fa8, 7).
    std::vector<std::pair<double, double>> kept(static_cast<std::size_t>(a_n));
    for (int i = 0; i < a_n; ++i) {
        double kn = a_boxN[std::size_t(i)], kp = a_boxP[std::size_t(i)];
        const int d = a_nodeDomain[std::size_t(i)];
        if (d >= 0 && a_media[std::size_t(d)].incomplete) {
            double nd = 0, na = 0, dnd = 0, dna = 0;
            ionizedAt(i, x, &nd, &dnd, &na, &dna);
            kn -= a_box[std::size_t(i)] * dnd;
            kp -= a_box[std::size_t(i)] * dna;
        }
        kept[std::size_t(i)] = {kn, kp};
    }
    auto stores = [&kept](int i) { return kept[std::size_t(i)]; };
    // The mobilities' own change (a saturation's with the force, a surface's
    // with the field across): Newton holds them a step, and the solution is
    // theirs at its end - but the small signal is the Jacobian itself, and
    // held they are not in it (a saturated bar's conductance 197 times its
    // DC curve's slope; a MOSFET's output conductance 3: the update check of
    // 110e8b9). Each triangle's mobilities by its nine unknowns (V, n, p at
    // its corners; central differences), times each of its edges' current
    // per unit of the edge's weight.
    const std::vector<MobilityChange> moving = mobilityChanges(x);
    std::vector<double> perWn(a_edges.size(), 0.0), perWp(a_edges.size(), 0.0);
    if (!moving.empty())
        for (std::size_t m = 0; m < a_edges.size(); ++m) {
            const Edge& e = a_edges[m];
            if (e.w <= 0) continue;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            if (e.wn > 0) perWn[m] = Q * a_Vt * (e.fNj * x(a_iN[std::size_t(e.j)]) * bern(dN) - e.fNi * x(a_iN[std::size_t(e.i)]) * bern(-dN));
            if (e.wp > 0) perWp[m] = Q * a_Vt * (e.fPi * x(a_iP[std::size_t(e.i)]) * bern(dP) - e.fPj * x(a_iP[std::size_t(e.j)]) * bern(-dP));
        }
    for (const MobilityChange& mc : moving) {
        const ElementMobility& em = a_elements[std::size_t(mc.t)];
        for (int k = 0; k < 3; ++k) {
            const std::size_t m = std::size_t(em.edge[k]);
            const Edge& e = a_edges[m];
            for (int c = 0; c < 9; ++c) {
                const double fn = perWn[m] * em.cot[k] * mc.dn[c], fp = perWp[m] * em.cot[k] * mc.dp[c];
                if (fn != 0) {
                    trip.emplace_back(a_iN[std::size_t(e.i)], mc.col[c], fn);
                    trip.emplace_back(a_iN[std::size_t(e.j)], mc.col[c], -fn);
                }
                if (fp != 0) {
                    trip.emplace_back(a_iP[std::size_t(e.i)], mc.col[c], fp);
                    trip.emplace_back(a_iP[std::size_t(e.j)], mc.col[c], -fp);
                }
            }
        }
    }
    std::vector<char> fixed(std::size_t(a_unknowns), 0);
    for (const Contact& c : a_contacts)
        for (int i : c.nodes) {
            fixed[std::size_t(a_iV[std::size_t(i)])] = 1;
            if (c.kind == Contact::Ohmic) {
                const bool freed = a_minorityFree[std::size_t(i)];
                if (!freed || !electronsMinority(i)) fixed[std::size_t(a_iN[std::size_t(i)])] = 1;
                if (!freed || electronsMinority(i)) fixed[std::size_t(a_iP[std::size_t(i)])] = 1;
            }
        }
    // J + jω M (M the carriers' boxes: -qA on n's rows, +qA on p's), solved in
    // the quasi-Fermi potentials' changes: δn = n/Vt (δV - δφn), δp = p/Vt
    // (δφp - δV) (Fermi-Dirac's n ℱ'/ℱ, p ℱ'/ℱ for n, p). In n and p a current near nought is the difference of two
    // large terms (a MOS capacitor's inversion electrons, 10^14 above the
    // bulk's): in φ its gradient, well conditioned.
    const double omega = 2 * M_PI * frequency;
    using cd = std::complex<double>;
    std::vector<int> colNode(std::size_t(a_unknowns), -1), colKind(std::size_t(a_unknowns), 0);   // 0 V, 1 n, 2 p
    for (int i = 0; i < a_n; ++i) {
        if (a_iV[std::size_t(i)] >= 0) colNode[std::size_t(a_iV[std::size_t(i)])] = i;
        if (a_iN[std::size_t(i)] >= 0) {
            colNode[std::size_t(a_iN[std::size_t(i)])] = i;
            colKind[std::size_t(a_iN[std::size_t(i)])] = 1;
            colNode[std::size_t(a_iP[std::size_t(i)])] = i;
            colKind[std::size_t(a_iP[std::size_t(i)])] = 2;
        }
    }
    std::vector<Eigen::Triplet<cd>> ct;
    // An entry of the n-p system in the φ one's columns (each in units of Vt).
    auto put = [&](int r, int c, cd v) {
        const int i = colNode[std::size_t(c)];
        if (colKind[std::size_t(c)] == 0) ct.emplace_back(r, c, v * a_Vt);
        else if (colKind[std::size_t(c)] == 1) {
            // (n/Vt, times Vt; Fermi-Dirac's n ℱ'/ℱ.)
            const double s = x(c) * a_rN[std::size_t(i)];
            ct.emplace_back(r, a_iV[std::size_t(i)], v * s);
            ct.emplace_back(r, c, -v * s);
        } else {
            const double s = x(c) * a_rP[std::size_t(i)];
            ct.emplace_back(r, a_iV[std::size_t(i)], -v * s);
            ct.emplace_back(r, c, v * s);
        }
    };
    for (const auto& t : trip)
        if (!fixed[std::size_t(t.row())]) put(t.row(), t.col(), cd(t.value(), 0));
    // Interface traps follow with their own time: δf = (cn (1 - f) δn -
    // cp f δp)/(jω + 1/τ), 1/τ = cn (n + n1) + cp (p + p1) - each level's
    // charge -q N δf, the electrons it takes δUn = N (cn (1 - f) δn - cn (n
    // + n1) δf), the holes δUp = N (cp f δp + cp (p + p1) δf): at jω, in
    // the place of the static changes assembled (to which they come at 0).
    auto trapChanges = [&](const TrapLevel& tl, const TrapState& st, double n, double p, cd* dsn, cd* dsp, cd* dun, cd* dup, cd* dvn, cd* dvp) {
        const cd denom(st.D, omega);
        const cd fn = tl.cn * (1 - st.f) / denom, fp = -tl.cp * st.f / denom;   // δf per δn, δp
        *dsn = -Q * tl.N * fn;
        *dsp = -Q * tl.N * fp;
        *dun = tl.N * (tl.cn * (1 - st.f) - tl.cn * (n + tl.n1) * fn);
        *dup = tl.N * (-tl.cn * (n + tl.n1) * fp);
        *dvn = tl.N * (tl.cp * (p + tl.p1) * fn);
        *dvp = tl.N * (tl.cp * st.f + tl.cp * (p + tl.p1) * fp);
    };
    for (const TrapLevel& tl : a_traps) {
        // (A surface's recombination keeps nothing: its static changes stand.)
        if (!tl.charged) continue;
        const std::size_t k = std::size_t(tl.node);
        const int rv = a_iV[k], rn = a_iN[k], rp = a_iP[k];
        const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
        const double n = x(rn) / gn, p = x(rp) / gp;
        const TrapState st = trapState(tl, n, p);
        cd dsn, dsp, dun, dup, dvn, dvp;
        trapChanges(tl, st, n, p, &dsn, &dsp, &dun, &dup, &dvn, &dvp);
        if (!fixed[std::size_t(rv)]) {
            put(rv, rn, (dsn + Q * tl.N * st.dfdn) * tl.area / gn);
            put(rv, rp, (dsp + Q * tl.N * st.dfdp) * tl.area / gp);
        }
        if (!fixed[std::size_t(rn)]) {
            put(rn, rn, -Q * tl.area * (dun - st.dUdn) / gn);
            put(rn, rp, -Q * tl.area * (dup - st.dUdp) / gp);
        }
        if (!fixed[std::size_t(rp)]) {
            put(rp, rn, Q * tl.area * (dvn - st.dUdn) / gn);
            put(rp, rp, Q * tl.area * (dvp - st.dUdp) / gp);
        }
    }
    for (int i = 0; i < a_n; ++i) {
        const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
        const auto [kn, kp] = stores(i);
        if (rn >= 0 && !fixed[std::size_t(rn)]) put(rn, rn, cd(0, -omega * Q * kn));
        if (rp >= 0 && !fixed[std::size_t(rp)]) put(rp, rp, cd(0, omega * Q * kp));
    }
    // Fixed: δV at a contact; δφn = δφp = δVa at an ohmic one (its carriers
    // fixed). (A circuit's contact's own voltage held with its nodes: the
    // circuit taken after, on the contacts' admittance.)
    for (int k = 0; k < a_unknowns; ++k)
        if (fixed[std::size_t(k)]) ct.emplace_back(k, k, cd(a_Vt, 0));
    for (const Contact& c : a_contacts)
        if (c.iC >= 0) ct.emplace_back(c.iC, c.iC, cd(a_Vt, 0));
    Eigen::SparseMatrix<cd> A(a_unknowns, a_unknowns);
    A.setFromTriplets(ct.begin(), ct.end());
    Eigen::VectorXd rowMax = Eigen::VectorXd::Zero(a_unknowns);
    for (int k = 0; k < A.outerSize(); ++k)
        for (Eigen::SparseMatrix<cd>::InnerIterator itr(A, k); itr; ++itr) rowMax(itr.row()) = std::max(rowMax(itr.row()), std::abs(itr.value()));
    for (int k = 0; k < a_unknowns; ++k)
        if (!(rowMax(k) > 0)) rowMax(k) = 1;
    for (int k = 0; k < A.outerSize(); ++k)
        for (Eigen::SparseMatrix<cd>::InnerIterator itr(A, k); itr; ++itr) itr.valueRef() /= rowMax(itr.row());
    A.makeCompressed();
    SparseLu<cd> lu;
    lu.analyzePattern(A);
    lu.factorize(A);
    if (lu.info() != Eigen::Success) {
        *why = tr("%1: the small-signal matrix is singular").arg(a_node->name());
        return false;
    }
    // The reactions' rows (each contact node's V row as assembled: its
    // charge); the currents weighed as at rest (currentWeights()).
    std::map<int, std::vector<std::pair<int, double>>> rows;
    for (const auto& t : trip)
        if (fixed[std::size_t(t.row())] && colNode[std::size_t(t.row())] >= 0 && colKind[std::size_t(t.row())] == 0) rows[t.row()].emplace_back(t.col(), t.value());
    const std::size_t nc = a_contacts.size();
    const std::vector<std::vector<double>> weight = currentWeights(x);
    // (Each edge's current's change by its mobilities'.)
    auto moved = [&moving, &perWn, &perWp, this](const Eigen::VectorXcd& dx, std::vector<cd>& df) {
        for (const MobilityChange& mc : moving) {
            cd dmun(0, 0), dmup(0, 0);
            for (int c = 0; c < 9; ++c) {
                dmun += mc.dn[c] * dx(mc.col[c]);
                dmup += mc.dp[c] * dx(mc.col[c]);
            }
            const ElementMobility& em = a_elements[std::size_t(mc.t)];
            for (int k = 0; k < 3; ++k) df[std::size_t(em.edge[k])] += em.cot[k] * (perWn[std::size_t(em.edge[k])] * dmun + perWp[std::size_t(em.edge[k])] * dmup);
        }
    };
    // Each contact's current's change for a solution's change dy (δV/Vt,
    // δφn/Vt, δφp/Vt): the edges' currents' and the carriers' at jω weighed,
    // and its metal's charge's at jω.
    auto terminals = [&](const Eigen::VectorXcd& dy) {
        Eigen::VectorXcd dx(a_unknowns);
        for (int j = 0; j < a_unknowns; ++j) {
            const int i = colNode[std::size_t(j)];
            // (As the system's columns have it: Fermi-Dirac's n ℱ'/ℱ.)
            if (colKind[std::size_t(j)] == 0) dx(j) = dy(j) * a_Vt;
            else if (colKind[std::size_t(j)] == 1) dx(j) = x(j) * a_rN[std::size_t(i)] * (dy(a_iV[std::size_t(i)]) - dy(j));
            else dx(j) = x(j) * a_rP[std::size_t(i)] * (dy(j) - dy(a_iV[std::size_t(i)]));
        }
        auto rowDot = [&](int r) {
            cd s(0, 0);
            auto it = rows.find(r);
            if (it == rows.end()) return s;
            for (const auto& [c, v] : it->second) s += v * dx(c);
            return s;
        };
        // Each edge's change of current (Scharfetter and Gummel's, linear).
        std::vector<cd> df(a_edges.size(), cd(0, 0));
        for (std::size_t m = 0; m < a_edges.size(); ++m) {
            const Edge& e = a_edges[m];
            if (e.w <= 0) continue;
            const int i = e.i, j = e.j;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            const int ni = a_iN[std::size_t(i)], nj = a_iN[std::size_t(j)], pi = a_iP[std::size_t(i)], pj = a_iP[std::size_t(j)];
            const cd dV = (dx(a_iV[std::size_t(j)]) - dx(a_iV[std::size_t(i)])) / a_Vt;
            // (Each step's change: the potential's, and Fermi-Dirac's ln γ at
            // each end with its carriers.)
            const cd dVn = dV + a_dlgN[std::size_t(j)] * dx(nj) - a_dlgN[std::size_t(i)] * dx(ni);
            const cd dVp = dV - a_dlgP[std::size_t(j)] * dx(pj) + a_dlgP[std::size_t(i)] * dx(pi);
            const cd dfn = e.wn * (e.fNj * bern(dN) * dx(nj) - e.fNi * bern(-dN) * dx(ni) + (e.fNj * x(nj) * dbern(dN) + e.fNi * x(ni) * dbern(-dN)) * dVn);
            const cd dfp = e.wp * (e.fPi * bern(dP) * dx(pi) - e.fPj * bern(-dP) * dx(pj) + (e.fPi * x(pi) * dbern(dP) + e.fPj * x(pj) * dbern(-dP)) * dVp);
            df[m] = dfn + dfp;
        }
        moved(dx, df);
        std::vector<cd> out(nc, cd(0, 0));
        for (std::size_t c = 0; c < nc; ++c) {
            const Contact& ct2 = a_contacts[c];
            cd dI(0, 0), dQ(0, 0);
            for (int i : ct2.nodes) dQ -= rowDot(a_iV[std::size_t(i)]);
            const std::vector<double>& w = weight[c];
            if (!w.empty()) {
                for (std::size_t m = 0; m < a_edges.size(); ++m) {
                    const double dw = w[std::size_t(a_edges[m].i)] - w[std::size_t(a_edges[m].j)];
                    if (dw != 0) dI += df[m] * dw;
                }
                // (The carriers' change, -qA δn and qA δp on their rows, at jω;
                // the interface traps' charge's.)
                for (int i = 0; i < a_n; ++i)
                    if (w[std::size_t(i)] != 0 && a_iN[std::size_t(i)] >= 0) {
                        const auto [kn, kp] = stores(i);
                        dI += w[std::size_t(i)] * cd(0, omega) * Q * (kp * dx(a_iP[std::size_t(i)]) - kn * dx(a_iN[std::size_t(i)]));
                    }
                for (const TrapLevel& tl : a_traps) {
                    const std::size_t k = std::size_t(tl.node);
                    if (w[k] == 0 || !tl.charged) continue;
                    const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
                    const double n = x(a_iN[k]) / gn, p = x(a_iP[k]) / gp;
                    cd dsn, dsp, dun, dup, dvn, dvp;
                    trapChanges(tl, trapState(tl, n, p), n, p, &dsn, &dsp, &dun, &dup, &dvn, &dvp);
                    dI += w[k] * cd(0, omega) * tl.area * (dsn * dx(a_iN[k]) / gn + dsp * dx(a_iP[k]) / gp);
                }
            }
            out[c] = dI + cd(0, omega) * dQ;
        }
        return out;
    };
    Y.assign(nc, std::vector<cd>(nc, cd(0, 0)));
    for (std::size_t k = 0; k < nc; ++k) {
        Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(a_unknowns);
        // (A fixed row reads Vt y = 1 V: y in units of Vt.)
        for (int i : a_contacts[k].nodes) {
            rhs(a_iV[std::size_t(i)]) = cd(1.0 / rowMax(a_iV[std::size_t(i)]), 0);
            if (a_contacts[k].kind == Contact::Ohmic) {
                const bool freed = a_minorityFree[std::size_t(i)];
                if (!freed || !electronsMinority(i)) rhs(a_iN[std::size_t(i)]) = cd(1.0 / rowMax(a_iN[std::size_t(i)]), 0);
                if (!freed || electronsMinority(i)) rhs(a_iP[std::size_t(i)]) = cd(1.0 / rowMax(a_iP[std::size_t(i)]), 0);
            }
        }
        const std::vector<cd> dI = terminals(lu.solve(rhs));
        for (std::size_t c = 0; c < nc; ++c) Y[c][k] = dI[c];
    }
    // The light modulated wholly (δG = G), the contacts held: each one's
    // current - a photodiode's response, its carriers' transit.
    H.clear();
    if (a_Gop > 0) {
        Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(a_unknowns);
        const std::vector<double>& generated = generationNow();
        for (int i = 0; i < a_n; ++i) {
            const double g = generated.empty() ? 0.0 : generated[std::size_t(i)];
            if (g == 0 || a_iN[std::size_t(i)] < 0) continue;
            const int rn = a_iN[std::size_t(i)], rp = a_iP[std::size_t(i)];
            // (F's rows hold -qA (R - G) and qA (R - G): ∂F/∂G qA and -qA; J δx = -∂F/∂G δG.)
            if (!fixed[std::size_t(rn)]) rhs(rn) = cd(-Q * a_box[std::size_t(i)] * g / rowMax(rn), 0);
            if (!fixed[std::size_t(rp)]) rhs(rp) = cd(Q * a_box[std::size_t(i)] * g / rowMax(rp), 0);
        }
        H = terminals(lu.solve(rhs));
    }
    if (!noise) return true;
    // Noise, by the impedance field: each microscopic source a Poisson
    // process - every edge's carriers' hops each way (diffusion's noise,
    // Nyquist's 4kT G at equilibrium), every node's pairs made and
    // recombined, every ionization - its current to each contact its
    // Green's function's, λ_c^T b, λ_c from the transposed system (A^T λ =
    // the contact's current's coefficients g): S_ab = Σ 2 (rate) h_a h_b*.
    // (The current's coefficients in dx, as terminals() takes them: the
    // edges' changes weighed, the carriers' and traps' at jω, the metal's
    // charge.)
    std::vector<Eigen::VectorXcd> gdx(nc, Eigen::VectorXcd::Zero(a_unknowns));
    std::vector<cd> slope(a_edges.size(), cd(0, 0));   // (scratch)
    for (std::size_t c = 0; c < nc; ++c) {
        Eigen::VectorXcd& g = gdx[c];
        const std::vector<double>& w = weight[c];
        for (int i : a_contacts[c].nodes) {
            auto it = rows.find(a_iV[std::size_t(i)]);
            if (it == rows.end()) continue;
            for (const auto& [col, v] : it->second) g(col) -= cd(0, omega) * v;
        }
        if (w.empty()) continue;
        std::vector<cd> dwOf(a_edges.size(), cd(0, 0));
        for (std::size_t m = 0; m < a_edges.size(); ++m) {
            const Edge& e = a_edges[m];
            if (e.w <= 0) continue;
            const double dw = w[std::size_t(e.i)] - w[std::size_t(e.j)];
            if (dw == 0) continue;
            dwOf[m] = dw;
            double dN = 0, dP = 0;
            edgeSteps(e, x, &dN, &dP);
            const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
            const int ni = a_iN[i], nj = a_iN[j], pi = a_iP[i], pj = a_iP[j], vi = a_iV[i], vj = a_iV[j];
            const double Cn = e.fNj * x(nj) * dbern(dN) + e.fNi * x(ni) * dbern(-dN);
            const double Cp = e.fPi * x(pi) * dbern(dP) + e.fPj * x(pj) * dbern(-dP);
            g(nj) += dw * e.wn * (e.fNj * bern(dN) + Cn * a_dlgN[j]);
            g(ni) += dw * e.wn * (-e.fNi * bern(-dN) - Cn * a_dlgN[i]);
            g(pi) += dw * e.wp * (e.fPi * bern(dP) + Cp * a_dlgP[i]);
            g(pj) += dw * e.wp * (-e.fPj * bern(-dP) - Cp * a_dlgP[j]);
            g(vj) += dw * (e.wn * Cn + e.wp * Cp) / a_Vt;
            g(vi) -= dw * (e.wn * Cn + e.wp * Cp) / a_Vt;
        }
        for (const MobilityChange& mc : moving) {
            const ElementMobility& em = a_elements[std::size_t(mc.t)];
            for (int k = 0; k < 3; ++k) {
                const std::size_t m = std::size_t(em.edge[k]);
                if (dwOf[m] == cd(0, 0)) continue;
                for (int cc = 0; cc < 9; ++cc) g(mc.col[cc]) += dwOf[m] * em.cot[k] * (perWn[m] * mc.dn[cc] + perWp[m] * mc.dp[cc]);
            }
        }
        for (int i = 0; i < a_n; ++i)
            if (w[std::size_t(i)] != 0 && a_iN[std::size_t(i)] >= 0) {
                const auto [kn, kp] = stores(i);
                g(a_iP[std::size_t(i)]) += w[std::size_t(i)] * cd(0, omega) * Q * kp;
                g(a_iN[std::size_t(i)]) -= w[std::size_t(i)] * cd(0, omega) * Q * kn;
            }
        for (const TrapLevel& tl : a_traps) {
            const std::size_t k = std::size_t(tl.node);
            if (w[k] == 0 || !tl.charged) continue;
            const double gn = std::exp(a_lgN[k]), gp = std::exp(a_lgP[k]);
            const double n = x(a_iN[k]) / gn, p = x(a_iP[k]) / gp;
            cd dsn, dsp, dun, dup, dvn, dvp;
            trapChanges(tl, trapState(tl, n, p), n, p, &dsn, &dsp, &dun, &dup, &dvn, &dvp);
            g(a_iN[k]) += w[k] * cd(0, omega) * tl.area * dsn / gn;
            g(a_iP[k]) += w[k] * cd(0, omega) * tl.area * dsp / gp;
        }
    }
    // (In the system's columns: dx of dy.)
    std::vector<Eigen::VectorXcd> gdy(nc, Eigen::VectorXcd::Zero(a_unknowns));
    for (std::size_t c = 0; c < nc; ++c)
        for (int j = 0; j < a_unknowns; ++j) {
            const cd v = gdx[c](j);
            if (v == cd(0, 0)) continue;
            const int i = colNode[std::size_t(j)];
            if (colKind[std::size_t(j)] == 0) gdy[c](j) += v * a_Vt;
            else if (colKind[std::size_t(j)] == 1) {
                const double s = x(j) * a_rN[std::size_t(i)];
                gdy[c](a_iV[std::size_t(i)]) += v * s;
                gdy[c](j) -= v * s;
            } else {
                const double s = x(j) * a_rP[std::size_t(i)];
                gdy[c](j) += v * s;
                gdy[c](a_iV[std::size_t(i)]) -= v * s;
            }
        }
    Eigen::SparseMatrix<cd> At = A.transpose();
    At.makeCompressed();
    SparseLu<cd> lut;
    lut.analyzePattern(At);
    lut.factorize(At);
    if (lut.info() != Eigen::Success) {
        *why = tr("%1: the noise's transposed small-signal matrix is singular").arg(a_node->name());
        return false;
    }
    std::vector<Eigen::VectorXcd> lambda(nc);
    for (std::size_t c = 0; c < nc; ++c) lambda[c] = lut.solve(gdy[c]);
    noise->assign(nc, std::vector<cd>(nc, cd(0, 0)));
    std::vector<cd> h(nc);
    // (A source's currents to the contacts: b on rows r, unscaled; an
    // edge's flux itself a current too, through each contact's weights -
    // Ramo's, w_i - w_j - the carriers' answer to it besides.)
    auto add = [&](double strength, int r1, double b1, int r2, double b2, const Edge* along) {
        if (!(strength > 0)) return;
        for (std::size_t c = 0; c < nc; ++c) {
            h[c] = cd(0, 0);
            if (r1 >= 0 && !fixed[std::size_t(r1)]) h[c] += lambda[c](r1) * b1 / rowMax(r1);
            if (r2 >= 0 && !fixed[std::size_t(r2)]) h[c] += lambda[c](r2) * b2 / rowMax(r2);
            if (along && !weight[c].empty()) h[c] += weight[c][std::size_t(along->i)] - weight[c][std::size_t(along->j)];
        }
        for (std::size_t a = 0; a < nc; ++a)
            for (std::size_t b = 0; b < nc; ++b) (*noise)[a][b] += 2 * strength * h[a] * std::conj(h[b]);
    };
    // Each edge's diffusion noise, each carrier: an extra flux δ on it
    // (F(i) += δ, F(j) -= δ: b = -δ, +δ) of 4q² D n per volume - along the
    // edge, its constant current's density profile n(s) = ni + (nj - ni)
    // (e^(ds) - 1)/(e^d - 1), the sources' weight there 1/n: 4q w n_h, n_h
    // its harmonic mean, d ni/((1 - e^(-d)) φ(r)), r = nj/(ni e^d), φ = ln
    // r/(r - 1). At equilibrium (r 1) the edge's own conductance's 4kT G,
    // Nyquist's; drifting in a uniform density, n's. (Its two streams as
    // Poisson hops, 2q (forward + backward), the same at equilibrium but
    // each edge its drift current's shot noise in a field - d/2 = ΔV/2Vt
    // times too much: an APD's excess 5.1 against McIntyre's 2.1.)
    auto harmonic = [](double ni, double nj, double d) {
        ni = std::max(ni, 1e-300);
        nj = std::max(nj, 1e-300);
        const double r = nj / ni * std::exp(-d), lr = std::log(r);
        const double phi = std::abs(r - 1) > 1e-8 ? lr / (r - 1) : 1 - 0.5 * (r - 1);
        const double spread = std::abs(d) > 1e-8 ? -std::expm1(-d) / d : 1 - 0.5 * d;
        return ni / (spread * phi);
    };
    // (Not where it ionizes - an edge of a triangle making pairs more than
    // 10⁻⁶ of the most: in a local model the carriers' diffusion there
    // changes no ionization - carried forward, a carrier ionizes on the way
    // what its shorter way ahead loses -, and at a junction's dense side it
    // is thermal motion, which the field heats nothing; the discrete forms
    // leaked it, multiplied - an APD's excess 3 to 10 against McIntyre's
    // 2.1, with the mesh. There the noise is its ionizations' and the
    // primary's, McIntyre's.)
    std::vector<char> ionizes(a_edges.size(), 0);
    if (a_ionRate.size() == a_elements.size()) {
        const double most = *std::max_element(a_ionRate.begin(), a_ionRate.end());
        if (most > 0)
            for (int t = 0; t < int(a_elements.size()); ++t)
                if (a_elements[std::size_t(t)].domain >= 0 && a_ionRate[std::size_t(t)] > 1e-6 * most)
                    for (int k = 0; k < 3; ++k) ionizes[std::size_t(a_elements[std::size_t(t)].edge[k])] = 1;
    }
    for (std::size_t m = 0; m < a_edges.size(); ++m) {
        const Edge& e = a_edges[m];
        if (e.w <= 0 || ionizes[m]) continue;
        double dN = 0, dP = 0;
        edgeSteps(e, x, &dN, &dP);
        const std::size_t i = std::size_t(e.i), j = std::size_t(e.j);
        if (e.wn > 0 && a_iN[i] >= 0 && a_iN[j] >= 0)
            add(2 * Q * e.wn * harmonic(e.fNi * x(a_iN[i]), e.fNj * x(a_iN[j]), dN), a_iN[i], -1, a_iN[j], 1, &e);
        if (e.wp > 0 && a_iP[i] >= 0 && a_iP[j] >= 0)
            add(2 * Q * e.wp * harmonic(e.fPi * x(a_iP[i]), e.fPj * x(a_iP[j]), -dP), a_iP[i], -1, a_iP[j], 1, &e);
    }
    // Each node's pairs, made or recombined (each a pair at it: b = -q, +q
    // per event a second): the light's and the others' made, the thermal
    // ones both ways - SRH's (np + ni²)/D, Auger's, radiative's - and the
    // ionizations at its triangles' corners.
    std::vector<double> events(std::size_t(a_n), 0.0);
    const std::vector<double>& generated = generationNow();
    for (int i = 0; i < a_n; ++i) {
        if (a_iN[std::size_t(i)] < 0 || a_nodeDomain[std::size_t(i)] < 0) continue;
        if (!generated.empty()) events[std::size_t(i)] += std::abs(generated[std::size_t(i)]) * a_box[std::size_t(i)] * a_lightScale;
        const double n = x(a_iN[std::size_t(i)]), p = x(a_iP[std::size_t(i)]), ni = a_ni[std::size_t(i)];
        const double both = n * p + ni * ni * std::exp(a_lgN[std::size_t(i)] + a_lgP[std::size_t(i)]);
        for (int k = a_recStart[std::size_t(i)]; k < a_recStart[std::size_t(i) + 1]; ++k) {
            const RecombiningPart& part = a_rec[std::size_t(k)];
            const Medium& in = a_media[std::size_t(part.domain)];
            double rate = 0;
            if (in.srh && std::isfinite(part.tn) && std::isfinite(part.tp)) {
                const double n1 = part.domain == a_nodeDomain[std::size_t(i)] ? a_n1[std::size_t(i)] : ni * std::exp(in.Et / (KB * a_T));
                const double p1 = part.domain == a_nodeDomain[std::size_t(i)] ? a_p1[std::size_t(i)] : ni * std::exp(-in.Et / (KB * a_T));
                rate += both / (part.tp * (n + n1) + part.tn * (p + p1));
            }
            rate += (in.Cn * n + in.Cp * p) * both + in.B * both;
            events[std::size_t(i)] += part.volume * rate;
        }
    }
    if (a_ionRate.size() == std::size_t(field().space->elements()))
        for (int t = 0; t < int(a_elements.size()); ++t) {
            const ElementMobility& em = a_elements[std::size_t(t)];
            if (em.domain < 0 || !(a_ionRate[std::size_t(t)] > 0)) continue;
            const int* tdof = field().space->dofs(t);
            for (int k = 0; k < 3; ++k) events[std::size_t(tdof[k])] += a_ionRate[std::size_t(t)] * em.volume / 3;
        }
    for (int i = 0; i < a_n; ++i)
        if (events[std::size_t(i)] > 0) add(events[std::size_t(i)], a_iN[std::size_t(i)], -Q, a_iP[std::size_t(i)], Q, nullptr);
    (void)slope;
    return true;
}

void SemiconductorProblem::initialize()
{
    // From where it is (a step before's), or a solution at rest at the
    // contacts' voltages now: a study in time starts there.
    if (field().solved) {
        // (Its ionized dopants from its carriers: a history's start.)
        if (a_ionAt > 0) {
            const Eigen::VectorXd x = stateVector();
            degeneracy(x);
            storeIons(x);
        }
        // (Its mobilities - and hot carriers' temperatures, theirs again
        // at it - as they were: the start's snapshot showed setup's, the
        // low field's, and its first step began from them.)
        if ((a_varying || a_energy) && a_slices == 1) {
            const Eigen::VectorXd x = stateVector();
            for (int k = 0; k < (a_energy ? 100 : 1); ++k) {
                std::vector<std::pair<double, double>> was(a_edges.size());
                for (std::size_t e = 0; e < a_edges.size(); ++e) was[e] = {a_edges[e].wn, a_edges[e].wp};
                updateMobilities(x);
                double moved = 0;
                for (std::size_t e = 0; e < a_edges.size(); ++e) {
                    if (was[e].first > 0) moved = std::max(moved, std::abs(a_edges[e].wn / was[e].first - 1));
                    if (was[e].second > 0) moved = std::max(moved, std::abs(a_edges[e].wp / was[e].second - 1));
                }
                if (moved < 1e-9) break;
            }
            store(x);
        }
        return;
    }
    // (A laser's walk to its voltages is its step's, its mode read - no
    // step in time starts from it: walked here without the mode, it was
    // walked again - a Laser L-I of one point the slower, 113 s against 93
    // for 31: the update check of 110e8b9.)
    if (a_laser) return;
    a_generation.assign(std::size_t(a_n), 0.0);
    QString why;
    if (!stationary(&why)) a_initError = why;
}

void SemiconductorProblem::heteroSteps()
{
    // An edge into a heterojunction's other semiconductor: its carriers that
    // one's at both ends, the step from the node's bands to its at the node
    // (without Schrödinger-Poisson's quantum potentials).
    // (Along the edge, the step smeared over its length, as it was and is
    // graded over an element: a barrier one element wide, the current
    // through it the mesh's - a laser's at 1.7 V from 2.2 mA to 77 µA as
    // it was refined.)
    for (Edge& e : a_edges) {
        e.sNi = e.sNj = e.sPi = e.sPj = 0;
        e.fNi = e.fNj = e.fPi = e.fPj = 1;
        if (e.medium < 0) continue;
        const Medium& m = a_media[std::size_t(e.medium)];
        for (const auto& [node, sN, sP, fN, fP] : {std::tuple{e.i, &e.sNi, &e.sPi, &e.fNi, &e.fPi}, std::tuple{e.j, &e.sNj, &e.sPj, &e.fNj, &e.fPj}}) {
            if (a_bandsOf[std::size_t(a_nodeDomain[std::size_t(node)])] == e.medium) continue;
            const auto [tn, tp] = bandsIn(m, node);
            *sN = tn - a_thetaN0[std::size_t(node)];
            *sP = tp - a_thetaP0[std::size_t(node)];
            *fN = std::exp(*sN / a_Vt);
            *fP = std::exp(-*sP / a_Vt);
        }
    }
    a_boxN = a_box;
    a_boxP = a_box;
    for (int i = 0; i < a_n; ++i) {
        const int own = a_nodeDomain[std::size_t(i)];
        if (own < 0) continue;
        double bn = 0, bp = 0;
        for (int k = a_partStart[std::size_t(i)]; k < a_partStart[std::size_t(i) + 1]; ++k) {
            const BoxPart& part = a_parts[std::size_t(k)];
            if (a_bandsOf[std::size_t(part.domain)] == a_bandsOf[std::size_t(own)]) {
                bn += part.volume;
                bp += part.volume;
                continue;
            }
            const auto [tn, tp] = bandsIn(a_media[std::size_t(part.domain)], i);
            bn += part.volume * std::exp((tn - a_thetaN0[std::size_t(i)]) / a_Vt);
            bp += part.volume * std::exp(-(tp - a_thetaP0[std::size_t(i)]) / a_Vt);
        }
        a_boxN[std::size_t(i)] = bn;
        a_boxP[std::size_t(i)] = bp;
    }
}

void SemiconductorProblem::bandExtras()
{
    Field& f = field();
    const Space& sp = *f.space;
    const Mesh& mesh = a_s.mesh();
    for (int t = 0; t < sp.elements(); ++t) {
        if (!a_elemSemi[std::size_t(t)] || f.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1))) continue;
        const Medium& m = a_media[std::size_t(mesh.domain[std::size_t(t)])];
        double* e = &f.extra[std::size_t(ExtraHead + PerTriangle * t)];
        // (The gap's narrowing, its corners' mean: the bands closed by it.
        // Under a lattice temperature the gap shown its material's at TL,
        // ni the carriers' - the rest of the shift.)
        const int* tdof = sp.dofs(t);
        const double gap = (a_gap[std::size_t(tdof[0])] + a_gap[std::size_t(tdof[1])] + a_gap[std::size_t(tdof[2])]) / 3;
        double shown = gap;
        if (!a_nodeTL.empty())
            for (int k = 0; k < 3; ++k) {
                const int c = tdof[k], d = a_nodeDomain[std::size_t(c)];
                if (d >= 0) shown += (thermalGap(a_media[std::size_t(d)], c) - thermalShift(a_media[std::size_t(d)], c)) / 3;
            }
        e[2] = m.dEc - shown / 2;
        e[3] = m.Eg - shown;
        e[4] = m.ni * std::exp(gap / (2 * KB * a_T));
        e[24] = (m.dEc - gap / 2 - m.Et) / (KB * a_T);                  // Ec - Et over kT
        e[25] = (m.Eg - gap - (m.dEc - gap / 2) + m.Et) / (KB * a_T);   // Et - Ev over kT
        // (Its corners' steps from their nodes' bands to its, sN and sP: a
        // heterojunction's node, of the other semiconductor, its carriers
        // there this one's.)
        for (int k = 0; k < 3; ++k) {
            const int i = tdof[k];
            e[26 + k] = e[29 + k] = 0;
            if (a_nodeDomain[std::size_t(i)] < 0 || a_bandsOf[std::size_t(a_nodeDomain[std::size_t(i)])] == a_bandsOf[std::size_t(mesh.domain[std::size_t(t)])]) continue;
            const auto [tn, tp] = bandsIn(m, i);
            e[26 + k] = tn - a_thetaN0[std::size_t(i)];
            e[29 + k] = tp - a_thetaP0[std::size_t(i)];
        }
    }
}

void SemiconductorProblem::thermalBands()
{
    // Each node's gap, intrinsic density and bands at its lattice's
    // temperature (its own medium's; a heterojunction's other side's by
    // bandsIn), then the steps and boxes between them. (Schrödinger-
    // Poisson's quantum potentials on top, as they were.)
    for (int i = 0; i < a_n; ++i) {
        const int d = a_nodeDomain[std::size_t(i)];
        if (d < 0) continue;
        const Medium& m = a_media[std::size_t(d)];
        const double gap = narrowing(m, i);
        a_gap[std::size_t(i)] = gap;
        const double ni = m.ni * std::exp(gap / (2 * KB * a_T));
        a_ni[std::size_t(i)] = ni;
        a_n1[std::size_t(i)] = ni * std::exp(m.Et / (KB * a_T));
        a_p1[std::size_t(i)] = ni * std::exp(-m.Et / (KB * a_T));
        const auto [tn, tp] = bandsIn(m, i);
        a_thetaN0[std::size_t(i)] = tn;
        a_thetaP0[std::size_t(i)] = tp;
        a_thetaN[std::size_t(i)] = tn + (a_lamN.size() == std::size_t(a_n) ? a_lamN[std::size_t(i)] : 0.0);
        a_thetaP[std::size_t(i)] = tp + (a_lamP.size() == std::size_t(a_n) ? a_lamP[std::size_t(i)] : 0.0);
    }
    heteroSteps();
    bandExtras();
}

void SemiconductorProblem::latticeTemperature()
{
    if (!a_TL.isValid()) return;
    // Each triangle's lattice temperature at its middle: its mobilities'
    // factors (TL/T)^-ζ, its saturation velocities' (Quay's) and impact
    // ionization's (van Overstraeten and de Man's γ), the edges weighed
    // again.
    PointEvaluator ev(a_s);
    double yamaguchiFrom = Inf, yamaguchiTo = -Inf;   // (its lattice's temperatures, K)
    for (int t = 0; t < int(a_elements.size()); ++t) {
        ElementMobility& em = a_elements[std::size_t(t)];
        if (em.domain < 0) continue;
        ev.moveTo(t, 1.0 / 3, 1.0 / 3);
        const double T = ev.eval(a_TL);
        if (!(T > 0)) continue;
        const Medium& m = a_media[std::size_t(em.domain)];
        if (m.lowField == Medium::LowField::Yamaguchi) {
            yamaguchiFrom = std::min(yamaguchiFrom, T);
            yamaguchiTo = std::max(yamaguchiTo, T);
        }
        // (Its mobilities, saturation velocities and coefficients are its
        // material's at the physics' T: scaled from there.)
        em.tn = std::pow(T / a_T, -m.zetan);
        em.tp = std::pow(T / a_T, -m.zetap);
        // (Arora's its own temperature's; Dorkel and Leturcq's, Klaassen's
        // and an expression's of TL in elementMobilities; Yamaguchi's of
        // none - its 300 K.)
        if (m.lowField == Medium::LowField::Arora) {
            em.tn = aroraMobility(m.ar[0], em.N, T) / aroraMobility(m.ar[0], em.N, a_T);
            em.tp = aroraMobility(m.ar[1], em.N, T) / aroraMobility(m.ar[1], em.N, a_T);
        } else if (m.lowField != Medium::LowField::Plain)
            em.tn = em.tp = 1;
        if (m.lowField == Medium::LowField::Klaassen) {
            em.pmin[0] = klaassenPmin(T, m.klMe);
            em.pmin[1] = klaassenPmin(T, m.klMh);
        }
        em.vn = quayVsat(m.vsatAn, T) / quayVsat(m.vsatAn, a_T);
        em.vp = quayVsat(m.vsatAp, T) / quayVsat(m.vsatAp, a_T);
        if (m.vsatExp) em.vn = em.vp = expVsat(T) / expVsat(a_T);
        em.ii = phononGamma(m.ionPhonon, T) / phononGamma(m.ionPhonon, a_T);
        em.TL = T;
    }
    if (yamaguchiFrom <= yamaguchiTo && std::max(std::abs(yamaguchiFrom - 300), std::abs(yamaguchiTo - 300)) > 0.05) {
        const QString range = yamaguchiTo - yamaguchiFrom < 0.05 ? formatNumber(yamaguchiTo, 4)
                                                                 : tr("%1 to %2").arg(formatNumber(yamaguchiFrom, 4), formatNumber(yamaguchiTo, 4));
        const QString w = tr("Warning: %1: Yamaguchi's mobility is of no temperature - its 300 K fit's at a lattice temperature of %2 K")
                              .arg(a_node->name(), range);
        if (!a_s.log.contains(w)) a_s.log << w;
    }
    edgeWeights(nullptr);
    // Each node's, its bands made again where it changed (its intrinsic
    // density its material's at TL).
    bool tabled = false;
    for (const Medium& m : a_media) tabled = tabled || !m.lnNiAt.empty();
    if (tabled) {
        std::vector<double> at(std::size_t(a_n), 0.0);
        for (int i = 0; i < a_n; ++i) {
            if (a_nodeDomain[std::size_t(i)] < 0) continue;
            const auto& [t, r] = a_nodePoint[std::size_t(i)];
            if (t < 0) continue;
            ev.moveTo(t, r[0], r[1]);
            const double T = ev.eval(a_TL);
            at[std::size_t(i)] = T > 0 ? T : a_T;
        }
        if (at != a_nodeTL) {
            a_nodeTL = std::move(at);
            thermalBands();
        }
    }
    // (The slices' own then of these: set again as their fields saturate them.)
    a_sliceEdges.clear();
}

bool SemiconductorProblem::solve(const SolveOptions& o, QString* error)
{
    // (A failure at a low temperature said by it: at 1 mK "the equilibrium's
    // potential: a singular matrix", the temperature not named - the hunt of
    // 22 October, N9. A silicon or GaAs junction solves at 77 K, not at 40.)
    const bool ok = solveAt(o, error);
    if (!ok && error && a_gapOverKT > 150 && !error->contains(QLatin1String("cancelled")) && !error->contains(QLatin1String("(Eg/2kT)")))
        *error += tr(" - at %1 its materials' intrinsic density is e^(-%2) of their states' (Eg/2kT): drift and diffusion's carrier "
                     "statistics give out at such a temperature (a junction of silicon or GaAs solves at 77 K, not at 40)")
                      .arg(formatQuantity(a_T, Dim::temperature()))
                      .arg(qRound(a_gapOverKT));
    return ok;
}

bool SemiconductorProblem::solveAt(const SolveOptions& o, QString* error)
{
    if (!a_initError.isEmpty()) {
        *error = a_initError;
        return false;
    }
    if (a_1d) {
        if (o.mass > 0 || (o.harmonic && o.frequency > 0)) {
            *error = tr("%1: solved in 1D it is solved at rest alone - a stationary step, not one in time or frequency").arg(a_node->name());
            return false;
        }
        return solve1d(error);
    }
    if (a_laser) {
        if (o.mass > 0 || (o.harmonic && o.frequency > 0)) {
            *error = tr("%1: %2 lases at rest - a stationary step or a Laser L-I step's points, not a step in time or frequency")
                         .arg(a_node->name(), a_laser->name);
            return false;
        }
        // Its mode as its resonator solved it; none yet (a step before its
        // Eigenfrequency step's): no light, said once.
        QString why;
        if (!prepareLasing(&why)) {
            // (Its mode solved, it cannot lase as it is - its wells' gain not
            // tabulated in a stationary step, no spontaneous emission: refused,
            // not a line of the log - the update check of 110e8b9, 8.)
            const int fi = a_s.fieldIndex(a_laser->mode);
            if (fi >= 0 && a_s.fields()[std::size_t(fi)].solved) {
                *error = why;
                return false;
            }
            if (!a_laserSaid) {
                a_s.log << tr("%1: no lasing yet - %2").arg(a_node->name(), why);
                a_laserSaid = true;
            }
        }
    }
    latticeTemperature();
    // Generation at the nodes, as it is now (it may read other fields, t).
    a_generation.assign(std::size_t(a_n), 0.0);
    {
        // The light of each mode fed P0: its power absorbed across the gap
        // (Qgap, for 1 W) made pairs, a photon each, η of them - along the
        // device its power falls as e^(-2αz): over its length L (the
        // thickness) the mean (1 - e^(-2αL))/2αL of what it starts with. A
        // mode not solved yet (the dark current's step): none.
        std::vector<double> perWatt(a_optical.size(), 0.0), alphaOf(a_optical.size(), 0.0), perPair(a_optical.size(), 0.0);
        a_Pin = 0;
        for (std::size_t k = 0; k < a_optical.size(); ++k) {
            OpticalSource& o = a_optical[k];
            const std::optional<double> alpha = a_s.global(o.mode + QStringLiteral(".alpha"));
            const std::optional<double> lambda = a_s.global(o.mode + QStringLiteral(".lambda0"));
            if (!alpha || !lambda || !(*lambda > 0)) continue;
            // The mode it asks for: the one its step left selected (else
            // said - the light another mode's).
            const std::optional<double> selected = a_s.global(o.mode + QStringLiteral(".mode"));
            if (selected && int(std::lround(*selected)) != o.number) {
                *error = tr("%1: its Optical Generation is lit by mode %2 of %3, and mode %4 is the one solved last (a mode step "
                            "finds as many modes as it asks for: at least %2)")
                             .arg(a_node->name())
                             .arg(o.number)
                             .arg(o.mode)
                             .arg(int(std::lround(*selected)));
                return false;
            }
            // The power fed now (of t in time).
            if (o.power.isValid() && !o.power.isConstant()) {
                PointEvaluator pe(a_s);
                const auto& [t0, r0] = a_nodePoint[std::size_t(std::find_if(a_nodeDomain.begin(), a_nodeDomain.end(), [](int d) { return d >= 0; }) - a_nodeDomain.begin())];
                pe.moveTo(t0, r0[0], r0[1]);
                o.P0 = pe.eval(o.power);
                if (!(std::isfinite(o.P0) && o.P0 >= 0)) {
                    *error = tr("%1: the power fed its Optical Generation is %2, not a number of 0 or more").arg(a_node->name()).arg(o.P0);
                    return false;
                }
            }
            const double L = a_s.axisymmetric() ? 1.0 : a_s.thickness(), x = 2 * std::max(*alpha, 0.0) * L;
            const double mean = x > 1e-9 ? -std::expm1(-x) / x : 1.0;
            alphaOf[k] = std::max(*alpha, 0.0);
            const double photon = 6.62607015e-34 * 299792458.0 / *lambda;
            perWatt[k] = o.eta * o.P0 * mean / photon;
            // (Each pair's photons' energy: η pairs a photon, the rest heat.)
            perPair[k] = o.eta > 0 ? photon / o.eta : 0.0;
            a_Pin += o.P0;
        }
        // By triangle, each its own medium's at its corners, a third of its
        // volume to each (a node's box spans the media about it: a node on
        // germanium's edge took the light germanium absorbs over its
        // silicon's share too - more pairs than photons). Each mode's light
        // then scaled to what the mode itself absorbs over those triangles
        // (its own rule): a junction's corners do not take more or less.
        const Space& sp = *field().space;
        const Mesh& mesh = a_s.mesh();
        const double unit = mesh.unitScale;
        static const double ref[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        PointEvaluator ev(a_s);
        std::vector<double> amount(std::size_t(a_n), 0.0), energy(std::size_t(a_n), 0.0);
        std::vector<std::vector<double>> lit(a_optical.size());   // by source: its pairs by node, before its scale
        std::vector<double> exact(a_optical.size(), 0.0), discrete(a_optical.size(), 0.0);
        const TriangleRule& rule = triangleRule(4);
        for (int t = 0; t < sp.elements(); ++t) {
            if (!a_elemSemi[std::size_t(t)]) continue;
            const int d = mesh.domain[std::size_t(t)];
            const Medium& m = a_media[std::size_t(d)];
            if (m.generation.empty() && m.optical.empty()) continue;
            const int* dof = sp.dofs(t);
            QPointF x[3];
            for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * unit;
            const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
            const double third = std::abs(det) / 2 * a_s.weight((x[0] + x[1] + x[2]) / 3.0) / 3;
            for (int k = 0; k < 3; ++k) {
                ev.moveTo(t, ref[k][0], ref[k][1]);
                double g = 0, made = 0;
                for (std::size_t u = 0; u < m.generation.size(); ++u) {
                    const double v = ev.eval(m.generation[u]);
                    if (!std::isfinite(v)) continue;
                    g += v;
                    const double photon = u < m.generationPhoton.size() ? m.generationPhoton[u] : NaN;
                    made += v * (std::isfinite(photon) ? photon : m.Eg - a_gap[std::size_t(dof[k])]);
                }
                amount[std::size_t(dof[k])] += g * third;
                energy[std::size_t(dof[k])] += made * third;
                for (int o : m.optical) {
                    if (perWatt[std::size_t(o)] == 0 || !a_optical[std::size_t(o)].absorbed.isValid()) continue;
                    const double q = ev.eval(a_optical[std::size_t(o)].absorbed);
                    if (!(std::isfinite(q) && q > 0)) continue;
                    std::vector<double>& to = lit[std::size_t(o)];
                    if (to.empty()) to.assign(std::size_t(a_n), 0.0);
                    to[std::size_t(dof[k])] += q * third;
                    discrete[std::size_t(o)] += q * third;
                }
            }
            for (int o : m.optical) {
                if (perWatt[std::size_t(o)] == 0 || !a_optical[std::size_t(o)].absorbed.isValid()) continue;
                for (const auto& q : rule.points) {
                    ev.moveTo(t, q[0], q[1]);
                    const QPointF at = x[0] + q[0] * (x[1] - x[0]) + q[1] * (x[2] - x[0]);
                    const double v = ev.eval(a_optical[std::size_t(o)].absorbed);
                    if (std::isfinite(v) && v > 0) exact[std::size_t(o)] += q[2] * std::abs(det) * a_s.weight(at) * v;
                }
            }
        }
        const std::vector<double> userAmount = amount, userEnergy = energy;
        std::vector<double> scaleOf(a_optical.size(), 0.0);
        for (std::size_t o = 0; o < a_optical.size(); ++o) {
            if (lit[o].empty() || !(discrete[o] > 0)) continue;
            const double scale = perWatt[o] * exact[o] / discrete[o];
            scaleOf[o] = scale;
            for (int i = 0; i < a_n; ++i) {
                amount[std::size_t(i)] += lit[o][std::size_t(i)] * scale;
                energy[std::size_t(i)] += lit[o][std::size_t(i)] * scale * perPair[o];
            }
        }
        a_pairEnergy.assign(std::size_t(a_n), 0.0);
        a_Pabs = 0;
        for (int i = 0; i < a_n; ++i) {
            if (!(a_box[std::size_t(i)] > 0)) continue;
            a_generation[std::size_t(i)] = amount[std::size_t(i)] / a_box[std::size_t(i)];
            a_pairEnergy[std::size_t(i)] = energy[std::size_t(i)] / a_box[std::size_t(i)];
            a_Pabs += energy[std::size_t(i)];
        }
        // Slices along the length: each absorbing an equal share of the
        // first lit source's light (its power falling as e^(-2αz)) - the
        // finer where it is the stronger, at the start -, in each the light
        // of each source the mean of e^(-2αz) over it against the whole
        // length's (each slice's length its share; their pairs together all
        // of them).
        a_sliceGeneration.clear();
        a_slicePairEnergy.clear();
        if (a_slices > 1) {
            const std::size_t S = std::size_t(a_slices);
            const double L = a_s.thickness();
            double first = 0;
            for (std::size_t o = 0; o < a_optical.size() && !(first > 0); ++o)
                if (scaleOf[o] > 0) first = alphaOf[o];
            a_sliceZ.assign(S + 1, 0.0);
            for (std::size_t k = 0; k <= S; ++k) {
                const double share = double(k) / double(S);
                a_sliceZ[k] = first * L > 1e-9 ? -std::log1p(share * std::expm1(-2 * first * L)) / (2 * first) : share * L;
            }
            a_sliceZ[S] = L;
            a_sliceWeight.assign(S, 0.0);
            for (std::size_t k = 0; k < S; ++k) a_sliceWeight[k] = (a_sliceZ[k + 1] - a_sliceZ[k]) / L;
            // (The mean of e^(-2αz) from z0 to z1.)
            auto mean = [](double alpha, double z0, double z1) {
                const double x = 2 * alpha * (z1 - z0);
                return x > 1e-12 ? std::exp(-2 * alpha * z0) * -std::expm1(-x) / x : std::exp(-2 * alpha * z0);
            };
            a_sliceGeneration.assign(S, std::vector<double>(std::size_t(a_n), 0.0));
            a_slicePairEnergy.assign(S, std::vector<double>(std::size_t(a_n), 0.0));
            for (std::size_t k = 0; k < S; ++k) {
                std::vector<double> here = userAmount, made = userEnergy;
                for (std::size_t o = 0; o < a_optical.size(); ++o) {
                    if (!(scaleOf[o] > 0)) continue;
                    const double factor = mean(alphaOf[o], a_sliceZ[k], a_sliceZ[k + 1]) / mean(alphaOf[o], 0, L);
                    for (int i = 0; i < a_n; ++i) {
                        here[std::size_t(i)] += lit[o][std::size_t(i)] * scaleOf[o] * factor;
                        made[std::size_t(i)] += lit[o][std::size_t(i)] * scaleOf[o] * factor * perPair[o];
                    }
                }
                for (int i = 0; i < a_n; ++i)
                    if (a_box[std::size_t(i)] > 0) {
                        a_sliceGeneration[k][std::size_t(i)] = here[std::size_t(i)] / a_box[std::size_t(i)];
                        a_slicePairEnergy[k][std::size_t(i)] = made[std::size_t(i)] / a_box[std::size_t(i)];
                    }
            }
        }
        a_Gop = 0;
        for (int i = 0; i < a_n; ++i) a_Gop += a_generation[std::size_t(i)] * a_box[std::size_t(i)];
    }
    if (o.harmonic && o.frequency > 0) {
        if (!field().solved && !stationary(error)) return false;
        return smallSignal(o.frequency, error);
    }
    a_Y.clear();
    a_inTime = o.mass > 0 && o.history;
    if (o.mass > 0 && o.history) {
        // A step in time: straight to the contacts' voltages now.
        std::vector<double> goal;
        if (!targets(goal, error)) return false;
        contactValues(goal);
        Eigen::VectorXd x = stateVector();
        int steps = 0;
        QString why;
        if (!newton(x, o.mass, o.history, 40, &steps, &why)) {
            *error = why;   // ("did not converge": the step is taken again, smaller)
            return false;
        }
        for (std::size_t k = 0; k < a_contacts.size(); ++k) {
            Contact& c = a_contacts[k];
            c.applied = goal[k];
            // (Cp's current, Cp dVc/dt.)
            c.capCurrent = c.iC >= 0 && c.Cp > 0 && a_circuitAt > 0 && o.history->size() >= a_circuitAt + 3 * a_contacts.size()
                               ? c.Cp * (o.mass * x(c.iC) - (*o.history)[a_circuitAt + 3 * k])
                               : 0.0;
        }
        terminalAll(x, o.mass, o.history);
        store(x);
        return true;
    }
    return stationary(error);
}

bool SemiconductorProblem::solve1d(QString* error)
{
    // The line's stretches, each in one domain (along it, m), and the
    // geometry's boundaries at their ends.
    struct Stretch {
        double s0 = 0, s1 = 0;
        int domain = -1;
        std::set<int> start, end;
        int auxDomain = -1, auxStart = -1, auxEnd = -1;
    };
    std::vector<Stretch> stretches;
    for (int k = 0; k < a_line.elements(); ++k) {
        const int d = a_line.domain[std::size_t(k)];
        if (stretches.empty() || stretches.back().domain != d) {
            Stretch st;
            st.s0 = a_line.z[std::size_t(2 * k)];
            st.domain = d;
            st.start = a_line.boundaries[std::size_t(2 * k)];
            stretches.push_back(st);
        }
        stretches.back().s1 = a_line.z[std::size_t(2 * k + 2)];
        stretches.back().end = a_line.boundaries[std::size_t(2 * k + 2)];
    }
    if (stretches.empty()) {
        *error = tr("%1: its line crosses none of its domains").arg(a_node->name());
        return false;
    }
    const Model& parent = a_s.model();
    const Topology& topo = a_s.topology();
    const double unit = a_s.mesh().unitScale;
    const bool alongY = a_axis1d == 1;
    // The strip, in the geometry's unit: a rectangle each stretch, the line
    // its middle, as wide as the line is long, two columns across - read
    // along its middle. (A node where two layers meet takes their dopings
    // by its circumcentric box: a side's balanced as the middle's. Eight
    // columns, once a side's leaned: 3 to 10 times a 2D Layered Region's
    // time.)
    const double across = (alongY ? a_line.S.x() : a_line.S.y()) / unit, origin = (alongY ? a_line.S.y() : a_line.S.x()) / unit;
    const double width = a_line.L / unit;
    constexpr int Columns = 2;
    auto stripPoint = [alongY, across, origin](double along, double side) {
        return alongY ? QPointF(across + side, origin + along) : QPointF(origin + along, across + side);
    };
    Model aux = parent;
    aux.geometry().children.clear();
    for (std::size_t k = 0; k < stretches.size(); ++k) {
        Node r = aux.make(QStringLiteral("rectangle"));
        r.label = QStringLiteral("line1d_%1").arg(k + 1);
        const double length = (stretches[k].s1 - stretches[k].s0) / unit;
        const QPointF corner = stripPoint(stretches[k].s0 / unit, -width / 2);
        r.props.insert(QStringLiteral("position"), QJsonArray{formatNumber(corner.x(), 17), formatNumber(corner.y(), 17)});
        r.props.insert(QStringLiteral("size"), alongY ? QJsonArray{formatNumber(width, 17), formatNumber(length, 17)}
                                                      : QJsonArray{formatNumber(length, 17), formatNumber(width, 17)});
        aux.geometry().children.push_back(r);
    }
    const GeometryBuild built = buildGeometry(aux, *a_s.parametersPtr());
    if (!built.topology || !built.errors.isEmpty()) {
        *error = tr("%1: its line's strip: %2").arg(a_node->name(), QStringList(built.errors.values()).join(QLatin1String("; ")));
        return false;
    }
    const Topology& strip = *built.topology;
    // Each stretch's domain there, the boundaries at its ends.
    const double tolerance = width * 1e-6;
    for (Stretch& st : stretches) {
        // (Off the line: not on a rectangle's diagonal.)
        st.auxDomain = strip.domainAt(stripPoint((st.s0 + st.s1) / 2 / unit, width / (3 * Columns)));
        st.auxStart = strip.boundaryAt(stripPoint(st.s0 / unit, 0), tolerance);
        st.auxEnd = strip.boundaryAt(stripPoint(st.s1 / unit, 0), tolerance);
        if (st.auxDomain < 0 || st.auxStart < 0 || st.auxEnd < 0) {
            *error = tr("%1: its line's strip is not as made").arg(a_node->name());
            return false;
        }
    }
    auto numbers = [](const std::vector<int>& items) {
        QJsonArray a;
        for (int i : items) a.append(i + 1);
        return QJsonObject{{QStringLiteral("numbers"), a}};
    };
    std::vector<int> allDomains;
    for (const Stretch& st : stretches) allDomains.push_back(st.auxDomain);
    // Its materials on the stretches of their domains.
    QString why;
    for (Node& m : aux.materials().children) {
        const QVector<int> on = resolveSelection(topo, parent, Level::Domain, m.selection(), &why);
        std::vector<int> mine;
        for (const Stretch& st : stretches)
            if (on.contains(st.domain)) mine.push_back(st.auxDomain);
        if (mine.empty()) m.enabled = false;
        else m.props.insert(QStringLiteral("selection"), numbers(mine));
    }
    // This physics alone, its features on the strip's: a domain's on its
    // stretches, a boundary's where the line meets it.
    for (Node& n : aux.component().children) {
        if (!physicsTypes().contains(n.type)) continue;
        if (n.tag != a_node->tag) {
            n.enabled = false;
            continue;
        }
        n.props.insert(QStringLiteral("space"), QStringLiteral("2d"));
        n.props.insert(QStringLiteral("selection"), numbers(allDomains));
        for (Node& feature : n.children) {
            if (!feature.enabled) continue;
            const NodeKind* kind = feature.kind();
            const Level level = kind ? kind->selection : Level::None;
            if (level == Level::Domain) {
                const QVector<int> on = resolveSelection(topo, parent, Level::Domain, feature.selection(), &why);
                std::vector<int> mine;
                for (const Stretch& st : stretches)
                    if (on.contains(st.domain)) mine.push_back(st.auxDomain);
                if (mine.empty()) feature.enabled = false;
                else feature.props.insert(QStringLiteral("selection"), numbers(mine));
            } else if (level == Level::Boundary) {
                // (The insulation: the strip's outside, as made.)
                if (feature.type == QLatin1String("semiconductor/insulation")) {
                    feature.props.insert(QStringLiteral("selection"), aux.make(feature.type).value(QStringLiteral("selection")));
                    continue;
                }
                const QVector<int> on = resolveSelection(topo, parent, Level::Boundary, feature.selection(), &why);
                std::vector<int> mine;
                for (std::size_t k = 0; k < stretches.size(); ++k) {
                    for (int b : stretches[k].start)
                        if (on.contains(b) && std::find(mine.begin(), mine.end(), stretches[k].auxStart) == mine.end()) mine.push_back(stretches[k].auxStart);
                    if (k + 1 == stretches.size())
                        for (int b : stretches[k].end)
                            if (on.contains(b) && std::find(mine.begin(), mine.end(), stretches[k].auxEnd) == mine.end()) mine.push_back(stretches[k].auxEnd);
                }
                const bool contact = feature.type == QLatin1String("semiconductor/ohmic") || feature.type == QLatin1String("semiconductor/schottky")
                                     || feature.type == QLatin1String("semiconductor/gate");
                if (mine.empty() && contact) {
                    *error = tr("%1: %2 is not where its line ends or meets an interface - a 1D one's contacts are across its line")
                                 .arg(a_node->name(), feature.name());
                    return false;
                }
                if (mine.empty()) feature.enabled = false;
                else feature.props.insert(QStringLiteral("selection"), numbers(mine));
            } else if (level == Level::Point) {
                feature.enabled = false;
            }
        }
    }
    // Its mesh: a Layered Region each stretch, one column, a_h1d along it.
    Node& mesh = aux.mesh();
    mesh.children.clear();
    mesh.props.insert(QStringLiteral("custom"), true);
    mesh.props.insert(QStringLiteral("hmax"), formatNumber(width, 17));
    for (const Stretch& st : stretches) {
        Node layered = aux.make(QStringLiteral("mesh/layered"));
        layered.props.insert(QStringLiteral("axis"), alongY ? QStringLiteral("y") : QStringLiteral("x"));
        layered.props.insert(QStringLiteral("hlateral"), formatNumber(width / Columns, 17));
        layered.props.insert(QStringLiteral("count"), std::max(1, int(std::ceil((st.s1 - st.s0) / a_h1d - 1e-9))));
        layered.props.insert(QStringLiteral("selection"), numbers({st.auxDomain}));
        mesh.children.push_back(layered);
    }
    // A study of one stationary step.
    std::vector<Node> kept;
    for (Node& c : aux.root.children)
        if (c.type != QLatin1String("study")) kept.push_back(c);
    aux.root.children = kept;
    Node study = aux.make(QStringLiteral("study"));
    aux.root.children.push_back(study);
    const MeshBuild meshed = buildMesh(aux, *a_s.parametersPtr(), built.topology);
    if (!meshed.mesh) {
        *error = tr("%1: its line's strip: %2").arg(a_node->name(), meshed.error);
        return false;
    }
    // From a sweep's point before, its strip meshed alike: its solution,
    // the contacts' voltages raised from there. (Each point walked from
    // equilibrium again: 8 points of a PIN 0.75 s in 1D, 0.45 s on a 2D
    // Layered Region - which starts from the point before: the update check
    // of 8bd5709.)
    std::shared_ptr<const Mesh> stripMesh = meshed.mesh;
    const Solution* before = nullptr;
    if (const std::shared_ptr<const Solution> last = field().line) {
        const Mesh& m = last->mesh();
        if (m.nodes == meshed.mesh->nodes && m.triangles == meshed.mesh->triangles && m.domain == meshed.mesh->domain) {
            stripMesh = last->meshPtr();
            before = last.get();
        }
    }
    const StudyResult solved = solveStudy(aux, a_s.parametersPtr(), stripMesh, study.tag, [this](double, const QString&) { return !cancelled(); }, before);
    if (!solved.error.isEmpty() || !solved.solution) {
        *error = tr("%1, along its line: %2").arg(a_node->name(), solved.error);
        return false;
    }
    for (const QString& w : solved.warnings) a_s.log << tr("Warning: %1, along its line: %2").arg(a_node->name(), w);
    // Its solution along the strip's middle: V, n, p at each node.
    const Solution& line = *solved.solution;
    const int lf = line.fieldIndex(field().tag);
    if (lf < 0) {
        *error = tr("%1: its line's solution has no %2").arg(a_node->name(), field().tag);
        return false;
    }
    const Field& lfield = line.fields()[std::size_t(lf)];
    // (The unknowns it solved: the strip's, not this mesh's.)
    a_s.fields()[std::size_t(a_fi)].unknowns = lfield.space->size() * lfield.count();
    a_s.fields()[std::size_t(a_fi)].line = solved.solution;
    const Mesh& lmesh = line.mesh();
    const Space& lsp = *lfield.space;
    const std::size_t LN = std::size_t(lsp.size());
    struct Sample {
        double c, V, n, p;
    };
    std::vector<Sample> profile;
    {
        std::vector<int> dofOf(lmesh.nodes.size(), -1);
        for (int t = 0; t < lsp.elements(); ++t)
            for (int k = 0; k < 3; ++k) dofOf[std::size_t(lmesh.triangles[std::size_t(t)][std::size_t(k)])] = lsp.dofs(t)[k];
        for (std::size_t i = 0; i < lmesh.nodes.size(); ++i) {
            const QPointF q = lmesh.nodes[i];
            if (dofOf[i] < 0 || std::abs((alongY ? q.x() : q.y()) - across) > tolerance) continue;
            const std::size_t dof = std::size_t(dofOf[i]);
            profile.push_back({alongY ? q.y() : q.x(), lfield.values[dof], lfield.values[LN + dof], lfield.values[2 * LN + dof]});
        }
        std::sort(profile.begin(), profile.end(), [](const Sample& a, const Sample& b) { return a.c < b.c; });
    }
    if (profile.size() < 2) {
        *error = tr("%1: its line's solution has no nodes along it").arg(a_node->name());
        return false;
    }
    {
        // Read at a point along the line itself - PointEvaluator,
        // semiconductorAt -: each segment's ends as its own material reads
        // them (a heterojunction's node of the other semiconductor on this
        // one's bands, as a triangle's), and the quasi-Fermi potentials with
        // V and the carriers. (One value a node read the AlGaAs's carriers
        // from the GaAs's, 46 times too many, and φn came from this mesh's
        // nodes, 0.389 V for 0.059: the hunt of 22 October, A3 and A4.)
        auto along = std::make_shared<AlongLine>();
        along->alongY = alongY;
        along->scale = a_s.mesh().unitScale;
        for (const Sample& sm : profile)
            if (along->c.empty() || sm.c > along->c.back()) along->c.push_back(sm.c);
        const std::size_t segments = along->c.size() >= 2 ? along->c.size() - 1 : 0;
        along->from.assign(segments, {NaN, NaN, NaN, NaN, NaN});
        along->to.assign(segments, {NaN, NaN, NaN, NaN, NaN});
        const double Vt = lfield.extra.size() > 2 ? lfield.extra[0] : NaN, niRef = lfield.extra.size() > 2 ? lfield.extra[2] : NaN;
        const bool banded = lfield.values.size() >= 7 * LN;
        auto end = [&](int t, int k) {
            const std::size_t dof = std::size_t(lsp.dofs(t)[k]);
            double V = lfield.values[dof], n = lfield.values[LN + dof], p = lfield.values[2 * LN + dof];
            double thn = banded ? lfield.values[5 * LN + dof] : 0.0, thp = banded ? lfield.values[6 * LN + dof] : 0.0;
            if (lfield.extra.size() >= std::size_t(ExtraHead + PerTriangle * (t + 1))) {
                const double* ex = &lfield.extra[std::size_t(ExtraHead + PerTriangle * t)];
                if (std::isfinite(ex[0]) && (ex[26 + k] != 0 || ex[29 + k] != 0)) {
                    thn += ex[26 + k];
                    thp += ex[29 + k];
                    n *= std::exp(ex[26 + k] / Vt);
                    p *= std::exp(-ex[29 + k] / Vt);
                }
            }
            return std::array<double, 5>{V, n, p, V + thn - Vt * std::log(std::max(n, 1e-300) / niRef),
                                         V + thp + Vt * std::log(std::max(p, 1e-300) / niRef)};
        };
        std::vector<char> filled(segments, 0);
        for (int t = 0; t < lsp.elements() && segments > 0; ++t) {
            int on[3], count = 0;
            for (int k = 0; k < 3; ++k) {
                const QPointF q = lmesh.nodes[std::size_t(lmesh.triangles[std::size_t(t)][std::size_t(k)])];
                if (std::abs((alongY ? q.x() : q.y()) - across) <= tolerance) on[count++] = k;
            }
            if (count < 2) continue;
            auto coord = [&](int k) {
                const QPointF q = lmesh.nodes[std::size_t(lmesh.triangles[std::size_t(t)][std::size_t(k)])];
                return alongY ? q.y() : q.x();
            };
            int ka = on[0], kb = on[1];
            if (coord(ka) > coord(kb)) std::swap(ka, kb);
            const auto it = std::lower_bound(along->c.begin(), along->c.end(), coord(ka) - tolerance);
            const std::size_t j = std::size_t(it - along->c.begin());
            if (j >= segments || filled[j] || std::abs(along->c[j] - coord(ka)) > tolerance || std::abs(along->c[j + 1] - coord(kb)) > tolerance)
                continue;
            along->from[j] = end(t, ka);
            along->to[j] = end(t, kb);
            filled[j] = 1;
        }
        // (A segment no triangle of the strip lies along: its nodes' values.)
        for (std::size_t j = 0; j < segments; ++j) {
            if (filled[j]) continue;
            for (const Sample& sm : profile) {
                if (sm.c == along->c[j]) along->from[j] = {sm.V, sm.n, sm.p, NaN, NaN};
                if (sm.c == along->c[j + 1]) along->to[j] = {sm.V, sm.n, sm.p, NaN, NaN};
            }
        }
        a_s.fields()[std::size_t(a_fi)].along = along;
    }
    // Each node of its own by its coordinate: V linear between the line's
    // nodes, the carriers in their logarithm (held beyond its ends).
    const Field& f = field();
    const Space& sp = *f.space;
    const Mesh& own = a_s.mesh();
    std::vector<double> coordinate(std::size_t(a_n), NaN);
    for (int t = 0; t < sp.elements(); ++t)
        for (int k = 0; k < 3; ++k) {
            const QPointF q = own.nodes[std::size_t(own.triangles[std::size_t(t)][std::size_t(k)])];
            coordinate[std::size_t(sp.dofs(t)[k])] = alongY ? q.y() : q.x();
        }
    Eigen::VectorXd x = Eigen::VectorXd::Zero(a_unknowns);
    for (int i = 0; i < a_n; ++i) {
        const double c = std::clamp(coordinate[std::size_t(i)], profile.front().c, profile.back().c);
        if (!std::isfinite(c)) continue;
        const auto upper = std::upper_bound(profile.begin(), profile.end(), c, [](double v, const Sample& s) { return v < s.c; });
        const std::size_t j = std::min<std::size_t>(std::size_t(std::max<std::ptrdiff_t>(upper - profile.begin(), 1)) - 1, profile.size() - 2);
        const Sample &a = profile[j], &b = profile[j + 1];
        const double u = b.c > a.c ? std::clamp((c - a.c) / (b.c - a.c), 0.0, 1.0) : 0.0;
        auto carriers = [u](double x0, double x1) {
            return x0 > 0 && x1 > 0 ? std::exp((1 - u) * std::log(x0) + u * std::log(x1)) : (1 - u) * x0 + u * x1;
        };
        const std::size_t ii = std::size_t(i);
        if (a_iV[ii] >= 0) x(a_iV[ii]) = (1 - u) * a.V + u * b.V;
        if (a_iN[ii] >= 0) x(a_iN[ii]) = carriers(a.n, b.n);
        if (a_iP[ii] >= 0) x(a_iP[ii]) = carriers(a.p, b.p);
    }
    // The contacts: their voltages, and the line's currents and charges per
    // area (its contacts a strip's width) times the stack's: the narrowest of
    // its stretches' domains across the line (a mesa's), every contact alike
    // - the current through one stack, kept.
    std::vector<double> goal;
    if (!targets(goal, error)) return false;
    double narrowest = Inf;
    for (const Stretch& st : stretches) {
        const QRectF b = topo.domains[std::size_t(st.domain)].bounds;
        narrowest = std::min(narrowest, alongY ? b.width() : b.height());
    }
    const double scale = narrowest / width;
    for (std::size_t k = 0; k < a_contacts.size(); ++k) {
        Contact& c = a_contacts[k];
        c.applied = goal[k];
        for (const Field::Terminal& term : lfield.terminals)
            if (term.name == c.name) {
                c.current = term.charge * scale;
                c.charge = term.chargeIm * scale;
            }
    }
    store(x);
    // What it is not, said: its line's place not given (the middle of its
    // domains taken); a stack narrower somewhere (a mesa) - its current
    // through the narrowest, as a straight column's: where it widens, a 2D
    // solve's spreads (4 to 11 % more forward, 35 % through narrow-wide-
    // narrow); this mesh along the line coarser than its own - what reads
    // it here (fem_evaluate, a mode's index) sees it that coarse.
    if (a_node->text(QStringLiteral("at")).trimmed().isEmpty()) {
        // (Each a warning, not a log line alone: the update check of 8bd5709.)
        const QString w = tr("Warning: %1: solved in 1D along its domains' middle, %2 = %3 %4 - its \"at\" not given")
                              .arg(a_node->name(), alongY ? QStringLiteral("x") : QStringLiteral("y"), formatNumber(across, 6), a_s.topology().unitName);
        if (!a_s.log.contains(w)) a_s.log << w;
    }
    {
        double widest = 0;
        for (const Stretch& st : stretches) {
            const QRectF b = topo.domains[std::size_t(st.domain)].bounds;
            widest = std::max(widest, alongY ? b.width() : b.height());
        }
        if (widest > 1.05 * narrowest) {
            const QString w = tr("Warning: %1: solved in 1D its current is a straight column's %2 %3 across (its narrowest domain's) - "
                                 "a stack %4 %3 wide elsewhere spreads it, which a 2D solve takes (a mesa's 2D current was 4 to 11 % "
                                 "more forward, 35 % through narrow-wide-narrow)")
                                  .arg(a_node->name(), formatNumber(narrowest, 4), a_s.topology().unitName, formatNumber(widest, 4));
            if (!a_s.log.contains(w)) a_s.log << w;
        }
    }
    {
        // (Its nodes along the line: the widest gap between those of this
        // mesh within its domains, against its own h1d.)
        std::vector<double> along;
        for (int i = 0; i < a_n; ++i)
            if (std::isfinite(coordinate[std::size_t(i)])) along.push_back(coordinate[std::size_t(i)]);
        std::sort(along.begin(), along.end());
        along.erase(std::unique(along.begin(), along.end(), [](double a, double b) { return std::abs(a - b) < 1e-12; }), along.end());
        double gap = 0;
        for (std::size_t k = 1; k < along.size(); ++k) gap = std::max(gap, (along[k] - along[k - 1]) * unit);
        if (gap > 4 * a_h1d) {
            const QString w = tr("Warning: %1: solved in 1D every %2 nm along its line, this mesh's nodes up to %3 nm apart: at a point "
                                 "(fem_evaluate, a plot, a cut line, an export) its potential, carriers and what follows from them - the "
                                 "field, the bands - are read along its line; its currents and another physics coupled to it are this "
                                 "mesh's, that coarse (its terminals' currents its line's): a Layered Region along the line as fine keeps "
                                 "their detail")
                                  .arg(a_node->name(), formatNumber(a_h1d * 1e9, 3), formatNumber(gap * 1e9, 3));
            if (!a_s.log.contains(w)) a_s.log << w;
        }
    }
    const QString said = tr("%1: solved in 1D along %2 = %3 %4: %5 nodes on %6 stretches, its currents through %7 %4 across")
                             .arg(a_node->name(), alongY ? QStringLiteral("x") : QStringLiteral("y"), formatNumber(across, 6), a_s.topology().unitName)
                             .arg(profile.size())
                             .arg(stretches.size())
                             .arg(formatNumber(narrowest, 6));
    if (!a_s.log.contains(said)) a_s.log << said;
    return true;
}

QString matrixEntryName(const QString& tag, const QString& letter, const QString& a, const QString& b)
{
    if (a.size() == 1 && b.size() == 1) return tag + QLatin1Char('.') + letter + a + b;
    return tag + QLatin1Char('.') + letter + QLatin1Char('_') + a + QLatin1Char('_') + b;
}

void SemiconductorProblem::setGlobals()
{
    const QString p = field().tag + QLatin1Char('.');
    // Cut into slices along the length: each one's start and its share of
    // each contact's current (its own length's).
    if (a_slices > 1 && a_sliceZ.size() == std::size_t(a_slices) + 1) {
        a_s.setGlobal(p + QStringLiteral("slices"), a_slices, Dim(), tr("slices along the length, each the cross-section at its place"));
        for (int sl = 0; sl < a_slices; ++sl) {
            a_s.setGlobal(p + QStringLiteral("zs%1").arg(sl + 1), a_sliceZ[std::size_t(sl)], Dim::length(),
                          tr("where slice %1 starts along the device (the light fed at 0)").arg(sl + 1));
            if (std::size_t(sl) < a_sliceCurrent.size())
                for (std::size_t k = 0; k < a_contacts.size(); ++k)
                    a_s.setGlobal(p + QStringLiteral("Is%1_%2").arg(sl + 1).arg(a_contacts[k].name), a_sliceCurrent[std::size_t(sl)][k], Dim::current(),
                                  tr("current into the device at %1 through slice %2 (its length's share)").arg(a_contacts[k].name).arg(sl + 1));
        }
    }
    // Fowler-Nordheim: through each edge J = A E² e^(-B/E) at the barrier's
    // mean field - φB over the distance into the insulator, along the
    // edge's normal from its middle, at which the potential has moved by
    // φB/q (the classical turning point: Lenzlinger and Snow's triangle
    // fitted to the barrier's width), or the potential's change over the
    // insulator where it is thinner (a trapezoid: the tunnelling direct).
    // Not the field of the triangle at the edge: an inversion layer
    // thinner than the elements (0.1 nm in silicon under 10 MV/cm of
    // oxide) has its charge in the interface's nodes, each node's
    // potential moved by its box's depth (Vt ln of it: 25 mV from node to
    // node, meshed at 2 nm), and the first triangles' fields with it -
    // ±1 %, J 26 times as much: the current 1.5 % low and ±1.2 % with the
    // mesh; over the barrier, within 0.1 %.
    if (!a_fn.empty() && field().solved) {
        const Mesh& mesh = a_s.mesh();
        const std::vector<double>& v = field().values;
        const double unit = mesh.unitScale;
        auto corner = [&](int t, int c) { return mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(c)])] * unit; };
        auto potential = [&](int t, int c) { return v[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(c)])]; };
        auto insulator = [&](int t) { return t >= 0 && a_elemOn[std::size_t(t)] && !a_elemSemi[std::size_t(t)]; };
        // p's barycentric coordinates in t, their rates along d; t's area's root.
        auto coordinates = [&](int t, QPointF p, QPointF d, double l[3], double dl[3]) {
            const QPointF x[3] = {corner(t, 0), corner(t, 1), corner(t, 2)};
            const double det = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
            for (int c = 0; c < 3; ++c) {
                const QPointF& a = x[(c + 1) % 3];
                const QPointF& b = x[(c + 2) % 3];
                l[c] = ((b.x() - a.x()) * (p.y() - a.y()) - (b.y() - a.y()) * (p.x() - a.x())) / det;
                dl[c] = ((a.y() - b.y()) * d.x() + (b.x() - a.x()) * d.y()) / det;
            }
            return std::sqrt(std::abs(det) / 2);
        };
        // The triangle holding q, walked to from t (across the edge q is beyond).
        auto locate = [&](int t, QPointF q) {
            for (int walk = 0; walk < 2000 && t >= 0; ++walk) {
                double l[3], dl[3];
                coordinates(t, q, QPointF(), l, dl);
                int worst = 0;
                for (int c = 1; c < 3; ++c)
                    if (l[c] < l[worst]) worst = c;
                if (l[worst] >= -1e-12) return t;
                t = a_neighbors[std::size_t(t)][std::size_t((worst + 1) % 3)];
            }
            return -1;
        };
        auto barrierField = [&](const FowlerNordheim& fn, std::size_t k) {
            const QPointF ea = mesh.nodes[std::size_t(fn.edges[k][0])] * unit, eb = mesh.nodes[std::size_t(fn.edges[k][1])] * unit;
            const double h = std::hypot(eb.x() - ea.x(), eb.y() - ea.y());
            if (!(h > 0)) return 0.0;
            int t = fn.triangles[k];
            // Its normal toward the insulator's triangle.
            const QPointF m = (ea + eb) / 2.0, g = (corner(t, 0) + corner(t, 1) + corner(t, 2)) / 3.0;
            QPointF d(-(eb.y() - ea.y()) / h, (eb.x() - ea.x()) / h);
            if ((g.x() - m.x()) * d.x() + (g.y() - m.y()) * d.y() < 0) d = -d;
            const double V0 = 0.5 * (v[std::size_t(fn.edges[k][0])] + v[std::size_t(fn.edges[k][1])]);
            QPointF here = m;
            double s = 0, moved = 0;   // the way gone; the potential's change there
            for (int hop = 0; hop < 100000 && insulator(t); ++hop) {
                double l[3], dl[3];
                const double size = coordinates(t, here, d, l, dl);
                double change = -V0, rate = 0;   // linear in t along d
                for (int c = 0; c < 3; ++c) {
                    change += l[c] * potential(t, c);
                    rate += dl[c] * potential(t, c);
                }
                // Out of t where a falling coordinate reaches nought.
                double out = Inf;
                int side = -1;
                for (int c = 0; c < 3; ++c)
                    if (dl[c] < 0 && std::max(l[c], 0.0) / -dl[c] < out) {
                        out = std::max(l[c], 0.0) / -dl[c];
                        side = c;
                    }
                if (side < 0) break;
                if (out <= 1e-9 * size) {
                    // Leaving t at once (through a corner): the triangle just ahead.
                    t = locate(t, here + d * (1e-6 * size));
                    continue;
                }
                // |change + rate τ| reaching φ within t.
                if (rate != 0)
                    for (const double target : {fn.phi, -fn.phi}) {
                        const double tau = (target - change) / rate;
                        if (tau >= 0 && tau <= out) return fn.phi / (s + tau);
                    }
                moved = change + rate * out;
                s += out;
                here += d * out;
                t = a_neighbors[std::size_t(t)][std::size_t((side + 1) % 3)];
            }
            return s > 0 ? std::abs(moved) / s : 0.0;
        };
        double current = 0;
        for (const FowlerNordheim& fn : a_fn)
            for (std::size_t k = 0; k < fn.edges.size(); ++k) {
                const double E = barrierField(fn, k);
                if (E > 0) current += fn.A * E * E * std::exp(-fn.B / E) * fn.lengths[k];
            }
        a_s.setGlobal(p + QStringLiteral("Ifn"), current, Dim::current(),
                      tr("Fowler-Nordheim tunnelling's current through the insulators asked, at the mean field over each one's barrier (after the solve)"));
    }
    if (a_laser && field().solved) {
        const Lasing& L = *a_laser;
        const LaserState ls = laserState(stateVector());
        const double rate = 1 / L.tauP;
        a_s.setGlobal(p + QStringLiteral("S"), ls.S, Dim(), tr("photons in the lasing mode"));
        a_s.setGlobal(p + QStringLiteral("Pout"), L.ready ? L.eta * L.photon * ls.S * rate : 0.0, Dim::of(2, 1, -3),
                      tr("the laser's light out: η ħω S/τp"));
        a_s.setGlobal(p + QStringLiteral("G"), ls.G, Dim::of(0, 0, -1), tr("modal gain rate, Σ v g(N) Π (1/s)"));
        a_s.setGlobal(p + QStringLiteral("Gnet"), L.ready ? ls.G - rate : 0.0, Dim::of(0, 0, -1),
                      tr("net modal gain rate, G - 1/τp: nought at threshold, below it negative"));
        a_s.setGlobal(p + QStringLiteral("Gamma"), L.Gamma, Dim(), tr("confinement: the mode's share of its energy on the gain region"));
        const double v = L.ready && !L.speed.empty() ? L.speed.front() : 0.0;
        a_s.setGlobal(p + QStringLiteral("gth"), L.ready && L.Gamma > 0 && v > 0 ? rate / (v * L.Gamma) : 0.0, Dim::of(-1),
                      tr("the material gain at threshold, 1/(τp v Γ) (1/m: unit 1/cm)"));
        a_s.setGlobal(p + QStringLiteral("gmean"), ls.gmean, Dim::of(-1), tr("the material gain, the mode's mean over the gain region (1/m)"));
        a_s.setGlobal(p + QStringLiteral("Istim"), Q * ls.Rstim, Dim::current(), tr("stimulated recombination's current, q S G"));
        a_s.setGlobal(p + QStringLiteral("Isp"), Q * ls.Rsp, Dim::current(), tr("radiative recombination's current on the gain region"));
        a_s.setGlobal(p + QStringLiteral("Nact"), ls.Nact, Dim::of(-3), tr("the carriers N on the gain region, by volume"));
        // Carriers past its wells' gain table's end: its gain held at the
        // table's last there - said (it was not: the update check of
        // 110e8b9).
        if (L.ready && L.gain == 2 && L.table && !L.table->N.empty()) {
            const Eigen::VectorXd x = stateVector();
            double most = 0;
            for (const int i : L.nodes) {
                if (a_iN[std::size_t(i)] < 0) continue;
                const double n = std::max(x(a_iN[std::size_t(i)]), 0.0), pv = std::max(x(a_iP[std::size_t(i)]), 0.0);
                most = std::max(most, L.carrier == 1 ? n : L.carrier == 2 ? pv : std::sqrt(n * pv));
            }
            const QString first = tr("Warning: %1: its carriers reach").arg(L.name);
            if (most > L.table->N.back() * (1 + 1e-9)
                && std::none_of(a_s.log.begin(), a_s.log.end(), [&first](const QString& l) { return l.startsWith(first); }))
                a_s.log << tr("%1 %2 per cm³ on its gain region (first where it is solved past it), past its wells' gain table's %3 "
                              "(Nmax): its gain held at the table's last there - raise Nmax")
                               .arg(first, formatNumber(most * 1e-6, 3), formatNumber(L.table->N.back() * 1e-6, 3));
        }
        // (tauph: semi.taup is the holes' SRH lifetime, which this hid - the
        // update check of 110e8b9.)
        a_s.setGlobal(p + QStringLiteral("tauph"), L.tauP, Dim::of(0, 0, 1), tr("photon lifetime of the cold cavity, Q/ω"));
    }
    if (!a_stimulated.empty()) {
        const std::vector<double> rates = stimulatedRates();
        double current = 0;
        for (int i = 0; i < a_n; ++i) current += Q * a_recVolume[std::size_t(i)] * rates[std::size_t(i)];
        a_s.setGlobal(p + QStringLiteral("Istim"), current, Dim::current(),
                      tr("stimulated recombination's current: the light's, its photons' rate semi.Istim/q (by the norm length)"));
    }
    if (a_ionizationAny && field().solved) {
        ionizationIntegrals(stateVector());
        const Dim field = Dim::of(1, 1, -3, -1);
        a_s.setGlobal(p + QStringLiteral("In"), a_In, Dim(),
                      tr("electrons' ionization integral, the most of the field lines' through where it ionizes a thousandth of "
                         "its most or more (%1 lines), α of the field |E| whatever Impact Ionization's driving field: breakdown "
                         "where it reaches 1 - the field driving's (the others' as their currents say)")
                          .arg(a_ionLines));
        a_s.setGlobal(p + QStringLiteral("Ip"), a_Ip, Dim(), tr("holes' ionization integral, the most of those lines', α of |E|: breakdown where it reaches 1 - the field driving's"));
        a_s.setGlobal(p + QStringLiteral("Mn"), a_In < 1 ? 1 / (1 - a_In) : Inf, Dim(),
                      tr("multiplication of electrons entering In's line at its end, 1/(1 - In) (infinite: past breakdown)"));
        a_s.setGlobal(p + QStringLiteral("Mp"), a_Ip < 1 ? 1 / (1 - a_Ip) : Inf, Dim(),
                      tr("multiplication of holes entering Ip's line at its start, 1/(1 - Ip)"));
        a_s.setGlobal(p + QStringLiteral("Emax"), a_Emax, field, tr("the highest field where it ionizes"));
        a_s.setGlobal(p + QStringLiteral("Lion"), a_ionLength, Dim::length(), tr("In's field line's length"));
        a_s.setGlobal(p + QStringLiteral("xion"), a_ionX, Dim::length(), tr("x of In's field line's highest field: where it breaks down first"));
        a_s.setGlobal(p + QStringLiteral("yion"), a_ionY, Dim::length(), tr("y of In's field line's highest field"));
        // (Its pairs against those made otherwise: no line's - the
        // equations' own. With the field driving it, a doping gradient's
        // field at rest ionizes off any line's path: the Lumerical APD's
        // In 0.92 where its current ran away - its review.)
        if (a_ionization) {
            a_s.setGlobal(p + QStringLiteral("Iion"), a_ionCurrent, Dim::current(),
                          tr("current of impact ionization's pairs, q ∫ G: at breakdown the whole current"));
            a_s.setGlobal(p + QStringLiteral("Igen"), a_genCurrent, Dim::current(),
                          tr("current of the pairs made otherwise, q ∫ G of their generation: thermal, tunnelling, light, surfaces"));
            // (At rest - no current, both their rounding - 1. Where no pairs
            // are made otherwise, or no more than a rounding's of the current
            // - a resistor, a diode without recombination, one forward-biased
            // -, no multiplication to tell: not a number. Infinite there, and
            // 2.75e18 for a diode at 0.7 V, before the hunt of 19 October, B1.)
            bool still = true;
            double most = 0;
            for (const Contact& c : a_contacts) {
                still = still && c.current == 0;
                most = std::max(most, std::abs(c.current));
            }
            a_s.setGlobal(p + QStringLiteral("Mion"), still ? 1.0 : a_genCurrent > 1e-12 * most ? 1 + a_ionCurrent / a_genCurrent : NaN, Dim(),
                          tr("impact ionization's multiplication of the pairs made otherwise, 1 + Iion/Igen - of all of them: a "
                             "photocurrent's gain where its light's pairs all reach the field (less where some reach a contact or "
                             "recombine unmultiplied); rising without bound toward breakdown; not a number where no pairs are made "
                             "otherwise (Igen nought, or under 10⁻¹² of the current: no recombination model, a forward bias)"));
        }
    }
    for (const Contact& c : a_contacts) {
        a_s.setGlobal(p + QStringLiteral("I_") + c.name, c.current, Dim::current(), tr("current into the device at %1").arg(c.name));
        a_s.setGlobal(p + QStringLiteral("V_") + c.name, c.applied, Dim::voltage(),
                      c.iC >= 0 ? tr("its circuit's source's voltage V0 at %1").arg(c.name) : tr("voltage applied at %1").arg(c.name));
        if (c.iC >= 0) {
            // Behind a circuit: the contact's own voltage, and its load's
            // current, voltage and power (a bias tee's at rest none).
            const double IL = c.loadCurrent(a_inTime);
            a_s.setGlobal(p + QStringLiteral("Vc_") + c.name, c.voltage, Dim::voltage(),
                          tr("the contact %1's own voltage: its source's less its circuit's drop").arg(c.name));
            a_s.setGlobal(p + QStringLiteral("IL_") + c.name, IL, Dim::current(),
                          c.circuit == Contact::BiasTee ? tr("current through %1's load RL toward the device: in time the change from rest, through its bias tee").arg(c.name)
                                                        : tr("current through %1's load RL toward the device").arg(c.name));
            a_s.setGlobal(p + QStringLiteral("VL_") + c.name, c.RL * IL, Dim::voltage(), tr("voltage across %1's load RL, RL IL").arg(c.name));
            a_s.setGlobal(p + QStringLiteral("PL_") + c.name, c.RL * IL * IL, Dim::power(), tr("power into %1's load RL, RL IL²").arg(c.name));
        }
        // (An ohmic contact holds the field's flux at it alone: the
        // junction's charge is balanced inside the semiconductor. Said so -
        // "about nought" on an implant's contacts, 274 and -502 fC at every
        // bias, read as the junction's: the update check of a690d27.)
        a_s.setGlobal(p + QStringLiteral("Q_") + c.name, c.charge, Dim::charge(),
                      c.kind == Contact::Ohmic ? tr("charge on %1's metal, the field's flux at it - not the junction's charge, which is "
                                                    "balanced inside the semiconductor: nought on uniform neutral material, a constant "
                                                    "where the doping changes at the contact (an implant's slope); the junction's "
                                                    "capacitance is the small-signal %2")
                                                     .arg(c.name, matrixEntryName(field().tag, QStringLiteral("C"), c.name, c.name))
                                               : tr("charge on %1's metal").arg(c.name));
    }
    for (std::size_t a = 0; a < a_C0.size(); ++a)
        for (std::size_t b = 0; b < a_C0.size(); ++b) {
            const QString na = a_contacts[std::size_t(a_gates[a])].name, nb = a_contacts[std::size_t(a_gates[b])].name;
            a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("C0"), na, nb), a_C0[a][b], Dim::capacitance(),
                          tr("quasi-static capacitance at equilibrium (the carriers following: low frequency), %1 from %2").arg(na, nb));
        }
    if (!a_Y.empty() && a_frequency > 0) {
        const double omega = 2 * M_PI * a_frequency;
        for (std::size_t a = 0; a < a_contacts.size(); ++a)
            for (std::size_t b = 0; b < a_contacts.size(); ++b) {
                const QString na = a_contacts[a].name, nb = a_contacts[b].name;
                a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("G"), na, nb), a_Y[a][b].real(), Dim::conductance(),
                              tr("small-signal conductance, %1 from %2").arg(na, nb));
                a_s.setGlobal(matrixEntryName(field().tag, QStringLiteral("C"), na, nb), a_Y[a][b].imag() / omega, Dim::capacitance(),
                              tr("small-signal capacitance, %1 from %2").arg(na, nb));
            }
    }
    // Light: the pairs it makes, what they would carry each collected, the
    // power fed; modulated, each contact's share of it - the carriers'
    // transit, and with the load RL across the contact its RC too (the
    // device's own admittance Y beside RL: H RL⁻¹/(RL⁻¹ + Y)).
    if (!a_optical.empty() || a_Gop > 0) {
        a_s.setGlobal(p + QStringLiteral("Gop"), a_Gop, Dim::of(0, 0, -1),
                      tr("electron-hole pairs made per second - the light absorbed across the gap, and any generation given"));
        a_s.setGlobal(p + QStringLiteral("Iop"), Q * a_Gop, Dim::current(), tr("their current, every pair collected: q Gop"));
        a_s.setGlobal(p + QStringLiteral("Pabs"), a_Pabs, Dim::power(),
                      tr("the power the pairs are made with (W): a mode's photons absorbed across the gap, ħω over η each, and a "
                         "User-Defined Generation's at its photon energy or the gap - all of it heat in the end, the device's "
                         "∫ semi.H its contacts' I V and Pabs"));
    }
    if (!a_optical.empty())
        a_s.setGlobal(p + QStringLiteral("Pin"), a_Pin, Dim::power(),
                      tr("the light fed (W) - a responsivity |I|/Pin, an external quantum efficiency |I|/Pin hc/qλ"));
    if (!a_Hop.empty() && a_frequency > 0 && a_Gop > 0) {
        const double RL = a_optical.empty() ? 50.0 : a_optical.front().RL;
        for (std::size_t c = 0; c < a_contacts.size(); ++c) {
            const QString name = a_contacts[c].name;
            const std::complex<double> h = a_Hop[c] / (Q * a_Gop);
            a_s.setGlobal(p + QStringLiteral("Hop_") + name, std::abs(h), Dim(),
                          tr("small-signal photocurrent at %1 per pair made, the light modulated (1 at low frequency, every pair "
                             "collected): its transit's roll-off")
                              .arg(name));
            if (a_Hc.size() > c && a_contacts[c].iC >= 0)
                a_s.setGlobal(p + QStringLiteral("Hc_") + name, std::abs(a_Hc[c] / (Q * a_Gop)), Dim(),
                              tr("the small-signal photocurrent through %1's circuit's load per pair made: its RC and the device's, Cp "
                                 "across - a photodiode's bandwidth where it is 1/√2 of its low frequency's")
                                  .arg(name));
            if (RL > 0 && a_Y.size() > c) {
                const std::complex<double> yl = 1 / RL, loaded = h * yl / (yl + a_Y[c][c]);
                a_s.setGlobal(p + QStringLiteral("HopRL_") + name, std::abs(loaded), Dim(),
                              tr("the same through the load RL across %1 (the device's capacitance charged through it): a photodiode's "
                                 "bandwidth, where it is 1/√2 of its low frequency's")
                                  .arg(name));
            }
        }
    }
    // Noise at the frequency: each contact's short-circuit noise current,
    // its ratio to its current's full shot noise 2q|I|, through a circuit's
    // load; lit, its noise-equivalent power. (Per √Hz plain numbers: a
    // dimension's powers whole.)
    if (a_noiseC.size() == a_contacts.size() && a_frequency > 0) {
        const Dim density = Dim::current() * Dim::current() * Dim::of(0, 0, 1);
        for (std::size_t c = 0; c < a_contacts.size(); ++c) {
            const QString name = a_contacts[c].name;
            const double S = a_noiseC[c][c].real();
            // (In A²/Hz and A/√Hz: shown "s·A²" and as a plain number - the
            // hunt of 22 October, N7.)
            a_s.setGlobal(p + QStringLiteral("Sn_") + name, S, density,
                          tr("noise current spectral density at %1, short-circuited (A²/Hz, one-sided): the carriers' diffusion "
                             "(thermal at equilibrium), their pairs' generation and recombination (shot: the light's), impact "
                             "ionization's (an APD's excess)")
                              .arg(name),
                          QStringLiteral("A²/Hz"));
            a_s.setGlobal(p + QStringLiteral("in_") + name, std::sqrt(std::max(S, 0.0)), Dim(), tr("noise current at %1, A/√Hz").arg(name),
                          QStringLiteral("A/√Hz"));
            const double I = std::abs(a_contacts[c].current);
            if (I > 0)
                a_s.setGlobal(p + QStringLiteral("Fshot_") + name, S / (2 * Q * I), Dim(),
                              tr("its noise over its current's full shot noise, 2q|I|: 1 a photodiode's, M F an APD's (McIntyre's "
                                 "excess F), less where carriers recombine or diffuse back")
                                  .arg(name));
            if (a_noiseL.size() > c && std::isfinite(a_noiseL[c]))
                a_s.setGlobal(p + QStringLiteral("SnL_") + name, a_noiseL[c], density,
                              tr("the device's noise current through %1's circuit's load (A²/Hz; the circuit's own resistors' not in it)").arg(name),
                              QStringLiteral("A²/Hz"));
            if (a_Pin > 0 && a_Hop.size() > c && std::abs(a_Hop[c]) > 0)
                a_s.setGlobal(p + QStringLiteral("NEP_") + name, std::sqrt(std::max(S, 0.0)) / (std::abs(a_Hop[c]) / a_Pin), Dim(),
                              tr("noise-equivalent power at %1, W/√Hz: its noise current over its responsivity at the frequency").arg(name),
                              QStringLiteral("W/√Hz"));
        }
    }
}

} // namespace

std::unique_ptr<Problem> makeSemiconductorProblem(Solution& solution, int field)
{
    return std::make_unique<SemiconductorProblem>(solution, field);
}

SemiconductorState semiconductorAt(PointEvaluator& e, int f)
{
    SemiconductorState s;
    const Solution& sol = e.solution();
    const Field& field = sol.fields()[std::size_t(f)];
    const int t = e.triangle();
    if (t < 0 || field.extra.size() < std::size_t(ExtraHead + PerTriangle * (t + 1))) return s;
    const double* ex = &field.extra[std::size_t(ExtraHead + PerTriangle * t)];
    const double Vt = field.extra[0], niRef = field.extra[2];
    const int d = sol.mesh().domain[std::size_t(t)];
    if (d < 0 || !field.domains[std::size_t(d)]) return s;
    const Space& sp = *field.space;
    const int* dof = sp.dofs(t);
    const std::size_t N = std::size_t(sp.size());
    const double xi = e.xi(), eta = e.eta();
    const double w[3] = {1 - xi - eta, xi, eta};
    double V[3], n[3], p[3], thn[3], thp[3];
    for (int k = 0; k < 3; ++k) {
        V[k] = field.values[std::size_t(dof[k])];
        n[k] = field.values[N + std::size_t(dof[k])];
        p[k] = field.values[2 * N + std::size_t(dof[k])];
        const bool banded = field.values.size() >= 7 * N;
        thn[k] = banded ? field.values[5 * N + std::size_t(dof[k])] : 0.0;
        thp[k] = banded ? field.values[6 * N + std::size_t(dof[k])] : 0.0;
        // (A heterojunction's node of the other semiconductor: its carriers
        // on this one's bands.)
        if (std::isfinite(ex[0]) && (ex[26 + k] != 0 || ex[29 + k] != 0)) {
            thn[k] += ex[26 + k];
            thp[k] += ex[29 + k];
            n[k] *= std::exp(ex[26 + k] / Vt);
            p[k] *= std::exp(-ex[29 + k] / Vt);
        }
    }
    s.V = w[0] * V[0] + w[1] * V[1] + w[2] * V[2];
    // Solved along a line of its own (in 1D): V and the carriers its, read
    // at this point - not this mesh's nodes, up to some nm apart (the
    // update check of 21 October, 6).
    double along[5], slope[5];
    const bool alongLine = field.along && field.along->read(e.point(), along, slope);
    if (alongLine) s.V = along[0];
    s.defined = true;
    if (!std::isfinite(ex[0])) return s;   // an insulator
    s.semiconductor = true;
    s.Vt = Vt;
    // Carriers interpolated in their logarithm: they vary exponentially.
    double ln = 0, lp = 0;
    for (int k = 0; k < 3; ++k) {
        ln += w[k] * std::log(std::max(n[k], 1e-300));
        lp += w[k] * std::log(std::max(p[k], 1e-300));
    }
    s.n = std::exp(ln);
    s.p = std::exp(lp);
    if (alongLine && along[1] > 0 && along[2] > 0) {
        s.n = along[1];
        s.p = along[2];
    }
    s.mun = ex[0];
    s.mup = ex[1];
    s.dEc = ex[2];
    s.Eg = ex[3];
    s.ni = ex[4];
    s.delta = std::isfinite(ex[11]) ? ex[11] : 0.0;
    if (std::isfinite(ex[12]) && std::isfinite(ex[14])) {
        s.n1d = ex[12];
        s.gd = ex[13];
        s.p1a = ex[14];
        s.ga = ex[15];
    }
    // Fermi-Dirac's: γ = ℱ(η)/e^η, the carriers' Boltzmann densities n/γ.
    double gn = 1, gp = 1;
    s.joule = ex[18];
    s.heat = ex[33];
    s.lightHeat = std::isfinite(ex[34]) ? ex[34] : 0.0;
    s.Tn = std::isfinite(ex[35]) ? ex[35] : (field.extra.size() > 1 ? field.extra[1] : NaN);
    s.Tp = std::isfinite(ex[36]) ? ex[36] : (field.extra.size() > 1 ? field.extra[1] : NaN);
    s.Gion = std::isfinite(ex[32]) ? ex[32] : 0.0;
    if (ex[16] > 0 && ex[17] > 0) {
        const FermiHalf& fh = fermiHalf();
        auto gamma = [&](double c, double N) {
            const double u = c / N;
            return u > 1e-6 ? u / std::exp(fh.inverse(u)) : 1 - u / std::sqrt(8.0);
        };
        gn = gamma(s.n, ex[16]);
        gp = gamma(s.p, ex[17]);
    }
    s.nB = s.n / gn;
    s.pB = s.p / gp;
    // The quasi-Fermi potentials at the nodes (each its own bands'), linear
    // between them: φn = V + θn - Vt ln(n/ni_ref), φp = V + θp + Vt ln(p/ni_ref).
    s.phin = s.phip = 0;
    for (int k = 0; k < 3; ++k) {
        s.phin += w[k] * (V[k] + thn[k] - Vt * std::log(std::max(n[k], 1e-300) / niRef));
        s.phip += w[k] * (V[k] + thp[k] + Vt * std::log(std::max(p[k], 1e-300) / niRef));
    }
    // (Along the line: its own, with its V and carriers - from this mesh's
    // nodes φn was 0.389 V where the line's gave 0.059: the hunt of 22
    // October, A4.)
    if (alongLine && std::isfinite(along[3]) && std::isfinite(along[4])) {
        s.phin = along[3];
        s.phip = along[4];
    }
    double taun = ex[5], taup = ex[6];
    const double Et = ex[7], Cn = ex[8], Cp = ex[9], B = ex[10];
    const double excess = s.n * s.p - s.ni * s.ni * gn * gp;
    if (std::isfinite(taun) && std::isfinite(taup)) {
        const double kT = Vt * Q;
        const double n1 = s.ni * std::exp(Et / kT), p1 = s.ni * std::exp(-Et / kT);
        if (ex[22] > 0 || ex[23] > 0) {
            // (Trap-assisted tunnelling at the triangle's field.)
            const Mesh& tm = sol.mesh();
            QPointF y[3];
            for (int k = 0; k < 3; ++k) y[k] = tm.nodes[std::size_t(tm.triangles[std::size_t(t)][std::size_t(k)])] * tm.unitScale;
            const double area2 = (y[1].x() - y[0].x()) * (y[2].y() - y[0].y()) - (y[2].x() - y[0].x()) * (y[1].y() - y[0].y());
            double Ex = 0, Ey = 0;
            if (std::abs(area2) > 0)
                for (int k = 0; k < 3; ++k) {
                    const QPointF& a = y[(k + 1) % 3];
                    const QPointF& b = y[(k + 2) % 3];
                    Ex -= (a.y() - b.y()) / area2 * V[k];
                    Ey -= (b.x() - a.x()) / area2 * V[k];
                }
            // (Along the line: its field, A4.)
            if (alongLine) {
                Ex = field.along->alongY ? 0.0 : -slope[0];
                Ey = field.along->alongY ? -slope[0] : 0.0;
            }
            double g = 0, dg = 0;
            hurkx(std::hypot(Ex, Ey), ex[22] > 0 ? ex[22] : Inf, ex[24], &g, &dg);
            taun /= 1 + g;
            hurkx(std::hypot(Ex, Ey), ex[23] > 0 ? ex[23] : Inf, ex[25], &g, &dg);
            taup /= 1 + g;
        }
        s.R += excess / (taup * (s.n + gn * n1) + taun * (s.p + gp * p1));
        s.taun = taun;
        s.taup = taup;
    }
    s.R += (Cn * s.n + Cp * s.p) * excess + B * excess;
    // The currents: each edge's Scharfetter-Gummel current, the vector that
    // best agrees with the three (least squares).
    const Mesh& mesh = sol.mesh();
    QPointF x[3];
    for (int k = 0; k < 3; ++k) x[k] = mesh.nodes[std::size_t(mesh.triangles[std::size_t(t)][std::size_t(k)])] * mesh.unitScale;
    double a11 = 0, a12 = 0, a22 = 0, bn[2] = {0, 0}, bp[2] = {0, 0};
    for (int k = 0; k < 3; ++k) {
        const int i = k, j = (k + 1) % 3;
        const double hx = x[j].x() - x[i].x(), hy = x[j].y() - x[i].y(), h = std::hypot(hx, hy);
        if (!(h > 0)) continue;
        const double ux = hx / h, uy = hy / h;
        const double dn = (V[j] + thn[j] - V[i] - thn[i]) / Vt, dp = (V[j] + thp[j] - V[i] - thp[i]) / Vt;
        const double jn = Q * s.mun * Vt / h * (n[j] * bern(dn) - n[i] * bern(-dn));
        const double jp = Q * s.mup * Vt / h * (p[i] * bern(dp) - p[j] * bern(-dp));
        a11 += ux * ux;
        a12 += ux * uy;
        a22 += uy * uy;
        bn[0] += ux * jn;
        bn[1] += uy * jn;
        bp[0] += ux * jp;
        bp[1] += uy * jp;
    }
    const double det = a11 * a22 - a12 * a12;
    if (std::abs(det) > 1e-30) {
        s.Jn[0] = (a22 * bn[0] - a12 * bn[1]) / det;
        s.Jn[1] = (a11 * bn[1] - a12 * bn[0]) / det;
        s.Jp[0] = (a22 * bp[0] - a12 * bp[1]) / det;
        s.Jp[1] = (a11 * bp[1] - a12 * bp[0]) / det;
        // (A magnetic field's turn: (J0 + c ẑ×J0)/(1 + c²).)
        auto turn = [](double* J, double c) {
            if (!std::isfinite(c) || c == 0) return;
            const double x = (J[0] + c * J[1]) / (1 + c * c), y = (J[1] - c * J[0]) / (1 + c * c);
            J[0] = x;
            J[1] = y;
        };
        turn(s.Jn, ex[37]);
        turn(s.Jp, ex[38]);
    }
    // Band-to-band tunnelling's share of R at the triangle's field.
    if (ex[19] != 0) {
        const double area2 = (x[1].x() - x[0].x()) * (x[2].y() - x[0].y()) - (x[2].x() - x[0].x()) * (x[1].y() - x[0].y());
        if (std::abs(area2) > 0) {
            double Ex = 0, Ey = 0;
            for (int k = 0; k < 3; ++k) {
                const QPointF& a = x[(k + 1) % 3];
                const QPointF& b = x[(k + 2) % 3];
                Ex -= (a.y() - b.y()) / area2 * V[k];
                Ey -= (b.x() - a.x()) / area2 * V[k];
            }
            const double E = std::hypot(Ex, Ey);
            const double D = ex[19] < 0 ? -1.0 : (s.n * s.p - s.ni * s.ni) / ((s.n + s.ni) * (s.p + s.ni));
            if (E > 1e3) s.R += std::abs(ex[19]) * std::pow(E, ex[21]) * std::exp(-ex[20] / E) * D;
        }
    }
    return s;
}

} // namespace qucs_s::fem::detail

namespace qucs_s::fem {

void setSemiconductorJacobianFault(bool dropTunnellingField)
{
    detail::tunnellingFieldDropped.store(dropTunnellingField);
}

} // namespace qucs_s::fem
