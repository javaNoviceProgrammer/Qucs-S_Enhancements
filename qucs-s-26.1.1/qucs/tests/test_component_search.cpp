/*
 * Finding components and replacing property values (componentsearch.h):
 * the search itself, the find bar under a pane's schematics (Edit > Find)
 * and the Find and Replace dialog (Edit > Replace, F7) over one schematic,
 * the open ones and a whole project.
 */
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "config.h"
#include "qucs.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "wire.h"
#include "wirelabel.h"
#include "findbar.h"
#include "componentsearch.h"
#include "dialogs/findreplacedialog.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::search;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// R1 and R2 at 10k joined by a wire labelled Vout; R10 at 4.7k with a
// label "bias" on its lower pin; C1 at 10 pF; a source.
const char* const kCircuit =
    "<Qucs Schematic 26.1.2>\n"
    "<Properties>\n"
    "  <View=0,0,800,600,1,0,0>\n"
    "  <Grid=10,10,1>\n"
    "</Properties>\n"
    "<Symbol>\n"
    "</Symbol>\n"
    "<Components>\n"
    "  <R R1 1 100 100 13 -26 0 1 \"10k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
    "  <R R2 1 200 100 13 -26 0 1 \"10k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
    "  <R R10 1 300 100 13 -26 0 1 \"4.7k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
    "  <C C1 1 400 100 17 -26 0 1 \"10 pF\" 1 \"\" 0 \"neutral\" 0>\n"
    "  <Vdc V1 1 500 100 18 -26 0 1 \"5 V\" 1>\n"
    "</Components>\n"
    "<Wires>\n"
    "  <100 70 200 70 \"Vout\" 150 40 50 \"\">\n"
    "  <300 130 300 130 \"bias\" 320 150 0 \"\">\n"
    "</Wires>\n"
    "<Diagrams>\n"
    "</Diagrams>\n"
    "<Paintings>\n"
    "</Paintings>\n";

void write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(bytes);
}

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList describe(const QList<Match>& matches)
{
    QStringList out;
    for (const Match& m : matches) {
        const char* kind = m.kind == Match::Name ? "name" : m.kind == Match::Label ? "label" : "value";
        out << QStringLiteral("%1 %2%3").arg(kind, m.component, m.property.isEmpty() ? QString() : "." + m.property);
    }
    return out;
}

Component* component(Schematic* doc, const QString& name)
{
    for (Component* c : doc->a_DocComps)
        if (c->Name == name) return c;
    return nullptr;
}

QString value(Schematic* doc, const QString& name, const QString& property)
{
    Component* c = component(doc, name);
    Property* p = c ? c->getProperty(property) : nullptr;
    return p ? p->Value : QStringLiteral("<none>");
}

QStringList selected(Schematic* doc)
{
    QStringList out;
    for (Component* c : doc->a_DocComps)
        if (c->isSelected) out << c->Name;
    for (Wire* w : doc->a_DocWires)
        if (w->isSelected) out << "wire";
    return out;
}

Query names(const QString& text)
{
    Query q;
    q.text = text;
    q.names = true;
    q.values = false;
    return q;
}

Query values(const QString& text)
{
    Query q;
    q.text = text;
    return q;
}

} // namespace

class TestComponentSearch : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString circuit;

    std::unique_ptr<Schematic> loaded()
    {
        std::unique_ptr<Schematic> doc(new Schematic(nullptr, circuit));
        return doc->load() ? std::move(doc) : nullptr;
    }

    QTreeWidgetItem* row(FindReplaceDialog* dlg, const QString& componentName)
    {
        for (int i = 0; i < dlg->results()->topLevelItemCount(); ++i)
            if (dlg->results()->topLevelItem(i)->text(1) == componentName) return dlg->results()->topLevelItem(i);
        return nullptr;
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
        circuit = dir.filePath("circuit.sch");
        write(circuit, kCircuit);
    }

    // --- the search ------------------------------------------------------

    void aNameIsFoundExactMatchFirst()
    {
        auto doc = loaded();
        QVERIFY(doc);
        QCOMPARE(describe(find(doc.get(), names("R1"))), QStringList({"name R1", "name R10"}));
        QCOMPARE(describe(find(doc.get(), names("r1"))), QStringList({"name R1", "name R10"}));   // any case
        Query exact = names("r1");
        exact.matchCase = true;
        QVERIFY(find(doc.get(), exact).isEmpty());
        Query whole = names("R1");
        whole.wholeValue = true;
        QCOMPARE(describe(find(doc.get(), whole)), QStringList({"name R1"}));
        // Exact before the rest, whatever the document order.
        QCOMPARE(describe(find(doc.get(), names("R10"))).first(), QStringLiteral("name R10"));
        QVERIFY(find(doc.get(), names("")).isEmpty());   // nothing to look for
    }

    void aNetIsFoundByItsLabel()
    {
        auto doc = loaded();
        QVERIFY(doc);
        QCOMPARE(describe(find(doc.get(), names("out"))), QStringList({"label Vout"}));
        QCOMPARE(describe(find(doc.get(), names("bias"))), QStringList({"label bias"}));   // a node's label

        // Shown: the label and a wire of its net, so the net lights up.
        const QList<Match> vout = find(doc.get(), names("Vout"));
        reveal(doc.get(), vout.first());
        QCOMPARE(selected(doc.get()), QStringList({"wire"}));
        QVERIFY(!doc->selectedNet().empty());
        bool labelSelected = false;
        for (Wire* w : doc->a_DocWires)
            if (w->hasLabel() && w->label()->Name == "Vout") labelSelected = w->label()->isSelected;
        QVERIFY(labelSelected);

        // A search for some type of component or property has no labels.
        Query typed = names("out");
        typed.model = "R";
        QVERIFY(find(doc.get(), typed).isEmpty());
    }

    void valuesAreFoundNarrowedAsAsked()
    {
        auto doc = loaded();
        QVERIFY(doc);
        QCOMPARE(describe(find(doc.get(), values("10k"))), QStringList({"value R1.R", "value R2.R"}));
        QCOMPARE(describe(find(doc.get(), values("10"))), QStringList({"value R1.R", "value R2.R", "value C1.C"}));

        Query typed = values("10");
        typed.model = "C";
        QCOMPARE(describe(find(doc.get(), typed)), QStringList({"value C1.C"}));

        Query named = values("k");
        named.namePattern = "R?";   // R1 and R2, not R10
        QCOMPARE(describe(find(doc.get(), named)), QStringList({"value R1.R", "value R2.R"}));

        Query property = values("26.85");
        property.property = "Temp";
        QCOMPARE(describe(find(doc.get(), property)), QStringList({"value R1.Temp", "value R2.Temp", "value R10.Temp"}));

        Query regex = values("^\\d+k$");
        regex.regex = true;
        QCOMPARE(describe(find(doc.get(), regex)), QStringList({"value R1.R", "value R2.R"}));

        // No text: every value of the property.
        Query any;
        any.property = "R";
        QCOMPARE(find(doc.get(), any).size(), 3);

        Query broken = values("(");
        broken.regex = true;
        QString error;
        QVERIFY(!broken.isValid(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(find(doc.get(), broken).isEmpty());

        // Nothing in symbol editing, where the components are not on show.
        doc->setSymbolMode(true);
        QVERIFY(find(doc.get(), values("10k")).isEmpty());
        doc->setSymbolMode(false);
    }

    void aReplacementFollowsTheQuery()
    {
        QCOMPARE(replaced("10k", values("10k"), "22k"), QStringLiteral("22k"));
        QCOMPARE(replaced("10 pF", values("pF"), "nF"), QStringLiteral("10 nF"));
        QCOMPARE(replaced("10 PF", values("pf"), "nF"), QStringLiteral("10 nF"));   // any case
        Query exact = values("pf");
        exact.matchCase = true;
        QCOMPARE(replaced("10 PF", exact, "nF"), QStringLiteral("10 PF"));
        Query whole = values("10");
        whole.wholeValue = true;
        QCOMPARE(replaced("10k", whole, "x"), QStringLiteral("10k"));
        QCOMPARE(replaced("10", whole, "x"), QStringLiteral("x"));
        Query regex = values("(\\d+)k");
        regex.regex = true;
        QCOMPARE(replaced("10k", regex, "\\1 kOhm"), QStringLiteral("10 kOhm"));
        QCOMPARE(replaced("anything", Query(), "set"), QStringLiteral("set"));   // no text: the whole value
    }

    void replacingChangesTheValuesInOneUndoStep()
    {
        auto doc = loaded();
        QVERIFY(doc);
        const Query q = values("10k");
        int skipped = -1;
        QCOMPARE(replace(doc.get(), find(doc.get(), q), q, "22k", &skipped), 2);
        QCOMPARE(skipped, 0);
        QCOMPARE(value(doc.get(), "R1", "R"), QStringLiteral("22k"));
        QCOMPARE(value(doc.get(), "R2", "R"), QStringLiteral("22k"));
        QCOMPARE(value(doc.get(), "R10", "R"), QStringLiteral("4.7k"));
        doc->setChanged(true, true);
        QVERIFY(doc->undo());
        QCOMPARE(value(doc.get(), "R1", "R"), QStringLiteral("10k"));
        QCOMPARE(value(doc.get(), "R2", "R"), QStringLiteral("10k"));
    }

    void aValueChangedSinceOrNotAllowedIsLeftAlone()
    {
        auto doc = loaded();
        QVERIFY(doc);
        const Query q = values("10k");
        const QList<Match> found = find(doc.get(), q);
        component(doc.get(), "R1")->getProperty("R")->Value = "15k";   // changed after the search
        int skipped = 0;
        QCOMPARE(replace(doc.get(), found, q, "22k", &skipped), 1);
        QCOMPARE(skipped, 1);
        QCOMPARE(value(doc.get(), "R1", "R"), QStringLiteral("15k"));
        QCOMPARE(value(doc.get(), "R2", "R"), QStringLiteral("22k"));

        // A double quote would end the value in the file; nothing is no value.
        const QList<Match> again = find(doc.get(), values("22k"));
        QCOMPARE(replace(doc.get(), again, values("22k"), "2\"2k", &skipped), 0);
        QCOMPARE(skipped, 1);
        QCOMPARE(replace(doc.get(), again, values("22k"), "", &skipped), 0);
        QCOMPARE(skipped, 1);
        QCOMPARE(value(doc.get(), "R2", "R"), QStringLiteral("22k"));
    }

    // --- the find bar ----------------------------------------------------

    void theFindBarStepsThroughTheMatches()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(circuit));
        Schematic* doc = app.currentSchematic();
        QVERIFY(doc != nullptr);
        QVERIFY(app.editFind->isEnabled());
        app.editFind->trigger();
        FindBar* bar = app.findBarOf(app.DocumentTab);
        QVERIFY(bar != nullptr);
        QVERIFY(!bar->isHidden());
        QVERIFY(bar->edit()->isEnabled());

        bar->edit()->setText("R1");   // as typed: the first match at once
        QCOMPARE(bar->matches().size(), 2);
        QCOMPARE(bar->countLabel()->text(), QStringLiteral("1 of 2"));
        QCOMPARE(selected(doc), QStringList({"R1"}));

        QTest::keyClick(bar->edit(), Qt::Key_Return);
        QCOMPARE(selected(doc), QStringList({"R10"}));
        QCOMPARE(bar->countLabel()->text(), QStringLiteral("2 of 2"));
        QTest::keyClick(bar->edit(), Qt::Key_Return);   // round again
        QCOMPARE(selected(doc), QStringList({"R1"}));
        QTest::keyClick(bar->edit(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(selected(doc), QStringList({"R10"}));

        // Names first, then values; one stop per component.
        bar->edit()->setText("10");
        QCOMPARE(describe(bar->matches()), QStringList({"name R10", "value R1.R", "value R2.R", "value C1.C"}));
        QCOMPARE(selected(doc), QStringList({"R10"}));
        bar->edit()->setText("4.7");   // R10 by its value
        QCOMPARE(describe(bar->matches()), QStringList({"value R10.R"}));
        bar->edit()->setText("R10");   // named and valued alike: once
        QCOMPARE(bar->matches().size(), 1);

        // A net label lights its net.
        bar->edit()->setText("Vout");
        QCOMPARE(bar->matches().size(), 1);
        QVERIFY(!doc->selectedNet().empty());

        bar->edit()->setText("nothing like it");
        QCOMPARE(bar->countLabel()->text(), QStringLiteral("No matches"));

        QTest::keyClick(bar->edit(), Qt::Key_Escape);
        QVERIFY(bar->isHidden());
    }

    void theFindBarFollowsThePane()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(circuit));
        app.editFind->trigger();
        FindBar* bar = app.findBarOf(app.DocumentTab);
        bar->edit()->setText("R2");
        QCOMPARE(bar->matches().size(), 1);

        // A text document in front: nothing to search, the bar says so.
        const QString text = dir.filePath("notes.txt");
        write(text, "R2 is not a component here\n");
        QVERIFY(app.gotoPage(text));
        QVERIFY(!bar->edit()->isEnabled());
        QCOMPARE(bar->countLabel()->text(), QStringLiteral("Not a schematic"));
        QVERIFY(bar->matches().isEmpty());

        // Back to the schematic: its matches again.
        QVERIFY(app.gotoPage(circuit));
        QVERIFY(bar->edit()->isEnabled());
        QCOMPARE(bar->matches().size(), 1);
        app.closeAllFiles();
    }

    // --- the dialog ------------------------------------------------------

    void theDialogReplacesTheCheckedValues()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(circuit));
        Schematic* doc = app.currentSchematic();
        app.changeProps->trigger();
        FindReplaceDialog* dlg = app.findReplaceDialog();
        QVERIFY(dlg != nullptr);
        QVERIFY(!dlg->isHidden());
        QCOMPARE(dlg->scope(), FindReplaceDialog::ThisSchematic);
        QVERIFY(dlg->typeCombo()->findText("R") >= 0);   // the types of the schematic
        QVERIFY(dlg->typeCombo()->findText("C") >= 0);

        dlg->findEdit()->setText("10k");
        dlg->replaceEdit()->setText("22k");
        QVERIFY(!dlg->replaceButton()->isEnabled());   // nothing found yet
        dlg->search();
        QCOMPARE(dlg->results()->topLevelItemCount(), 2);
        QCOMPARE(row(dlg, "R1")->text(5), QStringLiteral("22k"));   // the preview
        QVERIFY(dlg->replaceButton()->isEnabled());

        row(dlg, "R2")->setCheckState(0, Qt::Unchecked);
        dlg->replaceChecked();
        QCOMPARE(value(doc, "R1", "R"), QStringLiteral("22k"));
        QCOMPARE(value(doc, "R2", "R"), QStringLiteral("10k"));   // unchecked
        QVERIFY(doc->getDocChanged());
        QVERIFY2(dlg->statusLabel()->text().startsWith("Replaced 1 value in 1 schematic."), qPrintable(dlg->statusLabel()->text()));
        QCOMPARE(dlg->results()->topLevelItemCount(), 1);   // searched again: R2 is left

        QVERIFY(doc->undo());   // one step
        QCOMPARE(value(doc, "R1", "R"), QStringLiteral("10k"));

        // Changed fields make the hits stale until the next search.
        dlg->search();
        QVERIFY(dlg->replaceButton()->isEnabled());
        dlg->findEdit()->setText("4.7k");
        QVERIFY(!dlg->replaceButton()->isEnabled());

        // A regular expression that does not compile says so.
        dlg->regexBox()->setChecked(true);
        dlg->findEdit()->setText("(");
        dlg->search();
        QCOMPARE(dlg->results()->topLevelItemCount(), 0);
        QVERIFY(dlg->statusLabel()->text().contains("not valid"));
        dlg->regexBox()->setChecked(false);
        dlg->close();
        app.closeAllFiles();
    }

    // What the dialog's predecessor did: one property of a kind of
    // component set to a value, whatever it was.
    void thePropertyOfATypeIsSetWhateverItsValue()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.gotoPage(circuit));
        Schematic* doc = app.currentSchematic();
        app.changeProps->trigger();
        FindReplaceDialog* dlg = app.findReplaceDialog();
        dlg->findEdit()->clear();
        dlg->search();   // no text and no property: refused
        QCOMPARE(dlg->results()->topLevelItemCount(), 0);
        QVERIFY(dlg->statusLabel()->text().contains("which property"));

        dlg->typeCombo()->setCurrentText("R");
        dlg->propertyCombo()->setCurrentText("Temp");
        dlg->replaceEdit()->setText("-273.15");
        dlg->search();
        QCOMPARE(dlg->results()->topLevelItemCount(), 3);
        dlg->replaceChecked();
        for (const char* r : {"R1", "R2", "R10"}) QCOMPARE(value(doc, r, "Temp"), QStringLiteral("-273.15"));
        QCOMPARE(value(doc, "R1", "R"), QStringLiteral("10k"));
        doc->setChanged(false);
        dlg->close();
        app.closeAllFiles();
    }

    // The project's schematics: an open one as it is, the others from disk
    // - opened when something is replaced in them, and left unsaved -
    // and nothing from Scratch.
    void theWholeProjectIsSearchedAndReplaced()
    {
        const QString project = dir.filePath("search_prj");
        write(project + "/top.sch", kCircuit);
        write(project + "/sub/amp.sch", kCircuit);
        write(project + "/Scratch/top/copy.sch", kCircuit);

        QucsApp app(false);
        MainGuard guard(&app);
        app.openProject(project);
        QCOMPARE(app.ProjName, QStringLiteral("search"));
        QVERIFY(app.gotoPage(project + "/top.sch"));
        app.changeProps->trigger();
        FindReplaceDialog* dlg = app.findReplaceDialog();
        dlg->setScope(FindReplaceDialog::ProjectSchematics);
        dlg->typeCombo()->setCurrentIndex(0);
        dlg->propertyCombo()->setCurrentIndex(0);
        dlg->findEdit()->setText("4.7k");
        dlg->replaceEdit()->setText("4.8k");
        dlg->search();
        QStringList files;
        for (int i = 0; i < dlg->results()->topLevelItemCount(); ++i) files << dlg->results()->topLevelItem(i)->text(0);
        QCOMPARE(files, QStringList({"top.sch", "sub/amp.sch"}));

        const QByteArray onDisk = read(project + "/sub/amp.sch");
        dlg->replaceChecked();
        QVERIFY2(dlg->statusLabel()->text().contains("1 schematic was opened for this"), qPrintable(dlg->statusLabel()->text()));
        auto* top = qobject_cast<Schematic*>(QucsApp::documentWidget(app.findDoc(project + "/top.sch")));
        auto* sub = qobject_cast<Schematic*>(QucsApp::documentWidget(app.findDoc(project + "/sub/amp.sch")));
        QVERIFY(top != nullptr && sub != nullptr);   // amp.sch opened for the change
        QCOMPARE(value(top, "R10", "R"), QStringLiteral("4.8k"));
        QCOMPARE(value(sub, "R10", "R"), QStringLiteral("4.8k"));
        QVERIFY(sub->getDocChanged());
        QCOMPARE(read(project + "/sub/amp.sch"), onDisk);   // not saved behind the user's back
        QCOMPARE(read(project + "/Scratch/top/copy.sch"), QByteArray(kCircuit));
        QCOMPARE(dlg->results()->topLevelItemCount(), 0);   // nothing left to find

        // A double click shows the component in its schematic.
        QVERIFY(app.gotoPage(project + "/top.sch"));
        dlg->findEdit()->setText("4.8k");
        dlg->search();
        QCOMPARE(dlg->results()->topLevelItemCount(), 2);
        emit dlg->results()->itemDoubleClicked(dlg->results()->topLevelItem(1), 0);
        QCOMPARE(app.currentSchematic(), sub);
        QCOMPARE(selected(sub), QStringList({"R10"}));

        top->setChanged(false);
        sub->setChanged(false);
        dlg->close();
        app.closeAllFiles();
    }
};

QTEST_MAIN(TestComponentSearch)
#include "test_component_search.moc"
