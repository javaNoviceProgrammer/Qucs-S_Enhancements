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
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>

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
                                  QStringLiteral("taun"),     QStringLiteral("taup"), QStringLiteral("compound"),
                                  QStringLiteral("x"),        QStringLiteral("y"),    QStringLiteral("me"),
                                  QStringLiteral("gamma1"),   QStringLiteral("gamma2"), QStringLiteral("gamma3"),
                                  QStringLiteral("Ep"),       QStringLiteral("VBO"),  QStringLiteral("Delta"),
                                  QStringLiteral("a0"),       QStringLiteral("ac"),   QStringLiteral("av"),
                                  QStringLiteral("b"),        QStringLiteral("C11"),  QStringLiteral("C12"),
                                  QStringLiteral("Egd"),      QStringLiteral("mr"),   QStringLiteral("Afk"),
                                  QStringLiteral("bands"),    QStringLiteral("lambdag")};

constexpr double KB = 1.380649e-23, Hbar = 1.054571817e-34, M0 = 9.1093837015e-31;

/// A III-V binary: its band parameters at 0 K (Vurgaftman et al. 2001;
/// a0 at 300 K), Varshni's α (eV/K) and β (K) of its Γ gap, and, roughly,
/// what other physics want of it.
struct Binary {
    const char* name;
    BandParameters p;   // Eg: at 0 K
    double alpha, beta;
    double k, rho, Cp, mun, mup;   // W/(m K), kg/m³, J/(kg K), cm²/(V s)
};

const std::vector<Binary>& binaries()
{
    // Eg VBO Delta me γ1 γ2 γ3 Ep a0 ac av b C11 C12 εr n (eV, m0, m, Pa).
    auto bp = [](double eg, double vbo, double delta, double me, double g1, double g2, double g3, double ep, double a0, double ac,
                 double av, double b, double c11, double c12, double epsr, double n) {
        BandParameters p;
        p.Eg = eg;
        p.VBO = vbo;
        p.Delta = delta;
        p.me = me;
        p.gamma1 = g1;
        p.gamma2 = g2;
        p.gamma3 = g3;
        p.Ep = ep;
        p.a0 = a0 * 1e-10;
        p.ac = ac;
        p.av = av;
        p.b = b;
        p.C11 = c11 * 1e9;
        p.C12 = c12 * 1e9;
        p.epsr = epsr;
        p.n = n;
        return p;
    };
    static const std::vector<Binary> list = {
        {"GaAs", bp(1.519, -0.80, 0.341, 0.067, 6.98, 2.06, 2.93, 28.8, 5.65325, -7.17, 1.16, -2.0, 1221, 566, 12.9, 3.37), 0.5405e-3, 204,
         55, 5320, 330, 8500, 400},
        {"AlAs", bp(3.099, -1.33, 0.28, 0.15, 3.76, 0.82, 1.42, 21.1, 5.6611, -5.64, 2.47, -2.3, 1250, 534, 10.06, 2.90), 0.885e-3, 530,
         91, 3760, 450, 200, 100},
        {"InAs", bp(0.417, -0.59, 0.39, 0.026, 20.0, 8.5, 9.2, 21.5, 6.0583, -5.08, 1.00, -1.8, 832.9, 452.6, 15.15, 3.42), 0.276e-3, 93,
         27, 5680, 250, 40000, 500},
        {"InP", bp(1.4236, -0.94, 0.108, 0.0795, 5.08, 1.60, 2.10, 20.7, 5.8697, -6.0, 0.6, -2.0, 1011, 561, 12.4, 3.17), 0.363e-3, 162,
         68, 4810, 310, 5400, 200},
        {"GaP", bp(2.886, -1.27, 0.08, 0.13, 4.05, 0.49, 2.93, 31.4, 5.4505, -8.2, 1.7, -1.6, 1405, 620.3, 11.1, 3.05), 0.1081e-3, 164,
         110, 4138, 430, 250, 150},
        {"GaSb", bp(0.812, -0.03, 0.76, 0.039, 13.4, 4.7, 6.0, 27.0, 6.0959, -7.5, 0.8, -2.0, 884.2, 402.6, 15.7, 3.80), 0.417e-3, 140,
         32, 5614, 250, 3000, 1000},
        {"AlSb", bp(2.386, -0.41, 0.676, 0.14, 5.18, 1.19, 1.97, 18.7, 6.1355, -4.5, 1.4, -1.35, 876.9, 434.1, 12.04, 3.20), 0.42e-3, 140,
         57, 4260, 450, 200, 400},
        {"InSb", bp(0.235, 0.0, 0.81, 0.0135, 34.8, 15.5, 16.5, 23.3, 6.4794, -6.94, 0.36, -2.0, 684.7, 373.5, 16.8, 4.00), 0.32e-3, 170,
         18, 5775, 200, 77000, 850},
    };
    return list;
}

const Binary* binary(const QString& name)
{
    for (const Binary& b : binaries())
        if (name.compare(QLatin1String(b.name), Qt::CaseInsensitive) == 0) return &b;
    return nullptr;
}

/// A binary's parameters at T (K): its Γ gap Varshni's - in the 300 K fit
/// moved to the gap measured there (InP's 9 meV down), the slope kept.
BandParameters binaryAt(const Binary& b, double T, BandSet set = BandSet::Vurgaftman)
{
    BandParameters p = b.p;
    auto varshni = [&](double t) { return b.p.Eg - b.alpha * t * t / (t + b.beta); };
    p.Eg = varshni(T);
    if (set == BandSet::Measured) {
        static const std::pair<const char*, double> at300[] = {{"InP", 1.344}, {"GaAs", 1.424}, {"InAs", 0.354}};
        for (const auto& [name, eg] : at300)
            if (qstrcmp(b.name, name) == 0) p.Eg += eg - varshni(300);
    }
    return p;
}

/// A ternary A_x B_(1-x) C: its binaries, what x is of, and the bowing of
/// Eg (Cg0 + Cg1 x: AlGaAs's depends on x), VBO, Delta, me, Ep and ac.
struct Ternary {
    const char* name;
    const char* a;
    const char* b;
    double cg0, cg1, cVBO, cDelta, cMe, cEp, cAc;
};

const std::vector<Ternary>& ternaries()
{
    static const std::vector<Ternary> list = {
        {"AlGaAs", "AlAs", "GaAs", -0.127, 1.310, 0, 0, 0, 0, 0},
        {"InGaAs", "InAs", "GaAs", 0.477, 0, -0.38, 0.15, 0.0091, -1.48, 2.61},
        {"InAlAs", "InAs", "AlAs", 0.70, 0, -0.64, 0.15, 0.049, -4.81, -1.4},
        {"GaInP", "GaP", "InP", 0.65, 0, 0, 0, 0.01051, 0, 0},
        {"GaAsP", "GaAs", "GaP", 0.19, 0, 0, 0, 0, 0, 0},
        {"InAsP", "InAs", "InP", 0.10, 0, 0, 0.16, 0, 0, 0},
        {"GaAsSb", "GaAs", "GaSb", 1.43, 0, -1.06, 0.6, 0, 0, 0},
    };
    return list;
}

const Ternary* ternary(const QString& name)
{
    for (const Ternary& t : ternaries())
        if (name.compare(QLatin1String(t.name), Qt::CaseInsensitive) == 0) return &t;
    return nullptr;
}

/// Each member of two sets of parameters weighed: u of a, v of b.
BandParameters blend(const BandParameters& a, double u, const BandParameters& b, double v)
{
    BandParameters p;
    for (auto m : {&BandParameters::Eg, &BandParameters::VBO, &BandParameters::Delta, &BandParameters::me, &BandParameters::gamma1,
                   &BandParameters::gamma2, &BandParameters::gamma3, &BandParameters::Ep, &BandParameters::a0, &BandParameters::ac,
                   &BandParameters::av, &BandParameters::b, &BandParameters::C11, &BandParameters::C12, &BandParameters::epsr,
                   &BandParameters::n})
        p.*m = u * (a.*m) + v * (b.*m);
    return p;
}

BandParameters ternaryAt(const Ternary& t, double x, double T, BandSet set = BandSet::Vurgaftman)
{
    const BandParameters a = binaryAt(*binary(QLatin1String(t.a)), T, set), b = binaryAt(*binary(QLatin1String(t.b)), T, set);
    BandParameters p = blend(a, x, b, 1 - x);
    const double w = x * (1 - x);
    // (The 300 K fit's bowings: InGaAs's Goetz et al.'s, 0.750 eV on InP's
    // lattice; InAlAs's to 1.46 eV there, of the 1.45-1.47 measured.)
    double cg0 = t.cg0;
    if (set == BandSet::Measured) {
        if (qstrcmp(t.name, "InGaAs") == 0) cg0 = 0.43;
        else if (qstrcmp(t.name, "InAlAs") == 0) cg0 = 0.663;
    }
    p.Eg -= w * (cg0 + t.cg1 * x);
    p.VBO -= w * t.cVBO;
    p.Delta -= w * t.cDelta;
    p.me -= w * t.cMe;
    p.Ep -= w * t.cEp;
    p.ac -= w * t.cAc;
    return p;
}

/// A wurtzite binary: its parameters (Eg at 0 K), Varshni's α (eV/K) and β
/// (K) of its gap. (Vurgaftman and Meyer's, J. Appl. Phys. 94, 3675 (2003) -
/// Lumerical's stock GaN, AlN and InN carry the same k·p, strain and lattice
/// values -; the valence bands' tops AlN 0.8 eV below GaN's (theirs), InN
/// 1.05 above it (Martin et al.'s: measured ones spread 0.6-1.1); Psp
/// Bernardini and Fiorentini's, e31 and e33 Bernardini, Fiorentini and
/// Vanderbilt's; εr along c; n near the gap.)
struct Nitride {
    const char* name;
    WurtziteParameters p;
    double alpha, beta;
};

const std::vector<Nitride>& nitrides()
{
    // Eg Ev Δcr Δso me∥ me⊥ A1-6 D1-6 a1 a2 Ep a c (Å) C11 C12 C13 C33 (GPa) Psp e31 e33 εr n.
    auto wp = [](double eg, double ev, double dcr, double dso, double mez, double mep, std::array<double, 6> A, std::array<double, 6> D,
                 double a1, double a2, double ep, double a, double c, double c11, double c12, double c13, double c33, double psp, double e31,
                 double e33, double epsr, double n) {
        WurtziteParameters p;
        p.Eg = eg;
        p.Ev = ev;
        p.dcr = dcr;
        p.dso = dso;
        p.mez = mez;
        p.mep = mep;
        for (int i = 0; i < 6; ++i) {
            p.A[i] = A[std::size_t(i)];
            p.D[i] = D[std::size_t(i)];
        }
        p.a1 = a1;
        p.a2 = a2;
        p.Ep = ep;
        p.a = a * 1e-10;
        p.c = c * 1e-10;
        p.C11 = c11 * 1e9;
        p.C12 = c12 * 1e9;
        p.C13 = c13 * 1e9;
        p.C33 = c33 * 1e9;
        p.Psp = psp;
        p.e31 = e31;
        p.e33 = e33;
        p.epsr = epsr;
        p.n = n;
        return p;
    };
    static const std::vector<Nitride> list = {
        {"GaN",
         wp(3.510, -2.64, 0.010, 0.017, 0.20, 0.20, {-7.21, -0.44, 6.68, -3.46, -3.40, -4.90}, {-3.7, 4.5, 8.2, -4.1, -4.0, -5.5}, -4.9, -11.3,
            14.0, 3.189, 5.185, 390, 145, 106, 398, -0.034, -0.49, 0.73, 10.4, 2.5),
         0.909e-3, 830},
        {"AlN",
         wp(6.25, -3.44, -0.169, 0.019, 0.32, 0.30, {-3.86, -0.25, 3.58, -1.32, -1.47, -1.64}, {-17.1, 7.9, 8.8, -3.9, -3.4, -3.4}, -3.4,
            -11.8, 14.5, 3.112, 4.982, 396, 137, 108, 373, -0.090, -0.60, 1.46, 9.1, 2.15),
         1.799e-3, 1462},
        {"InN",
         wp(0.78, -1.59, 0.040, 0.005, 0.07, 0.07, {-8.21, -0.68, 7.57, -5.23, -5.11, -5.96}, {-3.7, 4.5, 8.2, -4.1, -4.0, -5.5}, -3.5, -3.5,
            14.6, 3.545, 5.703, 223, 115, 92, 224, -0.042, -0.57, 0.97, 15.3, 2.9),
         0.245e-3, 624},
    };
    return list;
}

/// A nitride alloy A_x B_(1-x) N: its binaries and its gap's and
/// spontaneous polarization's bowings (eV, C/m²; Psp's added: Psp = x Pa +
/// (1 - x) Pb + b x (1 - x)).
struct NitrideAlloy {
    const char* name;
    const char* a;
    const char* b;
    double bowEg, bowPsp;
};

const std::vector<NitrideAlloy>& nitrideAlloys()
{
    static const std::vector<NitrideAlloy> list = {
        {"AlGaN", "AlN", "GaN", 0.8, 0.019},
        {"InGaN", "InN", "GaN", 1.4, 0.038},
        {"InAlN", "InN", "AlN", 2.5, 0.071},
    };
    return list;
}

const Nitride* nitride(const QString& name)
{
    for (const Nitride& n : nitrides())
        if (name.compare(QLatin1String(n.name), Qt::CaseInsensitive) == 0) return &n;
    return nullptr;
}

WurtziteParameters nitrideAt(const Nitride& n, double T)
{
    WurtziteParameters p = n.p;
    p.Eg = n.p.Eg - n.alpha * T * T / (T + n.beta);
    return p;
}

/// The binaries an alloy is of and their shares (for what has no bowing:
/// thermal and transport values).
std::vector<std::pair<const Binary*, double>> shares(const QString& compound, double x, double y)
{
    std::vector<std::pair<const Binary*, double>> out;
    auto add = [&](const char* name, double w) {
        if (w > 0) out.emplace_back(binary(QLatin1String(name)), w);
    };
    if (const Binary* b = binary(compound)) out.emplace_back(b, 1.0);
    else if (const Ternary* t = ternary(compound)) {
        add(t->a, x);
        add(t->b, 1 - x);
    } else if (compound.compare(QLatin1String("InGaAsP"), Qt::CaseInsensitive) == 0) {
        add("GaAs", x * y);
        add("GaP", x * (1 - y));
        add("InAs", (1 - x) * y);
        add("InP", (1 - x) * (1 - y));
    } else if (compound.compare(QLatin1String("AlGaInAs"), Qt::CaseInsensitive) == 0) {
        add("AlAs", x);
        add("GaAs", y);
        add("InAs", 1 - x - y);
    }
    return out;
}

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

/// A III-V's entry: its compound (the band parameters, an alloy's at its
/// composition) and, a binary's, what the other physics read of it.
MaterialEntry compoundEntry(const char* name, const char* description, const char* compound, const char* x = nullptr,
                            const char* y = nullptr)
{
    MaterialEntry e;
    e.name = QString::fromUtf8(name);
    e.category = QStringLiteral("III-V compounds");
    e.description = QString::fromUtf8(description);
    e.properties.insert(QStringLiteral("compound"), QString::fromLatin1(compound));
    if (x) e.properties.insert(QStringLiteral("x"), QString::fromLatin1(x));
    if (y) e.properties.insert(QStringLiteral("y"), QString::fromLatin1(y));
    if (!x)
        for (const char* key : {"epsilonr", "n", "k", "rho", "Cp", "mur", "Eg", "chi", "Nc", "Nv", "mun", "mup", "taun", "taup"}) {
            const QString v = qstrcmp(key, "mur") == 0 ? QStringLiteral("1") : compoundProperty(QString::fromLatin1(compound), 0, 0, QString::fromLatin1(key));
            if (!v.isEmpty()) e.properties.insert(QString::fromLatin1(key), v);
        }
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
        entry("Germanium", "Semiconductors",
              "Crystalline germanium; n at 1550 nm. Its direct gap's absorption (a Wave Equation's Direct-gap absorption): Egd 0.80 eV, "
              "A 2e4 /cm (8000 /cm at 1310 nm), µ 0.03",
              {{"epsilonr", "16.2"}, {"k", "60[W/(m*K)]"}, {"rho", "5323[kg/m^3]"}, {"Cp", "320[J/(kg*K)]"}, {"mur", "1"}, {"E", "103[GPa]"},
               {"nu", "0.26"}, {"alpha", "5.9e-6[1/K]"}, {"n", "4.275"}, {"dndT", "4.2e-4[1/K]"}, {"Eg", "0.66[eV]"}, {"chi", "4.0[eV]"},
               {"Nc", "1.04e19[1/cm^3]"}, {"Nv", "6.0e18[1/cm^3]"}, {"mun", "3900[cm^2/(V*s)]"}, {"mup", "1900[cm^2/(V*s)]"},
               {"taun", "1[ms]"}, {"taup", "1[ms]"}, {"Egd", "0.80[eV]"}, {"mr", "0.03"}, {"Afk", "2e4[1/cm]"}}),
        entry("Germanium on silicon", "Semiconductors",
              "Germanium grown on silicon: 0.25 % tensile strain as it cools, its gaps narrowed - the direct one to 0.773 eV, "
              "absorbing 4000 /cm at 1550 nm (a waveguide photodiode's). Its lifetimes the threading dislocations'",
              {{"epsilonr", "16.2"}, {"k", "60[W/(m*K)]"}, {"rho", "5323[kg/m^3]"}, {"Cp", "320[J/(kg*K)]"}, {"mur", "1"}, {"E", "103[GPa]"},
               {"nu", "0.26"}, {"alpha", "5.9e-6[1/K]"}, {"n", "4.275"}, {"dndT", "4.2e-4[1/K]"}, {"Eg", "0.65[eV]"}, {"chi", "4.0[eV]"},
               {"Nc", "1.04e19[1/cm^3]"}, {"Nv", "6.0e18[1/cm^3]"}, {"mun", "3900[cm^2/(V*s)]"}, {"mup", "1900[cm^2/(V*s)]"},
               {"taun", "1[ns]"}, {"taup", "1[ns]"}, {"Egd", "0.773[eV]"}, {"mr", "0.03"}, {"Afk", "2e4[1/cm]"}}),
        entry("Gallium nitride", "Semiconductors",
              "Wurtzite GaN: its bands for quantum wells (k·p 6x6, its polarization) its compound's; n at 1550 nm",
              {{"compound", "GaN"}, {"epsilonr", "8.9"}, {"k", "130[W/(m*K)]"}, {"rho", "6150[kg/m^3]"}, {"Cp", "490[J/(kg*K)]"}, {"mur", "1"}, {"E", "295[GPa]"},
               {"nu", "0.23"}, {"alpha", "5.6e-6[1/K]"}, {"n", "2.31"}, {"Eg", "3.4[eV]"}, {"chi", "4.1[eV]"}, {"Nc", "2.3e18[1/cm^3]"},
               {"Nv", "4.6e19[1/cm^3]"}, {"mun", "1000[cm^2/(V*s)]"}, {"mup", "30[cm^2/(V*s)]"}, {"taun", "1[ns]"}, {"taup", "1[ns]"}}),
        entry("Silicon dioxide", "Semiconductors", "SiO₂, thermal oxide or fused silica; n at 1550 nm",
              {{"epsilonr", "3.9"}, {"tand", "0.0001"}, {"sigma", "1e-15[S/m]"}, {"k", "1.4[W/(m*K)]"}, {"rho", "2200[kg/m^3]"}, {"Cp", "730[J/(kg*K)]"},
               {"mur", "1"}, {"E", "70[GPa]"}, {"nu", "0.17"}, {"alpha", "0.5e-6[1/K]"}, {"n", "1.444"}, {"dndT", "9.5e-6[1/K]"}}),
        entry("Silicon nitride", "Semiconductors", "Si₃N₄, stoichiometric (a thin film conducts heat less); n at 1550 nm",
              {{"epsilonr", "7.5"}, {"sigma", "1e-14[S/m]"}, {"k", "20[W/(m*K)]"}, {"rho", "3100[kg/m^3]"}, {"Cp", "700[J/(kg*K)]"},
               {"mur", "1"}, {"E", "250[GPa]"}, {"nu", "0.23"}, {"alpha", "2.3e-6[1/K]"}, {"n", "1.996"}, {"dndT", "2.45e-5[1/K]"}}),
        entry("Indium phosphide", "Semiconductors", "InP; n at 1550 nm", {{"compound", "InP"}, {"epsilonr", "12.4"}, {"k", "68[W/(m*K)]"},
              {"rho", "4810[kg/m^3]"}, {"Cp", "310[J/(kg*K)]"}, {"mur", "1"}, {"E", "61[GPa]"}, {"nu", "0.36"}, {"alpha", "4.6e-6[1/K]"}, {"n", "3.17"}, {"dndT", "2.0e-4[1/K]"},
              {"Eg", "1.344[eV]"}, {"chi", "4.38[eV]"}, {"Nc", "5.7e17[1/cm^3]"}, {"Nv", "1.1e19[1/cm^3]"}, {"mun", "5400[cm^2/(V*s)]"},
              {"mup", "200[cm^2/(V*s)]"}, {"taun", "1[ns]"}, {"taup", "1[ns]"}}),
        entry("Gallium arsenide", "Semiconductors", "GaAs; n at 1550 nm", {{"compound", "GaAs"}, {"epsilonr", "12.9"}, {"k", "55[W/(m*K)]"},
              {"rho", "5320[kg/m^3]"}, {"Cp", "330[J/(kg*K)]"}, {"mur", "1"}, {"E", "85.9[GPa]"}, {"nu", "0.31"}, {"alpha", "5.73e-6[1/K]"}, {"n", "3.37"}, {"dndT", "2.35e-4[1/K]"},
              {"Eg", "1.424[eV]"}, {"chi", "4.07[eV]"}, {"Nc", "4.7e17[1/cm^3]"}, {"Nv", "9.0e18[1/cm^3]"}, {"mun", "8500[cm^2/(V*s)]"},
              {"mup", "400[cm^2/(V*s)]"}, {"taun", "5[ns]"}, {"taup", "5[ns]"}}),
        // III-V binaries and alloys: their band parameters for quantum wells
        // (an alloy's at its composition x, y), the rest rough.
        compoundEntry("Aluminium arsenide", "AlAs: indirect, its Γ gap 3.0 eV at 300 K", "AlAs"),
        compoundEntry("Indium arsenide", "InAs: a narrow gap, 0.35 eV", "InAs"),
        compoundEntry("Gallium phosphide", "GaP: indirect, its Γ gap 2.78 eV", "GaP"),
        compoundEntry("Gallium antimonide", "GaSb", "GaSb"),
        compoundEntry("Aluminium antimonide", "AlSb: indirect, its Γ gap 2.3 eV", "AlSb"),
        compoundEntry("Indium antimonide", "InSb: the narrowest gap, 0.17 eV", "InSb"),
        compoundEntry("Aluminium gallium arsenide", "AlₓGa₁₋ₓAs, x the aluminium's share (barriers to GaAs; direct below x 0.45)", "AlGaAs", "0.3"),
        compoundEntry("Indium gallium arsenide", "InₓGa₁₋ₓAs, x the indium's share: 0.53 is InP's lattice, 0.2 a strained well on GaAs",
                      "InGaAs", "0.53"),
        compoundEntry("Indium aluminium arsenide", "InₓAl₁₋ₓAs, x the indium's share: 0.52 is InP's lattice", "InAlAs", "0.52"),
        compoundEntry("Gallium indium phosphide", "GaₓIn₁₋ₓP, x the gallium's share: 0.51 is GaAs's lattice", "GaInP", "0.51"),
        compoundEntry("Indium gallium arsenide phosphide", "In₁₋ₓGaₓAsᵧP₁₋ᵧ: on InP's lattice y ≈ 2.2 x; x 0.28, y 0.6 is the 1.3 µm quaternary",
                      "InGaAsP", "0.28", "0.6"),
        compoundEntry("Aluminium gallium indium arsenide", "AlₓGaᵧIn₁₋ₓ₋ᵧAs: x + y 0.47 is InP's lattice", "AlGaInAs", "0.2", "0.27"),
        // The nitrides (wurtzite): their bands, strain and polarization for
        // quantum wells.
        compoundEntry("Aluminium nitride", "AlN: wurtzite, the widest gap (6.2 eV at 300 K); its crystal field below 0 - its top band Z-like, "
                      "its light TM", "AlN"),
        compoundEntry("Indium nitride", "InN: wurtzite, a narrow gap (0.76 eV)", "InN"),
        compoundEntry("Aluminium gallium nitride", "AlₓGa₁₋ₓN, x the aluminium's share (barriers to GaN; a bowing of 0.8 eV)", "AlGaN", "0.2"),
        compoundEntry("Indium gallium nitride", "InₓGa₁₋ₓN, x the indium's share (blue and green wells on GaN; a bowing of 1.4 eV)", "InGaN",
                      "0.15"),
        compoundEntry("Indium aluminium nitride", "InₓAl₁₋ₓN, x the indium's share: 0.18 is GaN's lattice", "InAlN", "0.18"),
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

QStringList compounds()
{
    QStringList out;
    for (const Binary& b : binaries()) out << QString::fromLatin1(b.name);
    for (const Ternary& t : ternaries()) out << QString::fromLatin1(t.name);
    out << QStringLiteral("InGaAsP") << QStringLiteral("AlGaInAs");
    for (const Nitride& n : nitrides()) out << QString::fromLatin1(n.name);
    for (const NitrideAlloy& a : nitrideAlloys()) out << QString::fromLatin1(a.name);
    return out;
}

bool isWurtzite(const QString& compound)
{
    const QString name = compound.trimmed();
    if (nitride(name)) return true;
    for (const NitrideAlloy& a : nitrideAlloys())
        if (name.compare(QLatin1String(a.name), Qt::CaseInsensitive) == 0) return true;
    return false;
}

std::optional<WurtziteParameters> wurtziteParameters(const QString& compound, double x, double T, QString* error)
{
    auto fail = [&](const QString& why) -> std::optional<WurtziteParameters> {
        if (error) *error = why;
        return std::nullopt;
    };
    const QString name = compound.trimmed();
    if (!(T > 0)) return fail(tr("a temperature of %1 K").arg(T));
    if (const Nitride* n = nitride(name)) return nitrideAt(*n, T);
    for (const NitrideAlloy& al : nitrideAlloys()) {
        if (name.compare(QLatin1String(al.name), Qt::CaseInsensitive) != 0) continue;
        if (!(x >= -1e-12 && x <= 1 + 1e-12)) return fail(tr("%1: x = %2 is not between 0 and 1").arg(name).arg(x));
        x = std::clamp(x, 0.0, 1.0);
        const WurtziteParameters a = nitrideAt(*nitride(QLatin1String(al.a)), T), b = nitrideAt(*nitride(QLatin1String(al.b)), T);
        WurtziteParameters p;
        auto mix = [&](double WurtziteParameters::*m) { p.*m = x * (a.*m) + (1 - x) * (b.*m); };
        for (auto m : {&WurtziteParameters::Eg, &WurtziteParameters::Ev, &WurtziteParameters::dcr, &WurtziteParameters::dso,
                       &WurtziteParameters::mez, &WurtziteParameters::mep, &WurtziteParameters::a1, &WurtziteParameters::a2,
                       &WurtziteParameters::Ep, &WurtziteParameters::a, &WurtziteParameters::c, &WurtziteParameters::C11,
                       &WurtziteParameters::C12, &WurtziteParameters::C13, &WurtziteParameters::C33, &WurtziteParameters::Psp,
                       &WurtziteParameters::e31, &WurtziteParameters::e33, &WurtziteParameters::epsr, &WurtziteParameters::n})
            mix(m);
        for (int i = 0; i < 6; ++i) {
            p.A[i] = x * a.A[i] + (1 - x) * b.A[i];
            p.D[i] = x * a.D[i] + (1 - x) * b.D[i];
        }
        p.Eg -= al.bowEg * x * (1 - x);
        p.Psp += al.bowPsp * x * (1 - x);
        return p;
    }
    return fail(tr("%1 is not a nitride known (GaN, AlN, InN, AlGaN, InGaN, InAlN)").arg(name));
}

BandSet bandSetOf(const QString& text)
{
    return text.trimmed().compare(QLatin1String("measured"), Qt::CaseInsensitive) == 0 ? BandSet::Measured : BandSet::Vurgaftman;
}

std::optional<BandParameters> bandParameters(const QString& compound, double x, double y, double T, QString* error, BandSet set)
{
    auto fail = [&](const QString& why) -> std::optional<BandParameters> {
        if (error) *error = why;
        return std::nullopt;
    };
    const QString name = compound.trimmed();
    if (!(T > 0)) return fail(tr("a temperature of %1 K").arg(T));
    if (const Binary* b = binary(name)) return binaryAt(*b, T, set);
    if (isWurtzite(name)) {
        // A nitride's as zinc blende's would say it (other physics' gap,
        // affinity, permittivity, index; a substrate's lattice in the plane):
        // the holes' mass along c in γ1, γ2 0.
        const std::optional<WurtziteParameters> w = wurtziteParameters(name, x, T, error);
        if (!w) return std::nullopt;
        BandParameters p;
        p.Eg = w->Eg;
        p.VBO = w->Ev;
        p.Delta = w->dso;
        p.me = w->mez;
        p.gamma1 = -(w->A[0] + w->A[2]);
        p.Ep = w->Ep;
        p.a0 = w->a;
        p.ac = w->a2;
        p.C11 = w->C11;
        p.C12 = w->C12;
        p.epsr = w->epsr;
        p.n = w->n;
        return p;
    }
    auto inRange = [](double v) { return v >= -1e-12 && v <= 1 + 1e-12; };
    if (const Ternary* t = ternary(name)) {
        if (!inRange(x)) return fail(tr("%1: x = %2 is not between 0 and 1").arg(name).arg(x));
        return ternaryAt(*t, std::clamp(x, 0.0, 1.0), T, set);
    }
    if (name.compare(QLatin1String("InGaAsP"), Qt::CaseInsensitive) == 0) {
        // In1-x Gax Asy P1-y from its four ternaries (Vurgaftman's (6.4)).
        if (!inRange(x) || !inRange(y)) return fail(tr("InGaAsP: x = %1, y = %2 are not between 0 and 1").arg(x).arg(y));
        x = std::clamp(x, 0.0, 1.0);
        y = std::clamp(y, 0.0, 1.0);
        const double wx = x * (1 - x), wy = y * (1 - y);
        if (wx + wy < 1e-12) {
            const char* corner = x > 0.5 ? (y > 0.5 ? "GaAs" : "GaP") : (y > 0.5 ? "InAs" : "InP");
            return binaryAt(*binary(QLatin1String(corner)), T, set);
        }
        const BandParameters gaInAs = ternaryAt(*ternary(QStringLiteral("InGaAs")), 1 - x, T, set);   // (In's share 1 - x)
        const BandParameters gaInP = ternaryAt(*ternary(QStringLiteral("GaInP")), x, T, set);
        const BandParameters gaAsP = ternaryAt(*ternary(QStringLiteral("GaAsP")), y, T, set);
        const BandParameters inAsP = ternaryAt(*ternary(QStringLiteral("InAsP")), y, T, set);
        const BandParameters a = blend(gaInAs, y, gaInP, 1 - y), b = blend(gaAsP, x, inAsP, 1 - x);
        BandParameters p = blend(a, wx / (wx + wy), b, wy / (wx + wy));
        // (The ternaries alone leave its gap up to 70 meV high on InP's
        // lattice; a bowing of its own, 0 on its ternaries but InAsP's,
        // brings it within 15 meV of Nahory et al.'s 1.35 - 0.72 y + 0.12 y²
        // - the 300 K fit's, of two terms, within 7.)
        if (set == BandSet::Measured) p.Eg -= (1 - x) * y * (1 - y) * (0.131 + 0.499 * y);
        else p.Eg -= 0.36 * (1 - x) * y * (1 - y);
        return p;
    }
    if (name.compare(QLatin1String("AlGaInAs"), Qt::CaseInsensitive) == 0) {
        // Alx Gay In1-x-y As from its three ternaries (Vurgaftman's (6.5)).
        const double z = 1 - x - y;
        if (!inRange(x) || !inRange(y) || z < -1e-12)
            return fail(tr("AlGaInAs: x = %1, y = %2 are not shares that add up to 1 or less").arg(x).arg(y));
        x = std::clamp(x, 0.0, 1.0);
        y = std::clamp(y, 0.0, 1.0);
        const double zz = std::max(0.0, z);
        const double d = x * y + y * zz + x * zz;
        if (d < 1e-12) {
            const char* corner = x > 0.5 ? "AlAs" : y > 0.5 ? "GaAs" : "InAs";
            return binaryAt(*binary(QLatin1String(corner)), T, set);
        }
        const double u = (1 + x - y) / 2, v = (1 + y - zz) / 2, w = (1 + x - zz) / 2;
        const BandParameters alGa = ternaryAt(*ternary(QStringLiteral("AlGaAs")), u, T, set);
        const BandParameters gaIn = ternaryAt(*ternary(QStringLiteral("InGaAs")), 1 - v, T, set);   // (In's share 1 - v)
        const BandParameters alIn = ternaryAt(*ternary(QStringLiteral("InAlAs")), 1 - w, T, set);   // (In's share 1 - w)
        BandParameters p = blend(blend(alGa, x * y / d, gaIn, y * zz / d), 1, alIn, x * zz / d);
        // (The 300 K fit's own bowing: the 1.3 µm quaternary, Al0.165 Ga0.305
        // In0.53 As, at 0.954 eV - 0.993 from the ternaries -, and InP's
        // lattice within 15 meV of Olego et al.'s 0.76 + 0.49 z + 0.20 z².)
        if (set == BandSet::Measured) p.Eg -= 1.815 * x * y * zz;
        return p;
    }
    return fail(tr("%1 is not a compound known (%2)").arg(name, compounds().join(QLatin1String(", "))));
}

std::optional<std::vector<ValleySet>> valleySets(const QString& text, QString* error)
{
    auto fail = [&](const QString& why) -> std::optional<std::vector<ValleySet>> {
        if (error) *error = why;
        return std::nullopt;
    };
    const QString t = text.trimmed();
    std::vector<ValleySet> sets;
    if (t.isEmpty()) return sets;
    static const QRegularExpression preset(QStringLiteral(R"(^(si|ge)\s*\(\s*(100|110|111)\s*\)\s*(?:([^\s]+)\s+([^\s]+))?$)"),
                                           QRegularExpression::CaseInsensitiveOption);
    auto number = [](const QString& s, bool* ok) { return s.toDouble(ok); };
    if (const QRegularExpressionMatch m = preset.match(t); m.hasMatch()) {
        const bool si = m.captured(1).compare(QLatin1String("si"), Qt::CaseInsensitive) == 0;
        double ml = si ? 0.916 : 1.59, mt = si ? 0.190 : 0.0815;
        if (!m.captured(3).isEmpty()) {
            bool a = false, b = false;
            ml = number(m.captured(3), &a);
            mt = number(m.captured(4), &b);
            if (!a || !b || !(ml > 0) || !(mt > 0)) return fail(tr("%1: its masses ml and mt must be numbers more than 0").arg(t));
        }
        // The valleys' axes (Si's along <100>, two valleys each; Ge's along
        // <111>, half valleys at the zone's faces, two halves each) and the
        // plane's normal.
        std::vector<std::array<double, 3>> axes;
        if (si) axes = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        else axes = {{1, 1, 1}, {1, 1, -1}, {1, -1, 1}, {-1, 1, 1}};
        const double each = si ? 2 : 1;
        const QString plane = m.captured(2);
        const std::array<double, 3> n = plane == QLatin1String("100") ? std::array<double, 3>{1, 0, 0}
                                        : plane == QLatin1String("110") ? std::array<double, 3>{1, 1, 0}
                                                                        : std::array<double, 3>{1, 1, 1};
        const double nn = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (const auto& a : axes) {
            const double an = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            const double c = (a[0] * n[0] + a[1] * n[1] + a[2] * n[2]) / (an * nn), c2 = c * c;
            ValleySet v;
            v.g = each;
            v.mz = 1 / (c2 / ml + (1 - c2) / mt);
            v.md = std::sqrt(ml * mt * mt / v.mz);
            auto same = std::find_if(sets.begin(), sets.end(), [&](const ValleySet& s) { return std::abs(s.mz / v.mz - 1) < 1e-9; });
            if (same != sets.end()) same->g += v.g;
            else sets.push_back(v);
        }
        // (The heaviest along the axis first: the lowest ladder.)
        std::sort(sets.begin(), sets.end(), [](const ValleySet& a, const ValleySet& b) { return a.mz > b.mz; });
        return sets;
    }
    for (const QString& part : t.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList v = part.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (v.size() != 3 && v.size() != 4)
            return fail(tr("%1: Si(100), Si(110), Si(111), Ge(100), Ge(110) or Ge(111) (each with ml mt if wanted), or sets "
                           "\"g mz md [dE]\" split by ';'")
                            .arg(t));
        ValleySet s;
        bool ok[4] = {true, true, true, true};
        s.g = number(v[0], &ok[0]);
        s.mz = number(v[1], &ok[1]);
        s.md = number(v[2], &ok[2]);
        if (v.size() == 4) s.dE = number(v[3], &ok[3]);
        if (!ok[0] || !ok[1] || !ok[2] || !ok[3] || !(s.g > 0) || !(s.mz > 0) || !(s.md > 0) || !std::isfinite(s.dE))
            return fail(tr("%1: a set's g, mz and md must be numbers more than 0 (dE a number of eV)").arg(part.trimmed()));
        sets.push_back(s);
    }
    return sets;
}

QString compoundProperty(const QString& compound, double x, double y, const QString& key, BandSet set)
{
    const std::optional<BandParameters> p = bandParameters(compound, x, y, 300, nullptr, set);
    if (!p) return {};
    auto num = [](double v) { return formatNumber(v, 8); };
    auto eV = [&](double v) { return num(v) + QStringLiteral("[eV]"); };
    const QString k = key;
    if (k == QLatin1String("Eg")) return eV(p->Eg);
    if (k == QLatin1String("VBO")) return eV(p->VBO);
    if (k == QLatin1String("Delta")) return eV(p->Delta);
    if (k == QLatin1String("Ep")) return eV(p->Ep);
    if (k == QLatin1String("ac")) return eV(p->ac);
    if (k == QLatin1String("av")) return eV(p->av);
    if (k == QLatin1String("b")) return eV(p->b);
    if (k == QLatin1String("me")) return num(p->me);
    if (k == QLatin1String("gamma1")) return num(p->gamma1);
    if (k == QLatin1String("gamma2")) return num(p->gamma2);
    if (k == QLatin1String("gamma3")) return num(p->gamma3);
    if (k == QLatin1String("a0")) return num(p->a0 * 1e9) + QStringLiteral("[nm]");
    if (k == QLatin1String("C11")) return num(p->C11 / 1e9) + QStringLiteral("[GPa]");
    if (k == QLatin1String("C12")) return num(p->C12 / 1e9) + QStringLiteral("[GPa]");
    if (k == QLatin1String("epsilonr")) return num(p->epsr);
    if (k == QLatin1String("n")) return num(p->n);
    if (k == QLatin1String("chi")) {
        // Its conduction band against GaAs's (χ 4.07 eV): the offsets of
        // the common scale.
        const BandParameters gaas = binaryAt(*binary(QStringLiteral("GaAs")), 300, set);
        return eV(4.07 - ((p->VBO + p->Eg) - (gaas.VBO + gaas.Eg)));
    }
    if (k == QLatin1String("Nc") || k == QLatin1String("Nv")) {
        // 2 (m kT / 2πħ²)^(3/2); the holes' mass of the heavy and light ones'.
        double m = p->me;
        if (k == QLatin1String("Nv")) {
            const double mhh = 1 / (p->gamma1 - 2 * p->gamma2), mlh = 1 / (p->gamma1 + 2 * p->gamma2);
            m = std::pow(std::pow(mhh, 1.5) + std::pow(mlh, 1.5), 2.0 / 3.0);
        }
        return num(2 * std::pow(m * M0 * KB * 300 / (2 * M_PI * Hbar * Hbar), 1.5)) + QStringLiteral("[1/m^3]");
    }
    if (k == QLatin1String("taun") || k == QLatin1String("taup")) return QStringLiteral("1[ns]");
    const auto parts = shares(compound.trimmed(), x, y);
    if (parts.empty()) return {};
    double sum2 = 0;
    for (const auto& [b, w] : parts) sum2 += w * w;
    if (k == QLatin1String("mun") || k == QLatin1String("mup")) {
        // (Matthiessen's rule over the binaries: a rough mobility.)
        double inv = 0;
        for (const auto& [b, w] : parts) inv += w / (k == QLatin1String("mun") ? b->mun : b->mup);
        return num(1 / inv) + QStringLiteral("[cm^2/(V*s)]");
    }
    if (k == QLatin1String("k")) {
        // Thermal resistivities, with an alloy's disorder (0.3 m K/W of bowing).
        double w = 0.3 * (1 - sum2) / 2;
        for (const auto& [b, s] : parts) w += s / b->k;
        return num(1 / w) + QStringLiteral("[W/(m*K)]");
    }
    if (k == QLatin1String("rho") || k == QLatin1String("Cp")) {
        double v = 0;
        for (const auto& [b, s] : parts) v += s * (k == QLatin1String("rho") ? b->rho : b->Cp);
        return num(v) + (k == QLatin1String("rho") ? QStringLiteral("[kg/m^3]") : QStringLiteral("[J/(kg*K)]"));
    }
    return {};
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
