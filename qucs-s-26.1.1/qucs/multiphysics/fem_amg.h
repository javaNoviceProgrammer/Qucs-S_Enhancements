/*
 * fem_amg.h - algebraic multigrid for the multiphysics solver: conjugate
 *             gradients preconditioned by AMGCL's smoothed aggregation
 *             (third_party/amgcl), for the 3D models' symmetric positive
 *             definite systems, the incomplete Cholesky's the fallback
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_AMG_H
#define QUCS_FEM_AMG_H

#include <Eigen/SparseCore>

#include <QString>

#include <cstddef>
#include <memory>

namespace qucs_s::fem {

/// How an iterative solve went.
struct IterativeReport {
    int iterations = 0;
    double residual = 0;      ///< relative: |b - A x| / |b|
    bool converged = false;
    bool fallback = false;    ///< multigrid did not converge: the incomplete Cholesky's
    bool cancelled = false;   ///< stopped by its caller (GMRES's check)
};

/*!
 * A symmetric positive definite system A x = b solved by conjugate gradients
 * preconditioned by algebraic multigrid - AMGCL's smoothed aggregation with
 * a symmetric Gauss-Seidel smoother, measured on 3D Laplacians to a million
 * unknowns and on copper beside oxide: some 11 to 15 iterations at every
 * size, where an incomplete Cholesky's grow from 92 to 368; and on order 2's
 * stretched prisms, a chip's thin films and a VCSEL array's. Each right-hand
 * side is scaled to a norm of one, so
 * that a system of any unit solves alike. Set up once (compute), solved for
 * as many right-hand sides as wanted, each from the x given. Where it does not
 * converge, conjugate gradients with an incomplete Cholesky try instead (the
 * report says so).
 */
class AmgSolver
{
public:
    AmgSolver();
    ~AmgSolver();
    AmgSolver(const AmgSolver&) = delete;
    AmgSolver& operator=(const AmgSolver&) = delete;

    /// The relative residual it stops at (1e-9).
    void setTolerance(double tolerance);
    /// Its iterations at most (500; the fallback's ten times as many).
    void setMaxIterations(int iterations);
    /// A matrix that is not symmetric (Newton's method's): BiCGStab with the
    /// same multigrid over its rows, an incomplete LU's BiCGStab the
    /// fallback. Before compute().
    void setNonsymmetric(bool nonsymmetric);
    /// The near null space its coarse levels keep: \a cols vectors, \a b
    /// row by row (b[i * cols + j] row i's entry of vector j) - an elastic
    /// solid's six rigid-body modes, without which smoothed aggregation
    /// converges slowly. Empty: the constant. Before compute().
    void setNearNullspace(int cols, std::vector<double> b, int blockSize = 1);
    /// Sets matrix \a a up - its hierarchy; false and why when it cannot.
    bool compute(const Eigen::SparseMatrix<double>& a, QString* error);
    /// Solves for \a b from \a x as it is (zeros: none known); false and why
    /// when neither method converged (x the last iterate then).
    bool solve(const Eigen::VectorXd& b, Eigen::VectorXd& x, IterativeReport* report, QString* error);
    /// The memory its hierarchy takes (bytes; 0: none set up).
    std::size_t bytes() const;
    /// The size of the matrix set up.
    int size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> a_impl;
};

/*!
 * One V-cycle of AMGCL's smoothed aggregation, symmetric Gauss-Seidel its
 * smoother: a symmetric positive definite matrix's approximate inverse, for
 * a preconditioner made of several (the auxiliary spaces' of an edge
 * element system, fem3_ams). Linear in what it is applied to.
 */
class AmgCycle
{
public:
    AmgCycle();
    ~AmgCycle();
    AmgCycle(const AmgCycle&) = delete;
    AmgCycle& operator=(const AmgCycle&) = delete;

    /// Rows in blocks of \a size - a node's three components - aggregated
    /// together, each component's constant kept. Before compute().
    void setBlockSize(int size);
    /// Sets symmetric matrix \a a up; false and why when it cannot.
    bool compute(const Eigen::SparseMatrix<double>& a, QString* error);
    /// \a x = one cycle applied to \a r, from nought.
    void apply(const Eigen::VectorXd& r, Eigen::VectorXd& x) const;
    /// The memory its hierarchy takes (bytes).
    std::size_t bytes() const;
    int size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> a_impl;
};

} // namespace qucs_s::fem

#endif // QUCS_FEM_AMG_H
