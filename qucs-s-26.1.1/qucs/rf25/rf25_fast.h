/*
 * rf25_fast.h - a dense matrix of a method of moments held compressed, and
 *               solved by iteration: what lifts the full-wave solve past
 *               the few thousand unknowns a dense matrix holds
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Phase E of docs/proposals/2026-10-23-rf-2p5d-moment-method.md:
 * "acceleration: ACA or the adaptive integral method on a grid, GMRES with
 * a near-field preconditioner, the memory bound lifted from a few thousand
 * unknowns to a few hundred thousand".
 *
 * The matrices of the solve - the runs' partial inductances, the cells'
 * potential coefficients, the vias' couplings - are each a kernel between
 * places: smooth wherever the two are apart beside their own size, and
 * singular only where they meet. So:
 *
 *  - The places are split, again and again, into halves along the longest
 *    side of what they cover, down to clusters of a few dozen: a cluster
 *    tree (ClusterTree).
 *  - A pair of clusters further apart than the smaller is wide sees the
 *    kernel smooth across it, and its block of the matrix is of low rank:
 *    a sum of a few outer products, found by adaptive cross approximation
 *    (Bebendorf, Numer. Math. 86 (2000) 565) - a row and a column of the
 *    block at a time, each where the last left the most out, until the
 *    next adds less than the tolerance of what is held - and then cut to
 *    the fewest that keep the tolerance (a QR of each side and an SVD of
 *    the small core between them). Only those rows and columns of the
 *    kernel are computed, never the block.
 *  - The pairs that are near are kept dense, as the matrix had them.
 *
 * That is an H-matrix (Hackbusch, Computing 62 (1999) 89), here of one
 * level of blocks (HMatrix): its memory and the work of multiplying by it
 * grow as N log N where the dense matrix's grow as N^2. It is
 * kernel-independent - it needs the entries and nothing else - so the
 * stack's layered Green's functions, the closed forms of the near pairs and
 * the vias' corrections are all as the dense solve has them, and as the
 * tolerance goes to nought the answer is the dense solve's.
 *
 * A compressed matrix cannot be factorized, so the system is solved by
 * GMRES (Saad and Schultz, SIAM J. Sci. Stat. Comput. 7 (1986) 856): the
 * answer is built from the matrix's own products, restarted to bound its
 * memory, and preconditioned on the right so that its residual is the
 * system's own. The preconditioner is the caller's.
 */
#ifndef QUCS_RF25_FAST_H
#define QUCS_RF25_FAST_H

#include <Eigen/Dense>

#include <QList>

#include <atomic>
#include <complex>
#include <functional>
#include <vector>

namespace qucs_s::rf25 {

using Complex = std::complex<double>;
using VectorC = Eigen::VectorXcd;

/// An axis-aligned box, m: what an unknown of the solve covers.
struct Box {
    double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    /// The box from one corner to the other, whichever way round.
    static Box of(double x0, double y0, double z0, double x1, double y1, double z1);
    Box united(const Box& other) const;
    double centre(int axis) const { return 0.5 * (lo[axis] + hi[axis]); }
    double side(int axis) const { return hi[axis] - lo[axis]; }
    /// Its diagonal.
    double diameter() const;
    /// The shortest distance between a point of this and a point of
    /// \a other: nought where they touch or overlap.
    double distance(const Box& other) const;
};

/*!
 * \a items split into a binary tree of clusters: each node the items of a
 * stretch of order(), its two children the halves of it either side of
 * the middle item along the longest side its items' centres span, down to
 * leaves of \a leaf items or fewer. A node's box is all of its items'
 * boxes together, which is what admissibility is judged on.
 */
class ClusterTree
{
public:
    struct Node {
        int begin = 0, end = 0;     ///< its items: order()[begin, end)
        int child[2] = {-1, -1};
        Box box;
        bool leaf() const { return child[0] < 0; }
        int size() const { return end - begin; }
    };
    ClusterTree() = default;
    ClusterTree(const QList<Box>& items, int leaf);
    const std::vector<Node>& nodes() const { return a_nodes; }
    /// Each position of the tree, the item there.
    const std::vector<int>& order() const { return a_order; }
    int items() const { return int(a_order.size()); }

private:
    int split(int begin, int end, const QList<Box>& items, int leaf);
    std::vector<Node> a_nodes;
    std::vector<int> a_order;
};

/*!
 * A matrix between the items of two cluster trees - or of one, held as its
 * upper blocks where it is symmetric - each pair of clusters a block: low
 * rank, U V^T, where the pair is admissible (the smaller cluster no wider
 * than eta times their distance), and dense where the pair is two leaves
 * that are not. Indices are the items', not the trees' positions.
 */
class HMatrix
{
public:
    /// The entry of row item \a row and column item \a column.
    using Entry = std::function<Complex(int row, int column)>;
    struct Block {
        int row = 0, column = 0;    ///< nodes of the two trees
        bool lowRank = false;
        Eigen::MatrixXcd dense;     ///< rows x columns, where not low rank
        Eigen::MatrixXcd u, v;      ///< rows x k and columns x k, where it is
    };
    HMatrix() = default;
    /// The blocks of \a rows against \a columns (both kept by the caller,
    /// as long as this is); \a symmetric, of one tree against itself, its
    /// upper blocks alone.
    HMatrix(const ClusterTree* rows, const ClusterTree* columns, bool symmetric, double eta);

    /*!
     * Each block filled from \a entry: a low-rank one by adaptive cross
     * approximation to \a tolerance of its own size (Frobenius), then
     * recompressed; one whose rank would hold no less than its dense self
     * filled dense. \a entry must be safe on \a threads threads at once, and
     * a symmetric matrix's must be symmetric. False, unfilled, where
     * \a stop is set on the way.
     */
    bool fill(const Entry& entry, double tolerance, int threads = 1, const std::atomic_bool* stop = nullptr);

    /// y += scale H x, x and y by item.
    void multiply(const VectorC& x, VectorC& y, Complex scale = 1.0) const;
    /// y += scale H^T x.
    void multiplyTransposed(const VectorC& x, VectorC& y, Complex scale = 1.0) const;

    int rows() const { return a_rows != nullptr ? a_rows->items() : 0; }
    int columns() const { return a_columns != nullptr ? a_columns->items() : 0; }
    bool symmetric() const { return a_symmetric; }
    const std::vector<Block>& blocks() const { return a_blocks; }
    /// What its blocks hold, bytes; and what the dense matrix would.
    double bytes() const;
    double denseBytes() const;
    /// Before filling: what its dense blocks will hold, bytes - the part
    /// of its memory known before a rank is found - and how many entries
    /// its low-rank blocks span (rows plus columns, each), for a guess at
    /// the rest.
    double denseBlockBytes() const;
    double lowRankSpan() const;
    int largestRank() const;
    /// The low-rank blocks' ranks, averaged by the rows and columns they
    /// span.
    double meanRank() const;
    int lowRankBlocks() const;
    /// How many threads multiply() shares its blocks over (fill()'s).
    int threads() const { return a_threads; }

private:
    void build(int row, int column);
    void apply(const VectorC& x, VectorC& y, Complex scale, bool transposed) const;
    const ClusterTree* a_rows = nullptr;
    const ClusterTree* a_columns = nullptr;
    bool a_symmetric = false;
    double a_eta = 1;
    int a_threads = 1;
    std::vector<Block> a_blocks;
};

/*!
 * An m x n block, rows \a rows and columns \a columns (items), as U V^T by
 * adaptive cross approximation with partial pivoting to \a tolerance, then
 * recompressed to the fewest outer products that keep it. False where the
 * rank reached \a most first (then the block is better dense). Public for
 * the tests.
 */
bool crossApproximation(const HMatrix::Entry& entry, const int* rows, int m, const int* columns, int n, double tolerance,
                        int most, Eigen::MatrixXcd* u, Eigen::MatrixXcd* v);

/// What GMRES did.
struct Gmres {
    int iterations = 0;         ///< products with the matrix, the restarts' own residuals not counted
    double residual = 0;        ///< |b - A x| / |b| at the end
    bool converged = false;     ///< to the tolerance asked
    bool stopped = false;       ///< by the flag
};

/// y = A x, of a vector to a vector.
using Operator = std::function<void(const VectorC& x, VectorC& y)>;

/*!
 * A x = b by restarted GMRES, preconditioned on the right by
 * \a precondition (y = M^-1 x; empty: none), from \a x as given to
 * |b - A x| <= \a tolerance |b|, \a restart vectors at a time and
 * \a most products in all. Orthogonalized twice by modified Gram-Schmidt;
 * the residual at each restart the system's own, not the recurrence's.
 */
Gmres gmres(const Operator& apply, const Operator& precondition, const VectorC& b, VectorC& x, double tolerance,
            int restart, int most, const std::atomic_bool* stop = nullptr);

/// The machine's memory, bytes; nought where it cannot be told.
double physicalMemory();

} // namespace qucs_s::rf25

#endif
