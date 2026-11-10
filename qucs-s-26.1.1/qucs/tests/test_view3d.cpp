/*
 * test_view3d.cpp - Multiphysics 3D's view: drawn by the CPU, and by the
 *                   GPU - the same picture, a turn of the camera no buffers
 *                   made again
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The GPU's tests need a window system's GPU (Qt's RHI): skipped on the
 * offscreen platform the suite runs on (QT_QPA_PLATFORM=cocoa, xcb or windows
 * runs them, a window shown a moment). QUCS_VIEW_SAVE=<folder>: the GPU's
 * and the CPU's pictures there. QUCS_VIEW_BENCH=1: a frame's time on each.
 */
#include "view3d.h"
#include "gpurendering.h"

#include "isolated_settings.h"

#include <QActionEvent>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QMenu>
#include <QRhiWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <cmath>

using qucs_s::fem::Box3;
using qucs_s::fem::Vec3;

namespace {

QRhiWidget* canvasOf(View3D& view) { return view.findChild<QRhiWidget*>(QStringLiteral("view3dCanvas")); }

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

/// The same, of the pixels where \a a is smooth - its 5 by 5 round each
/// within 12 of it in each channel: the faces and the field, not the edges
/// and lines, whose antialiasing the two draw differently.
double differingInside(const QImage& a, const QImage& b, int by)
{
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32).scaled(x.size());
    qint64 off = 0, inside = 0;
    for (int j = 2; j + 2 < x.height(); ++j)
        for (int i = 2; i + 2 < x.width(); ++i) {
            const QRgb p = x.pixel(i, j);
            bool smooth = true;
            for (int dj = -2; dj <= 2 && smooth; ++dj)
                for (int di = -2; di <= 2 && smooth; ++di) {
                    const QRgb r = x.pixel(i + di, j + dj);
                    smooth = std::abs(qRed(p) - qRed(r)) <= 12 && std::abs(qGreen(p) - qGreen(r)) <= 12 && std::abs(qBlue(p) - qBlue(r)) <= 12;
                }
            if (!smooth) continue;
            ++inside;
            const QRgb q = y.pixel(i, j);
            if (std::abs(qRed(p) - qRed(q)) > by || std::abs(qGreen(p) - qGreen(q)) > by || std::abs(qBlue(p) - qBlue(q)) > by) ++off;
        }
    return inside > 0 ? double(off) / double(inside) : 1;
}

/// A box's six faces (two triangles each, a colour a face), its twelve
/// edges, its corners' dots; on its top a field over an n by n grid (each
/// corner its colour, blended); \a cells of the grid its mesh's lines.
View3D::Scene boxScene(int n)
{
    View3D::Scene s;
    const double x0 = 0, x1 = 2, y0 = 0, y1 = 1, z0 = 0, z1 = 0.6;
    const Vec3 c[8] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    const QColor colors[6] = {QColor(180, 180, 190), QColor(120, 160, 220), QColor(220, 140, 90), QColor(140, 200, 120), QColor(200, 200, 90), QColor(170, 120, 200)};
    for (int f = 0; f < 6; ++f) {
        View3D::Triangle a, b;
        a.p = {c[faces[f][0]], c[faces[f][1]], c[faces[f][2]]};
        b.p = {c[faces[f][0]], c[faces[f][2]], c[faces[f][3]]};
        a.color = b.color = colors[f];
        a.id = b.id = f;
        s.triangles.push_back(a);
        s.triangles.push_back(b);
    }
    // A field just above the top: x a hue, y its brightness.
    auto field = [](const Vec3& p) { return QColor::fromHsvF(std::fmod(0.7 * p.x / 2, 1.0), 0.8, 0.5 + 0.5 * p.y); };
    const double z = z1 + 0.05;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const Vec3 p00{x1 * i / n, y1 * j / n, z}, p10{x1 * (i + 1) / n, y1 * j / n, z}, p11{x1 * (i + 1) / n, y1 * (j + 1) / n, z},
                p01{x1 * i / n, y1 * (j + 1) / n, z};
            for (const auto& tri : {std::array<Vec3, 3>{p00, p10, p11}, std::array<Vec3, 3>{p00, p11, p01}}) {
                View3D::Triangle t;
                t.p = tri;
                t.shaded = true;
                t.colors = {field(tri[0]), field(tri[1]), field(tri[2])};
                t.id = 100;
                s.triangles.push_back(t);
            }
            View3D::Line l;
            l.a = p00;
            l.b = p10;
            l.color = QColor(30, 30, 30);
            s.lines.push_back(l);
        }
    const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) {
        View3D::Line l;
        l.a = c[e[0]];
        l.b = c[e[1]];
        l.color = Qt::black;
        l.width = 2;
        s.lines.push_back(l);
    }
    for (const Vec3& p : c) {
        View3D::Dot d;
        d.p = p;
        d.color = QColor(214, 48, 49);
        d.radius = 3;
        s.dots.push_back(d);
    }
    return s;
}

} // namespace

class TestView3D : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath(QStringLiteral("settings")));
    }

    // Its setting: a key of its own (Settings… > Viewers, Multiphysics 3D >
    // Settings…, the view's menu), the CPU's unless chosen; a view told when
    // it changes. Without a GPU (the offscreen platform) the CPU draws,
    // asked for one or not.
    void itsSetting()
    {
        using qucs_s::gpu::Viewer;
        QCOMPARE(qucs_s::gpu::settingKey(Viewer::Multiphysics3D), QStringLiteral("Viewers/multiphysics3dGpu"));
        QCOMPARE(qucs_s::gpu::viewerName(Viewer::Multiphysics3D), QStringLiteral("Multiphysics 3D view"));
        QVERIFY(!qucs_s::gpu::wanted(Viewer::Multiphysics3D));
        if (View3D::gpuAvailable()) QSKIP("a GPU here: theGpuDrawsAsTheCpu");
        View3D view;
        view.setScene(boxScene(4), true);
        qucs_s::gpu::setWanted(Viewer::Multiphysics3D, true);
        QVERIFY(qucs_s::gpu::wanted(Viewer::Multiphysics3D));
        QVERIFY(!view.gpu());
        QVERIFY(canvasOf(view) == nullptr);
        QCOMPARE(view.gpuFrames(), 0);
        View3D other;
        QVERIFY(!other.gpu());
        qucs_s::gpu::setWanted(Viewer::Multiphysics3D, false);
        QVERIFY(!view.gpu());
        // (The CPU's picture: the box drawn, not the background alone.)
        view.resize(320, 240);
        view.fit();
        const QImage cpu = view.picture();
        const QColor background = view.palette().color(QPalette::Base);
        QVERIFY(cpu.pixelColor(160, 120) != background);
    }

    // QUCS_GPU=0: the CPU draws whatever asks for the GPU - the setting, the
    // view's own menu -; the menu's item, "Draw with the GPU", unchecked and
    // off.
    void qucsGpu0KeepsTheCpu()
    {
        qputenv("QUCS_GPU", "0");
        struct Unset {
            ~Unset()
            {
                qunsetenv("QUCS_GPU");
                qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Multiphysics3D, false);
            }
        } unset;
        QVERIFY(!View3D::gpuAvailable());
        View3D view;
        view.setScene(boxScene(4), true);
        view.resize(300, 200);
        view.show();
        (void)QTest::qWaitForWindowExposed(&view, 1000);
        view.setGpu(true);
        QVERIFY(!view.gpu());
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Multiphysics3D, true);
        QVERIFY(!view.gpu());
        QVERIFY(canvasOf(view) == nullptr);
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

    // The GPU draws what the CPU draws: a box's faces, a field's blended
    // colours over its top, the mesh's and the box's edges, its corners'
    // dots, the axes and the cut's words over them - from the corner, the
    // top and the front, and cut half away - their pictures alike but for
    // the edges' and lines' antialiasing (the GPU's 4 samples a pixel, the
    // CPU's none: under 4 % of the pixels more than 40 apart in a channel,
    // the most where the mesh's lines fill the view), and where the CPU's is
    // smooth - the faces, their light, the field's colours - the same to
    // 0.2 % of those pixels; turned, panned and zoomed, the GPU's frames and no
    // buffers made again; a new scene its buffers once; told so, the CPU
    // again, and a view made after drawn by it.
    void theGpuDrawsAsTheCpu()
    {
        if (!View3D::gpuAvailable()) QSKIP("no GPU backend here (the offscreen platform)");
        struct Off {
            ~Off() { qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Multiphysics3D, false); }
        } off;
        View3D view;
        view.setGpu(true);
        QVERIFY(view.gpu());
        view.resize(720, 540);
        view.setScene(boxScene(24), true);
        view.setVisible(true);
        (void)QTest::qWaitForWindowExposed(&view, 1000);
        QRhiWidget* canvas = canvasOf(view);
        QVERIFY(canvas != nullptr);
        QCOMPARE(canvas->geometry(), view.rect());
        QVERIFY(!canvas->grabFramebuffer().isNull());
        auto compare = [&](const QString& what) {
            const QImage gpu = canvas->grabFramebuffer();
            const QImage cpu = view.picture(view.devicePixelRatioF());
            const double apart = differing(cpu, gpu, 40), inside = differingInside(cpu, gpu, 24);
            qInfo("%s: %.2f %% of the pixels apart, %.3f %% of the smooth ones", qPrintable(what), 100 * apart, 100 * inside);
            if (qEnvironmentVariableIsSet("QUCS_VIEW_SAVE")) {
                gpu.save(QStringLiteral("%1/view3d-%2-gpu.png").arg(qEnvironmentVariable("QUCS_VIEW_SAVE"), what));
                cpu.save(QStringLiteral("%1/view3d-%2-cpu.png").arg(qEnvironmentVariable("QUCS_VIEW_SAVE"), what));
            }
            return inside < 0.002 ? apart : 1.0;
        };
        QVERIFY(compare(QStringLiteral("iso")) < 0.04);
        view.viewFrom(View3D::Preset::Top);
        QVERIFY(compare(QStringLiteral("top")) < 0.04);
        view.viewFrom(View3D::Preset::Front);
        QVERIFY(compare(QStringLiteral("front")) < 0.04);
        view.viewFrom(View3D::Preset::Iso);
        view.setClip(0, 0.5);
        QVERIFY(compare(QStringLiteral("cut")) < 0.04);
        view.setClip(-1, 0.5);
        // Turned, panned, zoomed: frames, no buffers made again.
        const int builds = view.gpuBuilds(), frames = view.gpuFrames();
        QCOMPARE(builds, 1);
        for (int k = 0; k < 8; ++k) {
            view.setDirection(-55 + 10 * k, 30 + 3 * k);
            view.zoomBy(1.05);
            QVERIFY(!canvas->grabFramebuffer().isNull());
        }
        QCOMPARE(view.gpuBuilds(), builds);
        QVERIFY(view.gpuFrames() > frames);
        QVERIFY(compare(QStringLiteral("turned")) < 0.04);
        // A new scene: its buffers made once.
        view.setScene(boxScene(8));
        QVERIFY(!canvas->grabFramebuffer().isNull());
        QCOMPARE(view.gpuBuilds(), builds + 1);
        QVERIFY(compare(QStringLiteral("coarser")) < 0.04);
        if (qEnvironmentVariableIsSet("QUCS_VIEW_BENCH")) {
            view.setScene(boxScene(400), true);
            (void)canvas->grabFramebuffer();
            QElapsedTimer t;
            t.start();
            for (int k = 0; k < 10; ++k) {
                view.setDirection(-55 + 3 * k, 30);
                (void)canvas->grabFramebuffer();
            }
            const double gpuMs = t.elapsed() / 10.0;
            t.restart();
            for (int k = 0; k < 10; ++k) {
                view.setDirection(-55 + 3 * k, 30);
                (void)view.picture(view.devicePixelRatioF());
            }
            qInfo("%zu triangles: a frame %.1f ms on the GPU (grabbed), %.1f ms on the CPU", view.scene().triangles.size(), gpuMs, t.elapsed() / 10.0);
        }
        // Off: the CPU's, a view made after too.
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Multiphysics3D, false);
        QVERIFY(!view.gpu());
        QVERIFY(canvasOf(view) == nullptr);
        View3D other;
        QVERIFY(!other.gpu());
    }
};

QTEST_MAIN(TestView3D)
#include "test_view3d.moc"
