/*
 * formulapainting.h - a formula: TeX math typeset on the schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef FORMULAPAINTING_H
#define FORMULAPAINTING_H

#include "painting.h"
#include "paintingfields.h"

#include <QColor>
#include <QImage>

/*!
 * A formula written in TeX - "f_c = \frac{1}{2\pi RC}" - typeset as TeX
 * would (mathtypeset.h): fractions, roots, sums and integrals with their
 * limits, matrices, Greek. Placed with a click, written in its dialog
 * with a preview; drawn sharp at any zoom (typeset again for it), turned
 * in quarter turns, its handle scales it. In a subcircuit's symbol it is
 * an image.
 */
class FormulaPainting : public Painting, public FieldEditable {
public:
  FormulaPainting();

  void paint(QPainter* painter) override;
  void paintScheme(Schematic* sch) override;
  Painting* newOne() override { return new FormulaPainting(); }
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

  bool rotate() noexcept override;
  bool rotate(int xc, int yc) noexcept override;

  QRect boundingRect() const noexcept override;
  bool Dialog(QWidget* parent = nullptr) override;
  bool symbolPrimitives(SymbolPrimitives& into) const override;
  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;

  const QString& tex() const { return m_tex; }
  int size() const { return m_size; }
  int angle() const { return m_angle; }
  /// Its size before it is turned, in the schematic's units.
  QSizeF formulaSize() const;
  /// Typeset in \a colour at \a pixelRatio device pixels a unit.
  QImage image(const QColor& colour, qreal pixelRatio) const;
  /// \a wanted pixels a unit, fewer when the image would pass 64 MB or
  /// 8192 pixels a side.
  qreal cappedRatio(qreal wanted) const;

protected:
  void afterMove(int dx, int dy) noexcept override;

private:
  QTransform transform() const;
  void updateBounds() noexcept;

  QString m_tex;
  int m_size;
  QColor m_colour = QColor(0, 0, 0);
  bool m_display = true;
  int m_angle = 0;
  QPoint m_at;   // its top left before it is turned
  bool m_dragged = false;

  mutable QString m_cacheKey;
  mutable QImage m_cache;
  mutable QString m_sizeKey;
  mutable QSizeF m_sizeCache;
};

#endif
