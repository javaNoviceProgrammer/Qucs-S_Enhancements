/*
 * fem_sparse_external.cpp - MUMPS's and STRUMPACK's factorizations behind
 *                           fem_sparse (QUCS_MUMPS, QUCS_STRUMPACK: the
 *                           build found them; docs/feature_gaps/
 *                           2026-11-09-linux-parallel-solvers.md)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_sparse_external.h"

#include <QByteArray>

#include <algorithm>
#include <complex>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <type_traits>

#if defined(QUCS_MUMPS)
#include <dmumps_c.h>
#include <zmumps_c.h>
#endif
#if defined(QUCS_STRUMPACK)
#include <StrumpackConfig.h>
#include <StrumpackSparseSolver.h>
#if defined(_OPENMP)
#include <omp.h>   // (its target's OpenMP flags this file's)
#endif
#endif

namespace qucs_s::fem::external {

namespace {

/// QUCS_FEM_PIVOT (0 to 0.5), the pivot threshold the user asked for;
/// negative: none asked (each library's own default).
[[maybe_unused]] double pivotAsked()
{
    bool ok = false;
    const double v = qgetenv("QUCS_FEM_PIVOT").toDouble(&ok);
    return ok && v >= 0 && v <= 0.5 ? v : -1.0;
}

/// QUCS_FEM_THREADS: the threads one call into a library may take; 0 where
/// not set (the library's own: OpenMP's, all the cores).
[[maybe_unused]] int threadsAsked()
{
    bool ok = false;
    const int v = qgetenv("QUCS_FEM_THREADS").toInt(&ok);
    return ok && v > 0 ? v : 0;
}

} // namespace

// ------------------------------------------------------------------- MUMPS

#if defined(QUCS_MUMPS)
namespace {

// One call into MUMPS at a time, in the whole process (Factorization's
// comment: the Debian and Ubuntu packages are built without OpenMP, and
// gfortran keeps a routine's large local arrays static there).
std::mutex mumpsLock;

template <typename Scalar> struct MumpsApi;
template <> struct MumpsApi<double> {
    using Struc = DMUMPS_STRUC_C;
    using Value = DMUMPS_COMPLEX;   // (MUMPS's name for d's scalar: double)
    static void call(Struc* id) { dmumps_c(id); }
};
template <> struct MumpsApi<std::complex<double>> {
    using Struc = ZMUMPS_STRUC_C;
    using Value = ZMUMPS_COMPLEX;   // {double r, i}: std::complex's layout
    static void call(Struc* id) { zmumps_c(id); }
};

/// INFOG(1)'s meaning, as MUMPS's guide gives it (those a user may meet).
std::string mumpsErrorText(int info1, int info2)
{
    const char* what = nullptr;
    switch (info1) {
    case -5: case -13: what = "out of memory"; break;
    case -6: what = "the matrix is structurally singular"; break;
    case -7: what = "out of memory (an integer array)"; break;
    case -8: case -9: case -14: case -15: case -17: case -20: what = "its working memory too small"; break;
    case -10: what = "the matrix is numerically singular"; break;
    case -11: case -12: what = "its working memory too small for the solve"; break;
    case -16: what = "the matrix's order is out of range"; break;
    case -19: what = "past the memory it may take"; break;
    case -40: what = "the matrix is not positive definite"; break;
    default: break;
    }
    char text[200];
    if (what) std::snprintf(text, sizeof text, "%s (MUMPS: INFOG(1) = %d, INFOG(2) = %d)", what, info1, info2);
    else std::snprintf(text, sizeof text, "MUMPS failed: INFOG(1) = %d, INFOG(2) = %d", info1, info2);
    return text;
}

/// Its working memory short: more of it asked and the factorization tried
/// again.
bool mumpsShortOfWorkspace(int info1)
{
    return info1 == -8 || info1 == -9 || info1 == -11 || info1 == -12 || info1 == -14 || info1 == -15 || info1 == -17 || info1 == -20;
}

template <typename Scalar>
class MumpsFactorization final : public Factorization<Scalar>
{
    using Api = MumpsApi<Scalar>;

public:
    explicit MumpsFactorization(Kind kind)
    {
        a_id = std::make_unique<typename Api::Struc>();
        std::memset(a_id.get(), 0, sizeof(typename Api::Struc));
        a_id->comm_fortran = -987654;   // (MPI_COMM_WORLD's stand-in in its stubs)
        a_id->par = 1;
        a_id->sym = kind == Kind::General ? 0 : kind == Kind::PositiveDefinite ? 1 : 2;
        a_id->job = -1;
        {
            std::lock_guard<std::mutex> one(mumpsLock);
            Api::call(a_id.get());
        }
        a_live = a_id->infog[0] >= 0;
        if (!a_live) {
            a_error = mumpsErrorText(a_id->infog[0], a_id->infog[1]);
            return;
        }
        // Silent (ICNTL(1) to (4)); METIS's ordering (ICNTL(7) = 5 - its
        // automatic choice where the library has no METIS); a quarter more
        // working memory than its estimate (ICNTL(14), 20 by default).
        a_id->icntl[0] = -1;
        a_id->icntl[1] = -1;
        a_id->icntl[2] = -1;
        a_id->icntl[3] = 0;
        a_id->icntl[6] = 5;
        a_id->icntl[13] = 25;
        const double pivot = pivotAsked();
        if (pivot >= 0 && kind != Kind::PositiveDefinite) a_id->cntl[0] = pivot;
        // (ICNTL(16): its OpenMP threads, where asked - OpenMP's own else.)
        a_id->icntl[15] = threadsAsked();
        // (ICNTL(48): its threads on the elimination tree's subtrees too,
        // scheduled as they come free - off: its sums then come out in one
        // order, a solve the same to the last bit whatever else runs beside
        // it (test_fem3_eme's MMI on 1 and 8 threads differed with it on, in
        // the last digits), at 15 to 50 % of its speed - a 3D Laplacian's
        // Cholesky of 64,000 unknowns 0.62 s against 0.48 on 8 threads.
        // QUCS_FEM_MUMPS_TREE=1 the quicker.)
        a_id->icntl[47] = qgetenv("QUCS_FEM_MUMPS_TREE").trimmed() == "1" ? 1 : 0;
    }
    ~MumpsFactorization() override
    {
        if (!a_live) return;
        a_id->job = -2;
        std::lock_guard<std::mutex> one(mumpsLock);
        Api::call(a_id.get());
    }

    bool analyse(int n, const std::vector<long>& starts, const std::vector<int>& rows, const std::vector<Scalar>& values) override
    {
        if (!a_live) return false;
        a_analysed = false;
        a_n = n;
        a_irn.resize(rows.size());
        a_jcn.resize(rows.size());
        for (int j = 0; j < n; ++j)
            for (long k = starts[std::size_t(j)]; k < starts[std::size_t(j) + 1]; ++k) {
                a_irn[std::size_t(k)] = rows[std::size_t(k)] + 1;
                a_jcn[std::size_t(k)] = j + 1;
            }
        a_id->n = n;
        a_id->nnz = static_cast<MUMPS_INT8>(rows.size());
        a_id->irn = a_irn.data();
        a_id->jcn = a_jcn.data();
        a_id->a = const_cast<typename Api::Value*>(reinterpret_cast<const typename Api::Value*>(values.data()));
        a_id->job = 1;
        {
            std::lock_guard<std::mutex> one(mumpsLock);
            Api::call(a_id.get());
        }
        if (a_id->infog[0] < 0) {
            a_error = mumpsErrorText(a_id->infog[0], a_id->infog[1]);
            return false;
        }
        a_analysed = true;
        return true;
    }

    bool factorize(const std::vector<Scalar>& values) override
    {
        if (!a_analysed) return false;
        a_factored = false;
        a_id->a = const_cast<typename Api::Value*>(reinterpret_cast<const typename Api::Value*>(values.data()));
        for (int attempt = 0;; ++attempt) {
            a_id->job = 2;
            {
                std::lock_guard<std::mutex> one(mumpsLock);
                Api::call(a_id.get());
            }
            const int info1 = a_id->infog[0];
            if (info1 >= 0) break;
            if (mumpsShortOfWorkspace(info1) && attempt < 4) {
                a_id->icntl[13] = std::max(2 * a_id->icntl[13], 50);
                continue;
            }
            a_error = mumpsErrorText(info1, a_id->infog[1]);
            a_singular = info1 == -10;
            return false;
        }
        a_factored = true;
        a_singular = false;
        return true;
    }

    bool solve(Scalar* b, int columns) const override
    {
        if (!a_factored || columns <= 0) return a_factored;
        a_id->rhs = reinterpret_cast<typename Api::Value*>(b);
        a_id->nrhs = columns;
        a_id->lrhs = a_n;
        a_id->icntl[19] = 0;   // (dense right-hand sides, ICNTL(20))
        a_id->icntl[20] = 0;   // (the solution centralized, in rhs: ICNTL(21))
        a_id->job = 3;
        {
            std::lock_guard<std::mutex> one(mumpsLock);
            Api::call(a_id.get());
        }
        a_id->rhs = nullptr;
        return a_id->infog[0] >= 0;
    }

    std::string error() const override { return a_error; }
    // (INFOG(17): its estimate, after the analysis, of all it takes to
    // factorize in core, in millions of bytes.)
    double bytes() const override { return a_analysed ? 1e6 * double(a_id->infog[16]) : 0.0; }
    int negativePivots() const override { return a_factored && a_id->sym != 0 ? a_id->infog[11] : -1; }
    bool singular() const override { return a_singular; }

private:
    std::unique_ptr<typename Api::Struc> a_id;
    std::vector<MUMPS_INT> a_irn, a_jcn;
    int a_n = 0;
    bool a_live = false, a_analysed = false, a_factored = false, a_singular = false;
    std::string a_error;
};

} // namespace

template <typename Scalar> std::unique_ptr<Factorization<Scalar>> makeMumps(Kind kind)
{
    return std::make_unique<MumpsFactorization<Scalar>>(kind);
}
bool mumpsInBuild()
{
    return true;
}
std::string mumpsVersion()
{
    return MUMPS_VERSION;
}
#else
template <typename Scalar> std::unique_ptr<Factorization<Scalar>> makeMumps(Kind)
{
    return nullptr;
}
bool mumpsInBuild()
{
    return false;
}
std::string mumpsVersion()
{
    return std::string();
}
#endif

// --------------------------------------------------------------- STRUMPACK

#if defined(QUCS_STRUMPACK)
namespace {

// One call into STRUMPACK at a time, in the whole process: each call
// spreads over all the cores by OpenMP's tasks; calls on several threads at
// once would each take them all.
std::mutex strumpackLock;

/// The threads STRUMPACK's calls take - set on the calling thread before
/// each (0: OpenMP's own). QUCS_FEM_THREADS where set. On macOS 8 at most:
/// its fronts' memory pool is an OpenMP critical section, and macOS's libomp
/// spun on it - a 2D grid's LU of 20,000 unknowns 0.09 s on 8 threads, 34 s
/// on an M2 Ultra's 24 (16 already several times 8's: 9 November).
int strumpackThreads()
{
    if (const int asked = threadsAsked()) return asked;
#if defined(__APPLE__) && defined(_OPENMP)
    return std::min(8, omp_get_num_procs());
#else
    return 0;
#endif
}

/// Its threads set for this thread's calls (under the lock).
void useStrumpackThreads()
{
#if defined(_OPENMP)
    if (const int threads = strumpackThreads()) omp_set_num_threads(threads);
    omp_set_max_active_levels(1);
#endif
}

std::string strumpackErrorText(STRUMPACK_RETURN_CODE code)
{
    switch (code) {
    case STRUMPACK_SUCCESS: return std::string();
    case STRUMPACK_MATRIX_NOT_SET: return "STRUMPACK: no matrix given";
    case STRUMPACK_REORDERING_ERROR: return "STRUMPACK: its reordering failed";
    case STRUMPACK_ZERO_PIVOT: return "the matrix is singular (STRUMPACK: a pivot of nought)";
    case STRUMPACK_NO_CONVERGENCE: return "STRUMPACK: its refinement did not converge";
    default: return "STRUMPACK failed (code " + std::to_string(int(code)) + ")";
    }
}

template <typename Scalar>
class StrumpackFactorization final : public Factorization<Scalar>
{
public:
    StrumpackFactorization()
    {
        std::lock_guard<std::mutex> one(strumpackLock);
        STRUMPACK_init_mt(&a_solver, std::is_same_v<Scalar, double> ? STRUMPACK_DOUBLE : STRUMPACK_DOUBLECOMPLEX, STRUMPACK_MT, 0, nullptr, 0);
        // METIS's nested dissection; no compression (a direct solve to
        // rounding, as the others'); its own default otherwise - the
        // matching that scales and permutes by the values (MC64's product),
        // and iterative refinement where it pays.
        STRUMPACK_set_reordering_method(a_solver, STRUMPACK_METIS);
        STRUMPACK_set_compression(a_solver, STRUMPACK_NONE);
    }
    ~StrumpackFactorization() override
    {
        std::lock_guard<std::mutex> one(strumpackLock);
        STRUMPACK_destroy(&a_solver);
    }

    bool analyse(int n, const std::vector<long>& starts, const std::vector<int>& rows, const std::vector<Scalar>& values) override
    {
        a_analysed = a_factored = false;
        a_n = n;
        // (Its matrix by rows, P's by columns: P's entries placed in the
        // rows' order, each one's place kept for the values.)
        a_rowStarts.assign(std::size_t(n) + 1, 0);
        for (int r : rows) ++a_rowStarts[std::size_t(r) + 1];
        for (int i = 0; i < n; ++i) a_rowStarts[std::size_t(i) + 1] += a_rowStarts[std::size_t(i)];
        std::vector<int> next(a_rowStarts.begin(), a_rowStarts.end() - 1);
        a_columns.resize(rows.size());
        a_place.resize(rows.size());
        for (int j = 0; j < n; ++j)
            for (long k = starts[std::size_t(j)]; k < starts[std::size_t(j) + 1]; ++k) {
                const int at = next[std::size_t(rows[std::size_t(k)])]++;
                a_columns[std::size_t(at)] = j;
                a_place[std::size_t(k)] = at;
            }
        place(values);
        std::lock_guard<std::mutex> one(strumpackLock);
        useStrumpackThreads();
        STRUMPACK_set_csr_matrix(a_solver, &a_n, a_rowStarts.data(), a_columns.data(), a_values.data(), 0);
        const STRUMPACK_RETURN_CODE code = STRUMPACK_reorder(a_solver);
        if (code != STRUMPACK_SUCCESS) {
            a_error = strumpackErrorText(code);
            return false;
        }
        a_analysed = true;
        a_fresh = true;
        return true;
    }

    bool factorize(const std::vector<Scalar>& values) override
    {
        if (!a_analysed) return false;
        a_factored = false;
        std::lock_guard<std::mutex> one(strumpackLock);
        useStrumpackThreads();
        if (!a_fresh) {
            place(values);
            STRUMPACK_update_csr_matrix_values(a_solver, &a_n, a_rowStarts.data(), a_columns.data(), a_values.data(), 0);
        }
        a_fresh = false;
        const STRUMPACK_RETURN_CODE code = STRUMPACK_factor(a_solver);
        a_singular = code == STRUMPACK_ZERO_PIVOT;
        if (code != STRUMPACK_SUCCESS) {
            a_error = strumpackErrorText(code);
            return false;
        }
        a_factored = true;
        return true;
    }

    bool solve(Scalar* b, int columns) const override
    {
        if (!a_factored || columns <= 0) return a_factored;
        std::vector<Scalar> x(std::size_t(a_n) * std::size_t(columns));
        std::lock_guard<std::mutex> one(strumpackLock);
        useStrumpackThreads();
        const STRUMPACK_RETURN_CODE code = columns == 1 ? STRUMPACK_solve(a_solver, b, x.data(), 0)
                                                        : STRUMPACK_matsolve(a_solver, columns, b, a_n, x.data(), a_n, 0);
        std::copy(x.begin(), x.end(), b);
        return code == STRUMPACK_SUCCESS;
    }

    std::string error() const override { return a_error; }
    bool singular() const override { return a_singular; }
    double bytes() const override
    {
        if (!a_factored) return 0.0;
        std::lock_guard<std::mutex> one(strumpackLock);
        return double(STRUMPACK_factor_memory(a_solver));
    }

private:
    void place(const std::vector<Scalar>& values)
    {
        a_values.resize(values.size());
        for (std::size_t k = 0; k < values.size(); ++k) a_values[std::size_t(a_place[k])] = values[k];
    }
    // (Its C interface's calls take the handle by value; mutable: a solve
    // is const to fem_sparse.)
    mutable STRUMPACK_SparseSolver a_solver{};
    int a_n = 0;
    std::vector<int> a_rowStarts, a_columns, a_place;
    std::vector<Scalar> a_values;
    bool a_analysed = false, a_factored = false, a_fresh = false, a_singular = false;
    std::string a_error;
};

} // namespace

template <typename Scalar> std::unique_ptr<Factorization<Scalar>> makeStrumpack()
{
    return std::make_unique<StrumpackFactorization<Scalar>>();
}
bool strumpackInBuild()
{
    return true;
}
std::string strumpackVersion()
{
    return std::to_string(STRUMPACK_VERSION_MAJOR) + "." + std::to_string(STRUMPACK_VERSION_MINOR) + "." + std::to_string(STRUMPACK_VERSION_PATCH);
}
#else
template <typename Scalar> std::unique_ptr<Factorization<Scalar>> makeStrumpack()
{
    return nullptr;
}
bool strumpackInBuild()
{
    return false;
}
std::string strumpackVersion()
{
    return std::string();
}
#endif

template std::unique_ptr<Factorization<double>> makeMumps<double>(Kind);
template std::unique_ptr<Factorization<std::complex<double>>> makeMumps<std::complex<double>>(Kind);
template std::unique_ptr<Factorization<double>> makeStrumpack<double>();
template std::unique_ptr<Factorization<std::complex<double>>> makeStrumpack<std::complex<double>>();

} // namespace qucs_s::fem::external
