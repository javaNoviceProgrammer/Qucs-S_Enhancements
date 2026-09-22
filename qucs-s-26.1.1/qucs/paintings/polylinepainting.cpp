/*
 * polylinepainting.cpp - a run of straight segments, open or closed
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "polylinepainting.h"
#include "filldialog.h"
#include "geometry/geometry.h"
#include "misc.h"
#include "schematic.h"

#include <QObject>
#include <QPainter>
#include <QPolygon>

#include <algorithm>

namespace {
//! A click this near a corner counts as that corner.
constexpr int cornerTolerance = 5;

bool near(const QPoint& a, const QPoint& b, int tolerance = cornerTolerance)
{
  return (a - b).manhattanLength() <= tolerance;
}
} // namespace

PolylinePainting::PolylinePainting(bool filled)
    : m_pen(QColor()), m_brush(Qt::lightGray), m_filled(filled)
{
  Name = "Polyline ";
  isSelected = false;
  if (filled) m_brush.setStyle(Qt::SolidPattern);
  else m_brush.setStyle(Qt::NoBrush);
}

Painting* PolylinePainting::newOne()
{
  return new PolylinePainting(m_filled);
}

Element* PolylinePainting::info(QString& Name, char*& BitmapFile, bool getNewOne)
{
  Name = QObject::tr("Polyline");
  BitmapFile = (char*)"polyline";
  return getNewOne ? new PolylinePainting(false) : nullptr;
}

Element* PolylinePainting::info_filled(QString& Name, char*& BitmapFile, bool getNewOne)
{
  Name = QObject::tr("filled Polygon");
  BitmapFile = (char*)"filledpolyline";
  return getNewOne ? new PolylinePainting(true) : nullptr;
}

QPolygon PolylinePainting::polygon() const
{
  QPolygon points;
  points.reserve(int(m_points.size()));
  for (const QPoint& point : m_points) points << point;
  return points;
}

void PolylinePainting::paint(QPainter* painter)
{
  if (m_points.size() < 2) return;

  painter->save();
  painter->setPen(m_pen);
  painter->setBrush(m_filled ? m_brush : QBrush(Qt::NoBrush));

  if (m_closed || m_filled) painter->drawPolygon(polygon());
  else painter->drawPolyline(polygon());

  if (isSelected) {
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(Qt::darkGray, m_pen.width() + 5));
    if (m_closed || m_filled) painter->drawPolygon(polygon());
    else painter->drawPolyline(polygon());
    painter->setPen(QPen(Qt::white, m_pen.width(), m_pen.style()));
    if (m_closed || m_filled) painter->drawPolygon(polygon());
    else painter->drawPolyline(polygon());

    for (const QPoint& point : m_points) misc::draw_resize_handle(painter, point);
  }

  painter->restore();
}

void PolylinePainting::paintScheme(Schematic* sch)
{
  for (std::size_t i = 1; i < m_points.size(); i++)
    sch->PostPaintEvent(_Line, m_points[i - 1].x(), m_points[i - 1].y(),
                        m_points[i].x(), m_points[i].y());
  if (m_closed && m_points.size() > 2)
    sch->PostPaintEvent(_Line, m_points.back().x(), m_points.back().y(),
                        m_points.front().x(), m_points.front().y());
}

// "Polyline n x1 y1 ... xn yn colour width style fillcolour fillstyle filled closed"
bool PolylinePainting::load(const QString& s)
{
  bool ok = false;
  const int count = s.section(' ', 1, 1).toInt(&ok);
  if (!ok || count < 2 || count > 4096) return false;

  m_points.clear();
  m_points.reserve(count);
  for (int i = 0; i < count; i++) {
    const int x = misc::clampCoordinate(s.section(' ', 2 + 2 * i, 2 + 2 * i).toInt(&ok));
    if (!ok) return false;
    const int y = misc::clampCoordinate(s.section(' ', 3 + 2 * i, 3 + 2 * i).toInt(&ok));
    if (!ok) return false;
    m_points.push_back(QPoint(x, y));
  }

  int field = 2 + 2 * count;
  const QColor colour = misc::ColorFromString(s.section(' ', field, field));
  if (!colour.isValid()) return false;
  m_pen.setColor(colour);

  m_pen.setWidth(s.section(' ', ++field, field).toInt(&ok));
  if (!ok) return false;
  m_pen.setStyle((Qt::PenStyle)s.section(' ', ++field, field).toInt(&ok));
  if (!ok) return false;

  const QColor fill = misc::ColorFromString(s.section(' ', ++field, field));
  if (!fill.isValid()) return false;
  m_brush.setColor(fill);
  m_brush.setStyle((Qt::BrushStyle)s.section(' ', ++field, field).toInt(&ok));
  if (!ok) return false;

  m_filled = s.section(' ', ++field, field).toInt(&ok) != 0;
  if (!ok) return false;
  if (!m_filled) m_brush.setStyle(Qt::NoBrush);

  const QString closed = s.section(' ', ++field, field);
  m_closed = !closed.isEmpty() && closed.toInt() != 0;

  m_beingDrawn = false;
  updateBounds();
  return true;
}

QString PolylinePainting::save()
{
  QString s = Name + QString::number(int(m_points.size()));
  for (const QPoint& point : m_points)
    s += " " + QString::number(point.x()) + " " + QString::number(point.y());

  s += " " + m_pen.color().name() + " " + QString::number(m_pen.width()) + " "
       + QString::number(m_pen.style());
  s += " " + m_brush.color().name() + " " + QString::number(m_brush.style());
  s += m_filled ? " 1" : " 0";
  s += m_closed ? " 1" : " 0";
  return s;
}

QString PolylinePainting::saveCpp()
{
  QString points;
  for (const QPoint& point : m_points) {
    if (!points.isEmpty()) points += ", ";
    points += QStringLiteral("QPointF(%1, %2)").arg(point.x()).arg(point.y());
  }
  return QStringLiteral("Polylines.append (new qucs::Polyline ({%1}, "
                        "QPen (QColor (\"%2\"), %3, %4)));")
      .arg(points, m_pen.color().name())
      .arg(m_pen.width())
      .arg(toPenString(m_pen.style()));
}

QString PolylinePainting::saveJSON()
{
  QString points;
  for (const QPoint& point : m_points) {
    if (!points.isEmpty()) points += ", ";
    points += QStringLiteral("[%1, %2]").arg(point.x()).arg(point.y());
  }
  return QStringLiteral("{\"type\" : \"polyline\", \"points\" : [%1], "
                        "\"color\" : \"%2\", \"thick\" : %3, \"style\" : \"%4\", "
                        "\"closed\" : %5},")
      .arg(points, m_pen.color().name())
      .arg(m_pen.width())
      .arg(toPenString(m_pen.style()))
      .arg(m_closed ? "true" : "false");
}

bool PolylinePainting::getSelected(const QPoint& click, int tolerance)
{
  if (m_points.size() < 2) return false;

  if (m_filled && (m_closed || m_points.size() > 2))
    if (polygon().containsPoint(click, Qt::OddEvenFill)) return true;

  for (std::size_t i = 1; i < m_points.size(); i++)
    if (qucs_s::geom::is_near_line(click, m_points[i - 1], m_points[i], tolerance)) return true;

  if ((m_closed || m_filled) && m_points.size() > 2)
    if (qucs_s::geom::is_near_line(click, m_points.back(), m_points.front(), tolerance)) return true;

  return false;
}

bool PolylinePainting::resizeTouched(const QPoint& click, int tolerance)
{
  for (std::size_t i = 0; i < m_points.size(); i++)
    if (near(click, m_points[i], tolerance)) {
      m_dragged = int(i);
      return true;
    }

  m_dragged = -1;
  return false;
}

void PolylinePainting::MouseResizeMoving(int x, int y, Schematic* sch)
{
  if (m_dragged < 0 || m_dragged >= int(m_points.size())) return;

  m_points[m_dragged] = QPoint(x, y);
  updateBounds();
  paintScheme(sch);
}

void PolylinePainting::MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& cursor)
{
  if (m_beingDrawn && !m_points.empty()) {
    m_points.back() = onGrid;
    updateBounds();
    paintScheme(sch);
  } else {
    m_points.clear();
    m_points.push_back(onGrid);
    updateBounds();
  }

  // The cursor: three segments of a run.
  sch->PostPaintEvent(_Line, cursor.x() + 12, cursor.y() + 14, cursor.x() + 20, cursor.y(), 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 20, cursor.y(), cursor.x() + 28, cursor.y() + 10, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 28, cursor.y() + 10, cursor.x() + 36, cursor.y() + 2, 0, 0, true);
}

// A click fixes a corner. On the last corner it ends the run; on the
// first one it closes it into a polygon.
bool PolylinePainting::MousePressing(Schematic*)
{
  if (!m_beingDrawn) {
    const QPoint start = m_points.empty() ? QPoint() : m_points.front();
    m_points.clear();
    m_points.push_back(start);
    m_points.push_back(start);   // the one that follows the cursor
    m_beingDrawn = true;
    updateBounds();
    return false;
  }

  const QPoint at = m_points.back();
  const std::size_t fixed = m_points.size() - 1;   // without the one on the cursor

  if (fixed >= 3 && near(at, m_points.front())) {
    m_points.pop_back();
    m_closed = true;
    m_beingDrawn = false;
    updateBounds();
    return true;
  }

  if (fixed >= 2 && near(at, m_points[fixed - 1])) {
    m_points.pop_back();
    m_beingDrawn = false;
    updateBounds();
    return true;
  }

  m_points.push_back(at);   // fix this corner, carry on with the next
  updateBounds();
  return false;
}

void PolylinePainting::snapToGrid(Schematic* sch)
{
  for (QPoint& point : m_points) {
    int x = point.x(), y = point.y();
    sch->setOnGrid(x, y);
    point = QPoint(x, y);
  }
  updateBounds();
}

bool PolylinePainting::rotate() noexcept
{
  return rotate(cx, cy);
}

bool PolylinePainting::rotate(int xc, int yc) noexcept
{
  if (m_points.size() < 2) return false;

  for (QPoint& point : m_points) {
    int x = point.x(), y = point.y();
    qucs_s::geom::rotate_point_ccw(x, y, xc, yc);
    point = QPoint(x, y);
  }
  updateBounds();
  return true;
}

bool PolylinePainting::mirrorX() noexcept
{
  if (m_points.size() < 2) return false;

  for (QPoint& point : m_points) point.setY(qucs_s::geom::mirror_coordinate(point.y(), cy));
  updateBounds();
  return true;
}

bool PolylinePainting::mirrorY() noexcept
{
  if (m_points.size() < 2) return false;

  for (QPoint& point : m_points) point.setX(qucs_s::geom::mirror_coordinate(point.x(), cx));
  updateBounds();
  return true;
}

void PolylinePainting::afterMove(int dx, int dy) noexcept
{
  for (QPoint& point : m_points) point += QPoint(dx, dy);
}

void PolylinePainting::updateBounds() noexcept
{
  if (m_points.empty()) {
    x1 = y1 = x2 = y2 = 0;
    updateCenter();
    return;
  }

  x1 = x2 = m_points.front().x();
  y1 = y2 = m_points.front().y();
  for (const QPoint& point : m_points) {
    x1 = std::min(x1, point.x());
    x2 = std::max(x2, point.x());
    y1 = std::min(y1, point.y());
    y2 = std::max(y2, point.y());
  }
  updateCenter();
}

bool PolylinePainting::Dialog(QWidget* parent)
{
  bool changed = false;

  auto dialog = std::make_unique<FillDialog>(QObject::tr("Edit Polyline Properties"), true, parent);
  misc::setPickerColor(dialog->ColorButt, m_pen.color());
  dialog->LineWidth->setText(QString::number(m_pen.width()));
  dialog->StyleBox->setCurrentIndex(m_pen.style() - 1);
  misc::setPickerColor(dialog->FillColorButt, m_brush.color());
  dialog->FillStyleBox->setCurrentIndex(m_brush.style());
  dialog->CheckFilled->setChecked(m_filled);
  dialog->slotCheckFilled(m_filled);

  if (dialog->exec() == QDialog::Rejected) return false;

  const QColor colour = misc::getWidgetBackgroundColor(dialog->ColorButt);
  if (m_pen.color() != colour) { m_pen.setColor(colour); changed = true; }

  const int width = dialog->LineWidth->text().toInt();
  if (m_pen.width() != width) { m_pen.setWidth(width); changed = true; }

  const Qt::PenStyle style = (Qt::PenStyle)(dialog->StyleBox->currentIndex() + 1);
  if (m_pen.style() != style) { m_pen.setStyle(style); changed = true; }

  const QColor fill = misc::getWidgetBackgroundColor(dialog->FillColorButt);
  if (m_brush.color() != fill) { m_brush.setColor(fill); changed = true; }

  const Qt::BrushStyle fillStyle = (Qt::BrushStyle)dialog->FillStyleBox->currentIndex();
  if (m_brush.style() != fillStyle) { m_brush.setStyle(fillStyle); changed = true; }

  if (m_filled != dialog->CheckFilled->isChecked()) {
    m_filled = dialog->CheckFilled->isChecked();
    changed = true;
  }

  return changed;
}
