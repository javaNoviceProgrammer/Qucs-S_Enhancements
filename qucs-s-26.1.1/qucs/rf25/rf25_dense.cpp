/*
 * rf25_dense.cpp - the dense LU of RF 2.5D's two large systems: the full
 *                  wave's runs of current and the quasi-static extraction's
 *                  charges
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_dense.h"

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#if defined(QUCS_ACCELERATE_LAPACK)
// (The new interface, macOS 13.3's: without this the header gives the
//  classic one.)
#define ACCELERATE_NEW_LAPACK
#include <Accelerate/Accelerate.h>
#endif

namespace qucs_s::rf25 {

namespace {

using Complex = std::complex<double>;

std::atomic<int> chosen{int(DenseLu::Automatic)};
std::atomic<long long> byAccelerate{0}, byEigen{0};

#if defined(QUCS_ACCELERATE_LAPACK)
using LapackInt = __LAPACK_int;

/// The blocked LU's panel, Eigen's own: 128 was a quarter slower at 4000
/// complex rows, 512 within 7 % either way.
constexpr LapackInt kPanel = 256;
/// The columns a solve is padded to: Accelerate's solve of two or three
/// took three times its solve of eight (33 and 10 ms at 4000 complex rows;
/// Eigen's of two 23).
constexpr LapackInt kSolveColumns = 8;

bool useAccelerate()
{
    return denseLu() != DenseLu::Eigen && accelerateLuHere();
}

/// A complex matrix's LU, kPanel columns at a time: each panel by zgetrf,
/// its row swaps to the columns either side by zlaswp, then U12 by ztrsm
/// and the trailing matrix by zgemm - Eigen's blocked LU over BLAS, of
/// LAPACK's own pieces. Accelerate's zgetrf whole was 6 to 7.5 times Eigen
/// at 4000 to 6000 rows, this 9 to 11. LAPACK's info: the first pivot that
/// is nought, from 1, or 0.
__attribute__((availability(macos, introduced = 13.3))) LapackInt factorize(LapackInt n, Complex* a,
                                                                             LapackInt* pivots)
{
    const Complex one(1, 0), minusOne(-1, 0);
    const LapackInt unit = 1;
    LapackInt info = 0;
    for (LapackInt k = 0; k < n; k += kPanel) {
        const LapackInt width = std::min(kPanel, n - k), rows = n - k, right = n - k - width;
        Complex* const panel = a + std::size_t(k) + std::size_t(k) * std::size_t(n);
        LapackInt status = 0;
        zgetrf_(&rows, &width, panel, &n, pivots + k, &status);
        if (status > 0 && info == 0) info = status + k;
        // (The panel's rows were counted from k: the whole's.)
        for (LapackInt i = k; i < k + width; ++i) pivots[i] += k;
        const LapackInt first = k + 1, last = k + width;
        if (k > 0) zlaswp_(&k, a, &n, &first, &last, pivots, &unit);
        if (right > 0) {
            Complex* const a12 = a + std::size_t(k) + std::size_t(k + width) * std::size_t(n);
            zlaswp_(&right, a + std::size_t(k + width) * std::size_t(n), &n, &first, &last, pivots, &unit);
            ztrsm_("L", "L", "N", "U", &width, &right, &one, panel, &n, a12, &n);
            zgemm_("N", "N", &right, &right, &width, &minusOne, panel + width, &n, a12, &n, &one, a12 + width, &n);
        }
    }
    return info;
}

/// A real matrix's: dgetrf whole, 15 to 17 times Eigen at 6000 to 8000.
__attribute__((availability(macos, introduced = 13.3))) LapackInt factorize(LapackInt n, double* a,
                                                                             LapackInt* pivots)
{
    LapackInt info = 0;
    dgetrf_(&n, &n, a, &n, pivots, &info);
    return info;
}

__attribute__((availability(macos, introduced = 13.3))) void substitute(LapackInt n, LapackInt columns,
                                                                         const Complex* lu, const LapackInt* pivots,
                                                                         Complex* b)
{
    LapackInt info = 0;
    zgetrs_("N", &n, &columns, lu, &n, pivots, b, &n, &info);
}

__attribute__((availability(macos, introduced = 13.3))) void substitute(LapackInt n, LapackInt columns,
                                                                         const double* lu, const LapackInt* pivots,
                                                                         double* b)
{
    LapackInt info = 0;
    dgetrs_("N", &n, &columns, lu, &n, pivots, b, &n, &info);
}

__attribute__((availability(macos, introduced = 13.3))) double condition(LapackInt n, const Complex* lu, double norm)
{
    LapackInt info = 0;
    double rcond = 0;
    std::vector<Complex> work(std::size_t(2 * n));
    std::vector<double> rwork(std::size_t(2 * n));
    zgecon_("1", &n, lu, &n, &norm, &rcond, work.data(), rwork.data(), &info);
    return rcond;
}

__attribute__((availability(macos, introduced = 13.3))) double condition(LapackInt n, const double* lu, double norm)
{
    LapackInt info = 0;
    double rcond = 0;
    std::vector<double> work(std::size_t(4 * n));
    std::vector<LapackInt> iwork(static_cast<std::size_t>(n));
    dgecon_("1", &n, lu, &n, &norm, &rcond, work.data(), iwork.data(), &info);
    return rcond;
}
#else
using LapackInt = int;
#endif

} // namespace

void setDenseLu(DenseLu choice)
{
    chosen = int(choice);
}

DenseLu denseLu()
{
    const QByteArray asked = qgetenv("QUCS_RF25_LU").trimmed().toLower();
    if (asked == "eigen") return DenseLu::Eigen;
    if (asked == "accelerate") return DenseLu::Accelerate;
    if (asked == "auto" || asked == "automatic") return DenseLu::Automatic;
    if (qgetenv("QUCS_FEM_SOLVER").trimmed().toLower() == "eigen") return DenseLu::Eigen;
    return DenseLu(chosen.load());
}

bool accelerateLuHere()
{
#if defined(QUCS_ACCELERATE_LAPACK)
    if (__builtin_available(macOS 13.3, *)) return true;
#endif
    return false;
}

long long accelerateLuCount()
{
    return byAccelerate.load();
}

long long eigenLuCount()
{
    return byEigen.load();
}

template <class Scalar> struct DenseFactors<Scalar>::Impl {
    Matrix lu;                       ///< A, then its factors: L below the diagonal (its ones not held), U on and above
    std::vector<LapackInt> pivots;   ///< LAPACK's: row i swapped with pivots[i] - 1, in turn
    double norm = 0;                 ///< A's 1-norm, for gecon
    LapackInt info = 0;              ///< LAPACK's: a pivot nought
    bool accelerated = false;
    /// Eigen's, on lu where it is.
    std::optional<Eigen::PartialPivLU<Eigen::Ref<Matrix>>> eigen;
};

template <class Scalar> DenseFactors<Scalar>::DenseFactors(Matrix&& a) : d(std::make_unique<Impl>())
{
    d->lu = std::move(a);
#if defined(QUCS_ACCELERATE_LAPACK)
    if (useAccelerate()) {
        if (__builtin_available(macOS 13.3, *)) {
            const LapackInt n = LapackInt(d->lu.rows());
            d->pivots.assign(std::size_t(n), 0);
            if (n > 0) {
                d->norm = d->lu.cwiseAbs().colwise().sum().maxCoeff();
                d->info = factorize(n, d->lu.data(), d->pivots.data());
            }
            d->accelerated = true;
            ++byAccelerate;
            return;
        }
    }
#endif
    d->eigen.emplace(d->lu);
    ++byEigen;
}

template <class Scalar> DenseFactors<Scalar>::~DenseFactors() = default;

template <class Scalar> typename DenseFactors<Scalar>::Matrix DenseFactors<Scalar>::solve(const Matrix& b) const
{
    if (!d->accelerated) return d->eigen->solve(b);
#if defined(QUCS_ACCELERATE_LAPACK)
    if (__builtin_available(macOS 13.3, *)) {
        const LapackInt n = LapackInt(d->lu.rows()), columns = LapackInt(b.cols());
        if (n == 0 || columns == 0) return Matrix::Zero(n, columns);
        Matrix x = Matrix::Zero(n, std::max(columns, kSolveColumns));
        x.leftCols(columns) = b;
        substitute(n, LapackInt(x.cols()), d->lu.data(), d->pivots.data(), x.data());
        if (x.cols() != columns) x.conservativeResize(Eigen::NoChange, columns);
        return x;
    }
#endif
    // (Not reached: only Accelerate's own branch factorizes so.)
    return Matrix::Constant(b.rows(), b.cols(), Scalar(std::numeric_limits<double>::quiet_NaN()));
}

template <class Scalar> double DenseFactors<Scalar>::rcond() const
{
    if (!d->accelerated) return d->eigen->rcond();
#if defined(QUCS_ACCELERATE_LAPACK)
    if (__builtin_available(macOS 13.3, *)) {
        const LapackInt n = LapackInt(d->lu.rows());
        if (n == 0) return 1;
        // (A pivot of nought: singular, which gecon does not see.)
        if (d->info > 0 || !(d->norm > 0)) return 0;
        return condition(n, d->lu.data(), d->norm);
    }
#endif
    return 0;
}

template <class Scalar> bool DenseFactors<Scalar>::accelerated() const
{
    return d->accelerated;
}

template class DenseFactors<double>;
template class DenseFactors<std::complex<double>>;

} // namespace qucs_s::rf25
