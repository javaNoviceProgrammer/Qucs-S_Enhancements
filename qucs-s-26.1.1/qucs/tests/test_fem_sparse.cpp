/*
 * The multiphysics solver's sparse direct factorizations (fem_sparse.h):
 * Accelerate's LU (macOS 26) and Cholesky where there are, Eigen's
 * elsewhere, below a size and when asked - each solving what Eigen's does,
 * a pattern changing between factorizations, a singular matrix said, one
 * singular to rounding too, the solver chosen and said.
 */
#include "fem_problem.h"
#include "fem_sparse.h"

#include <QElapsedTimer>
#include <QtTest>

#include <complex>
#include <thread>
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

bool slowBuild()
{
    // (QUCS_TEST_SLOW: a slow build's path in any, to check it quickly.)
    if (qEnvironmentVariableIsSet("QUCS_TEST_SLOW")) return true;
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
    return false;
}

/// An m by m grid's Laplacian: \a floating, no value set anywhere (each
/// node's diagonal its neighbours' count: singular, the constant its null
/// vector - a potential left floating); else 4.01 on it (sound). \a scaled:
/// D A D, D from 1e-3 to 1e3 (a sound matrix badly scaled).
Eigen::SparseMatrix<double> laplacian(int m, bool floating, bool scaled = false)
{
    const int n = m * m;
    std::vector<Eigen::Triplet<double>> t;
    auto d = [scaled](int k) { return scaled ? std::pow(10.0, double(k % 7) - 3) : 1.0; };
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < m; ++j) {
            const int p = i * m + j;
            int neighbours = 0;
            auto link = [&](int q) {
                t.emplace_back(p, q, -d(p) * d(q));
                ++neighbours;
            };
            if (i > 0) link(p - m);
            if (i < m - 1) link(p + m);
            if (j > 0) link(p - 1);
            if (j < m - 1) link(p + 1);
            t.emplace_back(p, p, (floating ? double(neighbours) : 4.01) * d(p) * d(p));
        }
    Eigen::SparseMatrix<double> a(n, n);
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
    // (a Newton's Jacobian whose entries come and go): solved as each is,
    // on each backend. Accelerate's refactorization wants the pattern
    // analysed: the union is analysed, a pattern within it factorized
    // there. Eigen's LU wants the pattern it analysed: another analysed
    // again. (Given another, Eigen's wrote past its arrays - a heap
    // overflow under CI's sanitizers, a SIGSEGV on macOS 15: the backend
    // where Accelerate's LU is not, never run on a Mac where it is.)
    void aPatternThatChanges_data()
    {
        QTest::addColumn<int>("backend");
        QTest::newRow("Eigen's") << int(DirectSolver::Eigen);
        QTest::newRow("the platform's") << int(DirectSolver::Automatic);
    }
    void aPatternThatChanges()
    {
        QFETCH(int, backend);
        setDirectSolver(DirectSolver(backend));
        const struct Reset {
            ~Reset() { setDirectSolver(DirectSolver::Automatic); }
        } reset;
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
        // (Analysed three times - a, a and b's union, and with c's; b, c and
        // a within it after -, Accelerate's and Eigen's alike. Analysed at
        // each change - five here -, an analysis was eight factorizations'
        // time; Eigen's was so till the hunt of 2026-10-17, N4.)
        QCOMPARE(lu.analyses(), 3);
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

    // Analysed and factorized on several threads at once - a sweep's points
    // solved at once: Accelerate's symbolic LU at once on several threads
    // stopped the program (a SIGSEGV in libSparse); one analysis at a time,
    // each solved to 1e-12.
    void onThreadsAtOnce()
    {
        qputenv("QUCS_FEM_ACCELERATE_FROM", "0");
        std::vector<double> worst(8, 1.0);
        std::vector<std::thread> pool;
        for (int k = 0; k < 8; ++k)
            pool.emplace_back([k, &worst] {
                double w = 0;
                for (int m : {30, 34, 30, 38}) {
                    const auto a = grid<double>(m, 0.1 * k);
                    SparseLu<double> lu;
                    lu.compute(a);
                    const Eigen::VectorXd b = Eigen::VectorXd::Ones(a.rows());
                    w = std::max(w, lu.info() == Eigen::Success ? residual(a, lu.solve(b), b) : 1.0);
                }
                worst[std::size_t(k)] = w;
            });
        for (std::thread& t : pool) t.join();
        for (double w : worst) QVERIFY2(w < 1e-12, qPrintable(QString::number(w)));
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

    // One of many solved at once, each on a thread of its own (the array
    // scan's cells): Accelerate's LU from 50 000 unknowns, not 20 000 - its
    // LUs one after another slower below that than Eigen's on their threads
    // together (the update check of 48d5fa8, 4: a scan of 21k-unknown cells
    // three times so). The environment's size and the choice of
    // Accelerate over it.
    void manyAtOnceFromFiftyThousand()
    {
        init();
        QCOMPARE(acceleratedFromAtOnce(), 50000);
        const auto a = grid<double>(123);   // (30 258 unknowns)
        for (bool many : {false, true}) {
            SparseLu<double> lu;
            lu.setManyAtOnce(many);
            lu.compute(a);
            QCOMPARE(lu.info(), Eigen::Success);
            QCOMPARE(lu.accelerated(), acceleratedLu() && !many);
            const Eigen::VectorXd b = Eigen::VectorXd::Random(a.rows());
            QVERIFY(residual(a, lu.solve(b), b) < 1e-12);
        }
        qputenv("QUCS_FEM_ACCELERATE_FROM", "100");
        QCOMPARE(acceleratedFromAtOnce(), 100);
        init();
        setDirectSolver(DirectSolver::Accelerate);
        QCOMPARE(acceleratedFromAtOnce(), 0);
        setDirectSolver(DirectSolver::Automatic);
        QCOMPARE(acceleratedFromAtOnce(), 50000);
    }

    // The solver chosen (Multiphysics > Settings…) and the environment's
    // over it; each factorization counted by its solver, a step's said.
    void theSolverChosen()
    {
        init();
        setDirectSolver(DirectSolver::Eigen);
        QVERIFY(directSolver() == DirectSolver::Eigen);
        QVERIFY(!acceleratedLu() && !acceleratedCholesky());
        QCOMPARE(acceleratedFrom(), 20000);
        SolverUse before = solverUse();
        {
            // (Accelerate's size made 100 unknowns: Eigen's chosen still)
            qputenv("QUCS_FEM_ACCELERATE_FROM", "100");
            SparseLu<double> lu;
            lu.compute(grid<double>(10));
            QVERIFY(!lu.accelerated());
            qunsetenv("QUCS_FEM_ACCELERATE_FROM");
        }
        QCOMPARE(solverUseText(before, solverUse()), QStringLiteral("Eigen's LU"));
        setDirectSolver(DirectSolver::Accelerate);
        QCOMPARE(acceleratedFrom(), 0);
        QCOMPARE(acceleratedLu(), acceleratedLuHindrance().isEmpty());
        qInfo("Accelerate's LU here: %s", acceleratedLu() ? "yes" : qPrintable(acceleratedLuHindrance()));
        before = solverUse();
        {
            SparseLu<double> lu;
            lu.compute(grid<double>(10));   // (200: any size, asked)
            QCOMPARE(lu.accelerated(), acceleratedLu());
            SparseCholesky c;
            const auto a = laplacian(20, false);
            c.analyzePattern(a);
            c.factorize(a);
            QCOMPARE(c.info(), Eigen::Success);
        }
        const QString used = solverUseText(before, solverUse());
        QVERIFY2(used.contains(acceleratedLu() ? QStringLiteral("Accelerate's LU") : QStringLiteral("Eigen's LU")), qPrintable(used));
#if defined(__APPLE__)
        QVERIFY2(used.contains(QStringLiteral("Accelerate's Cholesky")), qPrintable(used));
#endif
        // The environment over the choice, both ways.
        qputenv("QUCS_FEM_SOLVER", "eigen");
        QVERIFY(directSolver() == DirectSolver::Eigen && !acceleratedCholesky());
        setDirectSolver(DirectSolver::Eigen);
        qputenv("QUCS_FEM_SOLVER", "accelerate");
        QVERIFY(directSolver() == DirectSolver::Accelerate);
        QCOMPARE(acceleratedFrom(), 0);
        qputenv("QUCS_FEM_SOLVER", "auto");
        QVERIFY(directSolver() == DirectSolver::Automatic);
        QCOMPARE(acceleratedFrom(), 20000);
        init();
        setDirectSolver(DirectSolver::Automatic);
        QCOMPARE(solverUseText(solverUse(), solverUse()), QString());
    }

    // A matrix singular to rounding (a grid's Laplacian, no value set: a
    // potential left floating): its Cholesky refuses it, and so does an LU
    // asked to check (a PDE's, a linear system's), on either backend - an
    // LU not asked (a semiconductor's) solves it, as before; a linear system
    // says it singular rather than solve it (Eigen's LDLT had, 1e9 off), its
    // conjugate gradients stopping where they stall (an hour, before). A
    // sound one scaled from 1e-3 to 1e3 is solved by each.
    void singularToRounding()
    {
        // (A sanitized build: a grid of 50 - the 160's conjugate gradients
        // took 229 s there.)
        const int m = slowBuild() ? 50 : 160;
        const auto floating = laplacian(m, true), sound = laplacian(m, false, true);
        Eigen::VectorXd b = Eigen::VectorXd::Zero(floating.rows());
        b[17] = 1;   // (a charge: no solution, any constant added)
        for (DirectSolver solver : {DirectSolver::Eigen, DirectSolver::Accelerate}) {
            init();
            setDirectSolver(solver);
            const char* which = solver == DirectSolver::Eigen ? "Eigen" : "Accelerate";
            SparseCholesky c;
            c.analyzePattern(floating);
            c.factorize(floating);
            QVERIFY2(c.info() != Eigen::Success, which);
            if (c.singularToRounding()) QVERIFY2(QString::fromStdString(c.lastErrorMessage()).contains(QStringLiteral("singular to rounding")), which);
            c.factorize(sound);
            QVERIFY2(c.info() == Eigen::Success && !c.singularToRounding(), which);
            QVERIFY(residual(sound, c.solve(b), b) < 1e-10);
            SparseLu<double> checked;
            checked.setSingularChecked(true);
            checked.compute(floating);
            QVERIFY2(checked.info() != Eigen::Success, which);
            qInfo("%s: floating, %s", which, checked.lastErrorMessage().c_str());
            checked.compute(sound);
            QVERIFY2(checked.info() == Eigen::Success, which);
            QVERIFY(residual(sound, checked.solve(b), b) < 1e-10);
            SparseLu<double> unchecked;
            unchecked.compute(floating);
            if (solver == DirectSolver::Eigen || !acceleratedLu()) QVERIFY2(unchecked.info() == Eigen::Success, which);
            // The linear system: said singular, not solved.
            detail::LinearSystem system;
            system.setPattern(m * m, [m](const std::function<void(const int*, int)>& element) {
                for (int i = 0; i < m; ++i)
                    for (int j = 0; j < m; ++j) {
                        const int p = i * m + j;
                        if (j + 1 < m) {
                            const int pair[2] = {p, p + 1};
                            element(pair, 2);
                        }
                        if (i + 1 < m) {
                            const int pair[2] = {p, p + m};
                            element(pair, 2);
                        }
                    }
            });
            for (int j = 0; j < floating.outerSize(); ++j)
                for (Eigen::SparseMatrix<double>::InnerIterator it(floating, j); it; ++it) system.add(int(it.row()), j, it.value());
            QString error;
            // (said by its Cholesky: no LU made after)
            const SolverUse before = solverUse();
            QVERIFY2(!system.factorize(true, QStringLiteral("auto"), 1e-6, QStringLiteral("Floating"), nullptr, &error), which);
            QVERIFY2(error.contains(QStringLiteral("the matrix is singular")), qPrintable(error));
            QVERIFY2(!solverUseText(before, solverUse()).contains(QStringLiteral("LU")), qPrintable(solverUseText(before, solverUse())));
            qInfo("%s: %s", which, qPrintable(error));
            // Singular and indefinite (the floating grid beside -1 on a
            // diagonal): its Cholesky fails by its sign, its LU says it.
            {
                detail::LinearSystem both;
                const int n = m * m;
                both.setPattern(2 * n, [m, n](const std::function<void(const int*, int)>& element) {
                    for (int i = 0; i < m; ++i)
                        for (int j = 0; j < m; ++j) {
                            const int p = i * m + j;
                            if (j + 1 < m) {
                                const int pair[2] = {p, p + 1};
                                element(pair, 2);
                            }
                            if (i + 1 < m) {
                                const int pair[2] = {p, p + m};
                                element(pair, 2);
                            }
                            const int one[1] = {n + p};
                            element(one, 1);
                        }
                });
                for (int j = 0; j < floating.outerSize(); ++j)
                    for (Eigen::SparseMatrix<double>::InnerIterator it(floating, j); it; ++it) both.add(int(it.row()), j, it.value());
                for (int k = 0; k < n; ++k) both.add(n + k, n + k, -1.0);
                QString why;
                QVERIFY2(!both.factorize(true, QStringLiteral("auto"), 1e-6, QStringLiteral("Indefinite"), nullptr, &why), which);
                QVERIFY2(why.contains(QStringLiteral("singular to rounding")), qPrintable(why));
            }
            // Its conjugate gradients: stopped where they stall.
            QElapsedTimer clock;
            clock.start();
            QStringList log;
            error.clear();
            if (system.factorize(true, QStringLiteral("iterative"), 1e-6, QStringLiteral("Floating"), &log, &error)) {
                Eigen::VectorXd x;
                QVERIFY(!system.solve(b, x, QStringLiteral("Floating"), &error));
                QVERIFY2(error.contains(QStringLiteral("stalled")), qPrintable(error));
            } else {
                QVERIFY2(error.contains(QStringLiteral("the matrix is singular")), qPrintable(error));
            }
            qInfo("%s, iterative: %s (%lld ms)", which, qPrintable(error), clock.elapsed());
            QVERIFY(clock.elapsed() < 60000);
        }
        init();
        setDirectSolver(DirectSolver::Automatic);
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
