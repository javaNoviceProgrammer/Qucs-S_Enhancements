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
/// A wurtzite nitride's bands (c-plane, c along the growth axis): its gap,
/// the valence band's top (unstrained; eV), the crystal field's and the
/// spin and orbit's splittings Δcr and Δso (Chuang and Chang's Δ1 = Δcr,
/// Δ2 = Δ3 = Δso/3), the electron's mass along c and across it, A1-A6
/// (ħ²/2m0), the deformation potentials D1-D6 and the conduction band's a1
/// (along c) and a2 (across), Kane's Ep, the lattice a and c, the stiffness
/// C11, C12, C13, C33, the spontaneous polarization Psp and the
/// piezoelectric e31, e33 (C/m², along c: the metal face's), the static
/// permittivity along c and the index near the gap.
struct WurtziteParameters {
    double Eg = 0, Ev = 0, dcr = 0, dso = 0;   ///< eV
    double mez = 0, mep = 0;                   ///< m0
    double A[6] = {0, 0, 0, 0, 0, 0};
    double D[6] = {0, 0, 0, 0, 0, 0};          ///< eV
    double a1 = 0, a2 = 0, Ep = 0;             ///< eV
    double a = 0, c = 0;                       ///< m
    double C11 = 0, C12 = 0, C13 = 0, C33 = 0; ///< Pa
    double Psp = 0, e31 = 0, e33 = 0;          ///< C/m²
    double epsr = 0, n = 0;
};
/// The nitrides: GaN, AlN, InN and AlGaN (x of Al), InGaN (x of In), InAlN
/// (x of In), AlGaInN (Alx Gay In1-x-y N): Vurgaftman and Meyer's (J. Appl. Phys. 94, 3675, 2003; their
/// gaps' bowings 0.8, 1.4 and 2.5 eV), the spontaneous polarization's
/// Bernardini and Fiorentini's (bowed), the piezoelectric constants
/// Bernardini, Fiorentini and Vanderbilt's. Wurtzite: a compound of them.
bool isWurtzite(const QString& compound);
/// A nitride's parameters at composition \a x (and \a y: AlGaInN's Alx Gay
/// In1-x-y N) and temperature \a T (K); none, and why, when it is not one or
/// x is out of range.
std::optional<WurtziteParameters> wurtziteParameters(const QString& compound, double x, double T, QString* error = nullptr, double y = 0);
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
/// b, C11, C12, and its models' parameters - munmin, zetan, caun... as
/// semiconductorModelKeys() names them) at temperature \a T (K) as an
/// expression with its unit: from its compound and composition (\a x, \a
/// y) - its binaries' models' parameters weighed by their shares
/// (mobilities harmonically), as Lumerical's alloys are. Empty for a key it
/// has none of. Its transport and thermal values are rough. SiGe (x of
/// Ge) is one: its X and L valleys' gaps, the lower.
QString compoundProperty(const QString& compound, double x, double y, const QString& key, BandSet set = BandSet::Vurgaftman,
                         double T = 300);

/// The composition of alloy \a compound whose gap at \a T (K) is \a gap
/// (eV) in band set \a set: a ternary's x; a quaternary's x and y on the
/// lattice of compound \a lattice (InP, GaAs) - InGaAsP's along its In-Ga
/// and As-P, AlGaInAs's and AlGaInP's along Al-Ga. Where the gap is reached
/// twice, the lower x (a quaternary: the line's start, As-poor or Al-poor).
/// None, and why - the gaps there are - when it is not reached.
std::optional<std::pair<double, double>> compositionForGap(const QString& compound, double gap, const QString& lattice, BandSet set,
                                                           QString* error = nullptr, double T = 300);

/// The material properties a semiconductor's models read from its
/// material - the Semiconductor Material Model's (Caughey and Thomas's and
/// Masetti's mobilities, ζ, saturation, band gap narrowing), SRH's (Fossum's
/// doping dependence, the tunnelling masses, the trap level), Auger's,
/// radiative, impact ionization's, band-to-band tunnelling's -, each
/// the key a material keeps it under.
QStringList semiconductorModelKeys();
/// The key a material keeps feature \a type's parameter \a key under (its
/// model set): the same name, or Auger's Cn and Cp as caun and caup, the
/// radiative B as copt, impact ionization's an, bn... as iian, iibn...,
/// band-to-band tunnelling's A, B, γ as bbtA, bbtB, bbtgamma. Empty when
/// it is the feature's alone (Lombardi's surface scattering, the dopants'
/// levels).
QString materialKeyOf(const QString& type, const QString& key);

/// A scope over \a parent in which a material's texts read "T", the
/// temperature they are taken at (K): a semiconductor's band gap,
/// densities of states, mobilities and lifetimes, a conductivity of heat
/// (the library's are expressions of T, anchored at 300 K).
class MaterialScope : public Scope
{
public:
    MaterialScope(const Scope& parent, double T);
};

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
