/*
 * polylinepainting.h - a run of straight segments, open or closed
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef POLYLINEPAINTING_H
#define POLYLINEPAINTING_H

#include "painting.h"

#include <QBrush>
#include <QPen>
#include <QPoint>

#include <vector>

/*!
 * \brief A run of straight segments: a polyline, or a polygon.
 *
 * Click a point at a time; clicking the last point again ends the run,
 * clicking the first one closes it into a polygon. The drawing
 * primitive behind it (qucs::Polyline) is one every component symbol
 * can carry, so a polyline drawn in a subcircuit's symbol reaches the
 * instances of that subcircuit like a line or an arc.
 */
class PolylinePainting : public Painting {
public:
  explicit PolylinePainting(bool filled = false);

  void paint(QPainter* painter) override;
  void paintScheme(Schematic*) override;

  Painting* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);
  static Element* info_filled(QString&, char*&, bool getNewOne = false);

  bool load(const QString&) override;
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

  bool Dialog(QWidget* parent = nullptr) override;

  //! The corners, in the order they were placed.
  const std::vector<QPoint>& corners() const { return m_points; }
  bool isClosed() const { return m_closed; }

protected:
  void afterMove(int dx, int dy) noexcept override;

private:
  void updateBounds() noexcept;
  QPolygon polygon() const;

  std::vector<QPoint> m_points;
  QPen m_pen;
  QBrush m_brush;
  bool m_filled;
  bool m_closed = false;

  //! While it is being drawn the last point follows the cursor.
  bool m_beingDrawn = false;
  //! Which corner a drag is moving, -1 for none.
  int m_dragged = -1;
};

#endif // POLYLINEPAINTING_H
