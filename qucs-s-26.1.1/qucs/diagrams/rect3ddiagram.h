/***************************************************************************
                              rect3ddiagram.h
                             -----------------
    begin                : Sat Mar 5 2005
    copyright            : (C) 2005 by Michael Margraf
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

#ifndef RECT3DDIAGRAM_H
#define RECT3DDIAGRAM_H

#include "diagram.h"


struct tPoint3D {
  int   x, y;
  int   No, done;
};

struct tPointZ {
  float z;
  int   No, NoCross;
  int   dx, dy;   // its graph's points a row and rows a plane (each cell walked with its own graph's)
};

struct tBound {
  int min, max;
};


class Rect3DDiagram : public Diagram  {
public:
  Rect3DDiagram(int _cx=0, int _cy=0);
 ~Rect3DDiagram();


  Diagram* newOne();
  static Element* info(QString&, char* &, bool getNewOne=false);
  int  calcDiagram();
  void calcLimits();
  void calcCoordinate(const double*, const double*, const double*, float*, float*, Axis const*) const;
  void finishMarkerCoordinates(float&, float&) const;

  void createAxisLabels();
  bool insideDiagram(float, float) const;
  /// (No plot area: a box in space, its grid on its back walls.)
  QList<Part> themeParts() const override;

  tPoint3D *Mem;   // memory for all points during hidden line algorithm
  tPoint3D *pMem;  // current position in "Mem"


protected:
  void calcData(Graph*);

private:
  int  calcAxis(Axis*, int, int, double, double);
  void createAxis(Axis*, bool, int, int, int, int);

  void   calcCoefficients();
  int    calcCross(int*, int*);
  double calcX_2D(double, double, double) const;
  double calcY_2D(double, double, double) const;
  double calcZ_2D(double, double, double) const;

  static int comparePointZ(const void*, const void*);
  bool isHidden(int, int, tBound*, char*);
  void enlargeMemoryBlock(tPoint3D* &);
  void calcLine(tPoint3D* &, tPoint3D* &, tBound*, char*);
  void calcCoordinate3D(double, double, double, double, tPoint3D*, tPointZ*);
  void removeHiddenLines(char*, tBound*);
  void removeHiddenCross(int, int, int, int, char*, tBound*, Part, const QPen&, QList<qucs::Line*>&);
  void createGrid(int corner, char* zBuffer, tBound* Bounds);

  float  xorig, yorig; // where is the 3D origin with respect to cx/cy
  double cxx, cxy, cxz, cyx, cyy, cyz, czx, czy, czz; // coefficients 3D -> 2D
  double scaleX, scaleY;
  /// Where each axis's numbers are (x, y, z), from 0 to 1 along it, as the
  /// points are (calcAxis()): the grid's lines go there.
  QVector<double> ticks[3];
};

#endif
