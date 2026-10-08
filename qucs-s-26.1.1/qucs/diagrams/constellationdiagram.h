/*
 * constellationdiagram.h - the constellation (IQ) diagram: an I and a Q
 * signal sampled once a symbol, against each other; the ideal points of a
 * modulation and the error vector (EVM)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef CONSTELLATIONDIAGRAM_H
#define CONSTELLATIONDIAGRAM_H

#include "rectdiagram.h"

#include <QPointF>

/*!
 * \brief Its traces in pairs - an I signal, then its Q - each pair sampled
 *        once a symbol (every \a period from \a offset; NaN: every sample)
 *        and drawn I along, Q up, on one scale. With a modulation, its
 *        ideal points (scaled to the samples' rms) are marked, and the
 *        error vector measured: each sample's distance to the nearest ideal
 *        point, its rms and its peak over the ideal points' rms (EVM, %).
 */
class ConstellationDiagram : public RectDiagram {
public:
  ConstellationDiagram(int cx = 0, int cy = 0);
  Diagram* newOne() override;
  static Element* info(QString&, char*&, bool getNewOne = false);

  enum Modulation { None = 0, Bpsk, Qpsk, Psk8, Qam16, Qam64 };
  static QStringList modulationNames();

  double period;           ///< a symbol's length (s); NaN: every sample a point
  double offset;           ///< the first sample's time; NaN: half a period from the start
  double from;             ///< samples from this time on (the settling left out); NaN: all
  int modulation = None;

  /// A modulation's ideal points, their rms 1.
  static QList<QPointF> idealPoints(int modulation);

  struct Pair {
    QString i, q;              ///< the traces' names
    QList<QPointF> points;     ///< sampled
    QList<QPointF> ideal;      ///< scaled to the samples' rms
    double evmRms = 0, evmPeak = 0;   ///< percent of the ideal rms
    QPointF centre;            ///< the samples' mean (a DC offset)
    QString error;
    QString note;              ///< what was left out (past the most symbols taken)
    bool ok() const { return error.isEmpty(); }
  };
  const QList<Pair>& pairs() const { return m_pairs; }
  /// The pair of \a gi and \a gq (an I's and a Q's traces).
  Pair pairOf(const Graph* gi, const Graph* gq) const;
  /// An I (\a ti, \a vi) and its Q sampled once a \a period from \a
  /// offset (NaN: half a period in; a NaN period: every sample of I), from
  /// \a from on, and measured against \a modulation's ideal points
  /// (get_dataset's evm too).
  static Pair sampled(const QString& i, const QString& q, const QVector<double>& ti, const QVector<double>& vi,
                      const QVector<double>& tq, const QVector<double>& vq, double period, double offset, double from,
                      int modulation);

  void getAxisLimits(Graph*) override;
  int calcDiagram() override;
  bool takesLimits() const override { return false; }
  QList<Part> themeParts() const override;

protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void calcData(Graph*) override {}   // the points are painted
  void createAxisLabels() override;
  void paintBehindGraphs(QPainter*) override;
  void paintInFront(QPainter*, const Colors&) override;
  bool paintsGraphs() const override { return false; }

private:
  QList<Pair> m_pairs;
};

#endif
