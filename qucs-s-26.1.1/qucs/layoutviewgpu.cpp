/*
 * layoutviewgpu.cpp - the layout viewer drawn on the GPU (Qt's RHI)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutviewgpu.h"

#include "layoutdoc.h"

#include <CDT.h>

#include <QFontMetricsF>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qucs_s::layout {

namespace {

// The shaders' uniforms (layout_shape.vert, layout_dot.vert): a draw's.
struct Uniforms {
    float corr[16];
    float color[4];   // straight alpha
    float center[2];
    float scale[2];
    float nudge[2];
    float viewport[2];   // device pixels
    float pixels;        // device pixels a µm
    float cell;          // a pattern's pixel in device pixels
    float patterned;
    float yUp;
};

// A dot's quad: a strip, a device pixel square about it.
const float kCorners[] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};

/// Convex - each turn the same way, once round: fanned.
bool convex(const std::vector<QPointF>& q)
{
    const std::size_t n = q.size();
    if (n < 4) return true;
    int sign = 0;
    double turned = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const QPointF a = q[k], b = q[(k + 1) % n], c = q[(k + 2) % n];
        const QPointF u = b - a, v = c - b;
        const double cross = u.x() * v.y() - u.y() * v.x();
        const double scaleOf = std::hypot(u.x(), u.y()) * std::hypot(v.x(), v.y());
        if (std::abs(cross) <= 1e-12 * scaleOf) continue;
        const int s = cross > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        else if (s != sign) return false;
        turned += std::atan2(cross, u.x() * v.x() + u.y() * v.y());
    }
    return std::abs(turned) < 2 * M_PI + 0.1;
}

void fan(const std::vector<QPointF>& q, std::vector<float>& out)
{
    for (std::size_t k = 1; k + 1 < q.size(); ++k)
        for (const QPointF& p : {q[0], q[k], q[k + 1]}) {
            out.push_back(float(p.x()));
            out.push_back(float(p.y()));
        }
}

/// Odd-even, as QPainter fills a polygon: its outline's edges constrained,
/// the triangles outside and in its holes taken away (a GDSII hole's slit
/// crossed twice).
bool triangulate(const std::vector<QPointF>& q, std::vector<float>& out)
{
    double x0 = q[0].x(), x1 = x0, y0 = q[0].y(), y1 = y0;
    for (const QPointF& p : q) {
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    }
    const double diag = std::hypot(x1 - x0, y1 - y0);
    if (!(diag > 0)) return true;
    try {
        CDT::Triangulation<double> cdt(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, diag * 1e-12);
        std::vector<CDT::V2d<double>> vertices;
        vertices.reserve(q.size());
        for (const QPointF& p : q) vertices.emplace_back(p.x(), p.y());
        std::vector<CDT::Edge> edges;
        for (std::size_t k = 0; k < q.size(); ++k) edges.emplace_back(CDT::VertInd(k), CDT::VertInd((k + 1) % q.size()));
        CDT::RemoveDuplicatesAndRemapEdges(vertices, edges);
        std::vector<CDT::Edge> kept;
        for (const CDT::Edge& e : edges)
            if (e.v1() != e.v2()) kept.push_back(e);
        if (vertices.size() < 3) return true;
        cdt.insertVertices(vertices);
        cdt.insertEdges(kept);
        cdt.eraseOuterTrianglesAndHoles();
        for (const CDT::Triangle& t : cdt.triangles)
            for (int k = 0; k < 3; ++k) {
                const CDT::V2d<double>& v = cdt.vertices[t.vertices[std::size_t(k)]];
                out.push_back(float(v.x));
                out.push_back(float(v.y));
            }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

Flattened flatten(const Layout& layout, int cell, int depth, const QVector<LayerStyle>& styles, bool labels, quint64 budget)
{
    Flattened f;
    if (cell < 0 || cell >= layout.cells.size()) return f;
    f.origin = layout.cells.at(cell).bounds.center();
    f.layers.resize(std::size_t(layout.layers.size()));
    const QPointF o = f.origin;
    const auto visible = [&styles](quint32 layer) { return int(layer) >= styles.size() || styles.at(int(layer)).visible; };
    std::vector<QPointF> q;
    // One copy of a shape: its outline, its fill, its dot.
    const auto put = [&](const Cell& c, const Shape& sh, const QTransform& T, QPointF off, double s) {
        Flattened::Layer& out = f.layers[std::size_t(sh.layer)];
        q.clear();
        const QPointF* p = c.points.constData() + sh.first;
        for (quint32 k = 0; k < sh.count; ++k) {
            const QPointF at = T.map(p[k] + off) - o;
            if (q.empty() || at != q.back()) q.push_back(at);
        }
        while (q.size() > 1 && q.front() == q.back()) q.pop_back();
        const QPointF mid = T.map(sh.bounds.center() + off) - o;
        out.dots.insert(out.dots.end(), {float(mid.x()), float(mid.y()), float(std::max(sh.bounds.width(), sh.bounds.height()) * s)});
        if (q.size() < 2) return;
        for (std::size_t k = 0; k < q.size(); ++k) {
            const QPointF a = q[k], b = q[(k + 1) % q.size()];
            out.outlines.insert(out.outlines.end(), {float(a.x()), float(a.y()), float(b.x()), float(b.y())});
        }
        if (q.size() < 3) return;
        if (convex(q)) fan(q, out.fills);
        else if (!triangulate(q, out.fills)) {
            fan(q, out.fills);
            ++f.approximated;
        }
    };
    std::function<void(int, const QTransform&, int)> visit = [&](int ci, const QTransform& T, int level) {
        const Cell& c = layout.cells.at(ci);
        const double s = std::sqrt(std::abs(T.determinant()));
        for (const Shape& sh : c.shapes) {
            if (f.tooLarge) return;
            if (!visible(sh.layer)) continue;
            if (sh.repetition < 0) {
                if (++f.shapes > budget) f.tooLarge = true;
                else put(c, sh, T, QPointF(), s);
                continue;
            }
            c.repetitions.at(sh.repetition).forEach(sh.bounds, QRectF(), [&](QPointF off) {
                if (++f.shapes > budget) {
                    f.tooLarge = true;
                    return false;
                }
                put(c, sh, T, off, s);
                return true;
            });
        }
        if (labels)
            for (const Label& l : c.labels) {
                if (!visible(l.layer) || f.texts.size() >= 100000) continue;
                const auto text = [&](QPointF off) {
                    f.texts.append({T.map(l.origin + off), l.text});
                    f.textLayers.push_back(l.layer);
                    return f.texts.size() < 100000;
                };
                if (l.repetition < 0) text(QPointF());
                else c.repetitions.at(l.repetition).forEach(QRectF(l.origin, QSizeF(0, 0)), QRectF(), text);
            }
        for (const Instance& inst : c.instances) {
            if (f.tooLarge) return;
            if (inst.cell < 0) continue;
            const Cell& child = layout.cells.at(inst.cell);
            if (child.bounds.isNull() && child.shapes.isEmpty() && child.labels.isEmpty() && child.instances.isEmpty()) continue;
            const QTransform it = inst.transform();
            const QRectF one = it.mapRect(child.bounds);
            const auto place = [&](QPointF off) -> bool {
                if (f.tooLarge) return false;
                const QTransform ct = it * QTransform::fromTranslate(off.x(), off.y()) * T;
                if (level + 1 > depth) {
                    if (++f.shapes > budget) {
                        f.tooLarge = true;
                        return false;
                    }
                    const QPolygonF frame = ct.map(QPolygonF(child.bounds));
                    for (int k = 0; k < frame.size() - 1; ++k) {
                        const QPointF a = frame.at(k) - o, b = frame.at(k + 1) - o;
                        f.frames.insert(f.frames.end(), {float(a.x()), float(a.y()), float(b.x()), float(b.y())});
                    }
                    const QPointF mid = T.map(one.center() + off) - o;
                    f.frameDots.insert(f.frameDots.end(), {float(mid.x()), float(mid.y()), float(std::max(one.width(), one.height()) * s)});
                    f.names.append({frame.boundingRect(), child.name});
                    ++f.frameCount;
                    return true;
                }
                visit(inst.cell, ct, level + 1);
                return !f.tooLarge;
            };
            if (inst.repetition < 0) place(QPointF());
            else c.repetitions.at(inst.repetition).forEach(one, QRectF(), place);
        }
    };
    visit(cell, QTransform(), 0);
    return f;
}

// ----------------------------------------------------------------------

LayoutGpu::LayoutGpu(LayoutView* view) : gpu::LayeredCanvas(view, 1), a_view(view)
{
    // (No multisampling: QPainter draws the layout without antialiasing.)
    setObjectName(QStringLiteral("layoutCanvas"));
}

LayoutGpu::~LayoutGpu()
{
    releaseNativeResources();
}

void LayoutGpu::paintFrame(QPainter& p)
{
    a_view->paintFrame(p, this);
}

void LayoutGpu::canvasFailed()
{
    QMetaObject::invokeMethod(a_view, [v = a_view] { v->canvasFailed(); }, Qt::QueuedConnection);
}

void LayoutGpu::releaseNativeResources()
{
    a_layers.clear();
    a_frameLines = Batch();
    a_frameDots = Batch();
    a_bindings.clear();
    a_patterns.clear();
    a_fill.reset();
    a_line.reset();
    a_dot.reset();
    a_layoutBindings.reset();
    a_plain.reset();
    a_nearest.reset();
    a_corners.reset();
    a_uniforms.reset();
    a_slots = 0;
    a_rhi = nullptr;
    // (Made again whole with the next frame's resources.)
    a_madeOf = nullptr;
    a_cell = -2;
}

void LayoutGpu::createResources(QRhi* rhi)
{
    a_rhi = rhi;
    a_slots = 64;
    a_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, quint32(a_slots * rhi->ubufAligned(sizeof(Uniforms)))));
    a_uniforms->create();
    a_corners.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kCorners)));
    a_corners->create();
    updates()->uploadStaticBuffer(a_corners.get(), kCorners);
    a_nearest.reset(rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::Repeat, QRhiSampler::Repeat));
    a_nearest->create();
    a_plain.reset(rhi->newTexture(QRhiTexture::R8, QSize(1, 1)));
    a_plain->create();
    QImage one(1, 1, QImage::Format_Grayscale8);
    one.fill(255);
    updates()->uploadTexture(a_plain.get(), one);
    a_layoutBindings.reset(rhi->newShaderResourceBindings());
    a_layoutBindings->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                                       0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, a_uniforms.get(),
                                       sizeof(Uniforms)),
                                   QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, a_plain.get(), a_nearest.get())});
    a_layoutBindings->create();
    QRhiVertexInputLayout shapes;
    shapes.setBindings({QRhiVertexInputBinding(2 * sizeof(float))});
    shapes.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)});
    QRhiVertexInputLayout dots;
    dots.setBindings({QRhiVertexInputBinding(2 * sizeof(float)), QRhiVertexInputBinding(3 * sizeof(float), QRhiVertexInputBinding::PerInstance)});
    dots.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0), QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float3, 0)});
    const QString vert = QStringLiteral("layout_shape.vert"), frag = QStringLiteral("layout_shape.frag");
    a_fill = gpu::makePipeline(rhi, pass(), samples(), a_layoutBindings.get(), {vert, frag, shapes, QRhiGraphicsPipeline::Triangles, false});
    a_line = gpu::makePipeline(rhi, pass(), samples(), a_layoutBindings.get(), {vert, frag, shapes, QRhiGraphicsPipeline::Lines, false});
    a_dot = gpu::makePipeline(rhi, pass(), samples(), a_layoutBindings.get(),
                              {QStringLiteral("layout_dot.vert"), QStringLiteral("layout_dot.frag"), dots, QRhiGraphicsPipeline::TriangleStrip, false});
    if (!a_fill || !a_line || !a_dot) {
        qWarning("Layout view: the GPU's pipelines not made; drawn by the CPU");
        canvasFailed();
    }
    a_madeOf = nullptr;
    a_cell = -2;
}

QRhiShaderResourceBindings* LayoutGpu::bindingsFor(QRhiTexture* pattern)
{
    auto it = a_bindings.find(pattern);
    if (it != a_bindings.end()) return it->get();
    auto b = std::shared_ptr<QRhiShaderResourceBindings>(a_rhi->newShaderResourceBindings());
    b->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                                                                               a_uniforms.get(), sizeof(Uniforms)),
                    QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, pattern, a_nearest.get())});
    b->create();
    a_bindings.insert(pattern, b);
    return b.get();
}

void LayoutGpu::upload(Batch& b, const std::vector<float>& data, int components)
{
    b.count = int(data.size()) / components;
    if (data.empty()) {
        b.buffer.reset();
        return;
    }
    b.buffer.reset(a_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(data.size() * sizeof(float))));
    b.buffer->create();
    updates()->uploadStaticBuffer(b.buffer.get(), data.data());
}

void LayoutGpu::sync()
{
    // (The buffers made of the cell to its depth and the texts: made again
    // when one changes - not for colours or patterns. A layer hidden keeps
    // its buffers, not drawn; one shown for the first time flattened alone,
    // its buffers added. Made again whole at each layer shown or hidden, as
    // they were, a large chip's layer took seconds to show or hide.)
    const Layout* layout = a_view->a_layout.get();
    const QVector<LayerStyle>& styles = a_view->a_styles;
    const auto shown = [&styles](std::size_t i) { return int(i) >= styles.size() || styles.at(int(i)).visible; };
    // (QUCS_LAYOUT_GPU_BUDGET another budget - the tests' small one.)
    bool ok = false;
    const quint64 asked = qEnvironmentVariable("QUCS_LAYOUT_GPU_BUDGET").toULongLong(&ok);
    const quint64 budget = ok ? asked : Budget;
    const bool same = layout == a_madeOf && a_view->a_layoutGeneration == a_generation && a_view->a_cell == a_cell && a_view->a_depth == a_depth
                      && a_view->a_labels == a_labels;
    std::vector<char> shownNow(layout ? layout->layers.size() : 0);
    for (std::size_t i = 0; i < shownNow.size(); ++i) shownNow[i] = shown(i) ? 1 : 0;
    // Past the budget (the CPU draws it): made again only when the layers
    // shown change - one hidden may bring it under. (Made again at every
    // frame, a chip of more than 2 million shapes was flattened that far
    // each time before the CPU drew it.)
    if (same && layout && a_flat.tooLarge && shownNow == a_shown) return;
    if (same && layout && !a_flat.tooLarge) {
        // (Every layer's flag its own: past the list flatten takes a layer
        // as shown.)
        QVector<LayerStyle> more = styles;
        more.resize(layout->layers.size());
        bool any = false;
        for (std::size_t i = 0; i < std::size_t(more.size()); ++i) {
            const bool wanted = shown(i) && (i >= a_made.size() || !a_made[i]);
            more[int(i)].visible = wanted;
            any = any || wanted;
        }
        if (!any) return;
        // (Only those layers, within what the budget leaves; past it, all
        // made again of the layers shown - the CPU's where they are many.)
        Flattened added = flatten(*layout, a_cell, a_depth, more, a_labels, budget - std::min(budget, a_flat.shapes));
        if (!added.tooLarge) {
            a_flat.shapes += added.shapes;
            a_flat.approximated += added.approximated;
            for (std::size_t i = 0; i < added.layers.size() && i < a_flat.layers.size(); ++i) {
                if (int(i) >= more.size() || !more.at(int(i)).visible) continue;
                a_flat.layers[i] = std::move(added.layers[i]);
                if (i >= a_made.size()) a_made.resize(i + 1, 0);
                a_made[i] = 1;
                while (a_layers.size() <= i) a_layers.push_back(std::make_unique<LayerBatches>());
                upload(a_layers[i]->fills, a_flat.layers[i].fills, 2);
                upload(a_layers[i]->outlines, a_flat.layers[i].outlines, 2);
                upload(a_layers[i]->dots, a_flat.layers[i].dots, 3);
                // (The CPU's arrays freed, as a whole build's: the GPU has
                // them.)
                std::vector<float>().swap(a_flat.layers[i].fills);
                std::vector<float>().swap(a_flat.layers[i].outlines);
                std::vector<float>().swap(a_flat.layers[i].dots);
            }
            for (int k = 0; k < added.texts.size(); ++k) {
                if (a_flat.texts.size() >= 100000) break;
                a_flat.texts.append(added.texts.at(k));
                a_flat.textLayers.push_back(added.textLayers[std::size_t(k)]);
            }
            ++a_layersAdded;
            return;
        }
    }
    a_madeOf = layout;
    a_generation = a_view->a_layoutGeneration;
    a_shown = shownNow;
    a_cell = a_view->a_cell;
    a_depth = a_view->a_depth;
    a_labels = a_view->a_labels;
    a_flat = layout ? flatten(*layout, a_cell, a_depth, styles, a_labels, budget) : Flattened();
    a_made.assign(a_flat.layers.size(), 0);
    for (std::size_t i = 0; i < a_made.size(); ++i) a_made[i] = shown(i) ? 1 : 0;
    ++a_builds;
    while (a_layers.size() < a_flat.layers.size()) a_layers.push_back(std::make_unique<LayerBatches>());
    for (std::size_t i = 0; i < a_layers.size(); ++i) {
        static const std::vector<float> none;
        const bool has = i < a_flat.layers.size();
        upload(a_layers[i]->fills, has ? a_flat.layers[i].fills : none, 2);
        upload(a_layers[i]->outlines, has ? a_flat.layers[i].outlines : none, 2);
        upload(a_layers[i]->dots, has ? a_flat.layers[i].dots : none, 3);
    }
    upload(a_frameLines, a_flat.frames, 2);
    upload(a_frameDots, a_flat.frameDots, 3);
    // (The CPU's arrays freed: the GPU has them.)
    for (Flattened::Layer& l : a_flat.layers) {
        std::vector<float>().swap(l.fills);
        std::vector<float>().swap(l.outlines);
        std::vector<float>().swap(l.dots);
    }
    std::vector<float>().swap(a_flat.frames);
    std::vector<float>().swap(a_flat.frameDots);
}

QRhiTexture* LayoutGpu::patternTexture(Qt::BrushStyle style)
{
    if (auto it = a_patterns.constFind(int(style)); it != a_patterns.constEnd()) return it->get();
    // Qt's own bitmap: its brush drawn on 8 by 8 pixels.
    QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.fillRect(image.rect(), QBrush(Qt::black, style));
    }
    QImage bits(8, 8, QImage::Format_Grayscale8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) bits.scanLine(y)[x] = qAlpha(image.pixel(x, y)) > 127 ? 255 : 0;
    auto t = std::shared_ptr<QRhiTexture>(a_rhi->newTexture(QRhiTexture::R8, QSize(8, 8)));
    t->create();
    updates()->uploadTexture(t.get(), bits);
    a_patterns.insert(int(style), t);
    return t.get();
}

std::size_t LayoutGpu::heldFloats() const
{
    std::size_t n = a_flat.frames.size() + a_flat.frameDots.size();
    for (const Flattened::Layer& l : a_flat.layers) n += l.fills.size() + l.outlines.size() + l.dots.size();
    return n;
}

bool LayoutGpu::drawLayout(QPainter& p, RenderStats* stats)
{
    if (!a_rhi || !a_fill) return false;
    sync();
    if (a_flat.tooLarge) return false;
    const LayoutView::Colors c = a_view->colors();
    const double ratio = devicePixelRatioF();
    const double s = a_view->a_scale;
    const QPointF center = a_view->a_center - a_flat.origin;
    const QSize px = pixels();
    const QMatrix4x4 corrMatrix = corr();
    const float up = yUp() ? 1.0f : 0.0f;
    const bool openGl = a_rhi->backend() == QRhi::OpenGLES2;
    // The frame's draws, a slot of uniforms each.
    struct Draw {
        QRhiGraphicsPipeline* pipeline;
        QRhiBuffer* buffer;
        int count;
        QRhiTexture* pattern;
        Uniforms u;
    };
    std::vector<Draw> draws;
    // (Its captures named: GCC 12 and 13 crash on long lambdas capturing all.)
    const auto uniforms = [this, &corrMatrix, &center, s, ratio, &px, up, openGl](QColor k, bool nudged, bool patterned) {
        Uniforms u{};
        std::memcpy(u.corr, corrMatrix.constData(), sizeof(u.corr));
        u.color[0] = float(k.redF());
        u.color[1] = float(k.greenF());
        u.color[2] = float(k.blueF());
        u.color[3] = float(k.alphaF());
        u.center[0] = float(center.x());
        u.center[1] = float(center.y());
        u.scale[0] = float(2 * s / std::max(1, width()));
        u.scale[1] = float(2 * s / std::max(1, height()));
        // (A little right, in clip space, so that an edge on a pixel's
        // boundary is inked on the pixel QPainter inks: on OpenGL half a
        // device pixel, down none; on Metal and Direct3D, which ink a line's
        // pixels as QPainter does otherwise, a 64th right and down - their
        // ties alone. Half a pixel there put every vertical outline a pixel
        // right of the CPU's.)
        const double bias = openGl ? 0.5 : 1.0 / 64;
        u.nudge[0] = nudged ? float(2.0 * bias / std::max(1, px.width())) : 0.0f;
        u.nudge[1] = nudged && !openGl ? float(-2.0 * bias / std::max(1, px.height())) : 0.0f;
        u.viewport[0] = float(px.width());
        u.viewport[1] = float(px.height());
        u.pixels = float(s * ratio);
        // (a pattern's pixel a device pixel, as QPainter has it on a
        // picture of the screen's pixel ratio)
        u.cell = 1.0f;
        u.patterned = patterned ? 1.0f : 0.0f;
        u.yUp = up;
        return u;
    };
    for (std::size_t i = 0; i < a_layers.size(); ++i) {
        LayerBatches& l = *a_layers[i];
        if (l.fills.count == 0 && l.outlines.count == 0 && l.dots.count == 0) continue;
        const LayerStyle raw = int(i) < a_view->a_styles.size() ? a_view->a_styles.at(int(i)) : paletteStyle(int(i));
        if (!raw.visible) continue;
        const LayerStyle style = a_view->shownStyle(raw);
        if (style.pattern != Qt::NoBrush && l.fills.count > 0) {
            const bool patterned = style.pattern != Qt::SolidPattern;
            draws.push_back({a_fill.get(), l.fills.buffer.get(), l.fills.count, patterned ? patternTexture(style.pattern) : a_plain.get(),
                             uniforms(style.fill, false, patterned)});
        }
        if (l.outlines.count > 0) draws.push_back({a_line.get(), l.outlines.buffer.get(), l.outlines.count, a_plain.get(), uniforms(style.frame, true, false)});
        if (l.dots.count > 0) draws.push_back({a_dot.get(), l.dots.buffer.get(), l.dots.count, a_plain.get(), uniforms(style.frame, true, false)});
    }
    const QColor frameColor = c.frames.isValid() ? c.frames : QColor(Qt::gray);
    if (a_frameLines.count > 0) draws.push_back({a_line.get(), a_frameLines.buffer.get(), a_frameLines.count, a_plain.get(), uniforms(frameColor, true, false)});
    if (a_frameDots.count > 0) draws.push_back({a_dot.get(), a_frameDots.buffer.get(), a_frameDots.count, a_plain.get(), uniforms(frameColor, true, false)});
    // The uniforms' slots, the buffer grown as it must be (its bindings
    // made again with it).
    const quint32 slot = a_rhi->ubufAligned(sizeof(Uniforms));
    if (int(draws.size()) > a_slots) {
        a_slots = int(draws.size()) * 2;
        a_uniforms.reset(a_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, quint32(a_slots) * slot));
        a_uniforms->create();
        a_bindings.clear();
    }
    std::vector<QRhiShaderResourceBindings*> bindings;
    for (std::size_t k = 0; k < draws.size(); ++k) {
        updates()->updateDynamicBuffer(a_uniforms.get(), quint32(k) * slot, sizeof(Uniforms), &draws[k].u);
        bindings.push_back(bindingsFor(draws[k].pattern));
    }
    QRhiBuffer* corners = a_corners.get();
    const QRhiViewport viewport = fullViewport();
    native(p, [draws, bindings, slot, corners, viewport](QRhiCommandBuffer* cb) {
        for (std::size_t k = 0; k < draws.size(); ++k) {
            const Draw& d = draws[k];
            cb->setGraphicsPipeline(d.pipeline);
            cb->setViewport(viewport);
            const QRhiCommandBuffer::DynamicOffset offset(0, quint32(k) * slot);
            cb->setShaderResources(bindings[k], 1, &offset);
            if (d.pipeline->topology() == QRhiGraphicsPipeline::TriangleStrip) {
                const QRhiCommandBuffer::VertexInput inputs[] = {{corners, 0}, {d.buffer, 0}};
                cb->setVertexInput(0, 2, inputs);
                cb->draw(4, quint32(d.count));
            } else {
                const QRhiCommandBuffer::VertexInput input(d.buffer, 0);
                cb->setVertexInput(0, 1, &input);
                cb->draw(quint32(d.count));
            }
        }
    });
    // The frames' names and the texts, as the CPU draws them, those in
    // sight.
    const QRectF seen = a_view->visibleRegion();
    if (!a_flat.names.isEmpty()) {
        QFont font = p.font();
        font.setPixelSize(11);
        p.setFont(font);
        p.setPen(frameColor);
        const QFontMetricsF fm(font);
        for (const auto& [box, name] : std::as_const(a_flat.names)) {
            if (!box.intersects(seen)) continue;
            const QRectF on = QRectF(a_view->toPixel(box.topLeft()), a_view->toPixel(box.bottomRight())).normalized();
            if (std::max(on.width(), on.height()) <= 48) continue;
            const QString shown = fm.elidedText(name, Qt::ElideRight, on.width() - 6);
            if (shown.size() < 2 || on.height() < fm.height() + 4) continue;
            p.drawText(on, Qt::AlignCenter, shown);
        }
    }
    int texts = 0;
    if (!a_flat.texts.isEmpty()) {
        QFont font = p.font();
        font.setPixelSize(10);
        p.setFont(font);
        // (a device pixel wide, as the CPU's picture has its crosses)
        QPen ink(c.ink.isValid() ? c.ink : QColor(Qt::black), 0);
        ink.setCosmetic(true);
        p.setPen(ink);
        for (int k = 0; k < a_flat.texts.size(); ++k) {
            const auto& [at, text] = a_flat.texts.at(k);
            if (!seen.contains(at)) continue;
            const quint32 layer = a_flat.textLayers[std::size_t(k)];
            if (int(layer) < a_view->a_styles.size() && !a_view->a_styles.at(int(layer)).visible) continue;
            if (++texts > 5000) break;
            const QPointF px = a_view->toPixel(at);
            p.drawLine(px + QPointF(-3, 0), px + QPointF(3, 0));
            p.drawLine(px + QPointF(0, -3), px + QPointF(0, 3));
            p.drawText(px + QPointF(4, -3), text);
        }
    }
    a_textsDrawn = std::min(texts, 5000);
    if (stats) {
        *stats = RenderStats();
        stats->polygons = a_flat.shapes - a_flat.frameCount;
        stats->frames = a_flat.frameCount;
        stats->labels = quint64(std::min(texts, 5000));
    }
    return true;
}

} // namespace qucs_s::layout
