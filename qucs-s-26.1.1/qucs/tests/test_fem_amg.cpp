/*
 * test_fem_amg.cpp - the multiphysics solver's algebraic multigrid
 *                    (multiphysics/fem_amg.h): a 3D Laplacian solved to its
 *                    tolerance, as a direct solve has it, in iterations that
 *                    hardly grow as the grid is refined - copper beside oxide
 *                    too -; from a guess; and where it cannot converge, the
 *                    incomplete Cholesky's conjugate gradients, said
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_amg.h"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <QtTest>

#include <array>
#include <cmath>

using namespace qucs_s::fem;

namespace {

/// The P1 Laplacian of an n x n x n grid of unit-cube cells, each cut into
/// Kuhn's six tetrahedra, its bottom held (the proposal's benchmark), with a
/// unit source; \a contrast: a block of k = 400 in k = 1.4.
void laplacian(int n, bool contrast, Eigen::SparseMatrix<double>& A, Eigen::VectorXd& b)
{
    const int N = n + 1;
    auto node = [N](int i, int j, int k) { return (k * N + j) * N + i; };
    std::vector<int> unk(std::size_t(N) * N * N, -1);
    int nu = 0;
    for (int k = 1; k < N; ++k)
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) unk[std::size_t(node(i, j, k))] = nu++;
    const double h = 1.0 / n;
    const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    std::vector<Eigen::Triplet<double>> t;
    b = Eigen::VectorXd::Zero(nu);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const double cx = (i + 0.5) * h, cy = (j + 0.5) * h, cz = (k + 0.5) * h;
                const double kc = contrast ? ((cx > 0.3 && cx < 0.7 && cy > 0.3 && cy < 0.7 && cz > 0.4) ? 400.0 : 1.4) : 1.0;
                for (const auto& p : perm) {
                    std::array<std::array<int, 3>, 4> v{};
                    for (int s = 0; s < 3; ++s) {
                        v[std::size_t(s + 1)] = v[std::size_t(s)];
                        v[std::size_t(s + 1)][std::size_t(p[s])] += 1;
                    }
                    Eigen::Matrix3d J;
                    for (int c = 0; c < 3; ++c)
                        for (int r = 0; r < 3; ++r) J(r, c) = (v[std::size_t(c + 1)][std::size_t(r)] - v[0][std::size_t(r)]) * h;
                    const double vol = std::abs(J.determinant()) / 6;
                    const Eigen::Matrix3d G = J.inverse().transpose();
                    Eigen::Vector3d g[4] = {-(G.col(0) + G.col(1) + G.col(2)), G.col(0), G.col(1), G.col(2)};
                    int id[4];
                    for (int a = 0; a < 4; ++a) id[a] = unk[std::size_t(node(i + v[std::size_t(a)][0], j + v[std::size_t(a)][1], k + v[std::size_t(a)][2]))];
                    for (int a = 0; a < 4; ++a) {
                        if (id[a] < 0) continue;
                        b(id[a]) += vol / 4;
                        for (int c = 0; c < 4; ++c)
                            if (id[c] >= 0) t.emplace_back(id[a], id[c], kc * vol * g[a].dot(g[c]));
                    }
                }
            }
    A.resize(nu, nu);
    A.setFromTriplets(t.begin(), t.end());
    A.makeCompressed();
}

} // namespace

class TestFemAmg : public QObject
{
    Q_OBJECT

private slots:
    // To its tolerance and as a direct solve has it; its iterations nearly
    // the same at twice the divisions (an incomplete Cholesky's grow about
    // half again); copper beside oxide alike. (Past AMGCL's 3000 unknowns:
    // below them its coarsest level, solved directly, is the whole.)
    void solvesALaplacian()
    {
        for (bool contrast : {false, true}) {
            int iterations[2] = {0, 0};
            for (int i = 0; i < 2; ++i) {
                Eigen::SparseMatrix<double> A;
                Eigen::VectorXd b;
                laplacian(i == 0 ? 16 : 32, contrast, A, b);
                AmgSolver amg;
                amg.setTolerance(1e-10);
                QString why;
                QVERIFY2(amg.compute(A, &why), qPrintable(why));
                QVERIFY(amg.bytes() > 0);
                QCOMPARE(amg.size(), int(A.rows()));
                Eigen::VectorXd x = Eigen::VectorXd::Zero(A.rows());
                IterativeReport r;
                QVERIFY2(amg.solve(b, x, &r, &why), qPrintable(why));
                QVERIFY(r.converged && !r.fallback);
                QVERIFY((b - A * x).norm() <= 1.0001e-10 * b.norm());
                iterations[i] = r.iterations;
                if (i > 0) continue;
                // (A direct solve's answer at the smaller size: Eigen's LDLT
                // of a 3D matrix is slow, sanitized all the more.)
                Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> direct(A);
                const Eigen::VectorXd y = direct.solve(b);
                QVERIFY((x - y).norm() <= 1e-7 * y.norm());
                // From the answer as its guess: at once, or nearly.
                IterativeReport again;
                Eigen::VectorXd z = y;
                QVERIFY(amg.solve(b, z, &again, &why));
                QVERIFY2(again.iterations <= 3 && again.iterations < r.iterations, qPrintable(QString::number(again.iterations)));
            }
            QVERIFY2(iterations[0] > 3 && iterations[1] <= iterations[0] + 6 && iterations[1] < 40,
                     qPrintable(QStringLiteral("%1, then %2 iterations").arg(iterations[0]).arg(iterations[1])));
        }
    }

    // Too few iterations for multigrid: the incomplete Cholesky's conjugate
    // gradients take over (ten times as many), said; too few for both, a
    // failure and why.
    void fallsBack()
    {
        Eigen::SparseMatrix<double> A;
        Eigen::VectorXd b;
        laplacian(20, false, A, b);
        AmgSolver amg;
        amg.setTolerance(1e-10);
        amg.setMaxIterations(2);
        QString why;
        QVERIFY(amg.compute(A, &why));
        Eigen::VectorXd x = Eigen::VectorXd::Zero(A.rows());
        IterativeReport r;
        const bool ok = amg.solve(b, x, &r, &why);
        QVERIFY(r.fallback);
        QVERIFY(!ok);
        QVERIFY2(why.contains(QStringLiteral("did not converge")), qPrintable(why));
        // An empty system and a zero right-hand side: nothing to do.
        AmgSolver none;
        QVERIFY(none.compute(Eigen::SparseMatrix<double>(0, 0), &why));
        Eigen::VectorXd e;
        QVERIFY(none.solve(Eigen::VectorXd(), e, &r, &why));
        Eigen::VectorXd zero = Eigen::VectorXd::Ones(A.rows());
        QVERIFY(amg.solve(Eigen::VectorXd::Zero(A.rows()), zero, &r, &why));
        QVERIFY(zero.norm() == 0);
    }
};

QTEST_GUILESS_MAIN(TestFemAmg)
#include "test_fem_amg.moc"
