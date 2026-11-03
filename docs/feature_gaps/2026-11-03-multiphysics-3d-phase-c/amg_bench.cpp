// amg_bench.cpp <n> <case> <method>: the P1 Laplacian of an n x n x n grid of
// cubes, each cut into six tetrahedra (Kuhn), the bottom face held - the
// proposal's benchmark (docs/proposals/2026-11-02-multiphysics-3d.md) - solved
// one way, to 1e-9: its setup, solve, iterations, residual and the process's
// peak memory (MB before the solve, and its peak). case: laplace (k = 1) or
// contrast (a copper block, k = 400, in oxide, k = 1.4). method: ic (Eigen's
// conjugate gradients with an incomplete Cholesky), amg-spai0, amg-gs, amg-ilu0
// (AMGCL's smoothed aggregation preconditioning conjugate gradients),
// chol-default, chol-metis (Accelerate's Cholesky in its order and METIS's).
// Built on its own (not part of the tree), one method a process for its peak:
//   c++ -std=c++20 -O2 -DNDEBUG -DAMGCL_NO_BOOST -isystem third_party/eigen \
//       -isystem third_party/amgcl/include amg_bench.cpp -framework Accelerate
// amg_bench.tsv: n = 30, 40, 50, 64, 80, 100, both cases, each method (the
// tree's order to 80 alone), on an M2 Ultra, 3 November 2026.
#include <Eigen/Dense>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCore>

#include <amgcl/adapter/crs_tuple.hpp>
#include <amgcl/amg.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/coarsening/smoothed_aggregation.hpp>
#include <amgcl/make_solver.hpp>
#include <amgcl/relaxation/gauss_seidel.hpp>
#include <amgcl/relaxation/ilu0.hpp>
#include <amgcl/relaxation/spai0.hpp>
#include <amgcl/solver/cg.hpp>

#include <Accelerate/Accelerate.h>

#include <sys/resource.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using SpMat = Eigen::SparseMatrix<double>;
using Clock = std::chrono::steady_clock;

static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

static double peakMB()
{
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return double(u.ru_maxrss) / (1024.0 * 1024.0);   // bytes on macOS
}

template <class Precond>
static void runAmg(const SpMat& A, const Eigen::VectorXd& b, Eigen::VectorXd& x, double& setup, double& solve, int& iters)
{
    using Backend = amgcl::backend::builtin<double>;
    using Solver = amgcl::make_solver<amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation, Precond::template type>,
                                      amgcl::solver::cg<Backend>>;
    typename Solver::params prm;
    prm.solver.tol = 1e-9;
    prm.solver.maxiter = 2000;
    const ptrdiff_t n = A.rows();
    auto ptr = amgcl::make_iterator_range(A.outerIndexPtr(), A.outerIndexPtr() + n + 1);
    auto col = amgcl::make_iterator_range(A.innerIndexPtr(), A.innerIndexPtr() + A.nonZeros());
    auto val = amgcl::make_iterator_range(A.valuePtr(), A.valuePtr() + A.nonZeros());
    auto t0 = Clock::now();
    Solver s(std::make_tuple(n, ptr, col, val), prm);
    setup = since(t0);
    std::vector<double> rhs(b.data(), b.data() + n), sol(std::size_t(n), 0.0);
    t0 = Clock::now();
    double err = 0;
    size_t it = 0;
    std::tie(it, err) = s(rhs, sol);
    solve = since(t0);
    iters = int(it);
    x = Eigen::Map<Eigen::VectorXd>(sol.data(), n);
}

template <template <class> class R> struct Relax {
    template <class B> using type = R<B>;
};

int main(int argc, char** argv)
{
    if (argc < 4) return 2;
    const int n = std::atoi(argv[1]);
    const std::string kase = argv[2], method = argv[3];
    const int N = n + 1;
    auto node = [N](int i, int j, int k) { return (k * N + j) * N + i; };
    // Unknowns: every node above the bottom.
    std::vector<int> unk(std::size_t(N) * N * N, -1);
    int nu = 0;
    for (int k = 1; k < N; ++k)
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) unk[std::size_t(node(i, j, k))] = nu++;
    const double h = 1.0 / n;
    // Kuhn's six tetrahedra of the unit cube: the paths from (0,0,0) to (1,1,1).
    const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(std::size_t(n) * n * n * 6 * 16);
    Eigen::VectorXd b = Eigen::VectorXd::Zero(nu);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                double kc = 1;
                if (kase == "contrast") {
                    const double cx = (i + 0.5) * h, cy = (j + 0.5) * h, cz = (k + 0.5) * h;
                    kc = (cx > 0.3 && cx < 0.7 && cy > 0.3 && cy < 0.7 && cz > 0.4) ? 400.0 : 1.4;
                }
                for (const auto& p : perm) {
                    std::array<std::array<int, 3>, 4> v{};
                    v[0] = {0, 0, 0};
                    for (int s = 0; s < 3; ++s) {
                        v[std::size_t(s + 1)] = v[std::size_t(s)];
                        v[std::size_t(s + 1)][std::size_t(p[s])] += 1;
                    }
                    // Gradients of the barycentric coordinates.
                    Eigen::Matrix3d J;
                    for (int c = 0; c < 3; ++c)
                        for (int r = 0; r < 3; ++r) J(r, c) = (v[std::size_t(c + 1)][std::size_t(r)] - v[0][std::size_t(r)]) * h;
                    const double vol = std::abs(J.determinant()) / 6;
                    const Eigen::Matrix3d G = J.inverse().transpose();
                    Eigen::Vector3d g[4];
                    g[1] = G.col(0);
                    g[2] = G.col(1);
                    g[3] = G.col(2);
                    g[0] = -(g[1] + g[2] + g[3]);
                    int id[4];
                    for (int a = 0; a < 4; ++a)
                        id[a] = unk[std::size_t(node(i + v[std::size_t(a)][0], j + v[std::size_t(a)][1], k + v[std::size_t(a)][2]))];
                    for (int a = 0; a < 4; ++a) {
                        if (id[a] < 0) continue;
                        b(id[a]) += vol / 4;
                        for (int c = 0; c < 4; ++c)
                            if (id[c] >= 0) trip.emplace_back(id[a], id[c], kc * vol * g[a].dot(g[c]));
                    }
                }
            }
    SpMat A(nu, nu);
    A.setFromTriplets(trip.begin(), trip.end());
    A.makeCompressed();
    trip.clear();
    trip.shrink_to_fit();
    const double before = peakMB();
    Eigen::VectorXd x;
    double setup = 0, solve = 0;
    int iters = 0;
    if (method == "ic") {
        auto t0 = Clock::now();
        Eigen::ConjugateGradient<SpMat, Eigen::Lower | Eigen::Upper, Eigen::IncompleteCholesky<double>> cg;
        cg.setTolerance(1e-9);
        cg.setMaxIterations(5000);
        cg.compute(A);
        setup = since(t0);
        t0 = Clock::now();
        x = cg.solve(b);
        solve = since(t0);
        iters = int(cg.iterations());
    } else if (method == "amg-spai0") {
        runAmg<Relax<amgcl::relaxation::spai0>>(A, b, x, setup, solve, iters);
    } else if (method == "amg-gs") {
        runAmg<Relax<amgcl::relaxation::gauss_seidel>>(A, b, x, setup, solve, iters);
    } else if (method == "amg-ilu0") {
        runAmg<Relax<amgcl::relaxation::ilu0>>(A, b, x, setup, solve, iters);
    } else if (method == "chol-default" || method == "chol-metis") {
        SpMat lower = A.triangularView<Eigen::Lower>();
        lower.makeCompressed();
        std::vector<long> starts(lower.outerIndexPtr(), lower.outerIndexPtr() + nu + 1);
        SparseAttributes_t at{};
        at.kind = SparseSymmetric;
        at.triangle = SparseLowerTriangle;
        SparseMatrixStructure st{nu, nu, starts.data(), lower.innerIndexPtr(), at, 1};
        SparseSymbolicFactorOptions so = _SparseDefaultSymbolicFactorOptions;
        if (method == "chol-metis") so.orderMethod = SparseOrderMetis;
        auto t0 = Clock::now();
        SparseOpaqueSymbolicFactorization sym = SparseFactor(SparseFactorizationCholesky, st, so);
        SparseOpaqueFactorization_Double num = SparseFactor(sym, SparseMatrix_Double{st, lower.valuePtr()});
        setup = since(t0);
        if (num.status != SparseStatusOK) {
            std::printf("factorization failed\n");
            return 1;
        }
        x.resize(nu);
        Eigen::VectorXd in = b;
        t0 = Clock::now();
        SparseSolve(num, DenseVector_Double{nu, in.data()}, DenseVector_Double{nu, x.data()});
        solve = since(t0);
        SparseCleanup(num);
        SparseCleanup(sym);
    } else {
        return 2;
    }
    const double res = (b - A * x).norm() / b.norm();
    std::printf("%d\t%d\t%ld\t%s\t%s\t%.3f\t%.3f\t%d\t%.1e\t%.0f\t%.0f\n", n, nu, long(A.nonZeros()), kase.c_str(), method.c_str(), setup,
                solve, iters, res, before, peakMB());
    return 0;
}
