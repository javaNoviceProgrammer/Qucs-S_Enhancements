/*
 * gpurendering.cpp - which viewers the GPU draws
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "gpurendering.h"

#include "settings.h"

#ifdef QUCS_GPU_VIEW
#include "multiphysicsviewgl.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLPaintDevice>
#include <QOpenGLWidget>
#endif

#include <QAbstractScrollArea>
#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QTextEdit>

#include <cmath>
#include <cstring>

namespace qucs_s::gpu {

QString settingKey(Viewer viewer)
{
    switch (viewer) {
    // (Keys of their own: the GPU chosen here, not by the defaults - on,
    // as they were in Rendering/... and Multiphysics/gpu, a layout viewer
    // slow to show or hide a layer. Those not read.)
    case Viewer::Pdf: return QStringLiteral("Viewers/pdfGpu");
    case Viewer::Markdown: return QStringLiteral("Viewers/markdownGpu");
    case Viewer::Layout: return QStringLiteral("Viewers/layoutGpu");
    case Viewer::Multiphysics: return QStringLiteral("Viewers/multiphysicsGpu");
    }
    return QString();
}

QString viewerName(Viewer viewer)
{
    switch (viewer) {
    case Viewer::Pdf: return QCoreApplication::translate("gpu", "PDF viewer");
    case Viewer::Markdown: return QCoreApplication::translate("gpu", "Markdown preview");
    case Viewer::Layout: return QCoreApplication::translate("gpu", "Layout viewer (GDS, OASIS)");
    case Viewer::Multiphysics: return QCoreApplication::translate("gpu", "Multiphysics Graphics view");
    }
    return QString();
}

bool wanted(Viewer viewer)
{
    // (The CPU's unless chosen, each viewer: Settings… > Viewers.)
    return QucsSettingsFile().value(settingKey(viewer), false).toBool();
}

void setWanted(Viewer viewer, bool gpu)
{
    QucsSettingsFile().setValue(settingKey(viewer), gpu);
    emit Notifier::instance()->changed(viewer);
}

QString hindrance()
{
    if (qEnvironmentVariable("QUCS_GPU") == QLatin1String("0"))
        return QCoreApplication::translate("gpu", "QUCS_GPU=0 is set: the CPU draws, whatever is chosen.");
#ifndef QUCS_GPU_VIEW
    return QCoreApplication::translate("gpu", "This build draws with the CPU alone (built without OpenGL).");
#else
    if (!qucs_s::fem::GlCanvas::available())
        return QCoreApplication::translate("gpu", "No OpenGL 3.3 here (a remote or offscreen display, or no such driver): the CPU draws.");
    return QString();
#endif
}

bool used(Viewer viewer)
{
    return wanted(viewer) && hindrance().isEmpty();
}

namespace {

#ifdef QUCS_GPU_VIEW
// An OpenGL viewport: each paint - the window's, a grab's - its paintGL, a
// painter on its framebuffer drawing the background and what the area
// gives it to draw. (Not the area's own painting: Qt's painter on an
// OpenGL widget outside paintGL is the grab's raster one in a grab, and
// paintGL - Qt's clears - the only thing a grab paints.)
class GlViewport : public QOpenGLWidget
{
public:
    explicit GlViewport(std::function<void(QPainter&)> paint) : a_paint(std::move(paint)) {}
    /// Its paint events taken before the area's filter has them (filters
    /// run last installed first).
    void takePaints() { installEventFilter(this); }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != this || event->type() != QEvent::Paint) return false;
        QOpenGLWidget::paintEvent(static_cast<QPaintEvent*>(event));
        return true;
    }
    void paintGL() override
    {
        const qreal ratio = devicePixelRatioF();
        QOpenGLPaintDevice device(QSize(int(std::lround(width() * ratio)), int(std::lround(height() * ratio))));
        device.setDevicePixelRatio(ratio);
        QPainter p(&device);
        p.setFont(font());
        p.fillRect(rect(), palette().color(backgroundRole()));
        if (a_paint) a_paint(p);
    }

private:
    std::function<void(QPainter&)> a_paint;
};
#endif

} // namespace

bool hasGlViewport(const QAbstractScrollArea* area)
{
#ifdef QUCS_GPU_VIEW
    return qobject_cast<QOpenGLWidget*>(area->viewport()) != nullptr;
#else
    Q_UNUSED(area);
    return false;
#endif
}

QImage shownFrame(QAbstractScrollArea* area)
{
#ifdef QUCS_GPU_VIEW
    auto* gl = qobject_cast<QOpenGLWidget*>(area->viewport());
    if (gl == nullptr || !gl->isValid()) return QImage();
    gl->makeCurrent();
    QOpenGLFunctions* f = gl->context()->functions();
    const qreal ratio = gl->devicePixelRatioF();
    const int w = int(std::lround(gl->width() * ratio)), h = int(std::lround(gl->height() * ratio));
    QImage upwards(w, h, QImage::Format_RGBA8888);
    f->glBindFramebuffer(GL_FRAMEBUFFER, gl->defaultFramebufferObject());
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, upwards.bits());
    gl->doneCurrent();
    // (OpenGL's rows from the bottom)
    QImage frame(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) std::memcpy(frame.scanLine(y), upwards.constScanLine(h - 1 - y), size_t(frame.bytesPerLine()));
    frame.setDevicePixelRatio(ratio);
    return frame.convertToFormat(QImage::Format_RGB32);
#else
    Q_UNUSED(area);
    return QImage();
#endif
}

bool setGlViewport(QAbstractScrollArea* area, bool gpu, std::function<void(QPainter&)> paint)
{
#ifdef QUCS_GPU_VIEW
    if (gpu == hasGlViewport(area)) return false;
    QWidget* old = area->viewport();
    // (Whether it fills its background: an OpenGL one keeps the plain one's.)
    const QVariant kept = old->property("qucsFillsBackground");
    const bool fills = kept.isValid() ? kept.toBool() : old->autoFillBackground();
    GlViewport* gl = gpu ? new GlViewport(std::move(paint)) : nullptr;
    QWidget* fresh = gl ? static_cast<QWidget*>(gl) : new QWidget();
    fresh->setObjectName(old->objectName());
    fresh->setMouseTracking(old->hasMouseTracking());
    fresh->setAcceptDrops(old->acceptDrops());
    fresh->setContextMenuPolicy(old->contextMenuPolicy());
    fresh->setToolTip(old->toolTip());
    fresh->setBackgroundRole(old->backgroundRole());
    fresh->setAutoFillBackground(fills);
    if (old->testAttribute(Qt::WA_SetCursor)) fresh->setCursor(old->cursor());
    for (Qt::WidgetAttribute a : {Qt::WA_OpaquePaintEvent, Qt::WA_NoSystemBackground, Qt::WA_StaticContents, Qt::WA_AcceptTouchEvents})
        fresh->setAttribute(a, old->testAttribute(a));
    // (A text view's layout measures by its viewport - QTextEdit sets it
    // so -: the new one, before the old is deleted.)
    if (auto* edit = qobject_cast<QTextEdit*>(area))
        if (QAbstractTextDocumentLayout* layout = edit->document()->documentLayout(); layout && layout->paintDevice() == old)
            layout->setPaintDevice(fresh);
    area->setViewport(fresh);   // (the old one deleted)
    if (gl) {
        // (setViewport has an OpenGL one clear itself at each painter's
        // start on it, as a graphics view's: its paintGL fills.)
        gl->setAutoFillBackground(false);
        gl->setProperty("qucsFillsBackground", fills);
        gl->takePaints();
    }
    // (The area's own style sheet - its colours, as the Markdown preview's
    // - given to the new viewport as it was to the old: the area polished
    // again. A viewport only polished as a new widget had the
    // application's palette.)
    if (!area->styleSheet().isEmpty()) area->setStyleSheet(area->styleSheet());
    fresh->update();
    return true;
#else
    Q_UNUSED(area);
    Q_UNUSED(gpu);
    Q_UNUSED(paint);
    return false;
#endif
}

Notifier* Notifier::instance()
{
    // (Never destroyed: viewers may ask at the program's end.)
    static Notifier* one = new Notifier();
    return one;
}

} // namespace qucs_s::gpu
