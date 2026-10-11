/*
 * fem_materials.h - the multiphysics solver's library of materials: metals,
 *                   dielectrics, semiconductors and their insulators,
 *                   magnetic materials (B-H curves, magnets), air and water,
 *                   each with its properties and their units; the user's
 *                   own beside them, in a JSON file; a layout's layer
 *                   stack as a model of its cross-section
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_MATERIALS_H
#define QUCS_FEM_MATERIALS_H

#include "fem_import.h"
#include "fem_model.h"

#include <QJsonObject>
#include <QLineF>
#include <QList>
#include <QString>

namespace qucs_s::fem {

/// A material of a library: its name, what kind it is, a word about it,
/// its properties - each an expression with its unit, keyed as a material
/// node's ("epsilonr", "tand", "sigma", "k", "rho", "Cp", "mur", "BH" - a
/// table of B and H -, "Br", "E", "nu", "alpha", "n", "kappa", "dndT").
struct MaterialEntry {
    QString name;
    QString category;
    QString description;
    QJsonObject properties;
    bool user = false;
};

/// Qucs-S's own: typical values at 20 to 25 °C (a datasheet's are better).
const QList<MaterialEntry>& builtinMaterials();
/// A library file: {"materials": [{"name": ..., "category": ...,
/// "description": ..., "properties": {...}}]}.
QList<MaterialEntry> readMaterials(const QString& path, QString* error = nullptr);
bool writeMaterials(const QString& path, const QList<MaterialEntry>& materials, QString* error = nullptr);
/// The entry named \a name (case not minded) in \a extra, else Qucs-S's.
std::optional<MaterialEntry> findMaterial(const QString& name, const QList<MaterialEntry>& extra = {});
/// A material node of \a entry: its properties copied, its library named;
/// on no domain yet (\a selection otherwise).
Node materialNode(const Model& model, const MaterialEntry& entry, const QJsonObject& selection = {});
/// The entry a material node would make (to keep it in a library).
MaterialEntry entryOf(const Node& material);

/*!
 * A zinc-blende III-V's band parameters, Vurgaftman, Meyer and Ram-Mohan's
 * (J. Appl. Phys. 89, 5815, 2001): its Γ gap at a temperature (Varshni's),
 * its valence band's top on one scale for all (InSb's at 0), the split-off
 * energy, the electron's mass, Luttinger's parameters, Kane's energy, the
 * lattice constant, deformation potentials (av in Chuang's sign: the gap
 * opens by (ac - av) times the volume's change), elastic constants; and,
 * roughly, its static permittivity and refractive index. An alloy's from
 * its binaries': linear, with their bowing where they have one.
 */
struct BandParameters {
    double Eg = 0;                          ///< eV
    double VBO = 0;                         ///< eV
    double Delta = 0;                       ///< eV
    double me = 0;                          ///< m0
    double gamma1 = 0, gamma2 = 0, gamma3 = 0;
    double Ep = 0;                          ///< eV
    double a0 = 0;                          ///< m
    double ac = 0, av = 0, b = 0;           ///< eV
    double C11 = 0, C12 = 0;                ///< Pa
    double epsr = 0, n = 0;
};
/// The binaries (GaAs, AlAs, InAs, InP, GaP, GaSb, AlSb, InSb) and the
/// alloys: AlGaAs (x of Al), InGaAs (x of In), InAlAs (x of In), GaInP (x of
/// Ga), GaAsP, InAsP, GaAsSb (x of As), InGaAsP (In1-x Gax Asy P1-y),
/// AlGaInAs (Alx Gay In1-x-y As).
QStringList compounds();
/// A compound's band parameters at composition \a x (and \a y) and
/// temperature \a T (K); none, and why, when it is not known or its
/// composition is out of range.
std::optional<BandParameters> bandParameters(const QString& compound, double x, double y, double T, QString* error = nullptr);
/// An alloy material's property \a key (Eg, chi, epsilonr, n, k, rho, Cp,
/// Nc, Nv, mun, mup, taun, taup, me, gamma1..3, Ep, VBO, Delta, a0, ac, av,
/// b, C11, C12) at 300 K as an expression with its unit: from its compound
/// and composition (\a x, \a y). Empty for a key it has none of. Its
/// transport and thermal values are rough.
QString compoundProperty(const QString& compound, double x, double y, const QString& key);

/// A row of a layout's layer stack: an object's name, the layer under the
/// cut it is made of ("1/0"; empty: all along the cut), its bottom and
/// thickness (µm), its material (a library entry's name; empty: none).
struct StackRow {
    QString name, layer;
    double z = 0, thickness = 1;
    QString material;
};
/// The rows a cut across \a shapes suggests: each layer under it, one on
/// another (a micrometre each), on a substrate, under air.
QList<StackRow> suggestedStack(const LayoutShapes& shapes, const QLineF& cut);
/// A new model (in µm) of layout \a layout's cell \a cell cut along \a cut
/// (µm): a Layout Cross-Section of \a rows, a material for each one named
/// (on the rows that name it).
Model crossSectionModel(const QString& layout, const QString& cell, const QLineF& cut, const QList<StackRow>& rows);

} // namespace qucs_s::fem

#endif // QUCS_FEM_MATERIALS_H
