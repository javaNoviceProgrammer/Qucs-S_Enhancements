/*
 * layoutviewgpu.h - the layout viewer drawn on the GPU (Qt's RHI: Metal,
 *                   Direct3D, OpenGL): the cell shown, flattened to its
 *                   depth once into the GPU's buffers, so a pan or a zoom
 *                   costs the GPU a frame and the CPU little; the grid, the
 *                   texts, the selection and the rulers by QPainter as
 *                   before, composed over and under it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_LAYOUTVIEWGPU_H
#define QUCS_LAYOUTVIEWGPU_H

#include "layout.h"
#include "rhicanvas.h"

#include <QHash>
#include <QPointF>
#include <QRectF>

#include <memory>
#include <vector>

class QPainter;

namespace qucs_s::layout {

class LayoutView;

/*!
 * A cell flattened to a depth, for the GPU: each layer's shapes as fill
 * triangles (a convex one fanned, the rest triangulated by CDT, odd-even as
 * QPainter fills them), outline segments and a dot each (where it is under
 * 1.5 pixels); the cells deeper as frames (and a dot each); their names and
 * the texts, for QPainter. Coordinates in µm from \a origin, as floats.
 * Every copy of a repetition its own (no level of detail: the GPU draws
 * them all); past \a budget shapes it stops, said by tooLarge.
 */
struct Flattened {
    QPointF origin;
    struct Layer {
        std::vector<float> fills;      // x y, three a triangle
        std::vector<float> outlines;   // x0 y0 x1 y1
        std::vector<float> dots;       // x y size (its larger side, µm)
    };
    std::vector<Layer> layers;         // by Layout::layers' index
    std::vector<float> frames;         // x0 y0 x1 y1
    std::vector<float> frameDots;      // x y size
    QVector<QPair<QRectF, QString>> names;    // a frame's box (µm) and its cell's name
    QVector<QPair<QPointF, QString>> texts;   // µm
    std::vector<quint32> textLayers;          // each text's layer
    quint64 shapes = 0, frameCount = 0;
    quint64 approximated = 0;   // fills CDT could not make: fanned
    bool tooLarge = false;
};
Flattened flatten(const Layout& layout, int cell, int depth, const QVector<LayerStyle>& styles, bool labels, quint64 budget);

/*!
 * The GPU's canvas over a LayoutView, its size: it paints through the
 * view's own painting (LayoutView::paintFrame), which hands it the layers
 * in the middle of its QPainter's (the grid under them, the texts, the
 * selection and the rulers over). Mouse and keys go to the view under it.
 * Its buffers made again when the layout, the cell, the depth or the texts
 * change; colours and patterns are the shaders' (a theme, a .lyp, the
 * adapted colours: no new buffers). A layer hidden keeps its buffers, not
 * drawn; one shown for the first time is flattened alone and its buffers
 * added - shown or hidden again, nothing made.
 */
class LayoutGpu : public gpu::LayeredCanvas
{
public:
    explicit LayoutGpu(LayoutView* view);
    ~LayoutGpu() override;

    /// Shapes at most on the GPU: beyond, the CPU's picture is shown.
    static constexpr quint64 Budget = 2000000;

    /// Its buffers made, whole (for the tests).
    int builds() const { return a_builds; }
    /// Layers flattened alone and added, shown for the first time (for the
    /// tests).
    int layersAdded() const { return a_layersAdded; }
    /// The texts its last frame drew (for the tests).
    int textsDrawn() const { return a_textsDrawn; }
    /// The vertices the CPU still holds (the GPU has them: none, for the
    /// tests).
    std::size_t heldFloats() const;
    /// Past its budget: the CPU draws (its picture shown here).
    bool tooLarge() const { return a_flat.tooLarge; }
    const Flattened& flattened() const { return a_flat; }

    /// The layers, frames, their names and the texts drawn now, in the
    /// view's painting; \a stats what it drew. False, nothing drawn, past
    /// its budget (the view draws the CPU's picture then).
    bool drawLayout(QPainter& p, RenderStats* stats);

protected:
    void paintFrame(QPainter& p) override;
    void createResources(QRhi* rhi) override;
    void releaseNativeResources() override;
    void canvasFailed() override;

private:
    struct Batch {
        std::unique_ptr<QRhiBuffer> buffer;
        int count = 0;   // vertices (fills, outlines) or instances (dots)
    };
    struct LayerBatches {
        Batch fills, outlines, dots;
    };
    void sync();
    void upload(Batch& b, const std::vector<float>& data, int components);
    QRhiTexture* patternTexture(Qt::BrushStyle style);
    QRhiShaderResourceBindings* bindingsFor(QRhiTexture* pattern);

    LayoutView* a_view;
    int a_builds = 0, a_layersAdded = 0, a_textsDrawn = 0;
    QRhi* a_rhi = nullptr;
    std::vector<std::unique_ptr<LayerBatches>> a_layers;
    Batch a_frameLines, a_frameDots;
    std::unique_ptr<QRhiBuffer> a_uniforms, a_corners;
    int a_slots = 0;   // the uniform buffer's, a draw each
    std::unique_ptr<QRhiSampler> a_nearest;
    std::unique_ptr<QRhiTexture> a_plain;   // no pattern
    QHash<int, std::shared_ptr<QRhiTexture>> a_patterns;   // by Qt::BrushStyle: 8 by 8, a byte each
    QHash<QRhiTexture*, std::shared_ptr<QRhiShaderResourceBindings>> a_bindings;
    std::unique_ptr<QRhiShaderResourceBindings> a_layoutBindings;
    std::unique_ptr<QRhiGraphicsPipeline> a_fill, a_line, a_dot;
    Flattened a_flat;
    // What the buffers were made of.
    const Layout* a_madeOf = nullptr;
    quint64 a_generation = 0;   // the view's a_layoutGeneration then
    int a_cell = -2, a_depth = -1;
    bool a_labels = false;
    std::vector<char> a_made;    // by layer: flattened into the buffers
    std::vector<char> a_shown;   // by layer: shown at the last whole build
};

} // namespace qucs_s::layout

#endif
