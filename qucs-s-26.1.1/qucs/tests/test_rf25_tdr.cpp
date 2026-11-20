/*
 * test_rf25_tdr.cpp - time-domain responses of a sweep's S
 * (rf25/rf25_tdr.h): a line with a step in its impedance gives that step at
 * its delay, against the ideal lines' S11 written here; a matched line a
 * flat profile; a transmission's step its delay; and a full-wave line's
 * profile its impedance.
 */
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtTest>

#include "rf25/rf25_tdr.h"
#include "rf25/rf25_fullwave.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_static.h"

#include <cmath>
#include <complex>

namespace rf = qucs_s::rf25;
using Complex = std::complex<double>;

namespace {

const Complex kJ(0, 1);
constexpr double kC = 299792458.0;

/// Where \a v first passes \a level, between the samples of \a t.
double crossing(const QList<double>& t, const QList<double>& v, double level)
{
    for (int i = 1; i < v.size(); ++i)
        if (v.at(i - 1) < level && v.at(i) >= level) return t.at(i - 1) + (t.at(i) - t.at(i - 1)) * (level - v.at(i - 1)) / (v.at(i) - v.at(i - 1));
    return -1;
}

/// An ideal line's input impedance: \a z0, \a length (m) in vacuum,
/// terminated by \a load, at \a f.
Complex inputOf(double z0, double length, Complex load, double f)
{
    const Complex t = std::tan(2 * M_PI * f / kC * length);
    return z0 * (load + kJ * z0 * t) / (z0 + kJ * load * t);
}

} // namespace

class TestRf25Tdr : public QObject
{
    Q_OBJECT

private slots:
    // A 50 Ohm port into 30 mm of 50 Ohm line, then 30 mm of 75, then a
    // matched 75 Ohm load, in vacuum: the profile is 50 Ohm until the round
    // trip to the step (0.2 ns), then 75 within 1 % (75.01), from 400 points
    // to 40 GHz, the step through its half at the round trip within 1 %. And
    // a matched line's flat within 0.5 Ohm.
    void aStepInImpedanceIsAtItsDelay()
    {
        QList<double> fs;
        QList<Complex> s11, matched;
        for (int k = 1; k <= 400; ++k) {
            const double f = k * 0.1e9;
            fs << f;
            const Complex z = inputOf(50, 0.03, inputOf(75, 0.03, 75, f), f);
            s11 << (z - 50.0) / (z + 50.0);
            const Complex zm = inputOf(50, 0.06, 50, f);
            matched << (zm - 50.0) / (zm + 50.0);
        }
        const rf::TimeDomain r = rf::timeDomainOf(fs, s11, true, 50);
        QVERIFY2(r.ok(), qPrintable(r.problems.join(" | ")));
        const double delay = 2 * 0.03 / kC;
        double before = 0, after = 0;
        int nb = 0, na = 0;
        for (int i = 0; i < r.t.size(); ++i) {
            if (r.t.at(i) > 0.3 * delay && r.t.at(i) < 0.7 * delay) {
                before += r.impedance.at(i);
                ++nb;
            }
            if (r.t.at(i) > 1.3 * delay && r.t.at(i) < 1.9 * delay) {
                after += r.impedance.at(i);
                ++na;
            }
        }
        before /= nb;
        after /= na;
        qWarning("a step from 50 to 75 Ohm: the profile %.4g Ohm before it, %.4g after", before, after);
        QVERIFY2(std::abs(before / 50 - 1) < 0.01 && std::abs(after / 75 - 1) < 0.01, qPrintable(QStringLiteral("%1, %2").arg(before).arg(after)));
        // Where it steps: halfway between the two, at the round trip.
        const double at = crossing(r.t, r.step, 0.5 * (75.0 - 50) / (75.0 + 50));
        QVERIFY2(std::abs(at - delay) < 0.01 * delay, qPrintable(QStringLiteral("%1 ns against %2").arg(at * 1e9).arg(delay * 1e9)));
        const rf::TimeDomain flat = rf::timeDomainOf(fs, matched, true, 50);
        double worst = 0;
        for (const double z : flat.impedance) worst = std::max(worst, std::abs(z - 50));
        QVERIFY2(worst < 0.5, qPrintable(QString::number(worst)));
        // Past the step, ringing little: within half an ohm of 75 from 1.3
        // round trips on (the window's; without it, Gibbs's 9 %).
        double ring = 0;
        for (int i = 0; i < r.t.size(); ++i)
            if (r.t.at(i) > 1.3 * delay && r.t.at(i) < 1.9 * delay) ring = std::max(ring, std::abs(r.impedance.at(i) - 75));
        QVERIFY2(ring < 0.5, qPrintable(QString::number(ring)));
        // A 75 Ohm load at the port itself: its reflection's pulse at t nought,
        // half of it before nought - whole in the step, the profile 75 from
        // the first steps on.
        QList<Complex> load;
        for (int k = 0; k < fs.size(); ++k) load << Complex(0.2, 0);
        const rf::TimeDomain at0 = rf::timeDomainOf(fs, load, true, 50);
        for (int i = 5; i < 40; ++i)
            QVERIFY2(std::abs(at0.impedance.at(i) - 75) < 0.1, qPrintable(QStringLiteral("%1 ps: %2 Ohm").arg(at0.t.at(i) * 1e12).arg(at0.impedance.at(i))));
    }

    // A transmission: 60 mm of matched line, S21 = exp(-j k l). Its step
    // rises through a half at the delay, 0.2 ns, to one.
    void aTransmissionsStepIsItsDelay()
    {
        QList<double> fs;
        QList<Complex> s21;
        for (int k = 1; k <= 400; ++k) {
            const double f = k * 0.1e9;
            fs << f;
            s21 << std::exp(-kJ * (2 * M_PI * f / kC * 0.06));
        }
        const rf::TimeDomain t = rf::timeDomainOf(fs, s21, false);
        QVERIFY(t.ok() && t.impedance.isEmpty());
        const double delay = 0.06 / kC, at = crossing(t.t, t.step, 0.5);
        QVERIFY2(std::abs(at - delay) < 0.01 * delay, qPrintable(QStringLiteral("%1 ns").arg(at * 1e9)));
        const int later = int(std::lround(1.5 * delay / (t.t.at(1) - t.t.at(0))));
        QVERIFY(std::abs(t.step.at(later) - 1) < 0.01);
    }

    // A matched delay line's transmission swept coarsely - 45 degrees a
    // step - and resampled four times finer: no echo above the window's
    // sidelobes (the review of 8b91a9ab: straight lines in Re and Im dip to
    // cos 22.5 = 0.92 between the points, and echo at the sweep's step's
    // inverse). And what is said: the phase's steps, a sweep that starts high,
    // a response that folds back.
    void aCoarseSweepHasNoFalseEchoes()
    {
        const double delay = 1e-9, step = 125e6;
        QList<double> fs;
        QList<Complex> s21;
        for (int k = 1; k <= 160; ++k) {
            fs << k * step;
            s21 << std::exp(-kJ * (2 * M_PI * k * step * delay));
        }
        const rf::TimeDomain t = rf::timeDomainOf(fs, s21, false, 50, 6, 640);
        QVERIFY2(t.ok(), qPrintable(t.problems.join(" | ")));
        double peak = 0, echo = 0, echoAt = 0;
        for (int i = 0; i < t.t.size(); ++i) {
            peak = std::max(peak, std::abs(t.impulse.at(i)));
            if (std::abs(t.t.at(i) - delay) > 2e-9 && std::abs(t.impulse.at(i)) > echo) {
                echo = std::abs(t.impulse.at(i));
                echoAt = t.t.at(i);
            }
        }
        qWarning("a coarse sweep's delay line: its largest echo %.2e of the peak, at %.3g ns", echo / peak, echoAt * 1e9);
        // (Kaiser's sidelobes at beta 6 are some 0.6 % of its peak; the chords
        //  in Re and Im gave 1.6 % at 9 ns, the sweep's step's inverse.)
        QVERIFY2(echo < 5e-3 * peak, qPrintable(QStringLiteral("%1 of the peak at %2 ns").arg(echo / peak).arg(echoAt * 1e9)));
        QVERIFY(t.lines.join(QLatin1Char(' ')).contains(QStringLiteral("phase turns by up to 45 degrees")));
        // A line of 25 Ohm in 50, its S11 swept finely - 3.6 degrees of the
        // line a step - through its nulls, where its phase flips by nearly
        // 180 degrees however fine the sweep: not said (P37 of the open
        // items: it said "178 degrees").
        {
            const double z = 25, r0 = 50, length = 0.1037e-9;   // (its one-way delay: its nulls off the sweep's points)
            QList<double> f1;
            QList<Complex> s11;
            for (int k = 1; k <= 200; ++k) {
                const double f = k * 0.1e9;
                const Complex t = kJ * std::tan(2 * M_PI * f * length);
                const Complex zin = z * (r0 + z * t) / (z + r0 * t);
                f1 << f;
                s11 << (zin - r0) / (zin + r0);
            }
            const rf::TimeDomain line = rf::timeDomainOf(f1, s11, true, 50, 6, 800);
            QVERIFY2(line.ok(), qPrintable(line.problems.join(" | ")));
            QVERIFY2(!line.lines.join(' ').contains(QStringLiteral("phase turns")), qPrintable(line.lines.join(" | ")));
        }
        // A sweep from 1 GHz on a grid of 31.25 MHz: said.
        QList<double> high;
        QList<Complex> sHigh;
        for (int k = 0; k < 20; ++k) {
            high << 1e9 + k * 0.5e9;
            sHigh << Complex(0.1, 0);
        }
        const rf::TimeDomain h = rf::timeDomainOf(high, sHigh, true, 50, 6, 320);
        QVERIFY2(h.lines.join(QLatin1Char(' ')).contains(QStringLiteral("starts at 1 GHz")), qPrintable(h.lines.join(" | ")));
        // A delay past the window's half (8 ns of 16 at 62.5 MHz): said.
        QList<double> fine;
        QList<Complex> late;
        for (int k = 1; k <= 160; ++k) {
            fine << k * 62.5e6;
            late << std::exp(-kJ * (2 * M_PI * k * 62.5e6 * 7.8e-9));
        }
        const rf::TimeDomain f = rf::timeDomainOf(fine, late, false);
        QVERIFY2(f.lines.join(QLatin1Char(' ')).contains(QStringLiteral("folds back")), qPrintable(f.lines.join(" | ")));
        QVERIFY(!t.lines.join(QLatin1Char(' ')).contains(QStringLiteral("folds back")));
    }

    // A full-wave line: 5 mm of 100 µm on 100 µm of eps_r 9.8, edge ports of
    // 50 Ohm, 100 frequencies to 20 GHz. Halfway along its round trip its
    // profile is its own impedance - the quasi-static solve's - within 2 %.
    void aFullWaveLinesProfileIsItsImpedance()
    {
        rf::Stack st;
        rf::Layer ground, sub, metal;
        ground.kind = rf::Layer::Ground;
        ground.name = QStringLiteral("GND");
        ground.tag = QStringLiteral("gnd");
        sub.kind = rf::Layer::Substrate;
        sub.name = QStringLiteral("SUB");
        sub.tag = QStringLiteral("sub");
        sub.thickness = 100e-6;
        sub.epsr = 9.8;
        metal.kind = rf::Layer::Metal;
        metal.name = QStringLiteral("M1");
        metal.tag = QStringLiteral("m1");
        st.layers << ground << sub << metal;
        rf::Conductor line;
        line.layer = QStringLiteral("M1");
        line.outline = QPolygonF(QRectF(0, 0, 5000, 100));
        rf::MeshSettings how;
        how.maximum = 100e-6;
        how.cellsAcross = 4;
        how.stretch = 4;
        const rf::Mesh mesh = rf::meshOf({line}, how, rf::Stack(), 0);
        rf::FullWavePort a, b;
        a.at = QPointF(0, 50);
        a.metal = QStringLiteral("M1");
        a.facing = QPointF(-1, 0);
        b = a;
        b.at = QPointF(5000, 50);
        b.facing = QPointF(1, 0);
        QList<double> fs;
        for (int k = 1; k <= 100; ++k) fs << k * 0.2e9;
        const rf::FullWave w = rf::fullWaveOf(mesh, {line}, st, {a, b}, fs);
        QVERIFY2(w.ok(), qPrintable(w.problems.join(" | ")));
        QList<Complex> s11;
        for (int k = 0; k < fs.size(); ++k) s11 << w.s.at(k).at(0).at(0);
        const rf::TimeDomain r = rf::timeDomainOf(fs, s11, true, 50);
        QVERIFY2(r.ok(), qPrintable(r.problems.join(" | ")));
        const rf::Extraction e = rf::extract(mesh, {line}, st);
        const rf::LineParameters lp = rf::lineParameters(e, 0, 5e-3, 1e9);
        const double trip = 2 * 5e-3 * std::sqrt(lp.epsilonEff) / kC;
        double mid = 0;
        int n = 0;
        for (int i = 0; i < r.t.size(); ++i)
            if (r.t.at(i) > 0.4 * trip && r.t.at(i) < 0.6 * trip) {
                mid += r.impedance.at(i);
                ++n;
            }
        mid /= n;
        qWarning("a full-wave line's profile halfway: %.4g Ohm, its quasi-static Z0 %.4g", mid, lp.z0);
        QVERIFY2(std::abs(mid / lp.z0 - 1) < 0.02, qPrintable(QStringLiteral("%1 Ohm against %2").arg(mid).arg(lp.z0)));
    }

    // A 50 Ohm line into 75 Ohm a nanosecond's round trip away, S11 = 0.2
    // exp(-j w 1 ns), swept every 50 MHz to 10 GHz from 25 MHz, 0.3, 1, 2 and
    // 5 GHz: S at nought 0.2 throughout, and the profile 50 Ohm before the
    // nanosecond and 75 after it. From 1 GHz the lowest three's quadratic gave
    // -3.56, from 5 GHz -97, and Z(t) nought throughout, said as fine (a bug hunt's second hour). And a sweep whose phase carried
    // to nought lands off the real axis: said.
    void aSweepFromHighHasItsLevel()
    {
        for (const double start : {25e6, 0.3e9, 1e9, 2e9, 5e9}) {
            QList<double> fs;
            QList<Complex> s11;
            for (double f = start; f <= 10e9 + 1; f += 50e6) {
                fs << f;
                s11 << 0.2 * std::exp(-kJ * (2 * M_PI * f * 1e-9));
            }
            const rf::TimeDomain r = rf::timeDomainOf(fs, s11, true, 50, 6, 1000);
            QVERIFY2(r.ok(), qPrintable(r.problems.join(" | ")));
            double before = 0, after = 0;
            int nb = 0, na = 0;
            for (int i = 0; i < r.t.size(); ++i) {
                if (r.t.at(i) > 0.3e-9 && r.t.at(i) < 0.7e-9) {
                    before += r.impedance.at(i);
                    ++nb;
                }
                if (r.t.at(i) > 1.3e-9 && r.t.at(i) < 1.7e-9) {
                    after += r.impedance.at(i);
                    ++na;
                }
            }
            before /= nb;
            after /= na;
            qWarning("swept from %.3g GHz: the profile %.4g Ohm before the step, %.4g after", start / 1e9, before, after);
            QVERIFY2(std::abs(before / 50 - 1) < 0.01 && std::abs(after / 75 - 1) < 0.01,
                     qPrintable(QStringLiteral("from %1 GHz: %2, %3 | %4").arg(start / 1e9).arg(before).arg(after).arg(r.lines.join(" | "))));
            QVERIFY2(r.lines.join(' ').contains(QRegularExpression(QStringLiteral("S at nought 0\\.(2|200\\d|199\\d),"))),
                     qPrintable(r.lines.join(" | ")));
        }
        // The phase carried to nought 90 degrees off the real axis: S11 =
        // 0.2 j exp(-j w 1 ns), which no port's S is. Said, the sign a guess.
        QList<double> fs;
        QList<Complex> odd;
        for (double f = 1e9; f <= 10e9 + 1; f += 50e6) {
            fs << f;
            odd << 0.2 * kJ * std::exp(-kJ * (2 * M_PI * f * 1e-9));
        }
        const rf::TimeDomain r = rf::timeDomainOf(fs, odd, true, 50, 6, 1000);
        QVERIFY2(r.lines.join(' ').contains(QStringLiteral("is a guess")), qPrintable(r.lines.join(" | ")));
    }

    // The FFT against the plain sum it replaces, on a grid of 1000 points
    // (2000 a transform: Bluestein's) and of 1024 (radix two): the same
    // impulse to 1e-9 of its peak. And 2^20 points to 10 GHz in a quarter of a
    // second, where the sum's 16,000 took 3.5 s; past 2^20, refused.
    void theTransformIsTheSum()
    {
        QList<double> fs;
        QList<Complex> s21;
        for (int k = 1; k <= 200; ++k) {
            fs << k * 50e6;
            s21 << std::exp(-kJ * (2 * M_PI * k * 50e6 * 0.7e-9)) * (1.0 - 0.02 * k / 200.0);
        }
        for (const int m : {1000, 1024}) {
            const rf::TimeDomain t = rf::timeDomainOf(fs, s21, false, 50, 6, m);
            QVERIFY2(t.ok(), qPrintable(t.problems.join(" | ")));
            // (The grid's S as the transform takes it: the same function's, at
            //  points m - its S at nought the quadratic's, its window
            //  Kaiser's.)
            const double df = t.df, dt = t.t.at(1) - t.t.at(0);
            QCOMPARE(t.t.size(), m);
            double peak = 0, worst = 0;
            for (const double h : t.impulse) peak = std::max(peak, std::abs(h));
            // The sum at a few of the times, S on the grid from the sweep's
            // own points (every 50 MHz, each a grid point where df divides it),
            // the window Kaiser's of beta 6.
            const auto i0 = [](double x) {
                double sum = 1, term = 1;
                for (int k = 1; k < 200; ++k) {
                    term *= (x / (2 * k)) * (x / (2 * k));
                    sum += term;
                }
                return sum;
            };
            // (S at nought the lowest three's quadratic in their real parts:
            //  the phase turns 12.6 degrees below the first point.)
            const double q = s21.at(0).real() * 3 - s21.at(1).real() * 3 + s21.at(2).real();
            const auto sOn = [&](double f) -> Complex {
                if (f < 50e6 - 1) return Complex(q, 0) + (s21.at(0) - Complex(q, 0)) * (f / 50e6);
                const int hi = std::min(int(std::ceil(f / 50e6 - 1e-9)), 200) - 1;
                if (hi <= 0) return s21.at(0);
                const double u = (f - fs.at(hi - 1)) / 50e6;
                const Complex lo = s21.at(hi - 1), up = s21.at(hi);
                return std::polar(std::abs(lo) * (1 - u) + std::abs(up) * u, std::arg(lo) + std::arg(up / lo) * u);
            };
            for (const int i : {0, 7, 13, m / 3, m / 2, m - 1}) {
                Complex sum = sOn(0);
                for (int k = 1; k <= m; ++k) {
                    const double x = double(k) / m;
                    const double w = i0(6 * std::sqrt(std::max(0.0, 1 - x * x))) / i0(6);
                    sum += 2.0 * sOn(k * df) * w * std::exp(Complex(0, 2 * M_PI * k * df * i * dt));
                }
                worst = std::max(worst, std::abs(df * sum.real() - t.impulse.at(i)));
            }
            QVERIFY2(worst < 1e-9 * peak, qPrintable(QStringLiteral("%1 points: %2 of the peak").arg(m).arg(worst / peak)));
        }
        QElapsedTimer clock;
        clock.start();
        const rf::TimeDomain big = rf::timeDomainOf(fs, s21, false, 50, 6, 1 << 20);
        QVERIFY2(big.ok() && big.t.size() == (1 << 20), qPrintable(big.problems.join(" | ")));
        qWarning("2^20 points: %lld ms", qint64(clock.elapsed()));
        QVERIFY(!rf::timeDomainOf(fs, s21, false, 50, 6, (1 << 20) + 1).ok());
    }

    // What it refuses: too few points, frequencies not ascending, a sweep
    // from nought.
    void whatItRefuses()
    {
        QVERIFY(!rf::timeDomainOf({1e9, 2e9}, {0.1, 0.1}, true).ok());
        QVERIFY(!rf::timeDomainOf({1e9, 3e9, 2e9, 4e9}, {0.1, 0.1, 0.1, 0.1}, true).ok());
        QVERIFY(!rf::timeDomainOf({0, 1e9, 2e9, 3e9}, {0.1, 0.1, 0.1, 0.1}, true).ok());
    }
};

QTEST_GUILESS_MAIN(TestRf25Tdr)
#include "test_rf25_tdr.moc"
