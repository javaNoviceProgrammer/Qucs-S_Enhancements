/*
 * shapes.cpp - paintings drawn in a box
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "shapes.h"

#include "ink.h"
#include "main.h"
#include "misc.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextLayout>
#include <QTextOption>
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

int toInt(const QStringList& f, int i, int fallback, bool* ok)
{
  if (i >= f.size()) {
    *ok = false;
    return fallback;
  }
  bool good = false;
  const int n = f.at(i).toInt(&good);
  if (!good) *ok = false;
  return good ? n : fallback;
}

// A text's primitives: the lines \a text wraps into in \a width, placed in
// \a rect of the frame by \a align (0 left, 1 centre, 2 right) and \a valign.
void textPrimitives(SymbolPrimitives& into, const QTransform& frame, const QString& text, const QFont& font,
                    const QColor& colour, const QRectF& rect, int align, int valign, int angle)
{
  QTextOption option;
  option.setWrapMode(QTextOption::WordWrap);
  struct Line {
    QString text;
    qreal width, top;
  };
  QList<Line> lines;
  qreal height = 0;
  const QFontMetricsF metrics(font);
  for (const QString& paragraph : text.split(QLatin1Char('\n'))) {
    QTextLayout layout(paragraph, font);
    layout.setTextOption(option);
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
      line.setLineWidth(rect.width());
      lines << Line{paragraph.mid(line.textStart(), line.textLength()).trimmed(), line.naturalTextWidth(), height};
      height += metrics.lineSpacing();
    }
    layout.endLayout();
    if (paragraph.isEmpty()) height += metrics.lineSpacing();
  }
  const qreal top = valign == 0 ? rect.top() : valign == 1 ? rect.center().y() - height / 2 : rect.bottom() - height;
  const double radians = qDegreesToRadians(double(angle));
  for (const Line& l : lines) {
    if (l.text.isEmpty()) continue;
    const qreal left = align == 0 ? rect.left() : align == 1 ? rect.center().x() - l.width / 2 : rect.right() - l.width;
    const QPointF at = frame.map(QPointF(left, top + l.top));
    into.texts << new Text(at.x(), at.y(), l.text, colour, font.pixelSize(), std::cos(radians), std::sin(radians));
  }
}

} // namespace

// ----------------------------------------------------------------------
RoundedRectangle::RoundedRectangle(bool filled) : ShapePainting(QStringLiteral("RoundRect"), filled) {}

Element* RoundedRectangle::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Rounded Rectangle");
  bitmap = (char*)"roundrect";
  return getNewOne ? new RoundedRectangle(false) : nullptr;
}

Element* RoundedRectangle::info_filled(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("filled Rounded Rectangle");
  bitmap = (char*)"filledroundrect";
  return getNewOne ? new RoundedRectangle(true) : nullptr;
}

QPainterPath RoundedRectangle::outline(qreal w, qreal h) const
{
  const qreal r = std::min<qreal>(m_radius, std::min(w, h) / 2);
  QPainterPath path;
  path.addRoundedRect(QRectF(0, 0, w, h), r, r);
  return path;
}

QStringList RoundedRectangle::saveExtra() const
{
  return {QString::number(m_radius)};
}

bool RoundedRectangle::loadExtra(const QStringList& f)
{
  bool ok = true;
  m_radius = std::clamp(toInt(f, 0, 10, &ok), 0, 1000);
  return ok;
}

QString RoundedRectangle::dialogTitle() const
{
  return tr("Edit Rounded Rectangle Properties");
}

QList<PaintingField> RoundedRectangle::fields() const
{
  QList<PaintingField> f = ShapePainting::fields();
  f.prepend(PaintingField{QStringLiteral("radius"), tr("Corner radius:"), PaintingField::Int, m_radius, 0, 500, 0, {}, tr("Shape")});
  return f;
}

void RoundedRectangle::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("radius")) m_radius = std::clamp(value.toInt(), 0, 1000);
  else ShapePainting::setField(key, value);
}

// ----------------------------------------------------------------------
RegularPolygon::RegularPolygon(Kind kind) : ShapePainting(QStringLiteral("RegPolygon"), false), m_kind(kind)
{
  switch (kind) {
  case Kind::Triangle:
    m_sides = 3;
    break;
  case Kind::Polygon:
    m_sides = 6;
    break;
  case Kind::Star:
    m_sides = 5;
    m_star = true;
    m_turn = -90;   // a point up
    m_filled = true;
    m_brush = QBrush(QColor(0xf2, 0xc2, 0x3a), Qt::SolidPattern);
    break;
  }
}

Element* RegularPolygon::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Triangle");
  bitmap = (char*)"triangle";
  return getNewOne ? new RegularPolygon(Kind::Triangle) : nullptr;
}

Element* RegularPolygon::info_polygon(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Regular Polygon");
  bitmap = (char*)"regpolygon";
  return getNewOne ? new RegularPolygon(Kind::Polygon) : nullptr;
}

Element* RegularPolygon::info_star(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Star");
  bitmap = (char*)"star";
  return getNewOne ? new RegularPolygon(Kind::Star) : nullptr;
}

QPainterPath RegularPolygon::outline(qreal w, qreal h) const
{
  const int corners = m_star ? 2 * m_sides : m_sides;
  QPolygonF unit;
  for (int k = 0; k < corners; ++k) {
    const double a = qDegreesToRadians(m_turn + k * 360.0 / corners);   // (clockwise on the screen)
    const double r = m_star && k % 2 == 1 ? m_inner / 100.0 : 1.0;
    unit << QPointF(r * std::cos(a), r * std::sin(a));
  }
  // Fitted to the box.
  const QRectF b = unit.boundingRect();
  QTransform fit;
  fit.scale(b.width() > 0 ? w / b.width() : 1, b.height() > 0 ? h / b.height() : 1);
  fit.translate(-b.left(), -b.top());
  QPainterPath path;
  path.addPolygon(fit.map(unit));
  path.closeSubpath();
  return path;
}

QStringList RegularPolygon::saveExtra() const
{
  return {QString::number(int(m_kind)), QString::number(m_sides), m_star ? QStringLiteral("1") : QStringLiteral("0"),
          QString::number(m_inner), QString::number(m_turn)};
}

bool RegularPolygon::loadExtra(const QStringList& f)
{
  bool ok = true;
  m_kind = Kind(std::clamp(toInt(f, 0, 1, &ok), 0, 2));
  m_sides = std::clamp(toInt(f, 1, 6, &ok), 3, 64);
  m_star = toInt(f, 2, 0, &ok) != 0;
  m_inner = std::clamp(toInt(f, 3, 45, &ok), 5, 95);
  m_turn = std::clamp(toInt(f, 4, 0, &ok), -360, 360);
  return ok;
}

QString RegularPolygon::dialogTitle() const
{
  return m_star ? tr("Edit Star Properties") : tr("Edit Polygon Properties");
}

QList<PaintingField> RegularPolygon::fields() const
{
  QList<PaintingField> f;
  const QString shape = tr("Shape");
  f << PaintingField{QStringLiteral("sides"), m_star ? tr("Points:") : tr("Sides:"), PaintingField::Int, m_sides, 3, 32, 0, {}, shape};
  f << PaintingField{QStringLiteral("star"), tr("a star"), PaintingField::Check, m_star, 0, 0, 0, {}, shape};
  PaintingField inner{QStringLiteral("inner"), tr("Inner radius (%):"), PaintingField::Int, m_inner, 5, 95, 0, {}, shape,
                      tr("How far in the star's inner corners are, as a part of the outer ones")};
  inner.enabledBy = QStringLiteral("star");
  f << inner;
  f << PaintingField{QStringLiteral("turn"), tr("First corner at (°):"), PaintingField::Int, m_turn, -180, 180, 0, {}, shape,
                     tr("0: the first corner points right; -90: up")};
  return f + ShapePainting::fields();
}

void RegularPolygon::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("sides")) m_sides = std::clamp(value.toInt(), 3, 64);
  else if (key == QLatin1String("star")) m_star = value.toBool();
  else if (key == QLatin1String("inner")) m_inner = std::clamp(value.toInt(), 5, 95);
  else if (key == QLatin1String("turn")) m_turn = std::clamp(value.toInt(), -360, 360);
  else ShapePainting::setField(key, value);
}

// ----------------------------------------------------------------------
BracePainting::BracePainting() : ShapePainting(QStringLiteral("Brace"), false) {}

Element* BracePainting::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Brace");
  bitmap = (char*)"brace";
  return getNewOne ? new BracePainting() : nullptr;
}

QPainterPath BracePainting::outline(qreal w, qreal h) const
{
  QPainterPath p;
  switch (m_kind) {
  case Kind::Curly: {
    const qreal m = w / 2, arm = std::min(h * 0.1, w * 1.5);
    p.moveTo(w, 0);
    p.cubicTo(m, 0, m, 0, m, arm);                        // the top turns down
    p.lineTo(m, h / 2 - arm);
    p.cubicTo(m, h / 2 - arm / 3, m * 0.6, h / 2, 0, h / 2);   // into the point
    p.cubicTo(m * 0.6, h / 2, m, h / 2 + arm / 3, m, h / 2 + arm);
    p.lineTo(m, h - arm);
    p.cubicTo(m, h, m, h, w, h);
    break;
  }
  case Kind::Square:
    p.moveTo(w, 0);
    p.lineTo(0, 0);
    p.lineTo(0, h);
    p.lineTo(w, h);
    break;
  case Kind::Round:
    p.moveTo(w, 0);
    p.cubicTo(-w / 3, h * 0.25, -w / 3, h * 0.75, w, h);   // (its leftmost at x = 0)
    break;
  }
  return p;
}

QStringList BracePainting::saveExtra() const
{
  return {QString::number(int(m_kind))};
}

bool BracePainting::loadExtra(const QStringList& f)
{
  bool ok = true;
  m_kind = Kind(std::clamp(toInt(f, 0, 0, &ok), 0, 2));
  return ok;
}

QString BracePainting::dialogTitle() const
{
  return tr("Edit Brace Properties");
}

QList<PaintingField> BracePainting::fields() const
{
  QList<PaintingField> f;
  f << PaintingField{QStringLiteral("kind"), tr("Kind:"), PaintingField::Choice, int(m_kind), 0, 0, 0,
                     {tr("curly brace"), tr("square bracket"), tr("parenthesis")}, tr("Shape")};
  return f + ShapePainting::fields();
}

void BracePainting::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("kind")) m_kind = Kind(std::clamp(value.toInt(), 0, 2));
  else ShapePainting::setField(key, value);
}

// ----------------------------------------------------------------------
WaveformPainting::WaveformPainting() : ShapePainting(QStringLiteral("Waveform"), false)
{
  m_pen.setWidth(2);
}

Element* WaveformPainting::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Waveform");
  bitmap = (char*)"waveform";
  return getNewOne ? new WaveformPainting() : nullptr;
}

namespace {

// The level (-1 to 1, 1 up) of \a shape at \a phase (in cycles); at a
// jump, the level after it when \a after.
double level(WaveformPainting::Shape shape, double phase, double duty, bool after)
{
  using Shape = WaveformPainting::Shape;
  double f = phase - std::floor(phase);
  if (!after && f == 0.0 && phase > 0) f = 1.0;   // (the end of the cycle before)
  switch (shape) {
  case Shape::Square: return (after ? f < 0.5 : f <= 0.5) && f < 1.0 ? 1.0 : -1.0;
  case Shape::Triangle: return f <= 0.25 ? 4 * f : f <= 0.75 ? 2 - 4 * f : 4 * f - 4;
  case Shape::Sawtooth: return f >= 1.0 ? 1.0 : -1 + 2 * f;
  case Shape::Pulse: {
    const double on = (1 - duty) / 2, off = (1 + duty) / 2;
    return (after ? f >= on && f < off : f > on && f <= off) ? 1.0 : -1.0;
  }
  default: return std::sin(2 * M_PI * phase);
  }
}

} // namespace

QPainterPath WaveformPainting::outline(qreal w, qreal h) const
{
  QPainterPath p;
  const qreal mid = h / 2, amplitude = h / 2;
  const double cycles = std::max(0.05, m_cycles);
  const double duty = m_duty / 100.0;
  const auto y = [&](double lv) { return mid - amplitude * lv; };
  if (m_shape == Shape::Sine || m_shape == Shape::DampedSine) {
    const int n = std::max(24, int(cycles * 48));
    for (int i = 0; i <= n; ++i) {
      const double t = double(i) / n;
      double lv = std::sin(2 * M_PI * cycles * t);
      if (m_shape == Shape::DampedSine) lv *= std::exp(-4.0 * t);
      const QPointF at(w * t, y(lv));
      if (i == 0) p.moveTo(at);
      else p.lineTo(at);
    }
  } else {
    // The corners of each cycle, a jump as two points at one x.
    QList<double> corners;
    switch (m_shape) {
    case Shape::Square: corners = {0.0, 0.5}; break;
    case Shape::Triangle: corners = {0.25, 0.75}; break;
    case Shape::Sawtooth: corners = {0.0}; break;
    default: corners = {(1 - duty) / 2, (1 + duty) / 2}; break;
    }
    p.moveTo(0, y(level(m_shape, 0, duty, true)));
    for (int k = 0; k <= int(std::ceil(cycles)); ++k)
      for (double c : corners) {
        const double phase = k + c;
        if (phase <= 0 || phase >= cycles) continue;
        const qreal x = w * phase / cycles;
        p.lineTo(x, y(level(m_shape, phase, duty, false)));
        const double after = level(m_shape, phase, duty, true);
        if (after != level(m_shape, phase, duty, false)) p.lineTo(x, y(after));
      }
    p.lineTo(w, y(level(m_shape, cycles, duty, false)));
  }
  if (m_baseline) {
    p.moveTo(0, mid);
    p.lineTo(w, mid);
  }
  return p;
}

QStringList WaveformPainting::saveExtra() const
{
  return {QString::number(int(m_shape)), QString::number(m_cycles, 'g', 6), QString::number(m_duty),
          m_baseline ? QStringLiteral("1") : QStringLiteral("0")};
}

bool WaveformPainting::loadExtra(const QStringList& f)
{
  bool ok = true;
  m_shape = Shape(std::clamp(toInt(f, 0, 0, &ok), 0, 5));
  bool good = f.size() > 1;
  const double cycles = good ? f.at(1).toDouble(&good) : 2.0;
  if (!good) ok = false;
  // (toDouble() takes "nan" and "inf": clamped, NaN stays NaN - nothing
  // drawn, and saved back so. Not a number of cycles: the default.)
  m_cycles = std::clamp(good && std::isfinite(cycles) ? cycles : 2.0, 0.05, 100.0);
  m_duty = std::clamp(toInt(f, 2, 25, &ok), 1, 99);
  m_baseline = toInt(f, 3, 0, &ok) != 0;
  return ok;
}

QString WaveformPainting::dialogTitle() const
{
  return tr("Edit Waveform Properties");
}

QList<PaintingField> WaveformPainting::fields() const
{
  QList<PaintingField> f;
  const QString shape = tr("Waveform");
  f << PaintingField{QStringLiteral("shape"), tr("Shape:"), PaintingField::Choice, int(m_shape), 0, 0, 0,
                     {tr("sine"), tr("square"), tr("triangle"), tr("sawtooth"), tr("pulse"), tr("damped sine")}, shape};
  f << PaintingField{QStringLiteral("cycles"), tr("Cycles:"), PaintingField::Real, m_cycles, 0.25, 50, 2, {}, shape};
  f << PaintingField{QStringLiteral("duty"), tr("Pulse width (%):"), PaintingField::Int, m_duty, 1, 99, 0, {}, shape,
                     tr("The part of each cycle a pulse is high")};
  f << PaintingField{QStringLiteral("baseline"), tr("a line through the middle"), PaintingField::Check, m_baseline, 0, 0, 0, {}, shape};
  return f + ShapePainting::fields();
}

void WaveformPainting::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("shape")) m_shape = Shape(std::clamp(value.toInt(), 0, 5));
  else if (key == QLatin1String("cycles")) {
    const double cycles = value.toDouble();
    m_cycles = std::clamp(std::isfinite(cycles) ? cycles : 2.0, 0.05, 100.0);
  }
  else if (key == QLatin1String("duty")) m_duty = std::clamp(value.toInt(), 1, 99);
  else if (key == QLatin1String("baseline")) m_baseline = value.toBool();
  else ShapePainting::setField(key, value);
}

// ----------------------------------------------------------------------
TextBoxPainting::TextBoxPainting(Kind kind)
    : ShapePainting(QStringLiteral("TextBox"), true), m_kind(kind), m_fontSize(QucsSettings.font.pointSize())
{
  if (m_fontSize <= 0) m_fontSize = 10;
  switch (kind) {
  case Kind::Block:
    m_text = tr("Block");
    m_brush = QBrush(QColor(0xff, 0xff, 0xff), Qt::SolidPattern);
    break;
  case Kind::Note:
    m_text = tr("Note");
    m_brush = QBrush(QColor(0xff, 0xf3, 0xb0), Qt::SolidPattern);
    m_pen.setColor(QColor(0xb8, 0x9a, 0x00));
    m_align = 0;
    m_valign = 0;
    m_radius = 3;
    break;
  case Kind::Callout:
    m_text = tr("Callout");
    m_brush = QBrush(QColor(0xe7, 0xf0, 0xff), Qt::SolidPattern);
    m_pen.setColor(QColor(0x2f, 0x6f, 0xca));
    m_align = 0;
    m_valign = 0;
    m_radius = 8;
    m_pointer = true;
    break;
  }
}

Element* TextBoxPainting::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Text Box");
  bitmap = (char*)"textbox";
  return getNewOne ? new TextBoxPainting(Kind::Block) : nullptr;
}

Element* TextBoxPainting::info_note(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Note");
  bitmap = (char*)"note";
  return getNewOne ? new TextBoxPainting(Kind::Note) : nullptr;
}

Element* TextBoxPainting::info_callout(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Callout");
  bitmap = (char*)"callout";
  return getNewOne ? new TextBoxPainting(Kind::Callout) : nullptr;
}

QSize TextBoxPainting::defaultSize() const
{
  return m_kind == Kind::Block ? QSize(80, 50) : QSize(140, 60);
}

QPainterPath TextBoxPainting::outline(qreal w, qreal h) const
{
  const qreal r = std::min<qreal>(m_radius, std::min(w, h) / 2);
  QPainterPath path;
  path.addRoundedRect(QRectF(0, 0, w, h), r, r);
  return path;
}

QPainterPath TextBoxPainting::extraWorldPath() const
{
  if (!m_pointer) return {};
  // A wedge from the middle of the box to the tip, as wide as a fifth of it.
  const QRect b = box();
  const QPointF centre((b.left() + b.right()) / 2.0, (b.top() + b.bottom()) / 2.0);
  const QPointF towards = QPointF(m_tip) - centre;
  const qreal length = std::hypot(towards.x(), towards.y());
  if (length < 1) return {};
  const QPointF across(-towards.y() / length, towards.x() / length);
  const qreal half = std::max<qreal>(4, std::min(b.width(), b.height()) * 0.18);
  QPainterPath wedge;
  wedge.moveTo(centre + across * half);
  wedge.lineTo(m_tip);
  wedge.lineTo(centre - across * half);
  wedge.closeSubpath();
  return wedge;
}

QList<QPoint> TextBoxPainting::extraHandles() const
{
  return m_pointer ? QList<QPoint>{m_tip} : QList<QPoint>{};
}

void TextBoxPainting::moveExtraHandle(int, const QPoint& to)
{
  m_tip = QPoint(misc::clampCoordinate(to.x()), misc::clampCoordinate(to.y()));
  m_tipPlaced = true;
}

void TextBoxPainting::mapExtraPoints(const std::function<QPoint(const QPoint&)>& map)
{
  m_tip = map(m_tip);
}

void TextBoxPainting::placed()
{
  if (m_pointer && !m_tipPlaced) {
    const QRect b = box();
    m_tip = QPoint(b.left() - 40, b.bottom() + 40);   // below left of it
    m_tipPlaced = true;
  }
}

QRectF TextBoxPainting::textRect(qreal w, qreal h) const
{
  return QRectF(m_padding, m_padding, std::max<qreal>(1, w - 2 * m_padding), std::max<qreal>(1, h - 2 * m_padding));
}

int TextBoxPainting::flags() const
{
  const int horizontal = m_align == 0 ? Qt::AlignLeft : m_align == 1 ? Qt::AlignHCenter : Qt::AlignRight;
  const int vertical = m_valign == 0 ? Qt::AlignTop : m_valign == 1 ? Qt::AlignVCenter : Qt::AlignBottom;
  return horizontal | vertical | Qt::TextWordWrap;
}

void TextBoxPainting::paintContent(QPainter* painter, qreal w, qreal h) const
{
  if (m_text.isEmpty()) return;
  painter->setFont(textFont(m_fontSize, m_bold));
  painter->setPen(qucs_s::ink::on(m_textColour));
  painter->drawText(textRect(w, h), flags(), m_text);
}

void TextBoxPainting::contentPrimitives(SymbolPrimitives& into, const QTransform& frame, qreal w, qreal h) const
{
  textPrimitives(into, frame, m_text, textFont(m_fontSize, m_bold), m_textColour, textRect(w, h), m_align, m_valign,
                 m_angle);
}

// "... radius padding textcolour size bold align valign pointer tipx tipy text"
QStringList TextBoxPainting::saveExtra() const
{
  return {QString::number(int(m_kind)),
          QString::number(m_radius),
          QString::number(m_padding),
          m_textColour.name(),
          QString::number(m_fontSize),
          m_bold ? QStringLiteral("1") : QStringLiteral("0"),
          QString::number(m_align),
          QString::number(m_valign),
          m_pointer ? QStringLiteral("1") : QStringLiteral("0"),
          QString::number(m_tip.x()),
          QString::number(m_tip.y()),
          QLatin1Char('~') + encodeText(m_text)};
}

bool TextBoxPainting::loadExtra(const QStringList& f)
{
  bool ok = true;
  m_kind = Kind(std::clamp(toInt(f, 0, 0, &ok), 0, 2));
  m_radius = std::clamp(toInt(f, 1, 6, &ok), 0, 1000);
  m_padding = std::clamp(toInt(f, 2, 6, &ok), 0, 200);
  const QColor colour = f.size() > 3 ? QColor(f.at(3)) : QColor();
  if (!colour.isValid()) ok = false;
  else m_textColour = colour;
  m_fontSize = std::clamp(toInt(f, 4, 10, &ok), 1, 400);
  m_bold = toInt(f, 5, 0, &ok) != 0;
  m_align = std::clamp(toInt(f, 6, 1, &ok), 0, 2);
  m_valign = std::clamp(toInt(f, 7, 1, &ok), 0, 2);
  m_pointer = toInt(f, 8, 0, &ok) != 0;
  // (Clamped as every other coordinate: turned or mirrored, a tip far out
  // overflowed int.)
  m_tip = QPoint(misc::clampCoordinate(toInt(f, 9, 0, &ok)), misc::clampCoordinate(toInt(f, 10, 0, &ok)));
  m_tipPlaced = true;
  if (f.size() > 11 && f.at(11).startsWith(QLatin1Char('~'))) m_text = decodeText(f.at(11).mid(1));
  else ok = false;
  return ok;
}

QString TextBoxPainting::dialogTitle() const
{
  switch (m_kind) {
  case Kind::Note: return tr("Edit Note");
  case Kind::Callout: return tr("Edit Callout");
  default: return tr("Edit Text Box");
  }
}

QList<PaintingField> TextBoxPainting::fields() const
{
  QList<PaintingField> f;
  const QString text = tr("Text");
  f << PaintingField{QStringLiteral("text"), tr("Text:"), PaintingField::LongText, m_text, 0, 0, 0, {}, text};
  f << PaintingField{QStringLiteral("fontSize"), tr("Size:"), PaintingField::Int, m_fontSize, 4, 96, 0, {}, text};
  f << PaintingField{QStringLiteral("textColour"), tr("Colour:"), PaintingField::Colour, QVariant::fromValue(m_textColour), 0, 0, 0, {}, text};
  f << PaintingField{QStringLiteral("bold"), tr("bold"), PaintingField::Check, m_bold, 0, 0, 0, {}, text};
  f << PaintingField{QStringLiteral("align"), tr("Across:"), PaintingField::Choice, m_align, 0, 0, 0,
                     {tr("left"), tr("centre"), tr("right")}, text};
  f << PaintingField{QStringLiteral("valign"), tr("Down:"), PaintingField::Choice, m_valign, 0, 0, 0,
                     {tr("top"), tr("middle"), tr("bottom")}, text};
  const QString box = tr("Box");
  f << PaintingField{QStringLiteral("radius"), tr("Corner radius:"), PaintingField::Int, m_radius, 0, 100, 0, {}, box};
  f << PaintingField{QStringLiteral("padding"), tr("Padding:"), PaintingField::Int, m_padding, 0, 50, 0, {}, box};
  f << PaintingField{QStringLiteral("pointer"), tr("a pointer (a callout)"), PaintingField::Check, m_pointer, 0, 0, 0, {}, box,
                     tr("A wedge to what it is about; drag its tip's handle")};
  return f + ShapePainting::fields();
}

void TextBoxPainting::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("text")) m_text = value.toString();
  else if (key == QLatin1String("fontSize")) m_fontSize = std::clamp(value.toInt(), 1, 400);
  else if (key == QLatin1String("textColour")) m_textColour = value.value<QColor>();
  else if (key == QLatin1String("bold")) m_bold = value.toBool();
  else if (key == QLatin1String("align")) m_align = std::clamp(value.toInt(), 0, 2);
  else if (key == QLatin1String("valign")) m_valign = std::clamp(value.toInt(), 0, 2);
  else if (key == QLatin1String("radius")) m_radius = std::clamp(value.toInt(), 0, 1000);
  else if (key == QLatin1String("padding")) m_padding = std::clamp(value.toInt(), 0, 200);
  else if (key == QLatin1String("pointer")) {
    m_pointer = value.toBool();
    placed();   // (a tip where it would be, the first time)
  } else ShapePainting::setField(key, value);
}

// ----------------------------------------------------------------------
TablePainting::TablePainting() : ShapePainting(QStringLiteral("Table"), true), m_fontSize(QucsSettings.font.pointSize())
{
  if (m_fontSize <= 0) m_fontSize = 10;
  m_brush = QBrush(QColor(0xff, 0xff, 0xff), Qt::SolidPattern);
  m_cells = {{tr("Part"), tr("Value"), tr("Note")}, {QStringLiteral("R1"), QStringLiteral("10k"), QString()},
             {QStringLiteral("C1"), QStringLiteral("100n"), QString()}};
}

Element* TablePainting::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Table");
  bitmap = (char*)"table";
  return getNewOne ? new TablePainting() : nullptr;
}

QSize TablePainting::defaultSize() const
{
  return QSize(m_columns * 70, rows() * 22);
}

QString TablePainting::cell(int row, int column) const
{
  return row >= 0 && row < m_cells.size() && column >= 0 && column < m_cells.at(row).size() ? m_cells.at(row).at(column)
                                                                                             : QString();
}

void TablePainting::resize(int rowCount, int columnCount)
{
  m_columns = std::clamp(columnCount, 1, 40);
  rowCount = std::clamp(rowCount, 1, 200);
  while (m_cells.size() > rowCount) m_cells.removeLast();
  while (m_cells.size() < rowCount) m_cells << QStringList();
  for (QStringList& row : m_cells) {
    while (row.size() > m_columns) row.removeLast();
    while (row.size() < m_columns) row << QString();
  }
}

QPainterPath TablePainting::outline(qreal w, qreal h) const
{
  QPainterPath p;
  p.addRect(QRectF(0, 0, w, h));
  const int n = rows();
  for (int r = 1; r < n; ++r) {
    p.moveTo(0, h * r / n);
    p.lineTo(w, h * r / n);
  }
  for (int c = 1; c < m_columns; ++c) {
    p.moveTo(w * c / m_columns, 0);
    p.lineTo(w * c / m_columns, h);
  }
  return p;
}

void TablePainting::paintUnder(QPainter* painter, qreal w, qreal h) const
{
  if (!m_header || rows() < 1) return;
  painter->fillRect(QRectF(0, 0, w, h / rows()), qucs_s::ink::on(m_headerColour));
}

void TablePainting::paintContent(QPainter* painter, qreal w, qreal h) const
{
  const int n = rows();
  if (n < 1) return;
  const qreal cellW = w / m_columns, cellH = h / n, pad = 4;
  painter->setPen(qucs_s::ink::on(m_textColour));
  for (int r = 0; r < n; ++r) {
    painter->setFont(textFont(m_fontSize, m_header && r == 0));
    for (int c = 0; c < m_columns; ++c) {
      const QString t = cell(r, c);
      if (t.isEmpty()) continue;
      const QRectF rect(c * cellW + pad, r * cellH, std::max<qreal>(1, cellW - 2 * pad), cellH);
      painter->drawText(rect, (m_align == 0 ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter | Qt::TextWordWrap, t);
    }
  }
}

void TablePainting::contentPrimitives(SymbolPrimitives& into, const QTransform& frame, qreal w, qreal h) const
{
  const int n = rows();
  if (n < 1) return;
  if (m_header) {
    // The header's band under the lines: first.
    std::vector<QPointF> band;
    for (const QPointF& p : {QPointF(0, 0), QPointF(w, 0), QPointF(w, h / n), QPointF(0, h / n)}) band.push_back(frame.map(p));
    auto* shade = new qucs::Polyline(band, QPen(Qt::NoPen), QBrush(m_headerColour, Qt::SolidPattern));
    shade->closed = true;
    into.polylines.prepend(shade);
  }
  const qreal cellW = w / m_columns, cellH = h / n, pad = 4;
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < m_columns; ++c) {
      const QString t = cell(r, c);
      if (t.isEmpty()) continue;
      textPrimitives(into, frame, t, textFont(m_fontSize, m_header && r == 0), m_textColour,
                     QRectF(c * cellW + pad, r * cellH, std::max<qreal>(1, cellW - 2 * pad), cellH), m_align, 1, m_angle);
    }
}

// "... rows columns header headercolour textcolour size align ~cell ~cell ..."
QStringList TablePainting::saveExtra() const
{
  QStringList f{QString::number(rows()),
                QString::number(m_columns),
                m_header ? QStringLiteral("1") : QStringLiteral("0"),
                m_headerColour.name(),
                m_textColour.name(),
                QString::number(m_fontSize),
                QString::number(m_align)};
  for (const QStringList& row : m_cells)
    for (const QString& c : row) f << QLatin1Char('~') + encodeText(c);
  return f;
}

bool TablePainting::loadExtra(const QStringList& f)
{
  bool ok = true;
  const int rowCount = std::clamp(toInt(f, 0, 1, &ok), 1, 200);
  const int columnCount = std::clamp(toInt(f, 1, 1, &ok), 1, 40);
  m_header = toInt(f, 2, 1, &ok) != 0;
  const QColor header = f.size() > 3 ? QColor(f.at(3)) : QColor();
  const QColor text = f.size() > 4 ? QColor(f.at(4)) : QColor();
  if (!header.isValid() || !text.isValid()) ok = false;
  if (header.isValid()) m_headerColour = header;
  if (text.isValid()) m_textColour = text;
  m_fontSize = std::clamp(toInt(f, 5, 10, &ok), 1, 400);
  m_align = std::clamp(toInt(f, 6, 0, &ok), 0, 1);
  m_cells.clear();
  resize(rowCount, columnCount);
  for (int r = 0; r < rowCount; ++r)
    for (int c = 0; c < columnCount; ++c) {
      const int i = 7 + r * columnCount + c;
      if (i >= f.size() || !f.at(i).startsWith(QLatin1Char('~'))) return false;
      m_cells[r][c] = decodeText(f.at(i).mid(1));
    }
  return ok;
}

QString TablePainting::dialogTitle() const
{
  return tr("Edit Table");
}

QList<PaintingField> TablePainting::fields() const
{
  QList<PaintingField> f;
  const QString table = tr("Table");
  f << PaintingField{QStringLiteral("rows"), tr("Rows:"), PaintingField::Int, rows(), 1, 50, 0, {}, table};
  f << PaintingField{QStringLiteral("columns"), tr("Columns:"), PaintingField::Int, m_columns, 1, 20, 0, {}, table};
  f << PaintingField{QStringLiteral("header"), tr("the first row is a header"), PaintingField::Check, m_header, 0, 0, 0, {}, table};
  PaintingField shade{QStringLiteral("headerColour"), tr("Header colour:"), PaintingField::Colour, QVariant::fromValue(m_headerColour),
                      0, 0, 0, {}, table};
  shade.enabledBy = QStringLiteral("header");
  f << shade;
  QVariantList cells;
  for (const QStringList& row : m_cells) cells << row;
  f << PaintingField{QStringLiteral("cells"), tr("Cells:"), PaintingField::Cells, cells, 0, 0, 0, {}, table};
  const QString text = tr("Text");
  f << PaintingField{QStringLiteral("fontSize"), tr("Size:"), PaintingField::Int, m_fontSize, 4, 96, 0, {}, text};
  f << PaintingField{QStringLiteral("textColour"), tr("Colour:"), PaintingField::Colour, QVariant::fromValue(m_textColour), 0, 0, 0, {}, text};
  f << PaintingField{QStringLiteral("align"), tr("Across:"), PaintingField::Choice, m_align, 0, 0, 0, {tr("left"), tr("centre")}, text};
  return f + ShapePainting::fields();
}

void TablePainting::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("rows")) resize(value.toInt(), m_columns);
  else if (key == QLatin1String("columns")) resize(rows(), value.toInt());
  else if (key == QLatin1String("header")) m_header = value.toBool();
  else if (key == QLatin1String("headerColour")) m_headerColour = value.value<QColor>();
  else if (key == QLatin1String("cells")) {
    const QVariantList list = value.toList();
    for (int r = 0; r < rows() && r < list.size(); ++r) {
      const QStringList row = list.at(r).toStringList();
      for (int c = 0; c < m_columns && c < row.size(); ++c) m_cells[r][c] = row.at(c);
    }
  } else if (key == QLatin1String("fontSize")) m_fontSize = std::clamp(value.toInt(), 1, 400);
  else if (key == QLatin1String("textColour")) m_textColour = value.value<QColor>();
  else if (key == QLatin1String("align")) m_align = std::clamp(value.toInt(), 0, 1);
  else ShapePainting::setField(key, value);
}
