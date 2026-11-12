/*
 * test_rf25_dense.cpp - the dense LU of RF 2.5D's two large systems
 * (rf25/rf25_dense.h): Accelerate's LAPACK where the build and the macOS
 * have it, OpenBLAS's where the build has it, Eigen's elsewhere and when
 * asked - the same solutions to rounding and each one's rcond the
 * matrix's, on a random matrix and on one shaped as a method of moments'
 * is, complex and real, past several of the blocked LU's panels; any
 * number of right-hand sides; a singular matrix said to be one; OpenBLAS's
 * calls from many threads at once; and what chooses between them.
 */
#include <QtTest>
#include <QOperatingSystemVersion>

#include "rf25/rf25_dense.h"

#include <Eigen/Dense>

#include <cmath>
#include <complex>
#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;
template <class S> using Mat = Eigen::Matrix<S, Eigen::Dynamic, Eigen::Dynamic>;

namespace {

/// Whether this build is one whose dense solves are many times slower - a
/// sanitized one, as CI's Linux job is (QUCS_TEST_SLOW: its path in any).
bool slowBuild()
{
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

/// splitmix64 of a key, to [-1, 1): the same matrix whichever path solves it.
double unit(std::uint64_t key)
{
    std::uint64_t z = key + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return double(z >> 11) * 0x1.0p-52 - 1.0;
}

template <class S> S entry(std::uint64_t key);
template <> double entry<double>(std::uint64_t key)
{
    return unit(2 * key);
}
template <> Complex entry<Complex>(std::uint64_t key)
{
    return {unit(2 * key), unit(2 * key + 1)};
}

/// Entries in [-1, 1) - their condition some n, rows swapped throughout.
template <class S> Mat<S> randomMatrix(int rows, int cols, std::uint64_t seed)
{
    Mat<S> a(rows, cols);
    for (int j = 0; j < cols; ++j)
        for (int i = 0; i < rows; ++i) a(i, j) = entry<S>((seed << 40) + std::uint64_t(j) * std::uint64_t(rows) + i);
    return a;
}

/// A method of moments' kind: n cells on a jittered grid four wavelengths
/// across, coupled by exp(-jkr)/(4 pi r) times a cell's area (real: 1/r),
/// symmetric, the diagonal 1.1 (1 - 0.5j) times the largest column's sum
/// - diagonally dominant, so no rows are swapped.
template <class S> Mat<S> momentMatrix(int n)
{
    const int side = int(std::ceil(std::sqrt(double(n))));
    const double h = 1.0 / side, k = 2 * M_PI / 0.25;
    QList<double> x, y;
    for (int i = 0; i < n; ++i) {
        x << (i % side + 0.5 + 0.3 * unit(7000000000ull + 2 * std::uint64_t(i))) * h;
        y << (i / side + 0.5 + 0.3 * unit(7000000000ull + 2 * std::uint64_t(i) + 1)) * h;
    }
    Mat<S> a = Mat<S>::Zero(n, n);
    double largest = 0;
    for (int j = 0; j < n; ++j) {
        double sum = 0;
        for (int i = 0; i < n; ++i) {
            if (i == j) continue;
            const double r = std::hypot(x.at(i) - x.at(j), y.at(i) - y.at(j));
            if constexpr (std::is_same_v<S, Complex>)
                a(i, j) = h * h * std::exp(Complex(0, -k * r)) / (4 * M_PI * r);
            else
                a(i, j) = h * h / (4 * M_PI * r);
            sum += std::abs(a(i, j));
        }
        largest = std::max(largest, sum);
    }
    for (int i = 0; i < n; ++i) {
        if constexpr (std::is_same_v<S, Complex>)
            a(i, i) = 1.1 * largest * Complex(1, -0.5);
        else
            a(i, i) = 1.1 * largest;
    }
    return a;
}

/// The largest of a solution's columns' ||b - A x||inf / (||A||inf ||x||inf).
template <class S> double backwardError(const Mat<S>& a, const Mat<S>& x, const Mat<S>& b)
{
    const double normA = a.cwiseAbs().rowwise().sum().maxCoeff();
    double most = 0;
    for (int q = 0; q < b.cols(); ++q) {
        const double r = (b.col(q) - a * x.col(q)).cwiseAbs().maxCoeff();
        most = std::max(most, r / (normA * x.col(q).cwiseAbs().maxCoeff()));
    }
    return most;
}

/// 1 / (||A||1 ||A^-1||1), A^-1 taken whole.
template <class S> double trueRcond(const Mat<S>& a)
{
    const Mat<S> inverse = a.partialPivLu().inverse();
    return 1 / (a.cwiseAbs().colwise().sum().maxCoeff() * inverse.cwiseAbs().colwise().sum().maxCoeff());
}

/// The paths there are here: Eigen's always, Accelerate's and OpenBLAS's
/// where they are.
QList<rf::DenseLu> paths()
{
    QList<rf::DenseLu> out{rf::DenseLu::Eigen};
    if (rf::accelerateLuHere()) out << rf::DenseLu::Accelerate;
    if (rf::openBlasLuHere()) out << rf::DenseLu::OpenBlas;
    return out;
}

QString nameOf(rf::DenseLu lu)
{
    return rf::denseLuName(lu);
}

/// Each path's solution and rcond of one matrix, checked against the
/// matrix itself, and the two against each other: their solutions to
/// rounding (\a agree, relative), their rcond within the estimators'
/// spread of each other and at or above the true one (an estimate of
/// ||A^-1|| is never above it) - the true one \a given, or taken where it
/// is cheap: up to 400 rows on a sanitized build.
template <class S> void solvedAlike(const Mat<S>& a, const Mat<S>& b, double agree, const QString& what, double given = -1)
{
    const double exact = given >= 0 ? given : a.rows() <= 400 || !slowBuild() ? trueRcond(a) : 0.0;
    QList<Mat<S>> solutions;
    QList<double> rconds;
    for (const rf::DenseLu lu : paths()) {
        rf::setDenseLu(lu);
        const long long accelerated = rf::accelerateLuCount(), openBlas = rf::openBlasLuCount(), eigen = rf::eigenLuCount();
        Mat<S> copy = a;
        const rf::DenseFactors<S> factors(std::move(copy));
        QCOMPARE(factors.library(), lu);
        QCOMPARE(factors.accelerated(), lu == rf::DenseLu::Accelerate);
        QCOMPARE(factors.openBlas(), lu == rf::DenseLu::OpenBlas);
        QCOMPARE(rf::accelerateLuCount() - accelerated, lu == rf::DenseLu::Accelerate ? 1LL : 0LL);
        QCOMPARE(rf::openBlasLuCount() - openBlas, lu == rf::DenseLu::OpenBlas ? 1LL : 0LL);
        QCOMPARE(rf::eigenLuCount() - eigen, lu == rf::DenseLu::Eigen ? 1LL : 0LL);
        const Mat<S> x = factors.solve(b);
        QCOMPARE(x.rows(), b.rows());
        QCOMPARE(x.cols(), b.cols());
        const double backward = backwardError(a, x, b);
        QVERIFY2(backward < 1e-14, qPrintable(QStringLiteral("%1, %2: a backward error of %3").arg(what, nameOf(lu)).arg(backward)));
        const double rcond = factors.rcond();
        if (exact > 0)
            QVERIFY2(rcond >= exact * (1 - 1e-6) && rcond <= 3 * exact,
                     qPrintable(QStringLiteral("%1, %2: rcond %3, the matrix's %4").arg(what, nameOf(lu)).arg(rcond).arg(exact)));
        else
            QVERIFY2(rcond > 0 && rcond <= 1, qPrintable(QStringLiteral("%1, %2: rcond %3").arg(what, nameOf(lu)).arg(rcond)));
        solutions << x;
        rconds << rcond;
    }
    rf::setDenseLu(rf::DenseLu::Automatic);
    // (Each library's against Eigen's.)
    for (int i = 1; i < solutions.size(); ++i) {
        const QString which = nameOf(paths().at(i));
        const double apart = (solutions.at(0) - solutions.at(i)).cwiseAbs().maxCoeff() / solutions.at(0).cwiseAbs().maxCoeff();
        QVERIFY2(apart < agree, qPrintable(QStringLiteral("%1: Eigen's and %2's solutions %3 apart").arg(what, which).arg(apart)));
        QVERIFY2(rconds.at(i) <= 2 * rconds.at(0) && rconds.at(0) <= 2 * rconds.at(i),
                 qPrintable(QStringLiteral("%1: rcond %2 and %3's %4").arg(what).arg(rconds.at(0)).arg(which).arg(rconds.at(i))));
    }
}

} // namespace

class TestRf25Dense : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        rf::setDenseLu(rf::DenseLu::Automatic);
        qunsetenv("QUCS_RF25_LU");
        qunsetenv("QUCS_FEM_SOLVER");
    }

    // A built Qucs-S on macOS 13.3 and later factorizes with Accelerate's
    // LAPACK: what the build's check found (QUCS_ACCELERATE_LAPACK) - a
    // build with Accelerate off has it not, as Linux's.
    void macOSFactorizesWithAccelerate()
    {
#if defined(QUCS_TEST_ACCELERATE_LAPACK)
        if (QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::MacOS, 13, 3))
            QSKIP("macOS before 13.3: Eigen's alone");
        QVERIFY(rf::accelerateLuHere());
        QCOMPARE(rf::denseLu(), rf::DenseLu::Automatic);
        Mat<Complex> a = randomMatrix<Complex>(40, 40, 1);
        const rf::DenseFactors<Complex> factors(std::move(a));
        QVERIFY(factors.accelerated());
#else
        QVERIFY(!rf::accelerateLuHere());
#endif
    }

    // A random matrix, its condition some n and its rows swapped throughout,
    // past three of the blocked LU's panels of 256 (and short of a fourth):
    // complex, as the full wave's runs, and real, as the charges.
    void aRandomMatrixIsSolvedAlike()
    {
        const int n = slowBuild() ? 600 : 900;
        solvedAlike<Complex>(randomMatrix<Complex>(n, n, 1), randomMatrix<Complex>(n, 3, 2), 1e-11,
                             QStringLiteral("complex %1").arg(n));
        solvedAlike<double>(randomMatrix<double>(n, n, 3), randomMatrix<double>(n, 2, 4), 1e-11,
                            QStringLiteral("real %1").arg(n));
    }

    // Shaped as a method of moments' is: no rows swapped, well conditioned.
    void aMomentMatrixIsSolvedAlike()
    {
        const int n = slowBuild() ? 520 : 800;
        solvedAlike<Complex>(momentMatrix<Complex>(n), randomMatrix<Complex>(n, 2, 5), 1e-13,
                             QStringLiteral("complex %1").arg(n));
        solvedAlike<double>(momentMatrix<double>(n), randomMatrix<double>(n, 5, 6), 1e-13,
                            QStringLiteral("real %1").arg(n));
    }

    // One right-hand side or many: a solve of fewer than eight columns is
    // padded to eight for Accelerate (its solve of two was three times its
    // solve of eight), and every one is the matrix's.
    void anyNumberOfRightHandSidesIsSolved()
    {
        const int n = 300;
        const Mat<Complex> a = randomMatrix<Complex>(n, n, 7);
        const double exact = trueRcond(a);
        for (const int columns : {1, 2, 3, 7, 8, 9, 20}) {
            const Mat<Complex> b = randomMatrix<Complex>(n, columns, 100 + columns);
            solvedAlike<Complex>(a, b, 1e-11, QStringLiteral("%1 column(s)").arg(columns), exact);
        }
        // A matrix of one row, and one of a panel's width exactly.
        solvedAlike<Complex>(randomMatrix<Complex>(1, 1, 8) + Mat<Complex>::Identity(1, 1),
                             randomMatrix<Complex>(1, 2, 9), 1e-14, QStringLiteral("one row"));
        solvedAlike<Complex>(randomMatrix<Complex>(256, 256, 10), randomMatrix<Complex>(256, 2, 11), 1e-11,
                             QStringLiteral("256 rows"));
    }

    // A matrix that is singular - a column of noughts - is said to be: its
    // rcond not over the full wave's warning (1e-14), each way (Eigen's
    // estimate of one may be NaN, its solve not finite: what the full wave
    // says first).
    void aSingularMatrixIsSaidToBe()
    {
        const int n = 300;
        for (const rf::DenseLu lu : paths()) {
            rf::setDenseLu(lu);
            Mat<Complex> a = randomMatrix<Complex>(n, n, 12);
            a.col(n / 2).setZero();
            const rf::DenseFactors<Complex> complexFactors(std::move(a));
            QVERIFY2(!(complexFactors.rcond() >= 1e-14), qPrintable(QStringLiteral("%1: rcond %2").arg(nameOf(lu)).arg(complexFactors.rcond())));
            Mat<double> r = randomMatrix<double>(n, n, 13);
            r.col(n - 1).setZero();
            const rf::DenseFactors<double> realFactors(std::move(r));
            QVERIFY2(!(realFactors.rcond() >= 1e-14), qPrintable(QStringLiteral("%1: rcond %2").arg(nameOf(lu)).arg(realFactors.rcond())));
        }
        rf::setDenseLu(rf::DenseLu::Automatic);
    }

    // What chooses: the process's choice; QUCS_RF25_LU over it; and
    // QUCS_FEM_SOLVER=eigen, Eigen everywhere, as on Linux and Windows.
    void theEnvironmentChoosesOverTheProcess()
    {
        // (The library that factorized, and the one said to be in effect.)
        const auto factorized = [] {
            Mat<double> a = randomMatrix<double>(30, 30, 14) + 30 * Mat<double>::Identity(30, 30);
            const rf::DenseLu said = rf::denseLuInEffect();
            const rf::DenseLu by = rf::DenseFactors<double>(std::move(a)).library();
            return said == by ? by : rf::DenseLu::Automatic;
        };
        const rf::DenseLu automatic = rf::accelerateLuHere()  ? rf::DenseLu::Accelerate
                                      : rf::openBlasLuHere() ? rf::DenseLu::OpenBlas
                                                             : rf::DenseLu::Eigen;
        const rf::DenseLu accelerate = rf::accelerateLuHere() ? rf::DenseLu::Accelerate : rf::DenseLu::Eigen;
        const rf::DenseLu openBlas = rf::openBlasLuHere() ? rf::DenseLu::OpenBlas : rf::DenseLu::Eigen;
        QCOMPARE(rf::denseLu(), rf::DenseLu::Automatic);
        QCOMPARE(factorized(), automatic);
        rf::setDenseLu(rf::DenseLu::Eigen);
        QCOMPARE(rf::denseLu(), rf::DenseLu::Eigen);
        QCOMPARE(factorized(), rf::DenseLu::Eigen);
        qputenv("QUCS_RF25_LU", "auto");
        QCOMPARE(rf::denseLu(), rf::DenseLu::Automatic);
        QCOMPARE(factorized(), automatic);
        qputenv("QUCS_RF25_LU", " Eigen ");
        rf::setDenseLu(rf::DenseLu::Accelerate);
        QCOMPARE(rf::denseLu(), rf::DenseLu::Eigen);
        QCOMPARE(factorized(), rf::DenseLu::Eigen);
        qunsetenv("QUCS_RF25_LU");
        QCOMPARE(rf::denseLu(), rf::DenseLu::Accelerate);
        QCOMPARE(factorized(), accelerate);
        rf::setDenseLu(rf::DenseLu::OpenBlas);
        QCOMPARE(rf::denseLu(), rf::DenseLu::OpenBlas);
        QCOMPARE(factorized(), openBlas);
        qputenv("QUCS_FEM_SOLVER", "eigen");
        rf::setDenseLu(rf::DenseLu::Automatic);
        QCOMPARE(rf::denseLu(), rf::DenseLu::Eigen);
        QCOMPARE(factorized(), rf::DenseLu::Eigen);
        qputenv("QUCS_RF25_LU", "accelerate");
        QCOMPARE(rf::denseLu(), rf::DenseLu::Accelerate);
        QCOMPARE(factorized(), accelerate);
        qputenv("QUCS_RF25_LU", "OpenBLAS");
        QCOMPARE(rf::denseLu(), rf::DenseLu::OpenBlas);
        QCOMPARE(factorized(), openBlas);
        qunsetenv("QUCS_RF25_LU");
        qunsetenv("QUCS_FEM_SOLVER");
        QCOMPARE(rf::denseLu(), rf::DenseLu::Automatic);
        QCOMPARE(rf::denseLuName(rf::DenseLu::OpenBlas), QStringLiteral("OpenBLAS"));
        QCOMPARE(rf::denseLuName(rf::DenseLu::Accelerate), QStringLiteral("Accelerate"));
        QCOMPARE(rf::denseLuName(rf::DenseLu::Eigen), QStringLiteral("Eigen"));
    }

    // OpenBLAS where the build has it: it says what it is, and is the
    // library LAPACK's names were found in (on macOS Accelerate exports
    // them too); where it is not, nothing is said of it.
    void openBlasSaysWhatItIs()
    {
        const QString about = rf::openBlasLuAbout();
        qInfo("OpenBLAS: %s", rf::openBlasLuHere() ? qPrintable(about) : "not in this build");
        if (!rf::openBlasLuHere()) {
            QVERIFY(about.isEmpty());
            return;
        }
        QVERIFY2(about.startsWith(QLatin1String("OpenBLAS")) && about.contains(QLatin1String(" thread")), qPrintable(about));
    }

    // A sweep's frequencies factorize side by side: OpenBLAS's calls from
    // eight threads at once - those with all the threads taking their turns
    // on its lane, those with one side by side where OpenBLAS can keep to
    // one (else Eigen's) - each solved as one alone would be, and what each
    // waited is said: nought for Eigen's and Accelerate's.
    void openBlasCallsFromManyThreadsTakeTurns()
    {
        if (!rf::openBlasLuHere()) QSKIP("No OpenBLAS in this build");
        rf::setDenseLu(rf::DenseLu::OpenBlas);
        const int n = slowBuild() ? 200 : 400, threads = 8;
        std::vector<Mat<Complex>> as, bs, xs(threads);
        std::vector<double> waited(threads, -1), rconds(threads, -1);
        for (int t = 0; t < threads; ++t) {
            as.push_back(randomMatrix<Complex>(n, n, 200 + t));
            bs.push_back(randomMatrix<Complex>(n, 3, 300 + t));
        }
        std::atomic_int byOpenBlas{0};
        std::vector<std::thread> running;
        for (int t = 0; t < threads; ++t)
            running.emplace_back([&, t] {
                Mat<Complex> copy = as.at(std::size_t(t));
                // (Half on all the threads, taking turns; half on one each,
                //  side by side where OpenBLAS can be, else Eigen's.)
                const rf::DenseFactors<Complex> factors(std::move(copy), t % 2 == 0 ? 0 : 1);
                if (factors.openBlas()) ++byOpenBlas;
                xs[std::size_t(t)] = factors.solve(bs.at(std::size_t(t)));
                rconds[std::size_t(t)] = factors.rcond();
                waited[std::size_t(t)] = factors.waited();
            });
        for (std::thread& t : running) t.join();
        rf::setDenseLu(rf::DenseLu::Automatic);
        QCOMPARE(byOpenBlas.load(), rf::openBlasSideBySide() ? threads : threads / 2);
        for (int t = 0; t < threads; ++t) {
            const double backward = backwardError(as.at(std::size_t(t)), xs.at(std::size_t(t)), bs.at(std::size_t(t)));
            QVERIFY2(backward < 1e-14, qPrintable(QStringLiteral("thread %1: a backward error of %2").arg(t).arg(backward)));
            QVERIFY2(rconds.at(std::size_t(t)) > 0 && rconds.at(std::size_t(t)) <= 1, qPrintable(QString::number(rconds.at(std::size_t(t)))));
            QVERIFY(waited.at(std::size_t(t)) >= 0);
        }
        rf::setDenseLu(rf::DenseLu::Eigen);
        Mat<Complex> a = as.front();
        const rf::DenseFactors<Complex> eigen(std::move(a));
        (void)eigen.solve(bs.front());
        QCOMPARE(eigen.waited(), 0.0);
        rf::setDenseLu(rf::DenseLu::Automatic);
    }

    // Each library's time for one matrix - factorized, solved for two
    // columns, its rcond -, complex as the full wave's and real as the
    // charges', and for eight frequencies factorized at once, each given
    // its share of the threads as a sweep's is (Eigen's side by side, one
    // thread each; OpenBLAS's side by side on one thread each where the
    // share is under four threads, else taking turns on all of them).
    // Skipped unless QUCS_RF25_BENCH is set (full: the larger sizes);
    // docs/feature_gaps/2026-11-09-linux-parallel-solvers.md.
    void benchmarkTheLibraries()
    {
        const QByteArray bench = qgetenv("QUCS_RF25_BENCH").trimmed().toLower();
        if (bench.isEmpty()) QSKIP("QUCS_RF25_BENCH not set");
        const bool full = bench == "full";
        qInfo("OpenBLAS: %s; threads: %u", rf::openBlasLuHere() ? qPrintable(rf::openBlasLuAbout()) : "not in this build",
              std::thread::hardware_concurrency());
        const auto timed = [](auto&& job) {
            QElapsedTimer clock;
            clock.start();
            job();
            return double(clock.nsecsElapsed()) * 1e-9;
        };
        double backward = 0;
        const auto once = [&timed, &backward](auto a, const auto& b) {
            using M = std::decay_t<decltype(a)>;
            const M copy = a;
            const double seconds = timed([&] {
                const rf::DenseFactors<typename M::Scalar> factors(std::move(a));
                const M x = factors.solve(b);
                (void)factors.rcond();
                backward = backwardError(copy, x, b);
            });
            return seconds;
        };
        const QList<int> complexSizes = full ? QList<int>{2000, 4000, 6000, 8000} : QList<int>{2000, 4000};
        const QList<int> realSizes = full ? QList<int>{4000, 8000, 12000} : QList<int>{4000, 6000};
        for (const bool complex : {true, false}) {
            for (const int n : complex ? complexSizes : realSizes) {
                QString line = QStringLiteral("%1 %2:").arg(complex ? QStringLiteral("complex") : QStringLiteral("real")).arg(n);
                double eigen = 0;
                for (const rf::DenseLu lu : paths()) {
                    rf::setDenseLu(lu);
                    const double seconds = complex ? once(randomMatrix<Complex>(n, n, 400), randomMatrix<Complex>(n, 2, 401))
                                                   : once(randomMatrix<double>(n, n, 402), randomMatrix<double>(n, 2, 403));
                    QVERIFY2(backward < 1e-13, qPrintable(QStringLiteral("%1, %2: %3").arg(line, nameOf(lu)).arg(backward)));
                    if (lu == rf::DenseLu::Eigen) eigen = seconds;
                    line += QStringLiteral(" %1 %2 s (%3x)").arg(nameOf(lu)).arg(seconds, 0, 'f', 3).arg(eigen / seconds, 0, 'f', 1);
                }
                qInfo("%s", qPrintable(line));
            }
        }
        const int n = full ? 4000 : 2500, atOnce = 8;
        const int share = std::max(1, int(std::thread::hardware_concurrency()) / atOnce);
        QString line = QStringLiteral("%1 complex %2 at once, %3 thread(s) each:").arg(atOnce).arg(n).arg(share);
        double eigen = 0;
        for (const rf::DenseLu lu : paths()) {
            rf::setDenseLu(lu);
            std::vector<std::thread> running;
            const double seconds = timed([&] {
                for (int t = 0; t < atOnce; ++t)
                    running.emplace_back([n, t, share] {
                        Mat<Complex> a = randomMatrix<Complex>(n, n, 500 + t);
                        const rf::DenseFactors<Complex> factors(std::move(a), share);
                        (void)factors.solve(randomMatrix<Complex>(n, 2, 600 + t));
                    });
                for (std::thread& t : running) t.join();
            });
            if (lu == rf::DenseLu::Eigen) eigen = seconds;
            line += QStringLiteral(" %1 %2 s (%3x)").arg(nameOf(lu)).arg(seconds, 0, 'f', 3).arg(eigen / seconds, 0, 'f', 1);
        }
        qInfo("%s", qPrintable(line));
        rf::setDenseLu(rf::DenseLu::Automatic);
    }
};

QTEST_MAIN(TestRf25Dense)
#include "test_rf25_dense.moc"
