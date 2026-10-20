/*
 * fem_exciton.cpp - a quantum well's excitons with their subband pairs
 *                   coupled, in momentum space
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_exciton.h"

#include <Eigen/Dense>

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace qucs_s::fem::exciton {

namespace {

constexpr double Q = 1.602176634e-19;      // C
constexpr double Hbar = 1.054571817e-34;   // J s
constexpr double Eps0 = 8.8541878128e-12;  // F/m

QString tr(const char* text)
{
    return QCoreApplication::translate("Exciton", text);
}

// Gauss-Legendre's n nodes and weights on [-1, 1].
void gaussLegendre(int n, std::vector<double>& x, std::vector<double>& w)
{
    x.assign(std::size_t(n), 0.0);
    w.assign(std::size_t(n), 0.0);
    for (int i = 0; i < (n + 1) / 2; ++i) {
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
        x[std::size_t(i)] = -z;
        x[std::size_t(n - 1 - i)] = z;
        w[std::size_t(i)] = w[std::size_t(n - 1 - i)] = 2 / ((1 - z * z) * dp * dp);
    }
}

// The arithmetic-geometric mean of 1 and b: K = π/(2 agm(1, √(1 - m))).
double agm(double b)
{
    double a = 1;
    for (int i = 0; i < 64 && std::abs(a - b) > 1e-16 * a; ++i) {
        const double t = 0.5 * (a + b);
        b = std::sqrt(a * b);
        a = t;
    }
    return a;
}

// The 2D Coulomb kernel's s channel, ∫ dθ/(2π) 1/|k - k'| = (2/π) K(m)/(k
// + k'), m = 4kk'/(k + k')² - its log singularity at k' = k (√(1 - m) =
// |k - k'|/(k + k'), taken so: no cancellation near it).
double coulombS(double k, double kp)
{
    const double s = k + kp;
    if (!(s > 0)) return 0;
    const double b = std::abs(k - kp) / s;
    if (!(b > 0)) return std::numeric_limits<double>::infinity();
    return 1 / (s * agm(b));   // (2/π)(π/(2 agm))/s
}

// The subtraction's function, h(1) = 1: (Kφ)(k) = ∫ k'dk'/(2π) A(k, k')
// [φ(k') - φ(k) h(k'/k)] + φ(k) ∫ k'dk'/(2π) A(k, k') h(k'/k), the first
// regular, the second (k/2π) c(kmax/k) below.
double subtracted(double x)
{
    return 2 / (1 + x * x);
}

// Gauss-Kronrod 7-15 on [a, b], and adaptively to an absolute tolerance.
double gk15(const std::function<double(double)>& f, double a, double b, double* err)
{
    static const double xgk[8] = {0.991455371120812639206854697526329, 0.949107912342758524526189684047851, 0.864864423359769072789712788640926,
                                  0.741531185599394439863864773280788, 0.586087235467691130294144845693013, 0.405845151377397166906606412076961,
                                  0.207784955007898467600689403773245, 0.0};
    static const double wgk[8] = {0.022935322010529224963732008058970, 0.063092092629978553290700663189204, 0.104790010322250183839876322541518,
                                  0.140653259715525918745189590510238, 0.169004726639267902826583426598550, 0.190350578064785409913256402421014,
                                  0.204432940075298892414161999234649, 0.209482141084727828012999174891714};
    static const double wg[4] = {0.129484966168869693270611432679082, 0.279705391489276667901467771423780, 0.381830050505118944950369775488975,
                                 0.417959183673469387755102040816327};
    const double c = 0.5 * (a + b), h = 0.5 * (b - a), fc = f(c);
    double rk = fc * wgk[7], rg = fc * wg[3];
    for (int j = 0; j < 7; ++j) {
        const double d = h * xgk[j], s = f(c - d) + f(c + d);
        rk += wgk[j] * s;
        if (j % 2 == 1) rg += wg[(j - 1) / 2] * s;
    }
    *err = std::abs((rk - rg) * h);
    return rk * h;
}

double adaptive(const std::function<double(double)>& f, double a, double b, double tol, int depth = 0)
{
    // (Its tolerance halved with its interval - floored at the rounding of
    // what it sums: the log singularity at an end converges, a smooth part
    // at once.)
    double err = 0;
    const double r = gk15(f, a, b, &err);
    if (err <= std::max(tol, 1e-14 * std::abs(r)) || depth > 40) return r;
    const double m = 0.5 * (a + b);
    return adaptive(f, a, m, 0.5 * tol, depth + 1) + adaptive(f, m, b, 0.5 * tol, depth + 1);
}

// c = ∫_0^∞ x A(1, x) h(x) dx: the subtraction's own, A(1, x) = (2/π)
// K(4x/(1 + x)²)/(1 + x) - singular (as a log) at x = 1, its integrand
// 2/x² far off (past 2^20, so: 2/x).
double subtractionIntegral()
{
    // (x = 1 itself, a node rounded onto it deep in the refinement: of no
    // measure, nought.)
    auto f = [](double x) { return x == 1 ? 0.0 : x * coulombS(1, x) * subtracted(x); };
    double sum = adaptive(f, 0, 1, 1e-11);
    double a = 1;
    for (int i = 0; i < 20; ++i, a *= 2) sum += adaptive(f, a, 2 * a, 1e-11);
    return sum + 2 / a;
}

// K(m) and E(m) by the arithmetic-geometric mean, 1 - m given apart (near
// m = 1 its digits): E = K (1 - Σ 2^(n-1) c_n²), c_0² = m.
void ellipticKE(double m, double m1, double* K, double* E)
{
    double a = 1, b = std::sqrt(m1), sum = 0.5 * m, p = 0.5;
    for (int i = 0; i < 64 && std::abs(a - b) > 1e-16 * a; ++i) {
        const double an = 0.5 * (a + b), cn = 0.5 * (a - b);
        b = std::sqrt(a * b);
        a = an;
        p *= 2;
        sum += p * cn * cn;
    }
    *K = M_PI / (2 * a);
    *E = *K * (1 - sum);
}

// Legendre's Q_{L-1/2}(χ), χ = 1 + d (d ≥ 0 given apart: near 1 its
// digits): near 1 of K and E (Q_{-1/2} = √m K, Q_{1/2} = χ √m K - 2E/√m, m
// = 2/(χ + 1)) and the recurrence (ν + 1) Q_{ν+1} = (2ν + 1) χ Q_ν - ν
// Q_{ν-1}; past χ = 2, where that loses digits, its hypergeometric series
// in 1/χ², √π Γ(ν + 1)/(Γ(ν + 3/2) (2χ)^(ν+1)) F((ν + 1)/2, (ν + 2)/2; ν +
// 3/2; 1/χ²).
double legendreQ(int L, double d)
{
    const double chi = 1 + d;
    if (chi > 2) {
        double pre = M_PI;   // √π Γ(L + 1/2)/L!
        for (int m = 1; m <= L; ++m) pre *= (m - 0.5) / m;
        const double a = 0.5 * L + 0.25, b = 0.5 * L + 0.75, cc = L + 1.0, x = 1 / (chi * chi);
        double t = 1, s = 1;
        for (int m = 0; m < 400 && t > 1e-17 * s; ++m) {
            t *= (a + m) * (b + m) / ((cc + m) * (m + 1)) * x;
            s += t;
        }
        return pre * std::pow(2 * chi, -(L + 0.5)) * s;
    }
    const double m = 2 / (chi + 1), m1 = d / (chi + 1);
    double K = 0, E = 0;
    ellipticKE(m, m1, &K, &E);
    const double sm = std::sqrt(m);
    double q0 = sm * K;
    if (L == 0) return q0;
    double q1 = chi * sm * K - 2 * E / sm;
    for (int n = 1; n < L; ++n) {
        const double q2 = (4 * n * chi * q1 - (2 * n - 1) * q0) / (2 * n + 1);
        q0 = q1;
        q1 = q2;
    }
    return q1;
}

} // namespace

double ellipticK(double m)
{
    if (!(m < 1)) return std::numeric_limits<double>::infinity();
    return M_PI / (2 * agm(std::sqrt(1 - m)));
}

double coulombChannel(int L, double k, double kp)
{
    if (!(k > 0) || !(kp > 0)) return 0;
    if (k == kp) return std::numeric_limits<double>::infinity();
    // χ = (k² + k'²)/(2kk'): Q_{L-1/2}(χ)/(π √(kk')).
    return legendreQ(std::max(0, L), (k - kp) * (k - kp) / (2 * k * kp)) / (M_PI * std::sqrt(k * kp));
}

Result solve(const Envelopes& envelopes, const std::vector<Pair>& pairs, const Options& o)
{
    Result r;
    const int P = int(pairs.size());
    if (P == 0) return r;
    const int C = std::max(1, o.components);
    const double reach = o.reach > 0 ? o.reach : 0.3 * Q;
    const double eps = Eps0 * (o.epsr > 0 ? o.epsr : 1.0);

    // Each pair's edge, the k its continuum reaches 'reach' above it at,
    // and its mass near the edge.
    std::vector<double> edge(static_cast<std::size_t>(P)), kReach(static_cast<std::size_t>(P)), mass(static_cast<std::size_t>(P));
    for (int p = 0; p < P; ++p) {
        const auto& E = pairs[std::size_t(p)].energy;
        edge[std::size_t(p)] = E(0);
        double lo = 0, hi = 1e7;
        while (E(hi) - edge[std::size_t(p)] < reach && hi < 1e11) hi *= 2;
        for (int it = 0; it < 80; ++it) {
            const double m = 0.5 * (lo + hi);
            (E(m) - edge[std::size_t(p)] < reach ? lo : hi) = m;
        }
        kReach[std::size_t(p)] = hi;
        const double k1 = 0.05 * hi, de = E(k1) - edge[std::size_t(p)];
        mass[std::size_t(p)] = de > 0 ? Hbar * Hbar * k1 * k1 / (2 * de) : Hbar * Hbar * hi * hi / (2 * reach);
    }
    const double kmax = *std::max_element(kReach.begin(), kReach.end());
    // In ε of a reference mass, the heaviest pair's (its continuum reaching
    // 'reach' at kmax): a lighter pair's energies the larger by its ratio,
    // its spacing too - the grid's the finer for it.
    const double mref = Hbar * Hbar * kmax * kmax / (2 * reach);
    double ratio = 1;
    for (double m : mass) ratio = std::max(ratio, std::min(10.0, mref / m));
    // The heaviest pair's 2D Rydberg: its bound states' reach in k, the grid
    // fine near 0 for them.
    const double ry = mref * std::pow(Q * Q / (4 * M_PI * eps), 2) / (2 * Hbar * Hbar);

    // Panels of 4 Gauss-Legendre points in ε: from Ry/24 apart near the edge
    // (the 2s, thrice the 1s's size, at k a third of its), growing by a
    // quarter each, to the continuum's spacing.
    std::vector<double> gx, gw;
    gaussLegendre(4, gx, gw);
    std::vector<double> nodes, weights;
    double s1 = (o.spacing > 0 ? o.spacing : 1e-3 * Q) / ratio;
    auto build = [&nodes, &weights, &gx, &gw, ry, reach](double s) {
        nodes.clear();
        weights.clear();
        double e = 0, w = 4 * std::max(std::min(s, ry / 24), s / 50);   // (no attraction to speak of: s/50)
        while (e < reach * (1 - 1e-12)) {
            const double width = std::min({w, 4 * s, reach - e});
            for (int g = 0; g < 4; ++g) {
                nodes.push_back(e + 0.5 * width * (gx[std::size_t(g)] + 1));
                weights.push_back(0.5 * width * gw[std::size_t(g)]);
            }
            e += width;
            w *= 1.25;
        }
    };
    build(s1);
    const int wanted = P * int(nodes.size());
    while (P * int(nodes.size()) > o.maxSize && s1 < reach / 8) {
        s1 *= 1.2;
        build(s1);
    }
    if (P * int(nodes.size()) < wanted)
        r.note = tr("the continuum's states %1 meV apart (of %2 asked), %3 pairs on %4 k points at most")
                     .arg(s1 * ratio / Q * 1e3, 0, 'g', 3)
                     .arg((o.spacing > 0 ? o.spacing : 1e-3 * Q) / Q * 1e3, 0, 'g', 3)
                     .arg(P)
                     .arg(o.maxSize / P);
    // Past kmax to k infinite, 40 points in u = kmax/k (Gauss-Legendre on
    // (0, 1]): a bound state's short range there - its |ψ(0)|² and binding
    // converge as 1/kmax without them (the ideal 2D 1s 3.65 Ry of 4, its line
    // half, at 0.3 eV), the light's window not.
    const int K0 = int(nodes.size()), tail = 40;
    const int K = K0 + tail;
    // (Past four times the most asked even so: refused, said - its matrix's
    // memory, not a crash.)
    if (P * K > 4 * std::max(o.maxSize, 100)) {
        r.note = tr("%1 pairs on %2 k points: too many to solve").arg(P).arg(K);
        return r;
    }
    std::vector<double> k(static_cast<std::size_t>(K)), W(static_cast<std::size_t>(K)), S(static_cast<std::size_t>(K));
    for (int i = 0; i < K0; ++i) {
        k[std::size_t(i)] = std::sqrt(2 * mref * nodes[std::size_t(i)]) / Hbar;
        W[std::size_t(i)] = mref * weights[std::size_t(i)] / (2 * M_PI * Hbar * Hbar);   // k dk/(2π)
    }
    {
        std::vector<double> ux, uw;
        gaussLegendre(tail, ux, uw);
        for (int t = 0; t < tail; ++t) {
            const double u = 0.5 * (ux[std::size_t(t)] + 1), w = 0.5 * uw[std::size_t(t)];
            k[std::size_t(K0 + t)] = kmax / u;
            W[std::size_t(K0 + t)] = kmax * kmax * w / (2 * M_PI * u * u * u);
        }
    }
    for (int i = 0; i < K; ++i) S[std::size_t(i)] = std::sqrt(W[std::size_t(i)]);
    const double kLast = *std::max_element(k.begin(), k.end());
    r.kmax = kmax;
    r.points = K0;
    r.spacing = s1 * ratio;
    r.asked = o.spacing > 0 ? o.spacing : 1e-3 * Q;

    // Each pair's electron, and its hole at the grid's points: its
    // coefficients on the holes given (Envelopes'; with the tests' form
    // factors each pair its own electron and hole).
    const bool given = bool(o.formFactor);
    const int nb = given ? P : int(envelopes.holes.size());
    std::vector<int> elec(static_cast<std::size_t>(P));
    std::vector<Eigen::MatrixXcd> coef(static_cast<std::size_t>(P));   // [pair]: K × nb
    std::vector<std::vector<int>> used(static_cast<std::size_t>(P));    // [pair]: the holes it is of
    {
        std::vector<std::complex<double>> v;
        for (int p = 0; p < P; ++p) {
            const Pair& pr = pairs[std::size_t(p)];
            elec[std::size_t(p)] = given ? p : pr.electron;
            Eigen::MatrixXcd& cp = coef[std::size_t(p)];
            cp = Eigen::MatrixXcd::Zero(K, nb);
            for (int i = 0; i < K; ++i) {
                if (!given && pr.holeAt) {
                    v.assign(std::size_t(nb), 0.0);
                    pr.holeAt(k[std::size_t(i)], v);
                    for (int s = 0; s < nb; ++s) cp(i, s) = v[std::size_t(s)];
                } else
                    cp(i, given ? p : pr.hole) = 1;
            }
            for (int s = 0; s < nb; ++s)
                if (cp.col(s).cwiseAbs().maxCoeff() > 0) used[std::size_t(p)].push_back(s);
        }
    }
    auto needed = [&](int a, int b) { return a == b || o.coupled; };
    // Each component's channel, |channel - its J_z| (the s channel without
    // them).
    const std::size_t ncomp = given || envelopes.holes.empty() ? 1 : envelopes.holes.front().size();
    std::vector<int> chan(ncomp, 0);
    if (!given)
        for (std::size_t c = 0; c < ncomp && c < envelopes.angular.size(); ++c)
            chan[c] = int(std::lround(std::abs(o.channel - envelopes.angular[c])));

    // The form factors F(q) = ∫∫ ρe(ze) ρh(zh) e^(-q|ze - zh|) of each two
    // electrons' product ρe and two holes' ρh (in a channel: its components
    // summed), tabulated as D(q) = (F(q) - F(0))/q - regular: F(0) the
    // Coulomb part's, singular, apart - on q from kmax/1e4 to 2.1 kmax (ln
    // q), D(0) = -<|ze - zh|>. A hole pair (a, b) the conjugate of (b, a):
    // each once.
    const int nq = 240;
    const double qlo = 1e-4 * kmax, qhi = 2.1 * kLast;
    std::vector<double> qs(static_cast<std::size_t>(nq));
    for (int l = 0; l < nq; ++l) qs[std::size_t(l)] = qlo * std::pow(qhi / qlo, double(l) / (nq - 1));
    using cd = std::complex<double>;
    struct Table {
        int ea = 0, eb = 0, sa = 0, sb = 0, L = 0;
        cd F0 = 0, D0 = 0;
        std::vector<cd> D;
    };
    std::vector<Table> tables;
    std::map<std::array<int, 5>, int> tableOf;   // (ea ≤ eb, sa ≤ sb, L): its index
    // (The holes' densities, by their pair and channel.)
    const std::vector<double>& z = envelopes.z;
    const std::size_t n = z.size();
    std::map<std::array<int, 3>, std::vector<cd>> holeDensity;
    auto holeOf = [&](int sa, int sb, int L) -> const std::vector<cd>& {
        std::vector<cd>& rho = holeDensity[{sa, sb, L}];
        if (rho.empty()) {
            rho.assign(n, 0.0);
            const auto& x = envelopes.holes[std::size_t(sa)];
            const auto& y = envelopes.holes[std::size_t(sb)];
            for (std::size_t c = 0; c < x.size() && c < y.size() && c < ncomp; ++c)
                if (chan[c] == L)
                    for (std::size_t p = 0; p < n; ++p) rho[p] += std::conj(x[c][p]) * y[c][p];
        }
        return rho;
    };
    {
        // (A hole pair and channel no component joins: none - each hole's
        // components of anything.)
        std::vector<std::vector<bool>> has(static_cast<std::size_t>(nb), std::vector<bool>(ncomp, given));
        if (!given)
            for (int s = 0; s < nb; ++s)
                for (std::size_t c = 0; c < ncomp && c < envelopes.holes[std::size_t(s)].size(); ++c)
                    for (const cd& v : envelopes.holes[std::size_t(s)][c])
                        if (v != cd(0)) {
                            has[std::size_t(s)][c] = true;
                            break;
                        }
        std::map<int, std::set<int>> usedBy;   // each electron's pairs' holes
        for (int p = 0; p < P; ++p) usedBy[elec[std::size_t(p)]].insert(used[std::size_t(p)].begin(), used[std::size_t(p)].end());
        std::set<std::pair<int, int>> electronPairs;
        for (int a = 0; a < P; ++a)
            for (int b = 0; b < P; ++b)
                if (needed(a, b)) electronPairs.insert({std::min(elec[std::size_t(a)], elec[std::size_t(b)]), std::max(elec[std::size_t(a)], elec[std::size_t(b)])});
        for (const auto& [ea, eb] : electronPairs)
            for (int s : usedBy[ea])
                for (int t : usedBy[eb])
                    for (std::size_t c = 0; c < ncomp; ++c) {
                        if (!has[std::size_t(s)][c] || !has[std::size_t(t)][c]) continue;
                        const std::array<int, 5> key{ea, eb, std::min(s, t), std::max(s, t), chan[c]};
                        if (tableOf.count(key)) continue;
                        tableOf[key] = int(tables.size());
                        Table tb;
                        tb.ea = ea;
                        tb.eb = eb;
                        tb.sa = key[2];
                        tb.sb = key[3];
                        tb.L = key[4];
                        tb.D.assign(std::size_t(nq), 0.0);
                        tables.push_back(std::move(tb));
                    }
    }
    if (given) {
        for (Table& t : tables) {
            const double F0 = o.formFactor(t.sa, t.sb, 0);
            const double qs0 = qlo * 1e-3;
            t.F0 = F0;
            t.D0 = (o.formFactor(t.sa, t.sb, qs0) - F0) / qs0;
            for (int l = 0; l < nq; ++l) t.D[std::size_t(l)] = (o.formFactor(t.sa, t.sb, qs[std::size_t(l)]) - F0) / qs[std::size_t(l)];
        }
    } else {
        std::map<std::pair<int, int>, std::vector<double>> electronDensity;
        for (const Table& t : tables) {
            std::vector<double>& rho = electronDensity[{t.ea, t.eb}];
            if (!rho.empty()) continue;
            const auto& x = envelopes.electrons[std::size_t(t.ea)];
            const auto& y = envelopes.electrons[std::size_t(t.eb)];
            rho.resize(n);
            for (std::size_t p = 0; p < n; ++p) rho[p] = x[p] * y[p];
        }
        // F(0) and D(0) = -Σp ρe_p Σr ρh_r |z_p - z_r|, by the holes' sums
        // below and above.
        for (Table& t : tables) {
            const std::vector<double>& re = electronDensity[{t.ea, t.eb}];
            const std::vector<cd>& rh = holeOf(t.sa, t.sb, t.L);
            cd below = 0, belowZ = 0, all = 0, allZ = 0, d0 = 0;
            double se = 0;
            for (std::size_t p = 0; p < n; ++p) {
                all += rh[p];
                allZ += rh[p] * z[p];
                se += re[p];
            }
            for (std::size_t p = 0; p < n; ++p) {
                below += rh[p];
                belowZ += rh[p] * z[p];
                d0 += re[p] * (z[p] * below - belowZ + (allZ - belowZ) - z[p] * (all - below));
            }
            t.D0 = -d0;
            t.F0 = se * all;
        }
        // Each electron density's convolution with e^(-q|z|) at every q in
        // two sweeps, O(n), and the holes' densities against them: F = Σp
        // ρh_p (ρe * e^(-q|z|))_p, all of an electron pair's tables at once.
        std::map<std::pair<int, int>, std::vector<int>> ofElectrons;
        for (std::size_t ti = 0; ti < tables.size(); ++ti) ofElectrons[{tables[ti].ea, tables[ti].eb}].push_back(int(ti));
        std::vector<double> step(n > 0 ? n - 1 : 0);
        for (const auto& [ee, list] : ofElectrons) {
            const std::vector<double>& re = electronDensity[ee];
            Eigen::MatrixXd G(Eigen::Index(n), nq);
            for (int l = 0; l < nq; ++l) {
                const double q = qs[std::size_t(l)];
                for (std::size_t p = 0; p + 1 < n; ++p) step[p] = std::exp(-q * (z[p + 1] - z[p]));
                double Lw = 0;
                for (std::size_t p = 0; p < n; ++p) {
                    Lw = (p > 0 ? Lw * step[p - 1] : 0.0) + re[p];
                    G(Eigen::Index(p), l) = Lw;
                }
                double Rw = 0;
                for (std::size_t p = n; p-- > 0;) {
                    if (p + 1 < n) Rw = (Rw + re[p + 1]) * step[p];
                    G(Eigen::Index(p), l) += Rw;
                }
            }
            Eigen::MatrixXd Hr(Eigen::Index(list.size()), Eigen::Index(n)), Hi(Eigen::Index(list.size()), Eigen::Index(n));
            for (std::size_t r = 0; r < list.size(); ++r) {
                const Table& t = tables[std::size_t(list[r])];
                const std::vector<cd>& rh = holeOf(t.sa, t.sb, t.L);
                for (std::size_t p = 0; p < n; ++p) {
                    Hr(Eigen::Index(r), Eigen::Index(p)) = rh[p].real();
                    Hi(Eigen::Index(r), Eigen::Index(p)) = rh[p].imag();
                }
            }
            const Eigen::MatrixXd Fr = Hr * G, Fi = Hi * G;
            for (std::size_t r = 0; r < list.size(); ++r) {
                Table& t = tables[std::size_t(list[r])];
                for (int l = 0; l < nq; ++l) t.D[std::size_t(l)] = (cd(Fr(Eigen::Index(r), l), Fi(Eigen::Index(r), l)) - t.F0) / qs[std::size_t(l)];
            }
        }
    }

    // The angle's Gauss-Legendre points on [0, π]: a channel's regular part
    // ∫ dθ/(2π) cos(Lθ) D(|k - k'|) there; its Coulomb part's (2/π) K/(k +
    // k') - its log singularity at k' = k subtracted - and, past the s
    // channel, the difference of channel L's from it, regular (finite at k'
    // = k: (ψ(1/2) - ψ(L + 1/2))/(πk)).
    const int nt = 24;
    std::vector<double> tx, tw;
    gaussLegendre(nt, tx, tw);
    std::vector<double> cosT(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t) cosT[std::size_t(t)] = std::cos(0.5 * M_PI * (tx[std::size_t(t)] + 1));
    int Lmax = 0;
    for (const Table& t : tables) Lmax = std::max(Lmax, t.L);
    std::vector<std::vector<double>> cosL(static_cast<std::size_t>(Lmax + 1), std::vector<double>(static_cast<std::size_t>(nt)));
    for (int L = 0; L <= Lmax; ++L)
        for (int t = 0; t < nt; ++t) cosL[std::size_t(L)][std::size_t(t)] = 0.5 * tw[std::size_t(t)] * std::cos(L * 0.5 * M_PI * (tx[std::size_t(t)] + 1));
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(K, K);
    for (int i = 0; i < K; ++i)
        for (int j = 0; j < K; ++j)
            if (i != j) A(i, j) = coulombS(k[std::size_t(i)], k[std::size_t(j)]);
    std::vector<Eigen::MatrixXd> dA(static_cast<std::size_t>(Lmax + 1));
    for (int L = 1; L <= Lmax; ++L) {
        Eigen::MatrixXd& d = dA[std::size_t(L)];
        d.resize(K, K);
        double psi = 0;
        for (int m = 0; m < L; ++m) psi -= 2.0 / (2 * m + 1);
        for (int i = 0; i < K; ++i)
            for (int j = 0; j < K; ++j)
                d(i, j) = i == j ? psi / (M_PI * k[std::size_t(i)])
                                 : coulombChannel(L, k[std::size_t(i)], k[std::size_t(j)]) - coulombChannel(0, k[std::size_t(i)], k[std::size_t(j)]);
    }
    std::vector<double> dsub(static_cast<std::size_t>(K));
    const double whole = subtractionIntegral();
    for (int i = 0; i < K; ++i) {
        double s = k[std::size_t(i)] / (2 * M_PI) * whole;
        for (int l = 0; l < K; ++l)
            if (l != i) s -= W[std::size_t(l)] * A(i, l) * subtracted(k[std::size_t(l)] / k[std::size_t(i)]);
        dsub[std::size_t(i)] = s;
    }

    // The Hamiltonian: pair a at k_i and pair b at k_j coupled by -(e²/2ε)
    // c_a(i)† M c_b(j), M over the two pairs' holes: the Coulomb part's F(0)
    // by K's channel - the s channel's singularity subtracted, the pair's
    // own c_a(i)† F(0) c_b(i) at the point -, the regular part's channel's
    // angular mean. Hermitian (a hole's phase, any, its coefficients' and
    // amplitudes' alike); real when its holes are, then solved so.
    const double c = Q * Q / (2 * eps);
    const int N = P * K;
    Eigen::MatrixXcd H = Eigen::MatrixXcd::Zero(N, N);
    for (int p = 0; p < P; ++p)
        for (int i = 0; i < K; ++i) H(p * K + i, p * K + i) = pairs[std::size_t(p)].energy(k[std::size_t(i)]);
    {
        // Each electron pair's tables; its pairs.
        std::map<std::pair<int, int>, std::vector<int>> byElectrons;
        for (std::size_t ti = 0; ti < tables.size(); ++ti) byElectrons[{tables[ti].ea, tables[ti].eb}].push_back(int(ti));
        std::map<int, std::vector<int>> ofElectron;
        for (int p = 0; p < P; ++p) ofElectron[elec[std::size_t(p)]].push_back(p);
        // (Its F(0)s: all channels', and each channel's past the s.)
        struct Group {
            int ea, eb;
            std::vector<int> list;
            Eigen::MatrixXcd F0all;
            std::vector<Eigen::MatrixXcd> F0of;   // [L]
        };
        std::vector<Group> groups;
        for (auto& [ee, list] : byElectrons) {
            Group gr{ee.first, ee.second, list, Eigen::MatrixXcd::Zero(nb, nb), {}};
            gr.F0of.assign(std::size_t(Lmax + 1), Eigen::MatrixXcd::Zero(nb, nb));
            for (int ti : list) {
                const Table& t = tables[std::size_t(ti)];
                gr.F0all(t.sa, t.sb) += t.F0;
                gr.F0of[std::size_t(t.L)](t.sa, t.sb) += t.F0;
                if (t.sa != t.sb) {
                    gr.F0all(t.sb, t.sa) += std::conj(t.F0);
                    gr.F0of[std::size_t(t.L)](t.sb, t.sa) += std::conj(t.F0);
                }
            }
            groups.push_back(std::move(gr));
        }
        // (Each grid point's coefficients of each electron's pairs: rows.)
        auto rows = [&](int e, int i) {
            const std::vector<int>& ps = ofElectron[e];
            Eigen::MatrixXcd m(Eigen::Index(ps.size()), nb);
            for (std::size_t r = 0; r < ps.size(); ++r) m.row(Eigen::Index(r)) = coef[std::size_t(ps[r])].row(i);
            return m;
        };
        std::vector<int> lq(static_cast<std::size_t>(nt));
        std::vector<double> w0(static_cast<std::size_t>(nt)), wa(static_cast<std::size_t>(nt)), wb(static_cast<std::size_t>(nt));
        std::vector<cd> R(tables.size());
        Eigen::MatrixXcd M(nb, nb);
        for (int i = 0; i < K; ++i)
            for (int j = i; j < K; ++j) {
                const double ki = k[std::size_t(i)], kj = k[std::size_t(j)];
                // (Each angle's q: D there w0 D(0) + wa D_l + wb D_l+1.)
                for (int s = 0; s < nt; ++s) {
                    const double q = std::sqrt(std::max(0.0, ki * ki + kj * kj - 2 * ki * kj * cosT[std::size_t(s)]));
                    const std::size_t ss = std::size_t(s);
                    if (q < qlo) {
                        lq[ss] = 0;
                        w0[ss] = 1 - q / qlo;
                        wa[ss] = q / qlo;
                        wb[ss] = 0;
                    } else if (q >= qhi) {
                        lq[ss] = nq - 2;
                        w0[ss] = wa[ss] = 0;
                        wb[ss] = qhi / q;
                    } else {
                        const double u = std::log(q / qlo) / std::log(qhi / qlo) * (nq - 1);
                        const int l = std::clamp(int(u), 0, nq - 2);
                        const double f = std::clamp(u - l, 0.0, 1.0);
                        lq[ss] = l;
                        w0[ss] = 0;
                        wa[ss] = 1 - f;
                        wb[ss] = f;
                    }
                }
                for (std::size_t ti = 0; ti < tables.size(); ++ti) {
                    const Table& t = tables[ti];
                    const std::vector<double>& wL = cosL[std::size_t(t.L)];
                    cd v = 0;
                    for (int s = 0; s < nt; ++s) {
                        const std::size_t ss = std::size_t(s);
                        v += wL[ss] * (w0[ss] * t.D0 + wa[ss] * t.D[std::size_t(lq[ss])] + wb[ss] * t.D[std::size_t(lq[ss] + 1)]);
                    }
                    R[ti] = v;
                }
                for (const Group& gr : groups) {
                    M.setZero();
                    for (int ti : gr.list) {
                        const Table& t = tables[std::size_t(ti)];
                        M(t.sa, t.sb) += R[std::size_t(ti)];
                        if (t.sa != t.sb) M(t.sb, t.sa) += std::conj(R[std::size_t(ti)]);
                    }
                    if (i != j) M += A(i, j) * gr.F0all;
                    for (int L = 1; L <= Lmax; ++L) M += dA[std::size_t(L)](i, j) * gr.F0of[std::size_t(L)];
                    // (Both ways round when the electrons differ: their
                    // product the same.)
                    for (int way = 0; way < (gr.ea == gr.eb ? 1 : 2); ++way) {
                        const int ea = way == 0 ? gr.ea : gr.eb, eb = way == 0 ? gr.eb : gr.ea;
                        const std::vector<int>& pa = ofElectron[ea];
                        const std::vector<int>& pb = ofElectron[eb];
                        const Eigen::MatrixXcd X = rows(ea, i).conjugate() * M;
                        const Eigen::MatrixXcd Wm = X * rows(eb, j).transpose();
                        Eigen::MatrixXcd O;
                        if (i == j) O = rows(ea, i).conjugate() * gr.F0all * rows(eb, i).transpose();
                        for (std::size_t x = 0; x < pa.size(); ++x)
                            for (std::size_t y = 0; y < pb.size(); ++y) {
                                const int a = pa[x], b = pb[y];
                                if (!needed(a, b)) continue;
                                cd h = -c * S[std::size_t(i)] * S[std::size_t(j)] * Wm(Eigen::Index(x), Eigen::Index(y));
                                if (i == j) h -= c * dsub[std::size_t(i)] * O(Eigen::Index(x), Eigen::Index(y));
                                H(a * K + i, b * K + j) += h;
                                if (i != j) H(b * K + j, a * K + i) += std::conj(h);
                            }
                    }
                }
            }
    }

    // Its states, and each's optical strength: P = Σ A(k) √W u, |P|² (the
    // exciton's |ψ(0)|² times its amplitude's square, summed coherently over
    // the pairs; u, of the Hamiltonian as it is, against the amplitudes as
    // they are).
    double largest = 0, imaginary = 0;
    for (Eigen::Index cc = 0; cc < N; ++cc)
        for (Eigen::Index rr = 0; rr < N; ++rr) {
            largest = std::max(largest, std::abs(H(rr, cc).real()));
            imaginary = std::max(imaginary, std::abs(H(rr, cc).imag()));
        }
    r.complex = imaginary > 1e-13 * largest;
    Eigen::MatrixXcd B = Eigen::MatrixXcd::Zero(N, C);
    std::vector<std::complex<double>> amp(static_cast<std::size_t>(C));
    for (int p = 0; p < P; ++p)
        for (int i = 0; i < K; ++i) {
            std::fill(amp.begin(), amp.end(), 0.0);
            pairs[std::size_t(p)].amplitude(k[std::size_t(i)], amp);
            for (int cc = 0; cc < C; ++cc) B(p * K + i, cc) = S[std::size_t(i)] * amp[std::size_t(cc)];
        }
    Eigen::MatrixXcd proj;
    // (Each state's largest parts: those holding 90 % of it, 8 at most,
    // each 1 % at least.)
    std::vector<std::vector<std::pair<int, double>>> parts(static_cast<std::size_t>(N));
    auto mainParts = [N, &parts](int nn, const auto& column) {
        std::vector<std::pair<double, int>> big;
        double most = 0;
        for (int row = 0; row < N; ++row) most = std::max(most, std::norm(column(row)));
        for (int row = 0; row < N; ++row)
            if (std::norm(column(row)) >= 0.01 * most) big.emplace_back(std::norm(column(row)), row);
        std::sort(big.rbegin(), big.rend());
        double held = 0;
        for (const auto& [w, row] : big) {
            if (held >= 0.9 || parts[std::size_t(nn)].size() >= 8) break;
            parts[std::size_t(nn)].emplace_back(row, w);
            held += w;
        }
    };
    if (r.complex) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> es(H);
        if (es.info() != Eigen::Success) {
            r.note = tr("the excitons' Hamiltonian did not diagonalize");
            return r;
        }
        r.energy.assign(es.eigenvalues().data(), es.eigenvalues().data() + N);
        proj = es.eigenvectors().transpose() * B;
        for (int nn = 0; nn < N; ++nn) mainParts(nn, es.eigenvectors().col(nn));
    } else {
        const Eigen::MatrixXd Hr = H.real();
        H.resize(0, 0);
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Hr);
        if (es.info() != Eigen::Success) {
            r.note = tr("the excitons' Hamiltonian did not diagonalize");
            return r;
        }
        r.energy.assign(es.eigenvalues().data(), es.eigenvalues().data() + N);
        const Eigen::MatrixXd pr = es.eigenvectors().transpose() * B.real(), pi = es.eigenvectors().transpose() * B.imag();
        proj = pr.cast<cd>() + cd(0, 1) * pi.cast<cd>();
        for (int nn = 0; nn < N; ++nn) mainParts(nn, es.eigenvectors().col(nn));
    }
    // Each state's cells: each of its largest parts' - a pair at a k point -
    // share of the plane, k from kl to kh (kh² - kl² = 4π W, from k 0 on: the
    // grid's points' shares fill it), its energies there, moved by the
    // state's own shift from the pair's at the point, weighted by the part's
    // share of the state - a continuum's state stands for its pairs' density
    // over them (without the attraction the free continuum's cells exactly;
    // a state of several pairs' continua spread over their cells alike, not
    // its largest's alone: that, where continua mix, rippled as much as the
    // line was narrow).
    std::vector<double> kl(static_cast<std::size_t>(K)), kh(static_cast<std::size_t>(K));
    {
        std::vector<int> order(static_cast<std::size_t>(K));
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&k](int a, int b) { return k[std::size_t(a)] < k[std::size_t(b)]; });
        double below = 0;
        for (int i : order) {
            kl[std::size_t(i)] = below;
            below = std::sqrt(below * below + 4 * M_PI * W[std::size_t(i)]);
            kh[std::size_t(i)] = below;
        }
    }
    r.cells.assign(std::size_t(N), {});
    for (int nn = 0; nn < N; ++nn) {
        double all = 0;
        for (const auto& [row, w] : parts[std::size_t(nn)]) all += w;
        for (const auto& [row, w] : parts[std::size_t(nn)]) {
            const int p = row / K, i = row % K;
            const auto& E = pairs[std::size_t(p)].energy;
            const double shift = r.energy[std::size_t(nn)] - E(k[std::size_t(i)]);
            const double a = E(kl[std::size_t(i)]) + shift, b = E(kh[std::size_t(i)]) + shift;
            r.cells[std::size_t(nn)].push_back({std::min(a, b), std::max(a, b), w / all});
        }
    }
    r.strength.assign(std::size_t(C), std::vector<double>(std::size_t(N), 0.0));
    for (int cc = 0; cc < C; ++cc)
        for (int nn = 0; nn < N; ++nn) r.strength[std::size_t(cc)][std::size_t(nn)] = std::norm(proj(nn, cc));

    // Past kmax: each pair's free continuum, k/(2π |dE/dk|) |A|² - the
    // states above the least pair's top the tail's, coarse: not lines.
    r.top.assign(std::size_t(P), 0.0);
    r.beyond.assign(std::size_t(P), std::vector<double>(std::size_t(C), 0.0));
    for (int p = 0; p < P; ++p) {
        const auto& E = pairs[std::size_t(p)].energy;
        const double top = E(kmax), slope = (E(1.005 * kmax) - E(0.995 * kmax)) / (0.01 * kmax);
        r.top[std::size_t(p)] = top;
        if (!(slope > 0)) continue;
        std::fill(amp.begin(), amp.end(), 0.0);
        pairs[std::size_t(p)].amplitude(kmax, amp);
        for (int cc = 0; cc < C; ++cc) r.beyond[std::size_t(p)][std::size_t(cc)] = std::norm(amp[std::size_t(cc)]) * kmax / (2 * M_PI * slope);
    }
    r.cut = *std::min_element(r.top.begin(), r.top.end());
    return r;
}

} // namespace qucs_s::fem::exciton
