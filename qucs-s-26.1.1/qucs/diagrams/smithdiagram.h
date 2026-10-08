/***************************************************************************
                               smithdiagram.h
                              ----------------
    begin                : Sat Oct 18 2003
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef SMITHDIAGRAM_H
#define SMITHDIAGRAM_H

#include "diagram.h"
#include "smithcircles.h"


class SmithDiagram : public Diagram  {
public: 
  SmithDiagram(int _cx=0, int _cy=0, bool ImpMode=true);
 ~SmithDiagram();


  Diagram* newOne();
  static Element* info(QString&, char* &, bool getNewOne=false);
  static Element* info_y(QString&, char* &, bool getNewOne=false);
  int  calcDiagram();
  void calcLimits();
  void calcCoordinate(const double*, const double*, const double*, float*, float*, Axis const*) const;
  QString extraMarkerText(Marker const*) const;
  QList<Part> themeParts() const override;

  /// Circles of the two-port the traces are of (smithcircles.h): its S
  /// (and noise) parameters read from their run at one frequency.
  enum CircleKind { InputStability = 0, OutputStability, Gain, Noise };
  static QStringList circleKindNames();
  struct CircleSpec {
    int kind = InputStability;
    double level = 0;          ///< a gain's or a noise figure's, dB
    bool operator==(const CircleSpec& o) const { return kind == o.kind && level == o.level; }
  };
  QList<CircleSpec> circles;
  /// The circles as a dialog writes them: "in, out, gain 12, noise 2" -
  /// false (and \a error why) for a text that is none.
  static bool circlesFromText(const QString& text, QList<CircleSpec>* out, QString* error);
  static QString circlesText(const QList<CircleSpec>& circles);
  double circleFrequency;      ///< NaN: the middle of the sweep
  struct DrawnCircle {
    CircleSpec spec;
    qucs_s::smith::Circle circle;
    QString label;
  };
  struct CircleSet {
    QString error;             ///< why there are none (no S-parameters, ...)
    double frequency = 0;
    qucs_s::smith::SParameters s;
    QList<DrawnCircle> drawn;
  };
  static QColor circleColour(int kind);
  /// The circles as last laid out.
  const CircleSet& circleSet() const { return m_circles; }
protected:
  QString extraSaveFields() const override;
  void loadExtraFields(const QStringList&) override;
  void paintBehindGraphs(QPainter*) override;
  void createAxisLabels() override;
  /// (A circle; its numbers inside it.)
  QPainterPath plotAreaShape() const override;
  bool numbersInside() const override { return true; }

private:
  void computeCircles();
  CircleSet m_circles;
};

#endif
