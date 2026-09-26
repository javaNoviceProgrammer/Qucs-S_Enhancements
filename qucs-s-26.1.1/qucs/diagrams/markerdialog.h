/***************************************************************************
                               markerdialog.h
                              ----------------
    begin                : Wed April 21 2004
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

#ifndef MARKERDIALOG_H
#define MARKERDIALOG_H
#include "marker.h"
#include <QDialog>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class QToolButton;

class MarkerDialog : public QDialog  {
Q_OBJECT
public:
  MarkerDialog(Marker *pm_, QWidget *parent=0);
 ~MarkerDialog();

  /// The colours chosen for the text and the background (invalid:
  /// automatic, the paper's), as the dialog shows them.
  void setTextColor(const QColor& color);
  void setFillColor(const QColor& color);
  QColor textColor() const { return a_textColor; }
  QColor fillColor() const { return a_fillColor; }

private slots:
  void slotAcceptValues();

private:
  void showColors();
  QColor a_textColor, a_fillColor;

public:
  Marker *pMarker;

  QComboBox  *NumberBox;
  QLineEdit  *Precision;
  QLineEdit  *XPosition;
  QComboBox  *IndicatorBox;
  QLineEdit  *SourceImpedance;
  QCheckBox  *TransBox;
  QPushButton *TextColorButton, *FillColorButton;   // a colour chosen
  QToolButton *TextColorAuto, *FillColorAuto;       // back to automatic
};

#endif
