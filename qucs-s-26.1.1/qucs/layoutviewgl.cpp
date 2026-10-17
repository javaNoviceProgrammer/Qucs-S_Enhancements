/*
 * layoutviewgl.cpp - the layout viewer drawn on the GPU
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "layoutviewgl.h"

#include "layoutdoc.h"

#include <CDT.h>

#include <QFontMetricsF>
#include <QImage>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QVector2D>

#include <algorithm>
#include <cmath>

namespace qucs_s::layout {

namespace {

// Fills and outlines: µm from the origin to clip space, y upwards as the
// layout has it. A fill's pattern Qt's 8 by 8 bitmap in device pixels from
// the top left, as QPainter draws a brush's.
// (Lines half a pixel to the right - nudge, in clip space - as QPainter
// draws an aliased one: on the pixels right of its mathematical place;
// fills none.)
const char* shapeVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
uniform vec2 center;
uniform vec2 scale;
uniform vec2 nudge;
void main() { gl_Position = vec4((position - center) * scale + nudge, 0.0, 1.0); }
)";
const char* shapeFragment = R"(#version 330 core
uniform vec4 color;
uniform bool patterned;
uniform sampler2D pattern;
uniform float cell;      // a pattern's pixel in device pixels
uniform float height;    // the framebuffer's
out vec4 fragColor;
void main()
{
    if (patterned) {
        vec2 at = floor(vec2(gl_FragCoord.x, height - gl_FragCoord.y) / cell);
        if (texelFetch(pattern, ivec2(mod(at, 8.0)), 0).r < 0.5) discard;
    }
    fragColor = color;
}
)";
// A shape's dot: where the shape is under 1.5 pixels across, a pixel at
// its middle (else none).
const char* dotVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in float size;
uniform vec2 center;
uniform vec2 scale;
uniform float pixels;    // per µm
uniform vec2 nudge;
void main()
{
    gl_Position = size * pixels < 1.5 ? vec4((position - center) * scale + nudge, 0.0, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 1.0;
}
)";
const char* dotFragment = R"(#version 330 core
uniform vec4 color;
out vec4 fragColor;
void main() { fragColor = color; }
)";

QSurfaceFormat canvasFormat()
{
    QSurfaceFormat f = QSurfaceFormat::defaultFormat();
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    // (No multisampling: QPainter draws the layout without antialiasing.)
    f.setSamples(0);
    return f;
}

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

LayoutGl::LayoutGl(LayoutView* view) : QOpenGLWidget(view), a_view(view)
{
    setObjectName(QStringLiteral("layoutCanvas"));
    setFormat(canvasFormat());
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(view->rect());
}

LayoutGl::~LayoutGl()
{
    disconnect(a_contextLost);
    if (a_ready) {
        makeCurrent();
        release();
        doneCurrent();
    }
}

void LayoutGl::release()
{
    auto drop = [](Batch& b) {
        b.vao.destroy();
        b.vbo.destroy();
        b.count = 0;
    };
    for (auto& l : a_layers) {
        drop(l->fills);
        drop(l->outlines);
        drop(l->dots);
    }
    a_layers.clear();
    drop(a_frameLines);
    drop(a_frameDots);
    for (unsigned t : std::as_const(a_patterns)) {
        GLuint id = t;
        glDeleteTextures(1, &id);
    }
    a_patterns.clear();
    a_fill.reset();
    a_dot.reset();
    a_madeOf = nullptr;
    a_cell = -2;
    a_ready = false;
}

void LayoutGl::initializeGL()
{
    initializeOpenGLFunctions();
    disconnect(a_contextLost);
    a_contextLost = connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
        makeCurrent();
        release();
        doneCurrent();
    });
    auto program = [](const char* vertex, const char* fragment) {
        auto p = std::make_unique<QOpenGLShaderProgram>();
        if (!p->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) || !p->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)
            || !p->link()) {
            qWarning("Layout view: OpenGL shaders: %s", qPrintable(p->log()));
            return std::unique_ptr<QOpenGLShaderProgram>();
        }
        return p;
    };
    a_fill = program(shapeVertex, shapeFragment);
    a_dot = program(dotVertex, dotFragment);
    if (!a_fill || !a_dot) {
        a_failed = true;
        QMetaObject::invokeMethod(a_view, [v = a_view] { v->canvasFailed(); }, Qt::QueuedConnection);
        return;
    }
    a_madeOf = nullptr;
    a_cell = -2;
    a_ready = true;
}

void LayoutGl::paintEvent(QPaintEvent* event)
{
    QOpenGLWidget::paintEvent(event);
    if (!isValid() && !a_failed) {
        qWarning("Layout view: no OpenGL context; drawn by the CPU");
        a_failed = true;
        QMetaObject::invokeMethod(a_view, [v = a_view] { v->canvasFailed(); }, Qt::QueuedConnection);
    }
}

void LayoutGl::paintGL()
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

void LayoutGl::upload(Batch& b, const std::vector<float>& data, int components, bool withSize)
{
    if (!b.vao.isCreated()) b.vao.create();
    b.vao.bind();
    if (!b.vbo.isCreated()) b.vbo.create();
    b.vbo.bind();
    b.vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
    b.vbo.allocate(data.data(), int(data.size() * sizeof(float)));
    const int stride = components * int(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    if (withSize) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(2 * sizeof(float)));
    }
    b.count = int(data.size()) / components;
    b.vao.release();
}

void LayoutGl::sync()
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
    const bool same = layout == a_madeOf && a_view->a_cell == a_cell && a_view->a_depth == a_depth && a_view->a_labels == a_labels;
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
                upload(a_layers[i]->fills, a_flat.layers[i].fills, 2, false);
                upload(a_layers[i]->outlines, a_flat.layers[i].outlines, 2, false);
                upload(a_layers[i]->dots, a_flat.layers[i].dots, 3, true);
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
        upload(a_layers[i]->fills, has ? a_flat.layers[i].fills : none, 2, false);
        upload(a_layers[i]->outlines, has ? a_flat.layers[i].outlines : none, 2, false);
        upload(a_layers[i]->dots, has ? a_flat.layers[i].dots : none, 3, true);
    }
    upload(a_frameLines, a_flat.frames, 2, false);
    upload(a_frameDots, a_flat.frameDots, 3, true);
    // (The CPU's arrays freed: the GPU has them.)
    for (Flattened::Layer& l : a_flat.layers) {
        std::vector<float>().swap(l.fills);
        std::vector<float>().swap(l.outlines);
        std::vector<float>().swap(l.dots);
    }
    std::vector<float>().swap(a_flat.frames);
    std::vector<float>().swap(a_flat.frameDots);
}

unsigned LayoutGl::patternTexture(Qt::BrushStyle style)
{
    if (auto it = a_patterns.constFind(int(style)); it != a_patterns.constEnd()) return *it;
    // Qt's own bitmap: its brush drawn on 8 by 8 pixels.
    QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        p.fillRect(image.rect(), QBrush(Qt::black, style));
    }
    unsigned char bits[64];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) bits[y * 8 + x] = qAlpha(image.pixel(x, y)) > 127 ? 255 : 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 8, 8, 0, GL_RED, GL_UNSIGNED_BYTE, bits);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    // (Qt's own uploads - the glyphs QPainter draws texts with after -
    // take the default alignment.)
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    a_patterns.insert(int(style), id);
    return id;
}

bool LayoutGl::drawLayout(QPainter& p, RenderStats* stats)
{
    sync();
    if (a_flat.tooLarge) return false;
    const LayoutView::Colors c = a_view->colors();
    const double ratio = devicePixelRatioF();
    const double s = a_view->a_scale;
    const QPointF center = a_view->a_center - a_flat.origin;
    const QVector2D centerUniform(float(center.x()), float(center.y()));
    const QVector2D scaleUniform(float(2 * s / std::max(1, width())), float(2 * s / std::max(1, height())));
    const auto color = [](QColor k) { return QVector4D(float(k.redF()), float(k.greenF()), float(k.blueF()), float(k.alphaF())); };
    // (half a device pixel right, in clip space: an edge on a pixel boundary
    // there inked as QPainter inks it; down none - the framebuffer's rows
    // upwards, an edge on their boundary falls as QPainter's already)
    const QVector2D nudge(float(1.0 / std::max(1.0, width() * ratio)), 0.0f);
    p.beginNativePainting();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.0f);
    const float height = float(std::lround(this->height() * ratio));
    for (std::size_t i = 0; i < a_layers.size(); ++i) {
        LayerBatches& l = *a_layers[i];
        if (l.fills.count == 0 && l.outlines.count == 0 && l.dots.count == 0) continue;
        const LayerStyle raw = int(i) < a_view->a_styles.size() ? a_view->a_styles.at(int(i)) : paletteStyle(int(i));
        if (!raw.visible) continue;
        const LayerStyle style = a_view->shownStyle(raw);
        a_fill->bind();
        a_fill->setUniformValue("center", centerUniform);
        a_fill->setUniformValue("scale", scaleUniform);
        a_fill->setUniformValue("height", height);
        // (a pattern's pixel a device pixel, as QPainter has it on a picture
        // of the screen's pixel ratio)
        a_fill->setUniformValue("cell", 1.0f);
        if (style.pattern != Qt::NoBrush && l.fills.count > 0) {
            const bool patterned = style.pattern != Qt::SolidPattern;
            a_fill->setUniformValue("patterned", patterned);
            if (patterned) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, patternTexture(style.pattern));
                a_fill->setUniformValue("pattern", 0);
            }
            a_fill->setUniformValue("color", color(style.fill));
            a_fill->setUniformValue("nudge", QVector2D());
            l.fills.vao.bind();
            glDrawArrays(GL_TRIANGLES, 0, l.fills.count);
        }
        a_fill->setUniformValue("patterned", false);
        a_fill->setUniformValue("color", color(style.frame));
        a_fill->setUniformValue("nudge", nudge);
        if (l.outlines.count > 0) {
            l.outlines.vao.bind();
            glDrawArrays(GL_LINES, 0, l.outlines.count);
        }
        if (l.dots.count > 0) {
            a_dot->bind();
            a_dot->setUniformValue("center", centerUniform);
            a_dot->setUniformValue("scale", scaleUniform);
            a_dot->setUniformValue("pixels", float(s * ratio));
            a_dot->setUniformValue("color", color(style.frame));
            a_dot->setUniformValue("nudge", nudge);
            l.dots.vao.bind();
            glDrawArrays(GL_POINTS, 0, l.dots.count);
        }
    }
    const QColor frameColor = c.frames.isValid() ? c.frames : QColor(Qt::gray);
    if (a_frameLines.count > 0) {
        a_fill->bind();
        a_fill->setUniformValue("center", centerUniform);
        a_fill->setUniformValue("scale", scaleUniform);
        a_fill->setUniformValue("patterned", false);
        a_fill->setUniformValue("color", color(frameColor));
        a_fill->setUniformValue("nudge", nudge);
        a_frameLines.vao.bind();
        glDrawArrays(GL_LINES, 0, a_frameLines.count);
        a_dot->bind();
        a_dot->setUniformValue("center", centerUniform);
        a_dot->setUniformValue("scale", scaleUniform);
        a_dot->setUniformValue("pixels", float(s * ratio));
        a_dot->setUniformValue("color", color(frameColor));
        a_dot->setUniformValue("nudge", nudge);
        a_frameDots.vao.bind();
        glDrawArrays(GL_POINTS, 0, a_frameDots.count);
    }
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    p.endNativePainting();
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
