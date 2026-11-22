/*
 * fem3_ams.h - an iterative solve of edge element systems: the auxiliary
 *              space preconditioner of Hiptmair and Xu (AMS) for a real
 *              symmetric positive definite curl-curl matrix, its smoother's
 *              Gauss-Seidel on the edges and algebraic multigrid on the
 *              nodal spaces its gradients and vector fields come from; and
 *              restarted GMRES, preconditioned on the right, for a complex
 *              (or real) system
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_AMS_H
#define QUCS_FEM3_AMS_H

#include "fem_amg.h"

#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include <QString>

#include <complex>
#include <functional>
#include <memory>
#include <vector>

namespace qucs_s::fem::detail3 {

/// A preconditioner's cycle: an approximate inverse of a symmetric
/// positive definite matrix, linear in what it is applied to.
class Cycle
{
public:
    virtual ~Cycle() = default;
    /// \a x = the cycle applied to \a r.
    virtual void apply(const Eigen::VectorXd& r, Eigen::VectorXd& x) const = 0;
    virtual std::size_t bytes() const = 0;
};

/// A cycle of a matrix, made: the matrix it is to be of given.
using CycleMaker = std::function<std::unique_ptr<Cycle>(const Eigen::SparseMatrix<double>& a, QString* error)>;

/// Algebraic multigrid's V-cycle as a Cycle (AmgCycle; \a blockSize rows
/// aggregated together).
CycleMaker multigrid(int blockSize = 1);

/*!
 * An auxiliary space cycle (Hiptmair and Xu, SIAM J. Numer. Anal. 45 (2007)
 * 2483; Kolev and Vassilevski, J. Comput. Math. 27 (2009) 604): symmetric
 * Gauss-Seidel on matrix A, forward \a sweeps times; then a correction in
 * each space P of \a order in turn - a cycle of Pᵀ A P, the residual's
 * restriction prolonged back; then Gauss-Seidel backward. Symmetric where
 * \a order reads the same both ways. Edge elements' (AMS, its "01210"): the
 * gradients G of the nodal space, its vector fields Π, G again. Quadratic
 * nodal elements': the linear ones, a two-level p-multigrid.
 */
class AuxiliaryCycle : public Cycle
{
public:
    struct Space {
        Eigen::SparseMatrix<double> p;   ///< rows A's, columns its own
        CycleMaker make;
    };
    bool compute(const Eigen::SparseMatrix<double>& a, std::vector<Space> spaces, std::vector<int> order, int sweeps, QString* error);
    void apply(const Eigen::VectorXd& r, Eigen::VectorXd& x) const override;
    std::size_t bytes() const override;

private:
    void smooth(const Eigen::VectorXd& r, Eigen::VectorXd& d, bool forward) const;
    Eigen::SparseMatrix<double, Eigen::RowMajor> a_a;
    Eigen::VectorXd a_diag;
    std::vector<Eigen::SparseMatrix<double>> a_p;
    std::vector<std::unique_ptr<Cycle>> a_inner;
    std::vector<int> a_order;
    int a_sweeps = 1;
};

/// A linear operator on complex vectors: y = Op(x).
using ComplexOperator = std::function<void(const Eigen::VectorXcd&, Eigen::VectorXcd&)>;

/// Restarted GMRES (Saad and Schultz, SIAM J. Sci. Stat. Comput. 7 (1986)
/// 856) for A x = b, preconditioned on the right by \a m: its residual the
/// true one's, |b - A x| / |b| under \a tolerance; from \a x as given (zeros:
/// none known). \a restart vectors kept; \a maxIterations at most;
/// \a cancelled asked at each iteration (true: stopped, said so).
IterativeReport gmres(const ComplexOperator& a, const ComplexOperator& m, const Eigen::VectorXcd& b, Eigen::VectorXcd& x, double tolerance,
                      int maxIterations, int restart = 60, const std::function<bool()>& cancelled = {});

} // namespace qucs_s::fem::detail3

#endif // QUCS_FEM3_AMS_H
