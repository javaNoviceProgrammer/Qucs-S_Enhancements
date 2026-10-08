/*
 * buspainting.h - a bus: a thick line named for the nets it gathers
 * (D[7:0]); it joins nothing by touching - its members are nets by their
 * names (D0 ... D7)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef BUSPAINTING_H
#define BUSPAINTING_H

#include "graphicline.h"

#include <QStringList>

/*!
 * \brief A bus, as many schematic tools draw one: a thick line with its
 *        name, D[7:0], to which the wires of its nets are drawn. It is a
 *        drawing: it joins nothing by touching - its members are the nets
 *        named D0 to D7 (as two labels of one name are one net), and Check
 *        Schematic says when a net ends on it with no member's name.
 */
class BusPainting : public GraphicLine {
public:
  BusPainting(int ax = 0, int ay = 0, int bx = 0, int by = 0, const QString& name = QString());

  void paint(QPainter* painter) override;
  Painting* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  bool load(const QString&) override;
  QString save() override;
  QString saveJSON() override;
  bool Dialog(QWidget* parent = nullptr) override;

  /// Its name, D[7:0] (D[0:7], D[0..7] alike).
  QString busName;
  /// The members of a bus named \a name: D[7:0]'s D7 down to D0 - in the
  /// order written; false (and none) when it is no bus's name.
  static bool membersOf(const QString& name, QStringList* members);
  QStringList members() const;
  /// Whether \a p is on its line (a wire's end drawn to it).
  bool touches(const QPoint& p) const;

  static constexpr int Width = 6;
};

#endif
