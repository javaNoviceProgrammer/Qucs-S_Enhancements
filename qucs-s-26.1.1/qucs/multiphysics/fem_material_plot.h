/*
 * fem_material_plot.h - a material's properties along one variable (the
 *                       temperature, the doping, an alloy's composition, the
 *                       wavelength, the field), as its physics take them:
 *                       Lumerical's material visualizer's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_MATERIAL_PLOT_H
#define QUCS_FEM_MATERIAL_PLOT_H

#include "fem_model.h"

#include <QString>
#include <QStringList>

#include <limits>
#include <vector>

namespace qucs_s::fem {

/// One variable swept, the others held: the temperature T (K), the doping N
/// (the donors and acceptors together, 1/m³), an alloy's composition x (its
/// y held), the wavelength lambda (m), the field F (V/m).
struct MaterialSweep {
    QString variable = QStringLiteral("T");
    double from = 200, to = 500;   // SI
    int points = 51;
    bool log = false;
    double T = 300, N = 0, lambda = 1.55e-6, F = 0;
    double x = std::numeric_limits<double>::quiet_NaN(), y = std::numeric_limits<double>::quiet_NaN();   // (NaN: its own)
};

/// A property's values along the sweep, in the unit said (eV, 1/cm³,
/// cm²/(V s), s, cm/s, cm⁶/s, cm³/s, 1/cm, W/(m K)...; NaN where it has none).
struct MaterialCurve {
    QString key, label, unit;
    std::vector<double> values;
};

struct MaterialCurves {
    QString variable, unit;          // the variable and its unit as shown (K, 1/cm³, µm, V/cm)
    std::vector<double> at;          // its values in that unit
    std::vector<MaterialCurve> curves;
    QStringList notes;               // what it has none of, which models are taken
    QString error;
};

/// The properties that can be plotted: Eg, chi, Nc, Nv, ni, epsilonr, mun, mup
/// (the lattice's of T, by doping - Caughey and Thomas's or Masetti's, as its
/// material gives them -, saturated by the field), vsatn, vsatp, taun, taup
/// (Fossum's by doping where it has his), caun, caup, copt, dEg (Slotboom's
/// narrowing), alphan, alphap (impact ionization's rates, of the field), k,
/// rho, Cp, n, kappa, alpha (4πκ/λ, the light's absorption).
QStringList plottableProperties();

/// \a material's properties \a keys (empty: those of plottableProperties()
/// it has) along \a sweep - its texts evaluated in \a parameters at each
/// point's T and wavelength, an alloy at each x.
MaterialCurves materialCurves(const Node& material, const QStringList& keys, const MaterialSweep& sweep, const Scope& parameters);

struct Topology;

/// What a semiconductor feature's parameter \a key takes where it is not
/// set on it (nor its source "user"): its first domain's material's - the
/// material's model set's - its name in \a material; empty where the
/// material gives none (the feature's default is taken: silicon's).
QString inheritedParameter(const Model& model, const Topology& topo, const Node& feature, const QString& key, QString* material = nullptr);

} // namespace qucs_s::fem

#endif // QUCS_FEM_MATERIAL_PLOT_H
