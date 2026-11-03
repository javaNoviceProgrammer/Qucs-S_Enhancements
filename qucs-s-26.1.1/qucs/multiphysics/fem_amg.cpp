/*
 * fem_amg.cpp - algebraic multigrid for the multiphysics solver (AMGCL's
 *               smoothed aggregation preconditioning conjugate gradients)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_amg.h"

#include <amgcl/adapter/crs_tuple.hpp>
#include <amgcl/amg.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/coarsening/smoothed_aggregation.hpp>
#include <amgcl/make_solver.hpp>
#include <amgcl/relaxation/gauss_seidel.hpp>
#include <amgcl/relaxation/ilu0.hpp>
#include <amgcl/relaxation/spai0.hpp>
#include <amgcl/solver/cg.hpp>

#include <Eigen/IterativeLinearSolvers>

#include <QCoreApplication>

#include <cmath>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace qucs_s::fem {

namespace {

using Backend = amgcl::backend::builtin<double>;
template <template <class> class Relaxation>
using SolverOf = amgcl::make_solver<amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation, Relaxation>, amgcl::solver::cg<Backend>>;
using Spai0 = SolverOf<amgcl::relaxation::spai0>;
using GaussSeidel = SolverOf<amgcl::relaxation::gauss_seidel>;
using Ilu0 = SolverOf<amgcl::relaxation::ilu0>;

/// The smoother: ILU(0) - or (QUCS_FEM_AMG_RELAX, for measuring) SPAI-0 or
/// Gauss-Seidel. A layered chip's thin films stretch its prisms, and order 2's
/// matrices are no M-matrices: at 160,357 unknowns SPAI-0 ran past 500
/// iterations in two of three solves (the fallback finishing them, 35 s),
/// Gauss-Seidel took 146 and ILU(0) 52 (6.5 and 5.6 s); on a plain Laplacian
/// ILU(0) is the fewest iterations too, 15% slower than SPAI-0 at a million.
int relaxation()
{
    static const int which = [] {
        const QByteArray v = qgetenv("QUCS_FEM_AMG_RELAX");
        return v == "spai0" ? 0 : v == "gauss_seidel" ? 1 : 2;
    }();
    return which;
}
using Fallback = Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper, Eigen::IncompleteCholesky<double>>;

QString tr(const char* text)
{
    return QCoreApplication::translate("AmgSolver", text);
}

} // namespace

struct AmgSolver::Impl {
    double tolerance = 1e-9;
    int maxIterations = 500;
    // (The matrix kept: the fallback's, and the residual's.)
    Eigen::SparseMatrix<double> a;
    // (One of them, by the smoother.)
    std::unique_ptr<Spai0> spai0;
    std::unique_ptr<GaussSeidel> gs;
    std::unique_ptr<Ilu0> ilu0;
    bool ready() const { return spai0 || gs || ilu0; }
    void reset()
    {
        spai0.reset();
        gs.reset();
        ilu0.reset();
    }
    std::pair<std::size_t, double> operator()(const std::vector<double>& rhs, std::vector<double>& x) const
    {
        return spai0 ? (*spai0)(rhs, x) : gs ? (*gs)(rhs, x) : (*ilu0)(rhs, x);
    }
    std::unique_ptr<Fallback> ic;
    std::size_t bytes = 0;
};

AmgSolver::AmgSolver() : a_impl(std::make_unique<Impl>()) {}
AmgSolver::~AmgSolver() = default;

void AmgSolver::setTolerance(double tolerance)
{
    if (tolerance > 0) a_impl->tolerance = tolerance;
}

void AmgSolver::setMaxIterations(int iterations)
{
    if (iterations > 0) a_impl->maxIterations = iterations;
}

std::size_t AmgSolver::bytes() const
{
    return a_impl->bytes;
}

int AmgSolver::size() const
{
    return int(a_impl->a.rows());
}

bool AmgSolver::compute(const Eigen::SparseMatrix<double>& a, QString* error)
{
    Impl& d = *a_impl;
    d.reset();
    d.ic.reset();
    d.bytes = 0;
    d.a = a;
    d.a.makeCompressed();
    if (d.a.rows() == 0) return true;
    // (Symmetric: its compressed columns are its rows.)
    const ptrdiff_t n = d.a.rows();
    auto ptr = amgcl::make_iterator_range(d.a.outerIndexPtr(), d.a.outerIndexPtr() + n + 1);
    auto col = amgcl::make_iterator_range(d.a.innerIndexPtr(), d.a.innerIndexPtr() + d.a.nonZeros());
    auto val = amgcl::make_iterator_range(d.a.valuePtr(), d.a.valuePtr() + d.a.nonZeros());
    const auto matrix = std::make_tuple(n, ptr, col, val);
    try {
        // (Each smoother's solver its own type: the same parameters.)
        auto make = [&d, &matrix](auto& solver) {
            using S = typename std::decay_t<decltype(solver)>::element_type;
            typename S::params prm;
            prm.solver.tol = d.tolerance;
            prm.solver.maxiter = std::size_t(d.maxIterations);
            solver = std::make_unique<S>(matrix, prm);
            d.bytes = solver->precond().bytes();
        };
        switch (relaxation()) {
        case 0: make(d.spai0); break;
        case 1: make(d.gs); break;
        default: make(d.ilu0); break;
        }
    } catch (const std::exception& e) {
        // (A hierarchy that cannot be built - a zero on the diagonal: a
        // degree of freedom no element holds - is the fallback's.)
        d.reset();
        if (error) *error = tr("algebraic multigrid could not be set up: %1").arg(QString::fromLocal8Bit(e.what()));
        return false;
    }
    return true;
}

bool AmgSolver::solve(const Eigen::VectorXd& b, Eigen::VectorXd& x, IterativeReport* report, QString* error)
{
    Impl& d = *a_impl;
    IterativeReport r;
    const ptrdiff_t n = d.a.rows();
    if (x.size() != n) x = Eigen::VectorXd::Zero(n);
    if (n == 0 || b.norm() == 0) {
        x.setZero();
        r.converged = true;
        if (report) *report = r;
        return true;
    }
    if (d.ready()) {
        std::vector<double> rhs(b.data(), b.data() + n), sol(x.data(), x.data() + n);
        try {
            std::size_t iterations = 0;
            double residual = 0;
            std::tie(iterations, residual) = d(rhs, sol);
            r.iterations = int(iterations);
            r.residual = residual;
            r.converged = residual <= d.tolerance * 1.0000001 && std::isfinite(residual);
            if (r.converged) {
                x = Eigen::Map<const Eigen::VectorXd>(sol.data(), n);
                if (report) *report = r;
                return true;
            }
        } catch (const std::exception&) {
        }
    }
    // The incomplete Cholesky's conjugate gradients, from x as given.
    r.fallback = true;
    if (!d.ic) {
        d.ic = std::make_unique<Fallback>();
        d.ic->setTolerance(d.tolerance);
        d.ic->setMaxIterations(10 * d.maxIterations);
        d.ic->compute(d.a);
    }
    const Eigen::VectorXd guess = x;
    x = d.ic->solveWithGuess(b, guess);
    r.iterations += int(d.ic->iterations());
    r.residual = d.ic->error();
    r.converged = d.ic->info() == Eigen::Success && x.allFinite();
    if (report) *report = r;
    if (!r.converged && error)
        *error = tr("the iterative solve did not converge: a relative residual of %1 after %2 iterations").arg(r.residual, 0, 'g', 3).arg(r.iterations);
    return r.converged;
}

} // namespace qucs_s::fem
