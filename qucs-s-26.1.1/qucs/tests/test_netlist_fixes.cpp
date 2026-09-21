/*
 * The netlists of the components that docs/bug_hunts/2026-09-21-component-
 * netlists.md found wrong, after the fix: one case per finding, checked on
 * the emitted text (and on ngspice's own reading where the finding was
 * about that).
 */
#include <QtTest>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "qucs.h"
#include "module.h"
#include "schematic.h"
#include "node.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The component of a schematic-file line, on \a doc, its nodes named
// n1, n2, ... in pin order.
Component* placed(Schematic* doc, const char* fileLine)
{
    QString line = QString::fromLatin1(fileLine);
    Component* c = getComponentFromName(line, doc);
    if (!c || !c->load(line)) return nullptr;
    c->setSchematic(doc);
    doc->insertRawComponent(c);
    int i = 1;
    for (Port* p : c->Ports) p->Connection->Name = QStringLiteral("n%1").arg(i++);
    return c;
}

QString ngspice(Component* c) { return c->getSpiceNetlist(spicecompat::SPICEDefault); }
QString xyce(Component* c) { return c->getSpiceNetlist(spicecompat::SPICEXyce); }

}   // namespace

class TestNetlistFixes : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    Schematic* doc = nullptr;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.font = QApplication::font();
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.SpiceLibDir = QStringLiteral(QUCS_EXAMPLES_DIR "/../library/spicelibrary/");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        doc = new Schematic(app, "");
        // A component the loader does not know raises a modal box; close
        // it rather than hang.
        QTimer* closer = new QTimer(this);
        connect(closer, &QTimer::timeout, this, [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->isModal() && w->isVisible()) w->close();
        });
        closer->start(200);
    }

    void cleanupTestCase()
    {
        delete doc;
        QucsMain = nullptr;
        delete app;
    }

    // A1: port 1 is the left pair of pins (1, 4), port 2 the right (2, 3).
    void fourTerminalLinePairsThePinsOfASide()
    {
        Component* c = placed(doc, "<TLIN4P Line1 1 200 200 -26 16 0 0 \"50 Ohm\" 1 \"1 mm\" 1 \"0 dB\" 0 \"26.85\" 0>");
        QVERIFY(c);
        QVERIFY2(ngspice(c).startsWith("TLine1 n1 n4 n2 n3 Z0=50 "), qPrintable(ngspice(c)));
    }

    // A2: T1 is the upper winding (pins 1, 6), T2 the lower (pins 5, 4).
    void symmetricTransformerRatiosFollowTheSymbol()
    {
        Component* c = placed(doc, "<sTr Tr1 1 200 200 -29 78 0 0 \"2\" 1 \"3\" 1>");
        QVERIFY(c);
        const QString s = ngspice(c);
        QVERIFY2(s.contains("X_Tr1_W1 n2 n3 n1 n6  XFMR RATIO=2\n"), qPrintable(s));
        QVERIFY2(s.contains("X_Tr1_W2 n2 n3 n5 n4  XFMR RATIO=3\n"), qPrintable(s));
    }

    // A3: K13 carries k13.
    void threeMutualInductorsNetEveryCoupling()
    {
        Component* c = placed(doc, "<MUT2 Tr1 1 200 200 -29 78 0 0 \"1 mH\" 0 \"2 mH\" 0 \"3 mH\" 0 \"0.1\" 0 \"0.2\" 0 \"0.3\" 0>");
        QVERIFY(c);
        const QString s = ngspice(c);
        QVERIFY2(s.contains("K12_Tr1 LTr1_L1 LTr1_L2 0.1\n"), qPrintable(s));
        QVERIFY2(s.contains("K13_Tr1 LTr1_L1 LTr1_L3 0.2\n"), qPrintable(s));
        QVERIFY2(s.contains("K23_Tr1 LTr1_L2 LTr1_L3 0.3\n"), qPrintable(s));
    }

    // A4: the seven TRNOISE parameters are the seven properties, and the
    // current flows with the arrow, toward pin 1.
    void currentNoiseSourceReadsEveryParameter()
    {
        Component* c = placed(doc, "<iTRNOISE I1 1 200 200 44 -26 0 1 \"20n\" 1 \"0.5n\" 1 \"1.1\" 1 \"12p\" 1 \"1u\" 1 \"2u\" 1 \"3u\" 1>");
        QVERIFY(c);
        QCOMPARE(ngspice(c).trimmed(), QString("I1 n2 n1 DC 0 AC 0 TRNOISE(20N 0.5N 1.1 12P 1U  2U 3U)"));
    }

    // A5: Xyce .TRAN <step> <stop> <start>
    void xyceTransientSensitivityOrdersTheTranArguments()
    {
        // The component is registered for Xyce only.
        QucsSettings.DefaultSimulator = spicecompat::simXyce;
        Module::unregisterModules();
        Module::registerModules();
        Component* c = placed(doc, "<.SENS_TR_XYCE SENS1 1 510 240 0 71 0 0 \"v(cap)\" 1 \"R1:R\" 1 \"direct\" 1 \"0\" 1 \"5m\" 1 \"5u\" 1 \"yes\" 1>");
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        Module::unregisterModules();
        Module::registerModules();
        QVERIFY(c);
        QVERIFY2(xyce(c).startsWith(".tran 5U 5M 0\n"), qPrintable(xyce(c)));
    }

    // A6: the pattern repeats (r=0) and returns to the initial level.
    void digitalSourceRepeatsItsPattern()
    {
        Component* c = placed(doc, "<DigiSource S1 1 200 200 -35 16 0 0 \"1\" 1 \"high\" 0 \"1ns; 2ns\" 0 \"1 V\" 0>");
        QVERIFY(c);
        // high for 1 ns, low for 2 ns, changes 10 ps long ending at the
        // nominal time, back to high at 3 ns, period 3 ns.
        QCOMPARE(ngspice(c).trimmed(), QString("VS1 n1 0 DC 1 PWL(0 1 9.9e-10 1 1e-09 0 2.99e-09 0 3e-09 1) r=0"));
        // An odd count ends on the first level, so the wrap-around is no
        // change (qucsator: the level of entry 1 begins each period).
        Component* d = placed(doc, "<DigiSource S2 1 200 300 -35 16 0 0 \"1\" 1 \"low\" 0 \"1ns; 1ns; 1ns\" 0 \"1 V\" 0>");
        QVERIFY(d);
        QCOMPARE(ngspice(d).trimmed(), QString("VS2 n1 0 DC 0 PWL(0 0 9.9e-10 0 1e-09 1 1.99e-09 1 2e-09 0 3e-09 0) r=0"));
        // A single entry is a constant level.
        Component* e = placed(doc, "<DigiSource S3 1 200 400 -35 16 0 0 \"1\" 1 \"high\" 0 \"1ns\" 0 \"1 V\" 0>");
        QVERIFY(e);
        QCOMPARE(ngspice(e).trimmed(), QString("VS3 n1 0 DC 1 PWL(0 1 1e-09 1) r=0"));
    }

    // A7: an even-numbered time list repeats; the change takes at most
    // MaxDuration.
    void timeSwitchRepeatsAnEvenList()
    {
        Component* c = placed(doc, "<Switch S1 1 200 200 -26 11 0 0 \"off\" 0 \"1 ms; 3 ms\" 0 \"1e-9\" 0 \"1e12\" 0 \"26.85\" 0 \"1e-6\" 0 \"spline\" 0 \"SPST\" 1>");
        QVERIFY(c);
        const QString s = ngspice(c);
        QVERIFY2(s.contains("VS1 control_netS1 0 DC 0 PWL(0 0 0.000999 0 0.001 1 0.003999 1 0.004 0) r=0\n"), qPrintable(s));
        // Three entries: on after 1 ms, off after 4 ms, on after 9 ms, and stays.
        Component* d = placed(doc, "<Switch S2 1 200 300 -26 11 0 0 \"off\" 0 \"1 ms; 3 ms; 5 ms\" 0 \"1e-9\" 0 \"1e12\" 0 \"26.85\" 0 \"1e-6\" 0 \"spline\" 0 \"SPST\" 1>");
        QVERIFY(d);
        const QString t = ngspice(d);
        QVERIFY2(t.contains("PWL(0 0 0.000999 0 0.001 1 0.003999 1 0.004 0 0.008999 0 0.009 1)\n"), qPrintable(t));
        QVERIFY2(!t.contains("r=0"), qPrintable(t));
    }

    // A8, B2: no Temp in the VDMOS card; RQ/VQ only when both are set.
    void vdmosCardLeavesTemperatureAndQuasiSaturationAlone()
    {
        Component* c = placed(doc, "<VDMOS M1 1 200 200 8 -36 0 0 \"nchan\" 1 \"1\" 1 \"0.0\" 1 \"1.0\" 1 \"0.6\" 0 \"0.0\" 1 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"1.0\" 0 \"0.0\" 0 \"0.0\" 0 \"1.0\" 0 \"0.0\" 0 \"0.1\" 0 \"1500\" 0 \"1.0e-10\" 0 \"1.0\" 0 \"1e7\" 0 \"0.0\" 0 \"1.0\" 0 \"0.0\" 0 \"1.11\" 0 \"3.0\" 0 \"1e-14\" 0 \"0.8\" 0 \"0.5\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"1.0\" 0 \"10e-6\" 0 \"1000\" 0 \"26.85\" 0 \"26.85\" 0 \"yes\" 0 \"off\" 1>");
        QVERIFY(c);
        QString s = ngspice(c);
        QVERIFY2(!s.contains("Temp="), qPrintable(s));
        QVERIFY2(!s.contains("RQ=") && !s.contains("VQ="), qPrintable(s));
        QVERIFY2(s.contains("Tnom=26.85"), qPrintable(s));
        c->getProperty("RQ")->Value = "0.5";
        c->getProperty("VQ")->Value = "2";
        s = ngspice(c);
        QVERIFY2(s.contains("RQ=0.5 VQ=2 "), qPrintable(s));
        // Not using the global temperature puts TEMP on the instance, still not in the card.
        c->getProperty("UseGlobTemp")->Value = "no";
        c->getProperty("Temp")->Value = "85";
        s = ngspice(c);
        QVERIFY2(s.contains("VDMOS_M1 TEMP=85"), qPrintable(s));
        QVERIFY2(!s.contains("Temp=85"), qPrintable(s));
    }

    // A9, A10: value notation.
    void valuesFollowQucsNotation_data()
    {
        QTest::addColumn<QString>("qucs");
        QTest::addColumn<QString>("spice");
        QTest::newRow("kOhm") << "1 kOhm" << "1K";
        QTest::newRow("MOhm") << "1 MOhm" << "1MEG";
        QTest::newRow("mOhm") << "1 mOhm" << "1M";
        QTest::newRow("bare mega") << "10M" << "10MEG";
        QTest::newRow("bare milli") << "10m" << "10M";
        QTest::newRow("bare kilo") << "4.7k" << "4.7K";
        QTest::newRow("bare micro with exponent") << "1e-3u" << "1E-3U";
        QTest::newRow("centimetre") << "10 cm" << "10E-2";
        QTest::newRow("millimetre") << "1 mm" << "1M";
        QTest::newRow("Meg spelled out") << "1Meg" << "1MEG";
        QTest::newRow("negative mega with unit") << "-1 MOhm" << "-1MEG";
        QTest::newRow("negative volts") << "-2.0 V" << "-2.0";
        QTest::newRow("plain number") << "1e-3" << "1E-3";
        QTest::newRow("variable") << "Rload" << "{RLOAD}";
        QTest::newRow("expression, letter first") << "Rload*2" << "{RLOAD*2}";
        QTest::newRow("expression, digit first") << "2*Rload" << "{2*RLOAD}";
        QTest::newRow("expression, division") << "1e3/f0" << "{1E3/F0}";
        QTest::newRow("expression, sum") << "0.5+x" << "{0.5+X}";
        QTest::newRow("already braced") << "{2*Rload}" << "{2*RLOAD}";
        QTest::newRow("quoted expression") << "'2*Rload'" << "'2*Rload'";
        QTest::newRow("dBm") << "10 dBm" << "10";
        QTest::newRow("time list untouched") << "1 ms; 3 ms" << "1M;3M";
        QTest::newRow("option word") << "yes" << "{YES}";
    }

    void valuesFollowQucsNotation()
    {
        QFETCH(QString, qucs);
        QFETCH(QString, spice);
        QCOMPARE(spicecompat::normalize_value(qucs), spice);
    }

    // B1, D: a relay with Ron = 0 gets a contact resistance SPICE can take;
    // Xyce's vswitch band is Vt-Vh .. Vt+Vh like sw's.
    void relayContactResistanceHasAFloor()
    {
        Component* c = placed(doc, "<Relais S1 1 200 200 51 -26 0 0 \"0.5 V\" 1 \"0.1 V\" 1 \"0\" 1 \"1e12\" 1 \"26.85\" 0 \"SPST\" 1>");
        QVERIFY(c);
        QVERIFY2(ngspice(c).contains("sw vt=0.5 vh=0.1 ron=1e-9 roff=1E12"), qPrintable(ngspice(c)));
        QVERIFY2(xyce(c).contains("vswitch von=0.6 voff=0.4 ron=1e-9 roff=1E12"), qPrintable(xyce(c)));
        c->getProperty("Ron")->Value = "1e-3";
        QVERIFY2(ngspice(c).contains("ron=1E-3 "), qPrintable(ngspice(c)));
    }

    // B3: the Xyce JFET card has no GUI property in it.
    void jfetXyceCardHasOnlyModelParameters()
    {
        Component* c = placed(doc, "<JFET T1 1 200 200 8 -26 0 0 \"nfet\" 1 \"-2.0 V\" 1 \"1e-4\" 1 \"0.0\" 1 \"0.0\" 0 \"0.0\" 0 \"1e-14\" 0 \"1.0\" 0 \"1e-14\" 0 \"2.0\" 0 \"0.0\" 0 \"0.0\" 0 \"1.0\" 0 \"0.5\" 0 \"0.5\" 0 \"0.0\" 0 \"1.0\" 0 \"1.0\" 0 \"26.85\" 0 \"3.0\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"1.0\" 0 \"yes\" 0 \"Generic\" 0 \"Generic\" 0>");
        QVERIFY(c);
        QVERIFY2(!xyce(c).contains("UseGlobTemp"), qPrintable(xyce(c)));
        QVERIFY2(!ngspice(c).contains("UseGlobTemp"), qPrintable(ngspice(c)));
    }

    // B4: XSPICE-based components are not offered to Xyce.
    void xspiceComponentsAreNotForXyce()
    {
        for (const char* line : {"<Vfile V1 1 200 200 18 -26 0 1 \"vfile.dat\" 1 \"linear\" 0 \"no\" 0 \"1\" 0 \"0\" 0>",
                                 "<Ifile I1 1 200 200 18 -26 0 1 \"ifile.dat\" 1 \"linear\" 0 \"no\" 0 \"1\" 0 \"0\" 0>"}) {
            Component* c = placed(doc, line);
            QVERIFY2(c, line);
            QVERIFY2((c->Simulator & spicecompat::simXyce) == 0, line);
            QVERIFY2((c->Simulator & spicecompat::simNgspice) != 0, line);
            QVERIFY2((c->Simulator & spicecompat::simQucsator) != 0, line);
        }
        QucsSettings.DefaultSimulator = spicecompat::simXyce;
        Module::unregisterModules();
        Module::registerModules();
        for (const char* model : {"Vfile", "Ifile", "SDTF", "XAPWL", "XSPICE_A"})
            QVERIFY2(Module::getComponent(model) == nullptr, model);
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        Module::unregisterModules();
        Module::registerModules();
        for (const char* model : {"Vfile", "Ifile", "SDTF", "XAPWL", "XSPICE_A"}) {
            Component* c = Module::getComponent(model);
            QVERIFY2(c != nullptr, model);
            delete c;
        }
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestNetlistFixes test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_netlist_fixes.moc"
