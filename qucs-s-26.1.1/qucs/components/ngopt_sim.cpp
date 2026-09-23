/*
 * ngopt_sim.cpp - NgOpt: ngspice's own parameter optimizer as a simulation
 * component (see ngopt_sim.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngopt_sim.h"

#include "ngoptimize.h"

NgOpt_Sim::NgOpt_Sim()
{
  Description = QObject::tr("ngspice optimize");
  Simulator = spicecompat::simNgspice;
  initSymbol(Description);
  Model = ".NGOPT";
  SpiceModel = ".NGOPT";   // the netlist has it (Ngspice::createNetlist)
  Name  = "NgOpt";

  // The method and an empty objective; the dialog adds the knobs and
  // targets. The properties are saved with their names (no description),
  // so the list can grow.
  qucs_s::ngopt::Command command;
  command.write(this);
}

Component* NgOpt_Sim::newOne()
{
  return new NgOpt_Sim();
}

Element* NgOpt_Sim::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("ngspice optimize");
  BitmapFile = (char *) "ngopt";

  if(getNewOne)  return new NgOpt_Sim();
  return 0;
}

QString NgOpt_Sim::spice_netlist(spicecompat::SpiceDialect)
{
  // Not a line of the netlist proper: the optimize command goes into the
  // .control section ahead of the simulations (Ngspice::createNetlist).
  return QString();
}
