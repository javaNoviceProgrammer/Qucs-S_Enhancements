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
#include <amgcl/solver/bicgstab.hpp>
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
// (A matrix not symmetric: BiCGStab, its smoother ILU(0) - no cycle need be
// symmetric for it.)
using Bicg = amgcl::make_solver<amgcl::amg<Backend, amgcl::coarsening::smoothed_aggregation, amgcl::relaxation::ilu0>, amgcl::solver::bicgstab<Backend>>;
using FallbackLu = Eigen::BiCGSTAB<Eigen::SparseMatrix<double>, Eigen::IncompleteLUT<double>>;

/// The smoother: Gauss-Seidel, forward before the coarse level and backward
/// after, so that the cycle conjugate gradients are preconditioned by is
/// symmetric - or (QUCS_FEM_AMG_RELAX, for measuring) SPAI-0 or ILU(0).
/// AMGCL's ILU(0) is the same factor both ways, a cycle that is not: order 2's
/// matrices on stretched prisms are no M-matrices, and with it the conjugate
/// gradients stalled - a VCSEL array's 1,209,654 unknowns (prisms 30 µm wide
/// and 0.5 thick) at a residual of 3e-4 after 400 iterations, two sweeps or a
/// strength of 0 or 0.25 alike, and the chip heater's 55,272 short of 1e-9
/// after 400 (phase C's 52 were of one mesh); Gauss-Seidel 156 and 102. On
/// the plain Laplacian of 270,400 unknowns, copper in oxide too, it is as
/// quick (14 and 15 iterations against 12 and 13, 10% less time, 27% less
/// memory). SPAI-0 ran past 500 on the chip at 160,357 unknowns.
int relaxation()
{
    static const int which = [] {
        const QByteArray v = qgetenv("QUCS_FEM_AMG_RELAX");
        return v == "spai0" ? 0 : v == "ilu0" ? 2 : 1;
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
    bool nonsymmetric = false;
    // (The matrix kept: the fallback's, and the residual's.)
    Eigen::SparseMatrix<double> a;
    // (One of them, by the smoother; a matrix not symmetric BiCGStab's.)
    std::unique_ptr<Spai0> spai0;
    std::unique_ptr<GaussSeidel> gs;
    std::unique_ptr<Ilu0> ilu0;
    std::unique_ptr<Bicg> bicg;
    bool ready() const { return spai0 || gs || ilu0 || bicg; }
    void reset()
    {
        spai0.reset();
        gs.reset();
        ilu0.reset();
        bicg.reset();
    }
    // (AMGCL's own tuple: libstdc++ makes no pair of a tuple, as libc++ does.)
    std::tuple<std::size_t, double> operator()(const std::vector<double>& rhs, std::vector<double>& x) const
    {
        return bicg ? (*bicg)(rhs, x) : spai0 ? (*spai0)(rhs, x) : gs ? (*gs)(rhs, x) : (*ilu0)(rhs, x);
    }
    std::unique_ptr<Fallback> ic;
    std::unique_ptr<FallbackLu> ilut;
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

void AmgSolver::setNonsymmetric(bool nonsymmetric)
{
    a_impl->nonsymmetric = nonsymmetric;
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
    d.ilut.reset();
    d.bytes = 0;
    d.a = a;
    d.a.makeCompressed();
    if (d.a.rows() == 0) return true;
    // (Symmetric: its compressed columns are its rows. Not: its rows, made -
    // read as rows, a column-major matrix is its transpose.)
    Eigen::SparseMatrix<double, Eigen::RowMajor> rows;
    if (d.nonsymmetric) {
        rows = d.a;
        rows.makeCompressed();
    }
    const ptrdiff_t n = d.a.rows();
    const int* outer = d.nonsymmetric ? rows.outerIndexPtr() : d.a.outerIndexPtr();
    const int* inner = d.nonsymmetric ? rows.innerIndexPtr() : d.a.innerIndexPtr();
    const double* values = d.nonsymmetric ? rows.valuePtr() : d.a.valuePtr();
    auto ptr = amgcl::make_iterator_range(outer, outer + n + 1);
    auto col = amgcl::make_iterator_range(inner, inner + d.a.nonZeros());
    auto val = amgcl::make_iterator_range(values, values + d.a.nonZeros());
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
        if (d.nonsymmetric) make(d.bicg);
        else
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
    // The right-hand side scaled to a norm of one, and the guess with it:
    // AMGCL takes a right-hand side under the machine's epsilon for nought
    // and answers nought - in 0 iterations, its norm for the residual, so
    // that it passed for converged. A 3D electrostatics' entries are ε0 times
    // a length: in µm a finger cell's right-hand side was some 1e-17, and its
    // capacitance came out 7 to 21 times too large, the more as its mesh was
    // refined. (The rest of a solve is of the matrix's scale alone - its
    // aggregates, its smoother, its coarse level, the fallback's factor.)
    const double norm = b.norm();
    if (n == 0 || norm == 0 || !std::isfinite(norm)) {
        x.setZero();
        r.converged = n == 0 || norm == 0;
        if (report) *report = r;
        if (!r.converged && error) *error = tr("the right-hand side is not finite");
        return r.converged;
    }
    const Eigen::VectorXd bn = b / norm;
    if (d.ready()) {
        std::vector<double> rhs(bn.data(), bn.data() + n), sol(n);
        for (ptrdiff_t i = 0; i < n; ++i) sol[std::size_t(i)] = x(i) / norm;
        try {
            std::size_t iterations = 0;
            double residual = 0;
            std::tie(iterations, residual) = d(rhs, sol);
            r.iterations = int(iterations);
            r.residual = residual;
            r.converged = residual <= d.tolerance * 1.0000001 && std::isfinite(residual);
            if (r.converged) {
                x = Eigen::Map<const Eigen::VectorXd>(sol.data(), n) * norm;
                if (report) *report = r;
                return true;
            }
        } catch (const std::exception&) {
        }
    }
    // The incomplete Cholesky's conjugate gradients, from x as given (a
    // matrix not symmetric: an incomplete LU's BiCGStab).
    r.fallback = true;
    if (d.nonsymmetric) {
        if (!d.ilut) {
            d.ilut = std::make_unique<FallbackLu>();
            d.ilut->setTolerance(d.tolerance);
            d.ilut->setMaxIterations(10 * d.maxIterations);
            d.ilut->compute(d.a);
        }
        const Eigen::VectorXd guess = x / norm;
        x = d.ilut->solveWithGuess(bn, guess) * norm;
        r.iterations += int(d.ilut->iterations());
        r.residual = d.ilut->error();
        r.converged = d.ilut->info() == Eigen::Success && x.allFinite();
        if (report) *report = r;
        if (!r.converged && error)
            *error = tr("the iterative solve did not converge: a relative residual of %1 after %2 iterations").arg(r.residual, 0, 'g', 3).arg(r.iterations);
        return r.converged;
    }
    if (!d.ic) {
        d.ic = std::make_unique<Fallback>();
        d.ic->setTolerance(d.tolerance);
        d.ic->setMaxIterations(10 * d.maxIterations);
        d.ic->compute(d.a);
    }
    const Eigen::VectorXd guess = x / norm;
    x = d.ic->solveWithGuess(bn, guess) * norm;
    r.iterations += int(d.ic->iterations());
    r.residual = d.ic->error();
    r.converged = d.ic->info() == Eigen::Success && x.allFinite();
    if (report) *report = r;
    if (!r.converged && error)
        *error = tr("the iterative solve did not converge: a relative residual of %1 after %2 iterations").arg(r.residual, 0, 'g', 3).arg(r.iterations);
    return r.converged;
}

} // namespace qucs_s::fem
