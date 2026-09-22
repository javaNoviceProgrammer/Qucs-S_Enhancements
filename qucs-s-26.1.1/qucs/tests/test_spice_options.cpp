/*
 * The .OPTIONS section component: every option line reaches the netlist,
 * whichever position the hidden Xyce "option package" property has - the
 * equation editor rebuilds the property list from its lines, and the
 * first line used to be dropped (taken for the package).
 */
#include <QtTest>
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
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestSpiceOptions test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_spice_options.moc"
