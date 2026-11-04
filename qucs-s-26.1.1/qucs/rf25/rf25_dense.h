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
 * one's speed and three times the matrix's memory at its peak. Elsewhere,
 * before 13.3, and asked so, Eigen's. Either way the matrix is factorized
 * where it is, not copied.
 */
#ifndef QUCS_RF25_DENSE_H
#define QUCS_RF25_DENSE_H

#include <Eigen/Dense>

#include <complex>
#include <memory>

namespace qucs_s::rf25 {

/// Which library factorizes the two systems.
enum class DenseLu { Automatic, Eigen, Accelerate };
/// For the whole process (a test's, both ways). QUCS_RF25_LU (auto, eigen
/// or accelerate) overrides it, and so does QUCS_FEM_SOLVER=eigen - Eigen
/// everywhere, as on Linux and Windows.
void setDenseLu(DenseLu choice);
/// What is in effect, read as each factorization begins.
DenseLu denseLu();
/// This build and this macOS have Accelerate's LAPACK: Automatic is it.
bool accelerateLuHere();
/// How many factorizations each has done, for tests.
long long accelerateLuCount();
long long eigenLuCount();

/*!
 * An LU with partial pivoting of a square matrix: the matrix taken, and
 * factorized where it is. Not copied or moved once made.
 */
template <class Scalar> class DenseFactors
{
public:
    using Matrix = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;

    explicit DenseFactors(Matrix&& a);
    ~DenseFactors();
    DenseFactors(const DenseFactors&) = delete;
    DenseFactors& operator=(const DenseFactors&) = delete;

    /// A^-1 b, for each of b's columns.
    Matrix solve(const Matrix& b) const;
    /// The reciprocal of A's condition in the 1-norm, estimated - LAPACK's
    /// gecon, or Eigen's rcond(): their estimates differ by some per cent.
    double rcond() const;
    /// Accelerate factorized it.
    bool accelerated() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

extern template class DenseFactors<double>;
extern template class DenseFactors<std::complex<double>>;

} // namespace qucs_s::rf25

#endif
