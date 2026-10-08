/***************************************************************************
                          marker.cpp  -  description
                             -------------------
    begin                : Sat Apr 10 2004
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
  \class Marker
  \brief The Marker class implements the marker object used for all the
         diagram
*/

#include "marker.h"

#include <algorithm>
#include "diagram.h"
#include "graph.h"
#include "ink.h"
#include "one_point.h"
#include "main.h"

#include <QLocale>
#include <QString>
#include <QPainter>
#include <QPainterPath>
#include <QDebug>

#include <limits.h>
#include <cmath>
#include <stdlib.h>

#include "misc.h"
#include "qucs_assert.h"

static double default_Z0=50;

#define IND_SIZE 8

/*!
 * create a marker based on click position and
 * the branch number.
 *
 * the click position is used to compute the marker position. currently, the
 * marker position is the sampling point closest to the click.
 */

Marker::Marker(Graph *pg_, int branchNo, int cx_, int cy_) :
  Element(),
  pGraph(pg_),
  Precision(3),
  numMode(0),
  indicatorMode(indicator_Triangle),
  notation(-1),
  Z0(default_Z0) // BUG: see declaration.
{
  Type = isMarker;
  isSelected = transparent = false;

  cx =  cx_;
  cy = -cy_;
  fCX = float(cx);
  fCY = float(cy);
  if(!pGraph){
    makeInvalid();
  }else{
    initText(branchNo);   // finally create marker
    createText();
  }

  x1 =  cx + 60;
  y1 = -cy - 60;

}

Marker::~Marker()
{
}

/*!
 * Original function doctext:
 *   compute VarPos from branch number n and click position (cx, cy)
 *   this is done by recreating branch samples and comparing against click
 *
 *   FIXME: should use ScrPoints instead. do not call calcCoordinate from here!
 *
 * 2025.06.03:
 *   This function not initializes text, but also populates VarPos member field.
 *
 *   Original doctext calls function parameter a "branch number", which is not
 *   exactly true. To explain the meaning of this parameter a few words on how
 *   Graph stores its datapoints have to be said beforehand.
 *
 *   A graph consists of one more "branches" and it stores all datapoints in one
 *   sequence. Special pseudo datapoints divide the sequence into subsequences
 *   each corresponding to a "branch".
 *
 *   This function, as it comes from its usage, is supplied with the number of
 *   datapoints before the subsequence of datapoints corresponding to a branch
 *   on which the click was made.
 */
void Marker::initText(int datapoints_before_branch)
{
  // (A graph its diagram does not draw - an axis it cannot lay out - has
  // its x but no y: nothing to walk.)
  if (pGraph->isEmpty() || pGraph->cPointsY == nullptr) {
    makeInvalid();
    return;
  }

  QUCS_ASSERT(diag());
  Axis const *pa = diag()->graphAxis(pGraph);

  double Dummy = 0.0; // needed for 2D graph in 3D diagram
  double *py = &Dummy;
  Text = "";

  bool isCross = false;
  int nn;
  DataX const *x_axis_values = pGraph->axis(0);
  double* x_axis_values_arr_p = x_axis_values->Points;
  std::size_t x_axis_values_count = x_axis_values->count;
  DataX const *pDy = pGraph->axis(1);
  if (pDy) { // only for 3D diagram
    nn = pGraph->countY * x_axis_values->count;
    py = pDy->Points;
    if (datapoints_before_branch >= nn) { // is on cross grid ?
      isCross = true;
      datapoints_before_branch -= nn;
      datapoints_before_branch /= x_axis_values_count;
      x_axis_values_arr_p += (datapoints_before_branch % x_axis_values_count);
      if (pGraph->axis(2)) { // more than 2 indep variables ?
        datapoints_before_branch = (datapoints_before_branch % x_axis_values_count) + (datapoints_before_branch / x_axis_values_count) * x_axis_values_count * pDy->count;
      }
      x_axis_values_count = pDy->count;
    } else {
      py += (datapoints_before_branch / x_axis_values->count) % pDy->count;
    }
  }

  // find exact marker position
  std::size_t closest_datapoint_ix = x_axis_values_count - 1;
  double* pz = pGraph->cPointsY + 2 * datapoints_before_branch;
  double smallest_distance = std::numeric_limits<double>::max();
  for (std::size_t datapoint_ix = 0; datapoint_ix < x_axis_values_count; datapoint_ix++) {
    diag()->calcCoordinate(x_axis_values_arr_p, pz, py, &fCX, &fCY, pa);
    ++x_axis_values_arr_p;
    pz += 2;
    if (isCross) {
      x_axis_values_arr_p--;
      py++;
      pz += 2 * (x_axis_values->count - 1);
    }

    // Here distance between click screen coordinates (cx, cy) and
    // datapoint screen coordinates (fCX, fCY) is assesed.
    const double x = fCX + 0.5 - cx;
    const double y = fCY + 0.5 - cy;
    const double r_square = x * x + y * y;
    if (r_square < smallest_distance) {
      smallest_distance = r_square;
      closest_datapoint_ix = datapoint_ix;
    }
  }
  if (isCross) {
    closest_datapoint_ix *= x_axis_values->count;
  }
  datapoints_before_branch += closest_datapoint_ix;

  // why check over and over again?! do in the right place and just assert
  // otherwise.
  if (VarPos.size() != pGraph->numAxes()) {
    qDebug() << "huh, wrong size" << VarPos.size() << pGraph->numAxes();
    VarPos.resize(pGraph->numAxes());
  }

  // gather text of all independent variables
  nn = datapoints_before_branch;
  for (unsigned axis_ix = 0; (x_axis_values = pGraph->axis(axis_ix)); ++axis_ix) {
    const double* x_value = x_axis_values->Points + (nn % x_axis_values->count);
    VarPos[axis_ix] = *x_value;
    Text += x_axis_values->Var + ": " + QString::number(*x_value, 'g', Precision) + "\n";
    nn /= x_axis_values->count;
  }

  // createText();
}

// ---------------------------------------------------------------------
/*!
 * (should)
 * create marker label Text the screen position cx and cy from VarPos.
 * does a lot of fancy stuff to be sorted out.
 */
void Marker::createText()
{
  if(!(pGraph->cPointsY)) {
    makeInvalid();
    return;
  }

  unsigned nVarPos = VarPos.size();

  if(nVarPos > pGraph->numAxes()){
    qDebug() << "huh, VarPos too big?!";
  }
  if(nVarPos != pGraph->numAxes()){
    qDebug() << "padding" << VarPos.size() << pGraph->numAxes();
    VarPos.resize(pGraph->numAxes());
    while((unsigned int)nVarPos < pGraph->numAxes()){
      VarPos[nVarPos++] = 0.; // pad
    }
  }

  // independent variables
  Text = "";
  double *pp;
  nVarPos = pGraph->numAxes();
  DataX const *pD;

  auto p = pGraph->findSample(VarPos);
  VarDep[0] = p.first;
  VarDep[1] = p.second;

  double v=0.;   // needed for 2D graph in 3D diagram
  double *py=&v;
  pD = pGraph->axis(0);
  if(pGraph->axis(1)) {
    *py = VarPos[1];
  }

  double pz[2];
  pz[0] = VarDep[0];
  pz[1] = VarDep[1];

  // now actually create text.
  // Every number in its notation (its own, or the diagram's).
  for(unsigned ii=0; (pD=pGraph->axis(ii)); ++ii) {
    Text += pD->Var + ": ";
    Text += numberText(VarPos[ii]) + "\n";
  }

  Text += pGraph->withValuePart(pGraph->Var.contains('/') ? pGraph->Var.section('/', 1) : pGraph->Var) + ": ";
  const Axis *ax = diag()->graphAxis(pGraph);
  int units = ax->Units;
  if (units == Axis::NoUnits || !ax->log) {
      Text += complexText(pz[0], pz[1], numMode);
  } else {
      double mag = sqrt(pz[0]*pz[0] + pz[1]*pz[1]);
      double val = qucs::num2db(mag,ax->Units);
      // (A notation left a blank line under it, as the engineering one
      // always had.)
      Text += numberText(val);
  }

  // A delta marker: from its reference, x and the value, and 1/Δx (a
  // frequency from a time, a period from a frequency).
  if (const Marker* ref = reference(); ref && !ref->varPos().empty() && !VarPos.empty()) {
    const double dx = VarPos[0] - ref->varPos().front();
    Text += QString::fromUtf8("\nΔx: ") + numberText(dx);
    // In the complex plane (a Smith chart, a polar diagram) neither axis
    // is the magnitude: the change of the magnitude, and the distance
    // between the two points, each named (bug hunt of 2026-10-08, N23).
    const QString kind = diag()->Name;
    if (kind == "Smith" || kind == "ySmith" || kind == "Polar" || kind == "PS" || kind == "SP") {
      std::vector<double> mine = VarPos, theirs = ref->varPos();
      const auto a = pGraph->findSample(mine);
      const auto b = ref->graph() ? ref->graph()->findSample(theirs) : std::pair<double, double>(0.0, 0.0);
      Text += QString::fromUtf8("\nΔ|y|: ") + numberText(std::hypot(a.first, a.second) - std::hypot(b.first, b.second));
      Text += QString::fromUtf8("\n|Δy|: ") + numberText(std::hypot(a.first - b.first, a.second - b.second));
    } else {
      Text += QString::fromUtf8("\nΔy: ") + numberText(shownValue() - ref->shownValue());
    }
    if (dx != 0.0) Text += QString::fromUtf8("\n1/Δx: ") + numberText(1.0 / dx);
  }

  QUCS_ASSERT(diag());
  Text += diag()->extraMarkerText(this);

  Axis const *pa = diag()->graphAxis(pGraph);
  pp = &(VarPos[0]);

  diag()->calcCoordinate(pp, pz, py, &fCX, &fCY, pa);
  diag()->finishMarkerCoordinates(fCX, fCY);

  cx = int(fCX+0.5);
  cy = int(fCY+0.5);
  getTextSize();
}

// ---------------------------------------------------------------------
qucs_s::numberformat::Notation Marker::shownNotation() const
{
  if (notation >= 0) return qucs_s::numberformat::fromInt(notation);
  return diag() ? diag()->notation : qucs_s::numberformat::Notation::Automatic;
}

QString Marker::numberText(double v) const
{
  // Automatic as markers always wrote numbers (significant digits); the
  // others with the precision as places after the point.
  const qucs_s::numberformat::Notation n = shownNotation();
  return n == qucs_s::numberformat::Notation::Automatic ? QString::number(v, 'g', Precision)
                                                        : qucs_s::numberformat::format(v, n, Precision);
}

QString Marker::complexText(double re, double im, int mode) const
{
  // As misc::complexRect, complexDeg and complexRad write it, with the
  // numbers in its notation: a real value alone, else re+jim, or the
  // magnitude / the angle.
  if (std::fabs(im) < 1e-250) return numberText(re);
  if (mode == nM_Deg)
    return numberText(std::sqrt(re*re + im*im)) + " / " + numberText(180.0/pi*std::atan2(im, re)) + QString::fromUtf8("°");
  if (mode == nM_Rad)
    return numberText(std::sqrt(re*re + im*im)) + " / " + numberText(std::atan2(im, re)) + "rad";
  QString imag = numberText(im);
  if (imag.startsWith('-')) imag = "-j" + imag.mid(1);
  else imag = "+j" + imag;
  return numberText(re) + imag;
}

// ---------------------------------------------------------------------
void Marker::makeInvalid()
{
  fCX = fCY = -1e3; // invalid coordinates
  QUCS_ASSERT(diag());
  diag()->finishMarkerCoordinates(fCX, fCY); // leave to diagram
  cx = int(fCX+0.5);
  cy = int(fCY+0.5);

  Text = QObject::tr("invalid");
  getTextSize();
}

// ---------------------------------------------------------------------
void Marker::getTextSize()
{
  // get size of text using the screen-compatible metric
  QFontMetrics metrics(QucsSettings.font, 0);
  QSize r = metrics.size(0, Text);
  x2 = r.width()+5;
  y2 = r.height()+5;
}

// ---------------------------------------------------------------------
bool Marker::moveLeftRight(bool left)
{
  int n;
  double *px;

  DataX const *pD = pGraph->axis(0);
  px = pD->Points;
  if(!px) return false;
  for(n=0; n<pD->count; n++) {
    if(VarPos[0] <= *px) break;
    px++;
  }
  if(n == pD->count) px--;

  if(left) {
    if(px <= pD->Points) return false;
    px--;  // one position to the left
  }
  else {
    if(px >= (pD->Points + pD->count - 1)) return false;
    px++;  // one position to the right
  }
  VarPos[0] = *px;
  createText();
  refreshDependents();

  return true;
}

// ---------------------------------------------------------------------
bool Marker::moveUpDown(bool up)
{
  int n, i=0;
  double *px;

  DataX const *pD = pGraph->axis(0);
  if(!pD) return false;

  if(up) {  // move upwards ? **********************
    do {
      pD = pGraph->axis(++i);
      if(!pD) return false;
      px = pD->Points;
      if(!px) return false;
      for(n=1; n<pD->count; n++) {  // go through all data points
        if(fabs(VarPos[i]-(*px)) < fabs(VarPos[i]-(*(px+1)))) break;
        px++;
      }

    } while(px >= (pD->Points + pD->count - 1));  // go to next dimension ?

    px++;  // one position up
    VarPos[i] = *px;
    while(i > 1) {
      pD = pGraph->axis(--i);
      VarPos[i] = *(pD->Points);
    }
  }
  else {  // move downwards **********************
    do {
      pD = pGraph->axis(++i);
      if(!pD) return false;
      px = pD->Points;
      if(!px) return false;
      for(n=0; n<pD->count; n++) {
        if(fabs(VarPos[i]-(*px)) < fabs(VarPos[i]-(*(px+1)))) break;
        px++;
      }

    } while(px <= pD->Points);  // go to next dimension ?

    px--;  // one position down
    VarPos[i] = *px;
    while(i > 1) {
      pD = pGraph->axis(--i);
      VarPos[i] = *(pD->Points + pD->count - 1);
    }
  }
  createText();

  return true;
}

namespace { // Helpers to be used in Marker::paint

// draws upside-down triangle with tip at given point
void triangle_marker(QPainter* p, const QPointF& triangle_head) {
  constexpr double cos60              = 0.866;
  constexpr double triangle_alt       = IND_SIZE * cos60;
  constexpr double triangle_half_edge = IND_SIZE / 2.0;

  // This is the triangle that we draw here:
  // a - - - b
  //  \     /
  //   \   /
  //    \ /
  //     h

  QPointF a{triangle_head.x() - triangle_half_edge,
            triangle_head.y() - triangle_alt};
  QPointF b{triangle_head.x() + triangle_half_edge,
            triangle_head.y() - triangle_alt};

  p->drawLine(triangle_head, a);
  p->drawLine(triangle_head, b);
  p->drawLine(a, b);
}

// draws a square with center at given point
void square_marker(QPainter* p, const QPointF& square_center) {
  QRectF r{0, 0, IND_SIZE, IND_SIZE};
  r.moveCenter(square_center);
  p->drawRect(r);
}
} // namespace

QColor Marker::shownTextColor() const
{
  return textColor.isValid() ? textColor : qucs_s::ink::on(Qt::black);
}

QColor Marker::shownFillColor() const
{
  // The paper drawn on - not the widget's background, which eraseRect()
  // took: the dark canvas's, around black text, in the dark theme.
  return fillColor.isValid() ? fillColor : qucs_s::ink::paper();
}

void Marker::paint(QPainter* painter) {
  // Marker inherits from Element four member vars: cx, cy, x1, y1
  // and uses them like this:
  //   - Point (x1,y1) defines top left corner of a box containing marker's text
  //   - Point (cx,cy) define a place on a graph to which the marker points,
  //     i.e. the marker's root
  // All these coordinates a relative to parent diagram's bottom left corner.

  painter->save();
  painter->translate(pGraph->parentDiagram()->cx, pGraph->parentDiagram()->cy);

  const QSize text_size = painter->fontMetrics().size(0, Text);
  const QRectF text_box{QPointF{static_cast<qreal>(x1), static_cast<qreal>(y1)},
                        text_size};

  if (!transparent) {
    painter->fillRect(text_box, shownFillColor());
  }

  painter->setPen(QPen(shownTextColor(), 1));
  painter->drawText(x1, y1, 1, 1, Qt::TextDontClip, Text);

  painter->setPen(QPen(qucs_s::ink::on(Qt::darkMagenta), 0));
  painter->drawRect(text_box);

  // `cy` is inverted because painter's Y-axis grows downwards but marker's `cy`
  // coordinate is defined in traditional coordinate system where Y-axis growing
  // upwards
  const QPointF marker_root{static_cast<qreal>(cx), static_cast<qreal>(-cy)};

  // Connect marker root and textbox
  painter->drawLine(
      marker_root,
      {marker_root.x() > text_box.right() ? text_box.right() : text_box.left(),
       marker_root.y() > text_box.bottom() ? text_box.bottom()
                                           : text_box.top()});

  // A delta marker: a dashed line from its reference's point to its own.
  if (const Marker* ref = reference()) {
    painter->save();
    painter->setPen(QPen(qucs_s::ink::on(Qt::darkMagenta), 0, Qt::DashLine));
    painter->drawLine(QPointF(ref->cx, -ref->cy), marker_root);
    painter->restore();
  }

  switch (indicatorMode) {
  case indicator_Square:
    square_marker(painter, marker_root);
    break;
  case indicator_Triangle:
    triangle_marker(painter, marker_root);
    break;
  default:;
  }

  if (isSelected) {
    painter->setPen(QPen(Qt::darkGray, 3));
    painter->drawRoundedRect(text_box.marginsAdded(QMargins{3, 3, 3, 3}), 4, 4);
  }

  painter->restore();
}

// -------------------------------------------------------
void Marker::Bounding(int& _x1, int& _y1, int& _x2, int& _y2)
{
  if(diag()) {
    _x1 = diag()->cx + x1;
    _y1 = diag()->cy + y1;
    _x2 = diag()->cx + x1+x2;
    _y2 = diag()->cy + y1+y2;
  }
  else {
    _x1 = x1;
    _y1 = y1+y2;
    _x2 = x1+x2;
    _y2 = y1;
  }
}

// ---------------------------------------------------------------------
QString Marker::save()
{
  QString s  = "<Mkr ";

  // As many digits as read it back as it was: at 6 it moved to another
  // sample on save, reopening and undo, and at an edge read the other side
  // of it (bug hunt of 2026-10-08, B9).
  for(auto i : VarPos){
    s += QString::number(i, 'g', QLocale::FloatingPointShortest)+"/";
  }
  s.replace(s.length()-1,1,' ');
  //s.at(s.length()-1) = (const QChar&)' ';

  s += QString::number(x1) +" "+ QString::number(y1) +" "
      +QString::number(Precision) +" "+ QString::number(numMode);
  s += transparent ? " 1" : " 0";
  // Then, when they are not the defaults (older files end above, and
  // older versions stop reading there): the indicator, the text's and
  // the background's colours ("-": automatic).
  const auto colour = [](const QColor& c) {
    return !c.isValid() ? QStringLiteral("-") : c.name(c.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
  };
  const bool colours = textColor.isValid() || fillColor.isValid();
  // Then a notation of its own (none: the diagram's); last, a delta
  // marker's reference, by its number among the diagram's markers.
  int ref = 0;
  if (const Marker* r = reference(); r && diag()) ref = int(diag()->markers().indexOf(const_cast<Marker*>(r))) + 1;
  const bool own = notation >= 0 || ref > 0;
  if (indicatorMode != indicator_Triangle || colours || own) s += " " + QString::number(int(indicatorMode));
  if (colours || own) s += " " + colour(textColor) + " " + colour(fillColor);
  if (own) s += " " + QString::number(notation);
  if (ref > 0) s += " " + QString::number(ref);
  return s + ">";
}

// ---------------------------------------------------------------------
// All graphs must have been loaded before this function !
bool Marker::load(const QString& Line)
{
  bool ok;
  QString s = Line;

  if (s.length() < 2 || !s.startsWith('<') || !s.endsWith('>')) return false;
  s = s.mid(1, s.length()-2);   // cut off start and end character

  if(s.section(' ',0,0) != "Mkr") return false;

  int i=0, j;
  QString n = s.section(' ',1,1);    // VarPos

  unsigned nVarPos = 0;
  j = (n.count('/') + 3);
  VarPos.resize(j);

  do {
    j = n.indexOf('/', i);
    VarPos[nVarPos++] = n.mid(i,j-i).toDouble(&ok);
    if(!ok) return false;
    i = j+1;
  } while(j >= 0);
  // As many as were read: the two more kept were saved too, each save
  // lengthening the line while the data was missing (createText() pads
  // it to the graph's axes when there is data).
  VarPos.resize(nVarPos);

  n  = s.section(' ',2,2);    // x1
  x1 = misc::clampCoordinate(n.toInt(&ok));
  if(!ok) return false;

  n  = s.section(' ',3,3);    // y1
  y1 = misc::clampCoordinate(n.toInt(&ok));
  if(!ok) return false;

  n  = s.section(' ',4,4);      // Precision
  Precision = n.toInt(&ok);
  if(!ok) return false;
  // As the dialog allows: a file's 999999999 made a label of gigabytes.
  Precision = std::clamp(Precision, 0, 12);

  n  = s.section(' ',5,5);      // numMode
  numMode = n.toInt(&ok);
  if(!ok) return false;

  n  = s.section(' ',6,6);      // transparent
  if(n.isEmpty()) return true;  // is optional
  transparent = n != "0";

  n  = s.section(' ',7,7);      // the indicator (optional)
  if(n.isEmpty()) return true;
  const int indicator = n.toInt(&ok);
  if(ok && indicator >= indicator_Off && indicator <= indicator_Triangle)
    indicatorMode = static_cast<indicatorMode_t>(indicator);

  // The text's and the background's colours (optional; "-": automatic).
  const auto colour = [](const QString& text) {
    return text.isEmpty() || text == QLatin1String("-") ? QColor() : QColor::fromString(text);
  };
  textColor = colour(s.section(' ',8,8));
  fillColor = colour(s.section(' ',9,9));

  // Its own notation (optional; none, or one there is not: the diagram's).
  n = s.section(' ',10,10);
  const int own = n.toInt(&ok);
  notation = ok && own >= 0 && own <= int(qucs_s::numberformat::Notation::Power) ? own : -1;

  // A delta marker's reference (optional): resolved by the diagram.
  n = s.section(' ',11,11);
  const int ref = n.toInt(&ok);
  pendingReference = ok && ref > 0 ? ref : 0;

  return true;
}

// ------------------------------------------------------------------------
// Checks if the coordinates x/y point to the marker text. x/y are relative
// to diagram cx/cy.
bool Marker::getSelected(int x_, int y_)
{
  if(x_ >= x1) if(x_ <= x1+x2) if(y_ >= y1) if(y_ <= y1+y2)
    return true;

  return false;
}

// ------------------------------------------------------------------------
/*
 * the diagram this belongs to
 */
const Diagram* Marker::diag() const
{
  if(!pGraph) return nullptr;
  return pGraph->parentDiagram();
}

// ------------------------------------------------------------------------
Marker* Marker::sameNewOne(Graph *pGraph_)
{
  Marker *pm = new Marker(pGraph_, 0, cx ,cy);

  pm->x1 = x1;  pm->y1 = y1;
  pm->x2 = x2;  pm->y2 = y2;

  pm->VarPos = VarPos;

  pm->Text          = Text;
  pm->transparent   = transparent;
  pm->Precision     = Precision;
  pm->numMode       = numMode;
  pm->indicatorMode = indicatorMode;
  pm->notation      = notation;
  pm->textColor     = textColor;
  pm->fillColor     = fillColor;
  pm->a_reference   = a_reference;   // (a copy of the diagram points it at its own: Diagram)

  return pm;
}

const Marker* Marker::reference() const
{
  if (a_reference == nullptr || a_reference == this || !diag()) return nullptr;
  // Still one of its diagram's.
  for (const Marker* m : diag()->markers())
    if (m == a_reference) return a_reference;
  return nullptr;
}

double Marker::shownValue() const
{
  if (!pGraph || pGraph->isEmpty() || VarPos.empty()) return 0.0;
  std::vector<double> at = VarPos;   // (findSample takes it to change)
  const auto p = pGraph->findSample(at);
  const double re = p.first, im = p.second;
  double v = std::fabs(im) > 1e-250 ? std::sqrt(re * re + im * im) : re;
  if (const Axis* a = diag() ? diag()->graphAxis(pGraph) : nullptr; a && a->log && a->Units != Axis::NoUnits)
    v = qucs::num2db(std::fabs(v), a->Units);
  return v;
}

void Marker::refreshDependents()
{
  if (!diag()) return;
  for (Marker* m : diag()->markers())
    if (m != this && m->reference() == this) m->createText();
}


QRect Marker::boundingRect() const noexcept
{
  // Where paint() draws it, on the schematic: its label, with the frame
  // drawn round it when it is selected, and the point it marks (y upwards
  // from the diagram's lower left corner) with its indicator.
  const QPoint origin = diag() ? QPoint{diag()->cx, diag()->cy} : QPoint{};
  const QRect label = QRect{x1, y1, x2, y2}.normalized().marginsAdded({5, 5, 5, 5});
  const QRect root = QRect{0, 0, IND_SIZE + 2, IND_SIZE + 2}.translated(cx - IND_SIZE / 2 - 1, -cy - IND_SIZE / 2 - 1);
  return label.united(root).translated(origin);
}


bool Marker::moveCenter(int dx, int dy) noexcept
{
  // Members cx and cy store coordinates of root of the marker.
  // Members x1 and y1 store coordinates of marker text
  x1 += dx;
  y1 += dy;
  return dx != 0 || dy != 0;
}


bool Marker::rotate() noexcept
{
  qucs_s::geom::rotate_point_ccw(x1, y1, cx, cy);
  return true;
}


bool Marker::mirrorX() noexcept
{
  return moveCenterTo(
    center().x(),
    qucs_s::geom::mirror_coordinate(center().y(), cy));
}


bool Marker::mirrorY() noexcept
{
  return moveCenterTo(
    qucs_s::geom::mirror_coordinate(center().x(), cx),
    center().y());
}

// vim:ts=8:sw=2:noet
