/*
 * test_optimization.cpp - an optimization component run with ngspice
 * (upstream #1327): the component's variables, goals and settings, the E
 * series, goals measured in a dataset, the cost, differential evolution,
 * the netlist of a candidate - and, with ngspice installed, whole runs
 * from the application: a voltage divider tuned to a ratio, with a
 * variable an equation defines and one only the resistor names, a goal
 * the results do not have, a run stopped early, and DC bias beside an
 * optimization component.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "optimization.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "components/component.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/optimizer.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::optimization;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// A voltage divider: 1 V over R1 = 1k and R2 = Rx, vo the voltage across
// R2 in an AC analysis of one point, and a transient beside it. vo = 0.25
// wants Rx = 1k/3.
QString divider(const QString& file, const QString& rx, const QString& equations, const QString& optimization)
{
    // The dataset and the display after the file, or opening it asks.
    const QString base = QFileInfo(file).completeBaseName();
    return QStringLiteral(
               "<Qucs Schematic " PACKAGE_VERSION ">\n"
               "<Properties>\n  <View=0,0,800,600,1,0,0>\n  <Grid=10,10,1>\n"
               "  <DataSet=%4.dat>\n  <DataDisplay=%4.dpl>\n  <OpenDisplay=0>\n"
               "  <Script=%4.m>\n  <RunScript=0>\n  <showFrame=0>\n</Properties>\n"
               "<Symbol>\n</Symbol>\n<Components>\n"
               "  <Vac V1 1 100 200 18 -26 1 1 \"1 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0>\n"
               "  <GND * 1 100 260 0 0 0 0>\n"
               "  <R R1 1 200 140 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
               "  <R R2 1 300 200 15 -26 0 1 \"%1\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
               "  <GND * 1 300 260 0 0 0 0>\n"
               "  <.AC AC1 1 100 350 0 45 0 0 \"lin\" 1 \"1 kHz\" 1 \"1 kHz\" 1 \"1\" 1 \"no\" 0>\n"
               "  <.TR TR1 1 100 450 0 75 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"11\" 0 \"Trapezoidal\" 0 \"2\" 0 "
               "\"1 ns\" 0 \"1e-16\" 0 \"150\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"26.85\" 0 \"1e-3\" 0 "
               "\"1e-6\" 0 \"1\" 0 \"CroutLU\" 0 \"no\" 0 \"yes\" 0 \"0\" 0>\n"
               "  <Eqn Eqn1 1 300 350 -31 17 0 0 %2 \"yes\" 0>\n"
               "  <.Opt Opt1 1 500 350 0 44 0 0 %3>\n"
               "</Components>\n<Wires>\n"
               "  <100 230 100 260 \"\" 0 0 0 \"\">\n"
               "  <100 140 100 170 \"\" 0 0 0 \"\">\n"
               "  <100 140 170 140 \"\" 0 0 0 \"\">\n"
               "  <230 140 300 140 \"out\" 260 110 0 \"\">\n"
               "  <300 140 300 170 \"\" 0 0 0 \"\">\n"
               "  <300 230 300 260 \"\" 0 0 0 \"\">\n"
               "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
        .arg(rx, equations, optimization, base);
}

bool write(const QString& file, const QString& text)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(text.toUtf8());
    return true;
}

Component* find(Schematic* doc, const QString& name)
{
    for (Component* c : doc->a_DocComps)
        if (c->Name == name) return c;
    return nullptr;
}

QStringList values(const Component* c, const QString& name)
{
    QStringList v;
    for (const Property* p : c->Props)
        if (p->Name == name) v << p->Value;
    return v;
}

double sphere(const QVector<double>& x)
{
    double s = 0;
    for (double v : x) s += (v - 0.3) * (v - 0.3);
    return s;
}

} // namespace

class TestOptimization : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    // Runs the schematic's simulation (F2) in the application until the
    // console is done; the document is left open for the checks.
    Schematic* simulate(QucsApp& app, const QString& file, int timeout = 120000)
    {
        if (!app.gotoPage(file)) return nullptr;
        Schematic* doc = app.currentSchematic();
        if (doc == nullptr) return nullptr;
        if (!QMetaObject::invokeMethod(&app, "slotSimulate")) return nullptr;
        QTest::qWait(50);
        QElapsedTimer t;
        t.start();
        while (app.simulationConsole()->isRunning() && t.elapsed() < timeout) QTest::qWait(50);
        return doc;
    }

    bool ngspice()
    {
        const QString exe = QStandardPaths::findExecutable("ngspice");
        if (exe.isEmpty()) return false;
        QucsSettings.NgspiceExecutable = exe;
        return true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.S4Qworkdir = dir.filePath("work");
        QucsSettings.tempFilesDir.setPath(dir.filePath("work"));
        QVERIFY(QDir().mkpath(dir.filePath("work")));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // What the component stores, as its dialog writes it and as the
    // shipped qucsator examples have it.
    void theComponentIsRead()
    {
        Variable v;
        QString why;
        QVERIFY(Variable::parse("Rx|yes|1k|10|100000|LOG_DOUBLE", &v, &why));
        QCOMPARE(v.name, QStringLiteral("Rx"));
        QVERIFY(v.active);
        QCOMPARE(v.initial, 1000.0);
        QCOMPARE(v.min, 10.0);
        QCOMPARE(v.max, 1e5);
        QVERIFY(v.logarithmic());
        QCOMPARE(v.lower(), 1.0);
        QCOMPARE(v.upper(), 5.0);
        QVERIFY(std::abs(v.value(v.coordinate(333.3)) - 333.3) < 1e-9);
        QCOMPARE(v.value(7), 1e5);   // beyond the bound: the bound

        QVERIFY(Variable::parse("L1|yes|3.900000E-07|100e-9|560e-9|E12", &v));
        QCOMPARE(v.initial, 3.9e-7);
        QVERIFY(v.logarithmic());
        QVERIFY(std::abs(v.value(v.coordinate(4.5e-7)) - 4.7e-7) < 1e-18);

        QVERIFY(Variable::parse("N|no|3|1|10|LIN_INT", &v));
        QVERIFY(!v.active);
        QVERIFY(!v.logarithmic());
        QCOMPARE(v.value(4.6), 5.0);
        QCOMPARE(v.value(0.2), 1.0);

        // A linear variable with a bound at 0 cannot be logarithmic.
        QVERIFY(Variable::parse("x|yes|1|0|10|LOG_DOUBLE", &v));
        QVERIFY(!v.logarithmic());

        QVERIFY(!Variable::parse("x|yes|abc|1|2|LIN_DOUBLE", &v, &why));
        QVERIFY2(why.contains("initial"), qPrintable(why));
        QVERIFY(!Variable::parse("x|yes|1|5|2|LIN_DOUBLE", &v, &why));
        QVERIFY2(why.contains("above"), qPrintable(why));
        QVERIFY(!Variable::parse("x|yes|1", &v, &why));

        Goal g;
        QVERIFY(Goal::parse("Min_S11|LE|-15", &g));
        QCOMPARE(g.type, GoalType::LessEqual);
        QCOMPARE(g.target, -15.0);
        QVERIFY(g.met(-20));
        QVERIFY(!g.met(-10));
        QCOMPARE(g.describe(-20), QStringLiteral("Min_S11 = -20 (<= -15, met)"));
        QVERIFY(Goal::parse("Ripple|MIN|0", &g));
        QCOMPARE(g.type, GoalType::Minimize);
        QVERIFY(g.isObjective());
        QVERIFY(Goal::parse("gain|MON|", &g));
        QCOMPARE(g.type, GoalType::Monitor);
        QVERIFY(!Goal::parse("gain|GE|lots", &g, &why));

        const Settings s = Settings::parse("3|1000|2|50|0.85|0.95|3|1e-6|10|100");
        QCOMPARE(s.method, 3);
        QCOMPARE(s.methodName(), QStringLiteral("DE/rand-to-best/1/exp"));
        QCOMPARE(s.generations, 1000);
        QCOMPARE(s.population, 50);
        QCOMPARE(s.F, 0.85);
        QCOMPARE(s.CR, 0.95);
        QCOMPARE(s.minCostVariance, 1e-6);
        QCOMPARE(s.constraintWeight, 100.0);
        QCOMPARE(Settings::parse("5|x").methodName(), QStringLiteral("DE/rand/2/exp"));
        QCOMPARE(Settings::parse("5|x").generations, 50);   // the default for what is not a number
    }

    void theSeriesAreTheStandards()
    {
        QCOMPARE(eSeries("E3").size(), 3);
        QCOMPARE(eSeries("E24").size(), 24);
        QVERIFY(eSeries("E24").contains(2.7));   // not the formula's 2.6
        QVERIFY(eSeries("E24").contains(8.2));
        const QList<double> e96 = eSeries("E96");
        QCOMPARE(e96.size(), 96);
        QCOMPARE(e96.at(1), 1.02);
        QCOMPARE(e96.last(), 9.76);
        QCOMPARE(eSeries("E192").at(185), 9.19);
        QCOMPARE(eSeries("E48").at(47), 9.53);
        QVERIFY(eSeries("LIN_DOUBLE").isEmpty());
        QCOMPARE(nearestInSeries(eSeries("E12"), 1.25), 1.2);
        QVERIFY(std::abs(nearestInSeries(eSeries("E12"), 9.6e3) - 1e4) < 1e-9);   // into the next decade

        // The nearest preferred value inside the bounds.
        Variable v;
        QVERIFY(Variable::parse("C|yes|100p|56e-12|330e-12|E24", &v));
        QVERIFY(std::abs(v.value(v.coordinate(50e-12)) - 56e-12) < 1e-21);
        QVERIFY(std::abs(v.value(std::log10(400e-12)) - 330e-12) < 1e-21);
    }

    void goalsAreMeasuredInTheDataset()
    {
        const QString file = dir.filePath("goals.dat");
        QVERIFY(write(file, "<Qucs Dataset 26.1.2>\n"
                            "<indep frequency 3>\n+1.0e+03\n+2.0e+03\n+3.0e+03\n</indep>\n"
                            "<dep ac.v(out) frequency>\n+3.0e+00+j4.0e+00\n+6.0e+00-j8.0e+00\n+0.0e+00+j1.0e+00\n</dep>\n"
                            "<dep ac.gain_db frequency>\n-1.0e+00+j0.0e+00\n-3.0e+00+j0.0e+00\n-2.0e+00+j0.0e+00\n</dep>\n"
                            "<indep ac.gmax 1>\n-2.5e-01+j0.0e+00\n</indep>\n"
                            "<indep tran.gmax 1>\n+7.0e+00\n</indep>\n"
                            "<dep ac.v(vo) frequency>\n+1.0e+00\n+2.0e+00\n+3.0e+00\n</dep>\n"
                            "<dep ac.i(vpr1) frequency>\n+1.0e+00\n+2.0e+00\n+3.0e+00\n</dep>\n"));
        QString why;
        const QHash<QString, QVector<double>> table = readDataset(file, &why);
        QVERIFY2(!table.isEmpty(), qPrintable(why));
        QCOMPARE(table.value("frequency"), QVector<double>({1e3, 2e3, 3e3}));
        QCOMPARE(table.value("ac.v(out)"), QVector<double>({5, 10, 1}));   // magnitudes
        QCOMPARE(table.value("ac.gain_db"), QVector<double>({-1, -3, -2}));   // real
        QCOMPARE(table.value("ac.gmax"), QVector<double>({-0.25}));

        const QStringList names = table.keys();
        QCOMPARE(resolve(names, "gain_dB"), QStringLiteral("ac.gain_db"));
        QCOMPARE(resolve(names, "v(out)"), QStringLiteral("ac.v(out)"));
        QCOMPARE(resolve(names, "ac.gmax"), QStringLiteral("ac.gmax"));
        QCOMPARE(resolve(names, "gmax"), QString());   // two of them
        QCOMPARE(resolve(names, "gmax", "tran"), QStringLiteral("tran.gmax"));
        QCOMPARE(resolve(names, "out)"), QString());   // not a name after a prefix
        // ngspice writes an expression of a voltage as one: vo = mag(v(out)).
        QCOMPARE(resolve(names, "vo"), QStringLiteral("ac.v(vo)"));
        QCOMPARE(resolve(names, "out"), QStringLiteral("ac.v(out)"));
        QCOMPARE(resolve(names, "vpr1"), QStringLiteral("ac.i(vpr1)"));
        QCOMPARE(resolve(names, "nothere"), QString());
        QCOMPARE(analysisPrefix(".AC"), QStringLiteral("ac"));
        QCOMPARE(analysisPrefix(".TR"), QStringLiteral("tran"));

        // A sweep by its worst point for the goal.
        const QVector<double> sweep = table.value("ac.gain_db");
        Goal g;
        g.type = GoalType::GreaterEqual;
        QCOMPARE(measure(g, sweep), -3.0);
        g.type = GoalType::LessEqual;
        QCOMPARE(measure(g, sweep), -1.0);
        g.type = GoalType::Maximize;
        QCOMPARE(measure(g, sweep), -3.0);
        g.type = GoalType::Minimize;
        QCOMPARE(measure(g, sweep), -1.0);
        g.type = GoalType::Equal;
        g.target = -2.2;
        QCOMPARE(measure(g, sweep), -1.0);
        QVERIFY(std::isnan(measure(g, {})));

        QVERIFY(readDataset(dir.filePath("none.dat"), &why).isEmpty());
        QVERIFY(why.contains("none.dat"));
    }

    void theCostWeighsTheGoals()
    {
        Settings s;   // objectives 10, constraints 100
        QList<Goal> goals(3);
        goals[0].type = GoalType::GreaterEqual;
        goals[0].target = 20;
        goals[1].type = GoalType::Minimize;
        goals[2].type = GoalType::Monitor;
        const QVector<double> scale = scaleOf({15, -4, 0});
        QCOMPARE(scale, QVector<double>({15, 4, 1}));
        // 5 short of 20: 100 * 5/20; -2 to minimize, against 4: 10 * -0.5.
        QCOMPARE(cost(goals, {15, -2, 99}, scale, s), 25.0 - 5.0);
        QCOMPARE(cost(goals, {25, -2, 99}, scale, s), -5.0);
        goals[1].type = GoalType::Maximize;
        QCOMPARE(cost(goals, {25, -2, 99}, scale, s), 5.0);
        goals[0].type = GoalType::Equal;
        goals[0].target = 0;   // relative to 1
        QCOMPARE(cost(goals, {-0.5, 0, 0}, scale, s), 50.0);
        QVERIFY(std::isinf(cost(goals, {NAN, 0, 0}, scale, s)));
        QCOMPARE(cost(goals, {0, 0, NAN}, scale, s), 0.0);   // a monitor does not count
    }

    void everyMethodFindsTheMinimum_data()
    {
        QTest::addColumn<int>("method");
        for (int m = 1; m <= 10; ++m) QTest::newRow(qPrintable(Settings{m}.methodName())) << m;
    }

    void everyMethodFindsTheMinimum()
    {
        QFETCH(int, method);
        Settings s;
        s.method = method;
        s.generations = 300;
        s.population = 20;
        s.F = 0.7;
        s.CR = 0.9;
        s.minCostVariance = 0;   // all the generations
        auto run = [&] {
            DifferentialEvolution de({-1, -1, -1}, {1, 1, 1}, s);
            const QVector<double> x0{0.9, -0.9, 0.9};
            de.start(x0, sphere(x0));
            while (!de.finished()) {
                const auto candidates = de.next();
                QVector<double> costs;
                for (const auto& c : candidates) {
                    for (int d = 0; d < 3; ++d) {
                        if (c.x.at(d) < -1 || c.x.at(d) > 1) return std::make_pair(-1.0, de.best());
                    }
                    costs << sphere(c.x);
                }
                de.report(costs);
            }
            return std::make_pair(de.bestCost(), de.best());
        };
        const auto [cost1, best1] = run();
        QVERIFY2(cost1 >= 0, "a candidate outside the bounds");
        QVERIFY2(cost1 < 1e-8, qPrintable(QString::number(cost1)));
        // The same seed, the same search.
        const auto [cost2, best2] = run();
        QCOMPARE(cost2, cost1);
        QCOMPARE(best2, best1);
    }

    void evolutionKeepsTheStartAndStops()
    {
        Settings s;
        s.generations = 4;
        s.population = 3;   // raised to 6: DE/x/2 draws five others
        DifferentialEvolution de({0}, {1}, s);
        QCOMPARE(de.populationSize(), 6);
        de.start({0.3}, 0.0);   // the start is the optimum
        QCOMPARE(de.next().size(), 5);   // the random members
        de.report({1, 2, 3, 4, 5});
        QCOMPARE(de.generation(), 0);
        QCOMPARE(de.best(), QVector<double>({0.3}));
        int generations = 0;
        while (!de.finished()) {
            const auto c = de.next();
            QCOMPARE(c.size(), 6);
            de.report(QVector<double>(c.size(), 7.0));   // none better
            ++generations;
        }
        QCOMPARE(generations, 4);
        QCOMPARE(de.bestCost(), 0.0);
        QVERIFY(de.next().isEmpty());
    }

    // The netlist of a candidate: the values in the definitions, a .PARAM
    // for a variable nothing defines, only the optimized simulation - and
    // the schematic as it was afterwards.
    void theNetlistCarriesTheCandidate()
    {
        const QString file = dir.filePath("scope.sch");
        QVERIFY(write(file, divider(file, "Rx", "\"Rx=1k\" 1 \"vo=mag(out.v)\" 1",
                                    "\"Sim=AC1\" 0 \"DE=7|30|5|12|0.8|0.9|1|1e-10|10|100\" 0 "
                                    "\"Var=Rx|yes|1000|10|100000|LOG_DOUBLE\" 0 "
                                    "\"Var=Cz|no|4.7n|1n|10n|E12\" 0 \"Goal=vo|EQ|0.25\" 0")));
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        Component* opt = find(&doc, "Opt1");
        QVERIFY(opt != nullptr);
        Problem problem;
        QString why;
        QVERIFY2(Problem::read(opt, &problem, &why), qPrintable(why));
        QCOMPARE(problem.simulation, QStringLiteral("AC1"));
        QCOMPARE(problem.variables.size(), 2);
        QCOMPARE(problem.lower().size(), 1);   // Cz is held
        QCOMPARE(simulationsOf(&doc, "AC1"), QStringList({"ac1"}));
        QCOMPARE(simulationsOf(&doc, QString()), QStringList({"ac1", "tr1"}));
        QCOMPARE(Optimizer::analysisOf(&doc, "AC1"), QStringLiteral("ac"));

        const QMap<QString, double> values = problem.values({std::log10(333.3)});
        QCOMPARE(values.value("Cz"), 4.7e-9);
        const QString netlist = dir.filePath("scope.cir");
        {
            const NetlistScope scope(&doc, problem, values);
            QCOMPARE(scope.undefined(), QStringList({"Cz"}));
            QCOMPARE(scope.extraParameters(), QStringLiteral(".PARAM Cz=4.7e-09\n"));
            QCOMPARE(find(&doc, "Eqn1")->Props.at(0)->Value, QStringLiteral("333.3"));
            QCOMPARE(find(&doc, "TR1")->isActive, COMP_IS_OPEN);
            QCOMPARE(opt->isActive, COMP_IS_OPEN);
            Ngspice kernel(&doc);
            kernel.setExtraParameters(scope.extraParameters());
            kernel.SaveNetlist(netlist, false);
        }
        QCOMPARE(find(&doc, "Eqn1")->Props.at(0)->Value, QStringLiteral("1k"));
        QCOMPARE(find(&doc, "TR1")->isActive, COMP_IS_ACTIVE);
        QCOMPARE(opt->isActive, COMP_IS_ACTIVE);

        QFile f(netlist);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY2(text.contains(".PARAM Rx=333.3"), qPrintable(text));
        QVERIFY2(text.contains(".PARAM Cz=4.7e-09"), qPrintable(text));
        QVERIFY2(text.contains("let Rx=333.3"), qPrintable(text));
        QVERIFY2(text.contains("ac lin 1"), qPrintable(text));
        QVERIFY2(!text.contains("tran "), qPrintable(text));

        // Every simulation for the best point's run.
        {
            const NetlistScope scope(&doc, problem, values, /*allSimulations=*/true);
            QCOMPARE(find(&doc, "TR1")->isActive, COMP_IS_ACTIVE);
            QCOMPARE(opt->isActive, COMP_IS_OPEN);
        }

        // The pre-flight check takes it: Qucs runs it, with ngspice.
        QVERIFY(opt->Simulator & spicecompat::simNgspice);
        for (const auto& issue : qucs_s::erc::check(&doc))
            QVERIFY2(issue.component != "Opt1", qPrintable(issue.message));
        QStringList incompatible;
        Ngspice kernel(&doc);
        QVERIFY(kernel.checkSchematic(incompatible));
    }

    void anIncompleteComponentIsRefused()
    {
        Problem p;
        QString why;
        Component c;
        c.Name = "Opt1";
        c.Props.append(new Property("Sim", "AC1"));
        c.Props.append(new Property("DE", "3|50|2|20|0.85|1|3|1e-6|10|100"));
        QVERIFY(!Problem::read(&c, &p, &why));
        QVERIFY2(why.contains("no variables"), qPrintable(why));
        c.Props.append(new Property("Var", "R|no|1|0|2|LIN_DOUBLE"));
        QVERIFY(!Problem::read(&c, &p, &why));
        QVERIFY2(why.contains("none of the variables"), qPrintable(why));
        c.Props.last()->Value = "R|yes|1|0|2|LIN_DOUBLE";
        c.Props.append(new Property("Goal", "g|MON|"));
        QVERIFY(!Problem::read(&c, &p, &why));
        QVERIFY2(why.contains("no goal"), qPrintable(why));
        c.Props.last()->Value = "g|MAX|";
        QVERIFY(Problem::read(&c, &p, &why));
        qDeleteAll(c.Props);
        c.Props.clear();
    }

    // The Problems tab says what the run would refuse.
    void thePreflightCheckReadsTheComponent()
    {
        const QString file = dir.filePath("erc.sch");
        QVERIFY(write(file, divider(file, "Rx", "\"Rx=1k\" 1 \"vo=mag(out.v)\" 1",
                                    "\"Sim=AC7\" 0 \"DE=7|30|5|12|0.8|0.9|1|1e-10|10|100\" 0 "
                                    "\"Var=Rx|yes|1000|10|100000|LOG_DOUBLE\" 0 \"Goal=vo|EQ|0.25\" 0")));
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        auto issuesOf = [&] {
            QStringList out;
            for (const auto& issue : qucs_s::erc::check(&doc))
                if (issue.component == "Opt1") out << issue.message;
            return out;
        };
        QCOMPARE(issuesOf(), QStringList({"Opt1 optimizes AC7, which is not in the schematic"}));
        Component* opt = find(&doc, "Opt1");
        opt->Props.at(0)->Value = "AC1";
        QVERIFY(issuesOf().isEmpty());
        opt->Props.at(2)->Value = "Rx|yes|1000|10|lots|LOG_DOUBLE";
        QCOMPARE(issuesOf(), QStringList({"Rx: the maximum \"lots\" is not a number"}));
        opt->Props.at(2)->Value = "Rx|yes|1000|10|100000|LOG_DOUBLE";
        // Not for Xyce: the component is not available there at all.
        QucsSettings.DefaultSimulator = spicecompat::simXyce;
        const QStringList xyce = issuesOf();
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QCOMPARE(xyce.size(), 1);
        QVERIFY2(xyce.first().contains("not available for"), qPrintable(xyce.first()));
    }

    // With ngspice: the divider tuned to vo = 0.25. Rx defined by the
    // equation and searched on a logarithmic scale; or named only by the
    // resistor (the netlist gets a .PARAM) and taken from the E24 series,
    // where 330 is the nearest to 1k/3.
    void aDividerIsTuned_data()
    {
        QTest::addColumn<QString>("equations");
        QTest::addColumn<QString>("type");
        QTest::addColumn<double>("expected");
        QTest::addColumn<double>("tolerance");
        QTest::newRow("equation, log") << "\"Rx=1k\" 1 \"vo=mag(out.v)\" 1" << "LOG_DOUBLE" << 1000.0 / 3 << 0.01;
        QTest::newRow("resistor only, E24") << "\"vo=mag(out.v)\" 1" << "E24" << 330.0 << 1e-9;
    }

    void aDividerIsTuned()
    {
        QFETCH(QString, equations);
        QFETCH(QString, type);
        QFETCH(double, expected);
        QFETCH(double, tolerance);
        if (!ngspice()) QSKIP("ngspice is not installed");

        const QString file = dir.filePath("div.sch");
        QFile::remove(dir.filePath("div.dat.ngspice"));
        QVERIFY(write(file, divider(file, "Rx", equations,
                                    QStringLiteral("\"Sim=AC1\" 0 \"DE=7|40|5|12|0.8|0.9|1|1e-12|10|100\" 0 "
                                                   "\"Var=Rx|yes|1000|10|100000|%1\" 0 \"Goal=vo|EQ|0.25\" 0")
                                        .arg(type))));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file);
        QVERIFY(doc != nullptr);
        QVERIFY(!app.simulationConsole()->isRunning());

        const QString console = app.simulationConsole()->console()->toPlainText();
        QVERIFY2(console.contains("Optimization Opt1 of AC1 with ngspice: 1 variable, 1 goal"), qPrintable(console));
        QVERIFY2(console.contains("Start: cost 100"), qPrintable(console));   // vo = 0.5: 100 * 0.25/0.25
        QVERIFY2(console.contains("Best: cost"), qPrintable(console));
        QVERIFY2(console.contains("Simulating the best point"), qPrintable(console));

        // The best value is the variable's initial value now.
        Variable v;
        QVERIFY(Variable::parse(values(find(doc, "Opt1"), "Var").value(0), &v));
        QVERIFY2(std::abs(v.initial - expected) <= tolerance * expected, qPrintable(QString::number(v.initial, 'g', 10)));
        QVERIFY(doc->getDocChanged());
        // The equation is as it was.
        if (equations.startsWith("\"Rx=")) QCOMPARE(find(doc, "Eqn1")->Props.at(0)->Value, QStringLiteral("1k"));

        // The dataset is the best point's, every simulation of it.
        const QHash<QString, QVector<double>> table = readDataset(dir.filePath("div.dat.ngspice"));
        const QString vo = resolve(table.keys(), "vo", "ac");
        QVERIFY2(!vo.isEmpty(), qPrintable(table.keys().join(' ')));
        QVERIFY(std::abs(table.value(vo).value(0) - v.initial / (1000 + v.initial)) < 1e-6);
        QVERIFY2(std::any_of(table.keyBegin(), table.keyEnd(), [](const QString& k) { return k.startsWith("tran."); }),
                 qPrintable(table.keys().join(' ')));

        // In a folder of each worker, under the schematic's Scratch folder.
        QVERIFY(QFileInfo::exists(misc::scratchDirFor(doc->getDocName()) + "/opt/1/spice4qucs.cir"));
        const QListWidget* log = app.simulationConsole()->statusLog();
        QVERIFY(log->count() > 0);
        bool finished = false;
        for (int i = 0; i < log->count(); ++i) finished |= log->item(i)->text().startsWith("Optimization finished");
        QVERIFY(finished);
    }

    void aGoalTheResultsLackStopsTheRun()
    {
        if (!ngspice()) QSKIP("ngspice is not installed");
        const QString file = dir.filePath("nogoal.sch");
        QVERIFY(write(file, divider(file, "Rx", "\"Rx=1k\" 1 \"vo=mag(out.v)\" 1",
                                    "\"Sim=AC1\" 0 \"DE=7|40|5|12|0.8|0.9|1|1e-12|10|100\" 0 "
                                    "\"Var=Rx|yes|1000|10|100000|LOG_DOUBLE\" 0 \"Goal=nothere|GE|1\" 0")));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file);
        QVERIFY(doc != nullptr);
        const QString console = app.simulationConsole()->console()->toPlainText();
        QVERIFY2(console.contains("The start cannot be simulated: the results have no nothere"), qPrintable(console));
        QVERIFY2(console.contains("ac.v(vo)"), qPrintable(console));   // what there is
        QCOMPARE(values(find(doc, "Opt1"), "Var").value(0), QStringLiteral("Rx|yes|1000|10|100000|LOG_DOUBLE"));
        QVERIFY(!doc->getDocChanged());
        const QListWidget* log = app.simulationConsole()->statusLog();
        QVERIFY(log->item(log->count() - 1)->text().contains("Optimization failed"));

        // A simulation the component names that is not there.
        find(doc, "Opt1")->Props.at(0)->Value = "AC7";
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulate"));
        QTRY_VERIFY(!app.simulationConsole()->isRunning());
        QVERIFY(log->item(log->count() - 1)->text().contains("AC7, which is not in the schematic"));
    }

    void aStoppedRunKeepsTheBest()
    {
        if (!ngspice()) QSKIP("ngspice is not installed");
        const QString file = dir.filePath("stop.sch");
        QVERIFY(write(file, divider(file, "Rx", "\"Rx=1k\" 1 \"vo=mag(out.v)\" 1",
                                    "\"Sim=AC1\" 0 \"DE=7|100000|1|40|0.8|0.9|1|0|10|100\" 0 "
                                    "\"Var=Rx|yes|1000|10|100000|LOG_DOUBLE\" 0 \"Goal=vo|EQ|0.25\" 0")));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        Schematic* doc = app.currentSchematic();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulate"));
        SimulationConsole* console = app.simulationConsole();
        QTRY_VERIFY_WITH_TIMEOUT(console->console()->toPlainText().contains("Generation 2 "), 60000);
        QVERIFY(console->isRunning());
        QVERIFY(QMetaObject::invokeMethod(console, "slotStop"));
        QTRY_VERIFY_WITH_TIMEOUT(!console->isRunning(), 60000);
        const QString text = console->console()->toPlainText();
        QVERIFY2(text.contains("Stopped after"), qPrintable(text));
        QVERIFY2(text.contains("Simulating the best point"), qPrintable(text));
        Variable v;
        QVERIFY(Variable::parse(values(find(doc, "Opt1"), "Var").value(0), &v));
        // Better than the start (vo = 0.5): nearer to 1k/3.
        QVERIFY2(std::abs(v.initial - 333.3) < 1000 - 333.3, qPrintable(QString::number(v.initial)));
    }

    // Limits the start meets already: nothing to search.
    void aStartThatMeetsTheGoalsIsKept()
    {
        if (!ngspice()) QSKIP("ngspice is not installed");
        const QString file = dir.filePath("met.sch");
        QVERIFY(write(file, divider(file, "Rx", "\"Rx=1k\" 1 \"vo=mag(out.v)\" 1",
                                    "\"Sim=AC1\" 0 \"DE=7|40|5|12|0.8|0.9|1|1e-12|10|100\" 0 "
                                    "\"Var=Rx|yes|1000|10|100000|LOG_DOUBLE\" 0 \"Goal=vo|LE|0.6\" 0 "
                                    "\"Goal=vo|MON|\" 0")));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file);
        QVERIFY(doc != nullptr);
        const QString console = app.simulationConsole()->console()->toPlainText();
        QVERIFY2(console.contains("The start meets every goal.\nBest: cost 0 after 1 simulation\n"), qPrintable(console));
        QVERIFY2(console.contains("vo = 0.5\n"), qPrintable(console));   // the monitor
        QVERIFY2(console.contains("Rx = 1 k\n"), qPrintable(console));
        QCOMPARE(values(find(doc, "Opt1"), "Var").value(0), QStringLiteral("Rx|yes|1000|10|100000|LOG_DOUBLE"));
        QVERIFY(!doc->getDocChanged());   // the same values: nothing written
    }

    // The example: an LC low-pass from E24 values, two goals measured by
    // Nutmeg equations over one sweep - both met at the end.
    void theShippedExampleMeetsItsGoals()
    {
        if (!ngspice()) QSKIP("ngspice is not installed");
        const QString file = dir.filePath("LC_lowpass_optimization.sch");
        QFile::remove(file);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/NGspice features/LC_lowpass_optimization.sch"),
                            file));
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file, 300000);
        QVERIFY(doc != nullptr);
        const QString console = app.simulationConsole()->console()->toPlainText();
        QVERIFY2(console.contains("Every goal is met after") && console.contains("Best: cost 0 "), qPrintable(console));
        QVERIFY2(console.contains("(>= -0.5, met)") && console.contains("(<= -30, met)"), qPrintable(console));

        const QList<double> e24 = eSeries("E24");
        for (const QString& value : values(find(doc, "Opt1"), "Var")) {
            Variable v;
            QVERIFY(Variable::parse(value, &v));
            const double mantissa = v.initial / std::pow(10.0, std::floor(std::log10(v.initial)));
            QVERIFY2(std::any_of(e24.begin(), e24.end(), [&](double e) { return std::abs(e - mantissa) < 1e-6; }),
                     qPrintable(value));
        }
        const QHash<QString, QVector<double>> table =
            readDataset(dir.filePath("LC_lowpass_optimization.dat.ngspice"));
        QVERIFY(table.value(resolve(table.keys(), "passband")).value(0) >= -0.5);
        QVERIFY(table.value(resolve(table.keys(), "stopband")).value(0) <= -30);
    }

    // DC bias does not optimize, and the component does not stand in its way.
    void dcBiasBesideAnOptimization()
    {
        if (!ngspice()) QSKIP("ngspice is not installed");
        const QString file = dir.filePath("bias.sch");
        QVERIFY(write(file, divider(file, "Rx", "\"Rx=1k\" 1 \"vo=mag(out.v)\" 1",
                                    "\"Sim=AC1\" 0 \"DE=7|40|5|12|0.8|0.9|1|1e-12|10|100\" 0 "
                                    "\"Var=Rx|yes|1000|10|100000|LOG_DOUBLE\" 0 \"Goal=vo|EQ|0.25\" 0")));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file));
        Schematic* doc = app.currentSchematic();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotDCbias"));
        QTRY_VERIFY_WITH_TIMEOUT(!app.simulationConsole()->isRunning(), 60000);
        QTRY_VERIFY(doc->getShowBias() > 0);
        const QString text = app.simulationConsole()->console()->toPlainText();
        QVERIFY2(!text.contains("Optimization"), qPrintable(text));
        QVERIFY2(!text.contains("incompatible"), qPrintable(text));
        QCOMPARE(values(find(doc, "Opt1"), "Var").value(0), QStringLiteral("Rx|yes|1000|10|100000|LOG_DOUBLE"));
    }
};

QTEST_MAIN(TestOptimization)
#include "test_optimization.moc"
