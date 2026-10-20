/*
 * fem_process_ions.cpp - ions in an amorphous target: their range's moments
 *                        from transport theory (the ZBL universal potential's
 *                        scattering, Lindhard and Scharff's electronic
 *                        stopping, Bragg's rule for compounds), Pearson's
 *                        distributions of them, the targets' compositions
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_process_p.h"

#include "fem_expr.h"
#include "fem_materials.h"
#include "fem_problem.h"

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>

namespace qucs_s::fem::process {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Process", text);
}

constexpr double A0 = 0.529177;     // Bohr's radius, Å
constexpr double E2 = 14.399645;    // e²/4πε0, eV·Å

// The elements: Z, symbol, the standard atomic weight (a target's), the
// mass of the most abundant isotope (an ion's: ¹¹B, ³¹P, ⁷⁵As, ¹²¹Sb...).
struct ElementRow {
    int Z;
    const char* symbol;
    double weight, isotope;
};
const ElementRow kElements[] = {
    {1, "H", 1.008, 1.00783},     {2, "He", 4.0026, 4.00260},   {3, "Li", 6.94, 7.01600},     {4, "Be", 9.0122, 9.01218},
    {5, "B", 10.81, 11.00931},    {6, "C", 12.011, 12.0},       {7, "N", 14.007, 14.00307},   {8, "O", 15.999, 15.99491},
    {9, "F", 18.998, 18.99840},   {10, "Ne", 20.180, 19.99244}, {11, "Na", 22.990, 22.98977}, {12, "Mg", 24.305, 23.98504},
    {13, "Al", 26.982, 26.98154}, {14, "Si", 28.085, 27.97693}, {15, "P", 30.974, 30.97376},  {16, "S", 32.06, 31.97207},
    {17, "Cl", 35.45, 34.96885},  {18, "Ar", 39.948, 39.96238}, {19, "K", 39.098, 38.96371},  {20, "Ca", 40.078, 39.96259},
    {22, "Ti", 47.867, 47.94795}, {23, "V", 50.942, 50.94396},  {24, "Cr", 51.996, 51.94051}, {25, "Mn", 54.938, 54.93805},
    {26, "Fe", 55.845, 55.93494}, {27, "Co", 58.933, 58.93320}, {28, "Ni", 58.693, 57.93535}, {29, "Cu", 63.546, 62.92960},
    {30, "Zn", 65.38, 63.92914},  {31, "Ga", 69.723, 68.92558}, {32, "Ge", 72.630, 73.92118}, {33, "As", 74.922, 74.92160},
    {34, "Se", 78.971, 79.91652}, {35, "Br", 79.904, 78.91834}, {36, "Kr", 83.798, 83.91151}, {38, "Sr", 87.62, 87.90561},
    {39, "Y", 88.906, 88.90585},  {40, "Zr", 91.224, 89.90470}, {41, "Nb", 92.906, 92.90638}, {42, "Mo", 95.95, 97.90541},
    {47, "Ag", 107.87, 106.90509}, {48, "Cd", 112.41, 113.90336}, {49, "In", 114.82, 114.90388}, {50, "Sn", 118.71, 119.90220},
    {51, "Sb", 121.76, 120.90382}, {52, "Te", 127.60, 129.90622}, {53, "I", 126.90, 126.90447}, {54, "Xe", 131.29, 131.90416},
    {56, "Ba", 137.33, 137.90525}, {57, "La", 138.91, 138.90636}, {68, "Er", 167.26, 165.93030}, {72, "Hf", 178.49, 179.94656},
    {73, "Ta", 180.95, 180.94800}, {74, "W", 183.84, 183.95093}, {78, "Pt", 195.08, 194.96479}, {79, "Au", 196.97, 196.96657},
};

const ElementRow* row(int Z)
{
    for (const ElementRow& e : kElements)
        if (e.Z == Z) return &e;
    return nullptr;
}

const ElementRow* row(const QString& symbol)
{
    for (const ElementRow& e : kElements)
        if (symbol.compare(QLatin1String(e.symbol), Qt::CaseInsensitive) == 0) return &e;
    return nullptr;
}

// ------------------------------------------------- The ZBL universal potential

/// Its screening function (Ziegler, Biersack and Littmark, The Stopping and
/// Range of Ions in Solids, 1985): φ(x), x in screening lengths.
double phi(double x)
{
    return 0.18175 * std::exp(-3.1998 * x) + 0.50986 * std::exp(-0.94229 * x) + 0.28022 * std::exp(-0.4029 * x)
           + 0.028171 * std::exp(-0.20162 * x);
}

/// The universal screening length (Å) of a pair.
double screening(int Z1, int Z2)
{
    return 0.8854 * A0 / (std::pow(double(Z1), 0.23) + std::pow(double(Z2), 0.23));
}

/*!
 * The scattering angle in the centre of mass, θ(ε, b), of the universal
 * potential - every pair's, in reduced units: ε its energy in Z1 Z2 e²/a,
 * b the impact parameter in a. From the classical integral θ = π - 2 b ∫
 * dr / (r² √(1 - V/Ec - b²/r²)), the closest approach r0 its root, by
 * Gauss-Mehler quadrature (as Biersack and Haggmark's TRIM checked its
 * fit against).
 */
double scatteringAngle(double eps, double b)
{
    auto g = [eps, b](double r) { return r * r - r * phi(r) / eps - b * b; };
    double lo = std::max(b, 1e-12), hi = 0.5 * (1 / eps + std::sqrt(1 / (eps * eps) + 4 * b * b)) + 1e-12;
    if (g(lo) >= 0) hi = lo;
    else {
        for (int k = 0; k < 200 && hi - lo > 1e-14 * hi; ++k) {
            const double m = 0.5 * (lo + hi);
            (g(m) < 0 ? lo : hi) = m;
        }
    }
    const double r0 = hi;
    constexpr int N = 48;
    double sum = 0;
    for (int j = 1; j <= N / 2; ++j) {
        const double u = std::cos((2 * j - 1) * M_PI / (2 * N));
        const double r = r0 / u;
        const double F = 1 - phi(r) / (r * eps) - (b / r) * (b / r);
        const double G = F / (1 - u * u);
        if (G > 0) sum += 1 / std::sqrt(G);
    }
    const double integral = M_PI / N * sum;   // ∫0^1 du / √F(r0/u)
    return std::clamp(M_PI - 2 * b / r0 * integral, 0.0, M_PI);
}

/// θ on a grid of ln ε and ln b, made once.
struct AngleTable {
    static constexpr int NE = 241, NB = 241;
    static constexpr double lnE0 = -13.815510557964274, lnE1 = 9.210340371976184;   // 1e-6 .. 1e4
    static constexpr double lnB0 = -9.210340371976184, lnB1 = 4.0943445622221;      // 1e-4 .. 60
    std::vector<double> theta;   // [e * NB + b]
    double dE() const { return (lnE1 - lnE0) / (NE - 1); }
    double dB() const { return (lnB1 - lnB0) / (NB - 1); }
    double b(int k) const { return std::exp(lnB0 + k * dB()); }
};

const AngleTable& angles()
{
    static AngleTable table;
    static std::once_flag once;
    std::call_once(once, [] {
        table.theta.resize(std::size_t(AngleTable::NE) * AngleTable::NB);
        for (int e = 0; e < AngleTable::NE; ++e) {
            const double eps = std::exp(AngleTable::lnE0 + e * table.dE());
            for (int k = 0; k < AngleTable::NB; ++k) table.theta[std::size_t(e) * AngleTable::NB + std::size_t(k)] = scatteringAngle(eps, table.b(k));
        }
    });
    return table;
}

double legendre(int l, double x)
{
    switch (l) {
    case 0: return 1;
    case 1: return x;
    case 2: return (3 * x * x - 1) / 2;
    case 3: return (5 * x * x * x - 3 * x) / 2;
    default: return (35 * x * x * x * x - 30 * x * x + 3) / 8;
    }
}

/// A pair's collisions at an energy, one for each impact parameter of the
/// table (b on a logarithmic grid): its weight in the cross-section (Å²,
/// 2π b db), the energy given the atom T (eV), the cosine of the ion's
/// deflection in the laboratory.
struct Collision {
    double w, T, cphi;
};

void collisions(int Z1, double M1, int Z2, double M2, double E, double share, std::vector<Collision>* out)
{
    const AngleTable& t = angles();
    const double a = screening(Z1, Z2);
    const double eps = a * E * M2 / ((M1 + M2) * Z1 * Z2 * E2);
    const double gamma = 4 * M1 * M2 / ((M1 + M2) * (M1 + M2));
    const double A = M2 / M1;
    double le = (std::log(eps) - AngleTable::lnE0) / t.dE();
    le = std::clamp(le, 0.0, double(AngleTable::NE - 1) - 1e-9);
    const int e0 = int(le);
    const double fe = le - e0;
    const double h = t.dB();
    for (int k = 0; k < AngleTable::NB; ++k) {
        const double th = (1 - fe) * t.theta[std::size_t(e0) * AngleTable::NB + std::size_t(k)]
                          + fe * t.theta[std::size_t(e0 + 1) * AngleTable::NB + std::size_t(k)];
        const double s = std::sin(th / 2);
        const double ct = std::cos(th);
        const double den = std::sqrt(std::max(1 + A * A + 2 * A * ct, 0.0));
        const double b = t.b(k) * a;
        // ∫ 2π b db f = ∫ 2π b² f d(ln b): the trapezoid on ln b.
        const double w = share * 2 * M_PI * b * b * h * ((k == 0 || k == AngleTable::NB - 1) ? 0.5 : 1.0);
        out->push_back({w, gamma * E * s * s, den > 1e-12 ? std::clamp((1 + A * ct) / den, -1.0, 1.0) : 0.0});
    }
}

/// Lindhard and Scharff's electronic stopping, per atom (eV·Å²), E in eV,
/// M1 in u: 1.212 Z1^(7/6) Z2 √E / ((Z1^(2/3) + Z2^(2/3))^(3/2) √M1) - LSS's
/// k ε^(1/2) in absolute units.
double electronic(int Z1, double M1, int Z2, double E)
{
    const double s = std::pow(double(Z1), 2.0 / 3) + std::pow(double(Z2), 2.0 / 3);
    return 1.212 * std::pow(double(Z1), 7.0 / 6) * Z2 * std::sqrt(E) / (std::pow(s, 1.5) * std::sqrt(M1));
}

} // namespace

// ----------------------------------------------------------------- Elements

const Element* element(int Z)
{
    static std::map<int, Element> made;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (auto it = made.find(Z); it != made.end()) return &it->second;
    const ElementRow* r = row(Z);
    if (!r) return nullptr;
    return &made.emplace(Z, Element{r->Z, r->weight, r->isotope, QLatin1String(r->symbol)}).first->second;
}

const Element* element(const QString& symbol)
{
    const ElementRow* r = row(symbol.trimmed());
    return r ? element(r->Z) : nullptr;
}

// ----------------------------------------------------------- Range moments

/*!
 * The range's moments by transport theory (Lindhard, Scharff and Schiøtt's
 * integral equations; Winterbon's and Biersack's moments of them): the
 * n-th moment of the depth an ion of energy E stops at, along a direction
 * at cos η to its own, F_n(E, η) = Σ_l F_nl(E) P_l(η), satisfies
 *
 *   N ∫ dσ [F_nl(E - T) P_l(cos φ) - F_nl(E)] - N Se F_nl' = -n Q_nl,
 *   Q_nl = l/(2l-1) F_(n-1)(l-1) + (l+1)/(2l+3) F_(n-1)(l+1),
 *
 * the integral over the nuclear collisions (T the energy given, φ the
 * ion's deflection; the ZBL potential's, exact), Se the electronic stopping
 * (Bragg's rule: a compound's atoms' weighted by their shares). Collisions
 * giving less than a tenth of E to second order in T (F(E - T) ≈ F - T F' +
 * T²F''/2), the others exactly - F below E already known: solved upward in
 * energy from 1 eV, the trapezoid on a logarithmic grid, F'' from the last
 * pass (three passes). (To second order throughout, a heavy ion's skewness
 * came out twice a Monte Carlo's of the same collisions.) Along the beam
 * (η = 1): Rp = F11, ⟨x²⟩ = F20 + F22..., across it ⟨y²⟩ = F20 - F22/2 and
 * ⟨x y²⟩ = F31/3 - F33/2. The energy that goes into nuclear collisions (ν,
 * eV) and where it goes: the same equations, a collision's T its source
 * (local deposition: a recoil's energy where it was struck).
 */
RangeMoments rangeMoments(int Z1, double M1, const Target& target, double energy, QString* error, double electronicScale)
{
    RangeMoments out;
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        out.ok = false;
        return out;
    };
    if (!(energy > 0)) return fail(tr("an energy of %1 eV").arg(energy));
    if (target.parts.empty() || !(target.atoms > 0)) return fail(tr("a target of nothing"));
    const double N = target.atoms * 1e-30;   // Å⁻³
    // The energy grid: 1 eV to E, logarithmic, 80 points a decade.
    const double e0 = std::min(1.0, energy / 10);
    const int n = std::max(200, int(std::ceil(std::log10(energy / e0) * 80)));
    const double dlog = std::log(energy / e0) / (n - 1);
    std::vector<double> E(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) E[std::size_t(i)] = e0 * std::exp(i * dlog);
    // At each energy: the small collisions' sums, the large ones as they are.
    struct Large {
        double w, T, P[5];
    };
    struct At {
        double sigma[5], J[5], K[5], Se, Sn, large;   // large: Σ w of the large
        std::vector<Large> big;
    };
    std::vector<At> at(static_cast<std::size_t>(n));
    std::vector<Collision> c;
    for (int i = 0; i < n; ++i) {
        At& a = at[std::size_t(i)];
        const double e = E[std::size_t(i)];
        std::fill(std::begin(a.sigma), std::end(a.sigma), 0.0);
        std::fill(std::begin(a.J), std::end(a.J), 0.0);
        std::fill(std::begin(a.K), std::end(a.K), 0.0);
        a.Se = a.Sn = a.large = 0;
        c.clear();
        for (const Target::Part& p : target.parts) {
            collisions(Z1, M1, p.Z, p.M, e, p.share, &c);
            a.Se += p.share * electronicScale * electronic(Z1, M1, p.Z, e);
        }
        const double cut = 0.1 * e;
        for (const Collision& k : c) {
            a.Sn += k.w * k.T;
            double P[5];
            for (int l = 0; l <= 4; ++l) P[l] = legendre(l, k.cphi);
            if (k.T < cut) {
                for (int l = 0; l <= 4; ++l) {
                    a.sigma[l] += k.w * (1 - P[l]);
                    a.J[l] += k.w * k.T * P[l];
                    a.K[l] += k.w * k.T * k.T * P[l];
                }
            } else {
                a.large += k.w;
                a.big.push_back({k.w, k.T, {P[0], P[1], P[2], P[3], P[4]}});
            }
        }
    }
    // The moments, (n, l): F[n][l] on the grid.
    using Grid = std::vector<double>;
    std::array<std::array<Grid, 5>, 5> F, D;   // ranges; the nuclear energy's
    for (auto& r : F)
        for (Grid& g : r) g.assign(std::size_t(n), 0.0);
    for (auto& r : D)
        for (Grid& g : r) g.assign(std::size_t(n), 0.0);
    std::fill(F[0][0].begin(), F[0][0].end(), 1.0);
    auto second = [&E, n](const Grid& f, int i) {
        if (i <= 0 || i >= n - 1) return 0.0;
        const double h0 = E[std::size_t(i)] - E[std::size_t(i - 1)], h1 = E[std::size_t(i + 1)] - E[std::size_t(i)];
        return 2 * (h0 * f[std::size_t(i + 1)] - (h0 + h1) * f[std::size_t(i)] + h1 * f[std::size_t(i - 1)]) / (h0 * h1 * (h0 + h1));
    };
    // f at an energy below the grid's point i (found already): linear on
    // the logarithmic grid, nought below its first.
    auto below = [e0, dlog](const Grid& f, double e) {
        if (e <= e0) return 0.0;
        const double u = std::log(e / e0) / dlog;
        const std::size_t k = std::size_t(u);
        if (k + 1 >= f.size()) return f.back();
        const double t = u - double(k);
        return (1 - t) * f[k] + t * f[k + 1];
    };
    // One moment: (J< + Se) f' + (σ< + Σw>) f = src + Σ> w P_l f(E - T) +
    // ½ K< f'' (f'' lagged).
    auto solve = [&](int l, const std::function<double(int)>& source, Grid& f) {
        for (int pass = 0; pass < 3; ++pass) {
            Grid fpp(std::size_t(n), 0.0);
            if (pass > 0)
                for (int i = 0; i < n; ++i) fpp[std::size_t(i)] = second(f, i);
            auto exact = [&](int i) {
                double s = 0;
                for (const Large& b : at[std::size_t(i)].big) s += b.w * b.P[l] * below(f, E[std::size_t(i)] - b.T);
                return s;
            };
            auto forcing = [&](int i) { return source(i) + exact(i) + 0.5 * at[std::size_t(i)].K[l] * fpp[std::size_t(i)]; };
            f[0] = 0;
            double now = forcing(0);
            for (int i = 0; i + 1 < n; ++i) {
                // The trapezoid, implicit in the decay term.
                const double h = E[std::size_t(i + 1)] - E[std::size_t(i)];
                const At& a = at[std::size_t(i)];
                const At& b = at[std::size_t(i + 1)];
                const double slope = (now - (a.sigma[l] + a.large) * f[std::size_t(i)]) / (a.J[l] + a.Se);
                const double next = forcing(i + 1);   // (of f below E[i + 1] alone)
                const double c1 = 1 / (b.J[l] + b.Se);
                f[std::size_t(i + 1)] = (f[std::size_t(i)] + 0.5 * h * slope + 0.5 * h * c1 * next) / (1 + 0.5 * h * c1 * (b.sigma[l] + b.large));
                now = next;
            }
        }
    };
    auto Q = [](const std::array<std::array<Grid, 5>, 5>& G, int nn, int l, int i) {
        double q = 0;
        if (l >= 1) q += double(l) / (2 * l - 1) * G[std::size_t(nn - 1)][std::size_t(l - 1)][std::size_t(i)];
        if (l + 1 <= 4) q += double(l + 1) / (2 * l + 3) * G[std::size_t(nn - 1)][std::size_t(l + 1)][std::size_t(i)];
        return q;
    };
    for (int nn = 1; nn <= 4; ++nn)
        for (int l = nn % 2; l <= nn; l += 2) {
            auto src = [&Q, &F, nn, l, N](int i) { return nn * Q(F, nn, l, i) / N; };
            solve(l, src, F[std::size_t(nn)][std::size_t(l)]);
        }
    // The nuclear energy: ν0 from every collision's T (the small ones' J<,
    // the large ones' given each), then its depth's first two moments.
    solve(0, [&at](int i) { return at[std::size_t(i)].Sn; }, D[0][0]);
    for (int nn = 1; nn <= 2; ++nn)
        for (int l = nn % 2; l <= nn; l += 2) {
            auto src = [&Q, &D, nn, l, N](int i) { return nn * Q(D, nn, l, i) / N; };
            solve(l, src, D[std::size_t(nn)][std::size_t(l)]);
        }
    const std::size_t last = std::size_t(n - 1);
    const double m1 = F[1][1][last];
    const double m2 = F[2][0][last] + F[2][2][last];
    const double m3 = F[3][1][last] + F[3][3][last];
    const double m4 = F[4][0][last] + F[4][2][last] + F[4][4][last];
    const double var = m2 - m1 * m1;
    if (!(m1 > 0) || !(var > 0)) return fail(tr("its range came out as nought (an energy of %1 eV)").arg(energy));
    const double sd = std::sqrt(var);
    const double mu3 = m3 - 3 * m1 * m2 + 2 * m1 * m1 * m1;
    const double mu4 = m4 - 4 * m1 * m3 + 6 * m1 * m1 * m2 - 3 * m1 * m1 * m1 * m1;
    const double y2 = F[2][0][last] - F[2][2][last] / 2;
    const double xy2 = F[3][1][last] / 3 - F[3][3][last] / 2;
    out.Rp = m1 * 1e-10;
    out.dRp = sd * 1e-10;
    out.gamma = mu3 / (var * sd);
    out.beta = mu4 / (var * var);
    out.lateral = std::sqrt(std::max(y2, 0.0)) * 1e-10;
    out.lateralSlope = (xy2 - m1 * y2) / var * 1e-10;
    const double nu = D[0][0][last];
    out.nuclear = nu;
    if (nu > 0) {
        const double d1 = D[1][1][last] / nu, d2 = (D[2][0][last] + D[2][2][last]) / nu;
        out.damageMean = d1 * 1e-10;
        out.damageStraggle = std::sqrt(std::max(d2 - d1 * d1, 0.0)) * 1e-10;
    }
    // The path's length: ∫ dE / S.
    double path = 0;
    for (int i = 0; i + 1 < n; ++i)
        path += 0.5 * (E[std::size_t(i + 1)] - E[std::size_t(i)])
                * (1 / (at[std::size_t(i)].Sn + at[std::size_t(i)].Se) + 1 / (at[std::size_t(i + 1)].Sn + at[std::size_t(i + 1)].Se));
    out.pathLength = path / N * 1e-10;
    out.ok = true;
    return out;
}

// ------------------------------------------------------------- Targets

namespace {

/// A formula's elements, by count: {{"Si", 1}, {"O", 2}}.
using Formula = std::vector<std::pair<const char*, double>>;

struct KnownTarget {
    const char* names;   ///< '|'-separated, lowercase: any of them in the name
    Formula formula;
    double density;      ///< g/cm³
};

/// Materials as targets by name, where they have no compound: their
/// formulas and typical densities (thermal oxide's 2.27, LPCVD nitride's
/// 3.1; a resist a novolac's C8H8O at 1.2 - cresol and formaldehyde's
/// repeat; BCB (Cyclotene) DVS-bis-BCB's C24H30OSi2 at 1.05).
const KnownTarget kKnown[] = {
    {"silicon dioxide|sio2|fused silica|quartz|silica|glass|teos|psg|bsg", {{"Si", 1}, {"O", 2}}, 2.27},
    {"titanium nitride|tin", {{"Ti", 1}, {"N", 1}}, 5.22},
    {"silicon nitride|si3n4|sinx|nitride", {{"Si", 3}, {"N", 4}}, 3.1},
    {"hafnium oxide|hfo2", {{"Hf", 1}, {"O", 2}}, 9.68},
    {"zirconium oxide|zro2", {{"Zr", 1}, {"O", 2}}, 5.68},
    {"titanium oxide|tio2", {{"Ti", 1}, {"O", 2}}, 4.23},
    {"aluminium oxide|aluminum oxide|alumina|sapphire|al2o3", {{"Al", 2}, {"O", 3}}, 3.95},
    {"lithium niobate|linbo3", {{"Li", 1}, {"Nb", 1}, {"O", 3}}, 4.64},
    {"lithium tantalate|litao3", {{"Li", 1}, {"Ta", 1}, {"O", 3}}, 7.46},
    {"indium tin oxide|ito", {{"In", 2}, {"O", 3}}, 7.14},
    {"silicon carbide|sic", {{"Si", 1}, {"C", 1}}, 3.21},
    {"germanium", {{"Ge", 1}}, 5.323},
    {"polysilicon|poly-si|amorphous silicon|silicon", {{"Si", 1}}, 2.329},
    {"photoresist|resist|novolac", {{"C", 8}, {"H", 8}, {"O", 1}}, 1.2},
    {"pmma", {{"C", 5}, {"H", 8}, {"O", 2}}, 1.18},
    {"bcb|benzocyclobutene|cyclotene", {{"C", 24}, {"H", 30}, {"O", 1}, {"Si", 2}}, 1.05},
    {"polyimide|kapton", {{"C", 22}, {"H", 10}, {"N", 2}, {"O", 5}}, 1.42},
    {"copper", {{"Cu", 1}}, 8.96},
    {"aluminium|aluminum", {{"Al", 1}}, 2.70},
    {"gold", {{"Au", 1}}, 19.3},
    {"silver", {{"Ag", 1}}, 10.49},
    {"titanium", {{"Ti", 1}}, 4.51},
    {"tungsten", {{"W", 1}}, 19.25},
    {"platinum", {{"Pt", 1}}, 21.45},
    {"nickel", {{"Ni", 1}}, 8.91},
    {"chromium", {{"Cr", 1}}, 7.19},
};

constexpr double U = 1.66053907e-27;   // kg
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

QString formulaText(const std::vector<Target::Part>& parts, double scale)
{
    QStringList out;
    for (const Target::Part& p : parts) {
        const double n = p.share * scale;
        const QString sym = element(p.Z)->symbol;
        out << (std::abs(n - 1) < 1e-6 ? sym : sym + formatNumber(n, 3));
    }
    return out.join(QLatin1Char(' '));
}

} // namespace

std::optional<Target> targetOf(const QString& material, const Model& model, const ParameterScope& ps, QString* why)
{
    auto fail = [&](const QString& w) -> std::optional<Target> {
        if (why) *why = w;
        return std::nullopt;
    };
    const QString name = material.trimmed();
    if (name.isEmpty()) return fail(tr("no material"));
    // Its compound and density: the model's material of that name (or of
    // that library entry), else the library's entry.
    QString compound, rho;
    double x = 0, y = 0;
    bool found = false;
    for (const Node& m : model.materials().children)
        if (m.name().compare(name, Qt::CaseInsensitive) == 0 || m.text(QStringLiteral("library")).compare(name, Qt::CaseInsensitive) == 0) {
            compound = detail::Problem::materialProperty(m, QStringLiteral("compound"), &ps.scope).trimmed();
            x = evaluateConstant(detail::Problem::materialProperty(m, QStringLiteral("x"), &ps.scope), ps.scope, Dim()).value_or(0);
            y = evaluateConstant(detail::Problem::materialProperty(m, QStringLiteral("y"), &ps.scope), ps.scope, Dim()).value_or(0);
            rho = detail::Problem::materialProperty(m, QStringLiteral("rho"), &ps.scope);
            found = true;
            break;
        }
    if (!found)
        if (const std::optional<MaterialEntry> e = findMaterial(name)) {
            compound = e->properties.value(QStringLiteral("compound")).toString().trimmed();
            x = evaluateConstant(e->properties.value(QStringLiteral("x")).toString(), ps.scope, Dim()).value_or(0);
            y = evaluateConstant(e->properties.value(QStringLiteral("y")).toString(), ps.scope, Dim()).value_or(0);
            rho = e->properties.value(QStringLiteral("rho")).toString();
        }
    const double density = rho.trimmed().isEmpty() ? NaN : evaluateConstant(rho, ps.scope, Dim::of(-3, 1)).value_or(NaN);   // kg/m³
    Target t;
    double tableDensity = NaN;
    if (!compound.isEmpty()) {
        const Sublattices s = sublatticesOf(compound, x, y);
        if (!s.ok) return fail(tr("%1: its compound %2 is not one an implant knows the atoms of").arg(name, compound));
        const double half = s.anion.empty() ? 1.0 : 0.5;
        for (const auto& [Z, f] : s.cation) t.parts.push_back({Z, element(Z)->weight, half * f});
        for (const auto& [Z, f] : s.anion) t.parts.push_back({Z, element(Z)->weight, half * f});
        // (Its atoms from its lattice where no density is given: eight to
        // a zinc blende's cube.)
        if (!std::isfinite(density))
            if (const std::optional<BandParameters> b = bandParameters(compound, x, y, 300); b && b->a0 > 0) t.atoms = 8 / (b->a0 * b->a0 * b->a0);
        t.formula = compound + (s.cation.size() > 1 || s.anion.size() > 1 ? QStringLiteral(" (x %1, y %2)").arg(formatNumber(x, 3), formatNumber(y, 3)) : QString());
    } else {
        const QString lower = name.toLower();
        const KnownTarget* k = nullptr;
        for (const KnownTarget& c : kKnown) {
            // (A short name - SiO2, ITO, BCB - the whole name or its first
            // word, not within another's.)
            for (const QString& n : QString::fromLatin1(c.names).split(QLatin1Char('|')))
                if (n.size() <= 4 ? lower == n || lower.startsWith(n + QLatin1Char(' ')) : lower.contains(n)) k = &c;
            if (k) break;
        }
        if (!k)
            return fail(tr("%1: what it is made of is not known - give its material a compound (GaAs, InGaAs...), or name it as a known one "
                           "(Silicon, Silicon dioxide, Silicon nitride, Photoresist, a metal...)")
                            .arg(name));
        double count = 0;
        for (const auto& [sym, n] : k->formula) count += n;
        for (const auto& [sym, n] : k->formula) t.parts.push_back({element(QLatin1String(sym))->Z, element(QLatin1String(sym))->weight, n / count});
        tableDensity = k->density * 1e3;
        t.formula = formulaText(t.parts, count);
    }
    if (t.atoms <= 0) {
        const double d = std::isfinite(density) && density > 0 ? density : tableDensity;
        if (!(d > 0)) return fail(tr("%1: its density is not known (give it rho)").arg(name));
        double mean = 0;
        for (const Target::Part& p : t.parts) mean += p.share * p.M;
        t.atoms = d / (mean * U);
    }
    return t;
}

int hostOf(const Target& t)
{
    auto group = [](int Z) {
        switch (Z) {
        case 5: case 13: case 31: case 49: return 3;
        case 6: case 14: case 32: case 50: return 4;
        case 7: case 15: case 33: case 51: return 5;
        case 30: case 48: case 80: return 2;
        case 8: case 16: case 34: case 52: return 6;
        default: return 0;
        }
    };
    double g[7] = {0, 0, 0, 0, 0, 0, 0};
    for (const Target::Part& p : t.parts) g[group(p.Z)] += p.share;
    if (g[4] > 0.999 && !(t.parts.size() == 2 && t.parts[0].Z == 14 && t.parts[1].Z == 6)) return 4;
    if (g[3] + g[5] > 0.999 && g[3] > 0.4 && g[5] > 0.4) return 35;
    if (g[2] + g[6] > 0.999 && g[2] > 0.4 && g[6] > 0.4) return 26;
    return 0;
}

// -------------------------------------------------------------- Implants

std::optional<Ion> ionOf(const QString& species)
{
    const QString s = species.trimmed();
    if (s.compare(QLatin1String("BF2"), Qt::CaseInsensitive) == 0) {
        // A molecule: it breaks up at the surface, its boron taking its share
        // of the energy by mass (11/49).
        const Element* b = element(5);
        const Element* f = element(9);
        return Ion{QStringLiteral("B"), 5, b->isotope, b->isotope / (b->isotope + 2 * f->isotope)};
    }
    if (const Element* e = element(s)) return Ion{e->symbol, e->Z, e->isotope, 1.0};
    return std::nullopt;
}

namespace {

/// Projected ranges in silicon read off the curves of Plummer, Deal and
/// Griffin's EE212 handout 14 (1999; LSS tables'), every 10 keV: Rp and ΔRp
/// in µm. (Their antimony curve, deeper than arsenic's below 140 keV, is
/// not taken: transport theory and a Monte Carlo of the same collisions
/// put it 25 to 30 % shallower - the theory's then.)
struct RangeTable {
    const char* ion;
    double e0, step;   // keV
    std::vector<double> Rp, dRp;
};
const RangeTable kSilicon[] = {
    {"B", 10, 10,
     {0.0512, 0.0856, 0.1178, 0.1453, 0.1738, 0.1992, 0.2245, 0.2497, 0.2727, 0.2986, 0.3215, 0.3442, 0.3661, 0.3880},
     {0.0256, 0.0392, 0.0494, 0.0566, 0.0633, 0.0692, 0.0739, 0.0785, 0.0829, 0.0865, 0.0901, 0.0931, 0.0954, 0.0984}},
    {"P", 10, 10,
     {0.0220, 0.0356, 0.0506, 0.0620, 0.0738, 0.0856, 0.0973, 0.1075, 0.1189, 0.1294, 0.1395, 0.1497, 0.1606, 0.1716, 0.1817, 0.1918,
      0.2008, 0.2098, 0.2198, 0.2277},
     {0.0069, 0.0127, 0.0184, 0.0235, 0.0281, 0.0320, 0.0361, 0.0397, 0.0435, 0.0466, 0.0494, 0.0523, 0.0552, 0.0580, 0.0607, 0.0633,
      0.0658, 0.0684, 0.0704, 0.0721}},
    // (At 70 keV the curve was read across a label: the mean of 60 and 80.)
    {"As", 10, 10,
     {0.0096, 0.0164, 0.0233, 0.0297, 0.0364, 0.0433, 0.0509, 0.0584, 0.0649, 0.0718, 0.0777, 0.0837, 0.0905, 0.0971, 0.1040, 0.1090,
      0.1156, 0.1221, 0.1276, 0.1315},
     {0.0047, 0.0082, 0.0104, 0.0129, 0.0155, 0.0178, 0.0201, 0.0224, 0.0246, 0.0266, 0.0284, 0.0302, 0.0321, 0.0339, 0.0360, 0.0381,
      0.0398, 0.0417, 0.0431, 0.0446}},
};

/// Lindhard and Scharff's electronic stopping times this for the ions
/// tabled: fitted so that the theory's Rp in silicon follows the table
/// (rms 2.5 % boron, 5 % phosphorus, 3.6 % arsenic) - what the ion loses
/// to the electrons LSS's k ε^(1/2) leaves short.
double electronicScaleOf(int Z)
{
    switch (Z) {
    case 5: return 1.6;
    case 15: return 1.4;
    case 33: return 1.1;
    default: return 1.0;
    }
}

/// The table's value at E (keV): linear in log E between its points; false
/// (the nearest end's) outside.
bool tableAt(const RangeTable& t, double keV, double* Rp, double* dRp)
{
    const int n = int(t.Rp.size());
    const double u = (keV - t.e0) / t.step;
    if (u < 0 || u > n - 1) {
        const int k = u < 0 ? 0 : n - 1;
        *Rp = t.Rp[std::size_t(k)];
        *dRp = t.dRp[std::size_t(k)];
        return false;
    }
    const int k = std::min(int(u), n - 2);
    const double e1 = t.e0 + k * t.step, e2 = e1 + t.step;
    const double w = std::log(keV / e1) / std::log(e2 / e1);
    *Rp = std::exp((1 - w) * std::log(t.Rp[std::size_t(k)]) + w * std::log(t.Rp[std::size_t(k + 1)]));
    *dRp = std::exp((1 - w) * std::log(t.dRp[std::size_t(k)]) + w * std::log(t.dRp[std::size_t(k + 1)]));
    return true;
}

Target silicon()
{
    Target t;
    t.parts.push_back({14, element(14)->weight, 1.0});
    t.atoms = 2.329e3 / (element(14)->weight * U);
    t.formula = QStringLiteral("Si");
    return t;
}

} // namespace

std::optional<ImplantMoments> implantMoments(const Ion& ion, double energy, const Target& target, QString* why)
{
    // (Made once for each ion, energy and target: a flow is run again at
    // every build.)
    static std::mutex m;
    static std::map<QString, ImplantMoments> made;
    const QString key = QStringLiteral("%1 %2 %3 %4 %5").arg(ion.Z).arg(ion.M, 0, 'g', 10).arg(energy, 0, 'g', 12).arg(target.atoms, 0, 'g', 10).arg(target.formula);
    {
        std::lock_guard<std::mutex> lock(m);
        if (auto it = made.find(key); it != made.end()) return it->second;
    }
    const double scale = electronicScaleOf(ion.Z);
    QString error;
    const RangeMoments th = rangeMoments(ion.Z, ion.M, target, energy, &error, scale);
    if (!th.ok) {
        if (why) *why = error;
        return std::nullopt;
    }
    ImplantMoments out;
    out.Rp = th.Rp;
    out.dRp = th.dRp;
    out.gamma = th.gamma;
    out.beta = th.beta;
    out.lateral = th.lateral;
    out.lateralSlope = th.lateralSlope;
    out.nuclear = th.nuclear;
    out.damageMean = th.damageMean;
    out.damageStraggle = th.damageStraggle;
    out.source = scale != 1 ? tr("transport theory, its electronic stopping fitted to the tables") : tr("transport theory (ZBL, Lindhard and Scharff)");
    // A table's ion: its Rp and ΔRp in silicon the table's; in another
    // target the theory's scaled as it is in silicon.
    for (const RangeTable& t : kSilicon)
        if (ion.symbol() == QLatin1String(t.ion)) {
            double Rp = 0, dRp = 0;
            const bool inside = tableAt(t, energy / 1e3, &Rp, &dRp);
            const RangeMoments si = target.formula == QLatin1String("Si") ? th : rangeMoments(ion.Z, ion.M, silicon(), energy, nullptr, scale);
            if (si.ok && si.Rp > 0 && si.dRp > 0) {
                const double sR = Rp * 1e-6 / si.Rp, sD = dRp * 1e-6 / si.dRp;
                out.Rp *= sR;
                out.dRp *= sD;
                out.lateral *= sD;
                out.lateralSlope *= sD;
                out.damageMean *= sR;
                out.damageStraggle *= sD;
                out.source = target.formula == QLatin1String("Si")
                                 ? tr("Plummer et al.'s LSS tables")
                                 : tr("transport theory, scaled as its silicon's is to Plummer et al.'s LSS tables");
            }
            if (!inside) {
                out.extrapolated = true;
                out.note = tr("%1 at %2 keV is outside its table (%3 to %4 keV): extrapolated")
                               .arg(t.ion)
                               .arg(formatNumber(energy / 1e3, 4))
                               .arg(formatNumber(t.e0, 4))
                               .arg(formatNumber(t.e0 + t.step * (t.Rp.size() - 1), 4));
            }
            break;
        }
    // Lindhard and Scharff's stopping is for ions slower than v0 Z1^(2/3):
    // past it (a light ion fast), the range is too short.
    const double perNucleon = energy / 1e3 / ion.M;
    if (perNucleon > 25 * std::pow(double(ion.Z), 4.0 / 3)) {
        out.extrapolated = true;
        out.note = tr("%1 at %2 keV is faster than Lindhard and Scharff's stopping holds for (v0 Z1^(2/3)): its range comes out short - give "
                      "its moments (SRIM's)")
                       .arg(ion.symbol())
                       .arg(formatNumber(energy / 1e3, 4));
    }
    std::lock_guard<std::mutex> lock(m);
    made[key] = out;
    return out;
}

// ------------------------------------------------------------ Compounds

Sublattices sublatticesOf(const QString& compound, double x, double y)
{
    Sublattices s;
    const QString c = compound.trimmed();
    auto is = [&c](const char* name) { return c.compare(QLatin1String(name), Qt::CaseInsensitive) == 0; };
    auto parts = [](std::initializer_list<std::pair<const char*, double>> list) {
        std::vector<std::pair<int, double>> out;
        for (const auto& [symbol, share] : list)
            if (share > 0) out.emplace_back(element(QLatin1String(symbol))->Z, share);
        return out;
    };
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    // Binaries: their two symbols.
    static const char* const binaries[][3] = {{"GaAs", "Ga", "As"}, {"AlAs", "Al", "As"}, {"InAs", "In", "As"}, {"InP", "In", "P"},
                                              {"GaP", "Ga", "P"},   {"AlP", "Al", "P"},   {"GaSb", "Ga", "Sb"}, {"AlSb", "Al", "Sb"},
                                              {"InSb", "In", "Sb"}, {"GaN", "Ga", "N"},   {"AlN", "Al", "N"},   {"InN", "In", "N"},
                                              {"ZnSe", "Zn", "Se"}, {"ZnS", "Zn", "S"},   {"CdTe", "Cd", "Te"}, {"CdS", "Cd", "S"},
                                              {"CdSe", "Cd", "Se"}, {"ZnTe", "Zn", "Te"}, {"SiC", "Si", "C"}};
    for (const auto& b : binaries)
        if (is(b[0])) {
            s.cation = parts({{b[1], 1.0}});
            s.anion = parts({{b[2], 1.0}});
            s.ok = true;
            return s;
        }
    if (is("Si")) s.cation = parts({{"Si", 1.0}});
    else if (is("Ge")) s.cation = parts({{"Ge", 1.0}});
    else if (is("SiGe")) s.cation = parts({{"Si", 1 - x}, {"Ge", x}});
    else if (is("AlGaAs")) { s.cation = parts({{"Al", x}, {"Ga", 1 - x}}); s.anion = parts({{"As", 1.0}}); }
    else if (is("InGaAs")) { s.cation = parts({{"In", x}, {"Ga", 1 - x}}); s.anion = parts({{"As", 1.0}}); }
    else if (is("InAlAs")) { s.cation = parts({{"In", x}, {"Al", 1 - x}}); s.anion = parts({{"As", 1.0}}); }
    else if (is("GaInP")) { s.cation = parts({{"Ga", x}, {"In", 1 - x}}); s.anion = parts({{"P", 1.0}}); }
    else if (is("GaAsP")) { s.cation = parts({{"Ga", 1.0}}); s.anion = parts({{"As", x}, {"P", 1 - x}}); }
    else if (is("InAsP")) { s.cation = parts({{"In", 1.0}}); s.anion = parts({{"As", x}, {"P", 1 - x}}); }
    else if (is("GaAsSb")) { s.cation = parts({{"Ga", 1.0}}); s.anion = parts({{"As", x}, {"Sb", 1 - x}}); }
    else if (is("InGaAsP")) { s.cation = parts({{"In", 1 - x}, {"Ga", x}}); s.anion = parts({{"As", y}, {"P", 1 - y}}); }
    else if (is("AlGaInAs")) { s.cation = parts({{"Al", x}, {"Ga", y}, {"In", std::max(0.0, 1 - x - y)}}); s.anion = parts({{"As", 1.0}}); }
    else if (is("AlGaInP")) { s.cation = parts({{"Al", x}, {"Ga", y}, {"In", std::max(0.0, 1 - x - y)}}); s.anion = parts({{"P", 1.0}}); }
    else if (is("AlGaN")) { s.cation = parts({{"Al", x}, {"Ga", 1 - x}}); s.anion = parts({{"N", 1.0}}); }
    else if (is("InGaN")) { s.cation = parts({{"In", x}, {"Ga", 1 - x}}); s.anion = parts({{"N", 1.0}}); }
    else if (is("InAlN")) { s.cation = parts({{"In", x}, {"Al", 1 - x}}); s.anion = parts({{"N", 1.0}}); }
    else if (is("AlGaInN")) { s.cation = parts({{"Al", x}, {"Ga", y}, {"In", std::max(0.0, 1 - x - y)}}); s.anion = parts({{"N", 1.0}}); }
    else return s;
    s.ok = true;
    return s;
}

bool compoundOf(const Sublattices& s, QString* compound, double* x, double* y)
{
    auto share = [](const std::vector<std::pair<int, double>>& side, const char* symbol) {
        const int Z = element(QLatin1String(symbol))->Z;
        double v = 0;
        for (const auto& [z, f] : side)
            if (z == Z) v += f;
        return v;
    };
    auto only = [](const std::vector<std::pair<int, double>>& side, std::initializer_list<const char*> allowed) {
        for (const auto& [z, f] : side) {
            if (f < 1e-9) continue;
            bool in = false;
            for (const char* a : allowed) in = in || element(QLatin1String(a))->Z == z;
            if (!in) return false;
        }
        return true;
    };
    const double sumC = share(s.cation, "Al") + share(s.cation, "Ga") + share(s.cation, "In");
    auto norm = [sumC](double v) { return sumC > 0 ? v / sumC : 0.0; };
    if (only(s.anion, {"As"}) && only(s.cation, {"Al", "Ga", "In"})) {
        *compound = QStringLiteral("AlGaInAs");
        *x = norm(share(s.cation, "Al"));
        *y = norm(share(s.cation, "Ga"));
        return true;
    }
    if (only(s.anion, {"P"}) && only(s.cation, {"Al", "Ga", "In"})) {
        *compound = QStringLiteral("AlGaInP");
        *x = norm(share(s.cation, "Al"));
        *y = norm(share(s.cation, "Ga"));
        return true;
    }
    if (only(s.anion, {"As", "P"}) && only(s.cation, {"Ga", "In"})) {
        const double sumA = share(s.anion, "As") + share(s.anion, "P");
        *compound = QStringLiteral("InGaAsP");
        *x = norm(share(s.cation, "Ga"));
        *y = sumA > 0 ? share(s.anion, "As") / sumA : 0;
        return true;
    }
    if (only(s.anion, {"As", "Sb"}) && only(s.cation, {"Ga"})) {
        const double sumA = share(s.anion, "As") + share(s.anion, "Sb");
        *compound = QStringLiteral("GaAsSb");
        *x = sumA > 0 ? share(s.anion, "As") / sumA : 0;
        *y = 0;
        return true;
    }
    if (only(s.anion, {"N"}) && only(s.cation, {"Al", "Ga", "In"})) {
        *compound = QStringLiteral("AlGaInN");
        *x = norm(share(s.cation, "Al"));
        *y = norm(share(s.cation, "Ga"));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- Pearson

bool Pearson::typeIV(double gamma, double beta)
{
    const double b1 = gamma * gamma;
    const double d1 = 4 * beta - 3 * b1, d2 = 2 * beta - 3 * b1 - 6;
    if (!(d1 > 0) || !(d2 > 0)) return false;
    const double kappa = b1 * (beta + 3) * (beta + 3) / (4 * d1 * d2);
    return kappa >= 0 && kappa < 1;
}

double Pearson::leastBeta(double gamma)
{
    // The type IV region's edge (κ = 1, type V) at this skewness: its β2
    // from above, by bisection.
    const double b1 = gamma * gamma;
    double lo = 3 + 1.5 * b1, hi = lo + 50;
    if (b1 == 0) return 3;
    for (int k = 0; k < 200; ++k) {
        const double m = 0.5 * (lo + hi);
        (typeIV(gamma, m) ? hi : lo) = m;
    }
    return hi;
}

Pearson Pearson::make(double mean, double sd, double gamma, double beta)
{
    Pearson p;
    p.a_mean = mean;
    p.a_sd = sd;
    p.a_gamma = gamma;
    p.a_beta = beta;
    // Pearson's equation, about the mean (s = z - Rp): f'/f = (s - a)/(b0 +
    // b1 s + b2 s²) - from the four moments (A = 10β - 12γ² - 18):
    // a = b1 = -γσ(β + 3)/A, b0 = -σ²(4β - 3γ²)/A, b2 = -(2β - 3γ² - 6)/A.
    const double A = 10 * beta - 12 * gamma * gamma - 18;
    const double a = -gamma * sd * (beta + 3) / A;
    const double b0 = -sd * sd * (4 * beta - 3 * gamma * gamma) / A;
    const double b2 = -(2 * beta - 3 * gamma * gamma - 6) / A;
    auto slope = [a, b0, b2](double s) {
        const double q = b0 + a * s + b2 * s * s;
        return q != 0 ? (s - a) / q : 0.0;
    };
    // Its support: out from the mode (f' = 0 at s = a) each way until it
    // falls below 1e-15 of its peak (its tails a power's, slow: 12σ left a
    // skewed one's kurtosis 1e-3 short), at most 300σ.
    auto reach = [&](double dir) {
        const double h = 0.02 * sd * dir;
        double s = a, lnf = 0;
        while (lnf > -34.5 && std::abs(s - a) < 300 * sd) {
            lnf += h / 6 * (slope(s) + 4 * slope(s + h / 2) + slope(s + h));
            s += h;
        }
        return s;
    };
    const double lo = std::min(reach(-1), -6 * sd), hi = std::max(reach(1), 6 * sd);
    const int N = 40001;
    p.a_z.resize(N);
    p.a_pdf.resize(N);
    p.a_cdf.resize(N);
    const double h = (hi - lo) / (N - 1);
    // From the mode outward (f' = 0 at s = a), Simpson's rule on each step.
    std::vector<double> lnf(N, 0.0);
    int m = std::clamp(int(std::lround((a - lo) / h)), 0, N - 1);
    for (int i = m + 1; i < N; ++i) {
        const double s0 = lo + (i - 1) * h, s1 = lo + i * h;
        lnf[std::size_t(i)] = lnf[std::size_t(i - 1)] + h / 6 * (slope(s0) + 4 * slope(0.5 * (s0 + s1)) + slope(s1));
    }
    for (int i = m - 1; i >= 0; --i) {
        const double s0 = lo + (i + 1) * h, s1 = lo + i * h;
        lnf[std::size_t(i)] = lnf[std::size_t(i + 1)] - h / 6 * (slope(s0) + 4 * slope(0.5 * (s0 + s1)) + slope(s1));
    }
    double total = 0;
    for (int i = 0; i < N; ++i) {
        p.a_z[std::size_t(i)] = mean + lo + i * h;
        p.a_pdf[std::size_t(i)] = std::exp(lnf[std::size_t(i)]);
    }
    for (int i = 0; i + 1 < N; ++i) total += 0.5 * h * (p.a_pdf[std::size_t(i)] + p.a_pdf[std::size_t(i + 1)]);
    p.a_cdf[0] = 0;
    for (int i = 0; i < N; ++i) {
        p.a_pdf[std::size_t(i)] /= total;
        if (i > 0) p.a_cdf[std::size_t(i)] = p.a_cdf[std::size_t(i - 1)] + 0.5 * h * (p.a_pdf[std::size_t(i - 1)] + p.a_pdf[std::size_t(i)]);
    }
    p.a_cdf.back() = 1;
    return p;
}

Pearson Pearson::gaussian(double mean, double sd)
{
    return make(mean, sd, 0, 3);
}

double Pearson::pdf(double z) const
{
    if (a_z.empty() || z <= a_z.front() || z >= a_z.back()) return 0;
    const double h = a_z[1] - a_z[0];
    const double f = (z - a_z.front()) / h;
    const std::size_t i = std::min(std::size_t(f), a_z.size() - 2);
    const double t = f - double(i);
    return (1 - t) * a_pdf[i] + t * a_pdf[i + 1];
}

double Pearson::cdf(double z) const
{
    if (a_z.empty()) return 0;
    if (z <= a_z.front()) return 0;
    if (z >= a_z.back()) return 1;
    const double h = a_z[1] - a_z[0];
    const double f = (z - a_z.front()) / h;
    const std::size_t i = std::min(std::size_t(f), a_z.size() - 2);
    const double t = f - double(i);
    // (The trapezoid's own: the pdf linear between the points.)
    return a_cdf[i] + h * t * (a_pdf[i] + 0.5 * t * (a_pdf[i + 1] - a_pdf[i]));
}

double Pearson::depthPassed(double fraction) const
{
    // Where the cdf reaches 1 - fraction (the fraction still moving).
    const double target = 1 - fraction;
    if (a_z.empty()) return 0;
    if (target <= 0) return a_z.front();
    if (target >= 1) return a_z.back();
    auto it = std::lower_bound(a_cdf.begin(), a_cdf.end(), target);
    std::size_t i = std::size_t(std::max<std::ptrdiff_t>(1, it - a_cdf.begin()));
    double lo = a_z[i - 1], hi = a_z[i];
    for (int k = 0; k < 60; ++k) {
        const double m = 0.5 * (lo + hi);
        (cdf(m) < target ? lo : hi) = m;
    }
    return 0.5 * (lo + hi);
}

std::array<double, 4> Pearson::momentsOfTable() const
{
    // Its own: Rp, σ, γ, β of the table, by the trapezoid.
    double m0 = 0, m1 = 0;
    const double h = a_z.size() > 1 ? a_z[1] - a_z[0] : 0;
    for (std::size_t i = 0; i < a_z.size(); ++i) {
        const double w = (i == 0 || i + 1 == a_z.size()) ? 0.5 * h : h;
        m0 += w * a_pdf[i];
        m1 += w * a_pdf[i] * a_z[i];
    }
    const double mean = m1 / m0;
    double c2 = 0, c3 = 0, c4 = 0;
    for (std::size_t i = 0; i < a_z.size(); ++i) {
        const double w = (i == 0 || i + 1 == a_z.size()) ? 0.5 * h : h;
        const double s = a_z[i] - mean;
        c2 += w * a_pdf[i] * s * s;
        c3 += w * a_pdf[i] * s * s * s;
        c4 += w * a_pdf[i] * s * s * s * s;
    }
    c2 /= m0;
    c3 /= m0;
    c4 /= m0;
    return {mean, std::sqrt(c2), c3 / std::pow(c2, 1.5), c4 / (c2 * c2)};
}

} // namespace qucs_s::fem::process
