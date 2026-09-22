/*
 * Where the DC bias labels go (biaslabels.h, upstream #1692): beside
 * their node, clear of symbols, texts, wires and one another where there
 * is room, the least in the way where there is not - with synthetic
 * obstacles, over the shipped examples, and as drawn.
 */
#include <QtTest>
#include <QDirIterator>
#include <QImage>
#include <QPainter>

#include "config.h"
#include "schematic.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "node.h"
#include "wire.h"
#include "biaslabels.h"
#include "components/component.h"
#include "extsimkernels/spicecompat.h"

using namespace qucs_s::bias;

namespace {

const QSize kBox(40, 16);

Label voltage(int x, int y) { return Label{QPoint(x, y), kBox, false}; }
Label current(int x, int y) { return Label{QPoint(x, y), kBox, true}; }

// Which side of its node a box is on.
QString sideOf(const QRect& box, const QPoint& anchor)
{
    QString s;
    s += box.bottom() < anchor.y() ? "upper" : box.top() > anchor.y() ? "lower" : "middle";
    s += box.right() < anchor.x() ? " left" : box.left() > anchor.x() ? " right" : " centre";
    return s;
}

int overlappingPairs(const QList<QRect>& boxes)
{
    int n = 0;
    for (int i = 0; i < boxes.size(); ++i)
        for (int j = i + 1; j < boxes.size(); ++j)
            if (boxes.at(i).intersects(boxes.at(j))) ++n;
    return n;
}

// The values SweepDialog::setBiasPoints() puts on the nodes after a DC
// run, made up: a voltage at each node with two connections or more and
// a component other than ground (the far ends of its wires left blank),
// a current at a probe's node.
int markBiasPoints(Schematic& doc)
{
    for (Node* pn : doc.a_DocNodes) {
        pn->Name = "net";
        pn->x1 = 0;
    }
    int n = 0;
    for (Node* pn : doc.a_DocNodes) {
        if (pn->Name.isEmpty()) continue;
        bool noComponent = true;
        for (Component* pc : pn->components()) {
            if (pc->Model == "GND") {
                noComponent = true;
                break;
            }
            noComponent = false;
        }
        if (pn->conn_count() < 2 || noComponent) {
            pn->Name = "";
            continue;
        }
        pn->Name = misc::num2str(1.2345 + n) + "V";
        ++n;
        for (Wire* pw : pn->wires()) (pw->Port1 != pn ? pw->Port1 : pw->Port2)->Name = "";
    }
    for (Component* pc : doc.a_DocComps) {
        if (pc->Model != "IProbe" || pc->Ports.size() < 2) continue;
        Node* pn = pc->Ports.first()->Connection;
        if (!pn->Name.isEmpty()) pn = pc->Ports.at(1)->Connection;
        pn->x1 = 0x10;
        pn->Name = misc::num2str(0.00123) + "A";
        ++n;
    }
    doc.setShowBias(1);
    return n;
}

// Where the labels used to go: a fixed offset from the node.
QList<QRect> fixedOffsets(const Schematic::BiasLabels& bias)
{
    QList<QRect> out;
    for (const Label& l : bias.labels) {
        const int x = l.anchor.x() + (l.current ? 10 : -10);
        const int y = l.anchor.y() + 4 - 10;
        out << QRect(x - l.size.width() / 2, y - l.size.height() / 2, l.size.width(), l.size.height());
    }
    return out;
}

struct Clutter {
    int labelPairs = 0;     // labels over labels
    int onSymbols = 0;      // labels over a symbol or its text
    int acrossWires = 0;    // labels a wire runs through
};

Clutter clutter(const Schematic& doc, const QList<QRect>& boxes)
{
    Clutter c;
    c.labelPairs = overlappingPairs(boxes);
    for (const QRect& b : boxes) {
        for (Component* pc : doc.a_DocComps)
            if (b.intersects(pc->boundingRectIncludingProperties())) {
                ++c.onSymbols;
                break;
            }
        const QRect inner = b.adjusted(1, 1, -1, -1);
        for (Wire* pw : doc.a_DocWires) {
            const QRect span = QRect(QPoint(pw->x1, pw->y1), QPoint(pw->x2, pw->y2)).normalized();
            if (inner.intersects(span.adjusted(0, 0, 1, 1))) {
                ++c.acrossWires;
                break;
            }
        }
    }
    return c;
}

} // namespace

class TestBiasLabels : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // --- the placement, with made-up obstacles -------------------------

    void withRoomALabelGoesWhereItAlwaysWent()
    {
        const QList<Placement> p = place({voltage(100, 100), current(300, 100)}, Obstacles());
        QCOMPARE(sideOf(p.at(0).box, {100, 100}), QStringLiteral("upper left"));   // a voltage
        QCOMPARE(sideOf(p.at(1).box, {300, 100}), QStringLiteral("upper right"));  // a current
        for (const Placement& q : p) {
            QCOMPARE(q.cost, 0.0);
            QVERIFY(!q.leader);
        }
        // Beside the node: a small gap, not over it.
        QVERIFY(!p.at(0).box.contains(QPoint(100, 100)));
        QVERIFY(QLineF(p.at(0).box.bottomRight(), QPointF(100, 100)).length() < 8);
    }

    void aSymbolPushesTheLabelToAnotherCorner()
    {
        Obstacles o;
        o.boxes << QRect(40, 60, 58, 38);   // a symbol to the upper left of the node
        const QList<Placement> p = place({voltage(100, 100)}, o);
        QCOMPARE(sideOf(p.first().box, {100, 100}), QStringLiteral("upper right"));
        QCOMPARE(p.first().cost, 0.0);
    }

    void aWireIsNotCrossed()
    {
        // A wire above the node, through both upper corners.
        Obstacles o;
        o.wires << QLine(0, 90, 200, 90);
        const QList<Placement> p = place({voltage(100, 100)}, o);
        QCOMPARE(sideOf(p.first().box, {100, 100}), QStringLiteral("lower left"));
        QCOMPARE(p.first().cost, 0.0);

        // The wires of the node itself leave the corners free.
        Obstacles own;
        own.wires << QLine(0, 100, 200, 100) << QLine(100, 0, 100, 200);
        QCOMPARE(sideOf(place({voltage(100, 100)}, own).first().box, {100, 100}), QStringLiteral("upper left"));

        // A wire along a box's edge is no harm; one right through it costs its area.
        const QRect box(0, 0, 40, 16);
        Obstacles edge, across;
        edge.wires << QLine(0, 16, 40, 16);
        across.wires << QLine(-10, 8, 50, 8);
        QCOMPARE(cost(box, edge, {}), 0.0);
        QCOMPARE(cost(box, across, {}), 40.0 * 16.0);
    }

    void labelsDoNotCoverOneAnother()
    {
        // Two nodes close together: the first one's usual corner would
        // sit on the second one's.
        const QList<Placement> p = place({voltage(100, 100), voltage(120, 100)}, Obstacles());
        QVERIFY(!p.at(0).box.intersects(p.at(1).box));
        QCOMPARE(p.at(0).cost + p.at(1).cost, 0.0);

        // A row of nodes on one wire, 20 apart: none on another.
        QList<Label> row;
        for (int i = 0; i < 6; ++i) row << voltage(100 + 20 * i, 100);
        Obstacles o;
        o.wires << QLine(80, 100, 220, 100);
        QList<QRect> boxes;
        for (const Placement& q : place(row, o)) boxes << q.box;
        QCOMPARE(overlappingPairs(boxes), 0);
    }

    void theLabelWithTheLeastRoomChoosesFirst()
    {
        // B (a voltage) has one free place, its upper left; A (a current)
        // would take that very spot as its own favourite, the upper right,
        // but has others. B chooses first.
        Obstacles o;
        o.boxes << QRect(160, 60, 38, 38)     // B's upper right, its top and its right
                << QRect(160, 102, 38, 38)    // B's lower right
                << QRect(102, 102, 38, 38);   // B's lower left, bottom and left; A's lower corners
        const QList<Placement> p = place({current(110, 100), voltage(156, 100)}, o);
        QCOMPARE(sideOf(p.at(1).box, {156, 100}), QStringLiteral("upper left"));
        QCOMPARE(p.at(1).cost, 0.0);
        QCOMPARE(sideOf(p.at(0).box, {110, 100}), QStringLiteral("upper left"));   // not its favourite
        QCOMPARE(p.at(0).cost, 0.0);
        QVERIFY(!p.at(0).box.intersects(p.at(1).box));
    }

    void aLabelIsNeverPutOnAnotherWhileThereIsAnyOtherPlace()
    {
        // Beside the node everything but one corner is under a symbol, and
        // that corner is taken by a label: the symbol is covered, not the
        // label.
        Obstacles o;
        o.boxes << QRect(0, 0, 400, 400);
        QList<Label> two = {voltage(200, 200), voltage(200, 200)};
        const QList<Placement> p = place(two, o);
        QVERIFY(!p.at(0).box.intersects(p.at(1).box));
    }

    void withNoRoomBesideItALabelStepsOutOrCoversTheLeast()
    {
        // Everything beside the node is taken, further out is free: a step
        // out, with a leader line.
        Obstacles crowded;
        crowded.boxes << QRect(40, 60, 120, 80);
        crowded.boxes.first() = QRect(52, 80, 96, 40);   // around the node, within the near ring
        const QList<Placement> out = place({voltage(100, 100)}, crowded);
        QVERIFY(out.first().leader);
        QCOMPARE(out.first().cost, 0.0);
        QVERIFY(!out.first().box.intersects(crowded.boxes.first()));

        // Nothing free anywhere: beside the node, where it covers the least.
        Obstacles full;
        full.boxes << QRect(0, 0, 200, 200);
        const QList<Placement> least = place({voltage(100, 100)}, full);
        QVERIFY(!least.first().leader);
        QVERIFY(least.first().cost > 0);
        QCOMPARE(sideOf(least.first().box, {100, 100}), QStringLiteral("upper left"));   // ties: the usual one

        // Partly free: the candidate that covers the least wins.
        Obstacles partly;
        partly.boxes << QRect(0, 0, 200, 99) << QRect(0, 110, 90, 90);   // all above, and lower left
        const QList<Placement> best = place({voltage(100, 100)}, partly);
        QCOMPARE(sideOf(best.first().box, {100, 100}), QStringLiteral("lower right"));
        QCOMPARE(best.first().cost, 0.0);
    }

    void theCandidatesAreBesideTheNodeThenAStepOut()
    {
        const QList<QRect> c = candidates(voltage(100, 100));
        QCOMPARE(c.size(), 16);
        for (int i = 0; i < 16; ++i) {
            QCOMPARE(c.at(i).size(), kBox);
            QVERIFY(!c.at(i).contains(QPoint(100, 100)));
        }
        QCOMPARE(sideOf(c.at(0), {100, 100}), QStringLiteral("upper left"));
        QCOMPARE(sideOf(candidates(current(100, 100)).at(0), {100, 100}), QStringLiteral("upper right"));
        QVERIFY(c.at(8).bottom() < c.at(0).bottom());   // the second ring is further out
    }

    // --- over the shipped examples ------------------------------------

    void theExamplesAreLessCluttered()
    {
        QStringList files;
        QDirIterator it(QStringLiteral(QUCS_EXAMPLES_DIR), {"*.sch"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        files.sort();
        QFont font;
        font.setPixelSize(12);
        const QFontMetrics metrics(font);

        Clutter before, after;
        int labels = 0, documents = 0, stepped = 0;
        QStringList worse;
        for (const QString& path : files) {
            Schematic doc(nullptr, path);
            if (!doc.load() || markBiasPoints(doc) == 0) continue;
            const Schematic::BiasLabels bias = doc.layoutBiasLabels(metrics);
            QList<QRect> placed;
            for (const Placement& p : bias.placements) {
                placed << p.box;
                if (p.leader) ++stepped;
            }
            const Clutter old = clutter(doc, fixedOffsets(bias)), now = clutter(doc, placed);
            before.labelPairs += old.labelPairs;
            before.onSymbols += old.onSymbols;
            before.acrossWires += old.acrossWires;
            after.labelPairs += now.labelPairs;
            after.onSymbols += now.onSymbols;
            after.acrossWires += now.acrossWires;
            labels += bias.labels.size();
            ++documents;
            if (now.labelPairs > old.labelPairs)
                worse << QDir(QStringLiteral(QUCS_EXAMPLES_DIR)).relativeFilePath(path);
        }
        qInfo().noquote() << QStringLiteral("%1 labels in %2 schematics; labels over labels %3 -> %4, "
                                            "over symbols or their text %5 -> %6, crossed by a wire %7 -> %8; "
                                            "%9 set a step apart")
                                 .arg(labels).arg(documents)
                                 .arg(before.labelPairs).arg(after.labelPairs)
                                 .arg(before.onSymbols).arg(after.onSymbols)
                                 .arg(before.acrossWires).arg(after.acrossWires).arg(stepped);
        QVERIFY(documents > 100);
        QVERIFY2(worse.isEmpty(), qPrintable(worse.join("\n")));
        QVERIFY(after.labelPairs < before.labelPairs);
        QVERIFY(after.onSymbols < before.onSymbols);
        QVERIFY(after.acrossWires < before.acrossWires);
    }

    // --- as drawn -------------------------------------------------------

    void theLabelsAreDrawnWhereTheyWerePlaced()
    {
        Schematic doc(nullptr, QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice/General Electronics/gain_phase_AC.sch"));
        QVERIFY(doc.load());
        QVERIFY(markBiasPoints(doc) > 0);
        const QRect all = doc.allBoundingRect().marginsAdded(QMargins(60, 60, 60, 60));
        QImage image(all.size(), QImage::Format_ARGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.translate(-all.topLeft());
        painter.setFont(QucsSettings.font);
        const Schematic::BiasLabels bias = doc.layoutBiasLabels(painter.fontMetrics());
        doc.paintSchToViewpainter(&painter, true);   // prints and exports show the bias too
        painter.end();
        QVERIFY(!bias.placements.isEmpty());
        int found = 0;
        for (const Placement& p : bias.placements) {
            // The box's grey just inside its top edge, above the text.
            const QPoint probe = QPoint(p.box.center().x(), p.box.top() + 1) - all.topLeft();
            if (image.pixelColor(probe) == QColor(230, 230, 230)) ++found;
        }
        QVERIFY2(found == bias.placements.size(), qPrintable(QStringLiteral("%1 of %2").arg(found).arg(bias.placements.size())));
    }
};

QTEST_MAIN(TestBiasLabels)
#include "test_bias_labels.moc"
