/*
 * test_multiphysics_view.cpp - the multiphysics Graphics view's drawing:
 *                              by the CPU (its color surface's raster in
 *                              bands, one a core), and by the GPU - the
 *                              same picture, and a zoom no raster
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The GPU's tests need a window system's GPU (Qt's RHI): skipped on the offscreen
 * platform the suite runs on (QT_QPA_PLATFORM=cocoa, xcb or windows runs
 * them, a window shown a moment). QUCS_VIEW_BENCH=1: a frame's time;
 * QUCS_VIEW_SAVE=<folder>: the GPU's and the CPU's pictures there.
 */
#include "multiphysicsview.h"
#include "multiphysicssettings.h"
#include "gpurendering.h"

#include "isolated_settings.h"
#include "main.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QMenu>
#include <QActionEvent>
#include <QTimer>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QRhiWidget>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <functional>

using namespace qucs_s::fem;

namespace {

/// A square of n by n cells, two triangles each, 1 a side.
std::shared_ptr<Mesh> gridMesh(int n)
{
    auto m = std::make_shared<Mesh>();
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) m->nodes.emplace_back(double(i) / n, double(j) / n);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
            m->triangles.push_back({a, b, d});
            m->triangles.push_back({a, d, c});
            m->domain.push_back(0);
            m->domain.push_back(0);
        }
    return m;
}

/// Its surface: \a f at each corner, from -1 to 1.
std::shared_ptr<PlotScene> sceneOf(const Mesh& m, const std::function<double(QPointF)>& f, bool contours)
{
    auto s = std::make_shared<PlotScene>();
    s->surface = true;
    s->colors = QStringLiteral("viridis");
    for (const auto& t : m.triangles)
        for (int k : t) {
            const QPointF p = m.nodes[std::size_t(k)];
            s->values.points.push_back(p);
            s->values.values.push_back(f(p));
        }
    s->values.min = s->min = -1;
    s->values.max = s->max = 1;
    if (contours) s->contours.push_back({contourData(s->values, 9), QStringLiteral("single"), Qt::white, -1, 1});
    return s;
}

/// The square as a topology: one domain, its four sides.
std::shared_ptr<Topology> squareTopology()
{
    auto t = std::make_shared<Topology>();
    t->vertices = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    Topology::Domain d;
    d.triangles = {{0, 1, 2}, {0, 2, 3}};
    t->domains.push_back(d);
    for (int k = 0; k < 4; ++k) {
        Topology::Segment s;
        s.v0 = k;
        s.v1 = (k + 1) % 4;
        t->segments.push_back(s);
        Topology::Boundary b;
        b.segments = {k};
        b.reversed = {false};
        b.start = k;
        b.end = (k + 1) % 4;
        b.up = 0;
        t->boundaries.push_back(b);
    }
    t->bounds = QRectF(0, 0, 1, 1);
    return t;
}

QRhiWidget* canvasOf(GraphicsView& view) { return view.findChild<QRhiWidget*>(QStringLiteral("multiphysicsCanvas")); }

/// Of two pictures, the share of pixels apart by more than \a by in a channel.
double differing(const QImage& a, const QImage& b, int by)
{
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32).scaled(x.size());
    qint64 off = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            const QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            if (std::abs(qRed(p) - qRed(q)) > by || std::abs(qGreen(p) - qGreen(q)) > by || std::abs(qBlue(p) - qBlue(q)) > by) ++off;
        }
    return double(off) / (double(x.width()) * x.height());
}

} // namespace

class TestMultiphysicsView : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath(QStringLiteral("settings")));
    }

    // The color surface's raster, in bands of rows on the cores: a field
    // linear in x (v = 2x - 1), every pixel inside the square in the color
    // of its x - within a step of the table -, on every row, across every
    // band's edge; drawn again at another size, a point's color the same.
    void theSurfaceRasterInBands()
    {
        auto mesh = gridMesh(60);   // 7200 triangles: in bands
        GraphicsView view;
        view.setGpu(false);
        view.resize(700, 517);
        view.setMesh(mesh);
        view.setPlot(sceneOf(*mesh, [](QPointF p) { return 2 * p.x() - 1; }, false));
        view.setShow(GraphicsView::Show::Results);
        view.setView(QPointF(0.5, 0.5), 480);
        const QImage picture = view.picture();
        QCOMPARE(view.rasters(), 1);
        const std::vector<QRgb>& table = colorTable(QStringLiteral("viridis"));
        const double dpr = picture.devicePixelRatio();
        auto near = [&table](QRgb got, int i) {
            for (int k = std::max(0, i - 1); k <= std::min(255, i + 1); ++k) {
                const QRgb t = table[std::size_t(k)];
                if (std::abs(qRed(got) - qRed(t)) <= 1 && std::abs(qGreen(got) - qGreen(t)) <= 1 && std::abs(qBlue(got) - qBlue(t)) <= 1) return true;
            }
            return false;
        };
        int checked = 0;
        for (int py = 0; py < picture.height(); ++py)
            for (int px = 3; px < picture.width(); px += 17) {
                // (The raster's pixel's center, in the model.)
                const QPointF at = view.toModel(QPointF((px + 0.5) / dpr, (py + 0.5) / dpr));
                if (at.x() < 0.01 || at.x() > 0.99 || at.y() < 0.01 || at.y() > 0.99) continue;
                const int i = std::clamp(int(at.x() * 255.0 + 0.5), 0, 255);
                QVERIFY2(near(picture.pixel(px, py), i), qPrintable(QStringLiteral("(%1, %2): %3, the table's %4")
                                                                         .arg(px)
                                                                         .arg(py)
                                                                         .arg(picture.pixel(px, py), 8, 16)
                                                                         .arg(table[std::size_t(i)], 8, 16)));
                ++checked;
            }
        QVERIFY2(checked > 9000, qPrintable(QString::number(checked)));
        // Another size, the same scale and middle: drawn again, the point's color the same.
        view.resize(640, 480);
        const QImage again = view.picture();
        QCOMPARE(view.rasters(), 2);
        const QPointF a = view.toPixel(QPointF(0.3, 0.7)) * dpr;
        const QPointF b = (QPointF(0.3 - 0.5, -(0.7 - 0.5)) * 480 + QPointF(350, 258.5)) * dpr;
        QCOMPARE(again.pixel(a.toPoint()), picture.pixel(b.toPoint()));
    }

    // Without a GPU (the offscreen platform) the CPU draws, asked for one
    // or not; a view made after the same.
    void theGpuWhenThereIsNone()
    {
        if (GraphicsView::gpuAvailable()) QSKIP("a GPU here");
        GraphicsView view;
        view.setGpu(true);
        QVERIFY(!view.gpu());
        QVERIFY(canvasOf(view) == nullptr);
        QCOMPARE(view.gpuFrames(), 0);
        view.setGpu(false);
        QVERIFY(!view.gpu());
        GraphicsView other;
        QVERIFY(!other.gpu());
    }

    // QUCS_GPU=0: the CPU draws the Graphics view whatever asks for the
    // GPU - the setting, Multiphysics > Settings…, the view's own menu
    // (each put it on the GPU: the variable read by the view's constructor
    // alone); the menu's item, "Draw with the GPU", unchecked and off.
    void qucsGpu0KeepsTheCpu()
    {
        qputenv("QUCS_GPU", "0");
        struct Unset {
            ~Unset()
            {
                qunsetenv("QUCS_GPU");
                qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Multiphysics, false);
            }
        } unset;
        QVERIFY(!GraphicsView::gpuAvailable());
        GraphicsView view;
        view.setMesh(gridMesh(4));
        view.resize(300, 200);
        view.show();
        (void)QTest::qWaitForWindowExposed(&view, 1000);
        view.setGpu(true);
        QVERIFY(!view.gpu());
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Multiphysics, true);
        QVERIFY(!view.gpu());
        {
            MultiphysicsSettingsDialog d;
            d.gpuBox()->setChecked(true);
            QVERIFY2(d.gpuStatus()->text().contains(QLatin1String("QUCS_GPU=0")), qPrintable(d.gpuStatus()->text()));
            d.apply();
        }
        QVERIFY(qucs_s::gpu::wanted(qucs_s::gpu::Viewer::Multiphysics));
        QVERIFY(!view.gpu());
        QVERIFY(canvasOf(view) == nullptr);
        GraphicsView other;
        QVERIFY(!other.gpu());
        // The menu: its item read as it is set (on macOS a native menu, its
        // exec perhaps over at once), the menu closed once open.
        bool seen = false;
        struct Watch : QObject {
            bool* seen;
            explicit Watch(bool* s) : seen(s) {}
            bool eventFilter(QObject* o, QEvent* e) override
            {
                if (e->type() == QEvent::ChildAdded) {
                    static_cast<QChildEvent*>(e)->child()->installEventFilter(this);
                } else if (e->type() == QEvent::ActionAdded || e->type() == QEvent::ActionChanged) {
                    const QAction* a = static_cast<QActionEvent*>(e)->action();
                    if (a->text() == QLatin1String("Draw with the GPU")) *seen = a->isCheckable() && !a->isChecked() && !a->isEnabled();
                    QTimer::singleShot(0, o, [o] {
                        if (auto* menu = qobject_cast<QMenu*>(o)) menu->close();
                    });
                }
                return false;
            }
        } watch(&seen);
        view.installEventFilter(&watch);
        QContextMenuEvent e(QContextMenuEvent::Mouse, QPoint(20, 20), view.mapToGlobal(QPoint(20, 20)));
        QCoreApplication::sendEvent(&view, &e);
        view.removeEventFilter(&watch);
        QVERIFY(seen);
        QVERIFY(!view.gpu());
    }

    // A wheel zooms or pans as Application Settings' Mouse wheel says, Ctrl
    // the other way; a trackpad's two fingers pan, and zoom with Ctrl;
    // Shift pans sideways - as the schematic and the layout viewer. (Every
    // wheel and scroll zoomed: a trackpad's scroll could not pan.)
    void aWheelZoomsOrPansAsTheSettingsSay()
    {
        const bool kept = QucsSettings.WheelZooms;
        GraphicsView view;
        view.setGpu(false);
        view.resize(400, 300);
        view.setMesh(gridMesh(4));
        const QPointF middle(200, 150);
        const auto send = [&view, middle](QPoint angle, QPoint pixels, Qt::KeyboardModifiers modifiers, Qt::ScrollPhase phase) {
            view.setView(QPointF(0.5, 0.5), 200);
            QWheelEvent w(middle, view.mapToGlobal(middle), pixels, angle, Qt::NoButton, modifiers, phase, false);
            QCoreApplication::sendEvent(&view, &w);
        };
        const auto zoomed = [&view] { return std::abs(view.scale() - 200) > 1e-9; };
        const auto moved = [&view] { return view.center() - QPointF(0.5, 0.5); };
        for (const bool wheelZooms : {true, false}) {
            QucsSettings.WheelZooms = wheelZooms;
            // A mouse wheel.
            send(QPoint(0, 120), QPoint(), Qt::NoModifier, Qt::NoScrollPhase);
            QCOMPARE(zoomed(), wheelZooms);
            if (wheelZooms) {
                QCOMPARE(view.scale(), 240.0);
                QCOMPARE(view.center(), QPointF(0.5, 0.5));   // (the point under the pointer kept)
            } else {
                QCOMPARE(moved(), QPointF(0, 60 / 200.0));   // half a notch's angle, in pixels
            }
            send(QPoint(0, 120), QPoint(), Qt::ControlModifier, Qt::NoScrollPhase);
            QCOMPARE(zoomed(), !wheelZooms);
            // Shift: sideways.
            send(QPoint(0, 120), QPoint(), Qt::ShiftModifier, Qt::NoScrollPhase);
            QVERIFY(!zoomed());
            QCOMPARE(moved(), QPointF(-60 / 200.0, 0));
            // A trackpad: its pixels; with Ctrl, a zoom.
            send(QPoint(0, 90), QPoint(0, 30), Qt::NoModifier, Qt::ScrollUpdate);
            QVERIFY(!zoomed());
            QCOMPARE(moved(), QPointF(0, 30 / 200.0));
            send(QPoint(-24, 0), QPoint(-8, 0), Qt::NoModifier, Qt::ScrollUpdate);
            QCOMPARE(moved(), QPointF(8 / 200.0, 0));
            send(QPoint(0, 90), QPoint(0, 30), Qt::ControlModifier, Qt::ScrollUpdate);
            QVERIFY(zoomed());
        }
        QucsSettings.WheelZooms = kept;
    }

    // The GPU draws what the CPU draws: the geometry (its domain chosen),
    // the mesh, a color surface with its contours and the mesh over it, the
    // view grabbed as a window is - their pictures alike but for the lines'
    // and letters' antialiasing (under 3 % of the pixels more than 40 apart
    // in a channel); a zoom the GPU's frames, no raster made; told so, the
    // setting kept - a view made after drawn by the CPU -, and on again.
    // A plot with no surface - rays, contours alone - draws the geometry's
    // boundaries, then the boundaries again over the results with other
    // strokes: their buffers made again in the frame, the first ones kept
    // till it is drawn. (Deleted under the frame's first step, Metal wrote
    // a freed buffer's update: a crash at random, at once on the sanitized
    // build - test_multiphysics_doc's raysDrawn on the display.)
    void theBoundariesTwiceAFrame()
    {
        if (!GraphicsView::gpuAvailable()) QSKIP("no GPU backend here (the offscreen platform)");
        GraphicsView view;
        view.setGpu(true);
        QVERIFY(view.gpu());
        view.resize(400, 300);
        view.setTopology(squareTopology());
        auto plot = std::make_shared<PlotScene>();
        plot->surface = false;
        view.setPlot(plot);
        view.setShow(GraphicsView::Show::Results);
        view.setVisible(true);
        (void)QTest::qWaitForWindowExposed(&view, 1000);
        QRhiWidget* canvas = canvasOf(view);
        QVERIFY(canvas != nullptr);
        for (int k = 0; k < 6; ++k) {
            view.setView(QPointF(0.5 + 0.01 * k, 0.5), 200);
            QVERIFY(!canvas->grabFramebuffer().isNull());
        }
        const double off = differing(view.picture(), canvas->grabFramebuffer(), 40);
        QVERIFY2(off < 0.03, qPrintable(QString::number(100 * off)));
    }

    void theGpuDrawsAsTheCpu()
    {
        if (!GraphicsView::gpuAvailable()) QSKIP("no GPU backend here (the offscreen platform)");
        auto mesh = gridMesh(40);
        GraphicsView view;
        view.setGpu(true);
        QVERIFY(view.gpu());
        view.resize(720, 540);
        view.setTopology(squareTopology());
        view.setMesh(mesh);
        // (Not show(): the view's own, what it shows.)
        view.setVisible(true);
        // (Exposed or not - a display asleep - the canvas draws into its own
        // framebuffer.)
        (void)QTest::qWaitForWindowExposed(&view, 1000);
        QRhiWidget* canvas = canvasOf(view);
        QVERIFY(canvas != nullptr);
        QCOMPARE(canvas->geometry(), view.rect());
        QVERIFY(!canvas->grabFramebuffer().isNull());
        auto compare = [&](const QString& what) {
            view.fit();
            const QImage gpu = canvas->grabFramebuffer();
            const QImage cpu = view.picture();
            const double off = differing(cpu, gpu, 40);
            qInfo("%s: %.2f %% of the pixels apart", qPrintable(what), 100 * off);
            if (qEnvironmentVariableIsSet("QUCS_VIEW_SAVE")) {
                gpu.save(QStringLiteral("%1/%2-gpu.png").arg(qEnvironmentVariable("QUCS_VIEW_SAVE"), what));
                cpu.save(QStringLiteral("%1/%2-cpu.png").arg(qEnvironmentVariable("QUCS_VIEW_SAVE"), what));
            }
            return off;
        };
        view.setShow(GraphicsView::Show::Geometry);
        view.setHighlight(Level::Domain, {0});
        // (On Metal the geometry's picture the CPU's, pixel for pixel - its
        // grid, words and fills; elsewhere the edges' antialiasing may
        // differ.)
        const bool metal = qucs_s::gpu::backendName() == QLatin1String("Metal");
        QVERIFY(compare(QStringLiteral("geometry")) < (metal ? 0.001 : 0.03));
        // Grabbed as a window is - QWidget::grab of the view, Claude's
        // screenshots -: the same picture, its background and words with it
        // (a painter on the canvas itself was the grab's, under the
        // framebuffer's picture).
        {
            const QImage grabbed = view.grab().toImage();
            const double off = differing(view.picture(), grabbed, 40);
            qInfo("grabbed with the view: %.2f %% of the pixels apart", 100 * off);
            QVERIFY(off < 0.03);
            // Grabbed again and again as it moves, the window not painting
            // between: each grab its own frame, none of those before it
            // showing through.
            for (int k = 0; k < 3; ++k) {
                view.zoomBy(1.15, QPointF(200 + 40 * k, 150));
                (void)view.grab();
            }
            view.fit();
            const double again = differing(view.picture(), view.grab().toImage(), 40);
            qInfo("grabbed after three moves: %.2f %% of the pixels apart", 100 * again);
            QVERIFY(again < 0.03);
        }
        // A boundary chosen: its color the CPU's (its strokes uploaded again).
        view.setHighlight(Level::Boundary, {1});
        QVERIFY(compare(QStringLiteral("boundary")) < 0.03);
        {
            const QPointF at = view.toPixel(QPointF(1, 0.5)) * view.devicePixelRatioF();
            const QColor gpu = canvas->grabFramebuffer().pixelColor(at.toPoint()), cpu = view.picture().pixelColor(at.toPoint());
            qInfo("the chosen boundary: %s (GPU), %s (CPU)", qPrintable(gpu.name()), qPrintable(cpu.name()));
            QVERIFY(std::abs(gpu.red() - cpu.red()) + std::abs(gpu.green() - cpu.green()) + std::abs(gpu.blue() - cpu.blue()) < 60);
        }
        view.setHighlight(Level::None, {});
        view.setShow(GraphicsView::Show::Mesh);
        QVERIFY(compare(QStringLiteral("mesh")) < 0.03);
        auto scene = sceneOf(*mesh, [](QPointF p) { return std::sin(5 * p.x()) * std::cos(4 * p.y()); }, true);
        scene->mesh = true;
        view.setPlot(scene);
        view.setShow(GraphicsView::Show::Results);
        QVERIFY(compare(QStringLiteral("surface")) < 0.03);
        // A zoom: the GPU's frames, no raster.
        const int rasters = view.rasters(), frames = view.gpuFrames();
        for (int k = 0; k < 10; ++k) {
            view.zoomBy(1.1, QPointF(300, 200));
            (void)canvas->grabFramebuffer();
        }
        QCOMPARE(view.rasters(), rasters);
        QVERIFY2(view.gpuFrames() >= frames + 10, qPrintable(QString::number(view.gpuFrames() - frames)));
        // Another plot: its buffers made again.
        auto other = sceneOf(*mesh, [](QPointF p) { return std::cos(7 * p.x() * p.y()); }, true);
        other->colors = QStringLiteral("thermal");
        view.setPlot(other);
        QVERIFY(compare(QStringLiteral("another")) < 0.03);
        // Resized: the canvas the view's size.
        view.resize(600, 610);
        QCOMPARE(canvas->geometry(), view.rect());
        QVERIFY(compare(QStringLiteral("resized")) < 0.03);
        // Deep in (a pixel 1e-9 of the square), far from its middle: the
        // mesh's line x = 0.375 where the CPU draws it, to a pixel - the
        // floats' origin moved there. (The middle 4.5 float steps from the
        // line, at 0.125 from the old origin: half a step, 7 pixels, off.)
        view.setShow(GraphicsView::Show::Mesh);
        auto lineAt = [](const QImage& image) {
            const QImage g = image.convertToFormat(QImage::Format_Grayscale8);
            int best = -1, brightest = -1;
            for (int x = 0; x < g.width(); ++x) {
                const int v = qGray(g.pixel(x, g.height() / 2));
                if (v > brightest) brightest = v, best = x;
            }
            return best;
        };
        const double off = 4.5 * std::ldexp(1.0, -27);
        view.setView(QPointF(0.375 + off, 0.6125), 1e9);
        const int gpuLine = lineAt(canvas->grabFramebuffer()), cpuLine = lineAt(view.picture());
        qInfo("deep in: the line at %d (GPU), %d (CPU) of %d", gpuLine, cpuLine, int(view.width() * view.devicePixelRatioF()));
        QVERIFY(std::abs(gpuLine - cpuLine) <= 1);
        QVERIFY(std::abs(cpuLine - int(std::lround((300 - off * 1e9) * view.devicePixelRatioF()))) <= 2);
        // Off: the CPU's; a view made now the CPU's too; on again.
        view.setGpu(false);
        QVERIFY(!view.gpu());
        QVERIFY(canvasOf(view) == nullptr);
        {
            GraphicsView other;
            QVERIFY(!other.gpu());
        }
        view.setGpu(true);
        QVERIFY(view.gpu());
        view.setShow(GraphicsView::Show::Results);
        canvas = canvasOf(view);
        QVERIFY(canvas != nullptr);
        QCOMPARE(canvas->geometry(), view.rect());
        QVERIFY(compare(QStringLiteral("surface-again")) < 0.03);
    }

    // The Multiphysics menu's Settings: every open view drawn as it says on
    // OK - by the CPU off, by the GPU on where there is one (the offscreen
    // platform: by the CPU, said); the setting the views' own menu keeps.
    void theSettingsDrawEveryViewSo()
    {
        GraphicsView a, b;
        for (GraphicsView* v : {&a, &b}) {
            v->setMesh(gridMesh(4));
            v->setShow(GraphicsView::Show::Mesh);
            v->resize(300, 200);
            v->show();
            (void)QTest::qWaitForWindowExposed(v, 1000);   // (as bench(): the GPU canvas a child window)
        }
        {
            MultiphysicsSettingsDialog d;
            d.gpuBox()->setChecked(false);
            QCOMPARE(d.gpuStatus()->text(), GraphicsView::gpuAvailable() ? QStringLiteral("The CPU draws the Graphics view.")
                                                                           : MultiphysicsSettingsDialog::gpuHindrance());
            d.apply();
        }
        QVERIFY(!MultiphysicsSettingsDialog::gpuWanted());
        QVERIFY(!a.gpu() && !b.gpu());
        {
            MultiphysicsSettingsDialog d;
            QVERIFY(!d.gpuBox()->isChecked());
            d.gpuBox()->setChecked(true);
            if (GraphicsView::gpuAvailable()) QCOMPARE(d.gpuStatus()->text(), QStringLiteral("The GPU draws the Graphics view."));
            else QVERIFY2(d.gpuStatus()->text().contains(QLatin1String("No GPU backend here")), qPrintable(d.gpuStatus()->text()));
            d.apply();
        }
        QVERIFY(MultiphysicsSettingsDialog::gpuWanted());
        QCOMPARE(a.gpu(), GraphicsView::gpuAvailable());
        QCOMPARE(b.gpu(), GraphicsView::gpuAvailable());
        qInfo("the GPU %s here", GraphicsView::gpuAvailable() ? "draws" : "cannot draw");
    }

    // (QUCS_VIEW_BENCH=1: a frame's time, zooming, the CPU's and the GPU's.)
    void bench()
    {
        if (!qEnvironmentVariableIsSet("QUCS_VIEW_BENCH")) QSKIP("QUCS_VIEW_BENCH not set");
        for (int n : {100, 300, 600})
            for (bool gpu : {false, true}) {
                if (gpu && !GraphicsView::gpuAvailable()) continue;
                auto mesh = gridMesh(n);
                GraphicsView view;
                view.setGpu(gpu);
                view.resize(1400, 900);
                view.setMesh(mesh);
                auto scene = sceneOf(*mesh, [](QPointF p) { return std::sin(9 * p.x()) * std::cos(7 * p.y()); }, true);
                scene->mesh = true;
                view.setPlot(scene);
                view.setShow(GraphicsView::Show::Results);
                view.fit();
                view.setVisible(true);
                (void)QTest::qWaitForWindowExposed(&view, 1000);
                QRhiWidget* canvas = canvasOf(view);
                if (gpu) (void)canvas->grabFramebuffer();   // (its buffers made)
                QElapsedTimer t;
                t.start();
                const int frames = 20;
                for (int k = 0; k < frames; ++k) {
                    view.zoomBy(k < 10 ? 1.05 : 1 / 1.05);
                    if (gpu) (void)canvas->grabFramebuffer();
                    else view.repaint();
                }
                qInfo("%d triangles, %s, dpr %g: %.1f ms a frame", int(mesh->triangles.size()), gpu ? "GPU" : "CPU", view.devicePixelRatioF(),
                      double(t.elapsed()) / frames);
            }
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestMultiphysicsView test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_multiphysics_view.moc"
