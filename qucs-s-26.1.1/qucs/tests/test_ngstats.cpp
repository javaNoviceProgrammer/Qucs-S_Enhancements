/*
 * test_ngstats.cpp - NgMonteCarlo and NgCorners, ngspice's own montecarlo
 * and corners commands as components: the command lines their properties
 * make, the components saved and loaded, the raw files read and turned
 * into the dataset, the netlist, what the status log says, the dialog, the
 * ERC, an ngspice without the commands - and, with an ngspice that has
 * them, the shipped Monte Carlo example and the corners of a Verilog-A
 * resistor run through Qucs-S.
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
#include <QProcess>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>
#include <numeric>

#include "config.h"
#include "erc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "ngstatistics.h"
#include "optimization.h"
#include "qucs.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "components/component.h"
#include "components/ngcorners_sim.h"
#include "components/ngmontecarlo_sim.h"
#include "components/ngstatisticsdialog.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::ngstats;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

const QString kExample = QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/NGspice features/RC_lowpass_montecarlo.sch");

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

// The example under another name in \a dir.
QString copyExample(const QString& dir, const QString& base)
{
    const QString file = dir + "/" + base + ".sch";
    QString text = read(kExample);
    text.replace("RC_lowpass_montecarlo.", base + ".");
    return write(file, text) ? file : QString();
}

// An RC low-pass: V1 on "in", R1 (or \a device) to "out", C1 to ground, an
// AC analysis AC1 and a transient TR1, and the components given.
QString lowpass(const QString& file, const QString& components, const QString& r1 = QString())
{
    const QString base = QFileInfo(file).completeBaseName();
    const QString resistor = r1.isEmpty()
        ? QStringLiteral("  <R R1 1 160 140 -26 15 0 0 \"{rr}\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
        : r1;
    return QStringLiteral(
               "<Qucs Schematic " PACKAGE_VERSION ">\n"
               "<Properties>\n  <View=0,0,800,600,1,0,0>\n  <Grid=10,10,1>\n"
               "  <DataSet=%2.dat>\n  <DataDisplay=%2.dpl>\n  <OpenDisplay=0>\n"
               "  <Script=%2.m>\n  <RunScript=0>\n  <showFrame=0>\n</Properties>\n"
               "<Symbol>\n</Symbol>\n<Components>\n"
               "  <Vac V1 1 60 200 18 -26 1 1 \"1 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0>\n"
               "  <GND * 1 60 260 0 0 0 0>\n"
               "%3"
               "  <C C1 1 260 200 17 -26 0 1 \"100n\" 1 \"\" 0 \"neutral\" 0>\n"
               "  <GND * 1 260 260 0 0 0 0>\n"
               "  <SpicePar SpicePar1 1 60 330 -28 16 0 0 \"rr=agauss(1k, 150, 3)\" 1>\n"
               "  <.AC AC1 1 60 430 0 45 0 0 \"log\" 1 \"100 Hz\" 1 \"100 kHz\" 1 \"7\" 1 \"no\" 0>\n"
               "  <.TR TR1 1 60 530 0 75 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"101\" 0 \"Trapezoidal\" 0 \"2\" 0 "
               "\"1 ns\" 0 \"1e-16\" 0 \"150\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"26.85\" 0 \"1e-3\" 0 "
               "\"1e-6\" 0 \"1\" 0 \"CroutLU\" 0 \"no\" 0 \"yes\" 0 \"0\" 0>\n"
               "%1"
               "</Components>\n<Wires>\n"
               "  <60 230 60 260 \"\" 0 0 0 \"\">\n"
               "  <60 140 60 170 \"\" 0 0 0 \"\">\n"
               "  <60 140 130 140 \"in\" 70 110 0 \"\">\n"
               "  <190 140 260 140 \"out\" 230 110 0 \"\">\n"
               "  <260 140 260 170 \"\" 0 0 0 \"\">\n"
               "  <260 230 260 260 \"\" 0 0 0 \"\">\n"
               "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
        .arg(components, base, resistor);
}

// ngspice from PATH if it has both commands, else empty.
QString statisticsNgspice(const QString& dir)
{
    const QString exe = QStandardPaths::findExecutable("ngspice");
    if (exe.isEmpty()) return QString();
    const QString deck = dir + "/probe.cir";
    if (!write(deck, "* probe\nR1 1 0 1\nV1 1 0 1\n.control\nhelp montecarlo\nhelp corners\n.endc\n.end\n"))
        return QString();
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, {"-b", deck});
    if (!p.waitForFinished(20000)) return QString();
    const QString out = QString::fromUtf8(p.readAll());
    return out.contains("packaged Monte Carlo") && out.contains("process corner") ? exe : QString();
}

// A Monte Carlo plot as ngspice writes it: 3 samples, a 2-point ac scale,
// the gain family, the fc scalar, the counts; padded or not.
QString monteCarloRaw(bool padded)
{
    const QList<QStringList> points = {
        {"1", "10", "100", "-1", "0.5", "1"},
        {"2", "20", "1000", "-2"},
        {"3", "30", "-3"},
        {"-4"}, {"-5"}, {"-6"}};
    QString s = QStringLiteral(
        "Title: ac dec 1 100 1k\nDate: today\nPlotname: Monte Carlo\nFlags: real%1\nNo. Variables: 6\n"
        "No. Points: 6\nDimensions: 3,2\nVariables:\n\t0\tsample\tnotype dims=3\n\t1\tfc\tnotype dims=3\n"
        "\t2\tfrequency\tfrequency dims=2\n\t3\tgain\tdecibel\n\t4\tmontecarlo_yield\tnotype dims=1\n"
        "\t5\tmontecarlo_nfailed\tnotype dims=1\nValues:\n").arg(padded ? "" : " unpadded");
    // Padded: every vector has a value at every point, zeros past its end.
    const QList<QStringList> full = {
        {"1", "10", "100", "-1", "0.5", "1"}, {"2", "20", "1000", "-2", "0", "0"}, {"3", "30", "0", "-3", "0", "0"},
        {"0", "0", "0", "-4", "0", "0"},      {"0", "0", "0", "-5", "0", "0"},    {"0", "0", "0", "-6", "0", "0"}};
    const QList<QStringList>& rows = padded ? full : points;
    for (int i = 0; i < rows.size(); ++i) {
        s += QStringLiteral(" %1").arg(i);
        for (const QString& v : rows.at(i)) s += "\t" + v + "\n";
        s += "\n";
    }
    return s;
}

// The corners plot and each corner's ac plot (complex), as the netlist
// writes them for two corners.
const char* kCornersRaw =
    "Title: ss ff\nDate: today\nPlotname: Corners\nFlags: real\nNo. Variables: 3\nNo. Points: 2\n"
    "Variables:\n\t0\tcorner\tnotype\n\t1\tcorners_n\tnotype dims=1\n\t2\tgmax\tnotype\nValues:\n"
    " 0\t0\n\t2\n\t-0.02\n\n 1\t1\n\t0\n\t-0.01\n\n";
QString wavesRaw(int corners)
{
    QString s;
    for (int k = 0; k < corners; ++k)
        s += QStringLiteral("Title: t\nDate: today\nPlotname: AC Analysis\nFlags: complex\nNo. Variables: 2\n"
                            "No. Points: 2\nVariables:\n\t0\tfrequency\tfrequency grid=3\n\t1\tv(out)\tvoltage\n"
                            "Values:\n 0\t1.0e+02,0.0e+00\n\t%1,-0.1\n\n 1\t1.0e+03,0.0e+00\n\t%2,-0.3\n\n")
                 .arg(0.5 + k)
                 .arg(0.1 + k);
    return s;
}

// What ngspice (Ngspice_OpenVAF_Enhancements) prints for a Monte Carlo and
// a corners run between the netlist's markers.
const char* kOutput =
    "qucs-s: begin NgMonteCarlo1\n"
    "montecarlo: 50 Latin-Hypercube samples, analysis 'op', 1 spec, seed 4\n"
    "montecarlo: recording 2 expressions per sample\n"
    "  NOTE    : no -seed given -- the random .params ... are drawn from the default seed 1\n"
    "montecarlo: 2 expressions over 50 samples recorded into plot 'montecarlo1' (now current)\n"
    "  yield  : 94.000%  (47 / 50 pass)\n"
    "  95% CI : [83.783%, 97.939%]  (Wilson score)\n"
    "  spec 1 (v(out)): 3 violations\n"
    "ASCII raw file \"spice4qucs.ngmontecarlo1.ngmc\"\n"
    "qucs-s: end NgMonteCarlo1\n"
    "qucs-s: begin NgCorners1\n"
    "corners: 4 corners (tt ss ff sf), analysis 'ac dec 2 100 100k'\n"
    "corners: 4 corners into plot 'corners1' (now current); the `corner` scale is the index\n"
    "  idx  corner                 gmax            rr\n"
    "  0    tt               -0.0171115          1000\n"
    "  1    ss               -0.0206964          1100\n"
    "  2    ff               -0.0138655           900\n"
    "  3    sf               -0.0188616          1050\n"
    "qucs-s: end NgCorners1\n";

} // namespace

class TestNgStats : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString ngspice;   // an ngspice with montecarlo and corners, if there is one

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
        ngspice = statisticsNgspice(dir.path());
    }

    // What a Monte Carlo's properties make of ngspice's command.
    void theMonteCarloCommandLine()
    {
        MonteCarlo c;
        c.samples = "200";
        c.analysis = "ac dec 10 100 100k";
        c.records = {{"gain", "db(v(out))"}, {"fc", "1 / (2*pi*@r1[resistance]*@c1[capacitance])"}};
        c.specs = {{"v(out)", "0.49", "0.51"}, {"maximum(db(v(out)))", "", "-0.1"}};
        QString line, why;
        QVERIFY2(commandLine(c, nullptr, &line, &why), qPrintable(why));
        QCOMPARE(line, QStringLiteral("montecarlo 200 -analysis \"ac dec 10 100 100k\" -spec v(out) -min 0.49 -max 0.51 "
                                      "-spec maximum(db(v(out))) -max -0.1 -expr gain=db(v(out)) "
                                      "-expr fc=1/(2*pi*@r1[resistance]*@c1[capacitance]) "
                                      "-expr spec1=v(out) -expr spec2=maximum(db(v(out)))"));
        c.lhs = true;
        c.seed = "7";
        c.specs.clear();
        QVERIFY(commandLine(c, nullptr, &line));
        QVERIFY2(line.startsWith("montecarlo 200 -lhs -seed 7 -analysis"), qPrintable(line));
        QVERIFY(!line.contains("spec"));

        // What cannot be written, and why.
        const QList<QPair<std::function<void(MonteCarlo&)>, QString>> refusals = {
            {[](MonteCarlo& m) { m.samples = "0"; }, "the samples \"0\" are not a whole number above 0"},
            {[](MonteCarlo& m) { m.samples = "1.5"; }, "the samples \"1.5\" are not a whole number above 0"},
            {[](MonteCarlo& m) { m.seed = "x"; }, "the seed \"x\" is not a whole number"},
            {[](MonteCarlo& m) { m.analysis.clear(); }, "no analysis to run"},
            {[](MonteCarlo& m) { m.records.clear(); }, "nothing to record and no spec to judge"},
            {[](MonteCarlo& m) { m.records[0].name = "2x"; }, "the name \"2x\" is not a name ngspice takes (letters, digits, _)"},
            {[](MonteCarlo& m) { m.records[0].name = "sample"; }, "the name sample is taken by the results"},
            {[](MonteCarlo& m) { m.records[0].name = "spec3"; }, "the name spec3 is taken by the results"},
            {[](MonteCarlo& m) { m.records[1].name = "Gain"; }, "the name Gain is given twice"},
            {[](MonteCarlo& m) { m.records[0].expression.clear(); }, "gain has no expression"},
            {[](MonteCarlo& m) { m.specs = {{"v(out)", "", ""}}; }, "v(out): a spec needs a minimum, a maximum or both"},
            {[](MonteCarlo& m) { m.specs = {{"v(out)", "1", "0.5"}}; }, "v(out): the maximum must be above the minimum"},
            {[](MonteCarlo& m) { m.specs = {{"v(out)", "low", ""}}; }, "v(out): the minimum \"low\" is not a number"},
            {[](MonteCarlo& m) { m.specs = {{"", "1", ""}}; }, "a spec has no expression"},
        };
        for (const auto& [change, expected] : refusals) {
            MonteCarlo m = c;
            change(m);
            QVERIFY2(!commandLine(m, nullptr, &line, &why), qPrintable(expected));
            QCOMPARE(why, expected);
        }
    }

    // What a corners component's properties make of ngspice's command.
    void theCornersCommandLine()
    {
        Corners c;
        c.analysis = "ac dec 2 100 100k";
        c.records = {{"gmax", "maximum(db(v(out)))"}, {"rr", "@rmod[r]"}};
        QString line, why;
        QVERIFY2(commandLine(c, nullptr, &line, &why), qPrintable(why));
        QCOMPARE(line, QStringLiteral("corners -analysis \"ac dec 2 100 100k\" -output gmax=maximum(db(v(out))) rr=@rmod[r]"));
        c.corners = "ss, ff;sf";
        c.nominal = false;
        QVERIFY(commandLine(c, nullptr, &line));
        QVERIFY2(line.startsWith("corners -list ss,ff,sf -nonominal -analysis"), qPrintable(line));
        // The waveforms alone are enough for an ac, dc or tran analysis.
        c.records.clear();
        QVERIFY(commandLine(c, nullptr, &line));
        QVERIFY(!line.contains("-output"));
        c.analysis = "op";
        QVERIFY(!commandLine(c, nullptr, &line, &why));
        QCOMPARE(why, QStringLiteral("nothing to record: add a value, or keep the waveforms of an ac, dc or tran analysis"));

        // A Monte Carlo at every corner: the specs judged, no values.
        c.analysis = "op";
        c.samples = "50";
        c.seed = "2";
        c.records = {{"gmax", "v(out)"}};
        QVERIFY(!commandLine(c, nullptr, &line, &why));
        QCOMPARE(why, QStringLiteral("a Monte Carlo at every corner needs a spec to judge"));
        c.specs = {{"v(out)", "", "1.2"}};
        QVERIFY2(commandLine(c, nullptr, &line, &why), qPrintable(why));
        QCOMPARE(line, QStringLiteral("corners -list ss,ff,sf -nonominal -mc 50 -seed 2 -analysis \"op\" -spec v(out) -max 1.2"));

        c = Corners();
        c.analysis = "op";
        c.records = {{"x", "v(out)"}};
        c.corners = "s-s";
        QVERIFY(!commandLine(c, nullptr, &line, &why));
        QCOMPARE(why, QStringLiteral("\"s-s\" is not a corner's name"));
        c.corners.clear();
        c.records = {{"corner", "v(out)"}};
        QVERIFY(!commandLine(c, nullptr, &line, &why));
        QCOMPARE(why, QStringLiteral("the name corner is taken by the results"));
        c.records = {{"x", "v(out)"}};
        c.samples = "many";
        QVERIFY(!commandLine(c, nullptr, &line, &why));
        QCOMPARE(why, QStringLiteral("the samples \"many\" are not a whole number"));
    }

    // The properties round trip through a schematic file, and a new
    // component starts from 100 samples / every corner with waveforms.
    void theComponentsAreSavedAndLoaded()
    {
        NgMonteCarlo_Sim mc;
        QCOMPARE(mc.Model, QStringLiteral(".NGMONTECARLO"));
        QCOMPARE(MonteCarlo::read(&mc).samples, QStringLiteral("100"));
        NgCorners_Sim cr;
        QVERIFY(Corners::read(&cr).nominal && Corners::read(&cr).waveforms);

        const QString file = dir.filePath("saved.sch");
        QVERIFY(write(file, lowpass(file,
            "  <.NGMONTECARLO NgMonteCarlo1 1 400 330 0 44 0 0 \"Samples=20\" 1 \"Seed=3\" 0 \"LHS=yes\" 0 "
            "\"ModelStats=no\" 0 \"Analysis=AC1\" 1 \"Record=gain|db(v(out))\" 1 \"Spec=v(out)||0.9\" 1>\n"
            "  <.NGCORNERS NgCorners1 1 400 450 0 44 0 0 \"Analysis=AC1\" 1 \"Corners=ss,ff\" 1 \"Nominal=no\" 0 "
            "\"Waveforms=yes\" 0 \"Samples=\" 0 \"Seed=\" 0 \"ModelStats=no\" 0 \"Record=g|maximum(db(v(out)))\" 1>\n")));
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        const MonteCarlo m = MonteCarlo::read(find(&doc, "NgMonteCarlo1"));
        QCOMPARE(m.samples, QStringLiteral("20"));
        QVERIFY(m.lhs);
        QCOMPARE(m.records.size(), 1);
        QCOMPARE(m.specs.at(0).max, QStringLiteral("0.9"));
        const Corners c = Corners::read(find(&doc, "NgCorners1"));
        QCOMPARE(c.corners, QStringLiteral("ss,ff"));
        QVERIFY(!c.nominal);
        QCOMPARE(c.records.at(0).toString(), QStringLiteral("g|maximum(db(v(out)))"));

        // Written back, what the schematic shows of them is kept.
        Component* comp = find(&doc, "NgMonteCarlo1");
        MonteCarlo changed = m;
        changed.records << Record{"fc", "1/(2*pi*@r1[resistance]*100n)"};
        changed.write(comp);
        QCOMPARE(comp->Props.at(1)->display, false);   // Seed
        QCOMPARE(comp->Props.at(4)->display, true);    // Analysis
        QCOMPARE(MonteCarlo::read(comp).records.size(), 2);
        QVERIFY(doc.save() >= 0);
        Schematic again(nullptr, file);
        QVERIFY(again.loadDocument());
        QCOMPARE(MonteCarlo::read(find(&again, "NgMonteCarlo1")).records.at(1).name, QStringLiteral("fc"));
    }

    // The raw files: padded and unpadded, the dimensions, several plots in
    // one file, complex values.
    void theRawFileIsRead()
    {
        for (const bool padded : {true, false}) {
            const QString file = dir.filePath(padded ? "padded.ngmc" : "unpadded.ngmc");
            QVERIFY(write(file, monteCarloRaw(padded)));
            const QList<RawPlot> plots = readRaw(file);
            QCOMPARE(plots.size(), 1);
            const RawPlot& p = plots.first();
            QCOMPARE(p.name, QStringLiteral("Monte Carlo"));
            QVERIFY(!p.complex);
            QCOMPARE(p.vector("sample")->re, QVector<double>({1, 2, 3}));
            QCOMPARE(p.vector("fc")->re, QVector<double>({10, 20, 30}));
            QCOMPARE(p.vector("frequency")->re, QVector<double>({100, 1000}));
            QCOMPARE(p.vector("gain")->dims, QList<int>({3, 2}));
            QCOMPARE(p.vector("gain")->re, QVector<double>({-1, -2, -3, -4, -5, -6}));
            QCOMPARE(p.vector("montecarlo_yield")->re, QVector<double>({0.5}));
        }
        const QString waves = dir.filePath("two.ngcwaves");
        QVERIFY(write(waves, wavesRaw(2)));
        const QList<RawPlot> plots = readRaw(waves);
        QCOMPARE(plots.size(), 2);
        QVERIFY(plots.at(1).complex);
        QCOMPARE(plots.at(1).vector("v(out)")->re, QVector<double>({1.5, 1.1}));
        QCOMPARE(plots.at(1).vector("v(out)")->im, QVector<double>({-0.1, -0.3}));
        QCOMPARE(plots.at(1).vector("frequency")->re, QVector<double>({100, 1000}));
        QVERIFY(readRaw(dir.filePath("none.raw")).isEmpty());
    }

    // The dataset: the family against the scale and the sample, the
    // scalars with their histograms, the counts; the corners' values and
    // waveforms against the corner.
    void theDatasetIsWritten()
    {
        const QString work = dir.filePath("ds");
        QVERIFY(QDir().mkpath(work));
        QVERIFY(write(work + "/spice4qucs.ngmontecarlo1.ngmc", monteCarloRaw(true)));
        const QString mc = datasetBlocks(work, "spice4qucs.ngmontecarlo1.ngmc");
        QVERIFY2(mc.contains("<indep ngmontecarlo1.sample 3>"), qPrintable(mc));
        QVERIFY2(mc.contains("<indep ngmontecarlo1.frequency 2>"), qPrintable(mc));
        QVERIFY2(mc.contains("<dep ngmontecarlo1.gain ngmontecarlo1.frequency ngmontecarlo1.sample>"), qPrintable(mc));
        QVERIFY2(mc.contains("<dep ngmontecarlo1.fc ngmontecarlo1.sample>"), qPrintable(mc));
        QVERIFY2(mc.contains("<dep ngmontecarlo1.fc_hist ngmontecarlo1.fc_bins>"), qPrintable(mc));
        QVERIFY(!mc.contains("frequency ngmontecarlo1.sample>\n1.0"));   // the scale is no scalar

        const QString file = dir.filePath("mc.dat");
        QVERIFY(write(file, "<Qucs Dataset " PACKAGE_VERSION ">\n" + mc));
        const auto table = qucs_s::optimization::readDataset(file);
        QCOMPARE(table.value("ngmontecarlo1.gain"), QVector<double>({-1, -2, -3, -4, -5, -6}));
        QCOMPARE(table.value("ngmontecarlo1.fc"), QVector<double>({10, 20, 30}));
        QCOMPARE(table.value("ngmontecarlo1.yield"), QVector<double>({0.5}));
        QCOMPARE(table.value("ngmontecarlo1.nfailed"), QVector<double>({1}));
        QCOMPARE(table.value("ngmontecarlo1.fc_hist"), QVector<double>({1, 1, 1}));
        const QVector<double> bins = table.value("ngmontecarlo1.fc_bins");
        QCOMPARE(bins.size(), 3);
        QVERIFY(std::abs(bins.at(0) - 40.0 / 3) < 1e-9 && std::abs(bins.at(2) - 80.0 / 3) < 1e-9);
        QVERIFY(!table.contains("ngmontecarlo1.montecarlo_n"));

        QVERIFY(write(work + "/spice4qucs.ngcorners1.ngcorners", kCornersRaw));
        QVERIFY(write(work + "/spice4qucs.ngcorners1.ngcwaves", wavesRaw(2)));
        const QString cr = datasetBlocks(work, "spice4qucs.ngcorners1.ngcorners");
        QVERIFY2(cr.contains("<indep ngcorners1.corner 2>"), qPrintable(cr));
        QVERIFY2(cr.contains("<dep ngcorners1.gmax ngcorners1.corner>"), qPrintable(cr));
        QVERIFY2(cr.contains("<dep ngcorners1.v(out) ngcorners1.frequency ngcorners1.corner>\n"
                             "5.000000000000e-01-j1.000000000000e-01\n"),
                 qPrintable(cr));
        QVERIFY(!cr.contains("corners_n"));
        QVERIFY(datasetBlocks(work, "spice4qucs.ngcorners1.ngcwaves").isEmpty());   // with the corners' own

        // Waveforms that do not match the corners are left out; the values stay.
        QVERIFY(write(work + "/spice4qucs.ngcorners1.ngcwaves", wavesRaw(3)));
        const QString odd = datasetBlocks(work, "spice4qucs.ngcorners1.ngcorners");
        QVERIFY(odd.contains("ngcorners1.gmax") && !odd.contains("v(out)"));
        // Not the plot it should be (a refused command writes another): nothing.
        QVERIFY(write(work + "/spice4qucs.ngmontecarlo2.ngmc", wavesRaw(1)));
        QVERIFY(datasetBlocks(work, "spice4qucs.ngmontecarlo2.ngmc").isEmpty());
        QVERIFY(isResultFile("spice4qucs.x.ngmc") && isResultFile("spice4qucs.x.ngcwaves"));
        QVERIFY(!isResultFile("spice4qucs.ac1.plot"));
    }

    // The netlist: each component's block after the simulations, the
    // results written where the dataset finds them.
    void theNetlist()
    {
        const QString file = dir.filePath("net.sch");
        QVERIFY(write(file, lowpass(file,
            "  <.NGMONTECARLO NgMonteCarlo1 1 400 330 0 44 0 0 \"Samples=20\" 1 \"Seed=3\" 0 \"LHS=no\" 0 "
            "\"ModelStats=yes\" 0 \"Analysis=TR1\" 1 \"Record=vmax|maximum(v(out))\" 1>\n"
            "  <.NGCORNERS NgCorners1 1 400 450 0 44 0 0 \"Analysis=AC1\" 1 \"Corners=\" 1 \"Nominal=yes\" 0 "
            "\"Waveforms=yes\" 0 \"Samples=\" 0 \"Seed=\" 0 \"ModelStats=no\" 0 \"Record=g|maximum(db(v(out)))\" 1>\n")));
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        const QString netlist = dir.filePath("net.cir");
        {
            Ngspice kernel(&doc);
            kernel.SaveNetlist(netlist, false);
            QCOMPARE(kernel.statistics(), QStringList({"NgMonteCarlo1", "NgCorners1"}));
        }
        const QString text = read(netlist);
        const int ac = text.indexOf("\nac dec");
        const int tran = text.indexOf("\ntran ");
        const int mc = text.indexOf("echo \"qucs-s: begin NgMonteCarlo1\"\nset filetype=ascii\n"
                                    "option interp\noption osdimc\nmontecarlo 20 -seed 3 -analysis \"tran ");
        const int mcEnd = text.indexOf("setplot $montecarlo_plot\nwrite spice4qucs.ngmontecarlo1.ngmc\n"
                                       "option noosdimc\necho \"qucs-s: end NgMonteCarlo1\"\ndestroy all\nreset\n");
        const int cr = text.indexOf("echo \"qucs-s: begin NgCorners1\"\nset filetype=ascii\n"
                                    "corners -analysis \"ac dec");
        const int walk = text.indexOf("setplot $corners_plot\nwrite spice4qucs.ngcorners1.ngcorners\n"
                                      "repeat $corners_n\nsetplot previous\nend\n"
                                      "write spice4qucs.ngcorners1.ngcwaves v(in) v(out)\n");
        const int exit = text.indexOf("\nexit\n");
        QVERIFY2(ac > 0 && tran > 0 && mc > std::max(ac, tran) && mcEnd > mc && cr > mcEnd && walk > cr
                     && exit > walk,
                 qPrintable(text));
        QVERIFY2(text.contains("-output g=maximum(db(v(out)))\n"), qPrintable(text));
        QVERIFY2(text.contains("repeat $&qucs_corners_more\nsetplot next\nwrite spice4qucs.ngcorners1.ngcwaves"),
                 qPrintable(text));

        // A component the netlist cannot write: an error in the output, and
        // the ERC says so beforehand.
        MonteCarlo broken = MonteCarlo::read(find(&doc, "NgMonteCarlo1"));
        broken.records.clear();
        broken.write(find(&doc, "NgMonteCarlo1"));
        {
            Ngspice kernel(&doc);
            kernel.SaveNetlist(netlist, false);
            QCOMPARE(kernel.statistics(), QStringList({"NgCorners1"}));
        }
        const QString refused = read(netlist);
        QVERIFY2(refused.contains("echo \"Error: NgMonteCarlo1: nothing to record and no spec to judge\""),
                 qPrintable(refused));
        QVERIFY(!refused.contains("montecarlo 20"));
        QStringList messages;
        for (const auto& issue : qucs_s::erc::check(&doc))
            if (issue.component == "NgMonteCarlo1") messages << issue.message;
        QCOMPARE(messages, QStringList({"NgMonteCarlo1: nothing to record and no spec to judge"}));

        // Inactive: nothing at all.
        find(&doc, "NgMonteCarlo1")->isActive = COMP_IS_OPEN;
        find(&doc, "NgCorners1")->isActive = COMP_IS_OPEN;
        {
            Ngspice kernel(&doc);
            kernel.SaveNetlist(netlist, false);
        }
        QVERIFY(!read(netlist).contains("qucs-s: begin"));
    }

    // The status log: ngspice's report of each, between its markers.
    void theSummary()
    {
        NgMonteCarlo_Sim mc;
        mc.Name = "NgMonteCarlo1";
        const Summary m = summarize(&mc, kOutput, dir.filePath("nowhere"));
        QCOMPARE(m.text, QStringLiteral("NgMonteCarlo1: 50 Latin-Hypercube samples, analysis 'op', 1 spec, seed 4\n"
                                        "yield : 94.000% (47 / 50 pass)\n"
                                        "95% CI : [83.783%, 97.939%] (Wilson score)\n"
                                        "spec 1 (v(out)): 3 violations"));
        QVERIFY(!m.warning);
        NgCorners_Sim cr;
        cr.Name = "NgCorners1";
        const Summary c = summarize(&cr, kOutput, dir.filePath("nowhere"));
        QCOMPARE(c.text, QStringLiteral("NgCorners1: 4 corners (tt ss ff sf), analysis 'ac dec 2 100 100k'\n"
                                        "tt: gmax=-0.0171115 rr=1000\n"
                                        "ss: gmax=-0.0206964 rr=1100\n"
                                        "ff: gmax=-0.0138655 rr=900\n"
                                        "sf: gmax=-0.0188616 rr=1050"));
        // Nothing between the markers: said.
        cr.Name = "NgCorners2";
        QVERIFY(summarize(&cr, kOutput, dir.path()).warning);
        // An error inside: a warning with the error.
        const Summary e = summarize(&mc, "qucs-s: begin NgMonteCarlo1\nError: montecarlo: -spec needs -min or -max\n"
                                         "qucs-s: end NgMonteCarlo1\n", dir.path());
        QVERIFY(e.warning);
        QVERIFY2(e.text.contains("Error: montecarlo: -spec needs"), qPrintable(e.text));
        QVERIFY(unsupported("montecarlo: no such command available in ngspice", false));
        QVERIFY(!unsupported("montecarlo: no such command available in ngspice", true));
    }

    // The form: what it shows, the command it makes, what it writes back.
    void theDialog()
    {
        const QString file = copyExample(dir.path(), "dialog");
        Schematic doc(nullptr, file);
        QVERIFY(doc.loadDocument());
        Component* mc = find(&doc, "NgMonteCarlo1");
        QVERIFY(mc != nullptr);
        NgStatisticsDialog dialog(mc, &doc);
        QVERIFY(!dialog.corners());
        QCOMPARE(dialog.recordTable()->rowCount(), 2);
        QCOMPARE(dialog.specTable()->rowCount(), 1);
        QString line;
        QVERIFY(commandLine(mc, &doc, &line));
        QCOMPARE(dialog.preview(), line);
        QVERIFY2(line.contains("-analysis \"ac dec 20 100 100k\""), qPrintable(line));
        QVERIFY(dialog.analysisCombo()->findText("AC1") >= 0);
        // For a look: QUCS_TEST_GRAB=<dir> saves a picture of every tab.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        auto grabTabs = [&](NgStatisticsDialog& d, const QString& name) {
            if (grabDir.isEmpty()) return;
            auto* tabs = d.findChild<QTabWidget*>();
            for (int i = 0; i < tabs->count(); ++i) {
                tabs->setCurrentIndex(i);
                d.grab().save(QStringLiteral("%1/%2_%3.png").arg(grabDir, name).arg(i));
            }
            tabs->setCurrentIndex(0);
        };
        grabTabs(dialog, "montecarlo");

        dialog.samplesEdit()->setText("");
        QVERIFY(dialog.preview().startsWith("(not complete"));
        dialog.samplesEdit()->setText("500");
        dialog.seedEdit()->setText("9");
        dialog.addRecord({"vmax", "maximum(v(out))"});
        QVERIFY2(dialog.preview().startsWith("montecarlo 500 -seed 9"), qPrintable(dialog.preview()));
        dialog.nameEdit()->setText("MC");
        QMetaObject::invokeMethod(&dialog, "slotOK");
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(mc->Name, QStringLiteral("MC"));
        const MonteCarlo m = MonteCarlo::read(mc);
        QCOMPARE(m.samples, QStringLiteral("500"));
        QCOMPARE(m.records.size(), 3);
        QCOMPARE(m.records.at(2).toString(), QStringLiteral("vmax|maximum(v(out))"));

        // Corners: a Monte Carlo at every corner judges specs instead of
        // recording values.
        NgCorners_Sim cr;
        cr.Name = "NgCorners1";
        NgStatisticsDialog corners(&cr, &doc);
        QVERIFY(corners.corners());
        QVERIFY(corners.preview().startsWith("(not complete: no analysis"));
        corners.analysisCombo()->setEditText("AC1");
        QVERIFY2(corners.preview().startsWith("corners -analysis"), qPrintable(corners.preview()));
        corners.cornersEdit()->setText("ss ff");
        corners.samplesEdit()->setText("40");
        QVERIFY(!corners.waveformsBox()->isEnabled());
        QVERIFY(corners.seedEdit()->isEnabled());
        QVERIFY(corners.preview().contains("needs a spec"));
        corners.addSpec({"maximum(db(v(out)))", "-1", ""});
        grabTabs(corners, "corners");
        QVERIFY2(corners.preview().startsWith("corners -list ss,ff -mc 40 -analysis"), qPrintable(corners.preview()));
        corners.samplesEdit()->setText("");
        QVERIFY(corners.waveformsBox()->isEnabled());
        QVERIFY(!corners.seedEdit()->isEnabled());
        QMetaObject::invokeMethod(&corners, "slotApply");
        QCOMPARE(Corners::read(&cr).corners, QStringLiteral("ss ff"));
        QMetaObject::invokeMethod(&corners, "slotCancel");
        QCOMPARE(corners.result(), int(QDialog::Accepted));   // Apply changed it
    }

    // Stock ngspice has neither command: the run says what is missing.
    void anNgspiceWithoutTheCommands()
    {
        const QString fake = dir.filePath("stock-ngspice");
        QVERIFY(write(fake, "#!/bin/sh\necho 'montecarlo: no such command available in ngspice'\nexit 0\n"));
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QucsSettings.NgspiceExecutable = fake;
        const QString file = copyExample(dir.path(), "stock");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(simulate(app, file) != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("This ngspice has no montecarlo command: NgMonteCarlo1 needs"), qPrintable(log));
    }

    // With an ngspice that has montecarlo: the example, 200 samples.
    void theExampleRuns()
    {
        if (ngspice.isEmpty()) QSKIP("no ngspice with the montecarlo and corners commands");
        QucsSettings.NgspiceExecutable = ngspice;
        const QString file = copyExample(dir.path(), "example");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(simulate(app, file) != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("NgMonteCarlo1: 200 random samples, analysis 'ac dec 20 100 100k', 1 spec, seed 1"),
                 qPrintable(log));
        QVERIFY2(log.contains("yield :"), qPrintable(log));

        const auto table = qucs_s::optimization::readDataset(dir.filePath("example.dat.ngspice"));
        QCOMPARE(table.value("ngmontecarlo1.sample").size(), 200);
        QCOMPARE(table.value("ngmontecarlo1.frequency").size(), 61);
        QCOMPARE(table.value("ngmontecarlo1.gain").size(), 200 * 61);
        const QVector<double> fc = table.value("ngmontecarlo1.fc");
        QCOMPARE(fc.size(), 200);
        const double mean = std::accumulate(fc.begin(), fc.end(), 0.0) / fc.size();
        QVERIFY2(std::abs(mean - 1591.5) < 60, qPrintable(QString::number(mean)));
        const QVector<double> hist = table.value("ngmontecarlo1.fc_hist");
        QCOMPARE(std::accumulate(hist.begin(), hist.end(), 0.0), 200.0);
        // +/- 10 % around a 7 % sigma: most samples pass, not all.
        const double yield = table.value("ngmontecarlo1.yield").value(0);
        QVERIFY2(yield > 0.7 && yield < 0.98, qPrintable(QString::number(yield)));
        // The ordinary simulation ran as before.
        QVERIFY(!table.value("ac.v(out)").isEmpty());
    }

    // With an ngspice that has corners, and OpenVAF next to it: the corners
    // of a Verilog-A resistor, their values and waveforms, and a Monte
    // Carlo at two of them.
    void theCornersOfAVerilogAModelRun()
    {
        if (ngspice.isEmpty()) QSKIP("no ngspice with the montecarlo and corners commands");
        QString openvaf = QStandardPaths::findExecutable("openvaf-r");
        if (openvaf.isEmpty()) openvaf = QFileInfo(QFileInfo(ngspice).canonicalFilePath()).dir().filePath("openvaf-r");
        if (!QFileInfo(openvaf).isExecutable()) QSKIP("no OpenVAF to compile the model");
        const QString va = dir.filePath("cres.va");
        QVERIFY(write(va, "`include \"disciplines.vams\"\n"
                          "module cres(p, n);\ninout p, n; electrical p, n;\n"
                          "(* std=20, corner=\"ss=+10%, ff=-10%, sf=+5%\" *) parameter real r = 1000 from (0:inf);\n"
                          "analog I(p,n) <+ V(p,n)/r;\nendmodule\n"));
        QProcess compile;
        compile.setWorkingDirectory(dir.path());
        compile.start(openvaf, {"cres.va", "-o", "cres.osdi"});
        QVERIFY(compile.waitForFinished(60000));
        QVERIFY2(QFile::exists(dir.filePath("cres.osdi")), qPrintable(QString::fromUtf8(compile.readAll())));

        QucsSettings.NgspiceExecutable = ngspice;
        const QString file = dir.filePath("corners.sch");
        const QString device = QStringLiteral(
            "  <INCLSCR INCLSCR1 1 160 60 -60 16 0 0 \".control\\npre_osdi %1\\n.endc\\n"
            "N1 in out rmod\\n.model rmod cres r=1000\\n\" 1 \"\" 0 \"\" 0>\n").arg(dir.filePath("cres.osdi"));
        QVERIFY(write(file, lowpass(file,
            "  <.NGCORNERS NgCorners1 1 400 450 0 44 0 0 \"Analysis=AC1\" 1 \"Corners=\" 1 \"Nominal=yes\" 0 "
            "\"Waveforms=yes\" 0 \"Samples=\" 0 \"Seed=\" 0 \"ModelStats=no\" 0 "
            "\"Record=gmax|maximum(db(v(out)))\" 1 \"Record=rr|@rmod[r]\" 1>\n"
            "  <.NGCORNERS NgCorners2 1 400 550 0 44 0 0 \"Analysis=op\" 1 \"Corners=ss,ff\" 1 \"Nominal=no\" 0 "
            "\"Waveforms=no\" 0 \"Samples=20\" 0 \"Seed=2\" 0 \"ModelStats=yes\" 0 \"Spec=@rmod[r]|950|\" 1>\n",
            device)));
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(simulate(app, file) != nullptr);
        const QString log = logText(app);
        QVERIFY2(log.contains("NgCorners1: 4 corners (tt ss ff sf)"), qPrintable(log));
        QVERIFY2(log.contains("ss: gmax=") && log.contains("rr=1100"), qPrintable(log));
        QVERIFY2(log.contains("NgCorners2: 2 corners (ss ff), montecarlo 20 per corner"), qPrintable(log));

        const auto table = qucs_s::optimization::readDataset(dir.filePath("corners.dat.ngspice"));
        QCOMPARE(table.value("ngcorners1.corner"), QVector<double>({0, 1, 2, 3}));
        QCOMPARE(table.value("ngcorners1.rr"), QVector<double>({1000, 1100, 900, 1050}));
        const QVector<double> gmax = table.value("ngcorners1.gmax");
        QCOMPARE(gmax.size(), 4);
        QVERIFY(gmax.at(1) < gmax.at(0) && gmax.at(0) < gmax.at(2));   // ss slower, ff faster
        const int points = int(table.value("ngcorners1.frequency").size());
        QCOMPARE(points, 7);
        const QVector<double> wave = table.value("ngcorners1.v(out)");
        QCOMPARE(wave.size(), 4 * points);
        QVERIFY(wave.at(points - 1) > wave.at(2 * points - 1));   // at the top: tt above ss
        // ss (r at 1100 +/- 20) always passes r >= 950; ff (900) never does.
        QCOMPARE(table.value("ngcorners2.yield"), QVector<double>({1, 0}));
        QCOMPARE(table.value("ngcorners2.nsamples"), QVector<double>({20, 20}));
    }
};

QTEST_MAIN(TestNgStats)
#include "test_ngstats.moc"
