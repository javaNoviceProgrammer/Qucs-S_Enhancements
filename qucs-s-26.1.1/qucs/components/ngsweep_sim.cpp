/*
 * ngsweep_sim.cpp - NgSweep: ngspice's own parametric sweep as a
 * simulation component (see ngsweep_sim.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngsweep_sim.h"

#include "ngsweep.h"

NgSweep_Sim::NgSweep_Sim()
{
  Description = QObject::tr("ngspice sweep");
  Simulator = spicecompat::simNgspice;
  initSymbol(Description);
  Model = qucs_s::ngsweep::kModel;
  SpiceModel = qucs_s::ngsweep::kModel;   // the netlist has it (Ngspice::createNetlist)
  Name  = "NgSweep";
  // A simulation of its own: an op sweep needs no other in the schematic.
  isSimulation = true;

  // R1 over a decade, each point's waveforms kept; the dialog adds the
  // analysis.
  qucs_s::ngsweep::Sweep command;
  command.knob.name = "R1";
  command.knob.type = "lin";
  command.knob.start = "1k";
  command.knob.stop = "10k";
  command.knob.points = "5";
  command.write(this);
}

Component* NgSweep_Sim::newOne()
{
  return new NgSweep_Sim();
}

Element* NgSweep_Sim::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("ngspice sweep");
  BitmapFile = (char *) "ngsweep";

  if(getNewOne)  return new NgSweep_Sim();
  return 0;
}

QString NgSweep_Sim::spice_netlist(spicecompat::SpiceDialect)
{
  // Not a line of the netlist proper: the sweep command goes into the
  // .control section after the simulations (Ngspice::createNetlist).
  return QString();
}
