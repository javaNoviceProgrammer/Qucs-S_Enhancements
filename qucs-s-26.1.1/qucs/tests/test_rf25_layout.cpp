/*
 * The shapes of an RF 2.5D model (rf25/rf25_layout.h): the layer map that
 * sends a file's layers to the stack's, and the processing steps that run
 * before meshing - merge, merge vias, drop slivers, snap, heal, remove
 * fill - each of which must say what it changed and must be possible to
 * turn off.
 */
#include <QtTest>
#include <QTemporaryDir>

#include <cmath>
#include <numeric>
#include <functional>
#include <random>

#include "layout.h"

#include <gdstk/gdstk.hpp>
#include "rf25/rf25_layout.h"
#include "rf25/rf25_mesh.h"
#include "rf25/rf25_inductance.h"
#include "rf25/rf25_solve.h"
#include "rf25/rf25_hfss.h"
#include "rf25/rf25_fullwave.h"

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

/// A rectangle, in micrometres.
QPolygonF box(double x, double y, double w, double h)
{
    return QPolygonF(QRectF(x, y, w, h));
}

/// A stack of a substrate, an oxide, M1, an oxide, a via and M2.
rf::Model stacked()
{
    rf::Model m = rf::Model::blank();
    m.stackNode().children.clear();
    rf::Node& sub = add(m.stackNode(), "rf_substrate", QStringLiteral("sub1"), QStringLiteral("Si"));
    set(sub, "thickness", QStringLiteral("300[um]"));
    rf::Node& ild = add(m.stackNode(), "rf_dielectric", QStringLiteral("d1"), QStringLiteral("ILD1"));
    set(ild, "thickness", QStringLiteral("1[um]"));
    rf::Node& m1 = add(m.stackNode(), "rf_metal", QStringLiteral("m1"), QStringLiteral("M1"));
    set(m1, "thickness", QStringLiteral("0.6[um]"));
    rf::Node& ild2 = add(m.stackNode(), "rf_dielectric", QStringLiteral("d2"), QStringLiteral("ILD2"));
    set(ild2, "thickness", QStringLiteral("2[um]"));
    rf::Node& via = add(m.stackNode(), "rf_via", QStringLiteral("v1"), QStringLiteral("V1"));
    set(via, "from", QStringLiteral("M1"));
    set(via, "to", QStringLiteral("M2"));
    rf::Node& m2 = add(m.stackNode(), "rf_metal", QStringLiteral("m2"), QStringLiteral("M2"));
    set(m2, "thickness", QStringLiteral("0.9[um]"));
    return m;
}

/// stacked() with \a shapes of its own and \a rows as its layer map.
rf::Model shaped(const QList<QJsonObject>& shapes, const QJsonArray& rows = QJsonArray() << QJsonArray{"1", "0", "M1"})
{
    rf::Model m = stacked();
    m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes")));
    const QString tag = m.layout().child(QStringLiteral("rf_shapes"))->tag;
    for (const QJsonObject& o : shapes) {
        rf::Node n = m.make(QStringLiteral("rf_shape"));
        n.props = o;
        m.add(tag, n);
    }
    m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), rows);
    return m;
}

/// A layout read with nothing in it: the model's own shapes alone.
qucs_s::layout::Layout nothingRead()
{
    qucs_s::layout::Layout empty;
    qucs_s::layout::Cell top;
    top.name = QStringLiteral("TOP");
    empty.cells << top;
    empty.byName.insert(top.name, 0);
    return empty;
}

/// A rectangle of the model's own on \a layer, µm.
QJsonObject boxOn(const char* layer, double x0, double y0, double x1, double y1)
{
    QJsonArray rows;
    for (const QPointF& p : {QPointF(x0, y0), QPointF(x1, y0), QPointF(x1, y1), QPointF(x0, y1)})
        rows.append(QJsonArray{QString::number(p.x(), 'g', 15) + "[um]", QString::number(p.y(), 'g', 15) + "[um]"});
    return QJsonObject{{"layer", layer}, {"shape", "polygon"}, {"points", rows}};
}

/// How many sides of \a mesh are held by three cells or twice by one, as the
/// solve pairs them (by their middles, to a nanometre), and how many pieces
/// its cells are in, joined side to side.
QPair<int, int> clashesAndPieces(const rf::Mesh& mesh)
{
    QHash<QString, QList<int>> sides;
    for (int c = 0; c < mesh.cells.size(); ++c) {
        const QPolygonF& o = mesh.cells.at(c).outline;
        for (int i = 0; i < o.size(); ++i) {
            const QPointF mid = (o.at(i) + o.at((i + 1) % o.size())) / 2;
            sides[mesh.cells.at(c).layer + QStringLiteral("|%1,%2").arg(std::llround(mid.x() * 1e3)).arg(std::llround(mid.y() * 1e3))] << c;
        }
    }
    QVector<int> parent(mesh.cells.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    int clashes = 0;
    for (auto it = sides.cbegin(); it != sides.cend(); ++it) {
        if (it.value().size() > 2 || (it.value().size() == 2 && it.value().at(0) == it.value().at(1))) ++clashes;
        else if (it.value().size() == 2) parent[find(it.value().at(0))] = find(it.value().at(1));
    }
    QSet<int> pieces;
    for (int c = 0; c < mesh.cells.size(); ++c) pieces.insert(find(c));
    return {clashes, int(pieces.size())};
}

/// rf::shareLines() as it was before its lists were halved (47fae397): each
/// line looked for by walking the lists whole - what the halving must give,
/// to the last line.
bool walkedShareLines(QList<double>& mine, QList<double>& theirs, double lo, double hi, double tol, bool ends, double merge)
{
    // (\a ends false: the lines strictly inside alone, not the stretch's
    //  ends - where a slanted side's part meets another, an end at a slanted
    //  corner is no line of either.)
    const double in = ends ? -tol : tol;
    QList<double> both;
    for (const double x : std::as_const(mine))
        if (x >= lo + in && x <= hi - in) both << x;
    for (const double x : std::as_const(theirs))
        if (x >= lo + in && x <= hi - in) both << x;
    if (ends) both << lo << hi;
    std::sort(both.begin(), both.end());
    bool gained = false;
    if (merge > tol) {
        // The lines that stay: the ends, then from the lowest up each one
        // further than \a merge from the last kept and from the next end.
        const auto isEnd = [&](double x) { return ends && (std::abs(x - lo) <= tol || std::abs(x - hi) <= tol); };
        QList<double> kept;
        for (const double x : std::as_const(both)) {
            if (!kept.isEmpty() && x - kept.last() <= tol) continue;
            if (isEnd(x)) {
                // (An end displaces the line kept just under it.)
                while (!kept.isEmpty() && !isEnd(kept.last()) && x - kept.last() <= merge) kept.removeLast();
                kept << x;
                continue;
            }
            if (!kept.isEmpty() && x - kept.last() <= merge) continue;
            if (ends && hi - x <= merge && hi - x > tol) continue;
            kept << x;
        }
        // Each side's lines inside the stretch that are not kept, out.
        for (QList<double>* lines : {&mine, &theirs}) {
            const qsizetype was = lines->size();
            lines->erase(std::remove_if(lines->begin(), lines->end(),
                                        [&](double x) {
                                            if (x < lo - tol || x > hi + tol) return false;
                                            if (std::abs(x - lo) <= tol || std::abs(x - hi) <= tol) return false;
                                            for (const double k : std::as_const(kept))
                                                if (std::abs(x - k) <= tol) return false;
                                            return true;
                                        }),
                         lines->end());
            gained = gained || lines->size() != was;
        }
        both = kept;
    }
    for (QList<double>* lines : {&mine, &theirs})
        for (const double x : std::as_const(both)) {
            bool known = false;
            for (const double y : std::as_const(*lines)) known = known || std::abs(x - y) <= tol;
            if (known) continue;
            lines->insert(std::lower_bound(lines->begin(), lines->end(), x), x);
            gained = true;
        }
    return gained;
}


/// A region added to \a m's Cutout: its shape and its properties.
rf::Node& region(rf::Model& m, const char* shape, const QList<QPair<const char*, QJsonValue>>& props, const QString& label = {})
{
    rf::Node& cutout = *m.layout().child(QStringLiteral("rf_cutout"));
    const QString name = label.isEmpty() ? QStringLiteral("Region %1").arg(cutout.children.size() + 1) : label;
    rf::Node& r = add(cutout, "rf_region", m.newTag(QStringLiteral("cut")), name);
    set(r, "shape", QLatin1String(shape));
    for (const auto& [key, value] : props) set(r, key, value);
    return r;
}

double areaOfAll(const QList<QPolygonF>& shapes)
{
    double a = 0;
    for (const QPolygonF& s : shapes) a += rf::areaOf(s);
    return a;
}

bool says(const QStringList& lines, const char* words)
{
    for (const QString& l : lines)
        if (l.contains(QLatin1String(words), Qt::CaseInsensitive)) return true;
    return false;
}

} // namespace

class TestRf25Layout : public QObject
{
    Q_OBJECT

private slots:
    // The map: a row sends a layer and datatype of the file to a layer of
    // the stack, a fourth cell marks it as dummy fill, and what it cannot
    // do it says.
    void theLayerMapSendsShapesWhereItSays()
    {
        rf::Model m = stacked();
        QJsonArray rows;
        rows.append(QJsonArray{QStringLiteral("31"), QStringLiteral("0"), QStringLiteral("M1")});
        rows.append(QJsonArray{QStringLiteral("32"), QStringLiteral("0"), QStringLiteral("V1")});
        rows.append(QJsonArray{QStringLiteral("33"), QStringLiteral("0"), QStringLiteral("M2")});
        rows.append(QJsonArray{QStringLiteral("40"), QStringLiteral("0"), QStringLiteral("M1"), QStringLiteral("fill")});
        rows.append(QJsonArray{QStringLiteral("99"), QStringLiteral("0"), QString()});        // (left out)
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", rows);

        const rf::LayerMap map = rf::layerMapOf(m);
        QVERIFY2(map.problems.isEmpty(), qPrintable(map.problems.join(" | ")));
        QCOMPARE(map.toLayer.value(QStringLiteral("31/0")), QStringLiteral("M1"));
        QCOMPARE(map.toLayer.value(QStringLiteral("32/0")), QStringLiteral("V1"));
        QVERIFY(!map.toLayer.contains(QStringLiteral("99/0")));
        QVERIFY(map.fill.contains(QStringLiteral("M1")));
        QVERIFY(!map.fill.contains(QStringLiteral("M2")));

        // A datatype left empty is 0; a layer sent twice is noted, the later
        // row taking it - it stands in no solve's way, where it was listed
        // as standing in the way of one that ran (the review of the MIMs) -;
        // one sent where the stack has nothing is a problem.
        rows.append(QJsonArray{QStringLiteral("31"), QString(), QStringLiteral("M2")});
        rows.append(QJsonArray{QStringLiteral("50"), QStringLiteral("0"), QStringLiteral("M9")});
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", rows);
        const rf::LayerMap again = rf::layerMapOf(m);
        QVERIFY2(!says(again.problems, "two layers of the stack"), qPrintable(again.problems.join(" | ")));
        QVERIFY2(says(again.notes, "two layers of the stack") && says(again.notes, "its shapes go to M2"),
                 qPrintable(again.notes.join(" | ")));
        QCOMPARE(again.toLayer.value(QStringLiteral("31/0")), QStringLiteral("M2"));
        QVERIFY2(says(again.problems, "no layer of the stack"), qPrintable(again.problems.join(" | ")));

        // Its layer and datatype written as numbers - [[1, 0, "M1"]], as a
        // file written by hand or a tool may have them - are their text: read
        // as nothing, the row was lost, its shapes nowhere.
        // Read again from a file, the model's rows are text.
        QJsonArray numbers;
        numbers.append(QJsonArray{1, 0, QStringLiteral("M1")});
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", numbers);
        QCOMPARE(rf::layerMapOf(m).toLayer.value(QStringLiteral("1/0")), QStringLiteral("M1"));
        QString why;
        const std::optional<rf::Model> read = rf::Model::fromJson(m.toJson(), &why);
        QVERIFY2(read.has_value(), qPrintable(why));
        const QJsonArray row = read->layout().child(QStringLiteral("rf_layer_map"))->props.value(QLatin1String("rows")).toArray().at(0).toArray();
        QCOMPARE(row.at(0).toString(), QStringLiteral("1"));
        QCOMPARE(row.at(1).toString(), QStringLiteral("0"));
        QCOMPARE(rf::tableCell(QJsonValue(2.5)), QStringLiteral("2.5"));
    }

    // The processing node read: its flags, its lengths in metres through
    // the parameters, and what is not a number.
    void theProcessingIsReadFromItsNode()
    {
        rf::Model m = stacked();
        rf::Node& how = *m.layout().child(QStringLiteral("rf_processing"));
        QStringList problems;
        const rf::Processing fresh = rf::processingOf(how, m.parameters(), &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
        QVERIFY(fresh.merge && fresh.mergeVias && fresh.keepArea && fresh.dropSlivers && fresh.heal);
        QVERIFY(!fresh.snap && !fresh.removeFill);
        QCOMPARE(fresh.viaDistance, 1e-6);
        QCOMPARE(fresh.sliverWidth, 1e-7);

        set(how, "merge_vias", false);
        set(how, "snap", true);
        set(how, "via_distance", QStringLiteral("2[um]"));
        QJsonArray parameters;
        parameters.append(QJsonArray{QStringLiteral("fine"), QStringLiteral("20[nm]")});
        set(*m.root.child(QStringLiteral("rf_parameters")), "rows", parameters);
        set(how, "grid", QStringLiteral("fine"));
        const rf::Processing set_ = rf::processingOf(how, m.parameters(), &problems);
        QVERIFY(!set_.mergeVias);
        QVERIFY(set_.snap);
        QCOMPARE(set_.viaDistance, 2e-6);
        QCOMPARE(set_.grid, 20e-9);

        // What is no number is said, and the default is kept.
        set(how, "sliver_width", QStringLiteral("0.1[um"));
        problems.clear();
        const rf::Processing bad = rf::processingOf(how, m.parameters(), &problems);
        QVERIFY2(says(problems, "is not a number"), qPrintable(problems.join(" | ")));
        QCOMPARE(bad.sliverWidth, 1e-7);
    }

    // Merging: the metal of a layout is drawn as many rectangles, and the
    // solver wants one conductor - but only where they touch.
    // A jog - a step of a few grid units in a side along x or y - is healed
    // away, its sides kept along x and y, and said (a
    // review's octagon with 5 nm jogs meshed to 153,590 cells, refused at
    // 3.2 TB, where snapped it was 7,418). One a grid unit long and one of
    // three both go; a step of five grid units stays.
    void aJogOfAFewGridUnitsIsHealed()
    {
        // A rectangle 100 by 50 µm whose top steps down 5 nm half way along,
        // and whose right side steps out 15 nm.
        const QPolygonF jogged({QPointF(0, 0), QPointF(100, 0), QPointF(100, 20), QPointF(100.015, 20), QPointF(100.015, 50),
                                QPointF(50, 50), QPointF(50, 49.995), QPointF(0, 49.995)});
        int removed = 0;
        const QPolygonF healed = rf::withoutJogs(jogged, 0.02, &removed);
        QCOMPARE(removed, 2);
        QCOMPARE(healed.size(), 4);
        for (int i = 0; i < healed.size(); ++i) {
            const QPointF d = healed.at((i + 1) % healed.size()) - healed.at(i);
            QVERIFY2(d.x() == 0 || d.y() == 0, qPrintable(QStringLiteral("a slanted side %1, %2").arg(d.x()).arg(d.y())));
        }
        QVERIFY2(std::abs(rf::areaOf(healed) - 5000) < 1, qPrintable(QString::number(rf::areaOf(healed))));
        // A slot and a tab 3 grid units wide, 40 deep, are no jogs: their sides
        // run the opposite ways (the review of 791a8ed2: the slot was filled,
        // the tab cut off).
        {
            const QPolygonF slot({QPointF(0, 0), QPointF(100, 0), QPointF(100, 50), QPointF(51.5, 50), QPointF(51.5, 10), QPointF(48.5, 10),
                                  QPointF(48.5, 50), QPointF(0, 50)});
            const QPolygonF tab({QPointF(0, 0), QPointF(100, 0), QPointF(100, 50), QPointF(51.5, 50), QPointF(51.5, 90), QPointF(48.5, 90),
                                 QPointF(48.5, 50), QPointF(0, 50)});
            for (const QPolygonF& shape : {slot, tab}) {
                int none = 0;
                const QPolygonF kept = rf::withoutJogs(shape, 4, &none);
                QCOMPARE(none, 0);
                QVERIFY2(std::abs(rf::areaOf(kept) - rf::areaOf(shape)) < 1e-9, qPrintable(QString::number(rf::areaOf(kept))));
            }
        }
        // A step of 25 nm is no jog at 20.
        const QPolygonF stepped({QPointF(0, 0), QPointF(100, 0), QPointF(100, 50), QPointF(50, 50), QPointF(50, 50.025), QPointF(0, 50.025)});
        removed = 0;
        QCOMPARE(rf::withoutJogs(stepped, 0.02, &removed).size(), 6);
        QCOMPARE(removed, 0);
        // Through the processing, on its 5 nm grid: healed, and said.
        const rf::Stack stack = stacked().stack();
        rf::Processing how;
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("M1")] = {jogged};
        const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
        QVERIFY2(done.problems.isEmpty(), qPrintable(done.problems.join(" | ")));
        QCOMPARE(done.conductors.size(), 1);
        QCOMPARE(done.conductors.first().outline.size() - (done.conductors.first().outline.isClosed() ? 1 : 0), 4);
        QVERIFY2(done.report.summary().contains(QStringLiteral("2 jog(s) under 0.02 µm")), qPrintable(done.report.summary()));
    }

    // A conductor's breadth across a run (breadthAcross(), which a metal
    // solved as its faces takes its inside by):
    // the line's stretches on the shapes,
    // joined where shapes touch, each judged by its middle - so that a corner
    // the line runs through, which crossings paired in turn took twice, cuts
    // nothing, and a gap does.
    void aConductorsBreadthAcrossARunIsItsShapesJoined()
    {
        const QPolygonF bar(QRectF(0, 0, 10, 2)), above(QRectF(0, 2, 10, 3));
        QCOMPARE(rf::breadthAcross({bar}, QPointF(5, 1), QPointF(0, 1), 100), 2.0);
        // (Two shapes touching along a side: one conductor, 5 across.)
        QCOMPARE(rf::breadthAcross({bar, above}, QPointF(5, 1), QPointF(0, 1), 100), 5.0);
        QCOMPARE(rf::breadthAcross({above, bar}, QPointF(5, 4), QPointF(0, -1), 100), 5.0);
        // (Along x: the bar alone 10; a triangle its corner on the bar's side
        //  at (10, 1) joins it, 12; one with its corner 2 past, apart, 10.)
        QCOMPARE(rf::breadthAcross({bar}, QPointF(5, 1), QPointF(1, 0), 100), 10.0);
        const QPolygonF touching({QPointF(10, 1), QPointF(12, 0), QPointF(12, 2)});
        const QPolygonF apart({QPointF(12, 1), QPointF(14, 0), QPointF(14, 2)});
        QCOMPARE(rf::breadthAcross({bar, touching}, QPointF(5, 1), QPointF(1, 0), 100), 12.0);
        QCOMPARE(rf::breadthAcross({bar, apart}, QPointF(5, 1), QPointF(1, 0), 100), 10.0);
        // (A corner of another shape the line only grazes, beside the bar:
        //  nothing added, the stretch past it judged by its middle.)
        const QPolygonF grazed({QPointF(10, 1), QPointF(12, 3), QPointF(8, 3)});
        QCOMPARE(rf::breadthAcross({bar, grazed}, QPointF(5, 1), QPointF(1, 0), 100), 10.0);
        // (The middle exactly on a side: the stretch beside it on the bar,
        //  not the one off it.)
        QCOMPARE(rf::breadthAcross({bar}, QPointF(0, 1), QPointF(1, 0), 100), 10.0);
        // (No further than the reach either way; nought off every shape.)
        QCOMPARE(rf::breadthAcross({bar}, QPointF(5, 1), QPointF(1, 0), 3), 6.0);
        QCOMPARE(rf::breadthAcross({bar}, QPointF(5, 9), QPointF(1, 0), 100), 0.0);
    }

    void touchingShapesAreMergedIntoOne()
    {
        const rf::Stack stack = stacked().stack();
        rf::Processing how;
        how.dropSlivers = false;
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("M1")] = {box(0, 0, 10, 2), box(10, 0, 10, 2)};   // touching
        shapes[QStringLiteral("M2")] = {box(0, 50, 5, 5), box(20, 50, 5, 5)};   // apart
        const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
        QVERIFY2(done.problems.isEmpty(), qPrintable(done.problems.join(" | ")));
        QCOMPARE(done.byLayer().value(QStringLiteral("M1")), 1);
        QCOMPARE(done.byLayer().value(QStringLiteral("M2")), 2);
        QVERIFY2(says(done.report.lines, "merged 2 shapes into 1"), qPrintable(done.report.summary()));
        // The one conductor has the area of the two rectangles.
        for (const rf::Conductor& c : done.conductors)
            if (c.layer == QLatin1String("M1")) QVERIFY(std::abs(c.area() - 40.0) < 0.01);

        // Turned off, nothing is joined and nothing is said.
        how.merge = false;
        const rf::Processed off = rf::processShapes(shapes, {}, how, stack);
        QCOMPARE(off.byLayer().value(QStringLiteral("M1")), 2);
        QVERIFY(off.report.lines.isEmpty());

        // But what overlaps is joined all the same, and said: left apart,
        // each was meshed whole and the overlap twice.
        // What only touches stays apart beside it.
        QMap<QString, QList<QPolygonF>> overlapping;
        overlapping[QStringLiteral("M1")] = {box(0, 0, 10, 2), box(6, 0, 10, 2), box(0, 0, 10, 2), box(16, 0, 4, 2)};
        overlapping[QStringLiteral("M2")] = {box(0, 50, 5, 5), box(20, 50, 5, 5)};
        const rf::Processed joined = rf::processShapes(overlapping, {}, how, stack);
        QVERIFY2(joined.problems.isEmpty(), qPrintable(joined.problems.join(" | ")));
        QCOMPARE(joined.byLayer().value(QStringLiteral("M1")), 2);
        QCOMPARE(joined.byLayer().value(QStringLiteral("M2")), 2);
        QVERIFY2(says(joined.report.lines, "M1: 3 overlapping shapes joined into 1, though shapes are not merged"),
                 qPrintable(joined.report.summary()));
        double area = 0;
        for (const rf::Conductor& c : joined.conductors)
            if (c.layer == QLatin1String("M1")) area += c.area();
        QVERIFY2(std::abs(area - 40.0) < 0.01, qPrintable(QString::number(area)));
        // The groups themselves: a corner or an edge in common is no overlap.
        QCOMPARE(rf::overlappingGroups({box(0, 0, 10, 2), box(10, 0, 10, 2), box(20, 2, 5, 5)}).size(), 0);
        QCOMPARE(rf::overlappingGroups({box(0, 0, 10, 2), box(9, 1, 10, 2)}, {QStringLiteral("M1"), QStringLiteral("M2")}).size(), 0);
        // (Their boxes overlapping, the shapes not: two triangles across a
        //  diagonal.)
        QCOMPARE(rf::overlappingGroups({QPolygonF({QPointF(0, 0), QPointF(10, 0), QPointF(0, 10)}),
                                        QPolygonF({QPointF(10, 10), QPointF(10, 1), QPointF(1, 10)})}).size(), 0);
        const QList<QList<int>> groups = rf::overlappingGroups({box(0, 0, 10, 2), box(30, 0, 5, 5), box(9, 1, 10, 2), box(18, 1.5, 1, 1)});
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first(), (QList<int>{0, 2, 3}));
    }

    // Shapes that touch along part of an edge are one shape, however the
    // union's own sweep meets them. gdstk's union left two pieces apart in
    // about one set in sixty of touching rectangles - a
    // low-pass filter among them, its centre stub then cut from its line in
    // the full-wave solve with nothing said
    // -: here one such set, whose fourth rectangle gdstk alone
    // left apart, and three thousand random ones, each as many shapes out
    // as it has pieces joined along edges, their area the rectangles'. Two
    // that meet at a corner alone stay two.
    void shapesTouchingAlongAnEdgeAreOneShape()
    {
        const QList<QPolygonF> four = {box(0, 0, 3600, 3900), box(1600, -2100, 2600, 2100), box(-950, -1600, 2500, 1600),
                                       box(2150, -5200, 100, 3100)};
        int joined = 0;
        const QList<QPolygonF> one = rf::unionOf(four, &joined);
        QCOMPARE(one.size(), 1);
        QVERIFY2(joined >= 1, "the set gdstk leaves in two needed no joining: the test no longer tests the join");
        double area = 0;
        for (const QPolygonF& s : four) area += rf::areaOf(s);
        QVERIFY2(std::abs(rf::areaOf(one.first()) - area) < 1e-6 * area,
                 qPrintable(QStringLiteral("%1 against %2").arg(rf::areaOf(one.first())).arg(area)));
        // Through the processing, which says what it joined.
        const rf::Stack stack = stacked().stack();
        rf::Processing how;
        how.dropSlivers = false;
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("M1")] = four;
        const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
        QCOMPARE(done.byLayer().value(QStringLiteral("M1")), 1);
        QVERIFY2(says(done.report.lines, "merged 4 shapes into 1") && says(done.report.lines, "touching along an edge joined"),
                 qPrintable(done.report.summary()));
        // Corners alone: two.
        QCOMPARE(rf::unionOf({box(0, 0, 10, 10), box(10, 10, 10, 10)}).size(), 2);
        QVERIFY(!rf::touchAlongAnEdge(box(0, 0, 10, 10), box(10, 10, 10, 10), 1e-6));
        QVERIFY(rf::touchAlongAnEdge(box(0, 0, 10, 10), box(10, 4, 10, 10), 1e-6));

        // Random sets: rectangles of 100 µm steps, each on a side of one
        // already there.
        std::mt19937 random(20261108);
        const auto step = [&random](int most) { return double(1 + random() % most) * 100; };
        int sets = 0;
        for (int t = 0; t < 3000; ++t) {
            QList<QRectF> rs = {QRectF(0, 0, step(40), step(40))};
            const int n = 2 + int(random() % 7);
            while (rs.size() < n) {
                const QRectF b = rs.at(int(random() % rs.size()));
                const int side = int(random() % 4);
                const double w = step(40), h = step(40);
                if (side < 2) {
                    const double x = b.left() + double(random() % 41) * 100 - w / 2;
                    rs << QRectF(x, side == 0 ? b.bottom() : b.top() - h, w, h);
                } else {
                    const double y = b.top() + double(random() % 41) * 100 - h / 2;
                    rs << QRectF(side == 2 ? b.right() : b.left() - w, y, w, h);
                }
            }
            // The pieces joined along edges or by overlap, counted.
            QList<int> parent(rs.size());
            for (int i = 0; i < rs.size(); ++i) parent[i] = i;
            std::function<int(int)> rootOf = [&parent, &rootOf](int i) { return parent.at(i) == i ? i : (parent[i] = rootOf(parent.at(i))); };
            for (int i = 0; i < rs.size(); ++i)
                for (int j = i + 1; j < rs.size(); ++j) {
                    const QRectF& a = rs.at(i);
                    const QRectF& b = rs.at(j);
                    const double ox = std::min(a.right(), b.right()) - std::max(a.left(), b.left());
                    const double oy = std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top());
                    if ((ox > 0 && oy >= 0) || (oy > 0 && ox >= 0)) parent[rootOf(j)] = rootOf(i);
                }
            int pieces = 0;
            for (int i = 0; i < rs.size(); ++i) pieces += rootOf(i) == i;
            QList<QPolygonF> polys;
            for (const QRectF& r : std::as_const(rs)) polys << QPolygonF(r);
            const QList<QPolygonF> u = rf::unionOf(polys);
            QVERIFY2(u.size() == pieces, qPrintable(QStringLiteral("set %1: %2 pieces, %3 shapes out").arg(t).arg(pieces).arg(u.size())));
            ++sets;
        }
        QCOMPARE(sets, 3000);
    }

    // A via farm: the shapes are joined by how close they are, and the
    // metal is kept - the union its footprint, conducting as the vias in it
    // do (Conductor::share), where it was shrunk to their area, a block in
    // the footprint's middle.
    void aViaFarmIsMergedAndItsAreaKept()
    {
        const rf::Stack stack = stacked().stack();
        rf::Processing how;
        how.dropSlivers = false;
        how.viaDistance = 1e-6;   // 1 µm, wider than the gaps below
        QList<QPolygonF> farm;
        double wanted = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                farm << box(i * 0.4, j * 0.4, 0.2, 0.2);
                wanted += 0.04;
            }
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("V1")] = farm;
        const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
        QCOMPARE(done.report.viasFrom, 9);
        QVERIFY2(done.report.viasTo < 9 && done.report.viasTo >= 1, qPrintable(QString::number(done.report.viasTo)));
        QVERIFY2(says(done.report.lines, "merged 9 vias into"), qPrintable(done.report.summary()));
        QVERIFY2(says(done.report.lines, "each conducting as the vias in it"), qPrintable(done.report.summary()));
        // The metal it kept is the vias' own, to a part in a million: the
        // footprint's area times its share. The footprint the closing's,
        // about the farm's 1 µm square.
        double got = 0, footprint = 0;
        for (const rf::Conductor& c : done.conductors) {
            got += c.area() * c.share;
            footprint += c.area();
        }
        QVERIFY2(std::abs(got - wanted) / wanted < 1e-6,
                 qPrintable(QStringLiteral("%1 µm² of %2").arg(got).arg(wanted)));
        QVERIFY2(footprint > 2 * wanted && footprint < 1.1, qPrintable(QStringLiteral("%1 µm²").arg(footprint)));
        QVERIFY2(std::abs(done.report.areaKept - wanted / footprint) < 1e-6, qPrintable(QString::number(done.report.areaKept)));
        // As solid metal, for an export (asSolid()): the footprint thinned in
        // place to the vias' area, within a per cent, one shape for one.
        const QList<rf::Conductor> solid = rf::asSolid(done.conductors);
        QCOMPARE(solid.size(), done.conductors.size());
        double solidArea = 0;
        for (const rf::Conductor& c : solid) {
            QCOMPARE(c.share, 1.0);
            solidArea += c.area();
        }
        QVERIFY2(std::abs(solidArea - wanted) / wanted < 0.02, qPrintable(QStringLiteral("%1 µm² of %2").arg(solidArea).arg(wanted)));
        // A ring of vias round a guard ring (100 µm across, 4 µm wide, a
        // quarter metal) and a row along a strip (100 by 2 µm, half): each
        // thinned within its footprint, the ring still a ring over its metal
        // and the row as long - where scaled about its middle the ring fell
        // inside its own hole and the row lost its ends (the review of
        // 31dbfeb6).
        {
            rf::Conductor ring;
            ring.layer = QStringLiteral("V1");
            ring.outline = QPolygonF({QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100), QPointF(0, 50), QPointF(4, 50),
                                      QPointF(4, 96), QPointF(96, 96), QPointF(96, 4), QPointF(4, 4), QPointF(4, 50), QPointF(0, 50)});
            ring.share = 0.25;
            rf::Conductor row;
            row.layer = QStringLiteral("V1");
            row.outline = QPolygonF(QRectF(0, 200, 100, 2));
            row.share = 0.5;
            const QList<rf::Conductor> made = rf::asSolid({ring, row});
            QCOMPARE(made.size(), 2);
            for (int k = 0; k < 2; ++k) {
                const rf::Conductor& was = k == 0 ? ring : row;
                const double metal = was.area() * was.share;
                QVERIFY2(std::abs(made.at(k).area() - metal) / metal < 0.02, qPrintable(QStringLiteral("%1 of %2 µm²").arg(made.at(k).area()).arg(metal)));
                for (const QPointF& p : made.at(k).outline)
                    QVERIFY2(was.outline.containsPoint(p, Qt::OddEvenFill), qPrintable(QStringLiteral("%1, %2 outside").arg(p.x()).arg(p.y())));
            }
            const QRectF ringBox = made.at(0).outline.boundingRect(), rowBox = made.at(1).outline.boundingRect();
            QVERIFY2(ringBox.width() > 96 && ringBox.height() > 96, qPrintable(QStringLiteral("%1 by %2").arg(ringBox.width()).arg(ringBox.height())));
            QVERIFY2(rowBox.width() > 99, qPrintable(QString::number(rowBox.width())));
        }
        // Two pads' arrays joined by a row along their trace, merged into one
        // footprint - two 20 µm blocks and a neck 2 µm wide and 8 long, a
        // quarter metal: thinned only as far as it stays one piece, both pads
        // in it, its metal then above the vias' (the review of 62c7f001: the
        // largest piece kept alone lost the row and a pad).
        {
            rf::Conductor pads;
            pads.layer = QStringLiteral("V1");
            pads.outline = QPolygonF({QPointF(0, 0), QPointF(20, 0), QPointF(20, 9), QPointF(28, 9), QPointF(28, 0), QPointF(48, 0),
                                      QPointF(48, 20), QPointF(28, 20), QPointF(28, 11), QPointF(20, 11), QPointF(20, 20), QPointF(0, 20)});
            pads.share = 0.25;
            const QList<rf::Conductor> made = rf::asSolid({pads});
            QCOMPARE(made.size(), 1);
            const rf::Conductor& one = made.first();
            const QList<QPolygonF> within = rf::offsetOf({pads.outline}, 1e-3);   // (a side's own points inside)
            QCOMPARE(within.size(), 1);
            for (const QPointF& p : one.outline)
                QVERIFY2(within.first().containsPoint(p, Qt::OddEvenFill), qPrintable(QStringLiteral("%1, %2 outside").arg(p.x()).arg(p.y())));
            QVERIFY2(one.outline.containsPoint(QPointF(10, 10), Qt::OddEvenFill) && one.outline.containsPoint(QPointF(38, 10), Qt::OddEvenFill)
                         && one.outline.containsPoint(QPointF(24, 10), Qt::OddEvenFill),
                     "a pad or the row left out");
            // (Thinned as far as the neck lets it - less than the footprint,
            //  more than the vias' metal.)
            qInfo("two pads joined by a row: the footprint %.1f um2, the vias' %.1f, written %.1f", pads.area(), pads.area() * pads.share, one.area());
            QVERIFY2(one.area() < 0.9 * pads.area() && one.area() >= 0.98 * pads.area() * pads.share, qPrintable(QString::number(one.area())));
        }

        // Without that choice the union is solid metal - more than the vias
        // were, which is the honest alternative.
        how.keepArea = false;
        double loose = 0;
        for (const rf::Conductor& c : rf::processShapes(shapes, {}, how, stack).conductors) {
            QCOMPARE(c.share, 1.0);
            loose += c.area();
        }
        // Larger than the vias were - and no larger than the closing makes
        // it: the farm spans 1 µm each way, so the shape it leaves is
        // about 1 µm², not the 4 µm² of a growth that is never undone.
        QVERIFY2(loose > got * 1.5, qPrintable(QStringLiteral("%1 µm² against %2").arg(loose).arg(got)));
        QVERIFY2(loose < 2.0, qPrintable(QStringLiteral("%1 µm²: the closing was not undone").arg(loose)));

        // Vias further apart than the distance stay apart.
        how.keepArea = true;
        how.viaDistance = 1e-7;   // 0.1 µm, narrower than the 0.2 µm gaps
        QCOMPARE(rf::processShapes(shapes, {}, how, stack).conductors.size(), 9);
    }

    // Slivers: fill, assist shapes and the crumbs a boolean leaves.
    void sliversAreDroppedAndSaid()
    {
        const rf::Stack stack = stacked().stack();
        rf::Processing how;
        how.merge = false;
        how.sliverWidth = 1e-7;   // 0.1 µm
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("M1")] = {box(0, 0, 10, 2), box(0, 5, 10, 0.05), box(20, 0, 0.05, 0.05)};
        const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
        QCOMPARE(done.byLayer().value(QStringLiteral("M1")), 1);
        QCOMPARE(done.report.slivers, 2);
        QVERIFY2(says(done.report.lines, "dropped 2 shape(s) thinner than"), qPrintable(done.report.summary()));

        how.dropSlivers = false;
        QCOMPARE(rf::processShapes(shapes, {}, how, stack).byLayer().value(QStringLiteral("M1")), 3);
    }

    // A strip is found on its outline as drawn - a path's outline, or a
    // polygon drawn as one: two polylines its width apart either side of a
    // centreline, mitred at its corners, flush at its ends - and its
    // centreline and width with it, whichever corner its outline starts at
    // and whichever way round it runs. A rectangle turned is one of a single
    // segment along its longer way; one along x and y, a taper, a strip of
    // two widths and an odd outline are none. Processed, a strip keeps its
    // centreline, thinned as its curves would be, and its outline is made
    // of it (the centreline mesh).
    void aStripIsFoundWithItsCentreline()
    {
        // A bend of three segments, 6 wide, its corners mitred.
        const QPolygonF spine({QPointF(0, 0), QPointF(40, 30), QPointF(80, 30), QPointF(110, 70)});
        const QPolygonF outline = rf::stripOutline(spine, 6);
        QCOMPARE(int(outline.size()), 8);
        QPolygonF found;
        double width = 0;
        for (int start = 0; start < outline.size(); ++start)
            for (const bool backwards : {false, true}) {
                QPolygonF turned;
                for (int i = 0; i < outline.size(); ++i) turned << outline.at((start + (backwards ? -i : i) + 8 * outline.size()) % outline.size());
                QVERIFY2(rf::stripOf(turned, 1e-9, &found, &width), qPrintable(QStringLiteral("from %1%2").arg(start).arg(backwards ? ", backwards" : "")));
                QVERIFY(std::abs(width - 6) < 1e-9);
                QCOMPARE(int(found.size()), 4);
                // (Its centreline either way along: the same points.)
                const bool same = (found.first() - spine.first()).manhattanLength() < 1e-9 || (found.first() - spine.last()).manhattanLength() < 1e-9;
                QVERIFY(same);
            }
        // A rectangle turned 30 degrees, 50 by 10: along its 50.
        QPolygonF bar;
        const double c = std::cos(M_PI / 6), s = std::sin(M_PI / 6);
        for (const QPointF& p : {QPointF(0, 0), QPointF(50, 0), QPointF(50, 10), QPointF(0, 10)}) bar << QPointF(c * p.x() - s * p.y(), s * p.x() + c * p.y());
        QVERIFY(rf::stripOf(bar, 1e-9, &found, &width));
        QVERIFY2(std::abs(width - 10) < 1e-9 && std::abs(QLineF(found.first(), found.last()).length() - 50) < 1e-9,
                 qPrintable(QStringLiteral("%1 wide, %2 long").arg(width).arg(QLineF(found.first(), found.last()).length())));
        // None: along x and y; a taper; two widths; an odd count of corners.
        QVERIFY(!rf::stripOf(box(0, 0, 50, 10), 1e-9));
        QVERIFY(!rf::stripOf(QPolygonF({QPointF(0, 0), QPointF(50, 10), QPointF(50, 20), QPointF(0, 40)}), 1e-9));
        // (Two widths: the bend's middle segment 12 wide, its others 6, every
        //  side parallel to its own segment - its corners where the offset
        //  sides meet.)
        {
            const QList<double> half{3, 6, 3};
            const auto side = [&](int k, double sign) {
                const QPointF d = spine.at(k + 1) - spine.at(k);
                const QPointF n = QPointF(-d.y(), d.x()) / std::hypot(d.x(), d.y());
                return QLineF(spine.at(k) + sign * half.at(k) * n, spine.at(k + 1) + sign * half.at(k) * n);
            };
            const auto meet = [](const QLineF& a, const QLineF& b) {
                QPointF at;
                a.intersects(b, &at);
                return at;
            };
            QPolygonF uneven;
            for (const double sign : {1.0, -1.0}) {
                QList<QPointF> corners{side(0, sign).p1()};
                for (int k = 0; k + 1 < 3; ++k) corners << meet(side(k, sign), side(k + 1, sign));
                corners << side(2, sign).p2();
                if (sign < 0) std::reverse(corners.begin(), corners.end());
                for (const QPointF& p : std::as_const(corners)) uneven << p;
            }
            QCOMPARE(int(uneven.size()), 8);
            QVERIFY(!rf::stripOf(uneven, 1e-9));
        }
        QVERIFY(!rf::stripOf(QPolygonF({QPointF(0, 0), QPointF(50, 10), QPointF(40, 30)}), 1e-9));

        // Processed: kept as a strip, its outline its centreline's; a
        // centreline of many corners on a curve thinned, its sides its width
        // apart still.
        const rf::Stack stack = stacked().stack();
        QPolygonF arc;
        for (int k = 0; k <= 90; ++k) arc << QPointF(100 * std::cos(k * M_PI / 180), 100 * std::sin(k * M_PI / 180));
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("M1")] = {rf::stripOutline(arc, 8)};
        const rf::Processed done = rf::processShapes(shapes, {}, rf::Processing(), stack);
        QVERIFY2(done.ok() && done.conductors.size() == 1, qPrintable(done.problems.join(" | ")));
        const rf::Conductor& strip = done.conductors.first();
        QVERIFY(strip.isStrip());
        QVERIFY2(strip.spine.size() < 60 && strip.spine.size() > 10, qPrintable(QString::number(strip.spine.size())));
        QVERIFY(std::abs(strip.spineWidth - 8) < 2e-3);
        QVERIFY(rf::stripOf(strip.outline, 2e-3));
        QCOMPARE(strip.outline, rf::stripOutline(strip.spine, strip.spineWidth));
        QVERIFY2(says(done.report.lines, "Simplified 1 shape(s)' curves"), qPrintable(done.report.summary()));
        // Simplify off: as drawn, every corner kept.
        rf::Processing asDrawn;
        asDrawn.simplify = false;
        const rf::Processed drawn = rf::processShapes(shapes, {}, asDrawn, stack);
        QVERIFY(drawn.conductors.first().isStrip());
        QCOMPARE(int(drawn.conductors.first().spine.size()), int(arc.size()));
        // And a shape along x and y, or a via's, is none.
        shapes[QStringLiteral("M1")] = {box(0, 0, 50, 10)};
        QVERIFY(!rf::processShapes(shapes, {}, rf::Processing(), stack).conductors.first().isStrip());
    }

    // Snapping, healing, and dummy fill.
    void snappingHealingAndFill()
    {
        const rf::Stack stack = stacked().stack();
        {   // Snap: the points move to the grid, and a shape that loses
            // its area with them is gone.
            rf::Processing how;
            how.merge = false;
            how.dropSlivers = false;
            how.heal = false;
            how.snap = true;
            how.grid = 5e-9;   // 5 nm
            QMap<QString, QList<QPolygonF>> shapes;
            shapes[QStringLiteral("M1")] = {box(0.0013, 0.0027, 2, 2)};
            const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
            QCOMPARE(done.report.snapped, 1);
            QVERIFY2(says(done.report.lines, "grid"), qPrintable(done.report.summary()));
            const QPolygonF& on = done.conductors.first().outline;
            for (const QPointF& p : on) {
                QVERIFY2(std::abs(std::remainder(p.x(), 0.005)) < 1e-9, qPrintable(QString::number(p.x())));
                QVERIFY2(std::abs(std::remainder(p.y(), 0.005)) < 1e-9, qPrintable(QString::number(p.y())));
            }
            how.snap = false;
            QCOMPARE(rf::processShapes(shapes, {}, how, stack).report.snapped, 0);
        }
        {   // Heal: a repeated point is taken out.
            rf::Processing how;
            how.merge = false;
            how.dropSlivers = false;
            QPolygonF twice{QPointF(0, 0), QPointF(2, 0), QPointF(2, 0), QPointF(2, 2), QPointF(0, 2)};
            QMap<QString, QList<QPolygonF>> shapes;
            shapes[QStringLiteral("M1")] = {twice};
            const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
            QCOMPARE(done.report.healed, 1);
            QVERIFY(says(done.report.lines, "Healed 1 shape"));
            QVERIFY(std::abs(done.conductors.first().area() - 4.0) < 0.01);
        }
        {   // Dummy fill: dropped when it is asked for, floating metal
            // when it is not.
            rf::Processing how;
            how.merge = false;
            how.dropSlivers = false;
            QMap<QString, QList<QPolygonF>> shapes;
            shapes[QStringLiteral("M1")] = {box(0, 0, 2, 2)};
            shapes[QStringLiteral("M2")] = {box(0, 0, 1, 1), box(3, 3, 1, 1)};
            const QSet<QString> fill{QStringLiteral("M2")};
            rf::Processed kept = rf::processShapes(shapes, fill, how, stack);
            QCOMPARE(kept.byLayer().value(QStringLiteral("M2")), 2);
            QCOMPARE(kept.report.fill, 0);
            for (const rf::Conductor& c : kept.conductors)
                QCOMPARE(c.fill, c.layer == QLatin1String("M2"));

            how.removeFill = true;
            const rf::Processed dropped = rf::processShapes(shapes, fill, how, stack);
            QCOMPARE(dropped.byLayer().value(QStringLiteral("M2")), 0);
            QCOMPARE(dropped.report.fill, 2);
            QVERIFY(says(dropped.report.lines, "Dropped 2 dummy fill"));
        }
    }

    // A layer with shapes that the stack has not, and a model whose every
    // shape is dropped, are said rather than passed over.
    void whatCannotBeProcessedIsSaid()
    {
        const rf::Stack stack = stacked().stack();
        rf::Processing how;
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("M9")] = {box(0, 0, 2, 2)};
        QVERIFY2(says(rf::processShapes(shapes, {}, how, stack).problems, "no layer of the stack"),
                 "a layer the stack has not went unsaid");

        QMap<QString, QList<QPolygonF>> slivers;
        slivers[QStringLiteral("M1")] = {box(0, 0, 0.01, 0.01)};
        const rf::Processed gone = rf::processShapes(slivers, {}, how, stack);
        QVERIFY(gone.conductors.isEmpty());
        QVERIFY2(says(gone.problems, "nothing is left to solve on"), qPrintable(gone.problems.join(" | ")));
    }

    // The model's own shapes (Layout > Shapes): each number an expression of
    // the parameters, in µm as the layout's; a rectangle by its corner or
    // its centre, a polygon by its corners, a via by its centre and side, a
    // path outlined as gdstk outlines one - flush, extended by half its width,
    // round; added to the file's shapes on their layer by the processing.
    // One that cannot be read - a width no number, a polygon of two corners,
    // no layer, a layer of the stack no shape may be on - left out, and said.
    void theModelsOwnShapesAreItsParameters()
    {
        rf::Model m = stacked();
        rf::Node* parameters = m.root.child(QStringLiteral("rf_parameters"));
        QVERIFY(parameters != nullptr);
        parameters->props.insert(QStringLiteral("rows"), QJsonArray{QJsonArray{"w", "50[um]"}, QJsonArray{"L", "2*w + 100[um]"}});
        QVERIFY(m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes"))) != nullptr);
        const QString shapes = m.layout().child(QStringLiteral("rf_shapes"))->tag;
        const auto shape = [&](const QJsonObject& props) {
            rf::Node n = m.make(QStringLiteral("rf_shape"));
            n.props = props;
            const rf::Node* added = m.add(shapes, n);
            return added != nullptr ? added->tag : QString();
        };
        const QString rect = shape({{"layer", "M1"}, {"x", "10[um]"}, {"y", "-5[um]"}, {"width", "L"}, {"height", "w"}});
        const QString centred = shape({{"layer", "M2"}, {"anchor", "centre"}, {"x", "0"}, {"y", "0"}, {"width", "20[um]"}, {"height", "w/5"}});
        const QString poly = shape({{"layer", "M1"}, {"shape", "polygon"}, {"points", QJsonArray{QJsonArray{"0", "0"}, QJsonArray{"w", "0"}, QJsonArray{"0", "w"}}}});
        const QString via = shape({{"layer", "V1"}, {"shape", "via"}, {"x", "5[um]"}, {"y", "5[um]"}, {"size", "2[um]"}});
        const QString flush = shape({{"layer", "M2"}, {"shape", "path"}, {"points", QJsonArray{QJsonArray{"0", "100[um]"}, QJsonArray{"100[um]", "100[um]"}}}, {"path_width", "10[um]"}});
        QString problem;
        QStringList problems;
        QList<rf::OwnShape> own = rf::ownShapesOf(m, &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
        QCOMPARE(int(own.size()), 5);
        const auto of = [&own](const QString& tag) {
            for (const rf::OwnShape& s : own)
                if (s.tag == tag) return s;
            return rf::OwnShape();
        };
        QCOMPARE(of(rect).layer, QStringLiteral("M1"));
        QCOMPARE(of(rect).outline.boundingRect(), QRectF(10, -5, 200, 50));
        QCOMPARE(of(centred).outline.boundingRect(), QRectF(-10, -5, 20, 10));
        QCOMPARE(std::abs(rf::areaOf(of(poly).outline)), 1250.0);
        QCOMPARE(of(via).outline.boundingRect(), QRectF(4, 4, 2, 2));
        QVERIFY2(std::abs(std::abs(rf::areaOf(of(flush).outline)) - 1000) < 1e-6, qPrintable(QString::number(rf::areaOf(of(flush).outline))));
        QCOMPARE(of(flush).outline.boundingRect(), QRectF(0, 95, 100, 10));
        // Its ends: extended by half its width at each, or round.
        m.find(flush)->props.insert(QStringLiteral("ends"), QStringLiteral("extended"));
        own = rf::ownShapesOf(m);
        QVERIFY2(std::abs(std::abs(rf::areaOf(of(flush).outline)) - 1100) < 1e-6, qPrintable(QString::number(rf::areaOf(of(flush).outline))));
        m.find(flush)->props.insert(QStringLiteral("ends"), QStringLiteral("round"));
        own = rf::ownShapesOf(m);
        QVERIFY2(std::abs(std::abs(rf::areaOf(of(flush).outline)) - (1000 + M_PI * 25)) < 1.0, qPrintable(QString::number(rf::areaOf(of(flush).outline))));

        // A parameter changed: the shapes with it.
        parameters->props.insert(QStringLiteral("rows"), QJsonArray{QJsonArray{"w", "80[um]"}, QJsonArray{"L", "2*w + 100[um]"}});
        own = rf::ownShapesOf(m);
        QCOMPARE(of(rect).outline.boundingRect(), QRectF(10, -5, 260, 80));

        // A bare number is in the Layout's unit, as its coordinates are drawn
        // (it was read as metres, a strip of "100" a
        // hundred metres long); a JSON number too; a length with a unit, or
        // of the parameters, as it says.
        {
            const QString bare = shape({{"layer", "M1"}, {"x", "100"}, {"y", QJsonValue(-20.0)}, {"width", "300"}, {"height", "w"}});
            own = rf::ownShapesOf(m);
            QCOMPARE(of(bare).outline.boundingRect(), QRectF(100, -20, 300, 80));
            m.layout().props.insert(QStringLiteral("unit"), QStringLiteral("1[mm]"));
            own = rf::ownShapesOf(m);
            QCOMPARE(of(bare).outline.boundingRect(), QRectF(100000, -20000, 300000, 80));
            QCOMPARE(of(rect).outline.boundingRect(), QRectF(10, -5, 260, 80));
            m.layout().props.remove(QStringLiteral("unit"));
            m.remove(bare);
            own = rf::ownShapesOf(m);
        }

        // Processed with the file's shapes, on their layers: a model of its
        // own shapes alone over an empty layout.
        rf::Node* map = m.layout().child(QStringLiteral("rf_layer_map"));
        map->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"1", "0", "M1"});
        qucs_s::layout::Layout empty;
        qucs_s::layout::Cell top;
        top.name = QStringLiteral("TOP");
        empty.cells << top;
        empty.byName.insert(top.name, 0);
        const rf::Processed processed = rf::processRead(m, empty, QStringLiteral("none"));
        QStringList layers;
        for (const rf::Conductor& c : processed.conductors) layers << c.layer;
        QVERIFY2(layers.contains(QStringLiteral("M1")) && layers.contains(QStringLiteral("M2")), qPrintable(layers.join(", ")));
        QVERIFY2(processed.report.lines.join(' ').contains(QStringLiteral("of the model's own")), qPrintable(processed.report.lines.join(" | ")));
        QVERIFY(m.problems().filter(QStringLiteral("names no file")).isEmpty());

        // What cannot be read: left out, and said.
        m.find(rect)->props.insert(QStringLiteral("width"), QStringLiteral("q * 2"));
        m.find(poly)->props.insert(QStringLiteral("points"), QJsonArray{QJsonArray{"0", "0"}, QJsonArray{"w", "0"}});
        m.find(via)->props.insert(QStringLiteral("layer"), QStringLiteral("M9"));
        m.find(centred)->props.insert(QStringLiteral("layer"), QString());
        problems.clear();
        own = rf::ownShapesOf(m, &problems);
        QCOMPARE(int(own.size()), 1);
        const QString said = problems.join(QStringLiteral(" | "));
        QVERIFY2(said.contains(QStringLiteral("width is not a number")), qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("a polygon of 2 corner(s)")), qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("M9, which is no layer of the stack")), qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("is on no layer")), qPrintable(said));
        // A ground plane has no shapes.
        rf::Node& ground = add(m.stackNode(), "rf_ground", QStringLiteral("g1"), QStringLiteral("GND"));
        Q_UNUSED(ground);
        m.find(flush)->props.insert(QStringLiteral("layer"), QStringLiteral("GND"));
        problems.clear();
        own = rf::ownShapesOf(m, &problems);
        QVERIFY2(problems.join(' ').contains(QStringLiteral("a ground plane has no shapes")), qPrintable(problems.join(" | ")));
        Q_UNUSED(problem);
    }

    // A hole in a slanted outline, as the processing writes it, a slanted
    // tee and a chamfered pad, at cells of 10 µm and at three across: their
    // cells meet edge to edge all round, as the solve pairs them (by their
    // sides' middles, to a nanometre) - a hexagon's beside its square hole's
    // corners met no cell across x = 60 for 17.5 µm, the pieces under and
    // over the hole ending at its corners and their ends not lines of the
    // piece beside them (the review of 27cc932c). And the keyhole the
    // processing writes for a square hole - its way back running on along
    // the hole's edge, no exact pair - is two loops to loopsOf().
    void aHoleInASlantedOutlineIsMeshedWhole()
    {
        const auto processedOf = [](const QPolygonF& shape) {
            rf::Model m = stacked();
            m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes")));
            rf::Node n = m.make(QStringLiteral("rf_shape"));
            QJsonArray rows;
            for (const QPointF& p : shape) rows.append(QJsonArray{QString::number(p.x()) + "[um]", QString::number(p.y()) + "[um]"});
            n.props = QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
            m.add(m.layout().child(QStringLiteral("rf_shapes"))->tag, n);
            m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"1", "0", "M1"});
            qucs_s::layout::Layout empty;
            qucs_s::layout::Cell top;
            top.name = QStringLiteral("TOP");
            empty.cells << top;
            empty.byName.insert(top.name, 0);
            return std::make_pair(rf::processRead(m, empty, QStringLiteral("none")), m.solveStack());
        };
        const QList<QPolygonF> shapes{
            // A hexagon with a square hole x 20-60, y 20-40, by a keyhole.
            QPolygonF({{0, 25}, {50, 0}, {100, 25}, {100, 75}, {50, 100}, {0, 75}, {0, 30}, {20, 30}, {20, 40}, {60, 40}, {60, 20}, {20, 20}, {20, 30}, {0, 30}}),
            // A square frame.
            QPolygonF({{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 50}, {30, 50}, {30, 70}, {70, 70}, {70, 30}, {30, 30}, {30, 50}, {0, 50}}),
            // A slanted tee: at three across its stem met its bar along no
            // side, its base cut (the review of 9acb72fd).
            QPolygonF({{0, 0}, {200, 0}, {200, 40}, {120, 40}, {160, 140}, {140, 148}, {100, 40}, {0, 40}}),
            // A chamfered pad: its trapezoid and the rectangle over it met
            // along 70 µm of 100 at three across.
            QPolygonF({{0, 0}, {80, 0}, {100, 20}, {100, 100}, {0, 100}})};
        for (int s = 0; s < shapes.size(); ++s)
        for (const bool fine : {true, false}) {
            const QPolygonF& shape = shapes.at(s);
            const auto [processed, stack] = processedOf(shape);
            QCOMPARE(int(processed.conductors.size()), 1);
            const QPolygonF whole = processed.conductors.first().outline;
            QCOMPARE(int(rf::loopsOf(whole).size()), s < 2 ? 2 : 1);
            // At cells of 10 µm, and as the tab meshes: three across, 25 µm at most.
            rf::MeshSettings how;
            how.maximum = fine ? 10e-6 : 25e-6;
            how.cellsAcross = fine ? 0 : 3;
            const rf::Mesh mesh = rf::meshOf(processed.conductors, how, stack, 1e9);
            QVERIFY(!mesh.cells.isEmpty());
            QHash<QString, int> sides;
            QHash<QString, QLineF> lines;
            for (const rf::MeshCell& c : mesh.cells)
                for (int k = 0; k < c.outline.size(); ++k) {
                    const QPointF a = c.outline.at(k), b = c.outline.at((k + 1) % c.outline.size());
                    if (QLineF(a, b).length() < 1e-9) continue;
                    const QString key = QStringLiteral("%1,%2").arg(std::llround((a.x() + b.x()) / 2 * 1e3)).arg(std::llround((a.y() + b.y()) / 2 * 1e3));
                    sides[key] += 1;
                    lines.insert(key, QLineF(a, b));
                }
            double lone = 0;
            QString where;
            for (auto it = sides.cbegin(); it != sides.cend(); ++it) {
                if (it.value() != 1) continue;
                const QLineF l = lines.value(it.key());
                const QPointF mid = (l.p1() + l.p2()) / 2, d = l.p2() - l.p1();
                // (Metal both sides of it, 0.15 µm off: the slabs' corners are
                //  snapped up to 0.1 µm off the drawn outline - piecesOf().)
                const QPointF off = QPointF(-d.y(), d.x()) / l.length() * 0.15;
                if (whole.containsPoint(mid + off, Qt::OddEvenFill) && whole.containsPoint(mid - off, Qt::OddEvenFill)) {
                    lone += l.length();
                    where += QStringLiteral(" (%1, %2)-(%3, %4)").arg(l.p1().x()).arg(l.p1().y()).arg(l.p2().x()).arg(l.p2().y());
                }
            }
            QVERIFY2(lone < 1e-9, qPrintable(QStringLiteral("shape %1 at %2: %3 µm of cell sides inside the metal are no other cell's:%4")
                                                 .arg(s).arg(fine ? "10 µm" : "three across").arg(lone).arg(where)));
        }
        // The keyhole of a square hole as the processing writes it: its way
        // back runs on along the hole's edge.
        const QPolygonF frame({{100, 0}, {100, 100}, {0, 100}, {0, 30}, {30, 30}, {30, 70}, {70, 70}, {70, 30}, {0, 30}, {0, 0}});
        const QList<QPolygonF> loops = rf::loopsOf(frame);
        QCOMPARE(int(loops.size()), 2);
        QList<double> areas{std::abs(rf::areaOf(loops.at(0))), std::abs(rf::areaOf(loops.at(1)))};
        std::sort(areas.begin(), areas.end());
        QCOMPARE(areas, (QList<double>{1600.0, 10000.0}));
    }

    // A disc or ring fed by strips 20 µm wide reaching 10 µm past it, no
    // part of them a strip of its own, at 30 or 45 degrees: meshed in its
    // feeds' frame too, its R within half a per cent of it fed at 0 degrees
    // (0.08 % over at most) - in the layout's axes, the circle at 45 degrees
    // read 3.0 % under. (Fed at 90, the ring's mesh is another, 1,583 cells
    // to 1,452, and reads 0.5 % over it fed at 0.) And the loop's stubs at each face, from its
    // edge to the cells' centres, as wide in all as the face: 20 µm. With a
    // shape on M2 over it, it is meshed in the layout's axes, its cells
    // square to them and clipped by the slanted face; its stubs taken as
    // wide as the cells' outlines across came to 26.6 to 32.5 µm, the
    // circle at 45 degrees 4.2 % under it fed square (0.27 % now).
    void aCurveFedShortAtASlantIsTurned()
    {
        struct Fed {
            int cells = 0;
            bool turned = false;
            double squares = 0, stubs = 0;
        };
        const auto fed = [this](bool ring, double degrees, bool over) {
            rf::Model m = stacked();
            m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes")));
            const QString shapes = m.layout().child(QStringLiteral("rf_shapes"))->tag;
            rf::Node round = m.make(QStringLiteral("rf_shape"));
            round.props = ring ? QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}, {"inner_radius", "50[um]"}}
                               : QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}};
            m.add(shapes, round);
            const double a = degrees * M_PI / 180;
            const QPointF along(std::cos(a), std::sin(a)), across(-std::sin(a), std::cos(a));
            for (const double side : {1.0, -1.0}) {
                QJsonArray rows;
                for (const QPointF& p : {side * 95 * along + 10 * across, side * 110 * along + 10 * across, side * 110 * along - 10 * across,
                                         side * 95 * along - 10 * across})
                    rows.append(QJsonArray{QString::number(p.x(), 'g', 12) + "[um]", QString::number(p.y(), 'g', 12) + "[um]"});
                rf::Node feed = m.make(QStringLiteral("rf_shape"));
                feed.props = QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
                m.add(shapes, feed);
            }
            if (over) {
                rf::Node plate = m.make(QStringLiteral("rf_shape"));
                plate.props = QJsonObject{{"layer", "M2"}, {"shape", "rectangle"}, {"x", "-20[um]"}, {"y", "-20[um]"}, {"width", "40[um]"}, {"height", "40[um]"}};
                m.add(shapes, plate);
            }
            m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"1", "0", "M1"} << QJsonArray{"2", "0", "M2"});
            qucs_s::layout::Layout empty;
            qucs_s::layout::Cell top;
            top.name = QStringLiteral("TOP");
            empty.cells << top;
            empty.byName.insert(top.name, 0);
            const rf::Processed processed = rf::processRead(m, empty, QStringLiteral("none"));
            const rf::Stack stack = m.solveStack();
            const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), stack, 1e9);
            const rf::Layer* metal = stack.named(QStringLiteral("M1"));
            const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, 110 * along, -110 * along);
            Fed out;
            out.cells = int(mesh.cells.size());
            out.turned = says(mesh.lines, "meshed in the frame of its feeds");
            if (!loop.ok() || metal == nullptr) return out;
            out.squares = loop.rDc * metal->sigma * metal->thickness;
            // (The stubs going in: the runs from the face at 110 µm out.)
            const QPointF face = 110e-6 * along;
            for (const rf::Filament& f : loop.runs)
                if (std::abs((f.from - face).x() * along.x() + (f.from - face).y() * along.y()) < 3e-9) out.stubs += f.width * 1e6;
            return out;
        };
        for (const bool ring : {false, true}) {
            const QString shape = ring ? QStringLiteral("a ring") : QStringLiteral("a circle");
            const Fed square = fed(ring, 0, false);
            QVERIFY2(square.squares > 0 && !square.turned, qPrintable(shape));
            for (const double degrees : {30.0, 45.0}) {
                const Fed slant = fed(ring, degrees, false);
                const QString what = QStringLiteral("%1 fed at %2 degrees 10 µm past it").arg(shape).arg(degrees);
                QVERIFY2(slant.turned, qPrintable(what));
                QVERIFY2(std::abs(slant.squares / square.squares - 1) < 0.005,
                         qPrintable(QStringLiteral("%1: %2 squares against %3 fed at 0").arg(what).arg(slant.squares).arg(square.squares)));
                QVERIFY2(std::abs(slant.stubs / 20 - 1) < 1e-3, qPrintable(QStringLiteral("%1: its stubs %2 µm across").arg(what).arg(slant.stubs)));
            }
        }
        const Fed square = fed(false, 0, true), slant = fed(false, 45, true);
        QVERIFY2(!slant.turned && slant.squares > 0, qPrintable(QStringLiteral("%1 cells").arg(slant.cells)));
        QVERIFY2(std::abs(slant.stubs / 20 - 1) < 1e-3, qPrintable(QStringLiteral("under M2, its stubs %1 µm across").arg(slant.stubs)));
        QVERIFY2(std::abs(slant.squares / square.squares - 1) < 0.01,
                 qPrintable(QStringLiteral("under M2 at 45 degrees: %1 squares against %2 fed at 0").arg(slant.squares).arg(square.squares)));
    }

    // Two parts of one conductor face a gap across two of its walls facing
    // each other, as two conductors do: a slot 4 µm wide, a jaw 4 and 60 µm
    // across, a comb of one conductor its fingers 10 µm apart. Where the
    // nearest point was one of its corners - a slot's arm's tip -, the side
    // there taken for its wall was the last of the two, the arm's end, which
    // faces nothing across the slot: none of them read a gap, and Automatic
    // took a thick metal on them as a sheet.
    void aGapWithinOneConductorIsAcrossItsWalls()
    {
        const QList<QPair<double, QList<QPointF>>> shapes{
            {4, {{0, 0}, {100, 0}, {100, 100}, {52, 100}, {52, 20}, {48, 20}, {48, 100}, {0, 100}}},
            {4, {{0, 0}, {100, 0}, {100, 48}, {20, 48}, {20, 52}, {100, 52}, {100, 100}, {0, 100}}},
            {60, {{0, 0}, {100, 0}, {100, 20}, {20, 20}, {20, 80}, {100, 80}, {100, 100}, {0, 100}}},
            {10, {{0, 0}, {100, 0}, {100, 10}, {10, 10}, {10, 20}, {100, 20}, {100, 30}, {10, 30}, {10, 40}, {100, 40}, {100, 50}, {0, 50}}}};
        for (const auto& [gap, points] : shapes) {
            QJsonArray rows;
            for (const QPointF& q : points) rows.append(QJsonArray{QString::number(q.x()) + "[um]", QString::number(q.y()) + "[um]"});
            const rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}}});
            const rf::Mesh mesh = rf::meshOf(rf::processRead(m, nothingRead(), QStringLiteral("none")).conductors, rf::MeshSettings(), m.solveStack(), 1e9);
            QVERIFY2(mesh.ok() && std::abs(mesh.gapByLayer.value(QStringLiteral("M1")) / gap - 1) < 0.01,
                     qPrintable(QStringLiteral("%1 points, a gap of %2 µm: read %3").arg(points.size()).arg(gap).arg(mesh.gapByLayer.value(QStringLiteral("M1")))));
        }
    }

    // A curve turned (turnOf()) beside other shapes on its layer, and fed
    // square with a strip at a slant.
    // - Its turned mesh cuts it alone, the rest there for the gaps it faces,
    //   and the whole mesh counts its turned cells, not the bands it has in
    //   the layout's axes. A ring 45 by 35 µm inside a ring fed at 30 degrees
    //   is cut as it is fed at 0 (3,075 cells; counted as its bands, 2,048),
    //   none of the turned ring's cells inside it (two slivers of its edge,
    //   their centres in no outline, were the turned ring's); a strip 2 by
    //   60 µm beside a ring 100 by 99 µm fed at 30 degrees is cut as many
    //   across as it is, 3 (125 cells; it was 1 across, 20 cells, the mesh
    //   4,063 of the 8,000).
    // - What its turned mesh measures among its neighbours slanted is not
    //   said, its own cut to fewer across alone; nor a gap between two of one
    //   conductor's parts with it halfway between them. A strip 2 by 60 µm
    //   1.5 µm past the ring fed at 30 degrees: its gap line said twice, of
    //   1.56 and 1.54 µm, and its narrowest conductor's too. 40 µm past, or
    //   with no strip: "thick beside gaps as narrow as 1.75 µm", of the
    //   ring's own parts banded about its feeds in the layout's axes.
    // - Its own measures are those of it fed square: its gaps between its
    //   own parts (Mesh::gapByLayer, by which Automatic solves a metal as
    //   its faces) - pairs across the wedges where its feeds meet its curve
    //   read 0.85 µm for the ring at 45 degrees, 5.06 for the disc, and
    //   Automatic took a 3 µm metal as its faces, C 2.6 % over -; and its
    //   narrowest part, cells across and cells there, none of them said
    //   with it turned. The ring 100 by 99 µm fed at 30 degrees, alone more
    //   than the 8,000 at 10 across, is cut at 3, and that said, of it; with
    //   20,000 and a strip 0.5 µm wide beside it, the narrowest conductor's
    //   cells across are said of the strip, not the ring's 5 (the mesh's
    //   across the least, for the convergence check).
    // - A disc or a ring with a stub of 20 µm square on it as well, fed at
    //   0, 30 or 45 degrees, is said to have no gaps: two of one conductor's
    //   parts face a gap only across two of its walls facing each other.
    //   (Looked for in the middle of their boxes' overlap, the disc read gaps
    //   of 1.79 and 1.75 µm; the ring, with the conductor looked for between
    //   their nearest points, 3.15 and 0.85 µm, two of its parts ending on
    //   its edge a hair apart, then 80 µm across its hole along a chord. Fed
    //   square, its two feeds' ends were a gap of 199 µm, the ring between
    //   them.) Fed at 30 or 45 it is turned, its feeds' sides the longer, its
    //   R within half a per cent of it fed at 0: kept in the layout's axes
    //   for any square side, it was banded, the disc at 30 degrees 1.8 %
    //   under.
    // - Fed at 0 degrees with a stub at 120, a straight side along an axis,
    //   it is not turned: turned by the stub, its feeds were banded about,
    //   its R 1.8 % under.
    // - Its C fed at 0 degrees within half a per cent of it turned at 30
    //   (0.16 % over): a cell clipped by the curve, its charge spread over
    //   its box past the metal, read the disc's C 1.7 % over.
    void aTurnedCurveMeshesItsNeighboursAsItIs()
    {
        // (A ring or a circle of 100 µm fed by two strips 20 µm wide from 95
        //  to 150 µm out at \a degrees, a ring of \a inner its hole's radius,
        //  and \a more shapes on M1.)
        const auto meshOf = [this](double inner, double degrees, const QList<QJsonObject>& more, rf::Processed* processed, rf::Stack* stack,
                                   const rf::MeshSettings& how = rf::MeshSettings()) {
            rf::Model m = stacked();
            m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes")));
            const QString shapes = m.layout().child(QStringLiteral("rf_shapes"))->tag;
            rf::Node round = m.make(QStringLiteral("rf_shape"));
            round.props = inner > 0 ? QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"},
                                                  {"inner_radius", QString::number(inner) + "[um]"}}
                                    : QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}};
            m.add(shapes, round);
            const double a = degrees * M_PI / 180;
            const QPointF along(std::cos(a), std::sin(a)), across(-std::sin(a), std::cos(a));
            const double half = inner > 90 ? 5 : 10, from = inner > 90 ? 99.5 : 95;
            QList<QJsonObject> all = more;
            for (const double side : {1.0, -1.0}) {
                QJsonArray rows;
                for (const QPointF& p : {side * from * along + half * across, side * 150 * along + half * across, side * 150 * along - half * across,
                                         side * from * along - half * across})
                    rows.append(QJsonArray{QString::number(p.x(), 'g', 12) + "[um]", QString::number(p.y(), 'g', 12) + "[um]"});
                all << QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
            }
            for (const QJsonObject& props : std::as_const(all)) {
                rf::Node shape = m.make(QStringLiteral("rf_shape"));
                shape.props = props;
                m.add(shapes, shape);
            }
            m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"1", "0", "M1"});
            qucs_s::layout::Layout empty;
            qucs_s::layout::Cell top;
            top.name = QStringLiteral("TOP");
            empty.cells << top;
            empty.byName.insert(top.name, 0);
            *processed = rf::processRead(m, empty, QStringLiteral("none"));
            *stack = m.solveStack();
            return rf::meshOf(processed->conductors, how, *stack, 1e9);
        };
        rf::Processed processed;
        rf::Stack stack;
        // Its own measures, as fed square.
        for (const double inner : {50.0, 0.0}) {
            const rf::Mesh square = meshOf(inner, 0, {}, &processed, &stack);
            for (const double degrees : {30.0, 45.0}) {
                const rf::Mesh mesh = meshOf(inner, degrees, {}, &processed, &stack);
                const QString what = QStringLiteral("%1 fed at %2 degrees").arg(inner > 0 ? "a ring" : "a disc").arg(degrees);
                QVERIFY2(mesh.gapByLayer.contains(QStringLiteral("M1")) == square.gapByLayer.contains(QStringLiteral("M1"))
                             && std::abs(mesh.gapByLayer.value(QStringLiteral("M1"), 1) / square.gapByLayer.value(QStringLiteral("M1"), 1) - 1) < 0.01,
                         qPrintable(QStringLiteral("%1: gaps of %2 µm, fed square %3").arg(what).arg(mesh.gapByLayer.value(QStringLiteral("M1"))).arg(square.gapByLayer.value(QStringLiteral("M1")))));
                QVERIFY2(!says(mesh.lines, "beside gaps"), qPrintable(what + ": " + mesh.lines.join(" | ")));
                QCOMPARE(mesh.across, square.across);
                QVERIFY2(square.narrowest > 0 && std::abs(mesh.narrowest / square.narrowest - 1) < 1e-3 && std::abs(mesh.acrossCell / square.acrossCell - 1) < 1e-3,
                         qPrintable(QStringLiteral("%1: narrowest %2 µm, cells %3 there; fed square %4, %5").arg(what).arg(mesh.narrowest).arg(mesh.acrossCell).arg(square.narrowest).arg(square.acrossCell)));
            }
        }
        for (const double inner : {0.0, 50.0}) {
            double square = 0;
            for (const double degrees : {0.0, 30.0, 45.0}) {
                QJsonArray rows;
                for (const QPointF& p : {QPointF(-10, 95), QPointF(-10, 110), QPointF(10, 110), QPointF(10, 95)})
                    rows.append(QJsonArray{QString::number(p.x()) + "[um]", QString::number(p.y()) + "[um]"});
                const rf::Mesh mesh = meshOf(inner, degrees, {QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}}}, &processed, &stack);
                const QString what = QStringLiteral("%1 fed at %2 degrees with a square stub").arg(inner > 0 ? "a ring" : "a disc").arg(degrees);
                QVERIFY2(mesh.ok() && says(mesh.lines, "frame of its feeds") == (degrees != 0) && !says(mesh.lines, "beside gaps"),
                         qPrintable(what + ": " + mesh.lines.join(" | ")));
                QVERIFY2(!mesh.gapByLayer.contains(QStringLiteral("M1")),
                         qPrintable(QStringLiteral("%1: gaps of %2 µm").arg(what).arg(mesh.gapByLayer.value(QStringLiteral("M1")))));
                const double a = degrees * M_PI / 180;
                const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, 150 * QPointF(std::cos(a), std::sin(a)),
                                                             -150 * QPointF(std::cos(a), std::sin(a)));
                const rf::Layer* metal = stack.named(QStringLiteral("M1"));
                QVERIFY2(loop.ok() && metal != nullptr, qPrintable(what + ": " + loop.problems.join(" | ")));
                const double squares = loop.rDc * metal->sigma * metal->thickness;
                if (degrees == 0) square = squares;
                QVERIFY2(std::abs(squares / square - 1) < 0.005, qPrintable(QStringLiteral("%1: %2 squares, fed at 0 %3").arg(what).arg(squares).arg(square)));
            }
        }
        {
            const rf::Mesh mesh = meshOf(99, 30, {}, &processed, &stack);
            QVERIFY2(mesh.ok() && mesh.across == 3 && says(mesh.warnings, "would alone be more than"),
                     qPrintable(QStringLiteral("%1 across: %2").arg(mesh.across).arg(mesh.lines.join(" | "))));
            // (The ring of aRoundShapeIsMeshedAsOneRegion, fed once, with a
            //  strip 0.5 µm wide, at Most cells 20,000: the whole cut at 7
            //  across, more than the dense solve's 8,000 and so refused,
            //  its lines said; the ring's own mesh cut to 5, not refused
            //  alone for those 8,000 - refused, the ring was counted as its
            //  bands, and the strip cut at 3.)
            rf::MeshSettings more;
            more.most = 20000;
            const double a = 30 * M_PI / 180;
            const QPointF along(std::cos(a), std::sin(a)), side(-5 * std::sin(a), 5 * std::cos(a));
            QJsonArray feed;
            for (const QPointF& p : {99.5 * along - side, 160 * along - side, 160 * along + side, 99.5 * along + side})
                feed.append(QJsonArray{QString::number(p.x(), 'f', 6) + "[um]", QString::number(p.y(), 'f', 6) + "[um]"});
            const rf::Model thin = shaped({QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}, {"inner_radius", "99[um]"}},
                                           QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", feed}},
                                           QJsonObject{{"layer", "M1"}, {"shape", "rectangle"}, {"x", "-10[um]"}, {"y", "-130[um]"}, {"width", "20[um]"}, {"height", "0.5[um]"}}});
            const rf::Mesh beside = rf::meshOf(rf::processRead(thin, nothingRead(), QStringLiteral("none")).conductors, more, thin.solveStack(), 1e9);
            QString line;
            for (const QString& l : beside.lines)
                if (l.contains(QStringLiteral("across the narrowest conductor"))) line = l;
            const int there = line.section(QLatin1Char(' '), 0, 0).toInt();
            QVERIFY2(there >= 6 && std::abs(beside.narrowest - 0.5) < 1e-3 && std::abs(there * beside.acrossCell / beside.narrowest - 1) < 0.01
                         && beside.across < there && says(beside.warnings, "would alone be more than"),
                     qPrintable(QStringLiteral("%1 across the mesh, %2 µm narrowest, cells %3: %4").arg(beside.across).arg(beside.narrowest).arg(beside.acrossCell).arg(line)));
        }
        // A ring inside the ring.
        const QJsonObject ring{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "45[um]"}, {"inner_radius", "35[um]"}};
        QMap<double, int> inside;
        for (const double degrees : {0.0, 30.0}) {
            const rf::Mesh mesh = meshOf(50, degrees, {ring}, &processed, &stack);
            QVERIFY2(mesh.ok(), qPrintable(mesh.problems.join(" | ")));
            QCOMPARE(says(mesh.lines, "meshed in the frame of its feeds"), degrees != 0);
            int turnedInside = 0;
            for (const rf::MeshCell& cell : mesh.cells) {
                const bool in = std::hypot(cell.centre.x(), cell.centre.y()) < 47.5;
                if (in && cell.angle == 0) ++inside[degrees];
                if (in && cell.angle != 0) ++turnedInside;
            }
            QVERIFY2(turnedInside == 0, qPrintable(QStringLiteral("fed at %1 degrees: %2 of the turned ring's cells inside the ring in it").arg(degrees).arg(turnedInside)));
        }
        QVERIFY2(inside.value(0) > 0 && std::abs(double(inside.value(30)) / inside.value(0) - 1) < 0.05,
                 qPrintable(QStringLiteral("the ring inside: %1 cells, %2 fed at 0").arg(inside.value(30)).arg(inside.value(0))));
        // A strip beside a thin ring.
        {
            const QJsonObject strip{{"layer", "M1"}, {"shape", "rectangle"}, {"x", "-30[um]"}, {"y", "-104[um]"}, {"width", "60[um]"}, {"height", "2[um]"}};
            const rf::Mesh mesh = meshOf(99, 30, {strip}, &processed, &stack);
            QVERIFY2(mesh.ok() && says(mesh.lines, "meshed in the frame of its feeds"), qPrintable(mesh.lines.join(" | ")));
            int cells = 0;
            for (const rf::MeshCell& cell : mesh.cells)
                if (cell.centre.y() < -101.5 && std::abs(cell.centre.x()) < 30) ++cells;
            QVERIFY2(mesh.cells.size() <= 8000 && cells >= 60,
                     qPrintable(QStringLiteral("the strip %1 cells, the mesh %2: %3").arg(cells).arg(mesh.cells.size()).arg(mesh.lines.join(" | "))));
        }
        // A strip past the ring fed at 30 degrees, far and near.
        for (const double gap : {40.0, 1.5}) {
            const QJsonObject strip{{"layer", "M1"}, {"shape", "rectangle"}, {"x", "-30[um]"}, {"y", QString::number(100 + gap) + "[um]"},
                                    {"width", "60[um]"}, {"height", "2[um]"}};
            const rf::Mesh mesh = meshOf(50, 30, {strip}, &processed, &stack);
            QVERIFY2(mesh.ok() && says(mesh.lines, "meshed in the frame of its feeds"), qPrintable(mesh.lines.join(" | ")));
            const QString all = mesh.lines.join(" | ");
            QCOMPARE(int(QSet<QString>(mesh.lines.cbegin(), mesh.lines.cend()).size()), int(mesh.lines.size()));
            QCOMPARE(int(QSet<QString>(mesh.warnings.cbegin(), mesh.warnings.cend()).size()), int(mesh.warnings.size()));
            QVERIFY2(all.count(QStringLiteral("beside gaps")) == (gap < 10 ? 1 : 0), qPrintable(QStringLiteral("%1 µm past: %2").arg(gap).arg(all)));
        }
        // Fed square, a stub at a slant.
        {
            const double b = 120 * M_PI / 180;
            const QPointF u(std::cos(b), std::sin(b)), v(-std::sin(b), std::cos(b));
            QJsonArray rows;
            for (const QPointF& p : {95 * u + 10 * v, 110 * u + 10 * v, 110 * u - 10 * v, 95 * u - 10 * v})
                rows.append(QJsonArray{QString::number(p.x(), 'g', 12) + "[um]", QString::number(p.y(), 'g', 12) + "[um]"});
            const rf::Mesh mesh = meshOf(0, 0, {QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}}}, &processed, &stack);
            QVERIFY2(mesh.ok() && !says(mesh.lines, "meshed in the frame of its feeds"), qPrintable(mesh.lines.join(" | ")));
            QVERIFY2(std::abs(double(mesh.cells.size()) / 1173 - 1) < 0.1, qPrintable(QStringLiteral("%1 cells").arg(mesh.cells.size())));
        }
        // The disc's C, fed square and turned.
        QMap<double, double> c;
        for (const double degrees : {0.0, 30.0}) {
            const rf::Mesh mesh = meshOf(0, degrees, {}, &processed, &stack);
            const rf::Extraction e = rf::extract(mesh, processed.conductors, stack);
            QVERIFY2(e.ok() && e.maxwell.size() == 1, qPrintable(e.problems.join(" | ")));
            c.insert(degrees, e.maxwell.at(0).at(0));
        }
        QVERIFY2(c.value(30) > 0 && std::abs(c.value(0) / c.value(30) - 1) < 0.005,
                 qPrintable(QStringLiteral("the disc's C fed at 0 degrees %1 fF, turned %2").arg(c.value(0) * 1e15).arg(c.value(30) * 1e15)));
    }

    // A circle or a ring joined by a feed is meshed whole, the feed at 0, 30,
    // 45 or 90 degrees, as the tab meshes them: no side held by three cells or
    // twice by one - which the solve leaves out as none's -, and the cells one
    // piece, joined side to side as the solve joins them (by their sides'
    // middles, to a nanometre). Parts of the curve's cap a few nanometres deep
    // had cells under a nanometre wide, which the snap to a nanometre left
    // with no width: 300 sides "could not be told apart", and the loop and
    // the full wave found the disc in pieces, an open circuit.
    // circleOf()'s side counts, odd at these radii
    // (703 and 497), put no corner where the feed meets the curve.
    //
    // Each in 1,140 to 7,600 cells, and the ring fed at 90 degrees meshed at
    // 5 µm in 1,355. Its slivers meshed by their own depth made some 37,000;
    // then its bands, each graded at every side and each a grid from its own
    // end, every one with all their lines, 20,000 to 39,000; then its bands on
    // one lattice, 3,300 to 7,600. Fed at 0 or 90 degrees, a curve's parts are
    // now one region (1,140 to 2,270); fed at 30 or 45, it is meshed in the
    // frame its feeds run along, one region there too (1,140 to 2,330) - in
    // the layout's own its feeds' slanted sides kept its bands, their cells
    // clipped by them, and its R read 1.3 to 1.6 % under the ring fed
    // square, converging from below (the review's P63). And the loop's R at
    // direct current, fed at 90 degrees, in squares of the metal within half
    // a per cent of a finite-difference solve of each sheet: the circle's
    // 7.4084, the ring's 8.2696 (7.4303 and 8.2649 here); fed at 0, 30 or 45
    // degrees, the same shape turned, within half a per cent of that.
    void aCurveJoinedByAFeedIsMeshedWhole()
    {
        // (A strip at 30 degrees alone, its ends' stubs as wide as its cells
        //  are across: taken by their boxes, three times that, it read 14.90
        //  squares of its 15.)
        {
            rf::Model m = stacked();
            m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes")));
            const QString shapes = m.layout().child(QStringLiteral("rf_shapes"))->tag;
            const double a = 30 * M_PI / 180;
            const QPointF along(std::cos(a), std::sin(a)), across(-std::sin(a), std::cos(a));
            QJsonArray rows;
            for (const QPointF& p : {-150 * along + 10 * across, 150 * along + 10 * across, 150 * along - 10 * across, -150 * along - 10 * across})
                rows.append(QJsonArray{QString::number(p.x(), 'g', 12) + "[um]", QString::number(p.y(), 'g', 12) + "[um]"});
            rf::Node strip = m.make(QStringLiteral("rf_shape"));
            strip.props = QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
            m.add(shapes, strip);
            m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"1", "0", "M1"});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            const rf::Stack stack = m.solveStack();
            const rf::Layer* metal = stack.named(QStringLiteral("M1"));
            QVERIFY(metal != nullptr);
            const rf::Inductance loop =
                rf::inductanceOf(rf::meshOf(processed.conductors, rf::MeshSettings(), stack, 1e9), processed.conductors, stack, 150 * along, -150 * along);
            QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
            const double squares = loop.rDc * metal->sigma * metal->thickness;
            QVERIFY2(std::abs(squares / 15 - 1) < 1e-3, qPrintable(QStringLiteral("a strip at 30 degrees: %1 squares of 15").arg(squares)));
        }
        QMap<QString, double> squaresOf;
        for (const bool ring : {false, true})
            for (const double degrees : {0.0, 30.0, 45.0, 90.0, -90.0}) {
                // (-90: the ring fed at 90 degrees meshed with no cells across
                //  and 5 µm cells at most.)
                const bool coarse = degrees < 0;
                if (coarse && !ring) continue;
                rf::Model m = stacked();
                m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes")));
                const QString shapes = m.layout().child(QStringLiteral("rf_shapes"))->tag;
                rf::Node round = m.make(QStringLiteral("rf_shape"));
                round.props = ring ? QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}, {"inner_radius", "50[um]"}}
                                   : QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}};
                m.add(shapes, round);
                // Two feeds 20 µm wide from 95 to 150 µm out, either side.
                const double a = std::abs(degrees) * M_PI / 180;
                const QPointF along(std::cos(a), std::sin(a)), across(-std::sin(a), std::cos(a));
                for (const double side : {1.0, -1.0}) {
                    QJsonArray rows;
                    for (const QPointF& p : {side * 95 * along + 10 * across, side * 150 * along + 10 * across, side * 150 * along - 10 * across,
                                             side * 95 * along - 10 * across})
                        rows.append(QJsonArray{QString::number(p.x(), 'g', 12) + "[um]", QString::number(p.y(), 'g', 12) + "[um]"});
                    rf::Node feed = m.make(QStringLiteral("rf_shape"));
                    feed.props = QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
                    m.add(shapes, feed);
                }
                m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"1", "0", "M1"});
                qucs_s::layout::Layout empty;
                qucs_s::layout::Cell top;
                top.name = QStringLiteral("TOP");
                empty.cells << top;
                empty.byName.insert(top.name, 0);
                const rf::Processed processed = rf::processRead(m, empty, QStringLiteral("none"));
                QCOMPARE(int(processed.conductors.size()), 1);
                rf::MeshSettings how;
                if (coarse) {
                    how.cellsAcross = 0;
                    how.maximum = 5e-6;
                }
                const rf::Mesh mesh = rf::meshOf(processed.conductors, how, m.solveStack(), 1e9);
                QVERIFY(!mesh.cells.isEmpty());
                const QString what = QStringLiteral("%1 fed at %2 degrees").arg(ring ? "a ring" : "a circle").arg(degrees);
                QHash<QString, QList<int>> sides;
                for (int c = 0; c < mesh.cells.size(); ++c) {
                    const QPolygonF& o = mesh.cells.at(c).outline;
                    for (int i = 0; i < o.size(); ++i) {
                        const QPointF mid = (o.at(i) + o.at((i + 1) % o.size())) / 2;
                        sides[QStringLiteral("%1,%2").arg(std::llround(mid.x() * 1e3)).arg(std::llround(mid.y() * 1e3))] << c;
                    }
                }
                QVector<int> parent(mesh.cells.size());
                std::iota(parent.begin(), parent.end(), 0);
                std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
                int clashes = 0;
                for (auto it = sides.cbegin(); it != sides.cend(); ++it) {
                    if (it.value().size() > 2 || (it.value().size() == 2 && it.value().at(0) == it.value().at(1))) ++clashes;
                    else if (it.value().size() == 2) parent[find(it.value().at(0))] = find(it.value().at(1));
                }
                QSet<int> pieces;
                for (int c = 0; c < mesh.cells.size(); ++c) pieces.insert(find(c));
                QVERIFY2(clashes == 0, qPrintable(QStringLiteral("%1: %2 sides held by three cells or twice by one").arg(what).arg(clashes)));
                QVERIFY2(pieces.size() == 1, qPrintable(QStringLiteral("%1: %2 pieces").arg(what).arg(pieces.size())));
                // (And not meshed as finely as its slivers would have it: a
                //  part of the cap meshed by its own depth, its lines taken by
                //  its neighbours.)
                // (Each within a tenth of its own count, either way: past it a
                //  band sized across its slanted sides, the lattice from the
                //  largest cell, the bands each a grid of their own; under it
                //  a curve's edges not graded.)
                const QMap<QString, int> counts{{"a circle fed at 0 degrees", 1145},  {"a circle fed at 30 degrees", 1385},
                                                {"a circle fed at 45 degrees", 1144}, {"a circle fed at 90 degrees", 1144},
                                                {"a ring fed at 0 degrees", 2222},    {"a ring fed at 30 degrees", 2330},
                                                {"a ring fed at 45 degrees", 2243},   {"a ring fed at 90 degrees", 2265},
                                                {"a ring fed at -90 degrees", 1355}};
                const double ratio = double(mesh.cells.size()) / counts.value(what);
                QVERIFY2(ratio > 0.9 && ratio < 1.1, qPrintable(QStringLiteral("%1: %2 cells, against %3").arg(what).arg(mesh.cells.size()).arg(counts.value(what))));
                // (Fed at an angle off the axes: meshed in its feeds' frame,
                //  every cell of it turned, and said.)
                if (degrees == 30.0 || degrees == 45.0) {
                    QVERIFY2(says(mesh.lines, "meshed in the frame of its feeds"), qPrintable(what + ": " + mesh.lines.join(" | ")));
                    QVERIFY2(std::all_of(mesh.cells.cbegin(), mesh.cells.cend(), [](const rf::MeshCell& cell) { return cell.angle != 0; }),
                             qPrintable(what));
                }
                if (!coarse) {
                    const rf::Stack stack = m.solveStack();
                    const rf::Layer* metal = stack.named(QStringLiteral("M1"));
                    QVERIFY(metal != nullptr);
                    const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, 150 * along, -150 * along);
                    QVERIFY2(loop.ok(), qPrintable(what + ": " + loop.problems.join(" | ")));
                    squaresOf.insert(what, loop.rDc * metal->sigma * metal->thickness);
                }
                if (degrees == 90.0) {
                    const rf::Stack stack = m.solveStack();
                    const rf::Layer* metal = stack.named(QStringLiteral("M1"));
                    QVERIFY(metal != nullptr);
                    const double square = 1 / (metal->sigma * metal->thickness);
                    const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, 150 * along, -150 * along);
                    QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
                    const double squares = loop.rDc / square, expected = ring ? 8.2696 : 7.4084;
                    QVERIFY2(std::abs(squares / expected - 1) < 0.005,
                             qPrintable(QStringLiteral("%1: %2 squares against %3").arg(what).arg(squares).arg(expected)));
                    // (Every cell off the edge a rectangle: its pieces - the
                    //  region and its feeds - meet on lines they share, no
                    //  cell's side split by another's corners where they meet.)
                    {
                        int cornered = 0;
                        for (const rf::MeshCell& cell : mesh.cells) {
                            QPolygonF o = cell.outline;
                            if (o.size() > 1 && o.first() == o.last()) o.removeLast();
                            if (!cell.onEdge && o.size() > 4) ++cornered;
                        }
                        QVERIFY2(cornered == 0, qPrintable(QStringLiteral("%1: %2 cells off the edge with sides split").arg(what).arg(cornered)));
                    }
                    // (And the ring at 20 across, within 0.3 %: its region met
                    //  a feed along a nanometre, the union's corners left on its
                    //  grid, or met the rectangle it was cut into at its slit on
                    //  lines of its own, and read 0.85 % over.)
                    if (ring) {
                        rf::MeshSettings finer;
                        finer.cellsAcross = 20;
                        const rf::Mesh fine = rf::meshOf(processed.conductors, finer, stack, 1e9);
                        const rf::Inductance at20 = rf::inductanceOf(fine, processed.conductors, stack, 150 * along, -150 * along);
                        QVERIFY2(at20.ok(), qPrintable(at20.problems.join(" | ")));
                        QVERIFY2(std::abs(at20.rDc / square / expected - 1) < 0.003,
                                 qPrintable(QStringLiteral("%1 at 20 across: %2 squares against %3").arg(what).arg(at20.rDc / square).arg(expected)));
                    }
                }
            }
        // The same shape turned, the same R: within half a per cent of it fed
        // at 90 degrees.
        for (const char* shape : {"a circle", "a ring"})
            for (const double degrees : {0.0, 30.0, 45.0}) {
                const QString what = QStringLiteral("%1 fed at %2 degrees").arg(QLatin1String(shape)).arg(degrees);
                const double square = squaresOf.value(QStringLiteral("%1 fed at 90 degrees").arg(QLatin1String(shape)));
                QVERIFY2(square > 0 && std::abs(squaresOf.value(what) / square - 1) < 0.005,
                         qPrintable(QStringLiteral("%1: %2 squares against %3 fed at 90").arg(what).arg(squaresOf.value(what)).arg(square)));
            }
    }


    // A ring 1 µm wide, and one 0.1 µm wide, meshed whole: no side held by
    // three cells or twice by one, one piece. A triangle 1 nm wide, cut where
    // a curve's line ran a hair inside its slanted side, had the middles of
    // its slanted side and its upright one under a nanometre apart - one side
    // as the solve pairs them, held twice by it: two such on the ring 1 µm
    // wide, 40 and the mesh in four pieces on the one 0.1 µm wide.
    void aThinRingIsMeshedWhole()
    {
        for (const double inner : {99.0, 99.9}) {
            const rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"},
                                                    {"inner_radius", QString::number(inner) + "[um]"}}});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            QCOMPARE(int(processed.conductors.size()), 1);
            const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), m.solveStack(), 1e9);
            QVERIFY(!mesh.cells.isEmpty());
            const auto [clashes, pieces] = clashesAndPieces(mesh);
            QVERIFY2(clashes == 0 && pieces == 1, qPrintable(QStringLiteral("inner radius %1: %2 clashes, %3 pieces").arg(inner).arg(clashes).arg(pieces)));
        }
    }

    // A shape reaching a metre or more from the origin is left out, said - a
    // rectangle 1e308 µm tall, which is finite, was past the boolean
    // operations' range, a ClipperLib::clipperException that nothing caught,
    // and Qucs-S ended on reading the model. The shapes beside it are kept.
    void aShapeFarPastAnyLayoutIsLeftOut()
    {
        const rf::Model m = shaped({boxOn("M1", 0, 0, 100, 1e308), boxOn("M1", 0, -50, 100, -10), boxOn("M1", 5e5, 0, 1.5e6, 10)});
        rf::Processed processed;
        try {
            processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
        } catch (...) {
            QFAIL("processing threw");
        }
        // (Not a problem standing in the way of the solve, which goes on
        //  without them, but said with its answer - it was in none.)
        QVERIFY2(processed.leftOut.join(' ').contains(QStringLiteral("M1: 2 shape(s) reach a metre or more from the origin")),
                 qPrintable(processed.leftOut.join(" | ")));
        QVERIFY2(processed.ok(), qPrintable(processed.problems.join(" | ")));
        QCOMPARE(int(processed.conductors.size()), 1);
        {
            rf::Model fed = shaped({boxOn("M1", 0, 0, 200, 20), boxOn("M1", 0, 0, 100, 1e308)});
            rf::Node* ports = nullptr;
            for (rf::Node& n : fed.root.children)
                if (n.type == QStringLiteral("rf_ports")) ports = &n;
            QVERIFY(ports != nullptr);
            ports->children.clear();
            for (const auto& [name, x] : {std::make_pair("P1", "0"), std::make_pair("P2", "200[um]")}) {
                rf::Node p = fed.make(QStringLiteral("rf_edge_port"));
                p.label = QLatin1String(name);
                p.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
                p.props.insert(QStringLiteral("x"), QLatin1String(x));
                p.props.insert(QStringLiteral("y"), QStringLiteral("10[um]"));
                ports->children.push_back(p);
            }
            rf::SolveJob job;
            rf::SolveRequest request;
            request.solver = QStringLiteral("quasi_static");
            request.loopOnly = true;
            const rf::ModelSolution sol = rf::solveModel(job, fed, nothingRead(), QStringLiteral("none"), QDir::tempPath(), request);
            QVERIFY2(sol.inductance && sol.inductance->ok(), qPrintable(sol.problem));
            // (The strip alone: ten squares.)
            const rf::Stack stack = fed.solveStack();
            const rf::Layer* metal = stack.named(QStringLiteral("M1"));
            QVERIFY(metal != nullptr);
            const double squares = sol.inductance->rDc * metal->sigma * metal->thickness;
            QVERIFY2(std::abs(squares / 10 - 1) < 0.01, qPrintable(QString::number(squares)));
            QVERIFY2(sol.inductance->lines.join(' ').count(QStringLiteral("M1: 1 shape(s) reach a metre or more")) == 1,
                     qPrintable(sol.inductance->lines.join(" | ")));
            // The extraction's: among its warnings, and not its lines - the
            // processing's lines say it, listed beside them by rf25_solve and
            // the tab, where it was said twice.
            rf::SolveJob both;
            request.loopOnly = false;
            const rf::ModelSolution whole = rf::solveModel(both, fed, nothingRead(), QStringLiteral("none"), QDir::tempPath(), request);
            QVERIFY2(whole.extraction && whole.extraction->ok(), qPrintable(whole.problem));
            QVERIFY2(whole.extraction->warnings.join(' ').count(QStringLiteral("M1: 1 shape(s) reach a metre or more")) == 1,
                     qPrintable(whole.extraction->warnings.join(" | ")));
            QVERIFY2(!whole.extraction->lines.join(' ').contains(QStringLiteral("reach a metre or more")),
                     qPrintable(whole.extraction->lines.join(" | ")));
            QVERIFY2(whole.processed && whole.processed->report.lines.join(' ').count(QStringLiteral("M1: 1 shape(s) reach a metre or more")) == 1,
                     qPrintable(whole.processed ? whole.processed->report.lines.join(" | ") : QString()));
        }
        // (The boolean operations themselves fail, and do not throw, past
        //  their range: a union and a clip of such a shape.)
        const QPolygonF huge(QRectF(0, 0, 100, 1e17));
        try {
            QCOMPARE(rf::unionOf({huge, QPolygonF(QRectF(0, 0, 10, 10))}).size(), 2);
            QVERIFY(rf::clipTo({huge}, QPolygonF(QRectF(0, 0, 10, 10))).isEmpty());
        } catch (...) {
            QFAIL("a boolean operation threw");
        }
    }

    // A loop's ends drawn on a circle's edge, and on a ring's outer edge,
    // are taken on the outline as processed, and said: the processing drops
    // a curve's corners within its tolerance of the line between their
    // neighbours, which puts its edge up to that inside the circle drawn,
    // and an end at (100, 0) on a disc of 100 µm was "on no conductor".
    void anEndOnACurvesEdgeIsTakenOnIt()
    {
        for (const bool ring : {false, true}) {
            rf::Model m = shaped({ring ? QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}, {"inner_radius", "50[um]"}}
                                       : QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}}});
            rf::Node* ports = nullptr;
            for (rf::Node& n : m.root.children)
                if (n.type == QStringLiteral("rf_ports")) ports = &n;
            QVERIFY(ports != nullptr);
            ports->children.clear();
            for (const auto& [name, x, y] : {std::make_tuple("P1", "100[um]", "0"), std::make_tuple("P2", "-70.7106781187[um]", "-70.7106781187[um]")}) {
                rf::Node p = m.make(QStringLiteral("rf_edge_port"));
                p.label = QLatin1String(name);
                p.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
                p.props.insert(QStringLiteral("x"), QLatin1String(x));
                p.props.insert(QStringLiteral("y"), QLatin1String(y));
                ports->children.push_back(p);
            }
            rf::SolveJob job;
            rf::SolveRequest request;
            request.solver = QStringLiteral("quasi_static");
            request.loopOnly = true;
            const rf::ModelSolution sol = rf::solveModel(job, m, nothingRead(), QStringLiteral("none"), QDir::tempPath(), request);
            const QString what = ring ? QStringLiteral("the ring") : QStringLiteral("the disc");
            QVERIFY2(sol.inductance && sol.inductance->ok(),
                     qPrintable(what + ": " + sol.problem + " | " + (sol.inductance ? sol.inductance->problems.join(" | ") : QString())));
            QVERIFY2(sol.inductance->rDc > 0 && std::isfinite(sol.inductance->rDc), qPrintable(what));
            QVERIFY2(sol.notes.join(' ').contains(QStringLiteral("taken on it")), qPrintable(what + ": " + sol.notes.join(" | ")));
        }
    }

    // A disc on M1 under the same disc on M2, 40 nm apart, is not said to
    // have cells that cannot be aligned: the two are one outline cut at the
    // same lines, cell for cell. Said so, its C read 0.2 % from the plates'
    // eps A / d, and it was told to take cells under 0.4 µm - some 60,000 on a
    // disc of 100 µm, past any solve. A smaller disc over it, off its centre,
    // still is.
    void aRoundCapacitorIsNotSaidMisaligned()
    {
        for (const bool alike : {true, false}) {
            rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "50[um]"}},
                                  alike ? QJsonObject{{"layer", "M2"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "50[um]"}}
                                        : QJsonObject{{"layer", "M2"}, {"shape", "circle"}, {"x", "7[um]"}, {"y", "3[um]"}, {"radius", "30[um]"}}},
                                 QJsonArray() << QJsonArray{"1", "0", "M1"} << QJsonArray{"2", "0", "M2"});
            for (rf::Node& n : m.stackNode().children)
                if (n.label == QLatin1String("ILD2")) n.props.insert(QStringLiteral("thickness"), QStringLiteral("0.04[um]"));
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            QCOMPARE(int(processed.conductors.size()), 2);
            const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), m.solveStack(), 1e9);
            const bool said = mesh.warnings.join(' ').contains(QStringLiteral("cannot be aligned"));
            QVERIFY2(said == !alike, qPrintable(mesh.warnings.join(" | ")));
        }
    }

    // A loop from a strip on M1 to a strip on M2, through a via where they
    // overlap, square or round, is solved: a via's run stands at the via, its
    // length its height, and the two cells it joins may be a cell apart in
    // the plane - counted as its height alone, the runs went from end to end
    // in 391 µm of 400 and the loop was refused as made wrong. A round via
    // and a square one of its area agree within a per cent. And a via drawn
    // off both strips joins nothing, said: taken however far, one 30 µm off
    // joined them, and the loop read 0.84 Ohm.
    void aLoopThroughAViaIsSolved()
    {
        const double r = 8, side = r * std::sqrt(M_PI);
        QList<double> rs;
        for (const QString kind : {QStringLiteral("square"), QStringLiteral("round"), QStringLiteral("away")}) {
            const QJsonObject via = kind == QLatin1String("square") ? boxOn("V1", -side / 2, -side / 2, side / 2, side / 2)
                                                                    : QJsonObject{{"layer", "V1"}, {"shape", "circle"}, {"x", "0"},
                                                                                  {"y", kind == QLatin1String("away") ? "40[um]" : "0"}, {"radius", "8[um]"}};
            const rf::Model m = shaped({boxOn("M1", -200, -10, 10, 10), boxOn("M2", -10, -10, 200, 10), via},
                                       QJsonArray() << QJsonArray{"1", "0", "M1"} << QJsonArray{"2", "0", "M2"} << QJsonArray{"3", "0", "V1"});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            const rf::Stack stack = m.solveStack();
            const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), stack, 1e9);
            const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, QPointF(-200, 0), QPointF(200, 0), 1e-6, 0, nullptr,
                                                         QStringLiteral("M1"), QStringLiteral("M2"));
            if (kind == QLatin1String("away")) {
                QVERIFY2(!loop.ok() && loop.problems.join(' ').contains(QStringLiteral("drawn off the pads of their two metals")),
                         qPrintable(loop.problems.join(" | ")));
                continue;
            }
            QVERIFY2(loop.ok() && loop.rDc > 0 && loop.lDc > 0, qPrintable(kind + ": " + loop.problems.join(" | ")));
            rs << loop.rDc;
        }
        QVERIFY2(std::abs(rs.at(1) / rs.at(0) - 1) < 0.01, qPrintable(QStringLiteral("%1 against %2 Ohm").arg(rs.at(1)).arg(rs.at(0))));
    }

    // Two pieces' lines shared (rf::shareLines()), its lists halved, come out
    // as they did when walked whole (walkedShareLines()). Halved with the
    // first line at or past x - tol asked whether it was at most x + tol, a
    // line one tolerance from one kept - two lines a nanometre apart on a
    // nanometre's grid - stayed on its own piece and never reached the other:
    // {4, 4.001} with nothing, merging at 0.01, gave {4, 4.001} and {4}, where
    // the walk gave {4} and {4} (the review of 4ce22e87). And 20,000 random
    // pairs of grids holding such pairs, each shared as meshOf() shares them,
    // in rounds until neither gains.
    void sharedLinesComeOutAsWalked()
    {
        {
            QList<double> mine{4, 4.001}, theirs;
            rf::shareLines(mine, theirs, 0, 100, 0.001, false, 0.01);
            QCOMPARE(mine, QList<double>{4});
            QCOMPARE(theirs, QList<double>{4});
        }
        std::mt19937 random(11);
        int apart = 0;
        for (int k = 0; k < 20000; ++k) {
            const double tol = 0.001;
            QList<double> mine, theirs;
            for (QList<double>* lines : {&mine, &theirs}) {
                const int n = 1 + int(random() % 12);
                for (int i = 0; i < n; ++i) {
                    const double x = double(random() % 100000) * tol;
                    *lines << x;
                    if (random() % 3 == 0) *lines << x + tol;
                }
                std::sort(lines->begin(), lines->end());
                lines->erase(std::unique(lines->begin(), lines->end()), lines->end());
            }
            const double lo = double(random() % 30000) * tol, hi = lo + double(random() % 70000) * tol + 1;
            const bool ends = random() % 2 == 0;
            const double merge = random() % 2 == 0 ? 0.0 : 0.01;
            QList<double> walkedMine = mine, walkedTheirs = theirs;
            for (int round = 0; round < 8 && rf::shareLines(mine, theirs, lo, hi, tol, ends, merge); ++round) {
            }
            for (int round = 0; round < 8 && walkedShareLines(walkedMine, walkedTheirs, lo, hi, tol, ends, merge); ++round) {
            }
            if (mine != walkedMine || theirs != walkedTheirs) ++apart;
        }
        QVERIFY2(apart == 0, qPrintable(QStringLiteral("%1 of 20,000 apart from the walk's").arg(apart)));
    }

    // A disc on M1 under the same disc on M2, 40 nm apart, is meshed as one
    // region each - in fewer than 1,000 cells, its C within half a per cent of
    // the plates' eps A / d (0.17 % under) -, where its 80 bands each were
    // 5,562 cells and 40 s to solve. And a ring 1 µm wide of 200 µm, filling a
    // fiftieth of its box, is one region too: only the squares its outline
    // crosses are cut. It kept its bands, their lines shared from band to
    // band: as a region every square of its box was clipped, 40,000 for
    // 1,462 cells, and one 0.1 µm wide was refused at one cell across.
    void aRoundShapeIsMeshedAsOneRegion()
    {
        rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "50[um]"}},
                              QJsonObject{{"layer", "M2"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "50[um]"}}},
                             QJsonArray() << QJsonArray{"1", "0", "M1"} << QJsonArray{"2", "0", "M2"});
        for (rf::Node& n : m.stackNode().children)
            if (n.label == QLatin1String("ILD2")) n.props.insert(QStringLiteral("thickness"), QStringLiteral("0.04[um]"));
        const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
        const rf::Stack stack = m.solveStack();
        const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), stack, 1e9);
        QVERIFY2(mesh.ok() && mesh.cells.size() < 1000, qPrintable(QStringLiteral("%1 cells").arg(mesh.cells.size())));
        QVERIFY2(mesh.lines.join(' ').contains(QStringLiteral("2 shape(s) with a curve meshed as one region each")), qPrintable(mesh.lines.join(" | ")));
        const rf::Extraction e = rf::extract(mesh, processed.conductors, stack);
        QVERIFY2(e.ok() && e.maxwell.size() == 2, qPrintable(e.problems.join(" | ")));
        double epsr = 0;
        for (const rf::Layer& l : stack.layers)
            if (l.name == QLatin1String("ILD2")) epsr = l.epsr;
        const double plates = 8.8541878128e-12 * epsr * M_PI * 50e-6 * 50e-6 / 0.04e-6;
        QVERIFY2(std::abs(-e.maxwell.at(0).at(1) / plates - 1) < 0.005, qPrintable(QStringLiteral("%1 against %2 F").arg(-e.maxwell.at(0).at(1)).arg(plates)));
        const rf::Model thin = shaped({QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}, {"inner_radius", "99[um]"}}});
        const rf::Processed ring = rf::processRead(thin, nothingRead(), QStringLiteral("none"));
        const rf::Mesh bands = rf::meshOf(ring.conductors, rf::MeshSettings(), thin.solveStack(), 1e9);
        QVERIFY2(bands.ok() && bands.lines.join(' ').contains(QStringLiteral("meshed as one region")), qPrintable(bands.lines.join(" | ")));
        // (4,185 cells, where its bands were 5,935.)
        QVERIFY2(bands.cells.size() < 4500, qPrintable(QStringLiteral("%1 cells").arg(bands.cells.size())));
        // Fed at 30 degrees, a strip 10 µm wide on its outer edge, it is meshed
        // in the frame its feed runs along, one region there: 7,845 cells,
        // within a tenth of the 7,874 it is fed at 0 degrees. (In the layout's
        // own frame its feed's slanted sides kept its bands, 4,271 cells
        // clipped by them; a band sized across its slanted sides, not by the
        // ring's chord, made 5,265; its pieces graded at every side of their
        // boxes, 7,474.)
        const double a = 30 * M_PI / 180;
        const QPointF along(std::cos(a), std::sin(a)), side(-5 * std::sin(a), 5 * std::cos(a));
        QJsonArray feed;
        for (const QPointF& p : {99.5 * along - side, 160 * along - side, 160 * along + side, 99.5 * along + side})
            feed.append(QJsonArray{QString::number(p.x(), 'f', 6) + "[um]", QString::number(p.y(), 'f', 6) + "[um]"});
        const rf::Model fed = shaped({QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}, {"inner_radius", "99[um]"}},
                                      QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", feed}}});
        const rf::Mesh banded = rf::meshOf(rf::processRead(fed, nothingRead(), QStringLiteral("none")).conductors, rf::MeshSettings(), fed.solveStack(), 1e9);
        QVERIFY2(banded.ok() && banded.lines.join(' ').contains(QStringLiteral("meshed in the frame of its feeds")), qPrintable(banded.lines.join(" | ")));
        QVERIFY2(std::abs(double(banded.cells.size()) / 7874 - 1) < 0.1, qPrintable(QStringLiteral("%1 cells, fed at 30 degrees").arg(banded.cells.size())));
    }

    // A ring of 900 µm radius and 10 µm wide, slotted, at the mesh's
    // defaults: one region, under the solver's 8,000 cells at three across
    // or more, its R within 5 % of the sector's theta / ln(ro / ri) squares
    // (4.0 % under at three). Its bands, their lines shared from band to band
    // along its upright stretches, were 4,006 cells at one across and 8,117
    // at two: cut to one across, 8.3 % under.
    void aThinRingIsMeshedUnderTheSolversBound()
    {
        const double r = 900, w = 10;
        rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", QString::number(r + w / 2) + "[um]"},
                                          {"inner_radius", QString::number(r - w / 2) + "[um]"}}});
        region(m, "rectangle", {{"x0", QString::number(r - w) + "[um]"}, {"y0", QStringLiteral("-1[um]")}, {"x1", QString::number(r + w) + "[um]"},
                                {"y1", QStringLiteral("1[um]")}, {"action", QStringLiteral("remove")}});
        const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
        QCOMPARE(int(processed.conductors.size()), 1);
        const rf::Stack stack = m.solveStack();
        const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), stack, 1e9);
        QVERIFY2(mesh.ok() && mesh.lines.join(' ').contains(QStringLiteral("meshed as one region")), qPrintable(mesh.lines.join(" | ")));
        QVERIFY2(mesh.across >= 3 && mesh.cells.size() <= 8000, qPrintable(QStringLiteral("%1 across, %2 cells").arg(mesh.across).arg(mesh.cells.size())));
        const auto [clashes, pieces] = clashesAndPieces(mesh);
        QVERIFY2(clashes == 0 && pieces == 1, qPrintable(QStringLiteral("%1 clashes, %2 pieces").arg(clashes).arg(pieces)));
        const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, QPointF(r, 1), QPointF(r, -1));
        QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
        const rf::Layer* metal = stack.named(QStringLiteral("M1"));
        const double squares = (2 * M_PI - 2 * std::asin(1.0 / r)) / std::log((r + w / 2) / (r - w / 2));
        const double read = loop.rDc * metal->sigma * metal->thickness;
        QVERIFY2(std::abs(read / squares - 1) < 0.05, qPrintable(QStringLiteral("%1 squares against %2").arg(read).arg(squares)));
    }

    // A ring of 100 and 50 µm drawn as a file draws one - a keyhole polygon of
    // 2-degree sides, its slit at 0 degrees, its corners given to a micron's
    // millionth -, fed at 90 degrees by two strips 20.0008 µm wide (their
    // sides off the nanometre's grid), is one region: every cell
    // off its edge a rectangle, and its loop's R in squares of the metal
    // within 0.3 % of a finite-difference solve's 8.2697 at 10 and at 20
    // cells across. Its region's corners left on the union's nanometre grid,
    // half a nanometre off the feeds' ends, the two met along a nanometre and
    // shared no lines; and the rectangle its slit cut it into kept apart, it
    // read 0.85 % over at 20 across, and is a fifth more cells at 10. (At
    // 10 MHz, its cells set by the cells across alone.)
    void aKeyholeRingIsMeshedAsOneRegion()
    {
        QJsonArray rows;
        const auto at = [&rows](double r, double degrees) {
            const double a = degrees * M_PI / 180;
            rows.append(QJsonArray{QString::number(r * std::cos(a), 'f', 6) + "[um]", QString::number(r * std::sin(a), 'f', 6) + "[um]"});
        };
        for (int k = 0; k <= 180; ++k) at(100, 2.0 * k);
        for (int k = 180; k >= 0; --k) at(50, 2.0 * k);
        rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}}, boxOn("M1", -10.0004, 95, 10.0004, 150), boxOn("M1", -10.0004, -150, 10.0004, -95)});
        // (On 100 µm of eps_r 4 over a perfect ground, the metal 1 µm of
        //  copper solved as a sheet.)
        m.stackNode().children.clear();
        set(add(m.stackNode(), "rf_ground", QStringLiteral("g1"), QStringLiteral("Ground")), "kind", QStringLiteral("perfect"));
        rf::Node& sub = add(m.stackNode(), "rf_substrate", QStringLiteral("s1"), QStringLiteral("Sub"));
        set(sub, "thickness", QStringLiteral("100[um]"));
        set(sub, "epsr", QStringLiteral("4"));
        rf::Node& copper = add(m.stackNode(), "rf_metal", QStringLiteral("m1"), QStringLiteral("M1"));
        set(copper, "thickness", QStringLiteral("1[um]"));
        set(copper, "sigma", QStringLiteral("5.8e7"));
        set(copper, "model", QStringLiteral("sheet"));
        const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
        QCOMPARE(int(processed.conductors.size()), 1);
        const rf::Stack stack = m.solveStack();
        const rf::Layer* metal = stack.named(QStringLiteral("M1"));
        QVERIFY(metal != nullptr);
        const double square = 1 / (metal->sigma * metal->thickness);
        for (const int across : {10, 20}) {
            rf::MeshSettings how = rf::tabMeshSettings(m, nullptr);
            how.cellsAcross = across;
            const rf::Mesh mesh = rf::meshOf(processed.conductors, how, stack, 1e7);
            QVERIFY2(mesh.ok() && mesh.lines.join(' ').contains(QStringLiteral("meshed as one region")), qPrintable((mesh.problems + mesh.lines).join(" | ")));
            // (No cell's side along x or y split by a corner of the cell
            //  beside it - three corners in a row on one such side - but one,
            //  where a corner the cuts left on the curve meets the grid.)
            int split = 0;
            for (const rf::MeshCell& cell : mesh.cells) {
                QPolygonF o = cell.outline;
                if (o.size() > 1 && o.first() == o.last()) o.removeLast();
                const int n = int(o.size());
                for (int k = 0; k < n; ++k) {
                    const QPointF a = o.at((k + n - 1) % n), b = o.at(k), c = o.at((k + 1) % n);
                    if ((a.x() == b.x() && b.x() == c.x()) || (a.y() == b.y() && b.y() == c.y())) {
                        ++split;
                        break;
                    }
                }
            }
            QVERIFY2(split <= 1, qPrintable(QStringLiteral("%1 across: %2 cells with a side split").arg(across).arg(split)));
            // (2,241 cells at 10 across; with the rectangle beside its slit
            //  kept apart, 2,720 - its R now within 0.3 % either way.)
            if (across == 10) QVERIFY2(mesh.cells.size() < 2400, qPrintable(QStringLiteral("%1 cells").arg(mesh.cells.size())));
            const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, QPointF(0, 150), QPointF(0, -150));
            QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
            QVERIFY2(std::abs(loop.rDc / square / 8.2697 - 1) < 0.003,
                     qPrintable(QStringLiteral("%1 across: %2 squares against 8.2697").arg(across).arg(loop.rDc / square)));
        }
    }

    // A disc of 50 µm fed by two strips 20 µm wide, the strips slid across
    // it: every mesh one piece, no side held by three cells or twice by one.
    // A region's clip whose line ran a few nanometres from a curve's corner
    // left an edge cell's outline out along a line and straight back, its two
    // edges one middle with each other and with the cell beside: 23 of 1,501
    // placements so, where the bands had none (the review of 048ec228; x0 of
    // 3.4256, 3.5453, 3.6783, 4.1172 and 4.1305 among them). Every tenth of
    // the review's sweep, from 3 to 23 µm in steps of 13.3 nm, and those.
    void aFedDiscMeshesWithoutClashesWhereverItsFeedsAre()
    {
        QList<double> placements{3.4256, 3.5453, 3.6783, 4.1172, 4.1305, 13.3};
        for (int k = 0; k <= 1500; k += 10) placements << 3.0 + 0.0133 * k;
        double thinnest = 1e300;
        for (const double x0 : std::as_const(placements)) {
            const rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "50[um]"}},
                                        boxOn("M1", x0, 30, x0 + 20, 120), boxOn("M1", -x0 - 20, -120, -x0, -30)});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            QCOMPARE(int(processed.conductors.size()), 1);
            const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), m.solveStack(), 1e9);
            QVERIFY(!mesh.cells.isEmpty());
            const auto [clashes, pieces] = clashesAndPieces(mesh);
            QVERIFY2(clashes == 0 && pieces == 1, qPrintable(QStringLiteral("x0 %1: %2 clashes, %3 pieces").arg(x0).arg(clashes).arg(pieces)));
            // (Where the disc is one region: placed so that a feed's side runs
            //  into the curve at a slant, it keeps its bands.)
            if (mesh.lines.join(' ').contains(QStringLiteral("meshed as one region")))
            for (const rf::MeshCell& cell : mesh.cells) {
                // (The cells inside the disc, half a micron clear of its curve:
                //  a cell its cap cuts may be as thin as the cap.)
                bool inside = true;
                for (const QPointF& p : cell.outline) inside = inside && std::hypot(p.x(), p.y()) < 49.5;
                const QRectF box = cell.outline.boundingRect();
                if (inside) thinnest = std::min(thinnest, std::min(box.width(), box.height()));
            }
        }
        // (And no row or column of thin cells across the region where a
        //  feed sits off its lattice: the feed's own lines in, the region's
        //  lines out of their way, its graded lines a quarter of their step
        //  clear of those there. Added however near, a feed at 13.3 to 33.3 µm
        //  drew a row 70 nm high, one at 3 µm 13 nm.)
        QVERIFY2(thinnest >= 0.2, qPrintable(QStringLiteral("a cell inside the disc %1 µm across").arg(thinnest)));
    }

    // A plane 1 mm square with a round opening of 50 µm radius at its middle,
    // the opening taken out by the Cutout - processing writes it as a keyhole,
    // its slit joining the plane's left side at y = -0.3 µm -, driven by a
    // loop from its left side to its right: R within 0.3 % of Maxwell's
    // (1 + f) / (1 - f) squares for a sheet with a small hole of area share f,
    // 1.0158, at 10 and at 30 cells across. Taken as the edges beside its
    // own alone, a port's face was half that left side - the slit's two
    // edges between its two halves -, and R read 1.24 squares.
    void aPortsFaceIsTheWholeSidePastAKeyholesSlit()
    {
        rf::Model m = shaped({boxOn("M1", -500, -500, 500, 500)});
        region(m, "circle", {{"x", QStringLiteral("0")}, {"y", QStringLiteral("0")}, {"radius", QStringLiteral("50[um]")}, {"action", QStringLiteral("remove")}});
        const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
        QCOMPARE(int(processed.conductors.size()), 1);
        const rf::Stack stack = m.solveStack();
        const rf::Layer* metal = stack.named(QStringLiteral("M1"));
        QVERIFY(metal != nullptr);
        const double square = 1 / (metal->sigma * metal->thickness), f = M_PI * 50 * 50 / 1e6;
        for (const int across : {10, 30}) {
            rf::MeshSettings how;
            how.cellsAcross = across;
            const rf::Mesh mesh = rf::meshOf(processed.conductors, how, stack, 1e9);
            const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, QPointF(-500, 100), QPointF(500, 100));
            QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
            QVERIFY2(std::abs(loop.rDc / square / ((1 + f) / (1 - f)) - 1) < 0.003,
                     qPrintable(QStringLiteral("%1 across: %2 squares against %3").arg(across).arg(loop.rDc / square).arg((1 + f) / (1 - f))));
        }
        // (The side itself: from corner to corner past the slit.)
        QLineF edge;
        QVERIFY(rf::faceEdge(processed.conductors, QPointF(-500, 100), QStringLiteral("M1"), QPointF(), &edge));
        const QLineF side = rf::wholeSideOf(processed.conductors, edge, QStringLiteral("M1"));
        QVERIFY2(std::abs(side.length() - 1000) < 1e-6, qPrintable(QStringLiteral("a side %1 µm long").arg(side.length())));
    }

    // A disc of 30 µm radius inside a ring from 34 to 60 µm, 3 µm of metal: the
    // narrowest gap on its layer is between them, some 3.9 µm - as a sheet,
    // said to read low; Automatic, solved as its two faces. Their pieces'
    // boxes overlap, the disc's inside the ring's, and no gap was found: the
    // sheet not said to read low (its C 26 % under its two faces'), and
    // Automatic took it as one.
    void aGapInsideARingIsFound()
    {
        rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "60[um]"}, {"inner_radius", "34[um]"}},
                              QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "30[um]"}}});
        for (rf::Node& n : m.stackNode().children)
            if (n.label == QLatin1String("M1")) {
                n.props.insert(QStringLiteral("thickness"), QStringLiteral("3[um]"));
                n.props.insert(QStringLiteral("model"), QStringLiteral("sheet"));
            }
        const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
        QCOMPARE(int(processed.conductors.size()), 2);
        const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), m.solveStack(), 1e9);
        const double gap = mesh.gapByLayer.value(QStringLiteral("M1"), 0);
        QVERIFY2(gap > 3.5 && gap < 4.1, qPrintable(QStringLiteral("%1 µm").arg(gap)));
        QVERIFY2(mesh.warnings.join(' ').contains(QStringLiteral("beside gaps as narrow as")), qPrintable(mesh.warnings.join(" | ")));
        rf::Stack automatic = m.solveStack();
        for (rf::Layer& l : automatic.layers)
            if (l.name == QLatin1String("M1")) {
                l.twoSheets = false;
                l.automatic = true;
            }
        QVERIFY(rf::thickened(automatic, processed.conductors, mesh).any);
    }

    // An edge port drawn on a disc's edge, at (100, 0) on a disc of 100 µm:
    // written by the HFSS export onto the outline as processed - its curve
    // simplified, up to 0.1 µm inside the circle drawn - as the solve takes
    // it, and said. It was "not written", on no conductor.
    void anExportedPortOnACurvesEdgeIsWritten()
    {
        rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "100[um]"}}});
        rf::Node* ports = nullptr;
        for (rf::Node& n : m.root.children)
            if (n.type == QStringLiteral("rf_ports")) ports = &n;
        QVERIFY(ports != nullptr);
        ports->children.clear();
        for (const auto& [name, x] : {std::make_pair("P1", "100[um]"), std::make_pair("P2", "-100[um]")}) {
            rf::Node p = m.make(QStringLiteral("rf_edge_port"));
            p.label = QLatin1String(name);
            p.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
            p.props.insert(QStringLiteral("x"), QLatin1String(x));
            p.props.insert(QStringLiteral("y"), QStringLiteral("0"));
            ports->children.push_back(p);
        }
        const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
        QVERIFY(processed.report.simplifiedMost > 0);
        rf::HfssOptions how;
        how.portReach = processed.report.simplifiedMost + 0.002;
        const rf::HfssExport written = rf::hfssExportOf(m, processed.conductors, how);
        QVERIFY2(written.ports == 2 && written.notes.join(' ').contains(QStringLiteral("written on it")), qPrintable((written.problems + written.notes).join(" | ")));
        const rf::HfssExport unreached = rf::hfssExportOf(m, processed.conductors, rf::HfssOptions());
        QVERIFY2(unreached.ports < 2, qPrintable(unreached.notes.join(" | ")));
    }

    // A Touchstone file compared with a solve: its frequencies out of order
    // refused, said where - read between the wrong points, nothing said -;
    // a point whose values are no finite number ("nan", "inf") left out and
    // said, not compared as a nan dB.
    void aTouchstoneFilesBadPointsAreSaid()
    {
        rf::FullWave solved;
        solved.portNames = {QStringLiteral("P1"), QStringLiteral("P2")};
        for (int k = 0; k < 5; ++k) {
            solved.frequencies << 1e9 * (1 + k);
            solved.z << QList<QList<rf::Complex>>{{rf::Complex(50, 1), rf::Complex(10, 0)}, {rf::Complex(10, 0), rf::Complex(50, 1)}};
        }
        solved.references = {50, 50};
        QTemporaryDir dir;
        const auto write = [&dir](const QString& name, const QStringList& points) {
            const QString path = dir.filePath(name);
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.write(("# GHz S RI R 50\n" + points.join('\n') + '\n').toUtf8());
            return path;
        };
        QStringList points;
        for (int k = 1; k <= 5; ++k) points << QStringLiteral("%1 0.1 0.01 0.9 -0.1 0.9 -0.1 0.1 0.01").arg(k);
        QVERIFY2(rf::compareWithTouchstone(solved, write(QStringLiteral("good.s2p"), points)).problem.isEmpty(), "a good file refused");
        // (A one-port file's out of order: a two-port's past a lower frequency
        //  is its noise data, as the format has it, and read so.)
        {
            rf::FullWave one;
            one.portNames = {QStringLiteral("P1")};
            for (int k = 0; k < 5; ++k) {
                one.frequencies << 1e9 * (1 + k);
                one.z << QList<QList<rf::Complex>>{{rf::Complex(50, 1)}};
            }
            one.references = {50};
            const rf::SCompared out = rf::compareWithTouchstone(
                one, write(QStringLiteral("unsorted.s1p"), {QStringLiteral("1 0.1 0.01"), QStringLiteral("3 0.1 0.01"), QStringLiteral("2 0.1 0.01"),
                                                            QStringLiteral("4 0.1 0.01"), QStringLiteral("5 0.1 0.01")}));
            QVERIFY2(out.problem.contains(QStringLiteral("do not rise")), qPrintable(out.problem));
        }
        QStringList nan = points;
        nan[2] = QStringLiteral("3 0.1 0.01 nan -0.1 nan -0.1 0.1 0.01");
        const rf::SCompared some = rf::compareWithTouchstone(solved, write(QStringLiteral("nan.s2p"), nan));
        QVERIFY2(some.problem.isEmpty() && some.notes.join(' ').contains(QStringLiteral("no finite number")), qPrintable(some.problem + " | " + some.notes.join(" | ")));
        for (const auto& at : some.theirs)
            for (const auto& row : at)
                for (const rf::Complex& v : row) QVERIFY(std::isfinite(v.real()) && std::isfinite(v.imag()));
    }

    // A curve's lattice lines (rf::latticeLines()) all within their piece,
    // its two ends first and last, rising, the lattice's own a quarter of its
    // step clear of the ends: on pieces shorter than the step - a disc's cap
    // band 0.3 µm long, graded at both ends - the point of the half step
    // nearest the outermost space's middle was past the piece, a line at 50
    // for a piece from 49 to 49.3 (the review of 621f4a9c: 46 of 522 short
    // pieces near x = 49 to 51). And on longer ones, the lattice's points
    // inside, a graded end halved where the half step has a point in it.
    void aCurveLatticeStaysInItsPiece()
    {
        QCOMPARE(rf::latticeLines(49.0, 0.3, 0, 5, 5, true, true, 0.3, 0.3), (QList<double>{49.0, 49.3}));
        int checked = 0;
        for (const double base : {5.0, 6.25})
            for (const double length : {0.3, 0.7, 1.3, 2.9, 4.9, 12.0})
                for (int k = 0; k <= 40; ++k)
                    for (const double finest : {0.0, 0.3}) {
                        const double from = 48 + 0.1 * k, to = from + length;
                        const QList<double> lines = rf::latticeLines(from, length, 0, base, 5, true, true, finest, finest);
                        const QString what = QStringLiteral("%1 to %2, base %3, finest %4: %5").arg(from).arg(to).arg(base).arg(finest).arg(
                            [&lines] {
                                QStringList l;
                                for (const double x : lines) l << QString::number(x, 'g', 8);
                                return l.join(' ');
                            }());
                        QVERIFY2(lines.size() >= 2 && lines.first() == from && std::abs(lines.last() - to) < 1e-12, qPrintable(what));
                        for (int i = 1; i < lines.size(); ++i) QVERIFY2(lines.at(i) > lines.at(i - 1), qPrintable(what));
                        ++checked;
                    }
        QCOMPARE(checked, 2 * 6 * 41 * 2);
        // (From 1 to 24 on a lattice of 5, graded: 5, 10, 15 and 20 inside,
        //  each end's space halved on 2.5 - 2.5 and 22.5. From 3 to 23 its
        //  ends are left: 2.5's points nearest their middles are on 5, and a
        //  hair from 23.)
        QCOMPARE(rf::latticeLines(1, 23, 0, 5, 5, true, true), (QList<double>{1, 2.5, 5, 10, 15, 20, 22.5, 24}));
        QCOMPARE(rf::latticeLines(3, 20, 0, 5, 5, true, true), (QList<double>{3, 5, 10, 15, 20, 23}));
    }

    // A face is the cells with a side along its edge by more than the
    // tolerance, not those whose side only reaches one of its ends
    // (faceCells(), which the loop and the full wave's ports drive). Taken
    // where they touched an end:
    // - two squares meeting at a corner, a loop from the first's top edge
    //   at (5, 10) to the second solved through the second alone (3.7 pH,
    //   0.147 Ohm), its face holding the second's corner cell; the full
    //   wave's port there drove both, Z21 15 % off. As from (0, 5), the
    //   pieces that nothing joins are said. So a figure of eight from the
    //   corner beside its pinch;
    // - an L's inner edge from (12, 5) took the two cells past its corner
    //   at (5, 5): their stubs and the run between them a loop of no area,
    //   the dense LU singular at 10 across - a one-piece L in "pieces
    //   that nothing joins" - and R 0.0491 Ohm at 20, where it is 0.0574.
    //   Now 0.0587 at 10 and 0.0574 at 20; drawn as two boxes the same, and
    //   from the arm's inner edge too.
    void aFaceStopsAtItsEdgesEnds()
    {
        const auto poly = [](const QList<QPointF>& pts) {
            QJsonArray rows;
            for (const QPointF& p : pts) rows.append(QJsonArray{QString::number(p.x()) + "[um]", QString::number(p.y()) + "[um]"});
            return QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
        };
        const auto solved = [](const QList<QJsonObject>& shapes, QPointF from, QPointF to, int across, QList<int>* face = nullptr) {
            const rf::Model m = shaped(shapes);
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            const rf::Stack stack = m.solveStack();
            rf::MeshSettings how;
            how.cellsAcross = across;
            const rf::Mesh mesh = rf::meshOf(processed.conductors, how, stack, 1e9);
            if (face != nullptr) {
                QList<int> all(mesh.cells.size());
                std::iota(all.begin(), all.end(), 0);
                face->clear();
                for (const int c : rf::faceCells(mesh, processed.conductors, all, from)) *face << c;
                // (Each as its cell's centre, packed: x * 1000 + y, in µm.)
                for (int& c : *face) c = int(std::lround(mesh.cells.at(c).centre.x() * 10)) * 1000 + int(std::lround(mesh.cells.at(c).centre.y() * 10));
            }
            return rf::inductanceOf(mesh, processed.conductors, stack, from, to);
        };
        // Two squares at a corner, and a figure of eight beside its pinch.
        const QList<QJsonObject> squares{boxOn("M1", 0, 0, 10, 10), boxOn("M1", 10, 10, 20, 20)};
        QList<int> face;
        const rf::Inductance corner = solved(squares, QPointF(5, 10), QPointF(20, 15), 10, &face);
        QVERIFY2(!corner.ok() && corner.problems.join(' ').contains(QStringLiteral("pieces that nothing joins")), qPrintable(corner.problems.join(" | ")));
        for (const int c : std::as_const(face)) QVERIFY2(c / 1000 < 100, qPrintable(QStringLiteral("a face cell at x %1 µm").arg((c / 1000) / 10.0)));
        const rf::Inductance eight = solved({poly({{0, 0}, {20, 20}, {20, 0}, {0, 10}})}, QPointF(0, 10), QPointF(20, 10), 10);
        QVERIFY2(!eight.ok() && eight.problems.join(' ').contains(QStringLiteral("pieces that nothing joins")), qPrintable(eight.problems.join(" | ")));
        // An L's inner edges.
        const QList<QJsonObject> l{poly({{0, 0}, {20, 0}, {20, 5}, {5, 5}, {5, 20}, {0, 20}})};
        const rf::Inductance at10 = solved(l, QPointF(12, 5), QPointF(0, 12), 10, &face);
        for (const int c : std::as_const(face)) QVERIFY2(c / 1000 > 50, qPrintable(QStringLiteral("a face cell at x %1 µm").arg((c / 1000) / 10.0)));
        const rf::Inductance at20 = solved(l, QPointF(12, 5), QPointF(0, 12), 20);
        const rf::Inductance arm = solved(l, QPointF(5, 12), QPointF(12, 0), 20);
        const rf::Inductance boxes = solved({boxOn("M1", 0, 0, 20, 5), boxOn("M1", 0, 5, 5, 20)}, QPointF(12, 5), QPointF(0, 12), 20);
        for (const auto& [what, loop] : {std::pair{"at 10", &at10}, std::pair{"at 20", &at20}, std::pair{"from the arm", &arm}, std::pair{"as boxes", &boxes}})
            QVERIFY2(loop->ok(), qPrintable(QStringLiteral("%1: %2").arg(what, loop->problems.join(" | "))));
        QVERIFY2(std::abs(at10.rDc / at20.rDc - 1) < 0.03 && std::abs(at10.lDc / at20.lDc - 1) < 0.03,
                 qPrintable(QStringLiteral("%1 Ohm %2 pH at 10, %3 Ohm %4 pH at 20").arg(at10.rDc).arg(at10.lDc * 1e12).arg(at20.rDc).arg(at20.lDc * 1e12)));
        QVERIFY2(std::abs(at20.rDc / 0.0574 - 1) < 0.01, qPrintable(QString::number(at20.rDc)));
        QVERIFY2(std::abs(arm.rDc / at20.rDc - 1) < 1e-6 && std::abs(boxes.rDc / at20.rDc - 1) < 1e-6,
                 qPrintable(QStringLiteral("%1 and %2 against %3 Ohm").arg(arm.rDc).arg(boxes.rDc).arg(at20.rDc)));
    }

    // A loop's runs taken as bars by the narrowest conductor its current runs
    // on, not by every one of their layer: a triangle 5 µm tall drawn 500 µm
    // off a strip, joined to nothing, made each of the strip's runs a bar -
    // its metal thick against the triangle -, and the strip's loop read
    // 1.3 % low. A strip as narrow as the triangle is still solved as bars,
    // its stubs to the ports not.
    void aShapeOffTheLoopLeavesItsInductanceAlone()
    {
        const auto triangle = [](double y) {
            QJsonArray rows;
            for (const QPointF& p : {QPointF(0, y), QPointF(10, y), QPointF(5, y + 5)})
                rows.append(QJsonArray{QString::number(p.x()) + "[um]", QString::number(p.y()) + "[um]"});
            return QJsonObject{{"layer", "M2"}, {"shape", "polygon"}, {"points", rows}};
        };
        QList<double> ls;
        for (const double y : {-1.0, 500.0, 50.0, -2.0}) {
            const double half = y == -2.0 ? 1 : 10;
            QList<QJsonObject> shapes{boxOn("M2", -100, -half, 100, half)};
            if (y >= 0) shapes << triangle(y);
            const rf::Model m = shaped(shapes, QJsonArray() << QJsonArray{"1", "0", "M1"} << QJsonArray{"2", "0", "M2"});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            QCOMPARE(int(processed.conductors.size()), y < 0 ? 1 : 2);
            const rf::Stack stack = m.solveStack();
            const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), stack, 1e9);
            const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, QPointF(-100, 0), QPointF(100, 0));
            QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
            int bars = 0;
            for (const rf::Filament& f : loop.runs) bars += f.bar > 0 ? 1 : 0;
            QVERIFY2((bars > 0) == (y == -2.0), qPrintable(QStringLiteral("case %1: %2 bars").arg(y).arg(bars)));
            // (Every run of the narrow strip a bar but the stubs to its
            //  ports, strips as they were made.)
            if (y == -2.0) {
                const QRegularExpressionMatch stubs = QRegularExpression(QStringLiteral("by (\\d+) stub")).match(loop.lines.join(' '));
                QVERIFY2(stubs.hasMatch() && stubs.captured(1).toInt() > 0, qPrintable(loop.lines.join(" | ")));
                QCOMPARE(int(loop.runs.size()) - bars, stubs.captured(1).toInt());
            }
            ls << loop.lDc;
        }
        for (const int k : {1, 2})
            QVERIFY2(std::abs(ls.at(k) / ls.at(0) - 1) < 1e-9, qPrintable(QStringLiteral("%1 pH against %2").arg(ls.at(k) * 1e12).arg(ls.at(0) * 1e12)));
    }

    // A polygon crossing itself into two equal lobes - a figure of eight - is
    // kept, its lobes each a conductor: their shoelace areas cancel, and it
    // was left out as a shape of no area, its metal lost. Unequal lobes were
    // kept already; a polygon on one line is still of no area.
    void aFigureOfEightPolygonIsKept()
    {
        const auto polygon = [](const QList<QPointF>& points) {
            QJsonArray rows;
            for (const QPointF& p : points) rows.append(QJsonArray{QString::number(p.x()) + "[um]", QString::number(p.y()) + "[um]"});
            return QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
        };
        for (const bool equal : {true, false}) {
            const rf::Model m = shaped({polygon(equal ? QList<QPointF>{{0, 0}, {10, 10}, {10, 0}, {0, 10}} : QList<QPointF>{{0, 0}, {20, 20}, {20, 0}, {0, 10}})});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            QVERIFY2(!processed.problems.join(' ').contains(QStringLiteral("no area")), qPrintable(processed.problems.join(" | ")));
            QCOMPARE(int(processed.conductors.size()), 2);
            double area = 0;
            for (const rf::Conductor& c : processed.conductors) area += rf::areaOf(c.outline);
            // (The lobes of the unequal one meet at (20/3, 20/3): 33.3 µm² on
            //  the left, 133.3 on the right.)
            QVERIFY2(std::abs(area - (equal ? 50 : 500.0 / 3)) < 0.01, qPrintable(QString::number(area)));
        }
        const rf::Model flat = shaped({polygon({{0, 0}, {10, 0}, {20, 0}, {5, 0}})});
        QVERIFY(rf::processRead(flat, nothingRead(), QStringLiteral("none")).problems.join(' ').contains(QStringLiteral("no area")));
    }

    // A gear of 18 teeth 100 µm across (arcs of 50 and 40 µm radius, 10
    // degrees each, joined by its teeth's radial sides), fed by two strips,
    // is one region at the mesh's defaults: ten across, one piece, its loop's
    // R within 1 % of its value at 20 across - square to the axes and turned.
    // It was no curve - its arcs, simplified to a corner of 5 degrees each,
    // never 45 degrees in a run - and its teeth's sides, slanted straight
    // edges, took any curve off its region: its parts each meshed by its own
    // width, their lines shared from part to part, it was cut to one or two
    // across in 6,500 to 7,600 cells, and turned read 9.05 squares where it
    // is 9.70. Square or turned, under 3,200 cells (2,728 and 2,652): turned,
    // a gap between teeth under a feed's end left a piece of the feed 5.3 µm
    // wide with its bottom on the outline, meshed as a strip of its own, its
    // lines fine through the feed and the gear - 6,939 cells. A pie's sector,
    // its two radial sides slanted straight edges,
    // is one region too: 469 cells where its bands were 4,115. (A slanted
    // strip - both sides parallel straight edges, a feed at 30 degrees -
    // keeps its bands: aCurveJoinedByAFeedIsMeshedWhole.)
    void aGearIsMeshedAsOneRegion()
    {
        const auto gear = [](double turn) {
            QJsonArray rows;
            for (int t = 0; t < 36; ++t) {
                const double r = t % 2 == 0 ? 50 : 40, a0 = turn + M_PI * t / 18;
                for (int k = 0; k <= 6; ++k) {
                    const double a = a0 + M_PI / 18 * k / 6;
                    rows.append(QJsonArray{QString::number(r * std::cos(a), 'f', 6) + "[um]", QString::number(r * std::sin(a), 'f', 6) + "[um]"});
                }
            }
            return QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", rows}};
        };
        for (const double turn : {0.0, 0.07}) {
            const rf::Model m = shaped({gear(turn), boxOn("M1", -10, 45, 10, 100), boxOn("M1", -10, -100, 10, -45)});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            QCOMPARE(int(processed.conductors.size()), 1);
            const rf::Stack stack = m.solveStack();
            const rf::Mesh mesh = rf::meshOf(processed.conductors, rf::MeshSettings(), stack, 1e9);
            const QString what = QStringLiteral("turned %1").arg(turn);
            QVERIFY2(mesh.ok() && mesh.lines.join(' ').contains(QStringLiteral("meshed as one region")), qPrintable(what + ": " + mesh.lines.join(" | ")));
            QVERIFY2(mesh.across == 10 && mesh.cells.size() < 3200, qPrintable(QStringLiteral("%1: %2 across, %3 cells").arg(what).arg(mesh.across).arg(mesh.cells.size())));
            const auto [clashes, pieces] = clashesAndPieces(mesh);
            QVERIFY2(clashes == 0 && pieces == 1, qPrintable(QStringLiteral("%1: %2 clashes, %3 pieces").arg(what).arg(clashes).arg(pieces)));
            rf::MeshSettings finer;
            finer.cellsAcross = 20;
            const rf::Mesh fine = rf::meshOf(processed.conductors, finer, stack, 1e9);
            const rf::Inductance at10 = rf::inductanceOf(mesh, processed.conductors, stack, QPointF(0, 100), QPointF(0, -100));
            const rf::Inductance at20 = rf::inductanceOf(fine, processed.conductors, stack, QPointF(0, 100), QPointF(0, -100));
            QVERIFY2(at10.ok() && at20.ok(), qPrintable((at10.problems + at20.problems).join(" | ")));
            QVERIFY2(std::abs(at10.rDc / at20.rDc - 1) < 0.01, qPrintable(QStringLiteral("%1: %2 Ohm at 10 across, %3 at 20").arg(what).arg(at10.rDc).arg(at20.rDc)));
        }
        QJsonArray pie;
        pie.append(QJsonArray{"0", "0"});
        for (int k = 0; k <= 45; ++k)
            pie.append(QJsonArray{QString::number(100 * std::cos(0.3 + k * M_PI / 90), 'f', 6) + "[um]", QString::number(100 * std::sin(0.3 + k * M_PI / 90), 'f', 6) + "[um]"});
        const rf::Model sector = shaped({QJsonObject{{"layer", "M1"}, {"shape", "polygon"}, {"points", pie}}});
        const rf::Mesh cut = rf::meshOf(rf::processRead(sector, nothingRead(), QStringLiteral("none")).conductors, rf::MeshSettings(), sector.solveStack(), 1e9);
        QVERIFY2(cut.ok() && cut.lines.join(' ').contains(QStringLiteral("meshed as one region")) && cut.cells.size() < 1000,
                 qPrintable(QStringLiteral("%1 cells | %2").arg(cut.cells.size()).arg(cut.lines.join(" | "))));
    }

    // A plate 300 µm square with three round holes cut by the cutout keeps its
    // edge strips - each with a long side on the plate's edge, as deep as half
    // its width (2A/P, 92 µm) and more - meshed as strips, their cells long
    // along the edge: 3,677 cells at 20 across, its R within 0.5 % of its
    // value at 40. Taken as strips only by both long sides on the outline
    // (1b2681d9), they joined the region's square cells: 8,610 at 20 across,
    // 33,602 at 40 where it is 13,600. A disc's short wide feed, 20 by 9 µm
    // beyond it, with one long side on the outline and shallower than half
    // the conductor's width, joins the disc's region: 431 cells at 10 across
    // where as a strip of its own it was 982.
    void aPlatesEdgeStripsStayStrips()
    {
        rf::Model plate = shaped({boxOn("M1", 0, 0, 300, 300)});
        for (const QPointF& h : {QPointF(80, 80), QPointF(210, 140), QPointF(110, 220)})
            region(plate, "circle", {{"x", QString::number(h.x()) + "[um]"}, {"y", QString::number(h.y()) + "[um]"}, {"radius", "30[um]"}, {"action", QStringLiteral("remove")}});
        const rf::Processed processed = rf::processRead(plate, nothingRead(), QStringLiteral("none"));
        QCOMPARE(int(processed.conductors.size()), 1);
        const rf::Stack stack = plate.solveStack();
        QList<double> rs;
        for (const int across : {20, 40}) {
            rf::MeshSettings how;
            how.cellsAcross = across;
            how.most = 60000;
            const rf::Mesh mesh = rf::meshOf(processed.conductors, how, stack, 1e9);
            // (At 40 across, past the charge solve's dense bound: the loop's
            //  is its own.)
            QVERIFY2(!mesh.cells.isEmpty() && mesh.lines.join(' ').contains(QStringLiteral("meshed as one region")), qPrintable(mesh.lines.join(" | ")));
            if (across == 20) QVERIFY2(mesh.ok() && mesh.cells.size() < 4500, qPrintable(QStringLiteral("%1 cells at 20 across").arg(mesh.cells.size())));
            const auto [clashes, pieces] = clashesAndPieces(mesh);
            QVERIFY2(clashes == 0 && pieces == 1, qPrintable(QStringLiteral("%1 across: %2 clashes, %3 pieces").arg(across).arg(clashes).arg(pieces)));
            const rf::Inductance loop = rf::inductanceOf(mesh, processed.conductors, stack, QPointF(0, 150), QPointF(300, 150));
            QVERIFY2(loop.ok(), qPrintable(loop.problems.join(" | ")));
            rs << loop.rDc;
        }
        QVERIFY2(std::abs(rs.at(0) / rs.at(1) - 1) < 0.005, qPrintable(QStringLiteral("%1 Ohm at 20 across, %2 at 40").arg(rs.at(0)).arg(rs.at(1))));
        const rf::Model fed = shaped({QJsonObject{{"layer", "M1"}, {"shape", "circle"}, {"x", "0"}, {"y", "0"}, {"radius", "50[um]"}},
                                      boxOn("M1", -10, 48, 10, 58), boxOn("M1", -10, -58, 10, -48)});
        const rf::Mesh disc = rf::meshOf(rf::processRead(fed, nothingRead(), QStringLiteral("none")).conductors, rf::MeshSettings(), fed.solveStack(), 1e9);
        QVERIFY2(disc.ok() && disc.cells.size() < 700, qPrintable(QStringLiteral("%1 cells, a disc with short feeds").arg(disc.cells.size())));
    }

    // A region's square clipped within its row (rf::RowClip) comes out as
    // against the whole outline, corner for corner: 10,000 random rows, six
    // squares in each, over a thin ring as processing writes it - a keyhole
    // of some 1,400 corners -, a keyhole ring with a slanted slit, a star
    // crossing itself and a figure of eight. Its band's outline is the edges
    // that reach the row, a stretch outside it one side, outside too: on the
    // ring, under a quarter of its corners. (meshOf() clipped every square
    // against the whole outline: the ring 1 µm wide took 5 s.) A square the
    // slanted slit crosses is clipped whole: against the band's outline the
    // boolean split some 70 of 72,000 along it, the same area in two pieces
    // where the whole outline gave one.
    void aRowsOutlineClipsAsTheWhole()
    {
        QList<QPolygonF> outlines;
        {
            const rf::Model m = shaped({QJsonObject{{"layer", "M1"}, {"shape", "ring"}, {"x", "0"}, {"y", "0"}, {"radius", "100.5[um]"}, {"inner_radius", "99.5[um]"}}});
            const rf::Processed processed = rf::processRead(m, nothingRead(), QStringLiteral("none"));
            QCOMPARE(int(processed.conductors.size()), 1);
            outlines << processed.conductors.first().outline;
        }
        QPolygonF key, star;
        for (int k = 0; k <= 180; ++k) key << QPointF(100 * std::cos(2.0 * k * M_PI / 180), 100 * std::sin(2.0 * k * M_PI / 180));
        for (int k = 180; k >= 0; --k) key << QPointF(30 + 50 * std::cos(2.0 * k * M_PI / 180), 10 + 50 * std::sin(2.0 * k * M_PI / 180));
        for (int k = 0; k < 7; ++k) star << QPointF(80 * std::cos(4 * M_PI * k / 7), 80 * std::sin(4 * M_PI * k / 7));
        outlines << key << star << QPolygonF{{0, 0}, {20, 20}, {20, 0}, {0, 10}};
        const auto corners = [](const QList<QPolygonF>& parts) {
            QStringList all;
            for (const QPolygonF& p : parts) {
                QStringList pts;
                for (const QPointF& q : p) pts << QStringLiteral("%1,%2").arg(qint64(std::llround(q.x() * 1e6))).arg(qint64(std::llround(q.y() * 1e6)));
                std::sort(pts.begin(), pts.end());
                all << pts.join(';');
            }
            std::sort(all.begin(), all.end());
            return all.join('|');
        };
        std::mt19937 random(5);
        int compared = 0, cut = 0;
        for (const QPolygonF& outline : std::as_const(outlines)) {
            const QRectF box = outline.boundingRect();
            const auto within = [&random](double lo, double hi) { return lo + (hi - lo) * double(random() % 100001) / 100000.0; };
            for (int row = 0; row < 2500; ++row) {
                const double y0 = within(box.top(), box.bottom()), y1 = std::min(box.bottom(), y0 + within(0.01, 0.1) * box.height());
                const double margin = 1e-6 * std::max(box.width(), box.height());
                const rf::RowClip clip(outline, y0 - margin, y1 + margin);
                if (outline.size() > 1000) QVERIFY2(clip.band().size() < outline.size() / 4, qPrintable(QStringLiteral("%1 of %2 corners").arg(clip.band().size()).arg(outline.size())));
                for (int k = 0; k < 6; ++k) {
                    const double x0 = within(box.left(), box.right()), x1 = std::min(box.right(), x0 + within(0.01, 0.2) * box.width());
                    const QPolygonF square(QRectF(QPointF(x0, y0), QPointF(x1, y1)));
                    const QList<QPolygonF> whole = rf::clipTo({outline}, square);
                    const QList<QPolygonF> banded = clip.clip(square.boundingRect());
                    QCOMPARE(corners(banded), corners(whole));
                    ++compared;
                    if (!whole.isEmpty()) ++cut;
                }
            }
        }
        QCOMPARE(compared, 4 * 2500 * 6);
        QVERIFY2(cut > compared / 4, qPrintable(QStringLiteral("%1 of %2 squares cut anything").arg(cut).arg(compared)));
    }

    // A circle and a ring of the model's own, by their centre and radii, each
    // an expression of the parameters: a circle's corners on it, its sides
    // within a nanometre, its area the circle's; a ring the keyhole a file
    // writes a hole as, its area between its two circles'. Processed, the
    // ring is one conductor with its hole, and its mesh has no cell in the
    // hole. A radius of nothing, or a ring's inner one past its outer, is
    // left out and said. The cutout's circle is the same polygon.
    void aCircleAndARingAreShapes()
    {
        rf::Model m = stacked();
        m.root.child(QStringLiteral("rf_parameters"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"R", "50[um]"});
        QVERIFY(m.add(m.layout().tag, m.make(QStringLiteral("rf_shapes"))) != nullptr);
        const QString shapes = m.layout().child(QStringLiteral("rf_shapes"))->tag;
        const auto shape = [&](const QJsonObject& props) {
            rf::Node n = m.make(QStringLiteral("rf_shape"));
            n.props = props;
            const rf::Node* added = m.add(shapes, n);
            return added != nullptr ? added->tag : QString();
        };
        const QString circle = shape({{"layer", "M1"}, {"shape", "circle"}, {"x", "100[um]"}, {"y", "0"}, {"radius", "R"}});
        const QString ring = shape({{"layer", "M1"}, {"shape", "ring"}, {"x", "400[um]"}, {"y", "0"}, {"radius", "60[um]"}, {"inner_radius", "40[um]"}});
        QStringList problems;
        QList<rf::OwnShape> own = rf::ownShapesOf(m, &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
        QCOMPARE(int(own.size()), 2);
        const auto of = [&own](const QString& tag) {
            for (const rf::OwnShape& s : own)
                if (s.tag == tag) return s;
            return rf::OwnShape();
        };
        const QPolygonF disc = of(circle).outline;
        QVERIFY2(disc.size() >= 32 && disc.size() <= 1024, qPrintable(QString::number(disc.size())));
        for (const QPointF& p : disc) QVERIFY(std::abs(std::hypot(p.x() - 100, p.y()) - 50) < 1e-9);
        QVERIFY(50 * (1 - std::cos(M_PI / disc.size())) <= 1e-3 + 1e-12);
        QVERIFY2(std::abs(std::abs(rf::areaOf(disc)) / (M_PI * 2500) - 1) < 1e-4, qPrintable(QString::number(rf::areaOf(disc))));
        QVERIFY2(std::abs(std::abs(rf::areaOf(of(ring).outline)) / (M_PI * (3600 - 1600)) - 1) < 1e-4,
                 qPrintable(QString::number(rf::areaOf(of(ring).outline))));
        // Its loops, its slit taken out, for drawing its edges: the outer
        // circle and the hole, each once; a circle's its own.
        {
            const QList<QPolygonF> loops = rf::loopsOf(of(ring).outline);
            QCOMPARE(int(loops.size()), 2);
            QList<double> areas{std::abs(rf::areaOf(loops.at(0))), std::abs(rf::areaOf(loops.at(1)))};
            std::sort(areas.begin(), areas.end());
            QVERIFY2(std::abs(areas.at(0) / (M_PI * 1600) - 1) < 1e-4 && std::abs(areas.at(1) / (M_PI * 3600) - 1) < 1e-4,
                     qPrintable(QStringLiteral("%1, %2").arg(areas.at(0)).arg(areas.at(1))));
            QList<int> corners{int(loops.at(0).size()), int(loops.at(1).size())};
            std::sort(corners.begin(), corners.end());
            QCOMPARE(corners, (QList<int>{int(rf::circleOf(QPointF(400, 0), 40).size()), int(rf::circleOf(QPointF(400, 0), 60).size())}));
            QCOMPARE(int(rf::loopsOf(disc).size()), 1);
            QCOMPARE(rf::loopsOf(disc).first(), disc);
        }
        const QRectF ringBox = of(ring).outline.boundingRect();
        QVERIFY2(std::abs(ringBox.left() - 340) < 1e-3 && std::abs(ringBox.right() - 460) < 1e-9 && std::abs(ringBox.height() - 120) < 1e-3,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(ringBox.left()).arg(ringBox.right()).arg(ringBox.height())));
        // The cutout's circle is the same.
        QCOMPARE(rf::circleOf(QPointF(100, 0), 50), disc);
        // Large ones capped at 1024 sides, 4.7 nm in at 1 mm; one past an int's
        // worth of sides capped as well, not overflowed (the review of
        // 747a2cfd).
        {
            const QPolygonF mm = rf::circleOf(QPointF(), 1000);
            QCOMPARE(int(mm.size()), 1024);
            const double in = 1000 * (1 - std::cos(M_PI / 1024));
            QVERIFY2(in > 4.6e-3 && in < 4.8e-3, qPrintable(QString::number(in)));
            QCOMPARE(int(rf::circleOf(QPointF(), 1e16).size()), 1024);
            QCOMPARE(int(rf::circleOf(QPointF(), 1e-6).size()), 32);
        }
        // A parameter changed: the circle with it.
        m.root.child(QStringLiteral("rf_parameters"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"R", "80[um]"});
        own = rf::ownShapesOf(m);
        QVERIFY(std::abs(of(circle).outline.boundingRect().right() - 180) < 1e-9);

        // Processed over an empty layout: two conductors, the ring one with
        // its hole; meshed, no cell in the hole, and the ring's cells its area.
        m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), QJsonArray() << QJsonArray{"1", "0", "M1"});
        qucs_s::layout::Layout empty;
        qucs_s::layout::Cell top;
        top.name = QStringLiteral("TOP");
        empty.cells << top;
        empty.byName.insert(top.name, 0);
        const rf::Processed processed = rf::processRead(m, empty, QStringLiteral("none"));
        QVERIFY2(processed.problems.isEmpty(), qPrintable(processed.problems.join(" | ")));
        QCOMPARE(int(processed.conductors.size()), 2);
        rf::MeshSettings how;
        how.maximum = 10e-6;
        how.cellsAcross = 0;
        const rf::Mesh mesh = rf::meshOf(processed.conductors, how, m.solveStack(), 1e9);
        QVERIFY(!mesh.cells.isEmpty());
        double ringArea = 0, discArea = 0;
        for (const rf::MeshCell& c : mesh.cells) {
            const double r = std::hypot(c.centre.x() - 400, c.centre.y());
            if (c.centre.x() > 300) {
                // (A cell cut by the inner circle, concave there, has its
                //  centroid a little inside it: none in the hole itself.)
                QVERIFY2(r > 39 && r < 61, qPrintable(QStringLiteral("a cell at %1, %2").arg(c.centre.x()).arg(c.centre.y())));
                ringArea += c.area;
            } else {
                discArea += c.area;
            }
        }
        QVERIFY2(std::abs(ringArea / (M_PI * 2000) - 1) < 0.005, qPrintable(QString::number(ringArea)));
        // Its cells meet edge to edge all round, across the keyhole's slit too:
        // every side of a cell inside the metal is another cell's, paired as
        // the solve pairs them - by its middle, to a nanometre. (The slit cut
        // each slab it crossed in two, meshed on their own, and they met
        // along a third of it - the review of 747a2cfd.)
        {
            QHash<QString, int> sides;
            QHash<QString, QPointF> middles;
            QHash<QString, double> lengths;
            for (const rf::MeshCell& c : mesh.cells)
                for (int k = 0; k < c.outline.size(); ++k) {
                    const QPointF p = c.outline.at(k), q = c.outline.at((k + 1) % c.outline.size());
                    if (QLineF(p, q).length() < 1e-9) continue;
                    const QPointF mid = (p + q) / 2;
                    const QString key = QStringLiteral("%1,%2").arg(std::llround(mid.x() * 1e3)).arg(std::llround(mid.y() * 1e3));
                    sides[key] += 1;
                    middles.insert(key, mid);
                    lengths.insert(key, QLineF(p, q).length());
                }
            double lone = 0;
            for (auto it = sides.cbegin(); it != sides.cend(); ++it) {
                const QPointF mid = middles.value(it.key());
                const double r = std::hypot(mid.x() - 400, mid.y());
                if (it.value() == 1 && mid.x() > 300 && r > 41 && r < 59) lone += lengths.value(it.key());
            }
            QVERIFY2(lone < 1e-9, qPrintable(QStringLiteral("%1 µm of cell sides inside the ring are no other cell's").arg(lone)));
        }
        QVERIFY2(std::abs(discArea / (M_PI * 6400) - 1) < 0.005, qPrintable(QString::number(discArea)));

        // What cannot be read: left out, and said.
        m.find(circle)->props.insert(QStringLiteral("radius"), QStringLiteral("0"));
        m.find(ring)->props.insert(QStringLiteral("inner_radius"), QStringLiteral("70[um]"));
        problems.clear();
        own = rf::ownShapesOf(m, &problems);
        QVERIFY(own.isEmpty());
        const QString said = problems.join(QStringLiteral(" | "));
        QVERIFY2(said.contains(QStringLiteral("radius is not a length above zero")), qPrintable(said));
        QVERIFY2(said.contains(QStringLiteral("inner radius is to be above zero and under its radius (70 and 60 µm)")), qPrintable(said));
    }

    // A Dimension (Layout > Dimension): the corners of a layer's shapes of
    // the layout file inside its box - its edges too - moved by an expression
    // of the parameters, before the processing. A box round a strip's top
    // edge widens it to w; one round the whole of it moves it, its width as
    // it was; another layer's shapes and those outside untouched; an angle
    // of 90 degrees as +y. One that would make a shape cross itself leaves it
    // as it was, and says so; one whose box holds no corner says that; one
    // that cannot be read is left out, and said.
    void aDimensionMovesTheFilesCorners()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("strips.gds"));
        {
            gdstk::Library lib = {};
            lib.init("STRIPS", 1e-6, 1e-9);
            auto* cell = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
            cell->init("TOP");
            lib.cell_array.append(cell);
            const auto box = [cell](double x0, double y0, double x1, double y1, uint32_t layer) {
                auto* r = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
                gdstk::Vec2 a, b;
                a.x = x0;
                a.y = y0;
                b.x = x1;
                b.y = y1;
                *r = gdstk::rectangle(a, b, gdstk::make_tag(layer, 0));
                cell->polygon_array.append(r);
            };
            box(0, 0, 1000, 50, 1);       // the strip
            box(2000, 0, 2100, 100, 1);   // a pad apart
            box(0, 40, 1000, 60, 2);      // another layer's strip over the edge
            QVERIFY(lib.write_gds(QFile::encodeName(file).constData(), 0, nullptr) == gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        QString why;
        const std::shared_ptr<qucs_s::layout::Layout> read = qucs_s::layout::read(file, &why);
        QVERIFY2(read != nullptr, qPrintable(why));
        rf::Model m = stacked();
        m.layout().props.insert(QStringLiteral("file"), file);
        QJsonArray map;
        map.append(QJsonArray{"1", "0", "M1"});
        map.append(QJsonArray{"2", "0", "M2"});
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", map);
        set(*m.root.child(QStringLiteral("rf_parameters")), "rows", QJsonArray() << QJsonArray{"w", "80[um]"});
        rf::Node d = m.make(QStringLiteral("rf_dimension"));
        d.props = QJsonObject{{"layer", "M1"}, {"x0", "-1[um]"}, {"y0", "45[um]"}, {"x1", "1001[um]"}, {"y1", "55[um]"},
                              {"direction", "+y"}, {"offset", "w - 50[um]"}};
        const rf::Node* added = m.add(m.layout().tag, d);
        QVERIFY(added != nullptr);
        const QString tag = added->tag;
        const auto boxesOn = [&](const QString& layer) {
            const rf::Processed p = rf::processRead(m, *read, QStringLiteral("strips.gds"));
            QList<QRectF> out;
            for (const rf::Conductor& c : p.conductors)
                if (c.layer == layer) out << c.outline.boundingRect();
            std::sort(out.begin(), out.end(), [](const QRectF& a, const QRectF& b) { return a.left() < b.left(); });
            return out;
        };
        QCOMPARE(boxesOn(QStringLiteral("M1")), (QList<QRectF>{QRectF(0, 0, 1000, 80), QRectF(2000, 0, 100, 100)}));
        QCOMPARE(boxesOn(QStringLiteral("M2")), (QList<QRectF>{QRectF(0, 40, 1000, 20)}));
        {
            const rf::Processed p = rf::processRead(m, *read, QStringLiteral("strips.gds"));
            QVERIFY2(p.problems.isEmpty(), qPrintable(p.problems.join(" | ")));
            QVERIFY2(p.report.lines.join(' ').contains(QStringLiteral("moved 2 corner(s) of M1 by 30 µm")), qPrintable(p.report.lines.join(" | ")));
        }
        // A box drawn along the edge itself, of no height: it holds the edge.
        m.find(tag)->props.insert(QStringLiteral("x0"), QStringLiteral("0"));
        m.find(tag)->props.insert(QStringLiteral("y0"), QStringLiteral("50[um]"));
        m.find(tag)->props.insert(QStringLiteral("x1"), QStringLiteral("1000[um]"));
        m.find(tag)->props.insert(QStringLiteral("y1"), QStringLiteral("50[um]"));
        QCOMPARE(boxesOn(QStringLiteral("M1")).first(), QRectF(0, 0, 1000, 80));
        // An angle of 90 degrees: +y.
        m.find(tag)->props.insert(QStringLiteral("direction"), QStringLiteral("angle"));
        m.find(tag)->props.insert(QStringLiteral("angle"), QStringLiteral("90"));
        QCOMPARE(boxesOn(QStringLiteral("M1")).first(), QRectF(0, 0, 1000, 80));
        // Round the whole strip: moved, as wide as it was.
        m.find(tag)->props = QJsonObject{{"layer", "M1"}, {"x0", "-1[um]"}, {"y0", "-1[um]"}, {"x1", "1001[um]"}, {"y1", "51[um]"},
                                         {"direction", "+x"}, {"offset", "100[um]"}};
        QCOMPARE(boxesOn(QStringLiteral("M1")).first(), QRectF(100, 0, 1000, 50));
        // A corner alone pulled across its shape: left as it was, and said.
        m.find(tag)->props = QJsonObject{{"layer", "M1"}, {"x0", "995[um]"}, {"y0", "45[um]"}, {"x1", "1005[um]"}, {"y1", "55[um]"},
                                         {"direction", "-y"}, {"offset", "100[um]"}};
        {
            const rf::Processed p = rf::processRead(m, *read, QStringLiteral("strips.gds"));
            QVERIFY2(p.problems.join(' ').contains(QStringLiteral("cross itself")), qPrintable(p.problems.join(" | ")));
        }
        QCOMPARE(boxesOn(QStringLiteral("M1")).first(), QRectF(0, 0, 1000, 50));
        // A box of no corner: said.
        m.find(tag)->props.insert(QStringLiteral("x0"), QStringLiteral("500[um]"));
        m.find(tag)->props.insert(QStringLiteral("x1"), QStringLiteral("510[um]"));
        {
            const rf::Processed p = rf::processRead(m, *read, QStringLiteral("strips.gds"));
            QVERIFY2(p.report.lines.join(' ').contains(QStringLiteral("moves no corner")), qPrintable(p.report.lines.join(" | ")));
        }
        // Not read: left out, and said.
        m.find(tag)->props.insert(QStringLiteral("offset"), QStringLiteral("q + 1[um]"));
        QStringList problems;
        QVERIFY(rf::dimensionsOf(m, &problems).isEmpty());
        QVERIFY2(problems.join(' ').contains(QStringLiteral("offset is not a number")), qPrintable(problems.join(" | ")));
        m.find(tag)->props.insert(QStringLiteral("offset"), QStringLiteral("1[um]"));
        m.find(tag)->props.insert(QStringLiteral("layer"), QStringLiteral("M7"));
        problems.clear();
        QVERIFY(rf::dimensionsOf(m, &problems).isEmpty());
        QVERIFY2(problems.join(' ').contains(QStringLiteral("M7, which is no layer")), qPrintable(problems.join(" | ")));
    }

    // The Cutout: a part of the tree in every model - a new one, and one of
    // a file written before it -, written and read back with its regions,
    // and a layer renamed in the regions that cut it.
    void everyModelHasACutout()
    {
        rf::Model m = rf::Model::blank();
        const rf::Node* cutout = m.layout().child(QStringLiteral("rf_cutout"));
        QVERIFY(cutout != nullptr);
        QCOMPARE(cutout->tag, QStringLiteral("cutout"));
        QVERIFY(cutout->children.empty());
        QVERIFY(rf::cutoutOf(m, nullptr).isEmpty());

        // A file of a build before it: the Cutout made.
        QJsonObject tree = m.toObject();
        QJsonObject root = tree.value("tree").toObject();
        QJsonArray parts = root.value("children").toArray();
        for (int i = 0; i < parts.size(); ++i) {
            QJsonObject part = parts.at(i).toObject();
            if (part.value("type").toString() != QLatin1String("rf_layout")) continue;
            QJsonArray kept;
            for (const QJsonValue& c : part.value("children").toArray())
                if (c.toObject().value("type").toString() != QLatin1String("rf_cutout")) kept.append(c);
            part.insert("children", kept);
            parts.replace(i, part);
        }
        root.insert("children", parts);
        tree.insert("tree", root);
        QVERIFY(!QJsonDocument(tree).toJson().contains("rf_cutout"));
        QString why;
        const std::optional<rf::Model> old = rf::Model::fromJson(QJsonDocument(tree).toJson(), &why);
        QVERIFY2(old.has_value(), qPrintable(why));
        QVERIFY(old->layout().child(QStringLiteral("rf_cutout")) != nullptr);

        // Its regions go out and come back; a layer renamed in them.
        m = stacked();
        region(m, "rectangle", {{"x1", QStringLiteral("50[um]")}, {"layers", QStringLiteral("M1, V1")}}, QStringLiteral("Pads"));
        const std::optional<rf::Model> back = rf::Model::fromJson(m.toJson(), &why);
        QVERIFY2(back.has_value() && why.isEmpty(), qPrintable(why));
        const rf::Node& r = back->layout().child(QStringLiteral("rf_cutout"))->children.at(0);
        QCOMPARE(r.label, QStringLiteral("Pads"));
        QCOMPARE(rf::textOf(r, QStringLiteral("x1")), QStringLiteral("50[um]"));
        m.renameLayer(QStringLiteral("M1"), QStringLiteral("Metal1"));
        QCOMPARE(rf::namesOf(m.layout().child(QStringLiteral("rf_cutout"))->children.at(0), QStringLiteral("layers")),
                 QStringList({QStringLiteral("Metal1"), QStringLiteral("V1")}));
        // (Its own kinds: a Region goes under the Cutout and nowhere else.)
        QString no;
        QVERIFY(m.add(QStringLiteral("cutout"), m.make(QStringLiteral("rf_region")), -1, &no) != nullptr);
        QVERIFY(m.add(QStringLiteral("ports"), m.make(QStringLiteral("rf_region")), -1, &no) == nullptr);
        QVERIFY(m.add(QStringLiteral("cutout"), m.make(QStringLiteral("rf_metal")), -1, &no) == nullptr);
    }

    // Its regions read: a rectangle by two corners in either order, a
    // polygon by its corners, a circle, each in µm through the parameters;
    // one disabled, or the Cutout not applied, left out; and what cannot be
    // read said and left out.
    void theCutoutIsReadFromItsRegions()
    {
        rf::Model m = stacked();
        QJsonArray parameters;
        parameters.append(QJsonArray{QStringLiteral("w"), QStringLiteral("40[um]")});
        set(*m.root.child(QStringLiteral("rf_parameters")), "rows", parameters);
        region(m, "rectangle", {{"x0", QStringLiteral("w")}, {"y0", QStringLiteral("30[um]")},
                                {"x1", QStringLiteral("-10[um]")}, {"y1", QStringLiteral("0")}});
        QJsonArray corners;
        corners.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
        corners.append(QJsonArray{QStringLiteral("w"), QStringLiteral("0")});
        corners.append(QJsonArray{QStringLiteral("0"), QStringLiteral("w")});
        corners.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});   // (closing the ring: not a corner)
        region(m, "polygon", {{"points", corners}, {"action", QStringLiteral("remove")}, {"layers", QStringLiteral("M2")}});
        region(m, "circle", {{"x", QStringLiteral("100[um]")}, {"y", QStringLiteral("-20[um]")}, {"radius", QStringLiteral("25[um]")}});
        region(m, "rectangle", {{"x1", QStringLiteral("1[mm]")}}).enabled = false;

        QStringList problems;
        const rf::Cutout read = rf::cutoutOf(m, &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(" | ")));
        QCOMPARE(read.regions.size(), 3);
        const rf::CutRegion& box = read.regions.at(0);
        QVERIFY(box.keep);
        QVERIFY(box.layers.isEmpty() && box.cuts(QStringLiteral("M1")) && box.cuts(QStringLiteral("V1")));
        QCOMPARE(box.outline.size(), 4);
        QCOMPARE(box.outline.boundingRect(), QRectF(QPointF(-10, 0), QPointF(40, 30)));
        const rf::CutRegion& triangle = read.regions.at(1);
        QVERIFY(!triangle.keep);
        QCOMPARE(triangle.layers, QStringList({QStringLiteral("M2")}));
        QVERIFY(triangle.cuts(QStringLiteral("M2")) && !triangle.cuts(QStringLiteral("M1")));
        QCOMPARE(triangle.outline.size(), 3);
        QVERIFY(qAbs(rf::areaOf(triangle.outline) - 800) < 1e-9);
        const rf::CutRegion& circle = read.regions.at(2);
        QVERIFY2(circle.outline.size() >= 32, qPrintable(QString::number(circle.outline.size())));
        for (const QPointF& p : circle.outline) QVERIFY(qAbs(std::hypot(p.x() - 100, p.y() + 20) - 25) < 1e-9);
        // Its sides within a nanometre of the circle.
        const double sagitta = 25 * (1 - std::cos(M_PI / circle.outline.size()));
        QVERIFY2(sagitta <= 1e-3, qPrintable(QString::number(sagitta)));

        // Not applied: none, and nothing said.
        set(*m.layout().child(QStringLiteral("rf_cutout")), "apply", false);
        QVERIFY(rf::cutoutOf(m, &problems).isEmpty());
        QVERIFY(problems.isEmpty());
        set(*m.layout().child(QStringLiteral("rf_cutout")), "apply", true);

        // What cannot be read: each said by its name, and left out.
        m.layout().child(QStringLiteral("rf_cutout"))->children.clear();
        region(m, "rectangle", {{"x1", QStringLiteral("50[um")}}, QStringLiteral("Typo"));
        QJsonArray two;
        two.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
        two.append(QJsonArray{QStringLiteral("1[um]"), QStringLiteral("0")});
        region(m, "polygon", {{"points", two}}, QStringLiteral("Line"));
        region(m, "rectangle", {{"x1", QStringLiteral("0")}}, QStringLiteral("Flat"));
        region(m, "circle", {{"radius", QStringLiteral("0")}}, QStringLiteral("Dot"));
        region(m, "rectangle", {{"layers", QStringLiteral("M9")}}, QStringLiteral("Elsewhere"));
        region(m, "rectangle", {}, QStringLiteral("Good"));
        problems.clear();
        const rf::Cutout bad = rf::cutoutOf(m, &problems);
        QCOMPARE(bad.regions.size(), 1);
        QCOMPARE(bad.regions.at(0).name, QStringLiteral("Good"));
        QCOMPARE(bad.regions.at(0).outline.boundingRect(), QRectF(0, 0, 100, 100));
        for (const char* said : {"Typo's x1 is not a number", "Line is a polygon of 2 corner", "Flat has no area",
                                 "Dot's radius is not a length above zero", "Elsewhere cuts M9, which is no layer"})
            QVERIFY2(says(problems, said), qPrintable(QLatin1String(said) + QLatin1String(": ") + problems.join(" | ")));
        QCOMPARE(problems.size(), 5);
        // Each with its region's tag: R1's own, not R10's, whose name begins
        // with R1's (the review of 26ff2805: matched by "Cutout: R1", R1's
        // row took R10's too).
        m.layout().child(QStringLiteral("rf_cutout"))->children.clear();
        const QString r1 = region(m, "rectangle", {{"x1", QStringLiteral("0")}}, QStringLiteral("R1")).tag;
        const QString r10 = region(m, "circle", {{"radius", QStringLiteral("0")}}, QStringLiteral("R10")).tag;
        const rf::Cutout tagged = rf::cutoutOf(m, nullptr);
        QCOMPARE(tagged.problems.size(), 2);
        QCOMPARE(tagged.problems.at(0).first, r1);
        QVERIFY(tagged.problems.at(0).second.contains(QStringLiteral("has no area")));
        QCOMPARE(tagged.problems.at(1).first, r10);
        QVERIFY(tagged.problems.at(1).second.contains(QStringLiteral("radius")));
    }

    // What the regions do to a layer's shapes: one inside kept as it was
    // drawn, one across an edge cut along it, one outside masked; what a
    // region takes out cut away; two regions that keep as their union; a
    // region of another layer nothing to this one.
    void theCutoutKeepsWhatIsInsideAndMasksTheRest()
    {
        rf::Cutout cutout;
        cutout.regions << rf::CutRegion{QStringLiteral("A"), QPolygonF(QRectF(0, 0, 100, 100)), true, {}};
        const QPolygonF inside = box(10, 10, 20, 20);
        const QPolygonF across = box(80, 40, 50, 10);
        const QPolygonF outside = box(200, 0, 10, 10);
        int cut = -1, masked = -1;
        const QList<QPolygonF> kept = rf::cutOf({inside, across, outside}, cutout, QStringLiteral("M1"), &cut, &masked);
        QCOMPARE(cut, 1);
        QCOMPARE(masked, 1);
        QCOMPARE(kept.size(), 2);
        QCOMPARE(kept.at(0), inside);   // (its points as drawn)
        QVERIFY(qAbs(rf::areaOf(kept.at(1)) - 200) < 1e-6);
        QVERIFY(kept.at(1).boundingRect().right() <= 100 + 1e-9);

        // Taken out: a hole in a plane, and a shape inside it gone.
        cutout.regions << rf::CutRegion{QStringLiteral("B"), QPolygonF(QRectF(40, 40, 20, 20)), false, {}};
        const QList<QPolygonF> holed = rf::cutOf({box(0, 0, 100, 100), box(45, 45, 5, 5)}, cutout, QStringLiteral("M1"), &cut, &masked);
        QCOMPARE(cut, 1);
        QCOMPARE(masked, 1);
        QVERIFY2(qAbs(areaOfAll(holed) - (10000 - 400)) < 1e-6, qPrintable(QString::number(areaOfAll(holed))));

        // Two that keep, overlapping, one given clockwise and one
        // anticlockwise, as a model reads them: a shape across both is whole.
        rf::Model m = stacked();
        region(m, "rectangle", {{"x1", QStringLiteral("60[um]")}, {"y1", QStringLiteral("10[um]")}});
        QJsonArray clockwise;
        for (const QPointF& p : {QPointF(40, 0), QPointF(40, 10), QPointF(100, 10), QPointF(100, 0)})
            clockwise.append(QJsonArray{QStringLiteral("%1[um]").arg(p.x()), QStringLiteral("%1[um]").arg(p.y())});
        region(m, "polygon", {{"points", clockwise}});
        const rf::Cutout two = rf::cutoutOf(m, nullptr);
        QCOMPARE(two.regions.size(), 2);
        const QPolygonF bridge = box(20, 2, 60, 6);
        const QList<QPolygonF> joined = rf::cutOf({bridge}, two, QStringLiteral("M1"), &cut, &masked);
        QCOMPARE(joined, QList<QPolygonF>{bridge});
        QCOMPARE(cut + masked, 0);

        // Of another layer: nothing done here; on its own layer, its work.
        rf::Cutout other;
        other.regions << rf::CutRegion{QStringLiteral("M2 only"), QPolygonF(QRectF(0, 0, 1, 1)), true, {QStringLiteral("M2")}};
        QCOMPARE(rf::cutOf({outside}, other, QStringLiteral("M1"), &cut, &masked), QList<QPolygonF>{outside});
        QCOMPARE(cut + masked, 0);
        QVERIFY(rf::cutOf({outside}, other, QStringLiteral("M2"), &cut, &masked).isEmpty());
        QCOMPARE(masked, 1);
        // Only what takes out, on this layer: everything else kept.
        rf::Cutout hole;
        hole.regions << rf::CutRegion{QStringLiteral("H"), QPolygonF(QRectF(0, 0, 10, 10)), false, {}};
        QCOMPARE(rf::cutOf({outside, inside}, hole, QStringLiteral("M1"), &cut, &masked), (QList<QPolygonF>{outside, inside}));
        QCOMPARE(cut + masked, 0);

        // A cut taking little of a long shape is made: a gap 1 µm long across
        // a line 0.3 µm wide and 1 mm long, and a region that keeps all of it
        // but its last micrometre. Each takes 0.3 µm², under the 2 µm² the
        // line's boundary's step allowed against its area, and the line was
        // handed on whole - as was a ring 0.3 µm wide whose slit was so cut.
        // One touching a region that takes out, along its edge, is still
        // handed on as drawn.
        const QPolygonF line = box(0, 0, 1000, 0.3);
        rf::Cutout gap;
        gap.regions << rf::CutRegion{QStringLiteral("G"), QPolygonF(QRectF(500, -1, 1, 2)), false, {}};
        const QList<QPolygonF> parted = rf::cutOf({line}, gap, QStringLiteral("M1"), &cut, &masked);
        QCOMPARE(cut, 1);
        QCOMPARE(parted.size(), 2);
        QVERIFY2(qAbs(areaOfAll(parted) - (300 - 0.3)) < 1e-6, qPrintable(QString::number(areaOfAll(parted), 'g', 10)));
        rf::Cutout most;
        most.regions << rf::CutRegion{QStringLiteral("K"), QPolygonF(QRectF(-1, -1, 1000, 2)), true, {}};
        const QList<QPolygonF> shorter = rf::cutOf({line}, most, QStringLiteral("M1"), &cut, &masked);
        QCOMPARE(cut, 1);
        QVERIFY2(qAbs(areaOfAll(shorter) - (300 - 0.3)) < 1e-6, qPrintable(QString::number(areaOfAll(shorter), 'g', 10)));
        rf::Cutout beside;
        beside.regions << rf::CutRegion{QStringLiteral("T"), QPolygonF(QRectF(30, 0, 10, 50)), false, {}};
        QCOMPARE(rf::cutOf({inside}, beside, QStringLiteral("M1"), &cut, &masked), QList<QPolygonF>{inside});
        QCOMPARE(cut + masked, 0);
    }

    // In the processing, the Cutout comes first: what it masks is never
    // merged, two shapes it keeps whole are not joined by it with the merge
    // off, and what it did is said - and, where it keeps nothing, why
    // nothing is left.
    void theProcessingCutsFirst()
    {
        const rf::Stack stack = stacked().stack();
        QMap<QString, QList<QPolygonF>> shapes;
        shapes[QStringLiteral("M1")] = {box(0, 0, 10, 10), box(5, 5, 10, 10), box(90, 0, 20, 5), box(300, 300, 5, 5)};
        shapes[QStringLiteral("M2")] = {box(500, 500, 5, 5)};
        rf::Processing how;
        how.cutout.regions << rf::CutRegion{QStringLiteral("A"), QPolygonF(QRectF(0, 0, 100, 100)), true, {}};
        how.merge = false;
        const rf::Processed done = rf::processShapes(shapes, {}, how, stack);
        QVERIFY2(done.ok(), qPrintable(done.problems.join(" | ")));
        // (The two it keeps whole overlap, and are joined for that alone,
        //  said: apart, their overlap was meshed twice.)
        QCOMPARE(done.byLayer().value(QStringLiteral("M1")), 2);
        QVERIFY2(says(done.report.lines, "M1: 2 overlapping shapes joined into 1"), qPrintable(done.report.summary()));
        QCOMPARE(done.byLayer().value(QStringLiteral("M2")), 0);
        QCOMPARE(done.report.cut, 1);
        QCOMPARE(done.report.masked, 2);
        QVERIFY2(says(done.report.lines, "cutout's 1 region(s) applied first"), qPrintable(done.report.summary()));
        QVERIFY2(says(done.report.lines, "M1: the cutout cut 1 shape(s) along its regions' edges and masked 1"),
                 qPrintable(done.report.summary()));
        how.merge = true;
        QCOMPARE(rf::processShapes(shapes, {}, how, stack).byLayer().value(QStringLiteral("M1")), 2);

        // A region nowhere near the shapes: nothing left, and the Cutout said
        // to be why.
        how.cutout.regions = {rf::CutRegion{QStringLiteral("Far"), QPolygonF(QRectF(1e5, 1e5, 10, 10)), true, {}}};
        const rf::Processed none = rf::processShapes(shapes, {}, how, stack);
        QVERIFY(none.conductors.isEmpty());
        QVERIFY2(says(none.problems, "regions keep none of the shapes"), qPrintable(none.problems.join(" | ")));
        // No Cutout: nothing of it said.
        how.cutout = {};
        QVERIFY(!says(rf::processShapes(shapes, {}, how, stack).report.lines, "cutout"));
    }

    // A layout file cut: half of it kept by a rectangle over its left half,
    // a model's Cutout as processRead() reads it - the shapes all inside that
    // half, less of their area than the whole layout's, and none of it
    // outside; turned off, the whole layout again.
    void aLayoutFileIsCutByTheModelsCutout()
    {
        const QString file = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/thermo_optic_heater.gds");
        if (!QFileInfo::exists(file)) QSKIP("the example layout is not in this build");
        rf::Model m = stacked();
        m.layout().props.insert(QStringLiteral("file"), file);
        QJsonArray rows;
        for (int layer = 0; layer < 64; ++layer)
            rows.append(QJsonArray{QString::number(layer), QStringLiteral("0"), QStringLiteral("M1")});
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", rows);
        QString why;
        const std::shared_ptr<qucs_s::layout::Layout> read = qucs_s::layout::read(file, &why);
        QVERIFY2(read != nullptr, qPrintable(why));
        const rf::Processed whole = rf::processRead(m, *read, QStringLiteral("heater.gds"));
        QVERIFY(!whole.conductors.isEmpty());
        QRectF all;
        double wholeArea = 0;
        for (const rf::Conductor& c : whole.conductors) {
            all = all.united(c.outline.boundingRect());
            wholeArea += c.area();
        }
        const QRectF left(all.left() - 1, all.top() - 1, all.width() / 2 + 1, all.height() + 2);
        region(m, "rectangle", {{"x0", QStringLiteral("%1[um]").arg(left.left(), 0, 'g', 12)},
                                {"y0", QStringLiteral("%1[um]").arg(left.top(), 0, 'g', 12)},
                                {"x1", QStringLiteral("%1[um]").arg(left.right(), 0, 'g', 12)},
                                {"y1", QStringLiteral("%1[um]").arg(left.bottom(), 0, 'g', 12)}});
        const rf::Processed half = rf::processRead(m, *read, QStringLiteral("heater.gds"));
        QVERIFY2(half.ok(), qPrintable(half.problems.join(" | ")));
        QVERIFY(!half.conductors.isEmpty());
        double halfArea = 0;
        for (const rf::Conductor& c : half.conductors) {
            halfArea += c.area();
            QVERIFY2(c.outline.boundingRect().right() <= left.right() + 1e-3,
                     qPrintable(QString::number(c.outline.boundingRect().right())));
        }
        QVERIFY2(halfArea < wholeArea * 0.95 && halfArea > 0, qPrintable(QStringLiteral("%1 of %2").arg(halfArea).arg(wholeArea)));
        QVERIFY2(half.report.cut + half.report.masked > 0, qPrintable(half.report.summary()));
        QVERIFY2(says(half.report.lines, "cutout"), qPrintable(half.report.summary()));

        set(*m.layout().child(QStringLiteral("rf_cutout")), "apply", false);
        const rf::Processed off = rf::processRead(m, *read, QStringLiteral("heater.gds"));
        QCOMPARE(off.conductors.size(), whole.conductors.size());
        double offArea = 0;
        for (const rf::Conductor& c : off.conductors) offArea += c.area();
        QCOMPARE(offArea, wholeArea);
        QVERIFY(!says(off.report.lines, "cutout"));
        // A region that cannot be read is the processing's problem too.
        set(*m.layout().child(QStringLiteral("rf_cutout")), "apply", true);
        set(m.layout().child(QStringLiteral("rf_cutout"))->children.at(0), "x1", QStringLiteral("nonsense"));
        QVERIFY(says(rf::processRead(m, *read, QStringLiteral("heater.gds")).problems, "is not a number"));
    }

    // End to end: a real layout file read, mapped and processed.
    void aLayoutFileIsReadAndProcessed()
    {
        const QString file = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/thermo_optic_heater.gds");
        if (!QFileInfo::exists(file)) QSKIP("the example layout is not in this build");
        rf::Model m = stacked();
        m.layout().props.insert(QStringLiteral("file"), file);

        // With no map, every shape is on a layer it does not name.
        rf::Processed none = rf::processLayout(m, QFileInfo(file).absolutePath());
        QCOMPARE(none.conductors.size(), 0);
        QVERIFY2(none.report.ignored > 0, qPrintable(QString::number(none.report.ignored)));
        QVERIFY2(says(none.report.lines, "the map does not name"), qPrintable(none.report.summary()));

        // Its layers sent to M1: the shapes come through as conductors.
        QJsonArray rows;
        for (int layer = 0; layer < 64; ++layer)
            rows.append(QJsonArray{QString::number(layer), QStringLiteral("0"), QStringLiteral("M1")});
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", rows);
        QString why;
        const rf::Processed done = rf::processLayout(m, QFileInfo(file).absolutePath(), &why);
        QVERIFY2(why.isEmpty(), qPrintable(why));
        QVERIFY2(!done.conductors.isEmpty(), qPrintable(done.report.summary() + QStringLiteral(" | ") + done.problems.join(" | ")));
        for (const rf::Conductor& c : done.conductors) {
            QCOMPARE(c.layer, QStringLiteral("M1"));
            QVERIFY(c.outline.size() >= 3);
            QVERIFY(c.area() > 0);
        }

        // A file that is not there, and a cell the file has not.
        m.layout().props.insert(QStringLiteral("file"), QStringLiteral("nothing.gds"));
        QVERIFY(!rf::processLayout(m, QFileInfo(file).absolutePath(), &why).ok());
        QVERIFY2(why.contains(QLatin1String("no file")), qPrintable(why));
        m.layout().props.insert(QStringLiteral("file"), file);
        m.layout().props.insert(QStringLiteral("cell"), QStringLiteral("no_such_cell"));
        QVERIFY2(says(rf::processLayout(m, QFileInfo(file).absolutePath()).problems, "has no cell"),
                 "a cell that is not there went unsaid");
    }

    // A layout already in memory processed, which is what a tab has: the
    // same shapes and the same report as reading the file again gives, and
    // the file read but once.
    void aLayoutInMemoryIsProcessedWithoutReadingItAgain()
    {
        const QString file = QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/thermo_optic_heater.gds");
        if (!QFileInfo::exists(file)) QSKIP("the example layout is not in this build");
        rf::Model m = stacked();
        m.layout().props.insert(QStringLiteral("file"), file);
        QJsonArray rows;
        for (int layer = 0; layer < 64; ++layer)
            rows.append(QJsonArray{QString::number(layer), QStringLiteral("0"), QStringLiteral("M1")});
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", rows);

        QString why;
        const std::shared_ptr<qucs_s::layout::Layout> read = qucs_s::layout::read(file, &why);
        QVERIFY2(read != nullptr, qPrintable(why));
        const rf::Processed held = rf::processRead(m, *read, QStringLiteral("heater.gds"));
        const rf::Processed fresh = rf::processLayout(m, QFileInfo(file).absolutePath());
        QVERIFY(!held.conductors.isEmpty());
        QCOMPARE(held.conductors.size(), fresh.conductors.size());
        QCOMPARE(held.report.read, fresh.report.read);
        QCOMPARE(held.report.ignored, fresh.report.ignored);
        QCOMPARE(held.report.lines, fresh.report.lines);
        QCOMPARE(held.problems, fresh.problems);
        for (int i = 0; i < held.conductors.size(); ++i) {
            QCOMPARE(held.conductors.at(i).layer, fresh.conductors.at(i).layer);
            QCOMPARE(held.conductors.at(i).outline, fresh.conductors.at(i).outline);
        }

        // A map that names one layer of the three: what it let through,
        // and what it left out, counted - and counted the same either way.
        QJsonArray one;   // (a braced list of one list is a copy of it to CI's AppleClang)
        one.append(QJsonArray{QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")});
        QCOMPARE(one.size(), 1);
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", one);
        const rf::Processed some = rf::processRead(m, *read, QStringLiteral("heater.gds"));
        QVERIFY2(some.report.ignored > 0, "the shapes the map does not name went uncounted");
        QCOMPARE(some.report.ignored, rf::processLayout(m, QFileInfo(file).absolutePath()).report.ignored);
        QVERIFY2(says(some.report.lines, "the map does not name"), qPrintable(some.report.summary()));
        QVERIFY(!some.conductors.isEmpty());
        QCOMPARE(some.byLayer().keys(), QStringList({QStringLiteral("M1")}));

        // A cell the file has not is said of the name it was given, not of
        // a path it never read.
        m.layout().props.insert(QStringLiteral("cell"), QStringLiteral("no_such_cell"));
        const rf::Processed wrong = rf::processRead(m, *read, QStringLiteral("heater.gds"));
        QVERIFY2(says(wrong.problems, "has no cell"), qPrintable(wrong.problems.join(" | ")));
        QVERIFY2(says(wrong.problems, "heater.gds"), qPrintable(wrong.problems.join(" | ")));
        QVERIFY(wrong.conductors.isEmpty());
    }
};

QTEST_MAIN(TestRf25Layout)
#include "test_rf25_layout.moc"
