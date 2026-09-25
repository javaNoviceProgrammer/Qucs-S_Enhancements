/*
 * What the stress runs found, one test each: the GUI monkey
 * (test_gui_monkey), whose random walk goes elsewhere after any change, and
 * the scale cases (enormous values, self-including subcircuits). Each of
 * these read freed memory, crashed, hung, took gigabytes or was undefined
 * behaviour before; run under ASan/UBSan.
 */
#include <QtTest>
#include <QClipboard>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <tuple>

#include "schematic.h"
#include "mouseactions.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "config.h"
#include "components/component.h"
#include "components/resistor.h"
#include "wire.h"
#include "wirelabel.h"
#include "node.h"
#include "diagrams/diagram.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/curvediagram.h"
#include "dialogs/searchdialog.h"
#include "textdoc.h"
#include "extsimkernels/spicecompat.h"
#include "extsimkernels/abstractspicekernel.h"
#include "extsimkernels/ngspice.h"
#include "components/property.h"
#include "graphicsexport.h"
#include "healer.h"
#include "geometry/multi_point.h"

class TestStressFindings : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    void write(const QString& name, const QString& text)
    {
        QFile f(dir.filePath(name));
        QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(name));
        f.write(text.toUtf8());
    }

    // A schematic with the given components (and a two-port symbol).
    QString schematic(const QString& components) const
    {
        return QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,800,600,1,0,0>\n"
                              "  <Grid=10,10,1>\n</Properties>\n<Symbol>\n  <.PortSym -30 0 1 0 P1>\n"
                              "  <.PortSym 30 0 2 180 P2>\n</Symbol>\n<Components>\n")
               + components
               + QStringLiteral("</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
    }

    static QString sub(const QString& name, const QString& file)
    {
        return QStringLiteral("  <Sub %1 1 200 0 18 -26 0 0 \"%2\" 1>\n").arg(name, file);
    }

    QString example(const QString& relative) const
    {
        return QStringLiteral(QUCS_EXAMPLES_DIR) + QStringLiteral("/") + relative;
    }

    static void selectOnly(Schematic& sch, Element* e)
    {
        for (auto* c : sch.a_DocComps) c->isSelected = false;
        for (auto* w : sch.a_DocWires) w->isSelected = false;
        for (auto* d : sch.a_DocDiags) d->isSelected = false;
        for (auto* p : sch.a_DocPaints) p->isSelected = false;
        e->isSelected = true;
    }

    static void enterSymbolMode(Schematic& sch)
    {
        sch.switchPaintMode();
        sch.becomeCurrent(false);
        QVERIFY(sch.getSymbolMode());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        QucsSettings.QucsWorkDir.setPath(dir.path());
        QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // A double click on a wire is press, release, double click. The
    // release healed the schematic, which merged the clicked wire into its
    // neighbour and freed it; the double click then edited the freed wire.
    // The same happened to the element being dragged when Delete came in
    // the middle of the drag, and to "Reset diagram limits" on a diagram
    // freed since the right click.
    void theElementUnderTheMouseIsDroppedOnceFreed()
    {
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        MouseActions view(nullptr);

        Wire* wire = sch.a_DocWires.front();
        view.focusElement = wire;
        QVERIFY(sch.holds(wire));
        view.dropStaleElements(&sch);
        QCOMPARE(view.focusElement, static_cast<Element*>(wire));   // alive: kept

        selectOnly(sch, wire);
        QVERIFY(sch.deleteElements());
        QVERIFY(!sch.holds(wire));
        view.dropStaleElements(&sch);
        QVERIFY(view.focusElement == nullptr);

        // Graphs and markers belong to their diagram.
        Diagram* diagram = sch.a_DocDiags.front();
        QVERIFY(!diagram->Graphs.isEmpty());
        QVERIFY(sch.holds(diagram));
        QVERIFY(sch.holds(diagram->Graphs.front()));
        QVERIFY(sch.heldElements().contains(diagram->Graphs.front()));
    }

    // An element of the schematic is not one of the symbol's: a double
    // click in the symbol view edited the component picked in the
    // schematic view, and its dialog put the edited copy among the
    // symbol's elements - one component in two lists.
    void anElementOfTheOtherViewIsNotHeld()
    {
        Schematic sch(nullptr, example("ngspice/RF/Miscellaneous/RCL_resonance.sch"));
        QVERIFY(sch.load());
        MouseActions view(nullptr);
        Component* component = sch.a_DocComps.front();
        view.focusElement = component;
        enterSymbolMode(sch);
        QVERIFY(!sch.holds(component));
        view.dropStaleElements(&sch);
        QVERIFY(view.focusElement == nullptr);
    }

    // The lists a schematic uses for components, wires, nodes and diagrams
    // in symbol mode were globals shared by every open document: what a
    // tool put there showed up in every symbol, and closing one document
    // freed what the others still listed.
    void eachDocumentHasItsOwnSymbolLists()
    {
        auto* a = new Schematic(nullptr, QString());
        auto* b = new Schematic(nullptr, QString());
        enterSymbolMode(*a);
        enterSymbolMode(*b);
        a->a_Components->push_back(new Resistor());
        a->a_Nodes->push_back(new Node(10, 10));
        QCOMPARE(a->a_Components->size(), std::size_t(1));
        QCOMPARE(b->a_Components->size(), std::size_t(0));
        QCOMPARE(b->a_Nodes->size(), std::size_t(0));
        delete b;
        delete a;   // frees its resistor and node, once (ASan)
    }

    // Moving the end of a wire of no length that carries a label: the
    // label keeps its place along the wire by a ratio of two lengths, 0/0.
    void aLabelledWireOfNoLengthMoves()
    {
        auto* n1 = new Node(100, 100);
        auto* n2 = new Node(100, 100);
        {
            Wire wire(n1, n2);
            wire.setName("net", "", 100, 100, 120, 80);
            QVERIFY(wire.hasLabel());
            QVERIFY(wire.setP2(QPoint(200, 100)));
            QCOMPARE(wire.label()->root(), QPoint(100, 100));
            Wire other(new Node(0, 0), new Node(0, 0));
            other.setName("n2", "", 0, 0, 10, 10);
            QVERIFY(other.setP1(QPoint(-50, 0)));
            delete other.Port1;
            delete other.Port2;
        }
        delete n1;
        delete n2;
    }

    // A zoom rectangle on a diagram whose axis had collapsed, or "nan"
    // typed as a limit: the limits became nan, the log axis took nan for
    // valid, and the grid loop never moved on from its first line.
    void aDiagramWithNanLimitsIsDrawn()
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (const bool log : {true, false})
            for (const auto& [lo, hi] : {std::pair{nan, nan}, std::pair{nan, 10.0}, std::pair{100.0, 100.0},
                                         std::pair{1e-300, 1e300}, std::pair{inf, inf}, std::pair{-inf, inf}}) {
                RectDiagram rect(0, 300);
                rect.x2 = 400;
                rect.y2 = 300;
                CurveDiagram curve(0, 300);
                curve.x2 = 400;
                curve.y2 = 300;
                for (Diagram* d : {static_cast<Diagram*>(&rect), static_cast<Diagram*>(&curve)}) {
                    for (Axis* axis : {&d->xAxis, &d->yAxis, &d->zAxis}) {
                        axis->log = log;
                        axis->autoScale = false;
                        axis->limit_min = lo;
                        axis->limit_max = hi;
                        axis->numGraphs = 1;
                    }
                    QElapsedTimer clock;
                    clock.start();
                    d->calcDiagram();
                    QVERIFY2(clock.elapsed() < 10000,
                             qPrintable(QStringLiteral("%1 %2 axis [%3, %4] took %5 ms")
                                            .arg(d->Name, log ? "log" : "linear").arg(lo).arg(hi)
                                            .arg(clock.elapsed())));
                }
            }
    }

    // "1e308k" in a component's field: the reading of it is inf, and the
    // engineering prefix of inf was log10(inf) converted to int.
    void numbersWithoutAPrefixAreWritten()
    {
        const double inf = std::numeric_limits<double>::infinity();
        QCOMPARE(misc::num2str(inf, -1, "V"), QStringLiteral("infV"));
        QCOMPARE(misc::num2str(-inf, -1, "V"), QStringLiteral("-infV"));
        QCOMPARE(misc::num2str(std::numeric_limits<double>::quiet_NaN(), -1, "m"), QStringLiteral("nan"));
        QCOMPARE(misc::num2str(1500.0, -1, "Hz"), QStringLiteral("1.5kHz"));
    }

    // Paste with nothing on the clipboard (a platform without one, or
    // nothing copied yet): the clipboard's data is null.
    void pasteWithAnEmptyClipboard()
    {
        QApplication::clipboard()->clear();
        Schematic sch(nullptr, QString());
        QString text;
        QTextStream stream(&text);
        std::list<Element*> pasted;
        QVERIFY(!sch.paste(&stream, &pasted));
        QVERIFY(pasted.empty());
    }

    // When a node sits at none of its ports' places, healing moves it to
    // the closest of them - which findClosest() did not find: it never
    // lowered its best distance, so the last place closer than the first
    // won. Here the first place (in x order) is 100 away, 90 is 10 away
    // and 150 is 50 away: it chose 150.
    void aMisplacedNodeMovesToTheClosestPort()
    {
        struct Recorder : qucs_s::SchematicMutator {
            std::vector<std::pair<Node*, QPoint>> moves;
            void moveNode(Node* node, const QPoint& p) override { moves.emplace_back(node, p); }
        };

        Node node(100, 0);
        std::vector<std::unique_ptr<Node>> ends;
        std::list<Wire*> wires;
        for (const int x : {0, 90, 150}) {
            auto* w = new Wire(x, 0, x, 100);   // its first end is not at the node
            ends.push_back(std::make_unique<Node>(x, 100));
            w->Port1 = &node;
            node.connect(w);
            w->Port2 = ends.back().get();
            ends.back()->connect(w);
            wires.push_back(w);
        }
        const std::list<Component*> components;
        qucs_s::Healer healer{&components, &wires, {.allowWireReshaping = false, .allowWireRelaying = false}};
        Recorder recorder;
        for (auto& action : healer.planHealing()) action->execute(&recorder);

        QCOMPARE(recorder.moves.size(), std::size_t(1));
        QCOMPARE(recorder.moves.front().first, &node);
        QCOMPARE(recorder.moves.front().second, QPoint(90, 0));
        qDeleteAll(wires);
    }

    // A paste is parsed by the document current when it began and may be
    // dropped into another: the pasted components kept the first as their
    // schematic - freed with it when it closed, and read by the next
    // recreate of a subcircuit (Save All, a GUI-monkey walk). A component
    // inserted into a schematic belongs to it.
    void aPastedComponentBelongsToTheDocumentItLandsIn()
    {
        write("paste_sub.sch", schematic(QStringLiteral(
            "  <Port P1 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
            "  <Port P2 1 200 100 -23 12 0 0 \"2\" 1 \"analog\" 0>\n")));
        write("paste_from.sch", schematic(sub("SUB1", "paste_sub.sch")));
        auto* source = new Schematic(nullptr, dir.filePath("paste_from.sch"));
        QVERIFY(source->load());
        for (auto* c : source->a_DocComps) c->isSelected = true;
        source->copy();
        QString text = QApplication::clipboard()->text();
        QTextStream stream(&text, QIODevice::ReadOnly);
        std::list<Element*> pasted;
        QVERIFY(source->paste(&stream, &pasted));

        Schematic target(nullptr, dir.filePath("paste_to.sch"));
        Component* comp = nullptr;
        for (auto* e : pasted) {
            if (auto* c = dynamic_cast<Component*>(e); c && comp == nullptr) comp = c;
            else delete e;
        }
        QVERIFY(comp != nullptr);
        QCOMPARE(comp->Ports.size(), qsizetype(2));
        QCOMPARE(comp->getSchematic(), source);
        target.insertComponent(comp);
        QCOMPARE(comp->getSchematic(), &target);

        delete source;
        comp->recreate();   // finds its file through its schematic
        QCOMPARE(comp->Ports.size(), qsizetype(2));
    }

    // The search dialog stays up while its document is closed; closing
    // the dialog then disconnected from the freed document.
    void theSearchDialogOutlivesItsDocument()
    {
        auto* doc = new TextDoc(nullptr, QString());
        SearchDialog dialog(nullptr);
        dialog.initSearch(doc, "x", false);
        delete doc;
        dialog.reject();
        dialog.initSearch(doc = new TextDoc(nullptr, QString()), "y", true);
        delete doc;
        dialog.reject();
    }
    // Save All recreates every instance of the saved subcircuit; one whose
    // two ports sat on one node (port symbols dragged onto each other)
    // reported that node orphaned for each port, and it was freed twice.
    // A wire of no length (how a node label is saved) has both its ends
    // on one node the same way.
    void aNodeSharedByTwoPortsIsFreedOnce()
    {
        Schematic sch(nullptr, QString());
        auto* r = new Resistor();
        auto* n = new Node(0, 0);
        for (Port* port : r->Ports) {
            port->Connection = n;
            n->connect(r);
        }
        sch.a_DocComps.push_back(r);
        sch.a_DocNodes.push_back(n);
        sch.deleteComp(r);   // frees n once (ASan)
        QVERIFY(sch.a_DocComps.empty());
        QVERIFY(sch.a_DocNodes.empty());

        auto* m = new Node(10, 10);
        auto* w = new Wire(m, m);
        sch.a_DocWires.push_back(w);
        sch.a_DocNodes.push_back(m);
        sch.deleteWire(w);   // frees m once
        QVERIFY(sch.a_DocWires.empty());
        QVERIFY(sch.a_DocNodes.empty());
    }

    // Healing moves one end of a wire onto the node of its other end and
    // lets go of that other end (GenericPort::replaceNodeWith()); later
    // actions of the same heal made nodes on the wire's line, and
    // provideNode() split the wire - towards the end it no longer had.
    void aWireThatLostAnEndIsNotSplit()
    {
        Schematic sch(nullptr, QString());
        auto* a = new Node(0, 0);
        auto* b = new Node(100, 0);
        auto* w = new Wire(a, b);
        b->disconnect(w);
        w->Port2 = nullptr;
        sch.a_DocWires.push_back(w);
        sch.a_DocNodes.push_back(a);
        Node* n = sch.provideNode(50, 0);
        QVERIFY(n != nullptr);
        QCOMPARE(sch.a_DocWires.size(), std::size_t(1));   // not split
        delete b;
    }

    // A subcircuit that includes itself, directly or through others: the
    // walks that gather the SPICE libraries, library files, Verilog-A
    // files and .spiceinit blocks of the hierarchy recursed until the
    // stack ran out (the netlister itself has always known each file once).
    void aSubcircuitThatIncludesItselfIsWalkedOnce()
    {
        const QString ports = QStringLiteral("  <Port P1 1 0 0 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                                             "  <Port P2 1 300 0 -23 12 0 0 \"2\" 1 \"analog\" 0>\n");
        write("self.sch", schematic(ports + sub("SUB1", "self.sch")));
        write("a.sch", schematic(ports + sub("SUB1", "b.sch")));
        write("b.sch", schematic(ports + sub("SUB1", "a.sch") + sub("SUB2", "b.sch")));
        write("top.sch", schematic(sub("SUB1", "self.sch") + sub("SUB2", "a.sch") + sub("SUB3", "a.sch")));
        Schematic top(nullptr, dir.filePath("top.sch"));
        QVERIFY(top.loadDocument());
        QCOMPARE(top.a_DocComps.size(), std::size_t(3));
        QVERIFY(AbstractSpiceKernel::collectSpiceLibs(&top).isEmpty());
        QVERIFY(AbstractSpiceKernel::collectSpiceLibraryFiles(&top).isEmpty());
        QVERIFY(AbstractSpiceKernel::collectVerilogAFiles(&top).isEmpty());
        QVERIFY(Ngspice::collectSpiceinit(&top).isEmpty());
    }

    // A value of a megabyte (a PWL list of many thousand points): the
    // patterns that recognise units backtracked over every split of its
    // digits - minutes per pattern, a netlist that never came.
    void aLongValueIsNormalizedQuickly()
    {
        QElapsedTimer clock;
        clock.start();
        const QString digits(200000, QChar('1'));
        QCOMPARE(spicecompat::normalize_value(digits + "Ohm"), digits);
        QCOMPARE(spicecompat::normalize_value(digits + "x"), digits + "X");
        QCOMPARE(spicecompat::normalize_value(digits + "M"), digits + "MEG");
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed()) + " ms"));
        // And what it always did.
        QCOMPARE(spicecompat::normalize_value("1 kOhm"), QStringLiteral("1K"));
        QCOMPARE(spicecompat::normalize_value("2.2 uF"), QStringLiteral("2.2U"));
        QCOMPARE(spicecompat::normalize_value("10M"), QStringLiteral("10MEG"));
        QCOMPARE(spicecompat::normalize_value("4.7k"), QStringLiteral("4.7K"));
        QCOMPARE(spicecompat::normalize_value("1e3/f0"), QStringLiteral("{1E3/F0}"));
        QCOMPARE(spicecompat::normalize_value("-3 dBm"), QStringLiteral("-3"));
    }

    // Shown on the canvas, such a value was measured and drawn whole on
    // every repaint; exported, the drawing it stretched asked for an
    // image of tens of gigabytes.
    void aLongValueIsShownShortAndExportedSmall()
    {
        const Property shortOne("R", "1k", true);
        QCOMPARE(shortOne.displayText(), QStringLiteral("R=1k"));
        const Property longOne("R", QString(1000000, QChar('9')), true);
        QCOMPARE(longOne.displayText().size(), qsizetype(2 + Property::MaxShownValue + 1));
        QVERIFY(longOne.displayText().endsWith(QChar(0x2026)));

        write("long.sch", schematic(QStringLiteral(
            "  <R R1 1 100 100 -26 15 0 0 \"%1\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
            .arg(QString(1000000, QChar('9')))));
        Schematic sch(nullptr, dir.filePath("long.sch"));
        QVERIFY(sch.loadDocument());
        qucs_s::graphicsexport::Options options;
        options.scale = 50;   // and asked for large, too
        const QSize size = qucs_s::graphicsexport::pixelSize(&sch, options);
        QVERIFY(size.width() <= qucs_s::graphicsexport::MaxImageSide);
        QVERIFY(size.height() <= qucs_s::graphicsexport::MaxImageSide);
        QVERIFY(double(size.width()) * size.height() <= qucs_s::graphicsexport::MaxImagePixels * 1.01);
        QVERIFY(!qucs_s::graphicsexport::image(&sch, options).isNull());
    }

    // A subcircuit port numbered 2147483647 (typed into a Port's number and
    // saved): its symbol made a pin for every number up to it - sixteen
    // million after the coordinate clamp - and took the unused ones out one
    // at a time, which never ended. A number below 1 read Ports.at(-1). Such
    // a symbol is refused now, and the instance gets the standard symbol
    // with a pin for each port.
    void aSubcircuitPinNumberedInTheBillionsLoads()
    {
        for (const QString number : {QStringLiteral("2147483647"), QStringLiteral("-1"), QStringLiteral("0")}) {
            write("pins_sub.sch", QStringLiteral(
                "<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,800,600,1,0,0>\n"
                "  <Grid=10,10,1>\n</Properties>\n<Symbol>\n  <.PortSym -30 0 1 0 A>\n"
                "  <.PortSym 30 0 2 180 B>\n  <.PortSym 0 30 %1 0 C>\n</Symbol>\n<Components>\n"
                "  <Port A 1 100 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                "  <Port B 1 200 100 -23 12 0 0 \"2\" 1 \"analog\" 0>\n"
                "  <Port C 1 300 100 -23 12 0 0 \"%1\" 1 \"analog\" 0>\n"
                "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n").arg(number));
            write("pins_top.sch", schematic(sub("SUB1", "pins_sub.sch")));

            QElapsedTimer t;
            t.start();
            Schematic sch(nullptr, dir.filePath("pins_top.sch"));
            QVERIFY(sch.loadDocument());
            QVERIFY2(t.elapsed() < 20000, qPrintable(number));
            QCOMPARE(sch.a_DocComps.size(), std::size_t(1));
            QCOMPARE(sch.a_DocComps.front()->Ports.size(), qsizetype(3));
        }
    }

    // Elements billions of units apart (a file or a dialog can put them
    // anywhere an int reaches) and a view that says anything: the canvas -
    // the model plane times the scale - was sized in int, and zooming out
    // on such a schematic overflowed it, Q3ScrollView's arithmetic and
    // QRect's width. The model plane now stays within Schematic::ModelLimit
    // of the origin, whatever is further is not scrolled to, and every
    // zoom, scroll and view of it is sane.
    void farAwayElementsDoNotOverflowTheCanvas()
    {
        const QString parts = QStringLiteral(
            "  <R R1 1 2000000000 2000000000 15 -26 0 0 \"1 kOhm\" 1>\n"
            "  <R R2 1 -2000000000 -2000000000 15 -26 0 0 \"1 kOhm\" 1>\n"
            "  <R R3 1 100 100 15 -26 0 0 \"1 kOhm\" 1>\n");
        const QStringList views = {QStringLiteral("0,0,800,600,1,0,0"),
                                   QStringLiteral("-2147483648,-2147483648,2147483647,2147483647,10,-2147483648,2147483647"),
                                   QStringLiteral("0,0,800,600,0,0,0"), QStringLiteral("5,5,-5,-5,nan,0,0"),
                                   QStringLiteral("0,0,800,600,1e300,0,0")};
        const auto sane = [](Schematic& sch, const QString& what) {
            const QRect model = sch.modelRect();
            const qint64 limit = Schematic::ModelLimit;
            QVERIFY2(model.left() >= -limit && model.right() <= limit && model.top() >= -limit && model.bottom() <= limit,
                     qPrintable(what));
            QVERIFY2(std::isfinite(sch.getScale()) && sch.getScale() >= 0.1 && sch.getScale() <= 10.0, qPrintable(what));
            QVERIFY2(sch.contentsWidth() > 0 && sch.contentsHeight() > 0, qPrintable(what));
            QVERIFY2(sch.contentsWidth() <= 2 * limit * 10 + 10000 && sch.contentsHeight() <= 2 * limit * 10 + 10000,
                     qPrintable(what));
        };
        // The geometry of wires out there: products of such coordinates
        // overflowed int.
        QVERIFY(qucs_s::geom::is_it_line(QPoint(8000000, 8000000), QPoint(0, 0), QPoint(-8000000, -8000000)));
        QVERIFY(!qucs_s::geom::is_it_line(QPoint(8000000, 8000000), QPoint(0, 1), QPoint(-8000000, -8000000)));
        QVERIFY(qucs_s::geom::is_it_line(QPoint(8388430, 360), QPoint(8388430, -8388940), QPoint(8388430, 16777000)));
        for (const QString& view : views) {
            QString text = schematic(parts);
            text.replace(QStringLiteral("<View=0,0,800,600,1,0,0>"), QStringLiteral("<View=%1>").arg(view));
            write("far.sch", text);
            Schematic sch(nullptr, dir.filePath("far.sch"));
            sch.resize(800, 600);
            sch.show();
            QVERIFY(sch.loadDocument());
            sane(sch, view + " loaded");
            for (int i = 0; i < 40; ++i) sch.zoomBy(0.5);
            sane(sch, view + " zoomed out");
            for (int i = 0; i < 40; ++i) sch.zoomBy(2.0);
            sane(sch, view + " zoomed in");
            sch.showAll();
            sane(sch, view + " all shown");
            for (auto* c : sch.a_DocComps) c->isSelected = true;
            sch.zoomToSelection();
            sane(sch, view + " selection");
            sch.showNoZoom();
            sane(sch, view + " no zoom");
            for (int i = 0; i < 200; ++i) {
                sch.scrollUp(5000);
                sch.scrollLeft(5000);
            }
            sane(sch, view + " scrolled up and left");
            for (int i = 0; i < 400; ++i) {
                sch.scrollDown(5000);
                sch.scrollRight(5000);
            }
            sane(sch, view + " scrolled down and right");
            sch.zoomAroundPoint(0.1, QPoint(799, 599), true);
            sch.zoomAroundPoint(100.0, QPoint(0, 0), true);
            sane(sch, view + " zoomed around a corner");
            // What is near the origin is still where it was.
            sch.centerOn(QPoint(100, 100));
            const QPoint at = sch.modelToViewport(QPoint(100, 100));
            QVERIFY2(sch.viewportRect().contains(at), qPrintable(view));
            // A far element is mapped without overflow (clamped).
            const QPoint far = sch.modelToViewport(QPoint(2000000000, 2000000000));
            QVERIFY(far.x() > 0 && far.y() > 0);
        }
    }

    // Healing went through the nodes in the order of their addresses (the
    // healer's std::map<Node*, ...>, and an unordered_set when a drag
    // began), and where two repairs interact the order decides: the same
    // edits of the same schematic ended differently from run to run. The
    // same edits of one schematic loaded twice - its nodes at other
    // addresses the second time - must end the same.
    void healingDoesNotDependOnWhereNodesAreInMemory()
    {
        const auto state = [](Schematic& sch) {
            QStringList lines;
            for (auto* c : sch.a_DocComps) lines << c->save();
            QStringList wires;
            for (auto* w : sch.a_DocWires) {
                QPoint a = w->P1(), b = w->P2();
                if (std::pair(b.x(), b.y()) < std::pair(a.x(), a.y())) std::swap(a, b);
                wires << QStringLiteral("%1 %2 %3 %4 %5").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y())
                                                          .arg(w->hasLabel() ? w->label()->Name : QString());
            }
            QStringList nodes;
            for (auto* n : sch.a_DocNodes)
                nodes << QStringLiteral("%1 %2 %3 %4 %5").arg(n->x()).arg(n->y()).arg(n->wires().size())
                                                          .arg(n->components().size()).arg(n->hasLabel() ? n->label()->Name : QString());
            wires.sort();
            nodes.sort();
            return lines.join('\n') + "\n" + wires.join('\n') + "\n" + nodes.join('\n');
        };
        // every k-th component and wire, the wires taken by place
        const auto select = [](Schematic& sch, int k, int offset) {
            int i = 0;
            for (auto* c : sch.a_DocComps) c->isSelected = (i++ % k) == offset;
            std::vector<Wire*> wires(sch.a_DocWires.begin(), sch.a_DocWires.end());
            std::ranges::stable_sort(wires, {}, [](Wire* w) {
                return std::tuple(std::min(w->x1, w->x2), std::min(w->y1, w->y2), std::max(w->x1, w->x2), std::max(w->y1, w->y2));
            });
            i = 0;
            for (auto* w : wires) w->isSelected = (i++ % k) == offset;
        };
        const auto edit = [&select](Schematic& sch, int step) {
            switch (step) {
            case 0: select(sch, 2, 1); sch.rotateElements(); break;
            case 1: select(sch, 3, 0); sch.mirrorXComponents(); break;
            case 2: select(sch, 2, 0); sch.decoupleElements(sch.currentSelection(), true);
                    sch.currentSelection().moveCenter(20, 10); sch.healAfterMousyMutation(); break;
            case 3: select(sch, 1, 0); sch.aligning(0); break;
            case 4: select(sch, 3, 1); sch.rotateElements(); break;
            default: select(sch, 1, 0); sch.distributeHorizontal(); break;
            }
        };

        std::mt19937 rng(11);
        for (const char* file : {"ngspice/General Electronics/Audio Amplifiers/audio_amp_thd.sch",
                                 "ngspice/Digital/flip_flops_truth_tables.sch",
                                 "xyce/Xyce_Examples/10-ActiveBesselFilter/Bessel7.sch"}) {
            Schematic first(nullptr, example(file));
            QVERIFY(first.load());

            // Put the second copy's nodes elsewhere: hold on to every
            // other block of a scrambled heap while it loads
            std::vector<std::unique_ptr<Node>> blocks;
            for (int i = 0; i < 20000; ++i) blocks.push_back(std::make_unique<Node>(0, 0));
            std::ranges::shuffle(blocks, rng);
            blocks.resize(blocks.size() / 2);
            Schematic second(nullptr, example(file));
            QVERIFY(second.load());
            blocks.clear();

            for (int step = 0; step < 6; ++step) {
                edit(first, step);
                edit(second, step);
                QVERIFY2(state(first) == state(second), qPrintable(QStringLiteral("%1, edit %2").arg(file).arg(step)));
            }
        }
    }
};

QTEST_MAIN(TestStressFindings)
#include "test_stress_findings.moc"
