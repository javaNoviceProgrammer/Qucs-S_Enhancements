/*
 * test_diagram_tools.cpp - the diagrams of 7 October through the tools
 * (Qucs-S_missing_diagrams_2026-10-03.md): add_diagram and edit_diagram
 * with their types and their own settings, the traces' places in them,
 * the markers and what get_schematic, get_dataset and export_image say of
 * them. The datasets are written beside the schematics: nothing is run.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>
#include <functional>

#include "config.h"
#include "diagrams/diagram.h"
#include "diagrams/marker.h"
#include "diagrams/stackeddiagram.h"
#include "mouseactions.h"
#include "probe.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/markerdialog.h"
#include "diagrams/speclimits.h"
#include "diagrams/polezerodiagram.h"
#include "diagrams/bodediagram.h"
#include "diagrams/nicholsdiagram.h"
#include "diagrams/polardiagram.h"
#include "diagrams/spectrumdiagram.h"
#include "diagrams/bathtubdiagram.h"
#include "diagrams/levelhistogramdiagram.h"
#include "diagrams/histogramdiagram.h"
#include "diagrams/contourdiagram.h"
#include "diagrams/spectrogramdiagram.h"
#include <QDoubleSpinBox>
#include "diagrams/tornadodiagram.h"
#include "diagrams/boxplotdiagram.h"
#include "diagrams/constellationdiagram.h"
#include "diagrams/smithdiagram.h"
#include <QPushButton>
#include "eyeanalysis.h"
#include "cursorvalues.h"
#include "paintings/buspainting.h"
#include "ltspiceimport.h"
#include "erc.h"
#include <QLineEdit>
#include <QSpinBox>
#include <random>
#include "spectrum.h"
#include "valuereading.h"
#include <complex>
#include "statusbar.h"
#include <QCheckBox>
#include <QComboBox>
#include <QPainter>
#include <QTableWidget>
#include "wire.h"
#include "wirelabel.h"
#include "node.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "settings.h"
#include <QScopeGuard>
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"

namespace {

// A Qucs dataset: the independent variable and its values, then each
// dependent one's (complex as re, im pairs when \a im is given).
struct Dep {
    QString name;
    std::function<double(double)> re;
    std::function<double(double)> im = nullptr;
};

QString datasetText(const QString& indep, const QVector<double>& xs, const QList<Dep>& deps)
{
    QString s = QStringLiteral("<Qucs Dataset " PACKAGE_VERSION ">\n<indep %1 %2>\n").arg(indep).arg(xs.size());
    for (double x : xs) s += QString::number(x, 'g', 17) + "\n";
    s += "</indep>\n";
    for (const Dep& d : deps) {
        s += QStringLiteral("<dep %1 %2>\n").arg(d.name, indep);
        for (double x : xs) {
            const double re = d.re(x);
            if (d.im) {
                const double im = d.im(x);
                s += QString::number(re, 'g', 17) + (im < 0 ? "-j" : "+j") + QString::number(std::fabs(im), 'g', 17) + "\n";
            } else {
                s += QString::number(re, 'g', 17) + "\n";
            }
        }
        s += "</dep>\n";
    }
    return s;
}

// A data signal's corners: PRBS7 NRZ (or PAM4 of random symbols) at \a ui,
// edges a fifth of it long, each transition's crossing moved by a Dirac
// (+dj/2 or -dj/2, as the bit two before it is) and a Gaussian of rj.
struct Signal {
    QVector<double> t, v;
    double at(double x) const
    {
        const int i = int(std::lower_bound(t.cbegin(), t.cend(), x) - t.cbegin());
        if (i >= t.size()) return v.last();
        if (t.at(i) == x || i == 0) return v.at(i);
        return v.at(i - 1) + (x - t.at(i - 1)) / (t.at(i) - t.at(i - 1)) * (v.at(i) - v.at(i - 1));
    }
};

Signal dataSignal(double ui, int symbols, double rj, double dj, bool pam4 = false)
{
    constexpr double Pi = 3.14159265358979323846;
    std::mt19937 random(7);
    const auto uniform = [&] { return (double(random()) + 0.5) / 4294967296.0; };
    const auto gaussian = [&] { return std::sqrt(-2 * std::log(uniform())) * std::cos(2 * Pi * uniform()); };
    QVector<int> b;
    unsigned lfsr = 0x7f;
    for (int i = 0; i < symbols; ++i) {
        if (pam4) {
            b << int(random() % 4);
            continue;
        }
        const unsigned bit = ((lfsr >> 6) ^ (lfsr >> 5)) & 1u;
        lfsr = ((lfsr << 1) | bit) & 0x7fu;
        b << int(bit);
    }
    Signal s;
    const double edge = 0.2 * ui;
    s.t << 0;
    s.v << b.first();
    for (int i = 1; i < symbols; ++i) {
        if (b.at(i) == b.at(i - 1)) continue;
        const double at = i * ui + (i >= 2 && b.at(i - 2) % 2 ? dj / 2 : -dj / 2) + rj * gaussian();
        s.t << at - edge / 2 << at + edge / 2;
        s.v << b.at(i - 1) << b.at(i);
    }
    s.t << symbols * ui;
    s.v << b.last();
    return s;
}

// A data signal with noise on its levels: each symbol's value - its
// level, plus \a sigma (\a sigmaHigh on the upper levels, when given) times
// a Gaussian - held from a tenth of its UI to nine tenths, the edges
// between: sampled at the UI's middle, the value itself.
Signal noisySignal(double ui, int symbols, double sigma, bool pam4 = false, double sigmaHigh = -1.0, double gain = 1.0, double offset = 0.0)
{
    constexpr double Pi = 3.14159265358979323846;
    std::mt19937 random(11);
    const auto uniform = [&] { return (double(random()) + 0.5) / 4294967296.0; };
    const auto gaussian = [&] { return std::sqrt(-2 * std::log(uniform())) * std::cos(2 * Pi * uniform()); };
    unsigned lfsr = 0x7f;
    Signal s;
    for (int i = 0; i < symbols; ++i) {
        int level = 0;
        if (pam4) {
            level = int(random() % 4);
        } else {
            const unsigned bit = ((lfsr >> 6) ^ (lfsr >> 5)) & 1u;
            lfsr = ((lfsr << 1) | bit) & 0x7fu;
            level = int(bit);
        }
        const bool upper = pam4 ? level >= 2 : level == 1;
        const double v = offset + gain * level + (upper && sigmaHigh >= 0 ? sigmaHigh : sigma) * gaussian();
        s.t << (i + 0.1) * ui << (i + 0.9) * ui;
        s.v << v << v;
    }
    return s;
}

// A dataset of variables over two sweeps: x the inner one (fastest).
QString datasetText2(const QString& xName, const QVector<double>& xs, const QString& yName, const QVector<double>& ys,
                     const QList<QPair<QString, std::function<double(double, double)>>>& deps)
{
    QString s = QStringLiteral("<Qucs Dataset " PACKAGE_VERSION ">\n<indep %1 %2>\n").arg(xName).arg(xs.size());
    for (double x : xs) s += QString::number(x, 'g', 17) + "\n";
    s += QStringLiteral("</indep>\n<indep %1 %2>\n").arg(yName).arg(ys.size());
    for (double y : ys) s += QString::number(y, 'g', 17) + "\n";
    s += "</indep>\n";
    for (const auto& d : deps) {
        s += QStringLiteral("<dep %1 %2 %3>\n").arg(d.first, xName, yName);
        for (double y : ys)
            for (double x : xs) s += QString::number(d.second(x, y), 'g', 17) + "\n";
        s += "</dep>\n";
    }
    return s;
}

QVector<double> range(double from, double to, int n)
{
    QVector<double> xs;
    for (int i = 0; i < n; ++i) xs << from + (to - from) * i / (n - 1);
    return xs;
}

} // namespace

class TestDiagramTools : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 60000)
    {
        return control->callNow(tool, args, timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r)
    {
        QString first;
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                first = v.toObject().value(QStringLiteral("text")).toString();
                break;
            }
        return QJsonDocument::fromJson(first.toUtf8()).object();
    }
    Schematic* front() const { return app->currentSchematic(); }
    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    bool writeFile(const QString& file, const QString& text)
    {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(text.toUtf8());
        return true;
    }
    // An empty schematic \a name.sch, open in front, its dataset (ngspice's)
    // \a dataset.
    bool schematicWith(const QString& name, const QString& dataset)
    {
        const QString sch = path(name + ".sch");
        if (!writeFile(sch, QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
                                           "<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n"
                                           "<Paintings>\n</Paintings>\n")))
            return false;
        if (!writeFile(path(name + ".dat.ngspice"), dataset)) return false;
        return !failed(call("open_document", {{"path", sch}}));
    }
    // The netlist's words: what a simulation would be given now.
    QString get_netlistNames() { return text(call("get_netlist", {})); }
    // With ngspice, for the tests that simulate: false when there is none.
    bool withNgspice()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) return false;
        QucsSettings.NgspiceExecutable = ngspice;
        return true;
    }
    void closeAll()
    {
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        QucsSettings.font.setPointSize(12);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
    }

    void cleanupTestCase()
    {
        closeAll();
        delete app;
        QucsMain = nullptr;
    }

    void cleanup() { closeAll(); }

    // ---- Stacked panes: made with its panes and each trace's, a marker
    // reading every pane, refused where it does not apply, kept in the file.
    void stackedPanes()
    {
        const QVector<double> t = range(0, 5e-3, 101);
        QVERIFY(schematicWith("panes", datasetText("time", t, {{"tran.v(in)", [](double x) { return std::fmod(x, 2e-3) < 1e-3 ? 1.0 : 0.0; }},
                                                                {"tran.v(out)", [](double x) { return 1 - std::exp(-x / 1e-3); }},
                                                                {"tran.i(v1)", [](double x) { return 1e-3 * std::exp(-x / 1e-3); }}})));
        QJsonObject r = call("add_diagram", {{"type", "stacked"}, {"x", 100}, {"y", 600}, {"width", 400}, {"height", 360},
                                             {"panes", QJsonArray{QJsonObject{{"y_axis", QJsonObject{{"label", "input"}}}},
                                                                  QJsonObject{{"y_axis", QJsonObject{{"label", "output"}}},
                                                                              {"y2_axis", QJsonObject{{"label", "current"}, {"from", 0}, {"to", 2e-3}}}}}},
                                             {"traces", QJsonArray{"tran.v(in)", QJsonObject{{"variable", "tran.v(out)"}, {"pane", 2}},
                                                                   QJsonObject{{"variable", "tran.i(v1)"}, {"pane", 2}, {"axis", "right"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject d = json(r);
        QCOMPARE(d.value("type").toString(), QString("stacked"));
        const QJsonArray panes = d.value("panes").toArray();
        QCOMPARE(panes.size(), 2);
        QCOMPARE(panes.at(0).toObject().value("y_axis").toObject().value("label").toString(), QString("input"));
        QCOMPARE(panes.at(1).toObject().value("y2_axis").toObject().value("to").toDouble(), 2e-3);
        QVERIFY(!d.contains("y_axis"));   // its y axes are the panes'
        const QJsonArray traces = d.value("traces").toArray();
        QCOMPARE(traces.at(0).toObject().value("pane").toInt(), 1);
        QCOMPARE(traces.at(1).toObject().value("pane").toInt(), 2);
        QCOMPARE(traces.at(2).toObject().value("axis").toString(), QString("right"));
        QCOMPARE(traces.at(2).toObject().value("points").toInt(), 101);

        // A marker on v(out) reads v(in) and i(v1) where it is.
        r = call("add_marker", {{"trace", 2}, {"at", 1.5e-3}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString markerText = json(r).value("text").toString();
        QVERIFY2(markerText.contains("tran.v(in): ") && markerText.contains("tran.i(v1): "), qPrintable(markerText));

        // Refused where it does not apply, with what does.
        r = call("edit_diagram", {{"y_axis", QJsonObject{{"label", "x"}}}});
        QVERIFY(failed(r) && text(r).contains("'panes'"));
        r = call("edit_trace", {{"trace", 1}, {"pane", 3}});
        QVERIFY2(failed(r) && text(r).contains("2 pane(s)"), qPrintable(text(r)));
        r = call("edit_diagram", {{"panes", 9}});
        QVERIFY(failed(r) && text(r).contains("1 to 8"));
        r = call("edit_diagram", {{"panes", QJsonArray{QJsonObject{{"x_axis", QJsonObject{}}}}}});
        QVERIFY2(failed(r) && text(r).contains("panes[0] has no x_axis"), qPrintable(text(r)));

        // Three panes, a trace moved to the third.
        QVERIFY(!failed(call("edit_diagram", {{"panes", 3}})));
        r = call("edit_trace", {{"trace", 1}, {"pane", 3}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* stacked = dynamic_cast<StackedDiagram*>(front()->a_DocDiags.front());
        QVERIFY(stacked);
        QCOMPARE(stacked->paneCount(), 3);
        QCOMPARE(stacked->Graphs.at(0)->pane, 2);
        QCOMPARE(stacked->pane(1).left.Label, QString("output"));   // the others as they were

        // Kept in the file.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("panes.sch")}})));
        d = json(call("get_schematic", {})).value("diagrams").toArray().first().toObject();
        QCOMPARE(d.value("panes").toArray().size(), 3);
        QCOMPARE(d.value("traces").toArray().at(0).toObject().value("pane").toInt(), 3);
        QCOMPARE(d.value("panes").toArray().at(1).toObject().value("y2_axis").toObject().value("label").toString(), QString("current"));

        // A rect diagram has no panes.
        r = call("add_diagram", {{"type", "rect"}, {"panes", 2}});
        QVERIFY(failed(r) && text(r).contains("type stacked"));
        r = call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{QJsonObject{{"variable", "tran.v(in)"}, {"pane", 1}}}}});
        QVERIFY(failed(r) && text(r).contains("stacked diagram's trace"));
    }

    // ---- Cross-probing: a net's voltage, a pin's current, a part's power
    // into a diagram; what the next run saves; an unnamed net labelled; a
    // selected trace lighting up its net or its part.
    void crossProbing()
    {
        const QVector<double> t = range(0, 1e-3, 11);
        QJsonObject r = call("import_netlist", {{"text", "rc\nV1 in 0 PULSE(0 1 0 1u 1u 1 2)\nR1 in out 1k\nC1 out 0 1u\n.tran 10u 1m\n.end"},
                                                {"save_as", path("probe.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(writeFile(path("probe.dat.ngspice"),
                          datasetText("time", t, {{"tran.v(in)", [](double) { return 1.0; }},
                                                  {"tran.v(out)", [](double x) { return 1 - std::exp(-x / 1e-3); }},
                                                  {"tran.i(v1)", [](double x) { return -1e-3 * std::exp(-x / 1e-3); }}})));
        // A net by its label: its voltage, in a new diagram (there was none).
        r = call("probe", {{"what", "out"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("variable").toString(), QString("ngspice/tran.v(out)"));
        QVERIFY(o.value("has data").toBool() && o.value("new diagram").toBool());
        QCOMPARE(o.value("trace").toObject().value("net").toString(), QString("out"));
        QCOMPARE(o.value("trace").toObject().value("points").toInt(), 11);
        // Again: there already - with its data (it said none: bug hunt of
        // 2026-10-08, B6).
        o = json(call("probe", {{"what", "out"}}));
        QVERIFY(o.value("already there").toBool() && o.value("has data").toBool());
        QVERIFY2(!o.value("text").toString().contains("Simulate"), qPrintable(o.value("text").toString()));
        // A pin: the current into it, saved by the next run.
        o = json(call("probe", {{"what", "R1.1"}}));
        QCOMPARE(o.value("variable").toString(), QString("ngspice/tran.@r1[i]"));
        QVERIFY(!o.value("has data").toBool());
        QCOMPARE(o.value("saved by the next run").toString(), QString("@r1[i]"));
        QCOMPARE(o.value("trace").toObject().value("part").toString(), QString("r1"));
        // A part: its power. A source's pin: its branch, which the data has.
        o = json(call("probe", {{"what", "C1"}}));
        QCOMPARE(o.value("variable").toString(), QString("ngspice/tran.@c1[p]"));
        o = json(call("probe", {{"what", "V1.1"}}));
        QCOMPARE(o.value("variable").toString(), QString("ngspice/tran.i(v1)"));
        QVERIFY(o.value("has data").toBool() && !o.contains("saved by the next run"));
        QCOMPARE(front()->getProbeSaves(), QStringList({"@r1[i]", "@c1[p]"}));
        // The netlist saves them.
        const QString netlist = text(call("get_netlist", {}));
        QVERIFY2(netlist.contains(".options savecurrents") && netlist.contains(".save all @c1[p]"), qPrintable(netlist));
        bool written = false;
        for (const QString& line : netlist.split('\n'))
            written = written || (line.startsWith("write ") && line.contains("@r1[i]") && line.contains("@c1[p]"));
        QVERIFY2(written, qPrintable(netlist));
        // Once the run wrote it, the trace reads it (an @ that begins a
        // device's vector is no "plotted against").
        QVERIFY(writeFile(path("probe.dat.ngspice"),
                          datasetText("time", t, {{"tran.v(out)", [](double x) { return 1 - std::exp(-x / 1e-3); }},
                                                  {"tran.@r1[i]", [](double x) { return 1e-3 * std::exp(-x / 1e-3); }}})));
        QVERIFY(!failed(call("reload_data", {})));
        Diagram* d = front()->a_DocDiags.front();
        QCOMPARE(int(d->Graphs.at(1)->count(0)), 11);
        // A dataset an earlier Qucs-S wrote of a stock ngspice's run, where
        // the current is i(@r1[i]): read all the same.
        QCOMPARE(Graph::otherSpelling("tran.@r1[i]"), QString("tran.i(@r1[i])"));
        QCOMPARE(Graph::otherSpelling("tran.i(@r1[i])"), QString("tran.@r1[i]"));
        QCOMPARE(Graph::otherSpelling("@q1[ic]"), QString("i(@q1[ic])"));
        QVERIFY(writeFile(path("probe.dat.ngspice"),
                          datasetText("time", t, {{"tran.v(out)", [](double x) { return 1 - std::exp(-x / 1e-3); }},
                                                  {"tran.i(@r1[i])", [](double x) { return 1e-3 * std::exp(-x / 1e-3); }}})));
        QVERIFY(!failed(call("reload_data", {})));
        QCOMPARE(int(d->Graphs.at(1)->count(0)), 11);
        QCOMPARE(Graph::plotVsSeparator("tran.@r1[i]"), -1);
        QCOMPARE(Graph::plotVsSeparator("@r1[i]"), -1);
        QCOMPARE(Graph::plotVsSeparator("v(out)@v(in)"), 6);
        QCOMPARE(Graph::plotVsSeparator("tran.@r1[i]@time"), 11);
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("probe.sch")}})));
        QCOMPARE(front()->getProbeSaves(), QStringList({"@r1[i]", "@c1[p]"}));

        // A selected trace lights up its net, or its part.
        d = front()->a_DocDiags.front();
        QVERIFY(front()->selectedNet().empty());
        d->Graphs.at(0)->isSelected = true;   // v(out)
        Schematic::Net net = front()->selectedNet();
        QVERIFY(!net.wires.empty() || !net.nodes.empty());
        QVERIFY(net.parts.empty());
        d->Graphs.at(0)->isSelected = false;
        d->Graphs.at(1)->isSelected = true;   // @r1[i]
        net = front()->selectedNet();
        QCOMPARE(int(net.parts.size()), 1);
        QCOMPARE((*net.parts.begin())->Name, QString("R1"));
        d->Graphs.at(1)->isSelected = false;
        QCOMPARE(qucs_s::probe::netOfVariable("ngspice/tran.v(out)"), QString("out"));
        QCOMPARE(qucs_s::probe::netOfVariable("out.Vt"), QString("out"));
        QCOMPARE(qucs_s::probe::partOfVariable("ngspice/tran.@q1[ic]"), QString("q1"));
        QCOMPARE(qucs_s::probe::partOfVariable("ngspice/ac.i(v1)"), QString("v1"));
        QVERIFY(qucs_s::probe::partOfVariable("ngspice/tran.v(out)").isEmpty());

        // Which diagram: the one probed last, the selected one, the one named.
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}})));
        QCOMPARE(json(call("probe", {{"what", "in"}})).value("diagram").toInt(), 1);
        front()->a_DocDiags.back()->isSelected = true;
        QCOMPARE(json(call("probe", {{"what", "in"}})).value("diagram").toInt(), 2);
        front()->a_DocDiags.back()->isSelected = false;
        QCOMPARE(json(call("probe", {{"what", "out"}, {"diagram", 1}})).value("diagram").toInt(), 1);   // not the last probed
        QCOMPARE(json(call("probe", {{"what", "out"}, {"diagram", 2}})).value("diagram").toInt(), 2);
        QVERIFY(failed(call("probe", {{"what", "out"}, {"diagram", 7}})));

        // The window's Probe: a click on a pin's end, as the tool.
        app->probeAction->trigger();
        QCOMPARE(app->MousePressAction, &MouseActions::MPressProbe);
        Component* c1 = front()->getComponentByName("C1");
        QVERIFY(c1);
        // (Into diagram 2, probed into last.)
        Diagram* d2 = front()->a_DocDiags.back();
        const int before = int(d2->Graphs.size());
        app->view->MPressProbe(front(), nullptr, float(c1->cx + c1->Ports.at(0)->x), float(c1->cy + c1->Ports.at(0)->y));
        QCOMPARE(int(d2->Graphs.size()), before + 1);
        QCOMPARE(d2->Graphs.last()->Var, QString("ngspice/tran.@c1[i]"));
        app->probeAction->trigger();   // off
    }

    // An unnamed net is labelled (one step to undo); ground is refused; a
    // probe into a stacked diagram goes into its bottom pane.
    void probingAnUnnamedNet()
    {
        const QString sch = path("divider.sch");
        QVERIFY(writeFile(sch, QStringLiteral(
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n"
            "  <Vdc V1 1 100 200 18 -26 0 1 \"2 V\" 1>\n"
            "  <R R1 1 200 140 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <R R2 1 300 200 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <GND * 1 100 230 0 0 0 0>\n  <GND * 1 300 230 0 0 0 0>\n"
            "  <.TR TR1 1 100 300 0 51 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"11\" 0 \"Trapezoidal\" 0 \"2\" 0 \"1 ns\" 0 \"1e-16\" 0 \"150\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"26.85\" 0 \"1e-3\" 0 \"1e-6\" 0 \"1\" 0 \"CroutLU\" 0 \"no\" 0 \"yes\" 0 \"0\" 0>\n"
            "</Components>\n<Wires>\n  <100 170 100 140 \"\" 0 0 0 \"\">\n  <100 140 170 140 \"\" 0 0 0 \"\">\n"
            "  <230 140 300 140 \"\" 0 0 0 \"\">\n  <300 140 300 170 \"\" 0 0 0 \"\">\n  <300 230 360 230 \"\" 0 0 0 \"\">\n</Wires>\n"
            "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")));
        QVERIFY(!failed(call("open_document", {{"path", sch}})));
        QJsonObject r = call("probe", {{"what", QJsonArray{265, 140}}});   // the wire between R1 and R2
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject o = json(r);
        // (probe1: net1, net2 ... are the names get_schematic gives the nets
        // without a label - another net was called so too, bug hunt of
        // 2026-10-08, B3.)
        QCOMPARE(o.value("labelled").toString(), QString("probe1"));
        QCOMPARE(o.value("variable").toString(), QString("ngspice/tran.v(probe1)"));
        bool labelled = false;
        for (Wire* w : front()->a_DocWires) labelled = labelled || (w->hasLabel() && w->label()->Name == "probe1");
        QVERIFY(labelled);
        QVERIFY(get_netlistNames().contains("probe1"));
        QStringList netNames;
        for (const QJsonValue& n : json(call("get_schematic", {})).value("nets").toArray()) netNames << n.toObject().value("net").toString();
        QCOMPARE(netNames.count("probe1"), 1);
        QCOMPARE(netNames.count("net1"), 1);
        // One step to undo: label, diagram and trace.
        QVERIFY(!failed(call("undo", {})));
        labelled = false;
        for (Wire* w : front()->a_DocWires) labelled = labelled || w->hasLabel();
        QVERIFY(!labelled);
        QVERIFY(front()->a_DocDiags.empty());
        // Ground: 0 V, refused.
        r = call("probe", {{"what", QJsonArray{340, 230}}});   // a wire of ground
        QVERIFY2(failed(r) && text(r).contains("ground"), qPrintable(text(r)));
        // Into a stacked diagram: its bottom pane.
        QVERIFY(!failed(call("add_diagram", {{"type", "stacked"}, {"panes", 3}})));
        r = call("probe", {{"what", QJsonArray{265, 140}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("trace").toObject().value("pane").toInt(), 3);
        // Another unnamed net: a name not taken.
        r = call("probe", {{"what", QJsonArray{130, 140}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("labelled").toString(), QString("probe2"));
        // The nodes keep the names they had (a DC bias shown: its values).
        for (Node* n : front()->a_DocNodes) n->Name = "1.5 V";
        QVERIFY(!failed(call("probe", {{"what", "R1.2"}})));
        for (Node* n : front()->a_DocNodes) QCOMPARE(n->Name, QString("1.5 V"));
    }

    // ---- Delta markers: Δx, Δy and 1/Δx from another marker; followed
    // when it moves; kept with the schematic; chosen in the marker dialog.
    void deltaMarkers()
    {
        const QVector<double> t = range(0, 5e-3, 51);
        QVERIFY(schematicWith("delta", datasetText("time", t, {{"tran.v(out)", [](double x) { return 1000 * x; }}})));
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"tran.v(out)"}}})));
        QVERIFY(!failed(call("add_marker", {{"trace", 1}, {"at", 1e-3}})));
        QJsonObject r = call("add_marker", {{"trace", 1}, {"at", 3e-3}, {"relative_to", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("relative to").toInt(), 1);
        QVERIFY(std::abs(o.value("delta").toObject().value("x").toDouble() - 2e-3) < 1e-12);
        QVERIFY(std::abs(o.value("delta").toObject().value("y").toDouble() - 2.0) < 1e-9);
        QVERIFY(std::abs(o.value("delta").toObject().value("1/x").toDouble() - 500) < 1e-6);
        QVERIFY2(o.value("text").toString().contains(QString::fromUtf8("Δx: ")), qPrintable(o.value("text").toString()));
        // The reference moved: the delta follows.
        QVERIFY(!failed(call("edit_marker", {{"marker", 1}, {"at", 2e-3}})));
        Diagram* d = front()->a_DocDiags.front();
        QVERIFY2(d->markers().at(1)->Text.contains(QString::fromUtf8("Δx: 1.000m")), qPrintable(d->markers().at(1)->Text));
        // Itself, or one not there: refused.
        QVERIFY(failed(call("edit_marker", {{"marker", 2}, {"relative_to", 2}})));
        QVERIFY(failed(call("edit_marker", {{"marker", 2}, {"relative_to", 5}})));
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("delta.sch")}})));
        o = json(call("get_schematic", {})).value("diagrams").toArray().first().toObject();
        QCOMPARE(o.value("markers").toArray().at(1).toObject().value("relative to").toInt(), 1);
        // null: none.
        r = call("edit_marker", {{"marker", 2}, {"relative_to", QJsonValue::Null}});
        QVERIFY(!json(r).contains("relative to"));
        // The marker dialog: another marker of the diagram, or none.
        d = front()->a_DocDiags.front();
        Marker* m2 = d->markers().at(1);
        auto* dialog = new MarkerDialog(m2, nullptr);
        auto* box = dialog->findChild<QComboBox*>("markerRelativeTo");
        QVERIFY(box);
        QCOMPARE(box->count(), 2);   // none, marker 1
        box->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotAcceptValues"));
        QCOMPARE(m2->reference(), d->markers().at(0));
        delete dialog;
        // A reference deleted: no delta, nothing dangling.
        QVERIFY(!failed(call("delete_marker", {{"marker", 1}})));
        QVERIFY(front()->a_DocDiags.front()->markers().first()->reference() == nullptr);
    }

    // ---- Spec limits: a level and a piecewise mask with a step, the
    // verdict and where it fails, drawn, said by check_schematic, edited
    // in the dialog, kept in the file; a stacked diagram's by pane.
    void specLimits()
    {
        namespace lm = qucs_s::limits;
        // The limit itself: straight between points, a step's stricter
        // side, a level everywhere, nothing outside; logarithmic axes.
        lm::Limit mask;
        mask.points = {QPointF(1, 0), QPointF(2, 0), QPointF(2, -40), QPointF(3, -40)};
        QCOMPARE(mask.at(1.5), 0.0);
        QCOMPARE(mask.at(2), -40.0);   // upper: the stricter of the step
        QVERIFY(std::isnan(mask.at(0.5)) && std::isnan(mask.at(3.5)));
        mask.side = lm::Limit::Lower;
        QCOMPARE(mask.at(2), 0.0);
        // A mask ending in its step: the step alone there.
        lm::Limit ending;
        ending.points = {QPointF(1, 0), QPointF(2, 0), QPointF(2, -40)};
        QCOMPARE(ending.at(2), -40.0);
        ending.side = lm::Limit::Lower;
        QCOMPARE(ending.at(2), 0.0);
        lm::Limit level;
        level.points = {QPointF(0, 1.5)};
        QCOMPARE(level.at(-1e9), 1.5);
        lm::Limit slope;
        slope.points = {QPointF(10, 0), QPointF(1000, -40)};
        QCOMPARE(slope.at(100), -40.0 * 90 / 990);            // linear in x
        QVERIFY(std::abs(slope.at(100, true) - (-20)) < 1e-9);   // a decade of two on a log x axis
        lm::Limit back;
        QVERIFY(lm::Limit::load(mask.save(), &back) && back == mask);
        QVERIFY(!lm::Limit::load("<Limit 0 0 0 \"\" 2,1;1,1>", &back));   // x falling

        // On data: v = 1000 t from 0 to 5 ms (0 to 5 V).
        const QVector<double> t = range(0, 5e-3, 51);
        QVERIFY(schematicWith("limits", datasetText("time", t, {{"tran.v(out)", [](double x) { return 1000 * x; }}})));
        QJsonObject r = call("add_diagram", {{"type", "rect"}, {"x", 100}, {"y", 500}, {"width", 400}, {"height", 300},
                                             {"traces", QJsonArray{"tran.v(out)"}},
                                             {"limits", QJsonArray{QJsonObject{{"upper", 4}, {"label", "max"}},
                                                                   QJsonObject{{"lower", QJsonArray{QJsonArray{1e-3, 0.5}, QJsonArray{2e-3, 0.5},
                                                                                                    QJsonArray{2e-3, 3}, QJsonArray{3e-3, 3}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject verdict = json(r).value("verdict").toObject();
        QVERIFY(!verdict.value("pass").toBool());
        const QJsonArray beyond = verdict.value("beyond").toArray();
        QCOMPARE(beyond.size(), 2);
        // Above 4 V from 4 ms on, by 1 V at worst at 5 ms; below the mask's
        // 3 V from its step at 2 ms to 3 ms, by 1 V at 2 ms.
        QJsonObject above = beyond.at(0).toObject(), below = beyond.at(1).toObject();
        QCOMPARE(above.value("label").toString(), QString("max"));
        QVERIFY(std::abs(above.value("from").toDouble() - 4e-3) < 1e-9 && std::abs(above.value("to").toDouble() - 5e-3) < 1e-12);
        QVERIFY(std::abs(above.value("worst").toDouble() - 1) < 1e-9 && std::abs(above.value("at").toDouble() - 5e-3) < 1e-12);
        QCOMPARE(below.value("limit").toInt(), 2);
        QVERIFY(std::abs(below.value("from").toDouble() - 2e-3) < 1e-9 && std::abs(below.value("to").toDouble() - 3e-3) < 1e-9);
        QVERIFY(std::abs(below.value("worst").toDouble() - 1) < 1e-9);
        // check_schematic says it, a run's check before it does not.
        bool said = false;
        for (const QJsonValue& w : json(call("check_schematic", {})).value("warnings").toArray())
            said = said || w.toObject().value("message").toString().startsWith("diagram 1: tran.v(out) is beyond its upper limit max");
        QVERIFY(said);
        // Drawn: the trace red where beyond, the verdict in the corner.
        Diagram* d = front()->a_DocDiags.front();
        QImage img(d->x2 + 200, d->y2 + 100, QImage::Format_RGB32);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            p.translate(100 - d->cx, 50 + d->y2 - d->cy);
            d->paintDiagram(&p);
        }
        const auto redNear = [&](double x, double y) {
            float px = 0, py = 0;
            const double yd[2] = {y, 0};
            d->calcCoordinate(&x, yd, nullptr, &px, &py, &d->yAxis);
            int n = 0;
            for (int dx = -3; dx <= 3; ++dx)
                for (int dy = -3; dy <= 3; ++dy) {
                    const QColor c = img.pixelColor(100 + int(px) + dx, 50 + d->y2 - int(py) + dy);
                    if (c.red() > 150 && c.green() < 90 && c.blue() < 90) ++n;
                }
            return n;
        };
        QVERIFY(redNear(4.5e-3, 4.5) > 0);   // beyond: red
        QCOMPARE(redNear(3.5e-3, 3.5), 0);   // within: its colour
        // Replaced, and cleared.
        r = call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", 10}}}}});
        QVERIFY(json(r).value("verdict").toObject().value("pass").toBool());
        QVERIFY(failed(call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", 1}, {"lower", 0}}}}})));
        QVERIFY(failed(call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", QJsonArray{QJsonArray{2, 1}, QJsonArray{1, 1}}}}}}})));
        QVERIFY(failed(call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", 1}, {"pane", 2}}}}})));
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("limits.sch")}})));
        d = front()->a_DocDiags.front();
        QCOMPARE(d->limits.size(), 1);
        QCOMPARE(d->limits.first().points.first().y(), 10.0);
        // The dialog: a row each, and what the table says applied.
        auto* dialog = new DiagramDialog(d, nullptr);
        auto* table = dialog->findChild<QTableWidget*>("limitTable");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 1)->text(), QString("10"));
        table->item(0, 1)->setText("0, 1; 5m, 2");
        table->item(0, 2)->setText("ramp");
        qobject_cast<QComboBox*>(table->cellWidget(0, 0))->setCurrentIndex(1);   // lower
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(d->limits.size(), 1);
        QCOMPARE(d->limits.first().side, lm::Limit::Lower);
        QCOMPARE(d->limits.first().points, QVector<QPointF>({QPointF(0, 1), QPointF(5e-3, 2)}));
        QCOMPARE(d->limits.first().label, QString("ramp"));
        dialog->close();
        // A stacked diagram's limit holds for its pane's traces alone.
        r = call("add_diagram", {{"type", "stacked"}, {"panes", 2},
                                 {"traces", QJsonArray{"tran.v(out)", QJsonObject{{"variable", "tran.v(out)"}, {"pane", 2}}}},
                                 {"limits", QJsonArray{QJsonObject{{"upper", 4}, {"pane", 2}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray stackedBeyond = json(r).value("verdict").toObject().value("beyond").toArray();
        QCOMPARE(stackedBeyond.size(), 1);
        QCOMPARE(stackedBeyond.first().toObject().value("trace").toInt(), 2);
    }

    // ---- tune's hold of a diagram's limits: a run beyond them breaks it.
    void aTuneHoldKeepsTheLimits()
    {
        if (!withNgspice()) QSKIP("no ngspice here");
        QJsonObject r = call("import_netlist", {{"text", "rc\nV1 in 0 PULSE(0 1 0.1m 1u 1u 1m 2m)\nR1 in out 1k\nC1 out 0 1u\n.tran 10u 5m\n.end"},
                                                {"save_as", path("tuned.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("simulate", {{"timeout", 60}});
        QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
        QVERIFY(!failed(call("add_diagram", {{"traces", QJsonArray{"tran.v(out)"}}, {"limits", QJsonArray{QJsonObject{{"upper", 0.65}}}}})));
        r = call("tune", {{"component", "R1"}, {"values", QJsonArray{"1k", "2k"}}, {"measure", QJsonObject{{"variable", "tran.v(out)"}, {"what", "max"}}},
                          {"hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"limits", 1}}}, {"max", 0}}}}, {"apply", false}},
                 180000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QList<bool> keeps;
        for (const QJsonValue& run : json(r).value("runs").toArray())
            if (!run.toObject().value("as it was").toBool()) keeps << run.toObject().value("keeps").toBool();
        QCOMPARE(keeps, QList<bool>({false, true}));   // 1k overshoots 0.65 V, 2k stays under
        QVERIFY(failed(call("tune", {{"component", "R1"}, {"values", QJsonArray{"1k"}}, {"measure", QJsonObject{{"variable", "tran.v(out)"}, {"what", "max"}}},
                                     {"hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"limits", "x"}}}, {"max", 0}}}}})));
    }

    // ---- The pole-zero map: each root where it is, what it says, the
    // guides, in the tools, the readout, the dialog and the file.
    void poleZeroMap()
    {
        // Poles -1k +/- j2k and one at +500 (unstable); a zero at -5k.
        QString data = QStringLiteral("<Qucs Dataset " PACKAGE_VERSION ">\n<indep pole_number 3>\n1\n2\n3\n</indep>\n"
                                      "<dep pole pole_number>\n-1000+j2000\n-1000-j2000\n500+j0\n</dep>\n"
                                      "<indep zero_number 1>\n1\n</indep>\n<dep zero zero_number>\n-5000\n</dep>\n");
        QVERIFY(schematicWith("pz", data));
        // get_dataset: the roots.
        QJsonObject r = call("get_dataset", {{"variables", QJsonArray{"pole"}}, {"measure", QJsonArray{"roots"}}});
        QJsonObject roots = json(r).value("variables").toArray().first().toObject().value("measurements").toObject().value("roots").toObject();
        QCOMPARE(roots.value("count").toInt(), 3);
        const QJsonObject first = roots.value("roots").toArray().at(0).toObject();
        QCOMPARE(first.value("re").toDouble(), -1000.0);
        QCOMPARE(first.value("im").toDouble(), 2000.0);
        QVERIFY(std::abs(first.value("wn").toDouble() - std::hypot(1000, 2000)) < 0.01);
        QVERIFY(std::abs(first.value("zeta").toDouble() - 1000 / std::hypot(1000, 2000)) < 1e-6);
        QVERIFY(!roots.value("roots").toArray().at(2).toObject().value("stable").toBool());
        QVERIFY(roots.value("note").toString().contains("right half-plane"));
        // The map.
        r = call("add_diagram", {{"type", "pole_zero"}, {"x", 100}, {"y", 500}, {"traces", QJsonArray{"pole", "zero"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray traces = json(r).value("roots").toArray();
        QCOMPARE(traces.size(), 2);
        QCOMPARE(traces.at(0).toObject().value("kind").toString(), QString("poles"));
        QCOMPARE(traces.at(1).toObject().value("kind").toString(), QString("zeros"));
        QCOMPARE(traces.at(1).toObject().value("roots").toArray().first().toObject().value("re").toDouble(), -5000.0);
        auto* pz = dynamic_cast<PoleZeroDiagram*>(front()->a_DocDiags.front());
        QVERIFY(pz);
        // The plane: the origin in it, symmetric about the real axis, the
        // same however often it is laid out.
        QVERIFY(pz->xAxis.low < -5000 && pz->xAxis.up > 500);
        QCOMPARE(pz->yAxis.low, -pz->yAxis.up);
        const double low = pz->xAxis.low, up = pz->yAxis.up;
        pz->calcDiagram();
        pz->calcDiagram();
        QCOMPARE(pz->xAxis.low, low);
        QCOMPARE(pz->yAxis.up, up);
        // Drawn: a cross at the pole, a circle round the zero (its centre
        // empty), in the trace's colour.
        QImage img(pz->x2 + 200, pz->y2 + 100, QImage::Format_RGB32);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            p.translate(100 - pz->cx, 50 + pz->y2 - pz->cy);
            pz->paintDiagram(&p);
        }
        const auto pixel = [&](double re, double im, int dx, int dy) {
            const double y[2] = {re, im};
            float px = 0, py = 0;
            pz->calcCoordinate(nullptr, y, nullptr, &px, &py, &pz->yAxis);
            return img.pixelColor(100 + int(px) + dx, 50 + pz->y2 - int(py) + dy);
        };
        const auto inColour = [](QColor c, QColor want) { return std::abs(c.red() - want.red()) < 60 && std::abs(c.green() - want.green()) < 60 && std::abs(c.blue() - want.blue()) < 60; };
        QVERIFY(inColour(pixel(-1000, 2000, 0, 0), pz->Graphs.at(0)->Color));   // the cross's middle
        QVERIFY(!inColour(pixel(-5000, 0, 0, -1), pz->Graphs.at(1)->Color));  // a circle's middle is open
        // A marker on a root reads its omega n, zeta and Q.
        r = call("add_marker", {{"trace", 1}, {"at", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString markerText = json(r).value("text").toString();
        QVERIFY2(markerText.contains(QString::fromUtf8("ωn 2.24 krad/s")) && markerText.contains(QString::fromUtf8("ζ 0.447")), qPrintable(markerText));
        // The readout: sigma, j omega, and what a root there says.
        const QString read = qucs_s::status::readout(pz, MappedPoint{-1000, 2000, 0});
        QVERIFY2(read.startsWith(QString::fromUtf8("σ ")) && read.contains(QString::fromUtf8("ζ 0.447")), qPrintable(read));
        // The guides: off, kept; the dialog turns them on.
        QVERIFY(!failed(call("edit_diagram", {{"pole_zero", QJsonObject{{"guides", false}}}})));
        QVERIFY(!pz->guides);
        QVERIFY(failed(call("edit_diagram", {{"diagram", 1}, {"panes", 2}})));
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("pz.sch")}})));
        pz = dynamic_cast<PoleZeroDiagram*>(front()->a_DocDiags.front());
        QVERIFY(pz && !pz->guides);
        auto* dialog = new DiagramDialog(pz, nullptr);
        auto* guides = dialog->findChild<QCheckBox*>("poleZeroGuides");
        QVERIFY(guides && !guides->isChecked());
        guides->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(pz->guides);
        dialog->close();
        QVERIFY(failed(call("add_diagram", {{"type", "rect"}, {"pole_zero", QJsonObject{{"guides", true}}}})));
    }

    // ---- A loop's diagrams: the Bode pair with its margins as get_dataset
    // measures them, the Nichols chart over its contours, the polar
    // diagram as a Nyquist plot.
    void loopDiagrams()
    {
        // T = 100 / ((1 + jf/100)(1 + jf/10k)(1 + jf/100k)).
        QVector<double> f;
        for (int i = 0; i <= 280; ++i) f << std::pow(10.0, i / 40.0);   // 1 Hz to 10 MHz
        const auto T = [](double x) {
            const std::complex<double> j(0, 1);
            return 100.0 / ((1.0 + j * x / 100.0) * (1.0 + j * x / 1e4) * (1.0 + j * x / 1e5));
        };
        // T5: two more poles at 1 MHz - its phase past -360 degrees.
        const auto T5 = [&](double x) {
            const std::complex<double> j(0, 1);
            return T(x) / ((1.0 + j * x / 1e6) * (1.0 + j * x / 1e6));
        };
        QVERIFY(schematicWith("loop", datasetText("frequency", f, {{"ac.v(out)", [&](double x) { return T(x).real(); }, [&](double x) { return T(x).imag(); }},
                                                                   {"ac.v(t5)", [&](double x) { return T5(x).real(); }, [&](double x) { return T5(x).imag(); }}})));
        // What get_dataset measures.
        const QJsonObject measured = json(call("get_dataset", {{"variables", QJsonArray{"ac.v(out)"}}, {"measure", QJsonArray{"phase_margin", "gain_margin"}}}))
                                         .value("variables").toArray().first().toObject().value("measurements").toObject();
        const double pm = measured.value("phase_margin").toObject().value("value").toDouble();
        const double gm = measured.value("gain_margin").toObject().value("value").toDouble();
        QVERIFY(pm > 40 && pm < 55 && gm > 15 && gm < 25);

        // ---- Bode: the same margins, the phase below, a marker reading it.
        QJsonObject r = call("add_diagram", {{"type", "bode"}, {"x", 100}, {"y", 500}, {"traces", QJsonArray{"ac.v(out)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject margins = json(r).value("margins").toArray().first().toObject();
        QCOMPARE(margins.value("phase margin").toObject().value("value").toDouble(), pm);
        QCOMPARE(margins.value("gain margin").toObject().value("value").toDouble(), gm);
        auto* bode = dynamic_cast<BodeDiagram*>(front()->a_DocDiags.front());
        QVERIFY(bode);
        QCOMPARE(bode->paneCount(), 2);
        QVERIFY(bode->xAxis.log && bode->pane(0).left.log && bode->pane(0).left.Units == Axis::dbUnits);
        // The phase axis: steps of 45 or 90 degrees, to about -270.
        QVERIFY(bode->pane(1).left.low <= -260 && bode->pane(1).left.up >= -1);
        QCOMPARE(std::fmod(bode->pane(1).left.low, 45.0), 0.0);
        r = call("add_marker", {{"trace", 1}, {"at", 1000}});
        QVERIFY2(json(r).value("text").toString().contains(QString::fromUtf8("phase: -90.")), qPrintable(json(r).value("text").toString()));
        // The phase drawn in the lower pane: at 1 kHz, near -90 degrees.
        {
            QImage img(bode->x2 + 200, bode->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            {
                QPainter p(&img);
                p.translate(100 - bode->cx, 50 + bode->y2 - bode->cy);
                bode->paintDiagram(&p);
            }
            const double x = 1000, y[2] = {std::arg(T(1000)) * 180 / 3.14159265358979, 0};
            float px = 0, py = 0;
            bode->calcCoordinate(&x, y, nullptr, &px, &py, &bode->pane(1).left);
            QVERIFY(py >= bode->paneBottom(1) && py <= bode->paneBottom(1) + bode->paneHeight());
            int hits = 0;
            for (int dy = -3; dy <= 3; ++dy) {
                const QColor c = img.pixelColor(100 + int(px), 50 + bode->y2 - int(py) + dy);
                if (c.blue() > 150 && c.red() < 100) ++hits;
            }
            QVERIFY(hits > 0);
        }
        // Its panes are its own; its traces are complex.
        QVERIFY(failed(call("edit_diagram", {{"panes", 3}})));
        QVERIFY(failed(call("edit_trace", {{"trace", 1}, {"pane", 2}})));
        QVERIFY(failed(call("edit_trace", {{"trace", 1}, {"part", "db"}})));
        QVERIFY(!failed(call("edit_diagram", {{"bode", QJsonObject{{"margins", false}}}})));
        QVERIFY(!bode->margins);

        // ---- Nichols: the gain against the phase; the closed loop.
        r = call("add_diagram", {{"type", "nichols"}, {"x", 700}, {"y", 500}, {"traces", QJsonArray{"ac.v(out)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("margins").toArray().first().toObject().value("phase margin").toObject().value("value").toDouble(), pm);
        auto* nichols = dynamic_cast<NicholsDiagram*>(*std::next(front()->a_DocDiags.begin()));
        QVERIFY(nichols);
        QVector<double> phase, gain;
        NicholsDiagram::pointsOf(nichols->Graphs.first(), &phase, &gain);
        QVERIFY(std::abs(phase.first()) < 1 && std::abs(gain.first() - 40) < 0.1);   // 100 at DC: 40 dB, 0 degrees
        QVERIFY(phase.last() < -260);   // unwrapped to -270
        // Past -360 degrees: unwrapped on, not folded back (-438 at 10 MHz).
        QVERIFY(!failed(call("add_trace", {{"diagram", 2}, {"variable", "ac.v(t5)"}})));
        NicholsDiagram::pointsOf(nichols->Graphs.last(), &phase, &gain);
        QVERIFY2(phase.last() < -400 && phase.last() > -460, qPrintable(QString::number(phase.last())));
        QVERIFY(!failed(call("delete", {{"traces", QJsonArray{QJsonObject{{"diagram", 2}, {"trace", 2}}}}})));
        QVERIFY(nichols->xAxis.low <= -270 && nichols->xAxis.up >= 0);
        // The 0 dB M contour meets 0 dB open-loop gain at -120 (and -240)
        // degrees; an N contour's points have the closed loop's phase.
        bool met = false;
        for (const QPointF& p : NicholsDiagram::mContour(0.0))
            if (std::isfinite(p.x()) && std::abs(p.y()) < 0.2 && (std::abs(p.x() + 120) < 2 || std::abs(p.x() + 240) < 2)) met = true;
        QVERIFY(met);
        int checked = 0;
        for (const QPointF& p : NicholsDiagram::nContour(-30.0)) {
            if (!std::isfinite(p.x())) continue;
            const std::complex<double> g = std::polar(std::pow(10.0, p.y() / 20), p.x() * 3.14159265358979 / 180);
            QVERIFY(std::abs(std::arg(g / (1.0 + g)) * 180 / 3.14159265358979 + 30) < 1e-6);
            ++checked;
        }
        QVERIFY(checked > 100);
        r = call("add_marker", {{"diagram", 2}, {"trace", 1}, {"at", 7850}});
        QVERIFY2(json(r).value("text").toString().contains("closed loop: "), qPrintable(json(r).value("text").toString()));
        QVERIFY(!failed(call("edit_diagram", {{"diagram", 2}, {"nichols", QJsonObject{{"grid", false}}}})));
        QVERIFY(!nichols->grid);

        // ---- Nyquist: the critical point and the unit circle; the
        // negative frequencies mirrored.
        r = call("add_diagram", {{"type", "polar"}, {"x", 100}, {"y", 1000}, {"traces", QJsonArray{"ac.v(out)"}},
                                 {"nyquist", QJsonObject{{"marks", true}, {"mirror", true}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("nyquist").toObject().value("mirror").toBool(), true);
        auto* polar = dynamic_cast<PolarDiagram*>(front()->a_DocDiags.back());
        QVERIFY(polar && polar->nyquist && polar->mirror);
        {
            QImage img(polar->x2 + 200, polar->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            {
                QPainter p(&img);
                p.translate(100 - polar->cx, 50 + polar->y2 - polar->cy);
                polar->paintDiagram(&p);
            }
            // The mirror: blue where the conjugate of T(1 kHz) is.
            const std::complex<double> t = T(1000);
            const double conj[2] = {t.real(), -t.imag()};
            float px = 0, py = 0;
            polar->calcCoordinate(nullptr, conj, nullptr, &px, &py, &polar->yAxis);
            int hits = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    const QColor c = img.pixelColor(100 + int(px) + dx, 50 + polar->y2 - int(py) + dy);
                    if (c.blue() > 150 && c.red() < 100) ++hits;
                }
            QVERIFY(hits > 0);
            // -1: red.
            const double minusOne[2] = {-1, 0};
            polar->calcCoordinate(nullptr, minusOne, nullptr, &px, &py, &polar->yAxis);
            int red = 0;
            for (int dx = -5; dx <= 5; ++dx) {
                const QColor c = img.pixelColor(100 + int(px) + dx, 50 + polar->y2 - int(py));
                if (c.red() > 150 && c.green() < 80) ++red;
            }
            QVERIFY(red > 0);
            // Ringed: red on the ring's diagonals, where the cross is not.
            int ring = 0;
            for (const int dx : {-3, 3})
                for (const int dy : {-3, 3}) {
                    const QColor c = img.pixelColor(100 + int(std::lround(px)) + dx, 50 + polar->y2 - int(std::lround(py)) + dy);
                    ring += c.red() > 150 && c.green() < 120 ? 1 : 0;
                }
            QVERIFY2(ring >= 2, qPrintable(QString::number(ring)));
        }
        QVERIFY(failed(call("edit_diagram", {{"diagram", 1}, {"nyquist", QJsonObject{{"marks", true}}}})));

        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("loop.sch")}})));
        auto it = front()->a_DocDiags.begin();
        QVERIFY(!dynamic_cast<BodeDiagram*>(*it)->margins);
        QVERIFY(!dynamic_cast<NicholsDiagram*>(*std::next(it))->grid);
        QVERIFY(dynamic_cast<PolarDiagram*>(*std::next(it, 2))->mirror);
        // The dialog: each its box.
        for (const auto& [index, name] : {std::pair<int, const char*>{0, "bodeMargins"}, {1, "nicholsGrid"}, {2, "nyquistMarks"}}) {
            Diagram* d = *std::next(front()->a_DocDiags.begin(), index);
            auto* dialog = new DiagramDialog(d, nullptr);
            auto* box = dialog->findChild<QCheckBox*>(name);
            QVERIFY2(box, name);
            box->setChecked(!box->isChecked());
            QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
            dialog->close();
        }
        it = front()->a_DocDiags.begin();
        QVERIFY(dynamic_cast<BodeDiagram*>(*it)->margins);
        QVERIFY(dynamic_cast<NicholsDiagram*>(*std::next(it))->grid);
        QVERIFY(!dynamic_cast<PolarDiagram*>(*std::next(it, 2))->nyquist);
    }

    // ---- The spectrum view: a known signal's lines, THD, SFDR and SNR, in
    // each window; get_dataset's spectrum; the view in the tools, the
    // file and the dialog.
    void spectrumView()
    {
        namespace sp = qucs_s::spectrum;
        constexpr double Pi = 3.14159265358979323846;
        // 1 V at 1 kHz, 0.1 V at 3 kHz, 0.01 V at 5 kHz: ten whole periods.
        const QVector<double> t = range(0, 10e-3, 5001);
        const auto x = [&](double s) { return std::sin(2 * Pi * 1e3 * s) + 0.1 * std::sin(2 * Pi * 3e3 * s) + 0.01 * std::sin(2 * Pi * 5e3 * s); };
        QVector<double> y;
        for (double s : t) y << x(s);
        for (int w = 0; w <= int(sp::Window::FlatTop); ++w) {
            const sp::Spectrum s = sp::of(t, y, sp::Window(w));
            const sp::Analysis a = sp::analyse(s, sp::NaN, 9);
            QVERIFY2(a.ok, qPrintable(sp::windowName(sp::Window(w))));
            QVERIFY2(std::abs(a.fundamental - 1000) <= s.df, qPrintable(sp::windowName(sp::Window(w))));
            QVERIFY2(std::abs(a.amplitude - 1) < 0.01, qPrintable(QStringLiteral("%1: %2").arg(sp::windowName(sp::Window(w))).arg(a.amplitude)));
            QVERIFY2(std::abs(a.harmonics.at(2).dBc - (-20)) < 0.2, qPrintable(QStringLiteral("%1: %2").arg(sp::windowName(sp::Window(w))).arg(a.harmonics.at(2).dBc)));
            QVERIFY(std::abs(a.harmonics.at(4).dBc - (-40)) < 0.5);
            QVERIFY(std::abs(a.harmonics.at(2).frequency - 3000) < s.df / 2);
            // Each bin a sine's amplitude: the line at 1 kHz 1 V (the
            // window's gain taken out; ten whole periods, on a bin).
            QVERIFY2(std::abs(s.amplitude.at(10) - 1) < 1e-3, qPrintable(QString::number(s.amplitude.at(10))));
            QVERIFY(std::abs(100 * a.thd - 10.05) < 0.1);
            QVERIFY(std::abs(a.sfdr - 20) < 0.2);
            QVERIFY(a.snr > 60);   // no noise but rounding
        }
        // A fundamental given: its line, not the strongest.
        const sp::Analysis third = sp::analyse(sp::of(t, y, sp::Window::Hann), 3000, 3);
        QVERIFY(third.ok && std::abs(third.amplitude - 0.1) < 0.002);
        QCOMPARE(sp::windowOf("Blackman Harris"), int(sp::Window::BlackmanHarris));
        QCOMPARE(sp::windowOf("triangle"), -1);

        QVERIFY(schematicWith("spec", datasetText("time", t, {{"tran.v(out)", x}})));
        // get_dataset's spectrum, in a window asked for.
        QJsonObject r = call("get_dataset", {{"variables", QJsonArray{"tran.v(out)"}}, {"measure", QJsonArray{"spectrum"}}, {"window", "flat_top"}});
        const QJsonObject m = json(r).value("variables").toArray().first().toObject().value("measurements").toObject().value("spectrum").toObject();
        QCOMPARE(m.value("window").toString(), QString("flat_top"));
        QVERIFY(std::abs(m.value("thd %").toDouble() - 10.05) < 0.1);
        QVERIFY(std::abs(m.value("sfdr dB").toDouble() - 20) < 0.2);
        QVERIFY(failed(call("get_dataset", {{"variables", QJsonArray{"tran.v(out)"}}, {"measure", QJsonArray{"spectrum"}}, {"window", "triangle"}})));
        // The view.
        r = call("add_diagram", {{"type", "spectrum"}, {"x", 100}, {"y", 500}, {"traces", QJsonArray{"tran.v(out)"}},
                                 {"spectrum", QJsonObject{{"window", "blackman_harris"}, {"harmonics", 5}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject a = json(r).value("analyses").toArray().first().toObject();
        QCOMPARE(a.value("window").toString(), QString("blackman_harris"));
        QCOMPARE(a.value("harmonics").toArray().size(), 5);
        QVERIFY(std::abs(a.value("thd %").toDouble() - 10.05) < 0.1);
        auto* view = dynamic_cast<SpectrumDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view);
        // Along x to past the 5th harmonic; the fundamental at 0 dBc, drawn.
        QVERIFY(view->xAxis.up >= 5000 && view->xAxis.up <= 7000);
        {
            QImage img(view->x2 + 200, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            {
                QPainter p(&img);
                p.translate(100 - view->cx, 50 + view->y2 - view->cy);
                view->paintDiagram(&p);
            }
            const auto inkNear = [&](double f, double dB, int r) {
                const double y0[2] = {dB, 0};
                float px = 0, py = 0;
                view->calcCoordinate(&f, y0, nullptr, &px, &py, &view->yAxis);
                int hits = 0;
                for (int dx = -r; dx <= r; ++dx)
                    for (int dy = -r; dy <= r; ++dy) {
                        const QColor c = img.pixelColor(100 + int(px) + dx, 50 + view->y2 - int(py) + dy);
                        if (c.blue() > 150 && c.red() < 100) ++hits;
                    }
                return hits;
            };
            QVERIFY(inkNear(1000, 0, 3) > 0);   // its circle
            // The line through the bins: the fundamental's neighbour, 6 dB
            // down in a Blackman-Harris window (clear of the circle).
            const sp::Spectrum& shown = view->resultOf(view->Graphs.first()).spectrum;
            QVERIFY(inkNear(1100, 20 * std::log10(shown.amplitude.at(11) / shown.amplitude.at(10)), 1) > 0);
        }
        // Edited; refused what is not its own; markers it has none of.
        QVERIFY(!failed(call("edit_diagram", {{"spectrum", QJsonObject{{"dbc", false}, {"drawn", "stems"}, {"from", 2e-3}}}})));
        QVERIFY(!view->dbc && view->stems && view->from == 2e-3);
        // From 2 ms: eight periods, 125 Hz a bin.
        QVERIFY(std::abs(view->resultOf(view->Graphs.first()).spectrum.df - 125) < 0.5);
        QVERIFY(failed(call("edit_diagram", {{"spectrum", QJsonObject{{"window", "triangle"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"spectrum", QJsonObject{{"harmonics", 1}}}})));
        r = call("add_marker", {{"trace", 1}, {"at", 1e-3}});
        QVERIFY(failed(r) && text(r).contains("no markers"));
        QVERIFY(failed(call("add_diagram", {{"type", "rect"}, {"spectrum", QJsonObject{{"dbc", true}}}})));
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("spec.sch")}})));
        view = dynamic_cast<SpectrumDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->window == sp::Window::BlackmanHarris && view->harmonics == 5 && !view->dbc && view->stems && view->from == 2e-3);
        QVERIFY(std::isnan(view->fundamental));
        // The dialog.
        auto* dialog = new DiagramDialog(view, nullptr);
        auto* window = dialog->findChild<QComboBox*>("spectrumWindow");
        QVERIFY(window);
        QCOMPARE(window->currentIndex(), int(sp::Window::BlackmanHarris));
        window->setCurrentIndex(int(sp::Window::Hamming));
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(view->window, sp::Window::Hamming);
        dialog->close();
    }

    // ---- The bathtub curve: a signal of known jitter's RJ, DJ, TJ and
    // opening; counted against the model; get_dataset's; the diagram in
    // the tools, the file, the dialog and the readout.
    void bathtubCurve()
    {
        namespace eye = qucs_s::eye;
        QVERIFY(std::abs(eye::gaussianTailInverse(eye::gaussianTail(3.0)) - 3.0) < 1e-9);
        QVERIFY(std::abs(eye::gaussianTailInverse(1e-12) - 7.0345) < 1e-3);
        const double ui = 100e-12, rj = 2e-12, dj = 6e-12;
        const Signal sig = dataSignal(ui, 2540, rj, dj);
        const auto curveOf = [](const Signal& s) {
            qucs_s::dataset::Curve c;
            c.x = s.t;
            c.y = s.v;
            return c;
        };
        eye::Options o;
        o.ui = ui;
        const eye::Result r = eye::analyse(curveOf(sig), o);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const eye::Bathtub b = eye::bathtubOf(r, 0);
        QVERIFY2(b.ok(), qPrintable(b.error));
        QVERIFY(b.dualDirac);
        QVERIFY2(std::abs(b.rj() * ui - rj) < 0.3e-12, qPrintable(QString::number(b.rj() * ui)));
        QVERIFY2(std::abs(b.dj() * ui - dj) < 1e-12, qPrintable(QString::number(b.dj() * ui)));
        QVERIFY2(std::abs(b.density - 64.0 / 127.0) < 0.02, qPrintable(QString::number(b.density)));
        // The total jitter at 1e-12: DJ and 2 Q(2 BER / density) RJ.
        double from = 0, to = 0;
        QVERIFY(b.opening(1e-12, &from, &to));
        const double z = eye::gaussianTailInverse(2 * 1e-12 / b.density);
        const double tj = (1 - (to - from)) * ui;
        QVERIFY2(std::abs(tj - (b.dj() + 2 * z * b.rj()) * ui) < 0.005 * tj, qPrintable(QString::number(tj)));
        QVERIFY2(std::abs(tj - (dj + 2 * z * rj)) < 4e-12, qPrintable(QString::number(tj)));
        // Low at the centre, half the density at a wall; the counted near
        // the model a sigma or so into a tail.
        QVERIFY(std::abs(b.best()) < 0.05);
        QVERIFY(b.ber(b.best()) < 1e-30);
        QVERIFY(std::abs(b.ber(b.left) - b.density / 2) < 0.1 * b.density);
        const double tail = b.left + b.deltaRight + b.sigmaRight * eye::gaussianTailInverse(0.1);
        QVERIFY2(b.measured(tail) > 0.7 * b.ber(tail) && b.measured(tail) < 1.4 * b.ber(tail),
                 qPrintable(QStringLiteral("%1 %2").arg(b.measured(tail)).arg(b.ber(tail))));
        QCOMPARE(b.measured(b.best()), 0.0);
        // No jitter: walls straight down, the eye open a whole UI.
        const eye::Bathtub clean = eye::bathtubOf(eye::analyse(curveOf(dataSignal(ui, 2540, 0, 0)), o), 0);
        QVERIFY(clean.ok() && clean.rj() == 0 && std::abs(clean.dj()) < 1e-9);
        QVERIFY(clean.opening(1e-12, &from, &to) && std::abs((to - from) - 1) < 1e-6);
        // PAM4: three eyes, each its own.
        const Signal pam4 = dataSignal(ui, 2000, rj, 0, true);
        eye::Options o4 = o;
        o4.levels = 4;
        const eye::Result r4 = eye::analyse(curveOf(pam4), o4);
        QVERIFY2(r4.ok() && r4.eyes.size() == 3, qPrintable(r4.error));
        for (int k = 0; k < 3; ++k) QVERIFY(eye::bathtubOf(r4, k).ok() && eye::bathtubOf(r4, k).opening(1e-12, &from, &to));

        QVERIFY(schematicWith("tub", datasetText("time", sig.t, {{"tran.v(rx)", [&](double x) { return sig.at(x); }}})));
        // get_dataset's: the narrowest eye's opening at 'ber'.
        QJsonObject res = call("get_dataset", {{"variables", QJsonArray{"tran.v(rx)"}}, {"measure", QJsonArray{"bathtub"}}, {"bit_period", ui}, {"ber", 1e-9}});
        const QJsonObject m = json(res).value("variables").toArray().first().toObject().value("measurements").toObject().value("bathtub").toObject();
        QVERIFY2(m.contains("opening"), qPrintable(text(res)));
        QCOMPARE(m.value("BER").toDouble(), 1e-9);
        QVERIFY(b.opening(1e-9, &from, &to));
        QVERIFY(std::abs(m.value("opening").toDouble() - (to - from) * ui) < 1e-15);
        QVERIFY(std::abs(m.value("random jitter, rms").toDouble() - b.rj() * ui) < 1e-16);
        QCOMPARE(m.value("value").toDouble(), m.value("opening").toDouble());
        QVERIFY(failed(call("get_dataset", {{"variables", QJsonArray{"tran.v(rx)"}}, {"measure", QJsonArray{"bathtub"}}, {"ber", 0.5}})));
        // The diagram.
        res = call("add_diagram", {{"type", "bathtub"}, {"x", 100}, {"y", 500}, {"width", 400}, {"height", 260}, {"traces", QJsonArray{"tran.v(rx)"}},
                                   {"bathtub", QJsonObject{{"unit_interval", "100p"}, {"ber", 1e-12}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        const QJsonObject a = json(res).value("analyses").toArray().first().toObject();
        QVERIFY2(std::abs(a.value("total jitter").toDouble() - tj) < 1e-15, qPrintable(QJsonDocument(a).toJson()));
        QCOMPARE(a.value("unit interval").toDouble(), ui);
        auto* view = dynamic_cast<BathtubDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view);
        QCOMPARE(view->bathtubs().size(), 1);
        // Across from wall to wall, up from the floor (the rate's 1e-4) to 1.
        QVERIFY(view->xAxis.low <= b.left + 1e-9 && view->xAxis.up >= b.left + 1 - 1e-9 && view->xAxis.up - view->xAxis.low < 1.5);
        QVERIFY(view->yAxis.log && view->yAxis.low <= 1e-16 * 1.0001 && view->yAxis.up >= 1);
        // Its numbers: the instants in decimals, the rates powers of ten -
        // unless a notation is chosen.
        {
            using qucs_s::numberformat::Notation;
            QCOMPARE(view->notationOf(&view->xAxis), Notation::Decimal);
            QCOMPARE(view->notationOf(&view->yAxis), Notation::Power);
            bool power = false, decimal = false;
            for (const Text* t : view->Texts) {
                power = power || t->s == QStringLiteral("10\u207B\u00B9\u00B2");
                decimal = decimal || t->s == QStringLiteral("0.0");   // (in step with -0.1 and 0.1)
            }
            QVERIFY(power && decimal);
            view->notation = Notation::Engineering;
            QCOMPARE(view->notationOf(&view->yAxis), Notation::Engineering);
            view->notation = Notation::Automatic;
        }
        const auto render = [&]() {
            QImage img(view->x2 + 200, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(100 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&p);
            return img;
        };
        const auto inkNear = [&](const QImage& img, double phase, double rate, int radius) {
            const double y[2] = {rate, 0};
            float px = 0, py = 0;
            view->calcCoordinate(&phase, y, nullptr, &px, &py, &view->yAxis);
            int hits = 0;
            for (int dx = -radius; dx <= radius; ++dx)
                for (int dy = -radius; dy <= radius; ++dy) {
                    const QColor c = img.pixelColor(100 + int(px) + dx, 50 + view->y2 - int(py) + dy);
                    if (c.blue() > 150 && c.red() < 100) ++hits;
                }
            return hits;
        };
        QImage shown = render();
        // The model's curve where it falls through 1e-6, and the opening
        // across the middle at 1e-12.
        QVERIFY(b.opening(1e-6, &from, &to));
        QVERIFY(inkNear(shown, from, 1e-6, 2) > 0);
        QVERIFY(inkNear(shown, to, 1e-6, 2) > 0);
        QVERIFY(inkNear(shown, b.best(), 1e-12, 1) > 0);
        QVERIFY(inkNear(shown, b.best(), 1e-6, 2) == 0);
        // The crossings counted: drawn, and not when not asked for.
        QVERIFY(!failed(call("edit_diagram", {{"bathtub", QJsonObject{{"measured", false}}}})));
        QVERIFY(!view->measured);
        QVERIFY(render() != shown);
        // Refused: a rate out of range, a floor above it, another type's.
        QVERIFY(failed(call("edit_diagram", {{"bathtub", QJsonObject{{"ber", 0.5}}}})));
        QVERIFY(failed(call("edit_diagram", {{"bathtub", QJsonObject{{"floor", 1e-6}}}})));
        QVERIFY(failed(call("edit_diagram", {{"bathtub", QJsonObject{{"levels", 3}}}})));
        QCOMPARE(view->ber, 1e-12);
        res = call("add_marker", {{"trace", 1}, {"at", 1e-9}});
        QVERIFY(failed(res) && text(res).contains("no markers"));
        QVERIFY(!failed(call("edit_diagram", {{"bathtub", QJsonObject{{"ber", 1e-15}, {"floor", 1e-20}, {"from", "1n"}, {"levels", 2}}}})));
        QCOMPARE(view->floorRate(), 1e-20);
        QVERIFY(view->yAxis.low <= 1e-20 * 1.0001);
        // The readout at the cursor: the instant in UI and time, the rate.
        const QString read = qucs_s::status::readout(view, MappedPoint{0.1, 1e-9, 0});
        QVERIFY2(read.contains("0.100 UI (10 ps)") && read.contains("BER 1e-09") && read.contains("tran.v(rx) "), qPrintable(read));
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("tub.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        view = dynamic_cast<BathtubDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->ui == ui && view->ber == 1e-15 && view->floor == 1e-20 && view->start == 1e-9 && view->levels == 2 && !view->measured);
        QVERIFY(std::isnan(view->threshold));
        // The dialog.
        auto* dialog = new DiagramDialog(view, nullptr);
        auto* ber = dialog->findChild<QLineEdit*>("bathtubBer");
        auto* levels = dialog->findChild<QComboBox*>("bathtubLevels");
        auto* counted = dialog->findChild<QCheckBox*>("bathtubMeasured");
        QVERIFY(ber && levels && counted);
        QCOMPARE(levels->currentIndex(), 1);
        ber->setText("1e-9");
        counted->setChecked(true);
        dialog->findChild<QLineEdit*>("bathtubFloor")->setText("");
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(view->ber, 1e-9);
        QVERIFY(view->measured && std::isnan(view->floor));
        QCOMPARE(view->floorRate(), 1e-13);
        dialog->close();
    }

    // ---- The levels at the sampling instant: each level's mean and
    // spread of a signal of known noise, Q and the rate it gives, the
    // vertical bathtub's best threshold and opening against their closed
    // forms; counted against the model; PAM4's three; a phase off the
    // centre; no noise.
    void levelsAtTheSamplingInstant()
    {
        namespace eye = qucs_s::eye;
        const double ui = 100e-12, sigma = 0.05;
        const Signal sig = noisySignal(ui, 4000, sigma);
        const auto curveOf = [](const Signal& s) {
            qucs_s::dataset::Curve c;
            c.x = s.t;
            c.y = s.v;
            return c;
        };
        eye::Options o;
        o.ui = ui;
        const qucs_s::dataset::Curve c = curveOf(sig);
        const eye::Result r = eye::analyse(c, o);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const QVector<double> sampled = eye::sampledAt(c, r);
        QCOMPARE(int(sampled.size()), r.symbols);
        const QList<eye::Level> levels = eye::levelsOf(r, sampled);
        QCOMPARE(levels.size(), 2);
        QCOMPARE(levels.at(0).count() + levels.at(1).count(), r.symbols);
        for (int k = 0; k < 2; ++k) {
            const eye::Level& l = levels.at(k);
            QVERIFY2(std::abs(l.mean - k) < 4 * sigma / std::sqrt(double(l.count())), qPrintable(QString::number(l.mean)));
            QVERIFY2(std::abs(l.sigma - sigma) < 0.08 * sigma, qPrintable(QString::number(l.sigma)));
            QVERIFY(std::is_sorted(l.values.cbegin(), l.values.cend()));
        }
        // The vertical bathtub: Q, the rate it gives; the best threshold
        // halfway, the opening at 1e-12 where each level's tail, weighted by
        // its share, falls through it.
        const eye::VoltageBathtub b = eye::voltageBathtubOf(r, 0, sampled);
        QVERIFY2(b.ok(), qPrintable(b.error));
        QCOMPARE(b.symbols, r.symbols);
        const double q = (b.high.mean - b.low.mean) / (b.low.sigma + b.high.sigma);
        QCOMPARE(b.q(), q);
        QVERIFY2(std::abs(q - 1 / (2 * sigma)) < 0.1 / (2 * sigma), qPrintable(QString::number(q)));
        QCOMPARE(b.berOfQ(), eye::gaussianTail(q));
        QVERIFY2(std::abs(b.best() - 0.5) < 0.03, qPrintable(QString::number(b.best())));
        double from = 0, to = 0;
        QVERIFY(b.opening(1e-12, &from, &to));
        const double pLow = double(b.low.count()) / b.symbols, pHigh = double(b.high.count()) / b.symbols;
        const double lo = b.low.mean + b.low.sigma * eye::gaussianTailInverse(1e-12 / pLow);
        const double hi = b.high.mean - b.high.sigma * eye::gaussianTailInverse(1e-12 / pHigh);
        QVERIFY2(std::abs(from - lo) < 1e-6 && std::abs(to - hi) < 1e-6, qPrintable(QStringLiteral("%1 %2 / %3 %4").arg(from).arg(to).arg(lo).arg(hi)));
        // Counted: none wrong at the best threshold, half a level's at its mean.
        QCOMPARE(b.measured(b.best()), 0.0);
        QVERIFY2(std::abs(b.measured(b.low.mean) - pLow / 2) < 0.1 * pLow, qPrintable(QString::number(b.measured(b.low.mean))));
        QVERIFY(std::abs(b.ber(b.low.mean) - pLow / 2) < 0.01);
        // A noisier upper level: the best threshold nearer the quiet one.
        {
            const qucs_s::dataset::Curve c2 = curveOf(noisySignal(ui, 4000, sigma, false, 2 * sigma));
            const eye::Result r2 = eye::analyse(c2, o);
            const eye::VoltageBathtub b2 = eye::voltageBathtubOf(r2, 0, eye::sampledAt(c2, r2));
            QVERIFY2(b2.ok() && b2.best() < 0.45, qPrintable(QString::number(b2.best())));
        }
        // Off the centre, near the walls (the edges): the levels spread, Q falls.
        const eye::VoltageBathtub near = eye::voltageBathtubOf(r, 0, eye::sampledAt(c, r, 0.45), 0.45);
        QVERIFY2(near.ok() && near.q() < q / 2, qPrintable(QString::number(near.q())));
        QCOMPARE(near.phase, 0.45);
        // Off the centre, none from before the eye's start (the settling,
        // at 10 here, until just before it): the first instant moved a UI on.
        {
            qucs_s::dataset::Curve settled = c;
            for (int i = 0; i < settled.x.size(); ++i)
                if (settled.x.at(i) < 49.95 * ui) settled.y[i] = 10.0;
            eye::Options os = o;
            os.start = 50.3 * ui;
            const eye::Result rs = eye::analyse(settled, os);
            QVERIFY2(rs.ok(), qPrintable(rs.error));
            for (const double at : {-0.45, -0.2, 0.0, 0.3}) {
                const QVector<double> v = eye::sampledAt(settled, rs, at);
                QVERIFY2(!v.isEmpty() && *std::max_element(v.cbegin(), v.cend()) < 2.0, qPrintable(QString::number(at)));
            }
        }
        // No eye 2 in NRZ.
        QVERIFY(!eye::voltageBathtubOf(r, 1, sampled).ok());
        // PAM4: four levels, three eyes.
        const qucs_s::dataset::Curve c4 = curveOf(noisySignal(ui, 4000, sigma, true));
        eye::Options o4 = o;
        o4.levels = 4;
        const eye::Result r4 = eye::analyse(c4, o4);
        QVERIFY2(r4.ok(), qPrintable(r4.error));
        const QVector<double> s4 = eye::sampledAt(c4, r4);
        const QList<eye::Level> l4 = eye::levelsOf(r4, s4);
        QCOMPARE(l4.size(), 4);
        for (int k = 0; k < 4; ++k) QVERIFY2(std::abs(l4.at(k).mean - k) < 0.01, qPrintable(QString::number(l4.at(k).mean)));
        for (int k = 0; k < 3; ++k) {
            const eye::VoltageBathtub e = eye::voltageBathtubOf(r4, k, s4);
            QVERIFY2(e.ok() && std::abs(e.best() - (k + 0.5)) < 0.05, qPrintable(QString::number(e.best())));
        }
        // No noise - a spread below a billionth of the gap is the numbers'
        // rounding: Q infinite, the rate it gives 0, open from level to level.
        const qucs_s::dataset::Curve clean = curveOf(noisySignal(ui, 2000, 1e-13));
        const eye::Result rc = eye::analyse(clean, o);
        const eye::VoltageBathtub bc = eye::voltageBathtubOf(rc, 0, eye::sampledAt(clean, rc));
        QVERIFY(bc.ok() && std::isinf(bc.q()) && bc.berOfQ() == 0.0);
        QVERIFY(bc.opening(1e-12, &from, &to) && std::abs((to - from) - 1.0) < 1e-6);
        // As JSON.
        const QJsonObject j = eye::toJson(b, 1e-12);
        QCOMPARE(j.value("Q").toDouble(), qucs_s::dataset::rounded(q));
        // (Exactly: QCOMPARE takes a value below 1e-12 for 0.)
        QVERIFY(j.contains("BER from Q") && j.value("BER from Q").toDouble() == qucs_s::dataset::rounded(b.berOfQ()));
        QVERIFY(j.value("low level").toObject().value("symbols").toInt() == b.low.count() && j.contains("best threshold"));
        b.opening(1e-12, &from, &to);
        QCOMPARE(j.value("opening").toDouble(), qucs_s::dataset::rounded(to - from));
        QVERIFY(eye::toJson(bc, 1e-12).value("Q").isNull());
        // The automatic bins: from each level's spread, a bar a fraction of
        // a sigma - those of the values together are wider than the noise.
        const int bins = LevelHistogramDiagram::levelBins(levels, sampled.first(), sampled.last());
        QVector<double> sorted = sampled;
        std::sort(sorted.begin(), sorted.end());
        const double width = (sorted.last() - sorted.first()) / bins;
        QVERIFY2(width < sigma, qPrintable(QString::number(width)));
        QVERIFY(HistogramDiagram::automaticBins(sampled, sorted.first(), sorted.last()) < bins);
    }

    // ---- The level histogram and the bathtub on its side: in the tools,
    // the file, the dialog, the picture and the readout.
    void levelHistogramAndVoltageBathtub()
    {
        namespace eye = qucs_s::eye;
        const double ui = 100e-12, sigma = 0.05;
        // (Its levels 0.3 and 1.5: an axis from 0 to 1 is no fit. Beside it,
        // one at 0 and 1, through 0.)
        const Signal sig = noisySignal(ui, 3000, sigma, false, -1.0, 1.2, 0.3);
        const Signal zero = noisySignal(ui, 3000, sigma);
        QVERIFY(schematicWith("levels", datasetText("time", sig.t, {{"tran.v(rx)", [&](double x) { return sig.at(x); }},
                                                                    {"tran.v(zero)", [&](double x) { return zero.at(x); }}})));
        // Among the diagrams.
        bool listed = false;
        for (Module* m : Category::getModules(QObject::tr("diagrams"))) {
            QString name;
            char* file = nullptr;
            delete m->info(name, file, false);
            listed = listed || name == QObject::tr("Level Histogram");
        }
        QVERIFY(listed);
        QJsonObject res = call("add_diagram", {{"type", "level_histogram"}, {"x", 100}, {"y", 400}, {"width", 300}, {"height", 300},
                                               {"traces", QJsonArray{"tran.v(rx)"}}, {"level_histogram", QJsonObject{{"unit_interval", "100p"}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* hist = dynamic_cast<LevelHistogramDiagram*>(front()->a_DocDiags.front());
        QVERIFY(hist);
        QCOMPARE(hist->histograms().size(), 1);
        const LevelHistogramDiagram::Histogram h = hist->histograms().first();   // (a copy: an edit lays them out again)
        QVERIFY2(h.error.isEmpty(), qPrintable(h.error));
        double counted = 0;
        for (double n : h.counts) counted += n;
        QCOMPARE(int(counted), h.symbols);
        QVERIFY2(h.width < sigma, qPrintable(QString::number(h.width)));
        QCOMPARE(h.levels.size(), 2);
        QCOMPARE(h.thresholds.size(), 1);
        // Its numbers, as the eye's analysis gives them.
        const QJsonObject a = json(res).value("analyses").toArray().first().toObject();
        QCOMPARE(a.value("levels").toArray().size(), 2);
        QCOMPARE(a.value("levels").toArray().at(1).toObject().value("sigma").toDouble(), qucs_s::dataset::rounded(h.levels.at(1).sigma));
        QCOMPARE(a.value("eye").toObject().value("Q").toDouble(), qucs_s::dataset::rounded(h.eyes.first().q()));
        QCOMPARE(a.value("symbols").toInt(), h.symbols);
        QCOMPARE(json(res).value("level_histogram").toObject().value("unit_interval").toDouble(), ui);
        // The signal up, from its lowest value to its highest; the counts
        // across from 0.
        QVERIFY(hist->yAxis.low <= h.levels.first().values.first() && hist->yAxis.up >= h.levels.last().values.last());
        QVERIFY(hist->xAxis.low <= 0.0 && hist->xAxis.low > -0.2 * hist->xAxis.up);
        const auto render = [](Diagram* view) {
            QImage img(view->x2 + 200, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(100 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&p);
            return img;
        };
        // A bar of the lower level at its mean, none halfway between them.
        const auto inkAt = [&](const QImage& img, double count, double value) {
            const double X = (count - hist->xAxis.low) / (hist->xAxis.up - hist->xAxis.low) * hist->x2;
            const double Y = (value - hist->yAxis.low) / (hist->yAxis.up - hist->yAxis.low) * hist->y2;
            const QColor c = img.pixelColor(100 + int(X), 50 + hist->y2 - int(Y));
            return c != QColor(Qt::white);
        };
        const QImage shown = render(hist);
        QVERIFY(inkAt(shown, 1.0, h.levels.first().mean));
        QVERIFY(inkAt(shown, 1.0, h.levels.last().mean));
        // The threshold dashed across, left of the box.
        {
            const double Y = (h.thresholds.first() - hist->yAxis.low) / (hist->yAxis.up - hist->yAxis.low) * hist->y2;
            int grey = 0;
            for (int X = 5; X < 60; ++X)
                for (int dy = -1; dy <= 1; ++dy) {   // (the pixel's row either way of the value's)
                    const QColor col = shown.pixelColor(100 + X, 50 + hist->y2 - int(std::lround(Y)) + dy);
                    if (col.red() > 100 && col.red() < 180 && std::abs(col.red() - col.blue()) < 20) ++grey;
                }
            QVERIFY2(grey > 10, qPrintable(QString::number(grey)));
        }
        QVERIFY(!failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"gaussians", false}}}})));
        QVERIFY(!hist->gaussians);
        QVERIFY(render(hist) != shown);
        // No markers on it.
        res = call("add_marker", {{"trace", 1}, {"at", 1e-9}});
        QVERIFY(failed(res) && text(res).contains("no markers"));
        // Sampled into the edges: the levels spread.
        QVERIFY(!failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"bins", 40}, {"phase", 0.45}}}})));
        QVERIFY(hist->bins == 40 && hist->phase == 0.45);
        QCOMPARE(hist->histograms().first().counts.size(), 40);
        // A few symbols a bar: the counts from 0 all the same.
        QVERIFY(!failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"bins", 1000}}}})));
        QVERIFY2(hist->xAxis.low <= 0.0, qPrintable(QString::number(hist->xAxis.low)));
        QVERIFY(!failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"bins", 40}}}})));
        QVERIFY2(hist->histograms().first().levels.first().sigma > 1.5 * h.levels.first().sigma,
                 qPrintable(QString::number(hist->histograms().first().levels.first().sigma)));
        // Refused: a phase past a wall, bins below 0, a bool not one, a rate
        // out of range, another type's.
        QVERIFY(failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"phase", 0.6}}}})));
        QVERIFY(failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"bins", -1}}}})));
        QVERIFY(failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"gaussians", "yes"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"level_histogram", QJsonObject{{"ber", 0.5}}}})));
        QVERIFY(failed(call("edit_diagram", {{"bathtub", QJsonObject{{"direction", "voltage"}}}})));
        QVERIFY(hist->phase == 0.45 && hist->bins == 40);
        // The readout: the value up, and the symbols in its bar.
        const QString read = qucs_s::status::readout(hist, MappedPoint{1.0, h.levels.first().mean, 0});
        QVERIFY2(read.contains("symbols") && read.contains("tran.v(rx)"), qPrintable(read));

        // The bathtub on its side, beside it.
        res = call("add_diagram", {{"type", "bathtub"}, {"x", 500}, {"y", 400}, {"width", 400}, {"height", 260}, {"traces", QJsonArray{"tran.v(rx)"}},
                                   {"bathtub", QJsonObject{{"unit_interval", "100p"}, {"direction", "voltage"}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* tub = dynamic_cast<BathtubDiagram*>(front()->a_DocDiags.back());
        QVERIFY(tub && tub->direction == BathtubDiagram::Voltage);
        QCOMPARE(tub->voltageBathtubs().size(), 1);
        QVERIFY(tub->bathtubs().isEmpty());
        const eye::VoltageBathtub v = tub->voltageBathtubs().first().first();   // (a copy: an edit lays them out again)
        QVERIFY2(v.ok(), qPrintable(v.error));
        const QJsonObject va = json(res).value("analyses").toArray().first().toObject();
        QCOMPARE(va.value("Q").toDouble(), qucs_s::dataset::rounded(v.q()));
        double from = 0, to = 0;
        QVERIFY(v.opening(1e-12, &from, &to));
        QCOMPARE(va.value("opening").toDouble(), qucs_s::dataset::rounded(to - from));
        QCOMPARE(json(res).value("bathtub").toObject().value("direction").toString(), QString("voltage"));
        // (Its rates are on the log axis, not the signal's values through 0.)
        res = call("add_trace", {{"diagram", 2}, {"variable", "tran.v(zero)"}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QVERIFY2(!text(res).contains("left off the log axis"), qPrintable(text(res)));
        QVERIFY(!failed(call("delete", {{"traces", QJsonArray{QJsonObject{{"diagram", 2}, {"trace", 2}}}}})));
        // Across from the lower level to the upper; the thresholds' notation.
        QVERIFY(std::abs(tub->xAxis.low - v.low.mean) < 0.2 && std::abs(tub->xAxis.up - v.high.mean) < 0.2);
        QCOMPARE(tub->notationOf(&tub->xAxis), qucs_s::numberformat::Notation::Automatic);
        // The model drawn where it falls through 1e-6.
        const QImage side = render(tub);
        QVERIFY(v.opening(1e-6, &from, &to));
        const auto inkNear = [&](const QImage& img, double threshold, double rate) {
            const double y[2] = {rate, 0};
            float px = 0, py = 0;
            tub->calcCoordinate(&threshold, y, nullptr, &px, &py, &tub->yAxis);
            int hits = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    const QColor col = img.pixelColor(100 + int(px) + dx, 50 + tub->y2 - int(py) + dy);
                    if (col.blue() > 150 && col.red() < 100) ++hits;
                }
            return hits;
        };
        QVERIFY(inkNear(side, from, 1e-6) > 0 && inkNear(side, to, 1e-6) > 0);
        // Refused: a direction not one, a phase past a wall.
        QVERIFY(failed(call("edit_diagram", {{"diagram", 2}, {"bathtub", QJsonObject{{"direction", "sideways"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"diagram", 2}, {"bathtub", QJsonObject{{"phase", -0.7}}}})));
        QVERIFY(!failed(call("edit_diagram", {{"diagram", 2}, {"bathtub", QJsonObject{{"phase", -0.1}}}})));
        QCOMPARE(tub->phase, -0.1);
        QCOMPARE(tub->voltageBathtubs().first().first().phase, -0.1);
        const QString tubRead = qucs_s::status::readout(tub, MappedPoint{0.8, 1e-9, 0});
        QVERIFY2(tubRead.contains("threshold") && tubRead.contains("BER 1e-09"), qPrintable(tubRead));
        // Back across: the timing tubs again.
        QVERIFY(!failed(call("edit_diagram", {{"diagram", 2}, {"bathtub", QJsonObject{{"direction", "timing"}}}})));
        QVERIFY(tub->voltageBathtubs().isEmpty() && tub->bathtubs().size() == 1);
        QVERIFY(!failed(call("edit_diagram", {{"diagram", 2}, {"bathtub", QJsonObject{{"direction", "voltage"}}}})));

        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("levels.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        hist = dynamic_cast<LevelHistogramDiagram*>(front()->a_DocDiags.front());
        tub = dynamic_cast<BathtubDiagram*>(front()->a_DocDiags.back());
        QVERIFY(hist && hist->ui == ui && hist->phase == 0.45 && hist->bins == 40 && !hist->gaussians && hist->ber == 1e-12 && hist->levels == 0);
        QVERIFY(std::isnan(hist->start) && std::isnan(hist->threshold));
        QVERIFY(tub && tub->direction == BathtubDiagram::Voltage && tub->phase == -0.1);
        // The dialogs.
        auto* dialog = new DiagramDialog(hist, nullptr);
        auto* bins = dialog->findChild<QSpinBox*>("levelBins");
        auto* phase = dialog->findChild<QLineEdit*>("levelPhase");
        auto* gaussians = dialog->findChild<QCheckBox*>("levelGaussians");
        auto* levelsBox = dialog->findChild<QComboBox*>("levelLevels");
        QVERIFY(bins && phase && gaussians && levelsBox);
        QCOMPARE(bins->value(), 40);
        bins->setValue(0);
        phase->setText("0.9");   // (past a wall: the wall)
        gaussians->setChecked(true);
        levelsBox->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(hist->bins == 0 && hist->phase == 0.5 && hist->gaussians && hist->levels == 2);
        dialog->close();
        dialog = new DiagramDialog(tub, nullptr);
        auto* direction = dialog->findChild<QComboBox*>("bathtubDirection");
        auto* tubPhase = dialog->findChild<QLineEdit*>("bathtubPhase");
        QVERIFY(direction && tubPhase);
        QCOMPARE(direction->currentIndex(), 1);
        tubPhase->setText("0.2");
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(tub->phase, 0.2);
        direction->setCurrentIndex(0);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(tub->direction, int(BathtubDiagram::Timing));
        dialog->close();
        closeAll();
    }

    // ---- Resizing by the handle: a polar chart stays square, the new
    // diagrams (a bathtub and a stacked one here) take the shape dragged.
    void resizingKeepsItsShape()
    {
        QVERIFY(schematicWith("shape", datasetText("time", range(0, 1, 11), {{"tran.v(a)", [](double t) { return t; }}})));
        for (const QString type : {QStringLiteral("bathtub"), QStringLiteral("stacked"), QStringLiteral("polar")}) {
            QVERIFY(!failed(call("add_diagram", {{"type", type}, {"x", 100}, {"y", 400}, {"width", 200}, {"height", 200}})));
            Schematic* sch = front();
            Diagram* d = sch->a_DocDiags.back();
            for (Diagram* other : sch->a_DocDiags) other->isSelected = false;
            d->isSelected = true;
            // The top right corner's handle, dragged to 300 wide, 100 high.
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(), QPointF(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            app->view->MPressSelect(sch, &press, float(d->cx + d->x2), float(d->cy - d->y2));
            const QPoint to = sch->modelToContents(QPoint(d->cx + 300, d->cy - 100));
            QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            app->view->MMoveSelect(sch, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(to), QPointF(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            app->view->MReleaseResizeDiagram(sch, &release);
            // (Square: the smaller side.)
            QVERIFY2(d->x2 == (type == "polar" ? 100 : 300) && d->y2 == 100,
                     qPrintable(QStringLiteral("%1: %2 x %3").arg(d->Name).arg(d->x2).arg(d->y2)));
            d->isSelected = false;
        }
    }

    // ---- The contour map: a value over two sweeps - its grid, iso-lines,
    // colours, pass band and share; in the tools, the file, the dialog and
    // the readout.
    void contourMap()
    {
        const QVector<double> xs = range(1, 10, 10);
        QVector<double> ys = range(0, 1, 6);
        std::reverse(ys.begin(), ys.end());   // (swept down: turned round)
        const auto f = [](double x, double y) { return x + 10 * y; };
        QVERIFY(schematicWith("map", datasetText2("r", xs, "c", ys, {{"sw.v", f}, {"sw.w", [](double x, double y) { return x * y; }}})
                                         + datasetText("time", range(0, 1, 11), {{"tran.u", [](double t) { return t; }}})));
        QJsonObject res = call("add_diagram", {{"type", "contour"}, {"x", 100}, {"y", 400}, {"width", 300}, {"height", 200}, {"traces", QJsonArray{"sw.v"}},
                                               {"contour", QJsonObject{{"pass", QJsonObject{{"min", 10}}}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* view = dynamic_cast<ContourDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view);
        // The grid: each sweep rising, every value at its point.
        QCOMPARE(view->grids().size(), 1);
        const ContourDiagram::Grid& g = view->grids().first();
        QVERIFY2(g.ok(), qPrintable(g.error));
        QCOMPARE(g.x.size(), 10);
        QCOMPARE(g.y.size(), 6);
        QVERIFY(g.y.first() == 0 && g.y.last() == 1);
        for (int j = 0; j < 6; ++j)
            for (int i = 0; i < 10; ++i) QCOMPARE(g.at(i, j), f(g.x.at(i), g.y.at(j)));
        // Between the points: bilinear (exact for this one); outside none.
        QVERIFY(std::abs(g.valueAt(5.5, 0.55) - 11) < 1e-12);
        QVERIFY(std::isnan(g.valueAt(11, 0.5)));
        // The iso-lines at round values; the colours over the values.
        QCOMPARE(ContourDiagram::levelsIn(1, 20, 8), (QVector<double>{2.5, 5, 7.5, 10, 12.5, 15, 17.5}));
        QCOMPARE(ContourDiagram::levelsIn(-1, 1, 4), (QVector<double>{-0.5, 0, 0.5}));
        QVERIFY(view->zAxis.low == 1 && view->zAxis.up == 20);
        // The iso-line at 12.5 lies on x + 10 y = 12.5.
        const QVector<QLineF> line = view->isoLine(g, 12.5);
        QVERIFY(!line.isEmpty());
        for (const QLineF& l : line) {
            const auto onIt = [&](QPointF p) {
                const double x = view->xAxis.low + p.x() / view->x2 * (view->xAxis.up - view->xAxis.low);
                const double y = view->yAxis.low + p.y() / view->y2 * (view->yAxis.up - view->yAxis.low);
                return std::abs(f(x, y) - 12.5) < 0.05;
            };
            QVERIFY(onIt(l.p1()) && onIt(l.p2()));
        }
        // A cell crossed straight up, and the two saddles (by the centre).
        {
            ContourDiagram::Grid cell;
            cell.x = {2, 3};
            cell.y = {0.2, 0.4};
            const auto ends = [&](const QVector<QLineF>& lines) {
                QList<QPointF> data;
                for (const QLineF& l : lines)
                    for (const QPointF& p : {l.p1(), l.p2()})
                        data << QPointF(view->xAxis.low + p.x() / view->x2 * (view->xAxis.up - view->xAxis.low),
                                        view->yAxis.low + p.y() / view->y2 * (view->yAxis.up - view->yAxis.low));
                return data;
            };
            cell.v = {0, 1, 0, 1};   // rising along x alone: x = 2.5 at 0.5
            const QList<QPointF> up = ends(view->isoLine(cell, 0.5));
            QCOMPARE(up.size(), 2);
            for (const QPointF& p : up) QVERIFY(std::abs(p.x() - 2.5) < 0.01);
            QVERIFY(std::abs(up.at(0).y() - up.at(1).y()) > 0.15);
            cell.v = {1, 0, 0, 1};   // a and c (lower left, upper right) high: a saddle
            QList<QPointF> saddle = ends(view->isoLine(cell, 0.4));   // centre 0.5 above: a and c joined
            QCOMPARE(saddle.size(), 4);
            // (Joined through the centre: each segment cuts off b or d, a
            // corner below - its ends on b's or d's two edges.)
            for (int k = 0; k < 4; k += 2) {
                const QPointF a = saddle.at(k), b = saddle.at(k + 1);
                const bool aroundB = (std::abs(a.y() - 0.2) < 0.01 || std::abs(b.y() - 0.2) < 0.01) && (std::abs(a.x() - 3) < 0.01 || std::abs(b.x() - 3) < 0.01);
                const bool aroundD = (std::abs(a.y() - 0.4) < 0.01 || std::abs(b.y() - 0.4) < 0.01) && (std::abs(a.x() - 2) < 0.01 || std::abs(b.x() - 2) < 0.01);
                QVERIFY(aroundB || aroundD);
            }
            saddle = ends(view->isoLine(cell, 0.6));   // centre below: a and c each cut off
            QCOMPARE(saddle.size(), 4);
            for (int k = 0; k < 4; k += 2) {
                const QPointF a = saddle.at(k), b = saddle.at(k + 1);
                const bool aroundA = (std::abs(a.y() - 0.2) < 0.01 || std::abs(b.y() - 0.2) < 0.01) && (std::abs(a.x() - 2) < 0.01 || std::abs(b.x() - 2) < 0.01);
                const bool aroundC = (std::abs(a.y() - 0.4) < 0.01 || std::abs(b.y() - 0.4) < 0.01) && (std::abs(a.x() - 3) < 0.01 || std::abs(b.x() - 3) < 0.01);
                QVERIFY(aroundA || aroundC);
            }
        }
        // The share that passes (at least 10), and where.
        int good = 0;
        for (double y : ys)
            for (double x : xs) good += f(x, y) >= 10 ? 1 : 0;
        QCOMPARE(view->passing(), good / 60.0);
        const QJsonObject a = json(res).value("analyses").toArray().first().toObject();
        QVERIFY2(std::abs(a.value("passing share").toDouble() - good / 60.0) < 1e-6, qPrintable(QJsonDocument(a).toJson()));
        QCOMPARE(a.value("x").toObject().value("points").toInt(), 10);
        QCOMPARE(a.value("lowest").toObject().value("value").toDouble(), 1.0);
        QCOMPARE(a.value("highest").toObject().value("x").toDouble(), 10.0);
        QCOMPARE(a.value("highest").toObject().value("y").toDouble(), 1.0);
        QCOMPARE(a.value("iso-lines").toArray().size(), 7);
        QCOMPARE(a.value("passing within").toObject().value("y").toArray().first().toDouble(), 0.0);
        // Drawn: the colour of a value where it is, hatched where it fails,
        // the iso-line and the band's edge dark.
        const auto render = [&]() {
            QImage img(view->x2 + 300, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(100 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&p);
            return img;
        };
        const auto pixel = [&](const QImage& img, double x, double y, int dx = 0, int dy = 0) {
            const double yd[2] = {y, 0};
            float px = 0, py = 0;
            view->calcCoordinate(&x, yd, nullptr, &px, &py, &view->yAxis);
            return img.pixelColor(100 + int(px) + dx, 50 + view->y2 - int(py) + dy);
        };
        const auto near = [](QColor a, QColor b) { return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) < 30; };
        QImage shown = render();
        QVERIFY2(near(pixel(shown, 8.6, 0.85), view->colourAt((f(8.6, 0.85) - 1) / 19)), qPrintable(pixel(shown, 8.6, 0.85).name()));
        int hatched = 0, dark = 0, edge = 0;
        for (int dx = -6; dx <= 6; ++dx)
            for (int dy = -6; dy <= 6; ++dy) {
                if (pixel(shown, 3, 0.25, dx, dy).value() < 90) ++hatched;
                if (pixel(shown, 7.5, 0.5, dx / 3, dy / 3).value() < 90) ++dark;   // on the 12.5 iso-line
                if (pixel(shown, 5, 0.5, dx / 3, dy / 3).value() < 40) ++edge;     // on the band's edge, 10
            }
        QVERIFY2(hatched > 10, qPrintable(QString::number(hatched)));
        QVERIFY(dark > 0 && edge > 0);
        // Another trace: its iso-lines over the map; the readout at the
        // cursor reads both.
        QVERIFY(!failed(call("add_trace", {{"variable", "sw.w"}})));
        QCOMPARE(view->grids().size(), 2);
        const QString read = qucs_s::status::readout(view, MappedPoint{5.5, 0.55, 0});
        QVERIFY2(read.contains("r 5.5") && read.contains("c 0.55") && read.contains("sw.v 11") && read.contains("sw.w "), qPrintable(read));
        // A trace's part: its iso-lines in dB (x y = 1 at 2, 0.5: 0 dB).
        res = call("edit_trace", {{"trace", 2}, {"part", "db"}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QVERIFY(std::abs(view->grids().at(1).valueAt(2, 0.6) - 20 * std::log10(1.2)) < 1e-9);
        // One sweep alone: no map, said why.
        QVERIFY(!failed(call("add_trace", {{"variable", "tran.u"}})));
        QVERIFY(view->grids().last().error.contains("two sweeps"));
        // Edited: colours over a range given, no fill; refused what is not one.
        QVERIFY(!failed(call("edit_diagram", {{"contour", QJsonObject{{"range", QJsonObject{{"from", 0}, {"to", 40}}}, {"map", "turbo"}, {"levels", 4}, {"labels", false}}}})));
        QVERIFY(view->zAxis.low == 0 && view->zAxis.up == 40 && view->map == ContourDiagram::Turbo && view->levels == 4 && !view->labels);
        {
            // The band's edge (10) black and bold: no label there now, the
            // iso-line at 10 lighter.
            const QImage plain = render();
            int black = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) black += pixel(plain, 5, 0.5, dx, dy).value() < 12 ? 1 : 0;
            QVERIFY2(black > 0, qPrintable(pixel(plain, 5, 0.5).name()));
            // (Its axes the maps' alone: not the time of a trace of one sweep.)
            QVERIFY2(view->xAxis.low >= 1 - 1e-9 && view->xAxis.up <= 10 + 1e-9, qPrintable(QStringLiteral("%1 %2").arg(view->xAxis.low).arg(view->xAxis.up)));
        }
        QVERIFY(failed(call("edit_diagram", {{"contour", QJsonObject{{"levels", 51}}}})));
        QVERIFY(failed(call("edit_diagram", {{"contour", QJsonObject{{"map", "jet"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"contour", QJsonObject{{"pass", QJsonObject{{"min", 5}, {"max", 2}}}}}})));
        QVERIFY(failed(call("edit_diagram", {{"contour", QJsonObject{{"range", QJsonObject{{"from", 3}, {"to", 3}}}}}})));
        QCOMPARE(view->passMin, 10.0);
        res = call("add_marker", {{"trace", 1}, {"at", 5}});
        QVERIFY(failed(res) && text(res).contains("no markers"));
        QVERIFY(!failed(call("edit_diagram", {{"contour", QJsonObject{{"filled", false}, {"pass", QJsonObject{{"min", 4}, {"max", 16}}}}}})));
        QVERIFY(render() != shown);
        // Where it is drawn: its frame and the colour bar beside it.
        const QRectF drawn = view->paintedRect(QFontMetricsF(QucsSettings.font));
        QVERIFY2(drawn.contains(QPointF(view->cx + view->x2 + 20, view->cy - view->y2 / 2)) && drawn.contains(QPointF(view->cx + 1, view->cy - 1))
                     && drawn.top() > view->cy - view->y2 - 60 && drawn.left() > view->cx - 120,
                 qPrintable(QStringLiteral("%1 %2 %3 %4").arg(drawn.left()).arg(drawn.top()).arg(drawn.right()).arg(drawn.bottom())));
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("map.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        view = dynamic_cast<ContourDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->levels == 4 && view->map == ContourDiagram::Turbo && !view->filled && !view->labels && view->passMin == 4 && view->passMax == 16);
        QVERIFY(!view->zAxis.autoScale && view->zAxis.low == 0 && view->zAxis.up == 40);
        QCOMPARE(view->grids().size(), 3);
        // The dialog.
        auto* dialog = new DiagramDialog(view, nullptr);
        auto* levels = dialog->findChild<QSpinBox*>("contourLevels");
        auto* passMax = dialog->findChild<QLineEdit*>("contourPassMax");
        QVERIFY(levels && passMax);
        QCOMPARE(levels->value(), 4);
        levels->setValue(12);
        passMax->setText("");
        dialog->findChild<QCheckBox*>("contourFilled")->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(view->levels == 12 && std::isnan(view->passMax) && view->passMin == 4 && view->filled);
        dialog->close();
    }

    // ---- The tornado chart: a sensitivity run's parts, sorted, the ones
    // of nothing counted; spreads; in the tools, the file, the dialog and
    // the readout.
    void tornadoChart()
    {
        // As ngspice's .SENS of a divider writes it (temp swept 27 to 77).
        const QMap<QString, QVector<double>> sens{{"r1", {-0.00210925, -0.00210925, -0.00210925}},
                                                  {"r1_m", {2.109248, 2.109248, 2.109248}},
                                                  {"r1_scale", {-2.10925, -2.10925, -2.10925}},
                                                  {"r1_tce", {0, -0.524693, -1.04939}},
                                                  {"r2", {0.0005408323, 0.0005408323, 0.0005408323}},
                                                  {"r2_scale", {1.622497, 1.622497, 1.622497}},
                                                  {"r3", {4.867491e-05, 4.867491e-05, 4.867491e-05}},
                                                  {"r3_scale", {0.4867491, 0.4867491, 0.4867491}},
                                                  {"r4", {0, 0, 0}},
                                                  {"r4_scale", {0, 0, 0}},
                                                  {"v1", {0.6976744, 0.6976744, 0.6976744}},
                                                  {"v1_freq", {0, 0, 0}}};
        const QVector<double> temp{27, 52, 77};
        QList<Dep> deps;
        for (auto it = sens.cbegin(); it != sens.cend(); ++it) {
            const QVector<double> values = it.value();
            deps << Dep{it.key(), [values, temp](double t) { return values.at(int(temp.indexOf(t))); }};
        }
        const QVector<double> runs{1, 2, 3, 4, 5};
        QVERIFY(schematicWith("sens", datasetText("temp_sweep", temp, deps)
                                          + datasetText("run", runs, {{"mc.a", [](double r) { return r; }},
                                                                      {"mc.b", [](double r) { return int(r) % 2 ? 0.0 : 0.5; }}})));
        // A part each: its _scale where it has one; those of nothing left out.
        QCOMPARE(TornadoDiagram::sensitivityParts(sens.keys()), (QStringList{"r1_scale", "r2_scale", "r3_scale", "r4_scale", "v1"}));
        // (Another run's variables, and the sweep, no parts.)
        QCOMPARE(TornadoDiagram::sensitivityParts(QStringList(sens.keys()) << "mc.a" << "run" << "temp_sweep"),
                 (QStringList{"r1_scale", "r2_scale", "r3_scale", "r4_scale", "v1"}));
        QCOMPARE(TornadoDiagram::sensitivityParts(sens.keys(), [&](const QString& n) { return sens.value(n).first() != 0; }),
                 (QStringList{"r1_scale", "r2_scale", "r3_scale", "v1"}));
        QJsonObject res = call("add_diagram", {{"type", "tornado"}, {"x", 150}, {"y", 400}, {"width", 300}, {"height", 200}, {"traces", QJsonArray{"v1"}},
                                               {"tornado", QJsonObject{{"parts", true}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QVERIFY2(text(res).contains("3 part(s) added"), qPrintable(text(res)));
        auto* view = dynamic_cast<TornadoDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view);
        QCOMPARE(view->Graphs.size(), 4);
        // Sorted by size, the largest on top.
        QStringList order;
        for (const TornadoDiagram::Bar& b : view->shown()) order << b.name;
        QCOMPARE(order, (QStringList{"r1_scale", "r2_scale", "v1", "r3_scale"}));
        // Laid out again (resized): the same bars - its traces' data kept.
        view->x2 += 20;
        view->updateGraphData();
        view->calcDiagram();
        QCOMPARE(view->shown().size(), 4);
        view->x2 -= 20;
        QCOMPARE(view->shown().first().value, -2.10925);
        const QJsonObject t = json(res).value("tornado").toObject();
        QCOMPARE(t.value("shown").toArray().first().toObject().value("variable").toString(), QString("r1_scale"));
        QVERIFY(t.value("note").toString().contains("per 100 %"));
        // Both ways from 0 alike.
        QVERIFY(std::abs(view->xAxis.low + view->xAxis.up) < 1e-9 * view->xAxis.up && view->xAxis.up > 2.10925);
        // Drawn: a bar down from 0 in one colour, up in the other; its
        // name left of the frame.
        const auto render = [&]() {
            QImage img(view->x2 + 400, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(250 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&p);
            return img;
        };
        const auto pixel = [&](const QImage& img, double x, int row) {
            const double y[2] = {0, 0};
            float px = 0, py = 0;
            view->calcCoordinate(&x, y, nullptr, &px, &py, &view->yAxis);
            const double h = double(view->y2) / view->shown().size();
            return img.pixelColor(250 + int(px), 50 + int((row + 0.5) * h));
        };
        QImage shown = render();
        QCOMPARE(pixel(shown, -1.0, 0), QColor(238, 133, 74));
        QCOMPARE(pixel(shown, 0.8, 1), QColor(72, 120, 208));
        QVERIFY(pixel(shown, 1.7, 3) != QColor(72, 120, 208));   // r3_scale: 0.49
        int named = 0;
        for (int x = 250 - 60; x < 250 - 4; ++x)
            for (int y = 50; y < 50 + view->y2 / 4; ++y) named += shown.pixelColor(x, y).value() < 100 ? 1 : 0;
        QVERIFY(named > 5);
        QVERIFY(view->paintedRect(QFontMetricsF(QucsSettings.font)).left() < view->cx - 30);
        // (Its traces' names are no y label: the bars are named. Laid out
        // with its labels: the x axis' says what the bars are.)
        view->updateGraphData();
        bool xLabel = false;
        for (const Text* text : view->Texts) {
            QVERIFY2(!text->s.contains("v1"), qPrintable(text->s));
            xLabel |= text->s.startsWith("at ");
        }
        QVERIFY(xLabel);
        // The readout: the bar under the cursor.
        const QString read = qucs_s::status::readout(view, MappedPoint{-1.0, 3.5, 0});
        QVERIFY2(read.contains(QStringLiteral("r1_scale \u22122.109")), qPrintable(read));
        // Of nothing at the first point, then not; at most two.
        QVERIFY(!failed(call("add_trace", {{"variable", "r1_tce"}})));
        QCOMPARE(view->nothing(), 1);
        QVERIFY(!failed(call("edit_diagram", {{"tornado", QJsonObject{{"at", 77}, {"bars", 2}}}})));
        QCOMPARE(view->nothing(), 0);
        QCOMPARE(view->smaller(), 3);
        QCOMPARE(view->shown().size(), 2);
        // 'parts' again: none more.
        res = call("edit_diagram", {{"tornado", QJsonObject{{"parts", true}}}});
        QVERIFY2(!failed(res) && view->Graphs.size() == 5, qPrintable(text(res)));
        // Refused.
        QVERIFY(failed(call("edit_diagram", {{"tornado", QJsonObject{{"mode", "median"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"tornado", QJsonObject{{"bars", 0}}}})));
        QVERIFY(failed(call("edit_diagram", {{"tornado", QJsonObject{{"at", "abc"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"tornado", QJsonObject{{"parts", 1}}}})));
        res = call("add_marker", {{"trace", 1}, {"at", 27}});
        QVERIFY(failed(res) && text(res).contains("no markers"));
        // Spreads: over the runs, from the lowest to the highest.
        res = call("add_diagram", {{"type", "tornado"}, {"x", 150}, {"y", 800}, {"traces", QJsonArray{"mc.b", "mc.a"}}, {"tornado", QJsonObject{{"mode", "spread"}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* spread = dynamic_cast<TornadoDiagram*>(front()->a_DocDiags.back());
        QVERIFY(spread && spread->shown().size() == 2);
        QCOMPARE(spread->shown().first().name, QString("mc.a"));
        QVERIFY(spread->shown().first().low == 1 && spread->shown().first().high == 5 && spread->shown().first().value == 1);
        QCOMPARE(json(res).value("tornado").toObject().value("shown").toArray().at(1).toObject().value("spread").toDouble(), 0.5);
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("sens.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        view = dynamic_cast<TornadoDiagram*>(front()->a_DocDiags.front());
        spread = dynamic_cast<TornadoDiagram*>(front()->a_DocDiags.back());
        QVERIFY(view && view->at == 77 && view->bars == 2 && view->mode == TornadoDiagram::Value && view->Graphs.size() == 5);
        QVERIFY(spread && spread->mode == TornadoDiagram::Spread);
        // The dialog: its settings; a run's parts by a button.
        auto* dialog = new DiagramDialog(view, front());
        auto* mode = dialog->findChild<QComboBox*>("tornadoMode");
        auto* bars = dialog->findChild<QSpinBox*>("tornadoBars");
        QVERIFY(mode && bars && dialog->findChild<QLineEdit*>("tornadoAt"));
        QCOMPARE(dialog->findChild<QLineEdit*>("tornadoAt")->text(), QString("77"));
        bars->setValue(10);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(view->bars, 10);
        dialog->close();
        QVERIFY(!failed(call("add_diagram", {{"type", "tornado"}, {"x", 600}, {"y", 400}, {"traces", QJsonArray{"v1"}}})));
        auto* fresh = dynamic_cast<TornadoDiagram*>(front()->a_DocDiags.back());
        dialog = new DiagramDialog(fresh, front());
        auto* parts = dialog->findChild<QPushButton*>("tornadoParts");
        QVERIFY(parts);
        parts->click();
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QStringList vars;
        for (const Graph* g : fresh->Graphs) vars << g->Var.section('/', -1);
        QVERIFY2(vars.contains("r1_scale") && vars.contains("r2_scale") && vars.contains("r4_scale") && vars.size() == 5, qPrintable(vars.join(", ")));
        dialog->close();
    }

    // ---- A marker a file gives a diagram that has none (its traces not
    // drawn as curves): read, drawn and saved without harm.
    void aMarkerInAFileOnADiagramWithoutMarkers()
    {
        QVERIFY(schematicWith("mk", datasetText("time", range(0, 1e-3, 201), {{"tran.v", [](double t) { return std::sin(2 * 3.14159265 * 5e3 * t); }}})));
        for (const QString type : {QStringLiteral("spectrum"), QStringLiteral("bathtub"), QStringLiteral("contour"), QStringLiteral("tornado"),
                                   QStringLiteral("eye"), QStringLiteral("pole_zero")})
            QVERIFY2(!failed(call("add_diagram", {{"type", type}, {"traces", QJsonArray{"tran.v"}}})), qPrintable(type));
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        // Each trace with a marker at 0.5 ms.
        QFile file(path("mk.sch"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        file.close();
        int markers = 0;
        QStringList lines = content.split('\n');
        for (int i = 0; i < lines.size(); ++i)
            if (lines.at(i).trimmed().startsWith("<\"ngspice/tran.v\"")) {
                lines.insert(i + 1, "\t  <Mkr 0.0005 20 -30 3 0 0>");
                ++markers;
            }
        QCOMPARE(markers, 6);
        QVERIFY(writeFile(path("mk.sch"), lines.join('\n')));
        QJsonObject res = call("open_document", {{"path", path("mk.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QCOMPARE(int(front()->a_DocDiags.size()), 6);
        for (Diagram* d : front()->a_DocDiags) {
            QImage img(800, 600, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(100 - d->cx, 400 - d->cy);
            d->paintDiagram(&p);
            for (Graph* g : d->Graphs)
                for (Marker* m : g->Markers) {
                    m->createText();
                    m->paint(&p);
                }
        }
        QVERIFY(!failed(call("get_schematic", {})));
        QVERIFY(!failed(call("save_document", {})));
    }

    // ---- A kept run behind this one: ghosts of each trace (overlay), a
    // trace of a kept run (run), faint and behind; in the file and the
    // dialog.
    void keptRunOverlay()
    {
        constexpr double Pi = 3.14159265358979323846;
        const QVector<double> t = range(0, 1e-3, 401);
        QVERIFY(schematicWith("ov", datasetText("time", t, {{"tran.v(out)", [&](double s) { return std::sin(2 * Pi * 1e3 * s); }},
                                                            {"tran.w", [&](double s) { return s * 1e3; }}})));
        QVERIFY(writeFile(path("before.dat.ngspice"), datasetText("time", t, {{"tran.v(out)", [&](double s) { return 0.5 * std::sin(2 * Pi * 1e3 * s); }},
                                                                              {"tran.w", [&](double s) { return 1 - s * 1e3; }}})));
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}, {"x", 100}, {"y", 400}, {"width", 400}, {"height", 200}, {"traces", QJsonArray{"tran.v(out)"}},
                                             {"y_axis", QJsonObject{{"auto", false}, {"from", -1.2}, {"to", 1.2}, {"step", 0.4}}}})));
        Diagram* d = front()->a_DocDiags.front();
        QVERIFY(!failed(call("edit_trace", {{"trace", 1}, {"color", "#008000"}})));
        // A ghost of each trace from the run kept as before, in its colour.
        QJsonObject res = call("edit_diagram", {{"overlay", "before"}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QCOMPARE(d->Graphs.size(), 2);
        Graph* ghost = d->Graphs.at(1);
        QCOMPARE(ghost->Var, QString("ngspice/before:tran.v(out)"));
        QVERIFY(ghost->ghost && !d->Graphs.first()->ghost && ghost->Color == d->Graphs.first()->Color);
        QVERIFY(!ghost->isEmpty());
        const QJsonArray traces = json(res).value("traces").toArray();
        QVERIFY(traces.at(1).toObject().value("ghost").toBool() && !traces.at(0).toObject().contains("ghost"));
        // Again: none twice. Not a run kept, not a name: refused.
        QVERIFY(!failed(call("edit_diagram", {{"overlay", "before"}})));
        QCOMPARE(d->Graphs.size(), 2);
        res = call("edit_diagram", {{"overlay", "nothere"}});
        QVERIFY(failed(res) && text(res).contains("no run kept as nothere"));
        QVERIFY(failed(call("edit_diagram", {{"overlay", "a b"}})));
        QCOMPARE(d->Graphs.size(), 2);
        // Drawn: the ghost faint, the run's own in full, over it where they
        // cross.
        QVERIFY(!failed(call("edit_trace", {{"trace", 2}, {"color", "#ff0000"}})));
        const auto render = [&]() {
            QImage img(d->x2 + 200, d->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(100 - d->cx, 50 + d->y2 - d->cy);
            d->paintDiagram(&p);
            return img;
        };
        // (The most coloured pixel near a point of the curves.)
        const auto pixel = [&](const QImage& img, double x, double y) {
            const double yd[2] = {y, 0};
            float px = 0, py = 0;
            d->calcCoordinate(&x, yd, nullptr, &px, &py, &d->yAxis);
            QColor most(Qt::white);
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    const QColor c = img.pixelColor(100 + int(px) + dx, 50 + d->y2 - int(py) + dy);
                    if (c.hsvSaturation() > most.hsvSaturation()) most = c;
                }
            return most;
        };
        QImage shown = render();
        // (Its labels the run's own: a ghost's is in the legend.)
        int timeLabels = 0;
        for (const Text* text : d->Texts) {
            QVERIFY2(!text->s.contains("before"), qPrintable(text->s));
            timeLabels += text->s == "time" ? 1 : 0;
        }
        QCOMPARE(timeLabels, 1);
        // (Its names stripped as one run's are: its simulation's left off.)
        bool stripped = false;
        for (const Text* text : d->Texts) stripped = stripped || text->s == "v(out)";
        QVERIFY(stripped);
        const QColor faint = pixel(shown, 0.25e-3, 0.5), full = pixel(shown, 0.25e-3, 1.0), crossing = pixel(shown, 0.5e-3, 0.0);
        // (Red at a third - two segments over each other at a vertex, at
        // most two thirds; drawn in full, no green at all.)
        QVERIFY2(faint.red() == 255 && faint.green() > 70 && faint.green() < 210, qPrintable(faint.name()));
        QVERIFY2(full.green() > 100 && full.red() < 60 && full.blue() < 60, qPrintable(full.name()));
        QVERIFY2(crossing.green() > 100 && crossing.red() < 40, qPrintable(crossing.name()));
        // Behind: a ghost of the very same curve hides nothing of it.
        QVERIFY(writeFile(path("same.dat.ngspice"), datasetText("time", t, {{"tran.v(out)", [&](double s) { return std::sin(2 * Pi * 1e3 * s); }}})));
        QVERIFY(!failed(call("add_trace", {{"variable", "tran.v(out)"}, {"run", "same"}, {"color", "#ff0000"}})));
        {
            const QImage over = render();
            for (double at : {0.2e-3, 0.25e-3, 0.3e-3, 0.7e-3}) {
                const QColor c = pixel(over, at, std::sin(2 * Pi * 1e3 * at));
                QVERIFY2(c.green() > 100 && c.red() < 40, qPrintable(c.name()));
            }
        }
        QVERIFY(!failed(call("delete", {{"traces", QJsonArray{QJsonObject{{"diagram", 1}, {"trace", 3}}}}})));
        // A trace of a kept run: a ghost unless said otherwise.
        res = call("add_trace", {{"variable", "tran.w"}, {"run", "before"}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QVERIFY(d->Graphs.last()->Var == "ngspice/before:tran.w" && d->Graphs.last()->ghost);
        QVERIFY(!failed(call("edit_trace", {{"trace", 3}, {"ghost", false}})));
        QVERIFY(!d->Graphs.last()->ghost);
        QVERIFY(failed(call("add_trace", {{"variable", "tran.w"}, {"run", "nothere"}})));
        QVERIFY(failed(call("add_trace", {{"variable", "tran.w"}, {"run", "x/y"}})));
        QVERIFY(failed(call("edit_trace", {{"trace", 3}, {"ghost", "yes"}})));
        // Kept with the schematic (a field after the pane's).
        QVERIFY(!failed(call("save_document", {})));
        QFile file(path("ov.sch"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        // (Its line: colour, thickness, precision, numbers, style, axis; auto
        // colour, marker, part, pane; ghost.)
        const QString saved = QString::fromUtf8(file.readAll());
        QVERIFY2(saved.contains(QRegularExpression("\"ngspice/before:tran.v\\(out\\)\" #ff0000 \\d+ \\d+ \\d+ \\d+ 0 0 0 0 0 1>")), qPrintable(saved));
        file.close();
        closeAll();
        res = call("open_document", {{"path", path("ov.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        d = front()->a_DocDiags.front();
        QVERIFY(d->Graphs.size() == 3 && d->Graphs.at(1)->ghost && !d->Graphs.at(2)->ghost);
        // The dialog: the chosen trace's ghost box.
        auto* dialog = new DiagramDialog(d, front());
        auto* box = dialog->findChild<QCheckBox*>("traceGhost");
        auto* list = dialog->findChild<QTableWidget*>();
        QVERIFY(box);
        QTableWidget* graphs = nullptr;
        for (QTableWidget* w : dialog->findChildren<QTableWidget*>())
            if (w->rowCount() == 3) graphs = w;
        QVERIFY(graphs && list);
        graphs->setCurrentCell(2, 0);
        QMetaObject::invokeMethod(dialog, "slotSelectGraph", Q_ARG(QTableWidgetItem*, graphs->item(2, 0)));
        QVERIFY(!box->isChecked() && box->isEnabled());
        box->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(d->Graphs.at(2)->ghost && d->Graphs.at(1)->ghost && !d->Graphs.at(0)->ghost);
        dialog->close();
        // No ghosts.
        QVERIFY(!failed(call("edit_diagram", {{"overlay", QJsonValue()}})));
        QCOMPARE(d->Graphs.size(), 1);
        // A table draws no curves to fade.
        QVERIFY(!failed(call("add_diagram", {{"type", "tab"}, {"x", 600}, {"y", 400}, {"traces", QJsonArray{"tran.w"}}})));
        QVERIFY(failed(call("add_trace", {{"diagram", 2}, {"variable", "tran.v(out)"}, {"ghost", true}})));
        QVERIFY(failed(call("edit_diagram", {{"diagram", 2}, {"overlay", "before"}})));
    }

    // ---- Values at the marker: the named nets labelled with their values
    // where a marker is - a transient's time, an AC sweep's point, a kept
    // run's - following it; in the tools and the menu.
    void valuesAtTheMarker()
    {
        constexpr double Pi = 3.14159265358979323846;
        const QString sch = path("cv.sch");
        QVERIFY(writeFile(sch, QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
                                              "<Components>\n</Components>\n<Wires>\n"
                                              "  <300 100 400 100 \"a\" 350 70 0 \"\">\n  <300 200 400 200 \"b\" 350 170 0 \"\">\n"
                                              "  <300 300 400 300 \"\" 0 0 0 \"\">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")));
        const QVector<double> t = range(0, 1e-3, 11), f = range(100, 10000, 100);
        QVERIFY(writeFile(path("cv.dat.ngspice"), datasetText("time", t, {{"tran.v(a)", [](double s) { return s * 1e3; }}, {"tran.v(b)", [](double) { return 2.0; }}})
                                                      + datasetText("frequency", f, {{"ac.v(a)", [](double x) { return 1 / (1 + std::pow(x / 1000, 2)); },
                                                                                      [](double x) { return -(x / 1000) / (1 + std::pow(x / 1000, 2)); }}})));
        QVERIFY(writeFile(path("kept.dat.ngspice"), datasetText("time", t, {{"tran.v(a)", [](double s) { return 1 - s * 1e3; }}, {"tran.v(b)", [](double) { return 3.0; }}})));
        QVERIFY(!failed(call("open_document", {{"path", sch}})));
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}, {"x", 500}, {"y", 300}, {"traces", QJsonArray{"tran.v(a)"}}})));
        // No marker yet: nothing, said why.
        Schematic* doc = front();
        QVERIFY(qucs_s::cursor::source(doc) == nullptr);
        QVERIFY(qucs_s::cursor::at(doc, nullptr).error.contains("no marker"));
        // A marker between samples: each named net there, straight between.
        QJsonObject res = call("add_marker", {{"trace", 1}, {"at", 0.25e-3}, {"annotate", true}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QVERIFY(doc->cursorValuesShown());
        QJsonObject v = json(res).value("values at the marker").toObject();
        QCOMPARE(v.value("x").toString(), QString("time"));
        QJsonArray values = v.value("values").toArray();
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.at(0).toObject().value("net").toString(), QString("a"));
        // (The marker on the nearest sample: 0.2 or 0.3 ms.)
        const double at = v.value("at").toDouble();
        QVERIFY(std::abs(values.at(0).toObject().value("value").toDouble() - at * 1e3) < 1e-9);
        QCOMPARE(values.at(1).toObject().value("value").toDouble(), 2.0);
        // It follows the marker.
        res = call("edit_marker", {{"marker", 1}, {"at", 0.8e-3}});
        QVERIFY(!failed(res));
        v = json(res).value("values at the marker").toObject();
        QVERIFY(std::abs(v.value("values").toArray().at(0).toObject().value("value").toDouble() - 0.8) < 1e-9);
        // Between samples, straight: the reading at a marker's x itself.
        {
            Marker* m = doc->a_DocDiags.front()->Graphs.front()->Markers.front();
            const qucs_s::cursor::Reading r = qucs_s::cursor::at(doc, m);
            QVERIFY(r.error.isEmpty() && r.values.size() == 2);
            QCOMPARE(r.values.at(0).anchor, QPoint(300, 100));   // (where its label is on the wire)
            // Between samples (0.2 and 0.3 ms): straight between them.
            const double kept = m->varPos().front();
            m->setPos(0.25e-3);
            QVERIFY(std::abs(qucs_s::cursor::at(doc, m).values.at(0).value - 0.25) < 1e-12);
            m->setPos(kept);
            m->createText();
        }
        // Drawn on the canvas, in a colour of their own.
        {
            doc->resize(900, 700);
            doc->viewport()->resize(900, 700);
            doc->centerOn(QPoint(300, 150));   // (its labels left of the wires)
            const QImage canvas = doc->viewport()->grab().toImage();
            int own = 0;
            for (int y = 0; y < canvas.height(); ++y)
                for (int x = 0; x < canvas.width(); ++x) {
                    const QColor c = canvas.pixelColor(x, y);
                    own += std::abs(c.red() - 123) < 12 && std::abs(c.green() - 31) < 12 && std::abs(c.blue() - 162) < 12 ? 1 : 0;
                }
            QVERIFY2(own > 20, qPrintable(QString::number(own)));
        }
        // Laid out beside their labels, in a colour of their own.
        const Schematic::BiasLabels layout = doc->layoutCursorValues(QFontMetrics(QucsSettings.font));
        QCOMPARE(layout.texts.size(), 2);
        QCOMPARE(layout.placements.size(), 2);
        QCOMPARE(layout.texts.at(1), QString("2 V"));
        QCOMPARE(layout.texts.at(0), QString("800 mV"));
        // get_schematic lists them while they are shown.
        QVERIFY(json(call("get_schematic", {})).contains("values at the marker"));
        // An AC sweep's point: magnitude and phase.
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}, {"x", 300}, {"y", 600}, {"traces", QJsonArray{"ac.v(a)"}}})));
        res = call("add_marker", {{"diagram", 2}, {"trace", 1}, {"at", 1000}, {"annotate", true}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        v = json(res).value("values at the marker").toObject();
        QCOMPARE(v.value("diagram").toInt(), 2);
        const QJsonObject a = v.value("values").toArray().at(0).toObject();
        QVERIFY2(std::abs(a.value("value").toDouble() - 1 / std::sqrt(2.0)) < 1e-3 && std::abs(a.value("phase").toDouble() + 45) < 0.1,
                 qPrintable(QJsonDocument(a).toJson()));
        QVERIFY(v.value("values").toArray().size() == 1);   // (b has no AC voltage)
        QVERIFY(layout.texts.size() == 2 && doc->layoutCursorValues(QFontMetrics(QucsSettings.font)).texts.first().contains(QStringLiteral("∠")));
        // A kept run's trace: the values of that run.
        QVERIFY(!failed(call("add_trace", {{"diagram", 1}, {"variable", "tran.v(a)"}, {"run", "kept"}})));
        res = call("add_marker", {{"diagram", 1}, {"trace", 2}, {"at", 0.2e-3}, {"annotate", true}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        values = json(res).value("values at the marker").toObject().value("values").toArray();
        QVERIFY(std::abs(values.at(0).toObject().value("value").toDouble() - 0.8) < 1e-9 && values.at(1).toObject().value("value").toDouble() == 3.0);
        // Not chosen: a marker selected, else the first.
        doc->setCursorMarker(nullptr);
        QCOMPARE(qucs_s::cursor::source(doc), doc->a_DocDiags.front()->Graphs.front()->Markers.front());
        Marker* ac = doc->a_DocDiags.back()->Graphs.front()->Markers.front();
        ac->isSelected = true;
        QCOMPARE(qucs_s::cursor::source(doc), ac);
        ac->isSelected = false;
        // Off; the menu's check follows the schematic in front.
        QVERIFY(!failed(call("edit_marker", {{"diagram", 1}, {"marker", 1}, {"annotate", false}})));
        QVERIFY(!doc->cursorValuesShown());
        QVERIFY(!json(call("get_schematic", {})).contains("values at the marker"));
        QVERIFY(doc->layoutCursorValues(QFontMetrics(QucsSettings.font)).texts.size() == 2);   // (laid out on demand)
        QVERIFY(failed(call("edit_marker", {{"diagram", 1}, {"marker", 1}, {"annotate", "yes"}})));
        QVERIFY(!app->cursorValuesAction->isChecked());
        app->cursorValuesAction->trigger();
        QVERIFY(doc->cursorValuesShown() && app->cursorValuesAction->isChecked());
        QVERIFY(schematicWith("other", datasetText("time", t, {{"tran.v(x)", [](double s) { return s; }}})));
        QVERIFY(front() != doc && !app->cursorValuesAction->isChecked());
        app->cursorValuesAction->trigger();
        QVERIFY(front()->cursorValuesShown() && doc->cursorValuesShown());
        (void)Pi;
    }

    // ---- Buses: a thick line named for the nets it gathers; it joins
    // nothing by touching - Check Schematic says when a net ends on it
    // with no member's name; in the tools, the file and the menu.
    void buses()
    {
        QStringList m;
        QVERIFY(BusPainting::membersOf("D[7:0]", &m) && m == (QStringList{"D7", "D6", "D5", "D4", "D3", "D2", "D1", "D0"}));
        QVERIFY(BusPainting::membersOf("A[0..2]", &m) && m == (QStringList{"A0", "A1", "A2"}));
        QVERIFY(BusPainting::membersOf(" addr[1:3] ", &m) && m.size() == 3);
        QVERIFY(!BusPainting::membersOf("D7:0", &m) && m.isEmpty());
        QVERIFY(!BusPainting::membersOf("D[3:3]", &m));
        QVERIFY(!BusPainting::membersOf("9D[1:0]", &m));
        BusPainting line(100, 100, 300, 100);
        QVERIFY(line.touches(QPoint(150, 100)) && line.touches(QPoint(300, 100)) && !line.touches(QPoint(150, 110)) && !line.touches(QPoint(310, 100)));

        // A bus D[1:0] and three wires drawn down from it: D0's, X's, and
        // one with no name; a resistor between D0 and D1's wires.
        const QString sch = path("bus.sch");
        QVERIFY(writeFile(sch, QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n"
                                              "<Components>\n  <R R1 1 200 230 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"european\" 0>\n</Components>\n<Wires>\n"
                                              "  <170 100 170 230 \"D0\" 180 150 0 \"\">\n  <230 100 230 230 \"D1\" 240 150 0 \"\">\n"
                                              "  <290 100 290 230 \"X\" 300 150 0 \"\">\n  <350 100 350 230 \"\" 0 0 0 \"\">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n"
                                              "  <Bus 100 100 300 0 \"D[1:0]\">\n  <Bus 100 400 200 0 \"\">\n</Paintings>\n")));
        QJsonObject res = call("open_document", {{"path", sch}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        Schematic* doc = front();
        auto* bus = dynamic_cast<BusPainting*>(doc->a_DocPaints.front());
        QVERIFY(bus && bus->busName == "D[1:0]" && bus->members() == (QStringList{"D1", "D0"}));
        // Check Schematic: X is none of its members; a net with no name
        // ends on it; a bus with no name. D0 and D1 nothing.
        QStringList said;
        for (const qucs_s::erc::Issue& i : qucs_s::erc::check(doc, false)) said << i.message;
        const QString all = said.join("\n");
        QVERIFY2(all.contains("net X ends on bus D[1:0] at 290, 100 but is none of its members"), qPrintable(all));
        QVERIFY2(all.contains("a net ends on bus D[1:0] at 350, 100 with no name"), qPrintable(all));
        QVERIFY2(all.contains("a bus with no name"), qPrintable(all));
        QVERIFY(!all.contains("net D0 ends") && !all.contains("net D1 ends"));
        // It joins nothing: D0 and D1 two nets, the resistor between them.
        const QString netlist = text(call("get_netlist", {}));
        QVERIFY2(netlist.contains(QRegularExpression("R1 D0 D1|R1 D1 D0")), qPrintable(netlist));
        // In the tools: listed with its members; named, refused a name that
        // is no bus's.
        const QJsonArray paintings = json(call("get_schematic", {})).value("paintings").toArray();
        QJsonObject first = paintings.first().toObject();
        QCOMPARE(first.value("type").toString(), QString("bus"));
        QCOMPARE(first.value("name").toString(), QString("D[1:0]"));
        QCOMPARE(first.value("members").toString(), QStringLiteral("D1 … D0"));
        res = call("add_painting", {{"type", "bus"}, {"from", QJsonArray{100, 500}}, {"to", QJsonArray{400, 500}}, {"name", "A[0:3]"}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QVERIFY(failed(call("add_painting", {{"type", "bus"}, {"from", QJsonArray{100, 600}}, {"to", QJsonArray{400, 600}}, {"name", "A0-3"}})));
        auto* added = dynamic_cast<BusPainting*>(doc->a_DocPaints.back());
        QVERIFY(added && added->busName == "A[0:3]" && added->x1 == 100 && added->x2 == 400);
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        QFile file(sch);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(file.readAll()).contains("<Bus 100 500 300 0 \"A[0:3]\">"));
        file.close();
        // Drawn thick, in its colour.
        {
            QImage img(500, 300, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            bus->paint(&p);
            p.end();
            int thick = 0;
            for (int y = 94; y <= 106; ++y) thick += img.pixelColor(200, y) == QColor(20, 40, 140) ? 1 : 0;
            QVERIFY2(thick >= 4, qPrintable(QString::number(thick)));
        }
        // Insert > Bus: one to draw.
        app->insBus->trigger();
        QVERIFY(dynamic_cast<BusPainting*>(app->view->selElem) != nullptr);
    }

    // ---- LTspice's .asc: its parts by their symbols' pins (turned), its
    // nets by wires and flags, its directives; UTF-16 and µ; a symbol of
    // its own beside it; what is left out said; imported and simulated.
    void ltspiceImport()
    {
        namespace lt = qucs_s::ltspice;
        QCOMPARE(lt::turned(QPoint(16, 96), "R90"), QPoint(-96, 16));
        QCOMPARE(lt::turned(QPoint(16, 96), "R180"), QPoint(-16, -96));
        QCOMPARE(lt::turned(QPoint(16, 96), "R270"), QPoint(96, -16));
        QCOMPARE(lt::turned(QPoint(16, 96), "M0"), QPoint(-16, 96));
        QCOMPARE(lt::turned(QPoint(16, 96), "M90"), QPoint(-96, -16));
        // V1 at 48,64; R1 at 208,64 turned (pins 192,80 and 112,80); C1 at
        // 224,80 (240,80 and 240,144); out flagged; a wire from part way
        // along V1's, flagged tap (V1's net's name); the subcircuit's pins
        // flagged a, b, c; a three-pin nmos's d, g, s.
        const QString asc = QString::fromUtf8(
            "Version 4\nSHEET 1 880 680\n"
            "WIRE 48 80 112 80\nWIRE 192 80 240 80\nWIRE 80 80 80 300\n"
            "FLAG 48 160 0\nFLAG 240 144 0\nFLAG 240 80 out\nFLAG 80 300 tap\n"
            "FLAG 400 96 a\nFLAG 400 64 b\nFLAG 432 80 c\nFLAG 848 64 d\nFLAG 800 144 g\nFLAG 848 160 s\n"
            "SYMBOL voltage 48 64 R0\nWINDOW 123 0 0 Left 0\nSYMATTR InstName V1\nSYMATTR Value SINE(0 1 1k) Rser=0.1\n"
            "SYMBOL res 208 64 R90\nWINDOW 0 0 56 VBottom 2\nSYMATTR InstName R1\nSYMATTR Value 1k\n"
            "SYMBOL cap 224 80 R0\nSYMATTR InstName C1\nSYMATTR Value 1\xC2\xB5\n"
            "SYMBOL mysub 400 64 R0\nSYMATTR InstName U1\n"
            "SYMBOL nosuchpart 600 64 R0\nSYMATTR InstName Z9\n"
            "SYMBOL res 700 64 R0\nSYMATTR InstName Rloose\nSYMATTR Value 10\n"
            "SYMBOL nmos 800 64 R0\nSYMATTR InstName M1\nSYMATTR Value IRF530\n"
            "TEXT 48 200 Left 2 !.tran 5m\nTEXT 48 230 Left 2 ;a comment\nTEXT 48 260 Left 2 !.param a=1\\n.options gmin=1e-12\n");
        // UTF-16 as LTspice 24 writes it, its mark first.
        QByteArray bytes("\xFF\xFE", 2);
        const QString crlf = QString(asc).replace("\n", "\r\n");
        bytes += QByteArray(reinterpret_cast<const char*>(crlf.utf16()), crlf.size() * 2);
        QCOMPARE(lt::textOf(bytes), asc);
        QCOMPARE(lt::textOf(asc.toLatin1()), asc);   // (Windows' 8 bits: µ 0xB5)
        // A subcircuit's symbol beside it: three pins, SpiceOrder 2 first.
        QVERIFY(writeFile(path("lt/mysub.asy"), "Version 4\nSymbolType CELL\nPIN 0 0 NONE 8\nPINATTR PinName B\nPINATTR SpiceOrder 2\n"
                                                "PIN 0 32 NONE 8\nPINATTR PinName A\nPINATTR SpiceOrder 1\nPIN 32 16 NONE 8\nPINATTR PinName C\nPINATTR SpiceOrder 3\n"
                                                "SYMATTR Prefix X\nSYMATTR SpiceModel MYSUB\n"));
        const lt::Conversion c = lt::convert(asc, {path("lt")}, "rc.asc");
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        const QStringList lines = c.netlist.split('\n');
        // (Its Rser a resistor of its own, as LTspice draws it.)
        QVERIFY2(lines.contains("V1 tap V1_rser SIN(0 1 1k)") && lines.contains("RV1_rser V1_rser 0 0.1"), qPrintable(c.netlist));
        QVERIFY2(lines.contains("R1 out tap 1k"), qPrintable(c.netlist));
        QVERIFY2(lines.contains("M1 d g s s IRF530"), qPrintable(c.netlist));   // (its bulk on its source)
        QVERIFY2(lines.contains("C1 out 0 1u"), qPrintable(c.netlist));
        // (LTspice's .tran of its stop time alone: a step put first.)
        QVERIFY2(lines.contains(".tran 5e-06 5m") && lines.contains(".param a=1") && lines.contains(".options gmin=1e-12"), qPrintable(c.netlist));
        QVERIFY(!c.netlist.contains("comment"));
        // Its own symbol's pins in their SpiceOrder: A, B, C.
        QVERIFY2(lines.contains("XU1 a b c MYSUB"), qPrintable(c.netlist));
        const QString notes = c.notes.join("\n");
        QVERIFY2(notes.contains("Z9 (nosuchpart)") && notes.contains("left out"), qPrintable(notes));
        QVERIFY2(notes.contains("V1: rser=0.1 made parts of their own (RV1_rser)"), qPrintable(notes));
        QVERIFY2(notes.contains("of Rloose at 716, 80 joins nothing"), qPrintable(notes));
        QCOMPARE(c.parts, 6);
        // Not an .asc; none of its parts known.
        QVERIFY(!lt::convert("* a netlist\nR1 1 0 1k\n").error.isEmpty());
        QVERIFY(!lt::convert("Version 4\nSYMBOL what 0 0 R0\n").error.isEmpty());
        // Imported: a schematic of its parts, simulated as LTspice would.
        QVERIFY(writeFile(path("lt/rc.asc"), QString()));
        {
            QFile f(path("lt/rc.asc"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(bytes);
        }
        QJsonObject res = call("import_netlist", {{"file", path("lt/rc.asc")}, {"save_as", path("lt/rc.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        const QJsonObject r = json(res);
        QVERIFY2(r.value("LTspice").toArray().first().toString().contains("6 parts"), qPrintable(text(res)));
        QVERIFY(r.value("netlist").toString().contains("R1 out tap 1k"));
        const QString netlist = text(call("get_netlist", {}));
        QVERIFY2(netlist.contains(QRegularExpression("R1\\s+out\\s+tap")), qPrintable(netlist));
        if (withNgspice()) {
            QVERIFY(!failed(call("delete", {{"names", QJsonArray{"XU1", "M1"}}})));
            res = call("simulate", {{"timeout", 120}});
            QVERIFY2(json(res).value("succeeded").toBool(), qPrintable(text(res)));
            {
                const QJsonObject v = json(call("get_dataset", {{"variables", QJsonArray{"tran.v(out)"}}, {"from", 2e-3}}))
                                          .value("variables").toArray().first().toObject();
                // An RC of 1 ms at 1 kHz: 1/sqrt(1 + (2 pi)^2) of the input.
                const double peak = v.value("max").toDouble();
                QVERIFY2(std::abs(peak - 1 / std::sqrt(1 + 4 * 3.14159265 * 3.14159265)) < 0.02, qPrintable(QString::number(peak)));
            }
        }
    }

    // ---- Wires by their nets' kinds: a supply's red and thicker, ground's
    // green, a signal's as ever - on the canvas, printed, in the tools.
    void colouredWires()
    {
        const QString sch = path("cw.sch");
        QVERIFY(writeFile(sch, QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n"
                                              "  <Vdc V1 1 100 130 18 -26 0 1 \"10 V\" 1>\n  <GND * 1 100 190 0 0 0 0>\n"
                                              "  <R R1 1 160 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"european\" 0>\n"
                                              "  <R R2 1 220 130 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"european\" 0>\n  <GND * 1 220 190 0 0 0 0>\n"
                                              "</Components>\n<Wires>\n"
                                              "  <100 160 100 190 \"\" 0 0 0 \"\">\n  <100 100 130 100 \"\" 0 0 0 \"\">\n"
                                              "  <190 100 220 100 \"out\" 200 70 10 \"\">\n  <220 160 220 190 \"\" 0 0 0 \"\">\n"
                                              "  <300 100 400 100 \"VCC\" 320 70 0 \"\">\n  <300 200 400 200 \"+5V\" 320 170 0 \"\">\n"
                                              "  <300 300 400 300 \"3V3\" 320 270 0 \"\">\n  <300 400 400 400 \"data\" 320 370 0 \"\">\n"
                                              "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")));
        QVERIFY(!failed(call("open_document", {{"path", sch}})));
        Schematic* doc = front();
        const auto kindAt = [&](int x1, int y1) {
            const QHash<const Wire*, qucs_s::erc::NetKind> kinds = qucs_s::erc::wireKinds(doc);
            for (const Wire* w : doc->a_DocWires)
                if (w->x1 == x1 && w->y1 == y1) return kinds.value(w, qucs_s::erc::NetKind::Signal);
            return qucs_s::erc::NetKind(-1);
        };
        using K = qucs_s::erc::NetKind;
        QCOMPARE(kindAt(100, 160), K::Ground);
        QCOMPARE(kindAt(220, 160), K::Ground);
        QCOMPARE(kindAt(100, 100), K::Supply);   // (the DC source's pin, its other on ground)
        QCOMPARE(kindAt(190, 100), K::Signal);
        QCOMPARE(kindAt(300, 100), K::Supply);   // VCC
        QCOMPARE(kindAt(300, 200), K::Supply);   // +5V
        QCOMPARE(kindAt(300, 300), K::Supply);   // 3V3
        QCOMPARE(kindAt(300, 400), K::Signal);   // data
        // In the tools: each wire's kind, a signal's none.
        int supplies = 0, grounds = 0, said = 0;
        for (const QJsonValue& v : json(call("get_schematic", {})).value("wires").toArray()) {
            const QString kind = v.toObject().value("kind").toString();
            supplies += kind == "supply" ? 1 : 0;
            grounds += kind == "ground" ? 1 : 0;
            said += v.toObject().contains("kind") ? 1 : 0;
        }
        QVERIFY(supplies == 4 && grounds == 2 && said == 6);
        // Printed (and exported) as chosen: off, as ever; on, by kind.
        const auto printed = [&]() {
            const QRect all = doc->allBoundingRect().marginsAdded(QMargins(20, 20, 20, 20));
            QImage img(all.size(), QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(-all.topLeft());
            doc->paintSchToViewpainter(&p, true);
            p.end();
            return std::pair{img, all.topLeft()};
        };
        const auto colourAt = [](const std::pair<QImage, QPoint>& shot, int x, int y) { return shot.first.pixelColor(QPoint(x, y) - shot.second); };
        QVERIFY(!QucsSettings.ColourWires && !app->colourWires->isChecked());
        auto shot = printed();
        QCOMPARE(colourAt(shot, 350, 100), QColor(Qt::darkBlue));
        app->colourWires->trigger();
        QVERIFY(QucsSettings.ColourWires);
        shot = printed();
        QCOMPARE(colourAt(shot, 350, 100), QColor(190, 30, 30));    // VCC
        QCOMPARE(colourAt(shot, 100, 175), QColor(0, 120, 40));     // ground
        QCOMPARE(colourAt(shot, 350, 400), QColor(Qt::darkBlue));   // data
        // On the canvas, the scene drawn again for it.
        doc->resize(900, 700);
        doc->viewport()->resize(900, 700);
        doc->centerOn(QPoint(250, 250));
        const auto reds = [&]() {
            const QImage canvas = doc->viewport()->grab().toImage();
            int n = 0;
            for (int y = 0; y < canvas.height(); ++y)
                for (int x = 0; x < canvas.width(); ++x) n += canvas.pixelColor(x, y) == QColor(190, 30, 30) ? 1 : 0;
            return n;
        };
        QVERIFY(reds() > 50);
        app->colourWires->trigger();
        QVERIFY(!QucsSettings.ColourWires);
        QCOMPARE(reds(), 0);
    }

    // ---- The spectrogram: a chirp's frequency rising column by column, a
    // steady tone's row; in the tools, the file and the dialog.
    void spectrogram()
    {
        constexpr double Pi = 3.14159265358979323846;
        // 1 kHz to 9 kHz over 10 ms; and 20 kHz, 20 dB down, all along.
        const auto f = [](double t) { return 1e3 + 8e5 * t; };
        const auto y = [&](double t) { return std::sin(2 * Pi * (1e3 * t + 4e5 * t * t)) + 0.1 * std::sin(2 * Pi * 20e3 * t); };
        QVERIFY(schematicWith("gram", datasetText("time", range(0, 10e-3, 10001), {{"tran.v(x)", y}})));
        QJsonObject res = call("add_diagram", {{"type", "spectrogram"}, {"x", 100}, {"y", 400}, {"width", 400}, {"height", 240}, {"traces", QJsonArray{"tran.v(x)"}},
                                               {"spectrogram", QJsonObject{{"segment", "2m"}, {"overlap", 0.5}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* view = dynamic_cast<SpectrogramDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->grids().size() == 1);
        const ContourDiagram::Grid& g = view->grids().first();
        QVERIFY2(g.ok(), qPrintable(g.error));
        // Columns 1 ms apart (2 ms, half shared): 9; rows 500 Hz apart.
        QCOMPARE(g.x.size(), 9);
        QVERIFY(std::abs(g.x.first() - 1e-3) < 1e-9 && std::abs(g.y.at(1) - 500) < 1e-6);
        // Each column's loudest: the chirp where the column is.
        const int nx = int(g.x.size());
        for (int i = 0; i < nx; ++i) {
            int best = 1;
            for (int j = 1; j < g.y.size(); ++j)
                if (g.at(i, j) > g.at(i, best)) best = j;
            QVERIFY2(std::abs(g.y.at(best) - f(g.x.at(i))) <= 1000, qPrintable(QStringLiteral("%1: %2 Hz").arg(g.x.at(i)).arg(g.y.at(best))));
            // The steady tone: about 20 dB below a sine of 1.
            const double tone = g.at(i, int(std::lround(20e3 / 500)));
            QVERIFY2(tone > -26 && tone < -18, qPrintable(QString::number(tone)));
        }
        // Up to a quarter past what is within 80 dB of the loudest (the tone
        // and its window's skirts), not to Nyquist.
        QVERIFY2(g.y.last() > 20e3 && g.y.last() < 40e3, qPrintable(QString::number(g.y.last())));
        // The colours over the 80 dB below the loudest (a whole dB).
        QVERIFY(view->zAxis.up == 0 && view->zAxis.low == -80);
        // In the tools: its settings, each trace's columns and loudest.
        const QJsonObject a = json(res).value("analyses").toArray().first().toObject();
        QCOMPARE(a.value("columns").toInt(), 9);
        QVERIFY(std::abs(a.value("resolution").toDouble() - 500) < 1e-6);
        QVERIFY(a.value("loudest").toObject().value("frequency").toDouble() > 1000);
        QCOMPARE(json(res).value("spectrogram").toObject().value("window").toString(), QString("hann"));
        // Drawn: bright where the chirp is, dark away from it.
        {
            QImage img(view->x2 + 300, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(100 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&p);
            p.end();
            const auto pixel = [&](double x, double yv) {
                const double yd[2] = {yv, 0};
                float px = 0, py = 0;
                view->calcCoordinate(&x, yd, nullptr, &px, &py, &view->yAxis);
                return img.pixelColor(100 + int(px), 50 + view->y2 - int(py));
            };
            const QColor loud = pixel(5e-3, f(5e-3)), quiet = pixel(5e-3, 15e3);
            // (Turbo: its loudest a dark red, its quietest a dark purple.)
            QVERIFY2(loud.red() > 100 && loud.red() > 3 * loud.blue() && quiet.red() < 80 && quiet.blue() > quiet.red(),
                     qPrintable(loud.name() + " " + quiet.name()));
        }
        // Edited; refused what is not one; a contour map's options too.
        QVERIFY(!failed(call("edit_diagram", {{"spectrogram", QJsonObject{{"window", "blackman_harris"}, {"range", 60}, {"segment", QJsonValue()}}}})));
        QVERIFY(view->window == qucs_s::spectrum::Window::BlackmanHarris && view->range == 60 && std::isnan(view->segment));
        QCOMPARE(view->grids().first().x.size(), 31);   // (a sixteenth, half shared: 31 columns)
        QVERIFY(!failed(call("edit_diagram", {{"spectrogram", QJsonObject{{"up_to", "10k"}}}})));
        QVERIFY(std::abs(view->grids().first().y.last() - 10e3) <= view->grids().first().y.at(1));
        QVERIFY(failed(call("edit_diagram", {{"spectrogram", QJsonObject{{"up_to", -5}}}})));
        QVERIFY(failed(call("edit_diagram", {{"spectrogram", QJsonObject{{"overlap", 0.95}}}})));
        QVERIFY(failed(call("edit_diagram", {{"spectrogram", QJsonObject{{"window", "triangle"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"spectrogram", QJsonObject{{"segment", -1}}}})));
        QVERIFY(!failed(call("edit_diagram", {{"contour", QJsonObject{{"levels", 3}}}})));
        QVERIFY(failed(call("add_diagram", {{"type", "contour"}, {"spectrogram", QJsonObject{{"overlap", 0}}}})));
        res = call("add_marker", {{"trace", 1}, {"at", 1e-3}});
        QVERIFY(failed(res) && text(res).contains("no markers"));
        // Kept with the schematic.
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("gram.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        view = dynamic_cast<SpectrogramDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->window == qucs_s::spectrum::Window::BlackmanHarris && view->range == 60 && std::isnan(view->segment) && view->levels == 3
                && view->upTo == 10e3);
        // The dialog.
        auto* dialog = new DiagramDialog(view, nullptr);
        auto* overlap = dialog->findChild<QDoubleSpinBox*>("spectrogramOverlap");
        auto* segment = dialog->findChild<QLineEdit*>("spectrogramSegment");
        QVERIFY(overlap && segment);
        overlap->setValue(0);
        segment->setText("1m");
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(view->overlap == 0 && view->segment == 1e-3);
        dialog->close();
    }

    // ---- The box plot: quartiles, median, mean, Tukey's whiskers and
    // outliers; a swept trace's curves at an x; in the tools, the file, the
    // dialog and the readout.
    void boxPlot()
    {
        // 1 to 9 and 100: the quartiles between the samples (3.25, 5.5,
        // 7.75), 100 beyond 1.5 times the box.
        const BoxPlotDiagram::Box b = BoxPlotDiagram::boxOf({5, 1, 9, 100, 2, 8, 3, 7, 4, 6}, false);
        QVERIFY(b.ok() && b.n == 10);
        QVERIFY(b.q1 == 3.25 && b.median == 5.5 && b.q3 == 7.75 && b.min == 1 && b.max == 100 && b.mean == 14.5);
        QVERIFY(b.low == 1 && b.high == 9 && b.outliers == QVector<double>{100});
        const BoxPlotDiagram::Box all = BoxPlotDiagram::boxOf({5, 1, 9, 100, 2, 8, 3, 7, 4, 6}, true);
        QVERIFY(all.low == 1 && all.high == 100 && all.outliers.isEmpty());
        QVERIFY(!BoxPlotDiagram::boxOf({}, false).ok());
        // A run's values (one per run), and a sweep of five curves.
        const QVector<double> runs{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        const QVector<double> vals{5, 1, 9, 100, 2, 8, 3, 7, 4, 6};
        QVERIFY(schematicWith("box", datasetText("run", runs, {{"mc.v", [&](double r) { return vals.at(int(r) - 1); }}})
                                        + datasetText2("time", range(0, 1e-3, 11), "k", QVector<double>{1, 2, 3, 4, 5},
                                                       {{"tran.w", [](double t, double k) { return k * t * 1e3; }}})));
        QJsonObject res = call("add_diagram", {{"type", "box_plot"}, {"x", 150}, {"y", 400}, {"width", 300}, {"height", 220},
                                               {"traces", QJsonArray{"mc.v", "tran.w"}}, {"box_plot", QJsonObject{{"at", 0.55e-3}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* view = dynamic_cast<BoxPlotDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->boxes().size() == 2);
        QVERIFY(view->boxes().at(0).median == 5.5 && view->boxes().at(0).outliers.size() == 1);
        // The sweep's five curves at 0.55 ms (between samples): 0.55 ... 2.75.
        QCOMPARE(view->boxes().at(1).n, 5);
        QVERIFY(std::abs(view->boxes().at(1).median - 1.65) < 1e-12 && std::abs(view->boxes().at(1).max - 2.75) < 1e-12);
        // No numbers along x: a column is a box.
        for (const Text* t : view->Texts)
            if (t->part == static_cast<unsigned char>(qucs_s::diagramtheme::Part::XAxis)) QVERIFY2(t->s.trimmed().isEmpty(), qPrintable(t->s));
        const QJsonArray boxes = json(res).value("box_plot").toObject().value("boxes").toArray();
        QCOMPARE(boxes.at(0).toObject().value("q3").toDouble(), 7.75);
        QCOMPARE(boxes.at(0).toObject().value("outliers").toArray().size(), 1);
        // get_dataset's distribution: the same box, without a diagram.
        const QJsonObject spread = json(call("get_dataset", {{"variables", QJsonArray{"mc.v"}}, {"measure", QJsonArray{"distribution"}}}))
                                       .value("variables").toArray().first().toObject().value("measurements").toObject().value("distribution").toObject();
        QVERIFY2(spread.value("1st quartile").toDouble() == 3.25 && spread.value("3rd quartile").toDouble() == 7.75
                     && spread.value("whiskers").toArray() == QJsonArray({1, 9}) && spread.value("outliers").toArray() == QJsonArray({100}),
                 qPrintable(QJsonDocument(spread).toJson()));
        // Up y: every value (the outlier too).
        QVERIFY(view->yAxis.up >= 100 && view->yAxis.low <= 0.5);
        // Drawn: a box at its column, its median across it.
        {
            QImage img(view->x2 + 200, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter p(&img);
            p.translate(100 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&p);
            p.end();
            const double x = 0, y[2] = {5.5, 0};
            float px = 0, py = 0;
            view->calcCoordinate(&x, y, nullptr, &px, &py, &view->yAxis);
            int ink = 0;
            for (int dy = -2; dy <= 2; ++dy) ink += img.pixelColor(100 + int(view->x2 / 4), 50 + view->y2 - int(py) + dy).blue() > 150 ? 1 : 0;
            QVERIFY(ink > 0);
        }
        // Readout: the box under the cursor.
        const QString read = qucs_s::status::readout(view, MappedPoint{0.5, 5.0, 0});
        QVERIFY2(read.contains("mc.v: median 5.5") && read.contains("10 values"), qPrintable(read));
        // Edited, refused, kept.
        QVERIFY(!failed(call("edit_diagram", {{"box_plot", QJsonObject{{"whiskers", "range"}, {"at", QJsonValue()}}}})));
        QVERIFY(view->range && std::isnan(view->at) && view->boxes().at(0).outliers.isEmpty());
        QVERIFY(std::abs(view->boxes().at(1).max - 5) < 1e-12);   // (the last values: 1 ms)
        QVERIFY(failed(call("edit_diagram", {{"box_plot", QJsonObject{{"whiskers", "iqr"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"box_plot", QJsonObject{{"at", "abc"}}}})));
        res = call("add_marker", {{"trace", 1}, {"at", 3}});
        QVERIFY(failed(res) && text(res).contains("no markers"));
        QVERIFY(!failed(call("edit_diagram", {{"box_plot", QJsonObject{{"at", 0.2e-3}}}})));
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("box.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        view = dynamic_cast<BoxPlotDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->range && view->at == 0.2e-3);
        auto* dialog = new DiagramDialog(view, nullptr);
        auto* rangeBox = dialog->findChild<QCheckBox*>("boxPlotRange");
        QVERIFY(rangeBox && rangeBox->isChecked());
        rangeBox->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(!view->range);
        dialog->close();
    }

    // ---- The constellation: QPSK with noise, sampled once a symbol - its
    // points, the ideal ones, the EVM; in the tools, the file, the dialog.
    void constellation()
    {
        // 400 symbols of 1 us, ten samples each (the sixth mid-symbol), each
        // sample I and Q of the symbol's corner (an rms of 2) and Gaussian
        // noise of 0.1.
        constexpr double Pi = 3.14159265358979323846;
        std::mt19937 random(11);
        const auto uniform = [&] { return (double(random()) + 0.5) / 4294967296.0; };
        const auto gaussian = [&] { return std::sqrt(-2 * std::log(uniform())) * std::cos(2 * Pi * uniform()); };
        QVector<double> t, i, q;
        double si = 0, sq = 0;
        for (int k = 0; k < 4000; ++k) {
            if (k % 10 == 0) {
                si = (random() % 2 ? 2 : -2) / std::sqrt(2.0);
                sq = (random() % 2 ? 2 : -2) / std::sqrt(2.0);
            }
            t << k * 0.1e-6;
            i << si + 0.1 * gaussian();
            q << sq + 0.1 * gaussian();
        }
        QVERIFY(schematicWith("iq", datasetText("time", t, {{"tran.i", [&](double x) { return i.at(int(std::lround(x / 0.1e-6))); }},
                                                            {"tran.q", [&](double x) { return q.at(int(std::lround(x / 0.1e-6))); }},
                                                            {"tran.z", [](double) { return 0.0; }}})));
        QJsonObject res = call("add_diagram", {{"type", "constellation"}, {"x", 150}, {"y", 400}, {"traces", QJsonArray{"tran.i", "tran.q", "tran.z"}},
                                               {"constellation", QJsonObject{{"symbol_period", "1u"}, {"offset", "0.5u"}, {"modulation", "qpsk"}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* view = dynamic_cast<ConstellationDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->pairs().size() == 2);
        const ConstellationDiagram::Pair& p = view->pairs().first();
        QVERIFY2(p.ok(), qPrintable(p.error));
        QCOMPARE(p.points.size(), 400);
        QCOMPARE(p.ideal.size(), 4);
        // The first at 0.5 us: the sixth sample.
        QVERIFY(std::abs(p.points.first().x() - i.at(5)) < 1e-12 && std::abs(p.points.first().y() - q.at(5)) < 1e-12);
        // The error vector: sqrt(2) times 0.1 of an rms of 2.
        QVERIFY2(std::abs(p.evmRms - 7.07) < 0.6, qPrintable(QString::number(p.evmRms)));
        QVERIFY(p.evmPeak > p.evmRms && std::abs(p.centre.x()) < 0.2);
        // A third trace, an I with no Q.
        QVERIFY(view->pairs().at(1).error.contains("no Q"));
        // 16-QAM's points: an rms of 1.
        const QList<QPointF> qam = ConstellationDiagram::idealPoints(ConstellationDiagram::Qam16);
        double power = 0;
        for (const QPointF& x : qam) power += x.x() * x.x() + x.y() * x.y();
        QVERIFY(qam.size() == 16 && std::abs(power / 16 - 1) < 1e-12);
        // I along and Q up on one scale about 0.
        QVERIFY(std::abs(view->xAxis.low + view->xAxis.up) < 1e-12 && view->xAxis.up == view->yAxis.up);
        const QJsonObject c = json(res).value("constellation").toObject();
        QCOMPARE(c.value("pairs").toArray().first().toObject().value("symbols").toInt(), 400);
        QVERIFY(std::abs(c.value("pairs").toArray().first().toObject().value("EVM rms %").toDouble() - p.evmRms) < 1e-3);
        // get_dataset's evm: the same, without a diagram; half a symbol in
        // when no offset is given (the range from 0: 0.5 us).
        const auto evmOf = [&](const QJsonObject& evm, const QJsonObject& more = {}) {
            QJsonObject args{{"variables", QJsonArray{"tran.i"}}, {"measure", QJsonArray{"evm"}}, {"evm", evm}};
            for (auto it = more.begin(); it != more.end(); ++it) args.insert(it.key(), it.value());
            const QJsonObject r = call("get_dataset", args);
            return failed(r) ? QJsonObject{{"refused", text(r)}}
                             : json(r).value("variables").toArray().first().toObject().value("measurements").toObject().value("evm").toObject();
        };
        QJsonObject evm = evmOf({{"q", "tran.q"}, {"symbol_period", "1u"}, {"modulation", "qpsk"}});
        QVERIFY2(evm.value("symbols").toInt() == 400 && std::abs(evm.value("EVM rms %").toDouble() - p.evmRms) < 1e-3
                     && std::abs(evm.value("value").toDouble() - p.evmRms) < 1e-3,
                 qPrintable(QJsonDocument(evm).toJson()));
        // From 100 us on: 300 symbols; no modulation: no EVM, said so.
        QCOMPARE(evmOf({{"q", "tran.q"}, {"symbol_period", "1u"}, {"modulation", "qpsk"}}, {{"from", 100e-6}}).value("symbols").toInt(), 300);
        evm = evmOf({{"q", "q"}, {"symbol_period", "1u"}});
        QVERIFY2(evm.value("value").isNull() && evm.value("note").toString().contains("modulation") && evm.value("Q").toString() == "tran.q",
                 qPrintable(QJsonDocument(evm).toJson()));
        // No Q, a Q there is none of, a modulation not one: said, refused.
        QVERIFY(evmOf({{"symbol_period", "1u"}}).value("error").toString().contains("needs the Q"));
        QVERIFY(evmOf({{"q", "tran.nothing"}}).value("error").toString().contains("no tran.nothing"));
        QVERIFY(evmOf({{"q", "tran.q"}, {"modulation", "256qam"}}).value("refused").toString().contains("modulation"));
        QVERIFY(evmOf({{"q", "tran.q"}, {"symbol_period", 0}}).value("refused").toString().contains("symbol_period"));
        // Drawn: a cross on an ideal point.
        {
            QImage img(view->x2 + 200, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter painter(&img);
            painter.translate(100 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&painter);
            painter.end();
            const QPointF ideal = p.ideal.first() + QPointF(0.04, 0.04) * (view->xAxis.up / 1.1);
            const double x = ideal.x(), y[2] = {ideal.y(), 0};
            float px = 0, py = 0;
            view->calcCoordinate(&x, y, nullptr, &px, &py, &view->yAxis);
            int black = 0;
            for (int dx = -3; dx <= 3; ++dx)
                for (int dy = -3; dy <= 3; ++dy) black += img.pixelColor(100 + int(px) + dx, 50 + view->y2 - int(py) + dy).value() < 60 ? 1 : 0;
            QVERIFY(black > 0);
        }
        // Every sample, no modulation: no EVM.
        QVERIFY(!failed(call("edit_diagram", {{"constellation", QJsonObject{{"symbol_period", QJsonValue()}, {"modulation", "none"}}}})));
        QVERIFY(view->pairs().first().points.size() == 4000 && view->pairs().first().ideal.isEmpty());
        QVERIFY(failed(call("edit_diagram", {{"constellation", QJsonObject{{"modulation", "256qam"}}}})));
        QVERIFY(failed(call("edit_diagram", {{"constellation", QJsonObject{{"symbol_period", 0}}}})));
        res = call("add_marker", {{"trace", 1}, {"at", 1e-6}});
        QVERIFY(failed(res) && text(res).contains("no markers"));
        QVERIFY(!failed(call("edit_diagram", {{"constellation", QJsonObject{{"symbol_period", "1u"}, {"from", "100u"}, {"modulation", "16qam"}}}})));
        QCOMPARE(view->pairs().first().points.size(), 300);
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("iq.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        view = dynamic_cast<ConstellationDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->period == 1e-6 && view->from == 100e-6 && view->modulation == ConstellationDiagram::Qam16 && view->offset == 0.5e-6);
        auto* dialog = new DiagramDialog(view, nullptr);
        auto* modulation = dialog->findChild<QComboBox*>("constellationModulation");
        QVERIFY(modulation && modulation->currentIndex() == ConstellationDiagram::Qam16);
        modulation->setCurrentIndex(ConstellationDiagram::Bpsk);
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QCOMPARE(view->modulation, int(ConstellationDiagram::Bpsk));
        dialog->close();
    }

    void smithCircles()
    {
        // A transistor's S-parameters (Gonzalez's) and noise parameters at
        // 3 GHz, the middle of five; at the others its gain halved.
        using C = std::complex<double>;
        constexpr double Pi = 3.14159265358979323846;
        const auto polar = [&](double m, double degrees) { return std::polar(m, degrees * Pi / 180); };
        const C s11 = polar(0.385, -55), s12 = polar(0.045, 90), s21 = polar(2.7, 78), s22 = polar(0.89, -26.5);
        const C gammaOpt = polar(0.62, 100);
        const auto at3 = [](double f, C v) { return std::abs(f - 3e9) < 1 ? v : v * 0.5; };
        QList<Dep> deps;
        const QList<QPair<QString, C>> s{{"ac.s_1_1", s11}, {"ac.s_1_2", s12}, {"ac.s_2_1", s21}, {"ac.s_2_2", s22}};
        for (const auto& p : s) {
            const C v = p.second;
            const bool gain = p.first == "ac.s_2_1";
            deps << Dep{p.first, [=](double f) { return gain ? at3(f, v).real() : v.real(); },
                        [=](double f) { return gain ? at3(f, v).imag() : v.imag(); }};
        }
        deps << Dep{"ac.nfmin", [](double) { return 1.6; }} << Dep{"ac.rn", [](double) { return 20.0; }}
             << Dep{"ac.sopt", [=](double) { return gammaOpt.real(); }, [=](double) { return gammaOpt.imag(); }};
        QVERIFY(schematicWith("amp", datasetText("frequency", range(1e9, 5e9, 5), deps)));
        QJsonObject res = call("add_diagram", {{"type", "smith"}, {"x", 150}, {"y", 400}, {"width", 300}, {"height", 300}, {"traces", QJsonArray{"ac.s_1_1"}},
                                               {"smith_circles", QJsonObject{{"circles", QJsonArray{QJsonObject{{"kind", "stability_in"}}, QJsonObject{{"kind", "stability_out"}},
                                                                                                   QJsonObject{{"kind", "gain"}, {"level", 10}},
                                                                                                   QJsonObject{{"kind", "noise"}, {"level", 2}}}}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        auto* view = dynamic_cast<SmithDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view);
        const SmithDiagram::CircleSet& set = view->circleSet();
        QVERIFY2(set.error.isEmpty(), qPrintable(set.error));
        QCOMPARE(set.frequency, 3e9);
        QCOMPARE(set.drawn.size(), 4);
        // Rollett's K, |delta| and mu (worked by hand: 0.909, 0.402, 0.985).
        QVERIFY(std::abs(set.s.k() - 0.9095) < 1e-3 && std::abs(std::abs(set.s.delta()) - 0.4017) < 1e-3 && std::abs(set.s.mu() - 0.9853) < 1e-3);
        // Each circle where it says: on the input stability circle (the
        // source's plane) |Gamma_out| = 1, on the output one |Gamma_in| = 1,
        // on the gain circle the available gain 10 dB, on the noise one the
        // noise figure 2 dB.
        const auto onCircle = [](const qucs_s::smith::Circle& c, double a) { return c.centre + std::polar(c.radius, a); };
        for (double a : {0.3, 1.9, 4.0}) {
            const C gs = onCircle(set.drawn.at(0).circle, a), gl = onCircle(set.drawn.at(1).circle, a);
            QVERIFY(std::abs(std::abs(s22 + s12 * s21 * gs / (1.0 - s11 * gs)) - 1) < 1e-9);
            QVERIFY(std::abs(std::abs(s11 + s12 * s21 * gl / (1.0 - s22 * gl)) - 1) < 1e-9);
            const C g = onCircle(set.drawn.at(2).circle, a);
            const C gOut = s22 + s12 * s21 * g / (1.0 - s11 * g);
            const double ga = std::norm(s21) * (1 - std::norm(g)) / (std::norm(1.0 - s11 * g) * (1 - std::norm(gOut)));
            QVERIFY2(std::abs(10 * std::log10(ga) - 10) < 1e-9, qPrintable(QString::number(10 * std::log10(ga))));
            const C n = onCircle(set.drawn.at(3).circle, a);
            const double f = std::pow(10, 0.16) + 4 * 20.0 / 50 * std::norm(n - gammaOpt) / ((1 - std::norm(n)) * std::norm(1.0 + gammaOpt));
            QVERIFY(std::abs(10 * std::log10(f) - 2) < 1e-9);
        }
        // The noise circle on Gamma_opt's line, 0.563 out, 0.245 wide.
        QVERIFY(std::abs(std::abs(set.drawn.at(3).circle.centre) - 0.5627) < 1e-3 && std::abs(set.drawn.at(3).circle.radius - 0.2454) < 1e-3);
        // The chart's middle stable (|S11|, |S22| < 1): inside the input
        // circle (which holds it), outside the output one (which does not).
        QVERIFY(set.drawn.at(0).circle.stableInside && !set.drawn.at(1).circle.stableInside);
        // get_dataset's stability, without a diagram: potentially unstable
        // at 3 GHz alone (its S21 there the whole), mu its least there.
        const auto stabilityOf = [&](const QString& variable, const QJsonArray& at = {}) {
            return json(call("get_dataset", {{"variables", QJsonArray{variable}}, {"measure", QJsonArray{"stability"}}, {"at", at}}))
                .value("variables").toArray().first().toObject().value("measurements").toObject().value("stability").toObject();
        };
        QJsonObject st = stabilityOf("ac.s_2_1", {1e9, 3e9, 9e9});
        const QJsonArray unstable = st.value("potentially unstable").toArray();
        QVERIFY2(!st.value("unconditionally stable").toBool() && unstable.size() == 1 && unstable.first().toArray().first().toDouble() == 3e9
                     && unstable.first().toArray().last().toDouble() == 3e9
                     && std::abs(st.value("mu min").toDouble() - 0.9853) < 1e-3 && st.value("mu min at").toDouble() == 3e9
                     && std::abs(st.value("value").toDouble() - 0.9853) < 1e-3 && st.value("points").toInt() == 5,
                 qPrintable(QJsonDocument(st).toJson()));
        const QJsonArray atFrequencies = st.value("at").toArray();
        // 1 GHz: stable, its maximum available gain 10.2 dB; 3 GHz: the
        // maximum stable gain, |S21/S12| (17.8 dB), the circles the chart's.
        QVERIFY(std::abs(atFrequencies.at(0).toObject().value("MAG dB").toDouble() - 10.197) < 1e-3);
        const QJsonObject three = atFrequencies.at(1).toObject();
        QVERIFY(std::abs(three.value("MSG dB").toDouble() - 10 * std::log10(2.7 / 0.045)) < 1e-4 && !three.contains("MAG dB"));
        QVERIFY(std::abs(three.value("output stability circle").toObject().value("radius").toDouble() - set.drawn.at(1).circle.radius) < 1e-5);
        QCOMPARE(three.value("output stability circle").toObject().value("stable").toString(), QString("outside"));
        QVERIFY(atFrequencies.at(2).toObject().value("error").toString().contains("outside"));
        // Each of the four is the two-port's - named as the examples' traces
        // name them too, ac.v(s_1_2) - ; a range of the stable ones.
        QCOMPARE(stabilityOf("s_1_2").value("mu min").toDouble(), st.value("mu min").toDouble());
        QCOMPARE(stabilityOf("ac.v(s_1_2)").value("mu min").toDouble(), st.value("mu min").toDouble());
        st = json(call("get_dataset", {{"variables", QJsonArray{"ac.s_1_1"}}, {"measure", QJsonArray{"stability"}}, {"to", 2e9}}))
                 .value("variables").toArray().first().toObject().value("measurements").toObject().value("stability").toObject();
        QVERIFY2(st.value("unconditionally stable").toBool() && !st.contains("potentially unstable") && st.value("points").toInt() == 2,
                 qPrintable(QJsonDocument(st).toJson()));
        QVERIFY(stabilityOf("ac.nfmin").value("error").toString().contains("is none"));
        // A range ending where it is unstable: that range closed there.
        st = json(call("get_dataset", {{"variables", QJsonArray{"ac.s_2_1"}}, {"measure", QJsonArray{"stability"}}, {"to", 3e9}}))
                 .value("variables").toArray().first().toObject().value("measurements").toObject().value("stability").toObject();
        QVERIFY2(st.value("potentially unstable").toArray().size() == 1 && st.value("potentially unstable").toArray().first().toArray().last().toDouble() == 3e9,
                 qPrintable(QJsonDocument(st).toJson()));
        // Between the samples: 2.5 GHz, S21 halfway from half to whole.
        const QJsonObject between = stabilityOf("ac.s_2_1", {2.5e9}).value("at").toArray().first().toObject();
        QVERIFY2(std::abs(between.value("K").toDouble() - qucs_s::smith::SParameters{s11, s12, s21 * 0.75, s22}.k()) < 1e-5,
                 qPrintable(QJsonDocument(between).toJson()));
        // The tools' account of them.
        const QJsonObject sc = json(res).value("smith_circles").toObject();
        QCOMPARE(sc.value("at").toDouble(), 3e9);
        QVERIFY(std::abs(sc.value("K").toDouble() - 0.9095) < 1e-3 && !sc.value("unconditionally stable").toBool());
        const QJsonArray circles = sc.value("circles").toArray();
        QCOMPARE(circles.size(), 4);
        QCOMPARE(circles.at(1).toObject().value("stable").toString(), QString("outside"));
        QVERIFY(std::abs(circles.at(1).toObject().value("radius").toDouble() - 0.1926) < 1e-3);
        QCOMPARE(circles.at(2).toObject().value("level").toDouble(), 10.0);
        // Under the chart: where they are of, and the verdict.
        bool caption = false;
        for (const Text* t : view->Texts) caption |= t->s.contains("Circles at 3 GHz: K 0.909") && t->s.contains("potentially unstable");
        QVERIFY(caption);
        // Drawn: the gain circle's blue where it crosses the chart.
        {
            QImage img(view->x2 + 200, view->y2 + 100, QImage::Format_RGB32);
            img.fill(Qt::white);
            QPainter painter(&img);
            painter.translate(100 - view->cx, 50 + view->y2 - view->cy);
            view->paintDiagram(&painter);
            painter.end();
            const qucs_s::smith::Circle& g = set.drawn.at(2).circle;
            int blue = 0;
            for (double a = 0; a < 2 * Pi; a += 0.05) {
                const C p = g.centre + std::polar(g.radius, a);
                if (std::abs(p) > 0.95) continue;
                const double x = 0, y[2] = {p.real(), p.imag()};
                float px = 0, py = 0;
                view->calcCoordinate(&x, y, nullptr, &px, &py, &view->yAxis);
                const QColor c = img.pixelColor(100 + int(std::lround(px)), 50 + view->y2 - int(std::lround(py)));
                blue += c.blue() > 150 && c.red() < 120 ? 1 : 0;
            }
            QVERIFY2(blue > 20, qPrintable(QString::number(blue)));
        }
        // At 1 GHz (the nearest): its own S21, another K.
        QVERIFY(!failed(call("edit_diagram", {{"smith_circles", QJsonObject{{"frequency", "1.2G"}}}})));
        QCOMPARE(view->circleSet().frequency, 1e9);
        QVERIFY(std::abs(view->circleSet().s.s21 - s21 * 0.5) < 1e-12 && view->circles.size() == 4);
        // There unconditionally stable (K 1.61), its gain at most 10.2 dB:
        // no circle of 12 dB, one of 10; none of a noise figure below the
        // minimum - said, the others drawn.
        res = call("edit_diagram", {{"smith_circles", QJsonObject{{"circles", QJsonArray{QJsonObject{{"kind", "gain"}, {"level", 12}},
                                                                                          QJsonObject{{"kind", "noise"}, {"level", 1}},
                                                                                          QJsonObject{{"kind", "gain"}, {"level", 10}},
                                                                                          QJsonObject{{"kind", "stability_out"}}}}}}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        QVERIFY(std::abs(view->circleSet().s.k() - 1.6079) < 1e-3);
        QVERIFY(!view->circleSet().drawn.at(0).circle.ok && view->circleSet().drawn.at(0).circle.why.contains("more than the available gain"));
        QVERIFY(view->circleSet().drawn.at(1).circle.why.contains("below the minimum"));
        QVERIFY(view->circleSet().drawn.at(2).circle.ok && view->circleSet().drawn.at(3).circle.ok);
        caption = false;
        for (const Text* t : view->Texts) caption |= t->s.contains("Circles at 1 GHz: K 1.61") && t->s.contains("(unconditionally stable)");
        QVERIFY(caption);
        const QJsonObject atOne = json(call("get_schematic", {})).value("diagrams").toArray().first().toObject().value("smith_circles").toObject();
        const QJsonArray none = atOne.value("circles").toArray();
        QVERIFY2(none.size() == 4 && none.at(0).toObject().value("none").toString().contains("12 dB") && atOne.value("unconditionally stable").toBool(),
                 qPrintable(QJsonDocument(atOne).toJson()));
        // Potentially unstable (3 GHz, the middle again): any gain has its
        // circle, 40 dB too.
        QVERIFY(!failed(call("edit_diagram", {{"smith_circles", QJsonObject{{"frequency", QJsonValue()}, {"circles", QJsonArray{QJsonObject{{"kind", "gain"}, {"level", 40}},
                                                                                                                                  QJsonObject{{"kind", "noise"}, {"level", 1}},
                                                                                                                                  QJsonObject{{"kind", "stability_out"}}}}}}})));
        QCOMPARE(view->circleSet().frequency, 3e9);
        QVERIFY(view->circleSet().drawn.at(0).circle.ok);
        // Refused: an unknown kind, a gain without its level, circles on a
        // chart that is no Smith chart.
        QVERIFY(failed(call("edit_diagram", {{"smith_circles", QJsonObject{{"circles", QJsonArray{QJsonObject{{"kind", "power"}}}}}}})));
        QVERIFY(failed(call("edit_diagram", {{"smith_circles", QJsonObject{{"circles", QJsonArray{QJsonObject{{"kind", "gain"}}}}}}})));
        QVERIFY(failed(call("edit_diagram", {{"smith_circles", QJsonObject{{"frequency", -1e9}}}})));
        QVERIFY(failed(call("edit_diagram", {{"smith_circles", QJsonObject{{"frequency", "fast"}}}})));
        QVERIFY(view->circles.size() == 3 && std::isnan(view->circleFrequency));
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}, {"x", 600}, {"y", 400}, {"traces", QJsonArray{"ac.s_2_1"}}})));
        res = call("edit_diagram", {{"diagram", 2}, {"smith_circles", QJsonObject{{"circles", QJsonArray{QJsonObject{{"kind", "stability_in"}}}}}}});
        QVERIFY(failed(res) && text(res).contains("Smith chart"));
        QVERIFY(!failed(call("delete", {{"diagrams", QJsonArray{2}}})));
        // Kept with the schematic.
        QVERIFY(!failed(call("edit_diagram", {{"diagram", 1}, {"smith_circles", QJsonObject{{"frequency", "4G"}, {"circles", QJsonArray{QJsonObject{{"kind", "stability_in"}}, QJsonObject{{"kind", "noise"}, {"level", 2.5}}}}}}})));
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        res = call("open_document", {{"path", path("amp.sch")}});
        QVERIFY2(!failed(res), qPrintable(text(res)));
        view = dynamic_cast<SmithDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->circleFrequency == 4e9 && view->circles.size() == 2 && view->circles.at(1).kind == SmithDiagram::Noise && view->circles.at(1).level == 2.5);
        QCOMPARE(view->circleSet().frequency, 4e9);
        // The dialog's words for them, and back.
        QList<SmithDiagram::CircleSpec> read;
        QString why;
        QVERIFY(SmithDiagram::circlesFromText("in, OUT, gain 12, noise 2.5", &read, &why) && read.size() == 4 && read.at(2).level == 12);
        QCOMPARE(SmithDiagram::circlesText(read), QString("in, out, gain 12, noise 2.5"));
        QVERIFY(!SmithDiagram::circlesFromText("in, gain", &read, &why) && why.contains("level"));
        QVERIFY(!SmithDiagram::circlesFromText("in, power 3", &read, &why) && why.contains("no circle"));
        auto* dialog = new DiagramDialog(view, nullptr);
        auto* edit = dialog->findChild<QLineEdit*>("smithCircles");
        auto* at = dialog->findChild<QLineEdit*>("smithFrequency");
        QVERIFY(edit && at && edit->text() == "in, noise 2.5" && at->text().startsWith("4"));
        edit->setText("out, gain 8");
        at->clear();
        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        QVERIFY(view->circles.size() == 2 && view->circles.at(0).kind == SmithDiagram::OutputStability && view->circles.at(1).level == 8);
        QVERIFY(std::isnan(view->circleFrequency));
        dialog->close();
        // None: null.
        QVERIFY(!failed(call("edit_diagram", {{"smith_circles", QJsonValue()}})));
        QVERIFY(view->circles.isEmpty() && view->circleSet().drawn.isEmpty());
        for (const Text* t : view->Texts) QVERIFY(!t->s.contains("Circles"));
        // A run of no two-port: why there are none, under the chart.
        QVERIFY(schematicWith("rc", datasetText("frequency", range(1e3, 1e6, 5), {{"ac.v(out)", [](double) { return 0.5; }}})));
        QVERIFY(!failed(call("add_diagram", {{"type", "admittance_smith"}, {"x", 150}, {"y", 400}, {"traces", QJsonArray{"ac.v(out)"}},
                                              {"smith_circles", QJsonObject{{"circles", QJsonArray{QJsonObject{{"kind", "stability_in"}}}}}}})));
        view = dynamic_cast<SmithDiagram*>(front()->a_DocDiags.front());
        QVERIFY(view && view->circleSet().error.contains("no two-port"));
        caption = false;
        for (const Text* t : view->Texts) caption |= t->s.startsWith("No circles: the run has no two-port");
        QVERIFY(caption);
    }

    void sParameterRunWritesItsS()
    {
        // An S-parameter simulation's S, Y, Z (and noise) vectors written by
        // the simulator in use, whatever the settings stored name: a run's
        // own (--ngspice, a fresh settings folder of none) wrote the
        // equations' variables alone, the S-parameters lost.
        const int stored = _settings::Get().item<int>("DefaultSimulator");
        const int inUse = QucsSettings.DefaultSimulator;
        const auto restore = qScopeGuard([&] {
            _settings::Get().setItem<int>("DefaultSimulator", stored);
            QucsSettings.DefaultSimulator = inUse;
        });
        _settings::Get().setItem<int>("DefaultSimulator", spicecompat::simNotSpecified);
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        const QString sch = path("sp.sch");
        QFile::remove(sch);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/templates_ngspice/S-parameter_active_analysis.sch"), sch));
        QFile(sch).setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        QVERIFY(!failed(call("open_document", {{"path", sch}})));
        QVERIFY(!failed(call("edit_component", {{"name", "SP1"}, {"properties", QJsonObject{{"Noise", "yes"}}}})));
        const QString netlist = text(call("get_netlist", {}));
        bool written = false;
        for (const QString& line : netlist.split('\n'))
            written = written || (line.startsWith("write ") && line.contains("S_2_1") && line.contains("NFmin") && line.contains("SOpt") && line.contains("Rn"));
        QVERIFY2(written, qPrintable(netlist));
        closeAll();
    }

    // ==== The bug hunt of 8 October (docs/bug_hunts/2026-10-08-new-diagrams.md):
    // each finding's case, as it was found.

    // F1: a marker on a log x axis whose data start at 0 - a linear AC
    // sweep from 0 Hz on a Bode diagram, an FFT's spectrum on a log x -
    // crashed Qucs-S, and the diagram drew nothing. The point at 0 is left
    // out of the axis (said), the rest drawn; a trace not drawn refuses a
    // marker, with why.
    void huntMarkerOnALogAxisFromZero()
    {
        const auto h = [](double f) { return std::complex<double>(1.0) / std::complex<double>(1.0, f / 1e3); };
        QVERIFY(schematicWith("logzero", datasetText("frequency", range(0, 1e5, 101),
                                                     {{"ac.v(out)", [&](double f) { return h(f).real(); }, [&](double f) { return h(f).imag(); }},
                                                      {"ac.v(none)", [](double) { return 0.0; }}})));
        for (const char* type : {"bode", "rect", "stacked"}) {
            QJsonObject args{{"type", type}, {"traces", QJsonArray{"ac.v(out)"}}};
            if (QString(type) != "bode") args.insert("x_axis", QJsonObject{{"log", true}});
            QJsonObject r = call("add_diagram", args);
            QVERIFY2(!failed(r), qPrintable(text(r)));
            const QJsonObject t = json(r).value("traces").toArray().first().toObject();
            QVERIFY2(t.value("left off the log axis").toString().startsWith("1 point"), qPrintable(text(r)));
            QVERIFY(!t.contains("not drawn"));
            r = call("add_marker", {{"diagram", json(r).value("diagram")}, {"at", 1000}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QCOMPARE(json(r).value("at").toObject().value("frequency").toDouble(), 1000.0);
        }
        // All of a trace at 0 on a log y axis: not drawn - a marker refused.
        QJsonObject r = call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"ac.v(none)"}}, {"y_axis", QJsonObject{{"log", true}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("traces").toArray().first().toObject().contains("not drawn"), qPrintable(text(r)));
        r = call("add_marker", {{"diagram", json(r).value("diagram")}, {"at", 1000}});
        QVERIFY2(failed(r) && text(r).contains("is not drawn"), qPrintable(text(r)));
    }

    // F2: a spectrogram's segment far below the sampling took time growing
    // as 1/segment - a file keeping one could not be opened; and the answer
    // asked for a shorter one. B7: an AC sweep is no transient to cut.
    void huntASpectrogramsShortSegment()
    {
        constexpr double Pi = 3.14159265358979323846;
        QVERIFY(schematicWith("seg", datasetText("time", range(0, 10e-3, 20001), {{"tran.v(out)", [](double t) { return std::sin(2 * Pi * 1e3 * t); }}})
                                         + datasetText("frequency", range(1, 1e3, 11), {{"ac.v(out)", [](double) { return 1.0; }}})));
        QElapsedTimer timer;
        timer.start();
        QJsonObject r = call("add_diagram", {{"type", "spectrogram"}, {"traces", QJsonArray{"tran.v(out)"}}, {"spectrogram", QJsonObject{{"segment", 1e-12}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(timer.elapsed() < 5000);
        QString error = json(r).value("analyses").toArray().first().toObject().value("error").toString();
        QVERIFY2(error.contains("fewer than 8 of the run's samples") && error.contains("give a longer segment"), qPrintable(error));
        // Eight samples a segment: at most 1024 columns, spread over the
        // whole run (they stopped at 1024 of the first).
        r = call("edit_diagram", {{"spectrogram", QJsonObject{{"segment", 5e-6}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* gram = dynamic_cast<SpectrogramDiagram*>(front()->a_DocDiags.back());
        QVERIFY(gram);
        const ContourDiagram::Grid& g = gram->grids().first();
        QVERIFY2(g.ok(), qPrintable(g.error));
        QVERIFY(g.x.size() > 900 && g.x.size() <= 1024);
        QVERIFY2(g.x.last() > 9.9e-3, qPrintable(QString::number(g.x.last())));
        // Kept in the file, and read again at once.
        QVERIFY(!failed(call("edit_diagram", {{"spectrogram", QJsonObject{{"segment", 1e-12}}}})));
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        timer.restart();
        QVERIFY(!failed(call("open_document", {{"path", path("seg.sch")}})));
        QVERIFY(timer.elapsed() < 5000);
        // An AC sweep: refused, said.
        r = call("add_diagram", {{"type", "spectrogram"}, {"traces", QJsonArray{"ac.v(out)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        error = json(r).value("analyses").toArray().first().toObject().value("error").toString();
        QVERIFY2(error.contains("is over frequency") && error.contains("cuts a transient"), qPrintable(error));
        QVERIFY2(json(r).value("note").toString().contains("is over frequency: a spectrogram reads a transient"), qPrintable(text(r)));
    }

    // F3, B14, B15, N8, N14, N18, N19: the LTspice import.
    void huntLtspiceImport()
    {
        namespace lt = qucs_s::ltspice;
        const QString head = "Version 4\nSHEET 1 880 680\n";
        // F3: coordinates beyond the model plane's reach refused (they
        // overflowed an int: a Debug build aborted, Release wrapped).
        lt::Conversion c = lt::convert(head + "SYMBOL res 800 2147483647 R0\nSYMATTR InstName R1\nSYMATTR Value 1k\n");
        QVERIFY2(c.error.contains("line 3") && c.error.contains("within 8388608"), qPrintable(c.error));
        c = lt::convert(head + "WIRE -2147483648 0 0 0\nSYMBOL res 0 0 R0\nSYMATTR InstName R1\nSYMATTR Value 1k\n");
        QVERIFY2(c.error.contains("line 3"), qPrintable(c.error));
        QVERIFY(writeFile(path("lt2/far.asy"), "Version 4\nSymbolType CELL\nPIN 2147483647 -2147483648 NONE 8\nPINATTR SpiceOrder 1\n"
                                               "PIN 0 32 NONE 8\nPINATTR SpiceOrder 2\nSYMATTR Prefix X\nSYMATTR SpiceModel FAR\n"));
        c = lt::convert(head + "SYMBOL far 0 0 R0\nSYMATTR InstName U1\n", {path("lt2")});
        QVERIFY2(c.error.contains("far.asy: a pin beyond 8388608"), qPrintable(c.error));
        // N8: a symbol's SpiceOrder of 0, of text, twice: said.
        for (const char* order : {"0", "-1", "x"}) {
            QVERIFY(writeFile(path("lt2/odd.asy"), QStringLiteral("Version 4\nPIN 0 0 NONE 8\nPINATTR SpiceOrder %1\nPIN 0 32 NONE 8\nPINATTR SpiceOrder 2\n"
                                                                  "SYMATTR Prefix X\nSYMATTR SpiceModel ODD\n").arg(order)));
            c = lt::convert(head + "SYMBOL odd 0 0 R0\nSYMATTR InstName U1\n", {path("lt2")});
            QVERIFY2(c.notes.join('\n').contains(QStringLiteral("odd.asy: SpiceOrder %1 (the pin at 0, 0) is not 1, 2, 3").arg(order)), qPrintable(c.notes.join('\n')));
        }
        // B15: a capacitor's and an inductor's database ratings left out,
        // their parasitics parts of their own; a resistor's tolerance out,
        // its temperature kept. B14: .step and .four kept for the import,
        // .meas and .wave said; .backanno LTspice's own bookkeeping.
        const QString asc = head + "FLAG 0 0 g\nFLAG 0 64 0\nFLAG 100 16 h\nFLAG 100 96 0\nFLAG 216 16 h\nFLAG 216 96 0\nFLAG 0 80 0\n"
                                   "SYMBOL voltage 0 -16 R0\nSYMATTR InstName V1\nSYMATTR Value SINE(0 1 1k)\n"
                                   "SYMBOL cap -16 0 R0\nSYMATTR InstName C1\nSYMATTR Value 1u\nSYMATTR SpiceLine V=50 Irms=1 Rser=0.01\nSYMATTR SpiceLine2 Lser=1n\n"
                                   "SYMBOL ind 84 0 R0\nSYMATTR InstName L1\nSYMATTR Value 1m\n"
                                   "SYMATTR SpiceLine Ipk=1 Rser=0.05 Rpar=1000 Cpar=0 mfg=\"Coil craft\" pn=\"XAL4020\" type=\"ferrite\"\n"
                                   "SYMBOL res 200 0 R0\nSYMATTR InstName R1\nSYMATTR Value {Rv}\nSYMATTR SpiceLine tol=1 pwr=0.25 temp=50\n"
                                   "TEXT 0 200 Left 2 !.param Rv=1k\\n.step param Rv 1k 3k 1k\\n.meas tran vmax MAX V(g)\\n.four 1k V(h)\\n"
                                   ".wave out.wav 16 44.1K V(g)\\n.backanno\\n.tran 3m\n";
        c = lt::convert(asc);
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        QStringList lines = c.netlist.split('\n');
        for (const char* line : {"C1 g C1_rser 1u", "RC1_rser C1_rser C1_lser 0.01", "LC1_lser C1_lser 0 1n", "L1 h L1_rser 1m",
                                 "RL1_rser L1_rser 0 0.05", "RL1_rpar h 0 1000", "R1 h 0 {Rv} temp=50", ".step param Rv 1k 3k 1k", ".four 1k V(h)"})
            QVERIFY2(lines.contains(line), qPrintable(QString(line) + "\n" + c.netlist));
        QVERIFY2(!c.netlist.contains("Irms") && !c.netlist.contains("Ipk") && !c.netlist.contains("mfg") && !c.netlist.contains("CL1_cpar")
                     && !c.netlist.contains("tol") && !c.netlist.contains(".meas") && !c.netlist.contains(".wave") && !c.netlist.contains(".backanno"),
                 qPrintable(c.netlist));
        QString notes = c.notes.join('\n');
        for (const char* note : {"C1: V=50 Irms=1 left out", "C1: lser=1n rser=0.01 made parts of their own (LC1_lser, RC1_rser)",
                                 "L1: Ipk=1 mfg=\"Coil craft\" pn=\"XAL4020\" type=\"ferrite\" left out", "R1: tol=1 pwr=0.25 left out",
                                 ".meas tran vmax MAX V(g): LTspice's measurement - not taken", ".wave out.wav 16 44.1K V(g): LTspice's .wav file"})
            QVERIFY2(notes.contains(note), qPrintable(QString(note) + "\n" + notes));
        QCOMPARE(c.parts, 4);
        // Imported: the stepped runs a Parameter Sweep, .four a Fourier
        // analysis - and with ngspice, three runs of R1.
        QVERIFY(writeFile(path("lt2/st.asc"), asc));
        QJsonObject r = call("import_netlist", {{"file", path("lt2/st.asc")}, {"save_as", path("lt2/st.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QStringList converted = json(r).value("converted").toVariant().toStringList();
        QVERIFY2(converted.join('\n').contains(".step param Rv 1k 3k 1k: a Parameter Sweep of TR1 over Rv, 3 points (SW1)")
                     && converted.join('\n').contains(".four 1k V(h): a Fourier analysis of TR1 (FOUR1)"),
                 qPrintable(text(r)));
        Component* sweep = front()->getComponentByName("SW1");
        QVERIFY(sweep && sweep->getProperty("Param")->Value == "Rv" && sweep->getProperty("Points")->Value == "3");
        if (withNgspice()) {
            r = call("simulate", {{"timeout", 120}});
            QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
        }
        closeAll();
        // N14: UTF-16 big-endian, with its mark or without; the old Mac's
        // line ends; a line LTspice has no word for (a NUL first), said.
        const QString rc = head + "WIRE 0 0 100 0\nFLAG 0 0 a\nFLAG 0 64 0\nSYMBOL cap -16 0 R0\nSYMATTR InstName C1\nSYMATTR Value 1u\n";
        QByteArray be("\xFE\xFF", 2);
        for (const QChar ch : rc) {
            be += char(ch.unicode() >> 8);
            be += char(ch.unicode() & 0xff);
        }
        QCOMPARE(lt::textOf(be), rc);
        QCOMPARE(lt::textOf(be.mid(2)), rc);
        QCOMPARE(lt::textOf(QString(rc).replace('\n', '\r').toLatin1()), rc);
        c = lt::convert(rc + QStringLiteral("%1WIRE 0 64 100 64\n").arg(QChar(0)));
        QVERIFY2(c.notes.join('\n').contains("1 line(s) not read, none of an .asc's words: line 9"), qPrintable(c.notes.join('\n')));
        // N18: a model of LTspice's own library, defined nowhere: said.
        c = lt::convert(head + "FLAG 16 0 a\nFLAG 16 64 0\nSYMBOL diode 0 0 R0\nSYMATTR InstName D1\nSYMATTR Value 1N4148\n");
        QVERIFY2(c.notes.join('\n').contains("D1 (1N4148): models defined nowhere in it - LTspice takes them from its own library"),
                 qPrintable(c.notes.join('\n')));
        c = lt::convert(head + "FLAG 16 0 a\nFLAG 16 64 0\nSYMBOL diode 0 0 R0\nSYMATTR InstName D1\nSYMATTR Value DX\nTEXT 0 100 Left 2 !.model DX D\n");
        QVERIFY2(!c.notes.join('\n').contains("models defined"), qPrintable(c.notes.join('\n')));
        // N19: flags a net label cannot take, renamed to names it can - said.
        c = lt::convert(head + "FLAG 16 0 +5V\nFLAG 16 64 -12V\nFLAG 116 0 3V3\nFLAG 116 64 V+\nFLAG 216 0 a.b\nFLAG 216 64 a_b\n"
                               "SYMBOL cap 0 0 R0\nSYMATTR InstName C1\nSYMATTR Value 1u\nSYMBOL cap 100 0 R0\nSYMATTR InstName C2\nSYMATTR Value 1u\n"
                               "SYMBOL cap 200 0 R0\nSYMATTR InstName C3\nSYMATTR Value 1u\n");
        lines = c.netlist.split('\n');
        QVERIFY2(lines.contains("C1 P5V N12V 1u") && lines.contains("C2 V3V3 VP 1u") && lines.contains("C3 a_b_2 a_b 1u"), qPrintable(c.netlist));
        notes = c.notes.join('\n');
        QVERIFY2(notes.contains("the net +5V is P5V here") && notes.contains("the net a.b is a_b_2 here"), qPrintable(notes));
        // (And a SPICE netlist's: its signs as letters, said.)
        r = call("import_netlist", {{"text", "t\nV1 +5V 0 5\nR1 +5V v- 1k\nR2 v- 0 1k\n.op\n.end"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("converted").toVariant().toStringList().join('\n').contains("the node +5V is the net P5V"), qPrintable(text(r)));
    }

    // A1: a tornado chart's parts named with an underscore (R_load), and
    // ngspice 46's AC sensitivity (ac.v(r1_scale)); N3, N17: its 'at'
    // between two points of the sweep, and beyond it.
    void huntTornadoParts()
    {
        QCOMPARE(TornadoDiagram::sensitivityParts({"r1", "r1_m", "r1_scale", "r_load", "r_load_m", "r_load_scale", "v1", "v1_dc"}),
                 (QStringList{"r1_scale", "r_load_scale", "v1"}));
        QCOMPARE(TornadoDiagram::sensitivityParts({"ac.v(r1)", "ac.v(r1_scale)", "ac.v(c_in)", "ac.v(c_in_m)", "ac.v(c_in_scale)", "frequency"}),
                 (QStringList{"ac.v(r1_scale)", "ac.v(c_in_scale)"}));
        QVERIFY(schematicWith("tor", datasetText("r1", {1000, 2000, 4000}, {{"sw.v(out)", [](double r) { return r / 1000; }},
                                                                            {"sw.v(in)", [](double r) { return -r / 2000; }},
                                                                            {"sw.v(nil)", [](double) { return 0.0; }}})));
        QJsonObject r = call("add_diagram", {{"type", "tornado"}, {"traces", QJsonArray{"sw.v(out)", "sw.v(in)", "sw.v(nil)"}}, {"tornado", QJsonObject{{"at", 3000}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject t = json(r).value("tornado").toObject();
        QCOMPARE(t.value("shown").toArray().first().toObject().value("value").toDouble(), 3.0);   // between 2000 and 4000
        QCOMPARE(t.value("of 0, not shown").toInt(), 1);
        QVERIFY(!t.contains("at outside"));
        r = call("edit_diagram", {{"tornado", QJsonObject{{"at", 99}}}});
        t = json(r).value("tornado").toObject();
        QVERIFY2(t.value("at outside").toString().contains("99 is beyond the sweep (1000 to 4000)"), qPrintable(text(r)));
        QCOMPARE(t.value("shown").toArray().first().toObject().value("value").toDouble(), 1.0);
    }

    // A3: the spectrum view's fundamental read between the bins, and each
    // harmonic sought at its multiple: a line there a peak above the floor,
    // else the bin there. A square's 2nd was "at 1800 Hz, -45.6 dBc", and a
    // pure sine of 10.5 periods read at 952 Hz, its THD up to 5.5 %.
    void huntSpectrumHarmonics()
    {
        namespace sp = qucs_s::spectrum;
        constexpr double Pi = 3.14159265358979323846;
        QVector<double> t, square, sine;
        for (int i = 0; i <= 20000; ++i) {
            t << i * 0.5e-6;   // 10 ms
            square << (std::fmod(t.last(), 1e-3) < 0.5e-3 ? 1.0 : 0.0);
        }
        const sp::Analysis a = sp::analyse(sp::of(t, square, sp::Window::Hann));
        QVERIFY2(a.ok, qPrintable(a.error));
        QVERIFY(std::abs(a.fundamental - 1000) < 1);
        for (const sp::Line& l : a.harmonics) {
            QVERIFY2(std::abs(l.frequency - l.harmonic * a.fundamental) < 5, qPrintable(QStringLiteral("%1: %2").arg(l.harmonic).arg(l.frequency)));
            if (l.harmonic % 2 == 0) QVERIFY2(l.dBc < -40, qPrintable(QStringLiteral("%1: %2 dBc").arg(l.harmonic).arg(l.dBc)));
        }
        QVERIFY(std::abs(a.harmonics.at(2).dBc - 20 * std::log10(1.0 / 3)) < 0.5);   // the 3rd: a third of the 1st
        // A sine of 10.5 periods.
        QVector<double> x;
        for (int i = 0; i < 20000; ++i) {
            x << i * 10.5e-3 / 20000;
            sine << std::sin(2 * Pi * 1e3 * x.last());
        }
        for (sp::Window w : {sp::Window::Hann, sp::Window::Blackman, sp::Window::BlackmanHarris}) {
            const sp::Analysis s = sp::analyse(sp::of(x, sine, w));
            QVERIFY2(std::abs(s.fundamental - 1000) < 5, qPrintable(QString::number(s.fundamental)));
            QVERIFY2(s.thd < 1e-3, qPrintable(QString::number(s.thd)));
            QVERIFY(std::abs(s.harmonics.last().frequency - 9 * s.fundamental) < 1);
        }
        // As ngspice samples a square (each edge a nanosecond, the rest 10 us
        // apart), from 2 ms: the "2nd harmonic" was a ripple of the floor a
        // bin away (1875 Hz).
        QVector<double> ts, vs;
        for (int k = 0; k < 10; ++k)
            for (int half = 0; half < 2; ++half) {
                const double a = k * 1e-3 + half * 0.5e-3, level = half ? 0.0 : 1.0;
                ts << a << a + 1e-9;
                vs << (half ? 1.0 : 0.0) << level;
                for (int j = 1; j < 50; ++j) {
                    ts << a + j * 10e-6;
                    vs << level;
                }
            }
        ts << 10e-3;
        vs << 0.0;
        const sp::Analysis ng = sp::analyse(sp::of(ts, vs, sp::Window::Hann, 2e-3));
        QVERIFY2(ng.ok, qPrintable(ng.error));
        for (const sp::Line& l : ng.harmonics) {
            QVERIFY2(std::abs(l.frequency - l.harmonic * ng.fundamental) < 5, qPrintable(QStringLiteral("%1: %2").arg(l.harmonic).arg(l.frequency)));
            if (l.harmonic % 2 == 0) QVERIFY2(l.dBc < -40, qPrintable(QStringLiteral("%1: %2 dBc").arg(l.harmonic).arg(l.dBc)));
        }
        // N2: a spectrum from past the run's end says so.
        QVERIFY(schematicWith("spec", datasetText("time", t, {{"tran.v(sq)", [](double v) { return std::fmod(v, 1e-3) < 0.5e-3 ? 1.0 : 0.0; }}})));
        QJsonObject r = call("add_diagram", {{"type", "spectrum"}, {"traces", QJsonArray{"tran.v(sq)"}}, {"spectrum", QJsonObject{{"from", 0.02}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("analyses").toArray().first().toObject().value("error").toString().contains("past the run's end"), qPrintable(text(r)));
    }

    // A5: a two-terminal part's second pin gave its first's current - the
    // sign wrong; B12: a bipolar's emitter (@q1[ie], which ngspice writes
    // none of) failed every run. Each computed by a NutmegEq now.
    void huntProbedCurrents()
    {
        QJsonObject r = call("import_netlist", {{"text", "bjt\nV1 c 0 5\nVb b0 0 0.7\nRb b0 b 1k\nR1 c out 1k\nQ1 out b e QN\nRe e 0 100\n"
                                                         ".model QN NPN(BF=100)\n.tran 1u 100u\n.end"},
                                                {"save_as", path("bjt.sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Schematic* sch = front();
        const auto vectors = [&](const QString& part) {
            QStringList out;
            Component* c = sch->getComponentByName(part);
            for (int i = 0; c && i < c->Ports.size(); ++i) {
                QString error;
                out << qucs_s::probe::vectorOf(sch, qucs_s::probe::Target{qucs_s::probe::Kind::Current, nullptr, nullptr, c, i}, &error);
            }
            out.sort();
            return out;
        };
        QCOMPARE(vectors("R1"), (QStringList{"-@r1[i]", "@r1[i]"}));
        QCOMPARE(vectors("V1"), (QStringList{"-i(v1)", "i(v1)"}));
        QCOMPARE(vectors("Q1"), (QStringList{"-(@q1[ic] + @q1[ib])", "@q1[ib]", "@q1[ic]"}));
        // Probed: the pin's current named after it, a NutmegEq's equation.
        Component* r1 = sch->getComponentByName("R1");
        QString secondPin;
        for (int i = 0; i < r1->Ports.size(); ++i) {
            QString error;
            if (qucs_s::probe::vectorOf(sch, {qucs_s::probe::Kind::Current, nullptr, nullptr, r1, i}, &error).startsWith('-'))
                secondPin = QStringLiteral("R1.%1").arg(i + 1);
        }
        r = call("probe", {{"what", secondPin}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        const QString pin = secondPin.mid(3);
        QCOMPARE(o.value("variable").toString(), QStringLiteral("ngspice/tran.i(r1_pin%1)").arg(pin));
        QCOMPARE(o.value("computed").toString(), QStringLiteral("r1_pin%1 = -@r1[i], computed by a NutmegEq after TR1").arg(pin));
        QCOMPARE(o.value("saved by the next run").toString(), QString("@r1[i]"));
        // The emitter.
        Component* q1 = sch->getComponentByName("Q1");
        QString emitter;
        for (int i = 0; i < q1->Ports.size(); ++i) {
            QString error;
            if (qucs_s::probe::vectorOf(sch, {qucs_s::probe::Kind::Current, nullptr, nullptr, q1, i}, &error).startsWith('-'))
                emitter = QStringLiteral("Q1.%1").arg(i + 1);
        }
        r = call("probe", {{"what", emitter}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("saved by the next run").toString(), QString("@q1[ic], @q1[ib]"));
        const QString netlist = text(call("get_netlist", {}));
        QVERIFY2(netlist.contains(QStringLiteral("let r1_pin%1 = -@r1[i]").arg(pin)) && netlist.contains("= -(@q1[ic] + @q1[ib])"), qPrintable(netlist));
        if (withNgspice()) {
            r = call("simulate", {{"timeout", 120}});
            QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
            const QJsonArray v = json(call("get_dataset", {{"variables", QJsonArray{"tran.@r1[i]", QStringLiteral("tran.i(r1_pin%1)").arg(pin),
                                                                                       "tran.@q1[ic]", "tran.@q1[ib]", QStringLiteral("tran.i(q1_pin%1)").arg(emitter.mid(3))}},
                                                         {"at", QJsonArray{5e-5}}})).value("variables").toArray();
            QVERIFY2(v.size() == 5, qPrintable(QJsonDocument(v).toJson(QJsonDocument::Compact) + text(call("get_netlist", {}))));
            const auto at = [&](int k) { return v.at(k).toObject().value("at").toArray().first().toArray().at(1).toDouble(); };
            QVERIFY(at(0) != 0);
            QVERIFY2(std::abs(at(1) + at(0)) < 1e-12, qPrintable(QString::number(at(1))));
            QVERIFY2(std::abs(at(4) + at(2) + at(3)) < 1e-9, qPrintable(QString::number(at(4))));
            // The probes' traces draw (the emitter's ngspice wrote as the name
            // alone, of vectors of no type).
            for (const Graph* g : sch->a_DocDiags.front()->Graphs) QVERIFY2(!g->isEmpty(), qPrintable(g->Var));
        }
        // B13: under Qucsator a net is probed (the netlist "could not be
        // written").
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        const auto restore = qScopeGuard([] { QucsSettings.DefaultSimulator = spicecompat::simNgspice; });
        r = call("probe", {{"what", "out"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("variable").toString(), QString("out.Vt"));
        // N1: a list of names, each probed.
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        r = call("probe", {{"what", QJsonArray{"out", "R1"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray each = json(r).value("probed").toArray();
        QCOMPARE(each.size(), 2);
        QCOMPARE(each.at(1).toObject().value("variable").toString(), QString("ngspice/tran.@r1[p]"));
    }

    // A6: a delta marker placed on an earlier trace measured from the wrong
    // marker; B2: one whose reference went kept its Δ lines; B9: a marker's
    // x kept to six digits moved on save; N13: 'relative_to' with no other
    // marker said "1 to 1"; N23: in the complex plane, a Δ of magnitudes.
    void huntMarkers()
    {
        QVector<double> t;
        for (int i = 0; i <= 1000; ++i) {
            t << i * 1e-5;
            if (i == 300)
                for (int k = 1; k <= 20; ++k) t << 3e-3 + k * 1e-9;   // (an edge's samples, a nanosecond apart)
        }
        QVERIFY(schematicWith("mk", datasetText("time", t, {{"tran.a", [](double x) { return x; }}, {"tran.b", [](double x) { return 2 * x; }}})));
        QJsonObject r = call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"tran.a", "tran.b"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("add_marker", {{"trace", 1}, {"at", 0.001}, {"relative_to", 1}});
        QVERIFY2(failed(r) && text(r).contains("it has none yet"), qPrintable(text(r)));
        QVERIFY(!failed(call("add_marker", {{"trace", 2}, {"at", 0.002}})));
        QVERIFY(!failed(call("add_marker", {{"trace", 2}, {"at", 0.006}})));
        r = call("add_marker", {{"trace", 1}, {"at", 0.004}, {"relative_to", 2}});   // the one at 6 ms, as numbered before
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(std::abs(json(r).value("delta").toObject().value("x").toDouble() + 2e-3) < 1e-9, qPrintable(text(r)));
        // B2: the reference deleted - no Δ left in the text.
        Diagram* d = front()->a_DocDiags.front();
        Marker* delta = d->markers().first();
        QVERIFY(delta->Text.contains(QString::fromUtf8("Δx")));
        QVERIFY(!failed(call("delete_marker", {{"marker", 3}})));
        QVERIFY2(!delta->Text.contains(QString::fromUtf8("Δ")), qPrintable(delta->Text));
        QVERIFY(delta->reference() == nullptr);
        // B9: every digit of its x kept - on a sample a nanosecond from
        // others, it stays on it reopened (at six digits it moved).
        r = call("add_marker", {{"trace", 1}, {"at", 3.000006e-3}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const double x = json(r).value("at").toObject().value("time").toDouble();
        QVERIFY(std::abs(x - 3.000006e-3) < 1e-13);
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("mk.sch")}})));
        bool kept = false;
        for (const Marker* m : front()->a_DocDiags.front()->markers()) kept = kept || std::abs(m->varPos().front() - 3.000006e-3) < 1e-13;
        QVERIFY(kept);
        // N23: Smith - Δ|y| and |Δy|, the distance.
        using C = std::complex<double>;
        QVERIFY(schematicWith("mks", datasetText("frequency", {1e6, 2e6}, {{"ac.s_1_1", [](double f) { return f < 1.5e6 ? 0.3 : 0.0; },
                                                                                     [](double f) { return f < 1.5e6 ? 0.0 : 0.4; }}})));
        QVERIFY(!failed(call("add_diagram", {{"type", "smith"}, {"traces", QJsonArray{"ac.s_1_1"}}})));
        QVERIFY(!failed(call("add_marker", {{"at", 1e6}})));
        r = call("add_marker", {{"at", 2e6}, {"relative_to", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject dl = json(r).value("delta").toObject();
        QVERIFY(std::abs(dl.value("magnitude").toDouble() - 0.1) < 1e-9 && std::abs(dl.value("distance").toDouble() - std::abs(C(0, 0.4) - C(0.3, 0))) < 1e-9);
        QVERIFY(!dl.contains("y"));
        QVERIFY2(json(r).value("text").toString().contains(QString::fromUtf8("\n|Δy|: ")) && json(r).value("text").toString().contains(QString::fromUtf8("\nΔ|y|: "))
                     && !json(r).value("text").toString().contains(QString::fromUtf8("\nΔy: ")),
                 qPrintable(json(r).value("text").toString()));
    }

    // A2 (unchecked limits), N6 (a limit at one x), N7 (a limit that does
    // not read leaves the schematic be), N16 (one warning a limit).
    void huntLimits()
    {
        QVERIFY(schematicWith("lim", datasetText("time", range(0, 10e-3, 1001), {{"tran.v(sq)", [](double v) { return std::fmod(v, 1e-3) < 0.5e-3 ? 1.0 : 0.0; }}})));
        QJsonObject r = call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"tran.v(sq)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", QJsonArray{QJsonArray{0, 1}, QJsonArray{0, 1}, QJsonArray{0, 1}}}}}}});
        QVERIFY2(failed(r) && text(r).contains("all at x = 0"), qPrintable(text(r)));
        r = call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", 0.9}}}}});
        QVERIFY(!json(r).value("verdict").toObject().value("pass").toBool());
        int said = 0;
        for (const QJsonValue& w : json(call("check_schematic", {})).value("warnings").toArray())
            if (w.toObject().value("message").toString().startsWith("diagram 1: tran.v(sq) is beyond")) {
                ++said;
                QVERIFY2(w.toObject().value("message").toString().contains("(the worst of 11 stretches beyond it)"), qPrintable(w.toObject().value("message").toString()));
            }
        QCOMPARE(said, 1);
        // On the right axis, which no trace is drawn against: not a pass.
        r = call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", 0.9}, {"axis", "right"}}}}});
        QJsonObject verdict = json(r).value("verdict").toObject();
        QVERIFY(!verdict.value("pass").toBool());
        QCOMPARE(verdict.value("unchecked").toArray(), QJsonArray{1});
        bool unchecked = false;
        for (const QJsonValue& w : json(call("check_schematic", {})).value("warnings").toArray())
            unchecked = unchecked || w.toObject().value("message").toString().contains("its upper limit 1 checks nothing");
        QVERIFY(unchecked);
        // In a file: a negative pane, points that do not read - the limit
        // left out or its pane the first, the schematic read.
        QVERIFY(!failed(call("edit_diagram", {{"limits", QJsonArray{QJsonObject{{"upper", 0.9}}}}})));
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QFile f(path("lim.sch"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString saved = QString::fromUtf8(f.readAll());
        f.close();
        QVERIFY(saved.contains("<Limit 0 0 0 "));
        QVERIFY(writeFile(path("lim.sch"), QString(saved).replace("<Limit 0 0 0 ", "<Limit 0 0 -3 ")));
        QVERIFY(!failed(call("open_document", {{"path", path("lim.sch")}})));
        QCOMPARE(front()->a_DocDiags.front()->limits.size(), 1);
        closeAll();
        QVERIFY(writeFile(path("lim.sch"), QString(saved).replace(QRegularExpression("<Limit 0 0 0 \"[^\"]*\" "), "<Limit 0 0 0 \"\" abc;")));
        QVERIFY(!failed(call("open_document", {{"path", path("lim.sch")}})));
        QCOMPARE(int(front()->a_DocDiags.size()), 1);
        QVERIFY(front()->a_DocDiags.front()->limits.isEmpty());
    }

    // B1: a vector its simulation's name put before it (two simulations of
    // a kind ran: tr1.tran.v(out)) found by the name alone, when one is so.
    void huntPrefixedNames()
    {
        QVERIFY(schematicWith("pre", datasetText("time", range(0, 1e-3, 11), {{"tr1.tran.v(out)", [](double) { return 1.0; }}})
                                         + datasetText("frequency", range(1, 10, 10), {{"fft1.ac.s", [](double) { return 2.0; }},
                                                                                     {"fft1.ac.v(two)", [](double) { return 3.0; }},
                                                                                     {"ac1.ac.v(two)", [](double) { return 4.0; }}})));
        QJsonObject r = call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"tran.v(out)", "ac.s"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        for (const QJsonValue& t : json(r).value("traces").toArray()) QVERIFY2(t.toObject().value("points").toInt() > 0, qPrintable(text(r)));
        // (Named as asked - shown on when one simulation runs alone again -
        // and found: no "has no variable".)
        QCOMPARE(json(r).value("traces").toArray().first().toObject().value("variable").toString(), QString("ngspice/tran.v(out)"));
        QVERIFY2(!json(r).value("note").toString().contains("has no variable"), qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"tran.v(out)"}}});
        QVERIFY2(!failed(r) && json(r).value("variables").toArray().first().toObject().value("name").toString() == "tr1.tran.v(out)", qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"v(two)"}}});   // each simulation's
        QVERIFY2(!failed(r) && json(r).value("variables").toArray().size() == 2, qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"ac.v(two)"}}});   // two: both, each by its name
        QVERIFY2(!failed(r) && json(r).value("variables").toArray().size() == 2, qPrintable(text(r)));
        // B6: a trace shown, its data the run names otherwise (tr1.tran.v(out)):
        // probed again, it has data; and one of a dataset named before it, as
        // the examples' traces are, is found in it.
        {
            const QString sch = path("pre6.sch");
            QVERIFY(writeFile(sch, QStringLiteral(
                "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n"
                "  <R R1 1 160 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"european\" 0>\n</Components>\n<Wires>\n"
                "  <100 100 130 100 \"in\" 100 70 0 \"\">\n  <190 100 220 100 \"out\" 200 70 0 \"\">\n</Wires>\n<Diagrams>\n"
                "  <Rect 380 311 329 231 3 #c0c0c0 1 00 1 0 0.0002 0.001 1 -1 1 1 1 -1 5 5 315 0 225 0 0 0 \"\" \"\" \"\">\n"
                "\t<\"ngspice/tran.v(out)\" #0000ff 0 3 0 0 0>\n  </Rect>\n"
                "  <Rect 380 611 329 231 3 #c0c0c0 1 00 1 0 0.0002 0.001 1 -1 1 1 1 -1 5 5 315 0 225 0 0 0 \"\" \"\" \"\">\n"
                "\t<\"ngspice/pre6:tran.v(out)\" #0000ff 0 3 0 0 0>\n  </Rect>\n</Diagrams>\n<Paintings>\n</Paintings>\n")));
            QVERIFY(writeFile(path("pre6.dat.ngspice"), datasetText("time", range(0, 1e-3, 11), {{"tr1.tran.v(out)", [](double) { return 1.0; }},
                                                                                                {"tran.v(in)", [](double) { return 2.0; }}})));
            QVERIFY(!failed(call("open_document", {{"path", sch}})));
            QVERIFY(!front()->a_DocDiags.front()->Graphs.first()->isEmpty());
            r = call("probe", {{"what", "out"}, {"diagram", 1}});
            QVERIFY2(json(r).value("already there").toBool() && json(r).value("has data").toBool(), qPrintable(text(r)));
            r = call("probe", {{"what", "in"}, {"diagram", 2}});
            QCOMPARE(json(r).value("variable").toString(), QString("ngspice/pre6:tran.v(in)"));
            QVERIFY2(json(r).value("has data").toBool(), qPrintable(text(r)));
        }
        // A run: an .AC beside the example's .FFT (both write "ac") prefixes
        // theirs alone - the transient's stay tran.v(out), and the example's
        // diagrams draw (they went blank).
        if (withNgspice()) {
            const QString sch = path("rcfft.sch");
            QFile::remove(sch);
            QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/General Electronics/RC_filter_FFT.sch"), sch));
            QFile(sch).setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            QVERIFY(!failed(call("open_document", {{"path", sch}})));
            QVERIFY(!failed(call("add_analysis", {{"kind", "ac"}})));
            r = call("simulate", {{"timeout", 120}});
            QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
            QStringList names;
            for (const QJsonValue& v : json(call("get_dataset", {})).value("variables").toArray()) names << v.toObject().value("name").toString();
            QVERIFY2(names.contains("tran.v(out)") && names.contains("ac1.ac.v(out)") && names.contains("fft1.ac.s"), qPrintable(names.join(", ")));
            for (const Diagram* d : front()->a_DocDiags)
                for (const Graph* g : d->Graphs) QVERIFY2(!g->isEmpty(), qPrintable(g->Var));
        }
    }

    // B4 (Smith circles beyond the sweep), B5 (add_diagram's run), B8
    // (rails a label can name), B16 (a Nichols chart's grid of many turns),
    // N4, N21 (a constellation's limits said), N9 (a box plot's 'at' on one
    // curve), N10 (iso-lines the data does not reach), N20 (a zero's half-
    // plane), N22 (Values at the Marker kept with the schematic).
    void huntTheRest()
    {
        // B4.
        QVERIFY(schematicWith("sp", datasetText("frequency", range(1e6, 2e7, 5), {{"ac.s_1_1", [](double) { return 0.3; }, [](double) { return 0.1; }},
                                                                                 {"ac.s_1_2", [](double) { return 0.05; }}, {"ac.s_2_1", [](double) { return 2.0; }},
                                                                                 {"ac.s_2_2", [](double) { return 0.5; }}})));
        QJsonObject r = call("add_diagram", {{"type", "smith"}, {"traces", QJsonArray{"ac.s_1_1"}},
                                             {"smith_circles", QJsonObject{{"frequency", 1e15}, {"circles", QJsonArray{QJsonObject{{"kind", "stability_in"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).value("smith_circles").toObject().value("error").toString().contains("outside the sweep (1e+06 to 2e+07 Hz)"), qPrintable(text(r)));
        // B5 and N4, N21, N9 on a transient.
        QVERIFY(schematicWith("rest", datasetText("time", range(0, 10e-3, 1001), {{"tran.i", [](double t) { return std::cos(3000 * t); }},
                                                                                  {"tran.q", [](double t) { return std::sin(3000 * t); }}})));
        QVERIFY(writeFile(path("before.dat.ngspice"), datasetText("time", range(0, 10e-3, 11), {{"tran.i", [](double) { return 0.5; }}})));
        r = call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"tran.i", QJsonObject{{"variable", "tran.i"}, {"run", "before"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray traces = json(r).value("traces").toArray();
        QCOMPARE(traces.at(1).toObject().value("variable").toString(), QString("ngspice/before:tran.i"));
        QVERIFY(traces.at(1).toObject().value("ghost").toBool() && traces.at(1).toObject().value("points").toInt() == 11);
        r = call("add_diagram", {{"type", "constellation"}, {"traces", QJsonArray{"tran.i", "tran.q"}}, {"constellation", QJsonObject{{"symbol_period", 1e-15}}}});
        QVERIFY2(json(r).value("constellation").toObject().value("pairs").toArray().first().toObject().value("note").toString().startsWith("the first 20000 symbols only"),
                 qPrintable(text(r)));
        r = call("edit_diagram", {{"diagram", 2}, {"constellation", QJsonObject{{"symbol_period", 1}}}});
        QVERIFY2(json(r).value("constellation").toObject().value("pairs").toArray().first().toObject().value("error").toString().contains("longer than the run"),
                 qPrintable(text(r)));
        r = call("edit_diagram", {{"diagram", 2}, {"constellation", QJsonObject{{"symbol_period", 1e-3}, {"offset", 1}}}});
        QVERIFY2(json(r).value("constellation").toObject().value("pairs").toArray().first().toObject().value("error").toString().contains("the offset (1) is past"),
                 qPrintable(text(r)));
        QVERIFY(failed(call("edit_diagram", {{"diagram", 2}, {"constellation", QJsonObject{{"offset", -1e-3}}}})));
        r = call("add_diagram", {{"type", "box_plot"}, {"traces", QJsonArray{"tran.i"}}, {"box_plot", QJsonObject{{"at", 0.003}}}});
        QVERIFY2(json(r).value("box_plot").toObject().value("boxes").toArray().first().toObject().value("at").toString().startsWith("not used"), qPrintable(text(r)));
        // N22: Values at the Marker kept.
        r = call("add_marker", {{"diagram", 1}, {"trace", 1}, {"at", 0.004}, {"annotate", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(front()->cursorValuesShown());
        QVERIFY(!failed(call("save_document", {})));
        closeAll();
        QVERIFY(!failed(call("open_document", {{"path", path("rest.sch")}})));
        QVERIFY(front()->cursorValuesShown());
        QVERIFY(front()->cursorMarker() != nullptr && front()->cursorMarker() == front()->a_DocDiags.front()->markers().first());
        // N13: an NRZ signal read as PAM4 (levels 4) - three eyes drawn - said.
        if (withNgspice()) {
            const QString sch = path("nrz.sch");
            QFile::remove(sch);
            QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/NGspice features/PRBS_eye_diagram.sch"), sch));
            QFile(sch).setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            QVERIFY(!failed(call("open_document", {{"path", sch}})));
            r = call("simulate", {{"timeout", 120}});
        }
        // (An ngspice without a PRBS source - a stock one: left out.)
        if (withNgspice() && !json(r).value("succeeded").toBool() && text(r).contains("unknown parameter (prbs)", Qt::CaseInsensitive)) {
            qInfo("this ngspice has no PRBS source: N13 left out");
            closeAll();
        } else if (withNgspice()) {
            QVERIFY2(json(r).value("succeeded").toBool(), qPrintable(text(r)));
            r = call("add_diagram", {{"type", "bathtub"}, {"traces", QJsonArray{"tran.v(rx)"}}, {"bathtub", QJsonObject{{"levels", 4}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            const QString notes = json(r).value("analyses").toArray().first().toObject().value("notes").toVariant().toStringList().join('\n');
            QVERIFY2(notes.contains("is it NRZ") && notes.contains("levels 2 reads it so"), qPrintable(text(r)));
            r = call("edit_diagram", {{"diagram", int(front()->a_DocDiags.size())}, {"bathtub", QJsonObject{{"levels", 2}}}});
            QVERIFY2(!json(r).value("analyses").toArray().first().toObject().contains("notes"), qPrintable(text(r)));
            closeAll();
        }
        // B8: the rails a label can name.
        const auto kind = [](const QString& name) {
            const QString sch = QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n</Components>\n"
                                               "<Wires>\n  <300 100 400 100 \"%1\" 320 70 0 \"\">\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n").arg(name);
            return sch;
        };
        int k = 0;
        for (const auto& [name, expected] : std::initializer_list<std::pair<const char*, int>>{
                 {"P5V", 1}, {"N12V", 1}, {"V3V3", 1}, {"VDD_IO", 1}, {"VPP", 1}, {"VCC_3V3", 1}, {"VCC_EN", 0}, {"VIN", 0}, {"VREF", 0}, {"AGND", 2}, {"GNDA", 2}}) {
            const QString file = path(QStringLiteral("cw%1.sch").arg(++k));
            QVERIFY(writeFile(file, kind(name)));
            QVERIFY(!failed(call("open_document", {{"path", file}})));
            const QHash<const Wire*, qucs_s::erc::NetKind> kinds = qucs_s::erc::wireKinds(front());
            const qucs_s::erc::NetKind got = kinds.value(front()->a_DocWires.front(), qucs_s::erc::NetKind::Signal);
            QVERIFY2(int(got) == (expected == 1 ? int(qucs_s::erc::NetKind::Supply) : expected == 2 ? int(qucs_s::erc::NetKind::Ground) : int(qucs_s::erc::NetKind::Signal)),
                     name);
            closeAll();
        }
        // B16: a Nichols chart over many turns of the phase: no grid, said.
        QVERIFY(schematicWith("nic", datasetText("frequency", range(1, 1000, 1000), {{"ac.v(g)", [](double f) { return std::polar(0.5, -f * 0.1).real(); },
                                                                                    [](double f) { return std::polar(0.5, -f * 0.1).imag(); }}})));
        r = call("add_diagram", {{"type", "nichols"}, {"traces", QJsonArray{"ac.v(g)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* nichols = dynamic_cast<NicholsDiagram*>(front()->a_DocDiags.front());
        QVERIFY(nichols && nichols->turnsInView() > NicholsDiagram::MostTurns);
        QVERIFY2(json(r).value("nichols").toObject().value("grid not drawn").toString().contains("turns"), qPrintable(text(r)));
        QByteArray svg;
        {
            const QString file = path("nic.svg");
            QVERIFY(!failed(call("export_image", {{"diagram", 1}, {"save_as", file}})));
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            svg = f.readAll();
        }
        QVERIFY2(svg.size() < 2000000, qPrintable(QString::number(svg.size())));
        QVERIFY(!failed(call("edit_diagram", {{"nichols", QJsonObject{{"grid", false}}}})));
        {
            const QString file = path("nic-nogrid.svg");
            QVERIFY(!failed(call("export_image", {{"diagram", 1}, {"save_as", file}})));
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QVERIFY2(svg.size() < f.size() + f.size() / 10, qPrintable(QStringLiteral("%1 %2").arg(svg.size()).arg(f.size())));
        }
        // N10: iso-lines the data does not reach, not listed.
        const QVector<double> xs = range(1, 10, 10), ys = range(0, 1, 6);
        QVERIFY(schematicWith("iso", datasetText2("r", xs, "c", ys, {{"sw.v", [](double x, double y) { return x + 10 * y; }}})));
        r = call("add_diagram", {{"type", "contour"}, {"traces", QJsonArray{"sw.v"}}, {"contour", QJsonObject{{"range", QJsonObject{{"from", 0}, {"to", 1e-3}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject an = json(r).value("analyses").toArray().first().toObject();
        QVERIFY2(an.value("iso-lines").toArray().isEmpty() && an.value("iso-lines not drawn").toString().contains("beyond the data (1 to 20)"), qPrintable(text(r)));
        // N20: a zero's side is its phase's, not a pole's stability.
        QVERIFY(schematicWith("pz", datasetText("index", {1}, {{"pz.pole(1)", [](double) { return -1000.0; }}, {"pz.zero(1)", [](double) { return 0.0; }}})));
        r = call("add_diagram", {{"type", "pole_zero"}, {"traces", QJsonArray{"pz.pole(1)", "pz.zero(1)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray roots = json(r).value("roots").toArray();
        QVERIFY(roots.at(0).toObject().value("roots").toArray().first().toObject().value("stable").toBool());
        const QJsonObject zero = roots.at(1).toObject().value("roots").toArray().first().toObject();
        QVERIFY2(!zero.contains("stable") && !zero.value("minimum phase").toBool() && zero.value("on the imaginary axis").toBool(), qPrintable(text(r)));
    }

    // N5 (a bus named ""), N15 (one of no length), N11 (a dialog's value
    // read back as it was shown).
    void huntBusesAndDialogValues()
    {
        QVERIFY(schematicWith("bus", datasetText("time", range(0, 1, 11), {{"tran.v", [](double t) { return t; }}})));
        QJsonObject r = call("add_painting", {{"type", "bus"}, {"from", QJsonArray{0, 0}}, {"to", QJsonArray{100, 0}}, {"name", ""}});
        QVERIFY2(failed(r) && text(r).contains("D[7:0]"), qPrintable(text(r)));
        BusPainting bus;
        bus.x1 = bus.x2 = 10;
        bus.y1 = bus.y2 = 20;
        QVERIFY(!bus.MousePressing(front()));   // first click
        QVERIFY(!bus.MousePressing(front()));   // the second on it: drawing on
        bus.x2 = 60;
        QVERIFY(bus.MousePressing(front()));    // finished
        // (Exactly: QCOMPARE's doubles are fuzzy.)
        QVERIFY(qucs_s::units::read("10f").value == 1e-14);
        QVERIFY(qucs_s::units::read("100u").value == 1e-4);
        // Diagram Properties, OK as it was: 1e-14 and 1e-4 stay so.
        QVERIFY(!failed(call("add_diagram", {{"type", "bathtub"}, {"traces", QJsonArray{"tran.v"}},
                                             {"bathtub", QJsonObject{{"unit_interval", 5e-4}, {"ber", 1e-9}, {"floor", 1e-14}}}})));
        QVERIFY(!failed(call("add_diagram", {{"type", "constellation"}, {"traces", QJsonArray{"tran.v", "tran.v"}},
                                             {"constellation", QJsonObject{{"symbol_period", 5e-4}, {"offset", 1e-4}}}})));
        for (Diagram* d : front()->a_DocDiags) {
            auto* dialog = new DiagramDialog(d, nullptr);
            QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
            dialog->close();
        }
        auto* tub = dynamic_cast<BathtubDiagram*>(front()->a_DocDiags.front());
        auto* iq = dynamic_cast<ConstellationDiagram*>(front()->a_DocDiags.back());
        QVERIFY(tub && iq);
        QVERIFY2(tub->floor == 1e-14 && tub->ui == 5e-4 && tub->ber == 1e-9, qPrintable(QString::number(tub->floor, 'g', 17)));
        QVERIFY2(iq->offset == 1e-4 && iq->period == 5e-4, qPrintable(QString::number(iq->offset, 'g', 17)));
    }
};

QTEST_MAIN(TestDiagramTools)
#include "test_diagram_tools.moc"
