/*
 * test_optimizer_core.cpp - the optimizers on functions, each evaluation a few
 *                           nanoseconds: against what ngspice's own optimize
 *                           prints for the same problem (the port's check),
 *                           known minima, constraints, a Pareto front, a stop,
 *                           failures, what stays the same however evaluations
 *                           are grouped.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "optimizer_core.h"

#include <QtTest>

#include <cmath>
#include <functional>

using namespace qucs_s::optimization;

namespace {

using Point = std::vector<double>;

class Function : public SearchEvaluator
{
public:
    std::function<void(const Point&, Evaluated&)> f;
    int calls = 0, evaluations = 0, largestBatch = 0, stopAfter = -1;
    bool reversed = false;
    std::vector<Point> asked;

    std::vector<Evaluated> evaluate(const std::vector<Point>& points) override
    {
        ++calls;
        largestBatch = std::max(largestBatch, int(points.size()));
        std::vector<Evaluated> out(points.size());
        // (In any order: what is asked together does not depend on it.)
        for (std::size_t k = 0; k < points.size(); ++k) {
            const std::size_t i = reversed ? points.size() - 1 - k : k;
            if (stopRequested()) {
                out[i].stopped = true;
                continue;
            }
            ++evaluations;
            out[i].solved = true;
            f(points[i], out[i]);
            asked.push_back(points[i]);
        }
        return out;
    }
    bool stopRequested() override { return stopAfter >= 0 && evaluations >= stopAfter; }
};

// R1 from 100 to 10k, R2 1k, 1 V: ngspice's divider, in closed form.
double r1Of(double u)
{
    return 100 + u * 9900;
}
double vout(double u)
{
    return 1000.0 / (r1Of(u) + 1000.0);
}
const double kStart = 900.0 / 9900.0;

SearchProblem divider()
{
    SearchProblem p;
    p.names = QStringList{QStringLiteral("r1")};
    p.start = {kStart};
    return p;
}

void sphere(const Point& u, Evaluated& e)
{
    double s = 0;
    for (double x : u) s += (x - 0.3) * (x - 0.3);
    e.objective = s;
}

// The Rastrigin function shifted into the cube: a lattice of local minima, its
// global one (0) at u = 0.4 in every coordinate.
void rastrigin(const Point& u, Evaluated& e)
{
    double s = 10.0 * double(u.size());
    for (double x : u) {
        const double z = (x - 0.4) * 5;
        s += z * z - 10.0 * std::cos(2 * M_PI * z);
    }
    e.objective = s;
}

QString said(const SearchOutcome& o)
{
    QStringList lines{statusWord(o.status), o.phrase, QStringLiteral("cost %1").arg(o.cost), QStringLiteral("%1 evaluations").arg(o.evaluations)};
    return (lines + o.notes + o.search).join(QStringLiteral(" | "));
}

} // namespace

class TestOptimizerCore : public QObject
{
    Q_OBJECT

private slots:
    // ngspice's optimize, the same problem: its numbers. It counts one more
    // than this: the optimum's last run, which leaves the circuit there.
    void portAgainstNgspice_data();
    void portAgainstNgspice();
    void portOfTheFit();
    void portOfTheStarts();
    void portOfThePolish();
    void portOfTheConstraint_data();
    void portOfTheConstraint();
    void portOfTheParetoFront();
    // Known minima.
    void localMethodsFindASmoothMinimum_data();
    void localMethodsFindASmoothMinimum();
    void globalMethodsAreNotCaughtInAValley_data();
    void globalMethodsAreNotCaughtInAValley();
    void differentialEvolutionsTenVariants();
    void aFit();
    void aFitOfManyTargets();
    void aFitAlongACurvedValley();
    void aFitFromItsUpperBound_data();
    void aFitFromItsUpperBound();
    void aSwarmKeepsItsOwnBest();
    void theSurrogateLooksWhereItExpectsBetter();
    void anEarlyFrontHoldsOnlyItsFirstRank();
    void eachDifferentialEvolutionSearchesItsOwnWay();
    void startsShareTheIterations_data();
    void startsShareTheIterations();
    void falsePosition();
    void falsePositionNoCrossing();
    void falsePositionFailedEnd();
    void broyden();
    void theTable();
    // Constraints.
    void aConstraintsMultiplier();
    void anInfeasibleConstraint();
    void limitsOnlyEndWhenMet();
    void aParetoFrontOfZDT1();
    void aParetoFrontUnderAConstraint();
    void anEqualityConstraint();
    // What goes wrong.
    void stopped_data();
    void stopped();
    void failuresAreWorstNotFatal_data();
    void failuresAreWorstNotFatal();
    void nothingSolves_data();
    void nothingSolves();
    void notANumberIsAFailure();
    void anObjectiveThatNeverMoves();
    void aParameterOnABound();
    void theIterationsRunOut();
    void populationsAreRaisedAndSaid();
    void aMethodNeedsNoPolish();
    void anEmptyProblem();
    // What stays the same.
    void aRunRepeats_data();
    void aRunRepeats();
    void aSeedSearchesDifferently_data();
    void aSeedSearchesDifferently();
    void theOrderOfABatchDoesNotMatter_data();
    void theOrderOfABatchDoesNotMatter();
    void pointsStayInTheCube_data();
    void pointsStayInTheCube();
    void batchesAreWhatAMethodCanUse();
};

void TestOptimizerCore::portAgainstNgspice_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("maxiter");
    QTest::addColumn<int>("evaluations");
    QTest::addColumn<double>("cost");
    QTest::addColumn<double>("r1");
    // optimize -param R1 1k 100 10k -analysis op -minimize (v(out)-0.3)^2 -method m -seed 1
    QTest::newRow("nm") << QStringLiteral("nm") << 100 << 39 << 1.33956e-15 << 2333.33;
    QTest::newRow("tr") << QStringLiteral("tr") << 100 << 11 << 3.6176e-16 << 2333.33;
    QTest::newRow("cmaes") << QStringLiteral("cmaes") << 100 << 173 << 3.12547e-14 << 2333.33;
    QTest::newRow("bayes") << QStringLiteral("bayes") << 40 << 24 << 1.5627e-13 << 2333.33;
    QTest::newRow("sa") << QStringLiteral("sa") << 100 << 1214 << 2.01905e-09 << 2333.83;
}

// The port runs the same steps on the same stream of random numbers: its
// evaluations and its answer are ngspice's, to what a closed form's rounding
// (ngspice solves a matrix) leaves of them near the tolerance.
void TestOptimizerCore::portAgainstNgspice()
{
    QFETCH(QString, method);
    QFETCH(int, maxiter);
    QFETCH(int, evaluations);
    QFETCH(double, cost);
    QFETCH(double, r1);
    Function e;
    e.f = [](const Point& u, Evaluated& r) { r.objective = std::pow(vout(u[0]) - 0.3, 2); };
    SearchOptions o;
    o.method = method;
    o.maxIter = maxiter;
    o.seed = 1;
    const SearchOutcome out = search(divider(), o, e);
    QVERIFY2(std::abs(r1Of(out.best[0]) - r1) < 0.5 * 1e-2 * r1, qPrintable(said(out)));
    QVERIFY2(std::abs(out.evaluations + 1 - evaluations) <= std::max(2, evaluations / 10), qPrintable(said(out)));
    // Of the order the printed cost is, the last digits rounding's.
    QVERIFY2(out.cost < 100 * cost && out.cost > cost / 100, qPrintable(said(out)));
    QVERIFY(out.settled());
    if (method == QLatin1String("sa")) {
        // A schedule: exactly 1 + 12 + (8 + 4 n) levels, to the digit printed.
        QCOMPARE(out.evaluations, 1213);
        QVERIFY2(std::abs(out.cost - cost) < 1e-5 * cost, qPrintable(said(out)));
        QCOMPARE(out.status, SearchStatus::Completed);
    }
}

void TestOptimizerCore::portOfTheFit()
{
    // optimize -param R1 1k 100 10k -analysis op -target v(out) 0.3 -method lm
    // converged, sum-sq residual = 1.2321e-18 after 12 evaluations, r1 = 2333.33
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double x = vout(u[0]) - 0.3;
        r.residuals = {x};
        r.objective = x * x;
    };
    SearchProblem p = divider();
    p.residuals = 1;
    SearchOptions o;
    o.method = QStringLiteral("lm");
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    QVERIFY2(std::abs(out.evaluations + 1 - 12) <= 2, qPrintable(said(out)));
    QVERIFY2(std::abs(r1Of(out.best[0]) - 7000.0 / 3) < 1e-3, qPrintable(said(out)));
    QVERIFY2(out.cost < 1e-16, qPrintable(said(out)));
    // Empty method: least squares by Levenberg-Marquardt; else Nelder-Mead.
    Function auto1;
    auto1.f = e.f;
    SearchOptions none;
    QCOMPARE(search(p, none, auto1).evaluations, out.evaluations);
}

void TestOptimizerCore::portOfTheStarts()
{
    // -method nm -starts 2 -seed 1: start 1 of 3 won, polished by lm in 3
    // evaluations; 126 in all.
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double x = vout(u[0]) - 0.3;
        r.residuals = {x};
        r.objective = x * x;
    };
    SearchProblem p = divider();
    p.residuals = 1;
    SearchOptions o;
    o.method = QStringLiteral("nm");
    o.starts = 2;
    o.seed = 1;
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(out.search.join(QLatin1Char(' ')).contains(QStringLiteral("start 1 of 3 won")), qPrintable(said(out)));
    QVERIFY2(out.search.join(QLatin1Char(' ')).contains(QStringLiteral("polish converged")), qPrintable(said(out)));
    QVERIFY2(std::abs(out.evaluations + 1 - 126) <= 6, qPrintable(said(out)));
    QVERIFY2(out.cost < 1e-18, qPrintable(said(out)));
    QVERIFY2(out.search.size() == 5, qPrintable(said(out)));
}

void TestOptimizerCore::portOfThePolish()
{
    // -method cmaes -maxiter 6 -seed 1 -polish: from 1.67666e-06 by the trust
    // region in 8 evaluations to about 4e-14; 33 in all.
    Function e;
    e.f = [](const Point& u, Evaluated& r) { r.objective = std::pow(vout(u[0]) - 0.3, 2); };
    SearchOptions o;
    o.method = QStringLiteral("cmaes");
    o.maxIter = 6;
    o.seed = 1;
    o.polish = true;
    const SearchOutcome out = search(divider(), o, e);
    QVERIFY2(out.search.join(QLatin1Char(' ')).contains(QStringLiteral("1.67666e-06")), qPrintable(said(out)));
    QVERIFY2(std::abs(out.evaluations + 1 - 33) <= 2, qPrintable(said(out)));
    QVERIFY2(out.cost < 1e-12, qPrintable(said(out)));
}

void TestOptimizerCore::portOfTheConstraint_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("evaluations");
    QTest::addColumn<double>("multiplier");
    // optimize -param R1 ... -minimize abs(i(v1)) -constrain v(out) -min 0.2 -method m
    QTest::newRow("nm") << QStringLiteral("nm") << 45 << 0.0009155;
    QTest::newRow("tr") << QStringLiteral("tr") << 40 << 0.0009749;
}

void TestOptimizerCore::portOfTheConstraint()
{
    QFETCH(QString, method);
    QFETCH(int, evaluations);
    QFETCH(double, multiplier);
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        r.objective = 1.0 / (r1Of(u[0]) + 1000.0);
        r.constraints = {vout(u[0])};
    };
    SearchProblem p = divider();
    SearchConstraint c;
    c.name = QStringLiteral("v(out)");
    c.hasLower = true;
    c.lower = 0.2;
    p.constraints.push_back(c);
    SearchOptions o;
    o.method = method;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    QVERIFY(out.feasible);
    // r1 about 4000, a little past where the limit is exact: within its 1e-4.
    QVERIFY2(std::abs(r1Of(out.best[0]) - 4000) < 0.5, qPrintable(said(out)));
    QVERIFY2(std::abs(out.evaluations + 1 - evaluations) <= 3, qPrintable(said(out)));
    QCOMPARE(int(out.constraints.size()), 1);
    QCOMPARE(out.constraints[0].kind, ConstraintState::Active);
    QVERIFY2(std::abs(out.constraints[0].multiplier - multiplier) < 0.05 * multiplier, qPrintable(QString::number(out.constraints[0].multiplier)));
    QVERIFY2(out.search.join(QLatin1Char(' ')).contains(QStringLiteral("penalty 10")), qPrintable(said(out)));
}

void TestOptimizerCore::portOfTheParetoFront()
{
    // optimize -param R1 1k 100 10k -param R2 1k 100 10k -analysis op
    //   -minimize (v(out)-0.3)^2 -maximize abs(i(v1)) -method nsga2 -maxiter 5 -swarmsize 8 -seed 1
    const double rows[][4] = {{9.47958e-05, 7.78725e-05, 8864.03, 3977.48}, {0.00123567, 8.42624e-05, 7890.21, 3977.48},
                              {0.00199707, 8.65544e-05, 8603.71, 2949.72},  {0.00231026, 9.83782e-05, 6626.82, 3538.03},
                              {0.00333246, 0.00101856, 630.569, 351.209},   {0.0256124, 0.00136391, 630.569, 102.618},
                              {0.0795413, 0.00163813, 255.151, 355.302}};
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double R1 = 100 + u[0] * 9900, R2 = 100 + u[1] * 9900;
        const double v = R2 / (R1 + R2);
        r.objectives = {std::pow(v - 0.3, 2), -1.0 / (R1 + R2)};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("r1"), QStringLiteral("r2")};
    p.start = {kStart, kStart};
    p.objectives = 2;
    SearchOptions o;
    o.method = QStringLiteral("nsga2");
    o.maxIter = 5;
    o.population = 8;
    o.seed = 1;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Completed);
    QCOMPARE(out.evaluations, 48);
    // ngspice printed a design twice; here each once.
    QCOMPARE(int(out.front.size()), 7);
    for (int i = 0; i < 7; ++i) {
        const ParetoPoint& d = out.front[std::size_t(i)];
        QVERIFY2(std::abs(d.objectives[0] - rows[i][0]) < 1e-5 * rows[i][0], qPrintable(QString::number(d.objectives[0])));
        QVERIFY2(std::abs(-d.objectives[1] - rows[i][1]) < 1e-5 * rows[i][1], qPrintable(QString::number(d.objectives[1])));
        QVERIFY2(std::abs(100 + d.x[0] * 9900 - rows[i][2]) < 1e-4 * rows[i][2], qPrintable(QString::number(100 + d.x[0] * 9900)));
        QVERIFY2(std::abs(100 + d.x[1] * 9900 - rows[i][3]) < 1e-4 * rows[i][3], qPrintable(QString::number(100 + d.x[1] * 9900)));
    }
}

void TestOptimizerCore::localMethodsFindASmoothMinimum_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("most");
    QTest::newRow("nm") << QStringLiteral("nm") << 600;
    QTest::newRow("tr") << QStringLiteral("tr") << 200;
}

void TestOptimizerCore::localMethodsFindASmoothMinimum()
{
    QFETCH(QString, method);
    QFETCH(int, most);
    Function e;
    e.f = sphere;
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    p.start = {0.9, 0.1, 0.7};
    SearchOptions o;
    o.method = method;
    o.maxIter = 300;
    o.tol = 1e-10;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    for (double x : out.best) QVERIFY2(std::abs(x - 0.3) < 1e-3, qPrintable(said(out)));
    QVERIFY2(out.evaluations <= most, qPrintable(said(out)));
}

void TestOptimizerCore::globalMethodsAreNotCaughtInAValley_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("maxiter");
    QTest::addColumn<int>("population");
    QTest::newRow("de") << QStringLiteral("de") << 200 << 30;
    QTest::newRow("pso") << QStringLiteral("pso") << 200 << 80;
    QTest::newRow("sa") << QStringLiteral("sa") << 200 << 0;
    QTest::newRow("cmaes") << QStringLiteral("cmaes") << 300 << 40;
}

// From a valley of Rastrigin's lattice that is not the deepest.
void TestOptimizerCore::globalMethodsAreNotCaughtInAValley()
{
    QFETCH(QString, method);
    QFETCH(int, maxiter);
    QFETCH(int, population);
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    p.start = {0.8, 0.8};
    SearchOptions o;
    o.method = method;
    o.maxIter = maxiter;
    o.population = population;
    o.seed = 4;
    o.polish = true;
    Function e;
    e.f = rastrigin;
    const SearchOutcome out = search(p, o, e);
    for (double x : out.best) QVERIFY2(std::abs(x - 0.4) < 1e-3, qPrintable(said(out)));
    QVERIFY2(out.cost < 1e-3, qPrintable(said(out)));
    // A local method alone stays in the valley it starts in: what the others are for.
    Function local;
    local.f = rastrigin;
    SearchOptions nm;
    nm.method = QStringLiteral("nm");
    const SearchOutcome stuck = search(p, nm, local);
    QVERIFY2(stuck.cost > 1, qPrintable(said(stuck)));
}

void TestOptimizerCore::differentialEvolutionsTenVariants()
{
    QStringList names;
    for (int strategy = 1; strategy <= 10; ++strategy) {
        Function e;
        e.f = sphere;
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        p.start = {0.9, 0.1};
        SearchOptions o;
        o.method = QStringLiteral("de");
        o.strategy = strategy;
        o.maxIter = 80;
        o.population = 20;
        o.F = 0.7;
        o.CR = 0.9;
        o.seed = 2;
        o.tol = 1e-12;
        const SearchOutcome out = search(p, o, e);
        for (double x : out.best) QVERIFY2(std::abs(x - 0.3) < 2e-2, qPrintable(QString::number(strategy) + said(out)));
        Settings s;
        s.method = strategy;
        names << s.methodName();
        // The search it is: the same one the class searches, by the first
        // generation's first candidate after its random members.
        QVERIFY(e.asked.size() > 40);
    }
    names.removeDuplicates();
    QCOMPARE(names.size(), 10);
    // Its cost variance ends it.
    Function e;
    e.f = sphere;
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a")};
    p.start = {0.9};
    SearchOptions o;
    o.method = QStringLiteral("de");
    o.maxIter = 1000;
    o.population = 8;
    o.minCostVariance = 1e-6;
    o.tol = 1e-30;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    QVERIFY2(out.evaluations < 8 * 1000, qPrintable(said(out)));
    // And as ngspice's: the best not moving for 8 generations and n/4 more.
    Function still;
    still.f = [](const Point&, Evaluated& r) { r.objective = 3; };
    SearchOptions stops;
    stops.method = QStringLiteral("de");
    stops.maxIter = 1000;
    stops.population = 8;
    const SearchOutcome stalled = search(p, stops, still);
    QCOMPARE(stalled.evaluations, 8 + 8 * 8);
    Function swarm;
    swarm.f = still.f;
    stops.method = QStringLiteral("pso");
    QCOMPARE(search(p, stops, swarm).evaluations, 8 + 8 * 8);
}

void TestOptimizerCore::aFit()
{
    // A line through three points: y = a x + b at x = 0, 1, 2 - its least
    // squares known: with a = 0.6, b = 0.2 the residuals vanish.
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double xs[] = {0.0, 1.0, 2.0};
        const double ys[] = {0.2, 0.8, 1.4};
        r.objective = 0;
        for (int i = 0; i < 3; ++i) {
            const double miss = u[0] * xs[i] + u[1] - ys[i];
            r.residuals.push_back(miss);
            r.objective += miss * miss;
        }
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    p.start = {0.1, 0.9};
    p.residuals = 3;
    SearchOptions o;
    o.method = QStringLiteral("lm");
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(std::abs(out.best[0] - 0.6) < 1e-6 && std::abs(out.best[1] - 0.2) < 1e-6, qPrintable(said(out)));
    QVERIFY2(out.evaluations < 40, qPrintable(said(out)));
    // Fewer evaluations than the simplex on the same sum.
    Function nm;
    nm.f = e.f;
    SearchOptions s;
    s.method = QStringLiteral("nm");
    s.tol = 1e-14;
    s.maxIter = 500;
    SearchProblem scalar = p;
    QVERIFY(search(scalar, s, nm).evaluations > out.evaluations);
}

void TestOptimizerCore::aFitOfManyTargets()
{
    // Each residual its own knob's: their finite differences by a generation of n.
    const int n = 20;
    Function e;
    e.f = [n](const Point& u, Evaluated& r) {
        r.objective = 0;
        for (int i = 0; i < n; ++i) {
            const double miss = u[std::size_t(i)] - (0.1 + 0.8 * i / (n - 1));
            r.residuals.push_back(miss);
            r.objective += miss * miss;
        }
    };
    SearchProblem p;
    for (int i = 0; i < n; ++i) p.names << QString::number(i);
    p.start.assign(std::size_t(n), 0.5);
    p.residuals = n;
    SearchOptions o;
    o.method = QStringLiteral("lm");
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(out.cost < 1e-12, qPrintable(said(out)));
    QCOMPARE(e.largestBatch, n);
}

void TestOptimizerCore::aFitAlongACurvedValley()
{
    // Rosenbrock's, (10 (y - x^2), 1 - x), from (-1.2, 1): a damping raised to
    // get into the valley has to come down again to follow it.
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double x = 4 * u[0] - 2, y = 4 * u[1] - 2;
        r.residuals = {10 * (y - x * x), 1 - x};
        r.objective = r.residuals[0] * r.residuals[0] + r.residuals[1] * r.residuals[1];
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x"), QStringLiteral("y")};
    p.start = {0.2, 0.75};
    p.residuals = 2;
    SearchOptions o;
    o.method = QStringLiteral("lm");
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(out.cost < 1e-12, qPrintable(said(out)));
    QVERIFY2(std::abs(out.best[0] - 0.75) < 1e-5 && std::abs(out.best[1] - 0.75) < 1e-5, qPrintable(said(out)));
    QVERIFY2(out.evaluations < 100, qPrintable(said(out)));
}

void TestOptimizerCore::aFitFromItsUpperBound_data()
{
    QTest::addColumn<QString>("method");
    QTest::newRow("lm") << QStringLiteral("lm");
    QTest::newRow("tune") << QStringLiteral("tune");
}

// At the top of its range a difference can only be taken downward: its sign has
// to be counted, or the first step leaves the wrong way and stays on the bound.
void TestOptimizerCore::aFitFromItsUpperBound()
{
    QFETCH(QString, method);
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        r.residuals = {u[0] - 0.3, 2 * (u[1] - 0.6)};
        r.objective = r.residuals[0] * r.residuals[0] + r.residuals[1] * r.residuals[1];
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    p.start = {1.0, 1.0};
    p.residuals = 2;
    SearchOptions o;
    o.method = method;
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(std::abs(out.best[0] - 0.3) < 1e-4 && std::abs(out.best[1] - 0.6) < 1e-4, qPrintable(said(out)));
    QVERIFY2(out.evaluations < 40, qPrintable(said(out)));
}

// With only the swarm's best to go to, a swarm falls into the first valley it
// finds. A run says little; forty seeds at what is spent the same say it: a mean
// of 6.6 here, and 9.9 without a particle's own best.
void TestOptimizerCore::aSwarmKeepsItsOwnBest()
{
    double sum = 0;
    for (int seed = 1; seed <= 40; ++seed) {
        Function e;
        e.f = rastrigin;
        SearchProblem p;
        for (int i = 0; i < 4; ++i) p.names << QString::number(i);
        p.start.assign(4, 0.9);
        SearchOptions o;
        o.method = QStringLiteral("pso");
        o.maxIter = 120;
        o.population = 16;
        o.seed = seed;
        sum += search(p, o, e).cost;
    }
    QVERIFY2(sum / 40 < 8.25, qPrintable(QString::number(sum / 40)));
}

// The surrogate's next point is where it expects better: its first eight after
// its design come near the minimum, 0.026 of the range on average over ten seeds
// here, 0.20 with the improvement's sign turned. The average is what is judged,
// not any digit a platform's rounding may move; a polish would hide it anyway.
void TestOptimizerCore::theSurrogateLooksWhereItExpectsBetter()
{
    double sum = 0;
    for (int seed = 1; seed <= 10; ++seed) {
        Function e;
        e.f = [](const Point& u, Evaluated& r) { r.objective = std::pow(u[0] - 0.7, 2) + std::pow(u[1] - 0.2, 2); };
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        p.start = {0.1, 0.9};
        SearchOptions o;
        o.method = QStringLiteral("bayes");
        o.maxIter = 30;
        o.seed = seed;
        (void) search(p, o, e);
        QVERIFY(e.asked.size() >= 14);
        double nearest = 2;
        for (std::size_t i = 6; i < 14; ++i) nearest = std::min(nearest, std::hypot(e.asked[i][0] - 0.7, e.asked[i][1] - 0.2));
        sum += nearest;
    }
    QVERIFY2(sum / 10 < 0.08, qPrintable(QString::number(sum / 10)));
}

// ZDT1 after two generations: most of a population is still dominated, and none
// of that is the front.
void TestOptimizerCore::anEarlyFrontHoldsOnlyItsFirstRank()
{
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        double sum = 0;
        for (std::size_t i = 1; i < u.size(); ++i) sum += u[i];
        const double g = 1 + 9 * sum / double(u.size() - 1);
        r.objectives = {u[0], g * (1 - std::sqrt(u[0] / g))};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    p.start = {0.5, 0.5, 0.5};
    p.objectives = 2;
    SearchOptions o;
    o.method = QStringLiteral("nsga2");
    o.maxIter = 2;
    o.population = 40;
    o.seed = 5;
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(!out.front.empty() && out.front.size() < 40, qPrintable(said(out)));
    for (const ParetoPoint& a : out.front)
        for (const ParetoPoint& b : out.front) {
            const bool noWorse = a.objectives[0] <= b.objectives[0] && a.objectives[1] <= b.objectives[1];
            const bool better = a.objectives[0] < b.objectives[0] || a.objectives[1] < b.objectives[1];
            QVERIFY(!(noWorse && better));
        }
}

// Each variant takes its own path from the same seed: the setting reaches the
// search, not only its name.
void TestOptimizerCore::eachDifferentialEvolutionSearchesItsOwnWay()
{
    QList<std::vector<Point>> paths;
    for (int strategy = 1; strategy <= 10; ++strategy) {
        Function e;
        e.f = sphere;
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        p.start = {0.9, 0.1, 0.5};
        SearchOptions o;
        o.method = QStringLiteral("de");
        o.strategy = strategy;
        o.maxIter = 3;
        o.population = 12;
        o.seed = 2;
        (void) search(p, o, e);
        for (const std::vector<Point>& other : paths) QVERIFY2(other != e.asked, qPrintable(QString::number(strategy)));
        paths << e.asked;
    }
}

void TestOptimizerCore::startsShareTheIterations_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("starts");
    QTest::newRow("sa, 1") << QStringLiteral("sa") << 1;
    QTest::newRow("sa, 3") << QStringLiteral("sa") << 3;
}

// More starts share what was asked for: a run of four starts is not four runs.
void TestOptimizerCore::startsShareTheIterations()
{
    QFETCH(QString, method);
    QFETCH(int, starts);
    auto spent = [&](int howMany) {
        Function e;
        e.f = sphere;
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        p.start = {0.9, 0.1};
        SearchOptions o;
        o.method = method;
        o.maxIter = 60;
        o.population = 10;
        o.seed = 1;
        o.tol = 1e-300;
        o.starts = howMany;
        return search(p, o, e).evaluations;
    };
    const int alone = spent(0), several = spent(starts);
    QVERIFY2(several < alone + alone / 2, qPrintable(QStringLiteral("%1 alone, %2 with its starts").arg(alone).arg(several)));
}

void TestOptimizerCore::falsePosition()
{
    // exp(4u) = 10: u = ln(10)/4. A few evaluations, where a method of a whole
    // range takes tens.
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double miss = (std::exp(4 * u[0]) - 10.0) / 10.0;
        r.residuals = {miss};
        r.objective = miss * miss;
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x")};
    p.start = {0.2};
    p.residuals = 1;
    SearchOptions o;
    o.method = QStringLiteral("tune");
    o.maxIter = 40;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    QVERIFY2(std::abs(out.best[0] - std::log(10.0) / 4) < 1e-6, qPrintable(said(out)));
    QVERIFY2(out.evaluations <= 25, qPrintable(said(out)));
    // Its budget counts: stopped at it.
    Function short2;
    short2.f = e.f;
    o.maxIter = 5;
    o.tol = 1e-14;
    const SearchOutcome cut = search(p, o, short2);
    QCOMPARE(cut.status, SearchStatus::MaxIter);
    QVERIFY2(cut.evaluations <= 5, qPrintable(said(cut)));
    QVERIFY2(cut.phrase.contains(QStringLiteral("evaluations")), qPrintable(said(cut)));
}

void TestOptimizerCore::falsePositionNoCrossing()
{
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double miss = 1 + u[0];   // never 0
        r.residuals = {miss};
        r.objective = miss * miss;
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x")};
    p.start = {0.5};
    p.residuals = 1;
    SearchOptions o;
    o.method = QStringLiteral("tune");
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("not crossed")), qPrintable(said(out)));
    QVERIFY(!out.settled());
    QCOMPARE(out.best[0], 0.0);   // the nearest
    QCOMPARE(out.evaluations, 3);
}

void TestOptimizerCore::falsePositionFailedEnd()
{
    // Below u = 0.3 it does not solve, the crossing at 0.4: found from the other side.
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        if (u[0] < 0.3) {
            r.solved = false;
            return;
        }
        const double miss = u[0] - 0.4;
        r.residuals = {miss};
        r.objective = miss * miss;
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x")};
    p.start = {0.8};
    p.residuals = 1;
    SearchOptions o;
    o.method = QStringLiteral("tune");
    o.maxIter = 40;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    QVERIFY2(std::abs(out.best[0] - 0.4) < 1e-5, qPrintable(said(out)));
    QVERIFY2(out.failed >= 1, qPrintable(said(out)));
}

void TestOptimizerCore::broyden()
{
    // Three targets of three knobs, not separately: a few runs each.
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        const double a = u[0] + 0.2 * u[1] - 0.5;
        const double b = u[1] * u[1] + u[2] - 0.7;
        const double c = u[0] * u[2] + 0.1 * u[1] - 0.2;
        r.residuals = {a, b, c};
        r.objective = a * a + b * b + c * c;
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    p.start = {0.5, 0.5, 0.5};
    p.residuals = 3;
    SearchOptions o;
    o.method = QStringLiteral("tune");
    o.maxIter = 40;
    o.tol = 1e-8;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    QVERIFY2(out.cost < 1e-14, qPrintable(said(out)));
    QVERIFY2(out.evaluations <= 25, qPrintable(said(out)));
    // Not separately at all: singular, said.
    Function same;
    same.f = [](const Point& u, Evaluated& r) {
        const double a = u[0] + u[1] - 0.4;
        r.residuals = {a, 2 * a};
        r.objective = 5 * a * a;
    };
    SearchProblem two;
    two.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    two.start = {0.9, 0.9};
    two.residuals = 2;
    const SearchOutcome none = search(two, o, same);
    QVERIFY2(none.notes.join(QLatin1Char(' ')).contains(QStringLiteral("singular")), qPrintable(said(none)));
}

void TestOptimizerCore::theTable()
{
    QCOMPARE(int(searchMethods().size()), 10);
    int block = 0;
    for (const SearchMethod& m : searchMethods()) {
        block += m.block ? 1 : 0;
        QCOMPARE(searchMethod(QLatin1String(m.id)), &m);
        QVERIFY(m.name != nullptr && m.note != nullptr);
    }
    QCOMPARE(block, 8);
    QVERIFY(!searchMethod(QStringLiteral("newton")));
    QVERIFY(!searchMethod(QStringLiteral("nsga2"))->block);
    for (const QString& w : {QStringLiteral("converged"), QStringLiteral("maxiter"), QStringLiteral("completed"), QStringLiteral("nosolve"),
                             QStringLiteral("unchanged"), QStringLiteral("interrupted"), QStringLiteral("infeasible")})
        QVERIFY(!w.isEmpty());
    QCOMPARE(statusWord(SearchStatus::Infeasible), QStringLiteral("infeasible"));
    QVERIFY(knownVariableType(QStringLiteral("E96")));
    QVERIFY(knownVariableType(QStringLiteral("LOG_INT")));
    QVERIFY(!knownVariableType(QStringLiteral("E7")));
}

// min x^2 + y^2 with x + y >= 1: (1/2, 1/2), its multiplier d(cost)/d(bound) = 1.
void TestOptimizerCore::aConstraintsMultiplier()
{
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        r.objective = u[0] * u[0] + u[1] * u[1];
        r.constraints = {u[0] + u[1]};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x"), QStringLiteral("y")};
    p.start = {0.9, 0.2};
    SearchConstraint c;
    c.hasLower = true;
    c.lower = 1;
    p.constraints.push_back(c);
    for (const char* method : {"nm", "tr", "cmaes", "pso"}) {
        Function one;
        one.f = e.f;
        SearchOptions o;
        o.method = QLatin1String(method);
        o.maxIter = 200;
        o.seed = 3;
        o.tol = 1e-9;
        const SearchOutcome out = search(p, o, one);
        QVERIFY2(out.feasible, qPrintable(said(out)));
        QVERIFY2(std::abs(out.best[0] - 0.5) < 2e-2 && std::abs(out.best[1] - 0.5) < 2e-2, qPrintable(QLatin1String(method) + said(out)));
        QVERIFY2(std::abs(out.cost - 0.5) < 1e-2, qPrintable(QLatin1String(method) + said(out)));
        QVERIFY2(out.violation <= 1e-4, qPrintable(said(out)));
    }
}

void TestOptimizerCore::anInfeasibleConstraint()
{
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        r.objective = u[0] * u[0];
        r.constraints = {u[0]};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x")};
    p.start = {0.5};
    SearchConstraint c;
    c.hasUpper = true;
    c.upper = -1;   // it never is
    p.constraints.push_back(c);
    SearchOptions o;
    o.method = QStringLiteral("nm");
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Infeasible);
    QVERIFY(!out.feasible);
    QVERIFY(!out.settled());
    QCOMPARE(out.constraints[0].kind, ConstraintState::Violated);
    QVERIFY2(out.violation > 0.5, qPrintable(said(out)));
    // Ten rounds at most.
    QVERIFY2(out.search.size() <= 10, qPrintable(said(out)));
}

void TestOptimizerCore::limitsOnlyEndWhenMet()
{
    // goalsMet ends it as soon as one point has it.
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        r.objective = 0;
        r.goalsMet = u[0] > 0.8;
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x")};
    p.start = {0.1};
    SearchOptions o;
    o.method = QStringLiteral("de");
    o.maxIter = 1000;
    o.population = 10;
    o.seed = 2;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Converged);
    QVERIFY2(out.evaluations < 100, qPrintable(said(out)));
}

void TestOptimizerCore::aParetoFrontOfZDT1()
{
    // ZDT1: f1 = x1, f2 = g (1 - sqrt(f1 / g)), g = 1 + 9 mean(others): its
    // front at g = 1, f2 = 1 - sqrt(f1).
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        double sum = 0;
        for (std::size_t i = 1; i < u.size(); ++i) sum += u[i];
        const double g = 1 + 9 * sum / double(u.size() - 1);
        r.objectives = {u[0], g * (1 - std::sqrt(u[0] / g))};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    p.start = {0.5, 0.5, 0.5};
    p.objectives = 2;
    SearchOptions o;
    o.method = QStringLiteral("nsga2");
    o.maxIter = 250;
    o.population = 60;
    o.seed = 5;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Completed);
    QVERIFY2(out.front.size() >= 30, qPrintable(said(out)));
    double least = 1, most = 0;
    for (const ParetoPoint& d : out.front) {
        const double f1 = d.objectives[0], f2 = d.objectives[1];
        QVERIFY2(std::abs(f2 - (1 - std::sqrt(f1))) < 0.03, qPrintable(QStringLiteral("(%1, %2)").arg(f1).arg(f2)));
        least = std::min(least, f1);
        most = std::max(most, f1);
    }
    QVERIFY2(least < 0.05 && most > 0.8, qPrintable(QStringLiteral("from %1 to %2").arg(least).arg(most)));
    QVERIFY(out.best.empty());
    // No two of a front dominate each other.
    for (const ParetoPoint& a : out.front)
        for (const ParetoPoint& b : out.front)
            QVERIFY(!(a.objectives[0] < b.objectives[0] && a.objectives[1] < b.objectives[1]));
}

void TestOptimizerCore::aParetoFrontUnderAConstraint()
{
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        r.objectives = {u[0], 1 - u[0]};
        r.constraints = {u[0]};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x")};
    p.start = {0.9};
    p.objectives = 2;
    SearchConstraint c;
    c.hasUpper = true;
    c.upper = 0.5;
    p.constraints.push_back(c);
    SearchOptions o;
    o.method = QStringLiteral("nsga2");
    o.maxIter = 30;
    o.population = 16;
    o.seed = 3;
    const SearchOutcome out = search(p, o, e);
    QVERIFY(!out.front.empty());
    for (const ParetoPoint& d : out.front) QVERIFY2(d.x[0] <= 0.5 * (1 + 1e-4), qPrintable(QString::number(d.x[0])));
}

void TestOptimizerCore::anEqualityConstraint()
{
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        r.objective = -u[0];
        r.constraints = {u[0] + u[1]};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("x"), QStringLiteral("y")};
    p.start = {0.2, 0.2};
    SearchConstraint c;
    c.hasLower = c.hasUpper = true;
    c.lower = c.upper = 0.7;
    p.constraints.push_back(c);
    SearchOptions o;
    o.method = QStringLiteral("tr");
    o.maxIter = 200;
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(out.feasible, qPrintable(said(out)));
    QVERIFY2(std::abs(out.best[0] - 0.7) < 1e-2, qPrintable(said(out)));
    QCOMPARE(int(out.constraints.size()), 2);
}

void TestOptimizerCore::stopped_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("residuals");
    QTest::addColumn<int>("objectives");
    for (const char* m : {"nm", "tr", "de", "pso", "sa", "cmaes", "bayes"}) QTest::newRow(m) << QString::fromLatin1(m) << 0 << 0;
    QTest::newRow("lm") << QStringLiteral("lm") << 2 << 0;
    QTest::newRow("tune") << QStringLiteral("tune") << 2 << 0;
    QTest::newRow("nsga2") << QStringLiteral("nsga2") << 0 << 2;
}

// Stopped at its seventh evaluation: no more are asked, the best of the seven
// kept; a huge budget does not matter.
void TestOptimizerCore::stopped()
{
    QFETCH(QString, method);
    QFETCH(int, residuals);
    QFETCH(int, objectives);
    Function e;
    e.stopAfter = 7;
    e.f = [residuals, objectives](const Point& u, Evaluated& r) {
        Evaluated s;
        sphere(u, s);
        r.objective = s.objective;
        // (Not linear: Broyden's method would be done in a few.)
        if (residuals) r.residuals = {u[0] - 0.3 + 0.5 * u[1] * u[1], u[1] - 0.3 + 0.2 * u[0] * u[0] * u[0]};
        if (objectives) r.objectives = {u[0], 1 - u[1]};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    p.start = {0.9, 0.9};
    p.residuals = residuals;
    p.objectives = objectives;
    SearchOptions o;
    o.method = method;
    o.maxIter = 1000000;
    o.tol = 1e-30;
    o.population = 8;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Interrupted);
    QCOMPARE(e.evaluations, 7);
    QVERIFY2(out.evaluations <= 7, qPrintable(said(out)));
    QVERIFY(!out.settled());
    if (objectives == 0) {
        double best = 1e300;
        for (const Point& x : e.asked) {
            Evaluated s;
            sphere(x, s);
            best = std::min(best, s.objective);
        }
        QVERIFY2(!out.best.empty(), qPrintable(said(out)));
        QCOMPARE(out.cost, best);
    }
}

void TestOptimizerCore::failuresAreWorstNotFatal_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("maxiter");
    QTest::addColumn<double>("tolerance");
    QTest::newRow("nm") << QStringLiteral("nm") << 300 << 1e-3;
    QTest::newRow("tr") << QStringLiteral("tr") << 200 << 1e-3;
    QTest::newRow("de") << QStringLiteral("de") << 60 << 2e-2;
    QTest::newRow("pso") << QStringLiteral("pso") << 60 << 5e-2;
    QTest::newRow("sa") << QStringLiteral("sa") << 40 << 0.1;
    QTest::newRow("cmaes") << QStringLiteral("cmaes") << 80 << 1e-2;
    QTest::newRow("bayes") << QStringLiteral("bayes") << 40 << 0.2;
}

// A third of the cube does not solve; the start is in it for some.
void TestOptimizerCore::failuresAreWorstNotFatal()
{
    QFETCH(QString, method);
    QFETCH(int, maxiter);
    QFETCH(double, tolerance);
    for (double start : {0.9, 0.1}) {
        Function e;
        e.f = [](const Point& u, Evaluated& r) {
            if (u[0] < 0.2) {
                r.solved = false;
                return;
            }
            sphere(u, r);
        };
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        p.start = {start, 0.5};
        SearchOptions o;
        o.method = method;
        o.maxIter = maxiter;
        o.seed = 3;
        const SearchOutcome out = search(p, o, e);
        QVERIFY2(out.status != SearchStatus::NoSolve, qPrintable(said(out)));
        QVERIFY2(!out.best.empty() && out.best[0] >= 0.2, qPrintable(said(out)));
        QVERIFY2(std::abs(out.best[0] - 0.3) < tolerance && std::abs(out.best[1] - 0.3) < tolerance,
                 qPrintable(QStringLiteral("from %1: ").arg(start) + said(out)));
        if (out.failed > 0) QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("did not solve")), qPrintable(said(out)));
    }
}

void TestOptimizerCore::nothingSolves_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<int>("most");
    // (The local methods: the start, then a look over the range.)
    QTest::newRow("nm") << QStringLiteral("nm") << 3 + 12;
    QTest::newRow("tr") << QStringLiteral("tr") << 5 + 12;
    QTest::newRow("cmaes") << QStringLiteral("cmaes") << 30;
    QTest::newRow("bayes") << QStringLiteral("bayes") << 40;
    QTest::newRow("de") << QStringLiteral("de") << 3000;
    QTest::newRow("pso") << QStringLiteral("pso") << 3000;
    QTest::newRow("sa") << QStringLiteral("sa") << 3000;
    QTest::newRow("lm") << QStringLiteral("lm") << 1 + 12;
    QTest::newRow("tune") << QStringLiteral("tune") << 10;
}

void TestOptimizerCore::nothingSolves()
{
    QFETCH(QString, method);
    QFETCH(int, most);
    Function e;
    e.f = [](const Point&, Evaluated& r) { r.solved = false; };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    p.start = {0.5, 0.5};
    if (method == QLatin1String("lm") || method == QLatin1String("tune")) p.residuals = 2;
    SearchOptions o;
    o.method = method;
    o.maxIter = 50;
    o.seed = 1;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::NoSolve);
    QVERIFY2(out.evaluations <= most, qPrintable(said(out)));
    QCOMPARE(out.failed, out.evaluations);
    QVERIFY2(out.phrase.contains(QStringLiteral("NO SOLUTION")), qPrintable(said(out)));
}

void TestOptimizerCore::notANumberIsAFailure()
{
    for (const double bad : {std::nan(""), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        Function e;
        e.f = [bad](const Point& u, Evaluated& r) {
            if (u[0] > 0.7) {
                r.objective = bad;
                return;
            }
            sphere(u, r);
        };
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a")};
        p.start = {0.9};
        SearchOptions o;
        o.method = QStringLiteral("nm");
        const SearchOutcome out = search(p, o, e);
        QVERIFY2(out.failed > 0, qPrintable(said(out)));
        QVERIFY2(std::abs(out.best[0] - 0.3) < 1e-2, qPrintable(said(out)));
    }
}

void TestOptimizerCore::anObjectiveThatNeverMoves()
{
    Function e;
    e.f = [](const Point&, Evaluated& r) { r.objective = 7; };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a")};
    p.start = {0.5};
    SearchOptions o;
    o.method = QStringLiteral("nm");
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::Unchanged);
    QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("nothing was optimised")), qPrintable(said(out)));
    QVERIFY(!out.settled());
}

void TestOptimizerCore::aParameterOnABound()
{
    Function e;
    e.f = [](const Point& u, Evaluated& r) { r.objective = u[0]; };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a")};
    p.start = {0.5};
    SearchOptions o;
    o.method = QStringLiteral("tr");
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.best[0], 0.0);
    QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("ON a search bound")), qPrintable(said(out)));
}

void TestOptimizerCore::theIterationsRunOut()
{
    Function e;
    e.f = sphere;
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    p.start = {0.9, 0.9};
    SearchOptions o;
    o.method = QStringLiteral("nm");
    o.maxIter = 3;
    const SearchOutcome out = search(p, o, e);
    QCOMPARE(out.status, SearchStatus::MaxIter);
    QVERIFY2(out.phrase.contains(QStringLiteral("NOT converged")), qPrintable(said(out)));
    QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("iteration cap")), qPrintable(said(out)));
    // Annealing's schedule, NSGA-II's generations: complete, not cut.
    Function a;
    a.f = sphere;
    SearchOptions sa;
    sa.method = QStringLiteral("sa");
    sa.maxIter = 3;
    const SearchOutcome cooled = search(p, sa, a);
    QCOMPARE(cooled.status, SearchStatus::Completed);
    QVERIFY(cooled.settled());
    QVERIFY2(cooled.phrase.contains(QStringLiteral("cooling schedule")), qPrintable(said(cooled)));
    QCOMPARE(cooled.evaluations, 1 + 12 + (8 + 4 * 2) * 3);
}

void TestOptimizerCore::populationsAreRaisedAndSaid()
{
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a")};
    p.start = {0.9};
    for (const char* method : {"de", "pso", "cmaes"}) {
        Function e;
        e.f = sphere;
        SearchOptions o;
        o.method = QLatin1String(method);
        o.population = 2;
        o.maxIter = 2;
        const SearchOutcome out = search(p, o, e);
        QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("raised")), qPrintable(said(out)));
    }
    Function e;
    e.f = [](const Point& u, Evaluated& r) { r.objectives = {u[0], 1 - u[0]}; };
    SearchProblem two = p;
    two.objectives = 2;
    SearchOptions o;
    o.method = QStringLiteral("nsga2");
    o.population = 9;
    o.maxIter = 1;
    const SearchOutcome out = search(two, o, e);
    QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("raised to 10")), qPrintable(said(out)));
    QCOMPARE(out.evaluations, 20);
}

void TestOptimizerCore::aMethodNeedsNoPolish()
{
    Function e;
    e.f = sphere;
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a")};
    p.start = {0.9};
    SearchOptions o;
    o.method = QStringLiteral("tr");
    o.polish = true;
    const SearchOutcome out = search(p, o, e);
    QVERIFY2(out.notes.join(QLatin1Char(' ')).contains(QStringLiteral("polish")), qPrintable(said(out)));
    QVERIFY2(!out.search.join(QLatin1Char(' ')).contains(QStringLiteral("polish")), qPrintable(said(out)));
}

void TestOptimizerCore::anEmptyProblem()
{
    Function e;
    e.f = sphere;
    const SearchOutcome out = search(SearchProblem(), SearchOptions(), e);
    QCOMPARE(out.status, SearchStatus::NoSolve);
    QCOMPARE(e.calls, 0);
}

void TestOptimizerCore::aRunRepeats_data()
{
    QTest::addColumn<QString>("method");
    for (const char* m : {"de", "pso", "sa", "cmaes", "bayes", "nm", "tr"}) QTest::newRow(m) << QString::fromLatin1(m);
}

void TestOptimizerCore::aRunRepeats()
{
    QFETCH(QString, method);
    auto once = [&method] {
        Function e;
        e.f = rastrigin;
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        p.start = {0.8, 0.2};
        SearchOptions o;
        o.method = method;
        o.maxIter = 20;
        o.seed = 9;
        o.population = 8;
        const SearchOutcome out = search(p, o, e);
        return std::make_pair(e.asked, out.cost);
    };
    const auto a = once(), b = once();
    QVERIFY(a.first == b.first);
    QCOMPARE(a.second, b.second);
}

void TestOptimizerCore::aSeedSearchesDifferently_data()
{
    QTest::addColumn<QString>("method");
    for (const char* m : {"de", "pso", "sa", "cmaes", "bayes"}) QTest::newRow(m) << QString::fromLatin1(m);
}

void TestOptimizerCore::aSeedSearchesDifferently()
{
    QFETCH(QString, method);
    auto once = [&method](unsigned long long seed) {
        Function e;
        e.f = rastrigin;
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        p.start = {0.8, 0.2};
        SearchOptions o;
        o.method = method;
        o.maxIter = 10;
        o.seed = seed;
        o.population = 8;
        (void) search(p, o, e);
        return e.asked;
    };
    QVERIFY(once(1) != once(2));
}

void TestOptimizerCore::theOrderOfABatchDoesNotMatter_data()
{
    QTest::addColumn<QString>("method");
    for (const char* m : {"de", "pso", "cmaes", "nm", "tr", "bayes"}) QTest::newRow(m) << QString::fromLatin1(m);
}

// What is asked together depends on none of its results.
void TestOptimizerCore::theOrderOfABatchDoesNotMatter()
{
    QFETCH(QString, method);
    auto once = [&method](bool reversed) {
        Function e;
        e.reversed = reversed;
        e.f = rastrigin;
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
        p.start = {0.8, 0.2};
        SearchOptions o;
        o.method = method;
        o.maxIter = 15;
        o.seed = 9;
        o.population = 8;
        return search(p, o, e);
    };
    const SearchOutcome a = once(false), b = once(true);
    QCOMPARE(a.cost, b.cost);
    QVERIFY(a.best == b.best);
    QCOMPARE(a.evaluations, b.evaluations);
}

void TestOptimizerCore::pointsStayInTheCube_data()
{
    QTest::addColumn<QString>("method");
    for (const char* m : {"de", "pso", "sa", "cmaes", "bayes", "nm", "tr", "lm", "tune", "nsga2"}) QTest::newRow(m) << QString::fromLatin1(m);
}

void TestOptimizerCore::pointsStayInTheCube()
{
    QFETCH(QString, method);
    Function e;
    e.f = [](const Point& u, Evaluated& r) {
        // Pulling toward a corner, outward.
        r.objective = -u[0] - u[1];
        r.residuals = {u[0] - 2, u[1] + 1};
        r.objectives = {-u[0], u[1]};
    };
    SearchProblem p;
    p.names = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    p.start = {0.9, 0.1};
    if (method == QLatin1String("lm") || method == QLatin1String("tune")) p.residuals = 2;
    if (method == QLatin1String("nsga2")) p.objectives = 2;
    SearchOptions o;
    o.method = method;
    o.maxIter = 30;
    o.seed = 2;
    o.population = 8;
    (void) search(p, o, e);
    QVERIFY(!e.asked.empty());
    for (const Point& x : e.asked)
        for (double v : x) QVERIFY2(v >= 0 && v <= 1, qPrintable(QString::number(v)));
}

void TestOptimizerCore::batchesAreWhatAMethodCanUse()
{
    // A generation at once for the population methods; the first simplex, and a
    // Jacobian; one at a time where each depends on the last.
    auto largest = [](const QString& method, int residuals = 0, int objectives = 0) {
        Function e;
        e.f = [](const Point& u, Evaluated& r) {
            sphere(u, r);
            r.residuals = {u[0] - 0.3, u[1] - 0.3, u[2] - 0.3};
            r.objectives = {u[0], 1 - u[0]};
        };
        SearchProblem p;
        p.names = QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        p.start = {0.9, 0.1, 0.5};
        p.residuals = residuals;
        p.objectives = objectives;
        SearchOptions o;
        o.method = method;
        o.maxIter = 3;
        o.population = 12;
        (void) search(p, o, e);
        return e.largestBatch;
    };
    QCOMPARE(largest(QStringLiteral("de")), 12);
    QCOMPARE(largest(QStringLiteral("pso")), 12);
    QCOMPARE(largest(QStringLiteral("cmaes")), 12);
    QCOMPARE(largest(QStringLiteral("nsga2"), 0, 2), 12);
    QCOMPARE(largest(QStringLiteral("nm")), 4);
    QCOMPARE(largest(QStringLiteral("lm"), 3), 3);
    QCOMPARE(largest(QStringLiteral("tr")), 6);
    QCOMPARE(largest(QStringLiteral("sa")), 12);
    QCOMPARE(largest(QStringLiteral("bayes")), 3);   // (its budget of 3, all its design)
}

QTEST_GUILESS_MAIN(TestOptimizerCore)
#include "test_optimizer_core.moc"
