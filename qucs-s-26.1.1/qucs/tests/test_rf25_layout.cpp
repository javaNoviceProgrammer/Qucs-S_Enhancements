/*
 * The shapes of an RF 2.5D model (rf25/rf25_layout.h): the layer map that
 * sends a file's layers to the stack's, and the processing steps that run
 * before meshing - merge, merge vias, drop slivers, snap, heal, remove
 * fill - each of which must say what it changed and must be possible to
 * turn off.
 */
#include <QtTest>

#include <cmath>
#include <functional>
#include <random>

#include "layout.h"
#include "rf25/rf25_layout.h"

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

        // A datatype left empty is 0; a layer sent twice, and one sent
        // where the stack has nothing, are said.
        rows.append(QJsonArray{QStringLiteral("31"), QString(), QStringLiteral("M2")});
        rows.append(QJsonArray{QStringLiteral("50"), QStringLiteral("0"), QStringLiteral("M9")});
        set(*m.layout().child(QStringLiteral("rf_layer_map")), "rows", rows);
        const rf::LayerMap again = rf::layerMapOf(m);
        QVERIFY2(says(again.problems, "two layers of the stack"), qPrintable(again.problems.join(" | ")));
        QVERIFY2(says(again.problems, "no layer of the stack"), qPrintable(again.problems.join(" | ")));
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
    }

    // Shapes that touch along part of an edge are one shape, however the
    // union's own sweep meets them. gdstk's union left two pieces apart in
    // about one set in sixty of touching rectangles - a
    // low-pass filter among them, its centre stub then cut from its line in
    // the full-wave solve with nothing said (B1 of the open items of 8
    // November) -: here one such set, whose fourth rectangle gdstk alone
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
    // metal's area is kept, which is the approximation the proposal names.
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
        QVERIFY2(says(done.report.lines, "of their area"), qPrintable(done.report.summary()));
        // The area it kept is the vias' own, within a few per cent.
        double got = 0;
        for (const rf::Conductor& c : done.conductors) got += c.area();
        QVERIFY2(std::abs(got - wanted) / wanted < 0.1,
                 qPrintable(QStringLiteral("%1 µm² of %2").arg(got).arg(wanted)));
        QVERIFY2(std::abs(done.report.areaKept - 1) < 0.1, qPrintable(QString::number(done.report.areaKept)));

        // Without that choice the union is left as it is - larger than the
        // vias were, which is the honest alternative.
        how.keepArea = false;
        double loose = 0;
        for (const rf::Conductor& c : rf::processShapes(shapes, {}, how, stack).conductors) loose += c.area();
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
        QCOMPARE(done.byLayer().value(QStringLiteral("M1")), 3);
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
