/*
 * test_ngsweep.cpp - NgSweep, ngspice's own sweep command as a component:
 * the values a knob takes and the spec ngspice gets, the command line,
 * the component saved and loaded, the .control lines, the raw files read
 * into the dataset (the families of every point's waveforms, the values
 * recorded against the knobs), the status log, the netlist, the ERC, the
 * dialog, an ngspice without the command - and, with an ngspice that has
 * it, the shipped example and ac, op and tran sweeps run through Qucs-S.
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
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "ngsweep.h"
#include "optimization.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "components/component.h"
#include "components/ngsweep_sim.h"
#include "components/ngsweepdialog.h"
#include "diagrams/diagram.h"
#include "diagrams/graph.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::ngsweep;
using qucs_s::ngstats::Record;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

const QString kExample = QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/NGspice features/RC_lowpass_ngsweep.sch");

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

// An RC low-pass: V1 (1 V ac, 1 V dc) on "in", R1 to "out", C1 (100 nF) to
// ground, AC1 from 100 Hz to 100 kHz, TR1 and DC1, and the components given.
QString lowpass(const QString& file, const QString& components)
{
    const QString base = QFileInfo(file).completeBaseName();
    return QStringLiteral(
               "<Qucs Schematic " PACKAGE_VERSION ">\n"
               "<Properties>\n  <View=0,0,800,600,1,0,0>\n  <Grid=10,10,1>\n"
               "  <DataSet=%1.dat>\n  <DataDisplay=%1.dpl>\n  <OpenDisplay=0>\n"
               "  <Script=%1.m>\n  <RunScript=0>\n  <showFrame=0>\n</Properties>\n"
               "<Symbol>\n</Symbol>\n<Components>\n"
               "  <Vac V1 1 60 200 18 -26 1 1 \"1 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0>\n"
               "  <GND * 1 60 260 0 0 0 0>\n"
               "  <R R1 1 160 140 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
               "  <C C1 1 260 200 17 -26 0 1 \"100n\" 1 \"\" 0 \"neutral\" 0>\n"
               "  <GND * 1 260 260 0 0 0 0>\n"
               "  <.AC AC1 1 60 430 0 45 0 0 \"log\" 1 \"100 Hz\" 1 \"100 kHz\" 1 \"7\" 1 \"no\" 0>\n"
               "%2"
               "</Components>\n<Wires>\n"
               "  <60 230 60 260 \"\" 0 0 0 \"\">\n"
               "  <60 140 60 170 \"\" 0 0 0 \"\">\n"
               "  <60 140 130 140 \"in\" 70 110 0 \"\">\n"
               "  <190 140 260 140 \"out\" 230 110 0 \"\">\n"
               "  <260 140 260 170 \"\" 0 0 0 \"\">\n"
               "  <260 230 260 260 \"\" 0 0 0 \"\">\n"
               "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
        .arg(base, components);
}

// The line of an NgSweep component, from a sweep.
QString sweepLine(const QString& name, const Sweep& sweep, int x = 300, int y = 430)
{
    NgSweep_Sim c;
    c.Name = name;
    sweep.write(&c);
    QString line = QStringLiteral("  <.NGSWEEP %1 1 %2 %3 0 45 0 0").arg(name).arg(x).arg(y);
    for (const Property* p : c.Props)
        line += QStringLiteral(" \"%1=%2\" %3").arg(p->Name, p->Value).arg(p->display ? 1 : 0);
    return line + ">\n";
}

Sweep acSweep()
{
    Sweep s;
    s.analysis = "AC1";
    s.knob.name = "R1";
    s.knob.type = "list";
    s.knob.values = "1k; 2k; 4k";
    return s;
}

// ngspice from PATH if it has the sweep command, else empty.
QString sweepNgspice(const QString& dir)
{
    const QString exe = QStandardPaths::findExecutable("ngspice");
    if (exe.isEmpty()) return QString();
    const QString deck = dir + "/probe.cir";
    if (!write(deck, "* probe\nR1 1 0 1\nV1 1 0 1\n.control\nhelp sweep\n.endc\n.end\n")) return QString();
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, {"-b", deck});
    if (!p.waitForFinished(20000)) return QString();
    return QString::fromUtf8(p.readAll()).contains("sweep any knob") ? exe : QString();
}

// A raw file's plot, as ngspice writes one in ASCII.
QString rawPlot(const QString& name, bool complex, const QStringList& vectors, const QList<QStringList>& points)
{
    QString s = QStringLiteral("Title: t\nDate: today\nPlotname: %1\nFlags: %2\nNo. Variables: %3\nNo. Points: %4\n"
                               "Variables:\n")
                    .arg(name, complex ? "complex" : "real")
                    .arg(vectors.size())
                    .arg(points.size());
    for (int i = 0; i < vectors.size(); ++i) s += QStringLiteral("\t%1\t%2\tnotype\n").arg(i).arg(vectors.at(i));
    s += "Values:\n";
    for (int p = 0; p < points.size(); ++p) {
        s += QStringLiteral(" %1").arg(p);
        for (const QString& v : points.at(p)) s += QStringLiteral("\t%1\n").arg(v);
        s += "\n";
    }
    return s;
}

// The names of a dataset's variables, and what each depends on.
QMap<QString, QString> variables(const QString& dataset)
{
    QMap<QString, QString> out;
    static const QRegularExpression rx(QStringLiteral("^<(indep|dep) (\\S+)(?: ([^>]*))?"));
    for (const QString& line : dataset.split('\n')) {
        const QRegularExpressionMatch m = rx.match(line);
        if (m.hasMatch()) out.insert(m.captured(2), m.captured(1) == "indep" ? QStringLiteral("indep") : m.captured(3));
    }
    return out;
}

// The values of a variable in a dataset text, as written.
QStringList valuesOf(const QString& dataset, const QString& name)
{
    QStringList out;
    bool inside = false;
    for (const QString& line : dataset.split('\n')) {
        if (line.startsWith("<indep " + name + " ") || line.startsWith("<dep " + name + " ")) {
            inside = true;
            continue;
        }
        if (inside && line.startsWith("</")) break;
        if (inside) out << line;
    }
    return out;
}

} // namespace

class TestNgSweep : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString ngspice;

    Schematic* simulate(QucsApp& app, const QString& file)
    {
        if (!app.gotoPage(file)) return nullptr;
        Schematic* doc = app.currentSchematic();
        if (doc == nullptr || !QMetaObject::invokeMethod(&app, "slotSimulate")) return nullptr;
        QTest::qWait(50);
        QElapsedTimer t;
        t.start();
        while (app.simulationConsole()->isRunning() && t.elapsed() < 180000) QTest::qWait(50);
        return doc;
    }

    static QString logText(QucsApp& app)
    {
        QStringList lines;
        const QListWidget* log = app.simulationConsole()->statusLog();
        for (int i = 0; i < log->count(); ++i) lines << log->item(i)->text();
        return lines.join('\n');
    }

    // A schematic of the low-pass and the sweep, loaded.
    std::unique_ptr<Schematic> load(const QString& name, const Sweep& sweep)
    {
        const QString file = dir.filePath(name + ".sch");
        if (!write(file, lowpass(file, sweepLine("NgSweep1", sweep)))) return nullptr;
        auto doc = std::make_unique<Schematic>(nullptr, file);
        if (!doc->loadDocument()) return nullptr;
        return doc;
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
        ngspice = sweepNgspice(dir.path());
    }

    // The values of a knob and the spec ngspice gets: lin as ngspice
    // spaces it, log as a list of all its points, a list as numbers.
    void theValuesOfAKnob()
    {
        QVector<double> v;
        QString spec, why;
        Knob k;
        k.name = "R1";
        k.type = "lin";
        k.start = "1k";
        k.stop = "5 kOhm";
        k.points = "5";
        QVERIFY(knobValues(k, &v, &spec, &why));
        QCOMPARE(spec, QString("lin 5 1000 5000"));
        QCOMPARE(v, QVector<double>({1000, 2000, 3000, 4000, 5000}));

        k.points = "1";
        QVERIFY(knobValues(k, &v, &spec));
        QCOMPARE(v, QVector<double>({1000}));

        k.type = "log";
        k.stop = "100k";
        k.points = "3";
        QVERIFY(knobValues(k, &v, &spec));
        QCOMPARE(spec, QString("list 1000 10000 100000"));
        QCOMPARE(v.size(), 3);
        QVERIFY(std::abs(v.at(1) - 10000) < 1e-6);

        k.type = "list";
        k.values = "[1k; 2.2k, 4.7n 5]";
        QVERIFY(knobValues(k, &v, &spec));
        QCOMPARE(spec, QString("list 1000 2200 4.7e-09 5"));
        QCOMPARE(v, QVector<double>({1000, 2200, 4.7e-9, 5}));

        // What cannot be swept.
        auto refused = [&](const Knob& bad, const char* text) {
            QString error;
            if (knobValues(bad, &v, &spec, &error)) return false;
            return error.contains(QString::fromLatin1(text));
        };
        Knob bad = k;
        bad.values = "";
        QVERIFY(refused(bad, "no values to sweep"));
        bad.values = "1k; x";
        QVERIFY(refused(bad, "\"x\" is not a number"));
        bad = Knob{"R1", "lin", "a", "1k", "3", ""};
        QVERIFY(refused(bad, "the start \"a\" is not a number"));
        bad = Knob{"R1", "lin", "1", "b", "3", ""};
        QVERIFY(refused(bad, "the stop \"b\" is not a number"));
        bad = Knob{"R1", "lin", "1", "2", "0", ""};
        QVERIFY(refused(bad, "not a whole number above 0"));
        bad.points = "2.5";
        QVERIFY(refused(bad, "not a whole number above 0"));
        bad = Knob{"R1", "log", "0", "10", "3", ""};
        QVERIFY(refused(bad, "a logarithmic sweep needs a start and a stop above 0"));
        bad = Knob{"R1", "dec", "1", "10", "3", ""};
        QVERIFY(refused(bad, "is not lin, log or list"));

        // Saved as one property.
        Knob outer{"@dmod[is]", "list", "", "", "", "1e-15; 1e-14"};
        Knob back;
        QVERIFY(Knob::parse(outer.toString(), &back));
        QCOMPARE(back.toString(), outer.toString());
    }

    // The command line: the knob and its spec, the outer knobs, the
    // analysis a simulation component runs, the recorded values and then
    // the voltages and currents.
    void theCommandLine()
    {
        auto doc = load("line", acSweep());
        QVERIFY(doc != nullptr);
        Sweep s = acSweep();
        QString line, why;
        QVERIFY2(commandLine(s, doc.get(), "v(out) i(vpr1)", &line, &why), qPrintable(why));
        QCOMPARE(line, QString("sweep R1 list 1000 2000 4000 -analysis ac dec 2 100 100k -output v(out) i(vpr1)"));

        s.outer << Knob{"C1", "lin", "100n", "200n", "2", ""} << Knob{"temp", "list", "", "", "", "0; 50"};
        s.records << Record{"peak", "maximum(vdb(out))"};
        QVERIFY2(commandLine(s, doc.get(), "v(out)", &line, &why), qPrintable(why));
        QCOMPARE(line, QString("sweep R1 list 1000 2000 4000 -vs C1 lin 2 1e-07 2e-07 -vs temp list 0 50 "
                               "-analysis ac dec 2 100 100k -output peak=maximum(vdb(out)) v(out)"));

        // An ngspice command as the analysis; an op needs no output.
        s = acSweep();
        s.analysis = "op";
        QVERIFY(commandLine(s, doc.get(), "", &line, &why));
        QCOMPARE(line, QString("sweep R1 list 1000 2000 4000 -analysis op"));
        // Not known (the dialog, the ERC): nothing for the nodes.
        s.analysis = "AC1";
        QVERIFY(commandLine(s, doc.get(), QString(), &line, &why));
        QCOMPARE(line, QString("sweep R1 list 1000 2000 4000 -analysis ac dec 2 100 100k"));

        auto refused = [&](const Sweep& bad, const QString& nodes, const char* text) {
            QString error;
            if (commandLine(bad, doc.get(), nodes, &line, &error)) return false;
            if (!error.contains(QString::fromLatin1(text))) qWarning() << error;
            return error.contains(QString::fromLatin1(text));
        };
        Sweep bad = acSweep();
        bad.analysis.clear();
        QVERIFY(refused(bad, "v(out)", "no analysis to run"));
        bad = acSweep();
        bad.knob.name.clear();
        QVERIFY(refused(bad, "v(out)", "no parameter to sweep"));
        bad.knob.name = "R 1";
        QVERIFY(refused(bad, "v(out)", "is not a parameter ngspice can read"));
        bad = acSweep();
        bad.outer << Knob{"@r1", "list", "", "", "", "1"};
        QVERIFY(refused(bad, "v(out)", "@r1 is swept twice"));
        bad = acSweep();
        bad.knob.name = "time";
        QVERIFY(refused(bad, "v(out)", "the name of the analysis' own scale"));
        bad = acSweep();
        for (int i = 0; i < 4; ++i) bad.outer << Knob{QString("p%1").arg(i), "list", "", "", "", "1"};
        QVERIFY(refused(bad, "v(out)", "more than 4 parameters"));
        bad = acSweep();
        bad.outer << Knob{"C1", "lin", "1n", "2n", "100000", ""};
        QVERIFY(refused(bad, "v(out)", "more than 100000 runs"));
        bad = acSweep();
        bad.records << Record{"2x", "v(out)"};
        QVERIFY(refused(bad, "v(out)", "is not a name ngspice takes"));
        bad.records = {Record{"r1", "v(out)"}};
        QVERIFY(refused(bad, "v(out)", "taken by the results"));
        bad.records = {Record{"g", "v(out)"}, Record{"G", "v(in)"}};
        QVERIFY(refused(bad, "v(out)", "the name G is given twice"));
        bad.records = {Record{"g", " "}};
        QVERIFY(refused(bad, "v(out)", "g has no expression"));
        bad = acSweep();
        QVERIFY(refused(bad, "", "nothing to record: label a wire"));
        // Another analysis: the values named, not the voltages.
        bad.analysis = "noise v(out) v1 dec 10 1 1k";
        QVERIFY(refused(bad, "v(out)", "nothing to record: add a value"));
        bad.records << Record{"onoise", "onoise_total"};
        QVERIFY(commandLine(bad, doc.get(), "v(out)", &line, &why));
        QVERIFY(line.endsWith("-analysis noise v(out) v1 dec 10 1 1k -output onoise=onoise_total"));
        bad.knob.type = "list";
        bad.knob.values = "1k; y";
        QVERIFY(refused(bad, "v(out)", "\"y\" is not a number"));
    }

    // Saved and loaded as the component's properties; the range or the
    // list shown, whichever there is.
    void theComponentIsSavedAndLoaded()
    {
        NgSweep_Sim fresh;
        const Sweep defaults = Sweep::read(&fresh);
        QCOMPARE(defaults.knob.name, QString("R1"));
        QCOMPARE(defaults.knob.type, QString("lin"));
        QVERIFY(defaults.waveforms);
        QVERIFY(defaults.analysis.isEmpty());
        QCOMPARE(fresh.Model, QString(kModel));
        QVERIFY(fresh.isSimulation);

        Sweep s = acSweep();
        s.waveforms = false;
        s.outer << Knob{"C1", "list", "", "", "", "100n; 200n"};
        s.records << Record{"peak", "maximum(vdb(out))"};
        NgSweep_Sim c;
        s.write(&c);
        const Sweep back = Sweep::read(&c);
        QCOMPARE(back.analysis, s.analysis);
        QCOMPARE(back.knob.toString(), s.knob.toString());
        QCOMPARE(back.waveforms, false);
        QCOMPARE(back.outer.size(), 1);
        QCOMPARE(back.outer.first().toString(), s.outer.first().toString());
        QCOMPARE(back.records.size(), 1);
        QCOMPARE(back.records.first().toString(), QString("peak|maximum(vdb(out))"));
        auto shown = [&](const QString& name) {
            for (const Property* p : c.Props)
                if (p->Name == name) return p->display;
            return false;
        };
        QVERIFY(shown("List"));
        QVERIFY(!shown("Start") && !shown("Stop") && !shown("Points"));
        s.knob.type = "lin";
        s.write(&c);
        QVERIFY(!shown("List"));
        QVERIFY(shown("Start") && shown("Stop") && shown("Points"));

        // Through a schematic file and back.
        auto doc = load("saved", Sweep::read(&c));
        QVERIFY(doc != nullptr);
        Component* loaded = find(doc.get(), "NgSweep1");
        QVERIFY(loaded != nullptr);
        QVERIFY(isSweep(loaded));
        QCOMPARE(Sweep::read(loaded).outer.first().toString(), s.outer.first().toString());
        QVERIFY(doc->save() >= 0);
        std::unique_ptr<Schematic> again = std::make_unique<Schematic>(nullptr, doc->getDocName());
        QVERIFY(again->loadDocument());
        QCOMPARE(Sweep::read(find(again.get(), "NgSweep1")).records.first().toString(), QString("peak|maximum(vdb(out))"));
    }

    // The .control lines: between the markers, alone in the plots, the
    // sweep, its plot written, and each run's plot walked back to and
    // written after it.
    void theControlBlock()
    {
        Sweep s = acSweep();
        s.outer << Knob{"C1", "list", "", "", "", "100n; 200n"};
        auto doc = load("block", s);
        QVERIFY(doc != nullptr);
        Component* c = find(doc.get(), "NgSweep1");
        QStringList outputs;
        QString why;
        const QString block = controlBlock(c, doc.get(), "v(out) i(vpr1)", &outputs, &why);
        QVERIFY2(!block.isEmpty(), qPrintable(why));
        QCOMPARE(block, QString("echo \"qucs-s: begin NgSweep1\"\n"
                                "destroy all\n"
                                "set filetype=ascii\n"
                                "sweep R1 list 1000 2000 4000 -vs C1 list 1e-07 2e-07 -analysis ac dec 2 100 100k "
                                "-output v(out) i(vpr1)\n"
                                "write spice4qucs.ngsweep1.ngsweep\n"
                                "repeat 6\nsetplot previous\nend\n"
                                "write spice4qucs.ngsweep1.ngswaves v(out) i(vpr1)\n"
                                "set appendwrite\nrepeat 5\nsetplot next\n"
                                "write spice4qucs.ngsweep1.ngswaves v(out) i(vpr1)\nend\nunset appendwrite\n"
                                "echo \"qucs-s: end NgSweep1\"\n"));
        QCOMPARE(outputs, QStringList({"spice4qucs.ngsweep1.ngsweep", "spice4qucs.ngsweep1.ngswaves"}));
        QVERIFY(isResultFile(outputs.at(0)) && isResultFile(outputs.at(1)));
        QVERIFY(!isResultFile("spice4qucs.ac1.plot"));

        // A transient on its step; one run, no walk forward.
        Component* tran = find(doc.get(), "AC1");
        QVERIFY(tran != nullptr);
        s = acSweep();
        s.analysis = "tran 1u 1m";
        s.knob.values = "1k";
        s.write(c);
        outputs.clear();
        const QString one = controlBlock(c, doc.get(), "v(out)", &outputs);
        QVERIFY(one.contains("option interp\nsweep R1 list 1000 -analysis tran 1u 1m -output v(out)\n"));
        QVERIFY(one.contains("repeat 1\nsetplot previous\nend\nwrite spice4qucs.ngsweep1.ngswaves v(out)\necho"));
        QVERIFY(!one.contains("appendwrite"));

        // No waveforms: after an op, or when not asked for.
        s.analysis = "op";
        s.write(c);
        outputs.clear();
        QVERIFY(!controlBlock(c, doc.get(), "v(out)", &outputs).contains("ngswaves"));
        QCOMPARE(outputs, QStringList({"spice4qucs.ngsweep1.ngsweep"}));
        s = acSweep();
        s.waveforms = false;
        s.write(c);
        outputs.clear();
        QVERIFY(!controlBlock(c, doc.get(), "v(out)", &outputs).contains("ngswaves"));
        QVERIFY(!controlBlock(c, doc.get(), "", &outputs).contains("sweep"));   // nothing to record
        QVERIFY(!controlBlock(c, doc.get(), "", &outputs, &why).size());
        QVERIFY(why.contains("nothing to record"));
    }

    // The dataset: the knobs' values, every run's waveforms as a family
    // against the scale and the knobs, the recorded values against the
    // knobs; with outer knobs the values' curves found by their names.
    void theDataset()
    {
        Sweep s = acSweep();
        s.outer << Knob{"C1", "list", "", "", "", "100n; 200n"};
        s.records << Record{"peak", "maximum(vdb(out))"};
        auto doc = load("dataset", s);
        QVERIFY(doc != nullptr);
        const QString work = dir.filePath("dataset-work");
        QVERIFY(QDir().mkpath(work));

        // The values: r1, then each output's two curves over c1 - in the
        // order ngspice writes them, by name.
        QList<QStringList> points;
        for (int i = 0; i < 3; ++i)
            points << QStringList{QString::number(1000 * (1 << i)), QString::number(-1 - i), QString::number(-10 - i),
                                  QString::number(0.1 * (i + 1)), QString::number(0.2 * (i + 1))};
        QVERIFY(write(work + "/" + valuesFile("NgSweep1"),
                      rawPlot("Sweep", false, {"r1", "peak_c1_1e_07", "peak_c1_2e_07", "v(out)_c1_1e_07", "v(out)_c1_2e_07"},
                              points)));
        // Six runs of a 2-point ac: v(out) = run + j*frequency/1000.
        QString waves;
        for (int run = 0; run < 6; ++run)
            waves += rawPlot("AC Analysis", true, {"frequency", "v(out)"},
                             {{"100,0", QString("%1,0.1").arg(run)}, {"1000,0", QString("%1,1").arg(run)}});
        QVERIFY(write(work + "/" + waveformsFile("NgSweep1"), waves));

        QVERIFY(datasetBlocks(work, waveformsFile("NgSweep1"), doc.get()).isEmpty());   // with the values' file
        const QString ds = datasetBlocks(work, valuesFile("NgSweep1"), doc.get());
        const QMap<QString, QString> vars = variables(ds);
        QCOMPARE(vars.value("ngsweep1.r1"), QString("indep"));
        QCOMPARE(vars.value("ngsweep1.c1"), QString("indep"));
        QCOMPARE(vars.value("ngsweep1.frequency"), QString("indep"));
        QCOMPARE(vars.value("ngsweep1.v(out)"), QString("ngsweep1.frequency ngsweep1.r1 ngsweep1.c1"));
        QCOMPARE(vars.value("ngsweep1.peak"), QString("ngsweep1.r1 ngsweep1.c1"));
        QCOMPARE(vars.size(), 5);   // the voltages' last values give way to their waveforms
        QCOMPARE(valuesOf(ds, "ngsweep1.c1").size(), 2);
        QVERIFY(valuesOf(ds, "ngsweep1.c1").at(1).startsWith("2.0000"));
        // peak: r1 fastest, then c1.
        QStringList peak = valuesOf(ds, "ngsweep1.peak");
        QCOMPARE(peak.size(), 6);
        QCOMPARE(peak.at(0).toDouble(), -1.0);
        QCOMPARE(peak.at(2).toDouble(), -3.0);
        QCOMPARE(peak.at(3).toDouble(), -10.0);
        // v(out): frequency fastest, then the runs in order, complex.
        QStringList v = valuesOf(ds, "ngsweep1.v(out)");
        QCOMPARE(v.size(), 12);
        QCOMPARE(v.at(0), QString("0.000000000000e+00+j1.000000000000e-01"));
        QCOMPARE(v.at(11), QString("5.000000000000e+00+j1.000000000000e+00"));

        // A run missing: no family; the voltages' last values instead.
        waves.clear();
        for (int run = 0; run < 5; ++run)
            waves += rawPlot("AC Analysis", true, {"frequency", "v(out)"}, {{"100,0", "1,1"}, {"1000,0", "1,1"}});
        QVERIFY(write(work + "/" + waveformsFile("NgSweep1"), waves));
        const QMap<QString, QString> fallback = variables(datasetBlocks(work, valuesFile("NgSweep1"), doc.get()));
        QCOMPARE(fallback.value("ngsweep1.v(out)"), QString("ngsweep1.r1 ngsweep1.c1"));
        QVERIFY(!fallback.contains("ngsweep1.frequency"));

        // A plot that is not the sweep's (it failed before making one).
        QVERIFY(write(work + "/" + valuesFile("NgSweep1"), rawPlot("Constants", false, {"pi"}, {{"3.14"}})));
        QVERIFY(datasetBlocks(work, valuesFile("NgSweep1"), doc.get()).isEmpty());
        // Another component's file.
        QVERIFY(datasetBlocks(work, valuesFile("NgSweep9"), doc.get()).isEmpty());
    }

    // An op sweep: every voltage and current against the knob; and
    // transients on their own times, put onto the first run's.
    void theDatasetOfAnOpAndOfTransients()
    {
        Sweep s = acSweep();
        s.analysis = "op";
        auto doc = load("opdata", s);
        QVERIFY(doc != nullptr);
        const QString work = dir.filePath("op-work");
        QVERIFY(QDir().mkpath(work));
        QVERIFY(write(work + "/" + valuesFile("NgSweep1"),
                      rawPlot("Sweep", false, {"r1", "i(v1)", "v(out)"},
                              {{"1000", "-1e-3", "0.5"}, {"2000", "-5e-4", "nan"}, {"4000", "-2.5e-4", "0.8"}})));
        QString ds = datasetBlocks(work, valuesFile("NgSweep1"), doc.get());
        QMap<QString, QString> vars = variables(ds);
        QCOMPARE(vars.value("ngsweep1.v(out)"), QString("ngsweep1.r1"));
        QCOMPARE(vars.value("ngsweep1.i(v1)"), QString("ngsweep1.r1"));
        QCOMPARE(valuesOf(ds, "ngsweep1.r1").at(2).toDouble(), 4000.0);
        QCOMPARE(valuesOf(ds, "ngsweep1.v(out)").at(1), QString("nan"));   // the point that did not solve

        s.analysis = "tran 1u 2u";
        s.knob.values = "1k; 2k";
        Component* c = find(doc.get(), "NgSweep1");
        s.write(c);
        QVERIFY(write(work + "/" + valuesFile("NgSweep1"),
                      rawPlot("Sweep", false, {"r1", "v(out)"}, {{"1000", "1"}, {"2000", "1"}})));
        const QString waves = rawPlot("Transient Analysis", false, {"time", "v(out)"}, {{"0", "0"}, {"1e-6", "1"}, {"2e-6", "2"}})
                              + rawPlot("Transient Analysis", false, {"time", "v(out)"}, {{"0", "0"}, {"2e-6", "4"}});
        QVERIFY(write(work + "/" + waveformsFile("NgSweep1"), waves));
        ds = datasetBlocks(work, valuesFile("NgSweep1"), doc.get());
        vars = variables(ds);
        QCOMPARE(vars.value("ngsweep1.v(out)"), QString("ngsweep1.time ngsweep1.r1"));
        const QStringList v = valuesOf(ds, "ngsweep1.v(out)");
        QCOMPARE(v.size(), 6);
        QCOMPARE(v.at(4).toDouble(), 2.0);   // the second run at 1 us: halfway
        QCOMPARE(v.at(5).toDouble(), 4.0);
    }

    // The status log: what was swept, ngspice's warnings, and the runs
    // whose waveforms could not be read.
    void theStatusLog()
    {
        auto doc = load("log", acSweep());
        QVERIFY(doc != nullptr);
        Component* c = find(doc.get(), "NgSweep1");
        const QString work = dir.filePath("log-work");
        QVERIFY(QDir().mkpath(work));
        QString out = "Circuit: x\nqucs-s: begin NgSweep1\n"
                      "Warning from checkvalid: vector r1 is not available or has zero length.\n"
                      "sweep: r1 (instance/device) over 3 points, analysis 'ac dec 2 100 100k'\n"
                      "sweep: 3 points into plot 'sweep1' (now current); `plot <output>` to view vs r1.\n"
                      "ASCII raw file \"spice4qucs.ngsweep1.ngsweep\"\nqucs-s: end NgSweep1\n";
        qucs_s::ngstats::Summary summary = summarize(c, doc.get(), out, work);
        QCOMPARE(summary.text, QString("NgSweep1: r1 (instance/device) over 3 points, analysis 'ac dec 2 100 100k'"));
        QVERIFY(!summary.warning);

        out.replace("ASCII raw file", "sweep: WARNING -- 1 of 3 points did not converge; those points are recorded "
                                      "as NaN, not as results (Enhancement-445).\nASCII raw file");
        summary = summarize(c, doc.get(), out, work);
        QVERIFY(summary.warning);
        QVERIFY(summary.text.contains("\nWARNING -- 1 of 3 points did not converge"));

        // The waveforms of a run missing.
        QVERIFY(write(work + "/" + waveformsFile("NgSweep1"),
                      rawPlot("AC Analysis", true, {"frequency", "v(out)"}, {{"100,0", "1,1"}})));
        summary = summarize(c, doc.get(), out, work);
        QVERIFY(summary.text.contains("the voltages and currents of 2 of 3 runs could not be read"));

        summary = summarize(c, doc.get(), "qucs-s: begin NgSweep1\nsweep: 'rx' names no device, model, .param or "
                                          "temp that can be swept; nothing would have moved\nqucs-s: end NgSweep1\n",
                            work);
        QVERIFY(summary.warning);
        QVERIFY(summary.text.contains("'rx' names no device"));
        QVERIFY(summary.text.contains("ngspice ran no sweep"));
        summary = summarize(c, doc.get(), "nothing", work);
        QVERIFY(summary.text.contains("ngspice reported nothing"));

        // What ngspice prints of the knobs when it reads them is no warning.
        Sweep outer = acSweep();
        outer.outer << Knob{"C1", "list", "", "", "", "1n"};
        outer.write(c);
        const QString probed = "a\nWarning from checkvalid: vector r1 is not available or has zero length.\r\n"
                               "Warning from checkvalid: vector c1 is not available or has zero length.\n"
                               "Warning from checkvalid: vector v9 is not available or has zero length.\nb";
        QCOMPARE(withoutKnobProbes(probed, doc.get(), {"NgSweep1"}),
                 QString("a\nWarning from checkvalid: vector v9 is not available or has zero length.\nb"));
        QCOMPARE(withoutKnobProbes(probed, doc.get(), {}), probed);

        QVERIFY(unsupported("sweep: no such command available in ngspice\n"));
        QVERIFY(!unsupported("sweep: r1 over 3 points"));
        QCOMPARE(knobVariable("@r1[resistance]"), QString("r1_resistance"));
        QCOMPARE(knobVariable("@X1.rmod[res]"), QString("x1_rmod_res"));
        QCOMPARE(knobVariable("@@"), QString("knob"));
    }

    // The netlist: the sweep after the simulations, its voltages and
    // currents those of the simulations; one that cannot be written said
    // in the output.
    void theNetlist()
    {
        auto doc = load("netlist", acSweep());
        QVERIFY(doc != nullptr);
        Ngspice kernel(doc.get());
        kernel.setWorkdir(dir.filePath("work"));
        kernel.SaveNetlist(dir.filePath("work/net.cir"), false);
        const QString net = read(dir.filePath("work/net.cir"));
        const qsizetype ac = net.indexOf("\nac dec 2 100 100k");
        const qsizetype sweep = net.indexOf("\nsweep R1 list 1000 2000 4000 -analysis ac dec 2 100 100k -output v(in) v(out)\n");
        QVERIFY2(ac > 0 && sweep > ac, qPrintable(net));
        QVERIFY(net.contains("write spice4qucs.ngsweep1.ngswaves v(in) v(out)\n"));
        QVERIFY(net.indexOf("echo \"qucs-s: end NgSweep1\"\ndestroy all\nreset\n") > sweep);
        QCOMPARE(kernel.sweeps(), QStringList({"NgSweep1"}));

        Sweep bad = acSweep();
        bad.analysis.clear();
        Component* c = find(doc.get(), "NgSweep1");
        bad.write(c);
        kernel.SaveNetlist(dir.filePath("work/net.cir"), false);
        QVERIFY(read(dir.filePath("work/net.cir")).contains("echo \"Error: NgSweep1: no analysis to run\"\n"));
        QVERIFY(kernel.sweeps().isEmpty());
        c->isActive = COMP_IS_OPEN;   // off
        kernel.SaveNetlist(dir.filePath("work/net.cir"), false);
        QVERIFY(!read(dir.filePath("work/net.cir")).contains("NgSweep1"));
    }

    // Check Schematic: a sweep the netlist cannot write.
    void theErc()
    {
        Sweep bad = acSweep();
        bad.knob.values = "1k; nope";
        auto doc = load("erc", bad);
        QVERIFY(doc != nullptr);
        bool found = false;
        for (const qucs_s::erc::Issue& i : qucs_s::erc::check(doc.get()))
            found |= i.message.contains("NgSweep1: R1: \"nope\" is not a number");
        QVERIFY(found);
        bad.knob.values = "1k";
        bad.write(find(doc.get(), "NgSweep1"));
        for (const qucs_s::erc::Issue& i : qucs_s::erc::check(doc.get())) QVERIFY(!i.message.contains("NgSweep1"));
    }

    // The dialog: the form, the command it shows, what it writes.
    void theDialog()
    {
        Sweep s = acSweep();
        s.outer << Knob{"C1", "list", "", "", "", "100n; 200n"};
        s.records << Record{"peak", "maximum(vdb(out))"};
        auto doc = load("dialog", s);
        QVERIFY(doc != nullptr);
        Component* c = find(doc.get(), "NgSweep1");
        NgSweepDialog dialog(c, doc.get());
        QCOMPARE(dialog.analysisCombo()->currentText(), QString("AC1"));
        QVERIFY(dialog.analysisCombo()->findText("AC1") >= 0);
        QVERIFY(dialog.parameterCombo()->findText("R1") >= 0);
        QVERIFY(dialog.parameterCombo()->findText("temp") >= 0);
        QCOMPARE(dialog.parameterCombo()->currentText(), QString("R1"));
        QCOMPARE(dialog.typeCombo()->currentData().toString(), QString("list"));
        QVERIFY(!dialog.startEdit()->isEnabled());
        QVERIFY(dialog.valuesEdit()->isEnabled());
        QCOMPARE(dialog.outerTable()->rowCount(), 1);
        QCOMPARE(dialog.recordTable()->rowCount(), 1);
        QVERIFY2(dialog.preview().startsWith("sweep R1 list 1000 2000 4000 -vs C1 list 1e-07 2e-07 -analysis ac dec 2 100 "
                                             "100k -output peak=maximum(vdb(out))\n"),
                 qPrintable(dialog.preview()));

        dialog.typeCombo()->setCurrentIndex(dialog.typeCombo()->findData("log"));
        dialog.startEdit()->setText("1k");
        dialog.stopEdit()->setText("100k");
        dialog.pointsEdit()->setText("3");
        QVERIFY(dialog.startEdit()->isEnabled());
        QVERIFY(!dialog.valuesEdit()->isEnabled());
        QVERIFY(dialog.preview().startsWith("sweep R1 list 1000 10000 100000 -vs C1"));
        dialog.pointsEdit()->setText("");
        QVERIFY(dialog.preview().contains("not complete: R1: the points"));
        dialog.pointsEdit()->setText("3");
        dialog.addOuter(Knob{"temp", "lin", "0", "100", "3", ""});
        dialog.addOuter(Knob{"V1", "list", "", "", "", "1"});
        dialog.addOuter(Knob{"C2", "list", "", "", "", "1"});   // a fourth knob: not taken
        QCOMPARE(dialog.outerTable()->rowCount(), 3);
        dialog.waveformsBox()->setChecked(false);
        dialog.nameEdit()->setText("Sweep9");
        QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        QCOMPARE(c->Name, QString("Sweep9"));
        const Sweep back = Sweep::read(c);
        QCOMPARE(back.knob.type, QString("log"));
        QCOMPARE(back.knob.points, QString("3"));
        QCOMPARE(back.outer.size(), 3);
        QCOMPARE(back.outer.at(1).toString(), QString("temp|lin|0|100|3|"));
        QVERIFY(!back.waveforms);
        QCOMPARE(back.records.size(), 1);

        // An op: no waveforms to keep.
        dialog.analysisCombo()->setEditText("op");
        QVERIFY(!dialog.waveformsBox()->isEnabled());
        dialog.analysisCombo()->setEditText("tran 1u 1m");
        QVERIFY(dialog.waveformsBox()->isEnabled());
    }

    // The component in the palette, and its name on the schematic.
    void theComponentIsRegistered()
    {
        QString name;
        char* bitmap = nullptr;
        Element* e = NgSweep_Sim::info(name, bitmap, true);
        QVERIFY(e != nullptr);
        QCOMPARE(name, QString("ngspice sweep"));
        QCOMPARE(QString(bitmap), QString("ngsweep"));
        QVERIFY(QFile::exists(":/bitmaps/svg/ngsweep.svg"));
        delete e;
        auto* c = static_cast<Component*>(NgSweep_Sim::info(name, bitmap, true));
        QCOMPARE(c->Name, QString("NgSweep"));
        QCOMPARE(c->getSpiceNetlist(), QString());
        delete c;
    }

    // An ngspice without the command: said in the status log.
    void anNgspiceWithoutTheCommand()
    {
        const QString fake = dir.filePath("stock-ngspice");
        QVERIFY(write(fake, "#!/bin/sh\necho 'qucs-s: begin NgSweep1'\necho 'sweep: no such command available in ngspice'\n"
                            "echo 'qucs-s: end NgSweep1'\nexit 0\n"));
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QucsSettings.NgspiceExecutable = fake;
        const QString file = dir.filePath("stock.sch");
        QVERIFY(write(file, lowpass(file, sweepLine("NgSweep1", acSweep()))));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(simulate(app, file) != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("This ngspice has no sweep command: NgSweep1 needs"), qPrintable(log));
    }

    // With an ngspice that has sweep: R1 over an ac, each point's |v(out)|
    // that of its own low-pass.
    void anAcSweepRuns()
    {
        if (ngspice.isEmpty()) QSKIP("no ngspice with the sweep command");
        QucsSettings.NgspiceExecutable = ngspice;
        Sweep s = acSweep();
        s.records << Record{"atfc", "vdb(out)[3]"};
        const QString file = dir.filePath("acrun.sch");
        QVERIFY(write(file, lowpass(file, sweepLine("NgSweep1", s))));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(simulate(app, file) != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("NgSweep1: r1 (instance/device) over 3 points, analysis 'ac dec 2 100 100k'"), qPrintable(log));
        QVERIFY2(!log.contains("checkvalid"), qPrintable(log));

        const auto table = qucs_s::optimization::readDataset(dir.filePath("acrun.dat.ngspice"));
        const QVector<double> f = table.value("ngsweep1.frequency");
        const QVector<double> r = table.value("ngsweep1.r1");
        const QVector<double> v = table.value("ngsweep1.v(out)");
        QCOMPARE(r, QVector<double>({1000, 2000, 4000}));
        QCOMPARE(f.size(), 7);
        QCOMPARE(v.size(), 21);
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < f.size(); ++i) {
                const double w = 2 * M_PI * f.at(i) * r.at(k) * 100e-9;
                QVERIFY2(std::abs(v.at(k * f.size() + i) - 1 / std::sqrt(1 + w * w)) < 1e-6,
                         qPrintable(QString("%1 %2").arg(k).arg(i)));
            }
        const QVector<double> atfc = table.value("ngsweep1.atfc");
        QCOMPARE(atfc.size(), 3);
        QVERIFY(atfc.at(0) > atfc.at(1) && atfc.at(1) > atfc.at(2));   // the corner comes down as R grows
        // The simulation ran as well.
        QVERIFY(!table.value("ac.v(out)").isEmpty() || !table.value("v(out)").isEmpty());
    }

    // An op over a source and a resistor (-vs): the voltages against both;
    // a transient: each point's charging curve.
    void anOpAndATransientSweepRun()
    {
        if (ngspice.isEmpty()) QSKIP("no ngspice with the sweep command");
        QucsSettings.NgspiceExecutable = ngspice;
        Sweep s;
        s.analysis = "op";
        s.knob = Knob{"R1", "lin", "1k", "3k", "3", ""};
        s.outer << Knob{"V1", "list", "", "", "", "1; 2"};
        QString file = dir.filePath("oprun.sch");
        // A divider: R1 from "in" to "out", 1k from "out" to ground.
        QString text = lowpass(file, sweepLine("NgSweep1", s));
        text.replace("<C C1 1 260 200 17 -26 0 1 \"100n\" 1 \"\" 0 \"neutral\" 0>",
                     "<R R2 1 260 200 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>");
        text.replace("<Vac V1 1 60 200 18 -26 1 1 \"1 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0>", "<Vdc V1 1 60 200 18 -26 0 1 \"1 V\" 1>");
        QVERIFY(write(file, text));
        {
            QucsApp app(false);
            MainGuard guard(&app);
            QVERIFY(simulate(app, file) != nullptr);
            const QString log = logText(app);
            QVERIFY2(log.contains("NgSweep1: r1 over 3 points x v1(2) = 6 runs"), qPrintable(log));
            const auto table = qucs_s::optimization::readDataset(dir.filePath("oprun.dat.ngspice"));
            const QVector<double> out = table.value("ngsweep1.v(out)");
            QCOMPARE(table.value("ngsweep1.v1"), QVector<double>({1, 2}));
            QCOMPARE(out.size(), 6);
            for (int k = 0; k < 2; ++k)
                for (int i = 0; i < 3; ++i)
                    QVERIFY(std::abs(out.at(k * 3 + i) - (k + 1) * 1000.0 / (1000.0 * (i + 1) + 1000.0)) < 1e-9);
        }

        s = Sweep();
        s.analysis = "tran 10u 1m";
        s.knob = Knob{"R1", "list", "", "", "", "1k; 2k"};
        file = dir.filePath("tranrun.sch");
        text = lowpass(file, sweepLine("NgSweep1", s));
        text.replace("<Vac V1 1 60 200 18 -26 1 1 \"1 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0>",
                     "<Vdc V1 1 60 200 18 -26 0 1 \"1 V\" 1>");
        text.replace("<.AC AC1 1 60 430 0 45 0 0 \"log\" 1 \"100 Hz\" 1 \"100 kHz\" 1 \"7\" 1 \"no\" 0>",
                     "<.DC DC1 1 60 430 0 45 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 "
                     "\"none\" 0 \"CroutLU\" 0>");
        QVERIFY(write(file, text));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(simulate(app, file) != nullptr);
        const auto table = qucs_s::optimization::readDataset(dir.filePath("tranrun.dat.ngspice"));
        const QVector<double> t = table.value("ngsweep1.time");
        const QVector<double> v = table.value("ngsweep1.v(out)");
        QVERIFY2(t.size() > 50, qPrintable(logText(app)));
        QCOMPARE(v.size(), 2 * t.size());
        // The capacitor starts charged (the operating point): 1 V at all times.
        for (int i = 0; i < t.size(); i += 10) QVERIFY(std::abs(v.at(i) - 1) < 1e-3 && std::abs(v.at(t.size() + i) - 1) < 1e-3);
    }

    // With an ngspice that has sweep: the shipped example.
    void theExampleRuns()
    {
        if (ngspice.isEmpty()) QSKIP("no ngspice with the sweep command");
        QucsSettings.NgspiceExecutable = ngspice;
        const QString file = dir.filePath("example.sch");
        QString text = read(kExample);
        QVERIFY(!text.isEmpty());
        text.replace("RC_lowpass_ngsweep.", "example.");
        QVERIFY(write(file, text));
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* doc = simulate(app, file);
        QVERIFY(doc != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("NgSweep1: r1 (instance/device) over 5 points"), qPrintable(log));
        const auto table = qucs_s::optimization::readDataset(dir.filePath("example.dat.ngspice"));
        QCOMPARE(table.value("ngsweep1.r1").size(), 5);
        QCOMPARE(table.value("ngsweep1.v(out)").size(), 5 * table.value("ngsweep1.frequency").size());
        QCOMPARE(table.value("ngsweep1.fc").size(), 5);
        QVERIFY(table.value("ac.v(out)").isEmpty() && table.value("v(out)").isEmpty());   // AC1 is off: only swept
        // Its first diagram draws a curve for each value, each in a color
        // of its own; the second the corner frequency against R1.
        QCOMPARE(doc->a_Diagrams->size(), 2);
        Diagram* family = doc->a_Diagrams->front();
        family->loadGraphData(dir.filePath("example.dat"));
        QCOMPARE(family->Graphs.size(), 1);
        Graph* g = family->Graphs.first();
        QVERIFY(g->autoColor && g->colorsEachCurve());
        QCOMPARE(g->countY, 5);
        QCOMPARE(g->curveColor(0), Graph::autoPalette().at(0));
        QCOMPARE(g->curveColor(4), Graph::autoPalette().at(4));
        QCOMPARE(g->curveLabel(0), QString("r1=250"));
        QCOMPARE(g->curveLabel(4), QString("r1=4k"));
        Diagram* corner = doc->a_Diagrams->back();
        corner->loadGraphData(dir.filePath("example.dat"));
        QCOMPARE(corner->Graphs.first()->countY, 1);
        QCOMPARE(int(corner->Graphs.first()->count(0)), 5);
    }
};

QTEST_MAIN(TestNgSweep)
#include "test_ngsweep.moc"
