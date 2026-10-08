/*
 * boxplotdiagram.h - the box plot: each trace's spread of values - corner
 * and Monte Carlo runs - as a box of its quartiles, its median, whiskers
 * and outliers
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef BOXPLOTDIAGRAM_H
#define BOXPLOTDIAGRAM_H

#include "rectdiagram.h"

/*!
 * \brief A box a trace, side by side on one value axis: the middle half of
 *        its values (the first to the third quartile), its median across,
 *        its mean a diamond, whiskers out to the farthest values within 1.5
 *        times the box's height of it (Tukey's; or all the way, \a range),
 *        the values beyond as circles. A trace's values: of a trace of
 *        several curves - a parameter swept, Monte Carlo runs - each curve's
 *        at \a at (NaN: its last x, the final value); of a trace of one, all
 *        of its values (one per run).
 */
class BoxPlotDiagram : public RectDiagram {
public:
  BoxPlotDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  double at;             ///< x a curve's value is taken at; NaN: its last
  bool range = false;    ///< whiskers to the lowest and highest, no outliers

  struct Box {
    QString name;
    int n = 0;
    double min = 0, q1 = 0, median = 0, q3 = 0, max = 0, mean = 0, sd = 0;
    double low = 0, high = 0;   ///< the whiskers' ends
    QVector<double> outliers;
    QString error;              ///< why there is no box
    bool ok() const { return error.isEmpty(); }
  };
  /// \a values' box: quartiles between the samples (as R's type 7).
  static Box boxOf(QVector<double> values, bool range);
  /// The values of \a g a box is of.
  QVector<double> valuesOf(const Graph* g) const;
  const QList<Box>& boxes() const { return m_boxes; }

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  bool takesLimits() const override { return false; }
  QList<Part> themeParts() const override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the boxes are painted
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  bool paintsGraphs() const override { return false; }

private:
  QList<Box> m_boxes;
  /// A box's column: its middle, in its coordinates.
  double columnOf(int k) const;
};

#endif
