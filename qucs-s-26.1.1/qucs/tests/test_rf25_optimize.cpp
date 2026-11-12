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

    // A Parametric Sweep's plan: one or two parameters over a list, or From
    // to To in Points (linear, logarithmic), crossed or paired; the
    // quantities, each with its window and Over (not worst: a sweep has no
    // goal); what cannot be run refused, and why. A point's model is the
    // model with those parameters; its result kept as JSON, read back the
    // same; what it is of changes with the model, not with the parameters it
    // varies or what it found.
    void aParametricSweepsPlan()
    {
        rf::Model m = rf::Model::blank();
        m.root.child(QStringLiteral("rf_parameters"))->props.insert(QStringLiteral("rows"),
            QJsonArray{QJsonArray{"w", "50[um]"}, QJsonArray{"L", "1[mm]"}, QJsonArray{"er", "9.8"}});
        QVERIFY(m.add(m.root.tag, m.make(QStringLiteral("rf_analyses"))) != nullptr);
        rf::Node sweep = m.make(QStringLiteral("rf_sweep"));
        sweep.props = QJsonObject{{"parameters", QJsonArray{QJsonArray{"w", "10[um]", "30[um]", "3"}, QJsonArray{"L", "", "", "", "", "1[mm], 2[mm]"}}},
                                  {"quantities", QJsonArray{QJsonArray{"Z0"}, QJsonArray{"S21_dB", "min", "1[GHz]", "2[GHz]"}, QJsonArray{"S11_dB", "at", "", "", "1.5[GHz]"}}}};
        const rf::Node* added = m.add(m.root.child(QStringLiteral("rf_analyses"))->tag, sweep);
        QVERIFY(added != nullptr);
        const QString tag = added->tag;
        QCOMPARE(rf::sweepNode(m), m.find(tag));
        QString why;
        std::optional<rf::SweepPlan> plan = rf::sweepPlan(m, *m.find(tag), &why);
        QVERIFY2(plan.has_value(), qPrintable(why));
        QCOMPARE(plan->axes.size(), 2);
        QCOMPARE(plan->axes.at(0).values.size(), 3);
        QVERIFY(std::abs(plan->axes.at(0).values.at(1) - 20e-6) < 1e-15);
        QCOMPARE(plan->axes.at(1).values, (QList<double>{1e-3, 2e-3}));
        QCOMPARE(plan->points().size(), 6);   // (crossed)
        QCOMPARE(plan->points().at(1), (QList<double>{plan->axes.at(0).values.at(1), 1e-3}));
        QCOMPARE(plan->quantities.size(), 3);
        QVERIFY(plan->quantities.at(0).over == rf::Reading::Over::Last);
        QVERIFY(plan->quantities.at(1).over == rf::Reading::Over::Least && plan->quantities.at(1).from == 1e9 && plan->quantities.at(1).to == 2e9);
        QVERIFY(plan->quantities.at(2).over == rf::Reading::Over::At && plan->quantities.at(2).at == 1.5e9);
        QCOMPARE(plan->parallel, 1);
        // Logarithmic; paired.
        const auto withProps = [&](const QJsonObject& props) {
            rf::Model c = m;
            c.find(tag)->props = props;
            QString bad;
            const std::optional<rf::SweepPlan> p = rf::sweepPlan(c, *c.find(tag), &bad);
            why = bad;
            return p;
        };
        plan = withProps({{"parameters", (QJsonArray() << QJsonArray{"er", "1", "100", "3", "log"})}, {"quantities", (QJsonArray() << QJsonArray{"C11"})}});
        QVERIFY2(plan.has_value(), qPrintable(why));
        QVERIFY(std::abs(plan->axes.at(0).values.at(1) - 10) < 1e-9);
        plan = withProps({{"parameters", QJsonArray{QJsonArray{"w", "", "", "", "", "10[um], 20[um]"}, QJsonArray{"L", "", "", "", "", "1[mm], 2[mm]"}}},
                          {"combine", "paired"}, {"quantities", (QJsonArray() << QJsonArray{"C11"})}});
        QVERIFY2(plan.has_value(), qPrintable(why));
        QCOMPARE(plan->points().size(), 2);
        // Refused, each said.
        const auto refused = [&](const QJsonObject& props, const char* said) {
            const bool no = !withProps(props).has_value();
            if (!no || !why.contains(QLatin1String(said))) qWarning("%s: %s", said, qPrintable(why));
            return no && why.contains(QLatin1String(said));
        };
        const QJsonArray q = QJsonArray() << QJsonArray{"C11"};
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"h", "1", "2"})}, {"quantities", q}}, "no parameter h"));
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"w", "1[um]", "2[um]"})}}, "reads nothing"));
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"w", "1[um]", "2[um]"})}, {"quantities", (QJsonArray() << QJsonArray{"C11", "worst"})}}, "last, min, max or at"));
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"w", "0", "2[um]", "3", "log"})}, {"quantities", q}}, "above nought"));
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"w", "1[um]"})}, {"quantities", q}}, "a From and a To"));
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"w", "1[V]", "2[um]"})}, {"quantities", q}}, "From"));
        QVERIFY(refused({{"parameters", QJsonArray{QJsonArray{"w", "", "", "", "", "1[um], 2[um]"}, QJsonArray{"L", "", "", "", "", "1[mm]"}}},
                         {"combine", "paired"}, {"quantities", q}}, "as many values"));
        QVERIFY(refused({{"parameters", QJsonArray{QJsonArray{"w", "1[um]", "2[um]"}, QJsonArray{"L", "1[mm]", "2[mm]"}, QJsonArray{"er", "1", "2"}}}, {"quantities", q}}, "one or two"));
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"w", "1[um]", "2[um]"})}, {"quantities", (QJsonArray() << QJsonArray{"C11", "at"})}}, "give it in At"));
        QVERIFY(refused({{"parameters", (QJsonArray() << QJsonArray{"w", "1[um]", "2[um]"})}, {"quantities", q}, {"parallel", 0}}, "Solves at once"));

        // A point's model: the parameters it varies, as SI values in their units.
        const rf::Model point = rf::withParameters(m, {{"w", rf::siText(20e-6, fem::parseUnit(QStringLiteral("m"))->dim)}});
        QVERIFY2(point.parameters().at(0).expression.endsWith(QStringLiteral("[m]")), qPrintable(point.parameters().at(0).expression));
        QVERIFY(std::abs(fem::evaluateParameters(point.parameters())->values.value(QStringLiteral("w")) - 20e-6) < 1e-18);
        QCOMPARE(point.parameters().at(1).expression, QStringLiteral("1[mm]"));

        // Kept as JSON and read back.
        rf::SweepResult r;
        r.columns = {QStringLiteral("w"), QStringLiteral("S21_dB")};
        r.units = {QStringLiteral("m"), QString()};
        r.parameters = 1;
        r.solved = 1;
        r.failed = 1;
        rf::SweepPoint good;
        good.values = {10e-6};
        good.measured = {-0.5};
        good.solved = true;
        good.frequencies = {1e9};
        good.portNames = {QStringLiteral("P1"), QStringLiteral("P2")};
        good.references = {50, 50};
        good.s = {{{rf::Complex(0.1, 0.2), rf::Complex(0.9, -0.1)}, {rf::Complex(0.9, -0.1), rf::Complex(0.1, 0.2)}}};
        good.z = good.s;
        rf::SweepPoint bad;
        bad.values = {20e-6};
        bad.measured = {NAN};
        bad.why = QStringLiteral("no mesh");
        r.points = {good, bad};
        const rf::SweepResult back = rf::SweepResult::fromJson(r.toJson());
        QCOMPARE(back.columns, r.columns);
        QCOMPARE(back.points.size(), 2);
        QCOMPARE(back.points.at(0).measured, good.measured);
        QCOMPARE(back.points.at(0).s, good.s);
        QVERIFY(std::isnan(back.points.at(1).measured.at(0)));
        QCOMPARE(back.points.at(1).why, QStringLiteral("no mesh"));
        QVERIFY(back.waveOf(0) != nullptr && back.waveOf(0)->s == good.s);
        QVERIFY(back.waveOf(1) == nullptr);

        // What a result is of: not its own parameters, nor its result; the rest.
        const QString setup = rf::analysisSetup(m, *m.find(tag));
        rf::Model varied = rf::withParameters(m, {{"w", "99[um]"}});
        QCOMPARE(rf::analysisSetup(varied, *varied.find(tag)), setup);
        varied.find(tag)->props.insert(QStringLiteral("result"), r.toJson());
        QCOMPARE(rf::analysisSetup(varied, *varied.find(tag)), setup);
        rf::Model other = rf::withParameters(m, {{"er", "4.4"}});
        QVERIFY(rf::analysisSetup(other, *other.find(tag)) != setup);
    }

    // An Optimization's kind: the multiphysics one's settings, their defaults
    // and help, but for its study, and its objectives' own columns. Its plan:
    // read by the shared pieces, the kind's defaults where the node says
    // nothing, each objective's Reading from its Over, From, To and At; what
    // cannot be run refused, as a multiphysics plan's is.
    void anOptimizationsPlan()
    {
        const qucs_s::fem::NodeKind* kind = rf::nodeKind(QStringLiteral("rf_optimization"));
        QVERIFY(kind != nullptr);
        QVERIFY(kind->property(QStringLiteral("study")) == nullptr);
        for (const char* key : {"method", "maxiter", "tol", "seed", "polish", "starts", "constraints", "strategy", "parallel", "variables", "objectives", "variation"})
            QVERIFY2(kind->property(QLatin1String(key)) != nullptr, key);
        QCOMPARE(kind->property(QStringLiteral("maxiter"))->defaultValue, qucs_s::fem::nodeKind(QStringLiteral("optimization"))->property(QStringLiteral("maxiter"))->defaultValue);
        QCOMPARE(kind->property(QStringLiteral("objectives"))->choiceLabels.at(5), QStringLiteral("From"));

        rf::Model m = rf::Model::blank();
        m.root.child(QStringLiteral("rf_parameters"))->props.insert(QStringLiteral("rows"), QJsonArray{QJsonArray{"w", "50[um]"}, QJsonArray{"L", "1[mm]"}});
        QVERIFY(m.add(m.root.tag, m.make(QStringLiteral("rf_analyses"))) != nullptr);
        rf::Node opt = m.make(QStringLiteral("rf_optimization"));
        opt.props = QJsonObject{{"variables", QJsonArray() << QJsonArray{"w", "", "10[um]", "200[um]"}},
                                {"objectives", QJsonArray{QJsonArray{"Z0", "TARGET", "50[Ohm]"}, QJsonArray{"S21_dB", "MAX", "", "2", "min", "1[GHz]", "2[GHz]"},
                                                          QJsonArray{"GD21", "LE", "1[ns]", "", "at", "", "", "1.5[GHz]"}}}};
        const rf::Node* added = m.add(m.root.child(QStringLiteral("rf_analyses"))->tag, opt);
        QVERIFY(added != nullptr);
        const QString tag = added->tag;
        QCOMPARE(rf::optimizationNode(m), m.find(tag));
        QString why;
        std::optional<rf::OptimizationPlan> plan = rf::optimizationPlan(m, *m.find(tag), &why);
        QVERIFY2(plan.has_value(), qPrintable(why));
        QCOMPARE(plan->plan.search.maxIter, kind->property(QStringLiteral("maxiter"))->defaultValue.toInt());
        QCOMPARE(int(plan->plan.variables.size()), 1);
        QCOMPARE(int(plan->plan.objectives.size()), 3);
        QCOMPARE(plan->readings.size(), 3);
        QVERIFY(plan->readings.at(0).over == rf::Reading::Over::Worst);
        QVERIFY(plan->readings.at(1).over == rf::Reading::Over::Least && plan->readings.at(1).from == 1e9 && plan->readings.at(1).to == 2e9);
        QCOMPARE(plan->plan.objectives[1].weight, 2.0);
        QVERIFY(plan->readings.at(2).over == rf::Reading::Over::At && plan->readings.at(2).at == 1.5e9);
        // Refused.
        const auto refused = [&](const QJsonObject& props, const char* said) {
            rf::Model c = m;
            c.find(tag)->props = props;
            const bool no = !rf::optimizationPlan(c, *c.find(tag), &why).has_value();
            if (!no || !why.contains(QLatin1String(said))) qWarning("%s: %s", said, qPrintable(why));
            return no && why.contains(QLatin1String(said));
        };
        const QJsonArray w = QJsonArray() << QJsonArray{"w", "", "10[um]", "200[um]"};
        QVERIFY(refused({{"variables", w}, {"objectives", QJsonArray() << QJsonArray{"Z0", "MIN", "", "", "sometimes"}}}, "worst, last, min, max or at"));
        QVERIFY(refused({{"variables", w}, {"objectives", QJsonArray() << QJsonArray{"Z0", "MIN", "", "", "at"}}}, "give it in At"));
        QVERIFY(refused({{"variables", QJsonArray() << QJsonArray{"h", "", "1", "2"}}, {"objectives", QJsonArray() << QJsonArray{"Z0"}}}, "no parameter h"));
        QVERIFY(refused({{"variables", w}, {"objectives", QJsonArray() << QJsonArray{"Z0", "MON"}}}, "nothing to reach"));
        QVERIFY(refused({{"variables", w}, {"method", "nsga2"}, {"objectives", QJsonArray() << QJsonArray{"Z0", "MIN"}}}, "two objectives"));
        QVERIFY(refused({{"variables", w}, {"objectives", QJsonArray() << QJsonArray{"Z0", "LE", "w*1[Ohm/um]"}}}, "moves with w"));
    }

    // A run's stop reaches each solve under way at once, and a solve begun
    // after it is stopped as it begins; one ended is let go.
    void aStopReachesTheSolvesUnderWay()
    {
        rf::RunControl control;
        rf::SolveJob a, b, c;
        control.begin(&a);
        control.begin(&b);
        control.end(&b);
        QVERIFY(!a.stop.load());
        control.requestStop();
        QVERIFY(control.stop.load());
        QVERIFY(a.stop.load());
        QVERIFY(!b.stop.load());   // (ended before it)
        control.begin(&c);
        QVERIFY(c.stop.load());
    }
};

QTEST_GUILESS_MAIN(TestRf25Optimize)
#include "test_rf25_optimize.moc"
