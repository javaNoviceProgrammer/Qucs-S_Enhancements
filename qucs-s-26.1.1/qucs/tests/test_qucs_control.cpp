/*
 * Claude's tools for the Qucs-S window (qucscontrol.h), used on the
 * application itself: documents opened, shown, saved and closed; a
 * schematic built part by part - components, wires, labels, changes, one
 * step to undo each - read back as a summary and as text, replaced from
 * text (and left alone when the text does not read, without a message
 * box); a picture of it; the menus' actions, a dialog one opens read,
 * filled in and closed; a simulation waited for.
 */
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "components/component.h"
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "wire.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

} // namespace

class TestQucsControl : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 30000)
    {
        return control->callNow(tool, args, timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonValue json(const QJsonObject& r)
    {
        const QJsonDocument d = QJsonDocument::fromJson(text(r).toUtf8());
        return d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object());
    }
    Schematic* front() const { return app->currentSchematic(); }
    static QJsonObject componentIn(const QJsonObject& summary, const QString& name)
    {
        for (const QJsonValue& v : summary.value("components").toArray())
            if (v.toObject().value("name").toString() == name) return v.toObject();
        return {};
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
        QucsSettings.XyceExecutable = "xyce";
        QucsSettings.SpiceOpusExecutable = "spiceopus";
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
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
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    // The tools as MCP lists them, those that only look among them, and
    // what the others do in words.
    void theToolsAreListed()
    {
        const QJsonArray tools = control->tools();
        QVERIFY(tools.size() >= 20);
        QStringList names;
        for (const QJsonValue& v : tools) {
            const QJsonObject t = v.toObject();
            names << t.value("name").toString();
            QVERIFY2(!t.value("description").toString().isEmpty(), qPrintable(names.last()));
            QCOMPARE(t.value("inputSchema").toObject().value("type").toString(), QStringLiteral("object"));
        }
        for (const QString& ro : control->readOnlyTools()) QVERIFY2(names.contains(ro), qPrintable(ro));
        for (const QString& name : names)
            if (!control->readOnlyTools().contains(name))
                QVERIFY2(control->actionOf(name).contains("Qucs-S"), qPrintable(name));
        QVERIFY(!control->instructions().isEmpty());
        QVERIFY(failed(call("no_such_tool")));
        // Every session of the dock offers them.
        QCOMPARE(app->claudeCode()->current()->session()->toolHost(), control);
    }

    // A schematic built with the tools: parts placed with their properties,
    // wired pin to pin, a net labelled, a part changed, moved and turned,
    // one deleted - each one step to undo; the summary says where every
    // pin is and whether it is connected.
    void aSchematicIsBuiltPartByPart()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        const QJsonObject state = json(call("get_state")).toObject();
        QVERIFY(state.value("documents").toArray().last().toObject().value("in front").toBool());

        const QJsonObject r1 = json(call("add_component", {{"type", "R"}, {"x", 101}, {"y", 99}, {"properties", QJsonObject{{"R", "4.7k"}}}})).toObject();
        QCOMPARE(r1.value("name").toString(), QStringLiteral("R1"));
        QCOMPARE(r1.value("x").toInt(), 100);   // on the grid
        QCOMPARE(r1.value("y").toInt(), 100);
        QCOMPARE(r1.value("pins").toArray().size(), 2);
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("4.7k"));
        QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"x", 0}, {"y", 200}, {"name", "Vin"}, {"rotation", 0}})));
        QVERIFY(sch->getComponentByName("Vin") != nullptr);
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 0}, {"y", 300}})));
        QCOMPARE(sch->a_DocComps.size(), std::size_t(3));
        // Wrong type, wrong property, a name taken: nothing added.
        QVERIFY(text(call("add_component", {{"type", "NoSuchPart"}, {"x", 0}, {"y", 0}})).contains("list_component_types"));
        QVERIFY(text(call("add_component", {{"type", "R"}, {"x", 0}, {"y", 0}, {"properties", QJsonObject{{"Nope", "1"}}}})).contains("its properties are"));
        QVERIFY(failed(call("add_component", {{"type", "R"}, {"x", 0}, {"y", 0}, {"name", "Vin"}})));
        QCOMPARE(sch->a_DocComps.size(), std::size_t(3));

        // Undo takes the last part away, redo brings it back.
        QVERIFY(!failed(call("undo")));
        QCOMPARE(sch->a_DocComps.size(), std::size_t(2));
        QVERIFY(!failed(call("redo")));
        QCOMPARE(sch->a_DocComps.size(), std::size_t(3));

        // Wired: pin to pin, and through places.
        QVERIFY(!failed(call("connect", {{"from", "R1.1"}, {"to", "Vin.1"}})));
        QVERIFY(!failed(call("connect", {{"from", "Vin.2"}, {"to", "GND.1"}})));   // the ground, by its type
        QVERIFY(!failed(call("add_wire", {{"points", QJsonArray{QJsonArray{130, 100}, QJsonArray{200, 100}, QJsonArray{200, 150}}}})));
        QVERIFY(sch->a_DocWires.size() >= 3);
        QVERIFY(text(call("connect", {{"from", "R9.1"}, {"to", "R1.1"}})).contains("no component"));
        QVERIFY(text(call("connect", {{"from", "R1.7"}, {"to", "R1.1"}})).contains("no pin"));
        QJsonObject summary = json(call("get_schematic")).toObject();
        const QJsonArray pins = componentIn(summary, "R1").value("pins").toArray();
        QVERIFY(pins.at(0).toObject().value("connected").toBool());
        QVERIFY(pins.at(1).toObject().value("connected").toBool());

        // A net label, at a pin.
        QVERIFY(!failed(call("set_label", {{"at", "R1.2"}, {"name", "out"}})));
        summary = json(call("get_schematic")).toObject();
        bool labelled = false;
        for (const QJsonValue& v : summary.value("labels").toArray()) labelled = labelled || v.toObject().value("net").toString() == "out";
        QVERIFY(labelled);
        QVERIFY(text(call("set_label", {{"at", "Vin.2"}, {"name", "x"}})).contains("ground"));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 400}, {"y", 300}})));
        QVERIFY(text(call("connect", {{"from", "GND.1"}, {"to", QJsonArray{400, 200}}})).contains("There are 2"));
        QVERIFY(!failed(call("undo")));

        // Changed: a property, turned, moved, renamed, made inactive.
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "10k"}}}})));
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("10k"));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"rotation", 1}, {"x", 400}, {"y", 400}, {"rename", "Rload"}, {"active", false}})));
        Component* r = sch->getComponentByName("Rload");
        QVERIFY(r != nullptr);
        QCOMPARE(r->rotated, 1);
        QCOMPARE(r->cx, 400);
        QCOMPARE(r->cy, 400);
        QCOMPARE(r->isActive, COMP_IS_OPEN);
        QVERIFY(text(call("edit_component", {{"name", "Rload"}, {"properties", QJsonObject{{"Nope", "1"}}}})).contains("its properties are"));
        QVERIFY(failed(call("edit_component", {{"name", "Nobody"}})));

        // Deleted: a part, a label.
        QVERIFY(!failed(call("delete", {{"names", QJsonArray{"Rload", "out"}}})));
        QVERIFY(sch->getComponentByName("Rload") == nullptr);
        QVERIFY(failed(call("delete", {{"names", QJsonArray{"Nobody"}}})));

        // Selected, zoomed.
        QVERIFY(!failed(call("select", {{"names", QJsonArray{"Vin"}}})));
        QVERIFY(sch->getComponentByName("Vin")->isSelected);
        QVERIFY(!failed(call("zoom", {{"to", "all"}})));
        QVERIFY(failed(call("zoom", {{"to", "sideways"}})));
    }

    // The schematic as its file's text, and replaced from text: sections
    // given replace theirs, others stay; text that does not read changes
    // nothing and says why - without the message box it would open.
    void aSchematicIsReadAndReplacedAsText()
    {
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        const QString textNow = text(call("get_schematic", {{"format", "text"}}));
        QVERIFY(textNow.startsWith("<Qucs Schematic"));
        QVERIFY(textNow.contains("<Components>"));
        QVERIFY(textNow.contains("<Vdc Vin"));
        const std::size_t wires = sch->a_DocWires.size();

        QVERIFY(!failed(call("set_schematic", {{"text", "<Components>\n  <R R7 1 500 500 15 -26 0 0 \"1 kOhm\" 1>\n  <GND * 1 500 600 0 0 0 0>\n</Components>\n"}})));
        QCOMPARE(sch->a_DocComps.size(), std::size_t(2));
        QVERIFY(sch->getComponentByName("R7") != nullptr);
        QCOMPARE(sch->a_DocWires.size(), wires);   // <Wires> not given: kept
        QVERIFY(!failed(call("undo")));
        QVERIFY(sch->getComponentByName("Vin") != nullptr);

        const std::size_t parts = sch->a_DocComps.size();
        const QJsonObject bad = call("set_schematic", {{"text", "<Components>\n  <NoSuchThing X1 1 0 0 0 0 0 0>\n</Components>\n"}});
        QVERIFY(failed(bad));
        QVERIFY(text(bad).contains("NoSuchThing"));
        QCOMPARE(sch->a_DocComps.size(), parts);
        QVERIFY(failed(call("set_schematic", {{"text", "hello"}})));
        QVERIFY(failed(call("set_schematic", {{"text", "<Components>\n  <R R8 1 0 0 15 -26 0 0 \"1\" 1>\n"}})));   // not closed
        QCOMPARE(sch->a_DocComps.size(), parts);
    }

    // A picture of it, as Claude sees it.
    void aPictureIsTaken()
    {
        for (const QString& area : {QStringLiteral("all"), QStringLiteral("visible")}) {
            const QJsonObject shot = call("screenshot", {{"area", area}});
            QVERIFY(!failed(shot));
            const QJsonObject image = shot.value("content").toArray().at(0).toObject();
            QCOMPARE(image.value("type").toString(), QStringLiteral("image"));
            QCOMPARE(image.value("mimeType").toString(), QStringLiteral("image/png"));
            QImage decoded;
            QVERIFY(decoded.loadFromData(QByteArray::fromBase64(image.value("data").toString().toLatin1()), "PNG"));
            QVERIFY(decoded.width() > 10 && decoded.height() > 10);
            QVERIFY(decoded.width() <= 3200);
        }
    }

    // Documents: saved under a name, closed, opened again, shown; closing
    // one with unsaved changes wants to be told what to do with them.
    void documentsAreSavedClosedAndOpened()
    {
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        QVERIFY(failed(call("save_document")));   // untitled: needs 'as'
        QVERIFY(!failed(call("save_document", {{"as", "built"}})));
        const QString file = dir.filePath("workspace/built.sch");
        QVERIFY(QFileInfo::exists(file));
        QVERIFY(!sch->getDocChanged());
        QVERIFY(!failed(call("close_document")));
        QVERIFY(app->findDoc(file) == nullptr);
        QVERIFY(failed(call("open_document", {{"path", "nothing-here.sch"}})));
        QVERIFY(!failed(call("open_document", {{"path", "built.sch"}})));
        QVERIFY(front() != nullptr);
        QCOMPARE(QFileInfo(front()->getDocName()).fileName(), QStringLiteral("built.sch"));
        QVERIFY(front()->getComponentByName("Vin") != nullptr);
        QVERIFY(!failed(call("show_document", {{"path", file}})));
        QVERIFY(failed(call("show_document", {{"path", "elsewhere.sch"}})));

        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"x", 300}, {"y", 300}})));
        QVERIFY(text(call("close_document")).contains("unsaved"));
        QVERIFY(app->findDoc(file) != nullptr);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(app->findDoc(file) == nullptr);
        QVERIFY(!failed(call("open_document", {{"path", file}})));
        QVERIFY(front()->getComponentByName("C1") == nullptr);   // the change was discarded
    }

    // The library, the menus: listed; an action used; those that open the
    // system's file dialogs refused, as is quitting.
    void theLibraryAndTheMenusAreListed()
    {
        const QJsonArray types = json(call("list_component_types", {{"search", "resistor"}})).toArray();
        bool resistor = false;
        for (const QJsonValue& v : types) resistor = resistor || v.toObject().value("type").toString() == "R";
        QVERIFY(resistor);
        const QJsonArray actions = json(call("list_actions", {{"search", "undo"}})).toArray();
        bool undo = false;
        for (const QJsonValue& v : actions) undo = undo || v.toObject().value("action").toString().endsWith("Undo");
        QVERIFY(undo);

        Schematic* sch = front();
        for (Component* c : sch->a_DocComps) c->isSelected = false;
        QVERIFY(!failed(call("trigger_action", {{"action", "Edit > Select All"}})));
        QVERIFY(std::all_of(sch->a_DocComps.cbegin(), sch->a_DocComps.cend(), [](Component* c) { return c->isSelected; }));
        QVERIFY(text(call("trigger_action", {{"action", "File > Open"}})).contains("open_document"));
        QVERIFY(failed(call("trigger_action", {{"action", app->fileQuit->objectName().isEmpty() ? "File > Exit" : app->fileQuit->objectName()}})));
        QVERIFY(failed(call("trigger_action", {{"action", "Nowhere > Nothing"}})));
    }

    // A dialog an action opens: read, filled in, answered - from within
    // its own event loop, as the program's calls come while it is open.
    void aDialogIsReadFilledAndAnswered()
    {
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        // File > Document Settings (by the last part of its path).
        const QString settings = app->fileSettings->text();

        QJsonObject seen, answered;
        QTimer::singleShot(800, this, [&] {
            seen = json(call("get_dialog")).toObject();
            answered = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", "horizontal Grid"}, {"value", "20"}}}},
                                           {"press", "OK"}});
        });
        const QJsonObject triggered = call("trigger_action", {{"action", settings}}, 20000);
        QVERIFY2(!failed(triggered), qPrintable(text(triggered)));
        QVERIFY2(text(triggered).contains("get_dialog"), qPrintable(text(triggered)));
        QTRY_VERIFY_WITH_TIMEOUT(!answered.isEmpty(), 10000);
        QVERIFY(!seen.value("title").toString().isEmpty());
        bool grid = false;
        for (const QJsonValue& v : seen.value("controls").toArray())
            grid = grid || v.toObject().value("label").toString().contains("horizontal Grid", Qt::CaseInsensitive);
        QVERIFY2(grid, QJsonDocument(seen).toJson().constData());
        QVERIFY2(!failed(answered), qPrintable(text(answered)));
        QVERIFY2(text(answered).contains("no dialog is open"), qPrintable(text(answered)));
        QCOMPARE(sch->getGridX(), 20);
        QVERIFY(text(call("get_dialog")).contains("No dialog"));
    }

    // A simulation, waited for: this one fails (the simulator is false),
    // and says so when it is over.
    void aSimulationIsWaitedFor()
    {
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        QVERIFY(!failed(call("add_component", {{"type", ".DC"}, {"x", 100}, {"y", -100}})));
        const QJsonObject result = call("simulate", {{"timeout", 30}}, 60000);
        QVERIFY2(!failed(result), qPrintable(text(result)));
        const QJsonObject outcome = json(result).toObject();
        QVERIFY2(outcome.value("finished").toBool(), qPrintable(text(result)));
        QVERIFY(outcome.contains("succeeded"));
        QVERIFY(!outcome.value("succeeded").toBool());   // no dataset came of it
        QVERIFY(!outcome.value("dataset written").toBool());
        QVERIFY(outcome.value("dataset").toString().endsWith(".dat"));
        QVERIFY(!app->simulationConsole()->isRunning());
        QVERIFY(failed(call("simulate", {{"path", "not-open.sch"}})));
    }
};

QTEST_MAIN(TestQucsControl)
#include "test_qucs_control.moc"
