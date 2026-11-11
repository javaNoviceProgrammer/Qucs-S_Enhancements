/*
 * test_rf25_adaptive.cpp - RF 2.5D's adaptive frequency sweep and the
 * convergence check's verdicts (rf25/rf25_adaptive.h), on answers known in
 * closed form: a resonator's two-port and a lossy line's, solved by a
 * stand-in for the full-wave solve that counts what it is asked for.
 */
#include <QtTest>

#include "rf25/rf25_adaptive.h"

#include <Eigen/Dense>

#include <cmath>

namespace rf = qucs_s::rf25;

using cd = std::complex<double>;

namespace {

QList<double> sweepOf(double from, double to, int n)
{
    QList<double> f;
    for (int k = 0; k < n; ++k) f << from + (to - from) * k / (n - 1);
    return f;
}

// A series resonator between two ports, each port shunted by a capacitor:
// its Z at f (Hz). Q about 25 at 20 GHz.
QList<QList<cd>> resonatorZ(double f)
{
    const double w = 2 * M_PI * f;
    const cd ys = 1.0 / (cd(5, 0) + cd(0, w * 1e-9) + 1.0 / cd(0, w * 63e-15));
    const cd yp(0, w * 20e-15);
    Eigen::Matrix2cd y;
    y << ys + yp, -ys, -ys, ys + yp;
    const Eigen::Matrix2cd z = y.inverse();
    return {{z(0, 0), z(0, 1)}, {z(1, 0), z(1, 1)}};
}

// A lossy line 30 mm long, eps_eff 4, its loss rising as the root of f: no
// rational function of low degree, a resonance every 2.5 GHz.
QList<QList<cd>> lineZ(double f)
{
    const double c0 = 299792458.0, len = 0.03;
    const cd gamma(0.5 * std::sqrt(f / 1e9), 2 * M_PI * f * 2 / c0);
    const cd zc(50, 0);
    const cd z11 = zc / std::tanh(gamma * len), z12 = zc / std::sinh(gamma * len);
    return {{z11, z12}, {z12, z11}};
}

// What a solve's currents are, made of its Z: linear in it, so that they
// have its poles, as a structure's currents have its ports' Z's.
QList<QList<cd>> currentsOf(const QList<QList<cd>>& z)
{
    return {{z[0][0], z[1][0], z[0][0] + cd(0, 1) * z[1][0]}, {z[0][1], z[1][1], 2.0 * z[1][1]}};
}

// A stand-in for fullWaveOf: \a zOf at each frequency asked, in that order.
struct Solver {
    std::function<QList<QList<cd>>(double)> zOf;
    int asked = 0, rounds = 0;
    int failOn = -1;   // the round that fails (from 1), or none
    rf::FullWave operator()(const QList<double>& f)
    {
        ++rounds;
        asked += int(f.size());
        rf::FullWave w;
        w.portNames = {QStringLiteral("P1"), QStringLiteral("P2")};
        w.references = {50, 50};
        w.runs = 3;
        w.cells = 4;
        w.lines << QStringLiteral("solved %1").arg(f.size());
        if (rounds == failOn) {
            w.problems << QStringLiteral("It failed.");
            return w;
        }
        for (const double x : f) {
            const QList<QList<cd>> z = zOf(x);
            w.frequencies << x;
            w.z << z;
            w.s << rf::scatteringOf(z, w.references);
            w.currents << currentsOf(z);
        }
        return w;
    }
};

double largest(const QList<QList<cd>>& a, const QList<QList<cd>>& b)
{
    double d = 0;
    for (int r = 0; r < a.size(); ++r)
        for (int c = 0; c < a[r].size(); ++c) d = std::max(d, std::abs(a[r][c] - b[r][c]));
    return d;
}

} // namespace

class TestRf25Adaptive : public QObject
{
    Q_OBJECT

private slots:
    void aRationalFunctionIsFoundAgain();
    void anAdaptiveSweepSolvesFewAndReadsTheRest();
    void aLineOfManyResonancesIsReadWithinTheTolerance();
    void theCurrentsAreCarriedByTheFit();
    void aShortSweepIsSolvedWhole();
    void aStoppedOrFailedSweepSaysSo();
    void aCalibrationIsReadBetweenItsFrequencies();
    void reciprocityAndPassivityAreTakenOverEveryFrequency();
    void aValueConvergingIsExtrapolated();
    void aValueNotConvergingIsSaid();
    void aMeshIsRefinedAsAWhole();
};

void TestRf25Adaptive::aRationalFunctionIsFoundAgain()
{
    // Thirty samples of the resonator's Z: its terms are a rational function
    // of low degree, found again to rounding between the samples.
    QList<double> f = sweepOf(1e9, 40e9, 30);
    QList<QList<cd>> v;
    for (const double x : f) {
        const QList<QList<cd>> z = resonatorZ(x);
        v << QList<cd>{z[0][0], z[0][1], z[1][0], z[1][1]};
    }
    const rf::Rational fit = rf::fitRational(f, v);
    QVERIFY(fit.ok());
    QVERIFY2(fit.support.size() <= 8, qPrintable(QString::number(fit.support.size())));
    double worst = 0;
    for (const double x : sweepOf(1.1e9, 39.9e9, 97)) {
        const QList<cd> r = rf::rationalAt(fit, v, x);
        const QList<QList<cd>> z = resonatorZ(x);
        const QList<cd> want{z[0][0], z[0][1], z[1][0], z[1][1]};
        for (int q = 0; q < 4; ++q) worst = std::max(worst, std::abs(r[q] - want[q]) / std::abs(want[q]));
    }
    QVERIFY2(worst < 1e-6, qPrintable(QString::number(worst)));
    // At a support frequency, the sample itself.
    const QList<cd> c = fit.coefficients(fit.at.first());
    QCOMPARE(c.at(0), cd(1, 0));
}

void TestRf25Adaptive::anAdaptiveSweepSolvesFewAndReadsTheRest()
{
    Solver solver{resonatorZ};
    const QList<double> sweep = sweepOf(1e9, 40e9, 201);
    const rf::Adapted a = rf::adaptiveSweep(sweep, std::ref(solver));
    QVERIFY(a.wave.ok());
    QVERIFY(a.converged);
    QVERIFY(!a.whole);
    QVERIFY2(a.solved.size() <= 20, qPrintable(QString::number(a.solved.size())));
    QCOMPARE(solver.asked, int(a.solved.size()));
    QCOMPARE(solver.rounds, a.rounds);
    // The ends are solved; the answer has every frequency of the sweep, and
    // says which were solved.
    QCOMPARE(a.solved.first(), sweep.first());
    QCOMPARE(a.solved.last(), sweep.last());
    QCOMPARE(a.wave.frequencies, sweep);
    QCOMPARE(a.wave.solvedFrequencies, a.solved);
    QCOMPARE(a.wave.z.size(), sweep.size());
    QCOMPARE(a.wave.s.size(), sweep.size());
    double worst = 0;
    for (int k = 0; k < sweep.size(); ++k)
        worst = std::max(worst, largest(a.wave.s[k], rf::scatteringOf(resonatorZ(sweep[k]), {50, 50})));
    QVERIFY2(worst <= 2e-3, qPrintable(QString::number(worst)));
    QVERIFY(a.error <= 2e-3);
    qInfo("The resonator: %d of 201 solved in %d rounds, S within %.2g everywhere", int(a.solved.size()), a.rounds, worst);
    QVERIFY2(a.wave.lines.first().startsWith(QStringLiteral("Adding points where the answer changes: %1 of the study's 201 frequencies solved").arg(a.solved.size())),
             qPrintable(a.wave.lines.first()));
    QVERIFY(a.wave.lines.at(1).startsWith(QStringLiteral("Solved at 1, ")));
    // The solve's own lines after it.
    QVERIFY(a.wave.lines.contains(QStringLiteral("solved 5")));
}

void TestRf25Adaptive::aLineOfManyResonancesIsReadWithinTheTolerance()
{
    // Sixteen resonances over the sweep: more points, still under half of
    // it, and every frequency within a little of the tolerance.
    Solver solver{lineZ};
    const QList<double> sweep = sweepOf(0.5e9, 40e9, 301);
    rf::AdaptiveSettings how;
    how.tolerance = 1e-3;
    const rf::Adapted a = rf::adaptiveSweep(sweep, std::ref(solver), how);
    QVERIFY(a.wave.ok());
    QVERIFY(a.converged);
    QVERIFY2(a.solved.size() < 150, qPrintable(QString::number(a.solved.size())));
    double worst = 0;
    for (int k = 0; k < sweep.size(); ++k)
        worst = std::max(worst, largest(a.wave.s[k], rf::scatteringOf(lineZ(sweep[k]), {50, 50})));
    QVERIFY2(worst <= 3e-3, qPrintable(QString::number(worst)));
    qInfo("The line: %d of 301 solved in %d rounds, S within %.2g everywhere", int(a.solved.size()), a.rounds, worst);
    // At most four a round, and no two in one gap.
    QVERIFY(a.rounds >= 3);
    QVERIFY(a.solved.size() <= 9 + 4 * (a.rounds - 2));
}

void TestRf25Adaptive::theCurrentsAreCarriedByTheFit()
{
    Solver solver{resonatorZ};
    const QList<double> sweep = sweepOf(1e9, 40e9, 101);
    const rf::Adapted a = rf::adaptiveSweep(sweep, std::ref(solver));
    QCOMPARE(a.wave.currents.size(), sweep.size());
    double worst = 0;
    for (int k = 0; k < sweep.size(); ++k) {
        const QList<QList<cd>> want = currentsOf(resonatorZ(sweep[k]));
        QCOMPARE(a.wave.currents[k].size(), 2);
        for (int p = 0; p < 2; ++p) {
            QCOMPARE(a.wave.currents[k][p].size(), 3);
            for (int r = 0; r < 3; ++r)
                worst = std::max(worst, std::abs(a.wave.currents[k][p][r] - want[p][r]) / std::abs(want[p][r]));
        }
    }
    QVERIFY2(worst < 1e-4, qPrintable(QString::number(worst)));
}

void TestRf25Adaptive::aShortSweepIsSolvedWhole()
{
    Solver solver{resonatorZ};
    const QList<double> sweep = sweepOf(1e9, 40e9, 11);
    const rf::Adapted a = rf::adaptiveSweep(sweep, std::ref(solver));
    QVERIFY(a.whole);
    QCOMPARE(solver.rounds, 1);
    QCOMPARE(a.solved, sweep);
    QVERIFY(a.wave.solvedFrequencies.isEmpty());
    QCOMPARE(a.lines.first(),
             QStringLiteral("The study's 11 frequencies are too few for adding points to save anything: every one is solved."));
}

void TestRf25Adaptive::aStoppedOrFailedSweepSaysSo()
{
    const QList<double> sweep = sweepOf(1e9, 40e9, 101);
    {
        Solver solver{resonatorZ};
        int asked = 0;
        const rf::Adapted a = rf::adaptiveSweep(sweep, std::ref(solver), {}, [&asked] { return ++asked > 1; });
        QVERIFY(!a.wave.ok());
        QVERIFY2(a.wave.problems.join(' ').startsWith(QStringLiteral("Stopped, ")), qPrintable(a.wave.problems.join(' ')));
        QVERIFY(solver.rounds <= 2);
    }
    {
        Solver solver{resonatorZ};
        solver.failOn = 2;
        const rf::Adapted a = rf::adaptiveSweep(sweep, std::ref(solver));
        QVERIFY(!a.wave.ok());
        QCOMPARE(a.wave.problems, QStringList{QStringLiteral("It failed.")});
    }
}

void TestRf25Adaptive::aCalibrationIsReadBetweenItsFrequencies()
{
    // A feed's line and discontinuity, smooth in frequency, solved at every
    // fourth of the sweep's frequencies.
    const QList<double> sweep = sweepOf(1e9, 41e9, 41);
    const auto at = [](double f) {
        const double w = 2 * M_PI * f;
        const cd gamma(0.4 * std::sqrt(f / 1e9), w * std::sqrt(4.1) / 299792458.0);
        const cd z0(48 + 2e-11 * f, -3 / std::sqrt(f / 1e9));
        const std::array<cd, 4> e{cd(1, 0), cd(0, w * 20e-12), cd(0, w * 10e-15), cd(1, 0)};
        return std::make_tuple(gamma, z0, e);
    };
    rf::Calibration c;
    for (int k = 0; k < sweep.size(); k += 4) {
        const auto [g, z0, e] = at(sweep[k]);
        c.frequencies << sweep[k];
        c.gamma << g;
        c.impedance << z0;
        c.discontinuity << e;
        c.residual << 0.001 * k;
        c.stripAt << 600;
    }
    QVERIFY(c.ok());
    const rf::Calibration all = rf::calibrationAt(c, sweep);
    QCOMPARE(all.frequencies, sweep);
    QCOMPARE(all.gamma.size(), sweep.size());
    double worst = 0;
    for (int k = 0; k < sweep.size(); ++k) {
        const auto [g, z0, e] = at(sweep[k]);
        worst = std::max({worst, std::abs(all.gamma[k] - g) / std::abs(g), std::abs(all.impedance[k] - z0) / std::abs(z0),
                          std::abs(all.discontinuity[k][1] - e[1]) / std::abs(e[1]),
                          std::abs(all.discontinuity[k][2] - e[2]) / std::abs(e[2])});
        QCOMPARE(all.indexOf(sweep[k]), k);
    }
    QVERIFY2(worst < 1e-4, qPrintable(QString::number(worst)));
    // Its strips' residual where not solved: the nearest solved one's.
    QCOMPARE(all.residual.at(1), 0.0);
    QCOMPARE(all.residual.at(3), 0.004);
    // A calibration of every frequency already: as it is.
    const rf::Calibration again = rf::calibrationAt(all, sweep);
    QCOMPARE(again.gamma, all.gamma);
}

void TestRf25Adaptive::reciprocityAndPassivityAreTakenOverEveryFrequency()
{
    rf::FullWave w;
    w.references = {50, 50};
    for (const double f : {1e9, 2e9}) {
        QList<QList<cd>> z = resonatorZ(f);
        if (f > 1.5e9) z[0][1] *= 1.1;   // not reciprocal at the second
        w.frequencies << f;
        w.z << z;
        w.s << rf::scatteringOf(z, w.references);
    }
    w.s[0][0][0] = 1.2;   // gives back at the first
    rf::checkReciprocityAndPassivity(w);
    QVERIFY(w.asymmetry > 0.01);
    QVERIFY(w.gain > 0.1);
    QCOMPARE(w.gainAt, 1e9);
    QCOMPARE(w.solvedGain, w.gain);
}

void TestRf25Adaptive::aValueConvergingIsExtrapolated()
{
    // 2 + 0.3 h^2 on cells halving: order 2, the limit found.
    {
        const QList<double> h{4, 2, 1};
        QList<cd> v;
        for (const double x : h) v << cd(2 + 0.3 * x * x, 0);
        const rf::Convergence c = rf::convergenceOf(h, v);
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::Converging));
        QVERIFY(std::abs(c.order - 2) < 1e-6);
        QVERIFY(std::abs(c.limit - cd(2, 0)) < 1e-9);
        QVERIFY(std::abs(c.error - 0.3) < 1e-9);
        QVERIFY(std::abs(c.ratio - 0.25) < 1e-12);
    }
    // Sizes not halving, a complex value, order 1.5: found as it is.
    {
        const QList<double> h{3, 1.7, 1};
        QList<cd> v;
        for (const double x : h) v << cd(0.5, -0.2) + cd(0.01, 0.02) * std::pow(x, 1.5);
        const rf::Convergence c = rf::convergenceOf(h, v);
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::Converging));
        QVERIFY2(std::abs(c.order - 1.5) < 1e-6, qPrintable(QString::number(c.order)));
        QVERIFY(std::abs(c.limit - cd(0.5, -0.2)) < 1e-9);
    }
    // Four meshes: the last three.
    {
        const QList<double> h{8, 4, 2, 1};
        const QList<cd> v{cd(9, 0), cd(2.64, 0), cd(2.16, 0), cd(2.04, 0)};
        const rf::Convergence c = rf::convergenceOf(h, v);
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::Converging));
        QVERIFY(std::abs(c.limit - cd(2, 0)) < 1e-9);
    }
    // Not moving at all.
    {
        const rf::Convergence c = rf::convergenceOf({4, 2, 1}, {cd(7, 0), cd(7, 0), cd(7, 0)});
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::Settled));
        QCOMPARE(c.limit, cd(7, 0));
    }
}

void TestRf25Adaptive::aValueNotConvergingIsSaid()
{
    // Changes that grow.
    {
        const rf::Convergence c = rf::convergenceOf({4, 2, 1}, {cd(1, 0), cd(1.1, 0), cd(1.3, 0)});
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::NotYet));
        QVERIFY(std::abs(c.ratio - 2) < 1e-9);
        QVERIFY(std::abs(c.slowest - std::sqrt(0.5)) < 1e-9);
        QVERIFY(std::abs(c.error - 0.2) < 1e-9);
        QCOMPARE(c.limit, cd(1.3, 0));
    }
    // Changes that shrink too slowly: under the root of the cells'.
    {
        const rf::Convergence c = rf::convergenceOf({4, 2, 1}, {cd(1, 0), cd(1.1, 0), cd(1.18, 0)});
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::NotYet));
    }
    // Shrinking, but turning back: bounded by the last change.
    {
        const rf::Convergence c = rf::convergenceOf({4, 2, 1}, {cd(1, 0), cd(1.1, 0), cd(1.05, 0)});
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::Oscillating));
        QVERIFY(std::abs(c.error - 0.05) < 1e-9);
    }
    // Two meshes: a change, nothing more.
    {
        const rf::Convergence c = rf::convergenceOf({2, 1}, {cd(1, 0), cd(1.1, 0)});
        QCOMPARE(int(c.kind), int(rf::Convergence::Kind::TwoMeshes));
        QVERIFY(std::abs(c.error - 0.1) < 1e-9);
    }
}

void TestRf25Adaptive::aMeshIsRefinedAsAWhole()
{
    // Every size the settings set divided alike: the cells across and per
    // wavelength, the largest cell and the gaps' share; the rest as it was.
    rf::MeshSettings how;
    how.cellsAcross = 3;
    how.perWavelength = 20;
    how.maximum = 20e-6;
    how.stretch = 4;
    const rf::MeshSettings finer = rf::refinedBy(how, 2);
    QCOMPARE(finer.cellsAcross, 6);
    QCOMPARE(finer.perWavelength, 40);
    QCOMPARE(finer.maximum, 10e-6);
    QCOMPARE(finer.gapShare, 0.125);
    QCOMPARE(finer.stretch, 4.0);
    QCOMPARE(finer.edges, how.edges);
    const rf::MeshSettings coarser = rf::refinedBy(how, 0.25);
    QCOMPARE(coarser.cellsAcross, 1);
    QCOMPARE(coarser.perWavelength, 5);
    QCOMPARE(coarser.maximum, 80e-6);
    QCOMPARE(coarser.gapShare, 1.0);
    // No largest cell, no cells across: none made.
    how.maximum = 0;
    how.cellsAcross = 0;
    QCOMPARE(rf::refinedBy(how, 2).maximum, 0.0);
    QCOMPARE(rf::refinedBy(how, 2).cellsAcross, 0);
}

QTEST_GUILESS_MAIN(TestRf25Adaptive)
#include "test_rf25_adaptive.moc"
