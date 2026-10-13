/*
 * fem_lumerical.h - Lumerical's MQW projects (.ldev) made multiphysics
 *                   models
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_LUMERICAL_H
#define QUCS_FEM_LUMERICAL_H

#include <QJsonObject>
#include <QString>
#include <QStringList>


namespace qucs_s::fem {

/*!
 * A Lumerical MQW project (.ldev: HDF5) as a multiphysics model: its MQW
 * solver's layer table a stack of rectangles along y (nm, bottom first),
 * each layer a Quantum Well Material of its own - its material's gap at the
 * solver's temperature over the layer's valence band offset, its electron
 * mass, Luttinger's γ (zinc blende) or A1-A6 and Δcr, Δso (wurtzite), Δ and
 * Ep, deformation potentials and stiffness, the layer's strain -, the
 * solver's temperature, field, k·p order (4x4, 6x6 wurtzite, 8x8) and grid
 * step on a quantum wells physics, a Valence Bands step (k to its Brillouin
 * zone ratio of π/a, the wells' a) and an Optical Spectrum step (its
 * frequencies as photon energies; its carrier density, Lumerical's averaged
 * over the stack, per the wells' volume; its broadening half Lumerical's γ,
 * a full width; Lorentzian), plot groups of the bands and the gain. Its
 * notes say what was read and how it was taken.
 */
struct LumericalImport {
    QJsonObject model;
    QStringList notes;
    QString error;
};
LumericalImport importLumericalMqw(const QString& path);

} // namespace qucs_s::fem

#endif // QUCS_FEM_LUMERICAL_H
