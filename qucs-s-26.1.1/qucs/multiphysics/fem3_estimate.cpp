/*
 * fem3_estimate.cpp - what a 3D solve will hold and take, said before it
 *                     starts
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem3_estimate.h"
#include "fem3_mesh.h"
#include "fem_sparse.h"

#include <QCoreApplication>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <thread>

#if defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Estimate3", text);
}

// (Eigen's compressed matrix: a double and an index a nonzero, an index a
// column; an element-matrix entry's triplet, two indices and a double.)
constexpr double kMatrixPerNonzero = 12, kMatrixPerColumn = 4, kTriplet = 16;
// The vectors a solve holds beside its matrix: the right-hand side, the
// solution and conjugate gradients' four.
constexpr double kVectors = 6 * 8;
// The largest a matrix factorizes at on its own (System3::factorize).
constexpr qint64 kAutoDirectAtMost = 300000;

} // namespace

double iterativeSolverBytes(double nonzeros)
{
    // Multigrid's hierarchy and ILU(0) smoothers over the matrix, and what
    // else a solve holds: 85 bytes a nonzero (80 to 88 in the app's solves of
    // orders 1 and 2; phase C's benchmark of the solver alone 74).
    return 85.0 * nonzeros;
}

double iterativeSolverSeconds(double nonzeros)
{
    // Its set-up and some 11 to 15 iterations: 0.15 us a nonzero (within 25
    // %); harder matrices - thin layers, order 2's - take more iterations.
    return 1.508e-7 * nonzeros;
}

double directSolverBytes(double unknowns, double nonzeros)
{
    // The METIS-ordered factor: its fill grows with the rows and, more
    // slowly, with a row's nonzeros - order 2's denser rows fill more for
    // the same unknowns (within 17 % of each measured).
    if (!(unknowns > 0)) return 0;
    return 74.3342 * std::pow(unknowns, 1.2483) * std::pow(nonzeros / unknowns, 0.4802);
}

double directSolverSeconds(double unknowns, double nonzeros)
{
    // Its analysis, factorization and a solve, and its threads' start
    // (within a factor of two: timings under the other work of the machine).
    if (!(unknowns > 0)) return 0;
    return 0.25 + 2.14424e-9 * std::pow(unknowns, 1.3874) * std::pow(nonzeros / unknowns, 1.2916);
}

double assemblySeconds(double entries)
{
    // The elements' matrices, their coefficients read at each quadrature
    // point, and the matrix made of their triplets, and a solve's set-up:
    // fitted to the boxes of order 1 and 2 measured beside the solvers'.
    return 0.05 + 2.5e-8 * entries;
}

double Estimate3::bytes() const
{
    if (method == QLatin1String("direct")) return directBytes;
    if (method == QLatin1String("auto")) return std::max(directBytes, iterativeBytes);
    return iterativeBytes;
}

double Estimate3::seconds() const
{
    return method == QLatin1String("direct") ? directSeconds : iterativeSeconds;
}

qint64 patternNonzeros(const Space3& space, int threads)
{
    // Each degree of freedom's elements, then the degrees of freedom they
    // hold: its row's nonzeros.
    const int n = space.size(), per = space.perElement(), elements = space.elements();
    std::vector<int> start(std::size_t(n) + 1, 0);
    for (int e = 0; e < elements; ++e) {
        const int* d = space.dofs(e);
        for (int i = 0; i < per; ++i)
            if (d[i] >= 0) ++start[std::size_t(d[i]) + 1];
    }
    for (int i = 0; i < n; ++i) start[std::size_t(i) + 1] += start[std::size_t(i)];
    std::vector<int> of(std::size_t(start.back()), 0);
    std::vector<int> at(start.begin(), start.end() - 1);
    for (int e = 0; e < elements; ++e) {
        const int* d = space.dofs(e);
        for (int i = 0; i < per; ++i)
            if (d[i] >= 0) of[std::size_t(at[std::size_t(d[i])]++)] = e;
    }
    // Each row's degrees of freedom, sorted and counted once: rows in chunks
    // on threads (a million unknowns' took 0.9 s on one), summed in order.
    auto count = [&space, &start, &of, per](int from, int to) {
        qint64 total = 0;
        std::vector<int> row;
        for (int i = from; i < to; ++i) {
            row.clear();
            for (int k = start[std::size_t(i)]; k < start[std::size_t(i) + 1]; ++k) {
                const int* d = space.dofs(of[std::size_t(k)]);
                for (int j = 0; j < per; ++j)
                    if (d[j] >= 0) row.push_back(d[j]);
            }
            std::sort(row.begin(), row.end());
            total += std::unique(row.begin(), row.end()) - row.begin();
        }
        return total;
    };
    if (threads <= 0) threads = n < 20000 ? 1 : std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    threads = std::clamp(threads, 1, std::max(1, n));
    std::vector<qint64> part(std::size_t(threads), 0);
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back([&part, &count, t, threads, n] { part[std::size_t(t)] = count(int(qint64(n) * t / threads), int(qint64(n) * (t + 1) / threads)); });
    part[0] = count(0, int(qint64(n) / threads));
    for (std::thread& th : pool) th.join();
    qint64 total = 0;
    for (qint64 v : part) total += v;
    return total;
}

Estimate3 estimateOn(const Space3& space, qint64 nonzeros, const QString& solver)
{
    Estimate3 e;
    e.order = space.order();
    e.unknowns = space.size();
    e.nonzeros = nonzeros;
    e.elements = space.elements();
    e.perElement = space.perElement();
    const double nnz = double(nonzeros), n = double(e.unknowns);
    const double entries = double(e.elements) * e.perElement * e.perElement;
    const double matrix = kMatrixPerNonzero * nnz + kMatrixPerColumn * n;
    // Its assembly: the triplets, and the matrix made of them (Eigen sorts
    // them into a transposed one first).
    const double assembly = kTriplet * entries + 2 * matrix;
    const double vectors = kVectors * n;
    e.iterativeBytes = std::max(assembly, matrix + iterativeSolverBytes(nnz)) + vectors;
    e.directBytes = std::max(assembly, matrix + directSolverBytes(n, nnz)) + vectors;
    e.iterativeSeconds = assemblySeconds(entries) + iterativeSolverSeconds(nnz);
    e.directSeconds = assemblySeconds(entries) + directSolverSeconds(n, nnz);
    // (As System3::factorize chooses: direct asked, or on macOS a matrix of
    // many right-hand sides; off macOS auto iterates.)
    if (solver == QLatin1String("direct")) e.method = QStringLiteral("direct");
    else if (solver != QLatin1String("iterative") && accelerateInBuild() && acceleratedCholesky() && e.unknowns <= kAutoDirectAtMost)
        e.method = QStringLiteral("auto");
    else e.method = QStringLiteral("iterative");
    return e;
}

std::vector<Estimate3> estimate3(const Model& model, std::shared_ptr<const Mesh3> mesh, const QString& solver)
{
    std::vector<Estimate3> out;
    if (!mesh || mesh->prisms.empty()) return out;
    std::shared_ptr<const Space3> spaces[3];
    qint64 nonzeros[3] = {0, 0, 0};
    for (const Node* p : model.physics()) {
        if (!p->enabled) continue;
        const int order = p->text(QStringLiteral("order")) == QLatin1String("2") ? 2 : 1;
        if (!spaces[order]) {
            spaces[order] = std::make_shared<Space3>(mesh, order);
            nonzeros[order] = patternNonzeros(*spaces[order]);
        }
        Estimate3 e = estimateOn(*spaces[order], nonzeros[order], solver);
        e.physics = p->tag;
        e.name = p->name();
        out.push_back(e);
    }
    return out;
}

double peakBytes(const std::vector<Estimate3>& estimates)
{
    double most = 0;
    for (const Estimate3& e : estimates) most = std::max(most, e.bytes());
    return most;
}

QString bytesText(double bytes)
{
    if (bytes >= 1024.0 * 1048576.0) return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1048576.0), 0, 'f', bytes >= 10 * 1024.0 * 1048576.0 ? 0 : 1);
    return QStringLiteral("%1 MB").arg(std::max(1.0, std::round(bytes / 1048576.0)), 0, 'f', 0);
}

namespace {

QString secondsText(double s)
{
    if (s < 1) return QStringLiteral("%1 s").arg(s, 0, 'g', 1);
    if (s < 120) return QStringLiteral("%1 s").arg(s, 0, 'f', 0);
    if (s < 7200) return QStringLiteral("%1 min").arg(s / 60, 0, 'f', 0);
    return QStringLiteral("%1 h").arg(s / 3600, 0, 'f', 1);
}

} // namespace

QString estimateText(const Estimate3& e)
{
    const QLocale c;
    return tr("%1 (order %2): %3 unknowns, %4 nonzeros - by multigrid some %5 and %6, by a direct solve some %7 and %8%9")
        .arg(e.physics)
        .arg(e.order)
        .arg(c.toString(e.unknowns), c.toString(e.nonzeros), bytesText(e.iterativeBytes), secondsText(e.iterativeSeconds),
             bytesText(e.directBytes), secondsText(e.directSeconds),
             e.method == QLatin1String("direct") ? tr(" (direct asked)") : QString());
}

QString pastTheLimit(const std::vector<Estimate3>& estimates, double limit)
{
    if (!(limit > 0)) return {};
    const double peak = peakBytes(estimates);
    if (peak <= limit) return {};
    // What would fit: the iterative solver, where the direct one was asked
    // or may be chosen; else a mesh of fewer unknowns, by the share that
    // would - the memory's growth near the unknowns' own.
    QStringList ways;
    std::vector<Estimate3> iterated = estimates;
    for (Estimate3& e : iterated) e.method = QStringLiteral("iterative");
    const double iterative = peakBytes(iterated);
    if (iterative < peak && iterative <= limit) ways << tr("the iterative solver (some %1: fem_solve's solver iterative, or the step's Solver)").arg(bytesText(iterative));
    // (The memory near the unknowns' own growth, a factorization's more:
    // the share errs low.)
    const double share = limit / peak;
    ways << tr("a coarser mesh - %1 % of the unknowns, or fewer - or fewer divisions in z (the mesh's Distribution in z)")
                .arg(std::max(1, int(std::floor(100 * share))));
    return tr("it would take some %1, past the limit of %2 (Multiphysics 3D > Settings): %3. fem_solve's force solves past it all the same.")
        .arg(bytesText(peak), bytesText(limit), ways.join(tr("; or ")));
}

double physicalMemory()
{
#if defined(Q_OS_MACOS)
    quint64 bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) return double(bytes);
    return 0;
#elif defined(Q_OS_LINUX)
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? double(pages) * double(page) : 0;
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? double(status.ullTotalPhys) : 0;
#else
    return 0;
#endif
}

} // namespace qucs_s::fem
