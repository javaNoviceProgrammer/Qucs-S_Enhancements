/*
 * contourdiagram.h - the contour map: a value over two swept parameters in
 * colour, its iso-lines labelled, the region that passes a spec
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef CONTOURDIAGRAM_H
#define CONTOURDIAGRAM_H

#include "rectdiagram.h"

#include <QHash>
#include <QImage>
#include <QPolygonF>

/*!
 * \brief A value swept over two parameters - a gain over R and C, a corner
 *        grid, a load-pull map - drawn over them: the first graph in
 *        colour (its colour bar beside the frame, the colour range the z
 *        axis'), every graph's iso-lines at round values, labelled. With a
 *        pass band (a lowest and/or a highest value), what fails is hatched
 *        and the band's edges drawn bold, and the share that passes said.
 *        A graph's first sweep is along x, its second up y; further sweeps
 *        at their first value.
 */
class ContourDiagram : public RectDiagram {
public:
  ContourDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  enum Map { Viridis = 0, Turbo, Grey };
  static QStringList mapNames();
  static constexpr int MaxLevels = 50;

  int levels = 8;          ///< iso-lines across the range, at round values (0: none)
  int map = Viridis;
  bool filled = true;      ///< the first graph in colour; else its iso-lines alone
  bool labels = true;      ///< each iso-line's value written on it
  double passMin;          ///< the values that pass: at least this (NaN: no lowest) ...
  double passMax;          ///< ... and at most this (NaN: no highest)

  /// A graph's values on the grid of its two sweeps.
  struct Grid {
    QString error;         ///< why there is none
    QString xName, yName;  ///< the sweeps
    QVector<double> x, y;  ///< each rising
    QVector<double> v;     ///< v[j * x.size() + i]; NaN where there is none
    int slices = 1;        ///< the values of the sweeps beyond the second (their first taken)
    bool ok() const { return error.isEmpty(); }
    double at(int i, int j) const { return v.at(j * x.size() + i); }
    /// Bilinear between the four points round (x, y), in the axes' scales
    /// (log: in their logarithms); NaN outside the grid or by a gap.
    double valueAt(double x, double y, bool logX = false, bool logY = false) const;
  };
  /// \a g's grid: its first curve's sweeps and values (the part it shows).
  static Grid gridOf(const Graph* g);
  /// The grid this diagram makes of \a g: gridOf() - a spectrogram's, of
  /// its spectra.
  virtual Grid gridFor(const Graph* g) const { return gridOf(g); }
  /// Each graph's, as last laid out.
  const QList<Grid>& grids() const { return m_grids; }
  /// The round values the iso-lines of a range from \a low to \a high are
  /// at, about \a count of them.
  static QVector<double> levelsIn(double low, double high, int count);
  /// The share of the first grid's points that pass (0 to 1); NaN without
  /// a band or a grid.
  double passing() const;
  bool passes(double v) const;
  bool hasBand() const { return std::isfinite(passMin) || std::isfinite(passMax); }
  /// The map's colour at \a t (0 to 1).
  QColor colourAt(double t) const;
  /// Where \a level crosses the grid: segments, in the diagram's
  /// coordinates (origin the lower left corner, y up).
  QVector<QLineF> isoLine(const Grid& g, double level) const;

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  bool takesLimits() const override { return false; }
  QRectF paintedRect(const QFontMetricsF& metrics) const override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the grids are painted
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  bool paintsGraphs() const override { return false; }
  void clearExtraRanges() override { m_byGraph.clear(); }
  /// The colour range when it is automatic: the first grid's values'.
  virtual void autoRange(double lo, double hi);
  /// The name the colours are of, by the colour bar.
  virtual QString colourName() const;

private:
  QHash<const Graph*, Grid> m_byGraph;   // as each graph's was laid out (getAxisLimits)
  QImage render(const QSize& pixels) const;
  QPointF pointOf(double x, double y) const;
  QList<Grid> m_grids;
  QImage m_image;
  quint64 m_generation = 0, m_imageGeneration = ~quint64(0);
};

#endif
