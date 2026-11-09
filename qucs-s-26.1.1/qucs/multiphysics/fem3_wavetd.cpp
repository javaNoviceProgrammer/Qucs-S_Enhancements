/*
 * fem3_wavetd.cpp - light through a 3D model's devices in the time domain
 *                   by the finite element method (fem3_wave.h): one pulse,
 *                   the S-parameters at every wavelength asked
 *
 * The curl-curl equation of the frequency domain, A(p) E = F with p = jk0 -
 * ∫ µ⁻¹ Λ⁻¹ ∇×W·∇×V + p² ε Λ W·V, Λ the PMLs' stretching (s = 1 + σ/p, σ
 * the same at every frequency) -, stepped in time by the trapezoidal rule:
 * p replaced by p0 (1 - q)/(1 + q), q a step's delay and p0 = 2/(c Δt) -
 * Newmark's average acceleration where A is a polynomial, and of the PMLs'
 * and the materials' rational terms their own recursions, each at a
 * quadrature point. So the steps' Fourier transform at an angular frequency
 * ω is the frequency domain's solution at ω' = (2/Δt) tan(ωΔt/2), exactly:
 * the steps' error a known shift of the frequency, not an inaccuracy - the
 * run sampled where ω' is each frequency asked. Each step solves A(p0), the
 * frequency domain's matrix at a real p: symmetric, positive definite - a
 * Cholesky, factorized once.
 *
 * The ports must have a PML behind them: each fed port mode a sheet of
 * current on its faces with a Gaussian pulse in time, every port's field on
 * the device's side transformed at the frequencies and split into its
 * modes' incoming and outgoing waves there.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_wave.h"
#include "fem3_wave_setup.h"

#include "fem3_fe.h"
#include "fem3_geometry.h"
#include "fem_results.h"
#include "fem_solver.h"
#include "fem_sparse.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

namespace qucs_s::fem {

namespace {

using cd = std::complex<double>;
constexpr double C0 = 299792458.0;
const cd J(0, 1);

QString tr(const char* text)
{
    return QCoreApplication::translate("Wave3", text);
}

// ------------------------------------------------- rational functions of p

/// A polynomial's coefficients, the lowest first.
using Poly = std::vector<double>;

Poly times(const Poly& a, const Poly& b)
{
    Poly c(a.size() + b.size() - 1, 0.0);
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t k = 0; k < b.size(); ++k) c[i + k] += a[i] * b[k];
    return c;
}

/// A recursion of the steps: y = B(q)/A(q) u, A(0) = 1 - as a transposed
/// direct form, its states K = the larger degree.
struct Filter {
    int K = 0;
    double b[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    double a[9] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
};

/// f(p) = N(p)/D(p) - times the steps' (1 + q)² - as the trapezoidal rule
/// makes it: p = p0 (1 - q)/(1 + q). B(0)/A(0) = f(p0).
Filter bilinear(Poly N, Poly D, double p0)
{
    // (Common factors of p first.)
    while (N.size() > 1 && D.size() > 1 && N.front() == 0 && D.front() == 0) {
        N.erase(N.begin());
        D.erase(D.begin());
    }
    while (N.size() > 1 && N.back() == 0) N.pop_back();
    while (D.size() > 1 && D.back() == 0) D.pop_back();
    const int nN = int(N.size()) - 1, nD = int(D.size()) - 1;
    // Σ c_k p0^k (1 - q)^k (1 + q)^(n - k).
    auto mapped = [&](const Poly& c, int n) {
        Poly out(std::size_t(n) + 1, 0.0);
        for (int k = 0; k <= n && k < int(c.size()); ++k) {
            Poly t{std::pow(p0, k) * c[std::size_t(k)]};
            for (int i = 0; i < k; ++i) t = times(t, Poly{1, -1});
            for (int i = 0; i < n - k; ++i) t = times(t, Poly{1, 1});
            for (std::size_t i = 0; i < t.size(); ++i) out[i] += t[i];
        }
        return out;
    };
    Poly B = mapped(N, nN), A = mapped(D, nD);
    for (int i = 0; i < 2 + nD - nN; ++i) B = times(B, Poly{1, 1});
    Filter f;
    const double a0 = A[0];
    f.K = std::max(int(B.size()), int(A.size())) - 1;
    for (std::size_t i = 0; i < B.size() && i < 9; ++i) f.b[i] = B[i] / a0;
    for (std::size_t i = 0; i < A.size() && i < 9; ++i) f.a[i] = A[i] / a0;
    return f;
}

/// Π (p + r) over \a roots.
Poly ofRoots(const std::vector<double>& roots)
{
    Poly p{1};
    for (double r : roots) p = times(p, Poly{r, 1});
    return p;
}

/// A material's ε(p) as N/D: ε∞ + Δε W²/(p² + G p + W²) + σ/p - a Lorentz
/// pole (none where it is not dispersive) and a conductivity.
struct Medium {
    Poly N{1}, D{1};
    bool pole = false;
    double einf = 1, de = 0, W = 0, G = 0, sigma = 0;
    double worst = 0;   ///< the fit's largest |Δn| at the frequencies asked
};

/// ε of a medium at p.
cd mediumAt(const Medium& m, cd p)
{
    cd num = 0, den = 0, pk = 1;
    for (std::size_t i = 0; i < std::max(m.N.size(), m.D.size()); ++i, pk *= p) {
        if (i < m.N.size()) num += m.N[i] * pk;
        if (i < m.D.size()) den += m.D[i] * pk;
    }
    return num / den;
}

/// A medium from its ε at wavenumbers \a k (1/m) - the frequencies asked -:
/// its ε∞ and conductivity alone where they fit to 2e-6 in the index, else
/// with the Lorentz pole that fits best - its W and G on a grid (W from a
/// thirtieth of the band's middle to thirty times it, outside the band; G
/// from nought), ε∞, Δε and σ (≥ 0) by least squares at each.
Medium fitMedium(const std::vector<double>& k, const std::vector<cd>& eps, double kMid)
{
    const int M = int(k.size());
    auto worstOf = [&](const Medium& m) {
        double w = 0;
        for (int i = 0; i < M; ++i) w = std::max(w, std::abs(std::sqrt(mediumAt(m, cd(0, k[std::size_t(i)]))) - std::sqrt(eps[std::size_t(i)])));
        return w;
    };
    auto build = [&](Medium& m) {
        // ε∞ p (p² + Gp + W²) + Δε W² p + σ (p² + Gp + W²) over p (p² + Gp + W²).
        if (m.pole) {
            const Poly q{m.W * m.W, m.G, 1};
            Poly N = times(Poly{0, m.einf}, q);
            N[1] += m.de * m.W * m.W;
            if (m.sigma > 0) {
                for (std::size_t i = 0; i < q.size(); ++i) N[i] += m.sigma * q[i];
                m.N = N;
                m.D = times(Poly{0, 1}, q);
            } else {
                m.N = Poly(N.begin() + 1, N.end());
                m.D = q;
            }
        } else if (m.sigma > 0) {
            m.N = {m.sigma, m.einf};
            m.D = {0, 1};
        } else {
            m.N = {m.einf};
            m.D = {1};
        }
    };
    // (Least squares of ε∞, Δε (with a pole) and σ: the real parts' rows,
    // then the imaginary parts'.)
    auto solve = [&](bool pole, double W, double G, Medium& m) {
        const int cols = pole ? 3 : 2;
        Eigen::MatrixXd A = Eigen::MatrixXd::Zero(2 * M, cols);
        Eigen::VectorXd b(2 * M);
        for (int i = 0; i < M; ++i) {
            const double x = k[std::size_t(i)] / kMid;
            A(i, 0) = 1;
            b(i) = eps[std::size_t(i)].real();
            b(M + i) = eps[std::size_t(i)].imag();
            A(M + i, cols - 1) = -1 / x;   // σ/p at p = jk: -jσ/k (σ in units of kMid)
            if (pole) {
                const cd L = W * W / cd(W * W - x * x, G * x);
                A(i, 1) = L.real();
                A(M + i, 1) = L.imag();
            }
        }
        Eigen::VectorXd c = A.colPivHouseholderQr().solve(b);
        if (c(cols - 1) < 0) {   // (no gain: σ nought)
            c.head(cols - 1) = A.leftCols(cols - 1).colPivHouseholderQr().solve(b);
            c(cols - 1) = 0;
        }
        m.pole = pole;
        m.einf = c(0);
        m.de = pole ? c(1) : 0;
        m.W = W * kMid;
        m.G = G * kMid;
        m.sigma = c(cols - 1) * kMid;
        if (m.sigma < 1e-12 * kMid * std::abs(m.einf)) m.sigma = 0;   // (a lossless one's rounding)
        return (A * c - b).squaredNorm();
    };
    Medium plain;
    solve(false, 0, 0, plain);
    build(plain);
    plain.worst = worstOf(plain);
    if (plain.worst <= 2e-6 || M < 3) return plain;
    Medium best;
    double bestErr = 1e300;
    for (int iw = 0; iw <= 80; ++iw) {
        const double W = std::pow(30.0, (iw - 40) / 40.0);   // 1/30 to 30
        // (Outside the band, with a margin.)
        const double xLo = k.front() / kMid, xHi = k.back() / kMid;
        if (W > 0.8 * std::min(xLo, xHi) && W < 1.25 * std::max(xLo, xHi)) continue;
        for (int ig = -1; ig <= 20; ++ig) {
            const double G = ig < 0 ? 0.0 : 1e-4 * std::pow(1e4, ig / 20.0);
            Medium m;
            const double err = solve(true, W, G, m);
            if (err < bestErr) {
                bestErr = err;
                best = m;
            }
        }
    }
    build(best);
    best.worst = worstOf(best);
    return best.worst < plain.worst ? best : plain;
}

/// A PML's (or none's: σ nought) term along axis \a a at a point: of the
/// curl (1/Λ_a) or of the mass (p² ε Λ_a), Λ_a = s_b s_c / s_a.
Filter pmlFilter(bool curl, int a, const double sg[3], const Medium& m, double p0)
{
    const double sa = sg[a], sb = sg[(a + 1) % 3], sc = sg[(a + 2) % 3];
    std::vector<double> num, den;
    if (curl) {
        num = {0, sa};
        den = {sb, sc};
    } else {
        num = {0, sb, sc};
        den = {sa};
    }
    // (Equal roots cancelled: no step's state for them.)
    for (auto it = num.begin(); it != num.end();) {
        auto at = std::find(den.begin(), den.end(), *it);
        if (at != den.end()) {
            den.erase(at);
            it = num.erase(it);
        } else
            ++it;
    }
    Poly N = ofRoots(num), D = ofRoots(den);
    if (!curl) {
        N = times(N, m.N);
        D = times(D, m.D);
    }
    return bilinear(N, D, p0);
}

/// A filter packed: its K at most \a K, b0..bK then a1..aK (zeros past its
/// own order) - 2K + 1 doubles.
inline void pack(const Filter& f, int K, double* out)
{
    for (int k = 0; k <= K; ++k) out[k] = k <= f.K ? f.b[k] : 0.0;
    for (int k = 1; k <= K; ++k) out[K + k] = k <= f.K ? f.a[k] : 0.0;
}

/// Steps a packed filter of order \a K: its output's next history.
inline double stepPacked(const double* c, int K, double* s, double u)
{
    const double y = c[0] * u + s[0];
    for (int k = 1; k <= K; ++k) s[k - 1] = c[k] * u - c[K + k] * y + (k < K ? s[k] : 0.0);
    return s[0];
}

/// Threads kept for the steps: each step's parts handed to them - not a
/// thread started a part, thousands of steps.
class StepPool
{
public:
    explicit StepPool(int threads)
    {
        for (int t = 1; t < threads; ++t) a_workers.emplace_back([this] { loop(); });
    }
    ~StepPool()
    {
        {
            std::lock_guard<std::mutex> lock(a_mutex);
            a_stop = true;
        }
        a_wake.notify_all();
        for (std::thread& t : a_workers) t.join();
    }
    int threads() const { return int(a_workers.size()) + 1; }
    /// \a work(k) for each k in [0, tasks), on every thread: back when all are done.
    void run(int tasks, const std::function<void(int)>& work)
    {
        {
            std::lock_guard<std::mutex> lock(a_mutex);
            a_work = &work;
            a_tasks = tasks;
            a_next = 0;
            a_busy = int(a_workers.size());
            ++a_generation;
        }
        a_wake.notify_all();
        take(work, tasks);
        std::unique_lock<std::mutex> lock(a_mutex);
        a_done.wait(lock, [this] { return a_busy == 0; });
        a_work = nullptr;
    }

private:
    void take(const std::function<void(int)>& work, int tasks)
    {
        for (int k = a_next++; k < tasks; k = a_next++) work(k);
    }
    void loop()
    {
        unsigned seen = 0;
        for (;;) {
            const std::function<void(int)>* work = nullptr;
            int tasks = 0;
            {
                std::unique_lock<std::mutex> lock(a_mutex);
                a_wake.wait(lock, [&] { return a_stop || a_generation != seen; });
                if (a_stop) return;
                seen = a_generation;
                work = a_work;
                tasks = a_tasks;
            }
            take(*work, tasks);
            {
                std::lock_guard<std::mutex> lock(a_mutex);
                if (--a_busy == 0) a_done.notify_one();
            }
        }
    }
    std::vector<std::thread> a_workers;
    std::mutex a_mutex;
    std::condition_variable a_wake, a_done;
    const std::function<void(int)>* a_work = nullptr;
    std::atomic<int> a_next{0};
    int a_tasks = 0, a_busy = 0;
    unsigned a_generation = 0;
    bool a_stop = false;
};

} // namespace

Wave3TimeResult solveWave3Time(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology,
                               std::shared_ptr<const Mesh3> mesh, const Wave3Settings& asked, const std::vector<double>& frequencies)
{
    Wave3TimeResult out;
    // (Every wavelength's ports' cuts of one mesh, its band's shortest
    // wavelength's: each its own made their modes' discretization jump from
    // one wavelength to the next - a Y-branch's |S21|² by 2e-4.)
    Wave3Settings settings = asked;
    if (settings.portMeshWavelength <= 0 && !frequencies.empty())
        settings.portMeshWavelength = C0 / *std::max_element(frequencies.begin(), frequencies.end());
    QElapsedTimer timer;
    timer.start();
    auto tell = [&](double f, const QString& what) {
        if (settings.progress) settings.progress(f, what);
    };
    auto cancelled = [&] { return settings.cancelled && settings.cancelled(); };
    if (frequencies.empty()) {
        out.error = tr("no frequency asked");
        return out;
    }
    if (!parameters) parameters = evaluateParameters(model);
    // The band: its middle the reference - the materials' ε there, the
    // sources' profiles its modes'.
    const double fLo = *std::min_element(frequencies.begin(), frequencies.end()), fHi = *std::max_element(frequencies.begin(), frequencies.end());
    const double fMid = 0.5 * (fLo + fHi);
    wave3::Setup su;
    {
        QString why;
        const bool ok = wave3::setUp(model, *parameters, topology, mesh, settings, C0 / fHi, &su, &why);
        out.log << su.log;
        out.warnings << su.warnings;
        if (!ok) {
            out.error = why;
            return out;
        }
        // (ε at the band's middle, the mesh checked at its shortest wavelength.)
        for (std::size_t d = 0; d < su.material.size(); ++d)
            if (su.material[d]) su.eps[d] = relativePermittivity(*su.material[d], C0 / fMid, parameters->scope);
        su.pmlIndex = 0;
        for (int d : su.pml)
            if (d >= 0 && d < int(su.eps.size())) {
                const double n = std::sqrt(std::max(1.0, su.eps[std::size_t(d)].real()));
                su.pmlIndex = su.pmlIndex > 0 ? std::min(su.pmlIndex, n) : n;
            }
        if (!(su.pmlIndex > 0)) su.pmlIndex = 1;
    }
    for (std::size_t pi = 0; pi < su.ports.size(); ++pi)
        if (!su.ports[pi].backed) {
            out.error = tr("port %1 is on the geometry's outside: in the time domain a port needs a PML behind it - its waveguide going on "
                           "into a Perfectly Matched Layer domain, the port on the face between them")
                            .arg(settings.ports[pi].name);
            return out;
        }
    const Mesh3& m3 = *mesh;
    const EdgeSpace3& space = *su.space;
    const int N = su.N, per = space.perElement(), Ne = space.elements();
    const std::vector<char>& fixed = su.fixed;
    // The step: so many a period at the band's middle; p0 = 2/(c Δt). The
    // run's angular frequencies whose transform is each asked one's.
    const double dt = 1.0 / (fMid * std::max(4.0, settings.stepsPerPeriod));
    const double p0 = 2.0 / (C0 * dt);
    std::vector<double> omega;
    for (double f : frequencies) omega.push_back(2.0 / dt * std::atan(2 * M_PI * f * dt / 2));
    out.dt = dt;
    for (double w : omega) out.sampled.push_back(w / (2 * M_PI));
    // Each domain's medium: its material's ε at each frequency asked, fitted
    // (the steps' transforms are the frequency domain's of the fit, there).
    const double kMid = 2 * M_PI * fMid / C0;
    std::vector<Medium> medium(su.eps.size());
    {
        std::vector<double> ks;
        for (double f : frequencies) ks.push_back(2 * M_PI * f / C0);
        std::sort(ks.begin(), ks.end());
        ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
        std::map<const Node*, Medium> fitted;
        for (std::size_t d = 0; d < su.eps.size(); ++d) {
            const Node* mat = su.material[d];
            if (!mat) continue;
            auto it = fitted.find(mat);
            if (it == fitted.end()) {
                std::vector<cd> e;
                for (double kk : ks) e.push_back(relativePermittivity(*mat, 2 * M_PI / kk, parameters->scope));
                Medium m = fitMedium(ks, e, kMid);
                it = fitted.emplace(mat, m).first;
                if (m.pole)
                    out.log << tr("%1: its ε fitted over the band as ε∞ %2 and a pole at %3 (Δε %4, damping %5)%6 - its index within %7 of the "
                                  "material's at each wavelength asked")
                                   .arg(mat->name())
                                   .arg(m.einf, 0, 'g', 6)
                                   .arg(formatQuantity(2 * M_PI / m.W, Dim::length()))
                                   .arg(m.de, 0, 'g', 4)
                                   .arg(m.G / m.W, 0, 'g', 3)
                                   .arg(m.sigma > 0 ? tr(" and a conductivity") : QString())
                                   .arg(m.worst, 0, 'g', 2);
                if (m.worst > 1e-4)
                    out.warnings << tr("%1: its ε is fitted over the band to %2 in its index at worst - a narrower band, or the frequency "
                                       "domain")
                                        .arg(mat->name())
                                        .arg(m.worst, 0, 'g', 2);
            }
            medium[d] = it->second;
            if (mediumAt(medium[d], cd(p0, 0)).real() <= 0) {
                out.error = tr("%1: its fitted ε is not positive at the steps' own frequency - a narrower band, or more steps a period")
                                .arg(mat->name());
                return out;
            }
        }
    }
    // The ports' modes at the band's middle: the sources' profiles (the
    // modes taken, fed), their probes.
    struct Fed {
        int port = -1, mode = -1;   // its port, its mode's index at the middle
        int taken = -1;             // its index among the port modes taken
        QString label;              // as its port names it
    };
    std::vector<Fed> fedList;
    std::vector<wave3::Column> probes;   // every taken port mode's u at the middle
    {
        int at = 0;
        for (std::size_t pi = 0; pi < su.ports.size(); ++pi) {
            const Wave3Port& wp = settings.ports[pi];
            QString why;
            const EmeModes mid = wave3::portModes(model, parameters, topology, su, settings, int(pi), fMid, int(wp.modes.size()) + 2, &why);
            if (!why.isEmpty() || mid.count() == 0) {
                out.error = tr("port %1: %2").arg(wp.name, why.isEmpty() ? tr("no mode found") : why);
                return out;
            }
            std::vector<wave3::Column> u;
            wave3::portColumns(su, su.ports[pi], mid, settings.order, kMid, su.eps, &u, nullptr);
            for (const QString& l : wp.modes) {
                const int k = mid.find(l);
                if (k < 0) {
                    out.error = tr("port %1 has no mode %2").arg(wp.name, l);
                    return out;
                }
                probes.push_back(u[std::size_t(k)]);
                if (wp.excite) fedList.push_back({int(pi), k, at, l});
                ++at;
            }
        }
    }
    if (fedList.empty()) {
        out.error = tr("no port is fed");
        return out;
    }
    const int P = int(fedList.size());
    tell(0.05, tr("ports' modes"));
    // The elements: the PMLs' each a quadrature point's recursions, the rest
    // the bulk - its recursions those of the matrices: the curl's (1 + q)²,
    // each medium's p² ε (a polynomial: no state of its own).
    const PrismRule& rule = settings.order == 1 ? prismRule(2, 2) : prismRule(4, 3);
    const int Q = int(rule.points.size());
    std::vector<Filter> bulkMass(medium.size());
    std::vector<int> dispersive;   // the domains whose bulk's mass is a recursion of its own (a pole)
    for (std::size_t d = 0; d < medium.size(); ++d) {
        bulkMass[d] = bilinear(times(Poly{0, 0, 1}, medium[d].N), medium[d].D, p0);
        bool iir = bulkMass[d].K > 2;
        for (int i = 1; i < 9; ++i) iir = iir || bulkMass[d].a[i] != 0;
        if (iir) dispersive.push_back(int(d));
    }
    const Filter bulkCurl = bilinear(Poly{1}, Poly{1}, p0);
    std::vector<int> pmlElements;
    std::vector<int> pmlIndex(std::size_t(Ne), -1);
    for (int e = 0; e < Ne; ++e)
        if (su.pml.count(m3.domain[std::size_t(e)])) {
            pmlIndex[std::size_t(e)] = int(pmlElements.size());
            pmlElements.push_back(e);
        }
    const int Np = int(pmlElements.size());
    // (A PML point's filters: 3 of the curl, 3 of the mass - their states at
    // most 4 and 3 (more with a medium's own).)
    // (Their orders at most: the curl's 4, the mass's 3 and its medium's
    // denominator's - packed at those.)
    int KM = 3;
    for (int e : pmlElements) KM = std::max(KM, 2 + int(medium[std::size_t(m3.domain[std::size_t(e)])].D.size()));
    const int KC = 4, CC = 2 * KC + 1, CM = 2 * KM + 1, perCoef = 3 * CC + 3 * CM;
    std::vector<double> pmlCoef(std::size_t(Np) * std::size_t(Q) * std::size_t(perCoef));
    std::vector<double> pmlWeight(std::size_t(Np) * std::size_t(Q));
    {
        std::atomic<int> next{0};
        const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 32);
        auto work = [&] {
            EdgeSpace3::Basis b;
            for (int i = next++; i < Np; i = next++) {
                const int e = pmlElements[std::size_t(i)], d = m3.domain[std::size_t(e)];
                for (int r = 0; r < Q; ++r) {
                    const auto& q = rule.points[std::size_t(r)];
                    space.basis(e, q[0], q[1], q[2], b);
                    double sg[3];
                    su.sigma(d, b.x, sg);
                    pmlWeight[std::size_t(i) * std::size_t(Q) + std::size_t(r)] = q[3] * b.det;
                    for (int a = 0; a < 3; ++a) {
                        double* c = &pmlCoef[(std::size_t(i) * std::size_t(Q) + std::size_t(r)) * std::size_t(perCoef)];
                        const Filter fc = pmlFilter(true, a, sg, medium[std::size_t(d)], p0), fm = pmlFilter(false, a, sg, medium[std::size_t(d)], p0);
                        Q_ASSERT(fc.K <= KC && fm.K <= KM);
                        pack(fc, KC, c + a * CC);
                        pack(fm, KM, c + 3 * CC + a * CM);
                    }
                }
            }
        };
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
        for (std::thread& t : pool) t.join();
    }
    // The step's matrix A(p0), and the bulk's history matrices: of E one
    // step back and two, H1 = 2 Kc + b1 M, H2 = Kc + b2 M (each medium's b).
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 32);
    auto assemble = [&](int which, Eigen::SparseMatrix<double>* out, int massOf = -1) {
        std::vector<std::vector<Eigen::Triplet<double>>> parts(static_cast<std::size_t>(threads));
        std::atomic<int> nextChunk{0};
        const int chunk = 256;
        auto work = [&](int part) {
            EdgeSpace3::Basis b;
            Eigen::MatrixXd Ke(per, per);
            auto& o = parts[std::size_t(part)];
            for (int c0 = nextChunk++ * chunk; c0 < Ne; c0 = nextChunk++ * chunk) {
                for (int e = c0; e < std::min(Ne, c0 + chunk); ++e) {
                    const int d = m3.domain[std::size_t(e)];
                    const int pi = pmlIndex[std::size_t(e)];
                    if (pi >= 0 && which != 0) continue;
                    if (massOf >= 0 && d != massOf) continue;
                    Ke.setZero();
                    const Filter& fm = bulkMass[std::size_t(d)];
                    // (A pole's domain: its mass's history its own recursion's.)
                    const bool own = std::find(dispersive.begin(), dispersive.end(), d) != dispersive.end();
                    double wc = which == 0 ? bulkCurl.b[0] : which == 1 ? bulkCurl.b[1] : bulkCurl.b[2];
                    double wm = which == 0 ? fm.b[0] : own ? 0.0 : which == 1 ? fm.b[1] : fm.b[2];
                    if (massOf >= 0) {
                        wc = 0;
                        wm = 1;
                    }
                    for (int r = 0; r < Q; ++r) {
                        const auto& q = rule.points[std::size_t(r)];
                        space.basis(e, q[0], q[1], q[2], b);
                        double cw[3] = {wc, wc, wc}, mw[3] = {wm, wm, wm};
                        if (pi >= 0)
                            for (int a = 0; a < 3; ++a) {
                                const double* c = &pmlCoef[(std::size_t(pi) * std::size_t(Q) + std::size_t(r)) * std::size_t(perCoef)];
                                cw[a] = c[a * CC];
                                mw[a] = c[3 * CC + a * CM];
                            }
                        const double w = q[3] * b.det;
                        for (int i = 0; i < per; ++i)
                            for (int k = 0; k <= i; ++k) {
                                double v = 0;
                                for (int a = 0; a < 3; ++a) v += cw[a] * b.c[i][a] * b.c[k][a] + mw[a] * b.v[i][a] * b.v[k][a];
                                Ke(i, k) += w * v;
                            }
                    }
                    const int* dofs = space.dofs(e);
                    for (int i = 0; i < per; ++i) {
                        if (fixed[std::size_t(dofs[i])]) continue;
                        for (int k = 0; k < per; ++k) {
                            if (fixed[std::size_t(dofs[k])]) continue;
                            const double v = k <= i ? Ke(i, k) : Ke(k, i);
                            if (v != 0) o.emplace_back(dofs[i], dofs[k], v);
                        }
                    }
                }
                if (cancelled()) return;
            }
        };
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t) pool.emplace_back(work, t);
        work(0);
        for (std::thread& t : pool) t.join();
        std::size_t total = 0;
        for (const auto& p : parts) total += p.size();
        std::vector<Eigen::Triplet<double>> all;
        all.reserve(total + std::size_t(N));
        for (auto& p : parts) {
            all.insert(all.end(), p.begin(), p.end());
            std::vector<Eigen::Triplet<double>>().swap(p);
        }
        if (which == 0)
            for (int i = 0; i < N; ++i)
                if (fixed[std::size_t(i)]) all.emplace_back(i, i, 1.0);
        out->resize(N, N);
        out->setFromTriplets(all.begin(), all.end());
        out->makeCompressed();
    };
    std::optional<SparseCholesky> chol(std::in_place);
    {
        Eigen::SparseMatrix<double> A0;
        assemble(0, &A0);
        if (cancelled()) {
            out.error = tr("cancelled");
            return out;
        }
        tell(0.1, tr("factorizing %1 unknowns").arg(N));
        chol->setOrdering(SparseCholesky::Ordering::Metis);
        chol->analyzePattern(A0);
        if (chol->info() != Eigen::Success) {
            out.error = tr("the system could not be analysed (%1)").arg(QString::fromStdString(chol->lastErrorMessage()));
            return out;
        }
        if (settings.memoryLimit > 0 && chol->factorBytes() > settings.memoryLimit) {
            out.error = tr("its factors would take %1, past the limit of %2").arg(bytesText(chol->factorBytes()), bytesText(settings.memoryLimit));
            return out;
        }
        chol->factorize(A0);
        if (chol->info() != Eigen::Success) {
            out.error = tr("the system could not be factorized (%1)").arg(QString::fromStdString(chol->lastErrorMessage()));
            return out;
        }
    }
    const qint64 factorized = timer.elapsed();
    Eigen::SparseMatrix<double, Eigen::RowMajor> H1, H2;
    {
        Eigen::SparseMatrix<double> h;
        assemble(1, &h);
        H1 = h;
        assemble(2, &h);
        H2 = h;
        H1.makeCompressed();
        H2.makeCompressed();
    }
    // A pole's domains: their bulk's mass, and its recursion's states (a
    // vector each).
    std::vector<Eigen::SparseMatrix<double, Eigen::RowMajor>> massOfDomain(dispersive.size());
    std::vector<std::vector<Eigen::MatrixXd>> domainState(dispersive.size());
    for (std::size_t i = 0; i < dispersive.size(); ++i) {
        Eigen::SparseMatrix<double> h;
        assemble(1, &h, dispersive[i]);
        massOfDomain[i] = h;
        massOfDomain[i].makeCompressed();
        domainState[i].assign(std::size_t(bulkMass[std::size_t(dispersive[i])].K), Eigen::MatrixXd::Zero(N, P));
    }
    if (cancelled()) {
        out.error = tr("cancelled");
        return out;
    }
    // The PML points' plan functions, by plan triangle and the rule's plan
    // point (shared by a triangle's layers), and the ζ functions by the
    // rule's point.
    std::vector<int> planPoint(std::size_t(Q), 0);
    std::vector<std::array<double, 2>> planXY;
    for (int r = 0; r < Q; ++r) {
        const auto& q = rule.points[std::size_t(r)];
        int found = -1;
        for (int k = 0; k < int(planXY.size()); ++k)
            if (planXY[std::size_t(k)][0] == q[0] && planXY[std::size_t(k)][1] == q[1]) found = k;
        if (found < 0) {
            found = int(planXY.size());
            planXY.push_back({q[0], q[1]});
        }
        planPoint[std::size_t(r)] = found;
    }
    const int PP = int(planXY.size());
    struct PlanBasis {
        EdgeSpace::Basis eb;
        double n[6], gx[6], gy[6];
    };
    std::map<int, int> planOf;   // plan triangle → its first entry in planCache
    std::vector<PlanBasis> planCache;
    const int order = space.order(), pe = order == 1 ? 3 : 8, nk = order + 1, mc = order, pn = order == 1 ? 3 : 6;
    for (int e : pmlElements) {
        const int t = m3.prismTriangle[std::size_t(e)];
        if (planOf.count(t)) continue;
        planOf[t] = int(planCache.size());
        const AffineMap am = affineMap(space.planNodes(), t);
        for (int k = 0; k < PP; ++k) {
            PlanBasis pb;
            space.planEdges().basis(t, am, planXY[std::size_t(k)][0], planXY[std::size_t(k)][1], pb.eb);
            Shape sh;
            shapeAt(order, planXY[std::size_t(k)][0], planXY[std::size_t(k)][1], sh);
            for (int n = 0; n < sh.count; ++n) {
                pb.n[n] = sh.n[n];
                pb.gx[n] = sh.dxi[n] * am.grad[1][0] + sh.deta[n] * am.grad[2][0];
                pb.gy[n] = sh.dxi[n] * am.grad[1][1] + sh.deta[n] * am.grad[2][1];
            }
            planCache.push_back(pb);
        }
    }
    std::vector<int> pmlPlan(static_cast<std::size_t>(Np));
    for (int i = 0; i < Np; ++i) pmlPlan[std::size_t(i)] = planOf[m3.prismTriangle[std::size_t(pmlElements[std::size_t(i)])]];
    // (ζ's functions at each rule point: L, dL, M.)
    std::vector<std::array<double, 8>> zf(static_cast<std::size_t>(Q));
    for (int r = 0; r < Q; ++r) {
        const double z = rule.points[std::size_t(r)][2];
        auto& f = zf[std::size_t(r)];
        if (order == 1) f = {1 - z, z, 0, -1, 1, 0, 1, 0};
        else f = {(1 - z) * (1 - 2 * z), 4 * z * (1 - z), z * (2 * z - 1), 4 * z - 3, 4 - 8 * z, 4 * z - 1, 1 - z, z};
    }
    // The PML points' states: by element, point, filter (the curl's 3, the
    // mass's 3), state and fed column.
    const int SC = KC, SM = KM, perPoint = 3 * SC + 3 * SM;
    std::vector<double> pmlState(std::size_t(Np) * std::size_t(Q) * std::size_t(perPoint) * std::size_t(P), 0.0);
    std::vector<double> pmlOut(std::size_t(Np) * std::size_t(per) * std::size_t(P), 0.0);
    StepPool pool(threads);
    auto pmlStep = [&](const Eigen::MatrixXd& En) {
        const int chunk = 64;
        const std::function<void(int)> work = [&](int part) {
            double v[36][3], c[36][3];
            std::vector<double> Ee(std::size_t(per) * std::size_t(P)), hl(std::size_t(per) * std::size_t(P));
            const int c0 = part * chunk;
            {
                for (int i = c0; i < std::min(Np, c0 + chunk); ++i) {
                    const int e = pmlElements[std::size_t(i)];
                    const int* dofs = space.dofs(e);
                    for (int k = 0; k < per; ++k)
                        for (int j = 0; j < P; ++j) Ee[std::size_t(k) * std::size_t(P) + std::size_t(j)] = En(dofs[k], j);
                    std::fill(hl.begin(), hl.end(), 0.0);
                    const int jl = m3.prismLayer[std::size_t(e)];
                    const double h = space.z(jl + 1) - space.z(jl);
                    for (int r = 0; r < Q; ++r) {
                        const PlanBasis& pb = planCache[std::size_t(pmlPlan[std::size_t(i)] + planPoint[std::size_t(r)])];
                        const auto& f = zf[std::size_t(r)];
                        const double* L = &f[0];
                        const double* dL = &f[3];
                        const double* M = &f[6];
                        for (int k = 0; k < nk; ++k)
                            for (int ii = 0; ii < pe; ++ii) {
                                const int a = k * pe + ii;
                                v[a][0] = pb.eb.vx[ii] * L[k];
                                v[a][1] = pb.eb.vy[ii] * L[k];
                                v[a][2] = 0;
                                c[a][0] = -dL[k] / h * pb.eb.vy[ii];
                                c[a][1] = dL[k] / h * pb.eb.vx[ii];
                                c[a][2] = L[k] * pb.eb.curl[ii];
                            }
                        for (int m = 0; m < mc; ++m)
                            for (int n = 0; n < pn; ++n) {
                                const int a = pe * nk + m * pn + n;
                                v[a][0] = 0;
                                v[a][1] = 0;
                                v[a][2] = pb.n[n] * M[m];
                                c[a][0] = M[m] * pb.gy[n];
                                c[a][1] = -M[m] * pb.gx[n];
                                c[a][2] = 0;
                            }
                        const std::size_t pt = std::size_t(i) * std::size_t(Q) + std::size_t(r);
                        const double w = pmlWeight[pt];
                        const double* F = &pmlCoef[pt * std::size_t(perCoef)];
                        double* S = &pmlState[pt * std::size_t(perPoint) * std::size_t(P)];
                        for (int j = 0; j < P; ++j) {
                            double uc[3] = {0, 0, 0}, um[3] = {0, 0, 0};
                            for (int k = 0; k < per; ++k) {
                                const double x = Ee[std::size_t(k) * std::size_t(P) + std::size_t(j)];
                                if (x == 0) continue;
                                for (int a = 0; a < 3; ++a) {
                                    uc[a] += c[k][a] * x;
                                    um[a] += v[k][a] * x;
                                }
                            }
                            double gc[3], gm[3];
                            double* Sj = S + std::size_t(j) * std::size_t(perPoint);
                            for (int a = 0; a < 3; ++a) {
                                gc[a] = stepPacked(F + a * CC, KC, Sj + a * SC, uc[a]);
                                gm[a] = stepPacked(F + 3 * CC + a * CM, KM, Sj + 3 * SC + a * SM, um[a]);
                            }
                            for (int k = 0; k < per; ++k) {
                                double s = 0;
                                for (int a = 0; a < 3; ++a) s += c[k][a] * gc[a] + v[k][a] * gm[a];
                                hl[std::size_t(k) * std::size_t(P) + std::size_t(j)] += w * s;
                            }
                        }
                    }
                    std::copy(hl.begin(), hl.end(), pmlOut.begin() + std::ptrdiff_t(std::size_t(i) * std::size_t(per) * std::size_t(P)));
                }
            }
        };
        pool.run((Np + chunk - 1) / chunk, work);
    };
    // The bulk's history, H1 Eⁿ + H2 Eⁿ⁻¹, rows in parts.
    auto bulkHistory = [&](const Eigen::MatrixXd& En, const Eigen::MatrixXd& Eb, Eigen::MatrixXd& out) {
        const int rows = 4096;
        const std::function<void(int)> work = [&](int part) {
            const int r0 = part * rows, r1 = std::min(N, r0 + rows);
            for (int j = 0; j < P; ++j) {
                const double* en = En.col(j).data();
                const double* eb = Eb.col(j).data();
                double* o = out.col(j).data();
                for (int r = r0; r < r1; ++r) {
                    double s = 0;
                    for (int k = H1.outerIndexPtr()[r]; k < H1.outerIndexPtr()[r + 1]; ++k) s += H1.valuePtr()[k] * en[H1.innerIndexPtr()[k]];
                    for (int k = H2.outerIndexPtr()[r]; k < H2.outerIndexPtr()[r + 1]; ++k) s += H2.valuePtr()[k] * eb[H2.innerIndexPtr()[k]];
                    o[r] = s;
                }
            }
        };
        pool.run((N + rows - 1) / rows, work);
    };
    // The monitors: the degrees of freedom of the prisms on the device's
    // side of every port's plane (those touching it) - their transforms at
    // each frequency.
    std::vector<int> monitor;
    {
        std::set<int> m;
        for (const wave3::Port& p : su.ports)
            for (int e : p.side) {
                const int* dofs = space.dofs(e);
                for (int k = 0; k < per; ++k)
                    if (!fixed[std::size_t(dofs[k])]) m.insert(dofs[k]);
            }
        monitor.assign(m.begin(), m.end());
    }
    const int Nf = int(omega.size()), Nm = int(monitor.size());
    std::vector<Eigen::MatrixXcd> monitorHat(std::size_t(Nf), Eigen::MatrixXcd::Zero(Nm, P));
    // The first fed port mode's field, transformed: at the wavelengths
    // asked for it - the one nearest the band's middle, so many evenly over
    // the band (its ends among them), or every one - as far as 2 GB hold
    // them. (Each a complex vector of the unknowns, added to at every step:
    // fifty of them as much as the rest of a step.)
    const bool keepField = settings.fieldsKept != 0 && settings.fieldWavelengths != 0;
    std::vector<int> slotOf(std::size_t(Nf), -1);
    std::vector<int> slotFreq;
    if (keepField) {
        const int room = int(std::clamp(2e9 / (16.0 * N), 1.0, double(Nf)));
        const int want = settings.fieldWavelengths < 0 ? Nf : std::min(settings.fieldWavelengths, Nf);
        const int kept = std::min(room, want);
        int nearest = 0;
        for (int k = 1; k < Nf; ++k)
            if (std::abs(frequencies[std::size_t(k)] - fMid) < std::abs(frequencies[std::size_t(nearest)] - fMid)) nearest = k;
        for (int j = 0; j < kept; ++j) {
            const int k = kept == 1 ? nearest : int(std::lround(double(j) * (Nf - 1) / (kept - 1)));
            if (slotOf[std::size_t(k)] < 0) {
                slotOf[std::size_t(k)] = int(slotFreq.size());
                slotFreq.push_back(k);
            }
        }
        if (int(slotFreq.size()) < Nf) {
            QStringList at;
            for (int k : slotFreq) at << formatQuantity(C0 / frequencies[std::size_t(k)], Dim::length());
            out.log << tr("Its field kept at %1 of the %2 wavelengths (%3)%4 - the rest's S-parameters only")
                           .arg(slotFreq.size())
                           .arg(Nf)
                           .arg(at.join(QStringLiteral(", ")))
                           .arg(kept < want ? tr(", what 2 GB of them hold") : QString());
        }
    } else
        out.log << tr("Its field kept at none of the wavelengths: the S-parameters only");
    const int NS = int(slotFreq.size());
    std::vector<Eigen::VectorXcd> fieldHat(std::size_t(NS), Eigen::VectorXcd::Zero(N));
    // The pulse: a sine at the run's band's middle under a Gaussian, its
    // spectrum a tenth of its peak at the band's edges (the band 10 % about
    // its middle at least: a pulse of some 35 periods), from e^-15 to e^-15
    // (the S-parameters a ratio of its transforms: its spectrum's shape
    // cancelled, but where it is nought).
    const double wLo = *std::min_element(omega.begin(), omega.end()), wHi = *std::max_element(omega.begin(), omega.end());
    const double wc = 0.5 * (wLo + wHi), half = std::max(0.5 * (wHi - wLo), 0.1 * wc);
    const double tau = std::sqrt(2 * std::log(10.0)) / half, t0 = std::sqrt(30.0) * tau;
    int sourceSteps = int(std::ceil(2 * t0 / dt));
    // (Its samples' sum and first moment made nought - the envelope's own
    // shape and its moment's taken off: what the curl's null space, static
    // fields, integrates twice left nought after it, not a field at rest
    // the ports' fields never fall below.)
    // (Its envelope's ends, at e^-15 of its peak, brought to nought over
    // four periods (sin²) - not cut: a cut's step there, broadband, the
    // sources' filters below magnify thousands of times far from the band,
    // and the mesh's fastest waves rang on at 1e-7 of the peak.)
    const int taper = std::max(4, int(std::lround(4 * std::max(4.0, settings.stepsPerPeriod))));
    auto envelope = [&](int n) {
        const double t = double(n) * dt - t0;
        const int m = std::min(n, sourceSteps - n);
        const double w = m >= taper ? 1.0 : std::pow(std::sin(0.5 * M_PI * double(std::max(0, m)) / taper), 2);
        return std::exp(-0.5 * t * t / (tau * tau)) * w;
    };
    std::vector<double> pulseAt(std::size_t(sourceSteps) + 1);
    {
        std::vector<double> env(pulseAt.size());
        for (std::size_t n = 0; n < pulseAt.size(); ++n) {
            const double t = double(n) * dt - t0;
            env[n] = envelope(int(n));
            pulseAt[n] = env[n] * std::sin(wc * t);
        }
        // Σ (g - a e - b n e) = 0, Σ n (g - a e - b n e) = 0.
        double S0 = 0, S1 = 0, S2 = 0, G0 = 0, G1 = 0;
        for (std::size_t n = 0; n < pulseAt.size(); ++n) {
            const double x = double(n);
            S0 += env[n];
            S1 += x * env[n];
            S2 += x * x * env[n];
            G0 += pulseAt[n];
            G1 += x * pulseAt[n];
        }
        const double det = S0 * S2 - S1 * S1;
        const double a = (G0 * S2 - G1 * S1) / det, b = (S0 * G1 - S1 * G0) / det;
        for (std::size_t n = 0; n < pulseAt.size(); ++n) pulseAt[n] -= (a + b * double(n)) * env[n];
    }
    // The sources: each fed port mode's profile at the band's ends and its
    // middle (the middle's alone where one wavelength is asked; the ends'
    // where two), each sent with the pulse through a filter of its own -
    // its spectrum the profile's Lagrange polynomial in σ = sin²(ωΔt/2), a
    // polynomial in the second difference (σ ↔ -δ²/4, a symmetric filter of
    // a few steps: the pulse's sum and moment kept nought) -, so that at
    // each frequency the source is the mode's profile there, to the third
    // order in the band. (The middle's profile at every frequency sent the
    // band's ends a little of the port's other modes: a Y-branch's T 4e-4
    // from the frequency domain's 40 nm from its band's middle, 2e-5 there.)
    std::vector<double> nodeF;
    {
        std::vector<double> fs(frequencies.begin(), frequencies.end());
        std::sort(fs.begin(), fs.end());
        fs.erase(std::unique(fs.begin(), fs.end()), fs.end());
        nodeF = fs.size() == 1 ? std::vector<double>{fMid} : fs.size() == 2 ? std::vector<double>{fLo, fHi} : std::vector<double>{fLo, fMid, fHi};
    }
    const int NN = int(nodeF.size());
    // (Each node's filter: its Lagrange polynomial's coefficients in σ.)
    std::vector<std::vector<double>> signal(static_cast<std::size_t>(NN));
    {
        std::vector<double> sig;
        for (double f : nodeF) sig.push_back(std::pow(std::sin(std::atan(M_PI * f * dt)), 2));
        const int pad = NN - 1, len = int(pulseAt.size()) + 2 * pad;
        std::vector<std::vector<double>> coef(static_cast<std::size_t>(NN));
        for (int i = 0; i < NN; ++i) {
            std::vector<double> poly{1.0};
            for (int j = 0; j < NN; ++j) {
                if (j == i) continue;
                const double den = sig[std::size_t(i)] - sig[std::size_t(j)];
                std::vector<double> next(poly.size() + 1, 0.0);
                for (std::size_t k = 0; k < poly.size(); ++k) {
                    next[k + 1] += poly[k] / den;
                    next[k] -= poly[k] * sig[std::size_t(j)] / den;
                }
                poly = std::move(next);
            }
            coef[std::size_t(i)] = poly;
            signal[std::size_t(i)].assign(std::size_t(len), 0.0);
        }
        std::vector<double> t(std::size_t(len), 0.0);
        for (std::size_t n = 0; n < pulseAt.size(); ++n) t[n + std::size_t(pad)] = pulseAt[n];
        for (int p = 0; p < NN; ++p) {
            for (int i = 0; i < NN; ++i)
                for (int n = 0; n < len; ++n) signal[std::size_t(i)][std::size_t(n)] += coef[std::size_t(i)][std::size_t(p)] * t[std::size_t(n)];
            // t ← -δ²t/4
            std::vector<double> next(std::size_t(len), 0.0);
            for (int n = 0; n < len; ++n) {
                const double before = n > 0 ? t[std::size_t(n - 1)] : 0.0, after = n + 1 < len ? t[std::size_t(n + 1)] : 0.0;
                next[std::size_t(n)] = -(after - 2 * t[std::size_t(n)] + before) / 4;
            }
            t = std::move(next);
        }
        // (Each filtered signal's sum and moment taken off again, as the
        // pulse's were: a filter's polynomial is large far from the band - at
        // σ = 0 hundreds of times its value there -, and what rounding left
        // of the pulse's, so magnified and integrated twice by the curl's
        // null space, kept the field from falling to 1e-8 of its peak.)
        std::vector<double> env(std::size_t(len), 0.0);
        for (std::size_t n = 0; n < pulseAt.size(); ++n) env[n + std::size_t(pad)] = envelope(int(n));
        for (int i = 0; i < NN; ++i) {
            std::vector<double>& g = signal[std::size_t(i)];
            for (int pass = 0; pass < 2; ++pass) {
                double S0 = 0, S1 = 0, S2 = 0, G0 = 0, G1 = 0;
                for (int n = 0; n < len; ++n) {
                    const double x = double(n), e = env[std::size_t(n)];
                    S0 += e;
                    S1 += x * e;
                    S2 += x * x * e;
                    G0 += g[std::size_t(n)];
                    G1 += x * g[std::size_t(n)];
                }
                const double det = S0 * S2 - S1 * S1;
                const double a = (G0 * S2 - G1 * S1) / det, b = (S0 * G1 - S1 * G0) / det;
                for (int n = 0; n < len; ++n) g[std::size_t(n)] -= (a + b * double(n)) * env[std::size_t(n)];
            }
        }
        sourceSteps = len - 1;
    }
    // (Each profile real: the middle's u turned so that its largest value
    // is, its real part then - a lossless guide's modes real but for a
    // common phase -; an end's mode the one most like the middle's, turned
    // to it.)
    std::vector<std::vector<std::vector<std::pair<int, double>>>> source(static_cast<std::size_t>(P),
                                                                         std::vector<std::vector<std::pair<int, double>>>(static_cast<std::size_t>(NN)));
    {
        std::vector<std::unordered_map<int, double>> middle(static_cast<std::size_t>(P));
        for (int j = 0; j < P; ++j) {
            const wave3::Column& col = probes[std::size_t(fedList[std::size_t(j)].taken)];
            cd big = 0;
            for (const auto& [dof, val] : col)
                if (std::abs(val) > std::abs(big)) big = val;
            const cd turn = big == cd(0) ? cd(1) : std::conj(big) / std::abs(big);
            for (const auto& [dof, val] : col) middle[std::size_t(j)][dof] = (val * turn).real();
        }
        // The ends' modes, every fed port's, side by side.
        std::vector<std::pair<int, int>> jobs;   // (node, port)
        for (int i = 0; i < NN; ++i) {
            if (nodeF[std::size_t(i)] == fMid) continue;
            for (std::size_t pi = 0; pi < su.ports.size(); ++pi)
                if (settings.ports[pi].excite) jobs.push_back({i, int(pi)});
        }
        std::vector<std::vector<wave3::Column>> endU(jobs.size());
        std::vector<QString> endWhy(jobs.size());
        {
            std::atomic<int> next{0};
            auto work = [&] {
                for (int q = next++; q < int(jobs.size()); q = next++) {
                    const auto [i, pi] = jobs[std::size_t(q)];
                    const double f = nodeF[std::size_t(i)];
                    const Wave3Port& wp = settings.ports[std::size_t(pi)];
                    const EmeModes em = wave3::portModes(model, parameters, topology, su, settings, pi, f, int(wp.modes.size()) + 2, &endWhy[std::size_t(q)]);
                    if (!endWhy[std::size_t(q)].isEmpty() || em.count() == 0) {
                        if (endWhy[std::size_t(q)].isEmpty()) endWhy[std::size_t(q)] = tr("no mode found");
                        continue;
                    }
                    const double kk = 2 * M_PI * f / C0;
                    std::vector<cd> epsAt(medium.size());
                    for (std::size_t d = 0; d < medium.size(); ++d) epsAt[d] = mediumAt(medium[d], J * kk);
                    wave3::portColumns(su, su.ports[std::size_t(pi)], em, settings.order, kk, epsAt, &endU[std::size_t(q)], nullptr);
                }
            };
            const int lanes = std::clamp(int(std::thread::hardware_concurrency()), 1, std::max(1, int(jobs.size())));
            std::vector<std::thread> pool;
            for (int t = 1; t < lanes; ++t) pool.emplace_back(work);
            work();
            for (std::thread& t : pool) t.join();
        }
        for (std::size_t q = 0; q < jobs.size(); ++q)
            if (!endWhy[q].isEmpty()) {
                out.error = tr("port %1 at %2: %3")
                                .arg(settings.ports[std::size_t(jobs[q].second)].name, formatQuantity(C0 / nodeF[std::size_t(jobs[q].first)], Dim::length()), endWhy[q]);
                return out;
            }
        for (int j = 0; j < P; ++j) {
            const Fed& fd = fedList[std::size_t(j)];
            const auto& mid = middle[std::size_t(j)];
            double midNorm = 0;
            for (const auto& [dof, val] : mid) midNorm += val * val;
            midNorm = std::sqrt(midNorm);
            for (int i = 0; i < NN; ++i) {
                auto& prof = source[std::size_t(j)][std::size_t(i)];
                if (nodeF[std::size_t(i)] == fMid) {
                    for (const auto& [dof, val] : mid) prof.push_back({dof, val});
                    std::sort(prof.begin(), prof.end());
                    continue;
                }
                std::size_t q = 0;
                while (!(jobs[q].first == i && jobs[q].second == fd.port)) ++q;
                // The end's mode most like the middle's: |<u, u_mid>| / |u|.
                int best = -1;
                double likeness = -1;
                cd overlap = 0;
                for (int m = 0; m < int(endU[q].size()); ++m) {
                    cd c = 0;
                    double norm = 0;
                    for (const auto& [dof, val] : endU[q][std::size_t(m)]) {
                        norm += std::norm(val);
                        auto it = mid.find(dof);
                        if (it != mid.end()) c += val * it->second;
                    }
                    const double l = norm > 0 && midNorm > 0 ? std::abs(c) / (std::sqrt(norm) * midNorm) : 0;
                    if (l > likeness) {
                        likeness = l;
                        best = m;
                        overlap = c;
                    }
                }
                if (best < 0 || likeness < 0.5) {
                    out.error = tr("port %1: its fed mode %2 has no like at %3 (the best %4 alike) - a narrower band")
                                    .arg(settings.ports[std::size_t(fd.port)].name, fd.label)
                                    .arg(formatQuantity(C0 / nodeF[std::size_t(i)], Dim::length()))
                                    .arg(std::max(0.0, likeness), 0, 'f', 2);
                    return out;
                }
                if (likeness < 0.9)
                    out.warnings << tr("port %1: its fed mode at %2 only %3 like the band's middle's - the source's profile between them "
                                       "approximate (a narrower band)")
                                        .arg(settings.ports[std::size_t(fd.port)].name)
                                        .arg(formatQuantity(C0 / nodeF[std::size_t(i)], Dim::length()))
                                        .arg(likeness, 0, 'f', 2);
                const cd turn = overlap == cd(0) ? cd(1) : std::conj(overlap) / std::abs(overlap);
                for (const auto& [dof, val] : endU[q][std::size_t(best)]) prof.push_back({dof, (val * turn).real()});
                std::sort(prof.begin(), prof.end());
            }
        }
        if (NN > 1) {
            QStringList at;
            for (double f : nodeF) at << formatQuantity(C0 / f, Dim::length());
            out.log << tr("Its sources: each fed mode's profile at %1, between them by a filter of %2 steps").arg(at.join(QStringLiteral(", "))).arg(2 * NN - 1);
        }
    }
    // The steps.
    // (E1: the step before; hist: the next step's history.)
    Eigen::MatrixXd E1 = Eigen::MatrixXd::Zero(N, P), hist = Eigen::MatrixXd::Zero(N, P);
    double peak = 0, recent = 0, normPeak = 0;
    std::vector<cd> probeHat(probes.size() * std::size_t(P) * std::size_t(Nf), 0.0), probeHatBefore;
    std::vector<double> window;
    const int windowSteps = std::max(4, int(std::ceil(2 * std::max(4.0, settings.stepsPerPeriod))));
    int n = 0;
    bool settled = false;
    QElapsedTimer stepping, part;
    stepping.start();
    part.start();
    qint64 spent[4] = {0, 0, 0, 0};   // the solves, the bulk's history, the PMLs', the transforms (ns)
    const int maxSteps = std::max(settings.maxSteps, sourceSteps + 1);
    for (n = 0; n < maxSteps; ++n) {
        if (cancelled()) {
            out.error = tr("cancelled");
            return out;
        }
        Eigen::MatrixXd rhs = -hist;
        if (n <= sourceSteps)
            for (int i = 0; i < NN; ++i) {
                const double g = signal[std::size_t(i)][std::size_t(n)];
                for (int j = 0; j < P; ++j)
                    for (const auto& [dof, val] : source[std::size_t(j)][std::size_t(i)]) rhs(dof, j) += g * val;
            }
        qint64 mark = part.nsecsElapsed();
        Eigen::MatrixXd En = chol->solveMany(rhs);
        spent[0] += part.nsecsElapsed() - mark;
        mark = part.nsecsElapsed();
        // The next step's history: the bulk's, the PMLs' states.
        bulkHistory(En, E1, hist);
        for (std::size_t i = 0; i < dispersive.size(); ++i) {
            const Filter& f = bulkMass[std::size_t(dispersive[i])];
            const auto& Md = massOfDomain[i];
            auto& S = domainState[i];
            const int rows = 4096;
            const std::function<void(int)> work = [&](int task) {
                const int r0 = task * rows, r1 = std::min(N, r0 + rows);
                for (int j = 0; j < P; ++j) {
                    const double* en = En.col(j).data();
                    for (int r = r0; r < r1; ++r) {
                        double u = 0;
                        for (int k = Md.outerIndexPtr()[r]; k < Md.outerIndexPtr()[r + 1]; ++k) u += Md.valuePtr()[k] * en[Md.innerIndexPtr()[k]];
                        const double y = f.b[0] * u + S[0](r, j);
                        for (int k = 1; k <= f.K; ++k) S[std::size_t(k - 1)](r, j) = f.b[k] * u - f.a[k] * y + (k < f.K ? S[std::size_t(k)](r, j) : 0.0);
                        hist(r, j) += S[0](r, j);
                    }
                }
            };
            pool.run((N + rows - 1) / rows, work);
        }
        spent[1] += part.nsecsElapsed() - mark;
        mark = part.nsecsElapsed();
        if (Np > 0) {
            pmlStep(En);
            for (int i = 0; i < Np; ++i) {
                const int* dofs = space.dofs(pmlElements[std::size_t(i)]);
                const double* o = &pmlOut[std::size_t(i) * std::size_t(per) * std::size_t(P)];
                for (int k = 0; k < per; ++k) {
                    if (fixed[std::size_t(dofs[k])]) continue;
                    for (int j = 0; j < P; ++j) hist(dofs[k], j) += o[std::size_t(k) * std::size_t(P) + std::size_t(j)];
                }
            }
        }
        spent[2] += part.nsecsElapsed() - mark;
        mark = part.nsecsElapsed();
        // Transformed at each frequency: e^-jωnΔt (a frequency a task).
        {
            const std::function<void(int)> work = [&](int k) {
                const cd z = std::polar(1.0, -omega[std::size_t(k)] * n * dt);
                Eigen::MatrixXcd& Mh = monitorHat[std::size_t(k)];
                for (int j = 0; j < P; ++j)
                    for (int i = 0; i < Nm; ++i) Mh(i, j) += z * En(monitor[std::size_t(i)], j);
            };
            pool.run(Nf, work);
        }
        if (NS > 0) {
            // (By frequency and a part of the unknowns.)
            const int rows = 65536, parts = (N + rows - 1) / rows;
            const std::function<void(int)> work = [&](int task) {
                const int slot = task / parts, r0 = (task % parts) * rows, r1 = std::min(N, r0 + rows);
                const cd z = std::polar(1.0, -omega[std::size_t(slotFreq[std::size_t(slot)])] * n * dt);
                cd* f = fieldHat[std::size_t(slot)].data();
                const double* e = En.col(0).data();
                for (int r = r0; r < r1; ++r) f[r] += z * e[r];
            };
            pool.run(NS * parts, work);
        }
        spent[3] += part.nsecsElapsed() - mark;
        E1 = std::move(En);
        // Settled: the ports' fields, after the pulse, fallen to the decay
        // asked of their peak over two periods.
        double now = 0;
        for (std::size_t c = 0; c < probes.size(); ++c)
            for (int j = 0; j < P; ++j) {
                double s = 0;
                for (const auto& [dof, val] : probes[c]) s += val.real() * E1(dof, j);
                now = std::max(now, std::abs(s));
                for (int k = 0; k < Nf; ++k) probeHat[(c * std::size_t(P) + std::size_t(j)) * std::size_t(Nf) + std::size_t(k)] += std::polar(s, -omega[std::size_t(k)] * n * dt);
            }
        const double norm = E1.norm();
        normPeak = std::max(normPeak, norm);
        peak = std::max(peak, now);
        window.push_back(now);
        if (int(window.size()) > windowSteps) window.erase(window.begin());
        recent = *std::max_element(window.begin(), window.end());
        if (n % 20 == 0) {
            const double f = n < sourceSteps ? 0.3 * n / sourceSteps
                                             : 0.3 + 0.7 * std::clamp(std::log(std::max(peak, 1e-300) / std::max(recent, 1e-300)) / std::log(1 / settings.decay), 0.0, 1.0);
            tell(0.15 + 0.75 * f, tr("step %1, %2 fs: the ports' fields at %3 of their peak").arg(n).arg(n * dt * 1e15, 0, 'f', 1).arg(recent / std::max(peak, 1e-300), 0, 'g', 2));
        }
        if (n > sourceSteps && peak > 0 && recent < settings.decay * peak) {
            settled = true;
            ++n;
            break;
        }
        // Or the ports' transforms at the frequencies asked no longer moving
        // - to the decay asked of the largest, from one two periods to the
        // next - and the pulse gone, the field a hundredth of its peak: what
        // still rings (a metal window's modes at their cutoff, slowly) at
        // other frequencies.
        if (n > sourceSteps && (n - sourceSteps) % windowSteps == 0) {
            double change = 0, largest = 0;
            for (std::size_t i = 0; i < probeHat.size(); ++i) {
                if (!probeHatBefore.empty()) change = std::max(change, std::abs(probeHat[i] - probeHatBefore[i]));
                largest = std::max(largest, std::abs(probeHat[i]));
            }
            const bool still = !probeHatBefore.empty() && largest > 0 && change < settings.decay * largest && norm < 1e-2 * normPeak;
            probeHatBefore = probeHat;
            if (still) {
                settled = true;
                out.log << tr("Its transforms settled at the ports, the field at %1 of its peak").arg(norm / normPeak, 0, 'g', 2);
                ++n;
                break;
            }
        }
    }
    out.steps = n;
    out.settled = settled;
    const qint64 steppedFor = stepping.elapsed();
    if (!settled)
        out.warnings << tr("the time domain stopped at its %1 steps with the ports' fields at %2 of their peak, not %3: its spectra "
                           "cut short (a resonance ringing longer) - more steps, or a looser decay")
                            .arg(n)
                            .arg(recent / std::max(peak, 1e-300), 0, 'g', 2)
                            .arg(settings.decay);
    // The steps' own memory let go - the factors, the history, the PMLs'
    // states: what the ports' modes take below.
    const QString cholName = chol->name();
    const double cholBytes = chol->factorBytes();
    const std::size_t pmlStates = pmlState.size();
    chol.reset();
    H1 = {};
    H2 = {};
    E1 = {};
    hist = {};
    std::vector<double>().swap(pmlState);
    std::vector<double>().swap(pmlOut);
    domainState.clear();
    massOfDomain.clear();
    // Each frequency asked: its ports' modes, every port's field split into
    // its modes' waves, the S-matrix - the frequencies side by side, each
    // its ports' modes solved afresh (a cut some gigabyte at the ports'
    // resolution 30: as many at once as half the memory, or the limit,
    // holds).
    std::vector<int> fedTaken;
    for (const Fed& f : fedList) fedTaken.push_back(f.taken);
    out.at.assign(std::size_t(Nf), Wave3Result());
    std::vector<QString> failed(static_cast<std::size_t>(Nf));
    const double perCut = 1.5e9 * std::pow(std::max(1.0, settings.portResolution) / 30.0, 2) * (settings.order == 1 ? 0.5 : 1.0);
    const double held = double(NS) * 16.0 * N + double(Nf) * Nm * P * 16.0;
    const double budget = (settings.memoryLimit > 0 ? settings.memoryLimit : physicalMemory() / 2) - held;
    const int hardware = std::max(1, int(std::thread::hardware_concurrency()));
    const int lanes = std::clamp(int(budget / perCut), 1, std::min(hardware, Nf));
    std::atomic<int> nextFrequency{0}, doneFrequencies{0};
    std::mutex telling;
    QElapsedTimer modesTimer;
    modesTimer.start();
    tell(0.9, tr("the ports' modes at each frequency (%1 at once)").arg(lanes));
    auto oneFrequency = [&](int k) {
        Wave3Result r;
        r.frequency = frequencies[std::size_t(k)];
        r.space = su.space;
        r.unknowns = N;
        std::vector<EmeModes> modes(su.ports.size());
        std::vector<int> firstColumn, index, rowPort;
        int Mtot = 0;
        for (std::size_t pi = 0; pi < su.ports.size(); ++pi) {
            const Wave3Port& wp = settings.ports[pi];
            QString why;
            modes[pi] = wave3::portModes(model, parameters, topology, su, settings, int(pi), r.frequency, std::max(1, wp.absorbed), &why);
            if (!why.isEmpty() || modes[pi].count() == 0) {
                failed[std::size_t(k)] = tr("port %1 at %2: %3").arg(wp.name, formatQuantity(C0 / r.frequency, Dim::length()), why.isEmpty() ? tr("no mode found") : why);
                return;
            }
            firstColumn.push_back(Mtot);
            for (const QString& l : wp.modes) {
                const int m = modes[pi].find(l);
                if (m < 0) {
                    failed[std::size_t(k)] = tr("port %1 has no mode %2 at %3").arg(wp.name, l, formatQuantity(C0 / r.frequency, Dim::length()));
                    return;
                }
                index.push_back(Mtot + m);
                Wave3Result::PortMode pm;
                pm.port = int(pi);
                pm.label = modes[pi].label(m);
                pm.neff = modes[pi].neff(m);
                pm.te = modes[pi].te[std::size_t(m)];
                r.portModes.push_back(pm);
            }
            Mtot += modes[pi].count();
            rowPort.resize(std::size_t(Mtot), int(pi));
        }
        std::vector<char> carries(static_cast<std::size_t>(Mtot), 0);
        for (std::size_t pi = 0; pi < su.ports.size(); ++pi)
            for (int m = 0; m < modes[pi].count(); ++m) {
                const cd b = modes[pi].beta[std::size_t(m)];
                carries[std::size_t(firstColumn[pi] + m)] = b.real() > 0 && std::abs(b.imag()) <= 1e-3 * b.real() ? 1 : 0;
            }
        // (H = -∇×E/(jω'µ0): the transform's frequency the one asked; the
        // media there as the steps have them.)
        const cd jw = J * (2 * M_PI * r.frequency);
        const double kk = 2 * M_PI * r.frequency / C0;
        std::vector<cd> epsAt(medium.size());
        for (std::size_t d = 0; d < medium.size(); ++d) epsAt[d] = mediumAt(medium[d], J * kk);
        Eigen::MatrixXcd a(Mtot, P), b(Mtot, P);
        Eigen::VectorXcd full = Eigen::VectorXcd::Zero(N);
        for (std::size_t pi = 0; pi < su.ports.size(); ++pi) {
            std::vector<wave3::Column> u, v;
            wave3::portColumns(su, su.ports[pi], modes[pi], settings.order, kk, epsAt, &u, &v);
            for (int j = 0; j < P; ++j) {
                for (int i = 0; i < Nm; ++i) full(monitor[std::size_t(i)]) = monitorHat[std::size_t(k)](i, j);
                Eigen::VectorXcd ap, bp;
                wave3::waveAmplitudes(u, v, full, jw, &ap, &bp);
                a.block(firstColumn[pi], j, ap.size(), 1) = ap;
                b.block(firstColumn[pi], j, bp.size(), 1) = bp;
            }
        }
        Eigen::MatrixXcd perFed;
        wave3::scatteringOfAmplitudes(a, b, index, fedTaken, carries, rowPort, &r, &perFed);
        r.fed = {fedTaken.front()};
        if (slotOf[std::size_t(k)] >= 0) r.fields.push_back(fieldHat[std::size_t(slotOf[std::size_t(k)])] * perFed(0, 0));
        out.at[std::size_t(k)] = std::move(r);
    };
    {
        std::atomic<bool> stop{false};
        auto work = [&] {
            for (int k = nextFrequency++; k < Nf; k = nextFrequency++) {
                if (stop || cancelled()) {
                    stop = true;
                    return;
                }
                oneFrequency(k);
                if (!failed[std::size_t(k)].isEmpty()) stop = true;
                const int finished = ++doneFrequencies;
                std::lock_guard<std::mutex> lock(telling);
                tell(0.9 + 0.1 * finished / Nf, tr("the ports' modes at %1 of %2 frequencies (%3 at once)").arg(finished).arg(Nf).arg(lanes));
            }
        };
        std::vector<std::thread> lanePool;
        for (int t = 1; t < lanes; ++t) lanePool.emplace_back(work);
        work();
        for (std::thread& t : lanePool) t.join();
    }
    for (const QString& f : failed)
        if (!f.isEmpty()) {
            out.error = f;
            return out;
        }
    if (cancelled()) {
        out.error = tr("cancelled");
        return out;
    }
    const double modesSeconds = modesTimer.elapsed() / 1000.0;
    out.log << tr("Time-domain solve: %1 unknowns (order %2), %3 prisms (%4 in PMLs), %5 port modes fed together; Δt %6 fs (%7 a period at "
                  "%8), %9 steps to %10 fs%11; %12 s - %13 %14 s, the steps %15 s (%16 ms each), the ports' modes and the spectra %17 s "
                  "(%18 frequencies, %19 at once)")
                     .arg(N)
                     .arg(settings.order)
                     .arg(Ne)
                     .arg(Np)
                     .arg(P)
                     .arg(dt * 1e15, 0, 'f', 3)
                     .arg(settings.stepsPerPeriod)
                     .arg(formatQuantity(C0 / fMid, Dim::length()))
                     .arg(out.steps)
                     .arg(out.steps * dt * 1e15, 0, 'f', 0)
                     .arg(settled ? QString() : tr(", not settled"))
                     .arg(timer.elapsed() / 1000.0, 0, 'f', 2)
                     .arg(cholName)
                     .arg(factorized / 1000.0, 0, 'f', 2)
                     .arg(steppedFor / 1000.0, 0, 'f', 2)
                     .arg(out.steps > 0 ? double(steppedFor) / out.steps : 0.0, 0, 'f', 1)
                     .arg(modesSeconds, 0, 'f', 2)
                     .arg(Nf)
                     .arg(lanes);
    out.log << tr("Its steps: the solves %1 s, the bulk's history %2 s, the PMLs' points %3 s (%4 of them, %5 MB of states), the transforms %6 s")
                   .arg(spent[0] * 1e-9, 0, 'f', 2)
                   .arg(spent[1] * 1e-9, 0, 'f', 2)
                   .arg(spent[2] * 1e-9, 0, 'f', 2)
                   .arg(qint64(Np) * Q)
                   .arg(double(pmlStates) * 8 / 1e6, 0, 'f', 0)
                   .arg(spent[3] * 1e-9, 0, 'f', 2);
    if (cholBytes > 0) out.log << tr("Its factors and their workspace %1, as the analysis said").arg(bytesText(cholBytes));
    tell(1.0, tr("done"));
    return out;
}

Estimate3 estimateWave3Time(std::shared_ptr<const Mesh3> mesh, int order)
{
    Estimate3 e = estimateWave3(mesh, order);
    const double n = double(e.unknowns);
    e.directBytes = 1.2e4 * n;
    e.directSeconds = 0.4 * e.directSeconds + 700 * 2e-7 * n * std::pow(std::max(1.0, n / 6e4), 0.3);
    e.iterativeBytes = e.directBytes;
    e.iterativeSeconds = e.directSeconds;
    return e;
}

} // namespace qucs_s::fem
