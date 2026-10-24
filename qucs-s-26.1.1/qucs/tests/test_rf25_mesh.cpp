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

    // The edges: where the current crowds, the cells are divided again -
    // and only there.
    void theCellsAtAnEdgeAreDividedAgain()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.maximum = 1e-6;
        how.edges = false;
        const rf::Mesh plain = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 6, 6)}, how, m.stack(), 0);
        QCOMPARE(plain.cells.size(), 36);
        QVERIFY(!says(plain.lines, "Divided"));

        how.edges = true;
        const rf::Mesh refined = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 6, 6)}, how, m.stack(), 0);
        // The 20 cells of the border become four each; the 16 inside stay.
        QCOMPARE(refined.cells.size(), 16 + 20 * 4);
        QVERIFY2(says(refined.lines, "Divided 20 cell(s)"), qPrintable(refined.lines.join(" | ")));
        double area = 0;
        for (const rf::MeshCell& c : refined.cells) area += c.area;
        QVERIFY2(std::abs(area - 36.0) < 1e-6, qPrintable(QString::number(area)));
    }

    // The count a solver would hold: the edges two cells share, what a
    // dense matrix of them costs, and the refusal beyond what fits.
    void theUnknownsAreCountedAndTheirCostSaid()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.edges = false;
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
        fine.maximum = 1e-9;   // 1 nm cells over 100 µm
        const rf::Mesh huge = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 100, 100)}, fine, m.stack(), 0);
        QVERIFY(!huge.ok());
        QVERIFY2(says(huge.problems, "would take"), qPrintable(huge.problems.join(" | ")));
        QVERIFY(huge.cells.isEmpty());
    }
};

QTEST_MAIN(TestRf25Mesh)
#include "test_rf25_mesh.moc"
