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
#include <QComboBox>
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

        QCOMPARE(methods().size(), 5);
        QCOMPARE(methods().first().first, QStringLiteral("de"));
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

        // Differential evolution with a population and a seed.
        dialog.methodCombo()->setCurrentIndex(dialog.methodCombo()->findData("de"));
        dialog.sizeEdit()->setText("24");
        dialog.seedEdit()->setText("5");
        QVERIFY2(dialog.preview().endsWith("-method de -swarmsize 24 -seed 5"), qPrintable(dialog.preview()));

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
