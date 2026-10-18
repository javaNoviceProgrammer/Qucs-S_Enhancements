/*
 * fem_lumerical_scene.h - a Lumerical project or script as it reads (its
 *                         objects, materials, solvers), before it is made
 *                         a multiphysics model (internal)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_LUMERICAL_SCENE_H
#define QUCS_FEM_LUMERICAL_SCENE_H

#include "fem_lumerical.h"
#include "fem_materials.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <array>
#include <map>
#include <memory>
#include <vector>

namespace qucs_s::fem::lumerical {

/// A Lumerical "Properties" blob (little-endian): its entries' keys and
/// values - texts, numbers (one, or a list), matrices (column-major: their
/// real parts; a complex one's imaginary parts under its key and "@im"),
/// dictionaries (a QDataStream's). \a used: the bytes it took.
QVariantMap properties(const QByteArray& blob, bool* ok = nullptr, qint64 from = 0, qint64* used = nullptr);

/// A script's value: a number or a matrix (column-major, its rows and
/// columns), a text, a struct, a cell, a dataset.
struct Value {
    enum Kind { Null, Matrix, Text, Struct, Cell, Dataset };
    Kind kind = Null;
    std::vector<double> m;   // (a number: one)
    int rows = 0, cols = 0;
    QString s;
    std::map<QString, Value> fields;      // a struct's, a dataset's attributes
    std::vector<Value> cell;
    std::vector<int> dims;                // a reshaped matrix's
    static Value number(double v)
    {
        Value x;
        x.kind = Matrix;
        x.m = {v};
        x.rows = x.cols = 1;
        return x;
    }
    static Value text(const QString& t)
    {
        Value x;
        x.kind = Text;
        x.s = t;
        return x;
    }
    static Value matrix(std::vector<double> values, int rows, int cols)
    {
        Value x;
        x.kind = Matrix;
        x.m = std::move(values);
        x.rows = rows;
        x.cols = cols;
        return x;
    }
    bool isNumber() const { return kind == Matrix && m.size() == 1; }
    double toNumber(double otherwise = 0) const { return kind == Matrix && !m.empty() ? m[0] : otherwise; }
    QString toText() const;
    const Value* field(const QString& name) const
    {
        const auto it = fields.find(name);
        return it == fields.end() ? nullptr : &it->second;
    }
};

/// An object of the project or script: its kind - "rect", "poly",
/// "circle", "fde", "charge", "region", "dope", "diffusion", "importdope",
/// "contact", "modelmaterial", "mesh" (others as Lumerical names them) -,
/// its name and properties as set.
struct Object {
    QString kind;
    std::map<QString, Value> props;
    std::vector<std::pair<QString, Value>> history;   // (the sets in order: x, x span, x min...)
    QString name() const
    {
        const auto it = props.find(QStringLiteral("name"));
        return it == props.end() ? QString() : it->second.toText();
    }
    const Value* prop(const QString& key) const
    {
        const auto it = props.find(key);
        return it == props.end() ? nullptr : &it->second;
    }
    double number(const QString& key, double otherwise = NAN) const
    {
        const Value* v = prop(key);
        return v && v->kind == Value::Matrix && !v->m.empty() ? v->m[0] : otherwise;
    }
    QString text(const QString& key) const
    {
        const Value* v = prop(key);
        return v ? v->toText() : QString();
    }
};

/// An optical material (MODE's): Lumerical's database's by name, or an
/// (n,k) material's index (diagonal), or its permittivity sampled.
struct Material {
    QString name;
    QString type;                 // "(n,k) Material", "database", "PEC", "dielectric", "sampled", "sellmeier", "thermal"
    double n[3] = {1, 1, 1}, k[3] = {0, 0, 0};
    bool anisotropic = false;
    std::vector<double> f, epsRe, epsIm;   // sampled (Hz)
    std::array<std::array<double, 7>, 3> sellmeier{};   // Sellmeier's c0..c6 along x, y, z (λ in µm)
    QString base;                 // a temperature dependent one's material (its name)
    double dndT[3] = {0, 0, 0}, dkdT = 0, tref = 300;  // (its index n(λ) + dn/dT (T - Tref))
    bool thermal = true, carriers = false;             // (its temperature taken; its free carriers' terms set)
    int priority = 2;             // its mesh order
};

/// What was read: the objects in order, the optical materials, the MQW
/// stack's mqwindex inputs (a script's), what Lumerical would do that is
/// not taken.
struct Scene {
    QString source;               // the file's name
    std::vector<Object> objects;
    std::vector<Material> materials;
    Value stack, params, config;  // mqwindex's (Null: none)
    std::vector<double> fields;   // a script's swept fields (V/m)
    /// A Qucs-S export's own line ("#qucs-s {...}", a comment to Lumerical):
    /// what its script does not carry, read back (empty: none).
    QJsonObject carried;
    QStringList notes;
    QString error;
};

/// A Lumerical script (.lsf) run as far as a model is concerned: its
/// objects added and set, its materials, an mqwindex call's inputs.
Scene readScript(const QString& text, const QString& source);
/// A MODE project (.lms): its rectangles, materials and FDE region.
Scene readModeProject(const QByteArray& bytes, const QString& source);

/// Lumerical's material of a name as the library's (Silicon, Silicon
/// dioxide...): its electrical database's (CT) or optical one's (EM, with
/// their sources: "Si (Silicon) - Palik"); empty: none.
QString libraryOf(const QString& lumerical, bool electrical);
/// Lumerical's alloy (its CT database's "InGaAs (Indium Gallium Arsenide)",
/// its MQW database's "InGaAs") at its composition as a compound here
/// (InGaAs, x of In) and its x, y; empty: none.
QString compoundOf(const QString& lumerical, double x, double y, double* cx, double* cy);

/// The model a scene makes: MODE's (an FDE region), CHARGE's (a CHARGE
/// solver), an MQW stack's (mqwindex).
LumericalImport buildModel(const Scene& scene);
/// A scene's materials as a library's entries: DEVICE's records (each
/// semiconductor's gap, masses, χ and its models' parameters as
/// expressions of T, an insulator's permittivity, a conductor's work
/// function; their heat), MODE's and FDTD's optical ones (their index of the
/// wavelength); what is not taken in \a notes.
std::vector<MaterialEntry> libraryEntries(const Scene& scene, QStringList* notes = nullptr);

} // namespace qucs_s::fem::lumerical

#endif // QUCS_FEM_LUMERICAL_SCENE_H
