/***************************************************************************
                                painting.h
                               ------------
    begin                : Sat Nov 22 2003
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

#ifndef PAINTING_H
#define PAINTING_H

#include "element.h"

#include <QList>

/// Drawing primitives of a component's symbol, as a painting drawn in a
/// subcircuit's symbol becomes them in the instances.
struct SymbolPrimitives {
  QList<qucs::Line*> lines;
  QList<qucs::Polyline*> polylines;
  QList<Text*> texts;
  QList<qucs::Image*> images;
};

class Painting : public Element  {
public:
  Painting() { Type = isPainting; }
 ~Painting() override = default;

  virtual void paint(QPainter* p) = 0;
  virtual Painting* newOne() = 0;

  virtual bool load(const QString& /*input*/) = 0;
  virtual QString save() = 0;
  virtual QString saveCpp() = 0;
  virtual QString saveJSON() = 0;

  virtual bool getSelected(const QPoint& /*click*/, int /*tolerance*/) { return false; };
  virtual bool resizeTouched(const QPoint& /*click*/, int /*tolerance*/) { return false; };

  virtual void MouseMoving(const QPoint& /*onGrid*/, Schematic* /*sch*/, const QPoint& /*cursor*/) {};
  virtual bool MousePressing(Schematic* sch = nullptr) { Q_UNUSED(sch) return false; };

  virtual void MouseResizeMoving(int, int, Schematic*) {};

  virtual void snapToGrid(Schematic* /*sch*/) {};

  using Element::moveCenterTo;
  bool  moveCenterTo(int x, int y) noexcept override;
  bool  moveCenter(int dx, int dy) noexcept override;

  QRect boundingRect() const noexcept override;

  virtual bool Dialog(QWidget* parent = nullptr) { Q_UNUSED(parent) return false; };

  /// The painting as a symbol's drawing primitives (new, for the caller to
  /// own): how one drawn in a subcircuit's symbol reaches its instances.
  /// False for the kinds a symbol reads by their own lines.
  virtual bool symbolPrimitives(SymbolPrimitives& /*into*/) const { return false; }
  /// A painting of the kinds made of primitives (shapes, a dimension, a
  /// formula) by the word its line begins with; nullptr for any other.
  static Painting* newNamed(const QString& type);

  QString Name; // name of painting, e.g. for saving

protected:
  QString toPenString (int);
  QString toBrushString (int);
  void updateCenter() noexcept;
  virtual void afterMove([[maybe_unused]] int dx, [[maybe_unused]] int dy) noexcept {};
};

#endif
