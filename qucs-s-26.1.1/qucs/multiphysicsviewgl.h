/*
 * multiphysicsviewgl.h - the multiphysics Graphics view drawn on the GPU:
 *                        its domains, mesh, color surface, boundaries,
 *                        contours and rays from buffers of OpenGL made once,
 *                        so a pan or a zoom costs the GPU a frame and the
 *                        CPU nothing; the text over them (the grid's labels,
 *                        the legend, the title) by QPainter as before
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSVIEWGL_H
#define QUCS_MULTIPHYSICSVIEWGL_H

#include <QColor>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPointF>

#include <memory>
#include <vector>

class QPainter;

namespace qucs_s::fem {

class GraphicsView;

/*!
 * The GPU's canvas over a GraphicsView, the view's size: it paints through
 * the view's own painting (GraphicsView::paintFrame), which hands it the
 * heavy layers between its QPainter's native painting. Mouse and keys go
 * to the view under it.
 */
class GlCanvas : public QOpenGLWidget, protected QOpenGLExtraFunctions
{
public:
    explicit GlCanvas(GraphicsView* view);
    ~GlCanvas() override;

    /// Whether an OpenGL 3.3 core context can be made here (not on the
    /// offscreen or minimal platforms).
    static bool available();
    /// Its shaders did not build, or its context was lost: the view draws
    /// itself again.
    bool failed() const { return a_failed; }
    /// The frames it has drawn (for the tests).
    int frames() const { return a_frames; }

    struct Stroke {
        QColor color;
        float width = 1;   // device pixels (a cosmetic pen's)
        bool operator==(const Stroke& o) const { return color == o.color && width == o.width; }
    };
    // Drawn now, in the view's painting (its QPainter's native painting
    // around each).
    void drawDomains(QPainter& p, const std::vector<QColor>& fills);
    void drawBoundaries(QPainter& p, const std::vector<Stroke>& strokes);
    void drawMesh(QPainter& p, QColor ink, float width);
    void drawQuality(QPainter& p);
    void drawSurface(QPainter& p);
    void drawContours(QPainter& p);
    void drawRays(QPainter& p);

protected:
    void initializeGL() override;
    void paintGL() override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct Batch {
        QOpenGLVertexArrayObject vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer extra{QOpenGLBuffer::VertexBuffer};   // colors, values
        int count = 0;                                       // vertices or instances
        bool built = false;
    };
    void release();
    void begin(QPainter& p);
    void end(QPainter& p);
    void setView(QOpenGLShaderProgram& program);
    void sync();
    void buildLines(Batch& b, const std::vector<float>& segments, const std::vector<quint32>& colors, const std::vector<float>& widths);
    /// \a cosmetic: widths in device pixels, as QPainter's cosmetic pens;
    /// else in the view's.
    void drawLines(Batch& b, bool roundCaps, bool cosmetic, const QColor* color = nullptr, float width = 1);

    GraphicsView* a_view;
    QMetaObject::Connection a_contextLost;
    bool a_failed = false;
    bool a_ready = false;
    int a_frames = 0;
    std::unique_ptr<QOpenGLShaderProgram> a_fill, a_surface, a_lines;
    GLuint a_colormap = 0;
    QString a_colormapName;
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

#endif // QUCS_MULTIPHYSICSVIEWGL_H
