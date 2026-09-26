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
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <cmath>
#include <functional>
#include <random>

#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "components/component.h"
#include "config.h"
#include "diagrams/diagram.h"
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
#include "wire.h"
#include "wirelabel.h"

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

        QJsonObject seen, answered, refused, undone, state;
        const auto parts = sch->a_DocComps.size();
        QTimer::singleShot(800, this, [&] {
            // While it waits: what changes waits too (the dialog holds on to
            // what it edits); what looks goes on.
            refused = call("add_component", {{"type", "R"}, {"x", 500}, {"y", 500}});
            undone = call("undo");
            state = call("get_state");
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
        for (const QJsonValue& t : control->tools()) {
            const QJsonObject tool = t.toObject();
            keys.insert(tool.value("name").toString(), tool.value("inputSchema").toObject().value("properties").toObject().keys());
        }
        const QStringList tools = {"get_dataset", "add_diagram", "edit_diagram", "add_trace", "edit_trace", "reload_data",
                                   "rename_net", "describe_component_type", "get_netlist", "delete", "get_schematic"};
        const QStringList words = {"", "out", "in", "v(out)", "tran.v(out)", "ngspice/tran.v(out)", "ac.v(out)", "x:y@z", "../up",
                                   "auto", "red", "#zzz", "rect", "smith", "tab", "timing", "3d", "histogram", "left", "right",
                                   "dash", "arrows", "top_left", "dB", "bandwidth", "rise_time", "crossings", "net1", "gnd",
                                   "R1", "Vpulse", "db_phase", "real_imaginary", QString(3000, QLatin1Char('x'))};
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
                static const QStringList inner = {"label", "log", "auto", "from", "to", "step", "units", "diagram", "trace", "variable", "color"};
                QJsonObject o;
                for (int i = int(rng.bounded(4)); i > 0; --i) o.insert(inner.at(rng.bounded(int(inner.size()))), odd(depth + 1));
                return o;
            }
            }
        };
        int answered = 0;
        for (int i = 0; i < 600; ++i) {
            const QString tool = tools.at(rng.bounded(int(tools.size())));
            QJsonObject args;
            for (const QString& key : keys.value(tool))
                if (key != "path" && rng.bounded(10) < 6) args.insert(key, odd(0));
            const QJsonObject r = call(tool, args);
            QVERIFY2(r.contains("content"), qPrintable(tool + ' ' + QJsonDocument(args).toJson(QJsonDocument::Compact)));
            QVERIFY(front() == sch);
            if (!failed(r)) ++answered;
        }
        QVERIFY2(answered > 100, qPrintable(QString::number(answered)));   // not all refused
        // Still a schematic: read, undone and redone step by step.
        QVERIFY(!failed(call("get_schematic")));
        for (int i = 0; i < 25; ++i) call("undo");
        for (int i = 0; i < 25; ++i) call("redo");
        QVERIFY(sch->documentText().startsWith("<Qucs Schematic"));
        QVERIFY(!failed(call("get_schematic")));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
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

        QVERIFY(failed(call("simulate", {{"keep_as", "../elsewhere"}})));
        const QJsonObject result = call("simulate", {{"timeout", 60}, {"keep_as", "run1"}}, 90000);
        const QJsonObject outcome = json(result).toObject();
        QVERIFY2(outcome.value("succeeded").toBool(), qPrintable(text(result)));
        QVERIFY2(outcome.value("dataset written").toBool(), qPrintable(text(result)));
        QVERIFY(outcome.value("dataset").toString().endsWith("rc.dat.ngspice"));
        QVERIFY2(outcome.value("variables").toArray().contains(QJsonValue("tran.v(out)")), qPrintable(text(result)));
        QVERIFY(outcome.value("errors").toArray().isEmpty());
        QVERIFY2(!outcome.contains("traces without data"), qPrintable(text(result)));
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
};

QTEST_MAIN(TestQucsControl)
#include "test_qucs_control.moc"
