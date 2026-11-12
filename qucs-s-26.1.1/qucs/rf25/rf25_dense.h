/*
 * rf25_dense.h - the dense LU of RF 2.5D's two large systems: the full
 *                wave's runs of current and the quasi-static extraction's
 *                charges
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Each is a dense matrix of some thousands of rows - the full wave's
 * complex (rf25_fullwave.cpp's solveAt), the extraction's real (its
 * potential coefficients, rf25_static.cpp) - factorized with partial
 * pivoting and solved for a handful of right-hand sides: the ports, the
 * nets.
 *
 * On macOS 13.3 and later Accelerate's LAPACK factorizes them, its new
 * interface (ACCELERATE_NEW_LAPACK) - measured on an M2 Ultra 9 to 11 times
 * Eigen's PartialPivLU on a complex matrix of 4000 to 6000 rows and 15 to 17
 * times on a real one of 6000 to 8000, to the same accuracy
 * (docs/feature_gaps/2026-11-04-rf25-dense-lu.md). Its classic interface,
 * which macOS 13.0 has, is not used: for a complex matrix half the new
 * one's speed and three times the matrix's memory at its peak.
 *
 * Where Accelerate's is not - Linux, Windows - OpenBLAS's LAPACK, where the
 * build found it (rf25_openblas.h): a factorization that has the machine,
 * or a good part of it, on all of OpenBLAS's threads, such calls one at a
 * time; one of many side by side - a sweep's frequencies at once - on one
 * thread, side by side, where OpenBLAS can be made to keep to one, else by
 * Eigen's (docs/feature_gaps/2026-11-09-linux-parallel-solvers.md).
 * Elsewhere, and asked so, Eigen's (one thread). Either way the matrix is
 * factorized where it is, not copied.
 */
#ifndef QUCS_RF25_DENSE_H
#define QUCS_RF25_DENSE_H

#include <Eigen/Dense>

#include <QString>

#include <complex>
#include <memory>

namespace qucs_s::rf25 {

/// Which library factorizes the two systems.
enum class DenseLu { Automatic, Eigen, Accelerate, OpenBlas };
/// For the whole process (a test's, each way). QUCS_RF25_LU (auto, eigen,
/// accelerate or openblas) overrides it, and so does QUCS_FEM_SOLVER=eigen -
/// Eigen everywhere.
void setDenseLu(DenseLu choice);
/// What is asked, read as each factorization begins.
DenseLu denseLu();
/// This build and this macOS have Accelerate's LAPACK: Automatic is it.
bool accelerateLuHere();
/// This build has OpenBLAS's LAPACK: Automatic is it where Accelerate's is
/// not.
bool openBlasLuHere();
/// OpenBLAS's version, threading and threads ("OpenBLAS 0.3.26, OpenMP, 16
/// threads"); empty where it is not here.
QString openBlasLuAbout();
/// The library a factorization now would be: Eigen, Accelerate or
/// OpenBlas - the one asked where it is here, else Eigen; Automatic
/// Accelerate's, else OpenBLAS's, else Eigen's.
DenseLu denseLuInEffect();
/// \a lu's name as the solve's log and the tools say it: "Accelerate",
/// "OpenBLAS", "Eigen".
QString denseLuName(DenseLu lu);
/// A factorization now would be Accelerate's, OpenBLAS's (on the machine's
/// threads: one with a few of them may be Eigen's, DenseFactors).
bool luOnAccelerate();
bool luOnOpenBlas();
/// OpenBLAS's factorizations can run side by side on one thread each (its
/// OpenMP build, this build's OpenMP its runtime): where they cannot, one
/// with fewer than four threads is Eigen's.
bool openBlasSideBySide();
/// How many factorizations each has done, for tests.
long long accelerateLuCount();
long long openBlasLuCount();
long long eigenLuCount();

/*!
 * An LU with partial pivoting of a square matrix: the matrix taken, and
 * factorized where it is - by the library in effect, on as many of the
 * machine's threads as \a threads says it has (0: all of them; a sweep's
 * frequency its share), which only OpenBLAS's heeds (rf25_openblas.h). Not
 * copied or moved once made.
 */
template <class Scalar> class DenseFactors
{
public:
    using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;

    explicit DenseFactors(Matrix&& a, int threads = 0);
    ~DenseFactors();
    DenseFactors(const DenseFactors&) = delete;
    DenseFactors& operator=(const DenseFactors&) = delete;

    /// A^-1 b, for each of b's columns.
    Matrix solve(const Matrix& b) const;
    /// The reciprocal of A's condition in the 1-norm, estimated - LAPACK's
    /// gecon, or Eigen's rcond(): their estimates differ by some per cent.
    double rcond() const;
    /// Accelerate factorized it; OpenBLAS did.
    bool accelerated() const;
    bool openBlas() const;
    /// The library that factorized it: Eigen, Accelerate or OpenBlas.
    DenseLu library() const;
    /// The seconds it waited, factorized and solved, for the turn of
    /// another's call on OpenBLAS's thread (rf25_openblas.h): nought
    /// elsewhere.
    double waited() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

extern template class DenseFactors<double>;
extern template class DenseFactors<std::complex<double>>;

} // namespace qucs_s::rf25

#endif
