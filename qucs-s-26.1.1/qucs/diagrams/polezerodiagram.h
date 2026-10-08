/*
 * polezerodiagram.h - the pole-zero map: a pole-zero analysis' roots in
 * the s-plane
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef POLEZERODIAGRAM_H
#define POLEZERODIAGRAM_H

#include "rectdiagram.h"

/*!
 * \brief The s-plane: each value of a graph a root at (sigma, j omega) -
 *        a pole-zero analysis' poles as crosses, its zeros (a variable
 *        named zero) as circles. The j omega axis is drawn, the right
 *        half-plane (unstable) shaded; lines of constant damping (zeta)
 *        and circles of constant natural frequency (omega n) are guides.
 *        A marker on a root reads its omega n, zeta, Q and frequency.
 */
class PoleZeroDiagram : public RectDiagram {
public:
  PoleZeroDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  bool guides = true;   ///< zeta lines and omega n circles

  /// A root: where it is, and what it says.
  struct Root {
    double re = 0, im = 0;
    double wn() const;
    double zeta() const;   ///< NaN at the origin
    double q() const;      ///< infinite at zeta 0 or below
  };
  /// The roots of \a g (every curve's values).
  static QList<Root> rootsOf(const Graph* g);
  /// Whether \a g's roots are zeros (its variable is a zero's), drawn as
  /// circles - else poles, as crosses.
  static bool isZero(const Graph* g);
  /// What a root says: "omega n 31.6k rad/s (f 5.03 kHz), zeta 0.11, Q 4.5".
  static QString rootText(const Root& r);

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  void calcCoordinate(const double*, const double*, const double*, float*, float*, Axis const*) const override;
  QString extraMarkerText(Marker const*) const override;
  bool takesLimits() const override { return false; }
  bool zoomsByRectangle() const override { return true; }

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override;
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  bool paintsGraphs() const override { return false; }
};

#endif
