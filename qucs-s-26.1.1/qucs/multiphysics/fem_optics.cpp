/*
 * fem_optics.cpp - light absorbed across a semiconductor's gap: a direct
 *                  gap's edge and the Franz-Keldysh effect on it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_optics.h"

#include <algorithm>
#include <cmath>

namespace qucs_s::fem {

namespace {

constexpr double Q = 1.602176634e-19;      // C
constexpr double Hbar = 1.054571817e-34;   // J s
constexpr double M0 = 9.1093837015e-31;    // kg

/// The asymptotic expansions' coefficients u_k (DLMF 9.7.2), v_k = -(6k+1)/(6k-1) u_k.
constexpr int Terms = 16;
struct Coefficients {
    double u[Terms], v[Terms];
    Coefficients()
    {
        u[0] = v[0] = 1;
        for (int k = 1; k < Terms; ++k) {
            u[k] = u[k - 1] * (6.0 * k - 5) * (6.0 * k - 3) * (6.0 * k - 1) / ((2.0 * k - 1) * 216.0 * k);
            v[k] = -(6.0 * k + 1) / (6.0 * k - 1) * u[k];
        }
    }
};
const Coefficients& coefficients()
{
    static const Coefficients c;
    return c;
}

} // namespace

void airy(double x, double* ai, double* aiPrime)
{
    // Where the power series' cancellation and the expansions' truncation
    // meet: both about 1e-7 relative (ζ = ⅔ x^3/2 about 8.6, e^ζ and e^-2ζ).
    constexpr double Above = 5.5, Below = -8.0;
    if (x > Above || x < Below) {
        const Coefficients& c = coefficients();
        const double z = std::abs(x), zeta = 2.0 / 3.0 * z * std::sqrt(z), q = std::sqrt(std::sqrt(z));
        const double rootPi = std::sqrt(M_PI);
        if (x > 0) {
            // Ai(x) ~ e^-ζ/(2√π x^¼) Σ (-1)^k u_k ζ^-k, Ai'(x) ~ -x^¼ e^-ζ/(2√π) Σ (-1)^k v_k ζ^-k
            // (each to its smallest term).
            double su = 0, sv = 0, p = 1, lastU = HUGE_VAL;
            for (int k = 0; k < Terms; ++k) {
                const double tu = c.u[k] * p, tv = c.v[k] * p;
                if (std::abs(tu) > lastU) break;
                lastU = std::abs(tu);
                su += tu;
                sv += tv;
                p *= -1 / zeta;
            }
            const double e = std::exp(-zeta) / (2 * rootPi);
            *ai = e / q * su;
            *aiPrime = -q * e * sv;
            return;
        }
        // Ai(-z) ~ (cos(ζ - π/4) Σ (-1)^k u_2k ζ^-2k + sin(ζ - π/4) Σ (-1)^k u_2k+1 ζ^-2k-1)/(√π z^¼),
        // Ai'(-z) ~ z^¼ (sin(ζ - π/4) Σ (-1)^k v_2k ζ^-2k - cos(ζ - π/4) Σ (-1)^k v_2k+1 ζ^-2k-1)/√π.
        double ue = 0, uo = 0, ve = 0, vo = 0, p = 1, sign = 1;
        for (int k = 0; 2 * k + 1 < Terms; ++k) {
            ue += sign * c.u[2 * k] * p;
            ve += sign * c.v[2 * k] * p;
            p /= zeta;
            uo += sign * c.u[2 * k + 1] * p;
            vo += sign * c.v[2 * k + 1] * p;
            p /= zeta;
            sign = -sign;
            if (std::abs(c.u[2 * k + 1] * p) < 1e-17) break;
        }
        const double co = std::cos(zeta - M_PI / 4), si = std::sin(zeta - M_PI / 4);
        *ai = (co * ue + si * uo) / (rootPi * q);
        *aiPrime = q * (si * ve - co * vo) / rootPi;
        return;
    }
    // Ai = c1 f - c2 g, Ai' = c1 f' - c2 g' (DLMF 9.4.1): f = Σ a_k x^3k,
    // a_k+1 = a_k / ((3k+2)(3k+3)); g = Σ b_k x^3k+1, b_k+1 = b_k / ((3k+3)(3k+4)).
    constexpr double c1 = 0.355028053887817239, c2 = 0.258819403792806798;
    const double x3 = x * x * x;
    double f = 1, g = x, fp = 0, gp = 1;
    double tf = 1, tg = x, tfp = x * x / 2, tgp = 1;
    fp = tfp;
    for (int k = 0; k < 200; ++k) {
        tf *= x3 / ((3.0 * k + 2) * (3.0 * k + 3));
        tg *= x3 / ((3.0 * k + 3) * (3.0 * k + 4));
        tgp *= x3 / ((3.0 * k + 1) * (3.0 * k + 3));
        f += tf;
        g += tg;
        gp += tgp;
        if (k > 0) {
            tfp *= x3 / (3.0 * k * (3.0 * k + 2));
            fp += tfp;
        }
        if (std::abs(tf) + std::abs(tg) + std::abs(tfp) + std::abs(tgp) < 1e-18 * (std::abs(f) + std::abs(g) + std::abs(fp) + std::abs(gp))) break;
    }
    *ai = c1 * f - c2 * g;
    *aiPrime = c1 * fp - c2 * gp;
}

double franzKeldysh(double photon, double gap, double field, double mr, double A, double sigma)
{
    if (!(photon > 0) || !(mr > 0) || !(A > 0)) return 0;
    if (sigma > 0) {
        const double eV = Q, F = std::abs(field);
        const double theta = F > 0 ? std::cbrt(Q * Q * Hbar * Hbar * F * F / (2 * mr * M0)) : 0;
        auto gauss = [&](double s) { return std::exp(-0.5 * (s / sigma) * (s / sigma)) / (sigma * std::sqrt(2 * M_PI)); };
        if (theta < 0.2 * sigma) {
            // Without a field (or one too weak to tell beside σ): α = A √((d - s)/1 eV) (1 eV/ħω)
            // over s < d = ħω - Eg - its √ kink smoothed in t = √((d - s)/1 eV):
            // (2 A eV²/ħω) ∫ t² g(d - t² eV) dt, Simpson's rule to ~6σ beyond.
            const double d = photon - gap, top = d + 6 * sigma;
            if (!(top > 0)) return 0;
            constexpr int Steps = 80;
            const double tmax = std::sqrt(top / eV), h = tmax / Steps;
            double sum = 0;
            for (int k = 0; k <= Steps; ++k) {
                const double t = k * h, weight = (k == 0 || k == Steps) ? 1 : (k % 2 ? 4 : 2);
                sum += weight * t * t * gauss(d - t * t * eV);
            }
            return 2 * A * eV * eV / photon * sum * h / 3;
        }
        // Under a field: smooth (Airy's) - Simpson's rule over ±5σ, its step
        // a quarter of σ and of ħθ at most.
        int steps = int(std::ceil(10 * sigma / std::min(0.25 * sigma, 0.25 * theta)));
        steps = std::clamp(steps + steps % 2, 40, 400);
        const double h = 10 * sigma / steps;
        double sum = 0;
        for (int k = 0; k <= steps; ++k) {
            const double sh = -5 * sigma + k * h, weight = (k == 0 || k == steps) ? 1 : (k % 2 ? 4 : 2);
            sum += weight * gauss(sh) * franzKeldysh(photon, gap + sh, field, mr, A, 0);
        }
        return sum * h / 3;
    }
    const double eV = Q;
    const double F = std::abs(field);
    // ħθ = (q² ħ² F² / 2µ)^⅓.
    const double theta = F > 0 ? std::cbrt(Q * Q * Hbar * Hbar * F * F / (2 * mr * M0)) : 0;
    if (!(theta > 1e-9 * eV)) return photon > gap ? A * std::sqrt((photon - gap) / eV) * eV / photon : 0.0;
    const double beta = (gap - photon) / theta;
    // (Far below the gap: nothing to speak of.)
    if (beta > 40) return 0;
    double ai = 0, aip = 0;
    airy(beta, &ai, &aip);
    const double shape = M_PI * (aip * aip - beta * ai * ai);
    return A * std::sqrt(theta / eV) * std::max(shape, 0.0) * eV / photon;
}

} // namespace qucs_s::fem
