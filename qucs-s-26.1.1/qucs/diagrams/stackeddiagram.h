/*
 * stackeddiagram.h - stacked panes: Cartesian plots one above the other
 * that share one x axis
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef STACKEDDIAGRAM_H
#define STACKEDDIAGRAM_H

#include "rectdiagram.h"

#include <QVector>

/*!
 * \brief Panes one above the other, each a Cartesian plot with its own
 *        y axes (left and right) and its own traces, all on one x axis:
 *        a transient's v(in), v(out), i(L) and gate drive in stripes,
 *        time-aligned. The x range, its zoom and the markers are shared:
 *        a marker reads out the traces of every pane where it is, and a
 *        line across all of them shows where. A graph's pane is its
 *        Graph::pane (0 the top one), its side its yAxisNo.
 */
class StackedDiagram : public RectDiagram {
public:
  StackedDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  static constexpr int MaxPanes = 8;
  static constexpr int Gap = 12;   ///< between two panes' frames

  struct Pane {
    Axis left, right;
  };
  int paneCount() const { return int(m_panes.size()); }
  /// \a n panes (1 to MaxPanes): new ones as the last was set, graphs in
  /// a pane no longer there moved to the last.
  void setPaneCount(int n);
  Pane& pane(int i) { return m_panes[i]; }
  const Pane& pane(int i) const { return m_panes.at(i); }
  /// The pane \a g is in (its Graph::pane, at most the last).
  int paneOf(const Graph* g) const;
  /// A pane's height, and where its bottom is (y up from the frame's).
  int paneHeight() const;
  int paneBottom(int i) const;
  /// The pane at height \a y (up from the frame's bottom); -1 between two.
  int paneAt(double y) const;

  using Diagram::graphAxis;
  const Axis* graphAxis(const Graph* g) const override;
  int calcDiagram() override;
  void calcLimits() override;
  void calcCoordinate(const double*, const double*, const double*, float*, float*, Axis const*) const override;
  MappedPoint pointToValue(const QPointF& point) override;
  void setLimitsBySelectionRect(QRectF) override;
  QString extraMarkerText(Marker const*) const override;
  bool takesLimits() const override { return true; }
  const Axis* limitAxis(const qucs_s::limits::Limit& limit) const override;
  /// What a cursor at \a x reads in pane \a pane: a line per trace there -
  /// those over the x variable \a over only, when it is given; each value
  /// written by \a marker as it writes its own, when it is given.
  QStringList valuesAt(double x, int pane, const QString& over = QString(), const Marker* marker = nullptr) const;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override;
  void createAxisLabels() override;
  void paintInFront(QPainter*, const Colors&) override;
  bool drawsGraph(int valid, const Graph* g) const override;
  Part partOf(const Axis* axis) const override;
  void clearExtraRanges() override;
  void settleExtraRanges() override;
  QPainterPath plotAreaShape() const override;

private:
  /// The pane whose axis \a axis is (-1: none of theirs), and its side.
  int paneOfAxis(const Axis* axis, bool* right = nullptr) const;
  /// \a make run as if the frame were \a height high, what it adds to
  /// Lines and Texts moved up by \a bottom: grid lines behind the rest.
  template <class Make> auto inBand(int bottom, int height, Make make);
  QVector<Pane> m_panes;
};

#endif
