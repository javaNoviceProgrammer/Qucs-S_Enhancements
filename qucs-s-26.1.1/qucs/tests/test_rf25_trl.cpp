/*
 * test_rf25_trl.cpp - RF 2.5D's multiline TRL calibration (rf25/rf25_trl.h,
 * F3 of the open items of 8 November) on fixtures made up in closed form: a
 * device behind two error boxes unlike each other, the standards measured
 * through them, and the device found again.
 */
#include <QtTest>

#include "rf25/rf25_trl.h"

#include <Eigen/Dense>

#include <cmath>
#include <random>

namespace rf = qucs_s::rf25;
using cd = std::complex<double>;

namespace {

constexpr double kLight = 299792458.0;

rf::S2 cascade(const rf::S2& a, const rf::S2& b)
{
    const auto ta = rf::cascadeOf(a), tb = rf::cascadeOf(b);
    Eigen::Matrix2cd ma, mb;
    ma << ta[0], ta[1], ta[2], ta[3];
    mb << tb[0], tb[1], tb[2], tb[3];
    const Eigen::Matrix2cd m = ma * mb;
    return rf::scatteringOf({m(0, 0), m(0, 1), m(1, 0), m(1, 1)});
}

rf::S2 lineOf(cd gamma, double l)
{
    const cd t = std::exp(-gamma * l);
    return {0.0, t, t, 0.0};
}

/// A fixture as a pad would be: a little reflection each way, a lossy
/// transmission with a phase; \a seed makes it its own.
rf::S2 fixture(unsigned seed, double f)
{
    std::mt19937 g(seed);
    std::uniform_real_distribution<double> u(-1, 1);
    const double w = 2 * M_PI * f;
    const cd s11(0.15 + 0.05 * u(g), 0.1 * u(g)), s22(0.1 * u(g), 0.12 + 0.05 * u(g));
    const cd s21 = 0.9 * std::exp(cd(0, -w * (30 + 10 * u(g)) * 1e-12));
    return {s11 * cd(1, w * 2e-12), s21, s21, s22 * cd(1, -w * 1e-12)};
}

/// A device: a little filter, its S at \a f.
rf::S2 device(double f)
{
    const double w = 2 * M_PI * f;
    const cd y = cd(0.002, w * 80e-15), z = cd(1.5, w * 300e-12);
    // A series Z then a shunt Y, in 50 Ohm.
    Eigen::Matrix2cd abcd1, abcd2;
    abcd1 << 1.0, z, 0.0, 1.0;
    abcd2 << 1.0, 0.0, y, 1.0;
    const Eigen::Matrix2cd m = abcd1 * abcd2;
    const cd a = m(0, 0), b = m(0, 1) / 50.0, c = m(1, 0) * 50.0, d = m(1, 1);
    const cd den = a + b + c + d;
    return {(a + b - c - d) / den, 2.0 * (a * d - b * c) / den, 2.0 / den, (-a + b - c + d) / den};
}

cd gammaAt(double f)
{
    return cd(4.0 * std::sqrt(f / 1e10), 2 * M_PI * f * std::sqrt(6.5) / kLight);
}

/// The standards measured through the two fixtures; \a reflect at both.
rf::TrlStandards standards(const QList<double>& freqs, const QList<double>& longer, cd reflect, bool isShort)
{
    rf::TrlStandards st;
    st.frequencies = freqs;
    st.longer = longer;
    st.shortReflect = isShort;
    st.lines = QList<QList<rf::S2>>(longer.size());
    for (const double f : freqs) {
        const rf::S2 a = fixture(1, f), b = fixture(2, f);
        st.thru << cascade(a, b);
        for (int i = 0; i < longer.size(); ++i) st.lines[i] << cascade(cascade(a, lineOf(gammaAt(f), longer.at(i))), b);
        // Each end's reflect seen through its fixture: a's port 1 and b's
        // port 2.
        const cd r1 = a[0] + a[1] * a[2] * reflect / (1.0 - a[3] * reflect);
        const cd r2 = b[3] + b[1] * b[2] * reflect / (1.0 - b[0] * reflect);
        st.reflect << rf::S2{r1, 0.0, 0.0, r2};
    }
    return st;
}

QList<double> sweep(double from, double to, int n)
{
    QList<double> f;
    for (int k = 0; k < n; ++k) f << from + (to - from) * k / (n - 1);
    return f;
}

double worstOff(const QList<rf::S2>& a, const QList<rf::S2>& b)
{
    double worst = 0;
    for (int k = 0; k < a.size(); ++k)
        for (int t = 0; t < 4; ++t) worst = std::max(worst, std::abs(a.at(k)[t] - b.at(k)[t]));
    return worst;
}

} // namespace

class TestRf25Trl : public QObject
{
    Q_OBJECT

private slots:
    // Two fixtures unlike each other, two lines and an open: the device
    // behind them found again to rounding, the lines' gamma with it, and
    // what it did said - the low frequencies, where neither line's phase is
    // far from 0, said to be poorly told.
    void aDeviceBehindTwoFixturesIsFoundAgain()
    {
        const QList<double> f = sweep(1e9, 40e9, 40);
        const rf::TrlStandards st = standards(f, {0.5e-3, 2e-3}, cd(0.95, -0.15), false);
        const rf::TrlCalibration c = rf::calibrateTrl(st, 6.5);
        QVERIFY2(c.ok(), qPrintable(c.problems.join(" | ")));
        QList<rf::S2> measured, truth;
        for (const double x : f) {
            measured << cascade(cascade(fixture(1, x), device(x)), fixture(2, x));
            truth << device(x);
        }
        const double off = worstOff(rf::corrected(c, measured), truth);
        QVERIFY2(off < 1e-8, qPrintable(QString::number(off)));
        for (int k = 0; k < f.size(); ++k)
            QVERIFY2(std::abs(c.gamma.at(k) - gammaAt(f.at(k))) < 1e-6 * std::abs(gammaAt(f.at(k))),
                     qPrintable(QStringLiteral("%1 GHz").arg(f.at(k) / 1e9)));
        QVERIFY(std::abs(c.reflect.first() - cd(0.95, -0.15)) < 1e-8);
        QVERIFY2(c.lines.join(' ').contains(QStringLiteral("effective permittivity 6.5")), qPrintable(c.lines.join(" | ")));
        QVERIFY2(c.warnings.join(' ').contains(QStringLiteral("hardly differ")), qPrintable(c.warnings.join(" | ")));
        // (The lines not taken, corrected, are matched lines.)
        for (const double r : c.residual) QVERIFY(r < 1e-8);
    }

    // A line half a wave longer than the thru at 20 GHz tells nothing
    // there; with a second, each frequency takes the better - at 20 GHz the
    // second - and the device is found all the same. With that line alone
    // its phase there tells nothing, and that is said.
    void eachFrequencyTakesTheLineThatTellsBest()
    {
        const double half = M_PI / gammaAt(20e9).imag();
        const QList<double> f = sweep(10e9, 30e9, 21);
        const rf::TrlStandards both = standards(f, {half, 0.37 * half}, cd(0.97, 0.05), false);
        const rf::TrlCalibration c = rf::calibrateTrl(both, 6.5);
        QVERIFY2(c.ok(), qPrintable(c.problems.join(" | ")));
        const int at = int(f.indexOf(20e9));
        QVERIFY(at >= 0);
        QCOMPARE(c.line.at(at), 1);
        QList<rf::S2> measured, truth;
        for (const double x : f) {
            measured << cascade(cascade(fixture(1, x), device(x)), fixture(2, x));
            truth << device(x);
        }
        QVERIFY(worstOff(rf::corrected(c, measured), truth) < 1e-7);
        const rf::TrlCalibration alone = rf::calibrateTrl(standards(f, {half}, cd(0.97, 0.05), false), 6.5);
        QVERIFY(alone.ok());
        // (Alone, its phase tells nothing there - only the loss tells its two
        // eigenvalues apart, which a solve's noise would not leave - and said.)
        QVERIFY2(alone.phase.at(at) < 1e-6 && c.phase.at(at) > 0.5, qPrintable(QStringLiteral("%1, %2").arg(alone.phase.at(at)).arg(c.phase.at(at))));
        QVERIFY(alone.warnings.join(' ').contains(QStringLiteral("hardly differ")));
    }

    // A short for the reflect: its sign told by it, the device as well.
    void aShortReflectIsTold()
    {
        const QList<double> f = sweep(2e9, 30e9, 15);
        const rf::TrlStandards st = standards(f, {1e-3}, cd(-0.96, 0.1), true);
        const rf::TrlCalibration c = rf::calibrateTrl(st, 6.5);
        QVERIFY(c.ok());
        QVERIFY(std::abs(c.reflect.first() - cd(-0.96, 0.1)) < 1e-8);
        QList<rf::S2> measured, truth;
        for (const double x : f) {
            measured << cascade(cascade(fixture(1, x), device(x)), fixture(2, x));
            truth << device(x);
        }
        QVERIFY(worstOff(rf::corrected(c, measured), truth) < 1e-7);
        // Taken for an open, the sign the other way: not the device.
        rf::TrlStandards wrong = st;
        wrong.shortReflect = false;
        QVERIFY(!(worstOff(rf::corrected(rf::calibrateTrl(wrong, 6.5), measured), truth) < 1e-3));
    }

    // S in one impedance and in another, and back.
    void anSIsRenormalized()
    {
        const rf::S2 s = device(5e9);
        const rf::S2 back = rf::renormalized(rf::renormalized(s, 50, 35), 35, 50);
        for (int t = 0; t < 4; ++t) QVERIFY(std::abs(back[t] - s[t]) < 1e-12);
        // A matched 35 Ohm line is no longer matched in 50.
        const rf::S2 line{0.0, cd(0, -1), cd(0, -1), 0.0};
        QVERIFY(std::abs(rf::renormalized(line, 35, 50)[0]) > 0.1);
    }

    void whatCannotBeCalibratedIsSaid()
    {
        rf::TrlStandards st = standards({1e9, 2e9}, {1e-3}, cd(0.9, 0), false);
        st.longer = {0};
        QVERIFY(rf::calibrateTrl(st).problems.join(' ').contains(QStringLiteral("no longer than the thru")));
        st.lines.clear();
        st.longer.clear();
        QVERIFY(rf::calibrateTrl(st).problems.join(' ').contains(QStringLiteral("a line at least")));
    }
};

QTEST_MAIN(TestRf25Trl)
#include "test_rf25_trl.moc"
