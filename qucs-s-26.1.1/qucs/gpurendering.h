/*
 * gpurendering.h - which viewers the GPU draws: the PDF viewer, the
 *                  Markdown preview, the layout viewer and the multiphysics
 *                  Graphics view, each the user's choice (Qucs-S's
 *                  Settings… > Viewers, the CPU unless chosen), and what keeps the GPU
 *                  from drawing at all
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_GPURENDERING_H
#define QUCS_GPURENDERING_H

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>

class QAbstractScrollArea;

namespace qucs_s::gpu {

class Canvas2D;

enum class Viewer { Pdf, Markdown, Layout, Multiphysics };

/// The setting's key: Viewers/pdfGpu, Viewers/markdownGpu,
/// Viewers/layoutGpu, Viewers/multiphysicsGpu (the Graphics view's own menu
/// and Multiphysics > Settings… set it too).
QString settingKey(Viewer viewer);
/// "PDF viewer", "Markdown preview", ...
QString viewerName(Viewer viewer);
/// The user's choice (Settings… > Viewers): the CPU unless the GPU is
/// chosen - an opt-in, each viewer.
bool wanted(Viewer viewer);
/// Kept, and told to the open viewers (changed()).
void setWanted(Viewer viewer, bool gpu);
/// Why the GPU cannot draw here, whatever is chosen (empty: it can):
/// QUCS_GPU=0 set, a build without Qt's RHI, no backend for it here (an
/// offscreen or remote display, a driver without one).
QString hindrance();
/// The RHI's backend here: "Metal", "Direct3D 11", "OpenGL" (empty: none).
QString backendName();
/// Drawn by the GPU: chosen, and nothing in its way.
bool used(Viewer viewer);

/// \a area's viewport made the GPU's (\a gpu: Qt's RHI - Metal, Direct3D,
/// OpenGL) or a plain one again, its mouse tracking, cursor, tooltip, menu
/// policy and background kept. On the GPU \a paint draws all of it, each
/// time it is painted - by the window, by a grab of it (a screenshot) -:
/// fills and images (Canvas2D, rhicanvas.h; an image's texture kept by its
/// cache key), on its background (its role's colour); the area's own
/// painting is not used. False when nothing changed. The area's own
/// filters on its viewport are its to install again.
/// \a failed is called when the GPU's viewport gives up (no QRhi for it,
/// its pipelines not made): the area is not given the GPU's again, and
/// \a failed puts the CPU's back (setGpuViewport(area, false), the owner's
/// filters and state with it); without one the CPU's is put back here.
bool setGpuViewport(QAbstractScrollArea* area, bool gpu, std::function<void(Canvas2D&)> paint = {}, std::function<void()> failed = {});
/// The area's GPU viewport gave up: the CPU's for good.
bool gpuViewportBroken(const QAbstractScrollArea* area);
bool hasGpuViewport(const QAbstractScrollArea* area);
/// What \a area's GPU viewport shows: a frame of it drawn and read back
/// (for the tests). Null for a plain viewport.
QImage shownFrame(QAbstractScrollArea* area);

/// Tells the open viewers that a choice changed.
class Notifier : public QObject
{
    Q_OBJECT
public:
    static Notifier* instance();
signals:
    void changed(qucs_s::gpu::Viewer viewer);
};

} // namespace qucs_s::gpu

#endif
