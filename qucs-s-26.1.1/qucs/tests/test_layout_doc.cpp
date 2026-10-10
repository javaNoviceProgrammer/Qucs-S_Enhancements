/*
 * test_layout_doc.cpp - GDSII and OASIS layouts (layout.h, layoutdoc.h):
 * written by gdstk here and read back cell for cell and shape for shape -
 * OASIS with its compressed blocks, repetitions, properties and layer
 * names, GDSII gzipped too; a file that is none, or is damaged, said so;
 * a layout in a tab, its cells and layers beside it, a shape selected, a
 * distance measured, a cell found, its file written again and read again
 * where it was; a .lyp's colours; the layers' colours in contrast to the
 * canvas, light or dark; the grid behind the layout; a million shapes read
 * and drawn in time, and a read cancelled; Claude's get_layout,
 * find_shapes and show_layout.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QToolButton>
#include <QAction>
#include <QMenu>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <gdstk/gdstk.hpp>
#include <zlib.h>

#include <cmath>

#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "filebrowser.h"
#include "isolated_settings.h"
#include "layout.h"
#include "layoutdoc.h"
#include "links.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "settings.h"

using namespace qucs_s::layout;

// gdstk's Vec2 is a union of {x, y} and others: {1, 2} is one, braces or not.
#pragma GCC diagnostic ignored "-Wmissing-braces"

namespace {

// ----------------------------------------------------------------------
// Layouts written by gdstk.

gdstk::Cell* newCell(gdstk::Library& lib, const char* name)
{
    auto* c = static_cast<gdstk::Cell*>(gdstk::allocate_clear(sizeof(gdstk::Cell)));
    c->init(name);
    lib.cell_array.append(c);
    return c;
}

gdstk::Polygon* addRectangle(gdstk::Cell* c, gdstk::Vec2 a, gdstk::Vec2 b, uint32_t layer, uint32_t datatype = 0)
{
    auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
    *p = gdstk::rectangle(a, b, gdstk::make_tag(layer, datatype));
    c->polygon_array.append(p);
    return p;
}

gdstk::Reference* place(gdstk::Cell* parent, gdstk::Cell* child, gdstk::Vec2 at)
{
    auto* r = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
    r->init(child);
    r->origin = at;
    parent->reference_array.append(r);
    return r;
}

/*
 * The sample, in µm on a grid of 1 nm:
 *  VIA: a square 0.5 on 3/0.
 *  INV: a rectangle (0, 0)-(2, 1) on 1/0 with a property; a path on 2/0
 *       from (0, 0.5) to (2, 0.5), 0.2 wide, flush; a text "OUT" on 10/0
 *       at (2, 0.5).
 *  TOP: INV at (10, 0); INV at (20, 0) turned 90°; INV at (30, 0) mirrored;
 *       VIA as an array of 3 by 2, 1 apart, at (0, 0); a triangle on 5/0;
 *       a square 0.2 on 4/0 repeated 10 times, 1 apart, from (0, 20).
 * \a extra adds a cell EXTRA with a rectangle on 6/0, placed at (40, 0).
 */
void writeSample(const QString& path, bool oasis, int deflate = 6, bool extra = false)
{
    gdstk::Library lib = {};
    lib.init("SAMPLE", 1e-6, 1e-9);
    gdstk::Cell* via = newCell(lib, "VIA");
    addRectangle(via, {0, 0}, {0.5, 0.5}, 3);
    gdstk::Cell* inv = newCell(lib, "INV");
    gdstk::Polygon* body = addRectangle(inv, {0, 0}, {2, 1}, 1);
    gdstk::set_gds_property(body->properties, 1, "net_A", 5);
    if (oasis) gdstk::set_property(body->properties, "net", "A", true);
    auto* wire = static_cast<gdstk::FlexPath*>(gdstk::allocate_clear(sizeof(gdstk::FlexPath)));
    wire->num_elements = 1;
    wire->elements = static_cast<gdstk::FlexPathElement*>(gdstk::allocate_clear(sizeof(gdstk::FlexPathElement)));
    wire->init({0, 0.5}, 0.2, 0, 0.001, gdstk::make_tag(2, 0));
    wire->simple_path = true;
    wire->scale_width = true;
    wire->elements[0].end_type = gdstk::EndType::Flush;
    wire->segment(gdstk::Vec2{2, 0.5}, nullptr, nullptr, false);
    inv->flexpath_array.append(wire);
    auto* text = static_cast<gdstk::Label*>(gdstk::allocate_clear(sizeof(gdstk::Label)));
    text->init("OUT");
    text->origin = {2, 0.5};
    text->tag = gdstk::make_tag(10, 0);
    inv->label_array.append(text);

    gdstk::Cell* top = newCell(lib, "TOP");
    place(top, inv, {10, 0});
    place(top, inv, {20, 0})->rotation = M_PI / 2;
    place(top, inv, {30, 0})->x_reflection = true;
    gdstk::Reference* array = place(top, via, {0, 0});
    array->repetition.type = gdstk::RepetitionType::Rectangular;
    array->repetition.columns = 3;
    array->repetition.rows = 2;
    array->repetition.spacing = {1, 1};
    auto* triangle = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
    triangle->tag = gdstk::make_tag(5, 0);
    triangle->point_array.append({0, 10});
    triangle->point_array.append({4, 10});
    triangle->point_array.append({2, 13});
    top->polygon_array.append(triangle);
    gdstk::Polygon* dots = addRectangle(top, {0, 20}, {0.2, 20.2}, 4);
    dots->repetition.type = gdstk::RepetitionType::Rectangular;
    dots->repetition.columns = 10;
    dots->repetition.rows = 1;
    dots->repetition.spacing = {1, 0};
    if (extra) {
        gdstk::Cell* more = newCell(lib, "EXTRA");
        addRectangle(more, {0, 0}, {3, 3}, 6);
        place(top, more, {40, 0});
    }
    // An OASIS file names 1/0.
    gdstk::LayerName name = {};
    name.type = gdstk::LayerNameType::DATA;
    name.name = gdstk::copy_string("Metal1", nullptr);
    name.layer_interval = {gdstk::OasisInterval::SingleValue, 1, 0};
    name.type_interval = {gdstk::OasisInterval::SingleValue, 0, 0};
    lib.layer_names.append(name);

    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray file = QFile::encodeName(path);
    const gdstk::ErrorCode written = oasis ? lib.write_oas(file.constData(), 0, uint8_t(deflate), OASIS_CONFIG_DETECT_ALL | OASIS_CONFIG_STANDARD_PROPERTIES)
                                           : lib.write_gds(file.constData(), 0, nullptr);
    QCOMPARE(written, gdstk::ErrorCode::NoError);
    lib.free_all();
}

bool gzip(const QString& from, const QString& to)
{
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = in.readAll();
    gzFile out = gzopen(QFile::encodeName(to).constData(), "wb");
    if (out == nullptr) return false;
    const bool ok = gzwrite(out, bytes.constData(), unsigned(bytes.size())) == int(bytes.size());
    return gzclose(out) == Z_OK && ok;
}

bool write(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray read(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Every shape of \a cell placed in it, to the bottom, repetitions taken
// apart: "layer: x y, x y, ..." in nm, sorted - what a GDSII and an
// OASIS file of one layout both have, however each keeps it.
QStringList flattened(const Layout& layout, const QString& cell)
{
    Query q;
    q.cell = layout.cellIndex(cell);
    q.labels = false;
    QStringList all;
    search(layout, q, [&](const Found& f) {
        const Cell& c = layout.cells.at(f.cell);
        QStringList points;
        QPolygonF outline = outlineOf(layout, f);
        // (the same outline from any corner: from its least point)
        int first = 0;
        for (int i = 1; i < outline.size(); ++i)
            if (outline[i].x() < outline[first].x() - 1e-9
                || (std::abs(outline[i].x() - outline[first].x()) < 1e-9 && outline[i].y() < outline[first].y()))
                first = i;
        for (int i = 0; i < outline.size(); ++i) {
            const QPointF p = outline.at((first + i) % outline.size());
            points << QStringLiteral("%1 %2").arg(qRound64(p.x() * 1000)).arg(qRound64(p.y() * 1000));
        }
        all << layout.layers.at(int(c.shapes.at(f.shape).layer)).key.text() + QStringLiteral(": ") + points.join(QStringLiteral(", "));
        return true;
    });
    all.sort();
    return all;
}

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

void waitForReads()
{
    QElapsedTimer t;
    t.start();
    while (readsGoing() > 0 && t.elapsed() < 120000) QTest::qWait(20);
}

} // namespace

class TestLayoutDoc : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 120000)
    {
        return control->callNow(tool, args, timeoutMs);
    }
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
    void closeAll()
    {
        app->closeAllFiles();
        waitForReads();
    }
    // A press, a drag and a release of the left button in \a view, at
    // its pixels.
    static void drag(QWidget* view, QPointF from, QPointF to, Qt::KeyboardModifiers keys = {})
    {
        const auto send = [view](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons held, Qt::KeyboardModifiers k) {
            QMouseEvent e(type, at, view->mapToGlobal(at), button, held, k);
            QCoreApplication::sendEvent(view, &e);
        };
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, keys);
        for (int i = 1; i <= 5; ++i) send(QEvent::MouseMove, from + (to - from) * (i / 5.0), Qt::NoButton, Qt::LeftButton, keys);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, keys);
    }
    static void click(QWidget* view, QPointF at) { drag(view, at, at); }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
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
        writeSample(path("sample.gds"), false);
        writeSample(path("sample.oas"), true);
    }

    void cleanupTestCase()
    {
        closeAll();
        delete app;
        QucsMain = nullptr;
    }

    void cleanup()
    {
        closeAll();
        // The canvas as it is at first, whatever a test left.
        QucsSettingsFile settings;
        for (const char* key : {"LayoutViewer/background", "LayoutViewer/contrast", "LayoutViewer/grid", "LayoutViewer/gridStyle"})
            settings.remove(QLatin1String(key));
    }

    // ---- Reading

    // A GDSII layout read back as gdstk wrote it: its cells, the top one,
    // the placements (turned, mirrored, an array), every shape's points,
    // a path's width and spine, a text, a property, the layers, the units.
    void aGdsiiLayoutIsReadCellForCellAndShapeForShape()
    {
        QString error;
        const auto layout = read(path("sample.gds"), &error);
        QVERIFY2(layout, qPrintable(error));
        QCOMPARE(layout->format, Layout::Format::Gds);
        QCOMPARE(layout->library, QString("SAMPLE"));
        QCOMPARE(layout->dbu, 0.001);
        QCOMPARE(layout->cells.size(), 3);
        QCOMPARE(layout->topCells.size(), 1);
        const Cell& top = layout->cells.at(layout->topCells.first());
        QCOMPARE(top.name, QString("TOP"));
        QCOMPARE(layout->cells.at(layout->cellIndex("INV")).parents, 1);
        QCOMPARE(top.depth, 1);
        // Its bounds: everything it places, turned and mirrored as placed.
        QCOMPARE(top.bounds.left(), 0.0);
        QCOMPARE(top.bounds.right(), 32.0);
        QCOMPARE(top.bounds.top(), -1.0);
        QVERIFY(std::abs(top.bounds.bottom() - 20.2) < 1e-9);
        // The layers in order, and what is on each (a repetition's copies each).
        QStringList layers;
        for (const LayerInfo& l : layout->layers) layers << QStringLiteral("%1:%2/%3").arg(l.key.text()).arg(l.shapes).arg(l.labels);
        QCOMPARE(layers, QStringList({"1/0:1/0", "2/0:1/0", "3/0:1/0", "4/0:10/0", "5/0:1/0", "10/0:0/1"}));
        QCOMPARE(layout->shapeCount, quint64(14));
        QCOMPARE(layout->labelCount, quint64(1));
        QCOMPARE(layout->instanceCount, quint64(3 + 6));
        // INV, shape for shape.
        const Cell& inv = layout->cells.at(layout->cellIndex("INV"));
        QCOMPARE(inv.shapes.size(), 2);
        const qucs_s::layout::Shape& body = inv.shapes.at(0);
        QCOMPARE(body.kind, ShapeKind::Polygon);
        QCOMPARE(layout->layers.at(int(body.layer)).key, (LayerKey{1, 0}));
        QCOMPARE(QPolygonF(inv.points.mid(body.first, body.count)), QPolygonF({{0, 0}, {2, 0}, {2, 1}, {0, 1}}));
        QVERIFY(body.properties >= 0);
        QCOMPARE(inv.properties.at(body.properties), (Properties{{"1", "net_A"}}));
        const qucs_s::layout::Shape& wire = inv.shapes.at(1);
        QCOMPARE(wire.kind, ShapeKind::Path);
        QCOMPARE(inv.paths.at(wire.path).width, 0.2);
        QCOMPARE(inv.paths.at(wire.path).ends, QString("flush"));
        QCOMPARE(QPolygonF(inv.points.mid(inv.paths.at(wire.path).first, inv.paths.at(wire.path).count)), QPolygonF(QList<QPointF>{{0, 0.5}, {2, 0.5}}));
        QVERIFY2(std::abs(wire.bounds.top() - 0.4) < 1e-9 && std::abs(wire.bounds.bottom() - 0.6) < 1e-9
                     && wire.bounds.left() == 0 && wire.bounds.right() == 2,
                 qPrintable(QStringLiteral("%1 %2 %3 %4").arg(wire.bounds.left()).arg(wire.bounds.top()).arg(wire.bounds.right()).arg(wire.bounds.bottom())));
        QCOMPARE(inv.labels.size(), 1);
        QCOMPARE(inv.labels.at(0).text, QString("OUT"));
        QCOMPARE(inv.labels.at(0).origin, QPointF(2, 0.5));
        // TOP: the placements as placed.
        QCOMPARE(top.instances.size(), 4);
        int turned = 0, mirrored = 0, arrays = 0;
        for (const Instance& i : top.instances) {
            if (i.rotation == 90) ++turned;
            if (i.mirrored) ++mirrored;
            if (i.repetition >= 0) {
                ++arrays;
                QCOMPARE(top.repetitions.at(i.repetition).count(), quint64(6));
            }
        }
        QCOMPARE(turned, 1);
        QCOMPARE(mirrored, 1);
        QCOMPARE(arrays, 1);
        // A GDSII file has no repetitions of shapes: ten of them.
        int onFour = 0;
        for (const qucs_s::layout::Shape& s : top.shapes)
            if (layout->layers.at(int(s.layer)).key == LayerKey{4, 0}) ++onFour;
        QCOMPARE(onFour, 10);
        // The INV turned 90° about its origin, at (20, 0): (19, 0)-(20, 2).
        const QStringList flat = flattened(*layout, "TOP");
        QVERIFY2(flat.contains("1/0: 19000 0, 20000 0, 20000 2000, 19000 2000"), qPrintable(flat.join('\n')));
        // Mirrored at (30, 0): (30, -1)-(32, 0), its points the other way round.
        QVERIFY2(flat.contains("1/0: 30000 -1000, 30000 0, 32000 0, 32000 -1000"), qPrintable(flat.join('\n')));
        QCOMPARE(flat.size(), 3 /* INV */ * 2 + 6 /* VIA */ + 1 + 10);
    }

    // An OASIS file of the same layout - compressed blocks, a shape's
    // repetition kept as one, properties of its own, a layer's name - read
    // as the GDSII one is, shape for shape; a gzipped GDSII file too.
    void anOasisLayoutHasTheSameShapesItsBlocksRepetitionsAndNames()
    {
        QString error;
        const auto gds = read(path("sample.gds"), &error);
        QVERIFY2(gds, qPrintable(error));
        const auto oas = read(path("sample.oas"), &error);
        QVERIFY2(oas, qPrintable(error));
        QCOMPARE(oas->format, Layout::Format::Oasis);
        QCOMPARE(oas->dbu, 0.001);
        QCOMPARE(flattened(*oas, "TOP"), flattened(*gds, "TOP"));
        QCOMPARE(oas->shapeCount, gds->shapeCount);
        QCOMPARE(oas->instanceCount, gds->instanceCount);
        // Written plain, read the same.
        writeSample(path("plain.oas"), true, 0);
        const auto plain = read(path("plain.oas"), &error);
        QVERIFY2(plain, qPrintable(error));
        QCOMPARE(flattened(*plain, "TOP"), flattened(*oas, "TOP"));
        // Compressed blocks: three thousand shapes in a fraction of the bytes,
        // read as they were written.
        for (int deflate : {0, 9}) {
            gdstk::Library lib = {};
            lib.init("MANY", 1e-6, 1e-9);
            gdstk::Cell* many = newCell(lib, "MANY");
            for (int i = 0; i < 3000; ++i) {
                gdstk::Polygon* p = addRectangle(many, {i * 1.5, (i % 7) * 2.0}, {i * 1.5 + 1, (i % 7) * 2.0 + 1.25}, uint32_t(i % 3));
                if (i % 5 == 0) gdstk::set_property(p->properties, "id", uint64_t(i), true);
            }
            const QString file = path(deflate ? "many-compressed.oas" : "many-plain.oas");
            QCOMPARE(lib.write_oas(QFile::encodeName(file).constData(), 0, uint8_t(deflate), OASIS_CONFIG_DETECT_ALL), gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        QVERIFY2(QFileInfo(path("many-compressed.oas")).size() * 2 < QFileInfo(path("many-plain.oas")).size(),
                 qPrintable(QStringLiteral("%1 %2").arg(QFileInfo(path("many-compressed.oas")).size()).arg(QFileInfo(path("many-plain.oas")).size())));
        const auto packed = read(path("many-compressed.oas"), &error);
        QVERIFY2(packed, qPrintable(error));
        const auto unpacked = read(path("many-plain.oas"), &error);
        QVERIFY2(unpacked, qPrintable(error));
        QCOMPARE(packed->shapeCount, quint64(3000));
        QCOMPARE(flattened(*packed, "MANY"), flattened(*unpacked, "MANY"));
        const Cell& many = packed->cells.at(packed->cellIndex("MANY"));
        int withId = 0;
        for (const qucs_s::layout::Shape& sh : many.shapes) withId += sh.properties >= 0 && many.properties.at(sh.properties).first().first == "id";
        QCOMPARE(withId, 600);
        // The square on 4/0: one shape with ten copies.
        const Cell& top = oas->cells.at(oas->cellIndex("TOP"));
        int onFour = 0;
        for (const qucs_s::layout::Shape& s : top.shapes)
            if (oas->layers.at(int(s.layer)).key == LayerKey{4, 0}) {
                ++onFour;
                QVERIFY(s.repetition >= 0);
                QCOMPARE(top.repetitions.at(s.repetition).count(), quint64(10));
                QCOMPARE(top.repetitions.at(s.repetition).extent(s.bounds).right(), 9.2);
            }
        QCOMPARE(onFour, 1);
        // Its properties, and its name of 1/0.
        const Cell& inv = oas->cells.at(oas->cellIndex("INV"));
        QVERIFY(inv.shapes.at(0).properties >= 0);
        QVERIFY2(inv.properties.at(inv.shapes.at(0).properties).contains({"net", "A"}), "the OASIS property");
        QCOMPARE(oas->layers.at(oas->layerIndex({1, 0})).name, QString("Metal1"));
        // Gzipped GDSII: its GDSII.
        QVERIFY(gzip(path("sample.gds"), path("zipped.gds.gz")));
        const auto zipped = read(path("zipped.gds.gz"), &error);
        QVERIFY2(zipped, qPrintable(error));
        QVERIFY(zipped->compressed);
        QCOMPARE(flattened(*zipped, "TOP"), flattened(*gds, "TOP"));
    }

    // OASIS as KLayout writes it (data/klayout_sample.py): compressed
    // blocks, strict mode, the repetitions it finds itself - read as KLayout
    // counts it, layer for layer.
    void anOasisFileOfKLayoutsIsReadAsKLayoutReadsIt()
    {
        QString error;
        const auto layout = read(QStringLiteral(QUCS_TEST_DATA "/klayout_sample.oas"), &error);
        QVERIFY2(layout, qPrintable(error));
        QCOMPARE(layout->dbu, 0.001);
        QCOMPARE(layout->topCells.size(), 1);
        const Cell& chip = layout->cells.at(layout->mainCell());
        QCOMPARE(chip.name, QString("CHIP"));
        QCOMPARE(chip.bounds, QRectF(QPointF(0, -1), QPointF(32, 39.2)));
        QMap<QString, int> flat;
        Query q;
        q.cell = layout->mainCell();
        search(*layout, q, [&](const Found& f) {
            const Cell& c = layout->cells.at(f.cell);
            const quint32 layer = f.isText() ? c.labels.at(f.label).layer : c.shapes.at(f.shape).layer;
            flat[layout->layers.at(int(layer)).key.text()]++;
            return true;
        });
        // (KLayout's own count: klayout_sample.py prints it.)
        QCOMPARE(flat, (QMap<QString, int>{{"1/0", 3}, {"2/0", 3}, {"3/0", 6}, {"4/0", 400}, {"5/0", 1}, {"10/0", 3}}));
        QCOMPARE(layout->layers.at(layout->layerIndex({1, 0})).name, QString("Metal1"));
        const Cell& inv = layout->cells.at(layout->cellIndex("INV"));
        bool net = false;
        for (const qucs_s::layout::Shape& sh : inv.shapes)
            if (sh.properties >= 0) net = net || inv.properties.at(sh.properties).contains({"net", "A"});
        QVERIFY(net);
    }

    // What is not a layout, or is a damaged one, is said so; a cell placed
    // that the file has not is named; a cell that places itself (through
    // another) is drawn, that placement left out.
    void whatIsNotALayoutOrIsDamagedIsSaidSo()
    {
        QString error;
        QVERIFY(write(path("notes.gds"), "these are notes, not a layout\n"));
        QVERIFY(!read(path("notes.gds"), &error));
        QVERIFY2(error.contains("not a GDSII or OASIS layout"), qPrintable(error));
        QVERIFY(write(path("empty.oas"), ""));
        QVERIFY(!read(path("empty.oas"), &error));
        QVERIFY(!read(path("nowhere.gds"), &error));
        QVERIFY2(error.contains("There is no file"), qPrintable(error));
        // Cut in half: the HEADER is there, the rest not whole.
        const QByteArray whole = read(path("sample.gds"));
        QVERIFY(write(path("half.gds"), whole.left(whole.size() / 2)));
        const auto half = read(path("half.gds"), &error);
        QVERIFY2(!half || !half->warnings.isEmpty(), "a damaged file is said to be so");
        if (!half) QVERIFY2(error.contains("could not be read"), qPrintable(error));
        // A cut OASIS file.
        const QByteArray oas = read(path("sample.oas"));
        QVERIFY(write(path("half.oas"), oas.left(oas.size() / 2)));
        const auto halfOas = read(path("half.oas"), &error);
        QVERIFY2(!halfOas || !halfOas->warnings.isEmpty(), "a damaged OASIS file is said to be so");
        // A gzip of something else.
        QVERIFY(write(path("text.txt"), "not a layout either"));
        QVERIFY(gzip(path("text.txt"), path("text.gds.gz")));
        QVERIFY(!read(path("text.gds.gz"), &error));
        QVERIFY2(error.contains("not a GDSII layout"), qPrintable(error));

        // Placed but not there; and a loop.
        gdstk::Library lib = {};
        lib.init("ODD", 1e-6, 1e-9);
        gdstk::Cell* a = newCell(lib, "A");
        gdstk::Cell* b = newCell(lib, "B");
        addRectangle(a, {0, 0}, {1, 1}, 1);
        addRectangle(b, {0, 0}, {2, 2}, 2);
        place(a, b, {5, 0});
        place(b, a, {0, 5});   // A places B places A
        gdstk::Cell* top = newCell(lib, "MAIN");
        place(top, a, {0, 0});
        auto* ghost = static_cast<gdstk::Reference*>(gdstk::allocate_clear(sizeof(gdstk::Reference)));
        ghost->init("GHOST");
        ghost->origin = {9, 9};
        top->reference_array.append(ghost);
        QCOMPARE(lib.write_gds(QFile::encodeName(path("odd.gds")).constData(), 0, nullptr), gdstk::ErrorCode::NoError);
        lib.free_all();
        const auto odd = read(path("odd.gds"), &error);
        QVERIFY2(odd, qPrintable(error));
        QCOMPARE(odd->missing, QStringList{"GHOST"});
        QVERIFY2(odd->warnings.join('\n').contains("GHOST"), qPrintable(odd->warnings.join('\n')));
        QVERIFY2(odd->warnings.join('\n').contains("which places it"), qPrintable(odd->warnings.join('\n')));
        QCOMPARE(odd->topCells.size(), 1);
        QCOMPARE(odd->cells.at(odd->topCells.first()).name, QString("MAIN"));
        // Drawn and searched to the bottom: it ends.
        QImage image(200, 200, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        RenderOptions o;
        o.cell = odd->mainCell();
        o.device = QRectF(0, 0, 200, 200);
        o.toDevice = viewTransform(odd->cells.at(o.cell).bounds.center(), 10, o.device);
        const RenderStats stats = render(painter, *odd, o);
        QVERIFY(stats.polygons >= 2);
        QCOMPARE(flattened(*odd, "MAIN").size(), 2);
    }

    // Every cut of the sample files, and bytes of them changed at random
    // (the same each run): each read ends - a layout, or why not - and
    // nothing is read or written out of its memory (AddressSanitizer's
    // build says).
    void aDamagedFileNeverBreaksTheReader()
    {
        writeSample(path("fuzz/plain.oas"), true, 0);
        QStringList sources{path("sample.gds"), path("sample.oas"), path("fuzz/plain.oas")};
        int read_ = 0, refused = 0;
        for (const QString& source : std::as_const(sources)) {
            const QByteArray whole = read(source);
            QVERIFY(!whole.isEmpty());
            const QString suffix = QFileInfo(source).suffix();
            const QString cut = path("fuzz/cut." + suffix);
            for (qsizetype n = 0; n < whole.size(); ++n) {
                QVERIFY(write(cut, whole.left(n)));
                QString error;
                if (read(cut, &error)) ++read_;
                else {
                    QVERIFY(!error.isEmpty());
                    ++refused;
                }
            }
            quint32 seed = 2026;
            const auto next = [&seed] { return (seed = seed * 1103515245u + 12345u) >> 8; };
            const QString changed = path("fuzz/changed." + suffix);
            for (int round = 0; round < 400; ++round) {
                QByteArray bytes = whole;
                const int changes = 1 + int(next() % 4);
                for (int k = 0; k < changes; ++k) {
                    // (not the first bytes, which tell the kind: past them)
                    const qsizetype at = 16 + qsizetype(next() % quint32(bytes.size() - 16));
                    bytes[at] = char(next() & 0xff);
                }
                QVERIFY(write(changed, bytes));
                QString error;
                if (read(changed, &error)) ++read_;
                else ++refused;
            }
        }
        qInfo("Damaged files: %d read, %d refused", read_, refused);
        QVERIFY(refused > 0);
    }

    // A grid of a million copies with a few in sight: those few visited,
    // along v1 and v2 however they lie; an explicit list each; the extent.
    void aRepetitionIsVisitedWhereItIsInSight()
    {
        Repetition grid;
        grid.columns = 1000;
        grid.rows = 1000;
        grid.v1 = QPointF(1, 0);
        grid.v2 = QPointF(0, 1);
        const QRectF unit(0, 0, 0.5, 0.5);
        int seen = 0;
        grid.forEach(unit, QRectF(QPointF(10.2, 20.2), QPointF(12.7, 21.7)), [&](QPointF) { return ++seen, true; });
        QCOMPARE(seen, 3 * 2);   // columns 10 to 12, rows 20 and 21
        // Slanted vectors: what is found is what is there.
        Repetition slanted;
        slanted.columns = 300;
        slanted.rows = 200;
        slanted.v1 = QPointF(1, 0.5);
        slanted.v2 = QPointF(-0.3, 1);
        const QRectF visible(QPointF(40, 30), QPointF(47, 36));
        QSet<quint64> found, expected;
        quint64 visited = 0;
        slanted.forEach(unit, visible, [&](QPointF o) {
            ++visited;
            if (unit.translated(o).intersects(visible)) found.insert(qRound64(o.x() * 1000) * 1000003 + qRound64(o.y() * 1000));
            return true;
        });
        for (quint64 i = 0; i < slanted.count(); ++i) {
            const QPointF o = slanted.offset(i);
            if (unit.translated(o).intersects(visible)) expected.insert(qRound64(o.x() * 1000) * 1000003 + qRound64(o.y() * 1000));
        }
        QVERIFY(!expected.isEmpty());
        QCOMPARE(found, expected);
        QVERIFY2(visited < slanted.count() / 20, qPrintable(QString::number(visited)));
        // One row.
        Repetition row;
        row.columns = 100000;
        row.v1 = QPointF(2, 0);
        seen = 0;
        row.forEach(unit, QRectF(QPointF(99, -1), QPointF(104.1, 1)), [&](QPointF) { return ++seen, true; });
        QCOMPARE(seen, 3);   // at 100, 102, 104
        QCOMPARE(row.extent(unit), QRectF(QPointF(0, 0), QPointF(199998.5, 0.5)));
        // Explicit.
        Repetition list;
        list.type = Repetition::Type::Explicit;
        list.offsets = {{0, 0}, {5, 5}, {100, 0}};
        seen = 0;
        list.forEach(unit, QRectF(QPointF(4, 4), QPointF(6, 6)), [&](QPointF o) { return ++seen, o == QPointF(5, 5); });
        QCOMPARE(seen, 1);
        QCOMPARE(list.count(), quint64(3));
        // Four billion copies, six in sight: found at once, not one by one.
        Repetition huge;
        huge.columns = 65536;
        huge.rows = 65536;
        huge.v1 = QPointF(1, 0);
        huge.v2 = QPointF(0, 1);
        QElapsedTimer t;
        t.start();
        seen = 0;
        huge.forEach(unit, QRectF(QPointF(40000.2, 50000.2), QPointF(40002.7, 50001.7)), [&](QPointF) { return ++seen, true; });
        QCOMPARE(seen, 6);
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));
    }

    // gdstk's reading of OASIS stops at what there is: a read past a
    // compressed block's data is zeros and an error, not the memory after
    // it; a count of more than is left is refused before it is allocated.
    void anOasisReadStopsAtWhatThereIs()
    {
        gdstk::OasisStream in = {};
        in.data = static_cast<uint8_t*>(gdstk::allocate(4));
        for (uint8_t i = 0; i < 4; ++i) in.data[i] = uint8_t(i + 1);
        in.data_size = 4;
        in.cursor = in.data;
        QVERIFY(gdstk::oasis_count_fits(in, 4));
        QCOMPARE(in.error_code, gdstk::ErrorCode::NoError);
        QVERIFY(!gdstk::oasis_count_fits(in, 5));
        QCOMPARE(in.error_code, gdstk::ErrorCode::InvalidFile);
        in.error_code = gdstk::ErrorCode::NoError;
        uint8_t eight[8];
        memset(eight, 0xAB, sizeof eight);
        QCOMPARE(gdstk::oasis_read(eight, 1, 8, in), gdstk::ErrorCode::InputFileError);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(eight), 8), QByteArray("\x01\x02\x03\x04\0\0\0\0", 8));
        QVERIFY(in.data == nullptr);   // (the block is done)
    }

    // A placement mirrored and turned is mirrored first, about the x axis,
    // then turned, then magnified about its origin and moved - as GDSII's
    // STRANS and OASIS have it.
    void aPlacementIsMirroredThenTurned()
    {
        Instance inst;
        inst.origin = QPointF(5, 0);
        inst.rotation = 90;
        inst.mirrored = true;
        inst.magnification = 2;
        const QTransform t = inst.transform();
        QCOMPARE(t.map(QPointF(1, 0)), QPointF(5, 2));    // (1, 0) -> (2, 0) -> (0, 2)
        QCOMPARE(t.map(QPointF(0, 1)), QPointF(7, 0));    // -> (0, -1) -> (1, 0), doubled
    }

    // gdstk's convex hull without Qhull: the corners counterclockwise,
    // nothing inside nor on an edge; a line's two ends; one point.
    void theConvexHullIsOurOwn()
    {
        gdstk::Array<gdstk::Vec2> points = {};
        for (gdstk::Vec2 p : {gdstk::Vec2{0, 0}, gdstk::Vec2{4, 0}, gdstk::Vec2{4, 3}, gdstk::Vec2{0, 3}, gdstk::Vec2{2, 1},
                              gdstk::Vec2{2, 0}, gdstk::Vec2{1, 2}, gdstk::Vec2{4, 1.5}})
            points.append(p);
        gdstk::Array<gdstk::Vec2> hull = {};
        gdstk::convex_hull(points, hull);
        QCOMPARE(hull.count, uint64_t(4));
        QPolygonF h;
        for (uint64_t i = 0; i < hull.count; ++i) h << QPointF(hull[i].x, hull[i].y);
        QCOMPARE(h, QPolygonF({{0, 0}, {4, 0}, {4, 3}, {0, 3}}));
        hull.clear();
        points.clear();
        for (double t : {0.0, 1.0, 2.0, 3.0, 4.0}) points.append(gdstk::Vec2{t, -t});
        gdstk::convex_hull(points, hull);
        QCOMPARE(hull.count, uint64_t(2));
        QCOMPARE(QPointF(hull[0].x, hull[0].y), QPointF(0, 0));
        QCOMPARE(QPointF(hull[1].x, hull[1].y), QPointF(4, -4));
        hull.clear();
        points.clear();
        for (int i = 0; i < 6; ++i) points.append(gdstk::Vec2{1, 1});
        gdstk::convex_hull(points, hull);
        QCOMPARE(hull.count, uint64_t(1));
        hull.clear();
        points.clear();
    }

    // ---- The tab

    // Opened from its file: the top cell shown whole, the cells and the
    // layers beside it; a cell's cells and how often each is placed; a
    // layer hidden, fewer shapes drawn; fewer levels, frames for the rest.
    void aLayoutOpensInATabWithItsCellsAndLayers()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        QVERIFY2(doc->problem().isEmpty(), qPrintable(doc->problem()));
        QVERIFY(QucsApp::isLayoutDocument(doc));
        QVERIFY(doc->layout());
        QCOMPARE(doc->layout()->cells.at(doc->shownCell()).name, QString("TOP"));
        // The tree: TOP, and in it INV ×3 and VIA ×6.
        QTreeWidget* cells = doc->cellTree();
        QCOMPARE(cells->topLevelItemCount(), 1);
        QTreeWidgetItem* top = cells->topLevelItem(0);
        QCOMPARE(top->text(0), QString("TOP"));
        cells->expandItem(top);
        QStringList children;
        for (int i = 0; i < top->childCount(); ++i) children << top->child(i)->text(0) + " " + top->child(i)->text(1);
        QCOMPARE(children, QStringList({"INV ×3", "VIA ×6"}));
        // The layers, ticked, with what is on each.
        QTreeWidget* layers = doc->layerList();
        QCOMPARE(layers->topLevelItemCount(), 6);
        QCOMPARE(layers->topLevelItem(3)->text(0), QString("4/0"));
        QCOMPARE(layers->topLevelItem(3)->text(1), QString("10"));
        QCOMPARE(layers->topLevelItem(0)->checkState(0), Qt::Checked);
        // What it has, under the view.
        QVERIFY2(doc->infoLabel()->text().startsWith("3 cells, 14 shapes and 1 text on 6 layers; a database unit of 0.001 µm"),
                 qPrintable(doc->infoLabel()->text()));
        QVERIFY(doc->infoLabel()->width() > doc->positionLabel()->width());
        // The whole cell in sight, drawn.
        LayoutView* view = doc->view();
        view->repaint();
        const QRectF seen = view->visibleRegion();
        QVERIFY2(seen.contains(QRectF(QPointF(0, -1), QPointF(32, 20.2))), "the whole cell is in sight");
        const quint64 all = view->lastStats().polygons + view->lastStats().dots;
        QVERIFY2(all >= 10, qPrintable(QString::number(all)));
        // 1/0 hidden: three rectangles fewer.
        layers->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
        QVERIFY(!view->styles().at(0).visible);
        view->repaint();
        QCOMPARE(view->lastStats().polygons + view->lastStats().dots, all - 3);
        layers->topLevelItem(0)->setCheckState(0, Qt::Checked);
        // No level below TOP: its four placements as frames (the array's six).
        doc->depthBox()->setValue(0);
        view->repaint();
        QCOMPARE(view->lastStats().frames, quint64(3 + 6));
        QCOMPARE(view->lastStats().polygons + view->lastStats().dots, quint64(1 + 10));
        doc->depthBox()->setValue(doc->depthBox()->maximum());
        QCOMPARE(doc->depthBox()->text(), QString("all"));
        view->repaint();
        QCOMPARE(view->lastStats().frames, quint64(0));
        // A click in the tree shows that cell.
        QTreeWidgetItem* inv = top->child(0);
        emit cells->itemClicked(inv, 0);
        QCOMPARE(doc->layout()->cells.at(doc->shownCell()).name, QString("INV"));
        // Nothing to save, simulate or paste: it is read.
        QVERIFY(!doc->getDocChanged());
    }

    // A click selects the shape under the pointer, said under the view;
    // clicked again where two lie, the other; Escape lets it go.
    void aClickSelectsTheShapeUnderIt()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->showRegion(QRectF(QPointF(9, -1), QPointF(13, 2)));
        view->repaint();
        click(view, view->toPixel(QPointF(11, 0.2)));
        QVERIFY(view->selection().has_value());
        QVERIFY2(doc->infoLabel()->text().contains("Polygon on 1/0") && doc->infoLabel()->text().contains("in INV (TOP › INV)"),
                 qPrintable(doc->infoLabel()->text()));
        QVERIFY2(doc->infoLabel()->text().contains("2 × 1 µm from 10, 0 to 12, 1"), qPrintable(doc->infoLabel()->text()));
        QVERIFY2(doc->infoLabel()->text().contains("1 = net_A"), qPrintable(doc->infoLabel()->text()));
        // Where the path lies on the rectangle: the path (the smaller) first.
        click(view, view->toPixel(QPointF(11, 0.5)));
        QVERIFY2(doc->infoLabel()->text().startsWith("Path on 2/0"), qPrintable(doc->infoLabel()->text()));
        QVERIFY2(doc->infoLabel()->text().contains("0.2 µm wide, ends flush"), qPrintable(doc->infoLabel()->text()));
        click(view, view->toPixel(QPointF(11, 0.5)));
        QVERIFY2(doc->infoLabel()->text().startsWith("Polygon on 1/0"), qPrintable(doc->infoLabel()->text()));
        // The text, near its point.
        click(view, view->toPixel(QPointF(12, 0.5)) + QPointF(2, 0));
        QVERIFY(view->selection().has_value());
        QVERIFY2(doc->infoLabel()->text().contains("Text \"OUT\" on 10/0"), qPrintable(doc->infoLabel()->text()));
        // Copied: what it is and its points.
        click(view, view->toPixel(QPointF(11, 0.2)));
        doc->copySelection();
        QVERIFY2(QApplication::clipboard()->text().contains("points (µm): 10, 0; 12, 0; 12, 1; 10, 1"), qPrintable(QApplication::clipboard()->text()));
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(!view->selection().has_value());
        // Nothing there: nothing selected.
        click(view, view->toPixel(QPointF(12.8, 1.8)));
        QVERIFY(!view->selection().has_value());
    }

    // A ruler, dragged in ruler mode, snaps to the corners it comes near:
    // from (10, 0) to (12, 1), √5 µm; Shift keeps it across.
    void theRulerMeasuresFromCornerToCorner()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->showRegion(QRectF(QPointF(9, -1), QPointF(13, 2)));
        view->setMode(LayoutView::Mode::Ruler);
        drag(view, view->toPixel(QPointF(10, 0)) + QPointF(2, -2), view->toPixel(QPointF(12, 1)) + QPointF(-2, 2));
        QCOMPARE(view->rulers().size(), 1);
        QCOMPARE(view->rulers().last(), QLineF(QPointF(10, 0), QPointF(12, 1)));
        QVERIFY2(doc->infoLabel()->text().startsWith("Ruler: 2.236 µm (dx 2, dy 1)"), qPrintable(doc->infoLabel()->text()));
        // Where nothing is near: on the database grid.
        Query here;
        here.cell = doc->shownCell();
        QCOMPARE(snap(*doc->layout(), here, QPointF(5.00049, 7.00012), 0.01), QPointF(5, 7));
        // With Shift: across only.
        drag(view, view->toPixel(QPointF(10, 0)), view->toPixel(QPointF(12, 0.37)), Qt::ShiftModifier);
        QCOMPARE(view->rulers().size(), 2);
        QCOMPARE(view->rulers().last().p1().y(), view->rulers().last().p2().y());
        QTest::keyClick(view, Qt::Key_Escape);   // the rulers
        QVERIFY(view->rulers().isEmpty());
        QTest::keyClick(view, Qt::Key_Escape);   // ruler mode
        QCOMPARE(view->mode(), LayoutView::Mode::Select);
    }

    // The find bar finds cells by name - a part of it, or a wildcard - lists
    // them in the cell panel and shows each in turn.
    void aCellIsFoundByItsName()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        app->slotEditFind();
        QVERIFY(doc->findField()->isVisible());
        QTest::keyClicks(doc->findField(), "v");
        QCOMPARE(doc->findCount(), 2);   // INV, VIA
        QCOMPARE(doc->cellTree()->topLevelItemCount(), 2);
        QCOMPARE(doc->layout()->cells.at(doc->shownCell()).name, QString("INV"));
        QTest::keyClick(doc->findField(), Qt::Key_Return);
        QCOMPARE(doc->layout()->cells.at(doc->shownCell()).name, QString("VIA"));
        doc->findField()->setText("T?P");
        QCOMPARE(doc->findCount(), 1);
        QCOMPARE(doc->layout()->cells.at(doc->shownCell()).name, QString("TOP"));
        doc->findField()->setText("nothing");
        QCOMPARE(doc->findCount(), 0);
        doc->hideSearch();
        QCOMPARE(doc->cellTree()->topLevelItemCount(), 1);
    }

    // Written again by another program: read again, the view where it
    // was, a hidden layer still hidden, the new cell there.
    void aLayoutWrittenAgainIsReadAgainWhereItWas()
    {
        writeSample(path("changing.gds"), false);
        LayoutDoc* doc = open(path("changing.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->showRegion(QRectF(QPointF(9, -1), QPointF(13, 2)));
        const QPointF center = view->center();
        const double scale = view->scale();
        doc->setLayerVisible(1, false);   // 2/0
        QSignalSpy reloaded(doc, &LayoutDoc::reloaded);
        QTest::qWait(1100);   // (a file time of another second)
        writeSample(path("changing.gds"), false, 0, true);
        QVERIFY(reloaded.wait(20000));
        QVERIFY(doc->layout()->cellIndex("EXTRA") >= 0);
        QCOMPARE(view->center(), center);
        QCOMPARE(view->scale(), scale);
        QVERIFY(!view->styles().at(doc->layout()->layerIndex({2, 0})).visible);
        QCOMPARE(doc->layerList()->topLevelItemCount(), 7);
    }

    // The ⋯ menu's Background: the canvas light or dark whatever the
    // application's theme, in every layout tab, kept for the next one; or
    // the application's again.
    void theCanvasIsLightOrDarkWhateverTheTheme()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        QCOMPARE(view->theme(), LayoutView::Theme::Application);
        doc->setGridShown(false);   // (the background alone)
        QMenu* background = doc->menuButton()->menu()->findChild<QMenu*>("layoutBackground");
        QVERIFY(background != nullptr);
        QStringList choices;
        for (QAction* a : background->actions())
            if (!a->isSeparator()) choices << a->text();
        QCOMPARE(choices, QStringList({"Like the Application", "Light", "Dark", "Colours in Contrast to It"}));
        const auto choose = [](QMenu* menu, const QString& text) {
            for (QAction* a : menu->actions())
                if (a->text() == text) a->trigger();
        };
        // (a corner of the view: its margin, nothing drawn there)
        const auto corner = [view] { return view->picture().pixelColor(2, 2); };
        choose(background, "Dark");
        QCOMPARE(view->theme(), LayoutView::Theme::Dark);
        QVERIFY2(corner().lightness() < 40, qPrintable(corner().name()));
        QCOMPARE(QucsSettingsFile().value("LayoutViewer/background").toString(), QString("dark"));
        // Another tab: dark as it opens; Light chosen there, both light.
        LayoutDoc* other = open(path("sample.oas"));
        QVERIFY(other != nullptr && other != doc);
        QCOMPARE(other->view()->theme(), LayoutView::Theme::Dark);
        QMenu* otherBackground = other->menuButton()->menu()->findChild<QMenu*>("layoutBackground");
        choose(otherBackground, "Light");
        QCOMPARE(other->view()->theme(), LayoutView::Theme::Light);
        QCOMPARE(view->theme(), LayoutView::Theme::Light);
        QVERIFY2(corner().lightness() > 240, qPrintable(corner().name()));
        // The menu shows which.
        emit background->aboutToShow();
        QStringList checked;
        for (QAction* a : background->actions())
            if (a->isChecked()) checked << a->text();
        QCOMPARE(checked, QStringList({"Light", "Colours in Contrast to It"}));
        // The application's again: its Base.
        choose(background, "Like the Application");
        QCOMPARE(view->theme(), LayoutView::Theme::Application);
        QCOMPARE(corner(), view->palette().color(QPalette::Base));
        QCOMPARE(QucsSettingsFile().value("LayoutViewer/background").toString(), QString("app"));
        doc->setGridShown(true);
    }

    // The layers' colours on a light canvas darkened, on a dark one
    // lightened, till they stand out from it by 3.5:1 - their hue kept,
    // no more changed than it takes; as they were when they stood out
    // already, or when the Background menu says not to. The layer list's
    // swatches, and a print on white, alike.
    void theLayersStandOutFromTheCanvas()
    {
        const QColor white(Qt::white), dark(22, 22, 24);
        QVERIFY(qAbs(contrastRatio(Qt::black, Qt::white) - 21) < 1e-6);
        QCOMPARE(contrastRatio(QColor("#ff80a8"), QColor("#ff80a8")), 1.0);
        const auto hueDistance = [](const QColor& a, const QColor& b) {
            const double d = qAbs(a.hslHueF() - b.hslHueF());
            return std::min(d, 1 - d);
        };
        // The palette's sixteen and a .lyp's pastels, on white and on
        // dark: each enough, in the 8 bits it is drawn in; a hue kept.
        QStringList colours;
        for (int i = 0; i < 16; ++i) colours << paletteStyle(i).frame.name();
        colours << "#d6b656" << "#fdae6b" << "#9ecae1" << "#c7c7c7" << "#7f7f7f";
        int darkened = 0, lightened = 0;
        for (const QString& name : colours) {
            const QColor c(name);
            for (const QColor& bg : {white, dark}) {
                const QColor shown = standingOut(c, bg, LayerContrast);
                const QString what = name + " on " + bg.name() + ": " + shown.name();
                QVERIFY2(contrastRatio(shown, bg) >= LayerContrast, qPrintable(what));
                QCOMPARE(shown.rgba(), QColor::fromRgba(shown.rgba()).rgba());
                if (contrastRatio(c, bg) >= LayerContrast) {
                    QVERIFY2(shown == c, qPrintable(what));
                    continue;
                }
                // Just enough: a step less would not be.
                QVERIFY2(contrastRatio(shown, bg) < LayerContrast + 0.15, qPrintable(what));
                if (c.hslSaturationF() > 0.05) QVERIFY2(hueDistance(shown, c) < 0.02, qPrintable(what));
                else QVERIFY2(shown.red() == shown.green() && shown.green() == shown.blue(), qPrintable(what));
                (bg == white ? darkened : lightened)++;
                QVERIFY2(bg == white ? shown.lightnessF() < c.lightnessF() : shown.lightnessF() > c.lightnessF(), qPrintable(what));
            }
        }
        QVERIFY2(darkened >= 10 && lightened >= 2, qPrintable(QString("%1 %2").arg(darkened).arg(lightened)));
        // No background, or one in the middle: as it is, or as far as it goes.
        QCOMPARE(standingOut(QColor("#ff80a8"), QColor(), LayerContrast), QColor("#ff80a8"));
        QVERIFY(standingOut(QColor("#808080"), QColor("#777777"), LayerContrast).isValid());
        LayerStyle style = paletteStyle(0);
        style.fill = QColor("#fdae6b");
        const LayerStyle shownStyle = standingOut(style, white);
        QCOMPARE(shownStyle.frame, standingOut(style.frame, white, LayerContrast));
        QCOMPARE(shownStyle.fill, standingOut(style.fill, white, LayerContrast));
        QCOMPARE(shownStyle.pattern, style.pattern);

        // A tab: its six layers in the palette's first six colours - the
        // five pastels too pale for white, the red (10/0's, a text's: in
        // ink) not.
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        doc->setGridShown(false);   // (the layers alone)
        doc->setCanvasTheme(LayoutView::Theme::Light);
        LayoutView* view = doc->view();
        QVERIFY(view->colorsAdapted());
        const auto drawn = [view] {
            QSet<QRgb> set;
            const QImage picture = view->picture().convertToFormat(QImage::Format_RGB32);
            for (int y = 0; y < picture.height(); ++y)
                for (int x = 0; x < picture.width(); ++x) set.insert(picture.pixel(x, y));
            return set;
        };
        const auto swatchCorner = [doc](int layer) {
            return doc->layerList()->topLevelItem(layer)->icon(0).pixmap(QSize(16, 12)).toImage().pixelColor(0, 0);
        };
        QCOMPARE(doc->layout()->layers.size(), 6);
        QSet<QRgb> seen = drawn();
        for (int i = 0; i < 6; ++i) {
            const QColor pale = paletteStyle(i).frame, deeper = standingOut(pale, white, LayerContrast);
            QCOMPARE(contrastRatio(pale, white) < LayerContrast, i < 5);
            QCOMPARE(deeper == pale, i == 5);
            if (i < 5) QVERIFY2(seen.contains(deeper.rgb()) && !seen.contains(pale.rgb()), qPrintable(pale.name() + " " + deeper.name()));
            QCOMPARE(swatchCorner(i), deeper);
        }
        // Printed: on white, alike.
        QImage page(600, 400, QImage::Format_RGB32);
        {
            QPainter painter(&page);
            doc->print(nullptr, &painter, false, false);
        }
        QSet<QRgb> printed;
        for (int y = 0; y < page.height(); ++y)
            for (int x = 0; x < page.width(); ++x) printed.insert(page.pixel(x, y));
        QVERIFY(printed.contains(standingOut(paletteStyle(0).frame, white, LayerContrast).rgb()));
        QVERIFY(!printed.contains(paletteStyle(0).frame.rgb()));
        // Dark: the pink stands out as it is; a blue given to 1/0, too
        // dark there, lightened.
        doc->setCanvasTheme(LayoutView::Theme::Dark);
        QVERIFY(drawn().contains(paletteStyle(0).frame.rgb()));
        QCOMPARE(swatchCorner(0), paletteStyle(0).frame);
        // (a swatch on the canvas's background)
        const QImage swatch = doc->layerList()->topLevelItem(0)->icon(0).pixmap(QSize(16, 12)).toImage();
        int canvas = 0;
        for (int y = 0; y < swatch.height(); ++y)
            for (int x = 0; x < swatch.width(); ++x) canvas += swatch.pixelColor(x, y) == view->colors().background;
        QVERIFY2(canvas > 20, qPrintable(QString::number(canvas)));
        QVector<LayerStyle> styles = view->styles();
        styles[0].frame = styles[0].fill = QColor(0, 0, 255);
        view->setStyles(styles);
        const QColor lighter = standingOut(QColor(0, 0, 255), view->colors().background, LayerContrast);
        QVERIFY(lighter != QColor(0, 0, 255));
        seen = drawn();
        QVERIFY(seen.contains(lighter.rgb()) && !seen.contains(qRgb(0, 0, 255)));
        // Not in contrast: the colours as their styles have them, in every
        // layout tab, kept for the next.
        QMenu* background = doc->menuButton()->menu()->findChild<QMenu*>("layoutBackground");
        QAction* contrast = nullptr;
        for (QAction* a : background->actions())
            if (a->text() == "Colours in Contrast to It") contrast = a;
        QVERIFY(contrast != nullptr && contrast->isCheckable());
        emit background->aboutToShow();
        QVERIFY(contrast->isChecked());
        contrast->trigger();
        QVERIFY(!view->colorsAdapted());
        QCOMPARE(QucsSettingsFile().value("LayoutViewer/contrast").toBool(), false);
        seen = drawn();
        QVERIFY(seen.contains(qRgb(0, 0, 255)) && !seen.contains(lighter.rgb()));
        QCOMPARE(swatchCorner(0), QColor(0, 0, 255));
        LayoutDoc* other = open(path("sample.oas"));
        QVERIFY(other != nullptr && other != doc);
        QVERIFY(!other->view()->colorsAdapted());
        other->setColorsAdapted(true);
        QVERIFY(view->colorsAdapted());
        QCOMPARE(swatchCorner(0), lighter);
        doc->setCanvasTheme(LayoutView::Theme::Application);
        doc->setGridShown(true);
    }

    // The grid behind the layout, as KLayout has it: a dot every step - 1,
    // 2 or 5 times a power of ten µm, 16 pixels apart at least - those on
    // a power of ten stronger, a scale in the lower left; lines or crosses
    // instead; shown or hidden with its button, G or the ⋯ menu, in every
    // layout tab, kept for the next.
    void aGridIsBehindTheLayout()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        QVERIFY(view->gridShown());
        QVERIFY(doc->gridButton()->isChecked());
        QCOMPARE(view->gridStyle(), LayoutView::GridStyle::Dots);
        // Its step, at any scale.
        const auto powerOfTen = [](double v) {
            const double e = std::log10(v);
            return qAbs(e - std::round(e)) < 1e-9;
        };
        for (double scale : {0.0123, 0.37, 1.0, 3.0, 16.0, 25.0, 160.0, 999.0, 12345.0}) {
            view->setView(QPointF(), scale);
            const double step = view->gridStep(), major = view->gridMajorStep();
            const QString what = QString("%1 px/µm: %2, %3").arg(scale).arg(step).arg(major);
            QVERIFY2(step * scale >= LayoutView::MinGridPixels - 1e-9 && step * scale < 2.5 * LayoutView::MinGridPixels + 1e-9, qPrintable(what));
            QVERIFY2(powerOfTen(step) || powerOfTen(step / 2) || powerOfTen(step / 5), qPrintable(what));
            QVERIFY2(powerOfTen(major) && major > step * (1 + 1e-9) && major <= 10 * step * (1 + 1e-9), qPrintable(what));
        }
        // Never finer than the database unit.
        view->setView(QPointF(), 1e5);
        QCOMPARE(view->gridStep(), doc->layout()->dbu);

        // Where nothing is: a light canvas, 10 px a µm - a dot every 2 µm
        // (20 px), stronger every 10.
        doc->setCanvasTheme(LayoutView::Theme::Light);
        view->setView(QPointF(-1000, -1000), 10);
        QCOMPARE(view->gridStep(), 2.0);
        QCOMPARE(view->gridMajorStep(), 10.0);
        const auto at = [view](double x, double y) {
            const QPointF p = view->toPixel(QPointF(x, y));
            return QPoint(int(std::round(p.x())), int(std::round(p.y())));
        };
        // The darkest of the pixels about one.
        const auto darkest = [](const QImage& picture, QPoint p) {
            int least = 255;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) least = std::min(least, picture.pixelColor(p + QPoint(dx, dy)).lightness());
            return least;
        };
        QImage picture = view->picture();
        const QPoint strong = at(-1000, -1000), weak = at(-998, -1000), between = at(-999, -999);
        QVERIFY2(darkest(picture, strong) < darkest(picture, weak), qPrintable(QString("%1 %2").arg(darkest(picture, strong)).arg(darkest(picture, weak))));
        QVERIFY2(darkest(picture, weak) < 230, qPrintable(QString::number(darkest(picture, weak))));
        QCOMPARE(darkest(picture, between), 255);
        // The scale: its bar in the lower left, in ink.
        const auto scaleDrawn = [view](const QImage& picture) {
            for (int y = view->height() - 14; y < view->height() - 6; ++y)
                for (int x = 14; x < 30; ++x)
                    if (picture.pixelColor(x, y).lightness() < 90) return true;
            return false;
        };
        QVERIFY(scaleDrawn(picture));
        // G: hidden; the button, the setting, every tab alike.
        QTest::keyClick(view, Qt::Key_G);
        QVERIFY(!view->gridShown());
        QVERIFY(!doc->gridButton()->isChecked());
        QCOMPARE(QucsSettingsFile().value("LayoutViewer/grid").toBool(), false);
        picture = view->picture();
        QCOMPARE(darkest(picture, strong), 255);
        QVERIFY(!scaleDrawn(picture));
        LayoutDoc* other = open(path("sample.oas"));
        QVERIFY(other != nullptr && other != doc);
        QVERIFY(!other->view()->gridShown());
        QVERIFY(!other->gridButton()->isChecked());
        other->gridButton()->click();
        QVERIFY(view->gridShown());
        QVERIFY(doc->gridButton()->isChecked());
        QTest::keyClick(view, Qt::Key_G, Qt::MetaModifier);   // (not G: no app's shortcut has it either)
        QVERIFY(view->gridShown());
        // View > Show Grid (Alt+G) too, when a layout is in front: it says
        // so, and shows whether it is shown.
        QCOMPARE(app->showGrid->text(), QString("Show Grid (all layouts)"));
        QVERIFY(app->showGrid->isChecked());
        app->showGrid->trigger();
        QVERIFY(!view->gridShown() && !other->view()->gridShown());
        QVERIFY(!app->showGrid->isChecked());
        QVERIFY(!other->gridButton()->isChecked());
        other->gridButton()->click();
        QVERIFY(app->showGrid->isChecked());
        // A schematic in front: its own grid again; the layout: the layouts'.
        app->slotFileNew();
        QCOMPARE(app->showGrid->text(), QString("Show Grid (current document)"));
        app->DocumentTab->setCurrentWidget(doc);
        QCOMPARE(app->showGrid->text(), QString("Show Grid (all layouts)"));
        QVERIFY(app->showGrid->isChecked());
        // The ⋯ menu's Grid: shown or not, and how.
        QMenu* grid = doc->menuButton()->menu()->findChild<QMenu*>("layoutGrid");
        QVERIFY(grid != nullptr);
        QStringList items;
        QHash<QString, QAction*> action;
        for (QAction* a : grid->actions())
            if (!a->isSeparator()) {
                items << a->text();
                action.insert(a->text(), a);
            }
        QCOMPARE(items, QStringList({"Show the Grid", "Dots", "Lines", "Crosses"}));
        emit grid->aboutToShow();
        QVERIFY(action["Show the Grid"]->isChecked());
        QVERIFY(action["Dots"]->isChecked());
        // Lines: a weak one through each step, a strong one through each
        // power of ten; nothing between.
        action["Lines"]->trigger();
        QCOMPARE(view->gridStyle(), LayoutView::GridStyle::Lines);
        QCOMPARE(other->view()->gridStyle(), LayoutView::GridStyle::Lines);
        QCOMPARE(QucsSettingsFile().value("LayoutViewer/gridStyle").toString(), QString("lines"));
        picture = view->picture();
        const QPoint onWeak = at(-998, -999.3), onStrong = at(-1000, -999.3);
        QVERIFY(darkest(picture, onWeak) < 255);
        QVERIFY(darkest(picture, onStrong) < darkest(picture, onWeak));
        QCOMPARE(darkest(picture, between), 255);
        // Crosses: their arms, longer on a power of ten.
        action["Crosses"]->trigger();
        QCOMPARE(view->gridStyle(), LayoutView::GridStyle::Crosses);
        picture = view->picture();
        QVERIFY(picture.pixelColor(weak + QPoint(2, 0)).lightness() < 255);
        QCOMPARE(picture.pixelColor(weak + QPoint(4, 0)).lightness(), 255);
        QVERIFY(picture.pixelColor(strong + QPoint(4, 0)).lightness() < 255);
        QCOMPARE(picture.pixelColor(strong + QPoint(3, 3)).lightness(), 255);
        emit grid->aboutToShow();
        QVERIFY(action["Crosses"]->isChecked() && !action["Dots"]->isChecked());
        action["Show the Grid"]->trigger();
        QVERIFY(!view->gridShown() && !other->view()->gridShown());
        // Behind the layout: through a shape filled solid, none of it.
        QVector<LayerStyle> solid = view->styles();
        solid[0].pattern = Qt::SolidPattern;
        view->setStyles(solid);
        doc->setGridStyle(LayoutView::GridStyle::Lines);
        doc->setGridShown(true);
        view->setView(QPointF(11, 0.8), 200);   // in INV's rectangle on 1/0, placed at (10, 0)
        QCOMPARE(view->gridStep(), 0.1);
        picture = view->picture();
        QCOMPARE(picture.pixelColor(at(11, 0.8)), view->shownStyle(solid[0]).fill);
        QCOMPARE(picture.pixelColor(at(11.05, 0.85)), view->shownStyle(solid[0]).fill);
        doc->setGridStyle(LayoutView::GridStyle::Dots);
        doc->setCanvasTheme(LayoutView::Theme::Application);
    }

    // A KLayout .lyp beside the layout: its colours, fills, names and
    // hidden layers - in a group too; one of another name, chosen.
    void aLypBesideItGivesTheLayersTheirLook()
    {
        writeSample(path("styled/chip.gds"), false);
        QVERIFY(write(path("styled/chip.lyp"),
                      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<layer-properties>\n"
                      " <properties><frame-color>#ff0000</frame-color><fill-color>#ff0000</fill-color>"
                      "<dither-pattern>I1</dither-pattern><visible>true</visible><name>M1</name><source>1/0@1</source></properties>\n"
                      " <properties><name>group</name><source>*/*@*</source>\n"
                      "  <group-members><frame-color>#00ff00</frame-color><fill-color>#00ff00</fill-color>"
                      "<dither-pattern>I0</dither-pattern><visible>false</visible><source>POLY 2/0@1</source></group-members>\n"
                      " </properties>\n"
                      "</layer-properties>\n"));
        QCOMPARE(layerPropertiesBeside(path("styled/chip.gds")), path("styled/chip.lyp"));
        LayoutDoc* doc = open(path("styled/chip.gds"));
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->layerPropertiesFile(), path("styled/chip.lyp"));
        const QVector<LayerStyle>& styles = doc->view()->styles();
        QCOMPARE(styles.at(0).frame, QColor("#ff0000"));
        QCOMPARE(styles.at(0).pattern, Qt::NoBrush);
        QCOMPARE(styles.at(0).name, QString("M1"));
        QCOMPARE(styles.at(1).frame, QColor("#00ff00"));
        QCOMPARE(styles.at(1).pattern, Qt::SolidPattern);
        QCOMPARE(styles.at(1).name, QString("POLY"));
        QVERIFY(!styles.at(1).visible);
        QCOMPARE(doc->layerList()->topLevelItem(0)->text(0), QString("1/0  M1"));
        QCOMPARE(doc->layerList()->topLevelItem(1)->checkState(0), Qt::Unchecked);
        // The palette again; and one chosen by name.
        doc->usePalette();
        QCOMPARE(doc->view()->styles().at(0).frame, paletteStyle(0).frame);
        QVERIFY(doc->loadLayerProperties(path("styled/chip.lyp")));
        QCOMPARE(doc->view()->styles().at(0).frame, QColor("#ff0000"));
        QString error;
        QVERIFY(write(path("styled/bad.lyp"), "<layer-properties><properties>"));
        QVERIFY(!doc->loadLayerProperties(path("styled/bad.lyp"), &error));
        QVERIFY(!error.isEmpty());
    }

    // A million shapes: read in the background (the window answers
    // meanwhile), drawn whole at its full extent and a corner of it in
    // time; a read cancelled, and read again.
    void aMillionShapesAreReadAndDrawnInTime()
    {
        const QString big = path("million.oas");
        {
            gdstk::Library lib = {};
            lib.init("BIG", 1e-6, 1e-9);
            gdstk::Cell* top = newCell(lib, "MILLION");
            for (int i = 0; i < 1000; ++i)
                for (int j = 0; j < 1000; ++j)
                    addRectangle(top, {i * 2.0, j * 2.0}, {i * 2.0 + 1, j * 2.0 + 1}, uint32_t((i + j) % 4));
            QCOMPARE(lib.write_oas(QFile::encodeName(big).constData(), 0, 6, OASIS_CONFIG_DETECT_ALL), gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        // Cancelled as it starts: said, and nothing left reading.
        QVERIFY(app->gotoPage(big));
        auto* doc = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QVERIFY(doc->isLoading());
        QVERIFY(doc->cancelButton()->isVisible());
        QTest::mouseClick(doc->cancelButton(), Qt::LeftButton);
        QVERIFY(!doc->isLoading());
        QVERIFY2(doc->problem().contains("cancelled"), qPrintable(doc->problem()));
        waitForReads();
        QCOMPARE(readsGoing(), 0);
        // Read again; the window answers while it reads.
        QElapsedTimer t;
        t.start();
        QMetaObject::invokeMethod(doc, [doc] { doc->reload(); });
        QTest::qWait(1);
        int answered = 0;
        while (doc->isLoading() && t.elapsed() < 240000) {
            QTest::qWait(10);
            ++answered;
        }
        const qint64 readMs = t.elapsed();
        QVERIFY(doc->layout());
        QCOMPARE(doc->layout()->shapeCount, quint64(1000000));
        QVERIFY2(answered > 3, "the window answers while it reads");
        LayoutView* view = doc->view();
        view->fit();   // (drawn again, now)
        const int before = view->renders();
        t.restart();
        view->repaint();
        const qint64 fullMs = t.elapsed();
        QCOMPARE(view->renders(), before + 1);
        QVERIFY(!view->lastStats().incomplete);
        // Each square under a pixel: a dot each, once a pixel.
        QCOMPARE(view->lastStats().polygons, quint64(0));
        QVERIFY(view->lastStats().dots > 1000);
        QVERIFY(view->lastStats().dots < quint64(view->width()) * quint64(view->height()) * 4);
        view->showRegion(QRectF(QPointF(0, 0), QPointF(40, 40)));
        t.restart();
        view->repaint();
        const qint64 cornerMs = t.elapsed();
        QCOMPARE(view->renders(), before + 2);
        // What is in sight, no more: the rectangles [2i, 2i + 1] that meet it.
        const QRectF sight = view->visibleRegion().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
        const auto across = [](double from, double to) {
            quint64 n = 0;
            for (int i = 0; i < 1000; ++i) n += 2.0 * i <= to && 2.0 * i + 1 >= from;
            return n;
        };
        QVERIFY(view->lastStats().polygons >= 20 * 20);
        QVERIFY2(view->lastStats().polygons <= across(sight.left(), sight.right()) * across(sight.top(), sight.bottom()),
                 qPrintable(QString::number(view->lastStats().polygons)));
        qInfo("A million shapes: read in %lld ms, drawn whole in %lld ms, a corner in %lld ms", readMs, fullMs, cornerMs);
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer)) || !defined(NDEBUG)
        const qint64 slower = 10;   // (a build with sanitizers, or for debugging)
#else
        const qint64 slower = 1;
#endif
        QVERIFY2(readMs < 20000 * slower, qPrintable(QString::number(readMs)));
        QVERIFY2(fullMs < 3000 * slower, qPrintable(QString::number(fullMs)));
        QVERIFY2(cornerMs < 500 * slower, qPrintable(QString::number(cornerMs)));
    }

    // ---- The app

    // A layout is opened by the Open dialog's filter, the File Browser and
    // links as its own kind; Save As copies it; it is not simulated.
    void theAppKnowsALayout()
    {
        QVERIFY(QucsApp::isLayoutFile("chip.gds"));
        QVERIFY(QucsApp::isLayoutFile("chip.GDS2"));
        QVERIFY(QucsApp::isLayoutFile("chip.gdsii"));
        QVERIFY(QucsApp::isLayoutFile("chip.gds.gz"));
        QVERIFY(QucsApp::isLayoutFile("chip.oas"));
        QVERIFY(QucsApp::isLayoutFile("chip.oasis"));
        QVERIFY(!QucsApp::isLayoutFile("chip.gz"));
        QVERIFY(!QucsApp::isLayoutFile(".gds"));
        QVERIFY(qucs_s::links::opensItself("chip.oas"));
        const qucs_s::files::Kind kind = qucs_s::files::kindOf(QFileInfo(path("sample.gds")));
        QCOMPARE(kind.glyph, qucs_s::files::Kind::Layout);
        QVERIFY(kind.qucs);
        LayoutDoc* doc = open(path("sample.oas"));
        QVERIFY(doc != nullptr);
        // Save As: a copy.
        doc->setName(path("copy.oas"));
        QCOMPARE(doc->save(), 0);
        QCOMPARE(read(path("copy.oas")), read(path("sample.oas")));
        app->slotSimulate(doc);
        QVERIFY(!doc->getDocChanged());
    }

    // ---- Claude's tools

    void get_layoutReadsALayoutWithoutATab()
    {
        const QJsonObject r = call("get_layout", {{"path", "sample.oas"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject j = json(r);
        QCOMPARE(j.value("format").toString(), QString("OASIS"));
        QCOMPARE(j.value("database unit").toString(), QString("0.001 µm"));
        QCOMPARE(j.value("cells").toInt(), 3);
        QCOMPARE(j.value("shapes").toInt(), 14);
        QCOMPARE(j.value("placements").toInt(), 9);
        const QJsonObject top = j.value("top cells").toArray().at(0).toObject();
        QCOMPARE(top.value("cell").toString(), QString("TOP"));
        QCOMPARE(top.value("bounds").toArray(), QJsonArray({0, -1, 32, 20.2}));
        const QJsonArray tree = j.value("tree").toArray().at(0).toObject().value("cells").toArray();
        QCOMPARE(tree.size(), 2);
        QCOMPARE(tree.at(0).toObject().value("cell").toString(), QString("INV"));
        QCOMPARE(tree.at(0).toObject().value("placed").toInt(), 3);
        QCOMPARE(tree.at(1).toObject().value("placed").toInt(), 6);
        const QJsonArray layers = j.value("layers").toArray();
        QCOMPARE(layers.size(), 6);
        QCOMPARE(layers.at(0).toObject().value("name").toString(), QString("Metal1"));
        QCOMPARE(layers.at(3).toObject().value("shapes").toInt(), 10);
        QVERIFY(!j.contains("tab"));
        for (QucsDoc* d : app->allDocuments()) QVERIFY2(dynamic_cast<LayoutDoc*>(d) == nullptr, "no tab opened");
        // Kept while the file is as it was; read again once it is written again.
        writeSample(path("kept.gds"), false);
        QCOMPARE(json(call("get_layout", {{"path", "kept.gds"}})).value("cells").toInt(), 3);
        QTest::qWait(1100);   // (a file time of another second)
        writeSample(path("kept.gds"), false, 0, true);
        QCOMPARE(json(call("get_layout", {{"path", "kept.gds"}})).value("cells").toInt(), 4);
        // A file that is none; a cell that is none.
        QVERIFY(failed(call("get_layout", {{"path", "nowhere.gds"}})));
        const QJsonObject wrongCell = call("get_layout", {{"path", "sample.oas"}, {"cell", "NOPE"}});
        QVERIFY(failed(wrongCell));
        QVERIFY2(text(wrongCell).contains("top cells: TOP"), qPrintable(text(wrongCell)));
    }

    void find_shapesFindsByPlaceAndLayer()
    {
        // Under a point: the rectangle of the first INV, placed through TOP.
        QJsonObject j = json(call("find_shapes", {{"path", "sample.gds"}, {"at", QJsonArray{11, 0.2}}}));
        QCOMPARE(j.value("found").toInt(), 1);
        QJsonObject s = j.value("shapes").toArray().at(0).toObject();
        QCOMPARE(s.value("kind").toString(), QString("polygon"));
        QCOMPARE(s.value("layer").toString(), QString("1/0"));
        QCOMPARE(s.value("in").toString(), QString("INV"));
        QCOMPARE(s.value("via").toString(), QString("TOP > INV"));
        QCOMPARE(s.value("points").toArray(), QJsonArray({QJsonArray{10, 0}, QJsonArray{12, 0}, QJsonArray{12, 1}, QJsonArray{10, 1}}));
        QCOMPARE(s.value("properties").toObject().value("1").toString(), QString("net_A"));
        // In the triangle's bounds, outside it: nothing.
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"at", QJsonArray{0.5, 12.5}}}));
        QCOMPARE(j.value("found").toInt(), 0);
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"at", QJsonArray{2, 11}}}));
        QCOMPARE(j.value("found").toInt(), 1);
        // The path there too, with its width and spine.
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"at", QJsonArray{11, 0.5}}, {"layer", "2"}}));
        QCOMPARE(j.value("found").toInt(), 1);
        s = j.value("shapes").toArray().at(0).toObject();
        QCOMPARE(s.value("kind").toString(), QString("path"));
        QCOMPARE(s.value("width").toDouble(), 0.2);
        QCOMPARE(s.value("spine").toArray(), QJsonArray({QJsonArray{10, 0.5}, QJsonArray{12, 0.5}}));
        // By layer: the six VIAs, each a copy of the array; in a region, two.
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"layer", "3/0"}}));
        QCOMPARE(j.value("found").toInt(), 6);
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"layer", "3/0"}, {"region", QJsonArray{0.8, -0.5, 1.2, 2}}}));
        QCOMPARE(j.value("found").toInt(), 2);
        // A repetition's copies in OASIS, and a layer by its name.
        j = json(call("find_shapes", {{"path", "sample.oas"}, {"layer", "4/0"}, {"limit", 3}}));
        QCOMPARE(j.value("found").toInt(), 10);
        QCOMPARE(j.value("shapes").toArray().size(), 3);
        QVERIFY(j.value("note").toString().contains("3 listed of 10"));
        QCOMPARE(j.value("shapes").toArray().at(1).toObject().value("copy").toString(), QString("2 of 10"));
        j = json(call("find_shapes", {{"path", "sample.oas"}, {"layer", "metal1"}}));
        QCOMPARE(j.value("found").toInt(), 3);
        // The texts; a cell's own only (depth 0); another cell.
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"layer", "10/0"}}));
        QCOMPARE(j.value("found").toInt(), 3);
        QCOMPARE(j.value("shapes").toArray().at(0).toObject().value("text").toString(), QString("OUT"));
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"depth", 0}}));
        QCOMPARE(j.value("found").toInt(), 11);
        j = json(call("find_shapes", {{"path", "sample.gds"}, {"cell", "INV"}}));
        QCOMPARE(j.value("found").toInt(), 3);
        QCOMPARE(j.value("shapes").toArray().at(0).toObject().value("points").toArray().at(0).toArray(), QJsonArray({0, 0}));
        // A layer that is none: the layers said.
        const QJsonObject wrong = call("find_shapes", {{"path", "sample.gds"}, {"layer", "99/0"}});
        QVERIFY(failed(wrong));
        QVERIFY2(text(wrong).contains("No layer is 99/0") && text(wrong).contains("1/0"), qPrintable(text(wrong)));
        QVERIFY(failed(call("find_shapes", {{"path", "sample.gds"}, {"at", QJsonArray{1}}})));
    }

    void show_layoutShowsACellRegionAndLayers()
    {
        QJsonObject r = call("show_layout", {{"path", "sample.gds"}, {"cell", "INV"}, {"layers", QJsonArray{"1/0"}}, {"depth", 0}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* doc = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QCOMPARE(doc->layout()->cells.at(doc->shownCell()).name, QString("INV"));
        QJsonObject j = json(r);
        QCOMPARE(j.value("cell").toString(), QString("INV"));
        QCOMPARE(j.value("layers shown").toArray(), QJsonArray({"1/0"}));
        QCOMPARE(j.value("levels").toInt(), 0);
        QVERIFY(!doc->view()->styles().at(1).visible);
        QCOMPARE(j.value("drawn").toObject().value("shapes").toInt(), 1);
        // A region of TOP, every layer, all levels.
        r = call("show_layout", {{"cell", "TOP"}, {"region", QJsonArray{9, -1, 13, 2}}, {"layers", "all"}, {"depth", "all"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        j = json(r);
        QCOMPARE(j.value("layers shown").toString(), QString("all"));
        QCOMPARE(j.value("levels").toString(), QString("all"));
        const QJsonArray region = j.value("region").toArray();
        QVERIFY2(region.at(0).toDouble() <= 9 && region.at(2).toDouble() >= 13 && region.at(1).toDouble() <= -1 && region.at(3).toDouble() >= 2,
                 qPrintable(text(r)));
        // get_layout says what the tab shows; a screenshot shows it.
        j = json(call("get_layout", {{"path", "sample.gds"}}));
        QCOMPARE(j.value("tab").toObject().value("cell").toString(), QString("TOP"));
        const QJsonObject shot = call("screenshot", {{"path", path("sample.gds")}, {"area", "screen"}});
        QVERIFY2(!failed(shot), qPrintable(text(shot)));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"cell", "NOPE"}})));
        // get_state calls it a layout.
        QVERIFY2(text(call("get_state")).contains("\"layout\""), "get_state's kind");
        // The grid: hidden, shown - its step said.
        r = call("show_layout", {{"path", "sample.gds"}, {"grid", false}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(!doc->view()->gridShown());
        QVERIFY(!doc->gridButton()->isChecked());
        QVERIFY(!json(r).contains("grid"));
        r = call("show_layout", {{"path", "sample.gds"}, {"grid", true}});
        QVERIFY(doc->view()->gridShown());
        QVERIFY2(json(r).value("grid").toString().startsWith(QString("a point every %1 µm").arg(micrometres(*doc->layout(), doc->view()->gridStep()))),
                 qPrintable(text(r)));
        QCOMPARE(json(call("get_layout", {{"path", "sample.gds"}})).value("tab").toObject().value("grid").toString(),
                 micrometres(*doc->layout(), doc->view()->gridStep()) + " µm");
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"grid", "yes"}})));
    }
};

QTEST_MAIN(TestLayoutDoc)
#include "test_layout_doc.moc"
