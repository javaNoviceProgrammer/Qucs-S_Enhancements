/*
 * eyediagram.h - the eye diagram of a serial data signal: a transient
 * folded at its unit interval, its traces laid over each other
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef EYEDIAGRAM_H
#define EYEDIAGRAM_H

#include "rectdiagram.h"
#include "eyeanalysis.h"

#include <QImage>

/*!
 * \brief The eye diagram: every graph is a transient - a received data
 *        signal, NRZ or PAM4 - cut a unit interval (UI) apart into windows
 *        of a few UIs and laid over each other, as a sampling oscilloscope
 *        triggered at each bit shows it. The eye's centre is in the middle.
 *        Drawn as a density (how many traces pass each point, in colour) or
 *        as the traces themselves. The UI is given or told from the first
 *        graph's crossings; the settling at the start can be left out.
 *        Beside it, what is measured on each graph (eyeanalysis.h): the
 *        eye's height and width, the jitter, the levels, Q, and how many
 *        UIs go through a mask - with the height, the width, the threshold
 *        and the mask marked in it. The axes, grid, notation, zoom and
 *        legend are those of the Cartesian diagram.
 */
class EyeDiagram : public RectDiagram {
public:
  EyeDiagram(int cx = 0, int cy = 0);

  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  enum Drawn { Density = 0, Traces = 1 };
  static constexpr int MaxSpan = 8;

  double ui;                 ///< the unit interval; NaN: told from the first graph's crossings
  int span = 2;              ///< the UIs across it, 1 to MaxSpan
  double start;              ///< the eye from this time on (the settling before it left out); NaN: from the start
  int levels = 2;            ///< 2: NRZ, 4: PAM4
  double threshold;          ///< NRZ's decision threshold; NaN: halfway between the levels
  int drawn = Density;
  bool measurements = true;  ///< what was measured beside it, and the height, width and threshold marked in it
  double maskWidth;          ///< the mask's width, in UI (0 to 1); NaN: no mask
  double maskHeight;         ///< ... and its height, in the unit of the signal

  /// The eye of each graph (its first curve), as last laid out.
  const QList<qucs_s::eye::Result>& results() const { return m_results; }
  /// The unit interval it is folded at: NaN when there is none (nothing
  /// is drawn).
  double foldedUi() const { return m_ui; }
  /// What is written beside it, a line each.
  QStringList measurementLines() const;
  /// \a value in engineering notation with \a unit: "92.51 ps".
  static QString engineering(double value, const QString& unit);

  void getAxisLimits(Graph*) override;
  /// (No right axis.)
  QList<Part> themeParts() const override;
  int calcDiagram() override;
  QRectF paintedRect(const QFontMetricsF& metrics) const override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the traces are painted, folded
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;

private:
  struct Line {
    QString text;
    int graph = -1;    // the graph a heading names (its colour beside it), else -1
  };
  QList<Line> lines() const;
  /// Where the measurements are written: beside the frame, in the
  /// diagram's coordinates (origin at the lower left corner, y downwards).
  QRectF boxRect(const QFontMetricsF& metrics) const;
  QImage render(const QSize& pixels) const;
  void paintMarks(QPainter* painter) const;
  void analyse();

  QList<qucs_s::eye::Result> m_results;
  double m_ui;
  QImage m_image;   // the traces, as last drawn
  quint64 m_generation = 0, m_imageGeneration = ~quint64(0);
  QRgb m_imagePaper = 0;
};

#endif
