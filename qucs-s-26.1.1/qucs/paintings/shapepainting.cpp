/*
 * shapepainting.cpp - a painting drawn in a box
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "shapepainting.h"

#include "geometry/geometry.h"
#include "ink.h"
#include "misc.h"
#include "schematic.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPathStroker>

namespace {

QString tr(const char* text)
{
  return QCoreApplication::translate("ShapePainting", text);
}

// How many fields every shape's line has before its own (the name first).
constexpr int kCommonFields = 13;

QString penStyleName(Qt::PenStyle style)
{
  switch (style) {
  case Qt::DashLine: return QStringLiteral("Qt::DashLine");
  case Qt::DotLine: return QStringLiteral("Qt::DotLine");
  case Qt::DashDotLine: return QStringLiteral("Qt::DashDotLine");
  case Qt::DashDotDotLine: return QStringLiteral("Qt::DashDotDotLine");
  case Qt::NoPen: return QStringLiteral("Qt::NoPen");
  default: return QStringLiteral("Qt::SolidLine");
  }
}

} // namespace

ShapePainting::ShapePainting(const QString& name, bool filled)
    : m_pen(QColor(0, 0, 0)), m_brush(QColor(0xc8, 0xc8, 0xc8), Qt::SolidPattern), m_filled(filled)
{
  Name = name.endsWith(QLatin1Char(' ')) ? name : name + QLatin1Char(' ');
  isSelected = false;
  m_pen.setWidth(1);
  cx = cy = x1 = x2 = y1 = y2 = 0;
}

void ShapePainting::setBox(const QRect& box)
{
  x1 = box.left();
  y1 = box.top();
  x2 = box.right();
  y2 = box.bottom();
  updateCenter();
}

QSizeF ShapePainting::frameSize() const
{
  const QRect b = box();
  const qreal w = b.width() - 1, h = b.height() - 1;   // (QRect's right() is one short)
  return m_angle % 180 == 0 ? QSizeF(w, h) : QSizeF(h, w);
}

QTransform ShapePainting::frame(bool withMirror) const
{
  const QRect b = box();
  const QSizeF size = frameSize();
  QTransform t;
  t.translate((b.left() + b.right()) / 2.0, (b.top() + b.bottom()) / 2.0);
  t.rotate(-m_angle);   // (counter-clockwise on the screen, whose y goes down)
  if (withMirror && m_mirrored) t.scale(-1, 1);
  t.translate(-size.width() / 2.0, -size.height() / 2.0);
  return t;
}

QPainterPath ShapePainting::worldPath() const
{
  const QSizeF size = frameSize();
  QPainterPath path = frame().map(outline(size.width(), size.height()));
  const QPainterPath extra = extraWorldPath();
  if (!extra.isEmpty()) path = path.united(extra).simplified();
  return path;
}

QRect ShapePainting::boundingRect() const noexcept
{
  QRect r = box();
  const QPainterPath extra = extraWorldPath();
  if (!extra.isEmpty()) r = r.united(extra.boundingRect().toAlignedRect().adjusted(0, 0, 1, 1));   // (its far edge in)
  return r;
}

void ShapePainting::paint(QPainter* painter)
{
  painter->save();
  const QPainterPath path = worldPath();
  {
    const QSizeF size = frameSize();
    painter->save();
    painter->setTransform(frame(false), true);
    paintUnder(painter, size.width(), size.height());
    painter->restore();
  }
  painter->setPen(qucs_s::ink::on(m_pen));
  painter->setBrush(fillable() && m_filled ? qucs_s::ink::on(m_brush) : QBrush(Qt::NoBrush));
  painter->drawPath(path);
  {
    const QSizeF size = frameSize();
    painter->save();
    painter->setTransform(frame(false), true);
    paintContent(painter, size.width(), size.height());
    painter->restore();
  }
  if (isSelected) {
    painter->setBrush(Qt::NoBrush);
    painter->setPen(qucs_s::ink::on(QPen(Qt::darkGray, m_pen.width() + 5)));
    painter->drawPath(path);
    painter->setPen(qucs_s::ink::on(QPen(Qt::white, m_pen.width(), m_pen.style())));
    painter->drawPath(path);
    for (const QPoint& corner : {QPoint(x1, y1), QPoint(x2, y1), QPoint(x2, y2), QPoint(x1, y2)})
      misc::draw_resize_handle(painter, corner);
    for (const QPoint& handle : extraHandles()) misc::draw_resize_handle(painter, handle);
  }
  painter->restore();
}

void ShapePainting::paintScheme(Schematic* sch)
{
  const QRect b = box();
  sch->PostPaintEvent(_Rect, b.left(), b.top(), b.width() - 1, b.height() - 1);
  // The outline's straight runs, as it is drawn or moved.
  for (const QPolygonF& polygon : worldPath().toSubpathPolygons())
    for (qsizetype i = 1; i < polygon.size() && polygon.size() < 400; ++i)
      sch->PostPaintEvent(_Line, qRound(polygon.at(i - 1).x()), qRound(polygon.at(i - 1).y()), qRound(polygon.at(i).x()),
                          qRound(polygon.at(i).y()));
}

// "Name x y w h colour width style fillcolour fillstyle filled angle mirrored ..."
bool ShapePainting::load(const QString& line)
{
  const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (f.size() < kCommonFields) return false;
  bool ok = true;
  const auto number = [&](int i) {
    bool good = false;
    const int n = f.at(i).toInt(&good);
    ok = ok && good;
    return n;
  };
  const int x = misc::clampCoordinate(number(1)), y = misc::clampCoordinate(number(2));
  const int w = misc::clampCoordinate(number(3)), h = misc::clampCoordinate(number(4));
  const QColor colour = misc::ColorFromString(f.at(5));
  const int width = number(6), style = number(7);
  const QColor fill = misc::ColorFromString(f.at(8));
  const int fillStyle = number(9), filled = number(10), angle = number(11), mirrored = number(12);
  if (!ok || !colour.isValid() || !fill.isValid()) return false;
  x1 = x;
  y1 = y;
  x2 = x + w;
  y2 = y + h;
  updateCenter();
  m_pen = QPen(colour, std::clamp(width, 0, 100), Qt::PenStyle(std::clamp(style, 0, 5)));
  m_brush = QBrush(fill, Qt::BrushStyle(std::clamp(fillStyle, 0, 14)));
  m_filled = filled != 0;
  m_angle = ((angle / 90) % 4 + 4) % 4 * 90;
  m_mirrored = mirrored != 0;
  m_beingDrawn = false;
  return loadExtra(f.mid(kCommonFields));
}

QString ShapePainting::save()
{
  const QRect b = box();
  QStringList f{Name.trimmed(),
                QString::number(b.left()),
                QString::number(b.top()),
                QString::number(b.width() - 1),
                QString::number(b.height() - 1),
                m_pen.color().name(),
                QString::number(m_pen.width()),
                QString::number(int(m_pen.style())),
                m_brush.color().name(),
                QString::number(int(m_brush.style())),
                m_filled ? QStringLiteral("1") : QStringLiteral("0"),
                QString::number(m_angle),
                m_mirrored ? QStringLiteral("1") : QStringLiteral("0")};
  f += saveExtra();
  return f.join(QLatin1Char(' '));
}

QString ShapePainting::saveCpp()
{
  SymbolPrimitives parts;
  symbolPrimitives(parts);
  QStringList lines;
  for (const qucs::Polyline* p : parts.polylines) {
    QStringList points;
    for (const QPointF& point : p->points) points << QStringLiteral("QPointF(%1, %2)").arg(point.x()).arg(point.y());
    lines << QStringLiteral("Polylines.append (new qucs::Polyline ({%1}, QPen (QColor (\"%2\"), %3, %4)));")
                 .arg(points.join(QStringLiteral(", ")), p->pen.color().name())
                 .arg(p->pen.width())
                 .arg(penStyleName(p->pen.style()));
  }
  for (const Text* t : parts.texts)
    lines << QStringLiteral("Texts.append (new Text (%1, %2, \"%3\", QColor (\"%4\"), %5, %6, %7));")
                 .arg(t->x)
                 .arg(t->y)
                 .arg(QString(t->s).replace(QLatin1Char('"'), QStringLiteral("\\\"")), t->Color.name())
                 .arg(t->Size)
                 .arg(t->mCos)
                 .arg(t->mSin);
  qDeleteAll(parts.polylines);
  qDeleteAll(parts.texts);
  qDeleteAll(parts.lines);
  qDeleteAll(parts.images);
  return lines.join(QLatin1Char('\n'));
}

QString ShapePainting::saveJSON()
{
  SymbolPrimitives parts;
  symbolPrimitives(parts);
  QStringList items;
  for (const qucs::Polyline* p : parts.polylines) {
    QStringList points;
    for (const QPointF& point : p->points) points << QStringLiteral("[%1, %2]").arg(point.x()).arg(point.y());
    items << QStringLiteral("{\"type\" : \"polyline\", \"points\" : [%1], \"color\" : \"%2\", \"thick\" : %3, "
                            "\"style\" : \"%4\", \"closed\" : %5},")
                 .arg(points.join(QStringLiteral(", ")), p->pen.color().name())
                 .arg(p->pen.width())
                 .arg(penStyleName(p->pen.style()))
                 .arg(p->closed ? QStringLiteral("true") : QStringLiteral("false"));
  }
  for (const Text* t : parts.texts) {
    QString s = t->s;
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    items << QStringLiteral("{\"type\" : \"graphictext\", \"x\" : %1, \"y\" : %2, \"s\" : \"%3\", \"color\" : \"%4\", "
                            "\"size\" : %5, \"cos\" : %6, \"sin\" : %7},")
                 .arg(t->x)
                 .arg(t->y)
                 .arg(s, t->Color.name())
                 .arg(t->Size)
                 .arg(t->mCos)
                 .arg(t->mSin);
  }
  qDeleteAll(parts.polylines);
  qDeleteAll(parts.texts);
  qDeleteAll(parts.lines);
  qDeleteAll(parts.images);
  return items.join(QLatin1Char(' '));
}

bool ShapePainting::getSelected(const QPoint& click, int tolerance)
{
  const QPainterPath path = worldPath();
  if (picksInside() && (path.contains(QPointF(click)) || box().contains(click))) return true;
  QPainterPathStroker stroker;
  stroker.setWidth(2 * tolerance + m_pen.width());
  return stroker.createStroke(path).contains(QPointF(click));
}

bool ShapePainting::resizeTouched(const QPoint& click, int tolerance)
{
  using qucs_s::geom::distance;
  const QList<QPoint> extras = extraHandles();
  for (int i = 0; i < extras.size(); ++i)
    if (distance(extras.at(i), click) < tolerance) {
      m_dragged = Corner::extra;
      m_draggedExtra = i;
      return true;
    }
  // The box the way round it is drawn.
  const QRect b = box();
  x1 = b.left();
  y1 = b.top();
  x2 = b.right();
  y2 = b.bottom();
  const struct {
    QPoint at;
    Corner corner;
  } corners[] = {{QPoint(x1, y1), Corner::topLeft}, {QPoint(x2, y1), Corner::topRight},
                 {QPoint(x2, y2), Corner::bottomRight}, {QPoint(x1, y2), Corner::bottomLeft}};
  for (const auto& c : corners)
    if (distance(c.at, click) < tolerance) {
      m_dragged = c.corner;
      return true;
    }
  m_dragged = Corner::none;
  return false;
}

void ShapePainting::MouseResizeMoving(int x, int y, Schematic* sch)
{
  switch (m_dragged) {
  case Corner::topLeft: x1 = x; y1 = y; break;
  case Corner::topRight: x2 = x; y1 = y; break;
  case Corner::bottomRight: x2 = x; y2 = y; break;
  case Corner::bottomLeft: x1 = x; y2 = y; break;
  case Corner::extra: moveExtraHandle(m_draggedExtra, QPoint(x, y)); break;
  case Corner::none: return;
  }
  updateCenter();
  paintScheme(sch);
}

void ShapePainting::MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& cursor)
{
  if (m_beingDrawn) {
    x2 = onGrid.x();
    y2 = onGrid.y();
    updateCenter();
    paintScheme(sch);
  } else {
    x1 = x2 = onGrid.x();
    y1 = y2 = onGrid.y();
    updateCenter();
  }
  // The cursor: the shape, small.
  QTransform small;
  small.translate(cursor.x() + 13, cursor.y());
  const QSize size = defaultSize();
  const qreal scale = std::min(20.0 / std::max(1, size.width()), 14.0 / std::max(1, size.height()));
  small.scale(scale, scale);
  for (const QPolygonF& polygon : small.map(outline(size.width(), size.height())).toSubpathPolygons())
    for (qsizetype i = 1; i < polygon.size(); ++i)
      sch->PostPaintEvent(_Line, qRound(polygon.at(i - 1).x()), qRound(polygon.at(i - 1).y()), qRound(polygon.at(i).x()),
                          qRound(polygon.at(i).y()), 0, 0, true);
}

bool ShapePainting::MousePressing(Schematic* sch)
{
  if (!m_beingDrawn) {
    m_beingDrawn = true;
    return false;
  }
  m_beingDrawn = false;
  // A click and nothing more: its own size, from the click.
  const QRect b = box();
  if (b.width() <= 1 || b.height() <= 1) {
    const QSize size = defaultSize();
    x1 = b.left();
    y1 = b.top();
    x2 = x1 + (m_angle % 180 == 0 ? size.width() : size.height());
    y2 = y1 + (m_angle % 180 == 0 ? size.height() : size.width());
  }
  setBox(box());
  placed();
  if (asksWhenPlaced() && sch != nullptr) Dialog(sch);
  return true;
}

void ShapePainting::snapToGrid(Schematic* sch)
{
  sch->setOnGrid(x1, y1);
  sch->setOnGrid(x2, y2);
  mapExtraPoints([sch](const QPoint& p) {
    int x = p.x(), y = p.y();
    sch->setOnGrid(x, y);
    return QPoint(x, y);
  });
  updateCenter();
}

void ShapePainting::afterMove(int dx, int dy) noexcept
{
  mapExtraPoints([dx, dy](const QPoint& p) { return p + QPoint(dx, dy); });
}

bool ShapePainting::rotate() noexcept
{
  return rotate(cx, cy);
}

bool ShapePainting::rotate(int xc, int yc) noexcept
{
  int ax = x1, ay = y1, bx = x2, by = y2;
  qucs_s::geom::rotate_point_ccw(ax, ay, xc, yc);
  qucs_s::geom::rotate_point_ccw(bx, by, xc, yc);
  x1 = std::min(ax, bx);
  x2 = std::max(ax, bx);
  y1 = std::min(ay, by);
  y2 = std::max(ay, by);
  updateCenter();
  m_angle = (m_angle + 90) % 360;
  mapExtraPoints([xc, yc](const QPoint& p) {
    int x = p.x(), y = p.y();
    qucs_s::geom::rotate_point_ccw(x, y, xc, yc);
    return QPoint(x, y);
  });
  return true;
}

// About the box's horizontal axis: in its frame mirrored, turned the other
// way round - flipping y is a half turn after flipping x.
bool ShapePainting::mirrorX() noexcept
{
  m_angle = (540 - m_angle) % 360;
  m_mirrored = !m_mirrored;
  const int axis = cy;
  mapExtraPoints([axis](const QPoint& p) { return QPoint(p.x(), qucs_s::geom::mirror_coordinate(p.y(), axis)); });
  return true;
}

bool ShapePainting::mirrorY() noexcept
{
  m_angle = (360 - m_angle) % 360;
  m_mirrored = !m_mirrored;
  const int axis = cx;
  mapExtraPoints([axis](const QPoint& p) { return QPoint(qucs_s::geom::mirror_coordinate(p.x(), axis), p.y()); });
  return true;
}

bool ShapePainting::symbolPrimitives(SymbolPrimitives& into) const
{
  const QPen pen = m_pen;
  const QBrush brush = fillable() && m_filled ? m_brush : QBrush(Qt::NoBrush);
  const QPainterPath path = worldPath();
  for (const QPolygonF& polygon : path.toSubpathPolygons()) {
    if (polygon.size() < 2) continue;
    std::vector<QPointF> points(polygon.cbegin(), polygon.cend());
    const bool closed = polygon.size() > 2 && polygon.first() == polygon.last();
    if (closed) points.pop_back();
    auto* p = new qucs::Polyline(points, pen, closed ? brush : QBrush(Qt::NoBrush));
    p->closed = closed;
    into.polylines << p;
  }
  const QSizeF size = frameSize();
  contentPrimitives(into, frame(false), size.width(), size.height());
  return true;
}

QList<PaintingField> ShapePainting::fields() const
{
  QList<PaintingField> f;
  const QString line = tr("Line");
  f << PaintingField{QStringLiteral("lineColour"), tr("Colour:"), PaintingField::Colour, QVariant::fromValue(m_pen.color()), 0, 0, 0, {}, line};
  f << PaintingField{QStringLiteral("lineWidth"), tr("Width:"), PaintingField::Int, m_pen.width(), 0, 50, 0, {}, line};
  f << PaintingField{QStringLiteral("lineStyle"), tr("Style:"), PaintingField::PenStyle, int(m_pen.style()), 0, 0, 0, {}, line};
  if (fillable()) {
    const QString fill = tr("Filling");
    f << PaintingField{QStringLiteral("filled"), tr("filled"), PaintingField::Check, m_filled, 0, 0, 0, {}, fill};
    PaintingField colour{QStringLiteral("fillColour"), tr("Colour:"), PaintingField::Colour, QVariant::fromValue(m_brush.color()), 0, 0, 0, {}, fill};
    colour.enabledBy = QStringLiteral("filled");
    PaintingField style{QStringLiteral("fillStyle"), tr("Style:"), PaintingField::BrushStyle, int(m_brush.style()), 0, 0, 0, {}, fill};
    style.enabledBy = QStringLiteral("filled");
    f << colour << style;
  }
  return f;
}

void ShapePainting::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("lineColour")) m_pen.setColor(value.value<QColor>());
  else if (key == QLatin1String("lineWidth")) m_pen.setWidth(std::clamp(value.toInt(), 0, 100));
  else if (key == QLatin1String("lineStyle")) m_pen.setStyle(Qt::PenStyle(std::clamp(value.toInt(), 1, 5)));
  else if (key == QLatin1String("filled")) m_filled = value.toBool();
  else if (key == QLatin1String("fillColour")) m_brush.setColor(value.value<QColor>());
  else if (key == QLatin1String("fillStyle")) m_brush.setStyle(Qt::BrushStyle(std::clamp(value.toInt(), 0, 14)));
}

bool ShapePainting::Dialog(QWidget* parent)
{
  return qucs_s::paintings::editFields(this, dialogTitle(), parent);
}
