/*
 * spectrumdiagram.h - the spectrum view: a transient's spectrum in dBc,
 * its harmonics numbered, THD, SFDR, SNR and SINAD written beside it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef SPECTRUMDIAGRAM_H
#define SPECTRUMDIAGRAM_H

#include "rectdiagram.h"
#include "spectrum.h"

#include <QHash>

/*!
 * \brief Each graph a transient (a periodic signal): its spectrum,
 *        resampled evenly and windowed (spectrum.h), drawn in dBc - the
 *        fundamental at 0 - or in dB of its unit; its fundamental and
 *        harmonics marked and numbered; THD, SFDR, SNR and SINAD written
 *        in its corner.
 */
class SpectrumDiagram : public RectDiagram {
public:
  SpectrumDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  qucs_s::spectrum::Window window = qucs_s::spectrum::Window::Hann;
  int harmonics = 9;        ///< numbered and counted in THD
  bool dbc = true;          ///< in dBc (the fundamental 0), else in dB of the signal's unit
  bool stems = false;       ///< every bin a stem, else one line through them
  double from;              ///< the signal from this time on (the settling left out); NaN: all of it
  double fundamental;       ///< NaN: the strongest line

  struct Result {
    qucs_s::spectrum::Spectrum spectrum;
    qucs_s::spectrum::Analysis analysis;
  };
  /// Each graph's, as last laid out.
  Result resultOf(const Graph* g) const { return m_results.value(g); }
  /// The spectrum and what it says of \a g's first curve, with these
  /// settings.
  Result compute(const Graph* g) const;

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  bool takesLimits() const override { return false; }

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the spectra are painted
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  bool paintsGraphs() const override { return false; }
  void clearExtraRanges() override { m_results.clear(); }

private:
  /// \a v in the units it is drawn in: dBc, or dB.
  double shown(double amplitude, const Result& r) const;
  QStringList summary() const;
  QHash<const Graph*, Result> m_results;
};

#endif
