/*
 * multiphysicsviewgl.cpp - the multiphysics Graphics view drawn on the GPU
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicsviewgl.h"

#include "multiphysicsview.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QSurfaceFormat>
#include <QVector2D>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qucs_s::fem {

namespace {

// The triangles of the domains (and of the mesh's quality), each vertex its
// color.
const char* fillVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
uniform vec2 center;
uniform vec2 scale;
out vec4 vColor;
void main()
{
    gl_Position = vec4((position - center) * scale, 0.0, 1.0);
    vColor = color;
}
)";
const char* fillFragment = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

// A surface's values, (v - min) / (max - min) at each vertex, in color:
// the table's entry of the value interpolated, as the view's raster has it.
const char* surfaceVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in float value;
uniform vec2 center;
uniform vec2 scale;
out float vValue;
void main()
{
    gl_Position = vec4((position - center) * scale, 0.0, 1.0);
    vValue = value;
}
)";
const char* surfaceFragment = R"(#version 330 core
in float vValue;
uniform sampler2D colormap;
out vec4 fragColor;
void main()
{
    int i = clamp(int(vValue * 255.0 + 0.5), 0, 255);
    fragColor = texelFetch(colormap, ivec2(i, 0), 0);
}
)";

// Lines: each segment an instance, a quad round it in pixels; its pixels
// covered by their distance from it (antialiased, any width), with round
// or butt ends. Clipped to the viewport first: a segment thousands of
// screens long, deep in, keeps its pixels' floats exact.
const char* lineVertex = R"(#version 330 core
layout(location = 0) in vec4 segment;
layout(location = 1) in vec4 color;
layout(location = 2) in float width;
uniform vec2 center;
uniform vec2 scale;
uniform vec2 viewport;
uniform float ratio;
flat out vec2 a;
flat out vec2 b;
flat out float halfWidth;
flat out vec4 vColor;
void main()
{
    vec2 p0 = ((segment.xy - center) * scale * 0.5 + 0.5) * viewport;
    vec2 p1 = ((segment.zw - center) * scale * 0.5 + 0.5) * viewport;
    float w = width * ratio;
    vColor = color;
    if (w < 1.0) {
        vColor.a *= w;
        w = 1.0;
    }
    halfWidth = 0.5 * w;
    float r = halfWidth + 1.0;
    // (Liang and Barsky's clip to the viewport and a margin.)
    vec2 d = p1 - p0;
    float t0 = 0.0, t1 = 1.0;
    vec2 lo = vec2(-r - 2.0), hi = viewport + vec2(r + 2.0);
    for (int k = 0; k < 2; ++k) {
        if (abs(d[k]) < 1e-9) {
            if (p0[k] < lo[k] || p0[k] > hi[k]) t1 = -1.0;
        } else {
            float ta = (lo[k] - p0[k]) / d[k], tb = (hi[k] - p0[k]) / d[k];
            t0 = max(t0, min(ta, tb));
            t1 = min(t1, max(ta, tb));
        }
    }
    if (t1 < t0) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        a = b = vec2(-1e6);
        return;
    }
    vec2 q0 = p0 + d * t0, q1 = p0 + d * t1;
    vec2 e = q1 - q0;
    float len = length(e);
    vec2 t = len > 1e-6 ? e / len : vec2(1.0, 0.0);
    vec2 n = vec2(-t.y, t.x);
    int corner = gl_VertexID;
    vec2 at = (corner < 2 ? q0 - t * r : q1 + t * r) + n * ((corner == 0 || corner == 2) ? -r : r);
    // (The ends kept where the segment's are: a clipped end a butt off screen.)
    a = t0 > 0.0 ? q0 - t * r : q0;
    b = t1 < 1.0 ? q1 + t * r : q1;
    gl_Position = vec4(at / viewport * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* lineFragment = R"(#version 330 core
flat in vec2 a;
flat in vec2 b;
flat in float halfWidth;
flat in vec4 vColor;
uniform bool roundCaps;
out vec4 fragColor;
void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 d = b - a;
    float len = length(d);
    vec2 t = len > 1e-6 ? d / len : vec2(1.0, 0.0);
    float along = dot(p - a, t);
    float c;
    if (roundCaps) {
        c = clamp(halfWidth + 0.5 - length(p - (a + t * clamp(along, 0.0, len))), 0.0, 1.0);
    } else {
        float across = abs(dot(p - a, vec2(-t.y, t.x)));
        c = clamp(halfWidth + 0.5 - across, 0.0, 1.0) * clamp(along + 0.5, 0.0, 1.0) * clamp(len - along + 0.5, 0.0, 1.0);
    }
    if (c <= 0.0) discard;
    fragColor = vec4(vColor.rgb, vColor.a * c);
}
)";

QSurfaceFormat canvasFormat()
{
    QSurfaceFormat f = QSurfaceFormat::defaultFormat();
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    f.setSamples(4);
    return f;
}

quint32 rgba(const QColor& c)
{
    // Bytes r, g, b, a in memory.
    const quint32 r = quint32(c.red()), g = quint32(c.green()), b = quint32(c.blue()), a = quint32(c.alpha());
    return r | (g << 8) | (b << 16) | (a << 24);
}

} // namespace

bool GlCanvas::available()
{
    // (Every window's OpenGL the core profile, as prepareGpu() makes it
    // before the application: else its context is not shared - macOS falls
    // back to one of its own, its picture not composed.)
    const QSurfaceFormat all = QSurfaceFormat::defaultFormat();
    if (all.profile() != QSurfaceFormat::CoreProfile || all.version() < qMakePair(3, 3)) return false;
    static const bool can = [] {
        const QString platform = QGuiApplication::platformName();
        if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal") || platform == QLatin1String("vnc")) return false;
        QOpenGLContext context;
        context.setFormat(canvasFormat());
        if (!context.create()) return false;
        QOffscreenSurface surface;
        surface.setFormat(context.format());
        surface.create();
        if (!context.makeCurrent(&surface)) return false;
        const QSurfaceFormat got = context.format();
        const bool ok = got.version() >= qMakePair(3, 3) && !context.isOpenGLES();
        context.doneCurrent();
        return ok;
    }();
    return can;
}

GlCanvas::GlCanvas(GraphicsView* view) : QOpenGLWidget(view), a_view(view)
{
    setObjectName(QStringLiteral("multiphysicsCanvas"));
    setFormat(canvasFormat());
    // The view under it takes the mouse, the wheel, the gestures and keys.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(view->rect());
}

GlCanvas::~GlCanvas()
{
    // (Its context goes with QOpenGLWidget's destructor, after this one's:
    // told then, these objects are gone.)
    disconnect(a_contextLost);
    if (a_ready) {
        makeCurrent();
        release();
        doneCurrent();
    }
}

void GlCanvas::release()
{
    for (Batch* b : {&a_domains, &a_quality, &a_surfaceBatch, &a_mesh, &a_boundaries, &a_contours, &a_rays}) {
        b->vao.destroy();
        b->vbo.destroy();
        b->extra.destroy();
        b->count = 0;
        b->built = false;
    }
    a_fill.reset();
    a_surface.reset();
    a_lines.reset();
    if (a_colormap) glDeleteTextures(1, &a_colormap);
    a_colormap = 0;
    a_colormapName.clear();
    a_fills.clear();
    a_strokes.clear();
    a_ready = false;
}

void GlCanvas::initializeGL()
{
    initializeOpenGLFunctions();
    // (A context lost - the view moved to another window - its objects with it.)
    disconnect(a_contextLost);
    a_contextLost = connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        makeCurrent();
        release();
        doneCurrent();
    });
    auto program = [this](const char* vertex, const char* fragment) {
        auto p = std::make_unique<QOpenGLShaderProgram>();
        if (!p->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) || !p->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)
            || !p->link()) {
            qWarning("Graphics view: OpenGL shaders: %s", qPrintable(p->log()));
            return std::unique_ptr<QOpenGLShaderProgram>();
        }
        return p;
    };
    a_fill = program(fillVertex, fillFragment);
    a_surface = program(surfaceVertex, surfaceFragment);
    a_lines = program(lineVertex, lineFragment);
    if (!a_fill || !a_surface || !a_lines) {
        a_failed = true;
        QMetaObject::invokeMethod(a_view, [v = a_view] { v->canvasFailed(); }, Qt::QueuedConnection);
        return;
    }
    glGenTextures(1, &a_colormap);
    a_topologyStamp = a_meshStamp = a_plotStamp = 0;
    a_ready = true;
}

void GlCanvas::paintEvent(QPaintEvent* event)
{
    QOpenGLWidget::paintEvent(event);
    // (No context made - a driver without OpenGL 3.3, windows of another
    // profile: the view draws itself again, not a blank canvas.)
    if (!isValid() && !a_failed) {
        qWarning("Graphics view: no OpenGL context; drawn by the CPU");
        a_failed = true;
        QMetaObject::invokeMethod(a_view, [v = a_view] { v->canvasFailed(); }, Qt::QueuedConnection);
    }
}

void GlCanvas::paintGL()
{
    if (!a_ready) {
        glClearColor(0.5f, 0.5f, 0.5f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    // (A painter on the framebuffer itself: one on the widget, while Qt
    // grabs the view - QWidget::grab, a screenshot - is the grab's raster
    // painter, its fills and texts under the framebuffer's old picture.)
    QOpenGLPaintDevice device(QSize(std::lround(width() * devicePixelRatioF()), std::lround(height() * devicePixelRatioF())));
    device.setDevicePixelRatio(devicePixelRatioF());
    QPainter p(&device);
    p.setFont(font());
    a_view->paintFrame(p, this);
    ++a_frames;
}

void GlCanvas::begin(QPainter& p)
{
    p.beginNativePainting();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void GlCanvas::end(QPainter& p)
{
    glBindVertexArray(0);
    glUseProgram(0);
    p.endNativePainting();
}

void GlCanvas::setView(QOpenGLShaderProgram& program)
{
    const double s = a_view->a_scale;
    const QPointF c = a_view->a_center - a_origin;
    program.setUniformValue("center", QVector2D(float(c.x()), float(c.y())));
    program.setUniformValue("scale", QVector2D(float(2 * s / std::max(1, width())), float(2 * s / std::max(1, height()))));
}

void GlCanvas::sync()
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

void GlCanvas::buildLines(Batch& b, const std::vector<float>& segments, const std::vector<quint32>& colors, const std::vector<float>& widths)
{
    if (!b.vao.isCreated()) b.vao.create();
    b.vao.bind();
    if (!b.vbo.isCreated()) b.vbo.create();
    b.vbo.bind();
    b.vbo.allocate(segments.data(), int(segments.size() * sizeof(float)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribDivisor(0, 1);
    if (!colors.empty()) {
        // Each segment's color (four bytes) and width (a float).
        std::vector<char> extra(colors.size() * 8);
        for (std::size_t i = 0; i < colors.size(); ++i) {
            std::memcpy(&extra[i * 8], &colors[i], 4);
            std::memcpy(&extra[i * 8 + 4], &widths[i], 4);
        }
        if (!b.extra.isCreated()) b.extra.create();
        b.extra.bind();
        b.extra.allocate(extra.data(), int(extra.size()));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 8, nullptr);
        glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8, reinterpret_cast<void*>(4));
        glVertexAttribDivisor(2, 1);
    } else {
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
    }
    b.vao.release();
    b.count = int(segments.size() / 4);
    b.built = true;
}

void GlCanvas::drawLines(Batch& b, bool roundCaps, bool cosmetic, const QColor* color, float width)
{
    if (b.count == 0) return;
    a_lines->bind();
    setView(*a_lines);
    GLint vp[4] = {0, 0, 1, 1};
    glGetIntegerv(GL_VIEWPORT, vp);
    a_lines->setUniformValue("viewport", QVector2D(float(vp[2]), float(vp[3])));
    a_lines->setUniformValue("ratio", cosmetic ? 1.0f : float(devicePixelRatioF()));
    a_lines->setUniformValue("roundCaps", GLint(roundCaps));
    b.vao.bind();
    if (color) {
        glVertexAttrib4f(1, color->redF(), color->greenF(), color->blueF(), color->alphaF());
        glVertexAttrib1f(2, width);
    }
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, b.count);
}

void GlCanvas::drawDomains(QPainter& p, const std::vector<QColor>& fills)
{
    if (!a_view->a_topology) return;
    sync();
    const Topology& t = *a_view->a_topology;
    begin(p);
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
        if (!a_domains.vao.isCreated()) a_domains.vao.create();
        a_domains.vao.bind();
        if (!a_domains.vbo.isCreated()) a_domains.vbo.create();
        a_domains.vbo.bind();
        a_domains.vbo.allocate(pos.data(), int(pos.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        if (!a_domains.extra.isCreated()) a_domains.extra.create();
        a_domains.extra.bind();
        a_domains.extra.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        a_domains.extra.allocate(int(a_domainOf.size() * 4));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
        a_domains.vao.release();
        a_domains.count = int(a_domainOf.size());
        a_domains.built = true;
        a_fills.clear();
    }
    if (fills != a_fills) {
        // (The colors alone again: a domain hovered, chosen, faded.)
        std::vector<quint32> colors(a_domainOf.size());
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const int d = a_domainOf[i];
            colors[i] = d < int(fills.size()) ? rgba(fills[std::size_t(d)]) : 0;
        }
        a_domains.extra.bind();
        a_domains.extra.write(0, colors.data(), int(colors.size() * 4));
        a_fills = fills;
    }
    if (a_domains.count) {
        a_fill->bind();
        setView(*a_fill);
        a_domains.vao.bind();
        glDrawArrays(GL_TRIANGLES, 0, a_domains.count);
    }
    end(p);
}

void GlCanvas::drawBoundaries(QPainter& p, const std::vector<Stroke>& strokes)
{
    if (!a_view->a_topology) return;
    sync();
    const Topology& t = *a_view->a_topology;
    begin(p);
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
    drawLines(a_boundaries, true, true);
    end(p);
}

void GlCanvas::drawMesh(QPainter& p, QColor ink, float width)
{
    if (!a_view->a_mesh) return;
    sync();
    const Mesh& m = *a_view->a_mesh;
    begin(p);
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
    drawLines(a_mesh, false, true, &ink, width);
    end(p);
}

void GlCanvas::drawQuality(QPainter& p)
{
    if (!a_view->a_mesh) return;
    sync();
    const Mesh& m = *a_view->a_mesh;
    begin(p);
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
        if (!a_quality.vao.isCreated()) a_quality.vao.create();
        a_quality.vao.bind();
        if (!a_quality.vbo.isCreated()) a_quality.vbo.create();
        a_quality.vbo.bind();
        a_quality.vbo.allocate(pos.data(), int(pos.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        if (!a_quality.extra.isCreated()) a_quality.extra.create();
        a_quality.extra.bind();
        a_quality.extra.allocate(colors.data(), int(colors.size() * 4));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
        a_quality.vao.release();
        a_quality.count = int(colors.size());
        a_quality.built = true;
    }
    if (a_quality.count) {
        a_fill->bind();
        setView(*a_fill);
        a_quality.vao.bind();
        glDrawArrays(GL_TRIANGLES, 0, a_quality.count);
    }
    end(p);
}

void GlCanvas::drawSurface(QPainter& p)
{
    const PlotScene* plot = a_view->a_plot.get();
    if (!plot || !plot->surface) return;
    sync();
    begin(p);
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
        if (!a_surfaceBatch.vao.isCreated()) a_surfaceBatch.vao.create();
        a_surfaceBatch.vao.bind();
        if (!a_surfaceBatch.vbo.isCreated()) a_surfaceBatch.vbo.create();
        a_surfaceBatch.vbo.bind();
        a_surfaceBatch.vbo.allocate(data.data(), int(data.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        a_surfaceBatch.vao.release();
        a_surfaceBatch.count = int(data.size() / 3);
        a_surfaceBatch.built = true;
    }
    if (a_colormapName != plot->colors) {
        const std::vector<QRgb>& table = colorTable(plot->colors);
        std::vector<quint32> texels(256);
        for (std::size_t i = 0; i < 256; ++i) texels[i] = rgba(QColor::fromRgb(table[i]));
        glBindTexture(GL_TEXTURE_2D, a_colormap);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        a_colormapName = plot->colors;
    }
    if (a_surfaceBatch.count) {
        a_surface->bind();
        setView(*a_surface);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, a_colormap);
        a_surface->setUniformValue("colormap", 0);
        a_surfaceBatch.vao.bind();
        glDrawArrays(GL_TRIANGLES, 0, a_surfaceBatch.count);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    end(p);
}

void GlCanvas::drawContours(QPainter& p)
{
    const PlotScene* plot = a_view->a_plot.get();
    if (!plot || plot->contours.empty()) return;
    sync();
    begin(p);
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
    drawLines(a_contours, false, true);
    end(p);
}

void GlCanvas::drawRays(QPainter& p)
{
    const PlotScene* plot = a_view->a_plot.get();
    if (!plot || plot->rays.empty()) return;
    sync();
    begin(p);
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
    drawLines(a_rays, false, false);
    end(p);
}

} // namespace qucs_s::fem
