/*
 * potentiometer.cpp - device implementations for potentiometer module
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 */

#include "potentiometer.h"
#include "extsimkernels/spicecompat.h"
#include "misc.h"
#include "node.h"

potentiometer::potentiometer()
{
  Description = QObject::tr ("Potentiometer verilog device");
  Simulator = spicecompat::simAll;

  Props.append (new Property ("R_pot", "10k", true,
    QObject::tr ("nominal device resistance")
    +" ("+QObject::tr ("Ohm")+")"));
  Props.append (new Property ("Rotation", "120", true,
    QObject::tr ("shaft/wiper arm rotation")
    +" ("+QObject::tr ("degrees")+")"));
  Props.append (new Property ("Taper_Coeff", "0", false,
    QObject::tr ("resistive law taper coefficient")));
  Props.append (new Property ("LEVEL", "1", false,
    QObject::tr ("device type selector")+" [1, 2, 3]"));
  Props.append (new Property ("Max_Rotation", "240.0", true,
    QObject::tr ("maximum shaft/wiper rotation")
    +" ("+QObject::tr ("degrees")+")"));
  Props.append (new Property ("Conformity", "0.2", false,
    QObject::tr ("conformity error")
    +" ("+QObject::tr ("%")+")"));
  Props.append (new Property ("Linearity", "0.2", false,
    QObject::tr ("linearity error")
    +" ("+QObject::tr ("%")+")"));
  Props.append (new Property ("Contact_Res", "1", false,
    QObject::tr ("wiper arm contact resistance")
    +" ("+QObject::tr ("Ohm")+")"));
  // The temperature dependence is Qucsator's (not in the SPICE form).
  Props.append (new Property ("Temp_Coeff", "100", false,
    QObject::tr ("resistance temperature coefficient")
    +" ("+QObject::tr ("PPM/Celsius")+")", Property::Type::Value, spicecompat::simQucsator));
  Props.append (new Property ("Tnom", "26.85", false,
    QObject::tr ("parameter measurement temperature")
    +" ("+QObject::tr ("Celsius")+")", Property::Type::Value, spicecompat::simQucsator));
  Props.append (new Property ("Temp", "26.85", false,
    QObject::tr ("simulation temperature"), Property::Type::Value, spicecompat::simQucsator));

  createSymbol ();
  tx = x1 + 8;
  ty = y2 + 4;
  Model = "potentiometer";
  SpiceModel = "R";
  Name  = "POT";
}

Component * potentiometer::newOne()
{
  potentiometer * p = new potentiometer();
  p->Props.front()->Value = Props.front()->Value;
  p->recreate();
  return p;
}

Element * potentiometer::info(QString& Name, char * &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("Potentiometer");
  BitmapFile = (char *) "potentiometer";

  if(getNewOne) return new potentiometer();
  return 0;
}

void potentiometer::createSymbol()
{
  // frame
  Lines.append(new qucs::Line(-30,-13,-30, 10,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(-30, 10, 30, 10,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( 30, 10, 30,-13,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( 30,-13,-30,-13,QPen(Qt::darkBlue,2)));

  // resistor
  Lines.append(new qucs::Line(-40,  0, -25, 0,QPen(Qt::darkBlue,2, Qt::SolidLine, Qt::FlatCap)));
  Lines.append(new qucs::Line(-25,  0, -20,-5,QPen(Qt::darkBlue,2, Qt::SolidLine, Qt::RoundCap)));
  Lines.append(new qucs::Line(-20, -5, -15, 0,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(-15,  0, -10,-5,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(-10, -5, -5,  0,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( -5,  0,  0, -5,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(  0, -5,  5,  0,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(  5,  0, 10, -5,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( 10, -5, 15,  0,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( 15,  0, 20, -5,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( 20, -5, 25,  0,QPen(Qt::darkBlue,2, Qt::SolidLine, Qt::RoundCap)));
  Lines.append(new qucs::Line( 25,  0, 40,  0,QPen(Qt::darkBlue,2, Qt::SolidLine, Qt::FlatCap)));

  // arrow
  Lines.append(new qucs::Line( -4, -9,  0, -5,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(  4, -9,  0, -5,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(  0, -5,  0,-20,QPen(Qt::darkBlue,2)));

  Texts.append(new Text(-23,   0, QObject::tr("B"), Qt::black, 6.0, 1.0, 0.0));
  Texts.append(new Text( 18,   0, QObject::tr("T"), Qt::black, 6.0, 1.0, 0.0));

  Ports.append(new Port(-40,   0)); // B
  Ports.append(new Port(  0, -20)); // M
  Ports.append(new Port( 40,   0)); // T

  x1 = -40; y1 = -20;
  x2 =  40; y2 =  15;
}

QString potentiometer::spice_netlist(spicecompat::SpiceDialect dialect /* = spicecompat::SPICEDefault */)
{
    Q_UNUSED(dialect);

    // The Verilog-A model of qucsator (potentiometer.va): the wiper divides
    // R_pot by Rotation/Max_Rotation; the conformity and linearity errors
    // scale both parts, unless a taper (Taper_Coeff with LEVEL 2 or 3) puts
    // R_pot*Tpcoeff in parallel with the bottom (2) or the top (3) part;
    // Contact_Res sits between the wiper pin and the divider. The
    // temperature coefficient is not carried over.
    QString s;
    const auto v = [this](const char* name) { return spicecompat::normalize_value(getProperty(name)->Value); };
    const QString R = v("R_pot"), rot = v("Rotation"), maxRot = v("Max_Rotation");
    const QString conformity = v("Conformity"), linearity = v("Linearity"), taper = v("Taper_Coeff");
    QString pin1 = spicecompat::normalize_node_name(Ports.at(0)->Connection->Name);   // B
    QString pin2 = spicecompat::normalize_node_name(Ports.at(1)->Connection->Name);   // wiper
    QString pin3 = spicecompat::normalize_node_name(Ports.at(2)->Connection->Name);   // T

    double contact = 0.0, taperCoeff = 0.0, fac = 1.0;
    QString unit;
    misc::str2num(getProperty("Contact_Res")->Value, contact, unit, fac);
    contact *= fac;
    misc::str2num(getProperty("Taper_Coeff")->Value, taperCoeff, unit, fac);
    taperCoeff *= fac;
    const int level = getProperty("LEVEL")->Value.toInt();
    const bool tapered = taperCoeff != 0.0 && (level == 2 || level == 3);

    QString wiper = pin2;
    if (contact > 0.0) {
        wiper = QStringLiteral("_net_%1_w").arg(Name);
        s += QStringLiteral("R%1_c %2 %3 %4\n").arg(Name, pin2, wiper, v("Contact_Res"));
    }
    const QString angle = QStringLiteral("(%1)*3.14159265358979/180").arg(rot);
    const QString errorTerm = QStringLiteral("(1+((%1)+(%2)*sin(%3))/100)").arg(conformity, linearity, angle);
    const QString scale = tapered ? QStringLiteral("1") : errorTerm;
    s += QStringLiteral("R%1_1 %2 %3 R='(0.000001+(%4)/(%5))*(%6)*%7'\n").arg(Name, pin1, wiper, rot, maxRot, R, scale);
    s += QStringLiteral("R%1_2 %2 %3 R='(1.000001-(%4)/(%5))*(%6)*%7'\n").arg(Name, wiper, pin3, rot, maxRot, R, scale);
    if (tapered) {
        const QString tpcoeff = QStringLiteral("((%1)+((%2)+(%3)*sin(%4))/100)").arg(taper, conformity, linearity, angle);
        if (level == 2)
            s += QStringLiteral("R%1_tb %2 %3 R='(%4)*%5'\n").arg(Name, pin1, wiper, R, tpcoeff);
        else
            s += QStringLiteral("R%1_tt %2 %3 R='(%4)*%5'\n").arg(Name, wiper, pin3, R, tpcoeff);
    }
    return s;
}
