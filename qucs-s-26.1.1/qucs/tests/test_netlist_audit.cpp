/*
 * Every built-in component (all modules Module::registerModules() lists),
 * placed on a headless schematic with named nodes: its netlist in every
 * flavour must be produced without a crash, and must survive the two
 * round trips a user puts it through - a save and a load of the schematic,
 * and an Apply of the properties dialog without a change.
 *
 * QUCS_NETLIST_AUDIT=<dir> also writes the surveys (every component, its
 * properties and every netlist flavour; every property marked; one ngspice
 * deck per component) for reading by eye and for scripts/netlist-audit.sh;
 * that is where docs/bug_hunts/2026-09-21-component-netlists.md comes from.
 * QUCS_NETLIST_AUDIT_TRACE=1 names each component before it is placed.
 */
#include <QtTest>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QRegularExpression>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "qucs.h"
#include "module.h"
#include "schematic.h"
#include "node.h"
#include "components/component.h"
#include "components/componentdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

struct Flavours {
    QString ngspice, xyce, cdl, qucsator, model, expressionNg, expressionXyce, beforeSim, afterSim, library,
        equations, extraVars, probeVar, vaVars, vaExprs;
    bool operator==(const Flavours& o) const
    {
        return ngspice == o.ngspice && xyce == o.xyce && cdl == o.cdl && qucsator == o.qucsator && model == o.model
            && expressionNg == o.expressionNg && expressionXyce == o.expressionXyce && beforeSim == o.beforeSim
            && afterSim == o.afterSim && library == o.library && equations == o.equations
            && extraVars == o.extraVars && probeVar == o.probeVar && vaVars == o.vaVars && vaExprs == o.vaExprs;
    }
    QString text() const
    {
        QString s;
        auto add = [&](const char* title, const QString& v) {
            if (v.isEmpty() || v == "\n") return;
            s += QStringLiteral("--- %1\n%2").arg(title, v);
            if (!v.endsWith('\n')) s += '\n';
        };
        add("ngspice", ngspice);
        add("xyce", xyce);
        add("cdl", cdl);
        add("qucsator", qucsator);
        add("modelcard", model);
        add("expression ngspice", expressionNg);
        add("expression xyce", expressionXyce);
        add("before sim", beforeSim);
        add("after sim", afterSim);
        add("library", library);
        add("equations", equations);
        add("extra vars", extraVars);
        add("probe var", probeVar);
        add("va vars", vaVars);
        add("va exprs", vaExprs);
        return s;
    }
};

// Nodes of a component fresh on the schematic are nameless; give each
// port's node a name the way the netlister does, so lines can be read.
void nameNodes(Component* c)
{
    int i = 1;
    for (Port* p : c->Ports)
        if (p->Connection && p->Connection->Name.isEmpty())
            p->Connection->Name = QStringLiteral("n%1").arg(i++);
        else if (p->Connection)
            ++i;
}

Flavours flavours(Component* c)
{
    Flavours f;
    nameNodes(c);
    f.ngspice = c->getSpiceNetlist(spicecompat::SPICEDefault);
    f.xyce = c->getSpiceNetlist(spicecompat::SPICEXyce);
    f.cdl = c->getSpiceNetlist(spicecompat::CDL);
    f.qucsator = c->getNetlist();
    f.model = c->getSpiceModel();
    f.expressionNg = c->getExpression(spicecompat::SPICEDefault);
    f.expressionXyce = c->getExpression(spicecompat::SPICEXyce);
    f.beforeSim = c->getNgspiceBeforeSim("tran1");
    f.afterSim = c->getNgspiceAfterSim("tran1");
    f.library = c->getSpiceLibrary();
    QStringList deps;
    f.equations = c->getEquations("tran1", deps);
    f.extraVars = c->getExtraVariables().join(' ');
    f.probeVar = c->getProbeVariable(spicecompat::SPICEDefault);
    f.vaVars = c->getVAvariables();
    f.vaExprs = c->getVAExpressions();
    return f;
}

QString propsText(const Component* c)
{
    QString s;
    for (const Property* p : c->Props)
        s += QStringLiteral("  %1 = %2%3\n").arg(p->Name, p->Value, p->display ? "" : "   (hidden)");
    return s;
}

QString propsLine(const Component* c)
{
    QStringList l;
    for (const Property* p : c->Props) l << p->Name + "=" + p->Value + (p->display ? "" : "~");
    return l.join(" | ");
}

struct Entry {
    QString category, name, model;
    Module* module = nullptr;
};

QList<Entry> allEntries()
{
    QList<Entry> out;
    for (const QString& cat : Category::getCategories()) {
        for (Module* m : Category::getModules(cat)) {
            if (!m->info) continue;
            Entry e;
            e.category = cat;
            e.module = m;
            char* file = nullptr;
            (*m->info)(e.name, file, false);
            out << e;
        }
    }
    return out;
}

// A fresh component of \a e on \a doc, its nodes named, or nullptr for a
// module that makes no component (diagrams, paintings).
Component* place(const Entry& e, Schematic* doc)
{
    static const bool trace = qEnvironmentVariableIsSet("QUCS_NETLIST_AUDIT_TRACE");
    if (trace) fprintf(stderr, "== %s / %s\n", qPrintable(e.category), qPrintable(e.name));
    QString name;
    char* file = nullptr;
    Element* el = (*e.module->info)(name, file, true);
    if (!el) return nullptr;
    if ((el->Type & isComponent) == 0) { delete el; return nullptr; }   // a diagram or a painting
    Component* c = static_cast<Component*>(el);
    c->setSchematic(doc);
    c->cx = 200; c->cy = 200;
    doc->insertComponent(c);
    nameNodes(c);
    return c;
}

QStringList optionsOf(const QString& description)
{
    const int start = description.indexOf('[');
    const int end = description.indexOf(']');
    QStringList options;
    if (start != -1 && end != -1)
        for (const QString& o : description.mid(start + 1, end - start - 1).split(',')) options << o.trimmed();
    return options;
}

}   // namespace

#define QVERIFY_RETURN(cond, ret) do { if (!QTest::qVerify(static_cast<bool>(cond), #cond, "", __FILE__, __LINE__)) return ret; } while (false)

class TestNetlistAudit : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QList<Entry> entries;

    // The dialog Apply round trip for every component under \a simulator;
    // returns the components whose properties or netlist changed.
    QStringList dialogRoundTrip(int simulator, QucsApp& app, QString& report)
    {
        QucsSettings.DefaultSimulator = simulator;
        QStringList changed;
        for (const Entry& e : entries) {
            Schematic* doc = new Schematic(&app, "");
            Component* c = place(e, doc);
            if (!c) { delete doc; continue; }
            const QString before = propsLine(c);
            const Flavours fb = flavours(c);
            {
                ComponentDialog dlg(c, doc);
                QVERIFY_RETURN(QMetaObject::invokeMethod(&dlg, "slotApplyButton"), changed);
            }
            const QString after = propsLine(c);
            const Flavours fa = flavours(c);
            if (before != after || !(fb == fa)) {
                changed << QStringLiteral("%1 / %2 [%3]").arg(e.category, e.name, c->Model);
                report += QStringLiteral("### %1 / %2 [%3] simulator %4\n  before: %5\n  after:  %6\n")
                              .arg(e.category, e.name, c->Model).arg(simulator).arg(before, after);
                if (!(fb == fa)) report += "  netlist before:\n" + fb.text() + "  netlist after:\n" + fa.text();
            }
            delete doc;
        }
        return changed;
    }


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
        QucsSettings.tempFilesDir = QDir(dir.path());   // the optimization block writes its ASCO config there
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        // Modal boxes some components raise headless (a missing file, ...)
        // must not hang the run.
        QTimer* closer = new QTimer(this);
        connect(closer, &QTimer::timeout, this, [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->isModal() && w->isVisible()) w->close();
        });
        closer->start(200);
    }

    // The survey: every flavour of every component, and no crash. The
    // set of components depends on the simulator (registration filters
    // by Component::Simulator), so once per simulator.
    void everyComponentNetlists_data()
    {
        QTest::addColumn<int>("simulator");
        QTest::newRow("ngspice") << int(spicecompat::simNgspice);
        QTest::newRow("xyce") << int(spicecompat::simXyce);
        QTest::newRow("qucsator") << int(spicecompat::simQucsator);
    }

    void everyComponentNetlists()
    {
        QFETCH(int, simulator);
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettings.DefaultSimulator = simulator;
        Module::unregisterModules();
        Module::registerModules();   // the categories of this simulator
        entries = allEntries();
        QVERIFY(entries.size() > 150);
        QString survey;
        int components = 0;
        for (const Entry& e : entries) {
            Schematic* doc = new Schematic(&app, "");
            Component* c = place(e, doc);
            if (!c) { delete doc; continue; }
            ++components;
            const Flavours f = flavours(c);
            survey += QStringLiteral("### %1 / %2  [Model=%3 SpiceModel=%4 ports=%5%6%7%8 sims=%9]\n")
                          .arg(e.category, e.name, c->Model, c->SpiceModel).arg(c->Ports.size())
                          .arg(c->isSimulation ? " simulation" : "", c->isEquation ? " equation" : "",
                               c->isProbe ? " probe" : "").arg(c->Simulator);
            survey += "  file: " + c->save() + "\n" + propsText(c) + f.text() + "\n";
            delete doc;
        }
        QVERIFY2(components > 100, qPrintable(QString::number(components)));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        const QString out = qEnvironmentVariable("QUCS_NETLIST_AUDIT");
        if (!out.isEmpty()) {
            QFile f(QDir(out).filePath(QStringLiteral("netlist_audit_%1.txt").arg(QTest::currentDataTag())));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(survey.toUtf8());
        }
    }


    // The survey again with every property set to a value of its own
    // (property i gets 10+i; a choice property gets another of its
    // choices), so that a property the netlist ignores, and one it reads
    // twice in place of a neighbour, both show. Written for reading;
    // nothing is asserted.
    void everyPropertyMarked()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        entries = allEntries();
        QString survey;
        for (const Entry& e : entries) {
            Schematic* doc = new Schematic(&app, "");
            Component* c = place(e, doc);
            if (!c) { delete doc; continue; }
            QStringList markers;
            int i = 0;
            for (Property* p : c->Props) {
                ++i;
                const QStringList options = optionsOf(p->Description);
                if (p->Name == "File" || p->Name == "Symbol" || p->Name == "Sim" || p->Name == "Simulation") continue;
                if (!options.isEmpty()) {
                    // another choice than the current one
                    for (const QString& o : options) if (o != p->Value) { p->Value = o; break; }
                    continue;
                }
                p->Value = QString::number(10 + i);
                markers << p->Name;
            }
            doc->recreateComponent(c);   // symbols that depend on a property, with nodes again
            nameNodes(c);
            const Flavours f = flavours(c);
            const QString all = f.ngspice + f.xyce + f.model + f.expressionNg + f.expressionXyce + f.beforeSim
                + f.afterSim + f.equations + f.library;
            QString ignored, repeated;
            for (int k = 0; k < c->Props.size(); ++k) {
                const Property* p = c->Props.at(k);
                if (!markers.contains(p->Name)) continue;
                const QRegularExpression re(QStringLiteral("(?<![0-9.])%1(?![0-9])").arg(p->Value));
                const int n = all.count(re);
                if (n == 0) ignored += " " + p->Name;
                if (n > 1) repeated += QStringLiteral(" %1(x%2)").arg(p->Name).arg(n);
            }
            survey += QStringLiteral("### %1 / %2  [Model=%3]\n").arg(e.category, e.name, c->Model);
            if (!ignored.isEmpty()) survey += "  ignored by the SPICE netlist:" + ignored + "\n";
            if (!repeated.isEmpty()) survey += "  used more than once:" + repeated + "\n";
            survey += propsText(c) + f.text() + "\n";
            delete doc;
        }
        const QString out = qEnvironmentVariable("QUCS_NETLIST_AUDIT");
        if (!out.isEmpty()) {
            QFile f(QDir(out).filePath("marker_audit.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write(survey.toUtf8());
        }
    }


    // One ngspice deck per component into <dir>/cir, every node loaded
    // with a resistor to ground, for scripts/netlist-audit.sh to run
    // through the real simulator. Nothing is asserted here.
    void everyComponentAsADeck()
    {
        const QString out = qEnvironmentVariable("QUCS_NETLIST_AUDIT");
        if (out.isEmpty()) QSKIP("QUCS_NETLIST_AUDIT is not set");
        QDir(out).mkdir("cir");
        QucsApp app(false);
        MainGuard guard(&app);
        entries = allEntries();
        int n = 0;
        for (const Entry& e : entries) {
            Schematic* doc = new Schematic(&app, "");
            Component* c = place(e, doc);
            if (!c) { delete doc; continue; }
            const Flavours f = flavours(c);
            if (f.ngspice.trimmed().isEmpty() && f.expressionNg.trimmed().isEmpty()) { delete doc; continue; }
            QString deck = QStringLiteral("* %1 / %2 [%3]\n").arg(e.category, e.name, c->Model);
            deck += f.library + f.expressionNg + f.model + f.ngspice;
            if (!deck.endsWith('\n')) deck += '\n';
            const bool digital = e.category.contains("digital");
            if (!digital)
                for (int i = 1; i <= c->Ports.size(); ++i)
                    deck += QStringLiteral("R_t%1 n%1 0 1k\n").arg(i);
            deck += ".op\n.tran 1u 100u\n.ac dec 5 1 1G\n.end\n";
            QFile file(QDir(out).filePath(QStringLiteral("cir/%1_%2.cir").arg(++n, 3, 10, QLatin1Char('0'))
                                          .arg(QString(c->Model).replace(QRegularExpression("[^A-Za-z0-9]"), "_"))));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
            file.write(deck.toUtf8());
            delete doc;
        }
    }

    // Saved into a schematic file and loaded back: the same properties and
    // the same netlist.
    void savedAndLoadedComponentsNetlistTheSame()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        entries = allEntries();
        QStringList changed;
        QString report;
        for (const Entry& e : entries) {
            Schematic* doc = new Schematic(&app, "");
            Component* c = place(e, doc);
            if (!c) { delete doc; continue; }
            const QString before = propsLine(c);
            const Flavours fb = flavours(c);
            QString line = c->save();
            Component* d = getComponentFromName(line, doc);
            QVERIFY2(d != nullptr, qPrintable(line));
            QVERIFY2(d->load(line), qPrintable(line));
            d->setSchematic(doc);
            doc->insertRawComponent(d);   // keeps the name (the original is still on the sheet)
            const QString after = propsLine(d);
            const Flavours fa = flavours(d);
            if (before != after || !(fb == fa)) {
                changed << QStringLiteral("%1 / %2 [%3]").arg(e.category, e.name, c->Model);
                report += QStringLiteral("### %1 / %2 [%3]\n  line:   %4\n  before: %5\n  after:  %6\n")
                              .arg(e.category, e.name, c->Model, line, before, after);
                if (!(fb == fa)) report += "  netlist before:\n" + fb.text() + "  netlist after:\n" + fa.text();
            }
            delete doc;
        }
        const QString out = qEnvironmentVariable("QUCS_NETLIST_AUDIT");
        if (!out.isEmpty()) {
            QFile f(QDir(out).filePath("file_roundtrip.txt"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(report.toUtf8());
        }
        QVERIFY2(changed.isEmpty(), qPrintable(changed.join("\n")));
    }

    // The properties dialog opened and applied without a change, under
    // each simulator: the same properties and the same netlist.
    void theDialogAppliedUnchangedNetlistsTheSame_data()
    {
        QTest::addColumn<int>("simulator");
        QTest::newRow("ngspice") << int(spicecompat::simNgspice);
        QTest::newRow("xyce") << int(spicecompat::simXyce);
        QTest::newRow("qucsator") << int(spicecompat::simQucsator);
    }

    void theDialogAppliedUnchangedNetlistsTheSame()
    {
        QFETCH(int, simulator);
        QucsApp app(false);
        MainGuard guard(&app);
        entries = allEntries();
        QString report;
        const QStringList changed = dialogRoundTrip(simulator, app, report);
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        const QString out = qEnvironmentVariable("QUCS_NETLIST_AUDIT");
        if (!out.isEmpty()) {
            QFile f(QDir(out).filePath(QStringLiteral("dialog_roundtrip_%1.txt").arg(simulator)));
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(report.toUtf8());
        }
        QVERIFY2(changed.isEmpty(), qPrintable(changed.join("\n")));
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestNetlistAudit test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_netlist_audit.moc"
