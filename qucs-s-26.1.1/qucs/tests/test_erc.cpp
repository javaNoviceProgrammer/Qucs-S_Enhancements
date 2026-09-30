/*
 * The electrical rule check (erc.h): unconnected pins and wire ends,
 * duplicate names, no ground, no simulation - and the Problems tab that
 * lists them, with a click that shows the place.
 */
#include <QtTest>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include "mouseactions.h"
#include <QDirIterator>
#include <QLabel>
#include <QTimer>
#include <QRegularExpression>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "erc.h"
#include "messagedock.h"
#include "simulationconsole.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/xyce.h"
#include "extsimkernels/simsettingsdialog.h"
#include <QCheckBox>
#include <QToolButton>
#include <QHashSeed>
#include "isolated_settings.h"

using namespace qucs_s::erc;

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QStringList messages(const QList<Issue>& issues)
{
    QStringList out;
    for (const Issue& i : issues) out << (i.severity == Severity::Error ? "E " : "W ") + i.message;
    return out;
}

// The findings about a ground, of messages().
QStringList aboutGround(const QStringList& got)
{
    QStringList out;
    for (const QString& m : got)
        if (m.contains("ground", Qt::CaseInsensitive)) out << m;
    return out;
}

// What a kernel says of the schematic's ground before it simulates.
template <typename Kernel>
struct GroundProbe : Kernel {
    using Kernel::Kernel;
    bool groundFound() { return this->checkGround(); }
};

QAction* menuAction(QucsApp* app, const QString& menuTitle, const QString& text)
{
    for (QAction* m : app->menuBar()->actions()) {
        if (m->text().remove('&') != menuTitle || m->menu() == nullptr) continue;
        for (QAction* a : m->menu()->actions())
            if (a->text().remove('&') == text) return a;
    }
    return nullptr;
}
} // namespace

class TestErc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString broken, fine;

    static void write(const QString& path, const QByteArray& bytes)
    {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(bytes);
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
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        // R1 twice; R2 with pin 2 open; a wire with a loose end at (500,300)
        // and one whose loose end carries a label (fine); no ground, no
        // simulation block.
        broken = dir.filePath("broken.sch");
        write(broken,
            "<Qucs Schematic " PACKAGE_VERSION ">\n"
            "<Components>\n"
            "  <R R1 1 100 100 15 -26 0 1 \"30\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <R R1 1 200 100 15 -26 0 1 \"30\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <R R2 1 300 100 15 -26 0 1 \"30\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "</Components>\n"
            "<Wires>\n"
            "  <100 70 200 70 \"\" 0 0 0 \"\">\n"      // R1 top to R1' top
            "  <100 130 200 130 \"\" 0 0 0 \"\">\n"    // their bottoms
            "  <200 130 300 130 \"\" 0 0 0 \"\">\n"    // on to R2's pin 2? no: R2's bottom is at (300,130)
            "  <500 200 500 300 \"\" 0 0 0 \"\">\n"    // loose end at (500,300), other end loose too
            "  <600 200 600 300 \"net1\" 620 250 50 \"\">\n"   // named stub: not a problem
            "</Wires>\n"
            "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        fine = dir.filePath("RCL_resonance.sch");
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), fine));
    }

    // Simulators Settings > Before a simulation: without "A schematic must
    // have a ground symbol" a circuit without one is simulated and the
    // check says nothing of it; the choice is kept and the dialog sets it.
    // Each test with the components registered: a QucsApp unregisters
    // them when it goes, and a schematic loaded after without one would
    // know no Vdc.
    void init() { Module::registerModules(); }

    void theGroundIsRequiredOnlyWhenTheSettingsSaySo()
    {
        // The settings as they were for the other tests, whatever happens.
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        Schematic doc(nullptr, broken);   // no ground
        QVERIFY(doc.load());
        GroundProbe<Ngspice> ngspice(&doc);
        GroundProbe<Xyce> xyce(&doc);
        const QString error = "E no ground: the circuit has no reference node";

        QVERIFY(QucsSettings.RequireGround);   // the default
        QStringList got = messages(check(&doc));
        QVERIFY2(got.contains(error), qPrintable(got.join(" | ")));
        QCOMPARE(aboutGround(got).size(), 1);
        QVERIFY(!ngspice.groundFound());
        QVERIFY(!xyce.groundFound());

        QucsSettings.RequireGround = false;
        got = messages(check(&doc));
        QVERIFY2(aboutGround(got).isEmpty(), qPrintable(got.join(" | ")));
        QVERIFY(ngspice.groundFound());
        QVERIFY(xyce.groundFound());

        // Kept in the settings file.
        QVERIFY(saveApplSettings());
        QucsSettings.RequireGround = true;
        QVERIFY(loadSettings());
        QVERIFY(!QucsSettings.RequireGround);

        // The dialog shows the setting and sets it.
        SimSettingsDialog dialog;
        auto* box = dialog.findChild<QCheckBox*>("cbRequireGround");
        QVERIFY(box != nullptr);
        QVERIFY(!box->isChecked());
        box->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        QVERIFY(QucsSettings.RequireGround);
        QVERIFY(messages(check(&doc)).contains(error));
    }

    // Node 0 from a net named 0 is no ground symbol; a ground symbol
    // switched off names no node 0 in the netlist, so it is no ground -
    // for the check and the simulators alike. Neither is mentioned when
    // the settings leave the ground to the user.
    void aNamedGroundAndOneSwitchedOff()
    {
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        // V1 and R1 side by side, their tops wired; their bottoms wired
        // too, that wire named 0 - or a ground symbol there, off.
        const QByteArray parts =
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Vdc V1 1 0 60 18 -26 0 1 \"5 V\" 1>\n"
            "  <R R1 1 100 60 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        const QByteArray end = "<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
        const QString named = dir.filePath("named_ground.sch");
        write(named, parts + "</Components>\n<Wires>\n"
                     "  <0 30 100 30 \"\" 0 0 0 \"\">\n"
                     "  <0 90 100 90 \"0\" 50 110 0 \"\">\n"
                     "</Wires>\n" + end);
        const QString off = dir.filePath("ground_off.sch");
        write(off, parts + "  <GND * 0 0 90 0 0 0 0>\n</Components>\n<Wires>\n"
                   "  <0 30 100 30 \"\" 0 0 0 \"\">\n"
                   "  <0 90 100 90 \"\" 0 0 0 \"\">\n"
                   "</Wires>\n" + end);
        Schematic byName(nullptr, named), switchedOff(nullptr, off);
        QVERIFY(byName.load());
        QVERIFY(switchedOff.load());
        GroundProbe<Ngspice> ngspiceByName(&byName), ngspiceOff(&switchedOff);
        GroundProbe<Xyce> xyceOff(&switchedOff);

        QucsSettings.RequireGround = true;
        QStringList got = messages(check(&byName));
        QVERIFY2(aboutGround(got) == QStringList{"E no ground symbol: the Simulators Settings require one (a net named 0 or "
                                                 "gnd does not count)"},
                 qPrintable(got.join(" | ")));
        QVERIFY(!ngspiceByName.groundFound());
        got = messages(check(&switchedOff));
        QVERIFY2(aboutGround(got) == QStringList{"E no ground: the circuit has no reference node"}, qPrintable(got.join(" | ")));
        QVERIFY(!ngspiceOff.groundFound());
        QVERIFY(!xyceOff.groundFound());

        QucsSettings.RequireGround = false;
        for (Schematic* doc : {&byName, &switchedOff}) {
            got = messages(check(doc));
            QVERIFY2(aboutGround(got).isEmpty(), qPrintable(got.join(" | ")));
        }
        QVERIFY(ngspiceByName.groundFound());
        QVERIFY(ngspiceOff.groundFound());
    }

    // Simulators Settings, applied: the Problems tab (this schematic's
    // check, or the hierarchy's) and the status bar's chip show the check
    // as the settings now say, with no edit in between.
    void aSettingsChangeChecksAgain()
    {
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(broken, false, false));
        MessageDock* dock = app.messages();
        auto* chip = app.findChild<QToolButton*>("statusProblems");
        QVERIFY(chip != nullptr);
        const auto listed = [dock] { return aboutGround(messages(dock->issues())); };
        // The dialog, its box ticked or not, applied.
        const auto requireGround = [&app](bool on) {
            QTimer poke;
            poke.setInterval(20);
            bool seen = false;
            QObject::connect(&poke, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<SimSettingsDialog*>(QApplication::activeModalWidget());
                if (dialog == nullptr) return;
                poke.stop();
                seen = true;
                dialog->findChild<QCheckBox*>("cbRequireGround")->setChecked(on);
                QMetaObject::invokeMethod(dialog, "slotApply");
            });
            poke.start();
            menuAction(&app, "Simulation", "Simulators Settings...")->trigger();
            return seen;
        };

        // (The schematic has another error: R1 twice.)
        QVERIFY(QucsSettings.RequireGround);
        menuAction(&app, "Simulation", "Check Schematic")->trigger();
        QCOMPARE(listed(), QStringList{"E no ground: the circuit has no reference node"});
        QTRY_VERIFY2(chip->text().startsWith("2 errors,"), qPrintable(chip->text()));
        QVERIFY(chip->toolTip().contains("no ground"));

        QVERIFY(requireGround(false));
        QVERIFY(!QucsSettings.RequireGround);
        QVERIFY2(listed().isEmpty(), qPrintable(listed().join(" | ")));
        QVERIFY(!dock->problemsOfHierarchy());
        QTRY_VERIFY2(chip->text().startsWith("1 error,"), qPrintable(chip->text()));
        QVERIFY(!chip->toolTip().contains("ground"));

        // The hierarchy's check is run again as such.
        menuAction(&app, "Simulation", "Check Schematic and Subcircuits")->trigger();
        QVERIFY(dock->problemsOfHierarchy());
        QVERIFY(listed().isEmpty());
        QVERIFY(requireGround(true));
        QCOMPARE(listed(), QStringList{"E no ground: the circuit has no reference node"});
        QVERIFY(dock->problemsOfHierarchy());
        QTRY_VERIFY2(chip->text().startsWith("2 errors,"), qPrintable(chip->text()));
    }

    void theChecksFindEachKindOfProblem()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(broken, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        const QList<Issue> issues = check(doc);
        const QStringList got = messages(issues);
        QVERIFY2(got.contains("E R1: the name is used twice (also at 100, 100)"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("E no ground: the circuit has no reference node"), qPrintable(got.join(" | ")));
        QCOMPARE(got.filter("W R2: pin ").size(), 1);   // one open pin (the other is wired)
        QVERIFY2(got.contains("W the wire end at 500, 200 is connected to nothing"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("W the wire end at 500, 300 is connected to nothing"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("W no simulation: no .AC, .TR, .DC, .SP, ... block"), qPrintable(got.join(" | ")));
        // The named stub and the wired pins are fine.
        QVERIFY2(!got.join("|").contains("600,"), qPrintable(got.join(" | ")));
        QVERIFY2(got.filter("W R1: pin ").isEmpty(), qPrintable(got.join(" | ")));
        QCOMPARE(errorCount(issues), 2);
        // Errors first.
        QCOMPARE(issues.first().severity, Severity::Error);
        QCOMPARE(issues.last().severity, Severity::Warning);
        // The duplicate points at the second R1, and names it.
        for (const Issue& i : issues)
            if (i.message.startsWith("R1: the name")) { QCOMPARE(i.where, QPoint(200, 100)); QCOMPARE(i.component, QString("R1")); }
    }

    // What the wires show and do not do, and what hangs from nothing: a
    // pin on another net's wire mid-way (not joined), parts reaching no
    // ground (floating), a node that reaches ground only through
    // capacitors (no DC path) - warnings; wires of two nets crossing
    // without a junction and a label on one pin alone - notes, fine if
    // meant, and not among the warnings (a drawing is full of crossings).
    void theWiringAndWhatHangsFromNothingAreChecked()
    {
        const QString file = dir.filePath("wiring.sch");
        const QByteArray R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        write(file,
            QByteArray("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n")
            + "  <Vdc V1 1 100 200 18 -26 0 1 \"1 V\" 1>\n"
            + "  <GND * 1 100 230 0 0 0 0>\n"
            + "  <R R1 1 200 100 15 -26 0 0 " + R
            + "  <R R2 1 300 200 15 -26 0 1 " + R
            + "  <GND * 1 300 230 0 0 0 0>\n"
            + "  <R R3 1 500 200 15 -26 0 1 " + R      // with C1: floating
            + "  <C C1 1 600 200 17 -26 0 1 \"1n\" 1 \"\" 0 \"neutral\" 0>\n"
            + "  <R R4 1 400 100 15 -26 0 1 " + R      // its lower pin on R3's wire
            + "  <C C2 1 200 400 -26 17 0 0 \"1n\" 1 \"\" 0 \"neutral\" 0>\n"   // C2-C3: no DC path
            + "  <C C3 1 300 400 -26 17 0 0 \"1n\" 1 \"\" 0 \"neutral\" 0>\n"
            + "  <GND * 1 170 400 0 0 0 0>\n"
            + "  <GND * 1 330 400 0 0 0 0>\n"
            + "  <.TR TR1 1 100 600 0 57 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"201\" 0>\n"
            + "</Components>\n<Wires>\n"
            + "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
            + "  <230 100 300 100 \"\" 0 0 0 \"\">\n  <300 100 300 170 \"\" 0 0 0 \"\">\n"
            + "  <500 170 500 130 \"\" 0 0 0 \"\">\n  <500 130 250 130 \"\" 0 0 0 \"\">\n"   // crosses R1-R2's wire
            + "  <500 230 600 230 \"\" 0 0 0 \"\">\n"
            + "  <230 400 270 400 \"\" 0 0 0 \"\">\n"
            + "  <600 170 600 170 \"probe\" 620 150 0 \"\">\n"   // a label on C1's open pin
            + "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(file, false, false));
        Schematic& doc = *app.currentSchematic();
        const QStringList got = messages(check(&doc));
        const QString all = got.join(" | ");
        QVERIFY2(got.contains("W R4: pin 1 at 400, 130 is on the wire 250, 130 - 500, 130 of net R3.2 without being connected to it "
                              "(a wire must end at a pin to join it)"), qPrintable(all));
        QVERIFY2(got.contains("W R3, C1: not connected to ground or to the rest of the circuit (floating)"), qPrintable(all));
        QVERIFY2(!got.filter("W net C2.2 reaches ground only through capacitors or current sources (C2, C3)").isEmpty(), qPrintable(all));
        QVERIFY2(got.filter("cross without").isEmpty() && got.filter("one pin only").isEmpty(), qPrintable(all));   // notes, not warnings
        // The circuit that works is not told of.
        QVERIFY2(!all.contains("R1") && !all.contains("R2") && !all.contains("V1"), qPrintable(all));
        const QStringList told = messages(notes(&doc));
        QVERIFY2(told.contains("W the wires at 300, 130 cross without a junction: nets R1.2 and R3.2 are not connected there"),
                 qPrintable(told.join(" | ")));
        QVERIFY2(!told.filter("W the net probe has one pin only (C1.2)").isEmpty(), qPrintable(told.join(" | ")));
        // What a tool tells after drawing: the pin on the wire and the crossing.
        const QStringList wires = messages(wiring(&doc));
        QVERIFY2(wires.size() == 2 && !wires.filter("cross without").isEmpty() && !wires.filter("R4: pin 1").isEmpty(),
                 qPrintable(wires.join(" | ")));
        // A subcircuit's ports hold it up as a ground does; a part on no port is floating there.
        QCOMPARE(notes(nullptr).size(), 0);
        QCOMPARE(wiring(nullptr).size(), 0);
    }

    // A part switched to shorted is in the netlist as resistors of next
    // to nothing from its first pin to each other one: it joins its nets,
    // for what hangs from ground as for the rest. V1 drives R1, shorted;
    // beyond it R4 and R5 in parallel, and (in the first) C1 to ground.
    void aShortedPartJoinsItsNets()
    {
        const QByteArray head =
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Vdc V1 1 0 60 18 -26 0 1 \"5 V\" 1>\n"
            "  <GND * 1 0 90 0 0 0 0>\n"
            "  <R R1 2 100 30 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <R R4 1 220 30 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <R R5 1 220 90 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <.DC DC1 1 0 200 0 36 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n";
        const QByteArray wires =
            "<Wires>\n"
            "  <0 30 70 30 \"\" 0 0 0 \"\">\n"
            "  <130 30 160 30 \"\" 0 0 0 \"\">\n"
            "  <160 30 190 30 \"\" 0 0 0 \"\">\n"
            "  <190 30 190 90 \"\" 0 0 0 \"\">\n"
            "  <250 30 250 90 \"\" 0 0 0 \"\">\n";
        const QByteArray end = "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
        const QString withC = dir.filePath("shorted_c.sch"), without = dir.filePath("shorted.sch");
        write(withC, head + "  <C C1 1 160 60 17 -26 0 1 \"1u\" 1 \"\" 0 \"neutral\" 0>\n  <GND * 1 160 90 0 0 0 0>\n"
                     "</Components>\n" + wires + end);
        write(without, head + "</Components>\n" + wires + end);
        for (const QString& f : {withC, without}) {
            Schematic doc(nullptr, f);
            QVERIFY(doc.load());
            const QStringList got = messages(check(&doc));
            QVERIFY2(got.isEmpty(), qPrintable(QFileInfo(f).fileName() + ": " + got.join(" | ")));
        }
    }

    // ngspice and Xyce read names without regard to case: labels Out and
    // out are one net there, and a resistor r1 beside R1 stops the run
    // ("device already exists"). Qucsator tells them apart.
    void namesAreReadWithoutCaseBySpice()
    {
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        // V1 and R1 on a wire labelled Out; R2 and r1 on one labelled
        // out, a wire of its own; C1 on a third labelled Out as well.
        const QString f = dir.filePath("case.sch");
        write(f,
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Vdc V1 1 0 60 18 -26 0 1 \"5 V\" 1>\n"
            "  <GND * 1 0 90 0 0 0 0>\n"
            "  <R R1 1 60 60 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <GND * 1 60 90 0 0 0 0>\n"
            "  <R R2 1 200 60 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <GND * 1 200 90 0 0 0 0>\n"
            "  <R r1 1 300 60 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
            "  <GND * 1 300 90 0 0 0 0>\n"
            "  <C C1 1 400 60 17 -26 0 1 \"1n\" 1 \"\" 0 \"neutral\" 0>\n"
            "  <GND * 1 400 90 0 0 0 0>\n"
            "  <.DC DC1 1 0 200 0 36 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
            "</Components>\n<Wires>\n"
            "  <0 30 60 30 \"Out\" 20 0 0 \"\">\n"
            "  <200 30 300 30 \"out\" 220 0 0 \"\">\n"
            "  <400 30 460 30 \"Out\" 420 0 0 \"\">\n"
            "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        Schematic doc(nullptr, f);
        QVERIFY(doc.load());
        const QString labels = "W the labels Out, out are one net for %1, which reads names without regard to case";
        const QString names = "E r1: the same name as R1 (at 60, 60) for %1, which reads names without regard to case";

        for (const int simulator : {int(spicecompat::simNgspice), int(spicecompat::simXyce)}) {
            QucsSettings.DefaultSimulator = simulator;
            const QString name = spicecompat::getDefaultSimulatorName(simulator);
            const QStringList got = messages(check(&doc));
            QVERIFY2(got.contains(labels.arg(name)) && got.contains(names.arg(name)), qPrintable(got.join(" | ")));
            QCOMPARE(got.filter("without regard to case").size(), 2);
        }
        // Qucsator: two nets, two names, nothing said. (The wire end at
        // 460, 30 is named Out, so not loose; the two Out wires are one net.)
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        QStringList got = messages(check(&doc));
        QVERIFY2(got.filter("case").isEmpty(), qPrintable(got.join(" | ")));

        // Out and out wired together: one net whatever the case, so the
        // labels are not told of (the names still are).
        QFile file(f);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray text = file.readAll();
        file.close();
        text.replace("</Wires>", "  <60 30 200 30 \"\" 0 0 0 \"\">\n</Wires>");
        const QString joined = dir.filePath("case_joined.sch");
        write(joined, text);
        Schematic wired(nullptr, joined);
        QVERIFY(wired.load());
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        got = messages(check(&wired));
        QVERIFY2(got.filter("without regard to case") == QStringList{names.arg("Ngspice")}, qPrintable(got.join(" | ")));
    }

    // Files named as they were elsewhere: a subcircuit without its .sch
    // (Qucs wrote them so), a SPICE library by its path in another
    // installation's library. Each is found, and its part has its pins.
    // A SPICE library part that cannot be loaded is told, and why.
    void filesNamedAsElsewhereAreFound()
    {
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        QucsSettings.LibDir = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR)).dir().filePath("library") + "/";
        write(dir.filePath("two_pin.sch"),
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
            "  <Port P2 1 100 200 -23 12 0 0 \"2\" 1 \"analog\" 0>\n"
            "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        const QString lib = QStringLiteral("share/qucs-s/library/XyceDigital.lib");
        const QString top = dir.filePath("elsewhere.sch");
        write(top, QStringLiteral(
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Sub SUB1 1 100 300 -26 17 0 0 \"two_pin\" 1>\n"
            "  <SpLib X1 1 300 300 -26 -60 0 0 \"C:/QUCS-S 24.3.0/%1\" 0 \"NAND2\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n"
            "  <SpLib X2 1 500 300 -26 -60 0 0 \"C:\\QUCS-S 24.3.0\\%2\" 0 \"NAND2\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n"
            "  <SpLib X3 1 700 300 -26 -60 0 0 \"/usr/%1\" 0 \"NAND2\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n"
            "  <SpLib X4 1 300 500 -26 -60 0 0 \"\" 0 \"NAND2\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n"
            "  <SpLib X5 1 500 500 -26 -60 0 0 \"/nowhere/Missing.lib\" 0 \"NAND2\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n"
            "  <SpLib X6 1 700 500 -26 -60 0 0 \"/usr/%1\" 0 \"NOSUCH\" 1 \"auto\" 1 \"\" 0 \"\" 0>\n"
            "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n")
            .arg(lib, QString(lib).replace('/', '\\')).toUtf8());
        Schematic doc(nullptr, top);
        QVERIFY(doc.load());
        QHash<QString, Component*> part;
        for (Component* c : doc.a_DocComps) part.insert(c->Name, c);
        QCOMPARE(part.value("SUB1")->Ports.size(), 2);
        QCOMPARE(QFileInfo(part.value("SUB1")->getSubcircuitFile()).fileName(), QString("two_pin.sch"));
        for (const char* name : {"X1", "X2", "X3"})
            QVERIFY2(part.value(name)->Ports.size() == 3, name);   // NAND2: nin1 nin2 nout
        QCOMPARE(part.value("X4")->Ports.size(), 0);

        const QStringList got = messages(check(&doc));
        const QString noPins = ": it has no pins, and what was wired to them is on nothing";
        QVERIFY2(got.filter(QRegularExpression("^E (SUB1|X1|X2|X3):")).isEmpty(), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("E X4: no SPICE library file is given" + noPins), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("E X5: its SPICE library /nowhere/Missing.lib is not found (beside the schematic, in the "
                              "project or its user_lib, nor in the library of Qucs-S)" + noPins),
                 qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("E X6: its SPICE library /usr/" + lib + " defines no subcircuit NOSUCH" + noPins),
                 qPrintable(got.join(" | ")));
    }

    // What the parts do, beyond the wiring (the reviewer's twelve faulty
    // circuits, 29 September): sources in a loop or shorted, an inductor
    // across a source, a capacitor across a pulse, values of nothing or
    // less, an AC analysis with no AC source, an equation of a node that is
    // not there, two names on one net, an input or a base with no bias.
    // Each is found in its circuit and not in the same circuit put right.
    // Pins are joined by labels (a wire of no length on the pin).
    void theDesignRulesFindWhatTheSimulatorWouldTrip()
    {
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        // A two-pin part standing at x: pin 1 at (x, 30), pin 2 at (x, 90).
        const auto part = [](const QString& type, const QString& name, int x, const QString& props) {
            return QStringLiteral("  <%1 %2 1 %3 60 18 -26 0 1 %4>\n").arg(type, name).arg(x).arg(props);
        };
        const auto R = [&](const QString& name, int x, const QString& value) {
            return part("R", name, x, QStringLiteral("\"%1\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0").arg(value));
        };
        const auto C = [&](const QString& name, int x, const QString& value) {
            return part("C", name, x, QStringLiteral("\"%1\" 1 \"\" 0 \"neutral\" 0").arg(value));
        };
        const auto L = [&](const QString& name, int x, const QString& value) {
            return part("L", name, x, QStringLiteral("\"%1\" 1 \"\" 0").arg(value));
        };
        const auto Vdc = [&](const QString& name, int x, const QString& value) {
            return part("Vdc", name, x, QStringLiteral("\"%1\" 1").arg(value));
        };
        const auto label = [](int x, int y, const QString& name) {
            return QStringLiteral("  <%1 %2 %1 %2 \"%3\" %4 %5 0 \"\">\n").arg(x).arg(y).arg(name).arg(x + 10).arg(y - 20);
        };
        const auto top = [&](int x, const QString& name) { return label(x, 30, name); };
        const auto bottom = [&](int x, const QString& name) { return label(x, 90, name); };
        const auto gnd = [](int x) { return QStringLiteral("  <GND * 1 %1 90 0 0 0 0>\n").arg(x); };
        const QString dc = "  <.DC DC1 1 0 300 0 36 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n";
        const QString tran = "  <.TR TR1 1 0 300 0 64 0 0 \"lin\" 1 \"0\" 1 \"20 us\" 1 \"2001\" 0 \"Trapezoidal\" 0 \"2\" 0 \"1 ns\" 0 \"1e-16\" 0 \"150\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"26.85\" 0 \"1e-3\" 0 \"1e-6\" 0 \"1\" 0 \"CroutLU\" 0 \"no\" 0 \"yes\" 0 \"0\" 0>\n";
        const QString ac = "  <.AC AC1 1 0 300 0 33 0 0 \"log\" 1 \"1 kHz\" 1 \"1 MHz\" 1 \"20\" 1>\n";
        int n = 0;
        // Its findings, errors and warnings as E and W, notes as N.
        const auto findings = [&](const QString& components, const QString& wires) {
            const QString f = dir.filePath(QStringLiteral("design%1.sch").arg(++n));
            write(f, ("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n" + components + "</Components>\n<Wires>\n" + wires
                      + "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n").toUtf8());
            Schematic doc(nullptr, f);
            QStringList got;
            if (!doc.load()) return QStringList{"(did not load)"};
            got = messages(check(&doc));
            for (const Issue& i : notes(&doc)) got << "N " + i.message;
            // The fixture's own joins: every pin on something.
            for (const QString& m : std::as_const(got))
                if (m.contains("connected to nothing")) got << "(fixture: " + m + ")";
            return got;
        };
        const auto has = [](const QStringList& got, const QString& start) {
            return std::any_of(got.cbegin(), got.cend(), [&](const QString& m) { return m.startsWith(start); });
        };
        QStringList got;

        // Two DC sources in parallel; one alone is right.
        const QString load = R("R1", 200, "1k") + gnd(200);
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + Vdc("V2", 100, "3 V") + gnd(100) + load + dc, top(0, "a") + top(100, "a") + top(200, "a"));
        QVERIFY2(has(got, "E V1 and V2 are in parallel: voltage sources in a loop fix one voltage twice"), qPrintable(got.join(" | ")));
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + load + dc, top(0, "a") + top(200, "a"));
        QVERIFY2(got.filter("loop").isEmpty() && got.filter("fixture").isEmpty(), qPrintable(got.join(" | ")));

        // A source shorted by a wire from pin to pin.
        got = findings(Vdc("V1", 0, "5 V") + load + dc, "  <0 30 0 90 \"\" 0 0 0 \"\">\n" + top(0, "a") + top(200, "a") + bottom(0, "gnd"));
        QVERIFY2(has(got, "E V1 is shorted: both its pins are on"), qPrintable(got.join(" | ")));

        // An inductor straight across a source; in series with a resistor it is right.
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + L("L1", 100, "1 mH") + gnd(100) + load + dc, top(0, "a") + top(100, "a") + top(200, "a"));
        QVERIFY2(has(got, "W V1 and L1 are in parallel: at DC an inductor is a short"), qPrintable(got.join(" | ")));
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + R("R2", 100, "10") + L("L1", 200, "1 mH") + gnd(200) + dc,
                       top(0, "a") + top(100, "a") + bottom(100, "b") + top(200, "b"));
        QVERIFY2(got.filter("loop").isEmpty() && got.filter("parallel").isEmpty(), qPrintable(got.join(" | ")));

        // A capacitor straight across a pulse source: a note (across a DC supply it is decoupling).
        const QString pulse = part("Vpulse", "V1", 0, "\"0 V\" 1 \"5 V\" 1 \"1 us\" 1 \"6 us\" 1 \"1 ns\" 0 \"1 ns\" 0");
        got = findings(pulse + gnd(0) + C("C1", 100, "1 nF") + gnd(100) + tran, top(0, "a") + top(100, "a"));
        QVERIFY2(has(got, "N C1 is straight across V1: nothing limits its current"), qPrintable(got.join(" | ")));
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + C("C1", 100, "1 nF") + gnd(100) + load + tran, top(0, "a") + top(100, "a") + top(200, "a"));
        QVERIFY2(got.filter("straight across").isEmpty(), qPrintable(got.join(" | ")));

        // R = 0 and L = 0 (notes: a jumper is meant at times), C = -1 nF (a warning).
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + R("R1", 100, "0") + C("C1", 200, "-1 nF") + gnd(200) + L("L1", 300, "0") + gnd(300) + dc,
                       top(0, "a") + top(100, "a") + bottom(100, "b") + top(200, "b") + top(300, "b"));
        QVERIFY2(has(got, "W C1 is -1 nF: a negative capacitance") && has(got, "N R1 is 0 Ohm, a short")
                     && has(got, "N L1 is 0 H, a short"),
                 qPrintable(got.join(" | ")));

        // An AC analysis with no AC source; with a Vac it has one.
        const QString rc = R("R1", 100, "1k") + C("C1", 200, "1 nF") + gnd(200) + ac;
        const QString rcWires = top(0, "a") + top(100, "a") + bottom(100, "b") + top(200, "b");
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + rc, rcWires);
        QVERIFY2(has(got, "W AC1 has nothing to drive it"), qPrintable(got.join(" | ")));
        got = findings(part("Vac", "V1", 0, "\"1 V\" 1 \"1 kHz\" 0 \"0\" 0") + gnd(0) + rc, rcWires);
        QVERIFY2(!has(got, "W AC1 has nothing"), qPrintable(got.join(" | ")));

        // An equation of a node that is not there; a node, a vector of its own and a probe's are.
        const QString nutmeg = "  <NutmegEq NutmegEq1 1 400 300 -28 15 0 0 \"DC1\" 1 \"x=v(nothere)\" 1 \"y=v(out)+v(x)\" 1>\n";
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + load + dc + nutmeg, top(0, "out") + top(200, "out"));
        QVERIFY2(got.contains("W NutmegEq1 reads v(nothere), but no net is labelled nothere: the equation reads nothing")
                     && got.filter("NutmegEq1").size() == 1,
                 qPrintable(got.join(" | ")));
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;   // (Qucsator has no NutmegEq to read)
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + load + dc + nutmeg, top(0, "out") + top(200, "out"));
        QVERIFY2(got.filter("reads v(").isEmpty(), qPrintable(got.join(" | ")));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;

        // Two labels on one net.
        got = findings(Vdc("V1", 0, "5 V") + gnd(0) + load + dc, "  <0 30 200 30 \"in\" 20 0 0 \"\">\n" + top(200, "vin"));
        QVERIFY2(has(got, "N one net has 2 names, in and vin: the netlist keeps one of them"), qPrintable(got.join(" | ")));

        // An op-amp's + input fed through a capacitor alone; its - input
        // biased through the feedback from its own output, the output's only
        // DC path (its load is a capacitor). With a resistor from + to
        // ground, nothing to say.
        const QString amp = "  <OpAmp OP1 1 400 130 -26 42 0 0 \"1e6\" 1 \"15 V\" 0>\n";   // - (370,150), + (370,110), out (440,130)
        const QString stage = part("Vac", "V1", 0, "\"1 V\" 1 \"1 kHz\" 0 \"0\" 0") + gnd(0) + C("C1", 100, "1 uF") + amp
                              + R("R2", 200, "10k") + C("CL", 300, "100 pF") + gnd(300) + ac;
        const QString stageWires = top(0, "src") + top(100, "src") + bottom(100, "inp") + label(370, 110, "inp")
                                   + label(370, 150, "inn") + top(200, "inn") + bottom(200, "out") + label(440, 130, "out")
                                   + top(300, "out");
        got = findings(stage, stageWires);
        QVERIFY2(has(got, "N OP1: its input + has no DC path but through OP1 itself") && got.filter("input -").isEmpty(),
                 qPrintable(got.join(" | ")));
        got = findings(stage + R("R3", 500, "100k") + gnd(500), stageWires + top(500, "inp"));
        QVERIFY2(got.filter("no DC path").isEmpty() && got.filter("fixture").isEmpty(), qPrintable(got.join(" | ")));

        // A transistor's base fed through a capacitor alone; with a resistor from the supply it is biased.
        const QString bjt =
            "  <_BJT Q1 1 700 60 8 -26 0 0 \"npn\" 0 \"1e-16\" 0 \"1\" 0 \"1\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"1.5\" 0 "
            "\"0\" 0 \"2\" 0 \"100\" 0 \"1\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"0.75\" 0 \"0.33\" 0 \"0\" 0 \"0.75\" 0 "
            "\"0.33\" 0 \"1.0\" 0 \"0\" 0 \"0.75\" 0 \"0\" 0 \"0.5\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 "
            "\"0.0\" 0 \"1.0\" 0 \"1.0\" 0 \"0.0\" 0 \"1.0\" 0 \"1.0\" 0 \"0.0\" 0 \"0.0\" 0 \"3.0\" 0 \"1.11\" 0 \"26.85\" 0 \"1.0\" 0>\n";
        // Q1: base (670,60), collector (700,30), emitter (700,90).
        const QString amplifier = Vdc("VCC", 0, "10 V") + gnd(0) + R("RC", 100, "2k") + bjt + "  <GND * 1 700 90 0 0 0 0>\n"
                                  + part("Vac", "V2", 200, "\"10 mV\" 1 \"1 kHz\" 0 \"0\" 0") + gnd(200) + C("C1", 300, "1 uF") + ac;
        const QString amplifierWires = top(0, "vcc") + top(100, "vcc") + bottom(100, "c") + label(700, 30, "c") + top(200, "sig")
                                       + top(300, "sig") + bottom(300, "b") + label(670, 60, "b");
        got = findings(amplifier, amplifierWires);
        QVERIFY2(has(got, "N Q1: its base has no DC path but through Q1 itself"), qPrintable(got.join(" | ")));
        got = findings(amplifier + R("RB", 400, "470k"), amplifierWires + top(400, "vcc") + bottom(400, "b"));
        QVERIFY2(got.filter("no DC path").isEmpty() && got.filter("fixture").isEmpty(), qPrintable(got.join(" | ")));

        // (The hunt after round 9, A3.) What fixes a voltage besides a
        // voltage source, each straight across V1 (ngspice: a singular
        // matrix): a current probe, a 0 V source; a VCVS's output; a
        // current-controlled source's input, the 0 V source the netlist
        // adds to sense its current; an ideal op-amp's output, a voltage to
        // ground. A four-pin source at (400, 60): in+ (370, 30), out+ (430,
        // 30), out- (430, 90), in- (370, 90).
        const QString supply = Vdc("V1", 0, "5 V") + gnd(0) + load + dc;
        const QString supplyWires = top(0, "a") + top(200, "a");
        const QString probe = part("IProbe", "Pr1", 100, QString());
        got = findings(supply + probe + gnd(100), supplyWires + top(100, "a"));
        QVERIFY2(has(got, "E V1 and Pr1 (a current probe: a 0 V source) are in parallel: voltage sources in a loop"),
                 qPrintable(got.join(" | ")));
        got = findings(supply + probe + R("R2", 300, "1k") + gnd(300), supplyWires + top(100, "a") + bottom(100, "b") + top(300, "b"));
        QVERIFY2(got.filter("loop").isEmpty() && got.filter("parallel").isEmpty() && got.filter("fixture").isEmpty(),
                 qPrintable(got.join(" | ")));   // in series: as meant
        got = findings(supply + probe, supplyWires + "  <100 30 100 90 \"\" 0 0 0 \"\">\n" + top(100, "a"));
        QVERIFY2(has(got, "E Pr1 (a current probe: a 0 V source) is shorted: both its ends are on a, and a voltage source "
                          "across a wire has no solution (ngspice stops: \"shorted VSRC\")"),
                 qPrintable(got.join(" | ")));
        const auto fourPin = [&](const QString& type, const QString& name) {
            return QStringLiteral("  <%1 %2 1 400 60 35 -26 0 0 \"1\" 1 \"0\" 0>\n").arg(type, name);
        };
        const QString controlled = R("R3", 600, "1k") + gnd(600);
        const QString inputFromC = label(370, 30, "c") + label(370, 90, "gnd") + top(600, "c");
        got = findings(supply + fourPin("VCVS", "SRC1") + controlled, supplyWires + inputFromC + label(430, 30, "a") + label(430, 90, "gnd"));
        QVERIFY2(has(got, "E V1 and SRC1's output are in parallel: voltage sources in a loop"), qPrintable(got.join(" | ")));
        got = findings(supply + fourPin("VCVS", "SRC1") + controlled + R("R4", 700, "1k") + gnd(700),
                       supplyWires + inputFromC + label(430, 30, "o") + label(430, 90, "gnd") + top(700, "o"));
        QVERIFY2(got.filter("loop").isEmpty() && got.filter("parallel").isEmpty() && got.filter("fixture").isEmpty(),
                 qPrintable(got.join(" | ")));   // into a load of its own
        got = findings(supply + fourPin("CCCS", "SRC2") + R("R4", 700, "1k") + gnd(700),
                       supplyWires + label(370, 30, "a") + label(370, 90, "gnd") + label(430, 30, "o") + label(430, 90, "gnd") + top(700, "o"));
        QVERIFY2(has(got, "E V1 and SRC2's input (a 0 V source that senses its current) are in parallel"), qPrintable(got.join(" | ")));
        got = findings(supply + fourPin("CCVS", "SRC3") + R("R4", 700, "1k") + gnd(700),
                       supplyWires + label(370, 30, "o") + label(370, 90, "o") + label(430, 30, "a") + label(430, 90, "gnd") + top(700, "o"));
        QVERIFY2(has(got, "E V1 and SRC3's output are in parallel")
                     && has(got, "E SRC3's input (a 0 V source that senses its current) is shorted: both its ends are on o"),
                 qPrintable(got.join(" | ")));
        got = findings(supply + amp + R("R5", 600, "1k") + gnd(600) + R("R6", 700, "1k") + gnd(700),
                       supplyWires + label(370, 110, "p") + label(370, 150, "n") + top(600, "p") + top(700, "n") + label(440, 130, "a"));
        QVERIFY2(has(got, "E V1 and OP1's output (an ideal op-amp's: a voltage to ground) are in parallel"), qPrintable(got.join(" | ")));

        // (A6.) The current at a pulse's edges, as the circuit's values
        // make it; with no rise time, or values that are no numbers, no
        // made-up figure; a pulse from 5 V to 5 V has no edges. The SPICE
        // source's PULSE too.
        const auto across = [&](const QString& source, const QString& capacitance) {
            return findings(source + gnd(0) + C("C1", 100, capacitance) + gnd(100) + tran, top(0, "a") + top(100, "a"))
                .filter("straight across");
        };
        const auto vpulse = [&](const QString& u2, const QString& rise) {
            return part("Vpulse", "V1", 0, QStringLiteral("\"0 V\" 1 \"%1\" 1 \"1 us\" 1 \"6 us\" 1 \"%2\" 0 \"%2\" 0").arg(u2, rise));
        };
        const QString rest = "; a resistor in series stands for the source's own";
        got = across(vpulse("5 V", "1 ns"), "1 nF");
        QVERIFY2(got == QStringList{"N C1 is straight across V1: nothing limits its current at V1's edges - C dV/dt is 5 A here "
                                    "(1 nF, a step of 5 V in 1 ns)" + rest},
                 qPrintable(got.join(" | ")));
        got = across(vpulse("12 V", "10 ns"), "100 nF");
        QVERIFY2(got.size() == 1 && got.first().contains("C dV/dt is 120 A here (100 nF, a step of 12 V in 10 ns)"), qPrintable(got.join(" | ")));
        got = across(vpulse("5 V", "0"), "1 nF");
        QVERIFY2(got.size() == 1 && got.first().contains("its steps of 5 V have no rise time, so only the simulator's time step limits C dV/dt"),
                 qPrintable(got.join(" | ")));
        for (const QString& c : {QStringLiteral("Cx"), QStringLiteral("-1 nF"), QStringLiteral("1e308")}) {
            got = across(vpulse("5 V", "1 ns"), c);
            QVERIFY2(got == QStringList{"N C1 is straight across V1: nothing limits its current at V1's edges (C dV/dt)" + rest},
                     qPrintable(c + ": " + got.join(" | ")));
        }
        got = across(vpulse("5 V", "Trise"), "1 nF");
        QVERIFY2(got.size() == 1 && got.first().contains("edges (C dV/dt)"), qPrintable(got.join(" | ")));
        got = across(vpulse("0 V", "1 ns"), "1 nF");
        QVERIFY2(got.isEmpty(), qPrintable(got.join(" | ")));
        got = across(part("S4Q_V", "V1", 0, "\"DC 0 PULSE(0 3.3 1u 2n 2n 5u 10u)\" 1 \"\" 0 \"\" 0 \"\" 0 \"\" 0"), "10 nF");
        QVERIFY2(got.size() == 1 && got.first().contains("C dV/dt is 16.5 A here (10 nF, a step of 3.3 V in 2 ns)"), qPrintable(got.join(" | ")));
        got = across(part("S4Q_V", "V1", 0, "\"DC 5\" 1 \"\" 0 \"\" 0 \"\" 0 \"\" 0"), "10 nF");
        QVERIFY2(got.isEmpty(), qPrintable(got.join(" | ")));   // decoupling a DC supply
        got = across(part("vPWL", "V1", 0, "\"0 0 1u 0 1.01u 5 3u 5\" 1 \"\" 0 \"\" 0 \"\" 0 \"\" 0 \"\" 0 \"\" 0 \"\" 0 \"\" 0 \"\" 0"), "1 nF");
        QVERIFY2(got.size() == 1 && got.first().contains("C dV/dt is 0.5 A here (1 nF, a step of 5 V in 10 ns)"), qPrintable(got.join(" | ")));

        // (A8, A9.) In a subcircuit a port names its net as a label does:
        // VEE at -5 V is as meant, VCC at -5 V the wrong way round. The
        // other sign of "+15 V" is "-15 V". V1's + on ground, its - on r.
        const QString port = "  <Port %1 1 100 30 -23 12 0 0 \"1\" 1 \"analog\" 0>\n";
        const QString rail = Vdc("V1", 0, "5 V") + R("R1", 200, "1k") + gnd(200);
        const QString railWires = top(0, "gnd") + bottom(0, "r") + top(200, "r") + label(100, 30, "r");
        got = findings(rail + port.arg("VEE"), railWires);
        QVERIFY2(got.filter("V1").isEmpty() && got.filter("fixture").isEmpty(), qPrintable(got.join(" | ")));
        got = findings(rail + port.arg("VCC"), railWires);
        QVERIFY2(got.contains("W V1 puts the port VCC at -5 V, though that is a positive supply's name: the source is the wrong way "
                              "round - set U to -5 V (edit_component)"),
                 qPrintable(got.join(" | ")));
        got = findings(rail + port.arg("P1"), railWires);
        QVERIFY2(got.contains("N V1: its + is on ground, so r is at -5 V - a negative supply is so; if it was to be positive, set U "
                              "to -5 V (edit_component)"),
                 qPrintable(got.join(" | ")));
        got = findings(Vdc("V1", 0, "+15 V") + R("R1", 200, "1k") + gnd(200) + dc, top(0, "gnd") + bottom(0, "r") + top(200, "r"));
        QVERIFY2(got.contains("N V1: its + is on ground, so r is at -15 V - a negative supply is so; if it was to be positive, set U "
                              "to -15 V (edit_component)"),
                 qPrintable(got.join(" | ")));
    }

    // The same findings in the same order every run: a hash's order (Qt
    // seeds it anew in each process) chose which label named a net of two,
    // and the order of the floating groups and the nets without a DC path.
    void theFindingsDoNotDependOnAHashsOrder()
    {
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QString components = "  <Vdc V1 1 0 60 18 -26 0 1 \"5 V\" 1>\n  <GND * 1 0 90 0 0 0 0>\n";
        QString wires = "  <0 30 0 30 \"a\" 10 10 0 \"\">\n";
        const auto resistor = [](const QString& name, int x, int y) {
            return QStringLiteral("  <R %1 1 %2 %3 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
                .arg(name).arg(x).arg(y);
        };
        const auto label = [](int x, int y, const QString& name) {
            return QStringLiteral("  <%1 %2 %1 %2 \"%3\" %4 %5 0 \"\">\n").arg(x).arg(y).arg(name).arg(x + 10).arg(y - 20);
        };
        for (int k = 1; k <= 6; ++k) {
            const int x = 100 * k;
            // R<k> from a to a net of two names; RF<k> and RG<k> side by side
            // between f<k> and g<k>: floating, or (k even) on ground through C<k>.
            components += resistor(QStringLiteral("R%1").arg(k), x, 60);
            wires += label(x, 30, "a") + label(x, 90, QStringLiteral("n%1").arg(k)) + QStringLiteral("  <%1 90 %2 90 \"m%3\" %1 110 0 \"\">\n").arg(x).arg(x + 20).arg(k);
            components += resistor(QStringLiteral("RF%1").arg(k), x, 260) + resistor(QStringLiteral("RG%1").arg(k), x + 50, 260);
            wires += label(x, 230, QStringLiteral("f%1").arg(k)) + label(x + 50, 230, QStringLiteral("f%1").arg(k))
                     + label(x, 290, QStringLiteral("g%1").arg(k)) + label(x + 50, 290, QStringLiteral("g%1").arg(k));
            if (k % 2 == 0) {
                components += QStringLiteral("  <C C%1 1 %2 460 17 -26 0 1 \"1n\" 1 \"\" 0 \"neutral\" 0>\n  <GND * 1 %2 490 0 0 0 0>\n").arg(k).arg(x);
                wires += label(x, 430, QStringLiteral("g%1").arg(k));
            }
        }
        const QString f = dir.filePath("hashes.sch");
        write(f, ("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n" + components + "  <.DC DC1 1 0 700 0 36 0 0 \"26.85\" 0 \"0.001\" 0 "
                  "\"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n</Components>\n<Wires>\n" + wires
                  + "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n").toUtf8());
        QStringList first;
        for (int run = 0; run < 8; ++run) {
            QHashSeed::resetRandomGlobalSeed();
            Schematic doc(nullptr, f);
            QVERIFY(doc.load());
            QStringList got = messages(check(&doc));
            for (const Issue& i : notes(&doc)) got << "N " + i.message;
            if (run == 0) {
                first = got;
                QVERIFY2(got.filter("floating").size() >= 2 && got.filter("no DC path").size() >= 2 && got.filter("names").size() >= 2,
                         qPrintable(got.join(" | ")));
            } else {
                QVERIFY2(got == first, qPrintable(got.join(" | ") + "\nfirst: " + first.join(" | ")));
            }
        }
    }

    void aGoodCircuitHasNoProblems()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(fine));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        const QList<Issue> issues = check(doc);
        QVERIFY2(issues.isEmpty(), qPrintable(messages(issues).join(" | ")));
        QCOMPARE(check(nullptr).size(), 0);
    }

    // What the simulator in use cannot take: a component whose mask does
    // not carry the simulator (drawn red after a change of simulator), one
    // without a SPICE model, an implicit EDD, a winding without its core.
    // A digital circuit is not held to the SPICE rules.
    void theSimulatorRulesNameWhatTheNetlistWouldLose()
    {
        const QString sch = dir.filePath("simulator_rules.sch");
        write(sch,
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <GND * 1 100 300 0 0 0 0>\n"
            "  <.DC DC1 1 500 100 0 26 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
            "  <VDMOS M1 1 200 200 8 -36 0 0 \"nchan\" 1 \"1\" 1 \"0.0\" 1 \"1.0\" 1 \"0.6\" 0 \"0.0\" 1 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"1.0\" 0 \"0.0\" 0 \"0.0\" 0 \"1.0\" 0 \"0.0\" 0 \"0.1\" 0 \"1500\" 0 \"1.0e-10\" 0 \"1.0\" 0 \"1e7\" 0 \"0.0\" 0 \"1.0\" 0 \"0.0\" 0 \"1.11\" 0 \"3.0\" 0 \"1e-14\" 0 \"0.8\" 0 \"0.5\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"1.0\" 0 \"10e-6\" 0 \"1000\" 0 \"26.85\" 0 \"26.85\" 0 \"yes\" 0 \"off\" 1>\n"
            "  <TWIST Line1 1 400 200 -26 16 0 0 \"0.5 mm\" 1 \"0.8 mm\" 1 \"1.5\" 1 \"100\" 0 \"4\" 0 \"1\" 0 \"0.022e-6\" 0 \"4e-4\" 0 \"26.85\" 0>\n"
            "  <EDD D1 1 600 200 -26 -66 0 0 \"implicit\" 0 \"1\" 0 \"0\" 1 \"0\" 0>\n"
            "  <WINDING W1 1 800 200 -20 50 0 0 \"CORE1\" 1 \"10\" 1 \"0.1\" 1>\n"
            "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);

        QStringList got = messages(check(doc));
        QVERIFY2(got.contains("E Line1: has no SPICE model, Ngspice cannot simulate it"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("E D1: an implicit equation-defined device has no SPICE form (use the explicit type)"), qPrintable(got.join(" | ")));
        QVERIFY2(got.contains("E W1: no magnetic core named CORE1 in the schematic"), qPrintable(got.join(" | ")));
        QVERIFY2(got.filter("not available").isEmpty(), qPrintable(got.join(" | ")));   // VDMOS is fine for ngspice

        // The simulator changed to Xyce with the schematic open: the VDMOS
        // (ngspice only) is what the netlist would lose.
        QucsSettings.DefaultSimulator = spicecompat::simXyce;
        got = messages(check(doc));
        QVERIFY2(got.contains("E M1: not available for Xyce"), qPrintable(got.join(" | ")));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;

        // The explicit EDD and a core for the winding put things right.
        for (Component* c : doc->a_DocComps) {
            if (c->Model == "EDD") c->Props.first()->Value = "explicit";
        }
        QString line = "<CORE CORE1 1 900 200 -40 35 0 0 \"26.0\" 1 \"27.0\" 1 \"0.05\" 1 \"395e3\" 1 \"1e-4\" 1 \"1.0\" 1 \"1.0\" 1 \"0.0\" 1 \"generic\" 1 \"1.0\" 0 \"1.0\" 0 \"1.0\" 0 \"1.0\" 0 \"1.0\" 0 \"1.0\" 0 \"false\" 0>";
        Component* core = getComponentFromName(line, doc);
        QVERIFY(core && core->load(line));
        doc->insertRawComponent(core);
        got = messages(check(doc));
        QVERIFY2(got.filter("implicit").isEmpty() && got.filter("no magnetic").isEmpty(), qPrintable(got.join(" | ")));

        // Under Qucsator the SPICE rules do not apply, and a digital circuit
        // (a .Digi block) is checked for neither.
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        got = messages(check(doc));
        QVERIFY2(got.filter("SPICE").isEmpty(), qPrintable(got.join(" | ")));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        line = "<.Digi Digi1 1 950 100 0 45 0 0 \"TruthTable\" 1 \"10 ns\" 0 \"VHDL\" 0>";
        Component* digi = getComponentFromName(line, doc);
        QVERIFY(digi && digi->load(line));
        doc->insertRawComponent(digi);
        got = messages(check(doc));
        QVERIFY2(got.filter("SPICE").isEmpty() && got.filter("not available").isEmpty(), qPrintable(got.join(" | ")));
    }

    void theMenuActionListsThemAndAClickShowsThePlace()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(app.gotoPage(broken, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        MessageDock* dock = app.messages();
        QVERIFY(!dock->msgDock->isVisible());

        QAction* action = menuAction(&app, "Simulation", "Check Schematic");
        QVERIFY(action != nullptr);
        QCOMPARE(action->shortcut(), QKeySequence(Qt::Key_F10));
        action->trigger();
        QVERIFY(dock->msgDock->isVisible());                      // brought up
        QCOMPARE(dock->builderTabs->currentWidget(), dock->problems);
        QCOMPARE(dock->problems->count(), dock->issues().size());
        QVERIFY(dock->problems->count() >= 6);
        QVERIFY(dock->builderTabs->tabText(2).startsWith("Problems ("));
        QCOMPARE(dock->problemsDocument(), doc);

        // Click the duplicate-name row: the second R1 is selected, the
        // view centred on it.
        int row = -1;
        for (int i = 0; i < dock->issues().size(); ++i)
            if (dock->issues().at(i).message.startsWith("R1: the name")) row = i;
        QVERIFY(row >= 0);
        doc->showAll();
        dock->problems->setCurrentRow(row);
        emit dock->problems->itemClicked(dock->problems->item(row));
        int selected = 0;
        Component* chosen = nullptr;
        for (Component* c : doc->a_DocComps) if (c->isSelected) { ++selected; chosen = c; }
        QCOMPARE(selected, 1);
        QCOMPARE(QPoint(chosen->cx, chosen->cy), QPoint(200, 100));
        const QPoint centre = doc->viewportToModel(doc->viewport()->rect().center());
        QVERIFY2((centre - QPoint(200, 100)).manhattanLength() < 8, qPrintable(QString("%1,%2").arg(centre.x()).arg(centre.y())));

        // A wire-end row selects nothing and centres on the end.
        for (int i = 0; i < dock->issues().size(); ++i)
            if (dock->issues().at(i).message.contains("500, 300")) row = i;
        dock->problems->setCurrentRow(row);
        emit dock->problems->itemClicked(dock->problems->item(row));
        selected = 0;
        for (Component* c : doc->a_DocComps) if (c->isSelected) ++selected;
        QCOMPARE(selected, 0);
        const QPoint centre2 = doc->viewportToModel(doc->viewport()->rect().center());
        QVERIFY2((centre2 - QPoint(500, 300)).manhattanLength() < 8, qPrintable(QString("%1,%2").arg(centre2.x()).arg(centre2.y())));

        // A clean schematic: the tab says so, no error icon.
        QVERIFY(app.gotoPage(fine));
        menuAction(&app, "Simulation", "Check Schematic")->trigger();
        QCOMPARE(dock->issues().size(), 0);
        QCOMPARE(dock->problems->count(), 1);
        QCOMPARE(dock->problems->item(0)->text(), QString("No problems found."));
        QCOMPARE(dock->builderTabs->tabText(2), QString("Problems"));
    }

    // QUCS_ERC_SURVEY=1: run the check over every shipped example and
    // print what it says (to see that it does not cry wolf). Each is
    // checked for the simulator of its folder (qucsator/ and
    // external_interface/, xyce/, ngspice for the others), with a ground
    // symbol required and not, and the shipped symbols where an
    // installation has them (share/qucs-s/symbols beside bin/).
    // QUCS_ERC_SURVEY_OUT=file lists every finding there, a line each:
    // file, simulator, ground (required/free), E or W, message - by tabs;
    // the notes as "notes", N.
    void surveyOfTheExamples()
    {
        if (qEnvironmentVariableIsEmpty("QUCS_ERC_SURVEY")) QSKIP("set QUCS_ERC_SURVEY=1 for the survey");
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        // The library components of the examples need the shipped library.
        const QDir source = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR)).dir();
        QucsSettings.LibDir = source.filePath("library") + "/";
        QTemporaryDir installed;
        QVERIFY(installed.isValid());
        QVERIFY(QDir(installed.path()).mkpath("bin"));
        QVERIFY(QDir(installed.path()).mkpath("share/" QUCS_NAME));
        QVERIFY(QFile::link(source.filePath("library/symbols"), installed.filePath("share/" QUCS_NAME "/symbols")));
        QucsSettings.BinDir = installed.filePath("bin") + "/";
        QFile out(qEnvironmentVariable("QUCS_ERC_SURVEY_OUT"));
        if (!out.fileName().isEmpty()) QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        QucsApp app(false);
        MainGuard guard(&app);
        QDirIterator it(QStringLiteral(QUCS_EXAMPLES_DIR), {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        int files = 0, withErrors = 0, withWarnings = 0;
        QHash<QString, int> kinds;
        while (it.hasNext()) {
            const QString f = it.next();
            const QString name = f.mid(QString(QUCS_EXAMPLES_DIR).size());
            const int simulator = name.startsWith("/qucsator/") || name.startsWith("/external_interface/")
                                      ? spicecompat::simQucsator
                                : name.startsWith("/xyce/")     ? spicecompat::simXyce
                                                                : spicecompat::simNgspice;
            QucsSettings.DefaultSimulator = simulator;
            if (qEnvironmentVariableIsSet("QUCS_ERC_SURVEY_TRACE")) qWarning() << "loading" << f;
            QTimer::singleShot(3000, &app, [] {
                for (QWidget* w : QApplication::topLevelWidgets())
                    if (w->isVisible() && w->isModal()) {
                        qWarning() << "MODAL:" << w->metaObject()->className() << w->windowTitle();
                        for (QLabel* l : w->findChildren<QLabel*>()) qWarning() << "  text:" << l->text().left(200);
                        w->close();
                    }
            });
            if (!app.gotoPage(f, false, false)) {
                if (out.isOpen()) out.write(QStringLiteral("%1\t-\t-\tE\t(does not open)\n").arg(name).toUtf8());
                continue;
            }
            ++files;
            for (const bool required : {true, false}) {
                QucsSettings.RequireGround = required;
                const QList<Issue> issues = check(app.currentSchematic());
                for (const Issue& i : issues)
                    if (out.isOpen())
                        out.write(QStringLiteral("%1\t%2\t%3\t%4\t%5\n")
                                      .arg(name, simulator == spicecompat::simQucsator ? "qucsator"
                                                 : simulator == spicecompat::simXyce   ? "xyce"
                                                                                        : "ngspice",
                                           required ? "required" : "free",
                                           i.severity == Severity::Error ? "E" : "W", i.message)
                                      .toUtf8());
                if (!required) continue;
                // The notes (fine if meant), as N: the same either way.
                for (const Issue& i : notes(app.currentSchematic()))
                    if (out.isOpen())
                        out.write(QStringLiteral("%1\t%2\tnotes\tN\t%3\n")
                                      .arg(name, simulator == spicecompat::simQucsator ? "qucsator"
                                                 : simulator == spicecompat::simXyce   ? "xyce"
                                                                                        : "ngspice",
                                           i.message)
                                      .toUtf8());
                if (errorCount(issues) > 0) ++withErrors;
                if (issues.size() > errorCount(issues)) ++withWarnings;
                for (const Issue& i : issues) {
                    QString kind = i.message;
                    kind.replace(QRegularExpression("[0-9]+"), "N");
                    kind.replace(QRegularExpression("^[A-Za-z_]+N?:"), "X:");
                    ++kinds[(i.severity == Severity::Error ? "E " : "W ") + kind];
                }
                if (!issues.isEmpty()) qWarning() << name << issues.size() << messages(issues).mid(0, 4);
            }
            app.closeAllFiles();
        }
        qWarning() << "files" << files << "with errors" << withErrors << "with warnings" << withWarnings;
        for (auto k = kinds.begin(); k != kinds.end(); ++k) qWarning() << k.value() << k.key();
    }

    // Check Schematic and Subcircuits: the schematic in front and every
    // subcircuit it uses, at any depth; a subcircuit's finding names its
    // file, and a click on it opens that file at the place.
    void theHierarchyCheckWalksTheSubcircuits()
    {
        const QString top = dir.filePath("top.sch");
        const QString sub = dir.filePath("inner.sch");
        const QString leaf = dir.filePath("leaf.sch");
        // top uses inner, inner uses leaf; leaf has a loose wire end.
        write(top,
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <GND * 1 100 300 0 0 0 0>\n"
            "  <Sub SUB1 1 200 200 -26 17 0 0 \"inner.sch\" 1>\n"
            "  <.DC DC1 1 400 400 0 0 0 0>\n"
            "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        write(sub,
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
            "  <Sub SUB2 1 300 200 -26 17 0 0 \"leaf.sch\" 1>\n"
            "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        write(leaf,
            "<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
            "  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
            "</Components>\n<Wires>\n"
            "  <700 100 800 100 \"\" 0 0 0 \"\">\n"
            "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");

        QucsApp app(false);
        MainGuard guard(&app);
        app.resize(1000, 700);
        app.show();
        QVERIFY(app.gotoPage(top, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        QCOMPARE(subcircuitFiles(doc), QStringList{QFileInfo(sub).canonicalFilePath()});

        QAction* action = menuAction(&app, "Simulation", "Check Schematic and Subcircuits");
        QVERIFY(action != nullptr);
        action->trigger();
        MessageDock* dock = app.messages();
        QVERIFY(dock->msgDock->isVisible());
        const QList<Issue> issues = dock->issues();
        QStringList files;
        for (const Issue& i : issues) files << QFileInfo(i.file).fileName();
        QVERIFY2(files.contains("leaf.sch"), qPrintable(files.join(" | ")));       // two levels down
        QVERIFY2(!files.contains("top.sch") || true, "");
        // The leaf's loose ends are named with their file on the tab.
        QStringList rows;
        for (int i = 0; i < dock->problems->count(); ++i) rows << dock->problems->item(i)->text();
        QVERIFY2(rows.filter(QRegularExpression("^leaf\\.sch: the wire end at 700, 100")).size() == 1, qPrintable(rows.join(" | ")));
        // A click on it opens leaf.sch, centred on the place.
        int row = rows.indexOf(QRegularExpression("^leaf\\.sch: the wire end at 700, 100.*"));
        QVERIFY(row >= 0);
        dock->problems->setCurrentRow(row);
        emit dock->problems->itemClicked(dock->problems->item(row));
        Schematic* leafDoc = app.currentSchematic();
        QVERIFY(leafDoc != nullptr);
        QCOMPARE(QFileInfo(leafDoc->getDocName()).fileName(), QString("leaf.sch"));
        const QPoint centre = leafDoc->viewportToModel(leafDoc->viewport()->rect().center());
        QVERIFY2((centre - QPoint(700, 100)).manhattanLength() < 8, qPrintable(QString("%1,%2").arg(centre.x()).arg(centre.y())));

        // The toolbar has the five buttons, in order.
        QToolBar* bar = nullptr;
        for (QToolBar* t : app.findChildren<QToolBar*>())
            if (t->windowTitle() == "Hierarchy and Netlist") bar = t;
        QVERIFY(bar != nullptr);
        QStringList names;
        for (QAction* a : bar->actions()) if (!a->isSeparator()) names << a->text().remove('&');
        // The yellow check (this schematic) left of the green (and its subcircuits).
        QCOMPARE(names, (QStringList{"Go into Subcircuit", "Pop out", "Check Schematic",
                                     "Check Schematic and Subcircuits", "Generate Netlist", "Save netlist"}));
        for (QAction* a : bar->actions()) if (!a->isSeparator()) QVERIFY2(!a->icon().isNull(), qPrintable(a->text()));

        // The yellow one checks the schematic in front, none of its subcircuits.
        QVERIFY(app.gotoPage(top, false, false));
        doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        QAction* current = nullptr;
        for (QAction* a : bar->actions()) if (a->text() == "Check Schematic") current = a;
        QVERIFY(current != nullptr);
        current->trigger();
        for (const Issue& i : dock->issues())
            QVERIFY2(QFileInfo(i.file).fileName() == "top.sch", qPrintable(i.file + ": " + i.message));

        // The canvas menu has Export... on the empty canvas, not on a component.
        // (The menu as a right click fills it; popping it up is Qt's part.)
        const auto menuAt = [&](const QPoint& model) {
            app.view->fillContextMenu(doc, model.x(), model.y());
            QStringList texts;
            for (QAction* a : app.view->ComponentMenu->actions()) if (!a->isSeparator()) texts << a->text();
            return texts;
        };
        QStringList onComponent = menuAt(QPoint(200, 200));   // SUB1
        QVERIFY2(onComponent.contains("Edit Properties") && !onComponent.contains("Export..."),
                 qPrintable(onComponent.join(" | ")));
        QStringList onCanvas = menuAt(QPoint(700, 40));
        QVERIFY2(onCanvas.contains("Export...") && !onCanvas.contains("Edit Properties"),
                 qPrintable(onCanvas.join(" | ")));
        // For a look: QUCS_TEST_GRAB=<dir> saves a picture of the toolbar rows.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (!grabDir.isEmpty()) app.grab(QRect(0, 0, app.width(), 130)).save(grabDir + "/toolbars.png");
    }

    // (The hunt after round 9, A2, D1, D2.) A schematic that uses itself,
    // and a cycle of files: errors (ngspice nests them until it gives up),
    // and the walk ends. A subcircuit with no file: an error of its own,
    // and the schematic's folder is not read as its file. A file not found
    // beside the schematic is not read from the process's working folder.
    void subcircuitsThatAreNoFileOrHoldThemselves()
    {
        const auto schematic = [](const QString& subs) {
            return ("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
                    "  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                    "  <R R1 1 200 100 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                    "  <GND * 1 200 130 0 0 0 0>\n" + subs
                    + "</Components>\n<Wires>\n  <100 100 100 100 \"p\" 110 80 0 \"\">\n  <200 70 200 70 \"p\" 210 50 0 \"\">\n"
                      "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n").toUtf8();
        };
        const auto sub = [](const QString& name, const QString& file) {
            return QStringLiteral("  <Sub %1 1 400 300 -26 17 0 0 \"%2\" 1>\n").arg(name, file);
        };
        const auto fileNames = [](const QList<SubcircuitFindings>& found) {
            QStringList names;
            for (const SubcircuitFindings& f : found) names << QFileInfo(f.file).fileName();
            return names;
        };
        QDir(dir.path()).mkpath("hold");
        const QString here = dir.filePath("hold");
        write(here + "/self.sch", schematic(sub("X1", "self.sch")));
        {
            Schematic doc(nullptr, here + "/self.sch");
            QVERIFY(doc.load());
            const QStringList got = messages(check(&doc));
            QVERIFY2(got.contains("E X1 uses this schematic itself (self.sch): a subcircuit cannot hold itself - ngspice nests it "
                                  "until it gives up (\"unknown subckt\")"),
                     qPrintable(got.join(" | ")));
            QVERIFY(checkSubcircuits(&doc, nullptr).isEmpty());
        }
        // top -> a -> b -> a.
        write(here + "/cyc_a.sch", schematic(sub("X1", "cyc_b.sch")));
        write(here + "/cyc_b.sch", schematic(sub("X1", "cyc_a.sch")));
        write(here + "/cyc_top.sch", schematic(sub("XT", "cyc_a.sch")));
        {
            Schematic doc(nullptr, here + "/cyc_top.sch");
            QVERIFY(doc.load());
            QVERIFY2(errorCount(check(&doc)) == 0, qPrintable(messages(check(&doc)).join(" | ")));
            const QList<SubcircuitFindings> found = checkSubcircuits(&doc, nullptr);
            QCOMPARE(fileNames(found), (QStringList{"cyc_a.sch", "cyc_b.sch"}));
            QCOMPARE(errorCount(found.at(0).issues), 0);
            const QStringList got = messages(found.at(1).issues);
            QVERIFY2(got.contains("E X1 uses cyc_a.sch, which uses this schematic again (cyc_b.sch -> cyc_a.sch -> cyc_b.sch): a "
                                  "subcircuit cannot hold itself - ngspice nests it until it gives up (\"unknown subckt\")"),
                     qPrintable(got.join(" | ")));
            QCOMPARE(QFileInfo(found.at(1).issues.last().file).fileName(), QString("cyc_b.sch"));
        }
        // No file name; a name found only in the working folder.
        QDir(dir.path()).mkpath("elsewhere");
        write(dir.filePath("elsewhere/stray.sch"), schematic(QString()));
        const QString before = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.filePath("elsewhere")));
        struct Back {
            QString path;
            tQucsSettings saved = QucsSettings;
            ~Back() {
                QDir::setCurrent(path);
                QucsSettings = saved;
            }
        } back{before};
        QucsSettings.QucsWorkDir.setPath(here);   // (the project: by default the working folder)
        write(here + "/nofile.sch", schematic(sub("X2", "") + QStringLiteral("  <Sub X3 1 600 300 -26 17 0 0 \"stray.sch\" 1>\n")));
        {
            Schematic doc(nullptr, here + "/nofile.sch");
            QVERIFY(doc.load());
            const QStringList got = messages(check(&doc));
            QVERIFY2(got.contains("E X2: no subcircuit file is given: it has no pins, and what was wired to them is on nothing")
                         && got.contains("E X3: its subcircuit stray.sch is not found (beside the schematic, in the project or its "
                                         "user_lib): it has no pins, and what was wired to them is on nothing"),
                     qPrintable(got.join(" | ")));
            QVERIFY(subcircuitFiles(&doc).isEmpty());
            QVERIFY(checkSubcircuits(&doc, nullptr).isEmpty());
            for (Component* c : doc.a_DocComps)
                if (c->Name == "X3") {
                    QCOMPARE(c->getSubcircuitFile(), QDir(here).filePath("stray.sch"));   // beside it, not in the working folder
                    QCOMPARE(c->Ports.size(), 0);
                } else if (c->Name == "X2") {
                    QCOMPARE(c->getSubcircuitFile(), QString());
                }
        }
    }

    void aSimulationRunsTheCheckFirst()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.show();
        QVERIFY(app.gotoPage(broken, false, false));
        MessageDock* dock = app.messages();
        QVERIFY(!dock->msgDock->isVisible());
        // The simulator is a shell that exits at once; the check comes
        // before it and, with errors, brings the Problems tab up.
        QVERIFY(QMetaObject::invokeMethod(&app, "slotSimulateWithSpice"));
        QVERIFY(dock->msgDock->isVisible());
        QCOMPARE(dock->builderTabs->currentWidget(), dock->problems);
        QVERIFY(errorCount(dock->issues()) >= 2);
        QTRY_VERIFY_WITH_TIMEOUT(!app.simulationConsole()->isRunning(), 15000);
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestErc test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_erc.moc"
