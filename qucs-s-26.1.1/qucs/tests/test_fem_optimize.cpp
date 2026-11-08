/*
 * test_fem_optimize.cpp - a multiphysics model's optimization against what is
 *                         known in closed form: a capacitor's gap and width, a
 *                         bar's width, every method, several objectives, limits,
 *                         a Pareto front, design centering; what stops a run, a
 *                         solve that fails in one, ranges that are none, old
 *                         files; the section and its result in the file; the
 *                         optimum written in. Every run is bounded by its
 *                         evaluations and tolerances, none by a time.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_optimize.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <atomic>
#include <cmath>

using namespace qucs_s::fem;

namespace {

constexpr double Eps0 = 8.8541878188e-12;

/// A capacitor whose sides carry no charge: C = eps0 er w / d, per metre,
/// exactly on any mesh - w, d and er parameters, its element a third of its
/// gap - with the optimizations \a section.
QByteArray capacitor(const QByteArray& section, const QByteArray& parameters = {}, const QByteArray& study = R"({"type": "stationary"})")
{
    return QByteArray(R"({"format": "qucs-s multiphysics 1", "parameters": [
        {"name": "w", "expression": "4[mm]"}, {"name": "d", "expression": "1[mm]"}, {"name": "er", "expression": "4"}, {"name": "V0", "expression": "5[V]"})")
           + parameters + R"(],
      "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "gap", "size": ["w", "d"]}],
        "materials": [{"label": "dielectric", "epsilonr": "er", "selection": {"all": true}}],
        "physics": [{"type": "electrostatics", "tag": "es", "order": "1", "features": [
          {"type": "terminal", "name": "1", "V0": "V0", "selection": {"of": ["gap"], "side": "top"}},
          {"type": "ground", "selection": {"of": ["gap"], "side": "bottom"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "max(d, w)/4"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [)" + study + R"(]}],
      "optimizations": [)" + section + R"(]})";
}

/// A bar between two contacts: R = L / (sigma w), per metre of thickness,
/// exactly on any mesh.
QByteArray bar(const QByteArray& section, const QByteArray& parameters = {})
{
    return QByteArray(R"({"format": "qucs-s multiphysics 1", "parameters": [
        {"name": "w", "expression": "2[mm]"}, {"name": "L", "expression": "10[mm]"})")
           + parameters + R"(],
      "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "bar", "size": ["L", "w"]}],
        "materials": [{"label": "conductor", "sigma": "1[S/m]", "selection": {"all": true}}],
        "physics": [{"type": "currents", "tag": "ec", "order": "1", "features": [
          {"type": "terminal", "name": "1", "V0": "1[V]", "selection": {"of": ["bar"], "side": "left"}},
          {"type": "ground", "selection": {"of": ["bar"], "side": "right"}}]}],
        "mesh": {"size": "normal", "custom": true, "hmax": "max(L, w)/4"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}],
      "optimizations": [)" + section + R"(]})";
}

Model modelOf(const QByteArray& json)
{
    QString why;
    std::optional<Model> m = Model::fromJson(json, &why);
    if (!m) qFatal("not a model: %s", qPrintable(why));
    if (!why.isEmpty()) qWarning("%s", qPrintable(why));
    return *m;
}

OptimizationResult run(const QByteArray& json, const QString& tag = QStringLiteral("opt1"))
{
    return runOptimization(modelOf(json), tag, nullptr, nullptr, nullptr);
}

double value(const OptimizationResult& r, const QString& name)
{
    for (const OptimizationResult::Value& v : r.parameters)
        if (v.name == name) return v.value;
    for (const OptimizationResult::Value& v : r.measured)
        if (v.name == name) return v.value;
    return NAN;
}

QString said(const OptimizationResult& r)
{
    return QStringLiteral("error [%1] status %2: %3; notes %4; search %5; %6 evaluations, %7 solves, %8 failed")
        .arg(r.error, r.status, r.summary, r.notes.join(QLatin1String(" | ")), r.search.join(QLatin1String(" | ")))
        .arg(r.evaluations)
        .arg(r.solves)
        .arg(r.failed);
}

QByteArray quoted(const QString& text)
{
    return QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
}

} // namespace

class TestFemOptimize : public QObject
{
    Q_OBJECT

private slots:
    // The closed forms, each method.
    void everyMethodFindsTheGap_data();
    void everyMethodFindsTheGap();
    void differentialEvolutionsVariants();
    void aMinimumInsideItsRange_data();
    void aMinimumInsideItsRange();
    void severalObjectivesAreWeighed();
    void twoTargetsTwoParameters_data();
    void twoTargetsTwoParameters();
    void limitsAreHeld_data();
    void limitsAreHeld();
    void limitsInSmallUnits_data();
    void limitsInSmallUnits();
    void theHistoryMarksWhatBreaksALimit();
    void penaltyLimitsAsTheSearchJudgesThem();
    void heldParametersAreWrittenIn();
    void aValueThatMovesWithAVariedParameter();
    void aFormulaWrittenOverIsSaid();
    void penaltiesAsAComponentsGoals();
    void aFeasibilityProblemEndsOnceMet();
    void aParetoFront();
    void aParetoFrontWithALimit();
    void designCentering();
    void morStartsAndAPolish();
    // What a study gives.
    void aSweepCountsByItsWorst();
    void outputTimesToo();
    void derivedValuesAreRead();
    void anotherStudysValues();
    void aModeAnalysisReadsOneMode();
    void wholeNumbersAndSeries();
    // What may go wrong.
    void aStopKeepsTheBest();
    void aStopBeforeAnythingIsSolved();
    void aFailedSolveDuringARun();
    void nothingSolves();
    void whatTheResultsLackEndsTheRun();
    void theObjectiveNeverMoves();
    void aParameterOnItsBound();
    void badSetups_data();
    void badSetups();
    // The shipped example.
    void theShippedExample_data();
    void theShippedExample();
    // The file.
    void oldFilesHaveNoSection();
    void theSectionRoundTrips();
    void resultsReadBack();
    void aHostileResultIsHarmless();
    void aRowOfAnotherLengthIsNotRead();
    void aLongHistoryIsCutInTheFile();
    // The optimum written in.
    void theOptimumIsWrittenIn();
    void aFrontsDesignIsChosen();
    void whatIsNotWrittenIn();
    void theSetupTellsAnOldResult();
    // Solves at once.
    void theSameOnAnyNumberOfSolves();
    void repeatedPointsAreNotSolvedAgain();
};

void TestFemOptimize::everyMethodFindsTheGap_data()
{
    QTest::addColumn<QByteArray>("settings");
    QTest::addColumn<double>("tolerance");
    QTest::addColumn<int>("most");
    QTest::newRow("automatic") << QByteArray(R"("maxiter": 60)") << 1e-6 << 120;
    QTest::newRow("nm") << QByteArray(R"("method": "nm", "maxiter": 200)") << 1e-5 << 400;
    QTest::newRow("tr") << QByteArray(R"("method": "tr", "maxiter": 60, "tol": "1e-10")") << 1e-3 << 70;
    QTest::newRow("lm") << QByteArray(R"("method": "lm", "maxiter": 60)") << 1e-6 << 120;
    QTest::newRow("tune") << QByteArray(R"("method": "tune", "maxiter": 40)") << 1e-5 << 40;
    QTest::newRow("de") << QByteArray(R"("method": "de", "maxiter": 40, "population": 10, "seed": 3)") << 2e-3 << 420;
    QTest::newRow("pso") << QByteArray(R"("method": "pso", "maxiter": 40, "population": 10, "seed": 3)") << 2e-2 << 420;
    QTest::newRow("sa") << QByteArray(R"("method": "sa", "maxiter": 15, "seed": 3)") << 2e-2 << 200;
    QTest::newRow("cmaes") << QByteArray(R"("method": "cmaes", "maxiter": 40, "population": 6, "seed": 3)") << 1e-3 << 250;
    QTest::newRow("bayes") << QByteArray(R"("method": "bayes", "maxiter": 30, "seed": 3)") << 1e-2 << 30;
}

// C = eps0 er w / d: the gap that gives 200 pF, on a logarithmic scale over a
// range of decades.
void TestFemOptimize::everyMethodFindsTheGap()
{
    QFETCH(QByteArray, settings);
    QFETCH(double, tolerance);
    QFETCH(int, most);
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", )" + settings + R"(,
        "variables": [["d", "", "0.1[mm]", "5[mm]", "LOG_DOUBLE"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const double exact = Eps0 * 4 * 4e-3 / 200e-12;
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - exact) < tolerance * exact, qPrintable(QStringLiteral("d %1 of %2: ").arg(value(r, QStringLiteral("d"))).arg(exact) + said(r)));
    QVERIFY2(r.evaluations <= most, qPrintable(said(r)));
    QVERIFY2(r.solves <= r.evaluations, qPrintable(said(r)));
    QVERIFY2(r.failed == 0, qPrintable(said(r)));
    QCOMPARE(int(r.history.size()), r.solves);
    // Its history's best row is the optimum's.
    double bestRow = NAN;
    for (const OptimizationRow& row : r.history)
        if (row.best) bestRow = row.values.at(0);
    QCOMPARE(bestRow, value(r, QStringLiteral("d")));
    // The same again: a run repeats.
    const OptimizationResult again = run(capacitor(R"({"type": "optimization", "tag": "opt1", )" + settings + R"(,
        "variables": [["d", "", "0.1[mm]", "5[mm]", "LOG_DOUBLE"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    QCOMPARE(value(again, QStringLiteral("d")), value(r, QStringLiteral("d")));
    QCOMPARE(again.evaluations, r.evaluations);
}

// The ten of an optimization component: the same class, reached from here.
void TestFemOptimize::differentialEvolutionsVariants()
{
    for (const char* variant : {"DE/best/1/exp", "DE/rand/1/exp", "DE/rand-to-best/1/exp", "DE/best/2/exp", "DE/rand/2/exp", "DE/best/1/bin", "DE/rand/1/bin",
                                "DE/rand-to-best/1/bin", "DE/best/2/bin", "DE/rand/2/bin"}) {
        const OptimizationResult r = run(capacitor(QByteArray(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 25, "population": 8, "seed": 5,
            "F": "0.7", "CR": "0.9", "strategy": ")") + variant + R"(",
            "variables": [["d", "", "0.2[mm]", "3[mm]"]],
            "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
        QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
        const double exact = Eps0 * 4 * 4e-3 / 200e-12;
        QVERIFY2(std::abs(value(r, QStringLiteral("d")) - exact) < 0.02 * exact,
                 qPrintable(QStringLiteral("%1: d %2 of %3: ").arg(QLatin1String(variant)).arg(value(r, QStringLiteral("d"))).arg(exact) + said(r)));
    }
    // Its cost variance stops it, its generations otherwise.
    const QByteArray settled = capacitor(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 200, "population": 8, "seed": 5, "mincostvariance": "1e-2",
        "variables": [["d", "", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})");
    const OptimizationResult r = run(settled);
    QVERIFY2(r.evaluations < 8 * 200, qPrintable(said(r)));
    QCOMPARE(r.status, QStringLiteral("converged"));
}

void TestFemOptimize::aMinimumInsideItsRange_data()
{
    QTest::addColumn<QByteArray>("settings");
    QTest::addColumn<double>("tolerance");
    QTest::newRow("nm") << QByteArray(R"("method": "nm", "maxiter": 200)") << 1e-3;
    QTest::newRow("tr") << QByteArray(R"("method": "tr", "maxiter": 60)") << 1e-3;
    QTest::newRow("cmaes") << QByteArray(R"("method": "cmaes", "maxiter": 40, "seed": 2)") << 1e-2;
    QTest::newRow("bayes") << QByteArray(R"("method": "bayes", "maxiter": 30, "seed": 2)") << 3e-2;
    QTest::newRow("de") << QByteArray(R"("method": "de", "maxiter": 25, "population": 8, "seed": 2)") << 3e-2;
    QTest::newRow("pso") << QByteArray(R"("method": "pso", "maxiter": 30, "population": 8, "seed": 2)") << 5e-2;
    QTest::newRow("sa") << QByteArray(R"("method": "sa", "maxiter": 20, "seed": 2)") << 5e-2;
}

// A bar's resistance and its width, as one number: R/5ohm + w/2mm = 2mm/w + w/2mm
// at w in mm - its minimum at w = 2 mm, exactly.
void TestFemOptimize::aMinimumInsideItsRange()
{
    QFETCH(QByteArray, settings);
    QFETCH(double, tolerance);
    const OptimizationResult r = run(bar(R"({"type": "optimization", "tag": "opt1", )" + settings + R"(,
        "variables": [["w", "3.5[mm]", "0.5[mm]", "8[mm]"]],
        "objectives": [["ec.R/5[ohm] + w/2[mm]", "MIN"], ["ec.R", "MON"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("w")) - 2e-3) < tolerance * 2e-3, qPrintable(QStringLiteral("w %1: ").arg(value(r, QStringLiteral("w"))) + said(r)));
    // What is only shown is measured there: R = L / (sigma w).
    QVERIFY2(std::abs(value(r, QStringLiteral("ec.R")) - 10e-3 / value(r, QStringLiteral("w"))) < 1e-9, qPrintable(said(r)));
    QCOMPARE(r.kinds, (QStringList{QStringLiteral("MIN"), QStringLiteral("MON")}));
}

// Two objectives of different units weighed against each other, each relative
// to its value at the start: w1 R/Rs + w2 w/ws is least at w = ws sqrt(w1/w2).
void TestFemOptimize::severalObjectivesAreWeighed()
{
    const OptimizationResult r = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 80, "tol": "1e-9",
        "variables": [["w", "2[mm]", "0.5[mm]", "9[mm]"]],
        "objectives": [["ec.R", "MIN", "", "4"], ["w", "MIN", "", "1"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("w")) - 4e-3) < 1e-3 * 4e-3, qPrintable(QStringLiteral("w %1: ").arg(value(r, QStringLiteral("w"))) + said(r)));
    // A maximized objective the same way round: minus its weight.
    const OptimizationResult m = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 80, "tol": "1e-9",
        "variables": [["w", "2[mm]", "0.5[mm]", "9[mm]"]],
        "objectives": [["ec.R", "MIN", "", "4"], ["-w", "MAX", "", "1"]]})"));
    QVERIFY2(std::abs(value(m, QStringLiteral("w")) - 4e-3) < 1e-3 * 4e-3, qPrintable(said(m)));
}

void TestFemOptimize::twoTargetsTwoParameters_data()
{
    QTest::addColumn<QByteArray>("settings");
    QTest::addColumn<double>("tolerance");
    QTest::addColumn<int>("most");
    QTest::newRow("lm") << QByteArray(R"("method": "lm", "maxiter": 40)") << 1e-5 << 200;
    QTest::newRow("tune") << QByteArray(R"("method": "tune", "maxiter": 40)") << 1e-4 << 40;
    QTest::newRow("nm") << QByteArray(R"("method": "nm", "maxiter": 300, "tol": "1e-12")") << 1e-3 << 800;
}

// A capacitance and an area at once: d = sqrt(eps A / C), w = A / d.
void TestFemOptimize::twoTargetsTwoParameters()
{
    QFETCH(QByteArray, settings);
    QFETCH(double, tolerance);
    QFETCH(int, most);
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", )" + settings + R"(,
        "variables": [["w", "", "1[mm]", "20[mm]"], ["d", "", "0.2[mm]", "4[mm]"]],
        "objectives": [["es.C11", "TARGET", "150[pF]"], ["w*d", "TARGET", "6[mm^2]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const double area = 6e-6, eps = Eps0 * 4, C = 150e-12;
    const double d = std::sqrt(eps * area / C), w = area / d;
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - d) < tolerance * d, qPrintable(QStringLiteral("d %1 of %2: ").arg(value(r, QStringLiteral("d"))).arg(d) + said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("w")) - w) < tolerance * w, qPrintable(QStringLiteral("w %1 of %2: ").arg(value(r, QStringLiteral("w"))).arg(w) + said(r)));
    QVERIFY2(r.evaluations <= most, qPrintable(said(r)));
}

void TestFemOptimize::limitsAreHeld_data()
{
    QTest::addColumn<QByteArray>("settings");
    QTest::addColumn<double>("tolerance");
    QTest::newRow("tr") << QByteArray(R"("method": "tr", "maxiter": 60)") << 2e-3;
    QTest::newRow("nm") << QByteArray(R"("method": "nm", "maxiter": 120)") << 5e-3;
    QTest::newRow("cmaes") << QByteArray(R"("method": "cmaes", "maxiter": 25, "seed": 4)") << 2e-2;
    QTest::newRow("de") << QByteArray(R"("method": "de", "maxiter": 15, "population": 8, "seed": 4)") << 5e-2;
}

// The least width that keeps the bar's resistance at or below 2.5 ohm:
// R = L / (sigma w) <= 2.5 at w = L / (sigma 2.5) = 4 mm - an active limit.
void TestFemOptimize::limitsAreHeld()
{
    QFETCH(QByteArray, settings);
    QFETCH(double, tolerance);
    const OptimizationResult r = run(bar(R"({"type": "optimization", "tag": "opt1", )" + settings + R"(,
        "variables": [["w", "8[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["w", "MIN"], ["ec.R", "LE", "2.5[ohm]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("w")) - 4e-3) < tolerance * 4e-3, qPrintable(QStringLiteral("w %1: ").arg(value(r, QStringLiteral("w"))) + said(r)));
    QVERIFY2(r.feasible, qPrintable(said(r)));
    QVERIFY2(value(r, QStringLiteral("ec.R")) <= 2.5 * (1 + 2e-4), qPrintable(said(r)));
    QCOMPARE(r.constraints.size(), 1);
    // (A local method ends on it; a population at its resolution, feasible.)
    if (tolerance < 1e-2) QVERIFY2(r.constraints.first().contains(QStringLiteral("active")), qPrintable(r.constraints.join(QLatin1String(" | "))));
    QVERIFY2(!r.search.isEmpty() && r.search.first().contains(QStringLiteral("constraints round")), qPrintable(said(r)));
    // From below: GE, and a limit with room says so.
    const OptimizationResult ge = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 40,
        "variables": [["w", "3[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["w", "MAX"], ["ec.R", "GE", "2.5[ohm]"], ["ec.R", "LE", "100[ohm]"]]})"));
    QVERIFY2(std::abs(value(ge, QStringLiteral("w")) - 4e-3) < 0.01 * 4e-3, qPrintable(said(ge)));
    QVERIFY2(ge.constraints.join(QLatin1Char(' ')).contains(QStringLiteral("slack")), qPrintable(ge.constraints.join(QLatin1String(" | "))));
    // At a value: EQ, both sides.
    const OptimizationResult eq = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 100,
        "variables": [["w", "3[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["L", "MIN"], ["ec.R", "EQ", "2[ohm]"]]})"));
    QVERIFY2(std::abs(value(eq, QStringLiteral("w")) - 5e-3) < 0.01 * 5e-3, qPrintable(said(eq)));
    // One that cannot be met: said, not hidden.
    const OptimizationResult no = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 25,
        "variables": [["w", "3[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["w", "MIN"], ["ec.R", "LE", "0.1[ohm]"]]})"));
    QCOMPARE(no.status, QStringLiteral("infeasible"));
    QVERIFY2(!no.feasible, qPrintable(said(no)));
    QVERIFY2(no.constraints.join(QLatin1Char(' ')).contains(QStringLiteral("VIOLATED")), qPrintable(no.constraints.join(QLatin1String(" | "))));
    QString why;
    QVERIFY(!no.canApply(&why));
    QVERIFY2(why.contains(QStringLiteral("limit")), qPrintable(why));
}

// Goals as an optimization component has them: objectives and limits in one
// cost, by its weights, its cost variance and its early stop.
void TestFemOptimize::limitsInSmallUnits_data()
{
    QTest::addColumn<QByteArray>("settings");
    QTest::newRow("nm") << QByteArray(R"("method": "nm", "maxiter": 200)");
    QTest::newRow("tr") << QByteArray(R"("method": "tr", "maxiter": 200)");
    QTest::newRow("cmaes") << QByteArray(R"("method": "cmaes", "maxiter": 60, "seed": 1)");
    QTest::newRow("de") << QByteArray(R"("method": "de", "maxiter": 60, "population": 10, "seed": 2)");
    QTest::newRow("bayes") << QByteArray(R"("method": "bayes", "maxiter": 30, "seed": 1)");
    QTest::newRow("penalty") << QByteArray(R"("method": "nm", "maxiter": 200, "constraints": "penalty")");
}

// A limit in picofarads is judged as a share of its value, as the search judges
// it: by max(1, |value|), 900 pF read a miss of 546 pF as 5e-10, every limits-only
// run "converged" at its start, and its violating point could be written in.
void TestFemOptimize::limitsInSmallUnits()
{
    QFETCH(QByteArray, settings);
    // C = eps0 er w / d: 141.67 pF at d = 1 mm; 400 pF wants d <= 0.3542 mm.
    const double most = Eps0 * 4 * 4e-3 / 400e-12;
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", )" + settings + R"(,
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "GE", "400[pF]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(r.feasible, qPrintable(said(r) + r.constraints.join(QLatin1String(" | "))));
    QVERIFY2(r.solves > 1, qPrintable(said(r)));
    QVERIFY2(value(r, QStringLiteral("d")) <= most * (1 + 1e-4), qPrintable(QStringLiteral("d %1: ").arg(value(r, QStringLiteral("d"))) + said(r)));
    QVERIFY2(value(r, QStringLiteral("es.C11")) >= 400e-12 * (1 - 1e-4), qPrintable(said(r)));
    QVERIFY2(!r.constraints.join(QLatin1Char(' ')).contains(QStringLiteral("VIOLATED")) && !r.constraints.join(QLatin1Char(' ')).contains(QStringLiteral("NOT MET")),
             qPrintable(r.constraints.join(QLatin1String(" | "))));
    QVERIFY(r.canApply());
    // One that cannot be met in the range: not met, and not written in.
    const OptimizationResult no = run(capacitor(R"({"type": "optimization", "tag": "opt1", )" + settings + R"(,
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "GE", "10[nF]"]]})"));
    QVERIFY2(!no.feasible, qPrintable(said(no) + no.constraints.join(QLatin1String(" | "))));
    QVERIFY2(no.status != QLatin1String("converged") || no.constraints.join(QLatin1Char(' ')).contains(QStringLiteral("NOT MET")), qPrintable(said(no)));
    QString why;
    QVERIFY2(!no.canApply(&why), qPrintable(said(no)));
    QVERIFY2(why.contains(QStringLiteral("limit")), qPrintable(why));
}

// Each row of the history feasible or not as the limit is: by its share of the
// value, not 631 of 1000 rows breaking 200 pF in bold.
void TestFemOptimize::theHistoryMarksWhatBreaksALimit()
{
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "cmaes", "maxiter": 40, "seed": 3,
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]"]],
        "objectives": [["d", "MAX"], ["es.C11", "GE", "100[pF]"]]})"));
    QVERIFY2(r.error.isEmpty() && r.feasible, qPrintable(said(r)));
    int breaking = 0;
    for (const OptimizationRow& row : r.history) {
        if (!std::isfinite(row.cost)) continue;
        const double c = row.measured.at(1);
        const bool meets = c >= 100e-12 * (1 - 1e-4);
        QVERIFY2(row.feasible == meets, qPrintable(QStringLiteral("row %1: C %2, marked %3").arg(row.index).arg(c).arg(row.feasible)));
        if (row.best) QVERIFY2(row.feasible, qPrintable(QStringLiteral("row %1 best, C %2").arg(row.index).arg(c)));
        breaking += meets ? 0 : 1;
    }
    QVERIFY2(breaking > 0, "the search never crossed the limit: the test proves nothing");
}

// By penalties the core knows of no limit, and each is said met or not: within
// "Limits met within", as the search judged it. An EQ that cannot be reached is
// not met, however small its unit; one beside the bound of an active limit is.
void TestFemOptimize::penaltyLimitsAsTheSearchJudgesThem()
{
    const OptimizationResult eq = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 100, "constraints": "penalty",
        "variables": [["w", "4[mm]", "1[mm]", "8[mm]"]],
        "objectives": [["w", "MIN"], ["es.C11", "EQ", "900[pF]"]]})"));
    QVERIFY2(eq.error.isEmpty(), qPrintable(said(eq)));
    QVERIFY2(eq.constraints.size() == 1 && eq.constraints.first().contains(QStringLiteral("NOT MET")), qPrintable(eq.constraints.join(QLatin1String(" | "))));
    QVERIFY2(!eq.feasible, qPrintable(said(eq)));
    QVERIFY(!eq.canApply());
    // MIN w with C >= 100 pF: w = 2.823 mm, ending a hair beside the bound.
    const OptimizationResult ge = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 200, "constraints": "penalty",
        "variables": [["w", "4[mm]", "1[mm]", "8[mm]"]],
        "objectives": [["w", "MIN"], ["es.C11", "GE", "100[pF]"]]})"));
    QVERIFY2(ge.error.isEmpty(), qPrintable(said(ge)));
    const double c = value(ge, QStringLiteral("es.C11"));
    QVERIFY2(std::abs(c - 100e-12) < 1e-4 * 100e-12, qPrintable(said(ge)));
    QVERIFY2(ge.constraints.size() == 1 && ge.constraints.first().endsWith(QStringLiteral(", met")), qPrintable(ge.constraints.join(QLatin1String(" | "))));
    QVERIFY2(ge.feasible, qPrintable(said(ge)));
    QVERIFY(ge.canApply());
    // (The goal of an optimization component, by the same share.)
    qucs_s::optimization::Goal g;
    g.type = qucs_s::optimization::GoalType::Equal;
    g.target = 900e-12;
    QVERIFY(!g.met(864.58e-12));
    QVERIFY(g.met(900e-12 * (1 + 1e-12)));
}

// A parameter held at a start of its own was that in the run: written in with
// the optimum, else the optimum does not give what the run found.
void TestFemOptimize::heldParametersAreWrittenIn()
{
    // er held at 8 (the model's is 4): C = eps0 8 w / d = 200 pF at d = 1.41667 mm.
    Model m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 30,
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]"], ["er", "8", "1", "10", "", "no"], ["w", "", "1[mm]", "8[mm]", "", "no"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - Eps0 * 8 * 4e-3 / 200e-12) < 1e-4 * 1.4e-3, qPrintable(said(r)));
    // (d shapes the geometry: each candidate meshed again, and said, with what to check.)
    QVERIFY2(r.notes.join(QLatin1Char(' ')).contains(QStringLiteral("meshed again")) && r.notes.join(QLatin1Char(' ')).contains(QStringLiteral("Mesh Convergence")), qPrintable(said(r)));
    m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("result"), r.toJson());
    QStringList written;
    QString why;
    QVERIFY2(applyOptimum(m, *m.find(QStringLiteral("opt1")), 0, &written, &why), qPrintable(why));
    QVERIFY2(written.size() == 2 && written.at(1).startsWith(QStringLiteral("er = 8")) && written.at(1).contains(QStringLiteral("held")), qPrintable(written.join(QLatin1String(" | "))));
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    QCOMPARE(ps->values.value(QStringLiteral("er")), 8.0);
    QCOMPARE(ps->values.value(QStringLiteral("w")), 4e-3);   // (held with no start: its own value, untouched)
    // Solved there - a run whose first solve is at the model's own values now -:
    // what the run found.
    QJsonArray table;
    table.append(QJsonArray{QStringLiteral("d"), QString(), QStringLiteral("0.2[mm]"), QStringLiteral("3[mm]")});
    m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("variables"), table);
    const OptimizationResult again = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QVERIFY2(again.history.size() >= 1 && std::abs(again.history.front().measured.at(0) - 200e-12) < 1e-4 * 200e-12, qPrintable(said(again)));
}

// A target's or a limit's value is read once: one that a varied parameter moves
// is refused, with what to write instead - not frozen at its start, the search
// then "converged" on another answer.
void TestFemOptimize::aValueThatMovesWithAVariedParameter()
{
    QString why;
    Model m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm",
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]*d/1[mm]"]]})", R"(, {"name": "gap2", "expression": "2*d"})"));
    QVERIFY(!optimizationPlan(m, *m.find(QStringLiteral("opt1")), *evaluateParameters(m), &why));
    QVERIFY2(why.contains(QStringLiteral("moves with d")) && why.contains(QStringLiteral("into the expression")), qPrintable(why));
    // Through another parameter too.
    m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm",
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "LE", "gap2*1[pF/mm]"]]})", R"(, {"name": "gap2", "expression": "2*d"})"));
    QVERIFY(!optimizationPlan(m, *m.find(QStringLiteral("opt1")), *evaluateParameters(m), &why));
    QVERIFY2(why.contains(QStringLiteral("moves with d")), qPrintable(why));
    // A value of a parameter it does not vary is read as before.
    m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm",
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "50[pF]*er"]]})"));
    QVERIFY2(optimizationPlan(m, *m.find(QStringLiteral("opt1")), *evaluateParameters(m), &why), qPrintable(why));
}

// A parameter whose expression is a formula, written over: said, and the value
// in a dozen digits, not 0.010000000003209997[m].
void TestFemOptimize::aFormulaWrittenOverIsSaid()
{
    Model m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 30,
        "variables": [["half", "", "0.1[mm]", "2[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})", R"(, {"name": "half", "expression": "d/2"})"));
    // (d is half's twice: written as d = 2*half below, so the geometry follows.)
    QList<Model::Parameter> ps = m.parameters();
    for (Model::Parameter& p : ps)
        if (p.name == QLatin1String("d")) p.expression = QStringLiteral("1[mm]");
    m.setParameters(ps);
    OptimizationResult r;
    r.method = QStringLiteral("tune");
    r.status = QStringLiteral("converged");
    r.columns = QStringList{QStringLiteral("half"), QStringLiteral("es.C11")};
    r.kinds = QStringList{QStringLiteral("TARGET")};
    r.parameters << OptimizationResult::Value{QStringLiteral("half"), 0.010000000003209997, QString()};
    m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("result"), r.toJson());
    QStringList written;
    QString why;
    QVERIFY2(applyOptimum(m, *m.find(QStringLiteral("opt1")), 0, &written, &why), qPrintable(why));
    QCOMPARE(written.size(), 1);
    QVERIFY2(written.first().startsWith(QStringLiteral("half = 0.0100000000032[m]")), qPrintable(written.first()));
    QVERIFY2(written.first().contains(QStringLiteral("in place of its expression d/2")), qPrintable(written.first()));
}

void TestFemOptimize::penaltiesAsAComponentsGoals()
{
    const OptimizationResult r = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 40, "population": 10, "seed": 3, "strategy": "DE/rand-to-best/1/exp",
        "F": "0.85", "CR": "1", "constraints": "penalty", "objectiveweight": "10", "constraintweight": "100",
        "variables": [["w", "8[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["w", "MIN"], ["ec.R", "LE", "2.5[ohm]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("w")) - 4e-3) < 0.03 * 4e-3, qPrintable(QStringLiteral("w %1: ").arg(value(r, QStringLiteral("w"))) + said(r)));
    QVERIFY2(r.constraints.join(QLatin1Char(' ')).contains(QStringLiteral("2.5")), qPrintable(r.constraints.join(QLatin1String(" | "))));
    QVERIFY2(r.search.isEmpty(), qPrintable(said(r)));   // (no round of an augmented Lagrangian)
}

// Only limits: nothing to improve once all are met; a start that meets them all
// is the answer at once.
void TestFemOptimize::aFeasibilityProblemEndsOnceMet()
{
    const OptimizationResult met = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 50, "population": 8, "seed": 2,
        "variables": [["w", "2[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["ec.R", "LE", "100[ohm]"]]})"));
    QVERIFY2(met.error.isEmpty(), qPrintable(said(met)));
    QCOMPARE(met.status, QStringLiteral("converged"));
    QVERIFY2(met.solves == 1, qPrintable(said(met)));
    QCOMPARE(value(met, QStringLiteral("w")), 2e-3);
    const OptimizationResult found = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 50, "population": 8, "seed": 2, "constraints": "penalty",
        "variables": [["w", "2[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["ec.R", "LE", "1.5[ohm]"]]})"));
    QVERIFY2(found.feasible, qPrintable(said(found)));
    QVERIFY2(value(found, QStringLiteral("ec.R")) <= 1.5 * (1 + 1e-4), qPrintable(said(found)));   // (within "Limits met within")
    QVERIFY2(found.solves < 8 * 51, qPrintable(said(found)));
}

// Two objectives no design is best at: a bar's resistance and its area. A
// shorter bar is better at both, so the front is at the least L, its designs on
// R A = L^2 / sigma.
void TestFemOptimize::aParetoFront()
{
    const OptimizationResult r = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "nsga2", "maxiter": 14, "population": 16, "seed": 7,
        "variables": [["w", "2[mm]", "1[mm]", "9[mm]"], ["L", "10[mm]", "4[mm]", "12[mm]"]],
        "objectives": [["ec.R", "MIN"], ["w*L", "MIN"], ["ec.R*w", "MON"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QCOMPARE(r.status, QStringLiteral("completed"));
    QVERIFY2(r.front.size() >= 5, qPrintable(said(r)));
    QVERIFY(r.parameters.isEmpty());   // (no one optimum)
    double least = 1e30, most = 0;
    for (const OptimizationResult::FrontPoint& p : r.front) {
        const double w = p.values.at(0), L = p.values.at(1), R = p.measured.at(0), A = p.measured.at(1);
        QVERIFY2(std::abs(R - L / w) < 1e-9, "R is not L / (sigma w)");
        QVERIFY2(std::abs(A - w * L) < 1e-15, "the area is not w L");
        QVERIFY2(L < 6e-3, qPrintable(QStringLiteral("L %1: not at its least: ").arg(L) + said(r)));
        least = std::min(least, w);
        most = std::max(most, w);
    }
    // Along the trade-off, not one point of it; sorted by the first objective.
    QVERIFY2(most > 2 * least, qPrintable(QStringLiteral("w from %1 to %2").arg(least).arg(most)));
    for (std::size_t i = 1; i < r.front.size(); ++i) QVERIFY(r.front[i - 1].measured.at(0) <= r.front[i].measured.at(0));
    // Every design of the front is one a row of the history has.
    for (const OptimizationResult::FrontPoint& p : r.front) {
        bool there = false;
        for (const OptimizationRow& row : r.history) there = there || row.values == p.values;
        QVERIFY(there);
    }
    // Maximized the same way: the front of -R and -A the other way round.
    const OptimizationResult m = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "nsga2", "maxiter": 8, "population": 12, "seed": 7,
        "variables": [["w", "2[mm]", "1[mm]", "9[mm]"], ["L", "10[mm]", "4[mm]", "12[mm]"]],
        "objectives": [["-ec.R", "MAX"], ["w*L", "MIN"]]})"));
    QVERIFY2(m.error.isEmpty() && m.front.size() >= 3, qPrintable(said(m)));
}

// Designs that break a limit lose to those that do not (Deb's rule; ngspice's
// nsga2 has no limits). R <= 2 ohm: w >= L / (sigma 2).
void TestFemOptimize::aParetoFrontWithALimit()
{
    const OptimizationResult r = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "nsga2", "maxiter": 12, "population": 16, "seed": 3,
        "variables": [["w", "2[mm]", "1[mm]", "9[mm]"], ["L", "10[mm]", "4[mm]", "12[mm]"]],
        "objectives": [["ec.R", "MIN"], ["w*L", "MIN"], ["ec.R", "LE", "1.2[ohm]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(!r.front.empty(), qPrintable(said(r)));
    for (const OptimizationResult::FrontPoint& p : r.front)
        QVERIFY2(p.measured.at(2) <= 1.2 * (1 + 2e-4), qPrintable(QStringLiteral("R %1 past its limit").arg(p.measured.at(2))));
}

// Design centering: the capacitance held between two limits under a gap of 2 %
// sigma; the worst Cpk is best where it is halfway, C = (lo + hi) / 2, and then
// (hi - lo) / 2 over three sigma of C (2 % of it).
void TestFemOptimize::designCentering()
{
    const QByteArray section = R"({"type": "optimization", "tag": "opt1", "mode": "center", "method": "tr", "maxiter": 25, "samples": 24, "seed": 11, "lhs": true,
        "variables": [["d", "1.2[mm]", "0.3[mm]", "2[mm]"]],
        "variation": [["d", "gauss", "2 %"]],
        "objectives": [["es.C11", "LE", "160[pF]"], ["es.C11", "GE", "120[pF]"]]})";
    const OptimizationResult r = run(capacitor(section));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const double centre = 140e-12, exact = Eps0 * 4 * 4e-3 / centre;
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - exact) < 0.02 * exact, qPrintable(QStringLiteral("d %1 of %2: ").arg(value(r, QStringLiteral("d"))).arg(exact) + said(r)));
    const double cpk = 20e-12 / (3 * 0.02 * centre);
    QVERIFY2(std::abs(r.cpk - cpk) < 0.25 * cpk, qPrintable(QStringLiteral("Cpk %1 of %2: ").arg(r.cpk).arg(cpk) + said(r)));
    QVERIFY2(std::abs(r.yield - 1.0) < 0.1, qPrintable(said(r)));
    QVERIFY2(r.summary.contains(QStringLiteral("yield")), qPrintable(r.summary));
    QCOMPARE(r.solves, r.evaluations * 24);
    // A uniform spread, plain draws, an absolute width: another parameter's.
    const OptimizationResult u = run(capacitor(R"({"type": "optimization", "tag": "opt1", "mode": "center", "method": "nm", "maxiter": 4, "samples": 6, "seed": 2,
        "variables": [["d", "1.2[mm]", "0.3[mm]", "2[mm]"]],
        "variation": [["er", "uniform", "0.1"]],
        "objectives": [["es.C11", "LE", "160[pF]"], ["es.C11", "GE", "120[pF]"], ["es.W", "MON"]]})"));
    QVERIFY2(u.error.isEmpty(), qPrintable(said(u)));
    QVERIFY(std::isfinite(u.cpk));
}

void TestFemOptimize::morStartsAndAPolish()
{
    const QByteArray section = R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 40, "starts": 2, "seed": 3,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})";
    const OptimizationResult r = run(capacitor(section));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const QString lines = r.search.join(QLatin1String(" | "));
    QVERIFY2(lines.contains(QStringLiteral("start 3 of 3")), qPrintable(lines));
    QVERIFY2(lines.contains(QStringLiteral("won")), qPrintable(lines));
    QVERIFY2(lines.contains(QStringLiteral("polish")), qPrintable(lines));
    const double exact = Eps0 * 4 * 4e-3 / 200e-12;
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - exact) < 1e-3 * exact, qPrintable(said(r)));
    // A global method's best, polished: more digits than its own.
    const OptimizationResult plain = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "cmaes", "maxiter": 6, "population": 5, "seed": 3,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    const OptimizationResult polished = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "cmaes", "maxiter": 6, "population": 5, "seed": 3, "polish": true,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    QVERIFY2(polished.search.join(QLatin1Char(' ')).contains(QStringLiteral("polish")), qPrintable(said(polished)));
    QVERIFY2(std::abs(value(polished, QStringLiteral("d")) - exact) < std::abs(value(plain, QStringLiteral("d")) - exact),
             qPrintable(said(plain) + QStringLiteral(" | ") + said(polished)));
    // A local method needs none: said.
    const OptimizationResult local = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 30, "polish": true,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    QVERIFY2(local.notes.join(QLatin1Char(' ')).contains(QStringLiteral("polish")), qPrintable(said(local)));
}

// A study's sweep: each row by its worst point, or by its last.
// A mode analysis asked for two modes: a row reads one of them, the one asked,
// else the first (as a solve's globals and fem_evaluate) - not both as a sweep's
// points by the worst, which fitted the minimax of TE0's and TE1's misses.
void TestFemOptimize::aModeAnalysisReadsOneMode()
{
    const QByteArray slab = R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "t", "expression": "0.5[um]"}],
      "components": [{"unit": "um",
        "geometry": [{"type": "rectangle", "name": "below", "position": ["0", "-2"], "size": ["0.1", "2"]},
                     {"type": "rectangle", "name": "core", "size": ["0.1", "t"]},
                     {"type": "rectangle", "name": "above", "position": ["0", "t"], "size": ["0.1", "2"]}],
        "materials": [{"label": "oxide", "n": "1.444", "selection": {"only": ["below", "above"]}},
                      {"label": "silicon", "n": "3.476", "selection": {"only": ["core"]}}],
        "physics": [{"type": "modes", "tag": "emw", "features": [
            {"type": "wave", "selection": {"all": true}},
            {"type": "pmc", "selection": {"of": ["below", "core", "above"], "side": "left"}},
            {"type": "pmc", "selection": {"of": ["below", "core", "above"], "side": "right"}}]}],
        "mesh": {"size": "fine", "features": [{"type": "size", "level": "domain", "selection": {"only": ["core"]}, "hmax": "0.03"},
                                              {"type": "size", "level": "domain", "selection": {"only": ["below", "above"]}, "hmax": "0.1"}]}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "mode", "frequency": "1550[nm]", "modes": 2, "neff": "3.4", "polarization": "tm"}]}],
      "optimizations": [{"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 2,
        "variables": [["t", "", "0.45[um]", "0.55[um]"]],
        "objectives": [["t", "MIN"], ["emw.neff", "MON"], ["emw.neff", "MON", "", "", "", "", "1"], ["emw.neff", "MON", "", "", "", "", "2"],
                       ["emw.neff", "MON", "", "", "", "", "all"]]}]})json";
    const OptimizationResult r = run(slab);
    QVERIFY2(r.error.isEmpty() && !r.history.empty(), qPrintable(said(r)));
    const OptimizationRow& first = r.history.front();
    QVERIFY2(std::isfinite(first.measured.at(1)) && std::isfinite(first.measured.at(3)), qPrintable(said(r)));
    // Two TM modes of a 500 nm silicon slab, the first the higher (3.15 and 2.07).
    QVERIFY2(first.measured.at(2) > first.measured.at(3) && first.measured.at(3) > 1.444,
             qPrintable(QStringLiteral("mode 1 %1, mode 2 %2").arg(first.measured.at(2)).arg(first.measured.at(3))));
    QCOMPARE(first.measured.at(1), first.measured.at(2));   // (empty: the first)
    // All of them: the two read, as before, by the rule of a sweep's points.
    QVERIFY(first.measured.at(4) == first.measured.at(2) || first.measured.at(4) == first.measured.at(3));
}

void TestFemOptimize::aSweepCountsByItsWorst()
{
    // W = C V^2 / 2 at V0 = 1, 3, 2 V; es.Q_1 = C V likewise.
    const QByteArray json = capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 2,
        "variables": [["d", "1[mm]", "0.5[mm]", "2[mm]"]],
        "objectives": [["es.W", "MIN"], ["es.W", "MON", "", "", "last"], ["es.Q_1", "MAX"]]})", {},
        R"({"type": "sweep", "parameters": [["V0", "1[V], 3[V], 2[V]", ""]]}, {"type": "stationary"})");
    const OptimizationResult r = run(json);
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const OptimizationRow& row = r.history.front();
    const double C = Eps0 * 4 * 4e-3 / row.values.at(0);
    QVERIFY2(std::abs(row.measured.at(0) - 0.5 * C * 9) < 1e-6 * C, qPrintable(QStringLiteral("%1 for %2").arg(row.measured.at(0)).arg(4.5 * C)));   // MIN's worst: the largest
    QVERIFY2(std::abs(row.measured.at(1) - 0.5 * C * 4) < 1e-6 * C, qPrintable(QStringLiteral("%1 for %2").arg(row.measured.at(1)).arg(2 * C)));     // the last point's
    QVERIFY2(std::abs(row.measured.at(2) - C * 1) < 1e-6 * C, qPrintable(QStringLiteral("%1 for %2").arg(row.measured.at(2)).arg(C)));               // MAX's worst: the smallest
    // Each candidate's values at each of the sweep's points, a material's too:
    // its parameters are read from the model at each point.
    const OptimizationResult material = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 20,
        "variables": [["er", "4", "1", "12"]],
        "objectives": [["es.C11", "TARGET", "100[pF]"]]})", {},
        R"({"type": "sweep", "parameters": [["V0", "1[V], 2[V]", ""]]}, {"type": "stationary"})"));
    QVERIFY2(material.error.isEmpty(), qPrintable(said(material)));
    const double er = 100e-12 * 1e-3 / (Eps0 * 4e-3);
    QVERIFY2(std::abs(value(material, QStringLiteral("er")) - er) < 1e-3 * er, qPrintable(QStringLiteral("er %1 of %2: ").arg(value(material, QStringLiteral("er"))).arg(er) + said(material)));
    // One swept and varied: the sweep's values solved, said.
    const OptimizationResult both = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 2,
        "variables": [["V0", "", "1[V]", "9[V]"]], "objectives": [["es.W", "MIN"]]})", {},
        R"({"type": "sweep", "parameters": [["V0", "1[V], 2[V]", ""]]}, {"type": "stationary"})"));
    QVERIFY2(both.notes.join(QLatin1Char(' ')).contains(QStringLiteral("swept")), qPrintable(said(both)));
}

// Output times: a slab's boundary heated at a rate, Tmax on it exactly: 300 K at
// the first time, 300 K + rate t at the last. Over "last" reads the last; a
// limit's worst, for GE, the smallest of them; a target's, the farthest.
void TestFemOptimize::outputTimesToo()
{
    const QByteArray json = R"json({"format": "qucs-s multiphysics 1", "parameters": [{"name": "rate", "expression": "2[K/s]"}],
      "components": [{"unit": "mm", "geometry": [{"type": "rectangle", "name": "slab", "size": ["10", "10"]}],
        "materials": [{"label": "m", "k": "1[W/(m*K)]", "rho": "1000[kg/m^3]", "Cp": "1000[J/(kg*K)]", "selection": {"all": true}}],
        "physics": [{"type": "heat", "tag": "ht", "features": [
          {"type": "init", "T0": "300[K]", "selection": {"all": true}},
          {"type": "temperature", "T0": "300[K] + rate*t", "selection": {"of": ["slab"], "side": "left"}}]}],
        "mesh": {"size": "coarse"}}],
      "studies": [{"type": "study", "tag": "std1", "steps": [{"type": "transient", "times": "range(0, 0.5, 2)"}]}],
      "optimizations": [{"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 30, "tol": "1e-4",
        "variables": [["rate", "", "1[K/s]", "20[K/s]", "LOG_DOUBLE"]],
        "objectives": [["ht.Tmax", "TARGET", "310[K]", "", "last"], ["ht.Tmax", "GE", "0[K]"], ["ht.Tmax", "MON"]]}]})json";
    const OptimizationResult r = run(json);
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("rate")) - 5) < 0.05, qPrintable(QStringLiteral("rate %1: ").arg(value(r, QStringLiteral("rate"))) + said(r)));
    QVERIFY2(std::abs(r.measured.at(0).value - 310) < 0.1, qPrintable(said(r)));
    QVERIFY2(std::abs(r.measured.at(1).value - 300) < 1e-6, qPrintable(said(r)));
    QVERIFY2(std::abs(r.measured.at(2).value - 310) < 0.1, qPrintable(said(r)));
}

// A Derived Values node's values, by its tag and its name, or its tag where it
// has one.
void TestFemOptimize::derivedValuesAreRead()
{
    QByteArray json = capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 20,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]],
        "objectives": [["energy", "TARGET", "1[nN]"]]})");
    json.chop(1);
    json += R"(, "results": {"derived": [{"type": "integral", "tag": "energy", "expression": "es.We", "selection": {"all": true}}]}})";
    const OptimizationResult r = run(json);
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    // W = C V^2 / 2 = eps w V^2 / (2 d) = 1 nJ.
    const double exact = Eps0 * 4 * 4e-3 * 25 / (2 * 1e-9);
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - exact) < 1e-3 * exact, qPrintable(QStringLiteral("d %1 of %2: ").arg(value(r, QStringLiteral("d"))).arg(exact) + said(r)));
}

// A row read from another study: each candidate solves both - the second here
// with er = 2 by a sweep of one point, its C half the first's.
void TestFemOptimize::anotherStudysValues()
{
    QByteArray json = capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 14,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "100[pF]", "", "", "std2"], ["es.C11", "MON"]]})");
    json.replace(R"("studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]}],)",
                 R"("studies": [{"type": "study", "tag": "std1", "steps": [{"type": "stationary"}]},
                     {"type": "study", "tag": "std2", "steps": [{"type": "sweep", "parameters": [["er", "2", ""]]}, {"type": "stationary"}]}],)");
    const OptimizationResult r = run(json);
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const double exact = Eps0 * 2 * 4e-3 / 100e-12;
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - exact) < 1e-3 * exact, qPrintable(said(r)));
    // The row of the first study there: er = 4, twice.
    QVERIFY2(std::abs(r.measured.at(1).value - 200e-12) < 1e-3 * 200e-12, qPrintable(said(r)));
    // No such study: said before any solve.
    QByteArray missing = json;
    missing.replace("\"std2\"]", "\"nosuch\"]");
    const OptimizationResult no = run(missing);
    QVERIFY2(no.error.contains(QStringLiteral("nosuch")), qPrintable(said(no)));
}

// A whole number and a preferred value: never between them.
void TestFemOptimize::wholeNumbersAndSeries()
{
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 10, "population": 8, "seed": 1,
        "variables": [["er", "4", "1", "12", "LIN_INT"], ["d", "1[mm]", "0.1[mm]", "5[mm]", "E12"]],
        "objectives": [["es.C11", "TARGET", "300[pF]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const QList<double> series = qucs_s::optimization::eSeries(QStringLiteral("E12"));
    for (const OptimizationRow& row : r.history) {
        QVERIFY2(row.values.at(0) == std::round(row.values.at(0)), "not a whole number");
        const double scaled = row.values.at(1) * 1e4;
        bool preferred = false;
        for (double s : series)
            for (double decade : {1.0, 10.0, 100.0}) preferred = preferred || std::abs(scaled - s * decade) < 1e-6 * s * decade;
        QVERIFY2(preferred, qPrintable(QString::number(row.values.at(1))));
    }
    // Candidates that round to the same values are solved once.
    QVERIFY2(r.solves < r.evaluations, qPrintable(said(r)));
    // A warning for a method that cannot see steps.
    const OptimizationResult flat = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 4,
        "variables": [["er", "4", "1", "12", "LIN_INT"]], "objectives": [["es.C11", "TARGET", "300[pF]"]]})"));
    QVERIFY2(flat.notes.join(QLatin1Char(' ')).contains(QStringLiteral("flat")), qPrintable(said(flat)));
}

// A stop: within the evaluation at hand, the best so far kept, its status said.
void TestFemOptimize::aStopKeepsTheBest()
{
    for (const char* method : {"nm", "tr", "de", "pso", "sa", "cmaes", "bayes", "nsga2", "lm", "tune"}) {
        const bool two = QLatin1String(method) == QLatin1String("nsga2");
        const QByteArray section = QByteArray(R"({"type": "optimization", "tag": "opt1", "maxiter": 5000, "seed": 2, "method": ")") + method + R"(",
            "variables": [["d", "", "0.2[mm]", "3[mm]"]],
            "objectives": [["es.C11", "TARGET", "200[pF]"])" + (two ? R"(, ["d", "MIN"])" : "") + R"(]})";
        const Model m = modelOf(capacitor(section));
        std::atomic<int> rows{0};
        OptimizationListener listener;
        listener.row = [&rows](const OptimizationRow&, const QStringList&) { ++rows; };
        // Stopped by its own count of solves: at the fifth.
        const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr,
                                                     [&rows](double, const QString&) { return rows.load() < 5; }, listener);
        QVERIFY2(r.error.isEmpty(), qPrintable(QLatin1String(method) + said(r)));
        QCOMPARE(r.status, QStringLiteral("interrupted"));
        QVERIFY2(r.solves >= 5 && r.solves <= 6, qPrintable(QLatin1String(method) + said(r)));
        QVERIFY2(int(r.history.size()) == r.solves, qPrintable(said(r)));
        if (two) continue;
        QVERIFY2(r.hasBest(), qPrintable(QLatin1String(method) + said(r)));
        // The best of those solved.
        double best = 1e300;
        for (const OptimizationRow& row : r.history) best = std::min(best, row.cost);
        double bestAt = NAN;
        for (const OptimizationRow& row : r.history)
            if (row.cost == best) bestAt = row.values.at(0);
        QCOMPARE(value(r, QStringLiteral("d")), bestAt);
        QVERIFY(r.canApply());
    }
}

void TestFemOptimize::aStopBeforeAnythingIsSolved()
{
    const Model m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm",
        "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr, [](double, const QString&) { return false; });
    QCOMPARE(r.status, QStringLiteral("interrupted"));
    QVERIFY(r.hasBest() == false);
    QVERIFY(r.history.empty());
    QString why;
    QVERIFY(!r.canApply(&why));
    QVERIFY(!why.isEmpty());
}

// A candidate that does not solve is one bad point: below 0.5 mm the gap's
// permittivity is not a number, the search goes on and ends where it should.
void TestFemOptimize::aFailedSolveDuringARun()
{
    for (const char* method : {"de", "cmaes", "nm", "tr", "bayes", "pso", "sa", "lm", "tune"}) {
        const OptimizationResult r = run(capacitor(QByteArray(R"({"type": "optimization", "tag": "opt1", "maxiter": 40, "seed": 2, "population": 8, "method": ")") + method + R"(",
            "variables": [["d", "1.5[mm]", "0.1[mm]", "3[mm]"]],
            "objectives": [["es.C11", "TARGET", "200[pF]"]]})").replace(R"("epsilonr": "er")", R"x("epsilonr": "if(d < 0.5[mm], 0/0*er, er)")x"));
        QVERIFY2(r.error.isEmpty(), qPrintable(QLatin1String(method) + said(r)));
        const double exact = Eps0 * 4 * 4e-3 / 200e-12;
        if (QLatin1String(method) != QLatin1String("sa") && QLatin1String(method) != QLatin1String("pso")) {
            QVERIFY2(std::abs(value(r, QStringLiteral("d")) - exact) < 0.05 * exact,
                     qPrintable(QLatin1String(method) + QStringLiteral(" d %1 of %2: ").arg(value(r, QStringLiteral("d"))).arg(exact) + said(r)));
        }
        QVERIFY2(r.hasBest(), qPrintable(QLatin1String(method) + said(r)));
        for (const OptimizationRow& row : r.history)
            if (!std::isnan(row.cost)) QVERIFY2(row.values.at(0) >= 0.5e-3, "a candidate that should not solve did");
        // Where one fails, said; its row says why.
        if (r.failed > 0) {
            QVERIFY2(r.notes.join(QLatin1Char(' ')).contains(QStringLiteral("did not solve")), qPrintable(said(r)));
            bool told = false;
            for (const OptimizationRow& row : r.history) told = told || (std::isnan(row.cost) && !row.why.isEmpty());
            QVERIFY2(told, qPrintable(said(r)));
        }
    }
    // The start's not solving is not the end either.
    const OptimizationResult start = run(capacitor(R"({"type": "optimization", "tag": "opt1", "maxiter": 40, "seed": 2, "population": 8, "method": "de",
        "variables": [["d", "0.2[mm]", "0.1[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})").replace(R"("epsilonr": "er")", R"x("epsilonr": "if(d < 0.5[mm], 0/0*er, er)")x"));
    QVERIFY2(start.error.isEmpty() && start.hasBest(), qPrintable(said(start)));
    QVERIFY2(start.failed > 0, qPrintable(said(start)));
    QVERIFY2(std::isnan(start.history.front().cost), qPrintable(said(start)));
    QVERIFY2(!start.history.front().why.isEmpty(), qPrintable(said(start)));
}

void TestFemOptimize::nothingSolves()
{
    // A study that cannot be solved: no material's permittivity.
    for (const char* method : {"nm", "tr", "de", "pso", "sa", "cmaes", "bayes", "lm", "nsga2"}) {
        const bool two = QLatin1String(method) == QLatin1String("nsga2");
        const QByteArray section = QByteArray(R"({"type": "optimization", "tag": "opt1", "maxiter": 6, "seed": 2, "population": 8, "method": ")") + method + R"(",
            "variables": [["d", "", "0.2[mm]", "3[mm]"]],
            "objectives": [["es.C11", "TARGET", "200[pF]"])" + (two ? R"(, ["d", "MIN"])" : "") + R"(]})";
        const OptimizationResult r = run(capacitor(section).replace(R"("epsilonr": "er")", R"("epsilonr": "0/0")"));
        QVERIFY2(r.error.isEmpty(), qPrintable(QLatin1String(method) + said(r)));
        QCOMPARE(r.status, QStringLiteral("nosolve"));
        QVERIFY(!r.hasBest());
        QVERIFY(!r.canApply());
        QVERIFY2(r.notes.join(QLatin1Char(' ')).contains(QStringLiteral("first that did not solve")), qPrintable(QLatin1String(method) + said(r)));
    }
}

// What is asked that the results have not: no candidate would have it; the run
// ends there, with what they have.
void TestFemOptimize::whatTheResultsLackEndsTheRun()
{
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "de", "maxiter": 40,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.nosuch", "MIN"]]})"));
    QVERIFY2(r.error.contains(QStringLiteral("es.nosuch")), qPrintable(said(r)));
    QVERIFY2(r.error.contains(QStringLiteral("es.C11")), qPrintable(said(r)));
    // A field, not one number.
    const OptimizationResult f = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "de",
        "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.normE", "MIN"]]})"));
    QVERIFY2(f.error.contains(QStringLiteral("varies")), qPrintable(said(f)));
    // A value of another unit.
    const OptimizationResult u = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm",
        "variables": [["d", "", "0.2[mm]", "3[mm]"]], "objectives": [["es.C11", "TARGET", "5[V]"]]})"));
    QVERIFY2(u.error.contains(QStringLiteral("es.C11")), qPrintable(said(u)));
}

void TestFemOptimize::theObjectiveNeverMoves()
{
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 20,
        "variables": [["V0", "", "1[V]", "9[V]"]], "objectives": [["es.C11", "MIN"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QCOMPARE(r.status, QStringLiteral("unchanged"));
    QVERIFY2(r.notes.join(QLatin1Char(' ')).contains(QStringLiteral("nothing was optimised")), qPrintable(said(r)));
    // (V0 shapes nothing: one mesh for all, and no word of meshing again.)
    QVERIFY2(!r.notes.join(QLatin1Char(' ')).contains(QStringLiteral("meshed again")), qPrintable(said(r)));
}

void TestFemOptimize::aParameterOnItsBound()
{
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 40,
        "variables": [["d", "1[mm]", "0.5[mm]", "2[mm]"]], "objectives": [["es.C11", "MIN"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - 2e-3) < 1e-9, qPrintable(said(r)));
    QVERIFY2(r.notes.join(QLatin1Char(' ')).contains(QStringLiteral("bound")), qPrintable(said(r)));
    // The cap on iterations, said.
    const OptimizationResult cap = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "cmaes", "maxiter": 2, "seed": 1,
        "variables": [["d", "1[mm]", "0.5[mm]", "2[mm]"]], "objectives": [["es.C11", "TARGET", "100[pF]"]]})"));
    QCOMPARE(cap.status, QStringLiteral("maxiter"));
    QVERIFY(!cap.settled);
    QVERIFY2(cap.notes.join(QLatin1Char(' ')).contains(QStringLiteral("iteration cap")), qPrintable(said(cap)));
}

void TestFemOptimize::badSetups_data()
{
    QTest::addColumn<QByteArray>("section");
    QTest::addColumn<QString>("says");
    auto row = [](const char* name, const QByteArray& section, const char* says) {
        QTest::newRow(name) << QByteArray(R"({"type": "optimization", "tag": "opt1", )" + section + "}") << QString::fromLatin1(says);
    };
    const QByteArray goal = R"("objectives": [["es.C11", "MIN"]])";
    const QByteArray range = R"("variables": [["d", "", "0.2[mm]", "3[mm]"]])";
    row("no variable", goal, "varies no parameter");
    row("no objective", range, "nothing to reach");
    row("only monitored", range + R"(, "objectives": [["es.C11", "MON"]])", "nothing to reach");
    row("no such parameter", QByteArray(R"("variables": [["nosuch", "", "1", "2"]], )") + goal, "no parameter nosuch");
    row("minimum above maximum", QByteArray(R"("variables": [["d", "", "3[mm]", "1[mm]"]], )") + goal, "above the minimum");
    row("equal bounds", QByteArray(R"("variables": [["d", "", "1[mm]", "1[mm]"]], )") + goal, "above the minimum");
    row("no minimum", QByteArray(R"("variables": [["d", "", "", "1[mm]"]], )") + goal, "minimum and a maximum");
    row("no unit", QByteArray(R"("variables": [["d", "", "1", "2"]], )") + goal, "give it a unit");
    row("another unit", QByteArray(R"("variables": [["d", "", "1[V]", "2[V]"]], )") + goal, "in V");
    row("junk range", QByteArray(R"("variables": [["d", "", "((", "2[mm]"]], )") + goal, "minimum");
    row("infinite range", QByteArray(R"("variables": [["d", "", "1[mm]", "1/0*1[mm]"]], )") + goal, "maximum");
    row("twice", QByteArray(R"("variables": [["d", "", "1[mm]", "2[mm]"], ["d", "", "1[mm]", "2[mm]"]], )") + goal, "twice");
    row("bad type", QByteArray(R"("variables": [["d", "", "1[mm]", "2[mm]", "LOGARITHMIC"]], )") + goal, "LOGARITHMIC");
    row("series below zero", QByteArray(R"("variables": [["V0", "", "-1[V]", "2[V]", "E12"]], )") + goal, "above 0");
    row("no whole number", QByteArray(R"("variables": [["er", "", "1.2", "1.8", "LIN_INT"]], )") + goal, "no whole number");
    row("none active", QByteArray(R"("variables": [["d", "", "1[mm]", "2[mm]", "", "no"]], )") + goal, "varies no parameter");
    row("bad kind", range + R"(, "objectives": [["es.C11", "BEST"]])", "BEST");
    row("target without a value", range + R"(, "objectives": [["es.C11", "TARGET"]])", "needs a value");
    row("junk value", range + R"(, "objectives": [["es.C11", "LE", "(("]])", "value");
    row("zero weight", range + R"(, "objectives": [["es.C11", "MIN", "", "0"]])", "weight");
    row("negative weight", range + R"(, "objectives": [["es.C11", "MIN", "", "-1"]])", "weight");
    row("bad over", range + R"(, "objectives": [["es.C11", "MIN", "", "", "best"]])", "worst or last");
    row("no such study", range + R"(, "study": "nosuch", )" + goal, "no study nosuch");
    row("no such method", range + R"(, "method": "newton", )" + goal, "newton");
    row("lm without targets", range + R"(, "method": "lm", )" + goal, "fits targets");
    row("lm with penalties", range + R"(, "method": "lm", "constraints": "penalty", "objectives": [["es.C11", "TARGET", "1[pF]"], ["es.W", "LE", "1[J]"]])", "augmented Lagrangian");
    row("nsga2 of one", range + R"(, "method": "nsga2", )" + goal, "at least two objectives");
    row("nsga2 polished", range + R"(, "method": "nsga2", "polish": true, "objectives": [["es.C11", "MIN"], ["d", "MIN"]])", "cannot be combined");
    row("nsga2 of nine", range + R"(, "method": "nsga2", "objectives": [["d", "MIN"], ["d", "MIN"], ["d", "MIN"], ["d", "MIN"], ["d", "MIN"], ["d", "MIN"], ["d", "MIN"], ["d", "MIN"], ["d", "MIN"]])", "more than 8");
    row("tune of two", range + R"(, "method": "tune", "objectives": [["es.C11", "TARGET", "1[pF]"], ["es.W", "TARGET", "1[J]"]])", "as many targets");
    row("zero iterations", range + R"(, "maxiter": 0, )" + goal, "whole number");
    row("half iterations", range + R"(, "maxiter": 2.5, )" + goal, "whole number");
    row("a huge population", range + R"(, "population": 2147483647, )" + goal, "whole number");
    row("negative seed", range + R"(, "seed": -1, )" + goal, "seed");
    row("no tolerance", range + R"(, "tol": "0", )" + goal, "tolerance");
    row("junk tolerance", range + R"(, "tol": "((", )" + goal, "not a number");
    row("no spreads", range + R"(, "mode": "center", "objectives": [["es.C11", "LE", "1[pF]"]])", "spread");
    row("a spread with no unit", range + R"(, "mode": "center", "variation": [["d", "gauss", "0.02"]], "objectives": [["es.C11", "LE", "1[pF]"]])", "or a share of its value: 5 %");
    row("a value that moves with a varied parameter", range + R"(, "objectives": [["es.C11", "TARGET", "200[pF]*d/1[mm]"]])", "(es.C11) - (200[pF]*d/1[mm]) TARGET 0");
    row("mode 0", range + R"(, "objectives": [["es.C11", "MIN", "", "", "", "", "0"]])", "a mode's number from 1, or all");
    row("mode junk", range + R"(, "objectives": [["es.C11", "MIN", "", "", "", "", "first"]])", "a mode's number from 1, or all");
    row("a junk spread", range + R"(, "mode": "center", "variation": [["d", "gauss", "(("]], "objectives": [["es.C11", "LE", "1[pF]"]])", "spread");
    row("a spread of nothing", range + R"(, "mode": "center", "variation": [["nosuch", "gauss", "1 %"]], "objectives": [["es.C11", "LE", "1[pF]"]])", "no parameter");
    row("centering with a goal", range + R"(, "mode": "center", "variation": [["d", "gauss", "1 %"]], "objectives": [["es.C11", "MIN"]])", "cannot be combined");
    row("variables not a table", QByteArray(R"("variables": {"a": 1}, )") + goal, "not a table");
    row("a row not a list", QByteArray(R"("variables": [5], )") + goal, "not a list");
    row("objectives not a table", range + R"(, "objectives": "x")", "not a table");
}

// A setup that cannot be run: said before anything is solved.
void TestFemOptimize::badSetups()
{
    QFETCH(QByteArray, section);
    QFETCH(QString, says);
    int solves = 0;
    OptimizationListener listener;
    listener.row = [&solves](const OptimizationRow&, const QStringList&) { ++solves; };
    const Model m = modelOf(capacitor(section));
    const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr, {}, listener);
    QVERIFY2(r.error.contains(says), qPrintable(said(r)));
    QCOMPARE(solves, 0);
    QVERIFY(!r.canApply());
}

void TestFemOptimize::theShippedExample_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("maxiter");
    QTest::addColumn<double>("tolerance");
    QTest::newRow("as it is") << QString() << 0 << 5e-3;
    QTest::newRow("nm") << QStringLiteral("nm") << 120 << 2e-2;
    QTest::newRow("cmaes") << QStringLiteral("cmaes") << 30 << 3e-2;
}

// The least area for 100 pF within a field: d = V / E = 1 mm, w = C d / eps.
void TestFemOptimize::theShippedExample()
{
    QFETCH(QString, method);
    QFETCH(int, maxiter);
    QFETCH(double, tolerance);
    QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/capacitor_optimization.qfem"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    Model m = modelOf(f.readAll());
    if (!method.isEmpty()) {
        m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("method"), method);
        m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("maxiter"), maxiter);
        m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("seed"), 2);
    }
    const OptimizationResult r = runOptimization(m, {}, nullptr, nullptr, nullptr);
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(r.feasible, qPrintable(said(r)));
    const double d = 1e-3, w = 100e-12 * d / (Eps0 * 4);
    QVERIFY2(std::abs(value(r, QStringLiteral("d")) - d) < tolerance * d, qPrintable(QStringLiteral("d %1 of %2: ").arg(value(r, QStringLiteral("d"))).arg(d) + said(r) + r.constraints.join(QLatin1String(" | "))));
    QVERIFY2(std::abs(value(r, QStringLiteral("w")) - w) < tolerance * w, qPrintable(QStringLiteral("w %1 of %2: ").arg(value(r, QStringLiteral("w"))).arg(w) + said(r)));
    QVERIFY2(std::abs(value(r, QStringLiteral("w*d")) - w * d) < 2 * tolerance * w * d, qPrintable(said(r)));
}

void TestFemOptimize::oldFilesHaveNoSection()
{
    // Every model shipped: read, no optimization; written, no section; asked to
    // run, a sentence.
    const QDir dir(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics"));
    int read = 0;
    for (const QString& name : dir.entryList({QStringLiteral("*.qfem")})) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        const bool has = bytes.contains("\"optimizations\"");
        QString why;
        std::optional<Model> m = Model::fromJson(bytes, &why);
        QVERIFY2(m, qPrintable(name + why));
        const QJsonObject written = m->toObject();
        QCOMPARE(written.contains(QStringLiteral("optimizations")), has);
        QCOMPARE(optimizationNodes(*m).empty(), !has);
        if (!has) {
            const OptimizationResult r = runOptimization(*m, {}, nullptr, nullptr, nullptr);
            QVERIFY2(r.error.contains(QStringLiteral("no optimization")), qPrintable(r.error));
        }
        ++read;
    }
    QVERIFY(read > 30);
    // A blank one, too; and it round-trips with none added.
    const Model blank = Model::blank();
    QVERIFY(!blank.toObject().contains(QStringLiteral("optimizations")));
}

void TestFemOptimize::theSectionRoundTrips()
{
    const QByteArray section = R"({"type": "optimization", "tag": "opt1", "label": "Tune the gap", "method": "cmaes", "maxiter": 33, "tol": "1e-5", "population": 7, "seed": 9,
        "polish": true, "starts": 2, "ctol": "1e-3", "constraints": "penalty", "objectiveweight": "5", "constraintweight": "50", "parallel": 2, "refresh": 3,
        "variables": [["d", "1[mm]", "0.2[mm]", "3[mm]", "LOG_DOUBLE", "yes"], ["w", "", "1[mm]", "9[mm]", "", "no"]],
        "objectives": [["es.C11", "TARGET", "200[pF]", "2", "last", "std1"], ["es.W", "LE", "1[uJ]"], ["w*d", "MON"]]})";
    const Model m = modelOf(capacitor(section));
    const std::vector<const Node*> nodes = optimizationNodes(m);
    QCOMPARE(int(nodes.size()), 1);
    QCOMPARE(nodes.front()->tag, QStringLiteral("opt1"));
    QCOMPARE(nodes.front()->label, QStringLiteral("Tune the gap"));
    QCOMPARE(nodes.front()->integer(QStringLiteral("maxiter")), 33);
    QVERIFY(nodes.front()->flag(QStringLiteral("polish")));
    // Written and read again: the same.
    const QByteArray once = m.toJson();
    QString why;
    std::optional<Model> again = Model::fromJson(once, &why);
    QVERIFY2(again, qPrintable(why));
    QVERIFY2(why.isEmpty(), qPrintable(why));
    QCOMPARE(again->toObject(), m.toObject());
    QCOMPARE(again->toJson(), once);
    QVERIFY(m.toObject().value(QStringLiteral("optimizations")).toArray().size() == 1);
    // Under their node, between the studies and the results in the tree, added or read.
    int study = -1, optimization = -1, results = -1;
    for (int i = 0; i < int(m.root.children.size()); ++i) {
        const QString& type = m.root.children[std::size_t(i)].type;
        QVERIFY(type != QLatin1String("optimization"));
        if (type == QLatin1String("study")) study = i;
        if (type == QLatin1String("optimizations")) optimization = i;
        if (type == QLatin1String("results")) results = i;
    }
    QVERIFY(study >= 0 && study < optimization && optimization < results);
    const Node* group = m.root.child(QStringLiteral("optimizations"));
    QCOMPARE(int(group->children.size()), 1);
    QCOMPARE(group->children.front().type, QStringLiteral("optimization"));
    QVERIFY(m.root.child(QStringLiteral("optimizations"))->kind()->fixed);
    // Added as any node.
    Model blank = Model::blank();
    Node made = blank.make(QStringLiteral("optimization"));
    QVERIFY(made.kind());
    QString refused;
    Node* added = blank.add(QStringLiteral("model"), made, -1, &refused);
    QVERIFY2(added, qPrintable(refused));
    QCOMPARE(added->tag, QStringLiteral("opt1"));
    QVERIFY(blank.root.children.back().type == QLatin1String("results"));
    // There from the start, under it whichever way it is asked for, and no file's
    // business while it is empty.
    QCOMPARE(added, &blank.root.child(QStringLiteral("optimizations"))->children.front());
    QVERIFY(blank.add(QStringLiteral("optimizations"), blank.make(QStringLiteral("optimization")), -1, &refused));
    QCOMPARE(int(blank.root.child(QStringLiteral("optimizations"))->children.size()), 2);
    QVERIFY(!Model::blank().toObject().contains(QStringLiteral("optimizations")));
    QVERIFY(Model::blank().root.child(QStringLiteral("optimizations")));
    // A study added comes before it.
    QVERIFY(blank.add(QStringLiteral("model"), blank.make(QStringLiteral("study")), -1, &refused));
    int lastStudy = -1, node = -1;
    for (int i = 0; i < int(blank.root.children.size()); ++i) {
        if (blank.root.children[std::size_t(i)].type == QLatin1String("study")) lastStudy = i;
        if (blank.root.children[std::size_t(i)].type == QLatin1String("optimizations")) node = i;
    }
    QVERIFY(lastStudy >= 0 && lastStudy < node);
    // Not a 3D model's.
    Model three = Model::blank3d();
    QVERIFY(!three.root.child(QStringLiteral("optimizations")));
    QVERIFY(!three.add(QStringLiteral("model"), three.make(QStringLiteral("optimization")), -1, &refused));
    // Its plan as the setting says.
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    const std::optional<OptimizationPlan> plan = optimizationPlan(m, *nodes.front(), *ps, &why);
    QVERIFY2(plan, qPrintable(why));
    QCOMPARE(plan->search.maxIter, 33);
    QCOMPARE(plan->search.population, 7);
    QCOMPARE(plan->search.seed, 9ULL);
    QCOMPARE(plan->search.starts, 2);
    QCOMPARE(plan->search.polish, true);
    QCOMPARE(plan->parallel, 2);
    QCOMPARE(plan->penalty, true);
    QCOMPARE(plan->objectiveWeight, 5.0);
    QCOMPARE(int(plan->active().size()), 1);
    QCOMPARE(plan->variables.at(0).min, 0.2e-3);
    QCOMPARE(plan->variables.at(0).initial, 1e-3);
    QVERIFY(plan->variables.at(0).logarithmic());
    QVERIFY(!plan->variables.at(1).active);
    QCOMPARE(plan->variables.at(1).initial, 4e-3);
    QCOMPARE(plan->objectives.at(0).weight, 2.0);
    QVERIFY(plan->objectives.at(0).last);
    QVERIFY(!plan->objectives.at(1).last);
}

void TestFemOptimize::resultsReadBack()
{
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 30,
        "variables": [["d", "", "0.2[mm]", "3[mm]"], ["w", "", "1[mm]", "9[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"], ["es.W", "MON"], ["es.W", "LE", "1[J]"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    const QJsonObject o = r.toJson();
    // The file's own form.
    const QByteArray text = QJsonDocument(o).toJson();
    const OptimizationResult back = OptimizationResult::fromJson(QJsonDocument::fromJson(text).object());
    QCOMPARE(back.status, r.status);
    QCOMPARE(back.summary, r.summary);
    QCOMPARE(back.method, r.method);
    QCOMPARE(back.evaluations, r.evaluations);
    QCOMPARE(back.solves, r.solves);
    QCOMPARE(back.failed, r.failed);
    QCOMPARE(back.feasible, r.feasible);
    QCOMPARE(back.settled, r.settled);
    QCOMPARE(back.notes, r.notes);
    QCOMPARE(back.search, r.search);
    QCOMPARE(back.constraints, r.constraints);
    QCOMPARE(back.columns, r.columns);
    QCOMPARE(back.kinds, r.kinds);
    QCOMPARE(back.units, r.units);
    QCOMPARE(back.parameters.size(), r.parameters.size());
    for (int i = 0; i < r.parameters.size(); ++i) {
        QCOMPARE(back.parameters.at(i).name, r.parameters.at(i).name);
        QCOMPARE(back.parameters.at(i).value, r.parameters.at(i).value);
        QCOMPARE(back.parameters.at(i).text, r.parameters.at(i).text);
    }
    QCOMPARE(back.measured.size(), r.measured.size());
    QCOMPARE(back.history.size(), r.history.size());
    for (std::size_t i = 0; i < r.history.size(); ++i) {
        QCOMPARE(back.history[i].index, r.history[i].index);
        QCOMPARE(back.history[i].values.size(), r.history[i].values.size());
        QVERIFY(std::abs(back.history[i].values[0] - r.history[i].values[0]) <= 1e-9 * std::abs(r.history[i].values[0]));
        QCOMPARE(back.history[i].best, r.history[i].best);
        QCOMPARE(back.history[i].feasible, r.history[i].feasible);
    }
    // A front's too.
    const OptimizationResult p = run(bar(R"({"type": "optimization", "tag": "opt1", "method": "nsga2", "maxiter": 3, "population": 8, "seed": 7,
        "variables": [["w", "2[mm]", "1[mm]", "9[mm]"]], "objectives": [["ec.R", "MIN"], ["w", "MIN"]]})"));
    const OptimizationResult pback = OptimizationResult::fromJson(p.toJson());
    QCOMPARE(pback.front.size(), p.front.size());
    QVERIFY(!pback.front.empty());
    QCOMPARE(pback.front[0].values, p.front[0].values);
    QCOMPARE(pback.front[0].measured, p.front[0].measured);
    // A row that could not be read: left out, not a crash. A failed one's row keeps NaN.
    OptimizationResult fail;
    fail.columns = QStringList{QStringLiteral("d"), QStringLiteral("es.C11")};
    fail.kinds = QStringList{QStringLiteral("MIN")};
    OptimizationRow row;
    row.index = 1;
    row.values = {1e-3};
    row.measured = {NAN};
    row.feasible = true;
    fail.history.push_back(row);
    const OptimizationResult failBack = OptimizationResult::fromJson(fail.toJson());
    QCOMPARE(int(failBack.history.size()), 1);
    QVERIFY(std::isnan(failBack.history[0].measured[0]));
    QVERIFY(std::isnan(failBack.history[0].cost));
}

void TestFemOptimize::aHostileResultIsHarmless()
{
    // Each of these as a node's result: read, shown as nothing much, not written in.
    const QList<QJsonValue> hostile = {
        5, QStringLiteral("x"), QJsonArray{1, 2}, QJsonObject{}, QJsonObject{{QStringLiteral("status"), 3}},
        QJsonObject{{QStringLiteral("parameters"), 7}}, QJsonObject{{QStringLiteral("parameters"), QJsonArray{5, QStringLiteral("a"), QJsonArray{}}}},
        QJsonObject{{QStringLiteral("parameters"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("d")}}}}},
        QJsonObject{{QStringLiteral("columns"), QJsonArray{QStringLiteral("d")}}, {QStringLiteral("kinds"), QJsonArray{QStringLiteral("a"), QStringLiteral("b")}}, {QStringLiteral("history"), QJsonArray{QStringLiteral("1 2 3")}}},
        QJsonObject{{QStringLiteral("columns"), QJsonArray{QStringLiteral("d"), QStringLiteral("e")}}, {QStringLiteral("kinds"), QJsonArray{QStringLiteral("a")}}, {QStringLiteral("history"), QJsonArray{5, QStringLiteral("x y z 1 2 3"), QStringLiteral("1 nan inf 1e999 1 1")}}},
        QJsonObject{{QStringLiteral("front"), 5}}, QJsonObject{{QStringLiteral("front"), QJsonArray{5, QJsonObject{{QStringLiteral("values"), 3}}}}},
        QJsonObject{{QStringLiteral("front"), QJsonArray{QJsonObject{{QStringLiteral("values"), QJsonArray{1e308, QStringLiteral("a")}}}}}, {QStringLiteral("columns"), QJsonArray{QStringLiteral("d")}}},
        QJsonObject{{QStringLiteral("error"), QStringLiteral("no")}},
    };
    for (const QJsonValue& v : hostile) {
        Model m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1"})"));
        Node* n = m.find(QStringLiteral("opt1"));
        n->props.insert(QStringLiteral("result"), v);
        const OptimizationResult r = OptimizationResult::fromJson(v.toObject());
        (void) r.toJson();
        (void) r.canApply();
        // Written in at any point: it refuses or writes only what it can.
        for (int point : {-1, 0, 1, 2, 1000000}) {
            Model copy = m;
            QStringList written;
            QString why;
            const bool ok = applyOptimum(copy, *copy.find(QStringLiteral("opt1")), point, &written, &why);
            if (ok) QVERIFY(!written.isEmpty());
            else QVERIFY(!why.isEmpty());
        }
    }
}

void TestFemOptimize::aRowOfAnotherLengthIsNotRead()
{
    // Two columns, one measured: 1 + 1 + 1 + 3 cells. A row cut short or run on
    // is not read, and those about it are.
    QJsonObject o{{QStringLiteral("columns"), QJsonArray{QStringLiteral("d"), QStringLiteral("es.C11")}},
                  {QStringLiteral("kinds"), QJsonArray{QStringLiteral("MIN")}},
                  {QStringLiteral("history"), QJsonArray{QStringLiteral("1 2 3 4 1 0"), QString(), QStringLiteral("2"), QStringLiteral("3 2 3"), QStringLiteral("4 2 3 4 1"),
                                                         QStringLiteral("5 2 3 4 1 0 9"), QStringLiteral("6 2 3 4 1 1")}}};
    const OptimizationResult r = OptimizationResult::fromJson(o);
    QCOMPARE(int(r.history.size()), 2);
    QCOMPARE(r.history[0].index, 1);
    QCOMPARE(r.history[1].index, 6);
    QCOMPARE(r.history[1].cost, 4.0);
    QVERIFY(r.history[1].best);
}

void TestFemOptimize::aLongHistoryIsCutInTheFile()
{
    OptimizationResult r;
    r.method = QStringLiteral("de");
    r.status = QStringLiteral("converged");
    r.columns = QStringList{QStringLiteral("d"), QStringLiteral("es.C11")};
    r.kinds = QStringList{QStringLiteral("MIN")};
    for (int i = 1; i <= 5000; ++i) {
        OptimizationRow row;
        row.index = i;
        row.values = {double(i)};
        row.measured = {double(i)};
        row.cost = double(i);
        row.best = i % 500 == 0;
        r.history.push_back(row);
    }
    const OptimizationResult back = OptimizationResult::fromJson(r.toJson(100));
    QCOMPARE(int(back.history.size()), 100);
    // Those that improved kept; the rest the latest.
    int best = 0;
    for (const OptimizationRow& row : back.history) best += row.best ? 1 : 0;
    QCOMPARE(best, 10);
    QCOMPARE(back.history.back().index, 5000);
    QCOMPARE(back.history[back.history.size() - 2].index, 4999);
    // More that improved than there is room for: all of them all the same.
    const OptimizationResult few = OptimizationResult::fromJson(r.toJson(5));
    QCOMPARE(int(few.history.size()), 10);
    // Each row a line of the file.
    const QByteArray text = QJsonDocument(r.toJson(100)).toJson();
    QVERIFY(text.count('\n') < 200);
}

void TestFemOptimize::theOptimumIsWrittenIn()
{
    Model m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 20,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    QStringList written;
    QString why;
    QVERIFY(!applyOptimum(m, *m.find(QStringLiteral("opt1")), 0, &written, &why));
    QVERIFY2(why.contains(QStringLiteral("not been run")), qPrintable(why));
    const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("result"), r.toJson());
    QVERIFY2(applyOptimum(m, *m.find(QStringLiteral("opt1")), 0, &written, &why), qPrintable(why));
    QCOMPARE(written.size(), 1);
    // In the unit it was written in; its value the optimum's; the others left.
    QString expression;
    for (const Model::Parameter& p : m.parameters())
        if (p.name == QLatin1String("d")) expression = p.expression;
    QVERIFY2(expression.endsWith(QStringLiteral("[mm]")), qPrintable(expression));
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    QVERIFY2(std::abs(ps->values.value(QStringLiteral("d")) - value(r, QStringLiteral("d"))) < 1e-9 * value(r, QStringLiteral("d")), qPrintable(expression));
    QCOMPARE(ps->values.value(QStringLiteral("w")), 4e-3);
    // Solved there: what it said it would be.
    const GeometryBuild g = buildGeometry(m, *ps);
    const StudyRun solved = runStudy(m, ps, g.topology, nullptr, QString());
    QVERIFY2(solved.ok(), qPrintable(solved.error));
    QVERIFY(std::abs(*solved.solutions.back()->global(QStringLiteral("es.C11")) - 200e-12) < 1e-4 * 200e-12);
    // An expression, not a number with a unit: in SI; none where it has no unit.
    Model derived = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 15,
        "variables": [["h", "", "0.2[mm]", "3[mm]"], ["er", "", "1", "9"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"], ["er", "TARGET", "3"]]})", R"(, {"name": "h", "expression": "2*0.5[mm]"})").replace(R"("size": ["w", "d"])", R"("size": ["w", "h"])").replace(R"(max(d, w)/4)", R"(max(h, w)/4)"));
    const OptimizationResult dr = runOptimization(derived, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QVERIFY2(dr.error.isEmpty(), qPrintable(said(dr)));
    derived.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("result"), dr.toJson());
    QVERIFY2(applyOptimum(derived, *derived.find(QStringLiteral("opt1")), 0, &written, &why), qPrintable(why));
    QCOMPARE(written.size(), 2);
    for (const Model::Parameter& p : derived.parameters()) {
        if (p.name == QLatin1String("h")) QVERIFY2(p.expression.endsWith(QStringLiteral("[m]")), qPrintable(p.expression));
        if (p.name == QLatin1String("er")) QVERIFY2(!p.expression.contains(QLatin1Char('[')), qPrintable(p.expression));
    }
}

void TestFemOptimize::aFrontsDesignIsChosen()
{
    Model m = modelOf(bar(R"({"type": "optimization", "tag": "opt1", "method": "nsga2", "maxiter": 4, "population": 8, "seed": 7,
        "variables": [["w", "2[mm]", "1[mm]", "9[mm]"]], "objectives": [["ec.R", "MIN"], ["w", "MIN"]]})"));
    const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QVERIFY2(!r.front.empty(), qPrintable(said(r)));
    m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("result"), r.toJson());
    QStringList written;
    QString why;
    // No one optimum: which?
    QVERIFY(!applyOptimum(m, *m.find(QStringLiteral("opt1")), 0, &written, &why));
    QVERIFY2(why.contains(QStringLiteral("Pareto")), qPrintable(why));
    QVERIFY(!applyOptimum(m, *m.find(QStringLiteral("opt1")), int(r.front.size()) + 1, &written, &why));
    QVERIFY2(why.contains(QStringLiteral("there is no design %1").arg(r.front.size() + 1)), qPrintable(why));   // (not "say which")
    QVERIFY2(applyOptimum(m, *m.find(QStringLiteral("opt1")), 1, &written, &why), qPrintable(why));
    const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
    QVERIFY(std::abs(ps->values.value(QStringLiteral("w")) - r.front[0].values[0]) < 1e-9 * r.front[0].values[0]);
}

void TestFemOptimize::whatIsNotWrittenIn()
{
    Model m = modelOf(bar(R"({"type": "optimization", "tag": "opt1", "method": "tr", "maxiter": 10,
        "variables": [["w", "3[mm]", "1[mm]", "9[mm]"]],
        "objectives": [["w", "MIN"], ["ec.R", "LE", "0.1[ohm]"]]})"));
    const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QCOMPARE(r.status, QStringLiteral("infeasible"));
    m.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("result"), r.toJson());
    QStringList written;
    QString why;
    const QJsonObject before = m.toObject();
    QVERIFY(!applyOptimum(m, *m.find(QStringLiteral("opt1")), 0, &written, &why));
    QCOMPARE(m.toObject(), before);
    // A parameter since taken out: the others written, said.
    Model gone = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 12,
        "variables": [["d", "", "0.2[mm]", "3[mm]"], ["er", "", "1", "9"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"], ["er", "TARGET", "3"]]})"));
    const OptimizationResult g = runOptimization(gone, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QVERIFY2(g.error.isEmpty(), qPrintable(said(g)));
    gone.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("result"), g.toJson());
    QList<Model::Parameter> fewer;
    for (const Model::Parameter& p : gone.parameters())
        if (p.name != QLatin1String("er")) fewer << p;
    gone.setParameters(fewer);
    QVERIFY2(applyOptimum(gone, *gone.find(QStringLiteral("opt1")), 0, &written, &why), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("er")), qPrintable(why));
    QCOMPARE(written.size(), 1);
}

void TestFemOptimize::theSetupTellsAnOldResult()
{
    Model m = modelOf(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "tune", "maxiter": 12,
        "variables": [["d", "", "0.2[mm]", "3[mm]"]],
        "objectives": [["es.C11", "TARGET", "200[pF]"]]})"));
    const OptimizationResult r = runOptimization(m, QStringLiteral("opt1"), nullptr, nullptr, nullptr);
    QVERIFY(!r.setup.isEmpty());
    Node& n = *m.find(QStringLiteral("opt1"));
    QCOMPARE(optimizationSetup(m, n), r.setup);
    // Its result kept: the same.
    n.props.insert(QStringLiteral("result"), r.toJson());
    QCOMPARE(optimizationSetup(m, n), r.setup);
    // Its optimum written in: the same - the parameters varied are its own.
    QStringList written;
    QVERIFY(applyOptimum(m, *m.find(QStringLiteral("opt1")), 0, &written, nullptr));
    QCOMPARE(optimizationSetup(m, *m.find(QStringLiteral("opt1"))), r.setup);
    // Another parameter, the geometry, a material, a study, its settings: not.
    auto changed = [&](const std::function<void(Model&)>& change) {
        Model copy = m;
        change(copy);
        return optimizationSetup(copy, *copy.find(QStringLiteral("opt1"))) != r.setup;
    };
    QVERIFY(changed([](Model& c) {
        QList<Model::Parameter> ps = c.parameters();
        for (Model::Parameter& p : ps)
            if (p.name == QLatin1String("w")) p.expression = QStringLiteral("5[mm]");
        c.setParameters(ps);
    }));
    QVERIFY(changed([](Model& c) { c.mesh().props.insert(QStringLiteral("hmax"), QStringLiteral("1")); }));
    QVERIFY(changed([](Model& c) { c.find(QStringLiteral("opt1"))->props.insert(QStringLiteral("maxiter"), 13); }));
    QVERIFY(changed([](Model& c) { c.find(QStringLiteral("std1"))->label = QStringLiteral("other"); }));
}

void TestFemOptimize::theSameOnAnyNumberOfSolves()
{
    // Whatever is asked together is solved together: the same candidates, the
    // same rows, in the same order - to the last digit.
    for (const char* method : {"de", "pso", "cmaes", "nsga2", "lm", "nm", "tr", "bayes"}) {
        const bool two = QLatin1String(method) == QLatin1String("nsga2");
        auto section = [&](int parallel) {
            return capacitor(QByteArray(R"({"type": "optimization", "tag": "opt1", "maxiter": 5, "population": 8, "seed": 6, "method": ")") + method + R"(", "parallel": )"
                             + QByteArray::number(parallel) + R"(,
                "variables": [["d", "", "0.2[mm]", "3[mm]"], ["w", "", "1[mm]", "9[mm]"]],
                "objectives": [["es.C11", "TARGET", "200[pF]"])" + (two ? R"(, ["d*w", "MIN"])" : "") + R"(]})");
        };
        const OptimizationResult one = run(section(1)), four = run(section(4));
        QVERIFY2(one.error.isEmpty() && four.error.isEmpty(), qPrintable(said(one) + said(four)));
        QCOMPARE(four.evaluations, one.evaluations);
        QCOMPARE(int(four.history.size()), int(one.history.size()));
        for (std::size_t i = 0; i < one.history.size(); ++i) {
            QCOMPARE(four.history[i].index, one.history[i].index);
            QCOMPARE(four.history[i].values, one.history[i].values);
            QCOMPARE(four.history[i].measured, one.history[i].measured);
            QCOMPARE(four.history[i].best, one.history[i].best);
        }
        QCOMPARE(four.summary, one.summary);
    }
}

void TestFemOptimize::repeatedPointsAreNotSolvedAgain()
{
    // The start asked for again and again by a method that returns to it.
    const OptimizationResult r = run(capacitor(R"({"type": "optimization", "tag": "opt1", "method": "nm", "maxiter": 10,
        "variables": [["er", "4", "1", "4.0001", "LIN_INT"]],
        "objectives": [["es.C11", "MIN"]]})"));
    QVERIFY2(r.error.isEmpty(), qPrintable(said(r)));
    QVERIFY2(r.solves <= 4, qPrintable(said(r)));
    QVERIFY2(r.evaluations > r.solves, qPrintable(said(r)));
}

QTEST_GUILESS_MAIN(TestFemOptimize)
#include "test_fem_optimize.moc"
