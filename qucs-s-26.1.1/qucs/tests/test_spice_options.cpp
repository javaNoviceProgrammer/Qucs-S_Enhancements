/*
 * The .OPTIONS section component: every option line reaches the netlist,
 * whichever position the hidden Xyce "option package" property has - the
 * equation editor rebuilds the property list from its lines, and the
 * first line used to be dropped (taken for the package).
 */
#include <QtTest>
#include <QCheckBox>
#include <QLabel>
#include <QScopeGuard>
#include <QTabWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QTemporaryDir>
#include <QStandardPaths>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "qucs.h"
#include "module.h"
#include "schematic.h"
#include "spicecomponents/sp_options.h"
#include "components/componentdialog.h"
#include "extsimkernels/spicecompat.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/simsettingsdialog.h"
#include "settings.h"
#include "isolated_settings.h"

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};
}

class TestSpiceOptions : public QObject
{
    Q_OBJECT

    static void setProps(SpiceOptions& c, const QList<QPair<QString, QString>>& props)
    {
        qDeleteAll(c.Props);
        c.Props.clear();
        for (const auto& p : props) c.Props.append(new Property(p.first, p.second, true));
    }

    QTemporaryDir dir;

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
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void asShippedThePackageComesFirst()
    {
        SpiceOptions c;
        QCOMPARE(c.Props.at(0)->Name, QString("XyceOptionPackage"));
        QCOMPARE(c.getExpression(spicecompat::SPICEDefault), QString(".OPTION GMIN = 1e-12\n"));
        QCOMPARE(c.getExpression(spicecompat::SPICEXyce), QString(".OPTIONS DEVICE  GMIN = 1e-12 \n"));
    }

    void everyLineOfTheEditorReachesTheNetlist()
    {
        SpiceOptions c;
        // What the equation editor leaves after "temp = 50" and "autobus = on"
        // were typed in place of the package line.
        setProps(c, {{"temp", "50"}, {"autobus", "on"}});
        QCOMPARE(c.getExpression(spicecompat::SPICEDefault), QString(".OPTION temp = 50\n.OPTION autobus = on\n"));
        QCOMPARE(c.getExpression(spicecompat::SPICEXyce), QString(".OPTIONS DEVICE  temp = 50  autobus = on \n"));

        // The package line kept, anywhere: it names the package and is no option.
        setProps(c, {{"temp", "50"}, {"XyceOptionPackage", "TIMEINT"}, {"reltol", "1e-4"}});
        QCOMPARE(c.getExpression(spicecompat::SPICEDefault), QString(".OPTION temp = 50\n.OPTION reltol = 1e-4\n"));
        QCOMPARE(c.getExpression(spicecompat::SPICEXyce), QString(".OPTIONS TIMEINT  temp = 50  reltol = 1e-4 \n"));

        // Deactivated or for CDL: nothing.
        c.isActive = COMP_IS_OPEN;
        QVERIFY(c.getExpression(spicecompat::SPICEDefault).isEmpty());
        c.isActive = COMP_IS_ACTIVE;
        QVERIFY(c.getExpression(spicecompat::CDL).isEmpty());
    }

    // ngspice has a good many options that are flags with no value at
    // all; one of those is written on its own.
    void anOptionWithoutAValueIsAFlag()
    {
        SpiceOptions c;
        setProps(c, {{"noopiter", ""}, {"reltol", "1e-4"}, {"keepopinfo", ""}});
        QCOMPARE(c.getExpression(spicecompat::SPICEDefault),
                 QString(".OPTION noopiter\n.OPTION reltol = 1e-4\n.OPTION keepopinfo\n"));
        QCOMPARE(c.getExpression(spicecompat::SPICEXyce),
                 QString(".OPTIONS DEVICE  noopiter  reltol = 1e-4  keepopinfo \n"));

        // And it survives the file.
        SpiceOptions back;
        QVERIFY(back.load(c.save()));
        QCOMPARE(back.getExpression(spicecompat::SPICEDefault),
                 QString(".OPTION noopiter\n.OPTION reltol = 1e-4\n.OPTION keepopinfo\n"));
    }

    void aFileSavedWithoutThePackageLineLoadsRight()
    {
        // Saved by the old dialog: the first value is an option, not a package.
        SpiceOptions c;
        QVERIFY(c.load("<SpiceOptions SpiceOptions1 1 400 100 -32 15 0 0 \"temp=50\" 1 \"autobus=on\" 1>"));
        QCOMPARE(c.Props.size(), 3);
        QCOMPARE(c.Props.at(0)->Name, QString("XyceOptionPackage"));
        QCOMPARE(c.Props.at(0)->Value, QString("DEVICE"));
        QCOMPARE(c.Props.at(1)->Name, QString("temp"));
        QCOMPARE(c.Props.at(1)->Value, QString("50"));
        QCOMPARE(c.Props.at(2)->Name, QString("autobus"));
        QCOMPARE(c.getExpression(spicecompat::SPICEDefault), QString(".OPTION temp = 50\n.OPTION autobus = on\n"));
        // A file with the package, as the examples have it, is as before.
        SpiceOptions d;
        QVERIFY(d.load("<SpiceOptions SpiceOptions3 1 140 790 -38 16 0 0 \"TIMEINT\" 0 \"erroption=1\" 1 \"method=gear\" 1>"));
        QCOMPARE(d.Props.at(0)->Value, QString("TIMEINT"));
        QCOMPARE(d.getExpression(spicecompat::SPICEXyce), QString(".OPTIONS TIMEINT  erroption = 1  method = gear \n"));
        QCOMPARE(d.getExpression(spicecompat::SPICEDefault), QString(".OPTION erroption = 1\n.OPTION method = gear\n"));
    }

    // The properties dialog: the package in a field of its own, the
    // options as lines; Apply keeps the package first and every line.
    void theDialogKeepsThePackageAndEveryLine()
    {
        const QString sch = dir.filePath("opts.sch");
        {
            QFile f(sch);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n"
                    "  <SpiceOptions SpiceOptions1 1 400 100 -32 15 0 0 \"DEVICE\" 0 \"GMIN=1e-12\" 1>\n"
                    "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        }
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(sch, false, false));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        Component* comp = nullptr;
        for (Component* c : doc->a_DocComps) if (c->Model == "SpiceOptions") comp = c;
        QVERIFY(comp != nullptr);

        ComponentDialog dlg(comp, doc);
        QLineEdit* package = nullptr;
        for (QLineEdit* e : dlg.findChildren<QLineEdit*>())
            if (e->toolTip().contains("Xyce")) package = e;
        QVERIFY(package != nullptr);
        QCOMPARE(package->text(), QString("DEVICE"));
        QTextEdit* editor = dlg.findChild<QTextEdit*>();
        QVERIFY(editor != nullptr);
        QCOMPARE(editor->toPlainText().trimmed(), QString("GMIN = 1e-12"));   // no package line among the options

        editor->setPlainText("temp = 50\nautobus = on\n");
        package->setText("TIMEINT");
        QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApplyButton"));
        QCOMPARE(comp->Props.size(), 3);
        QCOMPARE(comp->Props.at(0)->Name, QString("XyceOptionPackage"));
        QCOMPARE(comp->Props.at(0)->Value, QString("TIMEINT"));
        QCOMPARE(comp->Props.at(1)->Name, QString("temp"));
        QCOMPARE(comp->Props.at(2)->Name, QString("autobus"));
        QCOMPARE(comp->getExpression(spicecompat::SPICEDefault), QString(".OPTION temp = 50\n.OPTION autobus = on\n"));
        QCOMPARE(comp->getExpression(spicecompat::SPICEXyce), QString(".OPTIONS TIMEINT  temp = 50  autobus = on \n"));
        // Saved with the package first, so it loads back the same.
        QVERIFY(comp->save().contains("\"TIMEINT\" 0 \"temp=50\" 1 \"autobus=on\" 1"));

        // Every shape a SPICE user writes an option in. Each line of the
        // editor is one or more options; up to now a line without "="
        // was dropped without a word, and two options on one line became
        // one with a nonsense value.
        {
            ComponentDialog shapes(comp, doc);
            QTextEdit* ed = shapes.findChild<QTextEdit*>();
            QVERIFY(ed != nullptr);
            ed->setPlainText("temp = 50\n"
                             "noopiter\n"
                             "gmin=1e-10 reltol=1e-4\n"
                             ".option method = gear\n"
                             ".OPTIONS srcsteps=10\n"
                             "* a comment\n");
            QVERIFY(QMetaObject::invokeMethod(&shapes, "slotApplyButton"));

            QStringList got;
            for (Property* p : comp->Props)
                if (p->Name != "XyceOptionPackage")
                    got << (p->Value.isEmpty() ? p->Name : p->Name + "=" + p->Value);
            QCOMPARE(got, (QStringList{"temp=50", "noopiter", "gmin=1e-10", "reltol=1e-4",
                                       "method=gear", "srcsteps=10"}));

            QCOMPARE(comp->getExpression(spicecompat::SPICEDefault),
                     QString(".OPTION temp = 50\n.OPTION noopiter\n.OPTION gmin = 1e-10\n"
                             ".OPTION reltol = 1e-4\n.OPTION method = gear\n.OPTION srcsteps = 10\n"));

            // The editor writes them back one to a line, a flag bare.
            ComponentDialog shown(comp, doc);
            QTextEdit* again = shown.findChild<QTextEdit*>();
            QVERIFY(again != nullptr);
            QCOMPARE(again->toPlainText().trimmed(),
                     QString("temp = 50\nnoopiter\ngmin = 1e-10\nreltol = 1e-4\n"
                             "method = gear\nsrcsteps = 10"));

            // And a round trip through the file changes nothing.
            SpiceOptions reloaded;
            QVERIFY(reloaded.load(comp->save()));
            QCOMPARE(reloaded.getExpression(spicecompat::SPICEDefault),
                     comp->getExpression(spicecompat::SPICEDefault));
        }

        // Back to two plain options for what follows.
        {
            ComponentDialog back(comp, doc);
            QTextEdit* ed = back.findChild<QTextEdit*>();
            QVERIFY(ed != nullptr);
            ed->setPlainText("temp = 50\nautobus = on\n");
            QVERIFY(QMetaObject::invokeMethod(&back, "slotApplyButton"));
        }

        // An empty package field means DEVICE.
        ComponentDialog again(comp, doc);
        for (QLineEdit* e : again.findChildren<QLineEdit*>())
            if (e->toolTip().contains("Xyce")) e->setText("");
        QVERIFY(QMetaObject::invokeMethod(&again, "slotApplyButton"));
        QCOMPARE(comp->Props.at(0)->Value, QString("DEVICE"));
        QCOMPARE(comp->Props.size(), 3);
    }

    // Simulator Settings > Netlist: an ngspice netlist includes the
    // installation's ngspice_mathfunc.inc (limexp, step, stp) while its box
    // is on - the default - and not while it is off; a file the
    // installation has not is left out either way, and the tab says so.
    // The choice is kept with the settings.
    void theMathFunctionsAreIncludedAsChosen()
    {
        // An installation with the file: <root>/bin, <root>/share/qucs-s/...
        const QString root = QFileInfo(dir.filePath("install")).absoluteFilePath();
        const QString inc = root + "/share/" QUCS_NAME "/xspice_cmlib/include/ngspice_mathfunc.inc";
        QVERIFY(QDir().mkpath(root + "/bin"));
        QVERIFY(QDir().mkpath(QFileInfo(inc).absolutePath()));
        {
            QFile f(inc);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(".func stp(x) {u(x)}\n");
        }
        const QString binWas = QucsSettings.BinDir;
        const auto back = qScopeGuard([binWas] {
            QucsSettings.BinDir = binWas;
            QucsSettings.NgspiceMathFuncs = true;
        });
        QucsSettings.BinDir = root + "/bin/";
        QVERIFY(QucsSettings.NgspiceMathFuncs);   // the default
        QVERIFY(_settings::Get().itemDefault<bool>("NgspiceMathFuncs"));

        const QString file = dir.filePath("lowpass.sch");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataSet=lowpass.dat>\n</Properties>\n<Components>\n"
                    "  <Vac V1 1 60 200 18 -26 1 1 \"1 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0>\n"
                    "  <GND * 1 60 260 0 0 0 0>\n"
                    "  <R R1 1 160 140 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                    "  <C C1 1 260 200 17 -26 0 1 \"100n\" 1 \"\" 0 \"neutral\" 0>\n"
                    "  <GND * 1 260 260 0 0 0 0>\n"
                    "  <.AC AC1 1 60 430 0 45 0 0 \"log\" 1 \"100 Hz\" 1 \"100 kHz\" 1 \"7\" 1 \"no\" 0>\n"
                    "</Components>\n<Wires>\n"
                    "  <60 230 60 260 \"\" 0 0 0 \"\">\n  <60 140 60 170 \"\" 0 0 0 \"\">\n"
                    "  <60 140 130 140 \"in\" 70 110 0 \"\">\n  <190 140 260 140 \"out\" 230 110 0 \"\">\n"
                    "  <260 140 260 170 \"\" 0 0 0 \"\">\n  <260 230 260 260 \"\" 0 0 0 \"\">\n"
                    "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        }
        Module::registerModules();   // (a window of a test before took them with it)
        Schematic sch(nullptr, file);
        QVERIFY(sch.loadDocument());
        const auto netlist = [this, &sch] {
            Ngspice kernel(&sch);
            kernel.setWorkdir(dir.filePath("work"));
            QDir().mkpath(dir.filePath("work"));
            kernel.SaveNetlist(dir.filePath("work/net.cir"), false);
            QFile f(dir.filePath("work/net.cir"));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };
        QString net = netlist();
        QVERIFY2(net.contains(".INCLUDE \"" + inc + "\"\n"), qPrintable(net));
        QucsSettings.NgspiceMathFuncs = false;
        net = netlist();
        QVERIFY2(!net.contains("ngspice_mathfunc") && net.contains("R1 "), qPrintable(net));

        // The tab: the box as the setting is, the file it names; applied,
        // the setting follows and is kept.
        QucsSettings.NgspiceMathFuncs = true;
        {
            SimSettingsDialog dialog;
            auto* tabs = dialog.findChild<QTabWidget*>();
            QVERIFY(tabs != nullptr);
            int netlistTab = -1;
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == "Netlist") netlistTab = i;
            QVERIFY(netlistTab >= 0);
            auto* box = dialog.findChild<QCheckBox*>("cbNgspiceMathFuncs");
            QVERIFY(box != nullptr && tabs->widget(netlistTab)->isAncestorOf(box));
            QVERIFY(box->isChecked());
            QCOMPARE(box->text(), QString("Include ngspice_mathfunc.inc (limexp, step, stp)"));
            auto* file = dialog.findChild<QLabel*>("lblNgspiceMathFuncsFile");
            QVERIFY(file != nullptr);
            QCOMPARE(file->toolTip(), QDir::toNativeSeparators(inc));
            box->setChecked(false);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        }
        QVERIFY(!QucsSettings.NgspiceMathFuncs);
        QVERIFY(!_settings::Get().item<bool>("NgspiceMathFuncs"));
        QVERIFY(!netlist().contains("ngspice_mathfunc"));
        QucsSettings.NgspiceMathFuncs = true;   // read back as the next start reads it
        const QString binNow = QucsSettings.BinDir;
        QVERIFY(loadSettings());
        QucsSettings.BinDir = binNow;
        QVERIFY(!QucsSettings.NgspiceMathFuncs);
        {
            SimSettingsDialog dialog;
            QVERIFY(!dialog.findChild<QCheckBox*>("cbNgspiceMathFuncs")->isChecked());
            dialog.findChild<QCheckBox*>("cbNgspiceMathFuncs")->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "slotApply"));
        }
        QVERIFY(QucsSettings.NgspiceMathFuncs && _settings::Get().item<bool>("NgspiceMathFuncs"));

        // Not in the installation: left out though on, and the tab says so.
        QVERIFY(QFile::remove(inc));
        QVERIFY(!netlist().contains("ngspice_mathfunc"));
        SimSettingsDialog dialog;
        QVERIFY2(dialog.findChild<QLabel*>("lblNgspiceMathFuncsFile")->text().startsWith("Not in this installation"),
                 qPrintable(dialog.findChild<QLabel*>("lblNgspiceMathFuncsFile")->text()));
    }

};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestSpiceOptions test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_spice_options.moc"
