/*
 * rf25_fast.cpp - a dense matrix of a method of moments held compressed, and
 *                 solved by iteration
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_fast.h"

#include <QHash>

#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(_WIN32)
// Lean: the whole of <windows.h> brings rpcndr.h, whose macro small is char.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace qucs_s::rf25 {

void onThreads(int threads, const std::function<void(int)>& job)
{
    QList<QThread*> pool;
    for (int t = 1; t < threads; ++t) {
        QThread* thread = QThread::create([&job, t] { job(t); });
        thread->setStackSize(8u << 20);
        thread->start();
        pool << thread;
    }
    job(0);
    for (QThread* thread : std::as_const(pool)) {
        thread->wait();
        delete thread;
    }
}

Box Box::of(double x0, double y0, double z0, double x1, double y1, double z1)
{
    Box b;
    b.lo[0] = std::min(x0, x1);
    b.lo[1] = std::min(y0, y1);
    b.lo[2] = std::min(z0, z1);
    b.hi[0] = std::max(x0, x1);
    b.hi[1] = std::max(y0, y1);
    b.hi[2] = std::max(z0, z1);
    return b;
}

Box Box::united(const Box& other) const
{
    Box b;
    for (int a = 0; a < 3; ++a) {
        b.lo[a] = std::min(lo[a], other.lo[a]);
        b.hi[a] = std::max(hi[a], other.hi[a]);
    }
    return b;
}

double Box::diameter() const
{
    return std::sqrt(side(0) * side(0) + side(1) * side(1) + side(2) * side(2));
}

double Box::distance(const Box& other) const
{
    double sum = 0;
    for (int a = 0; a < 3; ++a) {
        const double gap = std::max({0.0, other.lo[a] - hi[a], lo[a] - other.hi[a]});
        sum += gap * gap;
    }
    return std::sqrt(sum);
}

ClusterTree::ClusterTree(const QList<Box>& items, int leaf)
{
    a_order.resize(std::size_t(items.size()));
    std::iota(a_order.begin(), a_order.end(), 0);
    if (items.isEmpty()) return;
    a_nodes.reserve(std::size_t(4 * items.size() / std::max(1, leaf) + 4));
    split(0, int(items.size()), items, std::max(1, leaf));
}

int ClusterTree::split(int begin, int end, const QList<Box>& items, int leaf)
{
    const int index = int(a_nodes.size());
    a_nodes.push_back(Node());
    Node node;
    node.begin = begin;
    node.end = end;
    node.box = items.at(a_order[std::size_t(begin)]);
    double lo[3], hi[3];
    for (int a = 0; a < 3; ++a) lo[a] = hi[a] = node.box.centre(a);
    for (int i = begin; i < end; ++i) {
        const Box& b = items.at(a_order[std::size_t(i)]);
        node.box = node.box.united(b);
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], b.centre(a));
            hi[a] = std::max(hi[a], b.centre(a));
        }
    }
    if (end - begin > leaf) {
        // Halved at the middle item along the longest side its centres
        // span; items all at one place are one leaf, however many.
        int axis = 0;
        for (int a = 1; a < 3; ++a)
            if (hi[a] - lo[a] > hi[axis] - lo[axis]) axis = a;
        if (hi[axis] > lo[axis]) {
            const int middle = begin + (end - begin) / 2;
            std::nth_element(a_order.begin() + begin, a_order.begin() + middle, a_order.begin() + end,
                             [&items, axis](int a, int b) { return items.at(a).centre(axis) < items.at(b).centre(axis); });
            node.child[0] = split(begin, middle, items, leaf);
            node.child[1] = split(middle, end, items, leaf);
        }
    }
    a_nodes[std::size_t(index)] = node;
    return index;
}

HMatrix::HMatrix(const ClusterTree* rows, const ClusterTree* columns, bool symmetric, double eta)
    : a_rows(rows), a_columns(columns), a_symmetric(symmetric), a_eta(eta)
{
    if (rows == nullptr || columns == nullptr || rows->nodes().empty() || columns->nodes().empty()) return;
    build(0, 0);
}

void HMatrix::build(int row, int column)
{
    const ClusterTree::Node& r = a_rows->nodes()[std::size_t(row)];
    const ClusterTree::Node& c = a_columns->nodes()[std::size_t(column)];
    Block block;
    block.row = row;
    block.column = column;
    // A symmetric matrix's diagonal blocks are split into their halves'
    // two diagonal blocks and the one between them; the other way round of
    // that is its transpose, which multiply() applies from the same block.
    if (a_symmetric && row == column) {
        if (r.leaf()) {
            a_blocks.push_back(block);
            return;
        }
        build(r.child[0], r.child[0]);
        build(r.child[0], r.child[1]);
        build(r.child[1], r.child[1]);
        return;
    }
    const double apart = r.box.distance(c.box);
    if (apart > 0 && std::min(r.box.diameter(), c.box.diameter()) <= a_eta * apart) {
        block.lowRank = true;
        a_blocks.push_back(block);
        return;
    }
    if (r.leaf() && c.leaf()) {
        a_blocks.push_back(block);
        return;
    }
    if (r.leaf()) {
        build(row, c.child[0]);
        build(row, c.child[1]);
        return;
    }
    if (c.leaf()) {
        build(r.child[0], column);
        build(r.child[1], column);
        return;
    }
    build(r.child[0], c.child[0]);
    build(r.child[0], c.child[1]);
    build(r.child[1], c.child[0]);
    build(r.child[1], c.child[1]);
}

bool HMatrix::fill(const Entry& entry, double tolerance, int threads, const std::atomic_bool* stop)
{
    a_threads = std::max(1, threads);
    // The largest blocks first, so that the threads end together.
    std::vector<int> work(a_blocks.size());
    std::iota(work.begin(), work.end(), 0);
    const auto area = [this](int k) {
        const Block& b = a_blocks[std::size_t(k)];
        return double(a_rows->nodes()[std::size_t(b.row)].size()) * double(a_columns->nodes()[std::size_t(b.column)].size());
    };
    std::sort(work.begin(), work.end(), [&area](int a, int b) { return area(a) > area(b); });
    std::atomic_int next{0};
    std::atomic_bool halted{false};
    const int count = int(work.size());
    const auto job = [this, &work, &next, &halted, &entry, tolerance, stop, count](int) {
        for (int w = next++; w < count; w = next++) {
            if (halted || (stop != nullptr && stop->load())) {
                halted = true;
                return;
            }
            Block& b = a_blocks[std::size_t(work[std::size_t(w)])];
            const ClusterTree::Node& r = a_rows->nodes()[std::size_t(b.row)];
            const ClusterTree::Node& c = a_columns->nodes()[std::size_t(b.column)];
            const int m = r.size(), n = c.size();
            const int* rows = a_rows->order().data() + r.begin;
            const int* columns = a_columns->order().data() + c.begin;
            if (b.lowRank) {
                // A rank whose two sides hold as much as the block itself
                // is no saving.
                const int most = int(double(m) * double(n) / double(m + n));
                if (most >= 1 && crossApproximation(entry, rows, m, columns, n, tolerance, most, &b.u, &b.v)) continue;
                b.lowRank = false;
                b.u.resize(0, 0);
                b.v.resize(0, 0);
            }
            b.dense.resize(m, n);
            if (a_symmetric && b.row == b.column) {
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i <= j; ++i) b.dense(i, j) = entry(rows[i], columns[j]);
                for (int j = 0; j < n; ++j)
                    for (int i = j + 1; i < m; ++i) b.dense(i, j) = b.dense(j, i);
            } else {
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i < m; ++i) b.dense(i, j) = entry(rows[i], columns[j]);
            }
        }
    };
    onThreads(std::min(a_threads, std::max(1, count)), job);
    return !halted;
}

void HMatrix::multiply(const VectorC& x, VectorC& y, Complex scale) const
{
    apply(x, y, scale, false);
}

void HMatrix::multiplyTransposed(const VectorC& x, VectorC& y, Complex scale) const
{
    apply(x, y, scale, a_symmetric ? false : true);
}

void HMatrix::apply(const VectorC& x, VectorC& y, Complex scale, bool transposed) const
{
    if (a_blocks.empty()) return;
    const ClusterTree* in = transposed ? a_rows : a_columns;
    const ClusterTree* out = transposed ? a_columns : a_rows;
    VectorC xp(in->items());
    for (int p = 0; p < in->items(); ++p) xp[p] = x[in->order()[std::size_t(p)]];
    // Shared over threads only where there is work enough to pay for them:
    // each its own sum, added together at the end.
    double work = 0;
    for (const Block& b : a_blocks) work += double(b.dense.size() + b.u.size() + b.v.size());
    const int threads = work > 2e5 ? std::min(a_threads, int(a_blocks.size())) : 1;
    std::vector<VectorC> partial(std::size_t(threads), VectorC::Zero(out->items()));
    const auto job = [this, &xp, &partial, threads, transposed](int t) {
        VectorC& yp = partial[std::size_t(t)];
        for (std::size_t k = std::size_t(t); k < a_blocks.size(); k += std::size_t(threads)) {
            const Block& b = a_blocks[k];
            const ClusterTree::Node& r = a_rows->nodes()[std::size_t(b.row)];
            const ClusterTree::Node& c = a_columns->nodes()[std::size_t(b.column)];
            const int m = r.size(), n = c.size();
            // This block, and its transpose where it stands for both.
            const auto forward = [&] {
                if (b.lowRank) {
                    if (b.u.cols() > 0) yp.segment(r.begin, m).noalias() += b.u * (b.v.transpose() * xp.segment(c.begin, n));
                } else {
                    yp.segment(r.begin, m).noalias() += b.dense * xp.segment(c.begin, n);
                }
            };
            const auto backward = [&] {
                if (b.lowRank) {
                    if (b.u.cols() > 0) yp.segment(c.begin, n).noalias() += b.v * (b.u.transpose() * xp.segment(r.begin, m));
                } else {
                    yp.segment(c.begin, n).noalias() += b.dense.transpose() * xp.segment(r.begin, m);
                }
            };
            if (transposed) {
                backward();
            } else {
                forward();
                if (a_symmetric && b.row != b.column) backward();
            }
        }
    };
    onThreads(threads, job);
    for (int t = 1; t < threads; ++t) partial[0] += partial[std::size_t(t)];
    for (int p = 0; p < out->items(); ++p) y[out->order()[std::size_t(p)]] += scale * partial[0][p];
}

double HMatrix::bytes() const
{
    double sum = 0;
    for (const Block& b : a_blocks) sum += 16.0 * double(b.dense.size() + b.u.size() + b.v.size());
    return sum;
}

double HMatrix::denseBytes() const
{
    const double n = double(rows()), m = double(columns());
    return a_symmetric ? 8.0 * n * (n + 1) : 16.0 * n * m;
}

double HMatrix::denseBlockBytes() const
{
    double sum = 0;
    for (const Block& b : a_blocks)
        if (!b.lowRank)
            sum += 16.0 * double(a_rows->nodes()[std::size_t(b.row)].size()) * double(a_columns->nodes()[std::size_t(b.column)].size());
    return sum;
}

double HMatrix::lowRankSpan() const
{
    double sum = 0;
    for (const Block& b : a_blocks)
        if (b.lowRank)
            sum += double(a_rows->nodes()[std::size_t(b.row)].size()) + double(a_columns->nodes()[std::size_t(b.column)].size());
    return sum;
}

int HMatrix::largestRank() const
{
    int k = 0;
    for (const Block& b : a_blocks)
        if (b.lowRank) k = std::max(k, int(b.u.cols()));
    return k;
}

double HMatrix::meanRank() const
{
    double ranked = 0, spanned = 0;
    for (const Block& b : a_blocks)
        if (b.lowRank) {
            const double span = double(b.u.rows() + b.v.rows());
            ranked += span * double(b.u.cols());
            spanned += span;
        }
    return spanned > 0 ? ranked / spanned : 0;
}

int HMatrix::lowRankBlocks() const
{
    int count = 0;
    for (const Block& b : a_blocks) count += b.lowRank ? 1 : 0;
    return count;
}

bool crossApproximation(const HMatrix::Entry& entry, const int* rows, int m, const int* columns, int n, double tolerance,
                        int most, Eigen::MatrixXcd* u, Eigen::MatrixXcd* v)
{
    std::vector<VectorC> us, vs;
    std::vector<char> used(std::size_t(m), 0);
    double held = 0;   // the Frobenius norm squared of the sum so far
    int row = m / 2, misses = 0, slight = 0;
    VectorC r(n), c(m);
    while (true) {
        // The row's residual: what the sum so far leaves of it.
        for (int j = 0; j < n; ++j) r[j] = entry(rows[row], columns[j]);
        for (std::size_t l = 0; l < us.size(); ++l) r -= us[l][row] * vs[l];
        used[std::size_t(row)] = 1;
        int pivot = 0;
        double largest = 0;
        for (int j = 0; j < n; ++j)
            if (std::abs(r[j]) > largest) {
                largest = std::abs(r[j]);
                pivot = j;
            }
        if (!(largest > 0)) {
            // Held exactly along this row - or, nothing held yet, a block
            // of nought along it: another row, a few, before it is taken
            // for done (a block of nought, every row of it).
            if (++misses >= (us.empty() ? std::min(m, 8) : 3)) break;
            int next = -1;
            double best = -1;
            if (!us.empty())
                for (int i = 0; i < m; ++i)
                    if (!used[std::size_t(i)] && std::abs(us.back()[i]) > best) {
                        best = std::abs(us.back()[i]);
                        next = i;
                    }
            if (next < 0)
                for (int step = 1; step < m && next < 0; ++step)
                    if (!used[std::size_t((row + step * std::max(1, m / 8)) % m)]) next = (row + step * std::max(1, m / 8)) % m;
            if (next < 0)
                for (int i = 0; i < m && next < 0; ++i)
                    if (!used[std::size_t(i)]) next = i;
            if (next < 0) break;
            row = next;
            continue;
        }
        misses = 0;
        const VectorC vk = r / r[pivot];
        for (int i = 0; i < m; ++i) c[i] = entry(rows[i], columns[pivot]);
        for (std::size_t l = 0; l < us.size(); ++l) c -= vs[l][pivot] * us[l];
        double cross = 0;
        for (std::size_t l = 0; l < us.size(); ++l) cross += 2 * std::real(us[l].dot(c) * vs[l].dot(vk));
        const double nu = c.squaredNorm(), nv = vk.squaredNorm();
        held = std::max(0.0, held + cross + nu * nv);
        us.push_back(c);
        vs.push_back(vk);
        // Done when two outer products running each add less than the
        // tolerance of what is held: one alone can be small where the next
        // row's residual is not (a block of two clusters at different
        // heights, its rows of one cluster's half).
        if (nu * nv <= tolerance * tolerance * held) {
            if (++slight >= 2) break;
        } else {
            slight = 0;
        }
        if (int(us.size()) >= most) return false;
        // The next row: where this column's residual is largest, of the rows
        // not taken yet.
        int next = -1;
        double best = -1;
        for (int i = 0; i < m; ++i)
            if (!used[std::size_t(i)] && std::abs(c[i]) > best) {
                best = std::abs(c[i]);
                next = i;
            }
        if (next < 0) break;
        row = next;
    }
    const int k = int(us.size());
    Eigen::MatrixXcd a(m, k), b(n, k);
    for (int l = 0; l < k; ++l) {
        a.col(l) = us[std::size_t(l)];
        b.col(l) = vs[std::size_t(l)];
    }
    if (k <= 1) {
        *u = a;
        *v = b;
        return true;
    }
    // Recompressed: A = Qa Ra, B = Qb Rb, so the block is Qa (Ra Rb^T) Qb^T,
    // and the small core's singular values say how many of its outer
    // products the tolerance wants.
    const Eigen::HouseholderQR<Eigen::MatrixXcd> qa(a), qb(b);
    const int ka = std::min(m, k), kb = std::min(n, k);
    const Eigen::MatrixXcd ra = qa.matrixQR().topRows(ka).triangularView<Eigen::Upper>();
    const Eigen::MatrixXcd rb = qb.matrixQR().topRows(kb).triangularView<Eigen::Upper>();
    const Eigen::JacobiSVD<Eigen::MatrixXcd> svd(ra * rb.transpose(), Eigen::ComputeThinU | Eigen::ComputeThinV);
    const Eigen::VectorXd& s = svd.singularValues();
    const double total = s.squaredNorm();
    double tail = 0;
    int keep = int(s.size());
    while (keep > 1 && tail + s[keep - 1] * s[keep - 1] <= tolerance * tolerance * total) {
        tail += s[keep - 1] * s[keep - 1];
        --keep;
    }
    const Eigen::MatrixXcd thinA = qa.householderQ() * Eigen::MatrixXcd::Identity(m, ka);
    const Eigen::MatrixXcd thinB = qb.householderQ() * Eigen::MatrixXcd::Identity(n, kb);
    *u = thinA * (svd.matrixU().leftCols(keep) * s.head(keep).cast<Complex>().asDiagonal());
    *v = thinB * svd.matrixV().leftCols(keep).conjugate();
    return true;
}

Gmres gmres(const Operator& apply, const Operator& precondition, const VectorC& b, VectorC& x, double tolerance,
            int restart, int most, const std::atomic_bool* stop, const std::function<void(int, double)>& progress)
{
    Gmres out;
    const int n = int(b.size());
    if (x.size() != n) x = VectorC::Zero(n);
    const double scale = b.norm();
    if (!(scale > 0)) {
        x.setZero();
        out.converged = true;
        return out;
    }
    restart = std::max(1, std::min(restart, n));
    Eigen::MatrixXcd basis(n, restart + 1);
    Eigen::MatrixXcd h = Eigen::MatrixXcd::Zero(restart + 1, restart);
    std::vector<double> cs(static_cast<std::size_t>(restart));
    std::vector<Complex> sn(static_cast<std::size_t>(restart));
    VectorC g(restart + 1), w(n), z(n), r(n);
    const auto preconditioned = [&precondition](const VectorC& in, VectorC& result) {
        if (precondition)
            precondition(in, result);
        else
            result = in;
    };
    while (true) {
        // The residual of the system itself, at each restart.
        w.setZero();
        apply(x, w);
        r = b - w;
        const double beta = r.norm();
        out.residual = beta / scale;
        if (out.residual <= tolerance) {
            out.converged = true;
            return out;
        }
        if (out.iterations >= most) return out;
        if (stop != nullptr && stop->load()) {
            out.stopped = true;
            return out;
        }
        basis.col(0) = r / beta;
        g.setZero();
        g[0] = beta;
        h.setZero();
        int k = 0;
        while (k < restart && out.iterations < most) {
            ++out.iterations;
            preconditioned(basis.col(k), z);
            w.setZero();
            apply(z, w);
            // Orthogonal to the basis, twice over: once is not enough where
            // the system is as ill conditioned as a low frequency's.
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i <= k; ++i) {
                    const Complex d = basis.col(i).dot(w);
                    h(i, k) += d;
                    w -= d * basis.col(i);
                }
            const double next = w.norm();
            h(k + 1, k) = next;
            if (next > 0) basis.col(k + 1) = w / next;
            // The column's earlier rotations, then its own, which leaves the
            // residual's norm in g's next term.
            for (int i = 0; i < k; ++i) {
                const Complex t = cs[std::size_t(i)] * h(i, k) + sn[std::size_t(i)] * h(i + 1, k);
                h(i + 1, k) = -std::conj(sn[std::size_t(i)]) * h(i, k) + cs[std::size_t(i)] * h(i + 1, k);
                h(i, k) = t;
            }
            const Complex a = h(k, k);
            const double below = std::abs(h(k + 1, k)), above = std::abs(a);
            if (below == 0) {
                cs[std::size_t(k)] = 1;
                sn[std::size_t(k)] = 0;
            } else if (above == 0) {
                cs[std::size_t(k)] = 0;
                sn[std::size_t(k)] = std::conj(h(k + 1, k)) / below;
            } else {
                const double t = std::hypot(above, below);
                cs[std::size_t(k)] = above / t;
                sn[std::size_t(k)] = a / above * std::conj(h(k + 1, k)) / t;
            }
            h(k, k) = cs[std::size_t(k)] * a + sn[std::size_t(k)] * h(k + 1, k);
            h(k + 1, k) = 0;
            g[k + 1] = -std::conj(sn[std::size_t(k)]) * g[k];
            g[k] = cs[std::size_t(k)] * g[k];
            ++k;
            out.residual = std::abs(g[k]) / scale;
            if (progress) progress(out.iterations, out.residual);
            if (out.residual <= tolerance || next == 0 || (stop != nullptr && stop->load())) break;
        }
        // The least squares of the Hessenberg matrix, by its triangle.
        const VectorC y = h.topLeftCorner(k, k).triangularView<Eigen::Upper>().solve(g.head(k));
        preconditioned(basis.leftCols(k) * y, z);
        x += z;
    }
}

double physicalMemory()
{
#if defined(__APPLE__)
    std::int64_t bytes = 0;
    std::size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 && bytes > 0) return double(bytes);
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) return double(status.ullTotalPhys);
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? double(pages) * double(size) : 0;
#endif
}

/*!
 * The pairs of \a a and \a b whose boxes are near: no further apart than
 * \a reach times the longer of the two's longest sides in the plane. Of
 * one list (\a b null), each pair once, the first no later than the
 * second. Found by bins in the plane as wide as the widest box.
 */
QList<QPair<int, int>> nearPairs(const QList<Box>& a, const QList<Box>* b, double reach)
{
    const QList<Box>& other = b != nullptr ? *b : a;
    QList<QPair<int, int>> out;
    if (a.isEmpty() || other.isEmpty()) return out;
    const auto size = [](const Box& x) { return std::max(x.side(0), x.side(1)); };
    double widest = 0, lo[2] = {a.first().lo[0], a.first().lo[1]};
    for (const QList<Box>* list : {&a, &other})
        for (const Box& x : *list) {
            widest = std::max(widest, size(x));
            lo[0] = std::min(lo[0], x.lo[0]);
            lo[1] = std::min(lo[1], x.lo[1]);
        }
    const double bin = widest > 0 ? widest : 1.0, grow = reach * widest;
    const auto binOf = [&lo, bin](double v, int axis) { return qint64(std::floor((v - lo[axis]) / bin)); };
    const auto key = [](qint64 i, qint64 j) { return (i << 32) ^ (j & 0xffffffffLL); };
    QHash<qint64, QList<int>> bins;
    for (int j = 0; j < other.size(); ++j) {
        const Box& x = other.at(j);
        for (qint64 i0 = binOf(x.lo[0], 0); i0 <= binOf(x.hi[0], 0); ++i0)
            for (qint64 i1 = binOf(x.lo[1], 1); i1 <= binOf(x.hi[1], 1); ++i1) bins[key(i0, i1)] << j;
    }
    std::vector<int> seen(std::size_t(other.size()), -1);
    for (int i = 0; i < a.size(); ++i) {
        const Box& x = a.at(i);
        for (qint64 i0 = binOf(x.lo[0] - grow, 0); i0 <= binOf(x.hi[0] + grow, 0); ++i0)
            for (qint64 i1 = binOf(x.lo[1] - grow, 1); i1 <= binOf(x.hi[1] + grow, 1); ++i1) {
                const auto found = bins.constFind(key(i0, i1));
                if (found == bins.constEnd()) continue;
                for (const int j : found.value()) {
                    if (seen[std::size_t(j)] == i) continue;
                    seen[std::size_t(j)] = i;
                    if (b == nullptr && j < i) continue;
                    if (x.distance(other.at(j)) <= reach * std::max(size(x), size(other.at(j)))) out << qMakePair(i, j);
                }
            }
    }
    return out;
}


} // namespace qucs_s::rf25
