/*
 * test_fem_materials.cpp - the library of materials: each semiconductor's
 *                          models' parameters, its properties of the
 *                          temperature, the materials new to it, the
 *                          indices of the wavelength, a property's curve
 *                          (the visualizer's), Lumerical's projects'
 *                          materials into a library
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The figures checked are the sources' formulas worked here, not the
 * library's code read back: Varshni's, Caughey and Thomas's, Malitson's,
 * Vial's gold...; and the measured spectra the library's fits were made to
 * (the refractiveindex.info database's: Green's silicon, Skauli's GaAs,
 * Nunley's germanium, Yang's silver...), their figures written here.
 */
#include "fem_lumerical.h"
#include "fem_material_plot.h"
#include "fem_materials.h"
#include "fem_model.h"
#include "fem_problem.h"
#include "fem_solver.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <complex>

using namespace qucs_s::fem;

namespace {

constexpr double Q = 1.602176634e-19, KB = 1.380649e-23;

bool near(double a, double b, double rel)
{
    return std::abs(a - b) <= rel * std::max(std::abs(b), 1e-300);
}

/// Library entry \a name's property \a key at temperature \a T and
/// wavelength \a lambda (SI; an energy in eV), as the physics read it.
double valueOf(const QString& name, const QString& key, double T = 300, double lambda = 1.55e-6)
{
    const std::optional<MaterialEntry> e = findMaterial(name);
    if (!e) return NAN;
    const Node m = materialNode(Model::blank(), *e);
    MaterialScope scope(Scope::builtins(), T);
    scope.setConstant(QStringLiteral("lambda0"), lambda, Dim::length());
    const QString text = detail::Problem::materialProperty(m, key, &scope, T);
    if (text.trimmed().isEmpty()) return NAN;
    const Expression x = Expression::compile(text, scope);
    if (!x.isValid() || !x.isConstant()) return NAN;
    return x.dim() == Dim::of(2, 1, -2) ? x.constant() / Q : x.constant();
}

double expression(const QString& text)
{
    return evaluateConstant(text, Scope::builtins(), Dim()).value_or(NAN);
}

/// A compound's property \a key at composition x, y and wavelength \a
/// lambda (m), at \a T (300 K).
double compoundValue(const QString& compound, double x, double y, const QString& key, double lambda, double T = 300)
{
    MaterialScope scope(Scope::builtins(), T);
    scope.setConstant(QStringLiteral("lambda0"), lambda, Dim::length());
    const Expression e = Expression::compile(compoundProperty(compound, x, y, key, BandSet::Vurgaftman, T), scope);
    return e.isValid() && e.isConstant() ? e.constant() : NAN;
}

/// A Sellmeier's n at λ (µm): n² = A + Σ B λ²/(λ² - C²).
double sellmeierAt(double l, double A, std::initializer_list<std::pair<double, double>> terms)
{
    double n2 = A;
    for (const auto& [B, C] : terms) n2 += B * l * l / (l * l - C * C);
    return std::sqrt(n2);
}

/// Afromowitz's oscillator at λ (µm): E0, Ed, the gap Eg (eV), its gap's
/// log broadened by 20 meV (the real part of the log of E² + 2iEΓ's), held
/// 0.5 eV above the gap; lattice terms εs - ε∞ at a TO phonon (eV).
double oscillatorAt(double l, double E0, double Ed, double Eg, std::initializer_list<std::pair<double, double>> lattice = {})
{
    const double G = 0.02, Eph = 1.23984198 / l, E = std::min(Eph, Eg + 0.5), eta = M_PI * Ed / (2 * E0 * E0 * E0 * (E0 * E0 - Eg * Eg));
    const double Ef2 = 2 * E0 * E0 - Eg * Eg;
    double e1 = 1 + Ed / E0 + Ed * E * E / (E0 * E0 * E0)
                + eta / M_PI * std::pow(E, 4) * 0.5
                      * std::log((std::pow(Ef2 - E * E, 2) + 4 * Ef2 * G * G) / (std::pow(Eg * Eg - E * E, 2) + 4 * Eg * Eg * G * G));
    for (const auto& [S, ETO] : lattice) e1 += S / (1 - std::pow(Eph / ETO, 2));
    return std::sqrt(e1);
}

} // namespace

class TestFemMaterials : public QObject
{
    Q_OBJECT

private slots:
    // interp(x, x0, y0, ...): straight between its points, held beyond them;
    // neps and keps: a permittivity's index and extinction.
    void expressionTables()
    {
        QCOMPARE(expression(QStringLiteral("interp(1.5, 1, 10, 2, 20)")), 15.0);
        QCOMPARE(expression(QStringLiteral("interp(0, 1, 10, 2, 20, 3, 40)")), 10.0);
        QCOMPARE(expression(QStringLiteral("interp(5, 3, 40, 1, 10, 2, 20)")), 40.0);   // (its points in any order)
        QCOMPARE(expression(QStringLiteral("interp(2.5, 1, 10, 2, 20, 3, 40)")), 30.0);
        QVERIFY(near(expression(QStringLiteral("interp(1.25[um], 1[um], 10[eV], 2[um], 20[eV])")), 12.5 * Q, 1e-12));
        Scope s(&Scope::builtins());
        s.addVariable(QStringLiteral("u"));
        const Expression e = Expression::compile(QStringLiteral("interp(u, 0, 0, 1, 2)"), s);
        QVERIFY2(e.isValid() && !e.isConstant(), qPrintable(e.error()));
        for (const char* bad : {"interp(1, 2, 3)", "interp(1, 2, 3, 2, 4)", "interp(1, 2, 3, 4)"})
            QVERIFY2(!Expression::compile(QString::fromLatin1(bad), Scope::builtins()).isValid(), bad);
        QVERIFY(!Expression::compile(QStringLiteral("interp(1, u, 3, 4, 5)"), s).isValid());   // (points constants)
        const std::complex<double> nk = std::sqrt(std::complex<double>(-20, 3));
        QVERIFY(near(expression(QStringLiteral("neps(-20, 3)")), nk.real(), 1e-12) && near(expression(QStringLiteral("keps(-20, 3)")), nk.imag(), 1e-12));
        QVERIFY(near(expression(QStringLiteral("neps(12.1, 0)")), 3.478505426, 1e-9) && expression(QStringLiteral("keps(12.1, 0)")) == 0);
    }

    // Silicon at 300 K as before - its gap to the last digit, its densities
    // of states, mobilities, conductivity of heat -, and its models'
    // parameters the semiconductor features' own defaults: a model of it
    // solves as it did (but Masetti's µmax, its own now).
    void siliconAsItWas()
    {
        QCOMPARE(valueOf(QStringLiteral("Silicon"), QStringLiteral("Eg")), 1.12);
        QCOMPARE(valueOf(QStringLiteral("Silicon"), QStringLiteral("Nc")), 2.8e25);
        QCOMPARE(valueOf(QStringLiteral("Silicon"), QStringLiteral("Nv")), 1.04e25);
        QCOMPARE(valueOf(QStringLiteral("Silicon"), QStringLiteral("mun")), 0.14);
        QCOMPARE(valueOf(QStringLiteral("Silicon"), QStringLiteral("mup")), 0.045);
        QCOMPARE(valueOf(QStringLiteral("Silicon"), QStringLiteral("k")), 130.0);
        int compared = 0;
        for (const QString type : {QStringLiteral("semiconductor/model"), QStringLiteral("semiconductor/srh"), QStringLiteral("semiconductor/auger"),
                                   QStringLiteral("semiconductor/radiative"), QStringLiteral("semiconductor/impact"), QStringLiteral("semiconductor/btbt")}) {
            const NodeKind* k = nodeKind(type);
            QVERIFY(k);
            for (const PropertyDef& p : k->properties) {
                const QString mk = materialKeyOf(type, p.key);
                if (mk.isEmpty() || mk == QLatin1String("taun") || mk == QLatin1String("taup") || mk == QLatin1String("Et")) continue;
                // (Masetti's µmax: the feature's empty - the lattice's, the
                // Lumerical Masetti model's -, Silicon's Masetti's fit's own,
                // 1417 and 470.5 cm²/(V s): the update check of 21 October.)
                if (mk == QLatin1String("mmaxn") || mk == QLatin1String("mmaxp")) {
                    QVERIFY2(p.defaultValue.toString().isEmpty(), qPrintable(p.defaultValue.toString()));
                    QCOMPARE(valueOf(QStringLiteral("Silicon"), mk), mk.endsWith(QLatin1Char('n')) ? 0.1417 : 0.04705);
                    continue;
                }
                // (An energy in eV, as valueOf has it.)
                const Expression d = Expression::compile(p.defaultValue.toString(), Scope::builtins());
                const double feature = d.isValid() && d.isConstant() ? (d.dim() == Dim::of(2, 1, -2) ? d.constant() / Q : d.constant()) : NAN;
                const double material = valueOf(QStringLiteral("Silicon"), mk);
                // (The impact feature's defaults its user model's: van
                // Overstraeten and de Man's, the silicon's.)
                QVERIFY2(near(material, feature, 1e-12), qPrintable(QStringLiteral("%1 %2: %3 %4").arg(type, p.key).arg(material).arg(feature)));
                ++compared;
            }
        }
        QVERIFY2(compared >= 40, qPrintable(QString::number(compared)));
    }

    // Of the temperature: a gap Varshni's (anchored at its 300 K value), Nc
    // and Nv (T/300 K)^1.5, mobilities (T/300 K)^-ζ, a lifetime's exponent,
    // a conductivity of heat Palikovski's; a binary's entry as its compound
    // has it at T.
    void ofTheTemperature()
    {
        auto varshni = [](double eg300, double a, double b, double T) { return eg300 - a * (T * T / (T + b) - 9e4 / (300 + b)); };
        for (double T : {77.0, 250.0, 400.0, 500.0}) {
            const double t = T / 300;
            QVERIFY(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("Eg"), T), varshni(1.12, 4.73e-4, 636, T), 1e-12));
            QVERIFY(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("Nc"), T), 2.8e25 * std::pow(t, 1.5), 1e-12));
            QVERIFY(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("mun"), T), 0.14 * std::pow(t, -2.5), 1e-12));
            QVERIFY(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("k"), T), 130 * std::pow(t, -1.65), 1e-12));
            QVERIFY(near(valueOf(QStringLiteral("Germanium"), QStringLiteral("Eg"), T), varshni(0.66, 4.774e-4, 235, T), 1e-12));
            QVERIFY(near(valueOf(QStringLiteral("Gallium arsenide"), QStringLiteral("Eg"), T), varshni(1.424, 0.5405e-3, 204, T), 1e-12));
            QVERIFY(near(valueOf(QStringLiteral("Gallium arsenide"), QStringLiteral("taun"), T), 5e-9 * std::pow(t, -0.5), 1e-12));
            QVERIFY(near(valueOf(QStringLiteral("Indium phosphide"), QStringLiteral("mup"), T), 0.02 * std::pow(t, -1.2), 1e-12));
            // (A binary's entry and its compound alike: Vurgaftman's gap, its
            // masses' Nc.)
            QVERIFY(near(valueOf(QStringLiteral("Indium arsenide"), QStringLiteral("Eg"), T),
                         evaluateConstant(compoundProperty(QStringLiteral("InAs"), 0, 0, QStringLiteral("Eg"), BandSet::Vurgaftman, T), Scope::builtins(), Dim()).value_or(NAN) / Q, 1e-7));
            QVERIFY(near(valueOf(QStringLiteral("Indium arsenide"), QStringLiteral("Nc"), T),
                         evaluateConstant(compoundProperty(QStringLiteral("InAs"), 0, 0, QStringLiteral("Nc"), BandSet::Vurgaftman, T), Scope::builtins(), Dim()).value_or(NAN), 1e-6));
            // (An alloy's at T: its compound's.)
            QVERIFY(near(valueOf(QStringLiteral("Aluminium gallium arsenide"), QStringLiteral("Eg"), T), bandParameters(QStringLiteral("AlGaAs"), 0.3, 0, T)->Eg, 1e-6));
        }
    }

    // Each semiconductor's own models' parameters - Lumerical's database's,
    // its lowest valley's -, an alloy's its binaries' weighed (mobilities
    // harmonically).
    void modelSets()
    {
        const QString gaas = QStringLiteral("Gallium arsenide");
        QVERIFY(near(valueOf(gaas, QStringLiteral("munmin")), 40e-4, 1e-12) && near(valueOf(gaas, QStringLiteral("nrefn")), 8.1e22, 1e-12));
        QVERIFY(near(valueOf(gaas, QStringLiteral("munmin"), 400), 40e-4 * std::pow(4.0 / 3, -0.9), 1e-12));
        QVERIFY(near(valueOf(gaas, QStringLiteral("caun")), 1e-43, 1e-12) && near(valueOf(gaas, QStringLiteral("caup")), 2.28e-43, 1e-12));
        QVERIFY(near(valueOf(gaas, QStringLiteral("copt")), 7.2e-16, 1e-12) && near(valueOf(gaas, QStringLiteral("copt"), 400), 7.2e-16 * std::pow(4.0 / 3, -2.5), 1e-12));
        QVERIFY(near(valueOf(gaas, QStringLiteral("iian")), 2.99e7, 1e-12) && near(valueOf(gaas, QStringLiteral("iibn")), 6.85e7, 1e-12) && valueOf(gaas, QStringLiteral("iibetan")) == 1.6);
        QVERIFY(near(valueOf(gaas, QStringLiteral("vsatn")), 7.2e4, 1e-12) && valueOf(gaas, QStringLiteral("betan")) == 2);
        QVERIFY(near(valueOf(QStringLiteral("Germanium"), QStringLiteral("bbtA")), 9.1e16, 1e-12) && near(valueOf(QStringLiteral("Germanium"), QStringLiteral("bbtB")), 4.9e8, 1e-12));
        QVERIFY(near(valueOf(QStringLiteral("Indium phosphide"), QStringLiteral("vsatn")), 6.7e4, 1e-12) && valueOf(QStringLiteral("Indium phosphide"), QStringLiteral("betan")) == 1.25);
        QVERIFY(std::isnan(valueOf(gaas, QStringLiteral("crn"))));   // (Masetti's: silicon's alone)
        // AlGaAs x 0.3: AlAs 10 and GaAs 40 cm²/(V s) harmonically, Nref linearly.
        const QString algaas = QStringLiteral("Aluminium gallium arsenide");
        QVERIFY(near(valueOf(algaas, QStringLiteral("munmin")), 1e-4 / (0.3 / 10 + 0.7 / 40), 1e-9));   // (its text to 10 digits)
        QVERIFY(near(valueOf(algaas, QStringLiteral("nrefn")), (0.3 * 5e17 + 0.7 * 8.1e16) * 1e6, 1e-12));
        QVERIFY(near(valueOf(algaas, QStringLiteral("caun")), (0.3 * 1e-34 + 0.7 * 1e-31) * 1e-12, 1e-12));
        // Of T, the exponents weighed too (AlAs's µmin 0, GaAs's -0.9); a
        // parameter one binary has not - Slotboom's, GaAs's alone - its
        // other's whole, the shares renormalized.
        QVERIFY(near(valueOf(algaas, QStringLiteral("munmin"), 400), 1e-4 / (0.3 / 10 + 0.7 / 40) * std::pow(4.0 / 3, 0.7 * -0.9), 1e-9));
        QVERIFY(near(valueOf(algaas, QStringLiteral("Ebgn")), 0.0045, 1e-12));
        // A nitride alloy: its binaries' (AlN's, GaN's).
        QVERIFY(near(valueOf(QStringLiteral("Aluminium gallium nitride"), QStringLiteral("vsatn")), (0.2 * 1.6e7 + 0.8 * 1.34e7) * 1e-2, 1e-12));
    }

    // The materials new to the library: AlP, the II-VIs, silicon germanium,
    // AlInP, AlGaInP and the antimonides, AlGaInN, the high-κ oxides, the
    // metals' work functions.
    void newMaterials()
    {
        for (const char* name : {"Aluminium phosphide", "Cadmium sulfide", "Cadmium selenide", "Cadmium telluride", "Silicon germanium",
                                 "Aluminium indium phosphide", "Aluminium gallium indium phosphide", "Aluminium gallium phosphide",
                                 "Aluminium gallium antimonide", "Aluminium arsenide antimonide", "Indium arsenide antimonide",
                                 "Indium aluminium antimonide", "Aluminium gallium indium nitride", "Hafnium oxide", "Zirconium oxide",
                                 "Titanium oxide", "Aluminium oxide"})
            QVERIFY2(findMaterial(QString::fromLatin1(name)), name);
        QCOMPARE(valueOf(QStringLiteral("Hafnium oxide"), QStringLiteral("epsilonr")), 25.0);
        QVERIFY(std::isnan(valueOf(QStringLiteral("Hafnium oxide"), QStringLiteral("Eg"))));   // (an insulator to the semiconductor physics)
        QCOMPARE(valueOf(QStringLiteral("Gold"), QStringLiteral("phi")), 5.1);
        QCOMPARE(valueOf(QStringLiteral("Aluminium"), QStringLiteral("phi")), 4.28);
        QCOMPARE(valueOf(QStringLiteral("Cadmium telluride"), QStringLiteral("Eg")), 1.5);
        QVERIFY(near(valueOf(QStringLiteral("Cadmium telluride"), QStringLiteral("Nc")), 2.509e25 * std::pow(0.11, 1.5), 1e-3));
        // AlP: Vurgaftman's (its Γ gap 3.63 eV at 0 K, α 0.5771 meV/K, β 372 K).
        const std::optional<BandParameters> alp = bandParameters(QStringLiteral("AlP"), 0, 0, 300);
        QVERIFY(alp && near(alp->Eg, 3.63 - 0.5771e-3 * 9e4 / 672, 1e-12) && alp->Ep == 17.7 && alp->gamma1 == 3.35 && near(alp->a0, 5.4672e-10, 1e-12));
        // Their bowings (Vurgaftman's: Eg = x A + (1 - x) B - x (1 - x) C).
        auto bowed = [](const char* alloy, const char* a, const char* b, double x, double c) {
            const double ea = bandParameters(QLatin1String(a), 0, 0, 300)->Eg, eb = bandParameters(QLatin1String(b), 0, 0, 300)->Eg;
            return near(bandParameters(QLatin1String(alloy), x, 0, 300)->Eg, x * ea + (1 - x) * eb - x * (1 - x) * c, 1e-12);
        };
        QVERIFY(bowed("AlInP", "AlP", "InP", 0.52, -0.48));
        QVERIFY(bowed("AlGaSb", "AlSb", "GaSb", 0.3, -0.044 + 1.22 * 0.3));
        QVERIFY(bowed("InAsSb", "InAs", "InSb", 0.91, 0.67));
        QVERIFY(bowed("InAlSb", "InSb", "AlSb", 0.9, 0.43));
        QVERIFY(bowed("AlAsSb", "AlAs", "AlSb", 0.56, 0.8));
        QVERIFY(bowed("AlGaP", "AlP", "GaP", 0.5, 0));
        // The quaternaries: at their corners their binaries; AlGaInP of its
        // three ternaries.
        QVERIFY(near(bandParameters(QStringLiteral("AlGaInP"), 0, 1, 300)->Eg, bandParameters(QStringLiteral("GaP"), 0, 0, 300)->Eg, 1e-12));
        QVERIFY(near(bandParameters(QStringLiteral("AlGaInP"), 0, 0, 300)->Eg, bandParameters(QStringLiteral("InP"), 0, 0, 300)->Eg, 1e-12));
        QVERIFY(near(wurtziteParameters(QStringLiteral("AlGaInN"), 0, 300, nullptr, 1)->Eg, wurtziteParameters(QStringLiteral("GaN"), 0, 300)->Eg, 1e-12));
        QVERIFY(near(wurtziteParameters(QStringLiteral("AlGaInN"), 0.5, 300, nullptr, 0.5)->Eg, wurtziteParameters(QStringLiteral("AlGaN"), 0.5, 300)->Eg, 1e-9));
        QVERIFY(isWurtzite(QStringLiteral("AlGaInN")) && compounds().contains(QStringLiteral("AlGaInP")) && compounds().contains(QStringLiteral("SiGe")));
        // Silicon germanium: its X valley's gap (Lumerical's -0.4 eV bowing)
        // and its L valley's, the lower; its holes' mobility scattered by the
        // alloy, its conductivity of heat (Lumerical's 2.8 m K/W).
        auto sige = [](double x, const char* key) {
            return evaluateConstant(compoundProperty(QStringLiteral("SiGe"), x, 0, QLatin1String(key)), Scope::builtins(), Dim()).value_or(NAN);
        };
        QVERIFY(near(sige(0, "Eg") / Q, 1.12, 1e-12) && near(sige(1, "Eg") / Q, 0.66, 1e-12));
        QVERIFY(near(sige(0.5, "Eg") / Q, 0.5 * 1.12 + 0.5 * 0.93 - 0.1, 1e-12));
        QVERIFY(near(sige(0.9, "Eg") / Q, 0.1 * 2.01 + 0.9 * 0.66, 1e-12));   // (the L valley's there)
        QVERIFY(near(sige(0.5, "mup"), 1e-4 / (0.5 / 450 + 0.5 / 1900 + 0.25 / 100), 1e-9));
        QVERIFY(near(sige(0.5, "k"), 1 / (0.5 / 130 + 0.5 / 60 + 0.25 / 2.8), 1e-9));
        QVERIFY(near(sige(0, "chi") / Q, 4.05, 1e-12) && near(sige(1, "chi") / Q, 4.0, 1e-12));
    }

    // Indices of the wavelength (lambda0), each its source's formula or
    // measured points: silicon Green's and Li's (its pole at 1.135 µm gone),
    // Malitson's oxide, Luke's nitride, Pettit and Turner's InP, Vial's
    // gold, a silver fitted to Yang's; where each holds.
    void dispersion()
    {
        auto um = [](double l) { return l * 1e-6; };
        auto si = [&](double l) { return valueOf(QStringLiteral("Silicon"), QStringLiteral("n"), 300, um(l)); };
        auto siK = [&](double l) { return valueOf(QStringLiteral("Silicon"), QStringLiteral("kappa"), 300, um(l)); };
        // Silicon: the Sellmeier fitted to Green's and Li's from 0.4 µm -
        // Li's 3.4757 at 1550 nm to 2e-4, Green's within 0.007 -, Green's
        // points below; its absorption Green's, nought beyond 1.45 µm.
        const double l = 1.55;
        QVERIFY(near(si(l), sellmeierAt(l, 1, {{5.763557701, 0.2623529368}, {4.885714572, 0.3488033962}}), 1e-9));
        QVERIFY(std::abs(si(l) - 3.4757) < 2e-4);
        for (const auto& [lambda, green] : {std::pair{0.45, 4.691}, {0.5, 4.294}, {0.55, 4.077}, {0.6, 3.940}, {0.7, 3.772}, {0.8, 3.675}, {1.0, 3.572}})
            QVERIFY2(std::abs(si(lambda) - green) < 0.0072, qPrintable(QStringLiteral("%1: %2").arg(lambda).arg(si(lambda))));
        for (const auto& [lambda, li] : {std::pair{2.0, 3.4510}, {5.0, 3.4195}, {10.0, 3.4150}, {14.0, 3.4142}})
            QVERIFY2(std::abs(si(lambda) - li) < 0.001, qPrintable(QStringLiteral("%1: %2").arg(lambda).arg(si(lambda))));
        QVERIFY(near(si(0.4), 5.613, 1e-6) && near(si(0.37), 6.891, 1e-6) && near(si(0.305), (4.976 + 5.121) / 2, 1e-6));
        for (const auto& [lambda, green] : {std::pair{0.5, 0.044165}, {0.6, 0.019934}, {0.8, 0.0054113}, {1.0, 5.093e-4}, {1.1, 3.0637e-5}})
            QVERIFY2(near(siK(lambda), green, 1e-5), qPrintable(QStringLiteral("%1: %2").arg(lambda).arg(siK(lambda))));
        QVERIFY(near(siK(0.605), std::sqrt(0.019934 * 0.018446), 1e-5));   // (straight between their logarithms)
        QVERIFY(siK(l) == 0.0);   // (nought, not 1e-13: QCOMPARE takes that for it)
        // Finite and falling over its range from Green's E1 peak (0.37 µm):
        // the old Sellmeier's pole at 1.13475 µm gave NaN and 4.41 there.
        double before = si(0.4);
        for (int k = 1; k <= 4000; ++k) {
            const double lambda = 0.4 * std::pow(14 / 0.4, k / 4000.0), n = si(lambda);
            QVERIFY2(std::isfinite(n) && n < before, qPrintable(QStringLiteral("%1: %2 after %3").arg(lambda).arg(n).arg(before)));
            before = n;
        }
        for (double lambda = 1.130; lambda <= 1.140; lambda += 0.0005) QVERIFY(std::abs(si(lambda) - 3.534) < 0.004);
        QCOMPARE(findMaterial(QStringLiteral("Silicon"))->properties.value(QStringLiteral("nrange")).toString(), QStringLiteral("0.25[um] 14[um]"));
        QVERIFY(std::isfinite(si(0.2)) && std::isfinite(si(30)) && siK(0.2) > 3);

        auto sell = [](double l, std::initializer_list<std::pair<double, double>> t) { return sellmeierAt(l, 1, t); };
        QVERIFY(near(valueOf(QStringLiteral("Silicon dioxide"), QStringLiteral("n"), 300, um(0.6328)),
                     sell(0.6328, {{0.6961663, 0.0684043}, {0.4079426, 0.1162414}, {0.8974794, 9.896161}}), 1e-9));
        QVERIFY(near(valueOf(QStringLiteral("Silicon nitride"), QStringLiteral("n"), 300, um(l)), sell(l, {{3.0249, 0.1353406}, {40314, 1239.842}}), 1e-9));
        // InP: Pettit and Turner's from 0.95 µm; below 0.92 µm (its pole at
        // 0.626 µm) its oscillator, finite (held 0.5 eV over its gap, 0.67
        // µm, out of its range).
        QVERIFY(near(valueOf(QStringLiteral("Indium phosphide"), QStringLiteral("n"), 300, um(l)),
                     std::sqrt(7.255 + 2.316 * l * l / (l * l - 0.6263 * 0.6263) + 2.765 * l * l / (l * l - 32.935 * 32.935)), 1e-9));
        for (double lambda = 0.4; lambda < 1.0; lambda += 0.005) {
            const double n = valueOf(QStringLiteral("Indium phosphide"), QStringLiteral("n"), 300, um(lambda));
            QVERIFY2(std::isfinite(n) && n > 3.1 && n < 3.7, qPrintable(QStringLiteral("%1: %2").arg(lambda).arg(n)));
        }
        {
            // Vial et al.'s gold at 800 nm.
            const double w = 2 * M_PI * 299792458.0 / 0.8e-6, wd = 2 * M_PI * 2113.6e12, gd = 2 * M_PI * 15.92e12, wl = 2 * M_PI * 650.07e12, gl = 2 * M_PI * 104.86e12;
            const std::complex<double> eps = 5.9673 - wd * wd / (w * (w + std::complex<double>(0, gd))) - 1.09 * wl * wl / (w * w - wl * wl + std::complex<double>(0, gl * w));
            const std::complex<double> nk = std::sqrt(std::complex<double>(eps.real(), std::abs(eps.imag())));
            QVERIFY2(near(valueOf(QStringLiteral("Gold"), QStringLiteral("n"), 300, um(0.8)), nk.real(), 1e-4) && near(valueOf(QStringLiteral("Gold"), QStringLiteral("kappa"), 300, um(0.8)), nk.imag(), 1e-4),
                     qPrintable(QStringLiteral("%1 %2").arg(nk.real()).arg(nk.imag())));
        }
        {
            // Silver: Drude's and an oscillator at 8 eV, fitted to Yang et al.'s -
            // their n and κ at 1.55 and 10 µm within 1 %, Johnson and Christy's
            // κ within 5 % at 1.55 µm.
            const double w = 1.23984198 / l;
            const std::complex<double> eps = 2.4473 - 8.847 * 8.847 / (w * std::complex<double>(w, 0.03671)) + 0.89276 * 64 / (64 - w * w - std::complex<double>(0, 3 * w));
            const std::complex<double> nk = std::sqrt(std::complex<double>(eps.real(), std::abs(eps.imag())));
            QVERIFY(near(valueOf(QStringLiteral("Silver"), QStringLiteral("kappa"), 300, um(l)), nk.imag(), 1e-6));
            QVERIFY(near(valueOf(QStringLiteral("Silver"), QStringLiteral("n"), 300, um(l)), nk.real(), 1e-6));
            QVERIFY(near(nk.real(), 0.2582, 0.01) && near(nk.imag(), 10.9, 0.01) && near(nk.imag(), 11.366, 0.05));
            QVERIFY(near(valueOf(QStringLiteral("Silver"), QStringLiteral("n"), 300, um(10)), 10.006, 0.01));
            QVERIFY(near(valueOf(QStringLiteral("Silver"), QStringLiteral("kappa"), 300, um(10)), 69.04, 0.01));
        }
        QCOMPARE(findMaterial(QStringLiteral("Germanium"))->properties.value(QStringLiteral("nrange")).toString(), QStringLiteral("0.3[um] 14[um]"));
    }

    // Near the gap and above it: the oscillator's gap broadened (20 meV),
    // not held at 0.995 Eg - GaAs's index rising above its gap with
    // Aspnes and Studna's and Jellison's (within 0.04; held, 0.1 under at
    // 0.73 µm); the indirect AlAs's and AlSb's ranges ending 0.15 eV below
    // their direct gaps (their oscillators' spike there 0.2 and 0.3 above the
    // measured: Rakic's, Zollner's), within 0.05 in them; InGaAsP's (Adachi's
    // √ edge broadened by 50 meV) within 0.04 of his 1989 tables on both
    // sides of its gap - held, 0.1 and 0.14 over.
    void nearTheGapAndAbove()
    {
        auto um = [](double l) { return l * 1e-6; };
        for (const auto& [l, aspnes, jellison] : {std::tuple{0.7293, 3.742, 3.7347}, {0.7749, 3.700, 3.6891}, {0.8266, 3.666, 3.661}}) {
            const double n = valueOf(QStringLiteral("Gallium arsenide"), QStringLiteral("n"), 300, um(l));
            QVERIFY2(std::abs(n - aspnes) < 0.04 && std::abs(n - jellison) < 0.04, qPrintable(QStringLiteral("%1: %2").arg(l).arg(n)));
        }
        QVERIFY(std::abs(valueOf(QStringLiteral("Aluminium arsenide"), QStringLiteral("n"), 300, um(0.4387)) - 3.5736) < 0.05);
        for (const auto& [l, zollner] : {std::pair{0.6, 3.9742}, {0.886, 3.5}})
            QVERIFY2(std::abs(valueOf(QStringLiteral("Aluminium antimonide"), QStringLiteral("n"), 300, um(l)) - zollner) < 0.06, qPrintable(QString::number(l)));
        auto rangeOf = [](const char* name) {
            const QString t = findMaterial(QLatin1String(name))->properties.value(QStringLiteral("nrange")).toString();
            return evaluateConstant(t.simplified().split(QLatin1Char(' ')).value(0), Scope::builtins(), Dim::length()).value_or(NAN) * 1e6;
        };
        QVERIFY2(std::abs(rangeOf("Aluminium arsenide") - 1.23984198 / (3.003 - 0.15)) < 0.002, qPrintable(QString::number(rangeOf("Aluminium arsenide"))));
        QVERIFY2(std::abs(rangeOf("Gallium arsenide") - 1.23984198 / (1.424 + 0.2)) < 0.002, qPrintable(QString::number(rangeOf("Gallium arsenide"))));
        for (const auto& [l, adachi] : {std::pair{0.922, 3.4515}, {1.09, 3.379}, {1.3, 3.2906}}) {
            const double n = compoundValue(QStringLiteral("InGaAsP"), 0.109, 0.24, QStringLiteral("n"), um(l));
            QVERIFY2(std::abs(n - adachi) < 0.04, qPrintable(QStringLiteral("%1: %2 %3").arg(l).arg(n).arg(adachi)));
        }
        // (Finite through the gap: no spike, no pole - GaAs every meV from
        // 0.2 eV below to 0.5 above.)
        double last = 0;
        for (int k = 0; k <= 700; ++k) {
            const double E = 1.224 + 0.001 * k, n = valueOf(QStringLiteral("Gallium arsenide"), QStringLiteral("n"), 300, um(1.23984198 / E));
            QVERIFY2(std::isfinite(n) && (k == 0 || std::abs(n - last) < 0.01), qPrintable(QStringLiteral("%1 eV: %2").arg(E).arg(n)));
            last = n;
        }
    }

    // Of the temperature: a compound's index its 300 K one and its
    // oscillator's change as its gap and E0 move together by the gap's
    // shift - GaAs's dn/dT within 10 % of Franta et al.'s measured (2.25e-4
    // per K at 1.55 µm, 1.87e-4 at 5 µm; its gap alone moved, nought);
    // In0.53Ga0.47As's positive at 1550 nm, above its gap (it was -2e-4);
    // the 1.3 µm InGaAsP's near the literature's 2e-4 (it was half). Its
    // dndT the same derivative - so the Thermo-optic option takes a compound.
    void anIndexOfTheTemperature()
    {
        auto um = [](double l) { return l * 1e-6; };
        auto slope = [&](const char* c, double x, double y, double l) {
            return (compoundValue(QLatin1String(c), x, y, QStringLiteral("n"), um(l), 350) - compoundValue(QLatin1String(c), x, y, QStringLiteral("n"), um(l), 250)) / 100;
        };
        for (const auto& [l, franta] : {std::pair{1.55, 2.247e-4}, {5.0, 1.868e-4}}) {
            const double d = slope("GaAs", 0, 0, l);
            qInfo("GaAs at %g um: dn/dT %.4g per K (Franta et al.'s %.4g)", l, d, franta);
            QVERIFY2(near(d, franta, 0.1), qPrintable(QString::number(d)));
        }
        const double ingaas = slope("InGaAs", 0.53, 0, 1.55), q13 = slope("InGaAsP", 0.27775, 0.60475, 1.55);
        qInfo("In0.53Ga0.47As at 1550 nm %.4g per K; 1.3Q InGaAsP %.4g", ingaas, q13);
        QVERIFY(ingaas > 2e-4 && ingaas < 3.5e-4);
        // (The binaries' measured dn/dT at 1550 nm the compounds' - InP's
        // 2.0e-4, not its oscillator's 1.48e-4 -, an alloy's by their
        // shares: the quaternary's mostly InP's 2.05e-4 now 2.31e-4.)
        QVERIFY2(q13 > 2e-4 && q13 < 2.5e-4, qPrintable(QString::number(q13)));
        for (const auto& [c, measured] : {std::pair{"InP", 2.0e-4}, {"GaAs", 2.247e-4}})
            QVERIFY2(near(slope(c, 0, 0, 1.55), measured, 0.01), c);
        // (At 300 K the 300 K index itself; dndT the derivative.)
        QCOMPARE(compoundProperty(QStringLiteral("GaAs"), 0, 0, QStringLiteral("n"), BandSet::Vurgaftman, 300), compoundProperty(QStringLiteral("GaAs"), 0, 0, QStringLiteral("n")));
        const double dndT = compoundValue(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("dndT"), um(1.55));
        const double fd = (compoundValue(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("n"), um(1.55), 300.5)
                           - compoundValue(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("n"), um(1.55), 299.5));
        QVERIFY2(near(dndT, fd, 1e-6), qPrintable(QStringLiteral("%1 %2").arg(dndT).arg(fd)));
    }

    // An alloy's ends its binaries' own: AlGaAs at x 0 and 1 GaAs and AlAs
    // (they were 0.006 and 0.03 apart), GaInP at x 0 InP (0.005), InGaAs at
    // 0 and 1; between, the alloy's oscillator bowed as half its gap -
    // In0.53Ga0.47As within 0.01 of Le et al.'s measured at 16.7 µm (it
    // was 0.027 under), the lattice-matched InGaAsP at y 1 within 0.015 of
    // it at 2 µm (it was 0.03 over). Silicon germanium silicon's and
    // germanium's mixed: its ends theirs.
    void anAlloysEnds()
    {
        auto um = [](double l) { return l * 1e-6; };
        for (double l : {1.31, 1.55, 3.0, 8.0}) {
            auto lib = [&](const char* name) { return valueOf(QLatin1String(name), QStringLiteral("n"), 300, um(l)); };
            QVERIFY(near(compoundValue(QStringLiteral("AlGaAs"), 0, 0, QStringLiteral("n"), um(l)), lib("Gallium arsenide"), 1e-9));
            if (l <= 2.2) QVERIFY(near(compoundValue(QStringLiteral("AlGaAs"), 1, 0, QStringLiteral("n"), um(l)), lib("Aluminium arsenide"), 1e-9));
            QVERIFY(near(compoundValue(QStringLiteral("GaInP"), 0, 0, QStringLiteral("n"), um(l)), lib("Indium phosphide"), 1e-9));
            QVERIFY(near(compoundValue(QStringLiteral("InGaAs"), 1, 0, QStringLiteral("n"), um(l)), lib("Indium arsenide"), 1e-9));
        }
        // In0.52Ga0.48As near its gap against Adachi's 1989 (refractiveindex.info's
        // tables: 3.5276 at 1.5277 µm, 3.4688 at 1.9566 µm): on InP's lattice
        // his quaternary at y 1 - the oscillator, bowed, was 0.040 over at
        // 1550 nm: the update check of 8bd5709 -; the quaternary at y 1 and
        // the ternary the same there.
        for (const auto& [l, adachi] : {std::pair{1.5277, 3.5276}, {1.9566, 3.4688}}) {
            const double n = compoundValue(QStringLiteral("InGaAs"), 0.52, 0, QStringLiteral("n"), um(l));
            const double q = compoundValue(QStringLiteral("InGaAsP"), 0.468, 1, QStringLiteral("n"), um(l)), t = compoundValue(QStringLiteral("InGaAs"), 0.532, 0, QStringLiteral("n"), um(l));
            qInfo("In0.52Ga0.48As at %g um: %.4f (Adachi's %.4f); In0.532 %.4f, the quaternary at y 1 %.4f", l, n, adachi, t, q);
            QVERIFY2(std::abs(n - adachi) < 0.02, qPrintable(QString::number(n)));
            QVERIFY2(std::abs(q - t) < 0.002, qPrintable(QStringLiteral("%1 %2").arg(q).arg(t)));
        }
        const double le = compoundValue(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("n"), um(16.7));
        QVERIFY2(std::abs(le - 3.2983) < 0.01, qPrintable(QString::number(le)));
        const double q = compoundValue(QStringLiteral("InGaAsP"), 0.47, 1, QStringLiteral("n"), um(2)), t = compoundValue(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("n"), um(2));
        QVERIFY2(std::abs(q - t) < 0.015, qPrintable(QStringLiteral("%1 %2").arg(q).arg(t)));
        for (double l : {2.0, 5.0}) {
            QVERIFY(near(compoundValue(QStringLiteral("SiGe"), 0, 0, QStringLiteral("n"), um(l)), valueOf(QStringLiteral("Silicon"), QStringLiteral("n"), 300, um(l)), 1e-9));
            QVERIFY(near(compoundValue(QStringLiteral("SiGe"), 1, 0, QStringLiteral("n"), um(l)), valueOf(QStringLiteral("Germanium"), QStringLiteral("n"), 300, um(l)), 1e-9));
        }
        QVERIFY(compoundProperty(QStringLiteral("SiGe"), 0.2, 0, QStringLiteral("kappa")).isEmpty());
    }

    // The films' κ their measured (Rodriguez de Marcos et al.'s ALD Al2O3,
    // Bright et al.'s sputtered HfO2, within 3 %); an index whose formula
    // fails past its range (a Sellmeier's pole: silica beyond 8.3 µm,
    // lithium niobate below 0.22 µm - NaN in a solve) held at its ends,
    // the same within it.
    void filmsAndRanges()
    {
        auto um = [](double l) { return l * 1e-6; };
        QVERIFY(near(valueOf(QStringLiteral("Aluminium oxide (ALD)"), QStringLiteral("kappa"), 300, um(1.55)), 2.58e-4, 0.03));
        QVERIFY(near(valueOf(QStringLiteral("Aluminium oxide (ALD)"), QStringLiteral("kappa"), 300, um(4)), 1.75e-3, 0.03));
        QVERIFY(near(valueOf(QStringLiteral("Hafnium oxide (sputtered)"), QStringLiteral("kappa"), 300, um(1.55)), 6.97e-5, 0.03));
        int held = 0;
        for (const MaterialEntry& e : builtinMaterials()) {
            const QString n = e.properties.value(QStringLiteral("n")).toString();
            if (!n.contains(QLatin1String("lambda0"))) continue;
            const QStringList range = e.properties.value(QStringLiteral("nrange")).toString().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (range.size() != 2) continue;
            const double a = evaluateConstant(range[0], Scope::builtins(), Dim::length()).value_or(NAN);
            const double b = evaluateConstant(range[1], Scope::builtins(), Dim::length()).value_or(NAN);
            for (double l : {0.21 * a, 0.5 * a, 0.9 * a, 1.1 * b, 1.3 * b, 1.45 * b}) {
                const double v = valueOf(e.name, QStringLiteral("n"), 300, l);
                QVERIFY2(std::isfinite(v) && v > 0, qPrintable(QStringLiteral("%1 at %2 um: %3").arg(e.name).arg(l * 1e6).arg(v)));
            }
            if (n.contains(QLatin1String("min(max(lambda0"))) ++held;
        }
        qInfo("%d entries held at their ranges' ends", held);
        QVERIFY(held >= 10);
        // (Silica's held: its value at 6.7 µm at 9 µm, where its Sellmeier's
        // n² < 0; GaAs's not - past its phonon it is the Reststrahlen's.)
        QVERIFY(near(valueOf(QStringLiteral("Silicon dioxide"), QStringLiteral("n"), 300, um(9)), valueOf(QStringLiteral("Silicon dioxide"), QStringLiteral("n"), 300, um(6.7)), 1e-12));
        QVERIFY(near(valueOf(QStringLiteral("Silicon dioxide"), QStringLiteral("n"), 300, um(1.55)), 1.44402, 1e-5));
    }

    // The semiconductors' n and κ of the wavelength against the measured
    // spectra their fits were made to: a III-V's Afromowitz oscillator with
    // its lattice's term below the gap (Skauli's GaAs, Lorimor's InAs, Fern
    // and Onton's AlAs, Bond's GaP, Adachi's AlSb and InSb), the gap's
    // absorption above it (Aspnes and Studna's GaAs and InP), an alloy's of
    // its binaries at its composition and gap (InGaAs against Adachi's,
    // GaInP against Ferrini et al.'s); germanium Nunley's and Burnett's; the
    // II-VIs', GaN's, water's, the films' and LiTaO3's formulas.
    void semiconductorOptics()
    {
        auto um = [](double l) { return l * 1e-6; };
        auto value = [&](const char* name, const char* key, double l) { return valueOf(QLatin1String(name), QLatin1String(key), 300, um(l)); };
        // GaAs: its oscillator (3.6795, 36.290 eV; Afromowitz's own 3.65,
        // 36.1) and lattice (2.01 at 33.3 meV) - Skauli's within 0.002 from
        // 0.97 to 17 µm.
        auto skauli = [](double l) {
            return std::sqrt(1 + 4.372514 + 5.466742 * l * l / (l * l - 0.4431307 * 0.4431307) + 0.0242996 * l * l / (l * l - 0.8746453 * 0.8746453)
                             + 1.957522 * l * l / (l * l - 36.9166 * 36.9166));
        };
        for (double l : {0.97, 1.064, 1.31, 1.55, 2.0, 5.0, 10.0, 17.0}) {
            const double n = value("Gallium arsenide", "n", l);
            QVERIFY2(near(n, oscillatorAt(l, 3.6795, 36.290, 1.424, {{2.01, 0.0333}}), 1e-9), qPrintable(QString::number(l)));
            QVERIFY2(std::abs(n - skauli(l)) < 0.002, qPrintable(QStringLiteral("%1: %2 %3").arg(l).arg(n).arg(skauli(l))));
        }
        // Its absorption: α = A √(E - Eg)/E (A 6.578e4 /cm, Eg 1.424 eV) -
        // Aspnes and Studna's κ 0.091 at 1.6 eV, Jellison's 0.097, within
        // 25 %; nought below the gap.
        auto edge = [](double l, double A, double Eg) {
            const double E = 1.23984198 / l;
            return E > Eg ? A * 100 * l * 1e-6 / (4 * M_PI) * std::sqrt(E - Eg) / E : 0.0;
        };
        const double at16 = 1.23984198 / 1.6;
        QVERIFY(near(value("Gallium arsenide", "kappa", at16), edge(at16, 6.578e4, 1.424), 1e-9));
        QVERIFY2(std::abs(value("Gallium arsenide", "kappa", at16) - 0.094) < 0.25 * 0.094, qPrintable(QString::number(value("Gallium arsenide", "kappa", at16))));
        QVERIFY(value("Gallium arsenide", "kappa", 0.88) == 0.0);
        // InP: A its Afk, 1.16e5 /cm - Aspnes and Studna's 0.203 at 1.5 eV
        // within 5 %.
        QVERIFY(near(value("Indium phosphide", "kappa", 1.23984198 / 1.5), 0.203, 0.05));
        QVERIFY(value("Indium phosphide", "kappa", 1.55) == 0.0);
        // InAs: Lorimor's at 5 and 10 µm within 0.01; Adachi's κ at 1550 nm
        // (0.110) within 15 % (its split-off band's absorption, from 0.74
        // eV, not counted: 0.097). AlSb: Adachi's 3.253 at 1550 nm within 0.01,
        // Zollner's 3.524 at 0.85 µm within 0.05; transparent there (its Γ
        // gap 2.3 eV: Adachi's κ 0.038 is its broadening's tail). GaP:
        // Bond's within 0.01 at 1 µm, 0.008 from 1.6 to 3 µm. AlAs: Fern and Onton's within
        // 0.003. InSb: Adachi's at 10 µm within 0.005.
        QVERIFY(std::abs(value("Indium arsenide", "n", 5) - 3.4679) < 0.01 && std::abs(value("Indium arsenide", "n", 10) - 3.4236) < 0.01);
        QVERIFY2(near(value("Indium arsenide", "kappa", 1.55), 0.1104, 0.15), qPrintable(QString::number(value("Indium arsenide", "kappa", 1.55))));
        QVERIFY(std::abs(value("Aluminium antimonide", "n", 1.55) - 3.2534) < 0.01 && std::abs(value("Aluminium antimonide", "n", 0.85) - 3.5241) < 0.05);
        QVERIFY(value("Aluminium antimonide", "kappa", 1.55) == 0.0);
        for (const auto& [lambda, bond] : {std::pair{1.0, 3.1192}, {1.6, 3.0509}, {3.0, 3.0215}})
            QVERIFY2(std::abs(value("Gallium phosphide", "n", lambda) - bond) < (lambda < 1.5 ? 0.01 : 0.008), qPrintable(QString::number(lambda)));
        for (double lambda : {0.6, 1.0, 1.55, 2.2}) QVERIFY(std::abs(value("Aluminium arsenide", "n", lambda) - sellmeierAt(lambda, 2.0792, {{6.084, 0.2822}, {1.9, 27.62}})) < 0.003);
        QVERIFY(std::abs(value("Indium antimonide", "n", 10) - 3.9514) < 0.005);
        // InGaAs (x 0.53): below its gap Adachi's within 0.06 (his 0.03-0.07
        // above the measured binaries in the infrared); at 1550 nm absorbing
        // - κ 0.0925, α 7500 /cm (measured 6800-7000; Adachi's 0.076) -,
        // its A its binaries' over µ^1.5 Ep of its own µ and Ep; transparent
        // beyond its gap.
        QVERIFY(std::abs(value("Indium gallium arsenide", "n", 2.0) - 3.4655) < 0.06);
        const double kInGaAs = value("Indium gallium arsenide", "kappa", 1.55);
        QVERIFY2(kInGaAs > 0.08 && kInGaAs < 0.1, qPrintable(QString::number(kInGaAs)));
        QVERIFY(value("Indium gallium arsenide", "kappa", 1.7) == 0.0);
        {
            // (Its A from the binaries' A over their µ^1.5 Ep - InAs 9381, GaAs
            // 6.578e4 /cm -, at its own.)
            auto scale = [](const BandParameters& p) {
                const double mh = 1 / (p.gamma1 - 2 * p.gamma2), mr = p.me * mh / (p.me + mh);
                return std::pow(mr, 1.5) * p.Ep;
            };
            const BandParameters inas = *bandParameters(QStringLiteral("InAs"), 0, 0, 300), gaas = *bandParameters(QStringLiteral("GaAs"), 0, 0, 300);
            const BandParameters alloy = *bandParameters(QStringLiteral("InGaAs"), 0.53, 0, 300, nullptr, BandSet::Measured);
            const double A = (0.53 * 9381 / scale(inas) + 0.47 * 6.578e4 / scale(gaas)) * scale(alloy);
            QVERIFY2(near(kInGaAs, edge(1.55, A, alloy.Eg), 1e-9), qPrintable(QStringLiteral("%1 %2").arg(kInGaAs).arg(edge(1.55, A, alloy.Eg))));
            QVERIFY(near(alloy.Eg, 0.750, 1e-3));
        }
        // An alloy's index follows x: InGaAs at 1.8 µm from x 0.3 to 0.53.
        QVERIFY(compoundValue(QStringLiteral("InGaAs"), 0.53, 0, QStringLiteral("n"), um(1.8)) > compoundValue(QStringLiteral("InGaAs"), 0.3, 0, QStringLiteral("n"), um(1.8)) + 0.03);
        // GaInP (x 0.51): between Ferrini et al.'s film and Kaneko's at 0.95
        // and 1.31 µm (they 0.03 apart; Schubert's ordered sample between),
        // within 0.025 of Ferrini's at 1.55 µm (its oscillator bowed as half
        // its gap: 0.02 up).
        for (const auto& [l, ferrini, kaneko] : {std::tuple{0.95, 3.2138, 3.2452}, {1.31, 3.1283, 3.1553}}) {
            const double n = value("Gallium indium phosphide", "n", l);
            QVERIFY2(n > ferrini - 0.005 && n < kaneko + 0.005, qPrintable(QStringLiteral("%1: %2").arg(l).arg(n)));
        }
        QVERIFY(std::abs(value("Gallium indium phosphide", "n", 1.55) - 3.1053) < 0.025);
        // InGaAsP on InP's lattice: Adachi's (1982) A 8.4 - 3.4 y, B 6.6 + 3.4 y
        // at its gap - his 1989 dispersion (y 0.24) within 0.005 from 1.55 to 5
        // µm, 0.012 at 1.3 µm; at y 0 InP's (Pettit and Turner's 3.1649 at 1550 nm) within
        // 0.003; the 1.3 µm quaternary 3.386 at 1550 nm.
        // (Of the energy broadened by 50 meV: f's real part at (E + iΓ)/E0.)
        auto adachi = [](double l, double y, double E0, double Eso) {
            const double E = std::min(1.23984198 / l, E0 + 0.5);
            auto f = [E](double gap) {
                const std::complex<double> c = std::complex<double>(E, 0.05) / gap;
                return ((2.0 - std::sqrt(1.0 + c) - std::sqrt(1.0 - c)) / (c * c)).real();
            };
            return std::sqrt((8.4 - 3.4 * y) * (f(E0) + 0.5 * std::pow(E0 / Eso, 1.5) * f(Eso)) + 6.6 + 3.4 * y);
        };
        {
            const BandParameters q = *bandParameters(QStringLiteral("InGaAsP"), 0.28, 0.6, 300, nullptr, BandSet::Measured);
            QVERIFY(near(value("Indium gallium arsenide phosphide", "n", 1.55), adachi(1.55, 0.6, q.Eg, q.Eg + q.Delta), 1e-9));
            // (3.386: its edge broadened, 0.003 under the held 3.389.)
            QVERIFY2(std::abs(value("Indium gallium arsenide phosphide", "n", 1.55) - 3.386) < 0.002, qPrintable(QString::number(value("Indium gallium arsenide phosphide", "n", 1.55))));
            for (const auto& [lambda, adachi89] : {std::pair{1.3, 3.2906}, {1.55, 3.2515}, {2.0, 3.2206}, {5.0, 3.1866}}) {
                const double n = compoundValue(QStringLiteral("InGaAsP"), 0.1095, 0.24, QStringLiteral("n"), um(lambda));
                QVERIFY2(std::abs(n - adachi89) < (lambda < 1.5 ? 0.012 : 0.005), qPrintable(QStringLiteral("%1: %2").arg(lambda).arg(n)));
            }
            QVERIFY(std::abs(compoundValue(QStringLiteral("InGaAsP"), 0, 0, QStringLiteral("n"), um(1.55)) - 3.1649) < 0.003);
        }
        // The 1.3 µm quaternary on InP's lattice: transparent at 1550 nm.
        QVERIFY(value("Indium gallium arsenide phosphide", "kappa", 1.55) == 0.0);
        QVERIFY(value("Indium gallium arsenide phosphide", "kappa", 1.25) > 0.01);
        // Germanium: Nunley et al.'s n and κ at 1.31 and 1.55 µm, Burnett's at
        // 5 µm; on silicon its edge's κ (α 4100 /cm at 1550 nm) over bulk's.
        QVERIFY(std::abs(value("Germanium", "n", 1.31) - 4.2776) < 0.003 && std::abs(value("Germanium", "n", 1.55) - 4.25) < 0.003);
        QVERIFY(near(value("Germanium", "kappa", 1.31), 0.0861, 0.04) && near(value("Germanium", "kappa", 1.55), 0.0252, 0.04));
        QVERIFY(std::abs(value("Germanium", "n", 5) - 4.01578) < 1e-4);
        QVERIFY(value("Germanium", "kappa", 2.5) == 0.0);
        QVERIFY(near(value("Germanium on silicon", "kappa", 1.55), edge(1.55, 2e4, 0.773), 1e-9));
        QVERIFY(near(edge(1.55, 2e4, 0.773) * 4 * M_PI / 1.55e-4, 4100, 0.02));
        QVERIFY(near(value("Germanium on silicon", "kappa", 0.8), value("Germanium", "kappa", 0.8), 1e-9));
        // Its index bulk germanium's 27 meV lower: at 1550 nm germanium's at
        // 1.4994 µm (its peak at the strained gap).
        QVERIFY(near(value("Germanium on silicon", "n", 1.55), value("Germanium", "n", 1.55 / (1 + 0.027 * 1.55 / 1.23984198)), 1e-9));
        // The II-VIs and GaN: their sources' formulas, held beyond their
        // ranges (no pole reached).
        QVERIFY(near(value("Cadmium telluride", "n", 1.55), sellmeierAt(1.55, 5.68, {{1.53, std::sqrt(0.366)}}), 1e-9));
        QVERIFY(near(value("Cadmium selenide", "n", 1.55), sellmeierAt(1.55, 4.2243, {{1.768, std::sqrt(0.227)}, {3.12, std::sqrt(3380.0)}}), 1e-9));
        QVERIFY(near(value("Cadmium sulfide", "n", 1.0), std::sqrt(5.1792 + 0.23504 / (1 - 0.083591) + 0.036927 / (1 - 0.23504)), 1e-9));
        QVERIFY(near(value("Gallium nitride", "n", 1.55), sellmeierAt(1.55, 3.60, {{1.75, 0.256}, {4.1, 17.86}}), 1e-9));
        QVERIFY(near(value("Cadmium telluride", "n", 0.5), value("Cadmium telluride", "n", 0.86), 1e-12));
        QVERIFY(near(value("Cadmium sulfide", "n", 0.45), value("Cadmium sulfide", "n", 0.52), 1e-12));
        // Water: Daimon and Masumura's at 0.6 µm, Hale and Querry's n and κ
        // at 1.55 and 3 µm.
        QVERIFY(near(value("Water", "n", 0.6), 1.33302, 1e-5));
        QVERIFY(std::abs(value("Water", "n", 1.55) - 1.318) < 0.002 && near(value("Water", "kappa", 1.55), 9.86e-5, 0.03));
        QVERIFY(std::abs(value("Water", "n", 3.0) - 1.371) < 0.002 && near(value("Water", "kappa", 3.0), 0.272, 0.03));
        for (double lambda = 0.95; lambda < 1.2; lambda += 0.01) {   // (the two weighed between 1 and 1.13 µm: no step)
            const double step = value("Water", "n", lambda + 0.005) - value("Water", "n", lambda);
            QVERIFY2(step < 0 && step > -3e-4, qPrintable(QStringLiteral("%1: %2").arg(lambda).arg(step)));
        }
        // Silica's κ in the infrared (Franta et al.'s 0.0048 at 5 µm), nought
        // in its window; Fused silica's the same.
        QVERIFY(near(value("Silicon dioxide", "kappa", 5), 0.00476, 0.03) && value("Silicon dioxide", "kappa", 1.55) == 0);
        QVERIFY(near(value("Fused silica", "kappa", 5), value("Silicon dioxide", "kappa", 5), 1e-12));
        // The films: ALD alumina (Rodriguez de Marcos et al.'s 1.6460), sputtered
        // hafnia (Bright et al.'s 1.9564), PECVD nitride (Beliaev et al.'s
        // 1.9904), e-beam hafnia Al-Kuhaili's Cauchy.
        QVERIFY(std::abs(value("Aluminium oxide (ALD)", "n", 1.55) - 1.6460) < 0.001);
        QVERIFY(std::abs(value("Hafnium oxide (sputtered)", "n", 1.55) - 1.9564) < 0.004);
        QVERIFY(std::abs(value("Silicon nitride (PECVD)", "n", 1.55) - 1.9904) < 0.007);
        QVERIFY(near(value("Hafnium oxide", "n", 1.55), 1.875 + 6.28e-3 / (1.55 * 1.55) + 5.80e-4 / std::pow(1.55, 4), 1e-9));
        // LiTaO3: Sellmeiers to Bond's points within 0.0007; smooth - its group
        // index n - λ dn/dλ bending smoothly, 10 nm to 10 nm (the table's
        // slope jumped at each point: its group index by 0.005 there).
        {
            const Node lt = materialNode(Model::blank(), *findMaterial(QStringLiteral("Lithium tantalate")));
            MaterialSweep sweep;
            sweep.variable = QStringLiteral("lambda");
            sweep.from = 1.0e-6;
            sweep.to = 2.0e-6;
            sweep.points = 101;
            const MaterialCurves c = materialCurves(lt, {QStringLiteral("n")}, sweep, Scope::builtins());
            QCOMPARE(int(c.curves.size()), 3);   // (its axes)
            const std::vector<double>& no = c.curves[0].values;
            std::vector<double> group;
            for (std::size_t k = 1; k + 1 < no.size(); ++k) group.push_back(no[k] - c.at[k] * (no[k + 1] - no[k - 1]) / (c.at[k + 1] - c.at[k - 1]));
            double worst = 0;
            for (std::size_t k = 1; k + 1 < group.size(); ++k) worst = std::max(worst, std::abs(group[k + 1] - 2 * group[k] + group[k - 1]));
            QVERIFY2(worst < 2e-4, qPrintable(QString::number(worst)));
            for (const auto& [lambda, o, e] : {std::tuple{0.6, 2.1834, 2.1878}, {1.6, 2.1174, 2.1213}, {3.0, 2.0755, 2.0799}}) {
                QVERIFY(std::abs(sellmeierAt(lambda, 2.23997, {{2.27035, 0.194489}, {18.708, 28.2899}}) - o) < 0.0007);
                QVERIFY(std::abs(sellmeierAt(lambda, 2.18189, {{2.34332, 0.192952}, {7.17944, 17.8713}}) - e) < 0.0007);
            }
        }
    }

    // Every library entry's index finite and positive over its range, its κ
    // nought or more; taken outside the range, the material's curve says so.
    void everyIndexHoldsInItsRange()
    {
        int checked = 0;
        for (const MaterialEntry& e : builtinMaterials()) {
            const Node m = materialNode(Model::blank(), e);
            const QString n = detail::Problem::materialProperty(m, QStringLiteral("n"));
            if (n.trimmed().isEmpty()) continue;
            const QStringList range = detail::Problem::materialProperty(m, QStringLiteral("nrange")).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (range.size() != 2) continue;
            const double a = evaluateConstant(range[0], Scope::builtins(), Dim::length()).value_or(NAN);
            const double b = evaluateConstant(range[1], Scope::builtins(), Dim::length()).value_or(NAN);
            QVERIFY2(a > 0 && b > a, qPrintable(e.name));
            MaterialSweep sweep;
            sweep.variable = QStringLiteral("lambda");
            sweep.from = a;
            sweep.to = b;
            sweep.log = true;
            sweep.points = 600;
            const MaterialCurves c = materialCurves(m, {QStringLiteral("n"), QStringLiteral("kappa")}, sweep, Scope::builtins());
            QVERIFY2(c.error.isEmpty(), qPrintable(e.name + QStringLiteral(": ") + c.error));
            for (const MaterialCurve& curve : c.curves)
                for (std::size_t k = 0; k < curve.values.size(); ++k) {
                    const double v = curve.values[k];
                    if (curve.key == QLatin1String("kappa") && std::isnan(v)) continue;   // (none)
                    QVERIFY2(std::isfinite(v) && (curve.key == QLatin1String("n") ? v > 0 : v >= 0),
                             qPrintable(QStringLiteral("%1: %2 %3 at %4 um").arg(e.name, curve.key).arg(v).arg(c.at[k])));
                }
            QVERIFY2(c.notes.join(QLatin1Char(' ')).indexOf(QStringLiteral("nrange")) < 0, qPrintable(e.name + c.notes.join(QLatin1Char(' '))));
            ++checked;
        }
        QVERIFY2(checked > 40, qPrintable(QString::number(checked)));
        // Outside it: said - swept past it, or held there.
        const Node inas = materialNode(Model::blank(), *findMaterial(QStringLiteral("Indium arsenide")));
        MaterialSweep sweep;
        sweep.variable = QStringLiteral("lambda");
        sweep.from = 1.2e-6;
        sweep.to = 3e-6;
        sweep.points = 3;
        MaterialCurves c = materialCurves(inas, {QStringLiteral("n")}, sweep, Scope::builtins());
        QVERIFY2(c.notes.join(QLatin1Char(' ')).contains(QStringLiteral("nrange")) && c.notes.join(QLatin1Char(' ')).contains(QStringLiteral("1.2 to 2.238")),
                 qPrintable(c.notes.join(QLatin1Char(' '))));
        sweep.variable = QStringLiteral("T");
        sweep.from = 250;
        sweep.to = 350;
        sweep.lambda = 1.55e-6;
        c = materialCurves(inas, {QStringLiteral("kappa")}, sweep, Scope::builtins());
        QVERIFY2(c.notes.join(QLatin1Char(' ')).contains(QStringLiteral("1.55 µm")), qPrintable(c.notes.join(QLatin1Char(' '))));
        c = materialCurves(inas, {QStringLiteral("Eg")}, sweep, Scope::builtins());
        QVERIFY(!c.notes.join(QLatin1Char(' ')).contains(QStringLiteral("nrange")));   // (not the index)
    }

    // A property's curve (the material visualizer's): against T, N, x, λ,
    // F - the physics' formulas at each point.
    void curves()
    {
        const Node si = materialNode(Model::blank(), *findMaterial(QStringLiteral("Silicon")));
        MaterialSweep sweep;
        sweep.variable = QStringLiteral("T");
        sweep.from = 200;
        sweep.to = 500;
        sweep.points = 4;
        MaterialCurves c = materialCurves(si, {QStringLiteral("Eg"), QStringLiteral("ni")}, sweep, Scope::builtins());
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        QCOMPARE(int(c.at.size()), 4);
        QCOMPARE(c.unit, QStringLiteral("K"));
        for (int i = 0; i < 4; ++i) {
            const double T = c.at[std::size_t(i)], eg = 1.12 - 4.73e-4 * (T * T / (T + 636) - 9e4 / 936), t = T / 300;
            QVERIFY(near(c.curves[0].values[std::size_t(i)], eg, 1e-12));
            QVERIFY(near(c.curves[1].values[std::size_t(i)], std::sqrt(2.8e19 * 1.04e19) * std::pow(t, 1.5) * std::exp(-eg * Q / (2 * KB * T)), 1e-9));
        }
        // GaAs's electrons by doping: Caughey and Thomas's of its own.
        const Node gaas = materialNode(Model::blank(), *findMaterial(QStringLiteral("Gallium arsenide")));
        sweep.variable = QStringLiteral("N");
        sweep.from = 1e20;
        sweep.to = 1e26;
        sweep.log = true;
        sweep.points = 7;
        c = materialCurves(gaas, {QStringLiteral("mun")}, sweep, Scope::builtins());
        QVERIFY2(c.error.isEmpty() && c.curves.size() == 1, qPrintable(c.error));
        for (int i = 0; i < 7; ++i) {
            const double N = c.at[std::size_t(i)];   // (1/cm³)
            QVERIFY(near(c.curves[0].values[std::size_t(i)], 40 + (8500 - 40) / (1 + N / 8.1e16), 1e-9));
        }
        QVERIFY(c.notes.join(QLatin1Char(' ')).contains(QLatin1String("Caughey and Thomas's")));
        // Masetti's of a material that gives it: its own µmax where it gives
        // one (the library's Silicon's 1417 cm²/(V s)), else the lattice's -
        // as the solver's (the update check of 21 October).
        {
            Node masetti;
            masetti.type = QStringLiteral("material");
            masetti.props = {{"label", "M"},
                             {"mun", "1400[cm^2/(V*s)]"},
                             {"m1n", "52.2[cm^2/(V*s)]"},
                             {"m2n", "52.2[cm^2/(V*s)]"},
                             {"mu1n", "43.4[cm^2/(V*s)]"},
                             {"pcn", "0[1/cm^3]"},
                             {"crn", "9.68e16[1/cm^3]"},
                             {"csn", "3.43e20[1/cm^3]"},
                             {"man", "0.68"},
                             {"mbn", "2"}};
            sweep.points = 7;
            for (double mumax : {1400.0, 1417.0}) {
                if (mumax != 1400) masetti.props.insert(QStringLiteral("mmaxn"), QStringLiteral("1417[cm^2/(V*s)]"));
                c = materialCurves(masetti, {QStringLiteral("mun")}, sweep, Scope::builtins());
                QVERIFY2(c.error.isEmpty() && c.curves.size() == 1, qPrintable(c.error));
                QVERIFY(c.notes.join(QLatin1Char(' ')).contains(QLatin1String("Masetti's")));
                for (int i = 0; i < 7; ++i) {
                    const double N = c.at[std::size_t(i)];   // (1/cm³)
                    QVERIFY(near(c.curves[0].values[std::size_t(i)], 52.2 + (mumax - 52.2) / (1 + std::pow(N / 9.68e16, 0.68)) - 43.4 / (1 + std::pow(3.43e20 / N, 2)), 1e-9));
                }
            }
        }
        // An alloy against x; a field's ionization.
        const Node algaas = materialNode(Model::blank(), *findMaterial(QStringLiteral("Aluminium gallium arsenide")));
        sweep = MaterialSweep();
        sweep.variable = QStringLiteral("x");
        sweep.from = 0;
        sweep.to = 0.4;
        sweep.points = 5;
        c = materialCurves(algaas, {QStringLiteral("Eg")}, sweep, Scope::builtins());
        QVERIFY2(c.error.isEmpty(), qPrintable(c.error));
        for (int i = 1; i < 5; ++i) QVERIFY(c.curves[0].values[std::size_t(i)] > c.curves[0].values[std::size_t(i - 1)]);
        QVERIFY(near(c.curves[0].values[2], bandParameters(QStringLiteral("AlGaAs"), 0.2, 0, 300)->Eg, 1e-6));
        sweep = MaterialSweep();
        sweep.variable = QStringLiteral("F");
        sweep.from = 3e7;
        sweep.to = 6e7;
        sweep.points = 2;
        c = materialCurves(gaas, {QStringLiteral("alphan")}, sweep, Scope::builtins());
        QVERIFY(near(c.curves[0].values[1], 2.99e5 * std::exp(-std::pow(6.85e5 / 6e5, 1.6)), 1e-9));
        // A crystal's index along each of its axes: lithium niobate's no, no,
        // ne (Zelmon et al.'s) - three curves, named apart in a dataset.
        const Node ln = materialNode(Model::blank(), *findMaterial(QStringLiteral("Lithium niobate")));
        sweep = MaterialSweep();
        sweep.variable = QStringLiteral("lambda");
        sweep.from = 1.5e-6;
        sweep.to = 1.55e-6;
        sweep.points = 2;
        c = materialCurves(ln, {QStringLiteral("n"), QStringLiteral("rho")}, sweep, Scope::builtins());
        QVERIFY2(c.error.isEmpty() && c.curves.size() == 4, qPrintable(c.error));
        const double l2 = 1.55 * 1.55;
        const double no = std::sqrt(1 + 2.6734 * l2 / (l2 - 0.01764) + 1.2290 * l2 / (l2 - 0.05914) + 12.614 * l2 / (l2 - 474.60));
        const double ne = std::sqrt(1 + 2.9804 * l2 / (l2 - 0.02047) + 0.5981 * l2 / (l2 - 0.0666) + 8.9543 * l2 / (l2 - 416.08));
        QVERIFY(near(c.curves[0].values[1], no, 1e-12) && near(c.curves[1].values[1], no, 1e-12) && near(c.curves[2].values[1], ne, 1e-12));
        QCOMPARE(c.curves[2].name(), QStringLiteral("n_x3"));
        QVERIFY(c.curves[2].label.contains(QLatin1String("x3")));
        QCOMPARE(c.curves[3].name(), QStringLiteral("rho"));
        // What it is not.
        sweep.variable = QStringLiteral("x");
        QVERIFY(!materialCurves(si, {}, sweep, Scope::builtins()).error.isEmpty());
        QVERIFY(!materialCurves(si, {QStringLiteral("nonsense")}, MaterialSweep(), Scope::builtins()).error.isEmpty());
        sweep.variable = QStringLiteral("q");
        QVERIFY(!materialCurves(si, {}, sweep, Scope::builtins()).error.isEmpty());
    }

    // A Lumerical project's materials as a library's entries: each
    // semiconductor's gap (Varshni's of T), χ from its work function, its
    // masses' Nc; a conductor's work function; the optical ones' index of
    // the wavelength; its library written and read back.
    void lumericalIntoTheLibrary()
    {
        QString why;
        QStringList notes;
        const QList<MaterialEntry> device = lumericalMaterials(QStringLiteral(QUCS_TEST_DATA "/lumerical_device.ldev"), &why, &notes);
        QVERIFY2(why.isEmpty() && !device.isEmpty(), qPrintable(why));
        auto entry = [](const QList<MaterialEntry>& list, const QString& name) -> std::optional<MaterialEntry> {
            for (const MaterialEntry& e : list)
                if (e.name == name) return e;
            return std::nullopt;
        };
        const std::optional<MaterialEntry> si = entry(device, QStringLiteral("Si (Silicon)"));
        QVERIFY(si);
        auto at = [](const MaterialEntry& e, const char* key, double T) {
            MaterialScope s(Scope::builtins(), T);
            const Expression x = Expression::compile(e.properties.value(QLatin1String(key)).toString(), s);
            return x.isValid() && x.isConstant() ? (x.dim() == Dim::of(2, 1, -2) ? x.constant() / Q : x.constant()) : NAN;
        };
        // (Its record: Varshni's 1.169 eV, 4.73e-4 eV/K, 636 K.)
        for (double T : {250.0, 300.0, 400.0}) QVERIFY(near(at(*si, "Eg", T), 1.169 - 4.73e-4 * T * T / (T + 636), 1e-12));
        QVERIFY(si->properties.value(QStringLiteral("Nc")).toString().contains(QLatin1String("(T/300[K])^1.5")));
        QVERIFY(si->category.startsWith(QLatin1String("Lumerical")) && si->user);
        const std::optional<MaterialEntry> al = entry(device, QStringLiteral("Al (Aluminium) - CRC"));
        QVERIFY(al && al->properties.value(QStringLiteral("phi")).toString() == QLatin1String("4.28[eV]"));
        // An older project's two databases (DEVICE 7.1's): the oxide's
        // thermal record, under its electrical one's id, on the same entry.
        const QList<MaterialEntry> charge = lumericalMaterials(QStringLiteral(QUCS_TEST_DATA "/lumerical_charge.ldev"), &why);
        const std::optional<MaterialEntry> oxideCharge = entry(charge, QStringLiteral("SiO2 (Glass) - Sze"));
        QVERIFY2(oxideCharge && at(*oxideCharge, "epsilonr", 300) == 3.9, qPrintable(why));
        QVERIFY(near(at(*oxideCharge, "k", 400), 1.4 * std::pow(4.0 / 3, 0.1), 1e-12) && at(*oxideCharge, "Cp", 300) == 730);
        QCOMPARE(int(std::count_if(charge.begin(), charge.end(), [](const MaterialEntry& e) { return e.name == QLatin1String("SiO2 (Glass) - Sze"); })), 1);
        // The optical ones (MODE's): sampled data, Sellmeier's.
        const QList<MaterialEntry> mode = lumericalMaterials(QStringLiteral(QUCS_TEST_DATA "/lumerical_mode.lms"), &why);
        const std::optional<MaterialEntry> oxide = entry(mode, QStringLiteral("SiO2 (Glass) - Palik"));
        QVERIFY2(oxide && oxide->properties.value(QStringLiteral("n")).toString().startsWith(QLatin1String("interp(lambda0")), qPrintable(why));
        const std::optional<MaterialEntry> ln = entry(mode, QStringLiteral("LN_Sellmeier_MgO_Zel"));
        QVERIFY(ln && ln->properties.value(QStringLiteral("n")).toString().startsWith(QLatin1String("sqrt(")));
        // Into a library file and back.
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("materials.json"));
        QVERIFY(writeMaterials(file, device + mode, &why));
        const QList<MaterialEntry> back = readMaterials(file, &why);
        QCOMPARE(back.size(), device.size() + mode.size());
        QCOMPARE(entry(back, QStringLiteral("Si (Silicon)"))->properties, si->properties);
        // What it cannot read.
        QVERIFY(lumericalMaterials(QStringLiteral(QUCS_TEST_DATA "/klayout_sample.oas"), &why).isEmpty() && !why.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestFemMaterials)
#include "test_fem_materials.moc"
