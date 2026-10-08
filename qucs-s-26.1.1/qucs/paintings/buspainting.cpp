/*
 * buspainting.cpp - a bus: a thick line named for the nets it gathers
 * (see buspainting.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "buspainting.h"

#include "multi_point.h"
#include "ink.h"
#include "misc.h"

#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpression>

#include <cmath>

BusPainting::BusPainting(int ax, int ay, int bx, int by, const QString& name)
    : GraphicLine(ax, ay, bx, by, QPen(QColor(20, 40, 140), Width)), busName(name)
{
  Name = "Bus ";
}

bool BusPainting::MousePressing(Schematic* sch)
{
  if (m_started && x1 == x2 && y1 == y2) return false;
  m_started = !m_started;
  return GraphicLine::MousePressing(sch);
}

Painting* BusPainting::newOne()
{
  return new BusPainting();
}

Element* BusPainting::info(QString& name, char*& bitmapFile, bool getNewOne)
{
  name = QObject::tr("Bus");
  bitmapFile = (char*)"bus";
  if (getNewOne) return new BusPainting();
  return nullptr;
}

bool BusPainting::membersOf(const QString& name, QStringList* members)
{
  members->clear();
  // D[7:0], D[0:7], D[0..7]: a name and the first and last member's numbers.
  static const QRegularExpression bus(QStringLiteral("^([A-Za-z_][A-Za-z0-9_]*)\\[(\\d{1,4})(?::|\\.\\.)(\\d{1,4})\\]$"));
  const QRegularExpressionMatch m = bus.match(name.trimmed());
  if (!m.hasMatch()) return false;
  const int from = m.captured(2).toInt(), to = m.captured(3).toInt();
  if (from == to || std::abs(from - to) > 1023) return false;
  for (int i = from; ; i += from < to ? 1 : -1) {
    *members << m.captured(1) + QString::number(i);
    if (i == to) break;
  }
  return true;
}

QStringList BusPainting::members() const
{
  QStringList out;
  membersOf(busName, &out);
  return out;
}

bool BusPainting::touches(const QPoint& p) const
{
  // (On the segment, a unit's rounding either way.)
  const double dx = x2 - x1, dy = y2 - y1, length2 = dx * dx + dy * dy;
  if (length2 == 0) return p == QPoint(x1, y1);
  const double t = ((p.x() - x1) * dx + (p.y() - y1) * dy) / length2;
  if (t < 0 || t > 1) return false;
  return std::hypot(x1 + t * dx - p.x(), y1 + t * dy - p.y()) <= 1.0;
}

void BusPainting::paint(QPainter* painter)
{
  painter->save();
  const QColor ink(20, 40, 140);
  painter->setPen(qucs_s::ink::on(QPen(ink, Width, Qt::SolidLine, Qt::RoundCap)));
  painter->drawLine(x1, y1, x2, y2);
  if (isSelected) {
    painter->setPen(qucs_s::ink::on(QPen(Qt::darkGray, Width + 5)));
    painter->drawLine(x1, y1, x2, y2);
    painter->setPen(qucs_s::ink::on(QPen(Qt::white, Width - 2)));
    painter->drawLine(x1, y1, x2, y2);
    misc::draw_resize_handle(painter, QPoint{x1, y1});
    misc::draw_resize_handle(painter, QPoint{x2, y2});
  }
  // Its name above its first end (an unnamed one says so, faint).
  painter->setPen(qucs_s::ink::on(busName.isEmpty() ? QColor(150, 150, 150) : ink));
  painter->drawText(QPoint(std::min(x1, x2) + 4, std::min(y1, y2) - Width), busName.isEmpty() ? QObject::tr("bus (no name)") : busName);
  painter->restore();
}

bool BusPainting::load(const QString& s)
{
  bool ok = false;
  x1 = misc::clampCoordinate(s.section(' ', 1, 1).toInt(&ok));
  if (!ok) return false;
  y1 = misc::clampCoordinate(s.section(' ', 2, 2).toInt(&ok));
  if (!ok) return false;
  x2 = x1 + misc::clampCoordinate(s.section(' ', 3, 3).toInt(&ok));
  if (!ok) return false;
  y2 = y1 + misc::clampCoordinate(s.section(' ', 4, 4).toInt(&ok));
  if (!ok) return false;
  busName = s.section('"', 1, 1);
  updateCenter();
  return true;
}

QString BusPainting::save()
{
  return Name + QString::number(x1) + " " + QString::number(y1) + " " + QString::number(x2 - x1) + " "
         + QString::number(y2 - y1) + " \"" + busName + "\"";
}

QString BusPainting::saveJSON()
{
  return QStringLiteral("{\"type\" : \"bus\", \"x1\" : %1, \"y1\" : %2, \"x2\" : %3, \"y2\" : %4, \"name\" : \"%5\"},")
      .arg(x1)
      .arg(y1)
      .arg(x2 - x1)
      .arg(y2 - y1)
      .arg(busName);
}

bool BusPainting::Dialog(QWidget* parent)
{
  bool ok = false;
  const QString name = QInputDialog::getText(parent, QObject::tr("Bus"),
                                             QObject::tr("Its name, with its members' numbers: D[7:0] gathers the nets D7 to D0 "
                                                         "(a bus joins nothing by touching: a net is a member by its name)"),
                                             QLineEdit::Normal, busName, &ok)
                           .trimmed();
  if (!ok || name == busName) return false;
  QStringList members;
  if (!name.isEmpty() && !membersOf(name, &members)) {
    QMessageBox::warning(parent, QObject::tr("Bus"),
                         QObject::tr("%1 is no bus's name: give a name and its members' numbers, D[7:0] or A[0:15].").arg(name));
    return false;
  }
  busName = name;
  return true;
}
