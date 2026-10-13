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
/// Whose band gaps: Vurgaftman et al.'s (0 K, Varshni's law to the
/// temperature; 9-16 meV off at 300 K in the InP family), or fitted to gaps
/// measured at 300 K - InP 1.344, GaAs 1.424, InAs 0.354 eV (Varshni's slope
/// kept, shifted there), In0.53Ga0.47As 0.750 (Goetz et al.'s bowing, 0.43),
/// In0.52Al0.48As 1.46, InGaAsP on InP's lattice within 7 meV of Nahory et
/// al.'s 1.35 - 0.72 y + 0.12 y², AlGaInAs's 1.3 µm quaternary (Al0.165
/// Ga0.305 In0.53 As) 0.954 eV. Only the gaps differ (the valence band's
/// offset is Vurgaftman's: a gap moved moves the conduction band).
enum class BandSet { Vurgaftman, Measured };
/// A material's "bands": "measured" (the 300 K fit), else Vurgaftman's.
BandSet bandSetOf(const QString& text);
/// The binaries (GaAs, AlAs, InAs, InP, GaP, GaSb, AlSb, InSb) and the
/// alloys: AlGaAs (x of Al), InGaAs (x of In), InAlAs (x of In), GaInP (x of
/// Ga), GaAsP, InAsP, GaAsSb (x of As), InGaAsP (In1-x Gax Asy P1-y),
/// AlGaInAs (Alx Gay In1-x-y As).
QStringList compounds();
/// A compound's band parameters at composition \a x (and \a y) and
/// temperature \a T (K); none, and why, when it is not known or its
/// composition is out of range.
std::optional<BandParameters> bandParameters(const QString& compound, double x, double y, double T, QString* error = nullptr,
                                            BandSet set = BandSet::Vurgaftman);
/// A well's electron valley set (1D): its degeneracy (valleys alike, each
/// with spin's two), its mass along the growth axis (the quantization's) and
/// its density-of-states mass in the plane (m0), its edge above the band's
/// (eV).
struct ValleySet {
    double g = 1, mz = 0, md = 0, dE = 0;
};
/// The valley sets "valleys" gives: Si(100), Si(110), Si(111) (the six Δ
/// valleys, ml 0.916, mt 0.190), Ge(100), Ge(110), Ge(111) (the four L
/// valleys, ml 1.59, mt 0.0815), each followed by "ml mt" for other masses -
/// Stern and Howard's rule: along the axis 1/mz = cos²θ/ml + sin²θ/mt, in the
/// plane md = √(ml mt²/mz), the valleys of one mz a set -, or sets of their
/// own, "g mz md [dE]; ..." (dE in eV). None for an empty text; why when it is
/// none of these.
std::optional<std::vector<ValleySet>> valleySets(const QString& text, QString* error = nullptr);
/// An alloy material's property \a key (Eg, chi, epsilonr, n, k, rho, Cp,
/// Nc, Nv, mun, mup, taun, taup, me, gamma1..3, Ep, VBO, Delta, a0, ac, av,
/// b, C11, C12) at 300 K as an expression with its unit: from its compound
/// and composition (\a x, \a y). Empty for a key it has none of. Its
/// transport and thermal values are rough.
QString compoundProperty(const QString& compound, double x, double y, const QString& key, BandSet set = BandSet::Vurgaftman);

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
