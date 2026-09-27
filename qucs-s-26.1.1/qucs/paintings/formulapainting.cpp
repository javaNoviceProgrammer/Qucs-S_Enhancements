/*
 * formulapainting.cpp - TeX math typeset on the schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "formulapainting.h"

#include "embeddedimage.h"
#include "geometry/geometry.h"
#include "ink.h"
#include "main.h"
#include "mathtypeset.h"
#include "misc.h"
#include "schematic.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPaintDevice>
#include <QPixmap>

#include <algorithm>
#include <cmath>

using qucs_s::paintings::decodeText;
using qucs_s::paintings::encodeText;
using qucs_s::paintings::textFont;

namespace {

QString tr(const char* text)
{
  return QCoreApplication::translate("ShapePainting", text);
}

} // namespace

FormulaPainting::FormulaPainting()
    : m_tex(QStringLiteral("f_c = \\frac{1}{2\\pi R C}")), m_size(std::max(6, QucsSettings.font.pointSize() + 2))
{
  Name = QStringLiteral("Formula ");
  isSelected = false;
  updateBounds();
}

Element* FormulaPainting::info(QString& name, char*& bitmap, bool getNewOne)
{
  name = QObject::tr("Formula");
  bitmap = (char*)"formula";
  return getNewOne ? new FormulaPainting() : nullptr;
}

QImage FormulaPainting::image(const QColor& colour, qreal pixelRatio) const
{
  const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(m_tex).arg(m_size).arg(colour.name(QColor::HexArgb)).arg(m_display).arg(pixelRatio);
  if (key != m_cacheKey) {
    m_cacheKey = key;
    m_cache = m_tex.trimmed().isEmpty()
                  ? QImage()
                  : qucs_s::math::typeset(m_tex, textFont(m_size), colour, m_display, pixelRatio).image;
  }
  return m_cache;
}

QSizeF FormulaPainting::formulaSize() const
{
  const QString key = QStringLiteral("%1|%2|%3").arg(m_tex).arg(m_size).arg(m_display);
  if (key == m_sizeKey) return m_sizeCache;
  m_sizeKey = key;
  // Measured, not drawn: the size the image at 1.0 would have.
  const qucs_s::math::Typeset one =
      qucs_s::math::measure(m_tex.trimmed().isEmpty() ? QStringLiteral("?") : m_tex, textFont(m_size), m_display);
  m_sizeCache = QSizeF(std::max(1.0, std::ceil(one.width)), std::max(1.0, std::ceil(one.ascent + one.descent)));
  return m_sizeCache;
}

qreal FormulaPainting::cappedRatio(qreal wanted) const
{
  // At most MaxPixels (64 MB) and MaxSide pixels a side, however big the
  // formula and near the zoom: a size-400 formula at 8 pixels a unit was
  // an image of 4.6 GB. A big formula zoomed in is drawn from fewer
  // pixels than the screen has - a little soft, not out of memory.
  constexpr qreal MaxPixels = 16.0 * 1024 * 1024, MaxSide = 8192.0;
  const QSizeF size = formulaSize();
  const qreal fits = std::min({std::sqrt(MaxPixels / std::max(1.0, size.width() * size.height())),
                               MaxSide / std::max(1.0, size.width()), MaxSide / std::max(1.0, size.height())});
  return std::max(0.05, std::min(wanted, fits));
}

QTransform FormulaPainting::transform() const
{
  QTransform t;
  t.translate(m_at.x(), m_at.y());
  t.rotate(-m_angle);
  return t;
}

void FormulaPainting::updateBounds() noexcept
{
  const QRect b = transform().mapRect(QRectF(QPointF(0, 0), formulaSize())).toAlignedRect();
  x1 = b.left();
  y1 = b.top();
  x2 = b.right();
  y2 = b.bottom();
  updateCenter();
}

QRect FormulaPainting::boundingRect() const noexcept
{
  return QRect(QPoint(x1, y1), QPoint(x2, y2));
}

void FormulaPainting::paint(QPainter* painter)
{
  // As many pixels as the zoom and the screen give a unit.
  const QTransform& world = painter->worldTransform();
  const qreal zoom = std::sqrt(std::abs(world.determinant()));
  const qreal device = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
  const qreal ratio = cappedRatio(std::clamp(std::ceil(zoom * device * 2) / 2, 1.0, 8.0));
  const QImage picture = image(qucs_s::ink::on(m_colour), ratio);
  const QSizeF size = formulaSize();

  painter->save();
  painter->setTransform(transform(), true);
  painter->setRenderHint(QPainter::SmoothPixmapTransform);
  if (!picture.isNull()) {
    painter->drawImage(QRectF(QPointF(0, 0), size), picture);
  } else {
    painter->setPen(qucs_s::ink::on(QColor(Qt::gray)));
    painter->drawRect(QRectF(QPointF(0, 0), size));
  }
  if (isSelected) {
    painter->setBrush(Qt::NoBrush);
    painter->setPen(qucs_s::ink::on(QPen(Qt::darkGray, 3)));
    painter->drawRect(QRectF(QPointF(0, 0), size).adjusted(-2, -2, 2, 2));
  }
  painter->restore();
  if (isSelected) misc::draw_resize_handle(painter, QPoint(x2, y2));
}

void FormulaPainting::paintScheme(Schematic* sch)
{
  sch->PostPaintEvent(_Rect, x1, y1, x2 - x1, y2 - y1);
}

// "Formula x y size colour angle display ~tex"
bool FormulaPainting::load(const QString& s)
{
  const QStringList f = s.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (f.size() < 8 || !f.at(7).startsWith(QLatin1Char('~'))) return false;
  bool ok = true;
  const auto number = [&](int i) {
    bool good = false;
    const int n = f.at(i).toInt(&good);
    ok = ok && good;
    return n;
  };
  m_at = QPoint(misc::clampCoordinate(number(1)), misc::clampCoordinate(number(2)));
  m_size = std::clamp(number(3), 2, 400);
  const QColor colour = misc::ColorFromString(f.at(4));
  if (!colour.isValid()) return false;
  m_colour = colour;
  m_angle = ((number(5) / 90) % 4 + 4) % 4 * 90;
  m_display = number(6) != 0;
  m_tex = decodeText(f.at(7).mid(1));
  updateBounds();
  return ok;
}

QString FormulaPainting::save()
{
  return QStringList{Name.trimmed(),       QString::number(m_at.x()), QString::number(m_at.y()),
                     QString::number(m_size), m_colour.name(),         QString::number(m_angle),
                     m_display ? QStringLiteral("1") : QStringLiteral("0"), QLatin1Char('~') + encodeText(m_tex)}
      .join(QLatin1Char(' '));
}

QString FormulaPainting::saveCpp()
{
  return QString();   // (an image: nothing to draw in C++)
}

QString FormulaPainting::saveJSON()
{
  return QString();
}

bool FormulaPainting::getSelected(const QPoint& click, int tolerance)
{
  return boundingRect().marginsAdded(QMargins(tolerance, tolerance, tolerance, tolerance)).contains(click);
}

bool FormulaPainting::resizeTouched(const QPoint& click, int tolerance)
{
  m_dragged = qucs_s::geom::distance(QPoint(x2, y2), click) < tolerance;
  return m_dragged;
}

// Scaled: its size follows the handle.
void FormulaPainting::MouseResizeMoving(int x, int y, Schematic* sch)
{
  if (!m_dragged) return;
  const qreal wide = std::max(1, x2 - x1), tall = std::max(1, y2 - y1);
  const qreal ratio = std::max((x - x1) / wide, (y - y1) / tall);
  if (ratio <= 0) return;
  const int size = std::clamp(qRound(m_size * ratio), 4, 200);
  if (size == m_size) return;
  m_size = size;
  updateBounds();
  paintScheme(sch);
}

void FormulaPainting::MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& cursor)
{
  m_at = onGrid;
  updateBounds();
  // The cursor: a sigma.
  sch->PostPaintEvent(_Line, cursor.x() + 24, cursor.y(), cursor.x() + 14, cursor.y(), 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 14, cursor.y(), cursor.x() + 20, cursor.y() + 7, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 20, cursor.y() + 7, cursor.x() + 14, cursor.y() + 14, 0, 0, true);
  sch->PostPaintEvent(_Line, cursor.x() + 14, cursor.y() + 14, cursor.x() + 24, cursor.y() + 14, 0, 0, true);
}

// Placed once it is written (its dialog comes up at the click).
bool FormulaPainting::MousePressing(Schematic* sch)
{
  bool accepted = false;
  qucs_s::paintings::editFields(this, tr("Write the Formula"), sch, &accepted);
  updateBounds();
  return accepted && !m_tex.trimmed().isEmpty();
}

void FormulaPainting::afterMove(int dx, int dy) noexcept
{
  m_at += QPoint(dx, dy);
}

bool FormulaPainting::rotate() noexcept
{
  return rotate(cx, cy);
}

bool FormulaPainting::rotate(int xc, int yc) noexcept
{
  int x = m_at.x(), y = m_at.y();
  qucs_s::geom::rotate_point_ccw(x, y, xc, yc);
  m_at = QPoint(x, y);
  m_angle = (m_angle + 90) % 360;
  updateBounds();
  return true;
}

bool FormulaPainting::symbolPrimitives(SymbolPrimitives& into) const
{
  QImage picture = image(m_colour, cappedRatio(4.0));
  if (picture.isNull()) return true;
  if (m_angle != 0) picture = picture.transformed(QTransform().rotate(-m_angle), Qt::SmoothTransformation);
  qucs_s::EmbeddedImage embedded;
  if (!embedded.loadPixmap(QPixmap::fromImage(picture))) return true;
  const QRect b = boundingRect();
  into.images << new qucs::Image(b.left(), b.top(), b.width() - 1, b.height() - 1, embedded);
  return true;
}

QList<PaintingField> FormulaPainting::fields() const
{
  QList<PaintingField> f;
  const QString formula = tr("Formula");
  f << PaintingField{QStringLiteral("tex"), tr("TeX:"), PaintingField::LongText, m_tex, 0, 0, 0, {}, formula,
                     tr("TeX math, without dollars: \\frac{a}{b}, \\sqrt{x}, x^2, x_i, \\sum_{k=1}^{n}, "
                        "\\int_a^b, \\alpha, \\Omega, \\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}")};
  f << PaintingField{QStringLiteral("size"), tr("Size:"), PaintingField::Int, m_size, 4, 200, 0, {}, formula};
  f << PaintingField{QStringLiteral("colour"), tr("Colour:"), PaintingField::Colour, QVariant::fromValue(m_colour), 0, 0, 0, {}, formula};
  f << PaintingField{QStringLiteral("display"), tr("display style (larger fractions, limits above and below)"),
                     PaintingField::Check, m_display, 0, 0, 0, {}, formula};
  return f;
}

void FormulaPainting::setField(const QString& key, const QVariant& value)
{
  if (key == QLatin1String("tex")) m_tex = value.toString();
  else if (key == QLatin1String("size")) m_size = std::clamp(value.toInt(), 2, 400);
  else if (key == QLatin1String("colour")) m_colour = value.value<QColor>();
  else if (key == QLatin1String("display")) m_display = value.toBool();
  updateBounds();
}

bool FormulaPainting::Dialog(QWidget* parent)
{
  const bool changed = qucs_s::paintings::editFields(this, tr("Edit Formula"), parent);
  updateBounds();
  return changed;
}
