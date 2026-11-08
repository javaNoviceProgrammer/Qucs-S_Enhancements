/*
 * test_pen_reading.cpp - what Claude is told of the pen's strokes on a
 * layout (the review of 7 November, "Layout viewer pen markers: how Claude
 * reads them, and what would help"): the review's demo - two pads joined
 * by a bent 5 µm trace on 2/0, under a 300 x 140 µm frame on 10/0 - and
 * its three cases, a ring, an arrow and a freehand step, as the prompt has
 * them.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <gdstk/gdstk.hpp>

#include <cmath>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "layout.h"
#include "layoutdoc.h"
#include "main.h"
#include "markup.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "settings.h"
#include "strokeshape.h"

using namespace qucs_s::layout;
namespace markup = qucs_s::markup;

#pragma GCC diagnostic ignored "-Wmissing-braces"

namespace {

gdstk::Cell* newCell(gdstk::Library& lib, const char* name)
{
    auto* c = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    c->init(name);
    lib.cell_array.append(c);
    return c;
}

void addRectangle(gdstk::Cell* c, gdstk::Vec2 a, gdstk::Vec2 b, uint32_t layer)
{
    auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
    *p = gdstk::rectangle(a, b, gdstk::make_tag(layer, 0));
    c->polygon_array.append(p);
}

void place(gdstk::Cell* parent, gdstk::Cell* child, gdstk::Vec2 at)
{
    auto* r = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
    r->init(child);
    r->origin = at;
    parent->reference_array.append(r);
}

/*
 * The review's demo, in µm on a grid of 1 nm - three cells:
 *  PAD:   a square 20 on 1/0, (-10, -10) to (10, 10).
 *  TRACE: a path 5 wide on 2/0 through (-50, 0), (-20, 0), (0, 40),
 *         (20, 0), (50, 0) - flat, a peak at (0, 40), flat again.
 *  TOP:   a frame on 10/0, (-150, -70) to (150, 70); PAD at (-60, 0) and
 *         at (60, 0); TRACE at (0, 0).
 */
void writeDemo(const QString& path)
{
    gdstk::Library lib = {};
    lib.init("DEMO", 1e-6, 1e-9);
    gdstk::Cell* pad = newCell(lib, "PAD");
    addRectangle(pad, {-10, -10}, {10, 10}, 1);
    gdstk::Cell* trace = newCell(lib, "TRACE");
    auto* wire = static_cast<gdstk::FlexPath*>(gdstk::allocate_clear(sizeof(gdstk::FlexPath)));
    wire->num_elements = 1;
    wire->elements = static_cast<gdstk::FlexPathElement*>(gdstk::allocate_clear(sizeof(gdstk::FlexPathElement)));
    wire->init({-50, 0}, 5, 0, 0.001, gdstk::make_tag(2, 0));
    wire->simple_path = true;
    wire->elements[0].end_type = gdstk::EndType::Flush;
    const gdstk::Vec2 spine[] = {{-20, 0}, {0, 40}, {20, 0}, {50, 0}};
    gdstk::Array<gdstk::Vec2> points = {};
    for (const gdstk::Vec2& v : spine) points.append(v);
    wire->segment(points, nullptr, nullptr, false);
    points.clear();
    trace->flexpath_array.append(wire);
    gdstk::Cell* top = newCell(lib, "TOP");
    addRectangle(top, {-150, -70}, {150, 70}, 10);
    place(top, pad, {-60, 0});
    place(top, pad, {60, 0});
    place(top, trace, {0, 0});
    QVERIFY(lib.write_gds(QFile::encodeName(path).constData(), 0, nullptr) == gdstk::ErrorCode::NoError);
    lib.free_all();
}

markup::Stroke stroke(const QPolygonF& points, double width = 1.0)
{
    markup::Stroke s;
    s.on = QStringLiteral("TOP");
    s.points = points;
    s.width = width;
    s.color = QColor(0xe0, 0x1b, 0x24);
    return s;
}

/// A hand's wobble: the same every run, a few tenths of a µm.
QPointF wobble(int k, double size = 0.3)
{
    return QPointF(size * std::sin(k * 1.7 + 0.3), size * std::cos(k * 2.3 + 1.1));
}

/// The points along \a corners every \a step, with a hand's wobble.
QPolygonF drawnThrough(const QPolygonF& corners, double step = 0.5, double shake = 0.3)
{
    QPolygonF out;
    int k = 0;
    for (int i = 1; i < corners.size(); ++i) {
        const QLineF seg(corners.at(i - 1), corners.at(i));
        const int n = std::max(1, int(std::ceil(seg.length() / step)));
        for (int j = (i == 1 ? 0 : 1); j <= n; ++j) out << seg.pointAt(double(j) / n) + wobble(k++, shake);
    }
    return out;
}

/// The review's three strokes: a ring round the trace's peak, an arrow at it
/// from the upper right, and a freehand step over the trace ("make it look
/// like this"), its top right corner a little rounded.
markup::Stroke ringAtThePeak()
{
    QPolygonF circle;
    for (int k = 0; k < 46; ++k) circle << QPointF(0.5, 39) + 9 * QPointF(std::cos(2 * M_PI * k / 48 + 0.4), std::sin(2 * M_PI * k / 48 + 0.4)) + wobble(k, 0.2);
    return stroke(circle);
}

markup::Stroke arrowAtThePeak()
{
    QPolygonF shaft = drawnThrough({QPointF(36, 63), QPointF(3, 44)}, 1.0, 0.15);
    shaft << QPointF(4.2, 47.5) << QPointF(5.0, 49.8);   // back along a barb, at one go
    return stroke(shaft);
}

markup::Stroke freehandStep()
{
    // Its corners as meant: along the trace, up at x = -33, across at 40,
    // down at 36 (the top right rounded, some 6 µm), along to the pad.
    QPolygonF corners{QPointF(-50.4, 0.4), QPointF(-33.2, 0.4), QPointF(-33.2, 40.3)};
    const QPointF centre(36.1 - 6, 40.3 - 6);
    for (int k = 0; k <= 6; ++k) corners << centre + 6 * QPointF(std::sin(M_PI / 2 * k / 6), std::cos(M_PI / 2 * k / 6));
    corners << QPointF(36.1, 0.3) << QPointF(59, 0.3);
    return stroke(drawnThrough(corners));
}

/// \a path's cell \a name read back with gdstk: its paths' spines and
/// widths, its polygons, texts and placements, in µm.
struct ReadBack {
    QList<QPolygonF> spines;
    QList<double> widths;
    QList<QPair<QString, QPolygonF>> polygons;   // layer, points
    QStringList texts;
    QStringList placements;
    bool ok = false;
};

ReadBack readBack(const QString& path, const char* name)
{
    ReadBack out;
    gdstk::ErrorCode error = gdstk::ErrorCode::NoError;
    gdstk::Library lib = gdstk::read_gds(QFile::encodeName(path).constData(), 1e-6, 0, nullptr, &error);
    gdstk::Cell* cell = lib.get_cell(name);
    if (cell != nullptr) {
        out.ok = true;
        for (uint64_t i = 0; i < cell->flexpath_array.count; ++i) {
            const gdstk::FlexPath* p = cell->flexpath_array[i];
            QPolygonF spine;
            for (uint64_t k = 0; k < p->spine.point_array.count; ++k) spine << QPointF(p->spine.point_array[k].x, p->spine.point_array[k].y);
            out.spines << spine;
            out.widths << 2 * p->elements[0].half_width_and_offset[0].u;
        }
        for (uint64_t i = 0; i < cell->polygon_array.count; ++i) {
            const gdstk::Polygon* p = cell->polygon_array[i];
            QPolygonF points;
            for (uint64_t k = 0; k < p->point_array.count; ++k) points << QPointF(p->point_array[k].x, p->point_array[k].y);
            out.polygons << qMakePair(QStringLiteral("%1/%2").arg(gdstk::get_layer(p->tag)).arg(gdstk::get_type(p->tag)), points);
        }
        for (uint64_t i = 0; i < cell->label_array.count; ++i) out.texts << QString::fromUtf8(cell->label_array[i]->text);
        for (uint64_t i = 0; i < cell->reference_array.count; ++i) {
            const gdstk::Reference* r = cell->reference_array[i];
            out.placements << QStringLiteral("%1 %2 %3").arg(QString::fromUtf8(r->cell != nullptr ? r->cell->name : r->name)).arg(r->origin.x).arg(r->origin.y);
        }
    }
    lib.free_all();
    return out;
}

QByteArray contents(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void waitForReads()
{
    QElapsedTimer t;
    t.start();
    while (readsGoing() > 0 && t.elapsed() < 120000) QTest::qWait(20);
}

} // namespace

class TestPenReading : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    QJsonObject call(const QString& tool, const QJsonObject& args = {}) { return control->callNow(tool, args, 120000); }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r) { return QJsonDocument::fromJson(text(r).toUtf8()).object(); }

    LayoutDoc* open(const QString& file)
    {
        if (!app->gotoPage(file)) return nullptr;
        auto* doc = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        if (doc != nullptr && !doc->waitForLoaded(120000)) return nullptr;
        return doc;
    }
    /// The demo open, TOP shown, nothing drawn.
    LayoutDoc* demo()
    {
        LayoutDoc* doc = open(path("layout/demo.gds"));
        if (doc == nullptr) return nullptr;
        doc->showCell(doc->layout()->cellIndex("TOP"));
        doc->view()->clearDrawing(true);
        return doc;
    }
    /// What a prompt says of \a doc's drawing now.
    QString told(LayoutDoc* doc) { return app->drawingsOf(doc->getDocName()).second; }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace/layout"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        Module::registerModules();
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        writeDemo(path("layout/demo.gds"));
    }

    // Each test with the demo opened afresh: a tab's strokes, and their
    // numbers, are its own.
    void cleanup()
    {
        app->closeAllFiles();
        waitForReads();
    }

    void cleanupTestCase()
    {
        app->closeAllFiles();
        waitForReads();
        delete app;
        QucsMain = nullptr;
    }

    // The review's three cases, as the prompt tells them: a ring round the
    // trace's peak says what it holds there, the vertex and its angle; an
    // arrow says the feature its tip is nearest, the way it comes from, its
    // tip and tail, and its few points; a freehand step says how it
    // follows the trace and leaves it, and its polyline - its runs, snapped
    // to the trace and to round values, its rounded corner, its symmetry.
    // The frame under all of them is set apart, not what they mark.
    void theReviewsCasesAreToldWhatTheyAreAbout()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        doc->view()->addStroke(ringAtThePeak());
        doc->view()->addStroke(arrowAtThePeak());
        doc->view()->addStroke(freehandStep());
        qInfo().noquote() << told(doc);   // (what the prompt says, whole)
        const QStringList lines = told(doc).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 3);
        const auto upToBox = [](const QString& line) { return line.left(line.indexOf(QStringLiteral(" (its box "))); };
        QCOMPARE(upToBox(lines.at(0)), QStringLiteral("cell TOP, stroke 1: a red pen ring around the vertex at (0, 40) of the 2/0 path in TRACE, a 53° "
                                                      "corner, all of it over 1 shape(s) on 10/0 that covers it"));
        QCOMPARE(upToBox(lines.at(1)), QStringLiteral("cell TOP, stroke 2: a red pen arrow pointing at 1 shape(s) on 2/0, 1 placement(s) of TRACE, "
                                                      "nearest its tip the vertex at (0, 40) of the 2/0 path in TRACE, a 53° corner, coming from "
                                                      "the upper right, its tip at (2.9, 43.9), its tail at (36, 63.1), all of it over 1 shape(s) "
                                                      "on 10/0 that covers it"));
        // (Three points: said, so read_drawings is not needed.)
        QVERIFY2(lines.at(1).contains(QStringLiteral(" µm); its points (36.044, 63.068) (2.912, 43.857) (5, 49.8)")), qPrintable(lines.at(1)));
        QCOMPARE(upToBox(lines.at(2)),
                 QStringLiteral("cell TOP, stroke 3: a red pen mark on 2 shape(s) on 1/0, 1 shape(s) on 2/0, 2 placement(s) of PAD, 1 placement(s) "
                                "of TRACE, following the 2/0 path in TRACE from (-49.6, 0) to (50, 0), leaving it between (-33.8, 0) and "
                                "(36.7, 0) to go 40.6 µm above it, all of it over 1 shape(s) on 10/0 that covers it, read as a polyline of 5 "
                                "run(s): horizontal at y ≈ 0.4 (±0.3, snaps to 0: the 2/0 path's centre line) from x = -50.3 to -33.2; "
                                "vertical at x ≈ -33.2 (±0.3, snaps to -33: a 1 µm grid) up to y = 40.3; horizontal at y ≈ 40.3 (±0.3, "
                                "snaps to 40: the 2/0 path's vertex at (0, 40)) to x = 36.1, the corner there rounded (radius ≈ 5.7); "
                                "vertical at x ≈ 36.1 (±0.3, snaps to 36: a 1 µm grid) down to y = 0.3; horizontal at y ≈ 0.3 (±0.3, "
                                "snaps to 0: the 2/0 path's centre line) to x = 58.8; its corners symmetric about x ≈ 1.4 within 0.1 µm, "
                                "its ends not; snapped, its points (-50, 0) (-33, 0) (-33, 40) (36, 40) (36, 0) (59, 0) (its start to the "
                                "2/0 path's end at (-50, 0))"));
        // (Its corners given, its points are not: 400 of them.)
        QVERIFY(!lines.at(2).contains(QStringLiteral(" µm); its points")));
        // read_drawings has the same as fields.
        const QJsonArray read = json(call("read_drawings", {{"path", "demo.gds"}})).value("documents").toArray().at(0).toObject().value("strokes").toArray();
        QCOMPARE(read.size(), 3);
        const QJsonObject ring = read.at(0).toObject(), arrow = read.at(1).toObject(), step = read.at(2).toObject();
        QCOMPARE(ring.value("over").toObject(), (QJsonObject{{"shapes", QJsonObject{{"10/0", 1}}}}));
        QVERIFY(!ring.contains("around"));
        QCOMPARE(ring.value("holds part of").toArray(),
                 (QJsonArray{QJsonObject{{"layer", "2/0"}, {"kind", "path"}, {"in", "TRACE"}, {"vertex", QJsonArray{0, 40}}, {"angle", 53}}}));
        QCOMPARE(arrow.value("points at").toObject(), (QJsonObject{{"shapes", QJsonObject{{"2/0", 1}}}, {"cells", QJsonObject{{"TRACE", 1}}}}));
        QVERIFY(!arrow.contains("from"));   // (its tail is on the frame alone)
        QCOMPARE(arrow.value("tip").toArray(), (QJsonArray{2.9, 43.9}));
        QCOMPARE(arrow.value("comes from").toString(), QStringLiteral("the upper right"));
        QCOMPARE(arrow.value("nearest its tip").toObject().value("vertex").toArray(), (QJsonArray{0, 40}));
        const QJsonObject polyline = step.value("polyline").toObject();
        QCOMPARE(polyline.value("snapped corners").toArray(),
                 (QJsonArray{QJsonArray{-50, 0}, QJsonArray{-33, 0}, QJsonArray{-33, 40}, QJsonArray{36, 40}, QJsonArray{36, 0}, QJsonArray{59, 0}}));
        QCOMPARE(polyline.value("corner angles").toArray(), (QJsonArray{90, 90, 90, 90}));
        QCOMPARE(polyline.value("rounded").toArray().size(), 1);
        QCOMPARE(polyline.value("symmetric").toObject().value("about").toString(), QStringLiteral("x"));
        const QJsonObject follows = step.value("follows").toArray().at(0).toObject();
        QCOMPARE(follows.value("leaves").toArray().at(0).toObject().value("side").toString(), QStringLiteral("above"));
    }

    // A shape much larger than the stroke under all of it is set apart only
    // when the stroke marks something besides: a dot on a pad, under the
    // frame, is on the pad; one on the frame alone is on the frame, and
    // nothing is set apart. A ring round a pad holds it.
    void aLargeShapeIsSetApartOnlyWhenSomethingElseIsMarked()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        doc->view()->addStroke(stroke({QPointF(-62, 3)}));
        doc->view()->addStroke(stroke({QPointF(100, -50)}));
        QPolygonF round;
        for (int k = 0; k < 30; ++k) round << QPointF(60, 0) + 16 * QPointF(std::cos(2 * M_PI * k / 32), std::sin(2 * M_PI * k / 32));
        doc->view()->addStroke(stroke(round));
        const QJsonArray read = doc->view()->readDrawing(false);
        QCOMPARE(read.at(0).toObject().value("on").toObject(), (QJsonObject{{"shapes", QJsonObject{{"1/0", 1}}}, {"cells", QJsonObject{{"PAD", 1}}}}));
        QCOMPARE(read.at(0).toObject().value("over").toObject(), (QJsonObject{{"shapes", QJsonObject{{"10/0", 1}}}}));
        QCOMPARE(read.at(1).toObject().value("on").toObject(), (QJsonObject{{"shapes", QJsonObject{{"10/0", 1}}}}));
        QVERIFY(!read.at(1).toObject().contains("over"));
        QCOMPARE(read.at(2).toObject().value("around").toObject(), (QJsonObject{{"shapes", QJsonObject{{"1/0", 1}}}, {"cells", QJsonObject{{"PAD", 1}}}}));
        QVERIFY(!read.at(2).toObject().contains("polyline"));   // (a circle is no polygon)
    }

    // An arrow whose tip is beside a polygon's edge, not near a corner: the
    // edge, and which side the tip is on; one at a text, the text.
    void anArrowsTipIsNearestAnEdgeOrAText()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        // At the right pad's top edge, from above, from (60, 40) to (61, 12).
        doc->view()->addStroke(stroke({QPointF(61, 40), QPointF(60, 12), QPointF(58, 15)}));
        const QJsonObject arrow = doc->view()->readDrawing(false).at(0).toObject();
        QCOMPARE(arrow.value("drawn as").toString(), QStringLiteral("arrow"));
        const QJsonObject nearest = arrow.value("nearest its tip").toObject();
        QCOMPARE(nearest.value("edge").toArray(), (QJsonArray{QJsonArray{70, 10}, QJsonArray{50, 10}}));
        QCOMPARE(nearest.value("side").toString(), QStringLiteral("above"));
        QCOMPARE(nearest.value("in").toString(), QStringLiteral("PAD"));
        QCOMPARE(arrow.value("comes from").toString(), QStringLiteral("above"));
        QVERIFY2(told(doc).contains(QStringLiteral("nearest its tip the edge from (70, 10) to (50, 10) of the 1/0 polygon in PAD, from above, coming from above")),
                 qPrintable(told(doc)));
        // A tip on the trace's flank just under its peak - the flank's
        // segment a little nearer than the vertex -: the vertex, which is
        // what one points at there.
        doc->view()->addStroke(stroke({QPointF(30, 60), QPointF(3, 36), QPointF(6.5, 37.5)}));
        const QJsonObject flank = doc->view()->readDrawing(false).at(1).toObject();
        QCOMPARE(flank.value("drawn as").toString(), QStringLiteral("arrow"));
        QCOMPARE(flank.value("nearest its tip").toObject().value("vertex").toArray(), (QJsonArray{0, 40}));
    }

    // A stroke keeps its number: one taken away leaves a gap, the next is
    // numbered after the last, and undoing brings one back as it was.
    // Prompts and read_drawings say the same numbers; an arrowhead names its
    // arrow by its number; the router's from_stroke finds a stroke by it.
    void aStrokeKeepsItsNumber()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->addStroke(stroke({QPointF(-62, 3)}));
        view->addStroke(stroke({QPointF(62, 3)}));
        view->addStroke(stroke({QPointF(0, 60)}));
        QVERIFY(view->drawing().strokes().size() == 3);
        markup::Drawing copy = view->drawing();
        QVERIFY(copy.eraseAt(QStringLiteral("TOP"), QPointF(62, 3), 0.5));
        QCOMPARE(copy.strokes().size(), 2);
        // Through the view: the eraser's drag as the user's.
        QList<int> ids;
        for (const markup::Stroke& s : view->drawing().strokes()) ids << s.id;
        QCOMPARE(ids, (QList<int>{1, 2, 3}));
        view->clearDrawing(true);
        QVERIFY(view->undoDrawing());
        view->addStroke(stroke({QPointF(100, 50), QPointF(120, 50)}));
        view->addStroke(stroke({QPointF(117, 52), QPointF(120, 50), QPointF(117, 48)}));
        ids.clear();
        for (const markup::Stroke& s : view->drawing().strokes()) ids << s.id;
        QCOMPARE(ids, (QList<int>{1, 2, 3, 4, 5}));
        QVERIFY(view->drawing().find(QStringLiteral("TOP"), 4) != nullptr);
        QVERIFY(view->drawing().find(QStringLiteral("TOP"), 6) == nullptr);
        QVERIFY(view->drawing().find(QStringLiteral("INV"), 1) == nullptr);
        // Taken away: 2 goes, the others keep theirs; the next is 6.
        markup::Drawing& drawing = const_cast<markup::Drawing&>(view->drawing());
        QVERIFY(drawing.eraseAt(QStringLiteral("TOP"), QPointF(62, 3), 0.5));
        view->addStroke(stroke({QPointF(-100, -50)}));
        QJsonArray read = view->readDrawing(false);
        QList<int> said;
        for (const QJsonValue& v : read) said << v.toObject().value("stroke").toInt();
        QCOMPARE(said, (QList<int>{1, 3, 4, 5, 6}));
        QCOMPARE(read.at(3).toObject().value("drawn as").toString(), QStringLiteral("arrowhead"));
        QCOMPARE(read.at(3).toObject().value("of stroke").toInt(), 4);
        const QString prompt = told(doc);
        QVERIFY2(prompt.contains(QStringLiteral("cell TOP, stroke 5: a red pen arrowhead of stroke 4")) && prompt.contains(QStringLiteral("cell TOP, stroke 6: ")),
                 qPrintable(prompt));
        // The router's from_stroke by its number.
        QJsonObject r = call("route_setup", {{"path", "demo.gds"}, {"keep_outs", QJsonArray{QJsonObject{{"from_stroke", 2}}}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("numbered 1, 3, 4, 5, 6")), qPrintable(text(r)));
    }

    // Few points, said in the prompt; many, not (read_drawings has them).
    // And the pen still taken up, said with the drawing.
    void aFewPointsAndThePenAreSaid()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        doc->view()->addStroke(stroke({QPointF(100, 30), QPointF(130, 50)}));
        QString prompt = told(doc);
        QVERIFY2(prompt.endsWith(QStringLiteral("µm); its points (100, 30) (130, 50)")), qPrintable(prompt));
        QVERIFY(!prompt.contains(QStringLiteral("The pen is still taken up")));
        QPolygonF scribble;
        for (int k = 0; k < 200; ++k) scribble << QPointF(-120 + k * 0.4, -40 + 12 * std::sin(k * 0.7) + 5 * std::cos(k * 1.9));
        doc->view()->addStroke(stroke(scribble));
        QVERIFY(!failed(call("show_layout", {{"path", "demo.gds"}, {"pen", true}})));
        prompt = told(doc);
        const QStringList lines = prompt.split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 3);
        QVERIFY2(!lines.at(1).contains(QStringLiteral("its points")) && !lines.at(1).contains(QStringLiteral("polyline")), qPrintable(lines.at(1)));
        QVERIFY2(lines.at(2).startsWith(QStringLiteral("(The pen is still taken up in its tab: a drag there draws rather than pans; show_layout's pen: false")),
                 qPrintable(lines.at(2)));
        QVERIFY(!failed(call("show_layout", {{"path", "demo.gds"}, {"pen", false}})));
    }

    // The polyline reading on strokes of its own: snapped to what is near,
    // else to round values; its rounded corners and their radii; near
    // symmetry, and none where there is none; a closed one, a rectangle; a
    // circle, a curve and a scribble none.
    void aFreehandStrokeIsReadAsAPolyline()
    {
        using markup::polylineOf;
        markup::Snaps none;
        none.finest = 0.001;
        // A level line at y 10.3, a hand's wobble: a 10 µm grid's 10; with
        // an edge at 10.2, the edge.
        const QPolygonF level = drawnThrough({QPointF(0, 10.3), QPointF(80, 10.3)});
        std::optional<markup::Polyline> pl = polylineOf(level, 1, false, none);
        QVERIFY(pl.has_value());
        QCOMPARE(pl->runs.size(), 1);
        QCOMPARE(pl->runs.at(0).way, markup::Polyline::Way::Horizontal);
        QVERIFY(std::abs(pl->runs.at(0).at - 10.3) < 0.15);
        QCOMPARE(pl->runs.at(0).snappedAt, 10.0);
        QCOMPARE(pl->runs.at(0).snappedTo, QStringLiteral("a 10 µm grid"));
        markup::Snaps edge = none;
        edge.ys << markup::Snap{10.2, QStringLiteral("the edge")} << markup::Snap{14, QStringLiteral("too far")};
        edge.xs << markup::Snap{80.4, QStringLiteral("its end")};
        pl = polylineOf(level, 1, false, edge);
        QCOMPARE(pl->runs.at(0).snappedAt, 10.2);
        QCOMPARE(pl->runs.at(0).snappedTo, QStringLiteral("the edge"));
        QCOMPARE(pl->snapped.last(), QPointF(80.4, 10.2));
        QCOMPARE(pl->endsSnappedTo.at(1), QStringLiteral("its end"));
        // A U, symmetric about x = 25 within its wobble; one side moved 6
        // µm: not. Its corners sharp.
        const QPolygonF u = drawnThrough({QPointF(0, 40), QPointF(0, 0), QPointF(50, 0), QPointF(50, 40)});
        pl = polylineOf(u, 1, false, none);
        QVERIFY(pl.has_value());
        QCOMPARE(pl->runs.size(), 3);
        QCOMPARE(pl->axis, QStringLiteral("x"));
        QVERIFY(std::abs(pl->axisAt - 25) < 0.5);
        QVERIFY(pl->endsToo);
        QCOMPARE(pl->radii, (QList<double>{0, 0}));
        QVERIFY(std::abs(pl->angles.at(0) - 90) < 2 && std::abs(pl->angles.at(1) - 90) < 2);
        const QPolygonF lopsided = drawnThrough({QPointF(0, 40), QPointF(0, 0), QPointF(50, 0), QPointF(56, 40), QPointF(80, 40)});
        pl = polylineOf(lopsided, 1, false, none);
        QVERIFY(pl.has_value());
        QVERIFY2(pl->axis.isEmpty(), qPrintable(pl->axis));
        QCOMPARE(pl->runs.at(2).way, markup::Polyline::Way::Slanted);
        // An L rounded with a radius of 8 at its corner: about 8.
        QPolygonF el{QPointF(0, 60), QPointF(0, 8)};
        for (int k = 1; k < 8; ++k) el << QPointF(8, 8) - 8 * QPointF(std::cos(M_PI / 2 * k / 8), std::sin(M_PI / 2 * k / 8));
        el << QPointF(8, 0) << QPointF(60, 0);
        pl = polylineOf(drawnThrough(el), 1, false, none);
        QVERIFY(pl.has_value());
        QCOMPARE(pl->runs.size(), 2);
        QVERIFY2(std::abs(pl->radii.at(0) - 8) < 1.2, qPrintable(QString::number(pl->radii.at(0))));
        // A flick where the pen came down, and one where it lifted: dropped.
        pl = polylineOf(drawnThrough({QPointF(-2.5, 62), QPointF(0, 60), QPointF(0, 0), QPointF(60, 0)}), 1, false, none);
        QVERIFY(pl.has_value());
        QCOMPARE(pl->runs.size(), 2);
        pl = polylineOf(drawnThrough({QPointF(0, 60), QPointF(0, 0), QPointF(60, 0), QPointF(58, 2.5)}), 1, false, none);
        QVERIFY(pl.has_value());
        QCOMPARE(pl->runs.size(), 2);
        // A diagonal run.
        pl = polylineOf(drawnThrough({QPointF(0, 0), QPointF(40, 0), QPointF(70, 30)}), 1, false, none);
        QVERIFY(pl.has_value());
        QCOMPARE(pl->runs.at(1).way, markup::Polyline::Way::Diagonal);
        // A rectangle drawn as a ring: closed, four runs, square corners,
        // snapped to whole µm.
        QPolygonF rect = drawnThrough({QPointF(10.2, 5.3), QPointF(49.8, 5.3), QPointF(49.8, 30.4), QPointF(10.2, 30.4), QPointF(10.2, 6)});
        pl = polylineOf(rect, 1, true, none);
        QVERIFY(pl.has_value());
        QVERIFY(pl->closed);
        QCOMPARE(pl->runs.size(), 4);
        QCOMPARE(pl->snapped.size(), 4);
        QPolygonF corners = pl->snapped;
        std::sort(corners.begin(), corners.end(), [](QPointF a, QPointF b) { return a.x() != b.x() ? a.x() < b.x() : a.y() < b.y(); });
        QCOMPARE(corners, (QPolygonF{QPointF(10, 5), QPointF(10, 30), QPointF(50, 5), QPointF(50, 30)}));
        // A circle, an arc and a scribble: none.
        QPolygonF circle, arc, scribble;
        for (int k = 0; k < 60; ++k) circle << 20 * QPointF(std::cos(2 * M_PI * k / 62), std::sin(2 * M_PI * k / 62)) + wobble(k, 0.2);
        for (int k = 0; k <= 60; ++k) arc << 30 * QPointF(std::cos(M_PI * k / 60), std::sin(M_PI * k / 60)) + wobble(k, 0.2);
        for (int k = 0; k < 200; ++k) scribble << QPointF(k * 0.4, 12 * std::sin(k * 0.7) + 5 * std::cos(k * 1.9));
        QVERIFY(!polylineOf(circle, 1, true, none).has_value());
        QVERIFY(!polylineOf(arc, 1, false, none).has_value());
        QVERIFY(!polylineOf(scribble, 1, false, none).has_value());
        // Too small for its breadth, or nothing: none.
        QVERIFY(!polylineOf({QPointF(0, 0), QPointF(1, 1)}, 1, false, none).has_value());
        QVERIFY(!polylineOf({}, 1, false, none).has_value());
        // In words.
        pl = polylineOf(u, 1, false, none);
        const QString said = markup::polylineSaid(markup::polylineJson(*pl));
        QVERIFY2(said.startsWith(QStringLiteral("read as a polyline of 3 run(s): vertical at x ≈ ")) && said.contains(QStringLiteral("its ends too")), qPrintable(said));
        // The round values.
        double step = 0;
        QCOMPARE(markup::onRoundGrid(-33.2, 0.5, 0.001, &step), -33.0);
        QCOMPARE(step, 1.0);
        QCOMPARE(markup::onRoundGrid(0.1234, 0.0004, 0.001, &step), 0.123);
        QCOMPARE(markup::onRoundGrid(0.1234, 0.00001, 0.001, &step), 0.123);   // (no finer than the database's unit)
    }

    // Claude's overlay: drawn over the cell shown in its own colour, in the
    // picture a screenshot takes, and kept apart - the pen's strokes, their
    // reading and the router's overlays are as without it; replaced by the
    // next, shown with its cell alone, taken away by 'clear' or the tab's
    // button, which shows while there is one. Each wrong item refused, and
    // nothing drawn.
    void claudesOverlayIsKeptApartFromTheStrokes()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->addStroke(freehandStep());
        const QString before = told(doc);
        const quint64 revision = view->drawing().revision();
        QVERIFY(!doc->sketchButton()->isVisibleTo(doc));
        const QJsonArray step{QJsonArray{-50, 0}, QJsonArray{-33, 0}, QJsonArray{-33, 40}, QJsonArray{36, 40}, QJsonArray{36, 0}, QJsonArray{50, 0}};
        QJsonObject r = call("show_layout", {{"path", "demo.gds"}, {"region", QJsonArray{-80, -30, 80, 60}},
                                             {"overlay", QJsonArray{QJsonObject{{"path", step}, {"width", 5}},
                                                                    QJsonObject{{"arrow", QJsonArray{QJsonArray{60, 50}, QJsonArray{37, 41}}}, {"text", "5 µm step"}},
                                                                    QJsonObject{{"text", "here"}, {"at", QJsonArray{0, 50}}},
                                                                    QJsonObject{{"polygon", QJsonArray{QJsonArray{-70, -20}, QJsonArray{-60, -20}, QJsonArray{-60, -15}}}, {"colour", "orange"}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject drawn = json(r).value("overlay").toObject();
        QCOMPARE(drawn.value("paths").toInt(), 1);
        QCOMPARE(drawn.value("arrows").toInt(), 1);
        QCOMPARE(drawn.value("texts").toInt(), 1);
        QCOMPARE(drawn.value("polygons").toInt(), 1);
        QCOMPARE(view->sketch(QStringLiteral("TOP")).size(), 4);
        QVERIFY(doc->sketchButton()->isVisibleTo(doc));
        // Apart: the strokes, their reading, the prompt and the overlays
        // another tab draws are as they were.
        QCOMPARE(view->drawing().strokes().size(), 1);
        QCOMPARE(view->drawing().revision(), revision);
        QCOMPARE(told(doc), before);
        QVERIFY(view->overlays().isEmpty());
        view->setOverlays({});   // (the router's taking its own away leaves Claude's)
        QCOMPARE(view->sketch(QStringLiteral("TOP")).size(), 4);
        // In the picture: Claude's teal along the new step's top, at (0, 40)
        // - where the trace's peak is too -, and not on the old trace's flank.
        const QImage picture = view->picture();
        const auto teal = [&](QPointF at) {
            const QPoint px = (view->toPixel(at) * picture.devicePixelRatio()).toPoint();
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy) {
                    const QColor c = picture.pixelColor(px + QPoint(dx, dy));
                    if (c.blue() > c.red() + 40 && c.green() > c.red() + 40) return true;
                }
            return false;
        };
        QVERIFY(teal(QPointF(15, 40)));
        QVERIFY(teal(QPointF(-33, 20)));
        QVERIFY(!teal(QPointF(-10, 20)));
        // Replaced by the next; another cell has none of it.
        QVERIFY(!failed(call("show_layout", {{"path", "demo.gds"}, {"overlay", QJsonArray{QJsonObject{{"text", "only"}, {"at", QJsonArray{0, 0}}}}}})));
        QCOMPARE(view->sketch(QStringLiteral("TOP")).size(), 1);
        doc->showCell(doc->layout()->cellIndex("PAD"));
        QVERIFY(view->sketch(view->drawingPart()).isEmpty());
        QVERIFY(!json(call("show_layout", {{"path", "demo.gds"}})).contains("overlay"));
        doc->showCell(doc->layout()->cellIndex("TOP"));
        // Taken away by the tab's button.
        QTest::mouseClick(doc->sketchButton(), Qt::LeftButton);
        QVERIFY(!view->hasSketch() && !doc->sketchButton()->isVisibleTo(doc));
        QVERIFY(!failed(call("show_layout", {{"path", "demo.gds"}, {"overlay", QJsonArray{QJsonObject{{"text", "again"}, {"at", QJsonArray{0, 0}}}}}})));
        r = call("show_layout", {{"path", "demo.gds"}, {"overlay", "clear"}});
        QVERIFY(!view->hasSketch());
        QCOMPARE(json(r).value("overlay").toString(), QStringLiteral("taken away"));
        // Refused, each, and nothing drawn.
        QJsonArray onePoint;   // (a list of one list, by append: check-json-nesting.py)
        onePoint.append(QJsonArray{0, 0});
        const QList<QJsonValue> wrong{QJsonValue(5), QJsonValue(QJsonArray{}), QJsonValue("wipe"), QJsonValue(QJsonArray{5}),
                                      QJsonValue(QJsonArray{QJsonObject{{"polygon", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"path", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"path", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}, {"width", -1}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"arrow", onePoint}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"text", "x"}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"text", ""}, {"at", QJsonArray{0, 0}}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"at", QJsonArray{0, 0}}, {"text", "x"}, {"colour", "nocolour"}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"at", QJsonArray{0, 0}}, {"text", "x"}, {"layer", "1/0"}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"polygon", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}, QJsonArray{1, 0}}}, {"width", 2}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"polygon", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}, QJsonArray{1, 0}}},
                                                                        {"arrow", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}}}),
                                      QJsonValue(QJsonArray{QJsonObject{{"polygon", QJsonArray{QJsonArray{0, 0}, QJsonArray{1e300, 1}, QJsonArray{1, 0}}}}})};
        for (const QJsonValue& v : wrong) {
            r = call("show_layout", {{"path", "demo.gds"}, {"overlay", v}});
            QVERIFY2(failed(r), qPrintable(QJsonDocument(QJsonObject{{"overlay", v}}).toJson(QJsonDocument::Compact)));
            QVERIFY(!view->hasSketch());
        }
        // (An item's key it has not, named - by the tools' own check of the
        //  schema's items, before sketchItemOf()'s.)
        r = call("show_layout", {{"path", "demo.gds"}, {"overlay", QJsonArray{QJsonObject{{"at", QJsonArray{0, 0}}, {"text", "x"}, {"layer", "1/0"}}}}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("overlay[0] has no layer")), qPrintable(text(r)));
    }

    // A stroke marked addressed: faded, left out of the prompt (counted
    // there), still read by read_drawings with its mark; the pen's Undo
    // takes the mark away; a number the cell has not, refused, nothing
    // marked; none takes every mark away. All marked: the prompt says so,
    // not that the drawing was taken away.
    void aStrokeMarkedAddressedIsFadedAndLeftOut()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->addStroke(ringAtThePeak());
        view->addStroke(arrowAtThePeak());
        view->addStroke(freehandStep());
        QJsonObject r = call("show_layout", {{"path", "demo.gds"}, {"addressed", QJsonArray{9}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("no stroke 9 (its strokes: 1, 2, 3)")), qPrintable(text(r)));
        r = call("show_layout", {{"path", "demo.gds"}, {"addressed", QJsonArray{1, 3}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("addressed").toString().startsWith(QStringLiteral("2 stroke(s) marked addressed")));
        const QStringList lines = told(doc).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines.at(0).startsWith(QStringLiteral("cell TOP, stroke 2: a red pen arrow")));
        QCOMPARE(lines.at(1), QStringLiteral("(2 stroke(s) marked addressed, left out: cell TOP 1, cell TOP 3)"));
        const QJsonArray read = json(call("read_drawings", {{"path", "demo.gds"}, {"points", false}})).value("documents").toArray().at(0).toObject().value("strokes").toArray();
        QCOMPARE(read.at(0).toObject().value("addressed").toBool(), true);
        QVERIFY(!read.at(1).toObject().contains("addressed"));
        // Faded: the ring's red is a third of what it was.
        const markup::Stroke* ring = view->drawing().find(QStringLiteral("TOP"), 1);
        QVERIFY(ring != nullptr && ring->addressed);
        QVERIFY(view->undoDrawing());
        QVERIFY(!view->drawing().find(QStringLiteral("TOP"), 1)->addressed);
        QVERIFY(!failed(call("show_layout", {{"path", "demo.gds"}, {"addressed", QJsonArray{1, 2, 3}}})));
        QCOMPARE(told(doc), QStringLiteral("(3 stroke(s) marked addressed, left out: cell TOP 1, cell TOP 2, cell TOP 3)"));
        r = call("show_layout", {{"path", "demo.gds"}, {"addressed", "none"}});
        QVERIFY(json(r).value("addressed").toString().startsWith(QStringLiteral("3 stroke(s) no longer")));
        QCOMPARE(told(doc).split(QLatin1Char('\n')).size(), 3);
        for (const QJsonValue& v : {QJsonValue(QJsonArray{}), QJsonValue(QJsonArray{0}), QJsonValue(QJsonArray{1.5}), QJsonValue("all"), QJsonValue(QJsonArray{"1"})})
            QVERIFY(failed(call("show_layout", {{"path", "demo.gds"}, {"addressed", v}})));
    }

    // The overlay's items, as a fuzzer gives them: each refused or drawn,
    // never a crash.
    void anyOverlayIsRefusedOrDrawn()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        QRandomGenerator random(7112026);
        const auto any = [&random](auto&& self, int depth) -> QJsonValue {
            switch (random.bounded(depth > 2 ? 4 : 7)) {
            case 0: return QJsonValue(random.generateDouble() * 2e3 - 1e3);
            case 1: return QJsonValue(QStringLiteral("clear"));
            case 2: return QJsonValue(random.bounded(2) == 0);
            case 3: return QJsonValue();
            case 4: {
                QJsonArray a;
                for (int i = random.bounded(5); i > 0; --i) a.append(self(self, depth + 1));
                return a;
            }
            default: {
                static const char* const keys[] = {"polygon", "path", "width", "text", "at", "arrow", "colour", "x"};
                QJsonObject o;
                for (int i = random.bounded(4); i > 0; --i) o.insert(QLatin1String(keys[random.bounded(8)]), self(self, depth + 1));
                return o;
            }
            }
        };
        for (int round = 0; round < 400; ++round) {
            QJsonArray items;
            for (int i = random.bounded(1, 4); i > 0; --i) {
                QJsonObject o;
                switch (random.bounded(4)) {
                case 0: o.insert("polygon", any(any, 0)); break;
                case 1: o.insert("path", any(any, 0)); o.insert("width", any(any, 1)); break;
                case 2: o.insert("arrow", any(any, 0)); break;
                default: o.insert("text", any(any, 1)); o.insert("at", any(any, 0)); break;
                }
                if (random.bounded(3) == 0) o.insert("colour", any(any, 2));
                items.append(random.bounded(10) == 0 ? any(any, 0) : QJsonValue(o));
            }
            (void)call("show_layout", {{"path", "demo.gds"}, {"overlay", items}});
        }
        (void)doc->view()->picture();
    }

    // The review's request carried out: the freehand step's snapped corners
    // made the trace's spine - replace keeps its layer and width -, read
    // back with gdstk, the tab read again, what changed told by cell and
    // layer; added, moved and deleted elements likewise; undo puts each
    // back in turn, and is refused once the file changed some other way.
    void editLayoutChangesTheFileAndUndoes()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        const QString file = path("layout/demo.gds");
        const QByteArray original = contents(file);
        doc->view()->addStroke(freehandStep());
        const QJsonArray snapped = doc->view()->readDrawing(false).at(0).toObject().value("polyline").toObject().value("snapped corners").toArray();
        QCOMPARE(snapped.size(), 6);
        // (The trace ends on the pads, at x = -50 and 50.)
        QJsonArray spine = snapped;
        spine.replace(5, QJsonArray{50, 0});
        QJsonObject r = call("edit_layout", {{"path", "demo.gds"}, {"cell", "TRACE"},
                                             {"edits", QJsonArray{QJsonObject{{"replace", "path"}, {"layer", "2/0"}, {"at", QJsonArray{-40, 0}}, {"points", spine}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject o = json(r);
        QCOMPARE(o.value("cell").toString(), QStringLiteral("TRACE"));
        QVERIFY2(o.value("done").toArray().at(0).toString().startsWith(QStringLiteral("replaced the path on 2/0 (its box ")), qPrintable(text(r)));
        const QJsonObject shapes = o.value("changed").toObject().value("cells").toObject().value("TRACE").toObject().value("shapes").toObject();
        QCOMPARE(shapes.value("2/0").toObject().value("added").toInt(), 1);
        QCOMPARE(shapes.value("2/0").toObject().value("taken away").toInt(), 1);
        QCOMPARE(shapes.value("2/0").toObject().value("added boxes").toArray().at(0).toArray(), (QJsonArray{-50, -2.5, 50, 42.5}));
        ReadBack back = readBack(file, "TRACE");
        QVERIFY(back.ok);
        QCOMPARE(back.spines.size(), 1);
        QCOMPARE(back.spines.at(0), (QPolygonF{QPointF(-50, 0), QPointF(-33, 0), QPointF(-33, 40), QPointF(36, 40), QPointF(36, 0), QPointF(50, 0)}));
        QCOMPARE(back.widths.at(0), 5.0);
        QVERIFY(doc->waitForLoaded(120000));
        QVERIFY(!doc->view()->drawing().isEmpty());   // (the user's strokes kept as it is read again)
        // Added, moved, deleted - in one call.
        r = call("edit_layout", {{"path", "demo.gds"},
                                 {"edits", QJsonArray{QJsonObject{{"add", "polygon"}, {"layer", "3/0"}, {"points", QJsonArray{QJsonArray{0, -40}, QJsonArray{10, -40}, QJsonArray{10, -30}}}},
                                                      QJsonObject{{"add", "text"}, {"layer", "10/0"}, {"text", "STEP"}, {"at", QJsonArray{0, 50}}},
                                                      QJsonObject{{"add", "placement"}, {"of", "PAD"}, {"at", QJsonArray{0, -55}}, {"rotation", 90}},
                                                      QJsonObject{{"move", "placement"}, {"of", "PAD"}, {"at", QJsonArray{60, 0}}, {"by", QJsonArray{5, 0}}},
                                                      QJsonObject{{"add", "path"}, {"layer", "4/0"}, {"points", QJsonArray{QJsonArray{-100, -60}, QJsonArray{100, -60}}}, {"width", 2}, {"ends", "round"}},
                                                      QJsonObject{{"delete", "shape"}, {"layer", "4/0"}, {"box", QJsonArray{-120, -65, 120, -55}}}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("cell").toString(), QStringLiteral("TOP"));   // (the cell its tab shows)
        back = readBack(file, "TOP");
        QCOMPARE(back.texts, QStringList{"STEP"});
        QVERIFY(back.polygons.contains(qMakePair(QStringLiteral("3/0"), QPolygonF{QPointF(0, -40), QPointF(10, -40), QPointF(10, -30)})));
        QVERIFY2(back.placements.contains(QStringLiteral("PAD 65 0")) && back.placements.contains(QStringLiteral("PAD 0 -55")), qPrintable(back.placements.join(", ")));
        QVERIFY(back.spines.isEmpty());   // (the 4/0 path added, then deleted)
        const QJsonObject top = json(r).value("changed").toObject().value("cells").toObject().value("TOP").toObject();
        QCOMPARE(top.value("texts").toObject().value("added").toArray(), (QJsonArray{"STEP on 10/0 at (0, 50)"}));
        QCOMPARE(top.value("placements").toObject().value("taken away").toArray(), (QJsonArray{"PAD at (60, 0)"}));
        // All or none: a wrong edit after a good one, and the file is as it was.
        const QByteArray now = contents(file);
        r = call("edit_layout", {{"path", "demo.gds"},
                                 {"edits", QJsonArray{QJsonObject{{"add", "text"}, {"layer", "10/0"}, {"text", "NO"}, {"at", QJsonArray{0, 0}}},
                                                      QJsonObject{{"delete", "polygon"}, {"layer", "9/0"}, {"at", QJsonArray{0, 0}}}}}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).startsWith(QStringLiteral("Edit 2: TOP has no polygon on 9/0 at (0, 0)")) && text(r).endsWith(QStringLiteral("Nothing was changed.")), qPrintable(text(r)));
        QCOMPARE(contents(file), now);
        // Undone in turn, to the file as it began.
        r = call("edit_layout", {{"path", "demo.gds"}, {"undo", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(json(r).value("changed").toObject().value("cells").toObject().contains("TOP"));
        QVERIFY(readBack(file, "TOP").texts.isEmpty());
        QCOMPARE(readBack(file, "TRACE").spines.at(0).size(), 6);
        QVERIFY(!failed(call("edit_layout", {{"path", "demo.gds"}, {"undo", true}})));
        QCOMPARE(contents(file), original);
        r = call("edit_layout", {{"path", "demo.gds"}, {"undo", true}});
        QVERIFY(failed(r) && text(r).startsWith(QStringLiteral("No edit of demo.gds is kept")));
        // Changed some other way since: undo refused, the file as it is.
        QVERIFY(!failed(call("edit_layout", {{"path", "demo.gds"}, {"edits", QJsonArray{QJsonObject{{"add", "text"}, {"layer", "10/0"}, {"text", "A"}, {"at", QJsonArray{0, 0}}}}}})));
        writeDemo(file);
        r = call("edit_layout", {{"path", "demo.gds"}, {"undo", true}});
        QVERIFY(failed(r));
        QVERIFY2(text(r).contains(QStringLiteral("has changed since the last edit_layout")), qPrintable(text(r)));
        QVERIFY(readBack(file, "TOP").texts.isEmpty());   // (as the other program wrote it, not the edit's "A")
    }

    // What edit_layout refuses, each with nothing changed: a key it has
    // not, a kind, a layer, points, picking nothing or two at once, a cell
    // placed in itself, an array's element, the cell or file wrong.
    void editLayoutRefusesWhatItCannotDo()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        const QString file = path("layout/demo.gds");
        const QByteArray before = contents(file);
        const auto refused = [&](const QJsonObject& args, const QString& said) {
            QJsonObject a = args;
            a.insert("path", "demo.gds");
            const QJsonObject r = call("edit_layout", a);
            if (!failed(r)) return QStringLiteral("not refused");
            if (!text(r).contains(said)) return text(r);
            return contents(file) == before ? QString() : QStringLiteral("the file changed");
        };
        const auto edit = [](const QJsonObject& e) { return QJsonObject{{"edits", QJsonArray{e}}}; };
        const QList<std::pair<QJsonObject, QString>> wrong{
            {edit({{"add", "polygon"}, {"layer", "2/0"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}, {"colour", "red"}}), "'colour' is not a key of an edit"},
            {edit({{"add", "shape"}, {"layer", "2/0"}}), "what is added is a polygon, a path, a text or a placement"},
            {edit({{"add", "circle"}, {"layer", "2/0"}}), "an edit is {"},
            {edit({{"add", "polygon"}, {"delete", "polygon"}}), "an edit is one of add, delete, replace or move"},
            {edit({{"add", "polygon"}, {"layer", "a/b"}, {"points", QJsonArray{}}}), "a 'layer' is"},
            {edit({{"add", "polygon"}, {"layer", "2/0"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}}), "3 at least"},
            {edit({{"add", "polygon"}, {"layer", "2/0"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}, QJsonArray{2, 2}}}}), "enclose nothing"},
            {edit({{"add", "path"}, {"layer", "2/0"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}}), "a 'width' in µm"},
            {edit({{"add", "path"}, {"layer", "2/0"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}, {"width", 1}, {"ends", "pointed"}}), "flush, round or extended"},
            {edit({{"add", "polygon"}, {"layer", "2/0"}, {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}, QJsonArray{1, 0}}}, {"width", 2}}), "'width' is not of add polygon"},
            {edit({{"add", "placement"}, {"of", "NOPE"}, {"at", QJsonArray{0, 0}}}), "no cell NOPE"},
            {QJsonObject{{"cell", "PAD"}, {"edits", QJsonArray{QJsonObject{{"add", "placement"}, {"of", "TOP"}, {"at", QJsonArray{0, 0}}}}}}, "would place PAD in itself"},
            {QJsonObject{{"cell", "PAD"}, {"edits", QJsonArray{QJsonObject{{"add", "placement"}, {"of", "PAD"}, {"at", QJsonArray{0, 0}}}}}}, "would place PAD in itself"},
            {edit({{"delete", "polygon"}, {"layer", "1/0"}, {"at", QJsonArray{-60, 0}}}), "TOP has no polygon on 1/0 at (-60, 0) (its own elements"},
            {edit({{"delete", "polygon"}, {"layer", "10/0"}}), "picked by 'at'"},
            {edit({{"delete", "polygon"}, {"layer", "10/0"}, {"at", QJsonArray{0, 0}}, {"box", QJsonArray{0, 0, 1, 1}}}), "not both"},
            {edit({{"replace", "placement"}, {"of", "PAD"}, {"at_to", QJsonArray{0, 0}}}), "2 things are picked"},
            {edit({{"move", "polygon"}, {"layer", "10/0"}, {"at", QJsonArray{0, 0}}}), "'by' [dx, dy]"},
            {QJsonObject{{"cell", "NOPE"}, {"edits", QJsonArray{QJsonObject{{"delete", "text"}, {"text", "x"}}}}}, "has no cell NOPE"},
            {QJsonObject{{"edits", QJsonArray{}}}, "1 to 10000 edits"},
            {QJsonObject{{"edits", QJsonArray{5}}}, "is an object"},
            {QJsonObject{{"undo", true}, {"edits", QJsonArray{}}}, "alone"},
            {QJsonObject{}, "'edits' is a list"}};
        for (const auto& [args, said] : wrong) {
            const QString why = refused(args, said);
            QVERIFY2(why.isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)) + QStringLiteral(": ") + why));
        }
        // An array's element (an AREF): changed as a whole or not at all.
        {
            gdstk::Library lib = {};
            lib.init("ARR", 1e-6, 1e-9);
            gdstk::Cell* dot = newCell(lib, "DOT");
            addRectangle(dot, {0, 0}, {1, 1}, 1);
            gdstk::Cell* top = newCell(lib, "TOP");
            place(top, dot, {0, 0});
            gdstk::Reference* array = top->reference_array[0];
            array->repetition.type = gdstk::RepetitionType::Rectangular;
            array->repetition.columns = 3;
            array->repetition.rows = 1;
            array->repetition.spacing = gdstk::Vec2{2, 0};
            QVERIFY(lib.write_gds(QFile::encodeName(path("array.gds")).constData(), 0, nullptr) == gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        const QJsonObject r = call("edit_layout", {{"path", path("array.gds")}, {"cell", "TOP"},
                                                   {"edits", QJsonArray{QJsonObject{{"move", "placement"}, {"of", "DOT"}, {"by", QJsonArray{1, 0}}}}}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("is repeated (an array)")), qPrintable(text(r)));
        QVERIFY(failed(call("edit_layout", {{"path", "nowhere.gds"}, {"edits", QJsonArray{QJsonObject{{"delete", "text"}, {"text", "x"}}}}})));
    }

    // show_layout's reload says what reading the file again changed - here
    // a pad written in by another program -, and nothing when nothing did.
    void aReloadSaysWhatChanged()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        QJsonObject o = json(call("show_layout", {{"path", "demo.gds"}, {"reload", true}}));
        QCOMPARE(o.value("changed").toString(), QStringLiteral("nothing"));
        {
            gdstk::Library lib = {};
            gdstk::ErrorCode error = gdstk::ErrorCode::NoError;
            lib = gdstk::read_gds(QFile::encodeName(path("layout/demo.gds")).constData(), 1e-6, 0, nullptr, &error);
            addRectangle(lib.get_cell("TOP"), {100, 30}, {120, 50}, 1);
            QVERIFY(lib.write_gds(QFile::encodeName(path("layout/demo.gds")).constData(), 0, nullptr) == gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        o = json(call("show_layout", {{"path", "demo.gds"}, {"reload", true}}));
        QJsonArray boxes;   // (a list of one list, by append: check-json-nesting.py)
        boxes.append(QJsonArray{100, 30, 120, 50});
        QCOMPARE(o.value("changed").toObject(),
                 (QJsonObject{{"cells", QJsonObject{{"TOP", QJsonObject{{"shapes", QJsonObject{{"1/0", QJsonObject{{"added", 1}, {"added boxes", boxes}}}}}}}}}}));
        writeDemo(path("layout/demo.gds"));
    }

    // Edits as a fuzzer gives them, on a copy of the demo: each refused
    // with the file as it was, or written and read back whole.
    void anyEditIsRefusedOrWritten()
    {
        const QString file = path("fuzz.gds");
        writeDemo(file);
        QRandomGenerator random(1107);
        static const char* const ops[] = {"add", "delete", "replace", "move"};
        static const char* const kinds[] = {"polygon", "path", "shape", "text", "placement", "nothing"};
        static const char* const keys[] = {"layer", "points", "width", "ends", "text", "at", "of", "rotation", "mirrored", "magnification", "box", "by", "layer_to", "text_to", "at_to"};
        const auto value = [&random]() -> QJsonValue {
            const auto xy = [&random] { return QJsonArray{random.generateDouble() * 200 - 100, random.generateDouble() * 100 - 50}; };
            switch (random.bounded(9)) {
            case 0: return QStringLiteral("2/0");
            case 1: return QStringLiteral("PAD");
            case 2: return random.generateDouble() * 20 - 5;
            case 3: return xy();
            case 4: {
                QJsonArray a;
                for (int i = random.bounded(6); i > 0; --i) a.append(xy());
                return a;
            }
            case 5: return QJsonArray{-60, -20, 60, 50};
            case 6: return random.bounded(2) == 0;
            case 7: return QStringLiteral("round");
            default: return QJsonValue();
            }
        };
        for (int round = 0; round < 300; ++round) {
            QJsonArray edits;
            for (int n = random.bounded(1, 4); n > 0; --n) {
                QJsonObject e{{QLatin1String(ops[random.bounded(4)]), QLatin1String(kinds[random.bounded(6)])}};
                for (int k = random.bounded(5); k > 0; --k) e.insert(QLatin1String(keys[random.bounded(15)]), value());
                edits.append(e);
            }
            const QByteArray before = contents(file);
            const QJsonObject r = call("edit_layout", {{"path", file}, {"edits", edits}});
            if (failed(r)) {
                QCOMPARE(contents(file), before);
            } else {
                QString error;
                QVERIFY2(read(file, &error) != nullptr, qPrintable(error));
            }
        }
    }

    // Pictures for the write-up (QUCS_PEN_PICTURES=<folder>; run with
    // QT_QPA_PLATFORM=cocoa for the screen's own drawing): the review's
    // three strokes as the user drew them; then Claude's overlay of the
    // step it means to make, 5 µm wide, over the sketch, the ring and the
    // arrow marked addressed.
    void picturesForTheWriteUp()
    {
        const QString folder = qEnvironmentVariable("QUCS_PEN_PICTURES");
        if (folder.isEmpty()) QSKIP("QUCS_PEN_PICTURES names no folder");
        QDir().mkpath(folder);
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        app->resize(1100, 700);
        LayoutView* view = doc->view();
        view->addStroke(ringAtThePeak());
        view->addStroke(arrowAtThePeak());
        view->addStroke(freehandStep());
        QVERIFY(!failed(call("show_layout", {{"path", "demo.gds"}, {"region", QJsonArray{-80, -25, 80, 75}}})));
        QTest::qWait(200);
        QVERIFY(doc->grab().save(QDir(folder).filePath("strokes.png")));
        const QJsonArray step{QJsonArray{-50, 0}, QJsonArray{-33, 0}, QJsonArray{-33, 40}, QJsonArray{36, 40}, QJsonArray{36, 0}, QJsonArray{50, 0}};
        QVERIFY(!failed(call("show_layout", {{"path", "demo.gds"}, {"addressed", QJsonArray{1, 2}},
                                             {"overlay", QJsonArray{QJsonObject{{"path", step}, {"width", 5}},
                                                                    QJsonObject{{"arrow", QJsonArray{QJsonArray{62, 58}, QJsonArray{38, 43}}}, {"text", "the new trace, 5 µm"}}}}})));
        QTest::qWait(200);
        QVERIFY(doc->grab().save(QDir(folder).filePath("overlay.png")));
    }

    // Strokes of every shape, as a hand might draw or a fuzzer might, never
    // break the reading: none is asserted but that it reads.
    void anyStrokeIsRead()
    {
        LayoutDoc* doc = demo();
        QVERIFY(doc != nullptr);
        QRandomGenerator random(20261107);
        const auto real = [&random](double lo, double hi) { return lo + (hi - lo) * random.generateDouble(); };
        for (int round = 0; round < 300; ++round) {
            QPolygonF points;
            const int n = random.bounded(1, 60);
            const QPointF at(real(-200.0, 200.0), real(-100.0, 100.0));
            const double scale = std::pow(10.0, real(-3.0, 3.0));
            for (int k = 0; k < n; ++k) {
                switch (random.bounded(4)) {
                case 0: points << at + scale * QPointF(real(-1.0, 1.0), real(-1.0, 1.0)); break;
                case 1: points << at; break;   // (the same point again)
                case 2: points << at + scale * QPointF(k, 0); break;
                default: points << at + scale * QPointF(std::cos(k), std::sin(k)); break;
                }
            }
            markup::Stroke s = stroke(points, std::pow(10.0, real(-4.0, 1.0)));
            s.highlighter = random.bounded(5) == 0;
            doc->view()->addStroke(s);
            (void)markup::polylineOf(points, s.width, random.bounded(2) == 0, markup::Snaps{});
        }
        const QJsonArray read = doc->view()->readDrawing(true);
        QCOMPARE(read.size(), 300);
        QVERIFY(!told(doc).isEmpty());
    }
};

QTEST_MAIN(TestPenReading)
#include "test_pen_reading.moc"
