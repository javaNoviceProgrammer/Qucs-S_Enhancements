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
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>


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

/*!
 * A Lumerical project or script as a multiphysics model, by its kind:
 *  - an .ldev: an MQW project's stack (importLumericalMqw), a CHARGE
 *    project's cross-section;
 *  - an .lms (MODE): its FDE region's cross-section (2D Z normal) - its
 *    rectangles, each material its index at the wavelength (Lumerical's
 *    data) or the library's of its name, PML sides as layers inside the
 *    region -, the wavelength, trial modes and a sweep's wavelengths;
 *  - an .lsf (a script, as an export writes it, or one that builds a
 *    project with the same commands): what it makes - an FDE region's
 *    cross-section, a CHARGE simulation region's (its structures, model
 *    materials by name, constant and diffusion doping, contacts and their
 *    sweeps, temperature, norm length), an mqwindex stack's layers and
 *    spectrum.
 * Where structures overlap the one of the lower mesh order is taken. Its
 * notes say what was read and how it was taken, and what was not.
 */
LumericalImport importLumerical(const QString& path);

struct MaterialEntry;
/// A Lumerical project's materials as a library's entries (Lumerical's
/// "import materials from another project"): an .ldev's database - each
/// semiconductor's gap, masses, χ and its models' parameters as expressions
/// of T, an insulator's permittivity, a conductor's work function, their
/// heat -, an .lms's or .fsp's optical materials (their index of the
/// wavelength); its alloys left to the library's compounds. What is not
/// taken in \a notes; none, and why, when it cannot be read.
QList<MaterialEntry> lumericalMaterials(const QString& path, QString* error = nullptr, QStringList* notes = nullptr);

class Model;
struct Node;

/*!
 * What a Lumerical export node makes: a Lumerical script (.lsf) that, run
 * in the product, builds the model in a new project and saves it beside
 * the script (base.lms, base.ldev) - its cross-section a structure for
 * each domain (rectangles, polygons; the outer first, a domain within
 * another's hole after it), in metres:
 *  - MODE (lumericalmode): (n,k) materials of each domain's index at the
 *    mode step's wavelength as the model has it, or Lumerical's own by
 *    name; an FDE region (2D Z normal) over the physics' domains, PML
 *    where the model has PML domains, Metal or PMC on the other sides;
 *    the wavelength, trial modes and the index searched about;
 *  - CHARGE (lumericalcharge): Lumerical's electrical materials by name
 *    (alloys at their composition), the cross-section in the x-z plane
 *    (2D y-normal, Lumerical's default; y the model's y as z), uniform
 *    and box doping as its constant and diffusion objects, an implant or
 *    an expression sampled into an import doping object, each contact a
 *    thin metal strip outside its boundary (a metal domain there its own)
 *    with its voltage (a study's sweep a value table; a gate's metal its
 *    work function), the norm length the model's thickness;
 *  - MQW (lumericalmqw): the wells' stack for mqwindex (Lumerical's 4x4
 *    k·p; its MQW solver object has no script command) - each layer's own
 *    parameters (its gap, Ep, me, γ1-γ3, deformation potentials,
 *    stiffness, lattice, valence band on one scale), or Lumerical's
 *    database by compound and composition, and its strain -, the
 *    temperature, the field (a study's sweep of it a solve for each), the
 *    carrier density over the stack (a sweep's, mqwindex's densities),
 *    the spectrum step's photon frequencies and broadening (a full width),
 *    k of the bands step.
 * An axisymmetric model is refused (MODE, CHARGE). Its notes say what
 * was taken and how, and what Lumerical will do otherwise.
 */
struct LumericalScript {
    QString text;
    QString summary;
    QStringList notes;
    QString error;
};
/// \a progress told how far its geometry (a process flow's) and mesh are
/// built - false from it stops them.
LumericalScript lumericalScript(const Model& model, const Node& node, const QString& base, const QString& version,
                                const std::function<bool(double fraction, const QString& what)>& progress = {});

} // namespace qucs_s::fem

#endif // QUCS_FEM_LUMERICAL_H
