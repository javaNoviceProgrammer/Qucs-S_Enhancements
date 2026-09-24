/***************************************************************************
                                 graph.h
                                ---------
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

#ifndef GRAPH_H
#define GRAPH_H


#include "marker.h"
#include "element.h"

#include <cmath>
#include <QColor>
#include <QDateTime>
#include <QPolygonF>
#include "qucs_assert.h"


typedef enum{
  GRAPHSTYLE_INVALID = -1,
  GRAPHSTYLE_SOLID = 0,
  GRAPHSTYLE_DASH,
  GRAPHSTYLE_DOT,
  GRAPHSTYLE_LONGDASH,
  GRAPHSTYLE_STAR,
  GRAPHSTYLE_CIRCLE,
  GRAPHSTYLE_ARROW,
  GRAPHSTYLE_COUNT,
} graphstyle_t;

inline graphstyle_t toGraphStyle(int x){
  if (x<0){
    return GRAPHSTYLE_INVALID;
  }else if(x<GRAPHSTYLE_COUNT){
    return graphstyle_t(x);
  }else{
    return GRAPHSTYLE_INVALID;
  }
}

class Diagram;


struct DataX {
  DataX(const QString& Var_, double *Points_=0, int count_=0)
       : Var(Var_), Points(Points_), count(count_), Min(INFINITY), Max(-INFINITY) {};
 ~DataX() { if(Points) delete[] Points; };
  QString Var;
  double *Points;
  int     count;

public:
  const double& min()const {return Min;}
  const double& max()const {return Max;}
public: // only called from Graph. cleanup later.
  const double& min(const double& x){if (Min<x) Min=x; return Min;}
  const double& max(const double& x){if (Max>x) Max=x; return Max;}
private:
  double Min;
  double Max;
};

struct Axis;

/*!
 * prepare data for plotting purposes in Diagram.
 * a Graph is a list of graphs (bug?!)
 * iterating yields points (in screen coordinates) and control tokens.
 *
 * also stores markers.
 */
class Graph : public Element {
public:
  Graph(const Diagram*, const QString& _Line="");
 ~Graph();

  enum PtType { NotDrawn = 0, DataPt = 1, STROKEEND = 2, BRANCHEND = 3, GRAPHEND = 4};

  class ScrPt{
  private:
    float ScrX;
    float ScrY;
    PtType type;

    double indep; // top level indep value (sweep)
    double dep; // top level dep value // FIXME: type?!
  public:
    ScrPt() { ScrX = 0; ScrY = 0;
              type = NotDrawn;
              dep = 0.0; indep = 0.0; }
    ~ScrPt(){}

    void setStrokeEnd();
    void setBranchEnd();
    void setGraphEnd();
    void setScrX(float); // screen horizontal coordinate
    void setScrY(float); // screen vertical coordinate
    void setScr(float,float); // both @ once.
    void setIndep(double);
    void setDep(double);
//    void attachCoords(double*);

    bool isPt() const; // indicate if this is a point on the screen
    bool isStrokeEnd() const;
    bool isBranchEnd() const;
    bool isGraphEnd() const;

    float getScrX() const;
    float getScrY() const;
    double getIndep() const;
    double getDep() const;
  };
  typedef std::vector<ScrPt> container;
  typedef container::iterator iterator;
  typedef container::const_iterator const_iterator;

  int loadDatFile(const QString& filename);
  int loadIndepVarData(const QString&, char* datfilecontent, DataX* where);

  void    paint(QPainter* painter);
  void    paintLines(QPainter* painter);
  QString save();
  bool    load(const QString&);
  int     getSelected(int, int);
  Graph*  sameNewOne();
  
private: // tmp hack
  DataX* mutable_axis(uint i) { if(i<(uint)cPointsX.size()) return cPointsX.at(i); return nullptr;}
public:
  unsigned numAxes() const { return cPointsX.size(); }
  DataX const* axis(uint i) const { if(i<(uint)cPointsX.size()) return cPointsX.at(i); return nullptr;}
  size_t count(uint i) const { if(axis(i)) return axis(i)->count; return 0; }
  QString axisName(unsigned i) const {if(axis(i))return axis(i)->Var; return "";}
  bool isEmpty() const { return !cPointsX.size(); }
  QVector<DataX*>& mutable_axes(){return cPointsX;} // HACK

  void clear(){ScrPoints.resize(0);}
  // Drops everything that came from a dataset (axes, dependent values, curve
  // count). Every failure path of loadDatFile() goes through this so that a
  // partial load never leaves stale pointers for getAxisLimits()/calcData().
  void clearData();
  void resizeScrPoints(size_t s){QUCS_ASSERT(s>=ScrPoints.size()); ScrPoints.resize(s); linesInvalidate();}
  iterator begin(){return ScrPoints.begin();}
  iterator end(){return ScrPoints.end();}
  const_iterator begin() const{return ScrPoints.begin();}
  const_iterator end() const{return ScrPoints.end();}

  QDateTime lastLoaded;  // when it was loaded into memory
  int     yAxisNo;       // which y axis is used
  double *cPointsY;
  int     countY;    // number of curves
  QString Var;
  QColor  Color;
  int     Thick;
  graphstyle_t Style;
  QList<Marker *> Markers;

  // for tabular diagram
  int  Precision;   // number of digits to show
  int  numMode;     // real/imag or polar (deg/rad)

  /// Auto colors: each curve of the graph - a value of a parameter swept
  /// - in a color of its own, from autoPalette() in its order, the
  /// graph's own Color aside. The auto graphs of a diagram share the
  /// colors: the next one goes on where the one before left off. Past the
  /// palette the colors come round again with the next line style.
  bool autoColor = false;
  /// The eight colors, in the order they are given (a categorical palette
  /// checked for color vision deficiency and on white paper).
  static const QList<QColor>& autoPalette();
  /// Whether a diagram of this kind draws curves that auto colors apply
  /// to (Rect, Polar, Smith, the polar-Smith ones, Curve).
  static bool autoColorApplies(const QString& diagramName);
  /// Whether this graph's curves are drawn each in a color of its own.
  bool colorsEachCurve() const;
  /// The color and the line style curve \a curve (0 to countY - 1) is
  /// drawn in: the graph's own when it does not color each curve.
  QColor curveColor(int curve) const;
  graphstyle_t curveStyle(int curve) const;
  /// The value of each swept parameter at curve \a curve: "r1=2k, c1=100n";
  /// empty when the graph has one curve.
  QString curveLabel(int curve) const;

private: // painting
  void drawStarSymbols(QPainter* painter) const;
  void drawLines(QPainter* painter) const;
  void drawCircleSymbols(QPainter* painter) const;
  void drawArrowSymbols(QPainter* painter) const;
public: // marker related
  void createMarkerText() const;
  std::pair<double,double> findSample(std::vector<double>&) const;
  Diagram const* parentDiagram() const{return diagram;}
private:
  int curveOffset() const;   // the curves of the diagram's auto graphs before this one
  QVector<DataX*>  cPointsX;
  std::vector<ScrPt> ScrPoints; // data in screen coordinates
  Diagram const* diagram;

  mutable QList<QLineF> lines;
  // The same points as one polyline per stroke: a dash pattern runs on
  // along a polyline, while every line of drawLines() starts it afresh.
  mutable QList<QPolygonF> strokes;
  mutable QList<int> lineCurves;     // the curve (branch) of each line
  mutable QList<int> strokeCurves;   // and of each stroke
  mutable QDateTime     linesCalculated;
  void linesInvalidate() {linesCalculated = QDateTime();} //Set to 'null' date
};

#endif

// vim:ts=8:sw=2:noet
