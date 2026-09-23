/*
 * histogramdiagram.h - a histogram of the values of a variable: a Monte
 * Carlo's recorded values, or any other
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef HISTOGRAMDIAGRAM_H
#define HISTOGRAMDIAGRAM_H

#include "rectdiagram.h"

#include <QVector>

/*!
 * \brief The histogram diagram: every graph is a variable, and every value
 *        it has in the dataset (each sample of a Monte Carlo, each point of
 *        a sweep) is counted into bins along the x axis - the same bins for
 *        all graphs, drawn as bars in the graph's colour over each other.
 *        The bins cover the values (or the x axis' manual limits), their
 *        number automatic (Freedman-Diaconis) or set; the height a count,
 *        a percentage or a probability density. A normal distribution of
 *        the same mean and deviation, a box with the number, mean and
 *        deviation of every graph, and lower and upper limits - with the
 *        share of values between them - can go with it. The axes, grid,
 *        notation and legend are those of the Cartesian diagram.
 */
class HistogramDiagram : public RectDiagram {
public:
  HistogramDiagram(int cx = 0, int cy = 0);

  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  enum Height { Counts = 0, Percent = 1, Density = 2 };

  int bins = 0;              ///< 0: automatic
  int height = Counts;
  bool normalFit = false;    ///< the normal distribution of each graph's mean and deviation
  bool statistics = true;    ///< the box with N, mean and deviation
  double lowerLimit;         ///< a vertical line, and the share of values between the two; NaN: none
  double upperLimit;

  /// One graph's histogram: bin k spans [low + k*width, low + (k+1)*width).
  struct Bars {
    double low = 0.0;
    double width = 1.0;
    QVector<double> counts;
    int n = 0;          ///< the finite values
    int outside = 0;    ///< of them, outside the bins
    int within = 0;     ///< of them, between the limits (all without limits)
    double mean = 0.0, sigma = 0.0, min = 0.0, max = 0.0;
  };
  /// The bars of each graph, as last laid out.
  const QList<Bars>& bars() const { return m_bars; }

  /// Every finite value of \a graph (a complex one by its magnitude).
  static QVector<double> values(const Graph* graph);
  /// The number of bins for \a values over [low, high]: Freedman-Diaconis,
  /// or the square root of their number when the quartiles meet.
  static int automaticBins(const QVector<double>& values, double low, double high);
  /// \a values counted into \a count bins over [low, high].
  static Bars bin(const QVector<double>& values, double low, double high, int count);
  /// The factor from a count to the height drawn.
  double scaleOf(const Bars& bars) const;

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the bars are painted, not traced
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*) override;

private:
  QString shortName(const Graph* graph) const;
  QString statisticText(double value) const;
  QList<Bars> m_bars;
};

#endif
