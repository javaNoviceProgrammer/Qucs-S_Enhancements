/*
 * multiphysicsviewgpu.cpp - the multiphysics Graphics view drawn on the GPU
 *                           (Qt's RHI)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicsviewgpu.h"

#include "multiphysicsview.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qucs_s::fem {

namespace {

// The shaders' uniforms: the triangles' (fem_fill, fem_surface) and the
// lines' (fem_line), a slot of 128 bytes each draw.
struct FillUniforms {
    float corr[16];
    float center[2];
    float scale[2];
};
struct LineUniforms {
    float corr[16];
    float uniformColor[4];   // straight alpha
    float center[2];
    float scale[2];
    float viewport[2];   // device pixels
    float ratio;
    float roundCaps;
    float useUniform;
    float uniformWidth;
    float yUp;
    float pad;
};
constexpr quint32 kSlotBytes = 128;
static_assert(sizeof(LineUniforms) == kSlotBytes && sizeof(FillUniforms) <= kSlotBytes);
// (A frame's draws: seven at most.)
constexpr int kSlots = 32;

// A line's quad, a strip: its corners' numbers.
const float kCorners[] = {0, 1, 2, 3};

quint32 rgba(const QColor& c)
{
    // Bytes r, g, b, a in memory.
    const quint32 r = quint32(c.red()), g = quint32(c.green()), b = quint32(c.blue()), a = quint32(c.alpha());
    return r | (g << 8) | (b << 16) | (a << 24);
}

} // namespace

GpuCanvas::GpuCanvas(GraphicsView* view) : gpu::LayeredCanvas(view, 4), a_view(view)
{
    setObjectName(QStringLiteral("multiphysicsCanvas"));
}

GpuCanvas::~GpuCanvas()
{
    releaseNativeResources();
}

void GpuCanvas::paintFrame(QPainter& p)
{
    a_slotsUsed = 0;
    a_view->paintFrame(p, this);
}

void GpuCanvas::canvasFailed()
{
    QMetaObject::invokeMethod(a_view, [v = a_view] { v->canvasFailed(); }, Qt::QueuedConnection);
}

void GpuCanvas::releaseNativeResources()
{
    for (Batch* b : {&a_domains, &a_quality, &a_surfaceBatch, &a_mesh, &a_boundaries, &a_contours, &a_rays}) *b = Batch();
    a_fill.reset();
    a_surface.reset();
    a_lines.reset();
    a_plainBindings.reset();
    a_surfaceBindings.reset();
    a_colormap.reset();
    a_colormapName.clear();
    a_nearest.reset();
    a_corners.reset();
    a_uniforms.reset();
    a_fills.clear();
    a_strokes.clear();
    a_rhi = nullptr;
}

void GpuCanvas::createResources(QRhi* rhi)
{
    a_rhi = rhi;
    const quint32 slot = quint32(rhi->ubufAligned(int(kSlotBytes)));
    a_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kSlots * slot));
    a_uniforms->create();
    a_corners = buffer(kCorners, sizeof(kCorners));
    a_nearest.reset(rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    a_nearest->create();
    a_colormap.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(256, 1)));
    a_colormap->create();
    a_colormapName.clear();
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    a_plainBindings.reset(rhi->newShaderResourceBindings());
    a_plainBindings->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, a_uniforms.get(), kSlotBytes)});
    a_plainBindings->create();
    a_surfaceBindings.reset(rhi->newShaderResourceBindings());
    a_surfaceBindings->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(0, stages, a_uniforms.get(), kSlotBytes),
                                    QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, a_colormap.get(), a_nearest.get())});
    a_surfaceBindings->create();
    QRhiVertexInputLayout fills;
    fills.setBindings({QRhiVertexInputBinding(2 * sizeof(float)), QRhiVertexInputBinding(4)});
    fills.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0), QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::UNormByte4, 0)});
    QRhiVertexInputLayout values;
    values.setBindings({QRhiVertexInputBinding(3 * sizeof(float))});
    values.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                          QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float, 2 * sizeof(float))});
    QRhiVertexInputLayout lines;
    lines.setBindings({QRhiVertexInputBinding(sizeof(float)), QRhiVertexInputBinding(4 * sizeof(float), QRhiVertexInputBinding::PerInstance),
                       QRhiVertexInputBinding(8, QRhiVertexInputBinding::PerInstance)});
    lines.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float, 0), QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float4, 0),
                         QRhiVertexInputAttribute(2, 2, QRhiVertexInputAttribute::UNormByte4, 0), QRhiVertexInputAttribute(2, 3, QRhiVertexInputAttribute::Float, 4)});
    a_fill = gpu::makePipeline(rhi, pass(), samples(), a_plainBindings.get(),
                               {QStringLiteral("fem_fill.vert"), QStringLiteral("fem_fill.frag"), fills, QRhiGraphicsPipeline::Triangles, false});
    a_surface = gpu::makePipeline(rhi, pass(), samples(), a_surfaceBindings.get(),
                                  {QStringLiteral("fem_surface.vert"), QStringLiteral("fem_surface.frag"), values, QRhiGraphicsPipeline::Triangles, false});
    a_lines = gpu::makePipeline(rhi, pass(), samples(), a_plainBindings.get(),
                                {QStringLiteral("fem_line.vert"), QStringLiteral("fem_line.frag"), lines, QRhiGraphicsPipeline::TriangleStrip, false});
    if (!a_fill || !a_surface || !a_lines) {
        qWarning("Graphics view: the GPU's pipelines not made; drawn by the CPU");
        canvasFailed();
    }
    a_topologyStamp = a_meshStamp = a_plotStamp = 0;
}

std::unique_ptr<QRhiBuffer> GpuCanvas::buffer(const void* data, std::size_t bytes)
{
    if (bytes == 0) return nullptr;
    std::unique_ptr<QRhiBuffer> b(a_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(bytes)));
    b->create();
    updates()->uploadStaticBuffer(b.get(), data);
    return b;
}

int GpuCanvas::slot(const void* uniforms, std::size_t bytes)
{
    if (a_slotsUsed >= kSlots) return -1;
    const int offset = a_slotsUsed++ * a_rhi->ubufAligned(int(kSlotBytes));
    updates()->updateDynamicBuffer(a_uniforms.get(), quint32(offset), quint32(bytes), uniforms);
    return offset;
}

void GpuCanvas::sync()
{
    // Coordinates as floats from an origin: the content's middle, moved to
    // where the view is when, deep in and far from it, a float's step would
    // show (some 2 million pixels from it).
    const GraphicsView& v = *a_view;
    const bool rebase = (std::abs(v.a_center.x() - a_origin.x()) + std::abs(v.a_center.y() - a_origin.y())) * v.a_scale > 2e6;
    if (v.a_topologyStamp != a_topologyStamp || rebase) {
        a_topologyStamp = v.a_topologyStamp;
        a_origin = rebase ? v.a_center : v.contentBounds().center();
        a_meshStamp = a_plotStamp = 0;
        a_domains.built = a_boundaries.built = false;
    }
    if (v.a_meshStamp != a_meshStamp) {
        a_meshStamp = v.a_meshStamp;
        a_mesh.built = a_quality.built = false;
    }
    if (v.a_plotStamp != a_plotStamp) {
        a_plotStamp = v.a_plotStamp;
        a_surfaceBatch.built = a_contours.built = a_rays.built = false;
    }
}

void GpuCanvas::buildLines(Batch& b, const std::vector<float>& segments, const std::vector<quint32>& colors, const std::vector<float>& widths)
{
    b.vertices = buffer(segments.data(), segments.size() * sizeof(float));
    b.extra.reset();
    if (!colors.empty()) {
        // Each segment's color (four bytes) and width (a float).
        std::vector<char> extra(colors.size() * 8);
        for (std::size_t i = 0; i < colors.size(); ++i) {
            std::memcpy(&extra[i * 8], &colors[i], 4);
            std::memcpy(&extra[i * 8 + 4], &widths[i], 4);
        }
        b.extra = buffer(extra.data(), extra.size());
    }
    b.count = int(segments.size() / 4);
    b.built = true;
}

void GpuCanvas::drawTriangles(QPainter& p, Batch& b, bool surface)
{
    if (b.count == 0 || !b.vertices || (!surface && !b.extra)) return;
    FillUniforms u{};
    std::memcpy(u.corr, corr().constData(), sizeof(u.corr));
    const double s = a_view->a_scale;
    const QPointF c = a_view->a_center - a_origin;
    u.center[0] = float(c.x());
    u.center[1] = float(c.y());
    u.scale[0] = float(2 * s / std::max(1, width()));
    u.scale[1] = float(2 * s / std::max(1, height()));
    const int offset = slot(&u, sizeof(u));
    if (offset < 0) return;
    QRhiGraphicsPipeline* pipeline = surface ? a_surface.get() : a_fill.get();
    QRhiShaderResourceBindings* bindings = surface ? a_surfaceBindings.get() : a_plainBindings.get();
    QRhiBuffer* vertices = b.vertices.get();
    QRhiBuffer* extra = b.extra.get();
    const int count = b.count;
    const QRhiViewport viewport = fullViewport();
    native(p, [pipeline, viewport, offset, bindings, surface, vertices, extra, count](QRhiCommandBuffer* cb) {
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(viewport);
        const QRhiCommandBuffer::DynamicOffset dynamic(0, quint32(offset));
        cb->setShaderResources(bindings, 1, &dynamic);
        if (surface) {
            const QRhiCommandBuffer::VertexInput input(vertices, 0);
            cb->setVertexInput(0, 1, &input);
        } else {
            const QRhiCommandBuffer::VertexInput inputs[] = {{vertices, 0}, {extra, 0}};
            cb->setVertexInput(0, 2, inputs);
        }
        cb->draw(quint32(count));
    });
}

void GpuCanvas::drawLines(QPainter& p, Batch& b, bool roundCaps, bool cosmetic, const QColor* color, float width)
{
    if (b.count == 0 || !b.vertices || (!color && !b.extra)) return;
    LineUniforms u{};
    std::memcpy(u.corr, corr().constData(), sizeof(u.corr));
    const double s = a_view->a_scale;
    const QPointF c = a_view->a_center - a_origin;
    u.center[0] = float(c.x());
    u.center[1] = float(c.y());
    u.scale[0] = float(2 * s / std::max(1, this->width()));
    u.scale[1] = float(2 * s / std::max(1, height()));
    const QSize px = pixels();
    u.viewport[0] = float(px.width());
    u.viewport[1] = float(px.height());
    u.ratio = cosmetic ? 1.0f : float(devicePixelRatioF());
    u.roundCaps = roundCaps ? 1.0f : 0.0f;
    if (color) {
        u.useUniform = 1.0f;
        u.uniformColor[0] = float(color->redF());
        u.uniformColor[1] = float(color->greenF());
        u.uniformColor[2] = float(color->blueF());
        u.uniformColor[3] = float(color->alphaF());
        u.uniformWidth = width;
    }
    u.yUp = yUp() ? 1.0f : 0.0f;
    const int offset = slot(&u, sizeof(u));
    if (offset < 0) return;
    QRhiGraphicsPipeline* pipeline = a_lines.get();
    QRhiShaderResourceBindings* bindings = a_plainBindings.get();
    QRhiBuffer* corners = a_corners.get();
    QRhiBuffer* segments = b.vertices.get();
    // (A color and width the uniforms': the instance's not read - the
    // segments stand in, as long.)
    QRhiBuffer* extra = b.extra ? b.extra.get() : segments;
    const int count = b.count;
    const QRhiViewport viewport = fullViewport();
    native(p, [pipeline, viewport, offset, bindings, corners, segments, extra, count](QRhiCommandBuffer* cb) {
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(viewport);
        const QRhiCommandBuffer::DynamicOffset dynamic(0, quint32(offset));
        cb->setShaderResources(bindings, 1, &dynamic);
        const QRhiCommandBuffer::VertexInput inputs[] = {{corners, 0}, {segments, 0}, {extra, 0}};
        cb->setVertexInput(0, 3, inputs);
        cb->draw(4, quint32(count));
    });
}

void GpuCanvas::drawDomains(QPainter& p, const std::vector<QColor>& fills)
{
    if (!a_rhi || !a_view->a_topology) return;
    sync();
    const Topology& t = *a_view->a_topology;
    if (!a_domains.built) {
        std::vector<float> pos;
        a_domainOf.clear();
        for (int d = 0; d < int(t.domains.size()); ++d)
            for (const auto& tri : t.domains[std::size_t(d)].triangles)
                for (int k = 0; k < 3; ++k) {
                    const QPointF v = t.vertices[std::size_t(tri[std::size_t(k)])];
                    pos.push_back(float(v.x() - a_origin.x()));
                    pos.push_back(float(v.y() - a_origin.y()));
                    a_domainOf.push_back(d);
                }
        a_domains.vertices = buffer(pos.data(), pos.size() * sizeof(float));
        a_domains.extra.reset();
        a_domains.count = int(a_domainOf.size());
        a_domains.built = true;
        a_fills.clear();
    }
    if (fills != a_fills || (!a_domains.extra && a_domains.count)) {
        // (The colors alone again: a domain hovered, chosen, faded.)
        std::vector<quint32> colors(a_domainOf.size());
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const int d = a_domainOf[i];
            colors[i] = d < int(fills.size()) ? rgba(fills[std::size_t(d)]) : 0;
        }
        a_domains.extra = buffer(colors.data(), colors.size() * 4);
        a_fills = fills;
    }
    drawTriangles(p, a_domains, false);
}

void GpuCanvas::drawBoundaries(QPainter& p, const std::vector<Stroke>& strokes)
{
    if (!a_rhi || !a_view->a_topology) return;
    sync();
    const Topology& t = *a_view->a_topology;
    if (!a_boundaries.built) {
        a_boundarySegments.clear();
        a_boundaryOf.clear();
        for (int b = 0; b < int(t.boundaries.size()); ++b) {
            const std::vector<QPointF> pts = t.boundaries[std::size_t(b)].polyline(t);
            for (std::size_t i = 1; i < pts.size(); ++i) {
                a_boundarySegments.insert(a_boundarySegments.end(),
                                          {float(pts[i - 1].x() - a_origin.x()), float(pts[i - 1].y() - a_origin.y()),
                                           float(pts[i].x() - a_origin.x()), float(pts[i].y() - a_origin.y())});
                a_boundaryOf.push_back(b);
            }
        }
        a_strokes.clear();
    }
    if (!a_boundaries.built || strokes != a_strokes) {
        std::vector<quint32> colors(a_boundaryOf.size());
        std::vector<float> widths(a_boundaryOf.size());
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const int b = a_boundaryOf[i];
            const Stroke s = b < int(strokes.size()) ? strokes[std::size_t(b)] : Stroke{Qt::transparent, 1};
            colors[i] = rgba(s.color);
            widths[i] = s.width;
        }
        buildLines(a_boundaries, a_boundarySegments, colors, widths);
        a_strokes = strokes;
    }
    drawLines(p, a_boundaries, true, true);
}

void GpuCanvas::drawMesh(QPainter& p, QColor ink, float width)
{
    if (!a_rhi || !a_view->a_mesh) return;
    sync();
    const Mesh& m = *a_view->a_mesh;
    if (!a_mesh.built) {
        std::vector<float> segments;
        segments.reserve(a_view->a_meshEdges.size() * 4);
        for (const auto& [a, b] : a_view->a_meshEdges) {
            const QPointF pa = m.nodes[std::size_t(a)], pb = m.nodes[std::size_t(b)];
            segments.insert(segments.end(), {float(pa.x() - a_origin.x()), float(pa.y() - a_origin.y()), float(pb.x() - a_origin.x()),
                                             float(pb.y() - a_origin.y())});
        }
        buildLines(a_mesh, segments, {}, {});
    }
    drawLines(p, a_mesh, false, true, &ink, width);
}

void GpuCanvas::drawQuality(QPainter& p)
{
    if (!a_rhi || !a_view->a_mesh) return;
    sync();
    const Mesh& m = *a_view->a_mesh;
    if (!a_quality.built) {
        std::vector<float> pos;
        std::vector<quint32> colors;
        pos.reserve(m.triangles.size() * 6);
        colors.reserve(m.triangles.size() * 3);
        for (int t = 0; t < int(m.triangles.size()); ++t) {
            const quint32 c = rgba(GraphicsView::qualityColor(m.quality(t)));
            for (int k : m.triangles[std::size_t(t)]) {
                const QPointF v = m.nodes[std::size_t(k)];
                pos.push_back(float(v.x() - a_origin.x()));
                pos.push_back(float(v.y() - a_origin.y()));
                colors.push_back(c);
            }
        }
        a_quality.vertices = buffer(pos.data(), pos.size() * sizeof(float));
        a_quality.extra = buffer(colors.data(), colors.size() * 4);
        a_quality.count = int(colors.size());
        a_quality.built = true;
    }
    drawTriangles(p, a_quality, false);
}

void GpuCanvas::drawSurface(QPainter& p)
{
    const PlotScene* plot = a_view->a_plot.get();
    if (!a_rhi || !plot || !plot->surface) return;
    sync();
    if (!a_surfaceBatch.built) {
        const SurfaceData& s = plot->values;
        const double lo = plot->min, span = plot->max > plot->min ? plot->max - plot->min : 1;
        std::vector<float> data;
        const std::size_t n = s.points.size() / 3;
        data.reserve(n * 9);
        for (std::size_t t = 0; t < n; ++t) {
            const double* v = &s.values[t * 3];
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
            for (int k = 0; k < 3; ++k) {
                const QPointF& q = s.points[t * 3 + std::size_t(k)];
                // (Scaled here, in doubles: a field of 300 K ± 1 mK keeps its colors.)
                data.insert(data.end(), {float(q.x() - a_origin.x()), float(q.y() - a_origin.y()), float((v[k] - lo) / span)});
            }
        }
        a_surfaceBatch.vertices = buffer(data.data(), data.size() * sizeof(float));
        a_surfaceBatch.count = int(data.size() / 3);
        a_surfaceBatch.built = true;
    }
    if (a_colormapName != plot->colors) {
        const std::vector<QRgb>& table = colorTable(plot->colors);
        QImage texels(256, 1, QImage::Format_RGBA8888);
        for (int i = 0; i < 256; ++i) texels.setPixelColor(i, 0, QColor::fromRgb(table[std::size_t(i)]));
        updates()->uploadTexture(a_colormap.get(), texels);
        a_colormapName = plot->colors;
    }
    drawTriangles(p, a_surfaceBatch, true);
}

void GpuCanvas::drawContours(QPainter& p)
{
    const PlotScene* plot = a_view->a_plot.get();
    if (!a_rhi || !plot || plot->contours.empty()) return;
    sync();
    if (!a_contours.built) {
        std::vector<float> segments;
        std::vector<quint32> colors;
        std::size_t level = 0;
        for (const PlotScene::Contours& c : plot->contours)
            for (std::size_t l = 0; l < c.data.levels.size(); ++l, ++level) {
                const quint32 color = level < a_view->a_contourColors.size() ? rgba(a_view->a_contourColors[level]) : rgba(Qt::black);
                for (const QLineF& line : c.data.lines[l]) {
                    segments.insert(segments.end(), {float(line.x1() - a_origin.x()), float(line.y1() - a_origin.y()),
                                                     float(line.x2() - a_origin.x()), float(line.y2() - a_origin.y())});
                    colors.push_back(color);
                }
            }
        buildLines(a_contours, segments, colors, std::vector<float>(colors.size(), 1.3f));
    }
    drawLines(p, a_contours, false, true);
}

void GpuCanvas::drawRays(QPainter& p)
{
    const PlotScene* plot = a_view->a_plot.get();
    if (!a_rhi || !plot || plot->rays.empty()) return;
    sync();
    if (!a_rays.built) {
        std::vector<float> segments;
        std::vector<quint32> colors;
        for (const PlotScene::Rays& rays : plot->rays)
            for (std::size_t k = 0; k < rays.paths.size(); ++k) {
                const std::vector<QPointF>& path = rays.paths[k];
                for (std::size_t i = 0; i + 1 < path.size(); ++i) {
                    segments.insert(segments.end(), {float(path[i].x() - a_origin.x()), float(path[i].y() - a_origin.y()),
                                                     float(path[i + 1].x() - a_origin.x()), float(path[i + 1].y() - a_origin.y())});
                    colors.push_back(rgba(GraphicsView::rayColor(rays, k, i)));
                }
            }
        buildLines(a_rays, segments, colors, std::vector<float>(colors.size(), 1.1f));
    }
    drawLines(p, a_rays, false, false);
}

} // namespace qucs_s::fem
