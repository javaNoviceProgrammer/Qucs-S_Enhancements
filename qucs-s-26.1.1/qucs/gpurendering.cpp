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
#include "rhicanvas.h"
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
    return QCoreApplication::translate("gpu", "This build draws with the CPU alone (built without Qt's RHI).");
#else
    if (!rhiAvailable())
        return QCoreApplication::translate("gpu", "No GPU backend here (Metal, Direct3D, OpenGL: a remote or offscreen display, or no such driver): the CPU draws.");
    return QString();
#endif
}

QString backendName()
{
#ifdef QUCS_GPU_VIEW
    if (!rhiAvailable()) return QString();
#if defined(Q_OS_MACOS)
    return QStringLiteral("Metal");
#elif defined(Q_OS_WIN)
    return QStringLiteral("Direct3D 11");
#else
    return QStringLiteral("OpenGL");
#endif
#else
    return QString();
#endif
}

bool used(Viewer viewer)
{
    return wanted(viewer) && hindrance().isEmpty();
}

bool hasGpuViewport(const QAbstractScrollArea* area)
{
#ifdef QUCS_GPU_VIEW
    return dynamic_cast<Viewport2D*>(area->viewport()) != nullptr;
#else
    Q_UNUSED(area);
    return false;
#endif
}

QImage shownFrame(QAbstractScrollArea* area)
{
#ifdef QUCS_GPU_VIEW
    auto* gpu = dynamic_cast<Viewport2D*>(area->viewport());
    if (gpu == nullptr) return QImage();
    QImage frame = gpu->grabFramebuffer();
    return frame.convertToFormat(QImage::Format_RGB32);
#else
    Q_UNUSED(area);
    return QImage();
#endif
}

bool setGpuViewport(QAbstractScrollArea* area, bool gpu, std::function<void(Canvas2D&)> paint)
{
#ifdef QUCS_GPU_VIEW
    if (gpu == hasGpuViewport(area)) return false;
    QWidget* old = area->viewport();
    // (Whether it fills its background: the GPU's keeps the plain one's.)
    const QVariant kept = old->property("qucsFillsBackground");
    const bool fills = kept.isValid() ? kept.toBool() : old->autoFillBackground();
    Viewport2D* rhi = gpu ? new Viewport2D(std::move(paint)) : nullptr;
    QWidget* fresh = rhi ? static_cast<QWidget*>(rhi) : new QWidget();
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
    if (rhi) {
        // (Its frame all its drawing: no background of the widget's.)
        rhi->setAutoFillBackground(false);
        rhi->setProperty("qucsFillsBackground", fills);
        rhi->takePaints();
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
