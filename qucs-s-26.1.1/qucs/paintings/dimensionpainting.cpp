/*
 * dimensionpainting.cpp - a dimension between two points
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "dimensionpainting.h"

#include "geometry/geometry.h"
#include "ink.h"
#include "main.h"
#include "misc.h"
#include "schematic.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <cmath>

using qucs_s::paintings::decodeText;
using qucs_s::paintings::encodeText;
using qucs_s::paintings::textFont;

namespace {

QString tr(const char* text)
{
  return QCoreApplication::translate("ShapePainting", text);
}

QPointF unit(const QPointF& v)
{
  const qreal length = std::hypot(v.x(), v.y());
  return length > 0 ? v / length : QPointF(1, 0);
}

// A scale as the dialog allows it (1e-6 to 1e6); "nan", "inf", 0 or less
// (from a file, or given to a tool) is 1 - its label read "nan mm".
double scaleOf(double scale)
{
  return std::isfinite(scale) && scale > 0 ? std::clamp(scale, 0.000001, 1e6) : 1.0;
}

} // namespace

DimensionPainting::DimensionPainting() : m_pen(QColor(0, 0, 0)), m_fontSize(QucsSettings.font.pointSize())
{
  Name = QStringLiteral("Dimension ");
  isSelected = false;
  if (m_fontSize <= 0) m_fontSize = 10;
  updateBounds();
}

Element* DimensionPainting::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Dimension");
  bitmap = (char*)"dimension";
  return getNewOne ? new DimensionPainting() : nullptr;
}

void DimensionPainting::setPoints(const QPoint& start, const QPoint& end, int offset)
{
  m_p1 = start;
  m_p2 = end;
  m_offset = offset;
  updateBounds();
}

QString DimensionPainting::label() const
{
  if (!m_text.isEmpty()) return m_text;
  const double length = std::hypot(double(m_p2.x() - m_p1.x()), double(m_p2.y() - m_p1.y())) * m_scale;
  const QString number = QString::number(length, 'f', m_decimals);
  return m_unit.isEmpty() ? number : number + QLatin1Char(' ') + m_unit;
}

DimensionPainting::Parts DimensionPainting::parts() const
{
  Parts p;
  const QPointF p1(m_p1), p2(m_p2);
  const QPointF u = unit(p2 - p1);
  const QPointF n(-u.y(), u.x());
  const QPointF a = p1 + n * m_offset, b = p2 + n * m_offset;
  p.line = QLineF(a, b);
  // Extension lines, from a little off the points to a little past the line.
  const qreal side = m_offset < 0 ? -1 : 1, gap = 3, past = 5;
  if (std::abs(m_offset) > gap) {
    p.extensions << QLineF(p1 + n * side * gap, a + n * side * past) << QLineF(p2 + n * side * gap, b + n * side * past);
  }
  const qreal w = m_pen.width();
  switch (m_ends) {
  case Ends::Arrows: {
    const qreal length = 8 + w, half = 3 + w / 2;
    p.heads << QPolygonF({a, a + u * length + n * half, a + u * length - n * half})
            << QPolygonF({b, b - u * length + n * half, b - u * length - n * half});
    break;
  }
  case Ends::Ticks: {
    const QPointF slant = (u - n) * (4 + w);
    p.ticks << QLineF(a - slant, a + slant) << QLineF(b - slant, b + slant);
    break;
  }
  case Ends::Dots: {
    const qreal r = 2.5 + w / 2;
    for (const QPointF& at : {a, b}) {
      QPolygonF dot;
      for (int k = 0; k < 12; ++k) dot << at + QPointF(r * std::cos(k * M_PI / 6), r * std::sin(k * M_PI / 6));
      p.heads << dot;
    }
    break;
  }
  }
  // The label: in the middle, above the line, turned to read.
  qreal degrees = qRadiansToDegrees(std::atan2(u.y(), u.x()));
  if (degrees > 90.0001 || degrees <= -90) degrees += 180;
  p.text.translate(p.line.center().x(), p.line.center().y());
  p.text.rotate(degrees);
  const QFontMetricsF metrics(textFont(m_fontSize));
  const QString shown = label();
  const qreal width = metrics.horizontalAdvance(shown), height = metrics.height();
  p.textRect = QRectF(-width / 2, -height - 2, width, height);
  return p;
}

void DimensionPainting::updateBounds() noexcept
{
  const Parts p = parts();
  QRectF r = QRectF(p.line.p1(), p.line.p2()).normalized();
  r = r.united(QRectF(QPointF(m_p1), QPointF(m_p2)).normalized());
  for (const QPolygonF& h : p.heads) r = r.united(h.boundingRect());
  r = r.united(p.text.mapRect(p.textRect));
  const QRect b = r.toAlignedRect();
  x1 = b.left();
  y1 = b.top();
  x2 = b.right();
  y2 = b.bottom();
  updateCenter();
}

QRect DimensionPainting::boundingRect() const noexcept
{
  return QRect(QPoint(x1, y1), QPoint(x2, y2));
}

void DimensionPainting::paint(QPainter* painter)
{
  const Parts p = parts();
  painter->save();
  const QPen pen = qucs_s::ink::on(m_pen);
  const auto draw = [&](const QPen& with, bool filled) {
    painter->setPen(with);
    painter->drawLine(p.line);
    for (const QLineF& l : p.extensions) painter->drawLine(l);
    for (const QLineF& l : p.ticks) painter->drawLine(l);
    painter->setBrush(filled ? QBrush(with.color()) : QBrush(Qt::NoBrush));
    for (const QPolygonF& h : p.heads) painter->drawPolygon(h);
  };
  if (isSelected) {
    draw(qucs_s::ink::on(QPen(Qt::darkGray, m_pen.width() + 5)), false);
    draw(qucs_s::ink::on(QPen(Qt::white, m_pen.width())), true);
  } else {
    draw(pen, true);
  }
  painter->setTransform(p.text, true);
  painter->setFont(textFont(m_fontSize));
  painter->setPen(pen);
  painter->drawText(p.textRect, Qt::AlignCenter, label());
  painter->restore();
  if (isSelected) {
    misc::draw_resize_handle(painter, m_p1);
    misc::draw_resize_handle(painter, m_p2);
    misc::draw_resize_handle(painter, p.line.center());
  }
}

void DimensionPainting::paintScheme(Schematic* sch)
{
  const Parts p = parts();
  const auto line = [sch](const QLineF& l) {
    sch->PostPaintEvent(_Line, qRound(l.x1()), qRound(l.y1()), qRound(l.x2()), qRound(l.y2()));
  };
  line(p.line);
  for (const QLineF& l : p.extensions) line(l);
}

// "Dimension x1 y1 x2 y2 offset colour width style ends size scale decimals ~unit ~text"
bool DimensionPainting::load(const QString& s)
{
  const QStringList f = s.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (f.size() < 15) return false;
  bool ok = true;
  const auto number = [&](int i) {
    bool good = false;
    const int n = f.at(i).toInt(&good);
    ok = ok && good;
    return n;
  };
  m_p1 = QPoint(misc::clampCoordinate(number(1)), misc::clampCoordinate(number(2)));
  m_p2 = QPoint(misc::clampCoordinate(number(3)), misc::clampCoordinate(number(4)));
  m_offset = misc::clampCoordinate(number(5));
  const QColor colour = misc::ColorFromString(f.at(6));
  if (!colour.isValid()) return false;
  m_pen = QPen(colour, std::clamp(number(7), 0, 100), Qt::PenStyle(std::clamp(number(8), 1, 5)));
  m_ends = Ends(std::clamp(number(9), 0, 2));
  m_fontSize = std::clamp(number(10), 1, 400);
  bool good = false;
  m_scale = scaleOf(f.at(11).toDouble(&good));
  ok = ok && good;
  m_decimals = std::clamp(number(12), 0, 6);
  if (!f.at(13).startsWith(QLatin1Char('~')) || !f.at(14).startsWith(QLatin1Char('~'))) return false;
  m_unit = decodeText(f.at(13).mid(1));
  m_text = decodeText(f.at(14).mid(1));
  m_placing = 0;
  updateBounds();
  return ok;
}

QString DimensionPainting::save()
{
  return QStringList{Name.trimmed(),
                     QString::number(m_p1.x()),
                     QString::number(m_p1.y()),
                     QString::number(m_p2.x()),
                     QString::number(m_p2.y()),
                     QString::number(m_offset),
                     m_pen.color().name(),
                     QString::number(m_pen.width()),
                     QString::number(int(m_pen.style())),
                     QString::number(int(m_ends)),
                     QString::number(m_fontSize),
                     QString::number(m_scale, 'g', 10),
                     QString::number(m_decimals),
                     QLatin1Char('~') + encodeText(m_unit),
                     QLatin1Char('~') + encodeText(m_text)}
      .join(QLatin1Char(' '));
}

QString DimensionPainting::saveCpp()
{
  const Parts p = parts();
  QStringList lines;
  const auto line = [&](const QLineF& l) {
    lines << QStringLiteral("Lines.append (new qucs::Line (%1, %2, %3, %4, QPen (QColor (\"%5\"), %6)));")
                 .arg(l.x1())
                 .arg(l.y1())
                 .arg(l.x2())
                 .arg(l.y2())
                 .arg(m_pen.color().name())
                 .arg(m_pen.width());
  };
  line(p.line);
  for (const QLineF& l : p.extensions) line(l);
  for (const QLineF& l : p.ticks) line(l);
  return lines.join(QLatin1Char('\n'));
}

QString DimensionPainting::saveJSON()
{
  const Parts p = parts();
  QStringList items;
  const auto line = [&](const QLineF& l) {
    items << QStringLiteral("{\"type\" : \"line\", \"x1\" : %1, \"y1\" : %2, \"x2\" : %3, \"y2\" : %4, "
                            "\"color\" : \"%5\", \"thick\" : %6, \"style\" : \"Qt::SolidLine\"},")
                 .arg(l.x1())
                 .arg(l.y1())
                 .arg(l.x2())
                 .arg(l.y2())
                 .arg(m_pen.color().name())
                 .arg(m_pen.width());
  };
  line(p.line);
  for (const QLineF& l : p.extensions) line(l);
  for (const QLineF& l : p.ticks) line(l);
  return items.join(QLatin1Char(' '));
}

bool DimensionPainting::getSelected(const QPoint& click, int tolerance)
{
  using qucs_s::geom::is_near_line;
  const Parts p = parts();
  const auto near = [&](const QLineF& l) {
    return is_near_line(click, l.p1().toPoint(), l.p2().toPoint(), tolerance + m_pen.width());
  };
  if (near(p.line)) return true;
  for (const QLineF& l : p.extensions)
    if (near(l)) return true;
  return p.text.map(QPolygonF(p.textRect.adjusted(-tolerance, -tolerance, tolerance, tolerance)))
      .containsPoint(QPointF(click), Qt::OddEvenFill);
}

bool DimensionPainting::resizeTouched(const QPoint& click, int tolerance)
{
  using qucs_s::geom::distance;
  const QPointF middle = parts().line.center();
  if (distance(m_p1, click) < tolerance) m_dragged = 0;
  else if (distance(m_p2, click) < tolerance) m_dragged = 1;
  else if (distance(middle.toPoint(), click) < tolerance) m_dragged = 2;
  else m_dragged = -1;
  return m_dragged >= 0;
}

void DimensionPainting::MouseResizeMoving(int x, int y, Schematic* sch)
{
  switch (m_dragged) {
  case 0: m_p1 = QPoint(x, y); break;
  case 1: m_p2 = QPoint(x, y); break;
  case 2: {
    // Off the points by as much as the handle is across the line.
    const QPointF u = unit(QPointF(m_p2 - m_p1));
    const QPointF n(-u.y(), u.x());
    const QPointF d = QPointF(x, y) - QPointF(m_p1);
    m_offset = qRound(d.x() * n.x() + d.y() * n.y());
    break;
  }
  default: return;
  }
  updateBounds();
  paintScheme(sch);
}

void DimensionPainting::MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& cursor)
{
  if (m_placing == 0) {
    m_p1 = m_p2 = onGrid;
  } else {
    m_p2 = onGrid;
    updateBounds();
    paintScheme(sch);
  }
  // The cursor: a double arrow.
  sch->PostPaintEvent(_Line, cursor.x() + 13, cursor.y() + 6, cursor.x() + 33, cursor.y() + 6, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 13, cursor.y() + 6, cursor.x() + 17, cursor.y() + 3, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 13, cursor.y() + 6, cursor.x() + 17, cursor.y() + 9, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 33, cursor.y() + 6, cursor.x() + 29, cursor.y() + 3, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 33, cursor.y() + 6, cursor.x() + 29, cursor.y() + 9, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 13, cursor.y() + 1, cursor.x() + 13, cursor.y() + 11, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 33, cursor.y() + 1, cursor.x() + 33, cursor.y() + 11, 0, 0, true);
}

bool DimensionPainting::MousePressing(Schematic*)
{
  if (m_placing == 0) {
    m_placing = 1;
    return false;
  }
  m_placing = 0;
  if (m_p1 == m_p2) m_p2 = m_p1 + QPoint(60, 0);   // a click and nothing more
  updateBounds();
  return true;
}

void DimensionPainting::snapToGrid(Schematic* sch)
{
  int x = m_p1.x(), y = m_p1.y();
  sch->setOnGrid(x, y);
  m_p1 = QPoint(x, y);
  x = m_p2.x();
  y = m_p2.y();
  sch->setOnGrid(x, y);
  m_p2 = QPoint(x, y);
  updateBounds();
}

void DimensionPainting::afterMove(int dx, int dy) noexcept
{
  m_p1 += QPoint(dx, dy);
  m_p2 += QPoint(dx, dy);
}

bool DimensionPainting::rotate() noexcept
{
  return rotate(cx, cy);
}

bool DimensionPainting::rotate(int xc, int yc) noexcept
{
  for (QPoint* p : {&m_p1, &m_p2}) {
    int x = p->x(), y = p->y();
    qucs_s::geom::rotate_point_ccw(x, y, xc, yc);
    *p = QPoint(x, y);
  }
  updateBounds();
  return true;
}

// Mirrored, the line stays on the side of the points it was on: across a
// mirrored direction that is the other side.
bool DimensionPainting::mirrorX() noexcept
{
  const int axis = cy;
  m_p1.setY(qucs_s::geom::mirror_coordinate(m_p1.y(), axis));
  m_p2.setY(qucs_s::geom::mirror_coordinate(m_p2.y(), axis));
  m_offset = -m_offset;
  updateBounds();
  return true;
}

bool DimensionPainting::mirrorY() noexcept
{
  const int axis = cx;
  m_p1.setX(qucs_s::geom::mirror_coordinate(m_p1.x(), axis));
  m_p2.setX(qucs_s::geom::mirror_coordinate(m_p2.x(), axis));
  m_offset = -m_offset;
  updateBounds();
  return true;
}

bool DimensionPainting::symbolPrimitives(SymbolPrimitives& into) const
{
  const Parts p = parts();
  const auto line = [&](const QLineF& l) { into.lines << new qucs::Line(l.x1(), l.y1(), l.x2(), l.y2(), m_pen); };
  line(p.line);
  for (const QLineF& l : p.extensions) line(l);
  for (const QLineF& l : p.ticks) line(l);
  for (const QPolygonF& h : p.heads) {
    auto* head = new qucs::Polyline(std::vector<QPointF>(h.cbegin(), h.cend()), m_pen, QBrush(m_pen.color()));
    head->closed = true;
    into.polylines << head;
  }
  // The label, from its top left, turned as the line is.
  const QPointF at = p.text.map(p.textRect.topLeft());
  const qreal degrees = -qRadiansToDegrees(std::atan2(p.text.m12(), p.text.m11()));
  into.texts << new Text(at.x(), at.y(), label(), m_pen.color(), textFont(m_fontSize).pixelSize(),
                         std::cos(qDegreesToRadians(degrees)), std::sin(qDegreesToRadians(degrees)));
  return true;
}

QList<PaintingField> DimensionPainting::fields() const
{
  QList<PaintingField> f;
  const QString label = tr("Label");
  f << PaintingField{QStringLiteral("text"), tr("Text:"), PaintingField::Text, m_text, 0, 0, 0, {}, label,
                     tr("Empty: the length, scaled, with the unit")};
  f << PaintingField{QStringLiteral("scale"), tr("Scale:"), PaintingField::Real, m_scale, 0.000001, 1e6, 6, {}, label,
                     tr("What one unit of the schematic stands for")};
  f << PaintingField{QStringLiteral("unit"), tr("Unit:"), PaintingField::Text, m_unit, 0, 0, 0, {}, label};
  f << PaintingField{QStringLiteral("decimals"), tr("Decimals:"), PaintingField::Int, m_decimals, 0, 6, 0, {}, label};
  f << PaintingField{QStringLiteral("fontSize"), tr("Size:"), PaintingField::Int, m_fontSize, 4, 96, 0, {}, label};
  const QString line = tr("Line");
  f << PaintingField{QStringLiteral("ends"), tr("Ends:"), PaintingField::Choice, int(m_ends), 0, 0, 0,
                     {tr("arrows"), tr("ticks"), tr("dots")}, line};
  f << PaintingField{QStringLiteral("lineColour"), tr("Colour:"), PaintingField::Colour, QVariant::fromValue(m_pen.color()), 0, 0, 0, {}, line};
  f << PaintingField{QStringLiteral("lineWidth"), tr("Width:"), PaintingField::Int, m_pen.width(), 0, 20, 0, {}, line};
  f << PaintingField{QStringLiteral("offset"), tr("Off the points by:"), PaintingField::Int, m_offset, -2000, 2000, 0, {}, line,
                     tr("How far the dimension line is from the points it measures (negative: to the left)")};
  return f;
}

void DimensionPainting::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("text")) m_text = value.toString();
  else if (key == QLatin1String("scale")) m_scale = scaleOf(value.toDouble());
  else if (key == QLatin1String("unit")) m_unit = value.toString();
  else if (key == QLatin1String("decimals")) m_decimals = std::clamp(value.toInt(), 0, 6);
  else if (key == QLatin1String("fontSize")) m_fontSize = std::clamp(value.toInt(), 1, 400);
  else if (key == QLatin1String("ends")) m_ends = Ends(std::clamp(value.toInt(), 0, 2));
  else if (key == QLatin1String("lineColour")) m_pen.setColor(value.value<QColor>());
  else if (key == QLatin1String("lineWidth")) m_pen.setWidth(std::clamp(value.toInt(), 0, 100));
  else if (key == QLatin1String("offset")) m_offset = value.toInt();
  updateBounds();
}

bool DimensionPainting::Dialog(QWidget* parent)
{
  return qucs_s::paintings::editFields(this, tr("Edit Dimension"), parent);
}
