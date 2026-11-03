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
#include <QWindow>
#include <QClipboard>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QTabWidget>
#include <QLabel>
#include <QToolButton>
#include <QAction>
#include <QMenu>
#include <QLineEdit>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>

#include <gdstk/gdstk.hpp>
#include <zlib.h>

#include <algorithm>
#include <cmath>

#ifdef QUCS_GPU_VIEW
#include "layoutviewgpu.h"
#endif
#include "multiphysicsview.h"
#include "gpurendering.h"
#include "config.h"
#include "extsimkernels/spicecompat.h"
#include "filebrowser.h"
#include "isolated_settings.h"
#include "layout.h"
#include "layoutdoc.h"
#include "markdowndoc.h"
#ifdef QUCS_HAVE_QTPDF
#include "pdfdoc.h"
#endif
#include "dialogs/qucssettingsdialog.h"
#include "links.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "settings.h"

using namespace qucs_s::layout;
namespace markup = qucs_s::markup;

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

/// A hostile hierarchy: a chain of \a depth cells, each placing the next
/// (the last a rectangle on 1/0); \a cycle a pair placing each other and
/// one placing itself; \a array a reference repeated 65535 by 65535.
void writeHierarchy(const QString& path, bool oasis, int depth, bool cycle, bool array)
{
    gdstk::Library lib = {};
    lib.init("HOSTILE", 1e-6, 1e-9);
    gdstk::Cell* leaf = newCell(lib, "LEAF");
    addRectangle(leaf, {0, 0}, {1, 1}, 1);
    gdstk::Cell* below = leaf;
    for (int k = 0; k < depth; ++k) {
        gdstk::Cell* c = newCell(lib, QStringLiteral("C%1").arg(k).toLatin1().constData());
        place(c, below, {0.5, 0.5});
        below = c;
    }
    if (cycle) {
        gdstk::Cell* a = newCell(lib, "PING");
        gdstk::Cell* b = newCell(lib, "PONG");
        place(a, b, {1, 0});
        place(b, a, {0, 1});
        addRectangle(a, {0, 0}, {1, 1}, 2);
        gdstk::Cell* self = newCell(lib, "SELF");
        place(self, self, {2, 2});
        place(below, a, {3, 3});
    }
    if (array) {
        gdstk::Reference* r = place(below, leaf, {0, 0});
        r->repetition.type = gdstk::RepetitionType::Rectangular;
        r->repetition.columns = 65535;
        r->repetition.rows = 65535;
        r->repetition.spacing = {1e-3, 1e-3};
    }
    // (Written top down, the leaf last: read so, a chain's bounds found by
    // recursion went the whole way down - bottom up, each cell's below was
    // done already.)
    std::reverse(lib.cell_array.items, lib.cell_array.items + lib.cell_array.count);
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray file = QFile::encodeName(path);
    if (oasis) lib.write_oas(file.constData(), 0, 6, OASIS_CONFIG_DETECT_ALL);
    else lib.write_gds(file.constData(), 0, nullptr);
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

    // A chain of cells 20,000 deep, each placing the one below half a
    // micrometre up and right: read on a thread of 512 kB (a pool's on
    // macOS, as the viewer's reading thread was), its bounds and depth
    // found, drawn and searched - no recursion as deep (its bounds, a level
    // a call, overflowed a thread's stack at 3,000 - the fuzz of 21
    // October, L1); drawn and searched 256 levels down at most.
    void aHierarchyThousandsDeepIsReadAndDrawn()
    {
        for (const bool oasis : {false, true}) {
            const QString file = path(oasis ? "deep/chain.oas" : "deep/chain.gds");
            writeHierarchy(file, oasis, 20000, false, false);
            QString error;
            std::shared_ptr<Layout> layout;
            QThread* reader = QThread::create([&layout, &error, &file] { layout = read(file, &error); });
            reader->setStackSize(512u << 10);
            reader->start();
            QVERIFY(reader->wait(120000));
            delete reader;
            QVERIFY2(layout, qPrintable(error));
            const int top = layout->cellIndex("C19999");
            QVERIFY(top >= 0);
            QCOMPARE(layout->cells.at(top).depth, 20000);
            QCOMPARE(layout->cells.at(top).bounds, QRectF(10000, 10000, 1, 1));
            const QVector<LayerStyle> styles = stylesFor(*layout, {});
            RenderOptions o;
            o.cell = top;
            o.depth = 1 << 20;
            o.device = QRectF(0, 0, 64, 64);
            o.toDevice = viewTransform(QPointF(10000.5, 10000.5), 10, o.device);
            o.styles = &styles;
            QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::white);
            QPainter painter(&image);
            (void)render(painter, *layout, o);
            painter.end();
            (void)countDrawn(*layout, o);
            // The leaf 20,000 levels down: past the 256 searched.
            auto count = [&layout](int cell) {
                Query q;
                q.cell = cell;
                q.depth = 1 << 20;
                int found = 0;
                search(*layout, q, [&found](const Found&) {
                    ++found;
                    return true;
                });
                return found;
            };
            QCOMPARE(count(top), 0);
            QCOMPARE(count(layout->cellIndex("C200")), 1);
        }
    }

    // OASIS written by hand: its header and START (version 1.0, 1000 units
    // a micrometre, the offsets in END), then \a records, then END.
    static QByteArray oasis(const QByteArray& records)
    {
        return QByteArray("%SEMI-OASIS\r\n") + QByteArray::fromHex("01" "03" "312e30" "00e807" "01") + records + QByteArray::fromHex("02");
    }

    // A text string's property named by a number (PROPNAME's, and no
    // PROPNAME given): read, the property's name "?". (Copied with the text
    // before the names were looked up: its name the number, 0, read as a
    // string - the fuzz of 21 October, L2.)
    void anOasisTextsPropertyNamedByNumberIsRead()
    {
        const QString file = path("hostile/textproperty.oas");
        QVERIFY(write(file, oasis(QByteArray::fromHex("05" "01" "41"         // TEXTSTRING "A", number 0
                                                      "1c" "06" "00"         // PROPERTY: its name PROPNAME 0, no values
                                                      "0e" "01" "54"         // CELL "T"
                                                      "13" "63" "00" "00" "00"))));   // TEXT TEXTSTRING 0, layer 0, type 0
        QString error;
        const auto layout = read(file, &error);
        QVERIFY2(layout, qPrintable(error));
        const int t = layout->cellIndex("T");
        QVERIFY(t >= 0);
        const Cell& cell = layout->cells.at(t);
        QCOMPARE(cell.labels.size(), 1);
        QCOMPARE(cell.labels.at(0).text, QString("A"));
        QVERIFY(cell.labels.at(0).properties >= 0);
        QCOMPARE(cell.properties.at(cell.labels.at(0).properties).first().first, QString("?"));
    }

    // A property of 2⁶² values in a file of 40 bytes: the file said damaged
    // at once, its cell before shown. (Its values were made, one by one,
    // until memory ran out - the fuzz of 21 October, L3.)
    void anOasisPropertysCountPastTheFileIsSaidDamaged()
    {
        const QString file = path("hostile/propertycount.oas");
        QVERIFY(write(file, oasis(QByteArray::fromHex("0e" "01" "54"                        // CELL "T"
                                                      "1c" "f4" "01" "6e"                   // PROPERTY "n", 15 values or more:
                                                      "808080808080808040"))));             // 2⁶² of them
        QElapsedTimer timer;
        timer.start();
        QString error;
        const auto layout = read(file, &error);
        QVERIFY2(layout, qPrintable(error));
        QVERIFY(timer.elapsed() < 10000);
        QVERIFY(layout->cellIndex("T") >= 0);
        QVERIFY(!layout->warnings.isEmpty());
        QVERIFY2(layout->warnings.first().contains("damaged"), qPrintable(layout->warnings.first()));
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

    // Polygons another tab draws over the layout (the RF 2.5D tab's
    // processed shapes and its cells): drawn over its picture, only where
    // they are in sight, a shape under two pixels as a point, and no more
    // of them than the budget.
    void overlaysAreDrawnOverTheLayoutWithinTheirBudget()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->fit();
        view->repaint();
        const quint64 shapesOfTheLayout = view->lastStats().polygons + view->lastStats().dots;
        QVERIFY(shapesOfTheLayout > 0);
        QVERIFY(view->overlays().isEmpty());
        QCOMPARE(view->overlayShapesDrawn(), 0);

        // Three in sight and one far away: the three are drawn, and the
        // layout under them is unchanged (it is cached, not drawn again).
        const QRectF seen = view->visibleRegion();
        LayoutView::Overlay here;
        here.shapes << QPolygonF(QRectF(seen.center(), QSizeF(seen.width() / 8, seen.height() / 8)))
                    << QPolygonF(seen.adjusted(seen.width() / 4, seen.height() / 4, -seen.width() / 4, -seen.height() / 4))
                    << QPolygonF(QRectF(seen.topLeft() + QPointF(1, 1), QSizeF(seen.width() / 9, seen.height() / 9)));
        here.line = QColor(Qt::red);
        here.name = QStringLiteral("processed");
        LayoutView::Overlay away;
        away.shapes << QPolygonF(QRectF(seen.right() + seen.width() * 10, seen.top(), 5, 5));
        away.line = QColor(Qt::blue);
        const int rendersBefore = view->renders();
        // (Any frame already asked for, delivered, so that what follows
        // waits on setOverlays() alone.)
        QCoreApplication::processEvents();
        view->setOverlays({here, away});
        QCOMPARE(view->overlays().size(), 2);
        // Setting them asks for a frame of its own: nothing else here
        // paints, and the three are drawn by the time the events are
        // through.
        QCoreApplication::processEvents();
        QCOMPARE(view->overlayShapesDrawn(), 3);
        QCOMPARE(view->overlayShapesLeftOut(), 0);
        view->repaint();
        QCOMPARE(view->overlayShapesDrawn(), 3);
        QCOMPARE(view->lastStats().polygons + view->lastStats().dots, shapesOfTheLayout);
        // And the layout under them is not drawn again for it: its picture
        // is cached, and the overlays go over that picture.
        view->repaint();
        QCOMPARE(view->renders(), rendersBefore);

        // Past the budget: that many drawn, the rest counted as left out
        // rather than silently dropped.
        LayoutView::Overlay many;
        const double step = seen.width() / (LayoutView::MostOverlayShapes + 60);
        for (int i = 0; i < LayoutView::MostOverlayShapes + 50; ++i)
            many.shapes << QPolygonF(QRectF(seen.left() + i * step, seen.center().y(), step / 2, step / 2));
        many.line = QColor(Qt::green);
        view->setOverlays({many});
        view->repaint();
        QCOMPARE(view->overlayShapesDrawn(), LayoutView::MostOverlayShapes);
        QCOMPARE(view->overlayShapesLeftOut(), 50);

        // Filled (a current density's cells): the fill is what is painted
        // there, the layout under it hidden. And a scale for what the fill
        // stands for, painted in the top right corner.
        LayoutView::Overlay filled;
        filled.shapes << QPolygonF(seen.adjusted(seen.width() / 4, seen.height() / 4, -seen.width() / 4, -seen.height() / 4));
        filled.fill = QColor(255, 0, 255);
        view->setOverlays({filled});
        LayoutView::Legend legend;
        legend.title = QStringLiteral("|J|, A/m");
        legend.ramp = {QColor(255, 0, 0), QColor(0, 0, 255)};
        legend.low = QStringLiteral("1");
        legend.high = QStringLiteral("1000");
        view->setLegend(legend);
        QCOMPARE(view->legend().title, legend.title);
        const QImage frame = view->grab().toImage();
        const double dpr = frame.devicePixelRatio();
        QCOMPARE(frame.pixelColor(int(view->width() / 2 * dpr), int(view->height() / 2 * dpr)), QColor(255, 0, 255));
        bool red = false, blue = false;
        for (int x = int((view->width() - 200) * dpr); x < frame.width(); ++x)
            for (int y = 0; y < int(90 * dpr) && y < frame.height(); ++y) {
                const QColor c = frame.pixelColor(x, y);
                red = red || (c.red() > 200 && c.green() < 60 && c.blue() < 60);
                blue = blue || (c.blue() > 200 && c.red() < 60 && c.green() < 60);
            }
        QVERIFY2(red && blue, "the legend's ramp is not in the top right corner");
        view->setLegend(LayoutView::Legend());
        QVERIFY(view->legend().isEmpty());

        // Taken away again: the layout alone, as before.
        view->setOverlays({});
        view->repaint();
        QCOMPARE(view->overlayShapesDrawn(), 0);
        QCOMPARE(view->lastStats().polygons + view->lastStats().dots, shapesOfTheLayout);
        QVERIFY(!view->picture().isNull());
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

    // The pen on a layout (markup.h). Taken up from the toolbar, a drag
    // draws on the cell shown in the pen's colour: in µm, so that it pans
    // and zooms with the layout, as broad as the pen was at the zoom it was
    // drawn at. Another cell shown, it is not there; the cell shown again,
    // it is. Ctrl+Z undoes it, pressed; the eraser takes it away and Ctrl+Z
    // gives it back; the middle button pans and draws nothing. Printed with
    // the cell; Escape puts the pen down; kept when the file is read again;
    // the pen's menu clears it.
    void aPenDrawsOnTheCellShown()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr && doc->penButton() != nullptr);
        LayoutView* view = doc->view();
        const int top = doc->layout()->cellIndex("TOP"), inv = doc->layout()->cellIndex("INV");
        doc->showCell(top);
        QApplication::processEvents();
        const QColor green(0, 170, 0);
        const auto near = [](const QColor& a, const QColor& b, int tolerance = 40) {
            return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
                   && std::abs(a.blue() - b.blue()) <= tolerance;
        };
        const auto colourAt = [view](QPointF pixel) {
            const QImage im = view->picture();
            return im.pixelColor((pixel * im.devicePixelRatio()).toPoint());
        };
        const auto greens = [view, &near, &green](const QImage& im) {
            int n = 0;
            for (int y = 0; y < im.height(); y += 2)
                for (int x = 0; x < im.width(); x += 2)
                    if (near(im.pixelColor(x, y), green, 30)) ++n;
            return n;
        };
        markup::Pen pen;
        pen.color = green;
        pen.width = 6;
        markup::setCurrentPen(pen);
        doc->penButton()->click();
        QCOMPARE(view->mode(), LayoutView::Mode::Draw);
        const QPointF from(view->width() * 0.3, view->height() * 0.4), to(view->width() * 0.6, view->height() * 0.5);
        drag(view, from, to);
        QCOMPARE(view->drawing().strokes().size(), 1);
        const markup::Stroke s = view->drawing().strokes().first();
        QCOMPARE(s.on, QStringLiteral("TOP"));
        QVERIFY(QLineF(s.points.first(), view->toLayout(from)).length() < 1e-9 && QLineF(s.points.last(), view->toLayout(to)).length() < 1e-9);
        QVERIFY(std::abs(s.width * view->scale() - 6) < 1e-9);
        // In its colour where it was drawn: 3 pixels either side and no more.
        const QPointF mid = (from + to) / 2, across = QPointF(-(to - from).y(), (to - from).x()) / QLineF(from, to).length();
        QVERIFY(near(colourAt(mid), green));
        QVERIFY(!near(colourAt(mid + 5 * across), green));
        // Zoomed twice about its middle: where it was on the layout, twice
        // as broad; panned, it went with the layout.
        const QPointF onLayout = view->toLayout(mid);
        view->zoomBy(2, mid);
        QVERIFY(QLineF(view->toPixel(onLayout), mid).length() < 1e-6);
        QVERIFY(near(colourAt(mid), green));
        QVERIFY(near(colourAt(mid + 4.5 * across), green));
        QVERIFY(!near(colourAt(mid + 8 * across), green));
        view->setView(view->center() + QPointF(40 / view->scale(), 0), view->scale());
        QVERIFY(QLineF(view->toPixel(onLayout), mid - QPointF(40, 0)).length() < 1e-6);
        QVERIFY(near(colourAt(view->toPixel(onLayout)), green));
        // Zoomed far out it is still drawn, a pixel broad.
        {
            const double scale = view->scale();
            view->zoomBy(0.01, view->toPixel(onLayout));
            const QImage far = view->picture();
            int greenish = 0;
            for (int y = 0; y < far.height(); ++y)
                for (int x = 0; x < far.width(); ++x) {
                    const QColor c = far.pixelColor(x, y);
                    if (c.green() > c.red() + 60 && c.green() > c.blue() + 60) ++greenish;
                }
            QVERIFY2(greenish > 0, "a stroke under a pixel broad drawn at one");
            view->setView(view->center(), scale);
        }
        // Another cell: not drawn there, where it would be (its place on TOP
        // framed); TOP again, it is.
        doc->showCell(inv);
        view->showRegion(s.bounds());
        QApplication::processEvents();
        QCOMPARE(greens(view->picture()), 0);
        doc->showCell(top);
        QApplication::processEvents();
        QVERIFY(greens(view->picture()) > 0);
        // Ctrl+Z, pressed: the window's Undo does not take it.
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(view->drawing().isEmpty());
        // Drawn again; the middle button pans and draws nothing.
        drag(view, from, to);
        QCOMPARE(view->drawing().strokes().size(), 1);
        const QPointF centre = view->center();
        const auto send = [view](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons held) {
            QMouseEvent e(type, at, view->mapToGlobal(at), button, held, Qt::NoModifier);
            QCoreApplication::sendEvent(view, &e);
        };
        send(QEvent::MouseButtonPress, from, Qt::MiddleButton, Qt::MiddleButton);
        for (int i = 1; i <= 5; ++i) send(QEvent::MouseMove, from + QPointF(12 * i, 0), Qt::NoButton, Qt::MiddleButton);
        send(QEvent::MouseButtonRelease, from + QPointF(60, 0), Qt::MiddleButton, Qt::NoButton);
        QVERIFY(QLineF(view->center(), centre).length() > 0);
        QCOMPARE(view->drawing().strokes().size(), 1);
        // The eraser across it, and the erasing undone.
        pen.tool = markup::Tool::Eraser;
        markup::setCurrentPen(pen);
        const markup::Stroke t = view->drawing().strokes().first();
        const QPointF on = view->toPixel((t.points.first() + t.points.last()) / 2);
        drag(view, on - 12 * across, on + 12 * across);
        QVERIFY(view->drawing().isEmpty());
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(view->drawing().strokes().size(), 1);
        // Printed with the cell.
        {
            QPrinter printer;
            QImage page(800, 600, QImage::Format_ARGB32_Premultiplied);
            page.fill(Qt::white);
            QPainter painter(&page);
            doc->print(&printer, &painter, false, true);
            painter.end();
            QVERIFY(greens(page) > 0);
        }
        // Escape puts the pen down.
        pen.tool = markup::Tool::Pen;
        markup::setCurrentPen(pen);
        QTest::keyClick(view, Qt::Key_Escape);
        QCOMPARE(view->mode(), LayoutView::Mode::Select);
        QVERIFY(!doc->penButton()->isChecked());
        // Kept when the file is read again.
        QVERIFY(doc->reload());
        QVERIFY(doc->waitForLoaded());
        QCOMPARE(view->drawing().strokes().size(), 1);
        QCOMPARE(view->drawing().strokes().first().on, QStringLiteral("TOP"));
        // The pen's menu clears it.
        for (QAction* a : doc->penButton()->penMenu()->actions())
            if (a->text() == QStringLiteral("Clear All the Drawings")) a->trigger();
        QVERIFY(view->drawing().isEmpty());
        markup::setCurrentPen(markup::Pen());
    }

    // What the pen drew on a layout, read as one reads it
    // (LayoutView::readDrawing()), in µm of the cell drawn on: a ring round
    // the first INV holds its two shapes by layer, its text and the INV
    // placed there; one round the VIA array its six squares and six VIAs;
    // an arrow drawn at one go from the triangle points at the turned INV
    // it stops short of; a highlighter over the row of squares lies on the
    // four it passes; a dot on the mirrored INV's body is on it, in that
    // INV; a line with its head drawn after it points at the first INV's
    // body from above. A layer hidden is not read, nor the texts hidden,
    // nor what is below the depth drawn; each stroke's points when asked;
    // a reading in a sentence.
    void whatThePenDrewOnALayoutIsRead()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        doc->showCell(doc->layout()->cellIndex("TOP"));
        const auto stroke = [](const QPolygonF& points, double width = 0.1, bool highlighter = false) {
            markup::Stroke s;
            s.on = QStringLiteral("TOP");
            s.points = points;
            s.width = width;
            s.highlighter = highlighter;
            s.color = QColor(0x1c, 0x71, 0xd8);
            return s;
        };
        QPolygonF circle, shaft;
        for (int k = 0; k < 23; ++k) circle << QPointF(11, 0.5) + 1.8 * QPointF(std::cos(2 * M_PI * k / 24), std::sin(2 * M_PI * k / 24));
        for (int k = 0; k <= 10; ++k) shaft << QPointF(2, 11) + (QPointF(18.6, 1) - QPointF(2, 11)) * (k / 10.0);
        view->addStroke(stroke(circle));
        view->addStroke(stroke({QPointF(-0.5, -0.5), QPointF(3, -0.5), QPointF(3, 2), QPointF(-0.5, 2), QPointF(-0.5, -0.3)}));
        view->addStroke(stroke(shaft << QPointF(17.7, 1.9)));
        view->addStroke(stroke({QPointF(2.5, 20.1), QPointF(6.7, 20.1)}, 0.3, true));
        view->addStroke(stroke({QPointF(31, -0.25)}));
        view->addStroke(stroke({QPointF(11, 8), QPointF(11, 1.55)}));
        view->addStroke(stroke({QPointF(10.6, 2.15), QPointF(11, 1.55), QPointF(11.4, 2.15)}));
        QJsonArray read = view->readDrawing(true);
        QCOMPARE(read.size(), 7);
        const auto r = [&read](int stroke) { return read.at(stroke - 1).toObject(); };
        const auto on = [](const QJsonObject& shapes, const QJsonObject& cells = {}) {
            QJsonObject o{{"shapes", shapes}};
            if (!cells.isEmpty()) o.insert("cells", cells);
            return o;
        };
        const QJsonObject inv{{"INV", 1}};
        QCOMPARE(r(1).value("drawn as").toString(), QStringLiteral("ring"));
        QCOMPARE(r(1).value("cell").toString(), QStringLiteral("TOP"));
        QCOMPARE(r(1).value("around").toObject(), (QJsonObject{{"shapes", QJsonObject{{"1/0", 1}, {"2/0", 1}}}, {"texts", QJsonArray{"OUT"}}, {"cells", inv}}));
        QCOMPARE(r(2).value("around").toObject(), on({{"3/0", 6}}, {{"VIA", 6}}));
        QCOMPARE(r(3).value("drawn as").toString(), QStringLiteral("arrow"));
        QCOMPARE(r(3).value("from").toObject(), on({{"5/0", 1}}));
        QCOMPARE(r(3).value("points at").toObject(), on({{"1/0", 1}}, inv));
        QCOMPARE(r(4).value("tool").toString(), QStringLiteral("highlighter"));
        QCOMPARE(r(4).value("drawn as").toString(), QStringLiteral("line"));
        QCOMPARE(r(4).value("highlights").toObject(), on({{"4/0", 4}}));
        QCOMPARE(r(5).value("drawn as").toString(), QStringLiteral("dot"));
        QCOMPARE(r(5).value("on").toObject(), on({{"1/0", 1}}, inv));
        QCOMPARE(r(6).value("drawn as").toString(), QStringLiteral("arrow"));
        QCOMPARE(r(6).value("points at").toObject(), on({{"1/0", 1}}, inv));
        QVERIFY(!r(6).contains("from"));
        QCOMPARE(r(7).value("drawn as").toString(), QStringLiteral("arrowhead"));
        QCOMPARE(r(7).value("of stroke").toInt(), 6);
        QCOMPARE(r(6).value("points").toArray(), (QJsonArray{QJsonArray{11, 8}, QJsonArray{11, 1.55}}));
        QVERIFY(!view->readDrawing(false).at(0).toObject().contains("points"));
        // In a sentence.
        const QString first = markup::sentencesOf(read, false).section(QLatin1Char('\n'), 0, 0);
        QVERIFY2(first.startsWith(QStringLiteral("cell TOP, stroke 1: a blue pen ring around 1 shape(s) on 1/0, 1 shape(s) on 2/0, the text OUT, "
                                                 "1 placement(s) of INV (its box "))
                     && first.endsWith(QStringLiteral(" µm)")),
                 qPrintable(first));
        // 1/0 hidden: not read - the dot is in the INV, on nothing of it shown.
        int metal = -1;
        for (int i = 0; i < doc->layout()->layers.size(); ++i)
            if (doc->layout()->layers.at(i).key.text() == QStringLiteral("1/0")) metal = i;
        QVERIFY(metal >= 0);
        doc->setLayerVisible(metal, false);
        read = view->readDrawing(false);
        QCOMPARE(r(1).value("around").toObject(), (QJsonObject{{"shapes", QJsonObject{{"2/0", 1}}}, {"texts", QJsonArray{"OUT"}}, {"cells", inv}}));
        QCOMPARE(r(5).value("on").toObject(), (QJsonObject{{"cells", inv}}));
        doc->setLayerVisible(metal, true);
        // The texts hidden; then nothing below the cell drawn.
        view->setLabelsShown(false);
        QVERIFY(!view->readDrawing(false).at(0).toObject().value("around").toObject().contains("texts"));
        view->setLabelsShown(true);
        view->setDepth(0);
        read = view->readDrawing(false);
        QCOMPARE(r(1).value("around").toObject(), (QJsonObject{{"cells", inv}}));
        QCOMPARE(r(2).value("around").toObject(), (QJsonObject{{"cells", QJsonObject{{"VIA", 6}}}}));
        view->setDepth(1 << 20);
    }

    // What the user drew with the pen on a layout, as show_layout has it:
    // stroke by stroke, its cell, how it was drawn, its colour and its box
    // in µm; read_drawings with its points too. show_layout takes the pen
    // up and puts it down, and clears the drawings; a 'pen' or 'drawings'
    // of another kind is refused.
    void claudeSeesWhatThePenDrewOnALayout()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        doc->showCell(doc->layout()->cellIndex("TOP"));
        QJsonObject o = json(call("show_layout", {{"path", "sample.gds"}}));
        QVERIFY(!o.contains("drawings") && !o.contains("pen"));
        markup::Stroke line;
        line.on = QStringLiteral("TOP");
        line.points = {QPointF(2, 3), QPointF(12, 8)};
        line.color = QColor(0, 160, 0);
        line.width = 0.5;
        doc->view()->addStroke(line);
        o = json(call("show_layout", {{"path", "sample.gds"}, {"pen", true}}));
        QCOMPARE(doc->view()->mode(), LayoutView::Mode::Draw);
        QVERIFY(doc->penButton()->isChecked() && o.value("pen").toString().contains(QStringLiteral("taken up")));
        const QJsonArray drawn = o.value("drawings").toArray();
        QCOMPARE(drawn.size(), 1);
        const QJsonObject first = drawn.at(0).toObject();
        QCOMPARE(first.value("cell").toString(), QStringLiteral("TOP"));
        QCOMPARE(first.value("stroke").toInt(), 1);
        QCOMPARE(first.value("drawn as").toString(), QStringLiteral("line"));
        QCOMPARE(first.value("box").toArray(), (QJsonArray{1.75, 2.75, 12.25, 8.25}));
        QCOMPARE(first.value("colour").toString(), QStringLiteral("#00a000"));
        QVERIFY(!first.contains("points"));
        const QJsonObject one = json(call("read_drawings", {})).value("documents").toArray().at(0).toObject();
        QCOMPARE(one.value("document").toString(), QStringLiteral("sample.gds"));
        QVERIFY(one.value("measures").toString().contains(QStringLiteral("µm")));
        QCOMPARE(one.value("strokes").toArray().at(0).toObject().value("points").toArray(), (QJsonArray{QJsonArray{2, 3}, QJsonArray{12, 8}}));
        // What a prompt from the Claude Code panel tells of it.
        const auto [revision, told] = app->drawingsOf(doc->getDocName());
        QVERIFY(revision > 0 && revision == doc->view()->drawing().revision());
        QVERIFY2(told.startsWith(QStringLiteral("cell TOP, stroke 1: a #00a000 pen line (its box 1.75, 2.75 to 12.25, 8.25 µm)")), qPrintable(told));
        QVERIFY(app->drawingsOf(doc->getDocName(), revision).second.isEmpty());   // (unchanged: not read)
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"pen", 1}})));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"drawings", "undo"}})));
        QCOMPARE(doc->view()->drawing().strokes().size(), 1);
        o = json(call("show_layout", {{"path", "sample.gds"}, {"pen", false}, {"drawings", "clear"}}));
        QVERIFY(doc->view()->drawing().isEmpty() && doc->view()->mode() == LayoutView::Mode::Select);
        QVERIFY(!o.contains("drawings") && !o.contains("pen") && o.value("done").toString().contains(QStringLiteral("taken away")));
    }

    // The pen drawn with as the user's mouse draws, through Claude's raw
    // input (the check of G8, G9 and the pens of 2 November: "send_input
    // has no target for a layout or slide view ... Wanted: a layout/slide
    // target for send_input"): 'target' layout, its points µm on the cell
    // shown - 'drag_through' a ring round the INV's output, out of sight
    // and brought into view, drawn by the mouse's events and read in the
    // answer as read_drawings reads it; 'pixels' the picture's; its keys to
    // the view, Ctrl+Z undoing and Escape putting the pen down; a drag too
    // long for the zoom, a layout not open and points that are none
    // refused, nothing drawn.
    void theRawInputDrawsOnALayout()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        doc->showCell(doc->layout()->cellIndex("TOP"));
        LayoutView* view = doc->view();
        doc->view()->clearDrawing(true);
        QVERIFY(!failed(call("show_layout", {{"path", "sample.gds"}, {"pen", true}, {"region", QJsonArray{40, 40, 52, 51}}})));
        const auto answer = [](const QJsonObject& r) {
            return QJsonDocument::fromJson(text(r).section(QLatin1Char('\n'), 0, 0).toUtf8()).object();
        };
        QJsonArray ring;
        for (int k = 1; k <= 24; ++k) ring.append(QJsonArray{11 + 1.8 * std::cos(2 * M_PI * k / 24), 0.5 + 1.8 * std::sin(2 * M_PI * k / 24)});
        QJsonObject r = call("send_input", {{"target", "layout"}, {"click", QJsonArray{12.8, 0.5}}, {"drag_through", ring}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY2(view->visibleRegion().contains(QPointF(11, 0.5)), "brought into view");
        QJsonObject o = answer(r);
        QCOMPARE(o.value("on").toString(), QStringLiteral("the cell TOP of sample.gds"));
        QVERIFY2(text(r).contains(QStringLiteral("µm, y up")), qPrintable(text(r)));
        QCOMPARE(o.value("drawn").toArray().size(), 1);
        const QJsonObject drawn = o.value("drawn").toArray().at(0).toObject();
        QCOMPARE(drawn.value("drawn as").toString(), QStringLiteral("ring"));
        QCOMPARE(drawn.value("around").toObject(),
                 (QJsonObject{{"shapes", QJsonObject{{"1/0", 1}, {"2/0", 1}}}, {"texts", QJsonArray{"OUT"}}, {"cells", QJsonObject{{"INV", 1}}}}));
        QCOMPARE(json(call("read_drawings", {{"points", false}})).value("documents").toArray().at(0).toObject().value("strokes").toArray().at(0),
                 QJsonValue(drawn));
        // Where the mouse went: the stroke's points on the ring asked, to
        // a pixel's worth of µm.
        const markup::Stroke stroke = view->drawing().on(QStringLiteral("TOP")).first();
        QVERIFY2(QLineF(stroke.points.first(), QPointF(12.8, 0.5)).length() <= 1 / view->scale(), "pressed where asked");
        for (const QPointF& p : stroke.points)
            QVERIFY2(std::abs(QLineF(p, QPointF(11, 0.5)).length() - 1.8) < 0.05, qPrintable(QStringLiteral("%1, %2").arg(p.x()).arg(p.y())));
        // An L dragged through its bend (the RF 2.5D assessment of 2
        // November: "an L read as a ring"): a corner, at what its bend is on.
        QJsonArray ell;
        ell.append(QJsonArray{31, -0.25});
        ell.append(QJsonArray{32.5, -0.25});
        ell.append(QJsonArray{34, -0.25});
        r = call("send_input", {{"target", "layout"}, {"click", QJsonArray{31, 2.75}}, {"drag_through", ell}});
        const QJsonObject bent = answer(r).value("drawn").toArray().at(0).toObject();
        QCOMPARE(bent.value("drawn as").toString(), QStringLiteral("corner"));
        QCOMPARE(bent.value("at").toObject(), (QJsonObject{{"shapes", QJsonObject{{"1/0", 1}}}, {"cells", QJsonObject{{"INV", 1}}}}));
        const QJsonArray point = bent.value("point").toArray();
        QVERIFY2(point.size() == 2 && std::abs(point.at(0).toDouble() - 31) < 0.1 && std::abs(point.at(1).toDouble() + 0.25) < 0.1,
                 qPrintable(QString::fromUtf8(QJsonDocument(bent).toJson(QJsonDocument::Compact))));
        // In the picture's pixels: a line (the view on it again - the L, out
        // of sight, was brought into view).
        view->showRegion(QRectF(QPointF(5, -5), QPointF(17, 6)));
        const QPointF from = view->toPixel(QPointF(8, -3)), to = view->toPixel(QPointF(14, -3));
        r = call("send_input", {{"target", "layout"}, {"click", QJsonArray{from.x(), from.y()}}, {"drag_to", QJsonArray{to.x(), to.y()}},
                                {"pixels", true}});
        QCOMPARE(answer(r).value("drawn").toArray().at(0).toObject().value("drawn as").toString(), QStringLiteral("line"));
        QCOMPARE(view->drawing().count(QStringLiteral("TOP")), 3);
        // Its keys: Ctrl+Z takes the line away; Escape puts the pen down.
        r = call("send_input", {{"target", "layout"}, {"keys", "Ctrl+Z"}});
        QVERIFY2(answer(r).value("taken away").toString().startsWith(QStringLiteral("1 of the pen's")), qPrintable(text(r)));
        QCOMPARE(view->drawing().count(QStringLiteral("TOP")), 2);
        QVERIFY(!failed(call("send_input", {{"target", "layout"}, {"keys", "Escape"}})));
        QCOMPARE(view->mode(), LayoutView::Mode::Select);
        // Refused, nothing drawn: a drag longer than the view shows at this
        // zoom, a layout not open, points that are none.
        r = call("send_input", {{"target", "layout"}, {"click", QJsonArray{-5000, 0}}, {"drag_to", QJsonArray{5000, 0}}});
        QVERIFY2(failed(r) && text(r).contains(QStringLiteral("does not fit")), qPrintable(text(r)));
        QVERIFY(failed(call("send_input", {{"target", "layout"}, {"path", "elsewhere.gds"}, {"click", QJsonArray{1, 1}}})));
        QJsonArray none;   // (a list of one point, which is no point: appended, as a braced one is a copy on some compilers)
        none.append(QJsonArray{2});
        QCOMPARE(none.size(), 1);
        QVERIFY(failed(call("send_input", {{"target", "layout"}, {"click", QJsonArray{1, 1}}, {"drag_through", none}})));
        QCOMPARE(view->drawing().strokes().size(), 2);
        view->clearDrawing(true);
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

    // For a tab that places things on the layout - the RF 2.5D tab's ports,
    // the user's request of 1 November: in PickPoint a click is picked() at
    // its point and selects nothing, and a drag still pans; in PickBox a
    // drag is a box picked, a click a point; Escape is Select again. And an
    // overlay's label is written over the layout, its arrow and its text in
    // the overlay's colour, by the point it names.
    void aPickingModeSaysWhatIsPickedAndSelectsNothing()
    {
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        const QRectF region(QPointF(9, -1), QPointF(13, 2));
        view->showRegion(region);
        view->repaint();
        QSignalSpy points(view, &LayoutView::picked), boxes(view, &LayoutView::boxPicked);
        view->setMode(LayoutView::Mode::PickPoint);
        QCOMPARE(view->cursor().shape(), Qt::CrossCursor);
        click(view, view->toPixel(QPointF(11, 0.2)));
        QCOMPARE(points.size(), 1);
        const QPointF at = points.first().first().toPointF();
        QVERIFY2(std::hypot(at.x() - 11, at.y() - 0.2) < 0.05, qPrintable(QStringLiteral("%1, %2").arg(at.x()).arg(at.y())));
        QVERIFY(!view->selection().has_value());
        const QPointF centre = view->center();
        drag(view, view->toPixel(QPointF(10, 0)), view->toPixel(QPointF(11, 0)));
        QCOMPARE(points.size(), 1);
        QVERIFY(view->center() != centre);
        QCOMPARE(boxes.size(), 0);

        view->showRegion(region);
        view->setMode(LayoutView::Mode::PickBox);
        drag(view, view->toPixel(QPointF(10, 0)), view->toPixel(QPointF(12, 1)));
        QCOMPARE(boxes.size(), 1);
        const QRectF box = boxes.first().first().toRectF();
        QVERIFY2(std::abs(box.left() - 10) < 0.05 && std::abs(box.right() - 12) < 0.05 && std::abs(box.top()) < 0.05
                     && std::abs(box.bottom() - 1) < 0.05,
                 qPrintable(QStringLiteral("%1, %2 to %3, %4").arg(box.left()).arg(box.top()).arg(box.right()).arg(box.bottom())));
        QVERIFY(!view->selection().has_value());
        click(view, view->toPixel(QPointF(12, 0.5)));
        QCOMPARE(points.size(), 2);
        QCOMPARE(boxes.size(), 1);
        // Whichever way it is dragged, the box between the two points, pixel
        // for pixel: Qt 6's QRect::normalized() moves a swapped corner in by
        // a pixel, and a box dragged up or to the left came out two short.
        {
            const QPoint a = view->toPixel(QPointF(10.2, 0.3)).toPoint(), b = view->toPixel(QPointF(11.6, 0.9)).toPoint();
            const QList<std::pair<QPoint, QPoint>> ways{
                {a, b}, {b, a}, {QPoint(a.x(), b.y()), QPoint(b.x(), a.y())}, {QPoint(b.x(), a.y()), QPoint(a.x(), b.y())}};
            for (const auto& [from, to] : ways) {
                drag(view, from, to);
                const QRectF picked = boxes.last().first().toRectF();
                const QPointF low = view->toPixel(picked.bottomLeft()), high = view->toPixel(picked.topRight());
                QVERIFY2(std::abs(std::abs(high.x() - low.x()) - std::abs(b.x() - a.x())) < 0.01
                             && std::abs(std::abs(high.y() - low.y()) - std::abs(b.y() - a.y())) < 0.01,
                         qPrintable(QStringLiteral("from %1, %2 to %3, %4: %5 by %6 pixels, not %7 by %8")
                                        .arg(from.x()).arg(from.y()).arg(to.x()).arg(to.y())
                                        .arg(std::abs(high.x() - low.x())).arg(std::abs(high.y() - low.y()))
                                        .arg(std::abs(b.x() - a.x())).arg(std::abs(b.y() - a.y()))));
            }
            QCOMPARE(boxes.size(), 5);
        }
        QTest::keyClick(view, Qt::Key_Escape);
        QCOMPARE(view->mode(), LayoutView::Mode::Select);
        QCOMPARE(view->cursor().shape(), Qt::ArrowCursor);

        LayoutView::Overlay named;
        named.line = QColor(255, 0, 255);
        named.labels << LayoutView::Overlay::Label{QPointF(11, 0.5), QStringLiteral("P1"), QPointF(1, 0)};
        view->setOverlays({named});
        const QImage frame = view->grab().toImage();
        const double dpr = frame.devicePixelRatio();
        const QPointF anchor = view->toPixel(QPointF(11, 0.5));
        int magenta = 0;
        for (int x = int(anchor.x() * dpr); x < int((anchor.x() + 70) * dpr) && x < frame.width(); ++x)
            for (int y = int((anchor.y() - 12) * dpr); y < int((anchor.y() + 12) * dpr) && y < frame.height(); ++y) {
                const QColor c = frame.pixelColor(x, y);
                magenta += c.red() > 200 && c.blue() > 200 && c.green() < 90 ? 1 : 0;
            }
        QVERIFY2(magenta > 40, qPrintable(QStringLiteral("%1 pixels of the label's colour by its point").arg(magenta)));
        view->setOverlays({});
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
        // The application's again: its Base, in the 8 bits it is drawn in (a
        // dark platform's Base has finer ones: the same #171717, unequal
        // as QColors).
        choose(background, "Like the Application");
        QCOMPARE(view->theme(), LayoutView::Theme::Application);
        QCOMPARE(corner().rgba(), view->palette().color(QPalette::Base).rgba());
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

    // A layer's swatch at any device pixel ratio: its frame the outermost
    // pixel all round, its corners too; inside, the canvas and the fill.
    // (Stroked through the pixels' centres, at a Retina screen's 2 its left
    // corners' outer device pixels were the background: the layer list's
    // corner read white on the display.)
    void aSwatchIsWholeAtAnyRatio()
    {
        LayerStyle style = paletteStyle(0);
        style.frame = QColor("#ff3474");
        style.pattern = Qt::NoBrush;
        const QColor canvas(Qt::white);
        for (const qreal ratio : {1.0, 1.5, 2.0}) {
            const QImage swatch = LayoutDoc::swatchPixmap(style, canvas, ratio).toImage();
            QCOMPARE(swatch.size(), (QSizeF(16, 12) * ratio).toSize());
            // (The frame a pixel deep: its device pixels whole, at least - a
            // ratio of 1.5's pixel 2 of them left and above, 1 right and
            // below, as Qt rounds the inside's edges.)
            const int deep = int(std::floor(ratio)), within = int(std::ceil(ratio));
            QString wrong;
            for (int y = 0; y < swatch.height(); ++y)
                for (int x = 0; x < swatch.width(); ++x) {
                    const bool onFrame = x < deep || y < deep || x >= swatch.width() - deep || y >= swatch.height() - deep;
                    const bool inside = x >= within && y >= within && x < swatch.width() - within && y < swatch.height() - within;
                    const QColor c = swatch.pixelColor(x, y);
                    if ((onFrame && c != style.frame) || (inside && c != canvas)) wrong += QStringLiteral(" %1,%2 %3").arg(x).arg(y).arg(c.name());
                }
            QVERIFY2(wrong.isEmpty(), qPrintable(QStringLiteral("at %1:%2").arg(ratio).arg(wrong.left(300))));
        }
        // Filled: its pattern inside the frame, the frame whole.
        style.fill = QColor("#3474ff");
        style.pattern = Qt::SolidPattern;
        const QImage filled = LayoutDoc::swatchPixmap(style, canvas, 2).toImage();
        QCOMPARE(filled.pixelColor(0, 0), style.frame);
        QCOMPARE(filled.pixelColor(0, filled.height() - 1), style.frame);
        QCOMPARE(filled.pixelColor(filled.width() / 2, filled.height() / 2), style.fill);
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
        // A view's pixel in its picture: the device pixel at its centre, at
        // the picture's ratio - a Retina screen's 2, the grid's dots and
        // lines drawn through pixels' centres (read as the view's pixels,
        // they were missed).
        const auto device = [](const QImage& picture, QPoint p) {
            const qreal r = picture.devicePixelRatio();
            return QPoint(int(std::floor((p.x() + 0.5) * r)), int(std::floor((p.y() + 0.5) * r)));
        };
        const auto pixel = [&device](const QImage& picture, QPoint p) { return picture.pixelColor(device(picture, p)); };
        // The darkest of the pixels about one.
        const auto darkest = [&device](const QImage& picture, QPoint p) {
            const QPoint d = device(picture, p);
            int least = 255;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) least = std::min(least, picture.pixelColor(d + QPoint(dx, dy)).lightness());
            return least;
        };
        QImage picture = view->picture();
        const QPoint strong = at(-1000, -1000), weak = at(-998, -1000), between = at(-999, -999);
        QVERIFY2(darkest(picture, strong) < darkest(picture, weak), qPrintable(QString("%1 %2").arg(darkest(picture, strong)).arg(darkest(picture, weak))));
        QVERIFY2(darkest(picture, weak) < 230, qPrintable(QString::number(darkest(picture, weak))));
        QCOMPARE(darkest(picture, between), 255);
        // The scale: its bar in the lower left, in ink.
        const auto scaleDrawn = [view, &device](const QImage& picture) {
            const QPoint from = device(picture, QPoint(14, view->height() - 14)), to = device(picture, QPoint(30, view->height() - 6));
            for (int y = from.y(); y < to.y(); ++y)
                for (int x = from.x(); x < to.x(); ++x)
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
        // Crosses: their arms, longer on a power of ten - a weak one's
        // reaching 2 pixels, not 3, a strong one's 3 at least. (Read a pixel
        // about: at a ratio of 2 Qt's aliased lines fall a device pixel left
        // of a whole coordinate, not above one - an arm ends there.)
        action["Crosses"]->trigger();
        QCOMPARE(view->gridStyle(), LayoutView::GridStyle::Crosses);
        picture = view->picture();
        QVERIFY(darkest(picture, weak + QPoint(2, 0)) < 255);
        QCOMPARE(darkest(picture, weak + QPoint(4, 0)), 255);
        QVERIFY(darkest(picture, strong + QPoint(4, 0)) < 255);
        QCOMPARE(darkest(picture, strong + QPoint(3, 3)), 255);
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
        QCOMPARE(pixel(picture, at(11, 0.8)), view->shownStyle(solid[0]).fill);
        QCOMPARE(pixel(picture, at(11.05, 0.85)), view->shownStyle(solid[0]).fill);
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
        // (The CPU's drawing: its dots for shapes under a pixel, what is in
        // sight alone - the GPU's below.)
        // (Drawn by grab(): a window that has had a GPU widget in it is
        // composed by the GPU from then on, a repaint() there only asked for.)
        view->setGpu(false);
        view->fit();   // (drawn again, now)
        const int before = view->renders();
        t.restart();
        (void)view->grab();
        const qint64 fullMs = t.elapsed();
        QCOMPARE(view->renders(), before + 1);
        const RenderStats wholeOnCpu = view->lastStats();
        QVERIFY(!view->lastStats().incomplete);
        // Each square under a pixel: a dot each, once a pixel.
        QCOMPARE(view->lastStats().polygons, quint64(0));
        QVERIFY(view->lastStats().dots > 1000);
        QVERIFY(view->lastStats().dots < quint64(view->width()) * quint64(view->height()) * 4);
        view->showRegion(QRectF(QPointF(0, 0), QPointF(40, 40)));
        t.restart();
        (void)view->grab();
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
        // The GPU's, where there is one: its buffers made once (the first
        // frame), then a pan or a zoom a frame of its own - the CPU drawing
        // nothing; the whole drawn, the corner in sight.
#ifdef QUCS_GPU_VIEW
        if (qucs_s::gpu::hindrance().isEmpty()) {
            view->fit();
            view->setGpu(true);
            QVERIFY(view->gpu());
            // (The window composed by the RHI from now on - its canvas's
            // frames on the screen, not only in its grabs: the window's
            // surface the GPU's, no longer a raster one.)
            QVERIFY(view->window()->windowHandle() != nullptr);
            QVERIFY(view->window()->windowHandle()->surfaceType() != QSurface::RasterSurface);
            const int renders = view->renders();
            t.restart();
            (void)view->canvas()->grabFramebuffer();
            const qint64 buildMs = t.elapsed();
            QCOMPARE(view->canvas()->builds(), 1);
            QCOMPARE(view->canvas()->flattened().shapes, quint64(1000000));   // (all in its buffers)
            // (its counts the CPU's picture's of the same view)
            QCOMPARE(view->lastStats().polygons, wholeOnCpu.polygons);
            QCOMPARE(view->lastStats().dots, wholeOnCpu.dots);
            qint64 worst = 0;
            for (int k = 0; k < 10; ++k) {
                view->setView(view->center() + QPointF(37, 23), view->scale() * (k % 2 ? 1.3 : 1 / 1.3));
                t.restart();
                (void)view->canvas()->grabFramebuffer();
                worst = std::max(worst, t.elapsed());
            }
            view->showRegion(QRectF(QPointF(0, 0), QPointF(40, 40)));
            t.restart();
            (void)view->canvas()->grabFramebuffer();
            const qint64 gpuCornerMs = t.elapsed();
            QCOMPARE(view->canvas()->builds(), 1);
            QCOMPARE(view->renders(), renders);
            qInfo("A million shapes on the GPU: its buffers made in %lld ms, a pan or a zoom %lld ms at most, the corner %lld ms "
                  "(the CPU: the whole %lld ms, the corner %lld ms)",
                  buildMs, worst, gpuCornerMs, fullMs, cornerMs);
            QVERIFY2(worst < 200 * slower, qPrintable(QString::number(worst)));
        }
#endif
    }

    // The GPU's picture (layoutviewgpu.h) the CPU's, pixel for pixel but at
    // the edges of shapes (its triangles' rasterization, not QPainter's):
    // the sample, and shapes the GPU must triangulate - an L, a square with
    // a hole by a slit (a GDSII hole), a ring, a bow tie (odd-even) - in
    // each pattern. A pan and a zoom drawn from its buffers, not made
    // again, the CPU drawing nothing; a layer hidden, its buffers kept, not
    // drawn. Past its budget the CPU's picture. Where the RHI has no backend
    // (CI's offscreen display) it is not there to compare. (The GPU chosen
    // for the layout viewer: the CPU's unless it is.)
    void theGpuDrawsAsTheCpu()
    {
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Layout, true);
        const struct Back {
            ~Back() { QucsSettingsFile().remove(qucs_s::gpu::settingKey(qucs_s::gpu::Viewer::Layout)); }
        } back;
        const QString shapes = path("shapes.gds");
        {
            gdstk::Library lib = {};
            lib.init("SHAPES", 1e-6, 1e-9);
            gdstk::Cell* top = newCell(lib, "SHAPES");
            auto polygon = [top](std::initializer_list<gdstk::Vec2> points, uint32_t layer) {
                auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
                p->tag = gdstk::make_tag(layer, 0);
                for (const gdstk::Vec2& v : points) p->point_array.append(v);
                top->polygon_array.append(p);
            };
            for (uint32_t layer = 0; layer < 8; ++layer) {
                const double x = layer * 12.0;
                polygon({{x, 0}, {x + 8, 0}, {x + 8, 3}, {x + 3, 3}, {x + 3, 8}, {x, 8}}, layer);                    // an L
                polygon({{x, 10}, {x + 8, 10}, {x + 8, 18}, {x + 4, 18}, {x + 4, 16}, {x + 6, 16}, {x + 6, 12}, {x + 2, 12},
                         {x + 2, 16}, {x + 4, 16}, {x + 4, 18}, {x, 18}},
                        layer);   // a square with a hole, by a slit
                polygon({{x, 20}, {x + 8, 28}, {x + 8, 20}, {x, 28}}, layer);   // a bow tie
            }
            QCOMPARE(lib.write_gds(QFile::encodeName(shapes).constData(), 0, nullptr), gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        const QString shots = qEnvironmentVariable("QUCS_TEST_SHOTS");
        for (const QString& file : {path("sample.gds"), shapes}) {
            LayoutDoc* doc = open(file);
            QVERIFY(doc != nullptr);
            LayoutView* view = doc->view();
            doc->setGridShown(false);
            view->resize(640, 480);
            view->fit();
            view->setGpu(false);
            QVERIFY(!view->gpu());
            const QImage cpu = view->picture().convertToFormat(QImage::Format_RGB32);
            view->setGpu(true);
            QVERIFY(view->gpu());
            QTRY_VERIFY(view->canvas()->frames() > 0);
            const QImage gpu = view->picture().convertToFormat(QImage::Format_RGB32);
            QCOMPARE(gpu.size(), cpu.size());
            // (the background painted: QPainter's, on the framebuffer, in a
            // grab too)
            QCOMPARE(gpu.pixelColor(1, 1), cpu.pixelColor(1, 1));
            // (Alike where a pixel of one has its colour in the other within a
            // pixel of it: an edge falling on a pixel's boundary - a half -
            // is rounded by QPainter's rule and the GPU's each its way, and
            // a glyph's grey edge the GL engine's.)
            int differ = 0, inked = 0, exact = 0;
            const QColor paper = cpu.pixelColor(1, 1);   // (a corner: nothing drawn there)
            const auto near = [](const QImage& in, int x, int y, QColor want) {
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!in.rect().contains(x + dx, y + dy)) continue;
                        const QColor k = in.pixelColor(x + dx, y + dy);
                        if (std::abs(k.red() - want.red()) + std::abs(k.green() - want.green()) + std::abs(k.blue() - want.blue()) <= 60) return true;
                    }
                return false;
            };
            for (int y = 0; y < cpu.height(); ++y)
                for (int x = 0; x < cpu.width(); ++x) {
                    const QColor a = cpu.pixelColor(x, y), b = gpu.pixelColor(x, y);
                    if (a == paper && b == paper) continue;
                    ++inked;
                    exact += a != b;
                    if (!near(gpu, x, y, a) || !near(cpu, x, y, b)) ++differ;
                }
            const QString name = QFileInfo(file).completeBaseName();
            if (!shots.isEmpty()) {
                cpu.save(shots + QStringLiteral("/%1_cpu.png").arg(name));
                gpu.save(shots + QStringLiteral("/%1_gpu.png").arg(name));
            }
            qInfo("%s: %d of %d pixels inked, %d differ (%.2f %%), %d not exactly alike", qPrintable(name), inked, cpu.width() * cpu.height(), differ,
                  100.0 * differ / std::max(1, inked), exact);
            QVERIFY(inked > 1000);
            // (On Metal pixel for pixel - none differ -; OpenGL's and
            // Direct3D's rasterization at the edges of shapes may differ.)
            const bool metal = qucs_s::gpu::backendName() == QLatin1String("Metal");
            QVERIFY2(differ < (metal ? inked / 1000 + 1 : inked / 20), qPrintable(QStringLiteral("%1 of %2").arg(differ).arg(inked)));
            // (On Metal each pixel alike but where a diagonal outline steps
            // to the next pixel at another place - 1.4 to 4.5 % of the inked
            // ones, at ratios 1 and 2 -: OpenGL's half-pixel nudge put every
            // vertical outline a pixel right of the CPU's, 27 to 35 % not
            // alike, hidden within the pixel the check above allows.)
            if (metal) QVERIFY2(exact <= inked / 10, qPrintable(QStringLiteral("%1 of %2 inked pixels not alike").arg(exact).arg(inked)));
            // Panned and zoomed: from its buffers, the CPU drawing nothing.
            const int builds = view->canvas()->builds(), renders = view->renders(), frames = view->canvas()->frames();
            view->setView(view->center() + QPointF(3, 2), view->scale() * 1.7);
            QTRY_VERIFY(view->canvas()->frames() > frames);
            QCOMPARE(view->canvas()->builds(), builds);
            QCOMPARE(view->renders(), renders);
            // A layer hidden: its buffers kept, not drawn - none made.
            QVector<LayerStyle> styles = view->styles();
            styles[0].visible = false;
            const int drawn = view->canvas()->frames();
            view->setStyles(styles);
            QTRY_VERIFY(view->canvas()->frames() > drawn);
            QCOMPARE(view->canvas()->builds(), builds);
            QCOMPARE(view->canvas()->layersAdded(), 0);
            closeAll();
        }
        // Past its budget: the CPU draws, its picture on the canvas.
        qputenv("QUCS_LAYOUT_GPU_BUDGET", "3");
        {
            LayoutDoc* doc = open(path("sample.gds"));
            QVERIFY(doc != nullptr);
            LayoutView* view = doc->view();
            view->resize(640, 480);
            view->fit();
            QVERIFY(view->gpu());
            const int renders = view->renders();
            view->repaint();
            QTRY_VERIFY(view->renders() > renders);
            QVERIFY(view->canvas()->tooLarge());
            closeAll();
        }
        qunsetenv("QUCS_LAYOUT_GPU_BUDGET");
        // The setting: a view open drawn by the CPU when it says so.
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        QVERIFY(doc->view()->gpu());
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Layout, false);
        QVERIFY(!doc->view()->gpu());
        QCOMPARE(QucsSettingsFile().value("Viewers/layoutGpu").toBool(), false);
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Layout, true);
        QVERIFY(doc->view()->gpu());
        closeAll();
    }

    // Settings… > Viewers: each viewer drawn by the CPU unless the GPU is
    // chosen for it - an opt-in each (the GPU by default, as it was, made
    // a large layout slow to show or hide a layer: the user's word). Its
    // choices shown, kept on OK, not on Cancel, and told to the viewers
    // open - a layout, a PDF, a Markdown preview, a multiphysics Graphics
    // view - each drawn as chosen where the GPU can (offscreen, where it
    // cannot, the CPU whatever is chosen); the status says which. The old
    // keys (Rendering/..., Multiphysics/gpu: the GPU on unless set off) not
    // read.
    void theViewersSettings()
    {
        using qucs_s::gpu::Viewer;
        const Viewer all[4] = {Viewer::Pdf, Viewer::Markdown, Viewer::Layout, Viewer::Multiphysics};
        QucsSettingsFile().setValue("Rendering/layout", "gpu");
        QucsSettingsFile().setValue("Multiphysics/gpu", true);
        LayoutDoc* layout = open(path("sample.gds"));
        QVERIFY(layout != nullptr);
#ifdef QUCS_HAVE_QTPDF
        const QString pdfFile = path("drawing.pdf");
        {
            QPdfWriter writer(pdfFile);
            QPainter p(&writer);
            p.drawText(QPointF(100, 100), QStringLiteral("A page."));
        }
        QVERIFY(app->gotoPage(pdfFile));
        auto* pdf = qobject_cast<PdfDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(pdf != nullptr);
        const auto pdfGpu = [pdf] { return pdf->view()->gpu(); };
#else
        const auto pdfGpu = [] { return false; };   // (no PDF viewer in this build)
#endif
        QFile md(path("drawing.md"));
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("# Drawn\n\nBy the GPU or the CPU.\n");
        md.close();
        QVERIFY(app->gotoPage(md.fileName()));
        auto* markdown = qobject_cast<MarkdownDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(markdown != nullptr);
        qucs_s::fem::GraphicsView graphics;
        graphics.resize(300, 200);
        const bool can = qucs_s::gpu::hindrance().isEmpty();
        const auto drawnAsChosen = [&] {
            QCOMPARE(layout->view()->gpu(), qucs_s::gpu::used(Viewer::Layout));
#ifdef QUCS_HAVE_QTPDF
            QCOMPARE(pdfGpu(), qucs_s::gpu::used(Viewer::Pdf));
#else
            QVERIFY(!pdfGpu());
#endif
            QCOMPARE(markdown->previewGpu(), qucs_s::gpu::used(Viewer::Markdown));
            QCOMPARE(graphics.gpu(), qucs_s::gpu::used(Viewer::Multiphysics));
        };
        // The defaults: the CPU, each - whatever the old keys say.
        for (Viewer v : all) QVERIFY(!qucs_s::gpu::wanted(v));
        drawnAsChosen();
        QVERIFY(!layout->view()->gpu());
        QVERIFY(!markdown->previewGpu());
        QVERIFY(!graphics.gpu());
        const QStringList names{"gpuPdf", "gpuMarkdown", "gpuLayout", "gpuMultiphysics"};
        {
            QucsSettingsDialog dlg(app);
            QVERIFY(dlg.findChild<QGroupBox*>("drawingGroup") == nullptr);   // (not in Appearance now)
            auto* tab = dlg.findChild<QWidget*>("viewersTab");
            QVERIFY(tab != nullptr);
            auto* tabs = dlg.findChild<QTabWidget*>();
            QVERIFY(tabs != nullptr);
            QCOMPARE(tabs->tabText(tabs->indexOf(tab)), QString("Viewers"));
            QVERIFY(tab->findChild<QGroupBox*>("gpuGroup") != nullptr);
            QList<QCheckBox*> boxes;
            for (const QString& name : names) {
                boxes << tab->findChild<QCheckBox*>(name);
                QVERIFY(boxes.last() != nullptr);
                QVERIFY(!boxes.last()->isChecked());
            }
            QCOMPARE(boxes[2]->text(), qucs_s::gpu::viewerName(Viewer::Layout));
            auto* status = tab->findChild<QLabel*>("gpuStatus");
            QVERIFY(status != nullptr);
            if (qucs_s::gpu::hindrance().isEmpty()) QVERIFY2(status->text().contains(qucs_s::gpu::backendName()) && !qucs_s::gpu::backendName().isEmpty(), qPrintable(status->text()));
            else QCOMPARE(status->text(), qucs_s::gpu::hindrance());
            // The GPU chosen for each.
            for (QCheckBox* box : boxes) box->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        }
        for (Viewer v : all) {
            QVERIFY(qucs_s::gpu::wanted(v));
            QCOMPARE(QucsSettingsFile().value(qucs_s::gpu::settingKey(v)).toBool(), true);
        }
        QVERIFY(qucs_s::gpu::settingKey(Viewer::Layout).startsWith("Viewers/"));
        drawnAsChosen();
        QCOMPARE(layout->view()->gpu(), can);
        QCOMPARE(markdown->previewGpu(), can);
        // Shown again as chosen; Cancel keeps nothing.
        {
            QucsSettingsDialog dlg(app);
            QVERIFY(dlg.findChild<QCheckBox*>("gpuPdf")->isChecked());
            dlg.findChild<QCheckBox*>("gpuLayout")->setChecked(false);
            dlg.reject();
        }
        QVERIFY(qucs_s::gpu::wanted(Viewer::Layout));
        QCOMPARE(layout->view()->gpu(), can);
        // QUCS_GPU=0: the CPU whatever is chosen, said.
        qputenv("QUCS_GPU", "0");
        QVERIFY(qucs_s::gpu::hindrance().contains("QUCS_GPU=0"));
        qucs_s::gpu::setWanted(Viewer::Markdown, true);
        QVERIFY(!markdown->previewGpu());
        qunsetenv("QUCS_GPU");
        // Back to the defaults: the CPU.
        for (Viewer v : all) QucsSettingsFile().remove(qucs_s::gpu::settingKey(v));
        QucsSettingsFile().remove("Rendering/layout");
        QucsSettingsFile().remove("Multiphysics/gpu");
        for (Viewer v : all) emit qucs_s::gpu::Notifier::instance()->changed(v);
        drawnAsChosen();
        QVERIFY(!layout->view()->gpu());
        closeAll();
    }

    // A photonic layout's rings and bends: 8,000 of 96 points, half not
    // convex (C's, an arc 300° wide, 1 µm thick), on layers 0 to 3.
    static bool writeCurves(const QString& file)
    {
        gdstk::Library lib = {};
        lib.init("CURVES", 1e-6, 1e-9);
        gdstk::Cell* top = newCell(lib, "CURVES");
        for (int i = 0; i < 80; ++i)
            for (int j = 0; j < 100; ++j) {
                auto* p = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
                p->tag = gdstk::make_tag(uint32_t((i + j) % 4), 0);
                const double cx = i * 10.0, cy = j * 10.0;
                const bool c = (i + j) % 2;
                for (int k = 0; k < 48; ++k) {
                    const double a = (c ? 0.5 + 5.3 * k / 47.0 : 2 * M_PI * k / 48.0);
                    p->point_array.append({cx + 4 * std::cos(a), cy + 4 * std::sin(a)});
                }
                for (int k = 47; k >= 0; --k) {
                    const double a = (c ? 0.5 + 5.3 * k / 47.0 : 2 * M_PI * k / 48.0);
                    p->point_array.append({cx + (c ? 3.0 : 4.0) * std::cos(a), cy + (c ? 3.0 : 4.0) * std::sin(a)});
                }
                top->polygon_array.append(p);
            }
        const bool ok = lib.write_gds(QFile::encodeName(file).constData(), 0, nullptr) == gdstk::ErrorCode::NoError;
        lib.free_all();
        return ok;
    }

    // A layer shown or hidden on the GPU at once: its buffers kept, hidden,
    // not drawn - none made again; one hidden from the start flattened
    // alone and added when shown. (Made again whole at each, a chip's
    // layer took as long to show or hide as the cell to load into the GPU:
    // the user's "very laggy when turning a layer on or off".)
    void aLayerShownOrHiddenAtOnce()
    {
#ifdef QUCS_GPU_VIEW
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        const QString curves = path("curves.gds");
        QVERIFY(writeCurves(curves));
        LayoutDoc* doc = open(curves);
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        doc->setGridShown(false);
        view->resize(1000, 800);
        view->fit();
        doc->setLayerVisible(1, false);   // (hidden before the GPU has it)
        view->setGpu(true);
        QVERIFY(view->gpu());
        QElapsedTimer t;
        t.start();
        (void)view->canvas()->grabFramebuffer();
        const qint64 build = t.elapsed();
        const int builds = view->canvas()->builds();
        QCOMPARE(view->canvas()->flattened().shapes, quint64(6000));
        // (the layer's outline colour in the framebuffer: inked by it or not)
        const auto inked = [view](int layer) {
            const QColor want = view->shownStyle(view->styles().at(layer)).frame;
            const QImage frame = view->canvas()->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
            int n = 0;
            for (int y = 0; y < frame.height(); y += 2)
                for (int x = 0; x < frame.width(); x += 2) {
                    const QColor k = frame.pixelColor(x, y);
                    n += std::abs(k.red() - want.red()) + std::abs(k.green() - want.green()) + std::abs(k.blue() - want.blue()) <= 30;
                }
            return n;
        };
        QVERIFY(inked(0) > 100);
        QCOMPARE(inked(1), 0);
        // Hidden and shown again: none made, drawn as asked.
        t.restart();
        doc->setLayerVisible(0, false);
        (void)view->canvas()->grabFramebuffer();
        const qint64 hide = t.elapsed();
        QCOMPARE(inked(0), 0);
        t.restart();
        doc->setLayerVisible(0, true);
        (void)view->canvas()->grabFramebuffer();
        const qint64 show = t.elapsed();
        QVERIFY(inked(0) > 100);
        QCOMPARE(view->canvas()->builds(), builds);
        QCOMPARE(view->canvas()->layersAdded(), 0);
        // The layer hidden from the start: flattened alone, added.
        t.restart();
        doc->setLayerVisible(1, true);
        (void)view->canvas()->grabFramebuffer();
        const qint64 added = t.elapsed();
        QVERIFY(inked(1) > 100);
        QCOMPARE(view->canvas()->builds(), builds);
        QCOMPARE(view->canvas()->layersAdded(), 1);
        QCOMPARE(view->canvas()->flattened().shapes, quint64(8000));
        // (Its vertices the GPU's alone, as a whole build leaves them - the
        // CPU kept a copy: the hunt of 2026-10-17, B1.)
        QCOMPARE(view->canvas()->heldFloats(), std::size_t(0));
        // Hidden again, shown again: nothing made.
        doc->setLayerVisible(1, false);
        (void)view->canvas()->grabFramebuffer();
        doc->setLayerVisible(1, true);
        (void)view->canvas()->grabFramebuffer();
        QCOMPARE(view->canvas()->layersAdded(), 1);
        QCOMPARE(view->canvas()->builds(), builds);
        // (Made again whole - the texts shown or not, as a layer shown or
        // hidden made them before -: timed, for the record.)
        t.restart();
        view->setLabelsShown(!view->labelsShown());
        (void)view->canvas()->grabFramebuffer();
        const qint64 whole = t.elapsed();
        view->setLabelsShown(!view->labelsShown());
        (void)view->canvas()->grabFramebuffer();
        QCOMPARE(view->canvas()->builds(), builds + 2);
        // A hidden layer's texts not drawn either (the sample's on 10/0).
        {
            LayoutDoc* sample = open(path("sample.gds"));
            QVERIFY(sample != nullptr);
            sample->view()->resize(640, 480);
            sample->view()->fit();
            sample->view()->setGpu(true);
            QVERIFY(sample->view()->labelsShown());
            (void)sample->view()->canvas()->grabFramebuffer();
            QVERIFY(sample->view()->canvas()->textsDrawn() > 0);
            const int text = sample->layout()->layerIndex(LayerKey{10, 0});
            QVERIFY(text >= 0);
            sample->setLayerVisible(text, false);
            (void)sample->view()->canvas()->grabFramebuffer();
            QCOMPARE(sample->view()->canvas()->textsDrawn(), 0);
            sample->setLayerVisible(text, true);
            (void)sample->view()->canvas()->grabFramebuffer();
            QVERIFY(sample->view()->canvas()->textsDrawn() > 0);
            QCOMPARE(sample->view()->canvas()->builds(), 1);
            app->DocumentTab->setCurrentWidget(doc);
        }
        // On the CPU, for the record: each a picture drawn again.
        view->setGpu(false);
        (void)view->grab();
        t.restart();
        doc->setLayerVisible(0, false);
        (void)view->grab();
        const qint64 cpuHide = t.elapsed();
        doc->setLayerVisible(0, true);
        qInfo("8,000 curves on the GPU: its first frame %lld ms; a layer hidden %lld ms, shown %lld ms, one shown for the first time %lld ms "
              "(its 2,000 alone); made again whole %lld ms (each layer shown or hidden, before); on the CPU hidden %lld ms",
              build, hide, show, added, whole, cpuHide);
        closeAll();
#else
        QSKIP("a build without Qt's RHI");
#endif
    }

    // The GPU's canvas makes its buffers again when it must, and only then
    // (the hunt of 2026-10-17): a layout set again - one read anew where a
    // freed one was, taken for it by its address (N1) - is made again; past
    // its budget, the CPU drawing the cell, it is not made again at every
    // frame (F1: each frame flattened 2 million shapes first) but when the
    // layers shown change, one hidden perhaps bringing it under.
    void theGpuCanvasMakesWhatItMustOnly()
    {
#ifdef QUCS_GPU_VIEW
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        view->resize(640, 480);
        view->fit();
        view->setGpu(true);
        QVERIFY(view->gpu());
        (void)view->canvas()->grabFramebuffer();
        int builds = view->canvas()->builds();
        view->setLayout(doc->layout());
        (void)view->canvas()->grabFramebuffer();
        QCOMPARE(view->canvas()->builds(), builds + 1);
        // Past the budget.
        qputenv("QUCS_LAYOUT_GPU_BUDGET", "5");
        struct Unset {
            ~Unset() { qunsetenv("QUCS_LAYOUT_GPU_BUDGET"); }
        } unset;
        view->setLayout(doc->layout());
        (void)view->canvas()->grabFramebuffer();
        QVERIFY(view->canvas()->flattened().tooLarge);
        builds = view->canvas()->builds();
        for (int i = 0; i < 5; ++i) {
            view->setView(view->center() + QPointF(0.5, 0), view->scale());
            (void)view->canvas()->grabFramebuffer();
        }
        QCOMPARE(view->canvas()->builds(), builds);
        doc->setLayerVisible(0, false);
        (void)view->canvas()->grabFramebuffer();
        QCOMPARE(view->canvas()->builds(), builds + 1);
        (void)view->canvas()->grabFramebuffer();
        QCOMPARE(view->canvas()->builds(), builds + 1);
        doc->setLayerVisible(0, true);
        closeAll();
#else
        QSKIP("a build without Qt's RHI");
#endif
    }

    // The layered canvas sends the GPU each layer's ink, not the window's
    // whole picture at each frame (a small layout's pan took twice the
    // CPU's time at full screen): the background the pass's clear - with no
    // grid, a frame sends nothing -, a ruler its box. A ruler taken away and
    // one drawn elsewhere: nothing of the first one shown - the picture a
    // new canvas draws, exactly.
    void theGpuCanvasSendsItsInkAlone()
    {
#ifdef QUCS_GPU_VIEW
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        // (Shapes alone: a layout's texts are QPainter's, over them.)
        const QString file = path("blocks.gds");
        {
            gdstk::Library lib = {};
            lib.init("BLOCKS", 1e-6, 1e-9);
            gdstk::Cell* top = newCell(lib, "BLOCKS");
            for (int i = 0; i < 6; ++i) addRectangle(top, {i * 3.0, 0}, {i * 3.0 + 2, 2.0 + i}, uint32_t(1 + i % 3));
            QCOMPARE(lib.write_gds(QFile::encodeName(file).constData(), 0, nullptr), gdstk::ErrorCode::NoError);
            lib.free_all();
        }
        LayoutDoc* doc = open(file);
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        doc->setGridShown(false);
        view->resize(640, 480);
        view->fit();
        view->setGpu(true);
        QVERIFY(view->gpu());
        const QImage first = view->canvas()->grabFramebuffer();
        const qint64 whole = qint64(first.width()) * first.height();
        const auto sentBy = [view] {
            const qint64 before = view->canvas()->sentPixels();
            (void)view->canvas()->grabFramebuffer();
            return view->canvas()->sentPixels() - before;
        };
        QCOMPARE(sentBy(), 0);
        view->setView(view->center() + QPointF(0.5, 0), view->scale());
        QCOMPARE(sentBy(), 0);
        const QRectF seen = view->visibleRegion();
        const QPointF a = seen.topLeft() + QPointF(seen.width() * 0.2, seen.height() * 0.2);
        view->addRuler(QLineF(a, a + QPointF(seen.width() * 0.2, seen.height() * 0.1)));
        QCOMPARE(sentBy(), whole);   // (its layer's texture new, its texels undefined: once)
        const qint64 ruler = sentBy();
        QVERIFY2(ruler > 0 && ruler < whole / 10, qPrintable(QStringLiteral("%1 of %2").arg(ruler).arg(whole)));
        view->clearRulers();
        QCOMPARE(sentBy(), 0);
        const QPointF b = seen.topLeft() + QPointF(seen.width() * 0.25, seen.height() * 0.5);
        view->addRuler(QLineF(b, b + QPointF(seen.width() * 0.15, -seen.height() * 0.05)));
        const qint64 other = sentBy();
        QVERIFY2(other > 0 && other < whole / 10, qPrintable(QStringLiteral("%1 of %2").arg(other).arg(whole)));
        const QImage shown = view->canvas()->grabFramebuffer();
        view->setGpu(false);
        view->setGpu(true);
        QVERIFY(view->gpu());
        const QImage fresh = view->canvas()->grabFramebuffer();
        QCOMPARE(shown.size(), fresh.size());
        QVERIFY(shown == fresh);
        // The grid shown: the canvas drawn again, all in sight sent.
        doc->setGridShown(true);
        QVERIFY(sentBy() > whole / 2);
        view->clearRulers();
        closeAll();
#else
        QSKIP("a build without Qt's RHI");
#endif
    }

    // Curves - a photonic layout's rings and bends, 8,000 of them of 96
    // points, half not convex (C's), on patterned layers, all in sight:
    // drawn by the GPU from its buffers faster than the CPU draws them,
    // and alike.
    void theGpuDrawsCurvesFaster()
    {
#ifdef QUCS_GPU_VIEW
        if (!qucs_s::gpu::hindrance().isEmpty()) QSKIP(qPrintable(qucs_s::gpu::hindrance()));
        const QString curves = path("curves.gds");
        QVERIFY(writeCurves(curves));
        LayoutDoc* doc = open(curves);
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        doc->setGridShown(false);
        view->resize(1000, 800);
        view->fit();
        view->setGpu(false);
        QElapsedTimer t;
        qint64 cpu = 0;
        for (int k = 0; k < 3; ++k) {
            view->setView(view->center() + QPointF(1, 1), view->scale());
            t.restart();
            (void)view->grab();
            cpu = std::max(cpu, t.elapsed());
        }
        view->fit();
        const QImage cpuPicture = view->grab().toImage().convertToFormat(QImage::Format_RGB32);
        view->setGpu(true);
        QVERIFY(view->gpu());
        t.restart();
        (void)view->canvas()->grabFramebuffer();
        const qint64 build = t.elapsed();
        qint64 gpu = 0;
        for (int k = 0; k < 3; ++k) {
            view->setView(view->center() + QPointF(1, 1), view->scale());
            t.restart();
            (void)view->canvas()->grabFramebuffer();
            gpu = std::max(gpu, t.elapsed());
        }
        QCOMPARE(view->canvas()->builds(), 1);
        QCOMPARE(view->canvas()->flattened().approximated, quint64(0));
        view->fit();
        const QImage gpuPicture = view->grab().toImage().convertToFormat(QImage::Format_RGB32);
        int differ = 0;
        for (int y = 0; y < cpuPicture.height(); y += 3)
            for (int x = 0; x < cpuPicture.width(); x += 3) differ += cpuPicture.pixel(x, y) != gpuPicture.pixel(x, y);
        if (!qEnvironmentVariable("QUCS_TEST_SHOTS").isEmpty()) {
            cpuPicture.save(qEnvironmentVariable("QUCS_TEST_SHOTS") + QStringLiteral("/curves_cpu.png"));
            gpuPicture.save(qEnvironmentVariable("QUCS_TEST_SHOTS") + QStringLiteral("/curves_gpu.png"));
        }
        qInfo("8,000 curves of 96 points: the CPU %lld ms a view, the GPU %lld ms (its buffers %lld ms, once); %d of %d sampled "
              "pixels differ",
              cpu, gpu, build, differ, (cpuPicture.width() / 3) * (cpuPicture.height() / 3));
        QVERIFY2(gpu * 2 < cpu, qPrintable(QStringLiteral("%1 against %2").arg(gpu).arg(cpu)));
        QVERIFY(differ < (cpuPicture.width() / 3) * (cpuPicture.height() / 3) / 10);
        closeAll();
#else
        QSKIP("a build without Qt's RHI");
#endif
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
        // What is drawn counted as the CPU draws the view, whichever draws
        // it (the GPU's canvas, painted when it will, had left them nought:
        // the update check of 48d5fa8).
        const QJsonObject drawn = j.value("drawn").toObject();
        QVERIFY2(drawn.value("shapes").toInt() > 0, qPrintable(text(r)));
        const bool onGpu = doc->view()->gpu();
        doc->view()->setGpu(!onGpu);
        r = call("show_layout", {{"cell", "TOP"}, {"region", QJsonArray{9, -1, 13, 2}}, {"layers", "all"}, {"depth", "all"}});
        QCOMPARE(json(r).value("drawn").toObject(), drawn);
        doc->view()->setGpu(onGpu);
        QCOMPARE(doc->view()->gpu(), onGpu);
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
        // 'texts' as the tab's Texts button: the button set with the canvas,
        // and kept - it was the canvas's alone. Texts in the file and none
        // drawn: the answer says why, the check of 3 November, 6, reading
        // "texts: 0" of a file with texts while the button was off.
        r = call("show_layout", {{"path", "sample.gds"}, {"texts", false}});
        QVERIFY(!doc->view()->labelsShown() && !doc->labelsButton()->isChecked());
        QCOMPARE(json(r).value("drawn").toObject().value("texts").toInt(-1), 0);
        QVERIFY2(json(r).value("texts hidden").toString().contains(QString("the file's cells hold %1 texts").arg(doc->layout()->labelCount))
                     && json(r).value("texts hidden").toString().contains("'texts': true"),
                 qPrintable(text(r)));
        QCOMPARE(QucsSettingsFile().value("LayoutViewer/labels", true).toBool(), false);
        r = call("show_layout", {{"path", "sample.gds"}});
        QVERIFY2(json(r).contains("texts hidden"), qPrintable(text(r)));
        doc->labelsButton()->click();   // (the user's: it draws them)
        QVERIFY(doc->view()->labelsShown());
        r = call("show_layout", {{"path", "sample.gds"}, {"texts", false}});
        QVERIFY(!doc->labelsButton()->isChecked());
        r = call("show_layout", {{"path", "sample.gds"}, {"texts", true}});
        QVERIFY(doc->view()->labelsShown() && doc->labelsButton()->isChecked());
        QVERIFY2(json(r).value("drawn").toObject().value("texts").toInt() > 0 && !json(r).contains("texts hidden"), qPrintable(text(r)));
        QCOMPARE(QucsSettingsFile().value("LayoutViewer/labels", false).toBool(), true);
    }

    // find_shapes by what a text says: a part of it, or * and ?, in any case.
    void find_shapesFindsTextsByWhatTheySay()
    {
        QJsonObject j = json(call("find_shapes", {{"path", "sample.gds"}, {"text", "OUT"}}));
        QCOMPARE(j.value("found").toInt(), 3);
        QCOMPARE(j.value("shapes").toArray().at(0).toObject().value("kind").toString(), QString("text"));
        QCOMPARE(json(call("find_shapes", {{"path", "sample.gds"}, {"text", "o?t"}})).value("found").toInt(), 3);
        QCOMPARE(json(call("find_shapes", {{"path", "sample.gds"}, {"text", "U"}})).value("found").toInt(), 3);
        QCOMPARE(json(call("find_shapes", {{"path", "sample.gds"}, {"text", "IN"}})).value("found").toInt(), 0);
        // With a region: the one there.
        QCOMPARE(json(call("find_shapes", {{"path", "sample.gds"}, {"text", "OUT"}, {"region", QJsonArray{11, 0, 13, 1}}})).value("found").toInt(), 1);
        QVERIFY(failed(call("find_shapes", {{"path", "sample.gds"}, {"text", ""}})));
        QVERIFY(failed(call("find_shapes", {{"path", "sample.gds"}, {"text", "OUT"}, {"texts", false}})));
    }

    // find_placements: where a cell is placed - each placement's origin, turn,
    // mirror and bounds in the cell searched, every copy of an array, the
    // cells it is placed through; in a region, to a depth; a cycle and an
    // array of 65535 by 65535 cut short; a cell placed nowhere, one that is
    // none.
    void find_placementsFindsWhereACellIsPlaced()
    {
        QJsonObject r = call("find_placements", {{"path", "sample.gds"}, {"cell", "INV"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QJsonObject j = json(r);
        QCOMPARE(j.value("in").toString(), QString("TOP"));
        QCOMPARE(j.value("found").toInt(), 3);
        QHash<QString, QJsonObject> at;   // by origin
        for (const QJsonValue& v : j.value("placements").toArray()) {
            const QJsonArray o = v.toObject().value("origin").toArray();
            at.insert(QStringLiteral("%1,%2").arg(o.at(0).toDouble()).arg(o.at(1).toDouble()), v.toObject());
        }
        QCOMPARE(at.value("10,0").value("rotation").toDouble(), 0.0);
        QVERIFY(!at.value("10,0").contains("mirrored"));
        QCOMPARE(at.value("10,0").value("bounds").toArray(), QJsonArray({10, 0, 12, 1}));
        QCOMPARE(at.value("10,0").value("in").toString(), QString("TOP"));
        QCOMPARE(at.value("20,0").value("rotation").toDouble(), 90.0);
        QCOMPARE(at.value("20,0").value("bounds").toArray(), QJsonArray({19, 0, 20, 2}));
        QCOMPARE(at.value("30,0").value("rotation").toDouble(), 0.0);
        QVERIFY(at.value("30,0").value("mirrored").toBool());
        QCOMPARE(at.value("30,0").value("bounds").toArray(), QJsonArray({30, -1, 32, 0}));
        // An array's copies, each its own; in a region, the one there.
        j = json(call("find_placements", {{"path", "sample.gds"}, {"cell", "VIA"}}));
        QCOMPARE(j.value("found").toInt(), 6);
        bool oneOne = false;
        for (const QJsonValue& v : j.value("placements").toArray())
            if (v.toObject().value("origin").toArray() == QJsonArray({1, 1})) oneOne = v.toObject().value("copy").toString() == QString("5 of 6");
        QVERIFY2(oneOne, qPrintable(text(call("find_placements", {{"path", "sample.gds"}, {"cell", "VIA"}}))));
        j = json(call("find_placements", {{"path", "sample.gds"}, {"cell", "VIA"}, {"region", QJsonArray{1.8, -0.5, 3, 0.4}}}));
        QCOMPARE(j.value("found").toInt(), 1);
        QCOMPARE(j.value("placements").toArray().at(0).toObject().value("origin").toArray(), QJsonArray({2, 0}));
        // Not in this cell; a cell that is none; 'limit'.
        j = json(call("find_placements", {{"path", "sample.gds"}, {"cell", "VIA"}, {"in", "INV"}}));
        QCOMPARE(j.value("found").toInt(), 0);
        QVERIFY2(j.value("note").toString().contains("VIA is not placed in INV"), qPrintable(j.value("note").toString()));
        const QJsonObject none = call("find_placements", {{"path", "sample.gds"}, {"cell", "NOPE"}});
        QVERIFY(failed(none) && text(none).contains("top cells: TOP"));
        j = json(call("find_placements", {{"path", "sample.gds"}, {"cell", "VIA"}, {"limit", 2}}));
        QCOMPARE(j.value("placements").toArray().size(), 2);
        QVERIFY(j.value("note").toString().contains("2 listed of 6"));
        QVERIFY(failed(call("find_placements", {{"path", "sample.gds"}})));
        QVERIFY(failed(call("find_placements", {{"path", "sample.gds"}, {"cell", "VIA"}, {"limit", 0}})));
        // Down a chain: through the cells between, its origin every level's
        // together; deep enough or not.
        writeHierarchy(path("chain.gds"), false, 3, false, false);
        j = json(call("find_placements", {{"path", "chain.gds"}, {"cell", "LEAF"}, {"in", "C2"}}));
        QCOMPARE(j.value("found").toInt(), 1);
        QCOMPARE(j.value("placements").toArray().at(0).toObject().value("via").toString(), QString("C2 > C1 > C0"));
        QCOMPARE(j.value("placements").toArray().at(0).toObject().value("in").toString(), QString("C0"));
        QCOMPARE(j.value("placements").toArray().at(0).toObject().value("origin").toArray(), QJsonArray({1.5, 1.5}));
        QCOMPARE(json(call("find_placements", {{"path", "chain.gds"}, {"cell", "LEAF"}, {"in", "C2"}, {"depth", 1}})).value("found").toInt(), 0);
        QCOMPARE(json(call("find_placements", {{"path", "chain.gds"}, {"cell", "LEAF"}, {"in", "C2"}, {"depth", 2}})).value("found").toInt(), 1);
        // A cycle, cut where the reader cuts it (and warns): PING placed in C2,
        // PONG no longer placed in PING; an array of 65535 by 65535 counted
        // to 100000.
        writeHierarchy(path("cycle.gds"), false, 3, true, false);
        r = call("find_placements", {{"path", "cycle.gds"}, {"cell", "PING"}, {"in", "C2"}});
        QVERIFY2(!failed(r) && json(r).value("found").toInt() == 1, qPrintable(text(r)));
        QCOMPARE(json(call("find_placements", {{"path", "cycle.gds"}, {"cell", "PONG"}, {"in", "C2"}})).value("found").toInt(), 0);
        // (In OASIS: a GDSII array's counts are 16 bits, and this one reads as
        // a single copy there.)
        writeHierarchy(path("array.oas"), true, 2, false, true);
        r = call("find_placements", {{"path", "array.oas"}, {"cell", "LEAF"}, {"in", "C1"}});
        QVERIFY2(!failed(r) && json(r).value("found").toString() == QString("100000 or more"), qPrintable(text(r)));
    }

    // show_layout selects as a click does and measures as the ruler does:
    // what is selected and the rulers - Claude's and the user's - in its
    // answer and get_layout's.
    void show_layoutSelectsAndMeasures()
    {
        // The INV at (10, 0): at (11, 0.5) its path and its rectangle - the
        // smaller first; at the same point again, the next.
        QJsonObject r = call("show_layout", {{"path", "sample.gds"}, {"cell", "TOP"}, {"layers", "all"}, {"depth", "all"},
                                             {"region", QJsonArray{9, -1, 13, 2}}, {"select", QJsonArray{11, 0.5}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* doc = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        QJsonObject selected = json(r).value("selected").toObject();
        QCOMPARE(selected.value("kind").toString(), QString("path"));
        QCOMPARE(selected.value("via").toString(), QString("TOP > INV"));
        QVERIFY(doc->view()->selection().has_value());
        r = call("show_layout", {{"path", "sample.gds"}, {"select", QJsonArray{11, 0.5}}});
        selected = json(r).value("selected").toObject();
        QCOMPARE(selected.value("kind").toString(), QString("polygon"));
        QCOMPARE(selected.value("points").toArray(), QJsonArray({QJsonArray{10, 0}, QJsonArray{12, 0}, QJsonArray{12, 1}, QJsonArray{10, 1}}));
        QCOMPARE(json(call("get_layout", {{"path", "sample.gds"}})).value("tab").toObject().value("selected").toObject().value("kind").toString(),
                 QString("polygon"));
        // Nothing there: said, and nothing selected.
        r = call("show_layout", {{"path", "sample.gds"}, {"select", QJsonArray{50, 50}}});
        QVERIFY2(json(r).contains("not selected") && !json(r).contains("selected"), qPrintable(text(r)));
        QVERIFY(!doc->view()->selection());
        // The user's own click, read; none, cleared.
        QVERIFY(doc->view()->selectAt(doc->view()->toPixel(QPointF(11, 0.2))));
        QCOMPARE(json(call("show_layout", {{"path", "sample.gds"}})).value("selected").toObject().value("kind").toString(), QString("polygon"));
        r = call("show_layout", {{"path", "sample.gds"}, {"select", "none"}});
        QVERIFY(!doc->view()->selection() && !json(r).contains("selected"));
        // Out of sight: brought into view, at the zoom shown - the mirrored
        // INV's path, the smaller of the two there.
        const double scale = doc->view()->scale();
        r = call("show_layout", {{"path", "sample.gds"}, {"select", QJsonArray{31, -0.5}}});
        QVERIFY2(json(r).value("selected").toObject().value("bounds").toArray() == QJsonArray({30, -0.6, 32, -0.4}), qPrintable(text(r)));
        QVERIFY(doc->view()->visibleRegion().contains(QPointF(31, -0.5)));
        QCOMPARE(doc->view()->scale(), scale);
        // Rulers: each end snapped to the corner near it, as a drag snaps it.
        // (Lists of one list built by append: braced, a newer clang copies it.)
        const auto oneRuler = [](const QJsonArray& ends) {
            QJsonArray list;
            list.append(ends);
            return list;
        };
        QCOMPARE(oneRuler(QJsonArray{1, 2, 3, 4}).size(), 1);
        r = call("show_layout", {{"path", "sample.gds"}, {"select", "none"}, {"region", QJsonArray{9, -1, 13, 2}},
                                 {"rulers", oneRuler(QJsonArray{10.01, 0.005, 12.01, 0.99})}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject ruler = json(r).value("rulers").toArray().at(0).toObject();
        QCOMPARE(ruler.value("from").toArray(), QJsonArray({10, 0}));
        QCOMPARE(ruler.value("to").toArray(), QJsonArray({12, 1}));
        QCOMPARE(ruler.value("across").toDouble(), 2.0);
        QCOMPARE(ruler.value("up").toDouble(), 1.0);
        QCOMPARE(ruler.value("length").toDouble(), 2.236);
        QVERIFY2(std::abs(ruler.value("angle").toDouble() - 26.565) < 1e-3, qPrintable(text(r)));
        QCOMPARE(doc->view()->rulers().size(), 1);
        QCOMPARE(doc->view()->rulers().at(0), QLineF(10, 0, 12, 1));
        // Far from any shape: where it was asked, on the database grid.
        r = call("show_layout", {{"path", "sample.gds"}, {"rulers", oneRuler(QJsonArray{50.0004, 50, 51, 50})}});
        QCOMPARE(json(r).value("rulers").toArray().size(), 2);
        QCOMPARE(json(r).value("rulers").toArray().at(1).toObject().value("from").toArray(), QJsonArray({50, 50}));
        // The user's own, read; clear, gone.
        doc->view()->addRuler(QLineF(0, 0, 0, 3));
        QCOMPARE(json(call("get_layout", {{"path", "sample.gds"}})).value("tab").toObject().value("rulers").toArray().size(), 3);
        r = call("show_layout", {{"path", "sample.gds"}, {"rulers", "clear"}});
        QVERIFY(doc->view()->rulers().isEmpty() && !json(r).contains("rulers"));
        // What is not right: refused.
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"select", QJsonArray{1}}})));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"select", "all"}})));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"rulers", oneRuler(QJsonArray{1, 2, 3})}})));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"rulers", "some"}})));
        QVERIFY(doc->view()->rulers().isEmpty());
    }

    // show_layout colours the layers - one by one, from a .lyp beside the
    // layout, the palette again -, sets the canvas light or dark, the
    // colours' contrast and the grid's style, and reads the file again.
    void show_layoutColoursTheLayersAndTheCanvas()
    {
        QJsonObject r = call("show_layout", {{"path", "sample.gds"}, {"colours", QJsonObject{{"1/0", "#ff8000"}, {"3", "navy"}}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* doc = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        const int metal = doc->layout()->layerIndex({1, 0}), via = doc->layout()->layerIndex({3, 0});
        QCOMPARE(doc->view()->styles().at(metal).frame, QColor("#ff8000"));
        QCOMPARE(doc->view()->styles().at(metal).fill, QColor("#ff8000"));
        QCOMPARE(doc->view()->styles().at(via).frame, QColor("navy"));
        QString colour;
        for (const QJsonValue& v : json(call("get_layout", {{"path", "sample.gds"}})).value("layers").toArray())
            if (v.toObject().value("layer").toString() == QString("1/0")) colour = v.toObject().value("colour").toString();
        QCOMPARE(colour, QString("#ff8000"));
        // A colour or a layer that is none: refused, none changed.
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"colours", QJsonObject{{"1/0", "notacolour"}}}})));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"colours", QJsonObject{{"1/0", "red"}, {"99/0", "red"}}}})));
        QCOMPARE(doc->view()->styles().at(metal).frame, QColor("#ff8000"));
        // A .lyp beside the layout; the palette again; one that is none.
        QVERIFY(write(path("props.lyp"),
                      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<layer-properties>\n"
                      " <properties><frame-color>#123456</frame-color><fill-color>#123456</fill-color>"
                      "<visible>true</visible><name>M1</name><source>1/0@1</source></properties>\n"
                      "</layer-properties>\n"));
        r = call("show_layout", {{"path", "sample.gds"}, {"layer_properties", "props.lyp"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("layer properties").toString(), QDir::toNativeSeparators(path("props.lyp")));
        QCOMPARE(doc->layerPropertiesFile(), path("props.lyp"));
        QCOMPARE(doc->view()->styles().at(metal).frame, QColor("#123456"));
        r = call("show_layout", {{"path", "sample.gds"}, {"layer_properties", "palette"}});
        QVERIFY(doc->layerPropertiesFile().isEmpty());
        QCOMPARE(json(r).value("layer properties").toString(), QString("the palette"));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"layer_properties", "nowhere.lyp"}})));
        // A .lyp named alone, beside a layout in a folder of its own (two
        // there, so that neither is taken when it opens).
        writeSample(path("lyp/chip.gds"), false);
        const char* lypText = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<layer-properties>\n"
                              " <properties><frame-color>#654321</frame-color><fill-color>#654321</fill-color>"
                              "<visible>true</visible><source>1/0@1</source></properties>\n</layer-properties>\n";
        QVERIFY(write(path("lyp/colours.lyp"), lypText) && write(path("lyp/other.lyp"), lypText));
        r = call("show_layout", {{"path", "lyp/chip.gds"}, {"layer_properties", "colours.lyp"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* chip = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QCOMPARE(chip->layerPropertiesFile(), path("lyp/colours.lyp"));
        QCOMPARE(chip->view()->styles().at(chip->layout()->layerIndex({1, 0})).frame, QColor("#654321"));
        // The canvas: dark, the colours as they are, the grid's lines.
        r = call("show_layout", {{"path", "sample.gds"}, {"theme", "dark"}, {"contrast", false}, {"grid", true}, {"grid_style", "lines"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(doc->view()->theme(), LayoutView::Theme::Dark);
        QVERIFY(!doc->view()->colorsAdapted());
        QCOMPARE(doc->view()->gridStyle(), LayoutView::GridStyle::Lines);
        QCOMPARE(json(r).value("theme").toString(), QString("dark"));
        QCOMPARE(json(r).value("colours in contrast").toBool(true), false);
        QCOMPARE(json(r).value("grid style").toString(), QString("lines"));
        // (The grid said as drawn: it said "a point every" of lines too -
        // the check of 3 November, 6.)
        const QString step = micrometres(*doc->layout(), doc->view()->gridStep());
        QVERIFY2(json(r).value("grid").toString().startsWith(QString("a line every %1 µm across and up").arg(step)), qPrintable(text(r)));
        r = call("show_layout", {{"path", "sample.gds"}, {"grid_style", "crosses"}});
        QVERIFY2(json(r).value("grid").toString().startsWith(QString("a cross every %1 µm").arg(step)), qPrintable(text(r)));
        QCOMPARE(json(call("get_layout", {{"path", "sample.gds"}})).value("tab").toObject().value("theme").toString(), QString("dark"));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"theme", "blue"}})));
        QVERIFY(failed(call("show_layout", {{"path", "sample.gds"}, {"grid_style", "stars"}})));
        r = call("show_layout", {{"path", "sample.gds"}, {"theme", "application"}, {"contrast", true}, {"grid_style", "dots"}});
        QCOMPARE(doc->view()->theme(), LayoutView::Theme::Application);
        QVERIFY(doc->view()->colorsAdapted() && !json(r).contains("theme") && !json(r).contains("colours in contrast"));
        // Read again: the cell kept, what the file has now shown.
        writeSample(path("again.gds"), false);
        r = call("show_layout", {{"path", "again.gds"}, {"cell", "TOP"}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* again = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QCOMPARE(again->layout()->cells.size(), 3);
        QTest::qWait(1100);   // (a file time of another second)
        writeSample(path("again.gds"), false, 0, true);
        r = call("show_layout", {{"path", "again.gds"}, {"reload", true}});
        QVERIFY2(!failed(r) && json(r).value("read again").toBool(), qPrintable(text(r)));
        QCOMPARE(again->layout()->cells.size(), 4);
        QCOMPARE(json(r).value("cell").toString(), QString("TOP"));
    }

    // export_image writes a layout's view as its tab shows it: a picture of
    // twice its pixels (or 'scale's) the same as the view's own, an SVG and a
    // PDF as vectors, a transparent PNG; what is a schematic's refused.
    void export_imageWritesTheLayoutsView()
    {
        QJsonObject r = call("show_layout", {{"path", "sample.gds"}, {"cell", "TOP"}, {"layers", "all"}, {"depth", "all"},
                                             {"region", QJsonArray{9, -1, 13, 2}}, {"grid", false}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        auto* doc = qobject_cast<LayoutDoc*>(app->DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        const QSize view = doc->view()->size();
        r = call("export_image", {{"path", "sample.gds"}, {"save_as", path("view.png")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(json(r).value("pixels").toArray(), QJsonArray({view.width() * 2, view.height() * 2}));
        QCOMPARE(json(r).value("cell").toString(), QString("TOP"));
        QCOMPARE(QImage(path("view.png")).size(), view * 2);
        // At its own pixels, the view's own picture.
        r = call("export_image", {{"path", "sample.gds"}, {"save_as", path("view1.png")}, {"scale", 1}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QImage written = QImage(path("view1.png")).convertToFormat(QImage::Format_ARGB32);
        const QImage shown = doc->view()->picture().convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(written.size(), shown.size());
        int differ = 0, inked = 0;
        const QRgb canvas = doc->view()->colors().background.rgb();
        for (int y = 0; y < written.height(); ++y)
            for (int x = 0; x < written.width(); ++x) {
                const QRgb a = written.pixel(x, y), b = shown.pixel(x, y);
                if (std::abs(qRed(a) - qRed(b)) > 8 || std::abs(qGreen(a) - qGreen(b)) > 8 || std::abs(qBlue(a) - qBlue(b)) > 8) ++differ;
                if ((a & 0xffffff) != (canvas & 0xffffff)) ++inked;
            }
        QVERIFY2(differ <= written.width() * written.height() / 100, qPrintable(QString::number(differ)));
        QVERIFY2(inked > written.width() * written.height() / 50, "the layout not drawn");
        // An SVG and a PDF: vectors.
        r = call("export_image", {{"path", "sample.gds"}, {"save_as", path("view.svg")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(read(path("view.svg")).contains("<svg"));
        QCOMPARE(json(r).value("size").toArray(), QJsonArray({view.width(), view.height()}));
        r = call("export_image", {{"path", "sample.gds"}, {"save_as", path("view.pdf")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QVERIFY(read(path("view.pdf")).startsWith("%PDF"));
        // Transparent: the canvas left out; a JPEG cannot be, and says so.
        r = call("export_image", {{"path", "sample.gds"}, {"save_as", path("clear.png")}, {"transparent", true}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(qAlpha(QImage(path("clear.png")).pixel(1, 1)), 0);
        r = call("export_image", {{"path", "sample.gds"}, {"save_as", path("view.jpg")}, {"transparent", true}});
        QVERIFY2(!failed(r) && json(r).value("note").toString().contains("cannot be transparent"), qPrintable(text(r)));
        // What is a schematic's: refused.
        const QJsonObject eps = call("export_image", {{"path", "sample.gds"}, {"save_as", path("view.eps")}});
        QVERIFY2(failed(eps) && text(eps).contains("png, jpeg, bmp, tiff, webp, svg or pdf"), qPrintable(text(eps)));
        QVERIFY(failed(call("export_image", {{"path", "sample.gds"}, {"save_as", path("v.pdf_tex")}, {"format", "pdf_tex"}})));
        QVERIFY(failed(call("export_image", {{"path", "sample.gds"}, {"save_as", path("d.png")}, {"diagram", 1}})));
        QVERIFY(failed(call("export_image", {{"path", "sample.gds"}, {"save_as", path("g.png")}, {"colours", "grayscale"}})));
        QVERIFY(call("show_layout", {{"path", "sample.gds"}, {"grid", true}}).value("isError").toBool() == false);
    }

    // The layouts' fuzzer (QUCS_LAYOUT_FUZZ seconds, QUCS_LAYOUT_FUZZ_SEED):
    // the sample as GDSII and OASIS (compressed or not), a chain of cells
    // thousands deep, cells placing each other and themselves, an array of
    // 65535 by 65535 - each damaged (bytes flipped, cut, repeated, records'
    // lengths and types written over), gzipped at times, read, then drawn,
    // counted, searched and its shapes outlined. The current file in
    // QUCS_FUZZ_OUT/layout-current.*, a slow one (over 10 s) beside it.
    void fuzz()
    {
        const int seconds = qEnvironmentVariableIntValue("QUCS_LAYOUT_FUZZ");
        if (seconds <= 0) QSKIP("set QUCS_LAYOUT_FUZZ to a number of seconds");
        QRandomGenerator rng(quint32(qEnvironmentVariableIntValue("QUCS_LAYOUT_FUZZ_SEED") + 1));
        const QString out = qEnvironmentVariable("QUCS_FUZZ_OUT");
        QTemporaryDir dir;
        struct Seed {
            QByteArray bytes;
            bool oasis;
        };
        std::vector<Seed> seeds;
        auto add = [&dir, &seeds](const QString& name, bool oasis) {
            QFile f(dir.filePath(name));
            if (f.open(QIODevice::ReadOnly)) seeds.push_back({f.readAll(), oasis});
        };
        writeSample(dir.filePath("s.gds"), false);
        writeSample(dir.filePath("s.oas"), true, 6);
        writeSample(dir.filePath("s0.oas"), true, 0, true);
        writeSample(dir.filePath("x.gds"), false, 6, true);
        writeHierarchy(dir.filePath("deep.gds"), false, 3000, false, false);
        writeHierarchy(dir.filePath("deep.oas"), true, 3000, false, false);
        writeHierarchy(dir.filePath("cycle.gds"), false, 3, true, false);
        writeHierarchy(dir.filePath("cycle.oas"), true, 3, true, false);
        writeHierarchy(dir.filePath("array.gds"), false, 2, false, true);
        for (const char* n : {"s.gds", "x.gds", "deep.gds", "cycle.gds", "array.gds"}) add(QLatin1String(n), false);
        for (const char* n : {"s.oas", "s0.oas", "deep.oas", "cycle.oas"}) add(QLatin1String(n), true);
        QVERIFY(!seeds.empty());
        QElapsedTimer timer;
        timer.start();
        long files = 0, read_ = 0, slow = 0;
        QImage image(256, 256, QImage::Format_ARGB32_Premultiplied);
        while (timer.elapsed() < seconds * 1000LL) {
            const Seed& seed = seeds[rng.bounded(quint32(seeds.size()))];
            QByteArray bytes = seed.bytes;
            const int damages = rng.bounded(5u) == 0 ? 0 : int(1 + rng.bounded(5u));
            for (int k = 0; k < damages && !bytes.isEmpty(); ++k) {
                const qsizetype at = qsizetype(rng.bounded(quint32(bytes.size())));
                switch (rng.bounded(6u)) {
                case 0: bytes[at] = char(rng.bounded(256u)); break;
                case 1: bytes.truncate(at); break;
                case 2: bytes.insert(at, bytes.mid(at, qsizetype(rng.bounded(300u)))); break;
                case 3: bytes.remove(at, qsizetype(rng.bounded(64u))); break;
                case 4: {
                    // A record's length or type (GDSII's 16-bit words; OASIS's bytes).
                    static const quint16 words[] = {0, 1, 2, 3, 4, 0x7fff, 0x8000, 0xffff, 0x0600, 0x0800, 0x0a00, 0x1203, 0x0d02, 0x1a01};
                    const quint16 w = words[rng.bounded(quint32(std::size(words)))];
                    const qsizetype even = at & ~qsizetype(1);
                    if (even + 2 <= bytes.size()) {
                        bytes[even] = char(w >> 8);
                        bytes[even + 1] = char(w & 0xff);
                    }
                    break;
                }
                default: bytes.insert(at, QByteArray(qsizetype(1 + rng.bounded(16u)), char(rng.bounded(256u)))); break;
                }
            }
            const bool zipped = !seed.oasis && rng.bounded(8u) == 0;
            const QString ext = seed.oasis ? QStringLiteral("oas") : QStringLiteral("gds");
            const QString path = dir.filePath(QStringLiteral("current.") + ext);
            {
                QFile f(path);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write(bytes);
            }
            QString file = path;
            if (zipped) {
                file = path + QStringLiteral(".gz");
                QFile::remove(file);
                QVERIFY(gzip(path, file));
            }
            if (!out.isEmpty()) {
                QFile::remove(QDir(out).filePath(QStringLiteral("layout-current.") + ext));
                QFile::copy(file, QDir(out).filePath(QStringLiteral("layout-current.") + ext + (zipped ? QStringLiteral(".gz") : QString())));
            }
            ++files;
            QElapsedTimer one;
            one.start();
            QString error;
            const std::shared_ptr<Layout> layout = read(file, &error);
            if (layout) {
                ++read_;
                const QVector<LayerStyle> styles = stylesFor(*layout, {});
                for (int pass = 0; pass < 3 && !layout->cells.isEmpty(); ++pass) {
                    const int cell = pass == 0 ? layout->mainCell() : int(rng.bounded(quint32(layout->cells.size())));
                    if (cell < 0) continue;
                    RenderOptions o;
                    o.cell = cell;
                    o.depth = rng.bounded(3u) == 0 ? int(rng.bounded(4u)) : 1 << 20;
                    o.device = QRectF(0, 0, 256, 256);
                    o.toDevice = viewTransform(QPointF(rng.generateDouble() * 10, rng.generateDouble() * 10), std::pow(10.0, rng.generateDouble() * 8 - 3), o.device);
                    o.styles = &styles;
                    o.budget = 200000;
                    image.fill(Qt::white);
                    QPainter painter(&image);
                    (void)render(painter, *layout, o);
                    painter.end();
                    (void)countDrawn(*layout, o);
                    Query q;
                    q.cell = cell;
                    q.depth = o.depth;
                    if (rng.bounded(2u)) q.region = QRectF(-5, -5, 20, 20);
                    long found = 0;
                    search(*layout, q, [&](const Found& f) {
                        if (found < 50) {
                            (void)outlineOf(*layout, f);
                            (void)spineOf(*layout, f);
                            (void)hits(*layout, f, QPointF(1, 1), 0.1);
                            (void)kindName(f);
                        }
                        return ++found < 20000;
                    });
                    (void)snap(*layout, q, QPointF(0.3, 0.3), 1);
                }
            }
            if (one.elapsed() > 10000) {
                ++slow;
                if (!out.isEmpty()) QFile::copy(file, QDir(out).filePath(QStringLiteral("layout-slow-%1.").arg(slow) + ext + (zipped ? QStringLiteral(".gz") : QString())));
                qWarning("slow: %lld ms", qint64(one.elapsed()));
            }
        }
        qInfo("layouts: %ld files, %ld read, %ld slow, in %d s", files, read_, slow, seconds);
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestLayoutDoc test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_layout_doc.moc"
