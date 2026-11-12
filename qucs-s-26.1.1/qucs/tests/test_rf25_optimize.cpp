/*
 * test_rf25_optimize.cpp - what an RF 2.5D model's Parametric Sweep and
 *                          Optimization read of a solution (rf25_optimize.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>

#include <cmath>
#include <complex>

#include "rf25/rf25_optimize.h"

namespace rf = qucs_s::rf25;
namespace fem = qucs_s::fem;
namespace optimization = qucs_s::optimization;

namespace {

constexpr double kTau = 100e-12;   // the through path's delay, s

/// A two-port's full wave made up in closed form: S11 0.1, S22 0.2, S21 =
/// S12 = 0.9 e^(-j w tau), Z11 50 + 10j Ohm, at 1, 2 and 3 GHz.
rf::ModelSolution throughLine(double tau = kTau)
{
    auto w = std::make_shared<rf::FullWave>();
    w->frequencies = {1e9, 2e9, 3e9};
    w->portNames = {QStringLiteral("P1"), QStringLiteral("P2")};
    for (double f : std::as_const(w->frequencies)) {
        const std::complex<double> t = 0.9 * std::exp(std::complex<double>(0, -2 * M_PI * f * tau));
        w->s << QList<QList<rf::Complex>>{{0.1, t}, {t, 0.2}};
        w->z << QList<QList<rf::Complex>>{{rf::Complex(50, 10), rf::Complex(5, 0)}, {rf::Complex(5, 0), rf::Complex(50, -10)}};
    }
    rf::ModelSolution s;
    s.wave = w;
    return s;
}

} // namespace

class TestRf25Optimize : public QObject
{
    Q_OBJECT

private slots:
    // A full wave's values by name at each frequency: S in decibels, size,
    // degrees and parts, Z's parts in ohms, the group delay from the phase
    // unwrapped along the sweep, each port's VSWR, f; each pair written
    // S21 and S2_1 alike.
    void aFullWavesValuesByName()
    {
        const rf::SolutionValues v = rf::solutionValues(throughLine(), rf::Model::blank());
        QCOMPARE(v.frequencies, (QList<double>{1e9, 2e9, 3e9}));
        for (const char* name : {"f", "S11_dB", "S21_dB", "S2_1_dB", "S21_mag", "S21_deg", "S21_re", "S21_im", "Z11_re", "Z11_im",
                                 "Z11_mag", "GD21", "VSWR1", "VSWR2"})
            QVERIFY2(v.index(QLatin1String(name)) >= 0, name);
        const auto at = [&v](const char* name, int k) { return v.at.at(k).at(v.index(QLatin1String(name))); };
        QVERIFY(std::abs(at("S21_dB", 1) - 20 * std::log10(0.9)) < 1e-12);
        QCOMPARE(at("S2_1_dB", 1), at("S21_dB", 1));
        QVERIFY(std::abs(at("S21_deg", 0) - -36) < 1e-9);
        QVERIFY(std::abs(at("S21_deg", 2) - -108) < 1e-9);
        for (int k = 0; k < 3; ++k) QVERIFY2(std::abs(at("GD21", k) - kTau) < 1e-18, qPrintable(QString::number(at("GD21", k))));
        QVERIFY(std::abs(at("VSWR1", 0) - 1.1 / 0.9) < 1e-12);
        QVERIFY(std::abs(at("VSWR2", 0) - 1.2 / 0.8) < 1e-12);
        QCOMPARE(at("Z11_re", 0), 50.0);
        QCOMPARE(at("Z22_im", 0), -10.0);
        QCOMPARE(v.dims.at(v.index(QStringLiteral("Z11_re"))), fem::parseUnit(QStringLiteral("Ohm"))->dim);
        QCOMPARE(v.dims.at(v.index(QStringLiteral("GD21"))), fem::parseUnit(QStringLiteral("s"))->dim);
        // A longer delay, its phase wrapping between the frequencies (-108,
        // -216, -324 degrees): the group delay from the phase unwrapped.
        const rf::SolutionValues longer = rf::solutionValues(throughLine(0.3e-9), rf::Model::blank());
        for (int k = 0; k < 3; ++k) {
            const double gd = longer.at.at(k).at(longer.index(QStringLiteral("GD21")));
            QVERIFY2(std::abs(gd - 0.3e-9) < 1e-18, qPrintable(QString::number(gd)));
        }
        // One frequency: no group delay.
        rf::ModelSolution one = throughLine();
        auto w = std::make_shared<rf::FullWave>(*one.wave);
        w->frequencies.resize(1);
        w->s.resize(1);
        w->z.resize(1);
        one.wave = w;
        QVERIFY(rf::solutionValues(one, rf::Model::blank()).index(QStringLiteral("GD21")) < 0);
    }

    // An expression of the values and the parameters over a window of
    // frequencies, judged by its worst (by its goal), its last, its least
    // or most, or between two frequencies at one; what cannot be read
    // refused, with the names there are.
    void readingsJudgeTheirFrequencies()
    {
        const rf::SolutionValues v = rf::solutionValues(throughLine(), rf::Model::blank());
        fem::ParameterScope ps;
        ps.scope.setConstant(QStringLiteral("k"), 2);
        QList<double> fs, ys;
        fem::Dim dim;
        QString why;
        QVERIFY2(rf::seriesOf(v, ps, QStringLiteral("S21_mag * k"), 1.5e9, 3e9, &fs, &ys, &dim, &why), qPrintable(why));
        QCOMPARE(fs, (QList<double>{2e9, 3e9}));
        QVERIFY(std::abs(ys.at(0) - 1.8) < 1e-12);
        QVERIFY2(rf::seriesOf(v, ps, QStringLiteral("f"), 1e9, 2e9, &fs, &ys, &dim, &why), qPrintable(why));
        QCOMPARE(fs.size(), 2);   // (both ends in it)
        QCOMPARE(dim, fem::parseUnit(QStringLiteral("Hz"))->dim);

        const auto read = [&](const QString& expression, rf::Reading::Over over, optimization::GoalType type, double at = NAN) {
            rf::Reading r;
            r.expression = expression;
            r.over = over;
            r.at = at;
            optimization::Goal goal;
            goal.type = type;
            double value = NAN;
            QString bad;
            if (!rf::readingValue(v, ps, r, goal, &value, &dim, &bad)) qWarning("%s", qPrintable(bad));
            return value;
        };
        using Over = rf::Reading::Over;
        using G = optimization::GoalType;
        // A phase falling with f: its worst for MIN the largest (the first),
        // for MAX the least (the last).
        QVERIFY(std::abs(read(QStringLiteral("S21_deg"), Over::Worst, G::Minimize) - -36) < 1e-9);
        QVERIFY(std::abs(read(QStringLiteral("S21_deg"), Over::Worst, G::Maximize) - -108) < 1e-9);
        QVERIFY(std::abs(read(QStringLiteral("S21_deg"), Over::Last, G::Minimize) - -108) < 1e-9);
        QVERIFY(std::abs(read(QStringLiteral("S21_deg"), Over::Least, G::Monitor) - -108) < 1e-9);
        QVERIFY(std::abs(read(QStringLiteral("S21_deg"), Over::Most, G::Monitor) - -36) < 1e-9);
        QVERIFY(std::abs(read(QStringLiteral("S21_deg"), Over::At, G::Monitor, 2.5e9) - -90) < 1e-9);
        QVERIFY(std::abs(read(QStringLiteral("S21_deg"), Over::At, G::Monitor, 2e9) - -72) < 1e-9);
        // Refused: no frequency for At, one outside, a name it has not.
        rf::Reading r;
        r.expression = QStringLiteral("S21_deg");
        r.over = Over::At;
        double value = 0;
        QVERIFY(!rf::readingValue(v, ps, r, optimization::Goal(), &value, &dim, &why));
        QVERIFY2(why.contains(QStringLiteral("none is given")), qPrintable(why));
        r.at = 5e9;
        QVERIFY(!rf::readingValue(v, ps, r, optimization::Goal(), &value, &dim, &why));
        QVERIFY2(why.contains(QStringLiteral("outside the solution's frequencies")), qPrintable(why));
        QVERIFY(!rf::seriesOf(v, ps, QStringLiteral("S31_dB"), NAN, NAN, &fs, &ys, &dim, &why));
        QVERIFY2(why.contains(QStringLiteral("S21_dB")), qPrintable(why));
        QVERIFY(!rf::seriesOf(v, ps, QStringLiteral("S21_dB"), 4e9, 5e9, &fs, &ys, &dim, &why));
        QVERIFY2(why.contains(QStringLiteral("no frequency")), qPrintable(why));

        // The words and frequencies a row is written in.
        rf::Reading::Over over = Over::Last;
        QVERIFY(rf::overOf(QString(), Over::Worst, &over) && over == Over::Worst);
        QVERIFY(rf::overOf(QStringLiteral("max"), Over::Worst, &over) && over == Over::Most);
        QVERIFY(rf::overOf(QStringLiteral("At"), Over::Worst, &over) && over == Over::At);
        QVERIFY(!rf::overOf(QStringLiteral("mean"), Over::Worst, &over));
        double hz = 0;
        QVERIFY(rf::frequencyOf(QStringLiteral("2.4[GHz]"), ps, &hz, &why) && std::abs(hz - 2.4e9) < 1);
        QVERIFY(rf::frequencyOf(QString(), ps, &hz, &why) && std::isnan(hz));
        QVERIFY(!rf::frequencyOf(QStringLiteral("3[m]"), ps, &hz, &why));
    }
};

QTEST_GUILESS_MAIN(TestRf25Optimize)
#include "test_rf25_optimize.moc"
