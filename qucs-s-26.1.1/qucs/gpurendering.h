/*
 * gpurendering.h - which viewers the GPU draws: the PDF viewer, the
 *                  Markdown preview, the layout viewer and the multiphysics
 *                  Graphics view, each the user's choice (Qucs-S's
 *                  Settings… > Appearance > Drawing), and what keeps the GPU
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
class QPainter;

namespace qucs_s::gpu {

enum class Viewer { Pdf, Markdown, Layout, Multiphysics };

/// The setting's key: Rendering/pdf, Rendering/markdown, Rendering/layout;
/// the Graphics view's Multiphysics/gpu (its own menu sets it too).
QString settingKey(Viewer viewer);
/// "PDF viewer", "Markdown preview", ...
QString viewerName(Viewer viewer);
/// The user's choice: the GPU unless set to the CPU - the Markdown
/// preview the CPU unless set to the GPU (its scrolls the CPU's cheaper).
bool wanted(Viewer viewer);
/// Kept, and told to the open viewers (changed()).
void setWanted(Viewer viewer, bool gpu);
/// Why the GPU cannot draw here, whatever is chosen (empty: it can):
/// QUCS_GPU=0 set, a build without OpenGL, no OpenGL 3.3 here (an offscreen
/// or remote display, a driver without it).
QString hindrance();
/// Drawn by the GPU: chosen, and nothing in its way.
bool used(Viewer viewer);

/// \a area's viewport made an OpenGL widget (\a gpu) or a plain one again,
/// its mouse tracking, cursor, tooltip, menu policy and background kept.
/// On the GPU \a paint draws all of it, each time it is painted - by the
/// window, by a grab of it (a screenshot) -, with QPainter on its
/// framebuffer: the GPU's (its images textures kept there, its glyphs an
/// atlas), on its background (its role's colour); the area's own painting
/// is not used. False when nothing changed. The area's own filters on its
/// viewport are its to install again.
bool setGlViewport(QAbstractScrollArea* area, bool gpu, std::function<void(QPainter&)> paint = {});
bool hasGlViewport(const QAbstractScrollArea* area);
/// What \a area's OpenGL viewport shows: its framebuffer as last painted,
/// read back - not painted again, as a grab paints it (for the tests).
/// Null for a plain viewport.
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
