/***************************************************************************
                          equation.cpp  -  description
                             -------------------
    begin                : Sat Aug 23 2003
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
#include "sp_options.h"
#include "main.h"

#include <QFontInfo>
#include <QFontMetrics>

SpiceOptions::SpiceOptions()
{
  isEquation = true;
  Type = isComponent; // Analogue and digital component.
  Description = QObject::tr(".OPTIONS section");
  Simulator = spicecompat::simSpice;

  QFont f = QucsSettings.font;
  f.setWeight(QFont::Light);
  f.setPointSizeF(12.0);
  QFontMetrics  metrics(f, 0);  // use the the screen-compatible metric
  QSize r = metrics.size(0, QObject::tr(".OPTIONS"));
  int xb = r.width()  >> 1;
  int yb = r.height() >> 1;

  Lines.append(new qucs::Line(-xb, -yb, -xb,  yb,QPen(Qt::darkRed,2)));
  Lines.append(new qucs::Line(-xb,  yb,  xb+3,yb,QPen(Qt::darkRed,2)));
  Texts.append(new Text(-xb+4,  -yb-3, QObject::tr(".OPTIONS"),
			QColor(0,0,0), QFontInfo(f).pixelSize()));

  x1 = -xb-3;  y1 = -yb-5;
  x2 =  xb+9; y2 =  yb+3;

  tx = x1+4;
  ty = y2+4;
  Model = "SpiceOptions";
  Name  = "SpiceOptions";

  Props.append(new Property("XyceOptionPackage", "DEVICE", false,
        QObject::tr("Xyce option package name")));
  Props.append(new Property("GMIN", "1e-12", true));
}

SpiceOptions::~SpiceOptions()
{
}

Component* SpiceOptions::newOne()
{
  return new SpiceOptions();
}

Element* SpiceOptions::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr(".OPTIONS Section");
  BitmapFile = (char *) "sp_options";

  if(getNewOne)  return new SpiceOptions();
  return 0;
}

bool SpiceOptions::load(const QString& s)
{
  if (!Component::load(s)) return false;
  // The first value is the package name; one with "=" in it is an option
  // that a file saved without the package line put there.
  if (!Props.isEmpty() && Props.first()->Name == QLatin1String("XyceOptionPackage")
      && Props.first()->Value.contains(QLatin1Char('='))) {
    const QString option = Props.first()->Value;
    Props.first()->Value = QStringLiteral("DEVICE");
    Props.insert(1, new Property(option.section('=', 0, 0).trimmed(), option.section('=', 1).trimmed(), true));
  }
  return true;
}

QString SpiceOptions::getExpression(spicecompat::SpiceDialect dialect /* = spicecompat::SPICEDefault */)
{
    if (isActive != COMP_IS_ACTIVE || dialect == spicecompat::CDL) return QString();

    // The Xyce option package is the property of that name, wherever it
    // is - the equation editor rebuilds the list from its lines, so it is
    // not always the first one (it used to be taken as the first, and the
    // first option line was then dropped from the netlist).
    QString package = QStringLiteral("DEVICE");
    QList<Property*> options;
    for (Property* p : Props) {
        if (p->Name == QLatin1String("XyceOptionPackage")) {
            if (!p->Value.trimmed().isEmpty()) package = p->Value.trimmed();
            continue;
        }
        options.append(p);
    }

    // An option with no value is a flag (ngspice has a good many:
    // noopiter, notrnoise, keepopinfo, ...) and is written on its own.
    QString s;
    if (dialect == spicecompat::SPICEXyce) {
        s += QStringLiteral(".OPTIONS %1 ").arg(package);
        for (Property* p : options) {
            if (p->Value.trimmed().isEmpty()) s += QStringLiteral(" %1 ").arg(p->Name);
            else s += QStringLiteral(" %1 = %2 ").arg(p->Name).arg(p->Value);
        }
        s += "\n";
    } else {
        for (Property* p : options) {
            if (p->Value.trimmed().isEmpty()) s += QStringLiteral(".OPTION %1\n").arg(p->Name);
            else s += QStringLiteral(".OPTION %1 = %2\n").arg(p->Name).arg(p->Value);
        }
    }
    return s;
}

