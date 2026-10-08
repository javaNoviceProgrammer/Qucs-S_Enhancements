/*
 * tornadodiagram.h - the tornado chart: a bar for each trace, sorted by
 * size - a sensitivity run's parts, corner or Monte Carlo spreads
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef TORNADODIAGRAM_H
#define TORNADODIAGRAM_H

#include "rectdiagram.h"

#include <functional>

/*!
 * \brief Each graph a horizontal bar, the largest on top, so what moves an
 *        output most reads first: its value at a point of its sweep (a
 *        sensitivity, from 0), or its spread over the whole sweep (corner
 *        or Monte Carlo runs: from its lowest to its highest, its value at
 *        the point marked). Bars of nothing (0, or no spread) are left out
 *        and counted, as are the smallest past the number shown.
 */
class TornadoDiagram : public RectDiagram {
public:
  TornadoDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  enum Mode { Value = 0, Spread };
  static constexpr int MaxBars = 50;

  int mode = Value;
  double at;           ///< the point of the sweep each value is taken at; NaN: its first
  int bars = 15;       ///< at most this many, the largest

  struct Bar {
    QString name;      ///< the trace's variable, without its dataset
    int graph = -1;
    double value = 0;  ///< at the point
    double low = 0, high = 0;   ///< what the bar spans: from 0 to the value, or the spread
    double size() const { return high - low; }
  };
  /// Shown, the largest first; and how many were left out as nothing or
  /// past the number shown.
  const QList<Bar>& shown() const { return m_bars; }
  int nothing() const { return m_nothing; }
  int smaller() const { return m_smaller; }
  /// A bar's where its trace has no data.
  QStringList noData() const { return m_noData; }

  /// Of a sensitivity run's variables (ngspice's .SENS: "r1", "r1_scale",
  /// "r1_tc1", "v1", "v1_freq" ...: a part's own, and its parameters'
  /// beside it - others' left out), one a part: its "_scale" when it has one - its
  /// value times the derivative, the output's change per 100 % of the
  /// part's value, comparable across parts - else its own derivative (per
  /// its unit: a source's V/V). \a nonzero, when given, leaves out the
  /// ones of nothing.
  static QStringList sensitivityParts(const QStringList& names, const std::function<bool(const QString&)>& nonzero = {});

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  bool takesLimits() const override { return false; }
  QRectF paintedRect(const QFontMetricsF& metrics) const override;
  QList<Part> themeParts() const override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the bars are painted
  /// Its traces' data kept after a layout (no y axis counts them): the
  /// bars are made of it again at every layout.
  bool drawsGraph(int, const Graph*) const override { return true; }
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  bool paintsGraphs() const override { return false; }

private:
  void collect();
  /// The bar's row: its band, in its coordinates (y up).
  QRectF rowOf(int k) const;
  double barsWidth(const QFontMetricsF& metrics) const;
  QList<Bar> m_bars;
  int m_nothing = 0, m_smaller = 0;
  QStringList m_noData;
};

#endif
