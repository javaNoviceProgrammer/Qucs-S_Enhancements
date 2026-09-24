/*
 * ngsweep_sim.h - NgSweep: ngspice's own parametric sweep (the `sweep`
 * command of ngspice builds that have it) as a simulation component
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef NGSWEEP_SIM_H
#define NGSWEEP_SIM_H

#include "simulation.h"

/*!
 * \brief The NgSweep component: its properties are the sweep command
 *        (qucs_s::ngsweep::Sweep) - the analysis, the parameter swept and
 *        its values, outer parameters for a family, and the values
 *        recorded at every point. The ngspice netlist runs it after the
 *        schematic's simulations (Ngspice::createNetlist); every point's
 *        voltages and currents land in the dataset as families of curves,
 *        the recorded values against the parameter. Edited with
 *        NgSweepDialog.
 */
class NgSweep_Sim : public qucs::component::SimulationComponent {
public:
  NgSweep_Sim();
  Component* newOne() override;
  static Element* info(QString&, char* &, bool getNewOne=false);

protected:
  QString spice_netlist(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault) override;
};

#endif
