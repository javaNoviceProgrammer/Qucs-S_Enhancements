/*
 * bodediagram.h - the Bode pair: a loop gain's magnitude in dB above its
 * phase, on one frequency axis, with the crossovers and the margins
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef BODEDIAGRAM_H
#define BODEDIAGRAM_H

#include "stackeddiagram.h"

#include <QJsonObject>
#include <QVector>

/*!
 * \brief Two panes on one logarithmic frequency axis: each graph - a
 *        complex loop gain T(f), an AC variable - as its magnitude in dB
 *        in the upper pane and its phase in degrees (unwrapped) in the
 *        lower one. Where T falls through 0 dB (the gain crossover) and
 *        where its phase falls through -180 degrees (the phase crossover)
 *        are marked across both, with the phase margin and the gain
 *        margin get_dataset measures. A marker on the magnitude reads the
 *        phase too.
 */
class BodeDiagram : public StackedDiagram {
public:
  BodeDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  bool margins = true;   ///< the crossovers and margins marked

  /// A graph's frequencies, magnitudes and phases (degrees, unwrapped):
  /// its first curve.
  static void responseOf(const Graph* g, QVector<double>* f, QVector<double>* magnitude, QVector<double>* phase);
  /// The phase margin and the gain margin of \a g, as get_dataset's
  /// phase_margin and gain_margin measure them: {"phase margin": {...},
  /// "gain margin": {...}}, each with "error" when there is none.
  static QJsonObject marginsOf(const Graph* g);

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  QString extraMarkerText(Marker const*) const override;
  bool takesLimits() const override { return false; }

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
};

#endif
