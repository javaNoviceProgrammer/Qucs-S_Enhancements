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
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QPdfWriter>
#include <QPainter>

#include <cmath>
#include <complex>
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
#include "diagrams/graph.h"
#include "qucscontrol_p.h"
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
        for (const QJsonValue& t : control->tools()) {
            const QJsonObject tool = t.toObject();
            keys.insert(tool.value("name").toString(), tool.value("inputSchema").toObject().value("properties").toObject().keys());
        }
        const QStringList tools = {"get_dataset", "add_diagram", "edit_diagram", "add_trace", "edit_trace", "reload_data",
                                   "rename_net", "describe_component_type", "get_netlist", "delete", "get_schematic",
                                   "add_marker", "edit_marker", "delete_marker", "describe_format", "export_netlist", "set_schematic",
                                   "add_painting", "edit_painting", "list_documents", "export_image", "set_simulator",
                                   "check_schematic", "move", "add_analysis", "undo_history", "find_library_component",
                                   "read_pdf", "edit_component", "add_component"};
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
        for (const QJsonObject& bad : {QJsonObject{{"at", "nonsense"}}, QJsonObject{{"at", "crossing:x"}}, QJsonObject{{"at", "crossing:12"}},
                                       QJsonObject{{"at", 1}, {"format", "bad"}}, QJsonObject{{"at", 1}, {"fill_color", "#zz"}}, QJsonObject{}})
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
        summary = json(call("get_schematic")).toObject();
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
        // The schematic's tools wait for the schematic; a painting of it
        // switches back.
        QVERIFY(failed(call("add_component", {{"type", "R"}, {"x", 0}, {"y", 0}})));
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
        QVERIFY(failed(call("move", {{"names", QJsonArray{"R1"}}, {"dx", 1}})));
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
            QVERIFY2(outcome.value("schematic check").toObject().value("warnings").toArray().size() > 0, qPrintable(text(r)));
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
        QVERIFY(!failed(call("save_document", {{"as", "divider"}})));
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
        // Not bracketed: said.
        r = call("tune", {{"component", "R1"}, {"target", 20}, {"range", QJsonArray{"1k", "2k"}},
                          {"measure", QJsonObject{{"operating_point", "out"}}}}, 300000);
        QVERIFY2(json(r).toObject().value("stopped").toString().contains("widen"), qPrintable(text(r)));
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
        // Copied into it by its name.
        r = call("copy_document", {{"to", "round5"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(QFileInfo::exists(ws + "/round5_prj/orig.sch"));
        // Scratch: the schematic's own subfolder, cleared.
        const QString scratch = misc::scratchDirFor(ws + "/orig.sch");
        QDir().mkpath(scratch);
        {
            QFile f(scratch + "/spice4qucs.cir");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("* netlist\n");
        }
        r = call("clean_scratch");
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!QFileInfo::exists(scratch + "/spice4qucs.cir"));
        QVERIFY(QFileInfo::exists(ws + "/orig.dat.ngspice"));   // datasets only when asked
        QVERIFY(!failed(call("clean_scratch", {{"datasets", true}})));
        QVERIFY(!QFileInfo::exists(ws + "/orig.dat.ngspice"));
        QVERIFY(!failed(call("close_document", {{"unsaved", "discard"}})));
    }

};

QTEST_MAIN(TestQucsControl)
#include "test_qucs_control.moc"
