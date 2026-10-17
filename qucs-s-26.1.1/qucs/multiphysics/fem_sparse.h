/*
 * fem_sparse.h - the multiphysics solver's sparse direct factorizations:
 *                Apple Accelerate's (supernodal, multithreaded) where there
 *                is one, Eigen's (SparseLU, SimplicialLDLT) else
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_SPARSE_H
#define QUCS_FEM_SPARSE_H

#include <Eigen/SparseCore>

#include <QString>

#include <complex>
#include <memory>
#include <string>

namespace qucs_s::fem {

/// Which factorizes: Accelerate (macOS 15.5 and later, a build whose SDK
/// has its LU; its Cholesky from macOS 10.13), or Eigen. QUCS_FEM_SOLVER=eigen
/// asks for Eigen's where Accelerate is there (to compare).
bool acceleratedLu();
bool acceleratedCholesky();
/// Accelerate's from this many unknowns, Eigen's below: its analysis costs
/// some eight factorizations, a matrix of 2000 unknowns 35 ms against
/// Eigen's 5 for the whole - one solve of fewer than some 20 000 is
/// Eigen's quicker (QUCS_FEM_ACCELERATE_FROM another; 0: always).
int acceleratedFrom();
/// "Accelerate" or "Eigen" (the LU's), for the log.
QString directSolverName();
/// Accelerate's LU analyses made so far - not those its cache gave again
/// (for the tests).
int acceleratedAnalysesMade();

/*!
 * A sparse LU of a square matrix (column-major, compressed or not), as
 * Eigen's SparseLU is used: its pattern analysed once (analyzePattern), its
 * values factorized as often as they change (factorize: the same pattern),
 * solved; info() and lastErrorMessage() after each. Accelerate's LU (with
 * threshold partial pivoting) where acceleratedLu(), its symbolic stage
 * kept and its numeric one done again (SparseRefactor); else Eigen's
 * SparseLU (COLAMD).
 */
template <typename Scalar>
class SparseLu
{
public:
    using Matrix = Eigen::SparseMatrix<Scalar>;
    using Vector = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
    SparseLu();
    ~SparseLu();
    SparseLu(const SparseLu&) = delete;
    SparseLu& operator=(const SparseLu&) = delete;

    void analyzePattern(const Matrix& a);
    void factorize(const Matrix& a);
    void compute(const Matrix& a)
    {
        analyzePattern(a);
        factorize(a);
    }
    Eigen::ComputationInfo info() const;
    std::string lastErrorMessage() const;
    Vector solve(const Vector& b) const;
    /// Accelerate's (else Eigen's), as analysed last.
    bool accelerated() const;
    /// Its patterns analysed so far (a pattern within the one analysed is
    /// not analysed again; for the tests).
    int analyses() const;

private:
    struct Impl;
    std::unique_ptr<Impl> a_impl;
};

/*!
 * A sparse Cholesky (LL^T) of a symmetric matrix that may not be positive
 * definite: info() a failure where it is not (as Eigen's SimplicialLDLT with
 * a pivot of nought or less - the caller then takes an LU). The matrix given
 * whole (both triangles; its lower one read). Accelerate's supernodal
 * Cholesky where acceleratedCholesky(), else Eigen's SimplicialLDLT.
 */
class SparseCholesky
{
public:
    using Matrix = Eigen::SparseMatrix<double>;
    SparseCholesky();
    ~SparseCholesky();
    SparseCholesky(const SparseCholesky&) = delete;
    SparseCholesky& operator=(const SparseCholesky&) = delete;

    void analyzePattern(const Matrix& a);
    /// Fails where the matrix is not positive definite.
    void factorize(const Matrix& a);
    Eigen::ComputationInfo info() const;
    Eigen::VectorXd solve(const Eigen::VectorXd& b) const;

private:
    struct Impl;
    std::unique_ptr<Impl> a_impl;
};

extern template class SparseLu<double>;
extern template class SparseLu<std::complex<double>>;

} // namespace qucs_s::fem

#endif
