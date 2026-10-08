/***************************************************************************
                               diagram.h
                              -----------
    begin                : Thu Oct 2 2003
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

#ifndef DIAGRAM_H
#define DIAGRAM_H

#include "graph.h"
#include "element.h"
#include "numberformat.h"
#include "diagramtheme.h"
#include "speclimits.h"

#include <QTextStream>
#include <QFontMetricsF>
#include <QList>
#include <QPainterPath>

#include <algorithm>
#include <utility>
#include <cmath>
#include "qucs_assert.h"

#define MIN_SCROLLBAR_SIZE 8

#define INVALID_STR QObject::tr(" <invalid>")

// ------------------------------------------------------------
// Enlarge memory block if necessary.
#define  FIT_MEMORY_SIZE  \
  if(p >= p_end) {     \
    int pos = p - g->begin(); \
    QUCS_ASSERT(pos<Size); \
    Size += 256;        \
    g->resizeScrPoints(Size); \
    p = g->begin() + pos; \
    p_end = g->begin() + (Size - 9); \
  }

struct Axis {
  double  min, max; // least and greatest values of all graph data
  double  low, up;  // the limits of the diagram
  bool    log;      // in "rectdiagram": logarithmic or linear
  QString Label;
  int     numGraphs;  // counts number of graphs using this axis
  bool    GridOn;

  enum LogUnits { NoUnits = 0, dbUnits = 1, dBuVUnits = 2, dBmUnits = 3 };
  int Units;

  bool   autoScale;    // manual limits or auto-scale ?
  double limit_min, limit_max, step;   // if not auto-scale
};

struct MappedPoint {
  qreal x, y1, y2;
  int pane = -1;   // a stacked diagram's pane the point is in
};

namespace qucs {
double inline num2db(double zD, int unit) {
    double yVal = zD;
    switch (unit) {
    case Axis::NoUnits: yVal = zD;
        break;
    case Axis::dbUnits:
        yVal = 20*log10(zD);
        if (fabs(yVal) < 1e-3) yVal = 0;
        break;
    case Axis::dBuVUnits:
        yVal = 20*log10(zD/1e-6);
        if (fabs(yVal) < 1e-3) yVal = 0;
        break;
    case Axis::dBmUnits:
        yVal = 10*log10(zD/1e-3);
        if (fabs(yVal) < 1e-3) yVal = 0;
        break;
    default: yVal = zD;
    }
    return yVal;
}

double inline db2num(double zD, int unit) {
    double yVal = zD;
    switch (unit) {
    case Axis::NoUnits: yVal = zD;
        break;
    case Axis::dbUnits:
        yVal = pow(10.0,zD/20.0);
        break;
    case Axis::dBuVUnits:
        yVal = 1e-6*pow(10.0,zD/20.0);
        break;
    case Axis::dBmUnits:
        yVal = 1e-3*pow(10.0,zD/10.0);
        break;
    default: yVal = zD;
    }
    return yVal;
}

}

class Diagram : public Element {
public:
  using Part = qucs_s::diagramtheme::Part;
  using Theme = qucs_s::diagramtheme::Theme;
  using Colors = qucs_s::diagramtheme::Colors;

  // At most this many grid lines per axis: limits so close together (a
  // zoom rectangle a pixel wide) or so far apart that the step does not
  // move the next line made the grid loops run forever.
  static constexpr int MaxGridLines = 10000;
  // A grid position (pixels) from what the axis arithmetic computed: nan
  // (a collapsed axis, nan limits) is off the diagram, and inf or huge
  // values are clamped - converting either to int is undefined.
  static int gridPixel(double v)
  {
    if (std::isnan(v)) return -1;
    return static_cast<int>(std::clamp(v, -1e9, 1e9));
  }

  Diagram(int _cx=0, int _cy=0);
  virtual ~Diagram();

  virtual Diagram* newOne();
  virtual int  calcDiagram() { return 0; };
  virtual void calcCoordinate
               (const double*, const double*, const double*, float*, float*, Axis const*) const {};
  void calcCoordinateP (const double*x, const double*y, const double*z, Graph::iterator& p, Axis const* A) const;
  // TODO: Make pointToValue a pure virtual function.
  virtual MappedPoint pointToValue(const QPointF&) { return MappedPoint(); };
  virtual void setLimitsBySelectionRect(QRectF) {};
  /// Whether a box dragged on it zooms in (a Cartesian diagram's axes).
  virtual bool zoomsByRectangle() const { return false; }
  virtual void finishMarkerCoordinates(float&, float&) const;
  virtual void calcLimits() {};
  virtual QString extraMarkerText(Marker const*) const {return "";}
  
  virtual void paint(QPainter* p);
  virtual void paintDiagram(QPainter* painter);
  void paintMarkers(QPainter* p, bool paintAll = true);
  void    paintScheme(Schematic*) override;
  void    Bounding(int&, int&, int&, int&);
  QRect boundingRect() const noexcept override;
  /// All it draws, in the schematic's coordinates, its texts measured
  /// with \a metrics (those of the font it is drawn in). boundingRect()
  /// leaves out the numbers of its axes, which are centred on the edges
  /// of its frame. Its markers, which draw their own boxes, are not in it.
  virtual QRectF paintedRect(const QFontMetricsF& metrics) const;
  bool    getSelected(int, int);
  bool    resizeTouched(float, float, float);
  QString save();
  bool    load(const QString&, QTextStream*);

  virtual void getAxisLimits(Graph*);
  /// How many of \a g's points its log axes leave out: at or below 0 where
  /// the graph has points above (each drawn off the axis, the rest laid out).
  int leftOffLogAxis(const Graph* g) const;
  /// The y axis \a g is drawn against: the left one (yAxis) or the right
  /// one (zAxis), as its yAxisNo says - a pane's own in a stacked diagram.
  virtual const Axis* graphAxis(const Graph* g) const;
  Axis* graphAxis(const Graph* g) { return const_cast<Axis*>(std::as_const(*this).graphAxis(g)); }
  void updateGraphData();
  void loadGraphData(const QString&);
  void recalcGraphData();
  bool sameDependencies(Graph const*, Graph const*) const;
  int  checkColumnWidth(const QString&, const QFontMetrics&, int, int, int);

  virtual bool insideDiagram(float, float) const;
  bool insideDiagramP(Graph::iterator const& ) const;
  Marker* setMarker(int x, int y);
  /// Its markers, its graphs' in order: a delta marker's reference is told
  /// by its place in this list.
  QList<Marker*> markers() const;
  /// Before \a gone is deleted: the markers measuring from it measure
  /// from none, their texts made again.
  void forgetMarker(const Marker* gone);

  QString Name; // identity of diagram type (e.g. Polar), used for saving etc.
  QPen    GridPen;

  QList<Graph *>  Graphs;
  QList<qucs::Arc *>    Arcs;
  QList<qucs::Line *>   Lines;
  QList<Text *>   Texts;

  int x3, y3;
  Axis  xAxis, yAxis, zAxis;   // axes (x, y left, y right)
  /// Its title, drawn centred above its frame: part of the diagram, it is
  /// selected, moved and exported with it. Saved after the axes' labels,
  /// only when there is one (older versions read the line without it).
  QString title;
  QFont titleFont() const;
  /// The room above the frame the title takes (0 without one).
  int titleHeight() const;
  int State;  // to remember which resize area was touched
  // How the numbers on the axes, in the markers and in the cursor
  // readout are written, and with how many places after the point (-1:
  // as many as they need).
  qucs_s::numberformat::Notation notation;
  int notationDecimals;
  /// \a value as this diagram writes numbers; \a step, the distance
  /// between the labels of an axis, lines decimal labels up.
  QString numberText(double value, double step = 0.0) const
  {
      return qucs_s::numberformat::format(value, notation, notationDecimals, step);
  }
  /// ... on \a axis' ticks: in notationOf(axis).
  QString numberText(const Axis* axis, double value, double step = 0.0) const
  {
      return qucs_s::numberformat::format(value, notationOf(axis), notationDecimals, step);
  }
  /// The notation \a axis' numbers are written in: the diagram's (a
  /// bathtub's rates have an exponent when it is automatic).
  virtual qucs_s::numberformat::Notation notationOf(const Axis*) const { return notation; }

  /// Where the legend (a colour/style sample and the variable of every
  /// graph) is drawn: LegendOff, or a corner of the diagram.
  enum LegendPosition { LegendOff = 0, LegendTopLeft, LegendTopRight,
                        LegendBottomLeft, LegendBottomRight };
  int legendPos;
  void paintLegend(QPainter* painter, const Colors& colors);

  /// The colours of its parts (the dialog's Theme tab): its background,
  /// plot area, frame, grid, axes, title, legend. The grid's is GridPen's
  /// (automatic when it is the light grey of old). Saved with it, after
  /// the title, when one is chosen.
  Theme theme() const;
  void setTheme(const Theme& theme);
  /// Its parts' colours as they are drawn on the paper in use
  /// (ink::paper(): the canvas's, white on prints and exports).
  Colors colors() const;
  /// The parts it has, whose colours the Theme tab sets.
  virtual QList<Part> themeParts() const;
  /// Its background (behind all it draws, with a margin) and its plot
  /// area filled, in the schematic's coordinates.
  void paintBackground(QPainter* painter, const Colors& colors) const;

  /// Its spec limits (limits.h): the lines and masks its traces keep
  /// within, drawn in it, its traces red where beyond them, and a verdict.
  /// Saved after its graphs, a line each.
  QList<qucs_s::limits::Limit> limits;
  /// Where its traces are beyond its limits, as last laid out.
  const QList<qucs_s::limits::Violation>& violations() const { return a_violations; }
  /// Whether it takes limits (a Cartesian diagram, stacked panes).
  virtual bool takesLimits() const { return false; }
  /// The axis \a limit is drawn against: null when it has none such.
  virtual const Axis* limitAxis(const qucs_s::limits::Limit& limit) const
  {
    return limit.axis == 0 ? &yAxis : &zAxis;
  }

  // Whether updateGraphData() has laid the diagram out since it was made:
  // until then its axes and labels are the constructor's defaults.
  bool laidOut = false;

  bool hideLines;       // for "Rect3D": hide invisible lines ?
  int rotX, rotY, rotZ; // for "Rect3D": rotation around x, y and z axis

protected:
  /// What a diagram type saves beyond the common fields, after them and
  /// before the labels (" 12 0 3"), and reads back from those fields.
  /// The dataset it was last loaded from (its traces' "name.dat"; a
  /// simulator's adds its suffix): what more a diagram reads of the run.
  QString dataSetFile() const { return a_dataSet; }
  virtual QString extraSaveFields() const { return QString(); }
  virtual void loadExtraFields(const QStringList&) {}
  QString a_dataSet;
  /// Painted under the graphs, in their coordinates (origin at the lower
  /// left corner, y upwards), and over the axis texts, in the diagram's
  /// (y downwards), before the legend.
  virtual void paintBehindGraphs(QPainter*) {}
  /// Whether its graphs draw themselves (a pole-zero map draws its roots
  /// as symbols of its own, in paintBehindGraphs()).
  virtual bool paintsGraphs() const { return true; }
  virtual void paintInFront(QPainter*, const Colors&) {}
  /// The area inside its frame, in its coordinates (origin at the lower
  /// left corner, y downwards).
  virtual QPainterPath plotAreaShape() const;
  /// Whether the numbers of its y axes are inside its frame (a polar or
  /// Smith chart's), on the plot area.
  virtual bool numbersInside() const { return false; }
  /// \a item, drawn in the colour of \a part.
  template <class Item> static Item* as(Part part, Item* item)
  {
    item->part = static_cast<unsigned char>(part);
    return item;
  }
  /// The part an axis of its is (XAxis, YAxis, RightAxis).
  virtual Part partOf(const Axis* axis) const;
  /// Where \a text is drawn, in the diagram's coordinates (origin at the
  /// lower left corner, y downwards): from its baseline, turned.
  virtual QRectF textRect(const Text& text, const QFontMetricsF& metrics) const;

  void calcSmithAxisScale(Axis*, int&, int&);
  void createSmithChart(Axis*, int Mode=7);
  void calcPolarAxisScale(Axis*, double&, double&, double&);
  void createPolarDiagram(Axis*, int Mode=3);

  /// Whether \a g is drawn, from what calcDiagram() returned: a bit for
  /// each y axis whose scale is valid (1 the left, 2 the right).
  virtual bool drawsGraph(int valid, const Graph* g) const { return (valid & (g->yAxisNo + 1)) != 0; }
  bool logLeavesOut(const Graph* g, bool x) const;
  /// The axes besides x, y and z a diagram type has (a stacked diagram's
  /// panes'): their ranges cleared before the graphs' limits are taken,
  /// and set to [0, 1] after when no graph gave any.
  virtual void clearExtraRanges() {}
  virtual void settleExtraRanges() {}
  /// Graphs clipped to a band of the frame (bottom and top, y up) while
  /// they are traced: a stacked diagram's pane. Off: the whole frame.
  bool clipBand = false;
  float clipLow = 0, clipHigh = 0;

  bool calcAxisScale(Axis*, double&, double&, double&, double&, double);
  bool calcAxisLogScale(Axis*, int&, double&, double&, double&, int);
  bool calcYAxis(Axis*, int);
  virtual void createAxisLabels();

  int  regionCode(float, float) const;
  virtual void clip(Graph::iterator &) const;
  void rectClip(Graph::iterator &) const;

  virtual void calcData(Graph*);

  QTransform pointTransform; // Transform between Qucs-S logical coordinates and diagram (logical) point coordinates.
  QTransform valueTransform; // Transform between diagram point coordinates and diagram values.

  // What it draws beyond its frame (set in createAxisLabels()).
  int Bounding_x1, Bounding_x2, Bounding_y1, Bounding_y2;

  /// Its limits drawn (in its coordinates, y up), and its traces again in
  /// red where beyond them; the verdict in its corner (y down).
  void paintLimits(QPainter* painter);
  void paintVerdict(QPainter* painter);

private:
  Theme a_theme;
  QList<qucs_s::limits::Violation> a_violations;   // (the grid's colour is GridPen's, not in it)
};

#endif
