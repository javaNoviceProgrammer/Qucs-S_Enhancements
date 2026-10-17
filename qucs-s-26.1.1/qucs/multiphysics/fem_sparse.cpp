/*
 * fem_sparse.cpp - the multiphysics solver's sparse direct factorizations
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_sparse.h"

#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>

#include <QByteArray>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <list>
#include <mutex>
#include <type_traits>
#include <vector>

#if defined(QUCS_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

namespace qucs_s::fem {

namespace {

/// QUCS_FEM_SOLVER=eigen: Eigen's, where Accelerate's would be taken (read
/// as each factorization is analysed).
bool eigenAsked()
{
    return qgetenv("QUCS_FEM_SOLVER").trimmed().toLower() == "eigen";
}

#if defined(QUCS_ACCELERATE)
// Accelerate's parameter errors: told to the caller (without a callback it
// stops the program, __builtin_trap()).
thread_local std::string accelerateError;
void reportAccelerate(const char* message)
{
    accelerateError = message ? message : "";
}

SparseSymbolicFactorOptions symbolicOptions()
{
    SparseSymbolicFactorOptions o = _SparseDefaultSymbolicFactorOptions;
    o.reportError = reportAccelerate;
    return o;
}

/// Its pivot tolerance (threshold partial pivoting, 0 to 0.5): 0.1, an
/// order above Accelerate's default for "difficult matrices" - the
/// semiconductors' Jacobians span 30 decades; QUCS_FEM_PIVOT another.
double pivotTolerance()
{
    static const double tolerance = [] {
        bool ok = false;
        const double v = qgetenv("QUCS_FEM_PIVOT").toDouble(&ok);
        return ok && v >= 0 && v <= 0.5 ? v : 0.1;
    }();
    return tolerance;
}

std::string statusText(SparseStatus_t s)
{
    switch (s) {
    case SparseStatusOK: return "";
    case SparseFactorizationFailed: return "the factorization failed";
    case SparseMatrixIsSingular: return "the matrix is singular";
    case SparseInternalError: return "an internal error of the sparse solver";
    case SparseParameterError: return "a parameter error: " + accelerateError;
    default: return "the factorization failed (status " + std::to_string(int(s)) + ")";
    }
}
#endif

#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
/// LU analyses kept for the next matrix of the same pattern - a sweep's next
/// point, a problem made again: the symbolic factorization of a
/// 681 054-unknown system took 1.9 s of its 4.7 s solve. The last four, by
/// pattern (checked whole, not by its hash alone); a symbolic factorization
/// is reference counted - the cache and each user its own retain.
struct Analysed {
    bool complex = false;
    std::uint64_t hash = 0;
    std::vector<long> starts;
    std::vector<int> rows;
    SparseOpaqueSymbolicFactorization symbolic{};
};
std::mutex analysesLock;
std::atomic<int> analysesMade{0};
// (Never destroyed: Accelerate's objects are not released at the program's
// end, after its own state may be gone.)
std::list<Analysed>& analyses()
{
    static auto* kept = new std::list<Analysed>();
    return *kept;
}

std::uint64_t patternHash(const std::vector<long>& starts, const std::vector<int>& rows)
{
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, std::size_t bytes) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(starts.data(), starts.size() * sizeof(long));
    mix(rows.data(), rows.size() * sizeof(int));
    return h;
}

/// This pattern's analysis kept: retained into \a out.
bool findAnalysed(bool complex, std::uint64_t hash, const std::vector<long>& starts, const std::vector<int>& rows, SparseOpaqueSymbolicFactorization* out)
{
    std::lock_guard<std::mutex> lock(analysesLock);
    auto& list = analyses();
    for (auto it = list.begin(); it != list.end(); ++it)
        if (it->complex == complex && it->hash == hash && it->starts == starts && it->rows == rows) {
            *out = SparseRetain(it->symbolic);
            list.splice(list.begin(), list, it);
            return true;
        }
    return false;
}

void keepAnalysed(bool complex, std::uint64_t hash, const std::vector<long>& starts, const std::vector<int>& rows, SparseOpaqueSymbolicFactorization symbolic)
{
    std::lock_guard<std::mutex> lock(analysesLock);
    auto& list = analyses();
    list.push_front(Analysed{complex, hash, starts, rows, SparseRetain(symbolic)});
    while (list.size() > 4) {
        SparseCleanup(list.back().symbolic);
        list.pop_back();
    }
}
#endif

} // namespace

int acceleratedFrom()
{
    bool ok = false;
    const int from = qgetenv("QUCS_FEM_ACCELERATE_FROM").toInt(&ok);
    return ok && from >= 0 ? from : 20000;
}

bool acceleratedLu()
{
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
    if (eigenAsked()) return false;
    if (__builtin_available(macOS 15.5, *)) return true;
#endif
    return false;
}

bool acceleratedCholesky()
{
#if defined(QUCS_ACCELERATE)
    return !eigenAsked();
#else
    return false;
#endif
}

int acceleratedAnalysesMade()
{
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
    return analysesMade;
#else
    return 0;
#endif
}

QString directSolverName()
{
    return acceleratedLu() ? QStringLiteral("Accelerate") : QStringLiteral("Eigen");
}

// ---------------------------------------------------------------------- LU

#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
namespace {
template <typename Scalar> struct AccelerateTypes;
template <> struct AccelerateTypes<double> {
    using Matrix = SparseMatrix_Double;
    using Factorization = SparseOpaqueFactorization_Double;
    using Dense = DenseVector_Double;
    static Matrix matrix(SparseMatrixStructure s, double* v) { return Matrix{s, v}; }
    static Dense dense(int n, double* v) { return Dense{n, v}; }
};
template <> struct AccelerateTypes<std::complex<double>> {
    using Matrix = SparseMatrix_Complex_Double;
    using Factorization = SparseOpaqueFactorization_Complex_Double;
    using Dense = DenseVector_Complex_Double;
    static Matrix matrix(SparseMatrixStructure s, std::complex<double>* v)
    {
        SparseAttributesComplex_t a{};
        a.kind = SparseOrdinary;
        return Matrix{SparseMatrixStructureComplex{s.rowCount, s.columnCount, s.columnStarts, s.rowIndices, a, s.blockSize},
                      reinterpret_cast<__SPARSE_double_complex*>(v)};
    }
    static Dense dense(int n, std::complex<double>* v) { return Dense{n, reinterpret_cast<__SPARSE_double_complex*>(v)}; }
};
} // namespace
#endif

template <typename Scalar>
struct SparseLu<Scalar>::Impl {
    bool accelerate = false;
    int analyses = 0;
    Eigen::ComputationInfo info = Eigen::InvalidInput;
    std::string error;
    std::unique_ptr<Eigen::SparseLU<Matrix>> eigen;
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
    using T = AccelerateTypes<Scalar>;
    // The pattern analysed (P: Accelerate's long column starts, int rows)
    // and the values in it. A matrix of another pattern within P is
    // factorized in P, its other entries nought; one with an entry outside
    // it, P becomes their union and is analysed again - a Newton's Jacobian
    // whose entries come and go (an avalanche's ionization where the field
    // is high) analysed 2264 times in 8500 factorizations, each analysis
    // eight factorizations' time.
    std::vector<long> starts;
    std::vector<int> rows;
    std::vector<Scalar> values;
    int n = 0;
    bool analysed = false, factored = false;
    SparseOpaqueSymbolicFactorization symbolic{};
    typename T::Factorization numeric{};
    SparseMatrixStructure structure() const
    {
        SparseAttributes_t a{};
        a.kind = SparseOrdinary;
        return SparseMatrixStructure{n, n, const_cast<long*>(starts.data()), const_cast<int*>(rows.data()), a, 1};
    }
    void release()
    {
        if (factored) SparseCleanup(numeric);
        if (analysed) SparseCleanup(symbolic);
        factored = analysed = false;
    }
    /// \a a's values put in P (false: an entry of it outside P).
    bool scatter(const Matrix& a)
    {
        if (a.rows() != n || a.cols() != n) return false;
        values.assign(rows.size(), Scalar(0));
        for (int j = 0; j < n; ++j) {
            long k = starts[std::size_t(j)];
            const long end = starts[std::size_t(j) + 1];
            for (typename Matrix::InnerIterator it(a, j); it; ++it) {
                const int i = int(it.row());
                while (k < end && rows[std::size_t(k)] < i) ++k;
                if (k == end || rows[std::size_t(k)] != i) return false;
                values[std::size_t(k)] += it.value();
            }
        }
        return true;
    }
    /// P with \a a's pattern added.
    void unite(const Matrix& a)
    {
        std::vector<long> s(std::size_t(n) + 1, 0);
        std::vector<int> r;
        r.reserve(rows.size() + std::size_t(a.nonZeros()));
        for (int j = 0; j < n; ++j) {
            long k = starts[std::size_t(j)];
            const long end = starts[std::size_t(j) + 1];
            typename Matrix::InnerIterator it(a, j);
            while (k < end || it) {
                const int p = k < end ? rows[std::size_t(k)] : std::numeric_limits<int>::max();
                const int q = it ? int(it.row()) : std::numeric_limits<int>::max();
                const int i = std::min(p, q);
                if (r.empty() || std::size_t(s[std::size_t(j)]) == r.size() || r.back() != i) r.push_back(i);
                if (p == i) ++k;
                if (q == i) ++it;
            }
            s[std::size_t(j) + 1] = long(r.size());
        }
        starts = std::move(s);
        rows = std::move(r);
    }
    /// The backward error of a solve with the factorization: |b - A x| /
    /// (|A| |x| + |b|), maximum norms, for a fixed right-hand side. Accelerate's
    /// LU says a singular matrix factorized - a pivot of nought replaced -,
    /// its solves finite and wrong (a row and column of noughts: a residual
    /// of 0.035); Eigen's says it singular, as the callers read it. Large
    /// here, it is said singular too.
    double backwardError() const
    {
        Vector b(n), x(n);
        for (int k = 0; k < n; ++k) b[k] = Scalar(0.5 + std::fmod(0.6180339887 * (k + 1), 1.0));
        Vector work = b;
        if (__builtin_available(macOS 15.5, *))
            SparseSolve(numeric, T::dense(n, work.data()), T::dense(n, x.data()));
        if (!x.allFinite()) return std::numeric_limits<double>::infinity();
        Vector r = -b;
        double normA = 0;
        std::vector<double> rowSum(std::size_t(n), 0.0);
        for (int j = 0; j < n; ++j)
            for (long k = starts[std::size_t(j)]; k < starts[std::size_t(j) + 1]; ++k) {
                const int i = rows[std::size_t(k)];
                r[i] += values[std::size_t(k)] * x[j];
                rowSum[std::size_t(i)] += std::abs(values[std::size_t(k)]);
            }
        for (double v : rowSum) normA = std::max(normA, v);
        const double denominator = normA * x.cwiseAbs().maxCoeff() + b.cwiseAbs().maxCoeff();
        return denominator > 0 ? r.cwiseAbs().maxCoeff() / denominator : 0.0;
    }
    /// P analysed (Accelerate's symbolic factorization).
    void analyse()
    {
        ++analyses;
        release();
        accelerateError.clear();
        info = Eigen::Success;
        if (n == 0) return;
        if (__builtin_available(macOS 15.5, *)) {
            const bool complex = !std::is_same_v<Scalar, double>;
            const std::uint64_t hash = patternHash(starts, rows);
            if (findAnalysed(complex, hash, starts, rows, &symbolic)) {
                analysed = true;
                return;
            }
            SparseMatrixStructure st = structure();
            ++analysesMade;
            if constexpr (std::is_same_v<Scalar, double>) {
                symbolic = SparseFactor(SparseFactorizationLU, st, symbolicOptions());
            } else {
                SparseAttributesComplex_t ac{};
                ac.kind = SparseOrdinary;
                symbolic = SparseFactor(SparseFactorizationLU, SparseMatrixStructureComplex{st.rowCount, st.columnCount, st.columnStarts, st.rowIndices, ac, 1},
                                        symbolicOptions());
            }
            analysed = symbolic.status == SparseStatusOK;
            if (!analysed) {
                error = statusText(symbolic.status);
                SparseCleanup(symbolic);
                info = Eigen::InvalidInput;
            } else {
                keepAnalysed(complex, hash, starts, rows, symbolic);
            }
        }
    }
#endif
    ~Impl()
    {
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
        if (accelerate) release();
#endif
    }
};

template <typename Scalar> SparseLu<Scalar>::SparseLu() : a_impl(std::make_unique<Impl>()) {}
template <typename Scalar> SparseLu<Scalar>::~SparseLu() = default;

template <typename Scalar> void SparseLu<Scalar>::analyzePattern(const Matrix& given)
{
    Impl& d = *a_impl;
    d.error.clear();
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
    if (d.accelerate) d.release();
#endif
    d.accelerate = acceleratedLu() && given.rows() == given.cols() && given.rows() >= acceleratedFrom();
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
    if (d.accelerate) {
        d.eigen.reset();
        d.n = int(given.rows());
        d.starts.assign(std::size_t(d.n) + 1, 0);
        d.rows.clear();
        d.unite(given);
        d.analyse();
        return;
    }
#endif
    d.eigen = std::make_unique<Eigen::SparseLU<Matrix>>();
    d.eigen->analyzePattern(given);
    ++d.analyses;
    // (Its info() only after a factorization - asserted in a Debug build;
    // its analysis does not fail.)
    d.info = Eigen::Success;
}

template <typename Scalar> void SparseLu<Scalar>::factorize(const Matrix& given)
{
    Impl& d = *a_impl;
    d.error.clear();
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
    if (d.accelerate) {
        if (__builtin_available(macOS 15.5, *)) {
            if (d.n == 0 && given.rows() == 0) {
                d.info = Eigen::Success;
                return;
            }
            if (given.rows() != d.n || given.cols() != d.n) {
                analyzePattern(given);
                if (d.info != Eigen::Success || !d.accelerate) {
                    if (!d.accelerate) factorize(given);
                    return;
                }
            }
            if (!d.analysed || !d.scatter(given)) {
                d.unite(given);
                d.analyse();
                if (d.info != Eigen::Success) return;
                (void)d.scatter(given);
            }
            auto m = Impl::T::matrix(d.structure(), d.values.data());
            SparseNumericFactorOptions o = std::is_same_v<Scalar, double> ? _SparseDefaultNumericFactorOptions_Double
                                                                          : _SparseDefaultNumericFactorOptions_Complex_Double;
            o.pivotTolerance = pivotTolerance();
            accelerateError.clear();
            if (d.factored) {
                SparseRefactor(m, &d.numeric, o);
            } else {
                d.numeric = SparseFactor(d.symbolic, m, o);
                d.factored = true;
            }
            const double eta = d.numeric.status == SparseStatusOK ? d.backwardError() : 0.0;
            if (d.numeric.status == SparseStatusOK && eta < 1e-6) {
                d.info = Eigen::Success;
            } else if (d.numeric.status == SparseStatusOK) {
                d.info = Eigen::NumericalIssue;
                d.error = "the matrix is singular (a solve with its factorization leaves a backward error of " + std::to_string(eta) + ")";
            } else {
                d.info = Eigen::NumericalIssue;
                d.error = statusText(d.numeric.status);
                // (A failed one is not refactored: made again next time.)
                SparseCleanup(d.numeric);
                d.factored = false;
            }
            return;
        }
    }
#endif
    if (!d.eigen) {
        d.eigen = std::make_unique<Eigen::SparseLU<Matrix>>();
        d.eigen->analyzePattern(given);
    }
    d.eigen->factorize(given);
    d.info = d.eigen->info();
    if (d.info != Eigen::Success) d.error = d.eigen->lastErrorMessage();
}

template <typename Scalar> Eigen::ComputationInfo SparseLu<Scalar>::info() const
{
    return a_impl->info;
}

template <typename Scalar> std::string SparseLu<Scalar>::lastErrorMessage() const
{
    return a_impl->error;
}

template <typename Scalar> bool SparseLu<Scalar>::accelerated() const
{
    return a_impl->accelerate;
}

template <typename Scalar> int SparseLu<Scalar>::analyses() const
{
    return a_impl->analyses;
}

template <typename Scalar> typename SparseLu<Scalar>::Vector SparseLu<Scalar>::solve(const Vector& b) const
{
    const Impl& d = *a_impl;
#if defined(QUCS_ACCELERATE) && defined(QUCS_ACCELERATE_LU)
    if (d.accelerate) {
        if (__builtin_available(macOS 15.5, *)) {
            Vector x(b.size());
            if (d.n == 0 || !d.factored || b.size() != d.n || d.info != Eigen::Success) {
                x.setConstant(Scalar(std::numeric_limits<double>::quiet_NaN()));
                return x;
            }
            Vector in = b;
            SparseSolve(d.numeric, Impl::T::dense(d.n, in.data()), Impl::T::dense(d.n, x.data()));
            return x;
        }
    }
#endif
    // (Not after a failed factorization: Eigen's solve would read nothing
    // there - NaNs, which the callers say are not finite.)
    if (!d.eigen || d.info != Eigen::Success) return Vector::Constant(b.size(), Scalar(std::numeric_limits<double>::quiet_NaN()));
    return Vector(d.eigen->solve(b));
}

template class SparseLu<double>;
template class SparseLu<std::complex<double>>;

// ---------------------------------------------------------------- Cholesky

struct SparseCholesky::Impl {
    bool accelerate = false;
    Eigen::ComputationInfo info = Eigen::InvalidInput;
    std::unique_ptr<Eigen::SimplicialLDLT<Matrix>> eigen;
#if defined(QUCS_ACCELERATE)
    // (Its lower triangle's pattern, kept: the matrix is given whole.)
    Matrix lower;
    std::vector<long> starts;
    int n = 0;
    bool analysed = false, factored = false;
    SparseOpaqueSymbolicFactorization symbolic{};
    SparseOpaqueFactorization_Double numeric{};
    SparseMatrixStructure structure()
    {
        SparseAttributes_t a{};
        a.kind = SparseSymmetric;
        a.triangle = SparseLowerTriangle;
        return SparseMatrixStructure{n, n, starts.data(), lower.innerIndexPtr(), a, 1};
    }
    void release()
    {
        if (factored) SparseCleanup(numeric);
        if (analysed) SparseCleanup(symbolic);
        factored = analysed = false;
    }
#endif
    ~Impl()
    {
#if defined(QUCS_ACCELERATE)
        if (accelerate) release();
#endif
    }
};

SparseCholesky::SparseCholesky() : a_impl(std::make_unique<Impl>()) {}
SparseCholesky::~SparseCholesky() = default;

void SparseCholesky::analyzePattern(const Matrix& a)
{
    Impl& d = *a_impl;
    d.accelerate = acceleratedCholesky() && a.rows() >= acceleratedFrom();
#if defined(QUCS_ACCELERATE)
    if (d.accelerate) {
        d.release();
        d.lower = a.triangularView<Eigen::Lower>();
        d.lower.makeCompressed();
        d.n = int(a.rows());
        d.starts.assign(d.lower.outerIndexPtr(), d.lower.outerIndexPtr() + d.lower.cols() + 1);
        accelerateError.clear();
        if (d.n > 0) {
            d.symbolic = SparseFactor(SparseFactorizationCholesky, d.structure(), symbolicOptions());
            d.analysed = d.symbolic.status == SparseStatusOK;
            if (!d.analysed) SparseCleanup(d.symbolic);
        }
        d.info = d.analysed || d.n == 0 ? Eigen::Success : Eigen::InvalidInput;
        return;
    }
#endif
    d.eigen = std::make_unique<Eigen::SimplicialLDLT<Matrix>>();
    d.eigen->analyzePattern(a);
    d.info = Eigen::Success;
}

void SparseCholesky::factorize(const Matrix& a)
{
    Impl& d = *a_impl;
#if defined(QUCS_ACCELERATE)
    if (d.accelerate) {
        if (d.n == 0) {
            d.info = Eigen::Success;
            return;
        }
        if (!d.analysed) {
            d.info = Eigen::InvalidInput;
            return;
        }
        // (Its lower triangle's values; its pattern another than the one
        // analysed: analysed again.)
        Matrix lower = a.triangularView<Eigen::Lower>();
        lower.makeCompressed();
        if (lower.nonZeros() != d.lower.nonZeros() || !std::equal(d.starts.begin(), d.starts.end(), lower.outerIndexPtr())
            || !std::equal(d.lower.innerIndexPtr(), d.lower.innerIndexPtr() + d.lower.nonZeros(), lower.innerIndexPtr())) {
            analyzePattern(a);
            if (d.info != Eigen::Success || d.n == 0) return;
        }
        d.lower = std::move(lower);
        d.starts.assign(d.lower.outerIndexPtr(), d.lower.outerIndexPtr() + d.lower.cols() + 1);
        SparseMatrix_Double m{d.structure(), d.lower.valuePtr()};
        accelerateError.clear();
        if (d.factored) {
            SparseRefactor(m, &d.numeric);
        } else {
            d.numeric = SparseFactor(d.symbolic, m);
            d.factored = true;
        }
        // (Not positive definite: a pivot of nought or less, said as Eigen's
        // LDLT would be read - the caller takes an LU.)
        d.info = d.numeric.status == SparseStatusOK ? Eigen::Success : Eigen::NumericalIssue;
        if (d.info != Eigen::Success) {
            SparseCleanup(d.numeric);
            d.factored = false;
        }
        return;
    }
#endif
    if (!d.eigen) {
        d.eigen = std::make_unique<Eigen::SimplicialLDLT<Matrix>>();
        d.eigen->analyzePattern(a);
    }
    d.eigen->factorize(a);
    d.info = d.eigen->info();
    // (A Cholesky of a matrix not positive definite: a pivot of 0 or less.)
    if (d.info == Eigen::Success && !(d.eigen->vectorD().array() > 0).all()) d.info = Eigen::NumericalIssue;
}

Eigen::ComputationInfo SparseCholesky::info() const
{
    return a_impl->info;
}

Eigen::VectorXd SparseCholesky::solve(const Eigen::VectorXd& b) const
{
    Impl& d = *a_impl;
#if defined(QUCS_ACCELERATE)
    if (d.accelerate) {
        Eigen::VectorXd x(b.size());
        if (d.n == 0 || !d.factored || b.size() != d.n) {
            x.setConstant(std::numeric_limits<double>::quiet_NaN());
            return x;
        }
        Eigen::VectorXd in = b;
        SparseSolve(d.numeric, DenseVector_Double{d.n, in.data()}, DenseVector_Double{d.n, x.data()});
        return x;
    }
#endif
    if (!d.eigen || d.info != Eigen::Success) return Eigen::VectorXd::Constant(b.size(), std::numeric_limits<double>::quiet_NaN());
    return Eigen::VectorXd(d.eigen->solve(b));
}

} // namespace qucs_s::fem
