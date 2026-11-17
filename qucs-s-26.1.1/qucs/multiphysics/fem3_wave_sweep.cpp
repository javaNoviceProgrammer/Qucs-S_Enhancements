/*
 * fem3_wave_sweep.cpp - a 3D full-wave solve's fast sweep: a few
 *                       wavelengths solved exactly (each a factorization),
 *                       the rest on the space their solutions span (a reduced
 *                       basis), each checked by its residual and the space
 *                       grown where it is not small enough
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * K(ω) x = b(ω): K = C - k0² Σ_g ε_g(ω) M_g + K_pml(ω) - C the curl's and
 * M_g each material's mass (of ε 1) over the elements outside the PMLs,
 * K_pml(ω) the PMLs' elements' whole (their stretching ω's). On V, the
 * solutions at the wavelengths solved (orthonormal): x ≈ V y, (Vᵀ K V) y =
 * Vᵀ b - Vᵀ (no conjugate), K's own symmetry kept (reciprocity). Vᵀ C V and
 * Vᵀ M_g V made once (grown with V); Vᵀ K_pml V at each wavelength, of its
 * elements alone. The ports' modes at every wavelength (side by side); each
 * reduced solution's result as an exact one's (resultOfSolves).
 */
#include "fem3_wave.h"
#include "fem3_wave_setup.h"

#include "fem3_fe.h"
#include "fem_sparse.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <QCoreApplication>
#include <QElapsedTimer>

#if defined(QUCS_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace qucs_s::fem {

namespace {

using cd = std::complex<double>;
using RowSparse = Eigen::SparseMatrix<cd, Eigen::RowMajor>;
const cd J(0, 1);
constexpr double C0 = 299792458.0;

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Wave3", text);
}

int threadCount()
{
    return std::clamp(int(std::thread::hardware_concurrency()), 1, 32);
}

/// C = op(A) B, op the transpose (\a conjugate: and conjugated) or none - on
/// the machine's threads (Accelerate's BLAS where it is, Eigen's elsewhere).
void gemm(const Eigen::MatrixXcd& A, bool transpose, bool conjugate, const Eigen::MatrixXcd& B, Eigen::MatrixXcd& C)
{
    const int m = transpose ? int(A.cols()) : int(A.rows()), k = transpose ? int(A.rows()) : int(A.cols()), n = int(B.cols());
    C.resize(m, n);
    if (m == 0 || n == 0) return;
    if (k == 0) {
        C.setZero();
        return;
    }
#if defined(QUCS_ACCELERATE)
    const cd one(1), zero(0);
    cblas_zgemm(CblasColMajor, transpose ? (conjugate ? CblasConjTrans : CblasTrans) : CblasNoTrans, CblasNoTrans, m, n, k, &one, A.data(),
                int(A.rows()), B.data(), int(B.rows()), &zero, C.data(), int(C.rows()));
#else
    if (!transpose) C.noalias() = A * B;
    else if (conjugate) C.noalias() = A.adjoint() * B;
    else C.noalias() = A.transpose() * B;
#endif
}

/// Y = S X, S's rows on the machine's threads.
void multiply(const RowSparse& S, const Eigen::MatrixXcd& X, Eigen::MatrixXcd& Y)
{
    const int n = int(S.rows()), c = int(X.cols());
    Y.resize(n, c);
    const int threads = threadCount(), rows = std::max(1, (n + threads * 8 - 1) / (threads * 8));
    std::atomic<int> next{0};
    auto work = [&] {
        for (int r0 = next++ * rows; r0 < n; r0 = next++ * rows)
            for (int r = r0; r < std::min(n, r0 + rows); ++r)
                for (int j = 0; j < c; ++j) {
                    cd s = 0;
                    for (RowSparse::InnerIterator it(S, r); it; ++it) s += it.value() * X(it.col(), j);
                    Y(r, j) = s;
                }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
}

} // namespace

std::vector<Wave3Result> sweepWave3(const Model& model, std::shared_ptr<ParameterScope> parameters, const Topology3& topology,
                                    std::shared_ptr<const Mesh3> mesh, const Wave3Settings& settings, const std::vector<double>& frequencies,
                                    QStringList* log, QString* error)
{
    QElapsedTimer timer;
    timer.start();
    std::vector<Wave3Result> out(frequencies.size());
    auto tell = [&](double f, const QString& what) {
        if (settings.progress) settings.progress(f, what);
    };
    auto cancelled = [&] { return settings.cancelled && settings.cancelled(); };
    if (frequencies.empty()) return out;
    if (!parameters) parameters = evaluateParameters(model);
    const int Nf = int(frequencies.size());
    const double fHi = *std::max_element(frequencies.begin(), frequencies.end());
    // The setup at the shortest wavelength (the mesh checked there), its
    // materials at each one's as each is solved.
    wave3::Setup su;
    {
        QString why;
        if (!wave3::setUp(model, *parameters, topology, mesh, settings, C0 / fHi, &su, &why)) {
            *error = why;
            return out;
        }
        if (log) *log << su.log;
    }
    const bool backed = su.ports.front().backed;
    for (const wave3::Port& p : su.ports)
        if (p.backed != backed) {
            *error = tr("some ports on the outside, some inside with a PML behind them - all one or the other");
            return out;
        }
    const int N = su.N;
    const EdgeSpace3& space = *su.space;
    const int per = space.perElement();
    const PrismRule& rule = settings.order == 1 ? prismRule(2, 2) : prismRule(4, 3);
    std::vector<std::vector<cd>> epsOf(static_cast<std::size_t>(Nf));
    for (int k = 0; k < Nf; ++k) {
        wave3::atWavelength(su, *parameters, C0 / frequencies[std::size_t(k)]);
        epsOf[std::size_t(k)] = su.eps;
    }
    // The ports at every wavelength: their modes side by side (a cut some
    // gigabyte at resolution 30: as many at once as half the memory, or the
    // limit, holds), each one's columns and what is taken, fed, carrying.
    std::vector<wave3::PortsAt> ports(static_cast<std::size_t>(Nf));
    std::vector<QString> failed(static_cast<std::size_t>(Nf));
    {
        tell(0.02, tr("the ports' modes at each wavelength"));
        const double perCut = 1.5e9 * std::pow(std::max(1.0, settings.portResolution) / 30.0, 2) * (settings.order == 1 ? 0.5 : 1.0);
        const double budget = settings.memoryLimit > 0 ? settings.memoryLimit : physicalMemory() / 2;
        const int lanes = std::clamp(int(budget / perCut), 1, std::min(threadCount(), Nf));
        std::atomic<int> next{0}, done{0};
        std::atomic<bool> stop{false};
        std::mutex telling;
        auto work = [&] {
            for (int k = next++; k < Nf; k = next++) {
                if (stop || cancelled()) {
                    stop = true;
                    return;
                }
                const double f = frequencies[std::size_t(k)];
                std::vector<EmeModes> modes(su.ports.size());
                QString why;
                for (std::size_t pi = 0; pi < su.ports.size() && why.isEmpty(); ++pi) {
                    modes[pi] = wave3::portModes(model, parameters, topology, su, settings, int(pi), f, std::max(1, settings.ports[pi].absorbed), &why);
                    if (why.isEmpty() && modes[pi].count() == 0) why = tr("no mode found");
                    if (!why.isEmpty()) why = tr("port %1 at %2: %3").arg(settings.ports[pi].name, formatQuantity(C0 / f, Dim::length()), why);
                }
                if (why.isEmpty())
                    if (!wave3::portsAt(model, parameters, topology, su, settings, f, epsOf[std::size_t(k)], std::move(modes), &ports[std::size_t(k)], &why))
                        why = tr("at %1: %2").arg(formatQuantity(C0 / f, Dim::length()), why);
                if (!why.isEmpty()) {
                    failed[std::size_t(k)] = why;
                    stop = true;
                }
                const int finished = ++done;
                std::lock_guard<std::mutex> lock(telling);
                tell(0.02 + 0.18 * finished / Nf, tr("the ports' modes at %1 of %2 wavelengths (%3 at once)").arg(finished).arg(Nf).arg(lanes));
            }
        };
        std::vector<std::thread> pool;
        for (int t = 1; t < lanes; ++t) pool.emplace_back(work);
        work();
        for (std::thread& t : pool) t.join();
        for (const QString& f : failed)
            if (!f.isEmpty()) {
                *error = f;
                return out;
            }
        if (cancelled()) {
            *error = tr("cancelled");
            return out;
        }
    }
    const qint64 portsMs = timer.elapsed();
    // The elements outside the PMLs and inside them; the curl's and each
    // material's mass outside them, made once.
    std::vector<int> bulk, pmlElements;
    for (int e = 0; e < space.elements(); ++e) (su.pml.count(mesh->domain[std::size_t(e)]) ? pmlElements : bulk).push_back(e);
    std::vector<int> groupOf(su.eps.size(), -1);
    std::vector<const Node*> groupMaterial;
    for (std::size_t d = 0; d < su.material.size(); ++d) {
        if (su.pml.count(int(d))) continue;
        auto it = std::find(groupMaterial.begin(), groupMaterial.end(), su.material[d]);
        if (it == groupMaterial.end()) {
            groupOf[d] = int(groupMaterial.size());
            groupMaterial.push_back(su.material[d]);
        } else
            groupOf[d] = int(it - groupMaterial.begin());
    }
    const int G = int(groupMaterial.size());
    std::vector<int> groupDomain(static_cast<std::size_t>(G), -1);   // a domain of each group (its ε)
    for (std::size_t d = 0; d < groupOf.size(); ++d)
        if (groupOf[d] >= 0 && groupDomain[std::size_t(groupOf[d])] < 0) groupDomain[std::size_t(groupOf[d])] = int(d);
    RowSparse C;
    std::vector<RowSparse> M(static_cast<std::size_t>(G));
    {
        tell(0.21, tr("the curl's and the materials' matrices"));
        const double kRef = 2 * M_PI * fHi / C0;
        Eigen::SparseMatrix<cd> A;
        const std::vector<cd> ones(su.eps.size(), cd(1));
        if (bulk.empty()) A = Eigen::SparseMatrix<cd>(N, N);
        else if (!wave3::assembleElements(su, bulk, [&](int e, Eigen::MatrixXcd& Ke) { wave3::elementMatrix(su, e, 0.0, ones, rule, Ke); }, false,
                                          cancelled, &A)) {
            *error = tr("cancelled");
            return out;
        }
        C = A;
        for (int g = 0; g < G; ++g) {
            std::vector<int> of;
            for (int e : bulk)
                if (groupOf[std::size_t(mesh->domain[std::size_t(e)])] == g) of.push_back(e);
            if (of.empty()) {
                M[std::size_t(g)] = RowSparse(N, N);
                continue;
            }
            // (Its mass of ε 1: the curl's less the whole at the band's own
            // k0, over k0² - at k0 = 1 m⁻¹ the curl's entries, some 1e14 the
            // mass's, left it rounding alone.)
            if (!wave3::assembleElements(
                    su, of,
                    [&](int e, Eigen::MatrixXcd& Ke) {
                        Eigen::MatrixXcd K1(per, per);
                        wave3::elementMatrix(su, e, 0.0, ones, rule, Ke);
                        wave3::elementMatrix(su, e, kRef, ones, rule, K1);
                        Ke = (Ke - K1) / (kRef * kRef);
                    },
                    false, cancelled, &A)) {
                *error = tr("cancelled");
                return out;
            }
            M[std::size_t(g)] = A;
        }
    }
    // The PMLs' rows (their elements' dofs): where K_pml has entries.
    std::vector<int> pmlRows;
    {
        std::vector<char> in(std::size_t(N), 0);
        for (int e : pmlElements) {
            const int* dofs = space.dofs(e);
            for (int i = 0; i < per; ++i)
                if (!su.fixed[std::size_t(dofs[i])]) in[std::size_t(dofs[i])] = 1;
        }
        for (int i = 0; i < N; ++i)
            if (in[std::size_t(i)]) pmlRows.push_back(i);
    }
    auto pmlMatrix = [&](int k, RowSparse* Kp) -> bool {
        wave3::atWavelength(su, *parameters, C0 / frequencies[std::size_t(k)]);
        // (No PML: nothing - an empty list is every element to assembleElements.)
        if (pmlElements.empty()) {
            *Kp = RowSparse(N, N);
            return true;
        }
        const double k0 = 2 * M_PI * frequencies[std::size_t(k)] / C0;
        Eigen::SparseMatrix<cd> A;
        if (!wave3::assembleElements(su, pmlElements, [&](int e, Eigen::MatrixXcd& Ke) { wave3::elementMatrix(su, e, k0, su.eps, rule, Ke); }, false,
                                     cancelled, &A))
            return false;
        *Kp = A;
        return true;
    };
    // The basis, its projections.
    Eigen::MatrixXcd V(N, 0);
    Eigen::MatrixXcd Cr(0, 0);
    double factorBytes = 0;   // the last factorization's
    bool capped = false;
    std::vector<Eigen::MatrixXcd> Mr(static_cast<std::size_t>(G));
    auto grow = [&](const Eigen::MatrixXcd& X) {
        // Orthonormal to V (twice: classical Gram-Schmidt), its columns
        // whose rest is a millionth of their own left out - each of its own
        // size first: an evanescent port mode's response, small beside a
        // propagating one's, is no less a direction (dropped by the largest's
        // measure, its columns' residual stayed 1).
        Eigen::MatrixXcd W = X;
        for (int j = 0; j < W.cols(); ++j) {
            const double n = W.col(j).norm();
            if (n > 0) W.col(j) /= n;
        }
        const Eigen::VectorXd norms = Eigen::VectorXd::Ones(W.cols());
        for (int pass = 0; pass < 2 && V.cols() > 0; ++pass) {
            Eigen::MatrixXcd h;
            gemm(V, true, true, W, h);
            Eigen::MatrixXcd Vh;
            gemm(V, false, false, h, Vh);
            W -= Vh;
        }
        // (Among themselves: a QR with pivoting's columns, those that count.)
        Eigen::ColPivHouseholderQR<Eigen::MatrixXcd> qr(W);
        const Eigen::MatrixXcd R = qr.matrixR().template triangularView<Eigen::Upper>();
        int rank = 0;
        const double biggest = norms.size() ? norms.maxCoeff() : 0;
        for (int i = 0; i < std::min(int(R.rows()), int(R.cols())); ++i)
            if (std::abs(R(i, i)) > 1e-6 * biggest) ++rank;
        // (As many as the memory holds beside a factorization: the basis and
        // two of its size at work.)
        const double budget = (settings.memoryLimit > 0 ? settings.memoryLimit : physicalMemory() / 2) - factorBytes;
        const int room = std::max(16, int(budget / (3 * 16.0 * N))) - int(V.cols());
        if (rank > room) {
            if (!capped && log)
                *log << tr("The fast sweep's basis kept to %1 columns (the memory beside a factorization): more wavelengths solved exactly instead")
                            .arg(int(V.cols()) + std::max(0, room));
            capped = true;
            rank = std::max(0, room);
        }
        if (rank == 0) return 0;
        const Eigen::MatrixXcd Q = qr.householderQ() * Eigen::MatrixXcd::Identity(W.rows(), rank);
        // The projections grown: [Vᵀ A V, Vᵀ A Q; Qᵀ A V, Qᵀ A Q].
        const int r0 = int(V.cols());
        auto growProjection = [&](const RowSparse& A, Eigen::MatrixXcd& P) {
            Eigen::MatrixXcd AQ;
            multiply(A, Q, AQ);
            Eigen::MatrixXcd VtAQ, QtAQ;
            gemm(V, true, false, AQ, VtAQ);
            gemm(Q, true, false, AQ, QtAQ);
            Eigen::MatrixXcd next(r0 + rank, r0 + rank);
            next.topLeftCorner(r0, r0) = P;
            next.topRightCorner(r0, rank) = VtAQ;
            next.bottomLeftCorner(rank, r0) = VtAQ.transpose();
            next.bottomRightCorner(rank, rank) = QtAQ;
            P = next;
        };
        growProjection(C, Cr);
        for (int g = 0; g < G; ++g) growProjection(M[std::size_t(g)], Mr[std::size_t(g)]);
        V.conservativeResize(N, r0 + rank);
        V.rightCols(rank) = Q;
        return rank;
    };
    // An exact solve at wavelength k: its result, its solutions into the
    // basis.
    std::vector<char> exact(static_cast<std::size_t>(Nf), 0);
    double factorSeconds = 0, momentSeconds = 0;
    QString factorName;
    auto solveExactly = [&](int k) -> bool {
        const double f = frequencies[std::size_t(k)], k0 = 2 * M_PI * f / C0;
        wave3::atWavelength(su, *parameters, C0 / f);
        Eigen::SparseMatrix<cd> K;
        if (!wave3::assembleElements(su, {}, [&](int e, Eigen::MatrixXcd& Ke) { wave3::elementMatrix(su, e, k0, su.eps, rule, Ke); }, true, cancelled, &K)) {
            *error = tr("cancelled");
            return false;
        }
        QElapsedTimer t;
        t.start();
        SparseComplexLdlt lu;
        lu.setMemoryLimit(settings.memoryLimit);
        lu.compute(K);
        if (lu.info() != Eigen::Success) {
            *error = tr("at %1: the system could not be factorized (%2)").arg(formatQuantity(C0 / f, Dim::length()), QString::fromStdString(lu.lastErrorMessage()));
            return false;
        }
        factorName = lu.name();
        factorBytes = std::max(factorBytes, lu.factorBytes());
        const Eigen::MatrixXcd B = wave3::rightHandSides(su, ports[std::size_t(k)], backed);
        Eigen::MatrixXcd X(N, B.cols());
        for (int c0 = 0; c0 < B.cols(); c0 += 16) {
            const int nb = std::min(16, int(B.cols()) - c0);
            X.middleCols(c0, nb) = lu.solveMany(B.middleCols(c0, nb));
        }
        factorSeconds += t.elapsed() / 1000.0;
        Wave3Result& r = out[std::size_t(k)];
        r = Wave3Result();
        r.frequency = f;
        r.space = su.space;
        r.unknowns = N;
        r.portModes = ports[std::size_t(k)].portModes;
        r.warnings = ports[std::size_t(k)].warnings;
        wave3::resultOfSolves(su, ports[std::size_t(k)], settings, f, backed, X, &r);
        exact[std::size_t(k)] = 1;
        grow(X);
        // Its derivatives in k0, the factorization reused - each a solve, a
        // second where the factorization took forty: x' = K⁻¹ (b' - K' x),
        // then x⁽ʲ⁺¹⁾ ≈ -K⁻¹ K' x⁽ʲ⁾ (moments: the basis then follows the
        // solution away from the wavelengths solved, not just through
        // them). K' of K at k0 (1 ± 1e-4); b' of the next wavelengths'
        // right-hand sides, where they are alike.
        const int moments = settings.sweepMoments;
        if (moments > 0) {
            QElapsedTimer tm;
            tm.start();
            const double d = 1e-4 * k0;
            Eigen::SparseMatrix<cd> Kplus, Kminus;
            for (int side = 0; side < 2; ++side) {
                const double kk = side == 0 ? k0 + d : k0 - d;
                wave3::atWavelength(su, *parameters, 2 * M_PI / kk);
                if (!wave3::assembleElements(su, {}, [&](int e, Eigen::MatrixXcd& Ke) { wave3::elementMatrix(su, e, kk, su.eps, rule, Ke); }, false, cancelled,
                                             side == 0 ? &Kplus : &Kminus)) {
                    *error = tr("cancelled");
                    return false;
                }
            }
            const RowSparse Dk = RowSparse((Kplus - Kminus) * (1.0 / (2 * d)));
            Kplus = Eigen::SparseMatrix<cd>();
            Kminus = Eigen::SparseMatrix<cd>();
            // b' of the neighbours (by frequency) where theirs are of B's shape.
            Eigen::MatrixXcd dB = Eigen::MatrixXcd::Zero(N, B.cols());
            {
                int lo = -1, hi = -1;
                for (int q = 0; q < Nf; ++q) {
                    const double fq = frequencies[std::size_t(q)];
                    if (fq < f && (lo < 0 || fq > frequencies[std::size_t(lo)])) lo = q;
                    if (fq > f && (hi < 0 || fq < frequencies[std::size_t(hi)])) hi = q;
                }
                const int a = lo >= 0 ? lo : k, b = hi >= 0 ? hi : k;
                if (a != b) {
                    const Eigen::MatrixXcd Ba = a == k ? B : wave3::rightHandSides(su, ports[std::size_t(a)], backed);
                    const Eigen::MatrixXcd Bb = b == k ? B : wave3::rightHandSides(su, ports[std::size_t(b)], backed);
                    if (Ba.cols() == B.cols() && Bb.cols() == B.cols())
                        dB = (Bb - Ba) / (2 * M_PI * (frequencies[std::size_t(b)] - frequencies[std::size_t(a)]) / C0);
                }
            }
            Eigen::MatrixXcd xj = X, DX;
            for (int j = 1; j <= moments; ++j) {
                multiply(Dk, xj, DX);
                Eigen::MatrixXcd rhs = -DX;
                if (j == 1) rhs += dB;
                for (int i = 0; i < N; ++i)
                    if (su.fixed[std::size_t(i)]) rhs.row(i).setZero();
                Eigen::MatrixXcd next(N, rhs.cols());
                for (int c0 = 0; c0 < rhs.cols(); c0 += 16) {
                    const int nb = std::min(16, int(rhs.cols()) - c0);
                    next.middleCols(c0, nb) = lu.solveMany(rhs.middleCols(c0, nb));
                }
                grow(next);
                xj = next;
            }
            momentSeconds += tm.elapsed() / 1000.0;
        }
        return true;
    };
    // A reduced solve at wavelength k: its result, its residual.
    double reducedSeconds = 0;
    auto solveReduced = [&](int k, double* residual) -> bool {
        QElapsedTimer t;
        t.start();
        const double f = frequencies[std::size_t(k)], k0 = 2 * M_PI * f / C0;
        RowSparse Kp;
        if (!pmlMatrix(k, &Kp)) {
            *error = tr("cancelled");
            return false;
        }
        const int r = int(V.cols());
        // Vᵀ K_pml V of the PMLs' rows alone.
        Eigen::MatrixXcd Kr = Cr;
        for (int g = 0; g < G; ++g) Kr -= (k0 * k0) * epsOf[std::size_t(k)][std::size_t(groupDomain[std::size_t(g)])] * Mr[std::size_t(g)];
        {
            const int np = int(pmlRows.size());
            Eigen::MatrixXcd KpV;
            multiply(Kp, V, KpV);
            Eigen::MatrixXcd Vp(np, r), Yp(np, r);
            for (int i = 0; i < np; ++i) {
                Vp.row(i) = V.row(pmlRows[std::size_t(i)]);
                Yp.row(i) = KpV.row(pmlRows[std::size_t(i)]);
            }
            Eigen::MatrixXcd P;
            gemm(Vp, true, false, Yp, P);
            Kr += P;
        }
        const Eigen::MatrixXcd B = wave3::rightHandSides(su, ports[std::size_t(k)], backed);
        Eigen::MatrixXcd Br;
        gemm(V, true, false, B, Br);
        const Eigen::MatrixXcd y = Kr.partialPivLu().solve(Br);
        Eigen::MatrixXcd X;
        gemm(V, false, false, y, X);
        // Its residual, K x - b, of b's size: the column worst so.
        {
            Eigen::MatrixXcd R, T;
            multiply(C, X, R);
            for (int g = 0; g < G; ++g) {
                multiply(M[std::size_t(g)], X, T);
                R -= (k0 * k0) * epsOf[std::size_t(k)][std::size_t(groupDomain[std::size_t(g)])] * T;
            }
            multiply(Kp, X, T);
            R += T - B;
            double worst = 0;
            for (int j = 0; j < int(B.cols()); ++j) {
                const double nb = B.col(j).norm();
                if (nb > 0) worst = std::max(worst, R.col(j).norm() / nb);
            }
            *residual = worst;
        }
        Wave3Result& res = out[std::size_t(k)];
        res = Wave3Result();
        res.frequency = f;
        res.space = su.space;
        res.unknowns = N;
        res.portModes = ports[std::size_t(k)].portModes;
        res.warnings = ports[std::size_t(k)].warnings;
        wave3::resultOfSolves(su, ports[std::size_t(k)], settings, f, backed, X, &res);
        reducedSeconds += t.elapsed() / 1000.0;
        return true;
    };
    // The wavelengths by frequency; the first solved exactly its band's ends
    // and middle, then where the residual is largest, until every one's is
    // under the tolerance.
    std::vector<int> byFrequency(static_cast<std::size_t>(Nf));
    for (int k = 0; k < Nf; ++k) byFrequency[std::size_t(k)] = k;
    std::sort(byFrequency.begin(), byFrequency.end(), [&](int a, int b) { return frequencies[std::size_t(a)] < frequencies[std::size_t(b)]; });
    const double tolerance = settings.sweepTolerance > 0 ? settings.sweepTolerance : 1e-5;
    std::vector<int> start;
    if (Nf <= 3) start = byFrequency;
    else start = {byFrequency.front(), byFrequency[std::size_t(Nf / 2)], byFrequency.back()};
    int solvedExactly = 0;
    auto progressOf = [&] { return 0.25 + 0.7 * double(solvedExactly) / std::max(1, std::min(Nf, 8)); };
    for (int k : start) {
        if (cancelled()) {
            *error = tr("cancelled");
            return out;
        }
        tell(std::min(0.9, progressOf()), tr("solved exactly at %1").arg(formatQuantity(C0 / frequencies[std::size_t(k)], Dim::length())));
        if (!solveExactly(k)) return out;
        ++solvedExactly;
    }
    std::vector<double> residual(static_cast<std::size_t>(Nf), 0.0);
    // (Done when every residual is under the tolerance, or when the last
    // wavelength solved exactly - where the residual was largest - moved no
    // other one's S by more than it: an absorbing port's S, Galerkin's G =
    // Uᵀ K⁻¹ U, right to some residual squared - a junction's to 9 digits
    // where the residual was 1e-4 -, a port with a PML behind it's to some
    // residual.)
    int rounds = 0;
    double change = -1;
    std::vector<Eigen::MatrixXcd> before(static_cast<std::size_t>(Nf));
    for (;;) {
        double worst = 0;
        int at = -1;
        int evaluated = 0;
        change = -1;
        for (int k = 0; k < Nf; ++k) {
            if (exact[std::size_t(k)]) continue;
            if (cancelled()) {
                *error = tr("cancelled");
                return out;
            }
            if (!solveReduced(k, &residual[std::size_t(k)])) return out;
            ++evaluated;
            tell(std::min(0.95, progressOf()), tr("the reduced basis of %1 at %2 of %3 wavelengths").arg(V.cols()).arg(evaluated).arg(Nf - solvedExactly));
            if (residual[std::size_t(k)] > worst) {
                worst = residual[std::size_t(k)];
                at = k;
            }
            const Eigen::MatrixXcd& S = out[std::size_t(k)].S;
            if (before[std::size_t(k)].size() == S.size()) change = std::max(change, (S - before[std::size_t(k)]).cwiseAbs().maxCoeff());
            else change = std::numeric_limits<double>::infinity();
            before[std::size_t(k)] = S;
        }
        ++rounds;
        if (at < 0 || worst <= tolerance || (change >= 0 && change <= tolerance)) break;
        tell(std::min(0.95, progressOf()), tr("solved exactly at %1 (its residual %2)").arg(formatQuantity(C0 / frequencies[std::size_t(at)], Dim::length())).arg(worst, 0, 'g', 2));
        if (!solveExactly(at)) return out;
        ++solvedExactly;
    }
    double largest = 0;
    for (int k = 0; k < Nf; ++k)
        if (!exact[std::size_t(k)]) largest = std::max(largest, residual[std::size_t(k)]);
    if (log) {
        QStringList at;
        for (int k : byFrequency)
            if (exact[std::size_t(k)]) at << formatQuantity(C0 / frequencies[std::size_t(k)], Dim::length());
        *log << tr("Fast sweep: %1 wavelengths, %2 solved exactly (%3; %4 %5 s, their derivatives %15 s), the rest on their solutions' reduced "
                   "basis of %6 - its largest residual %7, S moved %16 by the last solved (tolerance %8), %9 rounds, %10 s; %11 unknowns (order %12), "
                   "the ports' modes %13 s; %14 s in all")
                    .arg(Nf)
                    .arg(solvedExactly)
                    .arg(at.join(QStringLiteral(", ")))
                    .arg(factorName)
                    .arg(factorSeconds, 0, 'f', 1)
                    .arg(V.cols())
                    .arg(largest, 0, 'g', 2)
                    .arg(tolerance, 0, 'g', 2)
                    .arg(rounds)
                    .arg(reducedSeconds, 0, 'f', 1)
                    .arg(N)
                    .arg(settings.order)
                    .arg(portsMs / 1000.0, 0, 'f', 1)
                    .arg(timer.elapsed() / 1000.0, 0, 'f', 1)
                    .arg(momentSeconds, 0, 'f', 1)
                    .arg(change >= 0 && std::isfinite(change) ? QString::number(change, 'g', 2) : tr("-"));
    }
    tell(1.0, tr("done"));
    return out;
}

} // namespace qucs_s::fem
