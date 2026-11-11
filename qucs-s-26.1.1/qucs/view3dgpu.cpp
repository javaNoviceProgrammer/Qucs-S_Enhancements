/*
 * view3dgpu.cpp - Multiphysics 3D's view drawn on the GPU (Qt's RHI)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "view3dgpu.h"

#include "view3d.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

using qucs_s::fem::Box3;
using qucs_s::fem::Vec3;

namespace {

// The shaders' uniforms (view3d_*.vert, .frag): one block for the three.
struct Uniforms {
    float corr[16];
    float right[4];
    float up[4];
    float toward[4];
    float center[4];
    float clip[4];
    float scale[2];
    float viewport[2];
    float depthRange;
    float bias;
    float clipOn;
    float pixel;
};
static_assert(sizeof(Uniforms) == 176);

// A strip's corners' numbers: a line's quad, a dot's square.
const float kCorners[] = {0, 1, 2, 3};

quint32 rgba(const QColor& c)
{
    // Bytes r, g, b, a in memory.
    return quint32(c.red()) | (quint32(c.green()) << 8) | (quint32(c.blue()) << 16) | (quint32(c.alpha()) << 24);
}

void put(float* to, const Vec3& v, double w = 0)
{
    to[0] = float(v.x);
    to[1] = float(v.y);
    to[2] = float(v.z);
    to[3] = float(w);
}

// A triangle's vertex: its position, its face's normal (w 1: a field's
// colours, lit less), its colour - 32 bytes.
struct TriangleVertex {
    float position[3];
    float normal[4];
    quint32 color;
};
static_assert(sizeof(TriangleVertex) == 32);
// A line's instance: its ends, its colour, its width in device pixels.
struct LineInstance {
    float a[3], b[3];
    quint32 color;
    float width;
};
static_assert(sizeof(LineInstance) == 32);
// A dot's instance: its place, its colour, its radius in device pixels.
struct DotInstance {
    float p[3];
    quint32 color;
    float radius;
};
static_assert(sizeof(DotInstance) == 20);

} // namespace

View3DCanvas::View3DCanvas(View3D* view) : qucs_s::gpu::LayeredCanvas(view, 4), a_view(view)
{
    setObjectName(QStringLiteral("view3dCanvas"));
}

View3DCanvas::~View3DCanvas()
{
    releaseNativeResources();
}

void View3DCanvas::paintFrame(QPainter& p)
{
    a_view->paintFrame(p, this);
}

void View3DCanvas::canvasFailed()
{
    QMetaObject::invokeMethod(a_view, [v = a_view] { v->canvasFailed(); }, Qt::QueuedConnection);
}

void View3DCanvas::releaseNativeResources()
{
    a_triangles.reset();
    a_lines.reset();
    a_dots.reset();
    a_bindings.reset();
    a_triangleData.reset();
    a_lineData.reset();
    a_dotData.reset();
    a_triangleCount = a_lineCount = a_dotCount = 0;
    a_corners.reset();
    a_uniforms.reset();
    a_stamp = 0;
    a_rhi = nullptr;
}

void View3DCanvas::createResources(QRhi* rhi)
{
    a_rhi = rhi;
    a_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
    a_uniforms->create();
    a_corners = buffer(kCorners, sizeof(kCorners));
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    a_bindings.reset(rhi->newShaderResourceBindings());
    a_bindings->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, a_uniforms.get())});
    a_bindings->create();
    QRhiVertexInputLayout triangles;
    triangles.setBindings({QRhiVertexInputBinding(sizeof(TriangleVertex))});
    triangles.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
                             QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, 3 * sizeof(float)),
                             QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UNormByte4, 7 * sizeof(float))});
    QRhiVertexInputLayout lines;
    lines.setBindings({QRhiVertexInputBinding(sizeof(float)), QRhiVertexInputBinding(sizeof(LineInstance), QRhiVertexInputBinding::PerInstance)});
    lines.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float, 0), QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float3, 0),
                         QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)),
                         QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::UNormByte4, 6 * sizeof(float)),
                         QRhiVertexInputAttribute(1, 4, QRhiVertexInputAttribute::Float, 7 * sizeof(float))});
    QRhiVertexInputLayout dots;
    dots.setBindings({QRhiVertexInputBinding(sizeof(float)), QRhiVertexInputBinding(sizeof(DotInstance), QRhiVertexInputBinding::PerInstance)});
    dots.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float, 0), QRhiVertexInputAttribute(1, 1, QRhiVertexInputAttribute::Float3, 0),
                        QRhiVertexInputAttribute(1, 2, QRhiVertexInputAttribute::UNormByte4, 3 * sizeof(float)),
                        QRhiVertexInputAttribute(1, 3, QRhiVertexInputAttribute::Float, 4 * sizeof(float))});
    // (The triangles nearest win, the first drawn where two are as near -
    // the CPU's; a line or a dot, drawn toward the eye already, where it is
    // as near as what it is on.)
    qucs_s::gpu::PipelineSpec tri{QStringLiteral("view3d_tri.vert"), QStringLiteral("view3d_tri.frag"), triangles, QRhiGraphicsPipeline::Triangles, false};
    tri.depth = true;
    tri.depthOp = QRhiGraphicsPipeline::Less;
    qucs_s::gpu::PipelineSpec line{QStringLiteral("view3d_line.vert"), QStringLiteral("view3d_line.frag"), lines, QRhiGraphicsPipeline::TriangleStrip, false};
    line.depth = true;
    line.depthOp = QRhiGraphicsPipeline::LessOrEqual;
    qucs_s::gpu::PipelineSpec dot{QStringLiteral("view3d_dot.vert"), QStringLiteral("view3d_dot.frag"), dots, QRhiGraphicsPipeline::TriangleStrip, false};
    dot.depth = true;
    dot.depthOp = QRhiGraphicsPipeline::LessOrEqual;
    a_triangles = qucs_s::gpu::makePipeline(rhi, pass(), samples(), a_bindings.get(), tri);
    a_lines = qucs_s::gpu::makePipeline(rhi, pass(), samples(), a_bindings.get(), line);
    a_dots = qucs_s::gpu::makePipeline(rhi, pass(), samples(), a_bindings.get(), dot);
    if (!a_triangles || !a_lines || !a_dots) {
        qWarning("Multiphysics 3D view: the GPU's pipelines not made; drawn by the CPU");
        canvasFailed();
    }
    a_stamp = 0;
}

std::unique_ptr<QRhiBuffer> View3DCanvas::buffer(const void* data, std::size_t bytes)
{
    if (bytes == 0) return nullptr;
    std::unique_ptr<QRhiBuffer> b(a_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(bytes)));
    b->create();
    updates()->uploadStaticBuffer(b.get(), data);
    return b;
}

void View3DCanvas::build()
{
    const double ratio = devicePixelRatioF();
    if (a_stamp == a_view->a_sceneStamp && a_ratio == ratio) return;
    a_stamp = a_view->a_sceneStamp;
    a_ratio = ratio;
    ++a_builds;
    const View3D::Scene& s = a_view->a_scene;
    // Coordinates as floats from the scene's middle.
    a_origin = s.bounds.isEmpty() ? Vec3{} : s.bounds.center();
    auto rel = [this](const Vec3& p, float* to) {
        to[0] = float(p.x - a_origin.x);
        to[1] = float(p.y - a_origin.y);
        to[2] = float(p.z - a_origin.z);
    };
    std::vector<TriangleVertex> tv;
    tv.reserve(s.triangles.size() * 3);
    for (const View3D::Triangle& t : s.triangles) {
        const Vec3 n = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalized();
        for (int k = 0; k < 3; ++k) {
            TriangleVertex v;
            rel(t.p[std::size_t(k)], v.position);
            put(v.normal, n, t.shaded ? 1 : 0);
            v.color = rgba(t.shaded ? t.colors[std::size_t(k)] : t.color);
            tv.push_back(v);
        }
    }
    std::vector<LineInstance> lv;
    lv.reserve(s.lines.size());
    for (const View3D::Line& l : s.lines) {
        LineInstance v;
        rel(l.a, v.a);
        rel(l.b, v.b);
        v.color = rgba(l.color);
        // (Whole device pixels, at least one: the CPU's brush.)
        v.width = float(std::max(1L, std::lround(l.width * ratio)));
        lv.push_back(v);
    }
    std::vector<DotInstance> dv;
    dv.reserve(s.dots.size());
    for (const View3D::Dot& d : s.dots) {
        DotInstance v;
        rel(d.p, v.p);
        v.color = rgba(d.color);
        v.radius = float(std::max(1L, std::lround(d.radius * ratio)));
        dv.push_back(v);
    }
    a_triangleData = buffer(tv.data(), tv.size() * sizeof(TriangleVertex));
    a_lineData = buffer(lv.data(), lv.size() * sizeof(LineInstance));
    a_dotData = buffer(dv.data(), dv.size() * sizeof(DotInstance));
    a_triangleCount = int(tv.size());
    a_lineCount = int(lv.size());
    a_dotCount = int(dv.size());
}

void View3DCanvas::drawScene(QPainter& p)
{
    if (!a_rhi || !a_triangles || !a_lines || !a_dots) return;
    build();
    if (a_triangleCount == 0 && a_lineCount == 0 && a_dotCount == 0) return;
    const View3D& v = *a_view;
    const View3D::Camera c = v.camera();
    const Box3& b = v.a_scene.bounds;
    const double ratio = devicePixelRatioF();
    const double pixel = 1 / std::max(v.a_scale * ratio, 1e-300);
    const double diagonal = b.isEmpty() ? 1 : std::max(b.diagonal(), 1e-300);
    Uniforms u{};
    std::memcpy(u.corr, corr().constData(), sizeof(u.corr));
    put(u.right, c.right);
    put(u.up, c.up);
    put(u.toward, c.toward);
    put(u.center, v.a_center - a_origin);
    u.scale[0] = float(2 * v.a_scale / std::max(1, width()));
    u.scale[1] = float(2 * v.a_scale / std::max(1, height()));
    const QSize px = pixels();
    u.viewport[0] = float(px.width());
    u.viewport[1] = float(px.height());
    // (Depth ±1 a little past what is drawn: the scene's half diagonal from
    // its middle, however far the camera's centre is panned from it, and
    // the lines' and dots' steps toward the eye.)
    u.bias = float(2.5 * pixel + 1e-9 * std::max(1.0, diagonal));
    u.depthRange = float(1.01 * ((v.a_center - a_origin).norm() + 0.5 * diagonal) + 64 * pixel);
    u.pixel = float(pixel);
    if (v.a_clipAxis >= 0 && !b.isEmpty()) {
        const Vec3 axis{v.a_clipAxis == 0 ? 1.0 : 0.0, v.a_clipAxis == 1 ? 1.0 : 0.0, v.a_clipAxis == 2 ? 1.0 : 0.0};
        put(u.clip, axis, v.cutValue() - axis.dot(a_origin));
        u.clipOn = 1;
    }
    updates()->updateDynamicBuffer(a_uniforms.get(), 0, sizeof(u), &u);
    QRhiGraphicsPipeline* triangles = a_triangles.get();
    QRhiGraphicsPipeline* lines = a_lines.get();
    QRhiGraphicsPipeline* dots = a_dots.get();
    QRhiShaderResourceBindings* bindings = a_bindings.get();
    QRhiBuffer* corners = a_corners.get();
    QRhiBuffer* triangleData = a_triangleData.get();
    QRhiBuffer* lineData = a_lineData.get();
    QRhiBuffer* dotData = a_dotData.get();
    const int nt = a_triangleCount, nl = a_lineCount, nd = a_dotCount;
    const QRhiViewport viewport = fullViewport();
    native(p, [=](QRhiCommandBuffer* cb) {
        if (nt > 0) {
            cb->setGraphicsPipeline(triangles);
            cb->setViewport(viewport);
            cb->setShaderResources(bindings);
            const QRhiCommandBuffer::VertexInput input(triangleData, 0);
            cb->setVertexInput(0, 1, &input);
            cb->draw(quint32(nt));
        }
        if (nl > 0) {
            cb->setGraphicsPipeline(lines);
            cb->setViewport(viewport);
            cb->setShaderResources(bindings);
            const QRhiCommandBuffer::VertexInput inputs[] = {{corners, 0}, {lineData, 0}};
            cb->setVertexInput(0, 2, inputs);
            cb->draw(4, quint32(nl));
        }
        if (nd > 0) {
            cb->setGraphicsPipeline(dots);
            cb->setViewport(viewport);
            cb->setShaderResources(bindings);
            const QRhiCommandBuffer::VertexInput inputs[] = {{corners, 0}, {dotData, 0}};
            cb->setVertexInput(0, 2, inputs);
            cb->draw(4, quint32(nd));
        }
    });
}
