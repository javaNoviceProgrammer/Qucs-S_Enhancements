/*
 * rhicanvas.h - the GPU viewers' canvases on Qt's RHI (Metal on macOS,
 *               Direct3D on Windows, OpenGL elsewhere): a canvas whose
 *               QPainter drawing is composed with the GPU's own between it,
 *               and a scroll area's viewport drawing images and fills
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RHICANVAS_H
#define QUCS_RHICANVAS_H

#include <QColor>
#include <QHash>
#include <QImage>
#include <QMatrix4x4>
#include <QPolygonF>
#include <QRectF>
#include <QRegion>
#include <QRhiWidget>

#include <rhi/qrhi.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class QPainter;

namespace qucs_s::gpu {

/// A shader compiled for the RHI (shaders/<name>.qsb, scripts/compile-shaders.sh).
QShader shader(const QString& name);

/// Whether the RHI can draw here: a backend made (Metal, Direct3D 11,
/// OpenGL), not on the offscreen or minimal platforms (the tests', a
/// remote's: the CPU draws there, as it did with OpenGL).
bool rhiAvailable();
/// Whether \a rhi can run the viewers' shaders: Metal; Direct3D 11 at
/// feature level 11; OpenGL 3.0 or OpenGL ES 3.0; instancing.
bool fitForTheShaders(QRhi* rhi);

/// A pipeline for a canvas: its shaders, vertex input, resources and blend
/// (premultiplied colors, or straight alpha's).
struct PipelineSpec {
    QString vertex, fragment;
    QRhiVertexInputLayout input;
    QRhiGraphicsPipeline::Topology topology = QRhiGraphicsPipeline::Triangles;
    bool premultiplied = true;
    bool scissor = false;   // its draws cut to a scissor (set before them)
};
std::unique_ptr<QRhiGraphicsPipeline> makePipeline(QRhi* rhi, QRhiRenderPassDescriptor* pass, int samples, QRhiShaderResourceBindings* layout,
                                                   const PipelineSpec& spec);

/*!
 * A canvas drawn by the GPU over a view: the view's own painting (its
 * QPainter's, as on the CPU) in layers, and between them the GPU's steps -
 * a layout's layers, a mesh - drawn from its buffers. The painting goes to a
 * transparent image the canvas's size; a step (native()) ends it there, the
 * step drawn after it, and the painting goes on - its state kept - on the
 * next image. The layers and steps are composed in their order: what the
 * view paints over a step is over it, as it was with OpenGL's painter.
 * Mouse and keys go to the view under it.
 */
class LayeredCanvas : public QRhiWidget
{
public:
    LayeredCanvas(QWidget* view, int samples);
    ~LayeredCanvas() override;

    /// No backend here, or one lost: the view draws itself again.
    bool failed() const { return a_failed; }
    /// The frames it has drawn (for the tests).
    int frames() const { return a_frames; }
    /// The layers' pixels sent to the GPU since the start (for the tests).
    qint64 sentPixels() const { return a_sent; }

    /// The frame's background filled with \a color, the first thing painted
    /// on \a p: the pass cleared to it (filled by the CPU, the first layer
    /// was opaque, sent whole at every frame); on another painter, a fill.
    void fillBackground(QPainter& p, const QColor& color);

protected:
    /// The view's painting of a frame on \a p, native() called in it.
    virtual void paintFrame(QPainter& p) = 0;
    /// The GPU's resources made for \a rhi (pipelines, ...): when it is new.
    virtual void createResources(QRhi* rhi) { Q_UNUSED(rhi); }
    /// And dropped: the canvas moved to another window, ended.
    virtual void releaseNativeResources() {}
    /// Told when no backend could be made (failed()).
    virtual void canvasFailed() {}

    /// A step drawn by the GPU here, between the layers: \a draw records its
    /// draws in the frame's pass (its viewport set). Uploads go to
    /// updates() before: it is the frame's batch, sent with the pass.
    void native(QPainter& p, std::function<void(QRhiCommandBuffer*)> draw);
    QRhiResourceUpdateBatch* updates() const { return a_updates; }
    /// For the native steps.
    QMatrix4x4 corr() const;
    QSize pixels() const;
    int samples() const { return a_samples; }
    QRhiRenderPassDescriptor* pass() const;
    QRhiViewport fullViewport() const;
    /// The framebuffer's rows upwards (OpenGL's): gl_FragCoord from the
    /// bottom left.
    bool yUp() const;

    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    void renderMissed();
    struct Layer {
        QImage image;
        QRect inked;   // the image's ink: what its next frame clears, and the texture's part drawn
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
    };
    struct Step {
        int layer = -1;   // a layer's index, or a native step
        std::function<void(QRhiCommandBuffer*)> draw;
    };
    Layer& layer(int index);
    void dropAll();

    int a_samples;
    bool a_failed = false;
    int a_frames = 0;
    qint64 a_sent = 0;
    int a_misses = 0;   // frames not drawn, no QRhi, since the last one drawn
    QRhi* a_rhi = nullptr;
    std::vector<std::unique_ptr<Layer>> a_layers;
    std::vector<Step> a_steps;
    int a_open = -1;   // the layer painted on now
    QColor a_clear;    // the frame's background, or the palette's window
    QPainter* a_painter = nullptr;
    QRhiResourceUpdateBatch* a_updates = nullptr;
    std::unique_ptr<QRhiBuffer> a_quad, a_uniforms;
    std::unique_ptr<QRhiSampler> a_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> a_layout;
    std::unique_ptr<QRhiGraphicsPipeline> a_composite;
};

/*!
 * A frame of 2D drawing for the GPU - what a PDF's pages and a Markdown
 * preview draw there: fills (rectangles, convex polygons) and images, in
 * the viewport's pixels, in their order; a clip of rectangles applied to
 * each. An image is a texture kept by its QImage's cache key: the same
 * image drawn again is not sent again.
 */
class Canvas2D
{
public:
    Canvas2D(QSize size, qreal ratio) : a_size(size), a_ratio(ratio) {}
    QSize size() const { return a_size; }
    qreal devicePixelRatio() const { return a_ratio; }

    void fillRect(const QRectF& rect, const QColor& color);
    /// Convex: fanned.
    void fillPolygon(const QPolygonF& polygon, const QColor& color);
    /// \a source in the image's pixels; the whole image when empty.
    void drawImage(const QRectF& target, const QImage& image, const QRectF& source = QRectF());
    /// At its device pixel ratio's size, on whole device pixels (as QPainter
    /// draws it).
    void drawImage(const QPointF& at, const QImage& image);
    /// The clip (empty: none).
    void setClip(const QRegion& region) { a_clip = region; }

    struct Op {
        QImage image;            // null: a fill
        std::vector<float> fill; // x y r g b a (premultiplied), three a triangle
        QRectF target, source;
    };
    const std::vector<Op>& ops() const { return a_ops; }

private:
    void addTriangles(const QPolygonF& polygon, const QColor& color);
    QSize a_size;
    qreal a_ratio;
    QRegion a_clip;
    std::vector<Op> a_ops;
};

/*!
 * A scroll area's viewport drawn by the GPU: each frame its owner's drawing
 * (a Canvas2D) - the area's own painting not used. An image's texture kept
 * while the image is (dropped at the next frame once it is destroyed or
 * changed, as Qt's OpenGL engine dropped its own), and not drawn for 120
 * frames or past 384 MB, the oldest first.
 */
class Viewport2D : public QRhiWidget
{
public:
    explicit Viewport2D(std::function<void(Canvas2D&)> paint);
    ~Viewport2D() override;
    bool failed() const { return a_failed; }
    int frames() const { return a_frames; }
    /// The textures it keeps (for the tests).
    int textures() const { return int(a_textures.size()); }
    /// Images sent to the GPU since the start (for the tests).
    int uploads() const { return a_uploads; }
    /// Its paint events taken before the area's filter has them (filters
    /// run last installed first): after setViewport. Before, it is not
    /// painted (the area's own painting would have the paint).
    void takePaints();
    /// \a f called once, when it gives up: no QRhi in some 40 tries, its
    /// pipelines not made (setGpuViewport puts the CPU's viewport back).
    void onFailure(std::function<void()> f) { a_onFailure = std::move(f); }
    /// It draws no more, its owner told (for the tests too).
    void giveUp();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Texture {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
        qint64 frame = 0;
        qsizetype bytes = 0;
    };
    void dropAll();
    /// An image let go - destroyed, changed (Qt's image cleanup hook; any
    /// thread): its textures dropped at each viewport's next frame.
    static void imageGone(qint64 key);

    std::function<void(Canvas2D&)> a_paint;
    std::vector<qint64> a_gone;   // (under the hook's lock)
    std::function<void()> a_onFailure;
    bool a_failed = false, a_toldFailure = false;
    bool a_takesPaints = false;
    int a_frames = 0, a_uploads = 0;
    int a_misses = 0;
    QRhi* a_rhi = nullptr;
    std::unordered_map<qint64, Texture> a_textures;   // by the image's cache key
    qsizetype a_textureBytes = 0;
    std::unique_ptr<QRhiBuffer> a_vertices, a_uniforms;
    std::unique_ptr<QRhiSampler> a_smooth;
    std::unique_ptr<QRhiShaderResourceBindings> a_solidBindings, a_imageLayout;
    std::unique_ptr<QRhiGraphicsPipeline> a_solid, a_image;
};

} // namespace qucs_s::gpu

#endif
