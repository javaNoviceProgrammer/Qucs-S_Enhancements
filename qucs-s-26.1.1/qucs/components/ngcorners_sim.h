/*
 * ngcorners_sim.h - NgCorners: ngspice's own process-corner loop (the
 * `corners` command of ngspice builds that have it) as a simulation
 * component
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef NGCORNERS_SIM_H
#define NGCORNERS_SIM_H

#include "simulation.h"

/*!
 * \brief The NgCorners component: its properties are the corners command
 *        (qucs_s::ngstats::Corners) - the analysis, the corners the
 *        Verilog-A models declare to run it at, the values recorded at
 *        each, or a Monte Carlo with its specs at each. The ngspice netlist
 *        runs it after the schematic's simulations (Ngspice::createNetlist);
 *        the values land in the dataset against the corner, the waveforms
 *        as a family, and the corners' names and values in the status log.
 *        Edited with NgStatisticsDialog.
 */
class NgCorners_Sim : public qucs::component::SimulationComponent {
public:
  NgCorners_Sim();
  Component* newOne() override;
  static Element* info(QString&, char* &, bool getNewOne=false);

protected:
  QString spice_netlist(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault) override;
};

#endif
