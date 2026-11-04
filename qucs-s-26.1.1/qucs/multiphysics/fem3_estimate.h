/*
 * fem3_estimate.h - what a 3D solve will hold and take, said before it
 *                   starts: each physics' unknowns and its matrix's
 *                   nonzeros (counted), and the memory and time of an
 *                   iterative and of a direct solve, fitted to the solves
 *                   measured in phase C; the limit a solve is refused past
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM3_ESTIMATE_H
#define QUCS_FEM3_ESTIMATE_H

#include "fem3_fe.h"
#include "fem_model.h"

#include <memory>
#include <vector>

namespace qucs_s::fem {

/*!
 * One physics' solve: its unknowns, its matrix's nonzeros (the pairs of
 * degrees of freedom that share a prism, counted), and its peak memory and
 * seconds by conjugate gradients with algebraic multigrid and by a Cholesky
 * in METIS's order.
 *
 * A solve's peak is the larger of its assembly - the elements' triplets, 16
 * bytes each of elements x per element², and the matrix made of them twice -
 * and its matrix with the solver's own: multigrid's hierarchy and the rest,
 * 85 bytes a nonzero; the Cholesky's factor, 74.3 bytes x n^1.248 x (its
 * nonzeros a row)^0.480. Its seconds are the assembly's and the solver's:
 * multigrid's 0.15 us a nonzero, the Cholesky's 0.25 s + 2.14e-9 s x
 * n^1.387 x (nonzeros a row)^1.292. Fitted on an M2 Ultra to the solves of
 * boxes of order 1 and 2 from 15,000 to 310,000 unknowns in the app's own
 * solver (docs/feature_gaps/2026-11-04-multiphysics-3d-estimates) and the
 * order-1 Laplacians of phase C to 1,020,100 (its amg_bench.tsv): multigrid's
 * memory within 7 % of each, its time 25 % past 40,000 unknowns; the
 * Cholesky's memory within 17 %, its time within a factor of two.
 */
struct Estimate3 {
    QString physics;   ///< its tag
    QString name;      ///< its node's name
    int order = 1;
    qint64 unknowns = 0;
    qint64 nonzeros = 0;
    qint64 elements = 0;
    int perElement = 6;
    double iterativeBytes = 0, iterativeSeconds = 0;
    double directBytes = 0, directSeconds = 0;
    /// How it is solved: "iterative", "direct", or "auto" - the solver's own
    /// choice, which may factorize a matrix of many right-hand sides (no
    /// more than 300,000 unknowns, on macOS).
    QString method = QStringLiteral("iterative");
    /// Its peak (bytes): the method's, auto's the larger of the two.
    double bytes() const;
    double seconds() const;
};

/// The nonzeros of \a space's matrix: the pairs of its degrees of freedom
/// that share a prism - counted on \a threads (0: one below 20,000 rows,
/// else the machine's, at most 16).
qint64 patternNonzeros(const Space3& space, int threads = 0);

/// One physics on \a space, its matrix of \a nonzeros, solved by \a solver
/// (auto, direct, iterative; empty: auto).
Estimate3 estimateOn(const Space3& space, qint64 nonzeros, const QString& solver);

/// Each enabled physics of \a model on \a mesh, each on its order's space
/// (made once an order), solved by \a solver.
std::vector<Estimate3> estimate3(const Model& model, std::shared_ptr<const Mesh3> mesh, const QString& solver = {});

/// A study's peak: its largest physics' (each solves alone and lets go of
/// its system).
double peakBytes(const std::vector<Estimate3>& estimates);

/// "ht (order 1): 28,830 unknowns, 410,248 nonzeros - by multigrid some 52 MB
/// and 0.1 s, by a direct solve some 160 MB and 0.2 s".
QString estimateText(const Estimate3& e);
/// A size: "350 MB", "1.2 GB" (2^20 and 2^30 bytes).
QString bytesText(double bytes);

/// Why a study of \a estimates is not solved past \a limit (bytes), and what
/// would bring it under: the iterative solver, a coarser mesh, fewer
/// divisions in z (empty: it fits, or no limit).
QString pastTheLimit(const std::vector<Estimate3>& estimates, double limit);

/// The solvers' own terms, as fitted (bytes and seconds of a matrix of \a
/// unknowns rows and \a nonzeros).
double iterativeSolverBytes(double nonzeros);
double iterativeSolverSeconds(double nonzeros);
double directSolverBytes(double unknowns, double nonzeros);
double directSolverSeconds(double unknowns, double nonzeros);
/// The assembly's seconds: \a entries element-matrix entries (elements x
/// per element²).
double assemblySeconds(double entries);

/// This machine's memory (bytes; 0 not known).
double physicalMemory();

} // namespace qucs_s::fem

#endif // QUCS_FEM3_ESTIMATE_H
