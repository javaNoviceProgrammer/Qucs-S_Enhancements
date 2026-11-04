// amg_smoothers.cpp <system> <config>: a symmetric system solved by AMGCL's
// conjugate gradients, preconditioned one way, to 1e-9 (400 iterations at
// most): its setup, solve, iterations, residual and the hierarchy's memory.
// <system> is laplace:<n> or contrast:<n> - phase C's benchmark, the P1
// Laplacian of an n x n x n grid of cubes cut into six tetrahedra (Kuhn), the
// bottom held, copper (k 400) in oxide (k 1.4) for contrast - or a file the
// solver's matrix was written to while probing (int64 n, nnz; int ptr[n+1],
// col[nnz]; double val[nnz], rhs[n]; scaled as AmgSolver scales them).
// <config>: sa-ilu0 (phase C's), sa-ilu0-2, sa-ilu0-eps0, sa-ilu0-eps25,
// sa-iluk1, sa-ilut, sa-gs, sa-gs-2, sa-gs-3, sa-cheb, rs-gs, rs-ilu0, em-ilu0,
// ag-ilu0 (sa smoothed aggregation, rs Ruge-Stuben, em its energy-minimising
// variant, ag plain aggregation; -2 two sweeps, eps the strength threshold).
// Built on its own, not part of the tree:
//   c++ -std=c++20 -O2 -DNDEBUG -DAMGCL_NO_BOOST -isystem third_party/eigen \
//       -isystem third_party/amgcl/include amg_smoothers.cpp
// amg_smoothers.tsv: what it gave on 4 November 2026 (M2 Ultra, one thread).
#include <amgcl/adapter/crs_tuple.hpp>
#include <amgcl/amg.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/coarsening/aggregation.hpp>
#include <amgcl/coarsening/ruge_stuben.hpp>
#include <amgcl/coarsening/smoothed_aggr_emin.hpp>
#include <amgcl/coarsening/smoothed_aggregation.hpp>
#include <amgcl/make_solver.hpp>
#include <amgcl/relaxation/chebyshev.hpp>
#include <amgcl/relaxation/damped_jacobi.hpp>
#include <amgcl/relaxation/gauss_seidel.hpp>
#include <amgcl/relaxation/ilu0.hpp>
#include <amgcl/relaxation/iluk.hpp>
#include <amgcl/relaxation/ilut.hpp>
#include <amgcl/relaxation/spai0.hpp>
#include <amgcl/solver/cg.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

using Backend = amgcl::backend::builtin<double>;
using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

struct Sys {
    long long n = 0, nz = 0;
    std::vector<int> ptr, col;
    std::vector<double> val, rhs;
};

template <template <class> class Coarsening, template <class> class Relax, class Tweak>
void run(const Sys& s, const char* name, Tweak tweak)
{
    using Solver = amgcl::make_solver<amgcl::amg<Backend, Coarsening, Relax>, amgcl::solver::cg<Backend>>;
    typename Solver::params prm;
    prm.solver.tol = 1e-9;
    prm.solver.maxiter = 400;
    tweak(prm);
    const ptrdiff_t n = ptrdiff_t(s.n);
    auto A = std::make_tuple(n, amgcl::make_iterator_range(s.ptr.data(), s.ptr.data() + n + 1),
                             amgcl::make_iterator_range(s.col.data(), s.col.data() + s.nz),
                             amgcl::make_iterator_range(s.val.data(), s.val.data() + s.nz));
    const auto t0 = Clock::now();
    Solver solve(A, prm);
    const double setup = since(t0);
    std::vector<double> x(std::size_t(n), 0.0);
    const auto t1 = Clock::now();
    size_t it = 0;
    double res = 0;
    std::tie(it, res) = solve(s.rhs, x);
    std::printf("%-34s setup %6.2f s  solve %7.2f s  %4zu it  residual %.2e  %.0f MB\n", name, setup, since(t1), it, res,
                solve.precond().bytes() / 1048576.0);
    std::fflush(stdout);
}


#include <Eigen/Dense>
#include <Eigen/SparseCore>
#include <array>
static void laplacian(int n, bool contrast, Sys& s)
{
    const int N = n + 1;
    auto node = [N](int i, int j, int k) { return (k * N + j) * N + i; };
    std::vector<int> unk(std::size_t(N) * N * N, -1);
    int nu = 0;
    for (int k = 1; k < N; ++k)
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) unk[std::size_t(node(i, j, k))] = nu++;
    const double h = 1.0 / n;
    const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    std::vector<Eigen::Triplet<double>> t;
    Eigen::VectorXd b = Eigen::VectorXd::Zero(nu);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const double cx = (i + 0.5) * h, cy = (j + 0.5) * h, cz = (k + 0.5) * h;
                const double kc = contrast ? ((cx > 0.3 && cx < 0.7 && cy > 0.3 && cy < 0.7 && cz > 0.4) ? 400.0 : 1.4) : 1.0;
                for (const auto& p : perm) {
                    std::array<std::array<int, 3>, 4> v{};
                    for (int q = 0; q < 3; ++q) {
                        v[std::size_t(q + 1)] = v[std::size_t(q)];
                        v[std::size_t(q + 1)][std::size_t(p[q])] += 1;
                    }
                    Eigen::Matrix3d J;
                    for (int c = 0; c < 3; ++c)
                        for (int r = 0; r < 3; ++r) J(r, c) = (v[std::size_t(c + 1)][std::size_t(r)] - v[0][std::size_t(r)]) * h;
                    const double vol = std::abs(J.determinant()) / 6;
                    const Eigen::Matrix3d G = J.inverse().transpose();
                    Eigen::Vector3d g[4] = {-(G.col(0) + G.col(1) + G.col(2)), G.col(0), G.col(1), G.col(2)};
                    int id[4];
                    for (int a = 0; a < 4; ++a) id[a] = unk[std::size_t(node(i + v[std::size_t(a)][0], j + v[std::size_t(a)][1], k + v[std::size_t(a)][2]))];
                    for (int a = 0; a < 4; ++a) {
                        if (id[a] < 0) continue;
                        b(id[a]) += vol / 4;
                        for (int c = 0; c < 4; ++c)
                            if (id[c] >= 0) t.emplace_back(id[a], id[c], kc * vol * g[a].dot(g[c]));
                    }
                }
            }
    Eigen::SparseMatrix<double, Eigen::RowMajor> A(nu, nu);
    A.setFromTriplets(t.begin(), t.end());
    A.makeCompressed();
    s.n = nu;
    s.nz = A.nonZeros();
    s.ptr.assign(A.outerIndexPtr(), A.outerIndexPtr() + nu + 1);
    s.col.assign(A.innerIndexPtr(), A.innerIndexPtr() + A.nonZeros());
    s.val.assign(A.valuePtr(), A.valuePtr() + A.nonZeros());
    const double nb = b.norm();
    s.rhs.assign(b.data(), b.data() + nu);
    for (double& v : s.rhs) v /= nb;
}

int main(int argc, char** argv)
{
    if (argc < 3) return 1;
    Sys s;
    const std::string src = argv[1];
    if (src.rfind("laplace:", 0) == 0 || src.rfind("contrast:", 0) == 0) {
        laplacian(std::stoi(src.substr(src.find(':') + 1)), src[0] == 'c', s);
    } else {
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fread(&s.n, 8, 1, f);
    std::fread(&s.nz, 8, 1, f);
    s.ptr.resize(std::size_t(s.n + 1));
    s.col.resize(std::size_t(s.nz));
    s.val.resize(std::size_t(s.nz));
    s.rhs.resize(std::size_t(s.n));
    std::fread(s.ptr.data(), sizeof(int), s.ptr.size(), f);
    std::fread(s.col.data(), sizeof(int), s.col.size(), f);
    std::fread(s.val.data(), 8, s.val.size(), f);
    std::fread(s.rhs.data(), 8, s.rhs.size(), f);
    std::fclose(f);
    }
    std::printf("%lld unknowns, %lld nonzeros\n", s.n, s.nz);
    const std::string c = argv[2];
    if (c == "sa-ilu0") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::ilu0>(s, "SA ilu0 (now)", [](auto&) {});
    if (c == "sa-ilu0-2") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::ilu0>(s, "SA ilu0 npre=npost=2", [](auto& p) { p.precond.npre = p.precond.npost = 2; });
    if (c == "sa-ilu0-eps0") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::ilu0>(s, "SA ilu0 eps 0", [](auto& p) { p.precond.coarsening.aggr.eps_strong = 0; });
    if (c == "sa-ilu0-eps25") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::ilu0>(s, "SA ilu0 eps 0.25", [](auto& p) { p.precond.coarsening.aggr.eps_strong = 0.25; });
    if (c == "sa-iluk1") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::iluk>(s, "SA iluk(1)", [](auto& p) { p.precond.relax.k = 1; });
    if (c == "sa-ilut") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::ilut>(s, "SA ilut", [](auto&) {});
    if (c == "sa-gs-3") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::gauss_seidel>(s, "SA gauss_seidel 3", [](auto& p) { p.precond.npre = p.precond.npost = 3; });
    if (c == "sa-gs") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::gauss_seidel>(s, "SA gauss_seidel", [](auto&) {});
    if (c == "sa-gs-2") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::gauss_seidel>(s, "SA gauss_seidel 2", [](auto& p) { p.precond.npre = p.precond.npost = 2; });
    if (c == "sa-cheb") run<amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::chebyshev>(s, "SA chebyshev", [](auto&) {});
    if (c == "rs-gs") run<amgcl::coarsening::ruge_stuben, amgcl::relaxation::gauss_seidel>(s, "RS gauss_seidel", [](auto&) {});
    if (c == "rs-ilu0") run<amgcl::coarsening::ruge_stuben, amgcl::relaxation::ilu0>(s, "RS ilu0", [](auto&) {});
    if (c == "em-ilu0") run<amgcl::coarsening::smoothed_aggr_emin, amgcl::relaxation::ilu0>(s, "SA-emin ilu0", [](auto&) {});
    if (c == "ag-ilu0") run<amgcl::coarsening::aggregation, amgcl::relaxation::ilu0>(s, "aggregation ilu0", [](auto&) {});
    return 0;
}
