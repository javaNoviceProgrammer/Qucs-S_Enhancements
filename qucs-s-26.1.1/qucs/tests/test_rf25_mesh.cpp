/*
 * The mesh of an RF 2.5D model (rf25/rf25_mesh.h), the last piece of phase
 * A: the conductors divided into cells, the size those cells come from, the
 * refinement at their edges, and the count a solver would have to hold -
 * which this build refuses when it is beyond it, rather than filling swap.
 * No fields are computed here and none are asked for.
 */
#include <QtTest>

#include "rf25/rf25_fullwave.h"
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

    // Where a conductor faces another across a gap narrower than its
    // outermost cells, those cells are halved until they are no wider than
    // a quarter of the gap: the charge crowds on both edges of a narrow gap,
    // and the edges' one halving left a coplanar line's 6 µm gaps, between a
    // signal of 50 µm and grounds of 150, with one cell of 3 µm on one side
    // and one of 9 on the other - its capacitance 10 % short (the review of
    // 3 November; test_rf25_static has the line against its references).
    // A side that faces nothing, or a gap as wide as its cells, is graded as
    // before, and with no grading at the edges there is none here either.
    // The quasi-static solve's mesh, which the tab asks it of for a
    // quasi-static study: not a full-wave one's yet, whose conductors' loss
    // at their edges was fitted on the edges' one halving.
    void aSideFacingANarrowGapIsGradedToAQuarterOfIt()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.cellsAcross = 4;
        how.maximum = 25e-6;
        const QList<rf::Conductor> line{on(QStringLiteral("M1"), 0, -25, 1000, 50), on(QStringLiteral("M1"), 0, 31, 1000, 150),
                                        on(QStringLiteral("M1"), 0, -181, 1000, 150)};
        // (Not for a full-wave solve yet, nor asked: as before.)
        QVERIFY(!how.gaps);
        const rf::Mesh before = rf::meshOf(line, how, m.stack(), 0);
        QVERIFY2(!says(before.lines, "across a gap"), qPrintable(before.lines.join(" | ")));
        how.gaps = true;
        const rf::Mesh mesh = rf::meshOf(line, how, m.stack(), 0);
        QVERIFY(mesh.cells.size() > before.cells.size());
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        QVERIFY2(says(mesh.lines, "4 side(s) face another conductor across a gap"), qPrintable(mesh.lines.join(" | ")));
        // How wide in y the cells are that touch a line y = at, from the
        // side of it that \a above says.
        const auto widthAt = [&mesh](double at, bool above) {
            double widest = 0;
            for (const rf::MeshCell& c : mesh.cells) {
                const QRectF b = c.outline.boundingRect();
                if (above ? std::abs(b.top() - at) < 1e-9 : std::abs(b.bottom() - at) < 1e-9) widest = std::max(widest, b.height());
            }
            return widest;
        };
        // The signal's two sides and each ground's inner one: a quarter of
        // 6 µm at most (from 6.25 µm halved: 3.125, 1.5625, then 0.78).
        for (const double w : {widthAt(25, false), widthAt(-25, true), widthAt(31, true), widthAt(-31, false)})
            QVERIFY2(w > 0 && w <= 1.5 + 1e-9, qPrintable(QString::number(w)));
        // Each ground's outer side faces nothing: its cells as before, half
        // of the 25 µm its cells are (the largest cell, six across it).
        QVERIFY2(std::abs(widthAt(181, false) - 12.5) < 1e-9, qPrintable(QString::number(widthAt(181, false))));
        QVERIFY2(std::abs(widthAt(-181, true) - 12.5) < 1e-9, qPrintable(QString::number(widthAt(-181, true))));
        // A gap as wide as the cells: graded as before.
        const QList<rf::Conductor> apart{on(QStringLiteral("M1"), 0, -25, 1000, 50), on(QStringLiteral("M1"), 0, 75, 1000, 150)};
        const rf::Mesh wide = rf::meshOf(apart, how, m.stack(), 0);
        QVERIFY2(!says(wide.lines, "across a gap"), qPrintable(wide.lines.join(" | ")));
        // No grading at the edges, none at the gaps.
        how.edges = false;
        const rf::Mesh plain = rf::meshOf(line, how, m.stack(), 0);
        QVERIFY2(!says(plain.lines, "across a gap"), qPrintable(plain.lines.join(" | ")));
        QVERIFY(plain.cells.size() < mesh.cells.size());
        // And the charges a quasi-static solve has: one a cell on a sheet;
        // twice over on a metal solved as its two faces, which the tab's
        // status line counts (it counted the cells alone: 924 where the
        // solve had 1848, a matrix a quarter of the size it filled).
        QCOMPARE(mesh.charges, int(mesh.cells.size()));
        rf::Model faced = stacked();
        faced.stackNode().children.back().props.insert(QStringLiteral("model"), QStringLiteral("faces"));
        const rf::Mesh two = rf::meshOf(line, how, faced.stack(), 0);
        QVERIFY2(faced.stack().layers.last().twoSheets, "the metal is not solved as its faces");
        QCOMPARE(two.charges, 2 * int(two.cells.size()));
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
        // A shape whose sides all run along x or y is cut into the
        // rectangles it is made of, each a strip of its own: an L of 20 µm
        // arms is its two arms, 20 across each and stretched along them. (It
        // was a grid of the whole, by twice its area over its perimeter -
        // 19 µm - and square: the phase C check of 1 November, F1.)
        rf::Conductor ell;
        ell.layer = QStringLiteral("M1");
        ell.outline = QPolygonF({QPointF(0, 0), QPointF(200, 0), QPointF(200, 20), QPointF(20, 20),
                                 QPointF(20, 200), QPointF(0, 200)});
        const rf::Mesh bent = rf::meshOf({ell}, how, m.stack(), 10e9);
        QVERIFY2(bent.ok(), qPrintable(bent.problems.join(" | ")));
        double area = 0;
        for (const rf::MeshCell& c : bent.cells) area += c.area;
        QVERIFY2(std::abs(area - (200 * 20 + 180 * 20)) < 1e-6, qPrintable(QString::number(area)));
        QVERIFY2(says(bent.lines, "narrowest conductor (20 "), qPrintable(bent.lines.join(" | ")));
        QVERIFY2(says(bent.lines, "cut into the 2 rectangle(s)"), qPrintable(bent.lines.join(" | ")));
        QVERIFY2(says(bent.lines, "times as long"), qPrintable(bent.lines.join(" | ")));
        // A shape with a slanted side is cut between its corners into
        // rectangles and trapezoids, each going by its own width (B4 of the
        // open items of 8 November): a right triangle of 200 µm legs, one
        // trapezoid, by its mean width across its slab, 100 µm. (It went by
        // twice its area over its perimeter whole, 58.58.)
        rf::Conductor wedge;
        wedge.layer = QStringLiteral("M1");
        wedge.outline = QPolygonF({QPointF(0, 0), QPointF(200, 0), QPointF(0, 200)});
        const rf::Mesh slanted = rf::meshOf({wedge}, how, m.stack(), 10e9);
        QVERIFY2(says(slanted.lines, "narrowest conductor (100 ") && says(slanted.lines, "into 0 rectangle(s)")
                     && says(slanted.lines, "1 part(s) along the slanted sides"),
                 qPrintable(slanted.lines.join(" | ")));
        // None at all: the wavelength alone, as before.
        rf::MeshSettings none = how;
        none.cellsAcross = 0;
        QVERIFY(rf::meshOf({on(QStringLiteral("M1"), 0, 0, 1000, 100)}, none, m.stack(), 10e9).cells.size() < 10);
    }

    // The phase C check of 1 November, F1: a tee merged into one outline
    // meshes as its two strips do - 1120 cells at four across, refused by
    // the full-wave solve, where its strips are 342 - and where they meet,
    // every cell of one shares a whole edge with a cell of the other. So do
    // two touching shapes left unmerged, which met through one run of
    // current, where two of their grid lines happened to coincide.
    void aTeeIsItsStripsMeetingEdgeToEdge()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.cellsAcross = 4;
        how.stretch = 4;
        const rf::Conductor line = on(QStringLiteral("M1"), 0, -50, 3000, 100);
        const rf::Conductor stub = on(QStringLiteral("M1"), 1450, 50, 100, 1800);
        rf::Conductor tee;
        tee.layer = QStringLiteral("M1");
        tee.outline = QPolygonF({QPointF(0, -50), QPointF(3000, -50), QPointF(3000, 50), QPointF(1550, 50),
                                 QPointF(1550, 1850), QPointF(1450, 1850), QPointF(1450, 50), QPointF(0, 50)});
        const rf::Mesh merged = rf::meshOf({tee}, how, m.stack(), 25e9);
        const rf::Mesh apart = rf::meshOf({line, stub}, how, m.stack(), 25e9);
        QVERIFY2(merged.ok() && apart.ok(), qPrintable((merged.problems + apart.problems).join(" | ")));
        QCOMPARE(merged.cells.size(), apart.cells.size());
        QVERIFY2(merged.cells.size() < 400, qPrintable(QString::number(merged.cells.size())));
        QVERIFY2(says(merged.lines, "narrowest conductor (100 "), qPrintable(merged.lines.join(" | ")));
        // Its line and its stub, two: cut across x it would be three, the
        // line in two halves and a column through it - meshed as well,
        // edge to edge, but with more lines forced where they meet.
        QVERIFY2(says(merged.lines, "cut into the 2 rectangle(s)"), qPrintable(merged.lines.join(" | ")));
        // The joint: as many edges shared across y = 50 as the stub has
        // cells along its foot, the whole 100 µm of it.
        for (const rf::Mesh* mesh : {&merged, &apart}) {
            QHash<QString, int> edges;
            double foot = 0;
            int stubCells = 0;
            for (const rf::MeshCell& c : mesh->cells) {
                const QRectF b = c.outline.boundingRect();
                if (std::abs(b.top() - 50) < 1e-9 && b.left() >= 1450 - 1e-9 && b.right() <= 1550 + 1e-9) {
                    ++stubCells;
                    foot += b.width();
                    edges[QStringLiteral("%1-%2").arg(b.left()).arg(b.right())] += 1;
                }
                if (std::abs(b.bottom() - 50) < 1e-9 && b.left() >= 1450 - 1e-9 && b.right() <= 1550 + 1e-9)
                    edges[QStringLiteral("%1-%2").arg(b.left()).arg(b.right())] += 1;
            }
            QVERIFY2(stubCells >= 4, qPrintable(QString::number(stubCells)));
            QVERIFY2(std::abs(foot - 100) < 1e-6, qPrintable(QString::number(foot)));
            for (auto it = edges.constBegin(); it != edges.constEnd(); ++it)
                QVERIFY2(it.value() == 2, qPrintable(it.key() + QStringLiteral(" is met by one side only")));
        }
        // Where the two meet is no edge: the stub's cell at its foot is a
        // whole step along it, and at its open end half of one.
        double atFoot = 0, atEnd = 0;
        for (const rf::MeshCell& c : merged.cells) {
            const QRectF b = c.outline.boundingRect();
            if (b.left() < 1450 - 1e-9 || b.right() > 1550 + 1e-9 || b.top() < 50 - 1e-9) continue;
            if (std::abs(b.top() - 50) < 1e-9) atFoot = b.height();
            if (std::abs(b.bottom() - 1850) < 1e-9) atEnd = b.height();
        }
        QVERIFY2(atFoot > 0 && std::abs(atEnd - atFoot / 2) < 1e-6, qPrintable(QStringLiteral("%1 %2").arg(atFoot).arg(atEnd)));
    }

    // Two shapes of one layer that touch along an edge are joined by the
    // mesh whatever the merge did: a piece with a slanted side - its
    // conductor gridded whole - takes the other's lines where they meet
    // along x or y, so that cells of both share whole edges there, the runs
    // of current across. Unmerged, such a piece met its neighbour through no
    // edge at all, and the full-wave solve had a filter's stub cut off with
    // nothing said (B1 of the open items of 8 November). Along a slant, both
    // are cut at the same points of it. Where no edge can be shared - two
    // shapes over each other, unmerged -: said, both named.
    void touchingPiecesAreJoinedOrSaid()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.cellsAcross = 3;
        const auto shared = [](const rf::Mesh& mesh, const rf::Conductor& a, const rf::Conductor& b) {
            // Edges with a cell of each on either side.
            QHash<QString, int> sides;
            int count = 0;
            for (const rf::MeshCell& c : mesh.cells) {
                const QPointF probe = c.centre;
                const int who = a.outline.containsPoint(probe, Qt::OddEvenFill) ? 1 : b.outline.containsPoint(probe, Qt::OddEvenFill) ? 2 : 0;
                for (int i = 0, n = int(c.outline.size()); i < n; ++i) {
                    const QPointF mid = (c.outline.at(i) + c.outline.at((i + 1) % n)) / 2;
                    sides[QStringLiteral("%1,%2").arg(std::llround(mid.x() * 1e3)).arg(std::llround(mid.y() * 1e3))] |= who;
                }
            }
            for (const int v : std::as_const(sides)) count += v == 3;
            return count;
        };
        // A line, and a pad with a chamfered corner touching its lower side.
        const rf::Conductor line = on(QStringLiteral("M1"), 0, 0, 3600, 300);
        rf::Conductor pad;
        pad.layer = QStringLiteral("M1");
        pad.outline = QPolygonF({QPointF(-950, -1200), QPointF(-550, -1600), QPointF(1550, -1600), QPointF(1550, 0), QPointF(-950, 0)});
        const rf::Mesh joined = rf::meshOf({line, pad}, how, m.stack(), 3e9);
        QVERIFY2(joined.ok(), qPrintable(joined.problems.join(" | ")));
        QVERIFY2(joined.warnings.isEmpty(), qPrintable(joined.warnings.join(" | ")));
        QVERIFY2(shared(joined, line, pad) >= 3, qPrintable(QString::number(shared(joined, line, pad))));
        // A line cut along a slant into two pieces: joined along the slant,
        // each cut at the same points along it.
        rf::Conductor left, right;
        left.layer = right.layer = QStringLiteral("M1");
        // (Steps that do not line up by themselves along the slant.)
        left.outline = QPolygonF({QPointF(0, 0), QPointF(1000, 0), QPointF(1230, 400), QPointF(0, 400)});
        right.outline = QPolygonF({QPointF(1000, 0), QPointF(3170, 0), QPointF(3170, 400), QPointF(1230, 400)});
        const rf::Mesh slant = rf::meshOf({left, right}, how, m.stack(), 3e9);
        QVERIFY2(slant.ok() && slant.warnings.isEmpty(), qPrintable((slant.problems + slant.warnings).join(" | ")));
        // Every cell's edge lying on the slant met by one on the other side.
        {
            QSet<QString> onLeft, onRight;
            const QPointF p0(1000, 0), p1(1230, 400);
            const QPointF unit = (p1 - p0) / std::hypot(230.0, 400.0), normal(-unit.y(), unit.x());
            for (const rf::MeshCell& c : slant.cells) {
                // (Which side by its centroid, which a sliver along the slant
                //  has inside it, where its box's centre may not be.)
                QPointF centroid(0, 0);
                double twice = 0;
                for (int i = 0, n = int(c.outline.size()); i < n; ++i) {
                    const QPointF a = c.outline.at(i), b = c.outline.at((i + 1) % n);
                    const double cross = a.x() * b.y() - b.x() * a.y();
                    twice += cross;
                    centroid += (a + b) * cross;
                }
                centroid /= 3 * twice;
                const bool l = left.outline.containsPoint(centroid, Qt::OddEvenFill);
                for (int i = 0, n = int(c.outline.size()); i < n; ++i) {
                    const QPointF a = c.outline.at(i) - p0, b = c.outline.at((i + 1) % n) - p0;
                    if (std::abs(a.x() * normal.x() + a.y() * normal.y()) > 1e-2 || std::abs(b.x() * normal.x() + b.y() * normal.y()) > 1e-2) continue;
                    if (QLineF(c.outline.at(i), c.outline.at((i + 1) % n)).length() < 1e-6) continue;
                    const QPointF mid = (c.outline.at(i) + c.outline.at((i + 1) % n)) / 2;
                    (l ? onLeft : onRight) << QStringLiteral("%1,%2").arg(std::llround(mid.x() * 1e2)).arg(std::llround(mid.y() * 1e2));
                }
            }
            QVERIFY2(onLeft.size() >= 3 && onLeft == onRight,
                     qPrintable(QStringLiteral("%1 edges on the slant on the left, %2 on the right, %3 shared").arg(onLeft.size()).arg(onRight.size())
                                    .arg(QSet<QString>(onLeft).intersect(onRight).size())));
        }
        // Two shapes left unmerged over each other, sharing an edge's line:
        // their cells cannot meet edge to edge - said, both named.
        const rf::Conductor under = on(QStringLiteral("M1"), 0, 0, 1000, 300), over = on(QStringLiteral("M1"), 450, 70, 1100, 230);
        const rf::Mesh apart = rf::meshOf({under, over}, how, m.stack(), 3e9);
        QVERIFY2(apart.ok(), qPrintable(apart.problems.join(" | ")));
        QCOMPARE(apart.warnings.size(), 1);
        QVERIFY2(apart.warnings.first().contains(QStringLiteral("M1's piece about 500, 150")) && apart.warnings.first().contains(QStringLiteral("M1's piece about 1000, 185"))
                     && apart.warnings.first().contains(QStringLiteral("no current crosses")),
                 qPrintable(apart.warnings.first()));
        QVERIFY2(says(apart.lines, "no current crosses"), qPrintable(apart.lines.join(" | ")));
        // Apart by a gap, or meeting at a corner alone, nothing is said.
        const rf::Mesh gap = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 1000, 300), on(QStringLiteral("M1"), 1050, 0, 1000, 300)}, how, m.stack(), 3e9);
        QVERIFY2(gap.warnings.isEmpty(), qPrintable(gap.warnings.join(" | ")));
    }

    // A metal as thick as the gaps it faces: the field between its side
    // walls is in neither model's answer, so the capacitance across them
    // reads low - 19 % on a comb of 1 µm metal in 1 µm gaps against Palace -,
    // and that is said, on a sheet and on two faces alike, from a thickness
    // of a quarter of the narrowest gap; not for 3 µm in 50 µm gaps, which
    // rf25_fullwave.h validates, nor for a thin film (B2 of the open items
    // of 8 November: nothing was said).
    void aMetalThickBesideNarrowGapsIsSaid()
    {
        rf::Model m = stacked();
        rf::MeshSettings how;
        how.cellsAcross = 3;
        const auto warned = [&](double thicknessUm, double gapUm, bool faces) {
            rf::Stack stack = m.stack();
            for (rf::Layer& l : stack.layers)
                if (l.name == QLatin1String("M1")) {
                    l.thickness = thicknessUm * 1e-6;
                    l.twoSheets = faces;
                }
            const rf::Mesh mesh = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 40, 1), on(QStringLiteral("M1"), 0, 1 + gapUm, 40, 1)},
                                             how, stack, 100e9);
            return mesh.warnings.join(QStringLiteral(" | "));
        };
        for (const bool faces : {false, true}) {
            const QString one = warned(1, 1, faces);
            QVERIFY2(one.contains(QStringLiteral("M1 is 1 µm thick beside gaps as narrow as 1 µm"))
                         && (faces || one.contains(QStringLiteral("reads low")))
                         && one.contains(faces ? QStringLiteral("as capacitances between facing edge cells") : QStringLiteral("a sheet has no walls")),
                     qPrintable(one));
            QVERIFY2(!warned(0.3, 1, faces).isEmpty(), "a quarter of the gap is said");
            QVERIFY2(warned(0.2, 1, faces).isEmpty(), qPrintable(warned(0.2, 1, faces)));
            QVERIFY2(warned(3, 50, faces).isEmpty(), qPrintable(warned(3, 50, faces)));
            QVERIFY2(warned(0.01, 1, faces).isEmpty(), qPrintable(warned(0.01, 1, faces)));
        }
    }

    // One cross-section, one mesh, however it is drawn: a coplanar line
    // drawn inside pads with tapers - its signal and its grounds polygons
    // with slanted sides - has the same cells along the line as the line
    // drawn as three rectangles, edge for edge, away from the line's ends.
    // Sized by each polygon's 2
    // area / perimeter whole, the line inside the pads was meshed apart from
    // the line alone, eps_eff 0.6-0.8 % over it (a pad pair's L-2L 1.4 % and
    // 3.3 % apart from the plain line's at three and six across here); and
    // a filter drawn as one polygon with one slanted side, its 100 µm stub
    // had cells of 503 µm, "3 cell(s) across the narrowest conductor (1508
    // µm across)" (B4 of the open items of 8 November).
    void aLineIsMeshedAlikeHoweverItIsDrawn()
    {
        rf::Model m = stacked();
        for (const int across : {3, 6}) {
            rf::MeshSettings how;
            how.cellsAcross = across;
            how.gaps = true;
            const double LP = 60, LT = 80, L = 1000, x0 = LP + LT, x1 = x0 + L, e = x1 + LT + LP;
            const auto shape = [](const QList<QPointF>& points) {
                rf::Conductor c;
                c.layer = QStringLiteral("M1");
                c.outline = QPolygonF(points);
                return c;
            };
            const rf::Conductor sig = shape({{0, -40}, {LP, -40}, {x0, -25}, {x1, -25}, {x1 + LT, -40}, {e, -40}, {e, 40}, {x1 + LT, 40}, {x1, 25},
                                             {x0, 25}, {LP, 40}, {0, 40}});
            const rf::Conductor up = shape({{0, 70}, {LP, 70}, {x0, 45}, {x1, 45}, {x1 + LT, 70}, {e, 70}, {e, 205}, {0, 205}});
            const rf::Conductor down = shape({{0, -205}, {e, -205}, {e, -70}, {x1 + LT, -70}, {x1, -45}, {x0, -45}, {LP, -70}, {0, -70}});
            const rf::Mesh pads = rf::meshOf({sig, up, down}, how, m.stack(), 30e9);
            const rf::Mesh plain = rf::meshOf({on(QStringLiteral("M1"), x0, -25, L, 50), on(QStringLiteral("M1"), x0, 45, L, 160),
                                               on(QStringLiteral("M1"), x0, -205, L, 160)},
                                              how, m.stack(), 30e9);
            QVERIFY2(pads.ok() && plain.ok() && pads.warnings.isEmpty(), qPrintable((pads.problems + plain.problems + pads.warnings).join(" | ")));
            QVERIFY2(says(pads.lines, "narrowest conductor (50 ") && says(pads.lines, "slanted sides cut at their corners"),
                     qPrintable(pads.lines.join(" | ")));
            // (Along the line, its ends apart: an open end there, a joint
            //  to the taper here.)
            const auto along = [x0, x1](const rf::Mesh& mesh) {
                QStringList cells;
                for (const rf::MeshCell& c : mesh.cells) {
                    const QRectF b = c.outline.boundingRect();
                    if (b.left() >= x0 + 250 - 1e-9 && b.right() <= x1 - 250 + 1e-9)
                        cells << QStringLiteral("%1 %2 %3 %4").arg(b.left(), 0, 'f', 6).arg(b.top(), 0, 'f', 6).arg(b.width(), 0, 'f', 6).arg(b.height(), 0, 'f', 6);
                }
                cells.sort();
                return cells;
            };
            const QStringList a = along(pads), b = along(plain);
            QVERIFY2(a.size() == b.size() && a == b, qPrintable(QStringLiteral("%1 across: %2 cells along the line in pads, %3 drawn alone")
                                                                    .arg(across).arg(a.size()).arg(b.size())));
            // Where the line meets its taper, every cell's edge met by one on
            // the other side: the current crosses there.
            {
                QHash<QString, int> joint;
                for (const rf::MeshCell& c : pads.cells)
                    for (int i = 0, n = int(c.outline.size()); i < n; ++i) {
                        const QPointF p = c.outline.at(i), q = c.outline.at((i + 1) % n);
                        if (std::abs(p.x() - x0) > 1e-6 || std::abs(q.x() - x0) > 1e-6 || std::abs(p.y() - q.y()) < 1e-6) continue;
                        if (std::max(p.y(), q.y()) > 25 + 1e-6 || std::min(p.y(), q.y()) < -25 - 1e-6) continue;
                        joint[QStringLiteral("%1").arg(std::llround((p.y() + q.y()) * 500))] += 1;
                    }
                QVERIFY2(joint.size() >= 3, qPrintable(QString::number(joint.size())));
                for (auto it = joint.constBegin(); it != joint.constEnd(); ++it)
                    QVERIFY2(it.value() == 2, qPrintable(QStringLiteral("%1 across: an edge at the joint, y %2 µm, met from one side only")
                                                             .arg(across).arg(it.key().toDouble() / 1000)));
            }
            // And at its ends its cells as long as the line alone's: its own
            // pad past its taper faces it across no gap.
            const auto endCell = [x1](const rf::Mesh& mesh) {
                double shortest = 0;
                for (const rf::MeshCell& c : mesh.cells) {
                    const QRectF b = c.outline.boundingRect();
                    if (std::abs(b.right() - x1) < 1e-9 && b.top() >= -25 - 1e-9 && b.bottom() <= 25 + 1e-9)
                        shortest = shortest > 0 ? std::min(shortest, b.width()) : b.width();
                }
                return shortest;
            };
            QVERIFY2(std::abs(endCell(pads) - endCell(plain)) < 1e-6, qPrintable(QStringLiteral("%1 against %2").arg(endCell(pads)).arg(endCell(plain))));
        }
        // The filter as one polygon with a slanted side: its 100 µm stub has
        // cells across it.
        rf::MeshSettings how;
        how.cellsAcross = 3;
        rf::Conductor filter;
        filter.layer = QStringLiteral("M1");
        filter.outline = QPolygonF({QPointF(-950, -1200), QPointF(-550, -1600), QPointF(1550, -1600), QPointF(1550, 0), QPointF(1600, 0),
                                    QPointF(1600, -2100), QPointF(2150, -2100), QPointF(2150, -5200), QPointF(2250, -5200), QPointF(2250, -2100),
                                    QPointF(4200, -2100), QPointF(4200, 0), QPointF(3600, 0), QPointF(3600, 3900), QPointF(0, 3900), QPointF(0, 0),
                                    QPointF(-950, 0)});
        const rf::Mesh one = rf::meshOf({filter}, how, m.stack(), 3e9);
        QVERIFY2(one.ok() && says(one.lines, "narrowest conductor (100 "), qPrintable(one.lines.join(" | ")));
        int acrossStub = 0;
        for (const rf::MeshCell& c : one.cells) {
            const QRectF b = c.outline.boundingRect();
            if (b.left() >= 2150 - 1e-9 && b.right() <= 2250 + 1e-9 && b.top() < -4000 && b.bottom() > -4000) ++acrossStub;
        }
        QVERIFY2(acrossStub >= 3, qPrintable(QString::number(acrossStub)));
    }

    // The charge on a two-face metal's side walls across a narrow gap, which
    // neither face holds: a capacitance between facing edge cells, what a 2D
    // solve of solid metal against its two faces leaves between walls that
    // far apart - 0.349 eps per metre at thickness / gap 1, growing as the
    // square of it below and nought under 1/32 (B2 of the open items of 8
    // November: a MOM comb of 1 µm metal in 1 µm gaps 19 % under Palace's
    // capacitance without it). Each wall with the nearest one facing it.
    void theWallsAcrossANarrowGapAreItsCapacitance()
    {
        QVERIFY(std::abs(rf::wallExcess(1.0) - 0.3489) < 1e-9);
        QCOMPARE(rf::wallExcess(0.02), 0.0);
        double was = 0;
        for (const double tau : {0.04, 0.0625, 0.1, 0.25, 0.7, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0}) {
            const double h = rf::wallExcess(tau);
            QVERIFY2(h > was, qPrintable(QStringLiteral("%1 at %2").arg(h).arg(tau)));
            was = h;
        }
        rf::Model m = stacked();
        rf::Stack stack = m.stack();
        double roomEps = 1;
        for (rf::Layer& l : stack.layers)
            if (l.name == QLatin1String("M1")) {
                l.thickness = 1e-6;
                l.twoSheets = true;
            }
        for (const rf::Layer& l : stack.layers)
            if (l.name == QLatin1String("ILD2")) roomEps = l.epsr;
        rf::MeshSettings how;
        how.cellsAcross = 3;
        // Three strips 40 µm long, 1 µm wide, 1 µm apart: two gaps, each its
        // walls' 40 µm; the outer two strips do not face each other.
        const QList<rf::Conductor> strips = {on(QStringLiteral("M1"), 0, 0, 40, 1), on(QStringLiteral("M1"), 0, 2, 40, 1),
                                             on(QStringLiteral("M1"), 0, 4, 40, 1)};
        const rf::Mesh mesh = rf::meshOf(strips, how, stack, 100e9);
        const rf::Thickened thick = rf::thickened(stack, strips, mesh);
        QVERIFY(thick.any && !thick.wallOn.isEmpty());
        const QList<rf::WallGap> gaps = rf::wallGapsOf(thick, 1e-6);
        double total = 0, length = 0;
        for (const rf::WallGap& g : gaps) {
            QVERIFY2(std::abs(g.gap - 1e-6) < 1e-12, qPrintable(QString::number(g.gap)));
            total += g.capacitance;
            length += g.length;
        }
        QVERIFY2(std::abs(length - 80e-6) < 1e-12, qPrintable(QString::number(length)));
        const double expected = 8.8541878128e-12 * roomEps * rf::wallExcess(1.0) * 80e-6;
        QVERIFY2(std::abs(total - expected) < 1e-9 * expected, qPrintable(QStringLiteral("%1 against %2").arg(total).arg(expected)));
        // 3 µm in 40 µm gaps: about a hundredth of eps per metre (past 16
        // times the thickness, 48 µm here, none at all).
        for (rf::Layer& l : stack.layers)
            if (l.name == QLatin1String("M1")) l.thickness = 3e-6;
        const QList<rf::Conductor> wide = {on(QStringLiteral("M1"), 0, 0, 400, 50), on(QStringLiteral("M1"), 0, 90, 400, 50)};
        const rf::Thickened apart = rf::thickened(stack, wide, rf::meshOf(wide, how, stack, 30e9));
        double far = 0;
        for (const rf::WallGap& g : rf::wallGapsOf(apart, 1e-6)) far += g.capacitance;
        QVERIFY2(far > 0 && far < 8.8541878128e-12 * roomEps * 0.02 * 400e-6, qPrintable(QString::number(far)));
        // A sheet has no walls.
        for (rf::Layer& l : stack.layers)
            if (l.name == QLatin1String("M1")) l.twoSheets = false;
        QVERIFY(rf::wallGapsOf(rf::thickened(stack, strips, mesh), 1e-6).isEmpty());
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

    // Phase E: for a full-wave solve held compressed the mesh may have as
    // many cells as that holds, 40 000, before fewer are put across - four
    // of the strips at all of their ten across, some 24 000 cells, which the
    // charge solve's 8000 put three across - its cost said compressed, and
    // no charge solve's bound its problem; its Most cells, given, the bound
    // either way. (The forty strips at ten across are some 240 000: three
    // across even so.)
    void aMeshForACompressedSolveHoldsMoreCells()
    {
        rf::Model m = stacked();
        QList<rf::Conductor> many;
        for (int i = 0; i < 40; ++i) many << on(QStringLiteral("M1"), 0, i * 30, 2000, 10);
        const QList<rf::Conductor> four = many.mid(0, 4);
        rf::MeshSettings how;
        const rf::Mesh charges = rf::meshOf(four, how, m.stack(), 10e9);
        QVERIFY2(says(charges.lines, "across instead") && charges.cells.size() <= rf::kMostCharges,
                 qPrintable(charges.lines.join(" | ")));
        how.compressed = true;
        QCOMPARE(how.mostCells(), rf::kMostCompressedCells);
        const rf::Mesh mesh = rf::meshOf(four, how, m.stack(), 10e9);
        QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
        QVERIFY2(mesh.cells.size() > rf::kMostCharges && mesh.cells.size() <= rf::kMostCompressedCells,
                 qPrintable(QString::number(mesh.cells.size())));
        QVERIFY2(!says(mesh.lines, "across instead"), qPrintable(mesh.lines.join(" | ")));
        QVERIFY2(says(mesh.lines, "10 cell(s) across the narrowest conductor"), qPrintable(mesh.lines.join(" | ")));
        QVERIFY2(mesh.cost.said.contains(QStringLiteral("held compressed")) && !mesh.cost.tooBig, qPrintable(mesh.cost.said));
        QVERIFY2(says(mesh.lines, "a full-wave solve's") && says(mesh.lines, "held compressed"), qPrintable(mesh.lines.join(" | ")));
        QVERIFY(!says(mesh.lines, "to solve the charge on"));
        const rf::Mesh forty = rf::meshOf(many, how, m.stack(), 10e9);
        QVERIFY2(says(forty.lines, "more than the 40000 this build solves: 3 across instead") && forty.cells.size() <= rf::kMostCompressedCells,
                 qPrintable(forty.lines.join(" | ")));
        // Its own bound, given: the charge solve's, or fewer.
        how.most = 5000;
        const rf::Mesh fewer = rf::meshOf(many, how, m.stack(), 10e9);
        QVERIFY2(fewer.cells.size() <= 5000, qPrintable(QString::number(fewer.cells.size())));
        QVERIFY2(says(fewer.lines, "more than the 5000 this build solves"), qPrintable(fewer.lines.join(" | ")));
        // Read from the node, and none fewer than none.
        rf::Node& node = m.mesh();
        QStringList problems;
        set(node, "most_cells", 12000);
        QCOMPARE(rf::meshSettingsOf(node, m.parameters(), &problems).most, 12000);
        QVERIFY(problems.isEmpty());
        set(node, "most_cells", -1);
        QCOMPARE(rf::meshSettingsOf(node, m.parameters(), &problems).most, 0);
        QVERIFY2(says(problems, "the most cells cannot be fewer than none"), qPrintable(problems.join(" | ")));
        QCOMPARE(rf::integerOf(rf::Model::blank().mesh(), QStringLiteral("most_cells")), 0);
        // The quasi-static's, as it was.
        QCOMPARE(rf::MeshSettings().mostCells(), rf::kMostCharges);
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

    // A metal solved as its two faces (its "Solved as"): a sheet at its
    // bottom and one at its top, the room between them filled with what is
    // over it, and walls joining the two along its edges - which is what
    // the solvers are handed, as sheets and vias.
    void aMetalSolvedAsItsTwoFacesIsTwoSheetsAndItsWalls()
    {
        rf::Model m = stacked();
        rf::Node& ground = add(m.stackNode(), "rf_ground", QStringLiteral("gnd"), QStringLiteral("GND"));
        set(ground, "kind", QStringLiteral("perfect"));
        rf::Node& ild2 = add(m.stackNode(), "rf_dielectric", QStringLiteral("d2"), QStringLiteral("ILD2"));
        set(ild2, "thickness", QStringLiteral("2[um]"));
        set(ild2, "epsr", QStringLiteral("3.5"));
        set(ild2, "tand", QStringLiteral("0.01"));
        rf::Node& m2 = add(m.stackNode(), "rf_metal", QStringLiteral("m2"), QStringLiteral("M2"));
        set(m2, "thickness", QStringLiteral("0.9[um]"));
        rf::Node& up = add(m.stackNode(), "rf_via", QStringLiteral("v1"), QStringLiteral("V1"));
        set(up, "from", QStringLiteral("M1"));
        set(up, "to", QStringLiteral("M2"));
        rf::Node& down = add(m.stackNode(), "rf_via", QStringLiteral("v0"), QStringLiteral("V0"));
        set(down, "from", QStringLiteral("ground"));
        set(down, "to", QStringLiteral("M1"));
        // Two strips of M1 touching end to end, and M2 over them.
        const QList<rf::Conductor> shapes{on(QStringLiteral("M1"), 0, 0, 4, 2), on(QStringLiteral("M1"), 4, 0, 2, 2),
                                          on(QStringLiteral("M2"), 0, 0, 6, 2)};
        rf::MeshSettings how;
        how.edges = false;
        how.cellsAcross = 0;
        how.maximum = 1e-6;
        {   // As sheets, as the stack has always been solved: nothing made.
            const rf::Stack stack = m.stack();
            QVERIFY2(stack.ok(), qPrintable(stack.problems.join(" | ")));
            const rf::Mesh mesh = rf::meshOf(shapes, how, stack, 0);
            const rf::Thickened t = rf::thickened(stack, shapes, mesh);
            QVERIFY(!t.any);
            QCOMPARE(t.stack.layers.size(), stack.layers.size());
            QCOMPARE(t.mesh.cells.size(), mesh.cells.size());
            QCOMPARE(t.conductors.size(), shapes.size());
            QCOMPARE(rf::metalHeight(t.stack, QStringLiteral("M2")), 303e-6);
        }
        set(*m.find(QStringLiteral("m1")), "model", QStringLiteral("faces"));
        const rf::Stack stack = m.stack();
        QVERIFY2(stack.ok(), qPrintable(stack.problems.join(" | ")));
        QVERIFY(stack.named(QStringLiteral("M1"))->twoSheets);
        QVERIFY(!stack.named(QStringLiteral("M2"))->twoSheets);
        const rf::Mesh mesh = rf::meshOf(shapes, how, stack, 0);
        QCOMPARE(mesh.cells.size(), 12 + 12);
        const rf::Thickened t = rf::thickened(stack, shapes, mesh);
        QVERIFY(t.any);
        QCOMPARE(t.cells, int(mesh.cells.size()));

        // The stack: M1 its bottom face, then the room its thickness takes -
        // of ILD2, which is over it and fills its gaps - its top face at
        // the top of that room, and the walls between the two.
        const rf::Layer* bottom = t.stack.named(QStringLiteral("M1"));
        QVERIFY(bottom != nullptr);
        QCOMPARE(bottom->face, rf::Layer::Bottom);
        const rf::Layer* room = t.stack.named(QStringLiteral("M1 (beside)"));
        QVERIFY(room != nullptr);
        QCOMPARE(room->kind, rf::Layer::Dielectric);
        QCOMPARE(room->thickness, 0.6e-6);
        QCOMPARE(room->epsr, 3.5);
        QCOMPARE(room->tand, 0.01);
        const rf::Layer* top = t.stack.named(QStringLiteral("M1 (top)"));
        QVERIFY(top != nullptr);
        QCOMPARE(top->kind, rf::Layer::Metal);
        QCOMPARE(top->face, rf::Layer::Top);
        QCOMPARE(top->faceOf, QStringLiteral("M1"));
        QCOMPARE(top->thickness, 0.6e-6);
        QCOMPARE(top->sigma, bottom->sigma);
        QVERIFY(top->tag.isEmpty());   // (the tag is the metal's own)
        const rf::Layer* walls = t.stack.named(QStringLiteral("M1 (walls)"));
        QVERIFY(walls != nullptr);
        QCOMPARE(walls->kind, rf::Layer::Via);
        QCOMPARE(walls->face, rf::Layer::Walls);
        QCOMPARE(walls->from, QStringLiteral("M1"));
        QCOMPARE(walls->to, QStringLiteral("M1 (top)"));
        QVERIFY(t.made(QStringLiteral("M1 (top)")));
        QVERIFY(t.made(QStringLiteral("M1 (walls)")));
        QVERIFY(!t.made(QStringLiteral("M1")));
        QVERIFY(!t.made(QStringLiteral("ILD2")));
        // Its top 0.6 µm over its bottom, and what is above it as high as
        // the stack draws it - 0.6 µm higher than as sheets.
        QCOMPARE(rf::metalHeight(t.stack, QStringLiteral("M1")), 301e-6);
        QCOMPARE(rf::metalHeight(t.stack, QStringLiteral("M1 (top)")), 301.6e-6);
        QCOMPARE(rf::metalHeight(t.stack, QStringLiteral("M2")), 303.6e-6);
        const rf::Layers slabs = rf::layersOf(t.stack);
        QVERIFY2(slabs.ok(), qPrintable(slabs.problems.join(" | ")));
        QCOMPARE(slabs.slabs.size(), 4);
        QCOMPARE(slabs.slabs.at(2).epsr, 3.5);
        QVERIFY(std::abs(slabs.slabs.at(2).z0 - 301e-6) < 1e-15 && std::abs(slabs.slabs.at(2).z1 - 301.6e-6) < 1e-15);
        // A via up from it leaves its top face; one up to it from the
        // ground arrives at its bottom.
        QCOMPARE(t.stack.named(QStringLiteral("V1"))->from, QStringLiteral("M1 (top)"));
        QCOMPARE(t.stack.named(QStringLiteral("V1"))->to, QStringLiteral("M2"));
        QCOMPARE(t.stack.named(QStringLiteral("V0"))->to, QStringLiteral("M1"));
        QVERIFY(t.said.join(' ').contains(QLatin1String("ILD2")));

        // The shapes: each of M1's on its top face too, and its walls over
        // both; M2's alone.
        QCOMPARE(t.conductors.size(), shapes.size() + 4);
        int onTop = 0, onWalls = 0;
        for (const rf::Conductor& c : t.conductors) {
            onTop += c.layer == QLatin1String("M1 (top)") ? 1 : 0;
            onWalls += c.layer == QLatin1String("M1 (walls)") ? 1 : 0;
        }
        QCOMPARE(onTop, 2);
        QCOMPARE(onWalls, 2);

        // The cells: the mesh's own in their places, each of M1's twelve
        // on its top face after them, and a wall along every side of an M1
        // cell that is the conductor's edge - not along the side where the
        // two strips touch, which is no edge: 6 + 6 along the long sides
        // and 2 at either end, 16.
        for (int c = 0; c < mesh.cells.size(); ++c) {
            QCOMPARE(t.mesh.cells.at(c).layer, mesh.cells.at(c).layer);
            QCOMPARE(t.mesh.cells.at(c).centre, mesh.cells.at(c).centre);
        }
        QCOMPARE(t.topOf.size(), 12);
        QCOMPARE(t.walls, 16);
        QCOMPARE(t.mesh.cells.size(), mesh.cells.size() + 12 + 16);
        for (auto it = t.topOf.cbegin(); it != t.topOf.cend(); ++it) {
            QCOMPARE(mesh.cells.at(it.key()).layer, QStringLiteral("M1"));
            const rf::MeshCell& copy = t.mesh.cells.at(it.value());
            QCOMPARE(copy.layer, QStringLiteral("M1 (top)"));
            QCOMPARE(copy.outline, mesh.cells.at(it.key()).outline);
            QCOMPARE(t.bottomOf.value(it.value()), it.key());
        }
        int where = 0;
        for (int c = int(mesh.cells.size()) + 12; c < t.mesh.cells.size(); ++c) {
            const rf::MeshCell& wall = t.mesh.cells.at(c);
            QCOMPARE(wall.layer, QStringLiteral("M1 (walls)"));
            QVERIFY(wall.area > 0);
            // It stands inside a cell of M1 along its edge, and never where
            // the two strips meet.
            bool inside = false;
            for (int k = 0; k < mesh.cells.size(); ++k)
                inside = inside || (mesh.cells.at(k).layer == QLatin1String("M1")
                                    && mesh.cells.at(k).outline.containsPoint(wall.centre, Qt::OddEvenFill));
            QVERIFY2(inside, qPrintable(QStringLiteral("a wall at %1, %2").arg(wall.centre.x()).arg(wall.centre.y())));
            QVERIFY2(std::abs(wall.centre.x() - 4) > 0.2, qPrintable(QStringLiteral("a wall at x %1").arg(wall.centre.x())));
            where += wall.centre.y() < 0.2 || wall.centre.y() > 1.8 ? 1 : 0;
        }
        QCOMPARE(where, 12);

        // What the panel counts: M1's cells and edges twice, and a wall up
        // from each of its cells along an edge.
        const rf::Mesh sheets = rf::meshOf(shapes, how, m.stack(), 0);
        rf::Model asSheets = m;
        set(*asSheets.find(QStringLiteral("m1")), "model", QStringLiteral("sheet"));
        const rf::Mesh plain = rf::meshOf(shapes, how, asSheets.stack(), 0);
        QCOMPARE(sheets.cost.unknowns - plain.cost.unknowns, 16 + 12);   // M1's 16 edges again, and its 12 cells all on an edge
        QVERIFY2(says(sheets.lines, "M1 solved as its two faces"), qPrintable(sheets.lines.join(" | ")));
        QVERIFY(!says(plain.lines, "two faces"));

        {   // A top metal's room is the air over it; and a metal of no
            // thickness to be two faces is a sheet still.
            rf::Model only = stacked();
            set(*only.find(QStringLiteral("m1")), "model", QStringLiteral("faces"));
            const rf::Stack s = only.stack();
            const rf::Mesh one = rf::meshOf({on(QStringLiteral("M1"), 0, 0, 2, 1)}, how, s, 0);
            const rf::Thickened aired = rf::thickened(s, {on(QStringLiteral("M1"), 0, 0, 2, 1)}, one);
            QVERIFY(aired.any);
            QCOMPARE(aired.stack.named(QStringLiteral("M1 (beside)"))->epsr, 1.0);
            QVERIFY(aired.said.join(' ').contains(QLatin1String("the air")));
            QCOMPARE(aired.walls, 6);
            set(*only.find(QStringLiteral("m1")), "thickness", QStringLiteral("0"));
            const rf::Stack flat = only.stack();
            QVERIFY(!rf::thickened(flat, {on(QStringLiteral("M1"), 0, 0, 2, 1)}, rf::meshOf({on(QStringLiteral("M1"), 0, 0, 2, 1)}, how, flat, 0)).any);
        }
        // And what is made is made once: a stack made already is as it is.
        const rf::Thickened again = rf::thickened(t.stack, t.conductors, t.mesh);
        QVERIFY(!again.any);
        // A mesh whose cells twice over are more charges than the
        // extraction solves is still a mesh: the solver says what it cannot
        // hold, and a calibration strip too long for the full wave is cut
        // short rather than refused.
        rf::MeshSettings fine = how;
        fine.maximum = 0.5e-6;
        const QList<rf::Conductor> wide{on(QStringLiteral("M1"), 0, 0, 50, 25)};
        const rf::Mesh many = rf::meshOf(wide, fine, stack, 0);
        QCOMPARE(many.cells.size(), 5000);   // 100 by 50 cells of 0.5 µm
        QVERIFY2(many.ok(), qPrintable(many.problems.join(" | ")));
        QVERIFY2(says(many.lines, "10000 to solve the charge on"), qPrintable(many.lines.join(" | ")));
    }
};

QTEST_MAIN(TestRf25Mesh)
#include "test_rf25_mesh.moc"
