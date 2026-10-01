/*
 * vPRBS.cpp - a pseudo-random bit sequence voltage source (see vPRBS.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "vPRBS.h"
#include "node.h"
#include "extsimkernels/spicecompat.h"


vPRBS::vPRBS()
{
  Description = QObject::tr("pseudo-random bit sequence voltage source (ngspice PRBS, PAM4)");
  Simulator = spicecompat::simNgspice;

  // A voltage source with a bit stream in it.
  Arcs.append(new qucs::Arc(-12,-12, 24, 24,     0, 16*360,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(-30,  0,-12,  0,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( 30,  0, 12,  0,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( 18,  5, 18, 11,QPen(Qt::red,1)));
  Lines.append(new qucs::Line( 21,  8, 15,  8,QPen(Qt::red,1)));
  Lines.append(new qucs::Line(-18,  5,-18, 11,QPen(Qt::black,1)));

  // 0 1 0 1 1, across the source (it is turned upright below).
  Lines.append(new qucs::Line( -4, -8, -4, -4,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( -4, -4,  4, -4,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(  4, -4,  4, -1,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(  4, -1, -4, -1,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( -4, -1, -4,  2,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line( -4,  2,  4,  2,QPen(Qt::darkBlue,2)));
  Lines.append(new qucs::Line(  4,  2,  4,  8,QPen(Qt::darkBlue,2)));

  Ports.append(new Port( 30,  0));
  Ports.append(new Port(-30,  0));

  x1 = -30; y1 = -14;
  x2 =  30; y2 =  14;

  tx = x1+4;
  ty = y2+4;
  Model = "vPRBS";
  SpiceModel = "V";
  Name  = "V";

  Props.append(new Property("U1", "0 V", true,
		QObject::tr("voltage of a 0 bit (PAM4: of the lowest level)")));
  Props.append(new Property("U2", "1 V", true,
		QObject::tr("voltage of a 1 bit (PAM4: of the highest level)")));
  Props.append(new Property("Tbit", "1 ns", true,
		QObject::tr("bit period, the unit interval (PAM4: a symbol's)")));
  Props.append(new Property("Td", "0", false,
		QObject::tr("delay before the first bit; U1 until then")));
  Props.append(new Property("Tr", "0", false,
		QObject::tr("rise time of an edge, at most Tbit (0: the time step)")));
  Props.append(new Property("Tf", "0", false,
		QObject::tr("fall time of an edge, at most Tbit (0: the time step)")));
  Props.append(new Property("Order", "7", true,
		QObject::tr("length n of the shift register, 2 to 31 (PRBS7, PRBS15, PRBS31...): "
		            "the sequence repeats every 2^n-1 bits")));
  Props.append(new Property("Seed", "", false,
		QObject::tr("initial contents of the register, a positive whole number "
		            "(empty: all ones)")));
  Props.append(new Property("Coding", "NRZ", false,
		QObject::tr("NRZ: a level per bit; PAM4: two bits a symbol, Gray coded on four "
		            "levels from U1 to U2 (order 13: PRBS13Q)") + " [NRZ, PAM4]"));

  rotate();  // fix historical flaw
}

vPRBS::~vPRBS()
{
}

Component* vPRBS::newOne()
{
  return new vPRBS();
}

Element* vPRBS::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("V(PRBS)");
  BitmapFile = (char *) "vPRBS";

  if(getNewOne)  return new vPRBS();
  return 0;
}

QString vPRBS::netlist()
{
    return QString();
}

QString vPRBS::spice_netlist(spicecompat::SpiceDialect dialect /* = spicecompat::SPICEDefault */)
{
    Q_UNUSED(dialect);

    QString s = spicecompat::check_refdes(Name,SpiceModel);
    for (Port *p1 : Ports) {
        QString nam = p1->Connection->Name;
        if (nam=="gnd") nam = "0";
        s += " "+ nam;   // node names
    }

    // Empty is ngspice's default: no delay, edges of the time step.
    auto value = [this](const char* name, const char* empty) {
        const QString v = getProperty(name)->Value.trimmed();
        return v.isEmpty() ? QString::fromLatin1(empty) : spicecompat::normalize_value(v);
    };
    const QString keyword = getProperty("Coding")->Value.trimmed().compare("PAM4", Qt::CaseInsensitive) == 0
                                ? QStringLiteral("PAM4") : QStringLiteral("PRBS");
    QString args = QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
                       .arg(value("U1", "0"), value("U2", "1"), value("Tbit", "1n"), value("Td", "0"),
                            value("Tr", "0"), value("Tf", "0"), value("Order", "7"));
    // The seed last: left out, the register starts all ones.
    const QString seed = getProperty("Seed")->Value.trimmed();
    if (!seed.isEmpty()) args += " " + spicecompat::normalize_value(seed);

    // No DC value: the operating point is U1, where the bits start.
    s += QStringLiteral(" %1(%2)\n").arg(keyword, args);
    return s;
}
