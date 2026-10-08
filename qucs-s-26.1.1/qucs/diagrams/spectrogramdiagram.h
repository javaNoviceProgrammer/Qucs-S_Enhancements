/*
 * spectrogramdiagram.h - the spectrogram (waterfall seen from above): a
 * transient's spectrum as it goes, time along, frequency up, level in colour
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef SPECTROGRAMDIAGRAM_H
#define SPECTROGRAMDIAGRAM_H

#include "contourdiagram.h"
#include "spectrum.h"

/*!
 * \brief Each graph a transient: cut into segments (each \a segment long,
 *        overlapping by \a overlap), each segment's windowed spectrum
 *        (spectrum.h) a column - its level in dB in colour, over the
 *        \a range dB below the loudest (the z axis' range when given). A
 *        contour map of time and frequency: its colours, colour bar,
 *        iso-lines (none unless asked), pass band and readout are the
 *        contour map's.
 */
class SpectrogramDiagram : public ContourDiagram {
public:
  SpectrogramDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  qucs_s::spectrum::Window window = qucs_s::spectrum::Window::Hann;
  double segment;          ///< seconds a column; NaN: a sixteenth of the transient
  double overlap = 0.5;    ///< of a segment with the next, 0 to 0.9
  double range = 80;       ///< dB of colours below the loudest (automatic range)
  double upTo;             ///< Hz the rows go up to; NaN: a quarter past the highest within range of the loudest

  /// The segment's length \a g's columns are of (NaN: no transient).
  double segmentOf(const Graph* g) const;
  Grid gridFor(const Graph* g) const override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void createAxisLabels() override;
  void autoRange(double lo, double hi) override;
  QString colourName() const override;
};

#endif
