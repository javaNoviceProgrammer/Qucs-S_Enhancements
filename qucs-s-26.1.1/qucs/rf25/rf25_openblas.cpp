/*
 * rf25_openblas.cpp - OpenBLAS's LAPACK for RF 2.5D's dense LU, where
 *                     Accelerate's is not
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_openblas.h"

#include <QElapsedTimer>
#include <QStringList>

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#if defined(QUCS_OPENBLAS) && defined(__APPLE__)
#include <dlfcn.h>
#endif
#if defined(QUCS_OPENBLAS_OPENMP)
#include <omp.h>
#endif

#if defined(QUCS_OPENBLAS)
// LAPACK's Fortran interface as OpenBLAS exports it - 32-bit integers (its
// LP64 build: Debian's libopenblas*-dev, MSYS2's, Homebrew's), and a
// character argument's length after the rest, as gfortran passes it (its
// gecon is LAPACK's own Fortran) - and OpenBLAS's own three. Declared here,
// not from a header: Debian keeps OpenBLAS's headers in a folder of each
// flavour's, and none of these needs more.
extern "C" {
void zgetrf_(const int* m, const int* n, std::complex<double>* a, const int* lda, int* pivots, int* info);
void dgetrf_(const int* m, const int* n, double* a, const int* lda, int* pivots, int* info);
void zgetrs_(const char* trans, const int* n, const int* columns, const std::complex<double>* a, const int* lda,
             const int* pivots, std::complex<double>* b, const int* ldb, int* info, std::size_t transLength);
void dgetrs_(const char* trans, const int* n, const int* columns, const double* a, const int* lda, const int* pivots,
             double* b, const int* ldb, int* info, std::size_t transLength);
void zgecon_(const char* norm, const int* n, const std::complex<double>* a, const int* lda, const double* anorm,
             double* rcond, std::complex<double>* work, double* rwork, int* info, std::size_t normLength);
void dgecon_(const char* norm, const int* n, const double* a, const int* lda, const double* anorm, double* rcond,
             double* work, int* iwork, int* info, std::size_t normLength);
char* openblas_get_config(void);
int openblas_get_num_threads(void);
int openblas_get_parallel(void);
}
#endif

namespace qucs_s::rf25::openblas {

namespace {

/// The one thread OpenBLAS is called on, and the calls' turns: a caller
/// holds \a callers from handing its job over until the job is done.
struct Lane {
    std::mutex callers;
    std::mutex mutex;
    std::condition_variable wake, done;
    const std::function<void()>* job = nullptr;

    void loop()
    {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return job != nullptr; });
            const std::function<void()>* now = job;
            lock.unlock();
            (*now)();
            lock.lock();
            job = nullptr;
            done.notify_all();
        }
    }
};

/// Made at the first call, kept to the process's end: its thread waits on
/// it until then (never destroyed, so never destroyed under it).
Lane& lane()
{
    static Lane* const made = [] {
        auto* l = new Lane;
        std::thread([l] { l->loop(); }).detach();
        return l;
    }();
    return *made;
}

} // namespace

bool here()
{
#if defined(QUCS_OPENBLAS) && defined(__APPLE__)
    // (Accelerate exports zgetrf_ too, and a program linked with both binds
    //  each name to whichever came first: OpenBLAS's only if it is the
    //  library zgetrf_ came from.)
    static const bool ours = [] {
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&zgetrf_), &info) == 0 || info.dli_fname == nullptr) return false;
        return QString::fromLocal8Bit(info.dli_fname).contains(QLatin1String("openblas"), Qt::CaseInsensitive);
    }();
    return ours;
#elif defined(QUCS_OPENBLAS)
    return true;
#else
    return false;
#endif
}

QString about()
{
#if defined(QUCS_OPENBLAS)
    if (!here()) return {};
    // ("OpenBLAS 0.3.26 NO_LAPACKE DYNAMIC_ARCH ... MAX_THREADS=64": its
    //  first two words.)
    const QStringList config = QString::fromLatin1(openblas_get_config()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString name = config.mid(0, 2).join(QLatin1Char(' '));
    const int parallel = openblas_get_parallel();
    const QString threading = parallel == 2 ? QStringLiteral("OpenMP") : parallel == 1 ? QStringLiteral("pthreads")
                                                                                       : QStringLiteral("sequential");
    const int threads = openblas_get_num_threads();
    return QStringLiteral("%1, %2, %3 thread%4").arg(name.isEmpty() ? QStringLiteral("OpenBLAS") : name, threading)
        .arg(threads)
        .arg(threads == 1 ? QString() : QStringLiteral("s"));
#else
    return {};
#endif
}

bool sideBySide()
{
#if defined(QUCS_OPENBLAS) && defined(QUCS_OPENBLAS_OPENMP)
    static const bool openMp = here() && openblas_get_parallel() == 2;
    return openMp;
#else
    return false;
#endif
}

Way wayFor(int threads)
{
    if (!here()) return Way::None;
#if defined(QUCS_OPENBLAS)
    if (threads <= 0 || threads >= kLaneFrom || threads >= openblas_get_num_threads()) return Way::Lane;
#endif
    return sideBySide() ? Way::Alone : Way::None;
}

namespace {

/// \a job inside an OpenMP region of two threads, on its first: OpenBLAS
/// in it keeps to one thread. False, \a job not made, where the region had
/// one thread (OMP_THREAD_LIMIT, OMP_DYNAMIC): not active, OpenBLAS would
/// take all of its threads.
bool alone(const std::function<void()>& job)
{
#if defined(QUCS_OPENBLAS_OPENMP)
    bool made = false;
#pragma omp parallel num_threads(2)
    {
        if (omp_get_thread_num() == 0 && omp_in_parallel()) {
            job();
            made = true;
        }
    }
    return made;
#else
    (void)job;
    return false;
#endif
}

} // namespace

double run(const std::function<void()>& job, Way way)
{
    if (way == Way::Alone && alone(job)) return 0;
    Lane& l = lane();
    QElapsedTimer clock;
    clock.start();
    const std::lock_guard turn(l.callers);
    const double waited = double(clock.nsecsElapsed()) * 1e-9;
    std::unique_lock lock(l.mutex);
    l.job = &job;
    l.wake.notify_one();
    l.done.wait(lock, [&l] { return l.job == nullptr; });
    return waited;
}

#if defined(QUCS_OPENBLAS)
int factorize(int n, std::complex<double>* a, int* pivots)
{
    int info = 0;
    zgetrf_(&n, &n, a, &n, pivots, &info);
    return info;
}

int factorize(int n, double* a, int* pivots)
{
    int info = 0;
    dgetrf_(&n, &n, a, &n, pivots, &info);
    return info;
}

void substitute(int n, int columns, const std::complex<double>* lu, const int* pivots, std::complex<double>* b)
{
    int info = 0;
    zgetrs_("N", &n, &columns, lu, &n, pivots, b, &n, &info, 1);
}

void substitute(int n, int columns, const double* lu, const int* pivots, double* b)
{
    int info = 0;
    dgetrs_("N", &n, &columns, lu, &n, pivots, b, &n, &info, 1);
}

double condition(int n, const std::complex<double>* lu, double norm)
{
    int info = 0;
    double rcond = 0;
    std::vector<std::complex<double>> work(std::size_t(2) * std::size_t(n));
    std::vector<double> rwork(std::size_t(2) * std::size_t(n));
    zgecon_("1", &n, lu, &n, &norm, &rcond, work.data(), rwork.data(), &info, 1);
    return rcond;
}

double condition(int n, const double* lu, double norm)
{
    int info = 0;
    double rcond = 0;
    std::vector<double> work(std::size_t(4) * std::size_t(n));
    std::vector<int> iwork(static_cast<std::size_t>(n));
    dgecon_("1", &n, lu, &n, &norm, &rcond, work.data(), iwork.data(), &info, 1);
    return rcond;
}
#else
// (Not called: rf25_dense.cpp asks here() first.)
int factorize(int, std::complex<double>*, int*) { return -1; }
int factorize(int, double*, int*) { return -1; }
void substitute(int, int, const std::complex<double>*, const int*, std::complex<double>*) {}
void substitute(int, int, const double*, const int*, double*) {}
double condition(int, const std::complex<double>*, double) { return 0; }
double condition(int, const double*, double) { return 0; }
#endif

} // namespace qucs_s::rf25::openblas
