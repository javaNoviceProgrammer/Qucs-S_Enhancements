/***************************************************************************
                              polardiagram.h
                             ----------------
    begin                : Fri Oct 17 2003
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

#ifndef POLARDIAGRAM_H
#define POLARDIAGRAM_H

#include "diagram.h"


class PolarDiagram : public Diagram  {
public: 
  PolarDiagram(int _cx=0, int _cy=0);
 ~PolarDiagram();


  Diagram* newOne();
  static Element* info(QString&, char* &, bool getNewOne=false);
  int  calcDiagram();
  void calcLimits();
  void calcCoordinate(const double*, const double*, const double*, float*, float*, Axis const*) const;

  QList<Part> themeParts() const override;

  /// A Nyquist plot's marks: the critical point -1 and the unit circle
  /// (the chart then reaches 1 at least).
  bool nyquist = false;
  /// Each graph's negative frequencies too: its mirror image in the real
  /// axis, dashed.
  bool mirror = false;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  /// (A circle; its numbers inside it.)
  QPainterPath plotAreaShape() const override;
  bool numbersInside() const override { return true; }
};

#endif
