/*
 * Large schematics: loading, healing after an edit, undo, deleting,
 * dragging and pasting took time quadratic in the size of the document -
 * each pin found its node by comparing it with every node, each edit
 * compared every node with every other and with every wire, and elements
 * left the document's lists one search at a time. A 16,000-component
 * schematic froze for nine seconds after rotating one resistor.
 *
 * The lookups by place that replaced those loops are checked here against
 * the plain comparisons they replace, and the edits are timed at two sizes:
 * four times the elements must cost about four times the time, not
 * sixteen. (Pasting is not timed: see editsTakeTimeInProportionToTheSize.)
 */
#include <QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>
#include <optional>
#include <random>
#include <set>

#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "config.h"
#include "conductor_index.h"
#include "healer.h"
#include "wirelabel.h"
#include "geometry/multi_point.h"
#include "components/component.h"
#include "components/resistor.h"
#include "wire.h"
#include "node.h"
#include "extsimkernels/spicecompat.h"

namespace {

int g_violations = 0;
QtMessageHandler g_previousHandler = nullptr;

void countViolations(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(QStringLiteral("Invariant violated"))) ++g_violations;
    g_previousHandler(type, context, message);
}

// N resistors in rows of 100, joined into one chain by wires
QString chain(int n)
{
    QString comps = QStringLiteral("  <Vdc V1 1 -60 60 18 -26 0 1 \"5 V\" 1>\n  <GND * 1 -60 90 0 0 0 0>\n");
    QString wires = QStringLiteral("  <-60 30 70 30 \"\" 0 0 0 \"\">\n");
    for (int k = 0; k < n; ++k) {
        const int row = k / 100, col = k % 100;
        const int x = 100 + 100 * col, y = 30 + 100 * row;
        comps += QStringLiteral("  <R R%1 1 %2 %3 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n")
                     .arg(k + 1).arg(x).arg(y);
        if (col < 99 && k < n - 1) {
            wires += QStringLiteral("  <%1 %2 %3 %2 \"\" 0 0 0 \"\">\n").arg(x + 30).arg(y).arg(x + 70);
        } else if (k < n - 1) {
            wires += QStringLiteral("  <%1 %2 %1 %3 \"\" 0 0 0 \"\">\n").arg(x + 30).arg(y).arg(y + 50);
            wires += QStringLiteral("  <70 %1 %2 %1 \"\" 0 0 0 \"\">\n").arg(y + 50).arg(x + 30);
            wires += QStringLiteral("  <70 %1 70 %2 \"\" 0 0 0 \"\">\n").arg(y + 50).arg(y + 100);
        }
    }
    return QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,2000,2000,1,0,0>\n"
                          "  <Grid=10,10,1>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n")
           + comps + QStringLiteral("</Components>\n<Wires>\n") + wires
           + QStringLiteral("</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n");
}

void selectNone(Schematic& sch)
{
    for (auto* c : sch.a_DocComps) c->isSelected = false;
    for (auto* w : sch.a_DocWires) w->isSelected = false;
}

void selectEvery(Schematic& sch, int k)
{
    int i = 0;
    for (auto* c : sch.a_DocComps) c->isSelected = (i++ % k) == 0;
    i = 0;
    for (auto* w : sch.a_DocWires) w->isSelected = (i++ % k) == 0;
}

std::size_t orphans(const Schematic& sch)
{
    return std::ranges::count_if(sch.a_DocNodes, [](const Node* n) { return n->conn_count() == 0; });
}

// What a healing plan would do, written down (not done).
struct PlanRecorder : qucs_s::SchematicMutator {
    QStringList steps;
    static QString at(const QPoint& p) { return QStringLiteral("(%1,%2)").arg(p.x()).arg(p.y()); }
    static QString of(const void* p) { return QString::number(quintptr(p), 16); }
    static QString port(const qucs_s::GenericPort* port)
    {
        const void* host = port->isOfWire() ? static_cast<const void*>(port->hostWire())
                                            : static_cast<const void*>(port->hostComponent());
        return of(host) + at(port->center()) + of(port->node());
    }
    void deleteWire(Wire* w) override { steps << "delete " + of(w); }
    void connectWithWire(const QPoint& a, const QPoint& b) override { steps << "connect " + at(a) + at(b); }
    void putLabel(WireLabel* l, Node* n) override { steps << "label " + of(l) + " " + of(n); }
    void moveNode(Node* n, const QPoint& p) override { steps << "node " + of(n) + at(p); }
    void movePort(qucs_s::GenericPort* p, const QPoint& to) override { steps << "port " + port(p) + at(to); }
    void replaceNode(qucs_s::GenericPort* p) override { steps << "replace " + port(p); }
};

QStringList plan(const std::list<Component*>& components, const std::list<Wire*>& wires, bool reshaping)
{
    const qucs_s::Healer healer{&components, &wires, {.allowWireReshaping = reshaping, .allowWireRelaying = false, .wireRelayingDepth = 2}};
    PlanRecorder recorder;
    for (auto& action : healer.planHealing()) action->execute(&recorder);
    return recorder.steps;
}

} // namespace

class TestLargeSchematics : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString write(const QString& name, const QString& text)
    {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(text.toUtf8());
        return f.fileName();
    }

    // The fastest of three runs of what f does to a fresh load of the file
    template <typename F>
    qint64 bestOfThree(const QString& file, F f)
    {
        qint64 best = std::numeric_limits<qint64>::max();
        for (int run = 0; run < 3; ++run) {
            Schematic sch(nullptr, file);
            QElapsedTimer t;
            t.start();
            f(sch, t);
            best = std::min(best, t.elapsed());
        }
        return best;
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
        g_previousHandler = qInstallMessageHandler(countViolations);
    }

    // NodesByPlace::between() finds the nodes geom::is_between() puts on a
    // segment - horizontal, vertical, diagonal, of no length - and no others,
    // also when several nodes share a place.
    void theNodesOnASegmentAreFoundByPlace()
    {
        std::mt19937 rng(20260925);
        const auto coordinate = [&rng](int range) { return int(rng() % (2 * range + 1)) - range; };

        std::list<Node*> nodes;
        for (int i = 0; i < 600; ++i) {
            nodes.push_back(new Node(10 * coordinate(20), 10 * coordinate(20)));
        }
        const qucs_s::NodesByPlace byPlace{nodes};

        for (int i = 0; i < 3000; ++i) {
            QPoint a{10 * coordinate(25), 10 * coordinate(25)};
            QPoint b = a;
            switch (i % 5) {
            case 0: b.rx() += 10 * coordinate(25); break;                    // horizontal
            case 1: b.ry() += 10 * coordinate(25); break;                    // vertical
            case 2: { const int d = 10 * coordinate(20); b += QPoint{d, d}; break; }
            case 3: { const int d = 10 * coordinate(20); b += QPoint{d, -2 * d}; break; }
            default: b = {coordinate(250), coordinate(250)}; break;          // anywhere
            }

            std::set<Node*> expected;
            for (auto* n : nodes)
                if (qucs_s::geom::is_between(n, a, b)) expected.insert(n);
            const auto found = byPlace.between(a, b);
            const std::set<Node*> got(found.begin(), found.end());
            QVERIFY2(got.size() == found.size(), "a node found twice");
            QVERIFY2(got == expected, qPrintable(QStringLiteral("segment (%1,%2)-(%3,%4)")
                                                     .arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y())));
        }
        qDeleteAll(nodes);
    }

    // InsertionIndex: the node at a place is the first one the list has
    // there, and the wires near a place include every wire a node there
    // would split - short, long, diagonal, at negative coordinates, added
    // before or after the index was made.
    void theInsertionIndexFindsWhatTheListsWould()
    {
        std::mt19937 rng(7);
        const auto coordinate = [&rng](int range) { return int(rng() % (2 * range + 1)) - range; };
        const auto randomWire = [&]() {
            const int x = 10 * coordinate(60), y = 10 * coordinate(60);
            const int len = 10 * (1 + int(rng() % (rng() % 4 == 0 ? 300 : 20)));
            switch (rng() % 3) {
            case 0: return new Wire(x, y, x + len, y);
            case 1: return new Wire(x, y, x, y - len);
            default: return new Wire(x, y, x + len, y + len);
            }
        };

        std::list<Node*> nodes;
        std::list<Wire*> wires;
        for (int i = 0; i < 400; ++i) nodes.push_back(new Node(10 * coordinate(30), 10 * coordinate(30)));
        for (int i = 0; i < 300; ++i) wires.push_back(randomWire());
        qucs_s::InsertionIndex index{nodes, wires};
        for (int i = 0; i < 200; ++i) {
            nodes.push_back(new Node(10 * coordinate(30), 10 * coordinate(30)));
            index.add(nodes.back());
            wires.push_back(randomWire());
            index.add(wires.back());
        }

        for (int i = 0; i < 5000; ++i) {
            const QPoint p{10 * coordinate(80), 10 * coordinate(80)};
            Node* first = nullptr;
            for (auto* n : nodes)
                if (n->center() == p) { first = n; break; }
            QCOMPARE(index.nodeAt(p), first);

            const auto near = index.wiresNear(p);
            const std::set<Wire*> nearSet(near.begin(), near.end());
            for (auto* w : wires)
                if (qucs_s::geom::is_between(p, w->P1(), w->P2()))
                    QVERIFY2(nearSet.contains(w), qPrintable(QStringLiteral("(%1,%2) on (%3,%4)-(%5,%6)")
                                                                 .arg(p.x()).arg(p.y()).arg(w->x1).arg(w->y1).arg(w->x2).arg(w->y2)));
        }
        qDeleteAll(nodes);
        qDeleteAll(wires);
    }

    // While loading, a new node splits the wires loaded before that it
    // lies on, as it did when provideNode() went through all wires.
    void loadingSplitsAWireAtANewNode()
    {
        const QString file = write(QStringLiteral("split.sch"),
            QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,800,600,1,0,0>\n"
                           "  <Grid=10,10,1>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n</Components>\n<Wires>\n"
                           "  <0 0 200 0 \"\" 0 0 0 \"\">\n"
                           "  <0 -100 200 100 \"\" 0 0 0 \"\">\n"
                           "  <100 0 100 50 \"\" 0 0 0 \"\">\n"
                           "  <100 0 100 -80 \"\" 0 0 0 \"\">\n"
                           "</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n"));
        Schematic sch(nullptr, file);
        QVERIFY(sch.load());
        // (100,0) splits the horizontal and the diagonal wire, which cross there
        QCOMPARE(sch.a_DocWires.size(), std::size_t(6));
        Node* middle = nullptr;
        for (auto* n : sch.a_DocNodes)
            if (n->center() == QPoint(100, 0)) middle = n;
        QVERIFY(middle != nullptr);
        QCOMPARE(middle->conn_count(), std::size_t(6));
        QCOMPARE(orphans(sch), std::size_t(0));
    }

    // Pasting numbers the components from a table of the numbers in use,
    // with the same result as looking through all components for each - also
    // when a new name counts for another prefix ("R+8" for "R").
    void pastedComponentsAreNumberedAsOneByOne()
    {
        const QString file = write(QStringLiteral("names.sch"),
            QStringLiteral("<Qucs Schematic " PACKAGE_VERSION ">\n<Properties>\n  <View=0,0,800,600,1,0,0>\n"
                           "  <Grid=10,10,1>\n</Properties>\n<Symbol>\n</Symbol>\n<Components>\n"
                           "  <R R1 1 100 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                           "  <R R3 1 200 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                           "  <R R+7 1 300 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                           "  <R Rload 1 400 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n"
                           "</Components>\n<Wires>\n</Wires>\n<Diagrams>\n</Diagrams>\n<Paintings>\n</Paintings>\n"));
        const QStringList prefixes = {"R", "R+", "R", "Rload", "R+", "R", "Q", "Rload", "R"};

        QStringList names[2];
        for (int bulk = 0; bulk < 2; ++bulk) {
            Schematic sch(nullptr, file);
            QVERIFY(sch.load());
            std::optional<Schematic::BulkNaming> naming;
            if (bulk) naming.emplace(&sch);
            int x = 0;
            for (const QString& prefix : prefixes) {
                auto* r = new Resistor();
                r->Name = prefix;
                r->moveCenterTo(x += 100, 500);
                sch.insertComponent(r);
                names[bulk] << r->Name;
            }
        }
        QCOMPARE(names[0], (QStringList{"R8", "R+8", "R9", "Rload1", "R+9", "R10", "Q1", "Rload2", "R11"}));
        QCOMPARE(names[1], names[0]);
    }

    // Deleting a selection and dragging everything leave no node that
    // nothing is connected to, and the healing after them finds nothing wrong.
    void deletingAndDraggingLeaveTheSchematicWhole()
    {
        const QString file = write(QStringLiteral("whole.sch"), chain(500));
        Schematic sch(nullptr, file);
        QVERIFY(sch.load());
        g_violations = 0;

        selectEvery(sch, 3);
        QVERIFY(sch.deleteElements());
        QCOMPARE(orphans(sch), std::size_t(0));

        selectEvery(sch, 1);
        const std::size_t nodes = sch.a_DocNodes.size();
        sch.decoupleElements(sch.currentSelection(), /*keepNodeLabel=*/true);
        QCOMPARE(orphans(sch), std::size_t(0));
        sch.currentSelection().moveCenter(20, 10);
        sch.healAfterMousyMutation();
        QCOMPARE(sch.a_DocNodes.size(), nodes);
        QCOMPARE(orphans(sch), std::size_t(0));
        QCOMPARE(g_violations, 0);
    }

    // At each step of a drag the canvas shows what healing will do once it
    // is dropped: planned from the elements joined to a node something has
    // left (and the selection), not from the whole schematic - the plan
    // must be the one the whole schematic gives, step for step. Selections
    // of components, of wires, of both, dragged over steps; with trouble
    // elsewhere too, left by an element moved without healing.
    void aDragPreviewPlansWhatTheWholeSchematicWould()
    {
        const QString file = write(QStringLiteral("preview.sch"), chain(300));
        std::mt19937 rng(20260926);
        int planned = 0;
        for (int round = 0; round < 40; ++round) {
            Schematic sch(nullptr, file);
            QVERIFY(sch.load());
            selectNone(sch);
            std::vector<Component*> comps(sch.a_DocComps.begin(), sch.a_DocComps.end());
            std::vector<Wire*> wires(sch.a_DocWires.begin(), sch.a_DocWires.end());
            if (round % 4 == 3) {
                // Trouble away from the selection
                Component* stray = comps[rng() % comps.size()];
                stray->moveCenter(10, 0);
            }
            const int picks = 1 + int(rng() % 6);
            for (int k = 0; k < picks; ++k) {
                if (round % 3 != 1) comps[rng() % comps.size()]->isSelected = true;
                if (round % 3 != 0) wires[rng() % wires.size()]->isSelected = true;
            }
            for (int step = 0; step < 4; ++step) {
                sch.currentSelection().moveCenter(10 * (int(rng() % 5) - 2), 10 * (int(rng() % 5) - 2));
                const qucs_s::HealingScope scope = qucs_s::scopeOfTrouble(sch.a_DocComps, sch.a_DocWires);
                QVERIFY(scope.components.size() < sch.a_DocComps.size());
                for (const bool reshaping : {true, false}) {
                    const QStringList whole = plan(sch.a_DocComps, sch.a_DocWires, reshaping);
                    QCOMPARE(plan(scope.components, scope.wires, reshaping), whole);
                    planned += int(whole.size());
                }
            }
        }
        QVERIFY(planned > 100);   // the drags did leave something to heal
    }

    // Four times the elements, about four times the time (sixteen when
    // quadratic; a bound of ten leaves room for a noisy machine). Not
    // pasting: it walks the document once for each element pasted, by
    // design, and a hundred such walks slow down more than fourfold once the
    // lists outgrow the processor's cache (13 times on a CI runner).
    // pastedComponentsAreNumberedAsOneByOne covers its numbering table.
    void editsTakeTimeInProportionToTheSize_data()
    {
        QTest::addColumn<QString>("edit");
        QTest::newRow("load") << "load";
        QTest::newRow("rotate one") << "rotate";
        QTest::newRow("undo") << "undo";
        QTest::newRow("delete a third") << "delete";
        QTest::newRow("drag all") << "drag";
    }

    void editsTakeTimeInProportionToTheSize()
    {
        QFETCH(QString, edit);
        const int small = 2000, large = 8000;
        const QString files[] = {write(QStringLiteral("chain_small.sch"), chain(small)),
                                 write(QStringLiteral("chain_large.sch"), chain(large))};
        qint64 times[2];
        for (int i = 0; i < 2; ++i) {
            times[i] = bestOfThree(files[i], [&edit](Schematic& sch, QElapsedTimer& t) {
                QVERIFY(sch.load());
                selectNone(sch);
                if (edit != "load") t.restart();

                if (edit == "rotate" || edit == "undo") {
                    sch.a_DocComps.back()->isSelected = true;
                    QVERIFY(sch.rotateElements());
                    if (edit == "undo") {
                        t.restart();
                        QVERIFY(sch.undo());
                    }
                } else if (edit == "delete") {
                    selectEvery(sch, 3);
                    QVERIFY(sch.deleteElements());
                } else if (edit == "drag") {
                    selectEvery(sch, 1);
                    sch.decoupleElements(sch.currentSelection(), /*keepNodeLabel=*/true);
                    sch.currentSelection().moveCenter(0, 20);
                    sch.healAfterMousyMutation();
                }
            });
        }
        qInfo("%s: %lld ms for %d resistors, %lld ms for %d", qPrintable(edit),
              times[0], small, times[1], large);
        // Too fast to tell apart from the timer's resolution: nothing to see.
        // (And a smaller time below 10 ms counts as 10: it is mostly noise.)
        if (times[1] < 40) return;
        QVERIFY2(times[1] <= 10 * std::max<qint64>(times[0], 10),
                 qPrintable(QStringLiteral("%1 ms for %2 resistors but %3 ms for %4")
                                .arg(times[0]).arg(small).arg(times[1]).arg(large)));
    }
};

QTEST_MAIN(TestLargeSchematics)
#include "test_large_schematics.moc"
