/*
 * nicholsdiagram.h - the Nichols chart: a loop gain's open-loop gain in dB
 * against its phase, over the closed loop's M and N contours
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef NICHOLSDIAGRAM_H
#define NICHOLSDIAGRAM_H

#include "rectdiagram.h"

#include <QHash>
#include <QPolygonF>
#include <QVector>

/*!
 * \brief Each graph - a complex loop gain G, an AC variable - drawn as its
 *        gain in dB (up) against its phase in degrees (along, unwrapped,
 *        the first sample's within -360 to 0). Behind it the contours of
 *        the closed loop G / (1 + G): of constant magnitude M (dB) and of
 *        constant phase N (degrees); the critical point, -180 degrees at
 *        0 dB, is marked.
 */
class NicholsDiagram : public RectDiagram {
public:
  NicholsDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  bool grid = true;   ///< the M and N contours

  /// The phase (degrees, unwrapped, starting within -360 to 0) and the
  /// gain (dB) of each sample of \a g (every curve's).
  static void pointsOf(const Graph* g, QVector<double>* phase, QVector<double>* gain);
  /// A contour of the closed loop as (phase, gain dB) points of G:
  /// constant |G/(1+G)| = \a mDb dB, or constant arg = \a nDegrees. Broken
  /// where the phase wraps (a NaN point between two pieces).
  static QPolygonF mContour(double mDb);
  static QPolygonF nContour(double nDegrees);

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  void calcCoordinate(const double*, const double*, const double*, float*, float*, Axis const*) const override;
  QString extraMarkerText(Marker const*) const override;
  bool takesLimits() const override { return false; }

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override;
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;

private:
  // The graph being traced, and its points' unwrapped phases: what
  // calcCoordinate() reads for a sample of it.
  const Graph* m_tracing = nullptr;
  QVector<double> m_phase;
};

#endif
