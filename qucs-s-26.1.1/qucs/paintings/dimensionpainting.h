/*
 * dimensionpainting.h - a dimension: the distance between two points,
 *                       with its arrows, extension lines and length
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef DIMENSIONPAINTING_H
#define DIMENSIONPAINTING_H

#include "painting.h"
#include "paintingfields.h"

#include <QLineF>
#include <QPen>
#include <QPolygonF>

/*!
 * A dimension, as a drawing has one: a click on each of two points, and
 * the dimension line runs beside them - off by as much as its middle
 * handle is dragged - with extension lines to the points, arrows (or
 * ticks, or dots) at its ends and the length written along it: the
 * distance in the schematic's units times a scale, with a unit ("12.5
 * mm"), or a text of its own. The points' handles move them.
 */
class DimensionPainting : public Painting, public FieldEditable {
public:
  enum class Ends { Arrows, Ticks, Dots };
  DimensionPainting();

  void paint(QPainter* painter) override;
  void paintScheme(Schematic* sch) override;
  Painting* newOne() override { return new DimensionPainting(); }
  static Element* info(QString& name, char*& bitmap, bool getNewOne = false);

  bool load(const QString& line) override;
  QString save() override;
  QString saveCpp() override;
  QString saveJSON() override;

  bool getSelected(const QPoint& click, int tolerance) override;
  bool resizeTouched(const QPoint& click, int tolerance) override;
  void MouseMoving(const QPoint& onGrid, Schematic* sch, const QPoint& cursor) override;
  bool MousePressing(Schematic* sch = nullptr) override;
  void MouseResizeMoving(int x, int y, Schematic* sch) override;
  void snapToGrid(Schematic* sch) override;

  bool rotate() noexcept override;
  bool rotate(int xc, int yc) noexcept override;
  bool mirrorX() noexcept override;
  bool mirrorY() noexcept override;

  QRect boundingRect() const noexcept override;
  bool Dialog(QWidget* parent = nullptr) override;
  bool symbolPrimitives(SymbolPrimitives& into) const override;
  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;

  /// What it says: its own text, or the length scaled with its unit.
  QString label() const;
  QPoint start() const { return m_p1; }
  QPoint end() const { return m_p2; }
  int offset() const { return m_offset; }
  void setPoints(const QPoint& start, const QPoint& end, int offset);

protected:
  void afterMove(int dx, int dy) noexcept override;

private:
  struct Parts {
    QLineF line;                  // the dimension line
    QList<QLineF> extensions;     // to the points
    QList<QLineF> ticks;
    QList<QPolygonF> heads;       // arrows, dots: filled
    QTransform text;              // the label's frame: its middle, turned to read
    QRectF textRect;              // in that frame
  };
  Parts parts() const;
  void updateBounds() noexcept;

  QPoint m_p1, m_p2;
  int m_offset = -20;   // off the points, across the line (negative: to its left)
  QPen m_pen;
  QString m_text;       // empty: the length
  double m_scale = 1.0;
  QString m_unit;
  int m_decimals = 0;
  int m_fontSize;
  Ends m_ends = Ends::Arrows;
  int m_placing = 0;    // points placed so far
  int m_dragged = -1;   // 0, 1 the points; 2 the line
};

#endif
