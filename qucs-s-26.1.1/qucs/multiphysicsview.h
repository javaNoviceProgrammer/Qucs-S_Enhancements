/*
 * multiphysicsview.h - a multiphysics model's Graphics view, as COMSOL's:
 *                      its geometry (domains, boundaries, points - picked
 *                      with a click, those of the node chosen shown), its
 *                      mesh, its results (a color map with its legend,
 *                      contours, arrows); panned, zoomed, the value under
 *                      the pointer
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSVIEW_H
#define QUCS_MULTIPHYSICSVIEW_H

#include "fem_results.h"

#include <QImage>
#include <QPainterPath>
#include <QWidget>

#include <memory>
#include <vector>

namespace qucs_s::fem {

/// A color table: 256 colors, from the least value to the greatest.
const std::vector<QRgb>& colorTable(const QString& name);

/*!
 * What a 2D Plot Group shows, worked out from a solution: a surface's
 * values in color, contours, arrows, the mesh.
 */
struct PlotScene {
    QString title;
    bool surface = false;
    SurfaceData values;
    QString colors = QStringLiteral("rainbow");
    QString surfaceLabel;            ///< "V (V)", for the legend
    double min = 0, max = 1;         ///< the color range
    struct Contours {
        ContourData data;
        QString colors;              ///< "single" or a color table
        QColor color;
        double min = 0, max = 1;
    };
    std::vector<Contours> contours;
    struct Arrows {
        ArrowData data;
        QColor color;
        bool normalized = false;
        double spacing = 1;          ///< the grid's step (the geometry's unit)
    };
    std::vector<Arrows> arrows;
    bool mesh = false;
    bool quality = false;
    QStringList problems;
};

class GraphicsView : public QWidget
{
    Q_OBJECT

public:
    explicit GraphicsView(QWidget* parent = nullptr);

    enum class Show { Geometry, Mesh, Results };
    Show show() const { return a_show; }
    void setShow(Show show);

    void setTopology(std::shared_ptr<const Topology> topology);
    std::shared_ptr<const Topology> topology() const { return a_topology; }
    void setMesh(std::shared_ptr<const Mesh> mesh);
    std::shared_ptr<const Mesh> mesh() const { return a_mesh; }
    void setPlot(std::shared_ptr<const PlotScene> plot);
    std::shared_ptr<const PlotScene> plot() const { return a_plot; }

    /// The entities of \a level shown as chosen (the node's selection).
    void setHighlight(Level level, const QVector<int>& entities);
    Level highlightLevel() const { return a_highlightLevel; }
    const QVector<int>& highlighted() const { return a_highlight; }
    /// What a click picks (Level::None: nothing; the view is panned).
    void setPickLevel(Level level);
    Level pickLevel() const { return a_pickLevel; }
    /// The entity under the pointer, of the pick level (-1: none).
    int hovered() const { return a_hovered; }
    /// The entity of \a level at \a pixel, or -1.
    int entityAt(Level level, QPointF pixel) const;

    /// Pixels per unit of the geometry; the point in the middle.
    double scale() const { return a_scale; }
    QPointF center() const { return a_center; }
    void setView(QPointF center, double scale);
    void fit();
    void zoomBy(double factor, QPointF anchor = QPointF(-1, -1));
    QPointF toModel(QPointF pixel) const;
    QPointF toPixel(QPointF point) const;
    /// What is drawn now, as an image.
    QImage picture();
    /// The surface's rasterizations so far (for the tests: the cache works).
    int rasters() const { return a_rasters; }

signals:
    /// A click on an entity of the pick level (\a entity -1: on none).
    void picked(qucs_s::fem::Level level, int entity);
    void pointerMoved(QPointF point, bool inside);
    void viewChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Colors {
        QColor background, ink, faint, boundary, highlight, hover;
    };
    Colors colors() const;
    QRectF contentBounds() const;
    void rebuildPaths();
    void paintGrid(QPainter& p, const Colors& c) const;
    void paintGeometry(QPainter& p, const Colors& c, bool faded);
    void paintMesh(QPainter& p, const Colors& c, bool quality);
    void paintResults(QPainter& p, const Colors& c);
    void paintLegend(QPainter& p, const Colors& c, const QString& label, const QString& colors, double lo, double hi);
    void paintArrows(QPainter& p, const PlotScene::Arrows& arrows);
    void rasterize();
    QTransform modelToPixel() const;

    Show a_show = Show::Geometry;
    std::shared_ptr<const Topology> a_topology;
    std::shared_ptr<const Mesh> a_mesh;
    std::shared_ptr<const PlotScene> a_plot;
    std::vector<QPainterPath> a_domainPaths;     // the geometry's unit
    std::vector<QPainterPath> a_boundaryPaths;
    QPainterPath a_meshPath;
    std::vector<QPainterPath> a_contourPaths;    // by contour plot and level, flattened
    std::vector<QColor> a_contourColors;

    Level a_highlightLevel = Level::None;
    QVector<int> a_highlight;
    Level a_pickLevel = Level::None;
    int a_hovered = -1;

    QPointF a_center;
    double a_scale = 1;
    bool a_fitted = false;

    QImage a_raster;                 // the surface, at the view's pixels
    bool a_rasterDirty = true;
    int a_rasters = 0;
    QImage a_qualityRaster;

    QPoint a_pressPos, a_lastPos;
    bool a_dragging = false;
    bool a_pressed = false;
};

} // namespace qucs_s::fem

#endif // QUCS_MULTIPHYSICSVIEW_H
