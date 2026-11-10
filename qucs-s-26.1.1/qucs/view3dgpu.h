/*
 * view3dgpu.h - Multiphysics 3D's view drawn on the GPU (Qt's RHI: Metal,
 *               Direct3D, OpenGL): its triangles, lines and dots from buffers
 *               made once a scene, their depth the GPU's, the Cut a
 *               half-space each fragment is tested against - so an orbit, a
 *               pan or a zoom costs the GPU a frame and the CPU nothing; the
 *               axes, the legend and the words over them by QPainter, as on
 *               the CPU, composed with them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_VIEW3DGPU_H
#define QUCS_VIEW3DGPU_H

#include "rhicanvas.h"

#include "fem3_geometry.h"

#include <memory>

class QPainter;
class View3D;

/*!
 * The GPU's canvas over a View3D, the view's size: it paints through the
 * view's own painting (View3D::paintFrame), which hands it the scene's
 * draw between its background and its overlay. Mouse and keys go to the
 * view under it. Drawn with 4 samples a pixel.
 */
class View3DCanvas : public qucs_s::gpu::LayeredCanvas
{
public:
    explicit View3DCanvas(View3D* view);
    ~View3DCanvas() override;

    /// The scene drawn now, in the view's painting.
    void drawScene(QPainter& p);
    /// The scenes whose buffers it has made (for the tests: a turn of the
    /// camera makes none).
    int builds() const { return a_builds; }

protected:
    void paintFrame(QPainter& p) override;
    void createResources(QRhi* rhi) override;
    void releaseNativeResources() override;
    void canvasFailed() override;

private:
    std::unique_ptr<QRhiBuffer> buffer(const void* data, std::size_t bytes);
    /// The scene's buffers, made again when it has changed (or the device's
    /// pixels a unit have: the lines' widths and the dots' radii are in them).
    void build();

    View3D* a_view;
    QRhi* a_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> a_uniforms, a_corners;
    std::unique_ptr<QRhiShaderResourceBindings> a_bindings;
    std::unique_ptr<QRhiGraphicsPipeline> a_triangles, a_lines, a_dots;
    std::unique_ptr<QRhiBuffer> a_triangleData, a_lineData, a_dotData;
    int a_triangleCount = 0, a_lineCount = 0, a_dotCount = 0;
    // What the buffers were made of: the view's scene's stamp, the device's
    // pixels a pixel, the origin their coordinates are relative to.
    quint64 a_stamp = 0;
    double a_ratio = 0;
    qucs_s::fem::Vec3 a_origin;
    int a_builds = 0;
};

#endif // QUCS_VIEW3DGPU_H
