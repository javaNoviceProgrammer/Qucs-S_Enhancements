/*
 * ngmontecarlo_sim.h - NgMonteCarlo: ngspice's own Monte Carlo loop (the
 * `montecarlo` command of ngspice builds that have it) as a simulation
 * component
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef NGMONTECARLO_SIM_H
#define NGMONTECARLO_SIM_H

#include "simulation.h"

/*!
 * \brief The NgMonteCarlo component: its properties are the montecarlo
 *        command (qucs_s::ngstats::MonteCarlo) - the samples, the analysis
 *        each one runs, the values recorded and the specs judged. The
 *        ngspice netlist runs it after the schematic's simulations
 *        (Ngspice::createNetlist); every recorded value lands in the
 *        dataset against the sample (a waveform as a family), with its
 *        histogram, and the yield in the status log. Edited with
 *        NgStatisticsDialog.
 */
class NgMonteCarlo_Sim : public qucs::component::SimulationComponent {
public:
  NgMonteCarlo_Sim();
  Component* newOne() override;
  static Element* info(QString&, char* &, bool getNewOne=false);

protected:
  QString spice_netlist(spicecompat::SpiceDialect dialect = spicecompat::SPICEDefault) override;
};

#endif
