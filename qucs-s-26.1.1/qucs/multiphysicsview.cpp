/*
 * multiphysicsview.cpp - a multiphysics model's Graphics view
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysicsview.h"

#include "ink.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPalette>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <set>

namespace qucs_s::fem {

namespace {

struct Stop {
    double at;
    double r, g, b;
};

std::vector<QRgb> makeTable(std::initializer_list<Stop> stops)
{
    std::vector<QRgb> out(256);
    const std::vector<Stop> s(stops);
    for (int i = 0; i < 256; ++i) {
        const double t = i / 255.0;
        std::size_t k = 1;
        while (k + 1 < s.size() && s[k].at < t) ++k;
        const Stop& a = s[k - 1];
        const Stop& b = s[k];
        const double u = b.at > a.at ? std::clamp((t - a.at) / (b.at - a.at), 0.0, 1.0) : 0;
        out[std::size_t(i)] = qRgb(int(std::lround(255 * (a.r + u * (b.r - a.r)))), int(std::lround(255 * (a.g + u * (b.g - a.g)))),
                                   int(std::lround(255 * (a.b + u * (b.b - a.b)))));
    }
    return out;
}

/// A 1, 2 or 5 times a power of ten at least \a least.
double niceStep(double least)
{
    if (!(least > 0) || !std::isfinite(least)) return 1;
    const double p = std::pow(10.0, std::floor(std::log10(least)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * p >= least) return m * p;
    return 10 * p;
}

QColor domainColor(int d, bool dark)
{
    // Soft colors, one a domain, as COMSOL's materials appear.
    static const QRgb light[] = {0xc9d9ec, 0xe8d5b5, 0xc8e3c8, 0xe6c8d6, 0xd6d0ea, 0xcfe6e4, 0xeedcc4, 0xd9e2c0};
    static const QRgb darkPalette[] = {0x38506b, 0x6b5733, 0x3d6640, 0x66435a, 0x4f4a72, 0x3f6663, 0x6e5b3c, 0x58643b};
    const int n = int(sizeof(light) / sizeof(light[0]));
    return QColor::fromRgb(dark ? darkPalette[d % n] : light[d % n]);
}

} // namespace

const std::vector<QRgb>& colorTable(const QString& name)
{
    static const std::vector<QRgb> rainbow = makeTable({{0.0, 0.0, 0.0, 0.55}, {0.13, 0.0, 0.15, 1.0}, {0.34, 0.0, 0.8, 1.0},
                                                        {0.5, 0.25, 1.0, 0.45}, {0.66, 1.0, 1.0, 0.0}, {0.86, 1.0, 0.4, 0.0},
                                                        {1.0, 0.72, 0.0, 0.0}});
    static const std::vector<QRgb> viridis = makeTable({{0.0, 0.267, 0.005, 0.329}, {0.25, 0.229, 0.322, 0.546}, {0.5, 0.128, 0.567, 0.551},
                                                        {0.75, 0.369, 0.789, 0.383}, {1.0, 0.993, 0.906, 0.144}});
    static const std::vector<QRgb> thermal = makeTable({{0.0, 0.0, 0.0, 0.0}, {0.35, 0.62, 0.0, 0.0}, {0.65, 1.0, 0.5, 0.0},
                                                        {0.85, 1.0, 0.9, 0.2}, {1.0, 1.0, 1.0, 1.0}});
    static const std::vector<QRgb> coolwarm = makeTable({{0.0, 0.23, 0.30, 0.75}, {0.5, 0.87, 0.87, 0.87}, {1.0, 0.71, 0.016, 0.15}});
    static const std::vector<QRgb> gray = makeTable({{0.0, 0.0, 0.0, 0.0}, {1.0, 1.0, 1.0, 1.0}});
    if (name == QLatin1String("viridis")) return viridis;
    if (name == QLatin1String("thermal")) return thermal;
    if (name == QLatin1String("coolwarm")) return coolwarm;
    if (name == QLatin1String("gray")) return gray;
    return rainbow;
}

GraphicsView::GraphicsView(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("multiphysicsGraphics"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
}

void GraphicsView::setShow(Show show)
{
    if (a_show == show) return;
    a_show = show;
    a_rasterDirty = true;
    update();
}

void GraphicsView::setTopology(std::shared_ptr<const Topology> topology)
{
    const bool first = !a_topology && topology;
    a_topology = std::move(topology);
    rebuildPaths();
    if (first || !a_fitted) fit();
    update();
}

void GraphicsView::setMesh(std::shared_ptr<const Mesh> mesh)
{
    a_mesh = std::move(mesh);
    a_meshPath = QPainterPath();
    if (a_mesh) {
        std::set<std::pair<int, int>> edges;
        for (const auto& t : a_mesh->triangles)
            for (int k = 0; k < 3; ++k) {
                const int a = t[std::size_t(k)], b = t[std::size_t((k + 1) % 3)];
                edges.emplace(std::min(a, b), std::max(a, b));
            }
        for (const auto& [a, b] : edges) {
            a_meshPath.moveTo(a_mesh->nodes[std::size_t(a)]);
            a_meshPath.lineTo(a_mesh->nodes[std::size_t(b)]);
        }
    }
    a_qualityRaster = QImage();
    a_rasterDirty = true;
    update();
}

void GraphicsView::setPlot(std::shared_ptr<const PlotScene> plot)
{
    a_plot = std::move(plot);
    a_contourPaths.clear();
    a_contourColors.clear();
    if (a_plot) {
        for (const PlotScene::Contours& c : a_plot->contours) {
            const std::vector<QRgb>& table = colorTable(c.colors);
            for (std::size_t l = 0; l < c.data.levels.size(); ++l) {
                QPainterPath path;
                for (const QLineF& line : c.data.lines[l]) {
                    path.moveTo(line.p1());
                    path.lineTo(line.p2());
                }
                a_contourPaths.push_back(path);
                if (c.colors == QLatin1String("single")) a_contourColors.push_back(c.color);
                else {
                    const double span = c.max > c.min ? c.max - c.min : 1;
                    const int i = std::clamp(int((c.data.levels[l] - c.min) / span * 255), 0, 255);
                    a_contourColors.push_back(QColor::fromRgb(table[std::size_t(i)]));
                }
            }
        }
    }
    a_rasterDirty = true;
    update();
}

void GraphicsView::setHighlight(Level level, const QVector<int>& entities)
{
    if (a_highlightLevel == level && a_highlight == entities) return;
    a_highlightLevel = level;
    a_highlight = entities;
    update();
}

void GraphicsView::setPickLevel(Level level)
{
    a_pickLevel = level;
    a_hovered = -1;
    setCursor(level == Level::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
    update();
}

void GraphicsView::rebuildPaths()
{
    a_domainPaths.clear();
    a_boundaryPaths.clear();
    if (!a_topology) return;
    const Topology& t = *a_topology;
    for (const Topology::Domain& d : t.domains) {
        QPainterPath path;
        path.setFillRule(Qt::WindingFill);
        for (const auto& tri : d.triangles) {
            QPolygonF poly;
            poly << t.vertices[std::size_t(tri[0])] << t.vertices[std::size_t(tri[1])] << t.vertices[std::size_t(tri[2])];
            path.addPolygon(poly);
            path.closeSubpath();
        }
        a_domainPaths.push_back(path);
    }
    for (const Topology::Boundary& b : t.boundaries) {
        QPainterPath path;
        const std::vector<QPointF> pts = b.polyline(t);
        if (!pts.empty()) path.moveTo(pts.front());
        for (std::size_t i = 1; i < pts.size(); ++i) path.lineTo(pts[i]);
        a_boundaryPaths.push_back(path);
    }
}

GraphicsView::Colors GraphicsView::colors() const
{
    const QPalette pal = palette();
    Colors c;
    c.background = pal.color(QPalette::Base);
    c.ink = pal.color(QPalette::Text);
    const bool dark = qucs_s::ink::isDark(c.background);
    c.faint = dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 22);
    c.boundary = dark ? QColor(200, 205, 215) : QColor(60, 64, 72);
    c.highlight = dark ? QColor(80, 160, 255) : QColor(0, 102, 214);
    c.hover = dark ? QColor(255, 190, 70) : QColor(232, 120, 0);
    return c;
}

QRectF GraphicsView::contentBounds() const
{
    if (a_topology && !a_topology->bounds.isNull()) return a_topology->bounds;
    if (a_mesh) return a_mesh->bounds();
    return QRectF(-1, -1, 2, 2);
}

QTransform GraphicsView::modelToPixel() const
{
    QTransform m;
    m.translate(width() / 2.0, height() / 2.0);
    m.scale(a_scale, -a_scale);
    m.translate(-a_center.x(), -a_center.y());
    return m;
}

QPointF GraphicsView::toModel(QPointF pixel) const
{
    return QPointF((pixel.x() - width() / 2.0) / a_scale + a_center.x(), -(pixel.y() - height() / 2.0) / a_scale + a_center.y());
}

QPointF GraphicsView::toPixel(QPointF point) const
{
    return QPointF((point.x() - a_center.x()) * a_scale + width() / 2.0, height() / 2.0 - (point.y() - a_center.y()) * a_scale);
}

void GraphicsView::setView(QPointF center, double scale)
{
    if (!std::isfinite(scale) || scale <= 0) return;
    a_center = center;
    a_scale = std::clamp(scale, 1e-12, 1e15);
    a_rasterDirty = true;
    update();
    emit viewChanged();
}

void GraphicsView::fit()
{
    const QRectF b = contentBounds();
    const double w = std::max(b.width(), 1e-12), h = std::max(b.height(), 1e-12);
    // Room for the legend on the right, the scale's labels below.
    const double sx = (width() - 110.0) / w, sy = (height() - 60.0) / h;
    a_fitted = width() > 10 && height() > 10;
    setView(b.center() + QPointF(26.0 / std::max(1e-12, std::min(sx, sy)), 0), std::max(1e-12, std::min(sx, sy) * 0.92));
}

void GraphicsView::zoomBy(double factor, QPointF anchor)
{
    if (anchor.x() < 0) anchor = QPointF(width() / 2.0, height() / 2.0);
    const QPointF at = toModel(anchor);
    const double scale = std::clamp(a_scale * factor, 1e-12, 1e15);
    // The point under the anchor stays there.
    const QPointF center(at.x() - (anchor.x() - width() / 2.0) / scale, at.y() + (anchor.y() - height() / 2.0) / scale);
    setView(center, scale);
}

int GraphicsView::entityAt(Level level, QPointF pixel) const
{
    if (!a_topology) return -1;
    const Topology& t = *a_topology;
    const QPointF m = toModel(pixel);
    if (level == Level::Point) return t.pointAt(m, 8 / a_scale);
    if (level == Level::Boundary) return t.boundaryAt(m, 6 / a_scale);
    if (level == Level::Domain) return t.domainAt(m);
    return -1;
}

void GraphicsView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!a_fitted) fit();
    a_rasterDirty = true;
}

void GraphicsView::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        a_rasterDirty = true;
        update();
    }
}

void GraphicsView::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0) zoomBy(std::pow(1.2, steps), event->position());
    event->accept();
}

bool GraphicsView::event(QEvent* event)
{
    if (event->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomBy(1 + g->value(), mapFromGlobal(g->globalPosition().toPoint()));
            return true;
        }
    }
    return QWidget::event(event);
}

void GraphicsView::mousePressEvent(QMouseEvent* event)
{
    a_pressPos = a_lastPos = event->position().toPoint();
    a_pressed = event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton;
    a_dragging = event->button() == Qt::MiddleButton;
    setFocus();
}

void GraphicsView::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    if (a_pressed && (event->buttons() & (Qt::LeftButton | Qt::MiddleButton))) {
        if (!a_dragging && (pos - a_pressPos).manhattanLength() > 4) a_dragging = true;
        if (a_dragging) {
            const QPoint d = pos - a_lastPos;
            setView(a_center - QPointF(d.x() / a_scale, -d.y() / a_scale), a_scale);
            setCursor(Qt::ClosedHandCursor);
        }
        a_lastPos = pos;
        return;
    }
    if (a_pickLevel != Level::None) {
        const int h = entityAt(a_pickLevel, pos);
        if (h != a_hovered) {
            a_hovered = h;
            update();
        }
    }
    emit pointerMoved(toModel(pos), true);
}

void GraphicsView::mouseReleaseEvent(QMouseEvent* event)
{
    const bool click = a_pressed && !a_dragging && event->button() == Qt::LeftButton;
    a_pressed = false;
    if (a_dragging) {
        a_dragging = false;
        setCursor(a_pickLevel == Level::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
    }
    if (click && a_pickLevel != Level::None) emit picked(a_pickLevel, entityAt(a_pickLevel, event->position()));
}

void GraphicsView::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_F:
    case Qt::Key_Home: fit(); return;
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomBy(1.25); return;
    case Qt::Key_Minus: zoomBy(1 / 1.25); return;
    case Qt::Key_Left: setView(a_center - QPointF(width() / 8.0 / a_scale, 0), a_scale); return;
    case Qt::Key_Right: setView(a_center + QPointF(width() / 8.0 / a_scale, 0), a_scale); return;
    case Qt::Key_Up: setView(a_center + QPointF(0, height() / 8.0 / a_scale), a_scale); return;
    case Qt::Key_Down: setView(a_center - QPointF(0, height() / 8.0 / a_scale), a_scale); return;
    default: QWidget::keyPressEvent(event);
    }
}

void GraphicsView::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    if (a_hovered >= 0) {
        a_hovered = -1;
        update();
    }
    emit pointerMoved({}, false);
}

QImage GraphicsView::picture()
{
    QImage image(size() * devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatioF());
    image.fill(Qt::transparent);
    render(&image);
    return image;
}

// ------------------------------------------------------------------ Painting

void GraphicsView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const Colors c = colors();
    p.fillRect(rect(), c.background);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (!a_topology && !a_mesh) {
        p.setPen(c.ink);
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Build the geometry to see it here: its features in the Multiphysics panel, then Build All."));
        return;
    }
    paintGrid(p, c);
    switch (a_show) {
    case Show::Geometry: paintGeometry(p, c, false); break;
    case Show::Mesh:
        paintGeometry(p, c, true);
        paintMesh(p, c, false);
        break;
    case Show::Results: paintResults(p, c); break;
    }
    // A word on what is shown, top left.
    QString title;
    if (a_show == Show::Geometry && a_topology)
        title = tr("%1 domains, %2 boundaries, %3 points")
                    .arg(a_topology->domains.size())
                    .arg(a_topology->boundaries.size())
                    .arg(a_topology->points.size());
    else if (a_show == Show::Mesh && a_mesh)
        title = tr("%1 triangles, %2 nodes; smallest angle %3°, mean quality %4")
                    .arg(QLocale().toString(qlonglong(a_mesh->triangles.size())))
                    .arg(QLocale().toString(qlonglong(a_mesh->nodes.size())))
                    .arg(a_mesh->minAngle, 0, 'f', 1)
                    .arg(a_mesh->meanQuality, 0, 'f', 3);
    else if (a_show == Show::Results && a_plot)
        title = a_plot->title;
    if (!title.isEmpty()) {
        p.setPen(c.ink);
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(10, 6, width() - 20, 20), Qt::AlignLeft | Qt::AlignVCenter, title);
        p.setFont(font());
    }
    if (a_show == Show::Results && a_plot && !a_plot->problems.isEmpty()) {
        p.setPen(QColor(200, 40, 40));
        p.drawText(QRectF(10, 26, width() - 20, 40), Qt::AlignLeft | Qt::TextWordWrap, a_plot->problems.join(QLatin1Char('\n')));
    }
}

void GraphicsView::setAxisymmetric(bool on)
{
    if (a_axisymmetric == on) return;
    a_axisymmetric = on;
    update();
}

void GraphicsView::paintGrid(QPainter& p, const Colors& c) const
{
    const double step = niceStep(70 / a_scale);
    const QPointF lo = toModel(QPointF(0, height())), hi = toModel(QPointF(width(), 0));
    p.save();
    p.setPen(QPen(c.faint, 1));
    p.setRenderHint(QPainter::Antialiasing, false);
    const double x0 = std::ceil(lo.x() / step) * step, y0 = std::ceil(lo.y() / step) * step;
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.82));
    p.setFont(small);
    const QColor label = QColor(c.ink.red(), c.ink.green(), c.ink.blue(), 140);
    int guard = 0;
    for (double x = x0; x <= hi.x() && guard < 400; x += step, ++guard) {
        const double px = toPixel(QPointF(x, 0)).x();
        p.setPen(QPen(c.faint, 1));
        p.drawLine(QPointF(px, 0), QPointF(px, height()));
        p.setPen(label);
        p.drawText(QRectF(px + 3, height() - 16, 80, 14), Qt::AlignLeft | Qt::AlignVCenter, formatNumber(std::abs(x) < step * 1e-9 ? 0 : x, 6));
    }
    guard = 0;
    for (double y = y0; y <= hi.y() && guard < 400; y += step, ++guard) {
        const double py = toPixel(QPointF(0, y)).y();
        p.setPen(QPen(c.faint, 1));
        p.drawLine(QPointF(0, py), QPointF(width(), py));
        p.setPen(label);
        p.drawText(QRectF(3, py - 15, 80, 14), Qt::AlignLeft | Qt::AlignVCenter, formatNumber(std::abs(y) < step * 1e-9 ? 0 : y, 6));
    }
    if (a_axisymmetric) {
        // The axis, as COMSOL draws it.
        const double px = toPixel(QPointF(0, 0)).x();
        QPen axis(QColor(c.ink.red(), c.ink.green(), c.ink.blue(), 170), 1.2, Qt::DashDotLine);
        p.setPen(axis);
        p.drawLine(QPointF(px, 0), QPointF(px, height()));
    }
    if (a_topology && !a_topology->unitName.isEmpty()) {
        // The unit, in the lower left corner, over the labels there.
        QString unit = a_topology->unitName == QLatin1String("um") ? QStringLiteral("µm") : a_topology->unitName;
        if (a_axisymmetric) unit = tr("r, z in %1").arg(unit);
        const QRectF box(0, height() - 16, p.fontMetrics().horizontalAdvance(unit) + 10, 16);
        p.fillRect(box, c.background);
        p.setPen(label);
        p.drawText(box.adjusted(3, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, unit);
    }
    p.restore();
}

void GraphicsView::paintGeometry(QPainter& p, const Colors& c, bool faded)
{
    if (!a_topology) return;
    const Topology& t = *a_topology;
    const bool dark = qucs_s::ink::isDark(c.background);
    p.save();
    p.setTransform(modelToPixel());
    const bool domainsChosen = a_highlightLevel == Level::Domain;
    for (int d = 0; d < int(a_domainPaths.size()); ++d) {
        QColor fill = domainColor(d, dark);
        if (faded) fill.setAlpha(110);
        if (domainsChosen && a_highlight.contains(d)) fill = c.highlight.lighter(dark ? 100 : 160);
        if (a_pickLevel == Level::Domain && a_hovered == d) fill = c.hover.lighter(dark ? 110 : 150);
        p.fillPath(a_domainPaths[std::size_t(d)], fill);
    }
    for (int b = 0; b < int(a_boundaryPaths.size()); ++b) {
        const bool chosen = a_highlightLevel == Level::Boundary && a_highlight.contains(b);
        const bool hover = a_pickLevel == Level::Boundary && a_hovered == b;
        QPen pen(hover ? c.hover : chosen ? c.highlight : c.boundary, hover || chosen ? 3.0 : 1.2);
        pen.setCosmetic(true);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawPath(a_boundaryPaths[std::size_t(b)]);
    }
    p.restore();
    // Points, in pixels.
    for (int i = 0; i < int(t.points.size()); ++i) {
        const bool chosen = a_highlightLevel == Level::Point && a_highlight.contains(i);
        const bool hover = a_pickLevel == Level::Point && a_hovered == i;
        const QPointF at = toPixel(t.vertices[std::size_t(t.points[std::size_t(i)].vertex)]);
        p.setPen(Qt::NoPen);
        p.setBrush(hover ? c.hover : chosen ? c.highlight : c.boundary);
        const double r = hover || chosen ? 4.5 : 2.2;
        p.drawEllipse(at, r, r);
    }
    p.setBrush(Qt::NoBrush);
}

void GraphicsView::paintMesh(QPainter& p, const Colors& c, bool quality)
{
    if (!a_mesh) return;
    if (quality) {
        // Each triangle in the color of its quality: red poor, green good.
        p.save();
        p.setTransform(modelToPixel());
        p.setPen(Qt::NoPen);
        for (int t = 0; t < int(a_mesh->triangles.size()); ++t) {
            const double q = a_mesh->quality(t);
            const auto& v = a_mesh->triangles[std::size_t(t)];
            QPolygonF poly;
            poly << a_mesh->nodes[std::size_t(v[0])] << a_mesh->nodes[std::size_t(v[1])] << a_mesh->nodes[std::size_t(v[2])];
            p.setBrush(QColor::fromHsvF(std::clamp((q - 0.3) / 0.7, 0.0, 1.0) * 0.33, 0.55, 0.95));
            p.drawPolygon(poly);
        }
        p.restore();
    }
    p.save();
    p.setTransform(modelToPixel());
    QColor ink = c.ink;
    ink.setAlpha(a_show == Show::Results ? 70 : 150);
    QPen pen(ink, 0.6);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.drawPath(a_meshPath);
    p.restore();
}

void GraphicsView::rasterize()
{
    const double dpr = devicePixelRatioF();
    const QSize px(std::max(1, int(width() * dpr)), std::max(1, int(height() * dpr)));
    a_raster = QImage(px, QImage::Format_ARGB32_Premultiplied);
    a_raster.fill(Qt::transparent);
    a_rasterDirty = false;
    ++a_rasters;
    if (!a_plot || !a_plot->surface) return;
    const SurfaceData& s = a_plot->values;
    const std::vector<QRgb>& table = colorTable(a_plot->colors);
    const double lo = a_plot->min, hi = a_plot->max;
    const double span = hi > lo ? hi - lo : 1;
    const double sx = a_scale * dpr, ox = width() / 2.0 * dpr - a_center.x() * sx;
    const double sy = -a_scale * dpr, oy = height() / 2.0 * dpr - a_center.y() * sy;
    const int W = px.width(), H = px.height();
    const std::size_t n = s.points.size() / 3;
    for (std::size_t t = 0; t < n; ++t) {
        const double v0 = s.values[t * 3], v1 = s.values[t * 3 + 1], v2 = s.values[t * 3 + 2];
        if (!std::isfinite(v0) || !std::isfinite(v1) || !std::isfinite(v2)) continue;
        const QPointF& a = s.points[t * 3];
        const QPointF& b = s.points[t * 3 + 1];
        const QPointF& c = s.points[t * 3 + 2];
        const double x0 = a.x() * sx + ox, y0 = a.y() * sy + oy;
        const double x1 = b.x() * sx + ox, y1 = b.y() * sy + oy;
        const double x2 = c.x() * sx + ox, y2 = c.y() * sy + oy;
        const int minX = std::max(0, int(std::floor(std::min({x0, x1, x2}))));
        const int maxX = std::min(W - 1, int(std::ceil(std::max({x0, x1, x2}))));
        const int minY = std::max(0, int(std::floor(std::min({y0, y1, y2}))));
        const int maxY = std::min(H - 1, int(std::ceil(std::max({y0, y1, y2}))));
        if (minX > maxX || minY > maxY) continue;
        const double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::abs(det) < 1e-12) continue;
        const double inv = 1 / det;
        const double eps = 1e-7;
        for (int y = minY; y <= maxY; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(a_raster.scanLine(y));
            const double py = y + 0.5;
            for (int x = minX; x <= maxX; ++x) {
                const double pxl = x + 0.5;
                const double l1 = ((pxl - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) * inv;
                const double l2 = ((x1 - x0) * (py - y0) - (pxl - x0) * (y1 - y0)) * inv;
                const double l0 = 1 - l1 - l2;
                if (l0 < -eps || l1 < -eps || l2 < -eps) continue;
                const double v = l0 * v0 + l1 * v1 + l2 * v2;
                const int i = std::clamp(int((v - lo) / span * 255.0 + 0.5), 0, 255);
                line[x] = table[std::size_t(i)];
            }
        }
    }
}

void GraphicsView::paintResults(QPainter& p, const Colors& c)
{
    if (!a_plot) {
        paintGeometry(p, c, true);
        return;
    }
    if (a_plot->surface) {
        if (a_rasterDirty || a_raster.size() != QSize(int(width() * devicePixelRatioF()), int(height() * devicePixelRatioF())))
            rasterize();
        QImage shown = a_raster;
        shown.setDevicePixelRatio(devicePixelRatioF());
        p.drawImage(QPointF(0, 0), shown);
    } else {
        paintGeometry(p, c, true);
    }
    if (a_plot->mesh) paintMesh(p, c, a_plot->quality);
    // The boundaries, thin, over the colors.
    p.save();
    p.setTransform(modelToPixel());
    QColor edge = c.boundary;
    edge.setAlpha(a_plot->surface ? 150 : 255);
    for (int b = 0; b < int(a_boundaryPaths.size()); ++b) {
        const bool chosen = a_highlightLevel == Level::Boundary && a_highlight.contains(b);
        QPen pen(chosen ? c.highlight : edge, chosen ? 2.5 : 0.8);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawPath(a_boundaryPaths[std::size_t(b)]);
    }
    for (std::size_t k = 0; k < a_contourPaths.size(); ++k) {
        QPen pen(a_contourColors[k], 1.3);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawPath(a_contourPaths[k]);
    }
    p.restore();
    for (const PlotScene::Arrows& a : a_plot->arrows) paintArrows(p, a);
    if (a_plot->surface) paintLegend(p, c, a_plot->surfaceLabel, a_plot->colors, a_plot->min, a_plot->max);
    else
        for (const PlotScene::Contours& k : a_plot->contours)
            if (k.colors != QLatin1String("single")) {
                paintLegend(p, c, QString(), k.colors, k.min, k.max);
                break;
            }
}

void GraphicsView::paintArrows(QPainter& p, const PlotScene::Arrows& arrows)
{
    if (arrows.data.at.empty() || !(arrows.data.longest > 0)) return;
    const double cell = arrows.spacing * a_scale;
    const double longest = std::max(4.0, 0.9 * cell);
    p.save();
    QPen pen(arrows.color, 1.2);
    p.setPen(pen);
    for (std::size_t i = 0; i < arrows.data.at.size(); ++i) {
        const QPointF v = arrows.data.vector[i];
        const double mag = std::hypot(v.x(), v.y());
        if (!(mag > 0)) continue;
        const double len = arrows.normalized ? 0.75 * longest : longest * mag / arrows.data.longest;
        if (len < 1.5) continue;
        const QPointF dir(v.x() / mag, -v.y() / mag);   // y up
        const QPointF from = toPixel(arrows.data.at[i]) - dir * len / 2;
        const QPointF to = from + dir * len;
        p.drawLine(from, to);
        const double head = std::min(7.0, len * 0.35);
        const QPointF back = -dir * head;
        const double cs = std::cos(0.45), sn = std::sin(0.45);
        p.drawLine(to, to + QPointF(back.x() * cs - back.y() * sn, back.x() * sn + back.y() * cs));
        p.drawLine(to, to + QPointF(back.x() * cs + back.y() * sn, -back.x() * sn + back.y() * cs));
    }
    p.restore();
}

void GraphicsView::paintLegend(QPainter& p, const Colors& c, const QString& label, const QString& colors, double lo, double hi)
{
    const std::vector<QRgb>& table = colorTable(colors);
    const int barW = 16;
    const int top = 40, bottom = height() - 40;
    const int left = width() - 86;
    if (bottom - top < 40) return;
    const int h = bottom - top;
    QImage bar(1, 256, QImage::Format_RGB32);
    for (int i = 0; i < 256; ++i) bar.setPixel(0, 255 - i, table[std::size_t(i)]);
    p.drawImage(QRect(left, top, barW, h), bar);
    p.setPen(QPen(c.ink, 1));
    p.drawRect(QRect(left, top, barW, h));
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.85));
    p.setFont(small);
    const int ticks = 6;
    for (int k = 0; k < ticks; ++k) {
        const double v = lo + (hi - lo) * k / (ticks - 1);
        const int y = bottom - int(std::lround(double(h) * k / (ticks - 1)));
        p.drawLine(left + barW, y, left + barW + 4, y);
        p.drawText(QRect(left + barW + 6, y - 8, 64, 16), Qt::AlignLeft | Qt::AlignVCenter, formatNumber(v, 4));
    }
    if (!label.isEmpty()) p.drawText(QRect(left - 60, top - 24, 140, 18), Qt::AlignRight | Qt::AlignVCenter, label);
    p.setFont(font());
}

} // namespace qucs_s::fem
