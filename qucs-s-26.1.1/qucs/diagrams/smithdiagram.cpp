/***************************************************************************
                          smithdiagram.cpp  -  description
                             -------------------
    begin                : Sat Oct 18 2003
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
  \class SmithDiagram
  \brief The SmithDiagram class implements the Impedance and Admittance Smith diagram
*/

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <algorithm>
#include <cmath>
#include <float.h>
#if HAVE_IEEEFP_H
# include <ieeefp.h>
#endif

#include "smithdiagram.h"
#include <memory>
#include <QPainterPath>
#include <QPainter>
#include <QFontMetricsF>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include "ink.h"
#include "dataset.h"
#include "misc.h"
#include "main.h"
#include "../dialogs/matchdialog.h" // For r2z function
#include "qucs_assert.h"


SmithDiagram::SmithDiagram(int _cx, int _cy, bool ImpMode) : Diagram(_cx, _cy), circleFrequency(std::nan(""))
{
  x1 = 10;     // position of label text
  y1 = 2;
  x2 = 200;    // initial size of diagram
  y2 = 200;
  y3 = 0;
  x3 = 207;    // with some distance for right axes text
  if(ImpMode)  Name = "Smith";  // with impedance circles
  else  Name = "ySmith";        // with admittance circles

  Arcs.append(as(Part::Frame, new struct qucs::Arc(0, y2, x2, y2, 0, 16*360, QPen(Qt::black,0))));
//  calcDiagram();    // calculate circles for smith chart with |r|=1
}

SmithDiagram::~SmithDiagram()
{
}

// ------------------------------------------------------------
// calculate the screen coordinates for the graph data
void SmithDiagram::calcCoordinate(const double*, const double* yD, const double*,
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

// ------------------------------------------------------------
void SmithDiagram::calcLimits()
{
  int a;
  calcSmithAxisScale(&yAxis, a, a);
  yAxis.limit_min = 0.0;
  yAxis.step = double(a);
  yAxis.limit_max = yAxis.up;
}

// ------------------------------------------------------------
// calculate the circles and arcs of the smith chart
int SmithDiagram::calcDiagram()
{
  qDeleteAll(Lines);
  Lines.clear();
  qDeleteAll(Texts);
  Texts.clear();
  qDeleteAll(Arcs);
  Arcs.clear();

  x3 = x2 + 7;
  if(Name.at(0) == 'y')  createSmithChart(&yAxis, 6);
  else  createSmithChart(&yAxis);

  // outer most circle
  Arcs.append(as(Part::Frame, new qucs::Arc(0, x2, x2, x2, 0, 16*360, QPen(Qt::black,0))));

  // horizontal line Im(r)=0
  Lines.append(as(Part::Grid, new qucs::Line(0, x2>>1, x2, x2>>1, GridPen)));

  computeCircles();
  return 3;
}

// ------------------------------------------------------------
QList<Diagram::Part> SmithDiagram::themeParts() const
{
  QList<Part> parts = Diagram::themeParts();
  parts.removeAll(Part::RightAxis);
  return parts;
}

QPainterPath SmithDiagram::plotAreaShape() const
{
  QPainterPath path;
  path.addEllipse(QRectF(0, -x2, x2, x2));   // (the outer circle, as calcDiagram() draws it)
  return path;
}

// ------------------------------------------------------------
Diagram* SmithDiagram::newOne()
{
  return new SmithDiagram();
}

// ------------------------------------------------------------
Element* SmithDiagram::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("Smith Chart");
  BitmapFile = (char *) "smith";

  if(getNewOne)  return new SmithDiagram();
  return 0;
}

// ------------------------------------------------------------
Element* SmithDiagram::info_y(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("Admittance Smith");
  BitmapFile = (char *) "ysmith";

  if(getNewOne)  return new SmithDiagram(0, 0, false);
  return 0;
}

QString SmithDiagram::extraMarkerText(Marker const* m) const
{
  QUCS_ASSERT(m);
  Graph const* pGraph = m->graph();
  QUCS_ASSERT(pGraph);
  std::vector<double> const& Pos = m->varPos();
  unsigned nVarPos = pGraph->numAxes();
  QUCS_ASSERT(nVarPos == Pos.size());
  double Zr, Zi;
  double Z0 = m->Z0;

  Zr = m->powReal();
  Zi = m->powImag();

  MatchDialog::r2z(Zr, Zi, Z0);
  QString Var = pGraph->Var;

  // (In the marker's notation, as its other numbers.)
  if(Var.startsWith("S")) { // uuh, ooh hack.
    return "\n"+ Var.replace('S', 'Z')+": " +m->complexText(Zr, Zi, nM_Rect);
  }else{
    return "\nZ("+ Var+"): " +m->complexText(Zr, Zi, nM_Rect);
  }
}

// vim:ts=8:sw=2:noet

// ------------------------------------------------------------
// The circles of the two-port its traces are of.

QStringList SmithDiagram::circleKindNames()
{
  return {QStringLiteral("stability_in"), QStringLiteral("stability_out"), QStringLiteral("gain"), QStringLiteral("noise")};
}

namespace {

namespace ds = qucs_s::dataset;

// The dataset last read for the circles, read again when its file changed.
const ds::Dataset* circleData(const QString& file)
{
  static QString lastFile;
  static QDateTime lastModified;
  static std::unique_ptr<ds::Dataset> data;
  const QFileInfo info(file);
  if (!info.exists()) return nullptr;
  if (data && lastFile == file && info.lastModified() == lastModified) return data.get();
  auto fresh = std::make_unique<ds::Dataset>();
  QString error;
  if (!fresh->read(file, &error)) return nullptr;
  data = std::move(fresh);
  lastFile = file;
  lastModified = info.lastModified();
  return data.get();
}

} // namespace

void SmithDiagram::computeCircles()
{
  m_circles = CircleSet();
  if (circles.isEmpty()) return;
  if (Graphs.isEmpty() || dataSetFile().isEmpty()) {
    m_circles.error = QObject::tr("no trace: the S-parameters are read from the run of its first trace");
    return;
  }
  // The run: its dataset (the simulator's suffix as the first trace's).
  QString var = Graphs.first()->Var, tail;
  if (const int slash = int(var.indexOf(QLatin1Char('/'))); slash > 0) {
    tail = QLatin1Char('.') + var.left(slash);
    var = var.mid(slash + 1);
  }
  QString file = dataSetFile() + tail;
  if (const int colon = int(var.indexOf(QLatin1Char(':'))); colon > 0) {
    file = QFileInfo(dataSetFile()).absoluteDir().filePath(var.left(colon) + QStringLiteral(".dat") + tail);
    var = var.mid(colon + 1);
  }
  const ds::Dataset* data = circleData(file);
  if (data == nullptr) {
    m_circles.error = QObject::tr("%1 cannot be read: simulate").arg(QFileInfo(file).fileName());
    return;
  }
  // S-parameters as ngspice's (ac.s_2_1) or Qucsator's (S[2,1]) run
  // names them.
  const QString analysis = var.contains(QLatin1Char('.')) ? var.section(QLatin1Char('.'), 0, 0) : QStringLiteral("ac");
  const auto find = [&](const QStringList& names) -> const ds::Variable* {
    for (const QString& n : names)
      if (const ds::Variable* v = data->find(n)) return v;
    return nullptr;
  };
  const auto sOf = [&](int i, int j) {
    return find({QStringLiteral("%1.s_%2_%3").arg(analysis).arg(i).arg(j), QStringLiteral("%1.v(s_%2_%3)").arg(analysis).arg(i).arg(j),
                 QStringLiteral("ac.s_%1_%2").arg(i).arg(j), QStringLiteral("ac.v(s_%1_%2)").arg(i).arg(j),
                 QStringLiteral("S[%1,%2]").arg(i).arg(j), QStringLiteral("s[%1,%2]").arg(i).arg(j)});
  };
  const ds::Variable* s[4] = {sOf(1, 1), sOf(1, 2), sOf(2, 1), sOf(2, 2)};
  for (const ds::Variable* v : s)
    if (v == nullptr || v->dependencies.isEmpty()) {
      m_circles.error = QObject::tr("the run has no two-port's S-parameters (S11, S12, S21, S22: an S-parameter simulation of two ports)");
      return;
    }
  const ds::Variable* f = data->find(s[0]->dependencies.first());
  if (f == nullptr || f->size() == 0 || f->size() > s[0]->size()) {
    m_circles.error = QObject::tr("the S-parameters' frequencies cannot be read");
    return;
  }
  // The frequency: given (the nearest sample), else the middle of the
  // sweep. One outside it has no circles: they were drawn at its end,
  // without a word (bug hunt of 2026-10-08, B4).
  if (std::isfinite(circleFrequency)) {
    const auto [lo, hi] = std::minmax_element(f->re.cbegin(), f->re.cend());
    const double tolerance = 1e-9 * std::max(std::abs(*lo), std::abs(*hi));
    if (circleFrequency < *lo - tolerance || circleFrequency > *hi + tolerance) {
      m_circles.error = QObject::tr("%1 Hz is outside the sweep (%2 to %3 Hz)").arg(circleFrequency).arg(*lo).arg(*hi);
      return;
    }
  }
  int k = f->size() / 2;
  if (std::isfinite(circleFrequency))
    for (int i = 0; i < f->size(); ++i)
      if (std::abs(f->re.at(i) - circleFrequency) < std::abs(f->re.at(k) - circleFrequency)) k = i;
  m_circles.frequency = f->re.at(k);
  const auto value = [&](const ds::Variable* v) {
    return k < v->size() ? std::complex<double>(v->re.at(k), v->isComplex() ? v->im.at(k) : 0.0) : std::complex<double>();
  };
  m_circles.s = {value(s[0]), value(s[1]), value(s[2]), value(s[3])};
  // The noise parameters: ngspice's (an S-parameter run with noise:
  // NFmin in dB) or Qucsator's (Fmin a ratio).
  const ds::Variable* nfmin = find({QStringLiteral("%1.nfmin").arg(analysis), QStringLiteral("ac.nfmin")});
  const ds::Variable* fmin = nfmin ? nfmin : find({QStringLiteral("Fmin"), QStringLiteral("fmin")});
  const ds::Variable* sopt = find({QStringLiteral("%1.sopt").arg(analysis), QStringLiteral("ac.sopt"), QStringLiteral("Sopt"), QStringLiteral("sopt")});
  const ds::Variable* rn = find({QStringLiteral("%1.rn").arg(analysis), QStringLiteral("ac.rn"), QStringLiteral("Rn"), QStringLiteral("rn")});
  for (const CircleSpec& spec : std::as_const(circles)) {
    DrawnCircle d;
    d.spec = spec;
    switch (spec.kind) {
    case InputStability:
      d.circle = qucs_s::smith::inputStability(m_circles.s);
      d.label = QObject::tr("input stability");
      break;
    case OutputStability:
      d.circle = qucs_s::smith::outputStability(m_circles.s);
      d.label = QObject::tr("output stability");
      break;
    case Gain:
      d.circle = qucs_s::smith::availableGain(m_circles.s, spec.level);
      d.label = QObject::tr("Ga %1 dB").arg(spec.level);
      break;
    case Noise:
      if (fmin == nullptr || sopt == nullptr || rn == nullptr) {
        d.circle.ok = false;
        d.circle.why = QObject::tr("the run has no noise parameters (NFmin, SOpt, Rn: an S-parameter simulation with noise)");
      } else {
        // ngspice's NFmin in dB; Qucsator's Fmin a ratio (10 log10 for dB).
        const double fminDb = nfmin ? value(fmin).real() : 10 * std::log10(std::max(std::abs(value(fmin)), 1.0));
        d.circle = qucs_s::smith::noise(spec.level, fminDb, value(sopt), std::abs(value(rn)));
      }
      d.label = QObject::tr("NF %1 dB").arg(spec.level);
      break;
    }
    m_circles.drawn << d;
  }
}

QColor SmithDiagram::circleColour(int kind)
{
  switch (kind) {
  case InputStability: return QColor(200, 30, 30);
  case OutputStability: return QColor(150, 40, 170);
  case Gain: return QColor(30, 90, 200);
  default: return QColor(0, 130, 60);
  }
}

void SmithDiagram::createAxisLabels()
{
  Diagram::createAxisLabels();
  if (circles.isEmpty()) return;
  // Under the traces' names: the frequency the circles are of, and the
  // two-port's stability there (or why there are none).
  // (10.02 MHz: four digits, the prefix with its unit.)
  const double f = m_circles.frequency;
  const int exponent = f > 0 ? std::clamp(3 * int(std::floor(std::log10(f) / 3 + 1e-9)), 0, 12) : 0;
  static const char* const prefixes[] = {"", "k", "M", "G", "T"};
  const QString frequency = QStringLiteral("%1 %2Hz").arg(QString::number(f / std::pow(10.0, exponent), 'g', 4), QLatin1String(prefixes[exponent / 3]));
  const QString text = !m_circles.error.isEmpty()
      ? QObject::tr("No circles: %1").arg(m_circles.error)
      : QObject::tr("Circles at %1: K %2, \u03bc %3%4")
            .arg(frequency)
            .arg(m_circles.s.k(), 0, 'g', 3)
            .arg(m_circles.s.mu(), 0, 'g', 3)
            .arg(m_circles.s.k() > 1 && std::abs(m_circles.s.delta()) < 1 ? QObject::tr(" (unconditionally stable)")
                                                                         : QObject::tr(" (potentially unstable)"));
  const QFontMetrics metrics(QucsSettings.font, nullptr);
  const int w = metrics.boundingRect(text).width() >> 1;
  Texts.append(as(Part::XAxis, new Text((x2 >> 1) - w, Bounding_y1, text, Qt::black, 12.0)));
  Bounding_y1 -= metrics.lineSpacing();
  Bounding_x2 = std::max(Bounding_x2, w - (x2 >> 1));
  Bounding_x1 = std::max(Bounding_x1, w - (x2 >> 1));
}

void SmithDiagram::paintBehindGraphs(QPainter* painter)
{
  if (m_circles.drawn.isEmpty() || !(yAxis.up > 0)) return;
  // (In its coordinates, y up.) Clipped to the chart; each kind its colour.
  painter->save();
  QPainterPath chart;
  chart.addEllipse(QRectF(0, 0, x2, y2));
  painter->setClipPath(chart);
  painter->setRenderHint(QPainter::Antialiasing, true);
  const double scale = double(x2) / 2.0 / yAxis.up;
  for (const DrawnCircle& d : m_circles.drawn) {
    if (!d.circle.ok) continue;
    const QPointF centre(x2 / 2.0 + d.circle.centre.real() * scale, y2 / 2.0 + d.circle.centre.imag() * scale);
    const double r = d.circle.radius * scale;
    const QColor ink = circleColour(d.spec.kind);
    painter->setPen(QPen(qucs_s::ink::on(ink), 1.5, d.spec.kind <= OutputStability ? Qt::DashLine : Qt::SolidLine));
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(centre, r, r);
    // A stability circle's unstable side, faintly filled (inside, or the
    // chart outside it).
    if (d.spec.kind <= OutputStability) {
      QColor fill = ink;
      fill.setAlpha(28);
      QPainterPath unstable;
      if (d.circle.stableInside) {
        unstable.addEllipse(QRectF(0, 0, x2, y2));
        QPainterPath inside;
        inside.addEllipse(centre, r, r);
        unstable = unstable.subtracted(inside);
      } else {
        unstable.addEllipse(centre, r, r);
      }
      painter->fillPath(unstable, fill);
    }
    // Its label where it comes nearest the chart's middle (in the chart,
    // if any of it is), reading towards the middle, upright.
    const QPointF middle(x2 / 2.0, y2 / 2.0);
    const QLineF towards(centre, middle);
    const QPointF at = towards.length() > 1e-9 ? centre + (middle - centre) * (r / towards.length()) : centre + QPointF(0, r);
    if (QLineF(at, middle).length() > x2 / 2.0 + 0.5) continue;
    const QFontMetricsF fm(painter->font());
    painter->save();
    painter->translate(at);
    painter->scale(1, -1);
    painter->setPen(qucs_s::ink::on(ink));
    // (Upright here: y down.)
    const double tx = at.x() > middle.x() ? -4 - fm.horizontalAdvance(d.label) : 4;
    const double ty = at.y() > middle.y() ? fm.ascent() + 3 : -3;
    painter->drawText(QPointF(tx, ty), d.label);
    painter->restore();
  }
  painter->restore();
}

QString SmithDiagram::extraSaveFields() const
{
  if (circles.isEmpty()) return QString();
  QString s = QStringLiteral(" %1 %2").arg(std::isfinite(circleFrequency) ? QString::number(circleFrequency, 'g', 15) : QStringLiteral("-"))
                  .arg(circles.size());
  for (const CircleSpec& c : circles) s += QStringLiteral(" %1 %2").arg(c.kind).arg(c.level, 0, 'g', 15);
  return s;
}

void SmithDiagram::loadExtraFields(const QStringList& fields)
{
  circles.clear();
  bool ok = false;
  const double f = fields.value(0).toDouble(&ok);
  circleFrequency = ok && f > 0 ? f : std::nan("");
  const int n = std::clamp(fields.value(1).toInt(), 0, 32);
  for (int i = 0; i < n; ++i) {
    const int kind = fields.value(2 + 2 * i).toInt(&ok);
    if (!ok || kind < InputStability || kind > Noise) continue;
    circles << CircleSpec{kind, fields.value(3 + 2 * i).toDouble()};
  }
}

bool SmithDiagram::circlesFromText(const QString& text, QList<CircleSpec>* out, QString* error)
{
  out->clear();
  for (QString item : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QStringList f = item.simplified().split(QLatin1Char(' '));
    const QString kind = f.value(0).toLower();
    CircleSpec c;
    if (kind == QLatin1String("in")) c.kind = InputStability;
    else if (kind == QLatin1String("out")) c.kind = OutputStability;
    else if (kind == QLatin1String("gain") || kind == QLatin1String("noise")) {
      bool ok = false;
      c.kind = kind == QLatin1String("gain") ? Gain : Noise;
      c.level = f.value(1).toDouble(&ok);
      if (!ok || !std::isfinite(c.level)) {
        *error = QObject::tr("%1 needs its level in dB: %1 12").arg(kind);
        return false;
      }
    } else {
      *error = QObject::tr("%1 is no circle: in, out, gain <dB> or noise <dB>").arg(item.trimmed());
      return false;
    }
    *out << c;
  }
  return true;
}

QString SmithDiagram::circlesText(const QList<CircleSpec>& circles)
{
  QStringList items;
  for (const CircleSpec& c : circles)
    items << (c.kind == InputStability ? QStringLiteral("in") : c.kind == OutputStability ? QStringLiteral("out")
              : QStringLiteral("%1 %2").arg(c.kind == Gain ? QStringLiteral("gain") : QStringLiteral("noise")).arg(c.level));
  return items.join(QStringLiteral(", "));
}
