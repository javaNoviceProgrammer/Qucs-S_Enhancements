/*
 * fem_semiconductor_models.h - the semiconductor solvers' models, shared by
 *                              the 2D solver (fem_semiconductor.cpp) and the
 *                              3D one (fem3_semiconductor.cpp): Fermi-Dirac's
 *                              integral of order 1/2, a material's values of
 *                              the lattice's temperature, saturation
 *                              velocities, Arora's, Dorkel and Leturcq's,
 *                              Klaassen's screening and Yamaguchi's
 *                              mobilities, van Overstraeten and de Man's
 *                              phonon factor, tunnelling through a spike,
 *                              Chynoweth's ionization rate and Hurkx's
 *                              trap-assisted tunnelling (not installed)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_SEMICONDUCTOR_MODELS_H
#define QUCS_FEM_SEMICONDUCTOR_MODELS_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace qucs_s::fem::detail {

namespace models {
inline constexpr double Q = 1.602176634e-19;   // C
inline constexpr double KB = 1.380649e-23;     // J/K
} // namespace models

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

inline const FermiHalf& fermiHalf()
{
    static const FermiHalf table;
    return table;
}

// A material's values of the lattice's temperature, tabulated from
// ThermalFrom by ThermalStep (K), cubic between (Catmull and Rom's), held
// at the ends.
inline constexpr double ThermalFrom = 150, ThermalStep = 5;
inline constexpr int ThermalPoints = 211;   // to 1200 K

inline double tabulated(const std::vector<double>& v, double T)
{
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
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
inline double quayVsat(double A, double T)
{
    return 1 / ((1 - A) + A * T / 300);
}

/// The saturation velocity of both carriers at \a T (K), m/s: 2.4e7/(1 +
/// 0.8 e^(T/600 K)) cm/s - the mobility overview's eq. 15, a device
/// simulator's default (1.035e7 cm/s at 300 K).
inline double expVsat(double T)
{
    return 2.4e5 / (1 + 0.8 * std::exp(T / 600));
}

/// Arora, Hauser and Roulston's mobility (1982) of \a c (µmin, a, µd, b,
/// Nref, c, α, d; SI) at \a N dopants (1/m³) and \a T (K): µmin Tn^a + µd
/// Tn^b/(1 + (N/(Nref Tn^c))^(α Tn^d)), Tn = T/300 K.
inline double aroraMobility(const double* c, double N, double T)
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
inline double dorkelMobility(const double* c, double N, double n, double p, double T)
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
inline double klaassenG(double P, double T, double m)
{
    return 1 - 0.89233 / std::pow(0.41372 + P * std::pow(T / (300 * m), 0.28227), 0.19778)
           + 0.005978 / std::pow(P * std::pow(300 * m / T, 0.72169), 1.80618);
}
inline double klaassenPmin(double T, double m)
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
inline double klaassenF(double P, double ratio)
{
    // (r5 -0.8552, device simulators' - Klaassen's paper's -0.01552.)
    const double q = std::pow(P, 0.6478);
    return (0.7643 * q + 2.2999 + 6.5505 * ratio) / (q + 2.3670 - 0.8552 * ratio);
}

/// Yamaguchi's low-field mobility (1979) of \a c (µL, S, Nref, as, uL, vs,
/// G; SI) at \a N dopants (1/m³): µL (1 + N/(N/S + Nref))^-½.
inline double yamaguchiLowField(const double* c, double N)
{
    return c[0] / std::sqrt(1 + N / (N / c[1] + c[2]));
}

/// Van Overstraeten and de Man's γ at \a T (K) for an optical phonon of
/// \a phonon (J; none: 1): tanh(ħω/2k 300 K)/tanh(ħω/2kT) - the carriers'
/// mean free path between phonons, λ ∝ tanh(ħω/2kT) (Crowell and Sze),
/// shorter as the lattice warms: α = γ a e^(-(γ b/E)^β).
inline double phononGamma(double phonon, double T)
{
    if (!(phonon > 0) || !(T > 0)) return 1;
    return std::tanh(phonon / (2 * models::KB * 300)) / std::tanh(phonon / (2 * models::KB * T));
}

/// What tunnels through a heterojunction's spike \a step (J) high under a
/// field \a F (V/m), against what is emitted over it: δ = (1/kT) ∫0^step
/// e^(u/kT) T(u) du, u the depth below its top, T(u) = e^(-4√(2m) u^(3/2) /
/// 3qħF) a triangle's WKB transmission (m its mass, kg) - the carriers
/// beneath its top through its tip (Grinberg et al., a heterojunction
/// bipolar transistor's emitter).
inline double tunnelling(double F, double step, double m, double kT)
{
    if (!(F > 0) || !(step > 0) || !(m > 0)) return 0;
    const double hbar = 1.054571817e-34, c = 4 * std::sqrt(2 * m) / (3 * models::Q * hbar * F);
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
inline void ionizationRate(double E, double a, double b, double a2, double b2, double E0, double beta, double* alpha, double* dalpha)
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
inline void hurkx(double F, double FG, double a, double* gamma, double* dgamma)
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


} // namespace qucs_s::fem::detail

#endif // QUCS_FEM_SEMICONDUCTOR_MODELS_H
