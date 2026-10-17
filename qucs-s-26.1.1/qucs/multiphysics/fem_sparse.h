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

/// The sparse direct solver asked for: Automatic - Accelerate's from
/// acceleratedFrom() unknowns where this macOS has it, Eigen's below -,
/// Eigen's at every size, or Accelerate's at every size (where it is).
enum class DirectSolver { Automatic, Eigen, Accelerate };
/// The app's choice (Multiphysics > Settings…), for the whole process.
void setDirectSolver(DirectSolver solver);
/// What is in effect: QUCS_FEM_SOLVER (auto, eigen or accelerate) over the
/// app's choice.
DirectSolver directSolver();
/// Which factorizes: Accelerate - its LU on macOS 26 and later in a build
/// whose SDK has it (on macOS 15.7 it stopped the program in semiconductor
/// solves: CI's runner), its Cholesky from macOS 10.13 - or Eigen.
bool acceleratedLu();
bool acceleratedCholesky();
/// This build has Accelerate (macOS) - its Cholesky at least.
bool accelerateInBuild();
/// Why Accelerate's LU cannot be taken here (empty: it can) - the build or
/// the macOS; not the choice.
QString acceleratedLuHindrance();
/// Accelerate's from this many unknowns, Eigen's below: its analysis costs
/// some eight factorizations, a matrix of 2000 unknowns 35 ms against
/// Eigen's 5 for the whole - one solve of fewer than some 20 000 is
/// Eigen's quicker (QUCS_FEM_ACCELERATE_FROM another; 0 - and the choice of
/// Accelerate - always).
int acceleratedFrom();
/// "Accelerate" or "Eigen" (the LU's), for the log.
QString directSolverName();

/// The factorizations made so far, by solver (a step's: the difference
/// before and after it; points solved at once count together).
struct SolverUse {
    long long accelerateLu = 0, eigenLu = 0, accelerateCholesky = 0, eigenCholesky = 0, iterative = 0;
};
SolverUse solverUse();
/// Conjugate gradients' solves (a LinearSystem's), counted.
void countIterative();
/// Those used between \a before and \a after, by name ("Accelerate's LU,
/// Eigen's Cholesky"); empty where none.
QString solverUseText(const SolverUse& before, const SolverUse& after);
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
    /// Each factorization's condition checked too (either backend's): over
    /// 1e15, its rows and columns scaled, said singular to rounding. For a
    /// matrix whose poor conditioning is no physics' own - a PDE's; not a
    /// semiconductor's Jacobian (the suite's to 1e22, solved well) nor a
    /// shift-and-invert's (near singular by design). Off by default.
    void setSingularChecked(bool on);
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
    /// Fails where the matrix is not positive definite - or is singular to
    /// rounding (a potential left floating: its pivots no better than
    /// rounding), said by singularToRounding().
    void factorize(const Matrix& a);
    Eigen::ComputationInfo info() const;
    bool singularToRounding() const;
    std::string lastErrorMessage() const;
    Eigen::VectorXd solve(const Eigen::VectorXd& b) const;

private:
    struct Impl;
    std::unique_ptr<Impl> a_impl;
};

extern template class SparseLu<double>;
extern template class SparseLu<std::complex<double>>;

} // namespace qucs_s::fem

#endif
