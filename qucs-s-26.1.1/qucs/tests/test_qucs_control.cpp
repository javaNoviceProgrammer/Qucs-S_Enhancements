/*
 * Claude's tools for the Qucs-S window (qucscontrol.h), used on the
 * application itself: documents opened, shown, saved and closed; a
 * schematic built part by part - components, wires, labels, changes, one
 * step to undo each; wires that join their ends' nets and nothing else,
 * parts turned and moved with the circuit kept - read back as a summary
 * (with the nets) and as text, replaced from
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
