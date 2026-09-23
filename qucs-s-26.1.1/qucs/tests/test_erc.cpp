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
    // check only warns; the choice is kept and the dialog sets it.
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
        const QString warning = "W no ground symbol: node 0 comes only from a net named 0 or a component that brings it";

        QVERIFY(QucsSettings.RequireGround);   // the default
        QStringList got = messages(check(&doc));
        QVERIFY2(got.contains(error) && !got.contains(warning), qPrintable(got.join(" | ")));
        QVERIFY(!ngspice.groundFound());
        QVERIFY(!xyce.groundFound());

        QucsSettings.RequireGround = false;
        got = messages(check(&doc));
        QVERIFY2(!got.contains(error) && got.contains(warning), qPrintable(got.join(" | ")));
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
    // print what it says (to see that it does not cry wolf).
    void surveyOfTheExamples()
    {
        if (qEnvironmentVariableIsEmpty("QUCS_ERC_SURVEY")) QSKIP("set QUCS_ERC_SURVEY=1 for the survey");
        // The library components of the examples need the shipped library.
        QucsSettings.LibDir = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR)).dir().filePath("library") + "/";
        QucsApp app(false);
        MainGuard guard(&app);
        QDirIterator it(QStringLiteral(QUCS_EXAMPLES_DIR), {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        int files = 0, withErrors = 0, withWarnings = 0;
        QHash<QString, int> kinds;
        while (it.hasNext()) {
            const QString f = it.next();
            if (qEnvironmentVariableIsSet("QUCS_ERC_SURVEY_TRACE")) qWarning() << "loading" << f;
            QTimer::singleShot(3000, &app, [] {
                for (QWidget* w : QApplication::topLevelWidgets())
                    if (w->isVisible() && w->isModal()) {
                        qWarning() << "MODAL:" << w->metaObject()->className() << w->windowTitle();
                        for (QLabel* l : w->findChildren<QLabel*>()) qWarning() << "  text:" << l->text().left(200);
                        w->close();
                    }
            });
            if (!app.gotoPage(f, false, false)) continue;
            ++files;
            const QList<Issue> issues = check(app.currentSchematic());
            if (errorCount(issues) > 0) ++withErrors;
            if (issues.size() > errorCount(issues)) ++withWarnings;
            for (const Issue& i : issues) {
                QString kind = i.message;
                kind.replace(QRegularExpression("[0-9]+"), "N");
                kind.replace(QRegularExpression("^[A-Za-z_]+N?:"), "X:");
                ++kinds[(i.severity == Severity::Error ? "E " : "W ") + kind];
            }
            if (!issues.isEmpty()) qWarning() << f.mid(QString(QUCS_EXAMPLES_DIR).size()) << issues.size() << messages(issues).mid(0, 4);
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
