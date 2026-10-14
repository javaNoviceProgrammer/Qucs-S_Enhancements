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
 * Vial's gold, Rajkanan's silicon...
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
    // solves as it did.
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

    // Indices of the wavelength (lambda0), each its source's formula: Li's
    // and Salzberg and Villa's silicon, Rajkanan's absorption, Malitson's
    // oxide, Luke's nitride, Afromowitz's GaAs, Pettit and Turner's InP,
    // Vial's gold, a Drude silver; where each holds.
    void dispersion()
    {
        auto um = [](double l) { return l * 1e-6; };
        const double l = 1.55;
        QVERIFY(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("n"), 300, um(l)),
                     std::sqrt(11.6858 + 0.939816 / (l * l) + 0.00810461 * 1.1071 * 1.1071 / (l * l - 1.1071 * 1.1071)), 1e-6));
        QCOMPARE(valueOf(QStringLiteral("Silicon"), QStringLiteral("kappa"), 300, um(l)), 0.0);
        auto salzbergVilla = [](double l) {
            const double l2 = l * l;
            return std::sqrt(1 + 10.6684293 * l2 / (l2 - 0.301516485 * 0.301516485) + 0.0030434748 * l2 / (l2 - 1.13475115 * 1.13475115)
                             + 1.54133408 * l2 / (l2 - 1104.0 * 1104.0));
        };
        auto li = [](double l) { return std::sqrt(11.6858 + 0.939816 / (l * l) + 0.00810461 * 1.1071 * 1.1071 / (l * l - 1.1071 * 1.1071)); };
        QVERIFY(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("n"), 300, um(0.8)), salzbergVilla(0.8), 1e-6));
        // (From 1.2 to 1.4 µm the two weighed: at 1.3 µm half each.)
        QVERIFY(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("n"), 300, um(1.3)), (salzbergVilla(1.3) + li(1.3)) / 2, 1e-6));
        // Rajkanan, 300 K: its four indirect terms - at 600 nm the first gap's
        // phonons alone, at 450 nm the second gap's too (the direct gap above
        // 3.2 eV not reached) -, κ = α λ/4π.
        auto rajkanan = [](double l) {
            const double E = 1.23984198 / l, kT = 8.617333262e-5 * 300, shift = 7.021e-4 * 9e4 / 1408;
            const double gaps[2] = {1.1557 - shift, 2.5 - shift}, A[2] = {323.1, 7237}, Ep[2] = {0.01827, 0.05773}, C[2] = {5.5, 4.0};
            double alpha = 0;
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    const double up = E - gaps[i] + Ep[j], down = E - gaps[i] - Ep[j];
                    if (up > 0) alpha += C[j] * A[i] * up * up / std::expm1(Ep[j] / kT);
                    if (down > 0) alpha += C[j] * A[i] * down * down / -std::expm1(-Ep[j] / kT);
                }
            return alpha * 100 * l * 1e-6 / (4 * M_PI);
        };
        for (double l : {0.6, 0.45}) {
            const double kappa = rajkanan(l);
            QVERIFY2(near(valueOf(QStringLiteral("Silicon"), QStringLiteral("kappa"), 300, um(l)), kappa, 1e-6), qPrintable(QString::number(kappa)));
        }
        QVERIFY(rajkanan(0.6) > 0.02 && rajkanan(0.6) < 0.04);   // (Palik's 0.026)
        auto sell = [](double l, std::initializer_list<std::pair<double, double>> t) {
            double n2 = 1;
            for (const auto& [b, c] : t) n2 += b * l * l / (l * l - c * c);
            return std::sqrt(n2);
        };
        QVERIFY(near(valueOf(QStringLiteral("Silicon dioxide"), QStringLiteral("n"), 300, um(0.6328)),
                     sell(0.6328, {{0.6961663, 0.0684043}, {0.4079426, 0.1162414}, {0.8974794, 9.896161}}), 1e-9));
        QVERIFY(near(valueOf(QStringLiteral("Silicon nitride"), QStringLiteral("n"), 300, um(l)), sell(l, {{3.0249, 0.1353406}, {40314, 1239.842}}), 1e-9));
        QVERIFY(near(valueOf(QStringLiteral("Indium phosphide"), QStringLiteral("n"), 300, um(l)),
                     std::sqrt(7.255 + 2.316 * l * l / (l * l - 0.6263 * 0.6263) + 2.765 * l * l / (l * l - 32.935 * 32.935)), 1e-9));
        {
            // Afromowitz's GaAs (x 0) at 1.55 µm.
            const double E = 1.23984198 / l, E0 = 3.65, Ed = 36.1, Eg = 1.424, eta = M_PI * Ed / (2 * E0 * E0 * E0 * (E0 * E0 - Eg * Eg));
            const double e1 = 1 + Ed / E0 + Ed * E * E / (E0 * E0 * E0) + eta / M_PI * std::pow(E, 4) * std::log((2 * E0 * E0 - Eg * Eg - E * E) / (Eg * Eg - E * E));
            QVERIFY(near(valueOf(QStringLiteral("Gallium arsenide"), QStringLiteral("n"), 300, um(l)), std::sqrt(e1), 1e-9));
            QVERIFY(std::abs(std::sqrt(e1) - 3.3737) < 0.005);   // (Palik's 3.3737)
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
            const double w = 1.23984198 / l;
            const std::complex<double> eps = 3.7 - 9.1 * 9.1 / (w * std::complex<double>(w, 0.018)), nk = std::sqrt(std::complex<double>(eps.real(), std::abs(eps.imag())));
            QVERIFY(near(valueOf(QStringLiteral("Silver"), QStringLiteral("kappa"), 300, um(l)), nk.imag(), 1e-6));
        }
        QCOMPARE(findMaterial(QStringLiteral("Silicon"))->properties.value(QStringLiteral("nrange")).toString(), QStringLiteral("0.4[um] 14[um]"));
        QCOMPARE(findMaterial(QStringLiteral("Germanium"))->properties.value(QStringLiteral("nrange")).toString(), QStringLiteral("1.5[um] 1.6[um]"));
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
