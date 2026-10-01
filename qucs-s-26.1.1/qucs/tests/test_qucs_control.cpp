/*
 * Claude's tools for the Qucs-S window (qucscontrol.h), used on the
 * application itself: documents opened, shown, saved and closed; a
 * schematic built part by part - components, wires, labels, changes, one
 * step to undo each; wires that join their ends' nets and nothing else,
 * parts turned and moved with the circuit kept - read back as a summary
 * (with the nets) and as text, replaced from
 * text (and left alone when the text does not read, without a message
 * box); a picture of it; the menus' actions, a dialog one opens read,
 * filled in and closed; a simulation waited for, its errors told each
 * with its part; the results read as numbers and measured, plotted in
 * diagrams made and changed by their named fields; a net renamed with its
 * traces; a component type described; the netlist.
 */
#include <QtTest>
#include <QElapsedTimer>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QScopeGuard>
#include <QMenuBar>
#include <QWidgetAction>
#include <QToolButton>
#include <QToolBar>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QPdfWriter>
#include <QProcess>
#include <QPainter>
#include <QUrl>

#include <cmath>
#include <complex>
#include <functional>
#include <random>

#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "components/component.h"
#include "config.h"
#include "dataimport.h"
#include "diagrams/diagram.h"
#include "erc.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "node.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "simulationconsole.h"
#include "valuereading.h"
#include "wire.h"
#include "wirelabel.h"
#include "diagrams/graph.h"
#include "qucscontrol_p.h"
#include "paintings/portsymbol.h"
#include "paintings/graphictext.h"
#include "dialogs/simmessage.h"
#include "textdoc.h"

using qucs_s::control::renameComponentIn;

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
        // (The first text: a note may follow it - what changed since the
        // last call.)
        QString first;
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                first = v.toObject().value(QStringLiteral("text")).toString();
                break;
            }
        const QJsonDocument d = QJsonDocument::fromJson(first.toUtf8());
        return d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object());
    }
    Schematic* front() const { return app->currentSchematic(); }
    // A schematic's text with each wire from its lesser end (as a rebuild
    // - an undo - writes it), for comparing.
    static QString sameText(const QString& text)
    {
        static const QRegularExpression wire(QStringLiteral("^  <(-?\\d+) (-?\\d+) (-?\\d+) (-?\\d+) (.*)$"));
        QStringList lines = text.split('\n');
        for (QString& line : lines) {
            const QRegularExpressionMatch m = wire.match(line);
            if (!m.hasMatch()) continue;
            const QPoint a(m.captured(1).toInt(), m.captured(2).toInt()), b(m.captured(3).toInt(), m.captured(4).toInt());
            const bool swap = std::pair(b.x(), b.y()) < std::pair(a.x(), a.y());
            const QPoint p = swap ? b : a, q = swap ? a : b;
            line = QStringLiteral("  <%1 %2 %3 %4 %5").arg(p.x()).arg(p.y()).arg(q.x()).arg(q.y()).arg(m.captured(5));
        }
        return lines.join('\n');
    }
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
            // Each field says what it is, in the schema itself: not only in
            // the tool's text (a deferred tool's, or a core tool's in
            // describe_tool alone).
            const QJsonObject fields = t.value("inputSchema").toObject().value("properties").toObject();
            for (auto it = fields.begin(); it != fields.end(); ++it)
                QVERIFY2(!it.value().toObject().value("description").toString().isEmpty(), qPrintable(names.last() + "." + it.key()));
        }
        for (const QString& ro : control->readOnlyTools()) QVERIFY2(names.contains(ro), qPrintable(ro));
        for (const QString& name : names)
            if (!control->readOnlyTools().contains(name))
                QVERIFY2(control->actionOf(name).contains("Qucs-S"), qPrintable(name));
        QVERIFY(!control->instructions().isEmpty());
        // Claude Code keeps 2,048 characters of each unless told more: the
        // dock tells it what these need, each tool's description is whole
        // in a terminal's, and what a terminal's keeps of the instructions
        // says where the rest is.
        const QString instructions = control->instructions();
        const int cap = qucs_s::claude::claudeEnvironment(QProcessEnvironment(), control)
                            .value("CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH", QString::number(qucs_s::claude::kDescriptionCap))
                            .toInt();
        QVERIFY2(cap >= instructions.size(), qPrintable(QString::number(cap)));
        for (const QJsonValue& t : control->tools()) {
            const QString description = t.toObject().value("description").toString();
            QVERIFY2(description.size() <= qucs_s::claude::kDescriptionCap,
                     qPrintable(t.toObject().value("name").toString() + ": " + QString::number(description.size())));
        }
        QVERIFY2(instructions.left(qucs_s::claude::kDescriptionCap).contains("the resource qucs://instructions holds them whole"),
                 qPrintable(instructions.left(qucs_s::claude::kDescriptionCap)));
        {
            QString error;
            const QJsonArray contents = control->readResource("qucs://instructions", &error);
            QVERIFY2(!contents.isEmpty(), qPrintable(error));
            QCOMPARE(contents.first().toObject().value("text").toString(), instructions);
            QCOMPARE(contents.first().toObject().value("mimeType").toString(), QStringLiteral("text/plain"));
            bool listed = false;
            for (const QJsonValue& v : control->resources()) listed = listed || v.toObject().value("uri").toString() == "qucs://instructions";
            QVERIFY(listed);
            QVERIFY(!control->resourceVersion("qucs://instructions").isEmpty());
        }
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

    // A part turned, mirrored and moved keeps each pin on its net: one
    // with nothing on its pins (whose nodes were once deleted from under
    // it, and the turn crashed), one wired to a capacitor, one pin on pin
    // with another, one with a net label on a pin. What would put a pin on
    // another net is not done, the schematic left as it was; an open pin
    // that lands on a wire joins it and is told of. Each change one step
    // to undo, every pin and wire end on its node.
    void aPartIsTurnedAndMovedKeepingItsNets()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        const auto inOrder = [sch]() -> QString {
            for (Component* c : sch->a_DocComps)
                for (Port* p : c->Ports) {
                    if (p->Connection == nullptr) return c->Name + " has a pin on no node";
                    if (p->Connection->center() != c->center() + QPoint(p->x, p->y))
                        return c->Name + " has a pin off its node";
                }
            for (Wire* w : sch->a_DocWires)
                if (w->P1() != w->Port1->center() || w->P2() != w->Port2->center()) return QStringLiteral("a wire is off its nodes");
            return {};
        };
        // Whether two pins are joined by wires.
        const auto joined = [sch](const QString& a, int pa, const QString& b, int pb) {
            Node* from = sch->getComponentByName(a)->Ports.at(pa - 1)->Connection;
            Node* to = sch->getComponentByName(b)->Ports.at(pb - 1)->Connection;
            QSet<Node*> seen{from};
            QList<Node*> next{from};
            while (!next.isEmpty()) {
                Node* n = next.takeLast();
                if (n == to) return true;
                for (Wire* w : sch->a_DocWires)
                    for (Node* m : {w->Port1 == n ? w->Port2 : nullptr, w->Port2 == n ? w->Port1 : nullptr})
                        if (m != nullptr && !seen.contains(m)) {
                            seen.insert(m);
                            next << m;
                        }
            }
            return false;
        };

        // Nothing on it: turned, mirrored and moved, over and over.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}})));
        for (int turn = 0; turn < 6; ++turn) {
            QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"rotation", turn}, {"mirror", turn % 2 == 1},
                                                    {"x", 100 + 20 * turn}, {"y", 100}})));
            QCOMPARE(sch->getComponentByName("R1")->rotated, turn % 4);
            QCOMPARE(sch->getComponentByName("R1")->mirroredX, turn % 2 == 1);
            QCOMPARE(sch->getComponentByName("R1")->cx, 100 + 20 * turn);
            QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));
        }
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"rotation", 0}, {"mirror", false}, {"x", 100}, {"y", 100}})));
        QCOMPARE(sch->a_DocNodes.size(), std::size_t(2));

        // Wired to a capacitor: turned, then moved and mirrored - pin 2
        // still on the capacitor's net, pin 1 not.
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("connect", {{"from", "R1.2"}, {"to", "C1.1"}})));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"rotation", 1}})));
        QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));
        QVERIFY(joined("R1", 2, "C1", 1));
        QVERIFY(!joined("R1", 1, "C1", 1));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"x", 100}, {"y", 300}, {"mirror", true}})));
        QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));
        QCOMPARE(sch->getComponentByName("R1")->cy, 300);
        QVERIFY(joined("R1", 2, "C1", 1));
        QVERIFY(!joined("R1", 1, "C1", 1));
        QVERIFY(!joined("R1", 1, "R1", 2));
        // One step back: where it was before.
        QVERIFY(!failed(call("undo")));
        QCOMPARE(sch->getComponentByName("R1")->cy, 100);
        QCOMPARE(sch->getComponentByName("R1")->rotated, 1);
        QVERIFY(joined("R1", 2, "C1", 1));

        // Mirrored where it is, pin 1 comes where pin 2's wire ends: that
        // wire is moved out of its way, and pin 2 wired to it again.
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"mirror", true}})));
        QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));
        QVERIFY(joined("R1", 2, "C1", 1));
        QVERIFY(!joined("R1", 1, "C1", 1));
        QVERIFY(!joined("R1", 1, "R1", 2));
        QVERIFY(!failed(call("undo")));
        // Not where pin 2 would be on another part's pin, on another net:
        // the capacitor's pin 2. Nothing changed.
        const QString unchanged = text(call("get_schematic", {{"format", "text"}}));
        const QJsonObject c1 = componentIn(json(call("get_schematic")).toObject(), "C1");
        const QJsonObject c1pin2 = c1.value("pins").toArray().at(1).toObject();
        QVERIFY(failed(call("edit_component", {{"name", "R1"}, {"rotation", 0}, {"x", c1pin2.value("x").toInt() - 30},
                                               {"y", c1pin2.value("y").toInt()}})));
        QCOMPARE(sameText(text(call("get_schematic", {{"format", "text"}}))), sameText(unchanged));

        // Pin on pin with another part, moved off it: wired to it.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 500}, {"y", 100}, {"name", "Ra"}})));
        const int raPin = componentIn(json(call("get_schematic")).toObject(), "Ra").value("pins").toArray().at(1).toObject().value("x").toInt();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", raPin + 30}, {"y", 100}, {"name", "Rb"}})));
        QVERIFY(joined("Ra", 2, "Rb", 1) || sch->getComponentByName("Ra")->Ports.at(1)->Connection == sch->getComponentByName("Rb")->Ports.at(0)->Connection);
        QVERIFY(!failed(call("edit_component", {{"name", "Rb"}, {"x", 700}, {"y", 400}, {"rotation", 3}})));
        QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));
        QVERIFY(joined("Ra", 2, "Rb", 1));
        QVERIFY(!failed(call("edit_component", {{"name", "Ra"}, {"rotation", 2}, {"x", 500}, {"y", 200}})));
        QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));
        QVERIFY(joined("Ra", 2, "Rb", 1));
        QVERIFY(!joined("Ra", 1, "Rb", 1));

        // A net label on a pin with nothing else: it goes with the pin.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 100}, {"y", 600}, {"name", "Rl"}})));
        QVERIFY(!failed(call("set_label", {{"at", "Rl.1"}, {"name", "vin"}})));
        QVERIFY(!failed(call("edit_component", {{"name", "Rl"}, {"x", 300}, {"y", 700}, {"rotation", 1}})));
        Node* labelled = sch->getComponentByName("Rl")->Ports.at(0)->Connection;
        QVERIFY(labelled->hasLabel());
        QCOMPARE(labelled->label()->Name, QStringLiteral("vin"));
        QCOMPARE(labelled->label()->root(), labelled->center());

        // A pin put down on a wire of another net: the wire is moved out
        // of its way, still joining what it joined.
        QVERIFY(!failed(call("add_wire", {{"points", QJsonArray{QJsonArray{900, 100}, QJsonArray{900, 300}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 1100}, {"y", 100}, {"name", "Ro"}})));
        const QJsonObject aside = call("edit_component", {{"name", "Ro"}, {"rotation", 0}, {"x", 930}, {"y", 200}});
        QVERIFY2(!failed(aside), qPrintable(text(aside)));
        QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));
        QCOMPARE(sch->getComponentByName("Ro")->Ports.at(0)->Connection->conn_count(), 1);
        {
            Node* top = sch->findNode(900, 100);
            Node* bottom = sch->findNode(900, 300);
            QVERIFY(top != nullptr && bottom != nullptr);
            QSet<Node*> seen{top};
            QList<Node*> next{top};
            while (!next.isEmpty()) {
                Node* n = next.takeLast();
                for (Wire* w : sch->a_DocWires)
                    for (Node* m : {w->Port1 == n ? w->Port2 : nullptr, w->Port2 == n ? w->Port1 : nullptr})
                        if (m != nullptr && !seen.contains(m)) {
                            seen.insert(m);
                            next << m;
                        }
            }
            QVERIFY(seen.contains(bottom));
        }
        // A pin with nothing on it put down on another's pin: on its net,
        // and told so.
        const int roPin = componentIn(json(call("get_schematic")).toObject(), "Ro").value("pins").toArray().at(1).toObject().value("x").toInt();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 1300}, {"y", 400}, {"name", "Rp"}})));
        const QJsonObject landed = json(call("edit_component", {{"name", "Rp"}, {"x", roPin + 30}, {"y", 200}})).toObject();
        QVERIFY2(landed.value("note").toString().contains("Rp.1"), qPrintable(QJsonDocument(landed).toJson()));
        QVERIFY2(inOrder().isEmpty(), qPrintable(inOrder()));

        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A wire joins its two ends' nets and nothing else: connect goes
    // around what is in the way (the wire tool's route from R1.2 to C1.1
    // here runs over C1.2, and from C1.2 to ground over V1's pins - they
    // once shorted the whole circuit), add_wire over another pin is not
    // drawn. get_schematic names each pin's net and lists the nets.
    void wiresJoinTheirEndsAndNothingElse()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"x", 0}, {"y", 150}, {"name", "V1"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 150}, {"y", 100}, {"name", "R1"}})));
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"x", 300}, {"y", 150}, {"name", "C1"}, {"rotation", 1}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 0}, {"y", 250}})));
        for (const auto& [from, to] : {std::pair{"V1.1", "R1.1"}, {"R1.2", "C1.1"}, {"V1.2", "GND.1"}, {"C1.2", "GND.1"}}) {
            const QJsonObject r = call("connect", {{"from", from}, {"to", to}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
        }
        const QJsonObject summary = json(call("get_schematic")).toObject();
        QStringList nets;
        for (const QJsonValue& v : summary.value("nets").toArray()) {
            QStringList pins;
            for (const QJsonValue& p : v.toObject().value("pins").toArray()) pins << p.toString();
            pins.sort();
            nets << pins.join(' ');
        }
        nets.sort();
        QCOMPARE(nets, QStringList({"C1.1 R1.2", "C1.2 GND.1 V1.2", "R1.1 V1.1"}));
        const QJsonObject r1 = componentIn(summary, "R1");
        QCOMPARE(r1.value("pins").toArray().at(1).toObject().value("net").toString(),
                 componentIn(summary, "C1").value("pins").toArray().at(0).toObject().value("net").toString());
        QCOMPARE(componentIn(summary, "V1").value("pins").toArray().at(1).toObject().value("net").toString(), QStringLiteral("gnd"));
        // Already one net: nothing drawn.
        const std::size_t wires = sch->a_DocWires.size();
        QVERIFY(text(call("connect", {{"from", "V1.2"}, {"to", "C1.2"}})).contains("already"));
        QCOMPARE(sch->a_DocWires.size(), wires);

        // Straight over R1 from pin to pin: R1 shorted - not drawn.
        const QString unchanged = text(call("get_schematic", {{"format", "text"}}));
        const QJsonObject over = call("add_wire", {{"points", QJsonArray{QJsonArray{100, 100}, QJsonArray{200, 100}}}});
        QVERIFY(failed(over));
        QVERIFY2(text(over).contains("R1."), qPrintable(text(over)));
        QCOMPARE(sameText(text(call("get_schematic", {{"format", "text"}}))), sameText(unchanged));
        // Where nothing is in the way: drawn.
        QVERIFY(!failed(call("add_wire", {{"points", QJsonArray{QJsonArray{500, 0}, QJsonArray{600, 0}, QJsonArray{600, 100}}}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
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
        for (const QString& area : {QStringLiteral("all"), QStringLiteral("visible"), QStringLiteral("paper"), QStringLiteral("screen"),
                                    QStringLiteral("window")}) {
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
        // Each says what it is a picture of: paper or the screen.
        QVERIFY(text(call("screenshot", {{"area", "paper"}})).contains("white paper"));
        QVERIFY(text(call("screenshot", {{"area", "screen"}})).contains("theme"));
        QVERIFY(text(call("screenshot", {{"area", "window"}})).contains("the window"));
        QVERIFY(failed(call("screenshot", {{"area", "moon"}})));
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

        QJsonObject seen, answered, refused, undone, state, window;
        const auto parts = sch->a_DocComps.size();
        QTimer::singleShot(800, this, [&] {
            // While it waits: what changes waits too (the dialog holds on to
            // what it edits); what looks goes on.
            refused = call("add_component", {{"type", "R"}, {"x", 500}, {"y", 500}});
            undone = call("undo");
            state = call("get_state");
            window = call("screenshot", {{"area", "window"}});
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
        QVERIFY(failed(refused));
        QVERIFY2(text(refused).contains("waits for an answer"), qPrintable(text(refused)));
        QVERIFY(failed(undone));
        QVERIFY(!failed(state));
        // The window, and the dialog over it: a picture each.
        int pictures = 0;
        for (const QJsonValue& c : window.value("content").toArray())
            if (c.toObject().value("type").toString() == "image") ++pictures;
        QVERIFY2(pictures == 2, qPrintable(text(window)));
        QVERIFY(text(window).contains("the dialog"));
        QCOMPARE(sch->a_DocComps.size(), parts);
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
        // "false" said nothing, but it ended with exit code 1: no success.
        QVERIFY2(!outcome.value("succeeded").toBool(), qPrintable(text(result)));
        QCOMPARE(outcome.value("exit code").toInt(), 1);
        // Where ngspice writes it - not name.dat, which is Qucsator's.
        QVERIFY2(outcome.value("dataset").toString().endsWith(".dat.ngspice"), qPrintable(text(result)));
        // Why it failed, as an error of its own.
        const QJsonArray errors = outcome.value("errors").toArray();
        QVERIFY2(!errors.isEmpty(), qPrintable(text(result)));
        QVERIFY2(errors.first().toObject().value("message").toString().contains("exit code 1"), qPrintable(text(result)));
        QVERIFY(!app->simulationConsole()->isRunning());
        QVERIFY(failed(call("simulate", {{"path", "not-open.sch"}})));
    }

    // The results: a dataset (written here as ngspice writes one) read as
    // numbers and measured; diagrams made, changed and deleted by their
    // named fields, their traces showing the data at once - and again
    // when their section is replaced as text; the data read again when it
    // changes; a net renamed with the traces that show it, and a label
    // taken away warning of those it leaves; a type described; the
    // netlist given.
    void theResultsAreReadAndPlotted()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(sch != nullptr);
        QVERIFY(!failed(call("add_component", {{"type", "Vpulse"}, {"name", "V1"}, {"x", 100}, {"y", 200},
                                               {"properties", QJsonObject{{"U1", "0"}, {"U2", "1 V"}, {"T1", "1 us"}, {"T2", "1 ms"}, {"Tr", "1 ns"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}, {"properties", QJsonObject{{"R", "1k"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 300}, {"y", 200}, {"rotation", 1},
                                               {"properties", QJsonObject{{"C", "1n"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 200}, {"y", 320}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"name", "TR1"}, {"x", 100}, {"y", 450},
                                               {"properties", QJsonObject{{"Stop", "10 us"}, {"Points", "1001"}}}})));
        for (const auto& [a, b] : {std::pair("V1.1", "R1.1"), std::pair("R1.2", "C1.1"), std::pair("C1.2", "GND.1"), std::pair("V1.2", "GND.1")}) {
            const QJsonObject r = call("connect", {{"from", a}, {"to", b}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
        }
        QVERIFY(!failed(call("set_label", {{"at", "R1.2"}, {"name", "out"}})));
        QVERIFY(!failed(call("set_label", {{"at", "R1.1"}, {"name", "in"}})));
        QVERIFY(!failed(call("save_document", {{"as", "rc"}})));
        const QString docFile = dir.filePath("workspace/rc.sch");
        QVERIFY(QFileInfo::exists(docFile));

        // Before a simulation: nothing to read, and diagrams wait for data.
        QVERIFY(text(call("get_dataset")).contains("simulate first"));

        // The dataset ngspice would write: the step response of the RC
        // (tau = 1 us, the step at 1 us), and its AC response.
        const auto writeDataset = [&](double gain) {
            QFile f(dir.filePath("workspace/rc.dat.ngspice"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream s(&f);
            s << "<Qucs Dataset 26.1.3>\n<indep time 1001>\n";
            for (int i = 0; i <= 1000; ++i) s << "  " << QString::number(i * 1e-8, 'e', 12) << "\n";
            s << "</indep>\n<dep tran.v(out) time>\n";
            for (int i = 0; i <= 1000; ++i) {
                const double t = i * 1e-8;
                s << "  " << QString::number(t < 1e-6 ? 0.0 : gain * (1 - std::exp(-(t - 1e-6) / 1e-6)), 'e', 12) << "\n";
            }
            s << "</dep>\n<dep tran.i(v1) time>\n";
            for (int i = 0; i <= 1000; ++i) {
                const double t = i * 1e-8;
                s << "  " << QString::number(t < 1e-6 ? 0.0 : -1e-3 * std::exp(-(t - 1e-6) / 1e-6), 'e', 12) << "\n";
            }
            s << "</dep>\n<indep frequency 201>\n";
            for (int i = 0; i <= 200; ++i) s << "  " << QString::number(std::pow(10.0, 3 + i / 50.0), 'e', 12) << "\n";
            s << "</indep>\n<dep ac.v(out) frequency>\n";
            for (int i = 0; i <= 200; ++i) {
                // 1 / (1 + j f / fc), fc = 1 / (2 pi RC)
                const double x = std::pow(10.0, 3 + i / 50.0) * 2 * M_PI * 1e-6;
                const double re = 1 / (1 + x * x), im = -x / (1 + x * x);
                s << "  " << QString::number(re, 'e', 12) << (im < 0 ? "-j" : "+j") << QString::number(std::abs(im), 'e', 12) << "\n";
            }
            s << "</dep>\n";
        };
        writeDataset(1.0);

        // The variables it holds, with the names traces take.
        const QJsonObject listed = json(call("get_dataset")).toObject();
        QVERIFY2(listed.value("dataset").toString().endsWith("rc.dat.ngspice"), qPrintable(QJsonDocument(listed).toJson()));
        bool out = false;
        for (const QJsonValue& v : listed.value("variables").toArray())
            if (v.toObject().value("name").toString() == "tran.v(out)") {
                out = true;
                QCOMPARE(v.toObject().value("trace").toString(), QStringLiteral("ngspice/tran.v(out)"));
                QCOMPARE(v.toObject().value("points").toInt(), 1001);
            }
        QVERIFY(out);
        QCOMPARE(listed.value("independent variables").toArray().size(), 2);

        // Values, samples, measurements - "out" is each analysis's.
        const QJsonObject read = json(call("get_dataset", {{"variables", QJsonArray{"tran.v(out)"}}, {"at", QJsonArray{2e-6}},
                                                           {"measure", QJsonArray{"rise_time", "settling_time"}}, {"from", 1e-6},
                                                           {"points", 5}}))
                                     .toObject();
        const QJsonObject vout = read.value("variables").toArray().first().toObject();
        QCOMPARE(vout.value("name").toString(), QStringLiteral("tran.v(out)"));
        QVERIFY2(std::abs(vout.value("at").toArray().first().toArray().at(1).toDouble() - (1 - std::exp(-1.0))) < 1e-3,
                 qPrintable(QJsonDocument(vout).toJson()));
        const double rise = vout.value("measurements").toObject().value("rise_time").toObject().value("value").toDouble();
        QVERIFY2(std::abs(rise - 1e-6 * std::log(9.0)) < 0.03e-6, qPrintable(QJsonDocument(vout).toJson()));
        QVERIFY(vout.value("measurements").toObject().value("settling_time").toObject().contains("value"));
        QCOMPARE(vout.value("samples").toArray().size(), 5);
        QCOMPARE(vout.value("columns").toArray().size(), 2);
        QVERIFY(std::abs(vout.value("max").toDouble() - 1) < 1e-3);
        QCOMPARE(json(call("get_dataset", {{"variables", QJsonArray{"out"}}})).toObject().value("variables").toArray().size(), 2);
        const QJsonObject ac = json(call("get_dataset", {{"variables", QJsonArray{"ac.v(out)"}}, {"form", "db_phase"},
                                                         {"at", QJsonArray{1 / (2 * M_PI * 1e-6)}}, {"measure", QJsonArray{"bandwidth"}}}))
                                   .toObject().value("variables").toArray().first().toObject();
        const QJsonArray corner = ac.value("at").toArray().first().toArray();
        QVERIFY2(std::abs(corner.at(1).toDouble() + 3.0103) < 0.05 && std::abs(corner.at(2).toDouble() + 45) < 0.5,
                 qPrintable(QJsonDocument(ac).toJson()));
        QVERIFY(std::abs(ac.value("measurements").toObject().value("bandwidth").toObject().value("value").toDouble() - 159155) < 2000);
        QVERIFY(text(call("get_dataset", {{"variables", QJsonArray{"v(nowhere)"}}})).contains("tran.v(out)"));
        QVERIFY(failed(call("get_dataset", {{"variables", QJsonArray{"out"}}, {"measure", QJsonArray{"nonsense"}}})));

        // A diagram: v(out) is of two analyses - say which; a Smith chart
        // takes the AC one.
        QVERIFY(text(call("add_diagram", {{"x", 450}, {"y", 400}, {"traces", QJsonArray{"v(out)"}}})).contains("say which"));
        const QJsonObject placed = json(call("add_diagram", {{"x", 450}, {"y", 400}, {"width", 300}, {"height", 200},
                                                            {"traces", QJsonArray{"tran.v(out)"}},
                                                            {"x_axis", QJsonObject{{"label", "time (s)"}}}}))
                                       .toObject();
        QCOMPARE(placed.value("diagram").toInt(), 1);
        QCOMPARE(placed.value("type").toString(), QStringLiteral("rect"));
        const QJsonObject trace = placed.value("traces").toArray().first().toObject();
        QCOMPARE(trace.value("variable").toString(), QStringLiteral("ngspice/tran.v(out)"));
        QCOMPARE(trace.value("points").toInt(), 1001);   // bound to the data at once
        QCOMPARE(sch->a_DocDiags.size(), std::size_t(1));
        Diagram* rect = sch->a_DocDiags.front();
        QCOMPARE(rect->xAxis.Label, QStringLiteral("time (s)"));
        const QJsonObject smith = json(call("add_diagram", {{"type", "smith"}, {"x", 800}, {"y", 400}, {"traces", QJsonArray{"v(out)"}}})).toObject();
        QCOMPARE(smith.value("traces").toArray().first().toObject().value("variable").toString(), QStringLiteral("ngspice/ac.v(out)"));
        QVERIFY(failed(call("add_diagram", {{"type", "pie"}, {"x", 0}, {"y", 0}})));

        // Changed by name: axes, legend; a trace added on the right axis,
        // restyled, pointed elsewhere.
        QVERIFY(!failed(call("edit_diagram", {{"diagram", 1}, {"y_axis", QJsonObject{{"label", "V(out)"}, {"from", 0}, {"to", 1.2}}},
                                              {"legend", "top_right"}})));
        QVERIFY(!rect->yAxis.autoScale);
        QCOMPARE(rect->yAxis.limit_max, 1.2);
        QVERIFY(rect->yAxis.step > 0);
        QCOMPARE(rect->yAxis.Label, QStringLiteral("V(out)"));
        QCOMPARE(rect->legendPos, int(Diagram::LegendTopRight));
        QVERIFY(failed(call("edit_diagram", {{"diagram", 1}, {"y_axis", QJsonObject{{"from", 2}, {"to", 1}}}})));
        QCOMPARE(rect->yAxis.limit_max, 1.2);   // refused: unchanged
        QVERIFY(failed(call("edit_diagram", {{"diagram", 7}})));
        QVERIFY(text(call("edit_diagram", {{"grid", false}})).contains("2 diagrams"));   // which?

        const QJsonObject added = json(call("add_trace", {{"diagram", 1}, {"variable", "i(v1)"}, {"color", "red"}, {"style", "dash"},
                                                          {"axis", "right"}}))
                                      .toObject();
        QCOMPARE(added.value("variable").toString(), QStringLiteral("ngspice/tran.i(v1)"));
        QCOMPARE(added.value("points").toInt(), 1001);
        QCOMPARE(rect->Graphs.size(), 2);
        QCOMPARE(rect->Graphs.at(1)->Color, QColor(Qt::red));
        QCOMPARE(rect->Graphs.at(1)->Style, GRAPHSTYLE_DASH);
        QCOMPARE(rect->Graphs.at(1)->yAxisNo, 1);
        QVERIFY(failed(call("add_trace", {{"diagram", 1}, {"variable", "tran.i(v1)"}})));   // there already
        QVERIFY(failed(call("add_trace", {{"diagram", 1}, {"variable", "tran.v(out)"}, {"color", "notacolor"}})));
        QVERIFY(!failed(call("edit_trace", {{"diagram", 1}, {"trace", 2}, {"thickness", 3}, {"color", "#00aa00"}})));
        QCOMPARE(rect->Graphs.at(1)->Thick, 3);
        QCOMPARE(rect->Graphs.at(1)->Color, QColor("#00aa00"));
        QVERIFY(!rect->Graphs.at(1)->autoColor);
        QVERIFY(!failed(call("edit_trace", {{"diagram", 1}, {"trace", 2}, {"color", "auto"}})));
        QVERIFY(rect->Graphs.at(1)->autoColor);
        QVERIFY(!failed(call("edit_trace", {{"diagram", 1}, {"trace", 2}, {"color", "#00aa00"}})));
        QVERIFY(!rect->Graphs.at(1)->autoColor);   // a color asked for is seen
        const QJsonObject nowhere = json(call("edit_trace", {{"diagram", 1}, {"trace", "tran.i(v1)"}, {"variable", "v(nowhere)"}})).toObject();
        QCOMPARE(nowhere.value("variable").toString(), QStringLiteral("ngspice/tran.v(nowhere)"));   // the one analysis
        QVERIFY2(nowhere.value("no data").toString().contains("has no variable"), qPrintable(QJsonDocument(nowhere).toJson()));
        QVERIFY(nowhere.contains("note"));

        // As get_schematic lists them.
        const QJsonArray diagrams = json(call("get_schematic")).toObject().value("diagrams").toArray();
        QCOMPARE(diagrams.size(), 2);
        QCOMPARE(diagrams.at(0).toObject().value("y_axis").toObject().value("to").toDouble(), 1.2);
        QCOMPARE(diagrams.at(0).toObject().value("traces").toArray().at(1).toObject().value("style").toString(), QStringLiteral("dash"));

        // Deleted: a trace, a diagram - one step to undo.
        QVERIFY(!failed(call("delete", {{"traces", QJsonArray{QJsonObject{{"diagram", 1}, {"trace", 2}}}}, {"diagrams", QJsonArray{2}}})));
        QCOMPARE(sch->a_DocDiags.size(), std::size_t(1));
        QCOMPARE(sch->a_DocDiags.front()->Graphs.size(), 1);
        QVERIFY(!failed(call("undo")));
        QCOMPARE(sch->a_DocDiags.size(), std::size_t(2));
        QCOMPARE(sch->a_DocDiags.front()->Graphs.size(), 2);
        QVERIFY(!sch->a_DocDiags.front()->Graphs.first()->isEmpty());   // undo reads the data again
        QVERIFY(!failed(call("delete", {{"diagrams", QJsonArray{2}}, {"traces", QJsonArray{QJsonObject{{"diagram", 1}, {"trace", 2}}}}})));
        rect = sch->a_DocDiags.front();

        // Its section replaced as text: the new diagram shows the data.
        const QString textNow = sch->documentText();
        const QString section = textNow.mid(textNow.indexOf("<Diagrams>"), textNow.indexOf("</Diagrams>") + 11 - textNow.indexOf("<Diagrams>"));
        QVERIFY(section.contains("ngspice/tran.v(out)"));
        QVERIFY(!failed(call("set_schematic", {{"text", QString(section).replace("#0000ff", "#ff00ff")}})));
        rect = sch->a_DocDiags.front();
        QCOMPARE(rect->Graphs.first()->Color, QColor("#ff00ff"));
        QVERIFY2(!rect->Graphs.first()->isEmpty(), "the replaced diagram lost its data");

        // The data changes: read again.
        writeDataset(2.0);
        const QJsonObject reloaded = json(call("reload_data", {{"path", docFile}})).toObject();
        QCOMPARE(reloaded.value("reloaded").toArray().first().toObject().value("traces with data").toInt(), 1);
        QVERIFY2(std::abs(rect->yAxis.max - 2) < 0.01, qPrintable(QString::number(rect->yAxis.max)));
        QStringList paths;
        bool reload = false;
        for (const QJsonValue& v : json(call("list_actions", {{"search", "reload"}})).toArray())
            reload = reload || v.toObject().value("action").toString() == "Simulation > Reload Simulation Data";
        QVERIFY(reload);

        // A net renamed: its labels, and the traces that show it.
        QVERIFY(text(call("rename_net", {{"from", "out"}, {"to", "in"}})).contains("join"));
        QVERIFY(failed(call("rename_net", {{"from", "nothing"}, {"to", "x"}})));
        const QJsonObject renamed = call("rename_net", {{"from", "out"}, {"to", "out1"}});
        QVERIFY2(!failed(renamed), qPrintable(text(renamed)));
        QVERIFY2(text(renamed).contains("dataset still calls it out"), qPrintable(text(renamed)));
        QCOMPARE(rect->Graphs.first()->Var, QStringLiteral("ngspice/tran.v(out1)"));
        QCOMPARE(json(call("get_schematic")).toObject().value("nets").toArray().size() > 0, true);
        bool labelled = false;
        for (Wire* w : sch->a_DocWires) labelled = labelled || (w->hasLabel() && w->label()->Name == "out1");
        for (Node* n : sch->a_DocNodes) labelled = labelled || (n->hasLabel() && n->label()->Name == "out1");
        QVERIFY(labelled);
        QVERIFY(!failed(call("undo")));
        rect = sch->a_DocDiags.front();
        QCOMPARE(rect->Graphs.first()->Var, QStringLiteral("ngspice/tran.v(out)"));

        // A label taken away: the traces of its name are told of.
        const QJsonObject unlabel = call("set_label", {{"at", "R1.2"}, {"name", ""}});
        QVERIFY2(text(unlabel).contains("still show its voltage"), qPrintable(text(unlabel)));
        QVERIFY(!failed(call("undo")));

        // A type described: its properties in order, with units and
        // defaults, its netlist line, and the trap of a single pulse.
        const QJsonObject vpulse = json(call("describe_component_type", {{"type", "Vpulse"}})).toObject();
        const QJsonArray props = vpulse.value("properties").toArray();
        QCOMPARE(props.at(0).toObject().value("name").toString(), QStringLiteral("U1"));
        QCOMPARE(props.at(0).toObject().value("unit").toString(), QStringLiteral("V"));
        QCOMPARE(props.at(2).toObject().value("name").toString(), QStringLiteral("T1"));
        QCOMPARE(props.at(2).toObject().value("unit").toString(), QStringLiteral("s"));
        QVERIFY(vpulse.value("notes").toArray().first().toString().contains("Vrect"));
        QVERIFY2(vpulse.value("netlist").toObject().value("with the defaults").toString().contains("PULSE"), qPrintable(QJsonDocument(vpulse).toJson()));
        QCOMPARE(vpulse.value("pins").toArray().size(), 2);
        const QJsonObject r = json(call("describe_component_type", {{"type", "R"}})).toObject();
        QCOMPARE(r.value("properties").toArray().first().toObject().value("unit").toString(), QStringLiteral("Ohm"));
        QVERIFY(failed(call("describe_component_type", {{"type", "NoSuchPart"}})));
        for (const QString& type : {QStringLiteral("Sub"), QStringLiteral(".TR"), QStringLiteral("Eqn"), QStringLiteral("GND"), QStringLiteral("_BJT")}) {
            const QJsonObject described = call("describe_component_type", {{"type", type}});
            QVERIFY2(!failed(described), qPrintable(type + ": " + text(described)));
        }

        // The netlist, as a simulation would write it now.
        const QString netlist = text(call("get_netlist", {{"numbered", true}}));
        QVERIFY2(netlist.contains("R1") && netlist.contains("tran"), qPrintable(netlist));
        QVERIFY2(netlist.contains("   1  "), qPrintable(netlist));
        QVERIFY(failed(call("get_netlist", {{"last", true}})));   // never simulated
    }

    // Every type of the library described - its netlist line made from a
    // part on no schematic - and the new tools given odd arguments at
    // random (seeded), on a copy of the schematic with its diagrams and
    // dataset: each answers, as a result or an error, and the schematic
    // stays whole.
    void theResultToolsTakeOddArguments()
    {
        int described = 0, netlisted = 0;
        for (const QJsonValue& v : json(call("list_component_types")).toArray()) {
            const QString type = v.toObject().value("type").toString();
            const QJsonObject r = call("describe_component_type", {{"type", type}});
            QVERIFY2(!failed(r), qPrintable(type + ": " + text(r)));
            ++described;
            if (json(r).toObject().contains("netlist")) ++netlisted;
        }
        QVERIFY2(described > 150 && netlisted > 100, qPrintable(QStringLiteral("%1 %2").arg(described).arg(netlisted)));

        QVERIFY(!failed(call("show_document", {{"path", dir.filePath("workspace/rc.sch")}})));
        const QString source = front()->documentText();
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("set_schematic", {{"text", source}})));
        QVERIFY(!failed(call("save_document", {{"as", "fuzz"}})));
        QFile::copy(dir.filePath("workspace/rc.dat.ngspice"), dir.filePath("workspace/fuzz.dat.ngspice"));
        Schematic* sch = front();

        QHash<QString, QStringList> keys;
        QHash<QString, QString> typeOf;   // "tool key": the argument's type, when its schema names one
        for (const QJsonValue& t : control->tools()) {
            const QJsonObject tool = t.toObject();
            const QJsonObject properties = tool.value("inputSchema").toObject().value("properties").toObject();
            keys.insert(tool.value("name").toString(), properties.keys());
            for (auto it = properties.begin(); it != properties.end(); ++it)
                if (it.value().toObject().value("type").isString())
                    typeOf.insert(tool.value("name").toString() + ' ' + it.key(), it.value().toObject().value("type").toString());
        }
        const QStringList tools = {"get_dataset", "add_diagram", "edit_diagram", "add_trace", "edit_trace", "reload_data",
                                   "rename_net", "describe_component_type", "get_netlist", "delete", "get_schematic",
                                   "add_marker", "edit_marker", "delete_marker", "describe_format", "export_netlist", "set_schematic",
                                   "add_painting", "edit_painting", "list_documents", "export_image", "set_simulator",
                                   "check_schematic", "move", "add_analysis", "undo_history", "find_library_component",
                                   "read_pdf", "edit_component", "add_component", "replace_component"};
        const QStringList words = {"", "out", "in", "v(out)", "tran.v(out)", "ngspice/tran.v(out)", "ac.v(out)", "x:y@z", "../up",
                                   "auto", "red", "#zzz", "rect", "smith", "tab", "timing", "3d", "histogram", "left", "right",
                                   "dash", "arrows", "top_left", "dB", "bandwidth", "rise_time", "crossings", "net1", "gnd",
                                   "R1", "Vpulse", "db_phase", "real_imaginary", "peak", "-3dB", "min", "crossing:0.5", "crossing:x",
                                   "non_default", "all", "cdl", "spice", "square", "#80ff0000", "fuzz.cir", "<Diagrams>\n</Diagrams>",
                                   "text", "arrow", "text_box", "table", "callout", "formula", "thd", "phase_margin", "gain", "png", "svg",
                                   "Eqn", "NutmegEq", ".NGMONTECARLO", "SpiceOptions", "y=1", "a|b", "a|1|2", "npn", "nmos", "dc",
                                   "distribution", "fft", "eye", "1-3", "selection",
                                   QString(3000, QLatin1Char('x'))};
        QRandomGenerator rng(11);
        std::function<QJsonValue(int)> odd = [&](int depth) -> QJsonValue {
            switch (rng.bounded(depth > 1 ? 7 : 9)) {
            case 0: return QJsonValue();
            case 1: return rng.bounded(2) == 1;
            case 2: {
                static const double numbers[] = {0, 1, 2, -1, 7, 1e-300, 1e308, -1e308, 2147483647.0, -2147483648.0, 0.5, 1e-6};
                return numbers[rng.bounded(int(std::size(numbers)))];
            }
            case 3:
            case 4: return words.at(rng.bounded(int(words.size())));
            case 5: return int(rng.bounded(5));
            case 6: return QJsonArray{};
            case 7: {
                QJsonArray a;
                for (int i = int(rng.bounded(4)); i > 0; --i) a.append(odd(depth + 1));
                return a;
            }
            default: {
                static const QStringList inner = {"label", "log", "auto", "from", "to", "step", "units", "diagram", "trace", "variable", "color",
                                                  "at", "marker", "label_offset", "name", "value", "expression", "min", "max", "Bf", "R"};
                QJsonObject o;
                for (int i = int(rng.bounded(4)); i > 0; --i) o.insert(inner.at(rng.bounded(int(inner.size()))), odd(depth + 1));
                return o;
            }
            }
        };
        // An odd value of the argument's own type, mostly: one of another
        // type is refused before the tool, which says so.
        const auto oddOf = [&](const QString& type) -> QJsonValue {
            for (int tries = 0; tries < 40; ++tries) {
                const QJsonValue v = odd(0);
                if (type == "string" ? v.isString()
                    : type == "number" ? v.isDouble()
                    : type == "integer" ? v.isDouble() && v.toDouble() == std::floor(v.toDouble())
                    : type == "boolean" ? v.isBool()
                    : type == "array"   ? v.isArray()
                    : type == "object"  ? v.isObject()
                                        : true)
                    return v;
            }
            return odd(0);
        };
        int answered = 0, refusedByType = 0;
        for (int i = 0; i < 600; ++i) {
            const QString tool = tools.at(rng.bounded(int(tools.size())));
            QJsonObject args;
            for (const QString& key : keys.value(tool))
                if (key != "path" && rng.bounded(10) < 6)
                    args.insert(key, rng.bounded(10) < 8 ? oddOf(typeOf.value(tool + ' ' + key)) : odd(0));
            const QJsonObject r = call(tool, args);
            QVERIFY2(r.contains("content"), qPrintable(tool + ' ' + QJsonDocument(args).toJson(QJsonDocument::Compact)));
            QVERIFY(front() == sch);
            if (!failed(r)) ++answered;
            else if (text(r).contains("Nothing was done (it would have been read as its default, not refused)")) ++refusedByType;
        }
        // Not all refused: one call in ten answered at least. (The share is
        // the draw's: over seeds 1 to 7 and 11, 101 to 122 of the 600 on
        // macOS, and the Linux CI answered 100 - over the bar of 100 it had,
        // it failed there every run.)
        QVERIFY2(answered >= 60 && refusedByType > 20, qPrintable(QStringLiteral("%1 %2").arg(answered).arg(refusedByType)));
        // Still a schematic: read, undone and redone step by step.
        QVERIFY(!failed(call("get_schematic")));
        for (int i = 0; i < 25; ++i) call("undo");
        for (int i = 0; i < 25; ++i) call("redo");
        QVERIFY(sch->documentText().startsWith("<Qucs Schematic"));
        QVERIFY(!failed(call("get_schematic")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A curve in dB (a Nutmeg equation's db(...), written as complex
    // numbers with no imaginary part) keeps its sign and is measured in
    // dB; a curve that goes below 0 and is not in dB is not given a
    // bandwidth; an op analysis's values are an operating point, each
    // device's under its component. Markers are placed where asked - at
    // an x, the peak, 3 dB below it, a crossing - listed, changed and
    // deleted, each one step to undo.
    void decibelsTheOperatingPointAndMarkers()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "NutmegEq"}, {"name", "NutmegEq1"}, {"x", 100}, {"y", 300},
                                               {"properties", QJsonObject{{"y", "db(norm(v(out)))"}}}})));
        QVERIFY(!failed(call("save_document", {{"as", "lowpass"}})));
        {
            QFile f(dir.filePath("workspace/lowpass.dat.ngspice"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream d(&f);
            d << "<Qucs Dataset 26.1.3>\n<indep v(out) 1>\n0.5\n</indep>\n<indep @r1[i] 1>\n1e-3\n</indep>\n<indep @r1[p] 1>\n2e-3\n</indep>\n";
            d << "<indep frequency 401>\n";
            for (int i = 0; i <= 400; ++i) d << QString::number(std::pow(10.0, i / 50.0), 'e', 12) << "\n";
            d << "</indep>\n<dep ac.v(out) frequency>\n";
            const auto mag = [](int i) { return 1 / std::sqrt(1 + std::pow(std::pow(10.0, i / 50.0) / 1e5, 2)); };
            for (int i = 0; i <= 400; ++i) d << QString::number(mag(i), 'e', 12) << "-j" << QString::number(1e-4, 'e', 12) << "\n";
            d << "</dep>\n<dep ac.y frequency>\n";
            for (int i = 0; i <= 400; ++i) d << QString::number(20 * std::log10(mag(i)), 'e', 12) << "+j0.000000000000e+00\n";
            d << "</dep>\n<dep ac.swing frequency>\n";
            for (int i = 0; i <= 400; ++i) d << QString::number(std::cos(i / 20.0), 'e', 12) << "+j0.000000000000e+00\n";
            d << "</dep>\n";
        }

        const QJsonObject listed = json(call("get_dataset")).toObject();
        QJsonObject y;
        for (const QJsonValue& v : listed.value("variables").toArray())
            if (v.toObject().value("name").toString() == "ac.y") y = v.toObject();
        QVERIFY2(y.value("real").toBool() && !y.contains("complex"), QJsonDocument(listed).toJson().constData());
        QCOMPARE(y.value("units").toString(), QStringLiteral("dB"));
        QCOMPARE(y.value("defined as").toString(), QStringLiteral("db(norm(v(out)))"));
        QVERIFY(y.value("max").toDouble() <= 0.01 && y.value("min").toDouble() < -30);   // falling
        const QJsonObject op = listed.value("operating point").toObject();
        QCOMPARE(op.value("nodes").toObject().value("v(out)").toDouble(), 0.5);
        const QJsonObject r1 = op.value("devices").toArray().first().toObject();
        QCOMPARE(r1.value("component").toString(), QStringLiteral("R1"));
        QCOMPARE(r1.value("values").toObject().value("i").toDouble(), 1e-3);
        QCOMPARE(op.value("units").toObject().value("i").toString(), QStringLiteral("A"));
        QCOMPARE(listed.value("independent variables").toArray().size(), 1);   // (only frequency)

        const auto bandwidth = [&](const QJsonObject& args) {
            return json(call("get_dataset", args)).toObject().value("variables").toArray().first().toObject()
                .value("measurements").toObject().value("bandwidth").toObject();
        };
        const QJsonObject inDb = bandwidth({{"variables", QJsonArray{"y"}}, {"measure", QJsonArray{"bandwidth"}}});
        QVERIFY2(std::abs(inDb.value("value").toDouble() - 1e5) < 2e3, QJsonDocument(inDb).toJson().constData());
        const QJsonObject magnitude = bandwidth({{"variables", QJsonArray{"ac.v(out)"}}, {"measure", QJsonArray{"bandwidth"}}});
        QVERIFY(std::abs(magnitude.value("value").toDouble() - 1e5) < 2e3);
        QVERIFY(bandwidth({{"variables", QJsonArray{"ac.swing"}}, {"measure", QJsonArray{"bandwidth"}}}).value("error").toString().contains("below 0"));
        QVERIFY(!bandwidth({{"variables", QJsonArray{"ac.swing"}}, {"measure", QJsonArray{"bandwidth"}}, {"decibels", true}}).contains("error"));
        // v(out): the operating point's, and the AC analysis's.
        const QJsonArray vout = json(call("get_dataset", {{"variables", QJsonArray{"v(out)"}}, {"points", 0}})).toObject().value("variables").toArray();
        QCOMPARE(vout.size(), 2);
        QVERIFY(vout.at(0).toObject().value("operating point").toBool());
        QCOMPARE(vout.at(1).toObject().value("name").toString(), QStringLiteral("ac.v(out)"));
        QCOMPARE(json(call("get_dataset", {{"operating_point", true}})).toObject().value("operating point").toObject()
                     .value("devices").toArray().size(), 1);

        // Markers.
        QVERIFY(!failed(call("add_diagram", {{"x", 400}, {"y", 400}, {"traces", QJsonArray{"ac.y"}},
                                             {"x_axis", QJsonObject{{"log", true}}}})));
        const QJsonObject at3dB = json(call("add_marker", {{"at", "-3dB"}})).toObject();
        QVERIFY2(std::abs(at3dB.value("found").toObject().value("crossing").toDouble() - 1e5) < 2e3, QJsonDocument(at3dB).toJson().constData());
        QVERIFY(std::abs(at3dB.value("value").toDouble() + 3) < 0.3);
        QCOMPARE(at3dB.value("variable").toString(), QStringLiteral("ngspice/ac.y"));
        const auto markers = [&] {
            return json(call("get_schematic")).toObject().value("diagrams").toArray().first().toObject().value("markers").toArray();
        };
        QCOMPARE(markers().size(), 1);
        const QJsonObject peak = json(call("edit_marker", {{"marker", 1}, {"at", "peak"}, {"precision", 5}, {"label", QJsonArray{450, 250}}})).toObject();
        QVERIFY2(peak.value("value").toDouble() > -0.1, QJsonDocument(peak).toJson().constData());
        QCOMPARE(peak.value("label").toArray(), (QJsonArray{450, 250}));
        QCOMPARE(peak.value("precision").toInt(), 5);
        const QJsonObject styled = json(call("add_marker", {{"at", 3e4}, {"fill_color", "#ffeecc"}, {"text_color", "navy"},
                                                            {"indicator", "square"}, {"format", "magnitude_degrees"}}))
                                       .toObject();
        QCOMPARE(styled.value("marker").toInt(), 2);
        QCOMPARE(styled.value("fill_color").toString(), QStringLiteral("#ffeecc"));
        QCOMPARE(styled.value("text_color").toString(), QStringLiteral("#000080"));
        QCOMPARE(styled.value("indicator").toString(), QStringLiteral("square"));
        QVERIFY(std::abs(json(call("add_marker", {{"at", "crossing:-10"}})).toObject().value("found").toObject().value("crossing").toDouble() - 3e5) < 1e4);
        // Its numbers' notation: the diagram's by default, its own when
        // given - every number of its text - and the diagram's set by
        // edit_diagram, with its decimals.
        QCOMPARE(styled.value("notation").toString(), QStringLiteral("diagram"));
        const QJsonObject own = json(call("edit_marker", {{"marker", 2}, {"notation", "scientific"}})).toObject();
        QVERIFY2(own.value("notation").toString() == "scientific" && own.value("text").toString().startsWith("frequency: 3.020e4\n"),
                 QJsonDocument(own).toJson().constData());
        QVERIFY(failed(call("edit_marker", {{"marker", 2}, {"notation", "hex"}})));
        QVERIFY(!failed(call("edit_diagram", {{"notation", "power_of_ten"}, {"decimals", 2}})));
        QJsonObject shown = json(call("get_schematic")).toObject().value("diagrams").toArray().first().toObject();
        QVERIFY2(shown.value("notation").toString() == "power_of_ten" && shown.value("decimals").toInt() == 2,
                 QJsonDocument(shown).toJson().constData());
        QVERIFY2(markers().at(2).toObject().value("text").toString().contains(QString::fromUtf8("×10⁵")), QJsonDocument(markers()).toJson().constData());
        QVERIFY(markers().at(1).toObject().value("text").toString().startsWith("frequency: 3.020e4\n"));   // its own stays
        for (const QJsonObject& bad : {QJsonObject{{"decimals", 16}}, QJsonObject{{"decimals", 1.5}}, QJsonObject{{"notation", "diagram"}}})
            QVERIFY2(failed(call("edit_diagram", bad)), QJsonDocument(bad).toJson().constData());
        QVERIFY(!failed(call("edit_marker", {{"marker", 2}, {"notation", "diagram"}})));
        QVERIFY(markers().at(1).toObject().value("text").toString().contains(QString::fromUtf8("×10⁴")));
        QVERIFY(!failed(call("edit_diagram", {{"notation", "engineering"}, {"decimals", -1}})));
        // (The hunt after round 9, C2.) Precision as the dialog takes it,
        // 0 to 12; out of it, or not whole, refused - 99 was 12 and 2.7 was
        // 1, without a word. (C3) A table has no axes: no notation.
        QVERIFY(!failed(call("edit_marker", {{"marker", 2}, {"precision", 0}})));
        QCOMPARE(markers().at(1).toObject().value("precision").toInt(), 0);
        for (const QJsonValue& bad : {QJsonValue(13), QJsonValue(-1), QJsonValue(2.7), QJsonValue("5")}) {
            const QJsonObject r = call("edit_marker", {{"marker", 2}, {"precision", bad}});
            QVERIFY2(failed(r) && (text(r).contains("'precision' is a whole number from 0 to 12") || text(r).contains("precision is a whole number")),
                     qPrintable(text(r)));
        }
        QCOMPARE(markers().at(1).toObject().value("precision").toInt(), 0);
        QVERIFY(!failed(call("edit_marker", {{"marker", 2}, {"precision", 3}})));
        const QJsonObject table = json(call("add_diagram", {{"type", "tab"}, {"x", 100}, {"y", 900}, {"traces", QJsonArray{"v(out)"}}})).toObject();
        QVERIFY2(!table.isEmpty(), qPrintable(QJsonDocument(table).toJson()));
        for (const QJsonObject& bad : {QJsonObject{{"notation", "scientific"}}, QJsonObject{{"decimals", 2}}}) {
            QJsonObject args = bad;
            args.insert("diagram", table.value("diagram").toInt());
            const QJsonObject r = call("edit_diagram", args);
            QVERIFY2(failed(r) && text(r).contains("A table has no axes: 'notation' and 'decimals' are for diagrams with axes"), qPrintable(text(r)));
        }
        QVERIFY(!failed(call("delete", {{"diagrams", QJsonArray{table.value("diagram").toInt()}}})));
        for (const QJsonObject& bad : {QJsonObject{{"at", "nonsense"}}, QJsonObject{{"at", "crossing:x"}}, QJsonObject{{"at", "crossing:12"}},
                                       QJsonObject{{"at", 1}, {"format", "bad"}}, QJsonObject{{"at", 1}, {"fill_color", "#zz"}}, QJsonObject{},
                                       // (off the trace: it sat on the last sample; an offset of 2^31 - 1 overflowed)
                                       QJsonObject{{"at", 1e308}}, QJsonObject{{"at", 3e4}, {"label_offset", QJsonArray{2147483647.0, 2147483647.0}}}})
            QVERIFY2(failed(call("add_marker", bad)), QJsonDocument(bad).toJson().constData());
        QVERIFY(failed(call("delete_marker", {{"marker", 9}})));
        QCOMPARE(markers().size(), 3);
        QVERIFY(!failed(call("delete_marker", {{"marker", 3}})));
        QCOMPARE(markers().size(), 2);
        QVERIFY(!failed(call("undo")));
        QCOMPARE(markers().size(), 3);

        // Diagrams replaced as text: what was read, markers and all.
        const QString text = sch->documentText();
        const QString diagrams = text.mid(text.indexOf("<Diagrams>"), text.indexOf("</Diagrams>") + 11 - text.indexOf("<Diagrams>"));
        const QJsonObject replaced = json(call("set_schematic", {{"text", diagrams}})).toObject();
        QCOMPARE(replaced.value("replaced").toArray(), (QJsonArray{"diagrams"}));
        QCOMPARE(replaced.value("diagrams").toArray().first().toObject().value("markers").toArray().size(), 3);
        QVERIFY(replaced.value("diagrams").toArray().first().toObject().value("traces").toArray().first().toObject().value("points").toInt() > 400);
        QVERIFY(QucsControl::textOf(call("describe_format", {{"element", "diagram"}})).contains("legend"));
        QVERIFY(QucsControl::textOf(call("describe_format")).contains("<Mkr"));
        QVERIFY(failed(call("describe_format", {{"element", "sprocket"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A conversation is told what changed since its last call that it did
    // not change itself: the user's edits, another conversation's, a
    // simulation run again, documents opened and closed - once. get_state
    // gives each document's revision and who made its last edit.
    void theToolsTellWhatChangedSinceTheLastCall()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", "watched"}})));
        const QString path = dir.filePath("workspace/watched.sch");
        const quint64 us = 101, other = 202;
        const auto as = [this](quint64 who, const QString& tool, const QJsonObject& args = {}) { return control->callNow(tool, args, 30000, who); };
        const auto entry = [&](const QJsonObject& state) {
            for (const QJsonValue& d : json(state).toObject().value("documents").toArray())
                if (d.toObject().value("path").toString() == path) return d.toObject();
            return QJsonObject();
        };
        const qint64 first = entry(as(us, "get_state")).value("revision").toInteger();
        QVERIFY(first >= 1);

        // Its own edit: not told back.
        QVERIFY(!failed(as(us, "edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "2k"}}}})));
        QJsonObject r = as(us, "get_state");
        QVERIFY2(!text(r).contains("Since your last call"), qPrintable(text(r)));
        QCOMPARE(entry(r).value("last edit").toObject().value("by").toString(), QStringLiteral("you"));
        QVERIFY(entry(r).value("revision").toInteger() > first);

        // The user's edit (no conversation's call): told once.
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "3k"}}}})));
        r = as(us, "get_schematic");
        QVERIFY2(text(r).contains("Since your last call") && text(r).contains("changed by the user"), qPrintable(text(r).right(400)));
        r = as(us, "get_state");
        QVERIFY(!text(r).contains("Since your last call"));
        QCOMPARE(entry(r).value("last edit").toObject().value("by").toString(), QStringLiteral("the user"));

        // Another conversation's.
        QVERIFY(!failed(as(other, "edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "4k"}}}})));
        r = as(us, "get_state");
        QVERIFY2(text(r).contains("another conversation"), qPrintable(text(r).right(400)));
        QCOMPARE(entry(r).value("last edit").toObject().value("by").toString(), QStringLiteral("another conversation"));

        // A simulation run again: its dataset written anew.
        {
            QFile f(dir.filePath("workspace/watched.dat.ngspice"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("<Qucs Dataset 26.1.3>\n<indep time 2>\n0\n1\n</indep>\n");
        }
        r = as(us, "get_state");
        QVERIFY2(text(r).contains("simulated again"), qPrintable(text(r).right(400)));
        QVERIFY(entry(r).contains("dataset written"));
        QVERIFY(!text(as(us, "get_state")).contains("simulated again"));

        // Opened and closed; a text document just opened is no one's edit.
        QVERIFY(!failed(call("export_netlist", {{"save_as", "watched.cir"}})));
        QVERIFY(!failed(call("open_document", {{"path", "watched.cir"}})));
        r = as(us, "get_state");
        QVERIFY2(text(r).contains("watched.cir has been opened"), qPrintable(text(r).right(400)));
        for (const QJsonValue& d : json(r).toObject().value("documents").toArray())
            if (d.toObject().value("title").toString() == "watched.cir") {
                QCOMPARE(d.toObject().value("revision").toInteger(), 1);
                QVERIFY(!d.toObject().contains("last edit"));
            }
        QVERIFY(!failed(call("close_document", {{"path", "watched.cir"}})));
        QVERIFY(text(as(us, "get_state")).contains("watched.cir has been closed"));
        QVERIFY(!failed(call("close_document", {{"path", path}, {"unsaved", "discard"}})));
    }

    // The summary lists what differs from the defaults (a JFET has 27
    // properties); asked, all of them, or the parts named or in a region;
    // a schematic of more than 200 parts in part, saying what is left out.
    // It has the paintings and the settings of the file.
    void aSummaryIsAsLongAsAsked()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "JFET"}, {"name", "T1"}, {"x", 200}, {"y", 200},
                                               {"properties", QJsonObject{{"Beta", "1e-2"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 600}, {"y", 200}})));
        QVERIFY(!failed(call("set_schematic", {{"text", "<Paintings>\n  <Text 100 50 12 #000000 0 \"a note\">\n</Paintings>\n"}})));
        const auto names = [](const QJsonObject& component) {
            QStringList list;
            for (const QJsonValue& p : component.value("properties").toArray()) list << p.toObject().value("name").toString();
            return list;
        };
        QJsonObject summary = json(call("get_schematic")).toObject();
        QStringList t1 = names(componentIn(summary, "T1"));
        QVERIFY2(t1.contains("Beta") && !t1.contains("Kf"), qPrintable(t1.join(',')));
        QVERIFY(summary.value("left out").toString().contains("defaults"));
        const QJsonObject note = summary.value("paintings").toArray().first().toObject();
        QCOMPARE(note.value("painting").toInt(), 1);
        QCOMPARE(note.value("type").toString(), QStringLiteral("text"));
        QCOMPARE(note.value("text").toString(), QStringLiteral("a note"));
        QCOMPARE(note.value("x").toInt(), 100);
        QVERIFY(summary.value("settings").toObject().contains("dataset"));
        t1 = names(componentIn(json(call("get_schematic", {{"properties", "all"}})).toObject(), "T1"));
        QVERIFY(t1.contains("Kf") && t1.size() > 20);
        const QJsonObject shown = componentIn(json(call("get_schematic", {{"properties", "shown"}})).toObject(), "T1");
        QVERIFY(!shown.value("properties").toArray().isEmpty());
        for (const QJsonValue& p : shown.value("properties").toArray()) QVERIFY(p.toObject().value("shown").toBool());
        QCOMPARE(json(call("get_schematic", {{"components", QJsonArray{"R1"}}})).toObject().value("components").toArray().size(), 1);
        QCOMPARE(json(call("get_schematic", {{"region", QJsonArray{0, 0, 400, 400}}})).toObject().value("components").toArray().size(), 1);
        QVERIFY(failed(call("get_schematic", {{"properties", "some"}})));
        QVERIFY(failed(call("get_schematic", {{"region", QJsonArray{1, 2}}})));

        QString many;
        for (int i = 0; i < 250; ++i)
            many += QStringLiteral("  <R R%1 1 %2 %3 15 -26 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
                        .arg(i + 10).arg(100 * (i % 20)).arg(100 * (i / 20) + 400);
        QVERIFY(!failed(call("set_schematic", {{"text", "<Components>\n" + many + "</Components>\n"}})));
        // Read whole, more than 200 parts: the first 50; asked for by place,
        // 200 of them.
        summary = json(call("get_schematic")).toObject();
        QCOMPARE(summary.value("components").toArray().size(), 50);
        QVERIFY2(summary.value("left out").toString().contains("200 more components"), qPrintable(summary.value("left out").toString()));
        summary = json(call("get_schematic", {{"region", QJsonArray{-100, 0, 2100, 3000}}})).toObject();
        QCOMPARE(summary.value("components").toArray().size(), 200);
        QVERIFY2(summary.value("left out").toString().contains("50 more components"), qPrintable(summary.value("left out").toString()));
        // At a glance: the parts counted by type, most first.
        const QJsonObject overview = json(call("get_schematic", {{"format", "overview"}})).toObject();
        QCOMPARE(overview.value("components").toInt(), 250);   // (the section replaced them all)
        const QJsonObject most = overview.value("by type").toArray().first().toObject();
        QCOMPARE(most.value("type").toString(), QStringLiteral("R"));
        QCOMPARE(most.value("count").toInt(), 250);
        QCOMPARE(overview.value("extent").toArray().size(), 4);
        QVERIFY(overview.contains("next") && !overview.contains("pins"));
        QVERIFY(QJsonDocument(overview).toJson(QJsonDocument::Compact).size() < 1500);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A netlist written to a file, SPICE or CDL - the menu's Save netlist
    // and Save CDL netlist ask for the file in a dialog no tool can answer.
    void aNetlistIsWrittenToAFile()
    {
        QVERIFY(!failed(call("show_document", {{"path", dir.filePath("workspace/rc.sch")}})));
        QJsonObject written = json(call("export_netlist", {{"save_as", "rc.cir"}})).toObject();
        QFile spice(written.value("written").toString());
        QVERIFY2(spice.open(QIODevice::ReadOnly), QJsonDocument(written).toJson().constData());
        QVERIFY(spice.readAll().contains("R1"));
        written = json(call("export_netlist", {{"save_as", "rc.cdl"}, {"format", "cdl"}})).toObject();
        QCOMPARE(written.value("format").toString(), QStringLiteral("cdl"));
        QVERIFY(QFileInfo::exists(dir.filePath("workspace/rc.cdl")));
        QVERIFY(text(call("get_netlist", {{"format", "cdl"}})).contains("CDL"));
        QVERIFY(!text(call("get_netlist", {{"save_as", "not.cir"}})).isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath("workspace/not.cir")));   // get_netlist only looks
        QVERIFY(failed(call("export_netlist")));
        QVERIFY(failed(call("export_netlist", {{"save_as", dir.filePath("no/such/folder/x.cir")}})));
        QVERIFY(failed(call("get_netlist", {{"format", "verilog"}})));
        QVERIFY(!control->readOnlyTools().contains("export_netlist"));
    }

    // The panes: which there are, what each holds; a document moved to a
    // new pane beside its own, and back (the pane it leaves goes).
    void panesAreToldAndArranged()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("save_document", {{"as", "panes"}})));
        QVERIFY(!failed(call("export_netlist", {{"save_as", "panes.cir"}})));
        QVERIFY(!failed(call("open_document", {{"path", "panes.cir"}})));
        QJsonObject state = json(call("get_state")).toObject();
        QCOMPARE(state.value("panes").toArray().size(), 1);
        QVERIFY(state.value("panes").toArray().first().toObject().value("active").toBool());
        state = json(call("move_to_pane", {{"pane", "right"}})).toObject();
        QJsonArray panes = state.value("panes").toArray();
        QCOMPARE(panes.size(), 2);
        QCOMPARE(panes.at(1).toObject().value("column").toInt(), 2);
        QCOMPARE(panes.at(1).toObject().value("documents").toArray().size(), 1);
        QVERIFY(panes.at(1).toObject().value("active").toBool());
        QCOMPARE(panes.at(1).toObject().value("rectangle").toArray().size(), 4);
        // Alone in its pane now: it cannot leave it for a new one.
        QVERIFY(text(call("move_to_pane", {{"pane", "below"}})).contains("only document"));
        QVERIFY(failed(call("move_to_pane", {{"pane", 7}})));
        QVERIFY(failed(call("move_to_pane", {{"pane", "left"}})));
        state = json(call("move_to_pane", {{"pane", 1}})).toObject();
        QCOMPARE(state.value("panes").toArray().size(), 1);
        QVERIFY(!failed(call("close_document", {{"path", "panes.cir"}})));
        QVERIFY(!failed(call("close_document", {{"path", "panes.sch"}})));
    }

    // With ngspice (when there is one): a simulation that succeeds says so
    // and names the dataset it wrote; one that fails says where.
    void aRealSimulationIsReported()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        const QString docFile = dir.filePath("workspace/rc.sch");
        QVERIFY(!failed(call("show_document", {{"path", docFile}})));
        QFile::remove(dir.filePath("workspace/rc.dat.ngspice"));
        // An op analysis too; and a DC bias run's files, from before.
        QVERIFY(!failed(call("add_component", {{"type", ".DC"}, {"name", "DC1"}, {"x", 400}, {"y", 450}})));
        QVERIFY(!failed(call("save_document")));
        const QDir scratch(misc::scratchDirFor(docFile));
        QDir().mkpath(scratch.path());
        for (const char* stale : {"spice4qucs.cir.dc_op", "spice4qucs.cir.dc_op_dev"}) {
            QFile f(scratch.filePath(stale));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("a circuit that is no more\n");
        }

        QVERIFY(failed(call("simulate", {{"keep_as", "../elsewhere"}})));
        const QJsonObject result = call("simulate", {{"timeout", 60}, {"keep_as", "run1"}}, 90000);
        const QJsonObject outcome = json(result).toObject();
        QVERIFY2(outcome.value("succeeded").toBool(), qPrintable(text(result)));
        QVERIFY2(outcome.value("dataset written").toBool(), qPrintable(text(result)));
        QVERIFY(outcome.value("dataset").toString().endsWith("rc.dat.ngspice"));
        QVERIFY2(outcome.value("variables").toArray().contains(QJsonValue("tran.v(out)")), qPrintable(text(result)));
        QVERIFY(outcome.value("errors").toArray().isEmpty());
        QVERIFY2(!outcome.contains("traces without data"), qPrintable(text(result)));
        // What was left of an earlier run is gone; the op analysis's
        // devices are in the dataset, under their components.
        QVERIFY(!QFileInfo::exists(scratch.filePath("spice4qucs.cir.dc_op")));
        QVERIFY(!QFileInfo::exists(scratch.filePath("spice4qucs.cir.dc_op_dev")));
        const QJsonObject op = json(call("get_dataset", {{"operating_point", true}})).toObject().value("operating point").toObject();
        bool r1 = false;
        for (const QJsonValue& d : op.value("devices").toArray())
            r1 = r1 || (d.toObject().value("component").toString() == "R1" && d.toObject().value("values").toObject().contains("i"));
        QVERIFY2(r1, QJsonDocument(op).toJson().constData());
        QVERIFY2(op.value("nodes").toObject().contains("v(out)"), QJsonDocument(op).toJson().constData());
        const QJsonObject vout = json(call("get_dataset", {{"variables", QJsonArray{"out"}}, {"measure", QJsonArray{"rise_time"}}}))
                                     .toObject().value("variables").toArray().first().toObject();
        const double rise = vout.value("measurements").toObject().value("rise_time").toObject().value("value").toDouble();
        QVERIFY2(std::abs(rise - 1e-6 * std::log(9.0)) < 0.1e-6, qPrintable(QJsonDocument(vout).toJson()));
        QVERIFY(text(call("get_netlist", {{"last", true}})).contains("spice4qucs.cir"));
        // The run kept: read by its file, and shown beside the current one.
        QVERIFY2(outcome.value("kept as").toString().endsWith("run1.dat.ngspice"), qPrintable(text(result)));
        QVERIFY(!failed(call("get_dataset", {{"path", "run1.dat.ngspice"}, {"variables", QJsonArray{"out"}}})));
        const QJsonObject kept = json(call("add_trace", {{"diagram", 1}, {"variable", "run1:tran.v(out)"}, {"style", "dot"}})).toObject();
        QCOMPARE(kept.value("variable").toString(), QStringLiteral("ngspice/run1:tran.v(out)"));
        QVERIFY2(kept.value("points").toInt() > 100, QJsonDocument(kept).toJson().constData());
        // keep_as the schematic's own name: that is the run's dataset itself,
        // not kept over itself - it was deleted (bug hunt 2026-09-26, A1).
        for (const char* own : {"rc", "RC"}) {
            if (QString(own) == "RC" && !QFileInfo::exists(dir.filePath("workspace/RC.sch"))) continue;   // (case matters here)
            const QJsonObject self = json(call("simulate", {{"timeout", 60}, {"keep_as", own}}, 90000)).toObject();
            QVERIFY2(self.value("dataset written").toBool(), QJsonDocument(self).toJson().constData());
            QVERIFY2(self.value("kept as").toString().startsWith("not kept"), QJsonDocument(self).toJson().constData());
            QVERIFY(QFileInfo(dir.filePath("workspace/rc.dat.ngspice")).size() > 0);
            QVERIFY(!failed(call("get_dataset", {{"variables", QJsonArray{"out"}}})));
        }

        // A value ngspice cannot read: the error names the part.
        QVERIFY(!failed(call("edit_component", {{"name", "C1"}, {"properties", QJsonObject{{"C", "{nosuchparam}"}}}})));
        QVERIFY(!failed(call("save_document")));
        const QJsonObject bad = json(call("simulate", {{"timeout", 60}}, 90000)).toObject();
        QVERIFY(!bad.value("succeeded").toBool());
        bool named = false;
        for (const QJsonValue& e : bad.value("errors").toArray())
            named = named || e.toObject().value("component").toString() == "C1";
        QVERIFY2(named, QJsonDocument(bad).toJson().constData());
        QVERIFY(!failed(call("undo")));
        QVERIFY(!failed(call("delete", {{"names", QJsonArray{"DC1"}}})));
        QVERIFY(!failed(call("save_document")));
        QucsSettings.NgspiceExecutable = before;
    }

    // A conversation pinned to a schematic (forDocument()): the tools that
    // act on the document in front act on it when given none, whichever is
    // in front; one given a path, open_document, show_document and
    // reload_data are as they were; get_state names it; a menu action
    // brings it to the front first; saved under another name, it stays
    // pinned. Not pinned, nothing changes.
    void aPinnedConversationWorksOnItsSchematic()
    {
        const auto made = [this](const QString& name) {
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            QVERIFY(!failed(call("save_document", {{"as", name}})));
        };
        made("pinned_a");
        made("pinned_b");   // (in front)
        const QString a = QFileInfo(dir.filePath("workspace/pinned_a.sch")).canonicalFilePath();
        const QString b = QFileInfo(dir.filePath("workspace/pinned_b.sch")).canonicalFilePath();
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        QCOMPARE(QFileInfo(front()->getDocName()).canonicalFilePath(), b);

        // What the tools are given.
        const QJsonObject resistor{{"type", "R"}, {"x", 100}, {"y", 100}};
        QCOMPARE(control->forDocument("add_component", resistor, a).value("path").toString(), a);
        QCOMPARE(control->forDocument("add_component", resistor, QString()), resistor);   // not pinned
        QJsonObject named = resistor;
        named.insert("path", b);
        QCOMPARE(control->forDocument("add_component", named, a).value("path").toString(), b);
        for (const char* own : {"get_schematic", "simulate", "get_dataset", "save_document", "undo", "screenshot",
                                "trigger_action", "get_state", "add_trace", "rename_net"})
            QVERIFY2(control->forDocument(own, {}, a).value("path").toString() == a, own);
        for (const char* other : {"open_document", "show_document", "reload_data", "new_document", "list_actions",
                                  "list_component_types", "get_dialog", "describe_component_type"})
            QVERIFY2(!control->forDocument(other, {}, a).contains("path"), other);

        // Changes go to it, with the other in front.
        QVERIFY(!failed(call("add_component", control->forDocument("add_component", resistor, a))));
        const auto open = [this](const QString& file) -> Schematic* {
            for (QucsDoc* doc : app->allDocuments())
                if (QFileInfo(doc->getDocName()).canonicalFilePath() == QFileInfo(file).canonicalFilePath())
                    return dynamic_cast<Schematic*>(doc);
            return nullptr;
        };
        const auto has = [&open](const QString& file, const char* part) {
            Schematic* sch = open(file);
            return sch != nullptr && sch->getComponentByName(part) != nullptr;
        };
        QVERIFY(has(a, "R1"));
        QVERIFY(!has(b, "R1"));
        const QJsonObject summary = json(call("get_schematic", control->forDocument("get_schematic", {}, a))).toObject();
        QVERIFY(!componentIn(summary, "R1").isEmpty());

        // get_state names it.
        const QJsonObject state = json(call("get_state", control->forDocument("get_state", {}, a))).toObject();
        QCOMPARE(QFileInfo(state.value("this conversation works on").toString()).canonicalFilePath(), a);
        int marked = 0;
        for (const QJsonValue& d : state.value("documents").toArray())
            if (d.toObject().value("this conversation's document").toBool()) {
                ++marked;
                QCOMPARE(QFileInfo(d.toObject().value("path").toString()).canonicalFilePath(), a);
            }
        QCOMPARE(marked, 1);
        QVERIFY(!json(call("get_state")).toObject().contains("this conversation works on"));

        // A menu action: on it, brought to the front first.
        QVERIFY(!failed(call("show_document", {{"path", b}})));
        QVERIFY(!failed(call("trigger_action", control->forDocument("trigger_action", {{"action", "View > View All"}}, a))));
        QCOMPARE(QFileInfo(front()->getDocName()).canonicalFilePath(), a);
        QCOMPARE(control->subjectOf("trigger_action", control->forDocument("trigger_action", {{"action", "View > View All"}}, a)),
                 QStringLiteral("View > View All on pinned_a.sch"));
        QVERIFY(failed(call("trigger_action", {{"action", "View > View All"}, {"path", "no_such.sch"}})));

        // Pinned to one not open: said so.
        QVERIFY(!failed(call("close_document", {{"path", b}})));
        QVERIFY(text(call("add_component", control->forDocument("add_component", resistor, b))).contains("not open"));
        QVERIFY(json(call("get_state", control->forDocument("get_state", {}, b))).toObject()
                    .value("this conversation works on").toString().contains("not open"));

        // The dock's conversation pinned to it follows a Save As.
        ClaudeCodeTabs* tabs = app->claudeCode();
        ClaudeCodePanel* panel = tabs->current();
        QVERIFY(!failed(call("show_document", {{"path", a}})));
        QCOMPARE(panel->pinnableDocument(), open(a)->getDocName());
        panel->pinButton()->click();
        QVERIFY(panel->isPinnedTo(a));
        QCOMPARE(panel->session()->document(), panel->pinnedDocument());
        QVERIFY(!failed(call("save_document", {{"path", a}, {"as", "pinned_a2"}})));
        QVERIFY(panel->isPinnedTo(dir.filePath("workspace/pinned_a2.sch")));
        QVERIFY(panel->session()->document().endsWith("pinned_a2.sch"));
        panel->pinDocument(QString());
        QVERIFY(panel->session()->document().isEmpty());
        open(dir.filePath("workspace/pinned_a2.sch"))->setChanged(false);
    }

    // Quick: several calls in one (batch) - in order, each change a step to
    // undo, stopped at one that fails unless keep_going, a batch not held
    // in another, images kept, the pinned document given to each; a menu
    // action answered as soon as it is over; the answers compact JSON.
    void aBatchRunsManyCallsInOne()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("save_document", {{"as", "batched"}})));
        const QJsonObject r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"x", 100}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "C"}, {"x", 300}, {"y", 100}, {"rotation", 90}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "GND"}, {"x", 300}, {"y", 200}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "C1.1"}}}},
            QJsonObject{{"tool", "get_schematic"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString all = text(r);
        QVERIFY2(all.startsWith("5 of 5 calls done."), qPrintable(all.left(200)));
        QVERIFY(all.contains("[1] add_component:"));
        QVERIFY(all.contains("[5] get_schematic:"));
        QVERIFY(front()->getComponentByName("R1") && front()->getComponentByName("C1"));
        // Each change a step to undo, as alone: four undo it all.
        for (int i = 0; i < 4; ++i) QVERIFY(!failed(call("undo")));
        QVERIFY(front()->getComponentByName("R1") == nullptr);
        QVERIFY(front()->getComponentByName("C1") == nullptr);
        for (int i = 0; i < 4; ++i) QVERIFY(!failed(call("redo")));
        QVERIFY(front()->getComponentByName("C1") != nullptr);

        // Stopped at a failure; keep_going goes on.
        const QJsonArray three{QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"x", 100}, {"y", 300}}}},
                               QJsonObject{{"tool", "edit_component"}, {"arguments", QJsonObject{{"name", "NoSuch"}, {"properties", QJsonObject{{"R", "1"}}}}}},
                               QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "L"}, {"x", 400}, {"y", 300}}}}};
        const QJsonObject stopped = call("batch", {{"calls", three}});
        QVERIFY(failed(stopped));
        QVERIFY2(text(stopped).startsWith("1 of 3 calls done, 1 failed; stopped there"), qPrintable(text(stopped).left(200)));
        QVERIFY(text(stopped).contains("[2] edit_component failed:"));
        QVERIFY(front()->getComponentByName("L1") == nullptr);
        const QJsonObject going = call("batch", {{"calls", three}, {"keep_going", true}});
        QVERIFY(text(going).startsWith("2 of 3 calls done, 1 failed."));
        QVERIFY(front()->getComponentByName("L1") != nullptr);

        // Not within another; not empty; a screenshot's image kept; a menu
        // action (answered later) in its turn.
        const QJsonObject inner{{"tool", "batch"}, {"arguments", QJsonObject{{"calls", QJsonArray{}}}}};
        QVERIFY(text(call("batch", {{"calls", QJsonArray{inner}}})).contains("cannot hold another"));
        QVERIFY(failed(call("batch", {{"calls", QJsonArray{}}})));
        const QJsonObject mixed = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "trigger_action"}, {"arguments", QJsonObject{{"action", "View > View All"}}}},
            QJsonObject{{"tool", "screenshot"}},
            QJsonObject{{"tool", "get_state"}}}}});
        QVERIFY2(!failed(mixed), qPrintable(text(mixed)));
        bool image = false;
        for (const QJsonValue& v : mixed.value("content").toArray()) image = image || v.toObject().value("type").toString() == "image";
        QVERIFY(image);
        QVERIFY(text(mixed).contains("View All: done."));

        // Pinned: each call given the document; the permission card sums it up.
        const QJsonObject batch{{"calls", QJsonArray{QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}}}},
                                                     QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}}}},
                                                     QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{}}},
                                                     QJsonObject{{"tool", "list_component_types"}}}}};
        const QJsonObject pinned = control->forDocument("batch", batch, "/w/amp.sch");
        const QJsonArray calls = pinned.value("calls").toArray();
        QCOMPARE(calls.at(0).toObject().value("arguments").toObject().value("path").toString(), QStringLiteral("/w/amp.sch"));
        QCOMPARE(calls.at(2).toObject().value("arguments").toObject().value("path").toString(), QStringLiteral("/w/amp.sch"));
        QVERIFY(!calls.at(3).toObject().value("arguments").toObject().contains("path"));
        QCOMPARE(control->subjectOf("batch", batch), QStringLiteral("add_component ×2, connect, list_component_types"));
        QVERIFY(!control->readOnlyTools().contains("batch"));

        // A menu action without a dialog: answered at once, not half a
        // second later; the answers compact.
        QElapsedTimer clock;
        clock.start();
        QVERIFY(!failed(call("trigger_action", {{"action", "View > View All"}})));
        QVERIFY2(clock.elapsed() < 300, qPrintable(QString::number(clock.elapsed())));
        const QString state = text(call("get_state"));
        QVERIFY(!state.contains("\n    "));
        QVERIFY(json(call("get_state")).isObject());
        front()->setChanged(false);
        QVERIFY(!failed(call("close_document")));
    }

    // Pinned calls among everything else - schematics opened, closed,
    // brought forward, saved, the pin moved or on one that is closed - in
    // any order: a part added goes to the pinned schematic and nowhere
    // else, one closed is said to be, nothing breaks (ASan).
    void pinnedCallsSurviveAnything()
    {
        QStringList files;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            QVERIFY(!failed(call("save_document", {{"as", QStringLiteral("fuzzpin_%1").arg(i)}})));
            files << QFileInfo(dir.filePath(QStringLiteral("workspace/fuzzpin_%1.sch").arg(i))).canonicalFilePath();
        }
        const auto open = [this](const QString& file) -> Schematic* {
            for (QucsDoc* doc : app->allDocuments())
                if (QFileInfo(doc->getDocName()).canonicalFilePath() == file) return dynamic_cast<Schematic*>(doc);
            return nullptr;
        };
        const auto parts = [&open, &files] {
            QList<int> n;
            for (const QString& f : files) n << (open(f) != nullptr ? int(open(f)->a_DocComps.size()) : -1);
            return n;
        };
        std::mt19937 rng(20260926);
        const auto pick = [&rng](int n) { return int(rng() % unsigned(n)); };
        int landed = 0;
        for (int round = 0; round < 300; ++round) {
            const int which = pick(4);   // 3: none
            const QString pinned = which < 3 ? files.at(which) : QString();
            const auto given = [&](const QString& tool, const QJsonObject& a) { return control->forDocument(tool, a, pinned); };
            const QList<int> before = parts();
            switch (pick(10)) {
            case 0: case 1: {
                const QJsonObject r = call("add_component", given("add_component", {{"type", "R"}, {"x", 20 * pick(40)}, {"y", 20 * pick(30)}}));
                if (pinned.isEmpty()) break;
                const QList<int> after = parts();
                if (open(pinned) == nullptr) {
                    QVERIFY(failed(r));
                    QVERIFY(text(r).contains("not open"));
                    QCOMPARE(after, before);
                    break;
                }
                QVERIFY2(!failed(r), qPrintable(text(r)));
                for (int i = 0; i < 3; ++i) QCOMPARE(after.at(i), before.at(i) + (i == which ? 1 : 0));
                ++landed;
                break;
            }
            case 2: call("get_schematic", given("get_schematic", {})); break;
            case 3: call("undo", given("undo", {})); break;
            case 4: call("trigger_action", given("trigger_action", {{"action", pick(2) ? "View > View All" : "Edit > Select All"}})); break;
            case 5: call("close_document", {{"path", files.at(pick(3))}, {"unsaved", "discard"}}); break;
            case 6: call("open_document", {{"path", files.at(pick(3))}}); break;
            case 7: call("show_document", {{"path", files.at(pick(3))}}); break;
            case 8: call("get_state", given("get_state", {})); break;
            default: call("save_document", given("save_document", {})); break;
            }
        }
        QVERIFY2(landed > 20, qPrintable(QString::number(landed)));
        for (const QString& f : files)
            if (Schematic* sch = open(f)) sch->setChanged(false);
    }

    // Paintings drawn and changed by their named fields - a text, an
    // arrow, a callout, a table, a dimension, a formula, a star, a filled
    // rectangle, a polygon, a picture - each one step to undo, listed by
    // their fields and written as the file has them; what a type does not
    // take, a wrong colour or style, a missing place refused, nothing
    // changed; selected and deleted by their numbers.
    void paintingsAreDrawnAndChangedByTheirFields()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        const auto add = [this](const QJsonObject& args) {
            const QJsonObject r = call("add_painting", args);
            if (failed(r)) qWarning("%s", qPrintable(text(r)));
            return json(r).toObject().value("added").toObject();
        };
        QJsonObject p = add({{"type", "text"}, {"x", 253}, {"y", 762}, {"size", 10}, {"text", "BW = 1.596 kHz"}});
        QCOMPARE(p.value("painting").toInt(), 1);
        QCOMPARE(p.value("type").toString(), QStringLiteral("text"));
        QCOMPARE(p.value("text").toString(), QStringLiteral("BW = 1.596 kHz"));
        p = add({{"type", "arrow"}, {"from", QJsonArray{302, 758}}, {"to", QJsonArray{207, 758}}, {"head", "filled"}, {"color", "#0000ff"}});
        QCOMPARE(p.value("painting").toInt(), 2);
        QCOMPARE(p.value("to").toArray(), (QJsonArray{207, 758}));
        QCOMPARE(p.value("head").toString(), QStringLiteral("filled"));
        const QString file = sch->documentText();
        QVERIFY2(file.contains("<Text 253 762 10 #000000 0 \"BW = 1.596 kHz\">"), qPrintable(file));
        QVERIFY2(file.contains("<Arrow 302 758 -95 0 20 8 #0000ff 1 1 1>"), qPrintable(file));
        p = add({{"type", "text_box"}, {"kind", "callout"}, {"x", 400}, {"y", 100}, {"width", 120}, {"height", 40},
                 {"text", "crossover\ndistortion"}});
        QCOMPARE(p.value("kind").toString(), QStringLiteral("callout"));
        QVERIFY(p.value("pointer").toBool());
        QCOMPARE(p.value("tip").toArray(), (QJsonArray{360, 180}));   // below left of it, as a click puts it
        QCOMPARE(p.value("text").toString(), QStringLiteral("crossover\ndistortion"));
        p = add({{"type", "table"}, {"x", 600}, {"y", 100}, {"rows", 2}, {"columns", 2},
                 {"cells", QJsonArray{QJsonArray{"f", "gain"}, QJsonArray{"1 kHz", "0 dB"}}}});
        QCOMPARE(p.value("cells").toArray().at(1).toArray().at(0).toString(), QStringLiteral("1 kHz"));
        QVERIFY(p.value("width").toInt() > 1);   // its own size, none given
        p = add({{"type", "dimension"}, {"from", QJsonArray{0, 300}}, {"to", QJsonArray{200, 300}}, {"unit", "mm"}, {"scale", 0.1}});
        QCOMPARE(p.value("unit").toString(), QStringLiteral("mm"));
        QCOMPARE(p.value("to").toArray(), (QJsonArray{200, 300}));
        p = add({{"type", "formula"}, {"x", 100}, {"y", 400}, {"tex", "f_c = \\frac{1}{2\\pi RC}"}});
        QCOMPARE(p.value("tex").toString(), QStringLiteral("f_c = \\frac{1}{2\\pi RC}"));
        p = add({{"type", "polygon"}, {"x", 0}, {"y", 0}, {"star", true}, {"sides", 5}, {"angle", 90}});
        QVERIFY(p.value("star").toBool());
        QCOMPARE(p.value("angle").toInt(), 90);
        p = add({{"type", "rectangle"}, {"x", 10}, {"y", 20}, {"width", 30}, {"height", 40}, {"filled", true},
                 {"fill_color", "#ffcc00"}, {"style", "dash"}});
        QCOMPARE(p.value("fill_color").toString(), QStringLiteral("#ffcc00"));
        QCOMPARE(p.value("style").toString(), QStringLiteral("dash"));
        QVERIFY(p.value("filled").toBool());
        p = add({{"type", "polyline"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{10, 10}, QJsonArray{20, 0}}}, {"closed", true}});
        QCOMPARE(p.value("points").toArray().size(), 3);
        QVERIFY(p.value("closed").toBool());
        QCOMPARE(p.value("painting").toInt(), 9);
        QCOMPARE(int(sch->a_DocPaints.size()), 9);

        // Listed by their fields, numbered.
        const QJsonArray listed = json(call("get_schematic")).toObject().value("paintings").toArray();
        QCOMPARE(listed.size(), 9);
        QCOMPARE(listed.at(1).toObject().value("type").toString(), QStringLiteral("arrow"));
        QCOMPARE(listed.at(3).toObject().value("rows").toInt(), 2);
        QCOMPARE(listed.at(8).toObject().value("painting").toInt(), 9);

        // Changed: what is given, the rest kept; one step to undo each.
        QJsonObject r = call("edit_painting", {{"painting", 2}, {"to", QJsonArray{400, 758}}, {"thickness", 3}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        p = json(r).toObject().value("changed").toObject();
        QCOMPARE(p.value("from").toArray(), (QJsonArray{302, 758}));
        QCOMPARE(p.value("to").toArray(), (QJsonArray{400, 758}));
        QCOMPARE(p.value("thickness").toInt(), 3);
        QCOMPARE(p.value("color").toString(), QStringLiteral("#0000ff"));
        QVERIFY(!failed(call("edit_painting", {{"painting", 1}, {"text", "BW = 2 kHz"}, {"color", "red"}})));
        QVERIFY2(sch->documentText().contains("<Text 253 762 10 #ff0000 0 \"BW = 2 kHz\">"), qPrintable(sch->documentText()));
        // (Built by append: QJsonArray{QJsonArray{"x"}} is ["x"] to some
        // compilers - one element of the class's own type is a copy.)
        QJsonArray firstRow;
        firstRow.append(QJsonArray{"x", "y"});
        r = call("edit_painting", {{"painting", 4}, {"cells", firstRow}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonArray cells = json(r).toObject().value("changed").toObject().value("cells").toArray();
        QCOMPARE(cells.at(0).toArray().at(0).toString(), QStringLiteral("x"));
        QCOMPARE(cells.at(0).toArray().at(1).toString(), QStringLiteral("y"));
        QCOMPARE(cells.at(1).toArray().at(0).toString(), QStringLiteral("1 kHz"));
        // A flat list of texts is the first row; a row that is not a list is refused.
        QVERIFY(!failed(call("edit_painting", {{"painting", 4}, {"cells", QJsonArray{"x", "z"}}})));
        QVERIFY(failed(call("edit_painting", {{"painting", 4}, {"cells", QJsonArray{QJsonArray{"a", "b"}, "c"}}})));
        QVERIFY(!failed(call("undo")));
        QVERIFY(!failed(call("edit_painting", {{"painting", 7}, {"angle", 180}, {"mirrored", true}})));
        QVERIFY(!failed(call("undo")));
        QVERIFY(!failed(call("undo")));
        cells = json(call("get_schematic")).toObject().value("paintings").toArray().at(3).toObject().value("cells").toArray();
        QCOMPARE(cells.at(0).toArray().at(0).toString(), QStringLiteral("f"));
        QVERIFY(!failed(call("undo")));
        QVERIFY(sch->documentText().contains("\"BW = 1.596 kHz\""));

        // Refused, and nothing changed.
        const QString before = sch->documentText();
        const QList<QJsonObject> refused{
            QJsonObject{{"type", "text"}, {"x", 0}, {"y", 0}},
            QJsonObject{{"type", "arrow"}, {"from", QJsonArray{0, 0}}},
            QJsonObject{{"type", "text"}, {"x", 0}, {"y", 0}, {"text", "a"}, {"colour", "red"}},
            QJsonObject{{"type", "rectangle"}, {"x", 0}, {"y", 0}, {"width", 5}, {"height", 5}, {"color", "#zzz"}},
            QJsonObject{{"type", "rectangle"}, {"x", 0}, {"y", 0}, {"width", 5}, {"height", 5}, {"style", "wavy"}},
            QJsonObject{{"type", "rectangle"}, {"x", 0}, {"y", 0}, {"width", -5}, {"height", 5}},
            QJsonObject{{"type", "waveform"}, {"x", 0}, {"y", 0}, {"shape", "zigzag"}},
            QJsonObject{{"type", "polygon"}, {"x", 0}, {"y", 0}, {"angle", 45}},
            QJsonObject{{"type", "polygon"}, {"x", 0}, {"y", 0}, {"sides", 500}},
            QJsonObject{{"type", "formula"}, {"x", 0}, {"y", 0}},
            QJsonObject{{"type", "port"}, {"x", 0}, {"y", 0}},
            QJsonObject{{"type", "hexagon"}, {"x", 0}, {"y", 0}},
            QJsonObject{{"type", "image"}, {"x", 0}, {"y", 0}, {"file", "no-such.png"}},
            QJsonObject{{"type", "polyline"}, {"points", QJsonArray{QJsonArray{0, 0}, "x"}}},
            QJsonObject{}};
        for (const QJsonObject& bad : refused) {
            const QJsonObject rr = call("add_painting", bad);
            QVERIFY2(failed(rr), qPrintable(QJsonDocument(bad).toJson(QJsonDocument::Compact)));
        }
        const QJsonObject unknown = call("add_painting", {{"type", "text"}, {"x", 0}, {"y", 0}, {"text", "a"}, {"colour", "red"}});
        QVERIFY2(text(unknown).contains("colour") && text(unknown).contains("color, "), qPrintable(text(unknown)));   // what it takes
        QVERIFY(failed(call("edit_painting", {{"painting", 99}, {"x", 1}})));
        QVERIFY(failed(call("edit_painting", {{"painting", 1}})));
        QVERIFY(failed(call("edit_painting", {{"painting", 1}, {"type", "arrow"}, {"x", 1}})));
        QVERIFY(failed(call("edit_painting", {{"painting", 2}, {"text", "an arrow has none"}})));
        QCOMPARE(sch->documentText(), before);

        // Selected and deleted by their numbers, one step to undo.
        QVERIFY2(text(call("select", {{"paintings", QJsonArray{1, 2}}})).contains("2 selected"), "");
        QVERIFY(!failed(call("delete", {{"paintings", QJsonArray{1, 2}}})));
        QCOMPARE(int(sch->a_DocPaints.size()), 7);
        QVERIFY(!failed(call("undo")));
        QCOMPARE(int(sch->a_DocPaints.size()), 9);
        QVERIFY(failed(call("delete", {{"paintings", QJsonArray{42}}})));

        // A picture, from a file; its size its own, then changed.
        QImage image(40, 20, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(dir.filePath("workspace/red.png")));
        p = add({{"type", "image"}, {"file", "red.png"}, {"x", 0}, {"y", 500}});
        QCOMPARE(p.value("width").toInt(), 40);
        QVERIFY2(p.value("picture").toString().contains("40 x 20"), qPrintable(p.value("picture").toString()));
        r = call("edit_painting", {{"painting", 10}, {"width", 80}, {"height", 40}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("changed").toObject().value("width").toInt(), 80);
        QVERIFY(!sch->documentText().contains("picture"));

        // The fields told, type by type.
        const QString format = text(call("describe_format", {{"element", "painting"}}));
        QVERIFY(format.contains("text_box") && format.contains("tip") && format.contains("curly_brace"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A subcircuit's symbol drawn with the same tools: the document shown
    // as its symbol when asked (and back), its paintings - ports and name
    // text among them - listed, a rectangle added and undone in the
    // symbol's own undo; a port moved but not deleted or made.
    void aSymbolIsDrawnToo()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", "P1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", "P2"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", "sub"}})));
        QJsonObject r = call("add_painting", {{"symbol", true}, {"type", "rectangle"}, {"x", -40}, {"y", -40}, {"width", 80}, {"height", 80}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(sch->getSymbolMode());
        QVERIFY(json(r).toObject().value("note").toString().contains("symbol"));
        const int count = int(sch->a_SymbolPaints.size());
        const QJsonArray symbol = json(call("get_schematic")).toObject().value("symbol paintings").toArray();
        QCOMPARE(symbol.size(), count);
        int port = 0;
        bool id = false;
        for (const QJsonValue& v : symbol) {
            if (v.toObject().value("type").toString() == "port" && port == 0) port = v.toObject().value("painting").toInt();
            id = id || v.toObject().value("type").toString() == "id";
        }
        QVERIFY(port > 0 && id);
        QCOMPARE(symbol.last().toObject().value("type").toString(), QStringLiteral("rectangle"));
        // Undone in the symbol's own undo: the rectangle goes, the symbol stays.
        QVERIFY(!failed(call("undo")));
        QCOMPARE(int(sch->a_SymbolPaints.size()), count - 1);
        QVERIFY(sch->getSymbolMode());
        // Its ports: moved, not deleted or made.
        r = call("edit_painting", {{"painting", port}, {"x", -60}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("changed").toObject().value("x").toInt(), -60);
        QVERIFY(failed(call("delete", {{"paintings", QJsonArray{port}}})));
        QVERIFY(failed(call("add_painting", {{"type", "port"}, {"x", 0}, {"y", 0}})));
        QVERIFY(failed(call("edit_painting", {{"painting", port}, {"number", "7"}})));
        // The schematic's tools switch back to the schematic themselves, and
        // say so; a painting of the symbol shows it again, one of the
        // schematic ('symbol': false) switches back too.
        r = call("add_component", {{"type", "R"}, {"name", "Rback"}, {"x", 300}, {"y", 300}});
        QVERIFY2(!failed(r) && text(r).contains("shows its schematic again"), qPrintable(text(r)));
        QVERIFY(!sch->getSymbolMode());
        QVERIFY(!failed(call("delete", {{"names", QJsonArray{"Rback"}}})));
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", port}, {"x", -70}})));
        QVERIFY(sch->getSymbolMode());
        r = call("add_painting", {{"symbol", false}, {"type", "text"}, {"x", 0}, {"y", 0}, {"text", "the schematic"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!sch->getSymbolMode());
        QCOMPARE(int(sch->a_DocPaints.size()), 1);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The files of the workspace, a project or a folder: kinds, sizes,
    // times, newest first or by name, a dataset with its simulator and
    // schematic, the open ones, the projects; narrowed by kind and name;
    // hidden files and unknown kinds left out; what is not there refused.
    void theWorkspaceIsListed()
    {
        const QString ws = dir.filePath("workspace");
        QVERIFY(QDir().mkpath(ws + "/amp_prj/sub"));
        const QByteArray empty = "<Qucs Schematic 26.1.3>\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n</Components>\n"
                                 "<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
        const auto write = [](const QString& path, const QByteArray& data, int secondsAgo) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) return false;
            f.write(data);
            f.flush();   // (else closing it writes it again, and now)
            return f.setFileTime(QDateTime::currentDateTime().addSecs(-secondsAgo), QFileDevice::FileModificationTime);
        };
        QVERIFY(write(ws + "/amp_prj/amp.sch", empty, 100));
        QVERIFY(write(ws + "/amp_prj/amp.dat.ngspice", "<Qucs Dataset 26.1.3>\n", 50));
        QVERIFY(write(ws + "/amp_prj/amp.dat.xyce", "<Qucs Dataset 26.1.3>\n", 500));
        QVERIFY(write(ws + "/amp_prj/sub/notes.md", "# notes\n", 10));
        QVERIFY(write(ws + "/amp_prj/data.csv", "a,b\n", 1000));
        QVERIFY(write(ws + "/amp_prj/.hidden.sch", empty, 5));
        QVERIFY(write(ws + "/amp_prj/thing.xyz", "?", 5));
        const auto paths = [](const QJsonObject& listed) {
            QStringList list;
            for (const QJsonValue& v : listed.value("files").toArray()) list << v.toObject().value("path").toString();
            return list;
        };
        QJsonObject listed = json(call("list_documents", {{"folder", "amp"}})).toObject();
        QCOMPARE(paths(listed), (QStringList{"sub/notes.md", "amp.dat.ngspice", "amp.sch", "amp.dat.xyce", "data.csv"}));
        const QJsonObject dataset = listed.value("files").toArray().at(1).toObject();
        QCOMPARE(dataset.value("kind").toString(), QStringLiteral("dataset"));
        QCOMPARE(dataset.value("simulator").toString(), QStringLiteral("ngspice"));
        QCOMPARE(dataset.value("of").toString(), QStringLiteral("amp.sch"));
        QVERIFY(dataset.value("changed").toString().contains('T'));
        QCOMPARE(listed.value("files").toArray().at(2).toObject().value("kind").toString(), QStringLiteral("schematic"));
        QCOMPARE(paths(json(call("list_documents", {{"folder", "amp_prj"}, {"kind", "dataset"}})).toObject()),
                 (QStringList{"amp.dat.ngspice", "amp.dat.xyce"}));
        QCOMPARE(paths(json(call("list_documents", {{"folder", ws + "/amp_prj"}, {"search", "AMP.s"}})).toObject()), (QStringList{"amp.sch"}));
        QCOMPARE(paths(json(call("list_documents", {{"folder", "amp"}, {"sort", "name"}})).toObject()),
                 (QStringList{"amp.dat.ngspice", "amp.dat.xyce", "amp.sch", "data.csv", "sub/notes.md"}));
        // The workspace: its projects, and the files in them.
        QVERIFY(!failed(call("open_document", {{"path", "amp_prj/amp.sch"}})));
        listed = json(call("list_documents")).toObject();
        bool project = false, open = false;
        for (const QJsonValue& v : listed.value("projects").toArray())
            project = project || (v.toObject().value("project").toString() == "amp" && v.toObject().value("folder").toString() == "amp_prj");
        for (const QJsonValue& v : listed.value("files").toArray())
            open = open || (v.toObject().value("path").toString() == "amp_prj/amp.sch" && v.toObject().value("open").toBool());
        QVERIFY2(project && open, qPrintable(QJsonDocument(listed).toJson()));
        QVERIFY(!failed(call("close_document")));
        QVERIFY(failed(call("list_documents", {{"folder", "nope"}})));
        QVERIFY(failed(call("list_documents", {{"kind", "movie"}})));
        QVERIFY(failed(call("list_documents", {{"sort", "size"}})));
        QVERIFY(json(call("list_documents", {{"folder", "amp"}, {"search", "zzz"}})).toObject().contains("note"));
    }

    // A picture written to a file as File > Export as image writes one,
    // without its dialog: the whole schematic, or one diagram alone (the
    // selection kept as it was), in the format of its suffix or the one
    // given, PDF+LaTeX with its PDF; a JPEG's paper said so when asked for
    // none; odd arguments refused.
    void aPictureIsWrittenToAFile()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_diagram", {{"x", 400}, {"y", 400}, {"width", 200}, {"height", 120}})));
        QVERIFY(!failed(call("select", {{"names", QJsonArray{"R1"}}})));
        QJsonObject r = call("export_image", {{"save_as", "whole.png"}, {"scale", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QImage whole(dir.filePath("workspace/whole.png"));
        QVERIFY(!whole.isNull());
        QCOMPARE(json(r).toObject().value("pixels").toArray().at(0).toInt(), whole.width());
        r = call("export_image", {{"save_as", "diagram.png"}, {"scale", 1}, {"diagram", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QImage alone(dir.filePath("workspace/diagram.png"));
        QVERIFY2(alone.width() < whole.width() && alone.width() >= 200, qPrintable(QStringLiteral("%1 %2").arg(alone.width()).arg(whole.width())));
        // What was selected is selected again.
        QVERIFY(sch->getComponentByName("R1")->isSelected);
        QVERIFY(!sch->a_DocDiags.front()->isSelected);
        // The format of the suffix, or the one given.
        r = call("export_image", {{"save_as", "whole.svg"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).toObject().contains("size in units"));
        QVERIFY(QFileInfo(dir.filePath("workspace/whole.svg")).size() > 100);
        r = call("export_image", {{"save_as", "report"}, {"format", "pdf_tex"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).toObject().value("written").toString().endsWith("report.pdf_tex"));
        QVERIFY(QFileInfo::exists(dir.filePath("workspace/report.pdf")));
        r = call("export_image", {{"save_as", "paper.jpg"}, {"transparent", true}});
        QVERIFY2(!failed(r) && json(r).toObject().value("note").toString().contains("transparent"), qPrintable(text(r)));
        r = call("export_image", {{"save_as", "grey.png"}, {"colours", "grayscale"}, {"transparent", true}});
        QVERIFY2(!failed(r) && !json(r).toObject().contains("note"), qPrintable(text(r)));
        // Refused.
        for (const QJsonObject& bad : {QJsonObject{{"save_as", "noformat"}}, QJsonObject{{"save_as", "a.png"}, {"format", "gif"}},
                                       QJsonObject{{"save_as", "a.png"}, {"diagram", 5}}, QJsonObject{{"save_as", "a.png"}, {"colours", "sepia"}},
                                       QJsonObject{{"save_as", "a.png"}, {"scale", 100}}, QJsonObject{{"save_as", "no/such/folder/a.png"}},
                                       QJsonObject{}})
            QVERIFY2(failed(call("export_image", bad)), qPrintable(QJsonDocument(bad).toJson(QJsonDocument::Compact)));
        QVERIFY(!failed(call("select")));
        QVERIFY(text(call("export_image", {{"save_as", "a.png"}, {"selection", true}})).contains("Nothing is selected"));
        QVERIFY(!QFileInfo::exists(dir.filePath("workspace/a.png")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The simulator chosen as the toolbar's list chooses it: one that is
    // installed, and back; one that is not refused, the setting kept.
    void theSimulatorIsChosen()
    {
        QJsonObject r = call("set_simulator", {{"simulator", "ngspice"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("simulator").toString(), QStringLiteral("ngspice"));
        const QJsonArray installed = json(r).toObject().value("installed").toArray();
        QVERIFY(installed.contains(QJsonValue("ngspice")));
        QCOMPARE(QucsSettings.DefaultSimulator, int(spicecompat::simNgspice));
        for (const char* other : {"xyce", "spiceopus", "qucsator"}) {
            r = call("set_simulator", {{"simulator", other}});
            if (installed.contains(QJsonValue(other))) {
                QVERIFY2(!failed(r), qPrintable(text(r)));
                QCOMPARE(json(r).toObject().value("simulator").toString(), QString::fromLatin1(other));
                QCOMPARE(json(r).toObject().value("was").toString(), QStringLiteral("ngspice"));
                QVERIFY(!failed(call("set_simulator", {{"simulator", "ngspice"}})));
            } else {
                QVERIFY2(failed(r) && text(r).contains("not installed"), qPrintable(text(r)));
            }
            QCOMPARE(QucsSettings.DefaultSimulator, int(spicecompat::simNgspice));
        }
        QVERIFY(failed(call("set_simulator", {{"simulator", "spectre"}})));
        QVERIFY(failed(call("set_simulator")));
    }

    // All or nothing: an atomic batch that fails leaves the schematic as
    // it was (one step to undo brings its changes back); one that is not
    // says how many changes stay, and undo takes them back in one call.
    void aBatchIsAllOrNothing()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R0"}, {"x", 0}, {"y", 0}})));
        const QJsonArray calls{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}}}},
            QJsonObject{{"tool", "add_painting"}, {"arguments", QJsonObject{{"type", "text"}, {"x", 0}, {"y", 200}, {"text", "note"}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "C"}, {"name", "C1"}, {"x", 300}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "NoSuchPart"}, {"x", 500}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "L"}, {"name", "L1"}, {"x", 700}, {"y", 100}}}}};
        QJsonObject r = call("batch", {{"calls", calls}, {"atomic", true}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("Atomic: the changes"), qPrintable(text(r).left(600)));
        QVERIFY(!text(r).contains("[5]"));   // the rest not run
        QCOMPARE(int(sch->a_DocComps.size()), 1);
        QCOMPARE(int(sch->a_DocPaints.size()), 0);
        // One step to undo brings the changes back, and redo takes them away.
        QVERIFY(!failed(call("undo")));
        QVERIFY(sch->getComponentByName("R1") && sch->getComponentByName("C1"));
        QCOMPARE(int(sch->a_DocPaints.size()), 1);
        QVERIFY(!failed(call("redo")));
        QCOMPARE(int(sch->a_DocComps.size()), 1);
        // Not atomic: how many changes stay, taken back in one call.
        r = call("batch", {{"calls", calls}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("3 on the document in front"), qPrintable(text(r).left(600)));
        QCOMPARE(int(sch->a_DocComps.size()), 3);
        r = call("undo", {{"steps", 3}});
        QVERIFY2(text(r).contains("Undone 3 steps"), qPrintable(text(r)));
        QCOMPARE(int(sch->a_DocComps.size()), 1);
        QCOMPARE(int(sch->a_DocPaints.size()), 0);
        QVERIFY(failed(call("undo", {{"steps", 0}})));
        QVERIFY(failed(call("undo", {{"steps", "two"}})));
        r = call("redo", {{"steps", 50}});
        QVERIFY2(text(r).contains("Redone 3 steps") && text(r).contains("no more"), qPrintable(text(r)));
        // An atomic batch that goes through keeps all it made.
        r = call("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R9"}, {"x", 900}, {"y", 100}}}}}},
                           {"atomic", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(sch->getComponentByName("R9") != nullptr);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Distortion and a loop's stability read from a dataset: the THD of a
    // transient at a stated fundamental over two periods, and a loop
    // gain's gain, phase margin and gain margin - its phase taken along.
    void distortionAndLoopGainAreReadFromADataset()
    {
        const QString file = dir.filePath("workspace/loop.dat.ngspice");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream s(&f);
            s << "<Qucs Dataset 26.1.3>\n<indep time 2001>\n";
            for (int i = 0; i <= 2000; ++i) s << "  " << QString::number(i * 1e-6, 'e', 12) << "\n";
            s << "</indep>\n<dep tran.v(out) time>\n";
            for (int i = 0; i <= 2000; ++i) {
                const double t = i * 1e-6;
                s << "  " << QString::number(std::sin(2 * M_PI * 1e3 * t) + 0.1 * std::sin(2 * M_PI * 2e3 * t), 'e', 12) << "\n";
            }
            s << "</dep>\n<indep frequency 601>\n";
            for (int i = 0; i <= 600; ++i) s << "  " << QString::number(std::pow(10.0, 1 + i / 100.0), 'e', 12) << "\n";
            s << "</indep>\n<dep ac.v(loop) frequency>\n";
            for (int i = 0; i <= 600; ++i) {
                const double x = std::pow(10.0, 1 + i / 100.0);
                std::complex<double> t(100.0, 0.0);
                for (double p : {1e3, 1e5, 1e6}) t /= std::complex<double>(1.0, x / p);
                s << "  " << QString::number(t.real(), 'e', 12) << (t.imag() < 0 ? "-j" : "+j") << QString::number(std::abs(t.imag()), 'e', 12) << "\n";
            }
            s << "</dep>\n";
        }
        QJsonObject r = call("get_dataset", {{"path", file}, {"variables", QJsonArray{"tran.v(out)"}}, {"measure", QJsonArray{"thd"}},
                                             {"fundamental", 1000}, {"periods", 2}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject thd = json(r).toObject().value("variables").toArray().first().toObject().value("measurements").toObject().value("thd").toObject();
        QVERIFY2(std::abs(thd.value("value").toDouble() - 10) < 0.01, qPrintable(QJsonDocument(thd).toJson()));
        QCOMPARE(thd.value("periods").toInt(), 2);
        r = call("get_dataset", {{"path", file}, {"variables", QJsonArray{"ac.v(loop)"}}, {"measure", QJsonArray{"gain", "phase_margin", "gain_margin"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject m = json(r).toObject().value("variables").toArray().first().toObject().value("measurements").toObject();
        QVERIFY2(std::abs(m.value("gain").toObject().value("value").toDouble() - 40) < 0.01, qPrintable(QJsonDocument(m).toJson()));
        QVERIFY2(m.value("phase_margin").toObject().contains("gain crossover"), qPrintable(QJsonDocument(m).toJson()));
        QVERIFY2(m.value("gain_margin").toObject().contains("phase crossover"), qPrintable(QJsonDocument(m).toJson()));
        // Odd arguments refused.
        QVERIFY(failed(call("get_dataset", {{"path", file}, {"variables", QJsonArray{"tran.v(out)"}}, {"measure", QJsonArray{"thd"}}, {"fundamental", -1}})));
        QVERIFY(failed(call("get_dataset", {{"path", file}, {"variables", QJsonArray{"tran.v(out)"}}, {"measure", QJsonArray{"thd"}}, {"harmonics", 1}})));
        QVERIFY(failed(call("get_dataset", {{"path", file}, {"variables", QJsonArray{"tran.v(out)"}}, {"measure", QJsonArray{"thd"}}, {"periods", 0.5}})));
    }

    // A simulation during which the user changes the schematic says its
    // results are of the schematic as it was when it began.
    void anEditWhileASimulationRunsIsTold()
    {
#ifdef Q_OS_WIN
        QSKIP("A shell script stands in for the simulator.");
#else
        const QString script = dir.filePath("slow-simulator.sh");
        {
            QFile f(script);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\nsleep 1\nexit 0\n");
        }
        QVERIFY(QFile::setPermissions(script, QFile::permissions(script) | QFileDevice::ExeOwner | QFileDevice::ExeUser));
        const QString was = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = script;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 100}, {"y", 200}})));
        QVERIFY(!failed(call("connect", {{"from", "R1.2"}, {"to", "GND.1"}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"x", 100}, {"y", -100}})));
        QVERIFY(!failed(call("save_document", {{"as", "slow"}})));
        // The user changes R1 while it runs.
        QTimer::singleShot(300, this, [this] {
            const quint64 editor = QucsDoc::editor();
            QucsDoc::setEditor(0);
            call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "5k"}}}});
            QucsDoc::setEditor(editor);
        });
        QJsonObject r = control->callNow("simulate", {{"timeout", 30}}, 60000, 404);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("changed while it ran").toString().contains("by the user"), qPrintable(text(r)));
        // Left alone while it runs: nothing said.
        r = control->callNow("simulate", {{"timeout", 30}}, 60000, 404);
        QucsSettings.NgspiceExecutable = was;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(!json(r).toObject().contains("changed while it ran"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
#endif
    }

    // The connectivity check and what a change tells of it: a wire drawn
    // across another net's (no junction: said), a part put with a pin on a
    // wire, what floats; check_schematic, the summary's count; grounds by
    // their 'ref' when there are several.
    void connectivityIsCheckedAndCrossingsAreTold()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Vdc V1 1 100 200 18 -26 0 1 \"1 V\" 1>\n"
                                                        "  <GND * 1 100 230 0 0 0 0>\n"
                                                        "  <R R1 1 200 100 15 -26 0 0 " + R +
                                                        "  <R R2 1 300 200 15 -26 0 1 " + R +
                                                        "  <GND * 1 300 230 0 0 0 0>\n"
                                                        "</Components>\n<Wires>\n"
                                                        "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
                                                        "  <230 100 300 100 \"\" 0 0 0 \"\">\n  <300 100 300 170 \"\" 0 0 0 \"\">\n"
                                                        "</Wires>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject checked = json(call("check_schematic")).toObject();
        QVERIFY2(checked.value("errors").toArray().isEmpty(), qPrintable(text(call("check_schematic"))));
        // Grounds by their refs.
        QJsonObject summary = json(call("get_schematic")).toObject();
        QStringList refs;
        for (const QJsonValue& c : summary.value("components").toArray()) refs << c.toObject().value("ref").toString();
        QVERIFY2(refs.contains("GND#1") && refs.contains("GND#2"), qPrintable(refs.join(',')));
        r = call("connect", {{"from", "GND.1"}, {"to", "R1.1"}});
        QVERIFY2(failed(r) && text(r).contains("GND#1.1"), qPrintable(text(r)));
        QVERIFY(failed(call("connect", {{"from", "GND#3.1"}, {"to", "R1.1"}})));
        QVERIFY(text(call("connect", {{"from", "GND#1.1"}, {"to", "GND#2.1"}})).contains("one net already"));   // (ground is one net)

        // A wire across R1-R2's: drawn, and said.
        r = call("add_component", {{"type", "R"}, {"name", "R3"}, {"x", 500}, {"y", 200}, {"rotation", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("add_wire", {{"points", QJsonArray{QJsonArray{500, 170}, QJsonArray{500, 130}, QJsonArray{250, 130}}}});
        QVERIFY2(!failed(r) && text(r).contains("Look:") && text(r).contains("cross without a junction"), qPrintable(text(r)));
        // A part with a pin on that wire: said too.
        r = call("add_component", {{"type", "R"}, {"name", "R4"}, {"x", 400}, {"y", 100}, {"rotation", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("note").toString().contains("pin 1"), qPrintable(text(r)));
        // The check: the crossing a note, R3's pin and the wire's open end, what floats.
        checked = json(call("check_schematic")).toObject();
        const QString all = QJsonDocument(checked).toJson(QJsonDocument::Compact);
        QVERIFY2(all.contains("cross without a junction"), qPrintable(all));
        QVERIFY2(all.contains("the wire end at 250, 130 is connected to nothing"), qPrintable(all));
        QVERIFY2(all.contains("floating"), qPrintable(all));
        bool noted = false;
        for (const QJsonValue& n : checked.value("notes").toArray()) noted = noted || n.toObject().value("message").toString().contains("cross");
        QVERIFY(noted);
        QCOMPARE(checked.value("warnings").toArray().first().toObject().value("at").toArray().size(), 2);
        summary = json(call("get_schematic")).toObject();
        QVERIFY2(summary.value("problems").toObject().value("warnings").toInt() > 0, qPrintable(QJsonDocument(summary.value("problems").toObject()).toJson()));
        QVERIFY(summary.value("problems").toObject().value("notes").toInt() > 0);
        QVERIFY(failed(call("check_schematic", {{"path", "nowhere.sch"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The operating point in one call, of a schematic with a transient
    // analysis only: nodes, branch currents and each device's quantities
    // under its component, with what they make plain; the datasets left
    // as they were, and the next run its analyses.
    void theOperatingPointComesInOneCall()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Vdc V1 1 100 200 18 -26 0 1 \"5 V\" 1>\n"
                                                        "  <GND * 1 100 230 0 0 0 0>\n"
                                                        "  <R R1 1 200 100 15 -26 0 0 " + R +
                                                        "  <Diode D1 1 300 200 15 -26 0 1 \"1e-15 A\" 1 \"1\" 1 \"10 fF\" 1 \"0.5\" 0 \"0.7 V\" 0 \"0.5\" 0 "
                                                        "\"0.0 fF\" 0 \"0.0\" 0 \"2.0\" 0 \"0.0 Ohm\" 0 \"0.0 ps\" 0 \"0\" 0 \"0.0\" 0 \"1.0\" 0 \"1.0\" 0 "
                                                        "\"0\" 0 \"1 mA\" 0 \"26.85\" 0 \"3.0\" 0 \"1.11\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 \"0.0\" 0 "
                                                        "\"0.0\" 0 \"26.85\" 0 \"1.0\" 0 \"normal\" 0>\n"
                                                        "  <GND * 1 300 230 0 0 0 0>\n"
                                                        "  <.TR TR1 1 100 400 0 57 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"11\" 0>\n"
                                                        "</Components>\n<Wires>\n"
                                                        "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
                                                        "  <230 100 300 100 \"out\" 250 70 30 \"\">\n  <300 100 300 170 \"\" 0 0 0 \"\">\n"
                                                        "</Wires>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", "diode_op"}})));
        const QString dataset = dir.filePath("workspace/diode_op.dat.ngspice");
        QFile::remove(dataset);
        r = call("simulate", {{"operating_point", true}, {"timeout", 60}}, 90000);
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject outcome = json(r).toObject();
        QVERIFY2(outcome.value("succeeded").toBool(), qPrintable(text(r)));
        const QJsonObject op = outcome.value("operating point").toObject();
        const double vout = op.value("nodes").toObject().value("out").toDouble(-1);
        QVERIFY2(vout > 0.5 && vout < 0.9, qPrintable(text(r)));   // a forward diode
        bool diode = false;
        for (const QJsonValue& d : op.value("devices").toArray())
            if (d.toObject().value("component").toString() == "D1") {
                diode = true;
                const double id = d.toObject().value("values").toObject().value("id").toDouble();
                QVERIFY2(id > 3e-3 && id < 5e-3, qPrintable(text(r)));   // (5 V - 0.7 V) / 1k
            }
        QVERIFY2(diode, qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(dataset));   // its datasets as they were
        QVERIFY2(outcome.value("analysis").toString().contains("operating point"), qPrintable(text(r)));
        // The next run is its analyses.
        r = call("simulate", {{"timeout", 60}}, 90000);
        QucsSettings.NgspiceExecutable = before;
        QVERIFY2(json(r).toObject().value("dataset written").toBool(), qPrintable(text(r)));
        QVERIFY(!json(r).toObject().contains("operating point"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A part's text: which properties are shown, whether its name is,
    // where the text is - without rewriting its line; rotation counted
    // from the type's own orientation (a DC source is made turned).
    void aPartsTextIsShownHiddenAndMoved()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QJsonObject r = call("add_component", {{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 100}});
        QCOMPARE(json(r).toObject().value("rotation").toInt(), 0);
        QCOMPARE(json(r).toObject().value("rotation in the file").toInt(), 1);
        r = call("edit_component", {{"name", "V1"}, {"rotation", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("rotation").toInt(), 1);
        QCOMPARE(json(r).toObject().value("rotation in the file").toInt(), 2);
        r = call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 300}, {"y", 100}, {"shown", QJsonObject{{"Temp", true}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("edit_component", {{"name", "R1"}, {"shown", QJsonObject{{"R", false}}}, {"name_shown", false}, {"text_at", QJsonArray{20, -40}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject c = json(r).toObject();
        QCOMPARE(c.value("text at").toArray(), (QJsonArray{20, -40}));
        QCOMPARE(c.value("name shown").toBool(true), false);
        Component* r1 = sch->getComponentByName("R1");
        QVERIFY(!r1->getProperty("R")->display && r1->getProperty("Temp")->display && !r1->showName);
        QCOMPARE(QPoint(r1->tx, r1->ty), QPoint(20, -40));
        QVERIFY(!failed(call("undo")));
        r1 = sch->getComponentByName("R1");
        QVERIFY(r1->getProperty("R")->display && r1->showName);
        // Refused, and nothing changed.
        QVERIFY(text(call("edit_component", {{"name", "R1"}, {"shown", QJsonObject{{"Nope", false}}}})).contains("its properties are"));
        QVERIFY(failed(call("edit_component", {{"name", "R1"}, {"shown", QJsonObject{{"R", "no"}}}})));
        QVERIFY(failed(call("edit_component", {{"name", "R1"}, {"text_at", QJsonArray{1}}})));
        QVERIFY(failed(call("edit_component", {{"name", "R1"}, {"name_shown", "no"}})));
        QVERIFY(sch->getComponentByName("R1")->getProperty("R")->display);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A component line with a value too many - every value after it would
    // be shifted - is refused, naming the part and both counts; one with
    // fewer is taken, the rest at their defaults, and said.
    void aShiftedComponentLineIsRefused()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n  <R R1 1 100 100 15 -26 0 1 \"1k\" 1 \"0\" 0 \"26.85\" 0 \"0.0\" 0 "
                                                        "\"0.0\" 0 \"26.85\" 0 \"european\" 0>\n</Components>\n"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("R1 (R) has 7 property values, its type 6 properties"), qPrintable(text(r)));
        QCOMPARE(int(sch->a_DocComps.size()), 0);
        r = call("set_schematic", {{"text", "<Components>\n  <R R1 1 100 100 15 -26 0 1 \"2k\" 1 \"26.85\" 0>\n</Components>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("note").toString().contains("took their defaults"), qPrintable(text(r)));
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("2k"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Diagrams placed where there is room when not told where, and a note
    // when one lies over another or over parts.
    void diagramsArePlacedWhereThereIsRoom()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QJsonObject r = call("add_diagram", {});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("note").toString().contains("Placed below the circuit"), qPrintable(text(r)));
        const Diagram* d = sch->a_DocDiags.front();
        QVERIFY(d->cy - d->y2 > sch->getComponentByName("R1")->boundingRect().bottom());
        r = call("add_diagram", {{"x", d->cx + 20}, {"y", d->cy}});
        QVERIFY2(json(r).toObject().value("note").toString().contains("lies over diagram 1"), qPrintable(text(r)));
        r = call("edit_diagram", {{"diagram", 2}, {"x", 60}, {"y", 150}});
        QVERIFY2(json(r).toObject().value("note").toString().contains("R1"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A group moved with the wiring among its parts, the wires to the rest
    // drawn on: every net as it was, one step to undo.
    void aGroupMovesWithItsWires()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R2"}, {"x", 200}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R3"}, {"x", 200}, {"y", 300}, {"rotation", 1}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "R2.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R2.2"}, {"to", "R3.1"}}}}}}})));
        const auto netsNow = [this] {
            QStringList nets;
            for (const QJsonValue& n : json(call("get_schematic")).toObject().value("nets").toArray()) {
                QStringList pins;
                for (const QJsonValue& p : n.toObject().value("pins").toArray()) pins << p.toString();
                pins.sort();
                nets << pins.join(',');
            }
            nets.sort();
            return nets;
        };
        const QStringList before = netsNow();
        QJsonObject r = call("move", {{"names", QJsonArray{"R1", "R2"}}, {"dx", 100}, {"dy", -52}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("by").toArray(), (QJsonArray{100, -50}));   // a step of the grid
        QVERIFY(json(r).toObject().value("wires moved with them").toInt() >= 1);
        QCOMPARE(sch->getComponentByName("R1")->cx, 200);
        QCOMPARE(sch->getComponentByName("R3")->cx, 200);   // not in the group
        QCOMPARE(netsNow(), before);
        QVERIFY(!failed(call("undo")));
        QCOMPARE(sch->getComponentByName("R1")->cx, 100);
        QCOMPARE(netsNow(), before);
        QVERIFY(failed(call("move", {{"names", QJsonArray{"R9"}}, {"dx", 10}})));
        // (Less than a step of the grid: moved by nothing - said, and what
        // would move told.)
        r = call("move", {{"names", QJsonArray{"R1"}}, {"dx", 1}});
        QVERIFY2(!failed(r) && json(r).toObject().value("moved").toArray().isEmpty(), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // An analysis added with its plot in one call: AC in dB over a log
    // axis, a transient beside it, a sweep of a part's value; what does
    // not make sense refused.
    void analysesAreAddedWithTheirPlot()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QJsonObject r = call("add_analysis", {{"kind", "ac"}, {"plot", QJsonArray{"out"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject result = json(r).toObject();
        QCOMPARE(result.value("analysis").toObject().value("type").toString(), QStringLiteral(".AC"));
        Component* ac = sch->getComponentByName(result.value("analysis").toObject().value("name").toString());
        QVERIFY(ac != nullptr);
        QCOMPARE(ac->getProperty("Type")->Value, QStringLiteral("log"));
        QCOMPARE(ac->getProperty("Stop")->Value, QStringLiteral("100 MHz"));
        QCOMPARE(result.value("diagram").toObject().value("traces").toArray().first().toObject().value("variable").toString(),
                 QStringLiteral("ngspice/ac.v(out)"));
        QCOMPARE(result.value("steps to undo").toInt(), 2);
        r = call("add_analysis", {{"kind", "tran"}, {"stop", "2 ms"}, {"plot", QJsonArray{"v(out)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Component* tr = sch->getComponentByName(json(r).toObject().value("analysis").toObject().value("name").toString());
        QCOMPARE(tr->getProperty("Stop")->Value, QStringLiteral("2 ms"));
        QVERIFY(tr->cx > ac->cx);   // beside the first
        r = call("add_analysis", {{"kind", "sweep"}, {"analysis", tr->Name}, {"parameter", "R1"}, {"from", "1k"}, {"to", "10k"}, {"points", 3}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Component* sw = sch->getComponentByName(json(r).toObject().value("analysis").toObject().value("name").toString());
        QCOMPARE(sw->getProperty("Sim")->Value, tr->Name);
        QCOMPARE(sw->getProperty("Param")->Value, QStringLiteral("R1"));
        QCOMPARE(sw->getProperty("Points")->Value, QStringLiteral("3"));
        QVERIFY(failed(call("add_analysis", {{"kind", "sweep"}, {"analysis", "TR9"}, {"parameter", "R1"}, {"from", 1}, {"to", 2}})));
        QVERIFY(failed(call("add_analysis", {{"kind", "sweep"}, {"analysis", tr->Name}, {"parameter", "Rnone"}, {"from", 1}, {"to", 2}})));
        QVERIFY(failed(call("add_analysis", {{"kind", "op"}, {"plot", QJsonArray{"out"}}})));
        QVERIFY(failed(call("add_analysis", {{"kind", "noise"}})));
        QVERIFY(!failed(call("add_analysis", {{"kind", "op"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A subcircuit made of parts: they go into a new schematic with a port
    // for each net that goes out and a ground inside; one instance in
    // their place, joined by labels - one step to undo. Its parameters on
    // its symbol's name text, read and changed.
    void aSubcircuitIsMadeOfParts()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Vdc V1 1 100 200 18 -26 0 1 \"1 V\" 1>\n"
                                                        "  <GND * 1 100 230 0 0 0 0>\n"
                                                        "  <R R1 1 200 100 15 -26 0 0 " + R +
                                                        "  <R R2 1 300 200 15 -26 0 1 " + R +
                                                        "  <GND * 1 300 230 0 0 0 0>\n"
                                                        "  <R R3 1 400 200 15 -26 0 1 " + R +
                                                        "  <GND * 1 400 230 0 0 0 0>\n"
                                                        "</Components>\n<Wires>\n"
                                                        "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
                                                        "  <230 100 300 100 \"\" 0 0 0 \"\">\n  <300 100 300 170 \"\" 0 0 0 \"\">\n"
                                                        "  <300 100 400 100 \"\" 0 0 0 \"\">\n  <400 100 400 170 \"\" 0 0 0 \"\">\n"
                                                        "</Wires>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(failed(call("create_subcircuit", {{"names", QJsonArray{"R1", "R2"}}, {"save_as", "divider"}})));   // not saved yet
        QVERIFY(!failed(call("save_document", {{"as", "divided"}})));
        r = call("create_subcircuit", {{"names", QJsonArray{"R1", "R2"}}, {"save_as", "divider"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject made = json(r).toObject();
        const QString file = dir.filePath("workspace/divider.sch");
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(made.value("ports").toArray().size(), 2);   // the source's net and R3's; R2's ground inside
        QCOMPARE(made.value("grounds inside").toInt(), 1);
        QVERIFY(sch->getComponentByName("R1") == nullptr && sch->getComponentByName("R2") == nullptr);
        Component* sub = sch->getComponentByName(made.value("instance").toString());
        QVERIFY(sub != nullptr && sub->Model == "Sub" && sub->Ports.size() == 2);
        // Joined, not floating: no open pin of it.
        const QString checked = text(call("check_schematic"));
        QVERIFY2(!checked.contains(sub->Name + ": pin"), qPrintable(checked));
        // Nothing left behind: no ground that held R2 alone, no wire to where its pin was.
        QVERIFY2(!checked.contains("connected to nothing"), qPrintable(checked));
        QVERIFY2(text(call("get_netlist")).contains(".SUBCKT", Qt::CaseInsensitive), qPrintable(text(call("get_netlist"))));
        QVERIFY(failed(call("create_subcircuit", {{"names", QJsonArray{"R3"}}, {"save_as", "divider"}})));   // there already
        QVERIFY(!failed(call("undo")));
        QVERIFY(sch->getComponentByName("R1") != nullptr && sch->getComponentByName(made.value("instance").toString()) == nullptr);
        // Its parameters: on its symbol's name text.
        QVERIFY(!failed(call("open_document", {{"path", file}})));
        r = call("add_painting", {{"symbol", true}, {"type", "text"}, {"x", 0}, {"y", -60}, {"text", "divider"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        int id = 0;
        for (const QJsonValue& p : json(call("get_schematic")).toObject().value("symbol paintings").toArray())
            if (p.toObject().value("type").toString() == "id") id = p.toObject().value("painting").toInt();
        QVERIFY(id > 0);
        r = call("edit_painting", {{"painting", id}, {"prefix", "DIV"},
                                   {"parameters", QJsonArray{QJsonObject{{"name", "Rtop"}, {"default", "1k"}, {"description", "top resistor"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject changed = json(r).toObject().value("changed").toObject();
        QCOMPARE(changed.value("prefix").toString(), QStringLiteral("DIV"));
        QCOMPARE(changed.value("parameters").toArray().first().toObject().value("default").toString(), QStringLiteral("1k"));
        QVERIFY(failed(call("edit_painting", {{"painting", id}, {"parameters", QJsonArray{QJsonObject{{"name", "a=b"}}}}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Expressions of variables and another run beside this one, read from
    // datasets as ngspice writes them; a peak to peak; a picture of one
    // diagram alone; the traps told.
    void expressionsComparisonsAndSmallThings()
    {
        const auto write = [this](const QString& name, double gain) {
            QFile f(dir.filePath("workspace/" + name));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
            QTextStream s(&f);
            s << "<Qucs Dataset 26.1.3>\n<indep time 101>\n";
            for (int i = 0; i <= 100; ++i) s << "  " << QString::number(i * 1e-5, 'e', 12) << "\n";
            s << "</indep>\n<dep tran.v(in) time>\n";
            for (int i = 0; i <= 100; ++i) s << "  " << QString::number(std::sin(2 * M_PI * 1e3 * i * 1e-5), 'e', 12) << "\n";
            s << "</dep>\n<dep tran.v(out) time>\n";
            for (int i = 0; i <= 100; ++i) s << "  " << QString::number(gain * std::sin(2 * M_PI * 1e3 * i * 1e-5), 'e', 12) << "\n";
            s << "</dep>\n";
            return true;
        };
        QVERIFY(write("amp.dat.ngspice", 0.5));
        QVERIFY(write("run1.dat.ngspice", 0.25));
        const QString file = dir.filePath("workspace/amp.dat.ngspice");
        QJsonObject r = call("get_dataset", {{"path", file}, {"variables", QJsonArray{"v(out)-v(in)", "tran.v(out)"}}, {"compare", "run1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray vars = json(r).toObject().value("variables").toArray();
        QCOMPARE(vars.size(), 2);
        const QJsonObject diff = vars.at(0).toObject();
        QVERIFY(diff.value("expression").toBool());
        QVERIFY2(std::abs(diff.value("min").toDouble() + 0.5) < 1e-3, qPrintable(QJsonDocument(diff).toJson()));   // 0.5 sin - sin
        const QJsonObject out = vars.at(1).toObject();
        QVERIFY2(std::abs(out.value("peak to peak").toDouble() - 1.0) < 1e-3, qPrintable(QJsonDocument(out).toJson()));
        const QJsonObject other = out.value("other run").toObject();
        QVERIFY2(std::abs(other.value("peak to peak").toDouble() - 0.5) < 1e-3, qPrintable(QJsonDocument(other).toJson()));
        const double largest = other.value("difference (this run less that)").toObject().value("largest").toDouble();
        QVERIFY2(std::abs(std::abs(largest) - 0.25) < 1e-3, qPrintable(QJsonDocument(other).toJson()));
        QVERIFY(failed(call("get_dataset", {{"path", file}, {"variables", QJsonArray{"v(out)/"}}})));
        QVERIFY(text(call("get_dataset", {{"path", file}, {"variables", QJsonArray{"tran.v(out)"}}, {"compare", "run9"}})).contains("run1"));
        // The traps told: equation names beside node names; a text's subscripts.
        QVERIFY(text(call("describe_component_type", {{"type", "NutmegEq"}})).contains("clash"));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        r = call("add_painting", {{"type", "text"}, {"x", 0}, {"y", 0}, {"text", "V_out"}});
        QVERIFY2(json(r).toObject().value("note").toString().contains("subscript"), qPrintable(text(r)));
        // A picture of one diagram, and of a region.
        QVERIFY(!failed(call("add_diagram", {{"x", 100}, {"y", 300}})));
        r = call("screenshot", {{"diagram", 1}});
        QVERIFY2(!failed(r) && text(r).contains("diagram 1"), qPrintable(text(r)));
        QCOMPARE(r.value("content").toArray().first().toObject().value("type").toString(), QStringLiteral("image"));
        QVERIFY(!failed(call("screenshot", {{"region", QJsonArray{0, -20, 50, 20}}})));
        QVERIFY(failed(call("screenshot", {{"region", QJsonArray{9000, 9000, 9100, 9100}}})));
        QVERIFY(failed(call("screenshot", {{"diagram", 7}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }
    // ---- Round 5 --------------------------------------------------------

    // An equation block's equations, a Monte Carlo's records and specs,
    // set by name - no set_schematic line written by hand; a Nutmeg
    // equation's simulation by the analysis' kind (tran).
    void equationsRecordsAndSpecsAreSetByName()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QJsonObject r = call("add_component", {{"type", "Eqn"}, {"name", "Eqn1"}, {"x", 100}, {"y", 100},
                                               {"equations", QJsonArray{"gain_db=db(v(out))", "vpp=max(v(out))-min(v(out))"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Component* eqn = sch->getComponentByName("Eqn1");
        QStringList names;
        for (Property* p : eqn->Props) names << p->Name;
        QCOMPARE(names, (QStringList{"gain_db", "vpp", "Export"}));   // the placeholder y=1 replaced; Export last
        QCOMPARE(eqn->getProperty("gain_db")->Value, QStringLiteral("db(v(out))"));
        // Set, added, taken away.
        r = call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonObject{{"gain_db", "db(v(out)/2)"}, {"vmax", "max(v(out))"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        eqn = sch->getComponentByName("Eqn1");
        names.clear();
        for (Property* p : eqn->Props) names << p->Name;
        QCOMPARE(names, (QStringList{"gain_db", "vpp", "vmax", "Export"}));
        QCOMPARE(eqn->getProperty("gain_db")->Value, QStringLiteral("db(v(out)/2)"));
        QVERIFY(!failed(call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonObject{{"vpp", ""}}}})));
        QVERIFY(sch->getComponentByName("Eqn1")->getProperty("vpp") == nullptr);
        QVERIFY(!failed(call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonArray{"only=1"}}, {"replace_equations", true}})));
        names.clear();
        for (Property* p : sch->getComponentByName("Eqn1")->Props) names << p->Name;
        QCOMPARE(names, (QStringList{"only", "Export"}));
        // In the file as name=value.
        QVERIFY(sch->getComponentByName("Eqn1")->save().contains("\"only=1\""));
        // A property it has not: where equations go is said.
        r = call("edit_component", {{"name", "Eqn1"}, {"properties", QJsonObject{{"newvar", "2"}}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("'equations'"), qPrintable(text(r)));
        // Not on a resistor; no quotes.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 300}, {"y", 100}})));
        QVERIFY(failed(call("edit_component", {{"name", "R1"}, {"equations", QJsonObject{{"a", "1"}}}})));
        QVERIFY(failed(call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonObject{{"a", "say \"hi\""}}}})));
        QVERIFY(failed(call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonArray{"no equals sign"}}})));
        // .OPTIONS: the package first, the options in their order.
        r = call("add_component", {{"type", "SpiceOptions"}, {"name", "OPT1"}, {"x", 100}, {"y", 300},
                                   {"equations", QJsonArray{"RELTOL=1e-4", "GMIN=1e-13", "ITL1=500"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        names.clear();
        for (Property* p : sch->getComponentByName("OPT1")->Props) names << p->Name;
        QCOMPARE(names, (QStringList{"XyceOptionPackage", "RELTOL", "GMIN", "ITL1"}));
        // A Nutmeg equation after the transient, named by its kind.
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"name", "TR1"}, {"x", 400}, {"y", 300}})));
        r = call("add_component", {{"type", "NutmegEq"}, {"name", "NutmegEq1"}, {"x", 300}, {"y", 400},
                                   {"properties", QJsonObject{{"Simulation", "tran"}}},
                                   {"equations", QJsonObject{{"vpk", "max(v(out))"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        names.clear();
        for (Property* p : sch->getComponentByName("NutmegEq1")->Props) names << p->Name;
        QCOMPARE(names, (QStringList{"Simulation", "vpk"}));
        const QString netlist = text(call("get_netlist"));
        QVERIFY2(netlist.contains("let vpk = max(v(out))"), qPrintable(netlist));
        // A Monte Carlo's records and specs.
        r = call("add_component", {{"type", ".NGMONTECARLO"}, {"name", "MC1"}, {"x", 500}, {"y", 400},
                                   {"properties", QJsonObject{{"samples", "50"}}},
                                   {"records", QJsonArray{QJsonObject{{"name", "gain"}, {"expression", "db(v(out))"}}, "vmax|max(v(out))"}},
                                   {"specs", QJsonArray{QJsonObject{{"expression", "gain"}, {"min", "19"}, {"max", "21"}}}}});
        if (failed(r) && text(r).contains("samples")) {
            r = call("add_component", {{"type", ".NGMONTECARLO"}, {"name", "MC1"}, {"x", 500}, {"y", 400},
                                       {"records", QJsonArray{QJsonObject{{"name", "gain"}, {"expression", "db(v(out))"}}, "vmax|max(v(out))"}},
                                       {"specs", QJsonArray{QJsonObject{{"expression", "gain"}, {"min", "19"}, {"max", "21"}}}}});
        }
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList records, specs;
        for (Property* p : sch->getComponentByName("MC1")->Props) {
            if (p->Name == "Record") records << p->Value;
            if (p->Name == "Spec") specs << p->Value;
        }
        QCOMPARE(records, (QStringList{"gain|db(v(out))", "vmax|max(v(out))"}));
        QCOMPARE(specs, (QStringList{"gain|19|21"}));
        QVERIFY(!failed(call("edit_component", {{"name", "MC1"}, {"specs", QJsonArray{"vmax||5"}}})));
        specs.clear();
        for (Property* p : sch->getComponentByName("MC1")->Props)
            if (p->Name == "Spec") specs << p->Value;
        QCOMPARE(specs, (QStringList{"vmax||5"}));
        QVERIFY(failed(call("edit_component", {{"name", "MC1"}, {"specs", QJsonArray{"gain"}}})));
        QVERIFY(failed(call("edit_component", {{"name", "R1"}, {"records", QJsonArray{"a|b"}}})));
        // Undone step by step.
        QVERIFY(!failed(call("undo")));
        specs.clear();
        for (Property* p : sch->getComponentByName("MC1")->Props)
            if (p->Name == "Spec") specs << p->Value;
        QCOMPARE(specs, (QStringList{"gain|19|21"}));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Renaming a part renames what names it: traces, equations.
    void aRenamedPartTakesItsReferencesAlong()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "Eqn"}, {"name", "Eqn1"}, {"x", 100}, {"y", 300},
                                               {"equations", QJsonObject{{"p", "i(V1)*v(out)"}, {"q", "@r1[i]"}}}})));
        QVERIFY(!failed(call("add_diagram", {{"traces", QJsonArray{"ngspice/tran.i(v1)", "ngspice/tran.v(out)", "ngspice/tran.v1#branch"}}})));
        QJsonObject r = call("edit_component", {{"name", "V1"}, {"rename", "Vin"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList vars;
        for (Graph* g : sch->a_DocDiags.front()->Graphs) vars << g->Var;
        QCOMPARE(vars, (QStringList{"ngspice/tran.i(vin)", "ngspice/tran.v(out)", "ngspice/tran.vin#branch"}));
        QCOMPARE(sch->getComponentByName("Eqn1")->getProperty("p")->Value, QStringLiteral("i(Vin)*v(out)"));
        QVERIFY2(json(r).toObject().value("renamed too").toArray().size() == 3, qPrintable(text(r)));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"rename", "RL"}})));
        QCOMPARE(sch->getComponentByName("Eqn1")->getProperty("q")->Value, QStringLiteral("@rl[i]"));
        // Not a net of that name: v(v1) stays.
        QCOMPARE(renameComponentIn("v(v1)+i(v1)", "V1", "V2"), QStringLiteral("v(v1)+i(v2)"));
        QCOMPARE(renameComponentIn("R1.I*2", "R1", "R7"), QStringLiteral("R7.I*2"));
        QCOMPARE(renameComponentIn("R10.I", "R1", "R7"), QStringLiteral("R10.I"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A diagram's title: drawn above it, part of it (moved, selected,
    // exported with it), saved after its labels, read back.
    void aDiagramHasATitleThatGoesWithIt()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QJsonObject r = call("add_diagram", {{"x", 100}, {"y", 400}, {"title", "Step response"}, {"traces", QJsonArray{"ngspice/tran.v(out)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Diagram* d = sch->a_DocDiags.front();
        QCOMPARE(d->title, QStringLiteral("Step response"));
        QVERIFY(d->save().contains("\"\" \"\" \"\" \"Step response\">"));
        int x1, y1, x2, y2;
        d->Bounding(x1, y1, x2, y2);
        QVERIFY2(y1 <= d->cy - d->y2 - d->titleHeight(), qPrintable(QString::number(y1)));   // above the frame
        QVERIFY(d->getSelected(d->cx + d->x2 / 2, d->cy - d->y2 - d->titleHeight() / 2));   // a click on it
        const QJsonObject summary = json(call("get_schematic")).toObject();
        QCOMPARE(summary.value("diagrams").toArray().first().toObject().value("title").toString(), QStringLiteral("Step response"));
        // Read back from the text, and a line without one as before.
        const QString sch1 = text(call("get_schematic", {{"format", "text"}}));
        QVERIFY(!failed(call("set_schematic", {{"text", sch1}})));
        QCOMPARE(sch->a_DocDiags.front()->title, QStringLiteral("Step response"));
        QVERIFY(!failed(call("edit_diagram", {{"title", ""}})));
        QVERIFY(sch->a_DocDiags.front()->title.isEmpty());
        QVERIFY(sch->a_DocDiags.front()->save().contains("\"\" \"\" \"\">"));   // as older versions wrote it
        QVERIFY(failed(call("edit_diagram", {{"title", "a \"quoted\" title"}})));
        QVERIFY(failed(call("edit_diagram", {{"y_axis", QJsonObject{{"label", "a\"b"}}}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // -3dB below a reference: the peak, the start (dc), or a level (0 dB).
    void aMarkerIsPlacedBelowAReference()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("save_document", {{"as", "peaking"}})));
        {
            // A low pass peaking to +1 dB at 1 kHz, from 0 dB: y in dB.
            QFile f(dir.filePath("workspace/peaking.dat.ngspice"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream d(&f);
            d << "<Qucs Dataset 26.1.3>\n<indep frequency 301>\n";
            for (int i = 0; i <= 300; ++i) d << QString::number(std::pow(10.0, 1 + i / 75.0), 'e', 12) << "\n";
            d << "</indep>\n<dep ac.vdb(out) frequency>\n";
            for (int i = 0; i <= 300; ++i) {
                const double f0 = std::pow(10.0, 1 + i / 75.0) / 1000.0;
                const double db = f0 < 1 ? 1.0 * f0 : 1.0 - 24 * std::log10(f0) * std::log10(f0) - 20 * std::log10(f0);
                d << QString::number(db, 'e', 12) << "+j0.000000000000e+00\n";
            }
            d << "</dep>\n";
        }
        QVERIFY(!failed(call("add_diagram", {{"traces", QJsonArray{"ngspice/ac.vdb(out)"}}})));
        const auto crossing = [&](const QJsonValue& reference) {
            QJsonObject args{{"at", "-3dB"}};
            if (!reference.isUndefined()) args.insert("reference", reference);
            const QJsonObject r = call("add_marker", args);
            if (failed(r)) return QJsonObject{{"error", text(r)}};
            return json(r).toObject();
        };
        const QJsonObject atPeak = crossing(QJsonValue());
        const QJsonObject atZero = crossing(0);
        const QJsonObject atDc = crossing("dc");
        const auto level = [](const QJsonObject& o) { return o.value("found").toObject().value("level").toDouble(NAN); };
        QVERIFY2(std::abs(level(atPeak) - (1.0 - 3.0)) < 0.05, QJsonDocument(atPeak).toJson().constData());
        QVERIFY2(std::abs(level(atZero) + 3.0) < 1e-9, QJsonDocument(atZero).toJson().constData());
        QVERIFY2(std::abs(level(atDc) - (0.01 - 3.0)) < 0.05, QJsonDocument(atDc).toJson().constData());
        // Below 0 dB it crosses later than below the peak.
        QVERIFY(atZero.value("found").toObject().value("crossing").toDouble() > atPeak.value("found").toObject().value("crossing").toDouble());
        QVERIFY(failed(call("add_marker", {{"at", "-3dB"}, {"reference", "the moon"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A picture of one diagram says its size in pixels, as written.
    void aDiagramsPictureSaysItsSize()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_diagram", {{"x", 100}, {"y", 500}, {"title", "Picture"}})));
        const QString png = dir.filePath("workspace/one-diagram.png");
        const QJsonObject r = call("export_image", {{"save_as", png}, {"diagram", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray pixels = json(r).toObject().value("pixels").toArray();
        const QImage image(png);
        QVERIFY(!image.isNull());
        QCOMPARE(pixels.at(0).toInt(), image.width());
        QCOMPARE(pixels.at(1).toInt(), image.height());
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // simulate: Check Schematic first (in the log's head too), and another
    // simulator for one run - the setting left as it is.
    void aRunIsCheckedFirstAndTakesAnotherSimulatorOnce()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        // A part on no ground: a warning (a ground there is, so no error).
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 400}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"name", "TR1"}, {"x", 300}, {"y", 300}})));
        QVERIFY(!failed(call("save_document", {{"as", "checked_first"}})));
        QJsonObject r = call("simulate", {{"timeout", 20}}, 40000);
        // (The simulator here is a stand-in that does not start: then the
        // check follows the error.)
        QVERIFY2(text(r).contains("Check Schematic, before the run") && text(r).contains("R1"), qPrintable(text(r)));
        // A run that starts (a real ngspice): the check heads its log.
        if (const QString ngspice = QStandardPaths::findExecutable("ngspice"); !ngspice.isEmpty()) {
            const QString was = QucsSettings.NgspiceExecutable;
            QucsSettings.NgspiceExecutable = ngspice;
            r = call("simulate", {{"timeout", 60}}, 90000);
            QucsSettings.NgspiceExecutable = was;
            QVERIFY2(!failed(r), qPrintable(text(r)));
            const QJsonObject outcome = json(r).toObject();
            QVERIFY2(outcome.value("before the run").toObject().value("warnings").toArray().size() > 0, qPrintable(text(r)));
            QVERIFY2(outcome.value("last lines").toString().startsWith("Check Schematic, before the run"), qPrintable(text(r)));
        }
        // Not installed: refused, the setting as it was.
        const int before = QucsSettings.DefaultSimulator;
        r = call("simulate", {{"simulator", "spiceopus"}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("not installed"), qPrintable(text(r)));
        QCOMPARE(QucsSettings.DefaultSimulator, before);
        QVERIFY(failed(call("simulate", {{"simulator", "pspice"}})));
        // Qucsator for one run, waited for (when it is built here).
        const QString qucsator = QDir(QCoreApplication::applicationDirPath()).filePath("../../qucsator_rf/src/qucsator_rf");
        if (QFileInfo(qucsator).isExecutable()) {
            const QString was = QucsSettings.Qucsator;
            QucsSettings.Qucsator = QFileInfo(qucsator).canonicalFilePath();
            app->simulatorList()->addItem("Qucsator", int(spicecompat::simQucsator));
            QVERIFY(!failed(call("delete", {{"names", QJsonArray{"TR1"}}})));
            QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 100}, {"y", 200}})));
            QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"name", "V1"}, {"x", 30}, {"y", 150}})));
            QVERIFY(!failed(call("add_component", {{"type", ".DC"}, {"name", "DC1"}, {"x", 300}, {"y", 300}})));
            QVERIFY(!failed(call("save_document")));
            r = call("simulate", {{"simulator", "qucsator"}, {"timeout", 60}}, 90000);
            const QJsonObject q = json(r).toObject();
            QCOMPARE(q.value("simulator").toString(), QStringLiteral("Qucsator"));
            QVERIFY2(q.value("finished").toBool(), qPrintable(text(r)));
            QVERIFY2(q.contains("succeeded"), qPrintable(text(r)));
            QVERIFY2(q.value("simulator in the settings").toString().contains("unchanged"), qPrintable(text(r)));
            QCOMPARE(QucsSettings.DefaultSimulator, before);
            for (SimMessage* m : app->findChildren<SimMessage*>()) m->slotClose();
            app->simulatorList()->removeItem(app->simulatorList()->count() - 1);
            QucsSettings.Qucsator = was;
        }
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // list_documents: the traces a dataset has not, and those whose
    // dataset is not there at all (ngspice's traces, Qucsator's dataset).
    void theWorkspaceSaysWhichTracesHaveNoData()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_diagram", {{"traces", QJsonArray{"ngspice/tran.v(out)", "xyce/tran.v(out)"}}})));
        QVERIFY(!failed(call("save_document", {{"as", "mixed"}})));
        {
            QFile f(dir.filePath("workspace/mixed.dat.ngspice"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("<Qucs Dataset 26.1.3>\n<indep time 2>\n0\n1\n</indep>\n<dep tran.v(in) time>\n0\n1\n</dep>\n");
        }
        const QJsonObject listed = json(call("list_documents", {{"search", "mixed"}})).toObject();
        QJsonObject sch, dat;
        for (const QJsonValue& v : listed.value("files").toArray()) {
            if (v.toObject().value("path").toString() == "mixed.sch") sch = v.toObject();
            if (v.toObject().value("path").toString() == "mixed.dat.ngspice") dat = v.toObject();
        }
        const QJsonArray lacking = dat.value("traces it does not have").toArray();
        QCOMPARE(lacking.size(), 1);
        QCOMPARE(lacking.first().toObject().value("trace").toString(), QStringLiteral("ngspice/tran.v(out)"));
        const QJsonArray missing = sch.value("traces without their dataset").toArray();
        QCOMPARE(missing.size(), 1);
        QCOMPARE(missing.first().toObject().value("needs").toString(), QStringLiteral("mixed.dat.xyce"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A text document whose file another program changed is loaded again,
    // unless it has changes of its own.
    void aTextChangedOnDiskIsLoadedAgain()
    {
        const QString file = dir.filePath("workspace/notes.cir");
        const auto write = [&file](const QByteArray& content) {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(content);
        };
        write("* first\n");
        QVERIFY(!failed(call("open_document", {{"path", file}})));
        auto* doc = dynamic_cast<TextDoc*>(app->getDoc());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->toPlainText(), QStringLiteral("* first\n"));
        QTest::qWait(2000);   // the watch set, and a later time on the file
        write("* second\n");
        QTRY_COMPARE_WITH_TIMEOUT(doc->toPlainText(), QStringLiteral("* second\n"), 8000);
        QVERIFY(!doc->getDocChanged());
        // With changes of its own: left.
        doc->insertPlainText("* mine\n");
        QVERIFY(doc->getDocChanged());
        QTest::qWait(1100);
        write("* third\n");
        QTest::qWait(3000);
        QVERIFY(doc->toPlainText().contains("* mine"));
        QVERIFY(!doc->toPlainText().contains("* third"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // build_verilog_a: the compiler's errors with their lines, now; a
    // module's parameters and a .model card from its source.
    void verilogAIsCompiledAndDescribed()
    {
#ifdef Q_OS_WIN
        QSKIP("A shell script stands in for OpenVAF.");
#endif
        const QString va = dir.filePath("workspace/good.va");
        {
            QFile f(va);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("`include \"disciplines.vams\"\nmodule good(p, n);\n  inout p, n;\n  electrical p, n;\n"
                    "  (* desc=\"Resistance\", units=\"Ohm\" *) parameter real r = 1e3 from (0:inf);\n"
                    "  (* desc=\"Scale\", type=\"instance\" *) parameter real m = 1;\n"
                    "  analog I(p,n) <+ V(p,n)/r*m;\nendmodule\n");
        }
        const QString bad = dir.filePath("workspace/bad.va");
        {
            QFile f(bad);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("module bad(p, n);\n  analog begin\n    I(p,n) <+ V(p,n)\n    I(p,n) <+ nothing;\n  end\nendmodule\n");
        }
        const QString fake = dir.filePath("fake-openvaf.sh");
        {
            QFile f(fake);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray("#!/bin/sh\n"
                               "case \"$1\" in *bad.va)\n"
                               "  echo \"error: unexpected token identifier; expected ';'\"\n"
                               "  echo \"  --> $1:4:5\"\n"
                               "  echo \"  |\"\n"
                               "  echo \"4 |     I(p,n) <+ nothing;\"\n"
                               "  echo \"  |     ^ unexpected token\"\n"
                               "  echo\n"
                               "  echo \"warning: unused variable\"\n"
                               "  echo \"  --> $1:2:3\"\n"
                               "  echo\n"
                               "  echo \"error: could not compile bad.va due to 1 previous error\"\n"
                               "  exit 65;;\n"
                               "esac\n"
                               "echo \"Finished building $1\"\n"
                               "printf 'x' > \"${1%.va}.osdi\"\n"));
            f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        }
        const QString before = QucsSettings.OpenVAFExecutable;
        QucsSettings.OpenVAFExecutable = fake;
        QJsonObject r = call("build_verilog_a", {{"file", "bad.va"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QVERIFY(!o.value("compiled").toBool());
        const QJsonArray errors = o.value("errors").toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toObject().value("line").toInt(), 4);
        QCOMPARE(errors.first().toObject().value("column").toInt(), 5);
        QCOMPARE(errors.first().toObject().value("source").toString(), QStringLiteral("I(p,n) <+ nothing;"));
        QCOMPARE(errors.first().toObject().value("notes").toArray().first().toString(), QStringLiteral("unexpected token"));
        QCOMPARE(o.value("warnings").toArray().size(), 1);
        r = call("build_verilog_a", {{"file", va}});
        o = json(r).toObject();
        QVERIFY2(o.value("compiled").toBool(), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(dir.filePath("workspace/good.osdi")));
        // Open with unsaved changes: said what to do.
        QVERIFY(!failed(call("open_document", {{"path", va}})));
        dynamic_cast<TextDoc*>(app->getDoc())->insertPlainText("// changed\n");
        r = call("build_verilog_a");
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("'unsaved'"), qPrintable(text(r)));
        QVERIFY(!failed(call("build_verilog_a", {{"unsaved", "as_saved"}})));
        QucsSettings.OpenVAFExecutable = before;
        // Described: from its source (the fake library is none).
        r = call("describe_component_type", {{"type", "good"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        o = json(r).toObject();
        QCOMPARE(o.value("module").toString(), QStringLiteral("good"));
        const QJsonArray params = o.value("parameters").toArray();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params.at(0).toObject().value("name").toString(), QStringLiteral("r"));
        QCOMPARE(params.at(0).toObject().value("units").toString(), QStringLiteral("Ohm"));
        QCOMPARE(params.at(1).toObject().value("kind").toString(), QStringLiteral("instance"));
        QVERIFY2(o.value("model card").toString().startsWith(".model good_model good (r=1e3"), qPrintable(text(r)));
        QVERIFY(!failed(call("describe_component_type", {{"type", "good.va"}})));
        QVERIFY(failed(call("describe_component_type", {{"type", "no_such_module"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // tune: the value that makes a number come out right, in a few runs;
    // one step to undo.
    void aPartIsTunedToATarget()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Vdc V1 1 100 200 18 -26 0 1 \"10 V\" 1>\n"
                                                        "  <GND * 1 100 230 0 0 0 0>\n"
                                                        "  <R R1 1 200 100 15 -26 0 0 " + R +
                                                        "  <R R2 1 300 200 15 -26 0 1 " + R +
                                                        "  <GND * 1 300 230 0 0 0 0>\n"
                                                        "  <.TR TR1 1 100 400 0 57 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"11\" 0>\n"
                                                        "</Components>\n<Wires>\n"
                                                        "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
                                                        "  <230 100 300 100 \"out\" 250 70 30 \"\">\n  <300 100 300 170 \"\" 0 0 0 \"\">\n"
                                                        "</Wires>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // (divider.sch is there from aSubcircuitIsMadeOfParts: written over when
        // 'replace' says so - no one here to ask.)
        r = call("save_document", {{"as", "divider"}});
        QVERIFY(failed(r) && text(r).contains("'replace'"));
        QVERIFY(!failed(call("save_document", {{"as", "divider"}, {"replace", true}})));
        // out = 10 V * 1k / (R1 + 1k) = 2.5 V: R1 = 3k.
        r = call("tune", {{"component", "R1"}, {"target", 2.5}, {"range", QJsonArray{"100", "100k"}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}}, 300000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QVERIFY2(o.value("within tolerance").toBool(), qPrintable(text(r)));
        double value = 0, factor = 1;
        QString unit;
        misc::str2num(sch->getComponentByName("R1")->getProperty("R")->Value, value, unit, factor);
        QVERIFY2(std::abs(value * factor - 3000) < 60, qPrintable(text(r)));
        QVERIFY2(o.value("runs").toArray().size() <= 12, qPrintable(text(r)));
        // One step to undo: 1k again.
        QVERIFY(!failed(call("undo")));
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("1k"));
        // A table of values, measured on the transient: nothing set.
        r = call("tune", {{"component", "R2"}, {"values", QJsonArray{"1k", "3k"}},
                          {"measure", QJsonObject{{"variable", "tran.v(out)"}, {"what", "final"}}}}, 300000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        o = json(r).toObject();
        const QJsonArray runs = o.value("runs").toArray();
        QCOMPARE(runs.size(), 2);
        QVERIFY2(std::abs(runs.at(0).toObject().value("measured").toDouble() - 5.0) < 0.01, qPrintable(text(r)));
        QVERIFY2(std::abs(runs.at(1).toObject().value("measured").toDouble() - 7.5) < 0.01, qPrintable(text(r)));
        QCOMPARE(sch->getComponentByName("R2")->getProperty("R")->Value, QStringLiteral("1k"));
        // Changed in the window while it runs (the user): left as they made
        // it - not put back, nothing applied over it.
        {
            QTimer poll;
            bool edited = false;
            connect(&poll, &QTimer::timeout, this, [&] {
                Component* c = sch->getComponentByName("R2");
                if (!edited && c != nullptr && c->getProperty("R")->Value.startsWith("3k")) {
                    c->getProperty("R")->Value = "47k";
                    edited = true;
                }
            });
            poll.start(1);
            r = call("tune", {{"component", "R2"}, {"values", QJsonArray{"1k", "3k"}},
                              {"measure", QJsonObject{{"variable", "tran.v(out)"}, {"what", "final"}}}}, 300000);
            poll.stop();
            if (edited) {
                QVERIFY2(json(r).toObject().value("set").toString().contains("was changed to 47k"), qPrintable(text(r)));
                QCOMPARE(sch->getComponentByName("R2")->getProperty("R")->Value, QStringLiteral("47k"));
                QVERIFY(!failed(call("edit_component", {{"name", "R2"}, {"properties", QJsonObject{{"R", "1k"}}}})));
            }
        }
        // Not bracketed: said - and the closest value tried, which misses the
        // target, not set.
        r = call("tune", {{"component", "R1"}, {"target", 20}, {"range", QJsonArray{"1k", "2k"}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}}, 300000);
        QVERIFY2(json(r).toObject().value("stopped").toString().contains("widen"), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("set").toString().startsWith("not set"), qPrintable(text(r)));
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("1k"));
        // A value that is a number alone (330): no unit to keep - the number
        // is not taken for one (it was, and every value tried was "3k 330").
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "330"}}}})));
        r = call("tune", {{"component", "R1"}, {"target", 2.5}, {"range", QJsonArray{100, 100000}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}}, 300000);
        o = json(r).toObject();
        QVERIFY2(o.value("within tolerance").toBool(), qPrintable(text(r)));
        // (Each value in six figures at most: 1k, not 999.999719.)
        for (const QJsonValue& run : o.value("runs").toArray()) {
            const QString v = run.toObject().value("value").toString();
            QVERIFY2(!v.contains(' ') && v.count(QRegularExpression("\\d")) <= 6, qPrintable(text(r)));
        }
        misc::str2num(sch->getComponentByName("R1")->getProperty("R")->Value, value, unit, factor);
        QVERIFY2(std::abs(value * factor - 3000) < 60 && unit.size() <= 1, qPrintable(text(r)));
        QVERIFY(!failed(call("undo")));
        QVERIFY(failed(call("tune", {{"component", "R1"}, {"target", 1}})));
        QVERIFY(failed(call("tune", {{"component", "R9"}, {"target", 1}, {"range", QJsonArray{1, 2}}, {"measure", QJsonObject{{"operating_point", "out"}}}})));
        QucsSettings.NgspiceExecutable = before;
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A document by its file's name alone; relative paths from the project.
    void aDocumentIsFoundByItsName()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("save_document", {{"as", "by_name"}})));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"path", "by_name.sch"}, {"type", "R"}, {"x", 100}, {"y", 100}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // (Changed, it came to the front.)
        r = call("close_document", {{"path", "by_name.sch"}, {"unsaved", "discard"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // What the user changes between two calls, told part by part; the
    // steps to undo in words, and undo to one of them.
    void changesAreToldPartByPartAndStepsInWords()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}, {"properties", QJsonObject{{"R", "47k"}}}})));
        const quint64 us = 303;
        const auto as = [this](const QString& tool, const QJsonObject& args = {}) { return control->callNow(tool, args, 30000, us); };
        QVERIFY(!failed(as("get_schematic")));
        // The user: a value changed, a part added, a part moved.
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "67k"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 400}, {"y", 100}})));
        QVERIFY(!failed(call("edit_component", {{"name", "C1"}, {"x", 400}, {"y", 300}})));
        QJsonObject r = as("get_state");
        const QString told = text(r);
        QVERIFY2(told.contains("What changed:"), qPrintable(told.right(600)));
        QVERIFY2(told.contains("R1: R 47k → 67k"), qPrintable(told.right(600)));
        QVERIFY2(told.contains("C1 added (C, C="), qPrintable(told.right(600)));
        QVERIFY2(!told.contains("read it again"), qPrintable(told.right(600)));
        // In words, step by step.
        r = call("undo_history");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject history = json(r).toObject();
        const QJsonArray steps = history.value("steps").toArray();
        QVERIFY(steps.size() >= 4);
        QString all;
        for (const QJsonValue& v : steps) all += v.toObject().value("change").toString() + "\n";
        QVERIFY2(all.contains("R1 added") && all.contains("R1: R 47k → 67k") && all.contains("C1 added") && all.contains("C1: moved"),
                 qPrintable(all));
        // Back to how it was after R1 was changed: C1 gone.
        int after = -1;
        for (const QJsonValue& v : steps)
            if (v.toObject().value("change").toString().contains("47k → 67k")) after = v.toObject().value("step").toInt();
        QVERIFY(after > 0);
        r = call("undo", {{"to", after}});
        QVERIFY2(!failed(r) && text(r).contains("C1 deleted"), qPrintable(text(r)));
        QVERIFY(sch->getComponentByName("C1") == nullptr);
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("67k"));
        // And forward again.
        r = call("undo", {{"to", history.value("at").toInt()}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(sch->getComponentByName("C1") != nullptr);
        QVERIFY(failed(call("undo", {{"to", 9999}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // "selection": what the user selected, instead of names.
    void theSelectionIsTakenInsteadOfNames()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R3"}, {"x", 500}, {"y", 100}})));
        QVERIFY(failed(call("move", {{"selection", true}, {"dx", 1}})));   // nothing selected
        QVERIFY(!failed(call("select", {{"names", QJsonArray{"R1", "R2"}}})));
        QJsonObject r = call("get_schematic", {{"selection", true}});
        QStringList names;
        for (const QJsonValue& v : json(r).toObject().value("components").toArray()) names << v.toObject().value("name").toString();
        QVERIFY2(names == (QStringList{"R1", "R2"}), qPrintable(text(r).left(1500)));
        r = call("add_painting", {{"type", "rectangle"}, {"around", "selection"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QRect box = sch->a_DocPaints.back()->boundingRect();
        QVERIFY2(box.contains(sch->getComponentByName("R1")->boundingRect()) && box.contains(sch->getComponentByName("R2")->boundingRect())
                     && !box.contains(sch->getComponentByName("R3")->boundingRect()),
                 qPrintable(text(r)));
        QVERIFY(!failed(call("select", {{"names", QJsonArray{"R1", "R2"}}})));
        r = call("move", {{"selection", true}, {"dx", 0}, {"dy", 100}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(sch->getComponentByName("R1")->cy, 200);
        QCOMPARE(sch->getComponentByName("R3")->cy, 100);
        QVERIFY(!failed(call("select", {{"names", QJsonArray{"R3"}}})));
        QVERIFY(!failed(call("delete", {{"selection", true}})));
        QVERIFY(sch->getComponentByName("R3") == nullptr);
        QVERIFY(sch->getComponentByName("R1") != nullptr);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Tables, the spectrum, an eye, a Monte Carlo's distribution; a
    // family measured as a table.
    void resultsAreMeasuredAsTablesSpectraAndEyes()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("save_document", {{"as", "signals"}})));
        {
            QFile f(dir.filePath("workspace/signals.dat.ngspice"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream d(&f);
            const int n = 4001;
            d << "<Qucs Dataset 26.1.3>\n<indep time " << n << ">\n";
            for (int i = 0; i < n; ++i) d << QString::number(i * 1e-6, 'e', 12) << "\n";   // 4 ms
            d << "</indep>\n<dep tran.v(tone) time>\n";
            for (int i = 0; i < n; ++i) {
                const double t = i * 1e-6;
                d << QString::number(std::sin(2 * M_PI * 1000 * t) + 0.1 * std::sin(2 * M_PI * 3000 * t), 'e', 12) << "\n";
            }
            // Bits of 40 us, a PRBS, edges of 4 us.
            const int bits[] = {1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1};
            d << "</dep>\n<dep tran.v(data) time>\n";
            for (int i = 0; i < n; ++i) {
                const double t = i * 1e-6;
                const int k = std::min(int(t / 40e-6), 99);
                const double into = t - k * 40e-6;
                const double now = bits[k % 20], before = k > 0 ? bits[(k - 1) % 20] : now;
                const double v = into < 4e-6 ? before + (now - before) * into / 4e-6 : now;
                d << QString::number(v, 'e', 12) << "\n";
            }
            d << "</dep>\n";
        }
        QJsonObject r = call("get_dataset", {{"variables", QJsonArray{"tran.v(tone)"}}, {"measure", QJsonArray{"fft"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject fft = json(r).toObject().value("variables").toArray().first().toObject().value("measurements").toObject().value("fft").toObject();
        QVERIFY2(std::abs(fft.value("value").toDouble() - 1000) < 30, qPrintable(text(r)));
        QVERIFY2(std::abs(fft.value("strongest").toObject().value("amplitude").toDouble() - 1.0) < 0.1, qPrintable(text(r)));
        bool third = false;
        for (const QJsonValue& l : fft.value("lines").toArray())
            if (std::abs(l.toObject().value("frequency").toDouble() - 3000) < 30) third = std::abs(l.toObject().value("dBc").toDouble() + 20) < 2;
        QVERIFY2(third, qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"tran.v(data)"}}, {"measure", QJsonArray{"eye"}}, {"bit_period", 40e-6}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject eye = json(r).toObject().value("variables").toArray().first().toObject().value("measurements").toObject().value("eye").toObject();
        QVERIFY2(std::abs(eye.value("height").toDouble() - 1.0) < 0.05, qPrintable(text(r)));
        QVERIFY2(eye.value("width, UI").toDouble() > 0.9, qPrintable(text(r)));
        QVERIFY(failed(call("get_dataset", {{"variables", QJsonArray{"tran.v(data)"}}, {"measure", QJsonArray{"eye"}}, {"bit_period", -1}})));
        // A Monte Carlo's workbook: a column per value, a row per sample.
        {
            QFile f(dir.filePath("workspace/mc_results.csv"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream d(&f);
            d << "sample,gain,fc\n";
            std::mt19937 gen(5);
            std::normal_distribution<double> gain(20, 0.5), fc(1e3, 30);
            for (int i = 1; i <= 400; ++i) d << i << "," << gain(gen) << "," << fc(gen) << "\n";
        }
        r = call("get_dataset", {{"path", "mc_results.csv"}, {"variables", QJsonArray{"gain"}}, {"measure", QJsonArray{"distribution"}}, {"level", 20}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject dist = json(r).toObject().value("variables").toArray().first().toObject().value("measurements").toObject().value("distribution").toObject();
        QCOMPARE(dist.value("count").toInt(), 400);
        QVERIFY2(std::abs(dist.value("mean").toDouble() - 20) < 0.1 && std::abs(dist.value("standard deviation").toDouble() - 0.5) < 0.08,
                 qPrintable(text(r)));
        QVERIFY2(std::abs(dist.value("at or above level").toDouble() - 50) < 8, qPrintable(text(r)));
        QVERIFY(dist.value("histogram").toArray().size() >= 10);
        // A family measured: a row for each curve.
        {
            QFile f(dir.filePath("workspace/signals.dat.ngspice"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream d(&f);
            d << "<Qucs Dataset 26.1.3>\n<indep time 101>\n";
            for (int i = 0; i <= 100; ++i) d << i * 1e-5 << "\n";
            d << "</indep>\n<indep r1 3>\n1000\n2000\n4000\n</indep>\n<dep tran.v(out) time r1>\n";
            for (double tau : {1e-4, 2e-4, 4e-4})
                for (int i = 0; i <= 100; ++i) d << 1 - std::exp(-i * 1e-5 / tau) << "\n";
            d << "</dep>\n";
        }
        r = call("get_dataset", {{"variables", QJsonArray{"tran.v(out)"}}, {"measure", QJsonArray{"rise_time"}}, {"at", QJsonArray{2e-4}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject table = json(r).toObject().value("variables").toArray().first().toObject().value("table").toObject();
        QCOMPARE(table.value("columns").toArray().first().toString(), QStringLiteral("r1"));
        const QJsonArray rows = table.value("rows").toArray();
        QCOMPARE(rows.size(), 3);
        QVERIFY2(rows.at(2).toArray().at(2).toDouble() > rows.at(0).toArray().at(2).toDouble() * 3, qPrintable(text(r)));   // rises slower
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A data display: made for a saved schematic, plotted on, exported.
    void aDataDisplayTakesTheReportsPlots()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(failed(call("new_document", {{"kind", "data_display"}})));   // no file yet
        QVERIFY(!failed(call("save_document", {{"as", "report"}})));
        QJsonObject r = call("new_document", {{"kind", "data_display"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(app->getDoc()->getDocName().endsWith("report.dpl"));
        r = call("add_diagram", {{"path", "report.dpl"}, {"title", "For the report"}, {"traces", QJsonArray{"ngspice/tran.v(out)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("export_image", {{"path", "report.dpl"}, {"save_as", dir.filePath("workspace/report.png")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!QImage(dir.filePath("workspace/report.png")).isNull());
        QVERIFY(!failed(call("close_document", {{"path", "report.dpl"}, {"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", "report.sch"}, {"unsaved", "discard"}})));
    }

    // A datasheet's text, a word looked for.
    void aDatasheetIsRead()
    {
#ifndef QUCS_HAVE_QTPDF
        QSKIP("no PDF reader in this build");
#else
        const QString pdf = dir.filePath("workspace/datasheet.pdf");
        {
            QPdfWriter writer(pdf);
            QPainter painter(&writer);
            painter.setFont(QFont("Helvetica", 12));
            painter.drawText(QRect(200, 200, 8000, 400), "2N3904 NPN switching transistor");
            painter.drawText(QRect(200, 800, 8000, 400), "hFE (BF) = 300 at IC = 10 mA");
            writer.newPage();
            painter.drawText(QRect(200, 200, 8000, 400), "Package: TO-92");
        }
        QJsonObject r = call("read_pdf", {{"path", "datasheet.pdf"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QCOMPARE(o.value("pages in all").toInt(), 2);
        QVERIFY2(o.value("pages").toArray().first().toObject().value("text").toString().contains("2N3904"), qPrintable(text(r)));
        r = call("read_pdf", {{"path", "datasheet.pdf"}, {"search", "TO-92"}});
        o = json(r).toObject();
        QCOMPARE(o.value("found").toArray().size(), 1);
        QCOMPARE(o.value("found").toArray().first().toObject().value("page").toInt(), 2);
        QVERIFY(failed(call("read_pdf", {{"path", "datasheet.pdf"}, {"pages", 9}})));
        QVERIFY(failed(call("read_pdf", {{"path", "nothing.pdf"}})));
#endif
    }

    // A part by its values: the libraries and the SPICE models.
    void partsAreFoundByTheirValues()
    {
        {
            QFile f(dir.filePath("workspace/vendor.lib"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("* a vendor's models\n.model QV210 NPN(IS=1e-14 BF=210\n+ VAF=80)\n.model QV90 NPN(BF=90)\n.model MP1 PMOS(VTO=-0.7)\n");
        }
        QJsonObject r = call("find_library_component", {{"type", "npn"}, {"near", QJsonObject{{"BF", 200}}}, {"limit", 5}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray found = json(r).toObject().value("found").toArray();
        QVERIFY(!found.isEmpty());
        bool vendor = false;
        for (const QJsonValue& v : found) {
            const QJsonObject o = v.toObject();
            if (o.value("model").toString() == "QV210") {
                vendor = true;
                QVERIFY2(o.value("model card").toString().contains("VAF=80"), qPrintable(text(r)));   // its continuation too
            }
            if (o.contains("library")) {
                double bf = 0, factor = 1;
                QString unit;
                misc::str2num(o.value("values").toObject().value("Bf").toString(), bf, unit, factor);
                QVERIFY2(bf > 100 && bf < 400, qPrintable(text(r)));
                QCOMPARE(o.value("place").toObject().value("type").toString(), QStringLiteral("Lib"));
            }
        }
        QVERIFY2(vendor, qPrintable(text(r)));
        // Placed as it says.
        if (QFileInfo::exists(QucsSettings.LibDir)) {
            QJsonObject lib;
            for (const QJsonValue& v : found)
                if (v.toObject().contains("library")) lib = v.toObject();
            if (!lib.isEmpty()) {
                QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
                const QJsonObject place = lib.value("place").toObject();
                r = call("add_component", {{"type", place.value("type")}, {"x", 100}, {"y", 100}, {"properties", place.value("properties")}});
                QVERIFY2(!failed(r), qPrintable(text(r)));
                QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
            }
        }
        r = call("find_library_component", {{"search", "QV90"}});
        QCOMPARE(json(r).toObject().value("found").toArray().size(), 1);
        QVERIFY(failed(call("find_library_component")));
        QVERIFY(failed(call("find_library_component", {{"near", QJsonObject{{"BF", "lots"}}}})));
        QFile::remove(dir.filePath("workspace/vendor.lib"));
    }

    // A SPICE netlist becomes a schematic: parts, nets as labels, grounds,
    // models, subcircuits, analyses - and it netlists back as it was.
    void aNetlistBecomesASchematic()
    {
        const QString netlist =
            "A divider and a follower\n"
            "V1 in 0 DC 5 AC 1\n"
            "R1 in out 1k\n"
            "C1 out 0 10n\n"
            "Q1 vcc out e QN\n"
            "Re e 0 2k\n"
            "Vcc vcc 0 12\n"
            "E1 amp 0 out 0 10\n"
            "X1 out y2 buf\n"
            ".model QN NPN(BF=100\n+ IS=1e-15)\n"
            ".subckt buf a y\nRb a y 1k\n.ends buf\n"
            ".param gain=10\n"
            ".tran 1u 1m\n"
            ".ac dec 10 1 1meg\n"
            ".four 1k v(out)\n"
            ".end\n";
        QJsonObject r = call("import_netlist", {{"text", netlist}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject o = json(r).toObject();
        QCOMPARE(o.value("title").toString(), QStringLiteral("A divider and a follower"));
        QCOMPARE(o.value("parts").toArray().size(), 8);
        QCOMPARE(o.value("models").toInt(), 1);
        QVERIFY2(o.value("grounds").toInt() >= 5, qPrintable(text(r)));
        QVERIFY2(o.value("not taken").toArray().size() == 1 && o.value("not taken").toArray().first().toString().startsWith(".four"),
                 qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(o.value("subcircuits").toString()));
        Schematic* sch = front();
        QCOMPARE(sch->getComponentByName("Q1")->Model, QStringLiteral("NPN_SPICE"));
        QCOMPARE(sch->getComponentByName("V1")->Props.first()->Value, QStringLiteral("DC 5 AC 1"));
        QCOMPARE(sch->getComponentByName("E1")->Model, QStringLiteral("VCVS"));
        QCOMPARE(sch->getComponentByName("X1")->getProperty("Model")->Value, QStringLiteral("buf"));
        QString spice = text(call("get_netlist"));
        spice.replace(QRegularExpression("[ \t]+"), " ");
        QVERIFY2(spice.contains("\nR1 in out 1k"), qPrintable(spice));
        QVERIFY2(spice.contains("\nV1 in 0 DC 5 AC 1"), qPrintable(spice));
        QVERIFY2(spice.contains("\nQ1 vcc out e QN"), qPrintable(spice));
        QVERIFY2(spice.contains("\nX1 out y2 buf"), qPrintable(spice));
        QVERIFY2(spice.contains(".model QN NPN(BF=100 IS=1e-15)"), qPrintable(spice));
        QVERIFY2(spice.contains("\nE1 amp 0 out 0 10"), qPrintable(spice));
        QVERIFY2(spice.contains("tran 1e-06 0.001") && spice.contains("ac dec 10 1 1e+06"), qPrintable(spice));
        // The nets joined as in the netlist: out has R1, C1, Q1, E1, X1.
        const QJsonObject summary = json(call("get_schematic")).toObject();
        bool out = false;
        for (const QJsonValue& n : summary.value("nets").toArray())
            if (n.toObject().value("net").toString() == "out") out = n.toObject().value("pins").toArray().size() == 5;
        QVERIFY2(out, qPrintable(QJsonDocument(summary.value("nets").toArray()).toJson()));
        // It simulates, where there is ngspice.
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (!ngspice.isEmpty()) {
            const QString before = QucsSettings.NgspiceExecutable;
            QucsSettings.NgspiceExecutable = ngspice;
            QVERIFY(!failed(call("save_document", {{"as", "imported_follower"}})));
            r = call("simulate", {{"timeout", 60}}, 90000);
            QucsSettings.NgspiceExecutable = before;
            QVERIFY2(json(r).toObject().value("succeeded").toBool(), qPrintable(text(r)));
        }
        QVERIFY(failed(call("import_netlist", {{"text", "* nothing here\n.end\n"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A symbol with pins on four sides.
    void aSymbolIsLaidOutOnFourSides()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        int y = 100;
        for (const char* name : {"in", "out", "vdd", "gnd", "bias"}) {
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}})));
            y += 60;
        }
        QVERIFY(!failed(call("save_document", {{"as", "fourside"}})));
        QJsonObject r = call("make_symbol", {{"sides", QJsonObject{{"in", "left"}, {"out", "right"}, {"bias", "left"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString pins = QJsonDocument(json(r).toObject().value("pins").toArray()).toJson();
        QVERIFY2(pins.contains("in: left") && pins.contains("out: right") && pins.contains("vdd: top") && pins.contains("gnd: bottom")
                     && pins.contains("bias: left"),
                 qPrintable(pins));
        QVERIFY(sch->getSymbolMode());
        int ports = 0;
        for (Painting* p : sch->a_SymbolPaints) ports += p->Name == ".PortSym " ? 1 : 0;
        QCOMPARE(ports, 5);
        QVERIFY(failed(call("make_symbol", {{"sides", QJsonObject{{"in", "upside down"}}}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Projects made, a schematic copied with its results, scratch cleared.
    void projectsAndCopiesAreTended()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", "orig"}})));
        const QString ws = dir.filePath("workspace");
        {
            QFile f(ws + "/orig.dat.ngspice");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Dataset 26.1.3>\n<indep time 2>\n0\n1\n</indep>\n");
            QFile g(ws + "/orig.dpl");
            QVERIFY(g.open(QIODevice::WriteOnly));
            g.write("<Qucs Schematic 26.1.3>\n<Properties>\n  <DataSet=orig.dat>\n  <DataDisplay=orig.sch>\n</Properties>\n");
        }
        QJsonObject r = call("copy_document", {{"to", "orig_copy"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(ws + "/orig_copy.sch") && QFileInfo::exists(ws + "/orig_copy.dat.ngspice") && QFileInfo::exists(ws + "/orig_copy.dpl"));
        QFile copied(ws + "/orig_copy.sch");
        QVERIFY(copied.open(QIODevice::ReadOnly));
        const QString text1 = QString::fromUtf8(copied.readAll());
        QVERIFY2(text1.contains("<DataSet=orig_copy.dat>") && text1.contains("<DataDisplay=orig_copy.dpl>"), qPrintable(text1.left(400)));
        QFile dpl(ws + "/orig_copy.dpl");
        QVERIFY(dpl.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(dpl.readAll()).contains("<DataDisplay=orig_copy.sch>"));
        QVERIFY(failed(call("copy_document", {{"to", "orig_copy"}})));   // there: 'replace'
        QVERIFY(!failed(call("copy_document", {{"to", "orig_copy"}, {"replace", true}, {"results", false}})));
        // A project: made, not opened while a document has unsaved changes.
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 300}, {"y", 100}})));
        r = call("new_project", {{"name", "round5"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo(ws + "/round5_prj/Scratch").isDir());
        QVERIFY2(json(r).toObject().value("opened").toString().startsWith("not opened"), qPrintable(text(r)));
        QVERIFY(failed(call("new_project", {{"name", "round5"}})));
        QVERIFY(failed(call("open_project", {{"name", "round5"}})));   // unsaved changes
        // The workspace itself, or the home folder, is no project - told,
        // not a message box that waits (bug hunt 2026-09-26, E2).
        r = call("open_project", {{"name", "."}});
        QVERIFY2(failed(r) && text(r).contains("workspace folder"), qPrintable(text(r)));
        r = call("open_project", {{"name", QDir::homePath()}});
        QVERIFY2(failed(r) && text(r).contains("home folder"), qPrintable(text(r)));
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        // Copied into it by its name.
        r = call("copy_document", {{"to", "round5"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(ws + "/round5_prj/orig.sch"));
        // Scratch: the schematic's own subfolder, cleared.
        // No project open: the folder schematics of no project share - its
        // files when the last run there was this one's, never the folder
        // or what else is in it (it was trashed whole).
        const QString scratch = misc::scratchDirFor(ws + "/orig.sch");
        QCOMPARE(scratch, QucsSettings.S4Qworkdir);
        QDir().mkpath(scratch + "/other");
        {
            // (Another's file there: not its last run's - bug hunt
            // 2026-09-30, D3.)
            QFile f(scratch + "/another_run.txt");
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        const auto netlist = [&](const QString& of) {
            QFile f(scratch + "/spice4qucs.cir");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(("* Qucs " PACKAGE_VERSION "  " + of + "\n").toUtf8());
        };
        netlist(ws + "/another.sch");
        r = call("clean_scratch");
        QVERIFY2(!failed(r) && text(r).contains("nothing of its own is there"), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(scratch + "/spice4qucs.cir"));
        netlist(ws + "/orig.sch");
        r = call("clean_scratch");
        QVERIFY2(!failed(r) && text(r).contains("In the trash"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(scratch + "/spice4qucs.cir"));
        QVERIFY(QFileInfo::exists(qEnvironmentVariable("QUCS_TRASH_DIR") + "/spice4qucs.cir"));   // (the test's trash)
        QVERIFY(QFileInfo(scratch + "/other").isDir() && QFileInfo(scratch).isDir());
        QVERIFY(QFileInfo::exists(scratch + "/another_run.txt"));
        QVERIFY(QFileInfo::exists(ws + "/orig.dat.ngspice"));   // datasets only when asked
        QVERIFY(!failed(call("clean_scratch", {{"datasets", true}})));
        QVERIFY(!QFileInfo::exists(ws + "/orig.dat.ngspice"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A schematic's results are its Data Set's, not of its file's name (bug
    // hunt 2026-09-30, A2-A5, A7, D2): clean_scratch and copy_document find
    // them by it, and pass an import by; a data display made for it reads
    // it; a simulation's names and an import's are kept apart; a Data Set
    // or Data Display is a name beside the schematic; kept netlists of
    // datasets that are gone go.
    void theDataSetNamesTheResults()
    {
        const QString folder = QucsSettings.qucsWorkspaceDir.absoluteFilePath("datasets");
        QVERIFY(QDir().mkpath(folder));
        const auto write = [&folder](const QString& name, const QByteArray& bytes) {
            QFile f(folder + "/" + name);
            if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        };
        const QByteArray run = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 2>\n0\n1\n</indep>\n<dep v(out) time>\n0\n1\n</dep>\n";

        // A7: names beside it ("run" a .dat), as a file has them too.
        QCOMPARE(QucsDoc::fileBeside("../up.dat", "amp.dat"), QStringLiteral("up.dat"));
        QCOMPARE(QucsDoc::fileBeside("..", "amp.dat"), QStringLiteral("amp.dat"));
        QCOMPARE(QucsDoc::fileBeside("sub\\x.dpl", "amp.dpl"), QStringLiteral("x.dpl"));
        QCOMPARE(QucsDoc::fileBeside("", "amp.dat"), QString());
        QCOMPARE(QucsDoc::dataSetBeside("run", "amp.dat"), QStringLiteral("run.dat"));
        QCOMPARE(QucsDoc::dataSetBeside("run.DAT", "amp.dat"), QStringLiteral("run.DAT"));
        write("up.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataSet=../up>\n  <DataDisplay=../../out.dpl>\n</Properties>\n"
                        "<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n");
        QJsonObject r = call("open_document", {{"path", folder + "/up.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(front()->getDataSet(), QStringLiteral("up.dat"));
        QCOMPARE(front()->getDataDisplay(), QStringLiteral("out.dpl"));
        front()->setDataSet("../../elsewhere.dat");
        QCOMPARE(front()->getDataSet(), QStringLiteral("elsewhere.dat"));
        front()->setDataSet("..");
        QCOMPARE(front()->getDataSet(), QStringLiteral("elsewhere.dat"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"x", 100}, {"y", 300}})));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/amp.sch"}, {"replace", true}})));
        Schematic* amp = front();
        // A Data Display that is no .dpl: refused, and no file of it made -
        // nor one there opened (as text).
        amp->setDataDisplay("amp.txt");
        r = call("add_diagram", {{"document", "data_display"}, {"type", "rect"}});
        QVERIFY2(failed(r) && text(r).contains("no data display (.dpl)"), qPrintable(text(r)));
        r = call("new_document", {{"kind", "data_display"}});
        QVERIFY2(failed(r) && text(r).contains("no data display (.dpl)"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/amp.txt"));
        write("amp.txt", "notes\n");
        r = call("add_diagram", {{"document", "data_display"}, {"type", "rect"}});
        QVERIFY2(failed(r) && text(r).contains("no data display (.dpl)"), qPrintable(text(r)));
        QCOMPARE(front(), amp);
        for (QucsDoc* d : app->allDocuments()) QVERIFY(!d->getDocName().endsWith("amp.txt"));   // (not opened)
        amp->setDataDisplay("amp.dpl");
        // A4: its Data Set another: the data display made for it reads it.
        amp->setDataSet("run.dat");
        QVERIFY(!failed(call("save_document")));
        r = call("add_diagram", {{"document", "data_display"}, {"type", "rect"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(front()->getDocName().endsWith("amp.dpl"));
        QCOMPARE(front()->getDataSet(), QStringLiteral("run.dat"));
        QVERIFY(!failed(call("close_document", {{"path", folder + "/amp.dpl"}, {"unsaved", "discard"}})));
        QCOMPARE(front(), amp);

        // An import of the schematic's own name beside it, and its run.
        write("bench.csv", "f,g\n1,2\n2,3\n");
        qucs_s::dataimport::Imported imported;
        QString error;
        QVERIFY2(qucs_s::dataimport::importFile(folder, folder + "/bench.csv", {}, &imported, &error, nullptr, "amp"), qPrintable(error));
        write("run.dat.ngspice", run);
        QVERIFY(misc::keepRunNetlist(folder + "/run.dat.ngspice", "* Qucs " PACKAGE_VERSION "  x.sch\nR1 1 0 1k\n.end\n", folder + "/amp.sch"));
        const QString kept = misc::runNetlistFile(folder + "/run.dat.ngspice");
        QVERIFY(QFileInfo::exists(kept));
        // A3: copied with its run (and that run's netlist), not with the import.
        r = call("copy_document", {{"to", "amp2"}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(folder + "/amp2.dat.ngspice"));
        QVERIFY(!QFileInfo::exists(folder + "/amp2.dat"));
        QVERIFY(!misc::runNetlistOf(folder + "/amp2.dat.ngspice").isEmpty());
        // A2: its run trashed, with the netlist kept for it; the import stays.
        r = call("clean_scratch", {{"datasets", true}});
        QVERIFY2(!failed(r) && text(r).contains("run.dat.ngspice") && !text(r).contains("amp.dat"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/run.dat.ngspice"));
        QVERIFY(QFileInfo::exists(folder + "/amp.dat"));
        QVERIFY(!QFileInfo::exists(kept));
        // Its Data Set the import (Document Settings took it): no run's,
        // neither copied nor trashed; its Data Display copied by its name.
        amp->setDataSet("amp.dat");
        amp->setDataDisplay("view.dpl");
        QFile::remove(folder + "/amp.dpl");   // (the one of its file's name: none)
        write("view.dpl", "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataSet=amp.dat>\n  <DataDisplay=amp.sch>\n</Properties>\n");
        r = call("copy_document", {{"to", "amp3"}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/amp3.dat"));
        QVERIFY(QFileInfo::exists(folder + "/amp3.dpl"));
        r = call("clean_scratch", {{"datasets", true}});
        QVERIFY2(!failed(r) && !text(r).contains("amp.dat"), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(folder + "/amp.dat"));
        amp->setDataSet("run.dat");
        amp->setDataDisplay("amp.dpl");

        // D2: the kept netlists of datasets that are gone (and of the stamp
        // of before, with no path) go when another is kept.
        write("gone.dat.ngspice", run);
        QVERIFY(misc::keepRunNetlist(folder + "/gone.dat.ngspice", "* Qucs " PACKAGE_VERSION "  x.sch\n.end\n"));
        const QString goneKept = misc::runNetlistFile(folder + "/gone.dat.ngspice");
        QVERIFY(QFile::remove(folder + "/gone.dat.ngspice"));
        const QString oldKept = QFileInfo(goneKept).absolutePath() + "/old.dat.ngspice-0123456789ab.cir";
        {
            QFile f(oldKept);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("* dataset 1 2\n.end\n");
        }
        QVERIFY(QFileInfo::exists(goneKept));
        write("again.dat.ngspice", run);
        QVERIFY(misc::keepRunNetlist(folder + "/again.dat.ngspice", "* Qucs " PACKAGE_VERSION "  x.sch\n.end\n"));
        QVERIFY(!QFileInfo::exists(goneKept));
        QVERIFY(!QFileInfo::exists(oldKept));
        QVERIFY(QFileInfo::exists(misc::runNetlistFile(folder + "/again.dat.ngspice")));
        QVERIFY(QFileInfo::exists(misc::runNetlistFile(folder + "/amp2.dat.ngspice")));

        // A5: keep_as, a new name and a copy's are not an import's...
        r = call("simulate", {{"keep_as", "amp"}, {"timeout", 5}});
        QVERIFY2(failed(r) && text(r).contains("imported from"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/amp.dat.ngspice"));
        write("measured.csv", "f,g\n1,2\n2,3\n");
        r = call("import_data", {{"file", "measured.csv"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("dataset").toString() == "measured", qPrintable(text(r)));
        r = call("save_document", {{"as", folder + "/measured.sch"}});
        QVERIFY2(failed(r) && text(r).contains("imported from"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/measured.sch"));
        r = call("copy_document", {{"to", "measured"}});
        QVERIFY2(failed(r) && text(r).contains("imported from"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/measured.sch"));
        // ... and an import's is not a schematic's Data Set.
        r = call("import_data", {{"file", "measured.csv"}, {"name", "run"}});
        QVERIFY2(failed(r) && text(r).contains("Data Set"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/run.dat"));
        QCOMPARE(qucs_s::dataimport::datasetNameFor(folder, folder + "/run.csv"), QStringLiteral("run_2"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The tools (bug hunt 2026-09-30, C1-C9): the trace of a dataset beside
    // the schematic's own names it (ngspice/run1:v, m:gain), and is taken
    // back; a script's call takes max_chars; a document of Qucs-S is no
    // data; a wide table's columns are cut; a default and a prefix are
    // what SPICE reads; a trace of no data is hinted at from the imports'
    // names alone.
    void theToolsTakeBackWhatTheyGive()
    {
        const QString folder = QucsSettings.qucsWorkspaceDir.absoluteFilePath("tools");
        QVERIFY(QDir().mkpath(folder));
        const auto write = [&folder](const QString& name, const QByteArray& bytes) {
            QFile f(folder + "/" + name);
            if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        };
        const QByteArray ac = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep frequency 2>\n1\n2\n</indep>\n<dep ac.v(in) frequency>\n1\n2\n</dep>\n";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const char* port : {"in", "out"})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", port}, {"x", 100}, {"y", port[0] == 'i' ? 100 : 200}})));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/p.sch"}, {"replace", true}})));
        const auto traces = [](const QJsonObject& r) {
            QStringList list;
            for (const QJsonValue& v : json(r).toObject().value("variables").toArray()) list << v.toObject().value("trace").toString();
            return list;
        };

        // C9: a run kept beside it - its traces name it; its own do not.
        write("run1.dat.ngspice", ac);
        write("p.dat.ngspice", ac);
        QJsonObject r = call("get_dataset", {{"path", folder + "/run1.dat.ngspice"}});
        QCOMPARE(traces(r), QStringList{"ngspice/run1:ac.v(in)"});
        r = call("get_dataset", {{"path", folder + "/p.dat.ngspice"}});
        QCOMPARE(traces(r), QStringList{"ngspice/ac.v(in)"});
        // (A schematic's own when it is not open too: its Data Set says so.)
        write("q.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataSet=qrun.dat>\n</Properties>\n");
        write("qrun.dat.ngspice", ac);
        r = call("get_dataset", {{"path", folder + "/qrun.dat.ngspice"}});
        QCOMPARE(traces(r), QStringList{"ngspice/ac.v(in)"});
        // Taken back: of the file, and of the schematic (to that dataset).
        r = call("get_dataset", {{"path", folder + "/run1.dat.ngspice"}, {"variables", QJsonArray{"ngspice/run1:ac.v(in)"}}});
        QVERIFY2(!failed(r) && json(r).toObject().value("variables").toArray().at(0).toObject().value("name") == "ac.v(in)", qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"ngspice/run1:ac.v(in)"}}});
        QVERIFY2(!failed(r) && json(r).toObject().value("dataset").toString().endsWith("run1.dat.ngspice"), qPrintable(text(r)));
        // C3: an imported one.
        write("m.csv", "f,gain\n1,2\n2,3\n");
        r = call("import_data", {{"file", "m.csv"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("dataset").toString() == "m", qPrintable(text(r)));
        r = call("get_dataset", {{"path", folder + "/m.dat"}});
        QCOMPARE(traces(r), QStringList{"m:gain"});
        r = call("get_dataset", {{"path", folder + "/m.dat"}, {"variables", QJsonArray{"m:gain"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"m:gain"}}});
        QVERIFY2(!failed(r) && json(r).toObject().value("dataset").toString().endsWith("m.dat"), qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"m:gain", "run1:ac.v(in)"}}});
        QVERIFY2(failed(r) && text(r).contains("of 2 datasets"), qPrintable(text(r)));
        r = call("get_dataset", {{"variables", QJsonArray{"nothere:x"}}});
        QVERIFY2(failed(r) && text(r).contains("There is no dataset nothere"), qPrintable(text(r)));
        // C4: a schematic is no data.
        r = call("import_data", {{"file", folder + "/p.sch"}});
        QVERIFY2(failed(r) && text(r).contains("p.sch is a schematic of Qucs-S, not data"), qPrintable(text(r)));
        // C8: a wide table's columns cut, as its variables are.
        QByteArray wide;
        for (int c = 0; c < 150; ++c) wide += (c ? "," : "") + QByteArray("c") + QByteArray::number(c);
        wide += '\n';
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 150; ++c) wide += (c ? "," : "") + QByteArray::number(row * c);
            wide += '\n';
        }
        write("wide.csv", wide);
        r = call("import_data", {{"file", "wide.csv"}});
        QVERIFY2(!failed(r), qPrintable(text(r).left(300)));
        QCOMPARE(json(r).toObject().value("columns").toArray().size(), 100);
        QVERIFY2(json(r).toObject().value("columns left out").toString().contains("50 more columns (150 in all)"), qPrintable(text(r).right(400)));

        // C1: a trace of no data is hinted at from the imports' names - not
        // each read in full for every trace. (A dataset of 8 MB: 8 traces
        // read it 8 times, 64 MB, before.)
        QByteArray big = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep x 400000>\n";
        for (int i = 0; i < 400000; ++i) big += QByteArray::number(i) + '\n';
        big += "</indep>\n<dep y x>\n";
        for (int i = 0; i < 400000; ++i) big += QByteArray::number(i * 0.25) + '\n';
        big += "</dep>\n";
        write("big.dat.source", big);
        qucs_s::dataimport::Imported imported;
        QString error;
        QVERIFY2(qucs_s::dataimport::importFile(folder, folder + "/big.dat.source", {}, &imported, &error, nullptr, "bigimport"), qPrintable(error));
        r = call("add_diagram", {{"traces", QJsonArray{"gain"}}});
        QVERIFY2(!failed(r) && text(r).contains("the trace m:gain shows it"), qPrintable(text(r)));
        QJsonArray none;
        for (int i = 0; i < 8; ++i) none.append(QStringLiteral("q%1").arg(i));
        QElapsedTimer clock;
        clock.start();
        r = call("add_diagram", {{"traces", none}});
        const qint64 first = clock.restart();
        r = call("get_schematic");
        const qint64 then = clock.elapsed();
        qInfo() << "8 traces of no data:" << first << "ms; a listing after:" << then << "ms";
        QVERIFY2(!failed(r), qPrintable(text(r).left(200)));
        QVERIFY2(first < 300 && then < 300, qPrintable(QStringLiteral("%1 ms, %2 ms").arg(first).arg(then)));

        // C2: a script's call takes max_chars, as a batch's does.
        if (control->scriptingBuilt()) {
            r = call("run_script", {{"script", "const a = qucs.call('get_schematic', {max_chars: 300});\nreturn JSON.stringify(a).length"}});
            QVERIFY2(!failed(r) && json(r).toObject().value("result").toInt() < 600, qPrintable(text(r)));
            r = call("run_script", {{"script", "return qucs.call('get_schematic', {max_chars: 'many'})"}});
            QVERIFY2(failed(r) && text(r).contains("max_chars is a whole number"), qPrintable(text(r)));
        }

        // C5: a default SPICE reads; C6: a prefix SPICE reads as a name.
        for (const char* bad : {"Rs=-", "Rs=1k;", "Rs=1,5", "Rs={2*k", "Rs=4k7%"}) {
            r = call("set_subcircuit_parameters", {{"parameters", QJsonArray{bad}}});
            QVERIFY2(failed(r) && text(r).contains("no value SPICE reads"), qPrintable(QString(bad) + ": " + text(r)));
        }
        r = call("set_subcircuit_parameters", {{"parameters", QJsonArray{"Rs=4.7n", "k={2*Rs}", "m='Rs/2'", "t=temp", "z=-1e-3", "w=10kOhm"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        for (const char* bad : {"X;Y", "1X", "\xC3\xA9", "A B", ""}) {
            r = call("make_symbol", {{"prefix", QString::fromUtf8(bad)}});
            QVERIFY2(failed(r) && text(r).contains("'prefix' is a word SPICE reads as a name"), qPrintable(QString::fromUtf8(bad) + ": " + text(r)));
        }
        r = call("make_symbol", {{"prefix", "XA_1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A run's commands besides the simulator (bug hunt 2026-09-30, E1): with
    // the Simulator Settings' check of commands on, a schematic that
    // carries one is not simulated, nor tuned, unless asked
    // ('allow_commands') - and check_schematic says what they are. Off (the
    // default): neither. (It has no analysis: a run stops there, and
    // nothing runs.)
    void commandsAreNotRunUnasked()
    {
        struct Restore {
            bool was = QucsSettings.CheckCommands;
            ~Restore() { QucsSettings.CheckCommands = was; }
        } restore;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "CMD"}, {"name", "CMD1"}, {"x", 100}, {"y", 100}, {"properties", QJsonObject{{"cmd", "echo qucs-test"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/commands.sch")}, {"replace", true}})));
        const QString said = "CMD1 runs a command in a shell after each simulation: echo qucs-test";
        // Off: not looked for - neither refused nor said.
        QVERIFY(!QucsSettings.CheckCommands);
        r = call("simulate", {{"timeout", 5}});
        QVERIFY2(failed(r) && text(r).contains("has no analysis to run"), qPrintable(text(r)));
        r = call("check_schematic");
        QVERIFY2(!text(r).contains(said), qPrintable(text(r)));
        // On.
        QucsSettings.CheckCommands = true;
        r = call("simulate", {{"timeout", 5}});
        QVERIFY2(failed(r) && text(r).contains("runs commands besides the simulator") && text(r).contains(said) && text(r).contains("Nothing was run"),
                 qPrintable(text(r)));
        r = call("tune", {{"component", "R1"}, {"target", 1}, {"range", QJsonArray{"1k", "2k"}}, {"measure", QJsonObject{{"variable", "v(x)"}}}});
        QVERIFY2(failed(r) && text(r).contains("runs commands besides the simulator"), qPrintable(text(r)));
        r = call("tune", {{"knobs", QJsonArray{}}, {"targets", QJsonArray{}}});
        QVERIFY2(failed(r) && text(r).contains("runs commands besides the simulator"), qPrintable(text(r)));
        // Asked for: past it (to what comes next - no analysis here).
        r = call("simulate", {{"timeout", 5}, {"allow_commands", true}});
        QVERIFY2(failed(r) && text(r).contains("has no analysis to run"), qPrintable(text(r)));
        r = call("check_schematic");
        QVERIFY2(text(r).contains(said), qPrintable(text(r)));
        // Turned off: nothing to say, nothing refused.
        QVERIFY(!failed(call("edit_component", {{"name", "CMD1"}, {"active", false}})));
        r = call("simulate", {{"timeout", 5}});
        QVERIFY2(failed(r) && text(r).contains("has no analysis to run"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A diagram's curves written for another program (export_data), as the
    // Export tab writes them: its traces, or some of them; a dataset's
    // variables - names, trace names, expressions - in each form; the
    // suffix gives the format; one dataset to a file; never the dataset
    // read or the file an import came from; a file written over put back.
    void curvesAreExported()
    {
        const QString folder = QucsSettings.qucsWorkspaceDir.absoluteFilePath("exports");
        QVERIFY(QDir().mkpath(folder));
        const auto write = [&folder](const QString& name, const QByteArray& bytes) {
            QFile f(folder + "/" + name);
            if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        };
        const auto read = [&folder](const QString& name) {
            QFile f(folder + "/" + name);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        const QByteArray run = "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 3>\n0\n1e-3\n2e-3\n</indep>\n<dep tran.v(out) time>\n0\n0.5\n1\n</dep>\n"
                               "<dep tran.v(in) time>\n1\n1\n1\n</dep>\n<indep frequency 2>\n1000\n1e6\n</indep>\n"
                               "<dep ac.v(out) frequency>\n1+j1\n0-j0.5\n</dep>\n<dep ac.v(in) frequency>\n1\n1\n</dep>\n";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/e.sch"}, {"replace", true}})));
        write("e.dat.ngspice", run);
        QJsonObject r = call("add_diagram", {{"traces", QJsonArray{"ngspice/tran.v(out)", "ngspice/tran.v(in)"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));

        // Its traces, all: CSV unless said, a table over time.
        r = call("export_data", {{"diagram", 1}, {"save_as", folder + "/curves"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QVERIFY(o.value("file").toString().endsWith("curves.csv") && o.value("format").toString() == "CSV");
        QCOMPARE(o.value("table").toObject().value("rows").toInt(), 3);
        QCOMPARE(read("curves.csv"), QByteArray("time,tran.v(out),tran.v(in)\n0,0,1\n0.001,0.5,1\n0.002,1,1\n"));
        // Some of them; the suffix gives the format.
        r = call("export_data", {{"diagram", 1}, {"traces", QJsonArray{2}}, {"save_as", folder + "/in.xlsx"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("format").toString() == "Excel workbook", qPrintable(text(r)));
        qucs_s::dataimport::Data back;
        QString error;
        QVERIFY2(qucs_s::dataimport::read(folder + "/in.xlsx", {}, &back, &error), qPrintable(error));
        QCOMPARE(back.variables.size(), 2);
        QCOMPARE(back.variables.at(1).name, QStringLiteral("tran.v(in)"));
        r = call("export_data", {{"diagram", 1}, {"save_as", folder + "/x.xlsx"}, {"format", "csv"}});
        QVERIFY2(failed(r) && text(r).contains("the one or the other"), qPrintable(text(r)));
        QVERIFY(failed(call("export_data", {{"diagram", 1}, {"variables", QJsonArray{"tran.v(in)"}}, {"save_as", "y.csv"}})));
        QVERIFY(failed(call("export_data", {{"save_as", "y.csv"}})));

        // A dataset's variables: a complex one in dB and phase, an
        // expression, one that may be two.
        r = call("export_data", {{"variables", QJsonArray{"ac.v(out)"}}, {"complex", "db_phase"}, {"save_as", folder + "/ac.csv"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(read("ac.csv").startsWith("frequency,dB(ac.v(out)),phase(ac.v(out))\n1000,3.0102999566398"), read("ac.csv").constData());
        r = call("export_data", {{"variables", QJsonArray{"db(ac.v(out)/ac.v(in))"}}, {"save_as", folder + "/gain.tsv"}});
        QVERIFY2(!failed(r) && read("gain.tsv").startsWith("frequency\tdb(ac.v(out)/ac.v(in))\n1000\t3.0102999566398"), qPrintable(text(r)));
        r = call("export_data", {{"variables", QJsonArray{"v(out)"}}, {"save_as", folder + "/v.csv"}});
        QVERIFY2(failed(r) && text(r).contains("may be any of"), qPrintable(text(r)));
        r = call("export_data", {{"variables", QJsonArray{"nothere"}}, {"save_as", folder + "/v.csv"}});
        QVERIFY2(failed(r) && text(r).contains("has no variable nothere"), qPrintable(text(r)));
        // A kept run: by its trace name, or by its name.
        write("run1.dat.ngspice", run);
        r = call("export_data", {{"variables", QJsonArray{"ngspice/run1:tran.v(out)"}}, {"save_as", folder + "/run1.csv"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("dataset").toString().endsWith("run1.dat.ngspice"), qPrintable(text(r)));
        r = call("export_data", {{"dataset", "run1"}, {"variables", QJsonArray{"tran.v(out)"}}, {"save_as", folder + "/run1b"}, {"format", "npz"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("arrays").toArray() == (QJsonArray{"time", "tran.v(out)"})
                     && json(r).toObject().value("dataset").toString().endsWith("run1.dat.ngspice"),
                 qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(folder + "/run1b.npz"));
        r = call("export_data", {{"dataset", "nosuch"}, {"variables", QJsonArray{"v"}}, {"save_as", "z.csv"}});
        QVERIFY2(failed(r) && text(r).contains("There is no dataset nosuch"), qPrintable(text(r)));

        // A run's and a measurement's traces in one diagram: one dataset to
        // a file.
        write("m.csv", "f,gain\n1,2\n2,3\n");
        QVERIFY(!failed(call("import_data", {{"file", "m.csv"}})));
        r = call("add_trace", {{"diagram", 1}, {"variable", "m:gain"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("export_data", {{"diagram", 1}, {"save_as", folder + "/mixed.csv"}});
        QVERIFY2(failed(r) && text(r).contains("of 2 datasets") && text(r).contains("m.dat: traces 3 (m:gain)"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/mixed.csv"));
        r = call("export_data", {{"diagram", 1}, {"traces", QJsonArray{"m:gain"}}, {"save_as", folder + "/measured.csv"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(read("measured.csv"), QByteArray("f,gain\n1,2\n2,3\n"));
        // Never the dataset read, nor the file an import came from.
        r = call("export_data", {{"diagram", 1}, {"traces", QJsonArray{3}}, {"save_as", folder + "/m.dat"}});
        QVERIFY2(failed(r) && text(r).contains("is the dataset itself"), qPrintable(text(r)));
        r = call("export_data", {{"diagram", 1}, {"traces", QJsonArray{3}}, {"save_as", folder + "/m.csv"}});
        QVERIFY2(failed(r) && text(r).contains("is the file the dataset m was imported from"), qPrintable(text(r)));
        QCOMPARE(read("m.csv"), QByteArray("f,gain\n1,2\n2,3\n"));
        // A trace of no data: said why.
        QVERIFY(!failed(call("add_trace", {{"diagram", 1}, {"variable", "ngspice/run9:tran.v(out)"}})));
        r = call("export_data", {{"diagram", 1}, {"traces", QJsonArray{4}}, {"save_as", folder + "/none.csv"}});
        QVERIFY2(failed(r) && text(r).contains("shows no data") && text(r).contains("run9.dat.ngspice"), qPrintable(text(r)));

        // Written over: said, and put back by undo with 'files'.
        r = call("export_data", {{"diagram", 1}, {"traces", QJsonArray{1}}, {"save_as", folder + "/curves.csv"}});
        QVERIFY2(!failed(r) && json(r).toObject().contains("written over"), qPrintable(text(r)));
        QCOMPARE(read("curves.csv"), QByteArray("time,tran.v(out)\n0,0\n0.001,0.5\n0.002,1\n"));
        QVERIFY(control->irreversible("export_data", {{"save_as", folder + "/curves.csv"}}));
        QVERIFY(!control->irreversible("export_data", {{"save_as", folder + "/fresh.csv"}}));
        r = call("undo", {{"files", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(read("curves.csv"), QByteArray("time,tran.v(out),tran.v(in)\n0,0,1\n0.001,0.5,1\n0.002,1,1\n"));
        // A dataset beside it: to plot.
        r = call("export_data", {{"diagram", 1}, {"traces", QJsonArray{1}}, {"save_as", folder + "/copy"}, {"format", "dataset"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("then").toString().contains("copy:variable"), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(folder + "/copy.dat"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // ---- the assessment of 2026-10-01 (qucs-s-mcp-api-assessment-2026-10-01)

    // 2: a trace shows a part of each value - dB, phase, magnitude, real,
    // imaginary - as it is read, so its markers and a table show it too, and
    // its name says it; kept in the file as a field of its own. Refused
    // where the complex value itself is drawn. An axis's units on a linear
    // axis, and several traces with no legend, said.
    void aTraceShowsAPartOfItsValues()
    {
        const QString folder = QucsSettings.qucsWorkspaceDir.absoluteFilePath("parts");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/p.sch"}, {"replace", true}})));
        {
            QFile f(folder + "/p.dat.ngspice");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Dataset " PACKAGE_VERSION ">\n<indep frequency 3>\n1\n2\n3\n</indep>\n<dep ac.v(out) frequency>\n3+j4\n0+j1\n-1+j0\n</dep>\n");
        }
        QJsonObject r = call("add_diagram", {{"traces", QJsonArray{QJsonObject{{"variable", "ac.v(out)"}, {"part", "db"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("traces").toArray().at(0).toObject().value("part").toString(), QStringLiteral("db"));
        Diagram* d = front()->a_DocDiags.back();
        Graph* g = d->Graphs.first();
        const auto values = [&g](int n) {
            QList<double> v;
            for (int i = 0; i < n; ++i) {
                v << std::round(g->cPointsY[2 * i] * 1e3) / 1e3;
                if (g->cPointsY[2 * i + 1] != 0 && g->valuePart != Graph::ValuePart::Auto) v << 999;   // (no imaginary part left)
            }
            return v;
        };
        QCOMPARE(values(3), (QList<double>{13.979, 0, 0}));
        QCOMPARE(g->withValuePart(QStringLiteral("ac.v(out)")), QStringLiteral("dB(ac.v(out))"));
        const QList<std::pair<const char*, QList<double>>> parts{{"phase", {53.13, 90, 180}}, {"real", {3, 0, -1}},
                                                                 {"imaginary", {4, 1, 0}}, {"magnitude", {5, 1, 1}}};
        for (const auto& [part, expected] : parts) {
            r = call("edit_trace", {{"diagram", 1}, {"trace", 1}, {"part", part}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QCOMPARE(values(3), expected);
        }
        r = call("edit_trace", {{"diagram", 1}, {"trace", 1}, {"part", "phase"}});
        // A marker says it: its value, under its name.
        r = call("add_marker", {{"diagram", 1}, {"trace", 1}, {"at", 2}});
        QVERIFY2(!failed(r) && json(r).toObject().value("text").toString().contains("phase(ac.v(out)): 90"), qPrintable(text(r)));
        // Kept in the file: the tenth field of the trace's line; read back so.
        QVERIFY(!failed(call("save_document")));
        QFile file(folder + "/p.sch");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString saved = QString::fromUtf8(file.readAll());
        QVERIFY2(saved.contains("<\"ngspice/ac.v(out)\" #0000ff 1 3 0 0 0 0 0 3>"), qPrintable(saved.section("<Diagrams>", 1)));
        QVERIFY(!failed(call("close_document")));
        QVERIFY(!failed(call("open_document", {{"path", folder + "/p.sch"}})));
        QCOMPARE(front()->a_DocDiags.back()->Graphs.first()->valuePart, Graph::ValuePart::Phase);
        QCOMPARE(json(call("get_schematic")).toObject().value("diagrams").toArray().at(0).toObject().value("traces").toArray().at(0)
                     .toObject().value("part").toString(),
                 QStringLiteral("phase"));
        // Where the complex value is drawn: no part.
        r = call("add_diagram", {{"type", "smith"}, {"traces", QJsonArray{QJsonObject{{"variable", "ac.v(out)"}, {"part", "db"}}}}});
        QVERIFY2(failed(r) && text(r).contains("plots the complex value itself"), qPrintable(text(r)));
        QVERIFY(failed(call("edit_trace", {{"diagram", 1}, {"trace", 1}, {"part", "decibels"}})));
        // Units on a linear axis: said, and what does it.
        r = call("add_diagram", {{"traces", QJsonArray{"ac.v(out)"}}, {"y_axis", QJsonObject{{"units", "dB"}}}});
        QVERIFY2(!failed(r) && text(r).contains("y_axis's units dB label a logarithmic axis's numbers"), qPrintable(text(r)));
        r = call("add_diagram", {{"traces", QJsonArray{QJsonObject{{"variable", "ac.v(out)"}, {"part", "db"}}}}, {"y_axis", QJsonObject{{"units", "dB"}}}});
        QVERIFY2(!failed(r) && text(r).contains("its traces are in dB already"), qPrintable(text(r)));
        r = call("add_diagram", {{"traces", QJsonArray{"ac.v(out)"}}, {"y_axis", QJsonObject{{"units", "dB"}, {"log", true}}}});
        QVERIFY2(!failed(r) && !text(r).contains("units dB"), qPrintable(text(r)));
        // Two traces, no legend: said; with one, not.
        r = call("add_diagram", {{"traces", QJsonArray{"ac.v(out)", QJsonObject{{"variable", "ac.v(out)"}, {"part", "phase"}}}}});
        QVERIFY2(!failed(r) && text(r).contains("No legend"), qPrintable(text(r)));
        r = call("add_diagram", {{"traces", QJsonArray{"ac.v(out)", QJsonObject{{"variable", "ac.v(out)"}, {"part", "phase"}}}}, {"legend", "top_right"}});
        QVERIFY2(!failed(r) && !text(r).contains("No legend"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // 3: a property a block has any number of - .NGOPT's Knob and Target -
    // given as a list (the schema took only a text, and refused a list the
    // tool reads); described with its fields.
    void aRepeatedPropertyTakesAList()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", ".NGOPT"}, {"name", "NgOpt1"}, {"x", 100}, {"y", 100},
                                               {"properties", QJsonObject{{"Method", "lm"}, {"Knob", QJsonArray{"dparam|R|500|100|10k"}},
                                                                          {"Target", QJsonArray{"AC1|db(v(out)[0])|-3.0103|1", "AC1|db(v(out)[1])|-6|1"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const auto props = [this] {
            for (const QJsonValue& c : json(call("get_schematic", {{"format", "json"}})).toObject().value("components").toArray())
                if (c.toObject().value("name") == "NgOpt1") return c.toObject().value("properties").toObject();
            return QJsonObject();
        };
        QCOMPARE(props().value("Knob").toArray(), QJsonArray{"dparam|R|500|100|10k"});
        QCOMPARE(props().value("Target").toArray().size(), 2);
        r = call("edit_component", {{"name", "NgOpt1"}, {"properties", QJsonObject{{"Knob", QJsonArray{"dparam|R|600|100|10k", "dparam|C|1n|100p|10n"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(props().value("Knob").toArray(), (QJsonArray{"dparam|R|600|100|10k", "dparam|C|1n|100p|10n"}));
        QCOMPARE(props().value("Target").toArray().size(), 2);
        r = call("edit_component", {{"name", "NgOpt1"}, {"properties", QJsonObject{{"Knob", QJsonArray{1}}}}});
        QVERIFY2(failed(r) && text(r).contains("a text"), qPrintable(text(r)));
        // Described: each with its fields, an example and how to give it.
        const QJsonArray repeated = json(call("describe_component_type", {{"type", ".NGOPT"}})).toObject().value("repeated properties").toArray();
        QCOMPARE(repeated.size(), 2);
        QCOMPARE(repeated.at(0).toObject().value("name").toString(), QStringLiteral("Knob"));
        QVERIFY(repeated.at(0).toObject().value("fields").toString().startsWith("kind|name|initial|low|high"));
        QCOMPARE(repeated.at(1).toObject().value("example").toString(), QStringLiteral("SP1|db(S_2_1[20])|-0.0771|1"));
        QVERIFY(json(call("describe_component_type", {{"type", "R"}})).toObject().value("repeated properties").isUndefined());
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // 4: what an ngspice optimize found comes back in simulate's answer;
    // with apply_optimum it goes into the parameters (one undo step) and
    // the dataset is not read as stale. The knobs Qucs-S writes after the
    // run are the simulation's own edit, not "changed while it ran".
    void anOptimumIsReportedAndApplied()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        {
            QTemporaryDir probe;
            QFile deck(probe.filePath("probe.cir"));
            QVERIFY(deck.open(QIODevice::WriteOnly));
            deck.write("* probe\nR1 1 0 1\nV1 1 0 1\n.control\nhelp optimize\n.endc\n.end\n");
            deck.close();
            QProcess p;
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(ngspice, {"-b", deck.fileName()});
            QVERIFY(p.waitForFinished(20000));
            if (!QString::fromUtf8(p.readAll()).contains("parameter optimizer")) QSKIP("no ngspice with the optimize command");
        }
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        const QString file = dir.filePath("workspace/rc_fit.sch");
        QJsonObject r = call("import_netlist", {{"text", "rc\nV1 in 0 DC 0 AC 1\nR1 in out {R}\nC1 out 0 1n\n.param R=500\n.ac lin 1 159.155k 159.155k\n.end"},
                                                {"save_as", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("add_component", {{"type", ".NGOPT"}, {"name", "NgOpt1"}, {"x", 400}, {"y", 500},
                                               {"properties", QJsonObject{{"Method", "lm"}, {"Knob", QJsonArray{"dparam|R|500|100|10k"}},
                                                                          {"Target", QJsonArray{"AC1|db(v(out)[0])|-3.0103|1"}}}}})));
        const auto parameter = [this] {
            for (const QJsonValue& c : json(call("get_schematic", {{"format", "json"}})).toObject().value("components").toArray())
                if (c.toObject().value("type") == "SpicePar") return c.toObject().value("equations").toArray().at(0).toString();
            return QString();
        };
        QCOMPARE(parameter(), QStringLiteral("R=500"));
        r = call("simulate", {{"timeout", 60}}, 90000);
        QJsonObject o = json(r).toObject();
        QVERIFY2(o.value("succeeded").toBool(), qPrintable(text(r)));
        QJsonObject optimum = o.value("optimum").toArray().at(0).toObject();
        QCOMPARE(optimum.value("block").toString(), QStringLiteral("NgOpt1"));
        QVERIFY2(optimum.value("summary").toString().startsWith("converged"), qPrintable(text(r)));
        QVERIFY2(std::abs(qucs_s::units::read(optimum.value("found").toObject().value("R").toString()).value - 1000) < 1, qPrintable(text(r)));
        QVERIFY2(optimum.value("applied").toString().startsWith("no:"), qPrintable(text(r)));
        QVERIFY2(!o.contains("changed while it ran"), qPrintable(text(r)));
        QCOMPARE(parameter(), QStringLiteral("R=500"));
        // Applied: into SpicePar1's R, one undo step; the dataset not stale.
        r = call("simulate", {{"timeout", 60}, {"apply_optimum", true}}, 90000);
        optimum = json(r).toObject().value("optimum").toArray().at(0).toObject();
        QCOMPARE(optimum.value("applied to").toObject().value("R").toString(), QStringLiteral("SpicePar1.R"));
        QCOMPARE(parameter(), QStringLiteral("R=1k"));
        r = call("get_dataset", {{"variables", QJsonArray{"ac.v(out)"}}, {"points", 1}});
        QVERIFY2(!failed(r) && !json(r).toObject().contains("stale"), qPrintable(text(r).left(600)));
        QVERIFY(!failed(call("undo")));
        QCOMPARE(parameter(), QStringLiteral("R=500"));
        QucsSettings.NgspiceExecutable = before;
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // 5: a pin's place on a part turned or mirrored, by rotation and
    // mirror or by the rule given - as a part placed so has it; connect
    // says when a pin faces away from the other end, and a body it crosses.
    void pinsAreGivenTurnedAndConnectSaysWhy()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        // (An op-amp: its pins off its centre line both ways, so a mirror
        // moves them.)
        const QJsonObject plain = json(call("describe_component_type", {{"type", "OpAmp"}})).toObject();
        QVERIFY(plain.value("turned").toString().contains("rotation 1 puts it at (y, -x)"));
        const QJsonArray at0 = plain.value("pins").toArray();
        QCOMPARE(at0.size(), 3);
        int n = 0;
        for (const bool mirror : {false, true})
            for (int rotation = 0; rotation < 4; ++rotation) {
                const QString name = QStringLiteral("OP%1").arg(++n);
                QVERIFY(!failed(call("add_component", {{"type", "OpAmp"}, {"name", name}, {"x", 200 * n}, {"y", 300}, {"rotation", rotation}, {"mirror", mirror}})));
                const QJsonArray placed = json(call("get_schematic", {{"components", QJsonArray{name}}})).toObject().value("components").toArray()
                                              .at(0).toObject().value("pins").toArray();
                const QJsonArray described = json(call("describe_component_type", {{"type", "OpAmp"}, {"rotation", rotation}, {"mirror", mirror}}))
                                                 .toObject().value("pins").toArray();
                for (int i = 0; i < 3; ++i) {
                    // As described; and as the rule says: mirror (x, -y), then (y, -x) each turn.
                    const int px = placed.at(i).toObject().value("x").toInt() - 200 * n, py = placed.at(i).toObject().value("y").toInt() - 300;
                    QCOMPARE(described.at(i).toObject().value("x").toInt(), px);
                    QCOMPARE(described.at(i).toObject().value("y").toInt(), py);
                    int x = at0.at(i).toObject().value("x").toInt(), y = at0.at(i).toObject().value("y").toInt();
                    if (mirror) y = -y;
                    for (int k = 0; k < rotation; ++k) std::tie(x, y) = std::make_pair(y, -x);
                    QVERIFY2(x == px && y == py, qPrintable(QStringLiteral("%1 rotation %2 mirror %3 pin %4").arg(name).arg(rotation).arg(mirror).arg(i + 1)));
                }
            }
        QVERIFY(failed(call("describe_component_type", {{"type", "OpAmp"}, {"rotation", 4}})));
        // The reviewer's case: C at rotation 1 (pin 1 at its bottom) wired to L above.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 220}, {"y", 230}, {"rotation", 1}})));
        QVERIFY(!failed(call("add_component", {{"type", "L"}, {"name", "L1"}, {"x", 340}, {"y", 160}})));
        QJsonObject r = call("connect", {{"from", "C1.1"}, {"to", "L1.1"}});
        QVERIFY2(!failed(r) && text(r).contains("C1's pin 1 faces down, away from L1.1: the wire goes round C1. At rotation 3 (now 1) it faces L1.1."),
                 qPrintable(text(r)));
        // Pins facing each other: nothing to say of them.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 220}, {"y", 100}, {"rotation", 1}})));
        r = call("connect", {{"from", "C1.2"}, {"to", "R2.1"}});
        QVERIFY2(!failed(r) && !text(r).contains("faces"), qPrintable(text(r)));
        // A wire through a part's body, by the way given: said.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R9"}, {"x", 600}, {"y", 300}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R7"}, {"x", 600}, {"y", 200}, {"rotation", 1}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R8"}, {"x", 600}, {"y", 400}, {"rotation", 1}})));
        r = call("connect", {{"from", "R7.1"}, {"to", "R8.2"}, {"via", QJsonArray{QJsonArray{600, 300}}}});
        QVERIFY2(text(r).contains("It crosses the body of R9"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // 6: the smaller notes - a bracketed name in an expression (db(S[2,1]));
    // a second import of a file of that name told why it is name_2;
    // edit_trace that changes nothing says so; a preview's wires by their
    // ends.
    void theSmallerNotesOfTheAssessment()
    {
        const QString folder = QucsSettings.qucsWorkspaceDir.absoluteFilePath("assess6");
        QVERIFY(QDir().mkpath(folder + "/other"));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 220}, {"y", 230}, {"rotation", 1}})));
        QVERIFY(!failed(call("add_component", {{"type", "L"}, {"name", "L1"}, {"x", 340}, {"y", 160}})));
        QVERIFY(!failed(call("connect", {{"from", "C1.1"}, {"to", "L1.1"}})));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/t.sch"}, {"replace", true}})));
        {
            QFile f(folder + "/target.dat");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Dataset " PACKAGE_VERSION ">\n<indep frequency 2>\n1e6\n2e6\n</indep>\n<dep S[2,1] frequency>\n0.6+j0.8\n0+j0.5\n</dep>\n");
        }
        QJsonObject r = call("get_dataset", {{"path", folder + "/target.dat"}, {"variables", QJsonArray{"db(S[2,1])", "phase(S[2,1])+1"}}, {"at", QJsonArray{1e6}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray vars = json(r).toObject().value("variables").toArray();
        QVERIFY2(std::abs(vars.at(0).toObject().value("at").toArray().at(0).toArray().at(1).toDouble()) < 1e-9, qPrintable(text(r)));
        QVERIFY2(std::abs(vars.at(1).toObject().value("at").toArray().at(0).toArray().at(1).toDouble() - 54.1301) < 1e-3, qPrintable(text(r)));
        // A second file of the name: named, and why.
        for (const QString& where : {folder, folder + "/other"}) {
            QFile f(where + "/meas.csv");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("t,v\n0,1\n1,2\n");
        }
        QVERIFY(!failed(call("import_data", {{"file", folder + "/meas.csv"}})));
        r = call("import_data", {{"file", folder + "/other/meas.csv"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("named").toString()
                                   == "meas is the dataset imported from meas.csv: this one is meas_2 ('name': \"meas\" puts it in that one's place).",
                 qPrintable(text(r)));
        r = call("import_data", {{"file", folder + "/meas.csv"}, {"reload", true}});
        QVERIFY2(!failed(r) && !json(r).toObject().contains("named"), qPrintable(text(r)));
        // edit_trace changing nothing: said so, and why.
        QVERIFY(!failed(call("add_diagram", {{"traces", QJsonArray{"meas:v"}}})));
        r = call("edit_trace", {{"diagram", 1}, {"trace", 1}, {"variable", "meas:v"}, {"thickness", 0}});
        r = call("edit_trace", {{"diagram", 1}, {"trace", 1}, {"variable", "meas:v"}, {"thickness", 0}});
        QVERIFY2(json(r).toObject().value("changed") == false && text(r).contains("Nothing changed: trace 1 is so already."), qPrintable(text(r)));
        r = call("edit_trace", {{"diagram", 1}, {"trace", 1}, {"thickness", 2}});
        QVERIFY2(!json(r).toObject().contains("changed"), qPrintable(text(r)));
        // A turn's wires by their ends, in the preview.
        r = call("edit_component", {{"name", "C1"}, {"rotation", 3}, {"preview", true}});
        const QString changes = QJsonDocument(json(r).toObject().value("would change").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(changes.contains("2 wires drawn (") && changes.contains("2 wires taken away (220,260-310,260; 310,160-310,260)"), qPrintable(changes));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // ---- what Claude can and cannot reach (qucs-s-claude-access, 1 October)

    // The file dialogs Claude opens - a menu action's, a dialog's button's -
    // are Qt's, which get_dialog reads and set_dialog fills in; the
    // system's (macOS's panel) could not be, and waited for the user. Those
    // the user opens stay the system's. File > Open and Save As are refused
    // (open_document and save_document do them), and printing.
    void fileDialogsClaudeOpensAreQts()
    {
        // (A modal dialog runs a loop of its own: what answers it is set
        // going before the call that opens it, and runs in that loop.)
        QVERIFY(!QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));
        const QString example = QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch");
        bool fileDialog = false, qts = false;
        QJsonObject answered;
        QTimer::singleShot(800, this, [&] {
            fileDialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()) != nullptr;
            qts = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);   // while it is open
            answered = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", "File name"}, {"value", example}}}}, {"press", "Open"}});
        });
        QJsonObject r = call("trigger_action", {{"action", "File > Examples"}}, 20000);
        QVERIFY2(!failed(r) && text(r).contains("waits for an answer"), qPrintable(text(r)));
        QTRY_VERIFY_WITH_TIMEOUT(!answered.isEmpty(), 10000);
        QVERIFY(fileDialog);
        QVERIFY(qts);
        QVERIFY2(!failed(answered) && text(answered).contains("no dialog is open now"), qPrintable(text(answered)));
        QTRY_VERIFY(!QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));   // as it was after it
        QVERIFY(app->findDoc(example) != nullptr);
        QVERIFY(!failed(call("close_document", {{"path", example}, {"unsaved", "discard"}})));

        // A dialog's button that opens one (Export as Image's Browse).
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        // (The export dialog is deleted once closed: looked at only while
        // it is open.)
        bool exportDialog = false, browseIsQts = false, stillQts = false, backToExport = false;
        QJsonObject browsed, cancelled, closed;
        QTimer::singleShot(800, this, [&] {
            const QPointer<QWidget> exporting = QApplication::activeModalWidget();
            exportDialog = exporting != nullptr && qobject_cast<QFileDialog*>(exporting.data()) == nullptr;
            QTimer::singleShot(800, this, [&] {
                browseIsQts = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()) != nullptr
                              && QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
                cancelled = call("set_dialog", {{"press", "Cancel"}});
            });
            browsed = call("set_dialog", {{"press", "Browse..."}});
            backToExport = exporting != nullptr && QApplication::activeModalWidget() == exporting.data();
            stillQts = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);   // (the action's dialog is still open)
            closed = call("set_dialog", {{"press", "Cancel"}});
        });
        r = call("trigger_action", {{"action", "File > Export as image..."}}, 30000);
        QVERIFY2(!failed(r) && text(r).contains("waits for an answer"), qPrintable(text(r)));
        QTRY_VERIFY_WITH_TIMEOUT(!closed.isEmpty(), 15000);
        QVERIFY(exportDialog);
        QVERIFY2(!failed(browsed) && text(browsed).contains("is open now"), qPrintable(text(browsed)));
        QVERIFY(browseIsQts);
        QVERIFY2(!failed(cancelled), qPrintable(text(cancelled)));
        QVERIFY(backToExport);
        QVERIFY(stillQts);
        QVERIFY2(!failed(closed) && text(closed).contains("no dialog is open now"), qPrintable(text(closed)));
        QTRY_VERIFY(!QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // A dialog the user opened, whose button Claude presses: the file
        // dialog that opens is Qt's too, and the user's setting is back
        // after it, the user's dialog still open.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        bool theirsBrowseIsQts = false, theirsAfter = true, theirsOpen = false;
        QJsonObject theirsPressed;
        QTimer::singleShot(500, this, [&] {
            QWidget* theirs = QApplication::activeModalWidget();
            QTimer::singleShot(800, this, [&] {
                theirsBrowseIsQts = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()) != nullptr
                                    && QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
                call("set_dialog", {{"press", "Cancel"}});
            });
            theirsPressed = call("set_dialog", {{"press", "Browse..."}});
            theirsAfter = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            theirsOpen = QApplication::activeModalWidget() == theirs;
            if (auto* d = qobject_cast<QDialog*>(theirs)) d->reject();
        });
        app->exportAsImage->trigger();   // the user's
        QVERIFY2(!failed(theirsPressed) && text(theirsPressed).contains("is open now"), qPrintable(text(theirsPressed)));
        QVERIFY(theirsBrowseIsQts);
        QVERIFY(!theirsAfter);
        QVERIFY(theirsOpen);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // The user's own: the system's, as before.
        bool forced = true;
        QTimer::singleShot(300, app, [&forced] {
            if (auto* d = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                forced = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
                d->reject();
            }
        });
        app->fileExamples->trigger();
        QVERIFY(!forced);

        // A folder chooser: Switch Workspace, reached now. (It closes the
        // documents first, asking about unsaved changes: those of the tests
        // before are not kept.)
        for (QucsDoc* doc : app->allDocuments()) doc->setDocChanged(false);
        const QString was = QucsSettings.qucsWorkspaceDir.absolutePath();
        const QString other = dir.filePath("access-workspace");
        QVERIFY(QDir().mkpath(other));
        QJsonObject chosen;
        QTimer::singleShot(800, this, [&] {
            chosen = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", "Directory"}, {"value", other}}}}, {"press", "Choose"}});
        });
        r = call("trigger_action", {{"action", "Project > Switch Workspace..."}}, 20000);
        QVERIFY2(!failed(r) && text(r).contains("waits for an answer"), qPrintable(text(r)));
        QTRY_VERIFY_WITH_TIMEOUT(!chosen.isEmpty(), 10000);
        QVERIFY2(!failed(chosen), qPrintable(text(chosen)));
        QTRY_COMPARE(QFileInfo(QucsSettings.qucsWorkspaceDir.absolutePath()).canonicalFilePath(), QFileInfo(other).canonicalFilePath());
        QVERIFY(app->switchWorkspace(was));
        QCOMPARE(QucsSettings.qucsWorkspaceDir.absolutePath(), was);

        // Refused, each with what does it instead.
        r = call("trigger_action", {{"action", "File > Save as..."}});
        QVERIFY2(failed(r) && text(r).contains("open_document and save_document do this"), qPrintable(text(r)));
        r = call("trigger_action", {{"action", "File > Print..."}});
        QVERIFY2(failed(r) && text(r).contains("prints on paper") && text(r).contains("export_image"), qPrintable(text(r)));
        r = call("trigger_action", {{"action", "File > Print Fit to Page..."}});
        QVERIFY2(failed(r) && text(r).contains("prints on paper"), qPrintable(text(r)));
        QVERIFY(QApplication::activeModalWidget() == nullptr);
    }

    // Every toolbar button is a menu action too, which trigger_action
    // reaches (it walks the menu bar); the simulator's list, the one
    // toolbar item that is no action, is set_simulator's.
    void everyToolbarButtonIsInAMenu()
    {
        QSet<QAction*> inMenus;
        const std::function<void(QMenu*)> walk = [&](QMenu* menu) {
            for (QAction* a : menu->actions()) {
                inMenus.insert(a);
                if (a->menu() != nullptr) walk(a->menu());
            }
        };
        for (QAction* top : app->menuBar()->actions())
            if (top->menu() != nullptr) walk(top->menu());
        QStringList missing;
        int buttons = 0;
        for (QToolBar* bar : app->findChildren<QToolBar*>()) {
            if (bar->window() != app) continue;
            for (QAction* a : bar->actions()) {
                if (a->isSeparator() || qobject_cast<QWidgetAction*>(a) != nullptr) continue;
                if (bar->widgetForAction(a) != nullptr && qobject_cast<QToolButton*>(bar->widgetForAction(a)) == nullptr) continue;
                ++buttons;
                if (!inMenus.contains(a)) missing << bar->windowTitle() + ": " + a->text();
            }
        }
        QVERIFY(buttons > 20);
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join("; ")));
    }

    // The simulation console's Stop and Clear are in the Simulation menu,
    // where trigger_action reaches them: Stop only while a run goes.
    void theConsolesStopAndClearAreReached()
    {
        QJsonObject r = call("list_actions", {{"search", "Simulation >"}});
        QJsonObject stop, clear;
        for (const QJsonValue& v : json(r).toArray()) {
            if (v.toObject().value("action").toString() == "Simulation > Stop Simulation") stop = v.toObject();
            if (v.toObject().value("action").toString() == "Simulation > Clear Simulation Console") clear = v.toObject();
        }
        QVERIFY2(!stop.isEmpty() && !clear.isEmpty(), qPrintable(text(r)));
        QVERIFY(!stop.value("enabled").toBool());
        r = call("trigger_action", {{"action", "Simulation > Stop Simulation"}});
        QVERIFY2(failed(r) && text(r).contains("cannot be used now"), qPrintable(text(r)));
        app->simulationConsole()->console()->setPlainText("the last run's output");
        r = call("trigger_action", {{"action", "Simulation > Clear Simulation Console"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QTRY_VERIFY(app->simulationConsole()->console()->toPlainText().isEmpty());
        QVERIFY(text(call("describe_tool", {{"name", "simulate"}})).contains("Simulation > Stop Simulation"));

        // A run the user started (a simulator that takes its time): Stop
        // is there, and stops it; trash_file waits for its end.
        const QString slow = dir.filePath("slow-ngspice.sh");
        {
            QFile f(slow);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\necho \"slow ngspice\"\nsleep 5\nexit 0\n");
        }
        QFile::setPermissions(slow, QFile::permissions(slow) | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        const QString simulatorWas = QucsSettings.NgspiceExecutable;
        const auto back = qScopeGuard([simulatorWas] { QucsSettings.NgspiceExecutable = simulatorWas; });
        QucsSettings.NgspiceExecutable = slow;
        const QString sch = QucsSettings.qucsWorkspaceDir.absoluteFilePath("access-run.sch");
        QFile::remove(sch);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/RF/Miscellaneous/RCL_resonance.sch"), sch));
        QVERIFY(!failed(call("open_document", {{"path", sch}})));
        QVERIFY(QMetaObject::invokeMethod(app, "slotSimulateWithSpice"));
        QTRY_VERIFY(app->simulationConsole()->console()->toPlainText().contains("slow ngspice"));
        r = call("list_actions", {{"search", "Stop Simulation"}});
        QVERIFY2(json(r).toArray().first().toObject().value("enabled").toBool(), qPrintable(text(r)));
        r = call("trash_file", {{"path", QucsSettings.qucsWorkspaceDir.absoluteFilePath("nothing-here.txt")}});
        QVERIFY(failed(r));   // (not there)
        {
            QFile f(QucsSettings.qucsWorkspaceDir.absoluteFilePath("access-note.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        r = call("trash_file", {{"path", QucsSettings.qucsWorkspaceDir.absoluteFilePath("access-note.txt")}});
        QVERIFY2(failed(r) && text(r).contains("simulation is running"), qPrintable(text(r)));
        r = call("trigger_action", {{"action", "Simulation > Stop Simulation"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QTRY_VERIFY_WITH_TIMEOUT(!app->simulationConsole()->isRunning(), 4000);   // not the 5 s it takes
        QVERIFY(QFileInfo::exists(QucsSettings.qucsWorkspaceDir.absoluteFilePath("access-note.txt")));
        QVERIFY(!failed(call("close_document", {{"path", sch}, {"unsaved", "discard"}})));
    }

    // rename_file and trash_file, as the File Browser renames and trashes:
    // the documents open from a file follow it, or close with it; the trash
    // keeps what goes (here a folder of the test's, QUCS_TRASH_DIR, not the
    // user's). Not the workspace, the home folder or the project open now.
    void filesAreRenamedAndTrashed()
    {
        const QByteArray trashWas = qgetenv("QUCS_TRASH_DIR");
        const auto restore = qScopeGuard([trashWas] {
            if (trashWas.isEmpty()) qunsetenv("QUCS_TRASH_DIR");
            else qputenv("QUCS_TRASH_DIR", trashWas);
        });
        const QString trash = dir.filePath("access-trash");
        qputenv("QUCS_TRASH_DIR", trash.toUtf8());
        const QString folder = QucsSettings.qucsWorkspaceDir.absoluteFilePath("access3");
        QVERIFY(QDir().mkpath(folder + "/sub"));
        const auto put = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write("x\n") == 2;
        };
        QVERIFY(put(folder + "/notes.txt") && put(folder + "/x.txt") && put(folder + "/y.txt"));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/amp.sch"}, {"replace", true}})));

        // A file closed: renamed in its folder.
        QJsonObject r = call("rename_file", {{"path", folder + "/notes.txt"}, {"to", "readme.txt"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(folder + "/readme.txt") && !QFileInfo::exists(folder + "/notes.txt"));
        QCOMPARE(json(r).toObject().value("to").toString(), QDir::toNativeSeparators(folder + "/readme.txt"));
        // An open schematic: its tab follows, its unsaved changes kept.
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 200}, {"y", 100}})));
        r = call("rename_file", {{"path", folder + "/amp.sch"}, {"to", "amp2.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("documents").toArray().first().toString(),
                 QStringLiteral("amp.sch -> %1").arg(QDir::toNativeSeparators(folder + "/amp2.sch")));
        QVERIFY2(text(r).contains("Data Set"), qPrintable(text(r)));
        QucsDoc* amp = app->findDoc(folder + "/amp2.sch");
        QVERIFY(amp != nullptr && amp->getDocChanged());
        QVERIFY(app->findDoc(folder + "/amp.sch") == nullptr);
        QVERIFY(!failed(call("save_document", {{"path", folder + "/amp2.sch"}})));
        QVERIFY(QFileInfo::exists(folder + "/amp2.sch") && !QFileInfo::exists(folder + "/amp.sch"));
        // Moved: into a folder there, or as the name the path ends in.
        r = call("rename_file", {{"path", folder + "/readme.txt"}, {"to", folder + "/sub"}});
        QVERIFY2(!failed(r) && QFileInfo::exists(folder + "/sub/readme.txt"), qPrintable(text(r)));
        r = call("rename_file", {{"path", folder + "/sub/readme.txt"}, {"to", folder + "/manual.txt"}});
        QVERIFY2(!failed(r) && QFileInfo::exists(folder + "/manual.txt"), qPrintable(text(r)));
        // An open document moved by a path: its tab follows too.
        r = call("rename_file", {{"path", folder + "/amp2.sch"}, {"to", folder + "/sub"}});
        QVERIFY2(!failed(r) && app->findDoc(folder + "/sub/amp2.sch") != nullptr, qPrintable(text(r)));
        QVERIFY(app->findDoc(folder + "/amp2.sch") == nullptr);
        QVERIFY(!failed(call("rename_file", {{"path", folder + "/sub/amp2.sch"}, {"to", folder}})));
        QVERIFY(app->findDoc(folder + "/amp2.sch") != nullptr);
        // Another suffix: said.
        r = call("rename_file", {{"path", folder + "/manual.txt"}, {"to", "manual.md"}});
        QVERIFY2(!failed(r) && text(r).contains("Its suffix changed (.txt to .md)"), qPrintable(text(r)));
        // Refused, and nothing moved.
        r = call("rename_file", {{"path", folder + "/x.txt"}, {"to", "y.txt"}});
        QVERIFY2(failed(r) && text(r).contains("one of that name already"), qPrintable(text(r)));
        r = call("rename_file", {{"path", folder + "/x.txt"}, {"to", folder + "/y.txt"}});
        QVERIFY2(failed(r) && text(r).contains("one there already"), qPrintable(text(r)));
        r = call("rename_file", {{"path", folder + "/x.txt"}, {"to", ".."}});
        QVERIFY2(failed(r), qPrintable(text(r)));
        r = call("rename_file", {{"path", folder + "/sub"}, {"to", folder + "/sub/inner"}});
        QVERIFY2(failed(r) && text(r).contains("cannot go into itself"), qPrintable(text(r)));
        r = call("rename_file", {{"path", QucsSettings.qucsWorkspaceDir.absolutePath()}, {"to", "elsewhere"}});
        QVERIFY2(failed(r) && text(r).contains("is the workspace"), qPrintable(text(r)));
        r = call("rename_file", {{"path", folder + "/nothing.txt"}, {"to", "something.txt"}});
        QVERIFY2(failed(r) && text(r).contains("There is no"), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(folder + "/x.txt") && QFileInfo::exists(folder + "/y.txt"));

        // To the trash: a file closed.
        r = call("trash_file", {{"path", folder + "/y.txt"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/y.txt") && QFileInfo::exists(trash + "/y.txt"));
        QCOMPARE(json(r).toObject().value("in trash").toString(), QDir::toNativeSeparators(trash + "/y.txt"));
        // One of the name there already: kept beside it.
        QVERIFY(put(folder + "/y.txt"));
        QVERIFY(!failed(call("trash_file", {{"path", folder + "/y.txt"}})));
        QVERIFY(QFileInfo::exists(trash + "/y.txt 2"));
        // An open document with unsaved changes: refused.
        QVERIFY(!failed(call("add_component", {{"type", "L"}, {"name", "L1"}, {"x", 300}, {"y", 100}, {"path", folder + "/amp2.sch"}})));
        r = call("trash_file", {{"path", folder + "/amp2.sch"}});
        QVERIFY2(failed(r) && text(r).contains("unsaved changes"), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(folder + "/amp2.sch"));
        // Saved: it goes, and its tab closes.
        QVERIFY(!failed(call("save_document", {{"path", folder + "/amp2.sch"}})));
        r = call("trash_file", {{"path", folder + "/amp2.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("closed").toArray().first().toString(), folder + "/amp2.sch");
        QVERIFY(app->findDoc(folder + "/amp2.sch") == nullptr);
        QVERIFY(QFileInfo::exists(trash + "/amp2.sch"));
        // A folder, with the document open from it.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/sub/inner.sch"}, {"replace", true}})));
        r = call("trash_file", {{"path", folder + "/sub"}});
        QVERIFY2(!failed(r) && app->findDoc(folder + "/sub/inner.sch") == nullptr, qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(trash + "/sub/inner.sch"));
        // Never the workspace, nor the project open now.
        r = call("trash_file", {{"path", QucsSettings.qucsWorkspaceDir.absolutePath()}});
        QVERIFY2(failed(r) && text(r).contains("is the workspace"), qPrintable(text(r)));
        QVERIFY(!failed(call("new_project", {{"name", "access_prj"}})));
        QVERIFY(!failed(call("open_project", {{"name", "access_prj"}})));
        const QString project = QucsSettings.QucsWorkDir.absolutePath();
        r = call("trash_file", {{"path", project}});
        QVERIFY2(failed(r) && text(r).contains("project open now"), qPrintable(text(r)));
        r = call("rename_file", {{"path", project}, {"to", "other_prj"}});
        QVERIFY2(failed(r) && text(r).contains("project open now"), qPrintable(text(r)));
        QVERIFY(QFileInfo(project).isDir());
        app->slotMenuProjClose();
        // trash_file is asked about every time; rename_file as a change is.
        QVERIFY(control->irreversible("trash_file", {{"path", folder + "/x.txt"}}));
        QVERIFY(!control->irreversible("rename_file", {{"path", folder + "/x.txt"}, {"to", "z.txt"}}));
        QVERIFY(!control->readOnlyTools().contains("rename_file") && !control->readOnlyTools().contains("trash_file"));
    }

    // ---- round 6: the gaps of one session (qucs-mcp-wishlist)

    // An ngspice .OPTIONS option with no value - a flag, written alone - is
    // set by 'flags', by true in 'equations' or by its name alone in a list;
    // "" takes it away; no other block takes one.
    void optionsTakeFlags()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QJsonObject r = call("add_component", {{"type", "SpiceOptions"}, {"name", "OPT1"}, {"x", 100}, {"y", 100},
                                               {"equations", QJsonObject{{"reltol", "1e-4"}}}, {"flags", QJsonArray{"noinit"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Component* opt = sch->getComponentByName("OPT1");
        QVERIFY(opt->getProperty("noinit") != nullptr);
        QVERIFY(opt->getProperty("noinit")->Value.isEmpty());
        QString expr = opt->getExpression(spicecompat::SPICEDefault);
        QVERIFY2(expr.contains(".OPTION noinit\n") && expr.contains(".OPTION reltol = 1e-4"), qPrintable(expr));
        QVERIFY2(sch->documentText().contains("\"noinit=\""), qPrintable(opt->save()));
        // Marked so where it is read.
        bool marked = false;
        for (const QJsonValue& v : componentIn(json(call("get_schematic")).toObject(), "OPT1").value("properties").toArray())
            marked = marked || (v.toObject().value("name").toString() == "noinit" && v.toObject().value("flag").toBool());
        QVERIFY(marked);
        // true in 'equations', a name alone in a list.
        QVERIFY(!failed(call("edit_component", {{"name", "OPT1"}, {"equations", QJsonObject{{"keepopinfo", true}}}})));
        QVERIFY(!failed(call("edit_component", {{"name", "OPT1"}, {"equations", QJsonArray{"noopiter", "gmin=1e-13"}}})));
        expr = sch->getComponentByName("OPT1")->getExpression(spicecompat::SPICEDefault);
        QVERIFY2(expr.contains(".OPTION keepopinfo\n") && expr.contains(".OPTION noopiter\n") && expr.contains(".OPTION gmin = 1e-13"),
                 qPrintable(expr));
        // Taken away by "" (and by false).
        QVERIFY(!failed(call("edit_component", {{"name", "OPT1"}, {"equations", QJsonObject{{"noinit", ""}, {"noopiter", false}}}})));
        expr = sch->getComponentByName("OPT1")->getExpression(spicecompat::SPICEDefault);
        QVERIFY2(!expr.contains("noinit") && !expr.contains("noopiter") && expr.contains("keepopinfo"), qPrintable(expr));
        // Nowhere else: an equation of Eqn has a value.
        QVERIFY(!failed(call("add_component", {{"type", "Eqn"}, {"name", "Eqn1"}, {"x", 300}, {"y", 100}})));
        r = call("edit_component", {{"name", "Eqn1"}, {"flags", QJsonArray{"x"}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(".OPTIONS"), qPrintable(text(r)));
        QVERIFY(failed(call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonObject{{"y", true}}}})));
        QVERIFY(failed(call("add_component", {{"type", "R"}, {"name", "R9"}, {"x", 500}, {"y", 100}, {"flags", QJsonArray{"x"}}})));
        QVERIFY(failed(call("edit_component", {{"name", "OPT1"}, {"flags", QJsonArray{"has space"}}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A symbol's port shows its instances a label beside the pin in place of
    // its name (the net's, which the netlist keeps), or nothing: set with
    // edit_painting, saved after the name in quotes, read back by the
    // symbol and by each instance.
    void aPortShowsALabel()
    {
        using qucs_s::portsym::read;
        QCOMPARE(read("inp").name, QStringLiteral("inp"));
        QVERIFY(!read("inp").labelSet);
        QCOMPARE(read("inp \"+\"").name, QStringLiteral("inp"));
        QCOMPARE(read("inp \"+\"").label, QStringLiteral("+"));
        QVERIFY(read("out \"\"").labelSet);
        QVERIFY(read("out \"\"").label.isEmpty());
        QCOMPARE(read("a \"two words\"").label, QStringLiteral("two words"));
        QVERIFY(!read("odd\"").labelSet);   // (one quote: no label)
        QCOMPARE(qucs_s::portsym::write({"inp", "+", true}), QStringLiteral("inp \"+\""));
        QCOMPARE(qucs_s::portsym::write({"inp", "", false}), QStringLiteral("inp"));

        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sub = front();
        int y = 100;
        for (const char* name : {"inp", "inn", "out"}) {
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}})));
            y += 60;
        }
        QVERIFY(!failed(call("save_document", {{"as", "labelled"}})));
        QVERIFY(!failed(call("make_symbol", {{"sides", QJsonObject{{"inp", "left"}, {"inn", "left"}, {"out", "right"}}}})));
        // The ports, by number among the symbol's paintings.
        QJsonArray paintings = json(call("get_schematic", {{"symbol", true}})).toObject().value("symbol paintings").toArray();
        QHash<QString, int> portAt;   // a port's name: its painting's number
        for (const QJsonValue& v : std::as_const(paintings))
            if (v.toObject().value("type").toString() == "port")
                portAt.insert(v.toObject().value("name").toString(), v.toObject().value("painting").toInt());
        QVERIFY2(portAt.size() == 3, qPrintable(QJsonDocument(paintings).toJson()));
        const auto numberOf = [&](const QString& port) { return portAt.value(port, -1); };
        QJsonObject r = call("edit_painting", {{"symbol", true}, {"painting", numberOf("inp")}, {"label", "+"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", numberOf("inn")}, {"label", "-"}})));
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", numberOf("out")}, {"label", ""}})));
        QVERIFY(failed(call("edit_painting", {{"symbol", true}, {"painting", numberOf("out")}, {"label", "say \"x\""}})));
        paintings = json(call("get_schematic", {{"symbol", true}})).toObject().value("symbol paintings").toArray();
        QCOMPARE(paintings.at(numberOf("inp") - 1).toObject().value("label").toString(), QStringLiteral("+"));
        QVERIFY(paintings.at(numberOf("out") - 1).toObject().value("label").isString());
        QVERIFY(paintings.at(numberOf("out") - 1).toObject().value("label").toString().isEmpty());
        // The file: after the name, in quotes; the name - the net's - kept.
        const QString file = sub->documentText();
        QVERIFY2(file.contains(" inp \"+\">") && file.contains(" inn \"-\">") && file.contains(" out \"\">"), qPrintable(file));
        // Kept when the ports are matched with the schematic's again.
        QVERIFY(!failed(call("save_document")));
        // An instance: its pins keep their names, and draw the labels.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        r = call("add_component", {{"type", "Sub"}, {"name", "X1"}, {"x", 300}, {"y", 200},
                                   {"properties", QJsonObject{{"File", "labelled.sch"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Component* x1 = front()->getComponentByName("X1");
        QCOMPARE(x1->Ports.size(), 3);
        QHash<QString, const Port*> byName;
        for (const Port* p : x1->Ports) byName.insert(p->Name, p);
        QVERIFY(byName.contains("inp") && byName.contains("inn") && byName.contains("out"));
        QCOMPARE(byName.value("inp")->shownName(), QStringLiteral("+"));
        QCOMPARE(byName.value("inn")->shownName(), QStringLiteral("-"));
        QVERIFY(byName.value("out")->shownName().isEmpty());
        QString pins = QJsonDocument(json(r).toObject().value("pins").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(pins.contains("\"label\":\"+\"") && pins.contains("\"name\":\"inp\""), qPrintable(pins));
        // null: its name again.
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("show_document", {{"path", "labelled.sch"}})));
        // And in the symbol editor: a double-click on a port asks what its
        // instances show - here a text, CLK.
        {
            auto* inn = static_cast<PortSymbol*>(*std::next(front()->a_SymbolPaints.begin(), numberOf("inn") - 1));
            QVERIFY(inn->Name == ".PortSym ");
            QTimer::singleShot(0, [] {
                QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (d == nullptr || d->objectName() != "portSymbolDialog") return;
                d->findChild<QRadioButton*>("portShowText")->setChecked(true);
                d->findChild<QLineEdit*>("portLabel")->setText("CLK");
                QVERIFY(d->findChild<QLineEdit*>("portName")->isReadOnly());   // (the schematic's Port's name)
                d->accept();
            });
            QVERIFY(inn->Dialog(front()));
            QVERIFY(inn->labelSet);
            QCOMPARE(inn->labelStr, QStringLiteral("CLK"));
            QCOMPARE(inn->editorText(), QStringLiteral("inn (shown as CLK)"));
            QTimer::singleShot(0, [] {
                QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (d == nullptr) return;
                d->findChild<QRadioButton*>("portShowNothing")->setChecked(true);
                d->accept();
            });
            QVERIFY(inn->Dialog(front()));
            QVERIFY(inn->labelSet && inn->labelStr.isEmpty());
            QTimer::singleShot(0, [] {
                if (QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget())) d->reject();
            });
            QVERIFY(!inn->Dialog(front()));   // cancelled: nothing changes
        }
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", numberOf("inp")}, {"label", QJsonValue()}})));
        QVERIFY2(front()->documentText().contains(" inp>"), qPrintable(front()->documentText()));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A schematic tool on a document that shows its symbol switches it
    // back to its schematic - and says so, naming File > Edit Schematic.
    void theSchematicToolsLeaveTheSymbol()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QJsonObject r = call("add_painting", {{"symbol", true}, {"type", "text"}, {"x", 0}, {"y", 0}, {"text", "sym"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(sch->getSymbolMode());
        r = call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 300}, {"y", 100}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!sch->getSymbolMode());
        QVERIFY2(text(r).contains("shows its schematic again") && text(r).contains("File > Edit Schematic"), qPrintable(text(r)));
        QVERIFY(sch->getComponentByName("C1") != nullptr);
        // And said once: the next call has no such note.
        r = call("edit_component", {{"name", "C1"}, {"properties", QJsonObject{{"C", "2 pF"}}}});
        QVERIFY(!text(r).contains("shows its schematic again"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // "Verilog-A": a module to start from - that OpenVAF compiles as it is,
    // where there is one - and how to write one.
    void aVerilogAModuleToStartFrom()
    {
        for (const char* asked : {"Verilog-A", "verilog_a", "new Verilog-A module"}) {
            const QJsonObject o = json(call("describe_component_type", {{"type", asked}})).toObject();
            QVERIFY2(o.value("template").toString().contains("module amp(inp, inn, out);"), asked);
        }
        const QJsonObject o = json(call("describe_component_type", {{"type", "Verilog-A"}})).toObject();
        const QString rules = QJsonDocument(o.value("rules").toArray()).toJson();
        QVERIFY(rules.contains("before the declaration") && rules.contains("type = \\\"instance\\\"") && rules.contains("DC path"));
        const QString va = o.value("template").toString();
        // Attributes before what they describe, every one of them: no
        // declaration with one after it.
        for (const QString& line : va.split('\n'))
            if (line.trimmed().startsWith("parameter")) QVERIFY2(!line.contains("(*"), qPrintable(line));
        const QString openvaf = QStandardPaths::findExecutable("openvaf");
        if (openvaf.isEmpty()) QSKIP("no OpenVAF here: the template is not compiled");
        {
            QFile f(dir.filePath("workspace/amp.va"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(va.toUtf8());
        }
        const QString before = QucsSettings.OpenVAFExecutable;
        QucsSettings.OpenVAFExecutable = openvaf;
        const QJsonObject r = call("build_verilog_a", {{"file", "amp.va"}}, 120000);
        QucsSettings.OpenVAFExecutable = before;
        QVERIFY2(json(r).toObject().value("compiled").toBool(), qPrintable(text(r)));
        QVERIFY(json(r).toObject().value("errors").toArray().isEmpty());
        // Its parameters as they are described.
        const QJsonObject module = json(call("describe_component_type", {{"type", "amp"}})).toObject();
        const QString params = QJsonDocument(module.value("parameters").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(params.contains("\"name\":\"vos\"") && params.contains("\"kind\":\"instance\"") && params.contains("open-loop voltage gain"),
                 qPrintable(params));
    }

    // Saving a subcircuit says which open schematics' instances took its new
    // symbol and each pin that moved; a wire that ended on a pin is drawn on
    // to where it is now - and a pin that comes to be on another net's wire
    // is said: the nets are compared before and after.
    void aSavedSymbolIsTakenByItsInstances()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, y] : {std::pair{"a", 100}, std::pair{"b", 200}})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}})));
        QVERIFY(!failed(call("save_document", {{"as", "twopin"}})));
        QVERIFY(!failed(call("make_symbol", {{"sides", QJsonObject{{"a", "left"}, {"b", "right"}}}})));
        QVERIFY(!failed(call("save_document")));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Sub"}, {"name", "X1"}, {"x", 300}, {"y", 200},
                                               {"properties", QJsonObject{{"File", "twopin.sch"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 300}})));
        QJsonObject r = call("connect", {{"from", "X1.1"}, {"to", "R1.1"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", "usestwopin"}})));
        // Saved as it is: the instance refreshed, its pins where they were.
        r = call("save_document", {{"path", "twopin.sch"}});
        QVERIFY2(text(r).contains("usestwopin.sch (X1)") && text(r).contains("where they were"), qPrintable(text(r)));
        // A port moved on the symbol: its pin moved, off its wire.
        QVERIFY(!failed(call("show_document", {{"path", "twopin.sch"}})));
        const QJsonArray paintings = json(call("get_schematic", {{"symbol", true}})).toObject().value("symbol paintings").toArray();
        int a = -1;
        QJsonObject port;
        for (const QJsonValue& v : paintings)
            if (v.toObject().value("type").toString() == "port" && v.toObject().value("name").toString() == "a") {
                a = v.toObject().value("painting").toInt();
                port = v.toObject();
            }
        QVERIFY(a > 0);
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", a}, {"y", port.value("y").toInt() + 20}})));
        r = call("save_document", {{"path", "twopin.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(text(r).contains("usestwopin.sch (X1)") && text(r).contains("X1.1 (a) in usestwopin.sch from")
                     && text(r).contains("X1.1 (a) wired on from") && text(r).contains("Every net is as it was"),
                 qPrintable(text(r)));
        QString checked = text(call("check_schematic", {{"path", "usestwopin.sch"}}));
        QVERIFY2(!checked.contains("X1: pin 1 is connected to nothing"), qPrintable(checked));
        // Moved again, onto the end of another net's wire: said.
        const QJsonObject pin = json(call("get_schematic", {{"path", "usestwopin.sch"}, {"components", QJsonArray{"X1"}}})).toObject()
                                    .value("components").toArray().first().toObject().value("pins").toArray().first().toObject();
        const int px = pin.value("x").toInt(), py = pin.value("y").toInt() + 20;
        QVERIFY(!failed(call("add_component", {{"path", "usestwopin.sch"}, {"type", "R"}, {"name", "R2"}, {"x", 100}, {"y", 500}})));
        r = call("connect", {{"path", "usestwopin.sch"}, {"from", "R2.1"}, {"to", QJsonArray{px, py}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("show_document", {{"path", "twopin.sch"}})));
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", a}, {"y", port.value("y").toInt() + 40}})));
        r = call("save_document", {{"path", "twopin.sch"}});
        QVERIFY2(!failed(r) && text(r).contains("the nets of usestwopin.sch are not as they were") && text(r).contains("R2.1"), qPrintable(text(r)));
        // (Undo there does not put them back: said what does - sixth round.)
        QVERIFY2(text(r).contains("The new symbol is no undo step there") && text(r).contains("connect each pin by its name")
                     && !text(r).contains("undo there puts back"),
                 qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"path", "usestwopin.sch"}, {"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", "twopin.sch"}, {"unsaved", "discard"}})));
    }

    // A part of another type put in one's place, its pins taking the old
    // pins' nets - by 'pins', by name, by number - turned and placed so the
    // pins meet their wiring; one step to undo.
    void aComponentIsReplacedKeepingItsNets()
    {
        // A subcircuit of three ports: inp, inn, out.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        int y = 100;
        for (const char* name : {"inp", "inn", "out"}) {
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}})));
            y += 60;
        }
        QVERIFY(!failed(call("save_document", {{"as", "amp3"}})));
        QVERIFY(!failed(call("make_symbol", {{"sides", QJsonObject{{"inp", "left"}, {"inn", "left"}, {"out", "right"}}}})));
        QVERIFY(!failed(call("save_document")));
        // An inverter's op-amp, its three pins on labelled nets.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "OpAmp"}, {"name", "OP1"}, {"x", 400}, {"y", 300}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 200}, {"rotation", 1}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 200}, {"y", 420}, {"rotation", 1}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R3"}, {"x", 600}, {"y", 300}, {"rotation", 1}})));
        QVERIFY(!failed(call("connect", {{"from", "OP1.1"}, {"to", "R2.2"}})));
        QVERIFY(!failed(call("connect", {{"from", "OP1.2"}, {"to", "R1.2"}})));
        QVERIFY(!failed(call("connect", {{"from", "OP1.3"}, {"to", "R3.1"}})));
        // (The OpAmp's pin 1 is its - input, pin 2 its +.)
        QVERIFY(!failed(call("set_label", {{"at", "R2.2"}, {"name", "minus"}})));
        QVERIFY(!failed(call("set_label", {{"at", "R1.2"}, {"name", "plus"}})));
        QVERIFY(!failed(call("set_label", {{"at", "R3.1"}, {"name", "vo"}})));
        QVERIFY(!failed(call("add_component", {{"type", ".DC"}, {"name", "DC1"}, {"x", 100}, {"y", 600}})));
        QVERIFY(!failed(call("save_document", {{"as", "inverter6"}})));
        // What the old pins are on, by net label.
        const auto netOfPin = [&](const QString& pin) {
            const QJsonObject s = json(call("get_schematic")).toObject();
            for (const QJsonValue& n : s.value("nets").toArray())
                for (const QJsonValue& p : n.toObject().value("pins").toArray())
                    if (p.toString() == pin) return n.toObject().value("net").toString();
            return QString();
        };
        QCOMPARE(netOfPin("OP1.1"), QStringLiteral("minus"));
        QCOMPARE(netOfPin("OP1.2"), QStringLiteral("plus"));
        QCOMPARE(netOfPin("OP1.3"), QStringLiteral("vo"));
        // Refused: a pin mapped to one the new part has not; an old pin with
        // wiring left out; a type there is not.
        QJsonObject r = call("replace_component", {{"name", "OP1"}, {"type", "Sub"}, {"properties", QJsonObject{{"File", "amp3.sch"}}},
                                                   {"pins", QJsonObject{{"1", "nope"}}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("inp") && text(r).contains("inn"), qPrintable(text(r)));
        r = call("replace_component", {{"name", "OP1"}, {"type", "Sub"}, {"properties", QJsonObject{{"File", "amp3.sch"}}},
                                       {"pins", QJsonObject{{"1", "inn"}, {"2", "inp"}}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains("OP1.3"), qPrintable(text(r)));
        QVERIFY(failed(call("replace_component", {{"name", "OP1"}, {"type", "NoSuchType"}})));
        QVERIFY(sch->getComponentByName("OP1")->Model == QLatin1String("OpAmp"));
        // Replaced: - to inn, + to inp, out to out.
        r = call("replace_component", {{"name", "OP1"}, {"type", "Sub"}, {"properties", QJsonObject{{"File", "amp3.sch"}}},
                                       {"pins", QJsonObject{{"1", "inn"}, {"2", "inp"}, {"3", "out"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject o = json(r).toObject();
        QCOMPARE(o.value("name").toString(), QStringLiteral("OP1"));   // (the name kept)
        QCOMPARE(o.value("type").toString(), QStringLiteral("Sub"));
        QCOMPARE(o.value("pins taken").toArray().size(), 3);
        QVERIFY2(o.value("placed").toString().contains("centred at"), qPrintable(text(r)));
        Component* c = sch->getComponentByName("OP1");
        QCOMPARE(c->Model, QStringLiteral("Sub"));
        const auto pinOf = [&](const QString& port) {
            for (int i = 0; i < c->Ports.size(); ++i)
                if (c->Ports.at(i)->Name == port) return QStringLiteral("OP1.%1").arg(i + 1);
            return QString();
        };
        QCOMPARE(netOfPin(pinOf("inp")), QStringLiteral("plus"));
        QCOMPARE(netOfPin(pinOf("inn")), QStringLiteral("minus"));
        QCOMPARE(netOfPin(pinOf("out")), QStringLiteral("vo"));
        // Each resistor still on its net.
        QCOMPARE(netOfPin("R2.2"), QStringLiteral("minus"));
        QCOMPARE(netOfPin("R1.2"), QStringLiteral("plus"));
        QCOMPARE(netOfPin("R3.1"), QStringLiteral("vo"));
        // No wire drawn across the new symbol (hard to read): placed so -
        // a wire's last stretch to one of its pins, along the pin's stub,
        // aside.
        const QRect bounds = QRect(QPoint(c->x1, c->y1), QPoint(c->x2, c->y2)).normalized().translated(c->center()).adjusted(1, 1, -1, -1);
        for (const Wire* w : sch->a_DocWires) {
            QPoint a = w->P1(), b = w->P2();
            const QPoint along((b.x() > a.x()) - (b.x() < a.x()), (b.y() > a.y()) - (b.y() < a.y()));
            const int length = std::abs(b.x() - a.x()) + std::abs(b.y() - a.y());
            const auto onPin = [&](const QPoint& p) {
                for (const Port* port : c->Ports)
                    if (c->center() + QPoint(port->x, port->y) == p) return true;
                return false;
            };
            const int from = onPin(a) ? std::min(12, length) : 0, to = onPin(b) ? std::max(length - 12, 0) : length;
            if (from >= to) continue;
            QVERIFY2(!QRect(a + along * from, a + along * to).normalized().intersects(bounds),
                     qPrintable(QStringLiteral("a wire %1,%2 - %3,%4 across %5").arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2).arg(text(r))));
        }
        // In the netlist: the subcircuit's line with its nodes in its ports' order.
        const QString netlist = text(call("get_netlist"));
        static const QRegularExpression line(QStringLiteral("(?m)^XOP1 plus minus vo "));
        QVERIFY2(line.match(netlist).hasMatch(), qPrintable(netlist));
        // One step back is the op-amp, wired as it was.
        QVERIFY(!failed(call("undo")));
        c = sch->getComponentByName("OP1");
        QCOMPARE(c->Model, QStringLiteral("OpAmp"));
        QCOMPARE(netOfPin("OP1.3"), QStringLiteral("vo"));
        // By number when the pins have no names in common (a resistor for a
        // resistor): in place, turned as it was.
        r = call("replace_component", {{"name", "R3"}, {"type", "R"}, {"properties", QJsonObject{{"R", "47k"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("mapped").toString(), QStringLiteral("by number"));
        QVERIFY2(json(r).toObject().value("placed").toString().contains("every pin on its old place"), qPrintable(text(r)));
        QCOMPARE(sch->getComponentByName("R3")->getProperty("R")->Value, QStringLiteral("47k"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", "amp3.sch"}, {"unsaved", "discard"}})));
    }

    // Where each shown text of a component is: its name, then each
    // property shown, one under the other from its text's corner.
    void aComponentsTextsHaveTheirBoxes()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        const QJsonObject r = call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 200}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Component* c = front()->getComponentByName("R1");
        const QJsonArray texts = json(r).toObject().value("texts").toArray();
        int shown = c->showName ? 1 : 0;
        for (const Property* p : c->Props) shown += p->display ? 1 : 0;
        QCOMPARE(texts.size(), shown);
        QCOMPARE(texts.first().toObject().value("text").toString(), QStringLiteral("R1"));
        const QJsonArray first = texts.first().toObject().value("box").toArray();
        QCOMPARE(first.at(0).toInt(), c->cx + c->tx);
        QCOMPARE(first.at(1).toInt(), c->cy + c->ty);
        // One under the other, all within what the part's bounds hold.
        const QRect all = c->boundingRectIncludingProperties();
        for (int i = 0; i < texts.size(); ++i) {
            const QJsonArray b = texts.at(i).toObject().value("box").toArray();
            QVERIFY(b.at(2).toInt() > b.at(0).toInt() && b.at(3).toInt() > b.at(1).toInt());
            QVERIFY2(all.adjusted(-2, -2, 2, 2).contains(QRect(QPoint(b.at(0).toInt(), b.at(1).toInt()), QPoint(b.at(2).toInt(), b.at(3).toInt()))),
                     qPrintable(QJsonDocument(texts).toJson()));
            if (i > 0) QCOMPARE(b.at(1).toInt(), texts.at(i - 1).toObject().value("box").toArray().at(3).toInt());
        }
        // In get_schematic too.
        QVERIFY(componentIn(json(call("get_schematic")).toObject(), "R1").value("texts").toArray().size() == shown);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The properties not shown on the schematic that the netlist line uses
    // all the same are flagged, with where they are in it: an OpAmp's Umax.
    void hiddenPropertiesInTheNetlistAreFlagged()
    {
        const QJsonObject o = json(call("describe_component_type", {{"type", "OpAmp"}})).toObject();
        QVERIFY2(o.value("hidden properties").toString().contains("Umax"), qPrintable(QJsonDocument(o).toJson()));
        bool umax = false;
        for (const QJsonValue& h : o.value("netlist").toObject().value("hidden but in it").toArray())
            if (h.toObject().value("name").toString() == "Umax") {
                umax = true;
                QVERIFY(!h.toObject().value("in the line").toString().isEmpty());
                QCOMPARE(h.toObject().value("default").toString(), QStringLiteral("15 V"));
            }
        QVERIFY(umax);
        // G is shown: not among them.
        QVERIFY(!QJsonDocument(o.value("netlist").toObject().value("hidden but in it").toArray()).toJson().contains("\"G\""));
    }

    // A file with a component line of more values than its type has
    // properties: said when it is opened (read positionally, one too many
    // shifts the rest).
    void aLineWithAValueTooManyIsSaidOnOpening()
    {
        const QString file = dir.filePath("workspace/shifted.sch");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n"
                    "  <R R1 1 100 100 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0 \"extra\" 0 \"more\" 0>\n"
                    "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
        }
        QJsonObject r = call("open_document", {{"path", "shifted.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(text(r).contains("R1 (R)") && text(r).contains("left out"), qPrintable(text(r)));
        // Brought to the front again: nothing to say.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        r = call("open_document", {{"path", "shifted.sch"}});
        QVERIFY(!text(r).contains("left out"));
        QVERIFY(!failed(call("close_document", {{"path", "shifted.sch"}, {"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The .sch text of set_schematic is read in order: a value left out or
    // two swapped read as other properties' - told, as the JSON form's are:
    // a value that is no number where one is wanted, a word that is not one
    // of its property's choices.
    void textValuesAreCheckedToo()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        const auto set = [&](const QString& rLine) {
            return call("set_schematic", {{"text", "<Components>\n" + rLine + "\n</Components>\n"}});
        };
        // Temp left out: Tnom reads "european", the symbol its default.
        QJsonObject r = set("  <R R1 1 100 100 15 -26 0 1 \"1k\" 1 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QString values = QJsonDocument(json(r).toObject().value("values").toArray()).toJson();
        QVERIFY2(values.contains("Tnom = \\\"european\\\" is no number, and no equation block here defines european"), qPrintable(values));
        QVERIFY2(values.contains("by name"), qPrintable(values));
        // Tnom and the symbol swapped: each told.
        r = set("  <R R1 1 100 100 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"european\" 0 \"26.85\" 0>");
        values = QJsonDocument(json(r).toObject().value("values").toArray()).toJson();
        QVERIFY2(values.contains("Tnom = \\\"european\\\"") && values.contains("Symbol = \\\"26.85\\\" is not one of its choices (european, US)"),
                 qPrintable(values));
        // Right: nothing to tell - a parameter's name or an expression neither.
        r = set("  <R R1 1 100 100 15 -26 0 1 \"{Rload}\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>");
        QVERIFY2(!json(r).toObject().contains("values"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // An equation block's answer lists its equations as they are then, in
    // the form get_schematic gives and 'equations' takes: what came of what
    // was given, at a look. {"k": null} in the list takes k away.
    void equationsAreEchoed()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "Eqn"}, {"name", "Eqn1"}, {"x", 100}, {"y", 100},
                                               {"equations", QJsonArray{"gain=2", "k=gain*3"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonArray eq = json(r).toObject().value("equations").toArray();
        QVERIFY2(eq.contains(QJsonValue("gain=2")) && eq.contains(QJsonValue("k=gain*3")), qPrintable(text(r)));
        const QStringList order = QVariant(eq.toVariantList()).toStringList();
        QVERIFY(order.indexOf("gain=2") < order.indexOf("k=gain*3"));
        r = call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonArray{QJsonObject{{"gain", QJsonValue::Null}}, "k=5"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        eq = json(r).toObject().value("equations").toArray();
        QVERIFY2(!eq.contains(QJsonValue("gain=2")) && eq.contains(QJsonValue("k=5")), qPrintable(text(r)));
        r = call("add_component", {{"type", "SpiceOptions"}, {"name", "OPT1"}, {"x", 300}, {"y", 100}, {"flags", QJsonArray{"noinit"}}});
        QVERIFY2(json(r).toObject().value("equations").toArray().contains(QJsonValue("noinit")), qPrintable(text(r)));
        // Not an equation block: no list.
        r = call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 300}});
        QVERIFY(!json(r).toObject().contains("equations"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // connect to "ground": a ground symbol of the pin's own - on it under a
    // standing part, a little away and wired beside a lying one - rather
    // than a ground found by its number among them (GND#2.1).
    void aPinIsConnectedToGround()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}, {"rotation", 1}});
        QJsonArray pins = json(r).toObject().value("pins").toArray();
        const QString lower = pins.at(0).toObject().value("y").toInt() > pins.at(1).toObject().value("y").toInt() ? "R1.1" : "R1.2";
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 300}, {"y", 100}})));
        r = call("connect", {{"from", lower}, {"to", "ground"}});
        QVERIFY2(!failed(r) && text(r).contains("a ground symbol on it"), qPrintable(text(r)));
        r = call("connect", {{"from", "GND"}, {"to", "R2.2"}});   // (either end; any case)
        QVERIFY2(!failed(r) && text(r).contains("wired to it"), qPrintable(text(r)));
        r = call("connect", {{"from", "R2.2"}, {"to", "ground"}});
        QVERIFY2(!failed(r) && text(r).contains("on ground already"), qPrintable(text(r)));
        QVERIFY(failed(call("connect", {{"from", "ground"}, {"to", "gnd"}})));
        int grounds = 0;
        for (const Component* c : front()->a_DocComps) grounds += c->Model == QLatin1String("GND");
        QCOMPARE(grounds, 2);
        const QJsonObject map = json(call("get_netlist", {{"map", true}})).toObject();
        const QJsonArray onGround = map.value("nodes").toObject().value("0").toArray();
        QVERIFY2(onGround.contains(QJsonValue(lower)) && onGround.contains(QJsonValue("R2.2")), qPrintable(QJsonDocument(map).toJson()));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Each net as a set of pins, from get_schematic - the ground symbols'
    // own pins left out (arrange puts them back anew, numbered otherwise).
    QList<QStringList> netsOfFront()
    {
        const QJsonObject s = json(call("get_schematic")).toObject();
        QSet<QString> grounds;
        for (const QJsonValue& c : s.value("components").toArray())
            if (c.toObject().value("type").toString() == "GND")
                grounds << (c.toObject().value("ref").toString().isEmpty() ? QStringLiteral("GND") : c.toObject().value("ref").toString());
        QList<QStringList> nets;
        for (const QJsonValue& n : s.value("nets").toArray()) {
            QStringList pins;
            for (const QJsonValue& p : n.toObject().value("pins").toArray())
                if (!grounds.contains(p.toString().section('.', 0, 0))) pins << p.toString();
            pins.sort();
            if (!pins.isEmpty()) nets << pins;
        }
        std::sort(nets.begin(), nets.end());
        return nets;
    }

    // arrange: the parts in columns by signal flow, two-pin parts turned
    // as a schematic has them, one ground back for each piece that had one,
    // labels back, every wire drawn again - and every net as it was. One
    // step to undo; a preview changes nothing.
    void aSchematicIsArranged()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("set_schematic", {{"components", QJsonArray{
            QJsonObject{{"type", "Vac"}, {"name", "V1"}, {"x", 430}, {"y", 380}},
            QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 90}, {"y", 120}},
            QJsonObject{{"type", "C"}, {"name", "C1"}, {"x", 600}, {"y", 90}, {"rotation", 1}},
            QJsonObject{{"type", "R"}, {"name", "R2"}, {"x", 250}, {"y", 480}, {"rotation", 1}},
            QJsonObject{{"type", "C"}, {"name", "C2"}, {"x", 700}, {"y", 300}},
            QJsonObject{{"type", "R"}, {"name", "R3"}, {"x", 150}, {"y", 300}},
            QJsonObject{{"type", ".AC"}, {"name", "AC1"}, {"x", 50}, {"y", 600}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const std::pair<const char*, const char*> links[] = {{"V1.1", "R1.1"}, {"R1.2", "C1.1"}, {"R1.2", "R2.1"}, {"C1.2", "C2.1"},
                                                             {"C2.2", "R3.1"}, {"V1.2", "ground"}, {"R2.2", "ground"}, {"R3.2", "ground"}};
        for (const auto& [a, b] : links) {
            r = call("connect", {{"from", a}, {"to", b}});
            QVERIFY2(!failed(r), qPrintable(QString("%1-%2: %3").arg(a, b, text(r))));
        }
        QVERIFY(!failed(call("set_label", {{"at", "C1.2"}, {"name", "mid"}})));
        // A label on a wire to nothing, of a name no pin's net has: it joins nothing.
        QVERIFY(!failed(call("add_wire", {{"points", QJsonArray{QJsonArray{900, 600}, QJsonArray{960, 600}}}})));
        QVERIFY(!failed(call("set_label", {{"at", QJsonArray{930, 600}}, {"name", "stub"}})));
        const QList<QStringList> nets = netsOfFront();
        Schematic* sch = front();
        const QString before = sch->documentText();
        const int undo = sch->undoIndex();

        // (Where the view is aside: it may have scrolled.)
        const auto drawn = [](QString t) { return t.remove(QRegularExpression(QStringLiteral("  <View=[^>]*>\n"))); };
        r = call("arrange", {{"preview", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(drawn(sameText(sch->documentText())), drawn(sameText(before)));   // (a put back writes each wire from its lesser end)
        QCOMPARE(sch->undoIndex(), undo);

        r = call("arrange");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject o = json(r).toObject();
        QCOMPARE(o.value("columns").toArray().first().toArray(), (QJsonArray{"V1"}));
        QCOMPARE(o.value("columns").toArray().at(1).toArray(), (QJsonArray{"R1"}));
        QCOMPARE(o.value("grounds").toInt(), 3);
        QCOMPARE(o.value("labels").toInt(), 1);
        QVERIFY2(o.value("dropped").toString().contains("stub"), qPrintable(text(r)));
        QCOMPARE(netsOfFront(), nets);
        QVERIFY(sch->documentText().contains("\"mid\""));
        QCOMPARE(sch->undoIndex(), undo + 1);   // one step
        // The signal chain lies in one row, each part apart, every pin on the grid.
        QList<QRect> boxes;
        for (const Component* c : sch->a_DocComps) {
            if (c->Ports.isEmpty()) continue;
            for (const Port* p : c->Ports) {
                QCOMPARE((c->cx + p->x) % 10, 0);
                QCOMPARE((c->cy + p->y) % 10, 0);
            }
            if (c->Model != QLatin1String("GND")) boxes << c->boundingRect();
        }
        for (int i = 0; i < boxes.size(); ++i)
            for (int j = i + 1; j < boxes.size(); ++j) QVERIFY(!boxes.at(i).intersects(boxes.at(j)));
        const auto partNamed = [&](const char* name) { return sch->getComponentByName(name); };
        QCOMPARE(partNamed("R1")->Ports.at(0)->y, partNamed("R1")->Ports.at(1)->y);   // in series: lying
        QVERIFY(partNamed("R1")->cx + partNamed("R1")->Ports.at(0)->x < partNamed("R1")->cx + partNamed("R1")->Ports.at(1)->x);   // driven from the left
        QCOMPARE(partNamed("R2")->Ports.at(0)->x, partNamed("R2")->Ports.at(1)->x);   // to ground: standing
        QVERIFY(partNamed("R2")->Ports.at(1)->y > partNamed("R2")->Ports.at(0)->y);   // ground below
        QCOMPARE(partNamed("C2")->cy + partNamed("C2")->Ports.at(0)->y, partNamed("R3")->cy + partNamed("R3")->Ports.at(0)->y);   // a straight chain
        // Undone in one step.
        QVERIFY(!failed(call("undo")));
        QCOMPARE(drawn(sameText(sch->documentText())), drawn(sameText(before)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // A subcircuit's port named GND is no ground symbol: kept on its net
        // (it was taken for one, and its net changed).
        const QString block = dir.filePath("workspace/RCBlock.sch");
        QFile::remove(block);
        QVERIFY(QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/xyce/Xyce_Examples/11-SParameters/RCBlock.sch"), block));
        QFile::setPermissions(block, QFile::ReadOwner | QFile::WriteOwner);
        QVERIFY(!failed(call("open_document", {{"path", block}})));
        const QList<QStringList> blockNets = netsOfFront();
        r = call("arrange");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(netsOfFront(), blockNets);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A netlist the simulator's netlister gives up on says why (a
    // subcircuit it cannot read) - it gave a title line alone - and says so
    // again the next time: the subcircuit taken in before it gave up was
    // remembered, and the next netlist left it out without a word.
    void aNetlistThatCannotBeWrittenSaysWhy()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Sub SUB1 1 200 200 20 -30 0 0 \"no_such_sub.sch\" 1>\n"
                                                        "  <R R1 1 400 200 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                                                        "  <GND * 1 400 230 0 0 0 0>\n"
                                                        "  <.DC DC1 1 100 400 0 40 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
                                                        "</Components>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        for (int time = 0; time < 2; ++time) {
            r = call("get_netlist");
            QVERIFY2(failed(r) && text(r).contains("no_such_sub"), qPrintable(QString("time %1: %2").arg(time).arg(text(r))));
        }
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A ground has no name: its ref (GND#2) names it to every tool that takes
    // a part's name - move, delete, select, edit_component, get_schematic's
    // 'components' - as it did to connect alone. A filtered read names the
    // nets as a full one does.
    void groundsAreToldByTheirRefs()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, x] : {std::pair{"R1", 100}, std::pair{"R2", 300}, std::pair{"R3", 500}})
            QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", name}, {"x", x}, {"y", 100}, {"rotation", 1}})));
        for (const char* pin : {"R1.2", "R2.2", "R3.2"}) {
            const QJsonObject r = call("connect", {{"from", pin}, {"to", "ground"}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
        }
        QVERIFY(!failed(call("connect", {{"from", "R2.1"}, {"to", "R3.1"}})));
        const QJsonObject full = json(call("get_schematic")).toObject();
        QHash<QString, QString> netOf;   // "R3.1" -> its net
        for (const QJsonValue& c : full.value("components").toArray())
            for (const QJsonValue& p : c.toObject().value("pins").toArray())
                netOf.insert(c.toObject().value("name").toString() + "." + QString::number(p.toObject().value("pin").toInt()),
                             p.toObject().value("net").toString());
        // Unnamed nets numbered without a gap: net1, net2 - gnd is not one.
        QStringList netNames;
        for (const QJsonValue& n : full.value("nets").toArray()) netNames << n.toObject().value("net").toString();
        QVERIFY2(netNames.contains("gnd") && !netNames.contains("net3"), qPrintable(netNames.join(',')));
        // Filtered: R3 and a ground; its nets named as in the full read.
        QJsonObject some = json(call("get_schematic", {{"components", QJsonArray{"R3", "GND#3", "nothere"}}})).toObject();
        QCOMPARE(some.value("components").toArray().size(), 2);
        QCOMPARE(some.value("not found").toArray(), QJsonArray{"nothere"});
        for (const QJsonValue& c : some.value("components").toArray())
            if (c.toObject().value("name").toString() == "R3")
                for (const QJsonValue& p : c.toObject().value("pins").toArray())
                    QCOMPARE(p.toObject().value("net").toString(), netOf.value("R3." + QString::number(p.toObject().value("pin").toInt())));
        // move, select, edit_component, delete.
        const QList<QString> before = QList<QString>{"GND#1", "GND#2", "GND#3"};
        QJsonObject r = call("move", {{"names", QJsonArray{"GND#2"}}, {"dx", 0}, {"dy", 20}});
        QVERIFY2(!failed(r) && json(r).toObject().value("moved").toArray() == QJsonArray{"GND#2"}, qPrintable(text(r)));
        r = call("move", {{"names", QJsonArray{"GND"}}, {"dx", 0}, {"dy", 20}});
        QVERIFY2(failed(r) && text(r).contains("say which, GND#1 to GND#3"), qPrintable(text(r)));
        r = call("select", {{"names", QJsonArray{"GND#1", "R1"}}});
        QVERIFY2(text(r).startsWith("2 selected"), qPrintable(text(r)));
        r = call("edit_component", {{"name", "GND#1"}, {"mirror", true}});
        QVERIFY2(!failed(r) && json(r).toObject().value("mirrored").toBool(), qPrintable(text(r)));
        QVERIFY2(before.contains(json(r).toObject().value("ref").toString()), qPrintable(text(r)));
        QVERIFY(failed(call("edit_component", {{"name", "GND#1"}, {"rename", "G1"}})));
        r = call("delete", {{"names", QJsonArray{"GND#5"}}});
        QVERIFY2(failed(r) && text(r).contains("not GND#5"), qPrintable(text(r)));
        r = call("delete", {{"names", QJsonArray{"GND#3"}}});
        QVERIFY2(!failed(r) && text(r).contains("GND#3"), qPrintable(text(r)));
        int grounds = 0;
        for (const Component* c : front()->a_DocComps) grounds += c->Model == QLatin1String("GND");
        QCOMPARE(grounds, 2);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A subcircuit made beside a schematic just saved under its name: its
    // file is found (the schematic's folder is the new one), so it has its
    // pins - a net joined by a label alone among them. A preview leaves no
    // file behind, and one it would replace as it was.
    void aSubcircuitBesideANewSchematicHasItsPins()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Vac"}, {"name", "V1"}, {"x", 100}, {"y", 200}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 150}})));
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 350}, {"y", 220}, {"rotation", 1}})));
        QVERIFY(!failed(call("connect", {{"from", "V1.2"}, {"to", "ground"}})));
        QVERIFY(!failed(call("connect", {{"from", "C1.2"}, {"to", "ground"}})));
        QVERIFY(!failed(call("connect", {{"from", "R1.2"}, {"to", "C1.1"}})));
        QVERIFY(!failed(call("set_label", {{"at", "V1.1"}, {"name", "in"}})));
        QVERIFY(!failed(call("set_label", {{"at", "R1.1"}, {"name", "in"}})));
        QDir().mkpath(dir.filePath("newfolder"));
        const QString top = dir.filePath("newfolder/top.sch");
        QVERIFY(!failed(call("save_document", {{"as", top}})));
        const QString sub = dir.filePath("newfolder/rc.sch");
        QJsonObject r = call("create_subcircuit", {{"names", QJsonArray{"R1", "C1"}}, {"save_as", "rc.sch"}, {"preview", true}});
        QVERIFY2(!json(r).toObject().value("it would fail").toBool(), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("files it would write").toArray().size() == 1, qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(sub));
        {   // one that is there, replaced in a preview: as it was
            QFile old(sub);
            QVERIFY(old.open(QIODevice::WriteOnly));
            old.write("old");
        }
        r = call("create_subcircuit", {{"names", QJsonArray{"R1", "C1"}}, {"save_as", "rc.sch"}, {"replace", true}, {"preview", true}});
        QVERIFY2(!json(r).toObject().value("it would fail").toBool(), qPrintable(text(r)));
        {
            QFile old(sub);
            QVERIFY(old.open(QIODevice::ReadOnly));
            QCOMPARE(old.readAll(), QByteArray("old"));
        }
        // A batch previewed: nothing that writes a file runs.
        r = call("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{}}}}}, {"preview", true}});
        QVERIFY2(failed(r) && text(r).contains("save_document"), qPrintable(text(r)));
        r = call("create_subcircuit", {{"names", QJsonArray{"R1", "C1"}}, {"save_as", "rc.sch"}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray ports = json(r).toObject().value("ports").toArray();
        QCOMPARE(ports.size(), 1);
        QCOMPARE(ports.at(0).toObject().value("net").toString(), QStringLiteral("in"));
        QVERIFY(QFileInfo(sub).size() > 100);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A number mistyped - digits, then letters no scale and unit - is
    // refused, not taken as the number before them (1kk: SPICE reads 1k).
    // Numbers with scales and units, words and expressions are taken.
    void aNumberMistypedIsRefused()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}, {"properties", QJsonObject{{"R", "1kk"}}}});
        QVERIFY2(failed(r) && text(r).contains("\"kk\" is no scale letter"), qPrintable(text(r)));
        QVERIFY(front()->a_DocComps.empty());
        for (const char* good : {"4.7 kOhm", "100n", "1Meg", "2.2e3", "10 GHz", "25 mil", "1 mm", "-3 dB", "{Rload*2}"}) {
            r = call("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}, {"properties", QJsonObject{{"R", good}}}});
            QVERIFY2(!failed(r), qPrintable(QString(good) + ": " + text(r)));
            QVERIFY(!failed(call("undo")));
        }
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        r = call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "10uu"}}}});
        QVERIFY2(failed(r) && text(r).contains("10uu"), qPrintable(text(r)));
        QCOMPARE(front()->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("1 kOhm"));
        r = call("replace_component", {{"name", "R1"}, {"type", "C"}, {"properties", QJsonObject{{"C", "1nn"}}}});
        QVERIFY2(failed(r) && text(r).contains("Not replaced"), qPrintable(text(r)));
        r = call("set_schematic", {{"components", QJsonArray{QJsonObject{{"type", "R"}, {"name", "R2"}, {"x", 100}, {"y", 100},
                                                                         {"properties", QJsonObject{{"R", "1kk"}}}}}}});
        QVERIFY2(failed(r) && text(r).contains("components[0]"), qPrintable(text(r)));
        // The .sch lines: read, then all put back - the schematic, whether
        // changed, its undo steps.
        const QString was = front()->snapshot();
        const bool changed = front()->getDocChanged();
        const QString history = text(call("undo_history"));
        r = call("set_schematic", {{"text", "<Components>\n  <R R2 1 100 100 15 -26 0 1 \"1kk\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 "
                                            "\"26.85\" 0 \"european\" 0>\n</Components>\n"}});
        QVERIFY2(failed(r) && text(r).contains("R2: R = \"1kk\""), qPrintable(text(r)));
        QCOMPARE(front()->snapshot(), was);
        QCOMPARE(front()->getDocChanged(), changed);
        QCOMPARE(text(call("undo_history")), history);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // add_analysis's 'plot' with an expression: a NutmegEq beside the
    // analysis computes it, and the trace shows its variable - on an AC
    // plot on the right axis. Under a simulator with no Nutmeg: refused,
    // nothing added.
    void anExpressionIsPlottedByAnEquation()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("set_label", {{"at", "R1.2"}, {"name", "out"}})));
        QJsonObject r = call("add_analysis", {{"kind", "ac"}, {"plot", QJsonArray{"out", "db(v(out))"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject answer = json(r).toObject();
        QCOMPARE(answer.value("equations").toObject().value("equations").toArray(), QJsonArray{"db_v_out=db(v(out))"});
        QCOMPARE(answer.value("steps to undo").toInt(), 3);
        const QJsonObject diagram = answer.value("diagram").toObject();
        QStringList variables;
        for (const QJsonValue& t : diagram.value("traces").toArray())
            variables << t.toObject().value("variable").toString() + "@" + t.toObject().value("axis").toString();
        QVERIFY2(variables.contains("ngspice/ac.v(out)@left") && variables.contains("ngspice/ac.db_v_out@right"), qPrintable(variables.join(' ')));
        QVERIFY2(diagram.value("note").toString().count("no dataset yet") <= 1, qPrintable(diagram.value("note").toString()));
        Component* eq = front()->getComponentByName(answer.value("equations").toObject().value("block").toString());
        QVERIFY(eq != nullptr && eq->Model == QLatin1String("NutmegEq") && eq->getProperty("Simulation")->Value == QLatin1String("AC1"));
        // Qucsator: no Nutmeg - refused, and nothing added.
        const int simulator = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        const size_t parts = front()->a_DocComps.size();
        r = call("add_analysis", {{"kind", "tran"}, {"plot", QJsonArray{"v(out)*2"}}});
        QucsSettings.DefaultSimulator = simulator;
        QVERIFY2(failed(r) && text(r).contains("Nothing was added"), qPrintable(text(r)));
        QCOMPARE(front()->a_DocComps.size(), parts);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // simulate on an untitled schematic: saved in the scratch folder first,
    // and said - not refused in the middle of a batch.
    void anUntitledSchematicIsSimulatedFromScratch()
    {
        QDir().mkpath(dir.filePath("workspace/Scratch"));   // (the workspace's own, not the cache's)
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(front()->getDocName().isEmpty());
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"x", 100}, {"y", 300}})));   // (something to run)
        const QJsonObject r = call("simulate", {{"timeout", 10}});
        QVERIFY2(text(r).contains("had no file: it is saved as"), qPrintable(text(r)));
        QVERIFY2(QFileInfo(front()->getDocName()).absolutePath() == QFileInfo(dir.filePath("workspace/Scratch")).absoluteFilePath(),
                 qPrintable(front()->getDocName()));
        QVERIFY(QFileInfo::exists(front()->getDocName()));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        // tune the same (its runs fail here: no simulator - not refused).
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"x", 100}, {"y", 300}})));
        const QJsonObject t = call("tune", {{"component", "R1"}, {"values", QJsonArray{"1k"}},
                                            {"measure", QJsonObject{{"variable", "v(out)"}, {"what", "final"}}}, {"timeout", 10}});
        QVERIFY2(!text(t).contains("has no file yet") && !front()->getDocName().isEmpty(), qPrintable(text(t)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // An untitled schematic nothing was done in is closed when a tool makes
    // or opens another: one "untitled" at a time.
    void anUntouchedUntitledTabIsClosed()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        int untitled = 0;
        for (QucsDoc* doc : app->allDocuments()) untitled += doc->getDocName().isEmpty() && dynamic_cast<Schematic*>(doc) != nullptr;
        QCOMPARE(untitled, 1);
        // One with something in it stays.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        untitled = 0;
        for (QucsDoc* doc : app->allDocuments()) untitled += doc->getDocName().isEmpty() && dynamic_cast<Schematic*>(doc) != nullptr;
        QCOMPARE(untitled, 2);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A script's 'return' at the top level gives its result, as its last
    // expression does.
    void aScriptReturnsItsResult()
    {
        if (!control->scriptingBuilt()) QSKIP("no Qml in this build");
        QJsonObject r = call("run_script", {{"script", "const n = 6 * 7;\nreturn n"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("result").toInt() == 42, qPrintable(text(r)));
        r = call("run_script", {{"script", "if (true) return 'early'\n'late'"}});
        QVERIFY2(json(r).toObject().value("result").toString() == "early", qPrintable(text(r)));
        r = call("run_script", {{"script", "6 * 7"}});
        QCOMPARE(json(r).toObject().value("result").toInt(), 42);
        r = call("run_script", {{"script", "\n\nthrow new Error('here')"}});
        QVERIFY2(failed(r) && json(r).toObject().value("line").toInt() == 3, qPrintable(text(r)));
        r = call("run_script", {{"script", "\nreturn x.y"}});   // (wrapped: its lines are still its own)
        QVERIFY2(failed(r) && json(r).toObject().value("line").toInt() == 2, qPrintable(text(r)));
    }

    // A part replaced by one of another type: its change told by the new
    // type's property names, not by their places ("property 3").
    void aChangeOfTypeIsToldByName()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "L"}, {"name", "X1"}, {"x", 100}, {"y", 100}, {"properties", QJsonObject{{"L", "100n"}}}})));
        const QJsonObject r = call("replace_component", {{"name", "X1"}, {"type", "C"}, {"properties", QJsonObject{{"C", "10m"}}}, {"preview", true}});
        const QString changes = QJsonDocument(json(r).toObject().value("would change").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(changes.contains("X1: now type C (was L), C=10m") && !changes.contains("property"), qPrintable(changes));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A schematic's file changed on disk and loaded again: the edit is the
    // file's, not the user's. A change made just after it was saved - before
    // its watch was set - is seen too.
    void anEditOnDiskIsTheFilesNotTheUsers()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        const QString file = dir.filePath("ondisk.sch");
        QVERIFY(!failed(call("save_document", {{"as", file}})));
        Schematic* sch = front();
        const quint64 revision = sch->revision();
        // Changed at once - before its watch is set (every 1.5 s): seen all
        // the same, as newer than its save.
        QTest::qWait(50);
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QString content = QString::fromUtf8(f.readAll());
            f.close();
            content.replace("\"1 kOhm\"", "\"4.7k\"");
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(content.toUtf8());
        }
        QTRY_VERIFY_WITH_TIMEOUT(sch->revision() > revision, 6000);
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("4.7k"));
        QJsonObject last;
        for (const QJsonValue& d : json(call("get_state")).toObject().value("documents").toArray())
            if (d.toObject().value("title").toString() == "ondisk.sch") last = d.toObject().value("last edit").toObject();
        QVERIFY2(last.value("by").toString().contains("changed on disk"), qPrintable(QJsonDocument(last).toJson()));
        QVERIFY(!failed(call("close_document")));
    }

    // ---- The hunt of 28 September (docs/bug_hunts/2026-09-28-claude-tools.md)

private:
    QString writeFile(const QString& name, const QByteArray& content)
    {
        const QString path = dir.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(content);
        return path;
    }
    static QByteArray readFile(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    static QByteArray schematicOf(const QByteArray& components, const QByteArray& wires = {})
    {
        return "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n" + components
               + "</Components>\n<Wires>\n" + wires + "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n";
    }
    void discardAll()
    {
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
    }

private slots:

    // A1: a netlist is not written over a document - an open one's file
    // (loaded again as the netlist, the schematic gone), nor a schematic's
    // unless 'replace' says so.
    void aNetlistDoesNotReplaceADocument()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"x", 100}, {"y", 300}})));
        const QString file = dir.filePath("workspace/victim.sch");
        QVERIFY(!failed(call("save_document", {{"as", file}})));
        const QByteArray before = readFile(file);
        QJsonObject r = call("export_netlist", {{"save_as", file}});
        QVERIFY2(failed(r) && text(r).contains("is open"), qPrintable(text(r)));
        const QString other = dir.filePath("workspace/victim_copy.sch");
        QFile::remove(other);
        QVERIFY(QFile::copy(file, other));
        r = call("export_netlist", {{"save_as", other}});
        QVERIFY2(failed(r) && text(r).contains("'replace'"), qPrintable(text(r)));
        QCOMPARE(readFile(other), before);
        QVERIFY2(!failed(call("export_netlist", {{"save_as", other}, {"replace", true}})), "replace: written");
        QVERIFY(!failed(call("export_netlist", {{"save_as", dir.filePath("workspace/victim.cir")}})));
        QCOMPARE(readFile(file), before);
        QVERIFY(!failed(call("close_document")));
    }

    // A2, A3, A9: a batch, a preview or a script runs alone - a call that
    // comes meanwhile (another conversation's) waits, and is not taken back
    // with a preview or a failed atomic batch; a batch's calls without a
    // path stay with its document when another is brought to the front.
    void callsWaitForOneThatRunsAlone()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QJsonArray adds;
        for (int i = 1; i <= 3; ++i)
            adds.append(QJsonObject{{"tool", "add_component"},
                                    {"arguments", QJsonObject{{"type", "R"}, {"name", QStringLiteral("RB%1").arg(i)}, {"x", 100 * i}, {"y", 100}}}});
        QJsonObject previewed, other;
        control->callToolFor(1, "batch", {{"calls", adds}, {"preview", true}}, [&](const QJsonObject& r) { previewed = r; });
        control->callToolFor(2, "add_component", {{"type", "C"}, {"name", "C_OTHER"}, {"x", 100}, {"y", 300}},
                             [&](const QJsonObject& r) { other = r; });
        QTRY_VERIFY_WITH_TIMEOUT(!previewed.isEmpty() && !other.isEmpty(), 10000);
        QVERIFY2(!failed(other), qPrintable(text(other)));
        QVERIFY(sch->getComponentByName("C_OTHER") != nullptr);
        QVERIFY(sch->getComponentByName("RB1") == nullptr);
        QVERIFY2(!failed(call("undo")), "its step is there to undo");
        QVERIFY(sch->getComponentByName("C_OTHER") == nullptr);
        QVERIFY(!failed(call("redo")));

        QJsonArray failing = adds;
        failing.append(QJsonObject{{"tool", "edit_component"}, {"arguments", QJsonObject{{"name", "NOPE"}, {"properties", QJsonObject{{"R", "1"}}}}}});
        QJsonObject atomic, behind;
        control->callToolFor(1, "batch", {{"calls", failing}, {"atomic", true}}, [&](const QJsonObject& r) { atomic = r; });
        control->callToolFor(2, "add_component", {{"type", "C"}, {"name", "C_BEHIND"}, {"x", 300}, {"y", 300}},
                             [&](const QJsonObject& r) { behind = r; });
        QTRY_VERIFY_WITH_TIMEOUT(!atomic.isEmpty() && !behind.isEmpty(), 10000);
        QVERIFY(failed(atomic));
        QVERIFY2(!text(atomic).contains(" -  as it was"), qPrintable(text(atomic)));   // (an untitled one was named "")
        QVERIFY(sch->getComponentByName("C_BEHIND") != nullptr);
        QVERIFY(sch->getComponentByName("RB1") == nullptr);

        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* second = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "RX"}, {"x", 100}, {"y", 100}})));
        app->showDocument(sch);
        QJsonObject stayed;
        control->callToolFor(1, "batch", {{"calls", adds}}, [&](const QJsonObject& r) { stayed = r; });
        QTimer::singleShot(0, this, [&] { app->showDocument(second); });   // (the user's click, between its calls)
        QTRY_VERIFY_WITH_TIMEOUT(!stayed.isEmpty(), 10000);
        QVERIFY2(!failed(stayed), qPrintable(text(stayed)));
        for (int i = 1; i <= 3; ++i) QVERIFY(sch->getComponentByName(QStringLiteral("RB%1").arg(i)) != nullptr);
        QVERIFY(second->getComponentByName("RB3") == nullptr);
        discardAll();
    }

    // A4 (and E5): the JSON form set back makes the netlist it was read
    // from - a part mirrored and turned the right way round (a source's
    // polarity was reversed), an equation-defined device's branches, a
    // sweep's records, a subcircuit's parameters, a library part, a
    // label's initial value, a .MODEL's extra lines, a list sweep, a
    // transformer and a transient both named Tr1/TR1.
    void theJsonFormMakesTheSameNetlist()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Vac"}, {"name", "V1"}, {"x", 100}, {"y", 100}, {"rotation", 1}, {"mirror", true}})));
        const QJsonObject v1 = componentIn(json(call("get_schematic")).toObject(), "V1");
        QCOMPARE(v1.value("rotation").toInt(), 1);
        QVERIFY(v1.value("mirrored").toBool());
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        const auto normalized = [](const QString& netlist) {
            QStringList out;
            for (QString line : netlist.split('\n')) {
                line = line.trimmed();
                if (line.isEmpty() || line.startsWith('*') || line.startsWith("The netlist of") || line.contains("spice4qucs")
                    || line.startsWith(".INCLUDE", Qt::CaseInsensitive))
                    continue;
                out << line.simplified();
            }
            out.sort();
            return out;
        };
        const QStringList examples{
            "ngspice/RF/Miscellaneous/RCL_resonance.sch", "ngspice/Power Electronics/preregulator.sch",
            "ngspice/Devices/Tunnel_Diode_EDD.sch", "ngspice/NGspice features/RC_lowpass_ngsweep.sch",
            "ngspice/Devices/XTAL/quarz_test.sch", "ngspice/RF/RFLumpComp/Test_chip_res_basic.sch",
            "ngspice/General Electronics/Waveform Generation/sawtooth-2.sch", "xyce/Xyce_Examples/11-SParameters/MRF501.sch",
            "templates_ngspice/Pwr-Amp_wingspread_analysis.sch", "qucsator/RF/Mixers/mixer.sch",
            "ngspice/NGspice features/LC_lowpass_ngopt.sch"};
        int k = 0;
        for (const QString& example : examples) {
            // (A copy of its folder: its subcircuits and libraries beside it.)
            const QString source = QStringLiteral(QUCS_EXAMPLES_DIR) + "/" + example;
            if (!QFileInfo::exists(source)) QSKIP(qPrintable("no example " + example));
            const QString folder = dir.filePath(QStringLiteral("roundtrip/%1").arg(++k));
            QDir().mkpath(folder);
            for (const QFileInfo& f : QDir(QFileInfo(source).absolutePath()).entryInfoList(QDir::Files))
                QFile::copy(f.absoluteFilePath(), folder + "/" + f.fileName());
            const QString file = folder + "/" + QFileInfo(source).fileName();
            QVERIFY2(!failed(call("open_document", {{"path", file}})), qPrintable(example));
            const QJsonObject netlisted = call("get_netlist");
            QVERIFY2(!failed(netlisted), qPrintable(example + ": " + text(netlisted)));
            const QJsonObject form = json(call("get_schematic", {{"format", "json"}})).toObject();
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            QVERIFY(!failed(call("save_document", {{"as", folder + "/copy.sch"}})));
            const QJsonObject set = call("set_schematic", {{"components", form.value("components")}, {"wires", form.value("wires")}});
            QVERIFY2(!failed(set), qPrintable(example + ": " + text(set)));
            const QJsonObject again = call("get_netlist");
            QVERIFY2(!failed(again), qPrintable(example + ": " + text(again)));
            QCOMPARE(normalized(text(again)), normalized(text(netlisted)));
            discardAll();
        }
    }

    // A5: a preview, and an atomic script that failed, leave the redo steps.
    void aPreviewKeepsWhatRedoWouldDo()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("undo")));
        QVERIFY(sch->getComponentByName("R2") == nullptr);
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"x", 100}, {"y", 300}, {"preview", true}})));
        QVERIFY(!failed(call("redo")));
        QVERIFY(sch->getComponentByName("R2") != nullptr);
        if (QucsControl::scriptingBuilt()) {
            QVERIFY(!failed(call("undo")));
            QVERIFY(failed(call("run_script", {{"script", "throw new Error('no')"}, {"atomic", true}})));
            QVERIFY2(!failed(call("redo")), "a failed script left redo");
            QVERIFY(sch->getComponentByName("R2") != nullptr);
        }
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A6, A8: an import keeps the netlist's nodes - their names know no
    // case, a name made safe does not join another node, a first line of
    // "; comment" is the title (not the first element), two elements of
    // one name are both placed, apart - and each import's subcircuits have
    // a file of their own.
    void aNetlistImportKeepsItsNodes()
    {
        const auto netOf = [this](const QString& part, int pin) {
            const QJsonObject c = componentIn(json(call("get_schematic")).toObject(), part);
            for (const QJsonValue& p : c.value("pins").toArray())
                if (p.toObject().value("pin").toInt() == pin) return p.toObject().value("net").toString();
            return QString();
        };
        QJsonObject r = call("import_netlist", {{"text", "case\nr1 In Out 1K\nc1 out 0 1U\nv1 in 0 1\n.end\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(netOf("c1", 1), netOf("r1", 2));
        QCOMPARE(netOf("v1", 1), netOf("r1", 1));
        r = call("import_netlist", {{"text", "safe\nv1 n+1 0 1\nv2 n_1 0 2\nr1 n+1 n_1 1k\n.end\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(netOf("v1", 1) != netOf("v2", 1));
        r = call("import_netlist", {{"text", "; a comment first\nR1 a 0 1k\nR2 a 0 2k\n.end\n"}});
        QVERIFY2(json(r).toObject().value("parts").toArray().contains("R1"), qPrintable(text(r)));
        r = call("import_netlist", {{"text", "twice\nR1 a 0 1k\nR1 a 0 2k\n.end\n"}});
        QVERIFY2(json(r).toObject().value("parts").toArray().contains("R1_2"), qPrintable(text(r)));
        QVERIFY2(text(r).contains("a second element of that name"), qPrintable(text(r)));
        const QJsonObject first = json(call("import_netlist", {{"text", "same\n.subckt blk a b\nR1 a b 1k\n.ends\nX1 1 0 blk\n.end\n"}})).toObject();
        const QJsonObject later = json(call("import_netlist", {{"text", "same\n.subckt blk a b\nC1 a b 1n\n.ends\nX1 1 0 blk\n.end\n"}})).toObject();
        QVERIFY(!first.value("subcircuits").toString().isEmpty());
        QVERIFY(first.value("subcircuits").toString() != later.value("subcircuits").toString());
        QVERIFY(readFile(first.value("subcircuits").toString()).contains("R1 a b 1k"));
        discardAll();
    }

    // A7: keep_as does not write over another schematic's dataset.
    void keepAsIsNotAnotherSchematicsDataset()
    {
        writeFile("workspace/rc9.sch", schematicOf(""));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"x", 100}, {"y", 300}})));
        QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/keeper.sch")}})));
        const QJsonObject r = call("simulate", {{"keep_as", "rc9"}, {"timeout", 5}});
        QVERIFY2(failed(r) && text(r).contains("rc9.sch"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A10 (and F): a Save As that fails keeps the document's name - every
    // save after failed too; a schematic is not saved as x.txt.
    void aFailedSaveAsKeepsItsName()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        const QString file = dir.filePath("workspace/keepname.sch");
        QVERIFY(!failed(call("save_document", {{"as", file}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 300}, {"y", 100}})));
        QJsonObject r = call("save_document", {{"as", dir.filePath("workspace/keepname.txt")}});
        QVERIFY2(failed(r) && text(r).contains(".sch"), qPrintable(text(r)));
#ifndef Q_OS_WIN
        const QString locked = dir.filePath("locked");
        QDir().mkpath(locked);
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        if (!QFileInfo(locked).isWritable()) {   // (not as root)
            r = call("save_document", {{"as", locked + "/x.sch"}});
            QVERIFY(failed(r));
            QCOMPARE(front()->getDocName(), file);
            QVERIFY(!failed(call("save_document")));
            QVERIFY(readFile(file).contains("R2"));
        }
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A11: one file is one document, however its path is spelt.
    void oneFileIsOneDocument()
    {
#ifdef Q_OS_WIN
        QSKIP("symbolic links");
#else
        const QString file = writeFile("twice/tn.sch", schematicOf("  <R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n"));
        QFile::remove(dir.filePath("twicelink"));
        QVERIFY(QFile::link(dir.filePath("twice"), dir.filePath("twicelink")));
        QVERIFY(!failed(call("open_document", {{"path", file}})));
        const qsizetype docs = app->allDocuments().size();
        QVERIFY(!failed(call("open_document", {{"path", dir.filePath("twicelink/tn.sch")}})));
        QCOMPARE(app->allDocuments().size(), docs);
        const QString upper = dir.filePath("twice/TN.SCH");
        if (QFileInfo::exists(upper)) {   // (a disk that ignores case)
            QVERIFY(!failed(call("open_document", {{"path", upper}})));
            QCOMPARE(app->allDocuments().size(), docs);
        }
        QVERIFY(!failed(call("close_document")));
#endif
    }

    // A12: a file that can be written but not read is not written over:
    // taken for none, it was deleted when put back.
    void anUnreadableFileIsNotWrittenOver()
    {
#ifdef Q_OS_WIN
        QSKIP("file modes");
#else
        writeFile("wo/top.sch", schematicOf("  <R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n  <R R2 1 200 100 15 -26 0 1 \"1k\" 1>\n",
                                            "  <100 130 200 130 \"\" 0 0 0 \"\">\n"));
        const QString sub = writeFile("wo/wosub.sch", "keep");
        QFile::setPermissions(sub, QFileDevice::WriteOwner);
        if (QFile f(sub); f.open(QIODevice::ReadOnly)) {
            QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            QSKIP("every file is readable here (root)");
        }
        QVERIFY(!failed(call("open_document", {{"path", dir.filePath("wo/top.sch")}})));
        const QJsonObject r = call("create_subcircuit", {{"names", QJsonArray{"R1"}}, {"save_as", "wosub.sch"}, {"replace", true}, {"preview", true}});
        QVERIFY2(text(r).contains("cannot be read"), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(sub));
        QFile::setPermissions(sub, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QCOMPARE(readFile(sub), QByteArray("keep"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
#endif
    }

    // B3: arrange takes time in proportion to the parts, not their square:
    // four times the parts, about four times the time (a minute at 3,000
    // parts before, sixteen times the time for four times the parts) -
    // however fast the machine, and each net as it was.
    void aLargeSchematicIsArrangedInTime()
    {
        const auto arranged = [this](int count) -> qint64 {
            QByteArray parts, wires;
            for (int i = 0; i < count; ++i) {
                const int x = 100 * (i % 50), y = 100 * (i / 50);
                parts += QStringLiteral("  <R R%1 1 %2 %3 15 -26 0 0 \"1k\" 1>\n").arg(i + 1).arg(x).arg(y).toUtf8();
                if (i % 50 != 49 && i % 10 != 9) wires += QStringLiteral("  <%1 %2 %3 %2 \"\" 0 0 0 \"\">\n").arg(x + 30).arg(y).arg(x + 70).toUtf8();
            }
            if (failed(call("open_document", {{"path", writeFile(QStringLiteral("workspace/big%1.sch").arg(count), schematicOf(parts, wires))}})))
                return -1;
            QElapsedTimer clock;
            clock.start();
            const QJsonObject r = call("arrange", {}, 300000);
            const qint64 took = clock.elapsed();
            const bool kept = !failed(r) && text(r).contains("every net as it was");
            if (!kept) qWarning().noquote() << text(r).left(400);
            call("close_document", {{"unsaved", "discard"}});
            return kept ? took : -1;
        };
        const qint64 small = arranged(800), large = arranged(3200);
        QVERIFY(small >= 0 && large >= 0);
        QVERIFY2(large < 8 * std::max<qint64>(small, 20), qPrintable(QStringLiteral("800 parts: %1 ms, 3200: %2 ms").arg(small).arg(large)));
        // A long chain with a ground at every stage (an RLC ladder): each
        // ground placed against every part - their texts measured each time -
        // was quadratic, 24 s at 2,000 parts. Read whole, it lists the first
        // 50 of each kind, not its 2,500 labels.
        const auto ladder = [this](int stages, QString* read) -> qint64 {
            QStringList lines{"* ladder", "V1 n0 0 DC 0 AC 1"};
            for (int i = 1; i <= stages; ++i)
                lines << QStringLiteral("R%1 n%2 m%1 10").arg(i).arg(i - 1) << QStringLiteral("L%1 m%1 n%1 1u").arg(i)
                      << QStringLiteral("C%1 n%1 0 1n").arg(i);
            lines << ".tran 1n 50u" << ".end";
            if (failed(call("import_netlist", {{"text", lines.join('\n')}}))) return -1;
            QElapsedTimer clock;
            clock.start();
            const QJsonObject r = call("arrange", {}, 300000);
            const qint64 took = clock.elapsed();
            const bool kept = !failed(r) && text(r).contains("every net as it was");
            if (!kept) qWarning().noquote() << text(r).left(400);
            if (read != nullptr) *read = text(call("get_schematic"));
            call("close_document", {{"unsaved", "discard"}});
            return kept ? took : -1;
        };
        QString whole;
        const qint64 short_ = ladder(100, nullptr), long_ = ladder(400, &whole);
        QVERIFY(short_ >= 0 && long_ >= 0);
        QVERIFY2(long_ < 8 * std::max<qint64>(short_, 20), qPrintable(QStringLiteral("100 stages: %1 ms, 400: %2 ms").arg(short_).arg(long_)));
        const QJsonObject read = QJsonDocument::fromJson(whole.toUtf8()).object();
        QCOMPARE(read.value("components").toArray().size(), 50);
        QVERIFY(!failed(call("import_netlist", {{"text", "* rc\nV1 a 0 DC 1\nR1 a b 1k\nR2 b c 1k\nR3 c d 1k\nR4 d 0 1k\n.end"}})));
        const QJsonArray some = json(call("get_schematic", {{"components", QJsonArray{"R4"}}})).toObject().value("labels").toArray();
        QVERIFY2(some.size() == 1 && some.first().toObject().value("net").toString() == "d", "a read of R4: the labels on its pins alone");
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY2(read.value("labels").toArray().size() <= 50 && read.value("left out").toString().contains("'region'"), qPrintable(whole.left(300)));
    }

    // arrange lays a schematic out the same way every time. A simulation
    // block measured itself anew when it was first painted - other bounds
    // (the air around its label) and its text placed again - so the blocks
    // went a step one way or the other as the window painted before arrange
    // or after; and among equal ways the closest was taken in a hash's
    // order, which is another in every process. 66 of the 251 examples
    // came out otherwise in two runs.
    void arrangeIsTheSameEveryTime()
    {
        // A block's bounds and text place: as loaded, as painted, on any
        // device - not the file's text place, nor the painter's measure.
        std::unique_ptr<Component> block(qucs_s::control::newComponent(".TR"));
        QVERIFY(block != nullptr);
        QVERIFY(block->load("<.TR TR1 1 100 480 0 65 0 0 \"lin\" 1 \"0\" 1 \"1 ms\" 1 \"11\" 0>"));
        const QRect loaded = block->boundingRectIncludingProperties();
        const int ty = block->ty;
        QVERIFY(ty != 65);
        {
            QImage image(600, 600, QImage::Format_ARGB32);
            image.setDotsPerMeterX(5000);   // (a device of its own resolution: a picture written)
            image.setDotsPerMeterY(5000);
            QPainter painter(&image);
            block->paint(&painter);
        }
        QCOMPARE(block->boundingRectIncludingProperties(), loaded);
        QCOMPARE(block->ty, ty);
        // The layout: painted first or not, and whatever the hashes' order.
        for (const QString& example : {QStringLiteral("ngspice/General Electronics/chargepump.sch"),
                                       QStringLiteral("ngspice/General Electronics/schmitt.sch"),
                                       QStringLiteral("ngspice/NGspice features/RC_lowpass_ngsweep.sch"),
                                       QStringLiteral("ngspice/RF/Oscillators/sym_osci.sch"), QStringLiteral("ngspice/RF/Mixers/gilbert.sch"),
                                       QStringLiteral("ngspice/RF/Oscillators/rf_osci.sch")}) {
            const QString file = QStringLiteral(QUCS_EXAMPLES_DIR "/") + example;
            const auto arranged = [&](bool paint) {
                if (failed(call("open_document", {{"path", file}}))) return QString();
                if (paint) front()->viewport()->grab();
                const QJsonObject r = call("arrange");
                // (Its elements: not the view it was scrolled to, <View=...>.)
                const QString laid = failed(r) ? text(r) : text(call("get_schematic", {{"format", "text"}})).section("</Properties>", 1);
                call("close_document", {{"unsaved", "discard"}});
                return laid;
            };
            const QString first = arranged(false);
            QVERIFY(first.contains("<Components>") && first.contains("<Wires>"));
            QVERIFY2(arranged(true) == first, qPrintable(example + ": another once painted"));
            for (int k = 0; k < 6; ++k) {
                QHashSeed::resetRandomGlobalSeed();
                QVERIFY2(arranged(k % 2 == 1) == first, qPrintable(example + ": another with other hashes"));
            }
        }
    }

    // B4, E7: no message box waits on a call (under --mcp-server no one
    // answers it): one opened in it is closed with its safe button, what it
    // said in the answer; what the window says in a box - a read-only
    // file, a newer version's, a data display that cannot be made - a
    // tool says in its answer.
    void noMessageBoxWaitsOnACall()
    {
        {
            misc::ErrorCapture capture;
            QElapsedTimer clock;
            clock.start();
            const auto answer = QMessageBox::question(nullptr, "Q", "Go on with it?", QMessageBox::Yes | QMessageBox::No);
            QCOMPARE(answer, QMessageBox::No);
            QVERIFY(clock.elapsed() < 5000);
            QVERIFY2(capture.errors().join(' ').contains("Go on with it?"), qPrintable(capture.errors().join(' ')));
        }
        const QString future = writeFile("workspace/v99.sch", QByteArray(schematicOf("  <R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n"))
                                                                   .replace(PACKAGE_VERSION, "99.0.0"));
        QJsonObject r = call("open_document", {{"path", future}});
        QVERIFY2(!failed(r) && text(r).contains("99.0.0"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document")));
#ifndef Q_OS_WIN
        const QString readOnly = writeFile("rofolder/ro.sch", schematicOf("  <R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n"));
        QFile::setPermissions(readOnly, QFileDevice::ReadOwner);
        QFile::setPermissions(dir.filePath("rofolder"), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        if (!QFileInfo(readOnly).isWritable()) {   // (not as root)
            r = call("open_document", {{"path", readOnly}});
            QVERIFY2(!failed(r) && text(r).contains("read-only"), qPrintable(text(r)));
            const qsizetype docs = app->allDocuments().size();
            r = call("new_document", {{"kind", "data_display"}});
            QVERIFY2(failed(r) && text(r).contains("Cannot create"), qPrintable(text(r)));
            QCOMPARE(app->allDocuments().size(), docs);   // (no tab of no file left)
            QVERIFY(!failed(call("close_document")));
        }
        QFile::setPermissions(dir.filePath("rofolder"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
    }

    // C1-C4: names as the dialogs take them - no net called 0, gnd or
    // net2, none with spaces or ; and no part named "R 9", 9R or R9.2 -
    // and a subcircuit of grounds alone is refused (the circuit lost its
    // ground).
    void namesAreCheckedAsTheDialogsCheckThem()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 300}, {"y", 200}})));
        for (const char* bad : {"0", "GND", "gnd", "a 0", "net2", "x;y", "n$1", "a=b", "9a"}) {
            const QJsonObject r = call("set_label", {{"at", "R1.1"}, {"name", bad}});
            QVERIFY2(failed(r), bad);
        }
        QVERIFY(!failed(call("set_label", {{"at", "R1.1"}, {"name", "out"}})));
        for (const char* bad : {"GND", "net5", "o ut"}) QVERIFY2(failed(call("rename_net", {{"from", "out"}, {"to", bad}})), bad);
        QVERIFY(!failed(call("rename_net", {{"from", "out"}, {"to", "out2"}})));
        QJsonObject r = call("rename_net", {{"from", "gnd"}, {"to", "x1"}});
        QVERIFY2(failed(r) && text(r).contains("is ground"), qPrintable(text(r)));
        for (const char* bad : {"R 9", "9R", "R9.2", "R;9", "R$9"}) {
            QVERIFY2(failed(call("edit_component", {{"name", "R1"}, {"rename", bad}})), bad);
            QVERIFY2(failed(call("add_component", {{"type", "R"}, {"name", bad}, {"x", 500}, {"y", 100}})), bad);
        }
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"rename", "R9"}})));
        QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/names.sch")}})));
        r = call("create_subcircuit", {{"names", QJsonArray{"GND"}}, {"save_as", "gonly.sch"}});
        QVERIFY2(failed(r) && text(r).contains("grounds alone"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(dir.filePath("workspace/gonly.sch")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // D1: real units are no typos (Ohms, Volts, dBV, degC, V/us), 1kk
    // still is; a text that changes no part checks none.
    void realUnitsAreNoTypos()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        int n = 0;
        for (const char* value : {"1 kOhms", "5 Volts", "2 Amps", "1 Hertz", "10 dBV", "3 dBc", "25 degC", "1.5 V/us", "4.7k", "10 uF"}) {
            const QJsonObject r = call("add_component", {{"type", "R"}, {"x", 100 * ++n}, {"y", 100}, {"properties", QJsonObject{{"R", value}}}});
            QVERIFY2(!failed(r), qPrintable(QString(value) + ": " + text(r)));
        }
        QVERIFY(failed(call("add_component", {{"type", "R"}, {"x", 100}, {"y", 300}, {"properties", QJsonObject{{"R", "1kk"}}}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        const QString legacy = writeFile("workspace/legacy.sch", schematicOf("  <R R1 1 100 100 15 -26 0 1 \"1 kQx\" 1>\n"));
        QVERIFY(!failed(call("open_document", {{"path", legacy}})));
        QJsonObject r = call("set_schematic", {{"text", "<Paintings>\n</Paintings>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // Its parts given again, the old value kept and a new part added:
        // the new part's value checked, not the kept one.
        r = call("set_schematic", {{"text", "<Components>\n  <R R1 1 100 100 15 -26 0 1 \"1 kQx\" 1>\n"
                                            "  <R R2 1 300 100 15 -26 0 1 \"2k\" 1>\n</Components>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("set_schematic", {{"text", "<Components>\n  <R R1 1 100 100 15 -26 0 1 \"1 kQx\" 1>\n"
                                            "  <R R2 1 300 100 15 -26 0 1 \"2kk\" 1>\n</Components>\n"}});
        QVERIFY2(failed(r) && text(r).contains("2kk") && !text(r).contains("kQx"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // D2: a file dated in the future is not loaded again after it opens.
    void aFileFromTheFutureIsNotLoadedAgain()
    {
        const QString file = writeFile("workspace/future.sch", schematicOf("  <R R1 1 100 100 15 -26 0 1 \"1k\" 1>\n"));
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(2), QFileDevice::FileModificationTime));
        }
        QVERIFY(!failed(call("open_document", {{"path", file}})));
        const quint64 revision = front()->revision();
        QTest::qWait(3000);
        QCOMPARE(front()->revision(), revision);
        QVERIFY(!failed(call("close_document")));
    }

    // D3, D4, D5: an expression traced in a diagram is a NutmegEq's
    // variable (it was a dead trace); one that does not read is refused
    // before anything is added; a formula's field is 'tex'.
    void anExpressionTraceIsComputed()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", ".AC"}, {"name", "AC1"}, {"x", 100}, {"y", 300}})));
        QJsonObject r = call("add_diagram", {{"x", 450}, {"y", 400}, {"traces", QJsonArray{"ac.db(v(out))"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(text(r).contains("ngspice/ac.db_v_out"), qPrintable(text(r)));
        Component* nutmeg = nullptr;
        for (Component* c : sch->a_DocComps)
            if (c->Model == "NutmegEq") nutmeg = c;
        QVERIFY(nutmeg != nullptr);
        QCOMPARE(nutmeg->getProperty("Simulation")->Value, QStringLiteral("AC1"));
        QVERIFY(nutmeg->getProperty("db_v_out") != nullptr);
        r = call("add_trace", {{"diagram", 1}, {"variable", "v(out)*2"}});
        QVERIFY2(!failed(r) && text(r).contains("ngspice/ac.v_out_2"), qPrintable(text(r)));
        int blocks = 0;   // (one NutmegEq for the analysis, not one an expression)
        for (Component* c : sch->a_DocComps) blocks += c->Model == "NutmegEq";
        QCOMPARE(blocks, 1);
        QVERIFY(nutmeg->getProperty("v_out_2") != nullptr);
        r = call("add_trace", {{"diagram", 1}, {"variable", "ac.db(v(out))"}});   // (the same: there already)
        QVERIFY2(failed(r) && text(r).contains("already"), qPrintable(text(r)));
        QVERIFY(failed(call("add_trace", {{"diagram", 1}, {"variable", "tran.v(out)*2"}})));
        r = call("add_trace", {{"diagram", 1}, {"variable", "v(out) +"}});
        QVERIFY2(failed(r) && text(r).contains("does not read"), qPrintable(text(r)));
        const size_t parts = sch->a_DocComps.size();
        r = call("add_analysis", {{"kind", "tran"}, {"plot", QJsonArray{"x=1"}}});
        QVERIFY2(failed(r) && text(r).contains("Nothing was added"), qPrintable(text(r)));
        QCOMPARE(sch->a_DocComps.size(), parts);
        const int simulator = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        r = call("add_trace", {{"diagram", 1}, {"variable", "v(out)/2"}});
        QucsSettings.DefaultSimulator = simulator;
        QVERIFY2(failed(r) && text(r).contains("Eqn"), qPrintable(text(r)));
        const QJsonObject painting = json(call("describe_tool", {{"name", "add_painting"}})).toObject();
        QVERIFY(painting.value("inputSchema").toObject().value("properties").toObject().contains("tex"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // E1, E4, E6: simulate says what is wrong - no analysis; its schematic
    // closed before it began - and diff's steps are checked before use.
    void simulateSaysWhatIsWrong()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QJsonObject r = call("simulate", {{"timeout", 5}});
        QVERIFY2(failed(r) && text(r).contains("no analysis"), qPrintable(text(r)));
        QVERIFY(front()->getDocName().isEmpty());   // (not saved for nothing)
        r = call("diff", {{"steps", double(std::numeric_limits<int>::min())}});
        QVERIFY2(failed(r) && text(r).contains("'steps'"), qPrintable(text(r)));
        QVERIFY(!failed(call("add_component", {{"type", ".TR"}, {"x", 100}, {"y", 300}})));
        QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/closing.sch")}})));
        QJsonObject simulated, closed;
        control->callToolFor(1, "simulate", {{"timeout", 5}}, [&](const QJsonObject& r) { simulated = r; });
        control->callToolFor(1, "close_document", {{"unsaved", "discard"}}, [&](const QJsonObject& r) { closed = r; });
        QTRY_VERIFY_WITH_TIMEOUT(!simulated.isEmpty() && !closed.isEmpty(), 20000);
        QVERIFY2(text(simulated).contains("was closed"), qPrintable(text(simulated)));
    }

    // E7: a dataset that cannot be written is an error of the run, in its
    // answer - it was a message box, and "dataset written": true.
    void aDatasetThatCannotBeWrittenIsSaid()
    {
#ifdef Q_OS_WIN
        QSKIP("file modes");
#else
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("set_schematic", {{"components", QJsonArray{
            QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}}, QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 150}},
            QJsonObject{{"type", "GND"}, {"x", 100}, {"y", 260}}, QJsonObject{{"type", ".TR"}, {"name", "TR1"}, {"x", 300}, {"y", 300}}}}})));
        QVERIFY(!failed(call("connect", {{"from", "V1.1"}, {"to", "R1.1"}})));
        QVERIFY(!failed(call("connect", {{"from", "V1.2"}, {"to", "GND.1"}})));
        QVERIFY(!failed(call("connect", {{"from", "R1.2"}, {"to", "GND.1"}})));
        QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/rods.sch")}})));
        const QString dataset = writeFile("workspace/rods.dat.ngspice", "{}");
        QFile::setPermissions(dataset, QFileDevice::ReadOwner);
        if (QFile f(dataset); f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            QFile::setPermissions(dataset, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            QucsSettings.NgspiceExecutable = before;
            QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
            QSKIP("every file is writable here (root)");
        }
        const QJsonObject r = call("simulate", {{"timeout", 30}}, 60000);
        QFile::setPermissions(dataset, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QucsSettings.NgspiceExecutable = before;
        const QJsonObject report = json(r).toObject();
        QVERIFY2(!report.value("dataset written").toBool(), qPrintable(text(r)));
        QVERIFY2(QJsonDocument(report.value("errors").toArray()).toJson().contains("dataset"), qPrintable(text(r)));
        QVERIFY(!report.value("succeeded").toBool());
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
#endif
    }

    // E5: a library part is placed as find_library_component says: type Lib.
    void aLibraryPartIsPlaced()
    {
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/Diodes.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        QVERIFY2(!failed(call("describe_component_type", {{"type", "Lib"}})), "Lib is a type");
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        const QJsonObject r = call("add_component", {{"type", "Lib"}, {"name", "D1"}, {"x", 100}, {"y", 100},
                                                     {"properties", QJsonObject{{"Lib", "Diodes"}, {"Comp", "1N4148"}}}});
        // One not there: refused, not a part with no pins.
        const QJsonObject missing = call("add_component", {{"type", "Lib"}, {"x", 300}, {"y", 100},
                                                           {"properties", QJsonObject{{"Lib", "Diodes"}, {"Comp", "NoSuchDiode"}}}});
        QucsSettings.LibDir = was;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(front()->getComponentByName("D1") != nullptr);
        QVERIFY(front()->getComponentByName("D1")->Ports.size() == 2);
        QVERIFY2(failed(missing) && text(missing).contains("NoSuchDiode"), qPrintable(text(missing)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Layout beyond arrange (sixth round, wishlist 1): a part put beside
    // another ('near'); a wire round a side ('side') or through points of
    // one's own ('via'); a label's text put where one wants it; arrange
    // keeping the parts where they are and drawing the wiring again, an
    // op-amp's feedback part below it, the supplies as labels.
    void theLayoutHelpers()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "OpAmp"}, {"name", "U1"}, {"x", 400}, {"y", 200}})));
        // Beside another part: its symbol that far from the other's, centred on it.
        QJsonObject r = call("add_component", {{"type", "R"}, {"name", "RF"}, {"near", QJsonObject{{"part", "U1"}, {"side", "below"}, {"gap", 40}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const Component* u1 = front()->getComponentByName("U1");
        const Component* rf = front()->getComponentByName("RF");
        QVERIFY(rf->boundingRect().top() >= u1->boundingRect().bottom() + 40 - 10 && rf->boundingRect().top() <= u1->boundingRect().bottom() + 40 + 10);
        QVERIFY(std::abs(rf->boundingRect().center().x() - u1->boundingRect().center().x()) <= 10);
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "RG"}, {"x", 100}, {"y", 100}})));
        r = call("edit_component", {{"name", "RG"}, {"near", QJsonObject{{"part", "U1"}, {"side", "left"}, {"gap", 60}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const Component* rg = front()->getComponentByName("RG");
        QVERIFY(rg->boundingRect().right() <= u1->boundingRect().left() - 60 + 10 && rg->boundingRect().right() >= u1->boundingRect().left() - 60 - 10);
        QVERIFY(failed(call("add_component", {{"type", "R"}, {"x", 0}, {"y", 0}, {"near", QJsonObject{{"part", "U1"}, {"side", "below"}}}})));
        QVERIFY(failed(call("edit_component", {{"name", "RF"}, {"near", QJsonObject{{"part", "RF"}, {"side", "below"}}}})));
        QVERIFY(failed(call("edit_component", {{"name", "RF"}, {"near", QJsonObject{{"part", "U1"}, {"side", "under"}}}})));

        // A wire round a side: the feedback path below the op-amp.
        r = call("connect", {{"from", "U1.3"}, {"to", "RF.2"}, {"side", "below"}});
        QVERIFY2(!failed(r) && text(r).contains("Round the lower side"), qPrintable(text(r)));
        r = call("connect", {{"from", "U1.1"}, {"to", "RF.1"}, {"side", "left"}});
        QVERIFY2(!failed(r) && text(r).contains("Round the left side"), qPrintable(text(r)));
        // Through points of one's own; not through another net.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R9"}, {"x", 100}, {"y", 400}})));
        r = call("connect", {{"from", "R9.1"}, {"to", "RG.1"}, {"via", QJsonArray{QJsonArray{20, 400}, QJsonArray{20, 250}}}});
        QVERIFY2(!failed(r) && text(r).contains("Through 20, 400; 20, 250"), qPrintable(text(r)));
        bool through = false;
        for (const Wire* w : front()->a_DocWires) through = through || (w->P1() == QPoint(20, 250) || w->P2() == QPoint(20, 250));
        QVERIFY(through);
        const int wires = int(front()->a_DocWires.size());
        // (One point, built by append: QJsonArray{QJsonArray{20, 400}} is
        // [20, 400] to Apple's newer clang - a copy - and 'via' was refused
        // as no list of points, not as no way through them.)
        QJsonArray onePoint;
        onePoint.append(QJsonArray{20, 400});
        r = call("connect", {{"from", "R9.2"}, {"to", "U1.2"}, {"via", onePoint}});
        QVERIFY2(failed(r) && text(r).contains("Not wired through the points given"), qPrintable(text(r)));
        QCOMPARE(int(front()->a_DocWires.size()), wires);
        r = call("connect", {{"from", "R9.2"}, {"to", "U1.2"}, {"via", onePoint}, {"side", "below"}});
        QVERIFY2(failed(r) && text(r).contains("Give 'via' or 'side', not both"), qPrintable(text(r)));

        // A label's text where it is wanted; the same name again moves it.
        r = call("set_label", {{"at", "U1.3"}, {"name", "out"}, {"text_at", QJsonArray{500, 150}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const auto labelAt = [this](const QString& name) {
            for (const Node* n : front()->a_DocNodes)
                if (n->hasLabel() && n->label()->Name == name) return QPoint(n->label()->x1, n->label()->y1);
            for (const Wire* w : front()->a_DocWires)
                if (w->hasLabel() && w->label()->Name == name) return QPoint(w->label()->x1, w->label()->y1);
            return QPoint(-1, -1);
        };
        QCOMPARE(labelAt("out"), QPoint(500, 150));
        QVERIFY(!failed(call("set_label", {{"at", "U1.3"}, {"name", "out"}, {"text_at", QJsonArray{520, 250}}})));
        QCOMPARE(labelAt("out"), QPoint(520, 250));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // arrange: a non-inverting amplifier with its supplies.
        const auto amplifier = [this] {
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            const QJsonObject made = call("batch", {{"calls", QJsonArray{
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 300}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "OpAmp"}, {"name", "U1"}, {"x", 500}, {"y", 100}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RF"}, {"x", 800}, {"y", 400}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RG"}, {"x", 300}, {"y", 500}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RL"}, {"x", 900}, {"y", 100}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V2"}, {"x", 100}, {"y", 600},
                                                                                  {"properties", QJsonObject{{"U", "12 V"}}}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RP1"}, {"x", 600}, {"y", 600}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RP2"}, {"x", 700}, {"y", 700}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "U1.2"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "U1.1"}, {"to", "RG.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RG.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RF.1"}, {"to", "RG.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RF.2"}, {"to", "U1.3"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "U1.3"}, {"to", "RL.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RL.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V2.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V2.1"}, {"to", "RP1.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RP1.1"}, {"to", "RP2.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RP1.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RP2.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", ".DC"}, {"name", "DC1"}, {"x", 100}, {"y", 900}}}}}}});
            QVERIFY2(!failed(made), qPrintable(text(made)));
        };
        amplifier();
        const QList<QStringList> nets = netsOfFront();
        QHash<QString, QPoint> centres;
        for (const Component* c : front()->a_DocComps) centres.insert(c->Name, c->center());
        // Where they are, the wiring drawn again.
        r = call("arrange", {{"keep_places", true}});
        QVERIFY2(!failed(r) && text(r).contains("where they were"), qPrintable(text(r)));
        for (const Component* c : front()->a_DocComps)
            if (c->Model != "GND") QCOMPARE(c->center(), centres.value(c->Name));
        QCOMPARE(netsOfFront(), nets);
        QVERIFY(failed(call("arrange", {{"keep_places", true}, {"feedback", "below"}})));
        // The feedback part below the op-amp, in its column.
        r = call("arrange", {{"feedback", "below"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(netsOfFront(), nets);
        u1 = front()->getComponentByName("U1");
        rf = front()->getComponentByName("RF");
        QVERIFY2(rf->center().y() > u1->boundingRect().bottom() && std::abs(rf->center().x() - u1->boundingRect().center().x()) <= 20,
                 qPrintable(QStringLiteral("U1 %1,%2; RF %3,%4").arg(u1->cx).arg(u1->cy).arg(rf->cx).arg(rf->cy)));
        QCOMPARE(rf->Ports.at(0)->y, rf->Ports.at(1)->y);   // lying
        QVERIFY(rf->cx + rf->Ports.at(0)->x < rf->cx + rf->Ports.at(1)->x);   // its pin on U1's input side on the left
        r = call("arrange", {{"feedback", "above"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        u1 = front()->getComponentByName("U1");
        rf = front()->getComponentByName("RF");
        QVERIFY(rf->center().y() < u1->boundingRect().top());
        QCOMPARE(netsOfFront(), nets);
        // The supplies as labels: a VCC label on each pin of the supply's net
        // (three parts: a rail), a ground symbol on each pin on ground, no
        // wire of theirs.
        r = call("arrange", {{"supplies", "labels"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(netsOfFront(), nets);
        int vcc = 0, grounds = 0;
        for (const Node* n : front()->a_DocNodes) vcc += n->hasLabel() && n->label()->Name == "VCC";
        for (const Component* c : front()->a_DocComps) grounds += c->Model == "GND";
        QCOMPARE(vcc, 3);       // V2.1, RP1.1, RP2.1
        QCOMPARE(grounds, 6);   // V1.2, RG.2, RL.2, V2.2, RP1.2, RP2.2
        const Component* v2 = front()->getComponentByName("V2");
        const QPoint plus(v2->cx + v2->Ports.at(0)->x, v2->cy + v2->Ports.at(0)->y);
        for (const Wire* w : front()->a_DocWires) QVERIFY(w->P1() != plus && w->P2() != plus);
        QVERIFY(failed(call("arrange", {{"supplies", "sideways"}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Answers of the size asked for, and before and after in one call
    // (sixth round, wishlist 4 and 6): a batch's calls each in a line with
    // 'brief'; simulate's with 'brief' without its log and long lists; its
    // 'compare' measures this run and a kept one side by side.
    void briefAnswersAndBeforeAndAfter()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"brief", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vac"}, {"name", "V1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 220}, {"y", 100},
                                                                              {"properties", QJsonObject{{"R", "1k"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "C"}, {"name", "C1"}, {"x", 340}, {"y", 200}, {"rotation", 1},
                                                                              {"properties", QJsonObject{{"C", "1 uF"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", ".AC"}, {"name", "AC1"}, {"x", 100}, {"y", 400},
                                                                              {"properties", QJsonObject{{"Type", "log"}, {"Start", "1 Hz"}, {"Stop", "100 kHz"}, {"Points", "201"}}}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "C1.2"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "C1.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.2"}, {"name", "out"}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "NoSuchType"}, {"x", 0}, {"y", 0}}}}}},
                                       {"keep_going", true}});
        const QString said = text(r);
        QVERIFY2(said.contains("[2] add_component: name R1, type R, x 220, y 100"), qPrintable(said));
        QVERIFY2(said.contains("[5] connect: Wired V1.1 to R1.1"), qPrintable(said));
        QVERIFY2(!said.contains("\"properties\""), qPrintable(said));   // (not the whole answer)
        QVERIFY2(said.contains("[10] add_component failed:") && said.contains("NoSuchType"), qPrintable(said));   // (a failure in full)
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) {
            QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
            QSKIP("no ngspice here for the runs");
        }
        const QString was = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("save_document", {{"as", "before_after"}, {"replace", true}})));
        r = call("simulate", {{"keep_as", "rc_before"}, {"brief", true}, {"timeout", 60}}, 90000);
        QVERIFY2(!failed(r) && json(r).toObject().value("succeeded").toBool(), qPrintable(text(r)));
        QVERIFY2(!json(r).toObject().contains("last lines") && !json(r).toObject().contains("data display"), qPrintable(text(r)));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "2k"}}}})));
        r = call("simulate", {{"compare", QJsonObject{{"with", "rc_before"},
                                                      {"measure", QJsonArray{QJsonObject{{"variable", "ac.v(out)"}, {"what", "bandwidth"}},
                                                                             QJsonObject{{"variable", "ac.v(out)"}, {"what", "max"}}}}}},
                              {"timeout", 60}}, 90000);
        QucsSettings.NgspiceExecutable = was;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject compared = json(r).toObject().value("compared").toObject();
        QCOMPARE(compared.value("with").toString(), QStringLiteral("rc_before"));
        const QJsonObject bw = compared.value("table").toArray().at(0).toObject();
        QVERIFY2(std::abs(bw.value("before").toDouble() - 159.15) < 3 && std::abs(bw.value("after").toDouble() - 79.58) < 2
                     && std::abs(bw.value("change %").toDouble() + 50) < 2,
                 qPrintable(QJsonDocument(compared).toJson()));
        const QJsonObject max = compared.value("table").toArray().at(1).toObject();
        QVERIFY2(max.contains("before") && max.contains("after") && max.contains("change"), qPrintable(QJsonDocument(compared).toJson()));
        // Not read: said so, before anything runs.
        r = call("simulate", {{"compare", QJsonObject{{"with", "rc_before"}}}});
        QVERIFY2(failed(r) && text(r).contains("'compare' is"), qPrintable(text(r)));
        r = call("simulate", {{"compare", QJsonObject{{"with", "rc_before"}, {"measure", QJsonArray{QJsonObject{{"variable", "v(out)"}, {"wat", "max"}}}}}}});
        QVERIFY2(failed(r) && text(r).contains("Meant what?"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // tune with two knobs for two targets (sixth round, wishlist 5): a
    // chain of three resistors from 10 V, R1 and R2 for v(a) = 6 V and
    // v(b) = 2 V (R3 1k): R1 = R2 = 2k. Found in a few runs, set as one
    // step to undo; what does not read is said so.
    void twoKnobsAreTunedTogether()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200},
                                                                              {"properties", QJsonObject{{"U", "10 V"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 220}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R2"}, {"x", 360}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R3"}, {"x", 480}, {"y", 200}, {"rotation", 1}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", ".DC"}, {"name", "DC1"}, {"x", 100}, {"y", 400}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "R2.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R2.2"}, {"to", "R3.2"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R3.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.2"}, {"name", "a"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R2.2"}, {"name", "b"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", "two_knobs"}, {"replace", true}})));
        const int step = front()->undoIndex();
        r = call("tune", {{"knobs", QJsonArray{QJsonObject{{"component", "R1"}, {"range", QJsonArray{"100", "100k"}}},
                                               QJsonObject{{"component", "R2"}, {"range", QJsonArray{"100", "100k"}}}}},
                          {"targets", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "a"}}}, {"target", 6}},
                                                 QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}}}, {"target", 2}}}}},
                 600000);
        QucsSettings.NgspiceExecutable = before;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject o = json(r).toObject();
        for (const QJsonValue& t : o.value("targets").toArray()) QVERIFY2(t.toObject().value("within tolerance").toBool(), qPrintable(text(r)));
        QVERIFY2(o.value("runs").toArray().size() <= 12, qPrintable(text(r)));
        const auto valueOf = [this](const QString& part) {
            return front()->getComponentByName(part)->getProperty("R")->Value;
        };
        const auto near2k = [](const QString& v) {
            const qucs_s::units::Reading x = qucs_s::units::read(QString(v).remove(QStringLiteral(" Ohm")));
            return x.kind == qucs_s::units::Reading::Number && std::abs(x.value - 2000) < 30;
        };
        QVERIFY2(near2k(valueOf("R1")) && near2k(valueOf("R2")), qPrintable(valueOf("R1") + " " + valueOf("R2") + "\n" + text(r)));
        QCOMPARE(front()->undoIndex(), step + 1);   // one step
        QVERIFY(!failed(call("undo")));
        QCOMPARE(valueOf("R1"), QStringLiteral("1 kOhm"));
        // What does not read.
        r = call("tune", {{"knobs", QJsonArray{QJsonObject{{"component", "R1"}, {"range", QJsonArray{"100", "100k"}}}}},
                          {"targets", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "a"}}}, {"target", 6}}}}});
        QVERIFY2(failed(r) && text(r).contains("2 to 4 parts"), qPrintable(text(r)));
        r = call("tune", {{"knobs", QJsonArray{QJsonObject{{"component", "R1"}, {"range", QJsonArray{"100", "100k"}}},
                                               QJsonObject{{"component", "R2"}, {"range", QJsonArray{"100", "100k"}}}}},
                          {"targets", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "a"}}}, {"target", 6}},
                                                 QJsonObject{{"measure", QJsonObject{{"variable", "v(b)"}}}, {"target", 2}}}}});
        QVERIFY2(failed(r) && text(r).contains("all of the operating point"), qPrintable(text(r)));
        r = call("tune", {{"knobs", QJsonArray{QJsonObject{{"component", "R1"}, {"rnage", QJsonArray{"100", "100k"}}}}}});
        QVERIFY2(failed(r) && text(r).contains("Meant range?"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Undo that covers files (sixth round, wishlist 7): the files a call
    // wrote put back - one it made removed, one it wrote over as it was, an
    // open document on it loaded again; one changed since left and said;
    // a preview's writes are none of them.
    void filesAreUndone()
    {
        const QString a = dir.filePath("workspace/undo_files_a.sch");
        QFile::remove(a);
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", a}})));
        QVERIFY(QFileInfo::exists(a));
        QJsonObject r = call("undo", {{"files", true}});
        QVERIFY2(!failed(r) && !QFileInfo::exists(a), qPrintable(text(r)));
        QVERIFY(text(r).contains("removed (the call made them)"));
        QVERIFY(!failed(call("save_document", {{"as", a}})));
        QVERIFY(!failed(call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("save_document")));
        QVERIFY(readFile(a).contains("<C C1"));
        const QJsonObject history = json(call("undo_history")).toObject();
        QVERIFY2(history.value("files written").toArray().first().toObject().value("tool").toString() == "save_document",
                 qPrintable(QJsonDocument(history).toJson()));
        r = call("undo", {{"files", 1}});
        QVERIFY2(!failed(r) && text(r).contains("put back"), qPrintable(text(r)));
        QVERIFY(!readFile(a).contains("<C C1") && readFile(a).contains("<R R1"));
        QVERIFY(front()->getComponentByName("C1") == nullptr);   // (loaded again: it had no unsaved changes)
        // Changed since: left.
        const QString net = dir.filePath("workspace/undo_files.cir");
        QFile::remove(net);
        QVERIFY(!failed(call("export_netlist", {{"save_as", net}})));
        writeFile("workspace/undo_files.cir", "* edited by hand\n");
        r = call("undo", {{"files", 1}});
        QVERIFY2(!failed(r) && text(r).contains("changed since export_netlist") && QFileInfo::exists(net), qPrintable(text(r)));
        // A preview writes nothing that is kept; not with 'steps'; not redone.
        const int kept = int(json(call("undo_history")).toObject().value("files written").toArray().size());
        QVERIFY(!failed(call("create_subcircuit", {{"names", QJsonArray{"R1"}}, {"save_as", "undo_files_sub.sch"}, {"preview", true}})));
        QCOMPARE(int(json(call("undo_history")).toObject().value("files written").toArray().size()), kept);
        QVERIFY(failed(call("undo", {{"files", 1}, {"steps", 2}})));
        QVERIFY(failed(call("redo", {{"files", 1}})));
        // A subcircuit's file, and its schematic's change: both taken back.
        r = call("create_subcircuit", {{"names", QJsonArray{"R1"}}, {"save_as", "undo_files_sub.sch"}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString sub = dir.filePath("workspace/undo_files_sub.sch");
        QVERIFY(QFileInfo::exists(sub));
        QVERIFY(!failed(call("undo", {{"files", true}})));
        QVERIFY(!QFileInfo::exists(sub));
        QVERIFY(!failed(call("undo")));
        QVERIFY(front()->getComponentByName("R1") != nullptr);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Check Schematic on supplies (sixth round, wishlist 8): an op-amp whose
    // supply pins nothing powers - a warning; a DC source with its + on
    // ground on its VEE pin - as meant (supplySignsAreRead has the rest).
    void suppliesAreChecked()
    {
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "Lib"}, {"name", "U1"}, {"x", 300}, {"y", 200},
                                               {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(TI)"}}}});
        QucsSettings.LibDir = was;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("add_component", {{"type", ".DC"}, {"name", "DC1"}, {"x", 100}, {"y", 500}})));
        QString checked = text(call("check_schematic"));
        QVERIFY2(checked.contains("U1: its supply pins VCC, VEE are on nothing that powers it"), qPrintable(checked));
        // VEE from a source drawn + to ground: -15 V there - on U1's VEE
        // pin, as meant, nothing said (the eighth round: it was a note
        // every time); VCC still open.
        QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"name", "V3"}, {"x", 100}, {"y", 300}, {"properties", QJsonObject{{"U", "15 V"}}}})));
        QVERIFY(!failed(call("connect", {{"from", "V3.1"}, {"to", "ground"}})));
        QVERIFY(!failed(call("set_label", {{"at", "V3.2"}, {"name", "vee"}})));
        QVERIFY(!failed(call("set_label", {{"at", "U1.VEE"}, {"name", "vee"}})));
        checked = text(call("check_schematic"));
        QVERIFY2(checked.contains("U1: its supply pin VCC is on nothing that powers it") && !checked.contains("VCC, VEE"), qPrintable(checked));
        QVERIFY2(!checked.contains("V3: its + is on ground"), qPrintable(checked));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The sixth round's small things: redo 'to' a step behind is refused (it
    // undid thirteen steps); a misspelt field of tune's 'measure' is refused
    // with the one meant (waht measured the default); import_netlist's
    // save_as over a file there already is refused before anything is made,
    // and 'replace' writes over it; the netlist map names the pins that
    // have names.
    void theSixthRoundsSmallThings()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (int i = 1; i <= 3; ++i)
            QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", QStringLiteral("R%1").arg(i)}, {"x", 100 * i}, {"y", 100}})));
        const int at = front()->undoIndex();
        QJsonObject r = call("redo", {{"to", at - 2}});
        QVERIFY2(failed(r) && text(r).contains("redo goes forward only") && text(r).contains("undo with 'to'"), qPrintable(text(r)));
        QCOMPARE(front()->undoIndex(), at);
        QVERIFY(front()->getComponentByName("R3") != nullptr);
        QVERIFY(!failed(call("undo", {{"to", at - 2}})));   // (undo's goes either way)
        QVERIFY(!failed(call("undo", {{"to", at}})));
        QVERIFY(front()->getComponentByName("R3") != nullptr);

        r = call("tune", {{"component", "R1"}, {"values", QJsonArray{"1k"}},
                          {"measure", QJsonObject{{"variable", "ac.v(out)"}, {"waht", "max"}}}});
        QVERIFY2(failed(r) && text(r).contains("measure has no waht") && text(r).contains("Meant what?"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        const QString there = dir.filePath("workspace/imported_there.sch");
        writeFile("workspace/imported_there.sch", "<Qucs Schematic " PACKAGE_VERSION ">\n");
        const int documents = int(app->allDocuments().size());
        r = call("import_netlist", {{"text", "t\nR1 a 0 1k\n.end\n"}, {"save_as", there}});
        QVERIFY2(failed(r) && text(r).contains("'replace': true writes over it") && text(r).contains("Nothing was imported"), qPrintable(text(r)));
        QCOMPARE(int(app->allDocuments().size()), documents);
        r = call("import_netlist", {{"text", "t\nR1 a 0 1k\n.end\n"}, {"save_as", there}, {"replace", true}});
        QVERIFY2(!failed(r) && json(r).toObject().value("saved").toString() == QDir::toNativeSeparators(there), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"path", there}, {"unsaved", "discard"}})));

        // The map: a subcircuit's pins by number and name (its symbol's).
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, y] : {std::pair{"in", 100}, std::pair{"out", 200}})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}})));
        QVERIFY(!failed(call("save_document", {{"as", "named_pins.sch"}, {"replace", true}})));
        QVERIFY(!failed(call("make_symbol")));
        QVERIFY(!failed(call("save_document")));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Sub"}, {"name", "SUB1"}, {"x", 300}, {"y", 200},
                                                {"properties", QJsonObject{{"File", "named_pins.sch"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "RL"}, {"x", 100}, {"y", 400}})));
        QVERIFY(!failed(call("connect", {{"from", "SUB1.2"}, {"to", "RL.1"}})));
        QVERIFY(!failed(call("save_document", {{"as", "uses_named_pins.sch"}, {"replace", true}})));
        const QJsonObject map = json(call("get_netlist", {{"map", true}})).toObject();
        bool named = false;
        for (const QJsonValue& node : map.value("nodes").toObject())
            for (const QJsonValue& pin : node.toArray()) named = named || pin.toString() == "SUB1.2 (out)";
        QVERIFY2(named, qPrintable(QJsonDocument(map.value("nodes").toObject()).toJson()));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", "named_pins.sch"}, {"unsaved", "discard"}})));
    }

    // The OpAmps library's 741s under ngspice (fifth round, 7): the
    // transistor-level uA741's C="30 pF" was netlisted as 30 farads (the
    // value split at its space), and the macromodels' tail current source
    // IEE was written with its nodes in Qucs' order, not a schematic Idc's -
    // the TI model's output sat at the negative rail.
    void theOpAmpLibrarysModelsAreNetlistedRight()
    {
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "Lib"}, {"name", "U2"}, {"x", 600}, {"y", 100},
                                               {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "uA741"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Lib"}, {"name", "U1"}, {"x", 300}, {"y", 200},
                                                                              {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(TI)"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 300},
                                                                              {"properties", QJsonObject{{"U", "0.1 V"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V2"}, {"x", 100}, {"y", 500},
                                                                              {"properties", QJsonObject{{"U", "15 V"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V3"}, {"x", 250}, {"y", 500},
                                                                              {"properties", QJsonObject{{"U", "15 V"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RG"}, {"x", 450}, {"y", 450}, {"rotation", 1}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RF"}, {"x", 550}, {"y", 350},
                                                                              {"properties", QJsonObject{{"R", "10k"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", ".DC"}, {"name", "DC1"}, {"x", 100}, {"y", 650}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "U1.1"}, {"name", "inn"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "U1.2"}, {"name", "inp"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "U1.3"}, {"name", "out"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "U1.4"}, {"name", "vcc"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "U1.5"}, {"name", "vee"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "V1.1"}, {"name", "inp"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "V2.1"}, {"name", "vcc"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V2.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V3.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "V3.2"}, {"name", "vee"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "RG.1"}, {"name", "inn"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RG.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "RF.1"}, {"name", "inn"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "RF.2"}, {"name", "out"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList lines;
        for (const QJsonValue& v : json(call("get_netlist", {{"map", true}})).toObject().value("netlist").toArray()) lines << v.toString();
        QucsSettings.LibDir = was;
        // 30 pF, not 30 (farads); the tail current source as a schematic's.
        // (Its C1: the TI model has one too.)
        const QStringList capacitor = lines.filter(QRegularExpression("^CC1 _net6 _net19 "));
        QVERIFY2(capacitor.size() == 1 && QRegularExpression("^CC1 _net6 _net19 30p\\s*$", QRegularExpression::CaseInsensitiveOption).match(capacitor.first()).hasMatch(),
                 qPrintable(lines.filter(QRegularExpression("^CC1 ")).join('\n')));
        const QStringList tail = lines.filter(QRegularExpression("^IdcIEE "));
        QVERIFY2(tail.size() == 1 && tail.first().startsWith("IdcIEE _net10 _netP_VEE DC 1.016e-05", Qt::CaseInsensitive), qPrintable(tail.join('\n')));
        // The macromodels' pins by the names their models give them (sixth
        // round, wishlist 2): the TI 741's INN, INP, OUT, VCC, VEE.
        QStringList pinNames;
        const QJsonObject read = json(call("get_schematic", {{"components", QJsonArray{"U1"}}})).toObject();
        for (const QJsonValue& p : read.value("components").toArray().first().toObject().value("pins").toArray())
            pinNames << p.toObject().value("name").toString();
        QVERIFY2(pinNames == (QStringList{"INN", "INP", "OUT", "VCC", "VEE"}), qPrintable(QJsonDocument(read).toJson().left(3000)));
        r = call("connect", {{"from", "U1.nothing"}, {"to", "RG.1"}});
        QVERIFY2(failed(r) && text(r).contains("2 INP"), qPrintable(text(r)));
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (!ngspice.isEmpty()) {
            // A non-inverting gain of 11: 0.1 V in, 1.1 V out (it sat at -12.9 V).
            QVERIFY(!failed(call("delete", {{"names", QJsonArray{"U2"}}})));
            QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/ua741_ti.sch")}, {"replace", true}})));
            const QString before = QucsSettings.NgspiceExecutable;
            QucsSettings.NgspiceExecutable = ngspice;
            QucsSettings.LibDir = library + "/";
            r = call("simulate", {{"operating_point", true}, {"timeout", 60}}, 90000);
            double out = json(r).toObject().value("operating point").toObject().value("nodes").toObject().value("out").toDouble(-100);
            QVERIFY2(std::abs(out - 1.1) < 0.02, qPrintable(text(r)));
            // The Boyle 741 in its place, whose pins come INP first: taken by
            // name, not by number (by number the inputs swapped, and the
            // amplifier sat at a rail with every answer green).
            r = call("replace_component", {{"name", "U1"}, {"type", "Lib"},
                                           {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(boyle)"}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            r = call("simulate", {{"operating_point", true}, {"timeout", 60}}, 90000);
            out = json(r).toObject().value("operating point").toObject().value("nodes").toObject().value("out").toDouble(-100);
            QucsSettings.LibDir = was;
            QucsSettings.NgspiceExecutable = before;
            QVERIFY2(std::abs(out - 1.1) < 0.02, qPrintable(text(r)));
        }
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Round 6, wish 3: find_library_component says how each part fared in
    // the run of every part under ngspice (ngspice-tested.json beside the
    // libraries), and 'tested' lists only the parts that passed - read
    // again when the file changes.
    void theLibrarysTestIsReported()
    {
        const QString source = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library/LEDs.lib").absoluteFilePath();
        if (!QFileInfo::exists(source)) QSKIP("no library here");
        QTemporaryDir libraries;
        QVERIFY(QFile::copy(source, libraries.filePath("LEDs.lib")));
        const auto writeResults = [&](const QByteArray& parts) {
            QFile f(libraries.filePath("ngspice-tested.json"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("{\"tested\": \"2026-09-29\", \"ngspice\": \"46\", \"parts\": {" + parts + "}}");
            f.close();
        };
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = libraries.path() + "/";
        const auto outcomes = [&](const QJsonObject& args) {
            QMap<QString, QString> by;
            for (const QJsonValue& v : json(call("find_library_component", args)).toObject().value("found").toArray())
                if (v.toObject().value("library").toString() == "LEDs") by.insert(v.toObject().value("component").toString(), v.toObject().value("ngspice").toString());
            return by;
        };
        // No results here: each part is untested.
        QMap<QString, QString> by = outcomes({{"search", "light emitting diode"}, {"library", "LEDs"}});
        QVERIFY2(by.size() == 5 && by.value("red").startsWith("not tested"), qPrintable(QStringList(by.values()).join('\n')));
        QVERIFY(outcomes({{"search", "light emitting diode"}, {"tested", true}}).isEmpty());

        writeResults("\"LEDs/red\": {\"passes\": true}, \"LEDs/blue\": {\"passes\": false, \"why\": \"the operating point fails: singular matrix\"}");
        by = outcomes({{"search", "light emitting diode"}, {"library", "LEDs"}});
        QVERIFY2(by.value("red").startsWith("tested: it netlists and its operating point converges") && by.value("red").contains("2026-09-29, ngspice 46"),
                 qPrintable(by.value("red")));
        QVERIFY2(by.value("blue").startsWith("tested: fails - the operating point fails: singular matrix"), qPrintable(by.value("blue")));
        QCOMPARE(by.value("green"), QStringLiteral("not tested"));
        QCOMPARE(outcomes({{"search", "light emitting diode"}, {"tested", true}}).keys(), QStringList{"red"});

        // A new run: read again. A part with no pins is not tested.
        QTest::qWait(1100);   // (a modification time a second on)
        writeResults("\"LEDs/red\": {\"passes\": true}, \"LEDs/green\": {\"passes\": true}, "
                     "\"LEDs/yellow\": {\"passes\": false, \"untested\": true, \"why\": \"it has no pins\"}");
        QCOMPARE(outcomes({{"search", "light emitting diode"}, {"tested", true}}).keys(), (QStringList{"green", "red"}));
        QCOMPARE(outcomes({{"search", "light emitting diode"}, {"library", "LEDs"}}).value("yellow"), QStringLiteral("not tested - it has no pins"));
        QucsSettings.LibDir = was;
    }

    // Found by that run: a library part whose model is one component line
    // (a varactor: a Diode with the library's values) was placed as a Lib,
    // whose netlist read the diode's values as its library and part
    // ("Cannot load library component "1.1718" from library/4.2156e-14").
    // It is that component, as the library panel places it - by
    // add_component, and by set_schematic's components.
    void aOneLineLibraryPartIsThatComponent()
    {
        const QString source = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library/Varactor.lib").absoluteFilePath();
        if (!QFileInfo::exists(source)) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = QFileInfo(source).absolutePath() + "/";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "Lib"}, {"name", "D1"}, {"x", 300}, {"y", 300},
                                               {"properties", QJsonObject{{"Lib", "Varactor"}, {"Comp", "BB833"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QCOMPARE(o.value("type").toString(), QStringLiteral("Diode"));
        QCOMPARE(o.value("name").toString(), QStringLiteral("D1"));
        QVERIFY2(o.value("note").toString().startsWith("Varactor/BB833 is a Diode with the library's values"), qPrintable(text(r)));
        // In a library with a default symbol too (the Lib took that symbol
        // and named a subcircuit there was none of).
        if (QFileInfo::exists(QucsSettings.LibDir + "NMOSFETs.lib")) {
            r = call("add_component", {{"type", "Lib"}, {"name", "M1"}, {"x", 300}, {"y", 500},
                                       {"properties", QJsonObject{{"Lib", "NMOSFETs"}, {"Comp", "2N3797"}}}});
            QVERIFY2(!failed(r) && json(r).toObject().value("type").toString() == "_MOSFET", qPrintable(text(r)));
            QVERIFY(!failed(call("delete", {{"names", QJsonArray{"M1"}}})));
        }
        // A Lib edited into one is refused; replace_component places it.
        if (QFileInfo::exists(QucsSettings.LibDir + "LEDs.lib")) {
            r = call("add_component", {{"type", "Lib"}, {"name", "X5"}, {"x", 600}, {"y", 500},
                                       {"properties", QJsonObject{{"Lib", "LEDs"}, {"Comp", "red"}}}});
            QVERIFY2(!failed(r) && json(r).toObject().value("type").toString() == "Lib", qPrintable(text(r)));
            r = call("edit_component", {{"name", "X5"}, {"properties", QJsonObject{{"Lib", "Varactor"}, {"Comp", "BB833"}}}});
            QVERIFY2(failed(r) && text(r).contains("replace_component X5 with type Lib"), qPrintable(text(r)));
            r = call("replace_component", {{"name", "X5"}, {"type", "Lib"}, {"properties", QJsonObject{{"Lib", "Varactor"}, {"Comp", "BB833"}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QString type;
            for (const QJsonValue& v : json(call("get_schematic", {{"format", "json"}})).toObject().value("components").toArray())
                if (v.toObject().value("name").toString() == "X5") type = v.toObject().value("type").toString();
            QCOMPARE(type, QStringLiteral("Diode"));
            // (A Lib that took the diode's looks for a while: its netlist
            // was a Lib's, reading the values as its library and part.)
            const QString placed = text(call("get_netlist"));
            QVERIFY2(!placed.contains("Cannot load") && placed.contains("DMOD_X5 D"), qPrintable(placed));
            QVERIFY(!failed(call("delete", {{"names", QJsonArray{"X5"}}})));
        }
        QJsonArray parts{QJsonObject{{"type", "Lib"}, {"name", "D2"}, {"x", 500}, {"y", 300},
                                     {"properties", QJsonObject{{"Lib", "Varactor"}, {"Comp", "BB914"}}}},
                         QJsonObject{{"type", "Lib"}, {"name", "D1"}, {"x", 300}, {"y", 300},
                                     {"properties", QJsonObject{{"Lib", "Varactor"}, {"Comp", "BB833"}}}}};
        // (Under a library's default symbol a Lib stays a Lib: named a
        // subcircuit there is none of.)
        const bool mosfets = QFileInfo::exists(QucsSettings.LibDir + "NMOSFETs.lib");
        if (mosfets)
            parts.append(QJsonObject{{"type", "Lib"}, {"name", "M3"}, {"x", 300}, {"y", 500},
                                     {"properties", QJsonObject{{"Lib", "NMOSFETs"}, {"Comp", "2N3797"}}}});
        r = call("set_schematic", {{"components", parts}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList lines;
        for (const QJsonValue& v : json(call("get_netlist", {{"map", true}})).toObject().value("netlist").toArray()) lines << v.toString();
        QucsSettings.LibDir = was;
        const QString netlist = lines.join('\n');
        QVERIFY2(!netlist.contains("Cannot load"), qPrintable(netlist));
        QVERIFY2(QRegularExpression("^\\.MODEL DMOD_D1 D \\(Is=4\\.2156E-14 N=1\\.1718 Cj0=2\\.9033E-11", QRegularExpression::MultilineOption).match(netlist).hasMatch(),
                 qPrintable(netlist));
        QVERIFY2(QRegularExpression("^\\.MODEL DMOD_D2 D \\(Is=1E-14 N=1\\.02 Cj0=7\\.554E-11", QRegularExpression::MultilineOption).match(netlist).hasMatch(),
                 qPrintable(netlist));
        if (mosfets)
            QVERIFY2(QRegularExpression("^M3 \\S+ \\S+ \\S+ \\S+ MMOD_M3 ", QRegularExpression::MultilineOption).match(netlist).hasMatch() && !netlist.contains("NMOSFETs_2N3797"),
                     qPrintable(netlist));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Where a wire crosses a part's symbol other than to leave one of its
    // pins straight out of it (as the pin's stub goes: toward the nearest
    // side of its box) - a ground symbol's too. Empty: none.
    static QStringList wiresOverSymbols(Schematic* sch)
    {
        QStringList over;
        for (const Wire* w : sch->a_DocWires) {
            const QRect piece = QRect(w->P1(), w->P2()).normalized();
            for (const Component* c : sch->a_DocComps) {
                if (c->Ports.isEmpty()) continue;
                const QRect box = c->boundingRect().adjusted(1, 1, -1, -1);
                if (!piece.intersects(box)) continue;
                bool leaves = false;
                for (const Port* p : c->Ports) {
                    const QPoint pin(c->cx + p->x, c->cy + p->y);
                    for (const auto& [end, other] : {std::pair(w->P1(), w->P2()), std::pair(w->P2(), w->P1())}) {
                        if (end != pin || !box.contains(pin)) continue;
                        const int l = pin.x() - box.left(), r = box.right() - pin.x(), u = pin.y() - box.top(), d = box.bottom() - pin.y();
                        const int least = std::min({l, r, u, d});
                        const QPoint out = least == l ? QPoint(-1, 0) : least == r ? QPoint(1, 0) : least == u ? QPoint(0, -1) : QPoint(0, 1);
                        const QPoint dir((other.x() > end.x()) - (other.x() < end.x()), (other.y() > end.y()) - (other.y() < end.y()));
                        leaves = leaves || dir == out;
                    }
                }
                if (!leaves)
                    over << QStringLiteral("%1,%2-%3,%4 over %5").arg(w->P1().x()).arg(w->P1().y()).arg(w->P2().x()).arg(w->P2().y())
                                .arg(c->Name.isEmpty() ? c->Model : c->Name);
            }
        }
        return over;
    }

    // Where a wire crosses the texts of a part with no pin on its net (its
    // name, its values): the router goes round them where there is room. (A
    // part's own - an op-amp's name right under its VEE pin - a wire from
    // that pin may have to pass.)
    static QStringList wiresOverTexts(Schematic* sch)
    {
        QStringList over;
        for (const Wire* w : sch->a_DocWires) {
            const QRect piece = QRect(w->P1(), w->P2()).normalized();
            QSet<const Node*> nodes{w->Port1, w->Port2};   // (its net's, through the wires)
            for (QList<const Node*> todo(nodes.cbegin(), nodes.cend()); !todo.isEmpty();) {
                const Node* n = todo.takeLast();
                for (const Wire* o : n->wires())
                    for (const Node* m : {o->Port1, o->Port2})
                        if (!nodes.contains(m)) {
                            nodes.insert(m);
                            todo << m;
                        }
            }
            for (const Component* c : sch->a_DocComps) {
                if (c->Ports.isEmpty() || c->Model == "GND") continue;
                bool ours = false;
                for (const Port* p : c->Ports) ours = ours || nodes.contains(p->Connection);
                if (!ours && piece.intersects(c->boundingRectIncludingProperties()))
                    over << QStringLiteral("%1,%2-%3,%4 over %5").arg(w->P1().x()).arg(w->P1().y()).arg(w->P2().x()).arg(w->P2().y()).arg(c->Name);
            }
        }
        return over;
    }

    // The seventh round: the 741 bench (a gain of 11 by pin name, supplies
    // in a column, a load) arranged with its feedback below. The wires went
    // over the parts - a library part's box is wider than its pins, an
    // op-amp's supply pins are in its box, so no way from them was clear
    // and they were drawn over anything (Rf's up through the op-amp, VEE's
    // through a source's lead and a ground symbol's stem); Rg went to the
    // load's column; the label sat at a pin, over the drawing.
    void theFeedbackNetworkIsDrawnAsOne()
    {
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        const auto add = [](const char* type, const char* name, int x, int y, const QJsonObject& props = {}, int rotation = 0) {
            return QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", type}, {"name", name}, {"x", x}, {"y", y},
                                                                                     {"rotation", rotation}, {"properties", props}}}};
        };
        const auto connect = [](const char* from, const char* to) {
            return QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", from}, {"to", to}}}};
        };
        QJsonObject r = call("batch", {{"calls", QJsonArray{
            add("Lib", "U1", 400, 300, QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(TI)"}}),
            add("Vac", "Vin", 150, 350, QJsonObject{{"U", "0.1 V"}}), add("R", "Rg", 300, 450, QJsonObject{{"R", "1k"}}, 1),
            add("R", "Rf", 450, 150, QJsonObject{{"R", "10k"}}), add("R", "RL", 600, 400, QJsonObject{{"R", "10k"}}, 1),
            add("Vdc", "V1", 50, 150, QJsonObject{{"U", "15 V"}}), add("Vdc", "V2", 50, 500, QJsonObject{{"U", "15 V"}}),
            add(".TR", "TR1", 100, 650, QJsonObject{{"Stop", "3 ms"}}),
            connect("Vin.1", "U1.INP"), connect("Vin.2", "ground"), connect("Rg.2", "U1.inn"), connect("Rg.1", "ground"),
            connect("Rf.1", "U1.inn"), connect("Rf.2", "U1.out"), connect("RL.1", "U1.out"), connect("RL.2", "ground"),
            connect("V1.1", "U1.vcc"), connect("V1.2", "ground"), connect("V2.1", "ground"), connect("V2.2", "U1.vee"),
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "U1.out"}, {"name", "out"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "U1.vee"}, {"name", "vn"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // connect's wires too go round the symbols (V2's to VEE went a step at a time).
        const QStringList connected = wiresOverSymbols(front());
        QVERIFY2(connected.isEmpty(), qPrintable(connected.join('\n')));
        QStringList before;
        for (const QJsonValue& v : json(call("get_netlist", {{"map", true}})).toObject().value("nodes").toObject()) before << v.toVariant().toStringList().join(' ');
        r = call("arrange", {{"feedback", "below"}});
        QVERIFY2(!failed(r) && text(r).contains("every net as it was"), qPrintable(text(r)));
        // Rg with the feedback network: U1's column, not the load's.
        const QJsonArray columns = json(r).toObject().value("columns").toArray();
        QVERIFY2(columns.size() == 3 && columns.at(1).toArray().contains("Rg") && !columns.at(2).toArray().contains("Rg"), qPrintable(text(r)));
        Schematic* sch = front();
        const Component *u1 = sch->getComponentByName("U1"), *rf = sch->getComponentByName("Rf"), *rg = sch->getComponentByName("Rg");
        QVERIFY(u1 && rf && rg);
        // Standing under Rf's end on the inverting input's net, ground below.
        const auto pinAt = [](const Component* c, int k) { return QPoint(c->cx + c->Ports.at(k)->x, c->cy + c->Ports.at(k)->y); };
        int rfIn = -1, rgIn = -1;
        for (const QJsonValue& v : json(call("get_netlist", {{"map", true}})).toObject().value("nodes").toObject()) {
            const QStringList on = v.toVariant().toStringList();
            if (!on.contains("U1.1 (INN)")) continue;
            for (int k = 0; k < 2; ++k) {
                if (on.contains(QStringLiteral("Rf.%1").arg(k + 1))) rfIn = k;
                if (on.contains(QStringLiteral("Rg.%1").arg(k + 1))) rgIn = k;
            }
        }
        QVERIFY(rfIn >= 0 && rgIn >= 0);
        QVERIFY2(pinAt(rg, rgIn).x() == pinAt(rf, rfIn).x() && pinAt(rg, rgIn).y() > pinAt(rf, rfIn).y() && pinAt(rg, 1 - rgIn).y() > pinAt(rg, rgIn).y()
                     && pinAt(rg, 0).x() == pinAt(rg, 1).x(),
                 qPrintable(QStringLiteral("Rf.%1 %2,%3; Rg.%4 %5,%6").arg(rfIn + 1).arg(pinAt(rf, rfIn).x()).arg(pinAt(rf, rfIn).y())
                                .arg(rgIn + 1).arg(pinAt(rg, rgIn).x()).arg(pinAt(rg, rgIn).y())));
        // No wire over a symbol - the op-amp's, a source's, a ground's.
        const QStringList over = wiresOverSymbols(sch);
        QVERIFY2(over.isEmpty(), qPrintable(over.join('\n')));
        // Nor over another part's texts (VEE's ran through the source's name).
        const QStringList overTexts = wiresOverTexts(sch);
        QVERIFY2(overTexts.isEmpty(), qPrintable(overTexts.join('\n')));
        // Each label on the longest wire of its net, lying down, its text clear of every part.
        for (const char* name : {"out", "vn"}) {
            const Wire* labelled = nullptr;
            for (const Wire* w : sch->a_DocWires)
                if (w->hasLabel() && w->label()->Name == name) labelled = w;
            QVERIFY2(labelled != nullptr && labelled->P1().y() == labelled->P2().y(), name);
            QList<const Wire*> itsWires{labelled};   // (its net's: joined through their nodes)
            for (int k = 0; k < itsWires.size(); ++k)
                for (const Node* n : {itsWires.at(k)->Port1, itsWires.at(k)->Port2})
                    for (const Wire* w : n->wires())
                        if (!itsWires.contains(w)) itsWires << w;
            for (const Wire* w : std::as_const(itsWires))
                if (w->P1().y() == w->P2().y())
                    QVERIFY2((w->P1() - w->P2()).manhattanLength() <= (labelled->P1() - labelled->P2()).manhattanLength(), name);
            const QRect textBox(QPoint(labelled->label()->x1, labelled->label()->y1), QSize(8 * int(strlen(name)) + 10, 16));
            for (const Component* c : sch->a_DocComps)
                QVERIFY2(!textBox.intersects(c->boundingRectIncludingProperties()), qPrintable(QStringLiteral("%1 over %2").arg(name, c->Name)));
        }
        // And the nets as they were.
        QStringList after;
        for (const QJsonValue& v : json(call("get_netlist", {{"map", true}})).toObject().value("nodes").toObject()) after << v.toVariant().toStringList().join(' ');
        QucsSettings.LibDir = was;
        before.sort();
        after.sort();
        QCOMPARE(after, before);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // tune's 'hold' and 'compare' (the assessment of 29 September): other
    // measurements kept within bounds while the value moves, a value that
    // reaches the target but breaks one told and not taken; every
    // measurement before and after. A ladder: 10 V, R1, a, R2, b, R3; with
    // R2 = R3 = 1k, a is 5 V at R1 = 2k, and b is then 2.5 V.
    void tuneHoldsAndCompares()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        // (Put back whatever happens: a test failing here left the others
        // another ngspice.)
        struct Restore {
            QString was = QucsSettings.NgspiceExecutable;
            ~Restore() { QucsSettings.NgspiceExecutable = was; }
        } restore;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Vdc V1 1 100 200 18 -26 0 1 \"10 V\" 1>\n"
                                                        "  <GND * 1 100 230 0 0 0 0>\n"
                                                        "  <R R1 1 200 100 15 -26 0 0 " + R +
                                                        "  <R R2 1 300 100 15 -26 0 0 " + R +
                                                        "  <R R3 1 400 200 15 -26 0 1 " + R +
                                                        "  <GND * 1 400 230 0 0 0 0>\n"
                                                        "  <.DC DC1 1 100 400 0 36 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
                                                        "</Components>\n<Wires>\n"
                                                        "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
                                                        "  <230 100 270 100 \"a\" 240 70 20 \"\">\n"
                                                        "  <330 100 400 100 \"b\" 350 70 30 \"\">\n  <400 100 400 170 \"\" 0 0 0 \"\">\n"
                                                        "</Wires>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", "ladder"}, {"replace", true}})));
        const QJsonObject target{{"component", "R1"}, {"target", 5}, {"range", QJsonArray{"100", "100k"}},
                                 {"measure", QJsonObject{{"operating_point", "a"}}}};
        // b kept above 2.4 V: it is (2.5 V), the value set; before and after.
        QJsonObject args = target;
        args.insert("hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}}}, {"min", 2.4}}});
        r = call("tune", args, 300000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QVERIFY2(o.value("within tolerance").toBool() && !o.contains("held back"), qPrintable(text(r)));
        double value = 0, factor = 1;
        QString unit;
        misc::str2num(sch->getComponentByName("R1")->getProperty("R")->Value, value, unit, factor);
        QVERIFY2(std::abs(value * factor - 2000) < 40, qPrintable(text(r)));
        QJsonArray runs = o.value("runs").toArray();
        QVERIFY2(runs.first().toObject().value("as it was").toBool() && runs.first().toObject().value("value").toString() == "1k"
                     && runs.first().toObject().value("hold").toArray().size() == 1,
                 qPrintable(text(r)));
        QJsonArray table = o.value("before and after").toArray();
        QVERIFY2(table.size() == 2 && std::abs(table.at(0).toObject().value("before").toDouble() - 20.0 / 3) < 0.01
                     && std::abs(table.at(0).toObject().value("after").toDouble() - 5) < 0.05
                     && std::abs(table.at(1).toObject().value("before").toDouble() - 10.0 / 3) < 0.01
                     && std::abs(table.at(1).toObject().value("after").toDouble() - 2.5) < 0.05,
                 qPrintable(text(r)));
        QVERIFY(!failed(call("undo")));
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("1k"));

        // b kept above 2.6 V: the target breaks it - told, not taken.
        args = target;
        args.insert("hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}}}, {"min", 2.6}}});
        r = call("tune", args, 300000);
        o = json(r).toObject();
        QVERIFY2(!failed(r) && o.value("held back").toString().startsWith("R1 = ") && o.value("held back").toString().contains("gives the target, but")
                     && o.value("held back").toString().contains("below 2.6 - not taken")
                     && o.value("set").toString().startsWith("not set") && o.value("set").toString().contains("keeping every hold"),
                 qPrintable(text(r)));
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("1k"));
        // compare alone: the table, no hold.
        args = target;
        args.insert("compare", true);
        r = call("tune", args, 300000);
        o = json(r).toObject();
        QVERIFY2(!failed(r) && o.value("before and after").toArray().size() == 1 && !o.contains("hold"), qPrintable(text(r)));
        QVERIFY(!failed(call("undo")));
        // Not of the target's kind: refused.
        args = target;
        args.insert("hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"variable", "tran.v(b)"}}}, {"min", 1}}});
        r = call("tune", args);
        QVERIFY2(failed(r) && text(r).contains("'hold' measures what the target measures"), qPrintable(text(r)));
        args.insert("hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}}}}});
        r = call("tune", args);
        QVERIFY2(failed(r) && text(r).contains("a 'min', a 'max' or both"), qPrintable(text(r)));
        // (The hunt after round 9, A4.) One hold given as an object, not a
        // list of them: refused - it was dropped, and the tune ran free. A
        // misspelt field in a hold's measure: refused, as the target's is.
        args.insert("hold", QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}}}, {"min", 2.4}});
        r = call("tune", args);
        QVERIFY2(failed(r) && text(r).contains("tune: hold is a list, not an object. Nothing was done"), qPrintable(text(r)));
        args.insert("hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}, {"waht", "x"}}}, {"min", 2.4}}});
        r = call("tune", args);
        QVERIFY2(failed(r) && text(r).contains("hold[0].measure has no waht") && text(r).contains("Meant what?"), qPrintable(text(r)));
        // A hold on a node there is not: its nodes and its currents, each called so.
        args.insert("hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "nowhere"}}}, {"min", 2.4}}});
        r = call("tune", args, 300000);
        QVERIFY2(text(r).contains(QRegularExpression("hold 1 could not be measured: the operating point has no nowhere \\(its nodes: "
                                                     "[^;()]+; its currents: i\\(v1\\)\\)")),
                 qPrintable(text(r)));

        // Two knobs, a hold: measured each run, before and after.
        r = call("tune", {{"knobs", QJsonArray{QJsonObject{{"component", "R1"}, {"range", QJsonArray{"100", "100k"}}},
                                               QJsonObject{{"component", "R2"}, {"range", QJsonArray{"100", "100k"}}}}},
                          {"targets", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "a"}}}, {"target", 5}},
                                                 QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}}}, {"target", 2}}}},
                          {"hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "a"}}}, {"min", 4}, {"max", 6}}}}},
                 300000);
        o = json(r).toObject();
        QVERIFY2(!failed(r) && o.value("set").toString().startsWith("The values found are set")
                     && o.value("before and after").toArray().size() == 3 && o.value("runs").toArray().first().toObject().value("as it was").toBool()
                     && o.value("runs").toArray().last().toObject().value("keeps").toBool(),
                 qPrintable(text(r)));
        for (const QJsonValue& run : o.value("runs").toArray())
            QVERIFY2(run.toObject().value("hold").toArray().size() == 1, qPrintable(text(r)));
        QVERIFY(!failed(call("undo")));
        // a kept under 4.9 V: the targets (a at 5 V) break it - told, not set.
        r = call("tune", {{"knobs", QJsonArray{QJsonObject{{"component", "R1"}, {"range", QJsonArray{"100", "100k"}}},
                                               QJsonObject{{"component", "R2"}, {"range", QJsonArray{"100", "100k"}}}}},
                          {"targets", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "a"}}}, {"target", 5}},
                                                 QJsonObject{{"measure", QJsonObject{{"operating_point", "b"}}}, {"target", 2}}}},
                          {"hold", QJsonArray{QJsonObject{{"measure", QJsonObject{{"operating_point", "a"}}}, {"max", 4.9}}}}},
                 300000);
        o = json(r).toObject();
        QVERIFY2(!failed(r) && o.value("held back").toString().contains("gives every target, but")
                     && o.value("held back").toString().contains("above 4.9 - not taken") && o.value("set").toString().startsWith("not set"),
                 qPrintable(text(r)));
        QCOMPARE(sch->getComponentByName("R1")->getProperty("R")->Value, QStringLiteral("1k"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // arrange's 'straighten' (the assessment of 29 September, wishlist 1): a
    // part nudged a grid step or a few so the pins of a wire between two
    // parts line up and it runs straight - here a jog of 10 between R1 and
    // R2. Not where it would come onto another part.
    void straightenLinesUpTheWires()
    {
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        const QString parts = "<Components>\n"
                              "  <R R1 1 100 100 -26 15 0 0 " + R +
                              "  <R R2 1 250 110 -26 15 0 0 " + R +
                              "  <R R3 1 400 110 -26 15 0 0 " + R +
                              "</Components>\n<Wires>\n"
                              "  <130 100 175 100 \"\" 0 0 0 \"\">\n  <175 100 175 110 \"\" 0 0 0 \"\">\n  <175 110 220 110 \"\" 0 0 0 \"\">\n"
                              "  <280 110 370 110 \"\" 0 0 0 \"\">\n"
                              "</Wires>\n";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("set_schematic", {{"text", parts}})));
        Schematic* sch = front();
        // The wire from R1.2 to R2.1, as drawn: its segments' heights.
        const auto heights = [sch] {
            QSet<int> ys;
            for (const Wire* w : sch->a_DocWires)
                if (std::min(w->x1, w->x2) < 220 && std::max(w->x1, w->x2) > 130) ys << w->y1 << w->y2;
            return ys;
        };
        QCOMPARE(heights().size(), 2);
        QJsonObject r = call("arrange", {{"keep_places", true}, {"straighten", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString said = json(r).toObject().value("straightened").toString();
        QVERIFY2(said.startsWith("1 parts nudged so their wires run straight: R1 10 down"), qPrintable(text(r)));
        QVERIFY2(heights().size() == 1, qPrintable(text(r)));   // one height: straight
        QCOMPARE(sch->getComponentByName("R2")->cy, 110);          // (R2 and R3 were in line: R1 moved, not R2)
        QCOMPARE(sch->getComponentByName("R1")->cy, 110);
        QVERIFY(!failed(call("undo")));
        QCOMPARE(sch->getComponentByName("R1")->cy, 100);
        // A part in the way: R4 where R1 would go - nothing nudged into it.
        // Its symbol meets the one R1 would have 10 lower (R1 from 99 to 121,
        // R4 from 119 to 141). At 150 only R1's texts reached it, and their
        // size is the font's: on the Linux CI, whose font is sized in pixels,
        // they did not, and R1 was nudged.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R4"}, {"x", 100}, {"y", 130}})));
        r = call("arrange", {{"keep_places", true}, {"straighten", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(!json(r).toObject().value("straightened").toString().contains("R1 10 down"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The ninth round: 'straighten' lines up two pins only when they face
    // each other - a straight wire can join them. By their places alone it
    // moved VCC down to put its + level with an op-amp's VCC pin, both
    // facing up: the wire went over the top all the same, VCC's ground was
    // pushed aside onto a jog, and the VEE wire crossed it.
    void straightenLinesUpOnlyPinsThatFace()
    {
        // Two standing resistors, their top pins joined over the top, 20 apart.
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        const QString parts = "<Components>\n"
                              "  <R R1 1 100 100 15 -26 0 1 " + R +
                              "  <R R2 1 250 120 15 -26 0 1 " + R +
                              "</Components>\n<Wires>\n"
                              "  <100 70 100 40 \"\" 0 0 0 \"\">\n  <100 40 250 40 \"\" 0 0 0 \"\">\n  <250 40 250 90 \"\" 0 0 0 \"\">\n"
                              "</Wires>\n";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("set_schematic", {{"text", parts}})));
        QJsonObject r = call("arrange", {{"keep_places", true}, {"straighten", true}});
        QVERIFY2(!failed(r) && json(r).toObject().value("straightened").toString().startsWith("nothing to nudge"), qPrintable(text(r)));
        QCOMPARE(front()->getComponentByName("R1")->cy, 100);
        QCOMPARE(front()->getComponentByName("R2")->cy, 120);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        // A coupled line's pin sits at a corner of its box, its stub going
        // up: a standing resistor 10 to its side, facing down onto it, is
        // lined up (by the box alone the pin faced left).
        const QString coupled = "<Components>\n"
                                "  <MCOUPLED MS1 1 300 300 -26 37 0 0 \"Subst1\" 0 \"W1\" 1 \"L1\" 1 \"S1\" 1 \"Kirschning\" 0 \"Kirschning\" 0 \"26.85\" 0>\n"
                                "  <R R1 1 260 210 15 -26 0 1 " + R +
                                "</Components>\n<Wires>\n"
                                "  <260 240 260 255 \"\" 0 0 0 \"\">\n  <260 255 270 255 \"\" 0 0 0 \"\">\n  <270 255 270 270 \"\" 0 0 0 \"\">\n"
                                "</Wires>\n";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("set_schematic", {{"text", coupled}})));
        r = call("arrange", {{"keep_places", true}, {"straighten", true}});
        QVERIFY2(!failed(r) && json(r).toObject().value("straightened").toString().startsWith("1 parts nudged"), qPrintable(text(r)));
        const Component *ms1 = front()->getComponentByName("MS1"), *r1 = front()->getComponentByName("R1");
        QVERIFY(ms1->cx + ms1->Ports.at(0)->x == r1->cx);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // The reviewer's bench: VCC stays, its ground under it, no wire over a ground's stub.
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        const auto add = [](const char* type, const char* name, int x, int y, const QJsonObject& props = {}, int rotation = 0) {
            return QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", type}, {"name", name}, {"x", x}, {"y", y},
                                                                                     {"rotation", rotation}, {"properties", props}}}};
        };
        const auto connect = [](const char* from, const char* to) {
            return QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", from}, {"to", to}}}};
        };
        r = call("batch", {{"calls", QJsonArray{
            add("Lib", "U1", 400, 300, QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(TI)"}}),
            add("Vac", "Vin", 150, 350, QJsonObject{{"U", "0.1 V"}}), add("R", "Rg", 300, 450, QJsonObject{{"R", "1k"}}, 1),
            add("R", "Rf", 450, 150, QJsonObject{{"R", "10k"}}), add("R", "RL", 600, 400, QJsonObject{{"R", "10k"}}, 1),
            add("Vdc", "VCC", 50, 150, QJsonObject{{"U", "15 V"}}), add("Vdc", "VEE", 50, 500, QJsonObject{{"U", "15 V"}}),
            add(".TR", "TR1", 100, 650, QJsonObject{{"Stop", "3 ms"}}),
            connect("Vin.1", "U1.INP"), connect("Vin.2", "ground"), connect("Rg.2", "U1.inn"), connect("Rg.1", "ground"),
            connect("Rf.1", "U1.inn"), connect("Rf.2", "U1.out"), connect("RL.1", "U1.out"), connect("RL.2", "ground"),
            connect("VCC.1", "U1.vcc"), connect("VCC.2", "ground"), connect("VEE.1", "ground"), connect("VEE.2", "U1.vee")}}});
        QucsSettings.LibDir = was;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("arrange", {{"feedback", "below"}, {"straighten", true}});
        QVERIFY2(!failed(r) && text(r).contains("every net as it was"), qPrintable(text(r)));
        QVERIFY2(!json(r).toObject().value("straightened").toString().contains("VCC"), qPrintable(text(r)));
        const Component* vcc = front()->getComponentByName("VCC");
        bool under = false;
        for (const Component* c : front()->a_DocComps)
            if (c->Model == "GND" && c->cx == vcc->cx + vcc->Ports.at(1)->x && c->cy == vcc->cy + vcc->Ports.at(1)->y) under = true;
        QVERIFY(under);
        const QString notes = QJsonDocument(json(call("check_schematic")).toObject().value("notes").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(!notes.contains("nets gnd and") && !notes.contains("and gnd are"), qPrintable(notes));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The assessment of 29 September, its small things: import_netlist
    // takes a title (a text above the circuit, the name of its subcircuits'
    // library); get_netlist's map lists the parts whose pins have no names,
    // each pin with its side.
    void theAssessmentsSmallThings()
    {
        QJsonObject r = call("import_netlist", {{"text", "r1 in out 1k\nc1 out 0 1u\nv1 in 0 1\n.subckt buf a b\nr1 a b 1\n.ends\n"
                                                         "x1 out 0 buf\n.end\n"},
                                                {"title", "My \"filter\""}});
        QVERIFY2(!failed(r) && json(r).toObject().value("title").toString() == "My \"filter\""
                     && json(r).toObject().value("subcircuits").toString().endsWith("My__filter__subcircuits.lib"),
                 qPrintable(text(r)));
        bool shown = false;
        for (Painting* p : front()->a_DocPaints)
            if (dynamic_cast<GraphicText*>(p) != nullptr && p->save().contains("\"My 'filter'\"")) shown = true;
        QVERIFY(shown);

        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "OpAmp"}, {"name", "OP1"}, {"x", 300}, {"y", 200}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 500}, {"y", 200}})));
        QVERIFY(!failed(call("connect", {{"from", "R1.1"}, {"to", "ground"}})));
        r = call("get_netlist", {{"map", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray unnamed = json(r).toObject().value("pins without names").toArray();
        QVERIFY2(unnamed == QJsonArray{"OP1: 1 (left, lower), 2 (left, upper), 3 (right)"}, qPrintable(text(r)));   // (R1: two pins)

        // describe_part: a library part's pins, model, supplies, test in one call.
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        r = call("describe_part", {{"library", "opamps"}, {"part", "uA741"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject d = json(r).toObject();
        QStringList pins;
        for (const QJsonValue& v : d.value("pins").toArray()) {
            const QJsonObject p = v.toObject();
            pins << QStringLiteral("%1 %2 %3 %4").arg(p.value("pin").toInt()).arg(p.value("name").toString(), p.value("role").toString(),
                                                         p.value("side").toString());
        }
        QVERIFY2(pins.size() == 5 && pins.at(0).startsWith("1 INN input ") && pins.at(1).startsWith("2 OUT output ")
                     && pins.at(2).startsWith("3 INP input ") && pins.at(3).startsWith("4 VCC supply ") && pins.at(4).startsWith("5 VEE supply "),
                 qPrintable(pins.join(" | ")));
        QVERIFY2(d.value("library").toString() == "OpAmps" && d.value("supply pins").toArray() == QJsonArray({"VCC", "VEE"})
                     && d.value("model").toString().startsWith("transistor level (21 transistors, 0 controlled sources, 0 diodes, 12 R/C/L")
                     && d.value("ngspice").toString().startsWith("tested")
                     && d.value("place").toObject().value("properties").toObject().value("Comp").toString() == "uA741"
                     && !d.contains("pins without names"),
                 qPrintable(text(r)));
        r = call("describe_part", {{"library", "OpAmps"}, {"part", "ua741(mod)"}});
        d = json(r).toObject();
        QVERIFY2(!failed(r) && d.value("pins").toArray().size() == 5 && d.contains("pins without names")
                     && d.value("model").toString().startsWith("macromodel") && !d.contains("supply pins"),
                 qPrintable(text(r)));
        r = call("describe_part", {{"library", "Diodes"}, {"part", "1N4148"}});
        d = json(r).toObject();
        QVERIFY2(!failed(r) && d.value("placed as").toString() == "Diode" && d.value("model").toString().startsWith("one component: placed as a Diode")
                     && d.value("pins").toArray().size() == 2,
                 qPrintable(text(r)));
        r = call("describe_part", {{"library", "OpAmps"}, {"part", "nosuch"}});
        QVERIFY2(failed(r) && text(r).contains("There is no part nosuch in a library OpAmps here"), qPrintable(text(r)));
        // The benches (wishlist 3): the uA741's passes; the S2K's diode, its
        // Is written 1.3 (amperes), fails - and is not 'tested'.
        r = call("describe_part", {{"library", "OpAmps"}, {"part", "uA741"}});
        QVERIFY2(json(r).toObject().value("ngspice").toString().contains("; its bench passes (op-amp: follower of 1 V, gain of 11"),
                 qPrintable(text(r)));
        r = call("describe_part", {{"library", "Diodes"}, {"part", "S2K"}});
        QVERIFY2(json(r).toObject().value("ngspice").toString().contains("but its bench FAILS (diode: 10 V through 9.3 kOhm")
                     && json(r).toObject().value("ngspice").toString().contains("the model runs and does the wrong thing"),
                 qPrintable(text(r)));
        r = call("find_library_component", {{"search", "S2"}, {"library", "Diodes"}, {"limit", 100}});
        QVERIFY2(text(r).contains("\"component\":\"S2K\""), qPrintable(text(r)));
        r = call("find_library_component", {{"search", "S2"}, {"library", "Diodes"}, {"limit", 100}, {"tested", true}});
        QVERIFY2(!text(r).contains("\"component\":\"S2K\"") && text(r).contains("\"component\":\"S2B\""), qPrintable(text(r)));
        QucsSettings.LibDir = was;
    }

    // check_schematic, the assessment of 29 September: it goes into the
    // subcircuits (a line of counts each, all of it with 'subcircuits'), and
    // reviews the design as well as the wiring - here an op-amp's load under
    // 1 kOhm, a note (the rest is test_erc's theDesignRulesFindWhatTheSimulatorWouldTrip).
    void theCheckGoesIntoSubcircuitsAndReviewsTheDesign()
    {
        // A subcircuit with a loose resistor inside, used by a schematic.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, y] : {std::pair{"a", 100}, std::pair{"b", 200}})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 150}, {"rotation", 1}})));
        QVERIFY(!failed(call("connect", {{"from", "a.1"}, {"to", "R1.1"}})));
        QVERIFY(!failed(call("connect", {{"from", "b.1"}, {"to", "R1.2"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R9"}, {"x", 400}, {"y", 300}})));
        QVERIFY(!failed(call("save_document", {{"as", "loose"}})));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Sub"}, {"name", "X1"}, {"x", 300}, {"y", 200},
                                               {"properties", QJsonObject{{"File", "loose.sch"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}})));
        QVERIFY(!failed(call("connect", {{"from", "V1.1"}, {"to", "X1.1"}})));
        QVERIFY(!failed(call("connect", {{"from", "V1.2"}, {"to", "ground"}})));
        QVERIFY(!failed(call("connect", {{"from", "X1.2"}, {"to", "ground"}})));
        QVERIFY(!failed(call("add_analysis", {{"kind", "op"}})));
        QJsonObject checked = json(call("check_schematic")).toObject();
        QVERIFY2(checked.value("errors").toArray().isEmpty() && checked.value("warnings").toArray().isEmpty(),
                 qPrintable(QJsonDocument(checked).toJson()));
        const QJsonArray subs = checked.value("subcircuits").toArray();
        QVERIFY2(subs.size() == 1 && subs.first().toObject().value("file").toString().endsWith("loose.sch")
                     && subs.first().toObject().value("warnings").toInt() == 2 && !subs.first().toObject().contains("warning list"),
                 qPrintable(QJsonDocument(checked).toJson()));
        QVERIFY2(checked.value("found").toString().endsWith("; in its subcircuits 0 errors, 2 warnings")
                     && checked.value("verdict").toString().contains("'subcircuits': true lists them"),
                 qPrintable(QJsonDocument(checked).toJson()));
        checked = json(call("check_schematic", {{"subcircuits", true}})).toObject();
        const QString listed = QJsonDocument(checked.value("subcircuits").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(listed.contains("R9: pin 1 is connected to nothing") && listed.contains("R9: pin 2 is connected to nothing"),
                 qPrintable(listed));

        // An op-amp's output loaded with 100 Ohm: a note; with 10 kOhm, none.
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Lib"}, {"name", "U1"}, {"x", 300}, {"y", 300},
                                               {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "uA741"}}}})));
        for (const auto& [pin, net] : {std::pair("U1.inp", "in"), std::pair("U1.inn", "out"), std::pair("U1.out", "out"),
                                       std::pair("U1.vcc", "vcc"), std::pair("U1.vee", "vee")})
            QVERIFY(!failed(call("set_label", {{"at", pin}, {"name", net}})));
        for (const auto& [name, x, value, net] : {std::tuple("VP", 100, "15 V", "vcc"), std::tuple("VN", 200, "-15 V", "vee"),
                                                  std::tuple("VIN", 500, "1 V", "in")}) {
            QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"name", name}, {"x", x}, {"y", 600},
                                                   {"properties", QJsonObject{{"U", value}}}})));
            QVERIFY(!failed(call("set_label", {{"at", QString(name) + ".1"}, {"name", net}})));
            QVERIFY(!failed(call("connect", {{"from", QString(name) + ".2"}, {"to", "ground"}})));
        }
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "RL"}, {"x", 600}, {"y", 400}, {"rotation", 1},
                                               {"properties", QJsonObject{{"R", "100 Ohm"}}}})));
        QVERIFY(!failed(call("set_label", {{"at", "RL.1"}, {"name", "out"}})));
        QVERIFY(!failed(call("connect", {{"from", "RL.2"}, {"to", "ground"}})));
        QVERIFY(!failed(call("add_analysis", {{"kind", "op"}})));
        QString notes = QJsonDocument(json(call("check_schematic")).toObject().value("notes").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(notes.contains("RL (100 Ohm) loads U1's output to ground"), qPrintable(notes));
        QVERIFY2(!notes.contains("no DC path"), qPrintable(notes));   // the follower's - input is on its output
        QVERIFY(!failed(call("edit_component", {{"name", "RL"}, {"properties", QJsonObject{{"R", "10 kOhm"}}}})));
        notes = QJsonDocument(json(call("check_schematic")).toObject().value("notes").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY2(!notes.contains("loads U1"), qPrintable(notes));
        QucsSettings.LibDir = was;
    }

    // The eighth round, 2.1: arrange's feedback network round a part whose
    // pins stand out as far as each other - a subcircuit's box - took the
    // first of Rf's two pins for the output: the load was put under Rf as
    // if it were Rg, and Rg went to a column of its own. The output is now
    // the pin named so, else the one on a net named so. And make_symbol
    // sides ports by their names; create_subcircuit names a port after the
    // pin of the part it came from.
    void theOutputIsFoundByNameWhenThePinsTie()
    {
        // An op-amp between ports inn, inp and out, with no symbol drawn:
        // its instance is a box with inn and out both at x -30.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, num, y] : {std::tuple("inn", "1", 100), std::tuple("inp", "2", 200), std::tuple("out", "3", 300)})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}, {"properties", QJsonObject{{"Num", num}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "OpAmp"}, {"name", "OP1"}, {"x", 300}, {"y", 200}})));
        for (const auto& [from, to] : {std::pair("inn.1", "OP1.2"), std::pair("inp.1", "OP1.1"), std::pair("out.1", "OP1.3")})
            QVERIFY(!failed(call("connect", {{"from", from}, {"to", to}})));
        QVERIFY(!failed(call("save_document", {{"as", "fbamp"}})));
        const auto bench = [this](bool outLabel) {
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            QJsonArray calls{QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Sub"}, {"name", "X1"}, {"x", 400}, {"y", 300},
                                                                                            {"properties", QJsonObject{{"File", "fbamp.sch"}}}}}}};
            for (const auto& [name, x, y, rotation] : {std::tuple("Vin", 150, 350, 0), std::tuple("Rf", 450, 150, 0), std::tuple("Rg", 300, 450, 1),
                                                       std::tuple("RL", 600, 400, 1)})
                calls << QJsonObject{{"tool", "add_component"},
                                     {"arguments", QJsonObject{{"type", QString(name) == "Vin" ? "Vdc" : "R"}, {"name", name}, {"x", x}, {"y", y}, {"rotation", rotation}}}};
            for (const auto& [from, to] : {std::pair("Vin.1", "X1.2"), std::pair("Vin.2", "ground"), std::pair("Rf.1", "X1.1"), std::pair("Rf.2", "X1.3"),
                                           std::pair("Rg.1", "X1.1"), std::pair("Rg.2", "ground"), std::pair("RL.1", "X1.3"), std::pair("RL.2", "ground")})
                calls << QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", from}, {"to", to}}}};
            if (outLabel) calls << QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "X1.3"}, {"name", "out"}}}};
            calls << QJsonObject{{"tool", "add_analysis"}, {"arguments", QJsonObject{{"kind", "op"}}}};
            const QJsonObject r = call("batch", {{"calls", calls}, {"atomic", true}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
        };
        const auto rgWithTheFeedback = [this](const QJsonObject& r) {
            const QJsonArray columns = json(r).toObject().value("columns").toArray();
            for (const QJsonValue& c : columns)
                if (c.toArray().contains("X1")) return c.toArray().contains("Rf") && c.toArray().contains("Rg") && !c.toArray().contains("RL");
            return false;
        };
        // No names on the box's pins: the label out says which is the output.
        bench(true);
        const Component* x1 = front()->getComponentByName("X1");
        QVERIFY(x1 && x1->Ports.size() == 3 && x1->Ports.at(0)->x == x1->Ports.at(2)->x && x1->Ports.at(0)->Name.isEmpty());
        QJsonObject r = call("arrange", {{"feedback", "below"}});
        QVERIFY2(!failed(r) && text(r).contains("every net as it was") && rgWithTheFeedback(r), qPrintable(text(r)));
        // make_symbol sides them by their names: inn and inp left, out right.
        QVERIFY(!failed(call("open_document", {{"path", dir.filePath("workspace/fbamp.sch")}})));
        r = call("make_symbol", {{"path", "fbamp.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList sides;
        for (const QJsonValue& p : json(r).toObject().value("pins").toArray()) sides << p.toString().section(',', 0, 0);
        QVERIFY2(sides == QStringList({"inn: left", "inp: left", "out: right"}), qPrintable(sides.join(" | ")));
        // All three on the left, and no label: the pins' names say it.
        r = call("make_symbol", {{"path", "fbamp.sch"}, {"sides", QJsonObject{{"out", "left"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"path", "fbamp.sch"}})));
        bench(false);
        x1 = front()->getComponentByName("X1");
        QVERIFY(x1 && x1->Ports.at(0)->x == x1->Ports.at(2)->x && x1->Ports.at(2)->Name == "out");
        r = call("arrange", {{"feedback", "below"}});
        QVERIFY2(!failed(r) && text(r).contains("every net as it was") && rgWithTheFeedback(r), qPrintable(text(r)));
        // Rf's end on the output's net under the output pin; Rg standing under its other.
        const Component *rf = front()->getComponentByName("Rf"), *rg = front()->getComponentByName("Rg");
        x1 = front()->getComponentByName("X1");
        const auto pinAt = [](const Component* c, int k) { return QPoint(c->cx + c->Ports.at(k)->x, c->cy + c->Ports.at(k)->y); };
        QVERIFY2(pinAt(rf, 1).x() == pinAt(x1, 2).x() && pinAt(rg, 0).x() == pinAt(rf, 0).x() && pinAt(rg, 0).y() > pinAt(rf, 0).y(),
                 qPrintable(QStringLiteral("X1.3 %1; Rf.1 %2, Rf.2 %3; Rg.1 %4,%5")
                                .arg(pinAt(x1, 2).x()).arg(pinAt(rf, 0).x()).arg(pinAt(rf, 1).x()).arg(pinAt(rg, 0).x()).arg(pinAt(rg, 0).y())));

        // create_subcircuit: a net without a label is named after the pin
        // of the part it came from - unless a net has that name already
        // (INP, whatever its case): then as before.
        QVERIFY(!failed(call("set_label", {{"at", "RL.1"}, {"name", "INP"}})));
        QVERIFY(!failed(call("save_document", {{"as", "fbparent"}})));
        r = call("create_subcircuit", {{"names", QJsonArray{"X1"}}, {"save_as", "fbwrap.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList ports;
        for (const QJsonValue& p : json(r).toObject().value("ports").toArray()) ports << p.toObject().value("net").toString();
        QVERIFY2(ports == QStringList({"inn", "fbwrap_n1", "INP"}), qPrintable(ports.join(" | ")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The eighth round, 2.2: a DC source below ground on a net a negative
    // supply's name is on (an op-amp's VEE pin, a label vee, the source
    // VEE) is as meant - no note. A supply the wrong way round is a
    // warning: a positive supply's pin (VCC) below ground, a negative
    // one's above it. A net nothing names keeps the note. The fix said is
    // the value that turns it (a turn is refused while wires would join).
    void supplySignsAreRead()
    {
        // A part with supply pins by name: a subcircuit, VCC on top, VEE below.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, num, y] : {std::tuple("VCC", "1", 100), std::tuple("VEE", "2", 300)})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}, {"properties", QJsonObject{{"Num", num}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 200}, {"rotation", 1}})));
        QVERIFY(!failed(call("connect", {{"from", "VCC.1"}, {"to", "R1.2"}})));
        QVERIFY(!failed(call("connect", {{"from", "VEE.1"}, {"to", "R1.1"}})));
        QVERIFY(!failed(call("save_document", {{"as", "rails"}})));
        QVERIFY(!failed(call("make_symbol")));
        QVERIFY(!failed(call("save_document")));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Sub"}, {"name", "X1"}, {"x", 400}, {"y", 300},
                                                                            {"properties", QJsonObject{{"File", "rails.sch"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 150}, {"properties", QJsonObject{{"U", "15 V"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V2"}, {"x", 100}, {"y", 450}, {"properties", QJsonObject{{"U", "15 V"}}}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "X1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V2.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V2.2"}, {"to", "X1.2"}}}},
            QJsonObject{{"tool", "add_analysis"}, {"arguments", QJsonObject{{"kind", "op"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(front()->getComponentByName("X1")->Ports.at(1)->Name == "VEE");
        const auto found = [this](const char* list) {
            return QJsonDocument(json(call("check_schematic")).toObject().value(list).toArray()).toJson(QJsonDocument::Compact);
        };
        // As drawn: VEE's pin at -15 V - as meant, nothing said.
        QString warnings = found("warnings"), notes = found("notes");
        QVERIFY2(warnings == "[]" && !notes.contains("negative supply"), qPrintable(warnings + notes));
        // VCC's source at -15 V: the wrong way round.
        QVERIFY(!failed(call("edit_component", {{"name", "V1"}, {"properties", QJsonObject{{"U", "-15 V"}}}})));
        warnings = found("warnings");
        QVERIFY2(warnings.contains("V1 puts X1.1 (VCC) at -15 V, though it is a positive supply's pin: the source is the wrong way round - "
                                   "set U to 15 V (edit_component)"),
                 qPrintable(warnings));
        QVERIFY(!failed(call("undo")));
        // VEE's at +15 V (its value negative, its + on ground).
        QVERIFY(!failed(call("edit_component", {{"name", "V2"}, {"properties", QJsonObject{{"U", "-15 V"}}}})));
        warnings = found("warnings");
        QVERIFY2(warnings.contains("V2 puts X1.2 (VEE) at 15 V, though it is a negative supply's pin: the source is the wrong way round - "
                                   "set U to 15 V (edit_component)"),
                 qPrintable(warnings));
        QVERIFY(!failed(call("undo")));
        QCOMPARE(found("warnings"), QString("[]"));
        // A net nothing names, below ground: the note, with the value that turns it.
        QVERIFY(!failed(call("add_component", {{"type", "Vdc"}, {"name", "V3"}, {"x", 700}, {"y", 300}, {"properties", QJsonObject{{"U", "5 V"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R9"}, {"x", 850}, {"y", 300}, {"rotation", 1}})));
        QVERIFY(!failed(call("connect", {{"from", "V3.1"}, {"to", "ground"}})));
        QVERIFY(!failed(call("connect", {{"from", "V3.2"}, {"to", "R9.2"}})));
        QVERIFY(!failed(call("connect", {{"from", "R9.1"}, {"to", "ground"}})));
        notes = found("notes");
        QVERIFY2(notes.contains("V3: its + is on ground, so V3.2 is at -5 V - a negative supply is so; if it was to be positive, set U to -5 V"),
                 qPrintable(notes));
        // Named vee: as meant. Named vcc: the wrong way round.
        QVERIFY(!failed(call("set_label", {{"at", "V3.2"}, {"name", "vee"}})));
        notes = found("notes");
        QVERIFY2(!notes.contains("V3:") && found("warnings") == "[]", qPrintable(notes));
        QVERIFY(!failed(call("undo")));
        QVERIFY(!failed(call("set_label", {{"at", "V3.2"}, {"name", "vcc"}})));
        warnings = found("warnings");
        QVERIFY2(warnings.contains("V3 puts vcc at -5 V, though that is a positive supply's name: the source is the wrong way round"), qPrintable(warnings));
        QVERIFY(!failed(call("undo")));
        // The source named as a negative supply (VNEG): as meant.
        QVERIFY(!failed(call("edit_component", {{"name", "V3"}, {"rename", "VNEG"}})));
        notes = found("notes");
        QVERIFY2(!notes.contains("negative supply is so"), qPrintable(notes));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The eighth round's small things: describe_part's bench numbers with
    // their units and what they are to be (a transistor's, one object per
    // bias point, read as 0); the instructions name only tools and
    // arguments there are.
    void theEighthRoundsSmallThings()
    {
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (QFileInfo::exists(library + "/OpAmps.lib")) {
            const QString was = QucsSettings.LibDir;
            QucsSettings.LibDir = library + "/";
            const auto tested = [this](const char* lib, const char* part) {
                return json(call("describe_part", {{"library", lib}, {"part", part}})).toObject().value("ngspice").toString();
            };
            const struct {
                const char *lib, *part, *pattern;
            } benches[] = {
                {"OpAmps", "uA741", "follower of 1 V: [0-9]\\.[0-9]{3} V \\(expected 1 V\\); gain of 11 of 0\\.5 V: [0-9]\\.[0-9]{3} V \\(expected 5\\.5 V\\)"},
                {"BJT_Extended", "2N2222", "at 9 uA: Vbe 0\\.[0-9]+ V \\(0\\.1 to 1\\.6 V\\), beta [0-9.]+ \\(3 to 5000\\), Vce [0-9.]+ V"},
                {"MOSFETs", "BSS123", "Id [0-9.]+ mA \\(1 to 10 mA\\)"},
                {"JFETs", "2N2608", "Id [0-9.]+ mA \\(0\\.001 to 10 mA\\)"},
                {"Diodes", "1N4148", "Vf 0\\.[0-9]+ V \\(0\\.1 to 4\\.5 V\\)"},
            };
            for (const auto& b : benches) {
                const QString said = tested(b.lib, b.part);
                QVERIFY2(said.contains("its bench passes") && said.contains(QRegularExpression(b.pattern)), qPrintable(said));
            }
            QucsSettings.LibDir = was;
        }
        // The instructions' guide: each 'argument' one some tool takes, each
        // name_with_underscores a tool or an argument.
        const QString instructions = control->instructions();
        const int guide = int(instructions.indexOf("How to work"));
        QVERIFY(guide > 0);
        QSet<QString> tools, arguments{QStringLiteral("max_chars")};
        const std::function<void(const QJsonObject&)> collect = [&](const QJsonObject& schema) {
            const QJsonObject properties = schema.value("properties").toObject();
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                arguments << it.key();
                collect(it.value().toObject());
                collect(it.value().toObject().value("items").toObject());
            }
        };
        for (const QJsonValue& t : control->tools()) {
            tools << t.toObject().value("name").toString();
            collect(t.toObject().value("inputSchema").toObject());
        }
        QStringList unknown;
        QRegularExpressionMatchIterator quoted = QRegularExpression("'([a-z_]+)'").globalMatch(instructions.mid(guide));
        while (quoted.hasNext())
            if (const QString a = quoted.next().captured(1); !arguments.contains(a)) unknown << "'" + a + "'";
        QRegularExpressionMatchIterator words = QRegularExpression("\\b([a-z]+_[a-z_]+)\\b").globalMatch(instructions.mid(guide));
        while (words.hasNext())
            if (const QString w = words.next().captured(1); !tools.contains(w) && !arguments.contains(w)) unknown << w;
        QVERIFY2(unknown.isEmpty(), qPrintable(unknown.join(", ")));
    }

    // The seventh round's small things: replace_component from named pins
    // to unnamed ones is refused, showing both (by number, INP went to the
    // uA741's output); the uA741's and AD825's pins have names now, LM3886's
    // come through its wrapper; 'near' says where the part went, and slides
    // it off what is there; find_library_component says what a one-line
    // part is placed as; an LM3886's rails are supplies.
    void theSeventhRoundsSmallThings()
    {
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        const auto names = [this](const QString& part) {
            QStringList n;
            for (const QJsonValue& p : json(call("get_schematic", {{"components", QJsonArray{part}}})).toObject().value("components").toArray()
                                           .first().toObject().value("pins").toArray())
                n << p.toObject().value("name").toString();
            return n;
        };
        for (const auto& [comp, want] : {std::pair("uA741", QStringList{"INN", "OUT", "INP", "VCC", "VEE"}),
                                         std::pair("AD825", QStringList{"INP", "INN", "VCC", "OUT", "VEE"}),
                                         std::pair("LM3886", QStringList{"POSIN", "NEGIN", "POSRAIL", "OUT", "NEGRAIL", "MUTE"})}) {
            QJsonObject r = call("add_component", {{"type", "Lib"}, {"name", QString("X_") + QString(comp).remove('(').remove(')')},
                                                   {"x", 1000}, {"y", 1000}, {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", comp}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            const QString n = json(r).toObject().value("name").toString();
            QCOMPARE(names(n), want);
            if (QString(comp) == "LM3886") {
                // Its rails on nothing: a supply warning, by their names.
                const QString checked = text(call("check_schematic"));
                QVERIFY2(checked.contains(n + ": its supply pins POSRAIL, NEGRAIL are on nothing that powers it"), qPrintable(checked));
            }
            QVERIFY(!failed(call("delete", {{"names", QJsonArray{n}}})));
        }
        // Named pins to unnamed ones: refused, both shown; by name or 'pins' it goes.
        QVERIFY(!failed(call("add_component", {{"type", "Lib"}, {"name", "U1"}, {"x", 300}, {"y", 300},
                                               {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(TI)"}}}})));
        for (const char* pin : {"U1.inn", "U1.inp", "U1.out", "U1.vcc", "U1.vee"})
            QVERIFY(!failed(call("set_label", {{"at", pin}, {"name", QString(pin).mid(3)}})));
        QJsonObject r = call("replace_component", {{"name", "U1"}, {"type", "Lib"}, {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(mod)"}}}});
        QVERIFY2(failed(r) && text(r).contains("U1's pins have names the new part's do not (INN, INP, OUT, VCC, VEE)")
                     && text(r).contains("1 INN (left, upper)") && text(r).contains("The new part's: 1 (left, upper), 2 (left, lower), 3 (right)")
                     && text(r).contains("\"by number\""),
                 qPrintable(text(r)));
        r = call("replace_component", {{"name", "U1"}, {"type", "Lib"}, {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "uA741"}}}});
        QVERIFY2(!failed(r) && json(r).toObject().value("mapped").toString() == "by name", qPrintable(text(r)));
        QStringList lines;
        for (const QJsonValue& v : json(call("get_netlist", {{"map", true}})).toObject().value("netlist").toArray()) lines << v.toString();
        QVERIFY2(!lines.filter(QRegularExpression("^XU1 ")).isEmpty()
                     && QRegularExpression("^XU1 0 inn out inp vcc vee ", QRegularExpression::CaseInsensitiveOption).match(lines.filter(QRegularExpression("^XU1 ")).first()).hasMatch(),
                 qPrintable(lines.filter(QRegularExpression("^XU1 ")).join('\n')));
        r = call("replace_component", {{"name", "U1"}, {"type", "Lib"}, {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(mod)"}}},
                                       {"pins", "by number"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("mapped").toString().startsWith("by number"), qPrintable(text(r)));
        QVERIFY(!failed(call("delete", {{"names", QJsonArray{"U1"}}})));

        // near: where it went; slid off a wire where it was to go.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "RA"}, {"x", 500}, {"y", 100}})));
        r = call("add_component", {{"type", "R"}, {"name", "RB"}, {"near", QJsonObject{{"part", "RA"}, {"side", "below"}, {"gap", 40}}}});
        QVERIFY2(!failed(r) && text(r).contains("Placed below RA, 40 from its symbol, centred on it"), qPrintable(text(r)));
        QVERIFY(!failed(call("delete", {{"names", QJsonArray{"RB"}}})));
        const int x = front()->getComponentByName("RA")->cx - 30;   // (RB's left pin, centred below)
        QVERIFY(!failed(call("add_wire", {{"points", QJsonArray{QJsonArray{x, 120}, QJsonArray{x, 250}}}})));
        r = call("add_component", {{"type", "R"}, {"name", "RB"}, {"near", QJsonObject{{"part", "RA"}, {"side", "below"}, {"gap", 40}}}});
        QVERIFY2(!failed(r) && text(r).contains("Placed below RA, 40 from its symbol, slid"), qPrintable(text(r)));
        const Component* rb = front()->getComponentByName("RB");
        for (const Port* p : rb->Ports) QVERIFY(rb->cx + p->x != x);
        r = call("edit_component", {{"name", "RB"}, {"near", QJsonObject{{"part", "RA"}, {"side", "above"}}}});
        QVERIFY2(!failed(r) && text(r).contains("Moved above RA, 40 from its symbol"), qPrintable(text(r)));

        // find_library_component: what a one-line part becomes.
        bool diode = false, led = false;
        for (const QJsonValue& v : json(call("find_library_component", {{"search", "1N4148"}})).toObject().value("found").toArray())
            if (v.toObject().value("component").toString() == "1N4148") diode = v.toObject().value("placed as").toString() == "Diode";
        for (const QJsonValue& v : json(call("find_library_component", {{"search", "light emitting diode"}, {"library", "LEDs"}})).toObject().value("found").toArray())
            led = led || v.toObject().contains("placed as");
        QucsSettings.LibDir = was;
        QVERIFY(diode);
        QVERIFY(!led);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // D3, after a run: ngspice writes a computed vector of a voltage's type
    // as v(name) (a NutmegEq's mag(v(out))) - its trace, named before the
    // run, finds it so.
    void aComputedVoltageIsFound()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", ".AC"}, {"name", "AC1"}, {"x", 100}, {"y", 300}})));
        QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/computed.sch")}})));
        writeFile("workspace/computed.dat.ngspice", "<Qucs Dataset " PACKAGE_VERSION ">\n<indep frequency 2>\n1\n10\n</indep>\n"
                                                    "<dep ac.v(mag_v_out) frequency>\n0.5\n0.25\n</dep>\n");
        const QJsonObject r = call("add_diagram", {{"x", 450}, {"y", 400}, {"traces", QJsonArray{"ngspice/ac.mag_v_out"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(!text(r).contains("no data"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // E3 and the minor notes: a marker on its trace, offsets in reason; a
    // pane's number whole; a region of the whole canvas; a wire of one
    // place; a place not given; two untitled documents named apart; a lone
    // ground as GND#1; an inactive ground grounds nothing; diff against a
    // file that is no schematic; tune's values without their zeros.
    void smallThingsAreRight()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "GND"}, {"x", 100}, {"y", 200}})));
        QVERIFY(failed(call("move_to_pane", {{"pane", 1.5}})));
        QVERIFY(!text(call("move_to_pane", {{"pane", 1.5}})).contains("pane 0"));
        QJsonObject r = call("screenshot", {{"region", QJsonArray{-2147483648.0, -2147483648.0, 2147483647.0, 2147483647.0}}});
        QVERIFY2(!failed(r) && !text(r).contains("nothing"), qPrintable(text(r)));
        QVERIFY(failed(call("add_wire", {{"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{0, 0}}}})));
        r = call("set_label", {{"at", QJsonValue()}, {"name", "a1"}});
        QVERIFY2(failed(r) && text(r).startsWith("Which place"), qPrintable(text(r)));
        r = call("get_schematic", {{"components", QJsonArray{"GND#1"}}});
        QVERIFY2(!text(r).contains("not found"), qPrintable(text(r)));
        QVERIFY(!failed(call("edit_component", {{"name", "GND"}, {"active", false}})));
        QVERIFY2(!text(call("get_schematic")).contains("\"gnd\""), "an inactive ground grounds nothing");
        const QString notSchematic = writeFile("workspace/notes.txt", "just text\n");
        r = call("diff", {{"against", notSchematic}});
        QVERIFY2(failed(r) && text(r).contains("not a schematic"), qPrintable(text(r)));
        QVERIFY(!failed(call("new_document", {{"kind", "text"}})));
        QStringList titles;
        for (const QJsonValue& d : json(call("get_state")).toObject().value("documents").toArray()) titles << d.toObject().value("title").toString();
        QVERIFY2(QSet<QString>(titles.cbegin(), titles.cend()).size() == titles.size(), qPrintable(titles.join(", ")));
        discardAll();
    }

    // The third round's small things: diff with no step back says so (not
    // "1 to 0"); a move by nothing tells what would move and moves nothing;
    // undo tells what it changed as that; add_analysis's expressions for a
    // simulator of its own, not only the settings'.
    void theThirdRoundsSmallThings()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("diff", {{"steps", 1}});
        QVERIFY2(failed(r) && text(r).contains("no step to go back to") && !text(r).contains("1 to 0"), qPrintable(text(r)));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 200}, {"y", 100}})));
        QVERIFY(!failed(call("connect", {{"from", "R1.2"}, {"to", "R2.1"}})));
        r = call("move", {{"names", QJsonArray{"R1"}}, {"dx", 0}, {"dy", 0}});
        QVERIFY2(!failed(r) && json(r).toObject().value("wires to the rest, drawn on to them").toInt() == 1, qPrintable(text(r)));
        QVERIFY(json(r).toObject().value("moved").toArray().isEmpty());
        QVERIFY(!failed(call("edit_component", {{"name", "R2"}, {"rotation", 1}})));
        r = call("undo");
        QVERIFY2(text(r).contains("The undo changed:") && !text(r).contains("Now:"), qPrintable(text(r)));
        const int was = QucsSettings.DefaultSimulator;
        QucsSettings.DefaultSimulator = spicecompat::simQucsator;
        r = call("add_analysis", {{"kind", "ac"}, {"plot", QJsonArray{"db(v(out))"}}});
        QVERIFY2(failed(r) && text(r).contains("'simulator'"), qPrintable(text(r)));
        r = call("add_analysis", {{"kind", "ac"}, {"plot", QJsonArray{"db(v(out))"}}, {"simulator", "ngspice"}});
        QucsSettings.DefaultSimulator = was;
        QVERIFY2(!failed(r), qPrintable(text(r)));
        bool nutmeg = false;
        for (Component* c : front()->a_DocComps) nutmeg = nutmeg || c->Model == "NutmegEq";
        QVERIFY(nutmeg);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // An imported netlist's nets are labels on every pin: arranged with
    // wire_labels, its pieces are wired together, one label kept for each
    // name - every net as it was.
    void anImportedNetlistIsWiredWhenAsked()
    {
        QJsonObject r = call("import_netlist", {{"text", "* rc\nV1 in 0 DC 0 AC 1\nR1 in mid 1k\nC1 mid 0 1n\nR2 mid out 1k\n"
                                                         "C2 out 0 1n\n.ac dec 10 1 1meg\n.end"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("arrange", {{"wire_labels", true}});
        QVERIFY2(!failed(r) && text(r).contains("every net as it was"), qPrintable(text(r)));
        const QJsonObject read = json(call("get_schematic")).toObject();
        QStringList names;
        for (const QJsonValue& l : read.value("labels").toArray()) names << l.toObject().value("net").toString();
        names.sort();
        QCOMPARE(names, (QStringList{"in", "mid", "out"}));
        QVERIFY2(read.value("wires").toArray().size() >= 5, qPrintable(text(call("get_schematic"))));
        const QString checked = text(call("check_schematic"));
        QVERIFY2(checked.contains("Nothing found"), qPrintable(checked));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Every tool listed is one call() runs - the tool list, its tables and
    // the dispatch are kept apart, and nothing else says they agree: each
    // called with nothing gets its own answer (mostly: what it needs), not
    // "There is no tool".
    void everyListedToolIsDispatched()
    {
        QStringList names;
        for (const QJsonValue& v : control->tools()) names << v.toObject().value("name").toString();
        // describe_tool lists each one's summary - the tools not in every
        // turn with theirs too, not their whole description.
        for (const QJsonValue& v : json(call("describe_tool")).toObject().value("tools").toArray())
            if (v.toObject().value("name").toString() == "tune")
                QVERIFY2(v.toObject().value("summary").toString().size() < 300, qPrintable(v.toObject().value("summary").toString()));
        for (const QString& name : std::as_const(names)) {
            if (name == "close_document" || name == "clean_scratch") continue;   // (on the document in front: nothing to see)
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            const QJsonObject r = call(name, {}, 60000);
            QVERIFY2(!text(r).startsWith("There is no tool"), qPrintable(name + ": " + text(r).left(200)));
            discardAll();
        }
    }

    // An argument a tool does not take is refused, with those it takes and
    // the one meant - it was left out, and the call done without it. A
    // painting's fields are its type's (the tools refuse the rest). A text
    // box keeps the width it was given alone; a tip makes it point.
    void unknownArgumentsAreRefused()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}, {"rotaton", 1}});
        QVERIFY2(failed(r) && text(r).contains("takes no rotaton") && text(r).contains("Meant rotation?"), qPrintable(text(r)));
        QVERIFY(front()->getComponentByName("R1") == nullptr);
        QVERIFY(failed(call("get_state", {{"verbose", true}})));
        QVERIFY(!failed(call("get_state", {{"path", "anything"}})));   // (as a pinned conversation names its schematic)
        QVERIFY2(text(call("zoom", {{"to", "all"}, {"preview", true}})).contains("takes no preview"), "preview is of the tools that change");
        r = call("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "add_component"},
                                                            {"arguments", QJsonObject{{"type", "C"}, {"x", 0}, {"y", 0}, {"colour", "red"}}}}}}});
        QVERIFY2(failed(r) || text(r).contains("takes no colour"), qPrintable(text(r)));
        // Paintings: their type's fields, all of them.
        r = call("add_painting", {{"type", "text_box"}, {"x", 100}, {"y", 300}, {"width", 180}, {"text", "hello"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("added").toObject().value("width").toInt(), 180);
        r = call("add_painting", {{"type", "text_box"}, {"x", 100}, {"y", 500}, {"text", "there"}, {"tip", QJsonArray{50, 600}}});
        QVERIFY2(json(r).toObject().value("added").toObject().value("pointer").toBool(), qPrintable(text(r)));
        r = call("add_painting", {{"type", "text_box"}, {"x", 100}, {"y", 700}, {"text", "x"}, {"sides", 5}});
        QVERIFY2(failed(r) && text(r).contains("has no sides"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A dataset that is not of the circuit as it is says so: after a value
    // changed (the netlist now is not the one its run was given), after a
    // run that failed - not after a diagram or a move.
    void aStaleDatasetIsSaid()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "GND"}, {"x", 100}, {"y", 300}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", ".TR"}, {"name", "TR1"}, {"x", 300}, {"y", 100}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "GND.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "GND.1"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.1"}, {"name", "top"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", "stale"}, {"replace", true}})));
        QVERIFY2(json(call("simulate", {}, 120000)).toObject().value("succeeded").toBool(), "the first run");
        const auto stale = [this] {
            const QJsonObject r = call("get_dataset");
            return failed(r) ? QStringLiteral("(failed) ") + text(r) : json(r).toObject().value("stale").toString();
        };
        // Whether the verdict is known (the netlists compared, a run failed)
        // or only the time of an edit tells.
        const auto certain = [this] { return json(call("get_dataset")).toObject().value("stale certain"); };
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"tran.i(V1)"}}})));
        QVERIFY(!failed(call("move", {{"names", QJsonArray{"R1"}}, {"dx", 0}, {"dy", 20}})));
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "2k"}}}})));
        QVERIFY2(stale().contains("changed since the run"), qPrintable(stale()));
        QCOMPARE(certain(), QJsonValue(true));
        QVERIFY2(json(call("simulate", {}, 120000)).toObject().value("succeeded").toBool(), "the second run");
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        // A preview that changes nothing is no edit.
        QTest::qWait(1100);
        QVERIFY(!failed(call("set_schematic", {{"text", front()->documentText()}, {"preview", true}})));
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        // A copy, with the dataset and no run of its own: the netlist its
        // run was given comes with it, and tells exactly - a move is no
        // change, a value is (the time alone said "edited" of both).
        QJsonObject copied = call("copy_document", {{"to", "stalecopy"}, {"replace", true}});
        QVERIFY2(!failed(copied), qPrintable(text(copied)));
        QVERIFY(!failed(call("open_document", {{"path", "stalecopy.sch"}})));
        QTest::qWait(1100);   // (past the second the copied dataset was written in)
        QVERIFY(!failed(call("move", {{"names", QJsonArray{"R1"}}, {"dx", 0}, {"dy", 20}, {"preview", true}})));
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        QVERIFY(!failed(call("move", {{"names", QJsonArray{"R1"}}, {"dx", 0}, {"dy", 20}})));
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "3k"}}}})));
        QVERIFY2(stale().contains("changed since the run"), qPrintable(stale()));
        QCOMPARE(certain(), QJsonValue(true));
        // A dataset written otherwise than by a run here (touched, copied
        // in): no netlist is known for it, and an edit after it is told by
        // its time alone - as not certain.
        {
            QFile touched(QFileInfo(front()->getDocName()).absoluteDir().filePath("stalecopy.dat.ngspice"));
            QVERIFY(touched.open(QIODevice::ReadWrite));
            QVERIFY(touched.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime));
        }
        QTest::qWait(1100);
        QVERIFY(!failed(call("move", {{"names", QJsonArray{"R1"}}, {"dx", 0}, {"dy", 20}})));
        QVERIFY2(stale().contains("not at hand to compare"), qPrintable(stale()));
        QCOMPARE(certain(), QJsonValue(false));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("show_document", {{"path", "stale.sch"}})));
        // A value tune tries and puts back ('apply': false) is no edit, yet
        // the dataset is its last trial's: said, and certain.
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        const QJsonObject tuned = call("tune", {{"component", "R1"}, {"measure", QJsonObject{{"variable", "tran.i(V1)"}, {"what", "final"}}},
                                                {"target", -0.25e-3}, {"range", QJsonArray{"1k", "10k"}}, {"apply", false}}, 240000);
        QVERIFY2(!failed(tuned), qPrintable(text(tuned)));
        QVERIFY2(stale().contains("changed since the run"), qPrintable(stale() + " | " + text(tuned)));
        QCOMPARE(certain(), QJsonValue(true));
        QVERIFY2(json(call("simulate", {}, 120000)).toObject().value("succeeded").toBool(), "the run after tune");
        QVERIFY2(stale().isEmpty(), qPrintable(stale()));
        QVERIFY(!failed(call("edit_component", {{"name", "R1"}, {"properties", QJsonObject{{"R", "{nosuchparam}"}}}})));
        QVERIFY(!json(call("simulate", {}, 120000)).toObject().value("succeeded").toBool());
        QVERIFY2(stale().contains("failed"), qPrintable(stale()));
        QCOMPARE(certain(), QJsonValue(true));
        QucsSettings.NgspiceExecutable = before;
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The subcircuit path as make_symbol's description gives it: grounds
    // told alike in the netlist map and everywhere else; a ground on a wire
    // stub to the group goes with it; a net that goes inside and an
    // equation names is told; the symbol drawn and saved, the instance's
    // pins take their labels along - the parent still netlists whole.
    void theSubcircuitPathKeepsTheParentWhole()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vac"}, {"name", "V1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 100}, {"rotation", 1}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "C"}, {"name", "C1"}, {"x", 350}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "GND"}, {"x", 100}, {"y", 300}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "GND"}, {"x", 350}, {"y", 300}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", ".AC"}, {"name", "AC1"}, {"x", 500}, {"y", 100}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "C1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "GND#1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "C1.2"}, {"to", "GND#2.1"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "C1.1"}, {"name", "out"}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "NutmegEq"}, {"name", "NutmegEq1"}, {"x", 500}, {"y", 300},
                                                                              {"equations", QJsonArray{"gain=db(v(out))"}}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // The netlist map names the grounds as get_schematic's refs do.
        QStringList refs;
        for (const QJsonValue& c : json(call("get_schematic")).toObject().value("components").toArray())
            if (c.toObject().contains("ref")) refs << c.toObject().value("ref").toString();
        QCOMPARE(refs, (QStringList{"GND#1", "GND#2"}));
        const QString map = text(call("get_netlist", {{"map", true}}));
        QVERIFY2(map.contains("\"GND#1.1\"") && map.contains("\"GND#2.1\"") && !map.contains("\"GND.1\""), qPrintable(map));
        QDir().mkpath(dir.filePath("workspace/subpath"));
        QVERIFY(!failed(call("save_document", {{"as", dir.filePath("workspace/subpath/top.sch")}})));
        r = call("create_subcircuit", {{"names", QJsonArray{"R1", "C1"}}, {"name", "LP1"}, {"save_as", "lp.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("inside now").toString().contains("gain=db(v(out)) in NutmegEq1"), qPrintable(text(r)));
        QString checked = text(call("check_schematic"));
        QVERIFY2(!checked.contains("connected to nothing"), qPrintable(checked));
        // The symbol drawn and saved: the instance's pin moves, its label with it.
        QVERIFY(!failed(call("open_document", {{"path", dir.filePath("workspace/subpath/lp.sch")}})));
        QVERIFY(!failed(call("make_symbol")));
        // (Its pin put in the open: where make_symbol puts it may be on a wire.)
        int port = 0, px = 0, py = 0;
        for (const QJsonValue& p : json(call("get_schematic")).toObject().value("symbol paintings").toArray())
            if (p.toObject().value("type").toString() == "port") {
                port = p.toObject().value("painting").toInt();
                px = p.toObject().value("x").toInt();
                py = p.toObject().value("y").toInt();
            }
        QVERIFY(port > 0);
        r = call("edit_painting", {{"painting", port}, {"x", px - 60}, {"y", py - 60}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("save_document");
        QVERIFY2(text(r).contains("'s label lp_n1 went with it") && text(r).contains("Every net is as it was"), qPrintable(text(r)));
        const QString top = dir.filePath("workspace/subpath/top.sch");
        checked = text(call("check_schematic", {{"path", top}}));
        QVERIFY2(!checked.contains("connected to nothing"), qPrintable(checked));
        const QString netlist = text(call("get_netlist", {{"path", top}}));
        QVERIFY2(netlist.contains(QRegularExpression("\\nXLP1 lp_n1 ", QRegularExpression::CaseInsensitiveOption)), qPrintable(netlist));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", top}, {"unsaved", "discard"}})));
    }

    // A schematic copied outside Qucs-S keeps the dataset and data display
    // names of the one it was copied from: said when it is opened, and
    // named after it on request. Its runs write where its DataSet says,
    // where get_dataset and its diagrams read (the runs wrote after its
    // file's name, where nothing read them).
    void aCopyMadeOutsideQucsIsToldOfItsDataNames()
    {
        QDir().mkpath(dir.filePath("workspace/copied"));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "GND"}, {"x", 100}, {"y", 300}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", ".DC"}, {"name", "DC1"}, {"x", 300}, {"y", 100}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "GND.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "GND.1"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString original = dir.filePath("workspace/copied/rc.sch"), copy = dir.filePath("workspace/copied/rc_copy.sch");
        QVERIFY(!failed(call("save_document", {{"as", original}})));
        QVERIFY(!failed(call("close_document")));
        QVERIFY(QFile::copy(original, copy));
        r = call("open_document", {{"path", copy}});
        QVERIFY2(!failed(r) && text(r).contains("not named after it") && text(r).contains("rc.dat.ngspice")
                     && text(r).contains("rc_copy.dat"), qPrintable(text(r)));
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        const QString before = QucsSettings.NgspiceExecutable;
        if (!ngspice.isEmpty()) {
            QucsSettings.NgspiceExecutable = ngspice;
            r = call("simulate", {}, 120000);
            QVERIFY2(json(r).toObject().value("dataset").toString().endsWith("rc.dat.ngspice"), qPrintable(text(r)));
            QVERIFY2(json(r).toObject().value("dataset written").toBool(), qPrintable(text(r)));
            r = call("get_dataset");
            QVERIFY2(json(r).toObject().value("dataset").toString().endsWith("rc.dat.ngspice"), qPrintable(text(r)));
        }
        // Named after it, on request.
        r = call("open_document", {{"path", copy}, {"own_data_names", true}});
        QVERIFY2(text(r).contains("named after it now"), qPrintable(text(r)));
        QCOMPARE(json(call("get_schematic")).toObject().value("settings").toObject().value("dataset").toString(), QStringLiteral("rc_copy.dat"));
        QVERIFY2(!text(call("open_document", {{"path", copy}})).contains("not named after it"), "told once it is right");
        if (!ngspice.isEmpty()) {
            r = call("simulate", {}, 120000);
            QVERIFY2(json(r).toObject().value("dataset").toString().endsWith("rc_copy.dat.ngspice")
                         && QFileInfo::exists(dir.filePath("workspace/copied/rc_copy.dat.ngspice")), qPrintable(text(r)));
        }
        QucsSettings.NgspiceExecutable = before;
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // ngspice's commands summed up: every one in a line by category, one
    // in full (syntax, Qucs-S's way, an example), a search, a category -
    // and what the ngspice of the settings has, asked of it: what it lacks
    // marked, what it has besides told. One that cannot be asked is said.
    void ngspiceCommandsAreSummedUp()
    {
        QJsonObject r = call("ngspice_commands");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        for (const QString& part : {QStringLiteral("analysis - Analyses"), QStringLiteral("  tran: "), QStringLiteral("  hb*: "),
                                    QStringLiteral("  montecarlo*: "), QStringLiteral("  foreach: "), QStringLiteral("Nutmeg script"),
                                    QStringLiteral("could not be asked")})
            QVERIFY2(text(r).contains(part), qPrintable(part + "\n" + text(r)));
        r = call("ngspice_commands", {{"command", ".TRAN"}});
        QVERIFY2(text(r).contains("Syntax: tran <tstep> <tstop>") && text(r).contains(".TR") && text(r).contains("Example:"), qPrintable(text(r)));
        r = call("ngspice_commands", {{"command", QJsonArray{"meas", "pre_set", "tarn", "sprocket"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(text(r).contains("Syntax: meas ") && text(r).contains("pre_ runs set before"), qPrintable(text(r)));
        QVERIFY2(text(r).contains("no command tarn") && text(r).contains("Near it: tran") && text(r).contains("no command sprocket"), qPrintable(text(r)));
        r = call("ngspice_commands", {{"search", "stability"}});
        QVERIFY2(text(r).contains("  stb*") && text(r).contains("  rfstab*") && !text(r).contains("  ac:"), qPrintable(text(r)));
        r = call("ngspice_commands", {{"search", "touchstone"}});
        QVERIFY2(text(r).contains("wrsnp") && !text(r).contains("codemodel"), qPrintable(text(r)));
        r = call("ngspice_commands", {{"category", "rf"}});
        QVERIFY2(text(r).contains("  sp dec|oct|lin") && !text(r).contains("  tran"), qPrintable(text(r)));
        QVERIFY(failed(call("ngspice_commands", {{"category", "sprockets"}})));
        QVERIFY(failed(call("ngspice_commands", {{"command", 5}})));
        QVERIFY(failed(call("ngspice_commands", {{"command", QJsonArray{"tran", 5}}})));
        // The same as a resource.
        QString error;
        const QJsonArray contents = control->readResource("qucs://ngspice-commands", &error);
        QVERIFY2(!contents.isEmpty() && contents.first().toObject().value("text").toString().contains("analysis - Analyses"), qPrintable(error));
        bool listed = false;
        for (const QJsonValue& v : control->resources()) listed = listed || v.toObject().value("uri").toString() == "qucs://ngspice-commands";
        QVERIFY(listed);
#ifdef Q_OS_WIN
        QSKIP("A shell script stands in for ngspice.");
#else
        // An ngspice with two of the commands, and one of its own.
        const QString script = dir.filePath("few-commands-ngspice.sh");
        {
            QFile f(script);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\ncat > /dev/null\n"
                    "echo '** ngspice-99 : Circuit level simulation program'\n"
                    "echo 'ac [.ac line args] : Do an ac analysis.'\n"
                    "echo 'tran [.tran line args] : Do a transient analysis.'\n"
                    "echo 'frob x y : Frobnicate a circuit.'\n");
        }
        QVERIFY(QFile::setPermissions(script, QFile::permissions(script) | QFileDevice::ExeOwner | QFileDevice::ExeUser));
        const QString was = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = script;
        r = call("ngspice_commands");
        const QString all = text(r);
        r = call("ngspice_commands", {{"command", QJsonArray{"tran", "hb", "frob"}}});
        const QString some = text(r);
        r = call("ngspice_commands", {{"search", "frobnicate"}});
        const QString found = text(r);
        QucsSettings.NgspiceExecutable = was;
        QVERIFY2(all.contains("ngspice-99") && all.contains("has 2 of the 165") && all.contains("it lacks") && all.contains("It also has frob"),
                 qPrintable(all));
        QVERIFY2(all.contains("  hb* [not in this ngspice]: ") && all.contains("  tran: "), qPrintable(all));
        QVERIFY2(some.contains("This ngspice's help: tran [.tran line args] : Do a transient analysis."), qPrintable(some));
        QVERIFY2(some.contains("hb (RF and periodic steady state; the enhanced build's, not stock ngspice's; not in this ngspice)"), qPrintable(some));
        QVERIFY2(some.contains("frob (this ngspice's; not described here)"), qPrintable(some));
        QVERIFY2(found.contains("frob (this ngspice's): frob x y : Frobnicate a circuit."), qPrintable(found));
#endif
    }

    // The fourth round's arguments: what a handler reads is in its schema
    // (replace_component's records and specs, redo's 'to'); an unknown key
    // inside an argument - a set_schematic part's, a wire's, a trace's - is
    // refused as one at the top is; a name that is no file's is refused
    // before anything is written.
    void theFourthRoundsArguments()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", ".NGMONTECARLO"}, {"name", "MC1"}, {"x", 100}, {"y", 100}})));
        QJsonObject r = call("replace_component", {{"name", "MC1"}, {"type", ".NGMONTECARLO"},
                                                    {"records", QJsonArray{"gain|db(v(out))"}}, {"specs", QJsonArray{"gain|19|21"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString mc = text(call("get_schematic", {{"format", "text"}}));
        QVERIFY2(mc.contains("db(v(out))") && mc.contains("19"), qPrintable(mc));
        // redo to a step, as undo goes to one.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 300}, {"y", 100}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 400}, {"y", 100}})));
        const int at = front()->undoIndex();
        QVERIFY(!failed(call("undo", {{"steps", 2}})));
        QVERIFY(front()->getComponentByName("R1") == nullptr);
        r = call("redo", {{"to", at}});
        QVERIFY2(!failed(r) && front()->getComponentByName("R2") != nullptr, qPrintable(text(r)));
        // Inside an argument.
        const int parts = int(front()->a_DocComps.size());
        r = call("set_schematic", {{"components", QJsonArray{QJsonObject{{"type", "R"}, {"name", "R9"}, {"x", 600}, {"y", 100}, {"rotaton", 1}}}}});
        QVERIFY2(failed(r) && text(r).contains("components[0] (R9) has no rotaton") && text(r).contains("Meant rotation?"), qPrintable(text(r)));
        QCOMPARE(int(front()->a_DocComps.size()), parts);
        r = call("set_schematic", {{"components", QJsonArray{QJsonObject{{"type", "R"}, {"name", "R9"}, {"x", 600}, {"y", 100}}}},
                                   {"wires", QJsonArray{QJsonObject{{"from", QJsonArray{0, 0}}, {"to", QJsonArray{0, 50}}, {"lable", "x"}}}}});
        QVERIFY2(failed(r) && text(r).contains("wires[0] has no lable") && text(r).contains("Meant label"), qPrintable(text(r)));
        r = call("add_diagram", {{"type", "rect"}, {"traces", QJsonArray{"v(out)", QJsonObject{{"variable", "v(in)"}, {"colour", "#ff0000"}}}}});
        QVERIFY2(failed(r) && text(r).contains("traces[1] has no colour") && text(r).contains("Meant color?"), qPrintable(text(r)));
        r = call("add_diagram", {{"type", "rect"}, {"x_axis", QJsonObject{{"lable", "f"}}}});
        QVERIFY2(failed(r) && text(r).contains("x_axis has no lable"), qPrintable(text(r)));
        r = call("delete", {{"traces", QJsonArray{QJsonObject{{"diagram", 1}, {"trce", 1}}}}});
        QVERIFY2(failed(r) && text(r).contains("traces[0] has no trce"), qPrintable(text(r)));
        // The JSON form get_schematic gives is taken back whole.
        const QJsonObject form = json(call("get_schematic", {{"format", "json"}})).toObject();
        r = call("set_schematic", {{"components", form.value("components")}, {"wires", form.value("wires")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // No file's name.
        for (const QString& bad : {QStringLiteral("\""), QStringLiteral("'"), QStringLiteral("{}"), QStringLiteral("<Components>"), QStringLiteral("NUL"),
                                   QStringLiteral("a:b"), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("sub/..")}) {
            r = call("save_document", {{"as", bad}});
            QVERIFY2(failed(r) && (text(r).contains("no file's name") || text(r).contains("is a folder")), qPrintable(bad + ": " + text(r)));
            r = call("new_project", {{"name", bad}, {"open", false}});
            QVERIFY2(failed(r) && (text(r).contains("no file's name") || text(r).contains("has a slash") || text(r).contains("no project's name")),
                     qPrintable(bad + ": " + text(r)));
        }
        // Each said as it is (sixth round): "." has no letter or digit, not
        // a slash; a name beginning with a dot would be hidden.
        r = call("new_project", {{"name", "."}, {"open", false}});
        QVERIFY2(failed(r) && text(r).contains("no letter or digit") && !text(r).contains("slash"), qPrintable(text(r)));
        r = call("new_project", {{"name", ".amp"}, {"open", false}});
        QVERIFY2(failed(r) && text(r).contains("begins with a dot"), qPrintable(text(r)));
        r = call("new_project", {{"name", "a/b"}, {"open", false}});
        QVERIFY2(failed(r) && text(r).contains("has a slash"), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(dir.filePath("workspace/{}_prj")) && !QFileInfo::exists(dir.filePath("workspace/'.sch")));
        QVERIFY(!QFileInfo::exists(dir.filePath("workspace.sch")));   // (".": the workspace, and .sch after it - beside it)
        r = call("export_netlist", {{"save_as", "?.cir"}});
        QVERIFY2(failed(r) && text(r).contains("no file's name"), qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // Foo.sch's foo.dat is its own where the file system does not tell case
    // (macOS's, Windows's): not told as a copy's.
    void aDatasetNamedInAnotherCaseIsItsOwn()
    {
        const QString file = dir.filePath("workspace/CaseDiv.sch");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <DataSet=casediv.dat>\n  <DataDisplay=casediv.dpl>\n"
                    "</Properties>\n<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n"
                    "<Paintings>\n</Paintings>\n");
        }
        const bool caseBlind = QFileInfo::exists(dir.filePath("workspace/casediv.sch"));
        const QJsonObject r = call("open_document", {{"path", file}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(text(r).contains("not named after it"), !caseBlind);
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A preview is no edit: its schematic's revision, last edit and edits
    // are as they were after it - a preview that changes nothing does not
    // make the dataset stale, nor tell the edit as "yours".
    void aPreviewIsNoEdit()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", "previewed"}, {"replace", true}})));
        Schematic* sch = front();
        const quint64 revision = sch->revision();
        const int edits = int(sch->recentEdits().size());
        const QDateTime last = sch->recentEdits().isEmpty() ? QDateTime() : sch->recentEdits().last().at;
        QTest::qWait(20);
        QJsonObject r = call("set_schematic", {{"text", sch->documentText()}, {"preview", true}});
        QVERIFY2(!failed(r) && json(r).toObject().value("would change").toArray().isEmpty(), qPrintable(text(r)));
        QCOMPARE(sch->revision(), revision);
        QCOMPARE(int(sch->recentEdits().size()), edits);
        QVERIFY(sch->recentEdits().isEmpty() || sch->recentEdits().last().at == last);
        r = call("add_component", {{"type", "C"}, {"name", "C1"}, {"x", 300}, {"y", 100}, {"preview", true}});
        QVERIFY2(!failed(r) && !json(r).toObject().value("would change").toArray().isEmpty(), qPrintable(text(r)));
        QCOMPARE(sch->revision(), revision);
        QCOMPARE(int(sch->recentEdits().size()), edits);
        QVERIFY(sch->getComponentByName("C1") == nullptr);
        // A batch previewed, over several turns: the same.
        r = call("batch", {{"calls", QJsonArray{QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "C"}, {"x", 300}, {"y", 200}}}},
                                                QJsonObject{{"tool", "move"}, {"arguments", QJsonObject{{"names", QJsonArray{"R1"}}, {"dx", 0}, {"dy", 40}}}}}},
                           {"preview", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(sch->revision(), revision);
        QCOMPARE(control->resourceVersion("qucs://schematic/" + QString::fromLatin1(QUrl::toPercentEncoding(sch->getDocName()))), QString::number(revision));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A subcircuit made in place of parts with wires to them: its pins land
    // on the wires' ends, and the symbol drawn after moves them - the
    // wires are drawn on to where they are now, and the parent netlists
    // whole (the round-3 fix carried a label on a pin alone only).
    void aSubcircuitPinOnAWireIsWiredOn()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vac"}, {"name", "V1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "C"}, {"name", "C1"}, {"x", 400}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "GND"}, {"x", 100}, {"y", 300}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "GND"}, {"x", 400}, {"y", 300}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "C1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "GND#1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "C1.2"}, {"to", "GND#2.1"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.1"}, {"name", "in"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.2"}, {"name", "out"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // (R1 lying: the subcircuit's pins land on its wires' ends.)
        QDir().mkpath(dir.filePath("workspace/wiredon"));
        const QString top = dir.filePath("workspace/wiredon/lowpass.sch");
        QVERIFY(!failed(call("save_document", {{"as", top}})));
        r = call("create_subcircuit", {{"names", QJsonArray{"R1"}}, {"name", "SUB1"}, {"save_as", "lpw.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document")));
        QVERIFY(!failed(call("open_document", {{"path", dir.filePath("workspace/wiredon/lpw.sch")}})));
        QVERIFY(!failed(call("make_symbol")));
        r = call("save_document");
        QVERIFY2(!failed(r) && text(r).contains("Their pins moved") && text(r).contains("wired on from") && text(r).contains("Every net is as it was"),
                 qPrintable(text(r)));
        QString checked = text(call("check_schematic", {{"path", top}}));
        QVERIFY2(!checked.contains("connected to nothing"), qPrintable(checked));
        const QString netlist = text(call("get_netlist", {{"path", top}}));
        QVERIFY2(netlist.contains(QRegularExpression("\\nXSUB1 in out ", QRegularExpression::CaseInsensitiveOption)), qPrintable(netlist));
        // One step to undo there puts the pins' wiring back as it was.
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", top}, {"unsaved", "discard"}})));
    }

    // Two parts of the subcircuit on one net that goes on outside: the wire
    // between them stays outside (it goes on to C1), so inside each takes
    // the port's label - R2 was on nothing there, and the divider read 5 V
    // (the end-to-end scenarios' s2).
    void aSubcircuitKeepsPinsWhoseWireGoesOn()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 220}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R2"}, {"x", 340}, {"y", 200}, {"rotation", 1}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "C"}, {"name", "C1"}, {"x", 460}, {"y", 200}, {"rotation", 1}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "R2.2"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R2.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "C1.2"}, {"to", "R2.2"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "C1.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.2"}, {"name", "mid"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "V1.1"}, {"name", "vin"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QDir().mkpath(dir.filePath("workspace/goeson"));
        const QString top = dir.filePath("workspace/goeson/div_top.sch");
        QVERIFY(!failed(call("save_document", {{"as", top}})));
        r = call("create_subcircuit", {{"names", QJsonArray{"R1", "R2"}}, {"save_as", "div.sch"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("open_document", {{"path", dir.filePath("workspace/goeson/div.sch")}})));
        const QString checked = text(call("check_schematic"));
        QVERIFY2(!checked.contains("connected to nothing"), qPrintable(checked));
        const QString netlist = text(call("get_netlist", {{"path", top}}));
        QVERIFY2(netlist.contains(QRegularExpression("\\nR2 0 mid ")) && netlist.contains(QRegularExpression("\\nR1 vin mid ")),
                 qPrintable(netlist));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", top}, {"unsaved", "discard"}})));
    }

    // The divider's R1 and R2 in one row, C1 on the wire between them: the
    // wire went with neither, and the instance put where they were landed
    // both its pins on it (XSUB1 mid mid, v(mid) = 0) - with the net's label
    // on R2.1, on C1's pin, or none (sixth round, 2.1). The wire between
    // them goes now, the ports are joined by labels, and the instance is
    // where none of its pins meets anything.
    void aDividerInARowKeepsItsNets()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        const QString parts = "<Components>\n"
                              "  <Vdc V1 1 100 200 18 -26 0 1 \"5 V\" 1>\n"
                              "  <GND * 1 100 230 0 0 0 0>\n"
                              "  <R R1 1 200 100 -26 15 0 0 " + R +
                              "  <R R2 1 300 100 -26 15 0 0 " + R +
                              "  <GND * 1 400 100 0 0 0 0>\n"
                              "  <C C1 1 250 200 17 -26 0 1 \"1 uF\" 1 \"\" 0 \"neutral\" 0>\n"
                              "  <GND * 1 250 230 0 0 0 0>\n"
                              "  <.DC DC1 1 100 400 0 43 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
                              "</Components>\n";
        const struct {
            const char* name;
            QString labels;   // (a label as a wire of no length)
            QString more;     // (parts besides)
        } variants[] = {{"none", "", ""},
                        {"on R2.1", "  <270 100 270 100 \"mid\" 280 70 0 \"\">\n", ""},
                        {"on C1's pin", "  <250 170 250 170 \"mid\" 260 140 0 \"\">\n", ""},
                        // Another net's wire where the instance's first pin
                        // would be, put where the group was: it goes
                        // elsewhere (it joined V1's net to ground).
                        {"a ground wire where its pin would be", "  <220 40 220 160 \"\" 0 0 0 \"\">\n",
                         "  <GND * 1 220 40 0 0 1 0>\n  <GND * 1 220 160 0 0 0 0>\n"}};
        int n = 0;
        for (const auto& v : variants) {
            ++n;
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            QJsonObject r = call("set_schematic", {{"text", QString(parts).replace("</Components>", v.more + "</Components>") + "<Wires>\n"
                                                                   "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
                                                                   "  <230 100 250 100 \"\" 0 0 0 \"\">\n  <250 100 270 100 \"\" 0 0 0 \"\">\n"
                                                                   "  <250 100 250 170 \"\" 0 0 0 \"\">\n  <330 100 400 100 \"\" 0 0 0 \"\">\n"
                                                    + v.labels + "</Wires>\n"}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QDir().mkpath(dir.filePath(QStringLiteral("workspace/inarow%1").arg(n)));
            const QString top = dir.filePath(QStringLiteral("workspace/inarow%1/top.sch").arg(n));
            QVERIFY(!failed(call("save_document", {{"as", top}})));
            r = call("create_subcircuit", {{"names", QJsonArray{"R1", "R2"}}, {"save_as", "div.sch"}});
            QVERIFY2(!failed(r), qPrintable(QStringLiteral("%1: %2").arg(v.name, text(r))));
            // Two nets, each as it was: V1's and C1's.
            const QString netlist = text(call("get_netlist", {{"path", top}}));
            const QRegularExpressionMatch x = QRegularExpression("\\nXSUB1 (\\S+) (\\S+) ").match(netlist);
            QVERIFY2(x.hasMatch() && x.captured(1) != x.captured(2), qPrintable(QStringLiteral("%1: %2").arg(v.name, netlist)));
            QVERIFY2(QRegularExpression(QStringLiteral("\\nV1 %1 0 ").arg(QRegularExpression::escape(x.captured(1)))).match(netlist).hasMatch(),
                     qPrintable(QStringLiteral("%1: %2").arg(v.name, netlist)));
            QVERIFY2(QRegularExpression(QStringLiteral("\\nC1 0 %1 |\\nC1 %1 0 ").arg(QRegularExpression::escape(x.captured(2)))).match(netlist).hasMatch(),
                     qPrintable(QStringLiteral("%1: %2").arg(v.name, netlist)));
            const QString checked = text(call("check_schematic"));
            QVERIFY2(!checked.contains("connected to nothing"), qPrintable(QStringLiteral("%1: %2").arg(v.name, checked)));
            // (Nor on another net's wire, unjoined: it looked joined.)
            QVERIFY2(!checked.contains("without being connected to it"), qPrintable(QStringLiteral("%1: %2").arg(v.name, checked)));
            if (!ngspice.isEmpty()) {
                const QString before = QucsSettings.NgspiceExecutable;
                QucsSettings.NgspiceExecutable = ngspice;
                r = call("simulate", {{"operating_point", true}, {"timeout", 60}}, 90000);
                QucsSettings.NgspiceExecutable = before;
                const double mid = json(r).toObject().value("operating point").toObject().value("nodes").toObject().value(x.captured(2)).toDouble(-1);
                QVERIFY2(std::abs(mid - 2.5) < 1e-3, qPrintable(QStringLiteral("%1: %2").arg(v.name, text(r))));
            }
            QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        }
    }

    // A library part whose library is not found, a subcircuit whose file is
    // not: Check Schematic says so - it said only that the wires to their
    // pins ended on nothing (lm386_amp.sch in a build without libraries).
    void aPartThatDidNotLoadIsSaid()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Lib U1 1 200 200 -30 50 0 0 \"NoSuchLibrary\" 0 \"LM386\" 0>\n"
                                                        "  <Sub SUB1 1 400 200 -26 21 0 0 \"no_such_sub.sch\" 0>\n"
                                                        "  <R R1 1 100 100 15 -26 0 1 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                                                        "</Components>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString checked = text(call("check_schematic"));
        QVERIFY2(checked.contains("U1: the library part LM386 of NoSuchLibrary could not be loaded"), qPrintable(checked));
        QVERIFY2(checked.contains("SUB1: its subcircuit no_such_sub.sch is not found"), qPrintable(checked));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A port numbered anew in the subcircuit (a new one numbered 1): each
    // instance pin's label follows the port by its name, not its number.
    void aRenumberedPortKeepsItsNet()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, y] : {std::pair{"a", 100}, std::pair{"b", 200}})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y}})));
        QVERIFY(!failed(call("save_document", {{"as", "renumbered"}, {"replace", true}})));
        QVERIFY(!failed(call("make_symbol", {{"sides", QJsonObject{{"a", "left"}, {"b", "right"}}}})));
        QVERIFY(!failed(call("save_document")));
        const auto ports = [this] {
            QHash<QString, QJsonObject> found;
            for (const QJsonValue& v : json(call("get_schematic", {{"symbol", true}})).toObject().value("symbol paintings").toArray())
                if (v.toObject().value("type").toString() == "port") found.insert(v.toObject().value("name").toString(), v.toObject());
            return found;
        };
        const QJsonObject aWas = ports().value("a");
        QVERIFY(!aWas.isEmpty());
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Sub"}, {"name", "X1"}, {"x", 300}, {"y", 200},
                                               {"properties", QJsonObject{{"File", "renumbered.sch"}}}})));
        QVERIFY(!failed(call("set_label", {{"at", "X1.1"}, {"name", "na"}})));
        QVERIFY(!failed(call("set_label", {{"at", "X1.2"}, {"name", "nb"}})));
        QVERIFY(!failed(call("save_document", {{"as", "usesrenumbered"}, {"replace", true}})));
        // c is port 1 now, a 2 and b 3.
        QVERIFY(!failed(call("show_document", {{"path", "renumbered.sch"}})));
        QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", "c"}, {"x", 100}, {"y", 300}})));
        QVERIFY(!failed(call("edit_component", {{"name", "a"}, {"properties", QJsonObject{{"Num", "2"}}}})));
        QVERIFY(!failed(call("edit_component", {{"name", "b"}, {"properties", QJsonObject{{"Num", "3"}}}})));
        QVERIFY(!failed(call("edit_component", {{"name", "c"}, {"properties", QJsonObject{{"Num", "1"}}}})));
        QJsonObject r = call("make_symbol", {{"sides", QJsonObject{{"a", "left"}, {"b", "right"}, {"c", "left"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // c where a was, a below it: the label left on that place is a's.
        const QHash<QString, QJsonObject> now = ports();
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", now.value("c").value("painting").toInt()},
                                               {"x", aWas.value("x").toInt()}, {"y", aWas.value("y").toInt()}})));
        QVERIFY(!failed(call("edit_painting", {{"symbol", true}, {"painting", now.value("a").value("painting").toInt()},
                                               {"x", aWas.value("x").toInt()}, {"y", aWas.value("y").toInt() + 40}})));
        r = call("save_document");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        // Each pin's net by its port's name.
        QVERIFY(!failed(call("show_document", {{"path", "usesrenumbered.sch"}})));
        QHash<QString, QString> netOf;
        for (const QJsonValue& c : json(call("get_schematic")).toObject().value("components").toArray()) {
            if (c.toObject().value("name").toString() != "X1") continue;
            for (const QJsonValue& p : c.toObject().value("pins").toArray())
                netOf.insert(p.toObject().value("name").toString(), p.toObject().value("net").toString());
        }
        QVERIFY2(netOf.value("a") == "na" && netOf.value("b") == "nb" && netOf.value("c") != "na" && netOf.value("c") != "nb",
                 qPrintable(text(r) + "\n" + text(call("get_schematic"))));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", "renumbered.sch"}, {"unsaved", "discard"}})));
    }

    // tune: a measurement that goes as the value (a divider's voltage over
    // its source) is found by the secant on the value - on the third run,
    // not the sixth; a value of 'values' is set as it is written there.
    // simulate: what Check Schematic found comes first in a failed run's
    // errors.
    void theFourthRoundsTuneAndSimulate()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        Schematic* sch = front();
        const QString R = "\"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        QJsonObject r = call("set_schematic", {{"text", "<Components>\n"
                                                        "  <Vdc V1 1 100 200 18 -26 0 1 \"1 V\" 1>\n"
                                                        "  <GND * 1 100 230 0 0 0 0>\n"
                                                        "  <R R1 1 200 100 15 -26 0 0 " + R +
                                                        "  <R R2 1 300 200 15 -26 0 1 " + R +
                                                        "  <GND * 1 300 230 0 0 0 0>\n"
                                                        "  <.DC DC1 1 100 400 0 57 0 0 \"26.85\" 0 \"0.001\" 0 \"1 pA\" 0 \"1 uV\" 0 \"no\" 0 \"150\" 0 \"no\" 0 \"none\" 0 \"CroutLU\" 0>\n"
                                                        "</Components>\n<Wires>\n"
                                                        "  <100 170 100 100 \"\" 0 0 0 \"\">\n  <100 100 170 100 \"\" 0 0 0 \"\">\n"
                                                        "  <230 100 300 100 \"out\" 250 70 30 \"\">\n  <300 100 300 170 \"\" 0 0 0 \"\">\n"
                                                        "</Wires>\n"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", "divider4"}, {"replace", true}})));
        // out = U / 2: 1.5 V at U = 3 V.
        r = call("tune", {{"component", "V1"}, {"property", "U"}, {"target", 1.5}, {"range", QJsonArray{"1", "10"}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}}, 300000);
        QVERIFY2(!failed(r) && json(r).toObject().value("within tolerance").toBool(), qPrintable(text(r)));
        QVERIFY2(json(r).toObject().value("runs").toArray().size() == 3, qPrintable(text(r)));
        QVERIFY(!failed(call("undo")));
        r = call("tune", {{"component", "V1"}, {"property", "U"}, {"target", 1.5}, {"range", QJsonArray{"0.1", "100"}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}, {"apply", false}}, 300000);
        QVERIFY2(!failed(r) && json(r).toObject().value("runs").toArray().size() <= 5, qPrintable(text(r)));
        r = call("tune", {{"component", "R2"}, {"target", 0.5}, {"range", QJsonArray{"100", "9k"}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}, {"apply", false}}, 300000);
        QVERIFY2(!failed(r) && json(r).toObject().value("runs").toArray().size() <= 6, qPrintable(text(r)));
        // A power of the value (R2's power goes as U squared): drawn on both
        // logarithms, found at once - 1 mW at U = 2 V (8 runs on the value's
        // logarithm alone).
        r = call("tune", {{"component", "V1"}, {"property", "U"}, {"target", 1e-3}, {"range", QJsonArray{"0.1", "100"}},
                          {"measure", QJsonObject{{"operating_point", "R2.p"}}}, {"apply", false}}, 300000);
        QVERIFY2(!failed(r) && json(r).toObject().value("runs").toArray().size() <= 4, qPrintable(text(r)));
        // Values as written: 3 kOhm, not 3k.
        r = call("tune", {{"component", "R2"}, {"target", 0.75}, {"values", QJsonArray{"1k", "3 kOhm"}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}}, 300000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("runs").toArray().at(1).toObject().value("value").toString(), QStringLiteral("3 kOhm"));
        QCOMPARE(sch->getComponentByName("R2")->getProperty("R")->Value, QStringLiteral("3 kOhm"));
        // A pin connected to nothing, and a run that fails.
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R3"}, {"x", 500}, {"y", 100},
                                               {"properties", QJsonObject{{"R", "{nosuchparam}"}}}})));
        r = call("simulate", {}, 120000);
        const QJsonObject outcome = json(r).toObject();
        QVERIFY2(!outcome.value("succeeded").toBool(true), qPrintable(text(r)));
        const QJsonObject first = outcome.value("errors").toArray().first().toObject();
        QVERIFY2(first.value("found").toString().contains("before the run") && first.value("message").toString().contains("R3"), qPrintable(text(r)));
        QVERIFY(outcome.contains("before the run"));
        QucsSettings.NgspiceExecutable = before;
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // The hunt after round 9 (docs/bug_hunts/2026-09-29-tools-after-round-9.md):
    // an argument of another JSON type is refused, not read as its default
    // (C1); the search hints name what rounds 7 to 9 added (C4);
    // create_subcircuit names no port GND, ground's name (A1); pins named
    // IN, OUT1 or OUTA have their roles, and make_symbol sides them (E5); a
    // run that fails says its subcircuits' errors (A5); a transistor's bench
    // points in the order tried, one out of its ranges said so (A10); a
    // power amplifier's speaker is no load to note (A7); the guide's op-amps
    // are as it says (E1); no library has a part twice (F1).
    void whatTheHuntAfterRoundNineFound()
    {
        // C1: 1 for a boolean, "yes", a list for a text, a number for a text.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}})));
        const struct {
            const char* tool;
            QJsonObject args;
            const char* said;
        } wrong[] = {
            {"arrange", {{"straighten", 1}}, "arrange: straighten is true or false, not the number 1."},
            {"arrange", {{"keep_places", "yes"}}, "arrange: keep_places is true or false, not the text \"yes\"."},
            {"arrange", {{"feedback", 1}}, "arrange: feedback is a text, not the number 1."},
            {"arrange", {{"supplies", QJsonArray{"labels"}}}, "arrange: supplies is a text, not a list."},
            {"check_schematic", {{"subcircuits", "yes"}}, "check_schematic: subcircuits is true or false, not the text \"yes\"."},
            {"get_netlist", {{"map", 1}}, "get_netlist: map is true or false, not the number 1."},
            {"import_netlist", {{"text", "* t\nR1 1 0 1k\n.end\n"}, {"title", 5}}, "import_netlist: title is a text, not the number 5."},
            {"make_symbol", {{"sides", QJsonArray{"in"}}}, "make_symbol: sides is an object, not a list."},
            {"make_symbol", {{"sides", QJsonObject{{"in", 3}}}}, "make_symbol: sides.in is a text, not the number 3."},
            {"add_component", {{"type", "R"}, {"x", 150.5}, {"y", 0}}, "add_component: x is a whole number, not the number 150.5."},
        };
        for (const auto& w : wrong) {
            const QJsonObject r = call(w.tool, w.args);
            QVERIFY2(failed(r) && text(r).startsWith(QString::fromUtf8(w.said))
                         && text(r).contains("Nothing was done (it would have been read as its default, not refused)"),
                     qPrintable(QString(w.tool) + ": " + text(r)));
        }
        // Of the right type, or null (not given): as before.
        QVERIFY(!failed(call("check_schematic", {{"subcircuits", QJsonValue()}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 150.0}, {"y", 300}})));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // C4: the words a search for what rounds 7 to 9 added uses.
        QHash<QString, QString> hints;
        for (const QJsonValue& t : control->tools())
            hints.insert(t.toObject().value("name").toString(), t.toObject().value("_meta").toObject().value("anthropic/searchHint").toString());
        for (const auto& [tool, words] : {std::pair("edit_diagram", "notation scientific engineering"), std::pair("add_marker", "notation"),
                                          std::pair("edit_marker", "notation"), std::pair("arrange", "feedback straighten supplies"),
                                          std::pair("tune", "hold compare")})
            for (const QString& word : QString(words).split(' '))
                QVERIFY2(hints.value(tool).split(' ').contains(word), qPrintable(QString(tool) + ": " + hints.value(tool)));

        // E5: the names of one pin and of a dual's.
        using qucs_s::erc::pinRole;
        for (const char* input : {"IN", "in1", "INA", "VIN", "input", "IN1+", "-INA", "INBN", "NONINV2"})
            QVERIFY2(pinRole(input) == "input", input);
        for (const char* output : {"OUT", "out1", "OUTA", "VOUT2", "OUTPUT", "OUT+", "outn"})
            QVERIFY2(pinRole(output) == "output", output);
        for (const char* other : {"ADJ", "INDEX", "OUTER", "GND", "BIAS"}) QVERIFY2(pinRole(other).isEmpty(), other);
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        int num = 0;
        for (const char* port : {"in1", "out1", "in2", "out2", "in3", "out3", "OUTA"}) {
            const QJsonObject r = call("add_component", {{"type", "Port"}, {"name", port}, {"x", 100}, {"y", 100 + 60 * num},
                                                         {"properties", QJsonObject{{"Num", QString::number(num + 1)}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            ++num;
        }
        QVERIFY(!failed(call("save_document", {{"as", "dual.sch"}, {"replace", true}})));
        QJsonObject r = call("make_symbol");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList sides;
        for (const QJsonValue& p : json(r).toObject().value("pins").toArray()) sides << p.toString().section(',', 0, 0);
        QVERIFY2(QSet<QString>(sides.cbegin(), sides.cend()) == QSet<QString>({"in1: left", "in2: left", "in3: left", "out1: right", "out2: right", "out3: right", "OUTA: right"}),
                 qPrintable(sides.join(" | ")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // A1: a moved part's pin named GND on a net of its own (a local
        // ground, on a mid-rail source in the parent): its port is not
        // named GND - ground's name, which joins that net to ground.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        for (const auto& [name, y] : {std::pair("IN", 100), std::pair("GND", 200)})
            QVERIFY(!failed(call("add_component", {{"type", "Port"}, {"name", name}, {"x", 100}, {"y", y},
                                                   {"properties", QJsonObject{{"Num", name == QString("IN") ? "1" : "2"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 150}, {"rotation", 1}})));
        QVERIFY(!failed(call("connect", {{"from", "IN.1"}, {"to", "R1.2"}})));
        QVERIFY(!failed(call("connect", {{"from", "GND.1"}, {"to", "R1.1"}})));
        QVERIFY(!failed(call("save_document", {{"as", "localgnd.sch"}, {"replace", true}})));
        QVERIFY(!failed(call("make_symbol")));
        QVERIFY(!failed(call("save_document")));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Sub"}, {"name", "SUB1"}, {"x", 400}, {"y", 300},
                                                                                {"properties", QJsonObject{{"File", "localgnd.sch"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "VM"}, {"x", 200}, {"y", 400}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "VS"}, {"x", 100}, {"y", 250}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "SUB1.2"}, {"to", "VM.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "VM.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "SUB1.1"}, {"to", "VS.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "VS.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{{"as", "gndparent.sch"}, {"replace", true}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("create_subcircuit", {{"names", QJsonArray{"SUB1"}}, {"save_as", "gndwrap.sch"}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList ports;
        for (const QJsonValue& p : json(r).toObject().value("ports").toArray()) ports << p.toObject().value("net").toString();
        QVERIFY2(ports.size() == 2 && ports.contains("IN") && !ports.contains("GND", Qt::CaseInsensitive), qPrintable(ports.join(" | ")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));

        // A5: a clean schematic, a subcircuit with two sources in parallel:
        // the run fails, and its errors begin with the subcircuit's.
        if (const QString ngspice = QStandardPaths::findExecutable("ngspice"); !ngspice.isEmpty()) {
            const QString before = QucsSettings.NgspiceExecutable;
            QucsSettings.NgspiceExecutable = ngspice;
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Port"}, {"name", "p"}, {"x", 100}, {"y", 100}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "VX"}, {"x", 250}, {"y", 200},
                                                                                    {"properties", QJsonObject{{"U", "5 V"}}}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "VY"}, {"x", 400}, {"y", 200},
                                                                                    {"properties", QJsonObject{{"U", "3 V"}}}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "p.1"}, {"to", "VX.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "VX.1"}, {"to", "VY.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "VX.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "VY.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{{"as", "twosrc.sch"}, {"replace", true}}}}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Sub"}, {"name", "X1"}, {"x", 300}, {"y", 200},
                                                                                    {"properties", QJsonObject{{"File", "twosrc.sch"}}}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RL"}, {"x", 450}, {"y", 200}, {"rotation", 1}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "X1.1"}, {"to", "RL.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RL.2"}, {"to", "ground"}}}},
                QJsonObject{{"tool", "add_analysis"}, {"arguments", QJsonObject{{"kind", "tran"}, {"stop", "1 ms"}}}},
                QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{{"as", "twosrctop.sch"}, {"replace", true}}}}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QVERIFY(json(call("check_schematic")).toObject().value("errors").toArray().isEmpty());
            r = call("simulate", {}, 120000);
            const QJsonObject outcome = json(r).toObject();
            const QJsonObject first = outcome.value("errors").toArray().first().toObject();
            QVERIFY2(!outcome.value("succeeded").toBool(true) && first.value("found").toString() == "by Check Schematic, in a subcircuit"
                         && first.value("message").toString().startsWith("VX and VY are in parallel")
                         && first.value("file").toString().endsWith("twosrc.sch")
                         && outcome.value("before the run").toObject().value("errors in its subcircuits").toArray().size() == 1,
                     qPrintable(text(r)));
            QucsSettings.NgspiceExecutable = before;
            QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        }

        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) return;
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        struct Back {
            QString was;
            ~Back() { QucsSettings.LibDir = was; }
        } back{was};

        // A10: 2N3773 passed at 0.9 mA, after failing at 9 uA: the points
        // in that order, the first said to be out of range.
        const QString said = json(call("describe_part", {{"library", "BJT_Extended"}, {"part", "2N3773"}})).toObject().value("ngspice").toString();
        QVERIFY2(said.contains(QRegularExpression("its bench passes \\(npn: .* - at 9 uA, out of range: Vbe [0-9.]+ V \\(0\\.1 to 1\\.6 V\\), "
                                                  "beta [0-9.]+ \\(3 to 5000\\), Vce [0-9.]+ V; at 0\\.9 mA: Vbe ")),
                 qPrintable(said));

        // A7: an LM3886 into its 8 Ohm speaker: its use, no note; a uA741 into 100 Ohm: the note.
        for (const auto& [part, load, noted] : {std::tuple("LM3886", "8", false), std::tuple("uA741", "100", true)}) {
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            const QJsonObject place = json(call("describe_part", {{"library", "OpAmps"}, {"part", part}})).toObject().value("place").toObject();
            r = call("add_component", {{"type", place.value("type")}, {"name", "U1"}, {"x", 300}, {"y", 200}, {"properties", place.value("properties")}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "RL"}, {"x", 500}, {"y", 300}, {"rotation", 1},
                                                   {"properties", QJsonObject{{"R", load}}}})));
            QVERIFY(!failed(call("connect", {{"from", "U1.OUT"}, {"to", "RL.1"}})));
            QVERIFY(!failed(call("connect", {{"from", "RL.2"}, {"to", "ground"}})));
            const QString notes = QJsonDocument(json(call("check_schematic")).toObject().value("notes").toArray()).toJson();
            QVERIFY2(notes.contains("loads U1's output to ground") == noted, qPrintable(QString(part) + ": " + notes));
            QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        }

        // E1: the guide's op-amps, their - input where it says.
        const QString instructions = control->instructions();
        const QRegularExpressionMatch guided = QRegularExpression("\\(upper in ([^;]+); lower in ([^:]+):").match(instructions);
        QVERIFY2(guided.hasMatch(), qPrintable(instructions.mid(instructions.indexOf("How to work"), 600)));
        for (const QString& side : {QStringLiteral("upper"), QStringLiteral("lower")})
            for (QString part : guided.captured(side == "upper" ? 1 : 2).split(QRegularExpression(",\\s*|\\s+and\\s+"))) {
                part = part.trimmed();
                const QJsonArray pins = json(call("describe_part", {{"library", "OpAmps"}, {"part", part}})).toObject().value("pins").toArray();
                QString inn;
                for (const QJsonValue& p : pins)
                    if (p.toObject().value("name").toString().compare("INN", Qt::CaseInsensitive) == 0) inn = p.toObject().value("side").toString();
                QVERIFY2(inn == "left, " + side, qPrintable(part + ": INN " + inn));
            }

        // F1: no part named twice within a library (the second could never
        // be placed; 13 were so).
        QStringList twice;
        for (const QFileInfo& lib : QDir(library).entryInfoList({"*.lib"}, QDir::Files)) {
            QFile f(lib.filePath());
            QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
            QSet<QString> seen;
            for (auto it = QRegularExpression("<Component\\s+([^>]+)>").globalMatch(QString::fromUtf8(f.readAll())); it.hasNext();)
                if (const QString name = it.next().captured(1).trimmed(); seen.contains(name)) twice << lib.completeBaseName() + "/" + name;
                else seen.insert(name);
        }
        QVERIFY2(twice.isEmpty(), qPrintable(twice.join(", ")));
    }

    // ---- the wishlist of 2026-09-30 ------------------------------------

    // 1: a subcircuit's parameters set by a tool, not by editing its file:
    // listed as they are set, drawn with a symbol when there is none, kept
    // when the symbol is drawn again, and followed by the instances by
    // name - one taken away shifted every value after it, and taking all
    // of them away took the instance's file too.
    void subcircuitParametersAreSetAndFollowByName()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Port"}, {"name", "P1"}, {"x", 100}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}, {"rotation", 1},
                                                                                {"properties", QJsonObject{{"R", "{Rs*k}"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Port"}, {"name", "P2"}, {"x", 300}, {"y", 100},
                                                                                {"properties", QJsonObject{{"Num", "2"}}}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "P1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "P2.1"}}}},
            QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{{"as", "rsub.sch"}, {"replace", true}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const auto names = [](const QJsonArray& list) {
            QStringList n;
            for (const QJsonValue& v : list) n << v.toObject().value("name").toString() + "=" + v.toObject().value("default").toString();
            return n.join(' ');
        };
        // No symbol yet: one is drawn, and the parameters are on it.
        r = call("set_subcircuit_parameters", {{"parameters", QJsonArray{QJsonObject{{"name", "Rs"}, {"default", "1k"}, {"description", "series R"}}, "k=2"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QCOMPARE(names(o.value("parameters").toArray()), QString("Rs=1k k=2"));
        QCOMPARE(o.value("on the .SUBCKT line").toString(), QString("Rs=1k k=2"));
        // (On the symbol's name text - the symbol Edit Circuit Symbol draws
        // for a schematic with none.)
        bool onTheId = false;
        for (const QJsonValue& p : json(call("get_schematic", {{"symbol", true}})).toObject().value("symbol paintings").toArray())
            onTheId = onTheId || (p.toObject().value("type").toString() == "id" && names(p.toObject().value("parameters").toArray()) == "Rs=1k k=2");
        QVERIFY(onTheId);
        QCOMPARE(o.value("parameters").toArray().first().toObject().value("description").toString(), QString("series R"));
        // Refused, each with why, and nothing changed.
        for (const auto& [given, said] : {std::pair(QJsonObject{{"parameters", QJsonArray{"bad name=1"}}}, "is a word"),
                                          std::pair(QJsonObject{{"parameters", QJsonArray{"m"}}}, "needs a 'default'"),
                                          std::pair(QJsonObject{{"parameters", QJsonArray{"m=1 k"}}}, "no space"),
                                          std::pair(QJsonObject{{"parameters", QJsonArray{"RS=3", "rs=4"}}}, "given twice"),
                                          std::pair(QJsonObject{{"parameters", QJsonArray{"File=1"}}}, "cannot be called File"),
                                          std::pair(QJsonObject{{"remove", QJsonArray{"zz"}}}, "no parameter zz"),
                                          std::pair(QJsonObject{{"parameters", QJsonArray{QJsonObject{{"name", "k"}, {"bogus", 1}}}}}, "has no bogus")}) {
            r = call("set_subcircuit_parameters", given);
            QVERIFY2(failed(r) && text(r).contains(said), qPrintable(text(r)));
        }
        QCOMPARE(names(json(call("get_schematic")).toObject().value("subcircuit parameters").toArray()), QString("Rs=1k k=2"));
        // Drawn again: kept; drawn again with only a prefix: kept too.
        r = call("make_symbol", {{"sides", QJsonObject{{"P1", "left"}}}});
        QVERIFY2(names(json(r).toObject().value("parameters").toArray()) == "Rs=1k k=2", qPrintable(text(r)));
        r = call("make_symbol", {{"prefix", "RS"}});
        QVERIFY2(names(json(r).toObject().value("parameters").toArray()) == "Rs=1k k=2", qPrintable(text(r)));
        QVERIFY(!failed(call("save_document")));
        // An instance: its values by name, on the netlist's X line.
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        r = call("add_component", {{"type", "Sub"}, {"name", "X1"}, {"x", 300}, {"y", 200},
                                   {"properties", QJsonObject{{"File", "rsub.sch"}, {"Rs", "5k"}, {"k", "3"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", "rtop.sch"}, {"replace", true}})));
        const auto props = [this] {
            QStringList p;
            for (const QJsonValue& v : componentIn(json(call("get_schematic", {{"path", "rtop.sch"}, {"properties", "all"}})).toObject(), "X1")
                                           .value("properties").toArray())
                p << v.toObject().value("name").toString() + "=" + v.toObject().value("value").toString();
            return p.join(' ');
        };
        QCOMPARE(props(), QString("File=rsub.sch Rs=5k k=3"));
        const QString netlist = text(call("get_netlist", {{"path", "rtop.sch"}}));
        QVERIFY2(netlist.contains(QRegularExpression("\\.SUBCKT rsub [^\\n]*Rs=1k k=2")) && netlist.contains(QRegularExpression("\\nX1 [^\\n]*rsub Rs=5K k=3")),
                 qPrintable(netlist));
        // Rs taken away, m added: k keeps its 3, m starts at its default.
        r = call("set_subcircuit_parameters", {{"path", "rsub.sch"}, {"remove", QJsonArray{"Rs"}}, {"parameters", QJsonArray{"m=7"}}});
        QVERIFY2(!failed(r) && json(r).toObject().contains("instances not open"), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"path", "rsub.sch"}})));
        QCOMPARE(props(), QString("File=rsub.sch k=3 m=7"));
        // All taken away: the instance keeps its file.
        r = call("set_subcircuit_parameters", {{"path", "rsub.sch"}, {"parameters", QJsonArray{}}, {"replace", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"path", "rsub.sch"}})));
        QCOMPARE(props(), QString("File=rsub.sch"));
        // One step to undo.
        QVERIFY(!failed(call("undo", {{"path", "rsub.sch"}})));
        QCOMPARE(names(json(call("get_schematic", {{"path", "rsub.sch"}})).toObject().value("subcircuit parameters").toArray()), QString("k=2 m=7"));
        for (const char* doc : {"rtop.sch", "rsub.sch"}) QVERIFY(!failed(call("close_document", {{"path", doc}, {"unsaved", "discard"}})));
    }

    // 2: sources that netlist as they say: a Vpulse (Ipulse) one pulse,
    // with a period beyond any run (with none, ngspice repeated it, high
    // most of the time); a Vac's (Iac's) AC magnitude of its own, ACmag -
    // and the check says when an AC analysis's sources are all of 0.
    void sourcesNetlistAsMeant()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vpulse"}, {"name", "V1"}, {"x", 100}, {"y", 200},
                                                                                {"properties", QJsonObject{{"U1", "0"}, {"U2", "1"}, {"T1", "1u"}, {"T2", "5u"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Ipulse"}, {"name", "I1"}, {"x", 300}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vac"}, {"name", "V2"}, {"x", 500}, {"y", 200},
                                                                                {"properties", QJsonObject{{"U", "0"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Iac"}, {"name", "I2"}, {"x", 700}, {"y", 200},
                                                                                {"properties", QJsonObject{{"I", "0"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 400}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "I1.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V2.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "I2.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "add_analysis"}, {"arguments", QJsonObject{{"kind", "ac"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const auto line = [this](const QString& name) {
            for (const QString& l : text(call("get_netlist")).split('\n'))
                if (l.startsWith(name + ' ')) return l;
            return QString();
        };
        QVERIFY2(line("V1").contains(QRegularExpression("PULSE\\(0 1 1U \\S+ \\S+ \\{[^}]*\\} 1e9\\)")), qPrintable(line("V1")));
        QVERIFY2(line("I1").contains(QRegularExpression("PULSE\\(.* 1e9\\)")), qPrintable(line("I1")));
        // U = 0: the AC magnitude 0 too, and said.
        QVERIFY2(line("V2").contains(" AC 0 "), qPrintable(line("V2")));
        const auto warnings = [this] { return QJsonDocument(json(call("check_schematic")).toObject().value("warnings").toArray()).toJson(); };
        QVERIFY2(warnings().contains("of AC magnitude 0") && warnings().contains("V2 (its U is 0") && warnings().contains("I2 (its I is 0"),
                 qPrintable(warnings()));
        // ACmag sets it apart from the transient's amplitude.
        QVERIFY(!failed(call("edit_component", {{"name", "V2"}, {"properties", QJsonObject{{"ACmag", "1"}}}})));
        QVERIFY2(line("V2").contains("SIN(0 0 ") && line("V2").contains(" AC 1 ACPHASE"), qPrintable(line("V2")));
        QVERIFY2(!warnings().contains("of AC magnitude 0") && !warnings().contains("nothing to drive it"), qPrintable(warnings()));
        QVERIFY(!failed(call("edit_component", {{"name", "I2"}, {"properties", QJsonObject{{"ACmag", "2m"}}}})));
        QVERIFY2(line("I2").contains(" AC 2M ") || line("I2").contains(" AC 2m "), qPrintable(line("I2")));
        // Under Qucsator the SPICE-only properties stay out.
        for (const char* name : {"V2", "I2"}) {
            const QString q = front()->getComponentByName(name)->getNetlist();
            QVERIFY2(!q.contains("ACmag") && !q.contains("TD=") && !q.contains("VO=") && !q.contains("IO="), qPrintable(q));
            QVERIFY2(q.contains("f=\"") && q.contains("Theta=\""), qPrintable(q));
        }
        // An old file's Vac (no ACmag in its line): U is the AC magnitude.
        QVERIFY(!failed(call("set_schematic", {{"text", "<Components>\n  <Vac V9 1 900 200 18 -26 0 1 \"2 V\" 1 \"1 kHz\" 0 \"0\" 0 \"0\" 0 \"0\" 0 \"0\" 0>\n</Components>\n"}})));
        QVERIFY2(line("V9").contains(" AC 2 "), qPrintable(line("V9")));
        // What describe_component_type says.
        QVERIFY(text(call("describe_component_type", {{"type", "Vac"}})).contains("ACmag"));
        QVERIFY(text(call("describe_component_type", {{"type", "Vpulse"}})).contains("1e9 s"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        // Under ngspice: one pulse, low again after T2.
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) return;
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vpulse"}, {"name", "V1"}, {"x", 100}, {"y", 200},
                                                                                {"properties", QJsonObject{{"U1", "0"}, {"U2", "1"}, {"T1", "1u"}, {"T2", "5u"},
                                                                                                           {"Tr", "1u"}, {"Tf", "1u"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 200}, {"rotation", 1}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.2"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.2"}, {"name", "a"}}}},
            QJsonObject{{"tool", "add_analysis"}, {"arguments", QJsonObject{{"kind", "tran"}, {"stop", "20u"}, {"points", 201}}}},
            QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{{"as", "onepulse.sch"}, {"replace", true}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(json(call("simulate", {}, 120000)).toObject().value("succeeded").toBool(), "the pulse's run");
        r = call("get_dataset", {{"variables", QJsonArray{"tran.v(a)"}}, {"at", QJsonArray{3e-6, 8e-6, 15e-6}}});
        const QJsonArray at = json(r).toObject().value("variables").toArray().first().toObject().value("at").toArray();
        QVERIFY2(at.size() == 3 && std::abs(at.at(0).toArray().at(1).toDouble() - 1) < 1e-6 && std::abs(at.at(1).toArray().at(1).toDouble()) < 1e-6
                     && std::abs(at.at(2).toArray().at(1).toDouble()) < 1e-6,
                 qPrintable(text(r)));
        QucsSettings.NgspiceExecutable = before;
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // 3: an equation's variable named as a net - under ngspice it writes
    // over the node's voltage, and never appears. Not a .param's, nor a
    // name no net has.
    void anEquationNamedAsANetIsWarned()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Vdc"}, {"name", "V1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 250}, {"y", 200}, {"rotation", 1}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.1"}, {"to", "R1.2"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "V1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "R1.1"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "set_label"}, {"arguments", QJsonObject{{"at", "R1.2"}, {"name", "tj1"}}}},
            QJsonObject{{"tool", "add_analysis"}, {"arguments", QJsonObject{{"kind", "tran"}, {"stop", "1m"}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "NutmegEq"}, {"name", "NutmegEq1"}, {"x", 400}, {"y", 400},
                                                                                {"equations", QJsonArray{"Tj1=v(tj1)+27", "ok1=v(tj1)*2", "time=1"}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Eqn"}, {"name", "Eqn1"}, {"x", 600}, {"y", 400},
                                                                                {"equations", QJsonArray{"TJ1=3", "tj1b=v(tj1)"}}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList said;
        for (const QJsonValue& w : json(call("check_schematic")).toObject().value("warnings").toArray()) said << w.toObject().value("message").toString();
        const QString all = said.join(" | ");
        QVERIFY2(all.contains("NutmegEq1: its variable Tj1 is named as the net tj1"), qPrintable(all));
        QVERIFY2(all.contains("NutmegEq1: its variable time is named as the analysis's axis time"), qPrintable(all));
        QVERIFY2(!all.contains("ok1") && !all.contains("variable TJ1") && !all.contains("tj1b"), qPrintable(all));   // a .param, and names of no net
        // An Eqn that reads a voltage is a 'let' too.
        QVERIFY(!failed(call("edit_component", {{"name", "Eqn1"}, {"equations", QJsonArray{"TJ1=v(tj1)"}}})));
        said.clear();
        for (const QJsonValue& w : json(call("check_schematic")).toObject().value("warnings").toArray()) said << w.toObject().value("message").toString();
        QVERIFY2(said.join(" | ").contains("Eqn1: its variable TJ1 is named as the net tj1"), qPrintable(said.join(" | ")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // 6: a dialog's tree read and its rows checked: Find and Replace's
    // results, each a row with a check box - unchecked, nothing replaced;
    // checked, the value.
    void aDialogsTreeIsReadAndChecked()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}, {"properties", QJsonObject{{"R", "1k"}}}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R2"}, {"x", 300}, {"y", 100}, {"properties", QJsonObject{{"R", "1k"}}}})));
        QJsonObject r = call("trigger_action", {{"action", "Edit > Replace..."}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", "Find"}, {"value", "1k"}}, QJsonObject{{"control", "Replace with"}, {"value", "2k"}},
                                                  QJsonObject{{"control", "Property"}, {"value", "R"}}}},
                                {"press", "Find"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject tree;
        for (const QJsonValue& c : json(call("get_dialog")).toObject().value("controls").toArray())
            if (c.toObject().value("kind").toString() == "tree") tree = c.toObject();
        QVERIFY2(tree.value("rows").toArray().size() == 2 && tree.value("checked").toArray() == QJsonArray({true, true})
                     && tree.value("columns").toArray().contains("New value"),
                 qPrintable(QJsonDocument(tree).toJson()));
        const QString id = tree.value("id").toString();
        const int r2 = tree.value("rows").toArray().at(0).toArray().at(1).toString() == "R2" ? 0 : 1;
        r = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", id}, {"value", QJsonArray{r2, 0, false}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        tree = QJsonObject();
        for (const QJsonValue& c : json(call("get_dialog")).toObject().value("controls").toArray())
            if (c.toObject().value("kind").toString() == "tree") tree = c.toObject();
        QCOMPARE(tree.value("checked").toArray().at(r2), QJsonValue(false));
        // A row that is not there, or a text where a box is: said - and
        // what the tree has (bug hunt 2026-09-30, C7).
        r = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", id}, {"value", QJsonArray{7, 0, true}}}}}});
        QVERIFY2(failed(r) && text(r).contains("the tree has 2 rows (0 to 1)"), qPrintable(text(r)));
        r = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", id}, {"value", QJsonArray{0, 99, true}}}}}});
        QVERIFY2(failed(r) && text(r).contains("columns (0 to"), qPrintable(text(r)));
        r = call("set_dialog", {{"set", QJsonArray{QJsonObject{{"control", id}, {"value", QJsonArray{0}}}}}});
        QVERIFY2(failed(r) && text(r).contains("a row is [row, column,"), qPrintable(text(r)));
        QVERIFY(!failed(call("set_dialog", {{"press", "Replace Checked"}})));
        QVERIFY(!failed(call("set_dialog", {{"press", "Close"}})));
        const QJsonObject summary = json(call("get_schematic")).toObject();
        const auto value = [&](const char* name) {
            for (const QJsonValue& p : componentIn(summary, name).value("properties").toArray())
                if (p.toObject().value("name").toString() == "R") return p.toObject().value("value").toString();
            return QString();
        };
        QCOMPARE(value("R1"), QString("2k"));
        QCOMPARE(value("R2"), QString("1k"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // 7: diagrams on a data display: 'document': "data_display" makes the
    // schematic's; a .dpl named but not open is opened; one of no open
    // schematic is refused, saying why.
    void diagramsGoOnTheDataDisplay()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", "withdpl.sch"}, {"replace", true}})));
        const QString dpl = QFileInfo(front()->getDocName()).absoluteDir().filePath("withdpl.dpl");
        QFile::remove(dpl);
        QJsonObject r = call("add_diagram", {{"path", "withdpl.sch"}, {"document", "data_display"}, {"type", "rect"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(text(r).contains("withdpl.dpl, the data display, is open for it"), qPrintable(text(r)));
        QVERIFY(front()->getDocName().endsWith("withdpl.dpl"));
        QVERIFY(json(call("get_schematic", {{"path", "withdpl.sch"}})).toObject().value("diagrams").toArray().isEmpty());
        QCOMPARE(json(call("get_schematic", {{"path", "withdpl.dpl"}})).toObject().value("diagrams").toArray().size(), 1);
        QVERIFY(!failed(call("save_document", {{"path", "withdpl.dpl"}})));
        QVERIFY(!failed(call("close_document", {{"path", "withdpl.dpl"}})));
        // Closed: opened again for the next diagram tool.
        r = call("edit_diagram", {{"path", "withdpl.dpl"}, {"diagram", 1}, {"title", "again"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(front()->getDocName().endsWith("withdpl.dpl"));
        // Its schematic closed too: the .dpl is there, and opened all the same.
        QVERIFY(!failed(call("save_document", {{"path", "withdpl.dpl"}})));
        QVERIFY(!failed(call("close_document", {{"path", "withdpl.dpl"}})));
        QVERIFY(!failed(call("close_document", {{"path", "withdpl.sch"}, {"unsaved", "discard"}})));
        r = call("add_trace", {{"path", "withdpl.dpl"}, {"diagram", 1}, {"variable", "v(x)"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(front()->getDocName().endsWith("withdpl.dpl"));
        QVERIFY(!failed(call("open_document", {{"path", "withdpl.sch"}})));
        // None of a schematic that is open: refused.
        r = call("add_diagram", {{"path", "nosuch.dpl"}});
        QVERIFY2(failed(r) && text(r).contains("no open schematic nosuch.sch"), qPrintable(text(r)));
        // 'document' is add_diagram's alone.
        QVERIFY(failed(call("edit_diagram", {{"path", "withdpl.sch"}, {"document", "data_display"}, {"diagram", 1}})));
        QVERIFY(failed(call("add_diagram", {{"path", "withdpl.sch"}, {"document", "elsewhere"}})));
        QVERIFY(!failed(call("close_document", {{"path", "withdpl.dpl"}, {"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", "withdpl.sch"}, {"unsaved", "discard"}})));
    }

    // The smaller notes: replace_component from unnamed pins (the built-in
    // OpAmp) onto named ones (a library 741) went by number - its + input
    // to the 741's output - with nothing said. Refused when the numbers do
    // not match the roles and places, with a map to pass back as 'pins';
    // taken by number when they do (the TI model's).
    void unnamedPinsOntoNamedOnesAreGuessed()
    {
        const QString library = QFileInfo(QStringLiteral(QUCS_EXAMPLES_DIR) + "/../library").absoluteFilePath();
        if (!QFileInfo::exists(library + "/OpAmps.lib")) QSKIP("no library here");
        const QString was = QucsSettings.LibDir;
        QucsSettings.LibDir = library + "/";
        struct Back {
            QString was;
            ~Back() { QucsSettings.LibDir = was; }
        } back{was};
        for (const int turns : {0, 2}) {
            QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
            QJsonObject r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "OpAmp"}, {"name", "U1"}, {"x", 400}, {"y", 300}, {"rotation", turns}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "Rin"}, {"x", 200}, {"y", 200}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "Rp"}, {"x", 200}, {"y", 400}}}},
                QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RL"}, {"x", 600}, {"y", 300}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "Rin.2"}, {"to", "U1.1"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "Rp.2"}, {"to", "U1.2"}}}},
                QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "U1.3"}, {"to", "RL.1"}}}}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            const QJsonObject lib{{"Lib", "OpAmps"}, {"Comp", "uA741"}};
            r = call("replace_component", {{"name", "U1"}, {"type", "Lib"}, {"properties", lib}});
            QVERIFY2(failed(r) && text(r).contains("'pins': {\"1\": \"INN\", \"2\": \"INP\", \"3\": \"OUT\"}")
                         && text(r).contains("2 (the + input) would go to 2 (OUT), not 3 (INP)"),
                     qPrintable(QString::number(turns) + ": " + text(r)));
            r = call("replace_component", {{"name", "U1"}, {"type", "Lib"}, {"properties", lib},
                                           {"pins", QJsonObject{{"1", "INN"}, {"2", "INP"}, {"3", "OUT"}}}});
            QVERIFY2(!failed(r), qPrintable(text(r)));
            const QJsonObject nodes = json(call("get_netlist", {{"map", true}})).toObject().value("nodes").toObject();
            const auto together = [&](const QString& a, const QString& b) {
                for (const QJsonValue& pins : nodes)
                    if (pins.toArray().contains(a) && pins.toArray().contains(b)) return true;
                return false;
            };
            QVERIFY2(together("Rin.2", "U1.1 (INN)") && together("Rp.2", "U1.3 (INP)") && together("RL.1", "U1.2 (OUT)"),
                     qPrintable(QJsonDocument(nodes).toJson()));
            QVERIFY(!failed(call("undo")));
            // The TI model's numbers are its roles (1 INN, 2 INP, 3 OUT): by number.
            r = call("replace_component", {{"name", "U1"}, {"type", "Lib"}, {"properties", QJsonObject{{"Lib", "OpAmps"}, {"Comp", "ua741(TI)"}}}});
            QVERIFY2(!failed(r) && json(r).toObject().value("mapped").toString() == "by number", qPrintable(text(r)));
            QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        }
        // A part with no roles, turned: its pins' places as its type draws
        // them (a resistor turned round has pin 1 on the right, where the
        // new part's b is - but as drawn it is on the left, a's side).
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Port"}, {"name", "a"}, {"x", 100}, {"y", 100}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "Port"}, {"name", "b"}, {"x", 300}, {"y", 100},
                                                                                {"properties", QJsonObject{{"Num", "2"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 200}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "a.1"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "b.1"}, {"to", "R1.2"}}}},
            QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{{"as", "twopin.sch"}, {"replace", true}}}},
            QJsonObject{{"tool", "make_symbol"}, {"arguments", QJsonObject{{"sides", QJsonObject{{"a", "left"}, {"b", "right"}}}}}},
            QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "RX"}, {"x", 300}, {"y", 200}, {"rotation", 2}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "Ra"}, {"x", 100}, {"y", 300}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "Rb"}, {"x", 500}, {"y", 300}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RX.1"}, {"to", "Rb.1"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "RX.2"}, {"to", "Ra.2"}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("replace_component", {{"name", "RX"}, {"type", "Sub"}, {"properties", QJsonObject{{"File", "twopin.sch"}}}});
        QVERIFY2(!failed(r) && json(r).toObject().value("mapped").toString() == "by number", qPrintable(text(r)));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(!failed(call("close_document", {{"path", "twopin.sch"}, {"unsaved", "discard"}})));
    }

    // Round 10: a preview that makes a data display left it open and its
    // empty file on disk, "would change" saying nothing; an untitled
    // schematic simulated in a project ran in Scratch/Scratch/untitled, and
    // one outside the project ran in the project's Scratch.
    void roundTensFindings()
    {
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QVERIFY(!failed(call("add_component", {{"type", "R"}, {"name", "R1"}, {"x", 200}, {"y", 100}})));
        QVERIFY(!failed(call("save_document", {{"as", "nodpl.sch"}, {"replace", true}})));
        const QString folder = QFileInfo(front()->getDocName()).absolutePath();
        QFile::remove(folder + "/nodpl.dpl");
        const auto opened = [this] {
            QStringList titles;
            for (const QJsonValue& d : json(call("get_state")).toObject().value("documents").toArray()) titles << d.toObject().value("title").toString();
            return titles;
        };
        const QStringList before = opened();
        QJsonObject r = call("add_diagram", {{"path", "nodpl.sch"}, {"document", "data_display"}, {"type", "rect"}, {"preview", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonArray would = json(r).toObject().value("would change").toArray();
        QVERIFY2(would.size() == 1 && would.first().toObject().value("document").toString() == "nodpl.dpl"
                     && would.first().toObject().value("changes").toArray().first().toString() == "made (it had none), and opened"
                     && QJsonDocument(would).toJson().contains("diagram"),
                 qPrintable(text(r)));
        QVERIFY2(!QFileInfo::exists(folder + "/nodpl.dpl"), "the data display's file is gone again");
        QCOMPARE(opened(), before);
        // One that is there but not open: opened for the preview, closed
        // after, its file as it was.
        QVERIFY(!failed(call("add_diagram", {{"path", "nodpl.sch"}, {"document", "data_display"}, {"type", "rect"}})));
        QVERIFY(!failed(call("save_document", {{"path", "nodpl.dpl"}})));
        QVERIFY(!failed(call("close_document", {{"path", "nodpl.dpl"}})));
        QFile dpl(folder + "/nodpl.dpl");
        QVERIFY(dpl.open(QIODevice::ReadOnly));
        const QByteArray saved = dpl.readAll();
        dpl.close();
        r = call("add_diagram", {{"path", "nodpl.dpl"}, {"type", "tab"}, {"preview", true}});
        QVERIFY2(!failed(r) && QJsonDocument(json(r).toObject().value("would change").toArray()).toJson().contains("\"opened\""), qPrintable(text(r)));
        QVERIFY(dpl.open(QIODevice::ReadOnly));
        QCOMPARE(dpl.readAll(), saved);
        QCOMPARE(opened(), before);
        QVERIFY(!failed(call("close_document", {{"path", "nodpl.sch"}, {"unsaved", "discard"}})));

        // The Scratch folder of an untitled schematic, and of a foreign one.
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString wasNgspice = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        r = call("new_project", {{"name", "round10"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QString project = QucsSettings.QucsWorkDir.absolutePath();
        QVERIFY2(project.endsWith("round10_prj") && app->ProjName == "round10", qPrintable(project));
        const QString lowpass = "lowpass\nV1 in 0 DC 0 AC 1\nR1 in out 1k\nC1 out 0 100n\n.ac dec 5 10 1meg\n.end";
        QVERIFY(!failed(call("import_netlist", {{"text", lowpass}})));
        QVERIFY2(json(call("simulate", {}, 120000)).toObject().value("succeeded").toBool(), "the untitled one's run");
        QVERIFY2(QFileInfo(project + "/Scratch/untitled").isDir() && !QFileInfo::exists(project + "/Scratch/Scratch"),
                 qPrintable(QDir(project + "/Scratch").entryList().join(", ")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(QDir().mkpath(dir.filePath("foreign")));
        QVERIFY(!failed(call("import_netlist", {{"text", lowpass}, {"save_as", dir.filePath("foreign/alien.sch")}})));
        QVERIFY2(json(call("simulate", {}, 120000)).toObject().value("succeeded").toBool(), "the foreign one's run");
        QVERIFY2(!QFileInfo::exists(project + "/Scratch/alien"), qPrintable(QDir(project + "/Scratch").entryList().join(", ")));
        QVERIFY(QFileInfo::exists(misc::scratchDirFor(dir.filePath("foreign/alien.sch")) + "/spice4qucs.cir"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
        QVERIFY(QMetaObject::invokeMethod(app, "slotMenuProjClose"));
        QucsSettings.NgspiceExecutable = wasNgspice;
    }

    // 4: a Verilog-A module ngspice was not given: where Qucs-S looked
    // (no project: beside the schematic), and where the module is if it
    // can see it - said with ngspice's error.
    void aModuleNotLoadedIsExplained()
    {
        const QString ngspice = QStandardPaths::findExecutable("ngspice");
        if (ngspice.isEmpty()) QSKIP("no ngspice here");
        const QString before = QucsSettings.NgspiceExecutable;
        QucsSettings.NgspiceExecutable = ngspice;
        const QString folder = dir.filePath("va_loose");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(!failed(call("new_document", {{"kind", "schematic"}})));
        QJsonObject r = call("batch", {{"atomic", true}, {"calls", QJsonArray{
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "SpiceModel"}, {"name", "SpiceModel1"}, {"x", 400}, {"y", 400},
                                                                                {"properties", QJsonObject{{"Line_1", ".model amp_model zzamp (gain=10)"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "SPICE_dev"}, {"name", "X1"}, {"x", 300}, {"y", 200},
                                                                                {"properties", QJsonObject{{"Letter", "N"}, {"Model", "amp_model"}}}}}},
            QJsonObject{{"tool", "add_component"}, {"arguments", QJsonObject{{"type", "R"}, {"name", "R1"}, {"x", 100}, {"y", 200}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "X1.1"}, {"to", "R1.2"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "X1.2"}, {"to", "ground"}}}},
            QJsonObject{{"tool", "connect"}, {"arguments", QJsonObject{{"from", "X1.3"}, {"to", "R1.1"}}}},
            QJsonObject{{"tool", "add_analysis"}, {"arguments", QJsonObject{{"kind", "op"}}}},
            QJsonObject{{"tool", "save_document"}, {"arguments", QJsonObject{{"as", folder + "/vauser.sch"}, {"replace", true}}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const auto hint = [this] {
            const QJsonObject run = json(call("simulate", {}, 120000)).toObject();
            for (const QJsonValue& e : run.value("errors").toArray())
                if (e.toObject().contains("hint")) return e.toObject().value("hint").toString();
            return QStringLiteral("(no hint) ") + QJsonDocument(run.value("errors").toArray()).toJson();
        };
        QString said = hint();
        QVERIFY2(said.contains("no .va or .osdi it can see defines a module zzamp") && said.contains("no project is open")
                     && said.contains(QDir::toNativeSeparators(QFileInfo(folder).absoluteFilePath()).section('/', -1)),
                 qPrintable(said));
        // In a folder below it: found there, and said not to be loaded.
        QVERIFY(QDir().mkpath(folder + "/models"));
        QFile va(folder + "/models/zzamp.va");
        QVERIFY(va.open(QIODevice::WriteOnly));
        va.write("`include \"disciplines.vams\"\nmodule zzamp(a, b, c);\ninout a, b, c;\nelectrical a, b, c;\nanalog begin\nend\nendmodule\n");
        va.close();
        const QString workspace = QucsSettings.qucsWorkspaceDir.absolutePath();
        QucsSettings.qucsWorkspaceDir.setPath(folder);
        said = hint();
        QucsSettings.qucsWorkspaceDir.setPath(workspace);
        QVERIFY2(said.contains("zzamp is a Verilog-A module, defined in models/zzamp.va - not loaded"), qPrintable(said));
        QucsSettings.NgspiceExecutable = before;
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

    // A data file plotted with no simulation (qucs-s-csv-import-mcp-fix):
    // there was no tool to import one, and a trace of an imported dataset
    // (name.dat, name:variable) was given the simulator's prefix by
    // add_diagram, add_trace and edit_trace - read from a name.dat.ngspice
    // that is not there, "simulate to make it". import_data writes it as
    // the Import tab does; its traces have no prefix; the texts say so.
    void aDataFileIsImportedAndPlotted()
    {
        const QString folder = QFileInfo(QucsSettings.qucsWorkspaceDir.absoluteFilePath("imports")).absoluteFilePath();
        QVERIFY(QDir().mkpath(folder));
        const auto write = [&folder](const QString& name, const QByteArray& bytes) {
            QFile f(folder + "/" + name);
            if (f.open(QIODevice::WriteOnly)) f.write(bytes);
        };
        const auto csv = [](int rows) {
            QByteArray b = "time,v1,v2,v3\n";
            for (int i = 0; i < rows; ++i)
                b += QByteArray::number(i * 1e-5) + ',' + QByteArray::number(i * 0.1) + ',' + QByteArray::number(1 - i * 0.1) + ','
                     + QByteArray::number(i * i) + '\n';
            return b;
        };
        const auto traceIn = [](const QJsonObject& diagram, int n) { return diagram.value("traces").toArray().at(n - 1).toObject(); };
        write("dummy_data.csv", csv(11));

        // A schematic of no file: refused (the dataset goes beside it).
        QVERIFY(!failed(call("new_document")));
        QJsonObject r = call("import_data", {{"file", folder + "/dummy_data.csv"}});
        QVERIFY2(failed(r) && text(r).contains("save it first"), qPrintable(text(r)));
        QVERIFY(!failed(call("save_document", {{"as", folder + "/csv_plot.sch"}, {"replace", true}})));
        // A trace of it before it is there: the simulator's, as ever.
        r = call("add_diagram", {{"traces", QJsonArray{"dummy_data:v1"}}});
        QCOMPARE(traceIn(json(r).toObject(), 1).value("variable").toString(), QStringLiteral("ngspice/dummy_data:v1"));

        // Imported, beside the schematic.
        r = call("import_data", {{"file", "dummy_data.csv"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r).toObject();
        QCOMPARE(o.value("dataset").toString(), QStringLiteral("dummy_data"));
        QCOMPARE(o.value("traces").toArray(), (QJsonArray{"dummy_data:v1", "dummy_data:v2", "dummy_data:v3"}));
        QCOMPARE(o.value("x").toString(), QStringLiteral("time"));
        QCOMPARE(o.value("columns").toArray(), (QJsonArray{"time", "v1", "v2", "v3"}));
        QVERIFY2(!o.contains("sheets") && !o.contains("read again") && !o.contains("replaced"), qPrintable(text(r)));
        const QJsonArray vars = o.value("variables").toArray();
        QCOMPARE(vars.size(), 4);
        QVERIFY(vars.at(0).toObject().value("independent").toBool());
        QCOMPARE(vars.at(0).toObject().value("points").toInt(), 11);
        QCOMPARE(vars.at(3).toObject().value("max").toDouble(), 100.0);
        qucs_s::dataimport::Origin origin;
        QVERIFY(qucs_s::dataimport::originOf(folder + "/dummy_data.dat", &origin));
        QCOMPARE(QFileInfo(origin.source).canonicalFilePath(), QFileInfo(folder + "/dummy_data.csv").canonicalFilePath());
        // The trace added before it: read again, and its prefix said.
        const QJsonObject shown = o.value("diagrams").toArray().at(0).toObject();
        QVERIFY2(shown.value("traces of it").toInt() == 1 && shown.value("with data").toInt() == 0, qPrintable(text(r)));
        QCOMPARE(shown.value("with a simulator's prefix").toArray().at(0).toObject().value("to read it").toString(),
                 QStringLiteral("dummy_data:v1"));

        // Plotted: no prefix, its points at once (the report's call).
        r = call("add_diagram", {{"traces", QJsonArray{"dummy_data:v1", "dummy_data:v2", "dummy_data:v3"}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        for (int n = 1; n <= 3; ++n) {
            const QJsonObject t = traceIn(json(r).toObject(), n);
            QCOMPARE(t.value("variable").toString(), QStringLiteral("dummy_data:v%1").arg(n));
            QVERIFY2(t.value("points").toInt() == 11 && !t.contains("no data"), qPrintable(text(r)));
        }
        // The first diagram's trace: why it shows nothing, and edit_trace
        // takes the prefix off (it put it back).
        const QJsonObject blank = json(call("reload_data")).toObject().value("reloaded").toArray().at(0).toObject();
        const QString why = blank.value("traces without data").toArray().at(0).toObject().value("why").toString();
        QVERIFY2(why.contains("dummy_data.dat is imported (from dummy_data.csv)") && why.contains("dummy_data:v1, without a simulator's prefix")
                     && !why.contains("simulate to make it"),
                 qPrintable(why));
        r = call("edit_trace", {{"diagram", 1}, {"trace", 1}, {"variable", "dummy_data:v1"}});
        QCOMPARE(json(r).toObject().value("variable").toString(), QStringLiteral("dummy_data:v1"));
        QCOMPARE(json(r).toObject().value("points").toInt(), 11);
        QVERIFY(!failed(call("add_diagram", {{"type", "rect"}})));   // (3: the traces below)
        // A prefix given is kept (as the diagram's dialog writes it); qucsator/
        // is no simulator's; a variable of it named alone is pointed to.
        r = call("add_trace", {{"diagram", 3}, {"variable", "ngspice/dummy_data:v1"}});
        QCOMPARE(json(r).toObject().value("variable").toString(), QStringLiteral("ngspice/dummy_data:v1"));
        QVERIFY2(json(r).toObject().value("no data").toString().contains("dummy_data.dat is imported"), qPrintable(text(r)));
        r = call("add_trace", {{"diagram", 3}, {"variable", "qucsator/dummy_data:v2"}});
        QVERIFY2(json(r).toObject().value("variable").toString() == "dummy_data:v2", qPrintable(text(r)));
        QCOMPARE(json(r).toObject().value("points").toInt(), 11);
        r = call("add_trace", {{"diagram", 3}, {"variable", "v3"}});
        QVERIFY2(json(r).toObject().value("note").toString().contains("dummy_data (imported from dummy_data.csv) has v3: the trace "
                                                                        "dummy_data:v3 shows it"),
                 qPrintable(text(r)));

        // list_documents and get_dataset: imported, from where; its traces.
        QJsonObject listed = json(call("list_documents", {{"folder", folder}})).toObject();
        QJsonObject dat, sch;
        for (const QJsonValue& f : listed.value("files").toArray()) {
            if (f.toObject().value("path").toString() == "dummy_data.dat") dat = f.toObject();
            if (f.toObject().value("path").toString() == "csv_plot.sch") sch = f.toObject();
        }
        QVERIFY2(dat.value("imported from").toString() == "dummy_data.csv" && !dat.contains("simulator")
                     && dat.value("traces").toString() == "dummy_data:variable",
                 qPrintable(QJsonDocument(listed).toJson()));
        const QJsonObject missing = sch.value("traces without their dataset").toArray().at(0).toObject();
        QVERIFY2(missing.value("trace").toString() == "ngspice/dummy_data:v1"
                     && missing.value("instead").toString().startsWith("dummy_data:v1: dummy_data.dat is imported"),
                 qPrintable(QJsonDocument(listed).toJson()));
        o = json(call("get_dataset", {{"path", folder + "/dummy_data.dat"}})).toObject();
        QVERIFY2(o.value("imported from").toObject().value("file").toString().endsWith("dummy_data.csv")
                     && o.value("variables").toArray().at(0).toObject().value("trace").toString() == "dummy_data:v1",
                 qPrintable(QJsonDocument(o).toJson()));

        // name:variable of another dataset: no prefix when it is no
        // simulator's (a name.dat with no name.dat.ngspice beside it, or
        // imported); the simulator's with one beside it, and for a kept run.
        write("plain.dat", "<Qucs Dataset " PACKAGE_VERSION ">\n<indep time 3>\n  0\n  1\n  2\n</indep>\n<dep v1 time>\n  1\n  2\n  3\n</dep>\n");
        r = call("add_trace", {{"diagram", 3}, {"variable", "plain:v1"}});
        QVERIFY2(json(r).toObject().value("variable").toString() == "plain:v1" && json(r).toObject().value("points").toInt() == 3,
                 qPrintable(text(r)));
        QVERIFY(QFile::copy(folder + "/plain.dat", folder + "/plain.dat.ngspice"));
        QCOMPARE(json(call("add_trace", {{"diagram", 3}, {"variable", "plain:v1"}})).toObject().value("variable").toString(),
                 QStringLiteral("ngspice/plain:v1"));
        QVERIFY(QFile::copy(folder + "/plain.dat", folder + "/dummy_data.dat.ngspice"));
        QCOMPARE(json(call("add_trace", {{"diagram", 3}, {"variable", "dummy_data:v3"}})).toObject().value("variable").toString(),
                 QStringLiteral("dummy_data:v3"));
        QVERIFY(QFile::remove(folder + "/dummy_data.dat.ngspice"));
        QVERIFY(QFile::copy(folder + "/plain.dat", folder + "/run1.dat.ngspice"));
        r = call("add_trace", {{"diagram", 3}, {"variable", "run1:v1"}});
        QVERIFY2(json(r).toObject().value("variable").toString() == "ngspice/run1:v1" && json(r).toObject().value("points").toInt() == 3,
                 qPrintable(text(r)));

        // What is not there, or not so: said, and nothing written.
        const auto refused = [this](const QJsonObject& args, const QString& said) {
            const QJsonObject answer = call("import_data", args);
            return failed(answer) && text(answer).contains(said) ? QString() : text(answer);
        };
        QCOMPARE(refused({{"file", "dummy_data.csv"}, {"x", "volts"}}, "has no column volts (it has time, v1, v2, v3"), QString());
        QCOMPARE(refused({{"file", "dummy_data.csv"}, {"sheet", "S1"}}, "is a CSV, which has no sheets"), QString());
        QCOMPARE(refused({{"file", "dummy_data.csv"}, {"name", "a:b"}}, "letters, digits and _"), QString());
        QCOMPARE(refused({{"file", "dummy_data.csv"}, {"name", "csv_plot"}}, "csv_plot.sch is there"), QString());
        QCOMPARE(refused({{"file", "dummy_data.csv"}, {"name", "run1"}}, "run1.dat.ngspice is a simulation's dataset"), QString());
        QVERIFY(QFile::remove(folder + "/plain.dat.ngspice"));
        QCOMPARE(refused({{"file", "dummy_data.csv"}, {"name", "plain"}}, "plain.dat is a dataset that was not imported"), QString());
        QCOMPARE(refused({{"file", "nothere.csv"}}, "There is no file nothere.csv"), QString());
        QCOMPARE(refused({{"name", "dummy_data"}, {"remove", true}, {"reload", true}}, "not both"), QString());
        QCOMPARE(refused({{"name", "nothere"}, {"remove", true}}, "imported there: dummy_data"), QString());
        QVERIFY(!qucs_s::dataimport::originOf(folder + "/plain.dat", &origin));
        QVERIFY(!QFileInfo::exists(folder + "/csv_plot.dat"));
        // x by its column's name in another case, or the row.
        r = call("import_data", {{"file", "dummy_data.csv"}, {"x", "V1"}});
        QVERIFY2(json(r).toObject().value("x").toString() == "v1" && json(r).toObject().value("read again").toBool(), qPrintable(text(r)));
        r = call("import_data", {{"file", "dummy_data.csv"}, {"x", "#row"}});
        QCOMPARE(json(r).toObject().value("x").toString(), QStringLiteral("the row"));

        // The file changed: read again - with the x it had (the row), then
        // with time - and the diagrams show it.
        write("dummy_data.csv", csv(21));
        r = call("import_data", {{"name", "dummy_data"}, {"reload", true}});
        QVERIFY2(json(r).toObject().value("x").toString() == "the row", qPrintable(text(r)));
        r = call("import_data", {{"file", "dummy_data.csv"}, {"reload", true}, {"x", "time"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("x").toString() == "time", qPrintable(text(r)));
        o = json(r).toObject();
        QVERIFY2(o.value("read again").toBool() && o.value("variables").toArray().at(0).toObject().value("points").toInt() == 21,
                 qPrintable(text(r)));
        const QJsonObject d2 = json(call("get_schematic")).toObject().value("diagrams").toArray().at(1).toObject();
        QCOMPARE(traceIn(d2, 1).value("points").toInt(), 21);

        // Another name for it; removed (the file it came from stays), and
        // put back by undo with 'files'.
        r = call("import_data", {{"file", "dummy_data.csv"}, {"name", "second"}});
        QVERIFY2(!failed(r) && json(r).toObject().value("dataset").toString() == "second", qPrintable(text(r)));
        r = call("import_data", {{"name", "second"}, {"remove", true}});
        QVERIFY2(!failed(r) && json(r).toObject().value("removed").toString() == "second", qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(folder + "/second.dat"));
        QVERIFY(QFileInfo::exists(folder + "/dummy_data.csv"));
        // (The trash of the test's settings, not the user's.)
        QVERIFY(QFileInfo::exists(qEnvironmentVariable("QUCS_TRASH_DIR") + "/second.dat"));
        QVERIFY(!failed(call("undo", {{"files", true}})));
        QVERIFY(qucs_s::dataimport::originOf(folder + "/second.dat", &origin));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

};

QTEST_MAIN(TestQucsControl)
#include "test_qucs_control.moc"
