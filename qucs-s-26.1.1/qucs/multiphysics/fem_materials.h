/*
 * fem_materials.h - the multiphysics solver's library of materials: metals,
 *                   dielectrics, semiconductors and their insulators, air
 *                   and water, each with its properties and their units;
 *                   the user's own beside them, in a JSON file
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

#include "fem_model.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace qucs_s::fem {

/// A material of a library: its name, what kind it is, a word about it,
/// its properties - each an expression with its unit, keyed as a material
/// node's ("epsilonr", "sigma", "k", "rho", "Cp", "mur", "E", "nu",
/// "alpha", "n").
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

} // namespace qucs_s::fem

#endif // QUCS_FEM_MATERIALS_H
