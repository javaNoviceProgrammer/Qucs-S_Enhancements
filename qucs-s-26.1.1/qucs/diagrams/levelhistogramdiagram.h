/*
 * levelhistogramdiagram.h - the level histogram: a serial data signal's
 * values at the sampling instant, counted, beside its eye
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef LEVELHISTOGRAMDIAGRAM_H
#define LEVELHISTOGRAMDIAGRAM_H

#include "eyediagram.h"
#include "rectdiagram.h"

/*!
 * \brief Each graph a transient - a received data signal - folded as the
 *        eye diagram folds it (the same unit interval, start, levels and
 *        threshold: EyeDiagram::foldingOf), and sampled at one instant of
 *        each symbol: the eye's centre, or a phase from it. The values are
 *        counted into bins up the signal's axis - as a sampling
 *        oscilloscope's vertical histogram, the counts across - with each
 *        level's Gaussian of its mean and spread over them and each eye's
 *        decision threshold. A box gives each level's mean, spread and
 *        symbols, and each eye's Q, the bit error rate Q gives and the
 *        vertical opening at a target rate (eyeanalysis.h's
 *        VoltageBathtub).
 */
class LevelHistogramDiagram : public RectDiagram {
public:
  LevelHistogramDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  double ui;                 ///< the unit interval; NaN: as the eye diagram tells it
  double start;              ///< from this time on; NaN: from the start
  int levels = 0;            ///< 2: NRZ, 4: PAM4; 0: as each graph's PRBS source is coded
  double threshold;          ///< NRZ's decision threshold; NaN: halfway between the levels
  double phase = 0.0;        ///< the sampling instant, in UI from the eye's centre (-0.5 to 0.5)
  int bins = 0;              ///< 0: automatic, from the levels' own spreads
  bool gaussians = true;     ///< each level's normal distribution over its bars
  double ber = 1e-12;        ///< the rate the vertical opening is measured at

  /// One graph's: its symbols' values counted into bin k, [low + k width,
  /// low + (k + 1) width); its levels and eyes.
  struct Histogram {
    QString error;                                 ///< why there is none (empty: there is one)
    double low = 0.0, width = 1.0;
    QVector<double> counts;
    QList<qucs_s::eye::Level> levels;              ///< lowest first
    QList<qucs_s::eye::VoltageBathtub> eyes;       ///< NRZ one, PAM4 three
    QVector<double> thresholds;                    ///< each eye's, as the fold decides
    int symbols = 0;
  };
  /// Each graph's, as last laid out.
  const QList<Histogram>& histograms() const { return m_histograms; }

  /// The number of bins over [low, high] for \a levels: Freedman-Diaconis
  /// of each level's own values, the finest of them - those of the values
  /// together take the gap between the levels for a spread, and a level's
  /// noise falls in one bar. The square root of their number when no level
  /// has a spread. At most 400.
  static int levelBins(const QList<qucs_s::eye::Level>& levels, double low, double high);

  void getAxisLimits(Graph*) override;
  /// (No right axis.)
  QList<Part> themeParts() const override;
  int calcDiagram() override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the bars are painted, not traced
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  bool paintsGraphs() const override { return false; }

private:
  QStringList summary() const;
  QList<Histogram> m_histograms;
};

#endif
