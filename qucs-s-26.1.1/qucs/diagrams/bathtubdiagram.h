/*
 * bathtubdiagram.h - the bathtub curve: a serial data signal's bit error
 * rate against the sampling instant, beside its eye
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef BATHTUBDIAGRAM_H
#define BATHTUBDIAGRAM_H

#include "eyediagram.h"
#include "rectdiagram.h"

/*!
 * \brief Each graph a transient - a received data signal - folded as the
 *        eye diagram folds it (the same unit interval, start, levels and
 *        threshold: EyeDiagram::foldingOf), and of each eye the bit error
 *        rate sampling at each phase of the UI (eyeanalysis.h's Bathtub):
 *        the crossings counted, and beyond them the dual-Dirac model, down
 *        to a floor on a log axis. The opening at a target rate (1e-12) is
 *        marked, and the random, deterministic and total jitter written.
 */
class BathtubDiagram : public RectDiagram {
public:
  BathtubDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  double ui;                 ///< the unit interval; NaN: as the eye diagram tells it
  double start;              ///< from this time on; NaN: from the start
  int levels = 0;            ///< 2: NRZ, 4: PAM4; 0: as each graph's PRBS source is coded
  double threshold;          ///< NRZ's decision threshold; NaN: halfway between the levels
  double ber = 1e-12;        ///< the rate the opening is measured at
  double floor;              ///< the axis down to; NaN: the target's 1e-4
  bool measured = true;      ///< the crossings counted drawn too

  /// The axis' bottom: floor, or the target's 1e-4.
  double floorRate() const;
  /// Each graph's eyes' bathtubs (NRZ one, PAM4 three), as last laid out.
  const QList<QList<qucs_s::eye::Bathtub>>& bathtubs() const { return m_tubs; }
  /// Each graph's fold, as last laid out.
  const QList<qucs_s::eye::Result>& results() const { return m_folding.results; }

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  QList<Part> themeParts() const override;
  bool takesLimits() const override { return false; }
  /// Left automatic: the instants in decimals (0.25, not 250m), the rates
  /// as powers of ten (not 1p).
  qucs_s::numberformat::Notation notationOf(const Axis* axis) const override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the bathtubs are painted
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  bool paintsGraphs() const override { return false; }

private:
  void analyse();
  QStringList summary() const;
  EyeDiagram::Folding m_folding;
  QList<QList<qucs_s::eye::Bathtub>> m_tubs;
};

#endif
