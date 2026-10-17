/*
 * The multiphysics solver's sparse direct factorizations (fem_sparse.h):
 * Accelerate's LU and Cholesky where there are (macOS 15.5), Eigen's
 * elsewhere, below a size and when asked - each solving what Eigen's does,
 * a pattern changing between factorizations, a singular matrix said.
 */
#include "fem_sparse.h"

#include <QtTest>

#include <complex>
#include <vector>

using namespace qucs_s::fem;

namespace {

/// A convection-diffusion on an m by m grid, two unknowns a node coupled:
/// unsymmetric, as the semiconductors' are; \a shift on its diagonal.
template <typename Scalar> Eigen::SparseMatrix<Scalar> grid(int m, Scalar shift = Scalar(0), int extra = 0)
{
    const int n = 2 * m * m;
    std::vector<Eigen::Triplet<Scalar>> t;
    auto id = [m](int i, int j) { return i * m + j; };
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < m; ++j)
            for (int c = 0; c < 2; ++c) {
                const int r = 2 * id(i, j) + c;
                t.emplace_back(r, r, Scalar(4.2 + c) + shift);
                t.emplace_back(r, 2 * id(i, j) + 1 - c, Scalar(0.3));
                if (i > 0) t.emplace_back(r, 2 * id(i - 1, j) + c, Scalar(-1.3));
                if (i < m - 1) t.emplace_back(r, 2 * id(i + 1, j) + c, Scalar(-0.7));
                if (j > 0) t.emplace_back(r, 2 * id(i, j - 1) + c, Scalar(-1.1));
                if (j < m - 1) t.emplace_back(r, 2 * id(i, j + 1) + c, Scalar(-0.9));
            }
    // (An entry the other's pattern has not: a far coupling, one way or the
    // other.)
    if (extra == 1) t.emplace_back(0, n - 1, Scalar(0.25));
    if (extra == 2) t.emplace_back(n - 1, 0, Scalar(0.25));
    Eigen::SparseMatrix<Scalar> a(n, n);
    a.setFromTriplets(t.begin(), t.end());
    a.makeCompressed();
    return a;
}

template <typename Scalar> double residual(const Eigen::SparseMatrix<Scalar>& a, const Eigen::Matrix<Scalar, Eigen::Dynamic, 1>& x,
                                           const Eigen::Matrix<Scalar, Eigen::Dynamic, 1>& b)
{
    return (a * x - b).norm() / b.norm();
}

} // namespace

class TestFemSparse : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        qunsetenv("QUCS_FEM_SOLVER");
        qunsetenv("QUCS_FEM_ACCELERATE_FROM");
    }
    void cleanupTestCase() { init(); }

    // Its LU on each backend: a real and a complex system solved to 1e-12;
    // Accelerate's where there is one (macOS 15.5) from the size it is
    // asked from - 20 000 unknowns by default, Eigen's below -, Eigen's
    // when QUCS_FEM_SOLVER=eigen.
    void theLuOnEachBackend()
    {
        qInfo("Accelerate's LU here: %s (the direct solver: %s)", acceleratedLu() ? "yes" : "no", qPrintable(directSolverName()));
        // (By default from 20 000 unknowns: 3200 Eigen's.)
        QCOMPARE(acceleratedFrom(), 20000);
        {
            SparseLu<double> lu;
            lu.compute(grid<double>(40));
            QVERIFY(!lu.accelerated());
        }
        for (const char* from : {"0", "100000", "eigen"}) {
            init();
            if (QByteArray(from) == "eigen") {
                qputenv("QUCS_FEM_SOLVER", "eigen");
                qputenv("QUCS_FEM_ACCELERATE_FROM", "0");
            } else {
                qputenv("QUCS_FEM_ACCELERATE_FROM", from);
            }
            const bool accelerate = acceleratedLu() && QByteArray(from) == "0";
            {
                const auto a = grid<double>(40);
                SparseLu<double> lu;
                lu.analyzePattern(a);
                lu.factorize(a);
                QCOMPARE(lu.info(), Eigen::Success);
                QCOMPARE(lu.accelerated(), accelerate);
                const Eigen::VectorXd b = Eigen::VectorXd::Random(a.rows());
                const double r = residual(a, lu.solve(b), b);
                QVERIFY2(r < 1e-12, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(from)).arg(r)));
            }
            {
                using cd = std::complex<double>;
                const auto a = grid<cd>(40, cd(0.2, 1.7));
                SparseLu<cd> lu;
                lu.compute(a);
                QCOMPARE(lu.info(), Eigen::Success);
                QCOMPARE(lu.accelerated(), accelerate);
                const Eigen::VectorXcd b = Eigen::VectorXcd::Random(a.rows());
                const double r = residual(a, lu.solve(b), b);
                QVERIFY2(r < 1e-12, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(from)).arg(r)));
            }
        }
    }

    // Factorized again with another pattern - an entry more, then fewer
    // (a Newton's Jacobian whose entries come and go): solved as each is.
    // Accelerate's refactorization wants the pattern analysed: the union is
    // analysed, a pattern within it factorized there.
    void aPatternThatChanges()
    {
        qputenv("QUCS_FEM_ACCELERATE_FROM", "0");
        const auto a = grid<double>(30), b = grid<double>(30, 0.5, 1), c = grid<double>(30, 1.0, 2);
        SparseLu<double> lu;
        lu.analyzePattern(a);
        const Eigen::VectorXd rhs = Eigen::VectorXd::Random(a.rows());
        for (const auto* m : {&a, &b, &c, &b, &c, &a}) {
            lu.factorize(*m);
            QCOMPARE(lu.info(), Eigen::Success);
            const double r = residual(*m, lu.solve(rhs), rhs);
            QVERIFY2(r < 1e-12, qPrintable(QString::number(r)));
        }
        // (Accelerate's: analysed three times - a, a and b's union, and
        // with c's; b, c and a within it after. Analysed at each change -
        // five here -, an analysis was eight factorizations' time.)
        if (lu.accelerated()) QCOMPARE(lu.analyses(), 3);
        // Another of the same pattern - a problem made again, a sweep's next
        // point -: its analysis the cache's, none made.
        if (lu.accelerated()) {
            const int before = acceleratedAnalysesMade();
            SparseLu<double> again;
            again.analyzePattern(a);
            again.factorize(a);
            QCOMPARE(again.info(), Eigen::Success);
            QCOMPARE(acceleratedAnalysesMade(), before);
            QVERIFY(residual(a, again.solve(rhs), rhs) < 1e-12);
        }
        // Another size: analysed again.
        const auto d = grid<double>(20);
        lu.factorize(d);
        QCOMPARE(lu.info(), Eigen::Success);
        const Eigen::VectorXd ones = Eigen::VectorXd::Ones(d.rows());
        QVERIFY(residual(d, lu.solve(ones), ones) < 1e-12);
    }

    // A singular matrix: said (info, a message) - not a stop of the program
    // (Accelerate's parameter errors stop it without a callback).
    void aSingularMatrixIsSaid()
    {
        for (const char* from : {"0", "eigen"}) {
            init();
            if (QByteArray(from) == "eigen") qputenv("QUCS_FEM_SOLVER", "eigen");
            else qputenv("QUCS_FEM_ACCELERATE_FROM", from);
            auto a = grid<double>(20);
            // (A row and a column of noughts.)
            for (int j = 0; j < a.cols(); ++j)
                for (Eigen::SparseMatrix<double>::InnerIterator it(a, j); it; ++it)
                    if (it.row() == 7 || it.col() == 7) it.valueRef() = 0;
            SparseLu<double> lu;
            lu.compute(a);
            QVERIFY2(lu.info() != Eigen::Success, from);
            QVERIFY2(!lu.lastErrorMessage().empty(), from);
            // (Solved all the same: no number - Accelerate's had solved it
            // finite, a residual of 0.035, said factorized.)
            QVERIFY(!lu.solve(Eigen::VectorXd::Ones(a.rows())).allFinite());
            qInfo("singular, %s: %s", from, lu.lastErrorMessage().c_str());
        }
    }

    // Its Cholesky: a symmetric positive definite matrix solved; one that is
    // not, refused - its caller then takes the LU.
    void theCholesky()
    {
        for (const char* from : {"0", "eigen"}) {
            init();
            if (QByteArray(from) == "eigen") qputenv("QUCS_FEM_SOLVER", "eigen");
            else qputenv("QUCS_FEM_ACCELERATE_FROM", from);
            const int m = 40, n = m * m;
            std::vector<Eigen::Triplet<double>> t;
            for (int i = 0; i < m; ++i)
                for (int j = 0; j < m; ++j) {
                    const int p = i * m + j;
                    t.emplace_back(p, p, 4.01);
                    if (i > 0) t.emplace_back(p, p - m, -1.0);
                    if (i < m - 1) t.emplace_back(p, p + m, -1.0);
                    if (j > 0) t.emplace_back(p, p - 1, -1.0);
                    if (j < m - 1) t.emplace_back(p, p + 1, -1.0);
                }
            Eigen::SparseMatrix<double> a(n, n);
            a.setFromTriplets(t.begin(), t.end());
            SparseCholesky c;
            c.analyzePattern(a);
            c.factorize(a);
            QCOMPARE(c.info(), Eigen::Success);
            const Eigen::VectorXd b = Eigen::VectorXd::Random(n);
            QVERIFY(residual(a, c.solve(b), b) < 1e-12);
            // (Its diagonal 3.9: indefinite.)
            Eigen::SparseMatrix<double> bad = a;
            for (int k = 0; k < n; ++k) bad.coeffRef(k, k) = k == n / 2 ? -3.9 : 4.01;
            c.factorize(bad);
            QVERIFY2(c.info() != Eigen::Success, from);
        }
    }
};

QTEST_GUILESS_MAIN(TestFemSparse)
#include "test_fem_sparse.moc"
