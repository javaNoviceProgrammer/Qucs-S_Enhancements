/*
 * fem_sparse.h - the multiphysics solver's sparse direct factorizations:
 *                Apple Accelerate's (supernodal, multithreaded) where there
 *                is one, MUMPS's or STRUMPACK's (multithreaded, by OpenMP)
 *                where the build found them, Eigen's (SparseLU,
 *                SimplicialLDLT) else
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

/// The sparse direct solver asked for: Automatic - the platform's from its
/// size up (Accelerate's where this macOS has it, else MUMPS's, else
/// STRUMPACK's, where the build has them), Eigen's below -, Eigen's at
/// every size, Accelerate's at every size (where it is), MUMPS's or
/// STRUMPACK's from their sizes up (backendFrom(); where the build has
/// them - Eigen's else).
enum class DirectSolver { Automatic, Eigen, Accelerate, Mumps, Strumpack };
/// The app's choice (Multiphysics > Settings…), for the whole process.
void setDirectSolver(DirectSolver solver);
/// What is in effect: QUCS_FEM_SOLVER (auto, eigen, accelerate, mumps or
/// strumpack) over the app's choice.
DirectSolver directSolver();
/// The libraries that factorize.
enum class Backend { Eigen, Accelerate, Mumps, Strumpack };
/// Which factorizes a matrix of its size or more (backendFrom()) now - the
/// choice, the build and the macOS: by LU (real and complex; a complex
/// symmetric one too, by MUMPS's LDLT or Accelerate's), and by Cholesky (a
/// symmetric positive definite matrix: Accelerate's, MUMPS's - its LDLT
/// without pivoting -, STRUMPACK's LU, Eigen's LDLT).
Backend luBackend();
Backend choleskyBackend();
/// The same for \a solver chosen (Multiphysics > Settings… says what each
/// would take).
Backend luBackendFor(DirectSolver solver);
Backend choleskyBackendFor(DirectSolver solver);
/// "Accelerate", "MUMPS", "STRUMPACK" or "Eigen".
QString backendName(Backend backend);
/// From this many unknowns \a backend factorizes, Eigen's below (\a atOnce:
/// one of many solved at once, each on a thread of its own): Accelerate's
/// acceleratedFrom() or acceleratedFromAtOnce(); MUMPS's and STRUMPACK's
/// mumpsFrom() and strumpackFrom(); Eigen's 0.
int backendFrom(Backend backend, bool atOnce = false);
/// A Cholesky on many threads where a large one factorizes (not Eigen's):
/// a 3D model's heat or electrostatics solved directly by itself where its
/// matrix has many right-hand sides.
bool threadedCholesky();
/// Which factorizes: Accelerate - its LU on macOS 26 and later in a build
/// whose SDK has it (on macOS 15.7 it stopped the program in semiconductor
/// solves: CI's runner), its Cholesky from macOS 10.13 - or another
/// (luBackend(), choleskyBackend()).
bool acceleratedLu();
bool acceleratedCholesky();
/// The build has MUMPS's (sequential, multithreaded where it was built with
/// OpenMP) and STRUMPACK's (multithreaded by OpenMP's tasks) - found by
/// CMake (QUCS_WITH_MUMPS, QUCS_WITH_STRUMPACK).
bool mumpsInBuild();
bool strumpackInBuild();
/// Why MUMPS's or STRUMPACK's cannot be taken here (empty: it can).
QString mumpsHindrance();
QString strumpackHindrance();
/// MUMPS's from this many unknowns, Eigen's below - its analysis (METIS's
/// ordering) as Accelerate's costs several factorizations of a small
/// matrix (QUCS_FEM_MUMPS_FROM another); \a atOnce: one of many solved at
/// once, each on a thread of its own - the calls into MUMPS one at a time,
/// Eigen's LUs side by side quicker to a larger size. STRUMPACK's likewise
/// (QUCS_FEM_STRUMPACK_FROM).
int mumpsFrom(bool atOnce = false);
int strumpackFrom(bool atOnce = false);
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
/// The same for a matrix of many solved at once, each on a thread of its
/// own (the array scan's cells): Eigen's LUs on their threads together
/// quicker than Accelerate's one after another below some 50 000 unknowns
/// (rows: a complex system's, its degrees of freedom twice as many) - a
/// scan of 32 cells of 21k complex unknowns 0.34 s against 1.02; 44k 1.19
/// against 1.35; 56k 1.89 against 1.71; 82k 3.40 against 2.27; 504k 106
/// against 20 (QUCS_FEM_ACCELERATE_FROM another for both; the choice of
/// Accelerate 0).
int acceleratedFromAtOnce();
/// "Accelerate", "MUMPS", "STRUMPACK" or "Eigen" (the LU's: luBackend()),
/// for the log.
QString directSolverName();

/// The factorizations made so far, by solver (a step's: the difference
/// before and after it; points solved at once count together).
struct SolverUse {
    long long accelerateLu = 0, eigenLu = 0, accelerateCholesky = 0, eigenCholesky = 0, iterative = 0;
    long long mumpsLu = 0, mumpsLdlt = 0, mumpsCholesky = 0, strumpackLu = 0;
};
SolverUse solverUse();
/// Conjugate gradients' solves (a LinearSystem's), counted.
void countIterative();
/// Those used between \a before and \a after, by name ("Accelerate's LU,
/// Eigen's Cholesky", "MUMPS's LDLT"); empty where none.
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
 * kept and its numeric one done again (SparseRefactor); MUMPS's (threshold
 * pivoting, METIS's ordering) or STRUMPACK's (METIS's ordering, a
 * matching, pivoting within its fronts) where luBackend() says, analysed
 * at its first factorization (with its values) and again at a pattern
 * outside the one analysed; else Eigen's SparseLU (COLAMD). Each from its
 * size up (backendFrom()), Eigen's below.
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
    /// Many right-hand sides at once, a column each (Accelerate's solve of
    /// them together: the factors read once).
    using DenseMatrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    DenseMatrix solveMany(const DenseMatrix& b) const;
    /// Accelerate's (else another's), as analysed last.
    bool accelerated() const;
    /// Which, as analysed last.
    Backend backend() const;
    /// "Accelerate's LU", "MUMPS's LU", "STRUMPACK's LU" or "Eigen's LU".
    QString name() const;
    /// Each factorization's condition checked too (either backend's): over
    /// 1e15, its rows and columns scaled, said singular to rounding. For a
    /// matrix whose poor conditioning is no physics' own - a PDE's; not a
    /// semiconductor's Jacobian (the suite's to 1e22, solved well) nor a
    /// shift-and-invert's (near singular by design). Off by default.
    void setSingularChecked(bool on);
    /// One of many solved at once, each on a thread of its own: Accelerate's
    /// from acceleratedFromAtOnce() unknowns, not acceleratedFrom().
    void setManyAtOnce(bool on);
    /// Accelerate's analysis ordered by METIS, not its default - a 3D
    /// model's curl-curl system of second-order edge elements: at 75 000
    /// unknowns its LU 1.7 times quicker in four fifths of the memory. Before
    /// analyzePattern(); Eigen's takes its own (COLAMD).
    void setMetisOrdering(bool on);
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
 * Cholesky where acceleratedCholesky(); MUMPS's LDLT without pivoting (a
 * negative pivot: not positive definite) or STRUMPACK's LU (any matrix it
 * can factorize, positive definite or not) where choleskyBackend() says;
 * else Eigen's SimplicialLDLT - each from its size up.
 */
class SparseCholesky
{
public:
    using Matrix = Eigen::SparseMatrix<double>;
    SparseCholesky();
    ~SparseCholesky();
    SparseCholesky(const SparseCholesky&) = delete;
    SparseCholesky& operator=(const SparseCholesky&) = delete;

    /// The fill-reducing ordering of Accelerate's analysis: its default, or
    /// METIS's - a 3D model's: at a million unknowns of a 3D Laplacian 2.6
    /// times less memory and 3.7 times quicker (the proposal of 2
    /// November). Before analyzePattern(); Eigen's takes its own (AMD).
    enum class Ordering { Default, Metis };
    void setOrdering(Ordering ordering);
    void analyzePattern(const Matrix& a);
    /// Fails where the matrix is not positive definite - or is singular to
    /// rounding (a potential left floating: its pivots no better than
    /// rounding), said by singularToRounding().
    void factorize(const Matrix& a);
    Eigen::ComputationInfo info() const;
    bool singularToRounding() const;
    std::string lastErrorMessage() const;
    Eigen::VectorXd solve(const Eigen::VectorXd& b) const;
    /// Several right-hand sides at once, a column each: the factors read once
    /// for them all.
    Eigen::MatrixXd solveMany(const Eigen::MatrixXd& b) const;
    /// Accelerate's: its factors and workspace as its analysis says (bytes;
    /// 0 Eigen's). After analyzePattern() - MUMPS's and STRUMPACK's after
    /// a factorization.
    double factorBytes() const;
    /// "Accelerate's Cholesky", "MUMPS's Cholesky", "STRUMPACK's LU" or
    /// "Eigen's LDLT".
    QString name() const;
    /// Which, as analysed last.
    Backend backend() const;

private:
    struct Impl;
    std::unique_ptr<Impl> a_impl;
};

/*!
 * A sparse LDLᵀ of a complex symmetric matrix (Aᵀ = A, not Hermitian - a
 * time-harmonic curl-curl system's, its PMLs' too): Accelerate's (macOS 26
 * and later; threshold partial pivoting, METIS's ordering) where
 * acceleratedLu() - a 3D wave solve of 97 000 unknowns (PMLs round a
 * wire) factorized in 1.4 s, its process 1.9 GB, against Accelerate's LU's
 * 66 s and 9.1 GB, the S-parameters the same to every digit printed; MUMPS's
 * LDLT (threshold pivoting, METIS's ordering) where luBackend() is MUMPS,
 * from its size up; else a SparseLu (METIS's ordering where Accelerate's).
 * The matrix given whole (its lower triangle read).
 */
class SparseComplexLdlt
{
public:
    using Matrix = Eigen::SparseMatrix<std::complex<double>>;
    using Vector = Eigen::VectorXcd;
    using DenseMatrix = Eigen::MatrixXcd;
    SparseComplexLdlt();
    ~SparseComplexLdlt();
    SparseComplexLdlt(const SparseComplexLdlt&) = delete;
    SparseComplexLdlt& operator=(const SparseComplexLdlt&) = delete;

    void compute(const Matrix& a);
    Eigen::ComputationInfo info() const;
    std::string lastErrorMessage() const;
    Vector solve(const Vector& b) const;
    DenseMatrix solveMany(const DenseMatrix& b) const;
    /// "Accelerate's LDLT", "MUMPS's LDLT", or the LU's ("Eigen's LU",
    /// "Accelerate's LU", "STRUMPACK's LU").
    QString name() const;
    /// Accelerate's and MUMPS's: its factors and workspace as its analysis
    /// says (bytes; 0 another's) - and past \a limit (0: none), not
    /// factorized: info() a failure, lastErrorMessage() what it would take.
    /// Before compute().
    void setMemoryLimit(double bytes);
    double factorBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> a_impl;
};

extern template class SparseLu<double>;
extern template class SparseLu<std::complex<double>>;

} // namespace qucs_s::fem

#endif
