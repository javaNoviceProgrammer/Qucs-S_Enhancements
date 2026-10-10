/*
 * fem_materials.cpp - the multiphysics solver's library of materials
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_materials.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Materials", text);
}

const QStringList PropertyKeys = {QStringLiteral("epsilonr"), QStringLiteral("sigma"), QStringLiteral("k"),
                                  QStringLiteral("rho"),      QStringLiteral("Cp"),    QStringLiteral("mur"),
                                  QStringLiteral("E"),        QStringLiteral("nu"),    QStringLiteral("alpha"),
                                  QStringLiteral("n")};

MaterialEntry entry(const char* name, const char* category, const char* description,
                    std::initializer_list<std::pair<const char*, const char*>> props)
{
    MaterialEntry e;
    e.name = QString::fromUtf8(name);
    e.category = QString::fromUtf8(category);
    e.description = QString::fromUtf8(description);
    for (const auto& [k, v] : props) e.properties.insert(QString::fromLatin1(k), QString::fromUtf8(v));
    return e;
}

} // namespace

const QList<MaterialEntry>& builtinMaterials()
{
    static const QList<MaterialEntry> list = {
        // Metals.
        entry("Copper", "Metals", "Annealed copper, the IACS reference", {{"epsilonr", "1"}, {"sigma", "5.998e7[S/m]"}, {"k", "400[W/(m*K)]"},
              {"rho", "8960[kg/m^3]"}, {"Cp", "385[J/(kg*K)]"}, {"mur", "1"}, {"E", "110[GPa]"}, {"nu", "0.35"}, {"alpha", "17e-6[1/K]"}}),
        entry("Aluminium", "Metals", "Pure aluminium", {{"epsilonr", "1"}, {"sigma", "3.774e7[S/m]"}, {"k", "238[W/(m*K)]"},
              {"rho", "2700[kg/m^3]"}, {"Cp", "900[J/(kg*K)]"}, {"mur", "1"}, {"E", "70[GPa]"}, {"nu", "0.33"}, {"alpha", "23e-6[1/K]"}}),
        entry("Gold", "Metals", "Pure gold", {{"epsilonr", "1"}, {"sigma", "4.10e7[S/m]"}, {"k", "317[W/(m*K)]"},
              {"rho", "19300[kg/m^3]"}, {"Cp", "129[J/(kg*K)]"}, {"mur", "1"}, {"E", "79[GPa]"}, {"nu", "0.44"}, {"alpha", "14.2e-6[1/K]"}}),
        entry("Silver", "Metals", "Pure silver", {{"epsilonr", "1"}, {"sigma", "6.30e7[S/m]"}, {"k", "429[W/(m*K)]"},
              {"rho", "10490[kg/m^3]"}, {"Cp", "235[J/(kg*K)]"}, {"mur", "1"}, {"E", "83[GPa]"}, {"nu", "0.37"}, {"alpha", "18.9e-6[1/K]"}}),
        entry("Solder (SAC305)", "Metals", "Sn96.5 Ag3.0 Cu0.5", {{"epsilonr", "1"}, {"sigma", "7.7e6[S/m]"}, {"k", "58[W/(m*K)]"},
              {"rho", "7400[kg/m^3]"}, {"Cp", "230[J/(kg*K)]"}, {"mur", "1"}, {"E", "50[GPa]"}, {"nu", "0.36"}, {"alpha", "21.7e-6[1/K]"}}),
        // Dielectrics of circuit boards and packages.
        entry("FR-4", "Dielectrics", "Glass-epoxy laminate; εr about 4.4 at 1 GHz, loss tangent 0.02",
              {{"epsilonr", "4.4"}, {"sigma", "0[S/m]"}, {"k", "0.3[W/(m*K)]"}, {"rho", "1900[kg/m^3]"}, {"Cp", "1150[J/(kg*K)]"},
               {"mur", "1"}, {"E", "22[GPa]"}, {"nu", "0.15"}, {"alpha", "14e-6[1/K]"}}),
        entry("Rogers RO4003C", "Dielectrics", "Hydrocarbon ceramic laminate; process εr 3.38, design 3.55; loss tangent 0.0027",
              {{"epsilonr", "3.38"}, {"sigma", "0[S/m]"}, {"k", "0.71[W/(m*K)]"}, {"rho", "1790[kg/m^3]"}, {"Cp", "900[J/(kg*K)]"},
               {"mur", "1"}, {"alpha", "11e-6[1/K]"}}),
        entry("Rogers RO4350B", "Dielectrics", "Hydrocarbon ceramic laminate; process εr 3.48, design 3.66; loss tangent 0.0037",
              {{"epsilonr", "3.48"}, {"sigma", "0[S/m]"}, {"k", "0.69[W/(m*K)]"}, {"rho", "1860[kg/m^3]"}, {"Cp", "900[J/(kg*K)]"},
               {"mur", "1"}, {"alpha", "10e-6[1/K]"}}),
        entry("Alumina (96%)", "Dielectrics", "Al₂O₃ substrate", {{"epsilonr", "9.4"}, {"sigma", "1e-12[S/m]"}, {"k", "24[W/(m*K)]"},
              {"rho", "3720[kg/m^3]"}, {"Cp", "880[J/(kg*K)]"}, {"mur", "1"}, {"E", "300[GPa]"}, {"nu", "0.21"}, {"alpha", "7.1e-6[1/K]"}}),
        entry("PTFE", "Dielectrics", "Polytetrafluoroethylene", {{"epsilonr", "2.1"}, {"sigma", "1e-16[S/m]"}, {"k", "0.25[W/(m*K)]"},
              {"rho", "2200[kg/m^3]"}, {"Cp", "1000[J/(kg*K)]"}, {"mur", "1"}, {"E", "0.5[GPa]"}, {"nu", "0.46"}, {"alpha", "120e-6[1/K]"}}),
        // Semiconductors and their insulators (no conductivity: it depends on the doping).
        entry("Silicon", "Semiconductors", "Crystalline silicon; n at 1550 nm. Its conductivity depends on its doping: give it",
              {{"epsilonr", "11.7"}, {"k", "130[W/(m*K)]"}, {"rho", "2329[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"}, {"mur", "1"},
               {"E", "170[GPa]"}, {"nu", "0.28"}, {"alpha", "2.6e-6[1/K]"}, {"n", "3.476"}}),
        entry("Silicon dioxide", "Semiconductors", "SiO₂, thermal oxide or fused silica; n at 1550 nm",
              {{"epsilonr", "3.9"}, {"sigma", "1e-15[S/m]"}, {"k", "1.4[W/(m*K)]"}, {"rho", "2200[kg/m^3]"}, {"Cp", "730[J/(kg*K)]"},
               {"mur", "1"}, {"E", "70[GPa]"}, {"nu", "0.17"}, {"alpha", "0.5e-6[1/K]"}, {"n", "1.444"}}),
        entry("Silicon nitride", "Semiconductors", "Si₃N₄, stoichiometric (a thin film conducts heat less); n at 1550 nm",
              {{"epsilonr", "7.5"}, {"sigma", "1e-14[S/m]"}, {"k", "20[W/(m*K)]"}, {"rho", "3100[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"},
               {"mur", "1"}, {"E", "250[GPa]"}, {"nu", "0.23"}, {"alpha", "2.3e-6[1/K]"}, {"n", "1.996"}}),
        entry("Indium phosphide", "Semiconductors", "InP; n at 1550 nm", {{"epsilonr", "12.4"}, {"k", "68[W/(m*K)]"},
              {"rho", "4810[kg/m^3]"}, {"Cp", "310[J/(kg*K)]"}, {"mur", "1"}, {"E", "61[GPa]"}, {"nu", "0.36"}, {"alpha", "4.6e-6[1/K]"}, {"n", "3.17"}}),
        entry("Gallium arsenide", "Semiconductors", "GaAs; n at 1550 nm", {{"epsilonr", "12.9"}, {"k", "55[W/(m*K)]"},
              {"rho", "5320[kg/m^3]"}, {"Cp", "330[J/(kg*K)]"}, {"mur", "1"}, {"E", "85.9[GPa]"}, {"nu", "0.31"}, {"alpha", "5.73e-6[1/K]"}, {"n", "3.37"}}),
        entry("Lithium niobate", "Semiconductors", "LiNbO₃ along its optical axis (εr33, ne at 1550 nm): it is anisotropic",
              {{"epsilonr", "28"}, {"sigma", "1e-15[S/m]"}, {"k", "4.6[W/(m*K)]"}, {"rho", "4640[kg/m^3]"}, {"Cp", "630[J/(kg*K)]"},
               {"mur", "1"}, {"E", "170[GPa]"}, {"nu", "0.25"}, {"alpha", "7.5e-6[1/K]"}, {"n", "2.138"}}),
        // Fluids.
        entry("Air", "Fluids", "Dry air at 20 °C and 1 atm (an insulator: no conductivity)", {{"epsilonr", "1"}, {"k", "0.0257[W/(m*K)]"},
              {"rho", "1.204[kg/m^3]"}, {"Cp", "1005[J/(kg*K)]"}, {"mur", "1"}, {"n", "1"}}),
        entry("Water", "Fluids", "Pure water at 20 °C", {{"epsilonr", "80.1"}, {"sigma", "5.5e-6[S/m]"}, {"k", "0.598[W/(m*K)]"},
              {"rho", "998[kg/m^3]"}, {"Cp", "4182[J/(kg*K)]"}, {"mur", "1"}, {"n", "1.333"}}),
    };
    return list;
}

QList<MaterialEntry> readMaterials(const QString& path, QString* error)
{
    QList<MaterialEntry> out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("%1 cannot be read: %2").arg(path, file.errorString());
        return out;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &pe);
    if (!doc.isObject()) {
        if (error) *error = tr("%1 is not a library of materials (%2)").arg(path, pe.errorString());
        return out;
    }
    for (const QJsonValue& v : doc.object().value(QStringLiteral("materials")).toArray()) {
        const QJsonObject o = v.toObject();
        MaterialEntry e;
        e.name = o.value(QStringLiteral("name")).toString();
        if (e.name.isEmpty()) continue;
        e.category = o.value(QStringLiteral("category")).toString(tr("My materials"));
        e.description = o.value(QStringLiteral("description")).toString();
        e.properties = o.value(QStringLiteral("properties")).toObject();
        e.user = true;
        out << e;
    }
    return out;
}

bool writeMaterials(const QString& path, const QList<MaterialEntry>& materials, QString* error)
{
    QJsonArray list;
    for (const MaterialEntry& e : materials)
        list.append(QJsonObject{{QStringLiteral("name"), e.name},
                                {QStringLiteral("category"), e.category},
                                {QStringLiteral("description"), e.description},
                                {QStringLiteral("properties"), e.properties}});
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("%1 cannot be written: %2").arg(path, file.errorString());
        return false;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("materials"), list}}).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = tr("%1 cannot be written: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

std::optional<MaterialEntry> findMaterial(const QString& name, const QList<MaterialEntry>& extra)
{
    for (const QList<MaterialEntry>* list : {&extra, &builtinMaterials()})
        for (const MaterialEntry& e : *list)
            if (e.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0) return e;
    return std::nullopt;
}

Node materialNode(const Model& model, const MaterialEntry& entry, const QJsonObject& selection)
{
    Node n = model.make(QStringLiteral("material"));
    n.label = entry.name;
    n.props.insert(QStringLiteral("library"), entry.name);
    for (auto it = entry.properties.begin(); it != entry.properties.end(); ++it)
        if (PropertyKeys.contains(it.key())) n.props.insert(it.key(), it.value());
    n.props.insert(QStringLiteral("selection"), selection.isEmpty() ? QJsonObject{{QStringLiteral("numbers"), QJsonArray()}} : selection);
    return n;
}

MaterialEntry entryOf(const Node& material)
{
    MaterialEntry e;
    e.name = material.name();
    e.category = tr("My materials");
    e.user = true;
    for (const QString& key : PropertyKeys) {
        const QString v = material.text(key);
        if (!v.trimmed().isEmpty()) e.properties.insert(key, v);
    }
    return e;
}

} // namespace qucs_s::fem
