/*
 * shapepainting.h - a painting drawn in a box: an outline (rounded
 *                   rectangle, polygon, star, brace, waveform, text box,
 *                   table...), turned and mirrored in quarter turns
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef SHAPEPAINTING_H
#define SHAPEPAINTING_H

#include "painting.h"
#include "paintingfields.h"

#include <QBrush>
#include <QPainterPath>
#include <QPen>
#include <QTransform>

/*!
 * A shape drawn in a box, as a rectangle is: a click on one corner and one
 * on the other (a click and nothing more gives it its own size); a handle
 * on each corner resizes it. A subclass gives its outline in its own frame
 * - (0, 0) to (w, h), before it is turned or mirrored - and what is drawn
 * in it (text); the shape turns in quarter turns and mirrors about the
 * box's axes (angle, mirrored). Its line and filling are a rectangle's.
 *
 * Saved as "Name x y w h colour width style fillcolour fillstyle filled
 * angle mirrored ..." - the subclass's own fields after them. In a
 * subcircuit's symbol it becomes polylines (curves flattened) and texts,
 * which every instance draws.
 */
class ShapePainting : public Painting, public FieldEditable {
public:
  ShapePainting(const QString& name, bool filled);

  void paint(QPainter* painter) override;
  void paintScheme(Schematic* sch) override;

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

  /// The box, its corners in order.
  QRect box() const { return QRect(QPoint(std::min(x1, x2), std::min(y1, y2)), QPoint(std::max(x1, x2), std::max(y1, y2))); }
  void setBox(const QRect& box);
  int angle() const { return m_angle; }
  bool isMirrored() const { return m_mirrored; }
  const QPen& pen() const { return m_pen; }
  const QBrush& brush() const { return m_brush; }
  bool isFilled() const { return m_filled; }
  /// The outline as drawn, in the schematic's coordinates.
  QPainterPath worldPath() const;
  /// From the shape's own frame to the schematic, mirrored or not.
  QTransform frame(bool withMirror = true) const;
  /// The size of the shape's own frame (w, h before it is turned).
  QSizeF frameSize() const;

protected:
  /// The outline in the frame (0, 0) - (w, h).
  virtual QPainterPath outline(qreal w, qreal h) const = 0;
  /// What is drawn in the shape, in its frame (never mirrored: text reads).
  virtual void paintContent(QPainter* /*painter*/, qreal /*w*/, qreal /*h*/) const {}
  /// What is drawn under the outline (a table's header band), in its frame.
  virtual void paintUnder(QPainter* /*painter*/, qreal /*w*/, qreal /*h*/) const {}
  /// And as symbol primitives, \a frame mapping its frame to the schematic.
  virtual void contentPrimitives(SymbolPrimitives& /*into*/, const QTransform& /*frame*/, qreal /*w*/, qreal /*h*/) const {}
  /// The subclass's fields of the line (no spaces in one: encodeText()).
  virtual QStringList saveExtra() const { return {}; }
  virtual bool loadExtra(const QStringList& /*fields*/) { return true; }
  /// Whether it has a filling to choose (not a brace, a waveform).
  virtual bool fillable() const { return true; }
  /// Whether a click inside picks it (a text box), not only on its line.
  virtual bool picksInside() const { return m_filled; }
  /// Its size when placed with a click and nothing more (frame size).
  virtual QSize defaultSize() const { return QSize(80, 50); }
  /// Whether its dialog comes up when it is placed (text to write).
  virtual bool asksWhenPlaced() const { return false; }
  /// Placed: a callout gives its tail a place.
  virtual void placed() {}

  /// Handles past the corners (a callout's tail), in the schematic.
  virtual QList<QPoint> extraHandles() const { return {}; }
  virtual void moveExtraHandle(int /*index*/, const QPoint& /*to*/) {}
  /// Points of its own in the schematic (moved, turned, mirrored with it).
  virtual void mapExtraPoints(const std::function<QPoint(const QPoint&)>& /*map*/) {}
  /// More outline in the schematic's coordinates (a callout's tail),
  /// united with the frame's.
  virtual QPainterPath extraWorldPath() const { return {}; }

  void afterMove(int dx, int dy) noexcept override;
  /// The title of its dialog.
  virtual QString dialogTitle() const = 0;

  QPen m_pen;
  QBrush m_brush;
  bool m_filled;
  int m_angle = 0;         // quarter turns counter-clockwise, in degrees
  bool m_mirrored = false; // mirrored in its own frame (about its vertical axis), before it is turned

private:
  enum class Corner { none, topLeft, topRight, bottomRight, bottomLeft, extra };
  Corner m_dragged = Corner::none;
  int m_draggedExtra = -1;
  bool m_beingDrawn = false;
};

#endif
