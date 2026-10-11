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
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Materials", text);
}

const QStringList PropertyKeys = {QStringLiteral("epsilonr"), QStringLiteral("tand"), QStringLiteral("sigma"),
                                  QStringLiteral("k"),        QStringLiteral("rho"),  QStringLiteral("Cp"),
                                  QStringLiteral("mur"),      QStringLiteral("BH"),   QStringLiteral("Br"),
                                  QStringLiteral("E"),        QStringLiteral("nu"),   QStringLiteral("alpha"),
                                  QStringLiteral("n"),        QStringLiteral("kappa"), QStringLiteral("dndT"),
                                  QStringLiteral("Eg"),       QStringLiteral("chi"),  QStringLiteral("Nc"),
                                  QStringLiteral("Nv"),       QStringLiteral("mun"),  QStringLiteral("mup"),
                                  QStringLiteral("taun"),     QStringLiteral("taup")};

/// A B-H curve's points: B (T), H (A/m).
MaterialEntry withCurve(MaterialEntry e, std::initializer_list<std::pair<double, double>> points)
{
    QJsonArray rows;
    for (const auto& [b, h] : points) rows.append(QJsonArray{b, h});
    e.properties.insert(QStringLiteral("BH"), rows);
    return e;
}

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
        entry("Stainless steel (304)", "Metals", "Austenitic stainless steel: all but non-magnetic", {{"epsilonr", "1"}, {"sigma", "1.39e6[S/m]"},
              {"k", "16.2[W/(m*K)]"}, {"rho", "8000[kg/m^3]"}, {"Cp", "500[J/(kg*K)]"}, {"mur", "1.008"}, {"E", "193[GPa]"}, {"nu", "0.29"},
              {"alpha", "17.3e-6[1/K]"}}),
        // Dielectrics of circuit boards and packages.
        entry("FR-4", "Dielectrics", "Glass-epoxy laminate; εr about 4.4 and loss tangent 0.02 at 1 GHz",
              {{"epsilonr", "4.4"}, {"tand", "0.02"}, {"sigma", "0[S/m]"}, {"k", "0.3[W/(m*K)]"}, {"rho", "1900[kg/m^3]"}, {"Cp", "1150[J/(kg*K)]"},
               {"mur", "1"}, {"E", "22[GPa]"}, {"nu", "0.15"}, {"alpha", "14e-6[1/K]"}}),
        entry("Rogers RO4003C", "Dielectrics", "Hydrocarbon ceramic laminate; process εr 3.38, design 3.55; loss tangent 0.0027",
              {{"epsilonr", "3.38"}, {"tand", "0.0027"}, {"sigma", "0[S/m]"}, {"k", "0.71[W/(m*K)]"}, {"rho", "1790[kg/m^3]"}, {"Cp", "900[J/(kg*K)]"},
               {"mur", "1"}, {"alpha", "11e-6[1/K]"}}),
        entry("Rogers RO4350B", "Dielectrics", "Hydrocarbon ceramic laminate; process εr 3.48, design 3.66; loss tangent 0.0037",
              {{"epsilonr", "3.48"}, {"tand", "0.0037"}, {"sigma", "0[S/m]"}, {"k", "0.69[W/(m*K)]"}, {"rho", "1860[kg/m^3]"}, {"Cp", "900[J/(kg*K)]"},
               {"mur", "1"}, {"alpha", "10e-6[1/K]"}}),
        entry("Alumina (96%)", "Dielectrics", "Al₂O₃ substrate", {{"epsilonr", "9.4"}, {"tand", "0.0003"}, {"sigma", "1e-12[S/m]"}, {"k", "24[W/(m*K)]"},
              {"rho", "3720[kg/m^3]"}, {"Cp", "880[J/(kg*K)]"}, {"mur", "1"}, {"E", "300[GPa]"}, {"nu", "0.21"}, {"alpha", "7.1e-6[1/K]"}}),
        entry("PTFE", "Dielectrics", "Polytetrafluoroethylene", {{"epsilonr", "2.1"}, {"tand", "0.0002"}, {"sigma", "1e-16[S/m]"}, {"k", "0.25[W/(m*K)]"},
              {"rho", "2200[kg/m^3]"}, {"Cp", "1000[J/(kg*K)]"}, {"mur", "1"}, {"E", "0.5[GPa]"}, {"nu", "0.46"}, {"alpha", "120e-6[1/K]"}}),
        // Semiconductors and their insulators (no conductivity: it depends on the doping).
        entry("Silicon", "Semiconductors", "Crystalline silicon at 300 K; n at 1550 nm. Its conductivity depends on its doping: give it, or solve its carriers (Semiconductor)",
              {{"epsilonr", "11.7"}, {"k", "130[W/(m*K)]"}, {"rho", "2329[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"}, {"mur", "1"},
               {"E", "170[GPa]"}, {"nu", "0.28"}, {"alpha", "2.6e-6[1/K]"}, {"n", "3.476"}, {"dndT", "1.86e-4[1/K]"},
               {"Eg", "1.12[eV]"}, {"chi", "4.05[eV]"}, {"Nc", "2.8e19[1/cm^3]"}, {"Nv", "1.04e19[1/cm^3]"}, {"mun", "1400[cm^2/(V*s)]"},
               {"mup", "450[cm^2/(V*s)]"}, {"taun", "10[us]"}, {"taup", "10[us]"}}),
        entry("Germanium", "Semiconductors", "Crystalline germanium; n at 1550 nm",
              {{"epsilonr", "16.2"}, {"k", "60[W/(m*K)]"}, {"rho", "5323[kg/m^3]"}, {"Cp", "320[J/(kg*K)]"}, {"mur", "1"}, {"E", "103[GPa]"},
               {"nu", "0.26"}, {"alpha", "5.9e-6[1/K]"}, {"n", "4.275"}, {"dndT", "4.2e-4[1/K]"}, {"Eg", "0.66[eV]"}, {"chi", "4.0[eV]"},
               {"Nc", "1.04e19[1/cm^3]"}, {"Nv", "6.0e18[1/cm^3]"}, {"mun", "3900[cm^2/(V*s)]"}, {"mup", "1900[cm^2/(V*s)]"},
               {"taun", "1[ms]"}, {"taup", "1[ms]"}}),
        entry("Gallium nitride", "Semiconductors", "Wurtzite GaN (its polarization not modelled); n at 1550 nm",
              {{"epsilonr", "8.9"}, {"k", "130[W/(m*K)]"}, {"rho", "6150[kg/m^3]"}, {"Cp", "490[J/(kg*K)]"}, {"mur", "1"}, {"E", "295[GPa]"},
               {"nu", "0.23"}, {"alpha", "5.6e-6[1/K]"}, {"n", "2.31"}, {"Eg", "3.4[eV]"}, {"chi", "4.1[eV]"}, {"Nc", "2.3e18[1/cm^3]"},
               {"Nv", "4.6e19[1/cm^3]"}, {"mun", "1000[cm^2/(V*s)]"}, {"mup", "30[cm^2/(V*s)]"}, {"taun", "1[ns]"}, {"taup", "1[ns]"}}),
        entry("Silicon dioxide", "Semiconductors", "SiO₂, thermal oxide or fused silica; n at 1550 nm",
              {{"epsilonr", "3.9"}, {"tand", "0.0001"}, {"sigma", "1e-15[S/m]"}, {"k", "1.4[W/(m*K)]"}, {"rho", "2200[kg/m^3]"}, {"Cp", "730[J/(kg*K)]"},
               {"mur", "1"}, {"E", "70[GPa]"}, {"nu", "0.17"}, {"alpha", "0.5e-6[1/K]"}, {"n", "1.444"}, {"dndT", "9.5e-6[1/K]"}}),
        entry("Silicon nitride", "Semiconductors", "Si₃N₄, stoichiometric (a thin film conducts heat less); n at 1550 nm",
              {{"epsilonr", "7.5"}, {"sigma", "1e-14[S/m]"}, {"k", "20[W/(m*K)]"}, {"rho", "3100[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"},
               {"mur", "1"}, {"E", "250[GPa]"}, {"nu", "0.23"}, {"alpha", "2.3e-6[1/K]"}, {"n", "1.996"}, {"dndT", "2.45e-5[1/K]"}}),
        entry("Indium phosphide", "Semiconductors", "InP; n at 1550 nm", {{"epsilonr", "12.4"}, {"k", "68[W/(m*K)]"},
              {"rho", "4810[kg/m^3]"}, {"Cp", "310[J/(kg*K)]"}, {"mur", "1"}, {"E", "61[GPa]"}, {"nu", "0.36"}, {"alpha", "4.6e-6[1/K]"}, {"n", "3.17"}, {"dndT", "2.0e-4[1/K]"},
              {"Eg", "1.344[eV]"}, {"chi", "4.38[eV]"}, {"Nc", "5.7e17[1/cm^3]"}, {"Nv", "1.1e19[1/cm^3]"}, {"mun", "5400[cm^2/(V*s)]"},
              {"mup", "200[cm^2/(V*s)]"}, {"taun", "1[ns]"}, {"taup", "1[ns]"}}),
        entry("Gallium arsenide", "Semiconductors", "GaAs; n at 1550 nm", {{"epsilonr", "12.9"}, {"k", "55[W/(m*K)]"},
              {"rho", "5320[kg/m^3]"}, {"Cp", "330[J/(kg*K)]"}, {"mur", "1"}, {"E", "85.9[GPa]"}, {"nu", "0.31"}, {"alpha", "5.73e-6[1/K]"}, {"n", "3.37"}, {"dndT", "2.35e-4[1/K]"},
              {"Eg", "1.424[eV]"}, {"chi", "4.07[eV]"}, {"Nc", "4.7e17[1/cm^3]"}, {"Nv", "9.0e18[1/cm^3]"}, {"mun", "8500[cm^2/(V*s)]"},
              {"mup", "400[cm^2/(V*s)]"}, {"taun", "5[ns]"}, {"taup", "5[ns]"}}),
        entry("Lithium niobate", "Semiconductors", "LiNbO₃ along its optical axis (εr33, ne at 1550 nm): it is anisotropic",
              {{"epsilonr", "28"}, {"sigma", "1e-15[S/m]"}, {"k", "4.6[W/(m*K)]"}, {"rho", "4640[kg/m^3]"}, {"Cp", "630[J/(kg*K)]"},
               {"mur", "1"}, {"E", "170[GPa]"}, {"nu", "0.25"}, {"alpha", "7.5e-6[1/K]"}, {"n", "2.138"}, {"dndT", "3.3e-5[1/K]"}}),
        // Optical glasses: n of the wavelength (Sellmeier's), for rays and modes.
        entry("Glass (BK7)", "Optical", "Schott N-BK7: n 1.5168 at 587.6 nm (Sellmeier's), for lenses and prisms",
              {{"epsilonr", "6.7"}, {"k", "1.114[W/(m*K)]"}, {"rho", "2510[kg/m^3]"}, {"Cp", "858[J/(kg*K)]"}, {"mur", "1"}, {"E", "82[GPa]"},
               {"nu", "0.206"}, {"alpha", "7.1e-6[1/K]"}, {"dndT", "1.6e-6[1/K]"},
               {"n", "sqrt(1 + 1.03961212*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.00600069867) + 0.231792344*(lambda0/1[um])^2/((lambda0/1[um])^2 - "
                     "0.0200179144) + 1.01046945*(lambda0/1[um])^2/((lambda0/1[um])^2 - 103.560653))"}}),
        entry("Fused silica", "Optical", "SiO₂ glass: n 1.4585 at 587.6 nm (Malitson's Sellmeier)",
              {{"epsilonr", "3.8"}, {"k", "1.38[W/(m*K)]"}, {"rho", "2203[kg/m^3]"}, {"Cp", "740[J/(kg*K)]"}, {"mur", "1"}, {"E", "73[GPa]"},
               {"nu", "0.17"}, {"alpha", "0.55e-6[1/K]"}, {"dndT", "1.0e-5[1/K]"},
               {"n", "sqrt(1 + 0.6961663*(lambda0/1[um])^2/((lambda0/1[um])^2 - 0.0684043^2) + 0.4079426*(lambda0/1[um])^2/((lambda0/1[um])^2 - "
                     "0.1162414^2) + 0.8974794*(lambda0/1[um])^2/((lambda0/1[um])^2 - 9.896161^2))"}}),
        // Magnetic materials: soft ones with a B-H curve (typical, not a datasheet's), permanent magnets.
        withCurve(entry("Iron (soft)", "Magnetic", "Low-carbon iron, annealed: a typical B-H curve; µr its first slope's",
                        {{"epsilonr", "1"}, {"sigma", "1.0e7[S/m]"}, {"mur", "4000"}, {"k", "80[W/(m*K)]"}, {"rho", "7870[kg/m^3]"},
                         {"Cp", "450[J/(kg*K)]"}, {"E", "200[GPa]"}, {"nu", "0.29"}, {"alpha", "12e-6[1/K]"}}),
                  {{0.4, 50}, {0.8, 100}, {1.0, 150}, {1.1, 200}, {1.22, 300}, {1.35, 500}, {1.48, 1000}, {1.58, 2000},
                   {1.70, 5000}, {1.80, 10000}, {1.90, 20000}, {2.03, 50000}, {2.10, 100000}}),
        withCurve(entry("Electrical steel (M400-50A)", "Magnetic",
                        "Non-oriented silicon steel: a typical B-H curve. Laminated, its eddy currents are small: its σ for a solid core",
                        {{"epsilonr", "1"}, {"sigma", "2.0e6[S/m]"}, {"mur", "3000"}, {"k", "30[W/(m*K)]"}, {"rho", "7700[kg/m^3]"},
                         {"Cp", "460[J/(kg*K)]"}, {"E", "200[GPa]"}, {"nu", "0.29"}, {"alpha", "12e-6[1/K]"}}),
                  {{0.2, 30}, {0.5, 50}, {0.8, 75}, {1.0, 100}, {1.18, 150}, {1.27, 200}, {1.36, 300}, {1.44, 500}, {1.53, 1000},
                   {1.63, 2500}, {1.72, 5000}, {1.81, 10000}, {1.93, 25000}, {2.03, 50000}, {2.10, 100000}}),
        withCurve(entry("Ferrite (MnZn)", "Magnetic", "A power ferrite: µr about 2000, saturating near 0.4 T; nearly an insulator",
                        {{"epsilonr", "12"}, {"sigma", "0.2[S/m]"}, {"mur", "2000"}, {"k", "4[W/(m*K)]"}, {"rho", "4800[kg/m^3]"},
                         {"Cp", "700[J/(kg*K)]"}}),
                  {{0.1, 40}, {0.2, 85}, {0.3, 170}, {0.35, 300}, {0.4, 800}, {0.45, 3000}}),
        entry("NdFeB (N42)", "Magnetic", "A sintered neodymium magnet: Br 1.3 T, its recoil permeability 1.05",
              {{"epsilonr", "1"}, {"sigma", "6.7e5[S/m]"}, {"mur", "1.05"}, {"Br", "1.3[T]"}, {"k", "9[W/(m*K)]"}, {"rho", "7500[kg/m^3]"},
               {"Cp", "440[J/(kg*K)]"}, {"E", "160[GPa]"}, {"nu", "0.24"}}),
        entry("SmCo (Sm2Co17)", "Magnetic", "A samarium-cobalt magnet: Br 1.1 T, for heat", {{"epsilonr", "1"}, {"sigma", "1.2e6[S/m]"},
              {"mur", "1.05"}, {"Br", "1.1[T]"}, {"k", "10[W/(m*K)]"}, {"rho", "8400[kg/m^3]"}, {"Cp", "370[J/(kg*K)]"}}),
        entry("Ferrite magnet (Y30)", "Magnetic", "A hard ferrite (ceramic) magnet: Br 0.39 T, an insulator",
              {{"epsilonr", "12"}, {"sigma", "1e-4[S/m]"}, {"mur", "1.1"}, {"Br", "0.39[T]"}, {"k", "4[W/(m*K)]"}, {"rho", "4900[kg/m^3]"},
               {"Cp", "800[J/(kg*K)]"}}),
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

QList<StackRow> suggestedStack(const LayoutShapes& shapes, const QLineF& cut)
{
    QList<StackRow> rows;
    rows << StackRow{QStringLiteral("substrate"), QString(), -10, 10, QStringLiteral("Silicon dioxide")};
    double z = 0;
    for (const LayoutShapes::Layer& l : shapes.layers) {
        if (cutIntervals(l.loops, cut.p1(), cut.p2()).empty()) continue;
        rows << StackRow{QStringLiteral("layer_%1_%2").arg(l.key.layer).arg(l.key.datatype), l.key.text(), z, 1, QString()};
        z += 1;
    }
    rows << StackRow{QStringLiteral("air"), QString(), 0, std::max(10.0, z + 5), QStringLiteral("Air")};
    return rows;
}

Model crossSectionModel(const QString& layout, const QString& cell, const QLineF& cut, const QList<StackRow>& rows)
{
    Model m = Model::blank();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("um"));
    Node xs = m.make(QStringLiteral("crosssection"));
    xs.label = QStringLiteral("cut");
    xs.props.insert(QStringLiteral("file"), layout);
    if (!cell.isEmpty()) xs.props.insert(QStringLiteral("cell"), cell);
    auto num = [](double v) { return formatNumber(v, 12); };
    xs.props.insert(QStringLiteral("start"), QJsonArray{num(cut.x1()), num(cut.y1())});
    xs.props.insert(QStringLiteral("end"), QJsonArray{num(cut.x2()), num(cut.y2())});
    QJsonArray stack;
    for (const StackRow& r : rows) stack.append(QJsonArray{r.name, r.layer, num(r.z), num(r.thickness)});
    xs.props.insert(QStringLiteral("stack"), stack);
    m.add(m.geometry().tag, xs);
    // A material for each named: on its rows (later rows over earlier).
    QStringList order;
    QHash<QString, QJsonArray> objects;
    for (const StackRow& r : rows) {
        if (r.material.trimmed().isEmpty()) continue;
        if (!order.contains(r.material)) order << r.material;
        objects[r.material].append(r.name);
    }
    for (const QString& name : order) {
        const std::optional<MaterialEntry> entry = findMaterial(name);
        Node mat = entry ? materialNode(m, *entry) : m.make(QStringLiteral("material"));
        if (!entry) mat.label = name;
        mat.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), objects.value(name)}});
        m.add(m.materials().tag, mat);
    }
    m.root.label = QStringLiteral("A cross-section of %1").arg(QFileInfo(layout).fileName());
    return m;
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
