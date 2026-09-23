/*
 * ngopt_sim.h - NgOpt: ngspice's own parameter optimizer (the `optimize`
 * command of ngspice builds that have it) as a simulation component
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef NGOPT_SIM_H
#define NGOPT_SIM_H

#include "simulation.h"

/*!
 * \brief The NgOpt component: its properties are the optimize command
 *        (qucs_s::ngopt::Command) - the method and its settings, the
 *        expression to minimize or the targets to fit, and a "Knob" for
 *        each parameter. The ngspice netlist runs it ahead of the
 *        schematic's simulations (Ngspice::createNetlist), which then see
 *        the optimum; the values found become the knobs' initial values
 *        (SimulationRun). Edited with NgOptDialog.
 */
class NgOpt_Sim : public qucs::component::SimulationComponent {
public:
  NgOpt_Sim();
  Component* newOne() override;
  static Element* info(QString&, char* &, bool getNewOne=false);

protected:
  QString spice_netlist(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault) override;
};

#endif
