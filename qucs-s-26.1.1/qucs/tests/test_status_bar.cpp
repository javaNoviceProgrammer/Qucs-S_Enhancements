/*
 * The status bar: the hint of the tool in hand; the diagram readout, the
 * selection, the cursor, the grid and the zoom; the rule check after each
 * edit, the last simulation and the simulator with its version; whether
 * the document is saved; the theme. And what it is made of: units from
 * variable names, values with SI prefixes, ages, durations, versions.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "apptheme.h"
#include "autosave.h"
#include "ink.h"
#include "messagedock.h"
#include "mouseactions.h"
#include "schematic.h"
#include "statusbar.h"
#include "settings.h"
#include "simulationconsole.h"
#include "textdoc.h"
#include "components/component.h"
#include "components/ground.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/histogramdiagram.h"
#include "extsimkernels/simulationrun.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::status;

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

struct ThemeGuard {
    int theme = QucsSettings.Theme;
    ~ThemeGuard()
    {
        QucsSettings.Theme = theme;
        _settings::Get().setItem<int>("Theme", theme);
        qucs_s::apptheme::apply(qucs_s::apptheme::System);
    }
};

QToolButton* chip(QucsApp& app, const char* name)
{
    return app.statusBar()->findChild<QToolButton*>(QLatin1String(name));
}

QLabel* label(QucsApp& app, const char* name)
{
    return app.statusBar()->findChild<QLabel*>(QLatin1String(name));
}

// Shown in the status bar (the window is shown, off the screen).
bool shown(QWidget* w)
{
    return w != nullptr && w->isVisibleTo(w->window());
}

void write(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(bytes);
}

// A window as the user has it: shown (off the screen), wide.
void showWide(QucsApp& app, int width = 1600)
{
    app.setAttribute(Qt::WA_DontShowOnScreen);
    app.resize(width, 800);
    app.show();
    QCoreApplication::processEvents();
}

Schematic* current(QucsApp& app)
{
    return qobject_cast<Schematic*>(app.DocumentTab->currentWidget());
}

// The colour of the round mark in front of a chip's text.
QColor markColour(QToolButton* chip)
{
    const QImage image = chip->icon().pixmap(QSize(10, 10), 1.0).toImage();
    return image.pixelColor(image.width() / 2, image.height() / 2);
}
} // namespace

class TestStatusBar : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString broken;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.GridMode = 0;   // each document shows its own grid or not
        QucsSettings.Theme = qucs_s::apptheme::System;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        qucs_s::autosave::setDirectory(dir.filePath("autosave"));
        // No ground, R1 with a pin open: an error and warnings.
        broken = dir.filePath("broken.sch");
        write(broken,
              "<Qucs Schematic " PACKAGE_VERSION ">\n"
              "<Components>\n"
              "  <R R1 1 100 100 15 -26 0 1 \"50\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
              "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
    }

    // --- what the chips are made of -----------------------------------

    void unitsComeFromTheNames()
    {
        QCOMPARE(unitOf("time"), QString("s"));
        QCOMPARE(unitOf("frequency"), QString("Hz"));
        QCOMPARE(unitOf("acfrequency"), QString("Hz"));
        QCOMPARE(unitOf("out.v"), QString("V"));
        QCOMPARE(unitOf("out.Vt"), QString("V"));
        QCOMPARE(unitOf("v(out)"), QString("V"));
        QCOMPARE(unitOf("ac.v(out)"), QString("V"));
        QCOMPARE(unitOf("Pr1.I"), QString("A"));
        QCOMPARE(unitOf("i(vin)"), QString("A"));
        QCOMPARE(unitOf("dB(out.v)"), QString("dB"));
        QCOMPARE(unitOf("phase(out.v)"), QString(QChar(0x00B0)));
        // Nothing to tell by.
        QCOMPARE(unitOf("vout"), QString());
        QCOMPARE(unitOf("dev(x)"), QString());
        QCOMPARE(unitOf("gain"), QString());
        QCOMPARE(unitOf(""), QString());
    }

    void valuesTakeAPrefixAndTheUnit()
    {
        const QString minus(QChar(0x2212)), micro(QChar(0x00B5));
        QCOMPARE(withUnit(1.5e-3, "s"), QString("1.5 ms"));
        QCOMPARE(withUnit(47e3, "Hz"), QString("47 kHz"));
        QCOMPARE(withUnit(-2.5e-6, "A"), minus + "2.5 " + micro + "A");
        QCOMPARE(withUnit(0.0, "V"), QString("0 V"));
        QCOMPARE(withUnit(1.23456, "V"), QString("1.235 V"));   // four digits
        QCOMPARE(withUnit(999.96, "Hz"), QString("1 kHz"));      // not 1000 Hz
        QCOMPARE(withUnit(1.5e6, "Hz", 2), QString("1.50 MHz"));
        // Decibels and degrees keep no prefix.
        QCOMPARE(withUnit(-3.0103, "dB"), minus + "3.01 dB");
        QCOMPARE(withUnit(45.0, QString(QChar(0x00B0))), QString("45.00") + " " + QChar(0x00B0));
    }

    void theReadoutNamesTheAxes()
    {
        // A transient: out.Vt against time.
        const QString data = dir.filePath("tran.dat");
        write(data, "<Qucs Dataset " PACKAGE_VERSION ">\n"
                    "<indep time 3>\n0\n0.001\n0.002\n</indep>\n"
                    "<dep out.Vt time>\n0\n1\n2\n</dep>\n"
                    "<dep Pr1.It time>\n0\n0.001\n0.002\n</dep>\n");
        const QString line = "<Rect 0 300 600 300 3 #c0c0c0 1 00 0 0 0.001 0.002 1 -0.1 0.5 2.1 1 -0.1 0.5 0.1 "
                             "315 0 225 0 0 0 \"\" \"\" \"\">";
        QString body = "<\"out.Vt\" #0000ff 1 3 0 0 0>\n</Rect>\n";
        QTextStream stream(&body, QIODevice::ReadOnly);
        RectDiagram d;
        QVERIFY(d.load(line, &stream));
        d.loadGraphData(data);
        QCOMPARE(d.Graphs.size(), 1);
        const QString dot = QStringLiteral("  ·  ");
        QCOMPARE(readout(&d, MappedPoint{1.5e-3, 1.2, 0.0}), "time 1.5 ms" + dot + "out.Vt 1.2 V");

        // A second graph on the right axis: y1 by its variable, y2 too.
        QString body2 = "<\"out.Vt\" #0000ff 1 3 0 0 0>\n<\"Pr1.It\" #ff0000 1 3 0 0 1>\n</Rect>\n";
        QTextStream stream2(&body2, QIODevice::ReadOnly);
        RectDiagram two;
        QVERIFY(two.load(line, &stream2));
        two.loadGraphData(data);
        QCOMPARE(readout(&two, MappedPoint{1e-3, 1.0, 2e-3}),
                 "time 1 ms" + dot + "out.Vt 1 V" + dot + "Pr1.It 2 mA");

        // Two graphs on one axis: the axis is y; the unit they share stays.
        QString body3 = "<\"out.Vt\" #0000ff 1 3 0 0 0>\n<\"Pr1.It\" #ff0000 1 3 0 0 0>\n</Rect>\n";
        QTextStream stream3(&body3, QIODevice::ReadOnly);
        RectDiagram mixed;
        QVERIFY(mixed.load(line, &stream3));
        mixed.loadGraphData(data);
        QCOMPARE(readout(&mixed, MappedPoint{1e-3, 0.5, 0.0}), "time 1 ms" + dot + "y 0.5");

        // A notation of the diagram's own is kept: scientific.
        d.notation = qucs_s::numberformat::Notation::Scientific;
        QVERIFY(readout(&d, MappedPoint{1.5e-3, 1.2, 0.0}).startsWith("time 1.5e"));
        // A dataset in the name: left out.
        QString body4 = "<\"ngspice/tran.v(out)\" #0000ff 1 3 0 0 0>\n</Rect>\n";
        QTextStream stream4(&body4, QIODevice::ReadOnly);
        RectDiagram spice;
        QVERIFY(spice.load(line, &stream4));
        QCOMPARE(readout(&spice, MappedPoint{0.0, 0.25, 0.0}), "x 0" + dot + "tran.v(out) 250 mV");
        QCOMPARE(readout(nullptr, MappedPoint{0, 0, 0}), QString());

        // A histogram: the variable's values along x, the count up y.
        const QString histogramLine = "<Histogram 0 300 400 300 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 -1 0.5 1 "
                                      "315 0 225 0 0 0 0 -1 \"\" \"\" \"\">";
        QString body5 = "<\"ngspice/v(out)\" #0050c8 1 3 0 0 0>\n</Histogram>\n";
        QTextStream stream5(&body5, QIODevice::ReadOnly);
        HistogramDiagram histogram;
        QVERIFY(histogram.load(histogramLine, &stream5));
        QCOMPARE(readout(&histogram, MappedPoint{0.4, 12.0, 0.0}), "v(out) 400 mV" + dot + "count 12");
        histogram.height = HistogramDiagram::Percent;
        QCOMPARE(readout(&histogram, MappedPoint{0.4, 12.5, 0.0}), "v(out) 400 mV" + dot + "percent 12.5");
    }

    void agesReadAsSaid()
    {
        const QDateTime now(QDate(2026, 9, 24), QTime(15, 0));
        QCOMPARE(age(now.addSecs(-10), now), QString("just now"));
        QCOMPARE(age(now.addSecs(-70), now), QString("1 min ago"));
        QCOMPARE(age(now.addSecs(-5 * 60), now), QString("5 min ago"));
        const QDateTime earlier(QDate(2026, 9, 24), QTime(9, 5));
        QCOMPARE(age(earlier, now), "at " + QLocale().toString(earlier.time(), QLocale::ShortFormat));
        QCOMPARE(age(QDateTime(QDate(2026, 9, 3), QTime(12, 0)), now),
                 "on " + QLocale().toString(QDate(2026, 9, 3), "d MMM"));
        QCOMPARE(age(QDateTime(QDate(2025, 9, 3), QTime(12, 0)), now),
                 "on " + QLocale().toString(QDate(2025, 9, 3), "d MMM yyyy"));
    }

    void durationsReadAsSaid()
    {
        QCOMPARE(duration(420), QLocale().toString(0.42, 'f', 2) + " s");
        QCOMPARE(duration(12345), QLocale().toString(12.3, 'f', 1) + " s");
        QCOMPARE(duration(125000), QString("2 min 5 s"));
    }

    void versionsAreReadFromTheOutput()
    {
        QCOMPARE(versionIn(spicecompat::simNgspice,
                           "******\n** ngspice-46 : Circuit level simulation program\n** Compiled with KLU"),
                 QString("46"));
        QCOMPARE(versionIn(spicecompat::simNgspice, "** ngspice-45.2 : Circuit level"), QString("45.2"));
        QCOMPARE(versionIn(spicecompat::simXyce, "Xyce Release 7.8.0-opensource"), QString("7.8.0"));
        QCOMPARE(versionIn(spicecompat::simQucsator, "Qucsator 1.0.7\nCopyright (C) 2003-2009"), QString("1.0.7"));
        // Another program's output names no version of the simulator.
        QCOMPARE(versionIn(spicecompat::simNgspice, "GNU bash, version 3.2.57(1)-release"), QString());
        QCOMPARE(versionArguments(spicecompat::simNgspice), QStringList({"--version"}));
        QVERIFY(versionArguments(spicecompat::simSpiceOpus).isEmpty());
    }

    void warningsAreCountedByTheLine()
    {
        QCOMPARE(SimulationRun::countWarnings("Warning: one\nfine\nwarning: two\nWarning: three"), 3);
        QCOMPARE(SimulationRun::countWarnings("all fine\n"), 0);
    }

    // --- the chips in the window ---------------------------------------

    void theHintFollowsTheTool()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        StatusPanel* panel = app.statusPanel();
        QVERIFY(panel != nullptr);
        QVERIFY(current(app) != nullptr);
        app.select->setChecked(true);
        panel->refresh();
        QVERIFY2(panel->hint().startsWith("Double-click to edit"), qPrintable(panel->hint()));

        app.insWire->setChecked(true);
        QTRY_VERIFY2(panel->hint().startsWith("Click where the wire starts"), qPrintable(panel->hint()));
        // After the first click: the route, and how to change it.
        app.MousePressAction = &MouseActions::MPressWire2;
        panel->refresh();
        QVERIFY2(panel->hint().contains("wiring") && panel->hint().contains("right-click"), qPrintable(panel->hint()));

        app.editDelete->setChecked(true);
        QTRY_VERIFY2(panel->hint().startsWith("Click an element to delete it"), qPrintable(panel->hint()));
        app.insGround->setChecked(true);
        QTRY_VERIFY2(panel->hint().startsWith("Click to place"), qPrintable(panel->hint()));
        QVERIFY(panel->hint().contains("right-click to rotate"));

        // Back to selecting with Escape.
        QMetaObject::invokeMethod(&app, "slotEscape");
        QTRY_VERIFY2(panel->hint().startsWith("Double-click to edit"), qPrintable(panel->hint()));
        // The label on the left says it (elided to its room).
        auto* hint = app.statusBar()->findChild<QLabel*>("statusHint");
        QVERIFY(hint != nullptr);
        QCOMPARE(hint->toolTip(), panel->hint());
    }

    void positionAndReadoutFollowTheCursor()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        StatusPanel* panel = app.statusPanel();
        app.printCursorPosition(240, -130, QString());
        QLabel* position = label(app, "statusPosition");
        QLabel* readoutLabel = label(app, "statusReadout");
        QVERIFY(position != nullptr && readoutLabel != nullptr);
        QCOMPARE(position->text(), "x 240   y " + QString(QChar(0x2212)) + "130");
        QTRY_VERIFY(shown(position));
        QVERIFY(!shown(readoutLabel));
        app.printCursorPosition(10, 20, "time 1 ms");
        QCOMPARE(readoutLabel->text(), QString("time 1 ms"));
        QTRY_VERIFY(shown(readoutLabel));
        app.printCursorPosition(10, 20, QString());
        QTRY_VERIFY(!shown(readoutLabel));

        // A text document: line and column.
        const QString file = dir.filePath("notes.va");
        write(file, "module m(a);\nendmodule\n");
        QVERIFY(app.gotoPage(file, false, false));
        QVERIFY(qobject_cast<TextDoc*>(app.DocumentTab->currentWidget()) != nullptr);
        app.printCursorPosition(2, 5, QString());
        QCOMPARE(position->text(), QString("Ln 2, Col 5"));
        panel->refresh();
        // No zoom, grid or selection for text.
        QTRY_VERIFY(!shown(chip(app, "statusZoom")));
        QVERIFY(!shown(chip(app, "statusGrid")));
    }

    void theSelectionIsSummedUpAndZoomedTo()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        QVERIFY(app.gotoPage(broken, false, false));
        Schematic* doc = current(app);
        QVERIFY(doc != nullptr);
        StatusPanel* panel = app.statusPanel();
        QToolButton* selection = chip(app, "statusSelection");
        QVERIFY(selection != nullptr);
        panel->refresh();
        QVERIFY(!shown(selection));   // nothing selected

        Component* r1 = doc->a_Components->front();
        r1->isSelected = true;
        panel->refresh();
        QTRY_VERIFY(shown(selection));
        QCOMPARE(selection->text(), QString("R1: R = 50"));
        QVERIFY(selection->toolTip().contains("1 component"));

        QMetaObject::invokeMethod(&app, "slotSelectAll");
        panel->refresh();
        QVERIFY(selection->text().endsWith("selected") || selection->text().startsWith("R1"));
        doc->a_Components->front()->isSelected = false;
        panel->refresh();
        QTRY_VERIFY(!shown(selection));
    }

    void theGridAndTheZoomShowAndChange()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        Schematic* doc = current(app);
        QVERIFY(doc != nullptr);
        StatusPanel* panel = app.statusPanel();
        panel->refresh();
        QToolButton* grid = chip(app, "statusGrid");
        QToolButton* zoom = chip(app, "statusZoom");
        QVERIFY(grid != nullptr && zoom != nullptr);
        QTRY_VERIFY(shown(grid) && shown(zoom));
        QCOMPARE(grid->text(), QString("Grid %1").arg(doc->getGridX()));
        const bool before = doc->gridShown();
        QCOMPARE(grid->property("shown").toBool(), before);
        grid->click();
        QCOMPARE(doc->gridShown(), !before);
        panel->refresh();
        QCOMPARE(grid->property("shown").toBool(), !before);

        QCOMPARE(zoom->text(), QString("%1%").arg(qRound(doc->getScale() * 100)));
        // The menu: a scale picked.
        zoom->click();
        auto* menu = zoom->findChild<QMenu*>("statusZoomMenu");
        QVERIFY(menu != nullptr);
        QAction* twice = nullptr;
        for (QAction* a : menu->actions())
            if (a->text() == "200%") twice = a;
        QVERIFY(twice != nullptr);
        twice->trigger();
        menu->close();
        QTRY_COMPARE(zoom->text(), QString("200%"));
        QCOMPARE(qRound(doc->getScale() * 100), 200);
    }

    void theProblemsAreCheckedAfterEachEdit()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        QToolButton* problems = chip(app, "statusProblems");
        QVERIFY(problems != nullptr);
        // An empty schematic: nothing wrong.
        QTRY_VERIFY(shown(problems));
        QCOMPARE(problems->text(), QString("No problems"));
        QCOMPARE(problems->property("tone").toString(), QString("ok"));

        QVERIFY(app.gotoPage(broken, false, false));
        QTRY_VERIFY2(problems->text().contains("error"), qPrintable(problems->text()));
        QCOMPARE(problems->property("tone").toString(), QString("error"));
        QVERIFY(problems->toolTip().contains("no ground"));

        // An edit: the check again, a moment later.
        Schematic* doc = current(app);
        auto* ground = new Ground();
        ground->setSchematic(doc);
        ground->cx = 100;
        ground->cy = 200;
        doc->insertComponent(ground);
        doc->setChanged(true, true);
        QTRY_VERIFY2(!problems->text().contains("error"), qPrintable(problems->text()));
        QCOMPARE(problems->property("tone").toString(), QString("warn"));   // the open pins

        // A click lists them, and goes to the first.
        problems->click();
        QVERIFY(!app.messages()->issues().isEmpty());
    }

    void theRunChipFollowsTheSimulation()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        StatusPanel* panel = app.statusPanel();
        QToolButton* run = chip(app, "statusRun");
        QVERIFY(run != nullptr);
        panel->refresh();
        QVERIFY(!shown(run));   // nothing simulated yet

        panel->runStarted(broken);
        QTRY_VERIFY(shown(run));
        QVERIFY2(run->text().startsWith("Simulating"), qPrintable(run->text()));
        QCOMPARE(run->property("tone").toString(), QString("busy"));
        panel->runEnded(StatusPanel::Outcome::Warned, 3);
        QVERIFY2(run->text().startsWith("3 warnings"), qPrintable(run->text()));
        QCOMPARE(run->property("tone").toString(), QString("warn"));
        panel->runStarted(broken);
        panel->runEnded(StatusPanel::Outcome::Failed, 0);
        QCOMPARE(run->text(), QString("Simulation failed"));
        QCOMPARE(run->property("tone").toString(), QString("error"));

        // An external simulator's run, to its end.
        QVERIFY(app.gotoPage(broken, false, false));
        SimulationRun simulation(current(app), false);
        panel->watchRun(&simulation, broken);
        QVERIFY(run->text().startsWith("Simulating"));
        emit simulation.simulated(&simulation);
        QVERIFY2(run->text().startsWith("Simulated in"), qPrintable(run->text()));
        QCOMPARE(run->property("tone").toString(), QString("ok"));
        QVERIFY(run->toolTip().contains("broken.sch"));
        // A click shows its output.
        run->click();
        QTRY_VERIFY(app.simulationConsole()->isVisibleTo(&app));

        // A project opened or closed: forgotten.
        panel->clearRun();
        QTRY_VERIFY(!shown(run));
    }

    void theSimulatorIsNamedAndSwitched()
    {
#ifdef Q_OS_WIN
        QSKIP("a shell script stands for the simulator");
#endif
        // A simulator that answers --version as ngspice does.
        const QString fake = dir.filePath("fake-ngspice");
        write(fake, "#!/bin/sh\necho '******'\necho '** ngspice-99.1 : Circuit level simulation program'\n");
        QVERIFY(QFile::setPermissions(fake, QFile::permissions(fake) | QFileDevice::ExeOwner | QFileDevice::ExeUser));
        struct Restore {
            QString ngspice = QucsSettings.NgspiceExecutable, qucsator = QucsSettings.Qucsator;
            int simulator = QucsSettings.DefaultSimulator;
            ~Restore()
            {
                QucsSettings.NgspiceExecutable = ngspice;
                QucsSettings.Qucsator = qucsator;
                QucsSettings.DefaultSimulator = simulator;
            }
        } restore;
        QucsSettings.NgspiceExecutable = fake;
        QucsSettings.Qucsator = QStandardPaths::findExecutable("sh");   // there, as a second simulator
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;

        QucsApp app(false);
        MainGuard guard(&app);
        StatusPanel* panel = app.statusPanel();
        QToolButton* simulator = chip(app, "statusSimulator");
        QVERIFY(simulator != nullptr);
        panel->refresh();
        QCOMPARE(simulator->text(), QString("Ngspice"));   // not asked before the window shows
        showWide(app);
        QTRY_COMPARE(panel->simulatorVersion(), QString("99.1"));
        QTRY_COMPARE(simulator->text(), QString("Ngspice 99.1"));
        QVERIFY(simulator->toolTip().contains(QDir::toNativeSeparators(fake)));

        // The menu switches, and the tool bar's list goes along.
        simulator->click();
        auto* menu = simulator->findChild<QMenu*>("statusSimulatorMenu");
        QVERIFY(menu != nullptr);
        QAction* qucsator = nullptr;
        for (QAction* a : menu->actions())
            if (a->text() == "Qucsator") qucsator = a;
        QVERIFY(qucsator != nullptr);
        qucsator->trigger();
        menu->close();
        QCOMPARE(QucsSettings.DefaultSimulator, int(spicecompat::simQucsator));
        QTRY_VERIFY2(simulator->text().startsWith("Qucsator"), qPrintable(simulator->text()));
        bool listed = false;
        for (QComboBox* c : app.findChildren<QComboBox*>())
            listed |= c->currentText() == "Qucsator";
        QVERIFY(listed);
    }

    void theDocumentSaysWhetherItIsSaved()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        StatusPanel* panel = app.statusPanel();
        QToolButton* saved = chip(app, "statusSaved");
        QVERIFY(saved != nullptr);
        Schematic* doc = current(app);
        QVERIFY(doc != nullptr);
        panel->refresh();
        QVERIFY(!shown(saved));   // untitled, unchanged: nothing to say
        doc->setChanged(true, true);
        QTRY_VERIFY(shown(saved));
        QCOMPARE(saved->text(), QString("Not saved"));
        QCOMPARE(saved->property("tone").toString(), QString("warn"));

        const QString file = dir.filePath("saved.sch");
        doc->setDocName(file);
        QVERIFY(doc->save() >= 0);
        QTRY_COMPARE(saved->text(), QString("Saved just now"));
        QCOMPARE(saved->property("tone").toString(), QString());
        QVERIFY(saved->toolTip().contains(QDir::toNativeSeparators(file)));

        doc->setChanged(true, true);
        QTRY_COMPARE(saved->text(), QString("Unsaved changes"));
        QVERIFY(app.autosaveAll() > 0);
        QTRY_VERIFY2(saved->text().startsWith("Unsaved") && saved->text().contains("autosaved just now"),
                     qPrintable(saved->text()));
        // A click saves it.
        saved->click();
        QTRY_COMPARE(saved->text(), QString("Saved just now"));
        QVERIFY(!doc->getDocChanged());
    }

    void theThemeChipNamesTheThemeAndOpensTheMenu()
    {
        ThemeGuard themeGuard;
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        QToolButton* theme = chip(app, "statusTheme");
        QVERIFY(theme != nullptr);
        QTRY_VERIFY(shown(theme));
        app.applyTheme(qucs_s::apptheme::Nord);
        QVERIFY(theme->toolTip().contains("Nord"));
        QVERIFY(!theme->icon().isNull());
        theme->click();
        // View > Theme, up (on macOS the menu bar's menus are not the
        // window's children).
        QTRY_VERIFY(qobject_cast<QMenu*>(QApplication::activePopupWidget()) != nullptr);
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        bool listsNord = false;
        for (QAction* a : menu->actions()) listsNord |= a->text() == "Nord";
        QVERIFY(listsNord);
        menu->close();
    }

    void everyMarkShowsOnEveryTheme()
    {
        // The marks - green, amber, red - against each theme's status bar.
        ThemeGuard themeGuard;
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app);
        StatusPanel* panel = app.statusPanel();
        QToolButton* run = chip(app, "statusRun");
        for (const auto& d : qucs_s::apptheme::designed()) {
            app.applyTheme(d.id);
            for (auto outcome : {StatusPanel::Outcome::Succeeded, StatusPanel::Outcome::Warned,
                                 StatusPanel::Outcome::Failed}) {
                panel->runStarted(broken);
                panel->runEnded(outcome, 1);
                const QColor mark = markColour(run);
                QVERIFY2(qucs_s::ink::contrast(mark, d.colours.surface) >= 3.0,
                         qPrintable(QString("%1: %2 on %3").arg(d.name, mark.name(), d.colours.surface.name())));
            }
            // The chip's text reads on the bar.
            QVERIFY2(qucs_s::ink::contrast(run->palette().color(QPalette::ButtonText), d.colours.surface) >= 4.5,
                     d.name);
        }
    }

    void chipsGiveWayOnANarrowWindow()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        showWide(app, 1600);
        StatusPanel* panel = app.statusPanel();
        panel->runStarted(broken);
        panel->runEnded(StatusPanel::Outcome::Succeeded, 0);
        app.printCursorPosition(10, 10, QString());
        QTRY_VERIFY(shown(chip(app, "statusTheme")));
        QVERIFY(shown(chip(app, "statusRun")));
        QVERIFY(shown(chip(app, "statusSimulator")));

        // Less room than the chips want: the least important go first,
        // the run stays.
        QWidget* row = app.statusBar()->findChild<QWidget*>("statusChips");
        QVERIFY(row != nullptr);
        row->setMaximumWidth(chip(app, "statusRun")->sizeHint().width());
        QTRY_VERIFY(!shown(chip(app, "statusTheme")));
        QVERIFY(!shown(chip(app, "statusSimulator")));
        QVERIFY(shown(chip(app, "statusRun")));
        // The status bar does not hold the window wide.
        QVERIFY2(app.statusBar()->minimumSizeHint().width() < 300,
                 qPrintable(QString::number(app.statusBar()->minimumSizeHint().width())));
        row->setMaximumWidth(QWIDGETSIZE_MAX);
        QTRY_VERIFY(shown(chip(app, "statusTheme")));
    }

    // QUCS_TEST_GRAB=<dir>: the status bar in some themes, for a look.
    void gallery()
    {
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (grabDir.isEmpty()) QSKIP("QUCS_TEST_GRAB not set");
        ThemeGuard themeGuard;
        using namespace qucs_s::apptheme;
        const QString example = QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/NGspice features/RC_lowpass_montecarlo.sch";
        QVERIFY(QFile::exists(example));
        for (int theme : {int(System), int(Daylight), int(Paper), int(Nord), int(Dracula), int(CatppuccinMocha)}) {
            QucsApp app(false);
            MainGuard guard(&app);
            app.applyTheme(theme);
            showWide(app, 1440);
            QVERIFY(app.gotoPage(example, false, false));
            StatusPanel* panel = app.statusPanel();
            current(app)->a_Components->front()->isSelected = true;
            app.printCursorPosition(-240, 130, "time 1.5 ms  \u00B7  out.Vt 1.235 V");
            panel->runStarted(example);
            panel->runEnded(StatusPanel::Outcome::Warned, 2);
            panel->refresh();
            QTest::qWait(600);   // the rule check
            const QString file = name(theme).toLower().replace(' ', '-');
            app.statusBar()->grab().save(grabDir + "/status-" + file + ".png");
            // Placing a component, the document changed.
            app.insGround->setChecked(true);
            current(app)->setChanged(true, true);
            QTest::qWait(600);
            app.statusBar()->grab().save(grabDir + "/status-" + file + "-placing.png");
        }
    }
};

QTEST_MAIN(TestStatusBar)
#include "test_status_bar.moc"
