/*
 * shapes.h - paintings drawn in a box: a rounded rectangle, a regular
 *            polygon (a triangle, a star), a brace, a waveform, a text
 *            box (a note, a callout), a table
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef SHAPES_H
#define SHAPES_H

#include "shapepainting.h"

#include <QColor>
#include <QStringList>

/// A rectangle with round corners: a block of a block diagram.
class RoundedRectangle : public ShapePainting {
public:
  explicit RoundedRectangle(bool filled = false);
  Painting* newOne() override { return new RoundedRectangle(m_filled); }
  static Element* info(QString& name, char*& bitmap, bool getNewOne = false);
  static Element* info_filled(QString& name, char*& bitmap, bool getNewOne = false);

  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;
  int radius() const { return m_radius; }

protected:
  QPainterPath outline(qreal w, qreal h) const override;
  QStringList saveExtra() const override;
  bool loadExtra(const QStringList& fields) override;
  QString dialogTitle() const override;

private:
  int m_radius = 10;
};

/// A regular polygon fitted to its box - a triangle points right, as an
/// amplifier does - or a star.
class RegularPolygon : public ShapePainting {
public:
  enum class Kind { Triangle, Polygon, Star };
  explicit RegularPolygon(Kind kind = Kind::Polygon);
  Painting* newOne() override { return new RegularPolygon(m_kind); }
  static Element* info(QString& name, char*& bitmap, bool getNewOne = false);
  static Element* info_polygon(QString& name, char*& bitmap, bool getNewOne = false);
  static Element* info_star(QString& name, char*& bitmap, bool getNewOne = false);

  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;
  int sides() const { return m_sides; }
  bool isStar() const { return m_star; }

protected:
  QPainterPath outline(qreal w, qreal h) const override;
  QStringList saveExtra() const override;
  bool loadExtra(const QStringList& fields) override;
  QString dialogTitle() const override;
  QSize defaultSize() const override { return QSize(60, 60); }

private:
  Kind m_kind;
  int m_sides = 6;
  bool m_star = false;
  int m_inner = 45;   // the star's inner radius, % of the outer
  int m_turn = 0;     // degrees the first corner is turned from the right
};

/// A curly brace, a square bracket or a parenthesis, opening to the right:
/// what groups the parts of a schematic.
class BracePainting : public ShapePainting {
public:
  enum class Kind { Curly, Square, Round };
  BracePainting();
  Painting* newOne() override { return new BracePainting(); }
  static Element* info(QString& name, char*& bitmap, bool getNewOne = false);

  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;
  Kind kind() const { return m_kind; }

protected:
  QPainterPath outline(qreal w, qreal h) const override;
  QStringList saveExtra() const override;
  bool loadExtra(const QStringList& fields) override;
  QString dialogTitle() const override;
  bool fillable() const override { return false; }
  bool picksInside() const override { return true; }
  QSize defaultSize() const override { return QSize(20, 100); }

private:
  Kind m_kind = Kind::Curly;
};

/// A few cycles of a waveform - sine, square, triangle, sawtooth, pulse,
/// damped sine - to mark a source or a signal.
class WaveformPainting : public ShapePainting {
public:
  enum class Shape { Sine, Square, Triangle, Sawtooth, Pulse, DampedSine };
  WaveformPainting();
  Painting* newOne() override { return new WaveformPainting(); }
  static Element* info(QString& name, char*& bitmap, bool getNewOne = false);

  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;
  Shape shape() const { return m_shape; }

protected:
  QPainterPath outline(qreal w, qreal h) const override;
  QStringList saveExtra() const override;
  bool loadExtra(const QStringList& fields) override;
  QString dialogTitle() const override;
  bool fillable() const override { return false; }
  bool picksInside() const override { return true; }
  QSize defaultSize() const override { return QSize(120, 40); }

private:
  Shape m_shape = Shape::Sine;
  double m_cycles = 2.0;
  int m_duty = 25;          // a pulse's, %
  bool m_baseline = false;  // a line through the middle
};

/// Text in a box with round corners, wrapped to it: a block with its name,
/// a note - or, with a pointer to what it is about, a callout.
class TextBoxPainting : public ShapePainting {
public:
  enum class Kind { Block, Note, Callout };
  explicit TextBoxPainting(Kind kind = Kind::Block);
  Painting* newOne() override { return new TextBoxPainting(m_kind); }
  static Element* info(QString& name, char*& bitmap, bool getNewOne = false);
  static Element* info_note(QString& name, char*& bitmap, bool getNewOne = false);
  static Element* info_callout(QString& name, char*& bitmap, bool getNewOne = false);

  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;
  const QString& text() const { return m_text; }
  bool hasPointer() const { return m_pointer; }
  QPoint pointerTip() const { return m_tip; }

protected:
  QPainterPath outline(qreal w, qreal h) const override;
  void paintContent(QPainter* painter, qreal w, qreal h) const override;
  void contentPrimitives(SymbolPrimitives& into, const QTransform& frame, qreal w, qreal h) const override;
  QStringList saveExtra() const override;
  bool loadExtra(const QStringList& fields) override;
  QString dialogTitle() const override;
  bool picksInside() const override { return true; }
  QSize defaultSize() const override;
  bool asksWhenPlaced() const override { return true; }
  void placed() override;
  QList<QPoint> extraHandles() const override;
  void moveExtraHandle(int index, const QPoint& to) override;
  void mapExtraPoints(const std::function<QPoint(const QPoint&)>& map) override;
  QPainterPath extraWorldPath() const override;

private:
  QRectF textRect(qreal w, qreal h) const;
  int flags() const;

  Kind m_kind;
  QString m_text;
  QColor m_textColour = QColor(0, 0, 0);
  int m_fontSize;
  bool m_bold = false;
  int m_align = 1;    // 0 left, 1 centre, 2 right
  int m_valign = 1;   // 0 top, 1 middle, 2 bottom
  int m_radius = 6;
  int m_padding = 6;
  bool m_pointer = false;
  bool m_tipPlaced = false;
  QPoint m_tip;       // the pointer's tip, in the schematic
};

/// A table of text: rows and columns, the first row a header or not - the
/// values of the parts, a revision block.
class TablePainting : public ShapePainting {
public:
  TablePainting();
  Painting* newOne() override { return new TablePainting(); }
  static Element* info(QString& name, char*& bitmap, bool getNewOne = false);

  QList<PaintingField> fields() const override;
  void setField(const QString& key, const QVariant& value) override;
  int rows() const { return int(m_cells.size()); }
  int columns() const { return m_columns; }
  QString cell(int row, int column) const;

protected:
  QPainterPath outline(qreal w, qreal h) const override;
  void paintUnder(QPainter* painter, qreal w, qreal h) const override;
  void paintContent(QPainter* painter, qreal w, qreal h) const override;
  void contentPrimitives(SymbolPrimitives& into, const QTransform& frame, qreal w, qreal h) const override;
  QStringList saveExtra() const override;
  bool loadExtra(const QStringList& fields) override;
  QString dialogTitle() const override;
  bool picksInside() const override { return true; }
  QSize defaultSize() const override;
  bool asksWhenPlaced() const override { return true; }

private:
  void resize(int rows, int columns);

  QList<QStringList> m_cells;   // rows of m_columns texts
  int m_columns = 3;
  bool m_header = true;
  QColor m_headerColour = QColor(0xe4, 0xe8, 0xee);
  QColor m_textColour = QColor(0, 0, 0);
  int m_fontSize;
  int m_align = 0;    // 0 left, 1 centre
};

#endif
