/*
 * view3d.h - a 3D view drawn on the CPU: triangles shaded by a light at the
 *            eye, lines and dots over them, each hidden where something is
 *            in front (a depth buffer of our own); an orthographic camera
 *            orbited, panned and zoomed by the mouse, set from the top, the
 *            front, the side or a corner; a half-space clipped away
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_VIEW3D_H
#define QUCS_VIEW3D_H

#include "fem3_geometry.h"

#include <QColor>
#include <QImage>
#include <QWidget>

#include <array>
#include <vector>

class View3D : public QWidget
{
    Q_OBJECT

public:
    struct Triangle {
        std::array<qucs_s::fem::Vec3, 3> p;
        QColor color;
        int id = -1;     ///< what it is of (a face): picked as such
    };
    struct Line {
        qucs_s::fem::Vec3 a, b;
        QColor color;
        float width = 1;
        int id = -1;
    };
    struct Dot {
        qucs_s::fem::Vec3 p;
        QColor color;
        float radius = 2;
        int id = -1;
    };
    struct Scene {
        std::vector<Triangle> triangles;
        std::vector<Line> lines;
        std::vector<Dot> dots;
        qucs_s::fem::Box3 bounds;   ///< what Fit frames (empty: the triangles')
    };

    explicit View3D(QWidget* parent = nullptr);

    /// What it draws; the camera kept, or (\a fit) framing it.
    void setScene(Scene scene, bool fit = false);
    const Scene& scene() const { return a_scene; }
    /// A word in the middle where there is nothing to draw.
    void setEmptyText(const QString& text);

    enum class Preset { Top, Front, Side, Iso };
    void viewFrom(Preset preset);
    /// The whole scene framed, the direction kept.
    void fit();
    void zoomBy(double factor);
    /// Pixels in a unit of the geometry.
    double scale() const { return a_scale; }
    /// The direction it looks from: azimuth about z from +x and elevation
    /// above the plane, degrees.
    double azimuth() const { return a_azimuth; }
    double elevation() const { return a_elevation; }
    void setDirection(double azimuth, double elevation);
    /// What lies beyond \a at along \a axis (0 x, 1 y, 2 z; -1: nothing) cut
    /// away - \a at a share of the scene's extent along it, 0 to 1.
    void setClip(int axis, double at);
    int clipAxis() const { return a_clipAxis; }
    /// The view as it is drawn, at \a scale times its pixels.
    QImage picture(double scale = 1);
    /// The scene's id at a point of the widget - the frontmost triangle's
    /// there - or -1.
    int idAt(QPoint p);

    QSize sizeHint() const override { return QSize(640, 480); }
    QSize minimumSizeHint() const override { return QSize(120, 90); }

signals:
    void cameraChanged();
    /// A click (not a drag) at \a p: the id there, -1 none.
    void clicked(int id, QPoint p, Qt::KeyboardModifiers modifiers);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Camera {
        qucs_s::fem::Vec3 right, up, toward;   ///< the screen's axes in the model's
    };
    Camera camera() const;
    /// \a q in pixels of an image \a w by \a h at \a dpr: x right, y down, z
    /// toward the eye.
    qucs_s::fem::Vec3 project(const qucs_s::fem::Vec3& q, const Camera& c, double w, double h, double dpr) const;
    /// Draws the scene into \a image, its ids into \a ids where given.
    void render(QImage& image, std::vector<int>* ids) const;
    void invalidate();

    Scene a_scene;
    QString a_emptyText;
    double a_azimuth = -55, a_elevation = 30;
    qucs_s::fem::Vec3 a_center;
    double a_scale = 1;
    int a_clipAxis = -1;
    double a_clipAt = 0.5;
    QImage a_cache;
    bool a_dirty = true;
    QPoint a_press, a_last;
    Qt::MouseButton a_button = Qt::NoButton;
    bool a_dragged = false;
};

#endif // QUCS_VIEW3D_H
