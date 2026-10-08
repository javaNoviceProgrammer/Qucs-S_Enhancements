/***************************************************************************
                          polardiagram.cpp  -  description
                             -------------------
    begin                : Fri Oct 17 2003
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

/*!
  \class PolarDiagram
  \brief The PolarDiagram class implements the Polar diagram
*/

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <cmath>
#include <float.h>
#if HAVE_IEEEFP_H
# include <ieeefp.h>
#endif

#include "polardiagram.h"
#include "ink.h"

#include <QPainter>
#include <algorithm>
#include <cmath>


PolarDiagram::PolarDiagram(int _cx, int _cy) : Diagram(_cx, _cy)
{
  x1 = 10;     // position of label text
  y1 = 2;
  x2 = 200;    // initial size of diagram
  y2 = 200;
  x3 = 207;    // with some distance for right axes text
  Name = "Polar";

  Arcs.append(as(Part::Frame, new struct qucs::Arc(0, y2, x2, y2, 0, 16*360, QPen(Qt::black,0))));
//  calcDiagram();
}

PolarDiagram::~PolarDiagram()
{
}

// ------------------------------------------------------------
void PolarDiagram::calcCoordinate(const double*, const double* yD, const double*,
				  float *px, float *py, Axis const*) const
{
  double yr = yD[0];
  double yi = yD[1];
  *px = float((yr/yAxis.up + 1.0)*double(x2)/2.0);
  *py = float((yi/yAxis.up + 1.0)*double(y2)/2.0);

  if(std::isfinite(*px))
    if(std::isfinite(*py))
      return;

  *px = *py = float(cx) / 2.0;
}

// --------------------------------------------------------------
void PolarDiagram::calcLimits()
{
  double a, b;
  calcPolarAxisScale(&yAxis, a, yAxis.step, b);
  yAxis.limit_min = 0.0;
  yAxis.limit_max = yAxis.up;
}

// --------------------------------------------------------------
int PolarDiagram::calcDiagram()
{
  qDeleteAll(Lines);
  Lines.clear();
  qDeleteAll(Texts);
  Texts.clear();
  qDeleteAll(Arcs);
  Arcs.clear();

  // x line
  Lines.append(as(Part::Grid, new qucs::Line(0, y2>>1, x2, y2>>1, GridPen)));

  x3 = x2 + 7;
  // A Nyquist plot: the critical point in sight.
  if (nyquist && yAxis.autoScale) yAxis.max = std::max(yAxis.max, 1.05);
  createPolarDiagram(&yAxis);
  return 3;
}

// ------------------------------------------------------------
QString PolarDiagram::extraSaveFields() const
{
  if (!nyquist && !mirror) return QString();   // (as before: none)
  return QStringLiteral(" %1 %2").arg(nyquist ? 1 : 0).arg(mirror ? 1 : 0);
}

void PolarDiagram::loadExtraFields(const QStringList& fields)
{
  nyquist = fields.value(0) == QLatin1String("1");
  mirror = fields.value(1) == QLatin1String("1");
}

void PolarDiagram::paintBehindGraphs(QPainter* painter)
{
  if (!mirror) return;
  // The negative frequencies: each graph's conjugate, dashed (y up).
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  for (const Graph* g : Graphs) {
    const DataX* xs = g->axis(0);
    if (xs == nullptr || g->cPointsY == nullptr) continue;
    for (int c = 0; c < g->countY; ++c) {
      QPolygonF line;
      for (int i = 0; i < xs->count; ++i) {
        const double re = g->cPointsY[2 * (c * xs->count + i)], im = g->cPointsY[2 * (c * xs->count + i) + 1];
        const double y[2] = {re, -im};
        float px = 0, py = 0;
        calcCoordinate(nullptr, y, nullptr, &px, &py, &yAxis);
        line << QPointF(px, py);
      }
      painter->setPen(QPen(qucs_s::ink::on(g->Color), std::max(1, g->Thick), Qt::DashLine));
      painter->drawPolyline(line);
    }
  }
  painter->restore();
}

void PolarDiagram::paintInFront(QPainter* painter, const Colors& colors)
{
  Diagram::paintInFront(painter, colors);
  if (!nyquist || !(yAxis.up > 0)) return;
  // (y down.) The unit circle, dashed, and -1, a red cross in a circle.
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  const QPointF centre(x2 / 2.0, -y2 / 2.0);
  const double r = x2 / 2.0 / yAxis.up;
  painter->setPen(QPen(qucs_s::ink::on(QColor(110, 110, 140)), 0, Qt::DashLine));
  painter->drawEllipse(centre, r, r * double(y2) / double(x2));
  const QPointF critical(centre.x() - r, centre.y());
  painter->setPen(QPen(qucs_s::ink::on(QColor(210, 30, 30)), 2));
  painter->drawLine(critical + QPointF(-6, 0), critical + QPointF(6, 0));
  painter->drawLine(critical + QPointF(0, -6), critical + QPointF(0, 6));
  painter->drawEllipse(critical, 4.0, 4.0);
  painter->drawText(critical + QPointF(-14, -8), QStringLiteral("-1"));
  painter->restore();
}

// ------------------------------------------------------------
QList<Diagram::Part> PolarDiagram::themeParts() const
{
  QList<Part> parts = Diagram::themeParts();
  parts.removeAll(Part::RightAxis);
  return parts;
}

QPainterPath PolarDiagram::plotAreaShape() const
{
  QPainterPath path;
  path.addEllipse(QRectF(0, -y2, x2, y2));
  return path;
}

// ------------------------------------------------------------
Diagram* PolarDiagram::newOne()
{
  return new PolarDiagram();
}

// ------------------------------------------------------------
Element* PolarDiagram::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("Polar");
  BitmapFile = (char *) "polar";

  if(getNewOne)  return new PolarDiagram();
  return 0;
}
