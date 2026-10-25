/*
 * The mesh of an RF 2.5D model (rf25/rf25_mesh.h), the last piece of phase
 * A: the conductors divided into cells, the size those cells come from, the
 * refinement at their edges, and the count a solver would have to hold -
 * which this build refuses when it is beyond it, rather than filling swap.
 * No fields are computed here and none are asked for.
 */
#include <QtTest>

#include "rf25/rf25_mesh.h"

namespace rf = qucs_s::rf25;

namespace {

rf::Node& add(rf::Node& parent, const char* type, const QString& tag, const QString& label = {})
{
    rf::Node n;
    n.type = QLatin1String(type);
    n.tag = tag;
    n.label = label;
    parent.children.push_back(n);
    return parent.children.back();
}

void set(rf::Node& node, const char* key, const QJsonValue& value)
{
    node.props.insert(QLatin1String(key), value);
}

/// A substrate of eps_r 4 and one metal, which is all a mesh needs.
rf::Model stacked()
{
    rf::Model m = rf::Model::blank();
    m.stackNode().children.clear();
    rf::Node& sub = add(m.stackNode(), "rf_substrate", QStringLiteral("sub1"), QStringLiteral("Si"));
    set(sub, "thickness", QStringLiteral("300[um]"));
    set(sub, "epsr", QStringLiteral("4"));
    rf::Node& ild = add(m.stackNode(), "rf_dielectric", QStringLiteral("d1"), QStringLiteral("ILD1"));
    set(ild, "thickness", QStringLiteral("1[um]"));
    set(ild, "epsr", QStringLiteral("4"));
    rf::Node& m1 = add(m.stackNode(), "rf_metal", QStringLiteral("m1"), QStringLiteral("M1"));
    set(m1, "thickness", QStringLiteral("0.6[um]"));
    return m;
}

rf::Conductor on(const QString& layer, double x, double y, double w, double h)
{
    rf::Conductor c;
    c.layer = layer;
    c.outline = QPolygonF(QRectF(x, y, w, h));
    return c;
}

bool says(const QStringList& lines, const char* words)
{
    for (const QString& l : lines)
        if (l.contains(QLatin1String(words), Qt::CaseInsensitive)) return true;
    return false;
}

} // namespace

class TestRf25Mesh : public QObject
{
    Q_OBJECT

private slots:
    // The settings: what the node says, and what it cannot say.
    void theMeshSettingsAreReadAndChecked()
    {
        rf::Model m = stacked();
        rf::Node& node = *m.root.child(QStringLiteral("rf_mesh"));
        QStringList problems;
        const rf::MeshSettings fresh = rf::meshSettingsOf(node, m.parameters(), &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
        QCOMPARE(fresh.perWavelength, 20);
        QVERIFY(fresh.edges);
        QCOMPARE(fresh.skin, 0);
        QCOMPARE(fresh.maximum, 0.0);

        set(node, "per_wavelength", 30);
        set(node, "edges", false);
        set(node, "maximum", QStringLiteral("2[um]"));
        const rf::MeshSettings given = rf::meshSettingsOf(node, m.parameters(), &problems);
        QCOMPARE(given.perWavelength, 30);
        QVERIFY(!given.edges);
        QCOMPARE(given.maximum, 2e-6);

        // Too few cells to a wavelength, a negative largest cell, and a
        // largest cell that is no number: each said, each put right.
        problems.clear();
        set(node, "per_wavelength", 2);
        set(node, "maximum", QStringLiteral("-1[um]"));
        const rf::MeshSettings odd = rf::meshSettingsOf(node, m.parameters(), &problems);
        QVERIFY2(says(problems, "too few"), qPrintable(problems.join(" | ")));
        QVERIFY2(says(problems, "above zero"), qPrintable(problems.join(" | ")));
        QCOMPARE(odd.perWavelength, 4);
        QCOMPARE(odd.maximum, 0.0);
        problems.clear();
        set(node, "maximum", QStringLiteral("2[um"));
        rf::meshSettingsOf(node, m.parameters(), &problems);
        QVERIFY2(says(problems, "is not a number"), qPrintable(problems.join(" | ")));
    }

    // The cell size comes from the wavelength at the study's top frequency,
    // in the fastest dielectric, and never exceeds the largest cell.
    void theCellSizeComesFromTheWavelength()
    {
        rf::Model m = stacked();
        rf::Node& study = *m.root.child(QStringLiteral("rf_study"));
        set(study, "from", QStringLiteral("1[GHz]"));
        set(study, "to", QStringLiteral("10[GHz]"));
        QCOMPARE(rf::topFrequency(m), 10e9);

        rf::MeshSettings how;
        how.edges = false;
        how.cellsAcross = 0;   // (the wavelength and the largest cell alone)
        // 10 GHz in eps_r 4: 30 mm / 2 = 15 mm, and 20 cells to it is 750 µm.
        const rf::Mesh mesh = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 2000, 100)}, how, m.stack(), rf::topFrequency(m));
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        QVERIFY2(std::abs(mesh.cellSize - 750) < 1, qPrintable(QString::number(mesh.cellSize)));
        QVERIFY2(says(mesh.lines, "wavelength at 10 GHz"), qPrintable(mesh.lines.join(" | ")));

        // The largest cell wins where it is smaller.
        how.maximum = 50e-6;   // 50 µm
        const rf::Mesh finer = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 2000, 100)}, how, m.stack(), rf::topFrequency(m));
        QCOMPARE(finer.cellSize, 50.0);
        QVERIFY2(says(finer.lines, "largest cell allowed"), qPrintable(finer.lines.join(" | ")));
        QVERIFY(finer.cells.size() > mesh.cells.size());

        // With neither a frequency nor a largest cell, it says so rather
        // than guessing.
        rf::MeshSettings nothing;
        nothing.edges = false;
        nothing.cellsAcross = 0;
        const rf::Mesh blind = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 10, 10)}, nothing, m.stack(), 0);
        QVERIFY(!blind.ok());
        QVERIFY2(says(blind.problems, "how large a cell may be"), qPrintable(blind.problems.join(" | ")));
    }

    // The cells: a grid cut against each conductor, which is exact on the
    // rectangles a layout is drawn with.
    void theConductorsAreCutIntoCells()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.edges = false;
        how.cellsAcross = 0;
        how.maximum = 1e-6;   // 1 µm cells
        const rf::Mesh mesh = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 4, 2)}, how, m.stack(), 0);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        QCOMPARE(mesh.cells.size(), 8);          // 4 by 2
        QCOMPARE(mesh.byLayer().value(QStringLiteral("M1")), 8);
        double area = 0;
        for (const rf::MeshCell& c : mesh.cells) {
            QVERIFY(c.area > 0);
            QCOMPARE(c.layer, QStringLiteral("M1"));
            area += c.area;
        }
        QVERIFY2(std::abs(area - 8.0) < 1e-6, qPrintable(QString::number(area)));   // the conductor's own
        QVERIFY2(says(mesh.lines, "8 cell(s) on 1 layer(s)"), qPrintable(mesh.lines.join(" | ")));

        // A conductor smaller than a cell is one cell, whatever the grid.
        const rf::Mesh tiny = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 0.3, 0.3)}, how, m.stack(), 0);
        QCOMPARE(tiny.cells.size(), 1);
        QVERIFY(std::abs(tiny.cells.first().area - 0.09) < 1e-9);

        // A shape that is not a rectangle: the cells outside it are gone
        // and the ones it cuts are trimmed, so the area is still its own.
        rf::Conductor ell;
        ell.layer = QStringLiteral("M1");
        ell.outline = QPolygonF({QPointF(0, 0), QPointF(3, 0), QPointF(3, 1), QPointF(1, 1),
                                 QPointF(1, 3), QPointF(0, 3)});
        const rf::Mesh cut = rf::meshOf({ell}, how, m.stack(), 0);
        QVERIFY2(cut.ok(), qPrintable(cut.problems.join(" | ")));
        QCOMPARE(cut.cells.size(), 5);   // (of the nine squares its bounds take)
        double ellArea = 0;
        for (const rf::MeshCell& c : cut.cells) ellArea += c.area;
        QVERIFY2(std::abs(ellArea - 5.0) < 1e-6, qPrintable(QString::number(ellArea)));
        // No cell of it lies where the conductor is not.
        for (const rf::MeshCell& c : cut.cells)
            QVERIFY2(ell.outline.containsPoint(c.centre, Qt::OddEvenFill),
                     qPrintable(QStringLiteral("a cell at %1,%2 is outside the conductor")
                                    .arg(c.centre.x()).arg(c.centre.y())));

        // Two layers are meshed apart, and nothing to mesh is said.
        const rf::Mesh both = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 2, 2), on(QStringLiteral("M1"), 9, 9, 2, 2)},
                                         how, m.stack(), 0);
        QCOMPARE(both.cells.size(), 8);
        const rf::Mesh none = rf::meshOf({}, how, m.stack(), 0);
        QVERIFY2(says(none.problems, "no shapes to mesh"), qPrintable(none.problems.join(" | ")));
    }

    // The edges: where the charge crowds, the outermost cells of each side
    // are half as wide - lines of the grid, not cells divided again, so
    // that every two neighbours still share a whole edge.
    void theCellsAtAnEdgeAreHalved()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.maximum = 1e-6;
        how.edges = false;
        how.cellsAcross = 0;
        const rf::Mesh plain = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 6, 6)}, how, m.stack(), 0);
        QCOMPARE(plain.cells.size(), 36);
        QVERIFY(!says(plain.lines, "half as wide"));

        how.edges = true;
        const rf::Mesh refined = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 6, 6)}, how, m.stack(), 0);
        // Lines at 0, 0.5, 1 ... 5, 5.5, 6: eight spaces a side.
        QCOMPARE(refined.cells.size(), 64);
        QVERIFY2(says(refined.lines, "half as wide"), qPrintable(refined.lines.join(" | ")));
        double area = 0, smallest = 1e9;
        for (const rf::MeshCell& c : refined.cells) {
            area += c.area;
            smallest = std::min(smallest, c.area);
        }
        QVERIFY2(std::abs(area - 36.0) < 1e-6, qPrintable(QString::number(area)));
        QVERIFY2(std::abs(smallest - 0.25) < 1e-9, qPrintable(QString::number(smallest)));   // a corner's
        // Every edge of a cell is either the conductor's or one whole edge
        // of one neighbour: 2 x 8 x 7 between them, no T-junction.
        QCOMPARE(refined.cost.unknowns, 2 * 8 * 7);
    }

    // Cells across a conductor: at least so many across its narrowest
    // part, whatever the wavelength - the phase B check found a 100 µm
    // strip one cell wide at the default, its Z0 12 % high - and a straight
    // strip's stretched along it, where the charge hardly changes.
    void aConductorHasCellsAcrossIt()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;   // ten across, four times as long, edges halved
        QCOMPARE(how.cellsAcross, 10);
        QCOMPARE(how.stretch, 4.0);
        // 10 GHz in eps_r 4: 750 µm cells by the wavelength, on a strip
        // 100 µm wide and 1000 long.
        const rf::Mesh strip = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 1000, 100)}, how, m.stack(), 10e9);
        QVERIFY2(strip.ok(), qPrintable(strip.problems.join(" | ")));
        // Across: 10 µm, the outermost 5 - twelve. Along: 40 µm, the
        // outermost 20 - twenty-seven.
        QCOMPARE(strip.cells.size(), 12 * 27);
        double narrowest = 1e9, longest = 0;
        for (const rf::MeshCell& c : strip.cells) {
            const QRectF b = c.outline.boundingRect();
            narrowest = std::min(narrowest, b.height());
            longest = std::max(longest, b.width());
        }
        QVERIFY2(std::abs(narrowest - 5) < 1e-9 && std::abs(longest - 40) < 1e-9,
                 qPrintable(QStringLiteral("%1 µm across, %2 along").arg(narrowest).arg(longest)));
        QVERIFY2(says(strip.lines, "10 cell(s) across the narrowest conductor (100 "), qPrintable(strip.lines.join(" | ")));
        QVERIFY2(says(strip.lines, "up to 4 times as long"), qPrintable(strip.lines.join(" | ")));
        // No stretch: square cells along it too.
        rf::MeshSettings square = how;
        square.stretch = 1;
        QCOMPARE(rf::meshOf({on(QStringLiteral("M1"), 0, 0, 1000, 100)}, square, m.stack(), 10e9).cells.size(), 12 * 102);
        // The largest cell still wins where it is smaller than that.
        rf::MeshSettings small = how;
        small.maximum = 5e-6;
        const rf::Mesh capped = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 1000, 100)}, small, m.stack(), 10e9);
        QCOMPARE(capped.cells.size(), 22 * 202);
        // A pad no strip: square cells, ten across its short side.
        const rf::Mesh pad = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 200, 100)}, how, m.stack(), 10e9);
        QCOMPARE(pad.cells.size(), 22 * 12);
        QVERIFY(!says(pad.lines, "times as long"));
        // A shape that is no rectangle goes by twice its area over its
        // perimeter, a trace's width: an L of 20 µm arms is 20 across.
        rf::Conductor ell;
        ell.layer = QStringLiteral("M1");
        ell.outline = QPolygonF({QPointF(0, 0), QPointF(200, 0), QPointF(200, 20), QPointF(20, 20),
                                 QPointF(20, 200), QPointF(0, 200)});
        const rf::Mesh bent = rf::meshOf({ell}, how, m.stack(), 10e9);
        QVERIFY2(bent.ok(), qPrintable(bent.problems.join(" | ")));
        double area = 0;
        for (const rf::MeshCell& c : bent.cells) area += c.area;
        QVERIFY2(std::abs(area - (200 * 20 + 180 * 20)) < 1e-6, qPrintable(QString::number(area)));
        QVERIFY2(says(bent.lines, "narrowest conductor (19 "), qPrintable(bent.lines.join(" | ")));
        // None at all: the wavelength alone, as before.
        rf::MeshSettings none = how;
        none.cellsAcross = 0;
        QVERIFY(rf::meshOf({on(QStringLiteral("M1"), 0, 0, 1000, 100)}, none, m.stack(), 10e9).cells.size() < 10);
    }

    // As many across as asked - or as many as the solver takes, said: a
    // layout too large for ten across a trace gets fewer, not a refusal.
    void aMeshTooLargeForTheSolverHasFewerAcross()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        // Forty strips 10 µm wide and 2 mm long: ten across is 40 x 12 x
        // 52, over 24 000 cells.
        QList<rf::Conductor> many;
        for (int i = 0; i < 40; ++i) many << on(QStringLiteral("M1"), 0, i * 30, 2000, 10);
        const rf::Mesh mesh = rf::meshOf(many, how, m.stack(), 10e9);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        QVERIFY2(mesh.cells.size() <= rf::kMostCharges, qPrintable(QString::number(mesh.cells.size())));
        QVERIFY2(mesh.cells.size() > rf::kMostCharges / 4, qPrintable(QString::number(mesh.cells.size())));
        QVERIFY2(says(mesh.lines, "10 cells across a conductor would be more than the 8000"), qPrintable(mesh.lines.join(" | ")));
        QVERIFY2(says(mesh.lines, "across instead"), qPrintable(mesh.lines.join(" | ")));
        // One that fits is not touched.
        const rf::Mesh one = rf::meshOf({many.first()}, how, m.stack(), 10e9);
        QVERIFY(!says(one.lines, "across instead"));
        // Where the largest cell sets the size, fewer across cannot help,
        // and the count is said as too many, with what would.
        rf::MeshSettings tiny = how;
        tiny.maximum = 4e-6;
        const rf::Mesh capped = rf::meshOf(many.mid(0, 20), tiny, m.stack(), 10e9);
        QVERIFY(!capped.ok());
        QVERIFY2(says(capped.problems, "more than this build will solve"), qPrintable(capped.problems.join(" | ")));
        QVERIFY2(says(capped.lines, "ask for a larger largest cell"), qPrintable(capped.lines.join(" | ")));
        QVERIFY(!says(capped.lines, "across instead"));
    }

    // The two new settings read from the node, and what is no setting.
    void theCellsAcrossAndTheStretchAreRead()
    {
        rf::Model m = stacked();
        rf::Node& node = m.mesh();
        QStringList problems;
        set(node, "cells_across", 16);
        set(node, "stretch", QStringLiteral("2.5"));
        const rf::MeshSettings how = rf::meshSettingsOf(node, m.parameters(), &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
        QCOMPARE(how.cellsAcross, 16);
        QCOMPARE(how.stretch, 2.5);
        set(node, "cells_across", -2);
        set(node, "stretch", QStringLiteral("0.5"));
        const rf::MeshSettings odd = rf::meshSettingsOf(node, m.parameters(), &problems);
        QVERIFY2(says(problems, "fewer than none"), qPrintable(problems.join(" | ")));
        QVERIFY2(says(problems, "a stretch of 1 at least"), qPrintable(problems.join(" | ")));
        QCOMPARE(odd.cellsAcross, 0);
        QCOMPARE(odd.stretch, 1.0);
        // And the node has them, with their defaults.
        const rf::Model blank = rf::Model::blank();
        QVERIFY2(blank.mesh().props.contains(QStringLiteral("cells_across")) || rf::integerOf(blank.mesh(), QStringLiteral("cells_across")) == 10,
                 "no cells_across on a new model's mesh");
        QCOMPARE(rf::integerOf(blank.mesh(), QStringLiteral("cells_across")), 10);
    }

    // The count a solver would hold: the edges two cells share, what a
    // dense matrix of them costs, and the refusal beyond what fits.
    void theUnknownsAreCountedAndTheirCostSaid()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.edges = false;
        how.cellsAcross = 0;
        how.maximum = 1e-6;
        // Two cells side by side share one edge: one unknown.
        const rf::Mesh two = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 2, 1)}, how, m.stack(), 0);
        QCOMPARE(two.cells.size(), 2);
        QCOMPARE(two.cost.unknowns, 1);
        // Four in a square share four: one between each neighbour.
        const rf::Mesh four = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 2, 2)}, how, m.stack(), 0);
        QCOMPARE(four.cells.size(), 4);
        QCOMPARE(four.cost.unknowns, 4);
        QVERIFY2(says(QStringList{four.cost.said}, "4 unknowns"), qPrintable(four.cost.said));
        QVERIFY2(says(QStringList{four.cost.said}, "MB"), qPrintable(four.cost.said));
        QVERIFY(!four.cost.tooBig);

        // Shapes that do not touch share nothing.
        const rf::Mesh apart = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 1, 1), on(QStringLiteral("M1"), 9, 9, 1, 1)},
                                          how, m.stack(), 0);
        QCOMPARE(apart.cost.unknowns, 0);

        // The cost of a mesh this build will not set up, said in full.
        const rf::MeshCost big = rf::costOf(100000);
        QVERIFY(big.tooBig);
        QVERIFY2(big.said.contains(QLatin1String("GB")), qPrintable(big.said));
        QVERIFY2(big.said.contains(QLatin1String("at most")), qPrintable(big.said));
        QVERIFY(!rf::costOf(1000).tooBig);

        // A conductor that would take more cells than the mesh may hold is
        // refused before it is cut, not after.
        rf::MeshSettings fine;
        fine.edges = false;
        fine.cellsAcross = 0;
        fine.maximum = 1e-9;   // 1 nm cells over 100 µm
        const rf::Mesh huge = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 100, 100)}, fine, m.stack(), 0);
        QVERIFY(!huge.ok());
        QVERIFY2(says(huge.problems, "would take"), qPrintable(huge.problems.join(" | ")));
        QVERIFY(huge.cells.isEmpty());
    }
};

QTEST_MAIN(TestRf25Mesh)
#include "test_rf25_mesh.moc"
