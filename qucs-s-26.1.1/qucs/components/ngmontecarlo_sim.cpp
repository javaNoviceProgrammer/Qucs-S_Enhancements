/*
 * ngmontecarlo_sim.cpp - NgMonteCarlo: ngspice's own Monte Carlo loop as a
 * simulation component (see ngmontecarlo_sim.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ngmontecarlo_sim.h"

#include "ngstatistics.h"

NgMonteCarlo_Sim::NgMonteCarlo_Sim()
{
  Description = QObject::tr("ngspice Monte Carlo");
  Simulator = spicecompat::simNgspice;
  initSymbol(Description);
  Model = qucs_s::ngstats::kMonteCarloModel;
  SpiceModel = qucs_s::ngstats::kMonteCarloModel;   // the netlist has it (Ngspice::createNetlist)
  Name  = "NgMonteCarlo";

  // 100 samples and nothing to record yet; the dialog adds the values and
  // specs. The properties are saved with their names, so the list can grow.
  qucs_s::ngstats::MonteCarlo command;
  command.write(this);
}

Component* NgMonteCarlo_Sim::newOne()
{
  return new NgMonteCarlo_Sim();
}

Element* NgMonteCarlo_Sim::info(QString& Name, char* &BitmapFile, bool getNewOne)
{
  Name = QObject::tr("ngspice Monte Carlo");
  BitmapFile = (char *) "ngmontecarlo";

  if(getNewOne)  return new NgMonteCarlo_Sim();
  return 0;
}

QString NgMonteCarlo_Sim::spice_netlist(spicecompat::SpiceDialect)
{
  // Not a line of the netlist proper: the montecarlo command goes into the
  // .control section after the simulations (Ngspice::createNetlist).
  return QString();
}
