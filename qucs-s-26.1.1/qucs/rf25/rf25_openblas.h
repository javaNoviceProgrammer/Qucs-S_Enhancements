/*
 * rf25_openblas.h - OpenBLAS's LAPACK for RF 2.5D's dense LU, where
 *                   Accelerate's is not (rf25_dense.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Linux's and Windows' counterpart of Accelerate's LAPACK: OpenBLAS's
 * getrf, getrs and gecon, multithreaded (its OpenMP build's threads, or its
 * own), linked where the build found it (QUCS_OPENBLAS, CMakeLists.txt).
 * In a file of its own, apart from rf25_dense.cpp: Accelerate's header
 * declares the same functions under its own names, and the two may not
 * meet in one file.
 *
 * A call runs one of two ways, by the threads it has of the machine's - a
 * sweep's frequency the share the frequencies at once leave it:
 * - on the lane, a thread kept for them, one call at a time, on all of
 *   OpenBLAS's threads: a call that has the machine, or a good part of it
 *   (kLaneFrom threads), as the quasi-static solve and a sweep of one or a
 *   few frequencies at a time. (Not each on its own team: an OpenMP
 *   runtime keeps a team for each thread that calls into it, and OpenBLAS's
 *   buffers and its count of threads are shared between its callers.)
 * - alone: in its own thread, side by side with the others, on one thread -
 *   a sweep of many frequencies at once, where taking turns at the whole
 *   machine was slower than Eigen's side by side (twenty-four frequencies
 *   of 1059 runs: 1.60 s against Eigen's 0.86). OpenBLAS's OpenMP build
 *   keeps to one thread, and touches nothing shared, when it is called
 *   inside an active OpenMP region; the call is made inside one of two
 *   threads, the second idle. That needs OpenBLAS's OpenMP build and this
 *   build's OpenMP its runtime (QUCS_OPENBLAS_OPENMP: GCC's libgomp on
 *   Linux and MSYS2, Homebrew's libomp on macOS): where it is not, such a
 *   call is none of OpenBLAS's, and Eigen's factorizes it (wayFor()).
 */
#ifndef QUCS_RF25_OPENBLAS_H
#define QUCS_RF25_OPENBLAS_H

#include <QString>

#include <complex>
#include <functional>

namespace qucs_s::rf25::openblas {

/// This build has OpenBLAS, and its LAPACK is the one called: on macOS,
/// where Accelerate exports the same names, the one the program found
/// them in is OpenBLAS.
bool here();
/// Its version, threading and threads, as OpenBLAS says them: "OpenBLAS
/// 0.3.26, OpenMP, 16 threads". Empty where it is not here.
QString about();

/// The threads from which a call takes the lane: fewer, it is made alone.
constexpr int kLaneFrom = 4;

/// OpenBLAS's calls can be made alone, side by side on one thread each:
/// its OpenMP build, and this build's OpenMP its runtime.
bool sideBySide();

/// How a call with \a threads of the machine's (0: all of them) runs: on
/// the lane, alone, or not by OpenBLAS (it is not here, or the call would
/// be alone and cannot be).
enum class Way { Lane, Alone, None };
Way wayFor(int threads);

/// \a job made \a way (Lane or Alone); the seconds it waited for its turn
/// on the lane. (An alone call that finds itself in no active region - its
/// OpenMP gave it one thread - takes the lane.)
double run(const std::function<void()>& job, Way way = Way::Lane);

/// LAPACK's getrf of a square column-major matrix where it is: its info.
int factorize(int n, std::complex<double>* a, int* pivots);
int factorize(int n, double* a, int* pivots);
/// getrs: b (n x columns) overwritten by A^-1 b.
void substitute(int n, int columns, const std::complex<double>* lu, const int* pivots, std::complex<double>* b);
void substitute(int n, int columns, const double* lu, const int* pivots, double* b);
/// gecon in the 1-norm, A's 1-norm \a norm.
double condition(int n, const std::complex<double>* lu, double norm);
double condition(int n, const double* lu, double norm);

} // namespace qucs_s::rf25::openblas

#endif
