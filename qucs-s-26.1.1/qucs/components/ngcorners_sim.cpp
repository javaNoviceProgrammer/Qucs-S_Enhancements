/*
 * ngcorners_sim.cpp - NgCorners: ngspice's own process-corner loop as a
 * simulation component (see ngcorners_sim.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngcorners_sim.h"

#include "ngstatistics.h"

NgCorners_Sim::NgCorners_Sim()
{
  Description = QObject::tr("ngspice corners");
  Simulator = spicecompat::simNgspice;
  initSymbol(Description);
  Model = qucs_s::ngstats::kCornersModel;
  SpiceModel = qucs_s::ngstats::kCornersModel;   // the netlist has it (Ngspice::createNetlist)
  Name  = "NgCorners";

  // Every declared corner, the nominal first, and their waveforms; the
  // dialog adds the analysis and the values.
  qucs_s::ngstats::Corners command;
  command.write(this);
}

Component* NgCorners_Sim::newOne()
{
  return new NgCorners_Sim();
}

Element* NgCorners_Sim::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("ngspice corners");
  BitmapFile = (char *) "ngcorners";

  if(getNewOne)  return new NgCorners_Sim();
  return 0;
}

QString NgCorners_Sim::spice_netlist(spicecompat::SpiceDialect)
{
  // Not a line of the netlist proper: the corners command goes into the
  // .control section after the simulations (Ngspice::createNetlist).
  return QString();
}
