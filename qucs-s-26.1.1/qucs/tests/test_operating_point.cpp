/*
 * The operating point of every device after a DC bias run (upstream #789):
 * ngspice's "show all" read (oppoint.h) - device types, names, models,
 * parameters, subcircuits, OSDI devices -, each device given to its
 * component, units, the component's tooltip, the Operating Point tab of
 * the message dock (filter, copy, a click locates), and a real DC bias
 * run of two shipped examples when ngspice is installed.
 */
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QHelpEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolTip>
#include <QTreeWidget>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "messagedock.h"
#include "simulationconsole.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "oppoint.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::oppoint;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// What ngspice 46 wrote for a circuit with a BJT, a diode, three level-1
// MOSFETs (one inside the subcircuit X1), resistors, sources and an OSDI
// device - abridged where the rows repeat.
const char* kShow = R"( BJT: Bipolar Junction Transistor
     device                    q1
      model                    qn
         ic           0.000924882
         ib           9.24882e-06
        vbe              0.653098
         gm             0.0357582
        gmu                 1e-12
        cpi           1.56464e-12
        cmu                     0

 Diode: Junction Diode model
     device                    d1
      model                  dmod
    thermal                     0
         vd              0.675066
         id            0.00216247
         gd             0.0836062
         cd           3.32359e-12

 Mos1: Level 1 MOSfet model with Meyer capacitance model
     device               m.x1.m9                   mt2                   mt1
      model                    nm                    nm                    nm
         id           0.000627468               0.00032               0.00032
        vgs                     5                   1.5                   1.5
        vds               1.86266                   1.8                     5
      vdsat                   4.3                   0.8                   0.8
         gm           0.000186266                0.0008                0.0008
        gds           0.000243734                     0                     0

 Resistor: Simple linear resistor
     device                    r1
      model                     R
 resistance                  1000
     bv_max                 1e+99
      noisy                     1
          i           0.000924873
          p            0.00085539

 Vsource: Independent voltage source
     device                   vg                    v1
         dc                   1.5                     5
      pulse                     -                     -
          i                     0           -0.00436406

 savecuroff: A simulator independent device loaded with OSDI
     device                    n1
      model                 scoff
       temp                    27
   _mfactor                     1
        i_p                0.0005
        i_n               -0.0005
)";

const Device* find(const QList<Device>& devices, const QString& name)
{
    for (const Device& d : devices)
        if (d.name == name) return &d;
    return nullptr;
}

double param(const Device* d, const QString& name)
{
    for (const Parameter& p : d->parameters)
        if (p.name == name) return p.value;
    return qQNaN();
}

QStringList names(const Device* d)
{
    QStringList n;
    for (const Parameter& p : d->parameters) n << p.name;
    return n;
}

} // namespace

class TestOperatingPoint : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        // Where a simulation outside a project works.
        QucsSettings.S4Qworkdir = dir.filePath("work");
        QucsSettings.tempFilesDir.setPath(dir.filePath("work"));
        QVERIFY(QDir().mkpath(dir.filePath("work")));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void showAllIsRead()
    {
        const QList<Device> devices = parseShow(QString::fromUtf8(kShow));
        QCOMPARE(devices.size(), 9);

        const Device* q1 = find(devices, "q1");
        QVERIFY(q1 != nullptr);
        QCOMPARE(q1->type, QStringLiteral("BJT"));
        QCOMPARE(q1->description, QStringLiteral("Bipolar Junction Transistor"));
        QCOMPARE(q1->model, QStringLiteral("qn"));
        QCOMPARE(names(q1), QStringList({"ic", "ib", "vbe", "gm", "gmu", "cpi", "cmu"}));
        QCOMPARE(param(q1, "ic"), 0.000924882);

        // Three devices a block, each its own column.
        const Device* inner = find(devices, "m.x1.m9");
        const Device* t1 = find(devices, "mt1");
        QVERIFY(inner != nullptr && t1 != nullptr);
        QCOMPARE(inner->type, QStringLiteral("Mos1"));
        QCOMPARE(param(inner, "id"), 0.000627468);
        QCOMPARE(param(t1, "vds"), 5.0);
        QCOMPARE(param(find(devices, "mt2"), "vds"), 1.8);

        // "-" is no number; both sources are read.
        QCOMPARE(names(find(devices, "v1")), QStringList({"dc", "i"}));
        QCOMPARE(param(find(devices, "v1"), "i"), -0.00436406);
        QVERIFY(find(devices, "vg")->model.isEmpty());

        // An OSDI device: its module is the type.
        const Device* n1 = find(devices, "n1");
        QCOMPARE(n1->type, QStringLiteral("savecuroff"));
        QCOMPARE(param(n1, "i_p"), 0.0005);

        // Nothing, or noise, reads as nothing.
        QVERIFY(parseShow(QString()).isEmpty());
        QVERIFY(parseShow("Error: no such vector\n  gm 1 2\n").isEmpty());
        // A row with a value missing is not taken for the block's.
        const QList<Device> ragged = parseShow(" Diode: d\n device d1 d2\n id 1 2\n gd 3\n");
        QCOMPARE(names(find(ragged, "d2")), QStringList({"id"}));
        // Windows line ends.
        QCOMPARE(parseShow(QString::fromUtf8(kShow).replace("\n", "\r\n")).size(), 9);
    }

    // ngspice's instance of a component: its name, or the SPICE letter and
    // its name; inside a subcircuit "<letter>.<instance>.<name>".
    void eachDeviceIsGivenToItsComponent()
    {
        QList<Device> devices = parseShow(QString::fromUtf8(kShow));
        attribute(devices, {"Q1", "D1", "T1", "T2", "X1", "R1", "V1", "VG", "N1", "R2"});
        QCOMPARE(find(devices, "q1")->component, QStringLiteral("Q1"));    // the name itself
        QCOMPARE(find(devices, "mt1")->component, QStringLiteral("T1"));   // M + T1
        QCOMPARE(find(devices, "mt2")->component, QStringLiteral("T2"));
        QCOMPARE(find(devices, "m.x1.m9")->component, QStringLiteral("X1"));
        QCOMPARE(find(devices, "m.x1.m9")->inside, QStringLiteral("m9"));
        QVERIFY(find(devices, "mt1")->inside.isEmpty());
        QCOMPARE(find(devices, "vg")->component, QStringLiteral("VG"));

        // A subcircuit SUB1 is XSUB1; nested ones keep their path; a
        // device no component owns has none.
        QList<Device> more = parseShow(" Mos1: m\n device m.xsub1.xin.m3 q.xsub1.q7 mzz\n id 1 2 3\n");
        attribute(more, {"SUB1", "Pr1"});
        QCOMPARE(find(more, "m.xsub1.xin.m3")->component, QStringLiteral("SUB1"));
        QCOMPARE(find(more, "m.xsub1.xin.m3")->inside, QStringLiteral("xin.m3"));
        QCOMPARE(find(more, "q.xsub1.q7")->inside, QStringLiteral("q7"));
        QVERIFY(find(more, "mzz")->component.isEmpty());
        QList<Device> probe = parseShow(" Vsource: v\n device vpr1\n i 1\n");
        attribute(probe, {"Pr1"});
        QCOMPARE(probe.first().component, QStringLiteral("Pr1"));   // an ammeter is a V source
    }

    void units_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("parameter");
        QTest::addColumn<QString>("unit");
        QTest::addColumn<bool>("operating");
        QTest::newRow("current") << "BJT" << "ic" << "A" << true;
        QTest::newRow("OSDI terminal current") << "savecuroff" << "i_p" << "A" << true;
        QTest::newRow("voltage") << "BSIM4" << "vdsat" << "V" << true;
        QTest::newRow("threshold") << "BSIM4" << "vth" << "V" << true;
        QTest::newRow("transconductance") << "Mos1" << "gm" << "S" << true;
        QTest::newRow("capacitance") << "Diode" << "cd" << "F" << true;
        QTest::newRow("BSIM4 capacitance") << "BSIM4" << "capbd" << "F" << true;
        QTest::newRow("charge") << "BSIM4" << "qinv" << "C" << true;
        QTest::newRow("power") << "Resistor" << "p" << "W" << true;
        QTest::newRow("resistance") << "Resistor" << "resistance" << "Ohm" << false;
        QTest::newRow("series resistance") << "Mos1" << "rd" << "Ohm" << false;
        QTest::newRow("V source's value") << "Vsource" << "dc" << "V" << true;
        QTest::newRow("I source's value") << "Isource" << "dc" << "A" << true;
        QTest::newRow("width") << "BSIM4" << "w" << "m" << false;
        QTest::newRow("perimeter is no power") << "BSIM4" << "pd" << "" << false;
        QTest::newRow("a flag") << "BSIM4" << "rgatemod" << "" << false;
        QTest::newRow("a multiplier") << "BSIM4" << "mult_i" << "" << false;
        QTest::newRow("an initial condition") << "BSIM4" << "icvgs" << "V" << false;
        QTest::newRow("a count") << "BSIM4" << "nf" << "" << false;
        QTest::newRow("temperature") << "savecuroff" << "temp" << "°C" << false;
    }

    void units()
    {
        QFETCH(QString, type);
        QFETCH(QString, parameter);
        QFETCH(QString, unit);
        QFETCH(bool, operating);
        QCOMPARE(unitOf(type, parameter), unit);
        QCOMPARE(isOperatingQuantity(type, parameter), operating);
    }

    void valuesAreWritten()
    {
        const Device d{"q1", "BJT", "", "qn", {}, "Q1", ""};
        QCOMPARE(valueText(d, {"ic", 0.000924882}), QStringLiteral("0.924882 mA"));   // as the DC bias labels
        QCOMPARE(valueText(d, {"gm", 0.0357582}), QStringLiteral("35.7582 mS"));
        QCOMPARE(valueText(d, {"vbe", 0.653098}), QStringLiteral("0.653098 V"));
        QCOMPARE(valueText(d, {"ib", 9.24882e-06}), QStringLiteral("9.24882 uA"));
        QCOMPARE(valueText(d, {"cpi", 1.56464e-12}), QStringLiteral("1.56464 pF"));
        const Device r{"r1", "Resistor", "", "R", {}, "R1", ""};
        QCOMPARE(valueText(r, {"bv_max", 1e99}), QStringLiteral("1e+99"));   // not set
        QCOMPARE(valueText(r, {"noisy", 1}), QStringLiteral("1"));
    }

    // The tooltip: the component, its type and model, what is flowing and
    // across it - no zeros, no set-up - and a limit.
    void theTooltip()
    {
        QList<Device> devices = parseShow(QString::fromUtf8(kShow));
        attribute(devices, {"Q1", "X1", "R1"});
        const QString q1 = tooltip({find(devices, "q1")});
        QVERIFY2(q1.contains("<b>Q1</b>") && q1.contains("BJT") && q1.contains("qn"), qPrintable(q1));
        QVERIFY(q1.contains("0.924882 mA") && q1.contains("35.7582 mS") && q1.contains("1.56464 pF"));
        QVERIFY(!q1.contains("<td>cmu&nbsp;"));   // zero
        QVERIFY(q1.contains("<td>gm&nbsp;"));
        const QString r1 = tooltip({find(devices, "r1")});
        QVERIFY2(r1.contains("<td>i&nbsp;") && r1.contains("<td>p&nbsp;"), qPrintable(r1));
        QVERIFY(!r1.contains("resistance") && !r1.contains("bv_max") && !r1.contains("noisy"));
        // Inside a subcircuit: the name there.
        QVERIFY(tooltip({find(devices, "m.x1.m9")}).contains("<b>m9</b>"));
        // At most so many rows, and a word on the rest.
        const QString cut = tooltip({find(devices, "q1"), find(devices, "d1")}, 4);
        QVERIFY2(cut.contains("more on the Operating Point tab"), qPrintable(cut));
        QVERIFY(tooltip({}).isEmpty());
        const Device off{"mt1", "Mos1", "", "nm", {{"id", 1.3e-134}, {"von", 0.7}}, "T1", ""};
        QVERIFY(!tooltip({&off}).contains("<td>id&nbsp;"));
        QVERIFY(tooltip({&off}).contains("<td>von&nbsp;"));
    }

    // The schematic gives a component's tooltip while the DC bias is shown,
    // and the tab lists, filters, copies and locates.
    void theSchematicAndTheTab()
    {
        // Under its own name: another would ask to correct the dataset's.
        QVERIFY(QDir(dir.path()).mkpath("tab"));
        const QString sch = dir.filePath("tab/2N3904_follower.sch");
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/General Electronics/2N3904_follower.sch"), sch));
        QFile::setPermissions(sch, QFile::ReadOwner | QFile::WriteOwner);
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        Component* bjt = nullptr;
        for (Component* c : doc->a_DocComps)
            if (c->Name == "Q2N3904_1") bjt = c;
        QVERIFY(bjt != nullptr);

        QList<Device> devices = parseShow(" BJT: b\n device q2n3904_1\n model qmod\n ic 0.001\n gm 0.04\n\n"
                                          " Resistor: r\n device r1\n i 0.002\n bv_max 1e99\n p 0.004\n\n"
                                          " Capacitor: c\n device c3\n i 0\n");
        QStringList components;
        for (Component* c : doc->a_DocComps) components << c->Name;
        attribute(devices, components);
        doc->setOperatingPoint(devices);
        QCOMPARE(doc->operatingPointOf("Q2N3904_1").size(), 1);

        // Over the transistor, while the DC bias is shown.
        doc->resize(800, 600);
        doc->show();
        doc->centerOn(QPoint(bjt->cx, bjt->cy));
        const QPoint over = doc->modelToViewport(QPoint(bjt->cx, bjt->cy));
        QVERIFY(doc->operatingPointTooltip(over).isEmpty());   // no DC bias shown
        doc->setShowBias(1);
        QVERIFY2(doc->operatingPointTooltip(over).contains("<b>Q2N3904_1</b>"),
                 qPrintable(doc->operatingPointTooltip(over)));
        QVERIFY(doc->operatingPointTooltip(doc->modelToViewport(QPoint(bjt->cx + 2000, bjt->cy + 2000))).isEmpty());
        doc->setChanged(true);   // an edit: the bias is gone, and the tooltip
        QVERIFY(doc->operatingPointTooltip(over).isEmpty());
        doc->setChanged(false);
        doc->setShowBias(1);

        // The tab: a row a component, a row a parameter.
        MessageDock* dock = app.messages();
        QVERIFY(dock != nullptr);
        dock->showOperatingPoint(doc, true);
        QTreeWidget* tree = dock->operatingPoint;
        // The transistor first, then the circuit elements (C3 before R1).
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Q2N3904_1"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("C3"));
        QCOMPARE(tree->topLevelItem(2)->text(0), QStringLiteral("R1"));
        QCOMPARE(tree->topLevelItem(2)->childCount(), 2);   // not bv_max's "not set"
        QCOMPARE(tree->topLevelItem(0)->text(1), QStringLiteral("BJT, qmod"));
        QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
        QCOMPARE(tree->topLevelItem(0)->child(1)->text(1), QStringLiteral("40 mS"));
        QVERIFY(dock->builderTabs->tabText(dock->builderTabs->currentIndex()).startsWith("Operating Point (3)"));

        // "gm": the transistor's gm only.
        dock->operatingPointFilter->setText("gm");
        QVERIFY(!tree->topLevelItem(0)->isHidden());
        QVERIFY(tree->topLevelItem(0)->isExpanded());
        QVERIFY(tree->topLevelItem(0)->child(0)->isHidden());    // ic
        QVERIFY(!tree->topLevelItem(0)->child(1)->isHidden());   // gm
        QVERIFY(tree->topLevelItem(1)->isHidden() && tree->topLevelItem(2)->isHidden());   // C3, R1
        const QString copied = dock->operatingPointText();
        QCOMPARE(copied, QStringLiteral("component\tdevice\tparameter\tvalue\tunit\nQ2N3904_1\tBJT\tgm\t0.04\tS\n"));
        auto* copy = dock->builderTabs->findChild<QPushButton*>("operatingPointCopy");
        QVERIFY(copy != nullptr);
        copy->click();
        QCOMPARE(QApplication::clipboard()->text(), copied);
        // "R1": all of it.
        dock->operatingPointFilter->setText("r1");
        QVERIFY(tree->topLevelItem(0)->isHidden());
        QVERIFY(!tree->topLevelItem(2)->child(0)->isHidden() && !tree->topLevelItem(2)->child(1)->isHidden());
        dock->operatingPointFilter->clear();

        // A click selects and centres the component.
        doc->deselectElements(nullptr);
        emit tree->itemClicked(tree->topLevelItem(0)->child(1), 0);
        QVERIFY(bjt->isSelected);

        // Another schematic's run: that one's; the document gone: nothing.
        dock->showOperatingPoint(nullptr, false);
        QCOMPARE(tree->topLevelItemCount(), 1);   // the note
        QVERIFY(!(tree->topLevelItem(0)->flags() & Qt::ItemIsSelectable));
        doc->setChanged(false);
        app.closeAllFiles();
    }

    // Calculate DC bias with the real ngspice: the devices of two shipped
    // examples, each on its component, on the tab and in the tooltip.
    void aRealDcBiasRun_data()
    {
        QTest::addColumn<QString>("example");
        QTest::addColumn<QString>("component");
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("parameter");
        QTest::newRow("BJT") << "2N3904_follower.sch" << "Q2N3904_1" << "BJT" << "gm";
        // Its sources are 0 V at DC: T1 is off, and the tooltip says so by
        // what it leaves out (no 1e-134 A), its threshold still there.
        QTest::newRow("MOSFET") << "chargepump.sch" << "T1" << "Mos1" << "von";
    }

    void aRealDcBiasRun()
    {
        QFETCH(QString, example);
        QFETCH(QString, component);
        QFETCH(QString, type);
        QFETCH(QString, parameter);
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("ngspice is not installed");
        QucsSettings.NgspiceExecutable = ngspice;

        const QString sch = dir.filePath(example);
        QFile::remove(sch);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/General Electronics/") + example, sch));
        QFile::setPermissions(sch, QFile::ReadOwner | QFile::WriteOwner);
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch));
        Schematic* doc = app.currentSchematic();
        QVERIFY(QMetaObject::invokeMethod(&app, "slotDCbias"));
        QTRY_VERIFY_WITH_TIMEOUT(!app.simulationConsole()->isRunning(), 60000);
        QTRY_VERIFY(doc->getShowBias() > 0);

        const QList<const Device*> mine = doc->operatingPointOf(component);
        QVERIFY2(mine.size() == 1, qPrintable(QStringLiteral("%1 devices for %2 of %3")
                                                  .arg(mine.size()).arg(component).arg(doc->operatingPoint().size())));
        QCOMPARE(mine.first()->type, type);
        QVERIFY(names(mine.first()).contains(parameter));
        // Every source and resistor of the schematic is someone's.
        for (const Device& d : doc->operatingPoint())
            QVERIFY2(!d.component.isEmpty(), qPrintable(d.name));

        MessageDock* dock = app.messages();
        QCOMPARE(dock->operatingPointDocument(), doc);
        bool listed = false;
        for (int i = 0; i < dock->operatingPoint->topLevelItemCount(); ++i)
            listed = listed || dock->operatingPoint->topLevelItem(i)->text(0) == component;
        QVERIFY(listed);

        Component* c = nullptr;
        for (Component* each : doc->a_DocComps)
            if (each->Name == component) c = each;
        doc->resize(800, 600);
        doc->show();
        doc->centerOn(QPoint(c->cx, c->cy));
        const QString tip = doc->operatingPointTooltip(doc->modelToViewport(QPoint(c->cx, c->cy)));
        QVERIFY2(tip.contains(QStringLiteral("<b>%1</b>").arg(component)) && tip.contains(parameter), qPrintable(tip));
        QVERIFY2(!tip.contains("e-1"), qPrintable(tip));   // nothing zero but for rounding
        doc->setChanged(false);
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestOperatingPoint)
#include "test_operating_point.moc"
