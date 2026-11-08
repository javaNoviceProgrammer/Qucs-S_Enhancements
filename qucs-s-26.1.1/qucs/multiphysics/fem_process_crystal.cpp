/*
 * fem_process_crystal.cpp - an implant into a crystal by binary collisions:
 *                           a Monte Carlo after MARLOWE's approach (the ion
 *                           from one lattice atom to the next, each
 *                           collision the ZBL potential's, tabulated), the
 *                           electronic stopping Cai et al.'s - the crystal's
 *                           own electron density along the ion's path -
 *                           with Firsov's loss in each collision; an
 *                           amorphous cover (a native oxide, films, a
 *                           mask's resist) by TRIM's random collisions
 *
 * The model is the one checked against Cai et al.'s phosphorus SIMS in the
 * review of 8 November 2026 (qucs-reviews, ImplantProcessFlow; the write-up
 * docs/feature_gaps/2026-11-08-implant-crystal-monte-carlo.md):
 *
 *   dE/dx = [Z1 γ(v, rs0)]² v F(rs(x))     (Cai, Grønbech-Jensen, Snell and
 *                                           Beardmore, PRB 54, 17147, 1996)
 *   F(rs): Echenique, Nieminen and Ritchie's friction with the G(rs) term;
 *   γ: Brandt and Kitagawa's effective charge, its one parameter rs0 =
 *   1.109 Å (theirs for boron and arsenic; their phosphorus value, fitted to
 *   the SIMS compared with, is not taken);
 *   rs(x): silicon's electron density - a solid-state LDA atom inside the
 *   muffin-tin radius, one electron per atom as a uniform background
 *   (data/si_density_cai.txt, a resource; no solver at run time);
 *   Firsov's loss in each collision, 4.3e-8 (Z1 + Z2)^(5/3) v / (1 + 0.31
 *   (Z1 + Z2)^(1/3) R0)^5 eV (v cm/s, R0 the closest approach in Å).
 *
 * Lengths in Å and energies in eV inside; the crystal in its own cubic
 * axes. Each ion its own random numbers (from the seed and its number), its
 * end kept by its number and summed in order: the result the same for any
 * number of threads.
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

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <thread>

namespace qucs_s::fem::process {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Process", text);
}

constexpr double PI = 3.14159265358979323846;
constexpr double E2 = 14.399645;          // e²/4πε0, eV Å
constexpr double BOHR = 0.529177210903;   // Å
constexpr double HARTREE = 27.211386;     // eV
constexpr double AMU_ME = 1822.888486;    // electron masses in a u
constexpr double RS0 = 1.109;             // Å: the effective charge's parameter
constexpr double EMIN = 10;               // eV: stopped below
constexpr double PMAX = 2.4;              // Å: the farthest lattice atom an ion is deflected by
constexpr double SEARCH = 4.0;            // Å: how far ahead it looks for one
constexpr double U1_300K = 0.075;         // Å: silicon's thermal vibration at 300 K (each axis)
constexpr double THETA_DEBYE = 543;       // K: silicon's, for the vibration's change with temperature

struct V3 {
    double x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(double s, V3 a) { return {s * a.x, s * a.y, s * a.z}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 normalize(V3 a) { return (1.0 / std::sqrt(dot(a, a))) * a; }

// ------------------------------------------------------- Random numbers

/// xoshiro256** (Blackman and Vigna), seeded by splitmix64: each ion's own
/// stream, the same on every platform (the standard library's
/// distributions are not).
class Random
{
public:
    Random(unsigned long long seed, unsigned long long stream)
    {
        unsigned long long x = seed * 0x9E3779B97F4A7C15ull ^ (stream + 0x632BE59BD9B4E019ull);
        for (auto& v : s) v = splitmix(x);
    }
    double uniform()   // [0, 1)
    {
        return double(next() >> 11) * (1.0 / 9007199254740992.0);
    }
    double normal(double sd)
    {
        if (hasSpare) {
            hasSpare = false;
            return spare * sd;
        }
        double u = 0, v = 0, r = 0;
        do {
            u = 2 * uniform() - 1;
            v = 2 * uniform() - 1;
            r = u * u + v * v;
        } while (r >= 1 || r == 0);
        const double f = std::sqrt(-2 * std::log(r) / r);
        spare = v * f;
        hasSpare = true;
        return u * f * sd;
    }

private:
    static unsigned long long splitmix(unsigned long long& x)
    {
        unsigned long long z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    static unsigned long long rotl(unsigned long long x, int k) { return (x << k) | (x >> (64 - k)); }
    unsigned long long next()
    {
        const unsigned long long result = rotl(s[1] * 5, 7) * 9;
        const unsigned long long t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
    unsigned long long s[4];
    double spare = 0;
    bool hasSpare = false;
};

// ------------------------------------------------- The scattering table

/*!
 * θ, τ and the closest approach (universalScattering) over log10 ε from -6
 * to 5 (every 0.02) and b from 0 to 40 screening lengths (every 0.01) -
 * one table for every pair, the potential universal in these units. Made
 * once, on the machine's threads (about a second).
 */
struct ScatteringTable {
    static constexpr int NE = 551, NB = 4001;
    static constexpr double LE0 = -6, DLE = 0.02, DB = 0.01;
    struct Entry {
        float theta, tau, r0;
    };
    std::vector<Entry> e;   // [i * NB + j]
};

const ScatteringTable& scatteringTable()
{
    static ScatteringTable t;
    static std::once_flag once;
    std::call_once(once, [] {
        t.e.resize(std::size_t(ScatteringTable::NE) * ScatteringTable::NB);
        const int nt = int(std::max(1u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (int k = 0; k < nt; ++k)
            pool.emplace_back([k, nt] {
                for (int i = k; i < ScatteringTable::NE; i += nt) {
                    const double eps = std::pow(10.0, ScatteringTable::LE0 + i * ScatteringTable::DLE);
                    for (int j = 0; j < ScatteringTable::NB; ++j) {
                        const Scattering s = universalScattering(eps, j * ScatteringTable::DB);
                        t.e[std::size_t(i) * ScatteringTable::NB + std::size_t(j)] = {float(s.theta), float(s.tau), float(s.r0)};
                    }
                }
            });
        for (std::thread& p : pool) p.join();
    });
    return t;
}

/// A pair, an ion and a target atom: its screening length (Å), ε an eV, the
/// largest share of the energy a collision gives, the masses' ratio.
struct Pair {
    int Z2 = 0;
    double M2 = 0, a = 0, epsPerEV = 0, gammaT = 0, massRatio = 0, A = 0;
    Pair() = default;
    Pair(int Z1, double M1, int z2, double m2)
        : Z2(z2), M2(m2)
    {
        a = screeningLength(Z1, Z2);
        epsPerEV = a * M2 / (M1 + M2) / (Z1 * Z2 * E2);
        gammaT = 4 * M1 * M2 / ((M1 + M2) * (M1 + M2));
        massRatio = M1 / M2;
        A = M2 / M1;
    }
    /// θ, τ (Å), R0 (Å) at the ion's energy E (eV) and impact parameter p (Å).
    void lookup(const ScatteringTable& t, double E, double p, double& theta, double& tau, double& r0) const
    {
        const double le = std::log10(std::max(E * epsPerEV, 1e-12));
        double fi = (le - ScatteringTable::LE0) / ScatteringTable::DLE;
        fi = std::clamp(fi, 0.0, ScatteringTable::NE - 1.0001);
        double fj = p / a / ScatteringTable::DB;
        fj = std::clamp(fj, 0.0, ScatteringTable::NB - 1.0001);
        const int i = int(fi), j = int(fj);
        const double di = fi - i, dj = fj - j;
        const ScatteringTable::Entry* r0p = &t.e[std::size_t(i) * ScatteringTable::NB + std::size_t(j)];
        const ScatteringTable::Entry* r1p = r0p + ScatteringTable::NB;
        auto bil = [&](float ScatteringTable::Entry::*f) {
            return (1 - di) * ((1 - dj) * double(r0p[0].*f) + dj * double(r0p[1].*f)) + di * ((1 - dj) * double(r1p[0].*f) + dj * double(r1p[1].*f));
        };
        theta = bil(&ScatteringTable::Entry::theta);
        tau = bil(&ScatteringTable::Entry::tau) * a;
        r0 = bil(&ScatteringTable::Entry::r0) * a;
    }
};

// ------------------------------------------------ Electronic stopping

const double ALPHA = std::cbrt(4.0 / (9.0 * PI));

/// Sp / v in atomic units at the electron gas's rs (bohr): Echenique,
/// Nieminen and Ritchie's friction, its G(rs) term (Cai et al.'s Eqs. 8-10).
double friction(double rs)
{
    rs = std::max(rs, 1e-3);
    const double rg = std::min(rs, 6.0);
    const double G = 1.0 + 0.717 * rg - 0.125 * rg * rg - 0.0124 * rg * rg * rg + 0.00212 * rg * rg * rg * rg;
    const double R = 2.0 / (3 * PI) * (std::log(1 + PI / (ALPHA * rs)) - 1.0 / (1 + ALPHA * rs / PI));
    return R * G;
}

/// Brandt and Kitagawa's fractional effective charge at v (a.u.), rs0 in
/// bohr (Cai et al.'s Eqs. 1-7).
double effectiveCharge(double v, double Z1, double rs0)
{
    const double vF = 1.0 / (ALPHA * rs0);
    const double vr = v >= vF ? v * (1 + vF * vF / (5 * v * v))
                              : 0.75 * vF * (1 + 2 * v * v / (3 * vF * vF) - std::pow(v, 4) / (15 * std::pow(vF, 4)));
    const double yr = vr / std::pow(Z1, 2.0 / 3.0);
    const double q = std::clamp(1 - std::exp(-0.95 * (yr - 0.07)), 0.0, 1.0);
    const double lambda = 2 * 0.24005 * std::pow(1 - q, 2.0 / 3.0) / (std::cbrt(Z1) * (1 - (1 - q) / 7));
    return q + 0.5 * (1 - q) * std::log(1 + std::pow(4 * lambda / rs0, 2));
}

/// The valence (outer s and p) electrons of an element: an amorphous
/// layer's electron gas (SiO₂'s rs 1.66 bohr, as Cai et al.'s oxide). The
/// transition metals' s electrons alone.
int valence(int Z)
{
    static const int shells[] = {0, 2, 10, 18, 36, 54, 86, 118};
    int below = 0;
    for (int k = 1; k < 8; ++k)
        if (Z <= shells[k]) {
            below = shells[k - 1];
            break;
        }
    const int n = Z - below;
    if (Z <= 18) return n;                             // H to Ar
    if (n <= 2) return n;                              // K, Ca, Rb, Sr, Cs, Ba
    if (Z <= 54) return n >= 13 ? n - 10 : 2;          // Ga..Kr, In..Xe; the d metals' s
    return n >= 27 ? n - 24 : 2;                       // Tl..Rn; the f and d metals' s
}

// ------------------------------------------------------------ The crystal

/// The diamond lattice's basis, in its cubic cell.
const double kDiamond[8][3] = {{0, 0, 0}, {0, .5, .5}, {.5, 0, .5}, {.5, .5, 0}, {.25, .25, .25}, {.25, .75, .75}, {.75, .25, .75}, {.75, .75, .25}};
constexpr double SILICON_A = 5.431;   // Å

/*!
 * F(rs(x)) over the cubic cell (periodic) from a radial density table - its
 * atoms' density inside the muffin-tin radius rmt (all of each table, when
 * rmt is 0: free atoms superposed), the background over all: a grid of n³,
 * trilinear between its points. Made once for each table.
 */
struct DensityGrid {
    int n = 0;
    double a = SILICON_A, average = 0;
    std::vector<float> F;
    double at(V3 p) const
    {
        const double fx = p.x / a * n - 0.5, fy = p.y / a * n - 0.5, fz = p.z / a * n - 0.5;
        const double flx = std::floor(fx), fly = std::floor(fy), flz = std::floor(fz);
        const double dx = fx - flx, dy = fy - fly, dz = fz - flz;
        auto w = [this](double v) {
            long long k = (long long)v % n;
            return int(k < 0 ? k + n : k);
        };
        const int x0 = w(flx), x1 = (x0 + 1) % n, y0 = w(fly), y1 = (y0 + 1) % n, z0 = w(flz), z1 = (z0 + 1) % n;
        auto g = [this](int i, int j, int k) { return double(F[(std::size_t(i) * std::size_t(n) + std::size_t(j)) * std::size_t(n) + std::size_t(k)]); };
        return (1 - dx) * ((1 - dy) * ((1 - dz) * g(x0, y0, z0) + dz * g(x0, y0, z1)) + dy * ((1 - dz) * g(x0, y1, z0) + dz * g(x0, y1, z1)))
               + dx * ((1 - dy) * ((1 - dz) * g(x1, y0, z0) + dz * g(x1, y0, z1)) + dy * ((1 - dz) * g(x1, y1, z0) + dz * g(x1, y1, z1)));
    }
};

const DensityGrid* densityGrid(const QString& resource, QString* why)
{
    static std::mutex m;
    static std::map<QString, std::unique_ptr<DensityGrid>> made;
    std::lock_guard<std::mutex> lock(m);
    if (auto it = made.find(resource); it != made.end()) return it->second.get();
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *why = tr("the crystal's electron density (%1) is not there").arg(resource);
        return nullptr;
    }
    double rmt = 0, background = 0;
    std::vector<double> rr, dd;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList w = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (w.size() < 2) continue;
        if (w[0] == QLatin1String("rmt")) rmt = w[1].toDouble();
        else if (w[0] == QLatin1String("background")) background = w[1].toDouble();
        else if (w[0] == QLatin1String("points")) continue;
        else {
            rr.push_back(w[0].toDouble());
            dd.push_back(w[1].toDouble());
        }
    }
    if (rr.size() < 2) {
        *why = tr("the crystal's electron density (%1) is empty").arg(resource);
        return nullptr;
    }
    // e/Å³ at r: log-log between the table's points.
    auto density = [&rr, &dd](double r) {
        if (r <= rr.front()) return dd.front();
        const auto it = std::upper_bound(rr.begin(), rr.end(), r);
        const std::size_t k = std::size_t(it - rr.begin());
        if (k >= rr.size()) return dd.back();
        const double t = std::log(r / rr[k - 1]) / std::log(rr[k] / rr[k - 1]);
        return std::exp((1 - t) * std::log(dd[k - 1]) + t * std::log(dd[k]));
    };
    auto g = std::make_unique<DensityGrid>();
    const int n = 96;
    g->n = n;
    g->F.assign(std::size_t(n) * n * n, 0.0f);
    const double cut = rmt > 0 ? rmt : rr.back(), A = g->a;
    const int nt = int(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (int t = 0; t < nt; ++t)
        pool.emplace_back([&, t] {
            for (int i = t; i < n; i += nt)
                for (int j = 0; j < n; ++j)
                    for (int k = 0; k < n; ++k) {
                        const V3 p{(i + 0.5) / n * A, (j + 0.5) / n * A, (k + 0.5) / n * A};
                        double rho = background;
                        for (int cx = -1; cx <= 1; ++cx)
                            for (int cy = -1; cy <= 1; ++cy)
                                for (int cz = -1; cz <= 1; ++cz)
                                    for (const auto& b : kDiamond) {
                                        const V3 d = p - V3{(cx + b[0]) * A, (cy + b[1]) * A, (cz + b[2]) * A};
                                        const double r = std::sqrt(dot(d, d));
                                        if (r < cut) rho += density(r);
                                    }
                        const double rs = std::cbrt(3.0 / (4 * PI * rho * BOHR * BOHR * BOHR));
                        g->F[(std::size_t(i) * n + std::size_t(j)) * n + std::size_t(k)] = float(friction(rs));
                    }
        });
    for (std::thread& p : pool) p.join();
    double sum = 0;
    for (float f : g->F) sum += f;
    g->average = sum / double(g->F.size());
    return made.emplace(resource, std::move(g)).first->second.get();
}

/// The crystal's own axes for a surface: its normal, the ⟨110⟩ in it the
/// rotation is from (the wafer's flat), the third; the periods along the
/// two in the surface (Å over the lattice constant), translations of the
/// lattice - an ion's start uniform over them is uniform over the lattice.
struct Axes {
    V3 n, f, g;
    double pf = 0, pg = 0;
};

std::optional<Axes> axesOf(const QString& orientation)
{
    const double r2 = std::sqrt(2.0), r3 = std::sqrt(3.0), r6 = std::sqrt(6.0);
    Axes a;
    if (orientation == QLatin1String("100")) {
        a.n = {0, 0, 1};
        a.f = {1 / r2, 1 / r2, 0};
        a.pf = 1 / r2;
        a.pg = 1 / r2;
    } else if (orientation == QLatin1String("110")) {
        a.n = {1 / r2, 1 / r2, 0};
        a.f = {1 / r2, -1 / r2, 0};
        a.pf = 1 / r2;
        a.pg = 1;
    } else if (orientation == QLatin1String("111")) {
        a.n = {1 / r3, 1 / r3, 1 / r3};
        a.f = {1 / r2, -1 / r2, 0};
        a.pf = 1 / r2;
        a.pg = r6 / 2;
    } else {
        return std::nullopt;
    }
    a.g = cross(a.n, a.f);
    return a;
}

/// Silicon's thermal vibration (Å, each axis) at T: Debye's ⟨u²⟩ ∝ T (φ(x)/x
/// + 1/4), x = Θ/T, through 0.075 Å at 300 K.
double vibration(double T)
{
    auto shape = [](double T) {
        const double x = THETA_DEBYE / std::max(T, 1.0);
        // Debye's φ(x) = (1/x) ∫0^x t/(e^t - 1) dt, by Simpson's rule.
        const int N = 200;
        double s = 0;
        for (int k = 0; k <= N; ++k) {
            const double t = x * k / N;
            const double f = t < 1e-9 ? 1.0 : t / std::expm1(t);
            s += f * (k == 0 || k == N ? 1 : (k % 2 ? 4 : 2));
        }
        const double phi = s * (x / N) / 3 / x;
        return std::max(T, 1.0) * (phi / x + 0.25);
    };
    return U1_300K * std::sqrt(shape(T) / shape(300));
}

// ------------------------------------------------------------- The cache

std::mutex cacheMutex;
std::list<std::pair<QString, std::shared_ptr<const CrystalProfile>>> cache;   // the newest first
constexpr std::size_t CACHE_SIZE = 32;

} // namespace

QString CrystalRun::key() const
{
    QStringList k;
    k << QString::number(Z1) << QString::number(M1, 'g', 10) << QString::number(energy, 'g', 12) << QString::number(tilt, 'g', 12)
      << QString::number(rotation, 'g', 12) << QString::number(divergence, 'g', 12) << orientation << crystal.formula
      << QString::number(crystal.atoms, 'g', 10) << QString::number(temperature, 'g', 10) << QString::number(ions)
      << QString::number(seed) << QString::number(depth, 'g', 10) << QString::number(int(amorphous));
    for (const CrystalLayer& c : cover)
        k << c.target.formula << QString::number(c.target.atoms, 'g', 10) << QString::number(c.thickness, 'g', 10);
    return k.join(QLatin1Char('|'));
}

Scattering tabulatedScattering(double eps, double b)
{
    // (A pair of unit screening length, one ε to the eV: its lookup reads
    // (ε, b) as they are.)
    Pair p;
    p.a = 1;
    p.epsPerEV = 1;
    Scattering s;
    p.lookup(scatteringTable(), eps, b, s.theta, s.tau, s.r0);
    return s;
}

void clearCrystalCache()
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.clear();
}

// ------------------------------------------------------ What it is for

QString crystalTrust(const CrystalRun& run, QStringList* warnings)
{
    // The crystal: silicon's lattice and electron density alone.
    if (run.crystal.formula != QLatin1String("Si"))
        return tr("the crystal profile has silicon's lattice and electron density alone, not %1's: a Pearson or dual Pearson profile "
                  "there")
            .arg(run.crystal.formula.isEmpty() ? tr("this crystal") : run.crystal.formula);
    if (!axesOf(run.orientation)) return tr("a (%1) surface: the crystal profile takes (100), (110) and (111)").arg(run.orientation);
    // The ion: Brandt and Kitagawa's effective charge holds from lithium.
    if (run.Z1 < 3 || run.Z1 > 92)
        return tr("the crystal profile's effective-charge model holds for 3 ≤ Z ≤ 92 (lithium to uranium), not Z %1").arg(run.Z1);
    // Its speed: the friction is linear in v up to the Fermi velocity
    // (rs0's, about 21 keV a nucleon); past twice that, refused.
    const double vF = 1.0 / (ALPHA * RS0 / BOHR);
    const double v = std::sqrt(2 * run.energy / HARTREE / (run.M1 * AMU_ME));
    const double keV = run.energy / 1e3;
    if (v > 2 * vF)
        return tr("%1 keV is too fast for the crystal profile's stopping (linear in the ion's speed, past twice the Fermi velocity): "
                  "a Pearson profile")
            .arg(formatNumber(keV, 4));
    if (run.energy < 1e3) return tr("%1 keV is below where binary collisions hold (1 keV): a Pearson profile").arg(formatNumber(keV, 4));
    if (v > vF)
        warnings->append(tr("%1 keV is past the Fermi velocity, where the crystal profile's electronic stopping (linear in the ion's speed) "
                            "comes out high").arg(formatNumber(keV, 4)));
    // What it was checked against: phosphorus into (100) silicon, 15 to 200
    // keV, Cai et al.'s SIMS, nothing fitted.
    const bool checked = run.Z1 == 15 && run.orientation == QLatin1String("100") && keV >= 15 * 0.999 && keV <= 200 * 1.001;
    if (!checked) {
        QString what;
        if (run.Z1 == 5 || run.Z1 == 33)
            what = tr("boron and arsenic take the parameter published for them (rs0 1.109 Å), but have not been checked against SIMS here");
        else if (run.Z1 != 15)
            what = tr("only phosphorus has been checked against SIMS");
        else if (run.orientation == QLatin1String("110"))
            what = tr("only (100) has been checked against SIMS, and on (110) the model's ⟨110⟩ channelling comes out shallower than its "
                      "⟨100⟩ - though ⟨110⟩ is silicon's most open channel: a profile there an estimate at best");
        else if (run.orientation != QLatin1String("100"))
            what = tr("only (100) has been checked against SIMS");
        else
            what = tr("phosphorus was checked against SIMS from 15 to 200 keV");
        warnings->append(tr("crystal profile outside what it was checked for: %1").arg(what));
    }
    return {};
}

// --------------------------------------------------------- The profile

double CrystalProfile::remaining(double depth) const
{
    if (ions <= 0 || edges.empty()) return 0;
    const double incoming = double(ions - back);
    double stopped = 0;
    if (depth <= edges.front()) stopped = 0;
    else if (depth >= edges.back()) stopped = cumulative.back();
    else {
        const std::size_t k = std::size_t(std::upper_bound(edges.begin(), edges.end(), depth) - edges.begin());
        const double t = (depth - edges[k - 1]) / (edges[k] - edges[k - 1]);
        stopped = cumulative[k - 1] + t * (cumulative[k] - cumulative[k - 1]);
    }
    return std::max(0.0, incoming - stopped) / double(ions);
}

double CrystalProfile::lateral(double depth) const
{
    if (pieceLateral.empty()) return 0;
    std::size_t k = std::size_t(std::upper_bound(edges.begin(), edges.end(), depth) - edges.begin());
    k = std::clamp<std::size_t>(k, 1, pieceLateral.size()) - 1;
    return pieceLateral[k];
}

void CrystalProfile::finish()
{
    // The pieces: bins merged until each holds 20 ions (40 bins at most; one
    // where there are enough) - not across the crystal's surface.
    edges.clear();
    cumulative.clear();
    pieceLateral.clear();
    const int nb = int(counts.size());
    std::vector<std::array<double, 3>> piece;   // n, Σx, Σx²
    edges.push_back(0);
    cumulative.push_back(0);
    double sum = 0;
    int i = 0;
    while (i < nb) {
        int j = i;
        double n = 0, sx = 0, sx2 = 0;
        while (j < nb && (n < 20 || j == i) && j - i < 40 && !(j > i && j == coverBins)) {
            n += counts[std::size_t(j)];
            sx += sumX[std::size_t(j)];
            sx2 += sumX2[std::size_t(j)];
            ++j;
        }
        sum += n;
        edges.push_back(j < coverBins ? std::min(j * bin, coverThickness) : coverThickness + (j - coverBins) * bin);
        cumulative.push_back(sum);
        piece.push_back({n, sx, sx2});
        i = j;
    }
    // Each piece's straggle across: about its own mean, pooled with its
    // neighbours until 10 ions.
    for (std::size_t k = 0; k < piece.size(); ++k) {
        double n = piece[k][0], sx = piece[k][1], sx2 = piece[k][2];
        for (std::size_t w = 1; n < 10 && (k >= w || k + w < piece.size()); ++w) {
            if (k >= w) {
                n += piece[k - w][0];
                sx += piece[k - w][1];
                sx2 += piece[k - w][2];
            }
            if (k + w < piece.size()) {
                n += piece[k + w][0];
                sx += piece[k + w][1];
                sx2 += piece[k + w][2];
            }
        }
        pieceLateral.push_back(n > 1 ? std::sqrt(std::max(sx2 / n - (sx / n) * (sx / n), 0.0)) : 0.0);
    }
    // In the crystal: its moments, from its surface; 1e-3 of the peak.
    double m0 = 0, m1 = 0;
    for (int k = coverBins; k < nb; ++k) {
        const double z = (k - coverBins + 0.5) * bin, c = counts[std::size_t(k)];
        m0 += c;
        m1 += c * z;
    }
    inCrystal = ions > 0 ? m0 / double(ions) : 0;
    if (m0 > 0) {
        Rp = m1 / m0;
        double c2 = 0, c3 = 0, c4 = 0;
        for (int k = coverBins; k < nb; ++k) {
            const double s = (k - coverBins + 0.5) * bin - Rp, c = counts[std::size_t(k)];
            c2 += c * s * s;
            c3 += c * s * s * s;
            c4 += c * s * s * s * s;
        }
        c2 /= m0;
        c3 /= m0;
        c4 /= m0;
        dRp = std::sqrt(c2);
        gamma = c2 > 0 ? c3 / std::pow(c2, 1.5) : 0;
        beta = c2 > 0 ? c4 / (c2 * c2) : 3;
        // The pieces' densities below the surface: the deepest past 1e-3 of
        // the highest.
        double peak = 0;
        std::vector<std::pair<double, double>> dens;   // (deeper edge, density)
        for (std::size_t k = 0; k + 1 < edges.size(); ++k) {
            if (edges[k] < coverThickness - 1e-15) continue;
            const double w = edges[k + 1] - edges[k];
            const double d = w > 0 ? (cumulative[k + 1] - cumulative[k]) / w : 0;
            dens.emplace_back(edges[k + 1] - coverThickness, d);
            peak = std::max(peak, d);
        }
        tail = 0;
        for (const auto& [z, d] : dens)
            if (d >= 1e-3 * peak) tail = z;
    }
}

// ---------------------------------------------------------- The Monte Carlo

std::shared_ptr<const CrystalProfile> crystalImplant(const CrystalRun& run, const std::function<bool(double)>& ticker, QString* error, bool* cached)
{
    if (cached) *cached = false;
    const QString key = run.key();
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key) {
                cache.splice(cache.begin(), cache, it);
                if (cached) *cached = true;
                return cache.front().second;
            }
    }
    QStringList ignored;
    if (const QString refused = crystalTrust(run, &ignored); !refused.isEmpty()) {
        *error = refused;
        return nullptr;
    }
    if (!(run.ions > 0)) {
        *error = tr("its ions must be more than 0");
        return nullptr;
    }
    QElapsedTimer clock;
    clock.start();
    const DensityGrid* grid = densityGrid(QStringLiteral(":/multiphysics/crystal/si_density_cai.txt"), error);
    if (!grid) return nullptr;
    const ScatteringTable& table = scatteringTable();
    const Axes ax = *axesOf(run.orientation);
    const double A = grid->a;
    const double Z1 = run.Z1, M1 = run.M1;
    const double rs0 = RS0 / BOHR;
    const double massMe = M1 * AMU_ME;
    const double conv = HARTREE / BOHR;   // Hartree/bohr to eV/Å
    const double u1 = vibration(run.temperature);

    // The amorphous layers: from the top, d (Å along the normal, the
    // crystal's surface at 0) from -T to 0; each its atoms (Å⁻³), free path,
    // largest impact parameter, pairs and their shares, its electron gas's F.
    struct Layer {
        double top = 0, bottom = 0, N = 0, L = 0, pm = 0, F = 0;
        std::vector<Pair> pairs;
        std::vector<double> share;   // cumulative
    };
    auto layerOf = [&](const Target& t, bool sameAsCrystal) {
        Layer l;
        l.N = t.atoms * 1e-30;
        l.L = std::cbrt(1 / l.N);
        l.pm = 1 / std::sqrt(PI * l.L * l.N);
        double cum = 0, electrons = 0;
        for (const Target::Part& p : t.parts) {
            l.pairs.emplace_back(run.Z1, M1, p.Z, p.M);
            cum += p.share;
            l.share.push_back(cum);
            electrons += p.share * valence(p.Z);
        }
        if (!l.share.empty()) l.share.back() = 1.0 + 1e-12;
        if (sameAsCrystal) l.F = grid->average;
        else {
            const double n = std::max(electrons * l.N * BOHR * BOHR * BOHR, 1e-12);   // bohr⁻³
            l.F = friction(std::cbrt(3.0 / (4 * PI * n)));
        }
        return l;
    };
    std::vector<Layer> layers;
    double T = 0;
    for (const CrystalLayer& c : run.cover) T += c.thickness * 1e10;
    {
        double d = -T;
        for (const CrystalLayer& c : run.cover) {
            if (c.target.parts.empty() || !(c.target.atoms > 0)) {
                *error = tr("%1 has no atoms known for the crystal profile").arg(c.material);
                return nullptr;
            }
            Layer l = layerOf(c.target, c.target.formula == run.crystal.formula);
            l.top = d;
            d += c.thickness * 1e10;
            l.bottom = d;
            layers.push_back(std::move(l));
        }
        if (!layers.empty()) layers.back().bottom = 0;
    }
    const Layer bulk = layerOf(run.crystal, true);   // the crystal as amorphous
    const std::vector<Pair> latticePairs = [&] {
        std::vector<Pair> v;
        for (const Target::Part& p : run.crystal.parts) v.emplace_back(run.Z1, M1, p.Z, p.M);
        return v;
    }();
    const Pair& lattice = latticePairs.front();   // silicon's (checked above)

    // The depth followed, and its bins.
    double zmax = run.depth * 1e10;
    if (!(zmax > 0)) {
        double Rp = 0.1e-6, dRp = 0.04e-6;
        const Element* e = element(run.Z1);
        const std::optional<ImplantMoments> m = implantMoments(Ion{e ? e->symbol : QString(), run.Z1, M1, 1.0}, run.energy, run.crystal);
        if (m) {
            Rp = m->Rp;
            dRp = m->dRp;
        }
        zmax = std::max(0.2e-6, 20 * (Rp + dRp)) * 1e10;
    }
    const double bin = std::max(2.5, zmax / 8000);   // Å
    const int coverBins = int(std::ceil(T / bin - 1e-9));
    const int nbin = coverBins + int(std::ceil(zmax / bin));

    // The beam: its direction in the crystal's axes, toward +x as it goes
    // down - x the section's, the rotation from the flat.
    const double tilt = std::abs(run.tilt);
    const double rot = run.rotation + (run.tilt < 0 ? PI : 0);
    const V3 across = normalize(std::cos(rot) * ax.f + std::sin(rot) * ax.g);   // the beam's way across
    const V3 ex = run.tilt < 0 ? -1.0 * across : across;                        // the section's +x
    const V3 u0 = normalize(std::sin(tilt) * across + std::cos(tilt) * ax.n);
    const double cosDiv = std::cos(run.divergence);

    // Each ion's end: its bin (-1 backscattered, -2 past the depth), its x.
    std::vector<int> endBin(std::size_t(run.ions), -2);
    std::vector<double> endX(std::size_t(run.ions), 0.0);
    std::atomic<long> next{0}, done{0};
    std::atomic<bool> cancel{false};

    auto worker = [&](bool caller) {
        QElapsedTimer told;
        told.start();
        auto depthOf = [&ax](V3 p) { return dot(ax.n, p); };
        auto layerAt = [&](double d) -> const Layer* {
            if (d >= 0) return run.amorphous ? &bulk : nullptr;
            for (const Layer& l : layers)
                if (d >= l.top && d < l.bottom) return &l;
            return layers.empty() ? nullptr : &layers.front();
        };
        for (;;) {
            if (cancel.load(std::memory_order_relaxed)) break;
            const long id = next.fetch_add(1);
            if (id >= run.ions) break;
            Random rng(run.seed, (unsigned long long)id);
            auto randomPerp = [&rng](V3 u) {
                const V3 a = std::abs(u.x) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0};
                const V3 e1 = normalize(a - dot(a, u) * u);
                const V3 e2 = cross(u, e1);
                const double f = 2 * PI * rng.uniform();
                return std::cos(f) * e1 + std::sin(f) * e2;
            };
            auto speed = [massMe](double E) { return std::sqrt(2 * E / HARTREE / massMe); };
            // The electronic loss along a straight segment: the crystal's
            // density sampled every 0.1 Å; a layer's gas, uniform.
            auto eloss = [&](V3 a, V3 b, double E) {
                const V3 d = b - a;
                const double L = std::sqrt(dot(d, d));
                if (!(L > 0)) return 0.0;
                const double v = speed(E), zg = Z1 * effectiveCharge(v, Z1, rs0), pref = zg * zg * v * conv;
                const double da = depthOf(a), db = depthOf(b);
                if ((da < 0 && db < 0) || (run.amorphous && da >= 0 && db >= 0)) {
                    const Layer* l = layerAt(0.5 * (da + db));
                    return pref * (l ? l->F : grid->average) * L;
                }
                double sum = 0;
                const int ns = std::max(1, int(std::ceil(L / 0.1)));
                for (int s = 0; s < ns; ++s) {
                    const V3 p = a + ((s + 0.5) / ns) * d;
                    const double dp = depthOf(p);
                    const Layer* l = dp < 0 || run.amorphous ? layerAt(dp) : nullptr;
                    sum += l ? l->F : grid->at(p);
                }
                return pref * sum / ns * L;
            };
            // A collision with a partner at impact parameter p, ep the unit
            // vector from it to the ion across the path, s ahead.
            auto collide = [&](const Pair& pr, double& E, V3& r, V3& u, double s, double p, V3 ep) {
                double theta = 0, tauA = 0, r0A = 0;
                pr.lookup(table, E, p, theta, tauA, r0A);
                const double Zs = Z1 + pr.Z2, vcms = speed(E) * 2.18769126e8;
                const double firsov = 4.3e-8 * std::pow(Zs, 5.0 / 3.0) * vcms / std::pow(1 + 0.31 * std::cbrt(Zs) * r0A, 5);
                // To where the asymptotes cross.
                const double x1 = (2 * tauA + (pr.A - 1) * p * std::tan(0.5 * theta)) / (1 + pr.A);
                const V3 q = r + std::max(s - x1, 0.0) * u;
                E -= eloss(r, q, E);
                r = q;
                const double psi = std::atan2(std::sin(theta), std::cos(theta) + pr.massRatio);
                u = normalize(std::cos(psi) * u + std::sin(psi) * ep);
                E -= pr.gammaT * E * std::pow(std::sin(0.5 * theta), 2);
                E -= std::min(firsov, 0.5 * E);
            };
            // In: the nominal direction within the cone, anywhere over the
            // surface's lattice.
            const double ca = 1 - rng.uniform() * (1 - cosDiv), sa = std::sqrt(std::max(0.0, 1 - ca * ca));
            V3 u = normalize(ca * u0 + sa * randomPerp(u0));
            const V3 start = (16 * ax.pf * A * rng.uniform()) * ax.f + (16 * ax.pg * A * rng.uniform()) * ax.g;
            V3 r = start - (T + 1e-6) * ax.n;
            double E = run.energy;
            std::array<long long, 12> struck;
            struck.fill(LLONG_MIN);
            int sp = 0;
            int end = -2;
            for (;;) {
                const double d = depthOf(r);
                if (E < EMIN) {
                    end = d < 0 ? std::clamp(int(std::floor((d + T) / bin)), 0, std::max(coverBins - 1, 0))
                                : coverBins + int(std::floor(d / bin));
                    if (end >= nbin) end = -2;
                    break;
                }
                if (d < -T - 1e-3) {
                    end = -1;
                    break;
                }
                if (d > zmax) {
                    end = -2;
                    break;
                }
                if (d < 0 && layers.empty()) {
                    // No cover: above the bare crystal - in, or gone.
                    const double un = dot(u, ax.n);
                    if (un <= 0) {
                        end = -1;
                        break;
                    }
                    r = r + ((-d + 1e-6) / un) * u;
                    continue;
                }
                if (d < 0 || run.amorphous) {
                    // Amorphous: a free path, then a partner at a random
                    // impact parameter - stopping at a layer's edge first.
                    const Layer* l = layerAt(d);
                    const V3 nr = r + l->L * u;
                    const double dn = depthOf(nr);
                    const double below = d < 0 ? l->bottom : std::numeric_limits<double>::infinity();
                    const double above = d < 0 ? l->top : 0;
                    const double un = dot(u, ax.n);
                    if ((dn >= below && un > 0) || (dn < above && un < 0 && above > -T + 1e-9)) {
                        const double edge = dn >= below ? below : above;
                        const double s = (edge - d) / un;
                        const V3 q = r + (s + 1e-6) * u;
                        E -= eloss(r, q, E);
                        r = q;
                        continue;
                    }
                    const double x = rng.uniform();
                    std::size_t k = 0;
                    while (k + 1 < l->share.size() && x > l->share[k]) ++k;
                    const double p = l->pm * std::sqrt(rng.uniform());
                    collide(l->pairs[k], E, r, u, l->L, p, randomPerp(u));
                    continue;
                }
                // The crystal: the nearest lattice atom ahead within PMAX, not
                // one of the last twelve struck.
                const V3 c = r + (0.5 * SEARCH) * u;
                const double h = 0.5 * SEARCH + PMAX;
                const int x0 = int(std::floor((c.x - h) / A)), x1 = int(std::floor((c.x + h) / A));
                const int y0 = int(std::floor((c.y - h) / A)), y1 = int(std::floor((c.y + h) / A));
                const int z0 = int(std::floor((c.z - h) / A)), z1 = int(std::floor((c.z + h) / A));
                double bestS = 1e30;
                V3 bestR{0, 0, 0};
                long long bestId = 0;
                for (int ix = x0; ix <= x1; ++ix)
                    for (int iy = y0; iy <= y1; ++iy)
                        for (int iz = z0; iz <= z1; ++iz)
                            for (int k = 0; k < 8; ++k) {
                                const V3 R{(ix + kDiamond[k][0]) * A, (iy + kDiamond[k][1]) * A, (iz + kDiamond[k][2]) * A};
                                if (depthOf(R) < -1e-6) continue;
                                const V3 dv = R - r;
                                const double s = dot(dv, u);
                                if (s <= 1e-6 || s > SEARCH || s >= bestS) continue;
                                const double p2 = dot(dv, dv) - s * s;
                                if (p2 > PMAX * PMAX) continue;
                                const long long aid = ((((long long)(ix + 100000) * 200003LL) + (iy + 100000)) * 200003LL + (iz + 100000)) * 8 + k;
                                bool seen = false;
                                for (long long sid : struck)
                                    if (sid == aid) {
                                        seen = true;
                                        break;
                                    }
                                if (seen) continue;
                                bestS = s;
                                bestR = R;
                                bestId = aid;
                            }
                if (bestS > 1e29) {   // a free flight
                    const V3 q = r + SEARCH * u;
                    E -= eloss(r, q, E);
                    r = q;
                    continue;
                }
                struck[std::size_t(sp)] = bestId;
                sp = (sp + 1) % int(struck.size());
                const V3 R = bestR + V3{rng.normal(u1), rng.normal(u1), rng.normal(u1)};   // its vibration
                const V3 dv = R - r;
                const double s = dot(dv, u);
                if (s <= 0) continue;
                const V3 perp = dv - s * u;
                const double p = std::sqrt(std::max(dot(perp, perp), 0.0));
                const V3 ep = p > 1e-12 ? (-1.0 / p) * perp : randomPerp(u);
                collide(lattice, E, r, u, s, p, ep);
            }
            endBin[std::size_t(id)] = end;
            endX[std::size_t(id)] = dot(r - start, ex);
            const long finished = done.fetch_add(1) + 1;
            if (caller && ticker && told.elapsed() > 100) {
                told.restart();
                if (!ticker(double(finished) / double(run.ions))) cancel = true;
            }
        }
    };
    const int nt = run.threads > 0 ? run.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (int t = 1; t < nt; ++t) pool.emplace_back(worker, false);
    worker(true);
    for (std::thread& p : pool) p.join();
    if (cancel) {
        *error = tr("cancelled");
        return nullptr;
    }
    auto out = std::make_shared<CrystalProfile>();
    out->bin = bin * 1e-10;
    out->coverBins = coverBins;
    out->coverThickness = T * 1e-10;
    out->counts.assign(std::size_t(nbin), 0.0);
    out->sumX.assign(std::size_t(nbin), 0.0);
    out->sumX2.assign(std::size_t(nbin), 0.0);
    out->ions = run.ions;
    for (long i = 0; i < run.ions; ++i) {
        const int b = endBin[std::size_t(i)];
        if (b == -1) ++out->back;
        else if (b < 0) ++out->beyond;
        else {
            const double x = endX[std::size_t(i)] * 1e-10;
            out->counts[std::size_t(b)] += 1;
            out->sumX[std::size_t(b)] += x;
            out->sumX2[std::size_t(b)] += x * x;
        }
    }
    out->threads = nt;
    out->finish();
    out->ms = double(clock.nsecsElapsed()) / 1e6;
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.emplace_front(key, out);
    while (cache.size() > CACHE_SIZE) cache.pop_back();
    return out;
}

} // namespace qucs_s::fem::process
