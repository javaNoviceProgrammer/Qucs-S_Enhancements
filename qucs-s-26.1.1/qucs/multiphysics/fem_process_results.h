/*
 * fem_process_results.h - a process flow's step as results: its state as a
 *                         solution to plot and evaluate (its dopants'
 *                         variables), the values derived down a column -
 *                         junction depth, sheet resistance, oxide
 *                         thickness, each species' retained dose
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_PROCESS_RESULTS_H
#define QUCS_FEM_PROCESS_RESULTS_H

#include "fem_solver.h"

#include <optional>

namespace qucs_s::fem {

/// The process flow node \a tag is (a flow) or is a step of; null if none.
const Node* processFlowOf(const Model& model, const QString& tag);

/// What a step leaves down the column at x (the geometry's unit): the
/// first junction below the top semiconductor's surface (m, NaN: none), the
/// sheet resistance of the layer above it (Ω/sq, Masetti's silicon
/// mobilities; NaN: none), the oxide over that semiconductor (m), each
/// species' dose kept over the window (m⁻²).
struct ProcessColumn {
    double x = 0;
    double junction = std::numeric_limits<double>::quiet_NaN();
    double sheet = std::numeric_limits<double>::quiet_NaN();
    double oxide = 0;
    QString surface;   ///< the top semiconductor's material ("": none)
    std::vector<std::pair<QString, double>> doses;
};
ProcessColumn processColumn(const ProcessState& state, double x);

/*!
 * Step \a step's state (a flow's tag: its last step's) as a solution: the
 * geometry built up to it, meshed as the model's mesh says, the flow's
 * variables on it (Solution::addProcessVariables: pf1.B, pf1.net...), its
 * globals the values derived down the column at \a x (the window's middle
 * when none): pf1.xj, pf1.Rs, pf1.tox, pf1.dose_B... Null, and why.
 * \a progress is told how far - the flow, then the mesh -: false stops it.
 */
std::shared_ptr<Solution> processSolution(const Model& model, std::shared_ptr<ParameterScope> parameters, const QString& step,
                                          std::optional<double> x, QString* error,
                                          const std::function<bool(double fraction, const QString& what)>& progress = {});
/// A process solution's column values found again at \a x (the window's
/// middle when none): its globals pf1.x, pf1.xj, pf1.Rs... - one solution
/// for every x.
void setProcessColumn(Solution& solution, std::optional<double> x);

} // namespace qucs_s::fem

#endif // QUCS_FEM_PROCESS_RESULTS_H
