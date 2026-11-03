/*
 * view3d.cpp - a 3D view drawn on the CPU: a depth buffer of our own under
 *              shaded triangles, lines and dots
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "view3d.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

using qucs_s::fem::Box3;
using qucs_s::fem::Vec3;

namespace {

double edgeFunction(const Vec3& a, const Vec3& b, double px, double py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

int clampByte(double v)
{
    return std::clamp(int(std::lround(v)), 0, 255);
}

} // namespace

View3D::View3D(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("View3D"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void View3D::setScene(Scene scene, bool fitIt)
{
    a_scene = std::move(scene);
    if (a_scene.bounds.isEmpty()) {
        for (const Triangle& t : a_scene.triangles)
            for (const Vec3& p : t.p) a_scene.bounds.add(p);
        for (const Line& l : a_scene.lines) {
            a_scene.bounds.add(l.a);
            a_scene.bounds.add(l.b);
        }
        for (const Dot& d : a_scene.dots) a_scene.bounds.add(d.p);
    }
    if (fitIt) fit();
    else invalidate();
}

void View3D::setEmptyText(const QString& text)
{
    a_emptyText = text;
    update();
}

void View3D::viewFrom(Preset preset)
{
    switch (preset) {
    case Preset::Top: setDirection(-90, 90); break;
    case Preset::Front: setDirection(-90, 0); break;
    case Preset::Side: setDirection(0, 0); break;
    case Preset::Iso: setDirection(-55, 30); break;
    }
    fit();
}

void View3D::setDirection(double azimuth, double elevation)
{
    a_azimuth = azimuth;
    a_elevation = std::clamp(elevation, -90.0, 90.0);
    invalidate();
    emit cameraChanged();
}

View3D::Camera View3D::camera() const
{
    const double phi = qDegreesToRadians(a_azimuth), theta = qDegreesToRadians(a_elevation);
    Camera c;
    c.toward = Vec3{std::cos(theta) * std::cos(phi), std::cos(theta) * std::sin(phi), std::sin(theta)};
    c.right = Vec3{-std::sin(phi), std::cos(phi), 0};
    c.up = Vec3{-std::sin(theta) * std::cos(phi), -std::sin(theta) * std::sin(phi), std::cos(theta)};
    return c;
}

Vec3 View3D::project(const Vec3& q, const Camera& c, double w, double h, double dpr) const
{
    const Vec3 d = q - a_center;
    const double k = a_scale * dpr;
    return Vec3{w / 2 + k * d.dot(c.right), h / 2 - k * d.dot(c.up), d.dot(c.toward)};
}

void View3D::fit()
{
    const Box3& b = a_scene.bounds;
    if (b.isEmpty()) {
        a_center = Vec3{};
        a_scale = 1;
        invalidate();
        emit cameraChanged();
        return;
    }
    a_center = b.center();
    const Camera c = camera();
    double r0 = INFINITY, r1 = -INFINITY, u0 = INFINITY, u1 = -INFINITY;
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{i & 1 ? b.x1 : b.x0, i & 2 ? b.y1 : b.y0, i & 4 ? b.z1 : b.z0};
        const Vec3 d = corner - a_center;
        r0 = std::min(r0, d.dot(c.right));
        r1 = std::max(r1, d.dot(c.right));
        u0 = std::min(u0, d.dot(c.up));
        u1 = std::max(u1, d.dot(c.up));
    }
    // (Centred on what is seen of it, not on the box's middle alone.)
    a_center = a_center + c.right * ((r0 + r1) / 2) + c.up * ((u0 + u1) / 2);
    const double sr = std::max(r1 - r0, 1e-300), su = std::max(u1 - u0, 1e-300);
    a_scale = 0.88 * std::min(std::max(width(), 1) / sr, std::max(height(), 1) / su);
    if (!std::isfinite(a_scale) || a_scale <= 0) a_scale = 1;
    invalidate();
    emit cameraChanged();
}

void View3D::zoomBy(double factor)
{
    if (!(factor > 0)) return;
    a_scale *= factor;
    invalidate();
    emit cameraChanged();
}

void View3D::setClip(int axis, double at)
{
    a_clipAxis = axis >= 0 && axis <= 2 ? axis : -1;
    a_clipAt = std::clamp(at, 0.0, 1.0);
    invalidate();
}

void View3D::invalidate()
{
    a_dirty = true;
    update();
}

void View3D::render(QImage& image, std::vector<int>* ids) const
{
    const int W = image.width(), H = image.height();
    const double dpr = image.devicePixelRatio();
    image.fill(palette().color(QPalette::Base));
    if (W <= 0 || H <= 0) return;
    std::vector<float> depth(std::size_t(W) * std::size_t(H), -std::numeric_limits<float>::infinity());
    if (ids) ids->assign(std::size_t(W) * std::size_t(H), -1);
    const Camera c = camera();
    const Box3& b = a_scene.bounds;
    const bool clipping = a_clipAxis >= 0 && !b.isEmpty();
    double clipValue = 0;
    if (clipping) {
        const double lo = a_clipAxis == 0 ? b.x0 : a_clipAxis == 1 ? b.y0 : b.z0;
        const double hi = a_clipAxis == 0 ? b.x1 : a_clipAxis == 1 ? b.y1 : b.z1;
        clipValue = lo + (hi - lo) * a_clipAt;
    }
    auto along = [this](const Vec3& p) { return a_clipAxis == 0 ? p.x : a_clipAxis == 1 ? p.y : p.z; };
    auto line = [&](int y) { return reinterpret_cast<QRgb*>(image.scanLine(y)); };
    // Triangles: shaded by the light at the eye, both sides alike (a face
    // between two domains is seen from either).
    for (const Triangle& t : a_scene.triangles) {
        const Vec3 s0 = project(t.p[0], c, W, H, dpr), s1 = project(t.p[1], c, W, H, dpr), s2 = project(t.p[2], c, W, H, dpr);
        double c0 = 0, c1 = 0, c2 = 0;
        if (clipping) {
            c0 = along(t.p[0]);
            c1 = along(t.p[1]);
            c2 = along(t.p[2]);
            if (c0 > clipValue && c1 > clipValue && c2 > clipValue) continue;
        }
        const double area = edgeFunction(s0, s1, s2.x, s2.y);
        if (!(std::abs(area) > 1e-12)) continue;
        const Vec3 n = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalized();
        const double light = 0.38 + 0.62 * std::abs(n.dot(c.toward));
        const QRgb rgb = qRgb(clampByte(t.color.red() * light), clampByte(t.color.green() * light), clampByte(t.color.blue() * light));
        const int x0 = std::max(0, int(std::floor(std::min({s0.x, s1.x, s2.x}))));
        const int x1 = std::min(W - 1, int(std::ceil(std::max({s0.x, s1.x, s2.x}))));
        const int y0 = std::max(0, int(std::floor(std::min({s0.y, s1.y, s2.y}))));
        const int y1 = std::min(H - 1, int(std::ceil(std::max({s0.y, s1.y, s2.y}))));
        for (int y = y0; y <= y1; ++y) {
            QRgb* row = line(y);
            const double py = y + 0.5;
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5;
                const double w0 = edgeFunction(s1, s2, px, py) / area;
                const double w1 = edgeFunction(s2, s0, px, py) / area;
                const double w2 = 1 - w0 - w1;
                if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
                if (clipping && w0 * c0 + w1 * c1 + w2 * c2 > clipValue) continue;
                const float z = float(w0 * s0.z + w1 * s1.z + w2 * s2.z);
                float& d = depth[std::size_t(y) * std::size_t(W) + std::size_t(x)];
                if (z <= d) continue;
                d = z;
                row[x] = rgb;
                if (ids) (*ids)[std::size_t(y) * std::size_t(W) + std::size_t(x)] = t.id;
            }
        }
    }
    if (ids) return;
    // Lines and dots over them: hidden where a triangle is in front, a
    // little drawn toward the eye so that an edge on a face shows.
    const double pixel = 1 / std::max(a_scale * dpr, 1e-300);
    const double bias = 2.5 * pixel + 1e-9 * std::max(1.0, b.diagonal());
    auto plot = [&](int x, int y, double z, QRgb rgb) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        float& d = depth[std::size_t(y) * std::size_t(W) + std::size_t(x)];
        if (z + bias < d) return;
        d = std::max(d, float(z));
        line(y)[x] = rgb;
    };
    for (const Line& l : a_scene.lines) {
        Vec3 pa = l.a, pb = l.b;
        if (clipping) {
            const double ca = along(pa), cb = along(pb);
            if (ca > clipValue && cb > clipValue) continue;
            if (ca > clipValue) pa = pb + (pa - pb) * ((clipValue - cb) / (ca - cb));
            else if (cb > clipValue) pb = pa + (pb - pa) * ((clipValue - ca) / (cb - ca));
        }
        const Vec3 a = project(pa, c, W, H, dpr), e = project(pb, c, W, H, dpr);
        const QRgb rgb = l.color.rgb();
        const int steps = std::max(1, int(std::ceil(std::max(std::abs(e.x - a.x), std::abs(e.y - a.y)))));
        if (steps > 100000) continue;
        const int thick = std::max(1, int(std::lround(l.width * dpr)));
        for (int i = 0; i <= steps; ++i) {
            const double u = double(i) / steps;
            const int x = int(std::floor(a.x + (e.x - a.x) * u)), y = int(std::floor(a.y + (e.y - a.y) * u));
            const double z = a.z + (e.z - a.z) * u;
            for (int dy = 0; dy < thick; ++dy)
                for (int dx = 0; dx < thick; ++dx) plot(x + dx - thick / 2, y + dy - thick / 2, z, rgb);
        }
    }
    for (const Dot& dot : a_scene.dots) {
        if (clipping && along(dot.p) > clipValue) continue;
        const Vec3 s = project(dot.p, c, W, H, dpr);
        const int r = std::max(1, int(std::lround(dot.radius * dpr)));
        const QRgb rgb = dot.color.rgb();
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                if (dx * dx + dy * dy <= r * r) plot(int(std::floor(s.x)) + dx, int(std::floor(s.y)) + dy, s.z + 2 * r * pixel, rgb);
    }
}

void View3D::paintEvent(QPaintEvent*)
{
    const qreal dpr = devicePixelRatioF();
    const QSize pixels = (QSizeF(size()) * dpr).toSize();
    if (a_dirty || a_cache.size() != pixels) {
        a_cache = QImage(pixels, QImage::Format_RGB32);
        a_cache.setDevicePixelRatio(dpr);
        render(a_cache, nullptr);
        a_dirty = false;
    }
    QPainter p(this);
    p.drawImage(QPointF(0, 0), a_cache);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor text = palette().color(QPalette::Text);
    if (a_scene.triangles.empty() && a_scene.lines.empty() && !a_emptyText.isEmpty()) {
        p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, a_emptyText);
    }
    // The axes as the camera turns them, at the bottom left.
    const Camera c = camera();
    const QPointF origin(34, height() - 34);
    struct Axis {
        Vec3 v;
        QColor color;
        QString name;
    };
    const Axis axes[3] = {{{1, 0, 0}, QColor(214, 48, 49), QStringLiteral("x")},
                          {{0, 1, 0}, QColor(46, 160, 67), QStringLiteral("y")},
                          {{0, 0, 1}, QColor(41, 98, 255), QStringLiteral("z")}};
    for (const Axis& a : axes) {
        const QPointF tip = origin + QPointF(a.v.dot(c.right), -a.v.dot(c.up)) * 24;
        p.setPen(QPen(a.color, 2));
        p.drawLine(origin, tip);
        p.setPen(a.color);
        p.drawText(QRectF(tip - QPointF(8, 8), QSizeF(16, 16)), Qt::AlignCenter, a.name);
    }
    if (a_clipAxis >= 0) {
        p.setPen(text);
        p.drawText(QRectF(8, 6, width() - 16, 18), Qt::AlignRight | Qt::AlignVCenter,
                   tr("Cut at %1 of %2").arg(QStringLiteral("%1 %").arg(qRound(a_clipAt * 100)),
                                             a_clipAxis == 0 ? QStringLiteral("x") : a_clipAxis == 1 ? QStringLiteral("y") : QStringLiteral("z")));
    }
}

QImage View3D::picture(double scale)
{
    const double k = scale > 0 ? scale : 1;
    QImage image((QSizeF(size()) * k).toSize(), QImage::Format_RGB32);
    image.setDevicePixelRatio(k);
    render(image, nullptr);
    return image;
}

int View3D::idAt(QPoint at)
{
    const qreal dpr = devicePixelRatioF();
    QImage image((QSizeF(size()) * dpr).toSize(), QImage::Format_RGB32);
    image.setDevicePixelRatio(dpr);
    std::vector<int> ids;
    render(image, &ids);
    const int x = int(at.x() * dpr), y = int(at.y() * dpr);
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) return -1;
    return ids[std::size_t(y) * std::size_t(image.width()) + std::size_t(x)];
}

void View3D::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    a_dirty = true;
}

void View3D::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) invalidate();
}

void View3D::mousePressEvent(QMouseEvent* event)
{
    a_press = a_last = event->position().toPoint();
    a_button = event->button();
    a_dragged = false;
    event->accept();
}

void View3D::mouseMoveEvent(QMouseEvent* event)
{
    if (a_button == Qt::NoButton) return;
    const QPoint now = event->position().toPoint();
    const QPoint d = now - a_last;
    a_last = now;
    if ((now - a_press).manhattanLength() > 3) a_dragged = true;
    if (!a_dragged) return;
    const bool pan = a_button == Qt::RightButton || a_button == Qt::MiddleButton || (event->modifiers() & Qt::ShiftModifier);
    if (pan) {
        const Camera c = camera();
        a_center = a_center - (c.right * double(d.x()) - c.up * double(d.y())) / a_scale;
        invalidate();
        emit cameraChanged();
    } else {
        // Turned as if the scene were held: dragged right, it turns right.
        setDirection(a_azimuth - 0.4 * d.x(), a_elevation + 0.4 * d.y());
    }
}

void View3D::mouseReleaseEvent(QMouseEvent* event)
{
    const bool click = !a_dragged && event->button() == a_button && a_button == Qt::LeftButton;
    a_button = Qt::NoButton;
    if (click) emit clicked(idAt(event->position().toPoint()), event->position().toPoint(), event->modifiers());
}

void View3D::wheelEvent(QWheelEvent* event)
{
    const double factor = std::pow(1.0015, event->angleDelta().y());
    if (!(factor > 0) || factor == 1) return;
    // The point under the pointer stays there.
    const Camera c = camera();
    const QPointF at = event->position() - QPointF(width() / 2.0, height() / 2.0);
    const Vec3 offset = c.right * at.x() - c.up * at.y();
    const Vec3 point = a_center + offset / a_scale;
    a_scale *= factor;
    a_center = point - offset / a_scale;
    invalidate();
    emit cameraChanged();
    event->accept();
}
