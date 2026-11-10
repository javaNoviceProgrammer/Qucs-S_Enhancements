/*
 * fem_sparse_external.h - the sparse direct solvers fem_sparse may take
 *                         from outside: MUMPS's and STRUMPACK's, where the
 *                         build found them (fem_sparse.cpp's own use)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_SPARSE_EXTERNAL_H
#define QUCS_FEM_SPARSE_EXTERNAL_H

#include <memory>
#include <string>
#include <vector>

namespace qucs_s::fem::external {

/// What a factorization is of: any square matrix (an LU), a symmetric
/// positive definite one (MUMPS's LDLᵀ without pivoting), a symmetric one -
/// real or complex symmetric, not Hermitian - (MUMPS's LDLᵀ with pivoting).
enum class Kind { General, PositiveDefinite, Symmetric };

/*!
 * One factorization of a library's: its pattern analysed (P, compressed
 * columns - a symmetric kind's lower triangle alone, its diagonal in it),
 * its values in P's order factorized as often as they change, solved. The
 * libraries are called one call at a time in the process (each a lock of
 * its own): a library built without OpenMP is not reentrant (gfortran's
 * static locals), and one call on all the cores is what a sweep's points
 * solved at once get from Accelerate too.
 */
template <typename Scalar>
class Factorization
{
public:
    virtual ~Factorization() = default;
    /// P analysed: \a n unknowns, \a starts (n + 1) and \a rows of each
    /// column (sorted), and \a values (one for each of P's entries: an
    /// analysis may scale and permute by them - MUMPS's maximum transversal,
    /// STRUMPACK's matching). False, error() why.
    virtual bool analyse(int n, const std::vector<long>& starts, const std::vector<int>& rows, const std::vector<Scalar>& values) = 0;
    /// \a values in P's order factorized - the next after analyse() those it
    /// was given, or others. False, error() why: a matrix singular, memory
    /// short.
    virtual bool factorize(const std::vector<Scalar>& values) = 0;
    /// \a b (n by \a columns, column-major) overwritten by the solutions.
    virtual bool solve(Scalar* b, int columns) const = 0;
    virtual std::string error() const = 0;
    /// Its factors' bytes - after analyse() where the library says before
    /// (MUMPS), else after factorize() (STRUMPACK); 0 not known.
    virtual double bytes() const = 0;
    /// A symmetric kind's negative pivots, after factorize() (-1: not known).
    virtual int negativePivots() const { return -1; }
    /// The last factorize() failed for a pivot of nought: the matrix
    /// singular to rounding.
    virtual bool singular() const = 0;
};

/// MUMPS's (sequential: its MPI stubs), where the build has it; else null.
template <typename Scalar> std::unique_ptr<Factorization<Scalar>> makeMumps(Kind kind);
/// STRUMPACK's (its LU: Kind::General alone), where the build has it; else
/// null.
template <typename Scalar> std::unique_ptr<Factorization<Scalar>> makeStrumpack();
bool mumpsInBuild();
bool strumpackInBuild();
/// "5.9.1" and the like; empty where not in the build.
std::string mumpsVersion();
std::string strumpackVersion();

} // namespace qucs_s::fem::external

#endif
