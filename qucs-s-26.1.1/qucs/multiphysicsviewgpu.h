/*
 * multiphysicsviewgpu.h - the multiphysics Graphics view drawn on the GPU
 *                         (Qt's RHI: Metal, Direct3D, OpenGL): its domains,
 *                         mesh, color surface, boundaries, contours and rays
 *                         from buffers made once, so a pan or a zoom costs
 *                         the GPU a frame and the CPU little; the text over
 *                         them (the grid's labels, the legend, the title) by
 *                         QPainter as before, composed with them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSVIEWGPU_H
#define QUCS_MULTIPHYSICSVIEWGPU_H

#include "rhicanvas.h"

#include <QColor>
#include <QPointF>

#include <memory>
#include <vector>

class QPainter;

namespace qucs_s::fem {

class GraphicsView;

/*!
 * The GPU's canvas over a GraphicsView, the view's size: it paints through
 * the view's own painting (GraphicsView::paintFrame), which hands it the
 * heavy layers between its QPainter's. Mouse and keys go to the view under
 * it. Drawn with 4 samples a pixel.
 */
class GpuCanvas : public gpu::LayeredCanvas
{
public:
    explicit GpuCanvas(GraphicsView* view);
    ~GpuCanvas() override;

    struct Stroke {
        QColor color;
        float width = 1;   // device pixels (a cosmetic pen's)
        bool operator==(const Stroke& o) const { return color == o.color && width == o.width; }
    };
    // Drawn now, in the view's painting (between its QPainter's layers).
    void drawDomains(QPainter& p, const std::vector<QColor>& fills);
    void drawBoundaries(QPainter& p, const std::vector<Stroke>& strokes);
    void drawMesh(QPainter& p, QColor ink, float width);
    void drawQuality(QPainter& p);
    void drawSurface(QPainter& p);
    void drawContours(QPainter& p);
    void drawRays(QPainter& p);

protected:
    void paintFrame(QPainter& p) override;
    void createResources(QRhi* rhi) override;
    void releaseNativeResources() override;
    void canvasFailed() override;

private:
    struct Batch {
        std::unique_ptr<QRhiBuffer> vertices;
        std::unique_ptr<QRhiBuffer> extra;   // colors, values; a line's color and width
        int count = 0;                       // vertices or instances
        bool built = false;
    };
    void sync();
    std::unique_ptr<QRhiBuffer> buffer(const void* data, std::size_t bytes);
    /// \a b let go - kept till the next frame: a step of this frame may draw
    /// from it, its upload not yet sent (a plot of rays draws the
    /// boundaries twice a frame, other strokes the second time: their
    /// buffers made again, the first ones were deleted under the frame's
    /// first step - Metal wrote a freed buffer's update).
    void retire(std::unique_ptr<QRhiBuffer>& b);
    void buildLines(Batch& b, const std::vector<float>& segments, const std::vector<quint32>& colors, const std::vector<float>& widths);
    /// A slot of the uniforms for a draw this frame: its offset (-1, none
    /// left).
    int slot(const void* uniforms, std::size_t bytes);
    void drawTriangles(QPainter& p, Batch& b, bool surface);
    /// \a cosmetic: widths in device pixels, as QPainter's cosmetic pens;
    /// else in the view's.
    void drawLines(QPainter& p, Batch& b, bool roundCaps, bool cosmetic, const QColor* color = nullptr, float width = 1);

    GraphicsView* a_view;
    QRhi* a_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> a_uniforms, a_corners;
    int a_slotsUsed = 0;
    std::vector<std::unique_ptr<QRhiBuffer>> a_retired;   // let go this frame
    std::unique_ptr<QRhiTexture> a_colormap;
    QString a_colormapName;
    std::unique_ptr<QRhiSampler> a_nearest;
    std::unique_ptr<QRhiShaderResourceBindings> a_plainBindings, a_surfaceBindings;
    std::unique_ptr<QRhiGraphicsPipeline> a_fill, a_surface, a_lines;
    Batch a_domains, a_quality, a_surfaceBatch, a_mesh, a_boundaries, a_contours, a_rays;
    // What the batches were made of: the view's data's stamps, the origin
    // their coordinates are relative to (floats near it).
    quint64 a_topologyStamp = 0, a_meshStamp = 0, a_plotStamp = 0;
    QPointF a_origin;
    std::vector<int> a_domainOf;                   // by domain vertex
    std::vector<QColor> a_fills;                   // as uploaded
    std::vector<float> a_boundarySegments;          // x0 y0 x1 y1, relative to the origin
    std::vector<int> a_boundaryOf;                 // by segment
    std::vector<Stroke> a_strokes;                 // as uploaded
};

} // namespace qucs_s::fem

#endif // QUCS_MULTIPHYSICSVIEWGPU_H
