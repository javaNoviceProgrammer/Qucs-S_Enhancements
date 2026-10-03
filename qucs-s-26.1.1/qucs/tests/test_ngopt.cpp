/*
 * test_ngopt.cpp - NgOpt, ngspice's own optimize command as a component:
 * the command line its properties make, the lines that keep an in-place
 * knob across the netlist's resets, the results ngspice prints, the
 * component saved and loaded, the netlist, the dialog, the ERC, an ngspice
 * without the command - and, with an ngspice that has it, the shipped
 * example fitted and a divider whose instance knob outlives a reset.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRadioButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "ngoptimize.h"
#include "optimization.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "valuereading.h"
#include "components/component.h"
#include "components/ngopt_sim.h"
#include "components/ngoptdialog.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::ngopt;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

const QString kExample = QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/NGspice features/LC_lowpass_ngopt.sch");

bool write(const QString& file, const QString& text)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(text.toUtf8());
    return true;
}

QString read(const QString& file)
{
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

Component* find(Schematic* doc, const QString& name)
{
    for (Component* c : doc->a_DocComps)
        if (c->Name == name) return c;
    return nullptr;
}

// The example under another name in \a dir, its dataset and display after it.
QString copyExample(const QString& dir, const QString& base)
{
    const QString file = dir + "/" + base + ".sch";
    QString text = read(kExample);
    text.replace("LC_lowpass_ngopt.", base + ".");
    return write(file, text) ? file : QString();
}

// A voltage divider, 1 V over R1 = 1k and R2 = 1k, an AC analysis of one
// point and a transient of a 1 V sine, and the NgOpt properties given.
QString divider(const QString& file, const QString& ngopt)
{
    const QString base = QFileInfo(file).completeBaseName();
    return QStringLiteral(
               "<Qucs Schematic " PACKAGE_VERSION ">\n"
               "<Properties>\n  <View=0,0,800,600,1,0,0>\n  <Grid=10,10,1>\n"
               "  <DataSet=%2.dat>\n  <DataDisplay=%2.dpl>\n  <OpenDisplay=0>\n"
               "  <Script=%2.m>\n  <RunScript=0>\n  <showFrame=0>\n</Properties>\n"
               "<Symbol>\n</Symbol>\n<Components>\n"
               "  <Vac V1 1 100 200 18 -26 1 1 \"1 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0>\n"
               "  <GND * 1 100 260 0 0 0 0>\n"
               "  <R R1 1 200 140 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
               "  <R R2 1 300 200 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
               "  <GND * 1 300 260 0 0 0 0>\n"
               "  <.AC AC1 1 100 350 0 45 0 0 \"lin\" 1 \"1 kHz\" 1 \"1 kHz\" 1 \"1\" 1 \"no\" 0>\n"
               "  <.TR TR1 1 100 450 0 75 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"101\" 0 \"Trapezoidal\" 0 \"2\" 0 "
               "\"1 ns\" 0 \"1e-16\" 0 \"150\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"26.85\" 0 \"1e-3\" 0 "
               "\"1e-6\" 0 \"1\" 0 \"CroutLU\" 0 \"no\" 0 \"yes\" 0 \"0\" 0>\n"
               "  <.NGOPT NgOpt1 1 500 350 0 44 0 0 %1>\n"
               "</Components>\n<Wires>\n"
               "  <100 230 100 260 \"\" 0 0 0 \"\">\n"
               "  <100 140 100 170 \"\" 0 0 0 \"\">\n"
               "  <100 140 170 140 \"\" 0 0 0 \"\">\n"
               "  <230 140 300 140 \"out\" 260 110 0 \"\">\n"
               "  <300 140 300 170 \"\" 0 0 0 \"\">\n"
               "  <300 230 300 260 \"\" 0 0 0 \"\">\n"
               "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
        .arg(ngopt, base);
}

// ngspice from PATH if it has the optimize command, else empty.
QString optimizingNgspice(const QString& dir)
{
    const QString exe = QStandardPaths::findExecutable("ngspice");
    if (exe.isEmpty()) return QString();
    const QString deck = dir + "/probe.cir";
    if (!write(deck, "* probe\nR1 1 0 1\nV1 1 0 1\n.control\nhelp optimize\n.endc\n.end\n")) return QString();
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, {"-b", deck});
    if (!p.waitForFinished(20000)) return QString();
    return QString::fromUtf8(p.readAll()).contains("parameter optimizer") ? exe : QString();
}

// \a ngspice if its optimize has the methods of the 2026-09-29 proposal
// (cmaes, bayes, tr) and constraints, else empty.
QString newMethodsNgspice(const QString& dir, const QString& ngspice)
{
    if (ngspice.isEmpty()) return QString();
    const QString deck = dir + "/methods.cir";
    if (!write(deck, "* methods\nV1 1 0 1\nR1 1 2 1k\nR2 2 0 1k\n.control\n"
                     "optimize -param r2 1k 100 10k -analysis op -minimize abs(v(2)-0.4) -constrain v(2) -min 0.1 "
                     "-method tr -maxiter 2\n.endc\n.end\n"))
        return QString();
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(ngspice, {"-b", deck});
    if (!p.waitForFinished(20000)) return QString();
    const QString out = QString::fromUtf8(p.readAll());
    return out.contains("(Trust-Region)") && out.contains("optimize: constraint v(2) >= 0.1") ? ngspice : QString();
}

// What ngspice (Ngspice_OpenVAF_Enhancements) prints for two optimize runs.
const char* kOutput =
    "Circuit: * qucs\n"
    "optimize: 3 parameters, 3 targets over 3 analysis stages, Levenberg-Marquardt\n"
    "Doing analysis at TEMP = 27.000000 and TNOM = 27.000000\n"
    "optimize: converged, sum-sq residual = 1.73152e-09 (rms 2.40244e-05) after 39 evaluations\n"
    "    cin = 3.18306e-09\n"
    "    lm = 1.59156e-05\n"
    "    cout = 3.18306e-09\n"
    "\n"
    "Doing analysis at TEMP = 27.000000 and TNOM = 27.000000\n"
    "optimize: 1 parameter, analysis 'op', minimizing '(abs(i(vd))-1m)^2' (Nelder-Mead)\n"
    "optimize: INTERRUPTED -- best point so far, objective = 1.47869e-13 after 29 evaluations\n"
    "optimize: NOTE -- 1 of 1 parameter finished ON a search bound; the optimum may lie outside the range given.\n"
    "    @dm[is] = 1.21927e-14\n"
    "No. of Data Rows : 41\n";

// What the enhanced ngspice prints since E-762 (why a search stopped),
// E-764 (-starts, -polish) and E-766 (constraints), taken from its runs: a
// CMA-ES fit from three starts, polished, with a constraint; a Bayesian
// search stopped at its budget; an annealing's schedule run out; a step
// of a search that printed no end, then another search; one infeasible,
// one where nothing solved, one whose objective never moved.
const char* kNewOutput =
    "optimize: 1 parameter, 1 target over 2 analysis stages, CMA-ES\n"
    "optimize: CMA-ES population of 4 (recombining 2), seed 3, up to 100 generations\n"
    "optimize: 3 starts -- the given point and 2 Latin-hypercube points, 34 iterations each, the winner polished\n"
    "optimize: 1 constraint (augmented Lagrangian around CMA-ES, feasible within 0.0001 of each bound): i(v1) <= -9e-06\n"
    "optimize: constraints round 1 -- objective 4.58617e-10, largest violation 0 (feasible), penalty 1.81e+05, maxiter after 138 evaluations\n"
    "optimize: start 1 of 3 (the given point, population 4) -- maxiter, cost 4.58617e-10 after 138 evaluations\n"
    "optimize: start 3 of 3 (Latin-hypercube point, population 16) -- converged, cost 2.13447e-12 after 354 evaluations\n"
    "optimize: start 3 of 3 won (cost 2.13447e-12)\n"
    "optimize: polish -- Levenberg-Marquardt from the best point (cost 2.13447e-12)\n"
    "optimize: constraints round 4 -- objective 1.37402e-18, largest violation 0 (feasible), penalty 1.81e+05, converged after 771 evaluations\n"
    "optimize: polish converged -- cost 2.13447e-12 -> 1.37402e-18 in 5 evaluations\n"
    "Reset re-loads circuit * rc\n"
    "Circuit: * rc\n"
    "optimize: converged, sum-sq residual = 1.37402e-18 (rms 1.17219e-09) after 772 evaluations\n"
    "optimize: constraint i(v1) <= -9e-06 -- -9.90197e-06, slack 9.02e-07\n"
    "    r = 990.001\n"
    "optimize: 1 parameter, analysis 'ac lin 1 159.155k 159.155k', minimizing 'abs(vdb(out)+3)' (Bayesian optimization)\n"
    "optimize: surrogate -- predicted cost 1.69395 (1.69 .. 1.7, one sigma) at the optimum after 15 evaluations, fitted to log cost; length scales (box widths): r 0.0666\n"
    "optimize: polish -- Trust-Region from the best point (cost 1.69381), up to 25 iterations (the remaining budget)\n"
    "optimize: polish stopped at -maxiter (25 iterations) -- NOT converged -- cost 1.69381 -> 0.999809 in 29 evaluations\n"
    "optimize: stopped at -maxiter (25 iterations) -- NOT converged, objective = 0.999809 after 47 evaluations\n"
    "optimize: NOTE -- the iteration cap ended the search before its own criterion did; raise -maxiter, or loosen -tol if the reported value is close enough.\n"
    "optimize: constraint vdb(out) >= -2 -- -2.00019, active (multiplier 1.911: raising the bound raises the objective by about that much per unit)\n"
    "    r = 754.857\n"
    "optimize: 1 parameter, analysis 'ac lin 1 159.155k 159.155k', minimizing 'abs(vdb(out)+3)' (Simulated Annealing)\n"
    "optimize: annealing, seed 2, 20 cooling levels\n"
    "optimize: cooling schedule complete (20 levels), objective = 0.0233052 after 254 evaluations\n"
    "    r = 982.263\n"
    "No. of Data Rows : 1\n"
    "optimize: start 2 of 2 won (cost 1)\n"
    "optimize: 1 parameter, analysis 'op', minimizing 'abs(i(v1))' (Nelder-Mead)\n"
    "optimize: converged, objective = 0.000333333 after 5 evaluations\n"
    "    r2 = 2000\n"
    "optimize: 1 parameter, analysis 'ac lin 1 159.155k 159.155k', minimizing 'abs(vdb(out)+3)' (Nelder-Mead)\n"
    "optimize: INFEASIBLE -- vdb(out) >= 1 missed by 1.05 after 10 rounds, objective = 2.94819 after 430 evaluations\n"
    "optimize: constraint vdb(out) >= 1 -- -0.0518094, VIOLATED by 1.05\n"
    "optimize: NOTE -- 1 of 1 parameter finished ON a search bound; the optimum may lie outside the range given.\n"
    "    r = 100\n"
    "optimize: 1 parameter, 1 target over 1 analysis stage, Levenberg-Marquardt\n"
    "optimize: NO SOLUTION -- no evaluation solved, sum-sq residual = 1e+30 (rms 1e+15) after 15 evaluations\n"
    "    r2 = 2000\n"
    "optimize: unchanged -- nothing was optimised, objective = 0.5 after 3 evaluations\n"
    "optimize: NOTE -- the objective was 0.5 at every one of the 3 evaluations, so nothing was optimised and the reported value is the starting value; check that the objective actually depends on the parameter.\n"
    "    r9 = 1000\n";

} // namespace

class TestNgOpt : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString optimizing;   // an ngspice with optimize, if there is one

    Schematic* simulate(QucsApp& app, const QString& file)
    {
        if (!app.gotoPage(file)) return nullptr;
        Schematic* doc = app.currentSchematic();
        if (doc == nullptr || !QMetaObject::invokeMethod(&app, "slotSimulate")) return nullptr;
        QTest::qWait(50);
        QElapsedTimer t;
        t.start();
        while (app.simulationConsole()->isRunning() && t.elapsed() < 120000) QTest::qWait(50);
        return doc;
    }

    static QString logText(QucsApp& app)
    {
        QStringList lines;
        const QListWidget* log = app.simulationConsole()->statusLog();
        for (int i = 0; i < log->count(); ++i) lines << log->item(i)->text();
        return lines.join('\n');
    }

    // The status log's entry that begins with \a start has the tick of a
    // run that went well (not a warning).
    static bool ticked(QucsApp& app, const QString& start)
    {
        const QListWidget* log = app.simulationConsole()->statusLog();
        const QImage tick = QIcon(":/bitmaps/svg/ok_apply.svg").pixmap(16).toImage();
        for (int i = 0; i < log->count(); ++i)
            if (log->item(i)->text().startsWith(start)) return log->item(i)->icon().pixmap(16).toImage() == tick;
        return false;
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
        optimizing = optimizingNgspice(dir.path());
    }

    // What a set of properties makes of ngspice's command.
    void theCommandLine()
    {
        Command c;
        c.method = "nm";
        c.maxIter = "50";
        c.knobs << Knob{KnobKind::Instance, "R1", "1k", "100", "10 kOhm"}
                << Knob{KnobKind::Param, "Cx", "2.2n", "470p", "22n"}
                << Knob{KnobKind::Model, "@dm[is]", "1e-15", "1e-16", "1e-12"};
        c.analysis = "ac lin 1 1meg 1meg";
        c.minimize = "(mag(v(out)) - 0.5)^2";
        QString line, why;
        QVERIFY2(commandLine(c, nullptr, &line, &why), qPrintable(why));
        QCOMPARE(line, QStringLiteral("optimize -param R1 1000 100 10000 -dparam Cx 2.2e-09 4.7e-10 2.2e-08 "
                                      "-mparam @dm[is] 1e-15 1e-16 1e-12 -analysis ac lin 1 1meg 1meg "
                                      "-minimize (mag(v(out))-0.5)^2 -method nm -maxiter 50"));

        // Targets: a stage for each analysis, in the order they come.
        c.minimize.clear();
        c.method = "lm";
        c.maxIter.clear();
        c.seed = "3";
        c.verbose = true;
        c.knobs = {Knob{KnobKind::Param, "Cx", "2.2n", "470p", "22n"}};
        c.targets << Target{"op", "v(out)", "0.4", ""} << Target{"ac lin 1 2k 2k", "mag(v(out))", "0.22", "1/0.22"}
                  << Target{"op", "i(v1)", "-0.2m", "2"};
        QVERIFY(!commandLine(c, nullptr, &line, &why));   // 1/0.22 is not a number
        QVERIFY2(why.contains("weight"), qPrintable(why));
        c.targets[1].weight = "4.5";
        QVERIFY2(commandLine(c, nullptr, &line, &why), qPrintable(why));
        QCOMPARE(line, QStringLiteral("optimize -dparam Cx 2.2e-09 4.7e-10 2.2e-08 -analysis op -target v(out) 0.4 "
                                      "-target i(v1) -0.0002 2 -analysis ac lin 1 2k 2k -target mag(v(out)) 0.22 4.5 "
                                      "-method lm -seed 3 -verbose"));

        // What cannot be written.
        Command bad = c;
        bad.knobs.clear();
        QVERIFY(!commandLine(bad, nullptr, &line, &why));
        QVERIFY(why.contains("no parameter"));
        bad = c;
        bad.knobs[0].init = "Rload";
        QVERIFY(!commandLine(bad, nullptr, &line, &why));
        QVERIFY(why.contains("initial value"));
        bad = c;
        bad.knobs[0].hi = "100p";
        QVERIFY(!commandLine(bad, nullptr, &line, &why));
        QVERIFY(why.contains("above the minimum"));
        bad = c;
        bad.targets.clear();
        QVERIFY(!commandLine(bad, nullptr, &line, &why));
        QVERIFY(why.contains("nothing to minimize"));
        bad = c;
        bad.minimize = "v(out)";
        bad.analysis = "op";
        QVERIFY(!commandLine(bad, nullptr, &line, &why));   // lm fits targets
        QVERIFY(why.contains("Levenberg"));
        bad = c;
        for (int i = 0; i < 8; ++i) bad.targets << Target{QString("ac lin 1 %1k %1k").arg(i + 1), "v(out)", "1", ""};
        QVERIFY(!commandLine(bad, nullptr, &line, &why));
        QVERIFY(why.contains("8 analyses"));
        bad = c;
        bad.maxIter = "2.5";
        QVERIFY(!commandLine(bad, nullptr, &line, &why));

        QStringList ids;
        for (const auto& m : methods()) ids << m.first;
        QCOMPARE(ids, QStringList({"de", "pso", "sa", "cmaes", "bayes", "nm", "tr", "lm"}));
        for (const QString& id : ids) QVERIFY2(!methodNote(id).isEmpty(), qPrintable(id));
        QVERIFY(methodNote("nsga2").isEmpty());
        QVERIFY(isGlobal("cmaes") && isGlobal("bayes") && isGlobal("sa") && !isGlobal("tr") && !isGlobal("nm"));
        QVERIFY(takesPopulation("cmaes") && takesPopulation("de") && !takesPopulation("bayes") && !takesPopulation("sa"));
    }

    // The methods and options since the 2026-09-29 proposal: CMA-ES,
    // Bayesian optimization, the trust region, -polish, -starts, and
    // constraints after the analysis each is measured on.
    void theNewMethodsAndOptions()
    {
        Command c;
        c.knobs << Knob{KnobKind::Param, "Rb", "10k", "1k", "1M"};
        c.analysis = "op";
        c.minimize = "abs(i(vdd))";
        c.method = "cmaes";
        c.size = "8";
        c.seed = "2";
        c.polish = true;
        c.starts = "3";
        c.constraints << Constraint{"", "v(out)", "0.9", ""} << Constraint{"op", "v(out) - v(in)", "", "-0.05"};
        c.ctol = "1e-5";
        QString line, why;
        QVERIFY2(commandLine(c, nullptr, &line, &why), qPrintable(why));
        QCOMPARE(line, QStringLiteral("optimize -dparam Rb 10000 1000 1000000 -analysis op -minimize abs(i(vdd)) "
                                      "-constrain v(out) -min 0.9 -constrain v(out)-v(in) -max -0.05 -method cmaes "
                                      "-swarmsize 8 -seed 2 -polish -starts 3 -ctol 1e-5"));
        for (const char* method : {"bayes", "tr", "sa", "pso", "nm"}) {
            Command m = c;
            m.method = method;
            QVERIFY2(commandLine(m, nullptr, &line, &why), qPrintable(why));
            QVERIFY2(line.contains(QStringLiteral("-method %1 ").arg(method)), qPrintable(line));
        }
        // An expression that begins with a minus, in parentheses: ngspice
        // would take it for an option.
        Command minus = c;
        minus.minimize = "-i(vdd)";
        minus.constraints = {Constraint{"", "-v(out)", "", "-0.9"}};
        QVERIFY2(commandLine(minus, nullptr, &line, &why), qPrintable(why));
        QVERIFY2(line.contains("-minimize (-i(vdd)) -constrain (-v(out)) -max -0.9 "), qPrintable(line));
        // A tolerance only with constraints; none, ngspice's.
        Command plain = c;
        plain.constraints.clear();
        QVERIFY(commandLine(plain, nullptr, &line, &why));
        QVERIFY2(!line.contains("-ctol") && !line.contains("-constrain"), qPrintable(line));
        plain.polish = false;
        plain.starts.clear();
        QVERIFY(commandLine(plain, nullptr, &line, &why));
        QVERIFY2(!line.contains("-polish") && !line.contains("-starts"), qPrintable(line));

        // Fitting: a constraint after its analysis's targets; one of an
        // analysis no target has, a stage of its own; one of none, the
        // first target's.
        Command fit;
        fit.method = "lm";
        fit.knobs = {Knob{KnobKind::Param, "R", "500", "100", "10k"}};
        fit.targets << Target{"ac lin 1 159k 159k", "vdb(out)", "-3", ""} << Target{"op", "v(out)", "0.5", ""};
        fit.constraints << Constraint{"op", "i(v1)", "", "-9u"} << Constraint{"tran 1u 10u", "vecmax(v(out))", "0.4", "0.6"}
                        << Constraint{"", "vdb(out)", "-3.5", ""};
        QVERIFY2(commandLine(fit, nullptr, &line, &why), qPrintable(why));
        QCOMPARE(line, QStringLiteral("optimize -dparam R 500 100 10000 -analysis ac lin 1 159k 159k -target vdb(out) -3 "
                                      "-constrain vdb(out) -min -3.5 -analysis op -target v(out) 0.5 -constrain i(v1) -max -9e-06 "
                                      "-analysis tran 1u 10u -constrain vecmax(v(out)) -min 0.4 -max 0.6 -method lm"));

        // What cannot be written.
        const auto refused = [&](const Command& bad, const char* says) {
            QString l, w;
            const bool ok = commandLine(bad, nullptr, &l, &w);
            return !ok && w.contains(QLatin1String(says)) ? QString() : QStringLiteral("%1 / %2").arg(l, w);
        };
        Command bad = c;
        bad.method = "cma";
        QCOMPARE(refused(bad, "is not one of de, pso, sa, cmaes, bayes, nm, tr, lm"), QString());
        bad.method = "nsga2";
        QCOMPARE(refused(bad, "Pareto front"), QString());
        bad = c;
        bad.starts = "0";
        QCOMPARE(refused(bad, "starts"), QString());
        bad.starts = "1.5";
        QCOMPARE(refused(bad, "starts"), QString());
        bad = c;
        bad.ctol = "0";
        QCOMPARE(refused(bad, "constraint tolerance"), QString());
        bad = c;
        bad.constraints = {Constraint{"", "v(out)", "", ""}};
        QCOMPARE(refused(bad, "neither a minimum nor a maximum"), QString());
        bad.constraints = {Constraint{"", "v(out)", "1", "0.5"}};
        QCOMPARE(refused(bad, "below its minimum"), QString());
        bad.constraints = {Constraint{"", "v(out)", "0.9V", ""}};
        QVERIFY(commandLine(bad, nullptr, &line, &why));   // a unit is a number
        bad.constraints = {Constraint{"", "v(out)", "high", ""}};
        QCOMPARE(refused(bad, "minimum \"high\" is not a number"), QString());
        bad.constraints = {Constraint{"", "v(out)", "", "low"}};
        QCOMPARE(refused(bad, "maximum \"low\" is not a number"), QString());
        bad.constraints = {Constraint{"", " ", "1", ""}};
        QCOMPARE(refused(bad, "no expression"), QString());
        // An expression to minimize has one analysis.
        bad.constraints = {Constraint{"ac lin 1 1k 1k", "vdb(out)", "-3", ""}};
        QCOMPARE(refused(bad, "has one analysis"), QString());
        bad.constraints.clear();
        for (int i = 0; i < 33; ++i) bad.constraints << Constraint{"", "v(out)", QString::number(i), ""};
        QCOMPARE(refused(bad, "more than 32 constraints"), QString());
        // The stages of constraints count to ngspice's eight.
        Command stages = fit;
        stages.constraints.clear();
        for (int i = 0; i < 7; ++i) stages.constraints << Constraint{QString("ac lin 1 %1k %1k").arg(i + 1), "v(out)", "0", ""};
        QCOMPARE(refused(stages, "8 analyses"), QString());
    }

    // ngspice keeps a .param the optimizer changed across a reset, not an
    // alter or an altermod.
    void inPlaceKnobsAreCarriedAcrossResets()
    {
        Command c;
        c.knobs << Knob{KnobKind::Param, "Cx", "1", "0", "2"} << Knob{KnobKind::Instance, "R3", "1", "0", "2"}
                << Knob{KnobKind::Model, "@dm[is]", "1", "0", "2"};
        QCOMPARE(carryLines(c, 2), QStringLiteral("set ngopt_2_2 = \"$&optimize_r3\"\n"
                                                  "set ngopt_2_3 = \"$&optimize_@dm[is]\"\n"));
        QCOMPARE(reapplyLines(c, 2), QStringLiteral("alter r3 = $ngopt_2_2\n"
                                                    "altermod @dm[is] = $ngopt_2_3\n"));
        c.knobs.removeLast();
        c.knobs.removeLast();
        QVERIFY(carryLines(c, 1).isEmpty());
        QVERIFY(reapplyLines(c, 1).isEmpty());
    }

    void theResultsAreRead()
    {
        const QList<Result> results = parseResults(QString::fromUtf8(kOutput));
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.at(0).summary,
                 QStringLiteral("converged, sum-sq residual = 1.73152e-09 (rms 2.40244e-05) after 39 evaluations"));
        QVERIFY(!results.at(0).interrupted);
        QVERIFY(results.at(0).notes.isEmpty());
        QCOMPARE(results.at(0).values.size(), 3);
        QCOMPARE(results.at(0).values.at(1).first, QStringLiteral("lm"));
        QCOMPARE(results.at(0).values.at(1).second, 1.59156e-05);
        QVERIFY(results.at(1).interrupted);
        QCOMPARE(results.at(1).notes.size(), 1);
        QVERIFY(results.at(1).notes.first().startsWith("1 of 1 parameter finished ON a search bound"));
        QCOMPARE(results.at(1).values.size(), 1);
        QCOMPARE(results.at(1).values.first().first, QStringLiteral("@dm[is]"));
        QVERIFY(!unsupported(QString::fromUtf8(kOutput)));
        QVERIFY(unsupported("optimize: no such command available in ngspice\n"));
        QVERIFY(parseResults("optimize: no such command available in ngspice\n").isEmpty());
        QCOMPARE(results.at(0).status, QStringLiteral("converged"));
        QCOMPARE(results.at(1).status, QStringLiteral("interrupted"));
        QVERIFY(results.at(0).settled() && !results.at(1).settled());
    }

    // Why each search stopped (every one used to read "converged", and
    // the rest went unread: no optimum), the constraints at its end - the
    // values after them still read - and the start that won and the
    // polish, each with the search it belongs to.
    void theNewResultsAreRead()
    {
        const QList<Result> r = parseResults(QString::fromUtf8(kNewOutput));
        QCOMPARE(r.size(), 7);
        QStringList statuses;
        for (const Result& one : r) statuses << one.status;
        QCOMPARE(statuses, QStringList({"converged", "maxiter", "completed", "converged", "infeasible", "nosolve", "unchanged"}));
        QList<bool> settled;
        for (const Result& one : r) settled << one.settled();
        QCOMPARE(settled, QList<bool>({true, false, true, true, false, false, false}));
        for (const Result& one : r) {
            QCOMPARE(one.values.size(), 1);
            QVERIFY(!one.interrupted);
        }

        QCOMPARE(r.at(0).summary, QStringLiteral("converged, sum-sq residual = 1.37402e-18 (rms 1.17219e-09) after 772 evaluations"));
        QCOMPARE(r.at(0).search, QStringList({"start 3 of 3 won (cost 2.13447e-12)",
                                              "polish converged -- cost 2.13447e-12 -> 1.37402e-18 in 5 evaluations"}));
        QCOMPARE(r.at(0).constraints, QStringList({"i(v1) <= -9e-06 -- -9.90197e-06, slack 9.02e-07"}));
        QCOMPARE(r.at(0).values.first(), qMakePair(QStringLiteral("r"), 990.001));
        QVERIFY(r.at(0).notes.isEmpty());

        QVERIFY(r.at(1).summary.startsWith("stopped at -maxiter (25 iterations) -- NOT converged, objective = 0.999809"));
        QCOMPARE(r.at(1).search, QStringList({"polish stopped at -maxiter (25 iterations) -- NOT converged -- cost 1.69381 -> "
                                              "0.999809 in 29 evaluations"}));
        QCOMPARE(r.at(1).notes.size(), 1);
        QVERIFY(r.at(1).constraints.first().contains("active (multiplier 1.911"));
        QCOMPARE(r.at(1).values.first().second, 754.857);

        QVERIFY(r.at(2).summary.startsWith("cooling schedule complete (20 levels)"));
        QVERIFY(r.at(2).search.isEmpty() && r.at(2).constraints.isEmpty());
        QVERIFY2(r.at(3).search.isEmpty(), qPrintable(r.at(3).search.join('|')));   // not the step before its first line
        QCOMPARE(r.at(3).values.first().first, QStringLiteral("r2"));

        QVERIFY(r.at(4).summary.startsWith("INFEASIBLE -- vdb(out) >= 1 missed by 1.05"));
        QCOMPARE(r.at(4).constraints, QStringList({"vdb(out) >= 1 -- -0.0518094, VIOLATED by 1.05"}));
        QCOMPARE(r.at(4).notes.size(), 1);
        QCOMPARE(r.at(4).values.first().second, 100.0);
        QVERIFY(r.at(5).summary.startsWith("NO SOLUTION"));
        QVERIFY(r.at(6).summary.startsWith("unchanged -- nothing was optimised"));
        QVERIFY(r.at(6).notes.first().startsWith("the objective was 0.5 at every one"));
    }

    // Saved with the names of its properties, so the knobs and targets come
    // back however many there are; the factory knows the model.
    void theComponentIsSavedAndLoaded()
    {
        const QString file = copyExample(dir.path(), "saved");
        QVERIFY(!file.isEmpty());
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        Component* ngopt = find(&doc, "NgOpt1");
        QVERIFY(ngopt != nullptr);
        QVERIFY(dynamic_cast<NgOpt_Sim*>(ngopt) != nullptr);
        QCOMPARE(ngopt->Simulator, int(spicecompat::simNgspice));
        Command c = Command::read(ngopt);
        QCOMPARE(c.method, QStringLiteral("lm"));
        QVERIFY(c.leastSquares());
        QCOMPARE(c.knobs.size(), 3);
        QCOMPARE(c.knobs.at(1).toString(), QStringLiteral("dparam|Lm|4.7u|1u|47u"));
        QCOMPARE(c.targets.size(), 3);
        QCOMPARE(c.targets.at(2).analysis, QStringLiteral("ac lin 1 2meg 2meg"));

        // Two more knobs, one target less: written, saved, loaded.
        c.knobs << Knob{KnobKind::Instance, "Rs", "50", "10", "100"} << Knob{KnobKind::Model, "@x[y]", "1", "0", "2"};
        c.targets.removeLast();
        c.seed = "7";
        c.write(ngopt);
        QVERIFY(doc.save() >= 0);
        Schematic again(nullptr, file);
        QVERIFY(again.loadDocument());
        const Command back = Command::read(find(&again, "NgOpt1"));
        QCOMPARE(back.knobs.size(), 5);
        QCOMPARE(back.knobs.at(4).toString(), QStringLiteral("mparam|@x[y]|1|0|2"));
        QCOMPARE(back.targets.size(), 2);
        QCOMPARE(back.seed, QStringLiteral("7"));
        // The schematic shows the method, the knobs and the targets.
        for (const Property* p : find(&again, "NgOpt1")->Props)
            if (p->Name == "Knob" || p->Name == "Target" || p->Name == "Method") QVERIFY2(p->display, qPrintable(p->Name));
    }

    // A block saved before Polish, Starts, CTol and Constraint: loaded,
    // it has them as a new one does - so they can be set by name - and
    // its values and what it shows stay; saved, they come back.
    void anOlderBlockGainsTheNewProperties()
    {
        const QString file = copyExample(dir.path(), "older");
        QVERIFY(read(file).contains("\"Minimize=\" 0 \"Knob=dparam|Cin"));   // the eight of before
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        Component* ngopt = find(&doc, "NgOpt1");
        QStringList names;
        for (const Property* p : ngopt->Props) names << p->Name;
        QCOMPARE(names, QStringList({"Method", "MaxIter", "Tol", "Size", "Seed", "Verbose", "Analysis", "Minimize", "Polish",
                                     "Starts", "CTol", "Knob", "Knob", "Knob", "Target", "Target", "Target"}));
        QCOMPARE(ngopt->getProperty("Polish")->Value, QStringLiteral("no"));
        QVERIFY(!ngopt->getProperty("Polish")->display);
        QVERIFY(ngopt->getProperty("Method")->display);
        QVERIFY(!ngopt->getProperty("Minimize")->display);   // as the file has it
        QCOMPARE(Command::read(ngopt).knobs.at(2).toString(), QStringLiteral("dparam|Cout|2.2n|470p|22n"));

        Command c = Command::read(ngopt);
        c.method = "bayes";
        c.polish = true;
        c.starts = "2";
        c.ctol = "1e-3";
        c.constraints << Constraint{"ac lin 1 1meg 1meg", "db(2*v(out))", "-3.2", "-2.8"};
        c.write(ngopt);
        QVERIFY(doc.save() >= 0);
        QVERIFY(read(file).contains("\"Constraint=ac lin 1 1meg 1meg|db(2*v(out))|-3.2|-2.8\" 1"));
        Schematic again(nullptr, file);
        QVERIFY(again.loadDocument());
        const Command back = Command::read(find(&again, "NgOpt1"));
        QCOMPARE(back.method, QStringLiteral("bayes"));
        QVERIFY(back.polish);
        QCOMPARE(back.starts, QStringLiteral("2"));
        QCOMPARE(back.ctol, QStringLiteral("1e-3"));
        QCOMPARE(back.constraints.size(), 1);
        QCOMPARE(back.constraints.first().max, QStringLiteral("-2.8"));
        QString line;
        QVERIFY(commandLine(back, &again, &line));
        QVERIFY2(line.contains("-analysis ac lin 1 1meg 1meg -target db(2*v(out)) -3.0103 -constrain db(2*v(out)) -min -3.2 -max -2.8 "),
                 qPrintable(line));
        QVERIFY2(line.endsWith("-method bayes -polish -starts 2 -ctol 1e-3"), qPrintable(line));
    }

    // The optimize line ahead of the simulations; an in-place knob set
    // again after each reset; a component the netlist cannot write says so.
    void theNetlist()
    {
        const QString file = dir.filePath("net.sch");
        QVERIFY(write(file, divider(file, "\"Method=nm\" 1 \"MaxIter=\" 0 \"Tol=\" 0 \"Size=\" 0 \"Seed=\" 0 "
                                          "\"Verbose=no\" 0 \"Analysis=AC1\" 0 \"Minimize=(mag(v(out))-0.25)^2\" 1 "
                                          "\"Knob=param|R2|1k|10|100k\" 1")));
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        QCOMPARE(analysisCommand(&doc, "ac1"), QStringLiteral("ac lin 1 1k 1k"));
        QCOMPARE(analysisCommand(&doc, "op"), QStringLiteral("op"));
        const QString netlist = dir.filePath("net.cir");
        {
            Ngspice kernel(&doc);
            kernel.SaveNetlist(netlist, false);
            QCOMPARE(kernel.optimizations(), QStringList({"NgOpt1"}));
        }
        const QString text = read(netlist);
        const int control = text.indexOf(".control");
        const int optimize = text.indexOf("optimize -param R2 1000 10 100000 -analysis ac lin 1 1k 1k "
                                          "-minimize (mag(v(out))-0.25)^2 -method nm");
        const int carry = text.indexOf("set ngopt_1_1 = \"$&optimize_r2\"");
        const int ac = text.indexOf("\nac lin 1 1k 1k");
        const int tran = text.indexOf("\ntran ");
        QVERIFY2(control > 0 && optimize > control && carry > optimize && ac > carry && tran > ac, qPrintable(text));
        QCOMPARE(text.count("reset\nalter r2 = $ngopt_1_1\n"), 2);   // after each simulation

        // A component the netlist cannot write: an error in the output.
        find(&doc, "NgOpt1")->Props.at(7)->Value.clear();   // Minimize: nothing to minimize or fit
        {
            Ngspice kernel(&doc);
            kernel.SaveNetlist(netlist, false);
            QVERIFY(kernel.optimizations().isEmpty());
        }
        const QString refused = read(netlist);
        QVERIFY2(refused.contains("echo \"Error: NgOpt1: nothing to minimize and no target to fit\""), qPrintable(refused));
        QVERIFY(!refused.contains("optimize -"));
        QVERIFY(!refused.contains("alter r2"));
        // The ERC says so beforehand.
        QStringList messages;
        for (const auto& issue : qucs_s::erc::check(&doc))
            if (issue.component == "NgOpt1") messages << issue.message;
        QCOMPARE(messages, QStringList({"NgOpt1: nothing to minimize and no target to fit"}));

        // Inactive: nothing at all.
        find(&doc, "NgOpt1")->isActive = COMP_IS_OPEN;
        {
            Ngspice kernel(&doc);
            kernel.SaveNetlist(netlist, false);
        }
        QVERIFY(!read(netlist).contains("NgOpt1"));
    }

    // The form: what it shows of the component, the command it makes, and
    // what it writes back.
    void theDialog()
    {
        const QString file = copyExample(dir.path(), "dialog");
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        Component* ngopt = find(&doc, "NgOpt1");
        NgOptDialog dialog(ngopt, &doc);
        QCOMPARE(dialog.knobTable()->rowCount(), 3);
        QCOMPARE(dialog.targetTable()->rowCount(), 3);
        QVERIFY(dialog.fitButton()->isChecked());
        QCOMPARE(dialog.methodCombo()->currentData().toString(), QStringLiteral("lm"));
        QString line;
        QVERIFY(commandLine(Command::read(ngopt), &doc, &line));
        QCOMPARE(dialog.preview(), line);

        // Every method, each with its note; what each takes.
        QCOMPARE(dialog.methodCombo()->count(), 8);
        const auto choose = [&](const char* method) {
            dialog.methodCombo()->setCurrentIndex(dialog.methodCombo()->findData(method));
            return dialog.methodCombo()->currentData().toString() == QLatin1String(method);
        };
        QVERIFY(choose("bayes"));
        QVERIFY2(dialog.methodNoteLabel()->text().contains("Gaussian-process"), qPrintable(dialog.methodNoteLabel()->text()));
        QVERIFY(!dialog.sizeEdit()->isEnabled() && dialog.seedEdit()->isEnabled() && dialog.polishCheck()->isEnabled());
        QVERIFY(choose("tr"));
        QVERIFY(!dialog.sizeEdit()->isEnabled() && !dialog.seedEdit()->isEnabled() && !dialog.polishCheck()->isEnabled());
        dialog.startsEdit()->setText("2");   // the starts are drawn at random
        QVERIFY(dialog.seedEdit()->isEnabled());
        QVERIFY2(dialog.preview().endsWith("-method tr -starts 2"), qPrintable(dialog.preview()));
        dialog.polishCheck()->setChecked(true);   // a local method has nothing to polish
        QVERIFY2(!dialog.preview().contains("-polish"), qPrintable(dialog.preview()));
        dialog.startsEdit()->clear();
        QVERIFY(choose("cmaes"));
        QVERIFY(dialog.sizeEdit()->isEnabled() && dialog.polishCheck()->isEnabled());
        QVERIFY(dialog.sizeEdit()->placeholderText().contains("3 ln"));
        QVERIFY2(dialog.preview().endsWith("-method cmaes -polish"), qPrintable(dialog.preview()));
        dialog.polishCheck()->setChecked(false);

        // Differential evolution with a population and a seed.
        QVERIFY(choose("de"));
        dialog.sizeEdit()->setText("24");
        dialog.seedEdit()->setText("5");
        QVERIFY2(dialog.preview().endsWith("-method de -swarmsize 24 -seed 5"), qPrintable(dialog.preview()));

        // A constraint, after the first target's analysis.
        dialog.addConstraint(Constraint{"", "db(2*v(out))", "-3.5", ""});
        QCOMPARE(dialog.constraintTable()->rowCount(), 1);
        dialog.ctolEdit()->setText("1e-3");
        QVERIFY2(dialog.preview().contains("-target db(2*v(out)) -0.0673 -constrain db(2*v(out)) -min -3.5 -analysis "),
                 qPrintable(dialog.preview()));
        QVERIFY2(dialog.preview().endsWith("-swarmsize 24 -seed 5 -ctol 1e-3"), qPrintable(dialog.preview()));
        dialog.constraintTable()->item(0, 2)->setText("");
        QVERIFY2(dialog.preview().contains("neither a minimum nor a maximum"), qPrintable(dialog.preview()));
        dialog.constraintTable()->item(0, 3)->setText("-2.5");

        // Minimizing after AC1, a simulation component of the schematic.
        dialog.minimizeButton()->setChecked(true);
        QVERIFY(dialog.preview().startsWith("(not complete"));
        dialog.analysisCombo()->setEditText("AC1");
        dialog.minimizeEdit()->setText("vecmax(db(2*v(out))[30,40])");
        QVERIFY2(dialog.preview().contains("-analysis ac dec 20 100k 10meg -minimize vecmax(db(2*v(out))[30,40])"),
                 qPrintable(dialog.preview()));

        // The equations' numbers as .param knobs - the three are there.
        dialog.addEquationVariables();
        QCOMPARE(dialog.knobTable()->rowCount(), 3);
        dialog.knobTable()->removeRow(2);
        dialog.addEquationVariables();
        QCOMPARE(dialog.knobTable()->rowCount(), 3);
        QCOMPARE(dialog.knobTable()->item(2, 1)->text(), QStringLiteral("Cout"));
        QCOMPARE(dialog.knobTable()->item(2, 3)->text(), QStringLiteral("220p"));
        QCOMPARE(dialog.knobTable()->item(2, 4)->text(), QStringLiteral("22n"));

        dialog.nameEdit()->setText("Filter");
        QMetaObject::invokeMethod(&dialog, "slotOK");
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(ngopt->Name, QStringLiteral("Filter"));
        const Command c = Command::read(ngopt);
        QCOMPARE(c.method, QStringLiteral("de"));
        QCOMPARE(c.size, QStringLiteral("24"));
        QCOMPARE(c.minimize, QStringLiteral("vecmax(db(2*v(out))[30,40])"));
        QCOMPARE(c.analysis, QStringLiteral("AC1"));
        QCOMPARE(c.targets.size(), 3);   // kept for the fit
        QCOMPARE(c.knobs.at(2).toString(), QStringLiteral("dparam|Cout|2.2n|220p|22n"));
        QCOMPARE(c.constraints.size(), 1);
        QCOMPARE(c.constraints.first().toString(), QStringLiteral("|db(2*v(out))||-2.5"));
        QCOMPARE(c.ctol, QStringLiteral("1e-3"));
        QVERIFY(!c.polish);
        QVERIFY(c.starts.isEmpty());
        // Opened again: as it was written.
        NgOptDialog reopened(ngopt, &doc);
        QCOMPARE(reopened.constraintTable()->rowCount(), 1);
        QCOMPARE(reopened.ctolEdit()->text(), QStringLiteral("1e-3"));
        QCOMPARE(reopened.preview(), dialog.preview());

        // A new component: nothing to optimize yet.
        NgOpt_Sim fresh;
        NgOptDialog empty(&fresh, &doc);
        QVERIFY(empty.preview().startsWith("(not complete"));
        QMetaObject::invokeMethod(&empty, "slotCancel");
        QCOMPARE(empty.result(), int(QDialog::Rejected));
    }

    // Stock ngspice has no optimize: the run says what is missing.
    void anNgspiceWithoutTheCommand()
    {
        const QString fake = dir.filePath("stock-ngspice");
        QVERIFY(write(fake, "#!/bin/sh\necho 'optimize: no such command available in ngspice'\nexit 0\n"));
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QucsSettings.NgspiceExecutable = fake;
        const QString file = copyExample(dir.path(), "stock");
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file);
        QVERIFY(doc != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("This ngspice has no optimize command"), qPrintable(log));
        QCOMPARE(Command::read(find(doc, "NgOpt1")).knobs.at(0).init, QStringLiteral("2.2n"));
    }

    // What the status log says of each ending, from an ngspice that prints
    // it: a search stopped at its iterations is reported and its values
    // become the knobs' initial ones, with the constraint at its end and
    // the start that won; one where no evaluation solved leaves them.
    void theLogSaysWhyItStopped()
    {
        // (The log and the knobs' initial values after the run.)
        const auto run = [this](const QString& name, const QString& printed, QString* log) {
            const QString fake = dir.filePath(name + "-ngspice");
            if (!write(fake, "#!/bin/sh\ncat <<'EOF'\n" + printed + "EOF\nexit 0\n")) return false;
            QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            QucsSettings.NgspiceExecutable = fake;
            QucsApp app(false);
            MainGuard guard(&app);
            Schematic* doc = simulate(app, copyExample(dir.path(), name));
            if (doc == nullptr) return false;
            QStringList inits;
            for (const Knob& k : Command::read(find(doc, "NgOpt1")).knobs) inits << k.init;
            *log = logText(app) + "\ninits: " + inits.join(' ') + (ticked(app, "NgOpt1: ") ? "\nticked" : "\nwarned");
            return true;
        };
        QString log;
        QVERIFY(run("capped",
                    "optimize: start 2 of 2 won (cost 0.25)\n"
                    "optimize: stopped at -maxiter (5 iterations) -- NOT converged, sum-sq residual = 0.01 (rms 0.05) after 40 evaluations\n"
                    "optimize: constraint db(2*v(out)) >= -3.5 -- -3.4, slack 0.1\n"
                    "    cin = 3e-09\n    lm = 1.5e-05\n    cout = 3e-09\n",
                    &log));
        QVERIFY2(log.contains("NgOpt1: stopped at -maxiter (5 iterations) -- NOT converged"), qPrintable(log));
        QVERIFY2(log.contains("start 2 of 2 won (cost 0.25)"), qPrintable(log));
        QVERIFY2(log.contains("constraint db(2*v(out)) >= -3.5 -- -3.4, slack 0.1"), qPrintable(log));
        QVERIFY2(log.contains("inits: 3n 15u 3n"), qPrintable(log));
        QVERIFY2(log.endsWith("warned"), qPrintable(log));   // not converged
        QVERIFY(run("unsolved",
                    "optimize: NO SOLUTION -- no evaluation solved, sum-sq residual = 1e+30 (rms 1e+15) after 15 evaluations\n"
                    "    cin = 1e-09\n    lm = 1e-06\n    cout = 1e-09\n",
                    &log));
        QVERIFY2(log.contains("NgOpt1: NO SOLUTION -- no evaluation solved"), qPrintable(log));
        QVERIFY2(log.contains("inits: 2.2n 4.7u 2.2n"), qPrintable(log));   // as they were
    }

    // With an ngspice that has optimize: the example fitted to Butterworth.
    void theExampleIsFitted()
    {
        if (optimizing.isEmpty()) QSKIP("no ngspice with the optimize command");
        QucsSettings.NgspiceExecutable = optimizing;
        const QString file = copyExample(dir.path(), "example");
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file);
        QVERIFY(doc != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("NgOpt1: converged, sum-sq residual"), qPrintable(log));

        const Command c = Command::read(find(doc, "NgOpt1"));
        const double expected[] = {3.183e-9, 15.92e-6, 3.183e-9};
        for (int k = 0; k < 3; ++k) {
            const double v = qucs_s::units::read(c.knobs.at(k).init).value;
            QVERIFY2(std::abs(v - expected[k]) < 1e-3 * expected[k], qPrintable(c.knobs.at(k).toString()));
        }
        QVERIFY2(c.knobs.at(0).init.endsWith("n"), qPrintable(c.knobs.at(0).init));   // 3.18306n
        QVERIFY(doc->getDocChanged());

        // AC1 ran at the optimum: -3.01 dB at 1 MHz.
        const auto table = qucs_s::optimization::readDataset(dir.filePath("example.dat.ngspice"));
        const QVector<double> f = table.value("frequency");
        const QVector<double> gain = table.value(qucs_s::optimization::resolve(table.keys(), "gain", "ac"));
        const int i = int(std::find_if(f.begin(), f.end(), [](double x) { return std::abs(x - 1e6) < 1; }) - f.begin());
        QVERIFY(i < gain.size());
        QVERIFY2(std::abs(gain.at(i) + 3.0103) < 1e-3, qPrintable(QString::number(gain.at(i))));
    }

    // A search that ends otherwise than "converged" is read (an annealing
    // whose schedule ran out was "no optimum" since ngspice says why it
    // stopped), and a constraint holds: the divider's v(out) kept at 0.3 or
    // more by CMA-ES from two starts, while 0.25 is the target.
    void theNewMethodsRun()
    {
        if (optimizing.isEmpty()) QSKIP("no ngspice with the optimize command");
        const QString ngspice = newMethodsNgspice(dir.path(), optimizing);
        if (ngspice.isEmpty()) QSKIP("no ngspice with optimize's cmaes, bayes, tr and constraints");
        QucsSettings.NgspiceExecutable = ngspice;
        const QString file = dir.filePath("anneal.sch");
        QVERIFY(write(file, divider(file, "\"Method=sa\" 1 \"MaxIter=5\" 0 \"Tol=\" 0 \"Size=\" 0 \"Seed=1\" 0 "
                                          "\"Verbose=no\" 0 \"Analysis=AC1\" 0 \"Minimize=(mag(v(out))-0.25)^2\" 1 "
                                          "\"Knob=param|R2|1k|10|100k\" 1")));
        {
            QucsApp app(false);
            MainGuard guard(&app);
            Schematic* doc = simulate(app, file);
            QVERIFY(doc != nullptr);
            const QString log = logText(app);
            QVERIFY2(log.contains("NgOpt1: cooling schedule complete (5 levels)"), qPrintable(log));
            QVERIFY2(!log.contains("reported no optimum"), qPrintable(log));
            QVERIFY2(Command::read(find(doc, "NgOpt1")).knobs.at(0).init != "1k", qPrintable(log));   // the next run starts there
        }

        const QString constrained = dir.filePath("constrained.sch");
        QVERIFY(write(constrained, divider(constrained, "\"Method=cmaes\" 1 \"MaxIter=\" 0 \"Tol=1e-10\" 0 \"Size=\" 0 "
                                                        "\"Seed=3\" 0 \"Verbose=no\" 0 \"Analysis=AC1\" 0 "
                                                        "\"Minimize=(mag(v(out))-0.25)^2\" 1 \"Polish=no\" 0 \"Starts=1\" 0 "
                                                        "\"CTol=\" 0 \"Knob=param|R2|1k|10|100k\" 1 "
                                                        "\"Constraint=AC1|mag(v(out))|0.3|\" 1")));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, constrained);
        QVERIFY(doc != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("NgOpt1: converged"), qPrintable(log));
        QVERIFY2(log.contains("start ") && log.contains(" won (cost"), qPrintable(log));
        QVERIFY2(log.contains("constraint mag(v(out)) >= 0.3 -- ") && log.contains("active"), qPrintable(log));
        QVERIFY(ticked(app, "NgOpt1: converged"));
        // R2 / (1k + R2) = 0.3: R2 = 428.6, not the 333.3 of 0.25.
        const double r2 = qucs_s::units::read(Command::read(find(doc, "NgOpt1")).knobs.at(0).init).value;
        QVERIFY2(std::abs(r2 - 3000.0 / 7) < 1, qPrintable(QString::number(r2)));
        const auto table = qucs_s::optimization::readDataset(dir.filePath("constrained.dat.ngspice"));
        const QVector<double> ac = table.value(qucs_s::optimization::resolve(table.keys(), "v(out)", "ac"));
        QVERIFY2(!ac.isEmpty() && std::abs(ac.first() - 0.3) < 1e-3, qPrintable(table.keys().join(' ')));
    }

    // An instance knob (alter) is lost at the reset after the first
    // simulation unless the netlist sets it again: the transient after the
    // AC analysis still sees R2 at the optimum.
    void anInstanceKnobOutlivesTheReset()
    {
        if (optimizing.isEmpty()) QSKIP("no ngspice with the optimize command");
        QucsSettings.NgspiceExecutable = optimizing;
        const QString file = dir.filePath("alter.sch");
        QVERIFY(write(file, divider(file, "\"Method=nm\" 1 \"MaxIter=\" 0 \"Tol=1e-10\" 0 \"Size=\" 0 \"Seed=\" 0 "
                                          "\"Verbose=no\" 0 \"Analysis=AC1\" 0 \"Minimize=(mag(v(out))-0.25)^2\" 1 "
                                          "\"Knob=param|R2|1k|10|100k\" 1")));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file);
        QVERIFY(doc != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("NgOpt1: converged"), qPrintable(log));
        const double r2 = qucs_s::units::read(Command::read(find(doc, "NgOpt1")).knobs.at(0).init).value;
        QVERIFY2(std::abs(r2 - 1000.0 / 3) < 1, qPrintable(QString::number(r2)));

        const auto table = qucs_s::optimization::readDataset(dir.filePath("alter.dat.ngspice"));
        const QVector<double> ac = table.value(qucs_s::optimization::resolve(table.keys(), "v(out)", "ac"));
        const QVector<double> tran = table.value(qucs_s::optimization::resolve(table.keys(), "v(out)", "tran"));
        QVERIFY2(!ac.isEmpty() && !tran.isEmpty(), qPrintable(table.keys().join(' ')));
        QVERIFY(std::abs(ac.first() - 0.25) < 1e-4);
        const double peak = *std::max_element(tran.begin(), tran.end());
        QVERIFY2(std::abs(peak - 0.25) < 5e-3, qPrintable(QString::number(peak)));   // not 0.5: R2 kept
    }
};

QTEST_MAIN(TestNgOpt)
#include "test_ngopt.moc"
