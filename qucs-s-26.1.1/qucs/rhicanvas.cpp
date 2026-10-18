/*
 * rhicanvas.cpp - the GPU viewers' canvases on Qt's RHI
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rhicanvas.h"

#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>

#include <QMutex>
#include <private/qimagepixmapcleanuphooks_p.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qucs_s::gpu {

namespace {

// The composite's uniforms: the clip space's correction and an opacity.
struct CompositeUniforms {
    float corr[16];
    float opacity;
    float pad[3];
};

void store(float* out, const QMatrix4x4& m)
{
    std::memcpy(out, m.constData(), 16 * sizeof(float));
}

// A full-viewport quad, a strip: clip x y (y up), texture u v (v down).
const float kQuad[] = {-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};

QRhiVertexInputLayout imageInput()
{
    QRhiVertexInputLayout input;
    input.setBindings({QRhiVertexInputBinding(4 * sizeof(float))});
    input.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                         QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float))});
    return input;
}

/// No pixel of \a image inked (all transparent).
bool blank(const QImage& image)
{
    const qsizetype bytes = image.sizeInBytes();
    const uchar* p = image.constBits();
    const auto* words = reinterpret_cast<const quint64*>(p);
    const qsizetype n = bytes / 8;
    for (qsizetype i = 0; i < n; ++i)
        if (words[i] != 0) return false;
    for (qsizetype i = n * 8; i < bytes; ++i)
        if (p[i] != 0) return false;
    return true;
}

// QPainter's state, kept across the image it paints on: what a view's
// painting may have set before a step and paint on with after it.
struct PainterState {
    QFont font;
    QPen pen;
    QBrush brush, background;
    QPointF brushOrigin;
    Qt::BGMode backgroundMode;
    qreal opacity;
    QPainter::CompositionMode composition;
    QPainter::RenderHints hints;
    Qt::LayoutDirection direction;
    QTransform transform;
    bool clipping;
    QPainterPath clip;
    explicit PainterState(const QPainter& p)
        : font(p.font()), pen(p.pen()), brush(p.brush()), background(p.background()), brushOrigin(p.brushOriginF()),
          backgroundMode(p.backgroundMode()), opacity(p.opacity()), composition(p.compositionMode()), hints(p.renderHints()),
          direction(p.layoutDirection()), transform(p.worldTransform()), clipping(p.hasClipping())
    {
        if (clipping) clip = p.clipPath();
    }
    void restore(QPainter& p) const
    {
        p.setFont(font);
        p.setPen(pen);
        p.setBrush(brush);
        p.setBackground(background);
        p.setBrushOrigin(brushOrigin);
        p.setBackgroundMode(backgroundMode);
        p.setOpacity(opacity);
        p.setCompositionMode(composition);
        p.setRenderHints(QPainter::RenderHints::fromInt(~0), false);
        p.setRenderHints(hints, true);
        p.setLayoutDirection(direction);
        // (The clip in logical coordinates, as clipPath() gives it: the
        // transform first.)
        p.setWorldTransform(transform);
        if (clipping) p.setClipPath(clip);
    }
};

/// A frame not drawn, no QRhi for it: tried again. (Before the window's
/// first frame on the GPU there is none - the window made again for the
/// widget, its own paint before that, as it is shown -: a moment after it
/// draws.) None in some 40 tries, or no backend here at all: true, the CPU
/// to draw.
bool missed(QRhiWidget* w, int& misses)
{
    if (++misses < 40 && rhiAvailable()) {
        QTimer::singleShot(50, w, [w] { w->update(); });
        return false;
    }
    return true;
}

} // namespace

QShader shader(const QString& name)
{
    QFile f(QStringLiteral(":/shaders/%1.qsb").arg(name));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("GPU: no shader %s", qPrintable(name));
        return QShader();
    }
    return QShader::fromSerialized(f.readAll());
}

bool rhiAvailable()
{
    static const bool can = [] {
        const QString platform = QGuiApplication::platformName();
        if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal") || platform == QLatin1String("vnc")) return false;
        std::unique_ptr<QRhi> rhi;
#if defined(Q_OS_MACOS)
        QRhiMetalInitParams params;
        rhi.reset(QRhi::create(QRhi::Metal, &params));
#elif defined(Q_OS_WIN)
        QRhiD3D11InitParams params;
        rhi.reset(QRhi::create(QRhi::D3D11, &params));
#else
        QRhiGles2InitParams params;
        std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
        params.fallbackSurface = surface.get();
        rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
#endif
        return rhi != nullptr;
    }();
    return can;
}

std::unique_ptr<QRhiGraphicsPipeline> makePipeline(QRhi* rhi, QRhiRenderPassDescriptor* pass, int samples, QRhiShaderResourceBindings* layout,
                                                   const PipelineSpec& spec)
{
    std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
    p->setTopology(spec.topology);
    p->setShaderStages({{QRhiShaderStage::Vertex, shader(spec.vertex)}, {QRhiShaderStage::Fragment, shader(spec.fragment)}});
    p->setVertexInputLayout(spec.input);
    p->setShaderResourceBindings(layout);
    p->setRenderPassDescriptor(pass);
    p->setSampleCount(samples);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = spec.premultiplied ? QRhiGraphicsPipeline::One : QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    p->setTargetBlends({blend});
    if (!p->create()) return nullptr;
    return p;
}

// ----------------------------------------------------------------------
// LayeredCanvas

LayeredCanvas::LayeredCanvas(QWidget* view, int samples) : QRhiWidget(nullptr), a_samples(std::max(1, samples))
{
    // (Parented now, its RHI asked for - QRhiWidget's constructor asks for
    // it after QWidget's has parented it: so made, the window was not made
    // ready for the RHI, its first frames with no QRhi.)
    setParent(view);
    setSampleCount(a_samples);
    // The view under it takes the mouse, the wheel, the gestures and keys.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(view->rect());
    connect(this, &QRhiWidget::renderFailed, this, &LayeredCanvas::renderMissed);
}

void LayeredCanvas::renderMissed()
{
    if (a_failed || !missed(this, a_misses)) return;
    qWarning("GPU canvas: no backend for Qt's RHI here; drawn by the CPU");
    a_failed = true;
    canvasFailed();
}

LayeredCanvas::~LayeredCanvas()
{
    dropAll();
}

void LayeredCanvas::dropAll()
{
    a_layers.clear();
    a_steps.clear();
    a_composite.reset();
    a_layout.reset();
    a_sampler.reset();
    a_uniforms.reset();
    a_quad.reset();
    a_rhi = nullptr;
}

void LayeredCanvas::releaseResources()
{
    releaseNativeResources();
    dropAll();
}

QMatrix4x4 LayeredCanvas::corr() const
{
    return rhi() ? rhi()->clipSpaceCorrMatrix() : QMatrix4x4();
}

QSize LayeredCanvas::pixels() const
{
    return renderTarget() ? renderTarget()->pixelSize() : QSize();
}

QRhiRenderPassDescriptor* LayeredCanvas::pass() const
{
    return renderTarget() ? renderTarget()->renderPassDescriptor() : nullptr;
}

QRhiViewport LayeredCanvas::fullViewport() const
{
    const QSize px = pixels();
    return QRhiViewport(0, 0, float(px.width()), float(px.height()));
}

bool LayeredCanvas::yUp() const
{
    return rhi() && rhi()->isYUpInFramebuffer();
}

void LayeredCanvas::initialize(QRhiCommandBuffer*)
{
    if (a_rhi != rhi()) {
        releaseNativeResources();
        dropAll();
        a_rhi = rhi();
    }
    if (!a_rhi || a_composite) return;
    a_quad.reset(a_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
    a_quad->create();
    a_uniforms.reset(a_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(CompositeUniforms)));
    a_uniforms->create();
    a_sampler.reset(a_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    a_sampler->create();
    // (The pipeline's layout: the bindings' kinds, any texture of them.)
    std::unique_ptr<QRhiTexture> any(a_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    any->create();
    a_layout.reset(a_rhi->newShaderResourceBindings());
    a_layout->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                                     a_uniforms.get()),
                           QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, any.get(), a_sampler.get())});
    a_layout->create();
    PipelineSpec spec;
    spec.vertex = QStringLiteral("composite.vert");
    spec.fragment = QStringLiteral("composite.frag");
    spec.input = imageInput();
    spec.topology = QRhiGraphicsPipeline::TriangleStrip;
    a_composite = makePipeline(a_rhi, pass(), a_samples, a_layout.get(), spec);
    if (!a_composite) {
        a_failed = true;
        canvasFailed();
        return;
    }
    QRhiResourceUpdateBatch* u = a_rhi->nextResourceUpdateBatch();
    u->uploadStaticBuffer(a_quad.get(), kQuad);
    CompositeUniforms cu{};
    store(cu.corr, a_rhi->clipSpaceCorrMatrix());
    cu.opacity = 1.0f;
    u->updateDynamicBuffer(a_uniforms.get(), 0, sizeof(cu), &cu);
    // (Sent with the first frame's pass.)
    a_updates = u;
    createResources(a_rhi);
}

LayeredCanvas::Layer& LayeredCanvas::layer(int index)
{
    while (int(a_layers.size()) <= index) a_layers.push_back(std::make_unique<Layer>());
    Layer& l = *a_layers[std::size_t(index)];
    const QSize px = pixels();
    if (l.image.size() != px) {
        l.image = QImage(px, QImage::Format_RGBA8888_Premultiplied);
        l.texture.reset();
        l.bindings.reset();
    }
    l.image.setDevicePixelRatio(devicePixelRatioF());
    l.image.fill(Qt::transparent);
    return l;
}

void LayeredCanvas::native(QPainter& p, std::function<void(QRhiCommandBuffer*)> draw)
{
    if (&p != a_painter || a_open < 0) {
        // (Not in this canvas's frame: nothing to compose with.)
        return;
    }
    const PainterState state(p);
    p.end();
    a_steps.push_back({-1, std::move(draw)});
    a_open = a_open + 1;
    Layer& next = layer(a_open);
    p.begin(&next.image);
    state.restore(p);
    a_steps.push_back({a_open, {}});
}

void LayeredCanvas::render(QRhiCommandBuffer* cb)
{
    if (a_failed || !a_rhi || !a_composite) return;
    if (!a_updates) a_updates = a_rhi->nextResourceUpdateBatch();
    a_steps.clear();
    a_open = 0;
    Layer& first = layer(0);
    a_steps.push_back({0, {}});
    {
        QPainter p(&first.image);
        p.setFont(font());
        a_painter = &p;
        paintFrame(p);
        a_painter = nullptr;
    }
    // The layers inked, sent; those not, passed over.
    std::vector<char> inked(a_layers.size(), 0);
    for (const Step& s : a_steps) {
        if (s.layer < 0) continue;
        Layer& l = *a_layers[std::size_t(s.layer)];
        if (blank(l.image)) continue;
        inked[std::size_t(s.layer)] = 1;
        if (!l.texture) {
            l.texture.reset(a_rhi->newTexture(QRhiTexture::RGBA8, l.image.size()));
            l.texture->create();
            l.bindings.reset(a_rhi->newShaderResourceBindings());
            l.bindings->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                         0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, a_uniforms.get()),
                                     QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, l.texture.get(),
                                                                               a_sampler.get())});
            l.bindings->create();
        }
        a_updates->uploadTexture(l.texture.get(), l.image);
    }
    QRhiResourceUpdateBatch* u = a_updates;
    a_updates = nullptr;
    const QColor clear = palette().color(QPalette::Window);
    cb->beginPass(renderTarget(), clear, {1.0f, 0}, u);
    const QRhiViewport viewport = fullViewport();
    const QRhiCommandBuffer::VertexInput quad(a_quad.get(), 0);
    for (const Step& s : a_steps) {
        if (s.layer < 0) {
            if (s.draw) s.draw(cb);
            continue;
        }
        if (!inked[std::size_t(s.layer)]) continue;
        cb->setGraphicsPipeline(a_composite.get());
        cb->setViewport(viewport);
        cb->setShaderResources(a_layers[std::size_t(s.layer)]->bindings.get());
        cb->setVertexInput(0, 1, &quad);
        cb->draw(4);
    }
    cb->endPass();
    a_steps.clear();
    ++a_frames;
    a_misses = 0;
}

// ----------------------------------------------------------------------
// Canvas2D

void Canvas2D::addTriangles(const QPolygonF& closed, const QColor& color)
{
    // (A closing point - QPolygonF(QRectF)'s, the first again - dropped:
    // fanned, an empty triangle.)
    const QPolygonF polygon = closed.size() > 3 && closed.isClosed() ? QPolygonF(closed.first(closed.size() - 1)) : closed;
    if (polygon.size() < 3 || color.alpha() == 0) return;
    if (a_ops.empty() || !a_ops.back().image.isNull()) a_ops.push_back(Op());
    std::vector<float>& f = a_ops.back().fill;
    const float a = float(color.alphaF());
    const float r = float(color.redF()) * a, g = float(color.greenF()) * a, b = float(color.blueF()) * a;
    for (qsizetype k = 1; k + 1 < polygon.size(); ++k)
        for (const QPointF& p : {polygon[0], polygon[k], polygon[k + 1]}) f.insert(f.end(), {float(p.x()), float(p.y()), r, g, b, a});
}

void Canvas2D::fillRect(const QRectF& rect, const QColor& color)
{
    if (a_clip.isEmpty()) {
        addTriangles(QPolygonF(rect), color);
        return;
    }
    for (const QRect& c : a_clip) {
        const QRectF piece = rect.intersected(QRectF(c));
        if (!piece.isEmpty()) addTriangles(QPolygonF(piece), color);
    }
}

void Canvas2D::fillPolygon(const QPolygonF& polygon, const QColor& color)
{
    if (a_clip.isEmpty()) {
        addTriangles(polygon, color);
        return;
    }
    // (Each clip rectangle's part: Sutherland and Hodgman's, the polygon
    // convex.)
    for (const QRect& c : a_clip) {
        QPolygonF poly = polygon;
        const QRectF box(c);
        const auto clipEdge = [](const QPolygonF& in, auto inside, auto cross) {
            QPolygonF out;
            for (qsizetype k = 0; k < in.size(); ++k) {
                const QPointF a = in[k], b = in[(k + 1) % in.size()];
                const bool ia = inside(a), ib = inside(b);
                if (ia) out << a;
                if (ia != ib) out << cross(a, b);
            }
            return out;
        };
        const auto atX = [](QPointF a, QPointF b, qreal x) { return QPointF(x, a.y() + (b.y() - a.y()) * (x - a.x()) / (b.x() - a.x())); };
        const auto atY = [](QPointF a, QPointF b, qreal y) { return QPointF(a.x() + (b.x() - a.x()) * (y - a.y()) / (b.y() - a.y()), y); };
        poly = clipEdge(poly, [&](QPointF p) { return p.x() >= box.left(); }, [&](QPointF a, QPointF b) { return atX(a, b, box.left()); });
        poly = clipEdge(poly, [&](QPointF p) { return p.x() <= box.right(); }, [&](QPointF a, QPointF b) { return atX(a, b, box.right()); });
        poly = clipEdge(poly, [&](QPointF p) { return p.y() >= box.top(); }, [&](QPointF a, QPointF b) { return atY(a, b, box.top()); });
        poly = clipEdge(poly, [&](QPointF p) { return p.y() <= box.bottom(); }, [&](QPointF a, QPointF b) { return atY(a, b, box.bottom()); });
        addTriangles(poly, color);
    }
}

void Canvas2D::drawImage(const QRectF& target, const QImage& image, const QRectF& source)
{
    if (image.isNull() || target.isEmpty()) return;
    const QRectF from = source.isEmpty() ? QRectF(image.rect()) : source;
    const auto add = [this, &image](const QRectF& t, const QRectF& s) {
        Op op;
        op.image = image;
        op.target = t;
        op.source = s;
        a_ops.push_back(std::move(op));
    };
    if (a_clip.isEmpty()) {
        add(target, from);
        return;
    }
    // (Each clip rectangle's part, the source's part with it.)
    const qreal sx = from.width() / target.width(), sy = from.height() / target.height();
    for (const QRect& c : a_clip) {
        const QRectF piece = target.intersected(QRectF(c));
        if (piece.isEmpty()) continue;
        add(piece, QRectF(from.left() + (piece.left() - target.left()) * sx, from.top() + (piece.top() - target.top()) * sy, piece.width() * sx,
                          piece.height() * sy));
    }
}

void Canvas2D::drawImage(const QPointF& at, const QImage& image)
{
    drawImage(QRectF(at, QSizeF(image.size()) / image.devicePixelRatio()), image);
}

// ----------------------------------------------------------------------
// Viewport2D

namespace {

// The solid fills' uniforms: the clip space's correction alone.
struct SolidUniforms {
    float corr[16];
};

// The viewports told of images let go, and the lock of their lists.
QMutex goneLock;
std::vector<Viewport2D*>& viewports()
{
    static std::vector<Viewport2D*> all;
    return all;
}

} // namespace

void Viewport2D::imageGone(qint64 key)
{
    QMutexLocker lock(&goneLock);
    for (Viewport2D* v : viewports()) v->a_gone.push_back(key);
}

Viewport2D::Viewport2D(std::function<void(Canvas2D&)> paint) : a_paint(std::move(paint))
{
    {
        static const bool hooked = [] {
            QImagePixmapCleanupHooks::instance()->addImageHook(&Viewport2D::imageGone);
            return true;
        }();
        Q_UNUSED(hooked);
        QMutexLocker lock(&goneLock);
        viewports().push_back(this);
    }
    connect(this, &QRhiWidget::renderFailed, this, [this] {
        if (a_failed || !missed(this, a_misses)) return;
        qWarning("GPU viewport: no backend for Qt's RHI here");
        a_failed = true;
    });
}

void Viewport2D::takePaints()
{
    installEventFilter(this);
    a_takesPaints = true;
    // (Its size told now - the resizes before passed over -, and painted;
    // not shown - in no window yet, its QRhi the window's -, as it is.)
    if (isVisible()) {
        QResizeEvent resized(size(), size());
        QRhiWidget::resizeEvent(&resized);
    }
}

bool Viewport2D::event(QEvent* event)
{
    // (Shown, QRhiWidget paints at once - as resized, below.)
    if (event->type() == QEvent::Show && !a_takesPaints) return QWidget::event(event);
    return QRhiWidget::event(event);
}

void Viewport2D::resizeEvent(QResizeEvent* event)
{
    // (QRhiWidget paints at once when resized: placed in its area by
    // setViewport, before its paints are its own, the area's painting -
    // QPainter's, on a widget it cannot paint - would have it. Painted a
    // moment after.)
    if (!a_takesPaints && !event->size().isEmpty()) {
        update();
        return;
    }
    QRhiWidget::resizeEvent(event);
}

Viewport2D::~Viewport2D()
{
    {
        QMutexLocker lock(&goneLock);
        std::erase(viewports(), this);
    }
    dropAll();
}

void Viewport2D::dropAll()
{
    a_textures.clear();
    a_textureBytes = 0;
    a_solid.reset();
    a_image.reset();
    a_solidBindings.reset();
    a_imageLayout.reset();
    a_smooth.reset();
    a_uniforms.reset();
    a_vertices.reset();
    a_rhi = nullptr;
}

void Viewport2D::releaseResources()
{
    dropAll();
}

bool Viewport2D::eventFilter(QObject* watched, QEvent* event)
{
    // (Its paints its own: the area's viewportEvent would paint it with
    // QPainter - not on an RHI widget.)
    if (watched != this || event->type() != QEvent::Paint) return false;
    QRhiWidget::paintEvent(static_cast<QPaintEvent*>(event));
    return true;
}

void Viewport2D::initialize(QRhiCommandBuffer*)
{
    if (a_rhi != rhi()) {
        dropAll();
        a_rhi = rhi();
    }
    if (!a_rhi || a_solid) return;
    // (One uniform buffer, two parts: the solid fills' and the images' - a
    // full composite's, its opacity 1.)
    const int part = int(a_rhi->ubufAligned(sizeof(CompositeUniforms)));
    a_uniforms.reset(a_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, quint32(2 * part)));
    a_uniforms->create();
    a_smooth.reset(a_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge));
    a_smooth->create();
    a_solidBindings.reset(a_rhi->newShaderResourceBindings());
    a_solidBindings->setBindings(
        {QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, a_uniforms.get(), 0, sizeof(SolidUniforms))});
    a_solidBindings->create();
    std::unique_ptr<QRhiTexture> any(a_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    any->create();
    a_imageLayout.reset(a_rhi->newShaderResourceBindings());
    a_imageLayout->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                                         a_uniforms.get(), quint32(part), sizeof(CompositeUniforms)),
                                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, any.get(), a_smooth.get())});
    a_imageLayout->create();
    QRhiVertexInputLayout solidInput;
    solidInput.setBindings({QRhiVertexInputBinding(6 * sizeof(float))});
    solidInput.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                              QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, 2 * sizeof(float))});
    PipelineSpec solid{QStringLiteral("solid.vert"), QStringLiteral("solid.frag"), solidInput, QRhiGraphicsPipeline::Triangles, true};
    a_solid = makePipeline(a_rhi, renderTarget()->renderPassDescriptor(), sampleCount(), a_solidBindings.get(), solid);
    PipelineSpec image{QStringLiteral("composite.vert"), QStringLiteral("composite.frag"), imageInput(), QRhiGraphicsPipeline::TriangleStrip, true};
    a_image = makePipeline(a_rhi, renderTarget()->renderPassDescriptor(), sampleCount(), a_imageLayout.get(), image);
    if (!a_solid || !a_image) a_failed = true;
}

void Viewport2D::render(QRhiCommandBuffer* cb)
{
    if (a_failed || !a_rhi || !a_solid) return;
    // Textures of images let go since: dropped.
    {
        QMutexLocker lock(&goneLock);
        for (const qint64 key : a_gone) {
            const auto it = a_textures.find(key);
            if (it == a_textures.end()) continue;
            a_textureBytes -= it->second.bytes;
            a_textures.erase(it);
        }
        a_gone.clear();
    }
    const QSize px = renderTarget()->pixelSize();
    Canvas2D canvas(size(), devicePixelRatioF());
    if (a_paint) a_paint(canvas);
    QRhiResourceUpdateBatch* u = a_rhi->nextResourceUpdateBatch();
    // The uniforms: the correction, an opacity of 1.
    const int part = int(a_rhi->ubufAligned(sizeof(CompositeUniforms)));
    SolidUniforms su{};
    store(su.corr, a_rhi->clipSpaceCorrMatrix());
    u->updateDynamicBuffer(a_uniforms.get(), 0, sizeof(su), &su);
    CompositeUniforms cu{};
    store(cu.corr, a_rhi->clipSpaceCorrMatrix());
    cu.opacity = 1.0f;
    u->updateDynamicBuffer(a_uniforms.get(), quint32(part), sizeof(cu), &cu);
    // The vertices, all the frame's in one buffer: the fills' triangles in
    // clip space, each image's quad.
    const double w = std::max(1, width()), h = std::max(1, height());
    const auto clipX = [w](double x) { return float(x / w * 2 - 1); };
    const auto clipY = [h](double y) { return float(1 - y / h * 2); };
    std::vector<float> data;
    struct Draw {
        qsizetype offset;   // bytes
        int count;          // vertices
        qint64 texture = 0; // an image's cache key, else a fill
    };
    std::vector<Draw> draws;
    ++a_frames;
    for (const Canvas2D::Op& op : canvas.ops()) {
        if (op.image.isNull()) {
            Draw d{qsizetype(data.size() * sizeof(float)), int(op.fill.size() / 6), 0};
            for (std::size_t i = 0; i < op.fill.size(); i += 6)
                data.insert(data.end(),
                            {clipX(op.fill[i]), clipY(op.fill[i + 1]), op.fill[i + 2], op.fill[i + 3], op.fill[i + 4], op.fill[i + 5]});
            if (d.count > 0) draws.push_back(d);
            continue;
        }
        const qint64 key = op.image.cacheKey();
        auto it = a_textures.find(key);
        if (it == a_textures.end()) {
            Texture t;
            const QImage rgba = op.image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            t.texture.reset(a_rhi->newTexture(QRhiTexture::RGBA8, rgba.size()));
            if (!t.texture->create()) continue;
            u->uploadTexture(t.texture.get(), rgba);
            ++a_uploads;
            t.bindings.reset(a_rhi->newShaderResourceBindings());
            t.bindings->setBindings({QRhiShaderResourceBinding::uniformBuffer(
                                         0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, a_uniforms.get(),
                                         quint32(part), sizeof(CompositeUniforms)),
                                     QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t.texture.get(),
                                                                               a_smooth.get())});
            t.bindings->create();
            t.bytes = rgba.sizeInBytes();
            a_textureBytes += t.bytes;
            // (Told when the image is let go.)
            QImagePixmapCleanupHooks::enableCleanupHooks(op.image);
            it = a_textures.emplace(key, std::move(t)).first;
        }
        it->second.frame = a_frames;
        const QSizeF size = op.image.size();
        const QRectF& s = op.source;
        const float u0 = float(s.left() / size.width()), u1 = float(s.right() / size.width());
        const float v0 = float(s.top() / size.height()), v1 = float(s.bottom() / size.height());
        const QRectF& t = op.target;
        Draw d{qsizetype(data.size() * sizeof(float)), 4, key};
        data.insert(data.end(), {clipX(t.left()), clipY(t.bottom()), u0, v1, clipX(t.right()), clipY(t.bottom()), u1, v1, clipX(t.left()), clipY(t.top()),
                                 u0, v0, clipX(t.right()), clipY(t.top()), u1, v0});
        draws.push_back(d);
    }
    // (One buffer, grown as it must be.)
    const quint32 bytes = quint32(std::max<std::size_t>(data.size() * sizeof(float), 64));
    if (!a_vertices || a_vertices->size() < bytes) {
        a_vertices.reset(a_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, std::max<quint32>(bytes, 64 * 1024)));
        a_vertices->create();
    }
    if (!data.empty()) u->updateDynamicBuffer(a_vertices.get(), 0, quint32(data.size() * sizeof(float)), data.data());
    cb->beginPass(renderTarget(), palette().color(backgroundRole()), {1.0f, 0}, u);
    const QRhiViewport viewport(0, 0, float(px.width()), float(px.height()));
    for (const Draw& d : draws) {
        const QRhiCommandBuffer::VertexInput input(a_vertices.get(), quint32(d.offset));
        if (d.texture == 0) {
            cb->setGraphicsPipeline(a_solid.get());
            cb->setViewport(viewport);
            cb->setShaderResources(a_solidBindings.get());
        } else {
            cb->setGraphicsPipeline(a_image.get());
            cb->setViewport(viewport);
            cb->setShaderResources(a_textures.at(d.texture).bindings.get());
        }
        cb->setVertexInput(0, 1, &input);
        cb->draw(quint32(d.count));
    }
    cb->endPass();
    a_misses = 0;
    // Textures not drawn for a while, or past some 384 MB, the oldest:
    // dropped.
    constexpr qsizetype kBudget = 384LL * 1024 * 1024;
    std::vector<std::pair<qint64, qint64>> old;   // frame, key
    for (const auto& [key, t] : a_textures) old.push_back({t.frame, key});
    std::sort(old.begin(), old.end());
    for (const auto& [frame, key] : old) {
        if (frame >= a_frames) break;
        if (a_frames - frame < 120 && a_textureBytes <= kBudget) break;
        a_textureBytes -= a_textures.at(key).bytes;
        a_textures.erase(key);
    }
}

} // namespace qucs_s::gpu
