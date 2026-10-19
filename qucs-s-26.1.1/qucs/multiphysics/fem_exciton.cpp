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
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

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

} // namespace

double ellipticK(double m)
{
    if (!(m < 1)) return std::numeric_limits<double>::infinity();
    return M_PI / (2 * agm(std::sqrt(1 - m)));
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

    // The pairs' form factors F_ab(q) = ∫∫ ρe(ze) ρh(zh) e^(-q|ze - zh|), ρe
    // their electrons' product, ρh their holes' (summed over its components),
    // tabulated as D(q) = (F(q) - F(0))/q - regular: within a pair F(0) is
    // 1 (its Coulomb part, singular, apart), between pairs 0 - on q from
    // kmax/1e4 to 2.1 kmax (ln q), D(0) = -<|ze - zh|>.
    const int nq = 240;
    const double qlo = 1e-4 * kmax, qhi = 2.1 * kLast;
    std::vector<double> qs(static_cast<std::size_t>(nq));
    for (int l = 0; l < nq; ++l) qs[std::size_t(l)] = qlo * std::pow(qhi / qlo, double(l) / (nq - 1));
    struct Table {
        double D0 = 0;
        std::vector<double> D;
    };
    std::map<std::pair<int, int>, Table> tables;
    auto needed = [&](int a, int b) { return a == b || o.coupled; };
    for (int a = 0; a < P; ++a)
        for (int b = a; b < P; ++b)
            if (needed(a, b)) tables[{a, b}].D.assign(std::size_t(nq), 0.0);
    if (o.formFactor) {
        for (auto& [ab, t] : tables) {
            const double F0 = o.formFactor(ab.first, ab.second, 0);
            const double qs0 = qlo * 1e-3;
            t.D0 = (o.formFactor(ab.first, ab.second, qs0) - F0) / qs0;
            for (int l = 0; l < nq; ++l) t.D[std::size_t(l)] = (o.formFactor(ab.first, ab.second, qs[std::size_t(l)]) - F0) / qs[std::size_t(l)];
        }
    } else {
        const std::vector<double>& z = envelopes.z;
        const std::size_t n = z.size();
        auto electron = [&](int a, int b) {
            std::vector<double> rho(n);
            const auto& x = envelopes.electrons[std::size_t(pairs[std::size_t(a)].electron)];
            const auto& y = envelopes.electrons[std::size_t(pairs[std::size_t(b)].electron)];
            for (std::size_t p = 0; p < n; ++p) rho[p] = x[p] * y[p];
            return rho;
        };
        auto hole = [&](int a, int b) {
            std::vector<double> rho(n, 0.0);
            const auto& x = envelopes.holes[std::size_t(pairs[std::size_t(a)].hole)];
            const auto& y = envelopes.holes[std::size_t(pairs[std::size_t(b)].hole)];
            for (std::size_t c = 0; c < x.size() && c < y.size(); ++c)
                for (std::size_t p = 0; p < n; ++p) rho[p] += (std::conj(x[c][p]) * y[c][p]).real();
            return rho;
        };
        // (Each hole pair's densities once; its convolution with e^(-q|z|) in
        // two sweeps, O(n).)
        std::map<std::pair<int, int>, std::vector<double>> holes;
        std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> users;
        std::map<std::pair<int, int>, std::vector<double>> electrons;
        for (auto& [ab, t] : tables) {
            const int ha = pairs[std::size_t(ab.first)].hole, hb = pairs[std::size_t(ab.second)].hole;
            const std::pair<int, int> hk{ha, hb};
            if (!holes.count(hk)) holes[hk] = hole(ab.first, ab.second);
            users[hk].push_back(ab);
            electrons[ab] = electron(ab.first, ab.second);
            // D(0) = -Σp ρe_p Σr ρh_r |z_p - z_r|, by the holes' sums below
            // and above.
            const std::vector<double>& rh = holes[hk];
            const std::vector<double>& re = electrons[ab];
            double below = 0, belowZ = 0, all = 0, allZ = 0, d0 = 0;
            for (std::size_t p = 0; p < n; ++p) {
                all += rh[p];
                allZ += rh[p] * z[p];
            }
            for (std::size_t p = 0; p < n; ++p) {
                below += rh[p];
                belowZ += rh[p] * z[p];
                d0 += re[p] * (z[p] * below - belowZ + (allZ - belowZ) - z[p] * (all - below));
            }
            t.D0 = -d0;
        }
        std::vector<double> g(n);
        std::map<std::pair<int, int>, double> zero;
        for (auto& [ab, t] : tables) {
            const int ha = pairs[std::size_t(ab.first)].hole, hb = pairs[std::size_t(ab.second)].hole;
            double se = 0, sh = 0;
            for (double v : electrons[ab]) se += v;
            for (double v : holes[{ha, hb}]) sh += v;
            zero[ab] = se * sh;
        }
        for (int l = 0; l < nq; ++l) {
            const double q = qs[std::size_t(l)];
            for (auto& [hk, rh] : holes) {
                double L = 0;
                for (std::size_t p = 0; p < n; ++p) {
                    L = (p > 0 ? L * std::exp(-q * (z[p] - z[p - 1])) : 0.0) + rh[p];
                    g[p] = L;
                }
                double R = 0;
                for (std::size_t p = n; p-- > 0;) {
                    if (p + 1 < n) R = (R + rh[p + 1]) * std::exp(-q * (z[p + 1] - z[p]));
                    g[p] += R;
                }
                for (const auto& ab : users[hk]) {
                    const std::vector<double>& re = electrons[ab];
                    double F = 0;
                    for (std::size_t p = 0; p < n; ++p) F += re[p] * g[p];
                    tables[ab].D[std::size_t(l)] = (F - zero[ab]) / q;
                }
            }
        }
    }
    auto Dof = [&](const Table& t, double q) {
        if (q < qlo) return t.D0 + (t.D[0] - t.D0) * q / qlo;
        if (q >= qhi) return t.D[std::size_t(nq - 1)] * qhi / q;
        const double u = std::log(q / qlo) / std::log(qhi / qlo) * (nq - 1);
        const int l = std::clamp(int(u), 0, nq - 2);
        const double f = std::clamp(u - l, 0.0, 1.0);
        return t.D[std::size_t(l)] + f * (t.D[std::size_t(l + 1)] - t.D[std::size_t(l)]);
    };

    // The regular part's angular average, ∫ dθ/(2π) D(|k - k'|), by
    // Gauss-Legendre on [0, π]; the Coulomb part's (2/π) K/(k + k') with
    // its singularity subtracted.
    const int nt = 24;
    std::vector<double> tx, tw;
    gaussLegendre(nt, tx, tw);
    std::vector<double> cosT(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t) cosT[std::size_t(t)] = std::cos(0.5 * M_PI * (tx[std::size_t(t)] + 1));
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(K, K);
    for (int i = 0; i < K; ++i)
        for (int j = 0; j < K; ++j)
            if (i != j) A(i, j) = coulombS(k[std::size_t(i)], k[std::size_t(j)]);
    std::vector<double> dsub(static_cast<std::size_t>(K));
    const double whole = subtractionIntegral();
    for (int i = 0; i < K; ++i) {
        double s = k[std::size_t(i)] / (2 * M_PI) * whole;
        for (int l = 0; l < K; ++l)
            if (l != i) s -= W[std::size_t(l)] * A(i, l) * subtracted(k[std::size_t(l)] / k[std::size_t(i)]);
        dsub[std::size_t(i)] = s;
    }
    const double c = Q * Q / (2 * eps);
    const int N = P * K;
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(N, N);
    for (int p = 0; p < P; ++p)
        for (int i = 0; i < K; ++i) H(p * K + i, p * K + i) = pairs[std::size_t(p)].energy(k[std::size_t(i)]);
    Eigen::MatrixXd R(K, K);
    for (auto& [ab, t] : tables) {
        for (int i = 0; i < K; ++i)
            for (int j = i; j < K; ++j) {
                const double ki = k[std::size_t(i)], kj = k[std::size_t(j)];
                double avg = 0;
                for (int s = 0; s < nt; ++s)
                    avg += 0.5 * tw[std::size_t(s)] * Dof(t, std::sqrt(std::max(0.0, ki * ki + kj * kj - 2 * ki * kj * cosT[std::size_t(s)])));
                R(i, j) = R(j, i) = avg;
            }
        const int a = ab.first, b = ab.second;
        for (int i = 0; i < K; ++i)
            for (int j = 0; j < K; ++j) {
                double v = R(i, j);
                if (a == b && i != j) v += A(i, j);
                const double h = -c * S[std::size_t(i)] * S[std::size_t(j)] * v;
                H(a * K + i, b * K + j) += h;
                if (a != b) H(b * K + j, a * K + i) += h;
            }
        if (a == b)
            for (int i = 0; i < K; ++i) H(a * K + i, a * K + i) -= c * dsub[std::size_t(i)];
    }

    // Its states, and each's optical strength: P = Σ A(k) √W u, |P|² (the
    // exciton's |ψ(0)|² times its amplitude's square, summed coherently over
    // the pairs).
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(H);
    if (es.info() != Eigen::Success) {
        r.note = tr("the excitons' Hamiltonian did not diagonalize");
        return r;
    }
    r.energy.assign(es.eigenvalues().data(), es.eigenvalues().data() + N);
    Eigen::MatrixXd B = Eigen::MatrixXd::Zero(N, 2 * C);
    std::vector<std::complex<double>> amp(static_cast<std::size_t>(C));
    for (int p = 0; p < P; ++p)
        for (int i = 0; i < K; ++i) {
            std::fill(amp.begin(), amp.end(), 0.0);
            pairs[std::size_t(p)].amplitude(k[std::size_t(i)], amp);
            for (int cc = 0; cc < C; ++cc) {
                B(p * K + i, 2 * cc) = S[std::size_t(i)] * amp[std::size_t(cc)].real();
                B(p * K + i, 2 * cc + 1) = S[std::size_t(i)] * amp[std::size_t(cc)].imag();
            }
        }
    const Eigen::MatrixXd proj = es.eigenvectors().transpose() * B;
    // Each state's cell: its largest part's - a pair at a k point - share of
    // the plane, k from kl to kh (kh² - kl² = 4π W, from k 0 on: the grid's
    // points' shares fill it), its energies there, moved by the state's own
    // shift from the pair's at the point - a continuum's state stands for its
    // pair's density over it (where pairs' continua interleave, each its
    // own; without the attraction the free continuum's cells exactly).
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
    r.lo.assign(std::size_t(N), 0.0);
    r.hi.assign(std::size_t(N), 0.0);
    for (int nn = 0; nn < N; ++nn) {
        Eigen::Index best = 0;
        es.eigenvectors().col(nn).cwiseAbs().maxCoeff(&best);
        const int p = int(best) / K, i = int(best) % K;
        const auto& E = pairs[std::size_t(p)].energy;
        const double shift = r.energy[std::size_t(nn)] - E(k[std::size_t(i)]);
        const double a = E(kl[std::size_t(i)]) + shift, b = E(kh[std::size_t(i)]) + shift;
        r.lo[std::size_t(nn)] = std::min(a, b);
        r.hi[std::size_t(nn)] = std::max(a, b);
    }
    r.strength.assign(std::size_t(C), std::vector<double>(std::size_t(N), 0.0));
    for (int cc = 0; cc < C; ++cc)
        for (int nn = 0; nn < N; ++nn) r.strength[std::size_t(cc)][std::size_t(nn)] = proj(nn, 2 * cc) * proj(nn, 2 * cc) + proj(nn, 2 * cc + 1) * proj(nn, 2 * cc + 1);

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
