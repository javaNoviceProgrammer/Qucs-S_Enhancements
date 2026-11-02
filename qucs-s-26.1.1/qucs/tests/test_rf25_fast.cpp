/*
 * test_rf25_fast.cpp - a matrix of a method of moments held compressed and
 * solved by iteration (rf25/rf25_fast.h), held to the dense matrix it
 * stands for: the cluster tree's halves; a block of two clusters apart as
 * its cross approximation to the tolerance asked, of low rank, nought where
 * it is nought; the H-matrix's products, both ways round and on several
 * threads, the dense matrix's to its tolerance in less memory; and GMRES
 * to its residual, preconditioned or not, restarted, and stopped.
 *
 * Phase E of docs/proposals/2026-10-23-rf-2p5d-moment-method.md:
 * "acceleration: ACA ... GMRES with a near-field preconditioner".
 */
#include <QtTest>

#include "rf25/rf25_fast.h"

#include <Eigen/Dense>

#include <atomic>
#include <cmath>
#include <complex>
#include <set>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

const Complex kJ(0, 1);

/// \a n points of a square \a side wide on a grid at height \a z, each a
/// little box: what a planar mesh's cells are to the tree.
QList<rf::Box> grid(int n, double side, double z, double x0 = 0, double y0 = 0)
{
    QList<rf::Box> out;
    const double step = side / n;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const double x = x0 + (i + 0.5) * step, y = y0 + (j + 0.5) * step;
            out << rf::Box::of(x - step / 2, y - step / 2, z, x + step / 2, y + step / 2, z);
        }
    return out;
}

/// The retarded kernel between two boxes' centres, exp(-jkR)/R, smoothed
/// within a box of itself so that it stays finite.
Complex kernel(const rf::Box& a, const rf::Box& b, double k)
{
    const double dx = a.centre(0) - b.centre(0), dy = a.centre(1) - b.centre(1), dz = a.centre(2) - b.centre(2);
    const double r = std::sqrt(dx * dx + dy * dy + dz * dz + 0.25 * a.side(0) * b.side(0));
    return std::exp(-kJ * k * r) / r;
}

Eigen::MatrixXcd dense(const QList<rf::Box>& rows, const QList<rf::Box>& columns, double k)
{
    Eigen::MatrixXcd m(rows.size(), columns.size());
    for (int i = 0; i < rows.size(); ++i)
        for (int j = 0; j < columns.size(); ++j) m(i, j) = kernel(rows.at(i), columns.at(j), k);
    return m;
}

Eigen::VectorXcd randomVector(int n, unsigned seed)
{
    std::srand(seed);
    return Eigen::VectorXcd::Random(n);
}

} // namespace

class TestRf25Fast : public QObject
{
    Q_OBJECT

private slots:
    // The tree: every item once, each node its children's two halves, its
    // box all of its items', leaves of the size asked or less; and a row of
    // points along x halved along x.
    void aClusterTreeHalvesItsItemsAlongTheirLongestSide()
    {
        const QList<rf::Box> items = grid(20, 1e-3, 0) + grid(10, 1e-3, 2e-4);
        const rf::ClusterTree tree(items, 16);
        QCOMPARE(tree.items(), int(items.size()));
        const std::vector<int>& order = tree.order();
        QCOMPARE(std::set<int>(order.begin(), order.end()).size(), std::size_t(items.size()));
        int leaves = 0;
        for (const rf::ClusterTree::Node& node : tree.nodes()) {
            for (int p = node.begin; p < node.end; ++p) {
                const rf::Box& b = items.at(order[std::size_t(p)]);
                for (int a = 0; a < 3; ++a) QVERIFY(node.box.lo[a] <= b.lo[a] && node.box.hi[a] >= b.hi[a]);
            }
            if (node.leaf()) {
                ++leaves;
                QVERIFY2(node.size() <= 16, qPrintable(QString::number(node.size())));
                continue;
            }
            const rf::ClusterTree::Node& left = tree.nodes()[std::size_t(node.child[0])];
            const rf::ClusterTree::Node& right = tree.nodes()[std::size_t(node.child[1])];
            QCOMPARE(left.begin, node.begin);
            QCOMPARE(left.end, right.begin);
            QCOMPARE(right.end, node.end);
            QVERIFY(std::abs(left.size() - right.size()) <= 1);
        }
        QVERIFY(leaves >= int(items.size()) / 16);
        QList<rf::Box> row;
        for (int i = 0; i < 64; ++i) row << rf::Box::of(i, 0, 0, i + 1, 1, 0);
        const rf::ClusterTree line(row, 8);
        const rf::ClusterTree::Node& first = line.nodes()[std::size_t(line.nodes().front().child[0])];
        QVERIFY2(first.box.hi[0] <= 32 + 1e-12, qPrintable(QString::number(first.box.hi[0])));
        // Items all at one place are one leaf however many.
        const rf::ClusterTree pile(QList<rf::Box>(40, rf::Box::of(0, 0, 0, 1, 1, 0)), 8);
        QCOMPARE(int(pile.nodes().size()), 1);
    }

    // Two clusters apart: the block of their kernel as its cross
    // approximation to the tolerance, the rank growing as the tolerance
    // tightens and staying far below the block's own; a block of nought as
    // no outer product at all; and a rank that would hold no less than the
    // block said so.
    void aCrossApproximationIsItsBlockToTheTolerance()
    {
        const QList<rf::Box> a = grid(12, 1e-3, 0), b = grid(12, 1e-3, 1e-4, 2.5e-3, 0.5e-3);
        const double k = 2 * M_PI / 10e-3;
        const Eigen::MatrixXcd exact = dense(a, b, k);
        const rf::HMatrix::Entry entry = [&a, &b, k](int i, int j) { return kernel(a.at(i), b.at(j), k); };
        std::vector<int> rows(a.size()), columns(b.size());
        for (int i = 0; i < a.size(); ++i) rows[std::size_t(i)] = i;
        for (int j = 0; j < b.size(); ++j) columns[std::size_t(j)] = j;
        int previous = 0;
        for (const double tolerance : {1e-3, 1e-6, 1e-9}) {
            Eigen::MatrixXcd u, v;
            QVERIFY(rf::crossApproximation(entry, rows.data(), int(rows.size()), columns.data(), int(columns.size()), tolerance,
                                           72, &u, &v));
            const double error = (exact - u * v.transpose()).norm() / exact.norm();
            qWarning("tolerance %g: rank %d, error %.3g", tolerance, int(u.cols()), error);
            QVERIFY2(error < 20 * tolerance, qPrintable(QStringLiteral("%1 at %2").arg(error).arg(tolerance)));
            QVERIFY(int(u.cols()) >= previous && int(u.cols()) < 40);
            previous = int(u.cols());
        }
        Eigen::MatrixXcd u, v;
        const rf::HMatrix::Entry nought = [](int, int) { return Complex(0, 0); };
        QVERIFY(rf::crossApproximation(nought, rows.data(), int(rows.size()), columns.data(), int(columns.size()), 1e-6, 72, &u,
                                       &v));
        QCOMPARE(int(u.cols()), 0);
        QVERIFY(!rf::crossApproximation(entry, rows.data(), int(rows.size()), columns.data(), int(columns.size()), 1e-12, 3, &u,
                                        &v));
    }

    // The whole matrix: a planar mesh's cells at two heights, the
    // symmetric H-matrix's product the dense one's to its tolerance, in a
    // part of the dense matrix's memory; the same on four threads; and a
    // matrix between two sets, both ways round.
    void anHMatrixMultipliesAsItsDenseMatrixDoes()
    {
        const QList<rf::Box> cells = grid(24, 2e-3, 0) + grid(12, 1e-3, 1e-4, 0.5e-3, 0.5e-3);
        const double k = 2 * M_PI / 20e-3;
        const rf::ClusterTree tree(cells, 32);
        rf::HMatrix h(&tree, &tree, true, 1.5);
        QVERIFY(h.lowRankBlocks() > 0);
        QVERIFY(h.denseBlockBytes() < h.denseBytes());
        QVERIFY(h.fill([&cells, k](int i, int j) { return kernel(cells.at(i), cells.at(j), k); }, 1e-6));
        const Eigen::MatrixXcd exact = dense(cells, cells, k);
        const Eigen::VectorXcd x = randomVector(int(cells.size()), 7);
        Eigen::VectorXcd y = Eigen::VectorXcd::Zero(cells.size()), t = Eigen::VectorXcd::Zero(cells.size());
        h.multiply(x, y);
        h.multiplyTransposed(x, t);
        const Eigen::VectorXcd want = exact * x;
        const double error = (y - want).norm() / want.norm();
        qWarning("%d cells: %.0f of %.0f kB, rank %d (mean %.1f), error %.3g", int(cells.size()), h.bytes() / 1024,
                 h.denseBytes() / 1024, h.largestRank(), h.meanRank(), error);
        QVERIFY2(error < 1e-5, qPrintable(QString::number(error)));
        QVERIFY2((t - y).norm() < 1e-14 * y.norm(), "a symmetric matrix's transpose is itself");
        // A larger grid, the far blocks most of it: a part of the dense
        // matrix's memory (its product taken row by row, the dense matrix
        // never made).
        const QList<rf::Box> many = grid(56, 4e-3, 0);
        const rf::ClusterTree large(many, 32);
        rf::HMatrix big(&large, &large, true, 1.5);
        QVERIFY(big.fill([&many, k](int i, int j) { return kernel(many.at(i), many.at(j), k); }, 1e-6, 4));
        const Eigen::VectorXcd xb = randomVector(int(many.size()), 19);
        Eigen::VectorXcd yb = Eigen::VectorXcd::Zero(many.size()), wb(many.size());
        big.multiply(xb, yb);
        for (int i = 0; i < many.size(); ++i) {
            Complex sum(0, 0);
            for (int j = 0; j < many.size(); ++j) sum += kernel(many.at(i), many.at(j), k) * xb[j];
            wb[i] = sum;
        }
        qWarning("%d cells: %.0f of %.0f kB, rank %d (mean %.1f), error %.3g", int(many.size()), big.bytes() / 1024,
                 big.denseBytes() / 1024, big.largestRank(), big.meanRank(), (yb - wb).norm() / wb.norm());
        QVERIFY2((yb - wb).norm() < 1e-5 * wb.norm(), qPrintable(QString::number((yb - wb).norm() / wb.norm())));
        QVERIFY2(big.bytes() < 0.35 * big.denseBytes(), qPrintable(QStringLiteral("%1 of %2").arg(big.bytes()).arg(big.denseBytes())));
        // scale, and += onto what is there
        Eigen::VectorXcd twice = y;
        h.multiply(x, twice, Complex(0, 2));
        QVERIFY((twice - (y + Complex(0, 2) * y)).norm() < 1e-12 * y.norm());
        // On four threads, the same to rounding.
        rf::HMatrix four(&tree, &tree, true, 1.5);
        QVERIFY(four.fill([&cells, k](int i, int j) { return kernel(cells.at(i), cells.at(j), k); }, 1e-6, 4));
        QCOMPARE(four.threads(), 4);
        Eigen::VectorXcd y4 = Eigen::VectorXcd::Zero(cells.size());
        four.multiply(x, y4);
        QVERIFY2((y4 - y).norm() < 1e-12 * y.norm(), qPrintable(QString::number((y4 - y).norm() / y.norm())));

        // Between two sets: the cells and a row of vias over them.
        QList<rf::Box> vias;
        for (int i = 0; i < 40; ++i) vias << rf::Box::of(i * 5e-5, 2.1e-3, 0, i * 5e-5 + 2e-5, 2.12e-3, 1e-4);
        const rf::ClusterTree viaTree(vias, 8);
        rf::HMatrix phi(&tree, &viaTree, false, 1.5);
        QVERIFY(phi.fill([&cells, &vias, k](int i, int j) { return kernel(cells.at(i), vias.at(j), k); }, 1e-6, 2));
        const Eigen::MatrixXcd across = dense(cells, vias, k);
        const Eigen::VectorXcd xv = randomVector(int(vias.size()), 3), xc = randomVector(int(cells.size()), 5);
        Eigen::VectorXcd yc = Eigen::VectorXcd::Zero(cells.size()), yv = Eigen::VectorXcd::Zero(vias.size());
        phi.multiply(xv, yc);
        phi.multiplyTransposed(xc, yv);
        QVERIFY2((yc - across * xv).norm() < 1e-5 * (across * xv).norm(), "the product");
        QVERIFY2((yv - across.transpose() * xc).norm() < 1e-5 * (across.transpose() * xc).norm(), "its transpose's");
        // Stopped before it is filled: false.
        std::atomic_bool stop{true};
        rf::HMatrix stopped(&tree, &tree, true, 1.5);
        QVERIFY(!stopped.fill([&cells, k](int i, int j) { return kernel(cells.at(i), cells.at(j), k); }, 1e-6, 1, &stop));
    }

    // GMRES: a complex system to its residual; in one step with the matrix
    // its own preconditioner; restarted every five vectors still there; a
    // right-hand side of nought, nought; stopped by its flag.
    void gmresSolvesToItsResidual()
    {
        const int n = 300;
        std::srand(11);
        Eigen::MatrixXcd a = Eigen::MatrixXcd::Random(n, n) / std::sqrt(double(n));
        a += Eigen::MatrixXcd::Identity(n, n) * Complex(2, 1);
        const Eigen::VectorXcd b = randomVector(n, 13);
        const rf::Operator apply = [&a](const Eigen::VectorXcd& x, Eigen::VectorXcd& y) { y = a * x; };
        Eigen::VectorXcd x;
        const rf::Gmres plain = rf::gmres(apply, {}, b, x, 1e-10, 100, 1000);
        QVERIFY(plain.converged);
        QVERIFY2((b - a * x).norm() <= 1.0001e-10 * b.norm(), qPrintable(QString::number((b - a * x).norm() / b.norm())));
        QVERIFY(plain.iterations > 5);
        const Eigen::PartialPivLU<Eigen::MatrixXcd> lu(a);
        const rf::Operator exact = [&lu](const Eigen::VectorXcd& v, Eigen::VectorXcd& y) { y = lu.solve(v); };
        Eigen::VectorXcd xe;
        const rf::Gmres preconditioned = rf::gmres(apply, exact, b, xe, 1e-10, 100, 1000);
        QVERIFY(preconditioned.converged);
        QVERIFY2(preconditioned.iterations <= 2, qPrintable(QString::number(preconditioned.iterations)));
        Eigen::VectorXcd xr;
        const rf::Gmres restarted = rf::gmres(apply, {}, b, xr, 1e-10, 5, 3000);
        QVERIFY(restarted.converged);
        QVERIFY((xr - x).norm() < 1e-8 * x.norm());
        Eigen::VectorXcd x0 = randomVector(n, 17);
        const rf::Gmres nothing = rf::gmres(apply, {}, Eigen::VectorXcd::Zero(n), x0, 1e-10, 100, 1000);
        QVERIFY(nothing.converged);
        QCOMPARE(x0.norm(), 0.0);
        std::atomic_bool stop{true};
        Eigen::VectorXcd xs;
        const rf::Gmres stopped = rf::gmres(apply, {}, b, xs, 1e-10, 100, 1000, &stop);
        QVERIFY(stopped.stopped && !stopped.converged);
        // Short of its iterations: not converged, and the residual it left.
        Eigen::VectorXcd xf;
        const rf::Gmres few = rf::gmres(apply, {}, b, xf, 1e-14, 100, 3);
        QVERIFY(!few.converged && few.iterations == 3 && few.residual > 1e-14);
    }

    void theMachinesMemoryIsKnown()
    {
        QVERIFY2(rf::physicalMemory() > 1e9, qPrintable(QString::number(rf::physicalMemory())));
    }
};

QTEST_MAIN(TestRf25Fast)
#include "test_rf25_fast.moc"
