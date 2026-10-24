/*
 * The shapes of an RF 2.5D model (rf25/rf25_layout.h): the layer map that
 * sends a file's layers to the stack's, and the processing steps that run
 * before meshing - merge, merge vias, drop slivers, snap, heal, remove
 * fill - each of which must say what it changed and must be possible to
 * turn off.
 */
#include <QtTest>

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
};

QTEST_MAIN(TestRf25Layout)
#include "test_rf25_layout.moc"
